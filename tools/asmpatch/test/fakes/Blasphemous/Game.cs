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
