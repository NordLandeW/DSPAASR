using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Threading.Tasks;
using System.Xml;
using DSPAAMod.Interop;
using UnityEngine;
using UnityEngine.Experimental.Rendering;
using UnityEngine.Rendering;

namespace DSPAAMod.Game
{
    // Opt-in diagnostics only. Call Enqueue after ExecuteCommandBuffer, and do not
    // dispose a retired RenderTargets while IsBusy returns true. No render inputs
    // are changed. Readback success does not assert that NGX evaluated the frame.
    internal sealed class FrameCapture
    {
        private sealed class Resource
        {
            public string Name, File, Error, Hash;
            public int Width, Height;
            public readonly Dictionary<string, string> Metadata = new Dictionary<string, string>();
            public byte[] Bytes;
            public bool Claimed;
        }
        private sealed class Frame
        {
            public NativeFrame Native;
            public int UnityFrame;
            public readonly Dictionary<string, string> Metadata = new Dictionary<string, string>();
            public readonly List<Resource> Resources = new List<Resource>();
        }
        private sealed class Group
        {
            public string Id, Directory;
            public RenderTargets Targets;
            public readonly List<Frame> Frames = new List<Frame>();
            public readonly List<string> Errors = new List<string>();
            public int Pending;
            public bool Sealed, Writing;
        }

        private readonly string directory;
        private readonly Action<string> log;
        private readonly object gate = new object();
        private readonly Dictionary<RenderTargets, int> pending = new Dictionary<RenderTargets, int>();
        private Group active;

        public FrameCapture(string directory, Action<string> log)
        {
            this.directory = directory;
            this.log = log;
        }

        // Main-thread entry points. A group remains active through disk completion,
        // bounding memory to two frames and rejecting overlapping requests.
        public bool Request()
        {
            lock (gate)
            {
                if (active != null)
                {
                    Log("Frame capture request ignored: a capture is already armed or in progress.");
                    return false;
                }
                try
                {
                    string id = DateTime.UtcNow.ToString("yyyyMMdd-HHmmss-fff", CultureInfo.InvariantCulture) + "-" + Guid.NewGuid().ToString("N");
                    var group = new Group { Id = id, Directory = Path.Combine(directory, id) };
                    if (Directory.Exists(group.Directory)) throw new IOException("Capture directory already exists.");
                    Directory.CreateDirectory(group.Directory);
                    WriteDocument(Path.Combine(group.Directory, "request.xml"), "captureRequest", writer =>
                    {
                        Value(writer, "id", id);
                        Value(writer, "requestedUtc", DateTime.UtcNow.ToString("O", CultureInfo.InvariantCulture));
                        Value(writer, "frameCount", 2);
                        Value(writer, "state", "armed; result.xml is the terminal record");
                    });
                    active = group;
                    if (!SystemInfo.supportsAsyncGPUReadback)
                    {
                        Fail(group, "AsyncGPUReadback is unsupported on this device.");
                        FinishIfReady(group);
                    }
                    else Log("Frame capture armed for two consecutive submissions from the first captured camera: " + group.Directory);
                    return true;
                }
                catch (Exception error)
                {
                    Log("Cannot arm frame capture: " + error.Message);
                    if (active != null)
                    {
                        Fail(active, "Cannot finish arming capture: " + error.Message);
                        FinishIfReady(active);
                    }
                    return false;
                }
            }
        }

        public bool IsBusy(RenderTargets targets)
        {
            lock (gate) return targets != null && pending.TryGetValue(targets, out int count) && count != 0;
        }

        // Cancellation closes an incomplete group, not its GPU requests. Call on
        // shutdown/disable; the target overload is safe for unrelated retirements.
        public void Cancel(string reason)
        {
            lock (gate)
            {
                if (active == null || active.Sealed) return;
                Fail(active, "Capture cancelled: " + reason);
                FinishIfReady(active);
            }
        }
        public void Cancel(RenderTargets targets, string reason)
        {
            lock (gate)
            {
                if (active != null && ReferenceEquals(active.Targets, targets)) Cancel(reason);
            }
        }

