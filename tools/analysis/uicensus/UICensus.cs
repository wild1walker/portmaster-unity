// uicensus: where a Unity game's UI and camera time goes, from inside its own Mono (a measurement; loaded by ua_host's
// UA_PROBE=<s> UA_PROBE_DLL=<this dll>, which calls UAProbe.Run once, <s> seconds after the first frame).
// Every 600 frames it logs (Debug.Log, "[census] ..."):
//   - the UI graphics rebuilt a frame (CanvasUpdateRegistry's graphic queue, read in a LateUpdate last in the script
//     order, after the scripts and the Animators wrote their values and before the canvases rebuild), how many found the
//     inputs of their mesh the same as at their previous rebuild (colour, sprite, fill, text, material, rect: a rebuild a
//     no-op), how many sat under CanvasGroups whose alpha multiplies to 0 (rebuilt and unseen), the layout rebuilds,
//     then the 30 graphics rebuilt most: their path, their GameObject's components, the nearest Animator above, the
//     inputs at their last rebuild;
//   - each camera's main-thread time a frame from Camera.onPreCull to Camera.onPostRender (its culling and drawing,
//     image effects not included) and what it is (target, culling mask, clear, components);
//   - once, after the second report: the running Animators by their top path parts, those under a CanvasGroup chain at
//     alpha 0 apart.
// Unity 2017.4 lessons it carries: UGUI's IndexedSet (the queues) throws NotImplementedException from GetEnumerator, so
// it is read by index; a handler put first into Canvas.willRenderCanvases through reflection was never called on 2017.4's
// Mono (the field held it), hence the LateUpdate. Built against the game's own UnityEngine.CoreModule.dll for the
// MonoBehaviour only (compile time, nothing of it kept): build.sh. Results: docs/profiling.md (Blasphemous on the G350).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Reflection;
using System.Text;

public static class UAProbe
{
    class Rec { public string path, comps, last; public int n, same, unseen, nlayout; }
    class Cam { public string desc; public int n; public double ms; public long t0; }
    const BindingFlags I = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance;
    const BindingFlags S = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static;
    static readonly string[] props = { "enabled", "color", "sprite", "overrideSprite", "type", "fillAmount", "fillMethod", "fillOrigin", "fillClockwise",
                                       "preserveAspect", "text", "font", "fontSize", "material", "texture", "uvRect", "raycastTarget", "pixelsPerUnit" };
    static readonly Dictionary<int, Rec> recs = new Dictionary<int, Rec>();
    static readonly Dictionary<int, Cam> cams = new Dictionary<int, Cam>();
    static readonly Stopwatch sw = Stopwatch.StartNew();
    static object registry;
    static FieldInfo fG, fL, fToRebuild;
    static MethodInfo log, getId;
    static Type graphicT, componentT, behT, animatorT, objectT, canvasGroupT;
    static int frames, windowStart, total, totalSame, totalUnseen, totalLayout, reports, threw;

    public static void Run(string facts)
    {
        Type debug = T("UnityEngine.Debug");
        if (debug != null) log = debug.GetMethod("Log", new Type[] { typeof(object) });
        Type reg = T("UnityEngine.UI.CanvasUpdateRegistry");
        graphicT = T("UnityEngine.UI.Graphic"); componentT = T("UnityEngine.Component"); behT = T("UnityEngine.Behaviour");
        animatorT = T("UnityEngine.Animator"); objectT = T("UnityEngine.Object"); canvasGroupT = T("UnityEngine.CanvasGroup");
        getId = objectT.GetMethod("GetInstanceID");
        Type lr = T("UnityEngine.UI.LayoutRebuilder");
        if (lr != null) fToRebuild = lr.GetField("m_ToRebuild", I);
        if (reg != null) {
            registry = reg.GetProperty("instance", S).GetValue(null, null);
            fG = reg.GetField("m_GraphicRebuildQueue", I);
            fL = reg.GetField("m_LayoutRebuildQueue", I);
        }
        object go = Activator.CreateInstance(T("UnityEngine.GameObject"), new object[] { "ua-census" });
        objectT.GetMethod("DontDestroyOnLoad").Invoke(null, new object[] { go });
        go.GetType().GetMethod("AddComponent", new Type[] { typeof(Type) }).Invoke(go, new object[] { typeof(UACensusLate) });
        Type camT = T("UnityEngine.Camera");
        foreach (string f in new string[] { "onPreCull", "onPostRender" }) {
            FieldInfo fi = camT.GetField(f, S);
            Delegate cur = (Delegate)fi.GetValue(null);
            Delegate mine = Delegate.CreateDelegate(fi.FieldType, typeof(UAProbe).GetMethod(f == "onPreCull" ? "Pre" : "Post", S));
            fi.SetValue(null, f == "onPreCull" ? Delegate.Combine(mine, cur) : Delegate.Combine(cur, mine));
        }
        Say("[census] on: UI queues " + (fG != null && fL != null) + ", cameras timed; a report every 600 frames");
    }

