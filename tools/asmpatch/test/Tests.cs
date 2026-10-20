// asmpatch tests. The "libraries" are made here with Cecil: a Rewired_Core with InputManager_Base and
// platform enums numbered unlike any real Rewired (so a patch that used numbers would fail), and a
// game whose InputManager.DetectPlatform says Linux, with early returns and a short branch that the
// patch pushes out of range. They reference mscorlib, as a game's assemblies do, so the .NET runtime
// running the tests loads them through its facade.
using System.Runtime.Loader;
using Mono.Cecil;
using Mono.Cecil.Cil;
using AssemblyName = System.Reflection.AssemblyName;

namespace AsmPatch.Tests;

static class TestMain
{
    static int failures, passes;
    static string tmp;

    static void Check(bool ok, string what)
    {
        if (ok) passes++; else failures++;
        Console.WriteLine($"{(ok ? "PASS" : "FAIL")} {what}");
    }

    static int Main()
    {
        tmp = Path.Combine(Path.GetTempPath(), "asmpatch-test-" + Environment.ProcessId);
        Directory.CreateDirectory(tmp);
        try
        {
            SourceEdition();
            DllEdition();
            NoRewired();
            NoAndroidConstant();
            Cli();
        }
        finally { Directory.Delete(tmp, true); }
        Console.WriteLine($"{passes} passed, {failures} failed: {(failures == 0 ? "PASSED" : "FAILED")}");
        return failures == 0 ? 0 : 1;
    }

    // ---- fake assemblies ------------------------------------------------------------------
    const int Android = 42, Linux = 6, Windows = 1, EditorNone = 3, WebNone = 0;

    static ModuleDefinition NewModule(string name) =>
        AssemblyDefinition.CreateAssembly(new AssemblyNameDefinition(name, new Version(1, 0, 0, 0)), name,
            new ModuleParameters { Kind = ModuleKind.Dll, Runtime = TargetRuntime.Net_4_0 }).MainModule;

    static TypeDefinition Enum(ModuleDefinition m, string ns, string name, params (string, int)[] values)
    {
        var e = new TypeDefinition(ns, name, TypeAttributes.Public | TypeAttributes.Sealed,
                                   new TypeReference("System", "Enum", m, m.TypeSystem.CoreLibrary));
        e.Fields.Add(new FieldDefinition("value__", FieldAttributes.Public | FieldAttributes.SpecialName | FieldAttributes.RTSpecialName, m.TypeSystem.Int32));
        foreach (var (n, v) in values)
            e.Fields.Add(new FieldDefinition(n, FieldAttributes.Public | FieldAttributes.Static | FieldAttributes.Literal | FieldAttributes.HasDefault, e) { Constant = v });
        m.Types.Add(e);
        return e;
    }

    static MethodReference ObjectCtor(ModuleDefinition m) => new(".ctor", m.TypeSystem.Void, m.TypeSystem.Object) { HasThis = true };

