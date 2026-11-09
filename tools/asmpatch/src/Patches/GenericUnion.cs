// mono5-generic-union (android, exact, opt-in): a generic struct with an explicit layout whose fields
// all lie at offset 0 (a union: an enum's value read as an int and back without boxing, e.g.
// `[StructLayout(LayoutKind.Explicit)] struct Converter<T> { [FieldOffset(0)] T Raw; [FieldOffset(0)]
// int AsInt; ... }`). Unity's own old Mono runs it; Mono 5 and later (the mono-arm64 route's runtime,
// Unity 2017.4's MonoBleedingEdge) and .NET refuse to load the type ("Generic class cannot have
// explicit layout"), so every method that uses one throws a TypeLoadException, and a static
// constructor that calls one leaves its whole class unusable.
// Where the union only lives in locals (no field, parameter, return value, array or method of its
// type), each such local becomes a zeroed 8-byte one and its uses keep their order and stack effect:
//   initobj Union<X>       -> ldc.i8 0; stind.i8      (the 8 bytes zeroed, as initobj did)
//   stfld Union<X>::F      -> stobj <F's type>        (the field at offset 0 = the local's address)
//   ldfld Union<X>::F      -> ldobj <F's type>
//   ldflda Union<X>::F     -> (nothing: the same address)
//   ldloc V; ldfld Union<X>::F (a field read from the value, as C# compiles `return u.AsInt`)
//                          -> ldloca V; ldobj <F's type>
// so every value is the union's own: the same bytes written, the same bytes read. The type itself
// gets a sequential layout (no method uses it any more; the class loader then accepts it if
// anything still names it, e.g. reflection over the assembly).
// Opt-in: on the player's own Mono (the android route) the game's code runs as it is, and faster.
using Mono.Cecil;
using Mono.Cecil.Cil;

namespace AsmPatch;

sealed class GenericUnion : Patch
{
    public override string Name => "mono5-generic-union";
    public override bool OptIn => true;
    public override string Summary =>
        "generic structs with an explicit layout used as unions (Mono 5+ refuses them): their locals as 8 zeroed bytes, the same values";

    const int MaxSize = 8;

    public override Result Run(Game game, bool apply)
    {
        var r = new Result { Outcome = Outcome.Applied };
        var none = new List<string>();
        foreach (var asm in game.GameAssemblies)
        {
            var m = asm.MainModule; var file = game.FileOf(asm);
            var unions = Cecil.AllTypes(m).Where(IsUnionShape).ToList();
            foreach (var u in unions)
            {
                var why = OnlyInLocals(m, u);
                if (why != null) { none.Add($"{file}: {u.FullName}: {why}"); continue; }
                var uses = Methods(m).Where(x => x.Body.Variables.Any(v => Is(v.VariableType, u))).ToList();
                var bad = uses.Select(x => (x, Why: CheckMethod(x, u))).Where(t => t.Why != null).ToList();
                if (bad.Count > 0) { none.Add($"{file}: {u.FullName}: {bad[0].x.FullName}: {bad[0].Why}"); continue; }
                r.Details.Add($"{file}: {u.FullName} ({u.Fields.Count(f => !f.IsStatic)} fields at offset 0): {uses.Count} methods' locals as 8 zeroed bytes, the type sequential");
                if (!apply) continue;
                foreach (var x in uses) Rewrite(m, x, u);
                u.IsExplicitLayout = false; u.IsSequentialLayout = true;
                foreach (var f in u.Fields) f.Offset = -1;
                u.PackingSize = -1; u.ClassSize = -1;
                game.MarkChanged(asm);
            }
        }
        if (r.Details.Count == 0)
            return none.Count > 0 ? Result.NotApplicable(string.Join("; ", none)) : Result.NotApplicable("no generic struct with an explicit layout");
        r.Details.AddRange(none.Select(n => "left as it is: " + n));
        return r;
    }

    /// <summary>A generic value type, explicit layout, every instance field at offset 0 and of a
    /// type of at most 8 bytes (a primitive, or one of its own generic parameters), no methods.</summary>
    static bool IsUnionShape(TypeDefinition t)
    {
        if (!t.IsValueType || t.IsEnum || !t.HasGenericParameters || !t.IsExplicitLayout || t.HasMethods) return false;
        var fs = t.Fields.Where(f => !f.IsStatic).ToList();
        return fs.Count > 1 && fs.All(f => f.Offset == 0 && (f.FieldType is GenericParameter gp && gp.Owner == t || PrimitiveSize(f.FieldType) is > 0 and <= MaxSize));
    }

    static int PrimitiveSize(TypeReference t) => t.MetadataType switch
    {
        MetadataType.Boolean or MetadataType.SByte or MetadataType.Byte => 1,
        MetadataType.Char or MetadataType.Int16 or MetadataType.UInt16 => 2,
        MetadataType.Int32 or MetadataType.UInt32 or MetadataType.Single => 4,
        MetadataType.Int64 or MetadataType.UInt64 or MetadataType.Double => 8,
        _ => 0,
    };

    static bool Is(TypeReference t, TypeDefinition u) =>
        t != null && (t is GenericInstanceType g ? g.ElementType : t).FullName == u.FullName && (t.Scope?.Name == u.Module.Name || t.Module == u.Module);

    static IEnumerable<MethodDefinition> Methods(ModuleDefinition m) =>
        Cecil.AllTypes(m).SelectMany(t => t.Methods).Where(x => x.HasBody);

    static bool Mentions(TypeReference t, TypeDefinition u) => t switch
    {
        null => false,
        GenericInstanceType g => Is(g, u) || g.GenericArguments.Any(a => Mentions(a, u)),
        TypeSpecification s => Mentions(s.ElementType, u),
        _ => Is(t, u),
    };

