using System.Collections.Generic;
using System.Linq;
using System.Windows.Forms;

namespace AO2Launcher
{
    // Key and gamepad bindings: the game's keybind_* / padbind_* settings, as the
    // launcher shows and stores them (kb_* / pad_* in its own settings file).
    static class Bindings
    {
        public sealed class Bind
        {
            public string Name;    // the game setting is keybind_<Name> / padbind_<Name>
            public string Label;   // English, translated by L.T
            public string Default;

            public Bind(string name, string label, string def)
            {
                Name = name;
                Label = label;
                Default = def;
            }

            public string KbKey
            {
                get { return "kb_" + Name; }
            }

            public string PadKey
            {
                get { return "pad_" + Name; }
            }
        }

        // The keyboard: what each Xbox 360 control does in the game. Defaults as in settings\mapping.toml.
        public static readonly Bind[] Keyboard =
        {
            new Bind("lstick_up", "Move forward", "W"),
            new Bind("lstick_down", "Move back", "S"),
            new Bind("lstick_left", "Move left", "A"),
            new Bind("lstick_right", "Move right", "D"),
            new Bind("lstick_press", "Crouch (L3)", "Control,C"),
            new Bind("right_trigger", "Fire (RT)", "LMB"),
            new Bind("left_trigger", "Aim (LT)", "RMB"),
            new Bind("x", "Reload (X)", "R"),
            new Bind("y", "Roll / cover / vault (Y)", "Space"),
            new Bind("a", "Use / heal / confirm (A)", "E,Return"),
            new Bind("b", "Cancel / back (B)", "Q,Backspace"),
            new Bind("left_shoulder", "Inventory (LB)", "Z,1,2,3,4,G"),
            new Bind("right_shoulder", "Co-op commands (RB)", "F"),
            new Bind("rstick_press", "Right stick press (R3)", "MMB"),
            new Bind("dpad_up", "D-pad up: advance / primary weapon", "Up,1"),
            new Bind("dpad_down", "D-pad down: partner view / grenades", "Down,4,G"),
            new Bind("dpad_left", "D-pad left: regroup / secondary weapon", "Left,2"),
            new Bind("dpad_right", "D-pad right: hold position / special weapon", "Right,3"),
            new Bind("rstick_up", "Look up (right stick)", ""),
            new Bind("rstick_down", "Look down (right stick)", ""),
            new Bind("rstick_left", "Look left (right stick)", ""),
            new Bind("rstick_right", "Look right (right stick)", ""),
            new Bind("back", "GPS (Back)", "Tab"),
            new Bind("start", "Pause (Start)", "Escape"),
            new Bind("guide", "Guide button", ""),
        };

        // The gamepad: which physical control presses each control of the game.
        public static readonly Bind[] Pad =
        {
            new Bind("a", "A: use / confirm", "A"),
            new Bind("b", "B: cancel / back", "B"),
            new Bind("x", "X: reload", "X"),
            new Bind("y", "Y: roll / cover / vault", "Y"),
            new Bind("right_trigger", "RT: fire", "RT"),
            new Bind("left_trigger", "LT: aim", "LT"),
            new Bind("left_shoulder", "LB: inventory", "LB"),
            new Bind("right_shoulder", "RB: co-op commands", "RB"),
            new Bind("lstick_press", "L3: crouch", "L3"),
            new Bind("rstick_press", "R3", "R3"),
            new Bind("dpad_up", "D-pad up", "Up"),
            new Bind("dpad_down", "D-pad down", "Down"),
            new Bind("dpad_left", "D-pad left", "Left"),
            new Bind("dpad_right", "D-pad right", "Right"),
            new Bind("back", "Back: GPS", "Back"),
            new Bind("start", "Start: pause", "Start"),
        };

