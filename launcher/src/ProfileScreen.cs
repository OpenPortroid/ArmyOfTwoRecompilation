using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Windows.Forms;

namespace AO2Launcher
{
    // The Profiles tab, the current profile under the main screen's buttons, choices that ask first, and
    // removing the game.
    sealed partial class LauncherForm
    {
        Row profileRow, newProfileRow;
        readonly Dictionary<string, Profiles.Summary> summaries = new Dictionary<string, Profiles.Summary>();

        // A choice row's new value: asks first if the row wants to (false: not set now, maybe after the
        // question), then sets it.
        bool SetChoice(Row r, string v)
        {
            if (r.ConfirmOn != null && v != S[r.Key])
            {
                Confirm c = r.ConfirmOn(v);
                if (c != null)
                {
                    c.OnOk = _ =>
                    {
                        ApplyChoice(r, v);
                        S.Save();
                        LayoutRows();
                    };
                    Ask(c);
                    return false;
                }
            }
            ApplyChoice(r, v);
            return true;
        }

        void ApplyChoice(Row r, string v)
        {
            S[r.Key] = v;
            if (r.Key == "language")
            {
                LanguageChanged();
            }
            if (r.Changed != null)
            {
                r.Changed();
            }
        }

        static string ProfileName(string name)
        {
            return string.IsNullOrEmpty(name) ? L.T("Main") : name;
        }

        static string MoneyText(long money)
        {
            return "$" + money.ToString("N0", CultureInfo.InvariantCulture);
        }

        Profiles.Summary SummaryOf(string name)
        {
            Profiles.Summary s;
            return summaries.TryGetValue(name ?? "", out s) ? s : new Profiles.Summary();
        }

        // "Iraq · $409,500" (what there is of it).
        string SummaryLine(string name)
        {
            Profiles.Summary s = SummaryOf(name);
            if (s.Map == null && s.Money < 0)
            {
                return L.T("no saves yet");
            }
            var parts = new List<string>();
            if (s.Map != null)
            {
                parts.Add(s.Map);
            }
            if (s.Money >= 0)
            {
                parts.Add(MoneyText(s.Money));
            }
            return string.Join("  ·  ", parts);
        }

        // The profile list and what is in each one's saves, read again (the game may have saved).
        void RefreshProfiles()
        {
            List<string> all = Profiles.All();
            if (!all.Contains(S["profile"]))
            {
                S["profile"] = Profiles.Main;
            }
            summaries.Clear();
            foreach (string p in all)
            {
                summaries[p] = Profiles.Read(p);
            }
            if (profileRow != null)
            {
                profileRow.Values = all.ToArray();
            }
            RefreshAchievements();
            playSeconds = Playtime.Seconds(S["profile"]);
        }

        long playSeconds; // the time played with the current profile (Playtime)

        List<Row> ProfileRows()
        {
            profileRow = new Row
            {
                Kind = RowKind.Choice, Label = "Profile", Key = "profile", Drop = true,
                Values = new[] { Profiles.Main },
                Name = i => ProfileName(profileRow.Values[i]),
                DropName = i => ProfileName(profileRow.Values[i]) + "   —   " + SummaryLine(profileRow.Values[i]),
                Changed = () =>
                {
                    RefreshProfiles();
                    ShowToast(L.F("Profile {0}: {1}", ProfileName(S["profile"]), SummaryLine(S["profile"])), false);
                },
                Desc = "The profile the game starts with. Each profile has its own saves, game settings, money and unlocks; " +
                       "downloadable content is shared."
            };
            newProfileRow = new Row
            {
                Kind = RowKind.Text, Label = "New profile name", Key = "new_profile", MaxLen = 32,
                Placeholder = Profiles.SuggestName,
                Desc = "Name for a new profile. Leave empty to use the suggested one."
            };
            return new List<Row>
            {
                profileRow,
                new Row
                {
                    Kind = RowKind.Info,
                    Text = () =>
                    {
                        Profiles.Summary s = SummaryOf(S["profile"]);
                        return L.F("Last checkpoint: {0}", s.Map ?? L.T("none yet")) + "      " +
                               L.F("Money: {0}", s.Money >= 0 ? MoneyText(s.Money) : L.T("none yet")) + "      " +
                               L.F("Time played: {0}", Playtime.Format(playSeconds));
                    }
                },
                newProfileRow,
                new Row
                {
                    Kind = RowKind.Button, Label = "Create profile", ButtonText = () => "CREATE",
                    Click = CreateProfile,
                    Desc = "Create a new profile with the name above and switch to it. The game starts it from scratch."
                },
                new Row
                {
                    Kind = RowKind.Button, Label = "Delete profile", ButtonText = () => "DELETE",
                    Click = DeleteProfile, Enabled = () => S["profile"] != Profiles.Main,
                    Desc = "Delete the current profile with all its saves. The main profile cannot be deleted."
                },
                new Row
                {
                    Kind = RowKind.Info,
                    Text = () => L.T("Profiles other than the main one are kept in Documents\\armyoftworecomp_profiles, or in AppData\\Local\\armyoftworecomp_profiles if Documents cannot be written to.")
                },
            };
        }

