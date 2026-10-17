using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

namespace AO2Launcher
{
    // The game controllers connected now, for the split-screen page: every HID
    // joystick / gamepad (Xbox, DualShock, DualSense, Switch Pro...), by its
    // product name, in the Raw Input order - usually the order the game finds
    // them in.
    static class Devices
    {
        [StructLayout(LayoutKind.Sequential)]
        struct RAWINPUTDEVICELIST
        {
            public IntPtr hDevice;
            public uint dwType;
        }

        [StructLayout(LayoutKind.Sequential)]
        struct RID_DEVICE_INFO_HID
        {
            public uint cbSize;
            public uint dwType;
            public uint dwVendorId;
            public uint dwProductId;
            public uint dwVersionNumber;
            public ushort usUsagePage;
            public ushort usUsage;
        }

        [DllImport("user32.dll", SetLastError = true)]
        static extern uint GetRawInputDeviceList([Out] RAWINPUTDEVICELIST[] list, ref uint count, uint size);

        [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        static extern uint GetRawInputDeviceInfo(IntPtr device, uint command, IntPtr data, ref uint size);

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        static extern IntPtr CreateFile(string name, uint access, uint share, IntPtr security, uint creation, uint flags, IntPtr template);

        [DllImport("kernel32.dll")]
        static extern bool CloseHandle(IntPtr handle);

        [DllImport("hid.dll", CharSet = CharSet.Unicode)]
        static extern bool HidD_GetProductString(IntPtr device, StringBuilder buffer, int length);

        const uint RIM_TYPEHID = 2;
        const uint RIDI_DEVICENAME = 0x20000007;
        const uint RIDI_DEVICEINFO = 0x2000000b;

        public static List<string> Gamepads()
        {
            var names = new List<string>();
            try
            {
                uint count = 0;
                uint size = (uint)Marshal.SizeOf(typeof(RAWINPUTDEVICELIST));
                GetRawInputDeviceList(null, ref count, size);
                if (count == 0)
                {
                    return names;
                }
                var list = new RAWINPUTDEVICELIST[count];
                if (GetRawInputDeviceList(list, ref count, size) == uint.MaxValue)
                {
                    return names;
                }
                for (int i = 0; i < count; i++)
                {
                    uint vendor;
                    if (list[i].dwType != RIM_TYPEHID || !IsGamepad(list[i].hDevice, out vendor))
                    {
                        continue;
                    }
                    names.Add(ProductName(list[i].hDevice, vendor));
                }
            }
            catch
            {
                // No Raw Input (or hid.dll): no names, the pages still work.
            }
            return names;
        }

        static bool IsGamepad(IntPtr device, out uint vendor)
        {
            vendor = 0;
            // RID_DEVICE_INFO: cbSize, dwType, then the HID part.
            uint size = 32;
            IntPtr buf = Marshal.AllocHGlobal(32);
            try
            {
                Marshal.WriteInt32(buf, 32);
                if ((int)GetRawInputDeviceInfo(device, RIDI_DEVICEINFO, buf, ref size) <= 0)
                {
                    return false;
                }
                var info = (RID_DEVICE_INFO_HID)Marshal.PtrToStructure(buf, typeof(RID_DEVICE_INFO_HID));
                vendor = info.dwVendorId;
                return info.usUsagePage == 1 && (info.usUsage == 4 || info.usUsage == 5);
            }
            finally
            {
                Marshal.FreeHGlobal(buf);
            }
        }

        // Without a product string (Xbox pads over Bluetooth, for one) the maker's name.
        static string ProductName(IntPtr device, uint vendor)
        {
            string fallback = vendor == 0x045E ? "Xbox Controller"
                : vendor == 0x054C ? "PlayStation Controller"
                : vendor == 0x057E ? "Nintendo Controller"
                : L.T("Game controller");
            uint chars = 0;
            GetRawInputDeviceInfo(device, RIDI_DEVICENAME, IntPtr.Zero, ref chars);
            if (chars == 0)
            {
                return fallback;
            }
            IntPtr buf = Marshal.AllocHGlobal((int)chars * 2 + 2);
            string path;
            try
            {
                if ((int)GetRawInputDeviceInfo(device, RIDI_DEVICENAME, buf, ref chars) <= 0)
                {
                    return fallback;
                }
                path = Marshal.PtrToStringUni(buf);
            }
            finally
            {
                Marshal.FreeHGlobal(buf);
            }
            // No access rights are needed for the product string.
            IntPtr h = CreateFile(path, 0, 3 /* read | write sharing */, IntPtr.Zero, 3 /* OPEN_EXISTING */, 0, IntPtr.Zero);
            if (h == IntPtr.Zero || h == new IntPtr(-1))
            {
                return fallback;
            }
            try
            {
                var sb = new StringBuilder(128);
                if (HidD_GetProductString(h, sb, sb.Capacity * 2) && sb.ToString().Trim().Length > 0)
                {
                    return sb.ToString().Trim();
                }
                return fallback;
            }
            finally
            {
                CloseHandle(h);
            }
        }
    }
}
