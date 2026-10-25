// A patch looks for its target in the game's assemblies and rewrites the game's own IL there. It
// applies only where it finds what it expects, never ships code of its own into the game, and says
// what it changed.
using Mono.Cecil;
using Mono.Cecil.Cil;

namespace AsmPatch;

enum Outcome { NotApplicable, AlreadyApplied, Applied, Failed }

sealed class Result
{
    public Outcome Outcome;
    public readonly List<string> Details = new();       // one line per changed (or found) place
    public string Reason;                               // NotApplicable / Failed

    public static Result NotApplicable(string why) => new() { Outcome = Outcome.NotApplicable, Reason = why };
    public static Result Failed(string why) => new() { Outcome = Outcome.Failed, Reason = why };
}

/// <summary>Android: makes the game do on Unity's Android player what its own Android build does.
/// Optimize: the game's code does the same, with less work (README.md, "Optimizations"): a
/// default-on one must leave every value the game can observe as it was. Reduce: a smaller picture
/// for small handhelds (README.md, "Reductions"): never the game play, always opt-in.</summary>
enum Kind { Android, Optimize, Reduce }

abstract class Patch
{
    public abstract string Name { get; }
    public abstract string Summary { get; }
    public virtual Kind Kind => Kind.Android;
    /// <summary>Finds the patch's targets; with apply, changes them (and marks their assemblies
    /// changed). Without apply nothing is modified (--list).</summary>
    public abstract Result Run(Game game, bool apply);
    /// <summary>Runs only when named (--only, --with): a patch that needs the port's data laid out
    /// for it (e.g. streaming-assets: android-data --streaming-assets), or an optimization that is
    /// not behaviour-identical.</summary>
    public virtual bool OptIn => false;

    public static readonly Patch[] All =
    {
        new RewiredAndroid(),
        new InputSystemAndroid(),
        new StreamingAssetsPatch(), new StreamingAssetsAllPatch(), new DataPathPatch(),
        // optimizations
        new PlayMakerLoopCount(), new PlayMakerDelayedEvents(),
        // reductions
        new HollowKnightBlur(), new HollowKnightPost(),
    };
}

static class Cecil
{
    public static IEnumerable<TypeDefinition> AllTypes(ModuleDefinition m)
    {
        foreach (var t in m.Types)
            foreach (var x in WithNested(t)) yield return x;
    }

    static IEnumerable<TypeDefinition> WithNested(TypeDefinition t)
    {
        yield return t;
        foreach (var n in t.NestedTypes)
            foreach (var x in WithNested(n)) yield return x;
    }

    /// <summary>The type and its base types, as far as they resolve.</summary>
    public static IEnumerable<TypeDefinition> Hierarchy(TypeDefinition t)
    {
        for (int guard = 0; t != null && guard < 64; guard++)
        {
            yield return t;
            try { t = t.BaseType?.Resolve(); } catch (AssemblyResolutionException) { t = null; }
        }
    }

    public static TypeDefinition TryResolve(TypeReference r)
    {
        try { return r?.Resolve(); } catch (AssemblyResolutionException) { return null; }
    }

    /// <summary>The value of an enum's constant, by name (never by number: the numbers differ
    /// between versions of a library).</summary>
    public static int? EnumValue(TypeDefinition e, string name)
    {
        if (e == null || !e.IsEnum) return null;
        var f = e.Fields.FirstOrDefault(x => x.IsStatic && x.HasConstant && x.Name == name);
        return f == null ? null : Convert.ToInt32(f.Constant);
    }

    public static string EnumName(TypeDefinition e, int value) =>
        e?.Fields.FirstOrDefault(x => x.IsStatic && x.HasConstant && Convert.ToInt32(x.Constant) == value)?.Name ?? value.ToString();

    /// <summary>The int an instruction pushes, if it is an ldc.i4 form.</summary>
    public static int? ConstInt(Instruction i)
    {
        var c = i.OpCode.Code;
        if (c >= Code.Ldc_I4_M1 && c <= Code.Ldc_I4_8) return c - Code.Ldc_I4_0;
        if (c == Code.Ldc_I4_S) return (sbyte)i.Operand;
        if (c == Code.Ldc_I4) return (int)i.Operand;
        return null;
    }

    /// <summary>The local variable an stloc (store) or ldloc (load) form names, else null.</summary>
    public static int? LocalIndex(Instruction i, bool store)
    {
        var c = i.OpCode.Code;
        if (store)
        {
            if (c >= Code.Stloc_0 && c <= Code.Stloc_3) return c - Code.Stloc_0;
            if (c is Code.Stloc or Code.Stloc_S) return ((VariableDefinition)i.Operand).Index;
        }
        else
        {
            if (c >= Code.Ldloc_0 && c <= Code.Ldloc_3) return c - Code.Ldloc_0;
            if (c is Code.Ldloc or Code.Ldloc_S or Code.Ldloca or Code.Ldloca_S) return ((VariableDefinition)i.Operand).Index;
        }
        return null;
    }

    public static bool SameField(object operand, FieldDefinition f) =>
        operand is FieldReference r && r.Name == f.Name && r.DeclaringType.FullName == f.DeclaringType.FullName;
}
