// imgui-no-layout (kind Optimize, opt-in). Unity sends every MonoBehaviour that has an OnGUI a Layout event before each
// of its other GUI events (Repaint once a frame at least) while its useGUILayout is true (the default), and builds
// GUILayout's groups for it (GUILayoutUtility.Begin: GUILayoutGroup, RectOffset and List objects every frame). A script
// that draws with GUI.* only, or nothing at all in normal play (Blasphemous's Core.OnGUI calls its systems' debug
// GUIs, all off; DamagedFlash draws the hurt flash with GUI.DrawTexture), pays for that and makes the garbage
// (Blasphemous on the G350: about a fifth of its managed garbage in play, after smartcolliders-raycast).
// useGUILayout = false is Unity's own switch for such scripts: no Layout event, no layout groups; GUILayout must
// then not be used by it. Patched: for each MonoBehaviour of the game's assemblies that declares OnGUI, when nothing
// its OnGUI can reach uses GUILayout (its calls followed through the game's assemblies, a virtual call into every
// override there; any GUILayout* or GUILayoutUtility member, or a call this walk cannot follow into a game assembly,
// leaves that script as it is), OnGUI first sets this.useGUILayout = false. From the next frame on the script gets
// its Repaint and input events as before, without the Layout one: what it draws is the same.
using Mono.Cecil;
using Mono.Cecil.Cil;
using Mono.Cecil.Rocks;

namespace AsmPatch;

sealed class ImguiNoLayout : Patch
{
    public override string Name => "imgui-no-layout";
    public override Kind Kind => Kind.Optimize;
    public override bool OptIn => true;
    public override string Summary =>
        "MonoBehaviours whose OnGUI can reach no GUILayout call set useGUILayout = false (Unity's switch): no Layout event and no layout groups every frame (less garbage, the same drawing)";

