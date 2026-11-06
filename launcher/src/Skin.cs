using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Linq;
using System.Windows.Forms;

namespace AO2Launcher
{
    // The launcher's look: the art behind rounded panels of dark glass, one cold accent, sentence case.
    // The blur under the glass is baked into the backdrops (GDI+ has none), so a repaint stays a few
    // milliseconds: solid fills over one cached bitmap.
    sealed partial class LauncherForm
    {
        static readonly Color InkDeep = Color.FromArgb(7, 16, 31);
        static readonly Color InkPanel = Color.FromArgb(14, 26, 48);
        static readonly Color InkRaise = Color.FromArgb(28, 44, 72);
        static readonly Color Paper = Color.FromArgb(213, 222, 234);
        static readonly Color PaperHi = Color.FromArgb(244, 247, 251);
        static readonly Color Accent = Color.FromArgb(143, 211, 255);
        static readonly Color Hairline = Color.FromArgb(38, 255, 255, 255);
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
        // Rounded shapes

        static GraphicsPath RoundPath(RectangleF r, float radius)
        {
            float d = Math.Min(radius * 2, Math.Min(r.Width, r.Height));
            var p = new GraphicsPath();
            p.AddArc(r.X, r.Y, d, d, 180, 90);
            p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
            p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
            p.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
            p.CloseFigure();
            return p;
        }

        static void FillRound(Graphics g, Color c, RectangleF r, float radius)
        {
            using (GraphicsPath p = RoundPath(r, radius))
            using (var b = new SolidBrush(c))
            {
                g.FillPath(b, p);
            }
        }

        static void OutlineRound(Graphics g, Color c, RectangleF r, float radius, float w = 1f)
        {
            using (GraphicsPath p = RoundPath(new RectangleF(r.X + w / 2, r.Y + w / 2, r.Width - w, r.Height - w), radius))
            using (var pen = new Pen(c, w))
            {
                g.DrawPath(pen, p);
            }
        }

        // ------------------------------------------------------------------
        // Text: the strings are keyed by their capitals; they are drawn in sentence case.

        static readonly HashSet<string> KeepCaps = new HashSet<string>
        {
            "DLC", "FPS", "AVX1", "AVX2", "IP", "TU2", "HDR", "FXAA", "SMAA", "SSAO", "HBAO", "GTAO", "LAN", "USB", "PC", "ПК",
        };

        // "FULL SCREEN" -> "Full screen"; text with a small letter, numbers and abbreviations stay as they are.
        static string Sentence(string s)
        {
            if (string.IsNullOrEmpty(s) || s.Any(char.IsLower))
            {
                return s;
            }
            if (s == "ARMY OF TWO")
            {
                return "Army of Two";
            }
            string[] words = s.Split(' ');
            bool first = true;
            for (int i = 0; i < words.Length; i++)
            {
                string w = words[i];
                if (!w.Any(char.IsLetter))
                {
                    continue;
                }
                if (!KeepCaps.Contains(w.TrimEnd(':', '…', ',', '.')) && !w.Any(char.IsDigit))
                {
                    w = w.ToLowerInvariant();
                    if (first)
                    {
                        w = char.ToUpperInvariant(w[0]) + w.Substring(1);
                    }
                }
                words[i] = w;
                first = false;
            }
            return string.Join(" ", words);
        }

        // Caps keeps its name: key caps and button glyphs (fBack) stay capitals, the rest goes to sentence
        // case with almost no extra spacing.
        void CapsStyle(ref string text, Font font, ref float spacing)
        {
            if (font != fBack)
            {
                text = Sentence(text);
                spacing *= 0.08f;
            }
        }

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
            CapsStyle(ref text, font, ref spacing);
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
            CapsStyle(ref text, font, ref spacing);
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

        // Primary: a white block with dark lettering. Others: faint glass, brighter and ringed when focused.
        void FlatButton(Graphics g, RectangleF r, string text, Font font, bool primary, bool active, bool disabled)
        {
            bool down = pressed != null && hover != null && pressed == hover.Id && hover.R == r;
            float radius = Math.Min(14, r.Height * 0.3f);
            Color fill, tc;
            if (disabled)
            {
                fill = Color.FromArgb(18, 255, 255, 255);
                tc = Color.FromArgb(130, 142, 160);
            }
            else if (primary)
            {
                fill = down ? Color.FromArgb(191, 230, 255) : active ? PaperHi : Color.FromArgb(226, 232, 240);
                tc = InkDeep;
            }
            else
            {
                fill = down ? Color.FromArgb(62, 255, 255, 255) : active ? Color.FromArgb(44, 255, 255, 255) : Color.FromArgb(24, 255, 255, 255);
                tc = PaperHi;
            }
            FillRound(g, fill, r, radius);
            if (active && !disabled)
            {
                OutlineRound(g, primary ? Color.FromArgb(150, Accent) : Color.FromArgb(170, Accent), primary ? RectangleF.Inflate(r, 3, 3) : r, radius + (primary ? 3 : 0), 1.5f);
            }
            Caps(g, text, font, tc, r, 0);
        }

        // ------------------------------------------------------------------
        // Chrome

