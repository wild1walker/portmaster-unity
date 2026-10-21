// Tests of streaming-assets / streaming-assets-all and PathFlow. The fakes are built with Cecil: a
// "UnityEngine.CoreModule" whose Application getters return settable fields and whose AssetBundle /
// UnityWebRequest record the path they were given, and a game whose StreamingAssets paths travel
// the ways real ones do: straight into System.IO, through a property and a helper, a static field,
// an array, a virtual method, a delegate, into Unity's APIs only, and into both. The patched game
// is loaded and run against a jar: path whose folder has the files, and against a desktop path.
using System.Runtime.Loader;
using Mono.Cecil;
using Mono.Cecil.Cil;
using AssemblyName = System.Reflection.AssemblyName;

namespace AsmPatch.Tests;

static partial class TestMain
{
    static void StreamingAssets()
    {
        var d = Dir("sa");
        var engine = SaEngine(d);
        SaGame(d, engine);

        // --list: which reads reach System.IO
        var (st, outp) = Patch(d, null);
        Check(st == 0 && outp.Contains("would streaming-assets: Assembly-CSharp.dll: Game.ReadConfig: streamingAssetsPath -> Game_Data/StreamingAssets/ beside the APK (File.ReadAllText)"),
              "streaming-assets --list: a read straight into File.ReadAllText");
        foreach (var m in new[] { "Game.Levels", "Game.get_Root", "Game.Init", "Game.ArrayFlow", "Game.Mixed", "SaLoader.Path", "Game.SaRoot" })
            Check(Would(outp, "streaming-assets", m), $"streaming-assets --list: {m} reaches System.IO");
        Check(!Would(outp, "streaming-assets", "Game.Bundle") && !Would(outp, "streaming-assets", "Game.Web") &&
              outp.Contains("kept streamingAssetsPath at 2 reads"), "streaming-assets --list: reads that reach only Unity's APIs keep the jar path");
        Check(Would(outp, "streaming-assets-all", "Game.Bundle") && Would(outp, "streaming-assets-all", "Game.Web"),
              "streaming-assets-all --list: every read");

        // opt-in: a plain run leaves them alone
        var plain = Path.Combine(tmp, "sa-plain");
        (st, outp) = Patch(d, plain);
        Check(st == 0 && outp.Contains("skip streaming-assets (opt-in") && NoDlls(plain), "opt-in: not applied unless named");

        // applied (--with), then run
        var o = Path.Combine(tmp, "sa-out");
        (st, outp) = Patch(d, o, with: new[] { "streaming-assets" });
        Check(st == 0 && outp.Contains("ok   streaming-assets") && File.Exists(Path.Combine(o, "Assembly-CSharp.dll")) &&
              !File.Exists(Path.Combine(o, "UnityEngine.CoreModule.dll")), "streaming-assets --with: applied, only the game's assembly written");
        Check(Refs(Path.Combine(o, "Assembly-CSharp.dll")).SequenceEqual(Refs(Path.Combine(d, "Assembly-CSharp.dll"))),
              "streaming-assets: assembly references unchanged (" + string.Join(", ", Refs(Path.Combine(o, "Assembly-CSharp.dll"))) + ")");

        var dev = Path.Combine(tmp, "device");
        var folder = Path.Combine(dev, "Game_Data", "StreamingAssets");
        Directory.CreateDirectory(Path.Combine(folder, "levels"));
        foreach (var (f, text) in new[] { ("config.json", "{cfg}"), ("levels/a.lvl", "a"), ("levels/b.lvl", "b"), ("x.txt", "x"), ("f.txt", "f"),
                                          ("arr.txt", "arr"), ("b", "bundle"), ("v.txt", "v"), ("l.txt", "l") })
            File.WriteAllText(Path.Combine(folder, f), text);
        string jar = "jar:file://" + dev + "/main.1.com.test.game.obb!/assets";

        var before = SaRun(d, d, jar);
        Check(before["ReadConfig"].StartsWith("ERR") && before["Levels"].StartsWith("ERR") && before["ReadField"] == "False",
              $"unpatched on Android: System.IO on the jar path fails (ReadConfig {before["ReadConfig"]})");
        var after = SaRun(d, o, jar);
        Check(after["ReadConfig"] == "{cfg}", $"patched: File.ReadAllText reads the folder beside the APK ({after["ReadConfig"]})");
        Check(after["Levels"] == "2", $"patched: Directory.GetFiles lists it ({after["Levels"]})");
        Check(after["Read2"] == "x", $"patched: through a property and a helper ({after["Read2"]})");
        Check(after["ReadField"] == "True", "patched: through a static field");
        Check(after["ArrayFlow"] == "True", "patched: through an array");
        Check(after["ViaVirtual"] == "True", "patched: through a virtual method's return");
        Check(after["Lambda"] == "True", "patched: through a delegate");
        Check(after["Bundle"] == jar + "/b", $"patched: AssetBundle.LoadFromFile still gets the jar path ({after["Bundle"]})");
        Check(after["Web"] == "file://" + jar + "/w.json", $"patched: UnityWebRequest still gets the jar path ({after["Web"]})");
        Check(after["Mixed"] == folder + "/b", $"patched: a read that reaches System.IO and AssetBundle gives both the folder ({after["Mixed"]})");

        // the desktop: the patched assembly behaves as the original
        var desk = Path.Combine(tmp, "desktop", "Game_Data", "StreamingAssets");
        Directory.CreateDirectory(Path.GetDirectoryName(desk));
        Directory.Move(folder, desk);
        var dOrig = SaRun(d, d, desk);
        var dPatched = SaRun(d, o, desk);
        Check(dOrig.Count > 0 && dOrig.All(kv => dPatched[kv.Key] == kv.Value) && dPatched["ReadConfig"] == "{cfg}",
              "desktop path: patched and original give the same results");
        // odd paths pass through unchanged
        foreach (var odd in new[] { "jar:file://nofolder!/assets", "jar:file://", "jar:file:///a/b.obb", "" })
            Check(SaHelper(o, d, odd) == odd, $"helper: '{odd}' unchanged");
        Check(SaHelper(o, d, "jar:file:///x/y/z.apk!/assets") == "/x/y/Game_Data/StreamingAssets", "helper: jar path -> folder beside the archive");
        Check(SaHelper(o, d, null) == null, "helper: null unchanged");

        // again on the patched copy: done, nothing written
        var d2 = Dir("sa-again"); var o2 = Path.Combine(tmp, "sa-again-out");
        File.Copy(Path.Combine(o, "Assembly-CSharp.dll"), Path.Combine(d2, "Assembly-CSharp.dll"));
        File.Copy(Path.Combine(d, "UnityEngine.CoreModule.dll"), Path.Combine(d2, "UnityEngine.CoreModule.dll"));
        (st, outp) = Patch(d2, o2, with: new[] { "streaming-assets" });
        Check(st == 0 && outp.Contains("done streaming-assets") && NoDlls(o2), "streaming-assets twice: already applied, nothing written");
        // and -all on top: the rest of the reads
        (st, outp) = Patch(d2, o2, with: new[] { "streaming-assets-all" });
        Check(st == 0 && Would(outp, "streaming-assets-all", "Game.Bundle", "ok  ") && !Would(outp, "streaming-assets-all", "Game.ReadConfig", "ok  "),
              "streaming-assets-all after streaming-assets: only the reads still on the jar path");

        // -all: Unity's APIs get the folder too
        var oa = Path.Combine(tmp, "sa-all-out");
        Directory.Move(desk, folder);
        (st, outp) = Patch(d, oa, with: new[] { "streaming-assets-all" });
        var all = SaRun(d, oa, jar);
        Check(st == 0 && all["Bundle"] == folder + "/b" && all["ReadConfig"] == "{cfg}", $"streaming-assets-all: AssetBundle gets the folder ({all["Bundle"]})");
        Check(Patch(d, Path.Combine(tmp, "sa-both"), with: new[] { "streaming-assets", "streaming-assets-all" }).status == 0,
              "both patches named: applied once");

        // data-path: dataPath + "/custom.txt" and GetDirectoryName(dataPath) + "/beside.txt" read files;
        // the jar URL built from dataPath for a UnityWebRequest stays
        (st, outp) = Patch(d, null);
        Check(Would(outp, "data-path", "Game.ReadData") && Would(outp, "data-path", "Game.Beside") && !Would(outp, "data-path", "Game.JarUrl"),
              "data-path --list: the reads that reach System.IO, not the jar URL");
        var od = Path.Combine(tmp, "dp-out");
        (st, outp) = Patch(d, od, with: new[] { "data-path" });
        File.WriteAllText(Path.Combine(dev, "Game_Data", "custom.txt"), "custom");
        File.WriteAllText(Path.Combine(dev, "beside.txt"), "beside");
        var obb = dev + "/main.1.com.test.game.obb";
        var dp0 = SaRun(d, d, jar, obb);
        var dp1 = SaRun(d, od, jar, obb);
        Check(st == 0 && dp0["ReadData"].StartsWith("ERR") && dp1["ReadData"] == "custom", $"data-path: dataPath + \"/x\" reads Game_Data/x ({dp1["ReadData"]})");
        Check(dp0["Beside"] == "True" && dp1["Beside"] == "True", "data-path: GetDirectoryName(dataPath) is still the folder beside the APK");
        Check(dp1["JarUrl"] == "jar:file://" + obb + "!/assets/x", $"data-path: the jar URL built from dataPath stays ({dp1["JarUrl"]})");
        var deskData = Path.Combine(tmp, "desktop2", "Game_Data");
        Directory.CreateDirectory(deskData);
        File.WriteAllText(Path.Combine(deskData, "custom.txt"), "desk");
        Check(SaRun(d, od, desk, deskData)["ReadData"] == "desk" && SaRun(d, d, desk, deskData)["ReadData"] == "desk", "data-path: a desktop dataPath unchanged");
        Check(SaHelper(od, d, null, "/x/y/Game.apk", DataPathPatch.HelperMethod) == "/x/y/Game_Data" &&
              SaHelper(od, d, null, "/x/y/Game.zip", DataPathPatch.HelperMethod) == "/x/y/Game.zip" &&
              SaHelper(od, d, null, ".obb", DataPathPatch.HelperMethod) == ".obb", "data-path helper: .apk/.obb -> Game_Data, anything else unchanged");

        // --survey's file access section
        var w = new StringWriter(); var old = Console.Out; Console.SetOut(w);
        var game = new Game(d);
        try { PathFlow.Print(game); } finally { game.Dispose(); Console.SetOut(old); }
        Check(w.ToString().Contains("streamingAssetsPath") && w.ToString().Contains("reach System.IO") && w.ToString().Contains("--streaming-assets both"),
              "survey: the file access section names the reads and the option");
    }

