// XR Window Panels - control app.
// Edits %LOCALAPPDATA%\XRWindowPanels\config.ini and pokes the layer through shared memory
// so changes show up in VR immediately. Shows live status reported by the layer.
//
// Build (C# 5, .NET Framework 4.x - csc ships with Windows):
//   csc /target:winexe /out:XRWindowPanelsControl.exe /r:System.Windows.Forms.dll
//       /r:System.Drawing.dll ControlPanel.cs

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.IO.MemoryMappedFiles;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Windows.Forms;

namespace XRWindowPanels
{
    // ------------------------------------------------------------------ Win32
    static class Native
    {
        public delegate bool EnumProc(IntPtr hwnd, IntPtr lParam);
        [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lParam);
        [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
        [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextLength(IntPtr h);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
        [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
        [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int attr, out int value, int size);
        [DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
        [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] public static extern bool QueryFullProcessImageName(IntPtr h, int flags, StringBuilder name, ref int size);
        [DllImport("kernel32.dll")] public static extern ulong GetTickCount64();
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] public static extern int GetPrivateProfileString(string sec, string key, string def, StringBuilder ret, int size, string file);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] public static extern int GetPrivateProfileSectionNames(char[] buf, int size, string file);
        [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    }

    // ------------------------------------------------------------------ Model
    class PanelData
    {
        public string Section;
        public string Window = "";
        public string Process = "";
        public bool Enabled = true;
        public float X = 0f, Y = -0.15f, Z = -0.7f, Yaw = 0f, Pitch = 0f, Width = 0.35f;
        public float Opacity = 1f, KeyTolerance = 0.1f;
        public bool UseWindowAlpha, ChromaKey;
        public string KeyColor = "#000000";
        public int MaxFps = 30;
        public int RecenterSeq;

        public override string ToString()
        {
            string target = Window.Length > 0 ? Window : (Process.Length > 0 ? Process : "(escolha uma janela)");
            return (Enabled ? "" : "[off] ") + target;
        }
    }

    class ConfigFile
    {
        public readonly string Path;
        public readonly List<PanelData> Panels = new List<PanelData>();
        public int RecenterAll;

        public ConfigFile()
        {
            string dir = System.IO.Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "XRWindowPanels");
            Directory.CreateDirectory(dir);
            Path = System.IO.Path.Combine(dir, "config.ini");
        }

        string Read(string sec, string key, string def)
        {
            var sb = new StringBuilder(1024);
            Native.GetPrivateProfileString(sec, key, def, sb, sb.Capacity, Path);
            return sb.ToString();
        }
        float ReadF(string sec, string key, float def)
        {
            string s = Read(sec, key, "").Replace(',', '.');
            float v;
            return float.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out v) ? v : def;
        }
        int ReadI(string sec, string key, int def)
        {
            int v;
            return int.TryParse(Read(sec, key, ""), out v) ? v : def;
        }
        bool ReadB(string sec, string key, bool def)
        {
            string s = Read(sec, key, "");
            if (s.Length == 0) return def;
            char c = char.ToLowerInvariant(s[0]);
            return c == '1' || c == 't' || c == 's';
        }

        public void Load()
        {
            Panels.Clear();
            if (!File.Exists(Path)) return;
            RecenterAll = ReadI("Control", "RecenterAll", 0);
            var buf = new char[32768];
            int n = Native.GetPrivateProfileSectionNames(buf, buf.Length, Path);
            foreach (string sec in new string(buf, 0, n).Split(new[] { '\0' }, StringSplitOptions.RemoveEmptyEntries))
            {
                if (!sec.StartsWith("Panel", StringComparison.OrdinalIgnoreCase)) continue;
                var p = new PanelData();
                p.Section = sec;
                p.Window = Read(sec, "Window", "");
                p.Process = Read(sec, "Process", "");
                p.Enabled = ReadB(sec, "Enabled", true);
                p.X = ReadF(sec, "X", p.X); p.Y = ReadF(sec, "Y", p.Y); p.Z = ReadF(sec, "Z", p.Z);
                p.Yaw = ReadF(sec, "Yaw", 0); p.Pitch = ReadF(sec, "Pitch", 0);
                p.Width = ReadF(sec, "Width", p.Width);
                p.Opacity = ReadF(sec, "Opacity", 1);
                p.UseWindowAlpha = ReadB(sec, "UseWindowAlpha", false);
                p.ChromaKey = ReadB(sec, "ChromaKey", false);
                p.KeyColor = Read(sec, "KeyColor", "#000000");
                p.KeyTolerance = ReadF(sec, "KeyTolerance", 0.1f);
                p.MaxFps = ReadI(sec, "MaxFps", 30);
                p.RecenterSeq = ReadI(sec, "RecenterSeq", 0);
                Panels.Add(p);
            }
        }

        static string F(float v) { return v.ToString("0.####", CultureInfo.InvariantCulture); }

        public void Save()
        {
            var sb = new StringBuilder();
            sb.AppendLine("; XR Window Panels - gerado pelo XRWindowPanelsControl.exe");
            sb.AppendLine("; Atalhos no jogo: Ctrl+F12 mostra/esconde, Ctrl+F11 recentraliza todos.");
            sb.AppendLine();
            sb.AppendLine("[Control]");
            sb.AppendLine("RecenterAll=" + RecenterAll);
            foreach (var p in Panels)
            {
                sb.AppendLine();
                sb.AppendLine("[" + p.Section + "]");
                sb.AppendLine("Window=" + p.Window);
                sb.AppendLine("Process=" + p.Process);
                sb.AppendLine("Enabled=" + (p.Enabled ? 1 : 0));
                sb.AppendLine("X=" + F(p.X));
                sb.AppendLine("Y=" + F(p.Y));
                sb.AppendLine("Z=" + F(p.Z));
                sb.AppendLine("Yaw=" + F(p.Yaw));
                sb.AppendLine("Pitch=" + F(p.Pitch));
                sb.AppendLine("Width=" + F(p.Width));
                sb.AppendLine("Opacity=" + F(p.Opacity));
                sb.AppendLine("UseWindowAlpha=" + (p.UseWindowAlpha ? 1 : 0));
                sb.AppendLine("ChromaKey=" + (p.ChromaKey ? 1 : 0));
                sb.AppendLine("KeyColor=" + p.KeyColor);
                sb.AppendLine("KeyTolerance=" + F(p.KeyTolerance));
                sb.AppendLine("MaxFps=" + p.MaxFps);
                sb.AppendLine("RecenterSeq=" + p.RecenterSeq);
            }
            // Write a temp file and swap it in, so the layer never reads a half-written file.
            string tmp = Path + ".tmp";
            File.WriteAllText(tmp, sb.ToString(), Encoding.Unicode);  // UTF-16 + BOM
            for (int attempt = 0; ; attempt++)
            {
                try
                {
                    if (File.Exists(Path)) File.Replace(tmp, Path, null);
                    else File.Move(tmp, Path);
                    break;
                }
                catch (IOException)
                {
                    if (attempt >= 10) throw;
                    Thread.Sleep(15);
                }
            }
        }

        public string NewSectionName()
        {
            int max = 0;
            foreach (var p in Panels)
            {
                int n;
                if (int.TryParse(p.Section.Substring(5), out n) && n > max) max = n;
            }
            return "Panel" + (max + 1);
        }
    }

