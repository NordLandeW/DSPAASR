using System;
using System.Runtime.InteropServices;
using System.Text;

namespace DSPAAMod.Interop
{
    internal enum CaptureOperation : uint { Declare, Invalidate, Seed, Handoff, BeginScope, EndScope, Abort, Remember, Trace, DepthCopy, BeginTransfers, EndTransfers }
    internal enum CaptureScopeKind : uint { SharedPreparation, DualColor, FullOnlyUiCoverage, PartialWrite, ExternalBlurPublication }
    [StructLayout(LayoutKind.Sequential)]
    internal struct NativeCaptureTexture { public IntPtr Resource; public ulong Epoch; }
    [StructLayout(LayoutKind.Sequential)]
    internal struct NativeCaptureDomain
    {
        public float M00, M01, M10, M11, BiasX, BiasY, OffsetX, OffsetY;
        public uint RadiusX, RadiusY;
        public static NativeCaptureDomain Identity => new NativeCaptureDomain { M00 = 1, M11 = 1 };
    }
    [StructLayout(LayoutKind.Sequential)]
    internal struct NativeCaptureInput { public uint ResourceSlot, SamplerSlot, FirstDomain, DomainCount; }
    [StructLayout(LayoutKind.Sequential)]
    internal struct NativeCaptureCommand
    {
        public uint Size, Version;
        public CaptureOperation Operation;
        public CaptureScopeKind Scope;
        public ulong ApplicationFrameId, Generation, PassId;
        public uint Flags, InputCount, DomainCount;
        public int OcclusionSlot;
        public NativeCaptureTexture Source, Previous;
        public NativeCaptureDomain OcclusionDomain;
    }
    [StructLayout(LayoutKind.Sequential)]
    internal struct NativeCaptureStatus
    {
        public uint Size, Version, Flags, Reserved;
        public ulong ApplicationFrameId, Generation, CompletedUnityFrame;
        public ulong Draws, ColorReplays, CoverageReplays, SupportPasses, PrivateBytes;
        public ulong ProofInspected, ProofAccepted, CopiedConstantBytes;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 384)] public byte[] ReasonBytes;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 65)] public byte[] PixelShaderHashBytes;
        public bool Ready => (Flags & 1) != 0;
        public bool Quarantined => (Flags & 8) != 0;
        public string Reason => Text(ReasonBytes);
        public string PixelShaderHash => Text(PixelShaderHashBytes);
        private static string Text(byte[] bytes)
        {
            if (bytes == null) return string.Empty;
            int end = Array.IndexOf(bytes, (byte)0);
            return Encoding.UTF8.GetString(bytes, 0, end < 0 ? bytes.Length : end);
        }
    }
    [StructLayout(LayoutKind.Sequential)]
    internal struct NativeDepthCopyResult
    {
        public uint Size, Version, Flags, Reserved;
        public ulong ApplicationFrameId, Generation, RequestId;
        public bool Submitted(ulong frame, ulong generation, ulong request) => request != 0 && Version == 1 &&
            (Flags & 3u) == 3u && ApplicationFrameId == frame && Generation == generation && RequestId == request;
    }
    internal sealed class CaptureBridge
    {
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate IntPtr QueueNative(ref NativeCaptureCommand command,
            [In, MarshalAs(UnmanagedType.LPArray)] NativeCaptureInput[] inputs,
            [In, MarshalAs(UnmanagedType.LPArray)] NativeCaptureDomain[] domains,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string basis);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void CancelNative(IntPtr token);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int StatusNative(ref NativeCaptureStatus value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int DepthCopyNative(ref NativeDepthCopyResult value);
        private readonly QueueNative queue;
        private readonly CancelNative cancel;
        private readonly StatusNative status;
        private readonly DepthCopyNative depthCopy;
        public IntPtr RenderEvent { get; }
        public static uint CommandSize => (uint)Marshal.SizeOf(typeof(NativeCaptureCommand));
        public static uint StatusSize => (uint)Marshal.SizeOf(typeof(NativeCaptureStatus));
        public CaptureBridge(NativeBridge library, IntPtr callback)
        {
            if (CommandSize != 128 || StatusSize != 560 || Marshal.SizeOf(typeof(NativeCaptureDomain)) != 40 ||
                Marshal.SizeOf(typeof(NativeCaptureInput)) != 16 || Marshal.SizeOf(typeof(NativeCaptureTexture)) != 16 ||
                Marshal.SizeOf(typeof(NativeDepthCopyResult)) != 40)
                throw new InvalidOperationException("Native/managed capture ABI mismatch.");
            queue = library.Get<QueueNative>("DspAaQueueCaptureCommand");
            cancel = library.Get<CancelNative>("DspAaCancelCaptureCommand");
            status = library.Get<StatusNative>("DspAaGetCaptureStatus");
            depthCopy = library.Get<DepthCopyNative>("DspAaGetDepthCopyResult");
            RenderEvent = callback;
            if (RenderEvent == IntPtr.Zero) throw new InvalidOperationException("Capture requires the ordered presentation render callback.");
        }
        // The caller owns the command-buffer submission transaction. Return its
        // opaque handle so an abandoned buffer can cancel queued, unconsumed work.
        public IntPtr Queue(NativeCaptureCommand command, NativeCaptureInput[] inputs = null,
            NativeCaptureDomain[] domains = null, string basis = "")
        {
            command.Size = CommandSize; command.Version = 1;
            command.InputCount = (uint)(inputs?.Length ?? 0); command.DomainCount = (uint)(domains?.Length ?? 0);
            return queue(ref command, inputs, domains, basis);
        }
        public void Cancel(IntPtr token) { if (token != IntPtr.Zero) cancel(token); }
        public bool TryGetStatus(out NativeCaptureStatus value)
        {
            value = new NativeCaptureStatus { Size = StatusSize, ReasonBytes = new byte[384], PixelShaderHashBytes = new byte[65] };
            return status(ref value) == 1;
        }
        public bool TryGetDepthCopyResult(out NativeDepthCopyResult value)
        {
            value = new NativeDepthCopyResult { Size = (uint)Marshal.SizeOf(typeof(NativeDepthCopyResult)) };
            return depthCopy(ref value) == 1;
        }
    }
}
