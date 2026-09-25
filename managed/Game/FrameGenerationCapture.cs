using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.InteropServices;
using DSPAAMod.Interop;
using HarmonyLib;
using UnityEngine;
using UnityEngine.Rendering;

namespace DSPAAMod.Game
{
    // Game-side command ordering and allocation identity. Effect semantics and
    // world-UI suppression are separate adapters, not inferred from an RT name.
    internal sealed class FrameGenerationCapture : IDisposable
    {
        private sealed class Allocation
        {
            public NativeCaptureTexture Native;
            public bool OwnedReference;
        }
        private sealed class CameraCommands
        {
            public Camera Camera;
            public ulong Frame;
            public readonly List<CommandBuffer> Buffers = new List<CommandBuffer>();
            public readonly List<CameraEvent> Events = new List<CameraEvent>();
            public readonly List<IntPtr> Tokens = new List<IntPtr>();
        }
        private readonly FrameGenerationController presentation;
        private readonly CaptureBridge bridge;
        private readonly Action<string> log, warning;
        private readonly bool traceOnly = Environment.GetEnvironmentVariable("DSPAASR_CAPTURE_TRACE_ONLY") == "1";
        public WorldUiCapture WorldUi { get; private set; }
        private readonly Dictionary<RenderTexture, Allocation> allocations = new Dictionary<RenderTexture, Allocation>();
        private readonly List<CameraCommands> cameras = new List<CameraCommands>();
        private readonly List<CameraCommands> retiredCameras = new List<CameraCommands>();
        private readonly Dictionary<Camera, ulong> tracedCameras = new Dictionary<Camera, ulong>();
        private readonly HashSet<Component> tracedImageRoots = new HashSet<Component>();
        private int imageTraceCount;
        private static readonly FieldInfo normalCanvas = AccessTools.Field(typeof(UIRoot), "overlayCanvas");
        private static readonly FieldInfo topCanvas = AccessTools.Field(typeof(UIRoot), "overlayCanvasTop");
        private ulong nextEpoch;
        private int traceScene = int.MinValue;
        private bool traceNullGame;
        private bool closed, failed, worldExcluded, resolved;
        private int scopes;
        private string lastReason;
        public bool Ready { get; }
        public bool WantsFrame => !traceOnly && Ready && !closed && !failed && presentation.ApplicationFrameId != 0 &&
            presentation.Settings.Backend != DSPAAMod.Core.FrameGenerationBackend.Off;
        public bool Active => WantsFrame && presentation.Capturing;
        public NativeCaptureStatus Status { get; private set; }
        public FrameGenerationCapture(FrameGenerationController owner, NativeBridge library, IntPtr renderEvent,
            Action<string> information, Action<string> warn)
        {
            presentation = owner; log = information; warning = warn;
            bridge = new CaptureBridge(library, renderEvent);
            if (!bridge.TryGetStatus(out var status)) throw new InvalidOperationException("Cannot query the ordered capture owner.");
            Status = status; Ready = status.Ready;
            if (!Ready) { warning(status.Reason); return; }
            WorldUi = new WorldUiCapture(presentation, this, warning);
            Camera.onPreCull += BeforeCamera;
        }
        private static Camera CanvasCamera(FieldInfo field)
        {
            var root = UIRoot.instance;
            return root && field?.GetValue(root) is Canvas canvas ? canvas.worldCamera : null;
        }
        public bool IsMain(Camera camera) => camera && camera == GameCamera.main;
        public bool IsScreenUi(Camera camera) => camera && (camera == CanvasCamera(normalCanvas) || camera == CanvasCamera(topCanvas));
        public bool Owns(Component component)
        {
            if (!Active || !component) return false;
            var camera = component.GetComponent<Camera>();
            return IsMain(camera) || IsScreenUi(camera);
        }
        private NativeCaptureCommand Command(CaptureOperation operation, ulong pass = 0) => new NativeCaptureCommand {
            Operation = operation, ApplicationFrameId = presentation.ApplicationFrameId, Generation = presentation.Generation,
            PassId = pass, OcclusionSlot = -1, OcclusionDomain = NativeCaptureDomain.Identity
        };
        private IntPtr Record(CommandBuffer commands, NativeCaptureCommand command, NativeCaptureInput[] inputs = null,
            NativeCaptureDomain[] domains = null, string basis = "")
        {
            IntPtr token = bridge.Queue(command, inputs, domains, basis);
            if (token == IntPtr.Zero) throw new InvalidOperationException("Capture command queue rejected its frame or allocation contract.");
            try { commands.IssuePluginEventAndData(bridge.RenderEvent, 3, token); return token; }
            catch { bridge.Cancel(token); throw; }
        }
        private void Execute(NativeCaptureCommand command, NativeCaptureInput[] inputs = null,
            NativeCaptureDomain[] domains = null, string basis = "")
        {
            IntPtr token = IntPtr.Zero;
            using (var commands = new CommandBuffer { name = "DSPAASR ordered capture command" })
            {
                try { token = Record(commands, command, inputs, domains, basis); Graphics.ExecuteCommandBuffer(commands); }
                catch { if (token != IntPtr.Zero) bridge.Cancel(token); throw; }
            }
        }
        // Called at an actual pool checkout, not from a texture-pointer heuristic.
        // Explicit checkout notifications supersede any first-use observation.
        public void Acquired(RenderTexture texture)
        {
            if (!Active || !texture) return;
            Forget(texture, true);
            var entry = new Allocation { Native = new NativeCaptureTexture { Epoch = ++nextEpoch } };
            if (entry.Native.Epoch == 0) throw new InvalidOperationException("Capture checkout epoch wrapped.");
            allocations.Add(texture, entry);
            // Unity creates hardware storage at first use. A logical checkout
            // may precede that allocation; do not create it early or guess shadow.
            if (texture.IsCreated()) Materialize(texture, entry, false);
        }
        private Allocation AllocationFor(RenderTexture texture)
        {
            if (!allocations.TryGetValue(texture, out var entry)) { Acquired(texture); allocations.TryGetValue(texture, out entry); }
            if (entry == null) throw new InvalidOperationException("No active capture allocation for this texture.");
            return entry;
        }
        private void Materialize(RenderTexture texture, Allocation entry, bool required)
        {
            if (entry.Native.Resource != IntPtr.Zero) return;
            var pointer = texture.GetNativeTexturePtr();
            if (pointer == IntPtr.Zero) {
                if (required) throw new InvalidOperationException("Required color input has no native resource: " + texture.name +
                    " created=" + texture.IsCreated() + " format=" + texture.format);
                return;
            }
            Marshal.AddRef(pointer); entry.Native.Resource = pointer; entry.OwnedReference = true;
            var command = Command(CaptureOperation.Declare); command.Source = entry.Native; Execute(command);
        }
        public ulong OutputEpoch(RenderTexture texture)
        {
            var entry = AllocationFor(texture);
            if (texture.IsCreated()) Materialize(texture, entry, false);
            return entry.Native.Epoch;
        }
        public NativeCaptureTexture Texture(RenderTexture texture)
        {
            if (!texture) return default; // Only this explicit null contract denotes the facade shadow.
            var entry = AllocationFor(texture); Materialize(texture, entry, true); return entry.Native;
        }
        public void Released(RenderTexture texture)
        {
            if (!texture) return;
            Forget(texture, Active);
        }
        private void Forget(RenderTexture texture, bool invalidate)
        {
            if (!allocations.TryGetValue(texture, out var entry)) return;
            try {
                if (invalidate) {
                    var native = entry.Native;
                    // A formerly lazy target may only have been seen by the
                    // actual draw observer. Retire that exact checkout, not shadow.
                    if (native.Resource == IntPtr.Zero && texture && texture.IsCreated()) native.Resource = texture.GetNativeTexturePtr();
                    if (native.Resource != IntPtr.Zero) {
                        var command = Command(CaptureOperation.Invalidate); command.Source = native; Execute(command);
                    }
                }
            }
            finally { if (entry.OwnedReference) Marshal.Release(entry.Native.Resource); allocations.Remove(texture); }
        }
        public void BeginPass(CaptureScopeKind kind, ulong passId, string basis, bool fullOverwrite,
            NativeCaptureInput[] inputs = null, NativeCaptureDomain[] domains = null, int occlusionSlot = -1,
            NativeCaptureDomain? occlusionDomain = null, bool implicitOutput = false, ulong outputEpoch = 0)
        {
            if (!Active) return;
            if (scopes != 0) throw new InvalidOperationException("Nested GPU capture pass annotations are unsupported.");
            var command = Command(CaptureOperation.BeginScope, passId); command.Scope = kind;
            command.Flags = fullOverwrite ? 2u : 0u; command.OcclusionSlot = occlusionSlot;
            command.OcclusionDomain = occlusionDomain ?? NativeCaptureDomain.Identity;
            if (implicitOutput) {
                if (kind != CaptureScopeKind.DualColor || !fullOverwrite)
                    throw new InvalidOperationException("An implicit output needs a trusted whole-target color draw.");
                command.Flags |= 8u; command.Source.Epoch = outputEpoch != 0 ? outputEpoch : ++nextEpoch;
                if (command.Source.Epoch == 0) throw new InvalidOperationException("Capture output epoch wrapped.");
            }
            Execute(command, inputs, domains, basis); scopes = 1;
        }
        public void EndPass()
        {
            if (!Active || scopes == 0) return;
            Execute(Command(CaptureOperation.EndScope)); scopes = 0;
        }
        public void Remember(RenderTexture texture)
        {
            if (!Active) return;
            // The actual scoped draw owns the value epoch, even when Unity's
            // CameraTarget has no managed Texture object. A concrete destination
            // is only an identity cross-check, not a new checkout/invalidation.
            var command = Command(CaptureOperation.Remember); command.Flags = 8;
            if (texture) {
                command.Source.Resource = texture.GetNativeTexturePtr();
                if (command.Source.Resource == IntPtr.Zero) throw new InvalidOperationException("Explicit output has no native resource.");
            }
            Execute(command);
        }
        public void FollowColorTransfers()
        {
            if (!Active || !resolved) return;
            if (scopes != 0) throw new InvalidOperationException("A camera color tail cannot replace an explicit capture pass.");
            var command = Command(CaptureOperation.BeginTransfers, FrameGenerationPasses.Copy);
            command.Scope = CaptureScopeKind.DualColor; command.Flags = 10; command.Source.Epoch = ++nextEpoch;
            if (command.Source.Epoch == 0) throw new InvalidOperationException("Camera color-transfer epoch wrapped.");
            Execute(command, basis: "Owned image-effect tail: actual pinned whole-output Unity Copy only");
        }
        public void SeedResolved(RenderTexture source, bool allWorldUiExcluded)
        {
            if (!Active || resolved) return;
            worldExcluded = allWorldUiExcluded;
            if (!worldExcluded) { Unsupported("World-space UI has not been excluded from this resolved scene"); return; }
            var command = Command(CaptureOperation.Seed); command.Source = Texture(source); Execute(command); resolved = true;
        }
        public void CopyWorldDepth(IntPtr destinationDepth, ulong allocationEpoch)
        {
            // Redrawing already-suppressed original UI is independent of whether
            // this frame remains eligible for FG. Do not hide it on a capture fallback.
            if (!Ready || closed || !presentation.Capturing) return;
            if (!presentation.World.InputsReady || destinationDepth == IntPtr.Zero || allocationEpoch == 0)
                throw new InvalidOperationException("World UI depth attachment lacks this frame's captured depth.");
            var command = Command(CaptureOperation.DepthCopy);
            command.Source = presentation.World.DepthSource;
            command.Previous = new NativeCaptureTexture { Resource = destinationDepth, Epoch = allocationEpoch };
            command.OcclusionDomain.BiasX = presentation.Inputs.JitterX / presentation.Inputs.RenderWidth;
            command.OcclusionDomain.BiasY = presentation.Inputs.JitterY / presentation.Inputs.RenderHeight;
            Execute(command);
        }
        public void Handoff(RenderTexture source)
        {
            if (!Active) return;
            var command = Command(CaptureOperation.Handoff); command.Source = Texture(source); command.Flags = 4;
            Execute(command);
        }
        public void Unsupported(string reason)
        {
            if (failed || closed) return;
            try { if (Active) Execute(Command(CaptureOperation.Abort), basis: reason); }
            finally { failed = true; if (lastReason != reason) { lastReason = reason; warning("FG input capture: " + reason); } }
        }
        public void BeginInput()
        {
            if (Ready && bridge.TryGetStatus(out var status)) Status = status;
            WorldUi?.BeginInput();
            ClearCameraCommands();
            tracedImageRoots.Clear();
            foreach (var entry in allocations.Values) if (entry.OwnedReference) Marshal.Release(entry.Native.Resource);
            allocations.Clear(); failed = false; resolved = false; worldExcluded = false; scopes = 0;
            int scene = GameCamera.sceneIndex; bool nullGame = GameMain.isNull;
            if (scene != traceScene || nullGame != traceNullGame) {
                tracedCameras.Clear(); traceScene = scene; traceNullGame = nullGame;
            }
        }
        public void FinishFrame()
        {
            WorldUi?.EndFrame();
            // Native sealing independently verifies every plane and allocation.
            // A root containing UI can never be promoted merely because it rendered.
            if (Active && resolved && worldExcluded && scopes == 0 && presentation.World.InputsReady)
                presentation.Inputs.Flags |= 1u;
            else presentation.Inputs.Flags &= ~1u;
        }
        public void Update()
        {
            if (closed || !Ready || !bridge.TryGetStatus(out var status)) return;
            Status = status;
        }
        public bool RequestTrace()
        {
            if (!traceOnly || !Ready || closed) return false;
            tracedCameras.Clear();
            log("Capture camera diagnostic requested; bounded BeforeAlpha comparison on the next rendered cameras.");
            return true;
        }
        internal void TraceImageInput(Component component, RenderTexture source, RenderTexture destination)
        {
            if (!traceOnly || !Ready || closed || !component || imageTraceCount >= 64) return;
            try {
                var camera = component.GetComponent<Camera>();
                bool observed = false;
                foreach (var entry in cameras) if (entry.Camera == camera) { observed = true; break; }
                if (!observed || !tracedImageRoots.Add(component)) return;
                ++imageTraceCount;
                string Describe(RenderTexture texture) => texture ?
                    "0x" + texture.GetNativeTexturePtr().ToInt64().ToString("X16") + " " + texture.width + "x" + texture.height +
                    " format=" + texture.format + " sRGB=" + texture.sRGB + " depthBits=" + texture.depth +
                    " depthPtr=0x" + texture.GetNativeDepthBufferPtr().ToInt64().ToString("X16") : "display";
                log("Capture image-input frame=" + presentation.ApplicationFrameId + " generation=" + presentation.Generation +
                    " camera=" + camera.name + " effect=" + component.GetType().FullName +
                    " source=" + Describe(source) + " destination=" + Describe(destination) +
                    " worldOwned=" + presentation.World.OwnsFrameCamera(camera) + " worldInputs=" + presentation.World.InputsReady +
                    " render=" + presentation.Inputs.RenderWidth + "x" + presentation.Inputs.RenderHeight +
                    " jitter=" + presentation.Inputs.JitterX + "," + presentation.Inputs.JitterY +
                    " motionScale=" + presentation.Inputs.MotionScaleX + "," + presentation.Inputs.MotionScaleY);
            } catch (Exception error) { warning("Camera image-input diagnostic unavailable: " + error.Message); }
        }
        private void TraceCanvas(FieldInfo field, string role)
        {
            var root = UIRoot.instance;
            var canvas = root ? field?.GetValue(root) as Canvas : null;
            if (!canvas) { log("Capture canvas role=" + role + " unavailable"); return; }
            var camera = canvas.worldCamera;
            log("Capture canvas role=" + role + " name=" + canvas.name + " mode=" + canvas.renderMode +
                " active=" + canvas.isActiveAndEnabled + " root=" + canvas.isRootCanvas +
                " camera=" + (camera ? camera.name : "none") + " cameraEnabled=" + (camera && camera.isActiveAndEnabled) +
                " cameraDepth=" + (camera ? camera.depth.ToString() : "none"));
        }
        private void Attach(CameraCommands entry, CameraEvent phase, string name, params NativeCaptureCommand[] commands)
        {
            var buffer = new CommandBuffer { name = name };
            entry.Buffers.Add(buffer); entry.Events.Add(phase);
            foreach (var command in commands) entry.Tokens.Add(Record(buffer, command));
            entry.Camera.AddCommandBuffer(phase, buffer);
        }
        private void BeforeCamera(Camera camera)
        {
            if (!Ready || closed || failed || !presentation.Capturing || !camera) return;
            bool owned = IsMain(camera) || IsScreenUi(camera);
            if (!owned && (!traceOnly || camera.targetTexture || camera.targetDisplay != 0)) return;
            if (!traceOnly && IsMain(camera)) WorldUi?.BeforeCull(camera);
            if (failed || !presentation.Capturing) return;
            bool trace = !tracedCameras.TryGetValue(camera, out var generation) || generation != presentation.Generation;
            if (traceOnly && !trace) return;
            var entry = new CameraCommands { Camera = camera, Frame = presentation.ApplicationFrameId };
            cameras.Add(entry);
            try {
                ulong role = IsMain(camera) ? 10ul : camera == CanvasCamera(normalCanvas) ? 20ul :
                    camera == CanvasCamera(topCanvas) ? 30ul : 40ul;
                if (!traceOnly) {
                    if (IsScreenUi(camera)) {
                        // allowMSAA is permission, not the actual sample count.
                        // Native capture checks the real resource and raster state.
                        var target = camera.targetTexture; int display = camera.targetDisplay;
                        var clear = camera.clearFlags; var rect = camera.rect;
                        // DSP slightly oversizes the normalized UI rect; Unity
                        // still reports an exact full-display pixel viewport.
                        // Test that raster domain, without an arbitrary epsilon.
                        if (target || display != 0 || clear != CameraClearFlags.Depth ||
                            camera.pixelRect != new Rect(0,0,Screen.width,Screen.height))
                            throw new InvalidOperationException("Screen UI camera contract changed: target=" +
                                (target ? target.name : "display") + " display=" + display + " clear=" + clear +
                                " rect=" + rect.ToString("R", System.Globalization.CultureInfo.InvariantCulture) +
                                " pixelRect=" + camera.pixelRect.ToString("R", System.Globalization.CultureInfo.InvariantCulture) +
                                " cameraPixels=" + camera.pixelWidth + "x" + camera.pixelHeight +
                                " screen=" + Screen.width + "x" + Screen.height);
                        // The actual HDR RTV is already bound at BeforeForwardOpaque
                        // in this Player (menu and world traces). Include opaque AND
                        // transparent UI; do not reconstruct this HDR value from LDR.
                        var handoff = Command(CaptureOperation.Handoff); handoff.Flags = 21; handoff.Source.Epoch = ++nextEpoch;
                        if (handoff.Source.Epoch == 0) throw new InvalidOperationException("UI camera epoch wrapped.");
                        var begin = Command(CaptureOperation.BeginScope, role * 10 + 4); begin.Scope = CaptureScopeKind.FullOnlyUiCoverage;
                        Attach(entry, CameraEvent.BeforeForwardOpaque, "DSPAASR screen UI begin", handoff, begin);
                        Attach(entry, CameraEvent.BeforeImageEffects, "DSPAASR screen UI end", Command(CaptureOperation.EndScope));
                    }
                    // Unity may still copy this camera's image after AfterEverything.
                    // Keep the shader-verified Copy tail until the next explicit
                    // handoff/scope or the real end-of-frame event closes it.
                }
                if (!trace) return;
                if (IsMain(camera)) { TraceCanvas(normalCanvas, "normal"); TraceCanvas(topCanvas, "top"); }
                var events = new[] { CameraEvent.BeforeForwardOpaque, CameraEvent.BeforeForwardAlpha,
                    CameraEvent.BeforeImageEffects, CameraEvent.AfterEverything };
                for (int i = 0; i < events.Length; ++i) {
                    var command = Command(CaptureOperation.Trace, role * 10 + (uint)i); command.Flags = traceOnly && (i == 1 || i == 3) ? 3u : 1u;
                    command.Source.Epoch = ++nextEpoch;
                    Attach(entry, events[i], "DSPAASR camera binding trace", command);
                }
                tracedCameras[camera] = presentation.Generation;
                log("Capture camera role=" + role + " name=" + camera.name + " depth=" + camera.depth +
                    " target=" + (camera.targetTexture ? camera.targetTexture.name : "display") +
                    " hdr=" + camera.allowHDR + " clear=" + camera.clearFlags + " allowMSAA=" + camera.allowMSAA +
                    " qualityMSAA=" + QualitySettings.antiAliasing + " rect=" + camera.rect.ToString("R", System.Globalization.CultureInfo.InvariantCulture) +
                    " pixelRect=" + camera.pixelRect.ToString("R", System.Globalization.CultureInfo.InvariantCulture) +
                    " scene=" + traceScene + " nullGame=" + traceNullGame);
            } catch (Exception error) { Unsupported(error.Message); }
        }
        private void ClearCameraCommands()
        {
            foreach (var entry in cameras) {
                for (int i = 0; i < entry.Buffers.Count; ++i) {
                    if (entry.Camera) entry.Camera.RemoveCommandBuffer(entry.Events[i], entry.Buffers[i]);
                    entry.Buffers[i].Dispose();
                }
                entry.Camera = null; entry.Buffers.Clear(); entry.Events.Clear();
                retiredCameras.Add(entry);
            }
            cameras.Clear();
            // Removing next frame's camera registration does not prove that the
            // render thread consumed last frame's plugin events. Cancel leftovers
            // only after that frame's ordered EOF fence has actually completed.
            for (int i = retiredCameras.Count - 1; i >= 0; --i) {
                var entry = retiredCameras[i];
                if (entry.Frame > Status.CompletedUnityFrame) continue;
                foreach (var token in entry.Tokens) bridge.Cancel(token);
                retiredCameras.RemoveAt(i);
            }
        }
        public void Dispose()
        {
            if (closed) return; closed = true;
            Camera.onPreCull -= BeforeCamera; ClearCameraCommands();
            // Pending event IDs still belong to the native broker: it consumes
            // them in render order or clears them on channel detach. Do not cancel
            // an unobserved in-flight callback merely because C# is disposing.
            retiredCameras.Clear();
            WorldUi?.Dispose(); WorldUi = null;
            foreach (var entry in allocations.Values) if (entry.OwnedReference) Marshal.Release(entry.Native.Resource);
            allocations.Clear();
        }
    }
}
