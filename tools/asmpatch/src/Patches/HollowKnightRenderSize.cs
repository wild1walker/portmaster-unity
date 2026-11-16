// hk-render-size (kind Screen, opt-in: the size the world is drawn at, never the game play). A handheld
// draws Hollow Knight below its panel's size (UA_RENDER_SIZE, docs/performance.md); with Unity's whole
// screen that small, the inventory, the menus and the HUD are drawn small and scaled up, and the small
// text of the item descriptions is hard to read. The game has its own way: CameraRenderScaled, on the
// main camera, draws the world into a texture of CameraRenderScaled.Resolution, CameraRenderScaledApply
// on the HUD camera copies that to the screen before the HUD, and DisplayFrozenCamera (the picture
// behind the inventory) is taken at that size. Nothing in the game sets the size: it stays 0x0, the
// screen's. With the patch, the environment variable ASMPATCH_HK_RENDER_SIZE sets it when the class is
// first used:
//   <w>x<h>   the world at w x h, everything else (HUD, inventory, menu text) at the screen's size
//   anything else, or unset: the game as it is.
// The game's rule for menus, the title and cutscene scenes (WorldInfo.NonGameplaySceneName: their world
// at the screen's size, set in CameraRenderScaled.OnActiveSceneChanged) is for a size that is a choice
// of quality; here the size is what the handheld can draw (the G350's title menu at its panel's 640x480
// runs at 50 fps, at 480x360 at 60), so with a size set those scenes' worlds are drawn at it too. Videos
// keep the screen's size (CinematicPlayer's own switch, untouched).
// ua_host's UA_RENDER_SIZE_VAR=ASMPATCH_HK_RENDER_SIZE hands it the size picked for the handheld and
// leaves Unity's screen at the panel's size (docs/runtime.md).
// Where the game draws bars (ForceCameraAspect's viewport rectangle: hk-aspect's bars below 16:10, or a
// screen wider than 2.39:1) the world camera draws into its rectangle of the texture, and
// CameraRenderScaledApply copied the whole texture into the HUD camera's rectangle (the same one) on
// the screen: the texture's own bars landed inside the picture (strips at its top and bottom, of
// whatever that part of the texture held: solid cyan or blue on the handhelds) and the world was
// squeezed into the rest. The game never takes this path itself (its size stays 0x0, no texture). With
// the patch the copy takes only the camera's rectangle of the texture (Graphics.Blit with that scale
// and offset): the whole texture, as before, where there are no bars. A game patched before (the
// helper without Copy) gets this part alone. Nothing else changes.
using Mono.Cecil;
using Mono.Cecil.Cil;
using Mono.Cecil.Rocks;

namespace AsmPatch;

sealed class HollowKnightRenderSize : Patch
{
    public override string Name => "hk-render-size";
    public override Kind Kind => Kind.Screen;
    public override bool OptIn => true;
    public override string Summary =>
        "Hollow Knight's world drawn at the size in ASMPATCH_HK_RENDER_SIZE=<w>x<h> by the game's own CameraRenderScaled, its HUD, inventory and menu text at the screen's size: readable text on a handheld that renders small (opt-in)";

    public const string Variable = "ASMPATCH_HK_RENDER_SIZE", HelperType = "<asmpatch>HollowKnightRenderSize";

