using System.Collections.Generic;
using DSPAAMod.Core;
using UnityEngine;
using UnityEngine.Rendering;

namespace DSPAAMod.Game
{
    // This unlit, ZTest-Always world HUD is not ordinary opaque scene geometry.
    // Keep the game's generated TextMesh, material and transform; only move its
    // draw from the jittered world input to the native-size temporal resolve.
    internal sealed class WorldTextOverlay
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
        private Matrix4x4 view, projection;
        public int PendingCount => entries.Count;
        public void Begin(Camera current, Matrix4x4 nonJitteredProjection)
        {
            End();
            if (current != GameCamera.main) return;
            if (!indicator) indicator = Object.FindObjectOfType<UISailIndicator>(true);
            if (!indicator || !indicator.group || !indicator.group.activeInHierarchy) return;
            if (group != indicator.group || texts == null)
            {
                group = indicator.group;
                texts = group.GetComponentsInChildren<TextMesh>(true);
            }
            camera = current;
            view = current.worldToCameraMatrix;
            projection = nonJitteredProjection;
            try
            {
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
            }
            catch { End(); throw; }
        }
        public void RestoreVisibility() => visibility.Restore();
        public int Draw(RenderTexture destination)
        {
            // Never rely on DrawRenderer bypassing forceRenderingOff. World geometry
            // has finished at OnRenderImage; restore renderers before the explicit draw.
            RestoreVisibility();
            if (entries.Count == 0 || !camera) { entries.Clear(); return 0; }
            var previous = RenderTexture.active;
            int count = 0;
            try
            {
                using (var commands = new CommandBuffer { name = "DSPAAMod native-resolution world text" })
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
                return count;
            }
            finally
            {
                RenderTexture.active = previous;
                entries.Clear();
            }
        }
        public void End()
        {
            try { RestoreVisibility(); }
            finally { entries.Clear(); camera = null; }
        }
    }
}
