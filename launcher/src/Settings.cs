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
            { "vsync", "0" },
            { "language", "auto" },
            { "mouse_aim", "1" },
            { "mouse_sens", "2" },
            { "mouse_zoom", "1" },
            { "invert_y", "0" },
            { "stick_sens", "1" },
            { "skip_cutscenes", "1" },
            { "skip_unskippable", "1" },
            { "god_mode", "0" },
            { "host_ip", "" },
            { "gamertag", "" },
            { "input_margin", "0" },
            { "coop_debug", "0" },
            { "extra_args", "" },
            { "close_on_launch", "1" },
            { "desktop_shortcut", "1" },
            { "game_path", "" },
            { "tab", "0" },
        };

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

        void Load()
        {
            try
            {
                if (!File.Exists(FilePath))
                {
                    return;
                }
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
                        values[key] = line.Substring(eq + 1).Trim();
                    }
                }
            }
            catch
            {
                // A broken file just means defaults.
            }
        }

        public void Save()
        {
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(FilePath));
                var sb = new StringBuilder();
                sb.AppendLine("# Army of Two launcher settings (the launcher rewrites this file)");
                foreach (var kv in values)
                {
                    sb.Append(kv.Key).Append(" = ").AppendLine(kv.Value);
                }
                File.WriteAllText(FilePath, sb.ToString(), new UTF8Encoding(false));
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

        public void Reset()
        {
            string game = this["game_path"];
            values.Clear();
            foreach (var kv in Defaults)
            {
                values[kv.Key] = kv.Value;
            }
            values["game_path"] = game;
        }
    }
}
