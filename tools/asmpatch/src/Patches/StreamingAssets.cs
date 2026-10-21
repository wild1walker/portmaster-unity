// streaming-assets, streaming-assets-all, data-path: a desktop build's own file access to
// StreamingAssets and to its data folder on Unity's Android player.
//
// On a desktop, Application.streamingAssetsPath is a folder (<Game>_Data/StreamingAssets) and games
// read it with System.IO: File.ReadAllText, Directory.GetFiles, FileStream, XML and SQLite files,
// mod or level folders. On Android it is "jar:file://<apk or obb>!/assets": the files are inside the
// archive, where Unity's own APIs (AssetBundle, UnityWebRequest, VideoPlayer) find them and System.IO
// does not. pmunity android-data --streaming-assets puts StreamingAssets beside the APK as a folder
// (StreamingAssets/), and this patch gives the game's code that folder where it read the getter:
//
//   streaming-assets       only the reads whose value reaches System.IO (or a native library's path
//                          argument); every other read keeps the jar path, which Unity's APIs read
//                          from the OBB (android-data --streaming-assets both: the files are in both)
//   streaming-assets-all   every read (android-data --streaming-assets loose: the files are only
//                          beside the APK; Unity's APIs read a plain path from the folder, as on the
//                          desktop)
//
//   data-path              the reads of Application.dataPath that reach System.IO: on Android it is
//                          the APK or OBB file; they get the Game_Data folder beside it
//                          (android-data --data-path puts the data folder's own files there)
//
// Each read `call Application::get_streamingAssetsPath()` becomes a call of a small static method
// the patch writes into the same assembly (<asmpatch>Paths::StreamingAssets): it reads
// Application.streamingAssetsPath, and only when that is a "jar:file://<dir>/<archive>!/..." path does
// it return "<dir>/Game_Data/StreamingAssets" instead (<asmpatch>Paths::DataPath: "<dir>/Game_Data"
// for a dataPath "<dir>/<x>.apk|obb"). On a desktop player the patched assembly behaves as the
// original. The patches are opt-in (asmpatch runs them only when named: --only, --with).
//
// Which reads reach what comes from PathFlow below, a forward data-flow analysis over the IL of the
// game's assemblies (and Unity's packages that ship with it, e.g. Addressables), which --survey also
// prints for the other paths desktop games use (dataPath, persistentDataPath, the working directory).
using Mono.Cecil;
using Mono.Cecil.Cil;
using Mono.Cecil.Rocks;

namespace AsmPatch;

/// <summary>Reads of a folder getter that the Android player answers with something System.IO
/// cannot use become calls of a helper the patch writes into the same assembly, which returns a
/// folder beside the APK there and the getter's own value anywhere else.</summary>
abstract class FolderPatch : Patch
{
    public override bool OptIn => true;
    protected virtual bool EveryRead => false;
    protected abstract PathFlow.Src Source { get; }
    protected abstract string Getter { get; }                   // (for messages)
    protected abstract string Method { get; }                   // the helper's method
    protected abstract string Folder { get; }                   // what it returns, beside the APK
    protected abstract string Kept { get; }                     // why reads not redirected are fine

    public const string HelperType = "<asmpatch>Paths";

    public override Result Run(Game game, bool apply)
    {
        var flow = PathFlow.For(game);
        var reads = flow.Sites.Where(s => s.Source == Source && !s.Patched).ToList();
        var pick = reads.Where(s => EveryRead || s.ReachesFiles).ToList();
        var patched = flow.Sites.Where(s => s.Source == Source && s.Patched).ToList();
        if (pick.Count == 0 && patched.Count > 0)
        {
            var done = new Result { Outcome = Outcome.AlreadyApplied };
            foreach (var g in patched.GroupBy(s => s.Asm))
                done.Details.Add($"{game.FileOf(g.Key)}: {g.Count()} reads of {Getter} give {Folder}/ beside the APK");
            return done;
        }
        if (reads.Count == 0)
            return Result.NotApplicable($"no code reads Application.{Getter}");
        if (pick.Count == 0)
            return Result.NotApplicable($"{reads.Count} reads of Application.{Getter}, none reaches System.IO ({Describe(reads)}): {Kept}");
        var r = new Result { Outcome = Outcome.Applied };
        foreach (var g in pick.GroupBy(s => s.Asm))
        {
            var asm = g.Key;
            var file = game.FileOf(asm);
            foreach (var s in g)
                r.Details.Add($"{file}: {PathFlow.Where(s.Method)}: {Getter} -> {Folder}/ beside the APK" +
                              (s.Sinks.Count > 0 ? $" ({string.Join(", ", s.Sinks.Take(6))}{(s.Sinks.Count > 6 ? ", ..." : "")})" : " (no sink found)"));
            if (!apply) continue;
            var helper = Helper(asm.MainModule, g.First().Ins.Operand as MethodReference);
            foreach (var s in g)
                s.Ins.Operand = helper;             // same stack effect: () -> string
            game.MarkChanged(asm);
        }
        var kept = reads.Except(pick).ToList();
        if (kept.Count > 0)
            r.Details.Add($"kept {Getter} at {kept.Count} reads that reach only Unity's APIs or nothing found ({Describe(kept)})");
        return r;
    }

    static string Describe(List<PathFlow.Site> sites) =>
        string.Join(", ", sites.Select(s => PathFlow.Where(s.Method)).Distinct().Take(4)) + (sites.Count > 4 ? ", ..." : "");

