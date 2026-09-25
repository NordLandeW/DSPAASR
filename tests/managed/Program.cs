using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using DSPAAMod.Core;
using DSPAAMod.Interop;

internal static class Program
{
    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate void RenderEvent(int eventId, IntPtr token);
    private static void Require(bool condition, string message)
    { if (!condition) throw new InvalidOperationException(message); }

    private static void ModelPolicyChecks()
    {
        var modes = new[] { ResolutionMode.Dlaa, ResolutionMode.Quality, ResolutionMode.Balanced, ResolutionMode.Performance, ResolutionMode.UltraPerformance };
        uint[] recommendations = { 11, 11, 11, 13, 12 };
        for (int i = 0; i < modes.Length; ++i)
        {
            Require(ModelPolicy.ResolvePreset(ModelSelection.Recommended, modes[i]) == recommendations[i], "NVIDIA recommendation mapping changed");
            foreach (var model in new[] { ModelSelection.TransformerK, ModelSelection.TransformerL, ModelSelection.TransformerM })
                Require(ModelPolicy.ResolvePreset(model, modes[i]) == ModelPolicy.ResolvePreset(model, ResolutionMode.Dlaa), "Resolution mode overrode an explicit transformer choice");
            uint cnn = ModelPolicy.ResolvePreset(ModelSelection.Cnn, modes[i]);
            Require(cnn == 5 || cnn == 6, "CNN silently became a transformer");
        }
        try { ModelPolicy.ResolvePreset((ModelSelection)99, ResolutionMode.Dlaa); throw new Exception("Invalid model silently accepted"); }
        catch (ArgumentOutOfRangeException) { }
        var session = new SettingsSession(AaSettings.Default);
        session.Open();
        session.Draft = new AaSettings(AaTechnique.Dlaa, ModelSelection.TransformerL);
        Require(session.Applied.Equals(AaSettings.Default), "Draft applied before Apply");
        session.Cancel();
        Require(session.Draft.Equals(session.Applied), "Cancel retained uncommitted settings");
        session.Draft = new AaSettings(AaTechnique.Dlaa, ModelSelection.Cnn);
        session.Apply();
        session.Defaults();
        Require(session.Applied.Model == ModelSelection.Cnn, "Defaults applied without confirmation");
        session.Cancel();
        Require(session.Draft.Model == ModelSelection.Cnn, "Cancel lost previously applied model");
        session.Defaults(); session.Apply(); session.Open();
        Require(session.Applied.Equals(AaSettings.Default) && session.Draft.Equals(AaSettings.Default), "Default/apply/reopen is inconsistent");
    }
    private static void MenuChecks()
    {
        var original = new AaMenuDraft(AaSettings.Default, 4, true);
        Require(AaLabels.Matches("多重采样抗锯齿 (MSAA)", "MSAA") &&
            AaLabels.Matches("快速近似抗锯齿 (FXAA)", "FXAA") &&
            AaLabels.Matches("Multisample Antialiasing (MSAA)", "MSAA") &&
            AaLabels.Matches("快速近似抗锯齿（FXAA）", "FXAA") && AaLabels.Matches("MSAA", "MSAA"),
            "Localized native AA labels cannot be resolved");
        Require(!AaLabels.Matches("快速近似抗锯齿 (FXAA)", "MSAA") &&
            !AaLabels.Matches("MSAA configuration", "MSAA") && !AaLabels.Matches(null, "FXAA"),
            "Label matching captured the other control or an unrelated description");
        Require(original.Choice == AaChoice.Msaa && original.NativeMsaa == 4 && original.NativeFxaa,
            "Opening the menu changed an existing original-game AA combination");
        original.SelectTechnique(AaChoice.None);
        Require(original.NativeMsaa == 0 && !original.NativeFxaa && !original.ConfigurationEnabled,
            "Off retained hidden native AA");
        original.SelectTechnique(AaChoice.Msaa);
        for (int index = 0; index < 3; ++index)
        {
            original.SelectConfiguration(index);
            Require(original.NativeMsaa == (2 << index) && !original.NativeFxaa && original.ConfigurationIndex == index,
                "MSAA configuration was confused with the technique dropdown index");
        }
        original.SelectTechnique(AaChoice.Dlss);
        original.SelectConfiguration((int)ModelSelection.TransformerM);
        Require(original.NativeMsaa == 0 && original.NativeFxaa && original.Settings.Technique == AaTechnique.Dlss,
            "DLAA left MSAA enabled or removed the game fallback");
        foreach (var choice in new[] { AaChoice.None, AaChoice.Fxaa, AaChoice.Taa })
        {
            original.SelectTechnique(choice);
            Require(!original.ConfigurationEnabled && original.NativeMsaa == 0,
                "A nonconfigurable technique retained MSAA or a configuration row");
            Require(original.NativeFxaa == (choice != AaChoice.None), "Hidden FXAA state disagrees with selected technique");
        }
        original.SelectTechnique(AaChoice.Dlss);
        Require(original.Settings.Model == ModelSelection.TransformerM && original.ConfigurationIndex == (int)ModelSelection.TransformerM,
            "Switching techniques overwrote the manual DLAA model");
        original.SelectTechnique(AaChoice.Msaa);
        Require(original.NativeMsaa == 8 && original.ConfigurationIndex == 2, "Switching techniques forgot the MSAA sample count");
        var session = new SettingsSession(new AaSettings(AaTechnique.Dlaa, ModelSelection.Cnn));
        var menu = new AaMenuDraft(session.Applied, 0, true);
        menu.SelectTechnique(AaChoice.Taa);
        session.Draft = menu.Settings;
        session.Cancel();
        Require(new AaMenuDraft(session.Draft, 0, true).Choice == AaChoice.Dlss && session.Draft.Model == ModelSelection.Cnn,
            "Cancel/reopen retained an uncommitted technique");
        session.Draft = menu.Settings;
        session.Apply();
        session.Open();
        Require(new AaMenuDraft(session.Draft, 0, true).Choice == AaChoice.Taa, "Apply/reopen lost the selected technique");
        session.Defaults();
        Require(session.Applied.Technique == AaTechnique.Taa && new AaMenuDraft(session.Draft, 0, true).Choice == AaChoice.Fxaa,
            "Defaults changed applied settings or failed to represent the game's default AA");
    }

