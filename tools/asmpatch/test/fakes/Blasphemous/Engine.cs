// The engine types blasphemous-catalogs looks for and calls (written here, no Unity code): Object with Unity's null
// rule (a destroyed object equals null), MonoBehaviour, GameObject, Resources (Load and LoadAll from a table the test
// fills), Debug.Log (kept for the test), Application.dataPath, Object.FindObjectsOfType (what the test sets).
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
        public static Object[] Scene = new Object[0];
        public static T[] FindObjectsOfType<T>() where T : Object => Scene.OfType<T>().ToArray();
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
