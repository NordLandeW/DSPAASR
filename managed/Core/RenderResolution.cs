using System;

namespace DSPAAMod.Core
{
    // Dimensions and FSR phases come from the selected SDK, not a second ratio table.
    public readonly struct RenderResolution
    {
        public readonly int InputWidth, InputHeight, OutputWidth, OutputHeight;
        public bool IsNative => InputWidth == OutputWidth && InputHeight == OutputHeight;
        private readonly uint explicitPhases;
        public RenderResolution(int inputWidth, int inputHeight, int outputWidth, int outputHeight, uint jitterPhases = 0)
        {
            if (inputWidth <= 0 || inputHeight <= 0 || outputWidth <= 0 || outputHeight <= 0)
                throw new ArgumentOutOfRangeException(nameof(inputWidth), "Render dimensions must be positive.");
            InputWidth = inputWidth; InputHeight = inputHeight;
            OutputWidth = outputWidth; OutputHeight = outputHeight;
            explicitPhases = jitterPhases;
        }
        // Preserve the verified native 32-phase sequence. At lower render resolutions
        // cover at least eight samples per output-pixel footprint (NGX guide 3.7.1.1).
        public uint JitterPhases => explicitPhases != 0 ? explicitPhases : checked((uint)Math.Max(Jitter.PhaseCount,
            Math.Ceiling(8.0 * OutputWidth / InputWidth * OutputHeight / InputHeight)));
    }
}
