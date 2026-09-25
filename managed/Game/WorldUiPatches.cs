using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection.Emit;
using HarmonyLib;
using UnityEngine;
using UnityEngine.PostProcessing;
using UnityEngine.UI;

namespace DSPAAMod.Game
{
    [HarmonyPatch(typeof(CanvasUpdateRegistry), "PerformUpdate")]
    internal static class WorldUiRebuildPatch
    {
        private static void Postfix() => Plugin.Instance?.Guard(() => Plugin.Instance.Presentation?.Capture?.WorldUi?.AfterCanvasUpdate());
    }
    [HarmonyPatch(typeof(PostProcessingBehaviour), "OnRenderImage")]
    internal static class WorldUiResolvePatch
    {
        internal static bool Supported { get; private set; }
        private static IEnumerable<CodeInstruction> Transpiler(IEnumerable<CodeInstruction> instructions)
        {
            var code = instructions.ToList(); Supported = false;
            var resolve = AccessTools.Method(typeof(TaaComponent), nameof(TaaComponent.Render), new[] { typeof(RenderTexture), typeof(RenderTexture) });
            var white = AccessTools.PropertyGetter(typeof(GraphicsUtils), "whiteTexture");
            int call = code.FindIndex(value => value.Calls(resolve));
            int boundary = code.FindIndex(value => value.Calls(white));
            if (call < 2 || boundary <= call || code.Count(value => value.Calls(resolve)) != 1 || code.Count(value => value.Calls(white)) != 1)
                return code; // Keep the independent SR patch; do not suppress UI without a verified boundary.
            var source = code[call - 2];
            if (source.opcode != OpCodes.Ldloc && source.opcode != OpCodes.Ldloc_S && source.opcode != OpCodes.Ldloc_0 &&
                source.opcode != OpCodes.Ldloc_1 && source.opcode != OpCodes.Ldloc_2 && source.opcode != OpCodes.Ldloc_3) return code;
            var first = new CodeInstruction(OpCodes.Ldarg_0);
            first.labels.AddRange(code[boundary].labels); code[boundary].labels.Clear();
            first.blocks.AddRange(code[boundary].blocks); code[boundary].blocks.Clear();
            code.InsertRange(boundary, new[] { first, new CodeInstruction(source.opcode,source.operand),
                new CodeInstruction(OpCodes.Call,AccessTools.Method(typeof(WorldUiResolvePatch),nameof(Resolved))) });
            Supported = true;
            return code;
        }
        private static void Resolved(PostProcessingBehaviour behaviour, RenderTexture source)
        {
            var capture = Plugin.Instance?.Presentation?.Capture;
            if (capture == null || !capture.IsMain(behaviour.GetComponent<Camera>())) return;
            Plugin.Instance.Guard(() => capture.WorldUi?.Resolved(source));
        }
    }
}
