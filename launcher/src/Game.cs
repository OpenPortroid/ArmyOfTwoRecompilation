using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;

namespace AO2Launcher
{
    // Everything about the game itself: where it is, the command line built
    // from the launcher settings, starting it.
    static class Game
    {
        public const string ExeName = "armyoftworecomp.exe";
        // The same game built for x86-64-v2: ExeName needs AVX2 (x86-64-v3).
        public const string CompatExeName = "armyoftworecomp_compat.exe";
        public const string LobbyServerName = "ao2_lobby_server.exe";
        const string FirewallRule = "Army of Two (recomp)";
        // ao2_game_language's values besides "auto".
        static readonly string[] Languages = { "en", "ru", "fr", "de", "es", "it" };

        // The launcher's own folder first (the usual place: next to the game),
        // then the remembered path, then a few folders around the launcher.
        public static string Find(Settings s)
        {
            var candidates = new List<string>();
            string own = AppDomain.CurrentDomain.BaseDirectory;
            candidates.Add(Path.Combine(own, ExeName));
            if (s["game_path"].Length > 0)
            {
                candidates.Add(s["game_path"]);
            }
            try
            {
                var dir = new DirectoryInfo(own);
                for (int i = 0; i < 3 && dir != null; i++, dir = dir.Parent)
                {
                    candidates.Add(Path.Combine(dir.FullName, ExeName));
                    candidates.Add(Path.Combine(dir.FullName, "ArmyOfTwo", ExeName));
                    candidates.Add(Path.Combine(dir.FullName, "Army of Two", ExeName));
                }
            }
            catch
            {
            }
            foreach (string c in candidates)
            {
                try
                {
                    if (File.Exists(c))
                    {
                        return Path.GetFullPath(c);
                    }
                }
                catch
                {
                }
            }
            return null;
        }

        public static bool IsRunning()
        {
            try
            {
                return Process.GetProcessesByName(Path.GetFileNameWithoutExtension(ExeName)).Length > 0 ||
                    Process.GetProcessesByName(Path.GetFileNameWithoutExtension(CompatExeName)).Length > 0;
            }
            catch
            {
                return false;
            }
        }

        [DllImport("kernel32.dll")]
        static extern bool IsProcessorFeaturePresent(int feature);

        const int PF_SSE4_2_INSTRUCTIONS_AVAILABLE = 38;
        const int PF_AVX2_INSTRUCTIONS_AVAILABLE = 40;

        // AVX2 usable (CPU and OS). Every such CPU also has the rest of
        // x86-64-v3 (BMI2, MOVBE, FMA...) the main build is compiled for.
        public static bool PretendNoAvx2;
        // Debug: act as a Windows older than 10 2004 (IsProcessorFeaturePresent knows no AVX2).
        public static bool PretendOldWindows;
        static bool? avx2;

        public static bool HasAvx2()
        {
            if (PretendNoAvx2)
            {
                return false;
            }
            if (avx2 == null)
            {
                avx2 = DetectAvx2();
            }
            return avx2.Value;
        }

        static bool DetectAvx2()
        {
            try
            {
                if (!PretendOldWindows)
                {
                    if (IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE))
                    {
                        return true;
                    }
                    // PF_SSSE3..PF_AVX512F came with Windows 10 2004: an older Windows answers no to all of
                    // them. SSE4.2 is on every CPU the game runs on, so a yes to it means the AVX2 answer is real.
                    if (IsProcessorFeaturePresent(PF_SSE4_2_INSTRUCTIONS_AVAILABLE))
                    {
                        Log.Write("cpu: no AVX2 (Windows)");
                        return false;
                    }
                }
            }
            catch
            {
            }
            // Windows can't tell: CPUID through ao2_install. Unknown: the compat build, which runs everywhere.
            bool? v3 = Installer.CpuRunsMainBuild(AppDomain.CurrentDomain.BaseDirectory);
            Log.Write("cpu: Windows does not report AVX2, ao2_install cpu: {0}", v3 == null ? "unknown" : v3.Value ? "x86-64-v3" : "no x86-64-v3");
            return v3 == true;
        }

        // The release ships both builds; leave in the package folder dir only
        // the one this CPU runs best, as ExeName: the compat build replaces the
        // main one without AVX2, else it is deleted. Returns what was done, or
        // null (nothing to do, or the game is running and its exe is locked -
        // then RunExe still picks the compat build).
        public static string InstallCpuBuild(string dir)
        {
            string main = Path.Combine(dir, ExeName);
            string compat = Path.Combine(dir, CompatExeName);
            if (!File.Exists(main) || !File.Exists(compat) || IsRunning())
            {
                return null;
            }
            try
            {
                if (HasAvx2())
                {
                    File.Delete(compat);
                    return "AVX2: removed " + CompatExeName;
                }
                string tmp = main + ".new";
                File.Copy(compat, tmp, true);
                File.Replace(tmp, main, null);
                File.Delete(compat);
                return "no AVX2: " + CompatExeName + " installed as " + ExeName;
            }
            catch (Exception ex)
            {
                return "CPU build not installed: " + ex.Message;
            }
        }

