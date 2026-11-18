// The blasphemous-catalogs tests' driver (kept out of the patched folder: it writes Owner, which the patch's check
// would count as a reader): the catalogs and their owners, the list file, then levels as the game runs them
// (FindObjectsOfType's spawn points, a level's AudioLoader: Awake, then Start), and a transcript to compare.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Framework.Audio;
using Framework.Managers;
using Gameplay.GameControllers.Entities;
using Tools.Level.Layout;
using UnityEngine;

public static class BlDriver
{
    static FMODAudioCatalog Cat(string name, Entity owner)
    {
        var c = new FMODAudioCatalog { name = name, Owner = owner };
        Resources.Table["fmodcatalogs/enemies/" + name] = c;
        return c;
    }
    static T Prefab<T>(string name) where T : Entity, new()
    {
        var go = new GameObject { name = name };
        var e = new T { name = name, gameObject = go };
        go.Components.Add(e);
        return e;
    }
    static string Names(IEnumerable<FMODAudioCatalog> cs) => string.Join(",", cs.Select(c => c == null ? "null" : c.name));

    public static string Run(string dataDir, bool writeList)
    {
        var a1 = Prefab<Enemies.One.Acolyte>("acolyte one");
        var a2 = Prefab<Enemies.Two.Acolyte>("acolyte two");
        var fool = Prefab<Enemies.Three.Fool>("fool");
        // in LoadAll's order (the table's): two catalogs own the Fool (the game takes the first not in the level's list)
        var cats = new[] { Cat("acolyte1", a1), Cat("noowner", null), Cat("dmkfool", fool), Cat("fool", fool), Cat("acolyte2", a2), Cat("foolsfx", null) };
        cats[2].HasAssociatedCatalog = true; cats[2].AssociatedCatalog = cats[5];
        Directory.CreateDirectory(dataDir);
        Application.Data = dataDir;
        if (writeList)
            File.WriteAllLines(Path.Combine(dataDir, "fmodcatalogs.txt"), new[] { "# pmu fmodcatalogs 1" }.Concat(cats.Select(c =>
                "fmodcatalogs/enemies/" + c.name + "\t" + (c.Owner == null ? "-" : c.Owner.GetType().FullName))));
        Core.Audio = new FMODAudioManager();
        Core.Audio.Initialize();
        var t = new List<string> { "startup array: " + Names(Core.Audio.EnemiesAudioCatalogs) };
        void Level(string name, Entity[] enemies, FMODAudioCatalog[] own)
        {
            UnityEngine.Object.Scene = enemies.Select(e => (UnityEngine.Object)new EnemySpawnPoint { SelectedEnemy = e.gameObject })
                                              .Append(new EnemySpawnPoint { SelectedEnemy = null }).ToArray();
            var loader = new AudioLoader { AudioCatalogs = own };
            loader.Awake(); loader.Start();
            t.Add($"{name}: loader {Names(loader.AudioCatalogs)}; registered {Names(Core.Audio.Registered)}; initialized " +
                  string.Join(",", Core.Audio.Registered.Select(c => c.Initialized)));
        }
        Level("L1", new Entity[] { a1 }, new FMODAudioCatalog[0]);
        Level("L2", new Entity[] { fool }, new FMODAudioCatalog[0]);
        Level("L3 again", new Entity[] { a1, fool }, new FMODAudioCatalog[0]);
        Level("L4 own dmkfool", new Entity[] { fool }, new[] { cats[2] });
        Level("L5", new Entity[] { a2, a1 }, new FMODAudioCatalog[0]);
        Level("L5 respawn", new Entity[] { a2, a1 }, new FMODAudioCatalog[0]);
        Level("L6 none", new Entity[0], new FMODAudioCatalog[0]);
        t.Add("owners after: " + string.Join(",", cats.Select(c => c.name + "=" + (c.Owner == null ? "-" : c.Owner.name))));
        return string.Join("\n", t);
    }
    public static string Logs() => string.Join("\n", UnityEngine.Debug.Logs);

