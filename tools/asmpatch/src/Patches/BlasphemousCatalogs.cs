// blasphemous-catalogs (kind Optimize, opt-in: it needs the setup's list, pmu convert's step fmodcatalogs). At startup
// Blasphemous's FMODAudioManager.Initialize loads every enemy's sound catalog,
//   EnemiesAudioCatalogs = Resources.LoadAll<FMODAudioCatalog>("FMODCatalogs/Enemies");
// and each catalog's Owner field references an enemy or boss component, so the load brings every enemy and boss
// with all their sprite sheets into memory (most of resources.assets: more than a 1 GB handheld has). The only
// reader of that array and of Owner is AudioLoader.AddCatalogItems (each level's AudioLoader.Awake): for each enemy
// type of the level, the first catalog in the array whose Owner has that type and is not in the list yet, with
// its AssociatedCatalog. With the patch the startup load returns an empty array, and AddCatalogItems walks,
// instead of the whole array, the catalogs of the setup's list (fmodcatalogs.txt in the data folder: the array's
// paths in its order, each with its owner's class) whose owner class is one of the level's enemy types, each
// loaded then by its path (Resources.Load): the same catalogs in the same order as the array has them, so the
// game's own loop picks the same ones (a catalog whose owner is another type never matches in that loop). Only
// when the catalogs and those enemies are loaded changes.
// ASMPATCH_BLASPHEMOUS_CATALOGS (read once): "off" = the game as it was; "check" = the startup load is done as
// well and compared with the list entry by entry (LoadAll's order, Resources.Load's object, the owner's type),
// "[blasphemous-catalogs] check ..." in the log, then the list is used; each level logs its types and catalogs.
// Without the list (or an unreadable one) the game loads them all at startup, as it does itself.
using System.Security.Cryptography;
using System.Text;
using Mono.Cecil;
using Mono.Cecil.Cil;
using Mono.Cecil.Rocks;

namespace AsmPatch;

sealed class BlasphemousCatalogs : Patch
{
    public override string Name => "blasphemous-catalogs";
    public override Kind Kind => Kind.Optimize;
    public override bool OptIn => true;
    public override string Summary =>
        "Blasphemous loads each level's enemy sound catalogs (and so their enemies) when the level needs them, from the setup's list, instead of every catalog with every enemy at startup (the same catalogs in the same order; opt-in: pmu convert's fmodcatalogs step writes the list)";

    public const string Variable = "ASMPATCH_BLASPHEMOUS_CATALOGS", HelperType = "<asmpatch>BlasphemousCatalogs";
    public const string ListFile = "fmodcatalogs.txt", Header = "# pmu fmodcatalogs 1", Path = "FMODCatalogs/Enemies";
    const string CatalogType = "Framework.Audio.FMODAudioCatalog", ManagerType = "Framework.Managers.FMODAudioManager",
                 LoaderType = "Framework.Audio.AudioLoader", EntityType = "Gameplay.GameControllers.Entities.Entity";
    // AudioLoader.AddCatalogItems and GetLevelEnemyTypes as the game (Blasphemous 4.0, Unity 2017.4.40f1) has them:
    // the hash of their IL (Shape below); another build's are not assumed to do the same
    const string AddHash = "1568c942a28fd079", TypesHash = "0a8a7fddd175bbf5";

    public override Result Run(Game game, bool apply)
    {
        var asm = game.Load("Assembly-CSharp");
        var m = asm?.MainModule;
        var cat = m?.GetType(CatalogType);
        var mgr = m?.GetType(ManagerType);
        var loader = m?.GetType(LoaderType);
        if (cat == null || mgr == null || loader == null)
            return Result.NotApplicable($"no Blasphemous ({CatalogType}, {ManagerType}, {LoaderType} in Assembly-CSharp)");
        var file = game.FileOf(asm);
        if (m.Types.Any(t => t.Name == HelperType))
            return new Result { Outcome = Outcome.AlreadyApplied, Details = { $"{file}: enemy catalogs loaded per level from {ListFile}" } };
        string why = Check(game, m, cat, mgr, loader, out var init, out var loadAll, out var add, out var getter, out var types);
        if (why != null) return Result.NotApplicable($"{file}: {why}");
        var r = new Result { Outcome = Outcome.Applied };
        r.Details.Add($"{file}: FMODAudioManager.Initialize loads no enemy catalogs; AudioLoader.AddCatalogItems loads those of the level's enemy types from {ListFile} (none there: the game's own load)");
        if (!apply) return r;

        var h = Helper(game, m, cat, loadAll);
        if (h == null) return Result.Failed($"{file}: a class library method the helper calls is missing");
        // Initialize: Resources.LoadAll<FMODAudioCatalog>(path) -> Startup(path)
        init.Operand = h.Value.startup;
        // AddCatalogItems: Core.Audio.EnemiesAudioCatalogs -> Candidates(Core.Audio.EnemiesAudioCatalogs, GetLevelEnemyTypes())
        var addBody = add.Body;
        addBody.SimplifyMacros();
        var il = addBody.GetILProcessor();
        var after = getter;
        foreach (var ins in new[] { il.Create(OpCodes.Ldarg_0), il.Create(OpCodes.Call, types), il.Create(OpCodes.Call, h.Value.candidates) })
        {
            il.InsertAfter(after, ins);
            after = ins;
        }
        addBody.OptimizeMacros();
        game.MarkChanged(asm);
        return r;
    }

