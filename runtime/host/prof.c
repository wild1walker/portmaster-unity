// Sampling profiler: where chosen threads (UnityMain, Unity's render thread) spend their time,
// recorded to a file that tools/analysis/profsum.py sums up by module, function and managed method.
// Off unless UA_PROF is set:
//
//   UA_PROF=<hz>             samples per second and thread while recording (e.g. 250, 500, 1000)
//   UA_PROF_THREADS=a,b*     threads by name, a trailing * matches a prefix; @swap is the thread that
//                            presents the frames (default UnityMain,UnityGfxDeviceW: Unity's render
//                            thread is "UnityGfxDeviceWorker"). "UnityMain" is the thread that runs
//                            Unity's frames: threads it starts inherit the name until they set theirs
//   UA_PROF_SECS=<s>         length of a recording (default 20)
//   UA_PROF_START=<s>        record once, <s> seconds after the first frame (default: only on prof.req)
//   UA_PROF_DEPTH=<n>        return addresses kept per sample (default 32, at most 128; 0: pc and lr)
//   UA_PROF_SCAN=<bytes>     stack scanned for them (default 16384)
//   touch <root>/prof.req    record now (looked for every second; a number in it is the length in s)
//
// Each recording ends in <root>/prof-<n>.txt ("[prof] ..." in the log says so).
//
// How: a sampler thread ("ua_prof") wakes <hz> times a second. For each thread it reads the
// thread's state (/proc/self/task/<tid>/stat, an open file read again):
//   - running (R): a signal (SIGRTMIN+5) makes the thread record itself into a buffer allocated for
//     the recording: pc, lr and its callers. Callers are unwound exactly while the code has unwind
//     tables (32-bit ARM: the .ARM.exidx/.ARM.extab EHABI opcodes of libunity, Mono's runtime, libc,
//     the GLES driver; AArch64: the frame-pointer chain); from where that stops (Mono's JIT code has
//     no such tables) the rest of the stack is scanned for words that are return addresses (the
//     instruction before them is a call: arch.h). Nothing is named, locked or allocated in the
//     handler, and every stack read stays inside the thread's stack mapping.
//     A thread whose last signal is still pending is counted as Q (runnable, not on a CPU, or in a
//     long kernel path);
//   - blocked (S, D): the thread is not disturbed: /proc/self/task/<tid>/syscall gives the system
//     call it waits in, its sp and pc, and the sampler scans its stack from that sp. (Under QEMU the
//     file shows the emulator's own registers: such samples keep only the state.)
// Unity's main thread at 60 fps is thus not woken from its waits, and no wait returns EINTR.
// After the recording, outside any signal handler, the sampler names what it saw: Android
// libraries by the loader's load bias (module+offset, named offline from Unity's symbol files),
// glibc libraries by dladdr (the nearest exported symbol), and Mono's JIT code by method
// (mono_jit_info_table_find + mono_method_full_name, attached to Mono's root domain meanwhile).
// It also writes each thread's CPU time (schedstat: on CPU, waiting for a CPU; utime, stime) over
// the recording, the system's per-core counters (/proc/stat), and the frame times (swaps and
// UnityMain's frames), so a profile can be read against the frame rate.
//
// Memory: (5 + UA_PROF_DEPTH) words per sample, <hz> x UA_PROF_SECS samples per thread, allocated
// when a recording starts and freed when it is written (500 Hz, 20 s, depth 32: 1.4 MB a thread).
//
// File format (text, one record per line, addresses in hex): see write_profile().
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include "prof.h"
#include "arch.h"
#include "hitches.h"
#include "../jni/unityplayer.h"
#include "../loader/so_loader.h"

#define MAX_THREADS 32
#define MAX_RANGES 2048             // executable mappings: libraries, Mono's code chunks
#define MAX_READABLE 4096           // file-backed readable mappings (unwind tables)
#define HDR 9                       // a sample: time (us), state|syscall<<8, depth|exact<<8, pc, lr,
                                    // 4 words of "scanned" bits (one a frame), then the frames
#define BITS (1u << 16)             // prefilter, one bit per MB of address space (hashed above 4 GB)
#define NO_SYSCALL 0xffffu          // a running sample, or a blocked one whose call is unknown
#define NOT_IN_SYSCALL 0xfffeu      // blocked outside a system call (a page fault)

typedef struct { uintptr_t lo, hi; const uint32_t *exidx; int nexidx, anon; } range;
typedef struct { uintptr_t lo, hi; } span;
typedef struct { int n, nrd; range r[MAX_RANGES]; span rd[MAX_READABLE]; uint8_t bits[BITS / 8]; } rtab;

typedef struct {
    _Atomic int tid;                // 0: unused slot
    char name[16];
    int stat_fd, sys_fd, dead, no_signal;
    uintptr_t stk_lo, stk_hi;       // the mapping that holds the stack (0: unknown, no scan)
    _Atomic uintptr_t probe_sp;
    _Atomic unsigned sent, got;
    uintptr_t *buf;                 // g_stride words a sample (HDR, then the frames)
    unsigned cap;
    _Atomic unsigned n;
} pthr;

static int g_hz, g_secs = 20, g_depth = 32, g_scan = 16384, g_stride;
static double g_start_s = -1;
static char g_names[MAX_THREADS][17];
static int g_nnames;
static char g_root[512];
static int g_sig;
static pthr g_thr[MAX_THREADS];
static _Atomic int g_nthr;
static _Atomic int g_recording;
static _Atomic int g_inflight;      // signal handlers and frame hooks that may still write a buffer
static long long g_rec_start;
static rtab g_tabs[2];
static _Atomic(rtab *) g_tab;
static _Atomic long long g_first_frame;
static _Atomic int g_main_tid, g_swap_tid;   // the threads that run Unity's frames and present them
static uint32_t *g_frames[2];       // swap times, UnityMain frame times (us since the start)
static unsigned g_fcap;
static _Atomic unsigned g_nframes[2];
static int g_count;

static long long now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000000ll + t.tv_nsec;
}

static inline unsigned bit_of(uintptr_t v) { return (unsigned)(v >> 20) & (BITS - 1); }

static const range *code_range(const rtab *t, uintptr_t v)
{
    if (!(t->bits[bit_of(v) >> 3] & (1u << (bit_of(v) & 7)))) return NULL;
    int lo = 0, hi = t->n - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (v < t->r[m].lo) hi = m - 1;
        else if (v >= t->r[m].hi) lo = m + 1;
        else return &t->r[m];
    }
    return NULL;
}

// v is a return address in mapped code (async-signal-safe: reads the table and the code only)
static int in_code(const rtab *t, uintptr_t v)
{
    const range *r = code_range(t, v);
    return r && is_return_address(v, r->lo);
}

// [a, a + n) lies in a file-backed readable mapping (where unwind tables are)
static int readable(const rtab *t, uintptr_t a, uintptr_t n)
{
    int lo = 0, hi = t->nrd - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (a < t->rd[m].lo) hi = m - 1;
        else if (a >= t->rd[m].hi) lo = m + 1;
        else return a + n <= t->rd[m].hi && a + n >= a;
    }
    return 0;
}