        public void EnqueueIfRequested(NativeFrame native, RenderTargets targets, Camera camera, Matrix4x4 renderedProjection, int deferredWorldTextCount)
        {
            lock (gate)
            {
                Group group = active;
                if (group == null || group.Sealed) return;
                if (group.Frames.Count != 0)
                {
                    Frame first = group.Frames[0];
                    int unityFrame = Time.frameCount;
                    // Other cameras in either candidate time frame are not paired.
                    // Continued rendering without the selected next frame fails,
                    // rather than leaving an incomplete request armed forever.
                    if (native.Camera != first.Native.Camera && unityFrame <= first.UnityFrame + 1) return;
                    if (native.Camera != first.Native.Camera || !ReferenceEquals(targets, group.Targets) ||
                        native.Frame != first.Native.Frame + 1 || unityFrame != first.UnityFrame + 1)
                    {
                        Fail(group, "Expected the same camera/targets on consecutive native and Unity frames; got native " +
                            first.Native.Frame + " -> " + native.Frame + ", Unity " + first.UnityFrame + " -> " + unityFrame + ".");
                        FinishIfReady(group);
                        return;
                    }
                }
                try
                {
                    if (targets == null || camera == null) throw new InvalidOperationException("Capture has no live targets/camera.");
                    var frame = new Frame { Native = native };
                    SnapshotFrame(frame, camera, renderedProjection, deferredWorldTextCount);
                    RenderTexture[] textures = { targets.Color, targets.Depth, targets.Motion, targets.Output };
                    string[] names = { "color", "depth", "motion", "output" };
                    for (int i = 0; i < textures.Length; ++i)
                    {
                        RenderTexture texture = textures[i];
                        if (!texture || !texture.IsCreated()) throw new InvalidOperationException(names[i] + " texture is not created.");
                        if (!SystemInfo.IsFormatSupported(texture.graphicsFormat, FormatUsage.ReadPixels))
                            throw new NotSupportedException(names[i] + " format does not support unconverted readback: " + texture.graphicsFormat);
                        var resource = new Resource
                        {
                            Name = names[i], File = "frame-" + group.Frames.Count + "-" + names[i] + ".raw",
                            Width = texture.width, Height = texture.height
                        };
                        Add(resource.Metadata, "width", texture.width);
                        Add(resource.Metadata, "height", texture.height);
                        Add(resource.Metadata, "volumeDepth", texture.volumeDepth);
                        Add(resource.Metadata, "dimension", texture.dimension);
                        Add(resource.Metadata, "renderTextureFormat", texture.format);
                        Add(resource.Metadata, "graphicsFormat", texture.graphicsFormat);
                        Add(resource.Metadata, "graphicsFormatValue", (int)texture.graphicsFormat);
                        Add(resource.Metadata, "antiAliasing", texture.antiAliasing);
                        Add(resource.Metadata, "sRGB", texture.sRGB);
                        Add(resource.Metadata, "mip", 0);
                        if (texture.dimension != TextureDimension.Tex2D || texture.volumeDepth != 1)
                            throw new NotSupportedException("Capture expects single-layer 2D render textures.");
                        frame.Resources.Add(resource);
                    }
                    group.Targets = targets;
                    group.Frames.Add(frame);
                    group.Sealed = group.Frames.Count == 2;
                    // Reserve the whole frame before issuing requests: callbacks or
                    // immediate errors must not finish a partially enqueued group.
                    group.Pending += textures.Length;
                    pending.TryGetValue(targets, out int previous);
                    pending[targets] = previous + textures.Length;
                    for (int i = 0; i < textures.Length; ++i)
                    {
                        Resource resource = frame.Resources[i];
                        try
                        {
                            var request = AsyncGPUReadback.Request(textures[i], 0,
                                completed => Complete(group, targets, resource, completed, null));
                            // Failed requests may not produce a later callback.
                            if (request.hasError) Complete(group, targets, resource, request, "GPU readback request failed.");
                        }
                        catch (Exception error)
                        {
                            Complete(group, targets, resource, default, "Cannot submit readback: " + error.Message);
                        }
                    }
                }
                catch (Exception error)
                {
                    Fail(group, "Cannot capture submitted frame: " + error.Message);
                    FinishIfReady(group);
                }
            }
        }

        private void Complete(Group group, RenderTargets targets, Resource resource, AsyncGPUReadbackRequest request, string failure)
        {
            lock (gate)
            {
                if (resource.Claimed) return;
                resource.Claimed = true;
            }
            try
            {
                if (failure != null) throw new InvalidOperationException(failure);
                if (!request.done || request.hasError) throw new InvalidOperationException("GPU readback did not complete successfully.");
                Add(resource.Metadata, "readbackWidth", request.width);
                Add(resource.Metadata, "readbackHeight", request.height);
                Add(resource.Metadata, "readbackDepth", request.depth);
                Add(resource.Metadata, "layerCount", request.layerCount);
                Add(resource.Metadata, "layerDataSize", request.layerDataSize);
                if (request.layerCount != 1 || request.depth != 1 || request.width != resource.Width || request.height != resource.Height)
                    throw new InvalidOperationException("Readback dimensions do not match the snapshotted 2D texture.");
                var data = request.GetData<byte>();
                if (data.Length == 0) throw new InvalidOperationException("Readback returned no bytes.");
                resource.Bytes = new byte[data.Length];
                data.CopyTo(resource.Bytes); // Request-owned memory lives for one frame only.
            }
            catch (Exception error) { resource.Error = error.Message; }
            finally
            {
                lock (gate)
                {
                    if (resource.Error != null) Fail(group, resource.File + ": " + resource.Error);
                    --group.Pending;
                    if (--pending[targets] == 0) pending.Remove(targets);
                    FinishIfReady(group);
                }
            }
            // Never access a RenderTexture, Camera or request-owned data on the I/O worker.
        }

