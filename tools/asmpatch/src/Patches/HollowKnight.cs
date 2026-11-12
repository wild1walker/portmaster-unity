// hk-blur (kind Reduce, opt-in: the picture changes, never the game play). Hollow Knight draws the
// far background with a camera of its own (LightBlurredBackground, on GameCameras) into a 576x360
// texture, blurs it in four passes (LightBlur; two with the game's own Shader Quality "Low") and
// shows it on the scene's blur planes: on the RG34XX-SP about 7.7 ms of a 29 ms GPU frame
// (docs/profiling.md, "Hollow Knight's blur and particles on 1 GB handhelds"). With the patch, the
// environment variable ASMPATCH_HK_BLUR (comma-separated words) chooses at run time:
//   low   two blur passes, as the game's own Shader Quality "Low" has it
//   half  the background camera and its blur at half the resolution (288x180): the same passes
//         over texels twice as large, so blurrier, the same layout ("half,low": both)
//   off   no background camera and no blur: the blur planes are hidden and the scene's camera
//         draws the background itself, sharp (the look of the NoBlur mod, credited in
//         docs/profiling.md)
//   off,hide  as off, and the scene's camera stops at the closest blur plane, as it does in the
//         game itself (LightBlurredBackground.UpdateCameraClipPlanes): what the game draws only
//         into the blurred texture is not drawn at all, and the camera's clear colour shows
//         behind the rest (docs/profiling.md, "What a room's scenery costs")
//   anything else, or unset: the game as it is.
// The variable is read when the game's camera rig starts (LightBlurredBackground and BlurPlane
// Awake), at each aspect change (the texture's size) and each blur (the passes); a port sets it per
// handheld (port.toml runtime_lowmem: on devices with 1 GB of RAM or less). Nothing else changes:
// no timing, physics, collision or FSM; with "hide" the far background's objects stay as they are
// (active, animated), only the camera's far clip plane is nearer.
// The patch has two stages, each with its helper class: an assembly patched before the "hide" word
// has the first only, and a run on it adds the second (a port's update step).
using Mono.Cecil;
using Mono.Cecil.Cil;
using Mono.Cecil.Rocks;

namespace AsmPatch;

sealed class HollowKnightBlur : Patch
{
    public override string Name => "hk-blur";
    public override Kind Kind => Kind.Reduce;
    public override bool OptIn => true;
    public override string Summary =>
        "Hollow Knight's blurred background with fewer passes, at half resolution, none or not drawn, chosen at run time by ASMPATCH_HK_BLUR=low|half|half,low|off|off,hide (the picture changes; opt-in)";

    public const string Variable = "ASMPATCH_HK_BLUR", HelperType = "<asmpatch>HollowKnight", HideType = "<asmpatch>HollowKnightHide",
                        ClipMethod = "<asmpatch>SceneClip";
    /// <summary>For the tests: the first stage only, as the tool was before the "hide" word.</summary>
    internal static bool OnlyBase;