static inline int stack_word(const pthr *t, uintptr_t a, uintptr_t *v)
{
    if (a < t->stk_lo || a + sizeof *v > t->stk_hi || (a & (sizeof *v - 1))) return 0;
    *v = *(const uintptr_t *)a;
    return 1;
}

#if defined(__arm__)
// ARM EHABI: the unwind opcodes of the function that holds key (from its .ARM.exidx entry, inline
// or in .ARM.extab), -1 when there are none (no table, EXIDX_CANTUNWIND, an unknown model)
static inline uintptr_t prel31(const uint32_t *p) { return (uintptr_t)p + (uintptr_t)(((int32_t)(*p << 1)) >> 1); }
static int ehabi_ops(const rtab *tab, const range *r, uintptr_t key, uint8_t *ops, int max)
{
    const uint32_t *e = r->exidx;
    int lo = 0, hi = r->nexidx - 1, f = -1;
    if (!e || hi < 0) return -1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (prel31(&e[2 * m]) <= key) { f = m; lo = m + 1; } else hi = m - 1;
    }
    if (f < 0 || e[2 * f + 1] == 1) return -1;           // before the first function, or CANTUNWIND
    uint32_t w = e[2 * f + 1], w0;
    const uint32_t *d = NULL, *more;
    int k = 0, words;
    if (w & 0x80000000u) w0 = w;                          // inline (personality 0)
    else {
        d = (const uint32_t *)prel31(&e[2 * f + 1]);
        if (!readable(tab, (uintptr_t)d, 8)) return -1;
        w0 = d[0];
    }
    if (w0 & 0x80000000u) {                               // compact model
        int pers = (w0 >> 24) & 0x0f;
        if (pers == 0) { ops[k++] = (uint8_t)(w0 >> 16); ops[k++] = (uint8_t)(w0 >> 8); ops[k++] = (uint8_t)w0; words = 0; }
        else if ((pers == 1 || pers == 2) && d) { words = (w0 >> 16) & 0xff; ops[k++] = (uint8_t)(w0 >> 8); ops[k++] = (uint8_t)w0; }
        else return -1;
        more = d ? d + 1 : NULL;
    } else {                                              // a personality routine, then the opcodes
        uint32_t w1 = d[1];
        words = (w1 >> 24) & 0xff;
        ops[k++] = (uint8_t)(w1 >> 16); ops[k++] = (uint8_t)(w1 >> 8); ops[k++] = (uint8_t)w1;
        more = d + 2;
    }
    if (words > (max - k) / 4) words = (max - k) / 4;
    if (words && !readable(tab, (uintptr_t)more, (uintptr_t)words * 4)) return -1;
    for (int i = 0; i < words; i++) {
        uint32_t x = more[i];
        ops[k++] = (uint8_t)(x >> 24); ops[k++] = (uint8_t)(x >> 16); ops[k++] = (uint8_t)(x >> 8); ops[k++] = (uint8_t)x;
    }
    return k;
}

// the call before return address v (checked: arch.h) was BLX <register>: how native code enters
// JIT-compiled code (Mono's runtime invoke, Unity's scripting calls), never a direct BL
static inline int call_is_indirect(uintptr_t v)
{
    if (v & 1) return (*(const uint16_t *)((v & ~1u) - 2) & 0xFF87) == 0x4780;
    return (*(const uint32_t *)(v - 4) & 0x0FFFFFF0) == 0x012FFF30;
}

// one frame up: r[] (r0-r15) from the callee's to the caller's; 0 when it cannot be done
static int ehabi_step(const pthr *t, const rtab *tab, uint32_t *r, int first)
{
    uintptr_t pc = r[15] & ~1u, key = first ? pc : pc - 2, v;
    const range *rg = code_range(tab, key);
    uint8_t ops[64];
    int n = rg ? ehabi_ops(tab, rg, key, ops, sizeof ops) : -1, set15 = 0;
    if (n < 0) return 0;
    uintptr_t vsp = r[13];
    for (int i = 0; i < n; ) {
        uint8_t op = ops[i++];
        if ((op & 0xc0) == 0x00) vsp += ((op & 0x3fu) << 2) + 4;
        else if ((op & 0xc0) == 0x40) vsp -= ((op & 0x3fu) << 2) + 4;
        else if ((op & 0xf0) == 0x80) {                   // pop r4-r15 under a mask
            if (i >= n) return 0;
            unsigned mask = ((op & 0x0fu) << 8) | ops[i++];
            uintptr_t nsp = 0;
            int sp_popped = 0;
            if (!mask) return 0;                          // "refuse to unwind"
            for (int b = 0; b < 12; b++) {
                if (!(mask & (1u << b))) continue;
                if (!stack_word(t, vsp, &v)) return 0;
                vsp += 4;
                if (4 + b == 13) { sp_popped = 1; nsp = v; } else r[4 + b] = (uint32_t)v;
                if (4 + b == 15) set15 = 1;
            }
            if (sp_popped) vsp = nsp;
        } else if ((op & 0xf0) == 0x90) {                 // vsp = r[n]
            unsigned reg = op & 0x0fu;
            if (reg == 13 || reg == 15) return 0;
            vsp = r[reg];
        } else if ((op & 0xf0) == 0xa0) {                 // pop r4-r[4+n] (and r14)
            for (unsigned b = 0; b <= (op & 7u); b++) { if (!stack_word(t, vsp, &v)) return 0; vsp += 4; r[4 + b] = (uint32_t)v; }
            if (op & 0x08) { if (!stack_word(t, vsp, &v)) return 0; vsp += 4; r[14] = (uint32_t)v; }
        } else if (op == 0xb0) break;                     // finish
        else if (op == 0xb1) {                            // pop r0-r3 under a mask
            if (i >= n) return 0;
            unsigned mask = ops[i++];
            if (!mask || (mask & 0xf0)) return 0;
            for (int b = 0; b < 4; b++) if (mask & (1u << b)) { if (!stack_word(t, vsp, &v)) return 0; vsp += 4; r[b] = (uint32_t)v; }
        } else if (op == 0xb2) {                          // vsp += 0x204 + (uleb128 << 2)
            uint32_t x = 0;
            int sh = 0;
            uint8_t c;
            do { if (i >= n) return 0; c = ops[i++]; x |= (uint32_t)(c & 0x7f) << sh; sh += 7; } while ((c & 0x80) && sh < 28);
            vsp += 0x204 + ((uintptr_t)x << 2);
        } else if (op == 0xb3 || op == 0xc8 || op == 0xc9) {   // pop VFP registers (FSTMFDX / VPUSH)
            if (i >= n) return 0;
            uint8_t x = ops[i++];
            vsp += ((x & 0x0fu) + 1) * 8 + (op == 0xb3 ? 4 : 0);
        } else if ((op & 0xf8) == 0xb8) vsp += ((op & 7u) + 1) * 8 + 4;
        else if ((op & 0xf8) == 0xd0) vsp += ((op & 7u) + 1) * 8;
        else return 0;                                    // iWMMX, spare opcodes
    }
    if (!set15) r[15] = r[14];
    uintptr_t osp = r[13];
    r[13] = (uint32_t)vsp;
    if (!r[15] || vsp < osp || (vsp == osp && (r[15] & ~1u) == pc) || vsp >= t->stk_hi) return 0;
    return 1;
}
#endif