    // ------------------------------------------------------------------ Shared memory (see src/shared.h)
    class LayerLink : IDisposable
    {
        const string Name = "Local\\XRWindowPanels_Shared_v1";
        const uint Magic = 0x50575258;
        MemoryMappedFile _mmf;
        MemoryMappedViewAccessor _view;

        bool Open()
        {
            if (_view != null) return true;
            try
            {
                _mmf = MemoryMappedFile.OpenExisting(Name, MemoryMappedFileRights.ReadWrite);
                _view = _mmf.CreateViewAccessor(0, 0, MemoryMappedFileAccess.ReadWrite);
                return true;
            }
            catch (Exception) { Dispose(); return false; }
        }

        public void Poke()
        {
            if (!Open()) return;
            _view.Write(8, _view.ReadInt32(8) + 1);
        }

        public bool Connected(out string app)
        {
            app = "";
            if (!Open() || _view.ReadUInt32(0) != Magic) return false;
            long hb = _view.ReadInt64(16);
            if ((long)Native.GetTickCount64() - hb > 2500) return false;
            app = ReadString(24, 64);
            return true;
        }

        public bool PanelStatus(string section, out int state, out int w, out int h, out int fps)
        {
            state = w = h = fps = 0;
            if (_view == null) return false;
            int count = Math.Min(_view.ReadInt32(152), 16);
            for (int i = 0; i < count; i++)
            {
                long b = 156 + i * 80;
                if (ReadString(b, 32) != section) continue;
                state = _view.ReadInt32(b + 64);
                w = _view.ReadInt32(b + 68);
                h = _view.ReadInt32(b + 72);
                fps = _view.ReadInt32(b + 76);
                return true;
            }
            return false;
        }

