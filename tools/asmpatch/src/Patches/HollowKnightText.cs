// hk-text (kind Screen, opt-in: the size of the game's small text, never the game play). Hollow Knight's words
// are TextMeshPro text (the game's own copy, namespace TMProOld) laid out for a 1920x1080 screen; a handheld
// shows that screen at 720x480 or 640x480 (0.375 or 0.33 of it), where the item and charm descriptions come
// out about 13 pixels tall (letters 5 pixels without ascenders) and the dialogue about 17. With the patch, the
// environment variable ASMPATCH_HK_TEXT chooses at run time:
//   <k>    the game's body text may grow up to k times its size (1 < k <= 4)
//   auto   k = 1024 / the screen's 16:9 width (its width, or its height times 16/9 if that is less), at most 2:
//          as big as the game draws it on a 1024-wide screen (720x480: 1.42, 640x480: 1.6, 854x480: 1.2;
//          1024x576 and up: the game as it is)
//   anything else, or unset: the game as it is.
// Body text is a TextMeshPro text (3D, the HUD's and inventory's) that wraps in a box of its own, overflows
// or pages (no ellipsis or truncation) and does not size itself: the descriptions, the dialogue, the journal,
// the map's labels, the prompts. Titles and counters (one line, no wrapping), the menus (TextMeshProUGUI and
// Unity's UI text) and the inventory's pane title in its frame ("Pane Name") keep their sizes.
// Each body text is fitted to its box by TextMeshPro's own auto size, between its own size and k times it: the
// biggest size at which the whole text fits the box as the game laid it out, so no text leaves its box or
// takes more pages than in the game, and none gets smaller. The fit runs when the text (or its size) changes
// and its result is kept: the dialogue's typewriter and the cursor's moves lay it out as before, once.
// Where the game places a text over less of the screen than its box (the inventory's, charms' and journal's
// descriptions: their boxes run past the frame, the Equip prompt, the journal's divider) the fit keeps it above a
// floor in the game's 1920x1080 layout (Floors, from the HUD camera: the HudCamera above the text), measured on
// the game's own screens; the text is laid out again at its box's full height once the size is chosen.
// The game reads and sets these texts' sizes in its own units (ChangeFontByLanguage's sizes per language): the
// fontSize property still reads the game's size and setting it sets the fit's range.
// The variable is read at the first text's Awake; "[hk-text] ..." is logged once. ASMPATCH_HK_TEXT_LOG=1 also
// logs each text when it wakes, each fit and each floor (what to look at when changing the rules).
// (TMProOld's TextContainer.rect starts at its pivot, so its yMax is the box's height; the box's top is
// corners[1], as TextMeshPro lays the text out from it.)
using Mono.Cecil;
using Mono.Cecil.Cil;
using Mono.Cecil.Rocks;

namespace AsmPatch;

sealed class HollowKnightText : Patch
{
    public override string Name => "hk-text";
    public override Kind Kind => Kind.Screen;
    public override bool OptIn => true;
    public override string Summary =>
        "Hollow Knight's body text (descriptions, dialogue, journal, map labels) up to ASMPATCH_HK_TEXT=<k>|auto times bigger, fitted to its own box by TextMeshPro's auto size: readable on a small screen, no text out of its box (opt-in)";

    public const string Variable = "ASMPATCH_HK_TEXT", LogVariable = "ASMPATCH_HK_TEXT_LOG", HelperType = "<asmpatch>HollowKnightText",
                        ScaleField = "<asmpatch>textScale", FittedField = "<asmpatch>textFitted";
    /// <summary>The GameObjects whose text never grows: the inventory's pane title, drawn in a gap of its frame.</summary>
    public static readonly string[] Kept = { "Pane Name" };
    /// <summary>(parent, name, floor): the texts kept above a floor, in the 1920x1080 layout's pixels from the top of
    /// its 16:9 frame (the frame's bottom bar starts at 951, the charms' Equip prompt at 889, the journal's divider at
    /// 468; measured at 720x480).</summary>
    public static readonly (string parent, string name, float floor)[] Floors =
    {
        ("Inv", "Text Desc", 927f), ("Inv", "Text Desc Low", 927f), ("Charms", "Text Desc", 876f),
        ("Journal", "Text Desc", 460f), ("Journal", "Text Notes", 927f),
    };
    /// <summary>The HUD camera's GameObject, above the texts with a floor; its orthographic size on a 16:9 screen
    /// is 8.4375 (1080 pixels: 64 a unit).</summary>
    public const string HudCamera = "HudCamera";
    const float LayoutUnit = 64f, LayoutMiddle = 540f;
    const int Overflow = 0, Page = 5;   // TMProOld.TextOverflowModes
    /// <summary>auto: k = Reference / the screen's 16:9 width, at most AutoMax.</summary>
    public const float Reference = 1024f, AutoMax = 2f;

