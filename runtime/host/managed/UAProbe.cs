// UAProbe: what a phone gives a Unity game, checked from inside the game's own Mono on the handheld (UA_PROBE,
// runtime/host/monodump.c). One check a line in the game's log: "[probe] ok <check>: <value>" or
// "[probe] FAIL <check>: <why>", then "[probe] done: <n> ok, <m> failed (<names>)". A missing feature shows up
// here before a game trips over it, and tools/regress compares the failed set with its baseline.
//
// Built for .NET 3.5 (mscorlib 2.0, as Unity 2017.x's old Mono has it; newer Monos take it too) and with no
// reference to Unity's assemblies: UnityEngine is reached by reflection, so one DLL serves every Unity version.
// It only reads, and writes only its own files (under the game's persistentDataPath and temporaryCachePath)
// and one PlayerPrefs key, removed again. The host passes what it knows ("key=value;..."): the local time's
// offset from UTC in minutes (utc_offset), the time zone's name (tz), the number of cores (cores).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Net;
using System.Net.Sockets;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Threading;

public static class UAProbe
{
    static int ok, failed;
    static readonly List<string> failures = new List<string>();
    static readonly Dictionary<string, string> host = new Dictionary<string, string>();
    static MethodInfo log;

    public static void Run(string facts)
    {
        foreach (string kv in (facts ?? "").Split(';')) {
            int eq = kv.IndexOf('=');
            if (eq > 0) host[kv.Substring(0, eq)] = kv.Substring(eq + 1);
        }
        Type debug = Unity("UnityEngine.Debug");
        if (debug != null) log = debug.GetMethod("Log", new Type[] { typeof(object) });

        Check("unity", delegate { return Get("UnityEngine.Application", "unityVersion") + ", " + Get("UnityEngine.Application", "platform") + ", runtime " + Environment.Version; });
        Check("time zone", TimeZoneCheck);
        Check("culture", delegate {
            CultureInfo c = CultureInfo.CurrentCulture;
            string s = string.Format(c, "{0:N2} {1:d}", 1234.5, new DateTime(2026, 11, 14));
            return (c.Name.Length > 0 ? c.Name : "invariant") + ", ui " + CultureInfo.CurrentUICulture.Name + ": " + s + ", \"i\".ToUpper " + "i".ToUpper(c);
        });
        Check("system language", delegate { return Get("UnityEngine.Application", "systemLanguage"); });
        Check("text encodings", delegate {
            // the code pages beyond UTF-8/16 come from Mono's I18N assemblies, which Unity leaves out of a build
            // unless the project asks for them: missing on a phone too, so not a failure
            byte[] u = Encoding.UTF8.GetBytes("é€");
            if (u.Length != 5) throw new Exception("UTF-8 gave " + u.Length + " bytes");
            try {
                byte[] b = Encoding.GetEncoding(1252).GetBytes("é€");
                if (b.Length != 2 || b[0] != 0xe9 || b[1] != 0x80) throw new Exception("windows-1252 gave " + BitConverter.ToString(b));
                return "UTF-8, windows-1252 (the game ships I18N)";
            } catch (NotSupportedException) { return "UTF-8; windows-1252 not in this build (no I18N assemblies, as on a phone)"; }
              catch (ArgumentException) { return "UTF-8; windows-1252 not in this build (no I18N assemblies, as on a phone)"; }
        });
        Check("persistent data", delegate { return FileRoundTrip((string)Get("UnityEngine.Application", "persistentDataPath")); });
        Check("cache folder", delegate { return FileRoundTrip((string)Get("UnityEngine.Application", "temporaryCachePath")); });
        Check("streaming assets", delegate {
            string p = (string)Get("UnityEngine.Application", "streamingAssetsPath");
            if (p.StartsWith("jar:")) return p + " (in the APK: UnityWebRequest reads it, File does not)";
            return p + (Directory.Exists(p) ? " (a folder: " + Directory.GetFileSystemEntries(p).Length + " entries)" : " (none)");
        });
        Check("player prefs", PrefsCheck);
        Check("threads", ThreadsCheck);
        Check("thread pool", delegate {
            ManualResetEvent done = new ManualResetEvent(false);
            int v = 0;
            ThreadPool.QueueUserWorkItem(delegate { v = 42; done.Set(); });
            if (!done.WaitOne(5000, false) || v != 42) throw new Exception("a work item did not run in 5 s");
            return "a work item ran";
        });
        Check("timers", delegate {
            Stopwatch w = Stopwatch.StartNew();
            Thread.Sleep(20);
            double ms = w.Elapsed.TotalMilliseconds;
            if (ms < 15 || ms > 500) throw new Exception("Sleep(20) took " + ms.ToString("F1") + " ms");
            return "Sleep(20) " + ms.ToString("F1") + " ms, Stopwatch " + (Stopwatch.IsHighResolution ? "high resolution" : "low resolution");
        });
        Check("sockets", SocketsCheck);
        Check("dns", delegate {
            IPAddress[] a = Dns.GetHostAddresses("localhost");
            if (a.Length == 0) throw new Exception("localhost has no address");
            return "localhost " + a[0];
        });
        Check("internet reachability", delegate { return Get("UnityEngine.Application", "internetReachability"); });
        Check("crypto", CryptoCheck);
        Check("compression", delegate {
            // GZipStream is Mono's libMonoPosixHelper.so; some Unity versions' Android players have none (2017.4's),
            // so a phone build of such a game has no gzip either: the port only ever loads the player's own libraries
            try { new GZipStream(new MemoryStream(), CompressionMode.Compress).Close(); }
            catch (DllNotFoundException) { return "no libMonoPosixHelper.so in this build's player (as on a phone): no gzip"; }
            byte[] data = Encoding.UTF8.GetBytes(new string('a', 4000) + "probe");
            MemoryStream m = new MemoryStream();
            using (GZipStream z = new GZipStream(m, CompressionMode.Compress, true)) z.Write(data, 0, data.Length);
            long packed = m.Length;
            m.Position = 0;
            MemoryStream o = new MemoryStream();
            using (GZipStream z = new GZipStream(m, CompressionMode.Decompress)) {
                byte[] buf = new byte[1024];
                int n;
                while ((n = z.Read(buf, 0, buf.Length)) > 0) o.Write(buf, 0, n);
            }
            if (o.Length != data.Length) throw new Exception("gzip gave back " + o.Length + " of " + data.Length + " bytes");
            return "gzip " + data.Length + " -> " + packed + " bytes and back (libMonoPosixHelper)";
        });
        Check("cores", delegate {
            int env = Environment.ProcessorCount;
            object unity = Get("UnityEngine.SystemInfo", "processorCount");
            string want;
            if (host.TryGetValue("cores", out want) && want != env.ToString()) throw new Exception("Environment.ProcessorCount " + env + ", the handheld has " + want);
            return env + " (SystemInfo.processorCount " + unity + ", " + Get("UnityEngine.SystemInfo", "processorType") + ")";
        });
        Check("memory", delegate { return Get("UnityEngine.SystemInfo", "systemMemorySize") + " MB, graphics " + Get("UnityEngine.SystemInfo", "graphicsMemorySize") + " MB"; });
        Check("battery", delegate {
            float level = Convert.ToSingle(Get("UnityEngine.SystemInfo", "batteryLevel"));
            object status = Get("UnityEngine.SystemInfo", "batteryStatus");
            string want;
            if (host.TryGetValue("battery", out want) && want.Length > 0 && Math.Abs(level * 100 - int.Parse(want)) > 2)
                throw new Exception("SystemInfo.batteryLevel " + level + ", the handheld's battery is at " + want + " %");
            return (level * 100).ToString("F0") + " %, " + status;
        });
        Check("device", delegate { return Get("UnityEngine.SystemInfo", "deviceModel") + ", " + Get("UnityEngine.SystemInfo", "operatingSystem"); });
        Check("graphics", delegate {
            return Get("UnityEngine.SystemInfo", "graphicsDeviceName") + ", " + Get("UnityEngine.SystemInfo", "graphicsDeviceVersion") +
                   ", shader level " + Get("UnityEngine.SystemInfo", "graphicsShaderLevel") + ", compute " + Get("UnityEngine.SystemInfo", "supportsComputeShaders");
        });
        Check("screen", delegate {
            return Get("UnityEngine.Screen", "width") + "x" + Get("UnityEngine.Screen", "height") + ", " + Get("UnityEngine.Screen", "dpi") + " dpi, " +
                   Get("UnityEngine.Screen", "currentResolution");
        });
        Check("controllers", delegate {
            string[] names = (string[])Unity("UnityEngine.Input").GetMethod("GetJoystickNames", Type.EmptyTypes).Invoke(null, null);
            return names.Length + (names.Length > 0 ? ": " + string.Join(", ", names) : "");
        });
        Check("microphones", delegate {
            Type mic = Unity("UnityEngine.Microphone");
            if (mic == null) return "no Microphone class";
            return ((string[])mic.GetProperty("devices").GetValue(null, null)).Length.ToString();
        });
        Check("dynamic code", delegate {
            Type dm = Type.GetType("System.Reflection.Emit.DynamicMethod");
            Probe f = delegate { return 1; };
            return "a delegate " + f() + ", reflection " + typeof(UAProbe).GetMethod("Run").Name + ", emit " + (dm != null);
        });
        Check("packages", delegate {
            List<string> found = new List<string>();
            foreach (string t in new string[] { "UnityEngine.AddressableAssets.Addressables", "UnityEngine.Localization.Settings.LocalizationSettings",
                                                "UnityEngine.Purchasing.UnityPurchasing", "UnityEngine.InputSystem.InputSystem", "Rewired.ReInput",
                                                "FMODUnity.RuntimeManager", "Steamworks.SteamAPI", "UnityEngine.Advertisements.Advertisement" })
                if (Unity(t) != null) found.Add(t);
            return found.Count == 0 ? "none of the usual ones" : string.Join(", ", found.ToArray());
        });
        Log("[probe] done: " + ok + " ok, " + failed + " failed" + (failed > 0 ? " (" + string.Join(", ", failures.ToArray()) + ")" : ""));
    }

