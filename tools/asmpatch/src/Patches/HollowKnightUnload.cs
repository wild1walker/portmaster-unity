// hk-unload (kind Reduce, opt-in: what the game keeps loaded on a small handheld, never the game play). Hollow
// Knight changes rooms by loading the next scene additively and unloading the last; the assets only the last
// room used (its textures, sprites, sounds, meshes) stay in memory until Resources.UnloadUnusedAssets.
// GameManager.IsUnloadAssetsRequired decides that at each room change: always after a death, a dream gate, a
// boss sequence; otherwise only when Unity's own memory (Profiler's reserved minus unused) is over 2.5 GB, or
// over 2.1 GB at a change of zone. On a 1 GB handheld Unity never gets near those, so every room visited since
// the last death stays loaded, and a long stretch without a death fills the memory until the system swaps or
// stops the game. With the patch, the environment variable ASMPATCH_HK_UNLOAD chooses at run time:
//   <MB>   a room change while the system's available memory (/proc/meminfo MemAvailable) is under <MB> MB
//          unloads the unused assets, as the game itself does past its limits (on the load's black screen,
//          before its garbage collection: the load takes longer then; a room left this way loads its assets
//          again when it is entered again). The game logs "[hk-unload] ..." each time.
//   anything else, or unset: the game as it is.
// The variable is read at the first room change. Nothing else changes: no timing, physics, collision or FSM;
// the assets a loaded scene uses are never unloaded (Unity unloads only what nothing references).
using Mono.Cecil;
using Mono.Cecil.Cil;
using Mono.Cecil.Rocks;

namespace AsmPatch;

sealed class HollowKnightUnload : Patch
{
    public override string Name => "hk-unload";
    public override Kind Kind => Kind.Reduce;
    public override bool OptIn => true;
    public override string Summary =>
        "Hollow Knight unloads its unused assets at a room change while the system's available memory is under ASMPATCH_HK_UNLOAD=<MB>, as it does itself past 2.1-2.5 GB of Unity's memory (memory on small handhelds; the game play the same; opt-in)";

    public const string Variable = "ASMPATCH_HK_UNLOAD", HelperType = "<asmpatch>HollowKnightUnload";
    // the game's own limits in IsUnloadAssetsRequired: 2.5 GB, and 2.1 GB at a change of zone
    const long HighLimit = 2621440000, ZoneLimit = 2202009600;

    public override Result Run(Game game, bool apply)
    {
        var asm = game.Load("Assembly-CSharp");
        var m = asm?.MainModule;
        var gm = m?.GetType("GameManager");
        var md = gm?.Methods.FirstOrDefault(x => x.Name == "IsUnloadAssetsRequired" && !x.IsStatic && x.HasBody && x.Parameters.Count == 2 &&
                                                 x.ReturnType.MetadataType == MetadataType.Boolean &&
                                                 x.Parameters.All(p => p.ParameterType.MetadataType == MetadataType.String));
        if (gm == null || md == null)
            return Result.NotApplicable("no Hollow Knight (GameManager.IsUnloadAssetsRequired(string, string) in Assembly-CSharp)");
        var file = game.FileOf(asm);
        if (m.Types.Any(t => t.Name == HelperType))
            return new Result { Outcome = Outcome.AlreadyApplied, Details = { $"{file}: room changes unload unused assets under {Variable} MB available" } };
        // (a C# compiler writes a long compared with a uint constant as ldc.i4 of its bits and conv.u8, or as ldc.i8)
        bool Limit(long v) => md.Body.Instructions.Any(i => i.OpCode.Code == Code.Ldc_I8 && i.Operand is long l && l == v ||
                                                            Cecil.ConstInt(i) is int k && (uint)k == v && i.Next?.OpCode.Code is Code.Conv_U8 or Code.Conv_I8);
        if (!Limit(HighLimit) || !Limit(ZoneLimit))
            return Result.Failed($"{file}: GameManager.IsUnloadAssetsRequired is not as this patch knows it (its 2.5 GB and 2.1 GB limits)");
        // UnityEngine.Debug.Log(object), as the game calls it (the line is left out where the module has none)
        var log = m.GetMemberReferences().OfType<MethodReference>().FirstOrDefault(x => x.Name == "Log" && x.DeclaringType.FullName == "UnityEngine.Debug" &&
                                                                                         x.Parameters.Count == 1 && x.Parameters[0].ParameterType.MetadataType == MetadataType.Object &&
                                                                                         !x.HasThis);

        var r = new Result { Outcome = Outcome.Applied };
        r.Details.Add($"{file}: GameManager.IsUnloadAssetsRequired: true while MemAvailable is under {Variable} MB, else the game's own limits");
        if (!apply) return r;

        var low = Helper(m, log);
        var il = md.Body.GetILProcessor();
        var first = md.Body.Instructions[0];
        // if (Low()) return true; then the game's own decision
        il.InsertBefore(first, il.Create(OpCodes.Call, low));
        il.InsertBefore(first, il.Create(OpCodes.Brfalse, first));
        il.InsertBefore(first, il.Create(OpCodes.Ldc_I4_1));
        il.InsertBefore(first, il.Create(OpCodes.Ret));
        md.Body.OptimizeMacros();
        game.MarkChanged(asm);
        return r;
    }

