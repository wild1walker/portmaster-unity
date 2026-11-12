// Stand-ins for the parts of Hollow Knight's scaled rendering that hk-render-size looks at (written for
// the tests; no game code): the size the world is drawn at, kept in a static (0x0: the screen's) beside
// another static with an initializer, the method that puts it back to 0x0, and the scene change that
// asks for the screen's size in a menu scene.
using System.Collections.Generic;

public struct ScreenRes
{
    public int Width { get; private set; }
    public int Height { get; private set; }
    public ScreenRes(int width, int height) { Width = width; Height = height; }
}

public class CameraRenderScaled
{
    public static ScreenRes Resolution;
    private static HashSet<object> fullResolutionSources = new HashSet<object>();

    public static int Sources { get { return fullResolutionSources.Count; } }

    public bool ForceFullResolution { get; set; }

    public static void Clear()
    {
        Resolution = new ScreenRes(0, 0);
    }

    public void OnActiveSceneChanged(string oldScene, string newScene)
    {
        bool full = newScene == "Menu";
        fullResolutionSources.RemoveWhere(o => o == null);
        ForceFullResolution = full;
    }
}
