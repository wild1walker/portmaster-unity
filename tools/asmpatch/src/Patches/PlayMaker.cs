// PlayMaker (Hutong Games' visual scripting runtime, PlayMaker.dll): two places where the runtime
// does work every frame for every FSM that leaves nothing changed, done only when it can change
// something. The idea comes from Clazex's HollowKnight.AlreadyEnoughPlayMaker mod (MIT); these
// rewrites are our own and keep PlayMaker's behaviour exactly (the mod's versions do not: see
// docs/profiling.md, "PlayMaker's per-frame work").
//
// playmaker-loopcount. Fsm.UpdateStateChanges (after every Update, FixedUpdate and LateUpdate of
// an FSM, and after each transition) sets loopCount = 0 on every state of the FSM: the count
// behind PlayMaker's guard against an FSM entering states more than MaxLoopCount times in one
// update. Only FsmState.OnEnter makes a count other than 0, so when no state anywhere was entered
// since this FSM last reset its states, and its state array has not changed, every count the loop
// would set is 0 already. The patch keeps a program-wide change number (<asmpatch>PlayMaker::
// Changes) that goes up after every write of a count other than 0 and after every store into a
// state array or into Fsm.states, and in each Fsm the number its last whole reset saw (plus one);
// UpdateStateChanges skips the loop while the two agree. Every count, the guard and its error are
// as before. The state arrays are followed through every assembly of the game (the field, its
// getter, locals, wrappers that return it): a use the patch cannot follow (the array passed to a
// method, kept in another field, the field's address taken) leaves the game unpatched.
//
// playmaker-delayedevents. Fsm.UpdateDelayedEvents (every Update of every FSM) clears two private
// work lists and copies the FSM's delayed events into one of them before it updates them. With no
// delayed events and both work lists empty it changes nothing but the lists' version numbers,
// which only an enumerator of those lists could see, and nothing else ever holds them (the patch
// checks): the method then returns at once.
using Mono.Cecil;
using Mono.Cecil.Cil;
using Mono.Cecil.Rocks;

namespace AsmPatch;

static class PlayMakerLib
{
    public const string FsmType = "HutongGames.PlayMaker.Fsm", StateType = "HutongGames.PlayMaker.FsmState";
    public const string HelperType = "<asmpatch>PlayMaker", ChangesField = "Changes", MarkField = "<asmpatch>loopCountsReset";

    /// <summary>PlayMaker's assembly (the one defining HutongGames.PlayMaker.Fsm) and its Fsm type.</summary>
    public static (AssemblyDefinition asm, TypeDefinition fsm) Find(Game game)
    {
        foreach (var a in game.GameAssemblies)
        {
            var t = a.MainModule.GetType(FsmType);
            if (t != null) return (a, t);
        }
        return (null, null);
    }

    public static bool IsThis(MethodDefinition m, Instruction i) =>
        !m.IsStatic && (i.OpCode.Code == Code.Ldarg_0 || i.OpCode.Code is Code.Ldarg or Code.Ldarg_S && i.Operand == m.Body.ThisParameter);
    public static bool IsLdloc(Instruction i, int local) =>
        i.OpCode.Code is not (Code.Ldloca or Code.Ldloca_S) && Cecil.LocalIndex(i, false) == local;
    public static bool IsBr(Instruction i) => i.OpCode.Code is Code.Br or Code.Br_S;
    public static bool IsBlt(Instruction i) => i.OpCode.Code is Code.Blt or Code.Blt_S;
    public static bool IsBrfalse(Instruction i) => i.OpCode.Code is Code.Brfalse or Code.Brfalse_S;
    public static bool IsCall(Instruction i, string type, string name, int pars) =>
        i.OpCode.Code is Code.Call or Code.Callvirt && i.Operand is MethodReference r && r.Name == name &&
        r.Parameters.Count == pars && r.DeclaringType.GetElementType().FullName == type;
    public static bool IsLdfld(Instruction i, FieldDefinition f) => i.OpCode.Code == Code.Ldfld && Cecil.SameField(i.Operand, f);

    /// <summary>Every instruction that branches to, or an exception handler that starts or ends at, i.</summary>
    public static bool Targeted(MethodBody b, Instruction i) =>
        b.Instructions.Any(x => x.Operand == i || x.Operand is Instruction[] t && t.Contains(i)) ||
        b.ExceptionHandlers.Any(h => h.TryStart == i || h.TryEnd == i || h.HandlerStart == i || h.HandlerEnd == i || h.FilterStart == i);

