// blasphemous-skin-idle (kind Optimize, opt-in). Blasphemous keeps its Extras menu (UI_EXTRAS: the skin selector,
// the achievements, ...) in the game's UI canvas for the whole run, active and hidden: ExtrasMenuWidget.ResetMenus
// sets each of its three pages' CanvasGroup alpha to 0, and ShowMenu sets the shown page's to 1. The skin selector's
// page holds a preview Image per skin (18 in the 4.0 build), each with an Animator playing the Penitent's idle
// animation by setting Image.sprite: at every sprite change, invisible at alpha 0, the Image's mesh is rebuilt and
// the canvas (4,200 RectTransforms) batched again, about 3.7 rebuilds a frame. On the BattleXP G350 that costs
// 58.4 -> 59.8 fps in the first level (2026-11-18). The patch disables those Images (Behaviour.enabled = false)
// while the skin selector's page is hidden and puts each one's enabled state back before ShowMenu runs:
//   ShowMenu(menu):   first <asmpatch>SkinShow(this) (the saved states back), then the game's code; at each of its
//                     returns, if currentMenu is not SKINSELECTOR, <asmpatch>SkinHide(this)
//   ResetMenus():     at each return <asmpatch>SkinHide(this) (every page at alpha 0)
//   SkinHide(w):      once (until SkinShow): for each skinSelectorDataElements[i].element,
//                     img = element.GetComponentInChildren<Image>() (the Image the game's own lookups find; none in an
//                     inactive element, whose Image rebuilds nothing); was[i] = img.enabled; img.enabled = false
// The Animators keep running (their state and the sprite they set are the game's); a disabled Image only skips
// registering its rebuilds. Every read and write of these Images' enabled flags (ShowSkinSelectorValues) and every
// write of the selector page's alpha (ShowMenu, ResetMenus) happens inside ShowMenu or ResetMenus or with the page
// shown (Update -> UpdateInputSkinSelectorOptions, only in the skin selector), so the game's code sees its own
// values; an Image enabled again rebuilds from its current sprite, the mesh the game's last rebuild made. The page's
// CanvasGroup is not animated (the Extras animations, extras_on/extras_off, animate UI_EXTRAS's and Options'
// groups). While hidden the Images take no raycasts: the port has no pointer (ua_host's UA_POINTER is off).
// ASMPATCH_BLASPHEMOUS_SKIN_IDLE=off (read once): the game's own behaviour.
// Checked at patch time: ShowMenu, ResetMenus, ShowSkinSelectorValues and UpdateInputSkinSelectorOptions as the game
// has them (IL hashes); skinSelectorDataElements read only in ShowMenu, SetOptionSkinSelected and
// ShowSkinSelectorValues; the selector page's root (SelectorRoot) read only in Awake; CanvasGroup.set_alpha called in
// the class only in ShowMenu and ResetMenus.
using Mono.Cecil;
using Mono.Cecil.Cil;
using Mono.Cecil.Rocks;

namespace AsmPatch;

sealed class BlasphemousSkinIdle : Patch
{
    public override string Name => "blasphemous-skin-idle";
    public override Kind Kind => Kind.Optimize;
    public override bool OptIn => true;
    public override string Summary =>
        "Blasphemous's hidden skin selector: its animated preview Images disabled while the page is hidden (no mesh rebuilds at alpha 0), their states back before the menu shows it (opt-in)";

    public const string Variable = "ASMPATCH_BLASPHEMOUS_SKIN_IDLE", HelperType = "<asmpatch>BlasphemousSkinIdle";
    const string WidgetType = "Gameplay.UI.Others.MenuLogic.ExtrasMenuWidget";
    internal static string ShowHash = "5b441c1c4a30e9ac", ResetHash = "ad978c06b547c7b0", ValuesHash = "d8f22185f3f93716", InputHash = "83267b68816770e1";

