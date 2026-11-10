using System;
using System.Collections;
using System.Globalization;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using UnityEngine;

// The Mach-O route's checks under the player (docs/macho-route.md): what has to be right
// between the game's code, compiled for macOS, and the Android player's. Each check logs
// "[checks] ok <name>" or "[checks] FAIL <name>", then one line with the counts; the test
// reads the player's log. Only the route's test build has this component (MacBuild.cs).
public class MachoChecks : MonoBehaviour
{
    int passed, failed, exceptionsLogged;
    const string OnPurpose = "checks: thrown on purpose";

    void Check(string name, Func<bool> check)
    {
        bool ok;
        string why = "";
        try { ok = check(); }
        catch (Exception e) { ok = false; why = ": " + e.GetType().Name + ": " + e.Message; }
        if (ok) passed++; else failed++;
        Debug.Log("[checks] " + (ok ? "ok " : "FAIL ") + name + why);
    }

    // Whether the call throws a T with that text in its message; logs what happened instead if not.
    bool Throws<T>(string what, Action call, string part) where T : Exception
    {
        try { call(); }
        catch (T e) {
            if (e.Message.Contains(part)) return true;
            Debug.Log("[checks] " + what + ": the message is \"" + e.Message + "\"");
            return false;
        }
        catch (Exception e) {
            Debug.Log("[checks] " + what + ": " + e.GetType().Name + ": " + e.Message);
            return false;
        }
        Debug.Log("[checks] " + what + ": no exception");
        return false;
    }

