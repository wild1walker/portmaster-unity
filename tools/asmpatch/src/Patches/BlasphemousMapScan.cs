// blasphemous-map-scan (kind Optimize, opt-in). Blasphemous's MapManager.OnLevelLoaded (a handler of
// LevelManager.OnLevelLoaded, in the frame the level's fade-in starts) fills its cache of the level's persistent
// objects with four scans,
//   FillSceneCacheObject<Door>(); FillSceneCacheObject<PrieDieu>(); FillSceneCacheObject<Gate>(); FillSceneCacheObject<Teleport>();
// each Object.FindObjectsOfType<T>() (= Resources.ConvertObjects<T>(FindObjectsOfType(typeof(T)))), which walks
// every loaded MonoBehaviour: on the RG34XX-SP 25 - 45 ms a scan, most of a 200 ms frame. All four types derive
// from PersistentObject; the patch scans once, FindObjectsOfType(typeof(PersistentObject)), and fills each type's
// list from that array in its order with the objects of that type (Type.IsInstanceOfType: T and classes deriving
// from it, as the scan for T has them), creating a type's list only when one is found, as FillSceneCacheObject
// does. Unity does not document FindObjectsOfType's order; the patch relies on the scan for a type returning, in
// the same order, the objects of that type the scan for a base type returns (one walk, filtered), and check mode
// checks that at every call.
// ASMPATCH_BLASPHEMOUS_MAP_SCAN (read once): "off" = the game's four scans; "check" = the patch's one scan, then the
// game's four, compared type by type and object by object in order: "[blasphemous-map-scan] check: the same ..."
// or "[blasphemous-map-scan] check DIFFERENT ..."; the game's lists are kept.
// Checked at patch time: OnLevelLoaded and FillSceneCacheObject as the game has them (IL hash), the four calls
// together after cacheObjects.Clear(), the four types deriving from PersistentObject.
using Mono.Cecil;
using Mono.Cecil.Cil;
using Mono.Cecil.Rocks;

namespace AsmPatch;

sealed class BlasphemousMapScan : Patch
{
    public override string Name => "blasphemous-map-scan";
    public override Kind Kind => Kind.Optimize;
    public override bool OptIn => true;
    public override string Summary =>
        "Blasphemous's MapManager.OnLevelLoaded finds the level's doors, prie-dieus, gates and teleports with one scan of the loaded objects instead of four (the same lists in the same order; opt-in)";

    public const string Variable = "ASMPATCH_BLASPHEMOUS_MAP_SCAN", HelperType = "<asmpatch>BlasphemousMapScan";
    const string MapType = "Framework.Managers.MapManager", PersistentType = "Framework.FrameworkCore.PersistentObject";
    internal static string LoadedHash = "28fca74849269bee", FillHash = "af3add2d274910fd";

    public override Result Run(Game game, bool apply)
    {
        var asm = game.Load("Assembly-CSharp");
        var m = asm?.MainModule;
        var map = m?.GetType(MapType);
        var po = m?.GetType(PersistentType);
        if (map == null || po == null) return Result.NotApplicable($"no Blasphemous ({MapType}, {PersistentType} in Assembly-CSharp)");
        var file = game.FileOf(asm);
        if (m.Types.Any(t => t.Name == HelperType))
            return new Result { Outcome = Outcome.AlreadyApplied, Details = { $"{file}: MapManager.OnLevelLoaded scans once" } };
        var why = Check(map, po, out var loaded, out var fill, out var calls, out var cache);
        if (why != null) return Result.NotApplicable($"{file}: {why}");
        var r = new Result { Outcome = Outcome.Applied };
        r.Details.Add($"{file}: MapManager.OnLevelLoaded fills its cache of {string.Join(", ", calls.Select(c => ((GenericInstanceMethod)c.Operand).GenericArguments[0].Name))} from one FindObjectsOfType(PersistentObject)");
        if (!apply) return r;

        var fillAll = Helper(game, m, map, po, fill, calls, cache);
        if (fillAll == null) return Result.Failed($"{file}: a class library method the helper calls is missing");
        var body = loaded.Body; body.SimplifyMacros();
        var il = body.GetILProcessor();
        // ldarg.0; call Fill<Door>; ldarg.0; call Fill<PrieDieu>; ... -> ldarg.0; call FillAll(this)
        calls[0].Operand = fillAll;
        foreach (var c in calls.Skip(1)) { il.Remove(c.Previous); il.Remove(c); }
        body.OptimizeMacros();
        game.MarkChanged(asm);
        return r;
    }

