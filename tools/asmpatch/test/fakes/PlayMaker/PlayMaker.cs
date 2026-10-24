// A stand-in for PlayMaker.dll (written for these tests; no PlayMaker code): the members the
// playmaker-* patches look at, shaped as C# compilers emit PlayMaker's (the state array behind
// Fsm.States, FsmState's loop count and its reset loop, the delayed events and their two work
// lists), and Driver.Run, which drives three FSMs through a seeded random sequence (state changes,
// states of other FSMs, states put into other arrays, new arrays, copies, delayed events that add,
// clear or change state while they are updated, a missing list) and returns everything the game
// could see after each step.
using System;
using System.Collections.Generic;
using System.Text;

namespace HutongGames.PlayMaker
{
    public class FsmState
    {
        private Fsm fsm;
        public readonly string Name;

        public int loopCount { get; private set; }
        public int maxLoopCount { get; private set; }

        public FsmState(string name) { Name = name; Driver.All.Add(this); }
        public FsmState(FsmState source) : this(source.Name + "'") { }

        public Fsm Fsm { get { return fsm; } set { fsm = value; } }

        public void OnEnter()
        {
            loopCount++;
            if (loopCount > maxLoopCount) maxLoopCount = loopCount;
            Driver.Log.Append(Name).Append('+');
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
        public readonly string Name;
        public int MaxLoopCount = 4;
        public bool Disabled;
        public FsmState ActiveState;

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

        public void UpdateStateChanges()
        {
            while (switchToState != null) SwitchState(switchToState);
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
            Random rnd = new Random(seed);
            List<Fsm> fsms = new List<Fsm> { new Fsm("a", 3), new Fsm("b", 5), new Fsm("c", 1) };
            string[] names = { "e", "add", "clear", "go", "goo" };
            StringBuilder sb = new StringBuilder();
            for (int step = 0; step < steps; step++)
            {
                Fsm f = fsms[rnd.Next(fsms.Count)], g = fsms[rnd.Next(fsms.Count)];
                int op = rnd.Next(13);
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
                    }
                }
                catch (Exception e) { sb.Append('<').Append(e.GetType().Name).Append('>'); }
                foreach (FsmState s in All) sb.Append(s.loopCount).Append(',');
                foreach (Fsm x in fsms)
                {
                    sb.Append(x.Disabled ? 'D' : '-');
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
