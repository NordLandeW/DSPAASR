using System;
using System.Collections.Generic;
using System.Reflection;
using System.Reflection.Emit;
using DSPAAMod.Interop;
using HarmonyLib;
using UnityEngine;
using UnityEngine.PostProcessing;

namespace DSPAAMod.Game
{
    // Annotate the original GPU operations in place. Never call Prepare or an
    // OnRenderImage twice: history, noise, exposure and publication run once.
    internal static class FrameGenerationEffects
    {
        private static readonly Guid supportedModule = new Guid("ee6dc40f-a6a2-4b39-b220-81c6de923db6");
        [ThreadStatic] internal static FrameGenerationCapture Current;
        internal static string UnsupportedFlow;
        private static FrameGenerationCapture Capture => Plugin.Instance?.Presentation?.Capture;
        internal static bool VersionMatches => typeof(PostProcessingBehaviour).Module.ModuleVersionId == supportedModule;

        internal static void Fail(FrameGenerationCapture capture, string reason)
        {
            if (capture == null) return;
            // Capture failure must not prevent the original GPU operation or
            // replace an exception thrown by the game with a bookkeeping error.
            try { capture.Unsupported(reason); } catch { }
        }
        private static FrameGenerationCapture Begin(FrameGenerationCapture capture, CaptureScopeKind kind,
            ulong id, string basis, bool fullOverwrite = true, RenderTexture destination = null)
        {
            if (capture?.Active != true) return null;
            try
            {
                // No guessed texel-sign, ST, slots or radii here. The actual-draw
                // native callback resolves the complete support before replay.
                // A real RenderTexture can still be a lazy, hardware-less
                // checkout. Full color draws declare their ACTUAL output with
                // that checkout's epoch, just as CameraTarget uses its own epoch.
                bool observeOutput = kind == CaptureScopeKind.DualColor && fullOverwrite;
                ulong outputEpoch = destination ? capture.OutputEpoch(destination) : 0;
                capture.BeginPass(kind, id, basis, fullOverwrite,
                    implicitOutput: observeOutput, outputEpoch: observeOutput ? outputEpoch : 0);
                return capture;
            }
            catch (Exception error) { Fail(capture, error.Message); return null; }
        }
        private static void End(FrameGenerationCapture capture)
        {
            if (capture == null) return;
            try { capture.EndPass(); } catch (Exception error) { Fail(capture, error.Message); }
        }
        private static void Remember(FrameGenerationCapture capture, RenderTexture target)
        {
            if (capture?.Active != true) return;
            try { capture.Remember(target); } catch (Exception error) { Fail(capture, error.Message); }
        }
        private static void Follow(FrameGenerationCapture capture)
        {
            if (capture?.Active != true) return;
            try { capture.FollowColorTransfers(); } catch (Exception error) { Fail(capture, error.Message); }
        }
        private static void BlitCopy(FrameGenerationCapture capture, Texture source, RenderTexture destination, bool remember)
        {
            var scope = Begin(capture, CaptureScopeKind.DualColor, FrameGenerationPasses.Copy,
                "Unity BlitCopy; actual-draw shader/CB proof required", destination: destination);
            try { Graphics.Blit(source, destination); }
            finally { End(scope); }
            if (remember) Remember(capture, destination);
            Follow(scope);
        }