    /// <summary>What the patch relies on, checked in the game's assemblies (null: all of it holds).</summary>
    static string Check(Game game, ModuleDefinition m, TypeDefinition cat, TypeDefinition mgr, TypeDefinition loader,
                        out Instruction init, out MethodReference loadAll, out MethodDefinition add, out Instruction getter, out MethodDefinition types)
    {
        init = null; loadAll = null; add = null; getter = null; types = null;
        // FMODAudioCatalog: a MonoBehaviour whose serialized fields start AssociatedCatalog, HasAssociatedCatalog, Owner
        // (the order pmu's step reads them in), with no class deriving from it (LoadAll<T> returns those too)
        if (cat.BaseType?.FullName != "UnityEngine.MonoBehaviour") return "FMODAudioCatalog is not a MonoBehaviour";
        var ser = cat.Fields.Where(f => !f.IsStatic && !f.IsInitOnly && !f.IsLiteral && !f.IsNotSerialized &&
                                        (f.IsPublic || f.CustomAttributes.Any(a => a.AttributeType.FullName == "UnityEngine.SerializeField"))).ToList();
        if (ser.Count < 3 || ser[0].Name != "AssociatedCatalog" || ser[0].FieldType.FullName != CatalogType ||
            ser[1].Name != "HasAssociatedCatalog" || ser[1].FieldType.MetadataType != MetadataType.Boolean ||
            ser[2].Name != "Owner" || ser[2].FieldType.FullName != EntityType)
            return "FMODAudioCatalog's serialized fields do not start AssociatedCatalog, HasAssociatedCatalog, Owner";
        var owner = ser[2];
        var all = game.Assemblies.SelectMany(a => Cecil.AllTypes(a.MainModule)).ToList();
        bool Derives(TypeDefinition t, string baseName) => Cecil.Hierarchy(t).Skip(1).Any(b => b.FullName == baseName);
        if (all.Any(t => Derives(t, CatalogType))) return "a class derives from FMODAudioCatalog";
        // the owners' classes, compared by full name: one class per name, none nested (pmu writes namespace.class)
        var entities = all.Where(t => t.FullName == EntityType || Derives(t, EntityType)).ToList();
        if (entities.Any(t => t.IsNested)) return "a nested class derives from Entity";
        var names = all.GroupBy(t => t.FullName).Where(g => g.Count() > 1).Select(g => g.Key).ToHashSet();
        if (entities.Any(t => names.Contains(t.FullName))) return "two classes of one name derive from Entity";

        // who reads and writes the array and Owner: Initialize writes it once, AddCatalogItems reads it once, Owner is read there only
        var prop = mgr.Properties.FirstOrDefault(p => p.Name == "EnemiesAudioCatalogs");
        if (prop?.GetMethod == null || prop.SetMethod == null || prop.PropertyType.FullName != CatalogType + "[]")
            return "no FMODAudioManager.EnemiesAudioCatalogs { get; set; }";
        var backing = mgr.Fields.Where(f => f.FieldType.FullName == CatalogType + "[]" && f.Name.Contains("EnemiesAudioCatalogs")).ToList();
        var uses = new List<(MethodDefinition at, Instruction ins, string what)>();
        foreach (var t in game.GameAssemblies.SelectMany(a => Cecil.AllTypes(a.MainModule)))
            foreach (var md in t.Methods.Where(x => x.HasBody))
                foreach (var ins in md.Body.Instructions)
                {
                    if (ins.Operand is MethodReference r && r.DeclaringType.FullName == ManagerType && (r.Name == "get_EnemiesAudioCatalogs" || r.Name == "set_EnemiesAudioCatalogs"))
                        uses.Add((md, ins, r.Name));
                    else if (ins.Operand is FieldReference f && (backing.Any(b => Cecil.SameField(f, b)) && md != prop.GetMethod && md != prop.SetMethod))
                        uses.Add((md, ins, "field"));
                    else if (ins.Operand is FieldReference o && Cecil.SameField(o, owner))
                        uses.Add((md, ins, "Owner"));
                    else if (ins.OpCode.Code == Code.Ldstr && (string)ins.Operand is "EnemiesAudioCatalogs" or "Owner" && md.DeclaringType.Module == m &&
                             md.DeclaringType.Namespace.StartsWith("Framework.Audio"))
                        uses.Add((md, ins, "name"));
                }
        add = loader.Methods.FirstOrDefault(x => x.Name == "AddCatalogItems" && x.HasBody && x.Parameters.Count == 0 && !x.IsStatic);
        types = loader.Methods.FirstOrDefault(x => x.Name == "GetLevelEnemyTypes" && x.HasBody && x.Parameters.Count == 0 && !x.IsStatic &&
                                                    x.ReturnType.FullName == "System.Collections.Generic.IEnumerable`1<System.Type>");
        var initMd = mgr.Methods.FirstOrDefault(x => x.Name == "Initialize" && x.HasBody && x.Parameters.Count == 0 && !x.IsStatic);
        if (add == null || types == null || initMd == null) return "no AudioLoader.AddCatalogItems/GetLevelEnemyTypes or FMODAudioManager.Initialize";
        var a2 = add; var i2 = initMd;
        var gets = uses.Where(u => u.what == "get_EnemiesAudioCatalogs").ToList();
        var sets = uses.Where(u => u.what == "set_EnemiesAudioCatalogs").ToList();
        if (gets.Count != 1 || gets[0].at != a2 || sets.Count != 1 || sets[0].at != i2 || uses.Any(u => u.what is "field" or "name") ||
            uses.Any(u => u.what == "Owner" && u.at != a2))
            return "EnemiesAudioCatalogs or FMODAudioCatalog.Owner is used where this patch does not know it: " +
                   string.Join(", ", uses.Select(u => $"{u.at.DeclaringType.Name}.{u.at.Name} {u.what}").Distinct());
        // the load: ldstr "FMODCatalogs/Enemies"; call Resources.LoadAll<FMODAudioCatalog>(string); call set_EnemiesAudioCatalogs (on this)
        var set = sets[0].ins;
        var call = set.Previous;
        if (call?.Operand is not GenericInstanceMethod gm || gm.Name != "LoadAll" || gm.DeclaringType.FullName != "UnityEngine.Resources" ||
            gm.GenericArguments.Count != 1 || gm.GenericArguments[0].FullName != CatalogType || gm.Parameters.Count != 1 ||
            call.Previous?.OpCode.Code != Code.Ldstr || (string)call.Previous.Operand != Path)
            return $"FMODAudioManager.Initialize does not set EnemiesAudioCatalogs = Resources.LoadAll<FMODAudioCatalog>(\"{Path}\")";
        if (initMd.Body.Instructions.Any(i => i.Operand == call)) return "a branch lands on the LoadAll call";
        init = call; loadAll = gm;
        getter = gets[0].ins;
        // AddCatalogItems and GetLevelEnemyTypes as this patch knows them
        string ha = Shape(add), ht = Shape(types);
        if (ha != AddHash || ht != TypesHash)
            return $"AudioLoader.AddCatalogItems/GetLevelEnemyTypes are not as this patch knows them (IL {ha}/{ht}, known {AddHash}/{TypesHash})";
        return null;
    }

