// hk-blur (kind Reduce, opt-in: the picture changes, never the game play). Hollow Knight draws the
// far background with a camera of its own (LightBlurredBackground, on GameCameras) into a 576x360
// texture, blurs it in four passes (LightBlur; two with the game's own Shader Quality "Low") and
// shows it on the scene's blur planes: on the RG34XX-SP about 7.7 ms of a 29 ms GPU frame
// (docs/profiling.md, "Hollow Knight's blur and particles on 1 GB handhelds"). With the patch, the
// environment variable ASMPATCH_HK_BLUR (comma-separated words) chooses at run time:
//   low   two blur passes, as the game's own Shader Quality "Low" has it
//   half  the background camera and its blur at half the resolution (288x180): the same passes
//         over texels twice as large, so blurrier, the same layout ("half,low": both)
//   off   no background camera and no blur: the blur planes are hidden and the scene's camera
//         draws the background itself, sharp (the look of the NoBlur mod, credited in
//         docs/profiling.md)
//   anything else, or unset: the game as it is.
// The variable is read when the game's camera rig starts (LightBlurredBackground and BlurPlane
// Awake), at each aspect change (the texture's size) and each blur (the passes); a port sets it per
// handheld (port.toml runtime_lowmem: on devices with 1 GB of RAM or less). Nothing else changes:
// no timing, physics, collision or FSM.
using Mono.Cecil;
using Mono.Cecil.Cil;
using Mono.Cecil.Rocks;

namespace AsmPatch;

sealed class HollowKnightBlur : Patch
{
    public override string Name => "hk-blur";
    public override Kind Kind => Kind.Reduce;
    public override bool OptIn => true;
    public override string Summary =>
        "Hollow Knight's blurred background with fewer passes, at half resolution or none, chosen at run time by ASMPATCH_HK_BLUR=low|half|half,low|off (the picture changes; opt-in)";

    public const string Variable = "ASMPATCH_HK_BLUR", HelperType = "<asmpatch>HollowKnight";

    public override Result Run(Game game, bool apply)
    {
        var asm = game.Load("Assembly-CSharp");
        var m = asm?.MainModule;
        var lbb = m?.GetType("LightBlurredBackground");
        var plane = m?.GetType("BlurPlane");
        var blur = m?.GetType("LightBlur");
        if (lbb == null || plane == null || blur == null)
            return Result.NotApplicable("no Hollow Knight (LightBlurredBackground, LightBlur, BlurPlane in Assembly-CSharp)");
        var file = game.FileOf(asm);
        if (m.Types.Any(t => t.Name == HelperType))
            return new Result { Outcome = Outcome.AlreadyApplied, Details = { $"{file}: the blur follows {Variable}" } };
        MethodDefinition M(TypeDefinition t, string n, int pars) => t.Methods.FirstOrDefault(x => x.Name == n && !x.IsStatic && x.Parameters.Count == pars && x.HasBody);
        var awake = M(lbb, "Awake", 0);
        var aspect = M(lbb, "OnCameraAspectChanged", 1);
        var planeAwake = M(plane, "Awake", 0);
        var passes = M(blur, "get_BlurPassCount", 0);
        var height = lbb.Fields.FirstOrDefault(f => f.Name == "renderTextureHeight" && f.FieldType.MetadataType == MetadataType.Int32);
        var cancel = lbb.Fields.FirstOrDefault(f => f.Name == "cancelEnable" && f.FieldType.MetadataType == MetadataType.Boolean);
        var groups = blur.Fields.FirstOrDefault(f => f.Name == "passGroupCount" && f.FieldType.MetadataType == MetadataType.Int32);
        var renderer = plane.Fields.FirstOrDefault(f => f.Name == "meshRenderer");
        // MeshRenderer.enabled's setter, as BlurPlane.SetPlaneVisibility calls it
        var setEnabled = M(plane, "SetPlaneVisibility", 1)?.Body.Instructions
            .Select(i => i.Operand).OfType<MethodReference>().FirstOrDefault(r => r.Name == "set_enabled");
        if (awake == null || aspect == null || planeAwake == null || passes == null || height == null || cancel == null || groups == null ||
            renderer == null || setEnabled == null)
            return Result.Failed($"{file}: LightBlurredBackground / LightBlur / BlurPlane are not as this patch knows them");
        var reads = aspect.Body.Instructions.Where(i => i.OpCode.Code == Code.Ldfld && Cecil.SameField(i.Operand, height)).ToList();
        var groupReads = passes.Body.Instructions.Where(i => i.OpCode.Code == Code.Ldfld && Cecil.SameField(i.Operand, groups)).ToList();
        if (reads.Count == 0 || groupReads.Count == 0)
            return Result.Failed($"{file}: OnCameraAspectChanged / BlurPassCount do not read renderTextureHeight / passGroupCount");

        var r = new Result { Outcome = Outcome.Applied };
        r.Details.Add($"{file}: LightBlurredBackground (its texture height at {reads.Count} read(s), its start), LightBlur.BlurPassCount and " +
                      $"BlurPlane.Awake follow {Variable} (low, half, off)");
        if (!apply) return r;

        var h = Helper(m);
        void Wrap(MethodDefinition md, List<Instruction> at, MethodDefinition f)
        {
            md.Body.SimplifyMacros();
            var il = md.Body.GetILProcessor();
            foreach (var i in at) il.InsertAfter(i, il.Create(OpCodes.Call, f));
            md.Body.OptimizeMacros();
        }
        Wrap(aspect, reads, h.height);           // the texture's height: h / 2 with "half"
        Wrap(passes, groupReads, h.groups);      // the pass groups: at most 1 with "low"
        // LightBlurredBackground.Awake: ... if (off) cancelEnable = true;   (OnEnable, LateUpdate, OnDisable then do nothing)
        BeforeReturns(awake, (p, end) => new[]
        {
            p.Create(OpCodes.Call, h.off), p.Create(OpCodes.Brfalse, end),
            p.Create(OpCodes.Ldarg_0), p.Create(OpCodes.Ldc_I4_1), p.Create(OpCodes.Stfld, cancel),
        });
        // BlurPlane.Awake: ... if (off) meshRenderer.enabled = false;
        BeforeReturns(planeAwake, (p, end) => new[]
        {
            p.Create(OpCodes.Call, h.off), p.Create(OpCodes.Brfalse, end),
            p.Create(OpCodes.Ldarg_0), p.Create(OpCodes.Ldfld, renderer), p.Create(OpCodes.Ldc_I4_0), p.Create(OpCodes.Callvirt, setEnabled),
        });
        game.MarkChanged(asm);
        return r;
    }