        // Explicit adapter entry for RenderController. Bookkeeping copies use
        // false; only the root/camera owner chooses the final native-size output.
        // Neither source handoff nor texture checkout is inferred here.
        internal static void Copy(Texture source, RenderTexture destination, bool remember)
            => BlitCopy(Capture, source, destination, remember);
        internal static void FinalCopy(Texture source, RenderTexture destination)
            => BlitCopy(Current, source, destination, true);
        internal static void SharedCopy(Texture source, RenderTexture destination)
        {
            var scope = Begin(Current, CaptureScopeKind.SharedPreparation, FrameGenerationPasses.EyeCopy,
                "Original exposure preparation copy, once", destination: destination);
            try { Graphics.Blit(source, destination); }
            finally { End(scope); }
        }
        private static void MaterialCopy(Texture source, RenderTexture destination, Material material, int pass, bool final)
        {
            var capture = Current;
            FrameGenerationCapture scope = null;
            bool remember = false;
            if (capture?.Active == true)
            {
                try
                {
                    var description = FrameGenerationPasses.Material(material, pass, final);
                    remember = description.Final;
                    scope = Begin(capture, description.Kind, description.Id, description.Basis, destination: destination);
                }
                catch (Exception error) { Fail(capture, error.Message); }
            }
            try { Graphics.Blit(source, destination, material, pass); }
            finally { End(scope); }
            if (remember) Remember(capture, destination);
            Follow(scope);
        }
        internal static void MaterialCopy(Texture source, RenderTexture destination, Material material, int pass)
            => MaterialCopy(source, destination, material, pass, false);
        internal static void FinalMaterialCopy(Texture source, RenderTexture destination, Material material, int pass)
            => MaterialCopy(source, destination, material, pass, true);
        internal static void Dispatch(ComputeShader shader, int kernel, int x, int y, int z)
        {
            var scope = Begin(Current, CaptureScopeKind.SharedPreparation, FrameGenerationPasses.EyeHistogram,
                "Original eye histogram dispatch and history preparation, once", false);
            try { shader.Dispatch(kernel, x, y, z); }
            finally { End(scope); }
        }
        internal static void ClearWithSkybox(bool clearDepth, Camera camera)
        {
            var scope = Begin(Current, CaptureScopeKind.SharedPreparation, FrameGenerationPasses.Skybox,
                "Original SunShafts skybox preparation, once", false);
            try { GL.ClearWithSkybox(clearDepth, camera); }
            finally { End(scope); }
        }
        internal static FrameGenerationCapture BeginBorder(Material material)
        {
            if (Current?.Active != true) return null;
            if (!material || !material.shader || material.shader.name != "Hidden/SimpleClear" || material.passCount != 1)
            { Fail(Current, "Unrecognized SunShafts border material."); return null; }
            return Begin(Current, CaptureScopeKind.PartialWrite, FrameGenerationPasses.Border,
                "Original SunShafts border raster; preserve unwritten auxiliary pixels", false);
        }
        internal static void EndBorder(FrameGenerationCapture capture) => End(capture);
        internal static void Acquired(RenderTexture texture)
        {
            if (Current?.Active != true) return;
            try { Current.Acquired(texture); } catch (Exception error) { Fail(Current, error.Message); }
        }
        internal static RenderTexture Temporary(int width, int height, int depth)
        {
            var texture = RenderTexture.GetTemporary(width, height, depth);
            Acquired(texture); return texture;
        }
        internal static RenderTexture Temporary(int width, int height, int depth, RenderTextureFormat format)
        {
            var texture = RenderTexture.GetTemporary(width, height, depth, format);
            Acquired(texture); return texture;
        }
        internal static void Release(RenderTexture texture)
        {
            var capture = Capture;
            try { capture?.Released(texture); } catch (Exception error) { Fail(capture, error.Message); }
            RenderTexture.ReleaseTemporary(texture);
        }
        internal static MethodInfo Method(Type type, string name)
        {
            var method = type == null ? null : AccessTools.DeclaredMethod(type, name);
            if (method == null) UnsupportedFlow = "Missing supported image-effect method: " + type?.FullName + "." + name;
            return method;
        }
        internal static Type GameType(string name) => typeof(PostProcessingBehaviour).Assembly.GetType(name, false);
        internal static IEnumerable<MethodBase> Roots()
        {
            foreach (string name in new[] { "UnityEngine.PostProcessing.PostProcessingBehaviour",
                "UnityStandardAssets.ImageEffects.SunShafts", "UnityStandardAssets.CinematicEffects.Bloom", "TranslucentImageSource" })
            {
                var method = Method(GameType(name), "OnRenderImage");
                if (method != null) yield return method;
            }
        }
        internal static IEnumerable<MethodBase> Operations()
        {
            foreach (var method in Roots()) yield return method;
            foreach (var entry in new[] {
                new[] { "UnityEngine.PostProcessing.BloomComponent", "Prepare" },
                new[] { "UnityEngine.PostProcessing.DepthOfFieldComponent", "Prepare" },
                new[] { "UnityEngine.PostProcessing.DepthOfFieldComponent", "OnDisable" },
                new[] { "UnityEngine.PostProcessing.EyeAdaptationComponent", "Prepare" },
                new[] { "UnityEngine.PostProcessing.GrainComponent", "Prepare" },
                new[] { "UnityEngine.PostProcessing.ColorGradingComponent", "GenerateLut" },
                new[] { "UnityEngine.PostProcessing.FxaaComponent", "Render" },
                new[] { "UnityEngine.PostProcessing.RenderTextureFactory", "Release" },
                new[] { "UnityEngine.PostProcessing.RenderTextureFactory", "ReleaseAll" },
                new[] { "TranslucentImageSource", "ProgressiveBlur" },
                new[] { "TranslucentImageSource", "ProgressiveResampling" } })
            {
                var method = Method(GameType(entry[0]), entry[1]);
                if (method != null) yield return method;
            }
        }
    }

