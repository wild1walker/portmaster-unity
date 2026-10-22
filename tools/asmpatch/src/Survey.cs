// --survey: which input stacks a game's code uses, and what each needs on Unity's Android player.
// Read only. Counts call sites in the game's own assemblies (not Unity's or the class libraries);
// library code compiled into them (Rewired's source edition: its UI input module, demos, control
// mapper; asset demos and examples) is counted apart, as it does not tell how the game's menus work.
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
        var textEntry = new Counter();          // InputField / TMP_InputField / TouchScreenKeyboard
        var mouseMsgs = new List<string>();
        var pointerTypes = new List<string>();
        var navTypes = new List<string>();
        var rewired = new List<string>();
        bool inControlLib = false, rewiredUiModule = false;
        var library = new Counter();            // input call sites in library code (LibraryCode)
        var locks = new Counter();              // Cursor.lockState = Locked
        var uiRaycasts = new Counter();         // UI hit tests outside an input module, at a position not read from the pointer
        var mouseCurrent = new Counter();       // the Input System's Mouse.current / Pointer.current
        var mouseUnchecked = new Counter();     // ... in methods that use it without testing it for null

        foreach (var asm in game.GameAssemblies)
        {
            var file = game.FileOf(asm);
            foreach (var t in Cecil.AllTypes(asm.MainModule))
            {
                if (t.FullName == "InControl.InputManager" || t.FullName == "InControl.InControlManager") inControlLib = true;
                if (t.FullName == "Rewired.Integration.UnityUI.RewiredStandaloneInputModule") rewiredUiModule = true;
                bool lib = LibraryCode(t), inputModule = DerivesFrom(t, "UnityEngine.EventSystems.BaseInputModule");
                foreach (var i in t.Interfaces)
                {
                    if (lib) break;
                    var n = i.InterfaceType.Name;
                    if (i.InterfaceType.Namespace != "UnityEngine.EventSystems") continue;
                    if (PointerHandlers.Contains(n) && !pointerTypes.Contains(t.FullName)) pointerTypes.Add(t.FullName);
                    if (NavHandlers.Contains(n) && !navTypes.Contains(t.FullName)) navTypes.Add(t.FullName);
                }
                foreach (var m in t.Methods)
                {
                    if (!lib && !m.IsStatic && m.Parameters.Count == 0 && MouseMessages.Contains(m.Name) && DerivesFromMonoBehaviour(t))
                        mouseMsgs.Add($"{t.FullName}.{m.Name}");
                    if (m.Name == "DetectPlatform" && m.HasBody && Cecil.Hierarchy(t).Skip(1).Any(b => b.FullName == "Rewired.InputManager_Base"))
                        rewired.Add($"{file}: {t.FullName} ({(file.StartsWith("Assembly-CSharp") ? "source edition" : "DLL edition")}), DetectPlatform sets {Platform(m)}");
                    if (!m.HasBody) continue;
                    bool readsPointer = m.Body.Instructions.Any(i => i.Operand is MethodReference r && PointerRead(r));
                    if (!lib && DeviceCurrentUse(m) is var (reads, tested) && reads > 0)
                    {
                        for (int k = 0; k < reads; k++) mouseCurrent.Add($"{t.FullName}.{m.Name}");
                        if (!tested) mouseUnchecked.Add($"{t.FullName}.{m.Name}");
                    }
                    foreach (var ins in m.Body.Instructions)
                    {
                        if (ins.Operand is not MemberReference mr || mr.DeclaringType == null) continue;
                        var dt = mr.DeclaringType.FullName;
                        var where = $"{t.FullName}.{m.Name}";
                        if (dt == "UnityEngine.Input")
                            foreach (var (g, members) in LegacyGroups)
                                if (members.Contains(mr.Name)) (lib ? library : legacy[g]).Add(where);
                        if ((dt == "UnityEngine.EventSystems.EventSystem" && (mr.Name == "SetSelectedGameObject" || mr.Name == "set_firstSelectedGameObject")) ||
                            (dt == "UnityEngine.UI.Selectable" || dt.StartsWith("UnityEngine.UI.")) && mr.Name == "Select")
                            (lib ? library : selects).Add(where);
                        if (lib) continue;
                        if (dt == "UnityEngine.Cursor" && mr.Name == "set_lockState" && ins.Previous != null && Cecil.ConstInt(ins.Previous) == 1)
                            locks.Add(where);           // CursorLockMode.Locked
                        if (!inputModule && !readsPointer && (mr.Name == "Raycast" && (dt == "UnityEngine.EventSystems.BaseRaycaster" || dt == "UnityEngine.UI.GraphicRaycaster") ||
                                             mr.Name == "RaycastAll" && dt == "UnityEngine.EventSystems.EventSystem"))
                            uiRaycasts.Add(where);
                        if (dt.StartsWith("UnityEngine.InputSystem")) inputSystemRefs.Add(where);
                        if (dt.StartsWith("InControl.") && !asm.Name.Name.StartsWith("InControl")) inControlRefs.Add(where);
                        if (dt == "UnityEngine.TouchScreenKeyboard" || dt == "UnityEngine.UI.InputField" || dt == "TMPro.TMP_InputField") textEntry.Add(where);
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
        if (rewiredUiModule)
            Console.WriteLine("  Rewired's UI input module (RewiredStandaloneInputModule) is in the build: it takes Unity's mouse and touches as Unity's\n" +
                              "  StandaloneInputModule does, so pointer mode reaches the uGUI elements it serves (with its \"Allow Mouse Input If Touch\n" +
                              "  Supported\" off it takes only touches on Android, where Input.touchSupported is true: UA_POINTER=touch)");
        Console.WriteLine("InControl: " + (inControlLib || inControlRefs.N > 0 ? $"yes ({inControlRefs.N} call sites); picks its device profiles at run time, by the running platform and the pad's name" : "no"));
        if (inControlLib || inControlRefs.N > 0)
        {
            var android = InControlAndroidProfiles(game);
            if (android.Count == 0)
                Console.WriteLine("  no Android device profile found in the game's assemblies (a pad will be an unknown controller)");
            else
            {
                Console.WriteLine($"  Android profiles ({android.Count}; a pad whose name none of them matches is an unknown controller):");
                foreach (var (type, title, pnames, regex) in android.Take(16))
                    Console.WriteLine($"    {type}{(title != null ? $" ({title})" : "")}: " + string.Join(", ", pnames.Select(n => "\"" + n + "\"").Concat(regex.Select(r => "/" + r + "/"))));
                if (android.Count > 16) Console.WriteLine("    ...");
                var first = android.SelectMany(a => a.names).FirstOrDefault();
                if (first != null) Console.WriteLine($"  -> UA_PAD_NAME=\"{first}\" (or another name above) for a pad InControl maps as on Android");
            }
        }
        bool isys = names.Contains("Unity.InputSystem");
        Console.WriteLine("Input System package: " + (isys ? $"Unity.InputSystem.dll, {inputSystemRefs.N} call sites in game code{inputSystemRefs.Examples()}" : "no"));
        if (isys) Console.WriteLine("  -> asmpatch inputsystem-android (a desktop build has no layout for the Android player's pads; keyboard, mouse and touch need nothing)");
        if (mouseCurrent.N > 0)
        {
            Console.WriteLine($"  Mouse.current / Pointer.current: {mouseCurrent.N} reads{mouseCurrent.Examples()}");
            if (mouseUnchecked.N > 0)
                Console.WriteLine($"  used with no null check in {mouseUnchecked.N} methods{mouseUnchecked.Examples()}: a handheld has no mouse unless pointer\n" +
                                  "  mode presents one (UA_POINTER=1); without it these throw NullReferenceException (every frame from an Update)");
        }
        Console.WriteLine("legacy Input (UnityEngine.Input):");
        foreach (var (g, c) in legacy) Console.WriteLine($"  {g,-15} {c.N,4}{c.Examples()}");
        Console.WriteLine($"mouse messages (OnMouse*): {mouseMsgs.Count}{Examples(mouseMsgs)}");
        Console.WriteLine($"UI pointer handlers (IPointer*/IDrag*/IScroll*): {pointerTypes.Count}{Examples(pointerTypes)}");
        Console.WriteLine($"UI navigation: {navTypes.Count} handler types, {selects.N} selection calls (SetSelectedGameObject/Select){selects.Examples()}");
        Console.WriteLine($"text entry (InputField, TMP_InputField, TouchScreenKeyboard): {textEntry.N}{textEntry.Examples()}" +
                          (textEntry.N > 0 ? " -> the soft keyboard opens ua_host's on-screen keyboard" : ""));
        Console.WriteLine($"pointer locked (Cursor.lockState = Locked): {locks.N}{locks.Examples()}");
        Console.WriteLine($"UI hit tests of its own (Raycast, RaycastAll outside an input module, at no mouse or touch position): {uiRaycasts.N}{uiRaycasts.Examples()}");
        if (library.N > 0)
            Console.WriteLine($"not counted above: {library.N} input call sites in library code compiled into the game (Rewired's own, asset demos/examples){library.Examples()}");
        Console.WriteLine();
        bool pointerUse = legacy["mouse"].N > 0 || mouseMsgs.Count > 0 || pointerTypes.Count > 0 || mouseCurrent.N > 0;
        bool padUse = rewired.Count > 0 || inControlLib || legacy["axes/buttons"].N > 0 || selects.N > 0 || navTypes.Count > 0;
        if (mouseUnchecked.N > 0)
            Console.WriteLine($"verdict: needs a mouse: the Input System's Mouse.current is used with no null check{mouseUnchecked.Examples()}: UA_POINTER=1\n" +
                              "  (pointer mode's Android mouse); if the game draws its own cursor, also UA_POINTER_CURSOR=0");
        else if (locks.N > 0 && uiRaycasts.N > 0)
            Console.WriteLine($"verdict: likely its own cursor: the game locks the system pointer and hit-tests its UI itself{uiRaycasts.Examples()},\n" +
                              "  so it draws a cursor and moves it by mouse motion (and by the pad, where it maps one). Pointer mode's position and\n" +
                              "  touches do not steer it: try the pad without UA_POINTER; else UA_POINTER=1 UA_POINTER_CURSOR=0 (the stick moves the\n" +
                              "  game's cursor through mouse motion, the buttons click). docs/input.md, \"Games with their own cursor\"");
        else if (pointerUse && selects.N == 0 && navTypes.Count == 0)
            Console.WriteLine("verdict: pointer-driven UI (no code selects UI elements for pad/keyboard navigation): UA_POINTER=1 likely needed for menus");
        else if (pointerUse)
            Console.WriteLine("verdict: reads the mouse, but also selects UI elements (pad navigation may work; UA_POINTER=1 where it does not)");
        else if (padUse)
            Console.WriteLine("verdict: pad/keyboard input");
        else
            Console.WriteLine("verdict: no input code found in the game's assemblies (UI built in scenes only? check the title screen)");
        if (locks.N > 0 && uiRaycasts.N == 0)
            Console.WriteLine("  the game locks the system pointer in places (mouse look, or a cursor of its own there): while it is locked the game\n" +
                              "  reads mouse motion, not the pointer's position");
        PathFlow.Print(game);
        return 0;
    }

    /// <summary>InControl's device profiles that include Android: the profile classes (deriving from
    /// an InControl *Profile* class) whose constructor or Define() stores "Android" into
    /// IncludePlatforms / SupportedPlatforms, with the joystick names and patterns they match
    /// (JoystickNames / NameLiteral, JoystickRegex / LastResortRegex / NamePattern). Read from
    /// the IL by the member each string is stored into, which covers the old (constructor) and
    /// the new (Define, matchers) profile styles.</summary>
    public static List<(string type, string title, List<string> names, List<string> regex)> InControlAndroidProfiles(Game game)
    {
        var found = new List<(string, string, List<string>, List<string>)>();
        foreach (var asm in game.GameAssemblies)
            foreach (var t in Cecil.AllTypes(asm.MainModule))
            {
                if (!t.IsClass || t.IsAbstract || !Cecil.Hierarchy(t).Skip(1).Any(b => b.Namespace == "InControl" && b.Name.Contains("Profile"))) continue;
                var stores = new List<(string member, string value)>();
                foreach (var m in t.Methods.Where(m => m.HasBody && (m.IsConstructor && !m.IsStatic || m.Name == "Define")))
                {
                    var ins = m.Body.Instructions;
                    for (int i = 0; i < ins.Count; i++)
                    {
                        if (ins[i].OpCode.Code != Code.Ldstr) continue;
                        for (int j = i + 1; j < ins.Count && j < i + 64; j++)
                        {
                            string member = ins[j].Operand switch
                            {
                                FieldReference f when ins[j].OpCode.Code is Code.Stfld or Code.Stsfld => f.Name,
                                MethodReference mr when (ins[j].OpCode.Code is Code.Call or Code.Callvirt) && mr.Name.StartsWith("set_") => mr.Name.Substring(4),
                                _ => null,
                            };
                            if (member == null) continue;
                            stores.Add((member, (string)ins[i].Operand));
                            break;
                        }
                    }
                }
                List<string> Of(params string[] members) => stores.Where(s => members.Contains(s.member)).Select(s => s.value).Distinct().ToList();
                if (!Of("IncludePlatforms", "SupportedPlatforms").Any(p => p.IndexOf("android", StringComparison.OrdinalIgnoreCase) >= 0)) continue;
                var title = Of("Name", "DeviceName").FirstOrDefault();
                found.Add((t.Name, title, Of("JoystickNames", "NameLiteral"), Of("JoystickRegex", "LastResortRegex", "NamePattern")));
            }
        // Xbox pads first (their Android profiles map the standard Android layout ua_host's pads have), then generic ones
        static int Rank(string type) => type.Contains("XboxOne", StringComparison.OrdinalIgnoreCase) ? 0 : type.Contains("Xbox", StringComparison.OrdinalIgnoreCase) ? 1 :
                                        type.Contains("Generic", StringComparison.OrdinalIgnoreCase) ? 2 : 3;
        return found.OrderBy(f => Rank(f.Item1)).ThenBy(f => f.Item1, StringComparer.Ordinal).ToList();
    }

    static string Platform(MethodDefinition m)
    {
        string last = "?";
        foreach (var i in m.Body.Instructions)
            if (i.OpCode.Code == Code.Stfld && i.Operand is FieldReference f && f.Name == "platform" && i.Previous != null && Cecil.ConstInt(i.Previous) is int v)
                last = Cecil.EnumName(Cecil.TryResolve(f.FieldType), v);
        return last;
    }

    /// <summary>Library code compiled into a game assembly, whose input calls say nothing about the
    /// game's own UI: Rewired's source edition (namespace Rewired: its UI input module, demos, control
    /// mapper) and the demo/example scripts assets ship (a Demos/Examples/Samples namespace).</summary>
    static bool LibraryCode(TypeDefinition t)
    {
        while (t.DeclaringType != null) t = t.DeclaringType;
        var ns = t.Namespace ?? "";
        return ns == "Rewired" || ns.StartsWith("Rewired.") ||
               ns.Split('.').Any(s => s is "Demo" or "Demos" or "Example" or "Examples" or "Sample" or "Samples");
    }

    /// <summary>A read of the pointer's position: the mouse's or a touch's (legacy Input, Touch, the
    /// Input System's Pointer.position or a Vector2 control's value).</summary>
    static bool PointerRead(MethodReference r)
    {
        var dt = r.DeclaringType?.FullName ?? "";
        return dt == "UnityEngine.Input" && r.Name is "get_mousePosition" or "GetTouch" or "get_touches" ||
               dt == "UnityEngine.Touch" && r.Name == "get_position" ||
               dt.StartsWith("UnityEngine.InputSystem.") && (r.Name == "get_position" || r.Name == "ReadValue" && dt.EndsWith("<UnityEngine.Vector2>"));
    }

    /// <summary>The method's reads of the Input System's Mouse.current / Pointer.current, and whether
    /// it tests one for null: the value branched on (brtrue/brfalse, `?.`'s dup + branch) or compared
    /// with null, directly or through the local it is stored in. A method that only dereferences it
    /// (Mouse.current.position...) throws when there is no mouse.</summary>
    static (int reads, bool tested) DeviceCurrentUse(MethodDefinition m)
    {
        int reads = 0;
        bool tested = false;
        var ins = m.Body.Instructions;
        for (int i = 0; i < ins.Count; i++)
        {
            if (ins[i].Operand is not MethodReference r || r.Name != "get_current" ||
                r.DeclaringType?.FullName is not ("UnityEngine.InputSystem.Mouse" or "UnityEngine.InputSystem.Pointer")) continue;
            reads++;
            var next = Skip(ins[i].Next);
            if (next != null && next.OpCode.Code == Code.Dup) next = Skip(next.Next);
            if (next != null && Cecil.LocalIndex(next, store: true) is int loc)
            {
                // the first load of that local after the store decides
                var store = next;
                next = null;
                for (var j = store.Next; j != null; j = j.Next)
                    if (Cecil.LocalIndex(j, store: false) == loc) { next = Skip(j.Next); break; }
            }
            if (next != null && Tests(next)) tested = true;
        }
        return (reads, tested);

        static Instruction Skip(Instruction i) { while (i != null && i.OpCode.Code == Code.Nop) i = i.Next; return i; }
        static bool Tests(Instruction i) => i.OpCode.Code is Code.Brtrue or Code.Brtrue_S or Code.Brfalse or Code.Brfalse_S ||
            i.OpCode.Code == Code.Ldnull && i.Next != null && i.Next.OpCode.Code is Code.Ceq or Code.Cgt_Un or Code.Beq or Code.Beq_S or Code.Bne_Un or Code.Bne_Un_S ||
            i.Operand is MethodReference r && r.Name is "op_Equality" or "op_Inequality" or "op_Implicit";
    }

    static bool DerivesFrom(TypeDefinition t, string baseName)
    {
        for (var b = t.BaseType; b != null; )
        {
            if (b.FullName == baseName) return true;
            var d = Cecil.TryResolve(b);
            if (d == null) return false;
            b = d.BaseType;
        }
        return false;
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
