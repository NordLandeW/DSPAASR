using UnityEngine;

namespace DSPAAMod.Game
{
    // Last image effect: only the world camera's render target is reduced. The
    // native-resolution postprocessing result is presented before overlay UI draws.
    [RequireComponent(typeof(Camera))]
    public sealed class SrPresentation : MonoBehaviour
    {
        private Camera camera;
        private void Awake() { camera = GetComponent<Camera>(); }
        private void OnRenderImage(RenderTexture source, RenderTexture destination)
        {
            if (Plugin.Instance == null || !Plugin.Instance.Renderer.Present(camera, source, destination))
                Graphics.Blit(source, destination);
        }
    }
}
