// inline (optimize, exact, opt-in): small methods copied into the game's hottest callers, where Unity 2017.x's
// Mono 2.6 JIT keeps them as calls. Its inliner takes only leaf methods of at most 20 bytes of IL with no throw,
// no call inside and few branches (MONO_INLINELIMIT raises the size alone and measured no gain), so a hot loop
// pays a call, a frame and its spills for List<T>.this[i], UnityEngine.Object's `if (obj)`, Mathf.Abs or a
// Vector2 operator. The callers and callees come from the port's list (`--inline-list=<file>`, else
// $ASMPATCH_INLINE_LIST, else files/inline.txt beside asmpatch, i.e. the setup's files/):
//   caller <Type>::<Method>          every overload of that method in the game's assemblies (from a profile)
//   callee <Type>::<Method>|<Type>::*  calls to these are copied in (also inside copied bodies, 3 deep)
//   maxil <bytes>                    the largest callee body copied (default 64)
// Exact: Mono 2.6 evaluates float32 as float64 and rounds where a value is stored or passed, so a float32
// argument is stored with conv.r4 and a float32 result gets conv.r4, as the call did; `this` of a callvirt is
// null-checked unless the copied body loads one of its fields first (the same NullReferenceException);
// the callee's locals are zeroed when it says .locals init; its own checks and throws are copied with it
// (List<T>.this[i]'s ArgumentOutOfRangeException). Left as calls: virtual targets, generic methods, bodies
// with exception handlers, calli/ldftn/localloc/jmp/tail, a static field of a class with a static constructor
// (its run would move), a class with a precise .cctor, members the caller may not see.
// Copied bodies use the long instruction forms: a short ldloca.s / br.s from a small callee would overflow in
// a large caller (a check build found wrong results from exactly that).
// Private fields a body reads in another assembly (mscorlib's List`1._items/_size, the engine's
// UnityEngine.Object.m_CachedPtr) become internal and that assembly gets [InternalsVisibleTo] for the caller's:
// Mono 2.6 refuses the read otherwise (FieldAccessException). Reflection's public view and Unity's
// serialization (public or [SerializeField]) see the same fields as before. So this runs on the player's own
// mscorlib and engine modules (the ones the setup packs), like unity-object-null.
// inline-check (opt-in): the same, and each copied call with a result also calls the original and compares
// the two bit for bit ("[inline] check ..." on stderr: mismatches, and a count every 200,000): the device test
// of exactness for a port's list (README.md).
using Mono.Cecil;
using Mono.Cecil.Cil;
using Mono.Cecil.Rocks;

namespace AsmPatch;

sealed class InlineHot : Patch
{
    readonly bool check;
    public InlineHot(bool check) { this.check = check; }
    public override string Name => check ? "inline-check" : "inline";
    public override Kind Kind => Kind.Optimize;
    public override bool OptIn => true;
    public override string Summary => check
        ? "inline, each copied call also compared with the original (a check build: logs [inline] check ...)"
        : "small callees (List<T>[i], Unity Object tests, Mathf, Vector2) copied into the port's listed hot callers (the same results)";

    const string Marker = "<asmpatch>Inline";
    public static string ListPath;                         // --inline-list=

    sealed class Spec { public List<(string T, string M)> Callers = new(), Callees = new(); public int MaxIl = 64, Depth = 3; public string File; }

    static Spec ReadSpec()
    {
        var f = ListPath ?? Environment.GetEnvironmentVariable("ASMPATCH_INLINE_LIST");
        if (string.IsNullOrEmpty(f)) f = Path.Combine(AppContext.BaseDirectory, "files", "inline.txt");
        if (!File.Exists(f)) return null;
        var s = new Spec { File = f };
        foreach (var raw in File.ReadAllLines(f))
        {
            var line = raw.Split('#')[0].Trim(); if (line.Length == 0) continue;
            var p = line.Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
            if (p.Length != 2) throw new FormatException($"{f}: {raw}");
            if (p[0] == "maxil") { s.MaxIl = int.Parse(p[1]); continue; }
            if (p[0] == "depth") { s.Depth = int.Parse(p[1]); continue; }
            var tm = p[1].Split(new[] { "::" }, StringSplitOptions.None);
            if (tm.Length != 2 || (p[0] != "caller" && p[0] != "callee")) throw new FormatException($"{f}: {raw}");
            (p[0] == "caller" ? s.Callers : s.Callees).Add((tm[0], tm[1]));
        }
        return s;
    }

