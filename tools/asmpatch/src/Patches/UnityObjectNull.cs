// unity-object-null (optimize, exact, opt-in): UnityEngine.Object's ==, != and implicit bool
// (`if (obj)`) run four or five calls in the engine's managed code for a null test and a pointer test:
// op_Equality -> CompareBaseObjects -> IsNativeObjectAlive -> GetCachedPtr -> IntPtr.op_Inequality.
// Where the engine's methods are exactly that (Unity 2017.4: IsNativeObjectAlive is
// `o.GetCachedPtr() != IntPtr.Zero`), the three operators get bodies that do the same tests inline:
//   op_Equality(a, b): b null -> a null or a.m_CachedPtr == 0; a null -> b.m_CachedPtr == 0;
//                      else a == b (references), as CompareBaseObjects does it
//   op_Inequality: its negation;   op_Implicit(o): o not null and o.m_CachedPtr != 0
// Every result is the engine's, for every argument: the patch checks the engine's methods' bodies
// (calls, fields, locals and branches, in order) and is n/a for any other shape (another Unity
// version, an editor build's module).
//
// Opt-in: it changes an engine module, UnityEngine.CoreModule, and must run on the one the player
// loads (the Android player's own, from its package), never on a desktop copy that android-data
// would then ship instead of the player's. Redirecting the game's call sites to helpers in the game's
// own assemblies would not need that, but Unity 2017.4's Mono refuses a read of the private
// m_CachedPtr from another assembly (FieldAccessException, seen in the emulated rig).
using Mono.Cecil;
using Mono.Cecil.Cil;

namespace AsmPatch;

sealed class UnityObjectNull : Patch
{
    public override string Name => "unity-object-null";
    public override Kind Kind => Kind.Optimize;
    public override bool OptIn => true;
    public override string Summary =>
        "UnityEngine.Object ==, != and bool as inline null and native-pointer tests (the engine's own results; run on the player's engine module)";

    public override Result Run(Game game, bool apply)
    {
        TypeDefinition obj = null; AssemblyDefinition asm = null;
        foreach (var a in game.Assemblies)
        {
            obj = a.MainModule.GetType("UnityEngine.Object");
            if (obj != null) { asm = a; break; }
        }
        if (obj == null) return Result.NotApplicable("no UnityEngine.Object (UnityEngine.CoreModule or UnityEngine.dll)");
        var file = game.FileOf(asm);
        var ops = new[] { ("op_Equality", 2, "Eq"), ("op_Inequality", 2, "Ne"), ("op_Implicit", 1, "Alive") }
            .Select(o => (M: obj.Methods.FirstOrDefault(m => m.Name == o.Item1 && m.Parameters.Count == o.Item2 && m.IsStatic && m.HasBody), Op: o.Item3))
            .ToList();
        if (ops.All(o => o.M != null) && ops.All(o => o.M.Body.Instructions.All(i => i.OpCode.Code != Code.Call && i.OpCode.Code != Code.Callvirt)))
            return new Result { Outcome = Outcome.AlreadyApplied };
        var why = CheckEngine(obj);
        if (why != null) return Result.NotApplicable(why);
        var r = new Result { Outcome = Outcome.Applied };
        r.Details.Add($"{file}: UnityEngine.Object op_Equality, op_Inequality, op_Implicit as inline null and m_CachedPtr tests " +
                      "(no calls to CompareBaseObjects, IsNativeObjectAlive, GetCachedPtr)");
        if (!apply) return r;
        var ptr = obj.Fields.First(f => f.Name == "m_CachedPtr");
        foreach (var (m, op) in ops)
        {
            m.Body.Instructions.Clear(); m.Body.Variables.Clear(); m.Body.ExceptionHandlers.Clear();
            var il = m.Body.GetILProcessor();
            if (op == "Alive") EmitAlive(il, ptr); else EmitCompare(il, ptr, op == "Eq");
        }
        game.MarkChanged(asm);
        return r;
    }

