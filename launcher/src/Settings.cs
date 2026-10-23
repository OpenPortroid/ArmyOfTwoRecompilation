using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;

namespace AO2Launcher
{
    // Launcher settings: flat key=value file next to the launcher (portable), or
    // %APPDATA%\ArmyOfTwoLauncher when that folder is read-only. The game's own
    // settings\*.toml are never written: everything goes to the game as --flags,
    // which take precedence over the toml files.
    sealed class Settings
    {
        public static readonly Dictionary<string, string> Defaults = new Dictionary<string, string>
        {
            { "fullscreen", "1" },
            { "window_size", "1920x1080" },
            { "monitor", "0" },
            { "aspect_ratio", "auto" },
            { "render_scale", "2" },
            { "fps", "240" },
            { "fps_overlay", "0" },
            { "aniso", "0" },
            { "aa", "off" },
            { "ao", "off" },
            { "motion_blur", "0" },
            { "dof", "0" },
            { "shadow_res", "0" },
            { "vsync", "0" },
            { "language", "auto" },
            { "seen_version", "" }, // the version whose release notes were shown
            { "mouse_aim", "1" },
            { "mouse_sens", "2" },
            { "mouse_zoom", "1" },
            { "invert_y", "0" },
            { "aim_assist", "0" },
            { "stick_sens", "1" },
            { "toggle_aim", "0" },
            { "god_mode", "0" },
            { "ach_notices", "1" },
            { "unlock_all", "0" },
            { "profile", "" },
            { "new_profile", "" },
            { "host_ip", "" },
            { "gamertag", "" },
            { "split_screen", "0" },
            { "gamertag2", "" },
            { "p1_input", "keyboard" },
            { "p2_input", "pads" },
            { "pad_left_stick", "LS" },
            { "pad_right_stick", "RS" },
            { "pad_invert_left_y", "0" },
            { "pad_invert_right_y", "0" },
            { "input_margin", "0" },
            { "coop_debug", "0" },
            { "extra_args", "" },
            { "close_on_launch", "1" },
            { "desktop_shortcut", "1" },
            { "game_path", "" },
            { "tab", "0" },
        };

        // Key and gamepad bindings: kb_<name> / pad_<name> (Bindings.cs).
        static Settings()
        {
            foreach (var b in Bindings.Keyboard)
            {
                Defaults[b.KbKey] = b.Default;
            }
            foreach (var b in Bindings.Pad)
            {
                Defaults[b.PadKey] = b.Default;
            }
        }

        readonly Dictionary<string, string> values = new Dictionary<string, string>(Defaults);
        public string FilePath { get; private set; }

        public Settings()
        {
            string dir = AppDomain.CurrentDomain.BaseDirectory;
            FilePath = Path.Combine(dir, "ArmyOfTwoLauncher.ini");
            if (!File.Exists(FilePath) && !CanWrite(dir))
            {
                string appdata = Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "ArmyOfTwoLauncher");
                FilePath = Path.Combine(appdata, "ArmyOfTwoLauncher.ini");
            }
            Load();
            // Depth of field had a HIGH level (2); now it is just on.
            if (this["dof"] == "2")
            {
                this["dof"] = "1";
            }
        }

        static bool CanWrite(string dir)
        {
            try
            {
                string probe = Path.Combine(dir, ".ao2launcher_probe");
                File.WriteAllText(probe, "");
                File.Delete(probe);
                return true;
            }
            catch
            {
                return false;
            }
        }

        // The file as this launcher last read or wrote it. The game writes the file too (its in-game
        // Graphics and Controls pages): a key whose value here is still the stored one takes the
        // file's (Reload, Save), one the launcher changed meanwhile keeps the launcher's.
        readonly Dictionary<string, string> stored = new Dictionary<string, string>(Defaults);