    /// <summary>The module's helper method (written once per module and method): it reads the
    /// getter and passes its value to Body, which returns either it or the folder. Only the module's
    /// own corlib and the getter reference it already has are used.</summary>
    MethodDefinition Helper(ModuleDefinition m, MethodReference getter)
    {
        var ts = m.TypeSystem;
        var t = m.Types.FirstOrDefault(x => x.Namespace == "" && x.Name == HelperType);
        if (t == null)
        {
            t = new TypeDefinition("", HelperType, TypeAttributes.NotPublic | TypeAttributes.Abstract | TypeAttributes.Sealed |
                                                   TypeAttributes.BeforeFieldInit | TypeAttributes.Class, ts.Object);
            m.Types.Add(t);
        }
        var md = t.Methods.FirstOrDefault(x => x.Name == Method);
        if (md != null) return md;
        md = new MethodDefinition(Method, MethodAttributes.Assembly | MethodAttributes.Static | MethodAttributes.HideBySig, ts.String);
        t.Methods.Add(md);
        var b = md.Body;
        b.InitLocals = true;
        var p = new VariableDefinition(ts.String);
        b.Variables.Add(p);
        var il = b.GetILProcessor();
        var ret = il.Create(OpCodes.Ldloc, p);
        il.Emit(OpCodes.Call, m.ImportReference(getter));
        il.Emit(OpCodes.Stloc, p);
        il.Emit(OpCodes.Ldloc, p); il.Emit(OpCodes.Brfalse, ret);
        Body(m, il, b, p, ret);
        il.Append(ret); il.Emit(OpCodes.Ret);
        b.OptimizeMacros();
        return md;
    }

    /// <summary>The helper's middle part: p holds the getter's value (not null); branch to ret to
    /// return it, or return something else.</summary>
    protected abstract void Body(ModuleDefinition m, ILProcessor il, MethodBody b, VariableDefinition p, Instruction ret);

    protected static MethodReference Str(ModuleDefinition m, string name, TypeReference ret, bool inst, params TypeReference[] ps)
    {
        var r = new MethodReference(name, ret, m.TypeSystem.String) { HasThis = inst };
        foreach (var x in ps) r.Parameters.Add(new ParameterDefinition(x));
        return r;
    }

    /// <summary>return p.Substring(0, slash) + suffix</summary>
    protected static void ReturnFolder(ModuleDefinition m, ILProcessor il, VariableDefinition p, VariableDefinition slash, string suffix)
    {
        var ts = m.TypeSystem;
        il.Emit(OpCodes.Ldloc, p); il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Ldloc, slash);
        il.Emit(OpCodes.Callvirt, Str(m, "Substring", ts.String, true, ts.Int32, ts.Int32));
        il.Emit(OpCodes.Ldstr, suffix); il.Emit(OpCodes.Call, Str(m, "Concat", ts.String, false, ts.String, ts.String)); il.Emit(OpCodes.Ret);
    }
}

class StreamingAssetsPatch : FolderPatch
{
    public override string Name => "streaming-assets";
    public override string Summary =>
        "Application.streamingAssetsPath reads that reach System.IO get the StreamingAssets folder beside the APK (android-data --streaming-assets both; opt-in)";
    protected override PathFlow.Src Source => PathFlow.Src.StreamingAssets;
    protected override string Getter => "streamingAssetsPath";
    protected override string Method => HelperMethod;
    protected override string Folder => FolderName;
    protected override string Kept => "Unity's APIs read the jar path";
    public const string HelperMethod = "StreamingAssets";
    public const string FolderName = "Game_Data/StreamingAssets";   // beside the APK (android-data --streaming-assets)

    // p = "jar:file://<dir>/<archive>!/assets": if it starts so and has a '!' after it and a '/' in
    // the archive part, return "<dir>/Game_Data/StreamingAssets"
    protected override void Body(ModuleDefinition m, ILProcessor il, MethodBody b, VariableDefinition p, Instruction ret)
    {
        var ts = m.TypeSystem;
        const string Jar = "jar:file://";
        var bang = new VariableDefinition(ts.Int32);
        var arch = new VariableDefinition(ts.String);
        var slash = new VariableDefinition(ts.Int32);
        b.Variables.Add(bang); b.Variables.Add(arch); b.Variables.Add(slash);
        var lastIndexOf = Str(m, "LastIndexOf", ts.Int32, true, ts.Char);
        // string.CompareOrdinal(p, 0, "jar:file://", 0, 11) == 0
        il.Emit(OpCodes.Ldloc, p); il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Ldstr, Jar); il.Emit(OpCodes.Ldc_I4_0);
        il.Emit(OpCodes.Ldc_I4, Jar.Length); il.Emit(OpCodes.Call, Str(m, "CompareOrdinal", ts.Int32, false, ts.String, ts.Int32, ts.String, ts.Int32, ts.Int32));
        il.Emit(OpCodes.Brtrue, ret);
        // bang = p.LastIndexOf('!'); if (bang <= 11) return p
        il.Emit(OpCodes.Ldloc, p); il.Emit(OpCodes.Ldc_I4, (int)'!'); il.Emit(OpCodes.Callvirt, lastIndexOf); il.Emit(OpCodes.Stloc, bang);
        il.Emit(OpCodes.Ldloc, bang); il.Emit(OpCodes.Ldc_I4, Jar.Length); il.Emit(OpCodes.Ble, ret);
        // arch = p.Substring(11, bang - 11); slash = arch.LastIndexOf('/'); if (slash < 0) return p
        il.Emit(OpCodes.Ldloc, p); il.Emit(OpCodes.Ldc_I4, Jar.Length); il.Emit(OpCodes.Ldloc, bang); il.Emit(OpCodes.Ldc_I4, Jar.Length);
        il.Emit(OpCodes.Sub); il.Emit(OpCodes.Callvirt, Str(m, "Substring", ts.String, true, ts.Int32, ts.Int32)); il.Emit(OpCodes.Stloc, arch);
        il.Emit(OpCodes.Ldloc, arch); il.Emit(OpCodes.Ldc_I4, (int)'/'); il.Emit(OpCodes.Callvirt, lastIndexOf); il.Emit(OpCodes.Stloc, slash);
        il.Emit(OpCodes.Ldloc, slash); il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Blt, ret);
        ReturnFolder(m, il, arch, slash, "/" + FolderName);
    }
}

