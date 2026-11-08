// playmaker-loadlock (fix, exact, opt-in): PlayMaker loads an FSM's actions (Fsm.InitData) on two
// threads at once. Unity calls PlayMakerFSM.OnAfterDeserialize on its loading thread while a scene or
// bundle loads in the background, and PlayMaker's runs Fsm.InitData there; the main thread runs the
// same InitData from PlayMakerFSM.Awake for the objects it creates or wakes meanwhile. Both use
// ActionData's shared lookups (static Dictionary<string, Type>, <Type, FieldInfo[]>, <Type, int>) and
// its two shared work lists, and FsmEvent's shared event list, with no lock. Two inserts at once can
// leave a dictionary's bucket chain in a loop: every later lookup of that key spins forever. Enter
// the Gungeon on a BattleXP G350 (RK3326, four Cortex-A35) hung that way at the Breach's load in 3 of
// 5 launches (UnityMain 100 % in Dictionary.TryGetValue under ActionData.GetActionTypeHashCode,
// RSS flat, no frame for minutes); faster CPUs make the overlap rarer, not impossible.
//
// The patch puts those methods under one lock (a Monitor on an object of the patch's own helper
// type, re-entrant): PlayMakerFSM.OnAfterDeserialize (the loading thread's way in), Fsm.InitData,
// ActionData.LoadActions, both ActionData.CreateAction, ActionData.GetActionType, GetFields and
// GetActionTypeHashCode, FsmEvent.GetFsmEvent (both) and FsmEvent.FindEvent. Each keeps its name,
// signature and attributes (callers, overrides and interface slots are untouched); its body moves
// to a private twin that the method calls between Monitor.Enter and Monitor.Exit (in a finally).
// What each call returns and changes is what it would do with the other thread's call before or
// after it, as on a machine where the two never overlap: only the waiting is new. Opt-in: a game
// whose PlayMaker never loads on Unity's loading thread gains nothing from it.
using Mono.Cecil;
using Mono.Cecil.Cil;
using Mono.Cecil.Rocks;

namespace AsmPatch;

sealed class PlayMakerLoadLock : Patch
{
    public override string Name => "playmaker-loadlock";
    public override Kind Kind => Kind.Fix;
    public override bool OptIn => true;
    public override string Summary =>
        "PlayMaker's FSM loading under one lock: Unity's loading thread (PlayMakerFSM.OnAfterDeserialize) and the main thread " +
        "no longer corrupt its shared lookups at once (a hang at loads on slow handhelds); the same results";

    const string HelperType = "<asmpatch>PlayMakerLock", LockField = "Lock", Suffix = "<asmpatch>locked";

    public override Result Run(Game game, bool apply)
    {
        var (asm, fsm) = PlayMakerLib.Find(game);
        if (asm == null) return Result.NotApplicable("no PlayMaker (HutongGames.PlayMaker.Fsm)");
        var m = asm.MainModule;
        var file = game.FileOf(asm);
        if (m.GetType("", HelperType) != null)
        {
            var done = new Result { Outcome = Outcome.AlreadyApplied };
            done.Details.Add($"{file}: PlayMaker's FSM loading is under the lock already");
            return done;
        }
        var pfsm = m.GetType("PlayMakerFSM");
        var after = pfsm?.Methods.FirstOrDefault(x => x.Name == "OnAfterDeserialize" && !x.IsStatic && !x.HasParameters && x.HasBody);
        var init = fsm.Methods.FirstOrDefault(x => x.Name == "InitData" && !x.IsStatic && !x.HasParameters && x.HasBody);
        if (after == null) return Result.NotApplicable("PlayMakerFSM has no OnAfterDeserialize: PlayMaker does not load on Unity's loading thread");
        if (init == null) return Result.NotApplicable("Fsm has no InitData");
        if (!after.Body.Instructions.Any(i => i.Operand is MethodReference r && r.Name == "InitData" &&
                                              r.DeclaringType.FullName == PlayMakerLib.FsmType))
            return Result.NotApplicable("PlayMakerFSM.OnAfterDeserialize does not run Fsm.InitData");
        var ad = m.GetType("HutongGames.PlayMaker.ActionData");
        var ev = m.GetType("HutongGames.PlayMaker.FsmEvent");
        if (ad == null || ev == null) return Result.NotApplicable("no HutongGames.PlayMaker.ActionData or FsmEvent");
        IEnumerable<MethodDefinition> Named(TypeDefinition t, string n, bool stat) =>
            t.Methods.Where(x => x.Name == n && x.IsStatic == stat && x.HasBody && !x.HasGenericParameters);
        var lookups = Named(ad, "GetActionType", true).Concat(Named(ad, "GetFields", true)).Concat(Named(ad, "GetActionTypeHashCode", true)).ToList();
        if (lookups.Count != 3) return Result.NotApplicable("ActionData's GetActionType, GetFields and GetActionTypeHashCode are not the ones this patch knows");
        var targets = new List<MethodDefinition> { after, init };
        targets.AddRange(Named(ad, "LoadActions", false));
        targets.AddRange(Named(ad, "CreateAction", false));
        targets.AddRange(lookups);
        targets.AddRange(Named(ev, "GetFsmEvent", true));
        targets.AddRange(Named(ev, "FindEvent", true));
        if (targets.Any(t => t.Name.EndsWith(Suffix) || t.DeclaringType.HasGenericParameters))
            return Result.Failed("a method to wrap is generic or already wrapped");
        var r = new Result { Outcome = Outcome.Applied };
        r.Details.Add($"{file}: {targets.Count} methods under one lock: " +
                      string.Join(", ", targets.Select(t => $"{t.DeclaringType.Name}.{t.Name}").Distinct()));
        if (!apply) return r;

        var (lk, enter, exit) = Helper(m);
        foreach (var t in targets) Wrap(t, lk, enter, exit);
        game.MarkChanged(asm);
        return r;
    }

