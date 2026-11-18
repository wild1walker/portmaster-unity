// The engine types the blasphemous-* patches look for and call (written here, no Unity code): Object with Unity's null
// rule (a destroyed object equals null) and an instance id, MonoBehaviour, GameObject, Resources (Load and LoadAll
// from a table the test fills, FindObjectsOfTypeAll: every loaded object, Loaded), Debug.Log (kept for the test),
// Application.dataPath, Object.FindObjectsOfType (the active objects of Scene, the generic one through the
// Type one as Unity's; Order lets a test give each type's scan its own order), counts of the scans.
using System;
using System.Collections.Generic;
using System.Linq;

namespace UnityEngine
{
    public class Object
    {
        public string name { get; set; }
        public bool destroyed;
        public static bool operator ==(Object a, Object b)
        {
            bool an = (object)a == null || a.destroyed, bn = (object)b == null || b.destroyed;
            if (an || bn) return an && bn;
            return ReferenceEquals(a, b);
        }
        public static bool operator !=(Object a, Object b) => !(a == b);
        public override bool Equals(object o) => o is Object x && this == x;
        public override int GetHashCode() => base.GetHashCode();
        public bool active = true;
        static int ids;
        readonly int id = ++ids;
        public int GetInstanceID() => id;
        public static Object[] Scene = new Object[0];
        public static int Finds;
        public static Func<Type, Object[], Object[]> Order;
        public static Object[] FindObjectsOfType(Type t)
        {
            Finds++;
            var r = Scene.Where(o => t.IsInstanceOfType(o) && !o.destroyed && o.active).ToArray();
            return Order != null ? Order(t, r) : r;
        }
        public static T[] FindObjectsOfType<T>() where T : Object => FindObjectsOfType(typeof(T)).Cast<T>().ToArray();
    }
    public class Component : Object { public GameObject gameObject; }
    public class Behaviour : Component { }
    public class MonoBehaviour : Behaviour { }
    public class GameObject : Object
    {
        public List<Component> Components = new List<Component>();
        public T GetComponentInChildren<T>() where T : Component => Components.OfType<T>().FirstOrDefault();
    }
    public static class Resources
    {
        public static Dictionary<string, Object> Table = new Dictionary<string, Object>();
        public static int Loads;
        public static Object Load(string path, Type t) { Loads++; return Table.TryGetValue(path.ToLowerInvariant(), out var o) && t.IsInstanceOfType(o) ? o : null; }
        public static List<Object> Loaded = new List<Object>();
        public static int FindAlls;
        public static Object[] FindObjectsOfTypeAll(Type t)
        {
            FindAlls++;
            var r = Loaded.Where(o => t.IsInstanceOfType(o) && !o.destroyed).ToArray();
            var a = Array.CreateInstance(t, r.Length);
            Array.Copy(r, a, r.Length);
            return (Object[])a;
        }
        public static T[] LoadAll<T>(string path) where T : Object =>
            Table.Where(kv => kv.Key.StartsWith(path.ToLowerInvariant() + "/")).Select(kv => kv.Value).OfType<T>().ToArray();
    }
    public static class Debug
    {
        public static List<string> Logs = new List<string>();
        public static void Log(object message) { Logs.Add(message?.ToString()); }
    }
    public static class Application { public static string Data = ""; public static string dataPath => Data; }
}
