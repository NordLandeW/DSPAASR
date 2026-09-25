using System;
using System.Collections.Generic;
using UnityEngine;
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
        public string Begin(Camera source, GameObject group, bool beforeCanvas)
        {
            End();
            var canvases = group.GetComponentsInChildren<Canvas>(true);
            if (canvases.Length == 0) return null; // Legacy TextMesh board.
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
            if (!renderer)
            {
                var owner = new GameObject("DSPAASR navigation canvas camera") { hideFlags = HideFlags.HideAndDontSave };
                renderer = owner.AddComponent<Camera>();
                renderer.enabled = false;
            }
            renderer.CopyFrom(source);
            renderer.enabled = false; // Always explicitly rendered, never scheduled twice.
            renderer.RemoveAllCommandBuffers();
            renderer.renderingPath = RenderingPath.Forward;
            renderer.clearFlags = CameraClearFlags.Nothing;
            renderer.cullingMask = bit;
            renderer.depthTextureMode = DepthTextureMode.None;
            renderer.allowMSAA = false;
            renderer.allowHDR = false;
            renderer.allowDynamicResolution = false;
            renderer.forceIntoRenderTexture = false;
            renderer.useJitteredProjectionMatrixForTransparentRendering = false;
            renderer.useOcclusionCulling = false;
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
            if (count == 0 || !renderer || !canvas || !canvas.isActiveAndEnabled) { End(); return 0; }
            var previous = RenderTexture.active;
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
                // Camera.Render handles the render-texture/backbuffer projection
                // convention and UGUI's original material/atlas/alpha itself.
                renderer.Render();
                return count;
            }
            finally
            {
                // SR-only drawing is nested inside the original PP stack. Its
                // following DoF/etc must retain the main camera's depth inputs.
                Shader.SetGlobalTexture(DepthTexture, depth);
                Shader.SetGlobalTexture(DepthNormalsTexture, depthNormals);
                Shader.SetGlobalTexture(MotionTexture, motion);
                RenderTexture.active = previous;
                End();
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
