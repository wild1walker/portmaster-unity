// hk-aspect (kind Screen, opt-in: the picture's shape, never the game play). Hollow Knight fits its
// view to the screen in ForceCameraAspect.AutoScaleViewportShared: the screen's aspect clamped to 1.6 -
// 2.39 (16:10 to 21:9); a screen narrower than 16:10 is letterboxed to 16:10, and from 16:9 down to
// 16:10 the camera shows more above and below (the field of view and the HUD camera grown by
// 16:9 / aspect, the same width). Handhelds are 3:2 (720x480) and 4:3 (640x480): bars above and below.
// With the patch, the environment variable ASMPATCH_HK_ASPECT chooses at run time:
//   fill  the clamp starts at 1:1 instead of 16:10: on 4:3 and 3:2 the game's own rule for 16:10 goes
//         on (the same width as 16:9, more above and below, the HUD at the top corner), no bars
//   anything else, or unset: the game as it is.
// The variable is read once, at the first viewport fit. Nothing else changes: the camera's position
// and its limits, timing, physics, collision and FSMs are the game's.
using Mono.Cecil;
using Mono.Cecil.Cil;
using Mono.Cecil.Rocks;

namespace AsmPatch;

sealed class HollowKnightAspect : Patch
{
    public override string Name => "hk-aspect";
    public override Kind Kind => Kind.Screen;
    public override bool OptIn => true;
    public override string Summary =>
        "Hollow Knight's view fitted to screens narrower than 16:10 (4:3, 3:2) by its own rule for 16:10 instead of bars, chosen at run time by ASMPATCH_HK_ASPECT=fill (opt-in)";

    public const string Variable = "ASMPATCH_HK_ASPECT", HelperType = "<asmpatch>HollowKnightAspect";
    const float Tall = 1.6f, Fill = 1f;

    public override Result Run(Game game, bool apply)
    {
        var asm = game.Load("Assembly-CSharp");
        var m = asm?.MainModule;
        var fca = m?.GetType("ForceCameraAspect");
        if (fca == null)
            return Result.NotApplicable("no Hollow Knight (ForceCameraAspect in Assembly-CSharp)");
        var file = game.FileOf(asm);
        if (m.Types.Any(t => t.Name == HelperType))
            return new Result { Outcome = Outcome.AlreadyApplied, Details = { $"{file}: the view's narrowest aspect follows {Variable}" } };
        var fit = fca.Methods.FirstOrDefault(x => x.Name == "AutoScaleViewportShared" && x.IsStatic && x.HasBody);
        // the clamp's lower end: ldc.r4 1.6 just before ldc.r4 2.3916667 (new MinMaxFloat(1.6f, 2.3916667f))
        var at = fit?.Body.Instructions.Where(i => i.OpCode.Code == Code.Ldc_R4 && (float)i.Operand == Tall &&
                                                   i.Next?.OpCode.Code == Code.Ldc_R4 && (float)i.Next.Operand > 2f).ToList();
        if (at == null || at.Count != 1)
            return Result.Failed($"{file}: ForceCameraAspect.AutoScaleViewportShared is not as this patch knows it (one clamp from 1.6)");

        var r = new Result { Outcome = Outcome.Applied };
        r.Details.Add($"{file}: ForceCameraAspect.AutoScaleViewportShared's narrowest aspect (1.6) follows {Variable} (fill: 1)");
        if (!apply) return r;

        var narrowest = Helper(m);
        var il = fit.Body.GetILProcessor();
        il.InsertAfter(at[0], il.Create(OpCodes.Call, narrowest));
        game.MarkChanged(asm);
        return r;
    }

    /// <summary>The helper class: static float Narrowest(float tall): read once, 1 with "fill", else tall.</summary>
    static MethodDefinition Helper(ModuleDefinition m)
    {
        var ts = m.TypeSystem;
        var t = new TypeDefinition("", HelperType, TypeAttributes.NotPublic | TypeAttributes.Abstract | TypeAttributes.Sealed |
                                                   TypeAttributes.BeforeFieldInit | TypeAttributes.Class, ts.Object);
        m.Types.Add(t);
        var env = new MethodReference("GetEnvironmentVariable", ts.String, new TypeReference("System", "Environment", m, ts.CoreLibrary));
        env.Parameters.Add(new ParameterDefinition(ts.String));
        var equals = new MethodReference("op_Equality", ts.Boolean, new TypeReference("System", "String", m, ts.CoreLibrary));
        equals.Parameters.Add(new ParameterDefinition(ts.String)); equals.Parameters.Add(new ParameterDefinition(ts.String));
        // static int mode: 0 not read, 1 the game's, 2 fill
        var mode = new FieldDefinition("mode", FieldAttributes.Private | FieldAttributes.Static, ts.Int32);
        t.Fields.Add(mode);
        var md = new MethodDefinition("Narrowest", MethodAttributes.Assembly | MethodAttributes.Static | MethodAttributes.HideBySig, ts.Single);
        md.Parameters.Add(new ParameterDefinition(ts.Single));
        t.Methods.Add(md);
        //   if (mode == 0) mode = Environment.GetEnvironmentVariable(V) == "fill" ? 2 : 1;
        //   return mode == 2 ? 1f : tall;
        var il = md.Body.GetILProcessor();
        var read = il.Create(OpCodes.Ldsfld, mode);
        var game = il.Create(OpCodes.Ldarg_0);
        var one = il.Create(OpCodes.Ldc_I4_1);
        var store = il.Create(OpCodes.Stsfld, mode);
        il.Emit(OpCodes.Ldsfld, mode); il.Emit(OpCodes.Brtrue, read);
        il.Emit(OpCodes.Ldstr, Variable); il.Emit(OpCodes.Call, env); il.Emit(OpCodes.Ldstr, "fill"); il.Emit(OpCodes.Call, equals);
        il.Emit(OpCodes.Brfalse, one);
        il.Emit(OpCodes.Ldc_I4_2); il.Emit(OpCodes.Br, store);
        il.Append(one); il.Append(store);
        il.Append(read); il.Emit(OpCodes.Ldc_I4_2); il.Emit(OpCodes.Bne_Un, game);
        il.Emit(OpCodes.Ldc_R4, Fill); il.Emit(OpCodes.Ret);
        il.Append(game); il.Emit(OpCodes.Ret);
        md.Body.OptimizeMacros();
        return md;
    }
}