    /// <summary>The helper type with its lock object (made by its static constructor) and the corlib's
    /// Monitor.Enter(object) and Monitor.Exit(object).</summary>
    static (FieldReference, MethodReference, MethodReference) Helper(ModuleDefinition m)
    {
        var ts = m.TypeSystem;
        var h = new TypeDefinition("", HelperType, TypeAttributes.NotPublic | TypeAttributes.Abstract | TypeAttributes.Sealed |
                                                   TypeAttributes.Class, ts.Object);
        m.Types.Add(h);
        var f = new FieldDefinition(LockField, FieldAttributes.Assembly | FieldAttributes.Static | FieldAttributes.InitOnly, ts.Object);
        h.Fields.Add(f);
        var cctor = new MethodDefinition(".cctor", MethodAttributes.Private | MethodAttributes.HideBySig | MethodAttributes.Static |
                                                   MethodAttributes.SpecialName | MethodAttributes.RTSpecialName, ts.Void);
        h.Methods.Add(cctor);
        var il = cctor.Body.GetILProcessor();
        il.Emit(OpCodes.Newobj, new MethodReference(".ctor", ts.Void, ts.Object) { HasThis = true });
        il.Emit(OpCodes.Stsfld, f);
        il.Emit(OpCodes.Ret);
        // Monitor: in mscorlib (Unity's players) or netstandard; a .NET (Core) build's System.Runtime
        // leaves it to System.Threading (the tests' stand-in)
        var scope = ts.CoreLibrary;
        if (scope is AssemblyNameReference core && core.Name == "System.Runtime")
        {
            var st = m.AssemblyReferences.FirstOrDefault(a => a.Name == "System.Threading");
            if (st == null)
            {
                st = new AssemblyNameReference("System.Threading", core.Version) { PublicKeyToken = core.PublicKeyToken };
                m.AssemblyReferences.Add(st);
            }
            scope = st;
        }
        var mon = new TypeReference("System.Threading", "Monitor", m, scope);
        MethodReference M(string n)
        {
            var x = new MethodReference(n, ts.Void, mon);
            x.Parameters.Add(new ParameterDefinition(ts.Object));
            return x;
        }
        return (f, M("Enter"), M("Exit"));
    }

    /// <summary>t's body moves to a private twin (t's name + Suffix, the same parameters); t calls it
    /// with the lock held: Monitor.Enter(lock); try { [r =] twin(args); } finally { Monitor.Exit(lock); } [return r;]</summary>
    static void Wrap(MethodDefinition t, FieldReference lk, MethodReference enter, MethodReference exit)
    {
        var twin = new MethodDefinition(t.Name + Suffix, MethodAttributes.Private | MethodAttributes.HideBySig |
                                                         (t.IsStatic ? MethodAttributes.Static : 0), t.ReturnType);
        foreach (var p in t.Parameters) twin.Parameters.Add(new ParameterDefinition(p.Name, ParameterAttributes.None, p.ParameterType));
        t.DeclaringType.Methods.Add(twin);
        MethodBody ob = t.Body, nb = twin.Body;
        nb.InitLocals = ob.InitLocals; nb.MaxStackSize = ob.MaxStackSize;
        var ins = ob.Instructions.ToList(); var vars = ob.Variables.ToList(); var ehs = ob.ExceptionHandlers.ToList();
        var self = t.IsStatic ? null : ob.ThisParameter;
        ob.ExceptionHandlers.Clear(); ob.Instructions.Clear(); ob.Variables.Clear();
        foreach (var v in vars) nb.Variables.Add(v);
        foreach (var i in ins)
        {
            if (i.Operand is ParameterDefinition p) i.Operand = p == self ? nb.ThisParameter : twin.Parameters[p.Index];
            nb.Instructions.Add(i);
        }
        foreach (var h in ehs) nb.ExceptionHandlers.Add(h);

        var ts = t.Module.TypeSystem;
        var il = ob.GetILProcessor();
        ob.InitLocals = true;
        var l = new VariableDefinition(ts.Object); ob.Variables.Add(l);
        var r = t.ReturnType.MetadataType == MetadataType.Void ? null : new VariableDefinition(t.ReturnType);
        if (r != null) ob.Variables.Add(r);
        il.Emit(OpCodes.Ldsfld, lk); il.Emit(OpCodes.Stloc, l);
        il.Emit(OpCodes.Ldloc, l); il.Emit(OpCodes.Call, enter);
        var body = new List<Instruction>();
        if (!t.IsStatic) body.Add(il.Create(OpCodes.Ldarg_0));
        foreach (var p in t.Parameters) body.Add(il.Create(OpCodes.Ldarg, p));
        body.Add(il.Create(OpCodes.Call, twin));
        if (r != null) body.Add(il.Create(OpCodes.Stloc, r));
        var end = r != null ? il.Create(OpCodes.Ldloc, r) : il.Create(OpCodes.Ret);
        body.Add(il.Create(OpCodes.Leave, end));
        foreach (var i in body) il.Append(i);
        var fin = il.Create(OpCodes.Ldloc, l);
        il.Append(fin); il.Emit(OpCodes.Call, exit); il.Emit(OpCodes.Endfinally);
        il.Append(end);
        if (r != null) il.Emit(OpCodes.Ret);
        ob.ExceptionHandlers.Add(new ExceptionHandler(ExceptionHandlerType.Finally)
        { TryStart = body[0], TryEnd = fin, HandlerStart = fin, HandlerEnd = end });
        ob.OptimizeMacros();
    }
}