        void CreateProfile()
        {
            string typed = S["new_profile"].Trim();
            string error;
            string name = Profiles.CheckName(typed.Length > 0 ? typed : Profiles.SuggestName(), out error);
            if (name == null)
            {
                ShowToast(error, true);
                return;
            }
            try
            {
                Profiles.Create(name);
            }
            catch (Exception ex)
            {
                ShowToast(L.F("Could not create the profile: {0}", ex.Message), true);
                return;
            }
            ActiveControl = null;
            S["profile"] = name;
            S["new_profile"] = "";
            S.Save();
            RefreshProfiles();
            SetCueBanner(newProfileRow);
            LayoutRows();
            ShowToast(L.F("Profile {0} created", name), false);
        }

        void DeleteProfile()
        {
            string name = S["profile"];
            if (name == Profiles.Main)
            {
                return;
            }
            Ask(new Confirm
            {
                Title = "DELETE PROFILE",
                Text = L.F("Profile {0} and all its saves will be deleted for good ({1}).", name, SummaryLine(name)),
                Ok = "DELETE",
                Danger = true,
                OnOk = _ =>
                {
                    if (Game.IsRunning())
                    {
                        ShowToast(L.T("Close the game first."), true);
                        return;
                    }
                    try
                    {
                        Profiles.Delete(name);
                    }
                    catch (Exception ex)
                    {
                        ShowToast(L.F("Could not delete the profile: {0}", ex.Message), true);
                        return;
                    }
                    S["profile"] = Profiles.Main;
                    S.Save();
                    RefreshProfiles();
                    LayoutRows();
                    ShowToast(L.F("Profile {0} deleted", name), false);
                }
            });
        }

        // Bottom left, mirroring the Discord / GitHub chips, in two rows: the profile the game starts with and its
        // time played (a click opens the Profiles tab) above, level with Settings / Exit the achievements of the profile the game starts with
        // (the Achievements tab): the icons of the latest unlocked and the progress.
        void DrawStatsChips(Graphics g, float cy)
        {
            const float x = 24;
            ProfileChip(g, x, cy - 40);
            AchievementChip(g, x, cy);
        }

        void ProfileChip(Graphics g, float left, float cy)
        {
            const float icon = 16, h = 32, padX = 12, gap = 8;
            const string id = "stats_profile";
            string text = ProfileName(S["profile"]) + "  ·  " + Playtime.Format(playSeconds);
            float tw = g.MeasureString(text, fSmall).Width;
            var r = new RectangleF(left, cy - h / 2, padX + icon + gap + tw + padX, h);
            FillRound(g, IsHover(id) ? Color.FromArgb(230, InkRaise) : Color.FromArgb(178, InkDeep), r, h / 2);
            StatIcon(g, r.X + padX + icon / 2, cy, false, icon / 18f);
            var fmt = new StringFormat { LineAlignment = StringAlignment.Center, FormatFlags = StringFormatFlags.NoWrap };
            DrawText(g, text, fSmall, PaperHi, new RectangleF(r.X + padX + icon + gap, r.Y, tw + 2, h), fmt);
            AddHot(r, id, () =>
            {
                SetScreen(UiScreen.Settings);
                SetTab(ProfilesTab);
            });
        }

        void AchievementChip(Graphics g, float left, float cy)
        {
            const float h = 32, padX = 10, icon = 22, gap = 4, barW = 72;
            const string id = "stats_ach";
            string text = achTotal > 0 ? achGot + " / " + achTotal : "—";
            float tw = g.MeasureString(text, fSmall).Width;
            int n = achRecent.Count;
            float iconsW = n > 0 ? n * icon + (n - 1) * gap : 16;
            float barX = left + padX + iconsW + 12;
            var r = new RectangleF(left, cy - h / 2, barX + barW + 8 + tw + padX - left, h);
            FillRound(g, IsHover(id) ? Color.FromArgb(230, InkRaise) : Color.FromArgb(178, InkDeep), r, h / 2);
            if (n == 0)
            {
                StatIcon(g, left + padX + 8, cy, true, 16f / 18f);
            }
            else
            {
                GraphicsState st = g.Save();
                g.SmoothingMode = SmoothingMode.AntiAlias;
                g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                g.PixelOffsetMode = PixelOffsetMode.HighQuality;
                float x = left + padX;
                foreach (Bitmap b in achRecent)
                {
                    var ir = new RectangleF(x, cy - icon / 2, icon, icon);
                    using (GraphicsPath p = RoundPath(ir, 5))
                    {
                        g.SetClip(p, CombineMode.Replace);
                        g.DrawImage(b, ir);
                    }
                    x += icon + gap;
                }
                g.Restore(st);
            }
            Progress(g, new RectangleF(barX, cy - 2, barW, 4), achTotal > 0 ? (float)achGot / achTotal : 0);
            var fmt = new StringFormat { LineAlignment = StringAlignment.Center, FormatFlags = StringFormatFlags.NoWrap };
            DrawText(g, text, fSmall, PaperHi, new RectangleF(barX + barW + 8, r.Y, tw + 2, h), fmt);
            AddHot(r, id, () =>
            {
                SetScreen(UiScreen.Settings);
                SetTab(AchievementsTab);
            });
        }