        string ReadString(long offset, int maxChars)
        {
            var chars = new char[maxChars];
            _view.ReadArray(offset, chars, 0, maxChars);
            int len = Array.IndexOf(chars, '\0');
            return new string(chars, 0, len < 0 ? maxChars : len);
        }

        public void Dispose()
        {
            if (_view != null) _view.Dispose();
            if (_mmf != null) _mmf.Dispose();
            _view = null; _mmf = null;
        }
    }

    // ------------------------------------------------------------------ Window list
    class WinItem
    {
        public string Title, Process;
        public override string ToString() { return Title + "   [" + Process + "]"; }
    }

    static class Windows
    {
        public static List<WinItem> List()
        {
            var list = new List<WinItem>();
            uint self = (uint)System.Diagnostics.Process.GetCurrentProcess().Id;
            Native.EnumWindows(delegate (IntPtr h, IntPtr l)
            {
                if (!Native.IsWindowVisible(h)) return true;
                int cloaked;
                if (Native.DwmGetWindowAttribute(h, 14 /*DWMWA_CLOAKED*/, out cloaked, 4) == 0 && cloaked != 0) return true;
                int len = Native.GetWindowTextLength(h);
                if (len <= 0) return true;
                var sb = new StringBuilder(len + 1);
                Native.GetWindowText(h, sb, sb.Capacity);
                uint pid;
                Native.GetWindowThreadProcessId(h, out pid);
                if (pid == self) return true;
                string title = sb.ToString();
                if (title == "Program Manager") return true;
                list.Add(new WinItem { Title = title, Process = ProcessName(pid) });
                return true;
            }, IntPtr.Zero);
            list.Sort(delegate (WinItem a, WinItem b) { return string.Compare(a.Title, b.Title, StringComparison.CurrentCultureIgnoreCase); });
            return list;
        }

        static string ProcessName(uint pid)
        {
            IntPtr h = Native.OpenProcess(0x1000 /*QUERY_LIMITED_INFORMATION*/, false, pid);
            if (h == IntPtr.Zero) return "?";
            try
            {
                var sb = new StringBuilder(1024);
                int size = sb.Capacity;
                return Native.QueryFullProcessImageName(h, 0, sb, ref size) ? System.IO.Path.GetFileName(sb.ToString()) : "?";
            }
            finally { Native.CloseHandle(h); }
        }
    }

    // ------------------------------------------------------------------ Hold-to-repeat button
    class RepeatButton : Button
    {
        readonly System.Windows.Forms.Timer _timer = new System.Windows.Forms.Timer();
        public event EventHandler Step;

        public RepeatButton()
        {
            _timer.Tick += delegate { _timer.Interval = 60; Fire(); };
        }
        void Fire() { if (Step != null) Step(this, EventArgs.Empty); }
        protected override void OnMouseDown(MouseEventArgs e)
        {
            base.OnMouseDown(e);
            if (e.Button != MouseButtons.Left) return;
            Fire();
            _timer.Interval = 400;
            _timer.Start();
        }
        protected override void OnMouseUp(MouseEventArgs e) { base.OnMouseUp(e); _timer.Stop(); }
        protected override void OnMouseLeave(EventArgs e) { base.OnMouseLeave(e); _timer.Stop(); }
        protected override void Dispose(bool disposing) { if (disposing) _timer.Dispose(); base.Dispose(disposing); }
    }

