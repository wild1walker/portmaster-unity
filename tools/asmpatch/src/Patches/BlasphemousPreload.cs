// blasphemous-preload (kind Android, where it applies): Blasphemous's LevelManager.AllPreInitialized loads the
// scene "Preload_" + GetPlatformName(), and GetPlatformName maps Application.platform to Windows, Linux or OSX,
// any other platform to its own name. The builds hold Preload_Windows, Preload_Linux and Preload_OSX only, so on
// Unity's Android player the game asks for Preload_Android, LevelScene.Load finds no such scene and throws a
// NullReferenceException: no level ever loads. The port's data is a desktop build's, and its own preload scene is
// the one its platform asks for; the patch makes GetPlatformName read that platform where it reads
// Application.platform and finds Android:
//   call Application::get_platform()  ->  call Application::get_platform(); dup; ldc.i4 Android; bne.un L; pop; ldc.i4 <data's>; L: nop
// The data's platform is the build target in the header of globalgamemanagers beside Managed (the player's own
// files): StandaloneLinux64 -> LinuxPlayer, StandaloneOSX -> OSXPlayer, StandaloneWindows64 -> WindowsPlayer.
// Only GetPlatformName changes: the game's other reads of Application.platform (README.md) see Android.
using Mono.Cecil;
using Mono.Cecil.Cil;
using Mono.Cecil.Rocks;

namespace AsmPatch;

sealed class BlasphemousPreload : Patch
{
    public override string Name => "blasphemous-preload";
    public override string Summary =>
        "Blasphemous's LevelManager.GetPlatformName names the data's own platform (its preload scene, e.g. Preload_Linux) on the Android player, which has no Preload_Android";

    const string ManagerType = "Framework.Managers.LevelManager", Method = "GetPlatformName";
    // GetPlatformName as the game (Blasphemous 4.0, Unity 2017.4.40f1) has it: the hash of its IL
    // (BlasphemousCatalogs.Shape); another build's is not assumed to do the same
    public const string KnownHash = "0b4caafd11559e86";

    public override Result Run(Game game, bool apply)
    {
        var asm = game.Load("Assembly-CSharp");
        var mgr = asm?.MainModule.GetType(ManagerType);
        var md = mgr?.Methods.FirstOrDefault(x => x.Name == Method && !x.IsStatic && x.HasBody && x.Parameters.Count == 0 &&
                                                  x.ReturnType.MetadataType == MetadataType.String);
        if (md == null) return Result.NotApplicable($"no Blasphemous ({ManagerType}.{Method}() in Assembly-CSharp)");
        var file = game.FileOf(asm);
        var where = $"{file}: {ManagerType}.{Method}";
        var plat = game.Assemblies.Select(a => a.MainModule.GetType("UnityEngine.RuntimePlatform")).FirstOrDefault(t => t != null);
        int? android = Cecil.EnumValue(plat, "Android");
        if (android == null) return Result.Failed($"{where}: no UnityEngine.RuntimePlatform.Android");
        var reads = md.Body.Instructions.Where(IsGetPlatform).ToList();
        if (reads.Count == 0) return Result.Failed($"{where}: does not read Application.platform");
        if (reads.All(i => Patched(i, android.Value, out _)))
        {
            Patched(reads[0], android.Value, out int was);
            return new Result { Outcome = Outcome.AlreadyApplied, Details = { $"{where}: Android reads as {Cecil.EnumName(plat, was)}" } };
        }
        string hash = BlasphemousCatalogs.Shape(md);
        if (hash != KnownHash) return Result.NotApplicable($"{where}: not as this patch knows it (IL {hash}, known {KnownHash})");

        int? target = BuildTarget(Path.Combine(game.Dir, "..", "globalgamemanagers"));
        if (target == null) return Result.Failed($"{where}: the data's platform is unknown (no readable globalgamemanagers beside Managed)");
        string name = target switch { 17 or 24 or 25 => "LinuxPlayer", 2 or 4 or 27 => "OSXPlayer", 5 or 19 => "WindowsPlayer", _ => null };
        if (name == null) return Result.Failed($"{where}: the data's build target {target} is not a desktop one");
        int? value = Cecil.EnumValue(plat, name);
        if (value == null) return Result.Failed($"{where}: no UnityEngine.RuntimePlatform.{name}");

        var r = new Result { Outcome = Outcome.Applied };
        r.Details.Add($"{where}: Application.platform Android reads as {name} ({reads.Count} reads): the preload scene of the data's build");
        if (!apply) return r;
        var body = md.Body;
        body.SimplifyMacros();
        var il = body.GetILProcessor();
        foreach (var read in reads)
        {
            var end = il.Create(OpCodes.Nop);
            var seq = new[] { il.Create(OpCodes.Dup), il.Create(OpCodes.Ldc_I4, android.Value), il.Create(OpCodes.Bne_Un, end),
                              il.Create(OpCodes.Pop), il.Create(OpCodes.Ldc_I4, value.Value), end };
            var prev = read;
            foreach (var x in seq) { il.InsertAfter(prev, x); prev = x; }
        }
        body.OptimizeMacros();
        game.MarkChanged(asm);
        return r;
    }

    static bool IsGetPlatform(Instruction i) =>
        i.OpCode.Code == Code.Call && i.Operand is MethodReference r && r.Name == "get_platform" && r.DeclaringType.FullName == "UnityEngine.Application";

    // call get_platform; dup; ldc.i4 Android; bne.un L; pop; ldc.i4 was; L: nop
    static bool Patched(Instruction read, int android, out int was)
    {
        was = 0;
        var a = read.Next; var b = a?.Next; var c = b?.Next; var d = c?.Next; var e = d?.Next; var f = e?.Next;
        if (a?.OpCode.Code != Code.Dup || b == null || Cecil.ConstInt(b) != android) return false;
        if (c == null || (c.OpCode.Code != Code.Bne_Un && c.OpCode.Code != Code.Bne_Un_S) || c.Operand != f) return false;
        if (d?.OpCode.Code != Code.Pop || e == null || Cecil.ConstInt(e) is not int v || f?.OpCode.Code != Code.Nop) return false;
        was = v;
        return true;
    }

    /// <summary>The build target in a serialized file's header (Unity 5 to 2019: format 9 to 21; 22 and later), null
    /// when the file is missing or not one.</summary>
    public static int? BuildTarget(string path)
    {
        try
        {
            using var f = File.OpenRead(path);
            var h = new byte[48];
            if (f.Read(h, 0, h.Length) < h.Length) return null;
            static uint Be(byte[] b, int o) => (uint)(b[o] << 24 | b[o + 1] << 16 | b[o + 2] << 8 | b[o + 3]);
            uint format = Be(h, 8);
            int pos;
            bool little;
            if (format >= 9 && format <= 21) { little = h[16] == 0; pos = 20; }
            else if (format >= 22 && format < 64) { little = h[16] == 0; pos = 48; }
            else return null;
            f.Position = pos;
            var s = new byte[64];
            int n = f.Read(s, 0, s.Length);
            int z = Array.IndexOf(s, (byte)0, 0, n);
            if (z < 1 || z + 5 > n || s[0] < (byte)'1' || s[0] > (byte)'9') return null;   // the Unity version, e.g. 2017.4.40f1
            int t = little ? BitConverter.ToInt32(s, z + 1) : (int)Be(s, z + 1);
            return t;
        }
        catch (IOException) { return null; }
        catch (UnauthorizedAccessException) { return null; }
    }
}
