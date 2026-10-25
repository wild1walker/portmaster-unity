// hk-post (kind Reduce, opt-in: the picture may change by a rounding step, never the game play).
// Hollow Knight's main camera ends its frame with three full-screen image effects, each a blit of
// the previous one's output into a texture of its own: BloomOptimized's composite (the scene plus
// its bloom texture), ColorCorrectionCurves (the `_RgbTex` curves and saturation) and the game's
// BrightnessEffect (brightness and contrast); on the RG34XX-SP about 3.7 ms of GPU a frame at
// 720x450 plus a job chain of driver time each (docs/profiling.md, "GPU, pass by pass"). The
// converter's shaders.fused_post appends to the brightness shader a pass that computes all three
// from the scene, the bloom texture and the curves texture (named PMUNITY_FUSED_POST). With this
// patch and ASMPATCH_HK_POST=fused, where the brightness material has that pass:
//   - BloomOptimized still draws its bloom (the downsample and blur passes at a fraction of the
//     size) but skips its composite blit, keeping the scene texture, its material (which holds the
//     bloom texture) and the bloom texture for the end of the chain;
//   - ColorCorrectionCurves skips its blit (its material holds the curves texture and saturation);
//   - BrightnessEffect copies `_Bloom`, `_RgbTex` and `_Saturation` from those materials into its
//     own and draws the fused pass from the scene into its destination; the camera's
//     GameCameraTextureDisplay, which the game gives the pre-brightness texture and the brightness
//     material, gets the scene and the fused pass instead (the same picture).
// The chain fuses only while all three run in that order every frame: each effect's blit records
// that it ran in the current chain (counted by the bloom's), and the bloom fuses a chain only when
// the other two ran in the previous one, are active and enabled now, and the pass exists; a chain
// whose brightness never came is drawn as the game draws it from the next one on, and the bloom
// texture kept for the fused draw is released at the next chain's start. Without the variable, or
// without the pass, every blit is the game's own (the brightness blits name their pass 0, which is
// the only one the game's shader had: the same draw). Nothing else changes: no timing, physics,
// collision or FSM; the effects' enabled states stay the game's.
using Mono.Cecil;
using Mono.Cecil.Cil;
using Mono.Cecil.Rocks;

namespace AsmPatch;

sealed class HollowKnightPost : Patch
{
    public override string Name => "hk-post";
    public override Kind Kind => Kind.Reduce;
    public override bool OptIn => true;
    public override string Summary =>
        "Hollow Knight's bloom composite, colour curves and brightness drawn as one pass where the data has shaders.fused_post's pass, chosen at run time by ASMPATCH_HK_POST=fused (a rounding step of difference; opt-in)";

    public const string Variable = "ASMPATCH_HK_POST", HelperType = "<asmpatch>HollowKnightPost";
    /// <summary>The fused pass's name (convert/pmunity/shaders/fusedpost.py's default) and the
    /// properties the fused pass reads from the other two materials.</summary>
    public const string FusedPass = "PMUNITY_FUSED_POST", BloomTex = "_Bloom", CurvesTex = "_RgbTex", Saturation = "_Saturation";

