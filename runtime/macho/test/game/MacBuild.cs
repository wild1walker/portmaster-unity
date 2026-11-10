using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;

// The route's test game: the donor's game of pong (tools/donor/Assets/Scripts) built as a macOS
// arm64 IL2CPP player, whose GameAssembly.dylib and global-metadata.dat the bridge runs under
// the Android player of the same Unity version (build.sh runs this).
public static class MacBuild
{
    public static void Il2cppArm64()
    {
        System.IO.Directory.CreateDirectory("out");
        var scene = EditorSceneManager.NewScene(NewSceneSetup.DefaultGameObjects, NewSceneMode.Single);
        new GameObject("Pong").AddComponent<Pong>();
        EditorSceneManager.SaveScene(scene, "Assets/Donor.unity");
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
        if (!System.IO.File.Exists("out/Pong.app/Contents/Frameworks/GameAssembly.dylib")) {
            Debug.LogError("build: " + report.summary.result + ", " + report.summary.totalErrors + " errors");
            EditorApplication.Exit(1);
        }
    }
}
