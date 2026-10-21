using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.Linq;
using System.Windows.Forms;

namespace AO2Launcher
{
    // The install screen: the launcher has no game files, the user gives their
    // own copy (disc image or extracted folder), Title Update 2 and, optionally,
    // the downloadable content. Files can also be dropped on the window.
    sealed partial class LauncherForm
    {
        const int InstRows = 4; // game, update, DLC, shortcut; then the install button
        const float InstTop = 160, InstRowH = 58;

        InstallSource srcGame, srcUpdate;
        readonly List<InstallSource> srcDlc = new List<InstallSource>();
        bool gameOk, updateOk, anyInstalled;
        int dlcCount;
        int instSel;
        bool installing;
        volatile bool cancelInstall;
        string instStep = "";
        double instProgress;
        string instError;

        static readonly Color Good = Color.FromArgb(128, 200, 140);
        static readonly Color Bad = Color.FromArgb(236, 120, 110);

        bool Installed
        {
            get { return gameOk && updateOk; }
        }

        string GameDir
        {
            get { return gameExe == null ? null : Path.GetDirectoryName(gameExe); }
        }

        void InitInstall()
        {
            AllowDrop = true;
            DragEnter += (s, e) =>
            {
                e.Effect = !installing && e.Data.GetDataPresent(DataFormats.FileDrop) ? DragDropEffects.Copy : DragDropEffects.None;
            };
            DragDrop += (s, e) =>
            {
                var files = e.Data.GetData(DataFormats.FileDrop) as string[];
                if (files != null && !installing)
                {
                    // Not inside the drag-and-drop call: Explorer waits for it.
                    BeginInvoke(new Action(() =>
                    {
                        if (screen != UiScreen.Install)
                        {
                            SetScreen(UiScreen.Install);
                        }
                        AddSources(files);
                    }));
                }
            };
            RefreshInstalled();
        }

        void RefreshInstalled()
        {
            string dir = GameDir;
            gameOk = dir != null && Installer.GameInstalled(dir);
            updateOk = dir != null && Installer.UpdateInstalled(dir);
            anyInstalled = Installer.AnythingInstalled(dir);
            dlcCount = Installer.InstalledDlcCount(dir);
        }

        bool CanInstall
        {
            get
            {
                return gameExe != null && !installing && (gameOk || srcGame != null) && (updateOk || srcUpdate != null) &&
                       (srcGame != null || srcUpdate != null || srcDlc.Count > 0);
            }
        }

        // Each picked file goes where it belongs, whichever row it was picked from.
        void AddSources(IEnumerable<string> paths)
        {
            if (gameExe == null && !BrowseGame())
            {
                return;
            }
            instError = null;
            var added = new List<string>();
            var found = new List<InstallSource>();
            Cursor = Cursors.WaitCursor;
            try
            {
                foreach (string path in paths)
                {
                    try
                    {
                        found.Add(Installer.Probe(GameDir, path));
                    }
                    catch (InstallException ex)
                    {
                        Log.Write("cannot use {0}: {1}", path, ex.Message);
                        instError = ex.Message;
                    }
                    catch (Exception ex)
                    {
                        Log.Error("probing " + path, ex);
                        instError = ex.Message;
                    }
                }
                // The game, then its update, then the DLC: each needs the one before (chosen now or installed).
                Func<string, int> rank = k => k == "game" ? 0 : k == "tu" ? 1 : 2;
                foreach (InstallSource src in found.OrderBy(f => rank(f.Kind)))
                {
                    switch (src.Kind)
                    {
                        case "game":
                            srcGame = src;
                            added.Add(L.T("game"));
                            break;
                        case "tu":
                            if (!InstRowEnabled(1))
                            {
                                instError = L.T("Choose the game first, then Title Update 2");
                                break;
                            }
                            srcUpdate = src;
                            added.Add("Title Update");
                            break;
                        default:
                            if (!InstRowEnabled(2))
                            {
                                instError = L.T("Choose Title Update 2 first, then the DLC");
                                break;
                            }
                            srcDlc.RemoveAll(d => string.Equals(Path.GetFileName(d.Path), Path.GetFileName(src.Path), StringComparison.OrdinalIgnoreCase));
                            srcDlc.Add(src);
                            added.Add("DLC");
                            break;
                    }
                }
            }
            finally
            {
                Cursor = Cursors.Default;
            }
            if (added.Count > 0 && instError == null)
            {
                ShowToast(L.F("Added: {0}", string.Join(", ", added)), false);
            }
            Invalidate();
        }