        private void FinishIfReady(Group group)
        {
            if (!group.Sealed || group.Pending != 0 || group.Writing) return;
            group.Writing = true;
            group.Targets = null; // Only copied bytes and plain metadata reach the worker.
            try { Task.Run(() => Persist(group)); }
            catch (Exception error)
            {
                Log("Cannot schedule capture persistence; request.xml remains incomplete: " + error.Message);
                if (ReferenceEquals(active, group)) active = null;
            }
        }

        private void Persist(Group group)
        {
            try
            {
                for (int i = 0; i < group.Frames.Count; ++i)
                {
                    Frame frame = group.Frames[i];
                    foreach (Resource resource in frame.Resources)
                    {
                        if (resource.Bytes == null) continue;
                        try
                        {
                            using (var stream = new FileStream(Path.Combine(group.Directory, resource.File), FileMode.CreateNew, FileAccess.Write, FileShare.Read))
                                stream.Write(resource.Bytes, 0, resource.Bytes.Length);
                            using (var hash = SHA256.Create()) resource.Hash = BitConverter.ToString(hash.ComputeHash(resource.Bytes)).Replace("-", "");
                            Add(resource.Metadata, "byteLength", resource.Bytes.Length);
                        }
                        catch (Exception error)
                        {
                            resource.Error = "Cannot persist raw data: " + error.Message;
                            group.Errors.Add(resource.File + ": " + resource.Error);
                        }
                        finally { resource.Bytes = null; }
                    }
                    try
                    {
                        WriteDocument(Path.Combine(group.Directory, "frame-" + i + ".xml"), "submittedFrame", writer =>
                        {
                            foreach (var pair in frame.Metadata) Value(writer, pair.Key, pair.Value);
                            foreach (Resource resource in frame.Resources)
                            {
                                writer.WriteStartElement("resource");
                                writer.WriteAttributeString("name", resource.Name);
                                Value(writer, "file", resource.File);
                                Value(writer, "success", resource.Error == null && resource.Hash != null);
                                Value(writer, "sha256", resource.Hash ?? "");
                                Value(writer, "error", resource.Error ?? "");
                                foreach (var pair in resource.Metadata) Value(writer, pair.Key, pair.Value);
                                writer.WriteEndElement();
                            }
                        });
                    }
                    catch (Exception error) { group.Errors.Add("Cannot persist frame metadata: " + error.Message); }
                }
                bool success = group.Frames.Count == 2 && group.Errors.Count == 0;
                WriteDocument(Path.Combine(group.Directory, "result.xml"), "captureResult", writer =>
                {
                    Value(writer, "id", group.Id);
                    Value(writer, "completedUtc", DateTime.UtcNow.ToString("O", CultureInfo.InvariantCulture));
                    Value(writer, "success", success);
                    Value(writer, "capturedFrames", group.Frames.Count);
                    Value(writer, "expectedFrames", 2);
                    Value(writer, "meaning", "Success means raw readback and persistence only; correlate NGX logs to verify evaluation/model.");
                    foreach (string error in group.Errors) Value(writer, "error", error);
                });
                Log("Frame capture " + (success ? "complete: " : "failed; see result.xml: ") + group.Directory);
                foreach (string error in group.Errors) Log("Frame capture: " + error);
            }
            catch (Exception error) { Log("Cannot write terminal capture result at " + group.Directory + ": " + error.Message); }
            finally
            {
                lock (gate) if (ReferenceEquals(active, group)) active = null;
            }
        }

