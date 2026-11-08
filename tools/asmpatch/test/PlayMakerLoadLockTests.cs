// playmaker-loadlock on the stand-in PlayMaker.dll (test/fakes/PlayMaker): opt-in; the patched copy loads
// FSMs with the same results as the original; its wrapped methods wait while another thread holds the
// lock; two threads loading FSMs at once (one through PlayMakerFSM.OnAfterDeserialize, as Unity's
// loading thread, one through Fsm.InitData) never see the shared work list change under them; a second
// run finds it applied.
using System.Threading;
using Assembly = System.Reflection.Assembly;
using BindingFlags = System.Reflection.BindingFlags;

namespace AsmPatch.Tests;

static partial class TestMain
{
    /// <summary>Loads two FSMs (one as Unity's loading thread does) and returns what the game could see of it.</summary>
    static string LoadOnce(Assembly a)
    {
        var fsmT = a.GetType("HutongGames.PlayMaker.Fsm", true);
        var pmT = a.GetType("PlayMakerFSM", true);
        var adT = a.GetType("HutongGames.PlayMaker.ActionData", true);
        var evT = a.GetType("HutongGames.PlayMaker.FsmEvent", true);
        var f1 = Activator.CreateInstance(fsmT, "x1", 3); var f2 = Activator.CreateInstance(fsmT, "y2", 2);
        var pm = Activator.CreateInstance(pmT);
        pmT.GetField("fsm").SetValue(pm, f1);
        pmT.GetMethod("OnAfterDeserialize").Invoke(pm, null);
        fsmT.GetMethod("InitData").Invoke(f2, null);
        var sb = new System.Text.StringBuilder();
        foreach (var f in new[] { f1, f2 })
            foreach (object[] st in (object[][])fsmT.GetField("Loaded").GetValue(f))
                sb.Append(string.Join(",", st.Select(x => x.GetType().Name))).Append(';');
        var hash = adT.GetMethod("GetActionTypeHashCode", BindingFlags.Static | BindingFlags.NonPublic);
        foreach (var n in new[] { "LoadA", "LoadB", "LoadC" })
            sb.Append(n).Append('=').Append(hash.Invoke(null, new object[] { a.GetType("HutongGames.PlayMaker." + n, true) })).Append(' ');
        sb.Append(adT.GetMethod("GetActionType").Invoke(null, new object[] { "Missing" }) == null ? "missing " : "found ");
        sb.Append("events ").Append(evT.GetProperty("Count").GetValue(null)).Append(' ');
        sb.Append(evT.GetMethod("FindEvent").Invoke(null, new object[] { "y:start" }) != null);
        return sb.ToString();
    }

    /// <summary>Two threads load FSMs at once for rounds rounds (the shared lookups emptied before each): how many
    /// loads failed (an exception, or one not finished within 20 s).</summary>
    static int LoadRace(Assembly a, int rounds)
    {
        var fsmT = a.GetType("HutongGames.PlayMaker.Fsm", true);
        var pmT = a.GetType("PlayMakerFSM", true);
        var adT = a.GetType("HutongGames.PlayMaker.ActionData", true);
        var lookups = new[] { "ActionFieldsLookup", "ActionHashCodeLookup" }.Select(n => adT.GetField(n).GetValue(null))
            .Append(adT.GetField("ActionTypeLookup", BindingFlags.Static | BindingFlags.NonPublic).GetValue(null)).ToList();
        int failed = 0;
        for (int r = 0; r < rounds; r++)
        {
            foreach (var d in lookups) d.GetType().GetMethod("Clear").Invoke(d, null);
            using var go = new ManualResetEventSlim(false);
            // (made here: the stand-in's FsmState constructor keeps a list of its own for the other tests)
            var made = new[] { true, false }.ToDictionary(l => l, l => Enumerable.Range(0, 20).Select(k => Activator.CreateInstance(fsmT, (l ? "l" : "m") + k, 4)).ToList());
            void Run(bool loader)
            {
                go.Wait();
                try
                {
                    foreach (var f in made[loader])
                    {
                        if (loader)
                        {
                            var pm = Activator.CreateInstance(pmT);
                            pmT.GetField("fsm").SetValue(pm, f);
                            pmT.GetMethod("OnAfterDeserialize").Invoke(pm, null);
                        }
                        else fsmT.GetMethod("InitData").Invoke(f, null);
                    }
                }
                catch (Exception) { Interlocked.Increment(ref failed); }
            }
            var t1 = new Thread(() => Run(true)); var t2 = new Thread(() => Run(false));
            t1.IsBackground = t2.IsBackground = true;
            t1.Start(); t2.Start(); go.Set();
            if (!t1.Join(20000)) failed++;
            if (!t2.Join(20000)) failed++;
        }
        return failed;
    }

