// playmaker-typelookup. HutongGames.PlayMaker.ReflectionUtils.GetGlobalType(string) finds a type by
// its name for the actions that name one (EnableBehaviour, GetComponent, ...). A short name such as
// "Animator" fails every Type.GetType it tries first and falls to the last resort: every loaded
// assembly's GetTypes(), looking for a type of that Name in UnityEngine, HutongGames.PlayMaker or
// HutongGames.PlayMaker.Actions. On Mono that makes the runtime load every class of every assembly:
// seconds on a 1 GB handheld's cores, in the middle of a level load (one game: a third of a 4.8 s
// frame on an RK3326). The patch hands that loop, assembly by assembly, only what a direct lookup
// finds there (Assembly.GetType of the three full names) instead of every type, and PlayMaker's own
// checks then run on that. The same type comes out wherever a name can mean at most one type per
// assembly: the names that are nested types in those namespaces, or two of their types in one
// assembly, are found here, at patch time, and keep the whole list, as do names Type.GetType would
// parse (+ , . [ ] & * \ ` and spaces) and assemblies the patcher did not see.
using Mono.Cecil;
using Mono.Cecil.Cil;
using Mono.Cecil.Rocks;

namespace AsmPatch;

sealed class PlayMakerTypeLookup : Patch
{
    const string UtilsType = "HutongGames.PlayMaker.ReflectionUtils", HelperType = "<asmpatch>PlayMakerTypes", HelperMethod = "Types";
    static readonly string[] Namespaces = { "UnityEngine", "HutongGames.PlayMaker", "HutongGames.PlayMaker.Actions" };
    const string Special = "+,.[]&*\\` \t";

    public override string Name => "playmaker-typelookup";
    public override Kind Kind => Kind.Optimize;
    public override string Summary =>
        "PlayMaker's ReflectionUtils.GetGlobalType looks a short type name up directly instead of listing every type of every assembly (the same type comes out)";

