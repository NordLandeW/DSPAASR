using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

namespace DSPAAMod.Interop
{
    [StructLayout(LayoutKind.Sequential)]
    public struct NativeFrame
    {
        public uint Size, Version;
        public ulong Camera, Frame;
        public IntPtr Color, Output, Depth, Motion;
        public uint Width, Height, Preset, Flags;
        public float JitterX, JitterY, MotionScaleX, MotionScaleY, FrameTimeMilliseconds;
        public uint Reserved;
        public uint OutputWidth, OutputHeight, Quality, Reserved2;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct NativeFsrParameters
    {
        public uint Size;
        public float CameraNear, CameraFar, VerticalFov, PreExposure, ViewSpaceToMeters, Sharpness;
        public uint Reserved;
        public IntPtr OpaqueColor;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct NativeFsrOptimalSettings
    {
        public NativeOptimalSettings Settings;
        public uint JitterPhases;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct NativeStatus
    {
        public uint Size;
        public int Result;
        public ulong Frame;
        public uint RequestedPreset, ObservedPreset, Verification, Reserved;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 256)] public byte[] MessageBytes;
        public string Message
        {
            get
            {
                if (MessageBytes == null) return string.Empty;
                int length = Array.IndexOf(MessageBytes, (byte)0);
                return Encoding.UTF8.GetString(MessageBytes, 0, length < 0 ? MessageBytes.Length : length);
            }
        }
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct NativeOptimalSettings
    {
        public uint Size;
        public int Result;
        public uint OutputWidth, OutputHeight, Quality, OptimalWidth, OptimalHeight, MinWidth, MinHeight, MaxWidth, MaxHeight;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 256)] public byte[] MessageBytes;
        public string Message
        {
            get
            {
                if (MessageBytes == null) return string.Empty;
                int length = Array.IndexOf(MessageBytes, (byte)0);
                return Encoding.UTF8.GetString(MessageBytes, 0, length < 0 ? MessageBytes.Length : length);
            }
        }
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct NativeSupport
    {
        public uint Size;
        public int Result;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 256)] public byte[] MessageBytes;
        public string Message
        {
            get
            {
                if (MessageBytes == null) return string.Empty;
                int length = Array.IndexOf(MessageBytes, (byte)0);
                return Encoding.UTF8.GetString(MessageBytes, 0, length < 0 ? MessageBytes.Length : length);
            }
        }
    }



    public sealed class NativeBridge
    {
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate uint Abi();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        private delegate int Initialize(string runtime, string data);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate IntPtr GetEvent();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate IntPtr QueueFrame(ref NativeFrame frame);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate IntPtr QueueRelease(ulong camera);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate IntPtr QueueShutdown();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void CancelCommand(IntPtr token);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int ReadStatus(ulong camera, ref NativeStatus status);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate IntPtr QueueOptimal(ulong camera, IntPtr deviceResource, uint width, uint height, uint quality);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int ReadOptimal(ulong camera, ref NativeOptimalSettings settings);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate IntPtr QueueSupport(IntPtr deviceResource);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int ReadSupport(IntPtr token, ref NativeSupport support);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate IntPtr QueueFsrFrame(ref NativeFrame frame, ref NativeFsrParameters parameters);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate IntPtr QueueBackendSupport(IntPtr resource, uint backend);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate IntPtr QueueBackendOptimal(ulong camera, IntPtr resource, uint width, uint height, uint quality, uint backend);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int ReadFsrOptimal(ulong camera, ref NativeFsrOptimalSettings result);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr LoadLibraryEx(string name, IntPtr file, uint flags);
        [DllImport("kernel32.dll", CharSet = CharSet.Ansi, ExactSpelling = true, SetLastError = true)]
        private static extern IntPtr GetProcAddress(IntPtr module, string name);

        private readonly IntPtr module;
        private readonly QueueFrame queueFrame;
        private readonly QueueRelease queueRelease;
        private readonly QueueShutdown queueShutdown;
        private readonly CancelCommand cancel;
        private readonly ReadStatus readStatus;
        private readonly QueueOptimal queueOptimal;
        private readonly ReadOptimal readOptimal;
        private readonly QueueSupport queueSupport;
        private readonly ReadSupport readSupport;
        private readonly QueueFsrFrame queueFsrFrame;
        private readonly QueueBackendSupport queueBackendSupport;
        private readonly QueueBackendOptimal queueBackendOptimal;
        private readonly ReadFsrOptimal readFsrOptimal;
        public IntPtr RenderEvent { get; }
        public static uint FrameSize => (uint)Marshal.SizeOf(typeof(NativeFrame));
        public static uint StatusSize => (uint)Marshal.SizeOf(typeof(NativeStatus));
        public static uint OptimalSize => (uint)Marshal.SizeOf(typeof(NativeOptimalSettings));
        public static uint SupportSize => (uint)Marshal.SizeOf(typeof(NativeSupport));
        public static uint FsrParametersSize => (uint)Marshal.SizeOf(typeof(NativeFsrParameters));
        public static uint FsrOptimalSize => (uint)Marshal.SizeOf(typeof(NativeFsrOptimalSettings));

        public NativeBridge(string directory, string dataDirectory)
        {
            if (!Environment.Is64BitProcess) throw new PlatformNotSupportedException("DSPAAMod requires x64.");
            module = LoadLibraryEx(Path.Combine(directory, "DSPAANative.dll"), IntPtr.Zero, 0x00000100 | 0x00001000);
            if (module == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot load DSPAANative.dll.");
            // Keep the DLL loaded for process lifetime: Unity may hold queued function pointers.
            if (Get<Abi>("DspAaGetAbiVersion")() != 2 || FrameSize != 112 || StatusSize != 288 || OptimalSize != 300 || SupportSize != 264 || FsrParametersSize != 40 || FsrOptimalSize != 304)
                throw new InvalidOperationException("Native/managed DSPAAMod ABI mismatch.");
            queueFrame = Get<QueueFrame>("DspAaQueueFrame");
            queueRelease = Get<QueueRelease>("DspAaQueueRelease");
            queueShutdown = Get<QueueShutdown>("DspAaQueueShutdown");
            cancel = Get<CancelCommand>("DspAaCancel");
            readStatus = Get<ReadStatus>("DspAaGetStatus");
            queueOptimal = Get<QueueOptimal>("DspAaQueueOptimalSettings");
            readOptimal = Get<ReadOptimal>("DspAaGetOptimalSettings");
            queueSupport = Get<QueueSupport>("DspAaQueueSupport");
            readSupport = Get<ReadSupport>("DspAaGetSupport");
            queueFsrFrame = Get<QueueFsrFrame>("DspAaQueueFsrFrame");
            queueBackendSupport = Get<QueueBackendSupport>("DspAaQueueSupportForBackend");
            queueBackendOptimal = Get<QueueBackendOptimal>("DspAaQueueOptimalSettingsForBackend");
            readFsrOptimal = Get<ReadFsrOptimal>("DspAaGetFsrOptimalSettings");
            RenderEvent = Get<GetEvent>("DspAaGetRenderEvent")();
            if (RenderEvent == IntPtr.Zero || Get<Initialize>("DspAaInitialize")(directory, dataDirectory) != 1)
                throw new InvalidOperationException("Native bridge initialization failed; check runtime path and log permissions.");
        }
        private T Get<T>(string name) where T : Delegate
        {
            IntPtr address = GetProcAddress(module, name);
            if (address == IntPtr.Zero) throw new EntryPointNotFoundException(name);
            return (T)Marshal.GetDelegateForFunctionPointer(address, typeof(T));
        }
        public IntPtr Submit(ref NativeFrame frame)
        {
            frame.Size = FrameSize;
            frame.Version = 2;
            return queueFrame(ref frame);
        }
        public IntPtr SubmitFsr(ref NativeFrame frame, ref NativeFsrParameters parameters)
        {
            frame.Size = FrameSize; frame.Version = 2; parameters.Size = FsrParametersSize;
            return queueFsrFrame(ref frame, ref parameters);
        }
        public IntPtr RequestOptimal(ulong camera, IntPtr resource, uint width, uint height, uint quality, uint backend) =>
            queueBackendOptimal(camera, resource, width, height, quality, backend);
        public bool TryGetFsrOptimal(ulong camera, out NativeFsrOptimalSettings result)
        {
            result = new NativeFsrOptimalSettings { Settings = new NativeOptimalSettings { Size = FsrOptimalSize, MessageBytes = new byte[256] } };
            return readFsrOptimal(camera, ref result) == 1;
        }
        public IntPtr RequestSupport(IntPtr resource, uint backend) => queueBackendSupport(resource, backend);
        public IntPtr RequestOptimal(ulong camera, IntPtr deviceResource, uint width, uint height, uint quality) =>
            queueOptimal(camera, deviceResource, width, height, quality);
        public bool TryGetOptimal(ulong camera, out NativeOptimalSettings result)
        {
            result = new NativeOptimalSettings { Size = OptimalSize, MessageBytes = new byte[256] };
            return readOptimal(camera, ref result) == 1;
        }
        public IntPtr RequestSupport(IntPtr deviceResource) => queueSupport(deviceResource);
        public bool TryGetSupport(IntPtr token, out NativeSupport result)
        {
            result = new NativeSupport { Size = SupportSize, MessageBytes = new byte[256] };
            return readSupport(token, ref result) == 1;
        }
        public IntPtr Release(ulong camera) => queueRelease(camera);
        public IntPtr Shutdown() => queueShutdown();
        public void Cancel(IntPtr token) { if (token != IntPtr.Zero) cancel(token); }
        public bool TryGetStatus(ulong camera, out NativeStatus status)
        {
            status = new NativeStatus { Size = StatusSize, MessageBytes = new byte[256] };
            return readStatus(camera, ref status) == 1;
        }
    }
}
