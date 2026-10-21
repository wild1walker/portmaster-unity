// inputsystem-android: Unity's Input System package (com.unity.inputsystem) compiles its Android
// device support only into Android (and editor) builds. On Unity's Android player the engine's
// input backend reports the handheld's pad as an Android game controller (interface "Android",
// device class "AndroidGameController", state format 'AGC '); a desktop build's
// Unity.InputSystem.dll has no layout for that, so the pad never becomes a Gamepad and the game's
// gamepad bindings and UI navigation see nothing. Keyboards, mice and touchscreens are reported
// in the package's common formats and need nothing.
//
// The patch registers layouts for those devices where the package registers its platform
// support (InputSystem.PerformDefaultPluginInitialization, right before each return), with the
// package's own public InputSystem.RegisterLayout(json). The layouts are written here from the
// Android player's state format, as its engine code writes it: Android key codes as bits from
// byte 0 (KEYCODE_BUTTON_A = bit 96, ...), MotionEvent axes as floats from byte 28 (AXIS_X at 28,
// AXIS_HAT_X at 88, ...). They extend the package's Gamepad layout, so the pad is a Gamepad with
// the usual controls: the A/B/X/Y buttons, shoulders, stick presses, Start/Select, both sticks
// (Android's y axes point down: inverted), the triggers (AXIS_BRAKE / AXIS_GAS) and the d-pad
// (AXIS_HAT_X / HAT_Y, the axes every pad of ua_host reports). A pad with Microsoft's USB vendor
// id becomes an XInputController (as an Android build makes it), where the package has that type.
using System.Text;
using Mono.Cecil;
using Mono.Cecil.Cil;
using Mono.Cecil.Rocks;

namespace AsmPatch;

sealed class InputSystemAndroid : Patch
{
    public override string Name => "inputsystem-android";
    public override string Summary =>
        "the Input System package recognises the Android player's game controllers (a desktop build has no layout for them): the pad becomes a Gamepad as in the game's Android builds";

    const string AssemblyName = "Unity.InputSystem";
    const string Marker = "\"deviceClass\":\"AndroidGameController\"";
    public const string GamepadLayout = "AndroidGamepadForPlayer";
    public const string XboxLayout = "AndroidXboxGamepadForPlayer";

    // android.view.KeyEvent / MotionEvent constants
    const int KEYCODE_BUTTON_A = 96, KEYCODE_BUTTON_B = 97, KEYCODE_BUTTON_X = 99, KEYCODE_BUTTON_Y = 100,
              KEYCODE_BUTTON_L1 = 102, KEYCODE_BUTTON_R1 = 103, KEYCODE_BUTTON_THUMBL = 106, KEYCODE_BUTTON_THUMBR = 107,
              KEYCODE_BUTTON_START = 108, KEYCODE_BUTTON_SELECT = 109;
    const int AXIS_X = 0, AXIS_Y = 1, AXIS_Z = 11, AXIS_RZ = 14, AXIS_HAT_X = 15, AXIS_HAT_Y = 16, AXIS_GAS = 22, AXIS_BRAKE = 23;
    // the 'AGC ' state: 220 key-code bits (7 x 32), then 48 floats indexed by axis
    const int AxisBase = 7 * 4;
    static int Axis(int a) => AxisBase + 4 * a;