        // The known keys of the file, null without one (or unreadable).
        Dictionary<string, string> ReadFile()
        {
            try
            {
                if (!File.Exists(FilePath))
                {
                    return null;
                }
                var file = new Dictionary<string, string>();
                foreach (string raw in File.ReadAllLines(FilePath, Encoding.UTF8))
                {
                    string line = raw.Trim();
                    if (line.Length == 0 || line[0] == '#' || line[0] == ';')
                    {
                        continue;
                    }
                    int eq = line.IndexOf('=');
                    if (eq <= 0)
                    {
                        continue;
                    }
                    string key = line.Substring(0, eq).Trim();
                    if (Defaults.ContainsKey(key))
                    {
                        file[key] = line.Substring(eq + 1).Trim();
                    }
                }
                return file;
            }
            catch
            {
                // A broken file just means defaults.
                return null;
            }
        }

        void Load()
        {
            Reload();
            // The game's MSAA is no longer offered.
            string aa;
            if (values.TryGetValue("aa", out aa) && aa == "msaa")
            {
                values["aa"] = "off";
            }
        }

        // Takes in what the game wrote to the file since (see `stored`).
        public void Reload()
        {
            Dictionary<string, string> file = ReadFile();
            if (file == null)
            {
                return;
            }
            foreach (var kv in file)
            {
                string mine, was;
                values.TryGetValue(kv.Key, out mine);
                stored.TryGetValue(kv.Key, out was);
                if (mine == was)
                {
                    values[kv.Key] = kv.Value;
                }
                stored[kv.Key] = kv.Value;
            }
        }

        public void Save()
        {
            try
            {
                Reload();
                Directory.CreateDirectory(Path.GetDirectoryName(FilePath));
                var sb = new StringBuilder();
                sb.AppendLine("# Army of Two launcher settings (the launcher rewrites this file)");
                foreach (var kv in values)
                {
                    sb.Append(kv.Key).Append(" = ").AppendLine(kv.Value);
                }
                // Written whole to a file of its own, then put in place: a crash or a read by the game
                // in the middle never sees half a file.
                string text = sb.ToString(), part = FilePath + ".tmp";
                try
                {
                    File.WriteAllText(part, text, new UTF8Encoding(false));
                    if (File.Exists(FilePath))
                    {
                        File.Replace(part, FilePath, null);
                    }
                    else
                    {
                        File.Move(part, FilePath);
                    }
                }
                catch (Exception ex)
                {
                    if (!(ex is IOException) && !(ex is UnauthorizedAccessException))
                    {
                        throw;
                    }
                    // No replace here (the file held open, a file system without it): written in place.
                    File.WriteAllText(FilePath, text, new UTF8Encoding(false));
                    try
                    {
                        File.Delete(part);
                    }
                    catch
                    {
                    }
                }
                foreach (var kv in values)
                {
                    stored[kv.Key] = kv.Value;
                }
            }
            catch
            {
                // Read-only media etc.: settings just aren't remembered.
            }
        }

        public string this[string key]
        {
            get
            {
                string v;
                return values.TryGetValue(key, out v) ? v : "";
            }
            set
            {
                values[key] = value ?? "";
            }
        }

        public bool Bool(string key)
        {
            return this[key] == "1";
        }

        public int Int(string key)
        {
            int v;
            if (int.TryParse(this[key], NumberStyles.Integer, CultureInfo.InvariantCulture, out v))
            {
                return v;
            }
            int.TryParse(Defaults[key], NumberStyles.Integer, CultureInfo.InvariantCulture, out v);
            return v;
        }

        public double Double(string key)
        {
            double v;
            if (double.TryParse(this[key], NumberStyles.Float, CultureInfo.InvariantCulture, out v))
            {
                return v;
            }
            double.TryParse(Defaults[key], NumberStyles.Float, CultureInfo.InvariantCulture, out v);
            return v;
        }

        // Everything but the game's path and the chosen profile.
        public void Reset()
        {
            string game = this["game_path"], profile = this["profile"], seen = this["seen_version"];
            values.Clear();
            foreach (var kv in Defaults)
            {
                values[kv.Key] = kv.Value;
            }
            values["game_path"] = game;
            values["profile"] = profile;
            values["seen_version"] = seen;
        }
    }
}