    public override Result Run(Game game, bool apply)
    {
        var asm = game.Load("Assembly-CSharp");
        var m = asm?.MainModule;
        var crs = m?.GetType("CameraRenderScaled");
        if (crs == null)
            return Result.NotApplicable("no Hollow Knight (CameraRenderScaled in Assembly-CSharp)");
        var file = game.FileOf(asm);
        // CameraRenderScaledApply.OnPreRender's one Graphics.Blit(Texture, RenderTexture) (its texture onto the screen)
        var apl = m.GetType("CameraRenderScaledApply");
        var pre = apl?.Methods.FirstOrDefault(x => x.Name == "OnPreRender" && !x.IsStatic && x.HasBody);
        var blits = pre?.Body.Instructions.Where(i => i.OpCode.Code == Code.Call && i.Operand is MethodReference c && c.Name == "Blit" &&
                                                      c.DeclaringType.Name == "Graphics" && c.Parameters.Count == 2).ToList();
        var srcCam = apl?.Methods.FirstOrDefault(x => x.Name == "get_SourceCamera" && !x.IsStatic && x.ReturnType.Name == "Camera");
        var helper = m.Types.FirstOrDefault(t => t.Name == HelperType);
        if (helper != null && helper.Methods.Any(x => x.Name == "Copy"))
            return new Result { Outcome = Outcome.AlreadyApplied, Details = { $"{file}: the size the world is drawn at follows {Variable}; its copy to the screen takes the camera's rectangle" } };
        if (blits == null || blits.Count != 1 || srcCam == null)
            return Result.Failed($"{file}: CameraRenderScaledApply is not as this patch knows it (OnPreRender with one Graphics.Blit(texture, target), a SourceCamera)");
        if (helper != null)
        {
            var r0 = new Result { Outcome = Outcome.Applied };
            r0.Details.Add($"{file}: CameraRenderScaledApply.OnPreRender: the world's texture copied from the camera's rectangle (where the game draws bars)");
            if (!apply) return r0;
            AddCopy(m, helper, apl, blits[0], srcCam);
            game.MarkChanged(asm);
            return r0;
        }
        // static ScreenRes Resolution; Clear() stores new ScreenRes(0, 0) in it; a static constructor (the field initializers)
        var res = crs.Fields.FirstOrDefault(f => f.Name == "Resolution" && f.IsStatic && f.FieldType.Name == "ScreenRes");
        var clear = crs.Methods.FirstOrDefault(x => x.Name == "Clear" && x.IsStatic && x.HasBody);
        var ctor = clear?.Body.Instructions.Where(i => i.OpCode.Code is Code.Newobj or Code.Call).Select(i => i.Operand as MethodReference)
                        .FirstOrDefault(c => c != null && c.Name == ".ctor" && c.DeclaringType.Name == "ScreenRes" && c.Parameters.Count == 2 &&
                                             c.Parameters.All(p => p.ParameterType.FullName == "System.Int32"));
        var cctor = crs.Methods.FirstOrDefault(x => x.IsConstructor && x.IsStatic && x.HasBody);
        var end = cctor?.Body.Instructions.LastOrDefault();
        // OnActiveSceneChanged's one store: ForceFullResolution = <the new scene is a menu, the title or a cutscene>
        var scene = crs.Methods.FirstOrDefault(x => x.Name == "OnActiveSceneChanged" && !x.IsStatic && x.HasBody);
        var force = scene?.Body.Instructions.Where(i => i.OpCode.Code is Code.Call or Code.Callvirt && i.Operand is MethodReference c &&
                                                        c.Name == "set_ForceFullResolution" && c.DeclaringType.Name == "CameraRenderScaled").ToList();
        if (res == null || ctor == null || end == null || end.OpCode.Code != Code.Ret || cctor.Body.Instructions.Count(i => i.OpCode.Code == Code.Ret) != 1 ||
            force == null || force.Count != 1)
            return Result.Failed($"{file}: CameraRenderScaled is not as this patch knows it (static ScreenRes Resolution, Clear() with new ScreenRes(int, int), a static constructor, OnActiveSceneChanged with one ForceFullResolution = ...)");

        var r = new Result { Outcome = Outcome.Applied };
        r.Details.Add($"{file}: CameraRenderScaled.Resolution (the size the world is drawn at; 0x0, the screen's) follows {Variable} (<w>x<h>)");
        r.Details.Add($"{file}: CameraRenderScaled.OnActiveSceneChanged: with a size set, a menu's or cutscene scene's world is drawn at it too");
        r.Details.Add($"{file}: CameraRenderScaledApply.OnPreRender: the world's texture copied from the camera's rectangle (where the game draws bars)");
        if (!apply) return r;

        var (set, full) = Helper(m, res, ctor);
        var il = cctor.Body.GetILProcessor();
        // (the last ret may be a branch target: the call takes its place, a new ret follows)
        end.OpCode = OpCodes.Call; end.Operand = set;
        il.Append(il.Create(OpCodes.Ret));
        // ForceFullResolution = Full(<the game's value>)
        scene.Body.GetILProcessor().InsertBefore(force[0], Instruction.Create(OpCodes.Call, full));
        AddCopy(m, m.Types.First(t => t.Name == HelperType), apl, blits[0], srcCam);
        game.MarkChanged(asm);
        return r;
    }

