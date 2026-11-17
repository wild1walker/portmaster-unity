// blasphemous-video-depth (kind Optimize, opt-in): Blasphemous's Cinematics.PlayVideo makes the render texture its
// videos play into once, `new RenderTexture(1920, 1080, 24)`, and keeps it for the rest of the run: 7.9 MB of colour
// and 7.9 MB of depth and stencil in the GPU's memory. Nothing draws into it with a depth test: the VideoPlayer
// writes its frames' colour (player.targetTexture) and a UI RawImage shows that colour (videoImage.texture). The
// patch makes it `new RenderTexture(1920, 1080, 0)`: the same colour, no depth buffer.
// Checked in the game's assemblies before it changes anything: the field is stored only there, from that
// constructor with those arguments, and every read of it goes to RawImage.texture, VideoPlayer.targetTexture or a
// null test (Object == / != null): no code reads its depth, its format or its depth buffer, or hands it on.
using Mono.Cecil;
using Mono.Cecil.Cil;

namespace AsmPatch;

sealed class BlasphemousVideoDepth : Patch
{
    public override string Name => "blasphemous-video-depth";
    public override Kind Kind => Kind.Optimize;
    public override bool OptIn => true;
    public override string Summary =>
        "Blasphemous's video render texture (1920x1080, kept for the whole run) without the depth buffer nothing uses (7.9 MB of the GPU's memory)";

    const string TypeName = "Framework.Managers.Cinematics", FieldName = "renderTexture";

    public override Result Run(Game game, bool apply)
    {
        var asm = game.Load("Assembly-CSharp");
        var t = asm?.MainModule.GetType(TypeName);
        var f = t?.Fields.FirstOrDefault(x => x.Name == FieldName && !x.IsStatic && x.FieldType.FullName == "UnityEngine.RenderTexture");
        if (f == null) return Result.NotApplicable($"no Blasphemous ({TypeName}.{FieldName} in Assembly-CSharp)");
        var file = game.FileOf(asm);
        Instruction depth = null;
        int stores = 0;
        foreach (var a in game.GameAssemblies)
            foreach (var ty in Cecil.AllTypes(a.MainModule))
                foreach (var m in ty.Methods.Where(x => x.HasBody))
                    foreach (var i in m.Body.Instructions)
                    {
                        if (!(i.Operand is FieldReference fr && Cecil.SameField(fr, f))) continue;
                        if (i.OpCode.Code == Code.Stfld)
                        {
                            stores++;
                            var ctor = i.Previous;
                            if (ctor?.OpCode.Code != Code.Newobj || ctor.Operand is not MethodReference c || c.DeclaringType.FullName != "UnityEngine.RenderTexture" ||
                                c.Parameters.Count != 3 || c.Parameters.Any(p => p.ParameterType.MetadataType != MetadataType.Int32))
                                return Result.NotApplicable($"{file}: {m.Name} stores {FieldName} from something other than new RenderTexture(w, h, depth)");
                            var d = ctor.Previous; var h = d?.Previous; var w = h?.Previous;
                            if (Cecil.ConstInt(w) != 1920 || Cecil.ConstInt(h) != 1080 || Cecil.ConstInt(d) is not int dv)
                                return Result.NotApplicable($"{file}: {m.Name}: not new RenderTexture(1920, 1080, <depth>)");
                            if (dv == 0) return new Result { Outcome = Outcome.AlreadyApplied, Details = { $"{file}: {TypeName}.{m.Name}: the video render texture has no depth buffer" } };
                            depth = d;
                        }
                        else if (i.OpCode.Code == Code.Ldfld)
                        {
                            var n = i.Next;
                            bool ok = n != null && (n.OpCode.Code is Code.Call or Code.Callvirt) && n.Operand is MethodReference mr &&
                                      ((mr.Name == "set_texture" && mr.DeclaringType.FullName == "UnityEngine.UI.RawImage") ||
                                       (mr.Name == "set_targetTexture" && mr.DeclaringType.FullName == "UnityEngine.Video.VideoPlayer"));
                            if (!ok && n?.OpCode.Code == Code.Ldnull && n.Next?.Operand is MethodReference eq &&
                                (eq.Name is "op_Equality" or "op_Inequality") && eq.DeclaringType.FullName == "UnityEngine.Object")
                                ok = true;
                            if (!ok) return Result.NotApplicable($"{file}: {ty.Name}.{m.Name} reads {FieldName} otherwise ({n})");
                        }
                        else return Result.NotApplicable($"{file}: {ty.Name}.{m.Name} takes {FieldName}'s address");
                    }
        if (stores != 1 || depth == null) return Result.NotApplicable($"{file}: {stores} stores of {FieldName}, not one");
        var r = new Result { Outcome = Outcome.Applied };
        r.Details.Add($"{file}: {TypeName}.PlayVideo: new RenderTexture(1920, 1080, {Cecil.ConstInt(depth)}) -> (1920, 1080, 0): no depth buffer (it is only drawn into by the VideoPlayer and shown by a RawImage)");
        if (!apply) return r;
        depth.OpCode = OpCodes.Ldc_I4_0;
        depth.Operand = null;
        game.MarkChanged(asm);
        return r;
    }
}