    public override Result Run(Game game, bool apply)
    {
        var asm = game.Load("Assembly-CSharp");
        var m = asm?.MainModule;
        var text = m?.GetType("TMProOld.TMP_Text");
        var tmp = m?.GetType("TMProOld.TextMeshPro");
        if (text == null || tmp == null)
            return Result.NotApplicable("no Hollow Knight (TMProOld.TMP_Text, TMProOld.TextMeshPro in Assembly-CSharp)");
        var file = game.FileOf(asm);
        if (m.Types.Any(t => t.Name == HelperType))
            return new Result { Outcome = Outcome.AlreadyApplied, Details = { $"{file}: the body text's size follows {Variable}" } };

        FieldDefinition F(TypeDefinition t, string n) => t.Fields.FirstOrDefault(f => f.Name == n && !f.IsStatic);
        var f = new Fields
        {
            size = F(text, "m_fontSize"), min = F(text, "m_fontSizeMin"), max = F(text, "m_fontSizeMax"),
            minRun = F(text, "m_minFontSize"), maxRun = F(text, "m_maxFontSize"), auto = F(text, "m_enableAutoSizing"),
            fontAsset = F(text, "m_fontAsset"), overflow = F(text, "m_overflowMode"), wrap = F(text, "m_enableWordWrapping"),
            container = F(tmp, "m_textContainer"), marginHeight = F(text, "m_marginHeight"), changed = F(text, "m_havePropertiesChanged"),
        };
        if (new[] { f.size, f.min, f.max, f.minRun, f.maxRun, f.auto, f.fontAsset, f.overflow, f.wrap, f.container, f.marginHeight, f.changed }.Any(x => x == null))
            return Result.Failed($"{file}: TMProOld.TMP_Text / TextMeshPro are not as this patch knows them (their font size, auto size, overflow and box fields)");
        MethodDefinition M(TypeDefinition t, string n, int args) => t.Methods.FirstOrDefault(x => x.Name == n && !x.IsStatic && x.HasBody && x.Parameters.Count == args);
        var getSize = M(text, "get_fontSize", 0); var setSize = M(text, "set_fontSize", 1); var parse = M(text, "ParseInputText", 0);
        var awake = M(tmp, "Awake", 0); var pre = M(tmp, "OnPreRenderObject", 0);
        var gens = pre?.Body.Instructions.Where(i => i.OpCode.Code is Code.Call or Code.Callvirt && i.Operand is MethodReference c &&
                                                     c.Name == "GenerateTextMesh" && c.Parameters.Count == 0).ToList();
        if (getSize == null || setSize == null || parse == null || awake == null || gens == null || gens.Count != 1)
            return Result.Failed($"{file}: TMProOld's fontSize, ParseInputText, TextMeshPro.Awake and OnPreRenderObject (one GenerateTextMesh) are not as this patch knows them");
        var refs = new Refs(m);
        if (refs.Missing is string missing)
            return Result.Failed($"{file}: Assembly-CSharp does not reference {missing} (the patch calls it as the game does)");

        var r = new Result { Outcome = Outcome.Applied };
        r.Details.Add($"{file}: TMProOld.TextMeshPro's body text up to {Variable} times bigger, fitted to its box by its own auto size (Awake, OnPreRenderObject); fontSize in the game's units");
        if (!apply) return r;

        var ts = m.TypeSystem;
        f.scale = new FieldDefinition(ScaleField, FieldAttributes.Assembly | FieldAttributes.NotSerialized, ts.Single);
        f.fitted = new FieldDefinition(FittedField, FieldAttributes.Assembly | FieldAttributes.NotSerialized, ts.Boolean);
        f.floor = new FieldDefinition("<asmpatch>textFloor", FieldAttributes.Assembly | FieldAttributes.NotSerialized, ts.Single);
        f.saved = new FieldDefinition("<asmpatch>textHeight", FieldAttributes.Assembly | FieldAttributes.NotSerialized, ts.Single);
        foreach (var x in new[] { f.scale, f.fitted, f.floor, f.saved }) text.Fields.Add(x);
        var h = Helper(m, refs);
        var (sizeGet, sizeSet) = Accessors(m, text, f);
        // fontSize: get -> SizeGet(); set: value = SizeSet(value) first
        {
            var il = getSize.Body.GetILProcessor();
            foreach (var ret in getSize.Body.Instructions.Where(i => i.OpCode.Code == Code.Ret).ToList())
            {
                il.InsertBefore(ret, il.Create(OpCodes.Pop));
                il.InsertBefore(ret, il.Create(OpCodes.Ldarg_0));
                il.InsertBefore(ret, il.Create(OpCodes.Call, sizeGet));
            }
            getSize.Body.OptimizeMacros();
        }
        Prepend(setSize, OpCodes.Ldarg_0, OpCodes.Ldarg_1, Instruction.Create(OpCodes.Call, sizeSet), Instruction.Create(OpCodes.Starg, setSize.Parameters[0]));
        // ParseInputText: the input changed, the fit runs again
        Prepend(parse, OpCodes.Ldarg_0, OpCodes.Ldc_I4_0, Instruction.Create(OpCodes.Stfld, f.fitted));
        var grow = Grow(m, tmp, h, refs, f, sizeGet);
        var (begin, end) = Fit(m, tmp, h, refs, f, sizeGet);
        {
            var il = awake.Body.GetILProcessor();
            foreach (var ret in awake.Body.Instructions.Where(i => i.OpCode.Code == Code.Ret).ToList())
            {
                // (a branch to this ret now lands on the call: every way out of Awake)
                var call = il.Create(OpCodes.Ldarg_0);
                il.InsertBefore(ret, call);
                il.InsertBefore(ret, il.Create(OpCodes.Call, grow));
                Retarget(awake.Body, ret, call);
            }
            awake.Body.OptimizeMacros();
        }
        {
            var il = pre.Body.GetILProcessor();
            var gen = gens[0];
            il.InsertAfter(gen, il.Create(OpCodes.Call, end));
            il.InsertAfter(gen, il.Create(OpCodes.Ldarg_0));
            // (before the call: after its own this was loaded, so ahead of that load)
            var first = il.Create(OpCodes.Ldarg_0);
            var at = gen.Previous;   // ldarg.0 for GenerateTextMesh's this
            if (at == null || at.OpCode.Code is not (Code.Ldarg_0 or Code.Ldarg))
                at = gen;
            il.InsertBefore(at, first);
            il.InsertBefore(at, il.Create(OpCodes.Call, begin));
            Retarget(pre.Body, at, first);
            pre.Body.OptimizeMacros();
        }
        game.MarkChanged(asm);
        return r;
    }

    sealed class Fields
    {
        public FieldDefinition size, min, max, minRun, maxRun, auto, fontAsset, overflow, wrap, container, marginHeight, changed, scale, fitted, floor, saved;
    }