    /// <summary>The private static class the patches keep their counter in (one per module).</summary>
    public static FieldDefinition Changes(ModuleDefinition m, bool create)
    {
        var t = m.Types.FirstOrDefault(x => x.Namespace == "" && x.Name == HelperType);
        if (t == null)
        {
            if (!create) return null;
            t = new TypeDefinition("", HelperType, TypeAttributes.NotPublic | TypeAttributes.Abstract | TypeAttributes.Sealed |
                                                   TypeAttributes.BeforeFieldInit | TypeAttributes.Class, m.TypeSystem.Object);
            m.Types.Add(t);
        }
        var f = t.Fields.FirstOrDefault(x => x.Name == ChangesField);
        if (f == null && create)
        {
            f = new FieldDefinition(ChangesField, FieldAttributes.Assembly | FieldAttributes.Static, m.TypeSystem.Int64);
            t.Fields.Add(f);
        }
        return f;
    }

    /// <summary>Changes += 1, after the instruction at.</summary>
    public static void BumpAfter(MethodDefinition m, Instruction at, FieldReference changes)
    {
        var il = m.Body.GetILProcessor();
        var seq = new[]
        {
            il.Create(OpCodes.Ldsfld, changes), il.Create(OpCodes.Ldc_I4_1), il.Create(OpCodes.Conv_I8),
            il.Create(OpCodes.Add), il.Create(OpCodes.Stsfld, changes),
        };
        var prev = at;
        foreach (var x in seq) { il.InsertAfter(prev, x); prev = x; }
    }
}

sealed class PlayMakerLoopCount : Patch
{
    public override string Name => "playmaker-loopcount";
    public override Kind Kind => Kind.Optimize;
    public override string Summary =>
        "PlayMaker's Fsm.UpdateStateChanges resets its states' loop counts only when one can be other than 0 (every count, and the loop guard, as before)";