    // ------------------------------------------------------------------ Main form
    class MainForm : Form
    {
        readonly ConfigFile _cfg = new ConfigFile();
        readonly LayerLink _link = new LayerLink();
        bool _loading;

        ListBox _list;
        ComboBox _windows, _step;
        TextBox _title, _process;
        CheckBox _enabled, _chroma, _alpha;
        Label _panelStatus, _pose, _opacityLabel, _game;
        TrackBar _opacity, _tolerance;
        NumericUpDown _fps;
        Button _color;
        readonly List<Control> _panelControls = new List<Control>();

        PanelData Current { get { return _list.SelectedItem as PanelData; } }

        float StepScale
        {
            get { return _step.SelectedIndex == 0 ? 0.25f : (_step.SelectedIndex == 2 ? 4f : 1f); }
        }

        public MainForm()
        {
            Text = "XR Window Panels";
            Font = new Font("Segoe UI", 9f);
            AutoScaleMode = AutoScaleMode.Font;
            ClientSize = new Size(760, 545);
            FormBorderStyle = FormBorderStyle.FixedSingle;
            MaximizeBox = false;
            StartPosition = FormStartPosition.CenterScreen;

            BuildUi();
            _cfg.Load();
            RefreshPanelList(null);
            RefreshWindows();

            var timer = new System.Windows.Forms.Timer { Interval = 500 };
            timer.Tick += delegate { UpdateStatus(); };
            timer.Start();
            UpdateStatus();
        }

        // ---------------- UI construction
        T Add<T>(Control parent, T c, int x, int y, int w, int h) where T : Control
        {
            c.SetBounds(x, y, w, h);
            parent.Controls.Add(c);
            return c;
        }
        Label AddLabel(Control parent, string text, int x, int y, int w)
        {
            return Add(parent, new Label { Text = text, AutoSize = false }, x, y, w, 18);
        }
        RepeatButton AddStep(Control parent, string text, int x, int y, int w, Action<PanelData, float> apply)
        {
            var b = Add(parent, new RepeatButton { Text = text }, x, y, w, 30);
            if (text.Length == 1) b.Font = new Font("Segoe UI Symbol", 12f);  // arrow glyphs
            b.Step += delegate
            {
                var p = Current;
                if (p == null) return;
                apply(p, StepScale);
                Commit(p);
            };
            _panelControls.Add(b);
            return b;
        }

        void BuildUi()
        {
            // Panel list
            AddLabel(this, "Painéis", 12, 10, 170);
            _list = Add(this, new ListBox { IntegralHeight = false }, 12, 30, 172, 410);
            _list.SelectedIndexChanged += delegate { ShowPanel(); };
            var addBtn = Add(this, new Button { Text = "+ Adicionar" }, 12, 446, 84, 28);
            addBtn.Click += delegate { AddPanel(); };
            var delBtn = Add(this, new Button { Text = "Remover" }, 100, 446, 84, 28);
            delBtn.Click += delegate { RemovePanel(); };

            // Window
            var gWin = Add(this, new GroupBox { Text = "Janela" }, 196, 6, 552, 124);
            _windows = Add(gWin, new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList }, 12, 22, 420, 24);
            _windows.SelectionChangeCommitted += delegate { PickWindow(); };
            var refresh = Add(gWin, new Button { Text = "Atualizar lista" }, 440, 21, 100, 26);
            refresh.Click += delegate { RefreshWindows(); };
            AddLabel(gWin, "Título contém:", 12, 58, 95);
            _title = Add(gWin, new TextBox(), 108, 55, 200, 24);
            AddLabel(gWin, "Processo:", 318, 58, 65);
            _process = Add(gWin, new TextBox(), 386, 55, 154, 24);
            foreach (var tb in new[] { _title, _process })
            {
                tb.Leave += delegate { TargetEdited(); };
                tb.KeyDown += delegate (object s, KeyEventArgs e) { if (e.KeyCode == Keys.Enter) { TargetEdited(); e.SuppressKeyPress = true; } };
            }
            _enabled = Add(gWin, new CheckBox { Text = "Ativo" }, 12, 90, 80, 22);
            _enabled.CheckedChanged += delegate { Edit(delegate (PanelData p) { p.Enabled = _enabled.Checked; }); };
            _panelStatus = AddLabel(gWin, "", 108, 93, 432);
            _panelControls.AddRange(new Control[] { _windows, _title, _process, _enabled });

