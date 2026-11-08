// A stand-in for PlayMaker.dll (written for these tests; no PlayMaker code): the members the
// playmaker-* patches look at, shaped as C# compilers emit PlayMaker's (the state array behind
// Fsm.States, FsmState's loop count and its reset loop, the delayed events and their two work
// lists, Fsm.Update with its owner check and execution stack, FsmState.OnUpdate and its finished
// flag), and Driver.Run, which drives three FSMs through a seeded random sequence (state changes,
// states of other FSMs, states put into other arrays, new arrays, copies, delayed events that add,
// clear or change state while they are updated, a missing list, updates of FSMs whose states work,
// finish and wait, owners destroyed, event targets, breakpoints) and returns everything the game
// could see after each step.
using System;
using System.Collections.Generic;
using System.Text;

namespace UnityEngine
{
    // (Unity's: a destroyed object compares equal to null)
    public class Object
    {
        public bool Dead;
        public static bool operator ==(Object a, Object b) =>
            ReferenceEquals(a, b) || (ReferenceEquals(a, null) ? b.Dead : ReferenceEquals(b, null) && a.Dead);
        public static bool operator !=(Object a, Object b) => !(a == b);
        public override bool Equals(object o) => base.Equals(o);
        public override int GetHashCode() => base.GetHashCode();
    }
    public class MonoBehaviour : Object { }
    public static class Debug
    {
        public static void LogError(object message) { HutongGames.PlayMaker.Driver.Log.Append("[error ").Append(message).Append(']'); }
    }
}

namespace HutongGames.PlayMaker
{
    public static class FsmTime
    {
        public static bool FirstUpdate;
        public static void RealtimeBugFix() { FirstUpdate = true; }
    }

    public static class FsmExecutionStack
    {
        private static readonly Stack<Fsm> stack = new Stack<Fsm>(256);
        public static int Pushes;                       // (how much work was done: not what the game sees)
        public static int StackCount => stack.Count;
        public static int MaxStackCount { get; private set; }
        public static void PushFsm(Fsm fsm)
        {
            Pushes++;
            stack.Push(fsm);
            if (stack.Count > MaxStackCount) MaxStackCount = stack.Count;
        }
        public static void PopFsm() { stack.Pop(); }
        public static void Reset() { stack.Clear(); MaxStackCount = 0; Pushes = 0; }
    }

    public class FsmState
    {
        private Fsm fsm;
        private bool finished;
        public readonly string Name;
        public int Work, StateTime;
        public bool Done => finished;

        public int loopCount { get; private set; }
        public int maxLoopCount { get; private set; }

        public FsmState(string name) { Name = name; Driver.All.Add(this); }
        public FsmState(FsmState source) : this(source.Name + "'") { }

        public Fsm Fsm { get { return fsm; } set { fsm = value; } }

        public void OnEnter()
        {
            loopCount++;
            if (loopCount > maxLoopCount) maxLoopCount = loopCount;
            finished = false;
            Work = Name.Length % 3;
            Driver.Log.Append(Name).Append('+');
        }

        public void OnUpdate()
        {
            if (finished) return;
            StateTime++;
            if (Work > 0) Work--;
            CheckAllActionsFinished();
        }

        private void CheckAllActionsFinished()
        {
            if (!finished && Work == 0)
            {
                finished = true;
                Driver.Log.Append(Name).Append('.');
                fsm.Finished(this);
            }
        }

        public void ResetLoopCount()
        {
            loopCount = 0;
        }
    }

    public class DelayedEvent
    {
        private readonly Fsm fsm;
        public readonly string Name;
        private int frames;

        public DelayedEvent(Fsm fsm, string name, int frames) { this.fsm = fsm; Name = name; this.frames = frames; }

        public bool Finished { get; private set; }
        public int Frames { get { return frames; } }

        public void Update()
        {
            if (Finished) return;
            if (--frames > 0) return;
            Finished = true;
            fsm.Fire(Name);
        }
    }

    public class Fsm
    {
        private FsmState[] states = new FsmState[1];
        private FsmState switchToState;
        private List<DelayedEvent> delayedEvents = new List<DelayedEvent>();
        private readonly List<DelayedEvent> updateEvents = new List<DelayedEvent>();
        private readonly List<DelayedEvent> removeEvents = new List<DelayedEvent>();
        private UnityEngine.MonoBehaviour owner = new UnityEngine.MonoBehaviour();
        private bool activeStateEntered;
        private FsmState activeState;
        public readonly string Name;
        public int MaxLoopCount = 4;
        public bool Disabled;
        public FsmState ActiveState { get { return activeState; } private set { activeState = value; } }
        public bool Entered => activeStateEntered;
        public static bool HitBreakpoint { get; set; }
        public bool IsSwitchingState => switchToState != null;
        public void KillOwner() { owner.Dead = true; }
        public void NewOwner() { owner = new UnityEngine.MonoBehaviour(); }