    public override Result Run(Game game, bool apply)
    {
        var (asm, fsm) = PlayMakerLib.Find(game);
        if (asm == null) return Result.NotApplicable("no PlayMaker (HutongGames.PlayMaker.Fsm)");
        var module = asm.MainModule;
        var file = game.FileOf(asm);
        var state = module.GetType(PlayMakerLib.StateType);
        var usc = fsm.Methods.FirstOrDefault(m => m.Name == "UpdateStateChanges" && !m.IsStatic && !m.HasParameters && m.HasBody &&
                                                  m.ReturnType.MetadataType == MetadataType.Void);
        var reset = state?.Methods.FirstOrDefault(m => m.Name == "ResetLoopCount" && !m.IsStatic && !m.HasParameters && m.HasBody);
        var getStates = fsm.Methods.FirstOrDefault(m => m.Name == "get_States" && !m.IsStatic && !m.HasParameters && m.HasBody);
        if (usc == null || reset == null || getStates == null)
            return Result.NotApplicable($"{file}: no Fsm.UpdateStateChanges, FsmState.ResetLoopCount or Fsm.States");
        var where = $"{file}: {fsm.FullName}.UpdateStateChanges";
        var changes = PlayMakerLib.Changes(module, false);
        if (changes != null && usc.Body.Instructions.Any(i => i.OpCode.Code == Code.Ldsfld && Cecil.SameField(i.Operand, changes)))
            return new Result { Outcome = Outcome.AlreadyApplied, Details = { $"{where}: resets loop counts only after a change" } };

        // Fsm.States { get => states; }
        var gi = getStates.Body.Instructions;
        if (gi.Count != 3 || !PlayMakerLib.IsThis(getStates, gi[0]) || gi[1].OpCode.Code != Code.Ldfld || gi[2].OpCode.Code != Code.Ret ||
            ((FieldReference)gi[1].Operand).Resolve() is not FieldDefinition statesField || statesField.DeclaringType != fsm ||
            statesField.FieldType is not ArrayType { Rank: 1 } || statesField.IsStatic)
            return Result.NotApplicable($"{file}: Fsm.States is not a plain getter of an FsmState[] field");
        // FsmState.ResetLoopCount() { loopCount = 0; }: a write of 0 through the setter or the field
        var ri = reset.Body.Instructions;
        if (ri.Count != 4 || !PlayMakerLib.IsThis(reset, ri[0]) || Cecil.ConstInt(ri[1]) != 0 || ri[3].OpCode.Code != Code.Ret)
            return Result.NotApplicable($"{file}: FsmState.ResetLoopCount is not loopCount = 0");
        var countWrite = ri[2];
        FieldDefinition countField;
        MethodDefinition countSetter = null;
        if (countWrite.OpCode.Code == Code.Stfld) countField = ((FieldReference)countWrite.Operand).Resolve();
        else if (countWrite.OpCode.Code is Code.Call && countWrite.Operand is MethodReference sr && sr.Resolve() is MethodDefinition sd &&
                 sd.DeclaringType == state && sd.Body?.Instructions is { Count: 4 } si && si[2].OpCode.Code == Code.Stfld)
        { countSetter = sd; countField = ((FieldReference)si[2].Operand).Resolve(); }
        else return Result.NotApplicable($"{file}: FsmState.ResetLoopCount is not loopCount = 0");
        if (countField == null || countField.DeclaringType != state || countField.IsStatic || countField.IsPublic)
            return Result.NotApplicable($"{file}: FsmState's loop count is not a private field");

        // the reset loop: for (int i = 0; i < States.Length; i++) States[i].ResetLoopCount();
        var loop = MatchLoop(usc, statesField, getStates, reset);
        if (loop == null) return Result.NotApplicable($"{where}: not the reset loop this patch knows");

        // every write of the count (other than a 0) and every store of a state array
        var writes = new List<(MethodDefinition m, Instruction at)>();
        foreach (var a in game.GameAssemblies)
            foreach (var t in Cecil.AllTypes(a.MainModule))
                foreach (var m in t.Methods.Where(x => x.HasBody))
                {
                    var ins = m.Body.Instructions;
                    for (int k = 0; k < ins.Count; k++)
                    {
                        var i = ins[k];
                        bool field = i.Operand is FieldReference fr && Cecil.SameField(fr, countField);
                        if (field && i.OpCode.Code is Code.Ldflda or Code.Ldsflda)
                            return Result.NotApplicable($"{file}: {Where(m)} takes the loop count's address");
                        bool write = field && i.OpCode.Code == Code.Stfld ||
                                     countSetter != null && i.OpCode.Code is Code.Call or Code.Callvirt && i.Operand is MethodReference mr &&
                                     mr.Name == countSetter.Name && mr.DeclaringType.FullName == state.FullName;
                        if (!write || m == countSetter) continue;
                        bool zero = k > 0 && Cecil.ConstInt(ins[k - 1]) == 0 && !PlayMakerLib.Targeted(m.Body, ins[k - 1]) && !PlayMakerLib.Targeted(m.Body, i);
                        if (!zero) writes.Add((m, i));
                    }
                    foreach (var i in ins.Where(x => x.OpCode.Code is Code.Ldftn or Code.Ldvirtftn && x.Operand is MethodReference r &&
                                                    countSetter != null && r.Name == countSetter.Name && r.DeclaringType.FullName == state.FullName))
                        return Result.NotApplicable($"{file}: {Where(m)} makes a delegate of the loop count's setter");
                }
        if (writes.Count == 0) return Result.NotApplicable($"{file}: nothing writes a loop count other than 0");
        var arrays = StateArrays.Follow(game, statesField, getStates);
        if (arrays.Escape != null) return Result.NotApplicable(arrays.Escape);
        // the counter is PlayMaker's own (internal): every place that has to count must be in PlayMaker
        var outside = writes.Concat(arrays.Stores).FirstOrDefault(x => x.m.Module != module);
        if (outside.m != null)
            return Result.NotApplicable($"{game.FileOf(outside.m.Module.Assembly)}: {Where(outside.m)} writes PlayMaker's state data");

        var r = new Result { Outcome = Outcome.Applied };
        r.Details.Add($"{where}: the reset loop runs only after a change (counted at {writes.Count} loop count write(s), " +
                      $"{arrays.Stores.Count} state array store(s) in {arrays.Stores.Select(s => Where(s.m)).Distinct().Count()} method(s))");
        if (!apply) return r;

        var ch = PlayMakerLib.Changes(module, true);
        var mark = new FieldDefinition(PlayMakerLib.MarkField, FieldAttributes.Private | FieldAttributes.NotSerialized, module.TypeSystem.Int64);
        fsm.Fields.Add(mark);
        foreach (var (m, at) in writes.Concat(arrays.Stores))
        {
            m.Body.SimplifyMacros();
            PlayMakerLib.BumpAfter(m, at, ch);
        }
        // UpdateStateChanges: if (mark == Changes + 1) skip the loop; after the loop: mark = Changes + 1
        var body = usc.Body;
        body.SimplifyMacros();
        var il = body.GetILProcessor();
        var init = loop.Value.init;             // ldc.i4.0 of "int i = 0"; branches to it reach the check
        var after = loop.Value.exit;
        var initCopy = il.Create(OpCodes.Ldc_I4_0);
        var check = new[]
        {
            il.Create(OpCodes.Ldfld, mark), il.Create(OpCodes.Ldsfld, ch), il.Create(OpCodes.Ldc_I4_1), il.Create(OpCodes.Conv_I8),
            il.Create(OpCodes.Add), il.Create(OpCodes.Beq, after), initCopy,
        };
        init.OpCode = OpCodes.Ldarg_0; init.Operand = null;
        var prev = init;
        foreach (var x in check) { il.InsertAfter(prev, x); prev = x; }
        foreach (var x in new[]
                 {
                     il.Create(OpCodes.Ldarg_0), il.Create(OpCodes.Ldsfld, ch), il.Create(OpCodes.Ldc_I4_1), il.Create(OpCodes.Conv_I8),
                     il.Create(OpCodes.Add), il.Create(OpCodes.Stfld, mark),
                 })
            il.InsertBefore(after, x);
        foreach (var m in writes.Concat(arrays.Stores).Select(x => x.m).Append(usc).Distinct()) m.Body.OptimizeMacros();
        game.MarkChanged(asm);
        return r;
    }

