// blasphemous-catalogs on a stand-in (test/fakes/Blasphemous: the catalog, the entities, the audio manager and a
// level's AudioLoader whose AddCatalogItems and GetLevelEnemyTypes do what the game's do, written here). The patch
// knows the game's IL by its hash; the test gives it the stand-in's. The driver (BlDriver) runs the same levels on
// the stand-in as built and as patched, in .NET: the loader's catalogs and the manager's registered ones must be the
// same, level by level (a level again after its owners were let go, two catalogs of one owner with the first one
// already the level's own, two classes of one name in two namespaces, a respawn, a level without enemies), and the
// patched game must have let the owners go (check-all: kept). Check logs no difference. Refusals: a nested or a
// generic class deriving from Entity, Owner named for reflection.
using AssemblyName = System.Reflection.AssemblyName;
using TargetInvocationException = System.Reflection.TargetInvocationException;
using System.Runtime.Loader;
using Mono.Cecil;
using Mono.Cecil.Cil;

namespace AsmPatch.Tests;

static partial class TestMain
{
    static readonly string FakeBl = Path.Combine(AppContext.BaseDirectory, "fakes", "bl");

    static string BlSrc(string name)
    {
        var src = Dir(name);
        foreach (var f in new[] { "UnityEngine.CoreModule.dll", "Assembly-CSharp.dll" }) File.Copy(Path.Combine(FakeBl, f), Path.Combine(src, f), true);
        var rt = Path.GetDirectoryName(typeof(object).Assembly.Location);
        foreach (var f in new[] { "System.Runtime.dll", "System.Private.CoreLib.dll", "System.Linq.dll", "System.Collections.dll" })
            File.Copy(Path.Combine(rt, f), Path.Combine(src, f), true);
        File.Copy(Path.Combine(rt, "System.Private.CoreLib.dll"), Path.Combine(src, "mscorlib.dll"), true);   // (the patch takes Exception from mscorlib)
        return src;
    }

    /// <summary>BlDriver.Run against the Assembly-CSharp in asmDir (the engine stand-in from the fakes), in its own
    /// load context, with ASMPATCH_BLASPHEMOUS_CATALOGS = mode; the transcript and the game's Debug.Log lines.</summary>
    static (string transcript, string logs) BlRun(string asmDir, string mode, bool writeList, string data)
    {
        Environment.SetEnvironmentVariable(BlasphemousCatalogs.Variable, mode);
        var ctx = new AssemblyLoadContext("bl" + Guid.NewGuid(), isCollectible: true);
        ctx.Resolving += (c, n) =>
        {
            foreach (var d in new[] { asmDir, FakeBl })
            {
                var p = Path.Combine(d, n.Name + ".dll");
                if (File.Exists(p) && (n.Name != "UnityEngine.CoreModule" || d == FakeBl)) return c.LoadFromStream(new MemoryStream(File.ReadAllBytes(p)));
            }
            return null;
        };
        try
        {
            var drv = ctx.LoadFromAssemblyName(new AssemblyName("BlDriver")).GetType("BlDriver", true);
            var t = (string)drv.GetMethod("Run").Invoke(null, new object[] { data, writeList });
            return (t, (string)drv.GetMethod("Logs").Invoke(null, null));
        }
        catch (TargetInvocationException e) { return ("EXCEPTION " + e.InnerException, ""); }
        finally { ctx.Unload(); Environment.SetEnvironmentVariable(BlasphemousCatalogs.Variable, null); }
    }

    // the message, with what was seen only when the check fails
    static void CheckSeen(bool ok, string what, string seen) => Check(ok, ok ? what : what + "\n" + seen);

