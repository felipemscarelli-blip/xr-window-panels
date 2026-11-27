// Global input for the control app: keyboard (GetAsyncKeyState) and wheels / button boxes /
// joysticks (WinMM joyGetPosEx: up to 32 buttons + POV hat per device). Everything is polled
// from a UI timer, works while the game has focus, and never touches the game process.

using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;
using Microsoft.Win32;

namespace XRWindowPanels
{
    static class InputNative
    {
        [DllImport("user32.dll")] public static extern short GetAsyncKeyState(int vk);

        [StructLayout(LayoutKind.Sequential)]
        public struct JOYINFOEX
        {
            public int dwSize, dwFlags, dwXpos, dwYpos, dwZpos, dwRpos, dwUpos, dwVpos;
            public int dwButtons, dwButtonNumber, dwPOV, dwReserved1, dwReserved2;
        }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        public struct JOYCAPS
        {
            public ushort wMid, wPid;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string szPname;
            public int wXmin, wXmax, wYmin, wYmax, wZmin, wZmax, wNumButtons, wPeriodMin, wPeriodMax;
            public int wRmin, wRmax, wUmin, wUmax, wVmin, wVmax, wCaps, wMaxAxes, wNumAxes, wMaxButtons;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string szRegKey;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] public string szOEMVxD;
        }

        [DllImport("winmm.dll")] public static extern int joyGetNumDevs();
        [DllImport("winmm.dll")] public static extern int joyGetPosEx(int id, ref JOYINFOEX info);
        [DllImport("winmm.dll", CharSet = CharSet.Unicode)] public static extern int joyGetDevCapsW(IntPtr id, ref JOYCAPS caps, int size);

