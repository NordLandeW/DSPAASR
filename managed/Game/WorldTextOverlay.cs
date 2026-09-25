using System.Collections.Generic;
using DSPAAMod.Core;
using UnityEngine;
using UnityEngine.Rendering;

namespace DSPAAMod.Game
{
    // This navigation-only world HUD is not ordinary opaque scene geometry.
    // Keep the game's TextMesh or sail Canvas, materials and transforms; only
    // move their draw to native-size temporal resolve, or after H when using FG.
    internal sealed class WorldTextOverlay : System.IDisposable
    {
        private struct Entry
        {
            public Renderer Renderer;
            public Material Material;
        }
        private readonly RenderVisibilityScope<Renderer> visibility = new RenderVisibilityScope<Renderer>(
            renderer => renderer, renderer => renderer.forceRenderingOff, (renderer, value) => renderer.forceRenderingOff = value);
        private readonly List<Entry> entries = new List<Entry>();
        private UISailIndicator indicator;
        private GameObject group;
        private TextMesh[] texts;
        private Camera camera;
        private int begunFrame = -1;
        private string unavailable;
        private readonly SailCanvasOverlay canvas = new SailCanvasOverlay();
        private Matrix4x4 view, projection;
        private Rect viewport;
        public int PendingCount => entries.Count + canvas.PendingCount;
        public void SetProjection(Matrix4x4 value) => projection = value;
        public string Begin(Camera current, Matrix4x4 nonJitteredProjection, bool beforeCanvas = false, NavigationBloom bloom = null)
        {
            // Canvas batches retain their layer before camera pre-cull. Continue
            // the early scope; restoring/re-borrowing here loses that isolation.
            if (camera == current && begunFrame == Time.frameCount)
            {
                UpdateCamera(current, nonJitteredProjection);
                return unavailable;
            }
            End();
            if (!current || current != GameCamera.main) return null;
            if (!indicator) indicator = Object.FindObjectOfType<UISailIndicator>(true);
            if (!indicator || !indicator.group || !indicator.group.activeInHierarchy) return null;
            if (group != indicator.group || texts == null)
            {
                group = indicator.group;
                texts = group.GetComponentsInChildren<TextMesh>(true);
            }
            camera = current;
            begunFrame = Time.frameCount;
            UpdateCamera(current, nonJitteredProjection);
            try
            {
                unavailable = canvas.Begin(current, group, beforeCanvas, bloom);
                if (unavailable != null && bloom != null) return unavailable;
                foreach (var text in texts)
                {
                    if (!text || !text.gameObject.activeInHierarchy || string.IsNullOrEmpty(text.text)) continue;
                    var renderer = text.GetComponent<Renderer>();
                    if (!renderer || !renderer.enabled || renderer.forceRenderingOff ||
                        (current.cullingMask & (1 << renderer.gameObject.layer)) == 0) continue;
                    var material = renderer.sharedMaterial;
                    // Do not repurpose an unknown/lit replacement shader: DrawRenderer
                    // does not populate per-object lighting, and depth must stay untouched.
                    if (!material || !material.shader || material.shader.name != "GUI/Text Shader" || material.passCount != 1) continue;
                    if (visibility.Suppress(renderer)) entries.Add(new Entry { Renderer = renderer, Material = material });
                }
                return unavailable;
            }
            catch { End(); throw; }
        }
        private void UpdateCamera(Camera current, Matrix4x4 nonJitteredProjection)
        {
            view = current.worldToCameraMatrix;
            projection = nonJitteredProjection;
            viewport = current.pixelRect;
        }
        public void RestoreVisibility() => visibility.Restore();
        public int Draw(RenderTexture destination)
        {
            // Never rely on DrawRenderer bypassing forceRenderingOff. World geometry
            // has finished at OnRenderImage; restore renderers before the explicit draw.
            RestoreVisibility();
            if (PendingCount == 0 || !camera) { End(); return 0; }
            var previous = RenderTexture.active;
            int count = 0;
            try
            {
                if (entries.Count > 0) using (var commands = new CommandBuffer { name = "DSPAAMod native-resolution world text" })
                {
                    if (destination) commands.SetRenderTarget(destination);
                    else commands.SetRenderTarget(BuiltinRenderTextureType.CameraTarget);
                    commands.SetViewport(destination ? new Rect(0, 0, destination.width, destination.height) : camera.pixelRect);
                    // Built-in SetViewProjectionMatrices takes the camera's CPU
                    // projection (Unity's documented convention), not an already
                    // API-adjusted GL.GetGPUProjectionMatrix result.
                    commands.SetViewProjectionMatrices(view, projection);
                    foreach (var entry in entries)
                    {
                        var renderer = entry.Renderer;
                        if (!renderer || !renderer.enabled || renderer.forceRenderingOff || !renderer.gameObject.activeInHierarchy || !entry.Material) continue;
                        commands.DrawRenderer(renderer, entry.Material, 0, 0);
                        ++count;
                    }
                    commands.SetViewProjectionMatrices(camera.worldToCameraMatrix, camera.projectionMatrix);
                    Graphics.ExecuteCommandBuffer(commands);
                }
                return count + canvas.Draw(destination, view, projection, viewport);
            }
            finally
            {
                RenderTexture.active = previous;
                entries.Clear();
                canvas.End();
            }
        }
        public int DrawWithBloom(NavigationBloom bloom)
        {
            if (!bloom.HasWorldBloom) return Draw(null);
            RestoreVisibility();
            if (PendingCount == 0 || !camera) { End(); return 0; }
            CommandBuffer legacy = null;
            int count = 0;
            try
            {
                foreach (var entry in entries)
                {
                    var renderer = entry.Renderer;
                    if (!renderer || !renderer.enabled || renderer.forceRenderingOff || !renderer.gameObject.activeInHierarchy || !entry.Material) continue;
                    if (legacy == null) legacy = new CommandBuffer { name = "DSPAASR navigation text" };
                    // The isolated camera supplies view/projection and CameraTarget;
                    // its empty culling mask keeps legacy scene geometry out.
                    legacy.DrawRenderer(renderer, entry.Material, 0, 0);
                    ++count;
                }
                return count + canvas.DrawWithBloom(bloom, legacy, view, projection, viewport);
            }
            finally
            {
                legacy?.Release();
                entries.Clear();
                canvas.End();
            }
        }
        public void End()
        {
            try { RestoreVisibility(); }
            finally
            {
                try { canvas.End(); }
                finally { entries.Clear(); camera = null; begunFrame = -1; unavailable = null; }
            }
        }
        public void Dispose()
        {
            try { End(); }
            finally { canvas.Dispose(); }
        }
    }
}