    public override Result Run(Game game, bool apply)
    {
        var asm = game.Load(AssemblyName);
        if (asm == null) return Result.NotApplicable("no Unity.InputSystem.dll");
        var module = asm.MainModule;
        var file = game.FileOf(asm);
        if (Cecil.AllTypes(module).Any(t => t.Namespace == "UnityEngine.InputSystem.Android"))
            return Result.NotApplicable($"{file} has the package's Android support (an Android or editor build)");
        var system = module.GetType("UnityEngine.InputSystem.InputSystem");
        if (system == null) return Result.Failed($"{file}: no UnityEngine.InputSystem.InputSystem");
        var init = system.Methods.FirstOrDefault(m => m.Name == "PerformDefaultPluginInitialization" && m.IsStatic && !m.HasParameters && m.HasBody &&
                                                      m.ReturnType.MetadataType == MetadataType.Void);
        if (init == null) return Result.Failed($"{file}: no InputSystem.PerformDefaultPluginInitialization()");
        var register = system.Methods.FirstOrDefault(m => m.Name == "RegisterLayout" && m.IsStatic && m.IsPublic && m.Parameters.Count == 3 &&
            m.Parameters[0].ParameterType.MetadataType == MetadataType.String && m.Parameters[1].ParameterType.MetadataType == MetadataType.String &&
            m.Parameters[2].ParameterType is GenericInstanceType g && g.ElementType.FullName == "System.Nullable`1");
        if (register == null) return Result.Failed($"{file}: no InputSystem.RegisterLayout(string json, string name, InputDeviceMatcher? matches)");
        var where = $"{file}: {system.FullName}.{init.Name}";
        if (init.Body.Instructions.Any(i => i.OpCode.Code == Code.Ldstr && ((string)i.Operand).Contains(Marker)))
            return new Result { Outcome = Outcome.AlreadyApplied, Details = { $"{where}: registers the Android game controller layouts" } };

        bool xinput = module.GetType("UnityEngine.InputSystem.XInput.XInputController") != null;
        var layouts = Layouts(xinput);
        var r = new Result { Outcome = Outcome.Applied };
        r.Details.Add($"{where}: + layouts {string.Join(", ", layouts.Select(l => l.name))} for interface Android, device class AndroidGameController ('AGC ' state)");
        if (!apply) return r;

        var body = init.Body;
        body.SimplifyMacros();
        var il = body.GetILProcessor();
        var nullable = register.Parameters[2].ParameterType;          // InputDeviceMatcher? (same module)
        var none = new VariableDefinition(nullable);
        body.Variables.Add(none);
        body.InitLocals = true;
        var rets = body.Instructions.Where(i => i.OpCode.Code == Code.Ret).ToList();
        if (rets.Count == 0) return Result.Failed($"{where}: no return");
        foreach (var ret in rets)
        {
            var seq = new List<Instruction>();
            foreach (var (_, json) in layouts)
            {
                seq.Add(il.Create(OpCodes.Ldstr, json));
                seq.Add(il.Create(OpCodes.Ldnull));
                seq.Add(il.Create(OpCodes.Ldloca, none));
                seq.Add(il.Create(OpCodes.Initobj, nullable));
                seq.Add(il.Create(OpCodes.Ldloc, none));
                seq.Add(il.Create(OpCodes.Call, register));
            }
            // the ret becomes the first new instruction: branches to it now lead into the calls
            ret.OpCode = seq[0].OpCode;
            ret.Operand = seq[0].Operand;
            var prev = ret;
            foreach (var x in seq.Skip(1).Append(il.Create(OpCodes.Ret))) { il.InsertAfter(prev, x); prev = x; }
        }
        body.OptimizeMacros();
        game.MarkChanged(asm);
        return r;
    }

    /* ---- the layouts (JSON, the package's layout format) ------------------------------------- */
    public static List<(string name, string json)> Layouts(bool xinput)
    {
        var l = new List<(string, string)> { (GamepadLayout, Gamepad()) };
        if (xinput) l.Add((XboxLayout, Xbox()));
        return l;
    }

    static string Q(string s) => "\"" + s + "\"";

    sealed class Controls
    {
        readonly List<string> items = new();
        public Controls Item(string name, params (string key, object value)[] fields)
        {
            var sb = new StringBuilder("{" + Q("name") + ":" + Q(name));
            foreach (var (k, v) in fields) sb.Append("," + Q(k) + ":" + (v is string s ? Q(s) : v.ToString()));
            items.Add(sb.Append('}').ToString());
            return this;
        }
        public Controls Button(string name, int keycode) => Item(name, ("offset", 0), ("bit", keycode), ("format", "BIT"), ("sizeInBits", 1));
        public override string ToString() => "[" + string.Join(",", items) + "]";
    }