// one sample into the thread's buffer: from its own signal handler (uc: running) or from the
// sampler (blocked: pc and sp only). The slot is reserved atomically: both may write the buffer.
// Callers: unwound while the code has tables; above that the stack is scanned. On 32-bit ARM a
// scanned return address into code with tables, after one into JIT code reached through BLX
// <register> (how native code calls into JIT code), starts an exact unwind again from there: the
// callee pushed lr at the top of its frame, so the caller's sp is the next word.
static void record(pthr *t, int state, unsigned sysnr, uintptr_t pc, uintptr_t lr, uintptr_t sp, const ucontext_t *uc)
{
    unsigned i = atomic_fetch_add(&t->n, 1);
    if (!t->buf || i >= t->cap) return;
    uintptr_t *s = t->buf + (size_t)i * g_stride, *scanned = s + 5;
    s[0] = (uintptr_t)((now_ns() - g_rec_start) / 1000);
    s[3] = pc;
    s[4] = lr;
    scanned[0] = scanned[1] = scanned[2] = scanned[3] = 0;
    unsigned d = 0, exact = 0, depth = (unsigned)g_depth;
    const rtab *tab = atomic_load(&g_tab);
    uintptr_t from = sp;
    if (depth && sp && t->stk_hi && sp >= t->stk_lo && sp < t->stk_hi) {
#if defined(__arm__)
        uint32_t r[16];
        const range *last = code_range(tab, pc);
        if (uc) {
            const unsigned long *g = &uc->uc_mcontext.arm_r0;          // r0-r10, fp, ip, sp, lr, pc in order
            for (int k = 0; k < 16; k++) r[k] = (uint32_t)g[k];
            const range *rg;
            for (int first = 1; d < depth && ehabi_step(t, tab, r, first) && (rg = code_range(tab, r[15] & ~1u)); first = 0) {
                s[HDR + d++] = r[15];
                from = r[13];
                last = rg;
            }
            exact = d;
        }
        int after_jit = last && last->anon;
#elif defined(__aarch64__)
        if (uc) {                                          // the frame records: [fp] = caller's fp, [fp + 8] = lr
            uintptr_t fp = uc->uc_mcontext.regs[29], v, ret;
            while (d < depth && fp >= sp && stack_word(t, fp, &v) && stack_word(t, fp + 8, &ret) && ret) {
                s[HDR + d++] = ret;
                from = fp + 16;
                if (v <= fp) break;
                fp = v;
            }
            exact = d;
        }
#endif
        uintptr_t end = t->stk_hi - from > (uintptr_t)g_scan ? from + g_scan : t->stk_hi;
        for (const uintptr_t *w = (const uintptr_t *)((from + sizeof(uintptr_t) - 1) & ~(sizeof(uintptr_t) - 1));
             (uintptr_t)(w + 1) <= end && d < depth; w++) {
            uintptr_t v = *w;
            const range *rg = code_range(tab, v);
            if (!rg || !is_return_address(v, rg->lo)) continue;
#if defined(__arm__)
            if (uc && rg->exidx && (!after_jit || call_is_indirect(v))) {
                memset(r, 0, sizeof r);
                r[15] = (uint32_t)v;
                r[13] = (uint32_t)(uintptr_t)(w + 1);
                unsigned at = d;
                s[HDR + d++] = v;
                const range *rg2 = rg;
                while (d < depth && ehabi_step(t, tab, r, 0) && (rg2 = code_range(tab, r[15] & ~1u))) s[HDR + d++] = r[15];
                if (d == at + 1) scanned[at >> 5] |= (uintptr_t)1 << (at & 31);   // nothing unwound from it: a scan's find
                after_jit = rg2 && rg2->anon;
                w = (const uintptr_t *)(uintptr_t)r[13] - 1;   // the scan goes on above the unwound frames
                if ((uintptr_t)(w + 1) < from) break;
                from = (uintptr_t)(w + 1);
                continue;
            }
            after_jit = rg->anon;
#endif
            scanned[d >> 5] |= (uintptr_t)1 << (d & 31);
            s[HDR + d++] = v;
        }
    }
    s[2] = (uintptr_t)d | (uintptr_t)exact << 8;
    s[1] = (uintptr_t)(state & 0xff) | (uintptr_t)(sysnr & 0xffff) << 8;
}

static void on_signal(int sig, siginfo_t *si, void *ctx)
{
    (void)sig; (void)si;
    int e = errno;
    atomic_fetch_add(&g_inflight, 1);
    int me = (int)syscall(SYS_gettid), n = atomic_load(&g_nthr);
    for (int i = 0; i < n; i++) {
        pthr *t = &g_thr[i];
        if (atomic_load_explicit(&t->tid, memory_order_relaxed) != me) continue;
        ucontext_t *uc = ctx;
        uintptr_t sp = UC_SP(uc);
        if (!atomic_load(&t->probe_sp)) atomic_store(&t->probe_sp, sp);
        if (atomic_load(&g_recording)) record(t, 'R', NO_SYSCALL, UC_PC(uc), UC_LR(uc), sp, uc);
        atomic_fetch_add(&t->got, 1);
        break;
    }
    atomic_fetch_sub(&g_inflight, 1);
    errno = e;
}

// ---- the sampler's side: maps, threads ---------------------------------------------------------

typedef struct { uintptr_t lo, hi; char perms[5]; char path[128]; unsigned long off; } mapline;

// every mapping of the process (malloc'd; *n entries)
static mapline *read_maps(int *n)
{
    FILE *f = fopen("/proc/self/maps", "r");
    int cap = 1024, k = 0;
    mapline *v = malloc(cap * sizeof *v);
    char line[600];
    if (!f || !v) { if (f) fclose(f); free(v); *n = 0; return NULL; }
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi, off;
        char perms[8], path[512] = "";
        if (sscanf(line, "%lx-%lx %7s %lx %*s %*s %511[^\n]", &lo, &hi, perms, &off, path) < 4) continue;
        if (k == cap) { mapline *w = realloc(v, (cap *= 2) * sizeof *v); if (!w) break; v = w; }
        v[k].lo = lo; v[k].hi = hi; v[k].off = off;
        snprintf(v[k].perms, sizeof v[k].perms, "%.4s", perms);
        snprintf(v[k].path, sizeof v[k].path, "%.127s", path);
        k++;
    }
    fclose(f);
    *n = k;
    return v;
}