sealed class StreamingAssetsAllPatch : StreamingAssetsPatch
{
    public override string Name => "streaming-assets-all";
    public override string Summary =>
        "every Application.streamingAssetsPath read gets the StreamingAssets folder beside the APK (android-data --streaming-assets loose; opt-in)";
    protected override bool EveryRead => true;
}

sealed class DataPathPatch : FolderPatch
{
    public override string Name => "data-path";
    public override string Summary =>
        "Application.dataPath reads that reach System.IO get the Game_Data folder beside the APK (android-data --data-path; opt-in)";
    protected override PathFlow.Src Source => PathFlow.Src.DataPath;
    protected override string Getter => "dataPath";
    protected override string Method => HelperMethod;
    protected override string Folder => FolderName;
    protected override string Kept => "Unity's APIs read the archive";
    public const string HelperMethod = "DataPath";
    public const string FolderName = "Game_Data";

    // p = "<dir>/<x>.apk" or "<dir>/<x>.obb" (the Android player's dataPath: the archive it mounted)
    // -> "<dir>/Game_Data"
    protected override void Body(ModuleDefinition m, ILProcessor il, MethodBody b, VariableDefinition p, Instruction ret)
    {
        var ts = m.TypeSystem;
        var n = new VariableDefinition(ts.Int32);
        var slash = new VariableDefinition(ts.Int32);
        b.Variables.Add(n); b.Variables.Add(slash);
        var cmp = Str(m, "CompareOrdinal", ts.Int32, false, ts.String, ts.Int32, ts.String, ts.Int32, ts.Int32);
        il.Emit(OpCodes.Ldloc, p); il.Emit(OpCodes.Callvirt, Str(m, "get_Length", ts.Int32, true)); il.Emit(OpCodes.Stloc, n);
        il.Emit(OpCodes.Ldloc, n); il.Emit(OpCodes.Ldc_I4_5); il.Emit(OpCodes.Blt, ret);
        var archive = il.Create(OpCodes.Ldloc, p);
        foreach (var ext in new[] { ".apk", ".obb" })
        {
            il.Emit(OpCodes.Ldloc, p); il.Emit(OpCodes.Ldloc, n); il.Emit(OpCodes.Ldc_I4_4); il.Emit(OpCodes.Sub);
            il.Emit(OpCodes.Ldstr, ext); il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Ldc_I4_4); il.Emit(OpCodes.Call, cmp);
            il.Emit(OpCodes.Brfalse, archive);
        }
        il.Emit(OpCodes.Br, ret);
        // slash = p.LastIndexOf('/'); if (slash < 0) return p
        il.Append(archive); il.Emit(OpCodes.Ldc_I4, (int)'/'); il.Emit(OpCodes.Callvirt, Str(m, "LastIndexOf", ts.Int32, true, ts.Char));
        il.Emit(OpCodes.Stloc, slash);
        il.Emit(OpCodes.Ldloc, slash); il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Blt, ret);
        ReturnFolder(m, il, p, slash, "/" + FolderName);
    }
}

/// <summary>Where a game's paths come from and where they go: a forward taint analysis over the IL
/// of the game's assemblies. Sources are the getters of the folders a desktop build reads
/// (Application.streamingAssetsPath, dataPath, persistentDataPath, ..., the working directory); each
/// read is a site with its own label. Labels flow through the stack, locals, arguments, fields
/// (static and instance alike, by name), return values (also of the game's virtual methods and
/// lambdas), arrays and containers (a collection or builder a labelled value was added to), and
/// through every call outside the analysed code that returns a string, a path object or a
/// collection (string.Concat, Path.Combine, Directory.GetFiles, ...). A sink is a call that touches
/// the file system with a labelled argument: System.IO, XML and SQLite loaders by path, a
/// P/Invoke's arguments; Unity's AssetBundle/UnityWebRequest/WWW/VideoPlayer calls are recorded
/// apart. It over-approximates (a label reaches more than it does at run time), which for the
/// patch means a read is redirected where it need not be, never the reverse.</summary>
sealed class PathFlow
{
    public enum Src { StreamingAssets, DataPath, PersistentDataPath, TemporaryCachePath, CurrentDirectory, BaseDirectory, AssemblyLocation }

    public sealed class Site
    {
        public int Id;
        public Src Source;
        public AssemblyDefinition Asm;
        public MethodDefinition Method;
        public Instruction Ins;
        public readonly SortedSet<string> Sinks = new(StringComparer.Ordinal);   // "File.ReadAllText", ...
        public bool ReachesFiles, ReachesWrite, ReachesUnity;
        public bool Patched;                                    // a call of the streaming-assets helper
    }

    public readonly List<Site> Sites = new();
    public int HelperSites;                                     // calls of an earlier run's helper

    static readonly Dictionary<Game, PathFlow> cache = new();
    public static PathFlow For(Game g)
    {
        if (!cache.TryGetValue(g, out var f)) cache[g] = f = new PathFlow(g);
        return f;
    }

    static (string type, string member, Src src)[] Sources =
    {
        ("UnityEngine.Application", "get_streamingAssetsPath", Src.StreamingAssets),
        ("UnityEngine.Application", "get_dataPath", Src.DataPath),
        ("UnityEngine.Application", "get_persistentDataPath", Src.PersistentDataPath),
        ("UnityEngine.Application", "get_temporaryCachePath", Src.TemporaryCachePath),
        ("System.IO.Directory", "GetCurrentDirectory", Src.CurrentDirectory),
        ("System.Environment", "get_CurrentDirectory", Src.CurrentDirectory),
        ("System.AppDomain", "get_BaseDirectory", Src.BaseDirectory),
        ("System.Reflection.Assembly", "get_Location", Src.AssemblyLocation),
        ("System.Reflection.Assembly", "get_CodeBase", Src.AssemblyLocation),
    };

