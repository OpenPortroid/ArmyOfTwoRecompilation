using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Threading;

namespace AO2Launcher
{
    sealed class InstallException : Exception
    {
        public InstallException(string message) : base(message)
        {
        }
    }

    // Something the user picked: a disc image, a content package or a folder
    // with the extracted game.
    sealed class InstallSource
    {
        public string Path;
        public string Kind;      // game, tu, dlc
        public bool Folder;
        public uint Title;
        public string Name = "";
        public long Bytes;

        public string Caption
        {
            get
            {
                string n = Name.Length > 0 ? Name : System.IO.Path.GetFileName(Path.TrimEnd('\\', '/'));
                return n + " · " + Installer.Size(Bytes);
            }
        }
    }

    // Puts the user's own copy of the game where the game looks for it:
    // <game folder>\assets (disc files + default.xexp of Title Update 2) and
    // <game folder>\dlc\<package> (downloadable content).
    // Images and packages are read by ao2_install.exe (next to the game).
    static class Installer
    {
        public const string ToolName = "ao2_install.exe";
        // The executable this build was recompiled from: the European disc
        // (title 4541084C) patched by Title Update 2.
        const string XexSha1 = "ca5c53e21b93ca2a3391fe0b1a190ee954ac6d66";
        const string XexpSha1 = "a1347695439c0f14dda26dc9bf33eb1ead2b78e0";
        static readonly uint[] DlcTitles = { 0x454107F8, 0x4541084C };
        const string SkipDir = "$SystemUpdate";

        public static string Size(long bytes)
        {
            return bytes >= 1L << 30 ? (bytes / (double)(1L << 30)).ToString("0.0", CultureInfo.InvariantCulture) + L.T(" GB")
                : (bytes / (double)(1L << 20)).ToString("0", CultureInfo.InvariantCulture) + L.T(" MB");
        }

        // Where the game keeps settings\, localization\ and assets\, as the game itself
        // finds it (utils::RepoRoot): the folder of the executable in a release, the
        // repository (aot\) for a build in out\build\<preset>.
        public static string DataRoot(string gameDir)
        {
            try
            {
                var dir = new DirectoryInfo(gameDir);
                for (int i = 0; i < 6 && dir != null; i++, dir = dir.Parent)
                {
                    if (File.Exists(Path.Combine(dir.FullName, "armyoftworecomp_manifest.toml")) ||
                        Directory.Exists(Path.Combine(dir.FullName, "settings")))
                    {
                        return dir.FullName;
                    }
                }
            }
            catch
            {
            }
            return gameDir;
        }

        public static string AssetsDir(string gameDir)
        {
            return Path.Combine(DataRoot(gameDir), "assets");
        }

        static string Sha1(string file)
        {
            using (var sha = SHA1.Create())
            using (var st = File.OpenRead(file))
            {
                return string.Concat(sha.ComputeHash(st).Select(b => b.ToString("x2")));
            }
        }

        static bool HashIs(string file, string sha1)
        {
            try
            {
                return File.Exists(file) && Sha1(file) == sha1;
            }
            catch
            {
                return false;
            }
        }

        public static bool GameInstalled(string gameDir)
        {
            string a = AssetsDir(gameDir);
            return Directory.Exists(Path.Combine(a, "AO2Game", "CookedXenon")) && HashIs(Path.Combine(a, "default.xex"), XexSha1);
        }

        public static bool UpdateInstalled(string gameDir)
        {
            return HashIs(Path.Combine(AssetsDir(gameDir), "default.xexp"), XexpSha1);
        }

        // Where the game reads downloadable content (aot\src\dlc_folder.cpp).
        public static string DlcRoot(string gameDir)
        {
            return Path.Combine(DataRoot(gameDir), "dlc");
        }

        // Where earlier versions put it, in the main profile's user data folder.
        static string OldDlcRoot()
        {
            return Path.Combine(Profiles.MainRoot, "0000000000000000", "4541084C", "00000002");
        }

        public static int InstalledDlcCount(string gameDir)
        {
            try
            {
                if (gameDir == null)
                {
                    return 0;
                }
                string root = DlcRoot(gameDir);
                return Directory.Exists(root)
                    ? Directory.GetDirectories(root).Count(d => !d.EndsWith(".tmp") && Directory.EnumerateFileSystemEntries(d).Any())
                    : 0;
            }
            catch
            {
                return 0;
            }
        }

        static string Tool(string gameDir)
        {
            string tool = Path.Combine(gameDir, ToolName);
            if (!File.Exists(tool))
            {
                throw new InstallException(L.F("{0} is missing from the game folder", ToolName));
            }
            return tool;
        }