    /// <summary>Puts code before each ret of a void method (each ret becomes the code's first
    /// instruction; the code may branch to end, the new ret).</summary>
    static void BeforeReturns(MethodDefinition md, Func<ILProcessor, Instruction, Instruction[]> code)
    {
        var body = md.Body;
        body.SimplifyMacros();
        var il = body.GetILProcessor();
        foreach (var ret in body.Instructions.Where(i => i.OpCode.Code == Code.Ret).ToList())
        {
            var end = il.Create(OpCodes.Ret);
            var seq = code(il, end);
            ret.OpCode = seq[0].OpCode; ret.Operand = seq[0].Operand;
            var prev = ret;
            foreach (var x in seq.Skip(1).Append(end)) { il.InsertAfter(prev, x); prev = x; }
        }
        body.OptimizeMacros();
    }

    /// <summary>The helper class: bool Has(word) (word is one of the variable's comma-separated
    /// words), int Height(h) (h / 2 with "half"), int Groups(g) (at most 1 with "low"), bool Off().</summary>
    static (MethodDefinition height, MethodDefinition groups, MethodDefinition off) Helper(ModuleDefinition m)
    {
        var ts = m.TypeSystem;
        var t = new TypeDefinition("", HelperType, TypeAttributes.NotPublic | TypeAttributes.Abstract | TypeAttributes.Sealed |
                                                   TypeAttributes.BeforeFieldInit | TypeAttributes.Class, ts.Object);
        m.Types.Add(t);
        var env = new MethodReference("GetEnvironmentVariable", ts.String, new TypeReference("System", "Environment", m, ts.CoreLibrary));
        env.Parameters.Add(new ParameterDefinition(ts.String));
        var concat = new MethodReference("Concat", ts.String, ts.String);
        for (int k = 0; k < 3; k++) concat.Parameters.Add(new ParameterDefinition(ts.String));
        var contains = new MethodReference("Contains", ts.Boolean, ts.String) { HasThis = true };
        contains.Parameters.Add(new ParameterDefinition(ts.String));
        MethodDefinition Static(string name, TypeReference ret, params TypeReference[] ps)
        {
            var md = new MethodDefinition(name, MethodAttributes.Assembly | MethodAttributes.Static | MethodAttributes.HideBySig, ret);
            foreach (var p in ps) md.Parameters.Add(new ParameterDefinition(p));
            t.Methods.Add(md);
            return md;
        }
        // static int Flags(): read once (the blur asks every frame): 1 | off 2 | half 4 | low 8
        //   if (flags == 0) { int f = 1; string v = Environment.GetEnvironmentVariable(V);
        //     if (v != null) { v = "," + v + ","; if (v.Contains(",off,")) f |= 2; ... } flags = f; } return flags;
        var flags = new FieldDefinition("flags", FieldAttributes.Private | FieldAttributes.Static, ts.Int32);
        t.Fields.Add(flags);
        var read = Static("Flags", ts.Int32);
        read.Body.InitLocals = true;
        var f = new VariableDefinition(ts.Int32); var v = new VariableDefinition(ts.String);
        read.Body.Variables.Add(f); read.Body.Variables.Add(v);
        var il = read.Body.GetILProcessor();
        var done = il.Create(OpCodes.Ldsfld, flags);
        var store = il.Create(OpCodes.Ldloc, f);
        il.Emit(OpCodes.Ldsfld, flags); il.Emit(OpCodes.Brtrue, done);
        il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Stloc, f);
        il.Emit(OpCodes.Ldstr, Variable); il.Emit(OpCodes.Call, env); il.Emit(OpCodes.Stloc, v);
        il.Emit(OpCodes.Ldloc, v); il.Emit(OpCodes.Brfalse, store);
        il.Emit(OpCodes.Ldstr, ","); il.Emit(OpCodes.Ldloc, v); il.Emit(OpCodes.Ldstr, ","); il.Emit(OpCodes.Call, concat); il.Emit(OpCodes.Stloc, v);
        foreach (var (word, bit) in new[] { ("off", 2), ("half", 4), ("low", 8) })
        {
            var next = il.Create(OpCodes.Nop);
            il.Emit(OpCodes.Ldloc, v); il.Emit(OpCodes.Ldstr, "," + word + ","); il.Emit(OpCodes.Callvirt, contains); il.Emit(OpCodes.Brfalse, next);
            il.Emit(OpCodes.Ldloc, f); il.Emit(OpCodes.Ldc_I4, bit); il.Emit(OpCodes.Or); il.Emit(OpCodes.Stloc, f);
            il.Append(next);
        }
        il.Append(store); il.Emit(OpCodes.Stsfld, flags);
        il.Append(done); il.Emit(OpCodes.Ret);
        MethodDefinition Bit(string name, int bit)        // static bool <name>() => (Flags() & bit) != 0;
        {
            var md = Static(name, ts.Boolean);
            var p = md.Body.GetILProcessor();
            p.Emit(OpCodes.Call, read); p.Emit(OpCodes.Ldc_I4, bit); p.Emit(OpCodes.And); p.Emit(OpCodes.Ldc_I4_0); p.Emit(OpCodes.Cgt_Un); p.Emit(OpCodes.Ret);
            return md;
        }
        var off = Bit("Off", 2);
        var half = Bit("Half", 4);
        var low = Bit("Low", 8);
        // static int Height(int h) => Half() ? h / 2 : h;
        var height = Static("Height", ts.Int32, ts.Int32);
        il = height.Body.GetILProcessor();
        var same = il.Create(OpCodes.Ldarg_0);
        il.Emit(OpCodes.Call, half); il.Emit(OpCodes.Brfalse_S, same);
        il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldc_I4_2); il.Emit(OpCodes.Div); il.Emit(OpCodes.Ret);
        il.Append(same); il.Emit(OpCodes.Ret);
        // static int Groups(int g) => Low() && g > 1 ? 1 : g;
        var groups = Static("Groups", ts.Int32, ts.Int32);
        il = groups.Body.GetILProcessor();
        var keep = il.Create(OpCodes.Ldarg_0);
        il.Emit(OpCodes.Call, low); il.Emit(OpCodes.Brfalse_S, keep);
        il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Ble_S, keep);
        il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Ret);
        il.Append(keep); il.Emit(OpCodes.Ret);
        foreach (var md in t.Methods) md.Body.OptimizeMacros();
        return (height, groups, off);
    }
}
