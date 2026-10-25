// hk-blur on a stand-in Assembly-CSharp (test/fakes/HollowKnight, built by build.sh test): opt-in,
// then patched, loaded and started with ASMPATCH_HK_BLUR unset and set to each of its words.
using System.Runtime.Loader;
using Assembly = System.Reflection.Assembly;
using AssemblyName = System.Reflection.AssemblyName;

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
        HollowKnightPostTests();
    }

    // ---- hk-post -----------------------------------------------------------------------------
    static readonly string FakeHollowKnightFirstpass = Path.Combine(AppContext.BaseDirectory, "fakes", "hk", "Assembly-CSharp-firstpass.dll");

    /// <summary>Runs the post chain's frames on the stand-ins (dir: patched or not; orig: where
    /// the other assembly is) with ASMPATCH_HK_POST set to env and the brightness material's
    /// passes named as given. Each frame is a word of letters: b the bloom (scene -> t1), c the
    /// curves (t1 -> t2), B the brightness (t2 -> dest); C / X the curves / brightness disabled and
    /// not called, n the curves enabled but not called, x the brightness enabled but not called, D
    /// the camera's texture display active. Returns per frame the full-screen blits (the bloom's
    /// small passes left out), then the temporaries still taken, the brightness material's
    /// FindPass lookups and its _Bloom, _RgbTex and _Saturation.</summary>
    static (string[] frames, int outstanding, int lookups, string bloom, string curves, float saturation) PostChain(
        string dir, string orig, string env, string[] passNames, params string[] frames)
    {
        var old = Environment.GetEnvironmentVariable(HollowKnightPost.Variable);
        Environment.SetEnvironmentVariable(HollowKnightPost.Variable, env);
        var ctx = new AssemblyLoadContext("hkp" + Guid.NewGuid(), isCollectible: true);
        ctx.Resolving += (c, n) =>
        {
            foreach (var d in new[] { dir, orig })
            {
                var p = Path.Combine(d, n.Name + ".dll");
                if (File.Exists(p)) return c.LoadFromStream(new MemoryStream(File.ReadAllBytes(p)));
            }
            return null;
        };
        try
        {
            var fp = ctx.LoadFromAssemblyName(new AssemblyName("Assembly-CSharp-firstpass"));
            var cs = ctx.LoadFromAssemblyName(new AssemblyName("Assembly-CSharp"));
            var rtT = fp.GetType("UnityEngine.RenderTexture", true);
            var gfxT = fp.GetType("UnityEngine.Graphics", true);
            var bloomT = fp.GetType("UnityStandardAssets.ImageEffects.BloomOptimized", true);
            var curvesT = fp.GetType("UnityStandardAssets.ImageEffects.ColorCorrectionCurves", true);
            var brightT = cs.GetType("BrightnessEffect", true);
            var displayT = cs.GetType("GameCameraTextureDisplay", true);
            object Rt(string name)
            {
                var rt = Activator.CreateInstance(rtT);
                rtT.GetField("Name").SetValue(rt, name);
                return rt;
            }
            object scene = Rt("scene"), t1 = Rt("t1"), t2 = Rt("t2"), dest = Rt("dest");
            var bloom = Activator.CreateInstance(bloomT);
            var curves = Activator.CreateInstance(curvesT);
            var bright = Activator.CreateInstance(brightT);
            var display = Activator.CreateInstance(displayT);
            curvesT.GetField("saturation").SetValue(curves, 0.8f);
            var brightMat = brightT.GetProperty("Material").GetValue(bright);
            brightMat.GetType().GetField("PassNames").SetValue(brightMat, passNames);
            var log = (System.Collections.Generic.List<string>)gfxT.GetField("Log").GetValue(null);
            var render = new Dictionary<char, (object self, Type t, object src, object dst)>
            {
                ['b'] = (bloom, bloomT, scene, t1), ['c'] = (curves, curvesT, t1, t2), ['B'] = (bright, brightT, t2, dest),
            };
            var enabledF = fp.GetType("UnityEngine.Behaviour", true).GetField("enabled");
            var outFrames = new List<string>();
            foreach (var frame in frames)
            {
                log.Clear();
                enabledF.SetValue(curves, !frame.Contains('C'));
                enabledF.SetValue(bright, !frame.Contains('X'));
                displayT.GetField("Instance").SetValue(null, frame.Contains('D') ? display : null);
                foreach (var ch in frame)
                    if (render.TryGetValue(ch, out var r))
                        r.t.GetMethod("OnRenderImage").Invoke(r.self, new[] { r.src, r.dst });
                outFrames.Add(string.Join(" ", log.Where(l => !l.Contains("temp180x112"))));
            }
            var mt = brightMat.GetType();
            return (outFrames.ToArray(), (int)rtT.GetField("Outstanding").GetValue(null), (int)mt.GetField("Lookups").GetValue(brightMat),
                    mt.GetMethod("GetTexture").Invoke(brightMat, new object[] { HollowKnightPost.BloomTex })?.ToString(),
                    mt.GetMethod("GetTexture").Invoke(brightMat, new object[] { HollowKnightPost.CurvesTex })?.ToString(),
                    (float)mt.GetMethod("GetFloat").Invoke(brightMat, new object[] { HollowKnightPost.Saturation }));
        }
        finally { ctx.Unload(); Environment.SetEnvironmentVariable(HollowKnightPost.Variable, old); }
    }

    static void HollowKnightPostTests()
    {
        if (!File.Exists(FakeHollowKnightFirstpass)) { Check(false, $"Hollow Knight firstpass stand-in at {FakeHollowKnightFirstpass} (build.sh test builds it)"); return; }
        var d = Dir("hkp");
        File.Copy(FakeHollowKnight, Path.Combine(d, "Assembly-CSharp.dll"));
        File.Copy(FakeHollowKnightFirstpass, Path.Combine(d, "Assembly-CSharp-firstpass.dll"));
        var (st, outp) = Patch(d, Path.Combine(tmp, "hkp-default"));
        Check(st == 0 && outp.Contains("skip hk-post (opt-in: --with=hk-post)") && NoDlls(Path.Combine(tmp, "hkp-default")), "hk-post: opt-in, not run by default");
        var o = Path.Combine(tmp, "hkp-out");
        (st, outp) = Patch(d, o, with: new[] { "hk-post" });
        Check(st == 0 && outp.Contains("ok   hk-post: Assembly-CSharp-firstpass.dll: BloomOptimized's composite blit") &&
              File.Exists(Path.Combine(o, "Assembly-CSharp.dll")) && File.Exists(Path.Combine(o, "Assembly-CSharp-firstpass.dll")),
              "hk-post: applied when named, both assemblies written");
        if (!File.Exists(Path.Combine(o, "Assembly-CSharp-firstpass.dll"))) return;
        string[] plain = { "" }, fusedPasses = { "", HollowKnightPost.FusedPass }, swapped = { HollowKnightPost.FusedPass, "" };
        const string game = "scene>t1:bloom#0 t1>t2:curves t2>dest:brightness", asIs = game + "#0", fused = "scene>dest:brightness#1";
        var r = PostChain(d, d, "fused", fusedPasses, "bcB", "bcB", "bcB");
        Check(r.frames.All(f => f == game) && r.outstanding == 0, $"hk-post: the unpatched stand-in draws the three passes ({string.Join(" | ", r.frames)})");
        foreach (var env in new[] { null, "", "fuse", "FUSED", "fused " })
        {
            r = PostChain(o, d, env, fusedPasses, "bcB", "bcB", "bcB");
            Check(r.frames.All(f => f == asIs) && r.outstanding == 0 && r.lookups == 0,
                  $"hk-post: ASMPATCH_HK_POST={env ?? "(unset)"}: the game as it is ({string.Join(" | ", r.frames)})");
        }
        r = PostChain(o, d, "fused", fusedPasses, "bcB", "bcB", "bcB", "bcB");
        Check(r.frames.SequenceEqual(new[] { asIs, fused, fused, fused }), $"hk-post: fused from the second chain on ({string.Join(" | ", r.frames)})");
        Check(r.outstanding == 1 && r.lookups == 1 && r.bloom == "temp180x112" && r.curves == "rgb" && r.saturation == 0.8f,
              $"hk-post: the brightness material gets the bloom texture, the curves and the saturation; the pass looked up once; the bloom texture kept " +
              $"for the next chain ({r.outstanding} taken, {r.lookups} lookups, {r.bloom}, {r.curves}, {r.saturation})");
        r = PostChain(o, d, "fused", swapped, "bcB", "bcB");
        Check(r.frames[1] == "scene>dest:brightness#0", $"hk-post: the pass found by name ({r.frames[1]})");
        r = PostChain(o, d, "fused", plain, "bcB", "bcB", "bcB");
        Check(r.frames.All(f => f == asIs) && r.lookups == 1 && r.outstanding == 0, $"hk-post: no fused pass in the material: the game as it is ({string.Join(" | ", r.frames)})");
        r = PostChain(o, d, "fused", fusedPasses, "bcB", "bCB", "bcB", "bcB");
        Check(r.frames.SequenceEqual(new[] { asIs, "scene>t1:bloom#0 t2>dest:brightness#0", asIs, fused }),
              $"hk-post: the curves off: not fused until they ran a whole chain again ({string.Join(" | ", r.frames)})");
        r = PostChain(o, d, "fused", fusedPasses, "bcB", "bcX", "bcB", "bcB");
        Check(r.frames.SequenceEqual(new[] { asIs, "scene>t1:bloom#0 t1>t2:curves", asIs, fused }),
              $"hk-post: the brightness off: not fused until it ran a whole chain again ({string.Join(" | ", r.frames)})");
        r = PostChain(o, d, "fused", fusedPasses, "bcB", "bcx", "bcB", "bcB");
        Check(r.frames.SequenceEqual(new[] { asIs, "", asIs, fused }) && r.outstanding == 1,
              $"hk-post: a fused chain whose brightness never came: the next drawn as the game's, nothing leaked ({string.Join(" | ", r.frames)}, {r.outstanding} taken)");
        r = PostChain(o, d, "fused", fusedPasses, "bcB", "bnB", "bcB");
        Check(r.frames.SequenceEqual(new[] { asIs, "scene>temp720x450:bloom#0 temp720x450>dest:brightness#0", asIs }) && r.outstanding == 0,
              $"hk-post: the curves enabled but not called in a fused chain: the bloom's composite and the brightness, as the game's ({string.Join(" | ", r.frames)})");
        r = PostChain(o, d, "fused", fusedPasses, "bcBD", "bcBD", "bcB");
        Check(r.frames.SequenceEqual(new[] { asIs + " t2>display:brightness#0", fused + " scene>display:brightness#1", fused }),
              $"hk-post: the camera's texture display gets the pre-brightness picture, or the scene with the fused pass ({string.Join(" | ", r.frames)})");
        (st, outp) = Patch(o, Path.Combine(tmp, "hkp-again"), with: new[] { "hk-post" });
        Check(st == 0 && outp.Contains("done hk-post") && NoDlls(Path.Combine(tmp, "hkp-again")), "hk-post: a second run finds it applied");
        (st, outp) = Patch(Dir("hk"), Path.Combine(tmp, "hkp-half"), with: new[] { "hk-post" });
        Check(st == 0 && outp.Contains("n/a  hk-post: no Hollow Knight"), "hk-post: without the effects' assembly: not applicable");
        (st, outp) = Patch(d, Path.Combine(tmp, "hkp-both"), with: new[] { "hk-blur", "hk-post" });
        Check(st == 0 && outp.Contains("ok   hk-blur") && outp.Contains("ok   hk-post"), "hk-post: with hk-blur in one run");
    }
}
