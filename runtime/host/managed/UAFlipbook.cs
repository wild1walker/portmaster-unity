// UAFlipbook: a stand-in for Unity's Animator on sprite flip-books, loaded into the game's Mono by ua_host
// (UA_FLIPBOOK, runtime/host/monodump.c). A flip-book is an Animator whose controller is one looping clip,
// without parameters, transitions or events, that only swaps its own SpriteRenderer's sprite (a crowd
// figure, a pulsing blob, a waterfall): Unity runs its whole animation pipeline for each of them every frame.
//
// Each one is shadowed first: while the Animator runs, the time it follows and the frame it shows are worked
// out here, and compared with the sprite it shows. After 240 frames in a row that agree the Animator is
// turned off, and the sprite is set from the same time before every render (Application.onBeforeRender,
// after the game's animation stage). The game's own changes give it back to the Animator: its object turned
// off, the Animator turned on, its controller changed. One that jumps after a long agreement (a script
// set its time) is left to the Animator. The time is followed as Unity's state machine holds it as far as
// the handheld showed (2026-11-07: the normalized time in float, + deltaTime * speed / length on every frame,
// on frames its renderer is seen for one that culls completely; the frame is the last key at or before
// the clip time, in float): on the Mali handheld a stand-in sprite differs from Unity's about once in
// 5,000 frames, by one frame early or late, the same sprites at the same speed. It never touches the game's
// state: only SpriteRenderer.sprite and Animator.enabled of the controllers named.
//
// Mode 0 (UA_FLIPBOOK_MODE=check) only shadows: the Animators keep running, the differences are counted.
using System;
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.SceneManagement;

public static class UAFlipbook
{
    sealed class Book
    {
        public Animator anim; public SpriteRenderer ren; public GameObject go; public RuntimeAnimatorController ctl;
        public Sprite[] frames; public float length, rate; public int culling;
        public float nt;            // the state's normalized time, followed as Unity holds it
        public int state;           // 0 shadowed (the Animator runs), 1 ours (the Animator off), 2 left to the Animator
        public int matched; public bool known;
    }

    static readonly List<Book> books = new List<Book>();
    static readonly HashSet<int> seen = new HashSet<int>();
    static HashSet<string> allow;
    static int mode;
    const int Needed = 240;
    static bool rescan = true;
    static long checks, frameMiss, timeMiss, jumps, takes, givebacks, logged;
    static float lastLog;

    public static void Init(string names, int m)
    {
        allow = new HashSet<string>(names.Split(new[] { ',' }, StringSplitOptions.RemoveEmptyEntries));
        mode = m;
        Application.onBeforeRender += Step;
        SceneManager.sceneLoaded += (s, lm) => rescan = true;
        SceneManager.activeSceneChanged += (a, b) => rescan = true;
        Debug.Log("[flipbook] on: " + allow.Count + " controllers, " + (mode == 0 ? "check only" : "an Animator taken over after " + Needed + " frames that agree"));
    }

    static void Scan()
    {
        foreach (var a in UnityEngine.Object.FindObjectsOfType<Animator>())
        {
            if (!a.isActiveAndEnabled || !seen.Add(a.GetInstanceID())) continue;
            var ctl = a.runtimeAnimatorController;
            if (ctl == null || !allow.Contains(ctl.name)) continue;
            if (a.layerCount != 1 || a.parameterCount != 0 || a.applyRootMotion || a.updateMode != AnimatorUpdateMode.Normal) continue;
            var clips = ctl.animationClips;
            if (clips.Length != 1) continue;
            var clip = clips[0];
            if (!clip.isLooping || clip.events.Length != 0 || clip.frameRate <= 0 || clip.length <= 0) continue;
            var ren = a.GetComponent<SpriteRenderer>();
            if (ren == null) continue;
            int n = Mathf.RoundToInt(clip.length * clip.frameRate);
            if (n < 1 || n > 1024) continue;
            // the clip's frames, sampled in the middle of each, and the sprite put back (all before this render)
            var keep = ren.sprite;
            var frames = new Sprite[n];
            for (int k = 0; k < n; k++) { clip.SampleAnimation(a.gameObject, (k + 0.5f) / clip.frameRate); frames[k] = ren.sprite; }
            ren.sprite = keep;
            books.Add(new Book { anim = a, ren = ren, go = a.gameObject, ctl = ctl, frames = frames, length = clip.length, rate = clip.frameRate, culling = (int)a.cullingMode });
        }
    }