    private static void AvailabilityChecks()
    {
        // Catch vendor-only approval, unusable DLSS selection, and loss of the user's
        // AA/model/resolution choices while capabilities are pending or rejected.
        var pending = UpscalerAvailability.ForPlatform(true, 0x10de, true, true);
        Require(pending.Pending && !pending.Available, "An NVIDIA vendor ID alone enabled DLSS");
        var states = new[] {
            pending,
            UpscalerAvailability.ForPlatform(true, 0x1002, true, true),
            UpscalerAvailability.ForPlatform(true, 0x8086, true, true),
            UpscalerAvailability.ForPlatform(false, 0x10de, true, true),
            UpscalerAvailability.ForPlatform(true, 0x10de, false, true),
            UpscalerAvailability.ForPlatform(true, 0x10de, true, false),
            UpscalerAvailability.Failed("Test driver/runtime rejection")
        };
        foreach (var state in states)
        {
            var initial = new AaSettings(AaTechnique.Original, ModelSelection.TransformerL, ResolutionMode.Balanced);
            var draft = new AaMenuDraft(initial, 8, true);
            Require(!draft.TrySelectTechnique(AaChoice.Dlss, state) && draft.Choice == AaChoice.Msaa &&
                draft.Settings.Equals(initial) && draft.NativeMsaa == 8 && draft.NativeFxaa,
                "Unavailable DLSS was selected or rejection changed existing settings");
            Require(!string.IsNullOrWhiteSpace(state.Describe(false)) && !string.IsNullOrWhiteSpace(state.Describe(true)),
                "A blocked selection has no user-visible reason");
            foreach (var choice in new[] { AaChoice.None, AaChoice.Msaa, AaChoice.Fxaa, AaChoice.Taa })
                Require(draft.TrySelectTechnique(choice, state) && draft.Choice == choice, "DLSS rejection disabled a native AA technique");
            Require(draft.TrySelectTechnique(AaChoice.Dlss, UpscalerAvailability.Supported) &&
                draft.Settings.Model == ModelSelection.TransformerL && draft.Settings.Resolution == ResolutionMode.Balanced,
                "Capability completion failed to enable DLSS or changed the requested model/resolution");
        }
        var saved = new AaSettings(AaTechnique.Dlss, ModelSelection.Cnn, ResolutionMode.Performance);
        var reopened = new AaMenuDraft(saved, 0, true);
        Require(!reopened.TrySelectTechnique(AaChoice.Dlss, pending) && reopened.Settings.Equals(saved),
            "Checking support rewrote an already saved DLSS configuration");
        Require(UpscalerAvailability.Failed("driver detail").Describe(true).Contains("driver detail"),
            "The native failure reason was lost in the translated status");
    }


