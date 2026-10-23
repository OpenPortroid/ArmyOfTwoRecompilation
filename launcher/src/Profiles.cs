using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;

namespace AO2Launcher
{
    // Launcher profiles: each is a user data folder of its own for the game (--user_data_root), so each
    // has its own saves, game profile (settings, money, unlocks) and achievements. The main profile is the
    // game's usual folder, Documents\armyoftworecomp_tu2; the others are folders under
    // Documents\armyoftworecomp_profiles. The downloadable content is in the game folder (dlc\), the same
    // for every profile, and the shader cache is shared.
    static class Profiles
    {
        public const string Main = ""; // the "profile" setting of the main profile
        const string DlcDir = "0000000000000000";

        static string Docs
        {
            get { return Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments); }
        }

        public static string MainRoot
        {
            get { return Path.Combine(Docs, "armyoftworecomp_tu2"); }
        }

        public static string ProfilesDir
        {
            get { return Path.Combine(Docs, "armyoftworecomp_profiles"); }
        }

        // Where a profile goes when Documents can't be written (Controlled Folder Access, a OneDrive
        // Documents that refuses new folders: Windows answers "file not found" there).
        static string LocalProfilesDir
        {
            get
            {
                return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                    "armyoftworecomp_profiles");
            }
        }

        static IEnumerable<string> Dirs
        {
            get { return new[] { ProfilesDir, LocalProfilesDir }; }
        }

        // The user data folder of a profile: where it is, Documents for a new one.
        public static string Root(string name)
        {
            if (string.IsNullOrEmpty(name))
            {
                return MainRoot;
            }
            foreach (string dir in Dirs)
            {
                string root = Path.Combine(dir, name);
                if (Directory.Exists(root))
                {
                    return root;
                }
            }
            return Path.Combine(ProfilesDir, name);
        }

        // The main profile first, then the others by name.
        public static List<string> All()
        {
            var names = new List<string>();
            foreach (string dir in Dirs)
            {
                try
                {
                    if (Directory.Exists(dir))
                    {
                        names.AddRange(Directory.GetDirectories(dir).Select(Path.GetFileName));
                    }
                }
                catch
                {
                }
            }
            var list = new List<string> { Main };
            list.AddRange(names.Distinct(StringComparer.OrdinalIgnoreCase)
                .OrderBy(n => n, StringComparer.CurrentCultureIgnoreCase));
            return list;
        }

        public static bool Exists(string name)
        {
            return string.IsNullOrEmpty(name) || Directory.Exists(Root(name));
        }

        // A name that can be a folder; null if it cannot (the text for the user in error).
        public static string CheckName(string name, out string error)
        {
            error = null;
            name = (name ?? "").Trim().TrimEnd('.');
            if (name.Length == 0)
            {
                error = L.T("Enter a name for the profile");
                return null;
            }
            if (name.Length > 32 || name.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0 ||
                new[] { "CON", "PRN", "AUX", "NUL" }.Contains(name.ToUpperInvariant()))
            {
                error = L.T("This name cannot be used for a profile");
                return null;
            }
            if (All().Skip(1).Any(n => string.Equals(n, name, StringComparison.OrdinalIgnoreCase)))
            {
                error = L.F("A profile named {0} already exists", name);
                return null;
            }
            return name;
        }

        // The next free "Profile N".
        public static string SuggestName()
        {
            var names = new HashSet<string>(All(), StringComparer.OrdinalIgnoreCase);
            for (int i = 2; ; i++)
            {
                string n = L.F("Profile {0}", i);
                if (!names.Contains(n) && n.IndexOfAny(Path.GetInvalidFileNameChars()) < 0)
                {
                    return n;
                }
            }
        }

        public static void Create(string name)
        {
            string docs = Path.Combine(ProfilesDir, name);
            try
            {
                Directory.CreateDirectory(docs);
                return;
            }
            catch (Exception ex)
            {
                Log.Error("profile folder " + docs, ex);
            }
            Directory.CreateDirectory(Path.Combine(LocalProfilesDir, name));
        }

        // Earlier versions linked each profile's content folder (a junction) to the main one's, where
        // the DLC was: the links are removed, not what they point to.
        public static void RemoveDlcLinks()
        {
            foreach (string name in All().Skip(1))
            {
                try
                {
                    string link = Path.Combine(Root(name), DlcDir);
                    if (Directory.Exists(link) && (File.GetAttributes(link) & FileAttributes.ReparsePoint) != 0)
                    {
                        Directory.Delete(link, false);
                    }
                }
                catch
                {
                }
            }
        }

        // Deletes a profile's folder with its saves. A junction in it (an earlier version's DLC link) is
        // removed, not what it points to.
        public static void Delete(string name)
        {
            if (string.IsNullOrEmpty(name))
            {
                return;
            }
            string root = Root(name);
            if (!Directory.Exists(root))
            {
                return;
            }
            RemoveLinks(root);
            Directory.Delete(root, true);
        }

        // Junctions anywhere under dir: deleted as links first, so nothing deletes through them.
        static void RemoveLinks(string dir)
        {
            foreach (string d in Directory.GetDirectories(dir))
            {
                if ((File.GetAttributes(d) & FileAttributes.ReparsePoint) != 0)
                {
                    Directory.Delete(d, false);
                }
                else
                {
                    RemoveLinks(d);
                }
            }
        }

        // Deletes the saves and the game profile of a user data folder (not its DLC, not the cache).
        public static void DeleteSaves(string root)
        {
            if (!Directory.Exists(root))
            {
                return;
            }
            RemoveLinks(root);
            foreach (string d in Directory.GetDirectories(root))
            {
                string n = Path.GetFileName(d);
                if (n == DlcDir || string.Equals(n, "cache", StringComparison.OrdinalIgnoreCase))
                {
                    continue;
                }
                Directory.Delete(d, true);
            }
        }

        // ------------------------------------------------------------------
        // What is in a profile's saves

        public sealed class Summary
        {
            public string Map;   // the mission of the last checkpoint ("Iraq"), translated; null: none
            public long Money = -1; // -1: no game profile yet
        }

        public static Summary Read(string name)
        {
            var s = new Summary();
            string root = Root(name);
            try
            {
                s.Map = CheckpointMap(root);
            }
            catch
            {
            }
            try
            {
                s.Money = ProfileValue(root, 1000); // AO2ProfileSettings.Inventory_CashMoney
            }
            catch
            {
            }
            return s;
        }

        // The missions by the number their levels begin with (05_map_world: Iraq).
        static string MissionName(int n)
        {
            switch (n)
            {
                case 1: return L.T("Somalia");
                case 4: return L.T("Afghanistan");
                case 5: return L.T("Iraq");
                case 6: return L.T("Aircraft carrier");
                case 7: return L.T("China");
                case 8: return L.T("Miami");
                case 9: return L.T("Kiev");
                case 10: return L.T("Miami (alternate ending)");
            }
            return null;
        }

        // The last checkpoint: <xuid>\4541084C\00000001\default_checkpoint_tu2.sav\<file> (or the disc
        // version's default_checkpoint.sav) of any player; its first string is the level, e.g. 05_map_world.
        static string CheckpointMap(string root)
        {
            FileInfo newest = null;
            foreach (string user in Directory.GetDirectories(root))
            {
                string xuid = Path.GetFileName(user);
                if (xuid.Length != 16 || xuid == DlcDir)
                {
                    continue;
                }
                string saves = Path.Combine(user, "4541084C", "00000001");
                foreach (string sav in new[] { "default_checkpoint_tu2.sav", "default_checkpoint.sav" })
                {
                    string dir = Path.Combine(saves, sav);
                    if (!Directory.Exists(dir))
                    {
                        continue;
                    }
                    foreach (FileInfo f in new DirectoryInfo(dir).GetFiles())
                    {
                        if (newest == null || f.LastWriteTimeUtc > newest.LastWriteTimeUtc)
                        {
                            newest = f;
                        }
                    }
                    break; // the TU2 save if there is one
                }
            }
            if (newest == null)
            {
                return null;
            }
            byte[] d = File.ReadAllBytes(newest.FullName);
            // Find the first "NN_" level name (length-prefixed string).
            string text = Encoding.ASCII.GetString(d);
            for (int i = 0; i + 3 < text.Length; i++)
            {
                if (char.IsDigit(text[i]) && char.IsDigit(text[i + 1]) && text[i + 2] == '_' && text.IndexOf("_map_", i, Math.Min(8, text.Length - i), StringComparison.Ordinal) == i + 2)
                {
                    return MissionName((text[i] - '0') * 10 + (text[i + 1] - '0'));
                }
            }
            return null;
        }

        // A value of the game profile of player 1 (4541084C\profile\User): the title's profile settings
        // 63E83FFF, 63E83FFE, 63E83FFD in a row are a 20-byte hash, the size of the data (big-endian),
        // then the data compressed with LZO1X: a count, then per setting owner (byte), id, type (byte,
        // 1 = int), value, advertisement (byte), all big-endian. Throws if there is no such value.
        public static long ProfileValue(string root, int id)
        {
            string dir = Path.Combine(root, "4541084C", "profile", "User");
            var all = new List<byte>();
            foreach (string part in new[] { "63E83FFF", "63E83FFE", "63E83FFD" })
            {
                string f = Path.Combine(dir, part);
                if (File.Exists(f))
                {
                    all.AddRange(File.ReadAllBytes(f));
                }
            }
            byte[] blob = all.ToArray();
            if (blob.Length < 24)
            {
                throw new InvalidDataException();
            }
            int size = BE(blob, 20);
            byte[] data = Lzo1x(blob, 24, size);
            int count = BE(data, 0), p = 4;
            for (int i = 0; i < count && p + 11 <= data.Length; i++, p += 11)
            {
                if (data[p + 5] != 1)
                {
                    throw new InvalidDataException(); // only ints are expected
                }
                if (BE(data, p + 1) == id)
                {
                    return BE(data, p + 6);
                }
            }
            throw new InvalidDataException();
        }

        static int BE(byte[] b, int o)
        {
            return (b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3];
        }

        // LZO1X decompression (the format of lzo1x_decompress).
        static byte[] Lzo1x(byte[] src, int ip, int outLen)
        {
            var o = new byte[outLen];
            int op = 0, state = 0;
            Action<int> lit = n =>
            {
                Buffer.BlockCopy(src, ip, o, op, n);
                ip += n;
                op += n;
            };
            Action<int, int> copy = (dist, n) =>
            {
                int s = op - dist;
                if (s < 0)
                {
                    throw new InvalidDataException();
                }
                for (int i = 0; i < n; i++)
                {
                    o[op++] = o[s + i];
                }
            };
            if (src[ip] > 17)
            {
                int n = src[ip++] - 17;
                lit(n);
                state = n < 4 ? n : 4;
            }
            while (true)
            {
                int t = src[ip++], next;
                if (t < 16)
                {
                    if (state == 0)
                    {
                        if (t == 0)
                        {
                            while (src[ip] == 0)
                            {
                                t += 255;
                                ip++;
                            }
                            t += 15 + src[ip++];
                        }
                        lit(t + 3);
                        state = 4;
                        continue;
                    }
                    next = src[ip++];
                    if (state == 4)
                    {
                        copy(1 + 0x800 + (t >> 2) + (next << 2), 3);
                    }
                    else
                    {
                        copy(1 + (t >> 2) + (next << 2), 2);
                    }
                    state = t & 3;
                }
                else if (t >= 64)
                {
                    next = src[ip++];
                    copy(1 + ((t >> 2) & 7) + (next << 3), (t >> 5) + 1);
                    state = t & 3;
                }
                else if (t >= 32)
                {
                    int n = t & 31;
                    if (n == 0)
                    {
                        while (src[ip] == 0)
                        {
                            n += 255;
                            ip++;
                        }
                        n += 31 + src[ip++];
                    }
                    int v = src[ip] | (src[ip + 1] << 8);
                    ip += 2;
                    copy(1 + (v >> 2), n + 2);
                    state = v & 3;
                }
                else
                {
                    int n = t & 7;
                    if (n == 0)
                    {
                        while (src[ip] == 0)
                        {
                            n += 255;
                            ip++;
                        }
                        n += 7 + src[ip++];
                    }
                    int v = src[ip] | (src[ip + 1] << 8);
                    ip += 2;
                    int dist = ((t & 8) << 11) + (v >> 2);
                    if (dist == 0)
                    {
                        break; // end of the stream
                    }
                    copy(dist + 0x4000, n + 2);
                    state = v & 3;
                }
                if (state > 0)
                {
                    lit(state);
                }
            }
            if (op != outLen)
            {
                throw new InvalidDataException();
            }
            return o;
        }
    }
}
