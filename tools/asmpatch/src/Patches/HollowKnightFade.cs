// hk-camerafade (kind Optimize, exact, opt-in). Hollow Knight's CameraFade draws its screen fade with IMGUI:
// OnGUI (every IMGUI event, two or more a frame) moves the overlay colour one step towards its target
// and writes it into a 1x1 texture, SetScreenOverlayColor: SetPixel(0, 0, colour), then Apply(),
// which uploads the texture again (glTexSubImage2D on the render thread). A CameraFade nothing has
// faded keeps the start state of its fields, colour (0,0,0,0), target (0,0,0,1), step 0: the two
// never meet, so every event writes and uploads the same transparent pixel, every frame of the game.
// The patch keeps the colour last written and the texture it went into, and SetScreenOverlayColor
// leaves out SetPixel and Apply when both are the ones it would write: the same texture, a colour
// equal in all four components (IEEE: 0 and -0 store the same byte; NaN never compares equal, so it
// is written as before). The overlay colour field is set first, as before; the fade's steps, the
// texture's pixel on the CPU and the GPU, and the overlay drawn are the game's. Only the texture's
// update count (Texture.updateCount, which Hollow Knight never reads) goes up less often. The patch
// checks that CameraFade's texture is a private field that only its Awake stores (a new texture),
// that SetScreenOverlayColor is exactly the store, the SetPixel and the Apply, and that every other
// read of the field only hands it to IMGUI to draw (GUIStyleState.background, GUI.Label); any other
// shape leaves the game as it is. Opt-in: on the handhelds the uploads go (Greenpath, RG34XX-SP: 2.8
// -> 0.3 glTexSubImage2D a frame) but no frame rate could tell it apart (README.md).
using Mono.Cecil;
using Mono.Cecil.Cil;

namespace AsmPatch;

sealed class HollowKnightFade : Patch
{
    public override string Name => "hk-camerafade";
    public override Kind Kind => Kind.Optimize;
    public override bool OptIn => true;
    public override string Summary =>
        "Hollow Knight's CameraFade writes and uploads its 1x1 fade texture only when the colour differs from the one in it (the fade and its pixel as before)";

    const string LastField = "<asmpatch>lastColour", TextureField = "<asmpatch>lastTexture";

    public override Result Run(Game game, bool apply)
    {
        var asm = game.Load("Assembly-CSharp");
        var fade = asm?.MainModule.GetType("CameraFade");
        if (fade == null) return Result.NotApplicable("no Hollow Knight CameraFade in Assembly-CSharp");
        var file = game.FileOf(asm);
        var where = $"{file}: CameraFade.SetScreenOverlayColor";
        if (fade.Fields.Any(f => f.Name == LastField))
            return new Result { Outcome = Outcome.AlreadyApplied, Details = { $"{where}: writes the texture only for a new colour" } };
        var set = fade.Methods.FirstOrDefault(m => m.Name == "SetScreenOverlayColor" && !m.IsStatic && m.Parameters.Count == 1 && m.HasBody &&
                                                   m.ReturnType.MetadataType == MetadataType.Void);
        var tex = fade.Fields.FirstOrDefault(f => f.Name == "fadeTexture" && !f.IsStatic && f.IsPrivate && f.FieldType.FullName == "UnityEngine.Texture2D");
        var cur = fade.Fields.FirstOrDefault(f => f.Name == "currentScreenOverlayColor" && !f.IsStatic && f.FieldType.FullName == "UnityEngine.Color");
        if (set == null || tex == null || cur == null || set.Parameters[0].ParameterType.FullName != "UnityEngine.Color")
            return Result.NotApplicable($"{file}: CameraFade has no SetScreenOverlayColor(Color), private Texture2D fadeTexture or Color currentScreenOverlayColor");
        var why = Check(fade, set, tex, cur);
        if (why != null) return Result.NotApplicable($"{where}: {why}");
        var colour = Cecil.TryResolve(set.Parameters[0].ParameterType);
        var rgba = new[] { "r", "g", "b", "a" }.Select(n => colour?.Fields.FirstOrDefault(f => f.Name == n && !f.IsStatic && f.FieldType.MetadataType == MetadataType.Single)).ToList();
        if (rgba.Any(f => f == null)) return Result.NotApplicable($"{file}: UnityEngine.Color (r, g, b, a) not found beside the game's assemblies");

        var r = new Result { Outcome = Outcome.Applied };
        r.Details.Add($"{where}: SetPixel and Apply only when the colour or the texture differs from the last written");
        if (!apply) return r;
        var m = asm.MainModule;
        var last = new FieldDefinition(LastField, FieldAttributes.Private, cur.FieldType);
        var lastTex = new FieldDefinition(TextureField, FieldAttributes.Private, tex.FieldType);
        fade.Fields.Add(last); fade.Fields.Add(lastTex);
        var comps = rgba.Select(f => m.ImportReference(f)).ToList();

        // stfld current (kept first); then: lastTexture != null && lastTexture == fadeTexture && the
        // four components equal -> ret; else the original SetPixel and Apply, then remember both
        var ins = set.Body.Instructions;
        var il = set.Body.GetILProcessor();
        var write = ins[3];                                    // ldarg.0 (ldfld fadeTexture for SetPixel)
        var ret = ins[ins.Count - 1];
        var test = new List<Instruction>
        {
            il.Create(OpCodes.Ldarg_0), il.Create(OpCodes.Ldfld, lastTex), il.Create(OpCodes.Brfalse, write),
            il.Create(OpCodes.Ldarg_0), il.Create(OpCodes.Ldfld, lastTex), il.Create(OpCodes.Ldarg_0), il.Create(OpCodes.Ldfld, tex),
            il.Create(OpCodes.Bne_Un, write),
        };
        foreach (var c in comps)
            test.AddRange(new[]
            {
                il.Create(OpCodes.Ldarg_0), il.Create(OpCodes.Ldflda, last), il.Create(OpCodes.Ldfld, c),
                il.Create(OpCodes.Ldarga_S, set.Parameters[0]), il.Create(OpCodes.Ldfld, c), il.Create(OpCodes.Bne_Un, write),
            });
        test.Add(il.Create(OpCodes.Ret));
        foreach (var x in test) il.InsertBefore(write, x);
        foreach (var x in new[]
        {
            il.Create(OpCodes.Ldarg_0), il.Create(OpCodes.Ldarg_0), il.Create(OpCodes.Ldfld, tex), il.Create(OpCodes.Stfld, lastTex),
            il.Create(OpCodes.Ldarg_0), il.Create(OpCodes.Ldarg_1), il.Create(OpCodes.Stfld, last),
        })
            il.InsertBefore(ret, x);
        set.Body.MaxStackSize = Math.Max(set.Body.MaxStackSize, 4);
        game.MarkChanged(asm);
        return r;
    }

