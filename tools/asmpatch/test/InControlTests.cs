// The survey's InControl Android profiles, on fake profile classes made with Cecil: an old-style
// profile (constructor: IncludePlatforms / JoystickNames / LastResortRegex), a new-style one
// (Define(): IncludePlatforms, a matcher's NameLiteral / NamePattern) and a desktop-only one.
using Mono.Cecil;
using Mono.Cecil.Cil;

namespace AsmPatch.Tests;

static partial class TestMain
{
    static void FakeInControl(string dir)
    {
        var m = NewModule("Assembly-CSharp");
        var str = m.TypeSystem.String;
        TypeDefinition Class(string ns, string name, TypeReference baseType, TypeAttributes extra = 0)
        {
            var t = new TypeDefinition(ns, name, TypeAttributes.Public | TypeAttributes.BeforeFieldInit | extra, baseType);
            m.Types.Add(t);
            return t;
        }
        var mgr = Class("InControl", "InputManager", m.TypeSystem.Object);
        var baseProfile = Class("InControl", "UnityInputDeviceProfile", m.TypeSystem.Object, TypeAttributes.Abstract);
        var arr = new ArrayType(str);
        var joyNames = new FieldDefinition("JoystickNames", FieldAttributes.Public, arr);
        var lastResort = new FieldDefinition("LastResortRegex", FieldAttributes.Public, str);
        baseProfile.Fields.Add(joyNames); baseProfile.Fields.Add(lastResort);
        MethodDefinition Setter(TypeDefinition t, string name, TypeReference type)
        {
            var f = new FieldDefinition("_" + name, FieldAttributes.Private, type);
            t.Fields.Add(f);
            var s = new MethodDefinition("set_" + name, MethodAttributes.Public | MethodAttributes.SpecialName | MethodAttributes.HideBySig, m.TypeSystem.Void);
            s.Parameters.Add(new ParameterDefinition(type));
            var il = s.Body.GetILProcessor();
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldarg_1); il.Emit(OpCodes.Stfld, f); il.Emit(OpCodes.Ret);
            t.Methods.Add(s);
            return s;
        }
        var setName = Setter(baseProfile, "Name", str);
        var setInclude = Setter(baseProfile, "IncludePlatforms", arr);
        var baseCtor = Ctor(m, ObjectCtor(m));
        baseProfile.Methods.Add(baseCtor);
        var define = new MethodDefinition("Define", MethodAttributes.Public | MethodAttributes.Virtual | MethodAttributes.NewSlot | MethodAttributes.HideBySig, m.TypeSystem.Void);
        define.Body.GetILProcessor().Emit(OpCodes.Ret);
        baseProfile.Methods.Add(define);
        // a matcher struct with NameLiteral / NamePattern fields (new style)
        var matcher = Class("InControl", "InputDeviceMatcher", new TypeReference("System", "ValueType", m, m.TypeSystem.CoreLibrary), TypeAttributes.Sealed | TypeAttributes.SequentialLayout);
        var literal = new FieldDefinition("NameLiteral", FieldAttributes.Public, str);
        var pattern = new FieldDefinition("NamePattern", FieldAttributes.Public, str);
        matcher.Fields.Add(literal); matcher.Fields.Add(pattern);

