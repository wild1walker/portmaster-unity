// playmaker-idle (kind Optimize). PlayMakerFSM.Update calls Fsm.Update for every enabled FSM every
// frame, and most FSMs of a scene sit in a state whose actions have all finished, waiting for an
// event. For such an FSM Fsm.Update changes nothing:
//   FsmTime.RealtimeBugFix()                   (kept: the patch's test comes after it)
//   owner == null -> return; HitBreakpoint -> return
//   FsmExecutionStack.PushFsm(this) ... PopFsm()   back where it was (but its MaxStackCount, a debug
//                                              statistic nothing else reads: with the stack left
//                                              deeper by an exception in an update, a skipped update
//                                              can leave its high-water mark one lower)
//   activeStateEntered, else Continue()         (the test needs it entered)
//   UpdateDelayedEvents()                      nothing with no delayed events and both work lists empty
//   UpdateState(ActiveState):
//     state.Fsm = this                         the same Fsm (an FSM's states are its own)
//     state.OnUpdate()                         nothing once the state is finished
//     UpdateStateChanges(): no state switch pending; the loop counts' reset writes 0 where every
//       count is 0 already; EventTarget = null when it is null already
// The patch adds Fsm.<asmpatch>Idle(), true exactly then (an active state that is set and finished,
// no switch pending, no breakpoint, the three delayed-event lists empty, every state of the FSM's
// array there and with a loop count of 0, no event target), and Fsm.Update returns right after
// FsmTime.RealtimeBugFix() when it is true. FsmState gets <asmpatch>Finished() and <asmpatch>Zero()
// (its private finished flag; its loop count is 0). Every method the test stands for is checked to
// be as described; otherwise the game is left as it is. ASMPATCH_PLAYMAKER_IDLE=0 at run time turns
// the test off (read once): measurements. Opt-in: the MaxStackCount difference above, and on the
// handhelds no frame rate it could be told apart by (README.md).
using Mono.Cecil;
using Mono.Cecil.Cil;
using Mono.Cecil.Rocks;

namespace AsmPatch;

sealed class PlayMakerIdle : Patch
{
    public override string Name => "playmaker-idle";
    public override Kind Kind => Kind.Optimize;
    public override bool OptIn => true;
    public override string Summary =>
        "PlayMaker's Fsm.Update returns at once for an FSM whose state has finished and that has nothing pending (it would change nothing)";

    const string IdleName = "<asmpatch>Idle", FinishedName = "<asmpatch>Finished", ZeroName = "<asmpatch>Zero";
    public const string Variable = "ASMPATCH_PLAYMAKER_IDLE";

    static List<(string type, string name)> Calls(MethodDefinition m) =>
        m.Body.Instructions.Where(i => i.OpCode.Code is Code.Call or Code.Callvirt && i.Operand is MethodReference)
            .Select(i => ((MethodReference)i.Operand).DeclaringType.GetElementType().Name + "::" + ((MethodReference)i.Operand).Name)
            .Select(s => (s.Split("::")[0], s.Split("::")[1])).ToList();

    static bool Same(List<(string type, string name)> got, params string[] want) =>
        got.Count == want.Length && got.Zip(want).All(p => p.First.type + "::" + p.First.name == p.Second);