    static string Where(MethodDefinition m) => m.DeclaringType.FullName + "." + m.Name;

    /// <summary>The loop "for (int i = 0; i &lt; States.Length; i++) States[i].ResetLoopCount();" as C#
    /// compilers emit it, with nothing else branching into it: its first instruction (ldc.i4.0) and
    /// the one after it.</summary>
    static (Instruction init, Instruction exit)? MatchLoop(MethodDefinition m, FieldDefinition states, MethodDefinition getter, MethodDefinition reset)
    {
        var ins = m.Body.Instructions;
        bool Arr(Instruction i) => PlayMakerLib.IsLdfld(i, states) ||
                                    i.OpCode.Code is Code.Call or Code.Callvirt && i.Operand is MethodReference r && r.Name == getter.Name &&
                                    r.DeclaringType.FullName == getter.DeclaringType.FullName && r.Parameters.Count == 0;
        for (int k = 0; k + 18 <= ins.Count; k++)
        {
            if (Cecil.ConstInt(ins[k]) != 0 || Cecil.LocalIndex(ins[k + 1], true) is not int v || !PlayMakerLib.IsBr(ins[k + 2])) continue;
            int b = k + 3;   // body: ldarg.0, States, ldloc i, ldelem.ref, callvirt ResetLoopCount, ldloc i, ldc.i4.1, add, stloc i
            if (!(PlayMakerLib.IsThis(m, ins[b]) && Arr(ins[b + 1]) && PlayMakerLib.IsLdloc(ins[b + 2], v) && ins[b + 3].OpCode.Code == Code.Ldelem_Ref &&
                  PlayMakerLib.IsCall(ins[b + 4], reset.DeclaringType.FullName, reset.Name, 0) && PlayMakerLib.IsLdloc(ins[b + 5], v) &&
                  Cecil.ConstInt(ins[b + 6]) == 1 && ins[b + 7].OpCode.Code == Code.Add && Cecil.LocalIndex(ins[b + 8], true) == v))
                continue;
            int c = b + 9;   // condition: ldloc i, ldarg.0, States, ldlen, conv.i4, blt body
            if (!(ins[k + 2].Operand == ins[c] && PlayMakerLib.IsLdloc(ins[c], v) && PlayMakerLib.IsThis(m, ins[c + 1]) && Arr(ins[c + 2]) &&
                  ins[c + 3].OpCode.Code == Code.Ldlen && ins[c + 4].OpCode.Code == Code.Conv_I4 && PlayMakerLib.IsBlt(ins[c + 5]) &&
                  ins[c + 5].Operand == ins[b] && c + 6 < ins.Count))
                continue;
            // nothing else jumps into the loop or out of it to elsewhere, no handler boundary inside
            for (int x = k + 1; x <= c + 6; x++)
            {
                var i = ins[x];
                bool ok = x == b && OnlyFrom(m.Body, i, ins[c + 5]) || x == c && OnlyFrom(m.Body, i, ins[k + 2]) ||
                          x == c + 6 || !PlayMakerLib.Targeted(m.Body, i);
                if (!ok) return null;
            }
            return (ins[k], ins[c + 6]);
        }
        return null;
    }

    static bool OnlyFrom(MethodBody b, Instruction target, Instruction from) =>
        b.Instructions.All(x => x == from || x.Operand != target && !(x.Operand is Instruction[] t && t.Contains(target))) &&
        !b.ExceptionHandlers.Any(h => h.TryStart == target || h.TryEnd == target || h.HandlerStart == target || h.HandlerEnd == target || h.FilterStart == target);
}

/// <summary>Where PlayMaker's state arrays go, in every assembly of the game: each store into one
/// (an element, or Fsm.states itself), or why they cannot be followed. A state array is a value of
/// Fsm.states (the field, its getter) or a new FsmState[] (which may become one). It is followed
/// through the stack, locals (by index, whatever the path) and methods that return it; it may be
/// read (ldlen, ldelem), compared, stored into Fsm.states or have its elements set.</summary>
sealed class StateArrays
{
    public readonly List<(MethodDefinition m, Instruction at)> Stores = new();
    public string Escape;

    readonly FieldDefinition field;
    Game game;
    readonly HashSet<string> sources = new(StringComparer.Ordinal);       // methods returning a state array
    readonly string setter, element;

    StateArrays(FieldDefinition f, MethodDefinition getter)
    {
        field = f;
        sources.Add(Key(getter));
        setter = getter.DeclaringType.FullName + "::set_States/1";
        element = ((ArrayType)f.FieldType).ElementType.FullName;
    }

    static string Key(MethodReference m) => m.DeclaringType.GetElementType().FullName + "::" + m.Name + "/" + m.Parameters.Count;

