using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace AO2Launcher
{
    // The video devices as DXGI lists them: the game picks its device from the same list by the same name
    // (--ao2_native_gpu_adapter), so the names match exactly. The calls go straight through the COM tables,
    // there is no managed DXGI in .NET Framework.
    static class VideoAdapters
    {
        public sealed class Info
        {
            public string Name;
            public long Vram; // dedicated video memory, bytes
        }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        struct AdapterDesc1
        {
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
            public string Description;
            public uint VendorId, DeviceId, SubSysId, Revision;
            public UIntPtr DedicatedVideoMemory, DedicatedSystemMemory, SharedSystemMemory;
            public uint LuidLow;
            public int LuidHigh;
            public uint Flags;
        }

        [DllImport("dxgi.dll")]
        static extern int CreateDXGIFactory1(ref Guid riid, out IntPtr factory);

        delegate int EnumAdapters1Fn(IntPtr self, uint index, out IntPtr adapter);
        delegate int GetDesc1Fn(IntPtr self, out AdapterDesc1 desc);
        delegate uint ReleaseFn(IntPtr self);

        const uint AdapterFlagSoftware = 2;

        static T Slot<T>(IntPtr obj, int index) where T : class
        {
            IntPtr table = Marshal.ReadIntPtr(obj);
            IntPtr fn = Marshal.ReadIntPtr(table, index * IntPtr.Size);
            return (T)(object)Marshal.GetDelegateForFunctionPointer(fn, typeof(T));
        }

        static List<Info> list;

        // The hardware adapters in DXGI's order (empty when DXGI is not there).
        public static List<Info> List()
        {
            if (list != null)
            {
                return list;
            }
            var result = new List<Info>();
            IntPtr factory = IntPtr.Zero;
            try
            {
                Guid iid = new Guid("770aae78-f26f-4dba-a829-253c83d1b387"); // IDXGIFactory1
                if (CreateDXGIFactory1(ref iid, out factory) >= 0)
                {
                    var enumerate = Slot<EnumAdapters1Fn>(factory, 12);
                    for (uint i = 0; ; i++)
                    {
                        IntPtr adapter;
                        if (enumerate(factory, i, out adapter) < 0)
                        {
                            break;
                        }
                        try
                        {
                            AdapterDesc1 d;
                            // The same name twice (one device listed twice, e.g. an integrated one): the game takes the first.
                            if (Slot<GetDesc1Fn>(adapter, 10)(adapter, out d) >= 0 && (d.Flags & AdapterFlagSoftware) == 0 &&
                                !string.IsNullOrEmpty(d.Description) && !result.Exists(x => x.Name == d.Description))
                            {
                                result.Add(new Info { Name = d.Description, Vram = (long)d.DedicatedVideoMemory.ToUInt64() });
                            }
                        }
                        finally
                        {
                            Slot<ReleaseFn>(adapter, 2)(adapter);
                        }
                    }
                }
            }
            catch (Exception ex)
            {
                Log.Error("listing the video devices", ex);
            }
            finally
            {
                if (factory != IntPtr.Zero)
                {
                    Slot<ReleaseFn>(factory, 2)(factory);
                }
            }
            list = result;
            return result;
        }
    }
}