    public static void Late()
    {
        try { Count(); } catch (Exception e) { if (threw++ < 3) Say("[census] threw " + e); }
    }

    static void Count()
    {
        frames++;
        if (registry != null) {
            foreach (object el in Items(fG.GetValue(registry))) {
                Rec r = RecFor(el);
                if (r == null) continue;
                r.n++; total++;
                if (!graphicT.IsInstanceOfType(el)) continue;
                string snap = Snap(el);
                if (r.last != null && snap == r.last) { r.same++; totalSame++; }
                r.last = snap;
                if (Alpha(el) == 0f) { r.unseen++; totalUnseen++; }
            }
            foreach (object el in Items(fL.GetValue(registry))) {
                object o = fToRebuild != null && el != null && el.GetType().Name == "LayoutRebuilder" ? fToRebuild.GetValue(el) : el;
                Rec r = RecFor(o);
                if (r != null) { r.nlayout++; totalLayout++; }
            }
        }
        if (frames - windowStart < 600) return;
        Report();
        if (++reports == 2) Animators();
    }

    // IndexedSet's enumerator throws NotImplementedException: by index
    static List<object> Items(object set)
    {
        List<object> l = new List<object>();
        if (set == null) return l;
        Type t = set.GetType();
        int n = (int)t.GetProperty("Count").GetValue(set, null);
        MethodInfo item = t.GetMethod("get_Item");
        for (int i = 0; i < n; i++) l.Add(item.Invoke(set, new object[] { i }));
        return l;
    }

    static Rec RecFor(object o)
    {
        if (o == null || !componentT.IsInstanceOfType(o)) return null;
        int id = (int)getId.Invoke(o, null);
        Rec r;
        if (!recs.TryGetValue(id, out r)) { r = new Rec(); r.path = Path(o) + " :" + o.GetType().Name; r.comps = Comps(o); recs[id] = r; }
        return r;
    }

    // the CanvasGroups' alphas above, multiplied (0: not seen)
    static float Alpha(object c)
    {
        object go = componentT.GetProperty("gameObject").GetValue(c, null);
        Array groups = (Array)go.GetType().GetMethod("GetComponentsInParent", new Type[] { typeof(Type) }).Invoke(go, new object[] { canvasGroupT });
        float a = 1f;
        foreach (object g in groups) a *= (float)canvasGroupT.GetProperty("alpha").GetValue(g, null);
        return a;
    }

    static string Snap(object g)
    {
        StringBuilder b = new StringBuilder();
        Type t = g.GetType();
        foreach (string p in props) {
            PropertyInfo pi = t.GetProperty(p, BindingFlags.Public | BindingFlags.Instance);
            if (pi == null || !pi.CanRead) continue;
            object v; try { v = pi.GetValue(g, null); } catch (Exception) { v = "!"; }
            b.Append(p).Append('=').Append(V(v)).Append(';');
        }
        object rt = t.GetProperty("rectTransform").GetValue(g, null);
        b.Append("rect=").Append(V(rt.GetType().GetProperty("rect").GetValue(rt, null)));
        b.Append(";pivot=").Append(V(rt.GetType().GetProperty("pivot").GetValue(rt, null)));
        return b.ToString();
    }

    static string V(object v)
    {
        if (v == null) return "null";
        Type t = v.GetType();
        if (objectT.IsInstanceOfType(v)) return t.GetProperty("name").GetValue(v, null) + "#" + getId.Invoke(v, null);
        if (t.IsValueType && !t.IsPrimitive && !t.IsEnum) {   // Color, Rect, Vector2: every field, full precision
            StringBuilder b = new StringBuilder("(");
            foreach (FieldInfo f in t.GetFields(I)) { object fv = f.GetValue(v); b.Append(fv is float ? ((float)fv).ToString("R") : fv.ToString()).Append(','); }
            return b.Append(')').ToString();
        }
        return v is float ? ((float)v).ToString("R") : v.ToString();
    }

    static string Path(object c)
    {
        object tr = componentT.GetProperty("transform").GetValue(c, null);
        PropertyInfo parent = tr.GetType().GetProperty("parent"), name = tr.GetType().GetProperty("name");
        List<string> parts = new List<string>();
        for (object x = tr; x != null && parts.Count < 12; x = parent.GetValue(x, null)) parts.Insert(0, (string)name.GetValue(x, null));
        return string.Join("/", parts.ToArray());
    }

    static string Comps(object c)
    {
        object go = componentT.GetProperty("gameObject").GetValue(c, null);
        Array cs = (Array)go.GetType().GetMethod("GetComponents", new Type[] { typeof(Type) }).Invoke(go, new object[] { componentT });
        StringBuilder b = new StringBuilder();
        foreach (object x in cs) if (x != null) b.Append(x.GetType().Name).Append(',');
        object an = animatorT == null ? null : go.GetType().GetMethod("GetComponentInParent", new Type[] { typeof(Type) }).Invoke(go, new object[] { animatorT });
        if (an != null) b.Append(" animator ").Append(Path(an));
        return b.ToString();
    }