    // ---- the code analysed -----------------------------------------------------------------
    /// <summary>Everything but the class libraries and Unity's engine modules (replaced by the
    /// Android player's own): the game's code, its libraries and Unity's packages (Addressables).</summary>
    public static bool Analysed(string n) =>
        !(n == "mscorlib" || n == "netstandard" || n.StartsWith("System") || n.StartsWith("Mono.") || n.StartsWith("Microsoft.") ||
          n == "UnityEngine" || n.StartsWith("UnityEngine.") && n.EndsWith("Module"));

    sealed class MState
    {
        public HashSet<int>[] Args, Locals;
        public HashSet<int> Ret;
    }

    readonly Dictionary<string, List<MethodDefinition>> byKey = new(StringComparer.Ordinal);       // name/arity keys
    readonly Dictionary<string, List<MethodDefinition>> virtualsByName = new(StringComparer.Ordinal);
    readonly Dictionary<string, HashSet<int>> fields = new(StringComparer.Ordinal);
    readonly Dictionary<string, List<MethodDefinition>> fieldReaders = new(StringComparer.Ordinal);
    readonly Dictionary<string, List<MethodDefinition>> callers = new(StringComparer.Ordinal);
    readonly Dictionary<MethodDefinition, MState> states = new();
    readonly Dictionary<MethodDefinition, AssemblyDefinition> asmOf = new();
    readonly Dictionary<Instruction, Site> siteAt = new();
    readonly Dictionary<TypeDefinition, HashSet<string>> supers = new();
    readonly Dictionary<(string, bool), List<MethodDefinition>> targetCache = new();
    readonly Queue<MethodDefinition> work = new();
    readonly HashSet<MethodDefinition> queued = new();

    public static string MKey(MethodReference m) => m.DeclaringType.GetElementType().FullName + "::" + m.Name + "/" + m.Parameters.Count;
    static string FKey(FieldReference f) => f.DeclaringType.GetElementType().FullName + "::" + f.Name;
    public static string Where(MethodDefinition m) => m.DeclaringType.FullName + "." + m.Name;

    PathFlow(Game game)
    {
        foreach (var asm in game.Assemblies.Where(a => Analysed(a.Name.Name)))
            foreach (var t in Cecil.AllTypes(asm.MainModule))
                foreach (var m in t.Methods)
                {
                    asmOf[m] = asm;
                    Add(byKey, MKey(m), m);
                    if (m.IsVirtual && !m.IsStatic) Add(virtualsByName, m.Name + "/" + m.Parameters.Count, m);
                    if (!m.HasBody) continue;
                    foreach (var i in m.Body.Instructions)
                    {
                        if (i.Operand is FieldReference f && (i.OpCode.Code is Code.Ldfld or Code.Ldsfld or Code.Ldflda or Code.Ldsflda))
                            Add(fieldReaders, FKey(f), m);
                        else if (i.Operand is MethodReference mr)
                        {
                            Add(callers, MKey(mr), m);
                            var dt = mr.DeclaringType.GetElementType().FullName;
                            var src = Sources.FirstOrDefault(s => s.type == dt && s.member == mr.Name && mr.Parameters.Count == 0);
                            bool helper = dt == FolderPatch.HelperType;
                            if (t.Name == FolderPatch.HelperType) continue;              // (the helper's own reads)
                            if ((src.type != null || helper) && i.OpCode.Code is Code.Call or Code.Callvirt)
                            {
                                var hs = mr.Name == DataPathPatch.HelperMethod ? Src.DataPath : Src.StreamingAssets;
                                var site = new Site { Id = Sites.Count, Source = helper ? hs : src.src, Asm = asm, Method = m, Ins = i, Patched = helper };
                                Sites.Add(site);
                                siteAt[i] = site;
                                Enqueue(m);
                            }
                            if (helper) HelperSites++;
                        }
                    }
                }
        while (work.Count > 0)
        {
            var m = work.Dequeue();
            queued.Remove(m);
            Analyse(m);
        }
        foreach (var s in Sites)
            s.ReachesFiles = s.Sinks.Any(x => !x.StartsWith("Unity:"));
    }

    static void Add<T>(Dictionary<string, List<T>> d, string k, T v)
    {
        if (!d.TryGetValue(k, out var l)) d[k] = l = new List<T>();
        if (!l.Contains(v)) l.Add(v);
    }

    void Enqueue(MethodDefinition m) { if (m.HasBody && queued.Add(m)) work.Enqueue(m); }
    void EnqueueAll(List<MethodDefinition> l) { if (l != null) foreach (var m in l) Enqueue(m); }

    MState State(MethodDefinition m)
    {
        if (!states.TryGetValue(m, out var s))
            states[m] = s = new MState
            {
                Args = new HashSet<int>[m.Parameters.Count + (m.HasThis ? 1 : 0)],
                Locals = new HashSet<int>[m.HasBody ? m.Body.Variables.Count : 0],
            };
        return s;
    }

    static bool UnionInto(ref HashSet<int> dst, HashSet<int> src)
    {
        if (src == null || src.Count == 0) return false;
        if (dst == null) { dst = new HashSet<int>(src); return true; }
        int n = dst.Count;
        dst.UnionWith(src);
        return dst.Count != n;
    }

    /// <summary>a with b's labels: a itself when b adds none, else a new set (never changes a).</summary>
    static HashSet<int> Plus(HashSet<int> a, HashSet<int> b)
    {
        if (b == null || b.Count == 0 || a != null && a.IsSupersetOf(b)) return a;
        var r = a == null ? new HashSet<int>(b) : new HashSet<int>(a);
        if (a != null) r.UnionWith(b);
        return r;
    }

    static HashSet<int> U(HashSet<int> a, HashSet<int> b)
    {
        if (a == null || a.Count == 0) return b;
        if (b == null || b.Count == 0) return a;
        var r = new HashSet<int>(a); r.UnionWith(b); return r;
    }

    // a stack value: its labels and where it was loaded from (so that adding a labelled value to an
    // array or a collection labels the variable or field holding it)
    sealed class Val
    {
        public HashSet<int> T;
        public string Field; public int Local = -1, Arg = -1;
        public Val(HashSet<int> t) { T = t; }
    }