    static void Prepend(MethodDefinition md, params object[] code)
    {
        var il = md.Body.GetILProcessor();
        var first = md.Body.Instructions[0];
        foreach (var c in code)
            il.InsertBefore(first, c is OpCode op ? il.Create(op) : (Instruction)c);
        md.Body.OptimizeMacros();
    }

    /// <summary>Branches and handler ends that pointed at old now point at to.</summary>
    static void Retarget(MethodBody body, Instruction old, Instruction to)
    {
        foreach (var i in body.Instructions)
        {
            if (i == to) continue;
            if (i.Operand == old) i.Operand = to;
            else if (i.Operand is Instruction[] targets)
                for (int k = 0; k < targets.Length; k++) if (targets[k] == old) targets[k] = to;
        }
        foreach (var eh in body.ExceptionHandlers)
        {
            if (eh.TryStart == old) eh.TryStart = to;
            if (eh.TryEnd == old) eh.TryEnd = to;
            if (eh.HandlerStart == old) eh.HandlerStart = to;
            if (eh.HandlerEnd == old) eh.HandlerEnd = to;
        }
    }

    /// <summary>The Unity and system methods the patch calls, as Assembly-CSharp already references them.</summary>
    sealed class Refs
    {
        public MethodReference log, objEq, getName, getTransform, getParent, lossy, position, abs, screenW, screenH, cDefW, cDefH, cCorners, cMargins;
        public FieldReference vecY, vec4Y;
        public string Missing;

        public Refs(ModuleDefinition m)
        {
            var all = m.GetMemberReferences().OfType<MethodReference>().ToList();
            MethodReference R(string type, string name, int n, bool hasThis, MetadataType? param = null)
            {
                var x = all.FirstOrDefault(c => c.Name == name && c.DeclaringType.FullName == type && c.Parameters.Count == n && c.HasThis == hasThis &&
                                                (param == null || c.Parameters[0].ParameterType.MetadataType == param));
                if (x == null) Missing ??= $"{type}::{name}";
                return x;
            }
            log = R("UnityEngine.Debug", "Log", 1, false, MetadataType.Object);
            objEq = R("UnityEngine.Object", "op_Equality", 2, false);
            getName = R("UnityEngine.Object", "get_name", 0, true);
            getTransform = R("UnityEngine.Component", "get_transform", 0, true);
            getParent = R("UnityEngine.Transform", "get_parent", 0, true);
            lossy = R("UnityEngine.Transform", "get_lossyScale", 0, true);
            position = R("UnityEngine.Transform", "get_position", 0, true);
            abs = R("UnityEngine.Mathf", "Abs", 1, false, MetadataType.Single);
            screenW = R("UnityEngine.Screen", "get_width", 0, false);
            screenH = R("UnityEngine.Screen", "get_height", 0, false);
            var vec = m.GetTypeReferences().FirstOrDefault(t => t.FullName == "UnityEngine.Vector3");
            if (vec != null) vecY = new FieldReference("y", m.TypeSystem.Single, vec);
            else Missing ??= "UnityEngine.Vector3";
            var vec4 = m.GetTypeReferences().FirstOrDefault(t => t.FullName == "UnityEngine.Vector4");
            if (vec4 != null) vec4Y = new FieldReference("y", m.TypeSystem.Single, vec4);
            else Missing ??= "UnityEngine.Vector4";
            var tc = m.GetType("TMProOld.TextContainer");
            MethodReference D(string name)
            {
                var x = tc?.Methods.FirstOrDefault(c => c.Name == name && !c.IsStatic && c.Parameters.Count == 0);
                if (x == null) Missing ??= $"TMProOld.TextContainer::{name}";
                return x;
            }
            cDefW = D("get_isDefaultWidth"); cDefH = D("get_isDefaultHeight"); cCorners = D("get_corners"); cMargins = D("get_margins");
        }
    }

    sealed class HelperMethods
    {
        public MethodDefinition read, path;
        public FieldDefinition scale, log;
    }

