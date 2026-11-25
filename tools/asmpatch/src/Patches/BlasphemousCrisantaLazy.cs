// blasphemous-crisanta-lazy (kind Optimize, opt-in). Crisanta's boss script (CrisantaBehaviour, twice in her arena's
// scene) names her two ending prefabs in executionPrefab and trueEndingExecutionPrefab, so the arena loads them and
// everything their animators reach: five sprite sheets, about 92 MB of textures, which only the fight's last moment
// shows. On a 1 GB handheld the arena's load runs out of memory (the BattleXP G350). pmu's lazyref step (convert.lazyref,
// docs/porting.md) clears those two references in the scene, leaves the prefabs and what they reach out of the
// scene's preload list and names each prefab in the game's resource table ("pmu-lazy/<name, lower case>"); this
// patch loads a cleared one by that name when the fight ends, before the game's own code reads it:
//   SubstituteForExecution():  first, for each of the two fields:
//       if (field == null) field = Resources.Load("pmu-lazy/<prefab>") as GameObject;
//   then the game's code, unchanged.
// With the scene as the game ships it (the fields set) the added lines do nothing. The same prefab is instantiated,
// so the picture and the game play are the same; the sheets load when the ending starts instead of with the arena.
// Checked in the game's assemblies before it changes anything: the two fields are never stored by code and are
// read only in SubstituteForExecution.
using Mono.Cecil;
using Mono.Cecil.Cil;

namespace AsmPatch;

sealed class BlasphemousCrisantaLazy : Patch
{
    public override string Name => "blasphemous-crisanta-lazy";
    public override Kind Kind => Kind.Optimize;
    public override bool OptIn => true;
    public override string Summary =>
        "Blasphemous: Crisanta's ending prefabs loaded by name when her fight ends, if pmu's lazyref step cleared the arena's references (92 MB of textures not loaded with the arena)";

    const string TypeName = "Gameplay.GameControllers.Bosses.Crisanta.CrisantaBehaviour", MethodName = "SubstituteForExecution";
    public const string Prefix = "pmu-lazy/";
    static readonly (string field, string prefab)[] Fields =
        { ("executionPrefab", "ACT_CrisantaExecution"), ("trueEndingExecutionPrefab", "ACT_TrueEndingCrisantaExecution") };

    public override Result Run(Game game, bool apply)
    {
        var asm = game.Load("Assembly-CSharp");
        var t = asm?.MainModule.GetType(TypeName);
        var sub = t?.Methods.FirstOrDefault(x => x.Name == MethodName && !x.IsStatic && x.Parameters.Count == 0 && x.HasBody);
        var fs = Fields.Select(x => t?.Fields.FirstOrDefault(f => f.Name == x.field && !f.IsStatic && f.FieldType.FullName == "UnityEngine.GameObject")).ToArray();
        if (sub == null || fs.Any(f => f == null)) return Result.NotApplicable($"no Blasphemous ({TypeName}.{MethodName} and its two prefab fields in Assembly-CSharp)");
        var file = game.FileOf(asm);
        if (sub.Body.Instructions.Any(i => i.Operand is string s && s.StartsWith(Prefix)))
            return new Result { Outcome = Outcome.AlreadyApplied, Details = { $"{file}: {TypeName}.{MethodName} loads a cleared prefab by name" } };
        foreach (var a in game.GameAssemblies)
            foreach (var ty in Cecil.AllTypes(a.MainModule))
                foreach (var m in ty.Methods.Where(x => x.HasBody))
                    foreach (var i in m.Body.Instructions)
                        if (i.Operand is FieldReference fr && fs.Any(f => Cecil.SameField(fr, f)) && (i.OpCode.Code != Code.Ldfld || m != sub))
                            return Result.NotApplicable($"{file}: {ty.Name}.{m.Name} uses {fr.Name} ({i.OpCode})");
        var mod = asm.MainModule;
        var eq = BlasphemousCatalogs.Lib(game, mod, "UnityEngine.Object", "op_Equality", "UnityEngine.Object", "UnityEngine.Object");
        var load = BlasphemousCatalogs.Lib(game, mod, "UnityEngine.Resources", "Load", "System.String");
        var goT = fs[0].FieldType;
        if (eq == null || load == null) return Result.NotApplicable("no UnityEngine.Object.op_Equality or Resources.Load(string) in the game's libraries");
        var r = new Result { Outcome = Outcome.Applied };
        for (int k = 0; k < fs.Length; k++)
            r.Details.Add($"{file}: {TypeName}.{MethodName}: if {fs[k].Name} is null, Resources.Load(\"{Prefix}{Fields[k].prefab.ToLowerInvariant()}\")");
        if (!apply) return r;
        var il = sub.Body.GetILProcessor();
        var first = sub.Body.Instructions[0];
        for (int k = 0; k < fs.Length; k++)
        {
            var skip = il.Create(OpCodes.Nop);
            foreach (var x in new[]
            {
                il.Create(OpCodes.Ldarg_0), il.Create(OpCodes.Ldfld, fs[k]), il.Create(OpCodes.Ldnull), il.Create(OpCodes.Call, eq),
                il.Create(OpCodes.Brfalse, skip),
                il.Create(OpCodes.Ldarg_0), il.Create(OpCodes.Ldstr, Prefix + Fields[k].prefab.ToLowerInvariant()), il.Create(OpCodes.Call, load),
                il.Create(OpCodes.Isinst, goT), il.Create(OpCodes.Stfld, fs[k]), skip,
            })
                il.InsertBefore(first, x);
        }
        game.MarkChanged(asm);
        return r;
    }
}