    static void BlasphemousCatalogsTests()
    {
        if (!File.Exists(Path.Combine(FakeBl, "BlDriver.dll"))) { Check(false, $"Blasphemous stand-ins in {FakeBl} (build.sh test builds them)"); return; }
        var src = BlSrc("blcat");
        var (addHash, typesHash) = (BlasphemousCatalogs.AddHash, BlasphemousCatalogs.TypesHash);
        using (var a = AssemblyDefinition.ReadAssembly(Path.Combine(src, "Assembly-CSharp.dll")))
        {
            var loader = a.MainModule.GetType("Framework.Audio.AudioLoader");
            BlasphemousCatalogs.AddHash = BlasphemousCatalogs.Shape(loader.Methods.First(x => x.Name == "AddCatalogItems"));
            BlasphemousCatalogs.TypesHash = BlasphemousCatalogs.Shape(loader.Methods.First(x => x.Name == "GetLevelEnemyTypes"));
        }
        Game.AllowForeign = true;
        try
        {
            var o = Path.Combine(tmp, "blcat-out");
            var (st, outp) = Patch(src, o, only: Array.Empty<string>(), with: new[] { "blasphemous-catalogs" });
            Check(st == 0 && outp.Contains("ok   blasphemous-catalogs: Assembly-CSharp.dll") && outp.Contains("lets their owners go"), "blasphemous-catalogs: applied to the stand-in");
            if (!File.Exists(Path.Combine(o, "Assembly-CSharp.dll"))) return;

            var data = Path.Combine(tmp, "blcat-data");
            var (orig, _) = BlRun(src, "", false, data);
            CheckSeen(orig.Contains("L4 own dmkfool: loader dmkfool,fool;") && orig.Contains("owners after: acolyte1=acolyte one,noowner=-,dmkfool=fool,fool=fool,acolyte2=acolyte two"),
                  "blasphemous-catalogs: the stand-in as built: the game's choices (L4 the second Fool catalog), owners kept", orig);
            var (patched, plog) = BlRun(o, "", true, data);
            string Levels(string t) => string.Join("\n", t.Split('\n').Where(l => l.StartsWith("L")));
            CheckSeen(Levels(patched) == Levels(orig), "blasphemous-catalogs: patched, every level's loader and registered catalogs as the game's",
                      "--- game\n" + Levels(orig) + "\n--- patched\n" + Levels(patched));
            Check(patched.Split('\n')[0] == "startup array: ",
                  "blasphemous-catalogs: patched, the startup load is empty: " + patched.Split('\n')[0]);
            Check(patched.Contains("owners after: acolyte1=-,noowner=-,dmkfool=-,fool=-,acolyte2=-,foolsfx=-"),
                  "blasphemous-catalogs: patched, the owners of the catalogs walked let go: " + patched.Split('\n').Last());
            // no list: the game's own load, its owners kept, the same levels
            var (nolist, _) = BlRun(o, "", false, Path.Combine(tmp, "blcat-nolist"));
            CheckSeen(Levels(nolist) == Levels(orig) && nolist.Contains("owners after: acolyte1=acolyte one"), "blasphemous-catalogs: patched without a list: the game's load and owners", nolist);
            // check: the startup comparison, each walked catalog's owner type against the list, the owners let go
            var (chk, clog) = BlRun(o, "check", true, data);
            CheckSeen(Levels(chk) == Levels(orig) && clog.Contains("entries different (order, object, owner's type, count): 0") && !clog.Contains("DIFFERENT") &&
                  clog.Contains("check: the owners of") && chk.Contains("owners after: acolyte1=-"),
                  "blasphemous-catalogs: check: the same levels, no difference logged, owners let go", chk + "\n" + clog);
            // check-all: the game's array kept and compared at each level; owners kept
            var (all, alog) = BlRun(o, "check-all", true, data);
            CheckSeen(Levels(all) == Levels(orig) && alog.Contains("check level: the same as the game's array gives") && !alog.Contains("DIFFERENT") &&
                  all.Contains("owners after: acolyte1=acolyte one,noowner=-,dmkfool=fool,fool=fool,acolyte2=acolyte two"),
                  "blasphemous-catalogs: check-all: the same levels and the game's array, owners kept", all + "\n" + alog);

            // refusals, on altered copies of the stand-in
            void Refuses(string name, Action<ModuleDefinition> change, string why)
            {
                var d = BlSrc("blcat-" + name);
                var p = Path.Combine(d, "Assembly-CSharp.dll");
                using (var a = AssemblyDefinition.ReadAssembly(p, new ReaderParameters { InMemory = true }))
                {
                    change(a.MainModule);
                    a.Write(p);
                }
                var (s2, out2) = Patch(d, Path.Combine(tmp, "blcat-" + name + "-out"), only: Array.Empty<string>(), with: new[] { "blasphemous-catalogs" });
                Check(s2 == 0 && out2.Contains("n/a  blasphemous-catalogs") && out2.Contains(why) && NoDlls(Path.Combine(tmp, "blcat-" + name + "-out")),
                      $"blasphemous-catalogs refuses {name}: {why}");
            }
            Refuses("nested", m =>
            {
                var enemy = m.GetType("Gameplay.GameControllers.Entities.Enemy");
                var host = m.GetType("Enemies.Three.Fool");
                var n = new TypeDefinition("", "Inner", TypeAttributes.NestedPublic | TypeAttributes.Class, enemy);
                host.NestedTypes.Add(n);
            }, "a nested class derives from Entity");
            Refuses("generic", m =>
            {
                var enemy = m.GetType("Gameplay.GameControllers.Entities.Enemy");
                var g = new TypeDefinition("Enemies.Four", "Swarm`1", TypeAttributes.Public | TypeAttributes.Class, enemy);
                g.GenericParameters.Add(new GenericParameter("T", g));
                m.Types.Add(g);
            }, "a generic class derives from Entity");
            Refuses("reflection", m =>
            {
                // typeof(FMODAudioCatalog).GetField("Owner") in some method of the game
                var t = m.GetType("Framework.Managers.Core");
                var md = new MethodDefinition("Peek", MethodAttributes.Public | MethodAttributes.Static, m.TypeSystem.Object);
                var il = md.Body.GetILProcessor();
                var typeT = new TypeReference("System", "Type", m, m.TypeSystem.CoreLibrary);
                var fromHandle = new MethodReference("GetTypeFromHandle", typeT, typeT)
                    { Parameters = { new ParameterDefinition(new TypeReference("System", "RuntimeTypeHandle", m, m.TypeSystem.CoreLibrary, true)) } };
                var fieldInfo = new TypeReference("System.Reflection", "FieldInfo", m, m.TypeSystem.CoreLibrary);
                var getField = new MethodReference("GetField", fieldInfo, typeT) { HasThis = true, Parameters = { new ParameterDefinition(m.TypeSystem.String) } };
                il.Emit(OpCodes.Ldtoken, m.GetType("Framework.Audio.FMODAudioCatalog")); il.Emit(OpCodes.Call, fromHandle);
                il.Emit(OpCodes.Ldstr, "Owner"); il.Emit(OpCodes.Callvirt, getField); il.Emit(OpCodes.Ret);
                t.Methods.Add(md);
            }, "used where this patch does not know it");
        }
        finally
        {
            Game.AllowForeign = false;
            (BlasphemousCatalogs.AddHash, BlasphemousCatalogs.TypesHash) = (addHash, typesHash);
        }
    }
}
