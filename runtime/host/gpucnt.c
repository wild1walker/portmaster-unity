// gpucnt: the Mali GPU's hardware counters on a Job Manager kbase kernel driver (the RG34XX-SP:
// Mali-G31, r20p0, UK 11.17), read from user space, and a probe of that kernel interface.
//
//   gpucnt sample <secs> [ms] [label]    the counters dumped every <ms> (100) for <secs>, summed
//   gpucnt exec <label> <cmd> [args...]  the same while <cmd> runs (e.g. shadercheck -t)
//   gpucnt probe                         the UK version handshake, the GPU properties, the DDK
//                                        version, one no-op atom (the job-submit ABI), memory
//                                        allocations through a driver's context setup (the
//                                        tracking page, the EXEC_VA zone, JIT), the reader
//
// The counters are kbase's vinstr reader (KBASE_IOCTL_HWCNT_READER_SETUP, kbase up to UK 11.32):
// every block (front end, tiler, L2, shader core), GPU-wide, whatever context makes the work (the
// game's, EmulationStation's). Each dump holds what was counted since the previous one; 32-bit
// counters, so dumps a second or more apart could overflow. The output lists every counter with
// Arm's name for the Mali-G31 (other GPUs: the same indices, other meanings) and the derived
// figures Arm's counter database gives for that GPU: the share of the shader core's active
// cycles each unit issued (arithmetic: EXEC_INSTR_COUNT; texture filtering:
// TEX_FILT_NUM_OPERATIONS; varying interpolation: VARY_SLOT_32 + VARY_SLOT_16; load/store), per
// fragment warp (4 threads on the G31), tile write-backs, transaction elimination, external
// bus traffic (beats of the bus width L2_FEATURES gives). Nothing is changed: no setting, no
// governor; the reader only reads (docs/profiling.md, "GPU counters").
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// --- kbase's user interface, UK 11.17 (mali_kbase_ioctl.h, mali_kbase_jm_ioctl.h) ---
#define KT 0x80
struct kb_version_check { uint16_t major, minor; };
struct kb_set_flags { uint32_t create_flags; };
struct kb_job_submit { uint64_t addr; uint32_t nr_atoms, stride; };
struct kb_get_gpuprops { uint64_t buffer; uint32_t size, flags; };
union kb_mem_alloc {
    struct { uint64_t va_pages, commit_pages, extent, flags; } in;
    struct { uint64_t flags, gpu_va; } out;
};
struct kb_mem_free { uint64_t gpu_addr; };
struct kb_hwcnt_reader_setup { uint32_t buffer_count, fe_bm, shader_bm, tiler_bm, mmu_l2_bm; };
struct kb_get_ddk_version { uint64_t version_buffer; uint32_t size, padding; };
#define KB_VERSION_CHECK _IOWR(KT, 0, struct kb_version_check)
#define KB_SET_FLAGS _IOW(KT, 1, struct kb_set_flags)
#define KB_JOB_SUBMIT _IOW(KT, 2, struct kb_job_submit)
#define KB_GET_GPUPROPS _IOW(KT, 3, struct kb_get_gpuprops)
#define KB_MEM_ALLOC _IOWR(KT, 5, union kb_mem_alloc)
#define KB_MEM_FREE _IOW(KT, 7, struct kb_mem_free)
#define KB_HWCNT_READER_SETUP _IOW(KT, 8, struct kb_hwcnt_reader_setup)
#define KB_GET_DDK_VERSION _IOW(KT, 13, struct kb_get_ddk_version)

// the vinstr reader's ioctls, on the fd KB_HWCNT_READER_SETUP returns
struct kb_hwcnt_meta { uint64_t timestamp; uint32_t event_id, buffer_idx; };
#define HR 0xBE
#define HR_GET_HWVER _IOR(HR, 0x00, uint32_t)
#define HR_GET_BUFFER_SIZE _IOR(HR, 0x01, uint32_t)
#define HR_DUMP _IOW(HR, 0x10, uint32_t)
#define HR_CLEAR _IOW(HR, 0x11, uint32_t)
#define HR_GET_BUFFER _IOR(HR, 0x20, struct kb_hwcnt_meta)
#define HR_PUT_BUFFER _IOW(HR, 0x21, struct kb_hwcnt_meta)
#define HR_GET_API_VERSION _IOW(HR, 0xFF, uint32_t)

