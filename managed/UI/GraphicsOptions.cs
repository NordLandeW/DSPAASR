using System;
using System.Collections.Generic;
using DSPAAMod.Core;
using UnityEngine;
using UnityEngine.UI;

namespace DSPAAMod.UI
{
    // Three controls replace the MSAA/FXAA rows and insert one resolution row. Keep the original controls
    // hidden and intact so native option refreshes never reinterpret our indices.
    internal sealed class GraphicsOptions : IDisposable
    {
        // Publicized compile references do not change the game's private runtime fields.
        private static readonly System.Reflection.FieldInfo ItemButtonsField =
            HarmonyLib.AccessTools.Field(typeof(UIComboBox), "ItemButtons") ??
            throw new MissingFieldException(typeof(UIComboBox).FullName, "ItemButtons");
        private readonly Plugin plugin;
        private UIOptionWindow window;
        private UIComboBox technique, resolution, configuration;
        private FrameGenerationOptions frameGeneration;
        private RectTransform layoutRoot;
        private Vector2 originalContentSize;
        private Vector2 configurationPosition;
        private Vector2 configurationLabelPosition, availabilityPosition;
        private Text availabilityLabel;
        private UpscalerAvailability shownAvailability, shownFsrAvailability;
        private float rowStep;
        private bool originalConfigurationLabelActive;
        private readonly Dictionary<RectTransform, Vector2> movedRows = new Dictionary<RectTransform, Vector2>();
        private AaMenuDraft draft;
        private Text aaLabel, resolutionLabel, configurationLabel;
        private Localizer aaLocalizer, configurationLocalizer;
        private string originalAaLabel, originalConfigurationLabel;
        private bool originalAaActive, originalFxaaActive, originalAaLocalized, originalConfigurationLocalized;
        private bool synchronizing;
        private int nativeRefreshMsaa;
        private bool nativeRefreshFxaa;
        private UIComboBox openedDropdown;
        private Vector3 dropdownPosition;
        private readonly Vector3[] dropdownCorners = new Vector3[4];
        public GraphicsOptions(Plugin owner) { plugin = owner; }
        private static bool Chinese => (Localization.CurrentLanguageLCID & 0x3ff) == 4;
        private static int MsaaFromIndex(int index) => index == 1 ? 2 : index == 2 ? 4 : index == 3 ? 8 : 0;

