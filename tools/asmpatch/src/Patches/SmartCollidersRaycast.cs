// smartcolliders-raycast (kind Optimize, opt-in). CreativeSpore's SmartColliders (an Asset Store library: Blasphemous's
// platform and character collisions) asks Physics2D.RaycastAll for every ray of every collider in
// SmartCollider2D._GetClosestHitInfo2D, and RaycastAll returns a new array each time: Blasphemous on the G350, its first
// level, 1,500 arrays a second, 44 % of all the game's managed garbage (UA_ALLOC_CENSUS), so a full collection (a
// 115 - 240 ms stop) comes sooner. The method only reads the array: its length and its elements, each copied to a
// local at the start of the loop's body. Patched: the call goes to an added helper with RaycastAll's signature:
//   n = Physics2D.RaycastNonAlloc(origin, direction, buf2, distance, layerMask);
//   n == 0: a shared empty array; n == 1: a shared one-element array holding that hit; else RaycastAll as before.
// RaycastNonAlloc runs the same query (the same filter, the same order: by distance) and stores at most the buffer's
// two hits, returning how many it stored, so for 0 and 1 hit the array the method reads holds the same hit RaycastAll
// would have given. The shared one-element array is safe when the loop's body calls back into a collider (its
// SendMessageUpwards for a trigger): the body copied its only element before any call, and its length does not change.
// Two or more hits: the game's own call (the query runs twice). Checked at patch time: one RaycastAll call in the
// method, its result stored in a local that is read only by ldlen and by an element read, never stored elsewhere.
using Mono.Cecil;
using Mono.Cecil.Cil;
using Mono.Cecil.Rocks;

namespace AsmPatch;

sealed class SmartCollidersRaycast : Patch
{
    public override string Name => "smartcolliders-raycast";
    public override Kind Kind => Kind.Optimize;
    public override bool OptIn => true;
    public override string Summary =>
        "SmartColliders' SmartCollider2D._GetClosestHitInfo2D: a ray with 0 or 1 hit reads a shared array (RaycastNonAlloc) instead of a new RaycastAll array (less garbage, the same hits)";

    const string TypeName = "CreativeSpore.SmartColliders.SmartCollider2D", MethodName = "_GetClosestHitInfo2D",
                 HelperType = "<asmpatch>SmartCollidersRaycast";
    const string V2 = "UnityEngine.Vector2", F = "System.Single", I = "System.Int32", HitArr = "UnityEngine.RaycastHit2D[]";