// UK 11.17's atom (11.19 added renderpass_id, 11.22 the v3 atom with seq_nr); r20p0 checks the
// stride against its own size
struct kb_dep { uint8_t atom_id, dependency_type; };
struct kb_atom_v2 {
    uint64_t jc;
    uint64_t udata[2];
    uint64_t extres_list;
    uint16_t nr_extres, compat_core_req;
    struct kb_dep pre_dep[2];
    uint8_t atom_number, prio, device_nr, jobslot;
    uint32_t core_req;
};
struct kb_event_v2 { uint32_t event_code; uint8_t atom_number, pad[3]; uint64_t udata[2]; };
_Static_assert(sizeof(struct kb_atom_v2) == 48, "atom v2 is 48 bytes");
_Static_assert(sizeof(struct kb_event_v2) == 24, "event v2 is 24 bytes");

#define MEM_CPU_RD (1u << 0)
#define MEM_CPU_WR (1u << 1)
#define MEM_GPU_RD (1u << 2)
#define MEM_GPU_WR (1u << 3)
#define MEM_SAME_VA (1u << 13)

static const char *prop_names[] = {
    [1] = "PRODUCT_ID", "VERSION_STATUS", "MINOR_REVISION", "MAJOR_REVISION", [6] = "GPU_FREQ_KHZ_MAX",
    [8] = "LOG2_PROGRAM_COUNTER_SIZE", "TEXTURE_FEATURES_0", "TEXTURE_FEATURES_1", "TEXTURE_FEATURES_2",
    "GPU_AVAILABLE_MEMORY_SIZE", "L2_LOG2_LINE_SIZE", "L2_LOG2_CACHE_SIZE", "L2_NUM_L2_SLICES",
    "TILER_BIN_SIZE_BYTES", "TILER_MAX_ACTIVE_LEVELS", "MAX_THREADS", "MAX_WORKGROUP_SIZE",
    "MAX_BARRIER_SIZE", "MAX_REGISTERS", "MAX_TASK_QUEUE", "MAX_THREAD_GROUP_SPLIT", "IMPL_TECH",
    "RAW_SHADER_PRESENT", "RAW_TILER_PRESENT", "RAW_L2_PRESENT", "RAW_STACK_PRESENT", "RAW_L2_FEATURES",
    "RAW_CORE_FEATURES", "RAW_MEM_FEATURES", "RAW_MMU_FEATURES", "RAW_AS_PRESENT", "RAW_JS_PRESENT",
    "RAW_JS_FEATURES_0", "RAW_JS_FEATURES_1", "RAW_JS_FEATURES_2", [51] = "RAW_TILER_FEATURES",
    "RAW_TEXTURE_FEATURES_0", "RAW_TEXTURE_FEATURES_1", "RAW_TEXTURE_FEATURES_2", "RAW_GPU_ID",
    "RAW_THREAD_MAX_THREADS", "RAW_THREAD_MAX_WORKGROUP_SIZE", "RAW_THREAD_MAX_BARRIER_SIZE",
    "RAW_THREAD_FEATURES", "RAW_COHERENCY_MODE", "COHERENCY_NUM_GROUPS", "COHERENCY_NUM_CORE_GROUPS",
    "COHERENCY_COHERENCY", [80] = "TEXTURE_FEATURES_3", "RAW_TEXTURE_FEATURES_3", "NUM_EXEC_ENGINES",
    "RAW_THREAD_TLS_ALLOC", "TLS_ALLOC", "RAW_GPU_FEATURES",
};

