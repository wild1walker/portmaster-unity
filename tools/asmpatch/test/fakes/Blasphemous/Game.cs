// A stand-in for what blasphemous-catalogs reads in Blasphemous's Assembly-CSharp (written here from the patch's
// description, no game code): the catalog, the entities, the audio manager and the level's audio loader, with
// AddCatalogItems and GetLevelEnemyTypes doing what the game's do.
using System;
using System.Collections.Generic;
using System.Linq;
using UnityEngine;

namespace Gameplay.GameControllers.Entities
{
    public class Entity : MonoBehaviour { }
    public class Enemy : Entity { }
}
namespace Enemies.One { public class Acolyte : Gameplay.GameControllers.Entities.Enemy { } }
namespace Enemies.Two { public class Acolyte : Gameplay.GameControllers.Entities.Enemy { } }
namespace Enemies.Three { public class Fool : Gameplay.GameControllers.Entities.Enemy { } }
namespace Tools.Level.Layout
{
    public class EnemySpawnPoint : MonoBehaviour { public GameObject SelectedEnemy; }
}
namespace Framework.Audio
{
    using Gameplay.GameControllers.Entities;
    public class FMODAudioCatalog : MonoBehaviour
    {
        public FMODAudioCatalog AssociatedCatalog;
        public bool HasAssociatedCatalog;
        public Entity Owner;
        public int Initialized;
        public void Initialize() { Initialized++; }
    }
    public class AudioLoader : MonoBehaviour
    {
        public FMODAudioCatalog[] AudioCatalogs = new FMODAudioCatalog[0];
        public void Awake() { AddCatalogItems(); }
        public void Start() { foreach (var c in AudioCatalogs) if (c != null) Framework.Managers.Core.Audio.RegisterCatalog(c); }
        private void AddCatalogItems()
        {
            List<FMODAudioCatalog> list = AudioCatalogs.ToList();
            FMODAudioCatalog[] enemiesAudioCatalogs = Framework.Managers.Core.Audio.EnemiesAudioCatalogs;
            IEnumerable<Type> levelEnemyTypes = GetLevelEnemyTypes();
            foreach (Type item in levelEnemyTypes)
            {
                FMODAudioCatalog[] array = enemiesAudioCatalogs;
                foreach (FMODAudioCatalog c in array)
                {
                    if (!(c.Owner == null) && c.Owner.GetType() == item && !list.Contains(c))
                    {
                        list.Add(c);
                        if (c.HasAssociatedCatalog && !list.Contains(c.AssociatedCatalog)) list.Add(c.AssociatedCatalog);
                        break;
                    }
                }
            }
            AudioCatalogs = list.ToArray();
        }
        private IEnumerable<Type> GetLevelEnemyTypes()
        {
            List<Type> list = new List<Type>();
            foreach (Tools.Level.Layout.EnemySpawnPoint p in UnityEngine.Object.FindObjectsOfType<Tools.Level.Layout.EnemySpawnPoint>())
            {
                if (!(p.SelectedEnemy == null))
                {
                    Type type = p.SelectedEnemy.GetComponentInChildren<Enemy>().GetType();
                    if (!list.Contains(type)) list.Add(type);
                }
            }
            return list;
        }
    }
}
namespace Framework.Managers
{
    using Framework.Audio;
    public class Core { public static FMODAudioManager Audio; }
    public class FMODAudioManager
    {
        FMODAudioCatalog[] _audioCatalogs = new FMODAudioCatalog[0];
        public FMODAudioCatalog[] EnemiesAudioCatalogs { get; private set; }
        public FMODAudioCatalog[] Registered => _audioCatalogs;
        public void Initialize() { EnemiesAudioCatalogs = Resources.LoadAll<FMODAudioCatalog>("FMODCatalogs/Enemies"); }
        public void RegisterCatalog(FMODAudioCatalog cat)
        {
            if (!_audioCatalogs.Any(t => t == cat))
            {
                var a = new FMODAudioCatalog[_audioCatalogs.Length + 1];
                _audioCatalogs.CopyTo(a, 0); a[_audioCatalogs.Length] = cat; _audioCatalogs = a;
                cat.Initialize();
            }
        }
    }
}

// blasphemous-map-scan's stand-in: the persistent objects' classes and MapManager's cache as the game fills it
namespace Framework.FrameworkCore
{
    public class PersistentObject : UnityEngine.MonoBehaviour { }
    public class Level { public string LevelName; }
}
namespace Tools.Level.Interactables
{
    public class Interactable : Framework.FrameworkCore.PersistentObject { }
    public class Door : Interactable { }
    public class SpecialDoor : Door { }
    public class PrieDieu : Interactable { }
}
namespace Tools.Level.Actionables { public class Gate : Framework.FrameworkCore.PersistentObject { } }
namespace Tools.Level { public class Teleport : Framework.FrameworkCore.PersistentObject { } }
namespace Framework.Managers
{
    using System;
    using System.Collections.Generic;
    using System.Linq;
    using Framework.FrameworkCore;
    using Tools.Level.Interactables;
    using Tools.Level.Actionables;
    using Tools.Level;

    public class MapManager : UnityEngine.MonoBehaviour
    {
        private Dictionary<Type, List<PersistentObject>> cacheObjects = new Dictionary<Type, List<PersistentObject>>();
        public string CurrentZone;

        public void OnLevelLoaded(Framework.FrameworkCore.Level oldLevel, Framework.FrameworkCore.Level newLevel)
        {
            string levelName = newLevel.LevelName;
            cacheObjects.Clear();
            FillSceneCacheObject<Door>();
            FillSceneCacheObject<PrieDieu>();
            FillSceneCacheObject<Gate>();
            FillSceneCacheObject<Teleport>();
            if (levelName == "NONE") return;
            CurrentZone = levelName;
        }