    /// <summary>Null when the engine's methods are the shapes the new bodies reproduce, else why not.</summary>
    static string CheckEngine(TypeDefinition obj)
    {
        var f = obj.Fields.FirstOrDefault(x => x.Name == "m_CachedPtr" && !x.IsStatic && x.FieldType.FullName == "System.IntPtr");
        if (f == null) return "UnityEngine.Object has no IntPtr m_CachedPtr";
        MethodDefinition M(string n, int np) => obj.Methods.FirstOrDefault(m => m.Name == n && m.Parameters.Count == np && m.HasBody);
        var get = obj.Methods.FirstOrDefault(m => m.Name == "GetCachedPtr" && !m.IsStatic && !m.HasParameters && m.HasBody);
        var alive = M("IsNativeObjectAlive", 1);
        var cmp = M("CompareBaseObjects", 2);
        var eq = M("op_Equality", 2); var ne = M("op_Inequality", 2); var imp = M("op_Implicit", 1);
        if (get == null || alive == null || cmp == null || eq == null || ne == null || imp == null)
            return "UnityEngine.Object lacks GetCachedPtr, IsNativeObjectAlive, CompareBaseObjects or one of its operators";
        // Unity 2017.4's bodies as compiled (no optimization: each result through a local; a branch's
        // target is the index of the instruction it goes to, nops left out). CompareBaseObjects: both
        // null -> true; b null -> !IsNativeObjectAlive(a); a null -> !IsNativeObjectAlive(b); else
        // ReferenceEquals(a, b). IsNativeObjectAlive: GetCachedPtr() != IntPtr.Zero.
        var want = new (MethodDefinition M, string Shape)[]
        {
            (get, "a0 ld:m_CachedPtr s0 br>4 l0 ret"),
            (alive, "a0 call:GetCachedPtr ld:Zero call:op_Inequality s0 br>6 l0 ret"),
            (cmp, "a0 null ceq s0 a1 null ceq s1 l1 bf>15 l0 bf>15 1 s2 br>36 l1 bf>23 a0 call:IsNativeObjectAlive 0 ceq s2 br>36 " +
                  "l0 bf>31 a1 call:IsNativeObjectAlive 0 ceq s2 br>36 a0 a1 call:ReferenceEquals s2 br>36 l2 ret"),
            (eq, "a0 a1 call:CompareBaseObjects s0 br>5 l0 ret"),
            (ne, "a0 a1 call:CompareBaseObjects 0 ceq s0 br>7 l0 ret"),
            (imp, "a0 null call:CompareBaseObjects 0 ceq s0 br>7 l0 ret"),
        };
        var bad = want.Where(w => Shape(w.M) != w.Shape).Select(w => $"{w.M.Name}: {Shape(w.M)}").ToList();
        if (bad.Count > 0) return "UnityEngine.Object's methods are not the shapes the new bodies reproduce: " + string.Join("; ", bad);
        if (alive.Body.Instructions.First(i => i.OpCode.Code == Code.Ldsfld).Operand is not FieldReference z ||
            z.DeclaringType.FullName != "System.IntPtr")
            return "IsNativeObjectAlive does not compare with IntPtr.Zero";
        return null;
    }

    /// <summary>What a body does, in order: calls and field loads by name, arguments, locals, null and
    /// small constants, ceq, branches with their targets; nops left out.</summary>
    static string Shape(MethodDefinition m) => string.Join(" ", m.Body.Instructions.Select(i => i.OpCode.Code switch
    {
        Code.Call or Code.Callvirt => "call:" + ((MethodReference)i.Operand).Name,
        Code.Ldfld or Code.Ldsfld or Code.Ldflda => "ld:" + ((FieldReference)i.Operand).Name,
        Code.Ldnull => "null",
        Code.Ceq => "ceq",
        Code.Ldarg_0 => "a0", Code.Ldarg_1 => "a1",
        Code.Ldc_I4_0 => "0", Code.Ldc_I4_1 => "1",
        Code.Stloc_0 => "s0", Code.Stloc_1 => "s1", Code.Stloc_2 => "s2",
        Code.Ldloc_0 => "l0", Code.Ldloc_1 => "l1", Code.Ldloc_2 => "l2",
        Code.Brfalse or Code.Brfalse_S => "bf>" + Index(m, (Instruction)i.Operand),
        Code.Brtrue or Code.Brtrue_S => "bt>" + Index(m, (Instruction)i.Operand),
        Code.Br or Code.Br_S => "br>" + Index(m, (Instruction)i.Operand),
        Code.Nop => null,
        Code.Ret => "ret",
        _ => "?" + i.OpCode.Name,
    }).Where(s => s != null));

    /// <summary>A branch target as the index of the next kept instruction (nops left out).</summary>
    static int Index(MethodDefinition m, Instruction target)
    {
        int k = 0;
        foreach (var i in m.Body.Instructions)
        {
            if (i == target) return k;
            if (i.OpCode.Code != Code.Nop) k++;
        }
        return -1;
    }

    /// <summary>eq = true: CompareBaseObjects(a, b); false: its negation.</summary>
    static void EmitCompare(ILProcessor il, FieldReference ptr, bool eq)
    {
        var yes = il.Create(eq ? OpCodes.Ldc_I4_1 : OpCodes.Ldc_I4_0);
        var no = il.Create(eq ? OpCodes.Ldc_I4_0 : OpCodes.Ldc_I4_1);
        var bNotNull = il.Create(OpCodes.Ldarg_0);
        var aNotNull = il.Create(OpCodes.Ldarg_0);
        // b null: a null -> yes; else a's pointer 0 -> yes, else no
        il.Emit(OpCodes.Ldarg_1); il.Emit(OpCodes.Brtrue, bNotNull);
        il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Brfalse, yes);
        il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, ptr); il.Emit(OpCodes.Brfalse, yes);
        il.Emit(OpCodes.Br, no);
        // b not null, a null: b's pointer 0 -> yes, else no
        il.Append(bNotNull); il.Emit(OpCodes.Brtrue, aNotNull);
        il.Emit(OpCodes.Ldarg_1); il.Emit(OpCodes.Ldfld, ptr); il.Emit(OpCodes.Brfalse, yes);
        il.Emit(OpCodes.Br, no);
        // both not null: the same reference -> yes
        il.Append(aNotNull); il.Emit(OpCodes.Ldarg_1); il.Emit(OpCodes.Beq, yes);
        il.Append(no); il.Emit(OpCodes.Ret);
        il.Append(yes); il.Emit(OpCodes.Ret);
    }

    /// <summary>!CompareBaseObjects(o, null): o not null and its pointer not 0.</summary>
    static void EmitAlive(ILProcessor il, FieldReference ptr)
    {
        var no = il.Create(OpCodes.Ldc_I4_0);
        il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Brfalse, no);
        il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, ptr); il.Emit(OpCodes.Brfalse, no);
        il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Ret);
        il.Append(no); il.Emit(OpCodes.Ret);
    }
}