        // Physical gamepad controls (padbind_* values); "" = not bound.
        public static readonly string[] PadSources =
            { "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "L3", "R3", "Up", "Down", "Left", "Right", "Back", "Start", "" };

        public static string PadSourceName(string v)
        {
            switch (v)
            {
                case "": return "—";
                case "Up": return "D-PAD ↑";
                case "Down": return "D-PAD ↓";
                case "Left": return "D-PAD ←";
                case "Right": return "D-PAD →";
                case "Back": return "BACK";
                case "Start": return "START";
                default: return v;
            }
        }

        // An XInput button (Gamepad.Raw* bits) as a padbind_* value.
        public static string PadSourceOfBit(int bit)
        {
            switch (bit)
            {
                case Gamepad.DpadUp: return "Up";
                case Gamepad.DpadDown: return "Down";
                case Gamepad.DpadLeft: return "Left";
                case Gamepad.DpadRight: return "Right";
                case Gamepad.Start: return "Start";
                case Gamepad.Back: return "Back";
                case Gamepad.L3: return "L3";
                case Gamepad.R3: return "R3";
                case Gamepad.LB: return "LB";
                case Gamepad.RB: return "RB";
                case Gamepad.A: return "A";
                case Gamepad.B: return "B";
                case Gamepad.X: return "X";
                case Gamepad.Y: return "Y";
                case Gamepad.LT: return "LT";
                case Gamepad.RT: return "RT";
            }
            return null;
        }

        // A key as the game names it (rexruntime's key table), or null for one it does not know.
        public static string KeyName(Keys k)
        {
            if (k >= Keys.A && k <= Keys.Z)
            {
                return ((char)('A' + (k - Keys.A))).ToString();
            }
            if (k >= Keys.D0 && k <= Keys.D9)
            {
                return ((char)('0' + (k - Keys.D0))).ToString();
            }
            if (k >= Keys.F1 && k <= Keys.F24)
            {
                return "F" + (k - Keys.F1 + 1);
            }
            if (k >= Keys.NumPad0 && k <= Keys.NumPad9)
            {
                return "Numpad" + (k - Keys.NumPad0);
            }
            switch (k)
            {
                case Keys.Oemtilde: return "Backtick";
                case Keys.OemMinus: return "Minus";
                case Keys.Oemplus: return "Plus";
                case Keys.Oemcomma: return "Comma";
                case Keys.OemPeriod: return "Period";
                case Keys.OemSemicolon: return "Semicolon";
                case Keys.OemQuestion: return "Slash";
                case Keys.OemPipe: return "Backslash";
                case Keys.OemBackslash: return "Backslash";
                case Keys.OemOpenBrackets: return "LBracket";
                case Keys.OemCloseBrackets: return "RBracket";
                case Keys.OemQuotes: return "Quote";
                case Keys.Escape: return "Escape";
                case Keys.Return: return "Return";
                case Keys.Space: return "Space";
                case Keys.Tab: return "Tab";
                case Keys.Back: return "Backspace";
                case Keys.Delete: return "Delete";
                case Keys.Insert: return "Insert";
                case Keys.Home: return "Home";
                case Keys.End: return "End";
                case Keys.PageUp: return "PageUp";
                case Keys.PageDown: return "PageDown";
                case Keys.Left: return "Left";
                case Keys.Right: return "Right";
                case Keys.Up: return "Up";
                case Keys.Down: return "Down";
                case Keys.ShiftKey:
                case Keys.LShiftKey:
                case Keys.RShiftKey:
                    return "Shift";
                case Keys.ControlKey:
                case Keys.LControlKey:
                case Keys.RControlKey:
                    return "Control";
                case Keys.Menu:
                case Keys.LMenu:
                case Keys.RMenu:
                    return "Alt";
                case Keys.Add: return "NumpadPlus";
                case Keys.Subtract: return "NumpadMinus";
                case Keys.Multiply: return "NumpadStar";
                case Keys.Divide: return "NumpadSlash";
                case Keys.Pause: return "Pause";
                case Keys.CapsLock: return "CapsLock";
                case Keys.NumLock: return "NumLock";
                case Keys.Scroll: return "ScrollLock";
            }
            return null;
        }

        public static bool IsModifier(string name)
        {
            return name == "Shift" || name == "Control" || name == "Alt";
        }

        public static string MouseName(MouseButtons b)
        {
            switch (b)
            {
                case MouseButtons.Left: return "LMB";
                case MouseButtons.Right: return "RMB";
                case MouseButtons.Middle: return "MMB";
                case MouseButtons.XButton1: return "MB4";
                case MouseButtons.XButton2: return "MB5";
            }
            return null;
        }

        public static List<string> Split(string value)
        {
            return (value ?? "").Split(',').Select(t => t.Trim()).Where(t => t.Length > 0).ToList();
        }

        // How a key looks in the launcher: "Control+C" as "CTRL+C", arrows as arrows.
        static string Caption(string token)
        {
            int plus = token.LastIndexOf('+');
            if (plus > 0 && plus < token.Length - 1)
            {
                return string.Join("+", token.Substring(0, plus).Split('+').Select(Caption)) + "+" + Caption(token.Substring(plus + 1));
            }
            switch (token)
            {
                case "LMB": return L.T("LMB");
                case "RMB": return L.T("RMB");
                case "MMB": return L.T("MMB");
                case "MB4": return L.T("MOUSE 4");
                case "MB5": return L.T("MOUSE 5");
                case "Up": return "↑";
                case "Down": return "↓";
                case "Left": return "←";
                case "Right": return "→";
                case "Control": return "CTRL";
                case "Return": return "ENTER";
                case "Escape": return "ESC";
                case "Backspace": return "BACKSPACE";
                case "Backtick": return "`";
                case "Minus": return "-";
                case "Plus": return "=";
                case "Comma": return ",";
                case "Period": return ".";
                case "Semicolon": return ";";
                case "Slash": return "/";
                case "Backslash": return "\\";
                case "LBracket": return "[";
                case "RBracket": return "]";
                case "Quote": return "'";
                case "PageUp": return "PG UP";
                case "PageDown": return "PG DN";
                case "CapsLock": return "CAPS";
                case "ScrollLock": return "SCRL";
                case "NumpadPlus": return "NUM +";
                case "NumpadMinus": return "NUM -";
                case "NumpadStar": return "NUM *";
                case "NumpadSlash": return "NUM /";
            }
            if (token.StartsWith("Numpad"))
            {
                return "NUM " + token.Substring(6);
            }
            return token.ToUpperInvariant();
        }

        public static string Display(string value)
        {
            List<string> keys = Split(value);
            return keys.Count == 0 ? "—" : string.Join("  ", keys.Select(Caption));
        }
    }
}