            // Position
            var gPos = Add(this, new GroupBox { Text = "Posição (segure o botão para repetir)" }, 196, 134, 552, 236);
            var recenter = Add(gPos, new Button { Text = "Recentralizar" }, 12, 22, 120, 30);
            recenter.Click += delegate { Edit(delegate (PanelData p) { p.RecenterSeq++; }); };
            _panelControls.Add(recenter);
            var recenterAll = Add(gPos, new Button { Text = "Recentralizar todos" }, 138, 22, 140, 30);
            recenterAll.Click += delegate { _cfg.RecenterAll++; Save(); };
            var reset = Add(gPos, new Button { Text = "Posição padrão" }, 284, 22, 115, 30);
            reset.Click += delegate { Edit(delegate (PanelData p) { p.X = 0; p.Y = -0.15f; p.Z = -0.7f; p.Yaw = 0; p.Pitch = 0; }); };
            _panelControls.Add(reset);
            AddLabel(gPos, "Passo:", 410, 29, 42);
            _step = Add(gPos, new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList }, 454, 26, 86, 24);
            _step.Items.AddRange(new object[] { "Fino", "Normal", "Grosso" });
            _step.SelectedIndex = 1;

            const float move = 0.01f, size = 0.01f, dist = 0.02f, tilt = 2f;
            AddLabel(gPos, "Mover", 12, 62, 120);
            AddStep(gPos, "▲", 52, 82, 40, delegate (PanelData p, float k) { p.Y += move * k; });
            AddStep(gPos, "◀", 10, 114, 40, delegate (PanelData p, float k) { p.X -= move * k; });
            AddStep(gPos, "▶", 94, 114, 40, delegate (PanelData p, float k) { p.X += move * k; });
            AddStep(gPos, "▼", 52, 146, 40, delegate (PanelData p, float k) { p.Y -= move * k; });

            AddLabel(gPos, "Tamanho", 160, 62, 100);
            AddStep(gPos, "Maior", 160, 82, 95, delegate (PanelData p, float k) { p.Width += size * k; });
            AddStep(gPos, "Menor", 160, 116, 95, delegate (PanelData p, float k) { p.Width = Math.Max(0.05f, p.Width - size * k); });

            AddLabel(gPos, "Distância", 270, 62, 100);
            AddStep(gPos, "Mais perto", 270, 82, 95, delegate (PanelData p, float k) { p.Z = Math.Min(-0.1f, p.Z + dist * k); });
            AddStep(gPos, "Mais longe", 270, 116, 95, delegate (PanelData p, float k) { p.Z -= dist * k; });

            AddLabel(gPos, "Inclinar / girar", 386, 62, 150);
            AddStep(gPos, "▲", 428, 82, 40, delegate (PanelData p, float k) { p.Pitch -= tilt * k; });
            AddStep(gPos, "◀", 386, 114, 40, delegate (PanelData p, float k) { p.Yaw -= tilt * k; });
            AddStep(gPos, "▶", 470, 114, 40, delegate (PanelData p, float k) { p.Yaw += tilt * k; });
            AddStep(gPos, "▼", 428, 146, 40, delegate (PanelData p, float k) { p.Pitch += tilt * k; });

            _pose = AddLabel(gPos, "", 12, 196, 528);
            _pose.ForeColor = SystemColors.GrayText;

