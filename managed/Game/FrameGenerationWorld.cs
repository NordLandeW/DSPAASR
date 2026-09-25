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
        private Camera camera, historyCamera;
        private DepthTextureMode savedDepth, installedDepth;
        private bool ownsDepth, history;
        private ulong seen, copied, previousId, previousGeneration, depthEpoch;
        public NativeCaptureTexture DepthSource => new NativeCaptureTexture { Resource = depthPointer, Epoch = depthEpoch };
        public Matrix4x4 UnjitteredProjection => unjittered;
        private int width, height, scene;
        private RenderTexture depth, motion;
        private IntPtr depthPointer, motionPointer;
        private Matrix4x4 unjittered, renderedProjection, previousViewProjection;
        private Vector3 previousPosition;
        private Quaternion previousRotation;
        private string lastFailure;
        private readonly bool traceDepthAttachment = Environment.GetEnvironmentVariable("DSPAASR_CAPTURE_DEPTH_BINDINGS") == "1";
        private readonly System.Collections.Generic.HashSet<int> tracedDepthScenes = new System.Collections.Generic.HashSet<int>();
        public bool InputsReady => copied != 0 && copied == presentation.ApplicationFrameId;
        public bool OwnsFrameCamera(Camera value) => camera && value == camera && seen == presentation.ApplicationFrameId;
        public FrameGenerationWorld(FrameGenerationController owner, Action<string> warn)
        {
            presentation = owner; warning = warn;
            Camera.onPreCull += BeforeCull;
            Camera.onPreRender += BeforeRaster;
            Camera.onPostRender += AfterRaster;
        }
        public void BeforeCull(Camera value)
        {
            if (!presentation.Capturing || !value || value != GameCamera.main) return;
            ulong id = presentation.ApplicationFrameId;
            if (seen == id) return; // Manual screenshot/preview cannot replace the main-display snapshot.
            if (value.targetTexture || value.targetDisplay != 0 || value.stereoEnabled || value.orthographic ||
                value.rect != new Rect(0,0,1,1) || !SystemInfo.supportsMotionVectors) return;
            RestoreDepth();
            seen = id; copied = 0;
            camera = value;
            unjittered = renderedProjection = value.projectionMatrix;
            savedDepth = value.depthTextureMode;
            installedDepth = savedDepth | DepthTextureMode.Depth | DepthTextureMode.MotionVectors;
            value.depthTextureMode = installedDepth; ownsDepth = true;
        }
        // Harmony also invokes this AFTER the original PP pre-cull so an inactive
        // TAA's stale nonJitteredProjectionMatrix is never mistaken for this frame.
        public void AfterProjection(PostProcessingBehaviour behaviour)
        {
            if (!camera || !behaviour || behaviour.GetComponent<Camera>() != camera || !presentation.Capturing) return;
            var profile = behaviour.profile;
            if (profile && profile.antialiasing.enabled && profile.antialiasing.settings.method == AntialiasingModel.Method.Taa &&
                !profile.debugViews.willInterrupt) unjittered = camera.nonJitteredProjectionMatrix;
        }
        private void BeforeRaster(Camera value)
        {
            if (presentation.Capturing && camera && value == camera && seen == presentation.ApplicationFrameId)
                renderedProjection = value.projectionMatrix; // PP may reset it in OnPostRender before our callback.
        }
        private void AfterRaster(Camera value)
        {
            if (!presentation.Capturing || !camera || value != camera || seen != presentation.ApplicationFrameId || copied == seen) return;
            try {
                if (traceDepthAttachment && tracedDepthScenes.Count < 8 && tracedDepthScenes.Add(GameCamera.sceneIndex)) {
                    var active = value.activeTexture;
                    warning("FG depth-attachment diagnostic frame=" + seen + " scene=" + GameCamera.sceneIndex +
                        " active=" + (active ? active.name : "none") + (active ? " size=" + active.width + "x" + active.height +
                        " color=" + active.format + " depthBits=" + active.depth + " samples=" + active.antiAliasing +
                        " colorPtr=0x" + active.GetNativeTexturePtr().ToInt64().ToString("X16") +
                        " depthPtr=0x" + active.GetNativeDepthBufferPtr().ToInt64().ToString("X16") : ""));
                }
                int w = value.pixelWidth, h = value.pixelHeight;
                if (w <= 0 || h <= 0) return;
                EnsureTargets(w,h);
                using (var commands = new CommandBuffer { name = "DSPAASR independent FG depth/motion" }) {
                    commands.Blit(BuiltinRenderTextureType.Depth, depth);
                    commands.Blit(BuiltinRenderTextureType.MotionVectors, motion);
                    Graphics.ExecuteCommandBuffer(commands);
                }
                ref var inputs = ref presentation.Inputs;
                inputs.Depth = depthPointer; inputs.Motion = motionPointer;
                inputs.RenderWidth = (uint)w; inputs.RenderHeight = (uint)h;
                inputs.MotionScaleX = -w; inputs.MotionScaleY = -h; // Unity current-minus-previous UV -> SDK pixel displacement.
                inputs.Milliseconds = Mathf.Max(Time.unscaledDeltaTime, .000001f) * 1000;
                inputs.CameraNear = value.nearClipPlane; inputs.CameraFar = value.farClipPlane;
                inputs.VerticalFov = value.fieldOfView * Mathf.Deg2Rad;
                inputs.PreExposure = inputs.ViewSpaceToMeters = 1;
                inputs.Flags = 16u | (SystemInfo.usesReversedZBuffer ? 4u : 0u);
                var projection = GL.GetGPUProjectionMatrix(unjittered, true);
                var raster = GL.GetGPUProjectionMatrix(renderedProjection, true);
                var anchor = new Vector4(0,0,-1,1);
                var cleanPoint = projection * anchor; var rasterPoint = raster * anchor;
                inputs.JitterX = (rasterPoint.x / rasterPoint.w - cleanPoint.x / cleanPoint.w) * w * .5f;
                inputs.JitterY = -(rasterPoint.y / rasterPoint.w - cleanPoint.y / cleanPoint.w) * h * .5f;
                var viewProjection = projection * value.worldToCameraMatrix;
                bool reset = !history || historyCamera != value || previousId + 1 != seen || previousGeneration != presentation.Generation ||
                    scene != GameCamera.sceneIndex || (value.transform.position - previousPosition).sqrMagnitude > 100f ||
                    Quaternion.Angle(value.transform.rotation, previousRotation) > 30f;
                var toPrevious = reset ? Matrix4x4.identity : previousViewProjection * viewProjection.inverse;
                Store(inputs.CameraViewToClip, projection); Store(inputs.ClipToCameraView, projection.inverse);
                Store(inputs.ClipToPrevClip, toPrevious); Store(inputs.PrevClipToClip, toPrevious.inverse);
                Store(inputs.CameraPosition,value.transform.position); Store(inputs.CameraUp,value.transform.up);
                Store(inputs.CameraRight,value.transform.right); Store(inputs.CameraForward,value.transform.forward);
                if (reset) inputs.Flags |= 2u;
                previousViewProjection = viewProjection; previousPosition = value.transform.position; previousRotation = value.transform.rotation;
                previousId = seen; previousGeneration = presentation.Generation; scene = GameCamera.sceneIndex; historyCamera = value;
                history = true; copied = seen;
                // 'complete' is deliberately NOT set here. The UI capture owner
                // must prove the matching clean/occlusion/influence planes too.
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
            RetireTargets(); width = w; height = h; history = false; ++depthEpoch;
            if (depthEpoch == 0) throw new InvalidOperationException("World depth allocation identity wrapped.");
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
        public void EndFrame() { RestoreDepth(); camera = null; }
        private void RestoreDepth()
        {
            // Do not overwrite unrelated flag changes made by another component.
            if (ownsDepth && camera && camera.depthTextureMode == installedDepth) camera.depthTextureMode = savedDepth;
            ownsDepth = false;
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
            EndFrame(); RetireTargets();
        }
    }
}