    delegate object Probe();

    static void Check(string name, Probe p)
    {
        try {
            object v = p();
            ok++;
            Log("[probe] ok " + name + ": " + v);
        } catch (Exception e) {
            if (e is TargetInvocationException && e.InnerException != null) e = e.InnerException;
            failed++;
            failures.Add(name);
            Log("[probe] FAIL " + name + ": " + e.GetType().Name + ": " + e.Message.Replace('\n', ' '));
        }
    }

    static void Log(string s)
    {
        if (log != null) { try { log.Invoke(null, new object[] { s }); return; } catch (Exception) { } }
        Console.Error.WriteLine(s);
    }

    static Type Unity(string full)
    {
        foreach (Assembly a in AppDomain.CurrentDomain.GetAssemblies()) {
            Type t = a.GetType(full, false);
            if (t != null) return t;
        }
        return null;
    }

    static object Get(string type, string prop)
    {
        Type t = Unity(type);
        if (t == null) throw new Exception("no " + type);
        PropertyInfo p = t.GetProperty(prop, BindingFlags.Public | BindingFlags.Static);
        if (p == null) throw new Exception("no " + type + "." + prop);
        return p.GetValue(null, null);
    }

    static object TimeZoneCheck()
    {
        TimeSpan off = DateTime.Now - DateTime.UtcNow;
        int min = (int)Math.Round(off.TotalMinutes);
        string id = "?";
        try {
            Type tzi = Type.GetType("System.TimeZoneInfo") ?? typeof(DateTime).Assembly.GetType("System.TimeZoneInfo") ?? Unity("System.TimeZoneInfo");
            if (tzi != null) id = (string)tzi.GetProperty("Id").GetValue(tzi.GetProperty("Local").GetValue(null, null), null);
        } catch (Exception e) { id = "(TimeZoneInfo.Local threw " + e.GetType().Name + ")"; }
        string s = "local " + DateTime.Now.ToString("yyyy-MM-dd HH:mm", CultureInfo.InvariantCulture) + ", UTC" + (min >= 0 ? "+" : "-") +
                   (Math.Abs(min) / 60).ToString("00") + ":" + (Math.Abs(min) % 60).ToString("00") + ", " + id;
        string want;
        if (host.TryGetValue("utc_offset", out want) && want != min.ToString()) {
            string tz;
            host.TryGetValue("tz", out tz);
            throw new Exception(s + "; the handheld's local time is UTC offset " + want + " min (" + tz + ")");
        }
        return s;
    }

