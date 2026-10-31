// codepatch from JavaScript: the input assemblies are written to the runtime's in-memory file system,
// codepatch's own Main runs on them with the port's arguments, and the patched Assembly-CSharp.dll and
// what Main printed come back.
using System.Runtime.InteropServices.JavaScript;
using System.Text;

public static partial class Entry
{
    // names: the Managed folder's file names; data: their bytes one after another, lengths in sizes;
    // flags: codepatch's options (e.g. "--target=android --steam-dlc"). Returns [exit code][log length][log][dll]
    [JSExport]
    public static byte[] Run(string[] names, byte[] data, int[] sizes, string flags)
    {
        string root = "/codepatch", inDir = root + "/in", outDir = root + "/out";
        if (Directory.Exists(root)) Directory.Delete(root, true);
        Directory.CreateDirectory(inDir);
        for (int i = 0, off = 0; i < names.Length; off += sizes[i], i++)
            File.WriteAllBytes(Path.Combine(inDir, names[i]), data.AsSpan(off, sizes[i]).ToArray());
        var log = new StringWriter();
        var (o, e) = (Console.Out, Console.Error);
        Console.SetOut(log); Console.SetError(log);
        int rc;
        try { rc = (int)typeof(Program).GetMethod("Main", System.Reflection.BindingFlags.Static | System.Reflection.BindingFlags.NonPublic)
                  .Invoke(null, new object[] { flags.Split(' ', StringSplitOptions.RemoveEmptyEntries).Append(inDir).Append(outDir).ToArray() }); }
        catch (Exception ex) { log.WriteLine($"FAIL {ex.InnerException?.Message ?? ex.Message}"); rc = 3; }
        finally { Console.SetOut(o); Console.SetError(e); }
        var lb = Encoding.UTF8.GetBytes(log.ToString());
        string dll = Path.Combine(outDir, "Assembly-CSharp.dll");
        var db = File.Exists(dll) ? File.ReadAllBytes(dll) : Array.Empty<byte>();
        Directory.Delete(root, true);
        var r = new byte[8 + lb.Length + db.Length];
        BitConverter.GetBytes(rc).CopyTo(r, 0); BitConverter.GetBytes(lb.Length).CopyTo(r, 4);
        lb.CopyTo(r, 8); db.CopyTo(r, 8 + lb.Length);
        return r;
    }
}
