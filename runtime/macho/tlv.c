// tlv.c - the images' thread-local variables. A variable is reached through its descriptor
// { thunk, key, offset } (sections of type S_THREAD_LOCAL_VARIABLES): the code calls thunk
// with the descriptor and gets the address. The initial contents of an image's variables are
// its thread-local data and zero-fill sections as they lie in memory; a thread's copy is made
// at its first use (tlv_thunk.S), at an address congruent to the image's own modulo the
// sections' alignment, so every variable is aligned as the linker placed it.
#define _GNU_SOURCE
#include "darwin.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define S_THREAD_LOCAL_REGULAR  0x11
#define S_THREAD_LOCAL_ZEROFILL 0x12
#define S_THREAD_LOCAL_VARIABLES 0x13
#define S_THREAD_LOCAL_INIT_FUNCTION_POINTERS 0x15

#define TLV_IMAGES 32
__attribute__((visibility("hidden"))) __thread void *ua_tlv_slots[TLV_IMAGES];
static struct darwin_image *owner[TLV_IMAGES];
static int nowner;

struct at_exit { void (*fn)(void *); void *arg; struct at_exit *next; };
static __thread struct at_exit *at_exit;

static pthread_key_t gone_key;
static pthread_once_t gone_once = PTHREAD_ONCE_INIT;

// A thread's block starts where the image's own does within tlv_align: so far into what
// posix_memalign gave (and free takes).
static size_t block_bias(const struct darwin_image *im) { return (uintptr_t)im->tlv_start & (im->tlv_align - 1); }

void darwin_tlv_thread_exit(void)
{
    // a destructor may register another: until the list stays empty
    while (at_exit) {
        struct at_exit *a = at_exit;
        at_exit = a->next;
        a->fn(a->arg);
        free(a);
    }
}

static void thread_gone(void *v)
{
    (void)v;
    darwin_tlv_thread_exit();
    for (int i = 0; i < TLV_IMAGES; i++) {
        if (ua_tlv_slots[i]) free((uint8_t *)ua_tlv_slots[i] - block_bias(owner[i]));
        ua_tlv_slots[i] = NULL;
    }
}

static void gone_init(void)
{
    if (pthread_key_create(&gone_key, thread_gone) != 0) { fprintf(stderr, "[darwin] no thread key for the thread-local variables\n"); abort(); }
}

// the thread has something to give back at its end (again, when a later destructor of the
// thread touched a variable after thread_gone ran)
static void arm(void)
{
    pthread_once(&gone_once, gone_init);
    pthread_setspecific(gone_key, (void *)1);
}

__attribute__((visibility("hidden"))) void *ua_tlv_slow(uintptr_t *desc)
{
    uintptr_t key = desc[1];
    if (key >= (uintptr_t)nowner) { fprintf(stderr, "[darwin] a thread-local descriptor at %p names no image (key %lu)\n", (void *)desc, (unsigned long)key); abort(); }
    struct darwin_image *im = owner[key];
    size_t bias = block_bias(im);
    void *mem = NULL;
    if (posix_memalign(&mem, im->tlv_align, bias + im->tlv_size + 1) != 0) { fprintf(stderr, "[darwin] no memory for a thread's variables (%zu bytes)\n", im->tlv_size); abort(); }
    uint8_t *block = (uint8_t *)mem + bias;
    memcpy(block, im->tlv_start, im->tlv_size);
    ua_tlv_slots[key] = block;
    arm();
    // the image's thread-local initializers, once the variables can be reached
    for (int i = 0; i < im->m->nsect; i++) {
        const struct macho_section *s = &im->m->sect[i];
        if ((s->flags & 0xff) != S_THREAD_LOCAL_INIT_FUNCTION_POINTERS) continue;
        for (uint64_t o = 0; o + 8 <= s->size; o += 8) {
            void (*f)(void);
            memcpy(&f, s->addr + o, 8);
            f();
        }
    }
    return block + desc[2];
}

int darwin_tlv_setup(struct darwin_image *im, char *err, size_t errlen)
{
    struct macho_image *m = im->m;
    uint8_t *lo = NULL, *hi = NULL;
    size_t align = 16;
    int vars = 0;
    im->tlv_slot = -1;
    for (int i = 0; i < m->nsect; i++) {
        const struct macho_section *s = &m->sect[i];
        unsigned type = s->flags & 0xff;
        if (type == S_THREAD_LOCAL_VARIABLES) vars += (int)(s->size / 24);
        if (type != S_THREAD_LOCAL_REGULAR && type != S_THREAD_LOCAL_ZEROFILL) continue;
        if (!lo || s->addr < lo) lo = s->addr;
        if (!hi || s->addr + s->size > hi) hi = s->addr + s->size;
        if (((size_t)1 << s->align) > align) align = (size_t)1 << s->align;
    }
    if (!vars) return 0;
    if (nowner == TLV_IMAGES) { snprintf(err, errlen, "%s: more than %d images with thread-local variables", m->path, TLV_IMAGES); return -1; }
    im->tlv_start = lo ? lo : m->header;
    im->tlv_size = lo ? (size_t)(hi - lo) : 0;
    im->tlv_align = align;
    for (int i = 0; i < m->nsect; i++) {
        const struct macho_section *s = &m->sect[i];
        if ((s->flags & 0xff) != S_THREAD_LOCAL_VARIABLES) continue;
        for (uint64_t o = 0; o + 24 <= s->size; o += 24) {
            uintptr_t d[3];
            memcpy(d, s->addr + o, 24);
            if (d[0] != (uintptr_t)ua_tlv_get_addr) { snprintf(err, errlen, "%s: a thread-local descriptor in %s,%s is not bound to _tlv_bootstrap", m->path, s->seg, s->name); return -1; }
            if (d[2] > im->tlv_size) { snprintf(err, errlen, "%s: a thread-local variable past the image's thread-local data", m->path); return -1; }
            d[1] = (uintptr_t)nowner;
            memcpy(s->addr + o, d, 24);
        }
    }
    owner[nowner] = im;
    im->tlv_slot = nowner++;
    return 0;
}

// void _tlv_atexit(void (*fn)(void *), void *arg): fn(arg) when the calling thread ends, the
// last registered first (a thread_local object's destructor).
static void darwin_tlv_atexit(void (*fn)(void *), void *arg)
{
    struct at_exit *a = malloc(sizeof *a);
    if (!a) { fprintf(stderr, "[darwin] no memory for a thread-local destructor\n"); abort(); }
    a->fn = fn;
    a->arg = arg;
    a->next = at_exit;
    at_exit = a;
    arm();
}

const struct darwin_sym darwin_syms_tlv[] = {
    NAMED("__tlv_bootstrap", ua_tlv_get_addr),
    NAMED("__tlv_atexit", darwin_tlv_atexit),
    { NULL, NULL }
};
