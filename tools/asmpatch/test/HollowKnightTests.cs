// hk-blur on a stand-in Assembly-CSharp (test/fakes/HollowKnight, built by build.sh test): opt-in,
// then patched, loaded and started with ASMPATCH_HK_BLUR unset and set to each of its words.
using System.Runtime.Loader;
using Assembly = System.Reflection.Assembly;

namespace AsmPatch.Tests;

static partial class TestMain
{
    static readonly string FakeHollowKnight = Path.Combine(AppContext.BaseDirectory, "fakes", "hk", "Assembly-CSharp.dll");

    /// <summary>(texture width, height, blur passes with 2 pass groups and with 1, cancelled, plane
    /// renderer enabled) after the rig's start at aspect 1.6.</summary>
    static (int w, int h, int passes, int passes1, bool cancelled, bool visible) StartRig(string dir, string blur)
    {
        var old = Environment.GetEnvironmentVariable(HollowKnightBlur.Variable);
        Environment.SetEnvironmentVariable(HollowKnightBlur.Variable, blur);
        var ctx = new AssemblyLoadContext("hk" + Guid.NewGuid(), isCollectible: true);
        try
        {
            Assembly a = ctx.LoadFromStream(new MemoryStream(File.ReadAllBytes(Path.Combine(dir, "Assembly-CSharp.dll"))));
            var lbbT = a.GetType("LightBlurredBackground", true);
            var lbb = Activator.CreateInstance(lbbT);
            lbbT.GetMethod("Start").Invoke(lbb, new object[] { 1.6f });
            var planeT = a.GetType("BlurPlane", true);
            var plane = Activator.CreateInstance(planeT);
            planeT.GetMethod("Start").Invoke(plane, null);
            var r = planeT.GetProperty("Renderer").GetValue(plane);
            var blurT = a.GetType("LightBlur", true);
            var lb = Activator.CreateInstance(blurT);
            int passes = (int)blurT.GetProperty("BlurPassCount").GetValue(lb);
            blurT.GetProperty("PassGroupCount").SetValue(lb, 1);
            int passes1 = (int)blurT.GetProperty("BlurPassCount").GetValue(lb);
            return ((int)lbbT.GetField("TextureWidth").GetValue(lbb), (int)lbbT.GetField("TextureHeight").GetValue(lbb), passes, passes1,
                    (bool)lbbT.GetProperty("Cancelled").GetValue(lbb), (bool)r.GetType().GetProperty("enabled").GetValue(r));
        }
        finally { ctx.Unload(); Environment.SetEnvironmentVariable(HollowKnightBlur.Variable, old); }
    }

    static void HollowKnightTests()
    {
        if (!File.Exists(FakeHollowKnight)) { Check(false, $"Hollow Knight stand-in at {FakeHollowKnight} (build.sh test builds it)"); return; }
        var d = Dir("hk");
        File.Copy(FakeHollowKnight, Path.Combine(d, "Assembly-CSharp.dll"));
        var (st, outp) = Patch(d, Path.Combine(tmp, "hk-default"));
        Check(st == 0 && outp.Contains("skip hk-blur (opt-in: --with=hk-blur)") && NoDlls(Path.Combine(tmp, "hk-default")), "hk-blur: opt-in, not run by default");
        var o = Path.Combine(tmp, "hk-out");
        (st, outp) = Patch(d, o, with: new[] { "hk-blur" });
        Check(st == 0 && outp.Contains("ok   hk-blur: Assembly-CSharp.dll: LightBlurredBackground") && File.Exists(Path.Combine(o, "Assembly-CSharp.dll")),
              "hk-blur: applied when named");
        if (!File.Exists(Path.Combine(o, "Assembly-CSharp.dll"))) return;
        var full = (576, 360, 4, 2, false, true);
        Check(StartRig(d, "off") == full, "hk-blur: the unpatched stand-in ignores the variable");
        foreach (var v in new[] { null, "", "full", "HALF", "halfway", "lower" })
            Check(StartRig(o, v) == full, $"hk-blur: ASMPATCH_HK_BLUR={v ?? "(unset)"}: the game as it is");
        foreach (var (v, want) in new[]
                 {
                     ("low", (576, 360, 2, 2, false, true)), ("half", (288, 180, 4, 2, false, true)),
                     ("half,low", (288, 180, 2, 2, false, true)), ("low,half", (288, 180, 2, 2, false, true)),
                     ("off", (576, 360, 4, 2, true, false)),
                 })
            Check(StartRig(o, v) == want, $"hk-blur: {v}: {StartRig(o, v)}");
        (st, outp) = Patch(o, Path.Combine(tmp, "hk-again"), with: new[] { "hk-blur" });
        Check(st == 0 && outp.Contains("done hk-blur") && NoDlls(Path.Combine(tmp, "hk-again")), "hk-blur: a second run finds it applied");
        var none = Dir("hk-none");
        var plain = NewModule("Assembly-CSharp");
        plain.Types.Add(new Mono.Cecil.TypeDefinition("", "Game", Mono.Cecil.TypeAttributes.Public, plain.TypeSystem.Object));
        plain.Write(Path.Combine(none, "Assembly-CSharp.dll"));
        (st, outp) = Patch(none, Path.Combine(tmp, "hk-none-out"), with: new[] { "hk-blur" });
        Check(st == 0 && outp.Contains("n/a  hk-blur: no Hollow Knight"), "hk-blur: another game: not applicable");
    }
}
