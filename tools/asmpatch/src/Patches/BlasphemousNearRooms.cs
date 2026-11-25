// blasphemous-near-rooms (kind Optimize, opt-in). After every room change LevelManager.StreamingLevels loads, one after
// another, every room of the current zone that is not loaded (and keeps them: a room left is only deactivated), and
// unloads the rooms of zones that are neither the current one nor the last:
//   foreach (Level level in levels)
//       if (level.IsBundle && unloaded && IsInSameZone(level)) yield return level.Load(streaming: true);
//       else if (level.IsBundle && (loaded || deactivated) && !IsInSameZone(level) && !IsInLastZone(level)) yield return level.UnLoad();
// A zone is up to 26 rooms of 4 scenes each (Mother of Mothers, D06Z01: 107 scenes loaded in Crisanta's arena), more
// than a 1 GB handheld holds (the BattleXP G350: killed for low memory seconds after her arena starts). The patch
// replaces the three zone tests with one, "near": the current room, the room the player came from, the room of the
// active prie-dieu (where a death respawns), and every room a Door of the current room leads to (Door.targetScene,
// what SpawnFromDoor loads when the player walks through it). Near rooms are loaded as before, the others unloaded as
// rooms of another zone are. Walking through a door finds the next room loaded, as before; a teleport within the
// zone, or the first walk into a room the prie-dieu or the doors do not name, loads it then, as entering another zone
// always does. The rooms, their objects and the game play are the game's: only when a room's scenes are loaded
// changes. The near list is made once per room change (Object.FindObjectsOfType(typeof(Door)): the doors of the active
// scenes, the current room's) and kept while the current and last rooms stay the same.
// Checked at patch time: StreamingLevels' MoveNext calls IsInSameZone twice and IsInLastZone once, no other method
// does, and the fields and methods named here are the game's.
using Mono.Cecil;
using Mono.Cecil.Cil;
using Mono.Cecil.Rocks;

namespace AsmPatch;

sealed class BlasphemousNearRooms : Patch
{
    public override string Name => "blasphemous-near-rooms";
    public override Kind Kind => Kind.Optimize;
    public override bool OptIn => true;
    public override string Summary =>
        "Blasphemous: after a room change only the rooms near the current one stay loaded (the doors' rooms, the last room, the prie-dieu's) instead of the whole zone (opt-in)";

    const string LM = "Framework.Managers.LevelManager", LevelT = "Framework.FrameworkCore.Level", DoorT = "Tools.Level.Interactables.Door",
                 SpawnT = "Framework.Managers.SpawnManager", CoreT = "Framework.Managers.Core";
    const string NearName = "PmuNear", PrieName = "PmuPrieScene";