        // Runs the tool; every output line goes to onLine. Returns the error text or null.
        static string RunTool(string gameDir, string args, Action<string> onLine, Func<bool> cancelled)
        {
            var psi = new ProcessStartInfo(Tool(gameDir), args)
            {
                WorkingDirectory = gameDir,
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                StandardOutputEncoding = Encoding.UTF8,
            };
            string error = null;
            using (Process p = Process.Start(psi))
            {
                string line;
                while ((line = p.StandardOutput.ReadLine()) != null)
                {
                    if (cancelled != null && cancelled())
                    {
                        try
                        {
                            p.Kill();
                        }
                        catch
                        {
                        }
                        throw new OperationCanceledException();
                    }
                    if (line.StartsWith("ERROR "))
                    {
                        error = line.Substring(6);
                    }
                    else if (onLine != null)
                    {
                        onLine(line);
                    }
                }
                p.WaitForExit();
                if (p.ExitCode != 0 && error == null)
                {
                    error = "ao2_install: exit code " + p.ExitCode;
                }
            }
            return error;
        }

        static string ErrorText(string toolError)
        {
            switch (toolError)
            {
                case "not a disc image or a content package": return L.T("not an Xbox 360 disc image or a TU / DLC package");
                case "damaged disc image": return L.T("the disc image is damaged");
                case "damaged content package": return L.T("the package is damaged");
                case "not a file": return L.T("file not found");
            }
            return toolError;
        }

        // What is this file or folder? Throws InstallException with a text for the user.
        public static InstallSource Probe(string gameDir, string path)
        {
            if (Directory.Exists(path) || string.Equals(Path.GetFileName(path), "default.xex", StringComparison.OrdinalIgnoreCase))
            {
                string dir = Directory.Exists(path) ? path : Path.GetDirectoryName(path);
                if (!File.Exists(Path.Combine(dir, "default.xex")) || !Directory.Exists(Path.Combine(dir, "AO2Game")))
                {
                    throw new InstallException(L.T("This folder has no extracted game (default.xex and AO2Game)"));
                }
                long bytes = 0;
                foreach (string f in GameFiles(dir))
                {
                    bytes += new FileInfo(f).Length;
                }
                return new InstallSource { Path = dir, Kind = "game", Folder = true, Bytes = bytes };
            }

            var info = new Dictionary<string, string>();
            string error = RunTool(gameDir, "probe " + Game.Quote(path), line =>
            {
                int eq = line.IndexOf('=');
                if (eq > 0)
                {
                    info[line.Substring(0, eq)] = line.Substring(eq + 1);
                }
            }, null);
            if (error != null)
            {
                throw new InstallException(Path.GetFileName(path) + ": " + ErrorText(error));
            }
            var src = new InstallSource { Path = path };
            string v;
            src.Kind = info.TryGetValue("KIND", out v) ? v : "unknown";
            src.Name = info.TryGetValue("NAME", out v) ? v.Trim() : "";
            if (info.TryGetValue("TITLE", out v))
            {
                uint.TryParse(v, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out src.Title);
            }
            if (info.TryGetValue("BYTES", out v))
            {
                long.TryParse(v, out src.Bytes);
            }
            if (src.Kind == "unknown")
            {
                throw new InstallException(L.F("{0}: no game on this disc (default.xex)", Path.GetFileName(path)));
            }
            if (src.Kind == "dlc" && !DlcTitles.Contains(src.Title))
            {
                throw new InstallException(L.F("{0}: this package belongs to another game ({1})", Path.GetFileName(path), src.Title.ToString("X8")));
            }
            return src;
        }

        static IEnumerable<string> GameFiles(string dir)
        {
            foreach (string f in Directory.GetFiles(dir))
            {
                yield return f;
            }
            foreach (string d in Directory.GetDirectories(dir))
            {
                if (string.Equals(Path.GetFileName(d), SkipDir, StringComparison.OrdinalIgnoreCase))
                {
                    continue;
                }
                foreach (string f in Directory.EnumerateFiles(d, "*", SearchOption.AllDirectories))
                {
                    yield return f;
                }
            }
        }

        static void CheckSpace(string dir, long need)
        {
            try
            {
                var drive = new DriveInfo(Path.GetPathRoot(Path.GetFullPath(dir)));
                if (drive.AvailableFreeSpace < need + (64L << 20))
                {
                    throw new InstallException(L.F("Not enough space on {0}: {1} needed, {2} free",
                        drive.Name, Size(need), Size(drive.AvailableFreeSpace)));
                }
            }
            catch (InstallException)
            {
                throw;
            }
            catch
            {
                // Network path etc.: the copy itself will tell.
            }
        }