    // ---- one method ------------------------------------------------------------------------
    void Analyse(MethodDefinition m)
    {
        var st = State(m);
        var body = m.Body;
        var ins = body.Instructions;
        var entry = new Dictionary<Instruction, List<Val>>();
        if (ins.Count == 0) return;
        entry[ins[0]] = new List<Val>();
        foreach (var h in body.ExceptionHandlers)
        {
            bool exc = h.HandlerType is ExceptionHandlerType.Catch or ExceptionHandlerType.Filter;
            if (h.HandlerStart != null && !entry.ContainsKey(h.HandlerStart)) entry[h.HandlerStart] = exc ? new List<Val> { new(null) } : new List<Val>();
            if (h.FilterStart != null && !entry.ContainsKey(h.FilterStart)) entry[h.FilterStart] = new List<Val> { new(null) };
        }
        for (int pass = 0; pass < 32; pass++)
        {
            bool changed = false;
            foreach (var i in ins)
            {
                if (!entry.TryGetValue(i, out var e)) continue;
                var stack = new List<Val>(e);
                changed |= Step(m, st, i, stack);
                var c = i.OpCode.Code;
                var flow = i.OpCode.FlowControl;
                if (c is Code.Leave or Code.Leave_S)
                    stack.Clear();
                if (i.Operand is Instruction target) changed |= Merge(entry, target, stack);
                if (i.Operand is Instruction[] targets) foreach (var x in targets) changed |= Merge(entry, x, stack);
                if (flow is not (FlowControl.Branch or FlowControl.Return or FlowControl.Throw) && c is not (Code.Endfinally or Code.Jmp) && i.Next != null)
                    changed |= Merge(entry, i.Next, stack);
            }
            if (!changed) break;
        }
    }

    static bool Merge(Dictionary<Instruction, List<Val>> entry, Instruction at, List<Val> stack)
    {
        if (!entry.TryGetValue(at, out var e))
        {
            entry[at] = new List<Val>(stack);       // (the same values: a dup'ed array stays one array)
            return true;
        }
        bool ch = false;
        for (int k = 0; k < Math.Min(e.Count, stack.Count); k++)
            if (!ReferenceEquals(e[k], stack[k]))
            {
                var t = Plus(e[k].T, stack[k].T);
                if (!ReferenceEquals(t, e[k].T)) { e[k].T = t; ch = true; }
            }
        return ch;
    }

    Val Pop(List<Val> s)
    {
        if (s.Count == 0) return new Val(null);
        var v = s[^1]; s.RemoveAt(s.Count - 1); return v;
    }

    /// <summary>Labels a value that was stored into (an array element, a collection, a builder):
    /// the value itself (shared with its dup'ed copies) and the variable or field it came from.</summary>
    bool Taint(MethodDefinition m, MState st, Val v, HashSet<int> t)
    {
        if (t == null || t.Count == 0) return false;
        v.T = Plus(v.T, t);
        bool ch = false;
        if (v.Local >= 0 && v.Local < st.Locals.Length) ch |= UnionInto(ref st.Locals[v.Local], t);
        if (v.Arg >= 0 && v.Arg < st.Args.Length) ch |= UnionInto(ref st.Args[v.Arg], t);
        if (v.Field != null)
        {
            fields.TryGetValue(v.Field, out var f);
            if (UnionInto(ref f, t)) { fields[v.Field] = f; fieldReaders.TryGetValue(v.Field, out var rd); EnqueueAll(rd); }
        }
        return ch;
    }

    int ArgIndex(MethodDefinition m, Instruction i)
    {
        var c = i.OpCode.Code;
        int n = c switch
        {
            Code.Ldarg_0 => 0, Code.Ldarg_1 => 1, Code.Ldarg_2 => 2, Code.Ldarg_3 => 3,
            _ => i.Operand is ParameterDefinition p ? p.Index + (m.HasThis ? 1 : 0) : -1,
        };
        return n;
    }

    static int LocalIndex(Instruction i) => i.OpCode.Code switch
    {
        Code.Ldloc_0 or Code.Stloc_0 => 0, Code.Ldloc_1 or Code.Stloc_1 => 1,
        Code.Ldloc_2 or Code.Stloc_2 => 2, Code.Ldloc_3 or Code.Stloc_3 => 3,
        _ => i.Operand is VariableDefinition v ? v.Index : -1,
    };

