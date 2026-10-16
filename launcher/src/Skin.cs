using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace AO2Launcher
{
    // The launcher's look: flat and quiet, in the cover's tones (deep navy ink, warm grey paper).
    // Solid fills and hairlines only, so a repaint costs a few milliseconds; the only pictures are the
    // cover and the logo, baked once into the backdrops.
    sealed partial class LauncherForm
    {
        static readonly Color InkDeep = Color.FromArgb(6, 16, 34);
        static readonly Color InkPanel = Color.FromArgb(10, 24, 48);
        static readonly Color InkRaise = Color.FromArgb(22, 42, 74);
        static readonly Color Paper = Color.FromArgb(224, 220, 212);
        static readonly Color PaperHi = Color.FromArgb(246, 244, 238);
        static readonly Color Hairline = Color.FromArgb(34, 255, 255, 255);
        static readonly Color Crimson = Color.FromArgb(232, 104, 96);

        FontFamily famCaps, famLcd;

        // Letter-spaced captions as paths at the origin, built once per text and font.
        readonly Dictionary<string, GraphicsPath> capsCache = new Dictionary<string, GraphicsPath>();
        readonly Dictionary<string, RectangleF> capsBounds = new Dictionary<string, RectangleF>();

        void LoadSkin()
        {
        }

        static Color Mix(Color a, Color b, float t)
        {
            t = Math.Max(0, Math.Min(1, t));
            return Color.FromArgb(
                (int)(a.A + (b.A - a.A) * t), (int)(a.R + (b.R - a.R) * t),
                (int)(a.G + (b.G - a.G) * t), (int)(a.B + (b.B - a.B) * t));
        }

        static void Fill(Graphics g, Color c, RectangleF r)
        {
            using (var b = new SolidBrush(c))
            {
                g.FillRectangle(b, r);
            }
        }

        static void Line(Graphics g, Color c, float x0, float y0, float x1, float y1, float w = 1f)
        {
            using (var p = new Pen(c, w))
            {
                g.DrawLine(p, x0, y0, x1, y1);
            }
        }

        static void Outline(Graphics g, Color c, RectangleF r, float w = 1f)
        {
            using (var p = new Pen(c, w))
            {
                g.DrawRectangle(p, r.X + w / 2, r.Y + w / 2, r.Width - w, r.Height - w);
            }
        }

        static void DrawText(Graphics g, string text, Font font, Color color, RectangleF r, StringFormat fmt)
        {
            using (var b = new SolidBrush(color))
            {
                g.DrawString(text, font, b, r, fmt);
            }
        }

        // ------------------------------------------------------------------
        // Letter-spaced capitals

        GraphicsPath CapsPath(Graphics g, string text, Font font, float spacing)
        {
            string key = text + "\u0001" + font.Name + "\u0001" + font.Size + "\u0001" + font.Style + "\u0001" + spacing;
            GraphicsPath path;
            if (capsCache.TryGetValue(key, out path))
            {
                return path;
            }
            path = new GraphicsPath();
            var fmt = new StringFormat(StringFormat.GenericTypographic) { FormatFlags = StringFormatFlags.MeasureTrailingSpaces };
            float x = 0;
            foreach (char ch in text)
            {
                string s = ch.ToString();
                path.AddString(s, font.FontFamily, (int)font.Style, font.Size, new PointF(x, 0), StringFormat.GenericTypographic);
                x += g.MeasureString(s, font, PointF.Empty, fmt).Width + spacing;
            }
            // Vertical reference: the capitals' box (so captions sit optically centred).
            using (var cap = new GraphicsPath())
            {
                cap.AddString("H", font.FontFamily, (int)font.Style, font.Size, PointF.Empty, StringFormat.GenericTypographic);
                RectangleF cb = cap.GetBounds();
                capsBounds[key] = new RectangleF(0, cb.Y, Math.Max(0, x - spacing), cb.Height);
            }
            capsCache[key] = path;
            return path;
        }

        float CapsWidth(Graphics g, string text, Font font, float spacing)
        {
            string key = text + "\u0001" + font.Name + "\u0001" + font.Size + "\u0001" + font.Style + "\u0001" + spacing;
            CapsPath(g, text, font, spacing);
            return capsBounds[key].Width;
        }

        void Caps(Graphics g, string text, Font font, Color color, RectangleF r, float spacing, StringAlignment align = StringAlignment.Center)
        {
            if (text.Length == 0)
            {
                return;
            }
            GraphicsPath path = CapsPath(g, text, font, spacing);
            string key = text + "\u0001" + font.Name + "\u0001" + font.Size + "\u0001" + font.Style + "\u0001" + spacing;
            RectangleF b = capsBounds[key];
            float x = align == StringAlignment.Center ? r.X + (r.Width - b.Width) / 2 : align == StringAlignment.Near ? r.X : r.Right - b.Width;
            float y = r.Y + r.Height / 2 - (b.Y + b.Height / 2);
            var st = g.Save();
            g.TranslateTransform(x, y);
            using (var br = new SolidBrush(color))
            {
                g.FillPath(br, path);
            }
            g.Restore(st);
        }

        // ------------------------------------------------------------------
        // Buttons

        // Primary: a paper block with ink lettering. Others: a hairline frame with paper lettering,
        // filled lightly when focused. A press darkens the block for the instant feedback.
        void FlatButton(Graphics g, RectangleF r, string text, Font font, bool primary, bool active, bool disabled)
        {
            bool down = pressed != null && hover != null && pressed == hover.Id && hover.R == r;
            Color fill, border, tc;
            if (disabled)
            {
                fill = Color.FromArgb(14, 255, 255, 255);
                border = Color.FromArgb(40, 255, 255, 255);
                tc = Color.FromArgb(110, 120, 136);
            }
            else if (primary)
            {
                fill = down ? Paper : active ? PaperHi : Color.FromArgb(214, 210, 202);
                border = fill;
                tc = InkDeep;
            }
            else
            {
                fill = down ? Color.FromArgb(46, 255, 255, 255) : active ? Color.FromArgb(26, 255, 255, 255) : Color.FromArgb(0, 0, 0, 0);
                border = active ? Color.FromArgb(170, Paper) : Color.FromArgb(70, Paper);
                tc = active ? PaperHi : Color.FromArgb(214, 218, 226);
            }
            if (fill.A > 0)
            {
                Fill(g, fill, r);
            }
            Outline(g, border, r);
            if (active && primary && !disabled)
            {
                Outline(g, Color.FromArgb(90, PaperHi), RectangleF.Inflate(r, 3, 3));
            }
            Caps(g, text, font, tc, r, font.Size * 0.16f);
        }

        // ------------------------------------------------------------------
        // Chrome

        void DrawTitleBar(Graphics g, string caption, bool withBack, bool backEnabled)
        {
            Fill(g, InkDeep, new RectangleF(0, 0, W, 40));
            Line(g, Hairline, 0, 39.5f, W, 39.5f);

            float tx = 18;
            if (withBack)
            {
                var back = new RectangleF(8, 6, 86, 28);
                bool hb = backEnabled && IsHover("back");
                if (hb)
                {
                    Fill(g, Color.FromArgb(pressed == "back" ? 40 : 24, 255, 255, 255), back);
                }
                Color ac = !backEnabled ? Color.FromArgb(90, 100, 116) : hb ? PaperHi : Color.FromArgb(200, 206, 216);
                Chevron(g, back.X + 16, back.Y + back.Height / 2, -1, ac, 4.5f);
                Caps(g, L.T("BACK"), fBack, ac, new RectangleF(back.X + 28, back.Y, back.Width - 34, back.Height), 1.6f, StringAlignment.Near);
                if (backEnabled)
                {
                    AddHot(back, "back", () => SetScreen(UiScreen.Main));
                }
                Line(g, Hairline, 104.5f, 10, 104.5f, 30);
                tx = 118;
            }

            Caps(g, "ARMY OF TWO", fTitle, PaperHi, new RectangleF(tx, 0, 300, 40), 3f, StringAlignment.Near);
            float tw = CapsWidth(g, "ARMY OF TWO", fTitle, 3f);
            var fmt = new StringFormat { LineAlignment = StringAlignment.Center };
            DrawText(g, L.T(caption), fTitleSub, Color.FromArgb(140, 152, 170), new RectangleF(tx + tw + 14, 0, 300, 40), fmt);
            DrawWindowButtons(g);
        }

        void DrawWindowButtons(Graphics g)
        {
            var min = new RectangleF(W - 92, 0, 46, 40);
            var close = new RectangleF(W - 46, 0, 46, 39);
            if (IsHover("min"))
            {
                Fill(g, Color.FromArgb(22, 255, 255, 255), min);
            }
            if (IsHover("close"))
            {
                Fill(g, Color.FromArgb(196, 43, 28), close);
            }
            using (var pen = new Pen(Color.FromArgb(220, 226, 234), 1.1f))
            {
                float cx = min.X + min.Width / 2, cy = 20;
                g.DrawLine(pen, cx - 5, cy + 0.5f, cx + 5, cy + 0.5f);
                cx = close.X + close.Width / 2;
                g.DrawLine(pen, cx - 5, cy - 5, cx + 5, cy + 5);
                g.DrawLine(pen, cx + 5, cy - 5, cx - 5, cy + 5);
            }
            AddHot(min, "min", () => WindowState = FormWindowState.Minimized);
            AddHot(close, "close", Close);
        }

        void DrawFrame(Graphics g)
        {
            using (var p = new Pen(Color.FromArgb(70, 120, 140, 170), 1f / scale))
            {
                g.DrawRectangle(p, 0, 0, W - 1f / scale, H - 1f / scale);
            }
        }

        // ------------------------------------------------------------------
        // Small widgets

        static void Chevron(Graphics g, float cx, float cy, int dir, Color color, float s = 4f)
        {
            using (var p = new Pen(color, 1.6f) { StartCap = LineCap.Round, EndCap = LineCap.Round, LineJoin = LineJoin.Round })
            {
                float x = cx + dir * s * 0.5f;
                g.DrawLines(p, new[] { new PointF(x - dir * s, cy - s), new PointF(x, cy), new PointF(x - dir * s, cy + s) });
            }
        }

        // A dark panel with a hairline frame (the settings and install screens' column).
        void Panel(Graphics g, RectangleF r)
        {
            Fill(g, Color.FromArgb(236, InkPanel), r);
            Outline(g, Hairline, r);
        }

        // The current row: a faint lift and a paper bar on the left.
        void SelectionBar(Graphics g, RectangleF rr)
        {
            Fill(g, Color.FromArgb(18, 255, 255, 255), rr);
            Fill(g, Paper, new RectangleF(rr.X, rr.Y, 2, rr.Height));
        }

        void SectionHeader(Graphics g, string text, float x, float y, float w)
        {
            Caps(g, text, fSection, Color.FromArgb(140, 152, 170), new RectangleF(x, y, w, 18), 2.2f, StringAlignment.Near);
        }

        void Progress(Graphics g, RectangleF r, float t)
        {
            t = Math.Max(0, Math.Min(1, t));
            Fill(g, Color.FromArgb(40, 255, 255, 255), r);
            Fill(g, Paper, new RectangleF(r.X, r.Y, r.Width * t, r.Height));
        }

        void Dot(Graphics g, float x, float cy, bool on, string text)
        {
            var r = new RectangleF(x, cy - 4, 8, 8);
            using (var b = new SolidBrush(on ? Color.FromArgb(120, 200, 140) : Color.FromArgb(70, 84, 104)))
            {
                g.FillEllipse(b, r);
            }
            var fmt = new StringFormat { LineAlignment = StringAlignment.Center };
            DrawText(g, text, fTiny, Color.FromArgb(130, 142, 160), new RectangleF(x + 14, cy - 10, 260, 20), fmt);
        }
    }
}
