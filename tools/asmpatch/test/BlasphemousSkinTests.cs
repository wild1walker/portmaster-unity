// blasphemous-skin-idle on the stand-in (test/fakes/Blasphemous: Game.cs' ExtrasMenuWidget, Engine.cs' Image and
// CanvasGroup). The driver goes through the Extras menu as a run does (frames of the previews' idle animation between
// the steps: the menu hidden, the extras page, the skin selector, the pad moving through the skins, back, hidden again,
// a locked skin, whose element the game makes inactive and whose lookup then throws, a page without its Selection).
// Patched, every frame shows what the game's shows (the skin page's alpha, and while it is shown each preview's
// enabled flag and sprite), with fewer rebuilds; off: the game's. Refusals: another ShowMenu, the page's alpha set
// elsewhere, the elements read elsewhere.
using Mono.Cecil;
using Mono.Cecil.Cil;

namespace AsmPatch.Tests;

static partial class TestMain
{
    static void BlasphemousSkinTests()
    {
        if (!File.Exists(Path.Combine(FakeBl, "Assembly-CSharp.dll"))) { Check(false, $"Blasphemous stand-ins in {FakeBl} (build.sh test builds them)"); return; }
        var src = BlSrc("blskin");
        var saved = (BlasphemousSkinIdle.ShowHash, BlasphemousSkinIdle.ResetHash, BlasphemousSkinIdle.ValuesHash, BlasphemousSkinIdle.InputHash);
        {
            using var a = AssemblyDefinition.ReadAssembly(Path.Combine(src, "Assembly-CSharp.dll"));
            var w = a.MainModule.GetType("Gameplay.UI.Others.MenuLogic.ExtrasMenuWidget");
            string H(string n) => BlasphemousCatalogs.Shape(w.Methods.First(x => x.Name == n));
            (BlasphemousSkinIdle.ShowHash, BlasphemousSkinIdle.ResetHash, BlasphemousSkinIdle.ValuesHash, BlasphemousSkinIdle.InputHash) =
                (H("ShowMenu"), H("ResetMenus"), H("ShowSkinSelectorValues"), H("UpdateInputSkinSelectorOptions"));
        }
        Game.AllowForeign = true;
        try
        {
            var o = Path.Combine(tmp, "blskin-out");
            var (st, outp) = Patch(src, o, only: Array.Empty<string>(), with: new[] { "blasphemous-skin-idle" });
            Check(st == 0 && outp.Contains("ok   blasphemous-skin-idle: Assembly-CSharp.dll"), "blasphemous-skin-idle: applied to the stand-in\n" + outp);
            if (!File.Exists(Path.Combine(o, "Assembly-CSharp.dll"))) return;
            foreach (var f in new[] { "UnityEngine.CoreModule.dll", "Assembly-CSharp-firstpass.dll" }) File.Copy(Path.Combine(src, f), Path.Combine(o, f), true);
            const string V = BlasphemousSkinIdle.Variable;
            static string Shown(string t) => string.Join("\n", t.Split('\n').Where(l => !l.StartsWith("rebuilds ")));
            static int Rebuilds(string t) => t.Split('\n').Last() is var l && l.StartsWith("rebuilds ") ? int.Parse(l[9..]) : -1;
            var (g, _) = BlCall(src, V, "", "RunSkins", false);
            var (p, pl) = BlCall(o, V, "", "RunSkins", false);
            CheckSeen(!g.StartsWith("EXCEPTION") && g.Contains("(threw NullReferenceException)") && Shown(p) == Shown(g) && Rebuilds(p) < Rebuilds(g) &&
                      pl.Contains("[blasphemous-skin-idle] the skin selector's preview Images idle"),
                      $"blasphemous-skin-idle: patched, every frame shows what the game's shows, rebuilds {Rebuilds(g)} -> {Rebuilds(p)}",
                      "--- game\n" + g + "\n--- patched\n" + p + "\n" + pl);
            var (f0, fl) = BlCall(o, V, "off", "RunSkins", false);
            CheckSeen(f0 == g && fl.Contains("=off"), "blasphemous-skin-idle: off: the game's", f0 + "\n" + fl);

            void Refuses(string name, Action<ModuleDefinition> change, string why)
            {
                var d = BlSrc("blskin-" + name);
                var path = Path.Combine(d, "Assembly-CSharp.dll");
                using (var a = AssemblyDefinition.ReadAssembly(path, new ReaderParameters { InMemory = true })) { change(a.MainModule); a.Write(path); }
                var (s2, out2) = Patch(d, Path.Combine(tmp, "blskin-" + name + "-out"), only: Array.Empty<string>(), with: new[] { "blasphemous-skin-idle" });
                Check(s2 == 0 && out2.Contains("n/a  blasphemous-skin-idle") && out2.Contains(why), $"blasphemous-skin-idle refuses {name}: {why}\n" + out2);
            }
            TypeDefinition W(ModuleDefinition m) => m.GetType("Gameplay.UI.Others.MenuLogic.ExtrasMenuWidget");
            Refuses("another-show", m =>
            {
                var md = W(m).Methods.First(x => x.Name == "ShowMenu");
                md.Body.GetILProcessor().InsertBefore(md.Body.Instructions[0], Instruction.Create(OpCodes.Nop));
            }, "not as this patch knows them");
            // a method of its own copying ResetMenus' body: sets an alpha, outside ShowMenu and ResetMenus
            Refuses("alpha-elsewhere", m =>
            {
                var w = W(m); var r = w.Methods.First(x => x.Name == "ResetMenus");
                var md = new MethodDefinition("Fade", MethodAttributes.Public, m.TypeSystem.Void);
                foreach (var v in r.Body.Variables) md.Body.Variables.Add(new VariableDefinition(v.VariableType));
                var map = new Dictionary<Instruction, Instruction>();
                foreach (var i in r.Body.Instructions) { var c = Instruction.Create(OpCodes.Nop); c.OpCode = i.OpCode; c.Operand = i.Operand; map[i] = c; md.Body.Instructions.Add(c); }
                foreach (var c in md.Body.Instructions)
                    if (c.Operand is Instruction ti) c.Operand = map[ti];
                    else if (c.Operand is VariableDefinition vd) c.Operand = md.Body.Variables[vd.Index];
                foreach (var e in r.Body.ExceptionHandlers)
                    md.Body.ExceptionHandlers.Add(new ExceptionHandler(e.HandlerType) { TryStart = map[e.TryStart], TryEnd = e.TryEnd == null ? null : map[e.TryEnd],
                                                                                      HandlerStart = map[e.HandlerStart], HandlerEnd = e.HandlerEnd == null ? null : map[e.HandlerEnd], CatchType = e.CatchType });
                w.Methods.Add(md);
            }, "a CanvasGroup's alpha is set in");
            Refuses("elements-elsewhere", m =>
            {
                var w = W(m); var f = w.Fields.First(x => x.Name == "skinSelectorDataElements");
                var md = new MethodDefinition("Peek", MethodAttributes.Public, m.TypeSystem.Void);
                var il = md.Body.GetILProcessor();
                il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, f); il.Emit(OpCodes.Pop); il.Emit(OpCodes.Ret);
                w.Methods.Add(md);
            }, "skinSelectorDataElements is read in");
        }
        finally
        {
            Game.AllowForeign = false;
            (BlasphemousSkinIdle.ShowHash, BlasphemousSkinIdle.ResetHash, BlasphemousSkinIdle.ValuesHash, BlasphemousSkinIdle.InputHash) = saved;
        }
    }
}
