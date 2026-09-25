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
        private readonly Text backendLabel, multiplierLabel, reflexLabel, status;
        private readonly RectTransform root;
        private readonly float step;
        private bool synchronizing, showDetails;
        private int statusRows;
        private uint shownFlags, shownMaximum, shownActive, shownRequested, shownSdkStatus;
        private FrameGenerationSettings shownDraft, shownApplied;
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
                    !plugin.FrameGeneration.Draft.Equals(shownDraft) || !plugin.FrameGeneration.Applied.Equals(shownApplied);
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
            shownDraft = value; shownApplied = plugin.FrameGeneration.Applied;
            bool usable = caps.Available && !caps.Quarantined;
            synchronizing = true;
            try {
                backendLabel.text = Chinese ? "帧生成" : "Frame generation";
                multiplierLabel.text = Chinese ? "帧生成倍率" : "Frame multiplier";
                reflexLabel.text = "NVIDIA Reflex";
                GraphicsOptions.SetItems(backend,new[] { Chinese ? "关闭" : "Off", "FSR 2×", "DLSS" },(int)value.Backend);
                GraphicsOptions.SetItemEnabled(backend,1,usable && caps.FsrRuntimePresent);
                GraphicsOptions.SetItemEnabled(backend,2,usable && caps.DlssSupported);
                var choices = new System.Collections.Generic.List<string> {"2×","3×","4×","5×","6×",Chinese ? "自适应" : "Dynamic"};
                int index = value.Mode == FrameGenerationMode.Dynamic ? 5 : (int)Math.Min(value.GeneratedFrames - 1,4u);
                if (value.Mode == FrameGenerationMode.Fixed && value.GeneratedFrames > 5) {
                    choices.Add(((ulong)value.GeneratedFrames+1) + "×"); index = choices.Count-1;
                }
                GraphicsOptions.SetItems(multiplier,choices.ToArray(),index);
                uint maximum = caps.ActiveBackend == 2 ? caps.MaximumGeneratedFrames : 1u;
                for (int i=0;i<5;++i) GraphicsOptions.SetItemEnabled(multiplier,i,usable && caps.DlssSupported && (uint)(i+1)<=maximum);
                GraphicsOptions.SetItemEnabled(multiplier,5,usable && caps.DynamicSupported && caps.ActiveBackend == 2);
                if (choices.Count>6) GraphicsOptions.SetItemEnabled(multiplier,6,false);
                GraphicsOptions.SetItems(reflex,new[] {Chinese ? "关闭" : "Off", Chinese ? "开启" : "On", Chinese ? "开启并增强" : "On + Boost"},(int)value.Reflex);
                showDetails = value.Backend == FrameGenerationBackend.Dlss;
                multiplier.gameObject.SetActive(showDetails); multiplierLabel.gameObject.SetActive(showDetails);
                reflex.gameObject.SetActive(showDetails); reflexLabel.gameObject.SetActive(showDetails);
                status.text = StatusText(caps, value, out bool warning);
                status.color = warning ? warningColor : backendLabel.color;
                status.gameObject.SetActive(status.text.Length != 0);
                statusRows = GraphicsOptions.MeasureNoteRows(status, step);
            } finally { synchronizing = false; }
        }
        private string StatusText(NativePresentationStatus caps, FrameGenerationSettings value, out bool warning)
        {
            warning = false;
            if (!caps.Available) { warning = true; return Chinese ? "帧生成不可用，请检查模组安装并重启游戏。" : "Frame generation is unavailable. Check the mod installation and restart the game."; }
            if (caps.Quarantined) { warning = true; return Chinese ? "帧生成因错误已停用，请重启游戏。" : "Frame generation was disabled after an error. Restart the game."; }
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
                    return Chinese ? "应用后将暂停 DLSS 帧生成。" : "Applying these settings will pause DLSS frame generation.";
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
            if (synchronizing || backend.itemIndex<0 || backend.itemIndex>2) return;
            var caps=Capabilities; var selected=(FrameGenerationBackend)backend.itemIndex;
            if (selected != FrameGenerationBackend.Off && (!caps.Available || caps.Quarantined ||
                (selected==FrameGenerationBackend.Fsr ? !caps.FsrRuntimePresent : !caps.DlssSupported))) { Refresh(); return; }
            var prior=plugin.FrameGeneration.Draft;
            plugin.FrameGeneration.Draft=new FrameGenerationSettings(selected,
                selected==FrameGenerationBackend.Fsr?FrameGenerationMode.Fixed:prior.Mode,
                selected==FrameGenerationBackend.Fsr?1:prior.GeneratedFrames,prior.Reflex,prior.DynamicTargetFrameRate,prior.FrameLimitMicroseconds);
            plugin.Options.Refresh();
        }
        private void MultiplierChanged()
        {
            if (synchronizing || !showDetails || multiplier.itemIndex<0 || multiplier.itemIndex>5) return;
            var caps=Capabilities; var prior=plugin.FrameGeneration.Draft; int index=multiplier.itemIndex;
            bool dynamic=index==5;
            uint maximum=caps.ActiveBackend==2?caps.MaximumGeneratedFrames:1u;
            if ((dynamic && (!caps.DynamicSupported || caps.ActiveBackend!=2)) || (!dynamic && (uint)(index+1)>maximum)) { Refresh(); return; }
            plugin.FrameGeneration.Draft=new FrameGenerationSettings(prior.Backend,
                dynamic?FrameGenerationMode.Dynamic:FrameGenerationMode.Fixed,dynamic?Math.Max(maximum,1u):(uint)(index+1),
                prior.Reflex,prior.DynamicTargetFrameRate,prior.FrameLimitMicroseconds);
        }
        private void ReflexChanged()
        {
            if (synchronizing || !showDetails || reflex.itemIndex<0 || reflex.itemIndex>2) return;
            var prior=plugin.FrameGeneration.Draft;
            plugin.FrameGeneration.Draft=new FrameGenerationSettings(prior.Backend,prior.Mode,prior.GeneratedFrames,
                (ReflexMode)reflex.itemIndex,prior.DynamicTargetFrameRate,prior.FrameLimitMicroseconds);
        }
        private static void Remove(Component value)
        { if (value) { value.gameObject.SetActive(false); UnityEngine.Object.Destroy(value.gameObject); } }
        public void Dispose()
        { Remove(backend); Remove(multiplier); Remove(reflex); Remove(backendLabel); Remove(multiplierLabel); Remove(reflexLabel); Remove(status); }
    }
}