    private static void FsrChecks()
    {
        // Detect accidental NVIDIA-only gating, wrong backend capability reuse,
        // and loss of saved DLSS model / FSR sharpness when switching algorithms.
        foreach (int vendor in new[] { 0x10de, 0x1002, 0x8086 })
        {
            var pending = UpscalerAvailability.ForPlatform(true, vendor, true, true, UpscalerBackend.Fsr);
            Require(pending.Pending && !pending.Available, "FSR vendor gate bypassed runtime validation");
            var saved = new AaSettings(AaTechnique.Dlss, ModelSelection.TransformerM, ResolutionMode.Balanced, 0.25f);
            var menu = new AaMenuDraft(saved, 0, true);
            Require(!menu.TrySelectTechnique(AaChoice.Fsr, pending) && menu.Settings.Equals(saved), "Pending FSR changed saved settings");
            Require(!menu.TrySelectTechnique(AaChoice.Fsr, UpscalerAvailability.Supported), "DLSS support was reused to approve FSR");
            var supported = pending.Complete(true, "3.1.5");
            Require(menu.TrySelectTechnique(AaChoice.Fsr, supported) && menu.ResolutionEnabled && !menu.ConfigurationEnabled &&
                menu.NativeMsaa == 0 && menu.NativeFxaa && menu.Settings.Temporal, "FSR selection lost its rendering/fallback policy");
            menu.SelectResolution(ResolutionMode.Dlaa);
            Require(menu.Settings.Technique == AaTechnique.Fsr && menu.Settings.Model == ModelSelection.TransformerM && menu.Settings.FsrSharpness == 0.25f,
                "FSR Native AA migrated to DLSS or overwrote an independent setting");
            var session = new SettingsSession(saved) { Draft = menu.Settings };
            session.Cancel(); Require(session.Draft.Equals(saved), "Cancelling FSR leaked settings");
            session.Draft = menu.Settings; session.Apply(); session.Open();
            Require(new AaMenuDraft(session.Draft, 0, true).Choice == AaChoice.Fsr, "FSR did not survive apply/reopen");
            menu.SelectTechnique(AaChoice.Dlss);
            Require(menu.Settings.Model == ModelSelection.TransformerM && menu.Settings.FsrSharpness == 0.25f, "Backend round-trip overwrote settings");
            var failed = pending.Complete(false, "interop unavailable");
            Require(!failed.CanSelect(AaChoice.Fsr) && failed.Describe(false).Contains("interop unavailable"), "FSR failure was hidden or selectable");
        }
        Require(!UpscalerAvailability.ForPlatform(false, 0x1002, true, true, UpscalerBackend.Fsr).Pending &&
            !UpscalerAvailability.ForPlatform(true, 0x1002, false, true, UpscalerBackend.Fsr).Pending &&
            !UpscalerAvailability.ForPlatform(true, 0x1002, true, false, UpscalerBackend.Fsr).Pending, "FSR ignored missing platform inputs");
        Require(new RenderResolution(640, 360, 640, 360, 8).JitterPhases == 8 &&
            new RenderResolution(426, 240, 640, 360, 18).JitterPhases == 18, "FSR SDK jitter phases were replaced by the DLSS minimum");
        foreach (float invalid in new[] { -0.1f, 1.1f, float.NaN, float.PositiveInfinity })
        {
            try { _ = new AaSettings(AaTechnique.Fsr, ModelSelection.Recommended, fsrSharpness: invalid); throw new Exception("Invalid FSR sharpening accepted"); }
            catch (ArgumentOutOfRangeException) { }
        }
    }