// the code ranges into the table the handlers are not reading, then switch them over (handlers
// that still read the old one finish within microseconds; the next rebuild is seconds away)
static void build_table(const mapline *m, int n)
{
    rtab *t = atomic_load(&g_tab) == &g_tabs[0] ? &g_tabs[1] : &g_tabs[0];
    memset(t->bits, 0, sizeof t->bits);
    t->n = t->nrd = 0;
    for (int i = 0; i < n && t->nrd < MAX_READABLE; i++) {     // where unwind tables can be read
        if (m[i].perms[0] != 'r' || m[i].path[0] == '[' || (!m[i].path[0] && !so_module_at((void *)m[i].lo, NULL))) continue;
        if (t->nrd && t->rd[t->nrd - 1].hi == m[i].lo) t->rd[t->nrd - 1].hi = m[i].hi;
        else { t->rd[t->nrd].lo = m[i].lo; t->rd[t->nrd].hi = m[i].hi; t->nrd++; }
    }
    for (int i = 0; i < n && t->n < MAX_RANGES; i++) {
        if (m[i].perms[0] != 'r' || m[i].perms[2] != 'x') continue;
        if (!strcmp(m[i].path, "[vectors]") || !strcmp(m[i].path, "[vsyscall]")) continue;
        range *r = &t->r[t->n++];
        r->lo = m[i].lo; r->hi = m[i].hi; r->exidx = NULL; r->nexidx = 0;
        r->anon = !m[i].path[0] && !so_module_at((void *)m[i].lo, NULL);   // Mono's JIT code, trampolines
        int cnt = 0;
        // the loader's modules, else glibc's (dl_iterate_phdr: an address inside the segment, not its page)
        uintptr_t x = so_find_exidx(m[i].lo + (m[i].hi - m[i].lo) / 2, &cnt);
        if (x && cnt > 0 && readable(t, x, (uintptr_t)cnt * 8)) { r->exidx = (const uint32_t *)x; r->nexidx = cnt; }
        for (uintptr_t a = m[i].lo; ; a += 1u << 20) {
            unsigned b = bit_of(a);
            t->bits[b >> 3] |= (uint8_t)(1u << (b & 7));
            if (a >= m[i].hi - 1 || a + (1u << 20) < a) { b = bit_of(m[i].hi - 1); t->bits[b >> 3] |= (uint8_t)(1u << (b & 7)); break; }
        }
    }
    atomic_store(&g_tab, t);
}

static int name_matches(const char *comm, int tid)
{
    // threads UnityMain starts carry its name until they set their own: UnityMain is the one that runs the frames
    if (!strcmp(comm, "UnityMain") && atomic_load(&g_main_tid) && tid != atomic_load(&g_main_tid)) return 0;
    for (int k = 0; k < g_nnames; k++) {
        if (!strcmp(g_names[k], "@swap")) { if (tid == atomic_load(&g_swap_tid)) return 1; continue; }
        size_t l = strlen(g_names[k]);
        if (l && g_names[k][l - 1] == '*' ? !strncmp(comm, g_names[k], l - 1) : !strcmp(comm, g_names[k])) return 1;
    }
    return 0;
}

static int read_comm(int tid, char *out, size_t n)
{
    char p[64];
    snprintf(p, sizeof p, "/proc/self/task/%d/comm", tid);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    int ok = fgets(out, (int)n, f) != NULL;
    fclose(f);
    if (ok) out[strcspn(out, "\n")] = 0;
    return ok;
}

// the threads to sample: slots kept for threads seen before, new ones appended (the handler finds
// a slot by tid: a slot is complete before its tid is set and g_nthr counts it)
static void find_threads(void)
{
    int n = atomic_load(&g_nthr);
    for (int i = 0; i < n; i++) {
        pthr *t = &g_thr[i];
        char c[32];
        int tid = atomic_load(&t->tid);
        if (!tid) continue;
        if (t->dead || !read_comm(tid, c, sizeof c) || !name_matches(c, tid)) {
            atomic_store(&t->tid, 0);
            if (t->stat_fd >= 0) close(t->stat_fd);
            if (t->sys_fd >= 0) close(t->sys_fd);
            t->stat_fd = t->sys_fd = -1;
        }
    }
    DIR *d = opendir("/proc/self/task");
    struct dirent *e;
    int self = (int)syscall(SYS_gettid);
    while (d && (e = readdir(d))) {
        int tid = atoi(e->d_name);
        char c[32];
        if (tid <= 0 || tid == self || !read_comm(tid, c, sizeof c) || !name_matches(c, tid)) continue;
        int have = 0;
        for (int i = 0; i < n; i++) if (atomic_load(&g_thr[i].tid) == tid) have = 1;
        if (have) continue;
        int slot = -1;
        for (int i = 0; i < n && slot < 0; i++) if (!atomic_load(&g_thr[i].tid)) slot = i;
        if (slot < 0 && n < MAX_THREADS) slot = n;
        if (slot < 0) break;
        pthr *t = &g_thr[slot];
        char p[64];
        snprintf(t->name, sizeof t->name, "%.15s", c);
        for (char *q = t->name; *q; q++) if (*q == ' ') *q = '_';
        snprintf(p, sizeof p, "/proc/self/task/%d/stat", tid);
        t->stat_fd = open(p, O_RDONLY | O_CLOEXEC);
        snprintf(p, sizeof p, "/proc/self/task/%d/syscall", tid);
        t->sys_fd = open(p, O_RDONLY | O_CLOEXEC);
        t->dead = t->no_signal = 0;
        t->stk_lo = t->stk_hi = 0;
        t->buf = NULL; t->cap = 0;
        atomic_store(&t->n, 0);
        atomic_store(&t->probe_sp, 0);
        atomic_store(&t->sent, 0); atomic_store(&t->got, 0);
        atomic_store(&t->tid, tid);
        if (slot == n) atomic_store(&g_nthr, ++n);
    }
    if (d) closedir(d);
}

// a blocked thread's system call, sp and pc from /proc/self/task/<tid>/syscall: 0 = it runs again
static int blocked_at(pthr *t, unsigned *nr, uintptr_t *sp, uintptr_t *pc)
{
    char s[256];
    *nr = NO_SYSCALL; *sp = *pc = 0;
    ssize_t k = t->sys_fd >= 0 ? pread(t->sys_fd, s, sizeof s - 1, 0) : -1;
    if (k <= 0) return 1;
    s[k] = 0;
    if (s[0] == 'r') return 0;                         // "running"
    char *tok[10];
    int n = 0;
    for (char *p = strtok(s, " \n"); p && n < 10; p = strtok(NULL, " \n")) tok[n++] = p;
    if (n < 3) return 1;
    long v = strtol(tok[0], NULL, 0);
    uintptr_t xsp = (uintptr_t)strtoull(tok[n - 2], NULL, 0), xpc = (uintptr_t)strtoull(tok[n - 1], NULL, 0);
    if (!t->stk_hi || xsp < t->stk_lo || xsp >= t->stk_hi) return 1;   // not this process's view (QEMU)
    *nr = v >= 0 ? (unsigned)v : NOT_IN_SYSCALL;
    *sp = xsp; *pc = xpc;
    return 1;
}

static char thread_state(pthr *t)
{
    char b[96];
    ssize_t k = t->stat_fd >= 0 ? pread(t->stat_fd, b, sizeof b - 1, 0) : -1;
    if (k <= 0) return 'X';
    b[k] = 0;
    char *p = strrchr(b, ')');
    return p && p[1] == ' ' && p[2] ? p[2] : '?';
}