    /// <summary>Null when CameraFade is as described above, else why not.</summary>
    static string Check(TypeDefinition fade, MethodDefinition set, FieldDefinition tex, FieldDefinition cur)
    {
        // SetScreenOverlayColor: current = c; fadeTexture.SetPixel(0, 0, current); fadeTexture.Apply();
        var i = set.Body.Instructions;
        bool Is(int k, Code c) => i[k].OpCode.Code == c;
        bool Fld(int k, Code c, FieldDefinition f) => Is(k, c) && Cecil.SameField(i[k].Operand, f);
        bool Call(int k, string name, int pars) => Is(k, Code.Callvirt) && i[k].Operand is MethodReference r && r.Name == name &&
            r.Parameters.Count == pars && r.DeclaringType.FullName == "UnityEngine.Texture2D" && r.ReturnType.MetadataType == MetadataType.Void;
        if (set.Body.ExceptionHandlers.Count != 0 || set.Body.Variables.Count != 0 || i.Count != 14 ||
            !Is(0, Code.Ldarg_0) || !Is(1, Code.Ldarg_1) || !Fld(2, Code.Stfld, cur) ||
            !Is(3, Code.Ldarg_0) || !Fld(4, Code.Ldfld, tex) || Cecil.ConstInt(i[5]) != 0 || Cecil.ConstInt(i[6]) != 0 ||
            !Is(7, Code.Ldarg_0) || !Fld(8, Code.Ldfld, cur) || !Call(9, "SetPixel", 3) ||
            !Is(10, Code.Ldarg_0) || !Fld(11, Code.Ldfld, tex) || !Call(12, "Apply", 0) || !Is(13, Code.Ret) ||
            set.Body.Instructions.Any(x => x.OpCode.FlowControl is FlowControl.Branch or FlowControl.Cond_Branch))
            return "not the store, SetPixel and Apply this patch knows";
        // the texture: stored only in Awake, from a new Texture2D; read only by SetScreenOverlayColor's
        // two calls and handed only to IMGUI's drawing (a style's background, a label's image). The
        // styles that may hold it are private and only drawn with or given the background.
        var styles = fade.Fields.Where(f => f.FieldType.FullName == "UnityEngine.GUIStyle").ToList();
        if (styles.Any(f => !f.IsPrivate || f.IsStatic)) return "a GUIStyle field of CameraFade is not private";
        bool Draws(MethodReference r) => r.DeclaringType.FullName == "UnityEngine.GUI" && r.Name == "Label";
        bool SetsBackground(MethodReference r) => r.DeclaringType.FullName == "UnityEngine.GUIStyleState" && r.Name == "set_background";
        foreach (var m in Types(fade).SelectMany(t => t.Methods).Where(x => x.HasBody))
        {
            var b = m.Body.Instructions;
            for (int k = 0; k < b.Count; k++)
            {
                var x = b[k];
                if (x.OpCode.Code is Code.Ldflda or Code.Ldsflda && (Cecil.SameField(x.Operand, tex) || styles.Any(s => Cecil.SameField(x.Operand, s))))
                    return "the address of fadeTexture or of a GUIStyle field is taken";
                if (x.OpCode.Code == Code.Stfld && Cecil.SameField(x.Operand, tex))
                {
                    if (m.Name != "Awake" || m.DeclaringType != fade || k == 0 || b[k - 1].OpCode.Code != Code.Newobj ||
                        ((MethodReference)b[k - 1].Operand).DeclaringType.FullName != "UnityEngine.Texture2D")
                        return $"fadeTexture is stored in {m.Name} other than as a new Texture2D in Awake";
                    continue;
                }
                if (x.OpCode.Code != Code.Ldfld) continue;
                if (Cecil.SameField(x.Operand, tex))
                {
                    if (m == set) continue;                    // its two calls, checked above
                    if (Callee(b, Consumer(b, k)) is not MethodReference r || !Draws(r) && !SetsBackground(r))
                        return $"fadeTexture is read in {m.Name} for something other than IMGUI's drawing";
                }
                else if (styles.Any(s => Cecil.SameField(x.Operand, s)))
                {
                    // GUI.Label(..., style), or style.normal.background = ...
                    int at = Consumer(b, k);
                    var c = Callee(b, at);
                    if (c != null && Draws(c)) continue;
                    if (c == null || c.DeclaringType.FullName != "UnityEngine.GUIStyle" || c.Name != "get_normal" ||
                        Callee(b, Consumer(b, at)) is not MethodReference s2 || !SetsBackground(s2))
                        return $"a GUIStyle of CameraFade is read in {m.Name} for something other than drawing or setting its background";
                }
            }
        }
        return null;
    }

