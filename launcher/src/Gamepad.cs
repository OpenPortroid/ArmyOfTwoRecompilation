using System;
using System.Runtime.InteropServices;

namespace AO2Launcher
{
    // Xbox controller navigation for the launcher (XInput, first connected pad).
    sealed class Gamepad
    {
        public const int DpadUp = 0x0001, DpadDown = 0x0002, DpadLeft = 0x0004, DpadRight = 0x0008;
        public const int Start = 0x0010, Back = 0x0020, LB = 0x0100, RB = 0x0200;
        public const int A = 0x1000, B = 0x2000, X = 0x4000, Y = 0x8000;
        public const int L3 = 0x0040, R3 = 0x0080, LT = 0x10000, RT = 0x20000; // RawPressed only

        [StructLayout(LayoutKind.Sequential)]
        struct XINPUT_GAMEPAD
        {
            public ushort wButtons;
            public byte bLeftTrigger, bRightTrigger;
            public short sThumbLX, sThumbLY, sThumbRX, sThumbRY;
        }

        [StructLayout(LayoutKind.Sequential)]
        struct XINPUT_STATE
        {
            public uint dwPacketNumber;
            public XINPUT_GAMEPAD Gamepad;
        }

        [DllImport("xinput1_4.dll", EntryPoint = "XInputGetState")]
        static extern int XInputGetState14(int index, out XINPUT_STATE state);

        [DllImport("xinput9_1_0.dll", EntryPoint = "XInputGetState")]
        static extern int XInputGetState910(int index, out XINPUT_STATE state);

        int api = 14; // 14, 910, or 0 = no XInput at all
        int pad = -1;
        int last;
        int repeatButton;
        DateTime repeatAt;
        DateTime nextScan = DateTime.MinValue;

        // Fires for each newly pressed button; direction buttons (D-pad and left
        // stick) repeat while held.
        public event Action<int> Pressed;
        // Fires for each newly pressed control as it is (no stick as D-pad, the triggers as LT / RT):
        // assigning a gamepad button. Before Pressed for the same press.
        public event Action<int> RawPressed;
        int lastRaw;

        public bool Connected
        {
            get { return pad >= 0; }
        }

        bool Read(int index, out XINPUT_STATE st)
        {
            st = new XINPUT_STATE();
            try
            {
                if (api == 14)
                {
                    return XInputGetState14(index, out st) == 0;
                }
                if (api == 910)
                {
                    return XInputGetState910(index, out st) == 0;
                }
            }
            catch (DllNotFoundException)
            {
                api = api == 14 ? 910 : 0;
            }
            catch (EntryPointNotFoundException)
            {
                api = api == 14 ? 910 : 0;
            }
            return false;
        }

        public void Poll(bool active)
        {
            if (api == 0)
            {
                return;
            }
            XINPUT_STATE st = new XINPUT_STATE();
            if (pad < 0 || !Read(pad, out st))
            {
                pad = -1;
                // Probing absent pads is slow (it stalls the UI thread); rescan every 1.5 s.
                if (DateTime.UtcNow < nextScan)
                {
                    return;
                }
                nextScan = DateTime.UtcNow.AddMilliseconds(1500);
                for (int i = 0; i < 4 && pad < 0; i++)
                {
                    if (Read(i, out st))
                    {
                        pad = i;
                    }
                }
                if (pad < 0)
                {
                    last = 0;
                    lastRaw = 0;
                    return;
                }
            }

            int raw = st.Gamepad.wButtons;
            if (st.Gamepad.bLeftTrigger > 60) raw |= LT;
            if (st.Gamepad.bRightTrigger > 60) raw |= RT;
            int rawPressed = raw & ~lastRaw;
            lastRaw = raw;
            if (active && RawPressed != null)
            {
                for (int bit = 1; bit <= RT; bit <<= 1)
                {
                    if ((rawPressed & bit) != 0)
                    {
                        RawPressed(bit);
                    }
                }
            }

            int b = st.Gamepad.wButtons;
            const int dead = 16000;
            if (st.Gamepad.sThumbLY > dead) b |= DpadUp;
            if (st.Gamepad.sThumbLY < -dead) b |= DpadDown;
            if (st.Gamepad.sThumbLX < -dead) b |= DpadLeft;
            if (st.Gamepad.sThumbLX > dead) b |= DpadRight;

            int pressed = b & ~last;
            last = b;
            if (!active)
            {
                repeatButton = 0;
                return;
            }
            DateTime now = DateTime.UtcNow;
            for (int bit = 1; bit <= 0x8000; bit <<= 1)
            {
                if ((pressed & bit) == 0)
                {
                    continue;
                }
                if ((bit & (DpadUp | DpadDown | DpadLeft | DpadRight)) != 0)
                {
                    repeatButton = bit;
                    repeatAt = now.AddMilliseconds(380);
                }
                if (Pressed != null)
                {
                    Pressed(bit);
                }
            }
            if (repeatButton != 0)
            {
                if ((b & repeatButton) == 0)
                {
                    repeatButton = 0;
                }
                else if (now >= repeatAt)
                {
                    repeatAt = now.AddMilliseconds(110);
                    if (Pressed != null)
                    {
                        Pressed(repeatButton);
                    }
                }
            }
        }
    }
}
