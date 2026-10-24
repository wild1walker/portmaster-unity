// asmpatch: generic patches for a Unity game's managed assemblies, for Unity's Android player
// (portmaster-unity). Rewrites the game's own IL; nothing is added from elsewhere.
//
//   asmpatch [--only=a,b] [--skip=a,b] [--with=c] [--no-optimize] <Managed in> <out dir>
//       scans every assembly of <Managed in>, runs each patch that applies and writes only the
//       assemblies it changed into <out dir> (the originals are not touched); opt-in patches run
//       only when named by --only or --with; --no-optimize leaves out the optimizations (patches
//       of kind Optimize, README.md "Optimizations") that are not named
//   asmpatch --list [<Managed in>]      the patches; with a folder, what each would change there
//   asmpatch --survey <Managed in>      the game's input stacks and what they need
//
// Output: one line per patch, "ok   <name>: <what>", "n/a  <name>: <why>", "done <name>: <already>",
// "skip <name>", "FAIL <name>: <why>"; then "wrote <file>" per assembly written.
// Exit status: 0, 1 if a patch failed (nothing is written then), 2 for usage errors.
using Mono.Cecil;

namespace AsmPatch;

static class Program
{
    static int Main(string[] argv)
    {
        bool list = argv.Contains("--list"), survey = argv.Contains("--survey"), noOptimize = argv.Contains("--no-optimize");
        string[] only = Opt(argv, "--only="), skip = Opt(argv, "--skip=") ?? Array.Empty<string>(), with = Opt(argv, "--with=") ?? Array.Empty<string>();
        var bad = argv.Where(a => a.StartsWith("-") && a != "--list" && a != "--survey" && a != "--no-optimize" &&
                                  !a.StartsWith("--only=") && !a.StartsWith("--skip=") && !a.StartsWith("--with=")).ToList();
        var args = argv.Where(a => !a.StartsWith("-")).ToArray();
        var unknown = (only ?? Array.Empty<string>()).Concat(skip).Concat(with).Where(n => Patch.All.All(p => p.Name != n)).ToList();
        if (bad.Count > 0 || unknown.Count > 0 || (survey ? args.Length != 1 : list ? args.Length > 1 : args.Length != 2))
        {
            foreach (var b in bad) Console.Error.WriteLine($"asmpatch: unknown option {b}");
            foreach (var u in unknown) Console.Error.WriteLine($"asmpatch: no patch named {u}");
            Console.Error.WriteLine("usage: asmpatch [--only=a,b] [--skip=a,b] [--with=c] [--no-optimize] <Managed in> <out dir>\n" +
                                   "       asmpatch --list [<Managed in>]\n       asmpatch --survey <Managed in>");
            return 2;
        }
        if (list && args.Length == 0)
        {
            foreach (var (kind, title) in new[] { (Kind.Android, "android: what the game's Android build does"),
                                                   (Kind.Optimize, "optimize: the same behaviour with less work"),
                                                   (Kind.Reduce, "reduce: a smaller picture for small handhelds, never other game play (opt-in)") })
            {
                Console.WriteLine(title);
                foreach (var p in Patch.All.Where(p => p.Kind == kind))
                    Console.WriteLine($"  {p.Name,-24} {p.Summary}");
            }
            return 0;
        }
        if (!Directory.Exists(args[0])) { Console.Error.WriteLine($"asmpatch: {args[0]}: no such folder"); return 2; }
        var game = new Game(args[0]);
        try
        {
            if (survey) return Survey.Run(game);
            string outDir = list ? null : args[1];
            if (outDir != null && Path.GetFullPath(outDir).TrimEnd('/', '\\') == Path.GetFullPath(args[0]).TrimEnd('/', '\\'))
            { Console.Error.WriteLine("asmpatch: the out dir must not be the Managed folder"); return 2; }
            return Run(game, outDir, only, skip, with, noOptimize);
        }
        finally { game.Dispose(); }
    }

    // (an option given more than once adds to the list: android-data appends --with=)
    static string[] Opt(string[] argv, string prefix) =>
        argv.Any(a => a.StartsWith(prefix))
            ? argv.Where(a => a.StartsWith(prefix)).SelectMany(a => a.Substring(prefix.Length).Split(',', StringSplitOptions.RemoveEmptyEntries)).ToArray()
            : null;

    /// <summary>Runs the patches (outDir null: report only). Returns the exit status.</summary>
    public static int Run(Game game, string outDir, string[] only, string[] skip, string[] with = null, bool noOptimize = false)
    {
        bool apply = outDir != null;
        int failures = 0;
        with ??= Array.Empty<string>();
        foreach (var p in Patch.All)
        {
            bool named = only != null && only.Contains(p.Name) || with.Contains(p.Name);
            if ((only != null && !named) || skip.Contains(p.Name)) { Console.WriteLine($"skip {p.Name}"); continue; }
            if (noOptimize && p.Kind == Kind.Optimize && !named) { Console.WriteLine($"skip {p.Name} (--no-optimize)"); continue; }
            // an opt-in patch: applied only when named; --list still says what it would do
            if (p.OptIn && !named && apply) { Console.WriteLine($"skip {p.Name} (opt-in: --with={p.Name})"); continue; }
            Result r;
            try { r = p.Run(game, apply); }
            catch (Exception e) when (e is not OutOfMemoryException) { r = Result.Failed($"{e.GetType().Name}: {e.Message}"); }
            switch (r.Outcome)
            {
                case Outcome.NotApplicable: Console.WriteLine($"n/a  {p.Name}: {r.Reason}"); break;
                case Outcome.Failed: failures++; Console.WriteLine($"FAIL {p.Name}: {r.Reason}"); break;
                case Outcome.AlreadyApplied: foreach (var d in r.Details) Console.WriteLine($"done {p.Name}: {d}"); break;
                case Outcome.Applied:
                    foreach (var d in r.Details) Console.WriteLine($"{(apply ? "ok  " : "would")} {p.Name}: {d}");
                    break;
            }
        }
        if (!apply) return failures == 0 ? 0 : 1;
        foreach (var a in game.Changed)
        {
            var foreign = game.ForeignReferences(a).ToList();
            if (foreign.Count > 0) { failures++; Console.WriteLine($"FAIL {game.FileOf(a)}: references the patcher's runtime ({string.Join(", ", foreign)})"); }
        }
        if (failures > 0) { Console.WriteLine("nothing written"); return 1; }
        Directory.CreateDirectory(outDir);
        foreach (var a in game.Changed.OrderBy(a => a.Name.Name, StringComparer.Ordinal))
        {
            var f = game.FileOf(a);
            a.Write(Path.Combine(outDir, f));
            Console.WriteLine($"wrote {f}");
        }
        return 0;
    }
}
