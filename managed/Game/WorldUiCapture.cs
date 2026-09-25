using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using DSPAAMod.Core;
using DSPAAMod.Interop;
using UnityEngine;
using UnityEngine.Experimental.Rendering;
using UnityEngine.Rendering;
using UnityEngine.UI;
using Object = UnityEngine.Object;

namespace DSPAAMod.Game
{
    // Capture the real, rebuilt uGUI meshes. Do not regenerate text, advance
    // UI logic, change Canvas mode, or borrow next frame's mutable mesh/atlas.
    internal sealed class WorldUiCapture : IDisposable
    {
        private sealed class Item
        {
            public CanvasRenderer Renderer;
            public Mesh Mesh;
            public Material Material;
            public Matrix4x4 Transform;
            public int LayerOrder, CanvasOrder, Depth;
        }
        private sealed class AtlasDescriptor
        {
            public Texture2D Source;
            public int Width, Height, Mips;
            public TextureFormat Format;
            public GraphicsFormat GraphicsFormat;
            public ulong Bytes;
            public bool Matches(Texture2D texture) => texture && texture.width == Width && texture.height == Height &&
                texture.graphicsFormat == GraphicsFormat && texture.mipmapCount == Mips;
        }
        private sealed class Atlas
        {
            public Texture2D Texture;
            public AtlasDescriptor Descriptor;
            public bool Retired;
        }
        private sealed class TextureBinding
        {
            public Material Material;
            public string Property;
            public Texture2D Source;
        }
        private sealed class RetiringAtlas
        {
            public Slot Owner;
            public Atlas Atlas;
            public int DestroyFrame;
        }
        private sealed class Slot
        {
            public ulong Frame;
            public RenderTexture Depth;
            public IntPtr DepthPointer;
            public ulong DepthEpoch;
            public readonly List<Item> Items = new List<Item>();
            public ulong AtlasBytes;
            public readonly Dictionary<Texture2D, Atlas> Textures = new Dictionary<Texture2D, Atlas>();
            public void ClearMeshes()
            {
                foreach (var item in Items) { if (item.Mesh) Object.Destroy(item.Mesh); if (item.Material) Object.Destroy(item.Material); }
                Items.Clear();
            }
            public void Dispose()
            {
                ClearMeshes(); foreach (var atlas in Textures.Values) RetireAtlas(this, atlas); Textures.Clear();
                if (DepthPointer != IntPtr.Zero) Marshal.Release(DepthPointer); DepthPointer = IntPtr.Zero;
                if (Depth) { Depth.Release(); Object.Destroy(Depth); } Depth = null;
            }
        }
        // All lifetime operations run on Unity's main thread. These are adapter
        // policy limits for conservative CPU + GPU atlas storage, not GPU limits
        // or measurements of driver overhead. CPU shadows remain allocated.
        private const ulong SlotAtlasBudget = 256UL * 1024 * 1024;
        private const ulong TotalAtlasBudget = 768UL * 1024 * 1024;
        private static ulong liveAtlasBytes;
        private static readonly List<RetiringAtlas> retiringAtlases = new List<RetiringAtlas>();
        // Unknown GPU retirement retains meshes, depth and atlases until process
        // teardown. Quarantined slots still count against the global byte policy.
        private static readonly List<Slot> quarantine = new List<Slot>();
        private readonly FrameGenerationController presentation;
        private readonly FrameGenerationCapture capture;
        private readonly Action<string> warning;
        private readonly List<Slot> slots = new List<Slot>();
        private readonly RenderVisibilityScope<CanvasRenderer> visibility = new RenderVisibilityScope<CanvasRenderer>(
            value => value, value => value.cull, (value, hidden) => value.cull = hidden);
        private Slot current;
        private Camera camera;
        private bool snapshotReady, suppressed, drawn, closed;
        private ulong nextDepthEpoch;
        private int canvasUpdateFrame = -1;
        public bool Excluded => snapshotReady && suppressed;
        public WorldUiCapture(FrameGenerationController owner, FrameGenerationCapture inputCapture, Action<string> warn)
        { presentation = owner; capture = inputCapture; warning = warn; }
        public void BeginInput()
        {
            visibility.Restore(); camera = null; current = null;
            snapshotReady = suppressed = drawn = false; canvasUpdateFrame = -1;
            CollectRetiredAtlases();
        }
        // Invoked after the original CanvasUpdateRegistry graphics rebuild.
        public void AfterCanvasUpdate()
        {
            if (closed || !capture.WantsFrame || !GameCamera.main || suppressed) return;
            canvasUpdateFrame = Time.frameCount;
        }
        private static ulong AtlasStorage(AtlasDescriptor descriptor)
        {
            uint blockWidth = GraphicsFormatUtility.GetBlockWidth(descriptor.GraphicsFormat);
            uint blockHeight = GraphicsFormatUtility.GetBlockHeight(descriptor.GraphicsFormat);
            uint blockBytes = GraphicsFormatUtility.GetBlockSize(descriptor.GraphicsFormat);
            if (descriptor.Mips <= 0 || blockWidth == 0 || blockHeight == 0 || blockBytes == 0)
                throw new InvalidOperationException("World UI atlas has an unsupported storage layout");
            int width = descriptor.Width, height = descriptor.Height;
            ulong bytes = 0;
            checked {
                for (int mip = 0; mip < descriptor.Mips; ++mip) {
                    // ComputeMipmapSize returns uint. Check its block lower bound
                    // in 64 bits first; converting an overflowed uint is too late.
                    ulong blocks = (((ulong)width + blockWidth - 1) / blockWidth) * (((ulong)height + blockHeight - 1) / blockHeight);
                    ulong minimum = blocks * blockBytes;
                    if (minimum > uint.MaxValue) throw new InvalidOperationException("World UI atlas mip exceeds the byte estimator range");
                    uint gpu = GraphicsFormatUtility.ComputeMipmapSize(width, height, descriptor.GraphicsFormat);
                    uint cpu = GraphicsFormatUtility.ComputeMipmapSize(width, height, descriptor.Format);
                    if (gpu == 0 || cpu == 0 || gpu < minimum)
                        throw new InvalidOperationException("World UI atlas mip has no reliable byte estimate");
                    // Retain CPU data; use the larger layout for BOTH copies so
                    // legacy TextureFormat expansion cannot undercount its shadow.
                    bytes += 2UL * Math.Max(gpu, cpu);
                    width = Math.Max(1, width / 2); height = Math.Max(1, height / 2);
                }
            }
            return bytes;
        }
        private static void StageTexture(Dictionary<Texture2D, AtlasDescriptor> descriptors, List<TextureBinding> bindings,
            Material material, string property, Texture source)
        {
            if (!(source is Texture2D original) || !original || original.width <= 0 || original.height <= 0)
                throw new InvalidOperationException("World UI uses a non-2D or unavailable atlas");
            // Deduplicate by source identity, NEVER by dimensions/format alone:
            // two identical descriptors can contain entirely different pixels.
            if (!descriptors.TryGetValue(original, out var descriptor)) {
                if (descriptors.Count >= 16) throw new InvalidOperationException("World UI snapshot atlas count policy exceeded (16)");
                descriptor = new AtlasDescriptor { Source = original, Width = original.width, Height = original.height,
                    Mips = original.mipmapCount, Format = original.format, GraphicsFormat = original.graphicsFormat };
                descriptor.Bytes = AtlasStorage(descriptor); descriptors.Add(original, descriptor);
            } else if (!descriptor.Matches(original) || descriptor.Format != original.format)
                throw new InvalidOperationException("World UI atlas changed during descriptor collection");
            bindings.Add(new TextureBinding { Material = material, Property = property, Source = original });
        }
        private static void ReserveAtlasBytes(Slot slot, ulong bytes)
        {
            ulong slotTotal = checked(slot.AtlasBytes + bytes), total = checked(liveAtlasBytes + bytes);
            if (slotTotal > SlotAtlasBudget || total > TotalAtlasBudget)
                throw new InvalidOperationException("World UI atlas allocation policy exceeded: conservative CPU + GPU bytes, slot " +
                    slotTotal + "/" + SlotAtlasBudget + ", all live/deferred/quarantine " + total + "/" + TotalAtlasBudget +
                    ". Limits are DSPAASR policy (256/768 MiB), not GPU capability.");
            slot.AtlasBytes = slotTotal; liveAtlasBytes = total;
        }
        private static void ReleaseAtlasBytes(Slot slot, ulong bytes)
        {
            ulong slotRemaining = checked(slot.AtlasBytes - bytes), remaining = checked(liveAtlasBytes - bytes);
            slot.AtlasBytes = slotRemaining; liveAtlasBytes = remaining;
        }
        private static void RetireAtlas(Slot slot, Atlas atlas)
        {
            if (atlas.Retired) return;
            if (!atlas.Texture) { ReleaseAtlasBytes(slot, atlas.Descriptor.Bytes); atlas.Retired = true; return; }
            // Only GPU-retired slots or never-submitted new allocations get here.
            // Destroy is deferred: keep charging old AND new storage at replacement.
            retiringAtlases.Add(new RetiringAtlas { Owner = slot, Atlas = atlas, DestroyFrame = Time.frameCount });
            atlas.Retired = true;
            Object.Destroy(atlas.Texture);
        }
        private static void CollectRetiredAtlases()
        {
            for (int i = retiringAtlases.Count - 1; i >= 0; --i) {
                var entry = retiringAtlases[i];
                if (entry.DestroyFrame == Time.frameCount) continue;
                if (entry.Atlas.Texture) {
                    // A failed/deferred Destroy cannot release its reservation.
                    Object.Destroy(entry.Atlas.Texture); entry.DestroyFrame = Time.frameCount; continue;
                }
                ReleaseAtlasBytes(entry.Owner, entry.Atlas.Descriptor.Bytes); retiringAtlases.RemoveAt(i);
            }
        }
        private static bool Reusable(Atlas atlas, AtlasDescriptor descriptor) => atlas != null && !atlas.Retired &&
            atlas.Descriptor.Format == descriptor.Format && descriptor.Matches(atlas.Texture);
        private static void PrepareAtlases(Slot slot, Dictionary<Texture2D, AtlasDescriptor> descriptors, CommandBuffer commands)
        {
            // Acquire has observed this slot's GPU fence. Evict unused OR
            // incompatible pages. Keep their charge until deferred destruction;
            // a replacement too large for this frame can then fit a later frame,
            // rather than being permanently blocked by an unusable cached page.
            var unused = new List<Texture2D>();
            foreach (var entry in slot.Textures) {
                if (!descriptors.TryGetValue(entry.Key, out var expected) || !Reusable(entry.Value, expected)) unused.Add(entry.Key);
            }
            foreach (var key in unused) { RetireAtlas(slot, slot.Textures[key]); slot.Textures.Remove(key); }
            ulong additional = 0;
            foreach (var descriptor in descriptors.Values) {
                if (!descriptor.Matches(descriptor.Source) || descriptor.Source.format != descriptor.Format)
                    throw new InvalidOperationException("World UI atlas changed before snapshot allocation");
                if (!slot.Textures.TryGetValue(descriptor.Source, out var atlas) || !Reusable(atlas, descriptor))
                    additional = checked(additional + descriptor.Bytes);
            }
            // Reserve the entire batch before allocating; old replacements and
            // deferred evictions remain charged. No visibility has changed yet.
            ReserveAtlasBytes(slot, additional);
            ulong unassigned = additional;
            try {
                foreach (var descriptor in descriptors.Values) {
                    slot.Textures.TryGetValue(descriptor.Source, out var atlas);
                    if (!Reusable(atlas, descriptor)) {
                        var fresh = new Atlas { Descriptor = descriptor };
                        unassigned = checked(unassigned - descriptor.Bytes);
                        bool installed = false;
                        try {
                            // Assign before property setters, so every partially
                            // initialized Unity allocation retains a byte owner.
                            fresh.Texture = new Texture2D(descriptor.Width, descriptor.Height, descriptor.Format, descriptor.Mips,
                                !GraphicsFormatUtility.IsSRGBFormat(descriptor.GraphicsFormat));
                            if (!descriptor.Matches(fresh.Texture)) throw new InvalidOperationException("World UI atlas copy changed its sampling layout");
                            fresh.Texture.name = "DSPAASR world UI atlas"; fresh.Texture.hideFlags = HideFlags.HideAndDontSave;
                            if (atlas != null) RetireAtlas(slot, atlas);
                            slot.Textures[descriptor.Source] = fresh; installed = true; atlas = fresh;
                        } finally { if (!installed) RetireAtlas(slot, fresh); }
                    }
                    var original = descriptor.Source; var copy = atlas.Texture;
                    copy.filterMode = original.filterMode; copy.wrapModeU = original.wrapModeU; copy.wrapModeV = original.wrapModeV;
                    copy.wrapModeW = original.wrapModeW; copy.anisoLevel = original.anisoLevel; copy.mipMapBias = original.mipMapBias;
                    commands.CopyTexture(original, copy);
                }
            } finally { ReleaseAtlasBytes(slot, unassigned); }
        }
        private Slot Acquire()
        {
            ulong completed = capture.Status.CompletedUnityFrame;
            foreach (var slot in slots) if (slot.Frame <= completed) { slot.ClearMeshes(); return slot; }
            if (slots.Count >= 3) throw new InvalidOperationException("World UI snapshots are still in GPU use");
            var fresh = new Slot(); slots.Add(fresh); return fresh;
        }
        private static bool SupportedMaterial(string shader)
        {
            // These original unlit families have a reviewed native alpha/VS
            // policy. A prefix or UI/Default name is not a snapshot contract.
            switch (shader) {
                case "UI Ex/Widget Alpha": case "UI Ex/Widget Additive":
                case "UI Ex/Text Alpha": case "UI Ex/Text Additive":
                case "UI Ex/Text Alpha YCGen(IconSet)": return true;
                default: return false;
            }
        }
        public void BeforeCull(Camera value)
        {
            if (closed || !capture.Active || !value || value != GameCamera.main || current != null) return;
            if (!WorldUiResolvePatch.Supported) { capture.Unsupported("The once-resolved world UI boundary is not verified"); return; }
            if (!presentation.World.OwnsFrameCamera(value)) return;
            if (canvasUpdateFrame != Time.frameCount) { capture.Unsupported("World UI has no completed Canvas rebuild for this application frame"); return; }
            camera = value;
            try {
                current = Acquire();
                var root = UIRoot.instance;
                if (!root) throw new InvalidOperationException("Current world UI root is unavailable");
                var seen = new HashSet<CanvasRenderer>();
                var descriptors = new Dictionary<Texture2D, AtlasDescriptor>();
                var bindings = new List<TextureBinding>();
                using (var commands = new CommandBuffer { name = "DSPAASR immutable world UI atlas snapshot" }) {
                    foreach (var canvas in root.GetComponentsInChildren<Canvas>(true)) {
                        if (!canvas || !canvas.enabled || !canvas.gameObject.activeInHierarchy || canvas.renderMode != RenderMode.WorldSpace) continue;
                        foreach (var graphic in canvas.GetComponentsInChildren<Graphic>(true)) {
                            if (!graphic || !graphic.isActiveAndEnabled || graphic.canvas != canvas ||
                                (value.cullingMask & (1 << graphic.gameObject.layer)) == 0) continue;
                            var renderer = graphic.canvasRenderer;
                            if (!renderer || renderer.cull || !seen.Add(renderer)) continue;
                            if (renderer.hasRectClipping || renderer.materialCount != 1 || renderer.popMaterialCount != 0)
                                throw new InvalidOperationException("World UI requires an unsupported clipping/pop-material snapshot");
                            var mesh = renderer.GetMesh();
                            if (!mesh || mesh.vertexCount == 0) continue;
                            var original = renderer.GetMaterial(0);
                            if (!original || !original.shader || original.passCount != 1 || mesh.subMeshCount != 1 ||
                                mesh.GetTopology(0) != MeshTopology.Triangles ||
                                !SupportedMaterial(original.shader.name))
                                throw new InvalidOperationException("World UI uses an unverified unlit mesh/material family");
                            if ((original.HasProperty("_Stencil") && original.GetInt("_Stencil") != 0) ||
                                (original.HasProperty("_StencilOp") && original.GetInt("_StencilOp") != 0) ||
                                (original.HasProperty("_StencilComp") && original.GetInt("_StencilComp") != (int)CompareFunction.Always))
                                throw new InvalidOperationException("World UI stencil state requires a dedicated snapshot contract");
                            if (current.Items.Count >= 128) throw new InvalidOperationException("World UI mesh snapshot budget exceeded");
                            var item = new Item { Renderer = renderer, Mesh = Object.Instantiate(mesh),
                                Material = new Material(original), Transform = graphic.transform.localToWorldMatrix,
                                LayerOrder = SortingLayer.GetLayerValueFromID(canvas.sortingLayerID), CanvasOrder = canvas.sortingOrder, Depth = renderer.absoluteDepth };
                            current.Items.Add(item); item.Mesh.hideFlags = item.Material.hideFlags = HideFlags.HideAndDontSave;
                            foreach (string property in original.GetTexturePropertyNames()) {
                                if (property == "_MainTex") continue; // Graphic supplies the effective atlas separately.
                                var texture = original.GetTexture(property);
                                if (texture) StageTexture(descriptors, bindings, item.Material, property, texture);
                            }
                            StageTexture(descriptors, bindings, item.Material, "_MainTex", graphic.mainTexture);
                            var tint = renderer.GetColor(); float alpha = renderer.GetInheritedAlpha();
                            if (float.IsNaN(alpha) || float.IsInfinity(alpha) || alpha < 0 || alpha > 1)
                                throw new InvalidOperationException("World UI inherited alpha is outside its runtime contract");
                            var color = original.HasProperty("_Color") ? original.GetColor("_Color") : Color.white;
                            color.r *= tint.r; color.g *= tint.g; color.b *= tint.b; color.a *= alpha;
                            item.Material.SetColor("_Color", color);
                            // The actual world canvas uses depth testing. A private
                            // native-size depth attachment is prepared before replay.
                            item.Material.SetInt("unity_GUIZTestMode", (int)CompareFunction.LessEqual);
                        }
                    }
                    current.Items.Sort((a,b) => { int order = a.LayerOrder.CompareTo(b.LayerOrder); if (order == 0) order = a.CanvasOrder.CompareTo(b.CanvasOrder); return order != 0 ? order : a.Depth.CompareTo(b.Depth); });
                    // Until the prepared raw/resolved transaction is wired, do
                    // not hide real geometry which lacks a guaranteed same-frame
                    // route. Empty-world-UI frames can still exercise full capture.
                    if (current.Items.Count != 0)
                        throw new InvalidOperationException("World UI mandatory raw/resolved replay is not prepared; original geometry retained");
                    PrepareAtlases(current, descriptors, commands);
                    foreach (var binding in bindings) binding.Material.SetTexture(binding.Property, current.Textures[binding.Source].Texture);
                    current.Frame = presentation.ApplicationFrameId;
                    Graphics.ExecuteCommandBuffer(commands);
                }
                snapshotReady = true;
                foreach (var item in current.Items) if (!visibility.Suppress(item.Renderer))
                    throw new InvalidOperationException("World UI visibility changed during its capture transaction");
                suppressed = true;
            } catch (Exception error) {
                visibility.Restore(); snapshotReady = suppressed = false; capture.Unsupported(error.Message);
            }
        }
        private void EnsureDepth(int width, int height)
        {
            // Acquire only returns a GPU-retired slot. Its depth attachment has
            // the same lifetime as the queued meshes/atlases, including quarantine.
            if (current.Depth && current.Depth.IsCreated() && current.Depth.width == width && current.Depth.height == height) return;
            if (current.DepthPointer != IntPtr.Zero) Marshal.Release(current.DepthPointer);
            current.DepthPointer = IntPtr.Zero;
            if (current.Depth) { current.Depth.Release(); Object.Destroy(current.Depth); }
            current.Depth = new RenderTexture(width,height,32,RenderTextureFormat.Depth,RenderTextureReadWrite.Linear) {
                name = "DSPAASR native world UI depth", hideFlags = HideFlags.HideAndDontSave,
                antiAliasing = 1, useMipMap = false, autoGenerateMips = false, filterMode = FilterMode.Point
            };
            if (!current.Depth.Create()) throw new InvalidOperationException("Cannot create native-size world UI depth");
            current.DepthPointer = current.Depth.GetNativeDepthBufferPtr();
            if (current.DepthPointer == IntPtr.Zero) throw new InvalidOperationException("Native world UI depth has no device resource");
            Marshal.AddRef(current.DepthPointer); current.DepthEpoch = ++nextDepthEpoch;
        }
        private void CaptureFailed(Exception error)
        {
            // An Abort/reporting failure is also optional bookkeeping; it must
            // not suppress the original mesh submission or replace its failure.
            try { capture.Unsupported(error.Message); } catch { }
        }
        private void SeedCapture(RenderTexture destination)
        {
            try { if (capture.Active) capture.SeedResolved(destination, true); }
            catch (Exception error) { CaptureFailed(error); }
        }
        private bool BeginUiCapture()
        {
            try {
                if (!capture.Active) return false;
                capture.BeginPass(CaptureScopeKind.FullOnlyUiCoverage, 1, "Current rebuilt world-space Canvas meshes and original unlit materials", false);
                return true;
            } catch (Exception error) { CaptureFailed(error); return false; }
        }
        private void EndUiCapture(bool entered)
        {
            if (!entered) return;
            try { capture.EndPass(); } catch (Exception error) { CaptureFailed(error); }
        }
        public void Resolved(RenderTexture destination)
        {
            if (!snapshotReady || !suppressed || drawn || !camera || current == null) return;
            visibility.Restore();
            try {
                SeedCapture(destination);
                if (current.Items.Count == 0) { drawn = true; return; }
                if (!destination) throw new InvalidOperationException("World UI resolve requires an explicit color target");
                EnsureDepth(destination.width,destination.height);
                capture.CopyWorldDepth(current.DepthPointer,current.DepthEpoch);
                // Only capture annotations are optional here. Mandatory-depth
                // failure or a missing resolve route STILL needs same-frame recovery.
                bool entered = BeginUiCapture();
                try {
                    using (var commands = new CommandBuffer { name = "DSPAASR resolved world UI meshes" }) {
                        commands.SetRenderTarget(destination,current.Depth);
                        commands.SetViewport(new Rect(0,0,destination.width,destination.height));
                        commands.SetViewProjectionMatrices(camera.worldToCameraMatrix,presentation.World.UnjitteredProjection);
                        foreach (var item in current.Items) commands.DrawMesh(item.Mesh,item.Transform,item.Material,0,0);
                        commands.SetViewProjectionMatrices(camera.worldToCameraMatrix,camera.projectionMatrix);
                        Graphics.ExecuteCommandBuffer(commands);
                    }
                    drawn = true;
                } finally { EndUiCapture(entered); }
            } catch (Exception error) { CaptureFailed(error); }
        }
        public void EndFrame()
        {
            visibility.Restore();
            if (suppressed && !drawn && current != null && current.Items.Count != 0)
                capture.Unsupported("Suppressed world UI did not reach the once-resolved color boundary");
        }
        public void Dispose()
        {
            if (closed) return; closed = true; visibility.Restore();
            foreach (var slot in slots) {
                if (slot.Frame <= capture.Status.CompletedUnityFrame) slot.Dispose();
                else { quarantine.Add(slot); warning("World UI snapshot retained because its final GPU fence was not observed during shutdown"); }
            }
            slots.Clear();
        }
    }
}
