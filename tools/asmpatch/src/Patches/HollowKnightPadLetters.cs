// hk-pad-letters (kind Prompts, opt-in: the button pictures, never the game play). On Unity's Android
// player Hollow Knight's InputHandler.SetActiveGamepadType knows no platform ("Unsupported platform for
// InputHander Android") and takes every pad for an Xbox 360 pad: the buttons work by position (the
// bottom one is InControl's Action1: jump and confirm) and the prompts show Xbox letters, A at the
// bottom. Handhelds print the Nintendo layout's letters (B at the bottom, A on the right). Naming the
// pad as a Switch pad would not do: InControl's Input System device swaps the face buttons by letter
// for a name with "switch" in it, so the right button jumped. With the patch the game's two pictures
// of the pad, the button glyphs (UIButtonSkins.GetButtonSkinFor) and the controller drawn in the
// controller settings (ControllerDetect.LookForActiveController), take the Switch Pro Controller's
// (Action1 -> B, Action2 -> A, Action3 -> Y, Action4 -> X) while the pad is that Xbox 360 pad. The
// environment variable UA_PAD_LETTERS chooses at run time:
//   xbox      the game as it is (Xbox letters)
//   anything else, or unset: the Nintendo layout's letters
// Read once, at the first picture. The bindings, the menus' confirm and back, the saved controller
// settings (kept per pad type) and everything else that reads the pad type are the game's.
using Mono.Cecil;
using Mono.Cecil.Cil;
using Mono.Cecil.Rocks;

namespace AsmPatch;

sealed class HollowKnightPadLetters : Patch
{
    public override string Name => "hk-pad-letters";
    public override Kind Kind => Kind.Prompts;
    public override bool OptIn => true;
    public override string Summary =>
        "Hollow Knight's button glyphs and controller picture show the Nintendo layout's letters printed on handhelds (B at the bottom), the buttons by position as before; UA_PAD_LETTERS=xbox keeps the Xbox letters (opt-in)";

    public const string Variable = "UA_PAD_LETTERS", HelperType = "<asmpatch>HollowKnightPadLetters";
    // GlobalEnums.GamepadType
    const int Xbox360 = 3, SwitchJoyconDual = 9, SwitchPro = 10;

    public override Result Run(Game game, bool apply)
    {
        var asm = game.Load("Assembly-CSharp");
        var m = asm?.MainModule;
        var skins = m?.GetType("UIButtonSkins");
        var detect = m?.GetType("ControllerDetect");
        if (skins == null || detect == null || m.GetType("InputHandler") == null)
            return Result.NotApplicable("no Hollow Knight (UIButtonSkins, ControllerDetect, InputHandler in Assembly-CSharp)");
        var file = game.FileOf(asm);
        if (m.Types.Any(t => t.Name == HelperType))
            return new Result { Outcome = Outcome.AlreadyApplied, Details = { $"{file}: the pad's pictures follow {Variable}" } };

        // the glyph table: ih.activeGamepadType stored and compared with SWITCH_JOYCON_DUAL first
        var glyphs = skins.Methods.FirstOrDefault(x => x.Name == "GetButtonSkinFor" && x.HasBody && x.Parameters.Count == 1 &&
                                                       x.Parameters[0].ParameterType.FullName == "InControl.InputControlType");
        var atGlyphs = glyphs?.Body.Instructions.Where(i => IsPadType(i) && i.Next is { } s && Cecil.LocalIndex(s, true) is int v &&
                                                            s.Next is { } l && Cecil.LocalIndex(l, false) == v &&
                                                            l.Next is { } k && Cecil.ConstInt(k) == SwitchJoyconDual).ToList();
        if (atGlyphs == null || atGlyphs.Count != 1)
            return Result.Failed($"{file}: UIButtonSkins.GetButtonSkinFor(InputControlType) is not as this patch knows it (one Switch test of the pad type)");
        // the controller picture: ShowController(ih.activeGamepadType)
        var look = detect.Methods.FirstOrDefault(x => x.Name == "LookForActiveController" && x.HasBody);
        var atPicture = look?.Body.Instructions.Where(i => IsPadType(i) && i.Next?.OpCode.Code == Code.Call &&
                                                          i.Next.Operand is MethodReference c && c.Name == "ShowController" &&
                                                          c.DeclaringType.FullName == "ControllerDetect").ToList();
        if (atPicture == null || atPicture.Count != 1)
            return Result.Failed($"{file}: ControllerDetect.LookForActiveController is not as this patch knows it (one ShowController(activeGamepadType))");

        var r = new Result { Outcome = Outcome.Applied };
        r.Details.Add($"{file}: UIButtonSkins.GetButtonSkinFor's glyphs and ControllerDetect's controller picture: the Switch Pro Controller's for the Xbox 360 pad unless {Variable}=xbox");
        if (!apply) return r;

        var shown = Helper(m, atGlyphs[0].Operand as FieldReference);
        foreach (var (md, at) in new[] { (glyphs, atGlyphs[0]), (look, atPicture[0]) })
            md.Body.GetILProcessor().InsertAfter(at, Instruction.Create(OpCodes.Call, shown));
        game.MarkChanged(asm);
        return r;
    }