        // The program to run for the game found at exe: the compat build next
        // to it when the CPU lacks AVX2 (the main one would stop at its first
        // AVX2 instruction).
        public static string RunExe(string exe)
        {
            if (HasAvx2())
            {
                return exe;
            }
            string compat = Path.Combine(Path.GetDirectoryName(exe), CompatExeName);
            return File.Exists(compat) ? compat : exe;
        }

        static string Flag(string name, bool on)
        {
            return on ? "--" + name : "--no-" + name;
        }

        static string Num(double v)
        {
            return v.ToString("0.###", CultureInfo.InvariantCulture);
        }

        public static string Quote(string arg)
        {
            if (arg.Length > 0 && arg.IndexOfAny(new[] { ' ', '\t', '"' }) < 0)
            {
                return arg;
            }
            // CommandLineToArgvW rules: backslashes are literal unless they
            // precede a quote.
            var sb = new StringBuilder("\"");
            int slashes = 0;
            foreach (char c in arg)
            {
                if (c == '\\')
                {
                    slashes++;
                    continue;
                }
                if (c == '"')
                {
                    sb.Append('\\', slashes * 2 + 1);
                }
                else
                {
                    sb.Append('\\', slashes);
                }
                slashes = 0;
                sb.Append(c);
            }
            sb.Append('\\', slashes * 2);
            sb.Append('"');
            return sb.ToString();
        }

