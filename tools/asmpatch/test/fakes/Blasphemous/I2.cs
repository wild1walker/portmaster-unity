// A stand-in for blasphemous-i2-sources' tests (no game or I2 code): I2 Localization's LanguageSource and the parts of
// LocalizationManager the patch looks at, doing what the game's do (Sources, AddSource, RemoveSource,
// UnregisterDeletededSources, RegisterSceneSources, UpdateSources), written here.
using System.Collections.Generic;
using UnityEngine;

namespace I2.Loc
{
    public class LanguageSource : MonoBehaviour
    {
        public int GoogleUpdateFrequency = 3;
        public float GoogleUpdateDelay = 5f;
        public List<string> mTerms = new List<string>();
        public bool NeverDestroy = true;
    }

    public static class LocalizationManager
    {
        public static List<LanguageSource> Sources = new List<LanguageSource>();

        public static bool UpdateSources()
        {
            UnregisterDeletededSources();
            RegisterSceneSources();
            return Sources.Count > 0;
        }

        private static void UnregisterDeletededSources()
        {
            for (int num = Sources.Count - 1; num >= 0; num--)
                if (Sources[num] == null) RemoveSource(Sources[num]);
        }

        private static void RegisterSceneSources()
        {
            LanguageSource[] array = (LanguageSource[])Resources.FindObjectsOfTypeAll(typeof(LanguageSource));
            int i = 0;
            for (int num = array.Length; i < num; i++)
                if (!Sources.Contains(array[i])) AddSource(array[i]);
        }

        public static void AddSource(LanguageSource Source)
        {
            if (Sources.Contains(Source)) return;
            Sources.Add(Source);
        }

        public static void RemoveSource(LanguageSource Source)
        {
            Sources.Remove(Source);
        }
    }
}
