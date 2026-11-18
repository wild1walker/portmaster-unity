// Stand-ins for the engine types hk-text's helpers call (written for the tests; no engine code), in the firstpass
// stand-in so that the Assembly-CSharp stand-in calls them through references, as the game calls the engine's:
// Object (its name and its == with null), Component.transform, Transform.parent, position and lossyScale, Vector3,
// Vector4, Mathf.Abs, Camera.allCameras, cullingMask and WorldToViewportPoint, Component.gameObject, GameObject.layer,
// Bounds.min and max.
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
        GameObject go;
        public GameObject gameObject => go ??= new GameObject();
    }

    public class GameObject : Object
    {
        public int layer { get; set; }
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

    /// <summary>An orthographic camera at X, HalfWidth world units each side of it: viewport x = (x - X) / (2 HalfWidth)
    /// + 1/2 (y 1/2, z 1: in front). allCameras and Mask: the tests set them.</summary>
    public class Camera : Component
    {
        public static Camera main { get; set; }
        public static Camera[] allCameras { get; set; } = new Camera[0];
        public float X, HalfWidth = 10f;
        public int Mask = -1;
        public int cullingMask => Mask;
        public Vector3 WorldToViewportPoint(Vector3 p) => new Vector3((p.x - X) / (2f * HalfWidth) + 0.5f, 0.5f, 1f);
    }

    public struct Bounds
    {
        Vector3 c, e;
        public Bounds(Vector3 center, Vector3 size) { c = center; e = new Vector3(size.x / 2f, size.y / 2f, size.z / 2f); }
        public Vector3 min => new Vector3(c.x - e.x, c.y - e.y, c.z - e.z);
        public Vector3 max => new Vector3(c.x + e.x, c.y + e.y, c.z + e.z);
    }
}
