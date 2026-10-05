using System;
using UnityEngine;

namespace DSPAAMod.Game
{
    // The game's blueprint camera owns an off-center projection. Keep that
    // projection distinct from the temporary raster jitter and Unity's default.
    internal sealed class TemporalProjection
    {
        public bool Captured { get; private set; }
        public bool PreserveOnReset { get; private set; }
        public Matrix4x4 Original { get; private set; }
        public Matrix4x4 Applied { get; private set; }
        public readonly Func<Vector2, Matrix4x4> JitteredMatrix;
        private int width, height;
        private bool orthographic, appliedLive;

        public TemporalProjection() { JitteredMatrix = Jitter; }

        public void Begin(Matrix4x4 projection, bool isOrthographic, int renderWidth, int renderHeight, bool preserveOnReset)
        {
            if (renderWidth <= 0 || renderHeight <= 0) throw new ArgumentOutOfRangeException(nameof(renderWidth));
            Original = Applied = projection;
            orthographic = isOrthographic;
            width = renderWidth; height = renderHeight;
            PreserveOnReset = preserveOnReset;
            Captured = true;
            appliedLive = true;
        }

        private Matrix4x4 Jitter(Vector2 offset)
        {
            Matrix4x4 projection = Original;
            if (orthographic)
            {
                projection.m03 -= 2f * offset.x / width;
                projection.m13 -= 2f * offset.y / height;
            }
            else
            {
                projection.m02 += 2f * offset.x / width;
                projection.m12 += 2f * offset.y / height;
            }
            Observe(projection);
            return projection;
        }

        public void Observe(Matrix4x4 projection)
        {
            if (!Captured) return;
            Applied = projection;
            appliedLive = true;
        }

        public bool TryReset(Matrix4x4 current, out Matrix4x4 restored)
        {
            restored = current;
            if (!Captured) return false;
            if (!PreserveOnReset)
            {
                // The original reset now owns the camera, even if its default
                // matrix happens to equal the custom delegate's last output.
                appliedLive = false;
                return false;
            }
            TryRestore(current, out restored);
            // Even after restoration, consume the stack's default reset. Keep
            // this snapshot until the next render or explicit scope teardown.
            return true;
        }

        public bool TryRestore(Matrix4x4 current, out Matrix4x4 restored)
        {
            restored = current;
            // Exact equality is deliberate: another component's projection,
            // including a small subpixel change, is no longer ours to restore.
            if (!Captured || !appliedLive) return false;
            appliedLive = false;
            if (!current.Equals(Applied)) return false;
            restored = Original;
            return true;
        }

        public void Clear()
        {
            Captured = false;
            PreserveOnReset = false;
            appliedLive = false;
        }
    }
}
