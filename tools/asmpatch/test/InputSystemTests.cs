// inputsystem-android tests on a fake Unity.InputSystem made with Cecil (no package code): an
// InputSystem class whose PerformDefaultPluginInitialization logs and may return early, and a
// RegisterLayout(string, string, InputDeviceMatcher?) that logs what it gets. Patched, loaded
// and called; the layouts it registers are parsed and their controls checked against the
// Android player's state format.
using System.Runtime.Loader;
using System.Text.Json;
using Mono.Cecil;
using Mono.Cecil.Cil;
using AssemblyName = System.Reflection.AssemblyName;

namespace AsmPatch.Tests;

static partial class TestMain
{
    /// <summary>Unity.InputSystem.dll stand-in. android: a type in UnityEngine.InputSystem.Android
    /// (an Android build); xinput: an XInputController class; init/register: those methods exist.</summary>
    static void FakeInputSystem(string dir, bool android = false, bool xinput = true, bool init = true, bool register = true)
    {
        var m = NewModule("Unity.InputSystem");
        var str = m.TypeSystem.String;
        var matcher = new TypeDefinition("UnityEngine.InputSystem.Layouts", "InputDeviceMatcher",
            TypeAttributes.Public | TypeAttributes.Sealed | TypeAttributes.SequentialLayout | TypeAttributes.BeforeFieldInit,
            new TypeReference("System", "ValueType", m, m.TypeSystem.CoreLibrary));
        matcher.Fields.Add(new FieldDefinition("m_Patterns", FieldAttributes.Private, m.TypeSystem.Object));
        m.Types.Add(matcher);
        var nullableDef = new TypeReference("System", "Nullable`1", m, m.TypeSystem.CoreLibrary, true);
        nullableDef.GenericParameters.Add(new GenericParameter(nullableDef));
        var nullable = new GenericInstanceType(nullableDef);
        nullable.GenericArguments.Add(matcher);
        var hasValue = new MethodReference("get_HasValue", m.TypeSystem.Boolean, nullable) { HasThis = true };
        var concat = new MethodReference("Concat", str, str);
        concat.Parameters.Add(new ParameterDefinition(str));
        concat.Parameters.Add(new ParameterDefinition(str));

        var sys = new TypeDefinition("UnityEngine.InputSystem", "InputSystem",
            TypeAttributes.Public | TypeAttributes.Abstract | TypeAttributes.Sealed | TypeAttributes.BeforeFieldInit, m.TypeSystem.Object);
        var log = new FieldDefinition("Log", FieldAttributes.Public | FieldAttributes.Static, str);
        var early = new FieldDefinition("EarlyOut", FieldAttributes.Public | FieldAttributes.Static, m.TypeSystem.Boolean);
        sys.Fields.Add(log); sys.Fields.Add(early);
        void Append(ILProcessor il, string text) { il.Emit(OpCodes.Ldsfld, log); il.Emit(OpCodes.Ldstr, text); il.Emit(OpCodes.Call, concat); il.Emit(OpCodes.Stsfld, log); }
        if (register)
        {
            var reg = new MethodDefinition("RegisterLayout", MethodAttributes.Public | MethodAttributes.Static | MethodAttributes.HideBySig, m.TypeSystem.Void);
            reg.Parameters.Add(new ParameterDefinition("json", ParameterAttributes.None, str));
            reg.Parameters.Add(new ParameterDefinition("name", ParameterAttributes.Optional, str));
            reg.Parameters.Add(new ParameterDefinition("matches", ParameterAttributes.Optional, nullable));
            var il = reg.Body.GetILProcessor();
            var no = il.Create(OpCodes.Nop);
            il.Emit(OpCodes.Ldsfld, log); il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Call, concat); il.Emit(OpCodes.Stsfld, log);
            Append(il, "\n");
            il.Emit(OpCodes.Ldarg_1); il.Emit(OpCodes.Brfalse, no); Append(il, "NAME\n");
            il.Append(no);
            var end = il.Create(OpCodes.Ret);
            il.Emit(OpCodes.Ldarga_S, reg.Parameters[2]); il.Emit(OpCodes.Call, hasValue); il.Emit(OpCodes.Brfalse, end); Append(il, "MATCHER\n");
            il.Append(end);
            sys.Methods.Add(reg);
            // an overload of the same name that must not be picked
            var regType = new MethodDefinition("RegisterLayout", MethodAttributes.Public | MethodAttributes.Static | MethodAttributes.HideBySig, m.TypeSystem.Void);
            regType.Parameters.Add(new ParameterDefinition(m.TypeSystem.Object));
            regType.Parameters.Add(new ParameterDefinition(str));
            regType.Parameters.Add(new ParameterDefinition(nullable));
            regType.Body.GetILProcessor().Emit(OpCodes.Ret);
            sys.Methods.Add(regType);
        }
        MethodDefinition pdi = null;
        if (init)
        {
            pdi = new MethodDefinition("PerformDefaultPluginInitialization", MethodAttributes.Private | MethodAttributes.Static | MethodAttributes.HideBySig, m.TypeSystem.Void);
            var il = pdi.Body.GetILProcessor();
            var ret1 = il.Create(OpCodes.Ret);
            Append(il, "plugins\n");
            il.Emit(OpCodes.Ldsfld, early); il.Emit(OpCodes.Brtrue_S, ret1);
            Append(il, "hid\n");
            il.Emit(OpCodes.Ret);
            il.Append(ret1);
            sys.Methods.Add(pdi);
        }
        var entry = new MethodDefinition("Init", MethodAttributes.Public | MethodAttributes.Static | MethodAttributes.HideBySig, m.TypeSystem.Void);
        var eil = entry.Body.GetILProcessor();
        if (pdi != null) eil.Emit(OpCodes.Call, pdi);
        eil.Emit(OpCodes.Ret);
        sys.Methods.Add(entry);
        m.Types.Add(sys);
        if (xinput) m.Types.Add(new TypeDefinition("UnityEngine.InputSystem.XInput", "XInputController", TypeAttributes.Public, m.TypeSystem.Object));
        if (android) m.Types.Add(new TypeDefinition("UnityEngine.InputSystem.Android", "AndroidGamepad", TypeAttributes.Public, m.TypeSystem.Object));
        m.Write(Path.Combine(dir, "Unity.InputSystem.dll"));
    }

    static string CallInit(string dir, bool earlyOut)
    {
        var ctx = new AssemblyLoadContext("is" + Guid.NewGuid(), isCollectible: true);
        try
        {
            var asm = ctx.LoadFromStream(new MemoryStream(File.ReadAllBytes(Path.Combine(dir, "Unity.InputSystem.dll"))));
            var t = asm.GetType("UnityEngine.InputSystem.InputSystem", true);
            t.GetField("Log").SetValue(null, "");
            t.GetField("EarlyOut").SetValue(null, earlyOut);
            t.GetMethod("Init").Invoke(null, null);
            return (string)t.GetField("Log").GetValue(null);
        }
        finally { ctx.Unload(); }
    }

    static Dictionary<string, JsonElement> ControlsOf(JsonElement layout) =>
        layout.GetProperty("controls").EnumerateArray().ToDictionary(c => c.GetProperty("name").GetString(), c => c);

    static int Int(JsonElement c, string k) => c.TryGetProperty(k, out var v) ? v.GetInt32() : -1;
    static string Str(JsonElement c, string k) => c.TryGetProperty(k, out var v) ? v.GetString() : null;

    static void InputSystemTests()
    {
        var d = Dir("isys"); var o = Path.Combine(tmp, "isys-out");
        FakeInputSystem(d);
        Check(CallInit(d, false) == "plugins\nhid\n", "Input System: unpatched initialization registers nothing");
        var (st, outp) = Patch(d, o, only: new[] { "inputsystem-android" });
        Check(st == 0 && outp.Contains("ok   inputsystem-android: Unity.InputSystem.dll: UnityEngine.InputSystem.InputSystem.PerformDefaultPluginInitialization: + layouts " +
                                       InputSystemAndroid.GamepadLayout + ", " + InputSystemAndroid.XboxLayout),
              "Input System: patched, both layouts reported");
        Check(Refs(Path.Combine(o, "Unity.InputSystem.dll")).SequenceEqual(Refs(Path.Combine(d, "Unity.InputSystem.dll"))),
              "Input System: assembly references unchanged");
        foreach (var early in new[] { false, true })
        {
            var log = CallInit(o, early);
            var lines = log.Split('\n', StringSplitOptions.RemoveEmptyEntries);
            var jsons = lines.Where(l => l.StartsWith("{")).ToList();
            Check(lines[0] == "plugins" && lines.Contains("hid") == !early && jsons.Count == 2 && !log.Contains("NAME") && !log.Contains("MATCHER"),
                  $"Input System: EarlyOut={early}: the layouts are registered on every return, json only (no name, no matcher argument)");
        }
        var all = CallInit(o, false).Split('\n').Where(l => l.StartsWith("{")).Select(l => JsonDocument.Parse(l).RootElement).ToList();
        var pad = all[0];
        var dev = pad.GetProperty("device");
        Check(Str(pad, "name") == InputSystemAndroid.GamepadLayout && Str(pad, "extend") == "Gamepad" && Str(pad, "format") == "AGC " &&
              Str(dev, "interface") == "Android" && Str(dev, "deviceClass") == "AndroidGameController",
              "Input System: the gamepad layout extends Gamepad, 'AGC ' state, matched by interface Android / class AndroidGameController");
        var c = ControlsOf(pad);
        // every control of the Gamepad layout is placed in the Android state
        var gamepad = new[] { "buttonSouth", "buttonEast", "buttonWest", "buttonNorth", "leftShoulder", "rightShoulder", "leftStickPress",
                              "rightStickPress", "start", "select", "leftTrigger", "rightTrigger", "leftStick", "rightStick", "dpad" };
        Check(gamepad.All(c.ContainsKey), "Input System: every Gamepad control is laid out");
        (string, int)[] buttons = { ("buttonSouth", 96), ("buttonEast", 97), ("buttonWest", 99), ("buttonNorth", 100), ("leftShoulder", 102),
                                    ("rightShoulder", 103), ("leftStickPress", 106), ("rightStickPress", 107), ("start", 108), ("select", 109) };
        Check(buttons.All(b => Int(c[b.Item1], "offset") == 0 && Int(c[b.Item1], "bit") == b.Item2 && Str(c[b.Item1], "format") == "BIT"),
              "Input System: buttons are the bits of their Android key codes (A 96 ... Select 109)");
        int Ax(int a) => 28 + 4 * a;
        Check(Int(c["leftStick"], "offset") == Ax(0) && Int(c["leftStick/y"], "offset") == 4 && Str(c["leftStick/y"], "parameters") == "invert" &&
              Int(c["rightStick"], "offset") == Ax(11) && Int(c["rightStick"], "sizeInBits") == 128 && Int(c["rightStick/y"], "offset") == 12 &&
              Str(c["rightStick/y"], "parameters") == "invert",
              "Input System: sticks at AXIS_X/Y and AXIS_Z/RZ, y inverted");
        Check(Str(c["leftStick/up"], "parameters").Contains("invert") && !Str(c["leftStick/down"], "parameters").Contains("invert") &&
              Str(c["rightStick/up"], "parameters") == Str(c["leftStick/up"], "parameters"),
              "Input System: stick up/down read the inverted axis");
        Check(Int(c["leftTrigger"], "offset") == Ax(23) && Int(c["rightTrigger"], "offset") == Ax(22) && Str(c["leftTrigger"], "format") == "FLT",
              "Input System: triggers at AXIS_BRAKE / AXIS_GAS");
        Check(Int(c["dpad"], "offset") == Ax(15) && Int(c["dpad/left"], "offset") == 0 && Int(c["dpad/right"], "offset") == 0 &&
              Int(c["dpad/up"], "offset") == 4 && Int(c["dpad/down"], "offset") == 4 && Str(c["dpad/up"], "parameters").Contains("invert") &&
              Str(c["dpad/left"], "parameters").Contains("invert") && !Str(c["dpad/down"], "parameters").Contains("invert"),
              "Input System: d-pad from AXIS_HAT_X / HAT_Y (-1: left / up)");
        var xbox = all[1];
        var xdev = xbox.GetProperty("device");
        Check(Str(xbox, "extend") == InputSystemAndroid.GamepadLayout && Str(xbox, "type").StartsWith("UnityEngine.InputSystem.XInput.XInputController,") &&
              xdev.GetProperty("capabilities")[0].GetProperty("path").GetString() == "vendorId" &&
              xdev.GetProperty("capabilities")[0].GetProperty("value").GetString() == "1118",
              "Input System: Microsoft's pads (vendor 0x045E) are XInputControllers");

        // a second run on the patched copy: nothing to do
        var d2 = Dir("isys-again"); var o2 = Path.Combine(tmp, "isys-again-out");
        File.Copy(Path.Combine(o, "Unity.InputSystem.dll"), Path.Combine(d2, "Unity.InputSystem.dll"));
        (st, outp) = Patch(d2, o2, only: new[] { "inputsystem-android" });
        Check(st == 0 && outp.Contains("done inputsystem-android") && NoDlls(o2), "Input System: patched twice: already applied, nothing written");
        // no XInputController in this build of the package: the gamepad layout only
        var d3 = Dir("isys-noxi"); var o3 = Path.Combine(tmp, "isys-noxi-out");
        FakeInputSystem(d3, xinput: false);
        (st, outp) = Patch(d3, o3, only: new[] { "inputsystem-android" });
        Check(st == 0 && outp.Contains("+ layouts " + InputSystemAndroid.GamepadLayout + " for") && CallInit(o3, false).Split('\n').Count(l => l.StartsWith("{")) == 1,
              "Input System: without XInputController, the gamepad layout alone");
        // an Android build of the package: not applicable
        var d4 = Dir("isys-android"); var o4 = Path.Combine(tmp, "isys-android-out");
        FakeInputSystem(d4, android: true);
        (st, outp) = Patch(d4, o4, only: new[] { "inputsystem-android" });
        Check(st == 0 && outp.Contains("n/a  inputsystem-android") && outp.Contains("Android support") && NoDlls(o4), "Input System: an Android build: not applicable");
        // the methods the patch needs are missing: fails, writes nothing
        foreach (var (init, reg, what) in new[] { (false, true, "PerformDefaultPluginInitialization"), (true, false, "RegisterLayout") })
        {
            var d5 = Dir("isys-no" + what); var o5 = Path.Combine(tmp, "isys-no" + what + "-out");
            FakeInputSystem(d5, init: init, register: reg);
            (st, outp) = Patch(d5, o5, only: new[] { "inputsystem-android" });
            Check(st == 1 && outp.Contains("FAIL inputsystem-android") && outp.Contains(what) && NoDlls(o5), $"Input System: no {what}: fails, nothing written");
        }
        // a game without the package
        (st, outp) = Patch(Path.Combine(tmp, "plain"), Path.Combine(tmp, "plain-isys-out"), only: new[] { "inputsystem-android" });
        Check(st == 0 && outp.Contains("n/a  inputsystem-android: no Unity.InputSystem.dll"), "Input System: no package: not applicable");
    }
}
