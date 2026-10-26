using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;

// Builds a donor: an empty one-scene game whose APK carries Unity's Android player (build.sh runs it).
public static class DonorBuild
{
    static void Build(ScriptingImplementation backend, string abi, string apk)
    {
        EditorPrefs.SetString("AndroidSdkRoot", System.Environment.GetEnvironmentVariable("DONOR_SDK"));
        EditorPrefs.SetString("JdkPath", System.Environment.GetEnvironmentVariable("DONOR_JDK"));
        // (the editor reads the NDK's folder from its preferences before this runs: build.sh sets them too)
        EditorPrefs.SetString("AndroidNdkRoot", System.Environment.GetEnvironmentVariable("DONOR_NDK") ?? "");
        System.IO.Directory.CreateDirectory("out");
        var scene = EditorSceneManager.NewScene(NewSceneSetup.DefaultGameObjects, NewSceneMode.Single);
        EditorSceneManager.SaveScene(scene, "Assets/Donor.unity");
        PlayerSettings.companyName = "portmaster-unity";
        PlayerSettings.productName = "Donor";
        PlayerSettings.SetApplicationIdentifier(BuildTargetGroup.Android, "org.portmasterunity.donor");
        PlayerSettings.SetScriptingBackend(BuildTargetGroup.Android, backend);
        PlayerSettings.SetApiCompatibilityLevel(BuildTargetGroup.Android, ApiCompatibilityLevel.NET_2_0);
        PlayerSettings.strippingLevel = StrippingLevel.Disabled;
        PlayerSettings.stripEngineCode = false;     // (IL2CPP: else libunity is relinked without what the empty scene leaves unused)
        // ARM64: PlayerSettings.Android.targetArchitectures (2017.4.16+); ARMv7: targetDevice
        var arch = typeof(PlayerSettings.Android).GetProperty("targetArchitectures");
        if (arch != null)
            arch.SetValue(null, System.Enum.Parse(arch.PropertyType, abi), null);
        else
            PlayerSettings.Android.targetDevice = (AndroidTargetDevice)System.Enum.Parse(typeof(AndroidTargetDevice), abi);
        PlayerSettings.Android.minSdkVersion = AndroidSdkVersions.AndroidApiLevel16;
        PlayerSettings.SetUseDefaultGraphicsAPIs(BuildTarget.Android, false);
        PlayerSettings.SetGraphicsAPIs(BuildTarget.Android, new[] { UnityEngine.Rendering.GraphicsDeviceType.OpenGLES3, UnityEngine.Rendering.GraphicsDeviceType.OpenGLES2 });
        EditorUserBuildSettings.androidBuildSystem = AndroidBuildSystem.Internal;
        var r = BuildPipeline.BuildPlayer(new[] { "Assets/Donor.unity" }, apk, BuildTarget.Android, BuildOptions.None);
        if (!string.IsNullOrEmpty(r)) { Debug.LogError(r); EditorApplication.Exit(1); }
    }

    public static void MonoArmv7() { Build(ScriptingImplementation.Mono2x, "ARMv7", "out/donor-" + Application.unityVersion + "-mono-armv7.apk"); }
    public static void Il2cppArm64() { Build(ScriptingImplementation.IL2CPP, "ARM64", "out/donor-" + Application.unityVersion + "-il2cpp-arm64.apk"); }
}
