// prof_preload.c: LD_PRELOAD sampler for the main thread's CPU time (ITIMER_PROF, 1 ms of process CPU):
// each sample's pc and lr as <module> <offset> lines in PROF_OUT at exit. PROF_DELAY: CPU seconds before
// the first sample (past the start-up's shader compiles). Test tool, aarch64.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>

#define MAXS (1 << 21)
static uintptr_t *g_pc, *g_lr;
static volatile size_t g_n;
static pid_t g_main;
static unsigned long g_other;

static void on_prof(int sig, siginfo_t *si, void *uc_)
{
   (void)sig; (void)si;
   if (syscall(SYS_gettid) != g_main) { g_other++; return; }
   ucontext_t *uc = uc_;
   size_t i = g_n;
   if (i < MAXS) { g_pc[i] = uc->uc_mcontext.pc; g_lr[i] = uc->uc_mcontext.regs[30]; g_n = i + 1; }
}

__attribute__((constructor)) static void start(void)
{
   if (!getenv("PROF_OUT")) return;
   g_pc = malloc(MAXS * sizeof *g_pc); g_lr = malloc(MAXS * sizeof *g_lr);
   g_main = getpid();
   struct sigaction sa = { 0 };
   sa.sa_sigaction = on_prof; sa.sa_flags = SA_SIGINFO | SA_RESTART;
   sigaction(SIGPROF, &sa, NULL);
   double d = getenv("PROF_DELAY") ? atof(getenv("PROF_DELAY")) : 0;
   struct itimerval it = { { 0, 1000 }, { (time_t)d, (long)((d - (time_t)d) * 1e6) + 1000 } };
   setitimer(ITIMER_PROF, &it, NULL);
}

static void put(FILE *f, const char *k, uintptr_t a)
{
   Dl_info di;
   if (dladdr((void *)a, &di) && di.dli_fname) fprintf(f, "%s %s %lx\n", k, di.dli_fname, (unsigned long)(a - (uintptr_t)di.dli_fbase));
   else fprintf(f, "%s ? %lx\n", k, (unsigned long)a);
}

__attribute__((destructor)) static void stop(void)
{
   const char *o = getenv("PROF_OUT");
   if (!o || !g_pc) return;
   struct itimerval it = { 0 };
   setitimer(ITIMER_PROF, &it, NULL);
   FILE *f = fopen(o, "w");
   if (!f) return;
   fprintf(f, "# samples %zu (main thread), other threads %lu\n", (size_t)g_n, g_other);
   for (size_t i = 0; i < g_n; i++) { put(f, "pc", g_pc[i]); put(f, "lr", g_lr[i]); }
   fclose(f);
}
