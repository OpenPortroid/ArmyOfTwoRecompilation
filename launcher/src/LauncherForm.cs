using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Drawing.Text;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace AO2Launcher
{
    enum RowKind
    {
        Choice,
        Slider,
        Text,
        Button,
        Info,
    }

    sealed class Row
    {
        public RowKind Kind;
        public string Label;
        public string Desc;
        public string Key;
        // Choice
        public string[] Values;
        public string[] Names;
        public Func<int, string> Name; // instead of Names (text that is not a fixed string)
        // Slider
        public double Min, Max, Step;
        public Func<double, string> Format;
        // Text
        public int MaxLen = 200;
        public Func<string> Placeholder;
        public TextBox Box;
        // Button
        public Func<string> ButtonText;
        public Action Click;
        // Info
        public Func<string> Text;

        public Func<bool> Visible = () => true;
        public Func<bool> Enabled = () => true;

        // Layout (logical pixels), set by LauncherForm.LayoutRows.
        public float Y, H;

        public string Caption
        {
            get { return L.T(Label); }
        }

        // The name of choice i, translated.
        public string ChoiceName(int i)
        {
            return Name != null ? Name(i) : L.T(Names[i]);
        }

        public bool Selectable
        {
            get { return Kind != RowKind.Info; }
        }
    }

    sealed class Hot
    {
        public RectangleF R;
        public string Id;
        public Action Click;
        public Action<float> Drag;
        public int Row = -1;
    }

    enum UiScreen
    {
        Main,
        Settings,
        Install,
    }

    sealed partial class LauncherForm : Form
    {
        // Logical canvas; everything is drawn in these units and scaled by DPI.
        const float W = 1200, H = 660;
        // Settings screen: centred column.
        const float ColX = 280, ColW = 640, RowsTop = 132, DescTop = 500;
        const float RowH = 34;
        const float ValueW = 186;

        // Light text on the cover's deep navy (Skin.cs); the accent is its paper grey.
        static readonly Color Amber = Color.FromArgb(224, 220, 212);
        static readonly Color TextLight = Color.FromArgb(206, 212, 222);
        static readonly Color TextDim = Color.FromArgb(140, 152, 170);
        static readonly Color TextOff = Color.FromArgb(86, 98, 116);
        static readonly Color TextBoxBack = Color.FromArgb(14, 30, 56);

        readonly Settings S = new Settings();
        readonly Gamepad pad = new Gamepad();
        readonly Timer timer = new Timer();

        float scale = 1;
        Bitmap bgMainSrc, bgDimSrc, logo;
        Bitmap bgMain, bgDim;
        FontFamily famCond, famReg;
        Font fLabel, fValue, fDesc, fSmall, fHint, fPlay, fInstall, fBtn, fTab;
        Font fTitle, fTitleSub, fBack, fSection, fLcd, fTiny;
        bool padConnected;

        string[] tabNames = { "GRAPHICS", "CONTROLS", "NETWORK", "ADVANCED" };
        List<Row>[] tabs;
        UiScreen screen = UiScreen.Main;
        int mainSel; // 0 = play, 1 = settings
        int tab;
        int sel;
        bool searching;
        bool snapshotting;

        readonly List<Hot> hots = new List<Hot>();
        Hot hover, dragging;
        string pressed; // the hot region under a held mouse button, drawn pressed
        bool focusHidden; // the mouse is off the items (or out of the window): the current item is not drawn lit
        Point lastMouse = new Point(-1, -1);
        public bool padInput; // the controller was used last: hints show its buttons, not keys
        string toast;
        bool toastBad;
        DateTime toastUntil;
        string gameExe;
        Updates.Release latest; // the latest release on GitHub; null until known (or offline)
        bool updatePrompt;      // the "update available" window is open
        int promptSel;          // in it: 0 = download, 1 = later

        public LauncherForm()
        {
            Text = "Army of Two Launcher";
            FormBorderStyle = FormBorderStyle.None;
            StartPosition = FormStartPosition.Manual;
            KeyPreview = true;
            DoubleBuffered = true;
            SetStyle(ControlStyles.OptimizedDoubleBuffer | ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint, true);
            BackColor = Color.FromArgb(14, 15, 17);

            using (var g = CreateGraphics())
            {
                scale = g.DpiX / 96f;
            }
            Rectangle wa = Screen.PrimaryScreen.WorkingArea;
            scale = Math.Min(scale, Math.Min(wa.Width * 0.96f / W, wa.Height * 0.96f / H));
            ClientSize = new Size((int)Math.Round(W * scale), (int)Math.Round(H * scale));
            Location = new Point(wa.X + (wa.Width - Width) / 2, wa.Y + (wa.Height - Height) / 2);

            L.Set(S["language"]);
            LoadResources();
            BuildRows();
            tab = Math.Max(0, Math.Min(tabs.Length - 1, S.Int("tab")));
            sel = FirstSelectable();
            gameExe = Game.Find(S);
            // The launcher always opens on the main screen; without the game files its button reads
            // INSTALL and leads to the installation.
            InitInstall();

            pad.Pressed += OnPad;
            timer.Interval = 16;
            timer.Tick += (s, e) =>
            {
                pad.Poll(Form.ActiveForm == this);
                if (pad.Connected != padConnected || installing)
                {
                    // Installing: the progress bar's stripes move.
                    padConnected = pad.Connected;
                    if (!padConnected)
                    {
                        padInput = false;
                    }
                    Invalidate();
                }
                if (toast != null && DateTime.UtcNow > toastUntil)
                {
                    toast = null;
                    Invalidate();
                }
            };
            timer.Start();
            LayoutRows();
            CheckForUpdate();
        }

        // Asks GitHub for the latest release off the UI thread; a newer one opens the update window.
        void CheckForUpdate()
        {
            if (Updates.Override != null)
            {
                OnLatest(Updates.Latest());
                return;
            }
            var th = new System.Threading.Thread(() =>
            {
                Updates.Release r = Updates.Latest();
                try
                {
                    BeginInvoke(new Action(() => OnLatest(r)));
                }
                catch (InvalidOperationException)
                {
                    // The form was closed meanwhile.
                }
            });
            th.IsBackground = true;
            th.Start();
        }

        void OnLatest(Updates.Release r)
        {
            if (IsDisposed || Disposing || r == null)
            {
                return;
            }
            latest = r;
            if (UpdateAvailable)
            {
                updatePrompt = true;
                promptSel = 0;
            }
            Invalidate();
        }

        bool UpdateAvailable
        {
            get { return latest != null && Updates.Newer(latest.Version, Updates.Current); }
        }

        // The update window is shown over the main screen only, and not while the game is being installed.
        bool PromptShown
        {
            get { return updatePrompt && screen == UiScreen.Main && !installing; }
        }

        void DownloadUpdate()
        {
            updatePrompt = false;
            try
            {
                System.Diagnostics.Process.Start(latest.Url);
            }
            catch (Exception ex)
            {
                ShowToast(L.F("Could not open the browser: {0}", ex.Message), true);
            }
            Invalidate();
        }

        void ClosePrompt()
        {
            updatePrompt = false;
            Invalidate();
        }

        void PromptActivate()
        {
            if (promptSel == 0)
            {
                DownloadUpdate();
            }
            else
            {
                ClosePrompt();
            }
        }

        public void Snapshot(string path, int t, int s, UiScreen scr)
        {
            IntPtr h = Handle;
            snapshotting = true;
            tab = Math.Max(0, Math.Min(tabs.Length - 1, t));
            screen = scr;
            sel = s;
            instSel = s;
            LayoutRows();
            using (var bmp = new Bitmap(ClientSize.Width, ClientSize.Height))
            {
                DrawToBitmap(bmp, new Rectangle(Point.Empty, ClientSize));
                bmp.Save(path, System.Drawing.Imaging.ImageFormat.Png);
            }
        }

        // Average time of one full repaint of each screen, in ms (debug: --bench=<file>).
        public void Bench(string path)
        {
            IntPtr h = Handle;
            snapshotting = true;
            var lines = new List<string>();
            using (var bmp = new Bitmap(ClientSize.Width, ClientSize.Height))
            {
                foreach (UiScreen scr in new[] { UiScreen.Main, UiScreen.Settings, UiScreen.Install })
                {
                    screen = scr;
                    tab = 0;
                    LayoutRows();
                    var clip = new Rectangle(Point.Empty, ClientSize);
                    using (Graphics g = Graphics.FromImage(bmp))
                    {
                        OnPaint(new PaintEventArgs(g, clip)); // warm-up: backdrops, caches
                    }
                    const int n = 40;
                    var sw = System.Diagnostics.Stopwatch.StartNew();
                    for (int i = 0; i < n; i++)
                    {
                        using (Graphics g = Graphics.FromImage(bmp))
                        {
                            OnPaint(new PaintEventArgs(g, clip));
                        }
                    }
                    lines.Add(string.Format(CultureInfo.InvariantCulture, "{0}: {1:0.00} ms", scr, sw.Elapsed.TotalMilliseconds / n));
                }
            }
            File.WriteAllLines(path, lines);
        }

        protected override CreateParams CreateParams
        {
            get
            {
                CreateParams cp = base.CreateParams;
                cp.ClassStyle |= 0x20000; // CS_DROPSHADOW
                cp.Style |= 0x20000;      // WS_MINIMIZEBOX: minimize/restore from the taskbar
                return cp;
            }
        }

        protected override void OnShown(EventArgs e)
        {
            base.OnShown(e);
            ActiveControl = null;
        }

        protected override void OnFormClosing(FormClosingEventArgs e)
        {
            cancelInstall = true;
            S["tab"] = tab.ToString(CultureInfo.InvariantCulture);
            S.Save();
            base.OnFormClosing(e);
        }

        // ------------------------------------------------------------------
        // Resources

        static Bitmap Res(string name)
        {
            Stream st = Assembly.GetExecutingAssembly().GetManifestResourceStream("AO2Launcher." + name);
            return st == null ? null : new Bitmap(st);
        }

        static FontFamily Family(params string[] names)
        {
            foreach (string n in names)
            {
                try
                {
                    return new FontFamily(n);
                }
                catch (ArgumentException)
                {
                }
            }
            return FontFamily.GenericSansSerif;
        }

        void LoadResources()
        {
            bgMainSrc = Res("bg.jpg");
            bgDimSrc = Res("bg_dim.jpg");
            logo = Res("logo.png");
            Stream ico = Assembly.GetExecutingAssembly().GetManifestResourceStream("AO2Launcher.app.ico");
            if (ico != null)
            {
                Icon = new Icon(ico);
            }

            // Plain Segoe UI; captions in letter-spaced semibold capitals, echoing the logo.
            famCaps = Family("Segoe UI Semibold", "Segoe UI", "Arial");
            famCond = famCaps;
            famReg = Family("Segoe UI", "Arial");
            famLcd = famReg;
            fLabel = new Font(famReg, 14.5f, FontStyle.Regular, GraphicsUnit.Pixel);
            fValue = new Font(famCaps, 14f, FontStyle.Regular, GraphicsUnit.Pixel);
            fDesc = new Font(famReg, 13f, FontStyle.Regular, GraphicsUnit.Pixel);
            fSmall = new Font(famReg, 12.5f, FontStyle.Regular, GraphicsUnit.Pixel);
            fHint = new Font(famReg, 13f, FontStyle.Regular, GraphicsUnit.Pixel);
            fPlay = new Font(famCaps, 17f, FontStyle.Regular, GraphicsUnit.Pixel);
            fInstall = new Font(famCaps, 15f, FontStyle.Regular, GraphicsUnit.Pixel);
            fBtn = new Font(famCaps, 12.5f, FontStyle.Regular, GraphicsUnit.Pixel);
            fTab = new Font(famCaps, 12f, FontStyle.Regular, GraphicsUnit.Pixel);
            fTitle = new Font(famCaps, 13f, FontStyle.Regular, GraphicsUnit.Pixel);
            fTitleSub = new Font(famReg, 13f, FontStyle.Regular, GraphicsUnit.Pixel);
            fBack = new Font(famCaps, 11f, FontStyle.Regular, GraphicsUnit.Pixel);
            fSection = new Font(famCaps, 11f, FontStyle.Regular, GraphicsUnit.Pixel);
            fLcd = new Font(famReg, 12.5f, FontStyle.Regular, GraphicsUnit.Pixel);
            fTiny = new Font(famReg, 12f, FontStyle.Regular, GraphicsUnit.Pixel);
            LoadSkin();
        }

        // ------------------------------------------------------------------
        // Rows

        static Row Toggle(string label, string key, string desc, string off = "OFF", string on = "ON")
        {
            return new Row { Kind = RowKind.Choice, Label = label, Key = key, Desc = desc, Values = new[] { "0", "1" }, Names = new[] { off, on } };
        }

        void BuildRows()
        {
            // Graphics
            var sizes = new List<string> { "1280x720", "1366x768", "1600x900", "1920x1080", "2560x1440", "3840x2160" };
            int maxW = Screen.AllScreens.Max(sc => sc.Bounds.Width);
            int maxH = Screen.AllScreens.Max(sc => sc.Bounds.Height);
            sizes = sizes.Where(sz =>
            {
                int w, h;
                Game.ParseSize(sz, out w, out h);
                return w <= maxW && h <= maxH;
            }).ToList();
            if (sizes.Count == 0)
            {
                sizes.Add("1280x720");
            }
            if (!sizes.Contains(S["window_size"]))
            {
                S["window_size"] = sizes.Contains("1920x1080") ? "1920x1080" : sizes[sizes.Count - 1];
            }

            var monValues = new List<string> { "0" };
            Screen[] screens = Screen.AllScreens;
            if (screens.Length > 1)
            {
                for (int i = 0; i < screens.Length; i++)
                {
                    monValues.Add((i + 1).ToString(CultureInfo.InvariantCulture));
                }
            }
            Func<int, string> monName = i => i == 0 ? L.T("PRIMARY")
                : string.Format("{0}: {1}×{2}{3}", i, screens[i - 1].Bounds.Width, screens[i - 1].Bounds.Height,
                    screens[i - 1].Primary ? L.T(" (primary)") : "");
            if (!monValues.Contains(S["monitor"]))
            {
                S["monitor"] = "0";
            }

            var graphics = new List<Row>
            {
                new Row
                {
                    Kind = RowKind.Choice, Label = "Language", Key = "language",
                    Values = new[] { "auto", "en", "ru", "fr", "de", "es", "it" },
                    Names = new[] { "AUTO", "ENGLISH", "РУССКИЙ", "FRANÇAIS", "DEUTSCH", "ESPAÑOL", "ITALIANO" },
                    Desc = "Language of the game (text and voices) and of this launcher. Auto follows the language of Windows. " +
                           "Russian is text only, with English voices. In co-op both players need the same voices: " +
                           "English and Russian play together, the other languages only with the same language.",
                },
                new Row
                {
                    Kind = RowKind.Choice, Label = "Display mode", Key = "fullscreen",
                    Values = new[] { "1", "0" }, Names = new[] { "FULLSCREEN", "WINDOWED" },
                    Desc = "Fullscreen is a borderless window covering the monitor; the desktop mode is not switched. " +
                           "A 16:9 picture is letterboxed with black bars."
                },
                new Row
                {
                    Kind = RowKind.Choice, Label = "Window size", Key = "window_size",
                    Values = sizes.ToArray(), Names = sizes.Select(sz => sz.Replace("x", "×")).ToArray(),
                    Enabled = () => !S.Bool("fullscreen"),
                    Desc = "Window size in screen pixels (windowed mode only). " +
                           "The game shrinks a window that does not fit the monitor's work area."
                },
                new Row
                {
                    Kind = RowKind.Choice, Label = "Monitor", Key = "monitor",
                    Values = monValues.ToArray(), Name = monName,
                    Enabled = () => monValues.Count > 1,
                    Desc = "Which monitor the game opens on."
                },
                new Row
                {
                    Kind = RowKind.Choice, Label = "Aspect ratio", Key = "aspect_ratio",
                    Values = new[] { "auto", "5:4", "4:3", "3:2", "16:10", "16:9", "2:1", "21:9", "32:9" },
                    Names = new[] { "AUTO", "5:4", "4:3", "3:2", "16:10", "16:9", "2:1", "21:9", "32:9" },
                    Desc = "Shape of the picture. Auto follows the window (the monitor in fullscreen), " +
                           "with no black bars. 16:9 is what the console had."
                },
                new Row
                {
                    Kind = RowKind.Choice, Label = "Render resolution", Key = "render_scale",
                    Values = new[] { "1", "1.25", "1.5", "2", "2.5", "3", "4" },
                    Names = new[] { "1280×720  (1×)", "1600×900  (1.25×)", "1920×1080  (1.5×)", "2560×1440  (2×)",
                                    "3200×1800  (2.5×)", "3840×2160  (3×)", "5120×2880  (4×)" },
                    Desc = "Internal resolution the game is rendered at (the console original is 720p). " +
                           "1.5× is exactly Full HD, pixel for pixel on a 1920×1080 monitor; 2× is sharper and smoother but heavier. " +
                           "Use 1× on a weak graphics card; 3× and 4× are for powerful ones."
                },
                new Row
                {
                    Kind = RowKind.Choice, Label = "FPS limit", Key = "fps",
                    Values = new[] { "30", "60", "75", "90", "120", "144", "165", "240" },
                    Names = new[] { "30", "60", "75", "90", "120", "144", "165", "240" },
                    Desc = "Maximum frame rate; the speed of the game itself does not depend on it. " +
                           "60 is the most tested option; above ~150 FPS the processor is usually the limit."
                },
                new Row
                {
                    Kind = RowKind.Choice, Label = "FPS counter", Key = "fps_overlay",
                    Values = new[] { "0", "1", "2" }, Names = new[] { "OFF", "FPS", "DETAILED" },
                    Desc = "Frame rate in the corner of the screen. Detailed adds frame time, graphics card load and temperature, " +
                           "processor load, video and system memory. F2 switches it in the game."
                },
                Toggle("Vertical sync", "vsync",
                    "Frames wait for the monitor's refresh: no tearing, but the frame rate stays at or below the refresh rate " +
                    "and the input lag grows a little."),
                Toggle("Anisotropic filtering", "aniso", "Sharper textures at an angle (16×). Costs almost no performance.",
                    "OFF", "16×"),
            };

            // Controls
            Func<double, string> times = v => v.ToString("0.00", CultureInfo.InvariantCulture);
            var controls = new List<Row>
            {
                Toggle("Mouse aiming", "mouse_aim",
                    "The mouse turns the camera directly, like in PC shooters. When off, the mouse acts as the gamepad's right stick."),
                new Row
                {
                    Kind = RowKind.Slider, Label = "Mouse sensitivity", Key = "mouse_sens", Min = 0.25, Max = 8, Step = 0.25,
                    Format = times, Enabled = () => S.Bool("mouse_aim"),
                    Desc = "How fast the mouse turns the camera. 1.0 is 0.022° per mouse count (as in Source/Quake); the default is 2.0."
                },
                new Row
                {
                    Kind = RowKind.Slider, Label = "Sensitivity when aiming", Key = "mouse_zoom", Min = 0.25, Max = 2, Step = 0.05,
                    Format = times, Enabled = () => S.Bool("mouse_aim"),
                    Desc = "Multiplier while aiming (right button, scopes). Turning already accounts for zoom; 1.0 feels the same as unaimed."
                },
                Toggle("Invert Y axis", "invert_y", "Mouse up looks down (on top of the inversion setting in the game itself)."),
                new Row
                {
                    Kind = RowKind.Slider, Label = "Mouse in vehicles and turrets", Key = "stick_sens", Min = 0.25, Max = 3, Step = 0.05,
                    Format = times,
                    Desc = "Where the camera cannot be turned directly (turrets, vehicles, menus) the mouse acts as the right stick; this is its sensitivity."
                },
                new Row
                {
                    Kind = RowKind.Info,
                    Text = () => L.T("WASD - move   mouse - camera   LMB - fire   RMB - aim   R - reload   " +
                                     "Space - roll / cover   Ctrl, C - crouch   E, Enter - use   F - co-op commands   " +
                                     "Z - inventory   1-4 - weapons and grenades   arrows - partner orders   Tab - GPS   Esc - pause.\n" +
                                     "Xbox gamepad: as on the console. Key bindings: settings\\mapping.toml in the game folder.")
                },
            };

            // Network
            var online = new List<Row>
            {
                new Row
                {
                    Kind = RowKind.Text, Label = "Player name", Key = "gamertag", MaxLen = 15,
                    Placeholder = () => Shorten(Environment.MachineName, 15),
                    Desc = "The name your partner sees (up to 15 characters). Empty: the computer's name."
                },
                new Row
                {
                    Kind = RowKind.Text, Label = "Host IP", Key = "host_ip", MaxLen = 64,
                    Placeholder = () => L.T("auto"),
                    Desc = "Leave empty: the host is found automatically on the local network or Radmin VPN. " +
                           "Enter an address only if the host is not found, for example over the internet."
                },
                new Row
                {
                    Kind = RowKind.Slider, Label = "Input delay margin", Key = "input_margin", Min = 0, Max = 100, Step = 4,
                    Format = v => ((int)v).ToString(CultureInfo.InvariantCulture) + L.T(" ms"),
                    Desc = "Added to the network delay. 0 is the minimum. If the game stutters waiting for the partner on an unstable " +
                           "connection, set 16-50. Must be the same for both players."
                },
                new Row
                {
                    Kind = RowKind.Button, Label = "Windows Firewall", ButtonText = () => "ALLOW",
                    Click = Firewall,
                    Desc = "Allow incoming connections to the game on every network type (Windows treats Radmin as public). " +
                           "Needs administrator rights: Windows will ask to confirm. Done once on both PCs."
                },
                Toggle("Co-op log", "coop_debug",
                    "Writes coop_debug.log in the game folder, needed to investigate desyncs. Costs a little performance."),
                new Row
                {
                    Kind = RowKind.Info,
                    Text = () => L.T("The role is chosen automatically: on Play the launcher looks for a host on the local network. If one is " +
                                     "found you join it, otherwise you become the host. The second player presses Play a few seconds after " +
                                     "the first player's game has started. Then both, in the game: Campaign Mode → Public Co-op Game.")
                },
            };

            // Extras
            var extras = new List<Row>
            {
                Toggle("Skip cutscenes", "skip_cutscenes",
                    "Enter or Space skips movies and in-engine cutscenes. Skipping is off in network games."),
                Toggle("Skip unskippable ones", "skip_unskippable",
                    "Also allow skipping the cutscenes the original game does not let you skip.",
                    "NO", "YES"),
                Toggle("God mode (cheat)", "god_mode", "The player takes no damage. Has no effect in online co-op."),
                Toggle("Close the launcher", "close_on_launch", "Close this window once the game has started.", "NO", "YES"),
                new Row
                {
                    Kind = RowKind.Text, Label = "Extra arguments", Key = "extra_args", MaxLen = 1000,
                    Placeholder = () => "--flag=value ...",
                    Desc = "Extra command line arguments for the game, appended at the end (for advanced users)."
                },
                new Row
                {
                    Kind = RowKind.Button, Label = "Game folder", ButtonText = () => "OPEN",
                    Click = () =>
                    {
                        if (gameExe != null)
                        {
                            Game.OpenFolder(Path.GetDirectoryName(gameExe));
                        }
                        else
                        {
                            ShowToast(L.T("Game not found"), true);
                        }
                    },
                    Desc = "Open the game folder in Explorer (logs, settings, shader cache)."
                },
                new Row
                {
                    Kind = RowKind.Button, Label = "Saves", ButtonText = () => "OPEN",
                    Click = () =>
                    {
                        string dir = Game.SavesFolder();
                        if (Directory.Exists(dir))
                        {
                            Game.OpenFolder(dir);
                        }
                        else
                        {
                            ShowToast(L.F("No saves yet: {0}", dir), true);
                        }
                    },
                    Desc = "Saves and profile folder: Documents\\armyoftworecomp_tu2 (or armyoftworecomp)."
                },
                new Row
                {
                    Kind = RowKind.Button, Label = "Game files and DLC", ButtonText = () => "INSTALL",
                    Click = () => SetScreen(UiScreen.Install),
                    Desc = "Install the game from your own disc image, Title Update 2, or add downloadable content (DLC)."
                },
                new Row
                {
                    Kind = RowKind.Button, Label = "Desktop shortcut", ButtonText = () => "CREATE",
                    Click = () =>
                    {
                        bool ok = Installer.CreateDesktopShortcut();
                        ShowToast(L.T(ok ? "Shortcut created on the desktop" : "Could not create the shortcut"), !ok);
                    },
                    Desc = "Create an \"Army of Two\" shortcut on the desktop that starts this launcher."
                },
                new Row
                {
                    Kind = RowKind.Button, Label = "Game path", ButtonText = () => "CHANGE",
                    Click = () => BrowseGame(),
                    Desc = "Point to armyoftworecomp.exe by hand if the launcher is not in the game folder."
                },
                new Row
                {
                    Kind = RowKind.Button, Label = "Launcher settings", ButtonText = () => "RESET",
                    Click = () =>
                    {
                        S.Reset();
                        S.Save();
                        LanguageChanged();
                        foreach (Row r in AllRows().Where(r => r.Box != null))
                        {
                            r.Box.Text = S[r.Key];
                        }
                        LayoutRows();
                        ShowToast(L.T("Settings reset to defaults"), false);
                    },
                    Desc = "Return every launcher setting to its default. Game files are not changed."
                },
            };

            tabs = new[] { graphics, controls, online, extras };

            foreach (Row r in AllRows().Where(r => r.Kind == RowKind.Text))
            {
                Row row = r;
                var tb = new TextBox
                {
                    BorderStyle = BorderStyle.None,
                    BackColor = TextBoxBack,
                    ForeColor = TextLight,
                    Font = new Font(famReg, 15f * scale, FontStyle.Regular, GraphicsUnit.Pixel),
                    MaxLength = row.MaxLen,
                    Text = S[row.Key],
                    Visible = false,
                };
                tb.TextChanged += (s, e) => { S[row.Key] = tb.Text; };
                tb.Leave += (s, e) =>
                {
                    S.Save();
                    Invalidate();
                };
                tb.GotFocus += (s, e) =>
                {
                    int i = VisibleRows().IndexOf(row);
                    if (i >= 0)
                    {
                        sel = i;
                    }
                    Invalidate();
                };
                tb.HandleCreated += (s, e) => SetCueBanner(row);
                row.Box = tb;
                Controls.Add(tb);
            }
        }

        // The grey hint text of an empty text box.
        void SetCueBanner(Row row)
        {
            if (row.Placeholder != null && row.Box != null && row.Box.IsHandleCreated)
            {
                SendMessage(row.Box.Handle, 0x1501 /* EM_SETCUEBANNER */, (IntPtr)1, row.Placeholder());
            }
        }

        // The launcher's text follows the language setting at once.
        void LanguageChanged()
        {
            L.Set(S["language"]);
            foreach (Row r in AllRows().Where(r => r.Box != null))
            {
                SetCueBanner(r);
            }
        }

        static string Shorten(string s, int n)
        {
            return s.Length <= n ? s : s.Substring(0, n - 1) + "…";
        }

        IEnumerable<Row> AllRows()
        {
            return tabs.SelectMany(t => t);
        }

        List<Row> VisibleRows()
        {
            return tabs[tab].Where(r => r.Visible()).ToList();
        }

        int FirstSelectable()
        {
            List<Row> rows = VisibleRows();
            for (int i = 0; i < rows.Count; i++)
            {
                if (rows[i].Selectable && rows[i].Enabled())
                {
                    return i;
                }
            }
            return 0;
        }

        Rectangle ToDevice(RectangleF r)
        {
            return Rectangle.Round(new RectangleF(r.X * scale, r.Y * scale, r.Width * scale, r.Height * scale));
        }

        float ValueX
        {
            get { return ColX + ColW - 18 - ValueW; }
        }

        // Row positions for the current tab and the text boxes over them.
        void LayoutRows()
        {
            float y = RowsTop;
            using (Graphics g = CreateGraphics())
            {
                g.ScaleTransform(scale, scale);
                foreach (Row r in VisibleRows())
                {
                    r.Y = y;
                    if (r.Kind == RowKind.Info)
                    {
                        SizeF sz = g.MeasureString(r.Text(), fSmall, (int)(ColW - 36));
                        r.H = Math.Min(sz.Height + 16, Math.Max(0, DescTop - 6 - y));
                    }
                    else
                    {
                        r.H = RowH;
                    }
                    y += r.H;
                }
            }
            var visible = new HashSet<Row>(VisibleRows());
            foreach (Row r in AllRows().Where(r => r.Box != null))
            {
                bool show = screen == UiScreen.Settings && visible.Contains(r);
                if (show)
                {
                    float boxH = 22;
                    r.Box.Bounds = ToDevice(new RectangleF(ValueX + 6, r.Y + (RowH - boxH) / 2 + 1, ValueW - 12, boxH));
                    r.Box.Enabled = r.Enabled();
                }
                if (r.Box.Visible != show)
                {
                    if (!show && r.Box.Focused)
                    {
                        ActiveControl = null;
                    }
                    r.Box.Visible = show;
                }
            }
            List<Row> rows = VisibleRows();
            if (sel >= rows.Count || !rows[sel].Selectable)
            {
                sel = FirstSelectable();
            }
            Invalidate();
        }

        // ------------------------------------------------------------------
        // Actions

        void SetScreen(UiScreen s)
        {
            ActiveControl = null;
            screen = s;
            hover = null;
            if (s == UiScreen.Settings)
            {
                sel = FirstSelectable();
            }
            else if (s == UiScreen.Install)
            {
                RefreshInstalled();
                instError = null;
                instSel = !gameOk && srcGame == null ? 0 : !updateOk && srcUpdate == null ? 1 : 2;
            }
            else
            {
                mainSel = 0;
            }
            LayoutRows();
        }

        void SetTab(int t)
        {
            t = (t + tabs.Length) % tabs.Length;
            if (t == tab)
            {
                return;
            }
            ActiveControl = null;
            tab = t;
            sel = FirstSelectable();
            LayoutRows();
        }

        void MoveSel(int dir)
        {
            List<Row> rows = VisibleRows();
            int i = sel;
            for (int n = 0; n < rows.Count; n++)
            {
                i = (i + dir + rows.Count) % rows.Count;
                if (rows[i].Selectable && rows[i].Enabled())
                {
                    sel = i;
                    break;
                }
            }
            ActiveControl = null;
            Invalidate();
        }

        void Change(Row r, int dir)
        {
            if (r == null || !r.Enabled())
            {
                return;
            }
            switch (r.Kind)
            {
                case RowKind.Choice:
                {
                    int i = Array.IndexOf(r.Values, S[r.Key]);
                    i = i < 0 ? 0 : (i + (dir == 0 ? 1 : dir) + r.Values.Length) % r.Values.Length;
                    S[r.Key] = r.Values[i];
                    if (r.Key == "language")
                    {
                        LanguageChanged();
                    }
                    break;
                }
                case RowKind.Slider:
                {
                    if (dir == 0)
                    {
                        return;
                    }
                    SetSlider(r, S.Double(r.Key) + dir * r.Step);
                    break;
                }
                case RowKind.Button:
                    if (dir == 0)
                    {
                        r.Click();
                    }
                    return;
                case RowKind.Text:
                    if (dir == 0 && r.Box != null)
                    {
                        r.Box.Focus();
                        r.Box.SelectAll();
                    }
                    return;
                default:
                    return;
            }
            S.Save();
            LayoutRows();
        }

        void SetSlider(Row r, double v)
        {
            v = Math.Round(v / r.Step) * r.Step;
            v = Math.Max(r.Min, Math.Min(r.Max, v));
            S[r.Key] = v.ToString("0.###", CultureInfo.InvariantCulture);
            Invalidate();
        }

        void ShowToast(string text, bool bad)
        {
            toast = text;
            toastBad = bad;
            toastUntil = DateTime.UtcNow.AddSeconds(bad ? 6 : 3.5);
            Invalidate();
        }

        bool BrowseGame()
        {
            using (var dlg = new OpenFileDialog())
            {
                dlg.Title = L.T("Where is armyoftworecomp.exe?");
                dlg.Filter = "Army of Two (armyoftworecomp.exe)|armyoftworecomp.exe|" + L.T("Programs") + " (*.exe)|*.exe";
                if (gameExe != null)
                {
                    dlg.InitialDirectory = Path.GetDirectoryName(gameExe);
                }
                if (dlg.ShowDialog(this) != DialogResult.OK)
                {
                    return false;
                }
                gameExe = dlg.FileName;
                S["game_path"] = gameExe;
                S.Save();
                RefreshInstalled();
                Invalidate();
                return true;
            }
        }

        void Firewall()
        {
            if (gameExe == null)
            {
                ShowToast(L.T("Set the game path first"), true);
                return;
            }
            if (Game.AllowThroughFirewall(gameExe))
            {
                ShowToast(L.T("Done: incoming connections to the game are allowed"), false);
            }
            else
            {
                ShowToast(L.T("Failed (the administrator prompt has to be confirmed)"), true);
            }
        }

        // Play: look for a host on the network (off the UI thread), then start
        // the game as a partner if one answered, otherwise as the host.
        void Play()
        {
            if (searching)
            {
                return;
            }
            if (gameExe == null || !File.Exists(gameExe))
            {
                gameExe = Game.Find(S);
                if (gameExe == null && !BrowseGame())
                {
                    return;
                }
            }
            RefreshInstalled();
            if (!Installed)
            {
                SetScreen(UiScreen.Install);
                return;
            }
            if (Game.IsRunning())
            {
                ShowToast(L.T("The game is already running"), true);
                return;
            }
            searching = true;
            Invalidate();
            string manual = S["host_ip"].Trim();
            var th = new System.Threading.Thread(() =>
            {
                string host = null;
                try
                {
                    host = Discovery.FindHost(manual, 1200);
                }
                catch
                {
                }
                try
                {
                    BeginInvoke(new Action(() => StartGame(host)));
                }
                catch (InvalidOperationException)
                {
                    // The form was closed meanwhile.
                }
            });
            th.IsBackground = true;
            th.Start();
        }

        void StartGame(string host)
        {
            if (IsDisposed || Disposing)
            {
                return;
            }
            searching = false;
            S["tab"] = tab.ToString(CultureInfo.InvariantCulture);
            S.Save();
            try
            {
                Game.Start(gameExe, S, host);
            }
            catch (Exception ex)
            {
                ShowToast(L.F("Could not start the game: {0}", ex.Message), true);
                return;
            }
            ShowToast(host != null ? L.F("Host found: {0} - joining", host) : L.T("No host found - hosting a game"), false);
            if (S.Bool("close_on_launch"))
            {
                Close();
            }
        }

        // ------------------------------------------------------------------
        // Input

        void ActivateMain(int i)
        {
            if (i == 0)
            {
                Play();
            }
            else if (i == 1)
            {
                SetScreen(UiScreen.Settings);
            }
            else
            {
                Close();
            }
        }

        // The main screen's buttons left to right: SETTINGS, PLAY, EXIT (mainSel 1, 0, 2).
        static readonly int[] MainOrder = { 1, 0, 2 };

        void MainMove(int dir)
        {
            int i = Array.IndexOf(MainOrder, mainSel);
            mainSel = MainOrder[(i + dir + MainOrder.Length) % MainOrder.Length];
            Invalidate();
        }

        void UsePad(bool pad)
        {
            if (padInput != pad)
            {
                padInput = pad;
                Invalidate();
            }
        }

        void OnPad(int b)
        {
            UsePad(true);
            if (PromptShown)
            {
                switch (b)
                {
                    case Gamepad.DpadLeft:
                    case Gamepad.DpadRight:
                        promptSel = 1 - promptSel;
                        Invalidate();
                        break;
                    case Gamepad.A:
                    case Gamepad.Start:
                        PromptActivate();
                        break;
                    case Gamepad.B:
                        ClosePrompt();
                        break;
                }
                return;
            }
            if (RevealFocus(b == Gamepad.A || b == Gamepad.Start))
            {
                return;
            }
            if (screen == UiScreen.Main)
            {
                switch (b)
                {
                    case Gamepad.DpadUp:
                    case Gamepad.DpadLeft:
                        MainMove(-1);
                        break;
                    case Gamepad.DpadDown:
                    case Gamepad.DpadRight:
                        MainMove(1);
                        break;
                    case Gamepad.A:
                    case Gamepad.Start:
                        ActivateMain(mainSel);
                        break;
                    case Gamepad.B:
                        Close();
                        break;
                }
                return;
            }
            if (screen == UiScreen.Install)
            {
                InstallPad(b);
                return;
            }
            Row cur = SelectedRow();
            switch (b)
            {
                case Gamepad.DpadUp: MoveSel(-1); break;
                case Gamepad.DpadDown: MoveSel(1); break;
                case Gamepad.DpadLeft: Change(cur, -1); break;
                case Gamepad.DpadRight: Change(cur, 1); break;
                case Gamepad.LB: SetTab(tab - 1); break;
                case Gamepad.RB: SetTab(tab + 1); break;
                case Gamepad.B: SetScreen(UiScreen.Main); break;
                case Gamepad.A: Change(cur, 0); break;
            }
        }

        Row SelectedRow()
        {
            List<Row> rows = VisibleRows();
            return sel >= 0 && sel < rows.Count ? rows[sel] : null;
        }

        protected override bool ProcessCmdKey(ref Message msg, Keys keyData)
        {
            UsePad(false);
            if (PromptShown)
            {
                switch (keyData)
                {
                    case Keys.Left:
                    case Keys.Right:
                    case Keys.Tab:
                    case Keys.Shift | Keys.Tab:
                        promptSel = 1 - promptSel;
                        Invalidate();
                        return true;
                    case Keys.Enter:
                    case Keys.Space:
                        PromptActivate();
                        return true;
                    case Keys.Escape:
                        ClosePrompt();
                        return true;
                }
                return true;
            }
            if (!(ActiveControl is TextBox) && RevealFocus(keyData == Keys.Enter || keyData == Keys.Space))
            {
                return true;
            }
            if (screen == UiScreen.Main)
            {
                switch (keyData)
                {
                    case Keys.Up:
                    case Keys.Left:
                    case Keys.Shift | Keys.Tab:
                        MainMove(-1);
                        return true;
                    case Keys.Down:
                    case Keys.Right:
                    case Keys.Tab:
                        MainMove(1);
                        return true;
                    case Keys.Enter:
                    case Keys.Space:
                        ActivateMain(mainSel);
                        return true;
                    case Keys.Escape:
                        Close();
                        return true;
                }
                return base.ProcessCmdKey(ref msg, keyData);
            }
            if (screen == UiScreen.Install)
            {
                return InstallKey(keyData) || base.ProcessCmdKey(ref msg, keyData);
            }
            if (ActiveControl is TextBox)
            {
                // Everything not listed here (letters, Q/E, Space, Backspace...)
                // belongs to the text box.
                switch (keyData)
                {
                    case Keys.Up: MoveSel(-1); return true;
                    case Keys.Down: MoveSel(1); return true;
                    case Keys.Tab: MoveSel(1); return true;
                    case Keys.Shift | Keys.Tab: MoveSel(-1); return true;
                    case Keys.Escape:
                    case Keys.Enter:
                        ActiveControl = null;
                        Invalidate();
                        return true;
                }
                return base.ProcessCmdKey(ref msg, keyData);
            }
            Row cur = SelectedRow();
            switch (keyData)
            {
                case Keys.Up: MoveSel(-1); return true;
                case Keys.Down: MoveSel(1); return true;
                case Keys.Left: Change(cur, -1); return true;
                case Keys.Right: Change(cur, 1); return true;
                case Keys.Space:
                case Keys.Enter:
                    Change(cur, 0);
                    return true;
                case Keys.Escape: SetScreen(UiScreen.Main); return true;
                case Keys.Tab:
                case Keys.PageDown:
                case Keys.E:
                    SetTab(tab + 1);
                    return true;
                case Keys.Shift | Keys.Tab:
                case Keys.PageUp:
                case Keys.Q:
                    SetTab(tab - 1);
                    return true;
            }
            return base.ProcessCmdKey(ref msg, keyData);
        }

        PointF Logical(Point p)
        {
            return new PointF(p.X / scale, p.Y / scale);
        }

        Hot HitTest(PointF p)
        {
            for (int i = hots.Count - 1; i >= 0; i--)
            {
                if (hots[i].R.Contains(p))
                {
                    return hots[i];
                }
            }
            return null;
        }

        // A hot region with a row index was entered/pressed.
        void SelectFromHot(Hot h)
        {
            if (h == null || h.Row < 0)
            {
                return;
            }
            if (screen == UiScreen.Main)
            {
                mainSel = h.Row;
            }
            else if (screen == UiScreen.Install)
            {
                instSel = h.Row;
            }
            else
            {
                sel = h.Row;
            }
        }

        protected override void OnMouseMove(MouseEventArgs e)
        {
            base.OnMouseMove(e);
            PointF p = Logical(e.Location);
            if (dragging != null)
            {
                dragging.Drag(p.X);
                return;
            }
            Hot h = HitTest(p);
            string old = hover == null ? null : hover.Id;
            hover = h;
            Cursor = h != null && (h.Click != null || h.Drag != null) ? Cursors.Hand : Cursors.Default;
            // Windows also sends mouse moves without a move (after a repaint, a window shown): only a
            // real move hands the highlight to the mouse, so keyboard focus is not lost to them.
            bool moved = e.Location != lastMouse;
            lastMouse = e.Location;
            if (moved && !(ActiveControl is TextBox))
            {
                // With the mouse, only the item under the pointer is lit: over an item it becomes the
                // current one, off the items nothing is lit.
                if (h != null && h.Row >= 0)
                {
                    int cur = screen == UiScreen.Main ? mainSel : screen == UiScreen.Install ? instSel : sel;
                    if (h.Row != cur || focusHidden)
                    {
                        SelectFromHot(h);
                        focusHidden = false;
                        Invalidate();
                    }
                }
                else if (!focusHidden)
                {
                    focusHidden = true;
                    Invalidate();
                }
            }
            if ((h == null ? null : h.Id) != old)
            {
                Invalidate();
            }
        }

        protected override void OnMouseLeave(EventArgs e)
        {
            base.OnMouseLeave(e);
            hover = null;
            // The pointer left the window (not just moved onto a text box): nothing stays lit, unless
            // a text box is being edited.
            if (!ClientRectangle.Contains(PointToClient(Cursor.Position)) && !(ActiveControl is TextBox))
            {
                focusHidden = true;
            }
            Invalidate();
        }

        // Whether item i of a list whose current item is cur is drawn lit.
        bool Lit(int cur, int i)
        {
            return !focusHidden && cur == i;
        }

        // With the highlight hidden, the first key or button shows it; an activation only shows it (so
        // nothing invisible is pressed), a move goes on from it.
        bool RevealFocus(bool activation)
        {
            if (!focusHidden)
            {
                return false;
            }
            focusHidden = false;
            Invalidate();
            return activation;
        }

        protected override void OnMouseDown(MouseEventArgs e)
        {
            base.OnMouseDown(e);
            UsePad(false);
            if (e.Button != MouseButtons.Left)
            {
                return;
            }
            PointF p = Logical(e.Location);
            Hot h = HitTest(p);
            SelectFromHot(h);
            if (h != null && h.Row >= 0)
            {
                focusHidden = false;
            }
            if (h != null && h.Drag != null)
            {
                ActiveControl = null;
                dragging = h;
                h.Drag(p.X);
                return;
            }
            if (h != null && h.Click != null)
            {
                if (!(screen == UiScreen.Settings && SelectedRow() != null && SelectedRow().Kind == RowKind.Text && h.Row == sel))
                {
                    ActiveControl = null;
                }
                pressed = h.Id;
                h.Click();
                Invalidate();
                return;
            }
            ActiveControl = null;
            Invalidate();
            // Empty space drags the window.
            ReleaseCapture();
            SendMessage(Handle, 0xA1 /* WM_NCLBUTTONDOWN */, (IntPtr)2 /* HTCAPTION */, IntPtr.Zero);
        }

        protected override void OnMouseUp(MouseEventArgs e)
        {
            base.OnMouseUp(e);
            if (pressed != null)
            {
                pressed = null;
                Invalidate();
            }
            if (dragging != null)
            {
                dragging = null;
                S.Save();
                LayoutRows();
            }
        }

        [DllImport("user32.dll")]
        static extern bool ReleaseCapture();

        [DllImport("user32.dll")]
        static extern IntPtr SendMessage(IntPtr hWnd, int msg, IntPtr wParam, IntPtr lParam);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        static extern IntPtr SendMessage(IntPtr hWnd, int msg, IntPtr wParam, string lParam);

        // ------------------------------------------------------------------
        // Painting

        Hot AddHot(RectangleF r, string id, Action click, int row = -1)
        {
            var h = new Hot { R = r, Id = id, Click = click, Row = row };
            hots.Add(h);
            return h;
        }

        bool IsHover(string id)
        {
            return hover != null && hover.Id == id;
        }

        protected override void OnResize(EventArgs e)
        {
            base.OnResize(e);
            if (bgMain != null)
            {
                bgMain.Dispose();
                bgMain = null;
            }
            if (bgDim != null)
            {
                bgDim.Dispose();
                bgDim = null;
            }
            Invalidate();
        }

        // Main screen: the art sits in a wide band between the title bar and the bottom bar.
        const float ArtTop = 40, ArtBottom = 520;

        // Draws src into dest (device pixels) cropped to cover it, centred.
        static void DrawCover(Graphics g, Bitmap src, RectangleF dest)
        {
            float sa = (float)src.Width / src.Height, da = dest.Width / dest.Height;
            RectangleF s = sa > da
                ? new RectangleF((src.Width - src.Height * da) / 2, 0, src.Height * da, src.Height)
                : new RectangleF(0, (src.Height - src.Width / da) / 2, src.Width, src.Width / da);
            using (var ia = new ImageAttributes())
            {
                ia.SetWrapMode(WrapMode.TileFlipXY);
                g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                g.PixelOffsetMode = PixelOffsetMode.HighQuality;
                g.DrawImage(src, Rectangle.Round(dest), s.X, s.Y, s.Width, s.Height, GraphicsUnit.Pixel, ia);
            }
        }

        // Backdrops are built once per window size in the screen's pixel format (premultiplied alpha
        // blits fastest), with everything that never changes already on them.
        Bitmap NewBackdrop()
        {
            return new Bitmap(ClientSize.Width, ClientSize.Height, PixelFormat.Format32bppPArgb);
        }

        Bitmap MainBackdrop(Bitmap src)
        {
            Bitmap bmp = NewBackdrop();
            using (Graphics bg = Graphics.FromImage(bmp))
            {
                bg.Clear(InkDeep);
                var band = new RectangleF(0, ArtTop * scale, W * scale, (ArtBottom - ArtTop) * scale);
                DrawCover(bg, src, band);
                float b = 90 * scale;
                using (var d = new LinearGradientBrush(new RectangleF(band.X, band.Bottom - b - 1, band.Width, b + 1),
                           Color.FromArgb(0, InkDeep), Color.FromArgb(200, InkDeep), LinearGradientMode.Vertical))
                {
                    bg.FillRectangle(d, band.X, band.Bottom - b, band.Width, b);
                }
                bg.ScaleTransform(scale, scale);
                bg.SmoothingMode = SmoothingMode.AntiAlias;
                if (logo != null)
                {
                    float lw = 440, lh = lw * logo.Height / logo.Width;
                    float top = ArtTop + (ArtBottom - ArtTop - lh) / 2 - 8;
                    bg.DrawImage(logo, new RectangleF((W - lw) / 2, top, lw, lh));
                }
                Fill(bg, InkDeep, new RectangleF(0, ArtBottom, W, H - ArtBottom));
                Line(bg, Hairline, 0, ArtBottom + 0.5f, W, ArtBottom + 0.5f);
            }
            return bmp;
        }

        // Settings and install: the art blurred under a deep ink wash.
        Bitmap DimBackdrop(Bitmap src)
        {
            Bitmap bmp = NewBackdrop();
            using (Graphics bg = Graphics.FromImage(bmp))
            {
                DrawCover(bg, src, new RectangleF(0, 0, bmp.Width, bmp.Height));
                using (var wash = new SolidBrush(Color.FromArgb(185, InkDeep)))
                {
                    bg.FillRectangle(wash, 0, 0, bmp.Width, bmp.Height);
                }
            }
            return bmp;
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            Graphics g = e.Graphics;
            hots.Clear();

            if (ClientSize.Width > 0)
            {
                if (bgMain == null && bgMainSrc != null)
                {
                    bgMain = MainBackdrop(bgMainSrc);
                }
                if (bgDim == null && bgDimSrc != null)
                {
                    bgDim = DimBackdrop(bgDimSrc);
                }
            }
            Bitmap bg = screen == UiScreen.Main ? bgMain : bgDim;
            g.CompositingMode = CompositingMode.SourceCopy;
            if (bg != null)
            {
                g.DrawImageUnscaled(bg, 0, 0);
            }
            g.CompositingMode = CompositingMode.SourceOver;

            g.ScaleTransform(scale, scale);
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.PixelOffsetMode = PixelOffsetMode.Half;
            g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;

            if (screen == UiScreen.Main)
            {
                DrawMain(g);
                if (PromptShown)
                {
                    hots.Clear();
                    DrawPrompt(g);
                }
                DrawTitleBar(g, "Launcher", false, false);
            }
            else if (screen == UiScreen.Install)
            {
                DrawInstall(g);
                DrawTitleBar(g, "Installation", true, !installing);
            }
            else
            {
                DrawSettings(g);
                DrawTitleBar(g, "Settings", true, true);
            }
            DrawToast(g);
            DrawFrame(g);
        }

        static StringFormat CenterFmt()
        {
            return new StringFormat
            {
                Alignment = StringAlignment.Center,
                LineAlignment = StringAlignment.Center,
                Trimming = StringTrimming.EllipsisCharacter,
                FormatFlags = StringFormatFlags.NoWrap,
            };
        }

        string Summary()
        {
            double scale = Math.Max(1.0, Math.Min(4.0, S.Double("render_scale")));
            string mode = S.Bool("fullscreen") ? L.T("Fullscreen") : L.F("Window {0}", S["window_size"].Replace("x", "×"));
            string aspect = S["aspect_ratio"] == "auto" ? "" : " " + S["aspect_ratio"];
            return string.Format("{0}×{1}{2}   ·   {3}   ·   {4} FPS", (int)Math.Round(1280 * scale), (int)Math.Round(720 * scale),
                aspect, mode, S.Int("fps"));
        }

        void DrawMain(Graphics g)
        {
            var play = new RectangleF((W - 260) / 2, 538, 260, 50);
            var set = new RectangleF(play.X - 20 - 170, 542, 170, 42);
            var exit = new RectangleF(play.Right + 20, 542, 170, 42);
            bool install = gameExe != null && !Installed;
            FlatButton(g, set, L.T("SETTINGS"), fBtn, false, Lit(mainSel, 1) || IsHover("settings"), false);
            FlatButton(g, exit, L.T("EXIT"), fBtn, false, Lit(mainSel, 2) || IsHover("exit"), false);
            FlatButton(g, play, L.T(searching ? "LOOKING FOR A HOST…" : install ? "INSTALL" : "PLAY"), searching ? fBtn : fPlay, true,
                !searching && (Lit(mainSel, 0) || IsHover("play")), searching);
            AddHot(set, "settings", () => SetScreen(UiScreen.Settings), 1);
            AddHot(exit, "exit", Close, 2);
            AddHot(play, "play", Play, 0);

            // The version under PLAY: up to date, or the newer release (click: the update window).
            var ver = new RectangleF((W - 520) / 2, 593, 520, 18);
            string current = "v" + Updates.Current;
            if (UpdateAvailable)
            {
                bool hv = IsHover("version");
                DrawText(g, L.F("{0} · update available: {1}", current, "v" + latest.Version), fTiny, hv ? PaperHi : Paper, ver, CenterFmt());
                AddHot(ver, "version", () =>
                {
                    updatePrompt = true;
                    promptSel = 0;
                    Invalidate();
                });
            }
            else
            {
                DrawText(g, latest != null ? L.F("{0} · up to date", current) : current, fTiny, Color.FromArgb(110, 122, 140), ver, CenterFmt());
            }

            var status = new RectangleF((W - 520) / 2, 616, 520, 22);
            if (gameExe == null)
            {
                bool hv = IsHover("path");
                DrawText(g, L.T("Game not found — click to locate armyoftworecomp.exe"), fLcd, hv ? PaperHi : Crimson, status, CenterFmt());
                AddHot(status, "path", () => BrowseGame());
            }
            else
            {
                DrawText(g, Summary(), fLcd, Color.FromArgb(130, 142, 160), status, CenterFmt());
            }
            Dot(g, 28, 627, padConnected, L.T(padConnected ? "Controller connected" : "No controller"));
        }

        // The update window: over the art, the rest of the main screen dimmed and not clickable.
        void DrawPrompt(Graphics g)
        {
            Fill(g, Color.FromArgb(170, InkDeep), new RectangleF(0, 40, W, H - 40));
            var box = new RectangleF((W - 520) / 2, 196, 520, 210);
            Panel(g, box);
            Caps(g, L.T("UPDATE AVAILABLE"), fTitle, PaperHi, new RectangleF(box.X, box.Y + 22, box.Width, 22), 2.4f);
            var text = new StringFormat { Alignment = StringAlignment.Center };
            DrawText(g, L.F("Version {0} is out, you have {1}. Download it now?", "v" + latest.Version, "v" + Updates.Current),
                fLabel, TextLight, new RectangleF(box.X + 30, box.Y + 62, box.Width - 60, 42), text);
            DrawText(g, L.T("Extract the archive over this folder: settings and saves are kept."),
                fSmall, TextDim, new RectangleF(box.X + 30, box.Y + 98, box.Width - 60, 36), text);

            var dl = new RectangleF(box.X + box.Width / 2 - 10 - 180, box.Bottom - 66, 180, 42);
            var later = new RectangleF(box.X + box.Width / 2 + 10, box.Bottom - 66, 180, 42);
            bool onButton = IsHover("upd_dl") || IsHover("upd_later");
            FlatButton(g, dl, L.T("DOWNLOAD"), fBtn, true, IsHover("upd_dl") || (!onButton && promptSel == 0), false);
            FlatButton(g, later, L.T("LATER"), fBtn, false, IsHover("upd_later") || (!onButton && promptSel == 1), false);
            AddHot(dl, "upd_dl", DownloadUpdate);
            AddHot(later, "upd_later", ClosePrompt);
        }

        void DrawSettings(Graphics g)
        {
            // Tabs: text, the current one underlined.
            Fill(g, Color.FromArgb(230, InkDeep), new RectangleF(0, 40, W, 44));
            Line(g, Hairline, 0, 83.5f, W, 83.5f);
            float x = 20;
            for (int i = 0; i < tabNames.Length; i++)
            {
                string name = L.T(tabNames[i]);
                float tw = CapsWidth(g, name, fTab, 1.8f);
                var r = new RectangleF(x, 40, tw + 36, 44);
                bool on = i == tab;
                bool hv = IsHover("tab" + i);
                if (hv && !on)
                {
                    Fill(g, Color.FromArgb(14, 255, 255, 255), r);
                }
                Caps(g, name, fTab, on ? PaperHi : hv ? Color.FromArgb(214, 218, 226) : Color.FromArgb(130, 142, 160), r, 1.8f);
                if (on)
                {
                    Fill(g, Paper, new RectangleF(r.X + 14, r.Bottom - 3, r.Width - 28, 2));
                }
                int t = i;
                AddHot(r, "tab" + i, () => SetTab(t));
                x = r.Right;
            }

            Panel(g, new RectangleF(ColX - 24, 96, ColW + 48, 500));
            SectionHeader(g, L.T(tabNames[tab]), ColX + 2, 106, ColW - 4);

            List<Row> rows = VisibleRows();
            for (int i = 0; i < rows.Count; i++)
            {
                DrawRow(g, rows[i], i);
            }

            Line(g, Hairline, ColX, DescTop + 0.5f, ColX + ColW, DescTop + 0.5f);
            Row cur = SelectedRow();
            if (cur != null && cur.Desc != null && !focusHidden)
            {
                DrawText(g, L.T(cur.Desc), fDesc, TextDim, new RectangleF(ColX, DescTop + 12, ColW, 76), StringFormat.GenericDefault);
            }
            DrawHints(g);
        }

        void DrawRow(Graphics g, Row r, int index)
        {
            var rr = new RectangleF(ColX, r.Y, ColW, r.H);
            if (r.Kind == RowKind.Info)
            {
                DrawText(g, r.Text(), fSmall, TextDim, new RectangleF(ColX + 16, r.Y + 8, ColW - 32, r.H - 8), StringFormat.GenericDefault);
                return;
            }
            bool enabled = r.Enabled();
            bool selected = Lit(sel, index) || (r.Box != null && r.Box.Focused);
            Line(g, Color.FromArgb(14, 255, 255, 255), rr.X, rr.Bottom - 0.5f, rr.Right, rr.Bottom - 0.5f);
            if (selected)
            {
                SelectionBar(g, rr);
            }
            Color labelColor = !enabled ? TextOff : selected ? PaperHi : TextLight;
            var lfmt = new StringFormat { LineAlignment = StringAlignment.Center, Trimming = StringTrimming.EllipsisCharacter, FormatFlags = StringFormatFlags.NoWrap };
            DrawText(g, r.Caption, fLabel, labelColor, new RectangleF(ColX + 16, r.Y, ValueX - ColX - 20, r.H), lfmt);

            float vx = ValueX;
            var vr = new RectangleF(vx, r.Y, ValueW, r.H);
            string id = "row" + tab + "_" + index;
            Color valColor = !enabled ? TextOff : selected ? PaperHi : Color.FromArgb(214, 218, 226);

            switch (r.Kind)
            {
                case RowKind.Choice:
                {
                    int i = Array.IndexOf(r.Values, S[r.Key]);
                    string text = i >= 0 ? r.ChoiceName(i) : S[r.Key];
                    AddHot(rr, id, () => Change(r, 1), index);
                    DrawText(g, text, fValue, valColor, new RectangleF(vx + 16, r.Y, ValueW - 32, r.H), CenterFmt());
                    if (enabled)
                    {
                        var left = new RectangleF(vx - 6, r.Y, 24, r.H);
                        var right = new RectangleF(vx + ValueW - 18, r.Y, 24, r.H);
                        Color dim = selected ? Color.FromArgb(190, Paper) : Color.FromArgb(90, Paper);
                        Chevron(g, left.X + left.Width / 2, r.Y + r.H / 2, -1, IsHover(id + "<") ? PaperHi : dim);
                        Chevron(g, right.X + right.Width / 2, r.Y + r.H / 2, 1, IsHover(id + ">") ? PaperHi : dim);
                        AddHot(left, id + "<", () => Change(r, -1), index);
                        AddHot(right, id + ">", () => Change(r, 1), index);
                    }
                    break;
                }
                case RowKind.Slider:
                {
                    double v = S.Double(r.Key);
                    var track = new RectangleF(vx + 6, r.Y + r.H / 2 - 1, ValueW - 74, 2);
                    float t = (float)((v - r.Min) / (r.Max - r.Min));
                    t = Math.Max(0, Math.Min(1, t));
                    Fill(g, Color.FromArgb(50, 255, 255, 255), track);
                    Color knob = !enabled ? TextOff : selected || dragging != null ? PaperHi : Paper;
                    Fill(g, knob, new RectangleF(track.X, track.Y, track.Width * t, track.Height));
                    using (var b = new SolidBrush(knob))
                    {
                        float kr = selected ? 6.5f : 5.5f;
                        g.FillEllipse(b, track.X + track.Width * t - kr, track.Y + 1 - kr, kr * 2, kr * 2);
                    }
                    var fmt = new StringFormat { Alignment = StringAlignment.Far, LineAlignment = StringAlignment.Center };
                    DrawText(g, r.Format(v), fValue, valColor, new RectangleF(track.Right + 8, r.Y, vr.Right - track.Right - 8, r.H), fmt);
                    AddHot(rr, id, null, index);
                    if (enabled)
                    {
                        var drag = new RectangleF(track.X - 8, r.Y, track.Width + 16, r.H);
                        Hot h = AddHot(drag, id + "~", null, index);
                        h.Drag = px =>
                        {
                            double f = Math.Max(0, Math.Min(1, (px - track.X) / track.Width));
                            SetSlider(r, r.Min + f * (r.Max - r.Min));
                        };
                    }
                    break;
                }
                case RowKind.Text:
                {
                    var box = new RectangleF(vx, r.Y + 5, ValueW, r.H - 10);
                    Fill(g, TextBoxBack, box);
                    bool focused = r.Box != null && r.Box.Focused;
                    Outline(g, focused ? Paper : selected ? Color.FromArgb(110, Paper) : Color.FromArgb(50, Paper), box);
                    if (snapshotting)
                    {
                        // DrawToBitmap leaves the real text boxes out: paint the text.
                        string t = S[r.Key];
                        bool empty = t.Length == 0;
                        if (empty && r.Placeholder != null)
                        {
                            t = r.Placeholder();
                        }
                        DrawText(g, t, fSmall, empty ? TextOff : TextLight, new RectangleF(box.X + 6, box.Y, box.Width - 12, box.Height),
                            new StringFormat { LineAlignment = StringAlignment.Center, Trimming = StringTrimming.EllipsisCharacter, FormatFlags = StringFormatFlags.NoWrap });
                    }
                    AddHot(rr, id, () => Change(r, 0), index);
                    break;
                }
                case RowKind.Button:
                {
                    var br = new RectangleF(vx, r.Y + 5, ValueW, r.H - 10);
                    FlatButton(g, br, L.T(r.ButtonText()), fTab, false, IsHover(id + "!") || selected, !enabled);
                    AddHot(rr, id, null, index);
                    AddHot(br, id + "!", () => Change(r, 0), index);
                    break;
                }
            }
        }

        void DrawButtonGlyph(Graphics g, float x, float cy, string letter, Color color)
        {
            var r = new RectangleF(x, cy - 9, 18, 18);
            using (var b = new SolidBrush(color))
            {
                g.FillEllipse(b, r);
            }
            DrawText(g, letter, fBack, Color.White, new RectangleF(r.X, r.Y + 0.5f, r.Width, r.Height), CenterFmt());
        }

        void DrawBumper(Graphics g, float x, float cy, string text)
        {
            var r = new RectangleF(x, cy - 8, 28, 16);
            Outline(g, Color.FromArgb(120, Paper), r);
            Caps(g, text, fBack, Color.FromArgb(214, 218, 226), r, 0.6f);
        }

        // A keyboard key: a hairline keycap sized to its caption. Returns its width.
        float KeyCapWidth(Graphics g, string text)
        {
            return Math.Max(20, CapsWidth(g, text, fBack, 0.6f) + 14);
        }

        void DrawKeyCap(Graphics g, float x, float cy, string text)
        {
            var r = new RectangleF(x, cy - 9, KeyCapWidth(g, text), 18);
            Outline(g, Color.FromArgb(120, Paper), r);
            Line(g, Color.FromArgb(120, Paper), r.X + 1, r.Bottom + 0.5f, r.Right - 1, r.Bottom + 0.5f);
            Caps(g, text, fBack, Color.FromArgb(214, 218, 226), r, 0.6f);
        }

        // Hint line at the bottom, like the game's "(A) Select (B) Back": keyboard keys, or the
        // controller's buttons while the controller was the last thing used.
        void DrawHints(Graphics g)
        {
            float cy = 630;
            var fmt = new StringFormat { LineAlignment = StringAlignment.Center };
            Color tc = Color.FromArgb(150, 162, 180);
            bool pad = padInput;
            string[][] keys = pad
                ? new[] { new[] { "LB", "RB" }, new[] { "A" }, new[] { "B" } }
                : new[] { new[] { "Q", "E" }, new[] { "ENTER" }, new[] { "ESC" } };
            string[] labels = { L.T("Tabs"), L.T("Select"), L.T("Back") };
            Action[] acts = { null, () => Change(SelectedRow(), 0), () => SetScreen(UiScreen.Main) };
            const float gap = 6, labelGap = 8, groupGap = 30;

            // Widths first, to centre the line under the column.
            var widths = new float[keys.Length];
            float total = 0;
            for (int i = 0; i < keys.Length; i++)
            {
                float w = 0;
                foreach (string k in keys[i])
                {
                    w += (pad ? (k.Length == 1 ? 18 : 28) : KeyCapWidth(g, k)) + gap;
                }
                w += labelGap - gap + g.MeasureString(labels[i], fHint).Width;
                widths[i] = w;
                total += w + (i > 0 ? groupGap : 0);
            }
            float x = ColX + (ColW - total) / 2f;
            for (int i = 0; i < keys.Length; i++)
            {
                float x0 = x;
                foreach (string k in keys[i])
                {
                    if (!pad)
                    {
                        DrawKeyCap(g, x, cy, k);
                        x += KeyCapWidth(g, k) + gap;
                    }
                    else if (k.Length == 1)
                    {
                        DrawButtonGlyph(g, x, cy, k, k == "A" ? Color.FromArgb(76, 150, 60) : Color.FromArgb(190, 56, 50));
                        x += 18 + gap;
                    }
                    else
                    {
                        DrawBumper(g, x, cy, k);
                        x += 28 + gap;
                    }
                }
                x += labelGap - gap;
                DrawText(g, labels[i], fHint, tc, new RectangleF(x, cy - 10, 80, 20), fmt);
                if (acts[i] != null)
                {
                    AddHot(new RectangleF(x0 - 2, cy - 14, widths[i] + 4, 28), "hint" + i, acts[i]);
                }
                x = x0 + widths[i] + groupGap;
            }
            Dot(g, 28, cy, padConnected, L.T(padConnected ? "Controller connected" : "No controller"));
        }

        void DrawToast(Graphics g)
        {
            if (toast == null)
            {
                return;
            }
            SizeF sz = g.MeasureString(toast, fLabel);
            float w = Math.Min(W - 40, sz.Width + 40);
            float y = screen == UiScreen.Main ? ArtBottom - 46 : 556;
            var tr = new RectangleF((W - w) / 2, y, w, 30);
            Fill(g, Color.FromArgb(245, InkDeep), tr);
            Outline(g, toastBad ? Crimson : Color.FromArgb(120, Paper), tr);
            DrawText(g, toast, fLabel, toastBad ? Crimson : PaperHi, tr, CenterFmt());
        }
    }

    static class Program
    {
        [DllImport("user32.dll")]
        static extern bool SetProcessDPIAware();

        [STAThread]
        static void Main()
        {
            try
            {
                SetProcessDPIAware();
            }
            catch
            {
            }
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            // Debug: --snapshot=<png> [--screen=main|settings] [--tab=N] [--sel=N] renders the window to a file without
            // showing it; --dump-args=<file> [--host=<ip>] writes the game command line; --probe=<file> runs the
            // host search and writes the found IP (or "none").
            string[] args = Environment.GetCommandLineArgs();
            Func<string, string> opt = name => args.Where(a => a.StartsWith("--" + name + "=")).Select(a => a.Substring(name.Length + 3)).FirstOrDefault();
            string snap = opt("snapshot");
            string dump = opt("dump-args");
            string probe = opt("probe");
            if (dump != null)
            {
                File.WriteAllText(dump, Game.CommandLine(new Settings(), opt("host")));
                return;
            }
            // --check-update=<file>: asks GitHub for the latest release, writes "<version> <url>" (or "none").
            string check = opt("check-update");
            if (check != null)
            {
                Updates.Release r = Updates.Latest();
                File.WriteAllText(check, r == null ? "none" : r.Version + " " + r.Url +
                    (Updates.Newer(r.Version, Updates.Current) ? " newer" : " current"));
                return;
            }
            if (probe != null)
            {
                string found = Discovery.FindHost(new Settings()["host_ip"].Trim(), 1200);
                File.WriteAllText(probe, found ?? "none");
                return;
            }
            // --install=<log> --src=<path>[;<path>...]: install from these sources without the window
            // (DLC goes to the real user data folder).
            string install = opt("install");
            if (install != null)
            {
                string result = "OK";
                try
                {
                    string dir = Path.GetDirectoryName(Game.Find(new Settings()));
                    InstallSource game = null, update = null;
                    var dlc = new List<InstallSource>();
                    foreach (string path in (opt("src") ?? "").Split(new[] { ';' }, StringSplitOptions.RemoveEmptyEntries))
                    {
                        InstallSource src = Installer.Probe(dir, path);
                        File.AppendAllText(install, src.Kind + ": " + src.Caption + "\r\n");
                        if (src.Kind == "game") game = src;
                        else if (src.Kind == "tu") update = src;
                        else dlc.Add(src);
                    }
                    string last = null;
                    Installer.Run(dir, game, update, dlc, (name, p) =>
                    {
                        if (name != last)
                        {
                            File.AppendAllText(install, name + "\r\n");
                            last = name;
                        }
                    }, () => false);
                }
                catch (Exception ex)
                {
                    result = "ERROR " + ex.Message;
                }
                File.AppendAllText(install, result + "\r\n");
                return;
            }
            Updates.Override = opt("latest"); // --latest=<tag>: the latest release, without asking GitHub
            var form = new LauncherForm();
            if (opt("bench") != null)
            {
                form.Bench(opt("bench"));
                return;
            }
            if (snap != null)
            {
                form.padInput = opt("hints") == "pad"; // --hints=pad: the controller's buttons in the hints
                int t = 0, sl = 0;
                int.TryParse(opt("tab"), out t);
                int.TryParse(opt("sel"), out sl);
                UiScreen scr = opt("screen") == "settings" ? UiScreen.Settings : opt("screen") == "install" ? UiScreen.Install : UiScreen.Main;
                form.Snapshot(snap, t, sl, scr);
                return;
            }
            Application.Run(form);
        }
    }
}
