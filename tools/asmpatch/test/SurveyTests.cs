// The survey's pointer verdicts, on fake games made with Cecil (references to Unity's types only, no
// Unity assembly): a game with its own cursor (it locks the system pointer and hit-tests its UI
// itself at the cursor it draws, beside Rewired's UI input module, demos, an input module of its own
// and a hit test at the mouse's position, none of which counts), a game that locks the pointer for
// mouse look and hit-tests at the mouse's position, and a pointer-driven game whose only UI
// selection calls are Rewired's.
using Mono.Cecil;
using Mono.Cecil.Cil;

namespace AsmPatch.Tests;

static partial class TestMain
{
    sealed class FakeGame
    {
        public readonly ModuleDefinition M = NewModule("Assembly-CSharp");
        readonly AssemblyNameReference core, ui;

        public FakeGame()
        {
            core = new AssemblyNameReference("UnityEngine.CoreModule", new Version(0, 0, 0, 0));
            ui = new AssemblyNameReference("UnityEngine.UI", new Version(1, 0, 0, 0));
            M.AssemblyReferences.Add(core);
            M.AssemblyReferences.Add(ui);
        }

        public TypeReference Unity(string ns, string name, bool inUi = false, bool valueType = false) =>
            new(ns, name, M, inUi ? ui : core, valueType);

        public TypeDefinition Class(string ns, string name, TypeReference baseType = null)
        {
            var t = new TypeDefinition(ns, name, TypeAttributes.Public | TypeAttributes.BeforeFieldInit, baseType ?? M.TypeSystem.Object);
            M.Types.Add(t);
            return t;
        }

        /// <summary>A method of t whose body calls each of `calls` (with a constant before the
        /// ones that take an argument: arg).</summary>
        public void Method(TypeDefinition t, string name, params (MethodReference m, int? arg)[] calls)
        {
            var md = new MethodDefinition(name, MethodAttributes.Public | MethodAttributes.HideBySig, M.TypeSystem.Void);
            var il = md.Body.GetILProcessor();
            foreach (var (m, arg) in calls)
            {
                if (m.HasThis) il.Emit(OpCodes.Ldnull);
                foreach (var p in m.Parameters)
                    if (arg is int v && p.ParameterType.IsValueType) il.Emit(OpCodes.Ldc_I4, v); else il.Emit(OpCodes.Ldnull);
                il.Emit(m.HasThis ? OpCodes.Callvirt : OpCodes.Call, m);
                if (m.ReturnType.MetadataType != MetadataType.Void) il.Emit(OpCodes.Pop);
            }
            il.Emit(OpCodes.Ret);
            t.Methods.Add(md);
        }

        public MethodReference Ref(TypeReference type, string name, TypeReference ret, bool instance, params TypeReference[] args)
        {
            var r = new MethodReference(name, ret, type) { HasThis = instance };
            foreach (var a in args) r.Parameters.Add(new ParameterDefinition(a));
            return r;
        }

        // the Unity members the survey looks for
        public MethodReference LockState => Ref(Unity("UnityEngine", "Cursor"), "set_lockState", M.TypeSystem.Void, false, Unity("UnityEngine", "CursorLockMode", valueType: true));
        public MethodReference Visible => Ref(Unity("UnityEngine", "Cursor"), "set_visible", M.TypeSystem.Void, false, M.TypeSystem.Boolean);
        public MethodReference Raycast => Ref(Unity("UnityEngine.EventSystems", "BaseRaycaster", true), "Raycast", M.TypeSystem.Void, true,
                                              Unity("UnityEngine.EventSystems", "PointerEventData", true), M.TypeSystem.Object);
        public MethodReference RaycastAll => Ref(Unity("UnityEngine.EventSystems", "EventSystem", true), "RaycastAll", M.TypeSystem.Void, true,
                                                 Unity("UnityEngine.EventSystems", "PointerEventData", true), M.TypeSystem.Object);
        public MethodReference Select => Ref(Unity("UnityEngine.EventSystems", "EventSystem", true), "SetSelectedGameObject", M.TypeSystem.Void, true, M.TypeSystem.Object);
        public MethodReference MouseDown => Ref(Unity("UnityEngine", "Input"), "GetMouseButtonDown", M.TypeSystem.Boolean, false, M.TypeSystem.Int32);
        public MethodReference MousePosition => Ref(Unity("UnityEngine", "Input"), "get_mousePosition", Unity("UnityEngine", "Vector3", valueType: true), false);
        public MethodReference GetAxis => Ref(Unity("UnityEngine", "Input"), "GetAxis", M.TypeSystem.Single, false, M.TypeSystem.String);

