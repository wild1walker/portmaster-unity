// blasphemous-debuginfo-idle (kind Optimize, opt-in). Blasphemous's DebugInformation widget writes the Penitent's position
// into its coordinates label every frame (Vector2.ToString: a new string a frame, set on a UI Text). In a release
// player its Awake reads Debug.isDebugBuild (false) into isDebugBuild and DisableWhenRetailMode deactivates that label
// (SetActive(isDebugBuild)): the text is never seen. Patched: Update writes the label only when isDebugBuild, the
// condition under which the label is shown; the rest of Update (the cursor) as before. Checked at patch time: the
// label field is used only by DebugInformation, isDebugBuild is written only in Awake (from Debug.isDebugBuild), and
// DisableWhenRetailMode deactivates the label with it.
using Mono.Cecil;
using Mono.Cecil.Cil;
using Mono.Cecil.Rocks;

namespace AsmPatch;

sealed class BlasphemousDebugInfoIdle : Patch
{
    public override string Name => "blasphemous-debuginfo-idle";
    public override Kind Kind => Kind.Optimize;
    public override bool OptIn => true;
    public override string Summary =>
        "Blasphemous: the debug widget's hidden coordinates label (release players deactivate it) no longer gets the Penitent's position as a new string every frame";

    public override Result Run(Game game, bool apply)
    {
        var asm = game.Load("Assembly-CSharp");
        var m = asm?.MainModule;
        var t = m?.GetType("Gameplay.UI.Widgets.DebugInformation");
        var upd = t?.Methods.FirstOrDefault(x => x.Name == "Update" && x.Parameters.Count == 0 && x.HasBody);
        var label = t?.Fields.FirstOrDefault(f => f.Name == "currentCoordinatesLabel");
        var dbg = t?.Fields.FirstOrDefault(f => f.Name == "isDebugBuild" && f.FieldType.FullName == "System.Boolean");
        var retail = t?.Methods.FirstOrDefault(x => x.Name == "DisableWhenRetailMode" && x.HasBody);
        if (upd == null || label == null || dbg == null || retail == null) return Result.NotApplicable("no Blasphemous DebugInformation (Update, currentCoordinatesLabel, isDebugBuild, DisableWhenRetailMode)");
        var file = game.FileOf(asm);
        var ins = upd.Body.Instructions;
        // the label's null check: ldarg.0; ldfld label; ldnull; call op_Inequality; brfalse <end>
        var at = ins.FirstOrDefault(i => i.OpCode.Code == Code.Ldfld && i.Operand is FieldReference fr && Cecil.SameField(fr, label) &&
                                         i.Next?.OpCode.Code == Code.Ldnull && i.Next.Next?.Operand is MethodReference mr && mr.Name == "op_Inequality" &&
                                         i.Next.Next.Next?.OpCode.Code is Code.Brfalse or Code.Brfalse_S);
        if (at == null) return Result.NotApplicable($"{file}: DebugInformation.Update is not as this patch knows it");
        var br = at.Next.Next.Next;
        if (br.Next?.OpCode.Code == Code.Ldarg_0 && br.Next.Next?.Operand is FieldReference f0 && Cecil.SameField(f0, dbg))
            return new Result { Outcome = Outcome.AlreadyApplied, Details = { $"{file}: DebugInformation.Update writes the label only in a debug build" } };
        foreach (var a in game.GameAssemblies)
            foreach (var ty in Cecil.AllTypes(a.MainModule))
                foreach (var md in ty.Methods.Where(x => x.HasBody))
                    foreach (var i in md.Body.Instructions)
                    {
                        if (i.Operand is FieldReference fr && Cecil.SameField(fr, label) && ty != t) return Result.NotApplicable($"{file}: {ty.Name}.{md.Name} uses the label");
                        if (i.Operand is FieldReference fd && Cecil.SameField(fd, dbg) && i.OpCode.Code is Code.Stfld && md.Name != "Awake") return Result.NotApplicable($"{file}: {ty.Name}.{md.Name} writes isDebugBuild");
                    }
        if (!retail.Body.Instructions.Any(i => i.Operand is FieldReference fr && Cecil.SameField(fr, label)) ||
            !retail.Body.Instructions.Any(i => i.Operand is MethodReference mr && mr.Name == "SetActive"))
            return Result.NotApplicable($"{file}: DisableWhenRetailMode does not deactivate the label");
        var r = new Result { Outcome = Outcome.Applied };
        r.Details.Add($"{file}: Gameplay.UI.Widgets.DebugInformation.Update: the coordinates label written only when isDebugBuild (it is shown only then)");
        if (!apply) return r;
        var b = upd.Body; b.SimplifyMacros();
        var il = b.GetILProcessor();
        var target = (Instruction)br.Operand;
        var after = br.Next;
        il.InsertBefore(after, il.Create(OpCodes.Ldarg_0));
        il.InsertBefore(after, il.Create(OpCodes.Ldfld, dbg));
        il.InsertBefore(after, il.Create(OpCodes.Brfalse, target));
        b.OptimizeMacros();
        game.MarkChanged(asm);
        return r;
    }
}
