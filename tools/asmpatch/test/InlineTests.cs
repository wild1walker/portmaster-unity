// inline and inline-check on stand-ins (test/fakes/Inline, built by build.sh test): a game assembly whose hot
// callers use a library's small methods (a list with private storage and a throwing indexer, float helpers with
// branches, a struct's operator and property, an implicit bool reading a private pointer). The patched callers
// must hold no call to a listed callee, the library's private fields must have become readable for the game
// (InternalsVisibleTo, as .NET enforces field access like Mono 2.6), and both copies must give the same values
// and the same exceptions. (Mono 2.6's own float rounding at call boundaries is checked in the emulated rig and
// by inline-check on the handheld: docs/profiling.md.)
using System.Reflection;
using System.Runtime.Loader;
using Mono.Cecil;

namespace AsmPatch.Tests;

static partial class TestMain
{
    static readonly string FakeInline = Path.Combine(AppContext.BaseDirectory, "fakes", "inline");

    static (List<object> values, List<string> errors) CallInline(string dir)
    {
        var ctx = new AssemblyLoadContext("i" + Guid.NewGuid(), isCollectible: true);
        var values = new List<object>(); var errors = new List<string>();
        try
        {
            ctx.Resolving += (c, n) => c.LoadFromStream(new MemoryStream(File.ReadAllBytes(Path.Combine(dir, n.Name + ".dll"))));
            var lib = ctx.LoadFromAssemblyName(new AssemblyName("InlineLib"));
            var game = ctx.LoadFromAssemblyName(new AssemblyName("Assembly-CSharp"));
            var bagF = lib.GetType("InlineLib.Bag`1", true).MakeGenericType(typeof(float));
            var objT = lib.GetType("InlineLib.Obj", true);
            var bagO = lib.GetType("InlineLib.Bag`1", true).MakeGenericType(objT);
            var hot = game.GetType("Hot", true);
            object Bag(Type t, IEnumerable<object> items) { var b = Activator.CreateInstance(t); foreach (var x in items) t.GetMethod("Add").Invoke(b, new[] { x }); return b; }
            var rnd = new Random(5);
            for (int k = 0; k < 40; k++)
            {
                var xs = Bag(bagF, Enumerable.Range(0, 1 + rnd.Next(6)).Select(_ => (object)(float)(rnd.NextDouble() * 20 - 10)));
                var objs = Bag(bagO, Enumerable.Range(0, rnd.Next(9)).Select(i => rnd.Next(5) == 0 ? null : Activator.CreateInstance(objT, rnd.Next(4) != 0, i)));
                float t = (float)(rnd.NextDouble() * 3 - 1);
                values.Add(BitConverter.SingleToInt32Bits((float)hot.GetMethod("Walk").Invoke(null, new[] { objs, xs, t })));
            }
            var xs3 = Bag(bagF, new object[] { 1.5f, 2.5f, 3.5f });
            foreach (var i in new[] { 0, 2, 3, -1 })
                try { values.Add(hot.GetMethod("At").Invoke(null, new object[] { xs3, i })); }
                catch (TargetInvocationException e) { errors.Add($"At({i}): {e.InnerException.GetType().Name}"); }
            try { values.Add(hot.GetMethod("At").Invoke(null, new object[] { null, 0 })); }
            catch (TargetInvocationException e) { errors.Add($"At(null): {e.InnerException.GetType().Name}"); }
            try { values.Add(hot.GetMethod("Value").Invoke(null, new object[] { null })); }
            catch (TargetInvocationException e) { errors.Add($"Value(null): {e.InnerException.GetType().Name}"); }
        }
        catch (Exception e) { errors.Add("load: " + e.GetType().Name + " " + (e.InnerException ?? e).GetType().Name + " " + (e.InnerException ?? e).Message); }
        finally { ctx.Unload(); }
        return (values, errors);
    }