    public override Result Run(Game game, bool apply)
    {
        TypeDefinition utils = null;
        AssemblyDefinition asm = null;
        foreach (var a in game.GameAssemblies)
            if ((utils = a.MainModule.GetType(UtilsType)) != null) { asm = a; break; }
        if (utils == null) return Result.NotApplicable("no PlayMaker (HutongGames.PlayMaker.ReflectionUtils)");
        var file = game.FileOf(asm);
        var m = utils.Methods.FirstOrDefault(x => x.Name == "GetGlobalType" && x.IsStatic && x.HasBody && x.Parameters.Count == 1 &&
                                                  x.Parameters[0].ParameterType.MetadataType == MetadataType.String &&
                                                  x.ReturnType.FullName == "System.Type");
        if (m == null) return Result.NotApplicable($"{file}: no ReflectionUtils.GetGlobalType(string)");
        var where = $"{file}: {UtilsType}.GetGlobalType";
        var ins = m.Body.Instructions;
        if (ins.Any(i => i.Operand is MethodReference r && r.Name == HelperMethod && r.DeclaringType.Name == HelperType))
            return new Result { Outcome = Outcome.AlreadyApplied, Details = { $"{where}: looks short names up directly" } };

        // the method this patch knows: one Assembly.GetTypes(), compared by Name, filtered by the three namespaces
        var calls = ins.Where(i => i.OpCode.Code is Code.Call or Code.Callvirt && i.Operand is MethodReference r && r.Name == "GetTypes" &&
                                   r.Parameters.Count == 0 && r.DeclaringType.FullName == "System.Reflection.Assembly").ToList();
        var strings = ins.Where(i => i.OpCode.Code == Code.Ldstr).Select(i => (string)i.Operand).ToHashSet();
        bool names = ins.Any(i => i.Operand is MethodReference r && r.Name == "get_Name" && r.DeclaringType.FullName is "System.Type" or "System.Reflection.MemberInfo");
        bool spaces = ins.Any(i => i.Operand is MethodReference r && r.Name == "get_Namespace" && r.DeclaringType.FullName == "System.Type");
        if (calls.Count != 1 || !Namespaces.All(strings.Contains) || !names || !spaces)
            return Result.NotApplicable($"{where}: not the method this patch knows");
        var call = calls[0];
        if (PlayMakerLib.Targeted(m.Body, call)) return Result.NotApplicable($"{where}: a branch lands on its GetTypes()");

        // patch time: the assemblies, and the names that can mean more than one type in one of them or a nested type
        var known = new List<string>();
        var excluded = new SortedSet<string>(StringComparer.Ordinal);
        foreach (var a in game.Assemblies)
        {
            known.Add(a.Name.FullName);
            var seen = new HashSet<string>(StringComparer.Ordinal);
            foreach (var t in Cecil.AllTypes(a.MainModule))
            {
                var outer = t;
                while (outer.DeclaringType != null) outer = outer.DeclaringType;
                if (!Namespaces.Contains(outer.Namespace)) continue;
                if (t.DeclaringType != null || !seen.Add(t.Name)) excluded.Add(t.Name);
            }
        }
        if (excluded.Any(n => n.Contains('\n'))) return Result.NotApplicable($"{where}: a type name with a line break");
        var r = new Result { Outcome = Outcome.Applied };
        r.Details.Add($"{where}: looks short names up directly in {known.Count} assemblies; {excluded.Count} names (nested types, or two in one assembly) keep the full list");
        if (!apply) return r;

        var module = asm.MainModule;
        var getTypes = (MethodReference)call.Operand;
        var helper = Helper(module, getTypes, "\n" + string.Join("\n", known) + "\n", "\n" + string.Join("\n", excluded) + "\n");
        // ...GetTypes() -> <asmpatch>PlayMakerTypes.Types(<the assembly>, typeName)
        m.Body.SimplifyMacros();
        var il = m.Body.GetILProcessor();
        il.InsertBefore(call, il.Create(OpCodes.Ldarg_0));
        call.OpCode = OpCodes.Call;
        call.Operand = helper;
        m.Body.OptimizeMacros();
        game.MarkChanged(asm);
        return r;
    }