        public Fsm(string name, int n)
        {
            Name = name;
            states = new FsmState[n];
            for (int i = 0; i < n; i++) states[i] = new FsmState(name + i) { Fsm = this };
        }

        public Fsm(Fsm source)
        {
            Name = source.Name + "'";
            states = new FsmState[source.States.Length];
            for (int i = 0; i < source.States.Length; i++) states[i] = new FsmState(source.States[i]);
        }

        public FsmState[] States { get { return states; } set { states = value; } }
        public object EventTarget { get; set; }
        public List<DelayedEvent> DelayedEvents { get { return delayedEvents; } }

        public void SetState(FsmState state) { switchToState = state; }

        public void SwitchState(FsmState state)
        {
            switchToState = null;
            if (state != null) EnterState(state);
        }

        private void EnterState(FsmState state)
        {
            if (state.loopCount >= MaxLoopCount)
            {
                Disabled = true;
                Driver.Log.Append("[guard ").Append(Name).Append(']');
                return;
            }
            ActiveState = state;
            state.Fsm = this;
            state.OnEnter();
        }

        public void Continue()
        {
            activeStateEntered = true;
            Driver.Log.Append(Name).Append('>');
        }

        public void Update()
        {
            FsmTime.RealtimeBugFix();
            if (owner == null) return;
            int stackCount = FsmExecutionStack.StackCount;
            if (HitBreakpoint) return;
            FsmExecutionStack.PushFsm(this);
            if (!activeStateEntered) Continue();
            UpdateDelayedEvents();
            if (ActiveState != null) UpdateState(ActiveState);
            FsmExecutionStack.PopFsm();
            int stackCount2 = FsmExecutionStack.StackCount;
            // (as PlayMaker's compiler emits "Stack error: " + n: the int boxed, Concat(object, object))
            if (stackCount2 != stackCount) UnityEngine.Debug.LogError(string.Concat("Stack error: ", (object)(stackCount2 - stackCount)));
        }

        private void UpdateState(FsmState state)
        {
            state.Fsm = this;
            state.OnUpdate();
            UpdateStateChanges();
        }

        // what FINISHED does: half the states go on to another state
        internal void Finished(FsmState state)
        {
            if (state.Name.Length % 2 == 0) SetState(states[(state.Name.Length + 1) % states.Length]);
        }

        public void UpdateStateChanges()
        {
            while (IsSwitchingState && !HitBreakpoint) SwitchState(switchToState);
            for (int i = 0; i < States.Length; i++) States[i].ResetLoopCount();
            EventTarget = null;
        }

        public void UpdateDelayedEvents()
        {
            removeEvents.Clear();
            updateEvents.Clear();
            updateEvents.AddRange(delayedEvents);
            for (int i = 0; i < updateEvents.Count; i++)
            {
                DelayedEvent e = updateEvents[i];
                e.Update();
                if (e.Finished) removeEvents.Add(e);
            }
            for (int j = 0; j < removeEvents.Count; j++)
            {
                DelayedEvent e = removeEvents[j];
                delayedEvents.Remove(e);
            }
        }

        public DelayedEvent DelayedEvent(string name, int frames)
        {
            DelayedEvent e = new DelayedEvent(this, name, frames);
            delayedEvents.Add(e);
            return e;
        }

        public void ClearDelayedEvents() { delayedEvents.Clear(); }
        public void DropDelayedEvents() { delayedEvents = null; }
        public void ReplaceState(int i, FsmState state) { states[i] = state; }

        // what a delayed event does when it fires: more delayed events, none, a state change
        internal void Fire(string name)
        {
            Driver.Log.Append('!').Append(name);
            if (name.StartsWith("add")) DelayedEvent("x" + name, 1);
            else if (name.StartsWith("clear")) ClearDelayedEvents();
            else if (name.StartsWith("go"))
            {
                SetState(states[name.Length % states.Length]);
                UpdateStateChanges();
            }
        }
    }

    public static class Driver
    {
        public static readonly List<FsmState> All = new List<FsmState>();
        public static readonly StringBuilder Log = new StringBuilder();

        static string Counts()
        {
            StringBuilder sb = new StringBuilder();
            foreach (FsmState s in All) sb.Append(s.loopCount).Append(',');
            return sb.Append(Log).Append('\n').ToString();
        }