    public override Result Run(Game game, bool apply)
    {
        Spec spec;
        try { spec = ReadSpec(); } catch (FormatException e) { return Result.Failed("the list: " + e.Message); }
        if (spec == null) return Result.NotApplicable("no list (--inline-list=, ASMPATCH_INLINE_LIST or files/inline.txt beside asmpatch)");
        var callers = new List<MethodDefinition>();
        foreach (var a in game.GameAssemblies)
            foreach (var t in Cecil.AllTypes(a.MainModule))
                foreach (var m in t.Methods)
                    if (m.HasBody && spec.Callers.Any(c => c.T == t.FullName && c.M == m.Name)) callers.Add(m);
        if (callers.Count == 0) return Result.NotApplicable($"none of the list's callers in the game's assemblies ({spec.File})");
        var asms = callers.Select(m => m.Module.Assembly).Distinct().ToList();
        if (asms.Any(a => a.MainModule.Types.Any(t => t.Name == Marker)))
            return new Result { Outcome = Outcome.AlreadyApplied, Details = { $"{string.Join(", ", asms.Select(game.FileOf))}: inlined already" } };
        var w = new Worker(game, spec, check, apply);
        foreach (var m in callers) w.Caller(m);
        if (w.Sites == 0) return Result.NotApplicable("no call of a listed callee in the listed callers that can be copied" + w.SkipText());
        var r = new Result { Outcome = Outcome.Applied };
        foreach (var kv in w.PerCaller) r.Details.Add($"{game.FileOf(kv.Key.Module.Assembly)}: {kv.Key.DeclaringType.FullName}::{kv.Key.Name}: {kv.Value} calls copied in");
        foreach (var kv in w.Friends) r.Details.Add($"{game.FileOf(kv.Key)}: {string.Join(", ", kv.Value.Select(f => f.DeclaringType.Name + "." + f.Name).Distinct())} internal, internals visible to {string.Join(", ", w.FriendOf[kv.Key])}");
        if (check) { r.Details.Add($"check build: {w.Checked} copied calls compared with the original at run time"); foreach (var n in w.SiteNames) r.Details.Add("check site " + n); }
        var skips = w.SkipText(); if (skips.Length > 0) r.Details.Add("left as calls" + skips);
        if (!apply) return r;
        foreach (var a in asms)
        {
            a.MainModule.Types.Add(new TypeDefinition("", Marker, TypeAttributes.NotPublic | TypeAttributes.Abstract | TypeAttributes.Sealed, a.MainModule.TypeSystem.Object));
            game.MarkChanged(a);
        }
        foreach (var kv in w.Friends) game.MarkChanged(kv.Key);
        return r;
    }

    sealed class Worker
    {
        readonly Game game; readonly Spec spec; readonly bool check, apply;
        public int Sites, Checked;
        public readonly List<string> SiteNames = new();
        public readonly Dictionary<MethodDefinition, int> PerCaller = new();
        public readonly Dictionary<AssemblyDefinition, List<FieldDefinition>> Friends = new();
        public readonly Dictionary<AssemblyDefinition, SortedSet<string>> FriendOf = new();
        readonly SortedDictionary<string, int> skips = new(StringComparer.Ordinal);
        readonly HashSet<Instruction> originals = new();
        readonly Dictionary<ModuleDefinition, MethodDefinition> reporters = new();
        int siteId;

        public Worker(Game g, Spec s, bool c, bool a) { game = g; spec = s; check = c; apply = a; }

        public string SkipText() => skips.Count == 0 ? "" : ": " + string.Join("; ", skips.Select(kv => $"{kv.Key} x{kv.Value}"));

        bool Listed(MethodReference r) =>
            spec.Callees.Any(c => c.T == r.DeclaringType.GetElementType().FullName && (c.M == "*" || c.M == r.Name));

        public void Caller(MethodDefinition m)
        {
            // dry run (--list): count on a copy of the decisions only
            m.Body.SimplifyMacros();
            int before = Sites;
            Inline(m, 0, null);
            m.Body.OptimizeMacros();
            if (Sites > before) PerCaller[m] = Sites - before;
        }

        static bool Friend(AssemblyDefinition owner, string me) =>
            owner.CustomAttributes.Any(ca => ca.AttributeType.Name == "InternalsVisibleToAttribute" && ca.ConstructorArguments.Count == 1 &&
                                             ((string)ca.ConstructorArguments[0].Value).Split(',')[0].Trim() == me);

        static bool Inside(TypeDefinition from, TypeDefinition t) { for (var x = from; x != null; x = x.DeclaringType) if (x == t) return true; return false; }