    static bool Would(string output, string patch, string method, string verb = "would") =>
        output.Split('\n').Any(l => l.StartsWith($"{verb} {patch}: Assembly-CSharp.dll: {method}:"));

    // ---- the fakes ---------------------------------------------------------------------------
    static ModuleDefinition SaEngine(string dir)
    {
        var m = NewModule("UnityEngine.CoreModule");
        var ts = m.TypeSystem;
        const TypeAttributes St = TypeAttributes.Public | TypeAttributes.Abstract | TypeAttributes.Sealed | TypeAttributes.BeforeFieldInit;
        var app = new TypeDefinition("UnityEngine", "Application", St, ts.Object);
        foreach (var (field, getter) in new[] { ("SA", "get_streamingAssetsPath"), ("DP", "get_dataPath"), ("PD", "get_persistentDataPath") })
        {
            var f = new FieldDefinition(field, FieldAttributes.Public | FieldAttributes.Static, ts.String);
            app.Fields.Add(f);
            var g = new MethodDefinition(getter, MethodAttributes.Public | MethodAttributes.Static | MethodAttributes.HideBySig | MethodAttributes.SpecialName, ts.String);
            var il = g.Body.GetILProcessor();
            il.Emit(OpCodes.Ldsfld, f); il.Emit(OpCodes.Ret);
            app.Methods.Add(g);
        }
        m.Types.Add(app);
        foreach (var (ns, name, method) in new[] { ("UnityEngine", "AssetBundle", "LoadFromFile"), ("UnityEngine.Networking", "UnityWebRequest", "Get") })
        {
            var t = new TypeDefinition(ns, name, St, ts.Object);
            var last = new FieldDefinition("Last", FieldAttributes.Public | FieldAttributes.Static, ts.String);
            t.Fields.Add(last);
            var mm = new MethodDefinition(method, MethodAttributes.Public | MethodAttributes.Static | MethodAttributes.HideBySig, ts.Object);
            mm.Parameters.Add(new ParameterDefinition("path", ParameterAttributes.None, ts.String));
            var il = mm.Body.GetILProcessor();
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Stsfld, last); il.Emit(OpCodes.Ldnull); il.Emit(OpCodes.Ret);
            t.Methods.Add(mm);
            m.Types.Add(t);
        }
        m.Write(Path.Combine(dir, "UnityEngine.CoreModule.dll"));
        return m;
    }

    static void SaGame(string dir, ModuleDefinition engine)
    {
        var m = NewModule("Assembly-CSharp");
        var ts = m.TypeSystem;
        MethodReference E(string type, string name) => m.ImportReference(engine.Types.First(t => t.Name == type).Methods.First(x => x.Name == name));
        FieldReference EF(string type, string name) => m.ImportReference(engine.Types.First(t => t.Name == type).Fields.First(x => x.Name == name));
        var getSA = E("Application", "get_streamingAssetsPath");
        var getDP = E("Application", "get_dataPath");
        TypeReference T(string ns, string n) => new TypeReference(ns, n, m, ts.CoreLibrary);
        MethodReference M(TypeReference t, string n, TypeReference ret, bool inst, params TypeReference[] ps)
        {
            var r = new MethodReference(n, ret, t) { HasThis = inst };
            foreach (var p in ps) r.Parameters.Add(new ParameterDefinition(p));
            return r;
        }
        var file = T("System.IO", "File"); var path = T("System.IO", "Path"); var directory = T("System.IO", "Directory");
        var readAll = M(file, "ReadAllText", ts.String, false, ts.String);
        var exists = M(file, "Exists", ts.Boolean, false, ts.String);
        var combine = M(path, "Combine", ts.String, false, ts.String, ts.String);
        var getFiles = M(directory, "GetFiles", new ArrayType(ts.String), false, ts.String);
        var concat2 = M(ts.String, "Concat", ts.String, false, ts.String, ts.String);
        var concat3 = M(ts.String, "Concat", ts.String, false, ts.String, ts.String, ts.String);

        var game = new TypeDefinition("", "Game", TypeAttributes.Public | TypeAttributes.Abstract | TypeAttributes.Sealed | TypeAttributes.BeforeFieldInit, ts.Object);
        m.Types.Add(game);
        MethodDefinition Def(string name, TypeReference ret, params TypeReference[] ps)
        {
            var md = new MethodDefinition(name, MethodAttributes.Public | MethodAttributes.Static | MethodAttributes.HideBySig, ret);
            foreach (var p in ps) md.Parameters.Add(new ParameterDefinition(p));
            game.Methods.Add(md);
            return md;
        }
        ILProcessor IL(MethodDefinition md) => md.Body.GetILProcessor();

        // File.ReadAllText(Path.Combine(Application.streamingAssetsPath, "config.json"))
        var il = IL(Def("ReadConfig", ts.String));
        il.Emit(OpCodes.Call, getSA); il.Emit(OpCodes.Ldstr, "config.json"); il.Emit(OpCodes.Call, combine); il.Emit(OpCodes.Call, readAll); il.Emit(OpCodes.Ret);
        // Directory.GetFiles(streamingAssetsPath + "/levels").Length
        il = IL(Def("Levels", ts.Int32));
        il.Emit(OpCodes.Call, getSA); il.Emit(OpCodes.Ldstr, "/levels"); il.Emit(OpCodes.Call, concat2); il.Emit(OpCodes.Call, getFiles);
        il.Emit(OpCodes.Ldlen); il.Emit(OpCodes.Conv_I4); il.Emit(OpCodes.Ret);
        // AssetBundle.LoadFromFile(streamingAssetsPath + "/b"); return AssetBundle.Last
        il = IL(Def("Bundle", ts.String));
        il.Emit(OpCodes.Call, getSA); il.Emit(OpCodes.Ldstr, "/b"); il.Emit(OpCodes.Call, concat2); il.Emit(OpCodes.Call, E("AssetBundle", "LoadFromFile"));
        il.Emit(OpCodes.Pop); il.Emit(OpCodes.Ldsfld, EF("AssetBundle", "Last")); il.Emit(OpCodes.Ret);
        // UnityWebRequest.Get("file://" + streamingAssetsPath + "/w.json"); return UnityWebRequest.Last
        il = IL(Def("Web", ts.String));
        il.Emit(OpCodes.Ldstr, "file://"); il.Emit(OpCodes.Call, getSA); il.Emit(OpCodes.Ldstr, "/w.json"); il.Emit(OpCodes.Call, concat3);
        il.Emit(OpCodes.Call, E("UnityWebRequest", "Get")); il.Emit(OpCodes.Pop); il.Emit(OpCodes.Ldsfld, EF("UnityWebRequest", "Last")); il.Emit(OpCodes.Ret);
        // Root => streamingAssetsPath; Join(a, b) => a + "/" + b; Read2() => File.ReadAllText(Join(Root, "x.txt"))
        var root = Def("get_Root", ts.String);
        il = IL(root); il.Emit(OpCodes.Call, getSA); il.Emit(OpCodes.Ret);
        var join = Def("Join", ts.String, ts.String, ts.String);
        il = IL(join); il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldstr, "/"); il.Emit(OpCodes.Ldarg_1); il.Emit(OpCodes.Call, concat3); il.Emit(OpCodes.Ret);
        il = IL(Def("Read2", ts.String));
        il.Emit(OpCodes.Call, root); il.Emit(OpCodes.Ldstr, "x.txt"); il.Emit(OpCodes.Call, join); il.Emit(OpCodes.Call, readAll); il.Emit(OpCodes.Ret);
        // s_dir = streamingAssetsPath (Init); File.Exists(s_dir + "/f.txt") (ReadField)
        var sdir = new FieldDefinition("s_dir", FieldAttributes.Private | FieldAttributes.Static, ts.String);
        game.Fields.Add(sdir);
        il = IL(Def("Init", ts.Void)); il.Emit(OpCodes.Call, getSA); il.Emit(OpCodes.Stsfld, sdir); il.Emit(OpCodes.Ret);
        il = IL(Def("ReadField", ts.Boolean));
        il.Emit(OpCodes.Ldsfld, sdir); il.Emit(OpCodes.Ldstr, "/f.txt"); il.Emit(OpCodes.Call, concat2); il.Emit(OpCodes.Call, exists); il.Emit(OpCodes.Ret);
        // var a = new string[1]; a[0] = streamingAssetsPath; File.Exists(a[0] + "/arr.txt")
        il = IL(Def("ArrayFlow", ts.Boolean));
        il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Newarr, ts.String); il.Emit(OpCodes.Dup); il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Call, getSA);
        il.Emit(OpCodes.Stelem_Ref); il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Ldelem_Ref); il.Emit(OpCodes.Ldstr, "/arr.txt"); il.Emit(OpCodes.Call, concat2);
        il.Emit(OpCodes.Call, exists); il.Emit(OpCodes.Ret);
        // var p = streamingAssetsPath + "/b"; if (File.Exists(p)) AssetBundle.LoadFromFile(p); return AssetBundle.Last
        var mixed = Def("Mixed", ts.String);
        mixed.Body.Variables.Add(new VariableDefinition(ts.String));
        il = IL(mixed);
        var end = il.Create(OpCodes.Ldsfld, EF("AssetBundle", "Last"));
        il.Emit(OpCodes.Call, getSA); il.Emit(OpCodes.Ldstr, "/b"); il.Emit(OpCodes.Call, concat2); il.Emit(OpCodes.Stloc_0);
        il.Emit(OpCodes.Ldloc_0); il.Emit(OpCodes.Call, exists); il.Emit(OpCodes.Brfalse_S, end);
        il.Emit(OpCodes.Ldloc_0); il.Emit(OpCodes.Call, E("AssetBundle", "LoadFromFile")); il.Emit(OpCodes.Pop);
        il.Append(end); il.Emit(OpCodes.Ret);
        // abstract Loader { abstract string Path(); }  SaLoader : Loader { override Path() => streamingAssetsPath }
        var loader = new TypeDefinition("", "Loader", TypeAttributes.Public | TypeAttributes.Abstract | TypeAttributes.BeforeFieldInit, ts.Object);
        var lpath = new MethodDefinition("Path", MethodAttributes.Public | MethodAttributes.Virtual | MethodAttributes.Abstract | MethodAttributes.NewSlot | MethodAttributes.HideBySig, ts.String);
        loader.Methods.Add(lpath);
        loader.Methods.Add(Ctor(m, ObjectCtor(m)));
        m.Types.Add(loader);
        var sal = new TypeDefinition("", "SaLoader", TypeAttributes.Public | TypeAttributes.BeforeFieldInit, loader);
        var sp = new MethodDefinition("Path", MethodAttributes.Public | MethodAttributes.Virtual | MethodAttributes.HideBySig, ts.String);
        il = sp.Body.GetILProcessor(); il.Emit(OpCodes.Call, getSA); il.Emit(OpCodes.Ret);
        sal.Methods.Add(sp);
        sal.Methods.Add(Ctor(m, loader.Methods.First(x => x.IsConstructor)));
        m.Types.Add(sal);
        // ViaVirtual(Loader l) => File.Exists(l.Path() + "/v.txt")
        il = IL(Def("ViaVirtual", ts.Boolean, loader));
        il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Callvirt, lpath); il.Emit(OpCodes.Ldstr, "/v.txt"); il.Emit(OpCodes.Call, concat2); il.Emit(OpCodes.Call, exists); il.Emit(OpCodes.Ret);
        // Func<string> f = SaRoot; File.Exists(f() + "/l.txt")
        var saRoot = Def("SaRoot", ts.String);
        il = IL(saRoot); il.Emit(OpCodes.Call, getSA); il.Emit(OpCodes.Ret);
        var func = new TypeReference("System", "Func`1", m, ts.CoreLibrary);
        func.GenericParameters.Add(new GenericParameter("TResult", func));
        var funcS = new GenericInstanceType(func); funcS.GenericArguments.Add(ts.String);
        var fctor = new MethodReference(".ctor", ts.Void, funcS) { HasThis = true };
        fctor.Parameters.Add(new ParameterDefinition(ts.Object)); fctor.Parameters.Add(new ParameterDefinition(ts.IntPtr));
        var invoke = new MethodReference("Invoke", func.GenericParameters[0], funcS) { HasThis = true };
        il = IL(Def("Lambda", ts.Boolean));
        il.Emit(OpCodes.Ldnull); il.Emit(OpCodes.Ldftn, saRoot); il.Emit(OpCodes.Newobj, fctor); il.Emit(OpCodes.Callvirt, invoke);
        il.Emit(OpCodes.Ldstr, "/l.txt"); il.Emit(OpCodes.Call, concat2); il.Emit(OpCodes.Call, exists); il.Emit(OpCodes.Ret);
        // dataPath: File.ReadAllText(dataPath + "/custom.txt"); File.Exists(GetDirectoryName(dataPath) + "/beside.txt");
        // UnityWebRequest.Get("jar:file://" + dataPath + "!/assets/x")
        var dirName = M(path, "GetDirectoryName", ts.String, false, ts.String);
        il = IL(Def("ReadData", ts.String));
        il.Emit(OpCodes.Call, getDP); il.Emit(OpCodes.Ldstr, "/custom.txt"); il.Emit(OpCodes.Call, concat2); il.Emit(OpCodes.Call, readAll); il.Emit(OpCodes.Ret);
        il = IL(Def("Beside", ts.Boolean));
        il.Emit(OpCodes.Call, getDP); il.Emit(OpCodes.Call, dirName); il.Emit(OpCodes.Ldstr, "/beside.txt"); il.Emit(OpCodes.Call, concat2);
        il.Emit(OpCodes.Call, exists); il.Emit(OpCodes.Ret);
        il = IL(Def("JarUrl", ts.String));
        il.Emit(OpCodes.Ldstr, "jar:file://"); il.Emit(OpCodes.Call, getDP); il.Emit(OpCodes.Ldstr, "!/assets/x"); il.Emit(OpCodes.Call, concat3);
        il.Emit(OpCodes.Call, E("UnityWebRequest", "Get")); il.Emit(OpCodes.Pop); il.Emit(OpCodes.Ldsfld, EF("UnityWebRequest", "Last")); il.Emit(OpCodes.Ret);
        m.Write(Path.Combine(dir, "Assembly-CSharp.dll"));
    }

    // ---- running the fakes -------------------------------------------------------------------
    static AssemblyLoadContext SaContext(string orig, string patched)
    {
        var ctx = new AssemblyLoadContext("sa" + Guid.NewGuid(), isCollectible: true);
        ctx.Resolving += (c, n) =>
        {
            foreach (var dd in new[] { patched, orig })
            {
                var p = Path.Combine(dd, n.Name + ".dll");
                if (File.Exists(p)) return c.LoadFromStream(new MemoryStream(File.ReadAllBytes(p)));
            }
            return null;
        };
        return ctx;
    }

    /// <summary>Every Game method with Application.streamingAssetsPath = sa: result or "ERR <exception>".</summary>
    static Dictionary<string, string> SaRun(string orig, string patched, string sa, string dp = null)
    {
        var ctx = SaContext(orig, patched);
        try
        {
            var engine = ctx.LoadFromAssemblyName(new AssemblyName("UnityEngine.CoreModule"));
            engine.GetType("UnityEngine.Application", true).GetField("SA").SetValue(null, sa);
            engine.GetType("UnityEngine.Application", true).GetField("DP").SetValue(null, dp);
            var asm = ctx.LoadFromAssemblyName(new AssemblyName("Assembly-CSharp"));
            var g = asm.GetType("Game", true);
            var res = new Dictionary<string, string>();
            string Call(string name, params object[] a)
            {
                try { return Convert.ToString(g.GetMethod(name).Invoke(null, a)); }
                catch (System.Reflection.TargetInvocationException e) { return "ERR " + e.InnerException?.GetType().Name; }
            }
            g.GetMethod("Init").Invoke(null, null);
            foreach (var n in new[] { "ReadConfig", "Levels", "Bundle", "Web", "Read2", "ReadField", "ArrayFlow", "Mixed", "Lambda", "ReadData", "Beside", "JarUrl" })
                res[n] = Call(n);
            res["ViaVirtual"] = Call("ViaVirtual", Activator.CreateInstance(asm.GetType("SaLoader", true)));
            return res;
        }
        finally { ctx.Unload(); }
    }

    /// <summary>The patched assembly's helper with Application.streamingAssetsPath = sa.</summary>
    static string SaHelper(string patched, string orig, string sa, string dp = null, string method = StreamingAssetsPatch.HelperMethod)
    {
        var ctx = SaContext(orig, patched);
        try
        {
            var app = ctx.LoadFromAssemblyName(new AssemblyName("UnityEngine.CoreModule")).GetType("UnityEngine.Application", true);
            app.GetField("SA").SetValue(null, sa);
            app.GetField("DP").SetValue(null, dp);
            var h = ctx.LoadFromAssemblyName(new AssemblyName("Assembly-CSharp")).GetType(FolderPatch.HelperType, true);
            return (string)h.GetMethod(method, System.Reflection.BindingFlags.Static | System.Reflection.BindingFlags.NonPublic).Invoke(null, null);
        }
        finally { ctx.Unload(); }
    }
}
