// A stand-in for the part of Hollow Knight's TextMeshPro copy (namespace TMProOld) that hk-text looks at (written
// for the tests; no game or TextMeshPro code): TMP_Text's size fields, its fontSize property as the game's is
// written, ParseInputText; TextMeshPro's box (TextContainer), Awake (two ways out) and OnPreRenderObject with its
// one layout call. The layout is a model: the text is PerSize times its size tall and fits while that is at most the
// height the layout may use (m_marginHeight, the box's height); the auto size goes from the size it starts at
// down in 0.05 steps until it fits, never under the range's lower end; each step counts as a layout.
using UnityEngine;

namespace TMProOld
{
    public enum TextOverflowModes { Overflow, Ellipsis, Masking, Truncate, ScrollRect, Page }

    public class TextContainer : Component
    {
        public bool DefaultWidth, DefaultHeight;
        public bool isDefaultWidth => DefaultWidth;
        public bool isDefaultHeight => DefaultHeight;
        public Vector3[] Corners = new Vector3[4];
        public Vector4 Margins;
        public Vector3[] corners => Corners;
        public Vector4 margins => Margins;
    }

    public class TMP_FontAsset : Object { }

    public abstract class TMP_Text : Component
    {
        protected TMP_FontAsset m_fontAsset;
        protected float m_fontSize = 36f;
        protected float m_fontSizeBase = 36f;
        protected bool m_enableAutoSizing;
        protected float m_maxFontSize;
        protected float m_minFontSize;
        protected float m_fontSizeMin;
        protected float m_fontSizeMax;
        protected bool m_enableWordWrapping;
        protected TextOverflowModes m_overflowMode;
        protected float m_marginHeight;
        protected bool m_havePropertiesChanged;
        public int Parses, Layouts;

        public float fontSize
        {
            get { return m_fontSize; }
            set
            {
                if (m_fontSize != value)
                {
                    m_fontSize = value;
                    if (!m_enableAutoSizing) m_fontSizeBase = m_fontSize;
                }
            }
        }

        protected void ParseInputText() { Parses++; }
        protected virtual void GenerateTextMesh() { }

        // the tests' view of the fields
        public float Size => m_fontSize;
        public float Min => m_fontSizeMin;
        public float Max => m_fontSizeMax;
        public bool Auto => m_enableAutoSizing;
        public float Height => m_marginHeight;
        public bool Changed => m_havePropertiesChanged;
    }

    public class TextMeshPro : TMP_Text
    {
        private TextContainer m_textContainer;
        public float PerSize = 0.01f;
        public bool InputChanged;
        /// <summary>The size at which the text just fits its whole box.</summary>
        public float Fits { set => PerSize = m_marginHeight / value; }

        /// <summary>A text named name (under a parent "Inv") of that size, wrapping or not, overflow mode, auto size,
        /// its box set or the default one, with a font asset or none.</summary>
        public static TextMeshPro Make(string name, float size, bool wrap, int mode, bool auto, bool defaultBox, bool font)
        {
            var t = new TextMeshPro();
            t.Own = new UnityEngine.Transform { name = name, parent = new UnityEngine.Transform { name = "Inv" } };
            t.m_fontSize = t.m_fontSizeBase = size;
            t.m_enableWordWrapping = wrap;
            t.m_overflowMode = (TextOverflowModes)mode;
            t.m_enableAutoSizing = auto;
            t.m_textContainer = new TextContainer { DefaultWidth = defaultBox, DefaultHeight = defaultBox };
            t.m_fontAsset = font ? new TMP_FontAsset { name = "perpetua_tmpro" } : null;
            t.Box(10f);
            return t;
        }

        /// <summary>The same, a body text at y in the world under parent, under a HudCamera at y 0 or not, its box h tall
        /// (centred on it, as the game's) with a top margin.</summary>
        public static TextMeshPro MakeAt(string name, string parent, bool hud, float size, float y, float h, float topMargin)
        {
            var t = Make(name, size, true, 5, false, false, true);
            var cam = new UnityEngine.Transform { name = hud ? "HudCamera" : "Camera" };
            var inv = new UnityEngine.Transform { name = "Inventory", parent = cam };
            t.Own.parent = new UnityEngine.Transform { name = parent, parent = inv };
            t.Own.position = new Vector3(0f, y, 0f);
            t.Box(h);
            t.m_textContainer.Margins = new Vector4(0f, topMargin, 0f, 0f);
            return t;
        }

        void Box(float h)
        {
            m_textContainer.Corners[1] = new Vector3(-1f, h / 2f, 0f);
            m_marginHeight = h - m_textContainer.Margins.y;
        }

        protected void Awake()
        {
            if (m_fontAsset == null)
            {
                // (the engine calls the game makes elsewhere, which the patch calls the same way)
                Debug.Log("no font asset on " + transform.name + " " + Mathf.Abs(transform.lossyScale.y) + " " + Screen.width + "x" + Screen.height +
                          " " + (transform.parent == null) + " " + new Vector3(0f, 0f, 0f).x + " " + transform.position.y);
                return;
            }
            if (m_fontSizeMin == 0f) m_fontSizeMin = m_fontSize / 2f;
            if (m_fontSizeMax == 0f) m_fontSizeMax = m_fontSize * 2f;
        }

        public void Wake() => Awake();

        public void OnPreRenderObject()
        {
            if (InputChanged) { ParseInputText(); InputChanged = false; }
            if (m_enableAutoSizing) m_fontSize = m_fontSize < m_fontSizeMin ? m_fontSizeMin : m_fontSize > m_fontSizeMax ? m_fontSizeMax : m_fontSize;
            m_maxFontSize = m_fontSizeMax;
            m_minFontSize = m_fontSizeMin;
            GenerateTextMesh();
        }

        protected override void GenerateTextMesh()
        {
            Layouts++;
            while (m_enableAutoSizing && m_fontSize * PerSize > m_marginHeight + 0.0001f && m_fontSize > m_minFontSize)
            {
                m_fontSize = m_fontSize - 0.05f < m_minFontSize ? m_minFontSize : m_fontSize - 0.05f;
                Layouts++;
            }
        }
    }
}
