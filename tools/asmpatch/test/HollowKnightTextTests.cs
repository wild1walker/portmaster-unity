// hk-text on the stand-in Assembly-CSharp's TMProOld (test/fakes/HollowKnight/Text.cs, built by build.sh test): opt-in,
// the factor the variable asks for on each screen, which texts grow, the fit to the box (kept until the input or the
// size changes, never under the game's size), fontSize in the game's units, the log lines.
using System.Reflection;
using System.Runtime.Loader;

namespace AsmPatch.Tests;

static partial class TestMain
{
    /// <summary>Loads the stand-ins from dir in a fresh context (the helper reads the variables once), sets the
    /// stand-in screen's size and runs body on the loaded Assembly-CSharp and firstpass.</summary>
    static T TextRun<T>(string dir, string env, string log, (int w, int h) screen, Func<Assembly, Assembly, T> body)
    {
        var old = (Environment.GetEnvironmentVariable(HollowKnightText.Variable), Environment.GetEnvironmentVariable(HollowKnightText.LogVariable));
        Environment.SetEnvironmentVariable(HollowKnightText.Variable, env);
        Environment.SetEnvironmentVariable(HollowKnightText.LogVariable, log);
        var ctx = new AssemblyLoadContext("hkt" + Guid.NewGuid(), isCollectible: true);
        ctx.Resolving += (c, n) => n.Name == "Assembly-CSharp-firstpass" ? c.LoadFromStream(new MemoryStream(File.ReadAllBytes(Path.Combine(dir, n.Name + ".dll")))) : null;
        try
        {
            var a = ctx.LoadFromStream(new MemoryStream(File.ReadAllBytes(Path.Combine(dir, "Assembly-CSharp.dll"))));
            var fp = ctx.LoadFromAssemblyName(new AssemblyName("Assembly-CSharp-firstpass"));
            var s = fp.GetType("UnityEngine.Screen", true);
            s.GetProperty("width").SetValue(null, screen.w);
            s.GetProperty("height").SetValue(null, screen.h);
            return body(a, fp);
        }
        finally
        {
            ctx.Unload();
            Environment.SetEnvironmentVariable(HollowKnightText.Variable, old.Item1);
            Environment.SetEnvironmentVariable(HollowKnightText.LogVariable, old.Item2);
        }
    }

    sealed class Txt
    {
        readonly object o;
        public Txt(Assembly a, string name, float size, bool wrap = true, int mode = 5, bool auto = false, bool defaultBox = false, bool font = true) =>
            o = a.GetType("TMProOld.TextMeshPro", true).GetMethod("Make").Invoke(null, new object[] { name, size, wrap, mode, auto, defaultBox, font });
        /// <summary>A body text of size 5 at y under parent (under a HudCamera or not), its box h tall, centred.</summary>
        public Txt(Assembly a, string name, string parent, bool hud, float y, float h, float topMargin = 0f) =>
            o = a.GetType("TMProOld.TextMeshPro", true).GetMethod("MakeAt").Invoke(null, new object[] { name, parent, hud, 5f, y, h, topMargin });
        float F(string p) => (float)o.GetType().GetProperty(p).GetValue(o);
        public float Size => F("Size");
        public float Min => F("Min");
        public float Max => F("Max");
        public bool Auto => (bool)o.GetType().GetProperty("Auto").GetValue(o);
        public float FontSize { get => F("fontSize"); set => o.GetType().GetProperty("fontSize").SetValue(o, value); }
        public int Layouts => (int)o.GetType().GetField("Layouts").GetValue(o);
        public int Parses => (int)o.GetType().GetField("Parses").GetValue(o);
        public float Fits { set => o.GetType().GetProperty("Fits").SetValue(o, value); }
        public float PerSize { set => o.GetType().GetField("PerSize").SetValue(o, value); }
        public float PerWidth { set => o.GetType().GetField("PerWidth").SetValue(o, value); }
        public float X { set => o.GetType().GetProperty("X").SetValue(o, value); }
        public float Height => F("Height");
        public bool Relaid => (bool)o.GetType().GetProperty("Changed").GetValue(o);
        public void Changed() => o.GetType().GetField("InputChanged").SetValue(o, true);
        public Txt Wake() { o.GetType().GetMethod("Wake").Invoke(o, null); return this; }
        public Txt Render() { o.GetType().GetMethod("OnPreRenderObject").Invoke(o, null); return this; }
    }