static void signal_thread(pthr *t)
{
    if (t->no_signal) { record(t, 'R', NO_SYSCALL, 0, 0, 0, NULL); return; }
    if ((int)(atomic_load(&t->sent) - atomic_load(&t->got)) > 0) { record(t, 'Q', NO_SYSCALL, 0, 0, 0, NULL); return; }
    atomic_fetch_add(&t->sent, 1);
    if (syscall(SYS_tgkill, getpid(), atomic_load(&t->tid), g_sig) != 0) { atomic_fetch_sub(&t->sent, 1); t->dead = 1; }
}

static void tick(pthr *t)
{
    char st = thread_state(t);
    if (st == 'R') { signal_thread(t); return; }
    if (st == 'X' || st == 'Z' || st == 'x') { t->dead = 1; return; }
    unsigned nr;
    uintptr_t sp, pc;
    if (!blocked_at(t, &nr, &sp, &pc)) { signal_thread(t); return; }
    record(t, st, nr, pc, 0, sp, NULL);
}

// ---- per-thread and per-core counters ------------------------------------------------------

typedef struct { int tid; char name[16]; unsigned long long run, wait, slices; unsigned long ut, st; } tcpu;

static tcpu *cpu_snapshot(int *n)
{
    int cap = 64, k = 0;
    tcpu *v = malloc(cap * sizeof *v);
    DIR *d = opendir("/proc/self/task");
    struct dirent *e;
    while (v && d && (e = readdir(d))) {
        int tid = atoi(e->d_name);
        if (tid <= 0) continue;
        if (k == cap) { tcpu *w = realloc(v, (cap *= 2) * sizeof *v); if (!w) break; v = w; }
        tcpu *c = &v[k];
        memset(c, 0, sizeof *c);
        c->tid = tid;
        if (!read_comm(tid, c->name, sizeof c->name)) continue;
        for (char *p = c->name; *p; p++) if (*p == ' ') *p = '_';
        char p[64], b[1024];
        snprintf(p, sizeof p, "/proc/self/task/%d/schedstat", tid);
        FILE *f = fopen(p, "r");
        if (f) { if (fscanf(f, "%llu %llu %llu", &c->run, &c->wait, &c->slices) != 3) c->run = c->wait = c->slices = 0; fclose(f); }
        snprintf(p, sizeof p, "/proc/self/task/%d/stat", tid);
        f = fopen(p, "r");
        if (f) {
            if (fgets(b, sizeof b, f)) {
                char *q = strrchr(b, ')');
                // fields after the comm: state(3) ... utime(14) stime(15)
                if (q && sscanf(q + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &c->ut, &c->st) != 2) c->ut = c->st = 0;
            }
            fclose(f);
        }
        k++;
    }
    if (d) closedir(d);
    *n = k;
    return v;
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    size_t cap = 4096, n = 0;
    char *b = malloc(cap);
    size_t r;
    while (b && (r = fread(b + n, 1, cap - n - 1, f)) > 0) {
        n += r;
        if (n + 1 == cap) { char *w = realloc(b, cap *= 2); if (!w) break; b = w; }
    }
    fclose(f);
    if (b) b[n] = 0;
    return b;
}

// ---- naming, after a recording ---------------------------------------------------------------

static void *(*m_root)(void);
static void *(*m_attach)(void *);
static void (*m_detach)(void *);
static void *(*m_ji_find)(void *, void *);
static void *(*m_ji_method)(void *);
static void *(*m_ji_start)(void *);
static int (*m_ji_size)(void *);
static char *(*m_full_name)(void *, int);
static void (*m_free)(void *);

static int mono_ready(void)
{
    if (m_root) return 1;
    void *h = mono_runtime_lib();
    if (!h) return 0;
    m_attach = so_dlsym(h, "mono_thread_attach"); m_detach = so_dlsym(h, "mono_thread_detach");
    m_ji_find = so_dlsym(h, "mono_jit_info_table_find"); m_ji_method = so_dlsym(h, "mono_jit_info_get_method");
    m_ji_start = so_dlsym(h, "mono_jit_info_get_code_start"); m_ji_size = so_dlsym(h, "mono_jit_info_get_code_size");
    m_full_name = so_dlsym(h, "mono_method_full_name"); m_free = so_dlsym(h, "mono_free");
    void *root = so_dlsym(h, "mono_get_root_domain");
    if (!m_attach || !m_ji_find || !m_ji_method || !m_ji_start || !m_ji_size || !m_full_name || !m_free || !root) return 0;
    m_root = root;
    return 1;
}

// "Namespace.Class:Method" from Mono's full name: wrappers as W:, the signature cut, spaces as _
static void method_token(const char *full, char *out, size_t n)
{
    const char *c = full;
    size_t k = (size_t)snprintf(out, n, "M:");
    if (!strncmp(c, "(wrapper ", 9) && strchr(c, ')')) { c = strchr(c, ')') + 1; while (*c == ' ') c++; k = (size_t)snprintf(out, n, "W:"); }
    for (; *c && k < n - 1; c++) {
        if (*c == ' ' && c[1] == '(') break;
        out[k++] = *c == ' ' ? '_' : *c;
    }
    out[k] = 0;
}

static int cmp_uptr(const void *a, const void *b)
{
    uintptr_t x = *(const uintptr_t *)a, y = *(const uintptr_t *)b;
    return x < y ? -1 : x > y;
}

static const uintptr_t *g_sort_buf;     // for cmp_sample (the sampler thread alone sorts)
static int cmp_sample(const void *a, const void *b)
{
    const uintptr_t *x = g_sort_buf + (size_t)*(const uint32_t *)a * g_stride, *y = g_sort_buf + (size_t)*(const uint32_t *)b * g_stride;
    for (unsigned i = 1; i < HDR; i++) if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    for (unsigned i = HDR; i < HDR + (x[2] & 0xff); i++) if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}

static int same_stack(const uintptr_t *x, const uintptr_t *y)
{
    for (unsigned i = 1; i < HDR + (x[2] & 0xff); i++) if (x[i] != y[i]) return 0;
    return 1;
}

static const char *base_name(const char *p) { const char *b = strrchr(p, '/'); return b ? b + 1 : p; }