    public override Result Run(Game game, bool apply)
    {
        foreach (var asm in game.GameAssemblies)
        {
            var t = asm.MainModule.GetType(TypeName);
            var md = t?.Methods.FirstOrDefault(x => x.Name == MethodName && x.HasBody);
            if (md == null) continue;
            var file = game.FileOf(asm);
            var m = asm.MainModule;
            if (m.Types.Any(x => x.Name == HelperType))
                return new Result { Outcome = Outcome.AlreadyApplied, Details = { $"{file}: {TypeName}.{MethodName} reads a shared array for 0 or 1 hit" } };
            var calls = md.Body.Instructions.Where(i => i.Operand is MethodReference r && r.Name == "RaycastAll" &&
                                                        r.DeclaringType.FullName == "UnityEngine.Physics2D").ToList();
            if (calls.Count != 1) return Result.NotApplicable($"{file}: {MethodName} has not one Physics2D.RaycastAll call");
            var call = calls[0];
            var cr = (MethodReference)call.Operand;
            if (cr.ReturnType.FullName != HitArr || !cr.Parameters.Select(p => p.ParameterType.FullName).SequenceEqual(new[] { V2, V2, F, I }))
                return Result.NotApplicable($"{file}: the RaycastAll call is not RaycastAll(Vector2, Vector2, float, int)");
            var why = CheckUse(md, call);
            if (why != null) return Result.NotApplicable($"{file}: {why}");
            var nonAlloc = BlasphemousCatalogs.Lib(game, m, "UnityEngine.Physics2D", "RaycastNonAlloc", V2, V2, HitArr, F, I);
            if (nonAlloc == null) return Result.NotApplicable("no Physics2D.RaycastNonAlloc(Vector2, Vector2, RaycastHit2D[], float, int)");
            var r = new Result { Outcome = Outcome.Applied };
            r.Details.Add($"{file}: {TypeName}.{MethodName}: Physics2D.RaycastAll -> RaycastNonAlloc into a shared array for 0 or 1 hit, RaycastAll for more");
            if (!apply) return r;

            var ts = m.TypeSystem;
            var hitT = ((ArrayType)cr.ReturnType).ElementType;
            var arrT = cr.ReturnType;
            var h = new TypeDefinition("", HelperType, TypeAttributes.NotPublic | TypeAttributes.Abstract | TypeAttributes.Sealed |
                                                       TypeAttributes.BeforeFieldInit | TypeAttributes.Class, ts.Object);
            m.Types.Add(h);
            const FieldAttributes SF = FieldAttributes.Private | FieldAttributes.Static;
            var empty = new FieldDefinition("empty", SF, arrT);
            var one = new FieldDefinition("one", SF, arrT);
            var buf = new FieldDefinition("buf", SF, arrT);
            h.Fields.Add(empty); h.Fields.Add(one); h.Fields.Add(buf);
            var cast = new MethodDefinition("Cast", MethodAttributes.Assembly | MethodAttributes.Static | MethodAttributes.HideBySig, arrT);
            foreach (var (n, pt) in new[] { ("origin", cr.Parameters[0].ParameterType), ("direction", cr.Parameters[1].ParameterType),
                                             ("distance", cr.Parameters[2].ParameterType), ("layerMask", cr.Parameters[3].ParameterType) })
                cast.Parameters.Add(new ParameterDefinition(n, ParameterAttributes.None, pt));
            cast.Body.InitLocals = true;
            var vn = new VariableDefinition(ts.Int32);
            cast.Body.Variables.Add(vn);
            h.Methods.Add(cast);
            var il = cast.Body.GetILProcessor();
            // the arrays, once
            var ready = il.Create(OpCodes.Nop);
            il.Emit(OpCodes.Ldsfld, buf); il.Emit(OpCodes.Brtrue, ready);
            il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Newarr, hitT); il.Emit(OpCodes.Stsfld, empty);
            il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Newarr, hitT); il.Emit(OpCodes.Stsfld, one);
            il.Emit(OpCodes.Ldc_I4_2); il.Emit(OpCodes.Newarr, hitT); il.Emit(OpCodes.Stsfld, buf);
            il.Append(ready);
            // n = RaycastNonAlloc(origin, direction, buf, distance, layerMask)
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldarg_1); il.Emit(OpCodes.Ldsfld, buf); il.Emit(OpCodes.Ldarg_2); il.Emit(OpCodes.Ldarg_3);
            il.Emit(OpCodes.Call, nonAlloc); il.Emit(OpCodes.Stloc, vn);
            // ASMPATCH_SMARTCOLLIDERS_RAYCAST=check: the game's own RaycastAll every time (what the game then uses), the
            // two compared for 0 and 1 hit (count; the hit's point, normal, distance, fraction, centroid and collider),
            // the calls and the differences logged every 600 compared calls and at each difference
            var mode = new FieldDefinition("mode", SF, ts.Int32);   // 0 unread, 1 on, 2 check
            var nCalls = new FieldDefinition("calls", SF, ts.Int32);
            var diffs = new FieldDefinition("diffs", SF, ts.Int32);
            h.Fields.Add(mode); h.Fields.Add(nCalls); h.Fields.Add(diffs);
            var env = BlasphemousCatalogs.Lib(game, m, "System.Environment", "GetEnvironmentVariable", "System.String");
            var strEq = BlasphemousCatalogs.Lib(game, m, "System.String", "op_Equality", "System.String", "System.String");
            var objEq = BlasphemousCatalogs.Lib(game, m, "UnityEngine.Object", "op_Equality", "UnityEngine.Object", "UnityEngine.Object");
            var concatO = BlasphemousCatalogs.Lib(game, m, "System.String", "Concat", "System.Object[]");
            var log = BlasphemousCatalogs.Lib(game, m, "UnityEngine.Debug", "Log", "System.Object");
            var hitDef = hitT.Resolve();
            MethodReference Get(string n) => m.ImportReference(hitDef.Methods.First(x => x.Name == n && x.Parameters.Count == 0));
            var v2Def = hitDef.Methods.First(x => x.Name == "get_point").ReturnType.Resolve();
            var fx = m.ImportReference(v2Def.Fields.First(f => f.Name == "x"));
            var fy = m.ImportReference(v2Def.Fields.First(f => f.Name == "y"));
            if (env == null || strEq == null || objEq == null || concatO == null || log == null)
                return Result.Failed("a class library method the check mode calls is missing");
            var vAll = new VariableDefinition(arrT); cast.Body.Variables.Add(vAll);
            var vA = new VariableDefinition(hitT); cast.Body.Variables.Add(vA);
            var vB = new VariableDefinition(hitT); cast.Body.Variables.Add(vB);
            var vBad = new VariableDefinition(ts.Int32); cast.Body.Variables.Add(vBad);
            var modeRead = il.Create(OpCodes.Nop);
            il.Emit(OpCodes.Ldsfld, mode); il.Emit(OpCodes.Brtrue, modeRead);
            il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Stsfld, mode);
            il.Emit(OpCodes.Ldstr, "ASMPATCH_SMARTCOLLIDERS_RAYCAST"); il.Emit(OpCodes.Call, env); il.Emit(OpCodes.Ldstr, "check"); il.Emit(OpCodes.Call, strEq);
            il.Emit(OpCodes.Brfalse, modeRead);
            il.Emit(OpCodes.Ldc_I4_2); il.Emit(OpCodes.Stsfld, mode);
            il.Append(modeRead);
            var notCheck = il.Create(OpCodes.Nop);
            il.Emit(OpCodes.Ldsfld, mode); il.Emit(OpCodes.Ldc_I4_2); il.Emit(OpCodes.Bne_Un, notCheck);
            // all = RaycastAll(...); compare where the patch would have answered (n < 2)
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldarg_1); il.Emit(OpCodes.Ldarg_2); il.Emit(OpCodes.Ldarg_3);
            il.Emit(OpCodes.Call, cr); il.Emit(OpCodes.Stloc, vAll);
            var retAll = il.Create(OpCodes.Ldloc, vAll);
            il.Emit(OpCodes.Ldloc, vn); il.Emit(OpCodes.Ldc_I4_2); il.Emit(OpCodes.Bge, retAll);
            il.Emit(OpCodes.Ldsfld, nCalls); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Add); il.Emit(OpCodes.Stsfld, nCalls);
            il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Stloc, vBad);
            var bad = il.Create(OpCodes.Ldc_I4_1);
            var compared = il.Create(OpCodes.Nop);
            // the count: all.Length == n
            il.Emit(OpCodes.Ldloc, vAll); il.Emit(OpCodes.Ldlen); il.Emit(OpCodes.Conv_I4); il.Emit(OpCodes.Ldloc, vn); il.Emit(OpCodes.Bne_Un, bad);
            il.Emit(OpCodes.Ldloc, vn); il.Emit(OpCodes.Brfalse, compared);
            // the hit: a = all[0], b = buf[0]
            il.Emit(OpCodes.Ldloc, vAll); il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Ldelem_Any, hitT); il.Emit(OpCodes.Stloc, vA);
            il.Emit(OpCodes.Ldsfld, buf); il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Ldelem_Any, hitT); il.Emit(OpCodes.Stloc, vB);
            foreach (var g in new[] { "get_point", "get_normal", "get_centroid" })
                foreach (var f in new[] { fx, fy })
                {
                    il.Emit(OpCodes.Ldloca, vA); il.Emit(OpCodes.Call, Get(g)); il.Emit(OpCodes.Ldfld, f);
                    il.Emit(OpCodes.Ldloca, vB); il.Emit(OpCodes.Call, Get(g)); il.Emit(OpCodes.Ldfld, f);
                    il.Emit(OpCodes.Bne_Un, bad);
                }
            foreach (var g in new[] { "get_distance", "get_fraction" })
            {
                il.Emit(OpCodes.Ldloca, vA); il.Emit(OpCodes.Call, Get(g));
                il.Emit(OpCodes.Ldloca, vB); il.Emit(OpCodes.Call, Get(g));
                il.Emit(OpCodes.Bne_Un, bad);
            }
            il.Emit(OpCodes.Ldloca, vA); il.Emit(OpCodes.Call, Get("get_collider"));
            il.Emit(OpCodes.Ldloca, vB); il.Emit(OpCodes.Call, Get("get_collider"));
            il.Emit(OpCodes.Call, objEq); il.Emit(OpCodes.Brfalse, bad);
            il.Emit(OpCodes.Br, compared);
            il.Append(bad);                                   // a difference
            il.Emit(OpCodes.Stloc, vBad);
            il.Emit(OpCodes.Ldsfld, diffs); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Add); il.Emit(OpCodes.Stsfld, diffs);
            il.Append(compared);
            // log at a difference and every 600 compared calls
            var doLog = il.Create(OpCodes.Ldc_I4_5);
            il.Emit(OpCodes.Ldloc, vBad); il.Emit(OpCodes.Brtrue, doLog);
            il.Emit(OpCodes.Ldsfld, nCalls); il.Emit(OpCodes.Ldc_I4, 600); il.Emit(OpCodes.Rem); il.Emit(OpCodes.Brtrue, retAll);
            il.Append(doLog);                                 // "[smartcolliders-raycast] check: <calls> compared, <diffs> different (n <n>)"
            il.Emit(OpCodes.Newarr, ts.Object);
            void Part(int k, Action push) { il.Emit(OpCodes.Dup); il.Emit(OpCodes.Ldc_I4, k); push(); il.Emit(OpCodes.Stelem_Ref); }
            Part(0, () => il.Emit(OpCodes.Ldstr, "[smartcolliders-raycast] check: "));
            Part(1, () => { il.Emit(OpCodes.Ldsfld, nCalls); il.Emit(OpCodes.Box, ts.Int32); });
            Part(2, () => il.Emit(OpCodes.Ldstr, " compared, "));
            Part(3, () => { il.Emit(OpCodes.Ldsfld, diffs); il.Emit(OpCodes.Box, ts.Int32); });
            Part(4, () => il.Emit(OpCodes.Ldstr, " DIFFERENT"));
            il.Emit(OpCodes.Call, concatO); il.Emit(OpCodes.Call, log);
            il.Append(retAll);                                // return all (the game's own array)
            il.Emit(OpCodes.Ret);
            il.Append(notCheck);
            var notZero = il.Create(OpCodes.Nop);
            il.Emit(OpCodes.Ldloc, vn); il.Emit(OpCodes.Brtrue, notZero);
            il.Emit(OpCodes.Ldsfld, empty); il.Emit(OpCodes.Ret);
            il.Append(notZero);
            var many = il.Create(OpCodes.Nop);
            il.Emit(OpCodes.Ldloc, vn); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Bne_Un, many);
            // one[0] = buf[0]; return one
            il.Emit(OpCodes.Ldsfld, one); il.Emit(OpCodes.Ldc_I4_0);
            il.Emit(OpCodes.Ldsfld, buf); il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Ldelem_Any, hitT);
            il.Emit(OpCodes.Stelem_Any, hitT);
            il.Emit(OpCodes.Ldsfld, one); il.Emit(OpCodes.Ret);
            // two or more: the game's own call
            il.Append(many);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldarg_1); il.Emit(OpCodes.Ldarg_2); il.Emit(OpCodes.Ldarg_3);
            il.Emit(OpCodes.Call, cr); il.Emit(OpCodes.Ret);
            cast.Body.OptimizeMacros();
            call.Operand = cast;
            game.MarkChanged(asm);
            return r;
        }
        return Result.NotApplicable($"no {TypeName}.{MethodName}");
    }

    // the call's result goes into a local; that local is read only as `ldloc a; ldlen` or `ldloc a; <index>; ldelem*/ldelema`
    // (and ldelema's address only copied: ldobj), never stored elsewhere, passed, returned or written again
    static string CheckUse(MethodDefinition md, Instruction call)
    {
        var st = call.Next;
        VariableDefinition v = st?.OpCode.Code switch
        {
            Code.Stloc_0 => md.Body.Variables[0], Code.Stloc_1 => md.Body.Variables[1], Code.Stloc_2 => md.Body.Variables[2],
            Code.Stloc_3 => md.Body.Variables[3], Code.Stloc_S or Code.Stloc => (VariableDefinition)st.Operand, _ => null,
        };
        if (v == null) return "RaycastAll's array is not stored in a local";
        VariableDefinition Var(Instruction i) => i.OpCode.Code switch
        {
            Code.Ldloc_0 => md.Body.Variables[0], Code.Ldloc_1 => md.Body.Variables[1], Code.Ldloc_2 => md.Body.Variables[2],
            Code.Ldloc_3 => md.Body.Variables[3], Code.Ldloc_S or Code.Ldloc or Code.Stloc_S or Code.Stloc or Code.Ldloca_S or Code.Ldloca => i.Operand as VariableDefinition,
            Code.Stloc_0 => md.Body.Variables[0], Code.Stloc_1 => md.Body.Variables[1], Code.Stloc_2 => md.Body.Variables[2],
            Code.Stloc_3 => md.Body.Variables[3], _ => null,
        };
        foreach (var i in md.Body.Instructions)
        {
            if (Var(i) != v) continue;
            var c = i.OpCode.Code;
            if (i == st) continue;
            if (c is Code.Stloc_0 or Code.Stloc_1 or Code.Stloc_2 or Code.Stloc_3 or Code.Stloc_S or Code.Stloc or Code.Ldloca_S or Code.Ldloca)
                return "RaycastAll's array local is written again or its address taken";
            var n = i.Next;
            if (n.OpCode.Code == Code.Ldlen) continue;
            var e = n.Next;
            if (e != null && (e.OpCode.Code is Code.Ldelema or Code.Ldelem_Any) && (n.OpCode.Code is Code.Ldloc_0 or Code.Ldloc_1 or Code.Ldloc_2 or Code.Ldloc_3 or Code.Ldloc_S or Code.Ldloc))
            {
                if (e.OpCode.Code == Code.Ldelema && e.Next?.OpCode.Code != Code.Ldobj) return "an element's address is used other than copied";
                continue;
            }
            return $"RaycastAll's array is used other than read ({n.OpCode})";
        }
        return null;
    }
}