        bool TypeVisible(TypeDefinition t, TypeDefinition from, string me)
        {
            for (var x = t; x != null; x = x.DeclaringType)
            {
                if (Inside(from, x)) continue;
                bool sameOrFriend = x.Module.Assembly.Name.Name == me || Friend(x.Module.Assembly, me);
                if (x.IsNested) { if (!(x.IsNestedPublic || ((x.IsNestedAssembly || x.IsNestedFamilyOrAssembly) && sameOrFriend))) return false; }
                else if (!x.IsPublic && !sameOrFriend) return false;
            }
            return true;
        }

        bool MemberVisible(TypeDefinition decl, bool pub, bool asm, bool famOrAsm, TypeDefinition from, string me) =>
            TypeVisible(decl, from, me) && (pub || Inside(from, decl) ||
                ((asm || famOrAsm) && (decl.Module.Assembly.Name.Name == me || Friend(decl.Module.Assembly, me))));

        // why md cannot be copied into caller (null: it can); friendFields: private fields of other assemblies it needs
        string WhyNot(MethodDefinition md, MethodReference site, MethodDefinition caller, OpCode op, List<FieldDefinition> friendFields)
        {
            var from = caller.DeclaringType; var me = caller.Module.Assembly.Name.Name;
            if (!md.HasBody) return "no body";
            if (md.HasGenericParameters || site is GenericInstanceMethod) return "generic method";
            if (md.Body.CodeSize > spec.MaxIl) return "too big";
            if (md.Body.HasExceptionHandlers) return "exception handlers";
            if (op == OpCodes.Callvirt && md.IsVirtual && !md.IsFinal && !md.DeclaringType.IsSealed) return "virtual";
            if (md.NoInlining || md.IsPInvokeImpl || md.IsInternalCall || md.IsRuntime || md.IsSynchronized) return "noinline, icall or synchronized";
            var dt = md.DeclaringType;
            bool hasCctor(TypeDefinition t) => t.Methods.Any(x => x.IsConstructor && x.IsStatic);
            if (hasCctor(dt) && !dt.IsBeforeFieldInit) return "precise .cctor";
            if (!MemberVisible(dt, md.IsPublic, md.IsAssembly, md.IsFamilyOrAssembly, from, me)) return "not visible";
            foreach (var i in md.Body.Instructions)
            {
                var c = i.OpCode.Code;
                if (c is Code.Localloc or Code.Jmp or Code.Arglist or Code.Tail or Code.Ldftn or Code.Ldvirtftn or Code.Calli or Code.Rethrow) return "opcode " + i.OpCode.Name;
                if (i.Operand is FieldReference fr)
                {
                    var fd = Cecil.TryResolve(fr.DeclaringType)?.Fields.FirstOrDefault(f => f.Name == fr.Name);
                    if (fd == null) return "unresolved field";
                    if (fd.IsStatic && hasCctor(fd.DeclaringType)) return "a static field of a class with a .cctor";
                    if (!MemberVisible(fd.DeclaringType, fd.IsPublic, fd.IsAssembly, fd.IsFamilyOrAssembly, from, me))
                    {
                        if (fd.IsPrivate && TypeVisible(fd.DeclaringType, from, me) && fd.Module.Assembly != caller.Module.Assembly) friendFields.Add(fd);
                        else return "a field it may not read";
                    }
                }
                else if (i.Operand is MethodReference mr)
                {
                    MethodDefinition d2; try { d2 = mr.Resolve(); } catch (AssemblyResolutionException) { d2 = null; }
                    if (d2 == null) return "unresolved method";
                    // a generic method instantiated on the callee's own type parameters (e.g. Array.Empty<T>() in the 4.x
                    // class libraries' List<T>): SubMethod substitutes the declaring type's arguments only, and importing
                    // the call as it is fails (Cecil: no generic context; Enter the Gungeon on the 64-bit route, 2026-11-21)
                    if (mr is GenericInstanceMethod gim && gim.GenericArguments.Any(a => a.ContainsGenericParameter))
                        return "a generic method call on its type's parameters";
                    if (!MemberVisible(d2.DeclaringType, d2.IsPublic, d2.IsAssembly, d2.IsFamilyOrAssembly, from, me)) return "a method it may not call";
                }
                else if (i.Operand is TypeReference tr && tr is not GenericParameter)
                {
                    var td = Cecil.TryResolve(tr is TypeSpecification ts ? ts.GetElementType() : tr);
                    if (td == null && tr.GetElementType() is not GenericParameter) return "unresolved type";
                    if (td != null && !TypeVisible(td, from, me)) return "a type it may not name";
                }
            }
            return null;
        }