    /// <summary>Null when the union appears only as the type of method locals, else where else.</summary>
    static string OnlyInLocals(ModuleDefinition m, TypeDefinition u)
    {
        foreach (var t in Cecil.AllTypes(m))
        {
            if (t != u && t.Fields.Any(f => Mentions(f.FieldType, u))) return $"a field of {t.FullName} has its type";
            foreach (var x in t.Methods)
            {
                if (Mentions(x.ReturnType, u) || x.Parameters.Any(p => Mentions(p.ParameterType, u)))
                    return $"{x.FullName} takes or returns it";
                if (!x.HasBody) continue;
                foreach (var i in x.Body.Instructions)
                {
                    if (i.Operand is TypeReference tr && Mentions(tr, u) && i.OpCode.Code != Code.Initobj)
                        return $"{x.FullName}: {i.OpCode.Name} on it";
                    if (i.Operand is MethodReference mr && (Mentions(mr.DeclaringType, u) || Mentions(mr.ReturnType, u) || mr.Parameters.Any(p => Mentions(p.ParameterType, u))))
                        return $"{x.FullName} calls {mr.Name} with it";
                    if (i.Operand is FieldReference fr && Mentions(fr.DeclaringType, u) &&
                        !x.Body.Variables.Any(v => Is(v.VariableType, u)))
                        return $"{x.FullName}: its fields used without a local of its type";
                }
            }
        }
        return null;
    }

    /// <summary>The union local an instruction loads (ldloc), stores (stloc) or takes the address of
    /// (ldloca), with which of the three, else null.</summary>
    static (VariableDefinition V, char Use)? LocalUse(MethodDefinition x, Instruction i, HashSet<VariableDefinition> locals)
    {
        var c = i.OpCode.Code;
        VariableDefinition v = c switch
        {
            >= Code.Ldloc_0 and <= Code.Ldloc_3 => x.Body.Variables[c - Code.Ldloc_0],
            >= Code.Stloc_0 and <= Code.Stloc_3 => x.Body.Variables[c - Code.Stloc_0],
            _ => i.Operand as VariableDefinition,
        };
        if (v == null || !locals.Contains(v)) return null;
        char use = c is Code.Ldloca or Code.Ldloca_S ? 'a' : c is (>= Code.Ldloc_0 and <= Code.Ldloc_3) or Code.Ldloc or Code.Ldloc_S ? 'l' : 's';
        return (v, use);
    }

    static bool IsUnionField(Instruction i, TypeDefinition u, Code code) =>
        i != null && i.OpCode.Code == code && i.Operand is FieldReference f && Mentions(f.DeclaringType, u);

    /// <summary>Null when every use of the method's union locals is by address (ldloca), or a load
    /// straight into a field read, and every field access and initobj is one this patch rewrites.</summary>
    static string CheckMethod(MethodDefinition x, TypeDefinition u)
    {
        var locals = x.Body.Variables.Where(v => Is(v.VariableType, u)).ToHashSet();
        foreach (var i in x.Body.Instructions)
        {
            var use = LocalUse(x, i, locals);
            if (use?.Use == 's' || use?.Use == 'l' && !IsUnionField(i.Next, u, Code.Ldfld))
                return $"{i.OpCode.Name} of a union local (only its address, or a field read from it, is handled)";
            if (i.Operand is FieldReference fr && Mentions(fr.DeclaringType, u) && i.OpCode.Code is not (Code.Ldfld or Code.Stfld or Code.Ldflda))
                return $"{i.OpCode.Name} of its field {fr.Name}";
        }
        return null;
    }

    /// <summary>The type a union field holds in this use: its own generic parameter as the union's
    /// argument there, else the primitive.</summary>
    static TypeReference FieldType(FieldReference fr)
    {
        if (fr.FieldType is GenericParameter gp && fr.DeclaringType is GenericInstanceType g && gp.Position < g.GenericArguments.Count)
            return g.GenericArguments[gp.Position];
        return fr.FieldType;
    }

    static void Rewrite(ModuleDefinition m, MethodDefinition x, TypeDefinition u)
    {
        var locals = x.Body.Variables.Where(v => Is(v.VariableType, u)).ToHashSet();
        var il = x.Body.GetILProcessor();
        foreach (var i in x.Body.Instructions.ToList())
        {
            if (LocalUse(x, i, locals) is { Use: 'l' } lu)      // (CheckMethod: a field read follows)
            {
                i.OpCode = lu.V.Index < 256 ? OpCodes.Ldloca_S : OpCodes.Ldloca; i.Operand = lu.V;
                continue;
            }
            switch (i.OpCode.Code)
            {
                case Code.Initobj when i.Operand is TypeReference t && Mentions(t, u):
                    i.OpCode = OpCodes.Ldc_I8; i.Operand = 0L;          // (in place: branches to it still land here)
                    il.InsertAfter(i, Instruction.Create(OpCodes.Stind_I8));
                    break;
                case Code.Stfld when i.Operand is FieldReference f && Mentions(f.DeclaringType, u):
                    i.OpCode = OpCodes.Stobj; i.Operand = FieldType(f);
                    break;
                case Code.Ldfld when i.Operand is FieldReference f && Mentions(f.DeclaringType, u):
                    i.OpCode = OpCodes.Ldobj; i.Operand = FieldType(f);
                    break;
                case Code.Ldflda when i.Operand is FieldReference f && Mentions(f.DeclaringType, u):
                    i.OpCode = OpCodes.Nop; i.Operand = null;
                    break;
            }
        }
        foreach (var v in locals) v.VariableType = m.TypeSystem.UInt64;
    }
}