        private void FillSceneCacheObject<T>() where T : PersistentObject
        {
            Type typeFromHandle = typeof(T);
            T[] array = UnityEngine.Object.FindObjectsOfType<T>();
            foreach (T item in array)
            {
                if (!cacheObjects.ContainsKey(typeFromHandle)) cacheObjects[typeFromHandle] = new List<PersistentObject>();
                cacheObjects[typeFromHandle].Add(item);
            }
        }

        // the test's view of the cache: each type's objects by name, in order, and how many lists
        public string Cache() =>
            string.Join(";", new[] { typeof(Door), typeof(PrieDieu), typeof(Gate), typeof(Teleport) }
                .Select(t => t.Name + ":" + (cacheObjects.TryGetValue(t, out var l) ? string.Join(",", l.Select(x => x.name)) : "-"))) + " n=" + cacheObjects.Count;
    }
}

// blasphemous-skin-idle's stand-in: ExtrasMenuWidget's pages and skin selector as the game shows them (ShowMenu,
// ResetMenus, ShowSkinSelectorValues and its lookups as Blasphemous 4.0 has them, cut to what touches the pages'
// alpha and the preview Images; a locked skin's element is made inactive, and ShowSkinSelectorValues then throws, as
// the game's does). skinSelectorDataElements is filled by the driver through reflection, as Unity fills it.
namespace Gameplay.UI.Others.MenuLogic
{
    using System;
    using System.Collections.Generic;
    using UnityEngine;
    using UnityEngine.UI;

    public class ExtrasMenuWidget : MonoBehaviour
    {
        [Serializable]
        public struct SkinSelectorElement { public string skinKey; public GameObject element; }
        private enum STATE { STATE_OFF, STATE_EXTRAS }
        private enum MENU { EXTRAS, SKINSELECTOR, ACHIEVEMENT }

        public GameObject ExtrasRoot, AchievementRoot;
        private GameObject SelectorRoot;
        private readonly Dictionary<MENU, GameObject> extrasRoot = new Dictionary<MENU, GameObject>();
        private List<SkinSelectorElement> skinSelectorDataElements;
        private STATE currentState;
        private MENU currentMenu;
        public List<string> allSkins = new List<string>();
        public List<string> unlocked = new List<string>();
        public string currentSkin, optionLastSkinSelected;
        public bool noSelection;

        public void Awake()
        {
            extrasRoot.Clear();
            extrasRoot[MENU.ACHIEVEMENT] = AchievementRoot;
            extrasRoot[MENU.EXTRAS] = ExtrasRoot;
            extrasRoot[MENU.SKINSELECTOR] = SelectorRoot;
            currentState = STATE.STATE_OFF;
            currentMenu = MENU.EXTRAS;
            ResetMenus();
        }

        public void ShowExtras() { currentState = STATE.STATE_EXTRAS; ShowMenu(MENU.EXTRAS); }
        public void Hide() { ResetMenus(); currentState = STATE.STATE_OFF; }
        public void GoBack() { if (currentState == STATE.STATE_EXTRAS) { if (currentMenu != MENU.EXTRAS) ShowMenu(MENU.EXTRAS); else Hide(); } }
        public void Option_MenuSkinSelector() { if (currentState == STATE.STATE_EXTRAS) ShowMenu(MENU.SKINSELECTOR); }
        public void Next(string skin)   // the pad moved, as Update sees it
        {
            if (currentState != STATE.STATE_EXTRAS || currentMenu != MENU.SKINSELECTOR) return;
            optionLastSkinSelected = skin;
            UpdateInputSkinSelectorOptions();
        }

        private void ShowMenu(MENU menu)
        {
            currentMenu = menu;
            foreach (KeyValuePair<MENU, GameObject> item in extrasRoot)
                if (item.Value != null) item.Value.GetComponentInChildren<CanvasGroup>(true).alpha = item.Key != currentMenu ? 0f : 1f;
            if (noSelection) return;
            switch (currentMenu)
            {
                case MENU.SKINSELECTOR:
                    foreach (SkinSelectorElement e in skinSelectorDataElements) SetOptionSkinSelected(e.skinKey, selected: false);
                    for (int j = 0; j < allSkins.Count; j++)
                    {
                        string k = allSkins[j];
                        skinSelectorDataElements.Find(x => x.skinKey == k).element.active = unlocked.Contains(k);
                    }
                    optionLastSkinSelected = currentSkin;
                    ShowSkinSelectorValues();
                    break;
            }
        }

        private void SetOptionSkinSelected(string option, bool selected)
        {
            skinSelectorDataElements.Find(x => x.skinKey == option).element.name = option + (selected ? "*" : "");
        }

        private void UpdateInputSkinSelectorOptions()
        {
            currentSkin = optionLastSkinSelected;
            ShowSkinSelectorValues();
        }

        private void ShowSkinSelectorValues()
        {
            int i;
            for (i = 0; i < allSkins.Count; i++)
            {
                Image img = skinSelectorDataElements.Find(x => x.skinKey == allSkins[i]).element.GetComponentInChildren<Image>(false);
                img.enabled = currentSkin == allSkins[i];
            }
        }

        private void ResetMenus()
        {
            foreach (KeyValuePair<MENU, GameObject> item in extrasRoot)
                if (item.Value != null) item.Value.GetComponentInChildren<CanvasGroup>(true).alpha = 0f;
        }
    }
}