    public override Result Run(Game game, bool apply)
    {
        var asm = game.Load("Assembly-CSharp");
        var m = asm?.MainModule;
        var w = m?.GetType(WidgetType);
        if (w == null) return Result.NotApplicable($"no Blasphemous ({WidgetType} in Assembly-CSharp)");
        var file = game.FileOf(asm);
        if (m.Types.Any(t => t.Name == HelperType))
            return new Result { Outcome = Outcome.AlreadyApplied, Details = { $"{file}: ExtrasMenuWidget's skin previews idle while hidden" } };
        var why = Check(w, out var show, out var reset, out var data, out var current, out int skinValue);
        if (why != null) return Result.NotApplicable($"{file}: {why}");
        var r = new Result { Outcome = Outcome.Applied };
        r.Details.Add($"{file}: ExtrasMenuWidget.ShowMenu/ResetMenus: the skin selector's preview Images disabled while its page is hidden, their states back before ShowMenu");
        if (!apply) return r;
        var h = Helper(game, m, w, data);
        if (h == null) return Result.Failed($"{file}: a class library or engine method the helper calls is missing");
        var (hide, showM) = h.Value;

        // ShowMenu: SkinShow(this) first; before each ret: if (currentMenu != SKINSELECTOR) SkinHide(this)
        {
            var body = show.Body; body.SimplifyMacros();
            var il = body.GetILProcessor();
            var first = body.Instructions[0];
            il.InsertBefore(first, il.Create(OpCodes.Ldarg_0));
            il.InsertBefore(first, il.Create(OpCodes.Call, showM));
            foreach (var ret in body.Instructions.Where(i => i.OpCode.Code == Code.Ret).ToList())
            {
                // the ret becomes the start of the epilogue (branches to it land there), a new ret ends it
                var end = il.Create(OpCodes.Ret);
                il.InsertAfter(ret, end);
                ret.OpCode = OpCodes.Ldarg_0; ret.Operand = null;
                il.InsertBefore(end, il.Create(OpCodes.Ldfld, current));
                il.InsertBefore(end, il.Create(OpCodes.Ldc_I4, skinValue));
                il.InsertBefore(end, il.Create(OpCodes.Beq, end));
                il.InsertBefore(end, il.Create(OpCodes.Ldarg_0));
                il.InsertBefore(end, il.Create(OpCodes.Call, hide));
            }
            body.OptimizeMacros();
        }
        // ResetMenus: before each ret: SkinHide(this)
        {
            var body = reset.Body; body.SimplifyMacros();
            var il = body.GetILProcessor();
            foreach (var ret in body.Instructions.Where(i => i.OpCode.Code == Code.Ret).ToList())
            {
                var end = il.Create(OpCodes.Ret);
                il.InsertAfter(ret, end);
                ret.OpCode = OpCodes.Ldarg_0; ret.Operand = null;
                il.InsertBefore(end, il.Create(OpCodes.Call, hide));
            }
            body.OptimizeMacros();
        }
        game.MarkChanged(asm);
        return r;
    }