        // the callee's declaring type's generic parameters -> the call site's arguments
        static TypeReference Sub(TypeReference t, GenericInstanceType inst)
        {
            if (inst == null) return t;
            switch (t)
            {
                case GenericParameter gp when gp.Type == GenericParameterType.Type: return inst.GenericArguments[gp.Position];
                case ArrayType at: return new ArrayType(Sub(at.ElementType, inst), at.Rank);
                case ByReferenceType br: return new ByReferenceType(Sub(br.ElementType, inst));
                case PointerType pt: return new PointerType(Sub(pt.ElementType, inst));
                case PinnedType pn: return new PinnedType(Sub(pn.ElementType, inst));
                case GenericInstanceType git:
                    {
                        var n = new GenericInstanceType(git.ElementType);
                        foreach (var a in git.GenericArguments) n.GenericArguments.Add(Sub(a, inst));
                        return n;
                    }
                default: return t;
            }
        }

        static TypeReference DeclOf(TypeReference decl, GenericInstanceType inst)
        {
            if (inst == null) return decl;
            if (decl is TypeDefinition td && td.HasGenericParameters) return inst;
            if (decl is GenericInstanceType g && g.GenericArguments.Any(a => a is GenericParameter)) return Sub(g, inst);
            return decl;
        }

        static FieldReference SubField(ModuleDefinition M, FieldReference fr, GenericInstanceType inst)
        {
            // (the reference's own field type: a resolved definition's would name the assembly that defines it,
            // e.g. a facade's target, which the game may not reference)
            return M.ImportReference(new FieldReference(fr.Name, fr.FieldType, DeclOf(fr.DeclaringType, inst)));
        }

        static MethodReference SubMethod(ModuleDefinition M, MethodReference mr, GenericInstanceType inst)
        {
            if (inst == null) return M.ImportReference(mr);
            var decl = DeclOf(mr.DeclaringType, inst);
            if (ReferenceEquals(decl, mr.DeclaringType)) return M.ImportReference(mr);
            var n = new MethodReference(mr.Name, mr.ReturnType, decl) { HasThis = mr.HasThis, ExplicitThis = mr.ExplicitThis, CallingConvention = mr.CallingConvention };
            foreach (var p in mr.Parameters) n.Parameters.Add(new ParameterDefinition(p.ParameterType));
            return M.ImportReference(n);
        }

        static bool IsR4(TypeReference t) => t.MetadataType == MetadataType.Single;

        static int ArgIndex(Instruction i, MethodDefinition md) => i.OpCode.Code switch
        {
            Code.Ldarg_0 => 0, Code.Ldarg_1 => 1, Code.Ldarg_2 => 2, Code.Ldarg_3 => 3,
            _ => i.Operand is ParameterDefinition pd ? pd.Index + (md.HasThis ? 1 : 0) : -1,
        };

        static bool PureLoad(Instruction i)
        {
            var c = i.OpCode.Code;
            return c is >= Code.Ldarg_0 and <= Code.Ldarg_3 or Code.Ldarg_S or Code.Ldarg or >= Code.Ldloc_0 and <= Code.Ldloc_3 or
                   Code.Ldloc_S or Code.Ldloc or >= Code.Ldc_I4_M1 and <= Code.Ldc_R8 or Code.Ldnull or Code.Nop;
        }

        // does the body's first use of `this` dereference it before anything with an effect?
        static bool DerefsThisFirst(MethodDefinition md)
        {
            foreach (var i in md.Body.Instructions)
            {
                bool isThis = i.OpCode.Code is Code.Ldarg_0 || (i.OpCode.Code is Code.Ldarg or Code.Ldarg_S && ArgIndex(i, md) == 0);
                if (isThis) return i.Next != null && i.Next.OpCode.Code is Code.Ldfld or Code.Ldflda;
                if (!PureLoad(i)) return false;
            }
            return false;
        }

        void Skip(string why) => skips[why] = skips.GetValueOrDefault(why) + 1;