        // Nothing behind it: the art or the backdrop goes up to the window's edge.
        void DrawTitleBar(Graphics g, string caption, bool withBack, bool backEnabled)
        {
            float tx = 24;
            if (withBack)
            {
                var back = new RectangleF(12, 7, 92, 30);
                bool hb = backEnabled && IsHover("back");
                if (hb)
                {
                    FillRound(g, Color.FromArgb(pressed == "back" ? 50 : 30, 255, 255, 255), back, 9);
                }
                Color ac = !backEnabled ? Color.FromArgb(120, 130, 146) : hb ? PaperHi : Paper;
                Chevron(g, back.X + 16, back.Y + back.Height / 2, -1, ac, 4.5f);
                var bf = new StringFormat { LineAlignment = StringAlignment.Center };
                DrawText(g, Sentence(L.T("BACK")), fTitleSub, ac, new RectangleF(back.X + 26, back.Y, back.Width - 28, back.Height), bf);
                if (backEnabled)
                {
                    AddHot(back, "back", () => SetScreen(UiScreen.Main));
                }
                tx = 118;
            }

            Caps(g, "ARMY OF TWO", fTitle, PaperHi, new RectangleF(tx, 0, 300, 44), 0, StringAlignment.Near);
            if (withBack)
            {
                float tw = CapsWidth(g, "ARMY OF TWO", fTitle, 0);
                var fmt = new StringFormat { LineAlignment = StringAlignment.Center };
                DrawText(g, L.T(caption), fTitleSub, Color.FromArgb(176, 188, 204), new RectangleF(tx + tw + 12, 0, 300, 44), fmt);
            }
            DrawWindowButtons(g);
        }

        void DrawWindowButtons(Graphics g)
        {
            var min = new RectangleF(W - 92, 0, 46, 40);
            var close = new RectangleF(W - 46, 0, 46, 40);
            if (IsHover("min"))
            {
                Fill(g, Color.FromArgb(40, 255, 255, 255), min);
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
        void Panel(Graphics g, RectangleF r, bool modal = false)
        {
            FillRound(g, modal ? InkPanel : Color.FromArgb(232, InkPanel), r, 16);
            OutlineRound(g, modal ? Color.FromArgb(70, 255, 255, 255) : Hairline, r, 16);
        }

        // The current row: a faint lift and a paper bar on the left.
        void SelectionBar(Graphics g, RectangleF rr)
        {
            FillRound(g, Color.FromArgb(46, Accent), rr, 10);
        }

        void SectionHeader(Graphics g, string text, float x, float y, float w)
        {
            Caps(g, text, fHead, PaperHi, new RectangleF(x, y, w, 30), 0, StringAlignment.Near);
        }

        void Progress(Graphics g, RectangleF r, float t)
        {
            t = Math.Max(0, Math.Min(1, t));
            r = new RectangleF(r.X, r.Y - 1, r.Width, r.Height + 2);
            FillRound(g, Color.FromArgb(44, 255, 255, 255), r, r.Height / 2);
            if (t > 0)
            {
                FillRound(g, Accent, new RectangleF(r.X, r.Y, Math.Max(r.Height, r.Width * t), r.Height), r.Height / 2);
            }
        }

        // A small rounded label ending at right, centred on cy: a hairline outline and capitals. Returns its width.
        float Pill(Graphics g, string text, float right, float cy, Color color)
        {
            float w = CapsWidth(g, text, fSection, 1.6f) + 20, h = 20;
            var r = new RectangleF(right - w, cy - h / 2, w, h);
            var st = g.Save();
            g.SmoothingMode = SmoothingMode.AntiAlias;
            using (var path = new GraphicsPath())
            using (var pen = new Pen(Color.FromArgb(150, color), 1f))
            using (var fill = new SolidBrush(Color.FromArgb(28, color)))
            {
                path.AddArc(r.X, r.Y, h, h, 90, 180);
                path.AddArc(r.Right - h, r.Y, h, h, 270, 180);
                path.CloseFigure();
                g.FillPath(fill, path);
                g.DrawPath(pen, path);
            }
            g.Restore(st);
            Caps(g, text, fSection, color, r, 1.6f);
            return w;
        }

        void Dot(Graphics g, float x, float cy, bool on, string text)
        {
            var r = new RectangleF(x, cy - 4, 8, 8);
            using (var b = new SolidBrush(on ? Color.FromArgb(120, 200, 140) : Color.FromArgb(70, 84, 104)))
            {
                g.FillEllipse(b, r);
            }
            var fmt = new StringFormat { LineAlignment = StringAlignment.Center };
            DrawText(g, text, fTiny, Color.FromArgb(176, 188, 204), new RectangleF(x + 14, cy - 10, 260, 20), fmt);
        }

        // A status pill over the art, ending at right, optionally a button. Returns its left edge.
        float Chip(Graphics g, string text, float right, float cy, Color color, string id = null, Action click = null)
        {
            float w = g.MeasureString(text, fSmall).Width + 22, h = 28;
            var r = new RectangleF(right - w, cy - h / 2, w, h);
            bool hv = id != null && IsHover(id);
            FillRound(g, hv ? Color.FromArgb(230, InkRaise) : Color.FromArgb(178, InkDeep), r, h / 2);
            DrawText(g, text, fSmall, color, r, CenterFmt());
            if (id != null)
            {
                AddHot(r, id, click);
            }
            return r.X;
        }

        // An on/off row's switch, centred in the value column's right end.
        void DrawSwitch(Graphics g, float right, float cy, bool on, bool enabled)
        {
            var r = new RectangleF(right - 48, cy - 13, 48, 26);
            Color back = !enabled ? Color.FromArgb(30, 255, 255, 255) : on ? Color.FromArgb(47, 134, 201) : Color.FromArgb(62, 255, 255, 255);
            FillRound(g, back, r, 13);
            using (var b = new SolidBrush(enabled ? PaperHi : Color.FromArgb(130, 142, 160)))
            {
                g.FillEllipse(b, on ? r.Right - 23 : r.X + 3, r.Y + 3, 20, 20);
            }
        }
    }
}
