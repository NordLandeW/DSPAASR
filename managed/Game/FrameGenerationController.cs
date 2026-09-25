using System;
using System.Collections;
using DSPAAMod.Core;
using DSPAAMod.Interop;
using UnityEngine;
using UnityEngine.Rendering;

namespace DSPAAMod.Game
{
    internal sealed class FrameGenerationController : IDisposable
    {
        private readonly Plugin owner;
        private readonly PresentationBridge bridge;
        private readonly Action<string> information, warning;
        public FrameGenerationWorld World { get; private set; }
        private PresentationLoop loop;
        private Coroutine endOfFrame;
        private NativePresentationBegin frame;
        private NativePresentationInputs inputs = NativePresentationInputs.Create();
        private bool began, renderStarted, closed, faulted, navigationFailed;
        private float nextStatus;
        private uint lastBackend = uint.MaxValue;
        private string lastReason, lastNavigationFailure;
        public NativePresentationStatus Status { get; private set; }
        public FrameGenerationSettings Settings { get; private set; }
        public ulong ApplicationFrameId => began ? frame.ApplicationFrameId : 0;
        public ulong Generation => began ? frame.Generation : 0;
        public bool TemporalRequested => !closed && !faulted && Settings.Backend != FrameGenerationBackend.Off;
        public bool Capturing => began && renderStarted && TemporalRequested;
        internal bool SharesMotionRequest(Camera camera) => owner.Renderer?.NeedsMotionVectors(camera) == true;
        internal bool CanPrepareNavigation(Camera camera) => began && TemporalRequested && !navigationFailed &&
            World != null && FrameGenerationWorld.AcceptsCamera(camera);
        internal void PrepareNavigation(Camera camera, Matrix4x4 projection)
        {
            if (!CanPrepareNavigation(camera)) return;
            try { owner.Renderer?.PrepareFrameGenerationNavigation(camera, projection); }
            catch (Exception error) { NavigationFailure(error); }
        }
        internal void BeginNavigation(Camera camera, Matrix4x4 projection)
        {
            if (navigationFailed) return;
            try { owner.Renderer?.BeginFrameGenerationNavigation(camera, projection); }
            catch (Exception error) { NavigationFailure(error); }
        }
        internal void NavigationProjection(Camera camera, Matrix4x4 projection)
        {
            if (navigationFailed) return;
            try { owner.Renderer?.SetFrameGenerationNavigationProjection(camera, projection); }
            catch (Exception error) { NavigationFailure(error); }
        }
        internal void NavigationBefore(Camera camera)
        {
            if (navigationFailed) return;
            try { owner.Renderer?.DrawFrameGenerationNavigationBefore(camera); }
            catch (Exception error) { NavigationFailure(error); }
        }
        internal void EndNavigation() => owner.Renderer?.EndFrameGenerationNavigation();
        private void NavigationFailure(Exception error)
        {
            navigationFailed = true;
            inputs.Flags &= ~1u;
            string message = error.Message;
            try { EndNavigation(); }
            catch (Exception restore) { message += "; restoration: " + restore.Message; }
            if (lastNavigationFailure != message) {
                lastNavigationFailure = message;
                warning("FG navigation unavailable; presenting the original frame: " + message);
            }
        }
        // Exact frame snapshot, not Time.frameCount. The game adapter updates only
        // this envelope; the original Final is obtained by the native facade.
        public ref NativePresentationInputs Inputs => ref inputs;
        internal IntPtr RenderEvent => bridge.RenderEvent;
        public FrameGenerationController(Plugin plugin, NativeBridge library, FrameGenerationSettings settings,
            Action<string> log, Action<string> warn)
        {
            owner = plugin; information = log; warning = warn; Settings = settings;
            bridge = new PresentationBridge(library);
            if (!bridge.TryGetStatus(out var status)) throw new InvalidOperationException("Cannot query presentation bootstrap.");
            Status = status;
            if (!status.Available) { information(status.Message); return; }
            try {
                if (!Apply(settings)) throw new InvalidOperationException("Native presentation rejected configured settings.");
                loop = new PresentationLoop(BeforeInput, BeforeRender);
                World = new FrameGenerationWorld(this, warning);
                endOfFrame = owner.StartCoroutine(FinalizeFrames());
                information("Presentation frame IDs: before-input PlayerLoop -> render event -> end-of-frame envelope -> actual native Present.");
            } catch { World?.Dispose(); loop?.Dispose(); throw; }
        }
        public bool Apply(FrameGenerationSettings value)
        {
            Settings = value;
            if (!Status.Available) return value.Backend == FrameGenerationBackend.Off;
            var configuration = new NativePresentationConfiguration {
                Backend = (uint)value.Backend, Mode = (uint)value.Mode, GeneratedFrames = value.GeneratedFrames,
                Reflex = (uint)value.Reflex, DynamicTargetFrameRate = value.DynamicTargetFrameRate, FrameLimitMicroseconds = value.FrameLimitMicroseconds
            };
            return bridge.Configure(configuration);
        }
        private void BeforeInput()
        {
            try { World?.EndFrame(); }
            catch (Exception error) { Fail(error); }
            began = false; renderStarted = false; navigationFailed = false;
            if (closed || faulted) return;
            try {
                // This call performs Reflex Sleep before engine input sampling.
                // Neither cameras nor Present allocate an application ID.
                began = bridge.Begin(out frame);
                inputs.ApplicationFrameId = frame.ApplicationFrameId; inputs.Generation = frame.Generation;
                inputs.Flags = 2u; // A missing/paused/incomplete world never reuses previous temporal inputs.
                inputs.Hudless = inputs.Depth = inputs.Motion = IntPtr.Zero;
                inputs.FsrDistortion = inputs.SlDistortion = IntPtr.Zero;
                inputs.RenderWidth = inputs.RenderHeight = 0;
                inputs.LogicalOutputWidth = inputs.LogicalOutputHeight = 0;
                Array.Clear(inputs.GenerationRect, 0, inputs.GenerationRect.Length);
            } catch (Exception error) { Fail(error); }
        }
        private void BeforeRender()
        {
            if (!began || closed || faulted || renderStarted) return;
            try {
                // In Native/FSR mode there is deliberately no SL ticket. Only an
                // active DLSS owner emits these markers through the same ID.
                bridge.SimulationEnd(frame.ApplicationFrameId);
                Issue(1, new IntPtr(unchecked((long)frame.ApplicationFrameId)));
                renderStarted = true;
            } catch (Exception error) { Fail(error); }
        }
        private IEnumerator FinalizeFrames()
        {
            var end = new WaitForEndOfFrame();
            while (!closed)
            {
                yield return end; // All Cameras AND GUI, including late gizmos, before screen presentation.
                if (!began || !renderStarted || faulted || closed) continue;
                IntPtr token = IntPtr.Zero;
                try {
                    if (!navigationFailed) {
                        try { owner.Renderer?.FinishFrameGenerationNavigation(); }
                        catch (Exception error) { NavigationFailure(error); }
                    }
                    if (World?.InputsReady == true && !navigationFailed) inputs.Flags |= 1u;
                    else inputs.Flags &= ~1u;
                    token = bridge.Queue(ref inputs);
                    if (token == IntPtr.Zero) throw new InvalidOperationException("Presentation end-of-frame queue is unavailable/full.");
                    Issue(2, token); token = IntPtr.Zero;
                } catch (Exception error) { if (token != IntPtr.Zero) bridge.Cancel(token); Fail(error); }
                finally {
                    try { World?.EndFrame(); } catch (Exception error) { Fail(error); }
                    began = false;
                }
            }
        }
        private void Issue(int eventId, IntPtr token)
        {
            using (var commands = new CommandBuffer { name = "DSPAASR presentation boundary" })
            {
                commands.IssuePluginEventAndData(bridge.RenderEvent, eventId, token);
                Graphics.ExecuteCommandBuffer(commands);
            }
        }
        private void Fail(Exception error)
        {
            if (faulted) return;
            faulted = true;
            bridge.Configure(new NativePresentationConfiguration { Backend = 0, GeneratedFrames = 1, Reflex = 1 });
            warning("Frame generation disabled without changing SR: " + error.Message);
        }
        public void Update()
        {
            if (closed || Time.unscaledTime < nextStatus) return;
            nextStatus = Time.unscaledTime + .5f;
            if (!bridge.TryGetStatus(out var status)) return;
            Status = status;
            if (status.ActiveBackend != lastBackend || (Settings.Backend != FrameGenerationBackend.Off && status.Message != lastReason))
            {
                lastBackend = status.ActiveBackend; lastReason = status.Message;
                information("Presentation: requested=" + status.RequestedBackend + " active=" + status.ActiveBackend +
                    " generation=" + status.Generation + " application=" + status.LastPresentedFrameId + " | " + status.Message);
            }
        }
        public void Dispose()
        {
            if (closed) return;
            closed = true;
            if (owner.Renderer?.Presentation == this) owner.Renderer.Presentation = null;
            if (endOfFrame != null) owner.StopCoroutine(endOfFrame);
            endOfFrame = null;
            loop?.Dispose(); loop = null;
            World?.Dispose(); World = null;
            if (Status.Available) bridge.Configure(new NativePresentationConfiguration { Backend = 0, GeneratedFrames = 1, Reflex = 1 });
        }
    }
}
