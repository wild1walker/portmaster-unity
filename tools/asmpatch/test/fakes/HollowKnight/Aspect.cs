// A stand-in for the part of Hollow Knight's ForceCameraAspect that hk-aspect looks at (written for the
// tests; no game code): the viewport fitted to the screen's aspect, clamped from 16:10 to 21:9.
public struct MinMaxFloat
{
    public float Start, End;
    public MinMaxFloat(float start, float end) { Start = start; End = end; }
    public float GetClampedBetween(float v) { return v < Start ? Start : v > End ? End : v; }
}

public class ForceCameraAspect
{
    public static float ScreenAspect = 16f / 9f;

    public static float AutoScaleViewportShared(float scaleAdjust, out float width, out float height, out float heightMult)
    {
        float num = ScreenAspect;
        float clamped = new MinMaxFloat(1.6f, 2.3916667f).GetClampedBetween(num);
        float r = num / clamped;
        float s = 1f + scaleAdjust;
        if (r < 1f) { width = s; height = r * s; }
        else { width = 1f / r * s; height = s; }
        heightMult = clamped < 1.7777778f ? 1.7777778f / clamped : 1f;
        return clamped;
    }
}