    /// <summary>static void Copy(Texture src, RenderTexture dst, CameraRenderScaledApply self) in the helper, called in
    /// place of OnPreRender's Graphics.Blit(src, dst) (self pushed before it): Rect r = self.SourceCamera.rect;
    /// Graphics.Blit(src, dst, new Vector2(r.width, r.height), new Vector2(r.x, r.y)) (the whole texture for 0,0,1,1).</summary>
    static void AddCopy(ModuleDefinition m, TypeDefinition helper, TypeDefinition apl, Instruction blit, MethodDefinition srcCam)
    {
        var ts = m.TypeSystem;
        var orig = (MethodReference)blit.Operand;
        var graphics = orig.DeclaringType;
        var tex = orig.Parameters[0].ParameterType; var rtex = orig.Parameters[1].ParameterType;
        var cam = srcCam.ReturnType;
        var rect = new TypeReference("UnityEngine", "Rect", m, graphics.Scope, true);
        var vec2 = new TypeReference("UnityEngine", "Vector2", m, graphics.Scope, true);
        var getRect = new MethodReference("get_rect", rect, cam) { HasThis = true };
        MethodReference R(string n) => new MethodReference(n, ts.Single, rect) { HasThis = true };
        var v2 = new MethodReference(".ctor", ts.Void, vec2) { HasThis = true };
        v2.Parameters.Add(new ParameterDefinition(ts.Single)); v2.Parameters.Add(new ParameterDefinition(ts.Single));
        var blit4 = new MethodReference("Blit", ts.Void, graphics);
        foreach (var t in new[] { tex, rtex, vec2, vec2 }) blit4.Parameters.Add(new ParameterDefinition(t));

        var md = new MethodDefinition("Copy", MethodAttributes.Assembly | MethodAttributes.Static | MethodAttributes.HideBySig, ts.Void);
        md.Parameters.Add(new ParameterDefinition("src", ParameterAttributes.None, tex));
        md.Parameters.Add(new ParameterDefinition("dst", ParameterAttributes.None, rtex));
        md.Parameters.Add(new ParameterDefinition("self", ParameterAttributes.None, apl));
        helper.Methods.Add(md);
        var r = new VariableDefinition(rect);
        md.Body.Variables.Add(r);
        md.Body.InitLocals = true;
        var il = md.Body.GetILProcessor();
        il.Emit(OpCodes.Ldarg_2); il.Emit(OpCodes.Call, srcCam); il.Emit(OpCodes.Callvirt, getRect); il.Emit(OpCodes.Stloc, r);
        il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldarg_1);
        il.Emit(OpCodes.Ldloca, r); il.Emit(OpCodes.Call, R("get_width")); il.Emit(OpCodes.Ldloca, r); il.Emit(OpCodes.Call, R("get_height"));
        il.Emit(OpCodes.Newobj, v2);
        il.Emit(OpCodes.Ldloca, r); il.Emit(OpCodes.Call, R("get_x")); il.Emit(OpCodes.Ldloca, r); il.Emit(OpCodes.Call, R("get_y"));
        il.Emit(OpCodes.Newobj, v2);
        il.Emit(OpCodes.Call, blit4);
        il.Emit(OpCodes.Ret);
        md.Body.OptimizeMacros();

