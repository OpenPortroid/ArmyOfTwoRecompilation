using System;
using System.IO;
using System.Net;
using System.Text;
using System.Text.RegularExpressions;

namespace AO2Launcher
{
    // Update check: the latest release on GitHub against the version of this
    // package. Version is bumped with every release (the tag is "v" + Version).
    static class Updates
    {
        public const string Version = "1.0";
        // The version compared with the latest release: Version, or --pretend-version=<ver> (debug: test an update).
        public static string Current = Version;
        public const string Repo = "OpenPortroid/ArmyOfTwoRecompilation";
        public static readonly string ReleasesPage = "https://github.com/" + Repo + "/releases/latest";

        // Debug: --latest=<tag> takes this as the latest release instead of asking GitHub.
        public static string Override;

        public sealed class Release
        {
            public string Version;  // "1.1" (the tag without the leading v)
            public string Url;      // the .zip asset, else the release page
        }

        // The latest release, or null when GitHub could not be reached. Blocks: call off the UI thread.
        public static Release Latest()
        {
            if (Override != null)
            {
                return new Release { Version = Override.TrimStart('v', 'V'), Url = ReleasesPage };
            }
            try
            {
                // .NET Framework 4.x does not offer TLS 1.2 by default, GitHub needs it.
                ServicePointManager.SecurityProtocol |= (SecurityProtocolType)3072;
                var req = (HttpWebRequest)WebRequest.Create("https://api.github.com/repos/" + Repo + "/releases/latest");
                req.UserAgent = "ArmyOfTwoLauncher/" + Current;
                req.Accept = "application/vnd.github+json";
                req.Timeout = 6000;
                req.ReadWriteTimeout = 6000;
                string json;
                using (var resp = (HttpWebResponse)req.GetResponse())
                using (var rd = new StreamReader(resp.GetResponseStream(), Encoding.UTF8))
                {
                    json = rd.ReadToEnd();
                }
                Match tag = Regex.Match(json, "\"tag_name\"\\s*:\\s*\"([^\"]+)\"");
                if (!tag.Success)
                {
                    return null;
                }
                Match zip = Regex.Match(json, "\"browser_download_url\"\\s*:\\s*\"([^\"]+\\.zip)\"");
                return new Release
                {
                    Version = tag.Groups[1].Value.TrimStart('v', 'V'),
                    Url = zip.Success ? zip.Groups[1].Value : "https://github.com/" + Repo + "/releases/tag/" + tag.Groups[1].Value,
                };
            }
            catch
            {
                return null;
            }
        }

        // Whether version a is newer than b ("1.10" > "1.9"); tags that are not numbers count as newer when they differ.
        public static bool Newer(string a, string b)
        {
            Version va, vb;
            if (System.Version.TryParse(Pad(a), out va) && System.Version.TryParse(Pad(b), out vb))
            {
                return va > vb;
            }
            return !string.Equals(a, b, StringComparison.OrdinalIgnoreCase);
        }

        static string Pad(string v)
        {
            return v.IndexOf('.') < 0 ? v + ".0" : v;
        }

        // ------------------------------------------------------------------
        // Self-update: the release archive is downloaded to update.tmp next to the launcher, unpacked
        // there, checked, and only then copied over the package. The running launcher cannot be
        // overwritten but can be renamed: it becomes ArmyOfTwoLauncher.exe.old, removed on the next start.

        public static string Dir
        {
            get { return Path.GetDirectoryName(System.Windows.Forms.Application.ExecutablePath); }
        }

        static string TempDir
        {
            get { return Path.Combine(Dir, "update.tmp"); }
        }

        static string OldExe
        {
            get { return Path.Combine(Dir, "ArmyOfTwoLauncher.exe.old"); }
        }

        // The package can update itself: the release has an archive and the launcher sits next to the game
        // (a release layout, not a launcher started from elsewhere).
        public static bool CanSelfUpdate(Release r)
        {
            return r != null && r.Url.EndsWith(".zip", StringComparison.OrdinalIgnoreCase) &&
                File.Exists(Path.Combine(Dir, "armyoftworecomp.exe"));
        }

        public static bool LeftOver
        {
            get { return File.Exists(OldExe) || Directory.Exists(TempDir); }
        }

        // Leftovers of the previous update.
        public static void CleanUp()
        {
            try
            {
                if (File.Exists(OldExe))
                {
                    File.Delete(OldExe);
                }
            }
            catch
            {
            }
            try
            {
                if (Directory.Exists(TempDir))
                {
                    Directory.Delete(TempDir, true);
                }
            }
            catch
            {
            }
        }

