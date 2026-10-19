using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.Globalization;
using System.IO;
using System.Linq;

namespace AO2Launcher
{
    // The achievements of a profile. Their names, descriptions and icons are the game's (its title data), not
    // the launcher's: the game writes them, in every language, to the shared cache
    // (Documents\armyoftworecomp_tu2\cache\achievements: catalog.tsv, icons\<image>.png) when it starts
    // (aot\src\achievements.cpp). What a profile has unlocked is in its own user data folder:
    // achievements\4541084C.toml, [unlocked.<id>] with the time as a FILETIME.
    static class Achievements
    {
        public sealed class Entry
        {
            public int Id, Section, Gamerscore, Image;
            public bool Secret;
            public string Label, Description, Locked; // in the launcher's language
            public DateTime? Unlocked;                // local time; null: locked
            public Bitmap Icon;
        }

        // The sections, as the game's Achievements page has them.
        public static readonly string[] Sections =
            { "Story", "Campaign", "Gear and co-op", "Weapons", "Combat", "Versus", "SSC Challenge" };

        static string Dir
        {
            get { return Path.Combine(Profiles.MainRoot, "cache", "achievements"); }
        }

        static readonly Dictionary<int, Bitmap> icons = new Dictionary<int, Bitmap>();

        static Bitmap IconOf(int image)
        {
            Bitmap b;
            if (icons.TryGetValue(image, out b))
            {
                return b;
            }
            try
            {
                string path = Path.Combine(Dir, "icons", image.ToString(CultureInfo.InvariantCulture) + ".png");
                if (File.Exists(path))
                {
                    using (var ms = new MemoryStream(File.ReadAllBytes(path)))
                    using (Image img = Image.FromStream(ms))
                    {
                        b = new Bitmap(img);
                    }
                }
            }
            catch
            {
                b = null;
            }
            icons[image] = b;
            return b;
        }

        // The ids a profile has unlocked, with when.
        static Dictionary<int, DateTime> Unlocks(string profile)
        {
            var map = new Dictionary<int, DateTime>();
            string path = Path.Combine(Profiles.Root(profile), "achievements", "4541084C.toml");
            if (!File.Exists(path))
            {
                return map;
            }
            int id = -1;
            foreach (string raw in File.ReadAllLines(path))
            {
                string line = raw.Trim();
                if (line.StartsWith("[unlocked.") && line.EndsWith("]"))
                {
                    if (!int.TryParse(line.Substring(10, line.Length - 11), NumberStyles.Integer, CultureInfo.InvariantCulture, out id))
                    {
                        id = -1;
                    }
                    else
                    {
                        map[id] = DateTime.MinValue;
                    }
                }
                else if (id >= 0 && line.StartsWith("filetime"))
                {
                    long ft;
                    int eq = line.IndexOf('=');
                    if (eq > 0 && long.TryParse(line.Substring(eq + 1).Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture, out ft) && ft > 0)
                    {
                        try
                        {
                            map[id] = DateTime.FromFileTimeUtc(ft).ToLocalTime();
                        }
                        catch (ArgumentOutOfRangeException)
                        {
                        }
                    }
                }
            }
            return map;
        }

        // The achievements in the launcher's language with a profile's unlocks; empty when the game has not
        // written them yet.
        public static List<Entry> Load(string profile)
        {
            var list = new List<Entry>();
            string catalog = Path.Combine(Dir, "catalog.tsv");
            if (!File.Exists(catalog))
            {
                return list;
            }
            var byId = new Dictionary<int, Entry>();
            var english = new Dictionary<int, string[]>();
            try
            {
                foreach (string line in File.ReadAllLines(catalog))
                {
                    string[] f = line.Split('\t');
                    int id;
                    if (f.Length < 2 || !int.TryParse(f[1], NumberStyles.Integer, CultureInfo.InvariantCulture, out id))
                    {
                        continue;
                    }
                    if (f[0] == "A" && f.Length >= 6)
                    {
                        var e = new Entry { Id = id };
                        int.TryParse(f[2], NumberStyles.Integer, CultureInfo.InvariantCulture, out e.Section);
                        int.TryParse(f[3], NumberStyles.Integer, CultureInfo.InvariantCulture, out e.Gamerscore);
                        int.TryParse(f[4], NumberStyles.Integer, CultureInfo.InvariantCulture, out e.Image);
                        e.Secret = f[5] == "1";
                        byId[id] = e;
                        list.Add(e);
                    }
                    else if (f[0] == "T" && f.Length >= 6)
                    {
                        if (f[2] == "en")
                        {
                            english[id] = f;
                        }
                        Entry e;
                        if (f[2] == L.Code && byId.TryGetValue(id, out e))
                        {
                            e.Label = f[3];
                            e.Description = f[4];
                            e.Locked = f[5];
                        }
                    }
                }
            }
            catch (IOException)
            {
                return new List<Entry>();
            }
            Dictionary<int, DateTime> unlocks = Unlocks(profile);
            foreach (Entry e in list)
            {
                string[] en;
                if (e.Label == null && english.TryGetValue(e.Id, out en))
                {
                    e.Label = en[3];
                    e.Description = en[4];
                    e.Locked = en[5];
                }
                DateTime when;
                if (unlocks.TryGetValue(e.Id, out when))
                {
                    e.Unlocked = when;
                }
                e.Icon = IconOf(e.Image);
            }
            // Section by section, in the game's order within each.
            return list.Select((e, i) => new { e, i })
                .OrderBy(x => x.e.Section < 0 ? int.MaxValue : x.e.Section).ThenBy(x => x.i)
                .Select(x => x.e).ToList();
        }