    static string FileRoundTrip(string dir)
    {
        if (string.IsNullOrEmpty(dir)) throw new Exception("no path");
        Directory.CreateDirectory(dir);
        string f = Path.Combine(dir, "uaprobe.tmp");
        File.WriteAllText(f, "probe é");
        string back = File.ReadAllText(f);
        File.Delete(f);
        if (back != "probe é") throw new Exception("read back \"" + back + "\"");
        return dir + " (written, read, removed)";
    }

    static object PrefsCheck()
    {
        Type p = Unity("UnityEngine.PlayerPrefs");
        if (p == null) throw new Exception("no PlayerPrefs");
        const string key = "uaprobe";
        p.GetMethod("SetString", new Type[] { typeof(string), typeof(string) }).Invoke(null, new object[] { key, "probe" });
        string v = (string)p.GetMethod("GetString", new Type[] { typeof(string), typeof(string) }).Invoke(null, new object[] { key, "" });
        p.GetMethod("DeleteKey", new Type[] { typeof(string) }).Invoke(null, new object[] { key });
        if (v != "probe") throw new Exception("GetString gave \"" + v + "\"");
        return "set, read, removed";
    }

    static object ThreadsCheck()
    {
        object gate = new object();
        bool signalled = false;
        int seen = 0;
        Thread t = new Thread(delegate () {
            Interlocked.Increment(ref seen);
            lock (gate) { signalled = true; Monitor.Pulse(gate); }
        });
        Stopwatch w = Stopwatch.StartNew();
        lock (gate) {
            t.Start();
            while (!signalled) if (!Monitor.Wait(gate, 5000)) throw new Exception("no signal from a new thread in 5 s");
        }
        if (!t.Join(5000)) throw new Exception("a thread did not end in 5 s");
        lock (gate) Monitor.Wait(gate, 50);           // a timed wait nobody signals ends by itself
        return "a thread started, signalled and ended (" + w.Elapsed.TotalMilliseconds.ToString("F0") + " ms with a 50 ms timed wait), seen " + seen;
    }