    public override Result Run(Game game, bool apply)
    {
        var asm = game.Load("Assembly-CSharp");
        var m = asm?.MainModule;
        var lbb = m?.GetType("LightBlurredBackground");
        var plane = m?.GetType("BlurPlane");
        var blur = m?.GetType("LightBlur");
        if (lbb == null || plane == null || blur == null)
            return Result.NotApplicable("no Hollow Knight (LightBlurredBackground, LightBlur, BlurPlane in Assembly-CSharp)");
        var file = game.FileOf(asm);
        bool hasBase = m.Types.Any(t => t.Name == HelperType), hasHide = m.Types.Any(t => t.Name == HideType);
        if (hasBase && (hasHide || OnlyBase))
            return new Result { Outcome = Outcome.AlreadyApplied, Details = { $"{file}: the blur follows {Variable}" } };
        MethodDefinition M(TypeDefinition t, string n, int pars) => t.Methods.FirstOrDefault(x => x.Name == n && !x.IsStatic && x.Parameters.Count == pars && x.HasBody);
        MethodReference Called(MethodDefinition md, string n) =>
            md?.Body.Instructions.Select(i => i.Operand).OfType<MethodReference>().FirstOrDefault(x => x.Name == n);
        var awake = M(lbb, "Awake", 0);
        var aspect = M(lbb, "OnCameraAspectChanged", 1);
        var planeAwake = M(plane, "Awake", 0);
        var passes = M(blur, "get_BlurPassCount", 0);
        var height = lbb.Fields.FirstOrDefault(f => f.Name == "renderTextureHeight" && f.FieldType.MetadataType == MetadataType.Int32);
        var cancel = lbb.Fields.FirstOrDefault(f => f.Name == "cancelEnable" && f.FieldType.MetadataType == MetadataType.Boolean);
        var groups = blur.Fields.FirstOrDefault(f => f.Name == "passGroupCount" && f.FieldType.MetadataType == MetadataType.Int32);
        var renderer = plane.Fields.FirstOrDefault(f => f.Name == "meshRenderer");
        // MeshRenderer.enabled's setter, as BlurPlane.SetPlaneVisibility calls it
        var setEnabled = Called(M(plane, "SetPlaneVisibility", 1), "set_enabled");
        List<Instruction> reads = null, groupReads = null;
        if (!hasBase)
        {
            if (awake == null || aspect == null || planeAwake == null || passes == null || height == null || cancel == null || groups == null ||
                renderer == null || setEnabled == null)
                return Result.Failed($"{file}: LightBlurredBackground / LightBlur / BlurPlane are not as this patch knows them");
            reads = aspect.Body.Instructions.Where(i => i.OpCode.Code == Code.Ldfld && Cecil.SameField(i.Operand, height)).ToList();
            groupReads = passes.Body.Instructions.Where(i => i.OpCode.Code == Code.Ldfld && Cecil.SameField(i.Operand, groups)).ToList();
            if (reads.Count == 0 || groupReads.Count == 0)
                return Result.Failed($"{file}: OnCameraAspectChanged / BlurPassCount do not read renderTextureHeight / passGroupCount");
        }
        // "hide": the scene camera's clip plane as the game sets it (UpdateCameraClipPlanes, without the background camera)
        var late = M(lbb, "LateUpdate", 0);
        var disable = M(lbb, "OnDisable", 0);
        var clip = M(lbb, "UpdateCameraClipPlanes", 0);
        var scene = lbb.Fields.FirstOrDefault(f => f.Name == "sceneCamera");
        var background = lbb.Fields.FirstOrDefault(f => f.Name == "backgroundCamera");
        var distant = lbb.Fields.FirstOrDefault(f => f.Name == "distantFarClipPlane" && f.FieldType.MetadataType == MetadataType.Single);
        // Camera.farClipPlane's getter and setter, as OnEnable and UpdateCameraClipPlanes call them
        var getFar = Called(M(lbb, "OnEnable", 0), "get_farClipPlane");
        var setFar = Called(clip, "set_farClipPlane");
        MethodDefinition sceneClip = null;
        if (!OnlyBase)
        {
            if (awake == null || late == null || disable == null || clip == null || scene == null || background == null || distant == null || getFar == null || setFar == null ||
                getFar.DeclaringType.FullName != scene.FieldType.FullName || setFar.DeclaringType.FullName != scene.FieldType.FullName ||
                lbb.Methods.Any(x => x.Name == ClipMethod) || (sceneClip = SceneClip(clip, background)) == null)
                return Result.Failed($"{file}: LightBlurredBackground's clip planes (UpdateCameraClipPlanes, LateUpdate, OnDisable) are not as this patch knows them");
        }

        var r = new Result { Outcome = Outcome.Applied };
        if (!hasBase)
            r.Details.Add($"{file}: LightBlurredBackground (its texture height at {reads.Count} read(s), its start), LightBlur.BlurPassCount and " +
                          $"BlurPlane.Awake follow {Variable} (low, half, off)");
        if (!OnlyBase)
            r.Details.Add($"{file}: LightBlurredBackground's scene camera stops at the closest blur plane with {Variable}=off,hide " +
                          "(Awake, LateUpdate, OnDisable; what is behind the plane is not drawn)");
        if (!apply) return r;

        if (!hasBase)
        {
            var h = Helper(m);
            void Wrap(MethodDefinition md, List<Instruction> at, MethodDefinition f)
            {
                md.Body.SimplifyMacros();
                var il = md.Body.GetILProcessor();
                foreach (var i in at) il.InsertAfter(i, il.Create(OpCodes.Call, f));
                md.Body.OptimizeMacros();
            }
            Wrap(aspect, reads, h.height);           // the texture's height: h / 2 with "half"
            Wrap(passes, groupReads, h.groups);      // the pass groups: at most 1 with "low"
            // LightBlurredBackground.Awake: ... if (off) cancelEnable = true;   (OnEnable, LateUpdate, OnDisable then do nothing)
            BeforeReturns(awake, (p, end) => new[]
            {
                p.Create(OpCodes.Call, h.off), p.Create(OpCodes.Brfalse, end),
                p.Create(OpCodes.Ldarg_0), p.Create(OpCodes.Ldc_I4_1), p.Create(OpCodes.Stfld, cancel),
            });
            // BlurPlane.Awake: ... if (off) meshRenderer.enabled = false;
            BeforeReturns(planeAwake, (p, end) => new[]
            {
                p.Create(OpCodes.Call, h.off), p.Create(OpCodes.Brfalse, end),
                p.Create(OpCodes.Ldarg_0), p.Create(OpCodes.Ldfld, renderer), p.Create(OpCodes.Ldc_I4_0), p.Create(OpCodes.Callvirt, setEnabled),
            });
        }
        if (!OnlyBase)
        {
            var hide = HideHelper(m);
            lbb.Methods.Add(sceneClip);
            // with "off" the game's own OnEnable, LateUpdate and OnDisable do nothing (cancelEnable), so:
            // Awake: ... if (hide) distantFarClipPlane = sceneCamera.farClipPlane;      (as OnEnable has it)
            BeforeReturns(awake, (p, end) => new[]
            {
                p.Create(OpCodes.Call, hide), p.Create(OpCodes.Brfalse, end),
                p.Create(OpCodes.Ldarg_0), p.Create(OpCodes.Ldarg_0), p.Create(OpCodes.Ldfld, scene), p.Create(OpCodes.Callvirt, getFar),
                p.Create(OpCodes.Stfld, distant),
            });
            // LateUpdate: ... if (hide) <asmpatch>SceneClip();
            BeforeReturns(late, (p, end) => new[]
            {
                p.Create(OpCodes.Call, hide), p.Create(OpCodes.Brfalse, end),
                p.Create(OpCodes.Ldarg_0), p.Create(OpCodes.Call, sceneClip),
            });
            // OnDisable: ... if (hide) sceneCamera.farClipPlane = distantFarClipPlane;  (as the game's own has it)
            BeforeReturns(disable, (p, end) => new[]
            {
                p.Create(OpCodes.Call, hide), p.Create(OpCodes.Brfalse, end),
                p.Create(OpCodes.Ldarg_0), p.Create(OpCodes.Ldfld, scene), p.Create(OpCodes.Ldarg_0), p.Create(OpCodes.Ldfld, distant),
                p.Create(OpCodes.Callvirt, setFar),
            });
        }
        game.MarkChanged(asm);
        return r;
    }

