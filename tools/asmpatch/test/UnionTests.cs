// mono5-generic-union on a stand-in game assembly (test/fakes/Union, built by build.sh test): .NET
// refuses the unpatched union as Mono 5 does; the patch is opt-in; the patched copy loads and gives the
// union's own values (8 zeroed bytes, the same bytes written and read) and its static constructor runs.
using System.Reflection;
using System.Runtime.Loader;

namespace AsmPatch.Tests;

static partial class TestMain
{
    static readonly string FakeUnion = Path.Combine(AppContext.BaseDirectory, "fakes", "GameUnion.dll");

    static bool HasTypeLoad(Exception e)
    {
        for (; e != null; e = e.InnerException) if (e is TypeLoadException) return true;
        return false;
    }

    /// <summary>The stand-in's results, or the exception the first call threw.</summary>
    static (object[] values, Exception error) CallUnion(string dir)
    {
        var ctx = new AssemblyLoadContext("u" + Guid.NewGuid(), isCollectible: true);
        try
        {
            var a = ctx.LoadFromStream(new MemoryStream(File.ReadAllBytes(Path.Combine(dir, "GameUnion.dll"))));
            Type T(string n) => a.GetType(n, true);
            var fe = T("FastEnum");
            object Call(string m, Type e, object v) => fe.GetMethod(m).MakeGenericMethod(e).Invoke(null, new[] { v });
            object E(string t, long v) => System.Enum.ToObject(T(t), v);
            try
            {
                return (new object[]
                {
                    Call("ToInt32", T("Small"), E("Small", 200)), Call("ToInt32", T("Normal"), E("Normal", -5)),
                    Call("ToInt32", T("Normal"), E("Normal", 123456)), Call("ToInt32", T("Big"), E("Big", 1L << 40)),
                    Call("ToLong", T("Normal"), E("Normal", -5)), Call("ToLong", T("Big"), E("Big", -3)),
                    Call("ToByte", T("Normal"), E("Normal", 123456)),
                    Call("ToEnum", T("Small"), 200).ToString(), Call("ToEnum", T("Normal"), -5).ToString(),
                    Call("ToEnumL", T("Big"), -3L).ToString(),
                    T("Keys").GetField("Up").GetValue(null),
                }, null);
            }
            catch (Exception e) { return (null, e); }
        }
        finally { ctx.Unload(); }
    }

    static void UnionTests()
    {
        if (!File.Exists(FakeUnion)) { Check(false, $"union stand-in at {FakeUnion} (build.sh test builds it)"); return; }
        var d = Dir("union");
        File.Copy(FakeUnion, Path.Combine(d, "GameUnion.dll"));
        var (_, err) = CallUnion(d);
        Check(HasTypeLoad(err), $"mono5-generic-union: .NET refuses the unpatched union as Mono 5 does ({err?.GetType().Name})");
        var (st, outp) = Patch(d, Path.Combine(tmp, "union-default"));
        Check(st == 0 && outp.Contains("skip mono5-generic-union (opt-in: --with=mono5-generic-union)") && NoDlls(Path.Combine(tmp, "union-default")),
              "mono5-generic-union: opt-in, not run by default");
        var o = Path.Combine(tmp, "union-out");
        (st, outp) = Patch(d, o, with: new[] { "mono5-generic-union" });
        Check(st == 0 && outp.Contains("ok   mono5-generic-union: GameUnion.dll: FastEnum/Converter`1 (6 fields at offset 0): 5 methods") &&
              File.Exists(Path.Combine(o, "GameUnion.dll")), "mono5-generic-union: applied when named");
        if (!File.Exists(Path.Combine(o, "GameUnion.dll"))) return;
        var (vals, err2) = CallUnion(o);
        // the union's values: an enum's bytes at offset 0 of 8 zeroed bytes, read back at another width
        var want = new object[] { 200, -5, 123456, 0, 4294967291L, -3L, (byte)64, "B", "X", "Q", 123456 };
        Check(err2 == null && vals != null && vals.SequenceEqual(want),
              $"mono5-generic-union: the patched union gives the union's own values ({(err2 != null ? err2.GetBaseException().Message : string.Join(", ", vals))})");
        (st, outp) = Patch(o, Path.Combine(tmp, "union-again"), with: new[] { "mono5-generic-union" });
        Check(st == 0 && NoDlls(Path.Combine(tmp, "union-again")), "mono5-generic-union: a patched copy is left as it is");
    }
}