    static string Check(TypeDefinition w, out MethodDefinition show, out MethodDefinition reset, out FieldDefinition data, out FieldDefinition current, out int skinValue)
    {
        skinValue = -1;
        MethodDefinition M(string name, int ps) => w.Methods.FirstOrDefault(x => x.Name == name && !x.IsStatic && x.HasBody && x.Parameters.Count == ps);
        show = M("ShowMenu", 1); reset = M("ResetMenus", 0);
        var values = M("ShowSkinSelectorValues", 0); var input = M("UpdateInputSkinSelectorOptions", 0);
        data = w.Fields.FirstOrDefault(f => f.Name == "skinSelectorDataElements" && !f.IsStatic);
        current = w.Fields.FirstOrDefault(f => f.Name == "currentMenu" && !f.IsStatic);
        var root = w.Fields.FirstOrDefault(f => f.Name == "SelectorRoot" && !f.IsStatic);
        if (show == null || reset == null || values == null || input == null || data == null || current == null || root == null)
            return "no ExtrasMenuWidget.ShowMenu/ResetMenus/ShowSkinSelectorValues/UpdateInputSkinSelectorOptions/skinSelectorDataElements/currentMenu/SelectorRoot";
        string hs = BlasphemousCatalogs.Shape(show), hr = BlasphemousCatalogs.Shape(reset), hv = BlasphemousCatalogs.Shape(values), hi = BlasphemousCatalogs.Shape(input);
        if (hs != ShowHash || hr != ResetHash || hv != ValuesHash || hi != InputHash)
            return $"ExtrasMenuWidget.ShowMenu/ResetMenus/ShowSkinSelectorValues/UpdateInputSkinSelectorOptions are not as this patch knows them (IL {hs}/{hr}/{hv}/{hi}, known {ShowHash}/{ResetHash}/{ValuesHash}/{InputHash})";
        if (data.FieldType is not GenericInstanceType lt || lt.ElementType.FullName != "System.Collections.Generic.List`1" ||
            lt.GenericArguments[0].Resolve() is not TypeDefinition el || !el.IsValueType ||
            !el.Fields.Any(f => f.Name == "element" && f.FieldType.FullName == "UnityEngine.GameObject"))
            return "skinSelectorDataElements is not a List<SkinSelectorElement> with a GameObject element";
        var menu = Cecil.TryResolve(current.FieldType);
        if (menu == null || !menu.IsEnum || Cecil.EnumValue(menu, "SKINSELECTOR") is not int sv) return "currentMenu is not an enum with SKINSELECTOR";
        skinValue = sv;
        // who reads the elements, the selector page's root and sets a CanvasGroup's alpha
        var readers = new HashSet<string>(); var rootReaders = new HashSet<string>(); var alphaWriters = new HashSet<string>();
        foreach (var ty in new[] { w }.Concat(w.NestedTypes))
            foreach (var md in ty.Methods.Where(x => x.HasBody))
                foreach (var i in md.Body.Instructions)
                {
                    if (Cecil.SameField(i.Operand, data)) readers.Add(md.Name);
                    if (Cecil.SameField(i.Operand, root)) rootReaders.Add(md.Name);
                    if (i.Operand is MethodReference mr && mr.Name == "set_alpha" && mr.DeclaringType.FullName == "UnityEngine.CanvasGroup") alphaWriters.Add(md.Name);
                }
        if (!readers.SetEquals(new[] { "ShowMenu", "SetOptionSkinSelected", "ShowSkinSelectorValues" }))
            return $"skinSelectorDataElements is read in {string.Join(", ", readers.OrderBy(x => x))}, not only in ShowMenu, SetOptionSkinSelected and ShowSkinSelectorValues";
        if (!rootReaders.SetEquals(new[] { "Awake" })) return $"SelectorRoot is read in {string.Join(", ", rootReaders.OrderBy(x => x))}, not only in Awake";
        if (!alphaWriters.SetEquals(new[] { "ShowMenu", "ResetMenus" })) return $"a CanvasGroup's alpha is set in {string.Join(", ", alphaWriters.OrderBy(x => x))}, not only in ShowMenu and ResetMenus";
        return null;
    }

