// playmaker-delayedquiet (kind Optimize, opt-in). Fsm.UpdateDelayedEvents runs in every Update of every
// FSM; with playmaker-delayedevents it returns at once when the FSM's delayed events and its two work
// lists are empty, but that test still reads three List objects of the FSM, and on a handheld each is a
// cache miss in nearly every call (Hollow Knight, King's Pass, RG34XX-SP 64-bit: 1.6 % of UnityMain,
// about 1.1 us a call, for a method that returns). This patch keeps the answer in the Fsm itself:
//   <asmpatch>delayedQuiet   true only while the delayed events are there and empty and both work lists
//                            are empty (the state in which the method changes nothing)
//   <asmpatch>delayedOut     the FSM's delayed-event list was handed out (a method returned it): the
//                            patch cannot see what is done with it later, so the FSM is never quiet again
// UpdateDelayedEvents starts with "if (quiet) return;" and, where the three lists are found empty, sets
// quiet = !out before it returns. Every place that could make the lists non-empty clears quiet first:
// each load of the delayed-event field outside UpdateDelayedEvents (an Add, a Clear, a read: all of them,
// whatever the receiver) and each store into it. The work lists are touched only by UpdateDelayedEvents
// and by constructors that set them to new lists (checked, as for playmaker-delayedevents). A load of the
// delayed-event field must be followed, with nothing but argument loads between, by a call of one of
// List<DelayedEvent>'s own methods, a null test, or the return of the method (then out = true); any other
// use, or its address taken, leaves the game unpatched. So while quiet is true no code has run that could
// have added a delayed event since the lists were last found empty: the skipped call would have changed
// nothing, and every list, count and event is as without the patch. New Fsm objects start not quiet
// (Unity's serializer and every constructor alike), so their first update looks at the lists.
// Not covered (as for the other playmaker-* patches): writes into the lists by reflection or native code.
using Mono.Cecil;
using Mono.Cecil.Cil;
using Mono.Cecil.Rocks;

namespace AsmPatch;

sealed class PlayMakerDelayedQuiet : Patch
{
    public override string Name => "playmaker-delayedquiet";
    public override Kind Kind => Kind.Optimize;
    public override bool OptIn => true;
    public override string Summary =>
        "PlayMaker's Fsm.UpdateDelayedEvents returns on one flag of the FSM's own while it has no delayed events (cleared wherever one could be added)";

    public const string QuietName = "<asmpatch>delayedQuiet", OutName = "<asmpatch>delayedOut";
    static readonly HashSet<string> ListMethods = new() { "Add", "AddRange", "Insert", "InsertRange", "Clear", "Remove", "RemoveAt", "RemoveRange",
        "get_Count", "get_Item", "Contains", "IndexOf", "GetEnumerator", "ToArray" };

    static bool IsListOfDelayed(MethodReference r) => r.DeclaringType is GenericInstanceType g &&
        g.ElementType.FullName == "System.Collections.Generic.List`1" && g.GenericArguments[0].FullName == "HutongGames.PlayMaker.DelayedEvent";

    static bool SimpleLoad(Instruction i) => i.OpCode.Code is Code.Ldarg or Code.Ldarg_S or Code.Ldarg_0 or Code.Ldarg_1 or Code.Ldarg_2 or Code.Ldarg_3 or
        Code.Ldloc or Code.Ldloc_S or Code.Ldloc_0 or Code.Ldloc_1 or Code.Ldloc_2 or Code.Ldloc_3 or Code.Ldnull or Code.Ldstr or
        Code.Ldc_I4 or Code.Ldc_I4_S or Code.Ldc_I4_0 or Code.Ldc_I4_1 or Code.Ldc_I4_2 or Code.Ldc_I4_3 or Code.Ldc_I4_4 or
        Code.Ldc_I4_5 or Code.Ldc_I4_6 or Code.Ldc_I4_7 or Code.Ldc_I4_8 or Code.Ldc_I4_M1 or Code.Ldc_I8 or Code.Ldc_R4 or Code.Ldc_R8;