        public static List<string> BuildArgs(Settings s, string hostIp)
        {
            var a = new List<string>();

            bool fullscreen = s.Bool("fullscreen");
            a.Add(Flag("fullscreen", fullscreen));
            int monitor = s.Int("monitor");
            a.Add("--monitor=" + monitor);
            if (!fullscreen)
            {
                int w, h;
                if (ParseSize(s["window_size"], out w, out h))
                {
                    // The game sizes its window in logical pixels (scaled by the
                    // monitor's DPI), the launcher offers physical pixels.
                    double dpi = Display.MonitorScale(monitor);
                    a.Add("--window_width=" + (int)Math.Round(w / dpi));
                    a.Add("--window_height=" + (int)Math.Round(h / dpi));
                }
            }
            a.Add("--ao2_aspect_ratio=" + s["aspect_ratio"]);
            a.Add("--ao2_native_gpu_resolution_scale=" + Num(Math.Max(1.0, Math.Min(4.0, s.Double("render_scale")))));
            a.Add("--ao2_fps_unlock");
            a.Add("--ao2_fps_target=" + s.Int("fps"));
            a.Add("--ao2_fps_overlay=" + Math.Max(0, Math.Min(2, s.Int("fps_overlay"))));
            a.Add(Flag("ao2_anisotropic_16x", s.Bool("aniso")));
            a.Add(Flag("ao2_native_gpu_vsync", s.Bool("vsync")));
            string aa = s["aa"];
            a.Add("--ao2_anti_aliasing=" + (aa == "fxaa" || aa == "smaa" ? aa : "off"));
            string ao = s["ao"];
            a.Add("--ao2_ambient_occlusion=" + (ao == "ssao" || ao == "hbao" || ao == "gtao" ? ao : "off"));
            a.Add("--ao2_motion_blur=" + Math.Max(0, Math.Min(2, s.Int("motion_blur"))));
            a.Add("--ao2_depth_of_field=" + (s.Int("dof") > 0 ? 1 : 0));
            int shadow = s.Int("shadow_res");
            a.Add("--ao2_shadow_resolution=" + (shadow == 1024 || shadow == 2048 || shadow == 4096 ? shadow : 0));
            string lang = s["language"];
            a.Add("--ao2_game_language=" + (Array.IndexOf(Languages, lang) >= 0 ? lang : "auto"));

            a.Add(Flag("ao2_mouse_aim", s.Bool("mouse_aim")));
            a.Add("--ao2_mouse_sensitivity=" + Num(s.Double("mouse_sens")));
            a.Add("--ao2_mouse_zoom_sensitivity=" + Num(s.Double("mouse_zoom")));
            a.Add(Flag("ao2_mouse_invert_y", s.Bool("invert_y")));
            a.Add(Flag("ao2_aim_assist", s.Bool("aim_assist")));
            a.Add("--ao2_mouse_stick_sensitivity=" + Num(s.Double("stick_sens")));
            a.Add(Flag("ao2_toggle_aim", s.Bool("toggle_aim")));

            a.Add(Flag("ao2_skip_cutscenes", true));
            a.Add(Flag("ao2_skip_unskippable", true));
            a.Add(Flag("ao2_god_mode", s.Bool("god_mode")));
            a.Add(Flag("ao2_unlock_all", s.Bool("unlock_all")));
            a.Add(Flag("ao2_achievement_notices", s.Bool("ach_notices")));

            // A profile other than the main one: its own user data folder, the main one's shader cache.
            string profile = s["profile"];
            if (profile.Length > 0 && Profiles.Exists(profile))
            {
                a.Add("--user_data_root=" + Profiles.Root(profile));
                a.Add("--cache_root=" + Path.Combine(Profiles.MainRoot, "cache"));
            }

            if (!string.IsNullOrEmpty(hostIp))
            {
                a.Add("--no-ao2_coop_server");
                a.Add("--ao2_coop_host=" + hostIp);
            }
            else
            {
                a.Add("--ao2_coop_server");
            }
            if (s.Bool("coop_debug"))
            {
                a.Add("--ao2_coop_debug");
            }
            string tag = s["gamertag"].Trim();
            if (tag.Length > 0)
            {
                a.Add("--net_gamertag=" + tag);
            }
            a.Add("--ao2_coop_input_margin_ms=" + s.Int("input_margin"));

            // Split-screen: the players' devices and the second one's name. Off, every device is player 1's
            // and nobody else signs in.
            if (s.Bool("split_screen"))
            {
                a.Add("--max_local_users=2");
                a.Add("--input_player1=" + s["p1_input"]);
                a.Add("--input_player2=" + s["p2_input"]);
                string tag2 = s["gamertag2"].Trim();
                if (tag2.Length > 0)
                {
                    a.Add("--user_gamertag_2=" + tag2);
                }
            }
            else
            {
                a.Add("--max_local_users=1");
                a.Add("--input_player1=keyboard,pads");
                a.Add("--input_player2=none");
            }

            // Bindings: all of them, so the launcher's page is what the game uses. A flag needs a value:
            // "none" binds nothing.
            Func<string, string> bind = v => v.Trim().Length > 0 ? v.Trim() : "none";
            foreach (var b in Bindings.Keyboard)
            {
                a.Add("--keybind_" + b.Name + "=" + bind(s[b.KbKey]));
            }
            foreach (var b in Bindings.Pad)
            {
                a.Add("--padbind_" + b.Name + "=" + bind(s[b.PadKey]));
            }
            a.Add("--padbind_left_stick=" + s["pad_left_stick"]);
            a.Add("--padbind_right_stick=" + s["pad_right_stick"]);
            a.Add(Flag("pad_invert_left_y", s.Bool("pad_invert_left_y")));
            a.Add(Flag("pad_invert_right_y", s.Bool("pad_invert_right_y")));
            // The game's Graphics page (Options) saves its changes here too.
            a.Add("--ao2_launcher_settings=" + s.FilePath);
            return a;
        }

        public static string CommandLine(Settings s, string hostIp)
        {
            string line = string.Join(" ", BuildArgs(s, hostIp).Select(Quote));
            string extra = s["extra_args"].Trim();
            if (extra.Length > 0)
            {
                line += " " + extra;
            }
            return line;
        }

        public static bool ParseSize(string text, out int w, out int h)
        {
            w = h = 0;
            string[] p = (text ?? "").ToLowerInvariant().Split('x');
            return p.Length == 2 && int.TryParse(p[0], out w) && int.TryParse(p[1], out h) && w > 0 && h > 0;
        }

        public static void Start(string exe, Settings s, string hostIp)
        {
            var psi = new ProcessStartInfo(RunExe(exe), CommandLine(s, hostIp))
            {
                WorkingDirectory = Path.GetDirectoryName(exe),
                UseShellExecute = false,
            };
            Process.Start(psi);
        }

