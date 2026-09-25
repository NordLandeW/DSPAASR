using System;

namespace DSPAAMod.Core
{
    public enum FrameGenerationBackend { Off, Fsr, Dlss }
    public enum FrameGenerationMode { Fixed, Dynamic }
    public enum ReflexMode { Off, On, OnWithBoost }
    public readonly struct FrameGenerationSettings : IEquatable<FrameGenerationSettings>
    {
        public readonly FrameGenerationBackend Backend;
        public readonly FrameGenerationMode Mode;
        // Additional frames. Display multiplier is GeneratedFrames + 1.
        public readonly uint GeneratedFrames;
        public readonly ReflexMode Reflex;
        public readonly float DynamicTargetFrameRate;
        public readonly uint FrameLimitMicroseconds;
        public FrameGenerationSettings(FrameGenerationBackend backend, FrameGenerationMode mode = FrameGenerationMode.Fixed,
            uint generatedFrames = 1, ReflexMode reflex = ReflexMode.On, float dynamicTargetFrameRate = 0, uint frameLimitMicroseconds = 0)
        {
            if (!Enum.IsDefined(typeof(FrameGenerationBackend), backend)) throw new ArgumentOutOfRangeException(nameof(backend));
            if (!Enum.IsDefined(typeof(FrameGenerationMode), mode)) throw new ArgumentOutOfRangeException(nameof(mode));
            if (!Enum.IsDefined(typeof(ReflexMode), reflex)) throw new ArgumentOutOfRangeException(nameof(reflex));
            if (generatedFrames == 0 || generatedFrames == uint.MaxValue) throw new ArgumentOutOfRangeException(nameof(generatedFrames));
            if (float.IsNaN(dynamicTargetFrameRate) || float.IsInfinity(dynamicTargetFrameRate) || dynamicTargetFrameRate < 0)
                throw new ArgumentOutOfRangeException(nameof(dynamicTargetFrameRate));
            if (backend == FrameGenerationBackend.Fsr && (mode != FrameGenerationMode.Fixed || generatedFrames != 1))
                throw new ArgumentException("Analytical FSR supports one additional frame, not DLSS Multi/Dynamic generation.");
            Backend = backend; Mode = mode; GeneratedFrames = generatedFrames; Reflex = reflex;
            DynamicTargetFrameRate = dynamicTargetFrameRate; FrameLimitMicroseconds = frameLimitMicroseconds;
        }
        public static FrameGenerationSettings Default => new FrameGenerationSettings(FrameGenerationBackend.Off);
        public bool Equals(FrameGenerationSettings other) => Backend == other.Backend && Mode == other.Mode &&
            GeneratedFrames == other.GeneratedFrames && Reflex == other.Reflex && DynamicTargetFrameRate == other.DynamicTargetFrameRate &&
            FrameLimitMicroseconds == other.FrameLimitMicroseconds;
        public override bool Equals(object value) => value is FrameGenerationSettings other && Equals(other);
        public override int GetHashCode() => (((((int)Backend * 397 ^ (int)Mode) * 397 ^ (int)GeneratedFrames) * 397 ^
            (int)Reflex) * 397 ^ DynamicTargetFrameRate.GetHashCode()) * 397 ^ (int)FrameLimitMicroseconds;
    }
    public sealed class FrameGenerationSettingsSession
    {
        public FrameGenerationSettings Applied { get; private set; }
        public FrameGenerationSettings Draft { get; set; }
        public FrameGenerationSettingsSession(FrameGenerationSettings value) { Applied = Draft = value; }
        public void Open() { Draft = Applied; }
        public void Cancel() { Draft = Applied; }
        public void Defaults() { Draft = FrameGenerationSettings.Default; }
        public void Apply() => Apply(null);
        public void Apply(Action<FrameGenerationSettings> persist)
        {
            var value = Draft;
            persist?.Invoke(value);
            Applied = value;
        }
    }
}