    private static void ResolutionChecks()
    {
        // Old configurations must stay native even when a newer resolution key remains.
        var migrated = new AaSettings(AaTechnique.Dlaa, ModelSelection.Cnn, ResolutionMode.Performance);
        Require(migrated.Technique == AaTechnique.Dlss && migrated.Resolution == ResolutionMode.Dlaa && migrated.Model == ModelSelection.Cnn,
            "Legacy DLAA migration silently enabled SR or changed the selected model");
        var session = new SettingsSession(migrated);
        var menu = new AaMenuDraft(session.Applied, 0, true);
        Require(menu.ResolutionEnabled, "DLSS resolution row is unavailable");
        menu.SelectResolution(ResolutionMode.Performance);
        menu.SelectConfiguration((int)ModelSelection.TransformerL);
        Require(menu.Settings.Resolution == ResolutionMode.Performance && menu.Settings.Model == ModelSelection.TransformerL,
            "Model selection overwrote the independent resolution mode");
        session.Draft = menu.Settings; session.Cancel();
        Require(session.Draft.Resolution == ResolutionMode.Dlaa && session.Draft.Model == ModelSelection.Cnn, "Cancel leaked SR configuration");
        session.Draft = menu.Settings; session.Apply(); session.Open();
        Require(session.Draft.Equals(menu.Settings), "Apply/reopen lost SR configuration");
        menu.SelectTechnique(AaChoice.Msaa);
        Require(!menu.ResolutionEnabled, "MSAA exposed the DLSS resolution row");
        try { menu.SelectResolution(ResolutionMode.Quality); throw new Exception("Disabled SR row changed settings"); }
        catch (InvalidOperationException) { }
        menu.SelectTechnique(AaChoice.Dlss);
        Require(menu.Settings.Resolution == ResolutionMode.Performance && menu.Settings.Model == ModelSelection.TransformerL,
            "Technique round-trip lost independent DLSS settings");
        Require(!new AaSettings(AaTechnique.Dlss, ModelSelection.Cnn, ResolutionMode.Quality).Equals(migrated), "Equality ignored resolution mode");
        try { new AaSettings(AaTechnique.Dlss, ModelSelection.Cnn, (ResolutionMode)99); throw new Exception("Invalid resolution accepted"); }
        catch (ArgumentOutOfRangeException) { }
        var native = new RenderResolution(3840, 2160, 3840, 2160);
        var upscale = new RenderResolution(1280, 720, 3840, 2160);
        Require(native.IsNative && native.JitterPhases == Jitter.PhaseCount, "Native mode changed its verified jitter sequence");
        Require(!upscale.IsNative && upscale.JitterPhases == 72, "Ultra-performance footprint was undersampled");
        var samples = new HashSet<(float, float)>();
        for (uint i = 0; i < upscale.JitterPhases; ++i)
        {
            var value = Jitter.ForFrame(i, upscale.JitterPhases);
            Require(samples.Add((value.X, value.Y)), "SR jitter sequence repeated before covering the footprint");
            var repeat = Jitter.ForFrame(i + upscale.JitterPhases, upscale.JitterPhases);
            Require(value.X == repeat.X && value.Y == repeat.Y, "SR jitter period does not match its render resolution");
        }
        try { new RenderResolution(0, 720, 3840, 2160); throw new Exception("Zero render dimension accepted"); }
        catch (ArgumentOutOfRangeException) { }
        try { Jitter.ForFrame(0, 0); throw new Exception("Zero jitter phases accepted"); }
        catch (ArgumentOutOfRangeException) { }
    }


