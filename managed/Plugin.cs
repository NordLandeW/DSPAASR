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
        private ConfigEntry<AaTechnique> technique;
        private ConfigEntry<ModelSelection> model;
        private ConfigEntry<ResolutionMode> resolution;
        private ConfigEntry<float> fsrSharpness;
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
                Renderer = new RenderController(initial, AcquireNative, text => Logger.LogWarning(text), text => Logger.LogInfo(text));
                Renderer.Capture = new FrameCapture(Path.Combine(Paths.CachePath, "DSPAAMod", "captures"), text => Logger.LogInfo(text));
                Options = new GraphicsOptions(this);
                harmony = new Harmony(Id);
                harmony.PatchAll(typeof(Plugin).Assembly);
                Logger.LogInfo("DSPAASR loaded. DLSS and FSR use the selected AA/SR resolution mode; animated objects may exhibit visual artifacts. No driver settings are modified.");
            }
            catch (Exception error)
            {
                harmony?.UnpatchSelf();
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
            Guard(() => Renderer.Shutdown());
            Guard(() => Options.Dispose());
            harmony?.UnpatchSelf();
            Instance = null;
        }
    }
}
