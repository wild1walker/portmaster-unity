// Stand-ins for the engine types hk-text's helpers call (written for the tests; no engine code), in the firstpass
// stand-in so that the Assembly-CSharp stand-in calls them through references, as the game calls the engine's:
// Object (its name and its == with null), Component.transform, Transform.parent, position and lossyScale, Vector3,
// Vector4, Mathf.Abs.
namespace UnityEngine
{
    public class Object
    {
        public string name { get; set; } = "";
        public static bool operator ==(Object a, Object b) => ReferenceEquals(a, b);
        public static bool operator !=(Object a, Object b) => !ReferenceEquals(a, b);
        public override bool Equals(object o) => ReferenceEquals(this, o);
        public override int GetHashCode() => System.Runtime.CompilerServices.RuntimeHelpers.GetHashCode(this);
    }

    public class Component : Object
    {
        public Transform Own;
        public Transform transform => Own;
    }

    public class Transform : Component
    {
        public Transform() { Own = this; }
        public Transform parent { get; set; }
        public Vector3 position { get; set; }
        public Vector3 lossyScale { get; set; } = new Vector3(1f, 1f, 1f);
    }

    public struct Vector3
    {
        public float x, y, z;
        public Vector3(float x, float y, float z) { this.x = x; this.y = y; this.z = z; }
    }

    public struct Vector4
    {
        public float x, y, z, w;
        public Vector4(float x, float y, float z, float w) { this.x = x; this.y = y; this.z = z; this.w = w; }
    }

    public static class Mathf
    {
        public static float Abs(float v) => v < 0f ? -v : v;
    }
}
