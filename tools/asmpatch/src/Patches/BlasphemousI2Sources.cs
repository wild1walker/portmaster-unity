// blasphemous-i2-sources (kind Optimize, opt-in). I2 Localization's ResourceManager.MyOnLevelWasLoaded runs at every
// scene load (SceneManager.sceneLoaded) and calls LocalizationManager.UpdateSources, whose RegisterSceneSources is
//   LanguageSource[] a = (LanguageSource[])Resources.FindObjectsOfTypeAll(typeof(LanguageSource));
//   for (i...) if (!Sources.Contains(a[i])) AddSource(a[i]);
// FindObjectsOfTypeAll walks every object Unity has loaded (assets too): on the RG34XX-SP about 28 ms, at each of
// the scenes LevelManager.StreamingLevels loads one after another (a level's start: 38 - 41 fps for 4 s). The scan
// can only add a LanguageSource that is not in Sources yet; Sources loses an entry only through RemoveSource, which
// only UnregisterDeletededSources calls, for destroyed ones (checked here). Every LanguageSource object Unity makes
// (a scene's, a prefab asset's, a clone's, inactive or not) runs its constructor, so the patch keeps, from the
// constructor, every LanguageSource made (a list, under a lock: Unity constructs on its loading thread too), and
// RegisterSceneSources first drops from it those in Sources: when none is left, the scan could add nothing (every
// object it would return is in Sources already) and it is skipped; otherwise it runs as the game's, adding in the
// scan's order. One made but not loaded yet stays in the list until a scan has added it, so the scan runs again at
// each call until then (as the game would find it). Sources is the same list in the same order either way.
// ASMPATCH_BLASPHEMOUS_I2_SOURCES (read once): "off" = the game's scan every time; "check" = the scan every time as
// well, and where the patch would have skipped it, each source the scan adds is logged
// "[blasphemous-i2-sources] check DIFFERENT ..." (the patched game would not have added it then); every scan logs
// "[blasphemous-i2-sources] check: scan <n>: ... added, the patch would have <skipped|scanned> it".
// Checked at patch time: RegisterSceneSources and LanguageSource's constructor as the game has them (IL hash), the
// writers of Sources (AddSource's Add, RemoveSource's Remove; no other List<LanguageSource> mutator anywhere),
// RemoveSource's only caller UnregisterDeletededSources, Sources never named for reflection.
using Mono.Cecil;
using Mono.Cecil.Cil;
using Mono.Cecil.Rocks;

namespace AsmPatch;

sealed class BlasphemousI2Sources : Patch
{
    public override string Name => "blasphemous-i2-sources";
    public override Kind Kind => Kind.Optimize;
    public override bool OptIn => true;
    public override string Summary =>
        "Blasphemous's I2 Localization skips its scene-load scan of every loaded object for language sources when no LanguageSource was made since the last scan (Sources the same list in the same order; opt-in)";

    public const string Variable = "ASMPATCH_BLASPHEMOUS_I2_SOURCES", HelperType = "<asmpatch>BlasphemousI2Sources";
    const string ManagerType = "I2.Loc.LocalizationManager", SourceType = "I2.Loc.LanguageSource";
    // RegisterSceneSources, UnregisterDeletededSources and LanguageSource's constructor as the game (Blasphemous 4.0)
    // has them, as this patch finds them (BlasphemousCatalogs.Shape; the tests set their stand-in's)
    internal static string ScanHash = "af530d3bf38347c4", UnregisterHash = "f184c8fd940c5eb4", CtorHash = "0457b2573d6ffebf";