    public override Result Run(Game game, bool apply)
    {
        var (asm, fsm) = PlayMakerLib.Find(game);
        if (asm == null) return Result.NotApplicable("no PlayMaker (HutongGames.PlayMaker.Fsm)");
        var module = asm.MainModule;
        var file = game.FileOf(asm);
        var m = fsm.Methods.FirstOrDefault(x => x.Name == "UpdateDelayedEvents" && !x.IsStatic && !x.HasParameters && x.HasBody &&
                                                x.ReturnType.MetadataType == MetadataType.Void);
        if (m == null) return Result.NotApplicable($"{file}: no Fsm.UpdateDelayedEvents");
        var where = $"{file}: {fsm.FullName}.UpdateDelayedEvents";
        if (fsm.Fields.Any(f => f.Name == QuietName))
            return new Result { Outcome = Outcome.AlreadyApplied, Details = { $"{where}: returns on the FSM's quiet flag" } };

        // the method as PlayMaker has it, behind playmaker-delayedevents' test or not
        var ins = m.Body.Instructions;
        int start = PrefixLength(m);
        var shape = PlayMakerDelayedEvents.Match(m, start);
        if (shape == null || PlayMakerLib.Targeted(m.Body, ins[0])) return Result.NotApplicable($"{where}: not the method this patch knows");
        var (remove, update, delayed, count) = shape.Value;
        if (!delayed.IsPrivate || delayed.IsStatic) return Result.NotApplicable($"{file}: Fsm.{delayed.Name} is not a private field");
        var why = PlayMakerDelayedEvents.WorkListsPrivate(game, m, file, remove, update);
        if (why != null) return Result.NotApplicable(why);

        // every use of the delayed-event field outside the method: what follows the load
        var loads = new List<(MethodDefinition at, Instruction ins, bool handsOut)>();
        var stores = new List<(MethodDefinition at, Instruction ins)>();
        foreach (var a in game.GameAssemblies)
            foreach (var t in Cecil.AllTypes(a.MainModule))
                foreach (var x in t.Methods.Where(y => y.HasBody))
                {
                    if (x == m) continue;
                    var xi = x.Body.Instructions;
                    for (int k = 0; k < xi.Count; k++)
                    {
                        if (!Cecil.SameField(xi[k].Operand, delayed)) continue;
                        var c = xi[k].OpCode.Code;
                        if (PlayMakerLib.Targeted(x.Body, xi[k]))
                            return Result.NotApplicable($"{file}: {t.FullName}.{x.Name}: a branch to a use of Fsm.{delayed.Name}");
                        if (c == Code.Stfld) { stores.Add((x, xi[k])); continue; }
                        if (c != Code.Ldfld) return Result.NotApplicable($"{file}: {t.FullName}.{x.Name} takes the address of Fsm.{delayed.Name}");
                        int j = k + 1;
                        while (j < xi.Count && SimpleLoad(xi[j]) && !PlayMakerLib.Targeted(x.Body, xi[j])) j++;
                        if (j >= xi.Count || PlayMakerLib.Targeted(x.Body, xi[j]))
                            return Result.NotApplicable($"{file}: {t.FullName}.{x.Name}: a use of Fsm.{delayed.Name} the patch cannot follow");
                        var u = xi[j];
                        bool ok = u.OpCode.Code is Code.Call or Code.Callvirt && u.Operand is MethodReference r && IsListOfDelayed(r) && ListMethods.Contains(r.Name) && j == k + 1 + r.Parameters.Count
                                  || u.OpCode.Code is Code.Brtrue or Code.Brtrue_S or Code.Brfalse or Code.Brfalse_S && j == k + 1;
                        bool ret = u.OpCode.Code == Code.Ret && j == k + 1;
                        if (!ok && !ret) return Result.NotApplicable($"{file}: {t.FullName}.{x.Name} passes Fsm.{delayed.Name} on ({u.OpCode})");
                        loads.Add((x, xi[k], ret));
                    }
                }
        var r0 = new Result { Outcome = Outcome.Applied };
        r0.Details.Add($"{where}: returns on the FSM's quiet flag (cleared at {loads.Count} uses of {delayed.Name} in " +
                       $"{string.Join(", ", loads.Select(l => l.at.DeclaringType.Name + "." + l.at.Name).Distinct())}; " +
                       $"{loads.Count(l => l.handsOut)} hand it out; {stores.Count} stores)");
        if (!apply) return r0;

        var quiet = new FieldDefinition(QuietName, FieldAttributes.Private | FieldAttributes.NotSerialized, module.TypeSystem.Boolean);
        var outf = new FieldDefinition(OutName, FieldAttributes.Private | FieldAttributes.NotSerialized, module.TypeSystem.Boolean);
        fsm.Fields.Add(quiet); fsm.Fields.Add(outf);
        FieldReference Q(ModuleDefinition mod) => mod == module ? quiet : mod.ImportReference(quiet);
        FieldReference O(ModuleDefinition mod) => mod == module ? outf : mod.ImportReference(outf);

        foreach (var g in loads.GroupBy(l => l.at))
        {
            var x = g.Key; var mod = x.Module;
            x.Body.SimplifyMacros();
            var il = x.Body.GetILProcessor();
            foreach (var (_, at, handsOut) in g)
            {
                // <fsm> -> dup; [dup; ldc 1; stfld out;] ldc 0; stfld quiet; ldfld delayedEvents
                il.InsertBefore(at, il.Create(OpCodes.Dup));
                if (handsOut) { il.InsertBefore(at, il.Create(OpCodes.Dup)); il.InsertBefore(at, il.Create(OpCodes.Ldc_I4_1)); il.InsertBefore(at, il.Create(OpCodes.Stfld, O(mod))); }
                il.InsertBefore(at, il.Create(OpCodes.Ldc_I4_0));
                il.InsertBefore(at, il.Create(OpCodes.Stfld, Q(mod)));
            }
            x.Body.OptimizeMacros();
            game.MarkChanged(x.Module.Assembly);
        }
        foreach (var g in stores.GroupBy(s => s.at))
        {
            var x = g.Key; var mod = x.Module;
            x.Body.SimplifyMacros();
            var il = x.Body.GetILProcessor();
            var tmp = new VariableDefinition(mod.ImportReference(delayed.FieldType));
            x.Body.Variables.Add(tmp);
            foreach (var (_, at) in g)
            {
                // <fsm> <value> -> stloc tmp; dup; ldc 0; stfld quiet; ldloc tmp; stfld delayedEvents
                il.InsertBefore(at, il.Create(OpCodes.Stloc, tmp));
                il.InsertBefore(at, il.Create(OpCodes.Dup));
                il.InsertBefore(at, il.Create(OpCodes.Ldc_I4_0));
                il.InsertBefore(at, il.Create(OpCodes.Stfld, Q(mod)));
                il.InsertBefore(at, il.Create(OpCodes.Ldloc, tmp));
            }
            x.Body.OptimizeMacros();
            game.MarkChanged(x.Module.Assembly);
        }

        // UpdateDelayedEvents: if (quiet) return;
        //   if (removeEvents.Count == 0 && updateEvents.Count == 0 && delayedEvents != null && delayedEvents.Count == 0) { quiet = !out; return; }
        //   <the method as it was>
        var body = m.Body;
        body.SimplifyMacros();
        var mil = body.GetILProcessor();
        var first = ins[0];
        var check = mil.Create(OpCodes.Ldarg_0);
        var seq = new List<Instruction>
        {
            mil.Create(OpCodes.Ldarg_0), mil.Create(OpCodes.Ldfld, quiet), mil.Create(OpCodes.Brfalse, check), mil.Create(OpCodes.Ret),
            check, mil.Create(OpCodes.Ldfld, remove), mil.Create(OpCodes.Callvirt, count), mil.Create(OpCodes.Brtrue, first),
            mil.Create(OpCodes.Ldarg_0), mil.Create(OpCodes.Ldfld, update), mil.Create(OpCodes.Callvirt, count), mil.Create(OpCodes.Brtrue, first),
            mil.Create(OpCodes.Ldarg_0), mil.Create(OpCodes.Ldfld, delayed), mil.Create(OpCodes.Brfalse, first),
            mil.Create(OpCodes.Ldarg_0), mil.Create(OpCodes.Ldfld, delayed), mil.Create(OpCodes.Callvirt, count), mil.Create(OpCodes.Brtrue, first),
            mil.Create(OpCodes.Ldarg_0), mil.Create(OpCodes.Ldarg_0), mil.Create(OpCodes.Ldfld, outf), mil.Create(OpCodes.Ldc_I4_0), mil.Create(OpCodes.Ceq),
            mil.Create(OpCodes.Stfld, quiet), mil.Create(OpCodes.Ret),
        };
        foreach (var x in seq) mil.InsertBefore(first, x);
        body.OptimizeMacros();
        game.MarkChanged(asm);
        return r0;
    }

    /// <summary>The length of playmaker-delayedevents' test at the method's start (16 instructions), or 0.</summary>
    static int PrefixLength(MethodDefinition m)
    {
        var ins = m.Body.Instructions;
        if (ins.Count > 16 && PlayMakerLib.IsThis(m, ins[0]) && ins[1].OpCode.Code == Code.Ldfld &&
            ins[2].Operand is MethodReference c0 && c0.Name == "get_Count" && ins[15].OpCode.Code == Code.Ret)
            return 16;
        return 0;
    }
}
