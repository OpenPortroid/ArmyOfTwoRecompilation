using System;
using System.Collections.Generic;
using System.Drawing;
using System.Linq;
using System.Windows.Forms;

namespace AO2Launcher
{
    // The Keyboard, Gamepad and Split-Screen tabs: key and button assignment
    // (press the key or button to bind) and the two players' devices.
    sealed partial class LauncherForm
    {
        Row capturing;          // the bind row waiting for a key / button, or null
        bool captureAdd;        // the key goes next to the bound ones instead of replacing them
        DateTime captureUntil;  // waiting stops by itself
        string captureMod;      // a modifier key held alone (bound when it is let go without another key)
        int captureShown = -1;  // seconds left, as drawn

        List<string> pads = new List<string>(); // names of the connected game controllers
        DateTime padsAt = DateTime.MinValue;

        const string BindDesc = "Enter or click: press the new key or mouse button (hold Shift / Ctrl / Alt for a combination). " +
                                "→ or +: add one more key. Delete or ×: clear. Waiting stops after a few seconds.";
        const string PadBindDesc = "←→: choose the gamepad control that presses this button in the game. Enter or click: press it " +
                                   "on the gamepad (Xbox controllers). Applies to every gamepad; the game still shows the Xbox buttons.";

        List<Row> KeyboardRows()
        {
            var rows = Bindings.Keyboard.Select(b => new Row
            {
                Kind = RowKind.Bind, Label = b.Label, Key = b.KbKey, Desc = BindDesc,
            }).ToList();
            rows.Add(new Row
            {
                Kind = RowKind.Button, Label = "Key bindings", ButtonText = () => "RESET",
                Click = () =>
                {
                    foreach (var b in Bindings.Keyboard)
                    {
                        S[b.KbKey] = b.Default;
                    }
                    S.Save();
                    ShowToast(L.T("Default keys restored"), false);
                },
                Desc = "Return every key to the default layout (WASD, mouse)."
            });
            return rows;
        }

        List<Row> PadRows()
        {
            var rows = Bindings.Pad.Select(b => new Row
            {
                Kind = RowKind.Bind, Pad = true, Label = b.Label, Key = b.PadKey, Desc = PadBindDesc,
                Values = Bindings.PadSources,
            }).ToList();
            string[] sticks = { "LS", "RS" };
            string[] stickNames = { "LEFT STICK", "RIGHT STICK" };
            rows.Add(new Row
            {
                Kind = RowKind.Choice, Label = "Movement (left stick)", Key = "pad_left_stick", Values = sticks, Names = stickNames,
                Desc = "Which stick of the gamepad moves the character. Swap both to play with the sticks swapped."
            });
            rows.Add(new Row
            {
                Kind = RowKind.Choice, Label = "Camera (right stick)", Key = "pad_right_stick", Values = sticks, Names = stickNames,
                Desc = "Which stick of the gamepad turns the camera and aims."
            });
            rows.Add(Toggle("Invert movement stick Y", "pad_invert_left_y", "Up on the movement stick walks backwards."));
            rows.Add(Toggle("Invert camera stick Y", "pad_invert_right_y",
                "Up on the camera stick looks down (on top of the Invert Y option in the game itself)."));
            rows.Add(new Row
            {
                Kind = RowKind.Button, Label = "Gamepad layout", ButtonText = () => "RESET",
                Click = () =>
                {
                    foreach (var b in Bindings.Pad)
                    {
                        S[b.PadKey] = b.Default;
                    }
                    S["pad_left_stick"] = "LS";
                    S["pad_right_stick"] = "RS";
                    S["pad_invert_left_y"] = "0";
                    S["pad_invert_right_y"] = "0";
                    S.Save();
                    ShowToast(L.T("Default gamepad layout restored"), false);
                },
                Desc = "Return every gamepad button to the console layout."
            });
            return rows;
        }