        void StoreArray(ILProcessor il, params string[] items)
        {
            il.Emit(OpCodes.Ldc_I4, items.Length); il.Emit(OpCodes.Newarr, str);
            for (int i = 0; i < items.Length; i++) { il.Emit(OpCodes.Dup); il.Emit(OpCodes.Ldc_I4, i); il.Emit(OpCodes.Ldstr, items[i]); il.Emit(OpCodes.Stelem_Ref); }
        }
        // old style: everything in the constructor
        var old = Class("InControl", "Xbox360AndroidProfile", baseProfile);
        var c = new MethodDefinition(".ctor", MethodAttributes.Public | MethodAttributes.HideBySig | MethodAttributes.SpecialName | MethodAttributes.RTSpecialName, m.TypeSystem.Void);
        var cil = c.Body.GetILProcessor();
        cil.Emit(OpCodes.Ldarg_0); cil.Emit(OpCodes.Call, baseCtor);
        cil.Emit(OpCodes.Ldarg_0); cil.Emit(OpCodes.Ldstr, "XBox 360 Controller"); cil.Emit(OpCodes.Call, setName);
        cil.Emit(OpCodes.Ldarg_0); StoreArray(cil, "Android"); cil.Emit(OpCodes.Call, setInclude);
        cil.Emit(OpCodes.Ldarg_0); StoreArray(cil, "Microsoft X-Box 360 pad", "Generic X-Box pad"); cil.Emit(OpCodes.Stfld, joyNames);
        cil.Emit(OpCodes.Ldarg_0); cil.Emit(OpCodes.Ldstr, "360"); cil.Emit(OpCodes.Stfld, lastResort);
        cil.Emit(OpCodes.Ret);
        old.Methods.Add(c);
        // new style: Define(), a matcher
        var neu = Class("InControl", "PlayStation4AndroidProfile", baseProfile);
        neu.Methods.Add(Ctor(m, baseCtor));
        var d = new MethodDefinition("Define", MethodAttributes.Public | MethodAttributes.Virtual | MethodAttributes.HideBySig, m.TypeSystem.Void);
        var v = new VariableDefinition(matcher);
        d.Body.Variables.Add(v);
        var dil = d.Body.GetILProcessor();
        dil.Emit(OpCodes.Ldarg_0); dil.Emit(OpCodes.Ldstr, "PlayStation 4 Controller"); dil.Emit(OpCodes.Call, setName);
        dil.Emit(OpCodes.Ldarg_0); StoreArray(dil, "Android"); dil.Emit(OpCodes.Call, setInclude);
        dil.Emit(OpCodes.Ldloca, v); dil.Emit(OpCodes.Initobj, matcher);
        dil.Emit(OpCodes.Ldloca, v); dil.Emit(OpCodes.Ldstr, "Sony Interactive Entertainment Wireless Controller"); dil.Emit(OpCodes.Stfld, literal);
        dil.Emit(OpCodes.Ldloca, v); dil.Emit(OpCodes.Ldstr, "Wireless Controller$"); dil.Emit(OpCodes.Stfld, pattern);
        dil.Emit(OpCodes.Ret);
        neu.Methods.Add(d);
        // desktop only
        var win = Class("InControl", "Xbox360WinProfile", baseProfile);
        var wc = new MethodDefinition(".ctor", MethodAttributes.Public | MethodAttributes.HideBySig | MethodAttributes.SpecialName | MethodAttributes.RTSpecialName, m.TypeSystem.Void);
        var wil = wc.Body.GetILProcessor();
        wil.Emit(OpCodes.Ldarg_0); wil.Emit(OpCodes.Call, baseCtor);
        wil.Emit(OpCodes.Ldarg_0); StoreArray(wil, "Windows"); wil.Emit(OpCodes.Call, setInclude);
        wil.Emit(OpCodes.Ldarg_0); StoreArray(wil, "Controller (XBOX 360 For Windows)"); wil.Emit(OpCodes.Stfld, joyNames);
        wil.Emit(OpCodes.Ret);
        win.Methods.Add(wc);
        m.Write(Path.Combine(dir, "Assembly-CSharp.dll"));
    }

    static void InControlTests()
    {
        var d = Dir("incontrol");
        FakeInControl(d);
        var game = new Game(d);
        try
        {
            var p = Survey.InControlAndroidProfiles(game);
            Check(p.Count == 2 && p[0].type == "Xbox360AndroidProfile" && p[1].type == "PlayStation4AndroidProfile",
                  "InControl: the two Android profiles found (Xbox first), the Windows one left out");
            Check(p[0].title == "XBox 360 Controller" && p[0].names.SequenceEqual(new[] { "Microsoft X-Box 360 pad", "Generic X-Box pad" }) &&
                  p[0].regex.SequenceEqual(new[] { "360" }), "InControl: old-style profile: name, joystick names, last-resort pattern");
            Check(p[1].title == "PlayStation 4 Controller" && p[1].names.SequenceEqual(new[] { "Sony Interactive Entertainment Wireless Controller" }) &&
                  p[1].regex.SequenceEqual(new[] { "Wireless Controller$" }), "InControl: new-style profile (Define, a matcher's NameLiteral / NamePattern)");
        }
        finally { game.Dispose(); }
        var w = new StringWriter();
        var old = Console.Out;
        Console.SetOut(w);
        var g2 = new Game(d);
        try { Survey.Run(g2); } finally { g2.Dispose(); Console.SetOut(old); }
        Check(w.ToString().Contains("UA_PAD_NAME=\"Microsoft X-Box 360 pad\""), "InControl: the survey suggests the Xbox 360 profile's name for UA_PAD_NAME");
    }

    /// <summary>A game's InControl actions with default pad bindings: Left and Right on the left stick only
    /// (Right through a DeviceBindingSource), SelectLeft on the stick and the d-pad, Aim on the right stick
    /// only, and (withDpadOnly) GunUp on the d-pad only, the InputControlType enum numbered unlike
    /// InControl's; with allBound every button a handheld without sticks has is bound, and only R1's
    /// action (Fire) is also on another one (Action1).</summary>
    static void FakeBindings(string dir, bool withDpadOnly, bool allBound = false)
    {
        var m = NewModule("Assembly-CSharp-firstpass");
        var ict = Enum(m, "InControl", "InputControlType", ("None", 0), ("DPadLeft", 3), ("LeftStickLeft", 13), ("LeftStickRight", 14),
                       ("RightStickLeft", 17), ("DPadUp", 21), ("LeftStickButton", 22), ("Action1", 30), ("Action2", 31), ("Action3", 32),
                       ("Action4", 33), ("LeftBumper", 40), ("RightBumper", 41), ("LeftTrigger", 42), ("RightTrigger", 43), ("Back", 50));
        var src = new TypeDefinition("InControl", "BindingSource", TypeAttributes.Public | TypeAttributes.Abstract, m.TypeSystem.Object);
        m.Types.Add(src);
        var dev = new TypeDefinition("InControl", "DeviceBindingSource", TypeAttributes.Public, src);
        var devCtor = new MethodDefinition(".ctor", MethodAttributes.Public | MethodAttributes.HideBySig | MethodAttributes.SpecialName | MethodAttributes.RTSpecialName, m.TypeSystem.Void);
        devCtor.Parameters.Add(new ParameterDefinition(ict));
        devCtor.Body.GetILProcessor().Emit(OpCodes.Ret);
        dev.Methods.Add(devCtor);
        m.Types.Add(dev);
        var action = new TypeDefinition("InControl", "PlayerAction", TypeAttributes.Public, m.TypeSystem.Object);
        MethodDefinition Add(TypeReference p)
        {
            var a = new MethodDefinition("AddDefaultBinding", MethodAttributes.Public | MethodAttributes.HideBySig, m.TypeSystem.Void);
            a.Parameters.Add(new ParameterDefinition(p));
            a.Body.GetILProcessor().Emit(OpCodes.Ret);
            action.Methods.Add(a);
            return a;
        }
        var addControl = Add(ict);
        var addSource = Add(src);
        m.Types.Add(action);
        var set = new TypeDefinition("", "GameActions", TypeAttributes.Public, m.TypeSystem.Object);
        var fields = new Dictionary<string, FieldDefinition>();
        foreach (var n in new[] { "Left", "Right", "SelectLeft", "GunUp", "Fire", "Aim", "Jump", "Use", "Map", "Menu", "Dodge", "Item", "Shield" })
            set.Fields.Add(fields[n] = new FieldDefinition(n, FieldAttributes.Public, action));
        var c = new MethodDefinition(".ctor", MethodAttributes.Public | MethodAttributes.HideBySig | MethodAttributes.SpecialName | MethodAttributes.RTSpecialName, m.TypeSystem.Void);
        var il = c.Body.GetILProcessor();
        void Bind(string f, int control) { il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, fields[f]); il.Emit(OpCodes.Ldc_I4_S, (sbyte)control); il.Emit(OpCodes.Callvirt, addControl); }
        Bind("Left", 13);
        il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, fields["Right"]); il.Emit(OpCodes.Ldc_I4_S, (sbyte)14);
        il.Emit(OpCodes.Newobj, devCtor); il.Emit(OpCodes.Callvirt, addSource);
        Bind("SelectLeft", 13); Bind("SelectLeft", 3);
        if (withDpadOnly) Bind("GunUp", 21);
        Bind("Fire", 30);
        Bind("Aim", 17);
        if (allBound)
        {
            Bind("Jump", 31); Bind("Use", 32); Bind("Map", 33); Bind("Menu", 50); Bind("Dodge", 42); Bind("Item", 43);
            Bind("Shield", 40); Bind("Fire", 41);
        }
        il.Emit(OpCodes.Ret);
        set.Methods.Add(c);
        m.Types.Add(set);
        // InControl's own example, compiled into the game: left out of the survey
        var ex = new TypeDefinition("BindingsExample", "PlayerActions", TypeAttributes.Public, m.TypeSystem.Object);
        var exLeft = new FieldDefinition("Left", FieldAttributes.Public, action);
        ex.Fields.Add(exLeft);
        var ec = new MethodDefinition("CreateWithDefaultBindings", MethodAttributes.Public | MethodAttributes.Static | MethodAttributes.HideBySig, m.TypeSystem.Void);
        var eil = ec.Body.GetILProcessor();
        eil.Emit(OpCodes.Ldnull); eil.Emit(OpCodes.Ldfld, exLeft); eil.Emit(OpCodes.Ldc_I4_3); eil.Emit(OpCodes.Callvirt, addControl);
        eil.Emit(OpCodes.Ret);
        ex.Methods.Add(ec);
        m.Types.Add(ex);
        m.Write(Path.Combine(dir, "Assembly-CSharp-firstpass.dll"));
    }

    static void InControlBindingsTests()
    {
        foreach (var dpadOnly in new[] { true, false })
        {
            var d = Dir("incontrol-bindings-" + dpadOnly);
            FakeBindings(d, dpadOnly);
            var game = new Game(d);
            try
            {
                var b = Survey.InControlBindings(game);
                Check(b.Contains(("GameActions.Left", "LeftStickLeft")) && b.Contains(("GameActions.Right", "LeftStickRight")) &&
                      b.Contains(("GameActions.SelectLeft", "DPadLeft")) && b.Contains(("GameActions.Fire", "Action1")) &&
                      b.Contains(("GameActions.Aim", "RightStickLeft")) && b.Count == (dpadOnly ? 7 : 6),
                      $"InControl bindings: actions and controls read by name (also through a DeviceBindingSource), d-pad-only action {dpadOnly}");
            }
            finally { game.Dispose(); }
            var w = new StringWriter();
            var old = Console.Out;
            Console.SetOut(w);
            var g2 = new Game(d);
            try { Survey.Run(g2); } finally { g2.Dispose(); Console.SetOut(old); }
            var o = w.ToString();
            Check(o.Contains("the left stick alone (no d-pad binding): GameActions.Left, GameActions.Right"), "InControl bindings: the stick-only actions listed");
            Check(dpadOnly ? o.Contains("-> UA_DPAD_STICK=auto (") && o.Contains("own bindings GameActions.GunUp need a UA_DPAD_SWAP")
                           : o.Contains("-> UA_DPAD_STICK=auto,both ("),
                  dpadOnly ? "InControl bindings: a d-pad-only action -> UA_DPAD_STICK=auto and UA_DPAD_SWAP" : "InControl bindings: no d-pad-only action -> UA_DPAD_STICK=auto,both");
            Check(o.Contains("    every binding, by control:\n      DPad" + (dpadOnly ? "Up: GameActions.GunUp\n      DPad" : "") + "Left: GameActions.SelectLeft\n") &&
                  o.Contains("\n      RightStickLeft: GameActions.Aim\n      Action1: GameActions.Fire\n"),
                  "InControl bindings: every binding listed by control, in a handheld's order");
            Check(o.Contains("the right stick alone (no other binding): GameActions.Aim\n  -> a handheld without a right stick cannot do those"),
                  "InControl bindings: a right-stick-only action noted");
            Check(o.Contains("-> UA_DPAD_SWAP=select (Select: the game binds nothing to it") == dpadOnly,
                  "InControl bindings: the swap button is one the game binds nothing to (only with d-pad-only actions)");
        }
        // every button bound: the swap button is the one whose action another button also does
        var da = Dir("incontrol-bindings-all");
        FakeBindings(da, true, allBound: true);
        var w2 = new StringWriter();
        var old2 = Console.Out;
        Console.SetOut(w2);
        var g3 = new Game(da);
        try { Survey.Run(g3); } finally { g3.Dispose(); Console.SetOut(old2); }
        Check(w2.ToString().Contains("-> UA_DPAD_SWAP=r1 (R1: the game binds every button a handheld without sticks has; R1 only GameActions.Fire, which other buttons also do"),
              "InControl bindings: every button bound -> the swap button whose actions are also elsewhere");
    }
}