    static void Report()
    {
        int nf = frames - windowStart;
        Say("[census] " + nf + " frames: " + (total / (double)nf).ToString("F2") + " graphic rebuilds a frame (" + (totalSame / (double)nf).ToString("F2") +
            " with their mesh's inputs as at their last rebuild, " + (totalUnseen / (double)nf).ToString("F2") + " under CanvasGroups at alpha 0), " +
            (totalLayout / (double)nf).ToString("F2") + " layout rebuilds a frame");
        List<Rec> l = new List<Rec>();
        foreach (Rec r in recs.Values) if (r.n + r.nlayout > 0) l.Add(r);
        l.Sort(delegate (Rec a, Rec b) { return (b.n + b.nlayout).CompareTo(a.n + a.nlayout); });
        for (int i = 0; i < l.Count && i < 30; i++) {
            Rec r = l[i];
            Say("[census]  " + r.n + " rebuilds (" + r.same + " same, " + r.unseen + " unseen), " + r.nlayout + " layout: " + r.path + " [" + r.comps + "] last " + (r.last ?? ""));
        }
        foreach (Rec r in recs.Values) { r.n = 0; r.same = 0; r.unseen = 0; r.nlayout = 0; }
        foreach (Cam c in cams.Values) {
            Say("[census] camera " + (c.n / (double)nf).ToString("F2") + " renders a frame, " + (c.ms / nf).ToString("F3") + " ms a frame on the main thread: " + c.desc);
            c.n = 0; c.ms = 0;
        }
        total = totalSame = totalUnseen = totalLayout = 0; windowStart = frames;
    }

    static Cam CamFor(object c)
    {
        int id = (int)getId.Invoke(c, null);
        Cam r;
        if (!cams.TryGetValue(id, out r)) { r = new Cam(); r.desc = CamDesc(c); cams[id] = r; }
        return r;
    }
    static object P(object o, string n) { PropertyInfo pi = o.GetType().GetProperty(n); return pi == null ? "?" : pi.GetValue(o, null); }
    static string CamDesc(object c)
    {
        object rt = P(c, "targetTexture");
        string rts = rt == null ? "screen" : V(rt) + " " + P(rt, "width") + "x" + P(rt, "height") + " " + P(rt, "format") + " depth " + P(rt, "depth");
        return Path(c) + " depth " + P(c, "depth") + " -> " + rts + ", mask 0x" + ((int)P(c, "cullingMask")).ToString("x") + ", clear " + P(c, "clearFlags") +
               ", ortho " + P(c, "orthographic") + ", occlusion " + P(c, "useOcclusionCulling") + " [" + Comps(c) + "]";
    }
    static void Pre(object c) { try { CamFor(c).t0 = sw.ElapsedTicks; } catch (Exception) { } }
    static void Post(object c)
    {
        try { Cam r = CamFor(c); if (r.t0 != 0) { r.n++; r.ms += (sw.ElapsedTicks - r.t0) * 1000.0 / Stopwatch.Frequency; r.t0 = 0; } } catch (Exception) { }
    }

    static void Animators()
    {
        Array all = (Array)objectT.GetMethod("FindObjectsOfType", new Type[] { typeof(Type) }).Invoke(null, new object[] { animatorT });
        Dictionary<string, int> by = new Dictionary<string, int>();
        int running = 0, hidden = 0;
        foreach (object a in all) {
            if (!(bool)behT.GetProperty("isActiveAndEnabled").GetValue(a, null)) continue;
            running++;
            bool hid = Alpha(a) == 0f;
            if (hid) hidden++;
            string[] parts = Path(a).Split('/');
            string key = (hid ? "unseen " : "") + string.Join("/", parts, 0, Math.Min(parts.Length, 3));
            int n; by.TryGetValue(key, out n); by[key] = n + 1;
        }
        Say("[census] animators: " + all.Length + " found, " + running + " running, " + hidden + " under CanvasGroups at alpha 0");
        List<KeyValuePair<string, int>> l = new List<KeyValuePair<string, int>>(by);
        l.Sort(delegate (KeyValuePair<string, int> x, KeyValuePair<string, int> y) { return y.Value.CompareTo(x.Value); });
        for (int i = 0; i < l.Count && i < 40; i++) Say("[census]   " + l[i].Value + " " + l[i].Key);
    }

    static Type T(string full)
    {
        foreach (Assembly a in AppDomain.CurrentDomain.GetAssemblies()) { Type t = a.GetType(full, false); if (t != null) return t; }
        return null;
    }

    static void Say(string s)
    {
        if (log != null) { try { log.Invoke(null, new object[] { s }); return; } catch (Exception) { } }
        Console.Error.WriteLine(s);
    }
}

// last in the script order: after the game's Update, LateUpdate and the Animators, before the canvases rebuild
[UnityEngine.DefaultExecutionOrder(32000)]
public class UACensusLate : UnityEngine.MonoBehaviour
{
    void LateUpdate() { UAProbe.Late(); }
}
