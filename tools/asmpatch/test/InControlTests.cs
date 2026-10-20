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
}