        void Inline(MethodDefinition caller, int depth, HashSet<Instruction> only)
        {
            var body = caller.Body; var il = body.GetILProcessor(); var M = caller.Module;
            foreach (var call in body.Instructions.ToList())
            {
                if (only != null && !only.Contains(call)) continue;
                if (call.OpCode != OpCodes.Call && call.OpCode != OpCodes.Callvirt) continue;
                if (originals.Contains(call) || call.Operand is not MethodReference site || !Listed(site)) continue;
                var key = site.DeclaringType.GetElementType().FullName + "::" + site.Name;
                MethodDefinition md; try { md = site.Resolve(); } catch (AssemblyResolutionException) { md = null; }
                if (md == null) { Skip(key + ": unresolved"); continue; }
                var friendFields = new List<FieldDefinition>();
                var why = WhyNot(md, site, caller, call.OpCode, friendFields);
                if (why != null) { Skip(key + ": " + why); continue; }
                Sites++;
                foreach (var f in friendFields)
                {
                    var owner = f.Module.Assembly;
                    if (!Friends.TryGetValue(owner, out var l)) { Friends[owner] = l = new List<FieldDefinition>(); FriendOf[owner] = new SortedSet<string>(StringComparer.Ordinal); }
                    l.Add(f); FriendOf[owner].Add(caller.Module.Assembly.Name.Name);
                }
                if (!apply) continue;
                MakeFriends(friendFields, caller.Module.Assembly.Name.Name);
                var inst = site.DeclaringType as GenericInstanceType;
                var cb = md.Body;
                cb.SimplifyMacros();
                var argTypes = new List<TypeReference>();
                if (md.HasThis)
                {
                    var tt = DeclOf(md.DeclaringType, inst);
                    argTypes.Add(M.ImportReference(md.DeclaringType.IsValueType ? new ByReferenceType(tt) : tt));
                }
                foreach (var p in md.Parameters) argTypes.Add(M.ImportReference(Sub(p.ParameterType, inst)));
                var argLocals = argTypes.Select(t => { var v = new VariableDefinition(t); body.Variables.Add(v); return v; }).ToList();
                var locals = cb.Variables.Select(v => { var nv = new VariableDefinition(M.ImportReference(Sub(v.VariableType, inst))); body.Variables.Add(nv); return nv; }).ToList();
                var seq = new List<Instruction>();
                for (int k = argLocals.Count - 1; k >= 0; k--)
                {
                    if (IsR4(argTypes[k])) seq.Add(il.Create(OpCodes.Conv_R4));
                    seq.Add(il.Create(OpCodes.Stloc, argLocals[k]));
                }
                if (md.HasThis && !md.DeclaringType.IsValueType && call.OpCode == OpCodes.Callvirt && !DerefsThisFirst(md))
                {
                    var ok = il.Create(OpCodes.Nop);
                    seq.Add(il.Create(OpCodes.Ldloc, argLocals[0])); seq.Add(il.Create(OpCodes.Brtrue, ok));
                    seq.Add(il.Create(OpCodes.Ldnull)); seq.Add(il.Create(OpCodes.Throw)); seq.Add(ok);
                }
                if (cb.InitLocals)
                    foreach (var v in locals)
                    {
                        if (v.VariableType.IsValueType || v.VariableType.IsGenericParameter) { seq.Add(il.Create(OpCodes.Ldloca, v)); seq.Add(il.Create(OpCodes.Initobj, v.VariableType)); }
                        else { seq.Add(il.Create(OpCodes.Ldnull)); seq.Add(il.Create(OpCodes.Stloc, v)); }
                    }
                var end = il.Create(OpCodes.Nop);
                var map = new Dictionary<Instruction, Instruction>(); var src = cb.Instructions.ToList();
                foreach (var i in src)
                {
                    Instruction n; int ai = ArgIndex(i, md); var c = i.OpCode.Code;
                    if (ai >= 0 && c is Code.Ldarg_0 or Code.Ldarg_1 or Code.Ldarg_2 or Code.Ldarg_3 or Code.Ldarg or Code.Ldarg_S) n = il.Create(OpCodes.Ldloc, argLocals[ai]);
                    else if (c is Code.Ldarga or Code.Ldarga_S) n = il.Create(OpCodes.Ldloca, argLocals[ai]);
                    else if (c is Code.Starg or Code.Starg_S) n = il.Create(OpCodes.Stloc, argLocals[ai]);
                    else if (c is >= Code.Ldloc_0 and <= Code.Ldloc_3) n = il.Create(OpCodes.Ldloc, locals[c - Code.Ldloc_0]);
                    else if (c is >= Code.Stloc_0 and <= Code.Stloc_3) n = il.Create(OpCodes.Stloc, locals[c - Code.Stloc_0]);
                    else if (i.Operand is VariableDefinition vd) n = il.Create(i.OpCode, locals[vd.Index]);
                    else if (c == Code.Ret) n = il.Create(OpCodes.Br, end);
                    else if (i.Operand is FieldReference fr) n = il.Create(i.OpCode, SubField(M, fr, inst));
                    else if (i.Operand is MethodReference mr) n = il.Create(i.OpCode, SubMethod(M, mr, inst));
                    else if (i.Operand is TypeReference tr) n = il.Create(i.OpCode, M.ImportReference(Sub(tr, inst)));
                    else if (i.Operand is Instruction or Instruction[]) n = il.Create(OpCodes.Nop);
                    else n = i.Operand switch
                    {
                        null => il.Create(i.OpCode), sbyte x => il.Create(i.OpCode, x), byte x => il.Create(i.OpCode, x), int x => il.Create(i.OpCode, x),
                        long x => il.Create(i.OpCode, x), float x => il.Create(i.OpCode, x), double x => il.Create(i.OpCode, x), string x => il.Create(i.OpCode, x),
                        _ => throw new NotSupportedException($"{md.FullName}: operand {i.Operand.GetType().Name}"),
                    };
                    map[i] = n;
                }
                foreach (var i in src)
                {
                    if (i.Operand is Instruction t) { map[i].OpCode = i.OpCode; map[i].Operand = map[t]; }
                    else if (i.Operand is Instruction[] ts) { map[i].OpCode = i.OpCode; map[i].Operand = ts.Select(x => map[x]).ToArray(); }
                }
                cb.OptimizeMacros();
                seq.AddRange(src.Select(i => map[i])); seq.Add(end);
                if (IsR4(md.ReturnType)) seq.Add(il.Create(OpCodes.Conv_R4));
                if (check && md.ReturnType.MetadataType != MetadataType.Void) seq.AddRange(CheckCode(caller, il, call, site, md, inst, argLocals));
                // the call instruction becomes the sequence's first (branches and handler bounds naming it stay right)
                var first = seq[0]; call.OpCode = first.OpCode; call.Operand = first.Operand;
                foreach (var x in seq)
                {
                    if (x.Operand == first) x.Operand = call;
                    if (x.Operand is Instruction[] arr) for (int q = 0; q < arr.Length; q++) if (arr[q] == first) arr[q] = call;
                }
                var prev = call; foreach (var x in seq.Skip(1)) { il.InsertAfter(prev, x); prev = x; }
                if (depth + 1 < spec.Depth) Inline(caller, depth + 1, new HashSet<Instruction>(seq.Skip(1).Append(call)));
            }
        }