    static string Gamepad()
    {
        // stick: x right, y down on Android; the package's y points up
        const string up = "clamp=2,clampMin=-1,clampMax=0,invert", down = "clamp=2,clampMin=0,clampMax=1";
        // d-pad from the hat axes: -1 / 0 / 1 per axis
        const string neg = "clamp=3,clampConstant=0,clampMin=-1,clampMax=0,invert", pos = "clamp=3,clampConstant=0,clampMin=0,clampMax=1";
        var c = new Controls()
            .Button("buttonSouth", KEYCODE_BUTTON_A).Button("buttonEast", KEYCODE_BUTTON_B)
            .Button("buttonWest", KEYCODE_BUTTON_X).Button("buttonNorth", KEYCODE_BUTTON_Y)
            .Button("leftShoulder", KEYCODE_BUTTON_L1).Button("rightShoulder", KEYCODE_BUTTON_R1)
            .Button("leftStickPress", KEYCODE_BUTTON_THUMBL).Button("rightStickPress", KEYCODE_BUTTON_THUMBR)
            .Button("start", KEYCODE_BUTTON_START).Button("select", KEYCODE_BUTTON_SELECT)
            .Item("leftTrigger", ("offset", Axis(AXIS_BRAKE)), ("format", "FLT"), ("sizeInBits", 32), ("parameters", "clamp=1,clampMin=0,clampMax=1"))
            .Item("rightTrigger", ("offset", Axis(AXIS_GAS)), ("format", "FLT"), ("sizeInBits", 32), ("parameters", "clamp=1,clampMin=0,clampMax=1"))
            .Item("leftStick", ("offset", Axis(AXIS_X)), ("format", "VEC2"), ("sizeInBits", 64))
            .Item("leftStick/x", ("offset", 0), ("format", "FLT"), ("sizeInBits", 32))
            .Item("leftStick/y", ("offset", 4 * (AXIS_Y - AXIS_X)), ("format", "FLT"), ("sizeInBits", 32), ("parameters", "invert"))
            .Item("leftStick/up", ("parameters", up)).Item("leftStick/down", ("parameters", down))
            .Item("rightStick", ("offset", Axis(AXIS_Z)), ("format", "VEC2"), ("sizeInBits", 32 * (AXIS_RZ - AXIS_Z + 1)))
            .Item("rightStick/x", ("offset", 0), ("format", "FLT"), ("sizeInBits", 32))
            .Item("rightStick/y", ("offset", 4 * (AXIS_RZ - AXIS_Z)), ("format", "FLT"), ("sizeInBits", 32), ("parameters", "invert"))
            .Item("rightStick/up", ("parameters", up)).Item("rightStick/down", ("parameters", down))
            .Item("dpad", ("offset", Axis(AXIS_HAT_X)), ("format", "VEC2"), ("sizeInBits", 64))
            .Item("dpad/right", ("offset", 0), ("bit", 0), ("format", "FLT"), ("sizeInBits", 32), ("parameters", pos))
            .Item("dpad/left", ("offset", 0), ("bit", 0), ("format", "FLT"), ("sizeInBits", 32), ("parameters", neg))
            .Item("dpad/down", ("offset", 4 * (AXIS_HAT_Y - AXIS_HAT_X)), ("bit", 0), ("format", "FLT"), ("sizeInBits", 32), ("parameters", pos))
            .Item("dpad/up", ("offset", 4 * (AXIS_HAT_Y - AXIS_HAT_X)), ("bit", 0), ("format", "FLT"), ("sizeInBits", 32), ("parameters", neg));
        return "{" + Q("name") + ":" + Q(GamepadLayout) + "," + Q("extend") + ":" + Q("Gamepad") + "," + Q("format") + ":" + Q("AGC ") + "," +
               Q("displayName") + ":" + Q("Android Gamepad") + "," +
               Q("device") + ":{" + Q("interface") + ":" + Q("Android") + "," + Marker + "}," +
               Q("controls") + ":" + c + "}";
    }

    static string Xbox() =>
        "{" + Q("name") + ":" + Q(XboxLayout) + "," + Q("extend") + ":" + Q(GamepadLayout) + "," +
        Q("type") + ":" + Q("UnityEngine.InputSystem.XInput.XInputController, Unity.InputSystem") + "," +
        Q("displayName") + ":" + Q("Xbox Controller") + "," +
        Q("device") + ":{" + Q("interface") + ":" + Q("Android") + "," + Marker + "," +
        Q("capabilities") + ":[{" + Q("path") + ":" + Q("vendorId") + "," + Q("value") + ":" + Q("1118") + "}]}}";   // 0x045E
}