    public override Result Run(Game game, bool apply)
    {
        var asms = game.GameAssemblies.ToList();
        var gameNames = new HashSet<string>(asms.Select(a => a.Name.Name));
        var types = asms.SelectMany(a => Cecil.AllTypes(a.MainModule)).ToList();
        bool IsBehaviour(TypeDefinition t)
        {
            for (var b = t; b != null; )
            {
                if (b.FullName == "UnityEngine.MonoBehaviour") return true;
                if (b.BaseType == null) return false;
                try { b = b.BaseType.Resolve(); } catch { return false; }
            }
            return false;
        }
        bool IsSubOf(TypeDefinition t, TypeDefinition baseT)
        {
            for (var b = t; b != null; )
            {
                if (b == baseT) return true;
                if (b.BaseType == null) return false;
                try { b = b.BaseType.Resolve(); } catch { return false; }
            }
            return false;
        }
        static TypeDefinition SafeBase(TypeDefinition t) { try { return t.BaseType?.Resolve(); } catch { return null; } }
        static bool IsLayout(MemberReference r) =>
            r.DeclaringType != null && r.DeclaringType.Namespace == "UnityEngine" &&
            (r.DeclaringType.Name.StartsWith("GUILayout") || r.DeclaringType.Name == "GUILayoutUtility");
        // what OnGUI can reach: null when it can reach GUILayout or a call the walk cannot follow
        string Reach(MethodDefinition start)
        {
            var seen = new HashSet<MethodDefinition>();
            var work = new Stack<MethodDefinition>();
            work.Push(start);
            while (work.Count > 0)
            {
                var md = work.Pop();
                if (!seen.Add(md)) continue;
                if (seen.Count > 20000) return "too many methods";
                if (!md.HasBody) continue;
                foreach (var i in md.Body.Instructions)
                {
                    if (i.Operand is MemberReference mr && IsLayout(mr)) return $"{md.DeclaringType.Name}.{md.Name} uses {mr.DeclaringType.Name}";
                    if (i.Operand is TypeReference tr && tr.Namespace == "UnityEngine" && tr.Name.StartsWith("GUILayout")) return $"{md.DeclaringType.Name}.{md.Name} names {tr.Name}";
                    if (i.Operand is not MethodReference r) continue;
                    // a delegate's Invoke (an event's subscribers) or an interface call: who runs is not known here
                    TypeDefinition dt = null;
                    try { dt = r.DeclaringType.Resolve(); } catch { }
                    if (r.Name is "Invoke" or "BeginInvoke" or "DynamicInvoke" && dt?.BaseType?.FullName is "System.MulticastDelegate" or "System.Delegate")
                        return $"{md.DeclaringType.Name}.{md.Name} invokes a delegate";
                    if (dt != null && dt.IsInterface && i.OpCode.Code == Code.Callvirt)
                    {
                        // every implementation in the game's assemblies (an interface of the class libraries or Unity's:
                        // their own implementations never reach GUILayout, the game's are followed like any other)
                        foreach (var t in types)
                            if (t.Interfaces.Any(x => x.InterfaceType.FullName == dt.FullName) || types.Any(_ => false))
                                foreach (var o in t.Methods.Where(x => (x.Name == r.Name || x.Name.EndsWith("." + r.Name)) && x.Parameters.Count == r.Parameters.Count))
                                    work.Push(o);
                        // implementations inherited from a base class that does not name the interface
                        foreach (var t in types.Where(t => t.Interfaces.Any(x => x.InterfaceType.FullName == dt.FullName)))
                            for (var b = SafeBase(t); b != null; b = SafeBase(b))
                                foreach (var o in b.Methods.Where(x => x.Name == r.Name && x.Parameters.Count == r.Parameters.Count))
                                    work.Push(o);
                        continue;
                    }
                    if (r.Name is "SendMessage" or "SendMessageUpwards" or "BroadcastMessage" or "Invoke" or "InvokeRepeating" or "StartCoroutine" && r.DeclaringType.Namespace == "UnityEngine")
                        return $"{md.DeclaringType.Name}.{md.Name} calls {r.Name}";
                    var scope = r.DeclaringType.Scope?.Name?.Replace(".dll", "");
                    if (scope == null || !gameNames.Contains(scope)) continue;      // the class libraries, Unity's own
                    MethodDefinition d;
                    try { d = r.Resolve(); } catch { return $"{md.Name}: cannot follow {r.FullName}"; }
                    if (d == null) return $"{md.Name}: cannot follow {r.FullName}";
                    work.Push(d);
                    if (i.OpCode.Code == Code.Callvirt && d.IsVirtual)
                        foreach (var t in types)
                            if (t != d.DeclaringType && IsSubOf(t, d.DeclaringType))
                                foreach (var o in t.Methods.Where(x => x.Name == d.Name && x.IsVirtual && x.Parameters.Count == d.Parameters.Count))
                                    work.Push(o);
                    if (i.OpCode.Code == Code.Ldftn || i.OpCode.Code == Code.Ldvirtftn) work.Push(d);
                }
            }
            return null;
        }
        var r = new Result();
        var targets = new List<(AssemblyDefinition asm, MethodDefinition onGui)>();
        foreach (var a in asms)
            foreach (var t in Cecil.AllTypes(a.MainModule).Where(t => t.IsClass && !t.IsAbstract || t.IsClass))
            {
                var og = t.Methods.FirstOrDefault(m => m.Name == "OnGUI" && !m.IsStatic && m.Parameters.Count == 0 && m.HasBody);
                if (og == null || !IsBehaviour(t)) continue;
                if (og.Body.Instructions.Take(3).Any(i => i.Operand is MethodReference mr && mr.Name == "set_useGUILayout"))
                { r.Details.Add($"{game.FileOf(a)}: {t.FullName}.OnGUI already sets useGUILayout"); continue; }
                var why = Reach(og);
                if (why != null) { r.Details.Add($"{game.FileOf(a)}: {t.FullName}: left as it is ({why})"); continue; }
                targets.Add((a, og));
                r.Details.Add($"{game.FileOf(a)}: {t.FullName}.OnGUI: useGUILayout = false");
            }
        if (targets.Count == 0)
        {
            if (!r.Details.Any(d => d.Contains("already sets"))) return Result.NotApplicable("no MonoBehaviour OnGUI without GUILayout");
            r.Outcome = Outcome.AlreadyApplied; return r;
        }
        r.Outcome = Outcome.Applied;
        if (!apply) return r;
        foreach (var (a, og) in targets)
        {
            var set = BlasphemousCatalogs.Lib(game, a.MainModule, "UnityEngine.MonoBehaviour", "set_useGUILayout", "System.Boolean");
            if (set == null) return Result.Failed("no MonoBehaviour.set_useGUILayout(bool)");
            var b = og.Body; b.SimplifyMacros();
            var il = b.GetILProcessor();
            var first = b.Instructions[0];
            il.InsertBefore(first, il.Create(OpCodes.Ldarg_0));
            il.InsertBefore(first, il.Create(OpCodes.Ldc_I4_0));
            il.InsertBefore(first, il.Create(OpCodes.Call, set));
            b.OptimizeMacros();
            game.MarkChanged(a);
        }
        return r;
    }
}
