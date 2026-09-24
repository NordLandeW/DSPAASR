using System;

namespace DSPAAMod.Core
{
    public enum DlssSupportReason { Checking, Supported, GraphicsApi, Vendor, MotionVectors, ComputeShaders, Runtime }

    // Capability, not a GPU-name whitelist or a model/performance recommendation.
    public sealed class DlssAvailability
    {
        public DlssSupportReason Reason { get; }
        public string Detail { get; }
        public bool Available => Reason == DlssSupportReason.Supported;
        public bool Pending => Reason == DlssSupportReason.Checking;
        private DlssAvailability(DlssSupportReason reason, string detail = "") { Reason = reason; Detail = detail ?? ""; }
        public static DlssAvailability Checking { get; } = new DlssAvailability(DlssSupportReason.Checking);
        public static DlssAvailability Supported { get; } = new DlssAvailability(DlssSupportReason.Supported);
        public static DlssAvailability Failed(string detail) => new DlssAvailability(DlssSupportReason.Runtime, detail);
        public static DlssAvailability ForPlatform(bool direct3D11, int vendorId, bool motionVectors, bool computeShaders)
        {
            if (!direct3D11) return new DlssAvailability(DlssSupportReason.GraphicsApi);
            if (vendorId != 0x10de) return new DlssAvailability(DlssSupportReason.Vendor);
            if (!motionVectors) return new DlssAvailability(DlssSupportReason.MotionVectors);
            if (!computeShaders) return new DlssAvailability(DlssSupportReason.ComputeShaders);
            return Checking; // NVIDIA alone does not prove that DLSS is available.
        }
        public bool CanSelect(AaChoice choice) => choice != AaChoice.Dlss || Available;
        public string Describe(bool chinese)
        {
            if (Available) return "";
            if (Pending) return chinese ? "DLSS：正在检测显卡与驱动，请稍候。" : "DLSS: checking GPU and driver support. Please wait.";
            string reason;
            switch (Reason)
            {
                case DlssSupportReason.GraphicsApi:
                    reason = chinese ? "需要 Direct3D 11；请移除强制使用其他图形 API 的启动参数。" : "Direct3D 11 is required; remove launch options forcing another graphics API."; break;
                case DlssSupportReason.Vendor:
                    reason = chinese ? "当前渲染设备不是 NVIDIA 显卡；DLSS 需要受支持的 NVIDIA RTX 显卡。" : "The active rendering device is not NVIDIA. DLSS requires a supported NVIDIA RTX GPU."; break;
                case DlssSupportReason.MotionVectors:
                    reason = chinese ? "当前设备不支持所需的运动矢量。" : "The active device does not support the required motion vectors."; break;
                case DlssSupportReason.ComputeShaders:
                    reason = chinese ? "当前设备不支持所需的计算着色器。" : "The active device does not support the required compute shaders."; break;
                default:
                    reason = (chinese ? "运行库检测失败：" : "Runtime check failed: ") +
                        (string.IsNullOrWhiteSpace(Detail) ? (chinese ? "请检查显卡、驱动和模组运行库。" : "Check the GPU, driver and mod runtime files.") : Detail); break;
            }
            return (chinese ? "DLSS 不可用：" : "DLSS unavailable: ") + reason;
        }
    }
}
