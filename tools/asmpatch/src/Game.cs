// The game's managed assemblies (every .NET .dll of its Managed folder), loaded once with a resolver
// that hands out these same objects, so a patch that follows a reference into another assembly
// (a base class in a library, an enum's constants) sees, and may change, the one copy that is written.
using Mono.Cecil;

namespace AsmPatch;

sealed class Game : IAssemblyResolver
{
    public readonly string Dir;
    readonly Dictionary<string, string> files = new(StringComparer.OrdinalIgnoreCase);   // simple name -> path
    readonly Dictionary<string, AssemblyDefinition> loaded = new(StringComparer.OrdinalIgnoreCase);
    readonly HashSet<string> bad = new(StringComparer.OrdinalIgnoreCase);                // not .NET images
    readonly HashSet<AssemblyDefinition> changed = new();
    readonly Dictionary<AssemblyDefinition, string[]> refsBefore = new();

    public Game(string dir)
    {
        Dir = dir;
        foreach (var p in Directory.GetFiles(dir, "*.dll").OrderBy(p => p, StringComparer.Ordinal))
            files[Path.GetFileNameWithoutExtension(p)] = p;
    }

    public IEnumerable<string> Names => files.Keys.OrderBy(n => n, StringComparer.Ordinal);

    public AssemblyDefinition Load(string name)
    {
        if (loaded.TryGetValue(name, out var a)) return a;
        if (bad.Contains(name) || !files.TryGetValue(name, out var path)) return null;
        try
        {
            a = AssemblyDefinition.ReadAssembly(path, new ReaderParameters
            {
                AssemblyResolver = this, ReadingMode = ReadingMode.Deferred, ReadSymbols = false, InMemory = true,
            });
        }
        catch (BadImageFormatException) { bad.Add(name); return null; }
        loaded[name] = a;
        refsBefore[a] = a.MainModule.AssemblyReferences.Select(r => r.Name).ToArray();
        return a;
    }

    /// <summary>All of the game's .NET assemblies.</summary>
    public IEnumerable<AssemblyDefinition> Assemblies => Names.Select(Load).Where(a => a != null).ToList();

    /// <summary>Assemblies built from the game's own code and its third-party libraries: not Unity's
    /// engine modules, packages or the class libraries.</summary>
    public IEnumerable<AssemblyDefinition> GameAssemblies => Names.Where(n => !IsPlatformAssembly(n)).Select(Load).Where(a => a != null).ToList();

    public static bool IsPlatformAssembly(string n) =>
        n == "mscorlib" || n == "netstandard" || n.StartsWith("System") || n.StartsWith("Mono.") ||
        n == "UnityEngine" || n.StartsWith("UnityEngine.") || n.StartsWith("Unity.") || n.StartsWith("Microsoft.");

    public void MarkChanged(AssemblyDefinition a) => changed.Add(a);
    public IReadOnlyCollection<AssemblyDefinition> Changed => changed;
    public string FileOf(AssemblyDefinition a) => Path.GetFileName(files[a.Name.Name]);

    /// <summary>References a patch added to an assembly that the game's Mono cannot load: the
    /// patcher's own runtime (System.Private.CoreLib, System.Runtime, netstandard) when it was not
    /// referenced before.</summary>
    public IEnumerable<string> ForeignReferences(AssemblyDefinition a) =>
        a.MainModule.AssemblyReferences.Select(r => r.Name).Except(refsBefore[a])
            .Where(n => n.StartsWith("System.Private") || n == "System.Runtime" || n == "netstandard");

    // IAssemblyResolver: our copies first; anything else (a reference the folder lacks) is unresolved
    public AssemblyDefinition Resolve(AssemblyNameReference name) => Resolve(name, null);
    public AssemblyDefinition Resolve(AssemblyNameReference name, ReaderParameters parameters) =>
        Load(name.Name) ?? throw new AssemblyResolutionException(name);

    public void Dispose()
    {
        foreach (var a in loaded.Values) a.Dispose();
        loaded.Clear();
    }
}
