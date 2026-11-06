using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;

// Builds a donor: a one-scene game (a tiny game of pong, Assets/Scripts/Pong.cs) whose APK carries
// Unity's Android player (build.sh runs it).
public static class DonorBuild
{
    static void Build(ScriptingImplementation backend, string abi, string apk)
    {
        EditorPrefs.SetString("AndroidSdkRoot", System.Environment.GetEnvironmentVariable("DONOR_SDK"));
        EditorPrefs.SetString("JdkPath", System.Environment.GetEnvironmentVariable("DONOR_JDK"));
        // (the editor reads the NDK's folder from its preferences before this runs: build.sh sets them too)
        EditorPrefs.SetString("AndroidNdkRoot", System.Environment.GetEnvironmentVariable("DONOR_NDK") ?? "");
        // (2019.3 on: the external tools settings, which Unity 6 checks for every Android build, NDK included)
        var tools = System.Type.GetType("UnityEditor.Android.AndroidExternalToolsSettings, UnityEditor.Android.Extensions");
        string[] props = { "sdkRootPath", "jdkRootPath", "ndkRootPath" }, envs = { "DONOR_SDK", "DONOR_JDK", "DONOR_NDK" };
        for (int i = 0; tools != null && i < props.Length; i++) {
            var v = System.Environment.GetEnvironmentVariable(envs[i]);
            var pi = tools.GetProperty(props[i]);
            if (pi != null && !string.IsNullOrEmpty(v)) pi.SetValue(null, v, null);
        }
        System.IO.Directory.CreateDirectory("out");
        var scene = EditorSceneManager.NewScene(NewSceneSetup.DefaultGameObjects, NewSceneMode.Single);
        new GameObject("Pong").AddComponent<Pong>();
        EditorSceneManager.SaveScene(scene, "Assets/Donor.unity");
        PlayerSettings.companyName = "portmaster-unity";
        PlayerSettings.productName = "Pong";
        PlayerSettings.SetApplicationIdentifier(BuildTargetGroup.Android, "org.portmasterunity.donor");
        PlayerSettings.SetScriptingBackend(BuildTargetGroup.Android, backend);
#if UNITY_2021_2_OR_NEWER
        // (Unity 6 and the 2021.2+ editors: .NET Framework 4.8 profile, Gradle, API 23 and up, GLES 3 only)
        var android = UnityEditor.Build.NamedBuildTarget.Android;
        PlayerSettings.SetApiCompatibilityLevel(android, ApiCompatibilityLevel.NET_Unity_4_8);
        PlayerSettings.SetManagedStrippingLevel(android, ManagedStrippingLevel.Disabled);
#else
        PlayerSettings.SetApiCompatibilityLevel(BuildTargetGroup.Android, ApiCompatibilityLevel.NET_2_0);
        PlayerSettings.strippingLevel = StrippingLevel.Disabled;
#endif
        PlayerSettings.stripEngineCode = false;     // (IL2CPP: else libunity is relinked without what the small game leaves unused)
        // ARM64: PlayerSettings.Android.targetArchitectures (2017.4.16+); ARMv7: targetDevice
        var arch = typeof(PlayerSettings.Android).GetProperty("targetArchitectures");
        if (arch != null)
            arch.SetValue(null, System.Enum.Parse(arch.PropertyType, abi), null);
        else
            PlayerSettings.Android.targetDevice = (AndroidTargetDevice)System.Enum.Parse(typeof(AndroidTargetDevice), abi);
        PlayerSettings.SetUseDefaultGraphicsAPIs(BuildTarget.Android, false);
#if UNITY_2021_2_OR_NEWER
        PlayerSettings.Android.minSdkVersion = AndroidSdkVersions.AndroidApiLevel23;
        PlayerSettings.SetGraphicsAPIs(BuildTarget.Android, new[] { UnityEngine.Rendering.GraphicsDeviceType.OpenGLES3 });
        EditorUserBuildSettings.buildAppBundle = false;
        EditorUserBuildSettings.exportAsGoogleAndroidProject = false;
#if UNITY_2023_1_OR_NEWER
        // (the plain Activity entry: GameActivity, the default, builds native code with the SDK's CMake)
        PlayerSettings.Android.applicationEntry = AndroidApplicationEntry.Activity;
#endif
#else
        PlayerSettings.Android.minSdkVersion = AndroidSdkVersions.AndroidApiLevel16;
        PlayerSettings.SetGraphicsAPIs(BuildTarget.Android, new[] { UnityEngine.Rendering.GraphicsDeviceType.OpenGLES3, UnityEngine.Rendering.GraphicsDeviceType.OpenGLES2 });
        EditorUserBuildSettings.androidBuildSystem = AndroidBuildSystem.Internal;
#endif
#if UNITY_2018_1_OR_NEWER
        // (2018.1 on: a BuildReport, whose summary says how the build ended)
        var report = BuildPipeline.BuildPlayer(new[] { "Assets/Donor.unity" }, apk, BuildTarget.Android, BuildOptions.None);
        var r = report.summary.result == UnityEditor.Build.Reporting.BuildResult.Succeeded ? "" : report.summary.result + ", " + report.summary.totalErrors + " errors";
#else
        var r = BuildPipeline.BuildPlayer(new[] { "Assets/Donor.unity" }, apk, BuildTarget.Android, BuildOptions.None);
#endif
        // (with -nographics the scene's reflection probe cannot be baked: "RenderTexture.Create failed:
        // cubemap not supported" counts as build errors, but the APK and its player are complete)
        if (!System.IO.File.Exists(apk)) { Debug.LogError(r); EditorApplication.Exit(1); }
        if (!string.IsNullOrEmpty(r)) Debug.LogWarning("build reported: " + r + " (the APK was written)");
    }

    public static void MonoArmv7() { Build(ScriptingImplementation.Mono2x, "ARMv7", "out/donor-" + Application.unityVersion + "-mono-armv7.apk"); }
    public static void Il2cppArm64() { Build(ScriptingImplementation.IL2CPP, "ARM64", "out/donor-" + Application.unityVersion + "-il2cpp-arm64.apk"); }
    // the 32-bit IL2CPP player (for an IL2CPP build on a 32-bit userland: the game's heap keeps 32-bit references)
    public static void Il2cppArmv7() { Build(ScriptingImplementation.IL2CPP, "ARMv7", "out/donor-" + Application.unityVersion + "-il2cpp-armv7.apk"); }
}