    public override Result Run(Game game, bool apply)
    {
        var asm = game.Load("Assembly-CSharp-firstpass");
        var m = asm?.MainModule;
        var mgr = m?.GetType(ManagerType);
        var src = m?.GetType(SourceType);
        if (mgr == null || src == null) return Result.NotApplicable($"no I2 Localization ({ManagerType}, {SourceType} in Assembly-CSharp-firstpass)");
        if (game.Load("Assembly-CSharp")?.MainModule.GetType("Framework.Managers.MapManager") == null)
            return Result.NotApplicable("not Blasphemous (no Framework.Managers.MapManager)");
        var file = game.FileOf(asm);
        if (m.Types.Any(t => t.Name == HelperType))
            return new Result { Outcome = Outcome.AlreadyApplied, Details = { $"{file}: the scene sources scan only when a LanguageSource was made" } };
        var why = Check(game, mgr, src, out var scan, out var ctor, out var addCall, out var sources);
        if (why != null) return Result.NotApplicable($"{file}: {why}");
        var r = new Result { Outcome = Outcome.Applied };
        r.Details.Add($"{file}: LocalizationManager.RegisterSceneSources scans (Resources.FindObjectsOfTypeAll) only when a LanguageSource was made that is not in Sources");
        if (!apply) return r;

        var h = Helper(game, m, mgr, src, scan, addCall, sources);
        if (h == null) return Result.Failed($"{file}: a class library method the helper calls is missing");
        // the constructor: Made(this) before its ret
        var cb = ctor.Body; cb.SimplifyMacros();
        var cil = cb.GetILProcessor();
        var cret = cb.Instructions.Single(x => x.OpCode.Code == Code.Ret);
        cil.InsertBefore(cret, cil.Create(OpCodes.Ldarg_0));
        cil.InsertBefore(cret, cil.Create(OpCodes.Call, h.Value.made));
        // (a branch to the ret would skip it: Check found none)
        cb.OptimizeMacros();
        // RegisterSceneSources: if (Skip()) return; ... AddSource(x) -> Added(x); ret -> Done(); ret
        var sb = scan.Body; sb.SimplifyMacros();
        var il = sb.GetILProcessor();
        var first = sb.Instructions[0];
        il.InsertBefore(first, il.Create(OpCodes.Call, h.Value.skip));
        il.InsertBefore(first, il.Create(OpCodes.Brfalse, first));
        il.InsertBefore(first, il.Create(OpCodes.Ret));
        addCall.Operand = h.Value.added;
        var ret = sb.Instructions.Last(x => x.OpCode.Code == Code.Ret);
        ret.OpCode = OpCodes.Call; ret.Operand = h.Value.done;
        il.InsertAfter(ret, il.Create(OpCodes.Ret));
        sb.OptimizeMacros();
        game.MarkChanged(asm);
        return r;
    }