    // blasphemous-i2-sources: language sources made and loaded as scenes come and go; at each scene load
    // UpdateSources as I2's ResourceManager calls it. Each line: the step, Sources in order, the scans so far.
    // noCtor: one source made without its constructor (what Unity does not do: the check's own test)
    public static string RunI2(bool noCtor)
    {
        var t = new List<string>();
        I2.Loc.LanguageSource Made(string name, bool load = true, bool active = true)
        {
            var s = new I2.Loc.LanguageSource { name = name, active = active };
            if (load) Resources.Loaded.Add(s);
            return s;
        }
        void Step(string what)
        {
            I2.Loc.LocalizationManager.UpdateSources();
            t.Add($"{what}: sources {string.Join(",", I2.Loc.LocalizationManager.Sources.Select(x => x.name))}; scans {Resources.FindAlls}");
        }
        Resources.Loaded.Add(new GameObject { name = "not a source" });
        var g = Made("global");
        I2.Loc.LocalizationManager.AddSource(g);          // (RegisterSourceInResources)
        Step("first");
        Step("a scene without sources");
        Made("scene a");
        Step("a scene with one");
        Step("the same scene again");
        Made("b inactive", active: false); Made("c");
        Step("two at once");
        var late = Made("late", load: false);
        Step("made, not loaded yet");
        Step("still not loaded");
        Resources.Loaded.Add(late);
        Step("loaded");
        if (noCtor)
        {
            var x = (I2.Loc.LanguageSource)System.Runtime.CompilerServices.RuntimeHelpers.GetUninitializedObject(typeof(I2.Loc.LanguageSource));
            x.name = "no constructor";
            Resources.Loaded.Add(x);
            Step("a source without its constructor");
        }
        Resources.Loaded.First(o => o.name == "scene a").destroyed = true;
        Step("scene a destroyed");
        var d = Made("d"); d.destroyed = true;
        Step("made and destroyed before a scan");
        Step("then");
        return string.Join("\n", t);
    }

    // blasphemous-map-scan: three levels' persistent objects, MapManager.OnLevelLoaded at each. Each line: the level,
    // the cache, the scans so far. reorder: each type's scan in its own order (what the check is for)
    public static string RunMap(bool reorder)
    {
        if (reorder) UnityEngine.Object.Order = (ty, r) => ty == typeof(Framework.FrameworkCore.PersistentObject) ? r.Reverse().ToArray() : r;
        var t = new List<string>();
        var m = new MapManager();
        T O<T>(string name, bool active = true) where T : UnityEngine.Object, new() => new T { name = name, active = active };
        void Level(string name, params UnityEngine.Object[] objs)
        {
            UnityEngine.Object.Scene = objs;
            m.OnLevelLoaded(null, new Framework.FrameworkCore.Level { LevelName = name });
            t.Add($"{name}: {m.Cache()}; scans {UnityEngine.Object.Finds}");
        }
        Level("L1", O<Tools.Level.Interactables.Door>("door 1"), O<Tools.Level.Actionables.Gate>("gate 1"), O<Framework.FrameworkCore.PersistentObject>("other"),
              O<Tools.Level.Interactables.PrieDieu>("prie-dieu"), O<Tools.Level.Interactables.Door>("door off", active: false),
              O<Tools.Level.Interactables.SpecialDoor>("special door"), O<Tools.Level.Teleport>("teleport"), O<GameObject>("not one"),
              O<Tools.Level.Interactables.Door>("door 2"), O<Tools.Level.Actionables.Gate>("gate 2"));
        Level("L2", O<Tools.Level.Interactables.Door>("door 3"));
        Level("L3");
        Level("NONE", O<Tools.Level.Teleport>("teleport 2"));
        return string.Join("\n", t);
    }
}