    public static StateArrays Follow(Game game, FieldDefinition f, MethodDefinition getter)
    {
        var s = new StateArrays(f, getter) { game = game };
        var methods = game.GameAssemblies.SelectMany(a => Cecil.AllTypes(a.MainModule)).SelectMany(t => t.Methods).Where(m => m.HasBody).ToList();
        for (int round = 0; round < 16 && s.Escape == null; round++)
        {
            int before = s.sources.Count;
            s.Stores.Clear();
            foreach (var m in methods)
            {
                if (!m.Body.Instructions.Any(i => s.Mentions(i))) continue;
                s.Analyse(m);
                if (s.Escape != null) break;
            }
            if (s.sources.Count == before) return s;
        }
        s.Escape ??= "the state arrays' uses do not settle";
        return s;
    }

    bool Mentions(Instruction i) =>
        i.Operand is FieldReference fr && Cecil.SameField(fr, field) ||
        i.Operand is MethodReference mr && (sources.Contains(Key(mr)) || Key(mr) == setter) ||
        i.OpCode.Code == Code.Newarr && ((TypeReference)i.Operand).FullName == element;

    sealed class V { public bool Arr; }      // (may be) a state array

    void Analyse(MethodDefinition m)
    {
        var body = m.Body;
        var ins = body.Instructions;
        if (ins.Count == 0) return;
        var locals = new bool[body.Variables.Count];
        var entry = new Dictionary<Instruction, List<V>> { [ins[0]] = new() };
        foreach (var h in body.ExceptionHandlers)
        {
            bool exc = h.HandlerType is ExceptionHandlerType.Catch or ExceptionHandlerType.Filter;
            if (h.HandlerStart != null) entry.TryAdd(h.HandlerStart, exc ? new List<V> { new() } : new List<V>());
            if (h.FilterStart != null) entry.TryAdd(h.FilterStart, new List<V> { new() });
        }
        var stores = new HashSet<Instruction>();
        for (int pass = 0; pass < 64; pass++)
        {
            bool changed = false;
            foreach (var i in ins)
            {
                if (!entry.TryGetValue(i, out var e)) continue;
                var st = new List<V>(e);
                changed |= Step(m, i, st, locals, stores);
                if (Escape != null) { Escape = $"{game.FileOf(m.Module.Assembly)}: {m.DeclaringType.FullName}.{m.Name}: {Escape}"; return; }
                var c = i.OpCode.Code;
                if (c is Code.Leave or Code.Leave_S) st.Clear();
                if (i.Operand is Instruction t) changed |= Merge(entry, t, st);
                if (i.Operand is Instruction[] ts) foreach (var x in ts) changed |= Merge(entry, x, st);
                var flow = i.OpCode.FlowControl;
                if (flow is not (FlowControl.Branch or FlowControl.Return or FlowControl.Throw) && c is not (Code.Endfinally or Code.Jmp) && i.Next != null)
                    changed |= Merge(entry, i.Next, st);
            }
            if (!changed) break;
        }
        foreach (var x in stores) Stores.Add((m, x));
    }

    static bool Merge(Dictionary<Instruction, List<V>> entry, Instruction at, List<V> st)
    {
        if (!entry.TryGetValue(at, out var e)) { entry[at] = new List<V>(st); return true; }
        bool ch = false;
        for (int k = 0; k < Math.Min(e.Count, st.Count); k++)
            if (st[k].Arr && !e[k].Arr) { e[k] = new V { Arr = true }; ch = true; }
        return ch;
    }

    static V Pop(List<V> s) { if (s.Count == 0) return new V(); var v = s[^1]; s.RemoveAt(s.Count - 1); return v; }