    /// <summary>The helper class: static bool Low(): the limit (MB) read once from the variable (-1: off); the
    /// system's MemAvailable (kB) read and compared; static long Parse(string meminfo): MemAvailable's kB, -1 if
    /// none; static bool Below(long kB, int MB) (the tests call the last two).</summary>
    static MethodDefinition Helper(ModuleDefinition m, MethodReference log)
    {
        var ts = m.TypeSystem;
        var t = new TypeDefinition("", HelperType, TypeAttributes.NotPublic | TypeAttributes.Abstract | TypeAttributes.Sealed |
                                                   TypeAttributes.BeforeFieldInit | TypeAttributes.Class, ts.Object);
        m.Types.Add(t);
        TypeReference Sys(string n, bool value = false) => new TypeReference("System", n, m, ts.CoreLibrary, value);
        var str = Sys("String");
        var env = new MethodReference("GetEnvironmentVariable", ts.String, Sys("Environment"));
        env.Parameters.Add(new ParameterDefinition(ts.String));
        var tryParse = new MethodReference("TryParse", ts.Boolean, Sys("Int32", true));
        tryParse.Parameters.Add(new ParameterDefinition(ts.String)); tryParse.Parameters.Add(new ParameterDefinition(new ByReferenceType(ts.Int32)));
        var indexOf = new MethodReference("IndexOf", ts.Int32, str) { HasThis = true };
        indexOf.Parameters.Add(new ParameterDefinition(ts.String)); indexOf.Parameters.Add(new ParameterDefinition(Sys("StringComparison", true)));
        var length = new MethodReference("get_Length", ts.Int32, str) { HasThis = true };
        var chars = new MethodReference("get_Chars", ts.Char, str) { HasThis = true };
        chars.Parameters.Add(new ParameterDefinition(ts.Int32));
        var readAll = new MethodReference("ReadAllText", ts.String, new TypeReference("System.IO", "File", m, ts.CoreLibrary));
        readAll.Parameters.Add(new ParameterDefinition(ts.String));
        var longToString = new MethodReference("ToString", ts.String, Sys("Int64", true)) { HasThis = true };
        var intToString = new MethodReference("ToString", ts.String, Sys("Int32", true)) { HasThis = true };
        var concat = new MethodReference("Concat", ts.String, str);
        concat.Parameters.Add(new ParameterDefinition(new ArrayType(ts.String)));
        const MethodAttributes Static = MethodAttributes.Assembly | MethodAttributes.Static | MethodAttributes.HideBySig;

        // static int limit: 0 not read, -1 off, else MB
        var limit = new FieldDefinition("limit", FieldAttributes.Private | FieldAttributes.Static, ts.Int32);
        t.Fields.Add(limit);

        // static long Parse(string s): the number after "MemAvailable:" (kB), -1 if there is none
        var parse = new MethodDefinition("Parse", Static, ts.Int64);
        parse.Parameters.Add(new ParameterDefinition("s", ParameterAttributes.None, ts.String));
        t.Methods.Add(parse);
        {
            var i = new VariableDefinition(ts.Int32); var kb = new VariableDefinition(ts.Int64); var c = new VariableDefinition(ts.Char);
            var any = new VariableDefinition(ts.Boolean);
            foreach (var v in new[] { i, kb, c, any }) parse.Body.Variables.Add(v);
            parse.Body.InitLocals = true;
            var il = parse.Body.GetILProcessor();
            var none = il.Create(OpCodes.Ldc_I4_M1);
            var loop = il.Create(OpCodes.Ldloc, i);
            var done = il.Create(OpCodes.Ldloc, any);
            var notDigit = il.Create(OpCodes.Ldloc, c);
            var next = il.Create(OpCodes.Ldloc, i);
            //   if (s == null) return -1; i = s.IndexOf("MemAvailable:", Ordinal); if (i < 0) return -1; i += 13;
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Brfalse, none);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldstr, "MemAvailable:"); il.Emit(OpCodes.Ldc_I4_4); il.Emit(OpCodes.Callvirt, indexOf);
            il.Emit(OpCodes.Stloc, i);
            il.Emit(OpCodes.Ldloc, i); il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Blt, none);
            il.Emit(OpCodes.Ldloc, i); il.Emit(OpCodes.Ldc_I4, 13); il.Emit(OpCodes.Add); il.Emit(OpCodes.Stloc, i);
            //   while (i < s.Length) { c = s[i]; if (c >= '0' && c <= '9') { kb = kb * 10 + (c - '0'); any = true; }
            //                          else if (c != ' ' || any) break; i++; }
            il.Append(loop); il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Callvirt, length); il.Emit(OpCodes.Bge, done);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldloc, i); il.Emit(OpCodes.Callvirt, chars); il.Emit(OpCodes.Stloc, c);
            il.Emit(OpCodes.Ldloc, c); il.Emit(OpCodes.Ldc_I4, (int)'0'); il.Emit(OpCodes.Blt, notDigit);
            il.Emit(OpCodes.Ldloc, c); il.Emit(OpCodes.Ldc_I4, (int)'9'); il.Emit(OpCodes.Bgt, notDigit);
            il.Emit(OpCodes.Ldloc, kb); il.Emit(OpCodes.Ldc_I8, 10L); il.Emit(OpCodes.Mul);
            il.Emit(OpCodes.Ldloc, c); il.Emit(OpCodes.Ldc_I4, (int)'0'); il.Emit(OpCodes.Sub); il.Emit(OpCodes.Conv_I8); il.Emit(OpCodes.Add);
            il.Emit(OpCodes.Stloc, kb);
            il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Stloc, any);
            il.Emit(OpCodes.Br, next);
            il.Append(notDigit); il.Emit(OpCodes.Ldc_I4, (int)' '); il.Emit(OpCodes.Bne_Un, done);
            il.Emit(OpCodes.Ldloc, any); il.Emit(OpCodes.Brtrue, done);
            il.Append(next); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Add); il.Emit(OpCodes.Stloc, i);
            il.Emit(OpCodes.Br, loop);
            //   return any ? kb : -1;
            il.Append(done); il.Emit(OpCodes.Brfalse, none);
            il.Emit(OpCodes.Ldloc, kb); il.Emit(OpCodes.Ret);
            il.Append(none); il.Emit(OpCodes.Conv_I8); il.Emit(OpCodes.Ret);
            parse.Body.OptimizeMacros();
        }

        // static bool Below(long kb, int mb): kb >= 0 && kb < mb * 1024
        var below = new MethodDefinition("Below", Static, ts.Boolean);
        below.Parameters.Add(new ParameterDefinition("kb", ParameterAttributes.None, ts.Int64));
        below.Parameters.Add(new ParameterDefinition("mb", ParameterAttributes.None, ts.Int32));
        t.Methods.Add(below);
        {
            var il = below.Body.GetILProcessor();
            var no = il.Create(OpCodes.Ldc_I4_0);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldc_I8, 0L); il.Emit(OpCodes.Blt, no);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldarg_1); il.Emit(OpCodes.Conv_I8); il.Emit(OpCodes.Ldc_I8, 1024L); il.Emit(OpCodes.Mul);
            il.Emit(OpCodes.Clt); il.Emit(OpCodes.Ret);
            il.Append(no); il.Emit(OpCodes.Ret);
            below.Body.OptimizeMacros();
        }

        // static bool Low()
        var low = new MethodDefinition("Low", Static, ts.Boolean);
        t.Methods.Add(low);
        {
            var s = new VariableDefinition(ts.String); var n = new VariableDefinition(ts.Int32); var kb = new VariableDefinition(ts.Int64);
            var text = new VariableDefinition(ts.String); var result = new VariableDefinition(ts.Boolean); var parts = new VariableDefinition(new ArrayType(ts.String));
            foreach (var v in new[] { s, n, kb, text, result, parts }) low.Body.Variables.Add(v);
            low.Body.InitLocals = true;
            var il = low.Body.GetILProcessor();
            var no = il.Create(OpCodes.Ldc_I4_0);
            var read = il.Create(OpCodes.Ldsfld, limit);
            var off = il.Create(OpCodes.Ldc_I4_M1);
            var store = il.Create(OpCodes.Stsfld, limit);
            //   if (limit == 0) { s = Environment.GetEnvironmentVariable(V); limit = s != null && int.TryParse(s, out n) && n > 0 ? n : -1; }
            il.Emit(OpCodes.Ldsfld, limit); il.Emit(OpCodes.Brtrue, read);
            il.Emit(OpCodes.Ldstr, Variable); il.Emit(OpCodes.Call, env); il.Emit(OpCodes.Stloc, s);
            il.Emit(OpCodes.Ldloc, s); il.Emit(OpCodes.Brfalse, off);
            il.Emit(OpCodes.Ldloc, s); il.Emit(OpCodes.Ldloca, n); il.Emit(OpCodes.Call, tryParse); il.Emit(OpCodes.Brfalse, off);
            il.Emit(OpCodes.Ldloc, n); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Blt, off);
            il.Emit(OpCodes.Ldloc, n); il.Emit(OpCodes.Br, store);
            il.Append(off); il.Append(store);
            //   if (limit < 0) return false;
            il.Append(read); il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Blt, no);
            //   try { text = File.ReadAllText("/proc/meminfo"); } catch (Exception) { text = null; }
            var tryStart = il.Create(OpCodes.Ldstr, "/proc/meminfo");
            var afterTry = il.Create(OpCodes.Ldloc, text);
            il.Append(tryStart); il.Emit(OpCodes.Call, readAll); il.Emit(OpCodes.Stloc, text);
            var leave1 = il.Create(OpCodes.Leave, afterTry); il.Append(leave1);
            var handlerStart = il.Create(OpCodes.Pop);
            il.Append(handlerStart); il.Emit(OpCodes.Ldnull); il.Emit(OpCodes.Stloc, text);
            var leave2 = il.Create(OpCodes.Leave, afterTry); il.Append(leave2);
            low.Body.ExceptionHandlers.Add(new ExceptionHandler(ExceptionHandlerType.Catch)
            {
                TryStart = tryStart, TryEnd = handlerStart, HandlerStart = handlerStart, HandlerEnd = afterTry, CatchType = Sys("Exception"),
            });
            //   kb = Parse(text); if (!Below(kb, limit)) return false;
            il.Append(afterTry); il.Emit(OpCodes.Call, parse); il.Emit(OpCodes.Stloc, kb);
            il.Emit(OpCodes.Ldloc, kb); il.Emit(OpCodes.Ldsfld, limit); il.Emit(OpCodes.Call, below); il.Emit(OpCodes.Brfalse, no);
            //   Debug.Log("[hk-unload] MemAvailable " + (kb >> 10) + " MB, under " + V + "=" + limit + ": this room change unloads unused assets");
            if (log != null)
            {
                var mb = new VariableDefinition(ts.Int64); low.Body.Variables.Add(mb);
                var lim = new VariableDefinition(ts.Int32); low.Body.Variables.Add(lim);
                il.Emit(OpCodes.Ldloc, kb); il.Emit(OpCodes.Ldc_I4, 10); il.Emit(OpCodes.Shr); il.Emit(OpCodes.Stloc, mb);
                il.Emit(OpCodes.Ldsfld, limit); il.Emit(OpCodes.Stloc, lim);
                il.Emit(OpCodes.Ldc_I4_5); il.Emit(OpCodes.Newarr, ts.String); il.Emit(OpCodes.Stloc, parts);
                void Part(int k, Action push) { il.Emit(OpCodes.Ldloc, parts); il.Emit(OpCodes.Ldc_I4, k); push(); il.Emit(OpCodes.Stelem_Ref); }
                Part(0, () => il.Emit(OpCodes.Ldstr, "[hk-unload] MemAvailable "));
                Part(1, () => { il.Emit(OpCodes.Ldloca, mb); il.Emit(OpCodes.Call, longToString); });
                Part(2, () => il.Emit(OpCodes.Ldstr, " MB, under " + Variable + "="));
                Part(3, () => { il.Emit(OpCodes.Ldloca, lim); il.Emit(OpCodes.Call, intToString); });
                Part(4, () => il.Emit(OpCodes.Ldstr, ": this room change unloads the unused assets"));
                il.Emit(OpCodes.Ldloc, parts); il.Emit(OpCodes.Call, concat); il.Emit(OpCodes.Call, log);
            }
            il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Ret);
            il.Append(no); il.Emit(OpCodes.Ret);
            low.Body.OptimizeMacros();
        }
        return low;
    }
}