    static IEnumerable<TypeDefinition> Types(TypeDefinition t)
    {
        yield return t;
        foreach (var n in t.NestedTypes)
            foreach (var x in Types(n)) yield return x;
    }

    static MethodReference Callee(Mono.Collections.Generic.Collection<Instruction> b, int at) =>
        at >= 0 ? b[at].Operand as MethodReference : null;

    /// <summary>The index of the call the value pushed at k is passed to, following the stack: the first
    /// call whose arguments reach back to it (no branch, duplicate or other consumer in between), else -1.</summary>
    static int Consumer(Mono.Collections.Generic.Collection<Instruction> b, int k)
    {
        int depth = 1;                                         // values above and including ours
        for (int j = k + 1; j < b.Count; j++)
        {
            var x = b[j];
            if (x.OpCode.FlowControl is FlowControl.Branch or FlowControl.Cond_Branch or FlowControl.Return or FlowControl.Throw) return -1;
            if (x.OpCode.Code is Code.Dup or Code.Pop) return -1;
            int pop, push;
            if (x.OpCode.FlowControl == FlowControl.Call && x.Operand is MethodReference r)
            {
                pop = r.Parameters.Count + (r.HasThis && x.OpCode.Code != Code.Newobj ? 1 : 0);
                push = r.ReturnType.MetadataType == MetadataType.Void && x.OpCode.Code != Code.Newobj ? 0 : 1;
                if (pop >= depth) return j;
            }
            else
            {
                pop = x.OpCode.StackBehaviourPop switch
                {
                    StackBehaviour.Pop0 => 0, StackBehaviour.Pop1 or StackBehaviour.Popi or StackBehaviour.Popref => 1,
                    StackBehaviour.Pop1_pop1 or StackBehaviour.Popi_pop1 or StackBehaviour.Popi_popi or StackBehaviour.Popi_popi8 or
                        StackBehaviour.Popi_popr4 or StackBehaviour.Popi_popr8 or StackBehaviour.Popref_pop1 or StackBehaviour.Popref_popi => 2,
                    StackBehaviour.Popi_popi_popi or StackBehaviour.Popref_popi_popi or StackBehaviour.Popref_popi_popi8 or
                        StackBehaviour.Popref_popi_popr4 or StackBehaviour.Popref_popi_popr8 or StackBehaviour.Popref_popi_popref => 3,
                    _ => -1,
                };
                push = x.OpCode.StackBehaviourPush switch
                {
                    StackBehaviour.Push0 => 0, StackBehaviour.Push1_push1 => 2, _ => 1,
                };
                if (pop < 0 || pop >= depth) return -1;        // unknown, or consumed by something other than a call
            }
            depth += push - pop;
        }
        return -1;
    }
}