        // The devices a player can have: the keyboard, every gamepad connected now (and the one chosen, if it is not),
        // any gamepad, and the keyboard with one of them.
        string[] InputValues(string current)
        {
            var v = new List<string> { "keyboard" };
            for (int i = 1; i <= Math.Min(4, pads.Count); i++)
            {
                v.Add("pad" + i);
            }
            v.Add("pads");
            for (int i = 1; i <= Math.Min(4, pads.Count); i++)
            {
                v.Add("keyboard,pad" + i);
            }
            v.Add("keyboard,pads");
            if (!v.Contains(current) && current.Length > 0)
            {
                v.Add(current);
            }
            return v.ToArray();
        }

        string InputName(string v)
        {
            bool kb = v.StartsWith("keyboard,");
            string pad = kb ? v.Substring(9) : v;
            if (v == "keyboard") return L.T("KEYBOARD + MOUSE");
            if (pad == "pads") return L.T(kb ? "KEYBOARD + ANY PAD" : "ANY GAMEPAD");
            int n;
            if (pad.Length == 4 && pad.StartsWith("pad") && int.TryParse(pad.Substring(3), out n))
            {
                string name = n <= pads.Count ? pads[n - 1] : L.T("not connected");
                return kb ? L.F("KEYBOARD + PAD {0}", n) + " · " + name : L.F("GAMEPAD {0}", n) + " · " + name;
            }
            return v;
        }

        // A device row: its choices follow the connected gamepads.
        Row InputRow(string label, string key, string desc, Func<bool> on)
        {
            var r = new Row { Kind = RowKind.Choice, Label = label, Key = key, Enabled = on, Desc = desc, Drop = true };
            r.Values = InputValues(S[key]);
            r.Name = i => InputName(r.Values[i]);
            return r;
        }

        void RefreshInputRows()
        {
            foreach (Row r in AllRows().Where(r => r.Drop && r != profileRow))
            {
                r.Values = InputValues(S[r.Key]);
            }
        }

        List<Row> SplitRows()
        {
            Func<bool> on = () => S.Bool("split_screen");
            return new List<Row>
            {
                Toggle("Split-screen", "split_screen",
                    "Two players on one PC, each with their own half of the screen. On Play the game starts at once, without " +
                    "looking for a network host. Off: every device controls player 1."),
                new Row
                {
                    Kind = RowKind.Text, Label = "Player 1 name", Key = "gamertag", MaxLen = 15,
                    Placeholder = () => Shorten(Environment.MachineName, 15),
                    Desc = "Gamertag of player 1 (up to 15 characters), the same as Network → Player name. Empty: the computer's name."
                },
                new Row
                {
                    Kind = RowKind.Text, Label = "Player 2 name", Key = "gamertag2", MaxLen = 15, Enabled = on,
                    Placeholder = () =>
                    {
                        string p1 = S["gamertag"].Trim().Length > 0 ? S["gamertag"].Trim() : Environment.MachineName;
                        return Shorten(p1, 12) + "(1)";
                    },
                    Desc = "Gamertag of player 2 (up to 15 characters); their own profile and settings. " +
                           "Empty: player 1's name with (1) after it."
                },
                InputRow("Player 1 controls", "p1_input",
                    "The devices of player 1. Gamepads are numbered in the order the game finds them (usually the order " +
                    "they were connected): if the players' pads turn out swapped, swap them here. " +
                    "Any gamepad: every gamepad the other player does not have.", on),
                InputRow("Player 2 controls", "p2_input",
                    "The devices of player 2. The mouse aims for whoever has the keyboard. " +
                    "Any gamepad: every gamepad the other player does not have.", on),
                new Row { Kind = RowKind.Info, Text = SplitInfo },
            };
        }

