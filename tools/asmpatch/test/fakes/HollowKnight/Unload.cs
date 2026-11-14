// A stand-in for Hollow Knight's GameManager.IsUnloadAssetsRequired, which hk-unload patches (written for the
// tests; no game code): unused assets are unloaded at a room change only past the game's own limits on Unity's
// memory, 2.5 GB, or 2.1 GB at a change of zone (MemoryUsage and ZoneChange stand for what the game reads).
using UnityEngine;

public class GameManager
{
    public static long MemoryUsage;
    public static bool ZoneChange;

    public bool IsUnloadAssetsRequired(string sourceSceneName, string destinationSceneName)
    {
        long memoryUsage = MemoryUsage;
        if (memoryUsage > 2621440000u)
        {
            Debug.Log("Memory exceeds high memory limit");
            return true;
        }
        if (ZoneChange && memoryUsage > 2202009600u)
        {
            Debug.Log("Memory exceeds low memory limit, and this is a zone transition.");
            return true;
        }
        return false;
    }
}
