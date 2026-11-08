// A stand-in for the engine types Hollow Knight's CameraFade uses, for the hk-camerafade test (written
// for the tests; no engine code): a colour, a 1x1 texture with a CPU copy that SetPixel writes and a
// GPU copy that Apply uploads (both counted), IMGUI's style and label (each label logs the GPU pixel
// it would draw) and the frame time.
using System;
using System.Collections.Generic;

namespace UnityEngine
{
    public struct Color
    {
        public float r, g, b, a;
        public Color(float r, float g, float b, float a) { this.r = r; this.g = g; this.b = b; this.a = a; }
        public static Color operator +(Color x, Color y) => new Color(x.r + y.r, x.g + y.g, x.b + y.b, x.a + y.a);
        public static Color operator -(Color x, Color y) => new Color(x.r - y.r, x.g - y.g, x.b - y.b, x.a - y.a);
        public static Color operator *(Color x, float f) => new Color(x.r * f, x.g * f, x.b * f, x.a * f);
        public static Color operator /(Color x, float f) => new Color(x.r / f, x.g / f, x.b / f, x.a / f);
        // the engine's: through Vector4, equal within a squared distance of 1e-10
        public static bool operator ==(Color x, Color y)
        {
            var d = x - y;
            return d.r * d.r + d.g * d.g + d.b * d.b + d.a * d.a < 9.99999944E-11f;
        }
        public static bool operator !=(Color x, Color y) => !(x == y);
        public override bool Equals(object o) => o is Color c && this == c;
        public override int GetHashCode() => 0;
        public override string ToString() => FormattableString.Invariant($"({r:R},{g:R},{b:R},{a:R})");
    }

    public class Texture2D : Texture
    {
        public static int SetPixels, Applies;
        public byte[] Cpu = { 205, 205, 205, 205 }, Gpu = { 205, 205, 205, 205 };

        public Texture2D(int w, int h) { width = w; height = h; }

        static byte Q(float v) => (byte)Math.Round(Math.Clamp(float.IsNaN(v) ? 0f : v, 0f, 1f) * 255f);

        public void SetPixel(int x, int y, Color c)
        {
            SetPixels++;
            Cpu = new[] { Q(c.r), Q(c.g), Q(c.b), Q(c.a) };
        }

        public void Apply() { Applies++; Gpu = (byte[])Cpu.Clone(); }
    }

    public struct Rect
    {
        public float x, y, w, h;
        public Rect(float x, float y, float w, float h) { this.x = x; this.y = y; this.w = w; this.h = h; }
    }

    public class GUIStyleState
    {
        public Texture2D background { get; set; }
    }

    public class GUIStyle
    {
        public GUIStyleState normal { get; } = new GUIStyleState();
    }

    public static class GUI
    {
        public static int depth;
        public static readonly List<string> Log = new List<string>();

        public static void Label(Rect r, Texture image, GUIStyle style)
        {
            var t = (Texture2D)image;
            Log.Add($"label {depth} {t.Gpu[0]},{t.Gpu[1]},{t.Gpu[2]},{t.Gpu[3]} bg {(style.normal.background == t ? "same" : "other")}");
        }
    }

    public static class Time
    {
        public static float deltaTime = 1f / 60f;
    }

    public static class Screen
    {
        public static int width = 720, height = 480;
    }
}