// Counter names of the Mali-G31 (Arm's libGPUCounters hardware layout, MIT; THIRD_PARTY.md),
// by block and index; 0-3 of each block are its header
static const char *const cnt_JM[64] = {
    [6] = "GPU_ACTIVE", "IRQ_ACTIVE", "JS0_JOBS", "JS0_TASKS", "JS0_ACTIVE", "JS0_WAIT_FLUSH", "JS0_WAIT_READ",
    "JS0_WAIT_ISSUE", "JS0_WAIT_DEPEND", "JS0_WAIT_FINISH", "JS1_JOBS", "JS1_TASKS", "JS1_ACTIVE", "JS1_WAIT_FLUSH",
    "JS1_WAIT_READ", "JS1_WAIT_ISSUE", "JS1_WAIT_DEPEND", "JS1_WAIT_FINISH", "JS2_JOBS", "JS2_TASKS", "JS2_ACTIVE",
    "JS2_WAIT_FLUSH", "JS2_WAIT_READ", "JS2_WAIT_ISSUE", "JS2_WAIT_DEPEND", "JS2_WAIT_FINISH", [63] = "CACHE_FLUSH",
};
static const char *const cnt_TILER[64] = {
    [4] = "TILER_ACTIVE", [6] = "TRIANGLES", "LINES", "POINTS", "FRONT_FACING", "BACK_FACING", "PRIM_VISIBLE",
    "PRIM_CULLED", "PRIM_CLIPPED", "PRIM_SAT_CULLED", [17] = "BUS_READ", [19] = "BUS_WRITE",
    [21] = "IDVS_POS_SHAD_REQ", "IDVS_POS_SHAD_WAIT", "IDVS_POS_SHAD_STALL", "IDVS_POS_FIFO_FULL",
    [26] = "VCACHE_HIT", "VCACHE_MISS", [34] = "IDVS_VBU_HIT", "IDVS_VBU_MISS", [37] = "IDVS_VAR_SHAD_REQ",
    "IDVS_VAR_SHAD_STALL",
};
static const char *const cnt_SC[64] = {
    [4] = "FRAG_ACTIVE", "FRAG_PRIMITIVES", "FRAG_PRIM_RAST", "FRAG_FPK_ACTIVE", [9] = "FRAG_WARPS",
    "FRAG_PARTIAL_WARPS", "FRAG_QUADS_RAST", "FRAG_QUADS_EZS_TEST", "FRAG_QUADS_EZS_UPDATE", "FRAG_QUADS_EZS_KILL",
    "FRAG_LZS_TEST", "FRAG_LZS_KILL", [18] = "FRAG_PTILES", "FRAG_TRANS_ELIM", "QUAD_FPK_KILLER",
    [22] = "COMPUTE_ACTIVE", "COMPUTE_TASKS", "COMPUTE_WARPS", [26] = "EXEC_CORE_ACTIVE", [28] = "EXEC_INSTR_COUNT",
    "EXEC_INSTR_DIVERGED", "EXEC_INSTR_STARVING", [35] = "TEX_MSGI_NUM_QUADS", "TEX_DFCH_NUM_PASSES",
    "TEX_DFCH_NUM_PASSES_MISS", "TEX_DFCH_NUM_PASSES_MIP_MAP", "TEX_TIDX_NUM_SPLIT_MIP_MAP",
    "TEX_TFCH_NUM_LINES_FETCHED", "TEX_TFCH_NUM_LINES_FETCHED_BLOCK_COMPRESSED", "TEX_TFCH_NUM_OPERATIONS",
    "TEX_FILT_NUM_OPERATIONS", "LS_MEM_READ_FULL", "LS_MEM_READ_SHORT", "LS_MEM_WRITE_FULL", "LS_MEM_WRITE_SHORT",
    "LS_MEM_ATOMIC", "VARY_INSTR", "VARY_SLOT_32", "VARY_SLOT_16", "ATTR_INSTR", [54] = "BEATS_RD_FTC",
    "BEATS_RD_FTC_EXT", "BEATS_RD_LSC", "BEATS_RD_LSC_EXT", "BEATS_RD_TEX", "BEATS_RD_TEX_EXT", "BEATS_RD_OTHER",
    "BEATS_WR_LSC_WB", "BEATS_WR_TIB", "BEATS_WR_LSC_OTHER",
};
static const char *const cnt_L2[64] = {
    [4] = "MMU_REQUESTS", "MMU_TABLE_READS_L3", "MMU_TABLE_READS_L2", "MMU_HIT_L3", "MMU_HIT_L2",
    [16] = "L2_RD_MSG_IN", "L2_RD_MSG_IN_STALL", "L2_WR_MSG_IN", "L2_WR_MSG_IN_STALL", "L2_SNP_MSG_IN",
    "L2_SNP_MSG_IN_STALL", "L2_RD_MSG_OUT", "L2_RD_MSG_OUT_STALL", "L2_WR_MSG_OUT", "L2_ANY_LOOKUP",
    "L2_READ_LOOKUP", "L2_WRITE_LOOKUP", "L2_EXT_SNOOP_LOOKUP", "L2_EXT_READ", "L2_EXT_READ_NOSNP",
    "L2_EXT_READ_UNIQUE", "L2_EXT_READ_BEATS", "L2_EXT_AR_STALL", "L2_EXT_AR_CNT_Q1", "L2_EXT_AR_CNT_Q2",
    "L2_EXT_AR_CNT_Q3", "L2_EXT_RRESP_0_127", "L2_EXT_RRESP_128_191", "L2_EXT_RRESP_192_255",
    "L2_EXT_RRESP_256_319", "L2_EXT_RRESP_320_383", "L2_EXT_WRITE", "L2_EXT_WRITE_NOSNP_FULL",
    "L2_EXT_WRITE_NOSNP_PTL", "L2_EXT_WRITE_SNP_FULL", "L2_EXT_WRITE_SNP_PTL", "L2_EXT_WRITE_BEATS",
    "L2_EXT_W_STALL", "L2_EXT_AW_CNT_Q1", "L2_EXT_AW_CNT_Q2", "L2_EXT_AW_CNT_Q3", "L2_EXT_SNOOP",
    "L2_EXT_SNOOP_STALL",
};
enum { B_JM, B_TILER, B_L2, B_SC };
static const char *const *names_of[] = { cnt_JM, cnt_TILER, cnt_L2, cnt_SC };
static const char *const kind_name[] = { "JM", "Tiler", "L2", "SC" };

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static int open_kbase(int verbose)
{
    int fd = open("/dev/mali0", O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) { perror("/dev/mali0"); exit(1); }
    struct kb_version_check v = { 11, 17 };
    if (ioctl(fd, KB_VERSION_CHECK, &v) != 0) { perror("VERSION_CHECK"); exit(1); }
    if (verbose) printf("kbase UK version (the kernel's answer to 11.17): %u.%u\n", v.major, v.minor);
    struct kb_set_flags f = { 0 };
    if (ioctl(fd, KB_SET_FLAGS, &f) != 0) { perror("SET_FLAGS"); exit(1); }
    return fd;
}

