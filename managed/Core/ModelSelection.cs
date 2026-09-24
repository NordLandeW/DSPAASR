using System;

namespace DSPAAMod.Core
{
    public enum ModelSelection
    {
        Recommended,
        Cnn,
        TransformerK,
        TransformerL,
        TransformerM
    }

    public enum ResolutionMode
    {
        Dlaa,
        Quality,
        Balanced,
        Performance,
        UltraPerformance
    }

    public static class ModelPolicy
    {
        // NGX preset values are the native SDK wire values, not a ranking of quality.
        public static uint ResolvePreset(ModelSelection selection, ResolutionMode mode)
        {
            if (!Enum.IsDefined(typeof(ResolutionMode), mode))
                throw new ArgumentOutOfRangeException(nameof(mode));
            switch (selection)
            {
                case ModelSelection.Recommended:
                    if (mode == ResolutionMode.Performance) return 13; // M
                    if (mode == ResolutionMode.UltraPerformance) return 12; // L
                    return 11; // K: DLAA, Quality, Balanced
                case ModelSelection.Cnn:
                    // Legacy CNN defaults: F for native AA / very low input resolution;
                    // E for the other SR ratios. Both remain explicit, verified CNN presets.
                    return mode == ResolutionMode.Dlaa || mode == ResolutionMode.UltraPerformance ? 6u : 5u;
                case ModelSelection.TransformerK: return 11;
                case ModelSelection.TransformerL: return 12;
                case ModelSelection.TransformerM: return 13;
                default: throw new ArgumentOutOfRangeException(nameof(selection));
            }
        }
    }
}
