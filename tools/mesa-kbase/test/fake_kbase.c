// fake_kbase.c: an LD_PRELOAD stand-in for /dev/mali0 (Arm's kbase, job manager) so Mesa's kbase
// backend runs off the device: open, the version handshake (UK 11.FAKE_KBASE_UK_MINOR, default 17),
// GPU properties of a Mali-G31 MP2, memory (SAME_VA through cookies, the executable zone from 11.13,
// each region a memfd so every mapping of it shows the same bytes), JOB_SUBMIT (every atom completes
// at once: its event is read back from the fd, poll() sees it; nothing is run, the GPU's writes never
// happen) and no sync files (FENCE_TRIGGER soft atoms and STREAM_CREATE refused, as on the RK3326).
// FAKE_KBASE_LOG=1 logs each request. Test only: no GPU, so no picture.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <execinfo.h>

#include "uapi.h"
#include "uapi_jm.h"

#define MAXR 65536
static struct region { uint64_t gpu_va, size; int memfd; int live; } R[MAXR];
static int g_cookie[64];                       // cookie slot -> region index + 1
static int g_fd = -1, g_log = -1, g_minor = 17;   // g_fd: the event fd (the first open); dups of it are fake too
static unsigned char g_fake[65536];
static int fake_fd(int fd) { return fd >= 0 && fd < 65536 && g_fake[fd]; }
static uint64_t g_exec_next = 0x101000000ull;  // the executable zone's next VA (fake)
static pthread_mutex_t L = PTHREAD_MUTEX_INITIALIZER;
static struct kb_jm_event g_ev[4096];
static unsigned g_evh, g_evn;
static unsigned long g_submits, g_atoms, g_allocs, g_foreign;

static void lg(const char *f, ...)
{
   if (g_log < 0) { const char *e = getenv("FAKE_KBASE_LOG"); g_log = e && *e == '1'; }
   if (!g_log) return;
   va_list ap; va_start(ap, f); fprintf(stderr, "[fake_kbase] "); vfprintf(stderr, f, ap); fprintf(stderr, "\n"); va_end(ap);
}

static int is_node(const char *p)
{
   const char *n = getenv("MESA_KBASE_NODE");
   return p && (!strcmp(p, "/dev/mali0") || (n && !strcmp(p, n)));
}

static int fake_open(void)
{
   pthread_mutex_lock(&L);
   const char *m = getenv("FAKE_KBASE_UK_MINOR");
   if (m && *m) g_minor = atoi(m);
   int fd = eventfd(0, EFD_SEMAPHORE | EFD_CLOEXEC);
   if (fd >= 0 && g_fd < 0) g_fd = fd;
   if (fd >= 0 && fd < 65536) g_fake[fd] = 1;
   pthread_mutex_unlock(&L);
   lg("open -> fd %d (UK 11.%d)", fd, g_minor);
   return fd;
}

