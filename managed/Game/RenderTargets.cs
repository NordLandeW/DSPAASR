using System;
using System.Runtime.InteropServices;
using DSPAAMod.Core;
using UnityEngine;

namespace DSPAAMod.Game
{
    internal sealed class RenderTargets : IDisposable
    {
        public readonly RenderResolution Resolution;
        public int Width => Resolution.InputWidth;
        public int Height => Resolution.InputHeight;
        public RenderTexture World { get; private set; }
        public RenderTexture PostA { get; private set; }
        public RenderTexture PostB { get; private set; }
        public RenderTexture Color { get; private set; }
        public RenderTexture Output { get; private set; }
        public RenderTexture Depth { get; private set; }
        public RenderTexture Motion { get; private set; }
        public IntPtr ColorPointer { get; private set; }
        public IntPtr OutputPointer { get; private set; }
        public IntPtr DepthPointer { get; private set; }
        public IntPtr MotionPointer { get; private set; }
        public bool Valid => Color && Output && Depth && Motion && Color.IsCreated() && Output.IsCreated() && Depth.IsCreated() && Motion.IsCreated() &&
            (Resolution.IsNative || (World && PostA && PostB && World.IsCreated() && PostA.IsCreated() && PostB.IsCreated()));
        public RenderTargets(RenderResolution resolution)
        {
            Resolution = resolution;
            try
            {
                Color = Create("DSPAAMod Color", RenderTextureFormat.ARGBHalf, false);
                Output = Create("DSPAAMod Output", RenderTextureFormat.ARGBHalf, true, true);
                if (!resolution.IsNative)
                {
                    World = Create("DSPAAMod Low-resolution World", RenderTextureFormat.ARGBHalf, false, false, 24);
                    PostA = Create("DSPAAMod Native Post A", RenderTextureFormat.ARGBHalf, false, true);
                    PostB = Create("DSPAAMod Native Post B", RenderTextureFormat.ARGBHalf, false, true);
                }
                Depth = Create("DSPAAMod Depth", RenderTextureFormat.RFloat, false);
                Motion = Create("DSPAAMod Motion", RenderTextureFormat.RGHalf, false);
                // Native pointers are cached on creation, never fetched every frame.
                ColorPointer = RetainPointer(Color);
                OutputPointer = RetainPointer(Output);
                DepthPointer = RetainPointer(Depth);
                MotionPointer = RetainPointer(Motion);
                if (ColorPointer == IntPtr.Zero || OutputPointer == IntPtr.Zero || DepthPointer == IntPtr.Zero || MotionPointer == IntPtr.Zero)
                    throw new InvalidOperationException("Cannot obtain D3D11 frame resources.");
            }
            catch { Dispose(); throw; }
        }
        private static IntPtr RetainPointer(RenderTexture texture)
        {
            IntPtr pointer = texture.GetNativeTexturePtr();
            // A cached borrowed pointer must not become dangling between frames,
            // including a Unity/driver resource recreation. Retain the D3D11 object
            // independently; a removed device is rejected by the native bridge.
            if (pointer != IntPtr.Zero) Marshal.AddRef(pointer);
            return pointer;
        }

        private RenderTexture Create(string name, RenderTextureFormat format, bool randomWrite, bool output = false, int depth = 0)
        {
            int width = output ? Resolution.OutputWidth : Width, height = output ? Resolution.OutputHeight : Height;
            var texture = new RenderTexture(width, height, depth, format, RenderTextureReadWrite.Linear)
            {
                name = name, hideFlags = HideFlags.HideAndDontSave,
                antiAliasing = 1, useMipMap = false, autoGenerateMips = false,
                enableRandomWrite = randomWrite, filterMode = FilterMode.Point, wrapMode = TextureWrapMode.Clamp
            };
            if (!texture.Create())
            {
                UnityEngine.Object.Destroy(texture);
                throw new InvalidOperationException("Cannot allocate " + name);
            }
            return texture;
        }
        public void Dispose()
        {
            if (ColorPointer != IntPtr.Zero) Marshal.Release(ColorPointer);
            if (OutputPointer != IntPtr.Zero) Marshal.Release(OutputPointer);
            if (DepthPointer != IntPtr.Zero) Marshal.Release(DepthPointer);
            if (MotionPointer != IntPtr.Zero) Marshal.Release(MotionPointer);
            ColorPointer = OutputPointer = DepthPointer = MotionPointer = IntPtr.Zero;
            Destroy(Color); Destroy(Output); Destroy(Depth); Destroy(Motion);
            Destroy(World); Destroy(PostA); Destroy(PostB);
            Color = Output = Depth = Motion = World = PostA = PostB = null;
        }
        private static void Destroy(RenderTexture texture)
        {
            if (!texture) return;
            texture.Release();
            UnityEngine.Object.Destroy(texture);
        }
    }
}
