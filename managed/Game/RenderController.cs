using System;
using System.Collections.Generic;
using System.Reflection;
using DSPAAMod.Core;
using DSPAAMod.Interop;
using HarmonyLib;
using UnityEngine;
using UnityEngine.PostProcessing;
using UnityEngine.Rendering;

namespace DSPAAMod.Game
{
    internal sealed class RenderController
    {
        internal sealed class CameraState
        {
            public Camera Camera;
            public PostProcessingBehaviour Behaviour;
            public ulong Key;
            public RenderTargets Targets;
            public RenderTexture SizingAnchor, OriginalTarget, ImageResult;
            public SrPresentation Presenter;
            public CommandBuffer OpaqueCapture;
            public readonly WorldTextOverlay WorldText = new WorldTextOverlay();
            public bool ReportedWorldText;
            public int WorldTextFrame = -1;
            public int RequestedWidth, RequestedHeight;
            public bool QueryIssued, TargetOverridden, SrThisRender, InputsCopied;
            public Matrix4x4 AppliedProjection;
            public uint Phase;
            public ulong Frame;
            public uint ReportedPreset;
            public int LastFrame = -1, Scene = -1, Profile;
            public Vector3 Position;
            public Quaternion Rotation;
            public Matrix4x4 Projection;
            public JitterSample Jitter;
            public bool Reset = true, Faulted, Prepared, JitterApplied, Overridden, OriginalMsaa, OriginalTransparentJitter;
            public AntialiasingModel Model;
            public AntialiasingModel.Settings OriginalSettings;
            public bool OriginalEnabled;
        }
        internal sealed class ImageScope
        {
            public CameraState State;
            public RenderTexture OriginalDestination, Target, Source;
            public bool TemporalStack;
        }
        private readonly Dictionary<RenderTexture, CameraState> imageSources = new Dictionary<RenderTexture, CameraState>();
        public FrameCapture Capture { get; set; }
        private readonly Dictionary<Camera, CameraState> cameras = new Dictionary<Camera, CameraState>();
        private readonly List<CameraState> retired = new List<CameraState>();
        private readonly Action<string> report;
        private readonly Action<string> information;
        private readonly Func<NativeBridge> acquireNative;
        private readonly Action<TaaComponent, Vector2> setJitter;
        private NativeBridge native;
        private sealed class SupportQuery
        {
            public UpscalerAvailability Availability;
            public RenderTexture Anchor;
            public IntPtr Token;
        }
        private readonly SupportQuery[] supportQueries = new SupportQuery[2];
        private bool supportRequested;
        public UpscalerAvailability Availability => supportQueries[0].Availability;
        public UpscalerAvailability FsrAvailability => supportQueries[1].Availability;
        public UpscalerAvailability GetAvailability(AaChoice choice) => choice == AaChoice.Fsr ? FsrAvailability : Availability;
        private bool FsrSelected => settings.Technique == AaTechnique.Fsr;
        private UpscalerAvailability ActiveAvailability => FsrSelected ? FsrAvailability : Availability;
        private string BackendName => FsrSelected ? "FSR" : "DLSS";
        private ulong nextKey;
        private AaSettings settings;
        private int diagnosticThrough = -1;
        private bool DiagnosticActive => diagnosticThrough >= 0 && Time.frameCount <= diagnosticThrough;
        private static readonly System.Reflection.FieldInfo ContextField = AccessTools.Field(typeof(PostProcessingBehaviour), "m_Context");
        public void RequestCapture()
        {
            if (diagnosticThrough >= 0 || Capture == null || !Capture.Request()) return;
            diagnosticThrough = Time.frameCount + 1;
            Camera.onPreCull += TraceCameraCull;
            Camera.onPostRender += TraceCameraPost;
            TraceOverview("requested");
        }
        private void TraceCameraCull(Camera camera) => TraceStage("camera.preCull", camera);
        private void TraceCameraPost(Camera camera) => TraceStage("camera.postRender", camera);
        private void FinishDiagnostic()
        {
            if (diagnosticThrough < 0) return;
            Camera.onPreCull -= TraceCameraCull;
            Camera.onPostRender -= TraceCameraPost;
            diagnosticThrough = -1;
            TraceOverview("window-complete");
            Capture?.Cancel("The two-frame diagnostic window ended without two consecutive DLSS submissions; inspect PipelineTrace in LogOutput.log.");
        }
        private void TraceOverview(string phase)
        {
            try
            {
                information("PipelineTrace " + phase + " unityFrame=" + Time.frameCount + " scene=" + GameCamera.sceneIndex +
                    " planet=" + (GameMain.localPlanet == null ? "space" : GameMain.localPlanet.id.ToString()) +
                    " selected=" + settings.Technique + "/" + settings.Resolution + " status=" + Status);
                foreach (var camera in Camera.allCameras)
                {
                    var components = new List<string>();
                    foreach (var component in camera.GetComponents<MonoBehaviour>())
                        if (component) components.Add(component.GetType().FullName + ":enabled=" + component.enabled);
                    information("PipelineTrace camera=" + camera.GetInstanceID() + ":" + camera.name + " depth=" + camera.depth +
                        " mask=" + camera.cullingMask + " viewport=" + camera.pixelRect + " target=" + TextureDescription(camera.targetTexture) +
                        " components=" + string.Join(",", components));
                    if (cameras.TryGetValue(camera, out var state)) TraceNative(state);
                }
            }
            catch (Exception error) { report("PipelineTrace overview failed: " + error.Message); }
        }
        private void TraceNative(CameraState state)
        {
            if (native != null && native.TryGetStatus(state.Key, out var result))
                information("PipelineTrace nativeLatest cameraKey=" + state.Key + " submitted=" + state.Frame + " completed=" + result.Frame +
                    " result=" + result.Result + " requested=" + result.RequestedPreset + " observed=" + result.ObservedPreset + " message=" + result.Message);
            else information("PipelineTrace nativeLatest cameraKey=" + state.Key + " submitted=" + state.Frame + " no-completion-record");
        }
        private static string TextureDescription(RenderTexture texture) => texture ? texture.name + ":" + texture.width + "x" + texture.height : "backbuffer";
        private void TraceStage(string stage, Camera camera, PostProcessingContext context = null, RenderTexture source = null, RenderTexture destination = null)
        {
            if (!DiagnosticActive) return;
            try
            {
                if (!camera) { information("PipelineTrace stage=" + stage + " no-camera"); return; }
                var behaviour = camera.GetComponent<PostProcessingBehaviour>();
                if (context == null && behaviour && ContextField != null) context = ContextField.GetValue(behaviour) as PostProcessingContext;
                var profile = behaviour ? behaviour.profile : context?.profile;
                string model = profile ? " profile=" + profile.GetInstanceID() + " enabled=" + profile.antialiasing.enabled +
                    " method=" + profile.antialiasing.settings.method + " debugInterrupt=" + profile.debugViews.willInterrupt : " no-profile";
                model += " contextProfile=" + (context != null && context.profile ? context.profile.GetInstanceID().ToString() : "none");
                string adapter = cameras.TryGetValue(camera, out var state) ? " key=" + state.Key + " prepared=" + state.Prepared +
                    " faulted=" + state.Faulted + " reset=" + state.Reset + " jitterApplied=" + state.JitterApplied +
                    " submitted=" + state.Frame : " unregistered-camera";
                information("PipelineTrace frame=" + Time.frameCount + " stage=" + stage + " camera=" + camera.GetInstanceID() + ":" + camera.name +
                    " postEnabled=" + (behaviour && behaviour.enabled) + model + " interrupted=" + (context != null && context.interrupted) + adapter +
                    " source=" + TextureDescription(source) + " destination=" + TextureDescription(destination));
            }
            catch (Exception error) { report("PipelineTrace stage failed: " + error.Message); }
        }