// File format (one record per line; addresses in hex without 0x; times in us since the start):
//   ua_prof 3                       format version (2: no ~ marks, callers after <exact> scanned;
//                                    1: no <exact> field)
//   arch armhf|arm64, hz <n>, secs <s>, depth <n> scan <bytes>, signal <n>, mono yes|no
//   thread <i> <tid> <name> <samples> <dropped>     the sampled threads
//   module <lo> <hi> <bias> <name>  code an address falls in: <name>+(address - bias), an ELF address
//                                    (Android libraries by the loader's bias, others by dladdr's base)
//   jit <start> <size> <name>       a JIT-compiled method (M:Namespace.Class:Method, W: wrappers)
//   sym <address> <symbol> <offset> dladdr's nearest exported symbol, for glibc libraries
//   cpu <tid> <name> <on-cpu ns> <runqueue ns> <slices> <utime> <stime>   deltas over the recording
//   stat <cpuN> <user> <nice> <system> <idle> <iowait> <irq> <softirq>  /proc/stat deltas (ticks)
//   stack <id> <count> <thread> <state> <syscall> <pc> <lr> <exact> <callers...>
//       state R (on a CPU, sampled by its signal), S/D (blocked; syscall = the call's number,
//       fffe outside a call, ffff unknown), Q (runnable, its signal still pending: no addresses);
//       the first <exact> callers were unwound from the sample's registers (lr is then already
//       among them); a caller written ~<address> was found by scanning the stack, the others
//       were unwound (also after a scanned one, from a call into JIT code)
//   seq <thread> <us>:<stack id> ...   every sample of that thread in time order
//   swap <us> ...                   presented frames (eglSwapBuffers)
//   main <us> ...                   UnityMain's frames (after each nativeRender)
//   end
static void write_profile(double secs, const tcpu *c0, int n0, const tcpu *c1, int n1, const char *st0, const char *st1)
{
    char path[600], tmp[620];
    snprintf(path, sizeof path, "%s/prof-%d.txt", g_root, ++g_count);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) { fprintf(stderr, "[prof] %s: %s\n", tmp, strerror(errno)); return; }
    int nthr = atomic_load(&g_nthr);
    fprintf(f, "ua_prof 3\narch %s\nhz %d\nsecs %.3f\ndepth %d scan %d\nsignal %d\n",
#if defined(__aarch64__)
            "arm64",
#else
            "armhf",
#endif
            g_hz, secs, g_depth, g_scan, g_sig);
    // every address once, sorted: modules and methods come in runs
    size_t total = 0;
    for (int i = 0; i < nthr; i++) {
        pthr *t = &g_thr[i];
        unsigned n = atomic_load(&t->n) < t->cap ? atomic_load(&t->n) : t->cap;
        if (!t->buf) continue;
        for (unsigned k = 0; k < n; k++) total += 2 + (t->buf[(size_t)k * g_stride + 2] & 0xff);
    }
    uintptr_t *addrs = malloc((total + 1) * sizeof *addrs);
    size_t na = 0;
    for (int i = 0; addrs && i < nthr; i++) {
        pthr *t = &g_thr[i];
        unsigned n = atomic_load(&t->n) < t->cap ? atomic_load(&t->n) : t->cap;
        if (!t->buf) continue;
        for (unsigned k = 0; k < n; k++) {
            const uintptr_t *s = t->buf + (size_t)k * g_stride;
            unsigned d = (unsigned)(s[2] & 0xff);
            for (unsigned j = 3; j < HDR + d; j++) if (s[j]) addrs[na++] = s[j];
        }
    }
    if (addrs) qsort(addrs, na, sizeof *addrs, cmp_uptr);
    size_t u = 0;
    for (size_t i = 0; i < na; i++) if (!u || addrs[i] != addrs[u - 1]) addrs[u++] = addrs[i];
    na = u;
    // Mono: attached while it names its methods (its shutdown waits for attached threads)
    int mono = !up_is_finishing() && mono_ready();
    void *domain = mono ? m_root() : NULL, *mthread = domain ? m_attach(domain) : NULL;
    fprintf(f, "mono %s\n", domain ? "yes" : "no");
    int nmaps = 0;
    mapline *maps = read_maps(&nmaps);
    uintptr_t mod_lo = 0, mod_hi = 0, jit_lo = 0, jit_hi = 0, last_line = 1;
    int mi = 0;
    for (size_t i = 0; i < na; i++) {
        uintptr_t a = addrs[i];
        if (a >= mod_lo && a < mod_hi) continue;           // in the mapping of the last address
        if (a >= jit_lo && a < jit_hi) continue;           // in the method of the last address
        while (mi < nmaps && maps[mi].hi <= a) mi++;
        const mapline *m = mi < nmaps && maps[mi].lo <= a ? &maps[mi] : NULL;
        uintptr_t lo = m ? m->lo : a, hi = m ? m->hi : a + 1, bias;
        const char *lib = so_module_at((void *)a, &bias);
        if (lib) {                                          // an Android library: by the loader's bias
            fprintf(f, "module %lx %lx %lx %s\n", (unsigned long)lo, (unsigned long)hi, (unsigned long)bias, base_name(lib));
            mod_lo = lo; mod_hi = hi;
            continue;
        }
        if (m && m->path[0] && m->path[0] != '[') {        // a glibc library or the host program
            Dl_info di;
            int ok = dladdr((void *)a, &di) && di.dli_fbase;
            if (lo != last_line)
                fprintf(f, "module %lx %lx %lx %s\n", (unsigned long)lo, (unsigned long)hi,
                        (unsigned long)(ok ? (uintptr_t)di.dli_fbase : lo - m->off), base_name(m->path));
            last_line = lo;
            if (ok && di.dli_sname && di.dli_saddr)        // each address its nearest exported symbol
                fprintf(f, "sym %lx %s %lx\n", (unsigned long)a, di.dli_sname, (unsigned long)(a - (uintptr_t)di.dli_saddr));
            else if (!ok) { mod_lo = lo; mod_hi = hi; }
            continue;
        }
        if (domain) {                                      // Mono's JIT code
            void *ji = m_ji_find(domain, (void *)a);
            if (ji) {
                uintptr_t st = (uintptr_t)m_ji_start(ji);
                int sz = m_ji_size(ji);
                char *full = m_full_name(m_ji_method(ji), 1), tok[400];
                if (full) { method_token(full, tok, sizeof tok); m_free(full); } else snprintf(tok, sizeof tok, "M:?");
                fprintf(f, "jit %lx %x %s\n", (unsigned long)st, (unsigned)sz, tok);
                jit_lo = st; jit_hi = st + (uintptr_t)(sz > 0 ? sz : 1);
                if (a < jit_lo || a >= jit_hi) jit_lo = jit_hi = 0;
            }
        }
    }
    if (mthread && m_detach) m_detach(mthread);
    free(maps);
    free(addrs);
    for (int i = 0; i < nthr; i++) {
        pthr *t = &g_thr[i];
        if (!t->buf) continue;
        unsigned n = atomic_load(&t->n);
        fprintf(f, "thread %d %d %s %u %u\n", i, atomic_load(&t->tid) ? atomic_load(&t->tid) : 0, t->name, n < t->cap ? n : t->cap, n > t->cap ? n - t->cap : 0);
    }
    for (int i = 0; i < n1; i++) {
        const tcpu *b = &c1[i], *a = NULL;
        for (int k = 0; k < n0 && !a; k++) if (c0[k].tid == b->tid) a = &c0[k];
        fprintf(f, "cpu %d %s %llu %llu %llu %lu %lu\n", b->tid, b->name, b->run - (a ? a->run : 0), b->wait - (a ? a->wait : 0),
                b->slices - (a ? a->slices : 0), b->ut - (a ? a->ut : 0), b->st - (a ? a->st : 0));
    }
    // /proc/stat: the cpu lines' deltas
    if (st0 && st1) {
        const char *p1 = st1;
        while (!strncmp(p1, "cpu", 3)) {
            char name[16];
            unsigned long long v1[7] = {0}, v0[7] = {0};
            if (sscanf(p1, "%15s %llu %llu %llu %llu %llu %llu %llu", name, &v1[0], &v1[1], &v1[2], &v1[3], &v1[4], &v1[5], &v1[6]) == 8) {
                const char *p0 = st0;
                while (!strncmp(p0, "cpu", 3)) {
                    char n0n[16];
                    if (sscanf(p0, "%15s %llu %llu %llu %llu %llu %llu %llu", n0n, &v0[0], &v0[1], &v0[2], &v0[3], &v0[4], &v0[5], &v0[6]) == 8 && !strcmp(n0n, name)) break;
                    memset(v0, 0, sizeof v0);
                    p0 = strchr(p0, '\n'); if (!p0) break; p0++;
                }
                fprintf(f, "stat %s", name);
                for (int k = 0; k < 7; k++) fprintf(f, " %llu", v1[k] - v0[k]);
                fputc('\n', f);
            }
            p1 = strchr(p1, '\n'); if (!p1) break; p1++;
        }
    }
    // stacks: each thread's samples sorted, equal ones counted; then the samples in time order
    int id = 0;
    for (int i = 0; i < nthr; i++) {
        pthr *t = &g_thr[i];
        if (!t->buf) continue;
        unsigned n = atomic_load(&t->n) < t->cap ? atomic_load(&t->n) : t->cap;
        uint32_t *idx = malloc((n + 1) * sizeof *idx), *ids = malloc((n + 1) * sizeof *ids);
        if (!idx || !ids) { free(idx); free(ids); continue; }
        for (unsigned k = 0; k < n; k++) idx[k] = k;
        g_sort_buf = t->buf;
        qsort(idx, n, sizeof *idx, cmp_sample);
        for (unsigned k = 0; k < n; ) {
            const uintptr_t *s = t->buf + (size_t)idx[k] * g_stride;
            unsigned e = k + 1;
            while (e < n && same_stack(s, t->buf + (size_t)idx[e] * g_stride)) e++;
            unsigned d = (unsigned)(s[2] & 0xff), exact = (unsigned)(s[2] >> 8) & 0xff, sysnr = (unsigned)(s[1] >> 8) & 0xffff;
            fprintf(f, "stack %d %u %d %c %x %lx %lx %u", id, e - k, i, (char)(s[1] & 0xff), sysnr, (unsigned long)s[3], (unsigned long)s[4], exact);
            for (unsigned j = 0; j < d; j++)
                fprintf(f, " %s%lx", (s[5 + (j >> 5)] >> (j & 31)) & 1 ? "~" : "", (unsigned long)s[HDR + j]);
            fputc('\n', f);
            for (unsigned j = k; j < e; j++) ids[idx[j]] = (uint32_t)id;
            id++;
            k = e;
        }
        for (unsigned k = 0; k < n; k++) {
            if (k % 16 == 0) fprintf(f, "%sseq %d", k ? "\n" : "", i);
            fprintf(f, " %lu:%u", (unsigned long)t->buf[(size_t)k * g_stride], ids[k]);
        }
        if (n) fputc('\n', f);
        free(idx); free(ids);
    }
    static const char *const fnames[2] = { "swap", "main" };
    for (int w = 0; w < 2; w++) {
        unsigned n = atomic_load(&g_nframes[w]) < g_fcap ? atomic_load(&g_nframes[w]) : g_fcap;
        for (unsigned k = 0; k < n; k++) {
            if (k % 20 == 0) fprintf(f, "%s%s", k ? "\n" : "", fnames[w]);
            fprintf(f, " %u", g_frames[w][k]);
        }
        if (n) fputc('\n', f);
    }
    fprintf(f, "end\n");
    fclose(f);
    if (rename(tmp, path) != 0) { fprintf(stderr, "[prof] %s: %s\n", path, strerror(errno)); return; }
    // one line for the log
    char line[1024];
    int len = snprintf(line, sizeof line, "[prof] %s: %.1f s", base_name(path), secs);
    for (int i = 0; i < nthr && len < (int)sizeof line - 120; i++) {
        pthr *t = &g_thr[i];
        if (!t->buf) continue;
        unsigned n = atomic_load(&t->n) < t->cap ? atomic_load(&t->n) : t->cap, r = 0, q = 0, b = 0;
        for (unsigned k = 0; k < n; k++) {
            char s = (char)(t->buf[(size_t)k * g_stride + 1] & 0xff);   // (the state)
            if (s == 'R') r++; else if (s == 'Q') q++; else b++;
        }
        if (n) len += snprintf(line + len, sizeof line - len, ", %s %u samples (running %.0f%%, waiting for a CPU %.0f%%, blocked %.0f%%)",
                               t->name, n, 100.0 * r / n, 100.0 * q / n, 100.0 * b / n);
    }
    unsigned nf = atomic_load(&g_nframes[0]);
    snprintf(line + len, sizeof line - len, ", %u frames (%.1f fps)", nf, secs > 0 ? nf / secs : 0);
    fprintf(stderr, "%s\n", line);
}