    [HarmonyPatch]
    internal static class FrameGenerationEffectRootPatch
    {
        private static IEnumerable<MethodBase> TargetMethods() => FrameGenerationEffects.Roots();
        [HarmonyPriority(Priority.Last)]
        private static void Prefix(object __instance, RenderTexture __0, RenderTexture __1, out FrameGenerationCapture __state)
        {
            __state = FrameGenerationEffects.Current;
            var capture = Plugin.Instance?.Presentation?.Capture;
            FrameGenerationEffects.Current = null;
            if (!(__instance is Component component)) return;
            capture?.TraceImageInput(component, __0, __1);
            if (capture?.Owns(component) != true) return;
            if (__state != null)
            { FrameGenerationEffects.Fail(capture, "Reentrant owned image-effect execution."); return; }
            if (!FrameGenerationEffects.VersionMatches || FrameGenerationEffects.UnsupportedFlow != null)
            {
                FrameGenerationEffects.Fail(capture, FrameGenerationEffects.UnsupportedFlow ?? "Unsupported game image-effect module.");
                return;
            }
            FrameGenerationEffects.Current = capture;
            // Source handoff and ending a preceding UI scope belong to the
            // root/camera adapter, not this prefix (PP can run before root seed).
        }
        private static Exception Finalizer(Exception __exception, FrameGenerationCapture __state)
        {
            if (__exception != null) FrameGenerationEffects.Fail(FrameGenerationEffects.Current, "Original image effect threw: " + __exception.Message);
            FrameGenerationEffects.Current = __state;
            return __exception;
        }
    }

    [HarmonyPatch]
    internal static class FrameGenerationEffectOperationsPatch
    {
        private static IEnumerable<MethodBase> TargetMethods() => FrameGenerationEffects.Operations();
        private static IEnumerable<CodeInstruction> Transpiler(IEnumerable<CodeInstruction> instructions, MethodBase __originalMethod)
        {
            foreach (var instruction in instructions)
            {
                if ((instruction.opcode == OpCodes.Call || instruction.opcode == OpCodes.Callvirt) && instruction.operand is MethodInfo called)
                {
                    string replacement = null;
                    var parameters = called.GetParameters();
                    var signature = new Type[parameters.Length];
                    for (int i = 0; i < signature.Length; ++i) signature[i] = parameters[i].ParameterType;
                    bool recognized = false;
                    if (called.DeclaringType == typeof(Graphics) && called.Name == "Blit")
                    {
                        recognized = true;
                        if (signature.Length == 2 && signature[0] == typeof(Texture) && signature[1] == typeof(RenderTexture))
                            replacement = __originalMethod.DeclaringType == typeof(EyeAdaptationComponent) ? "SharedCopy" : "FinalCopy";
                        else if (signature.Length == 4 && signature[0] == typeof(Texture) && signature[1] == typeof(RenderTexture) &&
                            signature[2] == typeof(Material) && signature[3] == typeof(int))
                            replacement = __originalMethod.DeclaringType == typeof(PostProcessingBehaviour) ? "FinalMaterialCopy" : "MaterialCopy";
                    }
                    else if (called.DeclaringType == typeof(RenderTexture) && called.Name == "GetTemporary")
                    { recognized = true; replacement = "Temporary"; }
                    else if (called.DeclaringType == typeof(RenderTexture) && called.Name == "ReleaseTemporary")
                    { recognized = true; replacement = "Release"; }
                    else if (called.DeclaringType == typeof(GL) && called.Name == "ClearWithSkybox")
                    { recognized = true; replacement = "ClearWithSkybox"; }
                    else if (called.DeclaringType == typeof(ComputeShader) && called.Name == "Dispatch")
                    {
                        recognized = true; replacement = "Dispatch";
                        var instanceSignature = new Type[signature.Length + 1]; instanceSignature[0] = typeof(ComputeShader);
                        Array.Copy(signature, 0, instanceSignature, 1, signature.Length); signature = instanceSignature;
                    }
                    if (recognized)
                    {
                        var wrapper = replacement == null ? null : AccessTools.DeclaredMethod(typeof(FrameGenerationEffects), replacement, signature);
                        if (wrapper == null)
                            FrameGenerationEffects.UnsupportedFlow = "Unsupported GPU call in " + __originalMethod.DeclaringType?.FullName + "." + __originalMethod.Name + ": " + called;
                        else
                        {
                            // Retain this instruction's labels and exception blocks.
                            instruction.opcode = OpCodes.Call; instruction.operand = wrapper;
                        }
                    }
                }
                yield return instruction;
            }
        }
    }

    [HarmonyPatch]
    internal static class FrameGenerationFactoryCheckoutPatch
    {
        private static MethodBase TargetMethod() => AccessTools.DeclaredMethod(typeof(RenderTextureFactory), "Get",
            new[] { typeof(int), typeof(int), typeof(int), typeof(RenderTextureFormat), typeof(RenderTextureReadWrite),
                typeof(FilterMode), typeof(TextureWrapMode), typeof(string) });
        private static void Postfix(RenderTexture __result) => FrameGenerationEffects.Acquired(__result);
    }

    [HarmonyPatch]
    internal static class FrameGenerationBorderPatch
    {
        private static MethodBase TargetMethod() => FrameGenerationEffects.Method(
            FrameGenerationEffects.GameType("UnityStandardAssets.ImageEffects.PostEffectsBase"), "DrawBorder");
        private static void Prefix(Material material, out FrameGenerationCapture __state)
            => __state = FrameGenerationEffects.BeginBorder(material);
        private static Exception Finalizer(Exception __exception, FrameGenerationCapture __state)
        { FrameGenerationEffects.EndBorder(__state); return __exception; }
    }
}