    static string Check(TypeDefinition map, TypeDefinition po, out MethodDefinition loaded, out MethodDefinition fill, out List<Instruction> calls, out FieldDefinition cache)
    {
        calls = new List<Instruction>();
        loaded = map.Methods.FirstOrDefault(x => x.Name == "OnLevelLoaded" && !x.IsStatic && x.HasBody && x.Parameters.Count == 2);
        fill = map.Methods.FirstOrDefault(x => x.Name == "FillSceneCacheObject" && !x.IsStatic && x.HasBody && x.HasGenericParameters && x.Parameters.Count == 0);
        cache = map.Fields.FirstOrDefault(f => f.Name == "cacheObjects" && !f.IsStatic &&
                                               f.FieldType.FullName == $"System.Collections.Generic.Dictionary`2<System.Type,System.Collections.Generic.List`1<{PersistentType}>>");
        if (loaded == null || fill == null || cache == null) return "no MapManager.OnLevelLoaded/FillSceneCacheObject<T>/cacheObjects";
        string hl = BlasphemousCatalogs.Shape(loaded), hf = BlasphemousCatalogs.Shape(fill);
        if (hl != LoadedHash || hf != FillHash)
            return $"MapManager.OnLevelLoaded/FillSceneCacheObject are not as this patch knows them (IL {hl}/{hf}, known {LoadedHash}/{FillHash})";
        var f0 = fill;
        var cs = loaded.Body.Instructions.Where(i => i.Operand is GenericInstanceMethod g && g.ElementMethod.Resolve() == f0).ToList();
        calls = cs;
        if (calls.Count != 4) return "OnLevelLoaded does not call FillSceneCacheObject four times";
        for (int k = 0; k < 4; k++)
        {
            var c = calls[k];
            if (c.Previous?.OpCode.Code != Code.Ldarg_0 || (k > 0 && c.Previous.Previous != calls[k - 1])) return "the four FillSceneCacheObject calls are not together";
            var ta = Cecil.TryResolve(((GenericInstanceMethod)c.Operand).GenericArguments[0]);
            if (ta == null || !Cecil.Hierarchy(ta).Any(b => b.FullName == PersistentType) || ta.HasGenericParameters) return "a FillSceneCacheObject type is not a PersistentObject";
        }
        if (loaded.Body.Instructions.Any(i => cs.Skip(1).Any(c => i.Operand == c || i.Operand == c.Previous))) return "a branch lands among the four calls";
        if (calls[0].Previous.Previous?.Operand is not MethodReference clr || clr.Name != "Clear") return "the four calls do not follow cacheObjects.Clear()";
        if (po.BaseType?.FullName != "UnityEngine.MonoBehaviour") return "PersistentObject is not a MonoBehaviour";
        return null;
    }