    private static void JitterChecks()
    {
        var samples = new HashSet<(float, float)>();
        float sumX = 0, sumY = 0;
        for (uint frame = 0; frame < Jitter.PhaseCount; ++frame)
        {
            var sample = Jitter.ForFrame(frame);
            Require(sample.X >= -0.5f && sample.X < 0.5f && sample.Y >= -0.5f && sample.Y < 0.5f, "Jitter escaped the pixel footprint");
            Require(samples.Add((sample.X, sample.Y)), "Duplicate temporal sample before sequence completion");
            var repeat = Jitter.ForFrame(frame + Jitter.PhaseCount);
            Require(sample.X == repeat.X && sample.Y == repeat.Y, "Jitter wrap/reset is inconsistent");
            // Project a stationary point with Unity's negative view Z and shifted
            // frustum. RT rendering flips clip Y; D3D's downward viewport flips it
            // again. NGX must receive the resulting pixel displacement on both axes.
            const double width = 2560, height = 1440, z = -3, clipX = 0.4, clipY = -0.3;
            double beforeX = (clipX / -z + 1) * width / 2;
            double beforeY = (clipY / -z + 1) * height / 2;
            double afterX = ((clipX + 2 * sample.X / width * z) / -z + 1) * width / 2;
            double afterY = ((clipY + 2 * sample.Y / height * z) / -z + 1) * height / 2;
            var ngx = Jitter.ToNgx(sample);
            Require(Math.Abs(ngx.X - (afterX - beforeX)) < 0.00001 && Math.Abs(ngx.Y - (afterY - beforeY)) < 0.00001,
                "NGX jitter does not match the projected render-texture displacement");
            sumX += sample.X; sumY += sample.Y;
        }
        Require(samples.Count >= 16 && Math.Abs(sumX / samples.Count) < 0.05f && Math.Abs(sumY / samples.Count) < 0.05f, "Jitter coverage is too short or biased");
    }
    private sealed class VisibilityItem
    {
        public bool Off, Dead, FailHide, FailRestore;
        public int Writes;
    }
    private static void VisibilityChecks()
    {
        // Catch permanent hiding, clobbered pre-existing suppression, and one
        // failed restoration preventing every other renderer from being restored.
        // This exercises the production lease, not a simulated Unity draw pipeline.
        var scope = new RenderVisibilityScope<VisibilityItem>(item => !item.Dead, item => item.Off,
            (item, value) =>
            {
                ++item.Writes;
                if (!value && item.FailRestore) throw new InvalidOperationException("restore failure");
                item.Off = value;
                if (value && item.FailHide) throw new InvalidOperationException("hide failure after mutation");
            });
        var visible = new VisibilityItem();
        var hidden = new VisibilityItem { Off = true };
        var dead = new VisibilityItem { Dead = true };
        Require(scope.Suppress(visible) && visible.Off, "Visible renderer was not suppressed");
        Require(!scope.Suppress(visible) && !scope.Suppress(hidden) && !scope.Suppress(dead) && !scope.Suppress(null),
            "Suppression stole pre-existing/duplicate/inactive ownership");
        scope.Restore(); scope.Restore();
        Require(!visible.Off && visible.Writes == 2 && hidden.Off && hidden.Writes == 0 && dead.Writes == 0,
            "Restoration changed unrelated visibility or was not idempotent");
        var partial = new VisibilityItem { FailHide = true };
        try { scope.Suppress(partial); throw new Exception("Expected injected hide failure"); }
        catch (InvalidOperationException) { }
        scope.Restore();
        Require(!partial.Off, "A partial hide failure left the renderer suppressed");
        var normal = new VisibilityItem();
        var retry = new VisibilityItem { FailRestore = true };
        scope.Suppress(normal); scope.Suppress(retry);
        try { scope.Restore(); throw new Exception("Expected injected restore failure"); }
        catch (InvalidOperationException) { }
        Require(!normal.Off && retry.Off, "A failing restoration blocked unrelated renderers");
        retry.FailRestore = false; scope.Restore();
        Require(!retry.Off, "Failed restoration was not retained for retry");
        var destroyed = new VisibilityItem();
        scope.Suppress(destroyed); destroyed.Dead = true; scope.Restore();
        Require(destroyed.Writes == 1, "Restoration wrote to a destroyed renderer");
    }

