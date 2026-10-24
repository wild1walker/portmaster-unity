// hk-blur (kind Reduce, opt-in: the picture changes, never the game play). Hollow Knight draws the
// far background with a camera of its own (LightBlurredBackground, on GameCameras) into a 576x360
// texture, blurs it in four passes (two with the game's Shader Quality "Low") and shows it on the
// scene's blur planes; on the RG34XX-SP that is about 7.7 ms of a 29 ms GPU frame
// (docs/profiling.md, "Hollow Knight's own reductions"). With the patch, the environment variable
// ASMPATCH_HK_BLUR chooses at run time:
//   half  the background camera and its blur at half the resolution (288x180): the same passes
//         over texels twice as large, so a little blurrier, the same layout
//   off   no background camera and no blur: the blur planes are hidden and the scene's camera
//         draws the background itself, sharp (what the NoBlur mod shows; idea credited in
//         docs/profiling.md)
//   anything else, or unset: the game as it is.
// The variable is read when the game's camera rig starts (LightBlurredBackground and BlurPlane
// Awake, the texture's size at each aspect change); a port sets it per handheld (port.toml
// runtime_lowmem: on devices with 1 GB of RAM or less). Nothing else changes: no timing, physics,
// collision or FSM.
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
        "Hollow Knight's blurred background at half resolution or none, chosen at run time by ASMPATCH_HK_BLUR=half|off (the picture changes; opt-in)";

    public const string Variable = "ASMPATCH_HK_BLUR", HelperType = "<asmpatch>HollowKnight";

    public override Result Run(Game game, bool apply)
    {
        var asm = game.Load("Assembly-CSharp");
        var m = asm?.MainModule;
        var lbb = m?.GetType("LightBlurredBackground");
        var plane = m?.GetType("BlurPlane");
        if (lbb == null || plane == null) return Result.NotApplicable("no Hollow Knight (LightBlurredBackground, BlurPlane in Assembly-CSharp)");
        var file = game.FileOf(asm);
        if (m.Types.Any(t => t.Name == HelperType))
            return new Result { Outcome = Outcome.AlreadyApplied, Details = { $"{file}: the blur follows {Variable}" } };
        MethodDefinition M(TypeDefinition t, string n, int pars) => t.Methods.FirstOrDefault(x => x.Name == n && !x.IsStatic && x.Parameters.Count == pars && x.HasBody);
        var awake = M(lbb, "Awake", 0);
        var aspect = M(lbb, "OnCameraAspectChanged", 1);
        var planeAwake = M(plane, "Awake", 0);
        var height = lbb.Fields.FirstOrDefault(f => f.Name == "renderTextureHeight" && f.FieldType.MetadataType == MetadataType.Int32);
        var cancel = lbb.Fields.FirstOrDefault(f => f.Name == "cancelEnable" && f.FieldType.MetadataType == MetadataType.Boolean);
        var renderer = plane.Fields.FirstOrDefault(f => f.Name == "meshRenderer");
        // MeshRenderer.enabled's setter, as BlurPlane.SetPlaneVisibility calls it
        var setEnabled = M(plane, "SetPlaneVisibility", 1)?.Body.Instructions
            .Select(i => i.Operand).OfType<MethodReference>().FirstOrDefault(r => r.Name == "set_enabled");
        if (awake == null || aspect == null || planeAwake == null || height == null || cancel == null || renderer == null || setEnabled == null)
            return Result.Failed($"{file}: LightBlurredBackground / BlurPlane are not as this patch knows them");
        var reads = aspect.Body.Instructions.Where(i => i.OpCode.Code == Code.Ldfld && Cecil.SameField(i.Operand, height)).ToList();
        if (reads.Count == 0) return Result.Failed($"{file}: LightBlurredBackground.OnCameraAspectChanged does not read renderTextureHeight");

        var r = new Result { Outcome = Outcome.Applied };
        r.Details.Add($"{file}: LightBlurredBackground (its texture height at {reads.Count} read(s), its start) and BlurPlane.Awake follow {Variable} (half, off)");
        if (!apply) return r;

        var (heightOf, off) = Helper(m);
        aspect.Body.SimplifyMacros();
        var il = aspect.Body.GetILProcessor();
        foreach (var i in reads) il.InsertAfter(i, il.Create(OpCodes.Call, heightOf));
        aspect.Body.OptimizeMacros();
        // LightBlurredBackground.Awake: ... if (off) cancelEnable = true;   (OnEnable, LateUpdate, OnDisable then do nothing)
        BeforeReturns(awake, (p, end) => new[]
        {
            p.Create(OpCodes.Call, off), p.Create(OpCodes.Brfalse, end),
            p.Create(OpCodes.Ldarg_0), p.Create(OpCodes.Ldc_I4_1), p.Create(OpCodes.Stfld, cancel),
        });
        // BlurPlane.Awake: ... if (off) meshRenderer.enabled = false;
        BeforeReturns(planeAwake, (p, end) => new[]
        {
            p.Create(OpCodes.Call, off), p.Create(OpCodes.Brfalse, end),
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

    /// <summary>The helper class: int Height(int h) (h / 2 with "half"), bool Off() ("off").</summary>
    static (MethodDefinition height, MethodDefinition off) Helper(ModuleDefinition m)
    {
        var ts = m.TypeSystem;
        var t = new TypeDefinition("", HelperType, TypeAttributes.NotPublic | TypeAttributes.Abstract | TypeAttributes.Sealed |
                                                   TypeAttributes.BeforeFieldInit | TypeAttributes.Class, ts.Object);
        m.Types.Add(t);
        var env = new MethodReference("GetEnvironmentVariable", ts.String, new TypeReference("System", "Environment", m, ts.CoreLibrary));
        env.Parameters.Add(new ParameterDefinition(ts.String));
        var eq = new MethodReference("op_Equality", ts.Boolean, ts.String);
        eq.Parameters.Add(new ParameterDefinition(ts.String));
        eq.Parameters.Add(new ParameterDefinition(ts.String));
        MethodDefinition Is(string name, string value)
        {
            var md = new MethodDefinition(name, MethodAttributes.Assembly | MethodAttributes.Static | MethodAttributes.HideBySig, ts.Boolean);
            var il = md.Body.GetILProcessor();
            il.Emit(OpCodes.Ldstr, Variable); il.Emit(OpCodes.Call, env); il.Emit(OpCodes.Ldstr, value); il.Emit(OpCodes.Call, eq); il.Emit(OpCodes.Ret);
            t.Methods.Add(md);
            return md;
        }
        var half = Is("Half", "half");
        var off = Is("Off", "off");
        var h = new MethodDefinition("Height", MethodAttributes.Assembly | MethodAttributes.Static | MethodAttributes.HideBySig, ts.Int32);
        h.Parameters.Add(new ParameterDefinition("h", ParameterAttributes.None, ts.Int32));
        var hil = h.Body.GetILProcessor();
        var full = hil.Create(OpCodes.Ldarg_0);
        hil.Emit(OpCodes.Call, half); hil.Emit(OpCodes.Brfalse_S, full);
        hil.Emit(OpCodes.Ldarg_0); hil.Emit(OpCodes.Ldc_I4_2); hil.Emit(OpCodes.Div); hil.Emit(OpCodes.Ret);
        hil.Append(full); hil.Emit(OpCodes.Ret);
        t.Methods.Add(h);
        return (h, off);
    }
}