// the GPU properties (key: id << 2 | log2 size, then the value); -1 when absent
static long long gpuprop(int fd, int want, int print)
{
    struct kb_get_gpuprops gp = { 0, 0, 0 };
    int size = ioctl(fd, KB_GET_GPUPROPS, &gp);
    if (size <= 0) return -1;
    uint8_t *b = calloc(1, size);
    gp.buffer = (uintptr_t)b; gp.size = size;
    long long found = -1;
    if (ioctl(fd, KB_GET_GPUPROPS, &gp) >= 0)
        for (int i = 0; i + 4 <= size;) {
            uint32_t key; memcpy(&key, b + i, 4); i += 4;
            int n = 1 << (key & 3), id = key >> 2;
            uint64_t val = 0; memcpy(&val, b + i, n); i += n;
            if (id == want) found = (long long)val;
            if (print) {
                const char *name = id < (int)(sizeof prop_names / sizeof *prop_names) && prop_names[id] ? prop_names[id] : "?";
                printf("  %2d %-32s 0x%llx (%llu)\n", id, name, (unsigned long long)val, (unsigned long long)val);
            }
        }
    free(b);
    return found;
}

struct kb_mem_exec_init { uint64_t va_pages; };
struct kb_mem_jit_init_11_5 { uint64_t va_pages; uint8_t max_allocations, trim_level, group_id, padding[5]; };
#define KB_MEM_EXEC_INIT _IOW(KT, 38, struct kb_mem_exec_init)
#define KB_MEM_JIT_INIT_11_5 _IOW(KT, 14, struct kb_mem_jit_init_11_5)

