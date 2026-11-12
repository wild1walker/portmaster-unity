// A stand-in for the members of Hollow Knight's blurred-background classes that hk-blur looks at
// (written for the tests; no game code): the camera rig's start, the texture's size at an aspect
// change, a blur plane's renderer, the two cameras' clip planes at the closest blur plane.
using System;

public class MeshRenderer
{
    public bool enabled { get; set; } = true;
}

public class Transform
{
    public float Z;
}

public static class Extensions
{
    public static float GetPositionZ(this Transform t) { return t.Z; }
}

public class Camera
{
    private readonly Transform t = new Transform();

    public float farClipPlane { get; set; } = 1000f;
    public float nearClipPlane { get; set; } = 0.25f;
    public Transform transform { get { return t; } }
}

public class LightBlurredBackground
{
    private float distantFarClipPlane;
    private int renderTextureHeight = 360;
    private float clipEpsilon = 0.5f;
    private Camera sceneCamera;
    private Camera backgroundCamera;
    private bool cancelEnable;

    public bool NotTheInstance;
    public int TextureWidth, TextureHeight;
    public bool Cancelled { get { return cancelEnable; } }
    public Camera SceneCamera { get { return sceneCamera; } }
    public Camera BackgroundCamera { get { return backgroundCamera; } }

    protected void Awake()
    {
        sceneCamera = new Camera();
        sceneCamera.transform.Z = -38f;
        if (NotTheInstance)
        {
            cancelEnable = true;
        }
    }

    protected void OnEnable()
    {
        if (!cancelEnable)
        {
            distantFarClipPlane = sceneCamera.farClipPlane;
            backgroundCamera = new Camera();
            backgroundCamera.transform.Z = sceneCamera.transform.Z;
            UpdateCameraClipPlanes();
        }
    }

    protected void OnDisable()
    {
        if (!cancelEnable)
        {
            sceneCamera.farClipPlane = distantFarClipPlane;
            backgroundCamera = null;
        }
    }

    protected void LateUpdate()
    {
        if (!cancelEnable)
        {
            UpdateCameraClipPlanes();
        }
    }

    private void UpdateCameraClipPlanes()
    {
        BlurPlane closestBlurPlane = BlurPlane.ClosestBlurPlane;
        if (closestBlurPlane != null)
        {
            sceneCamera.farClipPlane = closestBlurPlane.PlaneZ - sceneCamera.transform.GetPositionZ() + clipEpsilon;
            backgroundCamera.nearClipPlane = closestBlurPlane.PlaneZ - backgroundCamera.transform.GetPositionZ() + clipEpsilon;
        }
        else
        {
            sceneCamera.farClipPlane = distantFarClipPlane;
            backgroundCamera.nearClipPlane = distantFarClipPlane;
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

    public void Start(float aspect) { Awake(); OnEnable(); OnCameraAspectChanged(aspect); }
    public void Frame() { LateUpdate(); }
    public void Stop() { OnDisable(); }
}

public class LightBlur
{
    private int passGroupCount = 2;

    public int PassGroupCount { get { return passGroupCount; } set { passGroupCount = value; } }
    public int BlurPassCount { get { return passGroupCount * 2; } }
}

public class BlurPlane
{
    public static BlurPlane Closest;
    public float Z;

    private MeshRenderer meshRenderer;

    public MeshRenderer Renderer { get { return meshRenderer; } }
    public static BlurPlane ClosestBlurPlane { get { return Closest; } }
    public float PlaneZ { get { return Z; } }

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
