using System;

namespace DSPAAMod.Core
{
    // Dimensions come from NGX, never a duplicated table of approximate ratios.
    public readonly struct RenderResolution
    {
        public readonly int InputWidth, InputHeight, OutputWidth, OutputHeight;
        public bool IsNative => InputWidth == OutputWidth && InputHeight == OutputHeight;
        public RenderResolution(int inputWidth, int inputHeight, int outputWidth, int outputHeight)
        {
            if (inputWidth <= 0 || inputHeight <= 0 || outputWidth <= 0 || outputHeight <= 0)
                throw new ArgumentOutOfRangeException(nameof(inputWidth), "Render dimensions must be positive.");
            InputWidth = inputWidth; InputHeight = inputHeight;
            OutputWidth = outputWidth; OutputHeight = outputHeight;
        }
        // Preserve the verified native 32-phase sequence. At lower render resolutions
        // cover at least eight samples per output-pixel footprint (NGX guide 3.7.1.1).
        public uint JitterPhases => checked((uint)Math.Max(Jitter.PhaseCount,
            Math.Ceiling(8.0 * OutputWidth / InputWidth * OutputHeight / InputHeight)));
    }
}
