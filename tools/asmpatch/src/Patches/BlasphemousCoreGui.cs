// blasphemous-core-gui (kind Optimize, opt-in). Blasphemous's Core (the game's root MonoBehaviour, alive the whole run)
// has an OnGUI that calls its systems' debug GUIs only while a system's ShowDebug is on:
//   for (i...) if (systems[i].ShowDebug) systems[i].OnGUI();
// ShowDebug is false unless the console's debug command calls Core.SetDebug(system, true) (the only writer). Unity still
// sends Core a Layout event before every other GUI event and builds GUILayout's groups for it each frame (garbage and
// time for a GUI that draws nothing in play). Patched: Core.Awake first sets this.useGUILayout = false (Unity's own
// switch: no Layout event, no layout groups), and Core.SetDebug, when asked to turn a system's debug on, first sets it
// back to true, before the system's OnGUI can run. With no debug GUI on, Core.OnGUI does nothing either way; with one
// on, the game's own behaviour. Checked at patch time: ShowDebug is written only in SetDebug, and Core.OnGUI reads only
// ShowDebug and calls OnGUI.
using Mono.Cecil;
using Mono.Cecil.Cil;
using Mono.Cecil.Rocks;

namespace AsmPatch;

sealed class BlasphemousCoreGui : Patch
{
    public override string Name => "blasphemous-core-gui";
    public override Kind Kind => Kind.Optimize;
    public override bool OptIn => true;
    public override string Summary =>
        "Blasphemous: Core's OnGUI (its systems' debug GUIs, off in play) without Unity's Layout pass until the console turns a debug GUI on (less garbage and time each frame)";

    public override Result Run(Game game, bool apply)
    {
        var asm = game.Load("Assembly-CSharp");
        var m = asm?.MainModule;
        var core = m?.GetType("Framework.Managers.Core");
        var gs = m?.GetType("Framework.Managers.GameSystem");
        var awake = core?.Methods.FirstOrDefault(x => x.Name == "Awake" && x.Parameters.Count == 0 && x.HasBody);
        var setDebug = core?.Methods.FirstOrDefault(x => x.Name == "SetDebug" && x.Parameters.Count == 2 && x.Parameters[1].ParameterType.FullName == "System.Boolean" && x.HasBody);
        var onGui = core?.Methods.FirstOrDefault(x => x.Name == "OnGUI" && x.Parameters.Count == 0 && x.HasBody);
        var show = gs?.Fields.FirstOrDefault(f => f.Name == "ShowDebug" && f.FieldType.FullName == "System.Boolean");
        if (awake == null || setDebug == null || onGui == null || show == null)
            return Result.NotApplicable("no Blasphemous (Framework.Managers.Core Awake/SetDebug/OnGUI, GameSystem.ShowDebug)");
        var file = game.FileOf(asm);
        if (awake.Body.Instructions.Take(4).Any(i => i.Operand is MethodReference mr && mr.Name == "set_useGUILayout"))
            return new Result { Outcome = Outcome.AlreadyApplied, Details = { $"{file}: Core's OnGUI without the Layout pass until a debug GUI is on" } };
        foreach (var a in game.GameAssemblies)
            foreach (var t in Cecil.AllTypes(a.MainModule))
                foreach (var md in t.Methods.Where(x => x.HasBody))
                    if (md != setDebug && md.Body.Instructions.Any(i => i.Operand is FieldReference fr && Cecil.SameField(fr, show) && i.OpCode.Code is Code.Stfld or Code.Ldflda))
                        return Result.NotApplicable($"{file}: {t.Name}.{md.Name} writes GameSystem.ShowDebug");
        var calls = onGui.Body.Instructions.Where(i => i.Operand is MethodReference).Select(i => ((MethodReference)i.Operand).Name).Distinct().ToList();
        if (calls.Any(n => n is not ("get_Item" or "get_Count" or "OnGUI")))
            return Result.NotApplicable($"{file}: Core.OnGUI calls {string.Join(", ", calls)} (not as this patch knows it)");
        var set = BlasphemousCatalogs.Lib(game, m, "UnityEngine.MonoBehaviour", "set_useGUILayout", "System.Boolean");
        if (set == null) return Result.NotApplicable("no MonoBehaviour.set_useGUILayout(bool)");
        var r = new Result { Outcome = Outcome.Applied };
        r.Details.Add($"{file}: Framework.Managers.Core: useGUILayout = false at Awake, true when SetDebug turns a debug GUI on");
        if (!apply) return r;
        var ab = awake.Body; ab.SimplifyMacros();
        var ail = ab.GetILProcessor(); var af = ab.Instructions[0];
        ail.InsertBefore(af, ail.Create(OpCodes.Ldarg_0)); ail.InsertBefore(af, ail.Create(OpCodes.Ldc_I4_0)); ail.InsertBefore(af, ail.Create(OpCodes.Call, set));
        ab.OptimizeMacros();
        var sb = setDebug.Body; sb.SimplifyMacros();
        var sil = sb.GetILProcessor(); var sf = sb.Instructions[0];
        sil.InsertBefore(sf, sil.Create(OpCodes.Ldarg_2)); sil.InsertBefore(sf, sil.Create(OpCodes.Brfalse, sf));
        sil.InsertBefore(sf, sil.Create(OpCodes.Ldarg_0)); sil.InsertBefore(sf, sil.Create(OpCodes.Ldc_I4_1)); sil.InsertBefore(sf, sil.Create(OpCodes.Call, set));
        sb.OptimizeMacros();
        game.MarkChanged(asm);
        return r;
    }
}