// 16 pages CPU- and GPU-readable and writable, without and with SAME_VA: allocated, mapped,
// written, unmapped, freed
static void try_alloc(int fd, const char *when)
{
    for (int same = 0; same < 2; same++) {
        union kb_mem_alloc m = { 0 };
        m.in.va_pages = m.in.commit_pages = 16;
        m.in.flags = MEM_CPU_RD | MEM_CPU_WR | MEM_GPU_RD | MEM_GPU_WR | (same ? MEM_SAME_VA : 0);
        if (ioctl(fd, KB_MEM_ALLOC, &m) != 0) { printf("  %s: MEM_ALLOC%s: %s\n", when, same ? " SAME_VA" : "", strerror(errno)); continue; }
        uint64_t va = m.out.gpu_va;
        void *p = mmap(NULL, 16 * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)va);
        printf("  %s: MEM_ALLOC%s: gpu_va/cookie 0x%llx, flags 0x%llx, mmap %s\n", when, same ? " SAME_VA" : "",
               (unsigned long long)va, (unsigned long long)m.out.flags, p == MAP_FAILED ? strerror(errno) : "ok");
        if (p != MAP_FAILED) {
            memset(p, 0x5a, 16 * 4096);
            if (same) va = (uintptr_t)p;     // SAME_VA: the GPU address is the CPU one
            munmap(p, 16 * 4096);
        }
        struct kb_mem_free mf = { va };
        if (ioctl(fd, KB_MEM_FREE, &mf) != 0) printf("    MEM_FREE 0x%llx: %s\n", (unsigned long long)va, strerror(errno));
    }
}

static int probe(void)
{
    int fd = open_kbase(1);
    char ddk[128] = { 0 };
    struct kb_get_ddk_version dv = { (uintptr_t)ddk, sizeof ddk - 1, 0 };
    int r = ioctl(fd, KB_GET_DDK_VERSION, &dv);
    printf("DDK version: %s (ret %d)\n", r > 0 ? ddk : "?", r);
    printf("gpuprops:\n");
    gpuprop(fd, -1, 1);

    // one no-op atom (core_req 0 = BASE_JD_REQ_DEP: no job chain), then its completion event
    struct kb_atom_v2 a = { 0 };
    a.udata[0] = 0x1234abcd; a.atom_number = 1;
    struct kb_job_submit js = { (uintptr_t)&a, 1, sizeof a };
    double t0 = now();
    if (ioctl(fd, KB_JOB_SUBMIT, &js) != 0) printf("JOB_SUBMIT (no-op atom, stride 48): %s\n", strerror(errno));
    else {
        struct pollfd p = { fd, POLLIN, 0 };
        int pr = poll(&p, 1, 1000);
        struct kb_event_v2 e = { 0 };
        ssize_t n = pr > 0 ? read(fd, &e, sizeof e) : -1;
        printf("JOB_SUBMIT no-op atom: event %zd bytes, code 0x%x (1 = done), atom %u, udata 0x%llx, %.3f ms\n",
               n, e.event_code, e.atom_number, (unsigned long long)e.udata[0], (now() - t0) * 1e3);
    }

    // memory: what a user-space driver's context setup needs before allocations (the sequence the
    // blob and other kbase clients follow: flags, the tracking page, the EXEC_VA zone, JIT)
    try_alloc(fd, "after SET_FLAGS");
    void *trk = mmap(NULL, 4096, PROT_NONE, MAP_SHARED, fd, 3 << 12);   // BASE_MEM_MAP_TRACKING_HANDLE
    printf("tracking page: %s\n", trk == MAP_FAILED ? strerror(errno) : "mapped");
    try_alloc(fd, "after the tracking page");
    struct kb_mem_exec_init ei = { 0x100000 };
    printf("MEM_EXEC_INIT: %s\n", ioctl(fd, KB_MEM_EXEC_INIT, &ei) ? strerror(errno) : "ok");
    try_alloc(fd, "after MEM_EXEC_INIT");
    struct kb_mem_jit_init_11_5 ji = { 1 << 20, 255, 0, 0, { 0 } };
    printf("MEM_JIT_INIT (11.5 layout): %s\n", ioctl(fd, KB_MEM_JIT_INIT_11_5, &ji) ? strerror(errno) : "ok");
    try_alloc(fd, "after MEM_JIT_INIT");

    struct kb_hwcnt_reader_setup hs = { 2, ~0u, ~0u, ~0u, ~0u };
    int hfd = ioctl(fd, KB_HWCNT_READER_SETUP, &hs);
    if (hfd < 0) printf("HWCNT_READER_SETUP: %s\n", strerror(errno));
    else {
        uint32_t hw = 0, bs = 0, api = 0;
        ioctl(hfd, HR_GET_HWVER, &hw); ioctl(hfd, HR_GET_BUFFER_SIZE, &bs);
        int ar = ioctl(hfd, HR_GET_API_VERSION, &api);
        printf("hwcnt reader: hw version %u, buffer %u bytes (%u blocks of 256), api %u (ret %d)\n", hw, bs, bs / 256, api, ar);
        close(hfd);
    }
    close(fd);
    return 0;
}