        // The connected controllers, what is wrong with the choice, and how to start.
        string SplitInfo()
        {
            var lines = new List<string>();
            lines.Add(pads.Count == 0
                ? L.T("No gamepads connected.")
                : L.T("Connected:") + " " + string.Join(",  ", pads.Select((n, i) => L.F("gamepad {0} - {1}", i + 1, n))));
            if (S.Bool("split_screen"))
            {
                List<string> p1 = Bindings.Split(S["p1_input"]), p2 = Bindings.Split(S["p2_input"]);
                if (p1.Contains("keyboard") && p2.Contains("keyboard"))
                {
                    lines.Add(L.T("Both players have the keyboard: it goes to player 1."));
                }
                foreach (string t in p1.Intersect(p2).Where(t => t.StartsWith("pad") && t != "pads"))
                {
                    lines.Add(L.F("Both players have gamepad {0}: it goes to player 1.", t.Substring(3)));
                }
                if (p1.Contains("pads") && p2.Contains("pads"))
                {
                    lines.Add(L.T("Any gamepad for both players: they all go to player 1."));
                }
                foreach (string t in p1.Concat(p2).Distinct().Where(t => t.Length == 4 && t.StartsWith("pad") && char.IsDigit(t[3])))
                {
                    int n = t[3] - '0';
                    if (n > pads.Count)
                    {
                        lines.Add(L.F("Gamepad {0} is not connected.", n));
                    }
                }
                lines.Add(L.T("In the game: Campaign Mode → Splitscreen; player 2 signs in with their device when asked."));
            }
            return string.Join("\n", lines);
        }

        // The controller list, every 2 s while the split-screen page is shown.
        void RefreshPads()
        {
            if (screen != UiScreen.Settings || !tabs[tab].Any(r => r.Key == "p1_input") || DateTime.UtcNow < padsAt)
            {
                return;
            }
            padsAt = DateTime.UtcNow.AddSeconds(2);
            List<string> now = Devices.Gamepads();
            if (!now.SequenceEqual(pads))
            {
                pads = now;
                RefreshInputRows();
                if (dropRow != null)
                {
                    OpenDrop(dropRow); // the list changed under it
                }
                LayoutRows();
            }
        }

        // ------------------------------------------------------------------
        // Assigning

        void StartCapture(Row r, bool add)
        {
            ActiveControl = null;
            capturing = r;
            captureAdd = add;
            captureMod = null;
            captureUntil = DateTime.UtcNow.AddSeconds(6);
            captureShown = -1;
            Invalidate();
        }

        // token: the key / button pressed, null to stop waiting.
        void FinishCapture(string token)
        {
            Row r = capturing;
            capturing = null;
            captureMod = null;
            if (r != null && token != null)
            {
                if (r.Pad)
                {
                    S[r.Key] = token;
                }
                else
                {
                    List<string> keys = captureAdd ? Bindings.Split(S[r.Key]) : new List<string>();
                    keys.Remove(token);
                    keys.Add(token);
                    S[r.Key] = string.Join(",", keys);
                }
                S.Save();
            }
            Invalidate();
        }

        void ClearBind(Row r)
        {
            if (capturing == r)
            {
                FinishCapture(null);
            }
            S[r.Key] = "";
            S.Save();
            Invalidate();
        }

        // Timer: the countdown, and waiting stops after a while.
        void TickCapture()
        {
            RefreshPads();
            if (capturing == null)
            {
                return;
            }
            double left = (captureUntil - DateTime.UtcNow).TotalSeconds;
            if (left <= 0)
            {
                FinishCapture(null);
                return;
            }
            if ((int)Math.Ceiling(left) != captureShown)
            {
                captureShown = (int)Math.Ceiling(left);
                Invalidate();
            }
        }

        string CaptureText(Row r)
        {
            return L.F(r.Pad ? "PRESS A BUTTON… {0}" : "PRESS A KEY… {0}", Math.Max(1, captureShown));
        }

        static string Modifiers(Keys mods)
        {
            string m = "";
            if ((mods & Keys.Control) != 0) m += "Control+";
            if ((mods & Keys.Shift) != 0) m += "Shift+";
            if ((mods & Keys.Alt) != 0) m += "Alt+";
            return m;
        }