    /// <summary>One instruction's effect on the stack (labels) and on the method's and the
    /// program's state. True if a local or argument of this method gained labels.</summary>
    bool Step(MethodDefinition m, MState st, Instruction i, List<Val> s)
    {
        var c = i.OpCode.Code;
        switch (c)
        {
            case Code.Ldarg_0: case Code.Ldarg_1: case Code.Ldarg_2: case Code.Ldarg_3: case Code.Ldarg: case Code.Ldarg_S:
            case Code.Ldarga: case Code.Ldarga_S:
            {
                int a = ArgIndex(m, i);
                s.Add(new Val(a >= 0 && a < st.Args.Length ? st.Args[a] : null) { Arg = a });
                return false;
            }
            case Code.Starg: case Code.Starg_S:
            {
                int a = ArgIndex(m, i);
                var v = Pop(s);
                return a >= 0 && a < st.Args.Length && UnionInto(ref st.Args[a], v.T);
            }
            case Code.Ldloc_0: case Code.Ldloc_1: case Code.Ldloc_2: case Code.Ldloc_3: case Code.Ldloc: case Code.Ldloc_S:
            case Code.Ldloca: case Code.Ldloca_S:
            {
                int l = i.OpCode.Code is Code.Ldloca or Code.Ldloca_S ? ((VariableDefinition)i.Operand).Index : LocalIndex(i);
                s.Add(new Val(l >= 0 && l < st.Locals.Length ? st.Locals[l] : null) { Local = l });
                return false;
            }
            case Code.Stloc_0: case Code.Stloc_1: case Code.Stloc_2: case Code.Stloc_3: case Code.Stloc: case Code.Stloc_S:
            {
                int l = LocalIndex(i);
                var v = Pop(s);
                return l >= 0 && l < st.Locals.Length && UnionInto(ref st.Locals[l], v.T);
            }
            case Code.Ldfld: case Code.Ldflda: case Code.Ldsfld: case Code.Ldsflda:
            {
                if (c is Code.Ldfld or Code.Ldflda) Pop(s);
                var k = FKey((FieldReference)i.Operand);
                fields.TryGetValue(k, out var t);
                s.Add(new Val(t) { Field = k });
                return false;
            }
            case Code.Stfld: case Code.Stsfld:
            {
                var v = Pop(s);
                if (c == Code.Stfld) Pop(s);
                var k = FKey((FieldReference)i.Operand);
                fields.TryGetValue(k, out var t);
                if (UnionInto(ref t, v.T)) { fields[k] = t; fieldReaders.TryGetValue(k, out var rd); EnqueueAll(rd); }
                return false;
            }
            case Code.Dup:
            {
                var v = Pop(s); s.Add(v); s.Add(v);
                return false;
            }
            case Code.Ldelem_Ref: case Code.Ldelem_Any: case Code.Ldelema:
            {
                Pop(s); var arr = Pop(s);
                s.Add(new Val(arr.T));
                return false;
            }
            case Code.Stelem_Ref: case Code.Stelem_Any:
            {
                var v = Pop(s); Pop(s); var arr = Pop(s);
                return Taint(m, st, arr, v.T);
            }
            case Code.Castclass: case Code.Isinst: case Code.Box: case Code.Unbox_Any: case Code.Unbox:
            {
                var v = Pop(s); s.Add(new Val(v.T) { Local = v.Local, Arg = v.Arg, Field = v.Field });
                return false;
            }
            case Code.Ret:
                if (m.ReturnType.MetadataType != MetadataType.Void && s.Count > 0)
                {
                    var v = Pop(s);
                    if (UnionInto(ref st.Ret, v.T)) RetChanged(m);
                }
                return false;
            case Code.Call: case Code.Callvirt: case Code.Newobj:
                return Call(m, st, i, s);
            case Code.Calli:
            {
                var cs = (CallSite)i.Operand;
                Pop(s);
                for (int k = 0; k < cs.Parameters.Count + (cs.HasThis ? 1 : 0); k++) Pop(s);
                if (cs.ReturnType.MetadataType != MetadataType.Void) s.Add(new Val(null));
                return false;
            }
        }
        // everything else: by its stack behaviour, no labels through it
        int pop = Pops(i.OpCode.StackBehaviourPop), push = Pushes(i.OpCode.StackBehaviourPush);
        if (pop < 0) s.Clear(); else for (int k = 0; k < pop; k++) Pop(s);
        for (int k = 0; k < push; k++) s.Add(new Val(null));
        return false;
    }

    static int Pops(StackBehaviour b) => b switch
    {
        StackBehaviour.Pop0 => 0,
        StackBehaviour.Pop1 or StackBehaviour.Popi or StackBehaviour.Popref => 1,
        StackBehaviour.Pop1_pop1 or StackBehaviour.Popi_pop1 or StackBehaviour.Popi_popi or StackBehaviour.Popi_popi8 or
            StackBehaviour.Popi_popr4 or StackBehaviour.Popi_popr8 or StackBehaviour.Popref_pop1 or StackBehaviour.Popref_popi => 2,
        StackBehaviour.Popi_popi_popi or StackBehaviour.Popref_popi_popi or StackBehaviour.Popref_popi_popi8 or
            StackBehaviour.Popref_popi_popr4 or StackBehaviour.Popref_popi_popr8 or StackBehaviour.Popref_popi_popref => 3,
        StackBehaviour.PopAll => -1,
        _ => 0,
    };

    static int Pushes(StackBehaviour b) => b switch
    {
        StackBehaviour.Push0 => 0,
        StackBehaviour.Push1_push1 => 2,
        StackBehaviour.Push1 or StackBehaviour.Pushi or StackBehaviour.Pushi8 or StackBehaviour.Pushr4 or StackBehaviour.Pushr8 or StackBehaviour.Pushref => 1,
        _ => 0,
    };

    /// <summary>A method returns more labels: its callers again, also those that call it through
    /// a base class or an interface (a virtual call names the method it overrides).</summary>
    void RetChanged(MethodDefinition m)
    {
        callers.TryGetValue(MKey(m), out var cl); EnqueueAll(cl);
        if (!m.IsVirtual) return;
        foreach (var s in Supers(m.DeclaringType))
            if (callers.TryGetValue(s + "::" + m.Name + "/" + m.Parameters.Count, out var bl)) EnqueueAll(bl);
    }