        public void Open(UIOptionWindow value)
        {
            plugin.Settings.Open();
            plugin.FrameGeneration.Open();
            plugin.Renderer.RequestUpscalerSupport();
            if (window != value)
            {
                Dispose();
                try { Install(value); }
                catch { Dispose(); throw; }
            }
            draft = new AaMenuDraft(plugin.Settings.Draft, MsaaFromIndex(window.msaaComp.itemIndex), window.fxaaComp.isOn);
            Refresh();
        }
        private void Install(UIOptionWindow value)
        {
            window = value;
            var original = window.msaaComp;
            originalAaActive = original.gameObject.activeSelf;
            originalFxaaActive = window.fxaaComp.gameObject.activeSelf;
            if (original.Items.Count != 4) throw new InvalidOperationException("Another mod changed the MSAA list; refusing to overwrite its controls.");
            aaLabel = FindLabel(window, "MSAA");
            configurationLabel = FindLabel(window, "FXAA");
            if (!aaLabel || !configurationLabel) throw new InvalidOperationException("Cannot identify the native AA labels by their localization keys.");
            originalAaLabel = aaLabel.text;
            originalConfigurationLabel = configurationLabel.text;
            originalConfigurationLabelActive = configurationLabel.gameObject.activeSelf;
            aaLocalizer = aaLabel.GetComponent<Localizer>();
            configurationLocalizer = configurationLabel.GetComponent<Localizer>();
            if (aaLocalizer) { originalAaLocalized = aaLocalizer.enabled; aaLocalizer.enabled = false; }
            if (configurationLocalizer) { originalConfigurationLocalized = configurationLocalizer.enabled; configurationLocalizer.enabled = false; }
            var controls = original.transform.parent;
            var labels = aaLabel.transform.parent;
            if (configurationLabel.transform.parent != labels || window.fxaaComp.transform.parent != controls ||
                controls.parent != labels.parent || !(controls.parent is RectTransform content))
                throw new InvalidOperationException("Unexpected graphics-row hierarchy; refusing to shift unrelated controls.");
            layoutRoot = content;
            originalContentSize = content.sizeDelta;
            var aaRect = (RectTransform)aaLabel.transform;
            var configRect = (RectTransform)configurationLabel.transform;
            float sourceY = content.InverseTransformPoint(aaRect.TransformPoint(aaRect.rect.center)).y;
            float configY = content.InverseTransformPoint(configRect.TransformPoint(configRect.rect.center)).y;
            float step = sourceY - configY;
            if (step <= 0f) throw new InvalidOperationException("Invalid graphics-row spacing.");
            rowStep = step;
            // The game's labels and controls are separate columns, not row wrappers.
            // Snapshot/shift existing children before creating the three new controls.
            ShiftRows(labels, configY, step);
            ShiftRows(controls, configY, step);
            content.sizeDelta = originalContentSize + new Vector2(0f, step);
            technique = Clone(original, "DSPAAMod Antialiasing");
            resolution = Clone(original, "DSPAAMod Resolution");
            configuration = Clone(original, "DSPAAMod Configuration");
            resolution.transform.localPosition += new Vector3(0f, -step, 0f);
            configuration.transform.localPosition += new Vector3(0f, -2f * step, 0f);
            configurationPosition = ((RectTransform)configuration.transform).anchoredPosition;
            configurationLabelPosition = ((RectTransform)configurationLabel.transform).anchoredPosition;
            resolutionLabel = UnityEngine.Object.Instantiate(configurationLabel, labels);
            resolutionLabel.name = "DSPAAMod Resolution Label";
            resolutionLabel.transform.localPosition += new Vector3(0f, step, 0f);
            foreach (var localizer in resolutionLabel.GetComponents<Localizer>()) localizer.enabled = false;
            availabilityLabel = UnityEngine.Object.Instantiate(aaLabel, labels);
            availabilityLabel.name = "DSPAASR Availability";
            foreach (var localizer in availabilityLabel.GetComponents<Localizer>()) localizer.enabled = false;
            availabilityLabel.alignment = TextAnchor.MiddleLeft;
            availabilityLabel.supportRichText = false;
            availabilityLabel.raycastTarget = false;
            availabilityLabel.horizontalOverflow = HorizontalWrapMode.Wrap;
            availabilityLabel.verticalOverflow = VerticalWrapMode.Truncate;
            availabilityLabel.resizeTextForBestFit = false;
            availabilityLabel.fontSize = aaLabel.fontSize;
            availabilityLabel.color = new Color(1f, 0.82f, 0.55f, 1f);
            var corners = new Vector3[4];
            float left = float.PositiveInfinity, right = float.NegativeInfinity;
            foreach (var rect in new[] { aaRect, (RectTransform)technique.transform })
            {
                rect.GetWorldCorners(corners);
                foreach (var corner in corners)
                {
                    float x = layoutRoot.InverseTransformPoint(corner).x;
                    left = Mathf.Min(left, x); right = Mathf.Max(right, x);
                }
            }
            var statusRect = availabilityLabel.rectTransform;
            // Share the real label column, including its native layout offsets.
            statusRect.anchorMin = statusRect.anchorMax = ((RectTransform)labels).pivot;
            statusRect.pivot = new Vector2(0f, 0.5f);
            statusRect.localScale = Vector3.one;
            statusRect.sizeDelta = new Vector2(right - left, 2f * step - 4f);
            availabilityPosition = new Vector2(left, sourceY);
            statusRect.position = layoutRoot.TransformPoint(new Vector3(left, sourceY, 0f));
            original.gameObject.SetActive(false);
            window.fxaaComp.gameObject.SetActive(false);
            technique.onItemIndexChange.AddListener(TechniqueChanged);
            resolution.onItemIndexChange.AddListener(ResolutionChanged);
            configuration.onItemIndexChange.AddListener(ConfigurationChanged);
            frameGeneration = new FrameGenerationOptions(plugin, original, aaLabel, availabilityLabel, layoutRoot, rowStep);
            Canvas.preWillRenderCanvases += ConstrainDropdown;
        }
        private void ShiftRows(Transform column, float fromCenter, float distance)
        {
            foreach (Transform child in column)
            {
                if (!(child is RectTransform rect)) continue;
                float center = layoutRoot.InverseTransformPoint(rect.TransformPoint(rect.rect.center)).y;
                if (center > fromCenter + 0.5f) continue;
                movedRows.Add(rect, rect.anchoredPosition);
                rect.anchoredPosition += new Vector2(0f, -distance);
            }
        }
        internal static UIComboBox Clone(UIComboBox source, string name)
        {
            var copy = UnityEngine.Object.Instantiate(source, source.transform.parent);
            try
            {
                copy.name = name;
                copy.InitItemIndexSelf = false;
                copy.CanInput = false;
                copy.translated = true;
                copy.autoWidth = false;
                copy.DoubleClickChange = false;
                copy.isDroppedDown = false;
                copy.onItemIndexChange = new UIComboBox.ChangeEvent();
                copy.onSubmit = new UIComboBox.SubmitEvent();
                copy.Items = new List<string>();
                copy.ItemsData = new List<int>();
                foreach (var localizer in copy.GetComponentsInChildren<Localizer>(true)) localizer.enabled = false;
                // Runtime entries are cloned but their nonserialized ItemButtons cache
                // is not. Retain only templates, and let UpdateItems create fresh rows.
                foreach (Transform child in copy.m_DropDownContent)
                {
                    if (child == copy.m_ListItemRes.transform || child == copy.m_EmptyItemRes.transform || child == copy.m_SelectionBG.transform) continue;
                    child.gameObject.SetActive(false);
                    UnityEngine.Object.Destroy(child.gameObject);
                }
                copy.gameObject.SetActive(true);
                return copy;
            }
            catch { UnityEngine.Object.Destroy(copy.gameObject); throw; }
        }
        private static Text FindLabel(UIOptionWindow owner, string key)
        {
            foreach (var localizer in owner.GetComponentsInChildren<Localizer>(true))
                if (AaLabels.Matches(localizer.stringKey, key) || AaLabels.Matches(localizer.name, key))
                {
                    var text = localizer.GetComponent<Text>();
                    if (text) return text;
                }
            string translated = key.Translate();
            foreach (var text in owner.GetComponentsInChildren<Text>(true))
                if (text.text.Trim() == translated || AaLabels.Matches(text.text, key)) return text;
            return null;
        }
        internal static void SetItems(UIComboBox control, string[] items, int index)
        {
            control.isDroppedDown = false;
            control.Items = new List<string>(items);
            control.ItemsData = new List<int>();
            for (int i = 0; i < items.Length; ++i) control.ItemsData.Add(i);
            control.UpdateItems();
            control.itemIndex = index;
        }
        internal static void SetItemEnabled(UIComboBox control, int index, bool enabled) =>
            ((List<Button>)ItemButtonsField.GetValue(control))[index].interactable = enabled;
        private void TechniqueChanged()
        {
            if (synchronizing || draft == null || !technique || technique.itemIndex < 0 || technique.itemIndex > (int)AaChoice.Fsr) return;
            if (!draft.TrySelectTechnique((AaChoice)technique.itemIndex, plugin.Renderer.GetAvailability((AaChoice)technique.itemIndex))) { Refresh(); return; }
            plugin.Settings.Draft = draft.Settings;
            Refresh();
        }
        private void ResolutionChanged()
        {
            if (synchronizing || draft == null || !resolution || !draft.ResolutionEnabled || !plugin.Renderer.GetAvailability(draft.Choice).Available || resolution.itemIndex < 0 || resolution.itemIndex > 4) return;
            draft.SelectResolution((ResolutionMode)resolution.itemIndex);
            plugin.Settings.Draft = draft.Settings;
        }
        private void ConfigurationChanged()
        {
            if (synchronizing || draft == null || !configuration || !draft.ConfigurationEnabled || configuration.itemIndex < 0) return;
            if (draft.Choice == AaChoice.Dlss && !plugin.Renderer.Availability.Available) return;
            draft.SelectConfiguration(configuration.itemIndex);
            plugin.Settings.Draft = draft.Settings;
        }
        public void DropdownOpened(UIComboBox control)
        {
            if (!window || !layoutRoot || !control || !control.transform.IsChildOf(layoutRoot)) return;
            RestoreDropdown();
            if (!control.isDroppedDown) return;
            // Keep the native popup/input hierarchy, but raise the opened branch
            // and fit its final (SetState-computed) bounds before canvas rendering.
            for (Transform branch = control.transform; branch && branch != layoutRoot; branch = branch.parent)
                branch.SetAsLastSibling();
            openedDropdown = control;
            dropdownPosition = control.m_DropDownList.localPosition;
        }
        private void ConstrainDropdown()
        {
            if (!openedDropdown) return;
            if (!openedDropdown.isActiveAndEnabled || !openedDropdown.isDroppedDown) { RestoreDropdown(); return; }
            var scroll = openedDropdown.GetComponentInParent<ScrollRect>();
            if (!scroll) return;
            var viewport = scroll.viewport ? scroll.viewport : (RectTransform)scroll.transform;
            var popup = openedDropdown.m_DropDownList;
            popup.localPosition = dropdownPosition;
            BoundsIn(viewport, popup, out float bottom, out float top);
            float lower = viewport.rect.yMin + 2f, upper = viewport.rect.yMax - 2f;
            float shift = 0f;
            if (bottom < lower)
            {
                BoundsIn(viewport, (RectTransform)openedDropdown.transform, out _, out float controlTop);
                // Prefer a complete upward-opening list over covering its button.
                float upward = controlTop - bottom;
                shift = top + upward <= upper ? upward : lower - bottom;
            }
            if (top + shift > upper) shift = upper - top;
            popup.position += viewport.TransformVector(new Vector3(0f, shift, 0f));
        }
        private void BoundsIn(RectTransform relativeTo, RectTransform rect, out float bottom, out float top)
        {
            rect.GetWorldCorners(dropdownCorners);
            bottom = float.PositiveInfinity; top = float.NegativeInfinity;
            foreach (var corner in dropdownCorners)
            {
                float y = relativeTo.InverseTransformPoint(corner).y;
                bottom = Mathf.Min(bottom, y); top = Mathf.Max(top, y);
            }
        }
        private void RestoreDropdown()
        {
            if (openedDropdown && openedDropdown.m_DropDownList) openedDropdown.m_DropDownList.localPosition = dropdownPosition;
            openedDropdown = null;
        }
        public void BeginNativeRefresh() { synchronizing = true; }
        public void EndNativeRefresh()
        {
            if (window)
            {
                nativeRefreshMsaa = MsaaFromIndex(window.msaaComp.itemIndex);
                nativeRefreshFxaa = window.fxaaComp.isOn;
            }
            synchronizing = false;
            Refresh();
        }
        public void Update()
        {
            if (window && draft != null && (!ReferenceEquals(shownAvailability, plugin.Renderer.Availability) ||
                !ReferenceEquals(shownFsrAvailability, plugin.Renderer.FsrAvailability) || (frameGeneration?.Changed ?? false))) Refresh();
        }
        public void Refresh()
        {
            if (!window || !technique || !resolution || !configuration || draft == null) return;
            synchronizing = true;
            try
            {
                aaLabel.text = Chinese ? "抗锯齿 / 超分辨率" : "AA / Super Resolution";
                resolutionLabel.text = Chinese ? "超分辨率档位" : "Resolution mode";
                configurationLabel.text = Chinese ? "配置" : "Configuration";
                shownAvailability = plugin.Renderer.Availability;
                shownFsrAvailability = plugin.Renderer.FsrAvailability;
                SetItems(technique, new[] { Chinese ? "关闭" : "Off", "MSAA", "FXAA", "TAA",
                    CapabilityLabel(shownAvailability), CapabilityLabel(shownFsrAvailability) }, (int)draft.Choice);
                var buttons = (List<Button>)ItemButtonsField.GetValue(technique);
                buttons[(int)AaChoice.Dlss].interactable = shownAvailability.Available;
                buttons[(int)AaChoice.Fsr].interactable = shownFsrAvailability.Available;
                string dlssReason = AvailabilityReason(shownAvailability), fsrReason = AvailabilityReason(shownFsrAvailability);
                availabilityLabel.text = dlssReason + (dlssReason.Length > 0 && fsrReason.Length > 0 ? "\n" : "") + fsrReason;
                bool unavailable = (!shownAvailability.Available && !shownAvailability.Pending) ||
                    (!shownFsrAvailability.Available && !shownFsrAvailability.Pending);
                availabilityLabel.color = unavailable ? new Color(1f, 0.82f, 0.55f, 1f) : aaLabel.color;
                SetItems(resolution, new[] { draft.Choice == AaChoice.Fsr ? "Native AA" : "DLAA", Chinese ? "质量" : "Quality", Chinese ? "平衡" : "Balanced",
                    Chinese ? "性能" : "Performance", Chinese ? "超级性能" : "Ultra Performance" }, (int)draft.Settings.Resolution);
                string[] options = draft.Choice == AaChoice.Dlss ?
                    new[] { Chinese ? "推荐" : "Recommended", "CNN", "Transformer K", "Transformer L", "Transformer M" } :
                    draft.Choice == AaChoice.Msaa ? new[] { "2×", "4×", "8×" } :
                    new[] { Chinese ? "无" : "None" };
                SetItems(configuration, options, draft.ConfigurationIndex);
                frameGeneration?.Refresh();
                RefreshLayout();
            }
            finally { synchronizing = false; }
        }
        private static string CapabilityLabel(UpscalerAvailability state) => state.Name + (state.Available ? "" : state.Pending ?
            (Chinese ? "（检测中）" : " (checking)") : (Chinese ? "（不可用）" : " (unavailable)"));
        private static string AvailabilityReason(UpscalerAvailability state) => state.Reason == UpscalerSupportReason.Runtime ?
            state.Name + (Chinese ? " 不可用，请检查显卡、驱动与模组安装。" : " is unavailable. Check the GPU, driver and mod installation.") : state.Describe(Chinese);
        internal static int MeasureNoteRows(Text note, float spacing)
        {
            if (string.IsNullOrEmpty(note.text)) return 0;
            // Measure wrapping at the final width and fixed readable font size.
            int rows = Mathf.Max(1, Mathf.CeilToInt((note.preferredHeight + 8f) / spacing));
            note.rectTransform.SetSizeWithCurrentAnchors(RectTransform.Axis.Vertical, rows * spacing - 8f);
            return rows;
        }
        private void RefreshLayout()
        {
            bool showAvailability = !string.IsNullOrEmpty(availabilityLabel.text);
            availabilityLabel.gameObject.SetActive(showAvailability);
            int availabilityRows = MeasureNoteRows(availabilityLabel, rowStep);
            bool selectedAvailable = plugin.Renderer.GetAvailability(draft.Choice).Available;
            bool showResolution = draft.ResolutionEnabled && selectedAvailable;
            bool showConfiguration = draft.ConfigurationEnabled && (draft.Choice != AaChoice.Dlss || selectedAvailable);
            resolution.gameObject.SetActive(showResolution);
            resolutionLabel.gameObject.SetActive(showResolution);
            configuration.gameObject.SetActive(showConfiguration);
            configurationLabel.gameObject.SetActive(showConfiguration);
            int optionRows = (showResolution ? 1 : 0) + (showConfiguration ? 1 : 0);
            int secondaryRows = optionRows + availabilityRows + (frameGeneration?.Rows ?? 0);
            var labelRect = aaLabel.rectTransform;
            float left = layoutRoot.InverseTransformPoint(labelRect.TransformPoint(new Vector3(labelRect.rect.xMin, 0f, 0f))).x;
            frameGeneration?.Layout(availabilityPosition.y - rowStep * (optionRows + availabilityRows + 1), left);
            availabilityLabel.rectTransform.position = layoutRoot.TransformPoint(new Vector3(left,
                availabilityPosition.y - rowStep * (optionRows + 0.5f + availabilityRows * 0.5f), 0f));
            // Reflow from the installation snapshot, never from previously moved
            // positions: repeated mode changes must not accumulate row offsets.
            var offset = new Vector2(0f, rowStep * (1 - secondaryRows));
            foreach (var row in movedRows) if (row.Key) row.Key.anchoredPosition = row.Value + offset;
            ((RectTransform)configuration.transform).anchoredPosition = configurationPosition +
                new Vector2(0f, showResolution ? 0f : rowStep);
            ((RectTransform)configurationLabel.transform).anchoredPosition = configurationLabelPosition +
                new Vector2(0f, showResolution ? 0f : rowStep);
            layoutRoot.sizeDelta = originalContentSize - offset;
        }
        public void Read(ref GameOption option)
        {
            if (draft == null || !window) return;
            option.msaa = draft.NativeMsaa;
            option.fxaa = draft.NativeFxaa;
            plugin.Settings.Draft = draft.Settings;
        }
        public void Close() { RestoreDropdown(); plugin.Settings.Cancel(); plugin.FrameGeneration.Cancel(); draft = null; }
        public void Defaults(int tab)
        {
            if (tab != 0) return;
            plugin.Settings.Defaults();
            plugin.FrameGeneration.Defaults();
            draft = new AaMenuDraft(plugin.Settings.Draft, nativeRefreshMsaa, nativeRefreshFxaa);
            Refresh();
        }
        public void Dispose()
        {
            Canvas.preWillRenderCanvases -= ConstrainDropdown;
            RestoreDropdown();
            frameGeneration?.Dispose(); frameGeneration = null;
            if (technique) { technique.gameObject.SetActive(false); UnityEngine.Object.Destroy(technique.gameObject); }
            if (resolution) { resolution.gameObject.SetActive(false); UnityEngine.Object.Destroy(resolution.gameObject); }
            if (resolutionLabel) { resolutionLabel.gameObject.SetActive(false); UnityEngine.Object.Destroy(resolutionLabel.gameObject); }
            if (availabilityLabel) { availabilityLabel.gameObject.SetActive(false); UnityEngine.Object.Destroy(availabilityLabel.gameObject); }
            availabilityLabel = null;
            shownAvailability = shownFsrAvailability = null;
            foreach (var row in movedRows) if (row.Key) row.Key.anchoredPosition = row.Value;
            movedRows.Clear();
            if (layoutRoot) layoutRoot.sizeDelta = originalContentSize;
            layoutRoot = null;
            if (configuration) { configuration.gameObject.SetActive(false); UnityEngine.Object.Destroy(configuration.gameObject); }
            if (window)
            {
                window.msaaComp.gameObject.SetActive(originalAaActive);
                window.fxaaComp.gameObject.SetActive(originalFxaaActive);
            }
            if (aaLabel && originalAaLabel != null) aaLabel.text = originalAaLabel;
            if (configurationLabel && originalConfigurationLabel != null)
            {
                configurationLabel.text = originalConfigurationLabel;
                configurationLabel.gameObject.SetActive(originalConfigurationLabelActive);
            }
            if (aaLocalizer) aaLocalizer.enabled = originalAaLocalized;
            if (configurationLocalizer) configurationLocalizer.enabled = originalConfigurationLocalized;
            window = null;
            technique = resolution = configuration = null;
            aaLabel = resolutionLabel = configurationLabel = null;
            aaLocalizer = configurationLocalizer = null;
            draft = null;
        }
    }
}