        // A key while waiting (ProcessCmdKey). Every key binds, Esc too; a modifier alone binds when let go.
        bool CaptureKey(ref Message msg, Keys keyData)
        {
            if (capturing.Pad)
            {
                // Waiting for a gamepad button: Esc stops it, the rest is ignored.
                if ((keyData & Keys.KeyCode) == Keys.Escape)
                {
                    FinishCapture(null);
                }
                return true;
            }
            if ((msg.LParam.ToInt64() & 0x40000000) != 0)
            {
                return true; // auto-repeat of the key that started it
            }
            string name = Bindings.KeyName(keyData & Keys.KeyCode);
            if (name == null)
            {
                return true;
            }
            if (Bindings.IsModifier(name))
            {
                captureMod = name;
                return true;
            }
            FinishCapture(Modifiers(keyData & Keys.Modifiers) + name);
            return true;
        }

        const int WM_KEYUP = 0x101, WM_SYSKEYUP = 0x105;

        protected override void WndProc(ref Message m)
        {
            if (capturing != null && !capturing.Pad && captureMod != null && (m.Msg == WM_KEYUP || m.Msg == WM_SYSKEYUP))
            {
                string name = Bindings.KeyName((Keys)(m.WParam.ToInt64() & 0xFFFF));
                if (name == captureMod)
                {
                    // Alt let go alone would also open the window menu.
                    FinishCapture(name);
                    return;
                }
            }
            base.WndProc(ref m);
        }

        bool CaptureMouse(MouseButtons b)
        {
            if (capturing == null || capturing.Pad)
            {
                return false;
            }
            string name = Bindings.MouseName(b);
            if (name != null)
            {
                FinishCapture(Modifiers(ModifierKeys) + name);
            }
            return true;
        }

        void OnPadRaw(int bit)
        {
            if (capturing == null)
            {
                return;
            }
            if (!capturing.Pad)
            {
                if (bit == Gamepad.B)
                {
                    FinishCapture(null);
                }
                return;
            }
            string src = Bindings.PadSourceOfBit(bit);
            if (src != null)
            {
                FinishCapture(src);
            }
        }

        // ------------------------------------------------------------------
        // Scrolling (tabs longer than the panel)

        float scroll, maxScroll;

        float RowsView
        {
            get { return DescTop - 6 - RowsTop; }
        }

        bool RowShown(Row r)
        {
            return r.Y >= RowsTop - 0.5f && r.Y + r.H <= RowsTop + RowsView + 0.5f;
        }

        protected override void OnMouseWheel(MouseEventArgs e)
        {
            base.OnMouseWheel(e);
            if (NotesShown)
            {
                ScrollNotes(e.Delta > 0 ? -3 : 3);
                return;
            }
            if (screen != UiScreen.Settings || maxScroll <= 0 || capturing != null)
            {
                return;
            }
            float old = scroll;
            scroll = Math.Max(0, Math.Min(maxScroll, scroll - Math.Sign(e.Delta) * RowH * 3));
            if (scroll != old)
            {
                LayoutRows(false);
            }
        }

        void DrawScrollBar(Graphics g)
        {
            if (maxScroll <= 0)
            {
                return;
            }
            float x = ColX + ColW + 12, top = RowsTop, h = RowsView;
            float th = Math.Max(24, h * h / (h + maxScroll));
            float ty = top + (h - th) * scroll / maxScroll;
            DrawGrabBar(g, new RectangleF(x, top, 3, h), ty, th, 30, 150, f =>
            {
                float old = scroll;
                scroll = Math.Max(0, Math.Min(maxScroll, (float)Math.Round(f * maxScroll / RowH) * RowH));
                if (scroll != old)
                {
                    LayoutRows(false);
                }
            });
        }

        // The scroll bar that can be taken with the mouse: the one drawn last (Settings or the update notes).
        RectangleF sbTrack;
        float sbThumbY, sbThumbH, sbGrab;
        Action<float> sbSet; // 0..1 along the track -> scroll position; null: no bar shown
        bool sbDragging;

        // Draws a track and its thumb and makes them grabbable; `set` gets the wanted position 0..1.
        void DrawGrabBar(Graphics g, RectangleF track, float thumbY, float thumbH, int trackAlpha, int thumbAlpha, Action<float> set)
        {
            bool live = sbDragging || SbHit(Logical(PointToClient(Cursor.Position)));
            Fill(g, Color.FromArgb(trackAlpha, 255, 255, 255), track);
            Fill(g, live ? PaperHi : Color.FromArgb(thumbAlpha, Paper), new RectangleF(track.X, thumbY, track.Width, thumbH));
            sbTrack = track;
            sbThumbY = thumbY;
            sbThumbH = thumbH;
            sbSet = set;
        }