            // Appearance
            var gLook = Add(this, new GroupBox { Text = "Aparência e desempenho" }, 196, 374, 552, 130);
            AddLabel(gLook, "Opacidade", 12, 28, 70);
            _opacity = Add(gLook, new TrackBar { Minimum = 10, Maximum = 100, TickFrequency = 10 }, 82, 20, 170, 40);
            _opacityLabel = AddLabel(gLook, "", 254, 28, 45);
            _opacity.ValueChanged += delegate
            {
                _opacityLabel.Text = _opacity.Value + "%";
                Edit(delegate (PanelData p) { p.Opacity = _opacity.Value / 100f; });
            };
            AddLabel(gLook, "Atualizações/s", 312, 28, 95);
            _fps = Add(gLook, new NumericUpDown { Minimum = 5, Maximum = 90, Increment = 5 }, 410, 25, 60, 24);
            _fps.ValueChanged += delegate { Edit(delegate (PanelData p) { p.MaxFps = (int)_fps.Value; }); };
            var fpsHint = AddLabel(gLook, "menos = leve", 474, 28, 76);
            fpsHint.ForeColor = SystemColors.GrayText;

            _chroma = Add(gLook, new CheckBox { Text = "Remover cor de fundo" }, 12, 66, 150, 22);
            _chroma.CheckedChanged += delegate { Edit(delegate (PanelData p) { p.ChromaKey = _chroma.Checked; }); };
            _color = Add(gLook, new Button { FlatStyle = FlatStyle.Flat }, 166, 64, 40, 24);
            _color.Click += delegate { PickColor(); };
            AddLabel(gLook, "Tolerância", 222, 68, 70);
            _tolerance = Add(gLook, new TrackBar { Minimum = 1, Maximum = 60, TickFrequency = 5 }, 292, 60, 180, 40);
            _tolerance.ValueChanged += delegate { Edit(delegate (PanelData p) { p.KeyTolerance = _tolerance.Value / 100f; }); };
            _alpha = Add(gLook, new CheckBox { Text = "Usar transparência da própria janela" }, 12, 96, 260, 22);
            _alpha.CheckedChanged += delegate { Edit(delegate (PanelData p) { p.UseWindowAlpha = _alpha.Checked; }); };
            _panelControls.AddRange(new Control[] { _opacity, _fps, _chroma, _color, _tolerance, _alpha });

            // Footer
            _game = AddLabel(this, "", 12, 516, 736);
            _game.Height = 20;
        }

        // ---------------- Data <-> UI
        void RefreshPanelList(PanelData select)
        {
            _loading = true;
            _list.Items.Clear();
            foreach (var p in _cfg.Panels) _list.Items.Add(p);
            _loading = false;
            if (select != null) _list.SelectedItem = select;
            else if (_list.Items.Count > 0) _list.SelectedIndex = 0;
            ShowPanel();
        }

        void ShowPanel()
        {
            var p = Current;
            foreach (var c in _panelControls) c.Enabled = p != null;
            if (p == null) { _pose.Text = "Adicione um painel para começar."; return; }
            _loading = true;
            _title.Text = p.Window;
            _process.Text = p.Process;
            _enabled.Checked = p.Enabled;
            _opacity.Value = Clamp((int)Math.Round(p.Opacity * 100), 10, 100);
            _opacityLabel.Text = _opacity.Value + "%";
            _fps.Value = Clamp(p.MaxFps, 5, 90);
            _chroma.Checked = p.ChromaKey;
            _alpha.Checked = p.UseWindowAlpha;
            _tolerance.Value = Clamp((int)Math.Round(p.KeyTolerance * 100), 1, 60);
            _color.BackColor = ParseColor(p.KeyColor);
            _windows.SelectedIndex = -1;
            _loading = false;
            ShowPose(p);
            UpdateStatus();
        }

        void ShowPose(PanelData p)
        {
            _pose.Text = string.Format(CultureInfo.InvariantCulture,
                "X {0:0.00}  Y {1:0.00}  Distância {2:0.00} m   |   Largura {3:0.00} m   |   Inclinação {4:0}°  Giro {5:0}°",
                p.X, p.Y, -p.Z, p.Width, -p.Pitch, p.Yaw);
        }