        void MakeFriends(List<FieldDefinition> fields, string me)
        {
            foreach (var f in fields)
            {
                if (f.IsPrivate) f.Attributes = (f.Attributes & ~FieldAttributes.FieldAccessMask) | FieldAttributes.Assembly;
                var owner = f.Module.Assembly;
                if (Friend(owner, me)) continue;
                var mm = owner.MainModule;
                var ivt = Cecil.AllTypes(mm).FirstOrDefault(t => t.FullName == "System.Runtime.CompilerServices.InternalsVisibleToAttribute");
                MethodReference ctor = ivt != null
                    ? ivt.Methods.First(m => m.IsConstructor && !m.IsStatic && m.Parameters.Count == 1)
                    : mm.ImportReference(new MethodReference(".ctor", mm.TypeSystem.Void,
                        new TypeReference("System.Runtime.CompilerServices", "InternalsVisibleToAttribute", mm, mm.TypeSystem.CoreLibrary)) { HasThis = true, Parameters = { new ParameterDefinition(mm.TypeSystem.String) } });
                var ca = new CustomAttribute(ctor); ca.ConstructorArguments.Add(new CustomAttributeArgument(mm.TypeSystem.String, me));
                owner.CustomAttributes.Add(ca);
            }
        }

        // check build: result -> r1; the original call with the same arguments -> r2; compare bit for bit, report
        IEnumerable<Instruction> CheckCode(MethodDefinition caller, ILProcessor il, Instruction call, MethodReference site, MethodDefinition md, GenericInstanceType inst, List<VariableDefinition> argLocals)
        {
            var M = caller.Module; var body = caller.Body;
            var rt = M.ImportReference(Sub(md.ReturnType, inst));
            var r1 = new VariableDefinition(rt); var r2 = new VariableDefinition(rt); body.Variables.Add(r1); body.Variables.Add(r2);
            var s = new List<Instruction> { il.Create(OpCodes.Stloc, r1) };
            foreach (var v in argLocals) s.Add(il.Create(OpCodes.Ldloc, v));
            var orig = il.Create(call.OpCode, site); originals.Add(orig); s.Add(orig); s.Add(il.Create(OpCodes.Stloc, r2));
            var same = Compare(M, il, r1, r2, rt);
            if (same == null) { Skip($"{site.DeclaringType.GetElementType().FullName}::{site.Name}: check: no bit comparison for {rt.FullName}"); }
            else
            {
                s.AddRange(same);
                SiteNames.Add($"{siteId}: {caller.DeclaringType.FullName}::{caller.Name} -> {site.DeclaringType.GetElementType().FullName}::{site.Name}");
                s.Add(il.Create(OpCodes.Ldc_I4, siteId++)); s.Add(il.Create(OpCodes.Call, Reporter(M)));
                Checked++;
            }
            s.Add(il.Create(OpCodes.Ldloc, r1));
            return s;
        }