    /// <summary>A copy of UpdateCameraClipPlanes without its statements on the background camera:
    /// each is ldarg.0, ldfld backgroundCamera ... set_nearClipPlane, straight after the scene
    /// camera's set_farClipPlane, with no branch in it or into it. Null when the method is not of
    /// that shape. The copy is not yet in the class.</summary>
    static MethodDefinition SceneClip(MethodDefinition clip, FieldDefinition background)
    {
        var src = clip.Body;
        if (src.HasExceptionHandlers || !clip.ReturnType.FullName.Equals("System.Void")) return null;
        var md = new MethodDefinition(ClipMethod, MethodAttributes.Private | MethodAttributes.HideBySig, clip.ReturnType);
        md.Body.InitLocals = src.InitLocals;
        foreach (var v in src.Variables) md.Body.Variables.Add(new VariableDefinition(v.VariableType));
        var copy = new Dictionary<Instruction, Instruction>();
        var list = new List<Instruction>();
        foreach (var i in src.Instructions)
        {
            if (i.Operand is ParameterDefinition) return null;
            var c = Instruction.Create(OpCodes.Nop);
            c.OpCode = i.OpCode;
            c.Operand = i.Operand is VariableDefinition v ? md.Body.Variables[v.Index] : i.Operand;
            copy[i] = c; list.Add(c);
        }
        var targets = new HashSet<Instruction>();
        foreach (var c in list)
        {
            if (c.Operand is Instruction t) c.Operand = copy[t];
            else if (c.Operand is Instruction[] ts) c.Operand = ts.Select(x => copy[x]).ToArray();
            if (c.Operand is Instruction to) targets.Add(to);
            else if (c.Operand is Instruction[] tos) targets.UnionWith(tos);
        }
        static bool Calls(Instruction i, string name) =>
            (i.OpCode.Code == Code.Callvirt || i.OpCode.Code == Code.Call) && i.Operand is MethodReference r && r.Name == name;
        int removed = 0;
        for (int at; (at = list.FindIndex(i => i.OpCode.Code == Code.Ldfld && Cecil.SameField(i.Operand, background))) >= 0; removed++)
        {
            int start = at - 1, end = list.FindIndex(at, i => Calls(i, "set_nearClipPlane"));
            if (start < 1 || end < 0 || list[start].OpCode.Code != Code.Ldarg_0 || !Calls(list[start - 1], "set_farClipPlane")) return null;
            for (int k = start; k <= end; k++)
                if (targets.Contains(list[k]) || list[k].Operand is Instruction || list[k].Operand is Instruction[]) return null;
            list.RemoveRange(start, end - start + 1);
        }
        if (removed == 0 || !list.Any(i => Calls(i, "set_farClipPlane")) || list.Any(i => i.Operand is FieldReference f && f.Name == background.Name))
            return null;
        var il = md.Body.GetILProcessor();
        foreach (var c in list) il.Append(c);
        return md;
    }