        static void DeleteDir(string dir)
        {
            try
            {
                if (Directory.Exists(dir))
                {
                    Directory.Delete(dir, true);
                }
            }
            catch
            {
            }
        }

        // Moves everything under `from` into `to`, replacing files that are there.
        static void MoveInto(string from, string to)
        {
            if (!Directory.Exists(to))
            {
                Directory.Move(from, to);
                return;
            }
            foreach (string f in Directory.GetFiles(from))
            {
                string dest = Path.Combine(to, Path.GetFileName(f));
                if (File.Exists(dest))
                {
                    File.Delete(dest);
                }
                File.Move(f, dest);
            }
            foreach (string d in Directory.GetDirectories(from))
            {
                MoveInto(d, Path.Combine(to, Path.GetFileName(d)));
            }
            DeleteDir(from);
        }

        static void Extract(string gameDir, string source, string dest, string options, Action<long> progress, Func<bool> cancelled)
        {
            string error = RunTool(gameDir, "extract " + Game.Quote(source) + " " + Game.Quote(dest) + options, line =>
            {
                if (line.StartsWith("PROGRESS "))
                {
                    string[] p = line.Split(' ');
                    long done;
                    if (p.Length >= 2 && long.TryParse(p[1], out done))
                    {
                        progress(done);
                    }
                }
            }, cancelled);
            if (error != null)
            {
                throw new InstallException(ErrorText(error));
            }
        }

        static void CopyFolder(string from, string to, Action<long> progress, Func<bool> cancelled)
        {
            long done = 0;
            var buffer = new byte[4 << 20];
            foreach (string f in GameFiles(from))
            {
                string dest = Path.Combine(to, f.Substring(from.TrimEnd('\\', '/').Length + 1));
                Directory.CreateDirectory(Path.GetDirectoryName(dest));
                using (var src = File.OpenRead(f))
                using (var dst = File.Create(dest))
                {
                    int n;
                    while ((n = src.Read(buffer, 0, buffer.Length)) > 0)
                    {
                        if (cancelled())
                        {
                            throw new OperationCanceledException();
                        }
                        dst.Write(buffer, 0, n);
                        done += n;
                        progress(done);
                    }
                }
            }
        }

        // The whole installation; `report` gets the step's name and the overall progress 0..1.
        // Throws InstallException (text for the user) or OperationCanceledException.
        public static void Run(string gameDir, InstallSource game, InstallSource update, IList<InstallSource> dlc,
            Action<string, double> report, Func<bool> cancelled)
        {
            string assets = AssetsDir(gameDir);
            string temp = assets + ".tmp";
            long total = (game != null ? game.Bytes : 0) + (update != null ? update.Bytes : 0) + dlc.Sum(d => d.Bytes);
            long before = 0;
            Func<string, Action<long>> step = name => done => report(name, total > 0 ? Math.Min(1.0, (before + done) / (double)total) : 0);
            try
            {
                if (game != null)
                {
                    CheckSpace(gameDir, game.Bytes);
                    DeleteDir(temp);
                    Directory.CreateDirectory(temp);
                    report(L.T("Checking the game version"), 0);
                    string xex = Path.Combine(game.Folder ? game.Path : temp, "default.xex");
                    if (!game.Folder)
                    {
                        Extract(gameDir, game.Path, temp, " --only=default.xex", d => { }, cancelled);
                    }
                    if (!HashIs(xex, XexSha1))
                    {
                        throw new InstallException(L.T("This version of the game is not supported: the European release of Army of Two for Xbox 360 is needed (4541084C)"));
                    }
                    if (game.Folder)
                    {
                        CopyFolder(game.Path, temp, step(L.T("Copying the game files")), cancelled);
                    }
                    else
                    {
                        Extract(gameDir, game.Path, temp, " " + Game.Quote("--skip=" + SkipDir), step(L.T("Extracting the game")), cancelled);
                    }
                    MoveInto(temp, assets);
                    before += game.Bytes;
                }

                if (update != null)
                {
                    DeleteDir(temp);
                    Directory.CreateDirectory(temp);
                    Extract(gameDir, update.Path, temp, " --only=default.xexp", step("Title Update 2"), cancelled);
                    if (!HashIs(Path.Combine(temp, "default.xexp"), XexpSha1))
                    {
                        throw new InstallException(L.T("This is a different update: Title Update 2 of the European release is needed (TU_12K222C_…)"));
                    }
                    Directory.CreateDirectory(assets);
                    MoveInto(temp, assets);
                    before += update.Bytes;
                }

                foreach (InstallSource d in dlc)
                {
                    string dest = Path.Combine(DlcRoot(gameDir), Path.GetFileName(d.Path));
                    string part = dest + ".tmp";
                    Directory.CreateDirectory(DlcRoot(gameDir));
                    CheckSpace(DlcRoot(gameDir), d.Bytes);
                    DeleteDir(part);
                    try
                    {
                        Extract(gameDir, d.Path, part, "", step("DLC: " + (d.Name.Length > 0 ? d.Name : Path.GetFileName(d.Path))), cancelled);
                        DeleteDir(dest);
                        Directory.Move(part, dest);
                    }
                    finally
                    {
                        DeleteDir(part);
                    }
                    before += d.Bytes;
                }

                if (!GameInstalled(gameDir))
                {
                    throw new InstallException(L.T("The game files are not installed: choose a game image"));
                }
                if (!UpdateInstalled(gameDir))
                {
                    throw new InstallException(L.T("Title Update 2 is not installed: choose the update file"));
                }
            }
            catch (IOException ex)
            {
                throw new InstallException(ex.Message);
            }
            catch (UnauthorizedAccessException ex)
            {
                throw new InstallException(ex.Message);
            }
            finally
            {
                DeleteDir(temp);
            }
        }

