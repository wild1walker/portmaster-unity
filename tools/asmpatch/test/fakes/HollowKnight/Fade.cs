// A stand-in for Hollow Knight's CameraFade for the hk-camerafade test (written for the tests; no game
// code): the members the patch looks at, in the shapes it expects (a private 1x1 texture made in
// Awake and given to a private style, SetScreenOverlayColor as the store, SetPixel and Apply, an
// OnGUI that steps the colour per event and draws it while visible), and a driver that runs a
// seeded sequence of events, fades and direct sets and logs every observable after each step.
using System;
using System.Text;
using UnityEngine;

public class CameraFade : MonoBehaviour
{
    private GUIStyle backgroundStyle = new GUIStyle();
    private Texture2D fadeTexture;
    private Color currentScreenOverlayColor = new Color(0f, 0f, 0f, 0f);
    private Color targetScreenOverlayColor = new Color(0f, 0f, 0f, 1f);
    private Color deltaColor = new Color(0f, 0f, 0f, 0f);
    private int fadeGUIDepth = -1000;

    public void Awake()
    {
        fadeTexture = new Texture2D(1, 1);
        backgroundStyle.normal.background = fadeTexture;
    }

    public void OnGUI()
    {
        if (currentScreenOverlayColor != targetScreenOverlayColor)
        {
            if (Math.Abs(currentScreenOverlayColor.a - targetScreenOverlayColor.a) < Math.Abs(deltaColor.a) * Time.deltaTime)
            {
                currentScreenOverlayColor = targetScreenOverlayColor;
                SetScreenOverlayColor(currentScreenOverlayColor);
                deltaColor = new Color(0f, 0f, 0f, 0f);
            }
            else
                SetScreenOverlayColor(currentScreenOverlayColor + deltaColor * Time.deltaTime);
        }
        if (currentScreenOverlayColor.a > 0f)
        {
            GUI.depth = fadeGUIDepth;
            GUI.Label(new Rect(-10f, -10f, Screen.width + 10, Screen.height + 10), fadeTexture, backgroundStyle);
        }
    }

    public void SetScreenOverlayColor(Color newScreenOverlayColor)
    {
        currentScreenOverlayColor = newScreenOverlayColor;
        fadeTexture.SetPixel(0, 0, currentScreenOverlayColor);
        fadeTexture.Apply();
    }

    public void StartFade(Color newScreenOverlayColor, float fadeDuration)
    {
        if (fadeDuration <= 0f)
        {
            SetScreenOverlayColor(newScreenOverlayColor);
            return;
        }
        targetScreenOverlayColor = newScreenOverlayColor;
        deltaColor = (targetScreenOverlayColor - currentScreenOverlayColor) / (fadeDuration * 2f);
    }

    public void FadeToBlack(float duration)
    {
        SetScreenOverlayColor(new Color(0f, 0f, 0f, 0f));
        StartFade(new Color(0f, 0f, 0f, 1f), duration);
    }

    public void FadeToTransparent(float duration)
    {
        SetScreenOverlayColor(new Color(0f, 0f, 0f, 1f));
        StartFade(new Color(0f, 0f, 0f, 0f), duration);
    }
}

public static class CameraFadeDriver
{
    // the fields read from outside (CameraFade itself must not read its texture but to draw it)
    static object F(CameraFade f, string name) =>
        typeof(CameraFade).GetField(name, System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance).GetValue(f);

    static string State(CameraFade f)
    {
        var t = (Texture2D)F(f, "fadeTexture");
        return $"cur {F(f, "currentScreenOverlayColor")} tgt {F(f, "targetScreenOverlayColor")} d {F(f, "deltaColor")} " +
               $"cpu {string.Join(",", t?.Cpu ?? new byte[0])} gpu {string.Join(",", t?.Gpu ?? new byte[0])}";
    }

    static Color Pick(Random r)
    {
        switch (r.Next(8))
        {
            case 0: return new Color(0f, 0f, 0f, 0f);
            case 1: return new Color(0f, 0f, 0f, 1f);
            case 2: return new Color(-0f, 0f, -0f, -0f);
            case 3: return new Color(float.NaN, 0f, 0f, 0.5f);
            case 4: return new Color(1f, 1f, 1f, 0.5f);
            default: return new Color((float)r.NextDouble(), (float)r.NextDouble(), (float)r.NextDouble(), (float)r.NextDouble());
        }
    }

    /// <summary>A CameraFade nothing fades (the game's usual one): Awake, then OnGUI events; its state after.</summary>
    public static string Idle(int events)
    {
        var f = new CameraFade();
        GUI.Log.Clear();
        f.Awake();
        for (int i = 0; i < events; i++) f.OnGUI();
        var s = State(f) + " labels " + GUI.Log.Count;
        GUI.Log.Clear();
        return s;
    }

    /// <summary>The log of a seeded run: each step's action and every value the game could see after it.</summary>
    public static string Run(int seed, int steps)
    {
        var r = new Random(seed);
        var f = new CameraFade();
        var log = new StringBuilder();
        GUI.Log.Clear();
        bool awake = r.Next(10) != 0;                          // now and then an event before Awake: the exception
        if (awake) f.Awake();
        for (int i = 0; i < steps; i++)
        {
            int k = r.Next(100);
            string what;
            Time.deltaTime = r.Next(4) == 0 ? 0f : (float)(r.NextDouble() / 20);
            try
            {
                if (k < 70) { what = "gui"; f.OnGUI(); }
                else if (k < 78) { var c = Pick(r); float d = r.Next(3) == 0 ? 0f : (float)r.NextDouble() * 2; what = $"fade {c} {d:R}"; f.StartFade(c, d); }
                else if (k < 83) { what = "black"; f.FadeToBlack((float)r.NextDouble()); }
                else if (k < 88) { what = "clear"; f.FadeToTransparent((float)r.NextDouble()); }
                else if (k < 97) { var c = Pick(r); what = $"set {c}"; f.SetScreenOverlayColor(c); }
                else { what = "awake"; f.Awake(); }
            }
            catch (Exception e) { what = "threw " + e.GetType().Name; }
            log.Append(i).Append(' ').Append(what).Append(": ").Append(State(f)).Append(" labels ").Append(string.Join("|", GUI.Log)).Append('\n');
            GUI.Log.Clear();
        }
        return log.ToString();
    }
}