    public override Result Run(Game game, bool apply)
    {
        TypeDefinition Find(string name) => game.GameAssemblies.SelectMany(a => a.MainModule.Types).FirstOrDefault(t => t.Name == name);
        var bloom = Find("BloomOptimized");
        var curves = Find("ColorCorrectionCurves");
        var bright = Find("BrightnessEffect");
        if (bloom == null || curves == null || bright == null)
            return Result.NotApplicable("no Hollow Knight (BloomOptimized, ColorCorrectionCurves, BrightnessEffect)");
        var bm = bloom.Module;
        var bfile = game.FileOf(bm.Assembly);
        var brfile = game.FileOf(bright.Module.Assembly);
        if (bm.Types.Any(t => t.Name == HelperType))
            return new Result { Outcome = Outcome.AlreadyApplied, Details = { $"{bfile}: the post chain follows {Variable}" } };
        if (curves.Module != bm)
            return Result.Failed($"{bfile}: ColorCorrectionCurves is not in BloomOptimized's assembly");
        MethodDefinition Render(TypeDefinition t) => t.Methods.FirstOrDefault(x => x.Name == "OnRenderImage" && !x.IsStatic &&
            x.Parameters.Count == 2 && x.HasBody);
        var bloomRender = Render(bloom);
        var curvesRender = Render(curves);
        var brightRender = Render(bright);
        if (bloomRender == null || curvesRender == null || brightRender == null)
            return Result.Failed($"{bfile}: BloomOptimized / ColorCorrectionCurves / BrightnessEffect have no OnRenderImage(source, destination)");
        static bool IsBlit(Instruction i, int args) => i.OpCode.Code == Code.Call && i.Operand is MethodReference b && b.Name == "Blit" &&
            b.DeclaringType.Name == "Graphics" && b.Parameters.Count == args &&
            (args < 3 || b.Parameters[2].ParameterType.Name == "Material") &&
            (args < 4 || b.Parameters[3].ParameterType.MetadataType == MetadataType.Int32);
        // the bloom's composite: its one Graphics.Blit(source, destination, material, 0), then the ReleaseTemporary of its bloom texture
        var bloomBody = bloomRender.Body.Instructions;
        var composites = bloomBody.Where(i => IsBlit(i, 4) && i.Previous != null && Cecil.ConstInt(i.Previous) == 0).ToList();
        if (composites.Count != 1)
            return Result.Failed($"{bfile}: BloomOptimized.OnRenderImage has {composites.Count} Graphics.Blit(source, destination, material, 0) (one expected)");
        var composite = composites[0];
        var release = composite;
        while (release != null && !(release.OpCode.Code == Code.Call && release.Operand is MethodReference rr && rr.Name == "ReleaseTemporary" &&
                                    rr.DeclaringType.Name == "RenderTexture"))
            release = release.Next;
        var blits4 = bloomBody.Where(i => IsBlit(i, 4)).Select(i => (MethodReference)i.Operand).ToList();
        var getTemp = bloomBody.Select(i => i.Operand).OfType<MethodReference>().FirstOrDefault(r => r.Name == "GetTemporary" &&
            r.DeclaringType.Name == "RenderTexture" && r.Parameters.Count == 4 && r.Parameters[3].ParameterType.Name == "RenderTextureFormat");
        var getFormat = bloomBody.Select(i => i.Operand).OfType<MethodReference>().FirstOrDefault(r => r.Name == "get_format" && r.DeclaringType.Name == "RenderTexture");
        var getWidth = bloomBody.Select(i => i.Operand).OfType<MethodReference>().FirstOrDefault(r => r.Name == "get_width" && r.DeclaringType.Name == "Texture");
        var getHeight = bloomBody.Select(i => i.Operand).OfType<MethodReference>().FirstOrDefault(r => r.Name == "get_height" && r.DeclaringType.Name == "Texture");
        var curvesBlits = curvesRender.Body.Instructions.Where(i => IsBlit(i, 3)).ToList();
        var brightBlits = brightRender.Body.Instructions.Where(i => IsBlit(i, 3)).ToList();
        if (release == null || blits4.Count == 0 || getTemp == null || getFormat == null || getWidth == null || getHeight == null)
            return Result.Failed($"{bfile}: BloomOptimized.OnRenderImage is not as this patch knows it (its temporaries and blits)");
        if (curvesBlits.Count != 1 || brightBlits.Count != 1)
            return Result.Failed($"{bfile} / {brfile}: ColorCorrectionCurves / BrightnessEffect.OnRenderImage have {curvesBlits.Count} / {brightBlits.Count} " +
                                 "Graphics.Blit(source, destination, material) (one each expected)");
        var blit4 = blits4[0];
        var blit3 = (MethodReference)curvesBlits[0].Operand;
        var material = blit4.Parameters[2].ParameterType;
        var texture = blit4.Parameters[0].ParameterType;
        var renderTexture = blit4.Parameters[1].ParameterType;
        var matDef = Cecil.TryResolve(material);
        MethodDefinition Mat(string n, Func<MethodDefinition, bool> ok) => matDef?.Methods.FirstOrDefault(x => x.Name == n && !x.IsStatic && x.HasThis && ok(x));
        var findPass = Mat("FindPass", x => x.Parameters.Count == 1 && x.Parameters[0].ParameterType.MetadataType == MetadataType.String && x.ReturnType.MetadataType == MetadataType.Int32);
        var getTexture = Mat("GetTexture", x => x.Parameters.Count == 1 && x.Parameters[0].ParameterType.MetadataType == MetadataType.String);
        var setTexture = Mat("SetTexture", x => x.Parameters.Count == 2 && x.Parameters[0].ParameterType.MetadataType == MetadataType.String);
        var getFloat = Mat("GetFloat", x => x.Parameters.Count == 1 && x.Parameters[0].ParameterType.MetadataType == MetadataType.String);
        var setFloat = Mat("SetFloat", x => x.Parameters.Count == 2 && x.Parameters[0].ParameterType.MetadataType == MetadataType.String &&
                                          x.Parameters[1].ParameterType.MetadataType == MetadataType.Single);
        var behaviour = Cecil.Hierarchy(bloom).FirstOrDefault(t => t.Name == "Behaviour");
        var isActive = behaviour?.Methods.FirstOrDefault(x => x.Name == "get_isActiveAndEnabled" && !x.IsStatic && x.Parameters.Count == 0);
        if (findPass == null || getTexture == null || setTexture == null || getFloat == null || setFloat == null || isActive == null)
            return Result.Failed($"{bfile}: UnityEngine's Material (FindPass, GetTexture, SetTexture, GetFloat, SetFloat) or Behaviour (isActiveAndEnabled) not found");
        if (!Cecil.Hierarchy(curves).Contains(behaviour) || !Cecil.Hierarchy(bright).Contains(behaviour))
            return Result.Failed($"{bfile}: ColorCorrectionCurves / BrightnessEffect are not Behaviours");
        // the camera's texture display (Assembly-CSharp: GameCameraTextureDisplay.UpdateDisplay(source, material)), where the game has it
        var display = Find("GameCameraTextureDisplay");
        var update = display?.Methods.FirstOrDefault(x => x.Name == "UpdateDisplay" && !x.IsStatic && x.Parameters.Count == 2 && x.HasBody);
        var displayBlits = update?.Body.Instructions.Where(i => IsBlit(i, 3)).ToList();
        if (display != null && (update == null || displayBlits.Count != 1))
            return Result.Failed($"{game.FileOf(display.Module.Assembly)}: GameCameraTextureDisplay.UpdateDisplay has no one Graphics.Blit(source, destination, material)");

        var r = new Result { Outcome = Outcome.Applied };
        r.Details.Add($"{bfile}: BloomOptimized's composite blit and bloom texture release, ColorCorrectionCurves' blit; {brfile}: BrightnessEffect's blit" +
                      (display != null ? ", GameCameraTextureDisplay's blit" : "") + $" follow {Variable} (fused)");
        if (!apply) return r;

        var h = Helper(bm, material, texture, renderTexture, bm.ImportReference(behaviour), bm.ImportReference(isActive),
                       blit4, blit3, bm.ImportReference(getTemp), bm.ImportReference(getFormat), bm.ImportReference(getWidth), bm.ImportReference(getHeight),
                       bm.ImportReference(release.Operand as MethodReference), bm.ImportReference(findPass), bm.ImportReference(getTexture),
                       bm.ImportReference(setTexture), bm.ImportReference(getFloat), bm.ImportReference(setFloat));
        static void Redirect(MethodDefinition md, Instruction call, MethodReference to, bool withThis)
        {
            md.Body.SimplifyMacros();
            var il = md.Body.GetILProcessor();
            if (withThis) il.InsertBefore(call, il.Create(OpCodes.Ldarg_0));
            call.Operand = to;
            md.Body.OptimizeMacros();
        }
        Redirect(bloomRender, composite, h.bloomBlit, true);
        release.Operand = h.bloomRelease;
        Redirect(curvesRender, curvesBlits[0], h.curvesBlit, true);
        var brm = bright.Module;
        Redirect(brightRender, brightBlits[0], brm == bm ? h.brightBlit : brm.ImportReference(h.brightBlit), true);
        game.MarkChanged(bm.Assembly);
        game.MarkChanged(brm.Assembly);
        if (display != null)
        {
            var dm = display.Module;
            Redirect(update, displayBlits[0], dm == bm ? h.displayBlit : dm.ImportReference(h.displayBlit), false);
            game.MarkChanged(dm.Assembly);
        }
        return r;
    }