    static void InlineTests()
    {
        if (!File.Exists(Path.Combine(FakeInline, "Assembly-CSharp.dll"))) { Check(false, $"inline stand-ins in {FakeInline} (build.sh test builds them)"); return; }
        // the stand-ins with the runtime's own System.Runtime and CoreLib beside them, as a game's Managed folder has its
        // mscorlib: the patch resolves the callees' calls (ArgumentOutOfRangeException, Math.Floor, IntPtr) to check them
        var src = Path.Combine(tmp, "inline-src"); Directory.CreateDirectory(src);
        foreach (var f in Directory.GetFiles(FakeInline, "*.dll")) File.Copy(f, Path.Combine(src, Path.GetFileName(f)));
        var rt = Path.GetDirectoryName(typeof(object).Assembly.Location);
        foreach (var f in new[] { "System.Runtime.dll", "System.Private.CoreLib.dll" }) File.Copy(Path.Combine(rt, f), Path.Combine(src, f));
        var list = Path.Combine(tmp, "inline.txt");
        File.WriteAllText(list, "# test\ncaller Hot::Walk\ncaller Hot::At\ncaller Hot::Value\ncallee InlineLib.Bag`1::get_Item\ncallee InlineLib.Bag`1::get_Count\n" +
                                "callee InlineLib.M::*\ncallee InlineLib.V::*\ncallee InlineLib.Obj::op_Implicit\ncallee InlineLib.Obj::GetValue\n");
        var orig = CallInline(FakeInline);
        Check(orig.values.Count == 42 && orig.errors.Count == 4, $"inline: the stand-ins run unpatched ({orig.values.Count} values, {string.Join("; ", orig.errors)})");
        foreach (var mode in new[] { "inline", "inline-check" })
        {
            var outDir = Path.Combine(tmp, mode);
            InlineHot.ListPath = list;
            int status; var sw = new StringWriter(); var stdout = Console.Out; Console.SetOut(sw);
            using (var game = new Game(src))
                try { status = Program.Run(game, outDir, new[] { mode }, Array.Empty<string>()); } finally { Console.SetOut(stdout); InlineHot.ListPath = null; }
            var log = sw.ToString();
            if (status != 0) Console.WriteLine(log);
            Check(status == 0 && log.Contains($"ok   {mode}: Assembly-CSharp.dll: Hot::Walk:") && log.Contains("InlineLib.dll: Bag`1._") && log.Contains("Obj.ptr") && log.Contains("internals visible to Assembly-CSharp"),
                  $"{mode}: applied, with the library's private fields made readable ({log.Split('\n').FirstOrDefault(l => l.Contains("Hot::Walk"))?.Trim()})");
            foreach (var f in new[] { "InlineLib.dll", "Assembly-CSharp.dll" }) if (!File.Exists(Path.Combine(outDir, f))) File.Copy(Path.Combine(FakeInline, f), Path.Combine(outDir, f));
            var a = AssemblyDefinition.ReadAssembly(Path.Combine(outDir, "Assembly-CSharp.dll"));
            var walk = a.MainModule.GetType("Hot").Methods.First(m => m.Name == "Walk");
            var left = walk.Body.Instructions.Where(i => i.Operand is MethodReference r && r.DeclaringType.Namespace == "InlineLib" && r.Name != ".ctor" && mode == "inline").ToList();
            Check(left.Count == 0, $"{mode}: Hot.Walk calls no listed callee ({string.Join(", ", left.Select(i => ((MethodReference)i.Operand).Name))})");
            var patched = CallInline(outDir);
            Check(patched.values.SequenceEqual(orig.values) && patched.errors.SequenceEqual(orig.errors),
                  $"{mode}: the same {patched.values.Count} values and exceptions ({string.Join("; ", patched.errors)})");
            using (var again = new Game(outDir))
            {
                InlineHot.ListPath = list; var sw2 = new StringWriter(); Console.SetOut(sw2);
                try { Program.Run(again, null, new[] { mode }, Array.Empty<string>()); } finally { Console.SetOut(stdout); InlineHot.ListPath = null; }
                Check(sw2.ToString().Contains($"done {mode}:"), $"{mode}: a second run finds it done");
            }
        }
        InlineHot.ListPath = Path.Combine(tmp, "no-such-list.txt");
        var sw3 = new StringWriter(); var so = Console.Out; Console.SetOut(sw3);
        using (var g3 = new Game(src)) try { Program.Run(g3, null, new[] { "inline" }, Array.Empty<string>()); } finally { Console.SetOut(so); InlineHot.ListPath = null; }
        Check(sw3.ToString().Contains("n/a  inline: no list"), "inline: n/a without a list");
    }
}
