using System;
using DSPAAMod.Interop;
using UnityEngine;

namespace DSPAAMod.Game
{
    // IDs select a contract, not a shader-name allowlist. The native draw policy
    // must match the actual PS/VS bytes and resolve support from the bound CBs.
    internal static class FrameGenerationPasses
    {
        internal const ulong Copy = 0x1000;
        internal const ulong WorldBloom = 0x1100;
        internal const ulong CinematicBloom = 0x1200;
        internal const ulong DepthOfField = 0x1300;
        internal const ulong Uber = 0x1400;
        internal const ulong Fxaa = 0x1500;
        internal const ulong SunShafts = 0x1600;
        internal const ulong Border = 0x1700;
        internal const ulong EyeAdaptation = 0x1800;
        internal const ulong EyeCopy = 0x1810;
        internal const ulong EyeHistogram = 0x1811;
        internal const ulong Skybox = 0x1812;
        internal const ulong Grain = 0x1900;
        internal const ulong Lut = 0x1a00;
        internal const ulong ExternalBlur = 0x1b00;

        internal readonly struct Description
        {
            internal readonly ulong Id;
            internal readonly CaptureScopeKind Kind;
            internal readonly bool Final;
            internal readonly string Basis;
            internal Description(ulong id, CaptureScopeKind kind, bool final, string basis)
            { Id = id; Kind = kind; Final = final; Basis = basis; }
        }

        internal static Description Material(Material material, int pass, bool final)
        {
            if (!material || !material.shader || pass < 0 || pass >= material.passCount)
                throw new InvalidOperationException("Unidentified or multipass image-effect draw.");
            string shader = material.shader.name;
            ulong id;
            int expected;
            var kind = CaptureScopeKind.DualColor;
            switch (shader)
            {
                case "Hidden/Post FX/Blit": id = Copy; expected = 1; break;
                case "Hidden/Post FX/Bloom": id = WorldBloom; expected = 4; break;
                case "Hidden/Image Effects/Cinematic/Bloom":
                    id = CinematicBloom; expected = 11; final |= pass >= 7; break;
                case "Hidden/Post FX/Depth Of Field":
                    id = DepthOfField; expected = 8;
                    if (pass <= 1) kind = CaptureScopeKind.SharedPreparation;
                    break;
                case "Hidden/Post FX/Uber Shader": id = Uber; expected = 1; break;
                case "Hidden/Post FX/FXAA": id = Fxaa; expected = 1; final = true; break;
                case "Hidden/SunShaftsComposite":
                    id = SunShafts; expected = 5; final |= pass == 0 || pass == 4; break;
                case "Hidden/Post FX/Eye Adaptation":
                    id = EyeAdaptation; expected = 3; kind = CaptureScopeKind.SharedPreparation; break;
                case "Hidden/Post FX/Grain Generator":
                    id = Grain; expected = 2; kind = CaptureScopeKind.SharedPreparation; break;
                case "Hidden/Post FX/Lut Generator":
                    id = Lut; expected = 1; kind = CaptureScopeKind.SharedPreparation; break;
                case "Hidden/EfficientBlur":
                    id = ExternalBlur; expected = 2; kind = CaptureScopeKind.ExternalBlurPublication; break;
                default: throw new InvalidOperationException("Unrecognized image-effect shader: " + shader);
            }
            if (material.passCount != expected)
                throw new InvalidOperationException("Image-effect pass layout changed: " + shader);
            return new Description(id + (uint)pass, kind, final, shader + " pass " + pass + "; actual-draw shader/CB proof required");
        }
    }
}