    bool Step(MethodDefinition m, Instruction i, List<V> s, bool[] locals, HashSet<Instruction> stores)
    {
        var c = i.OpCode.Code;
        switch (c)
        {
            case Code.Ldfld: case Code.Ldsfld:
                if (c == Code.Ldfld) Pop(s);
                s.Add(new V { Arr = Cecil.SameField(i.Operand, field) });
                return false;
            case Code.Ldflda: case Code.Ldsflda:
                if (Cecil.SameField(i.Operand, field)) { Escape = "takes the address of Fsm.states"; return false; }
                if (c == Code.Ldflda) Pop(s);
                s.Add(new V());
                return false;
            case Code.Stfld: case Code.Stsfld:
            {
                var v = Pop(s);
                if (c == Code.Stfld) Pop(s);
                if (Cecil.SameField(i.Operand, field))
                {
                    // (the setter's own store: its callers' arguments are checked where they call it)
                    if (!v.Arr && Key(m) != setter) { Escape = "stores into Fsm.states an array it cannot follow"; return false; }
                    stores.Add(i);
                }
                else if (v.Arr) Escape = $"keeps a state array in {((FieldReference)i.Operand).Name}";
                return false;
            }
            case Code.Ldloc_0: case Code.Ldloc_1: case Code.Ldloc_2: case Code.Ldloc_3: case Code.Ldloc: case Code.Ldloc_S:
            {
                int l = Cecil.LocalIndex(i, false) ?? -1;
                s.Add(new V { Arr = l >= 0 && l < locals.Length && locals[l] });
                return false;
            }
            case Code.Ldloca: case Code.Ldloca_S:
            {
                int l = ((VariableDefinition)i.Operand).Index;
                if (l < locals.Length && locals[l]) { Escape = "takes the address of a local holding a state array"; return false; }
                s.Add(new V());
                return false;
            }
            case Code.Stloc_0: case Code.Stloc_1: case Code.Stloc_2: case Code.Stloc_3: case Code.Stloc: case Code.Stloc_S:
            {
                int l = Cecil.LocalIndex(i, true) ?? -1;
                var v = Pop(s);
                if (v.Arr && l >= 0 && l < locals.Length && !locals[l])
                {
                    // a local that held other values too would make its later loads look like state arrays: fine (more stores counted)
                    locals[l] = true; return true;
                }
                return false;
            }
            case Code.Starg: case Code.Starg_S:
                if (Pop(s).Arr) Escape = "stores a state array into an argument";
                return false;
            case Code.Dup:
            {
                var v = Pop(s);
                s.Add(v); s.Add(v);
                return false;
            }
            case Code.Newarr:
                Pop(s);
                s.Add(new V { Arr = ((TypeReference)i.Operand).FullName == element });
                return false;
            case Code.Ldelema:
            {
                Pop(s);
                if (Pop(s).Arr) { Escape = "takes the address of a state array's element"; return false; }
                s.Add(new V());
                return false;
            }
            case Code.Stelem_Any: case Code.Stelem_Ref: case Code.Stelem_I: case Code.Stelem_I1: case Code.Stelem_I2:
            case Code.Stelem_I4: case Code.Stelem_I8: case Code.Stelem_R4: case Code.Stelem_R8:
            {
                var v = Pop(s); Pop(s); var a = Pop(s);
                if (v.Arr) { Escape = "stores a state array into an array"; return false; }
                if (a.Arr) stores.Add(i);
                return false;
            }
            case Code.Ldlen:
                Pop(s); s.Add(new V());
                return false;
            case Code.Castclass: case Code.Isinst:
            {
                var v = Pop(s);
                s.Add(new V { Arr = v.Arr });
                return false;
            }
            case Code.Ldftn: case Code.Ldvirtftn:
                if (c == Code.Ldvirtftn) Pop(s);
                if (i.Operand is MethodReference fr && (sources.Contains(Key(fr)) || Key(fr) == setter))
                { Escape = $"makes a delegate of {fr.Name}"; return false; }
                s.Add(new V());
                return false;
            case Code.Ret:
                if (m.ReturnType.MetadataType != MetadataType.Void && s.Count > 0 && Pop(s).Arr)
                {
                    if (m.IsVirtual && !m.IsFinal) { Escape = "a virtual method returns a state array"; return false; }
                    sources.Add(Key(m));
                }
                return false;
            case Code.Call: case Code.Callvirt: case Code.Newobj:
            {
                var mr = (MethodReference)i.Operand;
                bool newobj = c == Code.Newobj;
                int n = mr.Parameters.Count + (mr.HasThis && !newobj ? 1 : 0);
                var args = new V[n];
                for (int k = n - 1; k >= 0; k--) args[k] = Pop(s);
                var key = Key(mr);
                if (key == setter && n == 2)
                {
                    if (!args[1].Arr) { Escape = "passes Fsm.States an array it cannot follow"; return false; }
                    stores.Add(i);
                }
                for (int k = 0; k < n; k++)
                {
                    if (!args[k].Arr || key == setter && k == 1) continue;
                    bool self = mr.HasThis && !newobj && k == 0;
                    var dt = mr.DeclaringType.GetElementType().FullName;
                    if (self && dt == "System.Array" && mr.Name is "get_Length" or "get_LongLength" or "GetLength" or "get_Rank") continue;
                    Escape = $"passes a state array to {dt}.{mr.Name}";
                    return false;
                }
                if (newobj || mr.ReturnType.MetadataType != MetadataType.Void)
                    s.Add(new V { Arr = !newobj && sources.Contains(key) });
                return false;
            }
            case Code.Calli:
            {
                var cs = (CallSite)i.Operand;
                Pop(s);
                for (int k = 0; k < cs.Parameters.Count + (cs.HasThis ? 1 : 0); k++)
                    if (Pop(s).Arr) { Escape = "passes a state array to an indirect call"; return false; }
                if (cs.ReturnType.MetadataType != MetadataType.Void) s.Add(new V());
                return false;
            }
            case Code.Stobj: case Code.Stind_Ref:
            {
                var v = Pop(s); Pop(s);
                if (v.Arr) Escape = "stores a state array through a pointer";
                return false;
            }
            case Code.Box: case Code.Mkrefany:
                if (Pop(s).Arr) { Escape = "boxes a state array"; return false; }
                s.Add(new V());
                return false;
        }
        // everything else: by its stack behaviour; reading a state array there (a comparison, a
        // null test, pop) is no use the patch has to know about
        int pop = Pops(i.OpCode.StackBehaviourPop), push = Pushes(i.OpCode.StackBehaviourPush);
        if (pop < 0) s.Clear(); else for (int k = 0; k < pop; k++) Pop(s);
        for (int k = 0; k < push; k++) s.Add(new V());
        return false;
    }

