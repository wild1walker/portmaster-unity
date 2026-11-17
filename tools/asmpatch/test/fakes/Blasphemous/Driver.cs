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
}