        // Whether there are packages at the old place to move into the game folder.
        public static bool OldDlcLeft()
        {
            try
            {
                string old = OldDlcRoot();
                return Directory.Exists(old) && Directory.GetDirectories(old).Any(d => !d.EndsWith(".tmp"));
            }
            catch
            {
                return false;
            }
        }

        // Moves the packages from the old place into <game folder>\dlc and removes the profiles' links
        // to the old place. A package in dlc\ is always whole (moved there by a rename): one already
        // there wins and the old copy is deleted. The game does the same when it starts
        // (dlc_folder.cpp); the mutex keeps the two from moving at once.
        // Returns how many were moved; throws InstallException if one could not be.
        public static int MoveOldDlc(string gameDir)
        {
            int moved = 0;
            string failed = null;
            using (var mutex = new Mutex(false, "AO2_DlcMove"))
            {
                try
                {
                    mutex.WaitOne();
                }
                catch (AbandonedMutexException)
                {
                }
                try
                {
                    string old = OldDlcRoot(), root = DlcRoot(gameDir);
                    if (Directory.Exists(old))
                    {
                        foreach (string from in Directory.GetDirectories(old))
                        {
                            string to = Path.Combine(root, Path.GetFileName(from));
                            if (from.EndsWith(".tmp"))
                            {
                                continue;
                            }
                            try
                            {
                                if (Directory.Exists(to))
                                {
                                    DeleteEntry(from); // installed again, or a move cut short after the rename
                                }
                                else
                                {
                                    Directory.CreateDirectory(root);
                                    MovePackage(from, to);
                                }
                                moved++;
                            }
                            catch (Exception ex)
                            {
                                failed = Path.GetFileName(from) + ": " + ex.Message;
                            }
                        }
                        RemoveEmpty(old, Profiles.MainRoot);
                    }
                    Profiles.RemoveDlcLinks();
                }
                finally
                {
                    mutex.ReleaseMutex();
                }
            }
            if (failed != null)
            {
                throw new InstallException(failed);
            }
            return moved;
        }

        // Renames on the same drive; else copies to <to>.tmp, renames that and deletes the old one.
        static void MovePackage(string from, string to)
        {
            // Directory.Move refuses different drive letters itself; a folder mounted from another
            // volume fails in Windows with ERROR_NOT_SAME_DEVICE.
            if (string.Equals(Path.GetPathRoot(Path.GetFullPath(from)), Path.GetPathRoot(Path.GetFullPath(to)),
                    StringComparison.OrdinalIgnoreCase))
            {
                try
                {
                    Directory.Move(from, to);
                    return;
                }
                catch (IOException ex)
                {
                    if ((ex.HResult & 0xFFFF) != 17)
                    {
                        throw;
                    }
                }
            }
            string part = to + ".tmp";
            DeleteDir(part);
            try
            {
                long bytes = new DirectoryInfo(from).EnumerateFiles("*", SearchOption.AllDirectories).Sum(f => f.Length);
                CheckSpace(Path.GetDirectoryName(to), bytes);
                foreach (string f in Directory.EnumerateFiles(from, "*", SearchOption.AllDirectories))
                {
                    string dest = Path.Combine(part, f.Substring(from.Length + 1));
                    Directory.CreateDirectory(Path.GetDirectoryName(dest));
                    File.Copy(f, dest);
                }
                Directory.Move(part, to);
            }
            finally
            {
                DeleteDir(part);
            }
            DeleteEntry(from);
        }

