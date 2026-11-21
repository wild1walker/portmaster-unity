// consolefeed: a game's own developer console driven from a file, for unattended tests of later areas and bosses (a test
// aid; loaded by ua_host's UA_PROBE=<s> UA_PROBE_DLL=<this dll>, which calls UAProbe.Run once, <s> seconds after the
// first frame). Never part of a port: it changes the game's state the way its console does.
//
// Every 15 frames it looks for UA_FEED_FILE (default console.req in the game's folder, where ua_host runs); its lines go to
// the console's command method one by one, then the file is deleted (write it elsewhere and mv it into place). An empty
// line or one starting with # is skipped. Each command is logged: "[feed] <s> s: <command>" (or why it failed).
//   UA_FEED_TARGET=<type>:<static member>:<method>  the console (default Blasphemous's
//                  Gameplay.UI.Widgets.ConsoleWidget:Instance:ProcessCommand; the method takes one string)
//   UA_FEED_WATCH=<type>:<static member>[:<member>...]  a value logged whenever it changes, checked every 30 frames
//                  ("[feed] <s> s: watch <value>"; default Blasphemous's level name,
//                  Framework.Managers.Core:LevelManager:currentLevel:LevelName), to line the frame-rate lines up with the
//                  room the game shows; empty: off
// Blasphemous's console: "load D08Z01S01" (a room; the area's music and enemies as on arrival), "invincible" (toggles),
// "help". Its Update runs whether or not the console is shown (Debug.isDebugBuild only opens it with F1), so commands
// work in the release player. Built against the game's UnityEngine.CoreModule.dll for the MonoBehaviour only: build.sh.
using System;
using System.Diagnostics;
using System.IO;
using System.Reflection;

public static class UAProbe
{
    const BindingFlags A = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static | BindingFlags.Instance;
    static readonly Stopwatch sw = Stopwatch.StartNew();
    static MethodInfo log;
    static string file, target, watch, last;
    static int frames, threw;

    public static void Run(string facts)
    {
        Type debug = T("UnityEngine.Debug");
        if (debug != null) log = debug.GetMethod("Log", new Type[] { typeof(object) });
        file = Env("UA_FEED_FILE", "console.req");
        target = Env("UA_FEED_TARGET", "Gameplay.UI.Widgets.ConsoleWidget:Instance:ProcessCommand");
        watch = Env("UA_FEED_WATCH", "Framework.Managers.Core:LevelManager:currentLevel:LevelName");
        object go = Activator.CreateInstance(T("UnityEngine.GameObject"), new object[] { "ua-consolefeed" });
        T("UnityEngine.Object").GetMethod("DontDestroyOnLoad").Invoke(null, new object[] { go });
        go.GetType().GetMethod("AddComponent", new Type[] { typeof(Type) }).Invoke(go, new object[] { typeof(UAConsoleFeed) });
        Say("[feed] on: commands from " + Path.GetFullPath(file) + " to " + target + (watch.Length > 0 ? ", watching " + watch : ""));
    }

    public static void Tick()
    {
        frames++;
        try {
            if (watch.Length > 0 && frames % 30 == 0) {
                object v = Chain(watch.Split(':'));
                string s = v == null ? "(none)" : v.ToString();
                if (s != last) { last = s; Say("[feed] " + Secs() + " s: watch " + s); }
            }
            if (frames % 15 != 0 || !File.Exists(file)) return;
            string[] lines = File.ReadAllLines(file);
            File.Delete(file);
            foreach (string raw in lines) {
                string line = raw.Trim();
                if (line.Length == 0 || line[0] == '#') continue;
                Say("[feed] " + Secs() + " s: " + line + Send(line));
            }
        } catch (Exception e) { if (threw++ < 5) Say("[feed] threw " + e); }
    }

    static string Send(string line)
    {
        string[] p = target.Split(':');
        if (p.Length != 3) return " (UA_FEED_TARGET is not <type>:<member>:<method>)";
        object inst = Chain(new string[] { p[0], p[1] });
        if (inst == null) return " (no " + p[0] + "." + p[1] + " yet)";
        MethodInfo m = inst.GetType().GetMethod(p[2], A, null, new Type[] { typeof(string) }, null);
        if (m == null) return " (no method " + p[2] + "(string))";
        m.Invoke(inst, new object[] { line });
        return "";
    }

    // <type>:<static member>[:<member>...]: fields or properties, the first static, the rest on the value before
    static object Chain(string[] p)
    {
        Type t = T(p[0]);
        if (t == null) return null;
        object v = Get(t, null, p[1]);
        for (int i = 2; i < p.Length && v != null; i++) v = Get(v.GetType(), v, p[i]);
        return v;
    }

    static object Get(Type t, object o, string name)
    {
        for (Type k = t; k != null; k = k.BaseType) {
            FieldInfo f = k.GetField(name, A);
            if (f != null) return f.GetValue(o);
            PropertyInfo pr = k.GetProperty(name, A);
            if (pr != null) return pr.GetValue(o, null);
        }
        return null;
    }

    static string Env(string name, string def) { string v = Environment.GetEnvironmentVariable(name); return v == null ? def : v; }
    static string Secs() { return (sw.ElapsedMilliseconds / 1000.0).ToString("0.0"); }

    static Type T(string name)
    {
        foreach (Assembly a in AppDomain.CurrentDomain.GetAssemblies()) {
            Type t = a.GetType(name);
            if (t != null) return t;
        }
        return null;
    }

    static void Say(string s) { if (log != null) log.Invoke(null, new object[] { s }); else Console.WriteLine(s); }
}

public class UAConsoleFeed : UnityEngine.MonoBehaviour
{
    void Update() { UAProbe.Tick(); }
}