    static string Check(Game game, TypeDefinition mgr, TypeDefinition src, out MethodDefinition scan, out MethodDefinition ctor,
                        out Instruction addCall, out FieldDefinition sources)
    {
        scan = null; ctor = null; addCall = null;
        sources = mgr.Fields.FirstOrDefault(f => f.Name == "Sources" && f.IsStatic && f.IsPublic &&
                                                 f.FieldType.FullName == $"System.Collections.Generic.List`1<{SourceType}>");
        if (sources == null) return "no public static List<LanguageSource> LocalizationManager.Sources";
        if (src.BaseType?.FullName != "UnityEngine.MonoBehaviour") return "LanguageSource is not a MonoBehaviour";
        scan = mgr.Methods.FirstOrDefault(x => x.Name == "RegisterSceneSources" && x.IsStatic && x.HasBody && x.Parameters.Count == 0);
        var unreg = mgr.Methods.FirstOrDefault(x => x.Name == "UnregisterDeletededSources" && x.IsStatic && x.HasBody && x.Parameters.Count == 0);
        var addSrc = mgr.Methods.FirstOrDefault(x => x.Name == "AddSource" && x.IsStatic && x.HasBody && x.Parameters.Count == 1);
        var remSrc = mgr.Methods.FirstOrDefault(x => x.Name == "RemoveSource" && x.IsStatic && x.HasBody && x.Parameters.Count == 1);
        var ctors = src.Methods.Where(x => x.IsConstructor && !x.IsStatic).ToList();
        if (scan == null || unreg == null || addSrc == null || remSrc == null || ctors.Count != 1 || ctors[0].Parameters.Count != 0)
            return "no LocalizationManager.RegisterSceneSources/UnregisterDeletededSources/AddSource/RemoveSource or not one LanguageSource()";
        ctor = ctors[0];
        string hs = BlasphemousCatalogs.Shape(scan), hu = BlasphemousCatalogs.Shape(unreg), hc = BlasphemousCatalogs.Shape(ctor);
        if (hs != ScanHash || hu != UnregisterHash || hc != CtorHash)
            return $"RegisterSceneSources/UnregisterDeletededSources/LanguageSource() are not as this patch knows them (IL {hs}/{hu}/{hc}, known {ScanHash}/{UnregisterHash}/{CtorHash})";
        var adds = scan.Body.Instructions.Where(i => i.Operand is MethodReference r && r.Name == "AddSource" && r.DeclaringType.FullName == ManagerType).ToList();
        if (adds.Count != 1 || scan.Body.Instructions.Count(i => i.OpCode.Code == Code.Ret) != 1) return "RegisterSceneSources has not one AddSource call and one ret";
        addCall = adds[0];
        var cret = ctor.Body.Instructions.Single(i => i.OpCode.Code == Code.Ret);
        if (ctor.Body.Instructions.Any(i => i.Operand == cret || (i.Operand is Instruction[] ts && ts.Contains(cret))) || ctor.Body.HasExceptionHandlers)
            return "LanguageSource() branches to its ret";

        // who changes Sources: AddSource (Add) and RemoveSource (Remove) only; RemoveSource only from UnregisterDeletededSources;
        // no other mutator of a List<LanguageSource> anywhere (the list cannot be changed through another name either)
        string list = sources.FieldType.FullName;
        var bad = new List<string>();
        foreach (var t in game.GameAssemblies.SelectMany(a => Cecil.AllTypes(a.MainModule)))
            foreach (var md in t.Methods.Where(x => x.HasBody))
                foreach (var ins in md.Body.Instructions)
                {
                    string at = $"{t.Name}.{md.Name}";
                    if (ins.Operand is FieldReference f && Cecil.SameField(f, sources) && ins.OpCode.Code is Code.Stsfld or Code.Ldsflda && !(md.IsConstructor && md.IsStatic && md.DeclaringType == mgr))
                        bad.Add(at + " writes Sources");
                    else if (ins.Operand is MethodReference r && r.DeclaringType.FullName == list && !(r.Name is "get_Count" or "get_Item" or "Contains" or "IndexOf" or "GetEnumerator" or ".ctor" or "ToArray"))
                    {
                        if (!(r.Name == "Add" && md == addSrc) && !(r.Name == "Remove" && md == remSrc)) bad.Add($"{at} {r.Name}");
                    }
                    else if (ins.Operand is MethodReference rs && rs.Name == "RemoveSource" && rs.DeclaringType.FullName == ManagerType && md != unreg)
                        bad.Add(at + " RemoveSource");
                    else if (ins.OpCode.Code == Code.Ldstr && (string)ins.Operand == "Sources" &&
                             Enumerable.Range(1, 3).Select(k => BlasphemousCatalogs.Nth(ins, k)).Any(x => x?.Operand is MethodReference mr && mr.DeclaringType.FullName is "System.Type" or "System.Reflection.TypeInfo" or "System.Reflection.IReflect" &&
                                                                                                 mr.Name is "GetField" or "GetProperty" or "GetMember" or "InvokeMember"))
                        bad.Add(at + " names Sources");
                }
        if (bad.Count > 0) return "Sources is changed where this patch does not know it: " + string.Join(", ", bad.Distinct());
        // the list's methods this patch calls, as the game calls them
        if (!scan.Body.Instructions.Any(i => i.Operand is MethodReference r && r.DeclaringType.FullName == list && r.Name == "Contains"))
            return "RegisterSceneSources does not ask Sources.Contains";
        return null;
    }

