using System;
using System.Runtime.InteropServices;
using DSPAAMod.Interop;
using UnityEngine;
using UnityEngine.PostProcessing;
using UnityEngine.Rendering;

namespace DSPAAMod.Game
{
    // Depth/motion are independent of the selected SR/AA. This observer never
    // advances TAA/SR, invokes an effect twice, or selects another camera's token.
    internal sealed class FrameGenerationWorld : IDisposable
    {
        private readonly FrameGenerationController presentation;
        private readonly Action<string> warning;
        private Camera camera, depthCamera, historyCamera;
        private DepthTextureMode savedDepth, installedDepth;
        private bool ownsDepth, depthHistoryReady, history, previousInputYFlip;
        private ulong seen, rastered, copied, previousId, previousGeneration;
        public Matrix4x4 UnjitteredProjection => unjittered;
        private int width, height, scene, rasterWidth, rasterHeight, logicalWidth, logicalHeight;
        private RenderTexture depth, motion;
        private IntPtr depthPointer, motionPointer;
        private Matrix4x4 unjittered, renderedProjection, previousViewProjection;
        private Vector3 previousPosition;
        private Quaternion previousRotation;
        private string lastFailure;
        private Camera eventCamera;
        private readonly CommandBuffer worldEnd = new CommandBuffer { name = "DSPAASR world-color boundary" };
        private readonly CommandBuffer worldInputs = new CommandBuffer { name = "DSPAASR independent FG depth/motion" };
        public bool InputsReady => copied != 0 && copied == presentation.ApplicationFrameId;
        public bool OwnsFrameCamera(Camera value) => camera && value == camera && seen == presentation.ApplicationFrameId;
        internal static bool AcceptsCamera(Camera value) => value && value == GameCamera.main && value.isActiveAndEnabled &&
            !value.targetTexture && value.targetDisplay == 0 && !value.stereoEnabled && !value.orthographic &&
            value.rect == new Rect(0,0,1,1) && SystemInfo.supportsMotionVectors;
        public FrameGenerationWorld(FrameGenerationController owner, Action<string> warn)
        {
            presentation = owner; warning = warn;
            Camera.onPreCull += BeforeCull;
            Camera.onPreRender += BeforeRaster;
            Camera.onPostRender += AfterRaster;
        }
        public void BeforeCull(Camera value)
        {
            presentation.NavigationBefore(value);
            if (!presentation.Capturing || !AcceptsCamera(value)) return;
            ulong id = presentation.ApplicationFrameId;
            if (seen == id) return; // Manual screenshot/preview cannot replace the main-display snapshot.
            RequestDepth(value);
            seen = id; copied = 0;
            camera = value;
            logicalWidth = Screen.width; logicalHeight = Screen.height;
            rasterWidth = rasterHeight = 0;
            unjittered = renderedProjection = value.projectionMatrix;
            presentation.BeginNavigation(value, unjittered);
            if (eventCamera != value) {
                if (eventCamera) eventCamera.RemoveCommandBuffer(CameraEvent.AfterEverything, worldEnd);
                eventCamera = value; eventCamera.AddCommandBuffer(CameraEvent.AfterEverything, worldEnd);
            }
            worldEnd.Clear();
            worldEnd.IssuePluginEventAndData(presentation.RenderEvent, 3, new IntPtr(unchecked((long)id)));
        }
        // Harmony also invokes this AFTER the original PP pre-cull so an inactive
        // TAA's stale nonJitteredProjectionMatrix is never mistaken for this frame.
        public void AfterProjection(PostProcessingBehaviour behaviour)
        {
            if (!camera || !behaviour || behaviour.GetComponent<Camera>() != camera || !presentation.Capturing) return;
            var profile = behaviour.profile;
            if (profile && profile.antialiasing.enabled && profile.antialiasing.settings.method == AntialiasingModel.Method.Taa &&
                !profile.debugViews.willInterrupt) unjittered = camera.nonJitteredProjectionMatrix;
            presentation.NavigationProjection(camera, unjittered);
        }
        private void BeforeRaster(Camera value)
        {
            if (presentation.Capturing && camera && value == camera && seen == presentation.ApplicationFrameId) {
                // The SR target is still bound here. Its final image effect restores
                // the logical target before onPostRender, so pixelWidth there is L, not R.
                rasterWidth = value.pixelWidth; rasterHeight = value.pixelHeight;
                renderedProjection = value.projectionMatrix; // PP may reset it in OnPostRender before our callback.
            }
        }
        private void AfterRaster(Camera value)
        {
            if (!presentation.Capturing || !camera || value != camera || seen != presentation.ApplicationFrameId || rastered == seen) return;
            rastered = seen; // A repeated render cannot turn the same warm-up frame into valid history.
            // Enabling motion vectors in pre-cull is too late to provide an
            // earlier camera transform on this first render. Present Final only
            // until Unity has completed a render with a persistent request.
            if (!depthHistoryReady) { depthHistoryReady = true; return; }
            try {
                int w = rasterWidth, h = rasterHeight;
                if (w <= 0 || h <= 0 || logicalWidth <= 0 || logicalHeight <= 0) return;
                EnsureTargets(w,h);
                // Built-in depth/motion use Unity's render-texture projection;
                // H/Final use the native display surface. Transform the texel
                // domain, vector basis and all camera metadata together.
                var projection = GL.GetGPUProjectionMatrix(unjittered, true);
                var raster = GL.GetGPUProjectionMatrix(renderedProjection, true);
                bool flipY = projection.m11 * unjittered.m11 < 0;
                var scale = new Vector2(1, flipY ? -1 : 1);
                var offset = new Vector2(0, flipY ? 1 : 0);
                var toDisplay = Matrix4x4.Scale(new Vector3(1, scale.y, 1));
                projection = toDisplay * projection; raster = toDisplay * raster;
                worldInputs.Clear();
                worldInputs.Blit(BuiltinRenderTextureType.Depth, depth, scale, offset);
                worldInputs.Blit(BuiltinRenderTextureType.MotionVectors, motion, scale, offset);
                Graphics.ExecuteCommandBuffer(worldInputs);
                ref var inputs = ref presentation.Inputs;
                inputs.Depth = depthPointer; inputs.Motion = motionPointer;
                inputs.RenderWidth = (uint)w; inputs.RenderHeight = (uint)h;
                inputs.LogicalOutputWidth = (uint)logicalWidth; inputs.LogicalOutputHeight = (uint)logicalHeight;
                // Blit reorders texels without negating their RG values. Convert
                // Unity current-minus-previous RT UV to previous-minus-current
                // display pixels; a reflected Y basis therefore has +height.
                inputs.MotionScaleX = -w; inputs.MotionScaleY = flipY ? h : -h;
                inputs.Milliseconds = Mathf.Max(Time.unscaledDeltaTime, .000001f) * 1000;
                inputs.CameraNear = value.nearClipPlane; inputs.CameraFar = value.farClipPlane;
                inputs.VerticalFov = value.fieldOfView * Mathf.Deg2Rad;
                inputs.PreExposure = inputs.ViewSpaceToMeters = 1;
                inputs.Flags = 16u | (SystemInfo.usesReversedZBuffer ? 4u : 0u);
                var anchor = new Vector4(0,0,-1,1);
                var cleanPoint = projection * anchor; var rasterPoint = raster * anchor;
                inputs.JitterX = (rasterPoint.x / rasterPoint.w - cleanPoint.x / cleanPoint.w) * w * .5f;
                inputs.JitterY = -(rasterPoint.y / rasterPoint.w - cleanPoint.y / cleanPoint.w) * h * .5f;
                var viewProjection = projection * value.worldToCameraMatrix;
                bool reset = !history || historyCamera != value || previousId + 1 != seen || previousGeneration != presentation.Generation ||
                    previousInputYFlip != flipY || scene != GameCamera.sceneIndex || (value.transform.position - previousPosition).sqrMagnitude > 100f ||
                    Quaternion.Angle(value.transform.rotation, previousRotation) > 30f;
                var toPrevious = reset ? Matrix4x4.identity : previousViewProjection * viewProjection.inverse;
                Store(inputs.CameraViewToClip, projection); Store(inputs.ClipToCameraView, projection.inverse);
                Store(inputs.ClipToPrevClip, toPrevious); Store(inputs.PrevClipToClip, toPrevious.inverse);
                Store(inputs.CameraPosition,value.transform.position); Store(inputs.CameraUp,value.transform.up);
                Store(inputs.CameraRight,value.transform.right); Store(inputs.CameraForward,value.transform.forward);
                if (reset) inputs.Flags |= 2u;
                previousViewProjection = viewProjection; previousPosition = value.transform.position; previousRotation = value.transform.rotation;
                previousId = seen; previousGeneration = presentation.Generation; scene = GameCamera.sceneIndex; historyCamera = value;
                previousInputYFlip = flipY; history = true; copied = seen;
                // EOF marks these metadata inputs ready. Native publication still
                // requires the same frame's post-world, pre-screen-UI snapshot.
            } catch (Exception error) {
                history = false; copied = 0;
                if (lastFailure != error.Message) { lastFailure = error.Message; warning("FG world inputs unavailable: " + error.Message); }
            }
        }
        private static void Store(float[] output, Matrix4x4 matrix)
        {
            // Unity transforms column vectors. SL's row-vector, row-major matrix
            // is the transpose; projection and cross-frame transforms use one convention.
            for (int row = 0; row < 4; ++row) for (int column = 0; column < 4; ++column) output[row * 4 + column] = matrix[column,row];
        }
        private static void Store(float[] output, Vector3 vector) { output[0] = vector.x; output[1] = vector.y; output[2] = vector.z; }
        private void EnsureTargets(int w, int h)
        {
            if (w == width && h == height && depth && motion && depth.IsCreated() && motion.IsCreated()) return;
            RetireTargets(); width = w; height = h; history = false;
            try { depth = Create("FG depth",RenderTextureFormat.RFloat,w,h,out depthPointer);
                motion = Create("FG motion",RenderTextureFormat.RGHalf,w,h,out motionPointer); }
            catch { RetireTargets(); throw; }
        }
        private static RenderTexture Create(string name, RenderTextureFormat format, int w, int h, out IntPtr pointer)
        {
            pointer = IntPtr.Zero;
            var texture = new RenderTexture(w,h,0,format,RenderTextureReadWrite.Linear) {
                name = "DSPAASR " + name, hideFlags = HideFlags.HideAndDontSave, antiAliasing = 1,
                useMipMap = false, autoGenerateMips = false, filterMode = FilterMode.Point, wrapMode = TextureWrapMode.Clamp
            };
            try {
                if (!texture.Create()) throw new InvalidOperationException("Cannot allocate " + name);
                pointer = texture.GetNativeTexturePtr();
                if (pointer == IntPtr.Zero) throw new InvalidOperationException("Cannot retain " + name);
                Marshal.AddRef(pointer); return texture;
            } catch { texture.Release(); UnityEngine.Object.Destroy(texture); throw; }
        }
        public void EndFrame()
        {
            presentation.EndNavigation();
            // The request belongs to the active camera, not an individual frame.
            // Clearing MotionVectors at EOF makes Unity discard its previous VP.
            // SR may have begun consuming the same flags after our acquisition.
            // Turning FG off must not erase SR's still-active camera history.
            if (!depthCamera || depthCamera != GameCamera.main || !depthCamera.isActiveAndEnabled ||
                (!presentation.TemporalRequested && !presentation.SharesMotionRequest(depthCamera)))
                RestoreDepth();
            camera = null; worldEnd.Clear();
        }
        private void RequestDepth(Camera value)
        {
            const DepthTextureMode required = DepthTextureMode.Depth | DepthTextureMode.MotionVectors;
            if (ownsDepth && depthCamera == value && (value.depthTextureMode & required) == required) return;
            RestoreDepth();
            depthCamera = value; savedDepth = value.depthTextureMode;
            installedDepth = savedDepth | required;
            value.depthTextureMode = installedDepth; ownsDepth = true;
        }
        private void RestoreDepth()
        {
            // Do not overwrite unrelated flag changes made by another component.
            if (ownsDepth && depthCamera && depthCamera.depthTextureMode == installedDepth) depthCamera.depthTextureMode = savedDepth;
            depthCamera = null; ownsDepth = false; depthHistoryReady = false; history = false;
        }
        private void RetireTargets()
        {
            // Every already queued native envelope owns COM refs. Unity's release
            // follows its own ordered render commands; no SDK borrows a naked pointer.
            if (depthPointer != IntPtr.Zero) Marshal.Release(depthPointer);
            if (motionPointer != IntPtr.Zero) Marshal.Release(motionPointer);
            depthPointer = motionPointer = IntPtr.Zero;
            if (depth) { depth.Release(); UnityEngine.Object.Destroy(depth); }
            if (motion) { motion.Release(); UnityEngine.Object.Destroy(motion); }
            depth = motion = null;
        }
        public void Dispose()
        {
            Camera.onPreCull -= BeforeCull; Camera.onPreRender -= BeforeRaster; Camera.onPostRender -= AfterRaster;
            EndFrame(); RestoreDepth(); RetireTargets();
            if (eventCamera) eventCamera.RemoveCommandBuffer(CameraEvent.AfterEverything, worldEnd);
            eventCamera = null; worldEnd.Dispose(); worldInputs.Dispose();
        }
    }
}
