// playmaker-loopcount and playmaker-delayedevents on a stand-in PlayMaker.dll (test/fakes/PlayMaker,
// built by build.sh test): patched, then the original and the patched copy are driven through the
// same seeded sequences (fakes/PlayMaker/PlayMaker.cs, Driver.Run) and must show the same loop
// counts, guard trips, delayed events, fired events and exceptions after every step; the patched
// copy must skip the work it is meant to skip. Game assemblies made with Cecil that write into
// PlayMaker's state arrays, pass them on or wrap the getter leave it unpatched, as do a PlayMaker
// whose reset writes something else and one whose work list is read elsewhere.
using System.Runtime.Loader;
using Mono.Cecil;
using Mono.Cecil.Cil;
using Assembly = System.Reflection.Assembly;
using BindingFlags = System.Reflection.BindingFlags;

namespace AsmPatch.Tests;

static partial class TestMain
{
    static readonly string FakePlayMaker = Path.Combine(AppContext.BaseDirectory, "fakes", "PlayMaker.dll");

    /// <summary>Loads PlayMaker.dll from dir in a context of its own and runs f on it.</summary>
    static T WithPlayMaker<T>(string dir, Func<Assembly, T> f)
    {
        var ctx = new AssemblyLoadContext("pm" + Guid.NewGuid(), isCollectible: true);
        try { return f(ctx.LoadFromStream(new MemoryStream(File.ReadAllBytes(Path.Combine(dir, "PlayMaker.dll"))))); }
        finally { ctx.Unload(); }
    }

    static string Drive(string dir, int seed, int steps) => WithPlayMaker(dir, a =>
        (string)a.GetType("HutongGames.PlayMaker.Driver", true).GetMethod("Run").Invoke(null, new object[] { seed, steps }));