        public string Status { get; private set; } = "Original game AA; DLAA not enabled.";
        public RenderController(AaSettings initial, Func<NativeBridge> acquire, Action<string> log, Action<string> info)
        {
            settings = initial;
            acquireNative = acquire;
            report = log;
            information = info;
            setJitter = AccessTools.MethodDelegate<Action<TaaComponent, Vector2>>(AccessTools.PropertySetter(typeof(TaaComponent), "jitterVector"));
            for (int i = 0; i < supportQueries.Length; ++i)
                supportQueries[i] = new SupportQuery { Availability = UpscalerAvailability.ForPlatform(
                    SystemInfo.graphicsDeviceType == GraphicsDeviceType.Direct3D11, SystemInfo.graphicsDeviceVendorID,
                    SystemInfo.supportsMotionVectors, SystemInfo.supportsComputeShaders, (UpscalerBackend)i) };
        }
        public void RequestUpscalerSupport() { supportRequested = true; }
        private void UpdateSupport(SupportQuery query)
        {
            if (!query.Availability.Pending) return;
            try
            {
                if (query.Token == IntPtr.Zero)
                {
                    if (native == null) native = acquireNative();
                    query.Anchor = new RenderTexture(2, 2, 0, RenderTextureFormat.ARGB32)
                    { name = "DSPAASR capability device anchor", hideFlags = HideFlags.HideAndDontSave };
                    if (!query.Anchor.Create()) throw new InvalidOperationException("Cannot allocate a graphics-device query anchor.");
                    IntPtr token = native.RequestSupport(query.Anchor.GetNativeTexturePtr(), (uint)query.Availability.Backend);
                    if (token == IntPtr.Zero) throw new InvalidOperationException("Cannot queue the " + query.Availability.Name + " capability check.");
                    IssueControl(token);
                    query.Token = token;
                }
                else if (native.TryGetSupport(query.Token, out var support) && support.Result != 0)
                {
                    query.Token = IntPtr.Zero;
                    DestroyAnchor(query.Anchor);
                    query.Anchor = null;
                    query.Availability = query.Availability.Complete(support.Result == 1, support.Message);
                    if (!query.Availability.Available) report(query.Availability.Describe(false));
                }
            }
            catch (Exception error)
            {
                // An issued query retains its anchor until completion or process teardown.
                if (query.Token == IntPtr.Zero) { DestroyAnchor(query.Anchor); query.Anchor = null; }
                query.Availability = query.Availability.Complete(false, error.Message);
                report(query.Availability.Describe(false));
            }
        }
        public void Configure(AaSettings value)
        {
            Capture?.Cancel("AA settings changed.");
            foreach (var state in cameras.Values)
            {
                Restore(state);
                if (state.Behaviour) state.Behaviour.ResetTemporalEffects();
                Retire(state);
            }
            cameras.Clear();
            settings = value;
            Status = value.Temporal ? "Waiting for a compatible camera." : "Using " + value.Technique;
        }
        public void BeforeCull(PostProcessingBehaviour behaviour)
        {
            if (DiagnosticActive && behaviour) TraceStage("post.preCull-entry", behaviour.GetComponent<Camera>());
            if (!behaviour || !behaviour.profile) return;
            Camera camera = behaviour.GetComponent<Camera>();
            if (!camera || settings.Technique == AaTechnique.Original) return;
            if (!cameras.TryGetValue(camera, out var state))
            {
                // Follow the serialized pipeline reference; do not assume the game's
                // controller and postprocessing component live on the same object.
                bool owned = false;
                foreach (var controller in UnityEngine.Object.FindObjectsOfType<PostEffectController>())
                    if (controller.postScript == behaviour) { owned = true; break; }
                if (!owned) return;
                state = new CameraState { Camera = camera, Behaviour = behaviour, Key = ++nextKey };
                cameras.Add(camera, state);
            }
            Restore(state); // Also recovers a previous render that was interrupted.
            state.Prepared = false;
            state.SrThisRender = false;
            state.InputsCopied = false;
            state.ImageResult = null;
            if (settings.Technique == AaTechnique.Original) return;
            if (settings.Temporal)
            {
                if (!ActiveAvailability.Available)
                {
                    Status = ActiveAvailability.Describe(false);
                    if (state.Presenter) state.Presenter.enabled = false;
                    return;
                }
                if (state.Faulted) { if (state.Presenter) state.Presenter.enabled = false; return; }
                try
                {
                    if (SystemInfo.graphicsDeviceType != GraphicsDeviceType.Direct3D11 || !SystemInfo.supportsMotionVectors ||
                        !SystemInfo.supportsComputeShaders || (!FsrSelected && SystemInfo.graphicsDeviceVendorID != 0x10de) || camera.stereoEnabled)
                        throw new NotSupportedException(BackendName + " requires compatible D3D11, motion vectors, compute support and a non-stereo camera.");
                    if (FsrSelected && camera.orthographic)
                        throw new NotSupportedException("FSR's depth reconstruction requires a perspective camera.");
                    if (behaviour.profile.debugViews.willInterrupt) return;
                    if (native == null) native = acquireNative();
                    if (native.TryGetStatus(state.Key, out var result))
                    {
                        if (result.Result < 0) { Fail(state, result.Message); return; }
                        if (result.Result == 1)
                        {
                            Status = BackendName + " " + settings.Resolution + " | " + state.Targets.Width + "x" + state.Targets.Height +
                                     " -> " + state.Targets.Resolution.OutputWidth + "x" + state.Targets.Resolution.OutputHeight +
                                     (FsrSelected ? " | " + result.Message : " | requested " + (char)('A' + result.RequestedPreset - 1) +
                                     " | observed " + (char)result.ObservedPreset + " | runtime-log verified");
                            uint marker = FsrSelected ? 1u : result.ObservedPreset;
                            if (state.ReportedPreset != marker)
                            {
                                information(Status);
                                state.ReportedPreset = marker;
                            }
                        }
                    }
                    // Screenshot/save-thumbnail and another mod's explicit target remain
                    // wholly owned by their caller; never steal that render target for SR.
                    if (settings.Resolution != ResolutionMode.Dlaa && camera.targetTexture) return;
                    if (camera.pixelWidth <= 0 || camera.pixelHeight <= 0) return;
                    Matrix4x4 projection = camera.projectionMatrix;
                    if (!PrepareTargets(state, camera.pixelWidth, camera.pixelHeight)) return;
                    if (!state.Targets.Resolution.IsNative)
                    {
                        if (camera.rect != new Rect(0f, 0f, 1f, 1f))
                        {
                            state.Reset = true;
                            Status = "Original AA for a partial camera viewport; SR resumes at full viewport.";
                            return;
                        }
                        EnsurePresentation(state);
                        state.OriginalTarget = camera.targetTexture;
                        state.TargetOverridden = true;
                        state.SrThisRender = true;
                        camera.targetTexture = state.Targets.World;
                        // Preserve the native projection/aspect while rasterizing fewer pixels.
                        camera.projectionMatrix = projection;
                    }
                    int scene = GameCamera.sceneIndex;
                    int profile = behaviour.profile.GetInstanceID();
                    // Floating-origin jumps, discontinuous camera motion, projection/profile
                    // changes and gaps in rendering invalidate temporal history.
                    if (state.LastFrame + 1 != Time.frameCount || state.Scene != scene || state.Profile != profile ||
                        (state.Position - camera.transform.position).sqrMagnitude > 100f ||
                        Quaternion.Angle(state.Rotation, camera.transform.rotation) > 30f || state.Projection != projection)
                        state.Reset = true;
                    if (state.Reset) state.Phase = 0;
                    state.Jitter = Jitter.ForFrame(state.Phase++, state.Targets.Resolution.JitterPhases);
                    state.Position = camera.transform.position;
                    state.Rotation = camera.transform.rotation;
                    state.Projection = projection;
                    state.Scene = scene;
                    state.Profile = profile;
                    state.LastFrame = Time.frameCount;
                    if (FsrSelected)
                    {
                        state.OpaqueCapture = new CommandBuffer { name = "DSPAASR FSR opaque color" };
                        state.OpaqueCapture.Blit(BuiltinRenderTextureType.CameraTarget, state.Targets.OpaqueColor);
                        camera.AddCommandBuffer(CameraEvent.BeforeForwardAlpha, state.OpaqueCapture);
                    }
                    state.WorldText.Begin(camera, projection);
                    state.WorldTextFrame = Time.frameCount;
                }
                catch (Exception error)
                {
                    Restore(state);
                    state.SrThisRender = false;
                    if (state.Presenter) state.Presenter.enabled = false;
                    Fail(state, error.Message); return;
                }
            }
            state.Model = behaviour.profile.antialiasing;
            state.OriginalEnabled = state.Model.enabled;
            state.OriginalSettings = state.Model.settings;
            state.OriginalMsaa = camera.allowMSAA;
            state.OriginalTransparentJitter = camera.useJitteredProjectionMatrixForTransparentRendering;
            state.Overridden = true;
            var aa = state.OriginalSettings;
            aa.method = settings.Technique == AaTechnique.Fxaa ? AntialiasingModel.Method.Fxaa : AntialiasingModel.Method.Taa;
            state.Model.settings = aa;
            state.Model.enabled = true;
            if (settings.Temporal) camera.allowMSAA = false;
            state.Prepared = settings.Temporal;
            TraceStage("post.preCull-prepared", camera);
        }
        private bool PrepareTargets(CameraState state, int width, int height)
        {
            if (state.RequestedWidth != width || state.RequestedHeight != height || (state.Targets != null && !state.Targets.Valid))
            {
                Retire(state);
                state.Key = ++nextKey;
                state.RequestedWidth = width; state.RequestedHeight = height;
                state.ReportedPreset = 0;
                state.Reset = true;
            }
            if (state.Targets != null) return true;
            var size = new RenderResolution(width, height, width, height);
            if (settings.Resolution != ResolutionMode.Dlaa || FsrSelected)
            {
                if (!state.QueryIssued)
                {
                    state.SizingAnchor = new RenderTexture(2, 2, 0, RenderTextureFormat.ARGBHalf, RenderTextureReadWrite.Linear)
                    { name = "DSPAAMod sizing device anchor", hideFlags = HideFlags.HideAndDontSave };
                    if (!state.SizingAnchor.Create()) throw new InvalidOperationException("Cannot allocate reconstruction sizing anchor.");
                    IntPtr token = native.RequestOptimal(state.Key, state.SizingAnchor.GetNativeTexturePtr(), (uint)width, (uint)height,
                        (uint)settings.Resolution, FsrSelected ? 1u : 0u);
                    IssueControl(token);
                    state.QueryIssued = true;
                    Status = "Querying " + BackendName + " render size for " + settings.Resolution;
                    return false;
                }
                NativeOptimalSettings result;
                uint phases = 0;
                if (FsrSelected)
                {
                    if (!native.TryGetFsrOptimal(state.Key, out var fsr) || fsr.Settings.Result == 0) return false;
                    result = fsr.Settings; phases = fsr.JitterPhases;
                    if (result.Result == 1 && phases == 0) throw new InvalidOperationException("FSR returned no jitter sequence.");
                }
                else if (!native.TryGetOptimal(state.Key, out result) || result.Result == 0) return false;
                if (result.Result < 0) throw new InvalidOperationException(result.Message);
                if (result.OutputWidth != (uint)width || result.OutputHeight != (uint)height || result.Quality != (uint)settings.Resolution)
                    throw new InvalidOperationException("Stale reconstruction resolution response.");
                size = new RenderResolution(checked((int)result.OptimalWidth), checked((int)result.OptimalHeight), width, height, phases);
                DestroyAnchor(state.SizingAnchor); state.SizingAnchor = null;
            }
            state.Targets = new RenderTargets(size, FsrSelected);
            information(BackendName + " render targets: " + size.InputWidth + "x" + size.InputHeight + " world -> " + width + "x" + height +
                " output/UI, mode " + settings.Resolution + ", jitter phases " + size.JitterPhases);
            return true;
        }
        private static void EnsurePresentation(CameraState state)
        {
            if (!state.Presenter) state.Presenter = state.Camera.GetComponent<SrPresentation>();
            if (!state.Presenter) state.Presenter = state.Camera.gameObject.AddComponent<SrPresentation>();
            state.Presenter.enabled = true;
            bool afterStack = false, afterPresenter = false;
            foreach (var component in state.Camera.GetComponents<MonoBehaviour>())
            {
                if (!component || !component.isActiveAndEnabled) continue;
                if (component == state.Behaviour) afterStack = true;
                if (component == state.Presenter) { afterPresenter = true; continue; }
                if (!afterStack || !HasImageEffect(component.GetType())) continue;
                if (afterPresenter || (component != state.Behaviour && !(component is TranslucentImageSource) && !(component is UnityStandardAssets.ImageEffects.SunShafts)))
                    throw new NotSupportedException("Unintegrated image effect after the reconstruction stack: " + component.GetType().FullName);
            }
        }
        private static bool HasImageEffect(Type type)
        {
            // Absence is normal for camera components; Harmony's Method helper logs
            // misses. Keep its inherited/private-method lookup without per-frame warnings.
            var signature = new[] { typeof(RenderTexture), typeof(RenderTexture) };
            for (; type != null; type = type.BaseType)
                if (type.GetMethod("OnRenderImage", BindingFlags.DeclaredOnly | BindingFlags.Public |
                    BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.Static, null, signature, null) != null)
                    return true;
            return false;
        }
        private static void RestoreTarget(CameraState state)
        {
            if (!state.TargetOverridden) return;
            if (state.Camera && state.Targets != null && state.Camera.targetTexture == state.Targets.World)
                state.Camera.targetTexture = state.OriginalTarget;
            state.TargetOverridden = false;
        }
        private static void DestroyAnchor(RenderTexture anchor)
        {
            if (!anchor) return;
            anchor.Release(); UnityEngine.Object.Destroy(anchor);
        }