        // instructions that push 1 when r1 and r2 hold the same bits (fields of a struct one by one), else null
        static List<Instruction> Compare(ModuleDefinition M, ILProcessor il, VariableDefinition r1, VariableDefinition r2, TypeReference t)
        {
            var leaves = new List<(List<FieldReference> Path, TypeReference T)>();
            if (!Leaves(M, t, new List<FieldReference>(), leaves, 0)) return null;
            var s = new List<Instruction>(); bool firstLeaf = true;
            foreach (var (path, lt) in leaves)
            {
                foreach (var r in new[] { r1, r2 })
                {
                    if (path.Count == 0 && !IsBits(lt)) { s.Add(il.Create(OpCodes.Ldloc, r)); continue; }
                    s.Add(il.Create(OpCodes.Ldloca, r));
                    foreach (var f in path) s.Add(il.Create(OpCodes.Ldflda, f));
                    s.Add(il.Create(Load(lt)));
                }
                s.Add(il.Create(OpCodes.Ceq));
                if (!firstLeaf) s.Add(il.Create(OpCodes.And));
                firstLeaf = false;
            }
            if (firstLeaf) s.Add(il.Create(OpCodes.Ldc_I4_1));
            return s;
        }

        static bool IsBits(TypeReference t) => t.MetadataType is MetadataType.Single or MetadataType.Double;

        static OpCode Load(TypeReference t) => t.MetadataType switch
        {
            MetadataType.Single or MetadataType.Int32 or MetadataType.UInt32 => OpCodes.Ldind_I4,
            MetadataType.Double or MetadataType.Int64 or MetadataType.UInt64 => OpCodes.Ldind_I8,
            MetadataType.Boolean or MetadataType.Byte => OpCodes.Ldind_U1, MetadataType.SByte => OpCodes.Ldind_I1,
            MetadataType.Int16 => OpCodes.Ldind_I2, MetadataType.UInt16 or MetadataType.Char => OpCodes.Ldind_U2,
            MetadataType.IntPtr or MetadataType.UIntPtr => OpCodes.Ldind_I,
            _ => OpCodes.Ldind_Ref,
        };

        static bool Leaves(ModuleDefinition M, TypeReference t, List<FieldReference> path, List<(List<FieldReference>, TypeReference)> outp, int depth)
        {
            if (depth > 4) return false;
            if (t.IsPrimitive || !t.IsValueType) { outp.Add((new List<FieldReference>(path), t)); return true; }   // (a reference: by identity)
            var td = Cecil.TryResolve(t); if (td == null || td.IsEnum) { if (td?.IsEnum == true) { outp.Add((new List<FieldReference>(path), td.GetEnumUnderlyingType())); return true; } return false; }
            if (t is GenericInstanceType) return false;
            foreach (var f in td.Fields.Where(f => !f.IsStatic))
            {
                path.Add(M.ImportReference(f));
                bool ok = Leaves(M, f.FieldType, path, outp, depth + 1);
                path.RemoveAt(path.Count - 1);
                if (!ok) return false;
            }
            return true;
        }

