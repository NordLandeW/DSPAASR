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
        private bool synchronizing, showDetails, showStatus;
        private uint shownFlags, shownMaximum, shownActive;
        private string shownMessage;
        private static bool Chinese => (Localization.CurrentLanguageLCID & 0x3ff) == 4;
        private NativePresentationStatus Capabilities => plugin.Presentation?.Status ?? default;
        public int Rows => 1 + (showDetails ? 2 : 0) + (showStatus ? 2 : 0);
        public bool Changed {
            get {
                var value = Capabilities;
                return value.Flags != shownFlags || value.MaximumGeneratedFrames != shownMaximum ||
                    value.ActiveBackend != shownActive || value.Message != shownMessage;
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
                status = UnityEngine.Object.Instantiate(statusTemplate,content);
                status.name = "DSPAASR FG Status";
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
            shownFlags = caps.Flags; shownMaximum = caps.MaximumGeneratedFrames;
            shownActive = caps.ActiveBackend; shownMessage = caps.Message;
            bool usable = caps.Available && !caps.Quarantined;
            synchronizing = true;
            try {
                backendLabel.text = Chinese ? "帧生成" : "Frame generation";
                multiplierLabel.text = Chinese ? "显示倍率" : "Display multiplier";
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
                GraphicsOptions.SetItems(reflex,new[] {Chinese ? "关闭（暂停 DLSS FG）" : "Off (pauses DLSS FG)", "On", "On + Boost"},(int)value.Reflex);
                showDetails = value.Backend == FrameGenerationBackend.Dlss;
                multiplier.gameObject.SetActive(showDetails); multiplierLabel.gameObject.SetActive(showDetails);
                reflex.gameObject.SetActive(showDetails); reflexLabel.gameObject.SetActive(showDetails);
                string reason;
                if (!caps.Available) reason = Chinese ? "帧生成需要安装早期呈现组件并重新启动游戏；抗锯齿／超分辨率不受影响。" : "FG requires the early presentation component and a game restart; AA/SR remain independent.";
                else if (caps.Quarantined) reason = (Chinese ? "呈现组件已安全隔离：" : "Presentation quarantined: ") + caps.Message;
                else if (showDetails && caps.ActiveBackend != 2 && caps.RequestedBackend != 2) reason = Chinese ? "先应用 DLSS，以查询本机多倍／自适应能力。帧生成与上方抗锯齿／超分辨率独立。" : "Apply DLSS to query this device's Multi/Dynamic capability. FG is independent of AA/SR.";
                else if (value.Backend != FrameGenerationBackend.Off) reason = caps.Message;
                else if (!caps.DlssSupported || !caps.FsrRuntimePresent) reason = Chinese ? "不可用后端已禁选；需要对应运行库与受支持的 GPU／驱动。" : "Unavailable backends are disabled; matching runtimes and supported GPU/driver are required.";
                else reason = string.Empty;
                status.text = reason; showStatus = reason.Length != 0; status.gameObject.SetActive(showStatus);
            } finally { synchronizing = false; }
        }
        public void Layout(float firstCenter)
        {
            Position(backend.transform,firstCenter); Position(backendLabel.transform,firstCenter);
            Position(multiplier.transform,firstCenter-step); Position(multiplierLabel.transform,firstCenter-step);
            Position(reflex.transform,firstCenter-2*step); Position(reflexLabel.transform,firstCenter-2*step);
            status.rectTransform.sizeDelta = new Vector2(status.rectTransform.sizeDelta.x,2*step-4);
            Position(status.transform,firstCenter-step*((showDetails?3:1)+.5f));
        }
        private void Position(Transform transform,float y)
        {
            var point = root.InverseTransformPoint(transform.position); point.y = y;
            transform.position = root.TransformPoint(point);
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
