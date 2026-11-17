// blasphemous-preload on a stand-in: a UnityEngine.CoreModule with RuntimePlatform (Unity's own numbers) and an
// Application whose platform the test sets, and an Assembly-CSharp whose LevelManager.GetPlatformName has the
// game's IL, instruction for instruction (so its hash is the one the patch knows). The data folder's
// globalgamemanagers says the build target. Patched, loaded and called for each platform.
using System.Runtime.Loader;
using Mono.Cecil;
using Mono.Cecil.Cil;
using AssemblyName = System.Reflection.AssemblyName;

namespace AsmPatch.Tests;

static partial class TestMain
{
    // UnityEngine.RuntimePlatform's own numbers (the game's switch has them compiled in)
    const int OSXEditor = 0, OSXPlayer = 1, WindowsPlayer = 2, WindowsEditor = 7, IPhonePlayer = 8, AndroidPlayer = 11, LinuxPlayer = 13, LinuxEditor = 16;

    static void BlasphemousTests()
    {
        foreach (var (target, data, androidName) in new[] { (24, "linux", "Linux"), (2, "osx", "OSX"), (19, "windows", "Windows") })
        {
            var (managed, _) = BlasphemousData("bl-" + data, target, 17);
            var o = Path.Combine(tmp, "bl-" + data + "-out");
            var (st, outp) = Patch(managed, o, only: new[] { "blasphemous-preload" });
            Check(st == 0 && outp.Contains("ok   blasphemous-preload") && File.Exists(Path.Combine(o, "Assembly-CSharp.dll")), $"blasphemous-preload ({data} data): applied");
            foreach (var (p, want) in new[] { (AndroidPlayer, androidName), (LinuxPlayer, "Linux"), (LinuxEditor, "Linux"), (WindowsPlayer, "Windows"),
                                              (WindowsEditor, "Windows"), (OSXPlayer, "OSX"), (OSXEditor, "OSX"), (IPhonePlayer, "IPhonePlayer") })
            {
                var got = PlatformName(managed, o, p);
                Check(got == want, $"blasphemous-preload ({data} data): platform {p} -> {got} (want {want})");
            }
            var unpatched = PlatformName(managed, managed, AndroidPlayer);
            Check(unpatched == "Android", $"blasphemous-preload: the stand-in unpatched says {unpatched} on Android, as the game does");
            // again on the patched assembly: done, nothing written
            var again = Path.Combine(tmp, "bl-" + data + "-again");
            File.Copy(Path.Combine(o, "Assembly-CSharp.dll"), Path.Combine(managed, "Assembly-CSharp.dll"), true);
            (st, outp) = Patch(managed, again, only: new[] { "blasphemous-preload" });
            Check(st == 0 && outp.Contains("done blasphemous-preload") && NoDlls(again), $"blasphemous-preload ({data} data): a second run finds it done");
        }
        // the 2019+ header (format 22) reads the same
        {
            var (managed, _) = BlasphemousData("bl-f22", 24, 22);
            var o = Path.Combine(tmp, "bl-f22-out");
            var (st, outp) = Patch(managed, o, only: new[] { "blasphemous-preload" });
            Check(st == 0 && outp.Contains("reads as LinuxPlayer") && PlatformName(managed, o, AndroidPlayer) == "Linux", "blasphemous-preload: format 22 header");
        }
        // no globalgamemanagers: refuses (exit 1, nothing written)
        {
            var (managed, data) = BlasphemousData("bl-none", 24, 17);
            File.Delete(Path.Combine(data, "globalgamemanagers"));
            var o = Path.Combine(tmp, "bl-none-out");
            var (st, outp) = Patch(managed, o, only: new[] { "blasphemous-preload" });
            Check(st == 1 && outp.Contains("FAIL blasphemous-preload") && NoDlls(o), "blasphemous-preload: unknown data platform fails, nothing written");
        }
        // a mobile build target (iOS 9): refuses
        {
            var (managed, _) = BlasphemousData("bl-ios", 9, 17);
            var (st, outp) = Patch(managed, Path.Combine(tmp, "bl-ios-out"), only: new[] { "blasphemous-preload" });
            Check(st == 1 && outp.Contains("not a desktop one"), "blasphemous-preload: a non-desktop build target fails");
        }
        // another GetPlatformName (one more instruction): not this patch's
        {
            var (managed, _) = BlasphemousData("bl-other", 24, 17, extraNop: true);
            var o = Path.Combine(tmp, "bl-other-out");
            var (st, outp) = Patch(managed, o, only: new[] { "blasphemous-preload" });
            Check(st == 0 && outp.Contains("n/a  blasphemous-preload") && outp.Contains("not as this patch knows it") && NoDlls(o), "blasphemous-preload: other IL is left alone");
        }
        // no LevelManager: n/a (the Rewired fakes elsewhere have none)
        {
            var d = Dir("bl-absent");
            NewModule("Assembly-CSharp").Write(Path.Combine(d, "Assembly-CSharp.dll"));
            var (st, outp) = Patch(d, Path.Combine(tmp, "bl-absent-out"), only: new[] { "blasphemous-preload" });
            Check(st == 0 && outp.Contains("n/a  blasphemous-preload: no Blasphemous"), "blasphemous-preload: n/a without LevelManager");
        }
    }