    static bool IsPadType(Instruction i) =>
        i.OpCode.Code == Code.Ldfld && i.Operand is FieldReference f && f.Name == "activeGamepadType" && f.DeclaringType.FullName == "InputHandler";

    /// <summary>The helper class: static GamepadType Shown(GamepadType t): read once; the Switch Pro
    /// Controller for the Xbox 360 pad unless "xbox", else t.</summary>
    static MethodDefinition Helper(ModuleDefinition m, FieldReference padType)
    {
        var ts = m.TypeSystem;
        var t = new TypeDefinition("", HelperType, TypeAttributes.NotPublic | TypeAttributes.Abstract | TypeAttributes.Sealed |
                                                   TypeAttributes.BeforeFieldInit | TypeAttributes.Class, ts.Object);
        m.Types.Add(t);
        var env = new MethodReference("GetEnvironmentVariable", ts.String, new TypeReference("System", "Environment", m, ts.CoreLibrary));
        env.Parameters.Add(new ParameterDefinition(ts.String));
        var equals = new MethodReference("op_Equality", ts.Boolean, new TypeReference("System", "String", m, ts.CoreLibrary));
        equals.Parameters.Add(new ParameterDefinition(ts.String)); equals.Parameters.Add(new ParameterDefinition(ts.String));
        // static int mode: 0 not read, 1 the game's (xbox), 2 the Nintendo letters
        var mode = new FieldDefinition("mode", FieldAttributes.Private | FieldAttributes.Static, ts.Int32);
        t.Fields.Add(mode);
        var md = new MethodDefinition("Shown", MethodAttributes.Assembly | MethodAttributes.Static | MethodAttributes.HideBySig, padType.FieldType);
        md.Parameters.Add(new ParameterDefinition(padType.FieldType));
        t.Methods.Add(md);
        //   if (mode == 0) mode = Environment.GetEnvironmentVariable(V) == "xbox" ? 1 : 2;
        //   return mode == 2 && t == XBOX_360 ? SWITCH_PRO_CONTROLLER : t;
        var il = md.Body.GetILProcessor();
        var read = il.Create(OpCodes.Ldsfld, mode);
        var game = il.Create(OpCodes.Ldarg_0);
        var two = il.Create(OpCodes.Ldc_I4_2);
        var store = il.Create(OpCodes.Stsfld, mode);
        il.Emit(OpCodes.Ldsfld, mode); il.Emit(OpCodes.Brtrue, read);
        il.Emit(OpCodes.Ldstr, Variable); il.Emit(OpCodes.Call, env); il.Emit(OpCodes.Ldstr, "xbox"); il.Emit(OpCodes.Call, equals);
        il.Emit(OpCodes.Brfalse, two);
        il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Br, store);
        il.Append(two); il.Append(store);
        il.Append(read); il.Emit(OpCodes.Ldc_I4_2); il.Emit(OpCodes.Bne_Un, game);
        il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldc_I4, Xbox360); il.Emit(OpCodes.Bne_Un, game);
        il.Emit(OpCodes.Ldc_I4, SwitchPro); il.Emit(OpCodes.Ret);
        il.Append(game); il.Emit(OpCodes.Ret);
        md.Body.OptimizeMacros();
        return md;
    }
}