    static int Pops(StackBehaviour b) => b switch
    {
        StackBehaviour.Pop0 => 0,
        StackBehaviour.Pop1 or StackBehaviour.Popi or StackBehaviour.Popref => 1,
        StackBehaviour.Pop1_pop1 or StackBehaviour.Popi_pop1 or StackBehaviour.Popi_popi or StackBehaviour.Popi_popi8 or
            StackBehaviour.Popi_popr4 or StackBehaviour.Popi_popr8 or StackBehaviour.Popref_pop1 or StackBehaviour.Popref_popi => 2,
        StackBehaviour.Popi_popi_popi or StackBehaviour.Popref_popi_popi or StackBehaviour.Popref_popi_popi8 or
            StackBehaviour.Popref_popi_popr4 or StackBehaviour.Popref_popi_popr8 or StackBehaviour.Popref_popi_popref => 3,
        StackBehaviour.PopAll => -1,
        _ => 0,
    };

    static int Pushes(StackBehaviour b) => b switch
    {
        StackBehaviour.Push1_push1 => 2,
        StackBehaviour.Push1 or StackBehaviour.Pushi or StackBehaviour.Pushi8 or StackBehaviour.Pushr4 or StackBehaviour.Pushr8 or StackBehaviour.Pushref => 1,
        _ => 0,
    };
}

sealed class PlayMakerDelayedEvents : Patch
{
    public override string Name => "playmaker-delayedevents";
    public override Kind Kind => Kind.Optimize;
    public override string Summary =>
        "PlayMaker's Fsm.UpdateDelayedEvents returns at once when the FSM has no delayed events and its work lists are empty (what it would leave as it is)";

    public override Result Run(Game game, bool apply)
    {
        var (asm, fsm) = PlayMakerLib.Find(game);
        if (asm == null) return Result.NotApplicable("no PlayMaker (HutongGames.PlayMaker.Fsm)");
        var file = game.FileOf(asm);
        var m = fsm.Methods.FirstOrDefault(x => x.Name == "UpdateDelayedEvents" && !x.IsStatic && !x.HasParameters && x.HasBody &&
                                                x.ReturnType.MetadataType == MetadataType.Void);
        if (m == null) return Result.NotApplicable($"{file}: no Fsm.UpdateDelayedEvents");
        var where = $"{file}: {fsm.FullName}.UpdateDelayedEvents";
        var ins = m.Body.Instructions;
        // already patched: it starts with "if (removeEvents.Count != 0) ..."
        if (ins.Count > 3 && PlayMakerLib.IsThis(m, ins[0]) && ins[1].OpCode.Code == Code.Ldfld &&
            ins[2].Operand is MethodReference c0 && c0.Name == "get_Count")
            return new Result { Outcome = Outcome.AlreadyApplied, Details = { $"{where}: returns at once with nothing to update" } };
        var shape = Match(m);
        if (shape == null) return Result.NotApplicable($"{where}: not the method this patch knows");
        var (remove, update, delayed, count) = shape.Value;
        // the two work lists: private, set only by constructors (to new lists), read only here
        foreach (var f in new[] { remove, update })
        {
            if (!f.IsPrivate || f.IsStatic) return Result.NotApplicable($"{file}: Fsm.{f.Name} is not a private field");
            foreach (var a in game.GameAssemblies)
                foreach (var t in Cecil.AllTypes(a.MainModule))
                    foreach (var x in t.Methods.Where(y => y.HasBody))
                    {
                        var xi = x.Body.Instructions;
                        for (int k = 0; k < xi.Count; k++)
                        {
                            if (!Cecil.SameField(xi[k].Operand, f) || x == m && xi[k].OpCode.Code == Code.Ldfld) continue;
                            bool ctorNew = x.IsConstructor && !x.IsStatic && xi[k].OpCode.Code == Code.Stfld && k > 0 && xi[k - 1].OpCode.Code == Code.Newobj;
                            if (!ctorNew) return Result.NotApplicable($"{file}: {t.FullName}.{x.Name} uses Fsm.{f.Name}");
                        }
                    }
        }
        var r = new Result { Outcome = Outcome.Applied };
        r.Details.Add($"{where}: returns at once when {delayed.Name}, {update.Name} and {remove.Name} are empty");
        if (!apply) return r;

        // if (removeEvents.Count != 0 || updateEvents.Count != 0 || delayedEvents == null || delayedEvents.Count != 0) <the method>; else return;
        var body = m.Body;
        body.SimplifyMacros();
        var il = body.GetILProcessor();
        var first = ins[0];
        var seq = new List<Instruction>
        {
            il.Create(OpCodes.Ldarg_0), il.Create(OpCodes.Ldfld, remove), il.Create(OpCodes.Callvirt, count), il.Create(OpCodes.Brtrue, first),
            il.Create(OpCodes.Ldarg_0), il.Create(OpCodes.Ldfld, update), il.Create(OpCodes.Callvirt, count), il.Create(OpCodes.Brtrue, first),
            il.Create(OpCodes.Ldarg_0), il.Create(OpCodes.Ldfld, delayed), il.Create(OpCodes.Brfalse, first),
            il.Create(OpCodes.Ldarg_0), il.Create(OpCodes.Ldfld, delayed), il.Create(OpCodes.Callvirt, count), il.Create(OpCodes.Brtrue, first),
            il.Create(OpCodes.Ret),
        };
        foreach (var x in seq) il.InsertBefore(first, x);
        body.OptimizeMacros();
        game.MarkChanged(asm);
        return r;
    }