#define WRAP_OPEN(name, ...)                                                       \
   int name(const char *p, int flags, ...)                                         \
   {                                                                               \
      static int (*real)(const char *, int, ...);                                  \
      if (!real) real = dlsym(RTLD_NEXT, #name);                                   \
      if (is_node(p)) return fake_open();                                          \
      va_list ap; va_start(ap, flags); int mode = va_arg(ap, int); va_end(ap);     \
      return real(p, flags, mode);                                                 \
   }
WRAP_OPEN(open)
WRAP_OPEN(open64)
int openat(int dir, const char *p, int flags, ...)
{
   static int (*real)(int, const char *, int, ...);
   if (!real) real = dlsym(RTLD_NEXT, "openat");
   if (is_node(p)) return fake_open();
   va_list ap; va_start(ap, flags); int mode = va_arg(ap, int); va_end(ap);
   return real(dir, p, flags, mode);
}
int __open_2(const char *p, int flags) { return open(p, flags); }
int __open64_2(const char *p, int flags) { return open(p, flags); }

ssize_t readlink(const char *p, char *buf, size_t n)
{
   static ssize_t (*real)(const char *, char *, size_t);
   if (!real) real = dlsym(RTLD_NEXT, "readlink");
   int fd;
   if (g_fd >= 0 && sscanf(p, "/proc/self/fd/%d", &fd) == 1 && fake_fd(fd)) {
      const char *s = "/dev/mali0";
      size_t l = strlen(s) < n ? strlen(s) : n;
      memcpy(buf, s, l);
      return (ssize_t)l;
   }
   return real(p, buf, n);
}

static void put_prop(uint8_t **w, unsigned key, unsigned size_code, uint64_t v)
{
   uint32_t t = key << 2 | size_code;
   memcpy(*w, &t, 4); *w += 4;
   unsigned b = 1u << size_code;
   for (unsigned i = 0; i < b; i++) (*w)[i] = (uint8_t)(v >> (8 * i));
   *w += b;
}

static long props(struct kb_ioctl_get_gpuprops *gp)
{
   uint8_t buf[2048], *w = buf;
   put_prop(&w, KB_GPUPROP_PRODUCT_ID, 2, 0x7093);
   put_prop(&w, KB_GPUPROP_VERSION_STATUS, 2, 0);
   put_prop(&w, KB_GPUPROP_MINOR_REVISION, 2, 0);
   put_prop(&w, KB_GPUPROP_MAJOR_REVISION, 2, 0);
   put_prop(&w, KB_GPUPROP_GPU_FREQ_KHZ_MAX, 3, 648000);
   put_prop(&w, KB_GPUPROP_LOG2_PROGRAM_COUNTER_SIZE, 1, 24);
   put_prop(&w, KB_GPUPROP_L2_LOG2_LINE_SIZE, 1, 6);
   put_prop(&w, KB_GPUPROP_L2_LOG2_CACHE_SIZE, 1, 16);
   put_prop(&w, KB_GPUPROP_L2_NUM_L2_SLICES, 1, 1);
   put_prop(&w, KB_GPUPROP_MAX_THREADS, 2, 512);
   put_prop(&w, KB_GPUPROP_MAX_WORKGROUP_SIZE, 2, 512);
   put_prop(&w, KB_GPUPROP_MAX_BARRIER_SIZE, 2, 512);
   put_prop(&w, KB_GPUPROP_MAX_REGISTERS, 2, 0x8000);
   put_prop(&w, KB_GPUPROP_MAX_TASK_QUEUE, 1, 4);
   put_prop(&w, KB_GPUPROP_RAW_SHADER_PRESENT, 3, 0x3);
   put_prop(&w, KB_GPUPROP_RAW_TILER_PRESENT, 3, 0x1);
   put_prop(&w, KB_GPUPROP_RAW_L2_PRESENT, 3, 0x1);
   put_prop(&w, KB_GPUPROP_RAW_STACK_PRESENT, 3, 0x1);
   put_prop(&w, KB_GPUPROP_RAW_L2_FEATURES, 2, 0x07120206);
   put_prop(&w, KB_GPUPROP_RAW_MEM_FEATURES, 2, 0x1);
   put_prop(&w, KB_GPUPROP_RAW_MMU_FEATURES, 2, 0x2830);
   put_prop(&w, KB_GPUPROP_RAW_AS_PRESENT, 2, 0xff);
   put_prop(&w, KB_GPUPROP_RAW_JS_PRESENT, 2, 0x7);
   put_prop(&w, KB_GPUPROP_RAW_JS_FEATURES_0, 2, 0x20e);
   put_prop(&w, KB_GPUPROP_RAW_JS_FEATURES_0 + 1, 2, 0x1fe);
   put_prop(&w, KB_GPUPROP_RAW_JS_FEATURES_0 + 2, 2, 0x7e);
   put_prop(&w, KB_GPUPROP_RAW_TILER_FEATURES, 2, 0x809);
   for (unsigned i = 0; i < 3; i++) put_prop(&w, KB_GPUPROP_RAW_TEXTURE_FEATURES_0 + i, 2, 0xffffffff);
   put_prop(&w, KB_GPUPROP_RAW_TEXTURE_FEATURES_3, 2, 0xffffffff);
   put_prop(&w, KB_GPUPROP_RAW_GPU_ID, 2, 0x70930000);
   put_prop(&w, KB_GPUPROP_RAW_THREAD_MAX_THREADS, 2, 512);
   put_prop(&w, KB_GPUPROP_RAW_THREAD_MAX_WORKGROUP_SIZE, 2, 512);
   put_prop(&w, KB_GPUPROP_RAW_THREAD_MAX_BARRIER_SIZE, 2, 512);
   put_prop(&w, KB_GPUPROP_RAW_THREAD_FEATURES, 2, 0x0a040400);
   put_prop(&w, KB_GPUPROP_RAW_COHERENCY_MODE, 2, 0x1f);
   put_prop(&w, KB_GPUPROP_COHERENCY_NUM_GROUPS, 2, 1);
   put_prop(&w, KB_GPUPROP_COHERENCY_NUM_CORE_GROUPS, 2, 1);
   put_prop(&w, KB_GPUPROP_COHERENCY_COHERENCY, 2, 0);
   put_prop(&w, KB_GPUPROP_COHERENCY_GROUP_0, 3, 0x3);
   long n = w - buf;
   if (!gp->buffer) return n;
   long c = gp->size < n ? gp->size : n;
   memcpy((void *)(uintptr_t)gp->buffer, buf, (size_t)c);
   return c;
}

static int new_region(uint64_t size)
{
   for (int i = 0; i < MAXR; i++)
      if (!R[i].live) {
         int fd = memfd_create("fake_kbase", MFD_CLOEXEC);
         if (fd < 0 || ftruncate(fd, (off_t)size) != 0) { if (fd >= 0) close(fd); return -1; }
         R[i] = (struct region){ .size = size, .memfd = fd, .live = 1 };
         return i;
      }
   return -1;
}

static struct region *by_va(uint64_t va)
{
   for (int i = 0; i < MAXR; i++)
      if (R[i].live && R[i].gpu_va && va >= R[i].gpu_va && va < R[i].gpu_va + R[i].size) return &R[i];
   return NULL;
}

static void event(uint8_t atom, const uint64_t udata[2], uint32_t code)
{
   if (g_evn == sizeof g_ev / sizeof g_ev[0]) { lg("event queue full"); return; }
   struct kb_jm_event *e = &g_ev[(g_evh + g_evn) % (sizeof g_ev / sizeof g_ev[0])];
   e->event_code = code;
   e->atom_number = atom;
   e->udata[0] = udata[0];
   e->udata[1] = udata[1];
   g_evn++;
   uint64_t one = 1;
   (void)!write(g_fd, &one, 8);
}

static long submit(struct kb_jm_job_submit *s)
{
   // the atom from jc on (base_jd_atom_v2) below 11.22, the whole kb_jm_atom (or with frame_nr) after
   const size_t base = s->stride == 48 || s->stride == 56 ? offsetof(struct kb_jm_atom, jc) : 0;
   if (s->stride != 48 && s->stride != 56 && s->stride != 64 && s->stride != 72) return -EINVAL;
   if (g_minor < 22 && s->stride != (g_minor >= 19 ? 56u : 48u)) return -EINVAL;
   const uint8_t *a = (const uint8_t *)(uintptr_t)s->addr;
   for (unsigned i = 0; i < s->nr_atoms; i++) {    // a refused atom refuses the whole call (soft fences: none here)
      struct kb_jm_atom at;
      memset(&at, 0, sizeof at);
      memcpy((uint8_t *)&at + base, a + (size_t)i * s->stride, s->stride < sizeof at - base ? s->stride : sizeof at - base);
      if (at.core_req == KB_JM_REQ_SOFT_FENCE_TRIGGER) { lg("JOB_SUBMIT: fence trigger refused (no sync files)"); return -EINVAL; }
   }
   for (unsigned i = 0; i < s->nr_atoms; i++) {
      struct kb_jm_atom at;
      memset(&at, 0, sizeof at);
      size_t n = s->stride < sizeof at - base ? s->stride : sizeof at - base;
      memcpy((uint8_t *)&at + base, a + (size_t)i * s->stride, n);
      lg("atom %u: jc 0x%llx req 0x%x deps %u/%u %u/%u", at.atom_number, (unsigned long long)at.jc, at.core_req,
         at.pre_dep[0].atom_id, at.pre_dep[0].dependency_type, at.pre_dep[1].atom_id, at.pre_dep[1].dependency_type);
      if (at.jc && !(at.core_req & KB_JM_REQ_SOFT_JOB) && !by_va(at.jc)) lg("atom %u: jc 0x%llx in no region", at.atom_number, (unsigned long long)at.jc);
      event(at.atom_number, at.udata, KB_JM_EVENT_DONE);
      g_atoms++;
   }
   g_submits++;
   return 0;
}

static long fake_ioctl(unsigned long req, void *arg)
{
   const unsigned nr = _IOC_NR(req);
   if (_IOC_TYPE(req) != KB_IOCTL_TYPE) {                       // a DRM (or other) request on the kbase node
      g_foreign++;
      fprintf(stderr, "[fake_kbase] FOREIGN ioctl type '%c' nr %u on the kbase node, from:\n", (int)_IOC_TYPE(req), nr);
      void *bt[16];
      backtrace_symbols_fd(bt, backtrace(bt, 16), 2);
      return -ENOTTY;
   }
   switch (nr) {
   case 52: return -EPERM;                                      // CSF VERSION_CHECK before setup
   case 0: { struct kb_ioctl_version_check *v = arg; v->major = 11; v->minor = (uint16_t)g_minor; return 0; }
   case 1: return 0;                                            // SET_FLAGS
   case 3: return props(arg);
   case 5: {                                                    // MEM_ALLOC
      union kb_ioctl_mem_alloc *m = arg;
      const uint64_t flags = m->in.flags, pages = m->in.va_pages, size = pages << 12;   // (in and out share the union)
      int r = new_region(size);
      if (r < 0) return -ENOMEM;
      g_allocs++;
      if ((flags & KB_MEM_PROT_GPU_EX) && g_minor >= 13) {      // the executable zone: a VA, mapped at it
         R[r].gpu_va = g_exec_next;
         g_exec_next += (size + 0xffff) & ~0xffffull;
         m->out.flags = flags;
         m->out.gpu_va = R[r].gpu_va;
         lg("MEM_ALLOC %llu pages exec -> 0x%llx", (unsigned long long)pages, (unsigned long long)R[r].gpu_va);
         return 0;
      }
      for (int c = 0; c < 64; c++)
         if (!g_cookie[c]) {
            g_cookie[c] = r + 1;
            m->out.flags = flags | KB_MEM_SAME_VA;
            m->out.gpu_va = KB_MEM_COOKIE_BASE + ((uint64_t)c << 12);
            lg("MEM_ALLOC %llu pages flags 0x%llx -> cookie %d", (unsigned long long)pages, (unsigned long long)flags, c);
            return 0;
         }
      R[r].live = 0; close(R[r].memfd);
      return -ENOMEM;
   }
   case 7: {                                                    // MEM_FREE
      uint64_t va = ((struct kb_ioctl_mem_free *)arg)->gpu_addr;
      struct region *g = by_va(va);
      if (!g) { lg("MEM_FREE 0x%llx: no region", (unsigned long long)va); return -EINVAL; }
      close(g->memfd);
      g->live = 0;
      return 0;
   }
   case 2: return submit(arg);                                  // JOB_SUBMIT
   case 24: return -ENOTTY;                                     // STREAM_CREATE: no sync files
   case 50: return -ENOTTY;                                     // GET_CPU_GPU_TIMEINFO
   default:
      lg("ioctl nr %u (size %u): 0", nr, (unsigned)_IOC_SIZE(req));
      return 0;                                                 // EXEC_INIT, JIT_INIT, MEM_SYNC, STICKY, POST_TERM, ...
   }
}

int ioctl(int fd, unsigned long req, ...)
{
   va_list ap; va_start(ap, req); void *arg = va_arg(ap, void *); va_end(ap);
   if (!fake_fd(fd)) return (int)syscall(SYS_ioctl, fd, req, arg);
   pthread_mutex_lock(&L);
   long r = fake_ioctl(req, arg);
   pthread_mutex_unlock(&L);
   if (r < 0) { errno = (int)-r; return -1; }
   return (int)r;
}

ssize_t read(int fd, void *buf, size_t n)
{
   if (!fake_fd(fd)) return syscall(SYS_read, fd, buf, n);
   fd = g_fd;
   size_t got = 0;
   pthread_mutex_lock(&L);
   while (g_evn && got + sizeof(struct kb_jm_event) <= n) {
      uint64_t one;
      pthread_mutex_unlock(&L);
      if (syscall(SYS_read, fd, &one, 8) != 8) { pthread_mutex_lock(&L); break; }   // blocks until an event
      pthread_mutex_lock(&L);
      memcpy((uint8_t *)buf + got, &g_ev[g_evh], sizeof(struct kb_jm_event));
      g_evh = (g_evh + 1) % (sizeof g_ev / sizeof g_ev[0]);
      g_evn--;
      got += sizeof(struct kb_jm_event);
   }
   pthread_mutex_unlock(&L);
   if (!got) { errno = EAGAIN; return -1; }
   return (ssize_t)got;
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
   static void *(*real)(void *, size_t, int, int, int, off_t);
   if (!real) real = dlsym(RTLD_NEXT, "mmap");
   if (!fake_fd(fd)) return real(addr, len, prot, flags, fd, off);
   uint64_t o = (uint64_t)off;
   pthread_mutex_lock(&L);
   void *p = MAP_FAILED;
   if (o < KB_MEM_COOKIE_BASE) {                                // tracking and other handles
      p = real(addr, len, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
   } else if (o < KB_MEM_FIRST_FREE_ADDRESS) {                  // a cookie: the region gets this address
      int c = (int)((o - KB_MEM_COOKIE_BASE) >> 12);
      int r = g_cookie[c] - 1;
      if (r >= 0) {
         p = real(addr, len, prot ? prot : PROT_READ, MAP_SHARED, R[r].memfd, 0);
         if (p != MAP_FAILED) { R[r].gpu_va = (uint64_t)(uintptr_t)p; g_cookie[c] = 0; }
      }
   } else {                                                     // a region's VA: another mapping of it
      struct region *g = by_va(o);
      if (g) p = real(addr, len, prot ? prot : PROT_READ, MAP_SHARED, g->memfd, (off_t)(o - g->gpu_va));
   }
   pthread_mutex_unlock(&L);
   lg("mmap off 0x%llx len %zu -> %p", (unsigned long long)o, len, p);
   if (p == MAP_FAILED) errno = EINVAL;
   return p;
}
// dups of the node stay the node (pipe_loader dups the fd)
int fcntl(int fd, int cmd, ...)
{
   va_list ap; va_start(ap, cmd); long arg = va_arg(ap, long); va_end(ap);
   int r = (int)syscall(SYS_fcntl, fd, cmd, arg);
   if (r >= 0 && r < 65536 && (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC) && fake_fd(fd)) g_fake[r] = 1;
   return r;
}
int fcntl64(int fd, int cmd, ...)
{
   va_list ap; va_start(ap, cmd); long arg = va_arg(ap, long); va_end(ap);
   return fcntl(fd, cmd, arg);
}
int dup(int fd)
{
   int r = (int)syscall(SYS_dup, fd);
   if (r >= 0 && r < 65536 && fake_fd(fd)) g_fake[r] = 1;
   return r;
}
int close(int fd)
{
   if (fd >= 0 && fd < 65536 && fd != g_fd) g_fake[fd] = 0;
   if (fd == g_fd) return 0;                                    // the event fd stays (dups share its events)
   return (int)syscall(SYS_close, fd);
}
void *mmap64(void *addr, size_t len, int prot, int flags, int fd, off_t off) { return mmap(addr, len, prot, flags, fd, off); }

__attribute__((destructor)) static void report(void)
{
   if (g_fd >= 0) fprintf(stderr, "[fake_kbase] %lu submissions, %lu atoms, %lu allocations, %lu foreign ioctls\n", g_submits, g_atoms, g_allocs, g_foreign);
}
