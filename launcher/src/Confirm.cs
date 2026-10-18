using System;
using System.Drawing;
using System.Windows.Forms;

namespace AO2Launcher
{
    // A question over any screen: a title, a text, an optional check box (an extra the user has to choose
    // by hand, off at first) and two buttons. The rest of the window is dimmed and not clickable.
    sealed class Confirm
    {
        public string Title;
        public string Text;
        public string Check;       // the check box's text; null: none
        public bool Checked;
        public string Ok = "OK";
        public bool Danger;        // the title in red (deleting things)
        public Action<bool> OnOk;  // gets the check box's state
    }

    sealed partial class LauncherForm
    {
        Confirm confirm;
        int confirmSel; // 0 = OK, 1 = cancel, 2 = the check box

        void Ask(Confirm c)
        {
            ActiveControl = null;
            CloseDrop();
            confirm = c;
            confirmSel = 1; // a stray Enter does not confirm
            Invalidate();
        }

        void CloseConfirm()
        {
            confirm = null;
            Invalidate();
        }

        void ConfirmActivate(int i)
        {
            Confirm c = confirm;
            if (c == null)
            {
                return;
            }
            if (i == 2)
            {
                c.Checked = !c.Checked;
                Invalidate();
                return;
            }
            confirm = null;
            if (i == 0 && c.OnOk != null)
            {
                c.OnOk(c.Checked);
            }
            Invalidate();
        }

        // The order focus moves in: the check box (if any), OK, cancel.
        void ConfirmMove(int dir)
        {
            int[] order = confirm.Check != null ? new[] { 2, 0, 1 } : new[] { 0, 1 };
            int i = Array.IndexOf(order, confirmSel);
            confirmSel = order[(Math.Max(0, i) + dir + order.Length) % order.Length];
            Invalidate();
        }

        bool ConfirmKey(Keys key)
        {
            switch (key)
            {
                case Keys.Left:
                case Keys.Up:
                case Keys.Shift | Keys.Tab:
                    ConfirmMove(-1);
                    break;
                case Keys.Right:
                case Keys.Down:
                case Keys.Tab:
                    ConfirmMove(1);
                    break;
                case Keys.Enter:
                case Keys.Space:
                    ConfirmActivate(confirmSel);
                    break;
                case Keys.Escape:
                    CloseConfirm();
                    break;
            }
            return true;
        }

        void ConfirmPad(int b)
        {
            switch (b)
            {
                case Gamepad.DpadLeft:
                case Gamepad.DpadUp:
                    ConfirmMove(-1);
                    break;
                case Gamepad.DpadRight:
                case Gamepad.DpadDown:
                    ConfirmMove(1);
                    break;
                case Gamepad.A:
                    ConfirmActivate(confirmSel);
                    break;
                case Gamepad.B:
                    CloseConfirm();
                    break;
            }
        }

        void DrawConfirm(Graphics g)
        {
            Confirm c = confirm;
            Fill(g, Color.FromArgb(190, InkDeep), new RectangleF(0, 40, W, H - 40));
            bool check = c.Check != null;
            var box = new RectangleF((W - 560) / 2, check ? 170 : 186, 560, check ? 276 : 236);
            Panel(g, box);
            Caps(g, L.T(c.Title), fTitle, c.Danger ? Crimson : PaperHi, new RectangleF(box.X, box.Y + 22, box.Width, 22), 2.4f);
            var text = new StringFormat { Alignment = StringAlignment.Center };
            DrawText(g, c.Text, fLabel, TextLight, new RectangleF(box.X + 30, box.Y + 60, box.Width - 60, 96), text);

            if (check)
            {
                var row = new RectangleF(box.X + 40, box.Bottom - 116, box.Width - 80, 32);
                bool lit = confirmSel == 2 || IsHover("cf_check");
                if (lit)
                {
                    SelectionBar(g, row);
                }
                var cb = new RectangleF(row.X + 14, row.Y + 8, 16, 16);
                Outline(g, lit ? PaperHi : Color.FromArgb(150, Paper), cb);
                if (c.Checked)
                {
                    Fill(g, Crimson, new RectangleF(cb.X + 4, cb.Y + 4, 8, 8));
                }
                var fmt = new StringFormat { LineAlignment = StringAlignment.Center, Trimming = StringTrimming.EllipsisCharacter, FormatFlags = StringFormatFlags.NoWrap };
                DrawText(g, L.T(c.Check), fLabel, c.Checked ? Crimson : lit ? PaperHi : TextLight,
                    new RectangleF(cb.Right + 12, row.Y, row.Right - cb.Right - 16, row.Height), fmt);
                AddHot(row, "cf_check", () => ConfirmActivate(2));
            }

            var ok = new RectangleF(box.X + box.Width / 2 - 10 - 190, box.Bottom - 66, 190, 42);
            var cancel = new RectangleF(box.X + box.Width / 2 + 10, box.Bottom - 66, 190, 42);
            bool onButton = IsHover("cf_ok") || IsHover("cf_cancel");
            FlatButton(g, ok, L.T(c.Ok), fBtn, true, IsHover("cf_ok") || (!onButton && confirmSel == 0), false);
            FlatButton(g, cancel, L.T("CANCEL"), fBtn, false, IsHover("cf_cancel") || (!onButton && confirmSel == 1), false);
            AddHot(ok, "cf_ok", () => ConfirmActivate(0));
            AddHot(cancel, "cf_cancel", () => ConfirmActivate(1));
        }
    }
}