    // the frame Unity shows at a normalized time: the last key at or before the clip time, both in float
    static Sprite FrameAt(Book b, float nt)
    {
        int n = b.frames.Length;
        float tc = (nt - Mathf.Floor(nt)) * b.length;
        int i = (int)(tc * b.rate);
        while (i + 1 < n && (float)(i + 1) / b.rate <= tc) i++;
        while (i > 0 && (float)i / b.rate > tc) i--;
        return b.frames[i < 0 ? 0 : i >= n ? n - 1 : i];
    }

    static void Step()
    {
        if (rescan) { rescan = false; Scan(); }
        float dt = Time.deltaTime;
        for (int i = books.Count - 1; i >= 0; i--)
        {
            var b = books[i];
            if (b.anim == null || b.ren == null) { books.RemoveAt(i); continue; }   // destroyed
            if (b.state == 2) continue;
            // Unity moves an Animator on every frame; one that culls completely only while its renderer is seen
            bool moves = b.culling != 2 || b.ren.isVisible;
            if (b.state == 1)
            {
                if (!b.go.activeInHierarchy || b.anim.enabled || b.anim.runtimeAnimatorController != b.ctl)
                {   // the game turned the object off, the Animator back on, or changed its controller: the Animator takes it back
                    b.anim.enabled = true; b.state = 0; b.known = false; b.matched = 0; givebacks++;
                    continue;
                }
                if (!moves) continue;
                b.nt += dt * b.anim.speed / b.length;
                var s = FrameAt(b, b.nt);
                if (b.ren.sprite != s) b.ren.sprite = s;
                continue;
            }
            if (!b.anim.isActiveAndEnabled) { b.known = false; b.matched = 0; continue; }
            var st = b.anim.GetCurrentAnimatorStateInfo(0);
            if (!b.known) { b.nt = st.normalizedTime; b.known = true; continue; }
            if (moves) b.nt += dt * b.anim.speed * st.speed / b.length;
            if (!moves) continue;
            checks++;
            var shown = b.ren.sprite;
            if (FrameAt(b, st.normalizedTime) != shown) frameMiss++;   // the frame rule itself, on Unity's own time
            float apart = (st.normalizedTime - b.nt) * b.length * b.rate;
            if (Math.Abs(apart) > 1.5f)
            {   // a jump: a script set a new time, or the first frames after a load
                jumps++;
                if (b.matched >= 60) { b.state = 2; Log(b, st, "a jump after " + b.matched + " frames: left to the Animator"); }
                b.nt = st.normalizedTime; b.matched = 0;
                continue;
            }
            if (FrameAt(b, b.nt) == shown) b.matched++;
            else { timeMiss++; Log(b, st, "off in time (" + apart.ToString("F4") + " frames) after " + b.matched); b.nt = st.normalizedTime; b.matched = 0; }
            if (mode == 1 && b.matched >= Needed) { b.anim.enabled = false; b.state = 1; takes++; }
        }
        if (Time.realtimeSinceStartup - lastLog >= 10f)
        {
            lastLog = Time.realtimeSinceStartup;
            int ours = 0; foreach (var b in books) if (b.state == 1) ours++;
            Debug.Log("[flipbook] " + books.Count + " followed, " + ours + " ours; " + checks + " frames checked: " + frameMiss + " not as the frame rule says, " +
                      timeMiss + " off in time, " + jumps + " jumps; " + takes + " taken over, " + givebacks + " given back");
        }
    }

    static void Log(Book b, AnimatorStateInfo st, string what)
    {
        if (++logged > 40) return;
        Debug.Log("[flipbook] " + b.go.name + " (" + b.ctl.name + ", culling " + b.culling + ", " + b.frames.Length + " frames of " + b.length.ToString("R") + " s): Unity " +
                  st.normalizedTime.ToString("R") + ", ours " + b.nt.ToString("R") + ": " + what);
    }
}