    public override Result Run(Game game, bool apply)
    {
        var asm = game.Load("Assembly-CSharp");
        var m = asm?.MainModule;
        var lm = m?.GetType(LM);
        var level = m?.GetType(LevelT);
        var door = m?.GetType(DoorT);
        var spawn = m?.GetType(SpawnT);
        var core = m?.GetType(CoreT);
        if (lm == null || level == null || door == null || spawn == null || core == null)
            return Result.NotApplicable("no Blasphemous (LevelManager, Level, Door, SpawnManager, Core in Assembly-CSharp)");
        var file = game.FileOf(asm);
        if (lm.Methods.Any(x => x.Name == NearName))
            return new Result { Outcome = Outcome.AlreadyApplied, Details = { $"{file}: LevelManager.StreamingLevels keeps the near rooms" } };
        var same = lm.Methods.FirstOrDefault(x => x.Name == "IsInSameZone" && !x.IsStatic && x.Parameters.Count == 1 && x.ReturnType.FullName == "System.Boolean");
        var last = lm.Methods.FirstOrDefault(x => x.Name == "IsInLastZone" && !x.IsStatic && x.Parameters.Count == 1 && x.ReturnType.FullName == "System.Boolean");
        var iter = lm.NestedTypes.FirstOrDefault(t => t.Name.Contains("<StreamingLevels>"));
        var move = iter?.Methods.FirstOrDefault(x => x.Name == "MoveNext" && x.HasBody);
        var cur = lm.Methods.FirstOrDefault(x => x.Name == "get_currentLevel" && !x.IsStatic);
        var lastGet = lm.Methods.FirstOrDefault(x => x.Name == "get_lastLevel" && !x.IsStatic);
        var lname = level.Methods.FirstOrDefault(x => x.Name == "get_LevelName" && !x.IsStatic && x.Parameters.Count == 0);
        var target = door.Fields.FirstOrDefault(f => f.Name == "targetScene" && !f.IsStatic && f.FieldType.FullName == "System.String");
        var prie = spawn.Fields.FirstOrDefault(f => f.Name == "activePrieDieuScene" && !f.IsStatic && f.FieldType.FullName == "System.String");
        var coreSpawn = core.Methods.FirstOrDefault(x => x.Name == "get_SpawnManager" && x.IsStatic && x.ReturnType.FullName == SpawnT);
        if (same == null || last == null || move == null || cur == null || lastGet == null || lname == null || target == null || prie == null || coreSpawn == null)
            return Result.NotApplicable($"{file}: LevelManager.StreamingLevels/IsInSameZone/IsInLastZone, Level.LevelName, Door.targetScene or SpawnManager.activePrieDieuScene not as this patch knows them");
        var calls = move.Body.Instructions.Where(i => i.Operand is MethodReference r && (r.Resolve() == same || r.Resolve() == last)).ToList();
        if (calls.Count(i => ((MethodReference)i.Operand).Resolve() == same) != 2 || calls.Count(i => ((MethodReference)i.Operand).Resolve() == last) != 1)
            return Result.NotApplicable($"{file}: StreamingLevels does not call IsInSameZone twice and IsInLastZone once");
        foreach (var t in Cecil.AllTypes(m))
            foreach (var md in t.Methods.Where(x => x.HasBody && x != move))
                if (md.Body.Instructions.Any(i => i.Operand is MethodReference r && (r.Resolve() == same || r.Resolve() == last)))
                    return Result.NotApplicable($"{file}: {t.Name}.{md.Name} calls the zone tests too");
        var concat3 = BlasphemousCatalogs.Lib(game, m, "System.String", "Concat", "System.String", "System.String", "System.String");
        var contains = BlasphemousCatalogs.Lib(game, m, "System.String", "Contains", "System.String");
        var typeOf = BlasphemousCatalogs.Lib(game, m, "System.Type", "GetTypeFromHandle", "System.RuntimeTypeHandle");
        var find = BlasphemousCatalogs.Lib(game, m, "UnityEngine.Object", "FindObjectsOfType", "System.Type");
        if (concat3 == null || contains == null || typeOf == null || find == null)
            return Result.NotApplicable("no String.Concat/Contains, Type.GetTypeFromHandle or Object.FindObjectsOfType(Type) in the game's libraries");
        var r = new Result { Outcome = Outcome.Applied };
        r.Details.Add($"{file}: LevelManager.StreamingLevels: IsInSameZone/IsInLastZone -> {NearName} (the current room, the last, the prie-dieu's, the doors' rooms)");
        if (!apply) return r;

        var ts = m.TypeSystem;
        // SpawnManager.PmuPrieScene(): activePrieDieuScene
        var pm = new MethodDefinition(PrieName, MethodAttributes.Assembly | MethodAttributes.HideBySig, ts.String);
        spawn.Methods.Add(pm);
        var pil = pm.Body.GetILProcessor();
        pil.Emit(OpCodes.Ldarg_0); pil.Emit(OpCodes.Ldfld, prie); pil.Emit(OpCodes.Ret);

        // LevelManager fields: the near list ("|A|B|...|") and the two rooms it was made for
        var nearF = new FieldDefinition("pmuNear", FieldAttributes.Private, ts.String);
        var forCur = new FieldDefinition("pmuNearCur", FieldAttributes.Private, level);
        var forLast = new FieldDefinition("pmuNearLast", FieldAttributes.Private, level);
        lm.Fields.Add(nearF); lm.Fields.Add(forCur); lm.Fields.Add(forLast);

        // bool PmuNear(Level level)
        var nm = new MethodDefinition(NearName, MethodAttributes.Assembly | MethodAttributes.HideBySig, ts.Boolean);
        nm.Parameters.Add(new ParameterDefinition("level", ParameterAttributes.None, level));
        nm.Body.InitLocals = true;
        lm.Methods.Add(nm);
        var vS = new VariableDefinition(ts.String);
        var vA = new VariableDefinition(new ArrayType(m.ImportReference(find.ReturnType.GetElementType())));
        var vI = new VariableDefinition(ts.Int32);
        var vT = new VariableDefinition(ts.String);
        var vP = new VariableDefinition(ts.String);
        foreach (var v in new[] { vS, vA, vI, vT, vP }) nm.Body.Variables.Add(v);
        var il = nm.Body.GetILProcessor();
        var test = il.Create(OpCodes.Nop);
        void Add(Action push)   // s = s + <string> + "|", for a non-null string
        {
            il.Emit(OpCodes.Ldloc, vS); push(); il.Emit(OpCodes.Ldstr, "|"); il.Emit(OpCodes.Call, concat3); il.Emit(OpCodes.Stloc, vS);
        }
        // if (pmuNear != null && pmuNearCur == currentLevel && pmuNearLast == lastLevel) goto test
        var build = il.Create(OpCodes.Nop);
        il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, nearF); il.Emit(OpCodes.Brfalse, build);
        il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, forCur); il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Call, cur); il.Emit(OpCodes.Bne_Un, build);
        il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, forLast); il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Call, lastGet); il.Emit(OpCodes.Bne_Un, build);
        il.Emit(OpCodes.Br, test);
        il.Append(build);
        // s = "|"; current (never null here: StreamingLevels runs for a loaded room), last, prie-dieu
        il.Emit(OpCodes.Ldstr, "|"); il.Emit(OpCodes.Stloc, vS);
        var noCur = il.Create(OpCodes.Nop);
        il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Call, cur); il.Emit(OpCodes.Brfalse, noCur);
        Add(() => { il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Call, cur); il.Emit(OpCodes.Callvirt, lname); });
        il.Append(noCur);
        var noLast = il.Create(OpCodes.Nop);
        il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Call, lastGet); il.Emit(OpCodes.Brfalse, noLast);
        Add(() => { il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Call, lastGet); il.Emit(OpCodes.Callvirt, lname); });
        il.Append(noLast);
        var noPrie = il.Create(OpCodes.Nop);
        il.Emit(OpCodes.Call, coreSpawn); il.Emit(OpCodes.Brfalse, noPrie);
        il.Emit(OpCodes.Call, coreSpawn); il.Emit(OpCodes.Callvirt, pm); il.Emit(OpCodes.Stloc, vP);
        il.Emit(OpCodes.Ldloc, vP); il.Emit(OpCodes.Brfalse, noPrie);
        Add(() => il.Emit(OpCodes.Ldloc, vP));
        il.Append(noPrie);
        // the doors: a = Object.FindObjectsOfType(typeof(Door)); for each, its targetScene
        il.Emit(OpCodes.Ldtoken, door); il.Emit(OpCodes.Call, typeOf); il.Emit(OpCodes.Call, find); il.Emit(OpCodes.Stloc, vA);
        il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Stloc, vI);
        var cond = il.Create(OpCodes.Ldloc, vI);
        var body = il.Create(OpCodes.Ldloc, vA);
        var next = il.Create(OpCodes.Ldloc, vI);
        il.Emit(OpCodes.Br, cond);
        il.Append(body);                                  // t = ((Door)a[i]).targetScene
        il.Emit(OpCodes.Ldloc, vI); il.Emit(OpCodes.Ldelem_Ref); il.Emit(OpCodes.Castclass, door); il.Emit(OpCodes.Ldfld, target); il.Emit(OpCodes.Stloc, vT);
        il.Emit(OpCodes.Ldloc, vT); il.Emit(OpCodes.Brfalse, next);
        Add(() => il.Emit(OpCodes.Ldloc, vT));
        il.Append(next);                                  // i++
        il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Add); il.Emit(OpCodes.Stloc, vI);
        il.Append(cond);                                  // i < a.Length
        il.Emit(OpCodes.Ldloc, vA); il.Emit(OpCodes.Ldlen); il.Emit(OpCodes.Conv_I4); il.Emit(OpCodes.Blt, body);
        // keep it, for these two rooms
        il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldloc, vS); il.Emit(OpCodes.Stfld, nearF);
        il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Call, cur); il.Emit(OpCodes.Stfld, forCur);
        il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Call, lastGet); il.Emit(OpCodes.Stfld, forLast);
        // return pmuNear.Contains("|" + level.LevelName + "|")
        il.Append(test);
        il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, nearF);
        il.Emit(OpCodes.Ldstr, "|"); il.Emit(OpCodes.Ldarg_1); il.Emit(OpCodes.Callvirt, lname); il.Emit(OpCodes.Ldstr, "|"); il.Emit(OpCodes.Call, concat3);
        il.Emit(OpCodes.Callvirt, contains);
        il.Emit(OpCodes.Ret);
        nm.Body.OptimizeMacros();

        // StreamingLevels: the three zone tests -> PmuNear (same signature: bool (Level) on the LevelManager)
        foreach (var c in calls) c.Operand = nm;
        game.MarkChanged(asm);
        return r;
    }
}