    /// <summary>A data folder (globalgamemanagers with the build target, header format 17 or 22) with Managed/:
    /// UnityEngine.CoreModule and Assembly-CSharp. Returns (Managed, data folder).</summary>
    static (string managed, string data) BlasphemousData(string name, int target, int format, bool extraNop = false)
    {
        var data = Dir(name);
        var managed = Path.Combine(data, "Managed");
        Directory.CreateDirectory(managed);
        var h = new List<byte>();
        void Be(uint v) { h.Add((byte)(v >> 24)); h.Add((byte)(v >> 16)); h.Add((byte)(v >> 8)); h.Add((byte)v); }
        Be(0x0fa5); Be(format == 22 ? 0u : 0x1ab9a8u); Be((uint)format); Be(format == 22 ? 0u : 0x1000u);
        h.AddRange(new byte[] { 0, 0, 0, 0 });
        if (format == 22) h.AddRange(new byte[28]);
        h.AddRange(System.Text.Encoding.ASCII.GetBytes("2017.4.40f1\0"));
        h.AddRange(BitConverter.GetBytes(target));
        h.AddRange(new byte[64]);
        File.WriteAllBytes(Path.Combine(data, "globalgamemanagers"), h.ToArray());

        var u = NewModule("UnityEngine.CoreModule");
        var plat = Enum(u, "UnityEngine", "RuntimePlatform", ("OSXEditor", OSXEditor), ("OSXPlayer", OSXPlayer), ("WindowsPlayer", WindowsPlayer),
                        ("WindowsEditor", WindowsEditor), ("IPhonePlayer", IPhonePlayer), ("Android", AndroidPlayer), ("LinuxPlayer", LinuxPlayer), ("LinuxEditor", LinuxEditor));
        var app = new TypeDefinition("UnityEngine", "Application", TypeAttributes.Public | TypeAttributes.Sealed | TypeAttributes.BeforeFieldInit, u.TypeSystem.Object);
        var field = new FieldDefinition("Platform", FieldAttributes.Public | FieldAttributes.Static, plat);
        app.Fields.Add(field);
        var get = new MethodDefinition("get_platform", MethodAttributes.Public | MethodAttributes.Static | MethodAttributes.HideBySig | MethodAttributes.SpecialName, plat);
        var gil = get.Body.GetILProcessor();
        gil.Emit(OpCodes.Ldsfld, field); gil.Emit(OpCodes.Ret);
        app.Methods.Add(get);
        app.Properties.Add(new PropertyDefinition("platform", PropertyAttributes.None, plat) { GetMethod = get });
        u.Types.Add(app);
        u.Write(Path.Combine(managed, "UnityEngine.CoreModule.dll"));

        // GetPlatformName as the game compiled it (Blasphemous 4.0)
        var m = NewModule("Assembly-CSharp");
        var lm = new TypeDefinition("Framework.Managers", "LevelManager", TypeAttributes.Public | TypeAttributes.BeforeFieldInit, m.TypeSystem.Object);
        var md = new MethodDefinition("GetPlatformName", MethodAttributes.Private | MethodAttributes.HideBySig, m.TypeSystem.String);
        md.Body.InitLocals = true;
        var pt = m.ImportReference(plat);
        var v0 = new VariableDefinition(m.TypeSystem.String); var v1 = new VariableDefinition(pt); var v2 = new VariableDefinition(pt);
        md.Body.Variables.Add(v0); md.Body.Variables.Add(v1); md.Body.Variables.Add(v2);
        var il = md.Body.GetILProcessor();
        var getP = m.ImportReference(get);
        var toStr = new MethodReference("ToString", m.TypeSystem.String, m.TypeSystem.Object) { HasThis = true };
        var end = il.Create(OpCodes.Ldloc_0);
        var win = il.Create(OpCodes.Ldstr, "Windows"); var lin = il.Create(OpCodes.Ldstr, "Linux"); var osx = il.Create(OpCodes.Ldstr, "OSX");
        var second = il.Create(OpCodes.Ldloc_2);
        il.Emit(OpCodes.Call, getP); il.Emit(OpCodes.Stloc_1); il.Emit(OpCodes.Ldloca_S, v1);
        il.Emit(OpCodes.Constrained, pt); il.Emit(OpCodes.Callvirt, toStr); il.Emit(OpCodes.Stloc_0);
        if (extraNop) il.Emit(OpCodes.Nop);
        il.Emit(OpCodes.Call, getP); il.Emit(OpCodes.Stloc_2); il.Emit(OpCodes.Ldloc_2);
        il.Emit(OpCodes.Switch, new[] { osx, osx, win, second, second, second, second, win });
        il.Append(second); il.Emit(OpCodes.Ldc_I4_S, (sbyte)13); il.Emit(OpCodes.Sub);
        il.Emit(OpCodes.Switch, new[] { lin, end, end, lin });
        il.Emit(OpCodes.Br, end);
        il.Append(win); il.Emit(OpCodes.Stloc_0); il.Emit(OpCodes.Br, end);
        il.Append(lin); il.Emit(OpCodes.Stloc_0); il.Emit(OpCodes.Br, end);
        il.Append(osx); il.Emit(OpCodes.Stloc_0); il.Emit(OpCodes.Br, end);
        il.Append(end); il.Emit(OpCodes.Ret);
        lm.Methods.Add(md);
        // public string Name() => GetPlatformName() (the test's way in)
        var call = new MethodDefinition("Name", MethodAttributes.Public | MethodAttributes.HideBySig, m.TypeSystem.String);
        var cil = call.Body.GetILProcessor();
        cil.Emit(OpCodes.Ldarg_0); cil.Emit(OpCodes.Call, md); cil.Emit(OpCodes.Ret);
        lm.Methods.Add(call);
        lm.Methods.Add(Ctor(m, ObjectCtor(m)));
        m.Types.Add(lm);
        m.Write(Path.Combine(managed, "Assembly-CSharp.dll"));
        return (managed, data);
    }

    /// <summary>GetPlatformName() of the Assembly-CSharp in dir (UnityEngine from orig) with Application.platform = p.</summary>
    static string PlatformName(string orig, string dir, int p)
    {
        var ctx = new AssemblyLoadContext("bl" + Guid.NewGuid(), isCollectible: true);
        ctx.Resolving += (c, n) =>
        {
            foreach (var d in new[] { dir, orig })
            {
                var f = Path.Combine(d, n.Name + ".dll");
                if (File.Exists(f)) return c.LoadFromStream(new MemoryStream(File.ReadAllBytes(f)));
            }
            return null;
        };
        try
        {
            var u = ctx.LoadFromAssemblyName(new AssemblyName("UnityEngine.CoreModule"));
            var app = u.GetType("UnityEngine.Application", true);
            app.GetField("Platform").SetValue(null, System.Enum.ToObject(u.GetType("UnityEngine.RuntimePlatform", true), p));
            var a = ctx.LoadFromAssemblyName(new AssemblyName("Assembly-CSharp"));
            var t = a.GetType("Framework.Managers.LevelManager", true);
            return (string)t.GetMethod("Name").Invoke(Activator.CreateInstance(t), null);
        }
        finally { ctx.Unload(); }
    }
}