    IEnumerator Start()
    {
        foreach (LogType t in Enum.GetValues(typeof(LogType))) Application.SetStackTraceLogType(t, StackTraceLogType.None);
        Application.logMessageReceived += (text, stack, type) => { if (type == LogType.Exception && text.Contains(OnPurpose)) exceptionsLogged++; };
        for (int i = 0; i < 30; i++) yield return null;          // the game's first frames as without the checks

        var go = new GameObject("checks-object");

        // Results narrower than 32 bits: the game's code expects them extended, the player's
        // functions leave the upper bits as they were.
        Check("bool results", () => {
            for (int i = 0; i < 1000; i++) {
                go.SetActive(false);
                if (go.activeSelf || go.activeInHierarchy) return false;
                go.SetActive(true);
                if (!go.activeSelf || !go.activeInHierarchy) return false;
            }
            return !go.isStatic && go.CompareTag("Untagged") && !go.CompareTag("MainCamera") && !Input.GetKey(KeyCode.F12) &&
                   Application.isPlaying && !Input.GetMouseButton(2);
        });

        // Arguments past the eighth: 4-byte stack slots on the game's side, 8-byte ones on the
        // player's (CopyTexture's region form has twelve).
        Check("stack arguments", () => {
            var src = new Texture2D(16, 16, TextureFormat.RGBA32, false);
            var dst = new Texture2D(16, 16, TextureFormat.RGBA32, false);
            var a = new Color32[256];
            var b = new Color32[256];
            for (int i = 0; i < 256; i++) { a[i] = new Color32((byte)(i % 16 * 16), (byte)(i / 16 * 16), 7, 255); b[i] = new Color32(1, 2, 3, 255); }
            src.SetPixels32(a); src.Apply();
            dst.SetPixels32(b); dst.Apply();
            Graphics.CopyTexture(src, 0, 0, 2, 3, 4, 5, dst, 0, 0, 6, 7);
            var r = dst.GetPixels32();
            for (int y = 0; y < 16; y++)
                for (int x = 0; x < 16; x++) {
                    bool inside = x >= 6 && x < 10 && y >= 7 && y < 12;
                    Color32 want = inside ? a[(y - 7 + 3) * 16 + (x - 6 + 2)] : b[0];
                    Color32 got = r[y * 16 + x];
                    if (got.r != want.r || got.g != want.g || got.b != want.b) {
                        Debug.Log("[checks] stack arguments: at " + x + "," + y + " " + got + ", expected " + want);
                        return false;
                    }
                }
            return true;
        });

        // An exception the player raises inside one of its functions, caught by the game: it
        // unwinds through the player's frames (and the bridge's, where a call is laid out again).
        Check("engine exception", () => {
            for (int i = 0; i < 200; i++)
                if (!Throws<UnityException>("FindWithTag", () => GameObject.FindWithTag("no such tag"), "no such tag")) return false;
            return true;
        });
        // (Input.GetButton with a name that is not set up is no such case: only the editor and
        // development players raise for it; a release player, macOS's too, has no such message.)
        Check("engine exception, bool result", () => {
            for (int i = 0; i < 200; i++)
                if (!Throws<ArgumentException>("GetKey", () => Input.GetKey("no such key"), "no such key")) return false;
            return true;
        });
        Check("engine exception, child index", () => {
            try { go.transform.GetChild(5); return false; }
            catch (UnityException) { return go.transform.childCount == 0; }
        });

        // An exception of the game's that reaches the player (a message's handler throws): the
        // player logs it and goes on.
        Check("exception into the player", () => {
            int before = exceptionsLogged;
            gameObject.SendMessage("ThrowOnPurpose");
            return exceptionsLogged == before + 1;
        });

        // Floats, doubles and structs through the player's functions.
        Check("float and struct values", () => {
            var t = go.transform;
            t.position = new Vector3(1.5f, -2.25f, 3.125f);
            t.localScale = new Vector3(2f, 4f, 8f);
            var q = Quaternion.Euler(0f, 90f, 0f);
            var m = Matrix4x4.TRS(new Vector3(1f, 2f, 3f), Quaternion.identity, new Vector3(2f, 2f, 2f));
            float n = Mathf.PerlinNoise(0.25f, 0.75f);
            return t.position == new Vector3(1.5f, -2.25f, 3.125f) && t.lossyScale == new Vector3(2f, 4f, 8f) &&
                   Mathf.Abs(q.y - 0.70710678f) < 1e-5f && Mathf.Abs(q.w - 0.70710678f) < 1e-5f && q.x == 0f && q.z == 0f &&
                   m.determinant == 8f && m.inverse.MultiplyPoint3x4(new Vector3(1f, 2f, 3f)) == Vector3.zero &&
                   Mathf.GammaToLinearSpace(1f) == 1f && Mathf.GammaToLinearSpace(0f) == 0f && n >= 0f && n <= 1f;
        });

        // Strings and arrays across the boundary.
        Check("strings and arrays", () => {
            go.name = "pöng ✓ 游戏";
            int id = Shader.PropertyToID("_ChecksProperty");
            var tex = new Texture2D(4, 4, TextureFormat.RGBA32, false);
            var px = new Color32[16];
            for (int i = 0; i < 16; i++) px[i] = new Color32((byte)i, (byte)(255 - i), (byte)(i * 3), 255);
            tex.SetPixels32(px);
            var back = tex.GetPixels32();
            for (int i = 0; i < 16; i++) if (back[i].r != px[i].r || back[i].g != px[i].g || back[i].b != px[i].b) return false;
            return go.name == "pöng ✓ 游戏" && GameObject.Find(go.name) == go && id == Shader.PropertyToID("_ChecksProperty") &&
                   id != Shader.PropertyToID("_ChecksOther") && Application.productName == "Pong" && Application.unityVersion.Length > 5;
        });

        // The class library on this system: formatting, culture, time, the environment.
        Check("class library", () => {
            var inv = CultureInfo.InvariantCulture;
            return (1234.5).ToString("N2", inv) == "1,234.50" && double.Parse("-0.125", inv) == -0.125 && (1.0f / 3).ToString("R", inv) == "0.333333343" &&
                   DateTime.UtcNow.Year >= 2025 && Environment.ProcessorCount > 0 && Environment.TickCount != 0 &&
                   string.Compare("a", "B", StringComparison.OrdinalIgnoreCase) < 0 && "Straße".ToUpperInvariant() == "STRAßE" &&
                   Math.Abs(Math.Sin(Math.PI / 6) - 0.5) < 1e-15 && Guid.NewGuid() != Guid.NewGuid();
        });

        // Files in the game's own folder.
        Check("files", () => {
            string dir = Path.Combine(Application.persistentDataPath, "checks");
            Directory.CreateDirectory(dir);
            string file = Path.Combine(dir, "a.txt");
            File.WriteAllText(file, "one\ntwo\n");
            File.AppendAllText(file, "three\n");
            bool ok = File.ReadAllLines(file).Length == 3 && new FileInfo(file).Length == 14 && Directory.GetFiles(dir).Length == 1;
            File.Delete(file);
            Directory.Delete(dir);
            return ok && !Directory.Exists(dir);
        });

        Check("player prefs", () => {
            PlayerPrefs.SetInt("checks.int", -42);
            PlayerPrefs.SetFloat("checks.float", 0.375f);
            PlayerPrefs.SetString("checks.string", "pöng");
            PlayerPrefs.Save();
            bool ok = PlayerPrefs.GetInt("checks.int") == -42 && PlayerPrefs.GetFloat("checks.float") == 0.375f &&
                      PlayerPrefs.GetString("checks.string") == "pöng" && PlayerPrefs.GetInt("checks.none", 9) == 9;
            PlayerPrefs.DeleteKey("checks.int"); PlayerPrefs.DeleteKey("checks.float"); PlayerPrefs.DeleteKey("checks.string");
            return ok && !PlayerPrefs.HasKey("checks.int");
        });

        // Threads of the game's, and the collector stopping them and the player's.
        Check("threads", () => {
            long sum = 0;
            var threads = new Thread[4];
            for (int i = 0; i < 4; i++) {
                int n = i;
                threads[i] = new Thread(() => { long s = 0; for (int k = 0; k < 1000000; k++) s += k % (n + 2); Interlocked.Add(ref sum, s); });
                threads[i].Start();
            }
            foreach (var t in threads) t.Join();
            long want = 0;
            for (int n = 0; n < 4; n++) for (int k = 0; k < 1000000; k++) want += k % (n + 2);
            int pooled = Task.Run(() => Thread.CurrentThread.IsThreadPoolThread ? 7 : 0).Result;
            long par = 0;
            Parallel.For(0, 1000, k => Interlocked.Add(ref par, k));
            return sum == want && pooled == 7 && par == 499500;
        });
        // Four threads make and check objects, a set amount of work each, while this one asks
        // for collection after collection: every thread is stopped at any point of its code,
        // again and again, and what it holds has to be as it left it. (The work is a set
        // amount, and this thread's is done when theirs is: how the collector's lock shares
        // its time between them is not what is checked. The line before the result has the
        // counts.)
        Check("collections with threads running", () => {
            const int Rounds = 50000, Ring = 32;
            var keep = new byte[64][];
            for (int i = 0; i < 64; i++) { keep[i] = new byte[1000]; for (int k = 0; k < 1000; k++) keep[i][k] = (byte)(i + k); }
            int wrong = 0, running = 4;
            var threads = new Thread[4];
            for (int i = 0; i < 4; i++) {
                int n = i;
                threads[i] = new Thread(() => {
                    var ring = new byte[Ring][];
                    string text = "";
                    int bad = 0;
                    for (int k = 0; k < Rounds; k++) {
                        int slot = k % Ring;
                        var old = ring[slot];
                        if (old != null) {
                            byte was = (byte)(n * 31 + k - Ring);
                            if (old.Length != 64 + slot) bad++;
                            else for (int j = 0; j < old.Length; j++) if (old[j] != (byte)(was + j)) { bad++; break; }
                        }
                        var b = new byte[64 + slot];
                        byte v = (byte)(n * 31 + k);
                        for (int j = 0; j < b.Length; j++) b[j] = (byte)(v + j);
                        ring[slot] = b;
                        if (k % 100 == 0) {
                            if (k > 0 && text != n + ":" + (k - 100)) bad++;
                            text = n.ToString(CultureInfo.InvariantCulture) + ":" + k.ToString(CultureInfo.InvariantCulture);
                        }
                    }
                    Interlocked.Add(ref wrong, bad);
                    Interlocked.Decrement(ref running);
                });
            }
            int before = GC.CollectionCount(0), asked = 0;
            float clock = Time.realtimeSinceStartup;
            foreach (var t in threads) t.Start();
            while (Volatile.Read(ref running) > 0) {
                GC.Collect();
                asked++;
                var junk = new object[100];
                for (int k = 0; k < 100; k++) junk[k] = new byte[64];
                Thread.Sleep(1);
            }
            foreach (var t in threads) t.Join();
            int ms = (int)((Time.realtimeSinceStartup - clock) * 1000f);
            Debug.Log("[checks] collections: " + asked + " asked for and " + (GC.CollectionCount(0) - before) + " in all in " + ms + " ms, while 4 threads made " + 4 * Rounds + " objects");
            for (int i = 0; i < 64; i++) for (int k = 0; k < 1000; k++) if (keep[i][k] != (byte)(i + k)) return false;
            return wrong == 0 && asked > 0 && GC.CollectionCount(0) >= before + asked;
        });

        // The player's own scheduling of the game's code: coroutines, and a task's continuation
        // back on the main thread.
        int frame = Time.frameCount;
        yield return null; yield return null; yield return null;
        int frames = Time.frameCount - frame;
        // WaitForSeconds counts the game's clock (Time.time), and that is what the wait is held to.
        // The Android player moves it by the display's frame times, a whole number of them each
        // frame, so in the frames after a long one the same wait is shorter on the real clock
        // (measured: 0.2000 s of game time in 0.1833 s, 0.2500 s in 0.2173 s); the real clock
        // only says that the wait ended.
        float clock = Time.realtimeSinceStartup, game = Time.time;
        yield return new WaitForSeconds(0.2f);
        float waited = Time.realtimeSinceStartup - clock, gameWaited = Time.time - game, last = Time.deltaTime;
        Debug.Log("[checks] coroutines: " + frames + " frames; the wait was " + gameWaited.ToString("F4", CultureInfo.InvariantCulture)
            + " s of game time (the last frame " + last.ToString("F4", CultureInfo.InvariantCulture) + " s), "
            + waited.ToString("F4", CultureInfo.InvariantCulture) + " s of real time");
        Check("coroutines", () => frames == 3 && gameWaited >= 0.1999f && waited < 2f);

        int main = Thread.CurrentThread.ManagedThreadId, after = -1;
        var delay = AfterDelay(id => after = id);
        while (!delay.IsCompleted) yield return null;
        Check("task continuation on the main thread", () => after == main && !delay.IsFaulted);

        Destroy(go);
        yield return null;
        Check("destroyed object", () => go == null);

        Debug.Log("[checks] done: " + passed + " passed, " + failed + " failed");
    }

    async Task AfterDelay(Action<int> report)
    {
        await Task.Delay(50);
        report(Thread.CurrentThread.ManagedThreadId);
    }

    void ThrowOnPurpose()
    {
        throw new InvalidOperationException(OnPurpose);
    }
}