        // IPv4 addresses of this PC the partner could reach (LAN, Radmin VPN...).
        public static List<KeyValuePair<string, string>> LocalAddresses()
        {
            var list = new List<KeyValuePair<string, string>>();
            try
            {
                foreach (var ni in NetworkInterface.GetAllNetworkInterfaces())
                {
                    if (ni.OperationalStatus != OperationalStatus.Up ||
                        ni.NetworkInterfaceType == NetworkInterfaceType.Loopback)
                    {
                        continue;
                    }
                    foreach (var ua in ni.GetIPProperties().UnicastAddresses)
                    {
                        if (ua.Address.AddressFamily != AddressFamily.InterNetwork)
                        {
                            continue;
                        }
                        string ip = ua.Address.ToString();
                        if (ip.StartsWith("127.") || ip.StartsWith("169.254."))
                        {
                            continue;
                        }
                        list.Add(new KeyValuePair<string, string>(ip, ni.Name));
                    }
                }
            }
            catch
            {
            }
            // VPN (Radmin 26.x) first: that's the one to give out over the internet.
            return list.OrderBy(kv => kv.Key.StartsWith("26.") ? 0 : 1).ToList();
        }

        // Local IPv4 unicast addresses (with masks) of Up, non-loopback adapters.
        public static List<KeyValuePair<IPAddress, IPAddress>> LocalSubnets()
        {
            var list = new List<KeyValuePair<IPAddress, IPAddress>>();
            try
            {
                foreach (var ni in NetworkInterface.GetAllNetworkInterfaces())
                {
                    if (ni.OperationalStatus != OperationalStatus.Up ||
                        ni.NetworkInterfaceType == NetworkInterfaceType.Loopback)
                    {
                        continue;
                    }
                    foreach (var ua in ni.GetIPProperties().UnicastAddresses)
                    {
                        if (ua.Address.AddressFamily != AddressFamily.InterNetwork)
                        {
                            continue;
                        }
                        string ip = ua.Address.ToString();
                        if (ip.StartsWith("127.") || ip.StartsWith("169.254."))
                        {
                            continue;
                        }
                        list.Add(new KeyValuePair<IPAddress, IPAddress>(ua.Address, ua.IPv4Mask));
                    }
                }
            }
            catch
            {
            }
            return list;
        }

        // Same rules as the dist's "Allow through firewall" batch file, one UAC prompt.
        public static bool AllowThroughFirewall(string exe)
        {
            string dir = Path.GetDirectoryName(exe);
            var cmd = new StringBuilder();
            cmd.AppendFormat("netsh advfirewall firewall delete rule name=\"{0}\" >nul 2>&1", FirewallRule);
            cmd.AppendFormat(" & netsh advfirewall firewall add rule name=\"{0}\" dir=in action=allow program=\"{1}\" enable=yes profile=any",
                FirewallRule, exe);
            foreach (string other in new[] { CompatExeName, LobbyServerName })
            {
                string path = Path.Combine(dir, other);
                if (File.Exists(path))
                {
                    cmd.AppendFormat(" & netsh advfirewall firewall add rule name=\"{0}\" dir=in action=allow program=\"{1}\" enable=yes profile=any",
                        FirewallRule, path);
                }
            }
            var psi = new ProcessStartInfo("cmd.exe", "/s /c \"" + cmd + "\"")
            {
                Verb = "runas",
                UseShellExecute = true,
                WindowStyle = ProcessWindowStyle.Hidden,
            };
            try
            {
                using (var p = Process.Start(psi))
                {
                    p.WaitForExit(15000);
                    return p.HasExited && p.ExitCode == 0;
                }
            }
            catch
            {
                // UAC declined.
                return false;
            }
        }

        public static void OpenFolder(string path)
        {
            try
            {
                Process.Start(new ProcessStartInfo("explorer.exe", Quote(path)) { UseShellExecute = true });
            }
            catch
            {
            }
        }
    }

    static class Display
    {
        [DllImport("user32.dll")]
        static extern IntPtr MonitorFromPoint(POINT pt, uint flags);

        [DllImport("shcore.dll")]
        static extern int GetDpiForMonitor(IntPtr monitor, int type, out uint dpiX, out uint dpiY);

        [StructLayout(LayoutKind.Sequential)]
        struct POINT
        {
            public int X, Y;
        }

        // The game's "monitor" setting: 0 = default (primary), N = N-th display.
        public static Screen ScreenFor(int monitor)
        {
            Screen[] all = Screen.AllScreens;
            if (monitor >= 1 && monitor <= all.Length)
            {
                return all[monitor - 1];
            }
            return Screen.PrimaryScreen;
        }

        public static double MonitorScale(int monitor)
        {
            try
            {
                Screen sc = ScreenFor(monitor);
                var pt = new POINT { X = sc.Bounds.Left + sc.Bounds.Width / 2, Y = sc.Bounds.Top + sc.Bounds.Height / 2 };
                IntPtr mon = MonitorFromPoint(pt, 2);
                uint dx, dy;
                if (GetDpiForMonitor(mon, 0, out dx, out dy) == 0 && dx > 0)
                {
                    return dx / 96.0;
                }
            }
            catch
            {
            }
            return 1.0;
        }
    }
}
