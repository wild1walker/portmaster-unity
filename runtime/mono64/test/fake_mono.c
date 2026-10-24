// A stand-in libmonobdwgc-2.0.so for test_il2cpp_mono.c: the Mono functions the bridge's
// startup and the tested il2cpp_* calls reach, recording what they were given.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct fclass { const char *name; struct fclass *parent; const char *method; } fclass;

static int domain_obj, corlib_obj, asm_obj;
static __thread int attached;
int fake_icalls, fake_attaches, fake_nostore;
const char *fake_last_icall, *fake_dirs_assembly;

void *mono_jit_init_version(const char *name, const char *ver) { (void)name; (void)ver; attached = 1; return &domain_obj; }
void mono_set_dirs(const char *a, const char *c) { (void)c; fake_dirs_assembly = strdup(a); }
void mono_set_assemblies_path(const char *p) { (void)p; }
void mono_config_parse(const char *f) { (void)f; }
void mono_add_internal_call(const char *name, const void *fn) { (void)fn; fake_icalls++; fake_last_icall = name; }
void *mono_get_corlib(void) { return &corlib_obj; }
void *mono_image_get_assembly(void *img) { (void)img; return &asm_obj; }
void *mono_domain_get(void) { return attached ? &domain_obj : NULL; }
void *mono_thread_attach(void *d) { (void)d; attached = 1; fake_attaches++; return &attached; }
void *mono_runtime_invoke(void *m, void *o, void **p, void **e) { (void)m; (void)o; (void)p; if (e) *e = NULL; return NULL; }
void *mono_domain_assembly_open(void *d, const char *n) { (void)d; (void)n; return NULL; }
void *mono_assembly_get_image(void *a) { (void)a; return &corlib_obj; }
const char *mono_image_get_name(void *i) { (void)i; return "mscorlib"; }
const char *mono_image_get_filename(void *i) { (void)i; return "/x/Managed/mscorlib.dll"; }
int mono_gc_is_incremental(void) { return 1; }
void mono_gc_wbarrier_generic_nostore(void *p) { (void)p; fake_nostore++; }
void *mono_class_get_parent(fclass *k) { return k->parent; }
void *mono_class_get_method_from_name(fclass *k, const char *n, int argc) { (void)argc; return k->method && !strcmp(k->method, n) ? k : NULL; }
uint32_t mono_gchandle_new(void *o, int pinned) { (void)o; return pinned ? 0x80000001u : 7; }
void *mono_string_new(void *d, const char *s) { (void)d; return (void *)s; }
