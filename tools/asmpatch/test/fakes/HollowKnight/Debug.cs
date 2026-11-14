// A stand-in for UnityEngine.Debug's Log (written for the tests; no engine code), in the firstpass stand-in so
// that the Assembly-CSharp stand-in calls it through a reference, as the game calls the engine's: hk-unload
// finds Debug.Log(object) among the module's references for its line.
namespace UnityEngine
{
    public static class Debug
    {
        public static string Last;
        public static int Count;
        public static void Log(object message) { Last = message as string; Count++; }
    }
}
