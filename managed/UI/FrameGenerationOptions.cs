using System;
using DSPAAMod.Core;
using DSPAAMod.Interop;
using UnityEngine;
using UnityEngine.UI;

namespace DSPAAMod.UI
{
    // The same Apply/Cancel transaction as AA, but no coupling of the two choices.
    internal sealed class FrameGenerationOptions : IDisposable
    {
        private readonly Plugin plugin;
        private readonly UIComboBox backend, multiplier, reflex;
        private FrameGenerationBackend[] backendChoices = Array.Empty<FrameGenerationBackend>();
        private FrameMultiplierChoice[] multiplierChoices = Array.Empty<FrameMultiplierChoice>();
        private readonly ReflexMode[] reflexChoices = GraphicsMenuChoices.ReflexModes();
        private readonly Text backendLabel, multiplierLabel, reflexLabel, status;
        private readonly RectTransform root;
        private readonly float step;
        private bool synchronizing, showDetails;
        private int statusRows;
        private uint shownFlags, shownMaximum, shownActive, shownRequested, shownSdkStatus;
        private FrameGenerationSettings shownDraft, shownApplied;
        private PresentationStartupState shownStartup;
        private readonly Color warningColor;
        private static bool Chinese => (Localization.CurrentLanguageLCID & 0x3ff) == 4;
        private NativePresentationStatus Capabilities => plugin.Presentation?.Status ?? default;
        public int Rows => 1 + (showDetails ? 2 : 0) + statusRows;
        public bool Changed {
            get {
                var value = Capabilities;
                // Per-frame generation/VSYNC and diagnostic prose do not change
                // this menu's capabilities; do not close a popup for those changes.
                return (value.Flags & ~96u) != shownFlags || value.MaximumGeneratedFrames != shownMaximum ||
                    value.ActiveBackend != shownActive || value.RequestedBackend != shownRequested || value.SdkStatus != shownSdkStatus ||
                    plugin.StartupState != shownStartup || !plugin.FrameGeneration.Draft.Equals(shownDraft) || !plugin.FrameGeneration.Applied.Equals(shownApplied);
            }
        }
        public FrameGenerationOptions(Plugin owner, UIComboBox template, Text label, Text statusTemplate, RectTransform content, float spacing)
        {
            plugin = owner; root = content; step = spacing;
            try {
                backend = GraphicsOptions.Clone(template,"DSPAASR Frame Generation");
                multiplier = GraphicsOptions.Clone(template,"DSPAASR FG Multiplier");
                reflex = GraphicsOptions.Clone(template,"DSPAASR Reflex");
                backendLabel = CloneLabel(label,"DSPAASR FG Label");
                multiplierLabel = CloneLabel(label,"DSPAASR FG Multiplier Label");
                reflexLabel = CloneLabel(label,"DSPAASR Reflex Label");
                status = UnityEngine.Object.Instantiate(statusTemplate,label.transform.parent);
                status.name = "DSPAASR FG Status";
                warningColor = statusTemplate.color;
                status.color = label.color;
                backend.onItemIndexChange.AddListener(BackendChanged);
                multiplier.onItemIndexChange.AddListener(MultiplierChanged);
                reflex.onItemIndexChange.AddListener(ReflexChanged);
            } catch { Dispose(); throw; }
        }
        private static Text CloneLabel(Text source,string name)
        {
            var copy = UnityEngine.Object.Instantiate(source,source.transform.parent);
            copy.name = name;
            foreach (var localizer in copy.GetComponents<Localizer>()) localizer.enabled = false;
            return copy;
        }
        public void Refresh()
        {
            var caps = Capabilities; var value = plugin.FrameGeneration.Draft;
            shownFlags = caps.Flags & ~96u; shownMaximum = caps.MaximumGeneratedFrames;
            shownActive = caps.ActiveBackend; shownRequested = caps.RequestedBackend; shownSdkStatus = caps.SdkStatus;
            shownDraft = value; shownApplied = plugin.FrameGeneration.Applied; shownStartup = plugin.StartupState;
            bool canRequestDlss = plugin.CanRequestFrameGeneration(FrameGenerationBackend.Dlss);
            bool detailsKnown = DlssDetailsKnown(caps);
            synchronizing = true;
            try {
                backendLabel.text = Chinese ? "帧生成" : "Frame generation";
                multiplierLabel.text = Chinese ? "帧生成倍率" : "Frame multiplier";
                reflexLabel.text = "NVIDIA Reflex";
                backendChoices = GraphicsMenuChoices.Backends(plugin.CanRequestFrameGeneration(FrameGenerationBackend.Fsr), canRequestDlss);
                GraphicsOptions.SetItems(backend, Array.ConvertAll(backendChoices, BackendLabel), Array.IndexOf(backendChoices, value.Backend), BackendLabel(value.Backend));
                multiplierChoices = GraphicsMenuChoices.Multipliers(canRequestDlss, detailsKnown, caps.MaximumGeneratedFrames, caps.DynamicSupported);
                GraphicsOptions.SetItems(multiplier, Array.ConvertAll(multiplierChoices, choice => MultiplierLabel(choice.Mode, choice.GeneratedFrames)),
                    GraphicsMenuChoices.FindMultiplier(multiplierChoices, value), MultiplierLabel(value.Mode, value.GeneratedFrames));
                GraphicsOptions.SetItems(reflex, Array.ConvertAll(reflexChoices, ReflexLabel), Array.IndexOf(reflexChoices, value.Reflex), ReflexLabel(value.Reflex));
                showDetails = value.Backend == FrameGenerationBackend.Dlss;
                multiplier.gameObject.SetActive(showDetails); multiplierLabel.gameObject.SetActive(showDetails);
                reflex.gameObject.SetActive(showDetails); reflexLabel.gameObject.SetActive(showDetails);
                status.text = StatusText(caps, value, out bool warning);
                if (value.Backend == FrameGenerationBackend.Dlss && canRequestDlss && GraphicsMenuChoices.FindMultiplier(multiplierChoices, value) < 0) {
                    string note = detailsKnown ?
                        (Chinese ? "所请求的帧生成倍率不可用。" : "The requested frame multiplier is unavailable.") :
                        (Chinese ? "所请求的帧生成倍率尚未检测。" : "The requested frame multiplier has not been checked.");
                    status.text += (status.text.Length == 0 ? "" : "\n") + note;
                    warning |= detailsKnown;
                }
                status.color = warning ? warningColor : backendLabel.color;
                status.gameObject.SetActive(status.text.Length != 0);
                statusRows = GraphicsOptions.MeasureNoteRows(status, step);
            } finally { synchronizing = false; }
        }
        private static bool DlssDetailsKnown(NativePresentationStatus caps) => caps.Available && !caps.Quarantined && caps.ActiveBackend == 2;
        private static string BackendLabel(FrameGenerationBackend value) => value == FrameGenerationBackend.Off ? (Chinese ? "关闭" : "Off") :
            value == FrameGenerationBackend.Fsr ? "FSR 2×" : "DLSS";
        private static string MultiplierLabel(FrameGenerationMode mode, uint generatedFrames) => mode == FrameGenerationMode.Dynamic ?
            (Chinese ? "自适应" : "Dynamic") : ((ulong)generatedFrames + 1) + "×";
        private static string ReflexLabel(ReflexMode value) => value == ReflexMode.Off ? (Chinese ? "关闭" : "Off") :
            value == ReflexMode.On ? (Chinese ? "开启" : "On") : (Chinese ? "开启并增强" : "On + Boost");
        private string StatusText(NativePresentationStatus caps, FrameGenerationSettings value, out bool warning)
        {
            warning = false;
            if (caps.Quarantined) { warning = true; return Chinese ? "帧生成因错误已停用，请重启游戏。" : "Frame generation was disabled after an error. Restart the game."; }
            if (!caps.Available) {
                if (plugin.Presentation != null && plugin.StartupState == PresentationStartupState.Inactive && value.Backend == FrameGenerationBackend.Off)
                    return string.Empty;
                if (PresentationAvailability.NeedsRestart(caps, plugin.StartupState, value) && plugin.CanRequestFrameGeneration(value.Backend)) {
                    string restart = value.Equals(plugin.FrameGeneration.Applied) ?
                        (Chinese ? "设置已保存。重启游戏以检测支持并启用帧生成。" : "Settings saved. Restart the game to check support and enable frame generation.") :
                        (Chinese ? "应用设置并重启游戏，以检测支持并启用帧生成。" : "Apply settings and restart the game to check support and enable frame generation.");
                    if (value.Backend == FrameGenerationBackend.Dlss && value.Reflex == ReflexMode.Off)
                        return (Chinese ? "DLSS 帧生成需要开启 NVIDIA Reflex。" : "DLSS frame generation requires NVIDIA Reflex.") + "\n" + restart;
                    return restart;
                }
                warning = true; return Chinese ? "帧生成不可用，请检查模组安装并重启游戏。" : "Frame generation is unavailable. Check the mod installation and restart the game.";
            }
            if (value.Backend == FrameGenerationBackend.Off) {
                if (!caps.FsrRuntimePresent && !caps.DlssSupported) {
                    warning = true; return Chinese ? "帧生成不可用，请检查显卡、驱动与模组安装。" : "Frame generation is unavailable. Check the GPU, driver and mod installation.";
                }
                return string.Empty;
            }
            if (value.Backend == FrameGenerationBackend.Fsr && !caps.FsrRuntimePresent) {
                warning = true; return Chinese ? "缺少 FSR 帧生成组件，请重新安装模组。" : "FSR frame generation files are missing. Reinstall the mod.";
            }
            if (value.Backend == FrameGenerationBackend.Dlss && !caps.DlssSupported) {
                warning = true; return Chinese ? "DLSS 帧生成不可用，请检查显卡、驱动与模组安装。" : "DLSS frame generation is unavailable. Check the GPU, driver and mod installation.";
            }
            if (!value.Equals(plugin.FrameGeneration.Applied)) {
                if (value.Backend == FrameGenerationBackend.Dlss && value.Reflex == ReflexMode.Off)
                    return Chinese ? "DLSS 帧生成需要开启 NVIDIA Reflex。" : "DLSS frame generation requires NVIDIA Reflex.";
                return Chinese ? "应用设置后生效。" : "Apply settings to use these changes.";
            }
            if (value.Backend == FrameGenerationBackend.Dlss && value.Reflex == ReflexMode.Off)
                return Chinese ? "开启 NVIDIA Reflex 以启用 DLSS 帧生成。" : "Enable NVIDIA Reflex to use DLSS frame generation.";
            if (caps.ActiveBackend == 2 && value.Backend == FrameGenerationBackend.Dlss && caps.SdkStatus != 0) {
                warning = true; return Chinese ? "DLSS 帧生成暂不可用，请检查日志。" : "DLSS frame generation is currently unavailable. Check the log for details.";
            }
            if (caps.RequestedBackend != (uint)value.Backend || caps.ActiveBackend != (uint)value.Backend)
                return Chinese ? "设置已应用，帧生成尚未启用。" : "Settings applied. Frame generation is not active yet.";
            return string.Empty;
        }
        public void Layout(float firstCenter, float left)
        {
            Position(backend.transform,firstCenter); Position(backendLabel.transform,firstCenter);
            Position(multiplier.transform,firstCenter-step); Position(multiplierLabel.transform,firstCenter-step);
            Position(reflex.transform,firstCenter-2*step); Position(reflexLabel.transform,firstCenter-2*step);
            Position(status.transform,firstCenter-step*((showDetails?3:1)+(statusRows-1)*.5f));
            var point = root.InverseTransformPoint(status.rectTransform.position);
            point.x = left;
            status.rectTransform.position = root.TransformPoint(point);
        }
        private void Position(Transform transform,float y)
        {
            var rect = (RectTransform)transform;
            float center = root.InverseTransformPoint(rect.TransformPoint(rect.rect.center)).y;
            var point = root.InverseTransformPoint(rect.position); point.y += y - center;
            rect.position = root.TransformPoint(point);
        }
        private void BackendChanged()
        {
            if (synchronizing || backend.itemIndex < 0 || backend.itemIndex >= backendChoices.Length) return;
            var selected = backendChoices[backend.itemIndex];
            if (!plugin.CanRequestFrameGeneration(selected)) { Refresh(); return; }
            var prior=plugin.FrameGeneration.Draft;
            plugin.FrameGeneration.Draft=new FrameGenerationSettings(selected,
                selected==FrameGenerationBackend.Fsr?FrameGenerationMode.Fixed:prior.Mode,
                selected==FrameGenerationBackend.Fsr?1:prior.GeneratedFrames,prior.Reflex,prior.DynamicTargetFrameRate,prior.FrameLimitMicroseconds);
            plugin.Options.Refresh();
        }
        private void MultiplierChanged()
        {
            if (synchronizing || !showDetails || multiplier.itemIndex < 0 || multiplier.itemIndex >= multiplierChoices.Length) return;
            var caps = Capabilities; var prior = plugin.FrameGeneration.Draft;
            var selected = multiplierChoices[multiplier.itemIndex];
            var next = new FrameGenerationSettings(prior.Backend, selected.Mode, selected.GeneratedFrames,
                prior.Reflex, prior.DynamicTargetFrameRate, prior.FrameLimitMicroseconds);
            var current = GraphicsMenuChoices.Multipliers(plugin.CanRequestFrameGeneration(FrameGenerationBackend.Dlss),
                DlssDetailsKnown(caps), caps.MaximumGeneratedFrames, caps.DynamicSupported);
            if (GraphicsMenuChoices.FindMultiplier(current, next) < 0) { Refresh(); return; }
            plugin.FrameGeneration.Draft = next;
        }
        private void ReflexChanged()
        {
            if (synchronizing || !showDetails || reflex.itemIndex < 0 || reflex.itemIndex >= reflexChoices.Length) return;
            var prior=plugin.FrameGeneration.Draft;
            plugin.FrameGeneration.Draft=new FrameGenerationSettings(prior.Backend,prior.Mode,prior.GeneratedFrames,
                reflexChoices[reflex.itemIndex],prior.DynamicTargetFrameRate,prior.FrameLimitMicroseconds);
        }
        private static void Remove(Component value)
        { if (value) { value.gameObject.SetActive(false); UnityEngine.Object.Destroy(value.gameObject); } }
        public void Dispose()
        { Remove(backend); Remove(multiplier); Remove(reflex); Remove(backendLabel); Remove(multiplierLabel); Remove(reflexLabel); Remove(status); }
    }
}