    /// <summary>Puts code before each ret of a void method (each ret becomes the code's first
    /// instruction; the code may branch to end, the new ret).</summary>
    static void BeforeReturns(MethodDefinition md, Func<ILProcessor, Instruction, Instruction[]> code)
    {
        var body = md.Body;
        body.SimplifyMacros();
        var il = body.GetILProcessor();
        foreach (var ret in body.Instructions.Where(i => i.OpCode.Code == Code.Ret).ToList())
        {
            var end = il.Create(OpCodes.Ret);
            var seq = code(il, end);
            ret.OpCode = seq[0].OpCode; ret.Operand = seq[0].Operand;
            var prev = ret;
            foreach (var x in seq.Skip(1).Append(end)) { il.InsertAfter(prev, x); prev = x; }
        }
        body.OptimizeMacros();
    }

    /// <summary>The helper class: bool Has(word) (word is one of the variable's comma-separated
    /// words), int Height(h) (h / 2 with "half"), int Groups(g) (at most 1 with "low"), bool Off().</summary>
    static (MethodDefinition height, MethodDefinition groups, MethodDefinition off) Helper(ModuleDefinition m)
    {
        var ts = m.TypeSystem;
        var t = new TypeDefinition("", HelperType, TypeAttributes.NotPublic | TypeAttributes.Abstract | TypeAttributes.Sealed |
                                                   TypeAttributes.BeforeFieldInit | TypeAttributes.Class, ts.Object);
        m.Types.Add(t);
        var (env, concat, contains) = Strings(m);
        MethodDefinition Static(string name, TypeReference ret, params TypeReference[] ps)
        {
            var md = new MethodDefinition(name, MethodAttributes.Assembly | MethodAttributes.Static | MethodAttributes.HideBySig, ret);
            foreach (var p in ps) md.Parameters.Add(new ParameterDefinition(p));
            t.Methods.Add(md);
            return md;
        }
        // static int Flags(): read once (the blur asks every frame): 1 | off 2 | half 4 | low 8
        //   if (flags == 0) { int f = 1; string v = Environment.GetEnvironmentVariable(V);
        //     if (v != null) { v = "," + v + ","; if (v.Contains(",off,")) f |= 2; ... } flags = f; } return flags;
        var flags = new FieldDefinition("flags", FieldAttributes.Private | FieldAttributes.Static, ts.Int32);
        t.Fields.Add(flags);
        var read = Static("Flags", ts.Int32);
        read.Body.InitLocals = true;
        var f = new VariableDefinition(ts.Int32); var v = new VariableDefinition(ts.String);
        read.Body.Variables.Add(f); read.Body.Variables.Add(v);
        var il = read.Body.GetILProcessor();
        var done = il.Create(OpCodes.Ldsfld, flags);
        var store = il.Create(OpCodes.Ldloc, f);
        il.Emit(OpCodes.Ldsfld, flags); il.Emit(OpCodes.Brtrue, done);
        il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Stloc, f);
        il.Emit(OpCodes.Ldstr, Variable); il.Emit(OpCodes.Call, env); il.Emit(OpCodes.Stloc, v);
        il.Emit(OpCodes.Ldloc, v); il.Emit(OpCodes.Brfalse, store);
        il.Emit(OpCodes.Ldstr, ","); il.Emit(OpCodes.Ldloc, v); il.Emit(OpCodes.Ldstr, ","); il.Emit(OpCodes.Call, concat); il.Emit(OpCodes.Stloc, v);
        foreach (var (word, bit) in new[] { ("off", 2), ("half", 4), ("low", 8) })
        {
            var next = il.Create(OpCodes.Nop);
            il.Emit(OpCodes.Ldloc, v); il.Emit(OpCodes.Ldstr, "," + word + ","); il.Emit(OpCodes.Callvirt, contains); il.Emit(OpCodes.Brfalse, next);
            il.Emit(OpCodes.Ldloc, f); il.Emit(OpCodes.Ldc_I4, bit); il.Emit(OpCodes.Or); il.Emit(OpCodes.Stloc, f);
            il.Append(next);
        }
        il.Append(store); il.Emit(OpCodes.Stsfld, flags);
        il.Append(done); il.Emit(OpCodes.Ret);
        MethodDefinition Bit(string name, int bit)        // static bool <name>() => (Flags() & bit) != 0;
        {
            var md = Static(name, ts.Boolean);
            var p = md.Body.GetILProcessor();
            p.Emit(OpCodes.Call, read); p.Emit(OpCodes.Ldc_I4, bit); p.Emit(OpCodes.And); p.Emit(OpCodes.Ldc_I4_0); p.Emit(OpCodes.Cgt_Un); p.Emit(OpCodes.Ret);
            return md;
        }
        var off = Bit("Off", 2);
        var half = Bit("Half", 4);
        var low = Bit("Low", 8);
        // static int Height(int h) => Half() ? h / 2 : h;
        var height = Static("Height", ts.Int32, ts.Int32);
        il = height.Body.GetILProcessor();
        var same = il.Create(OpCodes.Ldarg_0);
        il.Emit(OpCodes.Call, half); il.Emit(OpCodes.Brfalse_S, same);
        il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldc_I4_2); il.Emit(OpCodes.Div); il.Emit(OpCodes.Ret);
        il.Append(same); il.Emit(OpCodes.Ret);
        // static int Groups(int g) => Low() && g > 1 ? 1 : g;
        var groups = Static("Groups", ts.Int32, ts.Int32);
        il = groups.Body.GetILProcessor();
        var keep = il.Create(OpCodes.Ldarg_0);
        il.Emit(OpCodes.Call, low); il.Emit(OpCodes.Brfalse_S, keep);
        il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Ble_S, keep);
        il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Ret);
        il.Append(keep); il.Emit(OpCodes.Ret);
        foreach (var md in t.Methods) md.Body.OptimizeMacros();
        return (height, groups, off);
    }
    /// <summary>Environment.GetEnvironmentVariable(string), string.Concat(string, string, string) and
    /// string.Contains(string), as the helpers call them.</summary>
    static (MethodReference env, MethodReference concat, MethodReference contains) Strings(ModuleDefinition m)
    {
        var ts = m.TypeSystem;
        var env = new MethodReference("GetEnvironmentVariable", ts.String, new TypeReference("System", "Environment", m, ts.CoreLibrary));
        env.Parameters.Add(new ParameterDefinition(ts.String));
        var concat = new MethodReference("Concat", ts.String, ts.String);
        for (int k = 0; k < 3; k++) concat.Parameters.Add(new ParameterDefinition(ts.String));
        var contains = new MethodReference("Contains", ts.Boolean, ts.String) { HasThis = true };
        contains.Parameters.Add(new ParameterDefinition(ts.String));
        return (env, concat, contains);
    }

    /// <summary>The second helper class: bool On(), true when the variable has both "off" and "hide"
    /// (read once: LateUpdate asks every frame).</summary>
    static MethodDefinition HideHelper(ModuleDefinition m)
    {
        var ts = m.TypeSystem;
        var t = new TypeDefinition("", HideType, TypeAttributes.NotPublic | TypeAttributes.Abstract | TypeAttributes.Sealed |
                                                 TypeAttributes.BeforeFieldInit | TypeAttributes.Class, ts.Object);
        m.Types.Add(t);
        var (env, concat, contains) = Strings(m);
        // static bool On(): state 0 not read yet, 1 no, 2 yes
        //   if (state == 0) { int s = 1; string v = Environment.GetEnvironmentVariable(V);
        //     if (v != null) { v = "," + v + ","; if (v.Contains(",off,") && v.Contains(",hide,")) s = 2; } state = s; }
        //   return state == 2;
        var state = new FieldDefinition("state", FieldAttributes.Private | FieldAttributes.Static, ts.Int32);
        t.Fields.Add(state);
        var on = new MethodDefinition("On", MethodAttributes.Assembly | MethodAttributes.Static | MethodAttributes.HideBySig, ts.Boolean);
        t.Methods.Add(on);
        on.Body.InitLocals = true;
        var s = new VariableDefinition(ts.Int32); var v = new VariableDefinition(ts.String);
        on.Body.Variables.Add(s); on.Body.Variables.Add(v);
        var il = on.Body.GetILProcessor();
        var done = il.Create(OpCodes.Ldsfld, state);
        var store = il.Create(OpCodes.Ldloc, s);
        il.Emit(OpCodes.Ldsfld, state); il.Emit(OpCodes.Brtrue, done);
        il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Stloc, s);
        il.Emit(OpCodes.Ldstr, Variable); il.Emit(OpCodes.Call, env); il.Emit(OpCodes.Stloc, v);
        il.Emit(OpCodes.Ldloc, v); il.Emit(OpCodes.Brfalse, store);
        il.Emit(OpCodes.Ldstr, ","); il.Emit(OpCodes.Ldloc, v); il.Emit(OpCodes.Ldstr, ","); il.Emit(OpCodes.Call, concat); il.Emit(OpCodes.Stloc, v);
        foreach (var word in new[] { "off", "hide" })
        {
            il.Emit(OpCodes.Ldloc, v); il.Emit(OpCodes.Ldstr, "," + word + ","); il.Emit(OpCodes.Callvirt, contains); il.Emit(OpCodes.Brfalse, store);
        }
        il.Emit(OpCodes.Ldc_I4_2); il.Emit(OpCodes.Stloc, s);
        il.Append(store); il.Emit(OpCodes.Stsfld, state);
        il.Append(done); il.Emit(OpCodes.Ldc_I4_2); il.Emit(OpCodes.Ceq); il.Emit(OpCodes.Ret);
        on.Body.OptimizeMacros();
        return on;
    }
}
