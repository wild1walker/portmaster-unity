// blasphemous-i2-sources and blasphemous-map-scan on the stand-in (test/fakes/Blasphemous: I2.cs, Game.cs' MapManager).
// The driver runs the same steps as built and as patched: I2's Sources the same list in the same order at every
// scene load (sources made in a scene, two at once, one made before it is loaded, inactive, destroyed, made and
// destroyed before a scan) with fewer scans; MapManager's cache the same lists in the same order at every level
// (inactive objects, a subclass, other persistent objects, a level without any, "NONE") with one scan a level
// instead of four. Check modes log no difference; each check catches what it is there for (a source made without
// its constructor, a scan whose order depends on the type asked for). Refusals: another IL, Sources changed elsewhere.
using AssemblyName = System.Reflection.AssemblyName;
using TargetInvocationException = System.Reflection.TargetInvocationException;
using System.Runtime.Loader;
using Mono.Cecil;
using Mono.Cecil.Cil;

namespace AsmPatch.Tests;

static partial class TestMain
{
    /// <summary>BlDriver.&lt;method&gt;(arg) against the assemblies in asmDir, in its own load context, with variable = mode.</summary>
    static (string transcript, string logs) BlCall(string asmDir, string variable, string mode, string method, bool arg)
    {
        Environment.SetEnvironmentVariable(variable, mode);
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
            var t = (string)drv.GetMethod(method).Invoke(null, new object[] { arg });
            return (t, (string)drv.GetMethod("Logs").Invoke(null, null));
        }
        catch (TargetInvocationException e) { return ("EXCEPTION " + e.InnerException, ""); }
        finally { ctx.Unload(); Environment.SetEnvironmentVariable(variable, null); }
    }

    static void BlasphemousScanTests()
    {
        if (!File.Exists(Path.Combine(FakeBl, "Assembly-CSharp-firstpass.dll"))) { Check(false, $"Blasphemous stand-ins in {FakeBl} (build.sh test builds them)"); return; }
        var src = BlSrc("blscan");
        var saved = (BlasphemousI2Sources.ScanHash, BlasphemousI2Sources.UnregisterHash, BlasphemousI2Sources.CtorHash, BlasphemousMapScan.LoadedHash, BlasphemousMapScan.FillHash);
        void Hashes(string dir)
        {
            using var fp = AssemblyDefinition.ReadAssembly(Path.Combine(dir, "Assembly-CSharp-firstpass.dll"));
            var lm = fp.MainModule.GetType("I2.Loc.LocalizationManager");
            BlasphemousI2Sources.ScanHash = BlasphemousCatalogs.Shape(lm.Methods.First(x => x.Name == "RegisterSceneSources"));
            BlasphemousI2Sources.UnregisterHash = BlasphemousCatalogs.Shape(lm.Methods.First(x => x.Name == "UnregisterDeletededSources"));
            BlasphemousI2Sources.CtorHash = BlasphemousCatalogs.Shape(fp.MainModule.GetType("I2.Loc.LanguageSource").Methods.First(x => x.IsConstructor && !x.IsStatic));
            using var a = AssemblyDefinition.ReadAssembly(Path.Combine(dir, "Assembly-CSharp.dll"));
            var map = a.MainModule.GetType("Framework.Managers.MapManager");
            BlasphemousMapScan.LoadedHash = BlasphemousCatalogs.Shape(map.Methods.First(x => x.Name == "OnLevelLoaded"));
            BlasphemousMapScan.FillHash = BlasphemousCatalogs.Shape(map.Methods.First(x => x.Name == "FillSceneCacheObject"));
        }
        Hashes(src);
        Game.AllowForeign = true;
        try
        {
            var o = Path.Combine(tmp, "blscan-out");
            var (st, outp) = Patch(src, o, only: Array.Empty<string>(), with: new[] { "blasphemous-i2-sources", "blasphemous-map-scan" });
            Check(st == 0 && outp.Contains("ok   blasphemous-i2-sources: Assembly-CSharp-firstpass.dll") && outp.Contains("ok   blasphemous-map-scan: Assembly-CSharp.dll"),
                  "blasphemous-i2-sources and blasphemous-map-scan: applied to the stand-in\n" + outp);
            if (!File.Exists(Path.Combine(o, "Assembly-CSharp-firstpass.dll")) || !File.Exists(Path.Combine(o, "Assembly-CSharp.dll"))) return;
            foreach (var f in new[] { "UnityEngine.CoreModule.dll" }) File.Copy(Path.Combine(src, f), Path.Combine(o, f), true);

            const string IV = BlasphemousI2Sources.Variable, MV = BlasphemousMapScan.Variable;
            static string Sources(string t) => string.Join("\n", t.Split('\n').Select(l => l.Split("; scans")[0]));
            static int Scans(string t) => int.Parse(t.Split('\n').Last().Split("; scans ")[1]);

            // I2: the same Sources at every step, fewer scans
            var (gi, _) = BlCall(src, IV, "", "RunI2", false);
            var (pi, pil) = BlCall(o, IV, "", "RunI2", false);
            CheckSeen(!gi.StartsWith("EXCEPTION") && Sources(pi) == Sources(gi) && Scans(pi) < Scans(gi),
                      $"blasphemous-i2-sources: patched, Sources the same list in the same order at every scene load, scans {Scans(gi)} -> {(pi.StartsWith("EXCEPTION") ? -1 : Scans(pi))}",
                      "--- game\n" + gi + "\n--- patched\n" + pi + "\n" + pil);
            var (ci, cil) = BlCall(o, IV, "check", "RunI2", false);
            CheckSeen(Sources(ci) == Sources(gi) && Scans(ci) == Scans(gi) && !cil.Contains("DIFFERENT") && cil.Contains("the patch would have skipped it"),
                      "blasphemous-i2-sources: check: every scan done, no difference logged", ci + "\n" + cil);
            var (fi, _) = BlCall(o, IV, "off", "RunI2", false);
            CheckSeen(fi == gi, "blasphemous-i2-sources: off: the game's scans", fi);
            var (ni, nil) = BlCall(o, IV, "check", "RunI2", true);
            CheckSeen(nil.Contains("check DIFFERENT") && nil.Contains("no constructor"),
                      "blasphemous-i2-sources: check catches a source the patch could not have known (made without its constructor)", ni + "\n" + nil);

            // map: the same cache at every level, one scan instead of four
            var (gm, _) = BlCall(src, MV, "", "RunMap", false);
            var (pm, pml) = BlCall(o, MV, "", "RunMap", false);
            CheckSeen(!gm.StartsWith("EXCEPTION") && Sources(pm) == Sources(gm) && Scans(gm) == 16 && Scans(pm) == 4,
                      $"blasphemous-map-scan: patched, the same lists in the same order at every level, scans {(gm.StartsWith("EXCEPTION") ? -1 : Scans(gm))} -> {(pm.StartsWith("EXCEPTION") ? -1 : Scans(pm))}",
                      "--- game\n" + gm + "\n--- patched\n" + pm + "\n" + pml);
            var (cm, cml) = BlCall(o, MV, "check", "RunMap", false);
            CheckSeen(Sources(cm) == Sources(gm) && !cml.Contains("DIFFERENT") && cml.Split('\n').Count(l => l.Contains("check: the same")) == 4,
                      "blasphemous-map-scan: check: each level the same as the game's four scans", cm + "\n" + cml);
            var (fm, _) = BlCall(o, MV, "off", "RunMap", false);
            CheckSeen(fm == gm, "blasphemous-map-scan: off: the game's four scans", fm);
            var (rm, rml) = BlCall(o, MV, "check", "RunMap", true);
            var (rg, _) = BlCall(src, MV, "", "RunMap", true);
            CheckSeen(rml.Contains("check DIFFERENT") && Sources(rm) == Sources(rg),
                      "blasphemous-map-scan: check catches a scan whose order depends on the type (and keeps the game's lists)", rm + "\n" + rml);

            // refusals, on altered copies
            void Refuses(string patch, string name, Action<ModuleDefinition, ModuleDefinition> change, string why)
            {
                var d = BlSrc("blscan-" + name);
                foreach (var f in new[] { "Assembly-CSharp-firstpass.dll", "Assembly-CSharp.dll" })
                {
                    var p = Path.Combine(d, f);
                    using var a = AssemblyDefinition.ReadAssembly(p, new ReaderParameters { InMemory = true });
                    if (f.Contains("firstpass")) change(a.MainModule, null); else change(null, a.MainModule);
                    a.Write(p);
                }
                var (s2, out2) = Patch(d, Path.Combine(tmp, "blscan-" + name + "-out"), only: Array.Empty<string>(), with: new[] { patch });
                Check(s2 == 0 && out2.Contains("n/a  " + patch) && out2.Contains(why), $"{patch} refuses {name}: {why}");
            }
            Refuses("blasphemous-i2-sources", "another-scan", (fp, _) =>
            {
                if (fp == null) return;
                var md = fp.GetType("I2.Loc.LocalizationManager").Methods.First(x => x.Name == "RegisterSceneSources");
                md.Body.GetILProcessor().InsertBefore(md.Body.Instructions[0], Instruction.Create(OpCodes.Nop));
            }, "not as this patch knows them");
            Refuses("blasphemous-i2-sources", "sources-cleared", (fp, _) =>
            {
                if (fp == null) return;
                // LocalizationManager.Reset() { Sources.Clear(); }
                var lm = fp.GetType("I2.Loc.LocalizationManager");
                var sources = lm.Fields.First(f => f.Name == "Sources");
                var clear = new MethodReference("Clear", fp.TypeSystem.Void, sources.FieldType) { HasThis = true };
                var md = new MethodDefinition("Reset", MethodAttributes.Public | MethodAttributes.Static, fp.TypeSystem.Void);
                var il = md.Body.GetILProcessor();
                il.Emit(OpCodes.Ldsfld, sources); il.Emit(OpCodes.Callvirt, fp.ImportReference(clear)); il.Emit(OpCodes.Ret);
                lm.Methods.Add(md);
            }, "Sources is changed where this patch does not know it");
            Refuses("blasphemous-map-scan", "another-fill", (_, a) =>
            {
                if (a == null) return;
                var md = a.GetType("Framework.Managers.MapManager").Methods.First(x => x.Name == "FillSceneCacheObject");
                md.Body.GetILProcessor().InsertBefore(md.Body.Instructions[0], Instruction.Create(OpCodes.Nop));
            }, "not as this patch knows them");
        }
        finally
        {
            Game.AllowForeign = false;
            (BlasphemousI2Sources.ScanHash, BlasphemousI2Sources.UnregisterHash, BlasphemousI2Sources.CtorHash, BlasphemousMapScan.LoadedHash, BlasphemousMapScan.FillHash) = saved;
        }
    }
}
