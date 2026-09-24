using System;

namespace DSPAAMod.Core
{
    // Dlaa retains the old serialized value for migration; new selections use Dlss.
    public enum AaTechnique { Original, Fxaa, Taa, Dlaa, Dlss, Fsr }

    public readonly struct AaSettings : IEquatable<AaSettings>
    {
        public readonly AaTechnique Technique;
        public readonly ModelSelection Model;
        public readonly ResolutionMode Resolution;
        public readonly float FsrSharpness;
        public bool Temporal => Technique == AaTechnique.Dlss || Technique == AaTechnique.Fsr;
        public AaSettings(AaTechnique technique, ModelSelection model, ResolutionMode resolution = ResolutionMode.Dlaa, float fsrSharpness = 0f)
        {
            if (!Enum.IsDefined(typeof(AaTechnique), technique)) throw new ArgumentOutOfRangeException(nameof(technique));
            if (!Enum.IsDefined(typeof(ModelSelection), model)) throw new ArgumentOutOfRangeException(nameof(model));
            if (!Enum.IsDefined(typeof(ResolutionMode), resolution)) throw new ArgumentOutOfRangeException(nameof(resolution));
            if (float.IsNaN(fsrSharpness) || float.IsInfinity(fsrSharpness) || fsrSharpness < 0f || fsrSharpness > 1f)
                throw new ArgumentOutOfRangeException(nameof(fsrSharpness));
            FsrSharpness = fsrSharpness;
            // A legacy Dlaa config always meant native resolution, even if an unrelated
            // new resolution key was left behind by a later installation.
            Technique = technique == AaTechnique.Dlaa ? AaTechnique.Dlss : technique;
            Model = model;
            Resolution = technique == AaTechnique.Dlaa ? ResolutionMode.Dlaa : resolution;
        }
        // Installation never silently changes the game's current AA. Model selection
        // defaults to NVIDIA's recommended mapping once the user enables DLSS.
        public static AaSettings Default => new AaSettings(AaTechnique.Original, ModelSelection.Recommended);
        public bool Equals(AaSettings other) => Technique == other.Technique && Model == other.Model && Resolution == other.Resolution && FsrSharpness == other.FsrSharpness;
        public override bool Equals(object other) => other is AaSettings settings && Equals(settings);
        public override int GetHashCode() => ((((int)Technique * 397) ^ (int)Model) * 397 ^ (int)Resolution) * 397 ^ FsrSharpness.GetHashCode();
    }

    public sealed class SettingsSession
    {
        public AaSettings Applied { get; private set; }
        public AaSettings Draft { get; set; }
        public SettingsSession(AaSettings initial) { Applied = Draft = initial; }
        public void Open() { Draft = Applied; }
        public void Cancel() { Draft = Applied; }
        public void Defaults() { Draft = AaSettings.Default; }
        public void Apply() { Applied = Draft; }
    }
}