    static MethodDefinition Helper(Game game, ModuleDefinition m, TypeDefinition map, TypeDefinition po, MethodDefinition fill, List<Instruction> calls, FieldDefinition cache)
    {
        var ts = m.TypeSystem;
        const string S = "System.String", O = "System.Object";
        var env = BlasphemousCatalogs.Lib(game, m, "System.Environment", "GetEnvironmentVariable", S);
        var strEq = BlasphemousCatalogs.Lib(game, m, S, "op_Equality", S, S);
        var concatO = BlasphemousCatalogs.Lib(game, m, S, "Concat", O + "[]");
        var concat2 = BlasphemousCatalogs.Lib(game, m, S, "Concat", S, S);
        var log = BlasphemousCatalogs.Lib(game, m, "UnityEngine.Debug", "Log", O);
        var find = BlasphemousCatalogs.Lib(game, m, "UnityEngine.Object", "FindObjectsOfType", "System.Type");
        var instId = BlasphemousCatalogs.Lib(game, m, "UnityEngine.Object", "GetInstanceID");
        var fromHandle = BlasphemousCatalogs.Lib(game, m, "System.Type", "GetTypeFromHandle", "System.RuntimeTypeHandle");
        var isInst = BlasphemousCatalogs.Lib(game, m, "System.Type", "IsInstanceOfType", O);
        var typeName = BlasphemousCatalogs.Lib(game, m, "System.Reflection.MemberInfo", "get_Name");
        if (new[] { env, strEq, concatO, concat2, log, find, instId, fromHandle, isInst, typeName }.Any(x => x == null)) return null;
        // the dictionary's and the list's methods as FillSceneCacheObject calls them
        MethodReference From(MethodDefinition md, string name) =>
            (MethodReference)md.Body.Instructions.FirstOrDefault(i => i.Operand is MethodReference r && r.Name == name &&
                                                                      (r.DeclaringType.FullName == cache.FieldType.FullName || r.DeclaringType.FullName.StartsWith("System.Collections.Generic.List`1<" + PersistentType)))?.Operand;
        var containsKey = From(fill, "ContainsKey"); var setItem = From(fill, "set_Item"); var getItem = From(fill, "get_Item");
        var listNew = From(fill, ".ctor"); var listAdd = From(fill, "Add");
        if (new[] { containsKey, setItem, getItem, listNew, listAdd }.Any(x => x == null)) return null;
        var dictT = cache.FieldType;
        var listT = listNew.DeclaringType;
        MethodReference ListM(string name, TypeReference ret, params TypeReference[] ps)
        {
            var r = new MethodReference(name, ret, listT) { HasThis = true };
            foreach (var p in ps) r.Parameters.Add(new ParameterDefinition(p));
            return m.ImportReference(r);
        }
        var listGen = ((GenericInstanceType)listT).ElementType;
        var t0 = listGen.GenericParameters.Count > 0 ? listGen.GenericParameters[0] : new GenericParameter("T", listGen);
        var listCount = ListM("get_Count", ts.Int32);
        var listItem = ListM("get_Item", t0, ts.Int32);
        var dictCount = new MethodReference("get_Count", ts.Int32, dictT) { HasThis = true };
        var dictClear = (MethodReference)calls[0].Previous.Previous.Operand;
        var typeT = fromHandle.ReturnType;
        var objArr = find.ReturnType;
        var types = calls.Select(c => ((GenericInstanceMethod)c.Operand).GenericArguments[0]).ToList();

        // the mode, in a class of its own
        var t = new TypeDefinition("", HelperType, TypeAttributes.NotPublic | TypeAttributes.Abstract | TypeAttributes.Sealed |
                                                   TypeAttributes.BeforeFieldInit | TypeAttributes.Class, ts.Object);
        m.Types.Add(t);
        var mode = new FieldDefinition("mode", FieldAttributes.Assembly | FieldAttributes.Static, ts.Int32);   // 0 unread, 1 on, 2 check, 3 off
        t.Fields.Add(mode);
        const MethodAttributes Static = MethodAttributes.Assembly | MethodAttributes.Static | MethodAttributes.HideBySig;
        MethodDefinition Method(TypeDefinition on, string name, TypeReference ret, params (string n, TypeReference t)[] ps)
        {
            var md = new MethodDefinition(name, on == map ? MethodAttributes.Private | MethodAttributes.Static | MethodAttributes.HideBySig : Static, ret);
            foreach (var (n, pt) in ps) md.Parameters.Add(new ParameterDefinition(n, ParameterAttributes.None, pt));
            md.Body.InitLocals = true;
            on.Methods.Add(md);
            return md;
        }
        VariableDefinition Local(MethodDefinition md, TypeReference vt) { var v = new VariableDefinition(vt); md.Body.Variables.Add(v); return v; }
        void Log(ILProcessor il, params Action[] parts)
        {
            il.Emit(OpCodes.Ldc_I4, parts.Length); il.Emit(OpCodes.Newarr, ts.Object);
            for (int k = 0; k < parts.Length; k++) { il.Emit(OpCodes.Dup); il.Emit(OpCodes.Ldc_I4, k); parts[k](); il.Emit(OpCodes.Stelem_Ref); }
            il.Emit(OpCodes.Call, concatO); il.Emit(OpCodes.Call, log);
        }

        // static int Mode(): read once (off 3, check 2, else 1), said once
        var modeM = Method(t, "Mode", ts.Int32);
        {
            var v = Local(modeM, ts.String);
            var il = modeM.Body.GetILProcessor();
            var read = il.Create(OpCodes.Ldstr, Variable); var have = il.Create(OpCodes.Ldsfld, mode);
            il.Emit(OpCodes.Ldsfld, mode); il.Emit(OpCodes.Brfalse, read); il.Emit(OpCodes.Br, have);
            il.Append(read); il.Emit(OpCodes.Call, env); il.Emit(OpCodes.Stloc, v);
            var notOff = il.Create(OpCodes.Ldloc, v); var notCheck = il.Create(OpCodes.Ldc_I4_1);
            il.Emit(OpCodes.Ldloc, v); il.Emit(OpCodes.Ldstr, "off"); il.Emit(OpCodes.Call, strEq); il.Emit(OpCodes.Brfalse, notOff);
            il.Emit(OpCodes.Ldc_I4_3); il.Emit(OpCodes.Stsfld, mode); il.Emit(OpCodes.Br, have);
            il.Append(notOff); il.Emit(OpCodes.Ldstr, "check"); il.Emit(OpCodes.Call, strEq); il.Emit(OpCodes.Brfalse, notCheck);
            il.Emit(OpCodes.Ldc_I4_2); il.Emit(OpCodes.Stsfld, mode);
            Log(il, () => il.Emit(OpCodes.Ldstr, $"[blasphemous-map-scan] {Variable}=check: one scan, then the game's four, compared"));
            il.Emit(OpCodes.Br, have);
            il.Append(notCheck); il.Emit(OpCodes.Stsfld, mode);
            Log(il, () => il.Emit(OpCodes.Ldstr, "[blasphemous-map-scan] MapManager.OnLevelLoaded: one scan for its four types"));
            il.Append(have); il.Emit(OpCodes.Ret);
        }

        // static void <asmpatch>Add(MapManager m, Object[] all, Type t): the objects of type t in all, in its order
        var addM = Method(map, "<asmpatch>Add", ts.Void, ("m", map), ("all", objArr), ("t", typeT));
        {
            var i = Local(addM, ts.Int32); var o = Local(addM, ts.Object);
            var il = addM.Body.GetILProcessor();
            var cond = il.Create(OpCodes.Ldloc, i); var body = il.Create(OpCodes.Ldarg_1); var next = il.Create(OpCodes.Ldloc, i);
            var have = il.Create(OpCodes.Ldarg_0);
            il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Stloc, i); il.Emit(OpCodes.Br, cond);
            il.Append(body); il.Emit(OpCodes.Ldloc, i); il.Emit(OpCodes.Ldelem_Ref); il.Emit(OpCodes.Stloc, o);
            il.Emit(OpCodes.Ldarg_2); il.Emit(OpCodes.Ldloc, o); il.Emit(OpCodes.Callvirt, isInst); il.Emit(OpCodes.Brfalse, next);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, cache); il.Emit(OpCodes.Ldarg_2); il.Emit(OpCodes.Callvirt, containsKey); il.Emit(OpCodes.Brtrue, have);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, cache); il.Emit(OpCodes.Ldarg_2); il.Emit(OpCodes.Newobj, listNew); il.Emit(OpCodes.Callvirt, setItem);
            il.Append(have); il.Emit(OpCodes.Ldfld, cache); il.Emit(OpCodes.Ldarg_2); il.Emit(OpCodes.Callvirt, getItem);
            il.Emit(OpCodes.Ldloc, o); il.Emit(OpCodes.Castclass, po); il.Emit(OpCodes.Callvirt, listAdd);
            il.Append(next); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Add); il.Emit(OpCodes.Stloc, i);
            il.Append(cond); il.Emit(OpCodes.Ldarg_1); il.Emit(OpCodes.Ldlen); il.Emit(OpCodes.Conv_I4); il.Emit(OpCodes.Blt, body);
            il.Emit(OpCodes.Ret);
        }

        // static string <asmpatch>Sig(MapManager m): "<type>:<id>,<id>,...;" for the four types, then "n=<count of lists>"
        var sigM = Method(map, "<asmpatch>Sig", ts.String, ("m", map));
        {
            var s = Local(sigM, ts.String); var l = Local(sigM, listT); var i = Local(sigM, ts.Int32);
            var il = sigM.Body.GetILProcessor();
            il.Emit(OpCodes.Ldstr, ""); il.Emit(OpCodes.Stloc, s);
            foreach (var ty in types)
            {
                var none = il.Create(OpCodes.Ldloc, s);
                il.Emit(OpCodes.Ldloc, s); il.Emit(OpCodes.Ldtoken, ty); il.Emit(OpCodes.Call, fromHandle); il.Emit(OpCodes.Callvirt, typeName);
                il.Emit(OpCodes.Call, concat2); il.Emit(OpCodes.Ldstr, ":"); il.Emit(OpCodes.Call, concat2); il.Emit(OpCodes.Stloc, s);
                il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, cache); il.Emit(OpCodes.Ldtoken, ty); il.Emit(OpCodes.Call, fromHandle);
                il.Emit(OpCodes.Callvirt, containsKey); il.Emit(OpCodes.Brfalse, none);
                il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, cache); il.Emit(OpCodes.Ldtoken, ty); il.Emit(OpCodes.Call, fromHandle);
                il.Emit(OpCodes.Callvirt, getItem); il.Emit(OpCodes.Stloc, l);
                var cond = il.Create(OpCodes.Ldloc, i); var body = il.Create(OpCodes.Ldloc, s);
                il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Stloc, i); il.Emit(OpCodes.Br, cond);
                il.Append(body);
                il.Emit(OpCodes.Ldloc, l); il.Emit(OpCodes.Ldloc, i); il.Emit(OpCodes.Callvirt, listItem); il.Emit(OpCodes.Callvirt, instId);
                il.Emit(OpCodes.Box, ts.Int32); il.Emit(OpCodes.Ldstr, ",");
                // s = string.Concat(new object[] { s, id, "," })
                var arrS = Local(sigM, ts.Object); var arrI = Local(sigM, ts.Object);
                il.Emit(OpCodes.Stloc, arrS); il.Emit(OpCodes.Stloc, arrI);   // (arrS holds ",", arrI the id, s on the stack)
                var sv = Local(sigM, ts.Object); il.Emit(OpCodes.Stloc, sv);
                il.Emit(OpCodes.Ldc_I4_3); il.Emit(OpCodes.Newarr, ts.Object);
                il.Emit(OpCodes.Dup); il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Ldloc, sv); il.Emit(OpCodes.Stelem_Ref);
                il.Emit(OpCodes.Dup); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Ldloc, arrI); il.Emit(OpCodes.Stelem_Ref);
                il.Emit(OpCodes.Dup); il.Emit(OpCodes.Ldc_I4_2); il.Emit(OpCodes.Ldloc, arrS); il.Emit(OpCodes.Stelem_Ref);
                il.Emit(OpCodes.Call, concatO); il.Emit(OpCodes.Stloc, s);
                il.Emit(OpCodes.Ldloc, i); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Add); il.Emit(OpCodes.Stloc, i);
                il.Append(cond); il.Emit(OpCodes.Ldloc, l); il.Emit(OpCodes.Callvirt, listCount); il.Emit(OpCodes.Blt, body);
                il.Append(none); il.Emit(OpCodes.Ldstr, ";"); il.Emit(OpCodes.Call, concat2); il.Emit(OpCodes.Stloc, s);
            }
            il.Emit(OpCodes.Ldloc, s); il.Emit(OpCodes.Ldstr, "n=");
            il.Emit(OpCodes.Call, concat2);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, cache); il.Emit(OpCodes.Callvirt, m.ImportReference(dictCount));
            var cnt = Local(sigM, ts.Int32); il.Emit(OpCodes.Stloc, cnt); il.Emit(OpCodes.Ldloca, cnt);
            il.Emit(OpCodes.Call, BlasphemousCatalogs.Lib(game, m, "System.Int32", "ToString"));
            il.Emit(OpCodes.Call, concat2); il.Emit(OpCodes.Ret);
        }

        // static void <asmpatch>FillAll(MapManager m)
        //   mode = Mode(); if (mode == 3) goto game; all = FindObjectsOfType(typeof(PersistentObject)); Add(m, all, typeof(T)) for each;
        //   if (mode == 1) return; mine = Sig(m); m.cacheObjects.Clear();
        //   game: m.Fill<T>() for each; if (mode == 2) log (Sig(m) == mine ? the same : DIFFERENT)
        var fillAll = Method(map, "<asmpatch>FillAll", ts.Void, ("m", map));
        {
            var mv = Local(fillAll, ts.Int32); var all = Local(fillAll, objArr); var mine = Local(fillAll, ts.String); var theirs = Local(fillAll, ts.String);
            var il = fillAll.Body.GetILProcessor();
            var game1 = il.Create(OpCodes.Ldarg_0);
            il.Emit(OpCodes.Call, modeM); il.Emit(OpCodes.Dup); il.Emit(OpCodes.Stloc, mv); il.Emit(OpCodes.Ldc_I4_3); il.Emit(OpCodes.Beq, game1);
            il.Emit(OpCodes.Ldtoken, po); il.Emit(OpCodes.Call, fromHandle); il.Emit(OpCodes.Call, find); il.Emit(OpCodes.Stloc, all);
            foreach (var ty in types)
            {
                il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldloc, all); il.Emit(OpCodes.Ldtoken, ty); il.Emit(OpCodes.Call, fromHandle); il.Emit(OpCodes.Call, addM);
            }
            var chk = il.Create(OpCodes.Ldarg_0);
            il.Emit(OpCodes.Ldloc, mv); il.Emit(OpCodes.Ldc_I4_2); il.Emit(OpCodes.Beq, chk); il.Emit(OpCodes.Ret);
            il.Append(chk); il.Emit(OpCodes.Call, sigM); il.Emit(OpCodes.Stloc, mine);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, cache); il.Emit(OpCodes.Callvirt, dictClear);
            il.Append(game1); il.Emit(OpCodes.Call, (MethodReference)calls[0].Operand);
            foreach (var c in calls.Skip(1)) { il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Call, (MethodReference)c.Operand); }
            var end = il.Create(OpCodes.Ret);
            il.Emit(OpCodes.Ldloc, mv); il.Emit(OpCodes.Ldc_I4_2); il.Emit(OpCodes.Bne_Un, end);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Call, sigM); il.Emit(OpCodes.Stloc, theirs);
            var diff = il.Create(OpCodes.Nop);
            il.Emit(OpCodes.Ldloc, theirs); il.Emit(OpCodes.Ldloc, mine); il.Emit(OpCodes.Call, strEq); il.Emit(OpCodes.Brfalse, diff);
            Log(il, () => il.Emit(OpCodes.Ldstr, "[blasphemous-map-scan] check: the same as the game's four scans: "), () => il.Emit(OpCodes.Ldloc, theirs));
            il.Emit(OpCodes.Ret);
            il.Append(diff);
            Log(il, () => il.Emit(OpCodes.Ldstr, "[blasphemous-map-scan] check DIFFERENT: the game's "), () => il.Emit(OpCodes.Ldloc, theirs),
                () => il.Emit(OpCodes.Ldstr, ", the patch's "), () => il.Emit(OpCodes.Ldloc, mine));
            il.Append(end);
        }
        return fillAll;
    }
}
