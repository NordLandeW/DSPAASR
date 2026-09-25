using System.Collections.Generic;

namespace DSPAAMod.Core
{
    // Menu projections contain selectable requests, not the saved configuration.
    // A saved value absent from a projection must keep its own unselected caption.
    internal static class GraphicsMenuChoices
    {
        internal static AaChoice[] Antialiasing(bool dlssAvailable, bool fsrAvailable)
        {
            var choices = new List<AaChoice> { AaChoice.None, AaChoice.Msaa, AaChoice.Fxaa, AaChoice.Taa };
            if (dlssAvailable) choices.Add(AaChoice.Dlss);
            if (fsrAvailable) choices.Add(AaChoice.Fsr);
            return choices.ToArray();
        }
        internal static FrameGenerationBackend[] Backends(bool canRequestFsr, bool canRequestDlss)
        {
            var choices = new List<FrameGenerationBackend> { FrameGenerationBackend.Off };
            if (canRequestFsr) choices.Add(FrameGenerationBackend.Fsr);
            if (canRequestDlss) choices.Add(FrameGenerationBackend.Dlss);
            return choices.ToArray();
        }
        internal static ReflexMode[] ReflexModes() => new[] { ReflexMode.On, ReflexMode.OnWithBoost };
        internal static FrameMultiplierChoice[] Multipliers(bool canRequestDlss, bool dlssDetailsKnown,
            uint maximumGeneratedFrames, bool dynamicSupported)
        {
            var choices = new List<FrameMultiplierChoice>();
            if (!canRequestDlss) return choices.ToArray();
            // Before DLSS activation, 2x is only a baseline request. Another
            // backend's maximum/dynamic fields are not DLSS capability evidence.
            uint maximum = dlssDetailsKnown ? maximumGeneratedFrames : 1u;
            for (uint generated = 1; generated <= maximum && generated < uint.MaxValue; ++generated)
                choices.Add(new FrameMultiplierChoice(FrameGenerationMode.Fixed, generated));
            if (dlssDetailsKnown && dynamicSupported)
                choices.Add(new FrameMultiplierChoice(FrameGenerationMode.Dynamic, System.Math.Max(maximum, 1u)));
            return choices.ToArray();
        }
        internal static int FindMultiplier(FrameMultiplierChoice[] choices, FrameGenerationSettings value)
        {
            for (int i = 0; i < choices.Length; ++i)
                if (choices[i].Mode == value.Mode &&
                    (value.Mode == FrameGenerationMode.Dynamic || choices[i].GeneratedFrames == value.GeneratedFrames)) return i;
            return -1;
        }
    }
    internal readonly struct FrameMultiplierChoice
    {
        internal readonly FrameGenerationMode Mode;
        internal readonly uint GeneratedFrames;
        internal FrameMultiplierChoice(FrameGenerationMode mode, uint generatedFrames)
        { Mode = mode; GeneratedFrames = generatedFrames; }
    }
}