    static (MethodDefinition hide, MethodDefinition show)? Helper(Game game, ModuleDefinition m, TypeDefinition w, FieldDefinition data)
    {
        var ts = m.TypeSystem;
        var env = BlasphemousCatalogs.Lib(game, m, "System.Environment", "GetEnvironmentVariable", "System.String");
        var strEq = BlasphemousCatalogs.Lib(game, m, "System.String", "op_Equality", "System.String", "System.String");
        var log = BlasphemousCatalogs.Lib(game, m, "UnityEngine.Debug", "Log", "System.Object");
        var truth = BlasphemousCatalogs.Lib(game, m, "UnityEngine.Object", "op_Implicit", "UnityEngine.Object");
        var getEn = BlasphemousCatalogs.Lib(game, m, "UnityEngine.Behaviour", "get_enabled");
        var setEn = BlasphemousCatalogs.Lib(game, m, "UnityEngine.Behaviour", "set_enabled", "System.Boolean");
        if (new[] { env, strEq, log, truth, getEn, setEn }.Any(x => x == null)) return null;
        TypeDefinition image = null, go = null;
        foreach (var a in game.Assemblies)
        {
            image ??= a.MainModule.GetType("UnityEngine.UI.Image");
            go ??= a.MainModule.GetType("UnityEngine.GameObject");
        }
        var gcic = go?.Methods.FirstOrDefault(x => x.Name == "GetComponentInChildren" && x.HasGenericParameters && x.GenericParameters.Count == 1 &&
                                                   x.Parameters.Count == 1 && x.Parameters[0].ParameterType.FullName == "System.Boolean");
        if (image == null || gcic == null) return null;
        var imageT = m.ImportReference(image);
        var getImage = new GenericInstanceMethod(m.ImportReference(gcic)); getImage.GenericArguments.Add(imageT);
        var listT = (GenericInstanceType)data.FieldType;
        var elT = listT.GenericArguments[0];
        var element = m.ImportReference(elT.Resolve().Fields.First(f => f.Name == "element"));
        var listGen = listT.ElementType.Resolve();
        MethodReference ListM(string name, TypeReference ret, params TypeReference[] ps)
        {
            var r = new MethodReference(name, ret, listT) { HasThis = true };
            foreach (var p in ps) r.Parameters.Add(new ParameterDefinition(p));
            return m.ImportReference(r);
        }
        var count = ListM("get_Count", ts.Int32);
        var item = ListM("get_Item", listGen.GenericParameters[0], ts.Int32);
        var imgArr = new ArrayType(imageT); var boolArr = new ArrayType(ts.Boolean);

        var t = new TypeDefinition("", HelperType, TypeAttributes.NotPublic | TypeAttributes.Abstract | TypeAttributes.Sealed |
                                                   TypeAttributes.BeforeFieldInit | TypeAttributes.Class, ts.Object);
        m.Types.Add(t);
        var mode = new FieldDefinition("mode", FieldAttributes.Assembly | FieldAttributes.Static, ts.Int32);   // 0 unread, 1 on, 2 off
        t.Fields.Add(mode);
        var imgs = new FieldDefinition("<asmpatch>skinImages", FieldAttributes.Private, imgArr);
        var was = new FieldDefinition("<asmpatch>skinWas", FieldAttributes.Private, boolArr);
        w.Fields.Add(imgs); w.Fields.Add(was);
        MethodDefinition Method(TypeDefinition on, string name, params (string n, TypeReference t)[] ps)
        {
            var md = new MethodDefinition(name, (on == w ? MethodAttributes.Private : MethodAttributes.Assembly) | MethodAttributes.Static | MethodAttributes.HideBySig,
                                          on == t ? ts.Int32 : ts.Void);
            foreach (var (n, pt) in ps) md.Parameters.Add(new ParameterDefinition(n, ParameterAttributes.None, pt));
            md.Body.InitLocals = true;
            on.Methods.Add(md);
            return md;
        }
        VariableDefinition Local(MethodDefinition md, TypeReference vt) { var v = new VariableDefinition(vt); md.Body.Variables.Add(v); return v; }

        // static int Mode(): read once (off 2, else 1), said once
        var modeM = Method(t, "Mode");
        {
            var il = modeM.Body.GetILProcessor();
            var have = il.Create(OpCodes.Ldsfld, mode); var on = il.Create(OpCodes.Ldc_I4_1);
            var read = il.Create(OpCodes.Ldstr, Variable);
            il.Emit(OpCodes.Ldsfld, mode); il.Emit(OpCodes.Brfalse, read); il.Emit(OpCodes.Br, have);
            il.Append(read); il.Emit(OpCodes.Call, env); il.Emit(OpCodes.Ldstr, "off"); il.Emit(OpCodes.Call, strEq); il.Emit(OpCodes.Brfalse, on);
            il.Emit(OpCodes.Ldc_I4_2); il.Emit(OpCodes.Stsfld, mode);
            il.Emit(OpCodes.Ldstr, $"[blasphemous-skin-idle] {Variable}=off: the skin selector's previews as the game has them"); il.Emit(OpCodes.Call, log);
            il.Emit(OpCodes.Br, have);
            il.Append(on); il.Emit(OpCodes.Stsfld, mode);
            il.Emit(OpCodes.Ldstr, "[blasphemous-skin-idle] the skin selector's preview Images idle while its page is hidden"); il.Emit(OpCodes.Call, log);
            il.Append(have); il.Emit(OpCodes.Ret);
        }

        // static void <asmpatch>SkinHide(ExtrasMenuWidget w)
        //   if (Mode() != 1 || w.skinImages != null) return; n = w.skinSelectorDataElements.Count; a = new Image[n]; b = new bool[n];
        //   for (i = 0; i < n; i++) { g = list[i].element; if (!g) continue; img = g.GetComponentInChildren<Image>(false); a[i] = img;
        //                             if (!img) continue; b[i] = img.enabled; img.enabled = false; }
        //   w.skinImages = a; w.skinWas = b;
        var hide = Method(w, "<asmpatch>SkinHide", ("w", w));
        {
            var n = Local(hide, ts.Int32); var a = Local(hide, imgArr); var b = Local(hide, boolArr); var i = Local(hide, ts.Int32);
            var g = Local(hide, m.ImportReference(go)); var img = Local(hide, imageT);
            var il = hide.Body.GetILProcessor();
            var ret = il.Create(OpCodes.Ret);
            il.Emit(OpCodes.Call, modeM); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Bne_Un, ret);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, imgs); il.Emit(OpCodes.Brtrue, ret);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, data); il.Emit(OpCodes.Callvirt, count); il.Emit(OpCodes.Stloc, n);
            il.Emit(OpCodes.Ldloc, n); il.Emit(OpCodes.Newarr, imageT); il.Emit(OpCodes.Stloc, a);
            il.Emit(OpCodes.Ldloc, n); il.Emit(OpCodes.Newarr, ts.Boolean); il.Emit(OpCodes.Stloc, b);
            var cond = il.Create(OpCodes.Ldloc, i); var next = il.Create(OpCodes.Ldloc, i); var body = il.Create(OpCodes.Ldarg_0);
            il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Stloc, i); il.Emit(OpCodes.Br, cond);
            il.Append(body); il.Emit(OpCodes.Ldfld, data); il.Emit(OpCodes.Ldloc, i); il.Emit(OpCodes.Callvirt, item);
            il.Emit(OpCodes.Ldfld, element); il.Emit(OpCodes.Stloc, g);
            il.Emit(OpCodes.Ldloc, g); il.Emit(OpCodes.Call, truth); il.Emit(OpCodes.Brfalse, next);
            il.Emit(OpCodes.Ldloc, g); il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Callvirt, getImage); il.Emit(OpCodes.Stloc, img);
            il.Emit(OpCodes.Ldloc, a); il.Emit(OpCodes.Ldloc, i); il.Emit(OpCodes.Ldloc, img); il.Emit(OpCodes.Stelem_Ref);
            il.Emit(OpCodes.Ldloc, img); il.Emit(OpCodes.Call, truth); il.Emit(OpCodes.Brfalse, next);
            il.Emit(OpCodes.Ldloc, b); il.Emit(OpCodes.Ldloc, i); il.Emit(OpCodes.Ldloc, img); il.Emit(OpCodes.Callvirt, getEn); il.Emit(OpCodes.Stelem_I1);
            il.Emit(OpCodes.Ldloc, img); il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Callvirt, setEn);
            il.Append(next); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Add); il.Emit(OpCodes.Stloc, i);
            il.Append(cond); il.Emit(OpCodes.Ldloc, n); il.Emit(OpCodes.Blt, body);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldloc, a); il.Emit(OpCodes.Stfld, imgs);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldloc, b); il.Emit(OpCodes.Stfld, was);
            il.Append(ret);
        }

        // static void <asmpatch>SkinShow(ExtrasMenuWidget w)
        //   a = w.skinImages; if (a == null) return; b = w.skinWas; w.skinImages = null; w.skinWas = null;
        //   for (i = 0; i < a.Length; i++) { img = a[i]; if (img) img.enabled = b[i]; }
        var showM = Method(w, "<asmpatch>SkinShow", ("w", w));
        {
            var a = Local(showM, imgArr); var b = Local(showM, boolArr); var i = Local(showM, ts.Int32); var img = Local(showM, imageT);
            var il = showM.Body.GetILProcessor();
            var ret = il.Create(OpCodes.Ret);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, imgs); il.Emit(OpCodes.Stloc, a);
            il.Emit(OpCodes.Ldloc, a); il.Emit(OpCodes.Brfalse, ret);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, was); il.Emit(OpCodes.Stloc, b);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldnull); il.Emit(OpCodes.Stfld, imgs);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldnull); il.Emit(OpCodes.Stfld, was);
            var cond = il.Create(OpCodes.Ldloc, i); var next = il.Create(OpCodes.Ldloc, i); var body = il.Create(OpCodes.Ldloc, a);
            il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Stloc, i); il.Emit(OpCodes.Br, cond);
            il.Append(body); il.Emit(OpCodes.Ldloc, i); il.Emit(OpCodes.Ldelem_Ref); il.Emit(OpCodes.Stloc, img);
            il.Emit(OpCodes.Ldloc, img); il.Emit(OpCodes.Call, truth); il.Emit(OpCodes.Brfalse, next);
            il.Emit(OpCodes.Ldloc, img); il.Emit(OpCodes.Ldloc, b); il.Emit(OpCodes.Ldloc, i); il.Emit(OpCodes.Ldelem_U1); il.Emit(OpCodes.Callvirt, setEn);
            il.Append(next); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Add); il.Emit(OpCodes.Stloc, i);
            il.Append(cond); il.Emit(OpCodes.Ldloc, a); il.Emit(OpCodes.Ldlen); il.Emit(OpCodes.Conv_I4); il.Emit(OpCodes.Blt, body);
            il.Append(ret);
        }
        return (hide, showM);
    }
}