        void PickSources(int row)
        {
            using (var dlg = new OpenFileDialog())
            {
                dlg.Multiselect = true;
                if (row == 0)
                {
                    dlg.Title = L.T("Army of Two game image (Xbox 360)");
                    dlg.Filter = L.T("Disc image") + " (*.iso)|*.iso|" + L.T("Extracted game") + " (default.xex)|default.xex|" +
                                 L.T("All files") + "|*.*";
                }
                else
                {
                    dlg.Title = row == 1 ? "Title Update 2 (TU_12K222C_…)" : L.T("DLC packages");
                    dlg.Filter = L.T("All files") + "|*.*";
                }
                if (dlg.ShowDialog(this) == DialogResult.OK)
                {
                    AddSources(dlg.FileNames);
                }
            }
        }

        // Title Update 2 can be chosen once the game is (chosen or installed), the DLC once the update is.
        bool InstRowEnabled(int i)
        {
            switch (i)
            {
                case 1: return srcGame != null || gameOk;
                case 2: return srcUpdate != null || updateOk;
            }
            return true;
        }

        void ActivateInstall(int i)
        {
            if (installing)
            {
                if (i == InstRows)
                {
                    cancelInstall = true;
                }
                return;
            }
            if (!InstRowEnabled(i))
            {
                return;
            }
            if (i < 3)
            {
                PickSources(i);
            }
            else if (i == 3)
            {
                S["desktop_shortcut"] = S.Bool("desktop_shortcut") ? "0" : "1";
                S.Save();
            }
            else if (CanInstall)
            {
                StartInstall();
            }
            Invalidate();
        }

        void StartInstall()
        {
            installing = true;
            cancelInstall = false;
            instError = null;
            instStep = L.T("Preparing");
            instProgress = 0;
            string dir = GameDir;
            Log.Write("install requested from the window; log: {0}", Log.FilePath);
            InstallSource game = srcGame, update = srcUpdate;
            var dlc = srcDlc.ToList();
            var th = new System.Threading.Thread(() =>
            {
                string error = null;
                bool cancelled = false;
                try
                {
                    Installer.Run(dir, game, update, dlc, (name, p) =>
                    {
                        instStep = name;
                        instProgress = p;
                        try
                        {
                            BeginInvoke(new Action(Invalidate));
                        }
                        catch (InvalidOperationException)
                        {
                        }
                    }, () => cancelInstall);
                }
                catch (OperationCanceledException)
                {
                    Log.Write("install cancelled");
                    cancelled = true;
                }
                catch (InstallException ex)
                {
                    Log.Error("install failed: " + ex.Message, ex);
                    error = ex.Message;
                }
                catch (Exception ex)
                {
                    Log.Error("install crashed", ex);
                    error = ex.Message;
                }
                try
                {
                    BeginInvoke(new Action(() => FinishInstall(error, cancelled)));
                }
                catch (InvalidOperationException)
                {
                    // The form was closed meanwhile.
                }
            });
            th.IsBackground = true;
            th.Start();
        }

        void FinishInstall(string error, bool cancelled)
        {
            installing = false;
            RefreshInstalled();
            Log.Write("install ended: {0}; game {1}, update {2}", cancelled ? "cancelled" : error != null ? "ERROR " + error : "OK", gameOk, updateOk);
            if (cancelled)
            {
                ShowToast(L.T("Installation cancelled"), true);
            }
            else if (error != null)
            {
                instError = error;
            }
            else
            {
                srcGame = srcUpdate = null;
                srcDlc.Clear();
                bool shortcut = S.Bool("desktop_shortcut") && Installer.CreateDesktopShortcut();
                SetScreen(UiScreen.Main);
                ShowToast(L.T(shortcut ? "Game installed, shortcut created on the desktop" : "Installation complete"), false);
            }
            Invalidate();
        }