    static bool Near(float a, float b, float eps = 0.051f) => Math.Abs(a - b) <= eps;

    static void HollowKnightTextTests()
    {
        if (!File.Exists(FakeHollowKnightFirstpass)) { Check(false, $"Hollow Knight firstpass stand-in at {FakeHollowKnightFirstpass} (build.sh test builds it)"); return; }
        var d = Dir("hkt");
        File.Copy(FakeHollowKnight, Path.Combine(d, "Assembly-CSharp.dll"));
        File.Copy(FakeHollowKnightFirstpass, Path.Combine(d, "Assembly-CSharp-firstpass.dll"));
        var (st, outp) = Patch(d, Path.Combine(tmp, "hkt-default"));
        Check(st == 0 && outp.Contains("skip hk-text (opt-in: --with=hk-text)"), "hk-text: opt-in, not run by default");
        var o = Path.Combine(tmp, "hkt-out");
        (st, outp) = Patch(d, o, with: new[] { "hk-text" });
        Check(st == 0 && outp.Contains("ok   hk-text: Assembly-CSharp.dll: TMProOld.TextMeshPro's body text") && File.Exists(Path.Combine(o, "Assembly-CSharp.dll")),
              "hk-text: applied when named");
        if (!File.Exists(Path.Combine(o, "Assembly-CSharp.dll"))) return;
        File.Copy(FakeHollowKnightFirstpass, Path.Combine(o, "Assembly-CSharp-firstpass.dll"), true);

        // the factor each value asks for, on each screen
        foreach (var (v, w, h, want) in new (string, int, int, float)[]
                 {
                     ("auto", 720, 480, 1024f / 720f), ("auto", 640, 480, 1.6f), ("auto", 854, 480, 1024f / 853.3333f), ("auto", 960, 544, 1024f / 960f),
                     ("auto", 720, 720, 1024f / 720f), ("auto", 480, 640, 2f), ("auto", 320, 240, 2f), ("auto", 1024, 576, 0f), ("auto", 1280, 720, 0f),
                     ("auto", 1920, 1080, 0f), ("auto", 2560, 1080, 0f), ("auto", 0, 0, 0f),
                     ("1.5", 720, 480, 1.5f), ("1.5", 1920, 1080, 1.5f), (" 1.3 ", 720, 480, 1.3f), ("4", 720, 480, 4f), ("4.5", 720, 480, 0f),
                     ("1", 720, 480, 0f), ("1.01", 720, 480, 0f), ("0.8", 720, 480, 0f), ("-2", 720, 480, 0f), ("NaN", 720, 480, 0f),
                     ("1,5", 720, 480, 0f), ("AUTO", 720, 480, 0f), ("big", 720, 480, 0f), ("", 720, 480, 0f), (null, 720, 480, 0f),
                 })
        {
            float got = TextRun(o, null, null, (720, 480), (a, _) =>
                (float)a.GetType("<asmpatch>HollowKnightText", true).GetMethod("Factor", BindingFlags.Static | BindingFlags.NonPublic).Invoke(null, new object[] { v, w, h }));
            Check(Near(got, want, 0.0005f), $"hk-text: Factor({v ?? "null"}, {w}x{h}) = {want:0.####} ({got:0.####})");
        }

        // the unpatched stand-in, and the patched one with the variable unset or off: the game's sizes
        foreach (var (dir, v) in new[] { (d, "1.5"), (o, null), (o, ""), (o, "1"), (o, "off"), (o, "auto") })
        {
            var screen = v == "auto" ? (1280, 720) : (720, 480);
            bool same = TextRun(dir, v, null, screen, (a, _) =>
            {
                var t = new Txt(a, "Text Desc", 5f).Wake();
                t.Fits = 3f;
                t.Render();
                return t.Size == 5f && t.FontSize == 5f && !t.Auto && t.Min == 2.5f && t.Max == 10f && t.Layouts == 1;
            });
            Check(same, $"hk-text: {(dir == d ? "unpatched" : "patched")}, {HollowKnightText.Variable}={v ?? "(unset)"} on {screen.Item1}x{screen.Item2}: the game's size");
        }

        // a body text: the range 5 .. 7.5, fitted to the box once, kept, fitted again on new input or a new size
        TextRun(o, "1.5", null, (720, 480), (a, _) =>
        {
            var t = new Txt(a, "Text Desc", 5f).Wake();
            Check(t.FontSize == 5f && t.Min == 5f && t.Max == 7.5f && t.Size == 7.5f && t.Auto,
                  $"hk-text: 1.5: a body text's range is 5 .. 7.5, fontSize reads 5 ({t.FontSize}, {t.Min} .. {t.Max}, size {t.Size}, auto {t.Auto})");
            t.Fits = 6.2f;
            t.Render();
            int first = t.Layouts;
            Check(Near(t.Size, 6.2f) && !t.Auto && first > 1, $"hk-text: fitted to its box: 6.2 ({t.Size}, {first} layouts), auto size off after");
            t.Render(); t.Render();
            Check(t.Layouts == first + 2 && Near(t.Size, 6.2f), $"hk-text: the fit is kept: one layout per frame after it ({t.Layouts - first} for 2)");
            t.Fits = 100f; t.Changed(); t.Render();
            Check(t.Parses == 1 && t.Size == 7.5f && !t.Auto, $"hk-text: new input: fitted again, up to 7.5 ({t.Size})");
            t.Fits = 1f; t.Changed(); t.Render();
            Check(t.Size == 5f, $"hk-text: a text that does not fit even at the game's size keeps it, never smaller ({t.Size})");
            t.FontSize = 4f;
            t.Fits = 100f; t.Render();
            Check(t.FontSize == 4f && t.Min == 4f && t.Max == 6f && t.Size == 6f,
                  $"hk-text: the game sets 4: reads 4, range 4 .. 6, fitted ({t.FontSize}, {t.Min} .. {t.Max}, {t.Size})");
            var ov = new Txt(a, "Listen", 6f, mode: 0).Wake().Render();
            Check(ov.Size == 9f && ov.FontSize == 6f, $"hk-text: an overflowing (not paged) body text grows too ({ov.Size})");
            return 0;
        });

        // not body text: one line, truncated or ellipsed, auto-sized already, no box, no font, the pane title
        TextRun(o, "1.5", null, (720, 480), (a, _) =>
        {
            foreach (var (what, t) in new[]
                     {
                         ("no wrapping", new Txt(a, "Title", 5f, wrap: false)), ("truncated", new Txt(a, "Label", 5f, mode: 3)),
                         ("ellipsis", new Txt(a, "Label", 5f, mode: 1)), ("masked", new Txt(a, "Label", 5f, mode: 2)),
                         ("auto-sized by the game", new Txt(a, "Label", 5f, auto: true)), ("the default box", new Txt(a, "Text", 5f, defaultBox: true)),
                         ("no font asset", new Txt(a, "Text", 5f, font: false)), ("the pane title", new Txt(a, "Pane Name", 5f)),
                     })
            {
                bool wasAuto = t.Auto;
                t.Wake(); t.Fits = 3f; t.Render();
                // (the game's own auto size keeps its range: its size is the game's fit)
                Check(wasAuto ? t.Auto && t.Min == 2.5f && t.Max == 10f : !t.Auto && t.Size == 5f && t.FontSize == 5f,
                      $"hk-text: {what}: the game's size ({t.Size}, {t.Min} .. {t.Max}, auto {t.Auto})");
            }
            return 0;
        });

        // a text the game places over less of the screen than its box: kept above its floor, measured from the HudCamera
        TextRun(o, "1.5", null, (720, 480), (a, _) =>
        {
            // the charms' description at y -4.89, its box 10.14 tall: the floor 876 (y -5.25) leaves 5.43 of it
            var ch = new Txt(a, "Text Desc", "Charms", true, -4.89f, 10.14f).Wake();
            ch.PerSize = 0.8f;   // (its whole box would take 12.7)
            ch.Render();
            Check(Near(ch.Size, 6.75f, 0.03f) && Near(ch.Height, 10.14f, 0.0001f) && ch.Relaid && !ch.Auto,
                  $"hk-text: the charms' description fitted above the Equip prompt (6.75: {ch.Size}), its box's height back ({ch.Height}), laid out again");
            var top = new Txt(a, "Text Desc", "Charms", true, -4.89f, 10.14f, 0.4f).Wake();
            top.PerSize = 0.8f; top.Render();
            Check(Near(top.Size, 6.25f, 0.03f), $"hk-text: the box's top margin counts ({top.Size})");
            var jd = new Txt(a, "Text Desc", "Journal", true, -0.79f, 10.3f).Wake();
            jd.PerSize = 0.5f; jd.Render();
            Check(Near(jd.Size, 6.2f, 0.03f), $"hk-text: the journal's description above its divider (6.2: {jd.Size})");
            var inv = new Txt(a, "Text Desc", "Inv", true, -0.77f, 10.3f).Wake();
            inv.PerSize = 0.5f; inv.Render();
            Check(inv.Size == 7.5f && Near(inv.Height, 10.3f, 0.0001f) && !inv.Relaid, $"hk-text: a floor below the box changes nothing ({inv.Size}, {inv.Height})");
            var noCam = new Txt(a, "Text Desc", "Charms", false, -4.89f, 10.14f).Wake();
            noCam.PerSize = 0.8f; noCam.Render();
            Check(noCam.Size == 7.5f, $"hk-text: no HudCamera above: the box only ({noCam.Size})");
            var other = new Txt(a, "Text Name", "Charms", true, -4.89f, 10.14f).Wake();
            other.PerSize = 0.8f; other.Render();
            Check(other.Size == 7.5f, $"hk-text: a text without a floor: the box only ({other.Size})");
            return 0;
        });

        // a text outside the HUD whose line would run past the screen's sides (the main camera shows 20 units across):
        // a smaller top size, laid out again in the same frame, never under its own size
        TextRun(o, "1.5", null, (640, 480), (a, fp) =>
        {
            var camT = fp.GetType("UnityEngine.Camera", true);
            void Cameras(params object[] cs)
            {
                var arr = Array.CreateInstance(camT, cs.Length);
                for (int k = 0; k < cs.Length; k++) arr.SetValue(cs[k], k);
                camT.GetProperty("allCameras").SetValue(null, arr);
            }
            object Cam(int mask = -1, float x = 0f)
            {
                var c = Activator.CreateInstance(camT);
                camT.GetField("Mask").SetValue(c, mask); camT.GetField("X").SetValue(c, x);
                return c;
            }
            Cameras(Cam());
            var w = new Txt(a, "TextMeshPro", 5f).Wake();
            w.PerWidth = 3f;   // (7.5 would be 22.5 across; 2 % inside each side leaves 19.2: 6.4 at most)
            w.Render();
            Check(w.Size <= 6.4f && w.Size >= 6.2f && !w.Auto && w.Layouts == 2,
                  $"hk-text: a line past the screen's sides: smaller (6.27: {w.Size}), in the same frame ({w.Layouts} layouts)");
            w.Render();
            Check(w.Layouts == 3 && w.Size <= 6.4f, $"hk-text: that size is kept ({w.Size}, {w.Layouts} layouts)");
            var fits = new Txt(a, "TextMeshPro", 5f).Wake();
            fits.PerWidth = 2f; fits.Render();
            Check(fits.Size == 7.5f && fits.Layouts == 1, $"hk-text: a line inside the screen: the box only ({fits.Size}, {fits.Layouts} layouts)");
            var wide = new Txt(a, "TextMeshPro", 5f).Wake();
            wide.PerWidth = 5f; wide.Render();
            Check(wide.Size == 5f && wide.Layouts == 2, $"hk-text: wider than the screen even at its own size: its own size, never smaller ({wide.Size}, {wide.Layouts} layouts)");
            var side = new Txt(a, "TextMeshPro", 5f).Wake();
            side.X = 6f; side.PerWidth = 1f; side.Render();
            Check(side.Size < 7.2f && side.Size >= 6.9f, $"hk-text: a text right of the middle: kept inside the right side (7.06: {side.Size})");
            var hud = new Txt(a, "Text", "Prompts", true, 0f, 10f).Wake();
            hud.PerWidth = 3f; hud.Render();
            Check(hud.Size == 7.5f, $"hk-text: under the HudCamera: the box only ({hud.Size})");
            var off = new Txt(a, "TextMeshPro", 5f).Wake();
            off.X = 30f; off.PerWidth = 3f; off.Render();
            Check(off.Size == 7.5f, $"hk-text: its position off the camera's view: the box only ({off.Size})");
            // (the first camera drawing the text's layer with its position in view; layer 0)
            Cameras(Cam(mask: 2), Cam(x: 100f), Cam());
            var later = new Txt(a, "TextMeshPro", 5f).Wake();
            later.PerWidth = 3f; later.Render();
            Check(later.Size <= 6.4f && later.Size >= 6.2f, $"hk-text: measured through the camera that draws it (not one without its layer or its position) ({later.Size})");
            Cameras(Cam(mask: 2), Cam(x: 100f));
            var none = new Txt(a, "TextMeshPro", 5f).Wake();
            none.PerWidth = 3f; none.Render();
            Check(none.Size == 7.5f, $"hk-text: no camera draws it in view: the box only ({none.Size})");
            return 0;
        });

        // auto on a 720x480 and a 640x480 screen
        foreach (var (w, h) in new[] { (720, 480), (640, 480) })
            TextRun(o, "auto", null, (w, h), (a, _) =>
            {
                var t = new Txt(a, "Text Desc", 5f).Wake().Render();
                float k = w == 720 ? 1024f / 720f : 1.6f;
                Check(Near(t.Size, 5f * k, 0.001f) && t.FontSize == 5f, $"hk-text: auto on {w}x{h}: up to {k:0.###} ({t.Size / 5f:0.###})");
                return 0;
            });

        // the log: one line when on; with the log variable each text and each fit
        TextRun(o, "1.5", null, (720, 480), (a, fp) =>
        {
            var dbg = fp.GetType("UnityEngine.Debug", true);
            string Last() => (string)dbg.GetField("Last").GetValue(null);
            int Count() => (int)dbg.GetField("Count").GetValue(null);
            int c0 = Count();
            new Txt(a, "Text Desc", 5f).Wake().Render();
            new Txt(a, "Text Name", 7f).Wake().Render();
            Check(Count() == c0 + 1 && Last().StartsWith("[hk-text] body text up to 1.5 times its size, fitted to its box (720x480, ASMPATCH_HK_TEXT=1.5)"),
                  $"hk-text: one line, at the first text ({Last()})");
            return 0;
        });
        TextRun(o, "1.5", "1", (720, 480), (a, fp) =>
        {
            var dbg = fp.GetType("UnityEngine.Debug", true);
            string Last() => (string)dbg.GetField("Last").GetValue(null);
            var t = new Txt(a, "Text Desc", 5f).Wake();
            Check(Last().StartsWith("[hk-text] Inv/Text Desc size 5 world 5 overflow Page wrap True auto False font perpetua_tmpro body True"),
                  $"hk-text: {HollowKnightText.LogVariable}=1: each text when it wakes ({Last()})");
            t.Fits = 6f; t.Render();
            var fit = Last();
            int at = fit.IndexOf(" -> ", StringComparison.Ordinal);
            Check(fit.StartsWith("[hk-text] fit Inv/Text Desc 5 -> ") && fit.EndsWith(" (up to 7.5, height 10 of -1)") && at > 0 &&
                  float.TryParse(fit.Substring(at + 4).Split(' ')[0], System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var got) && Near(got, 6f),
                  $"hk-text: each fit ({fit})");
            new Txt(a, "Title", 9f, wrap: false).Wake();
            Check(Last().EndsWith("body False"), $"hk-text: a title is logged as not body ({Last()})");
            return 0;
        });

        (st, outp) = Patch(o, Path.Combine(tmp, "hkt-again"), with: new[] { "hk-text" });
        Check(st == 0 && outp.Contains("done hk-text") && NoDlls(Path.Combine(tmp, "hkt-again")), "hk-text: a second run finds it applied");
        var none = Dir("hkt-none");
        var plain = NewModule("Assembly-CSharp");
        plain.Types.Add(new Mono.Cecil.TypeDefinition("", "GameManager", Mono.Cecil.TypeAttributes.Public, plain.TypeSystem.Object));
        plain.Write(Path.Combine(none, "Assembly-CSharp.dll"));
        (st, outp) = Patch(none, Path.Combine(tmp, "hkt-none-out"), with: new[] { "hk-text" });
        Check(st == 0 && outp.Contains("n/a  hk-text: no Hollow Knight"), "hk-text: no TMProOld: not applicable");
    }
}