static void record_for(double secs)
{
    find_threads();
    int n = atomic_load(&g_nthr), live = 0;
    for (int i = 0; i < n; i++) if (atomic_load(&g_thr[i].tid)) live++;
    if (!live) { fprintf(stderr, "[prof] no thread to sample (UA_PROF_THREADS)\n"); return; }
    // one signal each tells where the stacks are; a blocked thread's sp also comes from /proc
    for (int i = 0; i < n; i++) {
        pthr *t = &g_thr[i];
        if (!atomic_load(&t->tid)) continue;
        atomic_store(&t->probe_sp, 0);
        atomic_store(&t->sent, 0); atomic_store(&t->got, 0);
        if (syscall(SYS_tgkill, getpid(), atomic_load(&t->tid), g_sig) != 0) t->dead = 1;
    }
    for (int w = 0; w < 100; w++) {
        int waiting = 0;
        for (int i = 0; i < n; i++) if (atomic_load(&g_thr[i].tid) && !g_thr[i].dead && !atomic_load(&g_thr[i].probe_sp)) waiting++;
        if (!waiting) break;
        struct timespec ts = { 0, 2000000 };
        nanosleep(&ts, NULL);
    }
    int nmaps = 0;
    mapline *maps = read_maps(&nmaps);
    build_table(maps, nmaps);
    size_t bytes = 0;
    unsigned cap = (unsigned)(secs * g_hz * 1.1) + 64;
    for (int i = 0; i < n; i++) {
        pthr *t = &g_thr[i];
        if (!atomic_load(&t->tid)) continue;
        uintptr_t sp = atomic_load(&t->probe_sp);
        t->no_signal = !sp;
        if (!sp) fprintf(stderr, "[prof] %s does not answer signal %d: its running samples have no addresses\n", t->name, g_sig);
        t->stk_lo = t->stk_hi = 0;
        for (int k = 0; k < nmaps; k++) if (sp >= maps[k].lo && sp < maps[k].hi) { t->stk_lo = maps[k].lo; t->stk_hi = maps[k].hi; break; }
        size_t sz = (size_t)cap * g_stride * sizeof(uintptr_t);
        void *b = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        t->buf = b == MAP_FAILED ? NULL : b;
        t->cap = t->buf ? cap : 0;
        atomic_store(&t->n, 0);
        atomic_store(&t->sent, 0); atomic_store(&t->got, 0);
        bytes += t->buf ? sz : 0;
    }
    free(maps);
    unsigned fcap = (unsigned)(secs * 250) + 64;        // up to 250 frames a second
    for (int w = 0; w < 2; w++) {
        atomic_store(&g_nframes[w], 0);
        if (fcap > g_fcap || !g_frames[w]) { uint32_t *p = realloc(g_frames[w], fcap * sizeof(uint32_t)); if (p) g_frames[w] = p; }
    }
    if (g_frames[0] && g_frames[1] && fcap > g_fcap) g_fcap = fcap;
    int n0 = 0, n1 = 0;
    tcpu *c0 = cpu_snapshot(&n0);
    char *st0 = read_file("/proc/stat");
    char names[200] = "";
    for (int i = 0; i < n; i++) if (atomic_load(&g_thr[i].tid))
        snprintf(names + strlen(names), sizeof names - strlen(names), "%s%s", names[0] ? ", " : "", g_thr[i].name);
    fprintf(stderr, "[prof] recording %.0f s of %s at %d Hz (%zu KB)\n", secs, names, g_hz, bytes >> 10);
    g_rec_start = now_ns();
    atomic_store(&g_recording, 1);
    long long period = 1000000000ll / g_hz, next = g_rec_start, end = g_rec_start + (long long)(secs * 1e9), refresh = g_rec_start + 2000000000ll;
    int aborted = 0;
    while (now_ns() < end) {
        next += period;
        struct timespec ts = { (time_t)(next / 1000000000ll), (long)(next % 1000000000ll) };
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
        long long t = now_ns();
        if (t > next + period) next = t;                   // late: no burst of catch-up samples
        if (up_is_finishing()) { aborted = 1; break; }
        for (int i = 0; i < n; i++) if (atomic_load(&g_thr[i].tid) && !g_thr[i].dead) tick(&g_thr[i]);
        if (t >= refresh) {                                // new code (JIT chunks, libraries)
            maps = read_maps(&nmaps);
            if (maps) build_table(maps, nmaps);
            free(maps);
            refresh = t + 2000000000ll;
        }
    }
    double took = (now_ns() - g_rec_start) / 1e9;
    atomic_store(&g_recording, 0);
    // signals still on their way, handlers still writing: the buffers stay until they are done
    for (int w = 0; w < 500 && (w < 10 || atomic_load(&g_inflight)); w++) { struct timespec ts = { 0, 2000000 }; nanosleep(&ts, NULL); }
    int busy = atomic_load(&g_inflight) != 0;
    tcpu *c1 = cpu_snapshot(&n1);
    char *st1 = read_file("/proc/stat");
    if (aborted) fprintf(stderr, "[prof] Unity is quitting: recording stopped after %.1f s\n", took);
    if (!aborted) write_profile(took, c0, n0, c1, n1, st0, st1);
    free(c0); free(c1); free(st0); free(st1);
    for (int i = 0; i < n; i++) {
        pthr *t = &g_thr[i];
        if (t->buf && !busy) munmap(t->buf, (size_t)t->cap * g_stride * sizeof(uintptr_t));   // (else kept: a handler is stuck)
        t->buf = NULL; t->cap = 0;
        atomic_store(&t->n, 0);
    }
}