        public bool Projection(TaaComponent taa, Func<Vector2, Matrix4x4> custom)
        {
            TraceStage("taa.projection-entry", taa.context?.camera, taa.context);
            if (!TryPrepared(taa, out var state)) return true;
            Camera camera = state.Camera;
            var jitter = new Vector2(state.Jitter.X, state.Jitter.Y);
            Matrix4x4 projection = camera.projectionMatrix;
            camera.nonJitteredProjectionMatrix = projection;
            if (custom != null) projection = custom(jitter);
            else if (camera.orthographic)
            {
                projection.m03 -= 2f * jitter.x / state.Targets.Width;
                projection.m13 -= 2f * jitter.y / state.Targets.Height;
            }
            else
            {
                projection.m02 += 2f * jitter.x / state.Targets.Width;
                projection.m12 += 2f * jitter.y / state.Targets.Height;
            }
            camera.projectionMatrix = projection;
            state.AppliedProjection = projection;
            state.JitterApplied = true;
            camera.useJitteredProjectionMatrixForTransparentRendering = true;
            setJitter(taa, new Vector2(jitter.x / state.Targets.Width, jitter.y / state.Targets.Height));
            return false; // Do not apply the legacy eight-phase jitter a second time.
        }
        public bool Resolve(TaaComponent taa, RenderTexture source, RenderTexture destination)
        {
            TraceStage("taa.resolve-entry", taa.context?.camera, taa.context, source, destination);
            if (!TryPrepared(taa, out var state)) return true;
            IntPtr token = IntPtr.Zero;
            bool submitted = false;
            try
            {
                var targets = state.Targets;
                if (source.width != targets.Width || source.height != targets.Height)
                    throw new InvalidOperationException("World source " + source.width + "x" + source.height + " does not match NGX input " + targets.Width + "x" + targets.Height + ".");
                if (state.Frame == 0)
                    information(BackendName + " actual world source " + source.width + "x" + source.height + "; resolve destination " + destination.width + "x" + destination.height + ".");
                var ngxJitter = Jitter.ToNgx(state.Jitter);
                var frame = new NativeFrame
                {
                    Camera = state.Key, Frame = ++state.Frame,
                    Color = targets.ColorPointer, Output = targets.OutputPointer,
                    Depth = targets.DepthPointer, Motion = targets.MotionPointer,
                    Width = (uint)targets.Width, Height = (uint)targets.Height,
                    OutputWidth = (uint)targets.Resolution.OutputWidth, OutputHeight = (uint)targets.Resolution.OutputHeight,
                    Quality = (uint)settings.Resolution,
                    Preset = FsrSelected ? 0u : ModelPolicy.ResolvePreset(settings.Model, settings.Resolution),
                    Flags = (state.Camera.allowHDR ? 1u : 0u) | (SystemInfo.usesReversedZBuffer ? 2u : 0u) | (state.Reset ? 4u : 0u),
                    JitterX = ngxJitter.X, JitterY = ngxJitter.Y,
                    // Built-in RGHalf motion is current-minus-previous in UV units.
                    MotionScaleX = -targets.Width, MotionScaleY = -targets.Height,
                    FrameTimeMilliseconds = Mathf.Max(Time.unscaledDeltaTime, 0.000001f) * 1000f
                };
                if (FsrSelected)
                {
                    var parameters = new NativeFsrParameters {
                        CameraNear = state.Camera.nearClipPlane, CameraFar = state.Camera.farClipPlane,
                        VerticalFov = state.Camera.fieldOfView * Mathf.Deg2Rad,
                        PreExposure = 1f, ViewSpaceToMeters = 1f, Sharpness = settings.FsrSharpness,
                        OpaqueColor = targets.OpaquePointer
                    };
                    token = native.SubmitFsr(ref frame, ref parameters);
                }
                else token = native.Submit(ref frame);
                if (token == IntPtr.Zero) throw new InvalidOperationException("Native frame queue is unavailable/full.");
                using (var commands = new CommandBuffer { name = "DSPAASR " + BackendName })
                {
                    commands.Blit(source, targets.Color);
                    // String/name identifiers address CommandBuffer temporary RTs,
                    // not the camera's globally bound Shader textures.
                    if (!state.InputsCopied)
                    {
                        commands.Blit(BuiltinRenderTextureType.Depth, targets.Depth);
                        commands.Blit(BuiltinRenderTextureType.MotionVectors, targets.Motion);
                    }
                    commands.Blit(source, targets.Output); // Deterministic image fallback, never uninitialized output.
                    commands.IssuePluginEventAndData(native.RenderEvent, 1, token);
                    commands.Blit(targets.Output, destination);
                    Graphics.ExecuteCommandBuffer(commands);
                    submitted = true;
                }
                TraceStage("ngx.submitted", state.Camera, taa.context, source, destination);
                Capture?.EnqueueIfRequested(frame, targets, state.Camera, state.AppliedProjection, state.WorldText.PendingCount);
                state.Reset = false;
                return false; // NGX consumed raw color, NOT an already TAA-resolved image.
            }
            catch (Exception error)
            {
                if (!submitted) native.Cancel(token);
                Fail(state, error.Message);
                taa.ResetHistory();
                return true; // Managed submission failure can use original TAA this frame.
            }
        }
        public void AfterResolve(TaaComponent taa, RenderTexture destination)
        {
            if (taa.context == null || !taa.context.camera || !cameras.TryGetValue(taa.context.camera, out var state)) return;
            try
            {
                int count = state.WorldText.Draw(destination);
                if (count > 0 && !state.ReportedWorldText)
                {
                    information("World navigation text: " + count + " renderers after temporal reconstruction at " +
                        destination.width + "x" + destination.height + ", before game postprocessing.");
                    state.ReportedWorldText = true;
                }
            }
            catch (Exception error)
            {
                state.WorldText.End();
                Fail(state, "World navigation text draw failed: " + error.Message);
            }
        }
        private bool TryPrepared(TaaComponent taa, out CameraState state)
        {
            state = null;
            return taa.context != null && taa.context.camera && cameras.TryGetValue(taa.context.camera, out state) && state.Prepared && !state.Faulted;
        }
        private void Fail(CameraState state, string message)
        {
            state.Faulted = true;
            state.Prepared = false;
            Status = "Original AA fallback: " + message;
            report(Status);
        }
        public ImageScope BeginImage(Component effect, ref RenderTexture source, ref RenderTexture destination)
        {
            Camera camera = effect.GetComponent<Camera>();
            if (DiagnosticActive) TraceStage("image." + effect.GetType().Name, camera, null, source, destination);
            if (!camera || !cameras.TryGetValue(camera, out var state)) return null;
            bool temporalStack = effect == state.Behaviour;
            if (temporalStack) state.WorldText.RestoreVisibility();
            if (!state.SrThisRender || state.Targets == null) return null;
            if (!temporalStack && !state.ImageResult) return null; // An effect before temporal reconstruction.
            if (temporalStack)
            {
                // Copy actual low-resolution camera inputs before restoring the native
                // camera target/context dimensions for the downstream postprocessing.
                using (var commands = new CommandBuffer { name = "DSPAAMod SR camera inputs" })
                {
                    commands.Blit(BuiltinRenderTextureType.Depth, state.Targets.Depth);
                    commands.Blit(BuiltinRenderTextureType.MotionVectors, state.Targets.Motion);
                    Graphics.ExecuteCommandBuffer(commands);
                }
                state.InputsCopied = true;
                imageSources[source] = state;
                RestoreTarget(state);
            }
            var scope = new ImageScope { State = state, OriginalDestination = destination, Source = source, TemporalStack = temporalStack };
            if (!temporalStack) source = state.ImageResult;
            scope.Target = source == state.Targets.PostA ? state.Targets.PostB : state.Targets.PostA;
            destination = scope.Target;
            return scope;
        }
        public RenderTexture ResolveTarget(RenderTextureFactory factory, RenderTexture source)
        {
            if (!imageSources.TryGetValue(source, out var state) || state.Targets == null) return factory.Get(source);
            var size = state.Targets.Resolution;
            return factory.Get(size.OutputWidth, size.OutputHeight, source.depth, source.format,
                source.sRGB ? RenderTextureReadWrite.sRGB : RenderTextureReadWrite.Linear, source.filterMode, source.wrapMode);
        }
        public void EndImage(ImageScope scope, bool success)
        {
            if (scope == null) return;
            if (scope.TemporalStack) imageSources.Remove(scope.Source);
            if (!success) { scope.State.ImageResult = null; return; }
            scope.State.ImageResult = scope.Target;
            // Honor Unity's original image-effect chain destinations while retaining
            // a separate native-size chain; later registered effects consume the latter.
            if (scope.Target != scope.OriginalDestination) Graphics.Blit(scope.Target, scope.OriginalDestination);
        }
        public bool Present(Camera camera, RenderTexture source, RenderTexture destination)
        {
            if (!camera || !cameras.TryGetValue(camera, out var state) || !state.SrThisRender) return false;
            RestoreTarget(state);
            RenderTexture final = state.ImageResult ? state.ImageResult : source;
            if (final != destination) Graphics.Blit(final, destination);
            // Graphics.Blit(null) uses Camera.main.targetTexture. That target has
            // already been restored; the world RT must never absorb the screen blit.
            if (final != state.OriginalTarget && destination != state.OriginalTarget) Graphics.Blit(final, state.OriginalTarget);
            state.SrThisRender = false;
            state.ImageResult = null;
            return true;
        }