        // A wide strip around the thin bar, so it is easy to hit.
        bool SbHit(PointF p)
        {
            return sbSet != null && dropRow == null && confirm == null &&
                   new RectangleF(sbTrack.X - 10, sbTrack.Y, sbTrack.Width + 20, sbTrack.Height).Contains(p);
        }

        bool SbDown(PointF p)
        {
            if (!SbHit(p))
            {
                return false;
            }
            // On the thumb: keep the point that was grabbed; on the track: the thumb centres there.
            sbGrab = p.Y >= sbThumbY && p.Y <= sbThumbY + sbThumbH ? p.Y - sbThumbY : sbThumbH / 2;
            sbDragging = true;
            SbMove(p);
            return true;
        }

        void SbMove(PointF p)
        {
            float range = sbTrack.Height - sbThumbH;
            if (range <= 0 || sbSet == null)
            {
                return;
            }
            sbSet(Math.Max(0, Math.Min(1, (p.Y - sbGrab - sbTrack.Y) / range)));
            Invalidate();
        }

        // ------------------------------------------------------------------
        // Drop-down list (the device rows)

        Row dropRow;   // the row whose list is open, or null
        int dropSel;   // the lit item

        const float DropItemH = 30;

        float DropW
        {
            get { return dropRow != null && dropRow.DropName != null ? 520 : 360; }
        }

        void OpenDrop(Row r)
        {
            RefreshInputRows();
            dropRow = r;
            dropSel = Math.Max(0, Array.IndexOf(r.Values, S[r.Key]));
            LayoutRows(false);
        }

        void CloseDrop()
        {
            if (dropRow != null)
            {
                dropRow = null;
                LayoutRows(false);
            }
        }

        void PickDrop(int i)
        {
            Row r = dropRow;
            dropRow = null;
            if (r != null && i >= 0 && i < r.Values.Length && SetChoice(r, r.Values[i]))
            {
                S.Save();
            }
            LayoutRows();
        }

        RectangleF DropRect()
        {
            float h = dropRow.Values.Length * DropItemH + 8;
            float x = ColX + ColW - DropW;
            float y = dropRow.Y + dropRow.H;
            if (y + h > H - 20)
            {
                y = dropRow.Y - h; // no room below: above the row
            }
            return new RectangleF(x, y, DropW, h);
        }

        void DrawDrop(Graphics g)
        {
            if (dropRow == null)
            {
                return;
            }
            RectangleF box = DropRect();
            // Everything else: a click there only closes the list.
            AddHot(new RectangleF(0, 0, W, H), "drop_off", CloseDrop);
            Fill(g, InkDeep, box);
            Outline(g, Color.FromArgb(120, Paper), box);
            var fmt = new StringFormat { LineAlignment = StringAlignment.Center, Trimming = StringTrimming.EllipsisCharacter, FormatFlags = StringFormatFlags.NoWrap };
            int current = Array.IndexOf(dropRow.Values, S[dropRow.Key]);
            for (int i = 0; i < dropRow.Values.Length; i++)
            {
                var ir = new RectangleF(box.X + 4, box.Y + 4 + i * DropItemH, box.Width - 8, DropItemH);
                bool lit = i == dropSel || IsHover("drop" + i);
                if (lit)
                {
                    SelectionBar(g, ir);
                }
                string v = dropRow.Values[i];
                bool connected = !(v.EndsWith("pad1") || v.EndsWith("pad2") || v.EndsWith("pad3") || v.EndsWith("pad4")) ||
                                 v[v.Length - 1] - '0' <= pads.Count;
                Color c = !connected ? TextOff : lit ? PaperHi : TextLight;
                if (i == current)
                {
                    DrawText(g, "✓", fLabel, c, new RectangleF(ir.X + 10, ir.Y, 20, ir.Height), fmt);
                }
                string text = dropRow.DropName != null ? dropRow.DropName(i) : dropRow.ChoiceName(i);
                DrawText(g, text, fLabel, c, new RectangleF(ir.X + 32, ir.Y, ir.Width - 40, ir.Height), fmt);
                int pick = i;
                AddHot(ir, "drop" + i, () => PickDrop(pick));
            }
        }