    private static void FrameGenerationChecks()
    {
        var settings = new FrameGenerationSettingsSession(FrameGenerationSettings.Default);
        settings.Draft = new FrameGenerationSettings(FrameGenerationBackend.Dlss, FrameGenerationMode.Dynamic, 5, ReflexMode.OnWithBoost, 165, 10000);
        Require(settings.Applied.Backend == FrameGenerationBackend.Off, "FG draft activated without Apply");
        settings.Cancel(); Require(settings.Draft.Backend == FrameGenerationBackend.Off, "FG cancel leaked an uncommitted mode");
        settings.Draft = new FrameGenerationSettings(FrameGenerationBackend.Dlss, FrameGenerationMode.Fixed, 5);
        settings.Apply(); settings.Open(); Require(settings.Draft.GeneratedFrames == 5, "MFG choice was hard-coded to 40-series limits");
        settings.Defaults(); Require(settings.Applied.GeneratedFrames == 5, "FG defaults rewrote applied settings before Apply");
        try { _ = new FrameGenerationSettings(FrameGenerationBackend.Fsr, FrameGenerationMode.Dynamic); throw new Exception("FSR accepted DLSS Dynamic"); }
        catch (ArgumentException) { }
        try { _ = new FrameGenerationSettings(FrameGenerationBackend.Dlss, generatedFrames: 0); throw new Exception("Zero additional frames accepted"); }
        catch (ArgumentOutOfRangeException) { }
    }