        public const int JOY_RETURNALL_POVCTS = 0xFF | 0x200;
        public static bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
    }

    // ------------------------------------------------------------------ Game controllers
    class JoyDevice
    {
        public int Id;
        public ushort Vid, Pid;
        public string Name;
        public int Buttons;        // bitmask of the 32 buttons
        public int Pov = 0xFFFF;   // hundredths of a degree, 0xFFFF = centered
    }

    static class Joysticks
    {
        static List<JoyDevice> _devices = new List<JoyDevice>();
        static int _nextScan = int.MinValue;

        public static List<JoyDevice> Devices { get { return _devices; } }

        static InputNative.JOYINFOEX Info()
        {
            var ji = new InputNative.JOYINFOEX();
            ji.dwSize = Marshal.SizeOf(typeof(InputNative.JOYINFOEX));
            ji.dwFlags = InputNative.JOY_RETURNALL_POVCTS;
            return ji;
        }

        public static void Poll()
        {
            int now = Environment.TickCount;
            if (_nextScan == int.MinValue || now - _nextScan >= 0)
            {
                Scan();  // hot-plug: rescan every 3 s
                _nextScan = now + 3000;
            }
            foreach (var d in _devices)
            {
                var ji = Info();
                if (InputNative.joyGetPosEx(d.Id, ref ji) == 0) { d.Buttons = ji.dwButtons; d.Pov = ji.dwPOV; }
                else { d.Buttons = 0; d.Pov = 0xFFFF; }
            }
        }

        static void Scan()
        {
            var list = new List<JoyDevice>();
            int n = Math.Min(InputNative.joyGetNumDevs(), 16);
            for (int i = 0; i < n; i++)
            {
                var ji = Info();
                if (InputNative.joyGetPosEx(i, ref ji) != 0) continue;
                var caps = new InputNative.JOYCAPS();
                if (InputNative.joyGetDevCapsW((IntPtr)i, ref caps, Marshal.SizeOf(typeof(InputNative.JOYCAPS))) != 0) continue;
                list.Add(new JoyDevice { Id = i, Vid = caps.wMid, Pid = caps.wPid, Name = OemName(caps.wMid, caps.wPid, caps.szPname) });
            }
            _devices = list;
        }

        public static JoyDevice Find(ushort vid, ushort pid)
        {
            foreach (var d in _devices) if (d.Vid == vid && d.Pid == pid) return d;
            return null;
        }

        public static string NameOf(ushort vid, ushort pid)
        {
            var d = Find(vid, pid);
            return d != null ? d.Name : OemName(vid, pid, string.Format("Controle {0:X4}:{1:X4}", vid, pid));
        }

        // Friendly product name registered by Windows for the device (falls back to WinMM name).
        static string OemName(ushort vid, ushort pid, string fallback)
        {
            try
            {
                string key = string.Format(@"System\CurrentControlSet\Control\MediaProperties\PrivateProperties\Joystick\OEM\VID_{0:X4}&PID_{1:X4}", vid, pid);
                foreach (var root in new[] { Registry.CurrentUser, Registry.LocalMachine })
                {
                    using (var k = root.OpenSubKey(key))
                    {
                        var name = k != null ? k.GetValue("OEMName") as string : null;
                        if (!string.IsNullOrEmpty(name)) return name.Trim();
                    }
                }
            }
            catch (Exception) { }
            return string.IsNullOrEmpty(fallback) ? string.Format("Controle {0:X4}:{1:X4}", vid, pid) : fallback.Trim();
        }

        // dir: 0 up, 1 right, 2 down, 3 left (diagonals count for both neighbours)
        public static bool PovDir(int pov, int dir)
        {
            if (pov < 0 || pov > 36000) return false;
            double deg = pov / 100.0;
            switch (dir)
            {
                case 0: return deg >= 292.5 || deg <= 67.5;
                case 1: return deg >= 22.5 && deg <= 157.5;
                case 2: return deg >= 112.5 && deg <= 247.5;
                default: return deg >= 202.5 && deg <= 337.5;
            }
        }
    }

    // ------------------------------------------------------------------ Binding
    // Spec strings stored in bindings.ini:
    //   K:<mods>:<vk>               keyboard, mods bitmask 1=Ctrl 2=Shift 4=Alt
    //   J:<vid>:<pid>:B<n>          controller button n (1-based)
    //   J:<vid>:<pid>:P<d>          controller POV direction d (0 up,1 right,2 down,3 left)
    class Binding
    {
        public readonly string Spec;
        readonly char _kind;
        readonly int _mods, _vk, _button = -1, _pov = -1;
        readonly ushort _vid, _pid;

        Binding(string spec, char kind, int mods, int vk, ushort vid, ushort pid, int button, int pov)
        {
            Spec = spec; _kind = kind; _mods = mods; _vk = vk; _vid = vid; _pid = pid; _button = button; _pov = pov;
        }

        public static Binding Key(int mods, int vk) { return Parse(string.Format("K:{0}:{1}", mods, vk)); }
        public static Binding JoyButton(ushort vid, ushort pid, int button) { return Parse(string.Format("J:{0:X4}:{1:X4}:B{2}", vid, pid, button)); }
        public static Binding JoyPov(ushort vid, ushort pid, int dir) { return Parse(string.Format("J:{0:X4}:{1:X4}:P{2}", vid, pid, dir)); }

        public static Binding Parse(string spec)
        {
            try
            {
                var p = spec.Split(':');
                if (p[0] == "K") return new Binding(spec, 'K', int.Parse(p[1]), int.Parse(p[2]), 0, 0, -1, -1);
                if (p[0] == "J")
                {
                    ushort vid = Convert.ToUInt16(p[1], 16), pid = Convert.ToUInt16(p[2], 16);
                    int n = int.Parse(p[3].Substring(1));
                    return p[3][0] == 'B' ? new Binding(spec, 'J', 0, 0, vid, pid, n, -1)
                                          : new Binding(spec, 'J', 0, 0, vid, pid, -1, n);
                }
            }
            catch (Exception) { }
            return null;
        }

        public static int CurrentMods()
        {
            return (InputNative.KeyDown(0x11) ? 1 : 0) | (InputNative.KeyDown(0x10) ? 2 : 0) | (InputNative.KeyDown(0x12) ? 4 : 0);
        }

        // Call Joysticks.Poll() once per tick before this.
        public bool IsDown(int mods)
        {
            if (_kind == 'K') return mods == _mods && InputNative.KeyDown(_vk);
            var d = Joysticks.Find(_vid, _pid);
            if (d == null) return false;
            if (_button > 0) return (d.Buttons & (1 << (_button - 1))) != 0;
            return Joysticks.PovDir(d.Pov, _pov);
        }

        public override string ToString()
        {
            if (_kind == 'K')
            {
                string s = "";
                if ((_mods & 1) != 0) s += "Ctrl+";
                if ((_mods & 2) != 0) s += "Shift+";
                if ((_mods & 4) != 0) s += "Alt+";
                return s + KeyName(_vk);
            }
            string what = _button > 0 ? "botão " + _button : "direcional " + new[] { "↑", "→", "↓", "←" }[_pov & 3];
            return Joysticks.NameOf(_vid, _pid) + " - " + what;
        }

        static string KeyName(int vk)
        {
            if (vk >= 0x30 && vk <= 0x39) return ((char)vk).ToString();
            if (vk >= 0x60 && vk <= 0x69) return "Num" + (vk - 0x60);
            return ((Keys)vk).ToString();
        }
    }

    // ------------------------------------------------------------------ Actions
    class ActionDef
    {
        public string Id, Label;
        public bool Repeat;  // keeps firing while held
        public ActionDef(string id, string label, bool repeat) { Id = id; Label = label; Repeat = repeat; }

        public static readonly ActionDef[] All =
        {
            new ActionDef("recenter",   "Recentralizar painel selecionado", false),
            new ActionDef("recenterAll","Recentralizar todos",              false),
            new ActionDef("toggleAll",  "Mostrar / esconder todos",         false),
            new ActionDef("nextPanel",  "Próximo painel",                   false),
            new ActionDef("prevPanel",  "Painel anterior",                  false),
            new ActionDef("togglePanel","Ativar / desativar painel selecionado", false),
            new ActionDef("up",         "Mover para cima",                  true),
            new ActionDef("down",       "Mover para baixo",                 true),
            new ActionDef("left",       "Mover para a esquerda",            true),
            new ActionDef("right",      "Mover para a direita",             true),
            new ActionDef("bigger",     "Maior",                            true),
            new ActionDef("smaller",    "Menor",                            true),
            new ActionDef("nearer",     "Mais perto",                       true),
            new ActionDef("further",    "Mais longe",                       true),
            new ActionDef("tiltUp",     "Inclinar para cima",               true),
            new ActionDef("tiltDown",   "Inclinar para baixo",              true),
            new ActionDef("turnLeft",   "Girar para a esquerda",            true),
            new ActionDef("turnRight",  "Girar para a direita",             true),
        };

        public static ActionDef Get(string id)
        {
            foreach (var a in All) if (a.Id == id) return a;
            return null;
        }
    }

    // ------------------------------------------------------------------ Storage
    class BindingStore
    {
        public readonly Dictionary<string, Binding> Map = new Dictionary<string, Binding>();
        readonly string _path;

        public BindingStore(string dir) { _path = Path.Combine(dir, "bindings.ini"); }

        public void Load()
        {
            Map.Clear();
            if (!File.Exists(_path)) return;
            foreach (var line in File.ReadAllLines(_path, Encoding.UTF8))
            {
                int eq = line.IndexOf('=');
                if (eq <= 0 || line.StartsWith(";")) continue;
                var b = Binding.Parse(line.Substring(eq + 1).Trim());
                string id = line.Substring(0, eq).Trim();
                if (b != null && ActionDef.Get(id) != null) Map[id] = b;
            }
        }

        public void Save()
        {
            var sb = new StringBuilder("; XR Window Panels - atalhos (editados pelo app de controle)\r\n");
            foreach (var a in ActionDef.All)
            {
                Binding b;
                if (Map.TryGetValue(a.Id, out b)) sb.Append(a.Id).Append('=').Append(b.Spec).Append("\r\n");
            }
            File.WriteAllText(_path, sb.ToString(), Encoding.UTF8);
        }
    }

    // ------------------------------------------------------------------ Poller
    class InputPoller : IDisposable
    {
        class State { public bool Down; public int NextRepeat; }

        readonly BindingStore _store;
        readonly Dictionary<string, State> _state = new Dictionary<string, State>();
        readonly Timer _timer = new Timer { Interval = 15 };
        public event Action<string> Fired;
        public bool Paused;

        public InputPoller(BindingStore store)
        {
            _store = store;
            _timer.Tick += delegate { Tick(); };
            _timer.Start();
        }

        void Tick()
        {
            if (Paused || _store.Map.Count == 0) return;
            bool anyJoy = false;
            foreach (var b in _store.Map.Values) if (b.Spec[0] == 'J') { anyJoy = true; break; }
            if (anyJoy) Joysticks.Poll();
            int mods = Binding.CurrentMods();
            int now = Environment.TickCount;

            foreach (var kv in _store.Map)
            {
                State st;
                if (!_state.TryGetValue(kv.Key, out st)) _state[kv.Key] = st = new State();
                bool down = kv.Value.IsDown(mods);
                if (down && !st.Down)
                {
                    st.NextRepeat = now + 400;
                    Fire(kv.Key);
                }
                else if (down && ActionDef.Get(kv.Key).Repeat && now - st.NextRepeat >= 0)
                {
                    st.NextRepeat = now + 60;
                    Fire(kv.Key);
                }
                st.Down = down;
            }
        }

        void Fire(string id) { if (Fired != null) Fired(id); }

        // Forget held state (e.g. after editing bindings) so nothing fires spuriously.
        public void Reset() { _state.Clear(); }

        public void Dispose() { _timer.Dispose(); }
    }

    // ------------------------------------------------------------------ "Press a key" dialog
    class CaptureForm : Form
    {
        readonly Timer _timer = new Timer { Interval = 15 };
        readonly HashSet<string> _held = new HashSet<string>();
        public Binding Result;

        public CaptureForm(string actionLabel)
        {
            Text = "Definir atalho";
            Font = new Font("Segoe UI", 9f);
            AutoScaleMode = AutoScaleMode.Font;
            ClientSize = new Size(420, 130);
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = MinimizeBox = false;
            StartPosition = FormStartPosition.CenterParent;
            KeyPreview = true;

            var title = new Label { Text = actionLabel, Font = new Font("Segoe UI", 11f, FontStyle.Bold), AutoSize = false };
            title.SetBounds(16, 14, 390, 24);
            var hint = new Label
            {
                Text = "Pressione uma tecla (pode combinar com Ctrl, Shift ou Alt) ou um botão do volante / button box.\nEsc cancela.",
                AutoSize = false
            };
            hint.SetBounds(16, 44, 390, 44);
            var cancel = new Button { Text = "Cancelar", DialogResult = DialogResult.Cancel };
            cancel.SetBounds(316, 92, 90, 28);
            Controls.AddRange(new Control[] { title, hint, cancel });
            CancelButton = cancel;

            Snapshot(_held);  // ignore anything already held when the dialog opens
            _timer.Tick += delegate { Tick(); };
            _timer.Start();
        }

        // Raw ids of everything currently pressed (keys without modifiers, controller buttons/POV).
        static void Snapshot(HashSet<string> set)
        {
            set.Clear();
            for (int vk = 0x08; vk <= 0xFE; vk++)
            {
                if (IsModifierOrMouse(vk)) continue;
                if (InputNative.KeyDown(vk)) set.Add("K" + vk);
            }
            Joysticks.Poll();
            foreach (var d in Joysticks.Devices)
            {
                for (int b = 0; b < 32; b++) if ((d.Buttons & (1 << b)) != 0) set.Add(string.Format("B{0}:{1}", d.Id, b + 1));
                for (int dir = 0; dir < 4; dir++) if (Joysticks.PovDir(d.Pov, dir)) set.Add(string.Format("P{0}:{1}", d.Id, dir));
            }
        }

        static bool IsModifierOrMouse(int vk)
        {
            return vk == 0x10 || vk == 0x11 || vk == 0x12 || (vk >= 0xA0 && vk <= 0xA5) || vk == 0x5B || vk == 0x5C;
        }

        void Tick()
        {
            var now = new HashSet<string>();
            Snapshot(now);
            foreach (var id in now)
            {
                if (_held.Contains(id)) continue;
                if (id == "K27") { DialogResult = DialogResult.Cancel; return; }  // Esc
                if (id[0] == 'K')
                {
                    Result = Binding.Key(Binding.CurrentMods(), int.Parse(id.Substring(1)));
                }
                else
                {
                    var parts = id.Substring(1).Split(':');
                    int devId = int.Parse(parts[0]), n = int.Parse(parts[1]);
                    JoyDevice dev = null;
                    foreach (var d in Joysticks.Devices) if (d.Id == devId) dev = d;
                    if (dev == null) continue;
                    Result = id[0] == 'B' ? Binding.JoyButton(dev.Vid, dev.Pid, n) : Binding.JoyPov(dev.Vid, dev.Pid, n);
                }
                DialogResult = DialogResult.OK;
                return;
            }
            _held.IntersectWith(now);  // released items can be pressed again
        }

        // Keys reach the dialog too; swallow them so e.g. Enter doesn't click a button.
        protected override bool ProcessCmdKey(ref Message msg, Keys keyData) { return keyData != Keys.Escape || base.ProcessCmdKey(ref msg, keyData); }
        protected override void Dispose(bool disposing) { if (disposing) _timer.Dispose(); base.Dispose(disposing); }
    }

    // ------------------------------------------------------------------ Bindings list dialog
    class BindingsForm : Form
    {
        readonly BindingStore _store;
        readonly ListView _view;

        public BindingsForm(BindingStore store)
        {
            _store = store;
            Text = "Atalhos - teclado e volante";
            Font = new Font("Segoe UI", 9f);
            AutoScaleMode = AutoScaleMode.Font;
            ClientSize = new Size(560, 520);
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = MinimizeBox = false;
            StartPosition = FormStartPosition.CenterParent;

            var hint = new Label
            {
                Text = "Duplo clique numa ação para definir o atalho. Os atalhos funcionam com o jogo em foco, " +
                       "enquanto o app estiver aberto (pode ficar minimizado na bandeja).\n" +
                       "Ctrl+F11 e Ctrl+F12 já são atalhos fixos da camada. Volantes: botões 1 a 32 e direcional (POV).",
                AutoSize = false
            };
            hint.SetBounds(12, 10, 536, 52);

            _view = new ListView { View = View.Details, FullRowSelect = true, MultiSelect = false, HideSelection = false, GridLines = true };
            _view.SetBounds(12, 66, 536, 402);
            _view.Columns.Add("Ação", 230);
            _view.Columns.Add("Atalho", 300);
            _view.DoubleClick += delegate { Define(); };

            var define = new Button { Text = "Definir..." };
            define.SetBounds(12, 478, 100, 30);
            define.Click += delegate { Define(); };
            var clear = new Button { Text = "Limpar" };
            clear.SetBounds(118, 478, 100, 30);
            clear.Click += delegate { Clear(); };
            var close = new Button { Text = "Fechar", DialogResult = DialogResult.OK };
            close.SetBounds(448, 478, 100, 30);
            CancelButton = close;

            Controls.AddRange(new Control[] { hint, _view, define, clear, close });
            Joysticks.Poll();
            Fill();
        }

        void Fill()
        {
            int sel = _view.SelectedIndices.Count > 0 ? _view.SelectedIndices[0] : 0;
            _view.BeginUpdate();
            _view.Items.Clear();
            foreach (var a in ActionDef.All)
            {
                Binding b;
                var item = new ListViewItem(a.Label) { Tag = a };
                item.SubItems.Add(_store.Map.TryGetValue(a.Id, out b) ? b.ToString() : "");
                _view.Items.Add(item);
            }
            _view.EndUpdate();
            if (_view.Items.Count > 0) _view.Items[Math.Min(sel, _view.Items.Count - 1)].Selected = true;
        }

        ActionDef Selected { get { return _view.SelectedItems.Count > 0 ? (ActionDef)_view.SelectedItems[0].Tag : null; } }

        void Define()
        {
            var a = Selected;
            if (a == null) return;
            using (var dlg = new CaptureForm(a.Label))
            {
                if (dlg.ShowDialog(this) != DialogResult.OK || dlg.Result == null) return;
                // One input = one action: remove it from any other action.
                string other = null;
                foreach (var kv in _store.Map) if (kv.Value.Spec == dlg.Result.Spec && kv.Key != a.Id) other = kv.Key;
                if (other != null) _store.Map.Remove(other);
                _store.Map[a.Id] = dlg.Result;
                _store.Save();
                Fill();
            }
        }

        void Clear()
        {
            var a = Selected;
            if (a == null || !_store.Map.Remove(a.Id)) return;
            _store.Save();
            Fill();
        }
    }
}
