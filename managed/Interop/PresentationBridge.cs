using System;
using System.Runtime.InteropServices;
using System.Text;

namespace DSPAAMod.Interop
{
    [StructLayout(LayoutKind.Sequential)]
    public struct NativePresentationConfiguration
    {
        public uint Size, Version, Backend, Mode, GeneratedFrames, Reflex;
        public float DynamicTargetFrameRate;
        public uint FrameLimitMicroseconds;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct NativePresentationBegin
    {
        public uint Size, Version;
        public ulong ApplicationFrameId, Generation;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct NativePresentationInputs
    {
        public uint Size, Version;
        public ulong ApplicationFrameId, Generation;
        public IntPtr Hudless, Depth, Motion, OcclusionAlpha, UiInfluence, FsrDistortion, SlDistortion;
        public uint RenderWidth, RenderHeight, Flags, Reserved;
        public float JitterX, JitterY, MotionScaleX, MotionScaleY;
        public float Milliseconds, CameraNear, CameraFar, VerticalFov;
        public float PreExposure, ViewSpaceToMeters, MinLuminance, MaxLuminance;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 3)] public float[] CameraPosition;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 3)] public float[] CameraUp;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 3)] public float[] CameraRight;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 3)] public float[] CameraForward;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 16)] public float[] CameraViewToClip;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 16)] public float[] ClipToCameraView;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 16)] public float[] ClipToPrevClip;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 16)] public float[] PrevClipToClip;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 4)] public int[] GenerationRect;
        public static NativePresentationInputs Create() => new NativePresentationInputs {
            CameraPosition = new float[3], CameraUp = new float[3], CameraRight = new float[3], CameraForward = new float[3],
            CameraViewToClip = new float[16], ClipToCameraView = new float[16], ClipToPrevClip = new float[16], PrevClipToClip = new float[16],
            GenerationRect = new int[4], PreExposure = 1, ViewSpaceToMeters = 1, MaxLuminance = 100
        };
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct NativePresentationStatus
    {
        public uint Size, Version, Flags, RequestedBackend, ActiveBackend, ActiveMode;
        public uint MaximumGeneratedFrames, Width, Height, SdkStatus;
        public ulong Generation, LastApplicationFrameId, LastPresentedFrameId, ApplicationPresents, GeneratedSubmissions, SdkReportedPresents;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 384)] public byte[] MessageBytes;
        public bool Available => (Flags & 1) != 0;
        public bool FsrRuntimePresent => (Flags & 2) != 0;
        public bool DlssSupported => (Flags & 4) != 0;
        public bool ReflexSupported => (Flags & 8) != 0;
        public bool DynamicSupported => (Flags & 16) != 0;
        public bool Generating => (Flags & 64) != 0;
        public bool Quarantined => (Flags & 128) != 0;
        public string Message {
            get {
                if (MessageBytes == null) return string.Empty;
                int end = Array.IndexOf(MessageBytes, (byte)0);
                return Encoding.UTF8.GetString(MessageBytes, 0, end < 0 ? MessageBytes.Length : end);
            }
        }
    }
    // Presentation has its own additive ABI/lifetime; SR's Initialize/Shutdown
    // must neither create a late swap-chain hook nor tear down a live presenter.
    public sealed class PresentationBridge
    {
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate uint Abi();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int ConfigureNative(ref NativePresentationConfiguration value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int BeginNative(ref NativePresentationBegin value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int EndSimulationNative(ulong id);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate IntPtr QueueNative(ref NativePresentationInputs value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void CancelNative(IntPtr token);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int StatusNative(ref NativePresentationStatus value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate IntPtr EventNative();
        private readonly ConfigureNative configure;
        private readonly BeginNative begin;
        private readonly EndSimulationNative simulationEnd;
        private readonly QueueNative queue;
        private readonly CancelNative cancel;
        private readonly StatusNative status;
        public IntPtr RenderEvent { get; }
        public static uint ConfigurationSize => (uint)Marshal.SizeOf(typeof(NativePresentationConfiguration));
        public static uint BeginSize => (uint)Marshal.SizeOf(typeof(NativePresentationBegin));
        public static uint InputsSize => (uint)Marshal.SizeOf(typeof(NativePresentationInputs));
        public static uint StatusSize => (uint)Marshal.SizeOf(typeof(NativePresentationStatus));
        public PresentationBridge(NativeBridge library)
        {
            if (library.Get<Abi>("DspAaGetPresentationAbiVersion")() != 1 || ConfigurationSize != 32 || BeginSize != 24 || InputsSize != 464 || StatusSize != 472)
                throw new InvalidOperationException("Native/managed presentation ABI mismatch.");
            configure = library.Get<ConfigureNative>("DspAaConfigurePresentation");
            begin = library.Get<BeginNative>("DspAaBeginPresentationFrame");
            simulationEnd = library.Get<EndSimulationNative>("DspAaPresentationSimulationEnd");
            queue = library.Get<QueueNative>("DspAaQueuePresentationInputs");
            cancel = library.Get<CancelNative>("DspAaCancelPresentationInputs");
            status = library.Get<StatusNative>("DspAaGetPresentationStatus");
            RenderEvent = library.Get<EventNative>("DspAaGetPresentationRenderEvent")();
            if (RenderEvent == IntPtr.Zero) throw new InvalidOperationException("Missing presentation render event.");
        }
        public bool Configure(NativePresentationConfiguration value)
        { value.Size = ConfigurationSize; value.Version = 1; return configure(ref value) == 1; }
        public bool Begin(out NativePresentationBegin value)
        { value = new NativePresentationBegin { Size = BeginSize, Version = 1 }; return begin(ref value) == 1; }
        public bool SimulationEnd(ulong id) => simulationEnd(id) == 1;
        public IntPtr Queue(ref NativePresentationInputs value)
        { value.Size = InputsSize; value.Version = 1; return queue(ref value); }
        public void Cancel(IntPtr token) { if (token != IntPtr.Zero) cancel(token); }
        public bool TryGetStatus(out NativePresentationStatus value)
        { value = new NativePresentationStatus { Size = StatusSize, MessageBytes = new byte[384] }; return status(ref value) == 1; }
    }
}