    static object SocketsCheck()
    {
        TcpListener l = new TcpListener(IPAddress.Loopback, 0);
        l.Start();
        try {
            int port = ((IPEndPoint)l.LocalEndpoint).Port;
            using (TcpClient c = new TcpClient()) {
                c.Connect(IPAddress.Loopback, port);
                using (TcpClient s = l.AcceptTcpClient()) {
                    byte[] msg = Encoding.ASCII.GetBytes("probe");
                    c.GetStream().Write(msg, 0, msg.Length);
                    byte[] got = new byte[16];
                    s.ReceiveTimeout = 5000;
                    int n = s.GetStream().Read(got, 0, got.Length);
                    if (Encoding.ASCII.GetString(got, 0, n) != "probe") throw new Exception("received " + n + " bytes");
                }
            }
            return "TCP on 127.0.0.1:" + port + " connected, sent and received";
        } finally { l.Stop(); }
    }

    static object CryptoCheck()
    {
        byte[] abc = Encoding.ASCII.GetBytes("abc");
        string sha = BitConverter.ToString(SHA256.Create().ComputeHash(abc)).Replace("-", "").ToLowerInvariant();
        if (sha != "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") throw new Exception("SHA-256(abc) " + sha);
        string md5 = BitConverter.ToString(MD5.Create().ComputeHash(abc)).Replace("-", "").ToLowerInvariant();
        if (md5 != "900150983cd24fb0d6963f7d28e17f72") throw new Exception("MD5(abc) " + md5);
        byte[] key = new byte[16], iv = new byte[16];
        new RNGCryptoServiceProvider().GetBytes(key);
        using (Rijndael aes = Rijndael.Create()) {
            aes.Key = key; aes.IV = iv;
            byte[] plain = Encoding.ASCII.GetBytes("probe probe probe");
            byte[] enc = aes.CreateEncryptor().TransformFinalBlock(plain, 0, plain.Length);
            byte[] dec = aes.CreateDecryptor().TransformFinalBlock(enc, 0, enc.Length);
            if (Encoding.ASCII.GetString(dec) != "probe probe probe") throw new Exception("AES round trip");
        }
        string hmac = BitConverter.ToString(new HMACSHA1(Encoding.ASCII.GetBytes("key")).ComputeHash(abc)).Replace("-", "").Substring(0, 8);
        return "SHA-256, MD5, AES-CBC, HMAC-SHA1 " + hmac.ToLowerInvariant() + "..., random";
    }
}