        void MoveInstSel(int dir)
        {
            do
            {
                instSel = (instSel + dir + InstRows + 1) % (InstRows + 1);
            } while (!InstRowEnabled(instSel));
            Invalidate();
        }

        bool InstallKey(Keys key)
        {
            switch (key)
            {
                case Keys.Up: MoveInstSel(-1); return true;
                case Keys.Down:
                case Keys.Tab:
                    MoveInstSel(1);
                    return true;
                case Keys.Enter:
                case Keys.Space:
                    ActivateInstall(instSel);
                    return true;
                case Keys.Escape:
                    if (!installing)
                    {
                        SetScreen(UiScreen.Main);
                    }
                    return true;
            }
            return false;
        }

        void InstallPad(int b)
        {
            switch (b)
            {
                case Gamepad.DpadUp: MoveInstSel(-1); break;
                case Gamepad.DpadDown: MoveInstSel(1); break;
                case Gamepad.A: ActivateInstall(instSel); break;
                case Gamepad.B:
                    if (!installing)
                    {
                        SetScreen(UiScreen.Main);
                    }
                    break;
            }
        }

        static readonly string[] InstDesc =
        {
            "Your copy of Army of Two for Xbox 360, European release: a disc image (.iso), a Games on Demand package or " +
            "default.xex in a folder with the extracted game. The files are copied to the game folder (about 5.5 GB); the image is not needed afterwards.",
            "Required: the port is built for Title Update 2. The update file is TU_12K222C_0000004000000.00000000000G1 " +
            "(on the console: Content\\0000000000000000\\4541084C\\000B0000).",
            "Optional: the Veteran Map Pack and SSC Challenge: Versus Mode Pack 1 packages (on the console: Content\\0000000000000000\\" +
            "454107F8\\00000002). Can be added later. In co-op both players need the same content.",
            "Create an \"Army of Two\" shortcut on the desktop that starts this launcher.",
            "Copy the chosen files. After that the game runs without the image.",
        };

        void DrawInstall(Graphics g)
        {
            // The title bar with BACK is drawn over this by OnPaint.
            var col = new RectangleF(ColX - 24, 56, ColW + 48, 540);
            Panel(g, col);
            SectionHeader(g, L.T("GAME INSTALLATION"), ColX + 2, 68, ColW - 4);
            using (var txt = new SolidBrush(TextLight))
            {
                g.DrawString(L.T("The launcher contains no game files: you need your own copy of Army of Two for Xbox 360 and " +
                                 "Title Update 2. Choose the files below or drop them on this window."),
                    fDesc, txt, new RectangleF(ColX + 6, 98, ColW - 12, 56));
            }

            string dlcStatus = !InstRowEnabled(2) ? L.T("choose Title Update 2 first")
                : srcDlc.Count > 0 ? string.Join(", ", srcDlc.Select(d => d.Name.Length > 0 ? d.Name : Path.GetFileName(d.Path)))
                : dlcCount > 0 ? L.F("packages installed: {0}", dlcCount) : L.T("none chosen - can be added later");
            DrawInstRow(g, 0, L.T("Game"), srcGame != null ? srcGame.Caption
                    : L.T(gameOk ? "installed" : "not chosen - a disc image (.iso) or the game folder"),
                srcGame != null || gameOk ? Good : Bad, L.T(srcGame != null || gameOk ? "CHANGE" : "CHOOSE"));
            DrawInstRow(g, 1, "Title Update 2", !InstRowEnabled(1) ? L.T("choose the game first")
                    : srcUpdate != null ? srcUpdate.Caption : L.T(updateOk ? "installed" : "not chosen - required"),
                !InstRowEnabled(1) ? TextOff : srcUpdate != null || updateOk ? Good : Bad, L.T(srcUpdate != null || updateOk ? "CHANGE" : "CHOOSE"));
            DrawInstRow(g, 2, L.T("Downloadable content (DLC)"), dlcStatus,
                !InstRowEnabled(2) ? TextOff : srcDlc.Count > 0 || dlcCount > 0 ? Good : TextDim, L.T("ADD"));
            DrawInstRow(g, 3, L.T("Desktop shortcut"), L.T("created after installation"), TextDim,
                L.T(S.Bool("desktop_shortcut") ? "YES" : "NO"));

            float by = InstTop + InstRows * InstRowH + 22;
            if (installing)
            {
                using (var tb = new SolidBrush(TextLight))
                {
                    string text = string.Format("{0} - {1:0}%", instStep, instProgress * 100);
                    g.DrawString(text, fLabel, tb, new RectangleF(ColX, by - 6, ColW, 22), CenterFmt());
                }
                Progress(g, new RectangleF(ColX + 40, by + 24, ColW - 80, 4), (float)instProgress);
                var cancel = new RectangleF((W - 180) / 2, by + 46, 180, 34);
                FlatButton(g, cancel, L.T(cancelInstall ? "CANCELLING…" : "CANCEL"), fBtn, false, Lit(instSel, InstRows) || IsHover("install"), cancelInstall);
                AddHot(cancel, "install", () => ActivateInstall(InstRows), InstRows);
            }
            else
            {
                bool nothing = Installed && srcGame == null && srcUpdate == null && srcDlc.Count == 0;
                var btn = new RectangleF((W - 300) / 2, by, 300, 54);
                FlatButton(g, btn, L.T(nothing ? "EVERYTHING IS INSTALLED" : "INSTALL"), nothing ? fBtn : fInstall, true,
                    CanInstall && (Lit(instSel, InstRows) || IsHover("install")), !CanInstall);
                AddHot(btn, "install", () => ActivateInstall(InstRows), InstRows);
                if (instError != null)
                {
                    using (var eb = new SolidBrush(Bad))
                    {
                        g.DrawString(instError, fDesc, eb, new RectangleF(ColX + 6, by + 60, ColW - 12, 36), CenterFmt2());
                    }
                }
            }

            using (var line = new Pen(Hairline, 1f))
            {
                g.DrawLine(line, ColX + 6, DescTop + 12, ColX + ColW - 6, DescTop + 12);
            }
            using (var b = new SolidBrush(TextDim))
            {
                g.DrawString(L.T(InstDesc[Math.Max(0, Math.Min(InstRows, instSel))]), fDesc, b, new RectangleF(ColX + 6, DescTop + 20, ColW - 12, 70));
            }
        }

