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
        var textEntry = new Counter();          // InputField / TMP_InputField / TouchScreenKeyboard
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
        Console.WriteLine("legacy Input (UnityEngine.Input):");
        foreach (var (g, c) in legacy) Console.WriteLine($"  {g,-15} {c.N,4}{c.Examples()}");
        Console.WriteLine($"mouse messages (OnMouse*): {mouseMsgs.Count}{Examples(mouseMsgs)}");
        Console.WriteLine($"UI pointer handlers (IPointer*/IDrag*/IScroll*): {pointerTypes.Count}{Examples(pointerTypes)}");
        Console.WriteLine($"UI navigation: {navTypes.Count} handler types, {selects.N} selection calls (SetSelectedGameObject/Select){selects.Examples()}");
        Console.WriteLine($"text entry (InputField, TMP_InputField, TouchScreenKeyboard): {textEntry.N}{textEntry.Examples()}" +
                          (textEntry.N > 0 ? " -> the soft keyboard opens ua_host's on-screen keyboard" : ""));
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