        // Removes dir and its parents up to (not including) stop while they are empty.
        static void RemoveEmpty(string dir, string stop)
        {
            try
            {
                while (dir != null && !string.Equals(dir.TrimEnd('\\'), stop.TrimEnd('\\'), StringComparison.OrdinalIgnoreCase) &&
                       Directory.Exists(dir) && !Directory.EnumerateFileSystemEntries(dir).Any())
                {
                    Directory.Delete(dir, false);
                    dir = Path.GetDirectoryName(dir);
                }
            }
            catch
            {
            }
        }

        // What the release itself puts in assets\ (not the user's game files).
        static readonly string[] ShippedAssets = { "README.md", "icon.png" };

        static IEnumerable<string> InstalledEntries(string gameDir)
        {
            string assets = AssetsDir(gameDir);
            if (!Directory.Exists(assets))
            {
                return new string[0];
            }
            return Directory.GetFileSystemEntries(assets)
                .Where(e => !ShippedAssets.Contains(Path.GetFileName(e), StringComparer.OrdinalIgnoreCase));
        }

        // Whether assets\ has anything installed (the game, the update, or a broken installation).
        public static bool AnythingInstalled(string gameDir)
        {
            try
            {
                return gameDir != null && InstalledEntries(gameDir).Any();
            }
            catch
            {
                return false;
            }
        }

        public static long InstalledBytes(string gameDir)
        {
            long bytes = 0;
            try
            {
                foreach (string e in InstalledEntries(gameDir))
                {
                    if (File.Exists(e))
                    {
                        bytes += new FileInfo(e).Length;
                    }
                    else if ((File.GetAttributes(e) & FileAttributes.ReparsePoint) == 0)
                    {
                        bytes += new DirectoryInfo(e).EnumerateFiles("*", SearchOption.AllDirectories).Sum(f => f.Length);
                    }
                }
            }
            catch
            {
            }
            return bytes;
        }

        // Deletes the installed game: everything in assets\ but the release's own files. A linked folder
        // (a junction to the files elsewhere) is unlinked, not emptied.
        public static void Uninstall(string gameDir)
        {
            foreach (string e in InstalledEntries(gameDir).ToList())
            {
                DeleteEntry(e);
            }
            DeleteDir(AssetsDir(gameDir) + ".tmp");
        }

        static void DeleteEntry(string path)
        {
            FileAttributes a = File.GetAttributes(path);
            if ((a & FileAttributes.Directory) == 0)
            {
                File.SetAttributes(path, FileAttributes.Normal); // files from a disc can be read-only
                File.Delete(path);
                return;
            }
            if ((a & FileAttributes.ReparsePoint) == 0)
            {
                foreach (string e in Directory.GetFileSystemEntries(path))
                {
                    DeleteEntry(e);
                }
            }
            Directory.Delete(path, false);
        }

        // "Army of Two.lnk" on the desktop, pointing at the launcher.
        public static bool CreateDesktopShortcut()
        {
            try
            {
                string exe = Assembly.GetExecutingAssembly().Location;
                string lnk = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), "Army of Two.lnk");
                Type t = Type.GetTypeFromProgID("WScript.Shell");
                object shell = Activator.CreateInstance(t);
                object sc = t.InvokeMember("CreateShortcut", BindingFlags.InvokeMethod, null, shell, new object[] { lnk });
                Type st = sc.GetType();
                st.InvokeMember("TargetPath", BindingFlags.SetProperty, null, sc, new object[] { exe });
                st.InvokeMember("WorkingDirectory", BindingFlags.SetProperty, null, sc, new object[] { Path.GetDirectoryName(exe) });
                st.InvokeMember("IconLocation", BindingFlags.SetProperty, null, sc, new object[] { exe + ",0" });
                st.InvokeMember("Description", BindingFlags.SetProperty, null, sc, new object[] { "Army of Two" });
                st.InvokeMember("Save", BindingFlags.InvokeMethod, null, sc, null);
                return File.Exists(lnk);
            }
            catch
            {
                return false;
            }
        }
    }
}