        // Centred, wrapping (error texts can take two lines).
        static StringFormat CenterFmt2()
        {
            return new StringFormat { Alignment = StringAlignment.Center, LineAlignment = StringAlignment.Near, Trimming = StringTrimming.EllipsisCharacter };
        }

        void DrawInstRow(Graphics g, int index, string label, string status, Color statusColor, string button)
        {
            var rr = new RectangleF(ColX, InstTop + index * InstRowH, ColW, InstRowH);
            bool enabled = InstRowEnabled(index);
            bool selected = Lit(instSel, index) && enabled;
            using (var b = new SolidBrush(Color.FromArgb(0, 0, 0, 0)))
            {
                g.FillRectangle(b, rr);
            }
            if (selected && !installing)
            {
                SelectionBar(g, rr);
            }
            const float btnW = 128;
            var fmt = new StringFormat { Trimming = StringTrimming.EllipsisCharacter, FormatFlags = StringFormatFlags.NoWrap };
            var lr = new RectangleF(ColX + 20, rr.Y + 9, ColW - btnW - 50, 22);
            var sr = new RectangleF(ColX + 20, rr.Y + 32, ColW - btnW - 50, 18);
            if (selected && !installing)
            {
                // The current row: brighter label.
                DrawText(g, label, fLabel, PaperHi, lr, fmt);
                DrawText(g, status, fSmall, statusColor, sr, fmt);
            }
            else
            {
                using (var lb = new SolidBrush(!enabled ? TextOff : installing ? TextDim : TextLight))
                using (var sb = new SolidBrush(statusColor))
                {
                    g.DrawString(label, fLabel, lb, lr, fmt);
                    g.DrawString(status, fSmall, sb, sr, fmt);
                }
            }
            var br = new RectangleF(rr.Right - btnW - 14, rr.Y + 13, btnW, 32);
            FlatButton(g, br, button, fBtn, false, selected && !installing, installing || !enabled);
            if (!installing && enabled)
            {
                AddHot(rr, "inst" + index, () => ActivateInstall(index), index);
            }
        }
    }
}
