using System;
using UnityEngine;
using UnityEngine.PostProcessing;
using UnityEngine.Rendering;

namespace DSPAAMod.Game
{
    // A navigation-only light source. Its alpha is deliberately never used for
    // composition: the game's text shader does not accumulate coverage alpha.
    internal sealed class NavigationBloom : IDisposable
    {
        private const string BloomShader = "Hidden/Post FX/Bloom";
        private const string UberShader = "Hidden/Post FX/Uber Shader";
        private static readonly int AutoExposure = Shader.PropertyToID("_AutoExposure");
        private static readonly int MainTexture = Shader.PropertyToID("_MainTex");
        private static readonly int MainTextureScale = Shader.PropertyToID("_MainTex_ST");
        private static readonly int MainTextureTexelSize = Shader.PropertyToID("_MainTex_TexelSize");
        private readonly BloomComponent bloom = new BloomComponent();
        private readonly BloomModel model = new BloomModel();
        private readonly PostProcessingContext context = new PostProcessingContext();
        private MaterialFactory materials;
        private RenderTextureFactory temporary;
        private Material uber;
        private CommandBuffer composite;
        private MaterialPropertyBlock compositeProperties;
        private Mesh quad; // Borrowed from the game's postprocessing utilities; never destroyed here.
        private RenderTexture source, finalCopy;
        private Camera camera;
        private PostProcessingProfile profile;
        private BloomModel.Settings settings;
        private Texture exposure;
        private int preparedFrame = -2, observedFrame = -1, width, height;
        private string failure;
        public RenderTexture Source => source;
        public bool HasWorldBloom => camera && preparedFrame == Time.frameCount && observedFrame == Time.frameCount;

        // Called before suppressing any navigation draw, while the main camera
        // still has its logical output dimensions rather than an SR input target.
        public string Preflight(Camera current)
        {
            var behaviour = current.GetComponent<PostProcessingBehaviour>();
            var nextProfile = behaviour && behaviour.isActiveAndEnabled ? behaviour.profile : null;
            int w = current.pixelWidth, h = current.pixelHeight;
            if (camera != current || profile != nextProfile || width != w || height != h)
            {
                ReleaseResources();
                failure = null;
            }
            camera = current; profile = nextProfile; width = w; height = h;
            preparedFrame = Time.frameCount; observedFrame = -1; exposure = null;
            if (!profile || profile.debugViews.willInterrupt || !profile.bloom.enabled || profile.bloom.settings.bloom.intensity <= 0)
            {
                ReleaseResources();
                return null;
            }
            if (failure != null) return "Navigation bloom is unavailable: " + failure;
            try
            {
                if (w < 4 || h < 4 || !SystemInfo.SupportsRenderTextureFormat(RenderTextureFormat.ARGBHalf))
                    throw new NotSupportedException("The navigation HDR target is unsupported.");
                if (materials == null) materials = new MaterialFactory();
                var filter = materials.Get(BloomShader);
                uber = materials.Get(UberShader);
                if (!filter.shader.isSupported || filter.passCount < 4 || !uber.shader.isSupported || uber.passCount < 1)
                    throw new NotSupportedException("The game's navigation bloom shaders are unsupported.");
                if (temporary == null) temporary = new RenderTextureFactory();
                if (composite == null) composite = new CommandBuffer { name = "DSPAASR navigation bloom composition" };
                if (compositeProperties == null) compositeProperties = new MaterialPropertyBlock();
                if (!quad) quad = GraphicsUtils.quad;
                if (!quad) throw new NotSupportedException("The game's postprocessing screen quad is unavailable.");
                if (!source || !source.IsCreated())
                {
                    Destroy(source);
                    source = Create("DSPAASR navigation HDR", w, h, 24, RenderTextureFormat.ARGBHalf, RenderTextureReadWrite.Linear);
                    source.filterMode = FilterMode.Bilinear; // Match the game's Bloom prefilter sampling.
                }
                if (!finalCopy || !finalCopy.IsCreated())
                {
                    Destroy(finalCopy);
                    finalCopy = Create("DSPAASR navigation final copy", w, h, 0, RenderTextureFormat.ARGB32, RenderTextureReadWrite.Default);
                }
                return null;
            }
            catch (Exception error)
            {
                failure = error.Message;
                ReleaseResources();
                return "Navigation bloom is unavailable: " + failure;
            }
        }

        // Observe the original world's successful call, not another camera or
        // this helper's own Prepare. Eye adaptation/history is never advanced here.
        public void Observe(BloomComponent original, Texture autoExposure)
        {
            if (!camera || preparedFrame != Time.frameCount || original.context == null || original.context.camera != camera) return;
            settings = original.model.settings;
            exposure = autoExposure;
            observedFrame = Time.frameCount;
        }