        // the cases a skipped reset could get wrong, one by one: a state entered through another
        // FSM and then put into a reset FSM's array (an element store, a new array, a copy), a state
        // in two arrays
        public static string Directed()
        {
            All.Clear();
            Log.Clear();
            StringBuilder sb = new StringBuilder();
            Fsm a = new Fsm("a", 3), b = new Fsm("b", 3);
            a.UpdateStateChanges(); b.UpdateStateChanges(); sb.Append(Counts());
            a.SwitchState(b.States[1]); a.SwitchState(b.States[1]);       // b1 counted twice, nobody reset it
            a.UpdateStateChanges(); sb.Append(Counts());                  // a resets its own states
            a.ReplaceState(0, b.States[1]);                               // b1 into a's reset array
            a.UpdateStateChanges(); sb.Append(Counts());                  // must reset b1
            a.SwitchState(b.States[2]); a.UpdateStateChanges(); b.SwitchState(b.States[0]); a.UpdateStateChanges();
            a.States = new FsmState[] { b.States[0], a.States[1] };      // b0 (counted) in a new array
            a.UpdateStateChanges(); sb.Append(Counts());
            b.SwitchState(b.States[2]); b.SwitchState(b.States[2]); a.UpdateStateChanges();
            Fsm c = new Fsm(b);                                           // a copy: new states
            c.UpdateStateChanges(); c.UpdateStateChanges(); sb.Append(Counts());
            b.UpdateStateChanges(); sb.Append(Counts());
            a.SwitchState(a.States[0]); b.UpdateStateChanges(); a.UpdateStateChanges(); a.UpdateStateChanges(); sb.Append(Counts());
            return sb.ToString();
        }

        public static string Run(int seed, int steps)
        {
            All.Clear();
            Log.Clear();
            FsmExecutionStack.Reset();
            Fsm.HitBreakpoint = false;
            Random rnd = new Random(seed);
            List<Fsm> fsms = new List<Fsm> { new Fsm("a", 3), new Fsm("b", 5), new Fsm("c", 1) };
            string[] names = { "e", "add", "clear", "go", "goo" };
            StringBuilder sb = new StringBuilder();
            for (int step = 0; step < steps; step++)
            {
                Fsm f = fsms[rnd.Next(fsms.Count)], g = fsms[rnd.Next(fsms.Count)];
                int op = rnd.Next(30);
                try
                {
                    switch (op)
                    {
                        case 0: case 1: f.SetState(f.States[rnd.Next(f.States.Length)]); f.UpdateStateChanges(); break;
                        case 2: f.UpdateStateChanges(); break;
                        case 3: f.SwitchState(g.States[rnd.Next(g.States.Length)]); break;
                        case 4: f.ReplaceState(rnd.Next(f.States.Length), g.States[rnd.Next(g.States.Length)]); break;
                        case 5: f.States = new FsmState[] { g.States[0], f.States[f.States.Length - 1] }; break;
                        case 6: if (fsms.Count < 6) fsms.Add(new Fsm(f)); break;
                        case 7: f.States[rnd.Next(f.States.Length)].ResetLoopCount(); break;
                        case 8: f.DelayedEvent(names[rnd.Next(names.Length)] + step, rnd.Next(3)); break;
                        case 9: case 10: f.UpdateDelayedEvents(); break;
                        case 11: f.SwitchState(f.States[0]); f.SwitchState(f.States[0]); break;
                        case 12: if (rnd.Next(25) == 0) f.DropDelayedEvents(); else f.UpdateDelayedEvents(); break;
                        case 13: case 14: case 15: case 16: f.Update(); break;
                        case 17: foreach (Fsm x in fsms) x.Update(); break;
                        case 18: f.EventTarget = rnd.Next(2) == 0 ? (object)("t" + step) : null; break;
                        case 19: if (rnd.Next(10) == 0) f.KillOwner(); else if (rnd.Next(4) == 0) f.NewOwner(); else f.Update(); break;
                        case 20: Fsm.HitBreakpoint = rnd.Next(8) == 0; break;
                        default: foreach (Fsm x in fsms) x.Update(); break;      // (a game's frames: most FSMs idle)
                    }
                }
                catch (Exception e) { sb.Append('<').Append(e.GetType().Name).Append('>'); }
                foreach (FsmState s in All) sb.Append(s.loopCount).Append(s.Done ? '.' : ':').Append(s.Work).Append('/').Append(s.StateTime).Append(',');
                // (not FsmExecutionStack.MaxStackCount, a debug statistic: with the stack left deeper by an exception
                //  inside an update, a skipped idle update can leave its high-water mark one lower)
                sb.Append(FsmTime.FirstUpdate ? 'T' : 'f').Append(FsmExecutionStack.StackCount).Append(' ');
                foreach (Fsm x in fsms)
                {
                    sb.Append(x.Disabled ? 'D' : '-').Append(x.Entered ? 'E' : 'e').Append(x.ActiveState != null ? x.ActiveState.Name : "~")
                      .Append(x.EventTarget != null ? "@" + x.EventTarget : "").Append(' ');
                    if (x.DelayedEvents != null)
                        foreach (DelayedEvent d in x.DelayedEvents) sb.Append(d.Name).Append(':').Append(d.Frames).Append(' ');
                    sb.Append('|');
                }
                sb.Append(Log).Append('\n');
                Log.Clear();
            }
            return sb.ToString();
        }
    }
}
