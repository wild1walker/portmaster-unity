// --survey: which input stacks a game's code uses, and what each needs on Unity's Android player.
// Read only. Counts call sites in the game's own assemblies (not Unity's or the class libraries).
using Mono.Cecil;
using Mono.Cecil.Cil;

namespace AsmPatch;

static class Survey
{
    // UnityEngine.Input members by what they read
    static readonly (string group, string[] members)[] LegacyGroups =
    {
        ("axes/buttons", new[] { "GetAxis", "GetAxisRaw", "GetButton", "GetButtonDown", "GetButtonUp" }),
        ("keys", new[] { "GetKey", "GetKeyDown", "GetKeyUp", "get_anyKey", "get_anyKeyDown", "get_inputString" }),
        ("mouse", new[] { "GetMouseButton", "GetMouseButtonDown", "GetMouseButtonUp", "get_mousePosition", "get_mouseScrollDelta", "get_mousePresent" }),
        ("touch", new[] { "get_touchCount", "GetTouch", "get_touches", "get_touchSupported", "get_multiTouchEnabled" }),
        ("joystick names", new[] { "GetJoystickNames" }),
    };
    static readonly string[] MouseMessages = { "OnMouseDown", "OnMouseUp", "OnMouseUpAsButton", "OnMouseEnter", "OnMouseExit", "OnMouseOver", "OnMouseDrag" };
    static readonly string[] PointerHandlers = { "IPointerClickHandler", "IPointerDownHandler", "IPointerUpHandler", "IPointerEnterHandler", "IPointerExitHandler", "IDragHandler", "IBeginDragHandler", "IScrollHandler" };
    static readonly string[] NavHandlers = { "ISubmitHandler", "ISelectHandler", "IMoveHandler", "ICancelHandler", "IDeselectHandler" };