    /// <summary>The helper class (public: BrightnessEffect's assembly calls it): the chain's state and
    /// the five blits (the class comment).</summary>
    static (MethodDefinition bloomBlit, MethodDefinition bloomRelease, MethodDefinition curvesBlit, MethodDefinition brightBlit, MethodDefinition displayBlit)
        Helper(ModuleDefinition m, TypeReference material, TypeReference texture, TypeReference renderTexture, TypeReference behaviour, MethodReference isActive,
               MethodReference blit4, MethodReference blit3, MethodReference getTemp, MethodReference getFormat, MethodReference getWidth, MethodReference getHeight,
               MethodReference releaseTemp, MethodReference findPass, MethodReference getTexture, MethodReference setTexture, MethodReference getFloat, MethodReference setFloat)
    {
        var ts = m.TypeSystem;
        var t = new TypeDefinition("", HelperType, TypeAttributes.Public | TypeAttributes.Abstract | TypeAttributes.Sealed |
                                                   TypeAttributes.BeforeFieldInit | TypeAttributes.Class, ts.Object);
        m.Types.Add(t);
        FieldDefinition F(string name, TypeReference type)
        {
            var f = new FieldDefinition(name, FieldAttributes.Private | FieldAttributes.Static, type);
            t.Fields.Add(f);
            return f;
        }
        var flags = F("flags", ts.Int32);          // 0 unread; 1 read | 2 fused
        var blooms = F("blooms", ts.Int32);        // chains so far (the bloom's blits)
        var curvesAt = F("curvesAt", ts.Int32);    // the chain the curves last blitted in
        var brightAt = F("brightAt", ts.Int32);    // the chain the brightness last blitted in
        var pending = F("pending", ts.Boolean);    // this chain is fused: the composite was skipped
        var fusedNow = F("fusedNow", ts.Boolean);  // the brightness drew the fused pass in this chain
        var bloomSelf = F("bloomSelf", behaviour);
        var curves = F("curves", behaviour);
        var bright = F("bright", behaviour);
        var scene = F("scene", texture);
        var bloomMat = F("bloomMat", material);
        var curvesMat = F("curvesMat", material);
        var brightMat = F("brightMat", material);
        var bloomRt = F("bloomRt", renderTexture);
        var passMat = F("passMat", material);
        var pass = F("pass", ts.Int32);
        MethodDefinition Static(string name, TypeReference ret, params TypeReference[] ps)
        {
            var md = new MethodDefinition(name, MethodAttributes.Public | MethodAttributes.Static | MethodAttributes.HideBySig, ret);
            foreach (var p in ps) md.Parameters.Add(new ParameterDefinition(p));
            md.Body.InitLocals = true;
            t.Methods.Add(md);
            return md;
        }
        var env = new MethodReference("GetEnvironmentVariable", ts.String, new TypeReference("System", "Environment", m, ts.CoreLibrary));
        env.Parameters.Add(new ParameterDefinition(ts.String));
        var concat = new MethodReference("Concat", ts.String, ts.String);
        for (int k = 0; k < 3; k++) concat.Parameters.Add(new ParameterDefinition(ts.String));
        var contains = new MethodReference("Contains", ts.Boolean, ts.String) { HasThis = true };
        contains.Parameters.Add(new ParameterDefinition(ts.String));
        // static bool Fused(): read once: if (flags == 0) { int f = 1; string v = Environment.GetEnvironmentVariable(V);
        //   if (v != null && ("," + v + ",").Contains(",fused,")) f |= 2; flags = f; } return (flags & 2) != 0;
        var fused = Static("Fused", ts.Boolean);
        {
            var f = new VariableDefinition(ts.Int32); var v = new VariableDefinition(ts.String);
            fused.Body.Variables.Add(f); fused.Body.Variables.Add(v);
            var il = fused.Body.GetILProcessor();
            var done = il.Create(OpCodes.Ldsfld, flags);
            var store = il.Create(OpCodes.Ldloc, f);
            il.Emit(OpCodes.Ldsfld, flags); il.Emit(OpCodes.Brtrue, done);
            il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Stloc, f);
            il.Emit(OpCodes.Ldstr, Variable); il.Emit(OpCodes.Call, env); il.Emit(OpCodes.Stloc, v);
            il.Emit(OpCodes.Ldloc, v); il.Emit(OpCodes.Brfalse, store);
            il.Emit(OpCodes.Ldstr, ","); il.Emit(OpCodes.Ldloc, v); il.Emit(OpCodes.Ldstr, ","); il.Emit(OpCodes.Call, concat);
            il.Emit(OpCodes.Ldstr, ",fused,"); il.Emit(OpCodes.Callvirt, contains); il.Emit(OpCodes.Brfalse, store);
            il.Emit(OpCodes.Ldloc, f); il.Emit(OpCodes.Ldc_I4_3); il.Emit(OpCodes.Or); il.Emit(OpCodes.Stloc, f);
            il.Append(store); il.Emit(OpCodes.Stsfld, flags);
            il.Append(done); il.Emit(OpCodes.Ldc_I4_2); il.Emit(OpCodes.And); il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Cgt_Un); il.Emit(OpCodes.Ret);
        }
        // static int Pass(): the fused pass's index in the brightness material (looked up once per material), -1 without
        //   if (brightMat == null) return -1; if ((object)brightMat != passMat) { pass = brightMat.FindPass(NAME); passMat = brightMat; } return pass;
        var passOf = Static("Pass", ts.Int32);
        {
            var il = passOf.Body.GetILProcessor();
            var none = il.Create(OpCodes.Ldc_I4_M1);
            var known = il.Create(OpCodes.Ldsfld, pass);
            il.Emit(OpCodes.Ldsfld, brightMat); il.Emit(OpCodes.Brfalse, none);
            il.Emit(OpCodes.Ldsfld, brightMat); il.Emit(OpCodes.Ldsfld, passMat); il.Emit(OpCodes.Beq, known);
            il.Emit(OpCodes.Ldsfld, brightMat); il.Emit(OpCodes.Ldstr, FusedPass); il.Emit(OpCodes.Callvirt, findPass); il.Emit(OpCodes.Stsfld, pass);
            il.Emit(OpCodes.Ldsfld, brightMat); il.Emit(OpCodes.Stsfld, passMat);
            il.Append(known); il.Emit(OpCodes.Ret);
            il.Append(none); il.Emit(OpCodes.Ret);
        }
        // static void BloomBlit(Texture src, RenderTexture dst, Material m, int p, Behaviour self):
        //   if (bloomRt != null) { RenderTexture.ReleaseTemporary(bloomRt); bloomRt = null; }
        //   fusedNow = false;
        //   bool fuse = Fused() && !pending && self == bloomSelf && curves != null && bright != null
        //               && curvesAt == blooms && brightAt == blooms && curves.isActiveAndEnabled && bright.isActiveAndEnabled && Pass() >= 0;
        //   bloomSelf = self; pending = false; blooms++;
        //   if (fuse) { pending = true; scene = src; bloomMat = m; return; }
        //   Graphics.Blit(src, dst, m, p);
        var bloomBlit = Static("BloomBlit", ts.Void, texture, renderTexture, material, ts.Int32, behaviour);
        {
            var fuse = new VariableDefinition(ts.Boolean);
            bloomBlit.Body.Variables.Add(fuse);
            var il = bloomBlit.Body.GetILProcessor();
            var noRt = il.Create(OpCodes.Ldc_I4_0);               // fusedNow = false
            var decided = il.Create(OpCodes.Ldarg_S, bloomBlit.Parameters[4]);   // bloomSelf = self
            var plain = il.Create(OpCodes.Ldarg_0);
            il.Emit(OpCodes.Ldsfld, bloomRt); il.Emit(OpCodes.Brfalse, noRt);
            il.Emit(OpCodes.Ldsfld, bloomRt); il.Emit(OpCodes.Call, releaseTemp);
            il.Emit(OpCodes.Ldnull); il.Emit(OpCodes.Stsfld, bloomRt);
            il.Append(noRt); il.Emit(OpCodes.Stsfld, fusedNow);
            il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Stloc, fuse);
            il.Emit(OpCodes.Call, fused); il.Emit(OpCodes.Brfalse, decided);
            il.Emit(OpCodes.Ldsfld, pending); il.Emit(OpCodes.Brtrue, decided);
            il.Emit(OpCodes.Ldarg_S, bloomBlit.Parameters[4]); il.Emit(OpCodes.Ldsfld, bloomSelf); il.Emit(OpCodes.Bne_Un, decided);
            il.Emit(OpCodes.Ldsfld, curves); il.Emit(OpCodes.Brfalse, decided);
            il.Emit(OpCodes.Ldsfld, bright); il.Emit(OpCodes.Brfalse, decided);
            il.Emit(OpCodes.Ldsfld, curvesAt); il.Emit(OpCodes.Ldsfld, blooms); il.Emit(OpCodes.Bne_Un, decided);
            il.Emit(OpCodes.Ldsfld, brightAt); il.Emit(OpCodes.Ldsfld, blooms); il.Emit(OpCodes.Bne_Un, decided);
            il.Emit(OpCodes.Ldsfld, curves); il.Emit(OpCodes.Callvirt, isActive); il.Emit(OpCodes.Brfalse, decided);
            il.Emit(OpCodes.Ldsfld, bright); il.Emit(OpCodes.Callvirt, isActive); il.Emit(OpCodes.Brfalse, decided);
            il.Emit(OpCodes.Call, passOf); il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Blt, decided);
            il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Stloc, fuse);
            il.Append(decided); il.Emit(OpCodes.Stsfld, bloomSelf);
            il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Stsfld, pending);
            il.Emit(OpCodes.Ldsfld, blooms); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Add); il.Emit(OpCodes.Stsfld, blooms);
            il.Emit(OpCodes.Ldloc, fuse); il.Emit(OpCodes.Brfalse, plain);
            il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Stsfld, pending);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Stsfld, scene);
            il.Emit(OpCodes.Ldarg_2); il.Emit(OpCodes.Stsfld, bloomMat);
            il.Emit(OpCodes.Ret);
            il.Append(plain); il.Emit(OpCodes.Ldarg_1); il.Emit(OpCodes.Ldarg_2); il.Emit(OpCodes.Ldarg_3); il.Emit(OpCodes.Call, blit4); il.Emit(OpCodes.Ret);
        }
        // static void BloomRelease(RenderTexture rt) { if (pending) bloomRt = rt; else RenderTexture.ReleaseTemporary(rt); }
        var bloomRelease = Static("BloomRelease", ts.Void, renderTexture);
        {
            var il = bloomRelease.Body.GetILProcessor();
            var free = il.Create(OpCodes.Ldarg_0);
            il.Emit(OpCodes.Ldsfld, pending); il.Emit(OpCodes.Brfalse, free);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Stsfld, bloomRt); il.Emit(OpCodes.Ret);
            il.Append(free); il.Emit(OpCodes.Call, releaseTemp); il.Emit(OpCodes.Ret);
        }
        // static void CurvesBlit(Texture src, RenderTexture dst, Material m, Behaviour self):
        //   curves = self; curvesAt = blooms; curvesMat = m; if (pending) return; Graphics.Blit(src, dst, m);
        var curvesBlit = Static("CurvesBlit", ts.Void, texture, renderTexture, material, behaviour);
        {
            var il = curvesBlit.Body.GetILProcessor();
            var plain = il.Create(OpCodes.Ldarg_0);
            il.Emit(OpCodes.Ldarg_3); il.Emit(OpCodes.Stsfld, curves);
            il.Emit(OpCodes.Ldsfld, blooms); il.Emit(OpCodes.Stsfld, curvesAt);
            il.Emit(OpCodes.Ldarg_2); il.Emit(OpCodes.Stsfld, curvesMat);
            il.Emit(OpCodes.Ldsfld, pending); il.Emit(OpCodes.Brfalse, plain); il.Emit(OpCodes.Ret);
            il.Append(plain); il.Emit(OpCodes.Ldarg_1); il.Emit(OpCodes.Ldarg_2); il.Emit(OpCodes.Call, blit3); il.Emit(OpCodes.Ret);
        }
        // static void BrightBlit(Texture src, RenderTexture dst, Material m, Behaviour self):
        //   bright = self; brightAt = blooms; brightMat = m;
        //   if (pending) {
        //     pending = false;
        //     if (curvesAt == blooms) {
        //       m.SetTexture("_Bloom", bloomMat.GetTexture("_Bloom")); m.SetTexture("_RgbTex", curvesMat.GetTexture("_RgbTex"));
        //       m.SetFloat("_Saturation", curvesMat.GetFloat("_Saturation"));
        //       Graphics.Blit(scene, dst, m, pass); fusedNow = true; return;
        //     }
        //     RenderTexture t = RenderTexture.GetTemporary(dst.width, dst.height, 0, dst.format);     (the curves did not run: as the game would draw)
        //     Graphics.Blit(scene, t, bloomMat, 0); Graphics.Blit(t, dst, m, 0); RenderTexture.ReleaseTemporary(t); return;
        //   }
        //   Graphics.Blit(src, dst, m, 0);
        var brightBlit = Static("BrightBlit", ts.Void, texture, renderTexture, material, behaviour);
        {
            var tmp = new VariableDefinition(renderTexture);
            brightBlit.Body.Variables.Add(tmp);
            var il = brightBlit.Body.GetILProcessor();
            var plain = il.Create(OpCodes.Ldarg_0);
            var twoStep = il.Create(OpCodes.Ldarg_1);
            il.Emit(OpCodes.Ldarg_3); il.Emit(OpCodes.Stsfld, bright);
            il.Emit(OpCodes.Ldsfld, blooms); il.Emit(OpCodes.Stsfld, brightAt);
            il.Emit(OpCodes.Ldarg_2); il.Emit(OpCodes.Stsfld, brightMat);
            il.Emit(OpCodes.Ldsfld, pending); il.Emit(OpCodes.Brfalse, plain);
            il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Stsfld, pending);
            il.Emit(OpCodes.Ldsfld, curvesAt); il.Emit(OpCodes.Ldsfld, blooms); il.Emit(OpCodes.Bne_Un, twoStep);
            il.Emit(OpCodes.Ldarg_2); il.Emit(OpCodes.Ldstr, BloomTex); il.Emit(OpCodes.Ldsfld, bloomMat); il.Emit(OpCodes.Ldstr, BloomTex);
            il.Emit(OpCodes.Callvirt, getTexture); il.Emit(OpCodes.Callvirt, setTexture);
            il.Emit(OpCodes.Ldarg_2); il.Emit(OpCodes.Ldstr, CurvesTex); il.Emit(OpCodes.Ldsfld, curvesMat); il.Emit(OpCodes.Ldstr, CurvesTex);
            il.Emit(OpCodes.Callvirt, getTexture); il.Emit(OpCodes.Callvirt, setTexture);
            il.Emit(OpCodes.Ldarg_2); il.Emit(OpCodes.Ldstr, Saturation); il.Emit(OpCodes.Ldsfld, curvesMat); il.Emit(OpCodes.Ldstr, Saturation);
            il.Emit(OpCodes.Callvirt, getFloat); il.Emit(OpCodes.Callvirt, setFloat);
            il.Emit(OpCodes.Ldsfld, scene); il.Emit(OpCodes.Ldarg_1); il.Emit(OpCodes.Ldarg_2); il.Emit(OpCodes.Ldsfld, pass); il.Emit(OpCodes.Call, blit4);
            il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Stsfld, fusedNow); il.Emit(OpCodes.Ret);
            il.Append(twoStep); il.Emit(OpCodes.Callvirt, getWidth); il.Emit(OpCodes.Ldarg_1); il.Emit(OpCodes.Callvirt, getHeight);
            il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Ldarg_1); il.Emit(OpCodes.Callvirt, getFormat); il.Emit(OpCodes.Call, getTemp); il.Emit(OpCodes.Stloc, tmp);
            il.Emit(OpCodes.Ldsfld, scene); il.Emit(OpCodes.Ldloc, tmp); il.Emit(OpCodes.Ldsfld, bloomMat); il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Call, blit4);
            il.Emit(OpCodes.Ldloc, tmp); il.Emit(OpCodes.Ldarg_1); il.Emit(OpCodes.Ldarg_2); il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Call, blit4);
            il.Emit(OpCodes.Ldloc, tmp); il.Emit(OpCodes.Call, releaseTemp); il.Emit(OpCodes.Ret);
            il.Append(plain); il.Emit(OpCodes.Ldarg_1); il.Emit(OpCodes.Ldarg_2); il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Call, blit4); il.Emit(OpCodes.Ret);
        }
        // static void DisplayBlit(Texture src, RenderTexture dst, Material m):
        //   if (fusedNow) Graphics.Blit(scene, dst, m, pass); else Graphics.Blit(src, dst, m, 0);
        var displayBlit = Static("DisplayBlit", ts.Void, texture, renderTexture, material);
        {
            var il = displayBlit.Body.GetILProcessor();
            var plain = il.Create(OpCodes.Ldarg_0);
            il.Emit(OpCodes.Ldsfld, fusedNow); il.Emit(OpCodes.Brfalse, plain);
            il.Emit(OpCodes.Ldsfld, scene); il.Emit(OpCodes.Ldarg_1); il.Emit(OpCodes.Ldarg_2); il.Emit(OpCodes.Ldsfld, pass); il.Emit(OpCodes.Call, blit4); il.Emit(OpCodes.Ret);
            il.Append(plain); il.Emit(OpCodes.Ldarg_1); il.Emit(OpCodes.Ldarg_2); il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Call, blit4); il.Emit(OpCodes.Ret);
        }
        foreach (var md in t.Methods) md.Body.OptimizeMacros();
        return (bloomBlit, bloomRelease, curvesBlit, brightBlit, displayBlit);
    }
}
