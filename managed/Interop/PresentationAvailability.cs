using DSPAAMod.Core;

namespace DSPAAMod.Interop
{
    // A request for the next launch is not a claim of SDK/device support.
    internal static class PresentationAvailability
    {
        internal static bool CanRequest(NativePresentationStatus caps, PresentationStartupState startup,
            FrameGenerationBackend backend, bool runtimeFilesPresent)
        {
            if (backend == FrameGenerationBackend.Off) return true;
            if (caps.Quarantined) return false;
            if (caps.Available)
                return backend == FrameGenerationBackend.Fsr ? caps.FsrRuntimePresent :
                    backend == FrameGenerationBackend.Dlss && caps.DlssSupported;
            return startup == PresentationStartupState.Inactive && runtimeFilesPresent &&
                (backend == FrameGenerationBackend.Fsr || backend == FrameGenerationBackend.Dlss);
        }
        internal static bool NeedsRestart(NativePresentationStatus caps, PresentationStartupState startup,
            FrameGenerationSettings request) => !caps.Available && !caps.Quarantined &&
                startup == PresentationStartupState.Inactive && request.Backend != FrameGenerationBackend.Off;
    }
}