    static void PlayMakerTests()
    {
        if (!File.Exists(FakePlayMaker)) { Check(false, $"PlayMaker stand-in at {FakePlayMaker} (build.sh test builds it)"); return; }
        var d = Dir("pm");
        File.Copy(FakePlayMaker, Path.Combine(d, "PlayMaker.dll"));
        var o = Path.Combine(tmp, "pm-out");
        var (st, outp) = Patch(d, o);
        Check(st == 0 && outp.Contains("ok   playmaker-loopcount: PlayMaker.dll: HutongGames.PlayMaker.Fsm.UpdateStateChanges") &&
              outp.Contains("ok   playmaker-delayedevents: PlayMaker.dll: HutongGames.PlayMaker.Fsm.UpdateDelayedEvents") &&
              File.Exists(Path.Combine(o, "PlayMaker.dll")), "PlayMaker: both optimizations apply");
        if (!File.Exists(Path.Combine(o, "PlayMaker.dll"))) return;
        Check(!Refs(Path.Combine(o, "PlayMaker.dll")).Except(Refs(Path.Combine(d, "PlayMaker.dll"))).Any(), "PlayMaker: no new assembly references");

        // the same behaviour, step by step
        int same = 0, guards = 0, nulls = 0, fired = 0;
        string first = null;
        for (int seed = 1; seed <= 60; seed++)
        {
            string a = Drive(d, seed, 400), b = Drive(o, seed, 400);
            if (a == b) same++;
            else if (first == null)
            {
                var la = a.Split('\n'); var lb = b.Split('\n');
                int k = 0; while (k < la.Length && k < lb.Length && la[k] == lb[k]) k++;
                first = $"seed {seed}, step {k}: {(k < la.Length ? la[k] : "")} / {(k < lb.Length ? lb[k] : "")}";
            }
            guards += a.Split("[guard").Length - 1; nulls += a.Split("<ArgumentNullException>").Length - 1; fired += a.Split('!').Length - 1;
        }
        Check(same == 60, $"PlayMaker: 60 seeded runs of 400 steps, the patched copy sees what the original sees ({same} the same{(first != null ? "; first difference " + first : "")})");
        Check(guards > 0 && nulls > 0 && fired > 0, $"PlayMaker: the runs trip the loop guard ({guards}), update a missing list ({nulls}) and fire delayed events ({fired})");
        string Directed(string dir) => WithPlayMaker(dir, a => (string)a.GetType("HutongGames.PlayMaker.Driver", true).GetMethod("Directed").Invoke(null, null));
        string da = Directed(d), db = Directed(o);
        Check(da == db && da.Contains("2,"), $"PlayMaker: states counted through another FSM, then put into a reset FSM's array (an element, a new array), copied, shared: the same counts\n{da}---\n{db}");

        // what is skipped: the reset loop while nothing changed, the work lists while there is nothing to update
        WithPlayMaker(o, a =>
        {
            var fsmT = a.GetType("HutongGames.PlayMaker.Fsm", true);
            var changes = a.GetType(PlayMakerLib.HelperType, true).GetField(PlayMakerLib.ChangesField, BindingFlags.Static | BindingFlags.NonPublic);
            var mark = fsmT.GetField(PlayMakerLib.MarkField, BindingFlags.Instance | BindingFlags.NonPublic);
            var f = Activator.CreateInstance(fsmT, "t", 4);
            bool Skips() => (long)mark.GetValue(f) == (long)changes.GetValue(null) + 1;
            fsmT.GetMethod("UpdateStateChanges").Invoke(f, null);
            bool after = Skips();
            fsmT.GetMethod("UpdateStateChanges").Invoke(f, null);
            bool again = Skips();
            var s = ((Array)fsmT.GetProperty("States").GetValue(f)).GetValue(1);
            fsmT.GetMethod("SetState").Invoke(f, new[] { s });
            long c0 = (long)changes.GetValue(null);
            fsmT.GetMethod("SwitchState").Invoke(f, new[] { s });
            bool entered = (long)changes.GetValue(null) > c0 && !Skips();
            fsmT.GetMethod("UpdateStateChanges").Invoke(f, null);
            Check(after && again && entered && Skips() && (int)s.GetType().GetProperty("loopCount").GetValue(s) == 0,
                  "playmaker-loopcount: the loop is skipped after a reset with no change since, and runs after a state was entered");
            return 0;
        });
        int Version(string dir) => WithPlayMaker(dir, a =>
        {
            var fsmT = a.GetType("HutongGames.PlayMaker.Fsm", true);
            var f = Activator.CreateInstance(fsmT, "t", 2);
            var upd = fsmT.GetField("updateEvents", BindingFlags.Instance | BindingFlags.NonPublic).GetValue(f);
            var ver = upd.GetType().GetField("_version", BindingFlags.Instance | BindingFlags.NonPublic);
            int v0 = (int)ver.GetValue(upd);
            for (int k = 0; k < 3; k++) fsmT.GetMethod("UpdateDelayedEvents").Invoke(f, null);
            return (int)ver.GetValue(upd) - v0;
        });
        int vo = Version(d), vp = Version(o);
        Check(vo > 0 && vp == 0, $"playmaker-delayedevents: with nothing to update the work lists are not touched (version +{vo} unpatched, +{vp} patched)");

        // patched again: done, nothing written; --no-optimize leaves them out
        (st, outp) = Patch(o, Path.Combine(tmp, "pm-again"));
        Check(st == 0 && outp.Contains("done playmaker-loopcount") && outp.Contains("done playmaker-delayedevents") && NoDlls(Path.Combine(tmp, "pm-again")),
              "PlayMaker: a second run finds both applied and writes nothing");
        (st, outp) = Patch(d, Path.Combine(tmp, "pm-noopt"), noOptimize: true);
        Check(st == 0 && outp.Contains("skip playmaker-loopcount (--no-optimize)") && outp.Contains("skip playmaker-delayedevents (--no-optimize)") &&
              NoDlls(Path.Combine(tmp, "pm-noopt")), "--no-optimize: the optimizations are left out");
        (st, outp) = Patch(d, Path.Combine(tmp, "pm-named"), only: new[] { "playmaker-delayedevents" }, noOptimize: true);
        Check(st == 0 && outp.Contains("ok   playmaker-delayedevents") && outp.Contains("skip playmaker-loopcount"), "--no-optimize: a named optimization still runs");

        // game code that does what the patch cannot follow: not applied
        GameWith(d, "writes an element", (m, il, states) =>
        { il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Callvirt, states); il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Ldnull); il.Emit(OpCodes.Stelem_Ref); },
            "Assembly-CSharp.dll: Game.Poke writes PlayMaker's state data");
        GameWith(d, "passes the array on", (m, il, states) =>
        {
            var array = new TypeReference("System", "Array", m, m.TypeSystem.CoreLibrary);
            var reverse = new MethodReference("Reverse", m.TypeSystem.Void, array);
            reverse.Parameters.Add(new ParameterDefinition(array));
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Callvirt, states); il.Emit(OpCodes.Call, reverse);
        }, "Assembly-CSharp.dll: Game.Poke: passes a state array to System.Array.Reverse");
        GameWith(d, "keeps it in a field", (m, il, states) =>
        {
            var keep = new FieldDefinition("kept", FieldAttributes.Public | FieldAttributes.Static, states.ReturnType);
            m.Types.First(t => t.Name == "Game").Fields.Add(keep);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Callvirt, states); il.Emit(OpCodes.Stsfld, keep);
        }, "Assembly-CSharp.dll: Game.Poke: keeps a state array in kept");
        GameWith(d, "reads it (still applies)", (m, il, states) =>
        { il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Callvirt, states); il.Emit(OpCodes.Ldlen); il.Emit(OpCodes.Pop);
          il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Callvirt, states); il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Ldelem_Ref); il.Emit(OpCodes.Pop); },
            null);
        // a wrapper of the getter in the game, and a write through it
        {
            var g = Dir("pm-wrap");
            File.Copy(FakePlayMaker, Path.Combine(g, "PlayMaker.dll"));
            var m = NewModule("Assembly-CSharp");
            using var pm = AssemblyDefinition.ReadAssembly(FakePlayMaker);
            var fsm = pm.MainModule.GetType("HutongGames.PlayMaker.Fsm");
            var states = m.ImportReference(fsm.Methods.First(x => x.Name == "get_States"));
            var t = new TypeDefinition("", "Game", TypeAttributes.Public | TypeAttributes.Abstract | TypeAttributes.Sealed, m.TypeSystem.Object);
            m.Types.Add(t);
            var wrap = new MethodDefinition("StatesOf", MethodAttributes.Public | MethodAttributes.Static, states.ReturnType);
            wrap.Parameters.Add(new ParameterDefinition(m.ImportReference(fsm)));
            var il = wrap.Body.GetILProcessor();
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Callvirt, states); il.Emit(OpCodes.Ret);
            t.Methods.Add(wrap);
            var poke = new MethodDefinition("Poke", MethodAttributes.Public | MethodAttributes.Static, m.TypeSystem.Void);
            poke.Parameters.Add(new ParameterDefinition(m.ImportReference(fsm)));
            il = poke.Body.GetILProcessor();
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Call, wrap); il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Ldnull); il.Emit(OpCodes.Stelem_Ref); il.Emit(OpCodes.Ret);
            t.Methods.Add(poke);
            m.Write(Path.Combine(g, "Assembly-CSharp.dll"));
            (st, outp) = Patch(g, Path.Combine(tmp, "pm-wrap-out"));
            Check(st == 0 && outp.Contains("n/a  playmaker-loopcount: Assembly-CSharp.dll: Game.Poke writes PlayMaker's state data") &&
                  outp.Contains("ok   playmaker-delayedevents"), "playmaker-loopcount: a write through the game's own wrapper of Fsm.States: not applied");
        }

        // PlayMakers this patch does not know: not applied
        PlayMakerVariant("a reset that writes 1", fsm =>
        {
            var reset = fsm.Module.GetType("HutongGames.PlayMaker.FsmState").Methods.First(x => x.Name == "ResetLoopCount");
            reset.Body.Instructions.First(i => i.OpCode == OpCodes.Ldc_I4_0).OpCode = OpCodes.Ldc_I4_1;
        }, "n/a  playmaker-loopcount: PlayMaker.dll: FsmState.ResetLoopCount is not loopCount = 0", "ok   playmaker-delayedevents");
        PlayMakerVariant("a work list read elsewhere", fsm =>
        {
            var upd = fsm.Fields.First(x => x.Name == "updateEvents");
            var peek = new MethodDefinition("Peek", MethodAttributes.Public, fsm.Module.TypeSystem.Object);
            var il = peek.Body.GetILProcessor();
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, upd); il.Emit(OpCodes.Ret);
            fsm.Methods.Add(peek);
        }, "n/a  playmaker-delayedevents: PlayMaker.dll: HutongGames.PlayMaker.Fsm.Peek uses Fsm.updateEvents", "ok   playmaker-loopcount");
        PlayMakerVariant("the field's address taken", fsm =>
        {
            var states = fsm.Fields.First(x => x.Name == "states");
            var grab = new MethodDefinition("Grab", MethodAttributes.Public, fsm.Module.TypeSystem.Void);
            var il = grab.Body.GetILProcessor();
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldflda, states); il.Emit(OpCodes.Pop); il.Emit(OpCodes.Ret);
            fsm.Methods.Add(grab);
        }, "n/a  playmaker-loopcount: PlayMaker.dll: HutongGames.PlayMaker.Fsm.Grab: takes the address of Fsm.states", "ok   playmaker-delayedevents");

        // no PlayMaker
        var none = Dir("pm-none");
        var plain = NewModule("Assembly-CSharp");
        plain.Types.Add(new TypeDefinition("", "Game", TypeAttributes.Public, plain.TypeSystem.Object));
        plain.Write(Path.Combine(none, "Assembly-CSharp.dll"));
        (st, outp) = Patch(none, Path.Combine(tmp, "pm-none-out"));
        Check(st == 0 && outp.Contains("n/a  playmaker-loopcount: no PlayMaker") && outp.Contains("n/a  playmaker-delayedevents: no PlayMaker"),
              "no PlayMaker: not applicable");
    }

    /// <summary>The stand-in PlayMaker next to a Cecil-made Assembly-CSharp whose Game.Poke(Fsm)
    /// does what body emits (with Fsm.get_States imported); expect: playmaker-loopcount's n/a
    /// reason, or null when it must still apply.</summary>
    static void GameWith(string pmDir, string what, Action<ModuleDefinition, ILProcessor, MethodReference> body, string expect)
    {
        var g = Dir("pm-game-" + Guid.NewGuid().ToString("n")[..8]);
        File.Copy(Path.Combine(pmDir, "PlayMaker.dll"), Path.Combine(g, "PlayMaker.dll"));
        var m = NewModule("Assembly-CSharp");
        using (var pm = AssemblyDefinition.ReadAssembly(FakePlayMaker))
        {
            var fsm = pm.MainModule.GetType("HutongGames.PlayMaker.Fsm");
            var states = m.ImportReference(fsm.Methods.First(x => x.Name == "get_States"));
            var t = new TypeDefinition("", "Game", TypeAttributes.Public | TypeAttributes.Abstract | TypeAttributes.Sealed, m.TypeSystem.Object);
            m.Types.Add(t);
            var poke = new MethodDefinition("Poke", MethodAttributes.Public | MethodAttributes.Static, m.TypeSystem.Void);
            poke.Parameters.Add(new ParameterDefinition(m.ImportReference(fsm)));
            t.Methods.Add(poke);
            var il = poke.Body.GetILProcessor();
            body(m, il, states);
            il.Emit(OpCodes.Ret);
            m.Write(Path.Combine(g, "Assembly-CSharp.dll"));
        }
        var (st, outp) = Patch(g, Path.Combine(tmp, Path.GetFileName(g) + "-out"));
        if (expect == null)
            Check(st == 0 && outp.Contains("ok   playmaker-loopcount"), $"playmaker-loopcount: game code that {what}");
        else
            Check(st == 0 && outp.Contains("n/a  playmaker-loopcount") && outp.Contains(expect), $"playmaker-loopcount: game code that {what}: not applied");
    }

    static void PlayMakerVariant(string what, Action<TypeDefinition> change, string expect, string other)
    {
        var g = Dir("pm-var-" + Guid.NewGuid().ToString("n")[..8]);
        using (var pm = AssemblyDefinition.ReadAssembly(FakePlayMaker))
        {
            change(pm.MainModule.GetType("HutongGames.PlayMaker.Fsm"));
            pm.Write(Path.Combine(g, "PlayMaker.dll"));
        }
        var (st, outp) = Patch(g, Path.Combine(tmp, Path.GetFileName(g) + "-out"));
        Check(st == 0 && outp.Contains(expect) && outp.Contains(other), $"PlayMaker with {what}: {expect.Split(':')[0]}, the other one applies");
    }
}
