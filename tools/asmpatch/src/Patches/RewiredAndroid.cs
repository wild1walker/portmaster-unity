// rewired-android: Rewired (Guavaman Enterprises' input library) decides its platform once, in
// InputManager.DetectPlatform, from compile-time symbols of the build it was compiled into. A
// desktop build says Windows, OSX or Linux, and on Unity's Android player Rewired then looks for that
// platform's input APIs (Linux: libudev; macOS: IOKit) and finds no pad. Android builds of the game
// set platform = Android and read pads through Unity's input, which the Android player has.
//
// The patch keeps DetectPlatform as compiled and stores Android (and no editor / web player
// platform, not the editor) right before each of its returns. Every constant is the enum's own
// value looked up by name, so Rewired versions that number their platforms differently patch the
// same way. It works on the source edition (InputManager compiled into Assembly-CSharp) and on
// DLL editions (InputManager in one of Rewired's own assemblies) alike: any class deriving from
// Rewired.InputManager_Base whose DetectPlatform has a body.
//
// pmunity android-data adds the Joy<j>Axis<a> axes Rewired's Unity input source reads
// (--rewired-joysticks).
using Mono.Cecil;
using Mono.Cecil.Cil;
using Mono.Cecil.Rocks;

namespace AsmPatch;

sealed class RewiredAndroid : Patch
{
    public override string Name => "rewired-android";
    public override string Summary =>
        "Rewired's InputManager.DetectPlatform reports Android, so Rewired reads pads through Unity's input as in the game's Android builds";

    const string BaseName = "Rewired.InputManager_Base";

    // field, enum constant (null: the bool false)
    static readonly (string field, string constant)[] Assign =
    {
        ("platform", "Android"),
        ("editorPlatform", "None"),
        ("webplayerPlatform", "None"),
        ("isEditor", null),
    };

    public override Result Run(Game game, bool apply)
    {
        var targets = new List<(AssemblyDefinition asm, TypeDefinition type, MethodDefinition method)>();
        foreach (var asm in game.GameAssemblies)
            foreach (var t in Cecil.AllTypes(asm.MainModule))
            {
                if (!t.IsClass || t.FullName == BaseName) continue;
                var m = t.Methods.FirstOrDefault(x => x.Name == "DetectPlatform" && !x.IsStatic && x.HasBody &&
                                                      x.Parameters.Count == 0 && x.ReturnType.MetadataType == MetadataType.Void);
                if (m == null || !Cecil.Hierarchy(t).Skip(1).Any(b => b.FullName == BaseName)) continue;
                targets.Add((asm, t, m));
            }
        if (targets.Count == 0) return Result.NotApplicable("no Rewired InputManager (a " + BaseName + " with a DetectPlatform body)");

        var r = new Result { Outcome = Outcome.AlreadyApplied };
        foreach (var (asm, type, method) in targets)
        {
            var where = $"{game.FileOf(asm)}: {type.FullName}.DetectPlatform";
            // the fields (InputManager_Base's, usually in Rewired_Core) and their values
            var sets = new List<(FieldDefinition f, int value, TypeDefinition e)>();
            foreach (var (fname, constant) in Assign)
            {
                var f = Cecil.Hierarchy(type).SelectMany(x => x.Fields).FirstOrDefault(x => !x.IsStatic && x.Name == fname);
                if (f == null)
                {
                    if (fname == "platform") return Result.Failed($"{where}: no 'platform' field in {type.FullName}'s base classes");
                    continue;                           // older/newer Rewired without it: nothing to reset
                }
                if (constant == null)
                {
                    if (f.FieldType.MetadataType == MetadataType.Boolean) sets.Add((f, 0, null));
                    continue;
                }
                var e = Cecil.TryResolve(f.FieldType);
                var v = Cecil.EnumValue(e, constant);
                if (v == null)
                {
                    if (fname == "platform") return Result.Failed($"{where}: {f.FieldType.FullName} has no '{constant}' constant (or does not resolve)");
                    continue;
                }
                sets.Add((f, v.Value, e));
            }
            var platform = sets[0];
            var body = method.Body;
            var rets = body.Instructions.Where(i => i.OpCode.Code == Code.Ret).ToList();
            if (rets.Count == 0) return Result.Failed($"{where}: no return");
            string was = WasPlatform(body, platform.f, platform.e);
            if (rets.All(ret => EndsWithPlatform(ret, platform.f, platform.value)))
            {
                r.Details.Add($"{where}: already sets platform = Android");
                continue;
            }
            r.Outcome = Outcome.Applied;
            var desc = $"{where}: platform {was} -> Android ({platform.value})" +
                       string.Concat(sets.Skip(1).Select(s => $", {s.f.Name} = {(s.e == null ? "false" : Cecil.EnumName(s.e, s.value))}"));
            if (!apply) { r.Details.Add(desc); continue; }

            body.SimplifyMacros();
            var il = body.GetILProcessor();
            var module = method.Module;
            foreach (var ret in rets)
            {
                var seq = new List<Instruction>();
                foreach (var (f, value, _) in sets)
                {
                    seq.Add(il.Create(OpCodes.Ldarg_0));
                    seq.Add(il.Create(OpCodes.Ldc_I4, value));
                    seq.Add(il.Create(OpCodes.Stfld, module.ImportReference(f)));
                }
                // the ret itself becomes the first new instruction: branches to it (and the end of a
                // handler right before it) now lead into the assignments
                ret.OpCode = seq[0].OpCode;
                ret.Operand = seq[0].Operand;
                var prev = ret;
                foreach (var x in seq.Skip(1).Append(il.Create(OpCodes.Ret))) { il.InsertAfter(prev, x); prev = x; }
            }
            body.OptimizeMacros();
            game.MarkChanged(asm);
            r.Details.Add(desc);
        }
        return r;
    }

    // the last constant DetectPlatform stores into platform, by name
    static string WasPlatform(MethodBody body, FieldDefinition platform, TypeDefinition e)
    {
        string was = "?";
        foreach (var i in body.Instructions)
            if (i.OpCode.Code == Code.Stfld && Cecil.SameField(i.Operand, platform) && i.Previous != null && Cecil.ConstInt(i.Previous) is int v)
                was = Cecil.EnumName(e, v);
        return was;
    }

    // ldarg.0; ldc.i4 <Android>; stfld platform among the assignments right before this ret
    static bool EndsWithPlatform(Instruction ret, FieldDefinition platform, int android)
    {
        for (var st = ret.Previous; st != null && st.OpCode.Code == Code.Stfld; )
        {
            var c = st.Previous;
            var self = c?.Previous;
            if (c == null || self == null || self.OpCode.Code != Code.Ldarg_0 || Cecil.ConstInt(c) is not int v) return false;
            if (Cecil.SameField(st.Operand, platform)) return v == android;
            st = self.Previous;
        }
        return false;
    }
}
