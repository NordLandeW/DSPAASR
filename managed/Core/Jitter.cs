namespace DSPAAMod.Core
{
    public readonly struct JitterSample
    {
        public readonly float X;
        public readonly float Y;
        public JitterSample(float x, float y) { X = x; Y = y; }
    }

    public static class Jitter
    {
        public const uint PhaseCount = 32;
        public static JitterSample ForFrame(uint frame, uint phases = PhaseCount)
        {
            if (phases == 0) throw new System.ArgumentOutOfRangeException(nameof(phases));
            uint index = frame % phases + 1;
            return new JitterSample(Halton(index, 2) - 0.5f, Halton(index, 3) - 0.5f);
        }
        // Unity's shifted frustum moves geometry by the opposite pixel offset in
        // a D3D11 render texture. NGX consumes that image displacement, not the
        // sampling/frustum offset. Keep the camera and NGX coordinate spaces distinct.
        public static JitterSample ToNgx(JitterSample projection) => new JitterSample(-projection.X, -projection.Y);
        private static float Halton(uint index, uint radix)
        {
            float result = 0f;
            float fraction = 1f;
            while (index != 0)
            {
                fraction /= radix;
                result += fraction * (index % radix);
                index /= radix;
            }
            return result;
        }
    }
}