        // Downloads the release archive; progress(done, total bytes; total -1 when unknown). Returns the file.
        public static string Download(Release r, Action<long, long> progress, Func<bool> cancel)
        {
            Directory.CreateDirectory(TempDir);
            string file = Path.Combine(TempDir, "update.zip");
            ServicePointManager.SecurityProtocol |= (SecurityProtocolType)3072;
            var req = (HttpWebRequest)WebRequest.Create(r.Url);
            req.UserAgent = "ArmyOfTwoLauncher/" + Current;
            req.Timeout = 15000;
            req.ReadWriteTimeout = 30000;
            using (var resp = (HttpWebResponse)req.GetResponse())
            using (Stream src = resp.GetResponseStream())
            using (var dst = new FileStream(file, FileMode.Create, FileAccess.Write))
            {
                long total = resp.ContentLength, done = 0;
                var buf = new byte[1 << 16];
                int n;
                progress(0, total);
                while ((n = src.Read(buf, 0, buf.Length)) > 0)
                {
                    if (cancel())
                    {
                        throw new OperationCanceledException();
                    }
                    dst.Write(buf, 0, n);
                    done += n;
                    progress(done, total);
                }
                if (total > 0 && done != total)
                {
                    throw new IOException("the download was cut short");
                }
            }
            return file;
        }

        // Unpacks the archive and copies it over the package; progress(0..1). The user's settings\*.toml
        // (key bindings, edited options) are kept; everything else is replaced.
        public static void Install(string zipFile, Action<double> progress)
        {
            string dir = Dir;
            string stage = Path.Combine(TempDir, "new");
            if (Directory.Exists(stage))
            {
                Directory.Delete(stage, true);
            }
            string stageFull = Path.GetFullPath(stage) + Path.DirectorySeparatorChar;

            var files = new System.Collections.Generic.List<string>(); // relative paths
            long total = 0, done = 0;
            using (System.IO.Compression.ZipArchive zip = System.IO.Compression.ZipFile.OpenRead(zipFile))
            {
                // The archive holds one folder (ArmyOfTwo\...): its contents are the package.
                string root = null;
                foreach (var e in zip.Entries)
                {
                    string name = e.FullName.Replace('\\', '/');
                    int slash = name.IndexOf('/');
                    string first = slash < 0 ? "" : name.Substring(0, slash + 1);
                    root = root == null ? first : root == first ? root : "";
                    total += e.Length;
                }
                root = root ?? "";
                total = Math.Max(1, total) * 2; // unpacking, then copying
                foreach (var e in zip.Entries)
                {
                    string rel = e.FullName.Replace('\\', '/').Substring(root.Length);
                    if (rel.Length == 0 || rel.EndsWith("/"))
                    {
                        continue;
                    }
                    string dst = Path.GetFullPath(Path.Combine(stage, rel.Replace('/', Path.DirectorySeparatorChar)));
                    if (!dst.StartsWith(stageFull, StringComparison.OrdinalIgnoreCase))
                    {
                        throw new IOException("bad path in the archive: " + e.FullName);
                    }
                    Directory.CreateDirectory(Path.GetDirectoryName(dst));
                    using (Stream s = e.Open())
                    using (var f = new FileStream(dst, FileMode.Create, FileAccess.Write))
                    {
                        s.CopyTo(f);
                    }
                    files.Add(dst.Substring(stageFull.Length));
                    done += e.Length;
                    progress((double)done / total);
                }
            }
            string exeName = Path.GetFileName(System.Windows.Forms.Application.ExecutablePath);
            if (!File.Exists(Path.Combine(stage, "armyoftworecomp.exe")) || !File.Exists(Path.Combine(stage, "ArmyOfTwoLauncher.exe")))
            {
                throw new IOException("the archive is not a release of the port");
            }

            foreach (string rel in files)
            {
                string src = Path.Combine(stage, rel);
                string dst = Path.Combine(dir, rel);
                long len = new FileInfo(src).Length;
                bool userSettings = rel.StartsWith("settings" + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) &&
                    rel.EndsWith(".toml", StringComparison.OrdinalIgnoreCase);
                if (!(userSettings && File.Exists(dst)))
                {
                    Directory.CreateDirectory(Path.GetDirectoryName(dst));
                    if (string.Equals(rel, "ArmyOfTwoLauncher.exe", StringComparison.OrdinalIgnoreCase) &&
                        string.Equals(rel, exeName, StringComparison.OrdinalIgnoreCase))
                    {
                        if (File.Exists(OldExe))
                        {
                            File.Delete(OldExe);
                        }
                        File.Move(dst, OldExe);
                        try
                        {
                            File.Copy(src, dst, true);
                        }
                        catch
                        {
                            // Never leave the package without its launcher.
                            File.Move(OldExe, dst);
                            throw;
                        }
                    }
                    else
                    {
                        File.Copy(src, dst, true);
                    }
                }
                done += len;
                progress((double)done / total);
            }
            progress(1);
            // The archive and the unpacked copy are not needed any more (the running launcher's .old stays
            // until the next start).
            CleanUp();
        }
    }
}