        var body = apl.Methods.First(x => x.Name == "OnPreRender" && x.HasBody).Body.GetILProcessor();
        body.InsertBefore(blit, body.Create(OpCodes.Ldarg_0));
        blit.Operand = md;
    }

    /// <summary>The helper class: static void Set(): Resolution = new ScreenRes(w, h) when the variable is "<w>x<h>", both
    /// above 0 (and sized = true); static bool Full(bool full): full && !sized.</summary>
    static (MethodDefinition set, MethodDefinition full) Helper(ModuleDefinition m, FieldReference res, MethodReference ctor)
    {
        var ts = m.TypeSystem;
        var t = new TypeDefinition("", HelperType, TypeAttributes.NotPublic | TypeAttributes.Abstract | TypeAttributes.Sealed |
                                                   TypeAttributes.BeforeFieldInit | TypeAttributes.Class, ts.Object);
        m.Types.Add(t);
        var str = new TypeReference("System", "String", m, ts.CoreLibrary);
        var env = new MethodReference("GetEnvironmentVariable", ts.String, new TypeReference("System", "Environment", m, ts.CoreLibrary));
        env.Parameters.Add(new ParameterDefinition(ts.String));
        var indexOf = new MethodReference("IndexOf", ts.Int32, str) { HasThis = true };
        indexOf.Parameters.Add(new ParameterDefinition(ts.Char));
        var head = new MethodReference("Substring", ts.String, str) { HasThis = true };
        head.Parameters.Add(new ParameterDefinition(ts.Int32)); head.Parameters.Add(new ParameterDefinition(ts.Int32));
        var tail = new MethodReference("Substring", ts.String, str) { HasThis = true };
        tail.Parameters.Add(new ParameterDefinition(ts.Int32));
        var parse = new MethodReference("TryParse", ts.Boolean, new TypeReference("System", "Int32", m, ts.CoreLibrary, true));
        parse.Parameters.Add(new ParameterDefinition(ts.String)); parse.Parameters.Add(new ParameterDefinition(new ByReferenceType(ts.Int32)));

        var sized = new FieldDefinition("sized", FieldAttributes.Private | FieldAttributes.Static, ts.Boolean);
        t.Fields.Add(sized);
        var md = new MethodDefinition("Set", MethodAttributes.Assembly | MethodAttributes.Static | MethodAttributes.HideBySig, ts.Void);
        t.Methods.Add(md);
        var s = new VariableDefinition(ts.String); var x = new VariableDefinition(ts.Int32);
        var w = new VariableDefinition(ts.Int32); var h = new VariableDefinition(ts.Int32);
        foreach (var v in new[] { s, x, w, h }) md.Body.Variables.Add(v);
        md.Body.InitLocals = true;
        //   string s = Environment.GetEnvironmentVariable(V); if (s == null) return;
        //   int x = s.IndexOf('x'); if (x < 1) return;
        //   if (!int.TryParse(s.Substring(0, x), out w) || !int.TryParse(s.Substring(x + 1), out h) || w < 1 || h < 1) return;
        //   CameraRenderScaled.Resolution = new ScreenRes(w, h); sized = true;
        var il = md.Body.GetILProcessor();
        var ret = il.Create(OpCodes.Ret);
        il.Emit(OpCodes.Ldstr, Variable); il.Emit(OpCodes.Call, env); il.Emit(OpCodes.Stloc, s);
        il.Emit(OpCodes.Ldloc, s); il.Emit(OpCodes.Brfalse, ret);
        il.Emit(OpCodes.Ldloc, s); il.Emit(OpCodes.Ldc_I4, (int)'x'); il.Emit(OpCodes.Callvirt, indexOf); il.Emit(OpCodes.Stloc, x);
        il.Emit(OpCodes.Ldloc, x); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Blt, ret);
        il.Emit(OpCodes.Ldloc, s); il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Ldloc, x); il.Emit(OpCodes.Callvirt, head);
        il.Emit(OpCodes.Ldloca, w); il.Emit(OpCodes.Call, parse); il.Emit(OpCodes.Brfalse, ret);
        il.Emit(OpCodes.Ldloc, s); il.Emit(OpCodes.Ldloc, x); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Add); il.Emit(OpCodes.Callvirt, tail);
        il.Emit(OpCodes.Ldloca, h); il.Emit(OpCodes.Call, parse); il.Emit(OpCodes.Brfalse, ret);
        il.Emit(OpCodes.Ldloc, w); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Blt, ret);
        il.Emit(OpCodes.Ldloc, h); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Blt, ret);
        il.Emit(OpCodes.Ldloc, w); il.Emit(OpCodes.Ldloc, h); il.Emit(OpCodes.Newobj, ctor); il.Emit(OpCodes.Stsfld, res);
        il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Stsfld, sized);
        il.Append(ret);
        md.Body.OptimizeMacros();

        //   static bool Full(bool full) => full && !sized;   (Resolution read first: CameraRenderScaled's static
        //   constructor, and with it Set(), has run by then whatever the caller touched before)
        var fd = new MethodDefinition("Full", MethodAttributes.Assembly | MethodAttributes.Static | MethodAttributes.HideBySig, ts.Boolean);
        fd.Parameters.Add(new ParameterDefinition("full", ParameterAttributes.None, ts.Boolean));
        t.Methods.Add(fd);
        il = fd.Body.GetILProcessor();
        var no = il.Create(OpCodes.Ldc_I4_0);
        il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Brfalse, no);
        il.Emit(OpCodes.Ldsfld, res); il.Emit(OpCodes.Pop);
        il.Emit(OpCodes.Ldsfld, sized); il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Ceq); il.Emit(OpCodes.Ret);
        il.Append(no); il.Emit(OpCodes.Ret);
        fd.Body.OptimizeMacros();
        return (md, fd);
    }
}