    static (MethodDefinition made, MethodDefinition skip, MethodDefinition added, MethodDefinition done)? Helper(
        Game game, ModuleDefinition m, TypeDefinition mgr, TypeDefinition src, MethodDefinition scan, Instruction addCall, FieldDefinition sources)
    {
        var ts = m.TypeSystem;
        const string S = "System.String", O = "System.Object";
        var env = BlasphemousCatalogs.Lib(game, m, "System.Environment", "GetEnvironmentVariable", S);
        var strEq = BlasphemousCatalogs.Lib(game, m, S, "op_Equality", S, S);
        var concatO = BlasphemousCatalogs.Lib(game, m, S, "Concat", O + "[]");
        var enter = BlasphemousCatalogs.Lib(game, m, "System.Threading.Monitor", "Enter", O);
        var exit = BlasphemousCatalogs.Lib(game, m, "System.Threading.Monitor", "Exit", O);
        var log = BlasphemousCatalogs.Lib(game, m, "UnityEngine.Debug", "Log", O);
        var uName = BlasphemousCatalogs.Lib(game, m, "UnityEngine.Object", "get_name");
        if (new[] { env, strEq, concatO, enter, exit, log, uName }.Any(x => x == null)) return null;
        var listT = sources.FieldType;   // List<LanguageSource>, as the game names it
        var contains = (MethodReference)scan.Body.Instructions.First(i => i.Operand is MethodReference r && r.DeclaringType.FullName == listT.FullName && r.Name == "Contains").Operand;
        MethodReference ListM(string name, TypeReference ret, params TypeReference[] ps)
        {
            var r = new MethodReference(name, ret, listT) { HasThis = true };
            foreach (var p in ps) r.Parameters.Add(new ParameterDefinition(p));
            return m.ImportReference(r);
        }
        var listGen = ((GenericInstanceType)listT).ElementType;
        var t0 = listGen.GenericParameters.Count > 0 ? listGen.GenericParameters[0] : new GenericParameter("T", listGen);
        var listNew = ListM(".ctor", ts.Void);
        var listAdd = ListM("Add", ts.Void, t0);
        var listCount = ListM("get_Count", ts.Int32);
        var listItem = ListM("get_Item", t0, ts.Int32);
        var listRemoveAt = ListM("RemoveAt", ts.Void, ts.Int32);
        var addSource = (MethodReference)addCall.Operand;

        var t = new TypeDefinition("", HelperType, TypeAttributes.NotPublic | TypeAttributes.Abstract | TypeAttributes.Sealed |
                                                   TypeAttributes.BeforeFieldInit | TypeAttributes.Class, ts.Object);
        m.Types.Add(t);
        const FieldAttributes SF = FieldAttributes.Private | FieldAttributes.Static;
        var madeF = new FieldDefinition("made", SF, listT);
        var mode = new FieldDefinition("mode", SF, ts.Int32);       // 0 unread, 1 on, 2 check, 3 off
        var would = new FieldDefinition("would", SF, ts.Int32);     // check: the patch would skip this scan
        var calls = new FieldDefinition("calls", SF, ts.Int32);
        var skips = new FieldDefinition("skips", SF, ts.Int32);
        var addedN = new FieldDefinition("addedN", SF, ts.Int32);
        foreach (var f in new[] { madeF, mode, would, calls, skips, addedN }) t.Fields.Add(f);
        const MethodAttributes Static = MethodAttributes.Assembly | MethodAttributes.Static | MethodAttributes.HideBySig;
        MethodDefinition Method(string name, TypeReference ret, params (string n, TypeReference t)[] ps)
        {
            var md = new MethodDefinition(name, Static, ret);
            foreach (var (n, pt) in ps) md.Parameters.Add(new ParameterDefinition(n, ParameterAttributes.None, pt));
            md.Body.InitLocals = true;
            t.Methods.Add(md);
            return md;
        }
        VariableDefinition Local(MethodDefinition md, TypeReference vt) { var v = new VariableDefinition(vt); md.Body.Variables.Add(v); return v; }
        void Log(ILProcessor il, params Action[] parts)
        {
            il.Emit(OpCodes.Ldc_I4, parts.Length); il.Emit(OpCodes.Newarr, ts.Object);
            for (int k = 0; k < parts.Length; k++) { il.Emit(OpCodes.Dup); il.Emit(OpCodes.Ldc_I4, k); parts[k](); il.Emit(OpCodes.Stelem_Ref); }
            il.Emit(OpCodes.Call, concatO); il.Emit(OpCodes.Call, log);
        }
        void Str(ILProcessor il, string s) => il.Emit(OpCodes.Ldstr, s);
        void Int(ILProcessor il, FieldDefinition f) { il.Emit(OpCodes.Ldsfld, f); il.Emit(OpCodes.Box, ts.Int32); }

        // static .cctor: made = new List<LanguageSource>()
        var cctor = new MethodDefinition(".cctor", MethodAttributes.Private | MethodAttributes.Static | MethodAttributes.HideBySig |
                                                   MethodAttributes.SpecialName | MethodAttributes.RTSpecialName, ts.Void);
        t.Methods.Add(cctor);
        { var il = cctor.Body.GetILProcessor(); il.Emit(OpCodes.Newobj, listNew); il.Emit(OpCodes.Stsfld, madeF); il.Emit(OpCodes.Ret); }

        // static int Mode(): mode, read once from the variable (off 3, check 2, else 1)
        var modeM = Method("Mode", ts.Int32);
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
            il.Emit(OpCodes.Ldc_I4_2); il.Emit(OpCodes.Stsfld, mode); il.Emit(OpCodes.Br, have);
            il.Append(notCheck); il.Emit(OpCodes.Stsfld, mode);
            il.Append(have); il.Emit(OpCodes.Ret);
        }