static void *sampler(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "ua_prof");
    char req[600];
    snprintf(req, sizeof req, "%s/prof.req", g_root);
    for (int k = 0; ; k++) {
        struct timespec ts = { 0, 250000000 };
        nanosleep(&ts, NULL);
        if (up_is_finishing()) return NULL;
        double secs = 0;
        if (k % 4 == 0 && access(req, F_OK) == 0) {
            char *s = read_file(req);
            secs = s && atof(s) > 0 ? atof(s) : g_secs;
            free(s);
            unlink(req);
        } else if (g_start_s >= 0 && atomic_load(&g_first_frame) && now_ns() - atomic_load(&g_first_frame) >= (long long)(g_start_s * 1e9)) {
            secs = g_secs;
            g_start_s = -1;
        }
        if (secs > 0) record_for(secs);
    }
    return NULL;
}

static void frame_at(int w)
{
    if (!g_hz) return;
    long long t = now_ns();
    long long z = 0;
    if (w == 0 && !atomic_load(&g_first_frame)) atomic_compare_exchange_strong(&g_first_frame, &z, t);
    if (!atomic_load(&g_recording)) return;
    atomic_fetch_add(&g_inflight, 1);
    unsigned i = atomic_fetch_add(&g_nframes[w], 1);
    uint32_t *f = g_frames[w];
    if (atomic_load(&g_recording) && f && i < g_fcap) f[i] = (uint32_t)((t - g_rec_start) / 1000);
    atomic_fetch_sub(&g_inflight, 1);
}

void prof_swap(void)
{
    if (g_hz && !atomic_load(&g_swap_tid)) atomic_store(&g_swap_tid, (int)syscall(SYS_gettid));
    frame_at(0);
}
void prof_main_frame(void)
{
    if (g_hz && !atomic_load(&g_main_tid)) atomic_store(&g_main_tid, (int)syscall(SYS_gettid));
    frame_at(1);
}

void prof_init(const char *root)
{
    const char *e = getenv("UA_PROF");
    g_hz = e ? atoi(e) : 0;
    if (g_hz <= 0) { g_hz = 0; return; }
    if (g_hz > 5000) g_hz = 5000;
    snprintf(g_root, sizeof g_root, "%s", root);
    if (getenv("UA_PROF_SECS") && atof(getenv("UA_PROF_SECS")) > 0) g_secs = atoi(getenv("UA_PROF_SECS"));
    if (getenv("UA_PROF_START") && *getenv("UA_PROF_START")) g_start_s = atof(getenv("UA_PROF_START"));
    if (getenv("UA_PROF_DEPTH")) g_depth = atoi(getenv("UA_PROF_DEPTH"));
    if (g_depth < 0) g_depth = 0;
    if (g_depth > 128) g_depth = 128;              // (8 bits in the sample)
    if (getenv("UA_PROF_SCAN") && atoi(getenv("UA_PROF_SCAN")) > 0) g_scan = atoi(getenv("UA_PROF_SCAN"));
    g_stride = HDR + g_depth;
    const char *names = getenv("UA_PROF_THREADS") && *getenv("UA_PROF_THREADS") ? getenv("UA_PROF_THREADS") : "UnityMain,UnityGfxDeviceW";
    char buf[512];
    snprintf(buf, sizeof buf, "%s", names);
    for (char *s = strtok(buf, ","); s && g_nnames < MAX_THREADS; s = strtok(NULL, ","))
        snprintf(g_names[g_nnames++], sizeof g_names[0], "%.16s", s);
    for (int i = 0; i < MAX_THREADS; i++) g_thr[i].stat_fd = g_thr[i].sys_fd = -1;
    atomic_store(&g_tab, &g_tabs[0]);
    g_sig = SIGRTMIN + 5;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(g_sig, &sa, NULL);
    pthread_t th;
    pthread_create(&th, NULL, sampler, NULL);
    fprintf(stderr, "[prof] on: %s at %d Hz, %d s per recording (touch %s/prof.req%s), depth %d\n", names, g_hz, g_secs, g_root,
            g_start_s >= 0 ? "; one starts by itself" : "", g_depth);
}
