using System;
using System.Collections.Generic;
using System.Drawing;
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
        }

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
                       "the downloadable content is shared."
            };
            newProfileRow = new Row
            {
                Kind = RowKind.Text, Label = "New profile name", Key = "new_profile", MaxLen = 32,
                Placeholder = Profiles.SuggestName,
                Desc = "The name of a new profile. Empty: the suggested one."
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
                               L.F("Money: {0}", s.Money >= 0 ? MoneyText(s.Money) : L.T("none yet"));
                    }
                },
                newProfileRow,
                new Row
                {
                    Kind = RowKind.Button, Label = "Create profile", ButtonText = () => "CREATE",
                    Click = CreateProfile,
                    Desc = "Create an empty profile with the name above and switch to it. The game starts it from the beginning."
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
                    Text = () => L.T("Profiles other than the main one are kept in Documents\\armyoftworecomp_profiles.")
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

        // Under the main screen's buttons, left: the profile the game starts with; a click opens the
        // Profiles tab.
        void DrawProfileChip(Graphics g)
        {
            var r = new RectangleF(24, 538, 232, 50);
            bool hv = IsHover("profile");
            if (hv)
            {
                Fill(g, Color.FromArgb(14, 255, 255, 255), r);
            }
            var fmt = new StringFormat { LineAlignment = StringAlignment.Center, Trimming = StringTrimming.EllipsisCharacter, FormatFlags = StringFormatFlags.NoWrap };
            string head = L.T("PROFILE");
            float hw = CapsWidth(g, head, fBack, 1.6f);
            Caps(g, head, fBack, Color.FromArgb(130, 142, 160), new RectangleF(r.X + 10, r.Y + 6, hw, 18), 1.6f, StringAlignment.Near);
            DrawText(g, ProfileName(S["profile"]), fLabel, hv ? PaperHi : TextLight, new RectangleF(r.X + 18 + hw, r.Y + 4, r.Width - hw - 24, 22), fmt);
            DrawText(g, SummaryLine(S["profile"]), fSmall, hv ? Paper : TextDim, new RectangleF(r.X + 10, r.Y + 26, r.Width - 16, 20), fmt);
            AddHot(r, "profile", () =>
            {
                SetScreen(UiScreen.Settings);
                SetTab(ProfilesTab);
            });
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