    /// <summary>The helper class: static void Read() (the variables, once); static float Factor(string v, int w, int h)
    /// (the factor a value asks for on that screen, 0: none; the tests call it); static string Path(Transform)
    /// (the text's GameObject and up to four parents, for the log).</summary>
    static HelperMethods Helper(ModuleDefinition m, Refs refs)
    {
        var ts = m.TypeSystem;
        var t = new TypeDefinition("", HelperType, TypeAttributes.NotPublic | TypeAttributes.Abstract | TypeAttributes.Sealed |
                                                   TypeAttributes.BeforeFieldInit | TypeAttributes.Class, ts.Object);
        m.Types.Add(t);
        TypeReference Sys(string n, bool value = false) => new TypeReference("System", n, m, ts.CoreLibrary, value);
        var strT = Sys("String");
        var env = new MethodReference("GetEnvironmentVariable", ts.String, Sys("Environment"));
        env.Parameters.Add(new ParameterDefinition(ts.String));
        var equals = new MethodReference("op_Equality", ts.Boolean, strT);
        equals.Parameters.Add(new ParameterDefinition(ts.String)); equals.Parameters.Add(new ParameterDefinition(ts.String));
        var styles = new TypeReference("System.Globalization", "NumberStyles", m, ts.CoreLibrary, true);
        var provider = new TypeReference("System", "IFormatProvider", m, ts.CoreLibrary);
        var culture = new TypeReference("System.Globalization", "CultureInfo", m, ts.CoreLibrary);
        var invariant = new MethodReference("get_InvariantCulture", culture, culture);
        var tryParse = new MethodReference("TryParse", ts.Boolean, Sys("Single", true));
        tryParse.Parameters.Add(new ParameterDefinition(ts.String)); tryParse.Parameters.Add(new ParameterDefinition(styles));
        tryParse.Parameters.Add(new ParameterDefinition(provider)); tryParse.Parameters.Add(new ParameterDefinition(new ByReferenceType(ts.Single)));
        var concat3 = new MethodReference("Concat", ts.String, strT);
        for (int i = 0; i < 3; i++) concat3.Parameters.Add(new ParameterDefinition(ts.String));
        const MethodAttributes Static = MethodAttributes.Assembly | MethodAttributes.Static | MethodAttributes.HideBySig;
        const int Float = 7 | 0x20 | 0x80;   // NumberStyles.Float (leading/trailing white, sign, decimal point, exponent)

        var h = new HelperMethods
        {
            scale = new FieldDefinition("scale", FieldAttributes.Assembly | FieldAttributes.Static, ts.Single),
            log = new FieldDefinition("log", FieldAttributes.Assembly | FieldAttributes.Static, ts.Boolean),
        };
        var state = new FieldDefinition("state", FieldAttributes.Private | FieldAttributes.Static, ts.Int32);
        foreach (var x in new[] { h.scale, h.log, state }) t.Fields.Add(x);

        // static float Factor(string v, int w, int h): "auto": Reference / min(w, h * 16 / 9), at most AutoMax;
        // else v as a number; 0 unless the result is over 1.01 and at most 4 (NaN fails)
        var factor = new MethodDefinition("Factor", Static, ts.Single);
        factor.Parameters.Add(new ParameterDefinition("v", ParameterAttributes.None, ts.String));
        factor.Parameters.Add(new ParameterDefinition("w", ParameterAttributes.None, ts.Int32));
        factor.Parameters.Add(new ParameterDefinition("h", ParameterAttributes.None, ts.Int32));
        t.Methods.Add(factor);
        {
            var k = new VariableDefinition(ts.Single); var w16 = new VariableDefinition(ts.Single);
            factor.Body.Variables.Add(k); factor.Body.Variables.Add(w16);
            factor.Body.InitLocals = true;
            var il = factor.Body.GetILProcessor();
            var none = il.Create(OpCodes.Ldc_R4, 0f);
            var number = il.Create(OpCodes.Ldarg_0);
            var check = il.Create(OpCodes.Ldloc, k);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Brfalse, none);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldstr, "auto"); il.Emit(OpCodes.Call, equals); il.Emit(OpCodes.Brfalse, number);
            //   w16 = h * 16 / 9; if (w < w16) w16 = w; if (w16 <= 0) none; k = min(Reference / w16, AutoMax)
            var haveW16 = il.Create(OpCodes.Ldloc, w16);
            il.Emit(OpCodes.Ldarg_2); il.Emit(OpCodes.Conv_R4); il.Emit(OpCodes.Ldc_R4, 16f / 9f); il.Emit(OpCodes.Mul); il.Emit(OpCodes.Stloc, w16);
            il.Emit(OpCodes.Ldarg_1); il.Emit(OpCodes.Conv_R4); il.Emit(OpCodes.Ldloc, w16); il.Emit(OpCodes.Bge_Un, haveW16);
            il.Emit(OpCodes.Ldarg_1); il.Emit(OpCodes.Conv_R4); il.Emit(OpCodes.Stloc, w16);
            il.Append(haveW16); il.Emit(OpCodes.Ldc_R4, 0f); il.Emit(OpCodes.Ble_Un, none);
            il.Emit(OpCodes.Ldc_R4, Reference); il.Emit(OpCodes.Ldloc, w16); il.Emit(OpCodes.Div); il.Emit(OpCodes.Stloc, k);
            il.Emit(OpCodes.Ldloc, k); il.Emit(OpCodes.Ldc_R4, AutoMax); il.Emit(OpCodes.Ble_Un, check);
            il.Emit(OpCodes.Ldc_R4, AutoMax); il.Emit(OpCodes.Stloc, k); il.Emit(OpCodes.Br, check);
            il.Append(number); il.Emit(OpCodes.Ldc_I4, Float); il.Emit(OpCodes.Call, invariant); il.Emit(OpCodes.Ldloca, k); il.Emit(OpCodes.Call, tryParse);
            il.Emit(OpCodes.Brfalse, none);
            il.Append(check); il.Emit(OpCodes.Ldc_R4, 1.01f); il.Emit(OpCodes.Ble_Un, none);
            il.Emit(OpCodes.Ldloc, k); il.Emit(OpCodes.Ldc_R4, 4f); il.Emit(OpCodes.Bgt_Un, none);
            il.Emit(OpCodes.Ldloc, k); il.Emit(OpCodes.Ret);
            il.Append(none); il.Emit(OpCodes.Ret);
            factor.Body.OptimizeMacros();
        }
        // static string Path(Transform t): "grandparent/parent/name", up to four parents
        h.path = new MethodDefinition("Path", Static, ts.String);
        h.path.Parameters.Add(new ParameterDefinition("t", ParameterAttributes.None, refs.getParent.ReturnType));
        t.Methods.Add(h.path);
        {
            var s = new VariableDefinition(ts.String); var n = new VariableDefinition(ts.Int32);
            h.path.Body.Variables.Add(s); h.path.Body.Variables.Add(n);
            h.path.Body.InitLocals = true;
            var il = h.path.Body.GetILProcessor();
            var loop = il.Create(OpCodes.Ldarg_0);
            var done = il.Create(OpCodes.Ldloc, s);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Callvirt, refs.getName); il.Emit(OpCodes.Stloc, s);
            il.Append(loop); il.Emit(OpCodes.Callvirt, refs.getParent); il.Emit(OpCodes.Starg, h.path.Parameters[0]);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldnull); il.Emit(OpCodes.Call, refs.objEq); il.Emit(OpCodes.Brtrue, done);
            il.Emit(OpCodes.Ldloc, n); il.Emit(OpCodes.Ldc_I4_4); il.Emit(OpCodes.Bge, done);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Callvirt, refs.getName); il.Emit(OpCodes.Ldstr, "/"); il.Emit(OpCodes.Ldloc, s);
            il.Emit(OpCodes.Call, concat3); il.Emit(OpCodes.Stloc, s);
            il.Emit(OpCodes.Ldloc, n); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Add); il.Emit(OpCodes.Stloc, n);
            il.Emit(OpCodes.Br, loop);
            il.Append(done); il.Emit(OpCodes.Ret);
            h.path.Body.OptimizeMacros();
        }
        // static void Read(): once: log = LogVariable == "1"; scale = Factor(Variable, Screen.width, Screen.height); one line when on
        h.read = new MethodDefinition("Read", Static, ts.Void);
        t.Methods.Add(h.read);
        {
            var il = h.read.Body.GetILProcessor();
            var ret = il.Create(OpCodes.Ret);
            il.Emit(OpCodes.Ldsfld, state); il.Emit(OpCodes.Brtrue, ret);
            il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Stsfld, state);
            il.Emit(OpCodes.Ldstr, LogVariable); il.Emit(OpCodes.Call, env); il.Emit(OpCodes.Ldstr, "1"); il.Emit(OpCodes.Call, equals);
            il.Emit(OpCodes.Stsfld, h.log);
            il.Emit(OpCodes.Ldstr, Variable); il.Emit(OpCodes.Call, env); il.Emit(OpCodes.Call, refs.screenW); il.Emit(OpCodes.Call, refs.screenH);
            il.Emit(OpCodes.Call, factor); il.Emit(OpCodes.Stsfld, h.scale);
            il.Emit(OpCodes.Ldsfld, h.scale); il.Emit(OpCodes.Ldc_R4, 0f); il.Emit(OpCodes.Ble_Un, ret);
            LogLine(m, il, refs, S("[hk-text] body text up to "), (x => x.Emit(OpCodes.Ldsfld, h.scale), ts.Single), S(" times its size, fitted to its box ("),
                    (x => x.Emit(OpCodes.Call, refs.screenW), ts.Int32), S("x"), (x => x.Emit(OpCodes.Call, refs.screenH), ts.Int32),
                    S(", " + Variable + "="), (x => { x.Emit(OpCodes.Ldstr, Variable); x.Emit(OpCodes.Call, env); }, null), S(")"));
            il.Append(ret);
            h.read.Body.OptimizeMacros();
        }
        return h;
    }

    /// <summary>Emits Debug.Log(string.Concat(new object[] { items... })): each item pushes one value and names
    /// its type when it is a value type (boxed).</summary>
    static void LogLine(ModuleDefinition m, ILProcessor il, Refs refs, params (Action<ILProcessor> push, TypeReference box)[] items)
    {
        var ts = m.TypeSystem;
        var concat = new MethodReference("Concat", ts.String, new TypeReference("System", "String", m, ts.CoreLibrary));
        concat.Parameters.Add(new ParameterDefinition(new ArrayType(ts.Object)));
        il.Emit(OpCodes.Ldc_I4, items.Length); il.Emit(OpCodes.Newarr, ts.Object);
        for (int i = 0; i < items.Length; i++)
        {
            il.Emit(OpCodes.Dup); il.Emit(OpCodes.Ldc_I4, i);
            items[i].push(il);
            if (items[i].box != null) il.Emit(OpCodes.Box, items[i].box);
            il.Emit(OpCodes.Stelem_Ref);
        }
        il.Emit(OpCodes.Call, concat);
        il.Emit(OpCodes.Call, refs.log);
    }

    static (Action<ILProcessor>, TypeReference) S(string s) => (il => il.Emit(OpCodes.Ldstr, s), null);
    static (Action<ILProcessor>, TypeReference) Fld(FieldReference f) => (il => { il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, f); },
                                                                         f.FieldType.IsValueType ? f.FieldType : null);

    /// <summary>TMP_Text's float SizeGet() (the game's size: the fit's lower end for a grown text) and
    /// float SizeSet(float v) (a grown text's range becomes v .. v * k, the fit runs again; returns the size to store).</summary>
    static (MethodDefinition, MethodDefinition) Accessors(ModuleDefinition m, TypeDefinition text, Fields f)
    {
        var ts = m.TypeSystem;
        var get = new MethodDefinition("<asmpatch>SizeGet", MethodAttributes.Assembly | MethodAttributes.HideBySig, ts.Single);
        text.Methods.Add(get);
        {
            var il = get.Body.GetILProcessor();
            var plain = il.Create(OpCodes.Ldarg_0);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, f.scale); il.Emit(OpCodes.Ldc_R4, 0f); il.Emit(OpCodes.Ble_Un, plain);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, f.min); il.Emit(OpCodes.Ret);
            il.Append(plain); il.Emit(OpCodes.Ldfld, f.size); il.Emit(OpCodes.Ret);
            get.Body.OptimizeMacros();
        }
        var set = new MethodDefinition("<asmpatch>SizeSet", MethodAttributes.Assembly | MethodAttributes.HideBySig, ts.Single);
        set.Parameters.Add(new ParameterDefinition("v", ParameterAttributes.None, ts.Single));
        text.Methods.Add(set);
        {
            var il = set.Body.GetILProcessor();
            var plain = il.Create(OpCodes.Ldarg_1);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, f.scale); il.Emit(OpCodes.Ldc_R4, 0f); il.Emit(OpCodes.Ble_Un, plain);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldarg_1); il.Emit(OpCodes.Stfld, f.min);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldarg_1); il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, f.scale); il.Emit(OpCodes.Mul); il.Emit(OpCodes.Stfld, f.max);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Stfld, f.fitted);
            il.Emit(OpCodes.Ldarg_1); il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, f.scale); il.Emit(OpCodes.Mul); il.Emit(OpCodes.Ret);
            il.Append(plain); il.Emit(OpCodes.Ret);
            set.Body.OptimizeMacros();
        }
        return (get, set);
    }

    /// <summary>TextMeshPro.TextGrow(), on every way out of Awake: a body text gets the range size .. size * k and
    /// TextMeshPro's auto size, once; the log line.</summary>
    static MethodDefinition Grow(ModuleDefinition m, TypeDefinition tmp, HelperMethods h, Refs refs, Fields f, MethodDefinition sizeGet)
    {
        var ts = m.TypeSystem;
        var md = new MethodDefinition("<asmpatch>TextGrow", MethodAttributes.Assembly | MethodAttributes.HideBySig, ts.Void);
        tmp.Methods.Add(md);
        var body = new VariableDefinition(ts.Boolean);
        md.Body.Variables.Add(body);
        md.Body.InitLocals = true;
        var il = md.Body.GetILProcessor();
        var decided = il.Create(OpCodes.Ldsfld, h.log);
        var apply = il.Create(OpCodes.Ldloc, body);
        var ret = il.Create(OpCodes.Ret);
        var strT = new TypeReference("System", "String", m, ts.CoreLibrary);
        var equals = new MethodReference("op_Equality", ts.Boolean, strT);
        equals.Parameters.Add(new ParameterDefinition(ts.String)); equals.Parameters.Add(new ParameterDefinition(ts.String));
        il.Emit(OpCodes.Call, h.read);
        //   body = scale > 1 && textScale == 0 && m_fontAsset != null && wrap && !auto && (overflow is Overflow or Page)
        //          && box set (not default width or height) && name not kept
        il.Emit(OpCodes.Ldsfld, h.scale); il.Emit(OpCodes.Ldc_R4, 1f); il.Emit(OpCodes.Ble_Un, decided);
        il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, f.scale); il.Emit(OpCodes.Ldc_R4, 0f); il.Emit(OpCodes.Bne_Un, decided);
        il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, f.fontAsset); il.Emit(OpCodes.Ldnull); il.Emit(OpCodes.Call, refs.objEq); il.Emit(OpCodes.Brtrue, decided);
        il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, f.wrap); il.Emit(OpCodes.Brfalse, decided);
        il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, f.auto); il.Emit(OpCodes.Brtrue, decided);
        var modeOk = il.Create(OpCodes.Ldarg_0);
        il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, f.overflow); il.Emit(OpCodes.Ldc_I4, Overflow); il.Emit(OpCodes.Beq, modeOk);
        il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, f.overflow); il.Emit(OpCodes.Ldc_I4, Page); il.Emit(OpCodes.Bne_Un, decided);
        il.Append(modeOk); il.Emit(OpCodes.Ldfld, f.container); il.Emit(OpCodes.Ldnull); il.Emit(OpCodes.Call, refs.objEq); il.Emit(OpCodes.Brtrue, decided);
        il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, f.container); il.Emit(OpCodes.Callvirt, refs.cDefW); il.Emit(OpCodes.Brtrue, decided);
        il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, f.container); il.Emit(OpCodes.Callvirt, refs.cDefH); il.Emit(OpCodes.Brtrue, decided);
        foreach (var name in Kept)
        {
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Call, refs.getTransform); il.Emit(OpCodes.Callvirt, refs.getName);
            il.Emit(OpCodes.Ldstr, name); il.Emit(OpCodes.Call, equals); il.Emit(OpCodes.Brtrue, decided);
        }
        il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Stloc, body);
        //   if (log) Debug.Log("[hk-text] " + Path(transform) + " size " + m_fontSize + " world " + ... + " body " + body)
        il.Append(decided); il.Emit(OpCodes.Brfalse, apply);
        var noFont = il.Create(OpCodes.Ldstr, "none");
        var fontDone = il.Create(OpCodes.Nop);
        LogLine(m, il, refs,
            S("[hk-text] "), (x => { x.Emit(OpCodes.Ldarg_0); x.Emit(OpCodes.Call, refs.getTransform); x.Emit(OpCodes.Call, h.path); }, null),
            S(" size "), Fld(f.size),
            S(" world "), (x =>
            {
                x.Emit(OpCodes.Ldarg_0); x.Emit(OpCodes.Ldfld, f.size);
                x.Emit(OpCodes.Ldarg_0); x.Emit(OpCodes.Call, refs.getTransform); x.Emit(OpCodes.Callvirt, refs.lossy); x.Emit(OpCodes.Ldfld, refs.vecY);
                x.Emit(OpCodes.Call, refs.abs); x.Emit(OpCodes.Mul);
            }, ts.Single),
            S(" overflow "), Fld(f.overflow), S(" wrap "), Fld(f.wrap), S(" auto "), Fld(f.auto),
            S(" font "), (x =>
            {
                x.Emit(OpCodes.Ldarg_0); x.Emit(OpCodes.Ldfld, f.fontAsset); x.Emit(OpCodes.Ldnull); x.Emit(OpCodes.Call, refs.objEq); x.Emit(OpCodes.Brtrue, noFont);
                x.Emit(OpCodes.Ldarg_0); x.Emit(OpCodes.Ldfld, f.fontAsset); x.Emit(OpCodes.Callvirt, refs.getName); x.Emit(OpCodes.Br, fontDone);
                x.Append(noFont); x.Append(fontDone);
            }, null),
            S(" body "), (x => x.Emit(OpCodes.Ldloc, body), ts.Boolean));
        //   if (body) { textScale = scale; min = size; max = size * scale; size = max; auto = true; fitted = false }
        il.Append(apply); il.Emit(OpCodes.Brfalse, ret);
        il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldsfld, h.scale); il.Emit(OpCodes.Stfld, f.scale);
        il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, f.size); il.Emit(OpCodes.Stfld, f.min);
        il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, f.size); il.Emit(OpCodes.Ldsfld, h.scale); il.Emit(OpCodes.Mul); il.Emit(OpCodes.Stfld, f.max);
        il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, f.max); il.Emit(OpCodes.Stfld, f.size);
        il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Stfld, f.auto);
        il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Stfld, f.fitted);
        //   floor: the first of Floors whose name and parent's name are the text's
        var parent = new VariableDefinition(refs.getParent.ReturnType);
        md.Body.Variables.Add(parent);
        il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Call, refs.getTransform); il.Emit(OpCodes.Callvirt, refs.getParent); il.Emit(OpCodes.Stloc, parent);
        il.Emit(OpCodes.Ldloc, parent); il.Emit(OpCodes.Ldnull); il.Emit(OpCodes.Call, refs.objEq); il.Emit(OpCodes.Brtrue, ret);
        foreach (var (pn, n, floor) in Floors)
        {
            var next = il.Create(OpCodes.Nop);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Call, refs.getTransform); il.Emit(OpCodes.Callvirt, refs.getName);
            il.Emit(OpCodes.Ldstr, n); il.Emit(OpCodes.Call, equals); il.Emit(OpCodes.Brfalse, next);
            il.Emit(OpCodes.Ldloc, parent); il.Emit(OpCodes.Callvirt, refs.getName);
            il.Emit(OpCodes.Ldstr, pn); il.Emit(OpCodes.Call, equals); il.Emit(OpCodes.Brfalse, next);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldc_R4, floor); il.Emit(OpCodes.Stfld, f.floor); il.Emit(OpCodes.Br, ret);
            il.Append(next);
        }
        il.Append(ret);
        md.Body.OptimizeMacros();
        return md;
    }

    /// <summary>TextMeshPro.TextFitBegin() before OnPreRenderObject's layout: a grown text not fitted to its input yet
    /// is laid out by the auto size from its top size; TextFitEnd() after it: the size found is kept (auto size off,
    /// fitted) until the input or the size changes.</summary>
    static (MethodDefinition, MethodDefinition) Fit(ModuleDefinition m, TypeDefinition tmp, HelperMethods h, Refs refs, Fields f, MethodDefinition sizeGet)
    {
        var ts = m.TypeSystem;
        var begin = new MethodDefinition("<asmpatch>TextFitBegin", MethodAttributes.Assembly | MethodAttributes.HideBySig, ts.Void);
        tmp.Methods.Add(begin);
        {
            var il = begin.Body.GetILProcessor();
            var ret = il.Create(OpCodes.Ret);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, f.scale); il.Emit(OpCodes.Ldc_R4, 0f); il.Emit(OpCodes.Ble_Un, ret);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, f.fitted); il.Emit(OpCodes.Brtrue, ret);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Stfld, f.auto);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, f.max); il.Emit(OpCodes.Stfld, f.size);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, f.max); il.Emit(OpCodes.Stfld, f.maxRun);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, f.min); il.Emit(OpCodes.Stfld, f.minRun);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldc_R4, -1f); il.Emit(OpCodes.Stfld, f.saved);
            //   with a floor: the HudCamera above the text (at most 8 parents up); the floor in the world:
            //   camera's y + (540 - floor) / 64; the height the fit may use: from the box's top (less its top
            //   margin) down to it, in the text's own units
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, f.floor); il.Emit(OpCodes.Ldc_R4, 0f); il.Emit(OpCodes.Ble_Un, ret);
            var t = new VariableDefinition(refs.getParent.ReturnType); var c = new VariableDefinition(refs.getParent.ReturnType);
            var n = new VariableDefinition(ts.Int32); var ly = new VariableDefinition(ts.Single); var fw = new VariableDefinition(ts.Single);
            var cap = new VariableDefinition(ts.Single);
            foreach (var x in new[] { t, c, n, ly, fw, cap }) begin.Body.Variables.Add(x);
            begin.Body.InitLocals = true;
            var strT = new TypeReference("System", "String", m, ts.CoreLibrary);
            var equals = new MethodReference("op_Equality", ts.Boolean, strT);
            equals.Parameters.Add(new ParameterDefinition(ts.String)); equals.Parameters.Add(new ParameterDefinition(ts.String));
            var loop = il.Create(OpCodes.Ldloc, c);
            var found = il.Create(OpCodes.Ldloc, t);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Call, refs.getTransform); il.Emit(OpCodes.Stloc, t);
            il.Emit(OpCodes.Ldloc, t); il.Emit(OpCodes.Callvirt, refs.getParent); il.Emit(OpCodes.Stloc, c);
            il.Append(loop); il.Emit(OpCodes.Ldnull); il.Emit(OpCodes.Call, refs.objEq); il.Emit(OpCodes.Brtrue, ret);
            il.Emit(OpCodes.Ldloc, c); il.Emit(OpCodes.Callvirt, refs.getName); il.Emit(OpCodes.Ldstr, HudCamera); il.Emit(OpCodes.Call, equals); il.Emit(OpCodes.Brtrue, found);
            il.Emit(OpCodes.Ldloc, n); il.Emit(OpCodes.Ldc_I4_8); il.Emit(OpCodes.Bge, ret);
            il.Emit(OpCodes.Ldloc, c); il.Emit(OpCodes.Callvirt, refs.getParent); il.Emit(OpCodes.Stloc, c);
            il.Emit(OpCodes.Ldloc, n); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Add); il.Emit(OpCodes.Stloc, n); il.Emit(OpCodes.Br, loop);
            //   ly = |lossyScale.y|; if (ly <= 0) return
            il.Append(found); il.Emit(OpCodes.Callvirt, refs.lossy); il.Emit(OpCodes.Ldfld, refs.vecY); il.Emit(OpCodes.Call, refs.abs); il.Emit(OpCodes.Stloc, ly);
            il.Emit(OpCodes.Ldloc, ly); il.Emit(OpCodes.Ldc_R4, 0f); il.Emit(OpCodes.Ble_Un, ret);
            //   fw = c.position.y + (540 - floor) / 64
            il.Emit(OpCodes.Ldloc, c); il.Emit(OpCodes.Callvirt, refs.position); il.Emit(OpCodes.Ldfld, refs.vecY);
            il.Emit(OpCodes.Ldc_R4, LayoutMiddle); il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, f.floor); il.Emit(OpCodes.Sub);
            il.Emit(OpCodes.Ldc_R4, LayoutUnit); il.Emit(OpCodes.Div); il.Emit(OpCodes.Add); il.Emit(OpCodes.Stloc, fw);
            //   cap = corners[1].y (the box's top left, as TextMeshPro lays it out) - margins.y - (fw - t.position.y) / ly
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, f.container); il.Emit(OpCodes.Callvirt, refs.cCorners);
            il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Ldelema, refs.vecY.DeclaringType); il.Emit(OpCodes.Ldfld, refs.vecY);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, f.container); il.Emit(OpCodes.Callvirt, refs.cMargins); il.Emit(OpCodes.Ldfld, refs.vec4Y); il.Emit(OpCodes.Sub);
            il.Emit(OpCodes.Ldloc, fw); il.Emit(OpCodes.Ldloc, t); il.Emit(OpCodes.Callvirt, refs.position); il.Emit(OpCodes.Ldfld, refs.vecY); il.Emit(OpCodes.Sub);
            il.Emit(OpCodes.Ldloc, ly); il.Emit(OpCodes.Div); il.Emit(OpCodes.Sub); il.Emit(OpCodes.Stloc, cap);
            var noLog = il.Create(OpCodes.Nop);
            il.Emit(OpCodes.Ldsfld, h.log); il.Emit(OpCodes.Brfalse, noLog);
            LogLine(m, il, refs,
                S("[hk-text] floor "), (x => { x.Emit(OpCodes.Ldloc, t); x.Emit(OpCodes.Call, h.path); }, null),
                S(" camera y "), (x => { x.Emit(OpCodes.Ldloc, c); x.Emit(OpCodes.Callvirt, refs.position); x.Emit(OpCodes.Ldfld, refs.vecY); }, ts.Single),
                S(" text y "), (x => { x.Emit(OpCodes.Ldloc, t); x.Emit(OpCodes.Callvirt, refs.position); x.Emit(OpCodes.Ldfld, refs.vecY); }, ts.Single),
                S(" scale "), (x => x.Emit(OpCodes.Ldloc, ly), ts.Single),
                S(" box top "), (x =>
                {
                    x.Emit(OpCodes.Ldarg_0); x.Emit(OpCodes.Ldfld, f.container); x.Emit(OpCodes.Callvirt, refs.cCorners);
                    x.Emit(OpCodes.Ldc_I4_1); x.Emit(OpCodes.Ldelema, refs.vecY.DeclaringType); x.Emit(OpCodes.Ldfld, refs.vecY);
                }, ts.Single),
                S(" floor y "), (x => x.Emit(OpCodes.Ldloc, fw), ts.Single), S(" height "), (x => x.Emit(OpCodes.Ldloc, cap), ts.Single));
            il.Append(noLog);
            //   if (0 < cap < marginHeight) { saved = marginHeight; marginHeight = cap }
            il.Emit(OpCodes.Ldloc, cap); il.Emit(OpCodes.Ldc_R4, 0f); il.Emit(OpCodes.Ble_Un, ret);
            il.Emit(OpCodes.Ldloc, cap); il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, f.marginHeight); il.Emit(OpCodes.Bge_Un, ret);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, f.marginHeight); il.Emit(OpCodes.Stfld, f.saved);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldloc, cap); il.Emit(OpCodes.Stfld, f.marginHeight);
            il.Append(ret);
            begin.Body.OptimizeMacros();
        }
        var end = new MethodDefinition("<asmpatch>TextFitEnd", MethodAttributes.Assembly | MethodAttributes.HideBySig, ts.Void);
        tmp.Methods.Add(end);
        {
            var il = end.Body.GetILProcessor();
            var ret = il.Create(OpCodes.Ret);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, f.scale); il.Emit(OpCodes.Ldc_R4, 0f); il.Emit(OpCodes.Ble_Un, ret);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, f.fitted); il.Emit(OpCodes.Brtrue, ret);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Stfld, f.auto);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Stfld, f.fitted);
            var logged = il.Create(OpCodes.Nop);
            il.Emit(OpCodes.Ldsfld, h.log); il.Emit(OpCodes.Brfalse, logged);
            LogLine(m, il, refs,
                S("[hk-text] fit "), (x => { x.Emit(OpCodes.Ldarg_0); x.Emit(OpCodes.Call, refs.getTransform); x.Emit(OpCodes.Call, h.path); }, null),
                S(" "), Fld(f.min), S(" -> "), Fld(f.size), S(" (up to "), Fld(f.max), S(", height "), Fld(f.marginHeight), S(" of "), Fld(f.saved), S(")"));
            //   a height cut by the floor: the box's own height back, laid out again at the size found (next frame)
            il.Append(logged);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, f.saved); il.Emit(OpCodes.Ldc_R4, 0f); il.Emit(OpCodes.Blt_Un, ret);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, f.saved); il.Emit(OpCodes.Stfld, f.marginHeight);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldc_R4, -1f); il.Emit(OpCodes.Stfld, f.saved);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Stfld, f.changed);
            il.Append(ret);
            end.Body.OptimizeMacros();
        }
        return (begin, end);
    }
}