        /// <summary>Rewired's source edition: its UI input module reads the mouse and selects UI elements.</summary>
        public void RewiredModule()
        {
            var mod = Class("Rewired.Integration.UnityUI", "RewiredStandaloneInputModule", Unity("UnityEngine.EventSystems", "BaseInputModule", true));
            Method(mod, "ProcessMouseEvents", (MouseDown, 0), (MousePosition, null), (RaycastAll, null), (Select, null));
            var demo = Class("Rewired.Demos", "TouchButtonExample");
            Method(demo, "Update", (MouseDown, 0));
        }

        public string Survey(string dir)
        {
            M.Write(Path.Combine(dir, "Assembly-CSharp.dll"));
            var w = new StringWriter();
            var old = Console.Out;
            Console.SetOut(w);
            var g = new Game(dir);
            try { AsmPatch.Survey.Run(g); } finally { g.Dispose(); Console.SetOut(old); }
            return w.ToString();
        }
    }

    static void SurveyTests()
    {
        // its own cursor: Baldi's Basics' shape (CursorManager locks and hides the pointer, CursorController
        // raycasts the UI at the cursor it draws), plus library code and a mouse-reading example
        var a = new FakeGame();
        a.RewiredModule();
        var mgr = a.Class("", "CursorManager");
        a.Method(mgr, "LockCursor", (a.LockState, 1), (a.Visible, 0));
        a.Method(mgr, "UnlockCursor", (a.LockState, 0), (a.Visible, 1));       // CursorLockMode.None: not a lock
        var cc = a.Class("", "CursorController");
        a.Method(cc, "Update", (a.GetAxis, null), (a.Raycast, null));
        var ex = a.Class("SomeAsset.Examples", "CameraController");
        a.Method(ex, "Update", (a.MousePosition, null));
        var cards = a.Class("", "CardHand");
        a.Method(cards, "Select", (a.MousePosition, null), (a.RaycastAll, null));   // a hit test at the mouse: the pointer's
        var own = a.Class("", "MyInputModule", a.Unity("UnityEngine.EventSystems", "BaseInputModule", true));
        a.Method(own, "Process", (a.RaycastAll, null));                       // an input module's raycast: not a hit test of its own
        var s = a.Survey(Dir("survey-own-cursor"));
        Check(s.Contains("pointer locked (Cursor.lockState = Locked): 1 (CursorManager.LockCursor)"), "survey: the pointer lock found (Locked only)");
        Check(s.Contains("UI hit tests of its own (Raycast, RaycastAll outside an input module, at no mouse or touch position): 1 (CursorController.Update)"),
              "survey: the game's own UI hit test found, not the input modules' (its own and Rewired's) nor one at the mouse's position");
        Check(s.Contains("verdict: likely its own cursor") && s.Contains("UA_POINTER_CURSOR=0"), "survey: own-cursor verdict");
        Check(s.Contains("  mouse              1 (CardHand.Select)") && s.Contains("0 selection calls"), "survey: Rewired's module, Rewired's demos and an asset's examples are not the game's input");
        Check(s.Contains("not counted above: 5 input call sites in library code"), "survey: library input call sites counted apart");
        Check(s.Contains("Rewired's UI input module (RewiredStandaloneInputModule) is in the build"), "survey: Rewired's UI input module noted");

        // mouse look: a lock, and UI hit tests only at the mouse's position
        var b = new FakeGame();
        var look = b.Class("", "PlayerLook");
        b.Method(look, "OnEnable", (b.LockState, 1));
        b.Method(look, "Update", (b.GetAxis, null));
        var pick = b.Class("", "Picker");
        b.Method(pick, "Update", (b.MousePosition, null), (b.Raycast, null));      // hit tests at the mouse's position only
        s = b.Survey(Dir("survey-mouse-look"));
        Check(!s.Contains("its own cursor") && s.Contains("verdict: pointer-driven UI") && s.Contains("locks the system pointer in places"),
              "survey: a pointer lock and hit tests at the mouse's position only: mouse look, noted under the usual verdict");
        Check(!s.Contains("Rewired's UI input module"), "survey: no Rewired, no Rewired note");

        // the Input System's Mouse.current: dereferenced at once (Hey, Listen!'s CursorHandler.Update), through a
        // local, and tested first (a branch on it, `?.`, a local compared with null) in other methods
        var d = new FakeGame();
        var isys = new AssemblyNameReference("Unity.InputSystem", new Version(1, 0, 0, 0));
        d.M.AssemblyReferences.Add(isys);
        var mouse = new TypeReference("UnityEngine.InputSystem", "Mouse", d.M, isys);
        var control = new TypeReference("UnityEngine.InputSystem.Controls", "Vector2Control", d.M, isys);
        var current = d.Ref(mouse, "get_current", mouse, false);
        var position = d.Ref(mouse, "get_position", control, true);
        void Body(TypeDefinition t, string name, Action<ILProcessor, MethodDefinition> emit)
        {
            var md = new MethodDefinition(name, MethodAttributes.Public | MethodAttributes.HideBySig, d.M.TypeSystem.Void);
            var il = md.Body.GetILProcessor();
            emit(il, md);
            il.Emit(OpCodes.Ret);
            t.Methods.Add(md);
        }
        var cursor = d.Class("", "CursorHandler");
        Body(cursor, "Update", (il, md) => { il.Emit(OpCodes.Call, current); il.Emit(OpCodes.Callvirt, position); il.Emit(OpCodes.Pop); });
        var aim = d.Class("", "CompanionVisual");
        Body(aim, "Update", (il, md) =>
        {
            md.Body.Variables.Add(new VariableDefinition(mouse));
            il.Emit(OpCodes.Call, current); il.Emit(OpCodes.Stloc_0); il.Emit(OpCodes.Ldloc_0); il.Emit(OpCodes.Callvirt, position); il.Emit(OpCodes.Pop);
        });
        var safe = d.Class("", "SafeCursor");
        Body(safe, "Update", (il, md) =>       // if (Mouse.current != null) Mouse.current.position...
        {
            var end = il.Create(OpCodes.Nop);
            il.Emit(OpCodes.Call, current); il.Emit(OpCodes.Brfalse_S, end);
            il.Emit(OpCodes.Call, current); il.Emit(OpCodes.Callvirt, position); il.Emit(OpCodes.Pop);
            il.Append(end);
        });
        Body(safe, "LateUpdate", (il, md) =>   // Mouse.current?.position
        {
            var has = il.Create(OpCodes.Callvirt, position);
            var end = il.Create(OpCodes.Pop);
            il.Emit(OpCodes.Call, current); il.Emit(OpCodes.Dup); il.Emit(OpCodes.Brtrue_S, has); il.Emit(OpCodes.Br_S, end);
            il.Append(has); il.Append(end);
        });
        Body(safe, "OnGUI", (il, md) =>        // var m = Mouse.current; if (m == null) return; m.position...
        {
            md.Body.Variables.Add(new VariableDefinition(mouse));
            var go = il.Create(OpCodes.Ldloc_0);
            il.Emit(OpCodes.Call, current); il.Emit(OpCodes.Stloc_0); il.Emit(OpCodes.Ldloc_0); il.Emit(OpCodes.Brtrue_S, go); il.Emit(OpCodes.Ret);
            il.Append(go); il.Emit(OpCodes.Callvirt, position); il.Emit(OpCodes.Pop);
        });
        s = d.Survey(Dir("survey-mouse-current"));
        Check(s.Contains("Mouse.current / Pointer.current: 6 reads"), "survey: Mouse.current reads counted");
        Check(s.Contains("used with no null check in 2 methods (CursorHandler.Update, CompanionVisual.Update)"),
              "survey: Mouse.current dereferenced at once or through a local, never tested: no null check; tested ones (branch, ?., local == null) not counted");
        Check(s.Contains("verdict: needs a mouse") && s.Contains("UA_POINTER=1"), "survey: a game that cannot run without a mouse: pointer mode");
        var e = new FakeGame();
        var isys2 = new AssemblyNameReference("Unity.InputSystem", new Version(1, 0, 0, 0));
        e.M.AssemblyReferences.Add(isys2);
        var mouse2 = new TypeReference("UnityEngine.InputSystem", "Mouse", e.M, isys2);
        var cur2 = e.Ref(mouse2, "get_current", mouse2, false);
        var safe2 = e.Class("", "SafeCursor");
        var m2 = new MethodDefinition("Update", MethodAttributes.Public | MethodAttributes.HideBySig, e.M.TypeSystem.Void);
        var il2 = m2.Body.GetILProcessor();
        var ret2 = il2.Create(OpCodes.Ret);
        il2.Emit(OpCodes.Call, cur2); il2.Emit(OpCodes.Brfalse_S, ret2); il2.Append(ret2);
        safe2.Methods.Add(m2);
        s = e.Survey(Dir("survey-mouse-checked"));
        Check(s.Contains("Mouse.current / Pointer.current: 1 reads") && !s.Contains("no null check") && s.Contains("verdict: pointer-driven UI"),
              "survey: Mouse.current tested before use: a mouse read (pointer use), no NullReferenceException note");

        // pointer-driven menus: the only selection calls are Rewired's module's, so they do not hide the verdict
        var c = new FakeGame();
        c.RewiredModule();
        var menu = c.Class("", "MainMenu");
        c.Method(menu, "Update", (c.MouseDown, 0), (c.MousePosition, null));
        s = c.Survey(Dir("survey-pointer"));
        Check(s.Contains("verdict: pointer-driven UI") && s.Contains("pointer locked (Cursor.lockState = Locked): 0"),
              "survey: pointer-driven menus (Rewired's own selection calls not counted)");
    }
}
