// Global input for the control app:
//  * keyboard: GetAsyncKeyState, polled from a UI timer
//  * wheels / button boxes / joysticks: Raw Input (HID). Event-driven, no button limit (HID button
//    usages 1..N, e.g. Simagic bases expose 128), plus the POV hat. Works while the game has focus
//    (RIDEV_INPUTSINK) and never touches the game process.

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
        public static bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

        // Raw Input
        [StructLayout(LayoutKind.Sequential)]
        public struct RAWINPUTDEVICE { public ushort usUsagePage, usUsage; public uint dwFlags; public IntPtr hwndTarget; }
        [StructLayout(LayoutKind.Sequential)]
        public struct RAWINPUTHEADER { public uint dwType, dwSize; public IntPtr hDevice, wParam; }
        [StructLayout(LayoutKind.Sequential)]
        public struct RAWINPUTDEVICELIST { public IntPtr hDevice; public uint dwType; }

        [DllImport("user32.dll", SetLastError = true)] public static extern bool RegisterRawInputDevices(RAWINPUTDEVICE[] devices, uint count, uint size);
        [DllImport("user32.dll")] public static extern uint GetRawInputData(IntPtr hRawInput, uint command, IntPtr data, ref uint size, uint headerSize);
        [DllImport("user32.dll")] public static extern uint GetRawInputDeviceInfo(IntPtr hDevice, uint command, IntPtr data, ref uint size);
        [DllImport("user32.dll")] public static extern uint GetRawInputDeviceList(IntPtr list, ref uint count, uint size);

        public const uint RID_INPUT = 0x10000003, RIM_TYPEHID = 2;
        public const uint RIDI_PREPARSEDDATA = 0x20000005, RIDI_DEVICENAME = 0x20000007, RIDI_DEVICEINFO = 0x2000000b;
        public const uint RIDEV_INPUTSINK = 0x100, RIDEV_DEVNOTIFY = 0x2000;
        public const int WM_INPUT = 0x00FF, WM_INPUT_DEVICE_CHANGE = 0x00FE;

        // HID parser
        [DllImport("hid.dll")] public static extern uint HidP_MaxUsageListLength(int reportType, ushort usagePage, IntPtr preparsed);
        [DllImport("hid.dll")] public static extern int HidP_GetUsages(int reportType, ushort usagePage, ushort linkCollection, [Out] ushort[] usages, ref uint usageLength, IntPtr preparsed, IntPtr report, uint reportLength);
        [DllImport("hid.dll")] public static extern int HidP_GetUsageValue(int reportType, ushort usagePage, ushort linkCollection, ushort usage, out uint value, IntPtr preparsed, IntPtr report, uint reportLength);
        [DllImport("hid.dll")] public static extern int HidP_GetSpecificValueCaps(int reportType, ushort usagePage, ushort linkCollection, ushort usage, [Out] byte[] valueCaps, ref ushort length, IntPtr preparsed);
        [DllImport("hid.dll", CharSet = CharSet.Unicode)] public static extern bool HidD_GetProductString(IntPtr file, StringBuilder buffer, int bufferBytes);
        public const int HIDP_STATUS_SUCCESS = 0x00110000;
        public const int HidP_Input = 0;

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern IntPtr CreateFile(string name, uint access, uint share, IntPtr security, uint disposition, uint flags, IntPtr template);
        [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
    }

    // ------------------------------------------------------------------ Game controllers (Raw Input HID)
    class HidDevice
    {
        public IntPtr Handle;
        public ushort Vid, Pid;
        public string Name;
        public IntPtr Preparsed;          // unmanaged copy of the HID preparsed data
        public uint MaxButtons;
        public bool HasHat;
        public int HatMin, HatMax;
        public readonly HashSet<int> Pressed = new HashSet<int>();  // HID button usages (1-based)
        public int Pov = -1;              // hundredths of a degree, -1 = centered
    }

    static class RawHid
    {
        static readonly Dictionary<IntPtr, HidDevice> _devices = new Dictionary<IntPtr, HidDevice>();
        static readonly HashSet<IntPtr> _ignored = new HashSet<IntPtr>();
        static SinkWindow _window;

        public static event Action<HidDevice> Changed;  // fired on every report (diagnostics)
        public static ICollection<HidDevice> Devices { get { return _devices.Values; } }

        class SinkWindow : NativeWindow
        {
            public SinkWindow() { CreateHandle(new CreateParams()); }
            protected override void WndProc(ref Message m)
            {
                if (m.Msg == InputNative.WM_INPUT) OnInput(m.LParam);
                else if (m.Msg == InputNative.WM_INPUT_DEVICE_CHANGE)
                {
                    if ((int)m.WParam == 1) Get(m.LParam);          // arrival
                    else if ((int)m.WParam == 2) Remove(m.LParam);  // removal
                }
                base.WndProc(ref m);
            }
        }

        public static void Init()
        {
            if (_window != null) return;
            _window = new SinkWindow();
            var reg = new InputNative.RAWINPUTDEVICE[3];
            ushort[] usages = { 4 /*joystick*/, 5 /*gamepad*/, 8 /*multi-axis*/ };
            for (int i = 0; i < 3; i++)
            {
                reg[i].usUsagePage = 1;
                reg[i].usUsage = usages[i];
                reg[i].dwFlags = InputNative.RIDEV_INPUTSINK | InputNative.RIDEV_DEVNOTIFY;
                reg[i].hwndTarget = _window.Handle;
            }
            InputNative.RegisterRawInputDevices(reg, 3, (uint)Marshal.SizeOf(typeof(InputNative.RAWINPUTDEVICE)));
            EnumerateExisting();
        }

        static void EnumerateExisting()
        {
            uint count = 0, size = (uint)Marshal.SizeOf(typeof(InputNative.RAWINPUTDEVICELIST));
            InputNative.GetRawInputDeviceList(IntPtr.Zero, ref count, size);
            if (count == 0) return;
            IntPtr buf = Marshal.AllocHGlobal((int)(count * size));
            try
            {
                if (InputNative.GetRawInputDeviceList(buf, ref count, size) == uint.MaxValue) return;
                for (int i = 0; i < count; i++)
                {
                    var e = (InputNative.RAWINPUTDEVICELIST)Marshal.PtrToStructure(IntPtr.Add(buf, (int)(i * size)), typeof(InputNative.RAWINPUTDEVICELIST));
                    if (e.dwType == InputNative.RIM_TYPEHID) Get(e.hDevice);
                }
            }
            finally { Marshal.FreeHGlobal(buf); }
        }

        static HidDevice Get(IntPtr h)
        {
            HidDevice d;
            if (h == IntPtr.Zero || _ignored.Contains(h)) return null;
            if (_devices.TryGetValue(h, out d)) return d;

            // RID_DEVICE_INFO: cbSize, dwType, then (HID) vid, pid, version, usagePage, usage
            uint size = 32;
            IntPtr info = Marshal.AllocHGlobal(32);
            try
            {
                Marshal.WriteInt32(info, 32);
                if (InputNative.GetRawInputDeviceInfo(h, InputNative.RIDI_DEVICEINFO, info, ref size) == uint.MaxValue ||
                    Marshal.ReadInt32(info, 4) != InputNative.RIM_TYPEHID) { _ignored.Add(h); return null; }
                ushort usagePage = (ushort)Marshal.ReadInt16(info, 20), usage = (ushort)Marshal.ReadInt16(info, 22);
                if (usagePage != 1 || (usage != 4 && usage != 5 && usage != 8)) { _ignored.Add(h); return null; }
                d = new HidDevice { Handle = h, Vid = (ushort)Marshal.ReadInt32(info, 8), Pid = (ushort)Marshal.ReadInt32(info, 12) };
            }
            finally { Marshal.FreeHGlobal(info); }

            uint psize = 0;
            InputNative.GetRawInputDeviceInfo(h, InputNative.RIDI_PREPARSEDDATA, IntPtr.Zero, ref psize);
            if (psize == 0) { _ignored.Add(h); return null; }
            d.Preparsed = Marshal.AllocHGlobal((int)psize);
            if (InputNative.GetRawInputDeviceInfo(h, InputNative.RIDI_PREPARSEDDATA, d.Preparsed, ref psize) == uint.MaxValue)
            {
                Marshal.FreeHGlobal(d.Preparsed);
                _ignored.Add(h);
                return null;
            }
            d.MaxButtons = InputNative.HidP_MaxUsageListLength(InputNative.HidP_Input, 9, d.Preparsed);

            // POV hat (usage 0x39): logical range tells how many positions it has.
            ushort capsLen = 1;
            var caps = new byte[72 * 4];
            capsLen = 4;
            if (InputNative.HidP_GetSpecificValueCaps(InputNative.HidP_Input, 1, 0, 0x39, caps, ref capsLen, d.Preparsed) == InputNative.HIDP_STATUS_SUCCESS && capsLen > 0)
            {
                d.HasHat = true;
                d.HatMin = BitConverter.ToInt32(caps, 40);
                d.HatMax = BitConverter.ToInt32(caps, 44);
            }

            d.Name = FriendlyName(h, d.Vid, d.Pid);
            _devices[h] = d;
            return d;
        }

        static void Remove(IntPtr h)
        {
            HidDevice d;
            if (_devices.TryGetValue(h, out d))
            {
                Marshal.FreeHGlobal(d.Preparsed);
                _devices.Remove(h);
            }
            _ignored.Remove(h);
        }

        static void OnInput(IntPtr hRaw)
        {
            uint header = (uint)Marshal.SizeOf(typeof(InputNative.RAWINPUTHEADER)), size = 0;
            InputNative.GetRawInputData(hRaw, InputNative.RID_INPUT, IntPtr.Zero, ref size, header);
            if (size == 0) return;
            IntPtr buf = Marshal.AllocHGlobal((int)size);
            try
            {
                if (InputNative.GetRawInputData(hRaw, InputNative.RID_INPUT, buf, ref size, header) == uint.MaxValue) return;
                var hdr = (InputNative.RAWINPUTHEADER)Marshal.PtrToStructure(buf, typeof(InputNative.RAWINPUTHEADER));
                if (hdr.dwType != InputNative.RIM_TYPEHID) return;
                var d = Get(hdr.hDevice);
                if (d == null) return;
                int sizeHid = Marshal.ReadInt32(buf, (int)header), count = Marshal.ReadInt32(buf, (int)header + 4);
                IntPtr data = IntPtr.Add(buf, (int)header + 8);
                for (int i = 0; i < count; i++) Parse(d, IntPtr.Add(data, i * sizeHid), (uint)sizeHid);
            }
            finally { Marshal.FreeHGlobal(buf); }
        }

        static void Parse(HidDevice d, IntPtr report, uint len)
        {
            bool changed = false;
            if (d.MaxButtons > 0)
            {
                var usages = new ushort[d.MaxButtons];
                uint n = d.MaxButtons;
                // Fails with "incompatible report id" for reports that carry no buttons: keep state.
                if (InputNative.HidP_GetUsages(InputNative.HidP_Input, 9, 0, usages, ref n, d.Preparsed, report, len) == InputNative.HIDP_STATUS_SUCCESS)
                {
                    d.Pressed.Clear();
                    for (int i = 0; i < n; i++) d.Pressed.Add(usages[i]);
                    changed = true;
                }
            }
            if (d.HasHat)
            {
                uint v;
                if (InputNative.HidP_GetUsageValue(InputNative.HidP_Input, 1, 0, 0x39, out v, d.Preparsed, report, len) == InputNative.HIDP_STATUS_SUCCESS)
                {
                    int iv = (int)v, range = d.HatMax - d.HatMin + 1;
                    d.Pov = (range > 0 && iv >= d.HatMin && iv <= d.HatMax) ? (iv - d.HatMin) * 36000 / range : -1;
                    changed = true;
                }
            }
            if (changed && Changed != null) Changed(d);
        }

        public static bool AnyDown(ushort vid, ushort pid, int button, int povDir)
        {
            foreach (var d in _devices.Values)
            {
                if (d.Vid != vid || d.Pid != pid) continue;
                if (button > 0 ? d.Pressed.Contains(button) : PovDir(d.Pov, povDir)) return true;
            }
            return false;
        }

        public static string NameOf(ushort vid, ushort pid)
        {
            foreach (var d in _devices.Values) if (d.Vid == vid && d.Pid == pid) return d.Name;
            return OemName(vid, pid) ?? string.Format("Controle {0:X4}:{1:X4}", vid, pid);
        }

        static string FriendlyName(IntPtr h, ushort vid, ushort pid)
        {
            string oem = OemName(vid, pid);
            if (oem != null) return oem;
            try
            {
                uint chars = 0;
                InputNative.GetRawInputDeviceInfo(h, InputNative.RIDI_DEVICENAME, IntPtr.Zero, ref chars);
                IntPtr buf = Marshal.AllocHGlobal((int)(chars + 1) * 2);
                try
                {
                    InputNative.GetRawInputDeviceInfo(h, InputNative.RIDI_DEVICENAME, buf, ref chars);
                    string path = Marshal.PtrToStringUni(buf);
                    IntPtr file = InputNative.CreateFile(path, 0, 3 /*share read|write*/, IntPtr.Zero, 3 /*OPEN_EXISTING*/, 0, IntPtr.Zero);
                    if (file != new IntPtr(-1))
                    {
                        var sb = new StringBuilder(128);
                        bool ok = InputNative.HidD_GetProductString(file, sb, sb.Capacity * 2);
                        InputNative.CloseHandle(file);
                        if (ok && sb.ToString().Trim().Length > 0) return sb.ToString().Trim();
                    }
                }
                finally { Marshal.FreeHGlobal(buf); }
            }
            catch (Exception) { }
            return string.Format("Controle {0:X4}:{1:X4}", vid, pid);
        }

        // Friendly product name registered by Windows for the device.
        static string OemName(ushort vid, ushort pid)
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
            return null;
        }

        // dir: 0 up, 1 right, 2 down, 3 left (diagonals count for both neighbours)
        public static bool PovDir(int pov, int dir)
        {
            if (pov < 0 || pov >= 36000) return false;
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
    //   J:<vid>:<pid>:B<n>          controller button (HID usage n, 1-based)
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

        public bool IsController { get { return _kind == 'J'; } }

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

        public bool IsDown(int mods)
        {
            if (_kind == 'K') return mods == _mods && InputNative.KeyDown(_vk);
            return RawHid.AnyDown(_vid, _pid, _button, _pov);
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
            string what = _button > 0 ? "botão " + _button : "direcional " + PovArrow(_pov);
            return RawHid.NameOf(_vid, _pid) + " - " + what;
        }

        public static string PovArrow(int dir) { return new[] { "↑", "→", "↓", "←" }[dir & 3]; }

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
            RawHid.Init();
            _timer.Tick += delegate { Tick(); };
            _timer.Start();
        }

        void Tick()
        {
            if (Paused || _store.Map.Count == 0) return;
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
        readonly Label _devices, _live;
        public Binding Result;

        public CaptureForm(string actionLabel)
        {
            RawHid.Init();
            Text = "Definir atalho";
            Font = new Font("Segoe UI", 9f);
            AutoScaleMode = AutoScaleMode.Font;
            ClientSize = new Size(480, 214);
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = MinimizeBox = false;
            StartPosition = FormStartPosition.CenterParent;
            KeyPreview = true;

            var title = new Label { Text = actionLabel, Font = new Font("Segoe UI", 11f, FontStyle.Bold), AutoSize = false };
            title.SetBounds(16, 14, 450, 24);
            var hint = new Label
            {
                Text = "Pressione uma tecla (pode combinar com Ctrl, Shift ou Alt) ou um botão do volante / button box.\nEsc cancela.",
                AutoSize = false
            };
            hint.SetBounds(16, 42, 450, 54);
            _devices = new Label { AutoSize = false, ForeColor = SystemColors.GrayText };
            _devices.SetBounds(16, 100, 450, 34);
            _live = new Label { AutoSize = false, ForeColor = SystemColors.GrayText, Text = "Aguardando entrada..." };
            _live.SetBounds(16, 136, 450, 34);
            var cancel = new Button { Text = "Cancelar", DialogResult = DialogResult.Cancel };
            cancel.SetBounds(376, 176, 90, 28);
            Controls.AddRange(new Control[] { title, hint, _devices, _live, cancel });
            CancelButton = cancel;

            var names = new List<string>();
            foreach (var d in RawHid.Devices) names.Add(d.Name);
            _devices.Text = names.Count > 0 ? "Controles detectados: " + string.Join(", ", names.ToArray())
                                            : "Nenhum volante / controle HID detectado.";

            RawHid.Changed += OnHid;
            Snapshot(_held);  // ignore anything already held when the dialog opens
            _timer.Tick += delegate { Tick(); };
            _timer.Start();
        }

        // Live diagnostics: show what each controller reports.
        void OnHid(HidDevice d)
        {
            if (d.Pressed.Count == 0 && d.Pov < 0) return;
            var parts = new List<string>();
            foreach (int b in d.Pressed) parts.Add(b.ToString());
            string text = d.Name + ": " + (parts.Count > 0 ? "botões " + string.Join(", ", parts.ToArray()) : "");
            if (d.Pov >= 0) text += "  direcional " + (d.Pov / 100) + "°";
            _live.Text = "Recebendo: " + text;
        }

        // Raw ids of everything currently pressed (keys without modifiers, controller buttons/POV).
        static void Snapshot(HashSet<string> set)
        {
            set.Clear();
            for (int vk = 0x08; vk <= 0xFE; vk++)
            {
                if (IsModifier(vk)) continue;
                if (InputNative.KeyDown(vk)) set.Add("K" + vk);
            }
            foreach (var d in RawHid.Devices)
            {
                foreach (int b in d.Pressed) set.Add(string.Format("B{0:X4}:{1:X4}:{2}", d.Vid, d.Pid, b));
                for (int dir = 0; dir < 4; dir++)
                    if (RawHid.PovDir(d.Pov, dir)) set.Add(string.Format("P{0:X4}:{1:X4}:{2}", d.Vid, d.Pid, dir));
            }
        }

        static bool IsModifier(int vk)
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
                    if (id == "K1" || id == "K2" || id == "K4") continue;  // mouse buttons
                    Result = Binding.Key(Binding.CurrentMods(), int.Parse(id.Substring(1)));
                }
                else
                {
                    var p = id.Substring(1).Split(':');
                    ushort vid = Convert.ToUInt16(p[0], 16), pid = Convert.ToUInt16(p[1], 16);
                    int n = int.Parse(p[2]);
                    Result = id[0] == 'B' ? Binding.JoyButton(vid, pid, n) : Binding.JoyPov(vid, pid, n);
                }
                DialogResult = DialogResult.OK;
                return;
            }
            _held.IntersectWith(now);  // released items can be pressed again
        }

        // Keys reach the dialog too; swallow them so e.g. Enter doesn't click a button.
        protected override bool ProcessCmdKey(ref Message msg, Keys keyData) { return keyData != Keys.Escape || base.ProcessCmdKey(ref msg, keyData); }
        protected override void Dispose(bool disposing)
        {
            if (disposing) { _timer.Dispose(); RawHid.Changed -= OnHid; }
            base.Dispose(disposing);
        }
    }

    // ------------------------------------------------------------------ Bindings list dialog
    class BindingsForm : Form
    {
        readonly BindingStore _store;
        readonly ListView _view;

        public BindingsForm(BindingStore store)
        {
            RawHid.Init();
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
                       "Ctrl+F11 e Ctrl+F12 já são atalhos fixos da camada.",
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