    static void PlayMakerLoadLockTests()
    {
        if (!File.Exists(FakePlayMaker)) { Check(false, $"PlayMaker stand-in at {FakePlayMaker} (build.sh test builds it)"); return; }
        var d = Dir("pmll");
        File.Copy(FakePlayMaker, Path.Combine(d, "PlayMaker.dll"));
        var (st, outp) = Patch(d, Path.Combine(tmp, "pmll-default"));
        Check(st == 0 && outp.Contains("skip playmaker-loadlock (opt-in"), "playmaker-loadlock: opt-in");
        var o = Path.Combine(tmp, "pmll-out");
        (st, outp) = Patch(d, o, only: new[] { "playmaker-loadlock" });
        Check(st == 0 && outp.Contains("ok   playmaker-loadlock: PlayMaker.dll: 10 methods under one lock: PlayMakerFSM.OnAfterDeserialize, Fsm.InitData, " +
                                       "ActionData.LoadActions, ActionData.CreateAction, ActionData.GetActionType, ActionData.GetFields, " +
                                       "ActionData.GetActionTypeHashCode, FsmEvent.GetFsmEvent, FsmEvent.FindEvent") &&
              File.Exists(Path.Combine(o, "PlayMaker.dll")), $"playmaker-loadlock: applies to the stand-in\n{outp}");
        if (!File.Exists(Path.Combine(o, "PlayMaker.dll"))) return;

        // the same results, one thread
        string a = WithPlayMaker(d, LoadOnce), b = WithPlayMaker(o, LoadOnce);
        Check(a == b && a.Contains("LoadA,LoadB,LoadC;") && a.Contains("missing"), $"playmaker-loadlock: the same FSM loads\n{a}\n{b}");

        // the wrapped methods wait for the lock
        bool waited = WithPlayMaker(o, asm =>
        {
            var lk = asm.GetType("<asmpatch>PlayMakerLock", true).GetField("Lock", BindingFlags.Static | BindingFlags.NonPublic).GetValue(null);
            var adT = asm.GetType("HutongGames.PlayMaker.ActionData", true);
            var fsmT = asm.GetType("HutongGames.PlayMaker.Fsm", true);
            bool ok = true;
            foreach (Action call in new Action[]
            {
                () => adT.GetMethod("GetFields").Invoke(null, new object[] { asm.GetType("HutongGames.PlayMaker.LoadB", true) }),
                () => fsmT.GetMethod("InitData").Invoke(Activator.CreateInstance(fsmT, "w", 2), null),
            })
            {
                Monitor.Enter(lk);
                var t = new Thread(() => call()) { IsBackground = true };
                t.Start();
                bool blocked = !t.Join(300);
                Monitor.Exit(lk);
                ok &= blocked && t.Join(5000);
            }
            return ok;
        });
        Check(waited, "playmaker-loadlock: Fsm.InitData and ActionData.GetFields wait while another thread holds the lock");

        // two threads loading at once
        int bad = WithPlayMaker(o, asm => LoadRace(asm, 200));
        int orig = WithPlayMaker(d, asm => LoadRace(asm, 200));
        Check(bad == 0, $"playmaker-loadlock: 200 rounds of two threads loading 20 FSMs each, none failed ({bad}; the original: {orig})");

        // patched again: done, nothing written
        (st, outp) = Patch(o, Path.Combine(tmp, "pmll-again"), only: new[] { "playmaker-loadlock" });
        Check(st == 0 && outp.Contains("done playmaker-loadlock") && NoDlls(Path.Combine(tmp, "pmll-again")),
              "playmaker-loadlock: a second run finds it applied and writes nothing");
    }
}