    static MethodDefinition Ctor(ModuleDefinition m, MethodReference baseCtor)
    {
        var c = new MethodDefinition(".ctor", MethodAttributes.Public | MethodAttributes.HideBySig | MethodAttributes.SpecialName | MethodAttributes.RTSpecialName, m.TypeSystem.Void);
        var il = c.Body.GetILProcessor();
        il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Call, baseCtor); il.Emit(OpCodes.Ret);
        return c;
    }

    /// <summary>Rewired_Core: the platform enums and InputManager_Base (fields, virtual DetectPlatform,
    /// public Detect() calling it). withAndroid=false: a Platform enum without Android.</summary>
    static ModuleDefinition Core(string dir, bool withAndroid = true)
    {
        var m = NewModule("Rewired_Core");
        var plat = withAndroid
            ? Enum(m, "Rewired.Platforms", "Platform", ("Unknown", 0), ("Windows", Windows), ("Linux", Linux), ("Android", Android))
            : Enum(m, "Rewired.Platforms", "Platform", ("Unknown", 0), ("Windows", Windows), ("Linux", Linux));
        var ed = Enum(m, "Rewired.Platforms", "EditorPlatform", ("Linux", 0), ("Windows", 1), ("None", EditorNone));
        var web = Enum(m, "Rewired.Platforms", "WebplayerPlatform", ("None", WebNone), ("Windows", 1));
        var b = new TypeDefinition("Rewired", "InputManager_Base", TypeAttributes.Public | TypeAttributes.Abstract | TypeAttributes.BeforeFieldInit, m.TypeSystem.Object);
        b.Fields.Add(new FieldDefinition("platform", FieldAttributes.Public, plat));
        b.Fields.Add(new FieldDefinition("editorPlatform", FieldAttributes.Public, ed));
        b.Fields.Add(new FieldDefinition("webplayerPlatform", FieldAttributes.Public, web));
        b.Fields.Add(new FieldDefinition("isEditor", FieldAttributes.Public, m.TypeSystem.Boolean));
        var dp = new MethodDefinition("DetectPlatform", MethodAttributes.Family | MethodAttributes.Virtual | MethodAttributes.HideBySig | MethodAttributes.NewSlot | MethodAttributes.Abstract, m.TypeSystem.Void);
        b.Methods.Add(dp);
        var detect = new MethodDefinition("Detect", MethodAttributes.Public | MethodAttributes.HideBySig, m.TypeSystem.Void);
        var il = detect.Body.GetILProcessor();
        il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Callvirt, dp); il.Emit(OpCodes.Ret);
        b.Methods.Add(detect);
        b.Methods.Add(Ctor(m, ObjectCtor(m)));
        m.Types.Add(b);
        m.Write(Path.Combine(dir, "Rewired_Core.dll"));
        return m;
    }

    /// <summary>A Rewired.InputManager deriving from core's base, in assembly <name>. Its
    /// DetectPlatform: editorPlatform = Windows, isEditor = true, webplayerPlatform = Windows;
    /// if (EarlyOut) return (a branch straight to a ret); if (Skip) goto L (a short branch over 100
    /// nops and a ret); ret; L: platform = <platform>; ret.</summary>
    static void Manager(string dir, string name, ModuleDefinition core, int platform)
    {
        var m = NewModule(name);
        var b = core.Types.First(t => t.Name == "InputManager_Base");
        FieldReference F(string n) => m.ImportReference(b.Fields.First(f => f.Name == n));
        var t = new TypeDefinition("Rewired", "InputManager", TypeAttributes.Public | TypeAttributes.BeforeFieldInit, m.ImportReference(b));
        var early = new FieldDefinition("EarlyOut", FieldAttributes.Public | FieldAttributes.Static, m.TypeSystem.Boolean);
        var skip = new FieldDefinition("Skip", FieldAttributes.Public | FieldAttributes.Static, m.TypeSystem.Boolean);
        t.Fields.Add(early); t.Fields.Add(skip);
        var dp = new MethodDefinition("DetectPlatform", MethodAttributes.Family | MethodAttributes.Virtual | MethodAttributes.HideBySig, m.TypeSystem.Void);
        var il = dp.Body.GetILProcessor();
        il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Stfld, F("editorPlatform"));
        il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Stfld, F("isEditor"));
        il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Stfld, F("webplayerPlatform"));
        var ret1 = il.Create(OpCodes.Ret);
        var label = il.Create(OpCodes.Ldarg_0);
        il.Emit(OpCodes.Ldsfld, early); il.Emit(OpCodes.Brtrue, ret1);
        il.Emit(OpCodes.Ldsfld, skip); il.Emit(OpCodes.Brtrue_S, label);
        for (int i = 0; i < 100; i++) il.Emit(OpCodes.Nop);
        il.Append(ret1);
        il.Append(label); il.Emit(OpCodes.Ldc_I4_S, (sbyte)platform); il.Emit(OpCodes.Stfld, F("platform"));
        il.Emit(OpCodes.Ret);
        t.Methods.Add(dp);
        t.Methods.Add(Ctor(m, m.ImportReference(b.Methods.First(x => x.IsConstructor))));
        m.Types.Add(t);
        m.Write(Path.Combine(dir, name + ".dll"));
    }

    static bool NoDlls(string d) => !Directory.Exists(d) || Directory.GetFiles(d, "*.dll").Length == 0;

    static string Dir(string name)
    {
        var d = Path.Combine(tmp, name);
        Directory.CreateDirectory(d);
        return d;
    }

    // ---- running and calling ----------------------------------------------------------------
    static (int status, string output) Patch(string inDir, string outDir, string[] only = null)
    {
        var w = new StringWriter();
        var old = Console.Out;
        Console.SetOut(w);
        int status;
        var game = new Game(inDir);
        try { status = Program.Run(game, outDir, only, Array.Empty<string>()); }
        finally { game.Dispose(); Console.SetOut(old); }
        foreach (var l in w.ToString().Split('\n', StringSplitOptions.RemoveEmptyEntries)) Console.WriteLine("     | " + l);
        return (status, w.ToString());
    }

    /// <summary>Loads <manager>.dll from the patched dir (the rest from the original dir), sets the
    /// static flags, calls Detect() and returns the four fields.</summary>
    static (int platform, int editor, int web, bool isEditor) Call(string orig, string patched, string manager, bool earlyOut, bool skip)
    {
        var ctx = new AssemblyLoadContext("t" + Guid.NewGuid(), isCollectible: true);
        ctx.Resolving += (c, n) =>
        {
            foreach (var d in new[] { patched, orig })
            {
                var p = Path.Combine(d, n.Name + ".dll");
                if (File.Exists(p)) return c.LoadFromStream(new MemoryStream(File.ReadAllBytes(p)));
            }
            return null;
        };
        try
        {
            var asm = ctx.LoadFromAssemblyName(new AssemblyName(manager));
            var t = asm.GetType("Rewired.InputManager", true);
            t.GetField("EarlyOut").SetValue(null, earlyOut);
            t.GetField("Skip").SetValue(null, skip);
            var o = Activator.CreateInstance(t);
            t.GetMethod("Detect").Invoke(o, null);
            int I(string f) => Convert.ToInt32(t.GetField(f).GetValue(o));
            return (I("platform"), I("editorPlatform"), I("webplayerPlatform"), (bool)t.GetField("isEditor").GetValue(o));
        }
        finally { ctx.Unload(); }
    }

    static void Calls(string orig, string patched, string manager, string what)
    {
        foreach (var (early, skip) in new[] { (false, false), (true, false), (false, true) })
        {
            var r = Call(orig, patched, manager, early, skip);
            Check(r == (Android, EditorNone, WebNone, false),
                  $"{what}: EarlyOut={early} Skip={skip}: platform {r.platform} editor {r.editor} web {r.web} isEditor {r.isEditor}");
        }
    }

    static IEnumerable<string> Refs(string path)
    {
        using var a = AssemblyDefinition.ReadAssembly(path);
        return a.MainModule.AssemblyReferences.Select(r => r.Name).OrderBy(x => x).ToList();
    }

    // ---- tests ------------------------------------------------------------------------------
    static void SourceEdition()
    {
        var d = Dir("source"); var o = Path.Combine(tmp, "source-out");
        var core = Core(d);
        Manager(d, "Assembly-CSharp", core, Linux);
        var before = Call(d, d, "Assembly-CSharp", false, true);
        Check(before.platform == Linux, "source edition: unpatched DetectPlatform says Linux");
        var (st, outp) = Patch(d, o);
        Check(st == 0 && outp.Contains("ok   rewired-android: Assembly-CSharp.dll: Rewired.InputManager.DetectPlatform: platform Linux -> Android (42)"),
              "source edition: patched, reported with the enum's names and Android's own number");
        Check(File.Exists(Path.Combine(o, "Assembly-CSharp.dll")) && !File.Exists(Path.Combine(o, "Rewired_Core.dll")),
              "source edition: only the changed assembly is written");
        Check(Refs(Path.Combine(o, "Assembly-CSharp.dll")).SequenceEqual(Refs(Path.Combine(d, "Assembly-CSharp.dll"))),
              "source edition: assembly references unchanged (" + string.Join(", ", Refs(Path.Combine(o, "Assembly-CSharp.dll"))) + ")");
        Calls(d, o, "Assembly-CSharp", "source edition");
        // again, on the patched copy: nothing to do
        var d2 = Dir("source-again"); var o2 = Path.Combine(tmp, "source-again-out");
        File.Copy(Path.Combine(o, "Assembly-CSharp.dll"), Path.Combine(d2, "Assembly-CSharp.dll"));
        File.Copy(Path.Combine(d, "Rewired_Core.dll"), Path.Combine(d2, "Rewired_Core.dll"));
        (st, outp) = Patch(d2, o2);
        Check(st == 0 && outp.Contains("done rewired-android") && NoDlls(o2), "patched twice: already applied, nothing written");
    }

    static void DllEdition()
    {
        var d = Dir("dll"); var o = Path.Combine(tmp, "dll-out");
        var core = Core(d);
        Manager(d, "Rewired_Windows", core, Windows);
        var (st, outp) = Patch(d, o);
        Check(st == 0 && outp.Contains("Rewired_Windows.dll: Rewired.InputManager.DetectPlatform: platform Windows -> Android"),
              "DLL edition: InputManager in a Rewired assembly is patched");
        Calls(d, o, "Rewired_Windows", "DLL edition");
        (st, outp) = Patch(d, Path.Combine(tmp, "dll-skip"), only: new[] { "none-such" });
        Check(st == 0 && outp.Contains("skip rewired-android") && NoDlls(Path.Combine(tmp, "dll-skip")), "--only without the patch: skipped");
    }

    static void NoRewired()
    {
        var d = Dir("plain"); var o = Path.Combine(tmp, "plain-out");
        var m = NewModule("Assembly-CSharp");
        m.Types.Add(new TypeDefinition("", "Game", TypeAttributes.Public, m.TypeSystem.Object));
        m.Write(Path.Combine(d, "Assembly-CSharp.dll"));
        File.WriteAllText(Path.Combine(d, "native.dll"), "not a PE file");   // e.g. a stray native library
        var (st, outp) = Patch(d, o);
        Check(st == 0 && outp.Contains("n/a  rewired-android") && NoDlls(o), "no Rewired: not applicable, nothing written");
    }

    static void NoAndroidConstant()
    {
        var d = Dir("noandroid"); var o = Path.Combine(tmp, "noandroid-out");
        var core = Core(d, withAndroid: false);
        Manager(d, "Assembly-CSharp", core, Linux);
        var (st, outp) = Patch(d, o);
        Check(st == 1 && outp.Contains("FAIL rewired-android") && outp.Contains("no 'Android' constant") && NoDlls(o),
              "Platform enum without Android: fails, nothing written");
    }

    static void Cli()
    {
        // the built tool itself, as a port script runs it (dotnet out/any/asmpatch.dll)
        var dll = Path.Combine(AppContext.BaseDirectory, "..", "any", "asmpatch.dll");
        if (!File.Exists(dll)) { Console.WriteLine("skip CLI (no out/any/asmpatch.dll)"); return; }
        var d = Path.Combine(tmp, "source");
        string Run(params string[] a)
        {
            var psi = new System.Diagnostics.ProcessStartInfo(Environment.ProcessPath ?? "dotnet") { RedirectStandardOutput = true, RedirectStandardError = true };
            if (Path.GetFileNameWithoutExtension(psi.FileName) != "dotnet") psi.FileName = "dotnet";
            psi.ArgumentList.Add(dll);
            foreach (var x in a) psi.ArgumentList.Add(x);
            using var p = System.Diagnostics.Process.Start(psi);
            var s = p.StandardOutput.ReadToEnd() + p.StandardError.ReadToEnd();
            p.WaitForExit();
            return p.ExitCode + "\n" + s;
        }
        var list = Run("--list", d);
        Check(list.StartsWith("0\n") && list.Contains("would rewired-android") && !Directory.Exists(Path.Combine(tmp, "cli-out")), "CLI --list <dir>: reports, writes nothing");
        var bad = Run("--only=nope", d, Path.Combine(tmp, "cli-out"));
        Check(bad.StartsWith("2\n") && bad.Contains("no patch named nope"), "CLI: unknown patch name is a usage error");
        var ok = Run(d, Path.Combine(tmp, "cli-out"));
        Check(ok.StartsWith("0\n") && ok.Contains("wrote Assembly-CSharp.dll"), "CLI: patches and writes");
        var survey = Run("--survey", d);
        Check(survey.StartsWith("0\n") && survey.Contains("source edition") && survey.Contains("DetectPlatform sets Linux"), "CLI --survey: finds Rewired");
    }
}