    /// <summary>A method's IL as text (op codes, operands by name, branch targets by index), hashed.</summary>
    public static string Shape(MethodDefinition md)
    {
        var ins = md.Body.Instructions;
        var idx = ins.Select((x, i) => (x, i)).ToDictionary(p => p.x, p => p.i);
        var sb = new StringBuilder();
        foreach (var i in ins)
        {
            sb.Append(i.OpCode.Name).Append(' ');
            sb.Append(i.Operand switch
            {
                Instruction t => "@" + idx[t],
                Instruction[] ts => string.Join(",", ts.Select(t => "@" + idx[t])),
                MemberReference r => r.FullName,
                VariableDefinition v => "v" + v.Index,
                ParameterDefinition p => "a" + p.Index,
                null => "",
                var o => o.ToString(),
            }).Append('\n');
        }
        foreach (var v in md.Body.Variables) sb.Append("local ").Append(v.VariableType.FullName).Append('\n');
        foreach (var e in md.Body.ExceptionHandlers)
            sb.Append($"eh {e.HandlerType} @{idx[e.TryStart]} @{(e.TryEnd == null ? -1 : idx[e.TryEnd])} @{idx[e.HandlerStart]} {e.CatchType?.FullName}\n");
        return Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(sb.ToString())))[..16].ToLowerInvariant();
    }

    /// <summary>A class library or engine method, by declaring type, name and parameter types, imported into m.</summary>
    static MethodReference Lib(Game game, ModuleDefinition m, string type, string name, params string[] ps)
    {
        foreach (var a in game.Assemblies)
        {
            var t = a.MainModule.GetType(type);
            var md = t?.Methods.FirstOrDefault(x => x.Name == name && x.Parameters.Count == ps.Length &&
                                                    x.Parameters.Select(p => p.ParameterType.FullName).SequenceEqual(ps) && !x.HasGenericParameters);
            if (md != null) return m.ImportReference(md);
        }
        return null;
    }

    /// <summary>The helper class: static FMODAudioCatalog[] Startup(string path) (the startup load) and
    /// Candidates(FMODAudioCatalog[] all, IEnumerable&lt;Type&gt; types) (the catalogs AddCatalogItems walks).</summary>
    static (MethodDefinition startup, MethodDefinition candidates)? Helper(Game game, ModuleDefinition m, TypeDefinition cat, MethodReference loadAll)
    {
        var ts = m.TypeSystem;
        const string S = "System.String", I = "System.Int32", O = "System.Object";
        var env = Lib(game, m, "System.Environment", "GetEnvironmentVariable", S);
        var strEq = Lib(game, m, S, "op_Equality", S, S);
        var endsWith = Lib(game, m, S, "EndsWith", S, "System.StringComparison");
        var indexOf = Lib(game, m, S, "IndexOf", S, "System.StringComparison");
        var indexOfChar = Lib(game, m, S, "IndexOf", "System.Char");
        var sub1 = Lib(game, m, S, "Substring", I);
        var sub2 = Lib(game, m, S, "Substring", I, I);
        var concat2 = Lib(game, m, S, "Concat", S, S);
        var concat3 = Lib(game, m, S, "Concat", S, S, S);
        var concatO = Lib(game, m, S, "Concat", O + "[]");
        var dirName = Lib(game, m, "System.IO.Path", "GetDirectoryName", S);
        var exists = Lib(game, m, "System.IO.File", "Exists", S);
        var readLines = Lib(game, m, "System.IO.File", "ReadAllLines", S);
        var fromHandle = Lib(game, m, "System.Type", "GetTypeFromHandle", "System.RuntimeTypeHandle");
        var fullName = Lib(game, m, "System.Type", "get_FullName");
        var getType = Lib(game, m, O, "GetType");
        var getEnum = Lib(game, m, "System.Collections.IEnumerable", "GetEnumerator");
        var moveNext = Lib(game, m, "System.Collections.IEnumerator", "MoveNext");
        var current = Lib(game, m, "System.Collections.IEnumerator", "get_Current");
        var dispose = Lib(game, m, "System.IDisposable", "Dispose");
        var copy = Lib(game, m, "System.Array", "Copy", "System.Array", "System.Array", I);
        var dataPath = Lib(game, m, "UnityEngine.Application", "get_dataPath");
        var load = Lib(game, m, "UnityEngine.Resources", "Load", S, "System.Type");
        var log = Lib(game, m, "UnityEngine.Debug", "Log", O);
        var uEq = Lib(game, m, "UnityEngine.Object", "op_Equality", "UnityEngine.Object", "UnityEngine.Object");
        var uName = Lib(game, m, "UnityEngine.Object", "get_name");
        var replace = Lib(game, m, S, "Replace", S, S);
        var refs = new MethodReference[] { env, strEq, endsWith, indexOf, indexOfChar, sub1, sub2, concat2, concat3, concatO, dirName, exists, readLines,
                                           fromHandle, fullName, getType, getEnum, moveNext, current, dispose, copy, dataPath, load, log, uEq, uName, replace };
        if (refs.Any(x => x == null)) return null;
        var typeT = fromHandle.ReturnType;
        var exc = m.ImportReference(game.Load("mscorlib")?.MainModule.GetType("System.Exception"));
        var idisp = dispose.DeclaringType;
        var owner = m.ImportReference(cat.Fields.First(f => f.Name == "Owner"));
        var catArr = new ArrayType(cat);
        var strArr = new ArrayType(ts.String);

        var t = new TypeDefinition("", HelperType, TypeAttributes.NotPublic | TypeAttributes.Abstract | TypeAttributes.Sealed |
                                                   TypeAttributes.BeforeFieldInit | TypeAttributes.Class, ts.Object);
        m.Types.Add(t);
        const FieldAttributes SF = FieldAttributes.Private | FieldAttributes.Static;
        var paths = new FieldDefinition("paths", SF, strArr); var owners = new FieldDefinition("owners", SF, strArr);
        var state = new FieldDefinition("state", SF, ts.Int32); var check = new FieldDefinition("check", SF, ts.Boolean);
        foreach (var f in new[] { paths, owners, state, check }) t.Fields.Add(f);
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
        // Log(object[] parts): Debug.Log(string.Concat(parts))
        void Log(ILProcessor il, params Action[] parts)
        {
            il.Emit(OpCodes.Ldc_I4, parts.Length); il.Emit(OpCodes.Newarr, ts.Object);
            for (int k = 0; k < parts.Length; k++) { il.Emit(OpCodes.Dup); il.Emit(OpCodes.Ldc_I4, k); parts[k](); il.Emit(OpCodes.Stelem_Ref); }
            il.Emit(OpCodes.Call, concatO); il.Emit(OpCodes.Call, log);
        }

        // static bool Map(): the list read once (state 1: read, -1: none or off); check from the variable
        var map = Method("Map", ts.Boolean);
        {
            var v = Local(map, ts.String); var d = Local(map, ts.String); var f = Local(map, ts.String); var lines = Local(map, strArr);
            var n = Local(map, ts.Int32); var p = Local(map, strArr); var o = Local(map, strArr); var i = Local(map, ts.Int32);
            var l = Local(map, ts.String); var tab = Local(map, ts.Int32);
            var il = map.Body.GetILProcessor();
            var ret = il.Create(OpCodes.Ldsfld, state);
            var logIt = il.Create(OpCodes.Nop);
            //   if (state != 0) goto ret; state = -1; v = Environment.GetEnvironmentVariable(V); if (v == "off") { log; goto ret; } check = v == "check";
            il.Emit(OpCodes.Ldsfld, state); il.Emit(OpCodes.Brtrue, ret);
            il.Emit(OpCodes.Ldc_I4_M1); il.Emit(OpCodes.Stsfld, state);
            il.Emit(OpCodes.Ldstr, Variable); il.Emit(OpCodes.Call, env); il.Emit(OpCodes.Stloc, v);
            var notOff = il.Create(OpCodes.Ldloc, v);
            il.Emit(OpCodes.Ldloc, v); il.Emit(OpCodes.Ldstr, "off"); il.Emit(OpCodes.Call, strEq); il.Emit(OpCodes.Brfalse, notOff);
            Log(il, () => il.Emit(OpCodes.Ldstr, $"[blasphemous-catalogs] {Variable}=off: every enemy catalog loaded at startup (the game's own)"));
            il.Emit(OpCodes.Br, ret);
            il.Append(notOff); il.Emit(OpCodes.Ldstr, "check"); il.Emit(OpCodes.Call, strEq); il.Emit(OpCodes.Stsfld, check);
            //   try { d = Application.dataPath; if (d.EndsWith(".apk") || d.EndsWith(".obb")) d = Path.GetDirectoryName(d) + "/Game_Data";
            //         f = d + "/fmodcatalogs.txt"; if (!File.Exists(f)) leave; lines = File.ReadAllLines(f);
            //         if (lines.Length < 2 || lines[0] != Header) leave; n = lines.Length - 1; p = new string[n]; o = new string[n];
            //         for (i = 0; i < n; i++) { l = lines[i + 1]; tab = l.IndexOf('\t'); if (tab <= 0) leave; p[i] = l.Substring(0, tab); o[i] = l.Substring(tab + 1); }
            //         paths = p; owners = o; state = 1; } catch (Exception) { }
            var tryStart = il.Create(OpCodes.Call, dataPath);
            var keep = il.Create(OpCodes.Ldloc, d);
            var gameData = il.Create(OpCodes.Ldloc, d);
            var loop = il.Create(OpCodes.Ldloc, i);
            var done = il.Create(OpCodes.Ldloc, p);
            var leaveBad = il.Create(OpCodes.Leave, logIt);
            il.Append(tryStart); il.Emit(OpCodes.Stloc, d);
            il.Emit(OpCodes.Ldloc, d); il.Emit(OpCodes.Ldstr, ".apk"); il.Emit(OpCodes.Ldc_I4_4); il.Emit(OpCodes.Callvirt, endsWith); il.Emit(OpCodes.Brtrue, gameData);
            il.Emit(OpCodes.Ldloc, d); il.Emit(OpCodes.Ldstr, ".obb"); il.Emit(OpCodes.Ldc_I4_4); il.Emit(OpCodes.Callvirt, endsWith); il.Emit(OpCodes.Brfalse, keep);
            il.Append(gameData); il.Emit(OpCodes.Call, dirName); il.Emit(OpCodes.Ldstr, "/Game_Data"); il.Emit(OpCodes.Call, concat2); il.Emit(OpCodes.Stloc, d);
            il.Append(keep); il.Emit(OpCodes.Ldstr, "/" + ListFile); il.Emit(OpCodes.Call, concat2); il.Emit(OpCodes.Stloc, f);
            il.Emit(OpCodes.Ldloc, f); il.Emit(OpCodes.Call, exists); il.Emit(OpCodes.Brfalse, leaveBad);
            il.Emit(OpCodes.Ldloc, f); il.Emit(OpCodes.Call, readLines); il.Emit(OpCodes.Stloc, lines);
            il.Emit(OpCodes.Ldloc, lines); il.Emit(OpCodes.Ldlen); il.Emit(OpCodes.Conv_I4); il.Emit(OpCodes.Ldc_I4_2); il.Emit(OpCodes.Blt, leaveBad);
            il.Emit(OpCodes.Ldloc, lines); il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Ldelem_Ref); il.Emit(OpCodes.Ldstr, Header); il.Emit(OpCodes.Call, strEq); il.Emit(OpCodes.Brfalse, leaveBad);
            il.Emit(OpCodes.Ldloc, lines); il.Emit(OpCodes.Ldlen); il.Emit(OpCodes.Conv_I4); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Sub); il.Emit(OpCodes.Stloc, n);
            il.Emit(OpCodes.Ldloc, n); il.Emit(OpCodes.Newarr, ts.String); il.Emit(OpCodes.Stloc, p);
            il.Emit(OpCodes.Ldloc, n); il.Emit(OpCodes.Newarr, ts.String); il.Emit(OpCodes.Stloc, o);
            il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Stloc, i);
            il.Append(loop); il.Emit(OpCodes.Ldloc, n); il.Emit(OpCodes.Bge, done);
            il.Emit(OpCodes.Ldloc, lines); il.Emit(OpCodes.Ldloc, i); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Add); il.Emit(OpCodes.Ldelem_Ref); il.Emit(OpCodes.Stloc, l);
            il.Emit(OpCodes.Ldloc, l); il.Emit(OpCodes.Ldc_I4, (int)'\t'); il.Emit(OpCodes.Callvirt, indexOfChar); il.Emit(OpCodes.Stloc, tab);
            il.Emit(OpCodes.Ldloc, tab); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Blt, leaveBad);
            il.Emit(OpCodes.Ldloc, p); il.Emit(OpCodes.Ldloc, i); il.Emit(OpCodes.Ldloc, l); il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Ldloc, tab); il.Emit(OpCodes.Callvirt, sub2); il.Emit(OpCodes.Stelem_Ref);
            il.Emit(OpCodes.Ldloc, o); il.Emit(OpCodes.Ldloc, i); il.Emit(OpCodes.Ldloc, l); il.Emit(OpCodes.Ldloc, tab); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Add); il.Emit(OpCodes.Callvirt, sub1); il.Emit(OpCodes.Stelem_Ref);
            il.Emit(OpCodes.Ldloc, i); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Add); il.Emit(OpCodes.Stloc, i); il.Emit(OpCodes.Br, loop);
            il.Append(done); il.Emit(OpCodes.Stsfld, paths); il.Emit(OpCodes.Ldloc, o); il.Emit(OpCodes.Stsfld, owners);
            il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Stsfld, state);
            il.Append(leaveBad);
            var handler = il.Create(OpCodes.Pop);
            il.Append(handler); il.Emit(OpCodes.Leave, logIt);
            map.Body.ExceptionHandlers.Add(new ExceptionHandler(ExceptionHandlerType.Catch)
            {
                TryStart = tryStart, TryEnd = handler, HandlerStart = handler, HandlerEnd = logIt, CatchType = exc,
            });
            //   Debug.Log("[blasphemous-catalogs] " + (state == 1 ? n + " enemy catalogs ... " : "no list ...") + f);
            il.Append(logIt);
            var none = il.Create(OpCodes.Nop);
            il.Emit(OpCodes.Ldsfld, state); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Bne_Un, none);
            Log(il, () => il.Emit(OpCodes.Ldstr, "[blasphemous-catalogs] "), () => { il.Emit(OpCodes.Ldsfld, paths); il.Emit(OpCodes.Ldlen); il.Emit(OpCodes.Conv_I4); il.Emit(OpCodes.Box, ts.Int32); },
                () => il.Emit(OpCodes.Ldstr, " enemy catalogs, each loaded with the first level that has its owner's type (list "),
                () => il.Emit(OpCodes.Ldloc, f), () => il.Emit(OpCodes.Ldstr, ")"));
            il.Emit(OpCodes.Br, ret);
            il.Append(none);
            Log(il, () => il.Emit(OpCodes.Ldstr, "[blasphemous-catalogs] no list ("), () => il.Emit(OpCodes.Ldloc, f),
                () => il.Emit(OpCodes.Ldstr, "): every enemy catalog loaded at startup (the game's own)"));
            il.Append(ret); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Ceq); il.Emit(OpCodes.Ret);
            map.Body.OptimizeMacros();
        }

        // static void Compare(FMODAudioCatalog[] all): check mode, at startup: the list against the game's own load
        var compare = Method("Compare", ts.Void, ("all", catArr));
        {
            var n = Local(compare, ts.Int32); var i = Local(compare, ts.Int32); var bad = Local(compare, ts.Int32);
            var a = Local(compare, cat); var b = Local(compare, cat); var own = Local(compare, ts.String); var same = Local(compare, ts.Boolean);
            var il = compare.Body.GetILProcessor();
            il.Emit(OpCodes.Ldsfld, paths); il.Emit(OpCodes.Ldlen); il.Emit(OpCodes.Conv_I4); il.Emit(OpCodes.Stloc, n);
            var lengthsOk = il.Create(OpCodes.Ldc_I4_0);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldlen); il.Emit(OpCodes.Conv_I4); il.Emit(OpCodes.Ldloc, n); il.Emit(OpCodes.Beq, lengthsOk);
            il.Emit(OpCodes.Ldloc, bad); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Add); il.Emit(OpCodes.Stloc, bad);
            il.Append(lengthsOk); il.Emit(OpCodes.Stloc, i);
            var loop = il.Create(OpCodes.Ldloc, i);
            var end = il.Create(OpCodes.Nop);
            var next = il.Create(OpCodes.Ldloc, i);
            il.Append(loop); il.Emit(OpCodes.Ldloc, n); il.Emit(OpCodes.Bge, end);
            il.Emit(OpCodes.Ldloc, i); il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldlen); il.Emit(OpCodes.Conv_I4); il.Emit(OpCodes.Bge, end);
            //   a = all[i]; b = Resources.Load(paths[i], typeof(FMODAudioCatalog)) as FMODAudioCatalog;
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldloc, i); il.Emit(OpCodes.Ldelem_Ref); il.Emit(OpCodes.Stloc, a);
            il.Emit(OpCodes.Ldsfld, paths); il.Emit(OpCodes.Ldloc, i); il.Emit(OpCodes.Ldelem_Ref);
            il.Emit(OpCodes.Ldtoken, cat); il.Emit(OpCodes.Call, fromHandle); il.Emit(OpCodes.Call, load); il.Emit(OpCodes.Isinst, cat); il.Emit(OpCodes.Stloc, b);
            //   own = a.Owner == null ? "-" : a.Owner.GetType().FullName;
            var hasOwner = il.Create(OpCodes.Ldloc, a);
            var ownDone = il.Create(OpCodes.Stloc, own);
            il.Emit(OpCodes.Ldloc, a); il.Emit(OpCodes.Ldfld, owner); il.Emit(OpCodes.Ldnull); il.Emit(OpCodes.Call, uEq); il.Emit(OpCodes.Brfalse, hasOwner);
            il.Emit(OpCodes.Ldstr, "-"); il.Emit(OpCodes.Br, ownDone);
            il.Append(hasOwner); il.Emit(OpCodes.Ldfld, owner); il.Emit(OpCodes.Callvirt, getType); il.Emit(OpCodes.Callvirt, fullName);
            il.Append(ownDone);
            //   same = a == b && own == owners[i]; if (!same) { bad++; log }
            var fail = il.Create(OpCodes.Ldc_I4_0);
            var sameDone = il.Create(OpCodes.Stloc, same);
            il.Emit(OpCodes.Ldloc, a); il.Emit(OpCodes.Ldloc, b); il.Emit(OpCodes.Call, uEq); il.Emit(OpCodes.Brfalse, fail);
            il.Emit(OpCodes.Ldloc, own); il.Emit(OpCodes.Ldsfld, owners); il.Emit(OpCodes.Ldloc, i); il.Emit(OpCodes.Ldelem_Ref); il.Emit(OpCodes.Call, strEq);
            il.Emit(OpCodes.Br, sameDone);
            il.Append(fail); il.Append(sameDone);
            il.Emit(OpCodes.Ldloc, same); il.Emit(OpCodes.Brtrue, next);
            il.Emit(OpCodes.Ldloc, bad); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Add); il.Emit(OpCodes.Stloc, bad);
            Log(il, () => il.Emit(OpCodes.Ldstr, "[blasphemous-catalogs] check DIFFERENT at "), () => { il.Emit(OpCodes.Ldloc, i); il.Emit(OpCodes.Box, ts.Int32); },
                () => il.Emit(OpCodes.Ldstr, ": the game's load has "), () => { il.Emit(OpCodes.Ldloc, a); il.Emit(OpCodes.Callvirt, uName); },
                () => il.Emit(OpCodes.Ldstr, " (owner "), () => il.Emit(OpCodes.Ldloc, own), () => il.Emit(OpCodes.Ldstr, "), the list "),
                () => { il.Emit(OpCodes.Ldsfld, paths); il.Emit(OpCodes.Ldloc, i); il.Emit(OpCodes.Ldelem_Ref); },
                () => il.Emit(OpCodes.Ldstr, " (owner "), () => { il.Emit(OpCodes.Ldsfld, owners); il.Emit(OpCodes.Ldloc, i); il.Emit(OpCodes.Ldelem_Ref); },
                () => il.Emit(OpCodes.Ldstr, "), Resources.Load gives "),
                () => { var nul = il.Create(OpCodes.Ldstr, "nothing"); var nd = il.Create(OpCodes.Nop); il.Emit(OpCodes.Ldloc, b); il.Emit(OpCodes.Brfalse, nul);
                        il.Emit(OpCodes.Ldloc, b); il.Emit(OpCodes.Callvirt, uName); il.Emit(OpCodes.Br, nd); il.Append(nul); il.Append(nd); });
            il.Append(next); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Add); il.Emit(OpCodes.Stloc, i); il.Emit(OpCodes.Br, loop);
            il.Append(end);
            Log(il, () => il.Emit(OpCodes.Ldstr, "[blasphemous-catalogs] check: the game's load "), () => { il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldlen); il.Emit(OpCodes.Conv_I4); il.Emit(OpCodes.Box, ts.Int32); },
                () => il.Emit(OpCodes.Ldstr, " catalogs, the list "), () => { il.Emit(OpCodes.Ldloc, n); il.Emit(OpCodes.Box, ts.Int32); },
                () => il.Emit(OpCodes.Ldstr, ", entries different (order, object, owner's type, count): "), () => { il.Emit(OpCodes.Ldloc, bad); il.Emit(OpCodes.Box, ts.Int32); });
            il.Emit(OpCodes.Ret);
            compare.Body.OptimizeMacros();
        }

        // static FMODAudioCatalog[] Startup(string path): no list: the game's load; else none (check: the game's load compared first)
        var startup = Method("Startup", catArr, ("path", ts.String));
        {
            var il = startup.Body.GetILProcessor();
            var list = il.Create(OpCodes.Ldsfld, check);
            il.Emit(OpCodes.Call, map); il.Emit(OpCodes.Brtrue, list);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Call, loadAll); il.Emit(OpCodes.Ret);
            var empty = il.Create(OpCodes.Ldc_I4_0);
            il.Append(list); il.Emit(OpCodes.Brfalse, empty);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Call, loadAll); il.Emit(OpCodes.Call, compare);
            il.Append(empty); il.Emit(OpCodes.Newarr, cat); il.Emit(OpCodes.Ret);
            startup.Body.OptimizeMacros();
        }

        // static FMODAudioCatalog[] Candidates(FMODAudioCatalog[] all, IEnumerable<Type> types): no list: all; else the list's
        // catalogs whose owner class is one of types, in the list's order, loaded by path
        var candidates = Method("Candidates", catArr, ("all", catArr), ("types", m.ImportReference(loadAllTypes(game, m))));
        {
            var names = Local(candidates, ts.String); var e = Local(candidates, getEnum.ReturnType); var d = Local(candidates, idisp);
            var n = Local(candidates, ts.Int32); var res = Local(candidates, catArr); var k = Local(candidates, ts.Int32); var i = Local(candidates, ts.Int32);
            var c = Local(candidates, cat); var r = Local(candidates, catArr); var shown = Local(candidates, ts.String);
            var il = candidates.Body.GetILProcessor();
            var list = il.Create(OpCodes.Ldstr, "\n");
            il.Emit(OpCodes.Call, map); il.Emit(OpCodes.Brtrue, list);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ret);
            //   names = "\n"; e = types.GetEnumerator(); while (e.MoveNext()) names = names + ((Type)e.Current).FullName + "\n"; (e as IDisposable)?.Dispose();
            il.Append(list); il.Emit(OpCodes.Stloc, names);
            il.Emit(OpCodes.Ldarg_1); il.Emit(OpCodes.Callvirt, getEnum); il.Emit(OpCodes.Stloc, e);
            var more = il.Create(OpCodes.Ldloc, e);
            var body = il.Create(OpCodes.Ldloc, names);
            il.Emit(OpCodes.Br, more);
            il.Append(body); il.Emit(OpCodes.Ldloc, e); il.Emit(OpCodes.Callvirt, current); il.Emit(OpCodes.Castclass, typeT); il.Emit(OpCodes.Callvirt, fullName);
            il.Emit(OpCodes.Ldstr, "\n"); il.Emit(OpCodes.Call, concat3); il.Emit(OpCodes.Stloc, names);
            il.Append(more); il.Emit(OpCodes.Callvirt, moveNext); il.Emit(OpCodes.Brtrue, body);
            var disposed = il.Create(OpCodes.Ldsfld, paths);
            il.Emit(OpCodes.Ldloc, e); il.Emit(OpCodes.Isinst, idisp); il.Emit(OpCodes.Stloc, d);
            il.Emit(OpCodes.Ldloc, d); il.Emit(OpCodes.Brfalse, disposed); il.Emit(OpCodes.Ldloc, d); il.Emit(OpCodes.Callvirt, dispose);
            //   n = paths.Length; res = new FMODAudioCatalog[n]; for (i...) if (names.IndexOf("\n" + owners[i] + "\n") >= 0) { c = Load(...); if (c != null) res[k++] = c; }
            il.Append(disposed); il.Emit(OpCodes.Ldlen); il.Emit(OpCodes.Conv_I4); il.Emit(OpCodes.Stloc, n);
            il.Emit(OpCodes.Ldloc, n); il.Emit(OpCodes.Newarr, cat); il.Emit(OpCodes.Stloc, res);
            var loop = il.Create(OpCodes.Ldloc, i);
            var next = il.Create(OpCodes.Ldloc, i);
            var end = il.Create(OpCodes.Ldloc, k);
            il.Append(loop); il.Emit(OpCodes.Ldloc, n); il.Emit(OpCodes.Bge, end);
            il.Emit(OpCodes.Ldloc, names); il.Emit(OpCodes.Ldstr, "\n"); il.Emit(OpCodes.Ldsfld, owners); il.Emit(OpCodes.Ldloc, i); il.Emit(OpCodes.Ldelem_Ref);
            il.Emit(OpCodes.Ldstr, "\n"); il.Emit(OpCodes.Call, concat3); il.Emit(OpCodes.Ldc_I4_4); il.Emit(OpCodes.Callvirt, indexOf);
            il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Blt, next);
            il.Emit(OpCodes.Ldsfld, paths); il.Emit(OpCodes.Ldloc, i); il.Emit(OpCodes.Ldelem_Ref);
            il.Emit(OpCodes.Ldtoken, cat); il.Emit(OpCodes.Call, fromHandle); il.Emit(OpCodes.Call, load); il.Emit(OpCodes.Isinst, cat); il.Emit(OpCodes.Stloc, c);
            il.Emit(OpCodes.Ldloc, c); il.Emit(OpCodes.Brfalse, next);
            il.Emit(OpCodes.Ldloc, res); il.Emit(OpCodes.Ldloc, k); il.Emit(OpCodes.Ldloc, c); il.Emit(OpCodes.Stelem_Ref);
            il.Emit(OpCodes.Ldloc, k); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Add); il.Emit(OpCodes.Stloc, k);
            il.Append(next); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Add); il.Emit(OpCodes.Stloc, i); il.Emit(OpCodes.Br, loop);
            //   r = new FMODAudioCatalog[k]; Array.Copy(res, r, k);
            il.Append(end); il.Emit(OpCodes.Newarr, cat); il.Emit(OpCodes.Stloc, r);
            il.Emit(OpCodes.Ldloc, res); il.Emit(OpCodes.Ldloc, r); il.Emit(OpCodes.Ldloc, k); il.Emit(OpCodes.Call, copy);
            //   check: log the level's types and the catalogs it walks
            var ret = il.Create(OpCodes.Ldloc, r);
            il.Emit(OpCodes.Ldsfld, check); il.Emit(OpCodes.Brfalse, ret);
            il.Emit(OpCodes.Ldstr, ""); il.Emit(OpCodes.Stloc, shown);
            il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Stloc, i);
            var sloop = il.Create(OpCodes.Ldloc, i);
            var send = il.Create(OpCodes.Nop);
            il.Append(sloop); il.Emit(OpCodes.Ldloc, k); il.Emit(OpCodes.Bge, send);
            il.Emit(OpCodes.Ldloc, shown); il.Emit(OpCodes.Ldloc, r); il.Emit(OpCodes.Ldloc, i); il.Emit(OpCodes.Ldelem_Ref); il.Emit(OpCodes.Callvirt, uName);
            il.Emit(OpCodes.Ldstr, " "); il.Emit(OpCodes.Call, concat3); il.Emit(OpCodes.Stloc, shown);
            il.Emit(OpCodes.Ldloc, i); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Add); il.Emit(OpCodes.Stloc, i); il.Emit(OpCodes.Br, sloop);
            il.Append(send);
            Log(il, () => il.Emit(OpCodes.Ldstr, "[blasphemous-catalogs] level: enemy types"),
                () => { il.Emit(OpCodes.Ldloc, names); il.Emit(OpCodes.Ldstr, "\n"); il.Emit(OpCodes.Ldstr, " "); il.Emit(OpCodes.Callvirt, replace); },
                () => il.Emit(OpCodes.Ldstr, "-> catalogs walked: "), () => il.Emit(OpCodes.Ldloc, shown));
            il.Append(ret); il.Emit(OpCodes.Ret);
            candidates.Body.OptimizeMacros();
        }
        return (startup, candidates);
    }

    /// <summary>IEnumerable&lt;Type&gt;, as GetLevelEnemyTypes returns it.</summary>
    static TypeReference loadAllTypes(Game game, ModuleDefinition m) =>
        m.GetType(LoaderType).Methods.First(x => x.Name == "GetLevelEnemyTypes").ReturnType;
}