    private static void InteropChecks(string dll)
    {
        string root = Path.Combine(Path.GetTempPath(), "DSPAAMod-managed-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        // A separate process owns this test module. It is never a deployed mod file.
        // The placeholder runtime is NOT loaded: this test only exercises queue/control
        // ABI, so it remains independent of NVIDIA hardware, drivers and NGX execution.
        File.Copy(dll, Path.Combine(root, "DSPAANative.dll"));
        File.WriteAllBytes(Path.Combine(root, "nvngx_dlss.dll"), new byte[] { 0 });
        var bridge = new NativeBridge(root, Path.Combine(root, "data"));
        var callback = Marshal.GetDelegateForFunctionPointer<RenderEvent>(bridge.RenderEvent);
        var presentation = new PresentationBridge(bridge);
        Require(presentation.TryGetStatus(out var presentationStatus) && !presentationStatus.Available && presentationStatus.Message.Length > 0,
            "No-bootstrap state invented presentation support or lost its reason");
        Require(!presentation.Begin(out _) && !presentation.Configure(new NativePresentationConfiguration { GeneratedFrames = 1, Reflex = 1 }),
            "A late SR-only DLL created presentation ownership");
        var noFrame = NativePresentationInputs.Create();
        Require(presentation.Queue(ref noFrame) == IntPtr.Zero, "Presentation accepted an unallocated application frame");
        var capture = new CaptureBridge(bridge, presentation.RenderEvent);
        Require(capture.TryGetStatus(out var captureStatus) && !captureStatus.Ready && captureStatus.Reason.Length > 0,
            "Capture ABI invented an early graphics owner in an SR-only process");
        Require(capture.TryGetDepthCopyResult(out var depthCopyResult) && depthCopyResult.Flags == 0 &&
            !depthCopyResult.Submitted(1,1,1), "An SR-only process invented a submitted depth-copy receipt");
        var receipt = new NativeDepthCopyResult { Version = 1, Flags = 3, ApplicationFrameId = 7, Generation = 9, RequestId = 11 };
        Require(receipt.Submitted(7,9,11) && !receipt.Submitted(8,9,11) && !receipt.Submitted(7,10,11) &&
            !receipt.Submitted(7,9,12) && !receipt.Submitted(7,9,0), "Depth-copy success escaped its frame/generation/request identity");
        receipt.Flags = 1;
        Require(!receipt.Submitted(7,9,11), "An executed but failed depth copy was treated as usable");
        Require(capture.Queue(new NativeCaptureCommand { ApplicationFrameId = 1, Generation = 1, OcclusionSlot = -1,
            OcclusionDomain = NativeCaptureDomain.Identity }) == IntPtr.Zero,
            "Capture queued graphics work without an early facade");
        capture.Cancel(new IntPtr(123456)); // Opaque late/unknown handles are harmless, not native pointers.
        Require((int)Marshal.OffsetOf<NativeCaptureCommand>(nameof(NativeCaptureCommand.Source)) == 56 &&
            (int)Marshal.OffsetOf<NativeCaptureStatus>(nameof(NativeCaptureStatus.ReasonBytes)) == 104,
            "Capture metadata no longer matches the native command/status layout");
        Require((int)Marshal.OffsetOf<NativeFrame>(nameof(NativeFrame.JitterX)) == 72, "Frame temporal payload has the wrong ABI offset");
        Require(Marshal.SizeOf<NativeFrame>() == 112 && (int)Marshal.OffsetOf<NativeFrame>(nameof(NativeFrame.OutputWidth)) == 96,
            "SR frame extension broke the ABI v2 prefix/size");
        Require(Marshal.SizeOf<NativeOptimalSettings>() == 300, "Optimal-settings payload has the wrong ABI size");
        Require(bridge.RequestOptimal(7, IntPtr.Zero, 2560, 1440, 1) == IntPtr.Zero && !bridge.TryGetOptimal(7, out _),
            "A null device anchor started a sizing query");
        Require(Marshal.SizeOf<NativeSupport>() == 264 && (int)Marshal.OffsetOf<NativeSupport>(nameof(NativeSupport.MessageBytes)) == 8,
            "Capability result marshaling disagrees with the native ABI");
        Require(bridge.RequestSupport(IntPtr.Zero) == IntPtr.Zero && !bridge.TryGetSupport(IntPtr.Zero, out _),
            "A null capability anchor/token was accepted");
        Require(NativeBridge.FsrParametersSize == 40 && NativeBridge.FsrOptimalSize == 304, "FSR extension ABI is misaligned");
        Require(bridge.RequestSupport(IntPtr.Zero, 1) == IntPtr.Zero && bridge.RequestOptimal(7, IntPtr.Zero, 640, 360, 0, 1) == IntPtr.Zero &&
            !bridge.TryGetFsrOptimal(7, out _), "FSR accepted a null device or invented a sizing result");
        var invalidFrame = new NativeFrame(); var fsrParameters = new NativeFsrParameters();
        Require(bridge.SubmitFsr(ref invalidFrame, ref fsrParameters) == IntPtr.Zero, "FSR accepted an empty frame");
        const ulong camera = 0xfedcba9876543210;
        IntPtr token = bridge.Release(camera);
        Require(token != IntPtr.Zero && !bridge.TryGetStatus(camera, out _), "Release completed before its render event");
        callback(1, token);
        Require(bridge.TryGetStatus(camera, out var status) && status.Result == 2 && status.Size == NativeBridge.StatusSize && status.Message.Length > 0,
            "Native completion did not round-trip through managed marshaling");
        Require(!bridge.TryGetStatus(camera, out _), "Completion acknowledgement leaked");
        callback(1, token); // duplicate opaque token must never be dereferenced
        callback(1, bridge.Shutdown());
        // The module must remain loaded while Unity might hold callbacks. Its Windows
        // file lock is released when this test process exits; the runner removes root.
        Console.WriteLine("interop_artifact=" + root);
    }
    private static int Main(string[] args)
    {
        try
        {
            Require(args.Length == 1, "Pass the built native DLL path");
            ModelPolicyChecks(); MenuChecks(); AvailabilityChecks(); FsrChecks(); ResolutionChecks(); JitterChecks(); VisibilityChecks(); FrameGenerationChecks(); InteropChecks(args[0]);
            Console.WriteLine("Model overrides, settings transactions, jitter coverage and real DLL interop passed.");
            return 0;
        }
        catch (Exception error) { Console.Error.WriteLine(error); return 1; }
    }
}
