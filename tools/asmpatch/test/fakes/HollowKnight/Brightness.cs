// A stand-in for the members of Hollow Knight's post-processing chain that hk-post looks at in
// Assembly-CSharp (written for the tests; no game code): the brightness effect (one blit of its
// material, then the camera's texture display, which gets the source and the material) and that
// display. The engine's types are the firstpass stand-in's (UnityEngine.*; Blur.cs's own Material
// and RenderTexture are hk-blur's).
public class BrightnessEffect : UnityStandardAssets.ImageEffects.ImageEffectBase
{
    private static readonly int BRIGHTNESS = UnityEngine.Shader.PropertyToID("_Brightness");
    private static readonly int CONTRAST = UnityEngine.Shader.PropertyToID("_Contrast");
    public float _Brightness = 1f;
    public float _Contrast = 1f;

    public void OnRenderImage(UnityEngine.RenderTexture source, UnityEngine.RenderTexture destination)
    {
        material.SetFloat(BRIGHTNESS, _Brightness);
        material.SetFloat(CONTRAST, _Contrast);
        UnityEngine.Graphics.Blit(source, destination, material);
        if (GameCameraTextureDisplay.Instance != null)
            GameCameraTextureDisplay.Instance.UpdateDisplay(source, material);
    }
}

public class GameCameraTextureDisplay : UnityEngine.MonoBehaviour
{
    public static GameCameraTextureDisplay Instance;
    private UnityEngine.RenderTexture texture;

    public void UpdateDisplay(UnityEngine.RenderTexture source, UnityEngine.Material material)
    {
        if (activeInHierarchy)
        {
            if (texture == null)
                texture = new UnityEngine.RenderTexture { Name = "display" };
            UnityEngine.Graphics.Blit(source, Instance.texture, material);
        }
    }
}
