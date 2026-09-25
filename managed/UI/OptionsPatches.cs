using HarmonyLib;

namespace DSPAAMod.UI
{
    [HarmonyPatch(typeof(UIOptionWindow), "_OnOpen")]
    internal static class OptionsOpenPatch
    {
        private static void Postfix(UIOptionWindow __instance)
        { Plugin.Instance?.Guard(() => Plugin.Instance.Options.Open(__instance)); }
    }
    [HarmonyPatch(typeof(UIOptionWindow), "_OnClose")]
    internal static class OptionsClosePatch
    {
        private static void Postfix() { Plugin.Instance?.Options.Close(); }
    }
    [HarmonyPatch(typeof(UIOptionWindow), "TempOptionToUI")]
    internal static class OptionsRefreshPatch
    {
        private static void Prefix() { Plugin.Instance?.Options.BeginNativeRefresh(); }
        private static void Postfix() { Plugin.Instance?.Guard(() => Plugin.Instance.Options.EndNativeRefresh()); }
    }
    [HarmonyPatch(typeof(UIOptionWindow), "UIToTempOption")]
    internal static class OptionsReadPatch
    {
        // Harmony field injection accesses the original private field safely. Do not
        // call publicized reference-only private fields directly in production code.
        private static void Postfix(ref GameOption ___tempOption)
        {
            GameOption value = ___tempOption;
            Plugin.Instance?.Guard(() => Plugin.Instance.Options.Read(ref value));
            ___tempOption = value;
        }
    }
    [HarmonyPatch(typeof(UIOptionWindow), "ApplyOptions")]
    internal static class OptionsApplyPatch
    {
        private static void Prefix(ref GameOption ___tempOption)
        {
            GameOption value = ___tempOption;
            Plugin.Instance?.Guard(() => Plugin.Instance.Options.Read(ref value));
            ___tempOption = value;
        }
        private static void Postfix()
        {
            Plugin.Instance?.Guard(() => Plugin.Instance.ApplySettings());
            Plugin.Instance?.Guard(() => Plugin.Instance.ApplyFrameGeneration());
        }
    }
    [HarmonyPatch(typeof(UIOptionWindow), nameof(UIOptionWindow.OnLanguageChange))]
    internal static class OptionsLanguagePatch
    {
        private static void Postfix() { Plugin.Instance?.Guard(() => Plugin.Instance.Options.Refresh()); }
    }
    [HarmonyPatch(typeof(UIComboBox), nameof(UIComboBox.OnPopButtonClick))]
    internal static class AaDropdownPatch
    {
        private static void Postfix(UIComboBox __instance)
        { Plugin.Instance?.Guard(() => Plugin.Instance.Options.DropdownOpened(__instance)); }
    }
    [HarmonyPatch(typeof(UIOptionWindow), "OnRevertButtonClick")]
    internal static class OptionsDefaultsPatch
    {
        private static void Postfix(int idx) { Plugin.Instance?.Guard(() => Plugin.Instance.Options.Defaults(idx)); }
    }
}