        // The chips' icons, drawn as lines around a centre (18 units across): a clock, or a cup.
        // A cup (achievements) or a person (the profile).
        static void StatIcon(Graphics g, float cx, float cy, bool cup, float k)
        {
            GraphicsState st = g.Save();
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.TranslateTransform(cx, cy);
            g.ScaleTransform(k, k);
            using (var pen = new Pen(PaperHi, 1.8f) { StartCap = LineCap.Round, EndCap = LineCap.Round, LineJoin = LineJoin.Round })
            {
                if (cup)
                {
                    g.DrawLine(pen, -6, -9, 6, -9);
                    g.DrawLine(pen, -6, -9, -6, -3);
                    g.DrawLine(pen, 6, -9, 6, -3);
                    g.DrawBezier(pen, 6, -3, 6, 2, -6, 2, -6, -3);
                    g.DrawBezier(pen, -6, -7, -11, -7, -11, -1, -5, 0);
                    g.DrawBezier(pen, 6, -7, 11, -7, 11, -1, 5, 0);
                    g.DrawLine(pen, 0, 2, 0, 6);
                    g.DrawLine(pen, -4, 7.5f, 4, 7.5f);
                }
                else
                {
                    // A person: the head over the shoulders.
                    g.DrawEllipse(pen, -4, -9, 8, 8);
                    g.DrawBezier(pen, -8, 9, -8, 1, 8, 1, 8, 9);
                }
            }
            g.Restore(st);
        }

        // Debug (--snapshot with --ask=unlock|uninstall|profile): that question open.
        public void SnapshotAsk(string what)
        {
            if (what == "unlock")
            {
                Row r = AllRows().First(x => x.Key == "unlock_all");
                Ask(r.ConfirmOn("1"));
            }
            else if (what == "uninstall")
            {
                Ask(new Confirm
                {
                    Title = "UNINSTALL THE GAME", Danger = true, Ok = "UNINSTALL", Check = "Also delete the saves of every profile",
                    Text = L.F("The game files will be deleted from {0} ({1}). To play again, install the game from your copy. " +
                               "Downloadable content stays in the dlc folder.", @"C:\Games\ArmyOfTwo\assets", "6.1" + L.T(" GB")),
                });
            }
            else if (what == "profile")
            {
                Ask(new Confirm
                {
                    Title = "DELETE PROFILE", Danger = true, Ok = "DELETE",
                    Text = L.F("Profile {0} and all its saves will be deleted for good ({1}).", "Test", SummaryLine("")),
                });
            }
        }

        void Uninstall()
        {
            if (gameExe == null)
            {
                ShowToast(L.T("Game not found"), true);
                return;
            }
            if (Game.IsRunning())
            {
                ShowToast(L.T("Close the game first."), true);
                return;
            }
            string dir = GameDir;
            Ask(new Confirm
            {
                Title = "UNINSTALL THE GAME",
                Text = L.F("The game files will be deleted from {0} ({1}). To play again, install the game from your copy. " +
                           "Downloadable content stays in the dlc folder.", Installer.AssetsDir(dir), Installer.Size(Installer.InstalledBytes(dir))),
                Check = "Also delete the saves of every profile",
                Ok = "UNINSTALL",
                Danger = true,
                OnOk = saves => DoUninstall(dir, saves),
            });
        }

        void DoUninstall(string dir, bool saves)
        {
            if (Game.IsRunning())
            {
                ShowToast(L.T("Close the game first."), true);
                return;
            }
            Cursor = Cursors.WaitCursor;
            string error = null;
            try
            {
                Installer.Uninstall(dir);
                if (saves)
                {
                    foreach (string p in Profiles.All().Where(p => p != Profiles.Main))
                    {
                        Profiles.Delete(p);
                    }
                    Profiles.DeleteSaves(Profiles.MainRoot);
                    S["profile"] = Profiles.Main;
                    S.Save();
                }
            }
            catch (Exception ex)
            {
                Log.Error("uninstall", ex);
                error = ex.Message;
            }
            finally
            {
                Cursor = Cursors.Default;
            }
            RefreshInstalled();
            RefreshProfiles();
            LayoutRows();
            if (error != null)
            {
                ShowToast(L.F("Could not delete everything: {0}", error), true);
            }
            else
            {
                ShowToast(L.T(saves ? "The game and the saves are deleted" : "The game is uninstalled, the saves are kept"), false);
            }
        }
    }
}
