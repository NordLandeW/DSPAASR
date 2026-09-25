using System;
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.UI;

namespace DSPAAMod.Game
{
    // The game's current sail board owns a small WorldSpace Canvas. Let Unity
    // render that SAME Canvas: rebuilding its glyph meshes/material properties
    // here would duplicate UGUI's font, tint, masking and color-space behavior.
    internal sealed class SailCanvasOverlay : IDisposable
    {
        private struct LayerChange
        {
            public GameObject Object;
            public int Original;
        }
        private readonly List<LayerChange> layers = new List<LayerChange>();
        private Canvas canvas;
        private Camera renderer;
        private int isolatedLayer = -1;
        private int count, checkedCameraLayers, lastUsedFrame = -2;
        private static readonly int DepthTexture = Shader.PropertyToID("_CameraDepthTexture");
        private static readonly int DepthNormalsTexture = Shader.PropertyToID("_CameraDepthNormalsTexture");
        private static readonly int MotionTexture = Shader.PropertyToID("_CameraMotionVectorsTexture");
        public int PendingCount => count;

        // A non-null reason rejects only this optional draw relocation. Unity
        // execution/restoration failures are exceptions, never capability results.
        public string Begin(Camera source, GameObject group, bool beforeCanvas, NavigationBloom bloom = null)
        {
            End();
            var canvases = group.GetComponentsInChildren<Canvas>(true);
            if (canvases.Length == 0)
            {
                // Legacy TextMesh draws can use this same explicit camera via a
                // command buffer; its empty mask cannot redraw the world.
                if (bloom == null) return null;
                ConfigureRenderer(source, 0);
                return bloom.Preflight(source);
            }
            // Only the board's own canvas, never the shared world/UI canvas or
            // arbitrary WorldSpace graphics. Unknown replacements stay original.
            if (canvases.Length != 1) return "Navigation board has an unknown canvas hierarchy.";
            var candidate = canvases[0];
            if (!candidate.isActiveAndEnabled) return null;
            if (candidate.renderMode != RenderMode.WorldSpace || !candidate.isRootCanvas || candidate.worldCamera ||
                (source.cullingMask & (1 << candidate.gameObject.layer)) == 0)
                return "Navigation board is not the original main-camera WorldSpace canvas.";
            if (!beforeCanvas) return "Navigation canvas was not prepared before world rendering.";
            if (candidate.GetComponentsInChildren<Renderer>(true).Length != 0)
                return "Navigation canvas contains non-UI renderers.";
            var graphics = candidate.GetComponentsInChildren<Graphic>(true);
            if (candidate.GetComponentsInChildren<CanvasRenderer>(true).Length != graphics.Length)
                return "Navigation canvas contains an unknown canvas renderer.";
            foreach (var graphic in graphics)
            {
                var text = graphic as Text;
                if (!text)
                    return "Navigation canvas contains an unsupported non-text graphic.";
                var material = text.materialForRendering;
                if (!material || !material.shader || material.shader.name != "UI Ex/Text Alpha YCGen(IconSet)")
                    return "Navigation text material changed; keeping the original drawing path.";
            }
            if (graphics.Length == 0) return null;
            int cameraLayers = 0;
            foreach (var other in Camera.allCameras)
                if (other != renderer) cameraLayers |= other.cullingMask;
            if (canvas != candidate || checkedCameraLayers != cameraLayers || lastUsedFrame + 1 < Time.frameCount)
            {
                canvas = candidate;
                isolatedLayer = FindUnusedLayer(cameraLayers);
                checkedCameraLayers = cameraLayers;
            }
            // A rejected optional adaptation must not become a full scene scan
            // every frame. Recheck on visibility, canvas or camera-mask changes.
            lastUsedFrame = Time.frameCount;
            if (isolatedLayer < 0) return "No unused rendering layer is available for the navigation canvas.";
            int bit = 1 << isolatedLayer;
            if (!string.IsNullOrEmpty(LayerMask.LayerToName(isolatedLayer)))
                return "The navigation drawing layer is now named for another use.";
            if ((cameraLayers & bit) != 0) return "The navigation drawing layer is now used by another camera.";
            ConfigureRenderer(source, bit);
            string unavailable = bloom?.Preflight(source);
            if (unavailable != null) return unavailable; // No layer was changed yet.
            try
            {
                Isolate(candidate.gameObject);
                foreach (var graphic in graphics) Isolate(graphic.gameObject);
                // Called from preWillRenderCanvases: the normal Canvas update
                // builds its batches with this layer. No recursive forced update.
                count = graphics.Length;
                return null;
            }
            catch { End(); throw; }
        }
        private void ConfigureRenderer(Camera source, int mask)
        {
            if (!renderer)
            {
                var owner = new GameObject("DSPAASR navigation canvas camera") { hideFlags = HideFlags.HideAndDontSave };
                renderer = owner.AddComponent<Camera>();
                renderer.enabled = false;
            }
            renderer.CopyFrom(source);
            renderer.enabled = false;
            renderer.RemoveAllCommandBuffers();
            renderer.renderingPath = RenderingPath.Forward;
            renderer.clearFlags = CameraClearFlags.Nothing;
            renderer.cullingMask = mask;
            renderer.depthTextureMode = DepthTextureMode.None;
            renderer.allowMSAA = false;
            renderer.allowHDR = false;
            renderer.allowDynamicResolution = false;
            renderer.forceIntoRenderTexture = false;
            renderer.useJitteredProjectionMatrixForTransparentRendering = false;
            renderer.useOcclusionCulling = false;
        }
        private void Isolate(GameObject item)
        {
            if (item.layer == isolatedLayer) return;
            layers.Add(new LayerChange { Object = item, Original = item.layer });
            item.layer = isolatedLayer;
        }

