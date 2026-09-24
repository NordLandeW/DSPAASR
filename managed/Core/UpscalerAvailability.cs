using System;

namespace DSPAAMod.Core
{
    public enum UpscalerBackend { Dlss, Fsr }
    public enum UpscalerSupportReason { Checking, Supported, GraphicsApi, Vendor, MotionVectors, ComputeShaders, Runtime }

    // Shared capability policy; FSR never inherits DLSS's NVIDIA requirement.
    public sealed class UpscalerAvailability
    {
        public UpscalerBackend Backend { get; }
        public UpscalerSupportReason Reason { get; }
        public string Detail { get; }
        public string Name => Backend == UpscalerBackend.Fsr ? "FSR" : "DLSS";
        public bool Available => Reason == UpscalerSupportReason.Supported;
        public bool Pending => Reason == UpscalerSupportReason.Checking;
        private UpscalerAvailability(UpscalerSupportReason reason, string detail = "", UpscalerBackend backend = UpscalerBackend.Dlss)
        { Reason = reason; Detail = detail ?? ""; Backend = backend; }
        public static UpscalerAvailability Checking { get; } = new UpscalerAvailability(UpscalerSupportReason.Checking);
        public static UpscalerAvailability Supported { get; } = new UpscalerAvailability(UpscalerSupportReason.Supported);
        public static UpscalerAvailability Failed(string detail) => new UpscalerAvailability(UpscalerSupportReason.Runtime, detail);
        public UpscalerAvailability Complete(bool available, string detail) =>
            new UpscalerAvailability(available ? UpscalerSupportReason.Supported : UpscalerSupportReason.Runtime, detail, Backend);
        public static UpscalerAvailability ForPlatform(bool direct3D11, int vendorId, bool motionVectors, bool computeShaders,
                                                       UpscalerBackend backend = UpscalerBackend.Dlss)
        {
            if (!Enum.IsDefined(typeof(UpscalerBackend), backend)) throw new ArgumentOutOfRangeException(nameof(backend));
            if (!direct3D11) return new UpscalerAvailability(UpscalerSupportReason.GraphicsApi, backend: backend);
            if (backend == UpscalerBackend.Dlss && vendorId != 0x10de) return new UpscalerAvailability(UpscalerSupportReason.Vendor, backend: backend);
            if (!motionVectors) return new UpscalerAvailability(UpscalerSupportReason.MotionVectors, backend: backend);
            if (!computeShaders) return new UpscalerAvailability(UpscalerSupportReason.ComputeShaders, backend: backend);
            return new UpscalerAvailability(UpscalerSupportReason.Checking, backend: backend);
        }
        public bool CanSelect(AaChoice choice) =>
            choice == AaChoice.Dlss ? Backend == UpscalerBackend.Dlss && Available :
            choice == AaChoice.Fsr ? Backend == UpscalerBackend.Fsr && Available : true;
        public string Describe(bool chinese)
        {
            if (Available) return "";
            if (Pending) return Name + (chinese ? "：正在检测显卡与驱动，请稍候。" : ": checking GPU and driver support. Please wait.");
            string reason;
            switch (Reason)
            {
                case UpscalerSupportReason.GraphicsApi:
                    reason = chinese ? "需要 Direct3D 11；请移除强制使用其他图形 API 的启动参数。" : "Direct3D 11 is required; remove launch options forcing another graphics API."; break;
                case UpscalerSupportReason.Vendor:
                    reason = chinese ? "当前渲染设备不是 NVIDIA 显卡；DLSS 需要受支持的 NVIDIA RTX 显卡。" : "The active rendering device is not NVIDIA. DLSS requires a supported NVIDIA RTX GPU."; break;
                case UpscalerSupportReason.MotionVectors:
                    reason = chinese ? "当前设备不支持所需的运动矢量。" : "The active device does not support the required motion vectors."; break;
                case UpscalerSupportReason.ComputeShaders:
                    reason = chinese ? "当前设备不支持所需的计算着色器。" : "The active device does not support the required compute shaders."; break;
                default:
                    reason = (chinese ? "运行库检测失败：" : "Runtime check failed: ") +
                        (string.IsNullOrWhiteSpace(Detail) ? (chinese ? "请检查显卡、驱动和模组运行库。" : "Check the GPU, driver and mod runtime files.") : Detail); break;
            }
            return Name + (chinese ? " 不可用：" : " unavailable: ") + reason;
        }
    }
}