        // A locked achievement's icon: grey and faded.
        public static readonly ImageAttributes LockedLook = MakeLockedLook();

        static ImageAttributes MakeLockedLook()
        {
            var cm = new ColorMatrix(new[]
            {
                new[] { 0.3f, 0.3f, 0.3f, 0, 0 },
                new[] { 0.59f, 0.59f, 0.59f, 0, 0 },
                new[] { 0.11f, 0.11f, 0.11f, 0, 0 },
                new[] { 0, 0, 0, 0.45f, 0 },
                new[] { 0, 0, 0, 0, 1f },
            });
            var a = new ImageAttributes();
            a.SetColorMatrix(cm);
            return a;
        }
    }

    // The Achievements tab: the current profile's achievements by section.
    sealed partial class LauncherForm
    {
        readonly List<Row> achievementRows = new List<Row>();

        List<Row> AchievementRows()
        {
            return achievementRows;
        }

        // Read again: the game may have unlocked some, or the profile or the language changed.
        void RefreshAchievements()
        {
            achievementRows.Clear();
            List<Achievements.Entry> all = Achievements.Load(S["profile"]);
            if (all.Count == 0)
            {
                achievementRows.Add(new Row
                {
                    Kind = RowKind.Info,
                    Text = () => L.T("The list of achievements comes from the game's files: start the game once to see it here.")
                });
                return;
            }
            int got = all.Count(e => e.Unlocked != null);
            int score = all.Where(e => e.Unlocked != null).Sum(e => e.Gamerscore);
            int max = all.Sum(e => e.Gamerscore);
            string profile = ProfileName(S["profile"]);
            achievementRows.Add(new Row
            {
                Kind = RowKind.Info,
                Text = () => L.F("Profile {0}: {1} of {2} unlocked, {3} of {4} G", profile, got, all.Count, score, max)
            });
            foreach (var section in all.GroupBy(e => e.Section))
            {
                int s = section.Key;
                string name = s >= 0 && s < Achievements.Sections.Length ? L.T(Achievements.Sections[s]) : "";
                int sGot = section.Count(e => e.Unlocked != null);
                int sCount = section.Count();
                achievementRows.Add(new Row
                {
                    Kind = RowKind.Info,
                    Text = () => name.ToUpper(CultureInfo.CurrentCulture) + "   " + sGot + " / " + sCount
                });
                foreach (Achievements.Entry e in section)
                {
                    string desc = e.Unlocked != null ? e.Description : e.Locked;
                    if (e.Unlocked != null && e.Unlocked.Value != DateTime.MinValue)
                    {
                        desc += "\n" + L.F("Earned {0}", e.Unlocked.Value.ToString("g", CultureInfo.CurrentCulture));
                    }
                    achievementRows.Add(new Row { Kind = RowKind.Achievement, Label = e.Label, Desc = desc, Ach = e });
                }
            }
        }

        void DrawAchievementRow(Graphics g, Row r, RectangleF rr, bool selected, string id, int index)
        {
            Achievements.Entry e = r.Ach;
            bool earned = e.Unlocked != null;
            const float icon = 26;
            var ir = new RectangleF(rr.X + 12, rr.Y + (rr.Height - icon) / 2, icon, icon);
            if (e.Icon != null)
            {
                if (earned)
                {
                    g.DrawImage(e.Icon, ir);
                }
                else
                {
                    g.DrawImage(e.Icon, Rectangle.Round(ir), 0, 0, e.Icon.Width, e.Icon.Height, GraphicsUnit.Pixel, Achievements.LockedLook);
                }
            }
            else
            {
                Outline(g, Color.FromArgb(50, Paper), ir);
            }
            Color labelColor = selected ? PaperHi : earned ? TextLight : TextDim;
            var lfmt = new StringFormat { LineAlignment = StringAlignment.Center, Trimming = StringTrimming.EllipsisCharacter, FormatFlags = StringFormatFlags.NoWrap };
            DrawText(g, r.Label, fLabel, labelColor, new RectangleF(ir.Right + 12, r.Y, ValueX - ir.Right - 16, r.H), lfmt);

            string value = earned ? "✓  " + e.Gamerscore + " G" : e.Gamerscore + " G";
            Color valColor = selected ? PaperHi : earned ? Color.FromArgb(214, 218, 226) : TextOff;
            var vfmt = new StringFormat { Alignment = StringAlignment.Far, LineAlignment = StringAlignment.Center, FormatFlags = StringFormatFlags.NoWrap };
            DrawText(g, value, fValue, valColor, new RectangleF(ValueX, r.Y, ValueW, r.H), vfmt);
            AddHot(rr, id, null, index);
        }
    }
}