        // Keys while the list is open; true when taken.
        bool DropKey(Keys keyData)
        {
            switch (keyData)
            {
                case Keys.Up:
                    dropSel = (dropSel + dropRow.Values.Length - 1) % dropRow.Values.Length;
                    Invalidate();
                    break;
                case Keys.Down:
                    dropSel = (dropSel + 1) % dropRow.Values.Length;
                    Invalidate();
                    break;
                case Keys.Enter:
                case Keys.Space:
                    PickDrop(dropSel);
                    break;
                case Keys.Escape:
                case Keys.Tab:
                    CloseDrop();
                    break;
            }
            return true;
        }

        void DropPad(int b)
        {
            switch (b)
            {
                case Gamepad.DpadUp: DropKey(Keys.Up); break;
                case Gamepad.DpadDown: DropKey(Keys.Down); break;
                case Gamepad.A: DropKey(Keys.Enter); break;
                case Gamepad.B: CloseDrop(); break;
            }
        }

        // The value of a bind row: the keys (or the gamepad control), + and × for the keyboard.
        void DrawBind(Graphics g, Row r, int index, string id, bool selected, Color valColor)
        {
            float vx = ValueX;
            bool cap = capturing == r;
            var rr = new RectangleF(ColX, r.Y, ColW, r.H);
            if (r.Pad)
            {
                string text = cap ? CaptureText(r) : Bindings.PadSourceName(S[r.Key]);
                AddHot(rr, id, () => Change(r, 0), index);
                DrawText(g, text, cap ? fBack : fValue, cap ? PaperHi : valColor, new RectangleF(vx + 16, r.Y, ValueW - 32, r.H), CenterFmt());
                if (!cap)
                {
                    var left = new RectangleF(vx - 6, r.Y, 24, r.H);
                    var right = new RectangleF(vx + ValueW - 18, r.Y, 24, r.H);
                    Color dim = selected ? Color.FromArgb(190, Paper) : Color.FromArgb(90, Paper);
                    Chevron(g, left.X + left.Width / 2, r.Y + r.H / 2, -1, IsHover(id + "<") ? PaperHi : dim);
                    Chevron(g, right.X + right.Width / 2, r.Y + r.H / 2, 1, IsHover(id + ">") ? PaperHi : dim);
                    AddHot(left, id + "<", () => Change(r, -1), index);
                    AddHot(right, id + ">", () => Change(r, 1), index);
                }
                return;
            }

            var box = new RectangleF(vx, r.Y + 5, ValueW - 46, r.H - 10);
            Fill(g, TextBoxBack, box);
            Outline(g, cap ? Paper : selected ? Color.FromArgb(110, Paper) : Color.FromArgb(50, Paper), box);
            string keys = cap ? CaptureText(r) : Bindings.Display(S[r.Key]);
            DrawText(g, keys, cap ? fBack : fSmall, cap ? PaperHi : valColor, new RectangleF(box.X + 4, box.Y, box.Width - 8, box.Height), CenterFmt());
            AddHot(rr, id, () => Change(r, 0), index);

            var add = new RectangleF(box.Right + 2, r.Y + 5, 21, r.H - 10);
            var clear = new RectangleF(add.Right + 1, r.Y + 5, 21, r.H - 10);
            Color dimc = selected ? Color.FromArgb(190, Paper) : Color.FromArgb(90, Paper);
            DrawText(g, "+", fValue, IsHover(id + "+") ? PaperHi : dimc, add, CenterFmt());
            DrawText(g, "×", fValue, IsHover(id + "x") ? PaperHi : dimc, clear, CenterFmt());
            AddHot(add, id + "+", () => StartCapture(r, true), index);
            AddHot(clear, id + "x", () => ClearBind(r), index);
        }
    }
}
