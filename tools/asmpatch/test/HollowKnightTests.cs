// hk-blur on a stand-in Assembly-CSharp (test/fakes/HollowKnight, built by build.sh test): opt-in,
// then patched, loaded and started with ASMPATCH_HK_BLUR unset, "full", "half" and "off".
using System.Runtime.Loader;
using Assembly = System.Reflection.Assembly;

namespace AsmPatch.Tests;

static partial class TestMain
{
    static readonly string FakeHollowKnight = Path.Combine(AppContext.BaseDirectory, "fakes", "hk", "Assembly-CSharp.dll");

    /// <summary>(texture width, height, cancelled, plane renderer enabled) after the rig's start at aspect 1.6.</summary>
    static (int w, int h, bool cancelled, bool visible) StartRig(string dir, string blur)
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
            return ((int)lbbT.GetField("TextureWidth").GetValue(lbb), (int)lbbT.GetField("TextureHeight").GetValue(lbb),
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
        var full = (576, 360, false, true);
        Check(StartRig(d, "off") == full, "hk-blur: the unpatched stand-in ignores the variable");
        foreach (var v in new[] { null, "", "full", "HALF" })
            Check(StartRig(o, v) == full, $"hk-blur: ASMPATCH_HK_BLUR={v ?? "(unset)"}: the game as it is");
        Check(StartRig(o, "half") == (288, 180, false, true), $"hk-blur: half: the texture at half size {StartRig(o, "half")}");
        var off = StartRig(o, "off");
        Check(off.cancelled && !off.visible, $"hk-blur: off: no background camera, the blur plane hidden {off}");
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