        private int FindUnusedLayer(int occupied)
        {
            // Acquire on canvas/visibility changes, not a per-frame scene scan. Do not
            // rename a layer or steal a named/game/other-mod rendering layer.
            foreach (var item in Resources.FindObjectsOfTypeAll<Renderer>()) occupied |= 1 << item.gameObject.layer;
            foreach (var item in Resources.FindObjectsOfTypeAll<Canvas>()) occupied |= 1 << item.gameObject.layer;
            // Unity's runtime layer API permits 6/7; DSP names every layer 8-31.
            // Keep built-in system layers 0-5 excluded, including unnamed layer 3.
            for (int layer = 31; layer >= 6; --layer)
                if ((occupied & (1 << layer)) == 0 && string.IsNullOrEmpty(LayerMask.LayerToName(layer))) return layer;
            return -1;
        }

        public int Draw(RenderTexture destination, Matrix4x4 view, Matrix4x4 projection, Rect viewport)
        {
            try { return DrawCamera(destination, view, projection, viewport); }
            finally { End(); }
        }
        private bool VisibleCanvas => count > 0 && canvas && canvas.isActiveAndEnabled;
        public int DrawWithBloom(NavigationBloom bloom, CommandBuffer legacy, Matrix4x4 view, Matrix4x4 projection, Rect viewport)
        {
            if (!VisibleCanvas && legacy == null) return 0;
            Exception failure = null;
            CommandBuffer composition = null;
            try
            {
                try
                {
                    if (!bloom.Source) throw new InvalidOperationException("Navigation HDR target was not prepared before world rendering.");
                    DrawCamera(bloom.Source, view, projection, viewport, legacy, null, true);
                    composition = bloom.PrepareComposite(renderer);
                }
                catch (Exception error)
                {
                    bloom.Failed(error);
                    failure = error;
                }
                // Even a failed HDR/glow preparation must still submit the actual
                // text this frame. Restoring its old layer alone cannot do that.
                // Never retry this final draw: an execution failure may be partial.
                int drawn = DrawCamera(null, view, projection, viewport, legacy, composition);
                if (failure != null)
                    throw new InvalidOperationException("Navigation text was submitted without bloom after preparation failed: " + failure.Message, failure);
                return drawn;
            }
            finally { bloom.EndFrame(); }
        }
        private int DrawCamera(RenderTexture destination, Matrix4x4 view, Matrix4x4 projection, Rect viewport,
            CommandBuffer legacy = null, CommandBuffer composition = null, bool hdrSource = false)
        {
            if (!renderer || (!VisibleCanvas && legacy == null)) return 0;
            var previous = RenderTexture.active;
            bool srgbWrite = GL.sRGBWrite;
            var depth = Shader.GetGlobalTexture(DepthTexture);
            var depthNormals = Shader.GetGlobalTexture(DepthNormalsTexture);
            var motion = Shader.GetGlobalTexture(MotionTexture);
            try
            {
                renderer.targetTexture = destination;
                renderer.rect = new Rect(0, 0, 1, 1);
                renderer.pixelRect = destination ? new Rect(0, 0, destination.width, destination.height) : viewport;
                renderer.worldToCameraMatrix = view;
                renderer.projectionMatrix = projection;
                renderer.nonJitteredProjectionMatrix = projection;
                renderer.cullingMatrix = projection * view;
                renderer.allowHDR = hdrSource;
                renderer.clearFlags = hdrSource ? CameraClearFlags.SolidColor : CameraClearFlags.Nothing;
                if (hdrSource) renderer.backgroundColor = Color.clear;
                if (legacy != null) renderer.AddCommandBuffer(CameraEvent.AfterForwardAlpha, legacy);
                if (composition != null) renderer.AddCommandBuffer(CameraEvent.AfterEverything, composition);
                // Both renders consume the same early Canvas batch and layer.
                // CameraTarget copies run INSIDE this camera, never in the caller's
                // pre-cull context. Unity owns RT projection and sRGB view handling.
                renderer.Render();
                return VisibleCanvas ? count : 0;
            }
            finally
            {
                try
                {
                    if (renderer)
                    {
                        if (legacy != null) renderer.RemoveCommandBuffer(CameraEvent.AfterForwardAlpha, legacy);
                        if (composition != null) renderer.RemoveCommandBuffer(CameraEvent.AfterEverything, composition);
                        renderer.targetTexture = null;
                        renderer.allowHDR = false;
                        renderer.clearFlags = CameraClearFlags.Nothing;
                    }
                }
                finally
                {
                    // SR-only drawing is nested inside the original PP stack. Its
                    // following DoF/etc must retain the main camera's depth inputs,
                    // even if removal of a failed camera's command buffer throws.
                    try
                    {
                        Shader.SetGlobalTexture(DepthTexture, depth);
                        Shader.SetGlobalTexture(DepthNormalsTexture, depthNormals);
                        Shader.SetGlobalTexture(MotionTexture, motion);
                    }
                    finally { RenderTexture.active = previous; GL.sRGBWrite = srgbWrite; }
                }
            }
        }

        public void End()
        {
            Exception failure = null;
            for (int i = layers.Count - 1; i >= 0; --i)
            {
                try
                {
                    var change = layers[i];
                    if (change.Object && change.Object.layer == isolatedLayer) change.Object.layer = change.Original;
                    layers.RemoveAt(i);
                }
                catch (Exception error) { if (failure == null) failure = error; }
            }
            count = 0;
            if (renderer) renderer.targetTexture = null;
            if (failure != null) throw failure; // Retain failed entries for the next recovery call.
        }
        public void Dispose()
        {
            End();
            if (renderer) UnityEngine.Object.Destroy(renderer.gameObject);
            renderer = null; canvas = null; isolatedLayer = -1; lastUsedFrame = -2;
        }
    }
}
