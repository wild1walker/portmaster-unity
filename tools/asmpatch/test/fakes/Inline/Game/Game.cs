// A stand-in game assembly for the inline test (written for the test): hot callers that use InlineLib's
// small methods in loops, a throwing index, and a call on a null receiver.
using InlineLib;

public static class Hot
{
    public static float Walk(Bag<Obj> objs, Bag<float> xs, float t)
    {
        float s = 0f;
        for (int i = 0; i < objs.Count; i++)
        {
            Obj o = objs[i];
            if (!o) continue;
            float x = xs[i % xs.Count];
            V v = new V(x, t * 3f) - new V(t, x * 0.5f);
            s += M.Clamp(v.Sqr - 1f, -50f, 50f) + M.Abs(x - t) + M.Lerp(s, x, t);
            s += M.Floor(x * 7.25f) + v.Scaled(t * x).y;
            s += o.GetValue();
        }
        return s;
    }
    public static float At(Bag<float> xs, int i) { return xs[i]; }
    public static int Value(Obj o) { return o.GetValue(); }
}