        // <asmpatch>InlineCheck.Report(bool same, int site): counts; logs a mismatch (the first 5 per site, 200 in all) and a total every 200,000
        MethodDefinition Reporter(ModuleDefinition M)
        {
            if (reporters.TryGetValue(M, out var r)) return r;
            var ts = M.TypeSystem;
            var t = new TypeDefinition("", "<asmpatch>InlineCheck", TypeAttributes.NotPublic | TypeAttributes.Abstract | TypeAttributes.Sealed | TypeAttributes.BeforeFieldInit, ts.Object);
            M.Types.Add(t);
            var n = new FieldDefinition("n", FieldAttributes.Static | FieldAttributes.Private, ts.Int64); t.Fields.Add(n);
            var bad = new FieldDefinition("bad", FieldAttributes.Static | FieldAttributes.Private, ts.Int32); t.Fields.Add(bad);
            r = new MethodDefinition("Report", MethodAttributes.Static | MethodAttributes.Assembly | MethodAttributes.HideBySig, ts.Void);
            r.Parameters.Add(new ParameterDefinition("same", ParameterAttributes.None, ts.Boolean));
            r.Parameters.Add(new ParameterDefinition("site", ParameterAttributes.None, ts.Int32));
            t.Methods.Add(r);
            var corlib = ts.CoreLibrary;
            TypeReference T(string ns, string name) => new TypeReference(ns, name, M, corlib);
            // (Console: mscorlib's on Mono; a reference assembly set (.NET's System.Runtime, the tests) has it apart)
            IMetadataScope consoleScope = corlib;
            if (corlib.Name != "mscorlib")
            {
                var cr = M.AssemblyReferences.FirstOrDefault(a => a.Name == "System.Console");
                if (cr == null) { cr = new AssemblyNameReference("System.Console", ((AssemblyNameReference)corlib).Version) { PublicKeyToken = ((AssemblyNameReference)corlib).PublicKeyToken }; M.AssemblyReferences.Add(cr); }
                consoleScope = cr;
            }
            var err = M.ImportReference(new MethodReference("get_Error", T("System.IO", "TextWriter"), new TypeReference("System", "Console", M, consoleScope)));
            var wl = new MethodReference("WriteLine", ts.Void, T("System.IO", "TextWriter")) { HasThis = true }; wl.Parameters.Add(new ParameterDefinition(ts.String));
            var concat = new MethodReference("Concat", ts.String, T("System", "String")); concat.Parameters.Add(new ParameterDefinition(new ArrayType(ts.String)));
            var i32s = new MethodReference("ToString", ts.String, ts.Int32) { HasThis = true };
            var i64s = new MethodReference("ToString", ts.String, ts.Int64) { HasThis = true };
            var il = r.Body.GetILProcessor(); r.Body.InitLocals = true;
            var arr = new VariableDefinition(new ArrayType(ts.String)); var tmp = new VariableDefinition(ts.Int32); var tmp64 = new VariableDefinition(ts.Int64);
            r.Body.Variables.Add(arr); r.Body.Variables.Add(tmp); r.Body.Variables.Add(tmp64);
            void Line(params object[] parts)   // Console.Error.WriteLine(string.Concat(parts)): strings, or a local to ToString
            {
                il.Emit(OpCodes.Call, M.ImportReference(err));
                il.Emit(OpCodes.Ldc_I4, parts.Length); il.Emit(OpCodes.Newarr, ts.String); il.Emit(OpCodes.Stloc, arr);
                for (int k = 0; k < parts.Length; k++)
                {
                    il.Emit(OpCodes.Ldloc, arr); il.Emit(OpCodes.Ldc_I4, k);
                    if (parts[k] is string str) il.Emit(OpCodes.Ldstr, str);
                    else if (parts[k] == (object)tmp64) { il.Emit(OpCodes.Ldloca, tmp64); il.Emit(OpCodes.Call, M.ImportReference(i64s)); }
                    else { il.Emit(OpCodes.Ldloca, (VariableDefinition)parts[k]); il.Emit(OpCodes.Call, M.ImportReference(i32s)); }
                    il.Emit(OpCodes.Stelem_Ref);
                }
                il.Emit(OpCodes.Ldloc, arr); il.Emit(OpCodes.Call, M.ImportReference(concat)); il.Emit(OpCodes.Callvirt, M.ImportReference(wl));
            }
            var counted = il.Create(OpCodes.Nop);
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Brtrue, counted);
            il.Emit(OpCodes.Ldsfld, bad); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Add); il.Emit(OpCodes.Stsfld, bad);
            il.Emit(OpCodes.Ldsfld, bad); il.Emit(OpCodes.Ldc_I4, 200); il.Emit(OpCodes.Bgt, counted);
            il.Emit(OpCodes.Ldarg_1); il.Emit(OpCodes.Stloc, tmp);
            Line("[inline] check MISMATCH at check site ", tmp, " (asmpatch's \"check site\" lines)");
            il.Append(counted);
            il.Emit(OpCodes.Ldsfld, n); il.Emit(OpCodes.Ldc_I4_1); il.Emit(OpCodes.Conv_I8); il.Emit(OpCodes.Add); il.Emit(OpCodes.Dup); il.Emit(OpCodes.Stsfld, n);
            il.Emit(OpCodes.Ldc_I4, 200000); il.Emit(OpCodes.Conv_I8); il.Emit(OpCodes.Rem);
            var done = il.Create(OpCodes.Ret);
            il.Emit(OpCodes.Brtrue, done);
            il.Emit(OpCodes.Ldsfld, n); il.Emit(OpCodes.Stloc, tmp64); il.Emit(OpCodes.Ldsfld, bad); il.Emit(OpCodes.Stloc, tmp);
            Line("[inline] check: ", tmp64, " compared, ", tmp, " mismatches");
            il.Append(done);
            reporters[M] = r;
            return r;
        }
    }
}
