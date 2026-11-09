using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text;
using System.Threading;
using System.Windows.Forms;

namespace AO2Launcher
{
    // The time played, per profile. The game does not count it: when the launcher starts the game it also
    // starts a tracker, a copy of this exe in %TEMP% (so an update can still replace the launcher itself),
    // that waits for the game's exit and adds the time to <launcher ini>.playtime once a minute. It is
    // there when the launcher window is closed (the default after Play) or never opened.
    // The file: "<profile>\t<seconds>" lines, the main profile is the empty name.
    static class Playtime
    {
        const string MutexName = "Local\\AO2LauncherPlaytime";
        static string storePath;

        // Next to the launcher's ini. The tracker runs from %TEMP%, where Settings would look next to the copy:
        // it gets the launcher's path as --track-store.
        static string StorePath
        {
            get
            {
                if (storePath == null)
                {
                    storePath = Path.ChangeExtension(new Settings().FilePath, ".playtime");
                    MigrateStray();
                }
                return storePath;
            }
        }

        // Trackers of 1.4.x wrote the time next to their copy in %TEMP%: it goes over to the real store once.
        static void MigrateStray()
        {
            try
            {
                string stray = Path.Combine(TrackerDir, "ArmyOfTwoLauncher.playtime");
                if (!File.Exists(stray) || string.Equals(Path.GetFullPath(stray), Path.GetFullPath(storePath), StringComparison.OrdinalIgnoreCase))
                {
                    return;
                }
                foreach (KeyValuePair<string, long> kv in ReadFile(stray))
                {
                    Add(kv.Key, kv.Value);
                }
                File.Delete(stray);
            }
            catch (Exception ex)
            {
                Log.Error("moving the time played from the tracker's folder", ex);
            }
        }

        static Dictionary<string, long> Read()
        {
            return ReadFile(StorePath);
        }

        static Dictionary<string, long> ReadFile(string path)
        {
            var map = new Dictionary<string, long>();
            try
            {
                if (File.Exists(path))
                {
                    foreach (string line in File.ReadAllLines(path, Encoding.UTF8))
                    {
                        int tab = line.LastIndexOf('\t');
                        long secs;
                        if (tab >= 0 && long.TryParse(line.Substring(tab + 1), NumberStyles.Integer, CultureInfo.InvariantCulture, out secs))
                        {
                            map[line.Substring(0, tab)] = secs;
                        }
                    }
                }
            }
            catch (IOException)
            {
            }
            return map;
        }

        // Seconds played with a profile.
        public static long Seconds(string profile)
        {
            long secs;
            return Read().TryGetValue(profile ?? "", out secs) ? secs : 0;
        }

        static void Add(string profile, long secs)
        {
            if (secs <= 0)
            {
                return;
            }
            bool owned = false;
            using (var mutex = new Mutex(false, MutexName))
            {
                try
                {
                    try
                    {
                        owned = mutex.WaitOne(5000);
                    }
                    catch (AbandonedMutexException)
                    {
                        owned = true;
                    }
                    Dictionary<string, long> map = Read();
                    long old;
                    map.TryGetValue(profile, out old);
                    map[profile] = old + secs;
                    var sb = new StringBuilder();
                    foreach (KeyValuePair<string, long> kv in map)
                    {
                        sb.Append(kv.Key).Append('\t').Append(kv.Value.ToString(CultureInfo.InvariantCulture)).Append("\r\n");
                    }
                    string tmp = StorePath + ".tmp";
                    File.WriteAllText(tmp, sb.ToString(), new UTF8Encoding(false));
                    if (File.Exists(StorePath))
                    {
                        File.Delete(StorePath);
                    }
                    File.Move(tmp, StorePath);
                }
                catch (Exception ex)
                {
                    Log.Error("saving the time played", ex);
                }
                finally
                {
                    if (owned)
                    {
                        mutex.ReleaseMutex();
                    }
                }
            }
        }

        // "42 min", "12.5 h".
        public static string Format(long secs)
        {
            if (secs < 3600)
            {
                return L.F("{0} min", (secs / 60).ToString(CultureInfo.InvariantCulture));
            }
            return L.F("{0} h", (secs / 3600.0).ToString("0.0", CultureInfo.InvariantCulture));
        }

        static string TrackerDir
        {
            get { return Path.Combine(Path.GetTempPath(), "AO2LauncherTracker"); }
        }

        // The game with this id has just started with this profile: counts its time.
        public static void Track(int pid, string profile)
        {
            try
            {
                string dir = TrackerDir;
                Directory.CreateDirectory(dir);
                foreach (string old in Directory.GetFiles(dir, "tracker_*.exe"))
                {
                    try
                    {
                        File.Delete(old); // a running tracker's copy refuses
                    }
                    catch
                    {
                    }
                }
                string copy = Path.Combine(dir, "tracker_" + pid.ToString(CultureInfo.InvariantCulture) + ".exe");
                File.Copy(Application.ExecutablePath, copy, true);
                Process.Start(new ProcessStartInfo(copy,
                    "--track=" + pid.ToString(CultureInfo.InvariantCulture) + " \"--track-profile=" + profile + "\"" +
                    " \"--track-store=" + StorePath + "\"")
                {
                    UseShellExecute = false,
                    CreateNoWindow = true,
                });
            }
            catch (Exception ex)
            {
                Log.Error("starting the play time tracker", ex);
            }
        }

        // --track=<pid> --track-profile=<name>: no window; waits for the game and adds its time.
        public static void RunTracker(int pid, string profile, string store)
        {
            if (!string.IsNullOrEmpty(store))
            {
                storePath = store;
            }
            Process p;
            try
            {
                p = Process.GetProcessById(pid);
            }
            catch (ArgumentException)
            {
                return;
            }
            var sw = Stopwatch.StartNew();
            long credited = 0;
            while (!p.WaitForExit(60000))
            {
                long now = (long)sw.Elapsed.TotalSeconds;
                Add(profile, now - credited);
                credited = now;
            }
            Add(profile, (long)sw.Elapsed.TotalSeconds - credited);
        }
    }
}
