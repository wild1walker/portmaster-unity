using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;

// The route's test game: the donor's game of pong (tools/donor/Assets/Scripts) built as a macOS
// arm64 IL2CPP player, whose GameAssembly.dylib and global-metadata.dat the bridge runs under
// the Android player of the same Unity version (build.sh runs this), with the route's checks
// (MachoChecks.cs) beside the game.
public static class MacBuild
{
    public static void Il2cppArm64()
    {
        System.IO.Directory.CreateDirectory("out");
        var scene = EditorSceneManager.NewScene(NewSceneSetup.DefaultGameObjects, NewSceneMode.Single);
        new GameObject("Pong").AddComponent<Pong>();
        new GameObject("Checks").AddComponent<MachoChecks>();     // MachoChecks.cs: its lines in the player's log
        EditorSceneManager.SaveScene(scene, "Assets/Donor.unity");
        // the game's native plugins (build.sh put them there): for the macOS player's arm64 half only
        foreach (var path in new[] { "Assets/Plugins/lib_checks.dylib", "Assets/Plugins/checks_bundle.bundle" }) {
            var plugin = AssetImporter.GetAtPath(path) as PluginImporter;
            if (plugin == null) { Debug.LogError("no plugin importer for " + path); EditorApplication.Exit(1); return; }
            plugin.SetCompatibleWithAnyPlatform(false);
            plugin.SetCompatibleWithEditor(false);
            plugin.SetCompatibleWithPlatform(BuildTarget.StandaloneOSX, true);
            plugin.SetPlatformData(BuildTarget.StandaloneOSX, "CPU", "ARM64");
            plugin.SaveAndReimport();
        }
        PlayerSettings.companyName = "portmaster-unity";
        PlayerSettings.productName = "Pong";
        var mac = UnityEditor.Build.NamedBuildTarget.Standalone;
        PlayerSettings.SetScriptingBackend(mac, ScriptingImplementation.IL2CPP);
        PlayerSettings.SetApiCompatibilityLevel(mac, ApiCompatibilityLevel.NET_Unity_4_8);
        PlayerSettings.SetManagedStrippingLevel(mac, ManagedStrippingLevel.Disabled);
        PlayerSettings.stripEngineCode = false;
        // (arm64 only: UnityEditor.OSXStandalone.UserBuildSettings.architecture, in the Mac module's assembly)
        var ubs = System.Type.GetType("UnityEditor.OSXStandalone.UserBuildSettings, UnityEditor.OSXStandalone.Extensions");
        var arch = ubs == null ? null : ubs.GetProperty("architecture");
        if (arch == null) { Debug.LogError("no OSXStandalone.UserBuildSettings.architecture"); EditorApplication.Exit(1); return; }
        arch.SetValue(null, System.Enum.Parse(arch.PropertyType, "ARM64"), null);
        var report = BuildPipeline.BuildPlayer(new[] { "Assets/Donor.unity" }, "out/Pong.app", BuildTarget.StandaloneOSX, BuildOptions.None);
        // (a plugin set up for one CPU lands in Contents/PlugIns/ARM64, one for both in Contents/PlugIns)
        if (!System.IO.File.Exists("out/Pong.app/Contents/Frameworks/GameAssembly.dylib") ||
            !System.IO.Directory.Exists("out/Pong.app/Contents/PlugIns") ||
            System.IO.Directory.GetFiles("out/Pong.app/Contents/PlugIns", "lib_checks.dylib", System.IO.SearchOption.AllDirectories).Length == 0) {
            Debug.LogError("build: " + report.summary.result + ", " + report.summary.totalErrors + " errors");
            EditorApplication.Exit(1);
        }
    }
}
