// A sound backend for the Wwise stub's test (test_plugins wwise-backend): records what it is given,
// plays every event until the test ends it (wb_finish), answers GetBufferTick itself.
#include "../wwise.h"
#include <string.h>

static const ua_wwise_host *H;
char wb_bank[64];
float wb_rtpc;
uint32_t wb_playing, wb_state;
float wb_pos[3];

static int call(const ua_ak_call *c, ua_ak_value *ret) { if (!strcmp(c->api, "GetBufferTick")) { ret->u = 42; return 1; } return 0; }
static int init(const char *base, const char *lang) { (void)base; (void)lang; return UA_AK_SUCCESS; }
static int load_bank(const char *name, uint32_t id, const void *data, uint32_t size, uint32_t *bank)
{
    (void)data; (void)size;
    if (name && !wb_bank[0]) { strncpy(wb_bank, name, sizeof wb_bank - 1); }
    *bank = id;                     // (the stub's: the ID of the name without its extension)
    return UA_AK_SUCCESS;
}
static int post_event(uint32_t event, uint64_t obj, uint32_t playing, uint32_t flags) { (void)event; (void)obj; (void)flags; if (!wb_playing) wb_playing = playing; return 1; }
static void set_rtpc(uint32_t id, float v, uint64_t obj, uint32_t playing, int every) { (void)id; (void)obj; (void)playing; (void)every; wb_rtpc = v; }
static void set_state(uint32_t g, uint32_t s) { (void)g; wb_state = s; }
static void set_position(uint64_t obj, const float p[3], const float f[3], const float t[3]) { (void)obj; (void)f; (void)t; memcpy(wb_pos, p, sizeof wb_pos); }
void wb_finish(void) { H->event_done(wb_playing); }

static const ua_wwise_backend B = { UA_WWISE_BACKEND_VERSION, "test", call, init, 0, 0, 0, load_bank, 0, post_event, 0, 0,
                                    set_rtpc, set_state, 0, 0, 0, set_position };
const ua_wwise_backend *ua_wwise_backend_get(const ua_wwise_host *host) { H = host; return &B; }