        public void AfterImage(PostProcessingBehaviour behaviour, RenderTexture destination, bool success)
        {
            Camera camera = behaviour.GetComponent<Camera>();
            TraceStage("post.image-complete", camera);
            if (!camera || !cameras.TryGetValue(camera, out var state)) return;
            try
            {
                if (success && state.WorldText.PendingCount > 0)
                {
                    // A changed/interrupted stack did not invoke temporal resolve.
                    // Preserve visibility this frame, then use the original path.
                    state.WorldText.Draw(destination);
                    Fail(state, "Temporal resolve was skipped; navigation text restored after the stack for this fallback frame.");
                }
            }
            finally { Restore(state); }
        }
        private static void Restore(CameraState state)
        {
            try { state.WorldText.End(); }
            finally
            {
                RestoreTarget(state);
                if (state.OpaqueCapture != null)
                {
                    if (state.Camera) state.Camera.RemoveCommandBuffer(CameraEvent.BeforeForwardAlpha, state.OpaqueCapture);
                    state.OpaqueCapture.Release(); state.OpaqueCapture = null;
                }
                if (state.Overridden)
                {
                    if (state.Model != null)
                    {
                        state.Model.settings = state.OriginalSettings;
                        state.Model.enabled = state.OriginalEnabled;
                    }
                    if (state.Camera)
                    {
                        state.Camera.allowMSAA = state.OriginalMsaa;
                        state.Camera.useJitteredProjectionMatrixForTransparentRendering = state.OriginalTransparentJitter;
                        if (state.JitterApplied && state.Camera.projectionMatrix == state.AppliedProjection) state.Camera.projectionMatrix = state.Projection;
                    }
                    state.JitterApplied = false;
                    state.Overridden = false;
                }
            }
        }
        public void Reset(PostProcessingBehaviour behaviour)
        {
            Camera camera = behaviour.GetComponent<Camera>();
            if (camera && cameras.TryGetValue(camera, out var state)) state.Reset = true;
        }
        public void Remove(PostProcessingBehaviour behaviour)
        {
            Camera camera = behaviour.GetComponent<Camera>();
            if (camera && cameras.TryGetValue(camera, out var state))
            {
                Restore(state);
                Retire(state);
                cameras.Remove(camera);
            }
        }
        private void Retire(CameraState state)
        {
            if (state.Presenter) state.Presenter.enabled = false;
            if (state.Targets == null && !state.QueryIssued && !state.SizingAnchor) return;
            Capture?.Cancel(state.Targets, "Camera render targets retired.");
            var old = new CameraState { Key = state.Key, Targets = state.Targets, SizingAnchor = state.SizingAnchor };
            state.Targets = null; state.SizingAnchor = null; state.QueryIssued = false;
            retired.Add(old);
            // Keep Unity textures alive until the render thread acknowledges that
            // every previously submitted user of this camera has drained.
            IssueControl(native.Release(old.Key));
        }
        private void IssueControl(IntPtr token)
        {
            if (token == IntPtr.Zero) throw new InvalidOperationException("Cannot queue native resource retirement.");
            try
            {
                using (var commands = new CommandBuffer { name = "DSPAAMod lifecycle" })
                {
                    commands.IssuePluginEventAndData(native.RenderEvent, 1, token);
                    Graphics.ExecuteCommandBuffer(commands);
                }
            }
            catch { native.Cancel(token); throw; }
        }
        public void Update()
        {
            for (int i = 0; i < supportQueries.Length; ++i)
                if (supportRequested || (settings.Temporal && i == (FsrSelected ? 1 : 0))) UpdateSupport(supportQueries[i]);
            if (diagnosticThrough >= 0 && Time.frameCount > diagnosticThrough) FinishDiagnostic();
            Camera dead = null;
            bool found = false;
            foreach (var pair in cameras)
            {
                if (pair.Value.WorldTextFrame != Time.frameCount) pair.Value.WorldText.End();
                if (!pair.Key || !pair.Value.Behaviour) { dead = pair.Key; found = true; break; }
            }
            if (found)
            {
                Restore(cameras[dead]);
                Retire(cameras[dead]);
                cameras.Remove(dead);
            }
            for (int i = retired.Count - 1; i >= 0; --i)
            {
                if (retired[i].Targets != null && Capture != null && Capture.IsBusy(retired[i].Targets)) continue;
                if (native.TryGetStatus(retired[i].Key, out var status) && status.Result == 2)
                {
                    retired[i].Targets?.Dispose();
                    DestroyAnchor(retired[i].SizingAnchor);
                    retired.RemoveAt(i);
                }
            }
        }
        public void Shutdown()
        {
            FinishDiagnostic();
            Capture?.Cancel("Renderer shutdown.");
            foreach (var state in cameras.Values) { Restore(state); Retire(state); }
            cameras.Clear();
            if (native != null) IssueControl(native.Shutdown());
            // On application quit Unity may stop issuing events. Do not destroy
            // pending RTs or unload code underneath the renderer; process teardown
            // reclaims them. Hot unloading this prototype is intentionally unsupported.
        }
    }
}
