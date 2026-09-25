using System;
using System.Collections.Generic;
using System.Reflection;
using System.Reflection.Emit;
using HarmonyLib;
using UnityEngine;
using UnityEngine.PostProcessing;

namespace DSPAAMod.Game
{
    [HarmonyPatch(typeof(PostProcessingBehaviour), "OnPreCull")]
    internal static class BeforeCullPatch
    {
        private static void Prefix(PostProcessingBehaviour __instance)
        {
            // Snapshot the real display camera before SR temporarily assigns its
            // private low-resolution target, regardless of global callback order.
            Plugin.Instance?.Guard(() => Plugin.Instance.Presentation?.World?.BeforeCull(__instance.GetComponent<Camera>()));
            Plugin.Instance?.Guard(() => Plugin.Instance.Renderer.BeforeCull(__instance));
        }
        private static void Postfix(PostProcessingBehaviour __instance)
        { Plugin.Instance?.Guard(() => Plugin.Instance.Presentation?.World?.AfterProjection(__instance)); }
    }
    [HarmonyPatch(typeof(BloomComponent), nameof(BloomComponent.Prepare))]
    internal static class NavigationBloomPatch
    {
        private static void Postfix(BloomComponent __instance, Texture autoExposure)
        { Plugin.Instance?.Renderer?.ObserveNavigationBloom(__instance, autoExposure); }
    }
    [HarmonyPatch(typeof(PostProcessingBehaviour), "OnRenderImage")]
    internal static class AfterImagePatch
    {
        private static void Prefix(PostProcessingBehaviour __instance, ref RenderTexture source, ref RenderTexture destination, out RenderController.ImageScope __state)
        {
            __state = Plugin.Instance?.Renderer.BeginImage(__instance, ref source, ref destination);
        }
        private static IEnumerable<CodeInstruction> Transpiler(IEnumerable<CodeInstruction> instructions)
        {
            var original = AccessTools.Method(typeof(RenderTextureFactory), nameof(RenderTextureFactory.Get), new[] { typeof(RenderTexture) });
            var replacement = AccessTools.Method(typeof(AfterImagePatch), nameof(ResolveTarget));
            int replacements = 0;
            var result = new List<CodeInstruction>();
            foreach (var instruction in instructions)
            {
                if (instruction.Calls(original)) { instruction.opcode = OpCodes.Call; instruction.operand = replacement; ++replacements; }
                result.Add(instruction);
            }
            if (replacements != 2) throw new InvalidOperationException("Unsupported postprocessing allocation flow; DLSS patch not applied.");
            return result;
        }
        private static RenderTexture ResolveTarget(RenderTextureFactory factory, RenderTexture source) =>
            Plugin.Instance != null ? Plugin.Instance.Renderer.ResolveTarget(factory, source) : factory.Get(source);
        private static Exception Finalizer(PostProcessingBehaviour __instance, RenderTexture destination, RenderController.ImageScope __state, Exception __exception)
        {
            Plugin.Instance?.Guard(() => Plugin.Instance.Renderer.AfterImage(__instance, destination, __exception == null));
            Plugin.Instance?.Guard(() => Plugin.Instance.Renderer.EndImage(__state, __exception == null));
            return __exception; // Never hide the game's own failure.
        }
    }
    [HarmonyPatch]
    internal static class FollowingImagePatch
    {
        private static IEnumerable<MethodBase> TargetMethods()
        {
            yield return AccessTools.Method(typeof(TranslucentImageSource), "OnRenderImage");
            yield return AccessTools.Method(typeof(UnityStandardAssets.ImageEffects.SunShafts), "OnRenderImage");
        }
        private static void Prefix(Component __instance, ref RenderTexture source, ref RenderTexture destination, out RenderController.ImageScope __state)
        { __state = Plugin.Instance?.Renderer.BeginImage(__instance, ref source, ref destination); }
        private static Exception Finalizer(RenderController.ImageScope __state, Exception __exception)
        {
            Plugin.Instance?.Guard(() => Plugin.Instance.Renderer.EndImage(__state, __exception == null));
            return __exception;
        }
    }
    [HarmonyPatch(typeof(PostProcessingBehaviour), "OnDisable")]
    internal static class DisablePatch
    {
        private static void Prefix(PostProcessingBehaviour __instance)
        { Plugin.Instance?.Guard(() => Plugin.Instance.Renderer.Remove(__instance)); }
    }
    [HarmonyPatch(typeof(PostProcessingBehaviour), nameof(PostProcessingBehaviour.ResetTemporalEffects))]
    internal static class ResetPatch
    {
        private static void Postfix(PostProcessingBehaviour __instance)
        { Plugin.Instance?.Renderer.Reset(__instance); }
    }
    [HarmonyPatch(typeof(TaaComponent), nameof(TaaComponent.SetProjectionMatrix))]
    internal static class JitterPatch
    {
        private static bool Prefix(TaaComponent __instance, Func<Vector2, Matrix4x4> jitteredFunc)
        {
            if (Plugin.Instance == null) return true;
            bool original = true;
            Plugin.Instance.Guard(() => original = Plugin.Instance.Renderer.Projection(__instance, jitteredFunc));
            return original;
        }
    }
    [HarmonyPatch(typeof(TaaComponent), nameof(TaaComponent.Render))]
    internal static class ResolvePatch
    {
        private static bool Prefix(TaaComponent __instance, RenderTexture source, RenderTexture destination)
        {
            if (Plugin.Instance == null) return true;
            bool original = true;
            Plugin.Instance.Guard(() => original = Plugin.Instance.Renderer.Resolve(__instance, source, destination));
            return original;
        }
        private static void Postfix(TaaComponent __instance, RenderTexture destination)
        { Plugin.Instance?.Guard(() => Plugin.Instance.Renderer.AfterResolve(__instance, destination)); }
    }
}