    public static int Run(Game game)
    {
        var names = game.Names.ToHashSet(StringComparer.OrdinalIgnoreCase);
        var legacy = LegacyGroups.ToDictionary(g => g.group, g => new Counter());
        var selects = new Counter();            // EventSystem.SetSelectedGameObject, Selectable.Select, firstSelectedGameObject
        var inputSystemRefs = new Counter();
        var inControlRefs = new Counter();
        var mouseMsgs = new List<string>();
        var pointerTypes = new List<string>();
        var navTypes = new List<string>();
        var rewired = new List<string>();
        bool inControlLib = false;

        foreach (var asm in game.GameAssemblies)
        {
            var file = game.FileOf(asm);
            foreach (var t in Cecil.AllTypes(asm.MainModule))
            {
                if (t.FullName == "InControl.InputManager" || t.FullName == "InControl.InControlManager") inControlLib = true;
                foreach (var i in t.Interfaces)
                {
                    var n = i.InterfaceType.Name;
                    if (i.InterfaceType.Namespace != "UnityEngine.EventSystems") continue;
                    if (PointerHandlers.Contains(n) && !pointerTypes.Contains(t.FullName)) pointerTypes.Add(t.FullName);
                    if (NavHandlers.Contains(n) && !navTypes.Contains(t.FullName)) navTypes.Add(t.FullName);
                }
                foreach (var m in t.Methods)
                {
                    if (!m.IsStatic && m.Parameters.Count == 0 && MouseMessages.Contains(m.Name) && DerivesFromMonoBehaviour(t))
                        mouseMsgs.Add($"{t.FullName}.{m.Name}");
                    if (m.Name == "DetectPlatform" && m.HasBody && Cecil.Hierarchy(t).Skip(1).Any(b => b.FullName == "Rewired.InputManager_Base"))
                        rewired.Add($"{file}: {t.FullName} ({(file.StartsWith("Assembly-CSharp") ? "source edition" : "DLL edition")}), DetectPlatform sets {Platform(m)}");
                    if (!m.HasBody) continue;
                    foreach (var ins in m.Body.Instructions)
                    {
                        if (ins.Operand is not MemberReference mr || mr.DeclaringType == null) continue;
                        var dt = mr.DeclaringType.FullName;
                        var where = $"{t.FullName}.{m.Name}";
                        if (dt == "UnityEngine.Input")
                            foreach (var (g, members) in LegacyGroups)
                                if (members.Contains(mr.Name)) legacy[g].Add(where);
                        if ((dt == "UnityEngine.EventSystems.EventSystem" && (mr.Name == "SetSelectedGameObject" || mr.Name == "set_firstSelectedGameObject")) ||
                            (dt == "UnityEngine.UI.Selectable" || dt.StartsWith("UnityEngine.UI.")) && mr.Name == "Select")
                            selects.Add(where);
                        if (dt.StartsWith("UnityEngine.InputSystem")) inputSystemRefs.Add(where);
                        if (dt.StartsWith("InControl.") && !asm.Name.Name.StartsWith("InControl")) inControlRefs.Add(where);
                    }
                }
            }
        }

        Console.WriteLine($"input survey of {game.Dir}");
        Console.WriteLine();
        Console.WriteLine("Rewired: " + (rewired.Count == 0 ? (names.Contains("Rewired_Core") ? "Rewired_Core.dll present, no InputManager found" : "no") : ""));
        foreach (var s in rewired) Console.WriteLine("  " + s);
        if (rewired.Count > 0)
            Console.WriteLine("  -> asmpatch rewired-android (Rewired then reads pads through Unity input; android-data adds its Joy<j>Axis<a> axes)");
        Console.WriteLine("InControl: " + (inControlLib || inControlRefs.N > 0 ? $"yes ({inControlRefs.N} call sites); picks its device profiles at run time (Android profiles by pad name)" : "no"));
        bool isys = names.Contains("Unity.InputSystem");
        Console.WriteLine("Input System package: " + (isys ? $"Unity.InputSystem.dll, {inputSystemRefs.N} call sites in game code{inputSystemRefs.Examples()}" : "no"));
        if (isys) Console.WriteLine("  (its Android device layouts are compiled in only for Android builds: pads may need more than this; the player setting Active Input Handling decides which stack runs)");
        Console.WriteLine("legacy Input (UnityEngine.Input):");
        foreach (var (g, c) in legacy) Console.WriteLine($"  {g,-15} {c.N,4}{c.Examples()}");
        Console.WriteLine($"mouse messages (OnMouse*): {mouseMsgs.Count}{Examples(mouseMsgs)}");
        Console.WriteLine($"UI pointer handlers (IPointer*/IDrag*/IScroll*): {pointerTypes.Count}{Examples(pointerTypes)}");
        Console.WriteLine($"UI navigation: {navTypes.Count} handler types, {selects.N} selection calls (SetSelectedGameObject/Select){selects.Examples()}");
        Console.WriteLine();
        bool pointerUse = legacy["mouse"].N > 0 || mouseMsgs.Count > 0 || pointerTypes.Count > 0;
        bool padUse = rewired.Count > 0 || inControlLib || legacy["axes/buttons"].N > 0 || selects.N > 0 || navTypes.Count > 0;
        if (pointerUse && selects.N == 0 && navTypes.Count == 0)
            Console.WriteLine("verdict: pointer-driven UI (no code selects UI elements for pad/keyboard navigation): UA_POINTER=1 likely needed for menus");
        else if (pointerUse)
            Console.WriteLine("verdict: reads the mouse, but also selects UI elements (pad navigation may work; UA_POINTER=1 where it does not)");
        else if (padUse)
            Console.WriteLine("verdict: pad/keyboard input");
        else
            Console.WriteLine("verdict: no input code found in the game's assemblies (UI built in scenes only? check the title screen)");
        return 0;
    }

    static string Platform(MethodDefinition m)
    {
        string last = "?";
        foreach (var i in m.Body.Instructions)
            if (i.OpCode.Code == Code.Stfld && i.Operand is FieldReference f && f.Name == "platform" && i.Previous != null && Cecil.ConstInt(i.Previous) is int v)
                last = Cecil.EnumName(Cecil.TryResolve(f.FieldType), v);
        return last;
    }

    static bool DerivesFromMonoBehaviour(TypeDefinition t) => Cecil.Hierarchy(t).Any(b => b.FullName == "UnityEngine.MonoBehaviour") ||
        t.BaseType != null && t.BaseType.FullName == "UnityEngine.MonoBehaviour";

    static string Examples(List<string> l) => l.Count == 0 ? "" : " (" + string.Join(", ", l.Take(4)) + (l.Count > 4 ? ", ..." : "") + ")";

    sealed class Counter
    {
        public int N;
        readonly List<string> where = new();
        public void Add(string w) { N++; if (!where.Contains(w)) where.Add(w); }
        public string Examples() => Survey.Examples(where);
    }
}