static int read_freq_mhz(void)
{
    static char path[256];
    if (!path[0]) {
        FILE *p = popen("ls /sys/class/devfreq/*/cur_freq 2>/dev/null | head -1", "r");
        if (p && fgets(path, sizeof path, p)) path[strcspn(path, "\n")] = 0;
        if (p) pclose(p);
        if (!path[0]) snprintf(path, sizeof path, "-");
    }
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    long long hz = 0;
    if (fscanf(f, "%lld", &hz) != 1) hz = 0;
    fclose(f);
    return (int)(hz > 100000 ? hz / 1000000 : hz);
}

static pid_t g_child = -1;

static int sample(double secs, int ms, const char *label, char **cmd)
{
    int fd = open_kbase(0);
    long long l2f = gpuprop(fd, 29, 0);          // RAW_L2_FEATURES: bits 24-31 log2 of the bus width
    int beat = l2f > 0 && ((l2f >> 24) & 0xff) >= 5 && ((l2f >> 24) & 0xff) <= 10 ? (1 << ((l2f >> 24) & 0xff)) / 8 : 16;
    struct kb_hwcnt_reader_setup hs = { 4, ~0u, ~0u, ~0u, ~0u };
    int hfd = ioctl(fd, KB_HWCNT_READER_SETUP, &hs);
    if (hfd < 0) { perror("HWCNT_READER_SETUP"); return 1; }
    uint32_t bs = 0;
    ioctl(hfd, HR_GET_BUFFER_SIZE, &bs);
    int nblk = bs / 256;
    if (nblk < 4 || nblk > 16) { fprintf(stderr, "unexpected dump size %u\n", bs); return 1; }
    uint8_t *map = mmap(NULL, (size_t)bs * hs.buffer_count, PROT_READ, MAP_PRIVATE, hfd, 0);
    if (map == MAP_FAILED) { perror("mmap hwcnt"); return 1; }
    uint64_t *tot = calloc(nblk * 64, sizeof *tot);
    double freq_sum = 0; int freq_n = 0, dumps = 0, status = 0;
    uint32_t z = 0;
    ioctl(hfd, HR_CLEAR, &z);
    if (cmd) {
        g_child = fork();
        if (g_child == 0) { execvp(cmd[0], cmd); perror(cmd[0]); _exit(127); }
        secs = 1e9;
    }
    double t0 = now(), tend = t0 + secs, tlast = t0;
    while (now() < tend) {
        int alive = g_child <= 0 || waitpid(g_child, &status, WNOHANG) == 0;
        usleep(ms * 1000);
        if (ioctl(hfd, HR_DUMP, &z) != 0) { perror("HR_DUMP"); break; }
        struct pollfd p = { hfd, POLLIN, 0 };
        if (poll(&p, 1, 1000) <= 0) { fprintf(stderr, "no dump\n"); continue; }
        struct kb_hwcnt_meta m;
        if (ioctl(hfd, HR_GET_BUFFER, &m) != 0) { perror("HR_GET_BUFFER"); continue; }
        const uint32_t *c = (const uint32_t *)(map + (size_t)m.buffer_idx * bs);
        for (int b = 0; b < nblk; b++)
            for (int i = 4; i < 64; i++) tot[b * 64 + i] += c[b * 64 + i];
        ioctl(hfd, HR_PUT_BUFFER, &m);
        int mhz = read_freq_mhz();
        if (mhz) { freq_sum += mhz; freq_n++; }
        dumps++;
        tlast = now();
        if (!alive) break;
    }
    double span = tlast - t0;
    // r20p0's vinstr copies in its metadata order: front end, tiler, shader cores, memory system;
    // with one core and one L2 slice the shader core is whichever of blocks 2 and 3 counted
    // fragment warps (index 9: nothing in an L2 block)
    int kind[16];
    for (int b = 0; b < nblk; b++) kind[b] = b == 0 ? B_JM : b == 1 ? B_TILER : b == 2 ? B_SC : B_L2;
    if (nblk == 4 && tot[3 * 64 + 9] > tot[2 * 64 + 9]) { kind[2] = B_L2; kind[3] = B_SC; }
    double mhz = freq_n ? freq_sum / freq_n : 0;
    printf("# gpucnt %s: %.2f s, %d dumps, %d blocks, GPU clock avg %.0f MHz, bus beat %d bytes\n", label, span, dumps,
           nblk, mhz, beat);
    for (int b = 0; b < nblk; b++) {
        const char *const *nm = names_of[kind[b]];
        printf("[%s block %d]\n", kind_name[kind[b]], b);
        for (int i = 4; i < 64; i++)
            if (tot[b * 64 + i] || nm[i])
                printf("  %2d %-44s %llu\n", i, nm[i] ? nm[i] : "?", (unsigned long long)tot[b * 64 + i]);
    }
#define C(b, i) ((double)tot[(b) * 64 + (i)])
    int jm = 0, ti = 1, l2 = kind[2] == B_L2 ? 2 : 3, sc = kind[2] == B_SC ? 2 : 3;
    double gpu = C(jm, 6) ? C(jm, 6) : 1, core = C(sc, 26) ? C(sc, 26) : 1, warps = C(sc, 9) ? C(sc, 9) : 1;
    double instr = C(sc, 28), tex = C(sc, 43), v32 = C(sc, 50), v16 = C(sc, 51);
    double ls = C(sc, 44) + C(sc, 45) + C(sc, 46) + C(sc, 47) + C(sc, 48);
    double tiles = C(sc, 18), te = C(sc, 19);
    printf("# derived (Arm's formulas for the Mali-G31)\n");
    printf("GPU active %.1f%% of %.2f s at %.0f MHz; fragment queue %.1f%%, non-fragment %.1f%%, tiler %.1f%% of GPU active\n",
           mhz ? 100.0 * gpu / (mhz * 1e6 * span) : 0, span, mhz, 100 * C(jm, 10) / gpu, 100 * C(jm, 18) / gpu, 100 * C(ti, 4) / gpu);
    printf("shader core active %.1f%% of GPU active; of its cycles: arithmetic %.1f%%, texture filtering %.1f%%, "
           "varying %.1f%% (32-bit %.1f, 16-bit %.1f), load/store %.1f%%\n",
           100 * core / gpu, 100 * instr / core, 100 * tex / core, 100 * (v32 + v16) / core, 100 * v32 / core, 100 * v16 / core,
           100 * ls / core);
    printf("per fragment warp: %.2f core cycles, %.2f instructions, %.2f texture filter ops, %.2f 32-bit and %.2f 16-bit varying slots, %.2f load/store\n",
           core / warps, instr / warps, tex / warps, v32 / warps, v16 / warps, ls / warps);
    printf("tiles %.0f, %.1f%% eliminated (transaction elimination), %.0f bytes written per written tile; "
           "external bus read %.1f MB/s, write %.1f MB/s\n",
           tiles, tiles ? 100 * te / tiles : 0, 16 * C(sc, 62) / (tiles - te > 0 ? tiles - te : 1),
           C(l2, 32) * beat / span / 1e6, C(l2, 47) * beat / span / 1e6);
    close(hfd);
    close(fd);
    return cmd && WIFEXITED(status) ? WEXITSTATUS(status) : 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "probe")) return probe();
    if (argc > 2 && !strcmp(argv[1], "sample"))
        return sample(atof(argv[2]), argc > 3 ? atoi(argv[3]) : 100, argc > 4 ? argv[4] : "-", NULL);
    if (argc > 3 && !strcmp(argv[1], "exec")) return sample(0, 100, argv[2], argv + 3);
    fprintf(stderr, "usage: gpucnt sample <secs> [ms] [label] | gpucnt exec <label> <cmd> [args...] | gpucnt probe\n");
    return 2;
}