        static int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

        // ---------------- Edits
        void Edit(Action<PanelData> change)
        {
            if (_loading) return;
            var p = Current;
            if (p == null) return;
            change(p);
            Commit(p);
        }

        void Commit(PanelData p)
        {
            ShowPose(p);
            Save();
            int i = _list.Items.IndexOf(p);
            if (i >= 0 && _list.Items[i].ToString() != p.ToString())
            {
                _loading = true;
                _list.Items[i] = p;  // refresh text
                _list.SelectedIndex = i;
                _loading = false;
            }
        }

        void Save()
        {
            try
            {
                _cfg.Save();
                _link.Poke();
            }
            catch (Exception ex)
            {
                _game.Text = "Erro ao salvar: " + ex.Message;
            }
        }

        void TargetEdited()
        {
            Edit(delegate (PanelData p) { p.Window = _title.Text.Trim(); p.Process = _process.Text.Trim(); });
        }

        void PickWindow()
        {
            var w = _windows.SelectedItem as WinItem;
            if (w == null) return;
            _title.Text = w.Title;
            _process.Text = w.Process == "?" ? "" : w.Process;
            TargetEdited();
        }

        void PickColor()
        {
            var p = Current;
            if (p == null) return;
            using (var dlg = new ColorDialog { Color = ParseColor(p.KeyColor), FullOpen = true })
            {
                if (dlg.ShowDialog(this) != DialogResult.OK) return;
                _color.BackColor = dlg.Color;
                Edit(delegate (PanelData d) { d.KeyColor = string.Format("#{0:X2}{1:X2}{2:X2}", dlg.Color.R, dlg.Color.G, dlg.Color.B); });
            }
        }

        static Color ParseColor(string hex)
        {
            try
            {
                int v = Convert.ToInt32(hex.TrimStart('#'), 16);
                return Color.FromArgb((v >> 16) & 255, (v >> 8) & 255, v & 255);
            }
            catch (Exception) { return Color.Black; }
        }

        void AddPanel()
        {
            var p = new PanelData { Section = _cfg.NewSectionName() };
            _cfg.Panels.Add(p);
            Save();
            RefreshPanelList(p);
            _windows.Focus();
            _windows.DroppedDown = true;
        }

        void RemovePanel()
        {
            var p = Current;
            if (p == null) return;
            if (MessageBox.Show(this, "Remover o painel \"" + p + "\"?", "XR Window Panels",
                                MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes) return;
            _cfg.Panels.Remove(p);
            Save();
            RefreshPanelList(null);
        }

        void RefreshWindows()
        {
            _windows.Items.Clear();
            foreach (var w in Windows.List()) _windows.Items.Add(w);
        }

        // ---------------- Status from the layer
        void UpdateStatus()
        {
            string app;
            bool connected = _link.Connected(out app);
            _game.Text = connected
                ? "● Conectado ao jogo: " + app + "   (alterações aparecem no VR na hora)"
                : "○ Nenhum jogo VR com a camada ativa no momento - as alterações ficam salvas";
            _game.ForeColor = connected ? Color.ForestGreen : SystemColors.GrayText;

            var p = Current;
            if (p == null) { _panelStatus.Text = ""; return; }
            int state, w, h, fps;
            if (!connected || !_link.PanelStatus(p.Section, out state, out w, out h, out fps))
            {
                _panelStatus.Text = "";
                return;
            }
            string[] names = { "desativado", "procurando a janela...", "iniciando captura...", "visível", "escondido (Ctrl+F12)" };
            string text = state >= 0 && state < names.Length ? names[state] : "?";
            if (state >= 3) text += string.Format("  -  {0}x{1}, {2} atualizações/s", w, h, fps);
            _panelStatus.Text = "No VR: " + text;
        }

        protected override void OnFormClosed(FormClosedEventArgs e)
        {
            _link.Dispose();
            base.OnFormClosed(e);
        }
    }

    static class Program
    {
        [STAThread]
        static void Main()
        {
            try { Native.SetProcessDPIAware(); } catch (Exception) { }
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new MainForm());
        }
    }
}
