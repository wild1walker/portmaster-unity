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
        HollowKnightAspectTests();
        HollowKnightFadeTests();
        HollowKnightPadLettersTests();
    }

    // ---- hk-pad-letters ----------------------------------------------------------------------
    /// <summary>The stand-in's glyphs for Action1-4, Start and Select, then the controller picture, for a
    /// pad type, with UA_PAD_LETTERS set to env (a fresh load each time: the helper reads it once).</summary>
    static string Letters(string dir, string env, int padType)
    {
        var old = Environment.GetEnvironmentVariable(HollowKnightPadLetters.Variable);
        Environment.SetEnvironmentVariable(HollowKnightPadLetters.Variable, env);
        var ctx = new AssemblyLoadContext("hkl" + Guid.NewGuid(), isCollectible: true);
        try
        {
            Assembly a = ctx.LoadFromStream(new MemoryStream(File.ReadAllBytes(Path.Combine(dir, "Assembly-CSharp.dll"))));
            var pad = System.Enum.ToObject(a.GetType("GlobalEnums.GamepadType", true), padType);
            var controls = a.GetType("InControl.InputControlType", true);
            var skinsT = a.GetType("UIButtonSkins", true);
            var skins = Activator.CreateInstance(skinsT);
            var ih = skinsT.GetField("ih").GetValue(skins);
            ih.GetType().GetField("activeGamepadType").SetValue(ih, pad);
            var glyphs = new List<string>();
            foreach (var c in new[] { "Action1", "Action2", "Action3", "Action4", "Start", "Select" })
                glyphs.Add((string)skinsT.GetMethod("GetButtonSkinFor").Invoke(skins, new[] { System.Enum.Parse(controls, c) }));
            var detectT = a.GetType("ControllerDetect", true);
            var detect = Activator.CreateInstance(detectT);
            var dih = detectT.GetField("ih").GetValue(detect);
            dih.GetType().GetField("activeGamepadType").SetValue(dih, pad);
            detectT.GetMethod("LookForActiveController").Invoke(detect, null);
            return string.Join(",", glyphs) + " / " + detectT.GetField("Shown").GetValue(detect);
        }
        finally { ctx.Unload(); Environment.SetEnvironmentVariable(HollowKnightPadLetters.Variable, old); }
    }

    static void HollowKnightPadLettersTests()
    {
        var d = Dir("hkl");
        File.Copy(FakeHollowKnight, Path.Combine(d, "Assembly-CSharp.dll"));
        var (st, outp) = Patch(d, Path.Combine(tmp, "hkl-default"));
        Check(st == 0 && outp.Contains("skip hk-pad-letters (opt-in: --with=hk-pad-letters)"), "hk-pad-letters: opt-in, not run by default");
        var o = Path.Combine(tmp, "hkl-out");
        (st, outp) = Patch(d, o, with: new[] { "hk-pad-letters" });
        Check(st == 0 && outp.Contains("ok   hk-pad-letters: Assembly-CSharp.dll: UIButtonSkins.GetButtonSkinFor") &&
              File.Exists(Path.Combine(o, "Assembly-CSharp.dll")), "hk-pad-letters: applied when named");
        if (!File.Exists(Path.Combine(o, "Assembly-CSharp.dll"))) return;
        const int none = 0, xboxOne = 2, xbox360 = 3, ps4 = 4, switchPro = 10, ps5 = 11, steamDeck = 13;
        const string xbox = "a,b,x,y,start,select / XBOX_360", nintendo = "switchHidB,switchHidA,switchHidY,switchHidX,switchHidPlus,switchHidMinus / SWITCH_PRO_CONTROLLER";
        Check(Letters(d, null, xbox360) == xbox, $"hk-pad-letters: the unpatched stand-in shows the Xbox letters ({Letters(d, null, xbox360)})");
        foreach (var v in new[] { null, "", "nintendo", "XBOX", "xbox ", "1" })
            Check(Letters(o, v, xbox360) == nintendo, $"hk-pad-letters: UA_PAD_LETTERS={v ?? "(unset)"}: the Nintendo letters ({Letters(o, v, xbox360)})");
        Check(Letters(o, "xbox", xbox360) == xbox, $"hk-pad-letters: xbox: the game as it is ({Letters(o, "xbox", xbox360)})");
        foreach (var v in new[] { null, "xbox" })
            foreach (var t in new[] { none, xboxOne, ps4, switchPro, ps5, steamDeck })
                Check(Letters(o, v, t) == Letters(d, v, t), $"hk-pad-letters: {v ?? "(unset)"}, pad type {t}: as the game ({Letters(o, v, t)})");
        (st, outp) = Patch(o, Path.Combine(tmp, "hkl-again"), with: new[] { "hk-pad-letters" });
        Check(st == 0 && outp.Contains("done hk-pad-letters") && NoDlls(Path.Combine(tmp, "hkl-again")), "hk-pad-letters: a second run finds it applied");
    }

    // ---- hk-aspect ---------------------------------------------------------------------------
    /// <summary>The stand-in's viewport fit (width, height, height multiplier, aspect used) for a screen
    /// aspect, with ASMPATCH_HK_ASPECT set to env (a fresh load each time: the helper reads it once).</summary>
    static (float w, float h, float mult, float used) Fit(string dir, string env, float aspect)
    {
        var old = Environment.GetEnvironmentVariable(HollowKnightAspect.Variable);
        Environment.SetEnvironmentVariable(HollowKnightAspect.Variable, env);
        var ctx = new AssemblyLoadContext("hka" + Guid.NewGuid(), isCollectible: true);
        try
        {
            Assembly a = ctx.LoadFromStream(new MemoryStream(File.ReadAllBytes(Path.Combine(dir, "Assembly-CSharp.dll"))));
            var t = a.GetType("ForceCameraAspect", true);
            t.GetField("ScreenAspect").SetValue(null, aspect);
            var args = new object[] { 0f, 0f, 0f, 0f };
            float used = (float)t.GetMethod("AutoScaleViewportShared").Invoke(null, args);
            static float R(object x) => MathF.Round((float)x, 4);
            return (R(args[1]), R(args[2]), R(args[3]), R(used));
        }
        finally { ctx.Unload(); Environment.SetEnvironmentVariable(HollowKnightAspect.Variable, old); }
    }

    static void HollowKnightAspectTests()
    {
        var d = Dir("hka");
        File.Copy(FakeHollowKnight, Path.Combine(d, "Assembly-CSharp.dll"));
        var (st, outp) = Patch(d, Path.Combine(tmp, "hka-default"));
        Check(st == 0 && outp.Contains("skip hk-aspect (opt-in: --with=hk-aspect)"), "hk-aspect: opt-in, not run by default");
        var o = Path.Combine(tmp, "hka-out");
        (st, outp) = Patch(d, o, with: new[] { "hk-aspect" });
        Check(st == 0 && outp.Contains("ok   hk-aspect: Assembly-CSharp.dll: ForceCameraAspect.AutoScaleViewportShared") &&
              File.Exists(Path.Combine(o, "Assembly-CSharp.dll")), "hk-aspect: applied when named");
        if (!File.Exists(Path.Combine(o, "Assembly-CSharp.dll"))) return;
        float a43 = 4f / 3f, a32 = 1.5f, a169 = 16f / 9f, a1610 = 1.6f, a219 = 2.3333333f;
        var bars43 = (1f, 0.8333f, 1.1111f, 1.6f);
        Check(Fit(d, "fill", a43) == bars43, "hk-aspect: the unpatched stand-in letterboxes 4:3 whatever the variable");
        foreach (var v in new[] { null, "", "FILL", "fill ", "bars", "1" })
            Check(Fit(o, v, a43) == bars43, $"hk-aspect: ASMPATCH_HK_ASPECT={v ?? "(unset)"}: the game as it is ({Fit(o, v, a43)})");
        foreach (var (asp, want) in new[]
                 {
                     (a43, (1f, 1f, 1.3333f, 1.3333f)), (a32, (1f, 1f, 1.1852f, 1.5f)), (a1610, (1f, 1f, 1.1111f, 1.6f)),
                     (a169, (1f, 1f, 1f, 1.7778f)), (a219, (1f, 1f, 1f, 2.3333f)), (0.8f, (1f, 0.8f, 1.7778f, 1f)),
                 })
            Check(Fit(o, "fill", asp) == want, $"hk-aspect: fill at {asp}: {Fit(o, "fill", asp)}");
        Check(Fit(o, null, a169) == Fit(d, null, a169) && Fit(o, null, a1610) == Fit(d, null, a1610), "hk-aspect: unset: 16:9 and 16:10 as the game");
        (st, outp) = Patch(o, Path.Combine(tmp, "hka-again"), with: new[] { "hk-aspect" });
        Check(st == 0 && outp.Contains("done hk-aspect") && NoDlls(Path.Combine(tmp, "hka-again")), "hk-aspect: a second run finds it applied");
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

    /// <summary>CameraFadeDriver.Run on the Assembly-CSharp in dir (the firstpass stand-in beside it), and
    /// how many times the texture was uploaded.</summary>
    static (string log, int applies) DriveFade(string dir, int seed, int steps) =>
        RunFade(dir, a => (string)a.GetType("CameraFadeDriver", true).GetMethod("Run").Invoke(null, new object[] { seed, steps }));

    static (string log, int applies) RunFade(string dir, Func<Assembly, string> f)
    {
        var ctx = new AssemblyLoadContext("hkf" + Guid.NewGuid(), isCollectible: true);
        ctx.Resolving += (c, n) => n.Name == "Assembly-CSharp-firstpass" ? c.LoadFromStream(new MemoryStream(File.ReadAllBytes(Path.Combine(dir, n.Name + ".dll")))) : null;
        try
        {
            Assembly a = ctx.LoadFromStream(new MemoryStream(File.ReadAllBytes(Path.Combine(dir, "Assembly-CSharp.dll"))));
            var tex = ctx.LoadFromAssemblyName(new AssemblyName("Assembly-CSharp-firstpass")).GetType("UnityEngine.Texture2D", true);
            tex.GetField("Applies").SetValue(null, 0);
            var log = f(a);
            return (log, (int)tex.GetField("Applies").GetValue(null));
        }
        finally { ctx.Unload(); }
    }

    static void HollowKnightFadeTests()
    {
        if (!File.Exists(FakeHollowKnightFirstpass)) { Check(false, $"Hollow Knight firstpass stand-in at {FakeHollowKnightFirstpass} (build.sh test builds it)"); return; }
        var d = Dir("hkf");
        File.Copy(FakeHollowKnight, Path.Combine(d, "Assembly-CSharp.dll"));
        File.Copy(FakeHollowKnightFirstpass, Path.Combine(d, "Assembly-CSharp-firstpass.dll"));
        var (st, outp) = Patch(d, Path.Combine(tmp, "hkf-default"));
        Check(st == 0 && outp.Contains("skip hk-camerafade (opt-in: --with=hk-camerafade)") && NoDlls(Path.Combine(tmp, "hkf-default")),
              "hk-camerafade: opt-in, not run by default");
        var o = Path.Combine(tmp, "hkf-out");
        (st, outp) = Patch(d, o, with: new[] { "hk-camerafade" });
        Check(st == 0 && outp.Contains("ok   hk-camerafade: Assembly-CSharp.dll: CameraFade.SetScreenOverlayColor") && File.Exists(Path.Combine(o, "Assembly-CSharp.dll")),
              "hk-camerafade: applied when named");
        if (!File.Exists(Path.Combine(o, "Assembly-CSharp.dll"))) return;
        File.Copy(FakeHollowKnightFirstpass, Path.Combine(o, "Assembly-CSharp-firstpass.dll"), true);
        int same = 0, a0 = 0, a1 = 0, labels = 0, threw = 0;
        string first = null;
        for (int seed = 1; seed <= 80; seed++)
        {
            var (la, pa) = DriveFade(d, seed, 600); var (lb, pb) = DriveFade(o, seed, 600);
            a0 += pa; a1 += pb; labels += la.Split("label ").Length - 1; threw += la.Split(" threw ").Length - 1;
            if (la == lb) same++;
            else if (first == null)
            {
                var x = la.Split('\n'); var y = lb.Split('\n');
                int k = 0; while (k < x.Length && k < y.Length && x[k] == y[k]) k++;
                first = $"seed {seed}: {(k < x.Length ? x[k] : "")} / {(k < y.Length ? y[k] : "")}";
            }
        }
        Check(same == 80, $"hk-camerafade: every field, pixel (CPU and GPU), label and exception the same after each step ({same} of 80 runs{(first == null ? "" : "; " + first)})");
        Check(labels > 1000 && threw > 0, $"hk-camerafade: the runs draw the fade ({labels} labels) and hit the exception before Awake ({threw})");
        Check(a1 < a0, $"hk-camerafade: uploads in the seeded runs {a0} -> {a1}");
        string Idle(Assembly a) => (string)a.GetType("CameraFadeDriver", true).GetMethod("Idle").Invoke(null, new object[] { 1000 });
        var (ia, ua) = RunFade(d, Idle); var (ib, ub) = RunFade(o, Idle);
        Check(ia == ib && ua == 1000 && ub == 1, $"hk-camerafade: a fade nothing started, 1,000 events: the same state ({ia}), uploads {ua} -> {ub}");
        var again = Path.Combine(tmp, "hkf-again");
        (st, outp) = Patch(o, again, only: new[] { "hk-camerafade" });
        Check(st == 0 && outp.Contains("done hk-camerafade") && NoDlls(again), "hk-camerafade: a second run finds it applied");
    }
}