        // static void Made(LanguageSource s): lock (made) made.Add(s)   (not when off)
        var madeM = Method("Made", ts.Void, ("s", src));
        {
            var l = Local(madeM, listT);
            var il = madeM.Body.GetILProcessor();
            var end = il.Create(OpCodes.Ret);
            il.Emit(OpCodes.Call, modeM); il.Emit(OpCodes.Ldc_I4_3); il.Emit(OpCodes.Beq, end);
            il.Emit(OpCodes.Ldsfld, madeF); il.Emit(OpCodes.Dup); il.Emit(OpCodes.Stloc, l); il.Emit(OpCodes.Call, enter);
            var tryStart = il.Create(OpCodes.Ldsfld, madeF);
            il.Append(tryStart); il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Callvirt, listAdd); il.Emit(OpCodes.Leave, end);
            var fin = il.Create(OpCodes.Ldloc, l);
            il.Append(fin); il.Emit(OpCodes.Call, exit); il.Emit(OpCodes.Endfinally);
            il.Append(end);
            madeM.Body.ExceptionHandlers.Add(new ExceptionHandler(ExceptionHandlerType.Finally) { TryStart = tryStart, TryEnd = fin, HandlerStart = fin, HandlerEnd = end });
        }

        // static bool Skip(): drops from made those in Sources; true (skip the scan) when none is left and not check/off
        //   m = Mode(); if (m == 3) return false; all = 1; lock (made) for (i = made.Count - 1; i >= 0; i--) if (Sources.Contains(made[i])) made.RemoveAt(i); else all = 0;
        //   if (calls == 0) log; calls++; if (m == 2) { would = all; return false; } skips += all; return all;
        var skipM = Method("Skip", ts.Boolean);
        {
            var mv = Local(skipM, ts.Int32); var all = Local(skipM, ts.Int32); var l = Local(skipM, listT); var i = Local(skipM, ts.Int32);
            var il = skipM.Body.GetILProcessor();
            var go = il.Create(OpCodes.Ldc_I4_1);
            il.Emit(OpCodes.Call, modeM); il.Emit(OpCodes.Dup); il.Emit(OpCodes.Stloc, mv); il.Emit(OpCodes.Ldc_I4_3); il.Emit(OpCodes.Bne_Un, go);
            il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Ret);
            il.Append(go); il.Emit(OpCodes.Stloc, all);
            il.Emit(OpCodes.Ldsfld, madeF); il.Emit(OpCodes.Dup); il.Emit(OpCodes.Stloc, l); il.Emit(OpCodes.Call, enter);
            var tryStart = il.Create(OpCodes.Ldsfld, madeF);
            var after = il.Create(OpCodes.Ldsfld, calls);
            var cond = il.Create(OpCodes.Ldloc, i); var body = il.Create(OpCodes.Ldsfld, sources); var keep = il.Create(OpCodes.Ldc_I4_0);
            var next = il.Create(OpCodes.Ldloc, i);
            il.Append(tryStart); il.Emit(OpCodes.Callvirt, listCount); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Sub); il.Emit(OpCodes.Stloc, i);
            il.Emit(OpCodes.Br, cond);
            il.Append(body); il.Emit(OpCodes.Ldsfld, madeF); il.Emit(OpCodes.Ldloc, i); il.Emit(OpCodes.Callvirt, listItem);
            il.Emit(OpCodes.Callvirt, contains); il.Emit(OpCodes.Brfalse, keep);
            il.Emit(OpCodes.Ldsfld, madeF); il.Emit(OpCodes.Ldloc, i); il.Emit(OpCodes.Callvirt, listRemoveAt); il.Emit(OpCodes.Br, next);
            il.Append(keep); il.Emit(OpCodes.Stloc, all);
            il.Append(next); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Sub); il.Emit(OpCodes.Stloc, i);
            il.Append(cond); il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Bge, body);
            il.Emit(OpCodes.Leave, after);
            var fin = il.Create(OpCodes.Ldloc, l);
            il.Append(fin); il.Emit(OpCodes.Call, exit); il.Emit(OpCodes.Endfinally);
            skipM.Body.ExceptionHandlers.Add(new ExceptionHandler(ExceptionHandlerType.Finally) { TryStart = tryStart, TryEnd = fin, HandlerStart = fin, HandlerEnd = after });
            // the first call: say how it runs
            var counted = il.Create(OpCodes.Ldsfld, calls);
            il.Append(after); il.Emit(OpCodes.Brtrue, counted);
            var notChk = il.Create(OpCodes.Nop);
            il.Emit(OpCodes.Ldloc, mv); il.Emit(OpCodes.Ldc_I4_2); il.Emit(OpCodes.Bne_Un, notChk);
            Log(il, () => Str(il, $"[blasphemous-i2-sources] {Variable}=check: the scene sources scan every time, compared with the patch's choice"));
            il.Emit(OpCodes.Br, counted);
            il.Append(notChk);
            Log(il, () => Str(il, "[blasphemous-i2-sources] the scene sources scan only when a LanguageSource was made that is not in Sources"));
            il.Append(counted); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Add); il.Emit(OpCodes.Stsfld, calls);
            var notCheck = il.Create(OpCodes.Ldsfld, skips);
            il.Emit(OpCodes.Ldloc, mv); il.Emit(OpCodes.Ldc_I4_2); il.Emit(OpCodes.Bne_Un, notCheck);
            il.Emit(OpCodes.Ldloc, all); il.Emit(OpCodes.Stsfld, would); il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Ret);
            il.Append(notCheck); il.Emit(OpCodes.Ldloc, all); il.Emit(OpCodes.Add); il.Emit(OpCodes.Stsfld, skips);
            il.Emit(OpCodes.Ldloc, all); il.Emit(OpCodes.Ret);
        }

        // static void Added(LanguageSource s): check: logged DIFFERENT where the patch would have skipped; AddSource(s)
        var addedM = Method("Added", ts.Void, ("s", src));
        {
            var il = addedM.Body.GetILProcessor();
            var add = il.Create(OpCodes.Ldsfld, addedN);
            il.Emit(OpCodes.Ldsfld, would); il.Emit(OpCodes.Brfalse, add);
            Log(il, () => Str(il, "[blasphemous-i2-sources] check DIFFERENT: scan "), () => Int(il, calls),
                () => Str(il, " adds "), () => { il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Callvirt, uName); },
                () => Str(il, ", where the patch would not have scanned"));
            il.Append(add); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Add); il.Emit(OpCodes.Stsfld, addedN);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Call, addSource); il.Emit(OpCodes.Ret);
        }

        // static void Done(): check: one line a scan; the counts reset
        var doneM = Method("Done", ts.Void);
        {
            var w = Local(doneM, ts.String);
            var il = doneM.Body.GetILProcessor();
            var reset = il.Create(OpCodes.Ldc_I4_0);
            il.Emit(OpCodes.Ldsfld, mode); il.Emit(OpCodes.Ldc_I4_2); il.Emit(OpCodes.Bne_Un, reset);
            var scanned = il.Create(OpCodes.Ldstr, "scanned"); var said = il.Create(OpCodes.Stloc, w);
            il.Emit(OpCodes.Ldsfld, would); il.Emit(OpCodes.Brfalse, scanned);
            il.Emit(OpCodes.Ldstr, "skipped"); il.Emit(OpCodes.Br, said);
            il.Append(scanned); il.Append(said);
            Log(il, () => Str(il, "[blasphemous-i2-sources] check: scan "), () => Int(il, calls), () => Str(il, ": "), () => Int(il, addedN),
                () => Str(il, " added, the patch would have "), () => il.Emit(OpCodes.Ldloc, w), () => Str(il, " it"));
            il.Append(reset); il.Emit(OpCodes.Stsfld, would); il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Stsfld, addedN); il.Emit(OpCodes.Ret);
        }
        return (madeM, skipM, addedM, doneM);
    }
}