    /// <summary>static Type[] Types(Assembly a, string name): the type a direct lookup finds in a under one of the
    /// three namespaces (or none), where that is all a.GetTypes() could give PlayMaker's loop; else a.GetTypes().</summary>
    static MethodDefinition Helper(ModuleDefinition m, MethodReference getTypes, string known, string excluded)
    {
        var ts = m.TypeSystem;
        var asmType = getTypes.DeclaringType;
        var typeType = ((ArrayType)getTypes.ReturnType).ElementType;
        var t = new TypeDefinition("", HelperType, TypeAttributes.NotPublic | TypeAttributes.Abstract | TypeAttributes.Sealed |
                                                   TypeAttributes.BeforeFieldInit | TypeAttributes.Class, ts.Object);
        m.Types.Add(t);
        var md = new MethodDefinition(HelperMethod, MethodAttributes.Assembly | MethodAttributes.Static | MethodAttributes.HideBySig,
                                      m.ImportReference(getTypes.ReturnType));
        md.Parameters.Add(new ParameterDefinition("a", ParameterAttributes.None, m.ImportReference(asmType)));
        md.Parameters.Add(new ParameterDefinition("name", ParameterAttributes.None, ts.String));
        t.Methods.Add(md);
        md.Body.InitLocals = true;
        md.Body.Variables.Add(new VariableDefinition(m.ImportReference(typeType)));

        MethodReference Str(string name, TypeReference ret, bool inst, params TypeReference[] ps)
        {
            var x = new MethodReference(name, ret, ts.String) { HasThis = inst };
            foreach (var p in ps) x.Parameters.Add(new ParameterDefinition(p));
            return x;
        }
        var charArray = new ArrayType(ts.Char);
        var toCharArray = Str("ToCharArray", charArray, true);
        var indexOfAny = Str("IndexOfAny", ts.Int32, true, charArray);
        var comparison = new TypeReference("System", "StringComparison", m, ts.CoreLibrary, true);
        var indexOf = Str("IndexOf", ts.Int32, true, ts.String, comparison);
        var concat2 = Str("Concat", ts.String, false, ts.String, ts.String);
        var concat3 = Str("Concat", ts.String, false, ts.String, ts.String, ts.String);
        var fullName = new MethodReference("get_FullName", ts.String, m.ImportReference(asmType)) { HasThis = true };
        var getType = new MethodReference("GetType", m.ImportReference(typeType), m.ImportReference(asmType)) { HasThis = true };
        getType.Parameters.Add(new ParameterDefinition(ts.String));

        var il = md.Body.GetILProcessor();
        var full = il.Create(OpCodes.Ldarg_0);
        var found = il.Create(OpCodes.Ldc_I4_1);
        void Emit(OpCode op, object operand = null)
        {
            il.Append(operand switch
            {
                null => il.Create(op), string s => il.Create(op, s), Instruction i => il.Create(op, i),
                MethodReference mr => il.Create(op, mr), TypeReference tr => il.Create(op, tr), int n => il.Create(op, n),
                _ => throw new ArgumentException(operand.GetType().Name),
            });
        }
        // a name Type.GetType would parse, an assembly not seen here, a name that may mean another type: the whole list
        Emit(OpCodes.Ldarg_1); Emit(OpCodes.Brfalse, full);
        Emit(OpCodes.Ldarg_1); Emit(OpCodes.Ldstr, Special); Emit(OpCodes.Callvirt, toCharArray); Emit(OpCodes.Callvirt, indexOfAny);
        Emit(OpCodes.Ldc_I4_0); Emit(OpCodes.Bge, full);
        Emit(OpCodes.Ldstr, known); Emit(OpCodes.Ldstr, "\n"); Emit(OpCodes.Ldarg_0); Emit(OpCodes.Callvirt, fullName); Emit(OpCodes.Ldstr, "\n");
        Emit(OpCodes.Call, concat3); Emit(OpCodes.Ldc_I4, 4 /* Ordinal */); Emit(OpCodes.Callvirt, indexOf); Emit(OpCodes.Ldc_I4_0); Emit(OpCodes.Blt, full);
        Emit(OpCodes.Ldstr, excluded); Emit(OpCodes.Ldstr, "\n"); Emit(OpCodes.Ldarg_1); Emit(OpCodes.Ldstr, "\n");
        Emit(OpCodes.Call, concat3); Emit(OpCodes.Ldc_I4, 4); Emit(OpCodes.Callvirt, indexOf); Emit(OpCodes.Ldc_I4_0); Emit(OpCodes.Bge, full);
        // the three full names, in PlayMaker's order
        foreach (var ns in Namespaces)
        {
            Emit(OpCodes.Ldarg_0); Emit(OpCodes.Ldstr, ns + "."); Emit(OpCodes.Ldarg_1); Emit(OpCodes.Call, concat2);
            Emit(OpCodes.Callvirt, getType); Emit(OpCodes.Stloc_0); Emit(OpCodes.Ldloc_0); Emit(OpCodes.Brtrue, found);
        }
        Emit(OpCodes.Ldc_I4_0); Emit(OpCodes.Newarr, m.ImportReference(typeType)); Emit(OpCodes.Ret);
        il.Append(found);
        Emit(OpCodes.Newarr, m.ImportReference(typeType)); Emit(OpCodes.Dup); Emit(OpCodes.Ldc_I4_0); Emit(OpCodes.Ldloc_0);
        Emit(OpCodes.Stelem_Ref); Emit(OpCodes.Ret);
        il.Append(full);
        Emit(OpCodes.Callvirt, getTypes); Emit(OpCodes.Ret);
        md.Body.OptimizeMacros();
        return md;
    }
}