        public CommandBuffer PrepareComposite(Camera renderer)
        {
            if (!HasWorldBloom || !exposure || !source || !source.IsCreated() || !finalCopy || !finalCopy.IsCreated() ||
                materials == null || temporary == null || !uber || composite == null || compositeProperties == null || !quad ||
                renderer.pixelWidth != width || renderer.pixelHeight != height)
                throw new InvalidOperationException("Navigation bloom lost its same-frame HDR source, exposure or output dimensions.");
            context.Reset();
            context.camera = renderer;
            context.profile = profile;
            context.materialFactory = materials;
            context.renderTextureFactory = temporary;
            model.enabled = true;
            model.settings = settings;
            bloom.Init(context, model);
            uber.shaderKeywords = null;
            // Only the navigation prefilter sees world exposure. The final base
            // has already been exposed/graded; multiplying it again is incorrect.
            uber.SetTexture(AutoExposure, Texture2D.whiteTexture);
            if (QualitySettings.activeColorSpace != ColorSpace.Linear) uber.EnableKeyword("UNITY_COLORSPACE_GAMMA");
            bloom.Prepare(source, uber, exposure);
            composite.Clear();
            // Execute inside the manual camera, where CameraTarget has an owner.
            // Never sample and render to the same texture or copy a guessed D-size
            // swapchain while Unity is still composing its logical L-size target.
            composite.Blit(BuiltinRenderTextureType.CameraTarget, finalCopy);
            // A screen copy and a camera-rendered HDR texture have opposite Y
            // conventions on top-origin APIs. The game's Uber uses the texel-size
            // sign for its Bloom UV, independently of its base-color UV. Bind both
            // explicitly; Blit's implicit fullscreen geometry/properties are not
            // the image-effect input contract expected by this shader.
            float screenY = SystemInfo.graphicsUVStartsAtTop ? -1f : 1f;
            compositeProperties.Clear();
            compositeProperties.SetTexture(MainTexture, finalCopy);
            compositeProperties.SetVector(MainTextureScale, new Vector4(1, 1, 0, 0));
            compositeProperties.SetVector(MainTextureTexelSize, new Vector4(1f / width, screenY / height, width, height));
            composite.SetRenderTarget(BuiltinRenderTextureType.CameraTarget);
            composite.SetViewProjectionMatrices(Matrix4x4.identity, Matrix4x4.identity);
            composite.DrawMesh(quad, Matrix4x4.Scale(new Vector3(1, screenY, 1)), uber, 0, 0, compositeProperties);
            composite.SetViewProjectionMatrices(renderer.worldToCameraMatrix, renderer.projectionMatrix);
            composite.SetViewport(renderer.pixelRect);
            return composite;
        }

        public void Failed(Exception error) => failure = error.Message;
        public void EndFrame()
        {
            composite?.Clear();
            temporary?.ReleaseAll();
            context.Reset();
            exposure = null; observedFrame = -1;
        }
        public void ReleaseIfUnused(bool requested)
        {
            if (requested && camera && preparedFrame + 1 >= Time.frameCount) return;
            Dispose();
        }
        private static RenderTexture Create(string name, int w, int h, int depth, RenderTextureFormat format, RenderTextureReadWrite readWrite)
        {
            var texture = new RenderTexture(w, h, depth, format, readWrite)
            {
                name = name, hideFlags = HideFlags.HideAndDontSave, antiAliasing = 1,
                useMipMap = false, autoGenerateMips = false, filterMode = FilterMode.Point, wrapMode = TextureWrapMode.Clamp
            };
            try
            {
                if (!texture.Create()) throw new InvalidOperationException("Cannot allocate " + name + ".");
                return texture;
            }
            catch { Destroy(texture); throw; }
        }
        private static void Destroy(RenderTexture texture)
        {
            if (!texture) return;
            texture.Release(); UnityEngine.Object.Destroy(texture);
        }
        private void ReleaseResources()
        {
            EndFrame();
            composite?.Release(); composite = null;
            compositeProperties?.Clear(); compositeProperties = null; quad = null;
            temporary?.Dispose(); temporary = null;
            materials?.Dispose(); materials = null; uber = null;
            Destroy(source); source = null;
            Destroy(finalCopy); finalCopy = null;
        }
        public void Dispose()
        {
            ReleaseResources();
            camera = null; profile = null; failure = null;
            preparedFrame = -2; observedFrame = -1; width = height = 0;
        }
    }
}