    /// <summary>The method as PlayMaker 1.9 has it:
    ///   removeEvents.Clear(); updateEvents.Clear(); updateEvents.AddRange(delayedEvents);
    ///   for (i < updateEvents.Count) { e = updateEvents[i]; e.Update(); if (e.Finished) removeEvents.Add(e); }
    ///   for (j < removeEvents.Count) delayedEvents.Remove(removeEvents[j]);
    /// with nothing branching to its start: the three List&lt;DelayedEvent&gt; fields and List.Count.</summary>
    static (FieldDefinition remove, FieldDefinition update, FieldDefinition delayed, MethodReference count)? Match(MethodDefinition m)
    {
        var ins = m.Body.Instructions;
        if (ins.Count < 40 || PlayMakerLib.Targeted(m.Body, ins[0]) || m.Body.HasExceptionHandlers) return null;
        FieldDefinition F(Instruction i) => i.OpCode.Code == Code.Ldfld ? ((FieldReference)i.Operand).Resolve() : null;
        bool L(Instruction i, string name, int pars) => i.OpCode.Code is Code.Callvirt or Code.Call && i.Operand is MethodReference r &&
            r.Name == name && r.Parameters.Count == pars && r.DeclaringType is GenericInstanceType g &&
            g.ElementType.FullName == "System.Collections.Generic.List`1" && g.GenericArguments[0].FullName == "HutongGames.PlayMaker.DelayedEvent";
        bool T(int k) => PlayMakerLib.IsThis(m, ins[k]);
        // prefix: removeEvents.Clear(); updateEvents.Clear(); updateEvents.AddRange(delayedEvents);
        if (!(T(0) && F(ins[1]) is { } remove && L(ins[2], "Clear", 0) && T(3) && F(ins[4]) is { } update && L(ins[5], "Clear", 0) &&
              T(6) && F(ins[7]) == update && T(8) && F(ins[9]) is { } delayed && L(ins[10], "AddRange", 1) && remove != update && delayed != update && delayed != remove))
            return null;
        if (remove.DeclaringType != m.DeclaringType || update.DeclaringType != m.DeclaringType || delayed.DeclaringType != m.DeclaringType) return null;
        if (remove.FieldType.FullName != update.FieldType.FullName || delayed.FieldType.FullName != update.FieldType.FullName) return null;
        // the rest: two counted loops over updateEvents and removeEvents whose bodies run only for elements
        // (each loop: ldc.i4.0, stloc, br cond ... cond: ldloc, ldarg.0, ldfld list, get_Count, blt body), then ret
        int k = 11;
        MethodReference count = null;
        foreach (var list in new[] { update, remove })
        {
            if (k + 3 > ins.Count || Cecil.ConstInt(ins[k]) != 0 || Cecil.LocalIndex(ins[k + 1], true) is not int v || !PlayMakerLib.IsBr(ins[k + 2])) return null;
            var cond = (Instruction)ins[k + 2].Operand;
            int c = ins.IndexOf(cond);
            if (c < k + 3 || c + 5 > ins.Count) return null;
            if (!(PlayMakerLib.IsLdloc(ins[c], v) && T(c + 1) && F(ins[c + 2]) == list && L(ins[c + 3], "get_Count", 0) &&
                  PlayMakerLib.IsBlt(ins[c + 4]) && ins[c + 4].Operand == ins[k + 3]))
                return null;
            count ??= (MethodReference)ins[c + 3].Operand;
            // the loop body: no way out of it but its end
            for (int x = k + 3; x < c; x++)
                if (ins[x].Operand is Instruction t && (ins.IndexOf(t) < k + 3 || ins.IndexOf(t) > c) || ins[x].OpCode.FlowControl is FlowControl.Return or FlowControl.Throw)
                    return null;
            k = c + 5;
        }
        if (k != ins.Count - 1 || ins[k].OpCode.Code != Code.Ret) return null;
        return (remove, update, delayed, count);
    }
}
