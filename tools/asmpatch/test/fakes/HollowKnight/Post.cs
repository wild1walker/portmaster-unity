// A stand-in for the members of Hollow Knight's post-processing chain that hk-post looks at in
// Assembly-CSharp-firstpass (written for the tests; no game code): the engine's textures,
// materials, blits and behaviours (namespace UnityEngine, as the patch finds them through the
// game's own references), the bloom (its small passes, its composite blit and the release of its
// texture) and the colour curves (one blit of their material).
using System;
using System.Collections.Generic;

namespace UnityEngine
{
    public enum RenderTextureFormat { ARGB32 = 0 }

    public static class Shader
    {
        static readonly List<string> names = new List<string>();

        public static int PropertyToID(string name)
        {
            int i = names.IndexOf(name);
            if (i < 0) { names.Add(name); i = names.Count - 1; }
            return i;
        }

        public static string NameOf(int id) => names[id];
    }

    public class Texture
    {
        public int width { get; set; } = 720;
        public int height { get; set; } = 450;
        public string Name;
        public override string ToString() => Name;
    }

    public class RenderTexture : Texture
    {
        public RenderTextureFormat format { get; set; }
        public static int Outstanding;                      // temporaries taken and not released
        public static readonly List<string> Log = new List<string>();

        public static RenderTexture GetTemporary(int w, int h, int depth, RenderTextureFormat f)
        {
            Outstanding++;
            return new RenderTexture { width = w, height = h, format = f, Name = "temp" + w + "x" + h };
        }

        public static void ReleaseTemporary(RenderTexture rt) { Outstanding--; Log.Add("release " + rt.Name); }
    }

    public class Material
    {
        public string Name;
        public string[] PassNames = { "" };
        public int Lookups;
        public readonly Dictionary<string, Texture> Textures = new Dictionary<string, Texture>();
        public readonly Dictionary<string, float> Floats = new Dictionary<string, float>();

        public int FindPass(string passName) { Lookups++; return Array.IndexOf(PassNames, passName); }
        public Texture GetTexture(string name) => Textures.TryGetValue(name, out var t) ? t : null;
        public void SetTexture(string name, Texture t) { Textures[name] = t; }
        public void SetTexture(int id, Texture t) { Textures[Shader.NameOf(id)] = t; }
        public float GetFloat(string name) => Floats.TryGetValue(name, out var f) ? f : 0f;
        public void SetFloat(string name, float f) { Floats[name] = f; }
        public void SetFloat(int id, float f) { Floats[Shader.NameOf(id)] = f; }
        public override string ToString() => Name;
    }

    public static class Graphics
    {
        public static readonly List<string> Log = new List<string>();

        public static void Blit(Texture source, RenderTexture dest) { Log.Add(source + ">" + dest); }
        public static void Blit(Texture source, RenderTexture dest, Material mat) { Log.Add(source + ">" + dest + ":" + mat); }
        public static void Blit(Texture source, RenderTexture dest, Material mat, int pass) { Log.Add(source + ">" + dest + ":" + mat + "#" + pass); }
    }

    public class Behaviour
    {
        public bool enabled = true;
        public bool activeInHierarchy = true;
        public bool isActiveAndEnabled => enabled && activeInHierarchy;
    }

    public class MonoBehaviour : Behaviour { }
}

namespace UnityStandardAssets.ImageEffects
{
    public class PostEffectsBase : UnityEngine.MonoBehaviour
    {
        protected bool effectIsSupported = true;
        public bool Supported { set { effectIsSupported = value; } }
    }

    public class ImageEffectBase : UnityEngine.MonoBehaviour
    {
        private UnityEngine.Material m_Material = new UnityEngine.Material { Name = "brightness" };
        protected UnityEngine.Material material => m_Material;
        public UnityEngine.Material Material => m_Material;
    }

    public class BloomOptimized : PostEffectsBase
    {
        public int blurIterations = 1;
        private UnityEngine.Material fastBloomMaterial = new UnityEngine.Material { Name = "bloom" };
        public UnityEngine.Material Material => fastBloomMaterial;

        public void OnRenderImage(UnityEngine.RenderTexture source, UnityEngine.RenderTexture destination)
        {
            if (!effectIsSupported) { UnityEngine.Graphics.Blit(source, destination); return; }
            int w = source.width / 4, h = source.height / 4;
            UnityEngine.RenderTexture rt = UnityEngine.RenderTexture.GetTemporary(w, h, 0, source.format);
            UnityEngine.Graphics.Blit(source, rt, fastBloomMaterial, 1);
            for (int i = 0; i < blurIterations; i++)
            {
                UnityEngine.RenderTexture rt2 = UnityEngine.RenderTexture.GetTemporary(w, h, 0, source.format);
                UnityEngine.Graphics.Blit(rt, rt2, fastBloomMaterial, 2);
                UnityEngine.RenderTexture.ReleaseTemporary(rt);
                rt = rt2;
                rt2 = UnityEngine.RenderTexture.GetTemporary(w, h, 0, source.format);
                UnityEngine.Graphics.Blit(rt, rt2, fastBloomMaterial, 3);
                UnityEngine.RenderTexture.ReleaseTemporary(rt);
                rt = rt2;
            }
            fastBloomMaterial.SetTexture("_Bloom", rt);
            UnityEngine.Graphics.Blit(source, destination, fastBloomMaterial, 0);
            UnityEngine.RenderTexture.ReleaseTemporary(rt);
        }
    }

    public class ColorCorrectionCurves : PostEffectsBase
    {
        private static readonly int RGB_TEX = UnityEngine.Shader.PropertyToID("_RgbTex");
        private static readonly int SATURATION = UnityEngine.Shader.PropertyToID("_Saturation");
        public float saturation = 1f;
        private UnityEngine.Material ccMaterial = new UnityEngine.Material { Name = "curves" };
        private UnityEngine.Texture rgbChannelTex = new UnityEngine.Texture { Name = "rgb" };
        public UnityEngine.Material Material => ccMaterial;
        public UnityEngine.Texture Curves => rgbChannelTex;

        public void OnRenderImage(UnityEngine.RenderTexture source, UnityEngine.RenderTexture destination)
        {
            if (!effectIsSupported) { UnityEngine.Graphics.Blit(source, destination); return; }
            ccMaterial.SetTexture(RGB_TEX, rgbChannelTex);
            ccMaterial.SetFloat(SATURATION, saturation);
            UnityEngine.Graphics.Blit(source, destination, ccMaterial);
        }
    }
}