        private static void SnapshotFrame(Frame frame, Camera camera, Matrix4x4 renderedProjection, int deferredWorldTextCount)
        {
            var data = frame.Metadata;
            NativeFrame native = frame.Native;
            Add(data, "capturedUtc", DateTime.UtcNow.ToString("O", CultureInfo.InvariantCulture));
            Add(data, "nativeAbiVersion", native.Version);
            Add(data, "nativeCameraId", native.Camera);
            Add(data, "nativeFrameId", native.Frame);
            frame.UnityFrame = Time.frameCount;
            Add(data, "unityFrameCount", frame.UnityFrame);
            Add(data, "unityTime", Time.time);
            Add(data, "unityUnscaledTime", Time.unscaledTime);
            Add(data, "unityRealtimeSinceStartup", Time.realtimeSinceStartup);
            Add(data, "unityUnscaledDeltaTime", Time.unscaledDeltaTime);
            Add(data, "deferredWorldTextCount", deferredWorldTextCount);
            Add(data, "worldTextObservation", "When deferredWorldTextCount is nonzero, navigation glyphs are excluded from these NGX inputs/output and drawn afterward into the temporal resolve destination.");
            Add(data, "cameraInstanceId", camera.GetInstanceID());
            Add(data, "cameraName", camera.name);
            Add(data, "inputWidth", native.Width);
            Add(data, "inputHeight", native.Height);
            Add(data, "outputWidth", native.OutputWidth);
            Add(data, "outputHeight", native.OutputHeight);
            Add(data, "requestedPresetValue", native.Preset);
            Add(data, "requestedPreset", native.Preset >= 1 && native.Preset <= 26 ? ((char)('A' + native.Preset - 1)).ToString() : "unknown");
            Add(data, "observedModel", "unknown; correlate the NGX log, not the requested preset");
            Add(data, "qualityValue", native.Quality);
            Add(data, "flags", native.Flags);
            Add(data, "reset", (native.Flags & 4) != 0);
            Add(data, "jitterX", native.JitterX);
            Add(data, "jitterY", native.JitterY);
            Add(data, "motionScaleX", native.MotionScaleX);
            Add(data, "motionScaleY", native.MotionScaleY);
            Add(data, "frameTimeMilliseconds", native.FrameTimeMilliseconds);
            Add(data, "cameraPosition", Vector(camera.transform.position));
            Quaternion rotation = camera.transform.rotation;
            Add(data, "cameraRotationXYZW", Join(rotation.x, rotation.y, rotation.z, rotation.w));
            Add(data, "worldToCameraMatrixRowMajor", Matrix(camera.worldToCameraMatrix));
            Add(data, "cameraToWorldMatrixRowMajor", Matrix(camera.cameraToWorldMatrix));
            Add(data, "projectionMatrixRowMajor", Matrix(camera.projectionMatrix));
            Add(data, "nonJitteredProjectionMatrixRowMajor", Matrix(camera.nonJitteredProjectionMatrix));
            Add(data, "renderProjectionMatrixRowMajor", Matrix(renderedProjection));
            Add(data, "gpuProjectionForRenderTextureRowMajor", Matrix(GL.GetGPUProjectionMatrix(renderedProjection, true)));
            Add(data, "projectionObservation", "Camera projection is snapshotted at submission after OnPostRender; renderProjection preserves the matrix used for this world draw.");
            Add(data, "rawEncoding", "Unconverted graphicsFormat bytes from mip0/layer0; no vertical flip or channel conversion.");
            Add(data, "littleEndianHost", BitConverter.IsLittleEndian);
        }
        private static string Vector(Vector3 value) => Join(value.x, value.y, value.z);
        private static string Join(params float[] values) => string.Join(" ", Array.ConvertAll(values, v => v.ToString("R", CultureInfo.InvariantCulture)));
        private static string Matrix(Matrix4x4 value)
        {
            var values = new float[16];
            for (int row = 0; row < 4; ++row)
                for (int column = 0; column < 4; ++column) values[row * 4 + column] = value[row, column];
            return Join(values);
        }
        private static string Format(object value)
        {
            if (value is bool boolean) return boolean ? "true" : "false";
            if (value is float single) return single.ToString("R", CultureInfo.InvariantCulture);
            if (value is double number) return number.ToString("R", CultureInfo.InvariantCulture);
            return Convert.ToString(value, CultureInfo.InvariantCulture);
        }
        private static void Add(IDictionary<string, string> data, string name, object value) => data.Add(name, Format(value));
        private static void Value(XmlWriter writer, string name, object value) => writer.WriteElementString(name, Format(value));
        private static void WriteDocument(string path, string root, Action<XmlWriter> write)
        {
            using (var stream = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.Read))
            using (var writer = XmlWriter.Create(stream, new XmlWriterSettings { Indent = true, Encoding = new UTF8Encoding(false) }))
            {
                writer.WriteStartDocument();
                writer.WriteStartElement(root);
                write(writer);
                writer.WriteEndElement();
                writer.WriteEndDocument();
            }
        }
        private void Fail(Group group, string error)
        {
            group.Errors.Add(error);
            group.Sealed = true;
            Log("Frame capture: " + error);
        }
        private void Log(string message)
        {
            try { log?.Invoke(message); }
            catch { /* A diagnostic sink must never interrupt rendering/retirement. */ }
        }
    }
}