    public override Result Run(Game game, bool apply)
    {
        var (asm, fsm) = PlayMakerLib.Find(game);
        if (asm == null) return Result.NotApplicable("no PlayMaker (HutongGames.PlayMaker.Fsm)");
        var module = asm.MainModule;
        var file = game.FileOf(asm);
        var state = module.GetType(PlayMakerLib.StateType);
        var where = $"{file}: {fsm.FullName}.Update";
        MethodDefinition M(TypeDefinition t, string name, int pars) =>
            t?.Methods.FirstOrDefault(m => m.Name == name && !m.IsStatic && m.Parameters.Count == pars && m.HasBody);
        var update = M(fsm, "Update", 0);
        if (update == null || state == null) return Result.NotApplicable($"{file}: no Fsm.Update or FsmState");
        if (fsm.Methods.Any(m => m.Name == IdleName))
            return new Result { Outcome = Outcome.AlreadyApplied, Details = { $"{where}: returns at once for an idle FSM" } };

        // the methods the test stands for, as described above
        var updateState = M(fsm, "UpdateState", 1);
        var usc = M(fsm, "UpdateStateChanges", 0);
        var ude = M(fsm, "UpdateDelayedEvents", 0);
        var onUpdate = M(state, "OnUpdate", 0);
        if (updateState == null || usc == null || ude == null || onUpdate == null)
            return Result.NotApplicable($"{file}: no Fsm.UpdateState, UpdateStateChanges, UpdateDelayedEvents or FsmState.OnUpdate");
        if (!Same(Calls(update), "FsmTime::RealtimeBugFix", "Object::op_Equality", "FsmExecutionStack::get_StackCount",
                  "Fsm::get_HitBreakpoint", "FsmExecutionStack::PushFsm", "Fsm::Continue", "Fsm::UpdateDelayedEvents",
                  "Fsm::get_ActiveState", "Fsm::get_ActiveState", "Fsm::UpdateState", "FsmExecutionStack::PopFsm",
                  "FsmExecutionStack::get_StackCount", "String::Concat", "Debug::LogError") ||
            update.Body.Instructions[0].OpCode.Code != Code.Call)
            return Result.NotApplicable($"{where} is not as this patch knows it");
        if (!Same(Calls(updateState), "FsmState::set_Fsm", "FsmState::OnUpdate", "Fsm::UpdateStateChanges"))
            return Result.NotApplicable($"{file}: Fsm.UpdateState is not as this patch knows it");
        // FsmState.OnUpdate: if (!finished) { ... } and nothing after it
        var oi = onUpdate.Body.Instructions;
        // (either "brtrue <the last ret>" or "brfalse <past a ret>; ret")
        bool skipsAll = oi.Count >= 5 && (oi[2].OpCode.Code is Code.Brtrue or Code.Brtrue_S && oi[2].Operand == oi[^1] && oi[^1].OpCode.Code == Code.Ret ||
                                          oi[2].OpCode.Code is Code.Brfalse or Code.Brfalse_S && oi[2].Operand == oi[4] && oi[3].OpCode.Code == Code.Ret);
        if (!skipsAll || !PlayMakerLib.IsThis(onUpdate, oi[0]) || oi[1].OpCode.Code != Code.Ldfld ||
            ((FieldReference)oi[1].Operand).Resolve() is not FieldDefinition finished || finished.DeclaringType != state ||
            finished.FieldType.MetadataType != MetadataType.Boolean)
            return Result.NotApplicable($"{file}: FsmState.OnUpdate does not begin with if (!finished)");
        // UpdateStateChanges: the switch loop, the loop counts' reset (playmaker-loopcount's or PlayMaker's), EventTarget = null at the end
        var ui = usc.Body.Instructions;
        var uscCalls = Calls(usc).Select(c => c.type + "::" + c.name).Distinct().OrderBy(s => s, StringComparer.Ordinal).ToArray();
        var want = new[] { "Fsm::SwitchState", "Fsm::get_HitBreakpoint", "Fsm::get_IsSwitchingState", "Fsm::get_States",
                           "Fsm::set_EventTarget", "FsmState::ResetLoopCount" }.OrderBy(s => s, StringComparer.Ordinal).ToArray();
        if (!uscCalls.SequenceEqual(want) || ui.Count < 4 || ui[^1].OpCode.Code != Code.Ret || ui[^2].OpCode.Code != Code.Call ||
            ((MethodReference)ui[^2].Operand).Name != "set_EventTarget" || ui[^3].OpCode.Code != Code.Ldnull)
            return Result.NotApplicable($"{file}: Fsm.UpdateStateChanges is not as this patch knows it ({string.Join(", ", uscCalls)})");
        // the fields and getters the test reads
        FieldDefinition F(string name) => fsm.Fields.FirstOrDefault(f => f.Name == name && !f.IsStatic);
        var entered = F("activeStateEntered");
        var active = F("activeState");
        var switchTo = F("switchToState");
        var delayed = F("delayedEvents");
        var upd = F("updateEvents");
        var rem = F("removeEvents");
        var hit = fsm.Methods.FirstOrDefault(m => m.Name == "get_HitBreakpoint" && m.IsStatic && !m.HasParameters);
        var getTarget = fsm.Methods.FirstOrDefault(m => m.Name == "get_EventTarget" && !m.IsStatic && !m.HasParameters);
        var getSwitching = M(fsm, "get_IsSwitchingState", 0);
        // List<DelayedEvent>.Count as UpdateDelayedEvents calls it
        var count = ude.Body.Instructions.FirstOrDefault(i => i.OpCode.Code is Code.Call or Code.Callvirt &&
                                                              i.Operand is MethodReference r && r.Name == "get_Count")?.Operand as MethodReference;
        if (entered == null || active == null || switchTo == null || delayed == null || upd == null || rem == null || hit == null ||
            getTarget == null || count == null || getSwitching == null ||
            getSwitching.Body.Instructions.Count(i => i.OpCode.Code == Code.Ldfld && Cecil.SameField(i.Operand, switchTo)) != 1)
            return Result.NotApplicable($"{file}: Fsm's fields are not as this patch knows them");
        // Fsm.States { get => states; } and FsmState.ResetLoopCount() { loopCount = 0; } (the field, or a setter that stores it)
        var getStates = M(fsm, "get_States", 0);
        var gi = getStates?.Body.Instructions;
        var statesField = gi is { Count: 3 } && PlayMakerLib.IsThis(getStates, gi[0]) && gi[1].OpCode.Code == Code.Ldfld && gi[2].OpCode.Code == Code.Ret
            ? ((FieldReference)gi[1].Operand).Resolve() : null;
        var reset = M(state, "ResetLoopCount", 0);
        var ri = reset?.Body.Instructions;
        FieldDefinition countField = null;
        if (ri is { Count: 4 } && PlayMakerLib.IsThis(reset, ri[0]) && Cecil.ConstInt(ri[1]) == 0 && ri[3].OpCode.Code == Code.Ret)
        {
            if (ri[2].OpCode.Code == Code.Stfld) countField = ((FieldReference)ri[2].Operand).Resolve();
            else if (ri[2].OpCode.Code == Code.Call && ri[2].Operand is MethodReference sr && sr.Resolve() is MethodDefinition sd &&
                     sd.DeclaringType == state && sd.Body?.Instructions is { Count: 4 } si && si[2].OpCode.Code == Code.Stfld)
                countField = ((FieldReference)si[2].Operand).Resolve();
        }
        if (statesField == null || statesField.DeclaringType != fsm || statesField.FieldType is not ArrayType { Rank: 1 } ||
            countField == null || countField.DeclaringType != state || countField.FieldType.MetadataType != MetadataType.Int32)
            return Result.NotApplicable($"{file}: Fsm.States or FsmState.ResetLoopCount is not as this patch knows it");

        var r = new Result { Outcome = Outcome.Applied };
        r.Details.Add($"{where}: returns at once for an FSM whose state has finished, with nothing pending");
        if (!apply) return r;

        // FsmState.<asmpatch>Finished() => finished
        var fin = new MethodDefinition(FinishedName, MethodAttributes.Assembly | MethodAttributes.HideBySig, module.TypeSystem.Boolean);
        state.Methods.Add(fin);
        var fil = fin.Body.GetILProcessor();
        fil.Emit(OpCodes.Ldarg_0); fil.Emit(OpCodes.Ldfld, finished); fil.Emit(OpCodes.Ret);
        // FsmState.<asmpatch>Zero() => loopCount == 0
        var zero = new MethodDefinition(ZeroName, MethodAttributes.Assembly | MethodAttributes.HideBySig, module.TypeSystem.Boolean);
        state.Methods.Add(zero);
        var zil = zero.Body.GetILProcessor();
        zil.Emit(OpCodes.Ldarg_0); zil.Emit(OpCodes.Ldfld, countField); zil.Emit(OpCodes.Ldc_I4_0); zil.Emit(OpCodes.Ceq); zil.Emit(OpCodes.Ret);

        // Fsm.<asmpatch>Idle(); ASMPATCH_PLAYMAKER_IDLE=0 turns it off (read once: static int mode, 0 not read, 1 on, 2 off)
        var ts = module.TypeSystem;
        var mode = new FieldDefinition("<asmpatch>idleMode", FieldAttributes.Private | FieldAttributes.Static, ts.Int32);
        fsm.Fields.Add(mode);
        var env = new MethodReference("GetEnvironmentVariable", ts.String, new TypeReference("System", "Environment", module, ts.CoreLibrary));
        env.Parameters.Add(new ParameterDefinition(ts.String));
        var streq = new MethodReference("op_Equality", ts.Boolean, new TypeReference("System", "String", module, ts.CoreLibrary));
        streq.Parameters.Add(new ParameterDefinition(ts.String)); streq.Parameters.Add(new ParameterDefinition(ts.String));
        var idle = new MethodDefinition(IdleName, MethodAttributes.Private | MethodAttributes.HideBySig, ts.Boolean);
        fsm.Methods.Add(idle);
        var il = idle.Body.GetILProcessor();
        var no = il.Create(OpCodes.Ldc_I4_0);
        var read = il.Create(OpCodes.Ldsfld, mode);
        var two = il.Create(OpCodes.Ldc_I4_2);
        var store = il.Create(OpCodes.Stsfld, mode);
        //   if (mode == 0) mode = Environment.GetEnvironmentVariable(V) == "0" ? 2 : 1;  if (mode == 2) return false;
        il.Emit(OpCodes.Ldsfld, mode); il.Emit(OpCodes.Brtrue, read);
        il.Emit(OpCodes.Ldstr, Variable); il.Emit(OpCodes.Call, env); il.Emit(OpCodes.Ldstr, "0"); il.Emit(OpCodes.Call, streq);
        il.Emit(OpCodes.Brtrue, two); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Br, store);
        il.Append(two); il.Append(store);
        il.Append(read); il.Emit(OpCodes.Ldc_I4_2); il.Emit(OpCodes.Beq, no);
        void Field(FieldDefinition f) { il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, f); }
        Field(entered); il.Emit(OpCodes.Brfalse, no);                               // entered
        Field(switchTo); il.Emit(OpCodes.Brtrue, no);                               // no switch pending
        il.Emit(OpCodes.Call, hit); il.Emit(OpCodes.Brtrue, no);                    // no breakpoint
        Field(active); il.Emit(OpCodes.Brfalse, no);                                // a state (set: its getter would not look it up)
        Field(active); il.Emit(OpCodes.Call, fin); il.Emit(OpCodes.Brfalse, no);    // ... that has finished
        foreach (var l in new[] { delayed, upd, rem })
        {
            Field(l); il.Emit(OpCodes.Brfalse, no);
            Field(l); il.Emit(OpCodes.Callvirt, count); il.Emit(OpCodes.Brtrue, no);    // empty
        }
        il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Call, getTarget); il.Emit(OpCodes.Brtrue, no);   // no event target
        // every state of the array there (else the reset's NullReferenceException) with a loop count of 0
        var arr = new VariableDefinition(statesField.FieldType); var ix = new VariableDefinition(ts.Int32); var st = new VariableDefinition(state);
        idle.Body.Variables.Add(arr); idle.Body.Variables.Add(ix); idle.Body.Variables.Add(st);
        idle.Body.InitLocals = true;
        Field(statesField); il.Emit(OpCodes.Stloc, arr);
        il.Emit(OpCodes.Ldloc, arr); il.Emit(OpCodes.Brfalse, no);
        il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Stloc, ix);
        var cond = il.Create(OpCodes.Ldloc, ix);
        il.Emit(OpCodes.Br, cond);
        var body = il.Create(OpCodes.Ldloc, arr);
        il.Append(body); il.Emit(OpCodes.Ldloc, ix); il.Emit(OpCodes.Ldelem_Ref); il.Emit(OpCodes.Stloc, st);
        il.Emit(OpCodes.Ldloc, st); il.Emit(OpCodes.Brfalse, no);
        il.Emit(OpCodes.Ldloc, st); il.Emit(OpCodes.Call, zero); il.Emit(OpCodes.Brfalse, no);
        il.Emit(OpCodes.Ldloc, ix); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Add); il.Emit(OpCodes.Stloc, ix);
        il.Append(cond); il.Emit(OpCodes.Ldloc, arr); il.Emit(OpCodes.Ldlen); il.Emit(OpCodes.Conv_I4); il.Emit(OpCodes.Blt, body);
        il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Ret);
        il.Append(no); il.Emit(OpCodes.Ret);
        idle.Body.OptimizeMacros();

        // Fsm.Update: after FsmTime.RealtimeBugFix(), if (<asmpatch>Idle()) return;
        var ul = update.Body.GetILProcessor();
        var first = update.Body.Instructions[0];
        var next = first.Next;
        var seq = new[] { ul.Create(OpCodes.Ldarg_0), ul.Create(OpCodes.Call, idle), ul.Create(OpCodes.Brfalse, next), ul.Create(OpCodes.Ret) };
        var prev = first;
        foreach (var x in seq) { ul.InsertAfter(prev, x); prev = x; }
        update.Body.SimplifyMacros(); update.Body.OptimizeMacros();
        game.MarkChanged(asm);
        return r;
    }
}
