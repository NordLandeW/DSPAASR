using System;
using System.IO;
using BepInEx;
using BepInEx.Configuration;
using DSPAAMod.Core;
using DSPAAMod.Game;
using DSPAAMod.Interop;
using DSPAAMod.UI;
using HarmonyLib;
using UnityEngine;

namespace DSPAAMod
{
    [BepInPlugin(Id, "DSPAASR", "1.1.0")]
    [BepInProcess("DSPGAME.exe")]
    public sealed class Plugin : BaseUnityPlugin
    {
        public const string Id = "dspaa.mod";
        internal static Plugin Instance { get; private set; }
        internal RenderController Renderer { get; private set; }
        internal SettingsSession Settings { get; private set; }
        internal GraphicsOptions Options { get; private set; }
        internal FrameGenerationController Presentation { get; private set; }
        internal FrameGenerationSettingsSession FrameGeneration { get; private set; }
        internal PresentationStartupState StartupState => PresentationStartup.Read(Path.GetDirectoryName(Info.Location)).State;
        internal bool CanRequestFrameGeneration(FrameGenerationBackend backend)
        {
            if (Presentation == null) return backend == FrameGenerationBackend.Off;
            var caps = Presentation?.Status ?? default;
            if (caps.Available || backend == FrameGenerationBackend.Off)
                return PresentationAvailability.CanRequest(caps, StartupState, backend, false);
            string directory = Path.GetDirectoryName(Info.Location);
            bool Has(string name) => File.Exists(Path.Combine(directory, name));
            bool present = backend == FrameGenerationBackend.Fsr ?
                Has("amd_fidelityfx_loader_dx12.dll") && Has("amd_fidelityfx_framegeneration_dx12.dll") :
                Has("sl.interposer.dll") && Has("sl.common.dll") && Has("sl.dlss_g.dll") &&
                Has("sl.reflex.dll") && Has("sl.pcl.dll") && Has("nvngx_dlssg.dll");
            return PresentationAvailability.CanRequest(caps, StartupState, backend, present);
        }
        private ConfigEntry<AaTechnique> technique;
        private ConfigEntry<ModelSelection> model;
        private ConfigEntry<ResolutionMode> resolution;
        private ConfigEntry<float> fsrSharpness;
        private ConfigEntry<FrameGenerationBackend> frameGeneration;
        private ConfigEntry<FrameGenerationMode> frameGenerationMode;
        private ConfigEntry<uint> generatedFrames, frameLimitMicroseconds;
        private ConfigEntry<ReflexMode> reflex;
        private ConfigEntry<float> dynamicTargetFrameRate;
        private ConfigEntry<KeyboardShortcut> captureShortcut;
        private NativeBridge native;
        private Harmony harmony;
        private bool stopped;
        private void Awake()
        {
            Instance = this;
            try
            {
                technique = Config.Bind("Antialiasing", "Technique", AaTechnique.Original,
                    "Original preserves game AA; Fxaa/Taa use the game's filters; Dlss/Fsr use the selected resolution mode. Legacy Dlaa migrates to Dlss with native-resolution DLAA. Changing this file requires restart.");
                model = Config.Bind("Antialiasing", "Model", ModelSelection.Recommended,
                    "Recommended: K for DLAA/Quality/Balanced, M for Performance, L for UltraPerformance. Explicit CNN/K/L/M overrides persist independently of resolution mode.");
                resolution = Config.Bind("Antialiasing", "Resolution", ResolutionMode.Dlaa,
                    "DLSS/FSR resolution mode: Dlaa means native-resolution AA (DLAA or FSR Native AA); SR modes query the selected SDK for actual lower world-render dimensions. Model selection remains independent. Changing this file requires restart.");
                fsrSharpness = Config.Bind("FSR", "Sharpness", 0f,
                    "FSR RCAS sharpening, 0 (disabled) to 1 (maximum). Independent of DLSS model. Changing this file requires restart.");
                captureShortcut = Config.Bind("Diagnostics", "CaptureShortcut", new KeyboardShortcut(KeyCode.F10, KeyCode.LeftControl, KeyCode.LeftShift),
                    "Explicitly trace two Unity frames in LogOutput.log and capture consecutive DLSS submissions (raw color/depth/motion/output) into BepInEx/cache/DSPAAMod/captures. An incomplete pair fails at the end of the trace window. Nothing runs until this shortcut is pressed; captures can be large. Changing this file requires restart.");
                AaSettings initial;
                try { initial = new AaSettings(technique.Value, model.Value, resolution.Value, fsrSharpness.Value); }
                catch (ArgumentOutOfRangeException) { initial = AaSettings.Default; Logger.LogWarning("Invalid settings; using safe defaults without overwriting the config."); }
                Settings = new SettingsSession(initial);
                frameGeneration = Config.Bind("FrameGeneration", "Backend", FrameGenerationBackend.Off,
                    "Off/Fsr/Dlss, independently of AA/SR. The preloader creates the presentation bridge only when FG is selected at startup. Enabling FG without an active bridge requires restart; an existing bridge supports live switching and remains until game exit even after FG is turned Off.");
                frameGenerationMode = Config.Bind("FrameGeneration", "Mode", FrameGenerationMode.Fixed,
                    "Fixed or DLSS Dynamic; availability is queried from the current SDK/device. Dynamic is not eAuto.");
                generatedFrames = Config.Bind("FrameGeneration", "AdditionalFrames", 1u,
                    "Number of additional frames, not display multiplier. FSR=1; DLSS upper limit comes from the runtime (MFG hardware required above 1).");
                reflex = Config.Bind("FrameGeneration", "Reflex", ReflexMode.On,
                    "DLSS Reflex Off/On/OnWithBoost. Off retains before-input Sleep/PCL but pauses FG; this never changes driver settings.");
                dynamicTargetFrameRate = Config.Bind("FrameGeneration", "DynamicTargetFrameRate", 0f,
                    "DLSS Dynamic target; 0 uses the monitor. VSync/driver restrictions still apply. No vendor-independent performance gate.");
                frameLimitMicroseconds = Config.Bind("FrameGeneration", "FrameLimitMicroseconds", 0u,
                    "Reflex frame limiter interval in microseconds; 0 disables that limiter. Does not rewrite the game's own frame-limit or VSync options.");
                FrameGenerationSettings fgInitial;
                try { fgInitial = new FrameGenerationSettings(frameGeneration.Value, frameGenerationMode.Value,
                    generatedFrames.Value, reflex.Value, dynamicTargetFrameRate.Value, frameLimitMicroseconds.Value); }
                catch (ArgumentException) { fgInitial = FrameGenerationSettings.Default; Logger.LogWarning("Invalid FG settings; using Off without overwriting the config."); }
                FrameGeneration = new FrameGenerationSettingsSession(fgInitial);
                try { Presentation = new FrameGenerationController(this, AcquireNative(), fgInitial, text => Logger.LogInfo(text), text => Logger.LogWarning(text)); }
                catch (Exception error) { Logger.LogWarning("Frame generation unavailable; AA/SR remain independent: " + error.Message); }
                Renderer = new RenderController(initial, AcquireNative, text => Logger.LogWarning(text), text => Logger.LogInfo(text));
                Renderer.Presentation = Presentation;
                Renderer.Capture = new FrameCapture(Path.Combine(Paths.CachePath, "DSPAAMod", "captures"), text => Logger.LogInfo(text));
                Options = new GraphicsOptions(this);
                harmony = new Harmony(Id);
                harmony.PatchAll(typeof(Plugin).Assembly);
                Logger.LogInfo("DSPAASR loaded. DLSS and FSR use the selected AA/SR resolution mode; animated objects may exhibit visual artifacts. No driver settings are modified.");
            }
            catch (Exception error)
            {
                harmony?.UnpatchSelf();
                Guard(() => Presentation?.Dispose());
                Guard(() => Renderer?.Shutdown());
                Instance = null;
                Logger.LogError(error);
            }
        }
        private NativeBridge AcquireNative()
        {
            if (native == null)
            {
                string directory = Path.GetDirectoryName(Info.Location);
                string data = Path.Combine(Paths.CachePath, "DSPAAMod");
                native = new NativeBridge(directory, data);
            }
            return native;
        }
        internal void ApplySettings()
        {
            Settings.Apply();
            technique.Value = Settings.Applied.Technique;
            model.Value = Settings.Applied.Model;
            resolution.Value = Settings.Applied.Resolution;
            fsrSharpness.Value = Settings.Applied.FsrSharpness;
            Config.Save();
            Renderer.Configure(Settings.Applied);
            Logger.LogInfo("AA settings applied: " + Settings.Applied.Technique + ", resolution " + Settings.Applied.Resolution + ", model " + Settings.Applied.Model);
        }
        private void StoreFrameGeneration(FrameGenerationSettings value)
        {
            frameGeneration.Value = value.Backend; frameGenerationMode.Value = value.Mode; generatedFrames.Value = value.GeneratedFrames;
            reflex.Value = value.Reflex; dynamicTargetFrameRate.Value = value.DynamicTargetFrameRate; frameLimitMicroseconds.Value = value.FrameLimitMicroseconds;
        }
        internal void ApplyFrameGeneration()
        {
            FrameGeneration.Apply(value => {
                bool autoSave = Config.SaveOnConfigSet;
                Config.SaveOnConfigSet = false;
                try { StoreFrameGeneration(value); Config.Save(); }
                catch { StoreFrameGeneration(FrameGeneration.Applied); throw; }
                finally { Config.SaveOnConfigSet = autoSave; }
            });
            var applied = FrameGeneration.Applied;
            var caps = Presentation?.Status ?? default;
            if (Presentation != null && !caps.Available && !caps.Quarantined && StartupState == PresentationStartupState.Inactive) {
                Logger.LogInfo(applied.Backend == FrameGenerationBackend.Off ? "Frame generation remains Off; native presentation retained." :
                    "Frame generation request saved; restart the game to check support and enable the presentation bridge.");
                return;
            }
            if (Presentation == null || !Presentation.Apply(applied)) Logger.LogWarning("Frame generation request could not be activated; check presentation status.");
        }
        internal void Guard(Action action)
        {
            try { action(); }
            catch (Exception error) { Logger.LogError(error); }
        }
        private void Update()
        {
            if (Instance != this || stopped) return;
            Guard(() =>
            {
                Renderer.Update();
                Presentation?.Update();
                Options.Update();
                if (captureShortcut.Value.IsDown())
                {
                    if (Settings.Applied.Technique == AaTechnique.Dlss) Renderer.RequestCapture();
                    else Logger.LogInfo("Select DLSS before requesting a frame capture.");
                }
            });
        }
        private void OnDestroy()
        {
            if (Instance != this || stopped) return;
            stopped = true;
            Guard(() => Presentation?.Dispose());
            Guard(() => Renderer.Shutdown());
            Guard(() => Options.Dispose());
            harmony?.UnpatchSelf();
            Instance = null;
        }
    }
}