    // ---- calls ---------------------------------------------------------------------------
    bool Call(MethodDefinition m, MState st, Instruction i, List<Val> s)
    {
        var mr = (MethodReference)i.Operand;
        bool newobj = i.OpCode.Code == Code.Newobj;
        int n = mr.Parameters.Count + (mr.HasThis && !newobj ? 1 : 0);
        var args = new Val[n];
        for (int k = n - 1; k >= 0; k--) args[k] = Pop(s);
        bool hasRet = newobj || mr.ReturnType.MetadataType != MetadataType.Void;
        bool changed = false;

        if (siteAt.TryGetValue(i, out var site))
        {
            s.Add(new Val(new HashSet<int> { site.Id }));
            return false;
        }
        var dt = mr.DeclaringType.GetElementType();
        // a delegate made from one of the game's methods: its return labels
        if (newobj && mr.Parameters.Count == 2 && mr.Parameters[1].ParameterType.MetadataType == MetadataType.IntPtr &&
            i.Previous != null && i.Previous.OpCode.Code is Code.Ldftn or Code.Ldvirtftn && i.Previous.Operand is MethodReference fn)
        {
            HashSet<int> t = null;
            foreach (var tm in Targets(fn, false)) { t = U(t, State(tm).Ret); Add(callers, MKey(fn), m); }
            s.Add(new Val(t));
            return false;
        }
        var all = args.Aggregate((HashSet<int>)null, (a, v) => U(a, v.T));
        bool labelled = all != null && all.Count > 0;
        if (labelled) Sink(m, mr, dt, args, all, newobj);
        // (dispatching a virtual call only matters when labels can flow in or out)
        var targets = Targets(mr, i.OpCode.Code == Code.Callvirt && (labelled || Carries(mr.ReturnType)));
        if (targets.Count > 0)
        {
            HashSet<int> ret = null;
            foreach (var tm in targets)
            {
                Add(callers, MKey(tm), m);             // (its return labels come back here)
                var ts = State(tm);
                int off = newobj ? 1 : 0;              // a constructor's argument 0 is the new object
                for (int k = 0; k < n; k++)
                    if (k + off < ts.Args.Length && UnionInto(ref ts.Args[k + off], args[k].T)) Enqueue(tm);
                ret = U(ret, ts.Ret);
            }
            if (hasRet) s.Add(new Val(newobj ? null : ret));
            return changed;
        }
        // outside the analysed code: a string, path or collection result carries its arguments' labels
        if (hasRet)
            s.Add(new Val(Carries(newobj ? dt : mr.ReturnType) ? all : null));
        // a labelled value added to a collection or a builder labels the collection
        if (mr.HasThis && !newobj && n > 1 && dt.FullName != "System.String")
        {
            var rest = args.Skip(1).Aggregate((HashSet<int>)null, (a, v) => U(a, v.T));
            if (rest != null && rest.Count > 0 && Carries(dt)) changed |= Taint(m, st, args[0], rest);
        }
        // out and ref arguments of a call with labels (TryGetValue(key, out path))
        if (labelled)
            for (int k = 0, off = mr.HasThis && !newobj ? 1 : 0; k < mr.Parameters.Count; k++)
                if (mr.Parameters[k].ParameterType is ByReferenceType br && Carries(br.ElementType))
                    changed |= Taint(m, st, args[k + off], all);
        return changed;
    }

    /// <summary>The analysed methods a call can run: the method itself, or for a virtual call
    /// (and a call of a method without a body here) the overrides and implementations in types
    /// that derive from the called one's type.</summary>
    List<MethodDefinition> Targets(MethodReference mr, bool virt)
    {
        var key = MKey(mr);
        if (targetCache.TryGetValue((key, virt), out var res)) return res;
        res = new List<MethodDefinition>();
        bool isVirtual = true;
        if (byKey.TryGetValue(key, out var l))
        {
            foreach (var x in l) if (x.HasBody) res.Add(x);
            isVirtual = l.Any(x => x.IsVirtual || x.IsAbstract);
        }
        var want = mr.DeclaringType.GetElementType().FullName;
        if (virt && isVirtual && want != "System.Object" && virtualsByName.TryGetValue(mr.Name + "/" + mr.Parameters.Count, out var vs))
            foreach (var x in vs)
                if (x.HasBody && !res.Contains(x) && Supers(x.DeclaringType).Contains(want)) res.Add(x);
        targetCache[(key, virt)] = res;
        return res;
    }

    HashSet<string> Supers(TypeDefinition t)
    {
        if (supers.TryGetValue(t, out var s)) return s;
        supers[t] = s = new HashSet<string>(StringComparer.Ordinal);
        foreach (var x in Cecil.Hierarchy(t))
        {
            s.Add(x.FullName);
            foreach (var itf in x.Interfaces) s.Add(itf.InterfaceType.GetElementType().FullName);
        }
        return s;
    }

    /// <summary>Types a path travels in: strings, char arrays, builders, URIs, the System.IO
    /// path objects, collections and arrays of them, object (boxed or untyped values) and generic
    /// parameters.</summary>
    static bool Carries(TypeReference t)
    {
        if (t == null) return false;
        if (t is ByReferenceType br) t = br.ElementType;
        if (t.IsGenericParameter) return true;
        if (t is ArrayType at) return Carries(at.ElementType) || at.ElementType.MetadataType is MetadataType.Byte;
        var e = t.GetElementType();
        var ns = e.Namespace;
        switch (e.MetadataType)
        {
            case MetadataType.String: case MetadataType.Object: return true;
            case MetadataType.Boolean: case MetadataType.Char: case MetadataType.SByte: case MetadataType.Byte:
            case MetadataType.Int16: case MetadataType.UInt16: case MetadataType.Int32: case MetadataType.UInt32:
            case MetadataType.Int64: case MetadataType.UInt64: case MetadataType.Single: case MetadataType.Double:
            case MetadataType.IntPtr: case MetadataType.UIntPtr: case MetadataType.Void:
                return false;
        }
        if (ns == "System.IO") return e.Name is "FileInfo" or "DirectoryInfo" or "FileSystemInfo";
        if (ns == "System.Text") return e.Name == "StringBuilder";
        if (ns == "System") return e.Name is "Uri" or "Tuple`2" or "Tuple`3" or "ValueTuple`2" or "ValueTuple`3" or "Lazy`1" or "Func`1";
        if (ns.StartsWith("System.Collections")) return true;
        if (t is GenericInstanceType g) return g.GenericArguments.Any(Carries);
        return false;
    }

