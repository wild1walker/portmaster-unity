// A stand-in for the members of Hollow Knight's blurred-background classes that hk-blur looks at
// (written for the tests; no game code): the camera rig's start, the texture's size at an aspect
// change, a blur plane's renderer.
using System;

public class MeshRenderer
{
    public bool enabled { get; set; } = true;
}

public class LightBlurredBackground
{
    private int renderTextureHeight = 360;
    private bool cancelEnable;

    public bool NotTheInstance;
    public int TextureWidth, TextureHeight;
    public bool Cancelled { get { return cancelEnable; } }

    protected void Awake()
    {
        if (NotTheInstance)
        {
            cancelEnable = true;
        }
    }

    private void OnCameraAspectChanged(float aspect)
    {
        if (aspect <= 0f) return;
        int num = (int)Math.Round(renderTextureHeight * aspect);
        if (num > 0)
        {
            TextureWidth = num;
            TextureHeight = renderTextureHeight;
        }
    }

    public void Start(float aspect) { Awake(); OnCameraAspectChanged(aspect); }
}

public class LightBlur
{
    private int passGroupCount = 2;

    public int PassGroupCount { get { return passGroupCount; } set { passGroupCount = value; } }
    public int BlurPassCount { get { return passGroupCount * 2; } }
}

public class BlurPlane
{
    private MeshRenderer meshRenderer;

    public MeshRenderer Renderer { get { return meshRenderer; } }

    protected void Awake()
    {
        meshRenderer = new MeshRenderer();
    }

    public void SetPlaneVisibility(bool isVisible)
    {
        meshRenderer.enabled = isVisible;
    }

    public void Start() { Awake(); }
}
