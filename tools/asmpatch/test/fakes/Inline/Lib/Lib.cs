// A stand-in library for the inline test (written for the test): a list with private storage and a throwing
// indexer, float helpers with branches, a struct with operators and a property, an object with a private
// pointer and an implicit bool, the shapes the inline patch copies out of mscorlib and Unity's engine modules.
using System;

namespace InlineLib
{
    public class Bag<T>
    {
        private T[] _items = new T[16];
        private int _size;
        public void Add(T x) { _items[_size++] = x; }
        public int Count { get { return _size; } }
        public T this[int i] { get { if ((uint)i >= (uint)_size) throw new ArgumentOutOfRangeException("i"); return _items[i]; } }
    }

    public static class M
    {
        public static float Clamp(float v, float lo, float hi) { if (v < lo) v = lo; else if (v > hi) v = hi; return v; }
        public static float Abs(float f) { return f < 0f ? -f : f; }
        public static int Floor(float f) { return (int)Math.Floor(f); }
        public static float Lerp(float a, float b, float t) { return a + (b - a) * Clamp(t, 0f, 1f); }
    }

    public struct V
    {
        public float x, y;
        public V(float a, float b) { x = a; y = b; }
        public static V operator -(V a, V b) { return new V(a.x - b.x, a.y - b.y); }
        public float Sqr { get { return x * x + y * y; } }
        public V Scaled(float d) { return new V(x * d, y * d); }
    }

    public class Obj
    {
        private IntPtr ptr;
        public int Value;
        public Obj(bool alive, int v) { ptr = alive ? (IntPtr)1 : IntPtr.Zero; Value = v; }
        public int GetValue() { return Value; }
        public static implicit operator bool(Obj o) { return o != null && o.ptr != IntPtr.Zero; }
    }
}
