// checks_plugin.c - the test game's own native plugins (build.sh builds them for macOS arm64,
// MachoChecks.cs calls them through DllImport): lib_checks.dylib, a plain file as a game ships
// a library, and with -DPC_BUNDLE checks_bundle.bundle, a bundle folder as Xcode makes one.
// Under the route both are Mach-O files in the port's lib folder that the game's code opens by
// name (docs/macho-route.md, "Plugins the game opens"). With -DPC_STEAM it is steam_api.bundle:
// a file of the name Steamworks' library has in a macOS build, which the setup does not copy
// (the runtime's own answer stands in for that SDK: Steam is not running).
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <string.h>

#if defined(PC_BUNDLE)

int pb_value(void) { return 21; }

#elif defined(PC_STEAM)

// The game's own stand-in of that name, with the few functions the checks call and answers no
// port may give: a check that sees one of them ran this file.
bool SteamAPI_Init(void) { return true; }
bool SteamAPI_IsSteamRunning(void) { return true; }
bool SteamAPI_RestartAppIfNecessary(unsigned app) { return app != 0; }
int SteamInternal_SteamAPI_Init(const char *versions, char *message)
{
    (void)versions;
    if (message) strcpy(message, "the stand-in file");
    return 0;
}
bool SteamAPI_ISteamUser_BLoggedOn(void *self) { (void)self; return true; }

#else

static int inits;
static __thread int calls;

__attribute__((constructor)) static void pc_init(void) { inits++; }

int pc_inits(void) { return inits; }
int pc_add(int a, int b) { return a + b; }
bool pc_not(bool v) { return !v; }

// How many calls this thread has made of it: a thread-local of the plugin's.
int pc_calls(void) { return ++calls; }

// Arguments past the eighth of each kind, of sizes below 8 bytes: on macOS's stack each takes
// its own size, and a caller that lays them out any other way gives a wrong sum.
double pc_mixed(int i1, int i2, int i3, int i4, int i5, int i6, int i7, int i8,
                double d1, double d2, double d3, double d4, double d5, double d6, double d7, double d8,
                signed char c, float f, short s, double d, int i, unsigned char u, long long l)
{
    return i1 + 2 * i2 + 3 * i3 + 4 * i4 + 5 * i5 + 6 * i6 + 7 * i7 + 8 * i8 +
           d1 + 2 * d2 + 3 * d3 + 4 * d4 + 5 * d5 + 6 * d6 + 7 * d7 + 8 * d8 +
           100.0 * c + 1000.0 * f + 10000.0 * s + 100000.0 * d + 1000000.0 * i + 10000000.0 * u + 100000000.0 * (double)l;
}

// The text turned round, bytes as they are; its length, or -1 if it does not fit.
int pc_reverse(const char *text, char *out, int size)
{
    int n = (int)strlen(text);
    if (n + 1 > size) return -1;
    for (int i = 0; i < n; i++) out[i] = text[n - 1 - i];
    out[n] = 0;
    return n;
}

struct pc_vec { float x, y, z; };                   // returned in registers
struct pc_vec pc_scale(struct pc_vec v, float k)
{
    struct pc_vec r = { v.x * k, v.y * k, v.z * k };
    return r;
}

struct pc_big { long long a, b, c; double d; };     // returned through the caller's memory
struct pc_big pc_big(int n)
{
    struct pc_big r = { n, 2LL * n, 3LL * n, n / 4.0 };
    return r;
}

void pc_sum(const int *values, int count, long long *out)
{
    long long s = 0;
    for (int i = 0; i < count; i++) s += values[i];
    *out = s;
}

// Fails the way a C library does: -1 and errno.
int pc_fail(int e) { errno = e; return -1; }

typedef int (*pc_callback)(int value, void *user);

// The game's function called count times on this thread; the sum of what it returned.
int pc_each(int count, pc_callback cb, void *user)
{
    int s = 0;
    for (int i = 0; i < count; i++) s += cb(i, user);
    return s;
}

// The same from a thread of the plugin's own, which the game's runtime has never seen.
static struct { pthread_t thread; int count, sum; pc_callback cb; void *user; } job;

static void *pc_job(void *arg)
{
    (void)arg;
    for (int i = 0; i < job.count; i++) job.sum += job.cb(i, job.user);
    return NULL;
}

int pc_thread_start(int count, pc_callback cb, void *user)
{
    job.count = count; job.cb = cb; job.user = user; job.sum = 0;
    return pthread_create(&job.thread, NULL, pc_job, NULL);
}

int pc_thread_join(void)
{
    pthread_join(job.thread, NULL);
    return job.sum;
}

#endif