    static readonly HashSet<string> IoTypes = new(StringComparer.Ordinal)
    {
        "System.IO.File", "System.IO.Directory", "System.IO.FileInfo", "System.IO.DirectoryInfo", "System.IO.FileSystemInfo",
        "System.IO.FileStream", "System.IO.StreamReader", "System.IO.StreamWriter", "System.IO.FileSystemWatcher",
        "System.IO.Compression.ZipFile", "System.IO.IsolatedStorage.IsolatedStorageFileStream",
        "System.Xml.XmlDocument", "System.Xml.XmlReader", "System.Xml.XmlWriter", "System.Xml.XmlTextReader",
        "System.Xml.XmlTextWriter", "System.Xml.Linq.XDocument", "System.Xml.Linq.XElement",
        "Mono.Data.Sqlite.SqliteConnection", "System.Data.SQLite.SQLiteConnection", "Mono.Data.SqliteClient.SqliteConnection",
        "System.Diagnostics.Process", "System.Diagnostics.ProcessStartInfo",
    };

    void Sink(MethodDefinition m, MethodReference mr, TypeReference dt, Val[] args, HashSet<int> all, bool newobj)
    {
        var tn = dt.FullName;
        string kind = null, name = dt.Name + "." + (newobj ? "ctor" : mr.Name);
        bool write = false;
        if (IoTypes.Contains(tn))
        {
            kind = "io";
            var nm = mr.Name;
            write = tn == "System.IO.StreamWriter" || tn == "System.Xml.XmlWriter" || tn == "System.Xml.XmlTextWriter" ||
                    nm.StartsWith("Write") || nm.StartsWith("Append") || nm.StartsWith("Create") || nm.StartsWith("Save") ||
                    nm is "Delete" or "Move" or "MoveTo" or "Copy" or "CopyTo" or "Replace" or "SetAttributes" or "OpenWrite" ||
                    tn == "System.IO.FileStream" && newobj && args.Length > 1;   // (a mode argument: may write)
        }
        else if (tn.StartsWith("UnityEngine"))
        {
            if (tn == "UnityEngine.AssetBundle" && mr.Name.StartsWith("LoadFromFile")) kind = "unity";
            else if (tn.StartsWith("UnityEngine.Networking.UnityWebRequest") || tn.StartsWith("UnityEngine.Networking.DownloadHandler") ||
                     tn.StartsWith("UnityEngine.Networking.UploadHandler") || tn == "UnityEngine.WWW") kind = "unity";
            else if (tn == "UnityEngine.Video.VideoPlayer" && mr.Name == "set_url") kind = "unity";
            else if (tn == "UnityEngine.Handheld" && mr.Name == "PlayFullScreenMovie") kind = "unity";
            if (kind != null) name = "Unity:" + name;
        }
        else
        {
            MethodDefinition md = null;
            try { md = mr.Resolve(); } catch (AssemblyResolutionException) { }
            if (md != null && md.IsPInvokeImpl)
            {
                kind = "native";
                name = $"P/Invoke {md.PInvokeInfo?.Module?.Name ?? "?"}:{md.PInvokeInfo?.EntryPoint ?? md.Name}";
            }
        }
        if (kind == null) return;
        foreach (var id in all)
        {
            var site = Sites[id];
            site.Sinks.Add(name + (write ? " (write)" : ""));
            if (write) site.ReachesWrite = true;
            if (kind == "unity") site.ReachesUnity = true;
        }
    }

    // ---- --survey -------------------------------------------------------------------------
    /// <summary>The file-access section of asmpatch --survey.</summary>
    public static void Print(Game game)
    {
        var f = For(game);
        Console.WriteLine();
        Console.WriteLine("file access (paths the code reads and where they reach):");
        bool any = false;
        foreach (var src in Enum.GetValues<Src>())
        {
            var sites = f.Sites.Where(s => s.Source == src).ToList();
            if (sites.Count == 0) continue;
            any = true;
            var files = sites.Where(s => s.ReachesFiles).ToList();
            var sinks = files.SelectMany(s => s.Sinks).Where(x => !x.StartsWith("Unity:")).Distinct().OrderBy(x => x, StringComparer.Ordinal).ToList();
            var unity = sites.Where(s => s.ReachesUnity).SelectMany(s => s.Sinks).Where(x => x.StartsWith("Unity:")).Distinct().ToList();
            Console.WriteLine($"  {Label(src),-22} {sites.Count,3} reads, {files.Count} reach System.IO/native" +
                              (files.Any(s => s.ReachesWrite) ? " (writes)" : "") +
                              (sinks.Count > 0 ? ": " + string.Join(", ", sinks.Take(8)) + (sinks.Count > 8 ? ", ..." : "") : "") +
                              (unity.Count > 0 ? $"; Unity APIs: {string.Join(", ", unity.Select(x => x[6..]).Take(4))}" : ""));
            foreach (var s in files.Take(6))
                Console.WriteLine($"      {game.FileOf(s.Asm)}: {Where(s.Method)}");
            if (files.Count > 6) Console.WriteLine($"      ... {files.Count - 6} more");
        }
        if (!any) Console.WriteLine("  none of the folder getters is read");
        var sa = f.Sites.Where(s => s.Source == Src.StreamingAssets).ToList();
        if (sa.Any(s => s.ReachesFiles))
            Console.WriteLine("  -> StreamingAssets read with System.IO: android-data --streaming-assets both (asmpatch streaming-assets), " +
                              "or loose (streaming-assets-all)");
        if (f.Sites.Any(s => s.Source == Src.DataPath && s.ReachesFiles))
            Console.WriteLine("  -> files read or written at Application.dataPath: on Android that is the APK/OBB file; android-data --data-path " +
                              "(asmpatch data-path) gives those reads Game_Data/ beside the APK, with the data folder's own files");
        if (f.HelperSites > 0) Console.WriteLine($"  (streaming-assets or data-path already applied: {f.HelperSites} reads)");
    }

    static string Label(Src s) => s switch
    {
        Src.StreamingAssets => "streamingAssetsPath",
        Src.DataPath => "dataPath",
        Src.PersistentDataPath => "persistentDataPath",
        Src.TemporaryCachePath => "temporaryCachePath",
        Src.CurrentDirectory => "working directory",
        Src.BaseDirectory => "AppDomain.BaseDirectory",
        _ => "Assembly.Location",
    };
}
