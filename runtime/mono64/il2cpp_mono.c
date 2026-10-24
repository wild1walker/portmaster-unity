// il2cpp_mono.c - the il2cpp_* API of Unity's IL2CPP players, implemented on Unity's Mono.
//
// Unity's 64-bit ARM Android players exist only for IL2CPP: their libunity.so dlopens libil2cpp.so
// and looks up the il2cpp_* functions (175 in 2017.4, 234 in 6000.0) by name. This library is
// that libil2cpp.so for a Mono game: its own managed assemblies (the desktop build's Managed
// folder, with the IL2CPP player's UnityEngine modules) run on Unity's own Mono runtime built for
// aarch64 Linux (libmonobdwgc-2.0.so, from the Unity-Technologies/mono branch of the game's Unity
// line), and every il2cpp_* call becomes the Mono call Unity's Mono players make for it. No IL2CPP
// conversion, no C++ compile: the game's IL is JIT-compiled at run time as on the desktop.
//
// What makes this work (docs/mono-arm64.md has the evidence):
//   - libunity treats IL2CPP's handles as opaque: Il2CppClass*, MethodInfo*, FieldInfo*,
//     Il2CppType*, Il2CppImage*... are only passed back to il2cpp_* functions. So here they are
//     MonoClass*, MonoMethod*, MonoClassField*, MonoType*, MonoImage*.... The one field libunity
//     reads itself is the class "user data" word at il2cpp_class_get_userdata_offset(), which
//     Unity's Mono has too (MonoClass.unity_user_data, mono_class_get_userdata_offset).
//   - Objects are read in place: the header is 16 bytes (Il2CppObject {klass, monitor}, MonoObject
//     {vtable, synchronisation}), instance fields at il2cpp_field_get_offset (header included in
//     both), arrays {header, bounds, max_length, data at 32} and strings {header, length, UTF-16
//     chars at 20} have the same layouts.
//   - libunity registers its internal calls with the managed names Mono uses
//     ("UnityEngine.Transform::get_position_Injected"), before il2cpp_init: they are queued and
//     handed to mono_add_internal_call once Mono runs. Their C signatures are the ones Mono's JIT
//     calls (AAPCS64 for both).
//   - Managed code is entered only through il2cpp_runtime_invoke (mono_runtime_invoke); libunity
//     never catches IL2CPP's C++ exceptions, so il2cpp_raise_exception is mono_raise_exception, as
//     in Unity's Mono players.
// Semantics follow IL2CPP's where they differ from the obvious Mono call (method lookup by name
// searches the parents, image names keep ".dll", ...).
//
// Loaded by ua_host (runtime/host/mono64.c) as a host library: libunity's dlopen("libil2cpp.so")
// is mapped to it (so_map_library), and ua_host hands it the loader's dlopen/dlsym first
// (ua_mono_setup), for the game's P/Invokes into Android libraries.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <dirent.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "il2cpp_mono.h"

#define API __attribute__((visibility("default")))
typedef int gboolean;
typedef struct _MonoDomain MonoDomain;
typedef struct _MonoAssembly MonoAssembly;
typedef struct _MonoImage MonoImage;
typedef struct _MonoClass MonoClass;
typedef struct _MonoType MonoType;
typedef struct _MonoMethod MonoMethod;
typedef struct _MonoMethodSignature MonoMethodSignature;
typedef struct _MonoClassField MonoClassField;
typedef struct _MonoProperty MonoProperty;
typedef struct _MonoEvent MonoEvent;
typedef struct _MonoObject MonoObject;
typedef struct _MonoString MonoString;
typedef struct _MonoArray MonoArray;
typedef struct _MonoThread MonoThread;
typedef struct _MonoVTable MonoVTable;
typedef struct _MonoCustomAttrInfo MonoCustomAttrInfo;
typedef struct _MonoReflectionType MonoReflectionType;
typedef MonoObject MonoException;
typedef uint16_t Il2CppChar;
typedef void (*fnptr)(void);
typedef gboolean (*MonoStackWalk)(MonoMethod *method, int32_t native_offset, int32_t il_offset, gboolean managed, void *data);

// ---------------------------------------------------------------------------------------------
// Mono's functions, bound with dlsym (one bridge for every Unity line's Mono: what a line lacks
// stays NULL and the il2cpp_* function using it falls back)
#define MONO_FUNCS(X) \
    X(MonoDomain *, mono_jit_init_version, (const char *, const char *)) \
    X(void, mono_set_dirs, (const char *, const char *)) \
    X(void, mono_set_assemblies_path, (const char *)) \
    X(void, mono_set_config_dir, (const char *)) \
    X(void, mono_config_parse, (const char *)) \
    X(void, mono_set_signal_chaining, (gboolean)) \
    X(void, mono_jit_parse_options, (int, char **)) \
    X(void, mono_add_internal_call, (const char *, const void *)) \
    X(MonoDomain *, mono_get_root_domain, (void)) \
    X(MonoDomain *, mono_domain_get, (void)) \
    X(MonoAssembly *, mono_domain_assembly_open, (MonoDomain *, const char *)) \
    X(MonoImage *, mono_assembly_get_image, (MonoAssembly *)) \
    X(MonoImage *, mono_get_corlib, (void)) \
    X(const char *, mono_image_get_name, (MonoImage *)) \
    X(const char *, mono_image_get_filename, (MonoImage *)) \
    X(MonoAssembly *, mono_image_get_assembly, (MonoImage *)) \
    X(uint32_t, mono_image_get_entry_point, (MonoImage *)) \
    X(int, mono_image_get_table_rows, (MonoImage *, int)) \
    X(MonoMethod *, mono_get_method, (MonoImage *, uint32_t, MonoClass *)) \
    X(MonoClass *, mono_unity_class_get, (MonoImage *, uint32_t)) \
    X(MonoClass *, mono_class_get, (MonoImage *, uint32_t)) \
    X(MonoClass *, mono_class_from_name, (MonoImage *, const char *, const char *)) \
    X(MonoClass *, mono_class_from_mono_type, (MonoType *)) \
    X(MonoType *, mono_reflection_type_get_type, (MonoReflectionType *)) \
    X(MonoType *, mono_class_get_type, (MonoClass *)) \
    X(const char *, mono_class_get_name, (MonoClass *)) \
    X(const char *, mono_class_get_namespace, (MonoClass *)) \
    X(MonoClass *, mono_class_get_parent, (MonoClass *)) \
    X(MonoClass *, mono_class_get_nesting_type, (MonoClass *)) \
    X(MonoClass *, mono_class_get_element_class, (MonoClass *)) \
    X(MonoImage *, mono_class_get_image, (MonoClass *)) \
    X(uint32_t, mono_class_get_flags, (MonoClass *)) \
    X(int, mono_class_get_rank, (MonoClass *)) \
    X(uint32_t, mono_class_get_type_token, (MonoClass *)) \
    X(int32_t, mono_class_instance_size, (MonoClass *)) \
    X(int32_t, mono_class_value_size, (MonoClass *, uint32_t *)) \
    X(int, mono_class_num_fields, (MonoClass *)) \
    X(gboolean, mono_class_is_valuetype, (MonoClass *)) \
    X(gboolean, mono_class_is_enum, (MonoClass *)) \
    X(MonoType *, mono_class_enum_basetype, (MonoClass *)) \
    X(gboolean, mono_class_is_subclass_of, (MonoClass *, MonoClass *, gboolean)) \
    X(gboolean, mono_class_is_assignable_from, (MonoClass *, MonoClass *)) \
    X(int32_t, mono_class_array_element_size, (MonoClass *)) \
    X(int32_t, mono_array_element_size, (MonoClass *)) \
    X(MonoClassField *, mono_class_get_fields, (MonoClass *, void **)) \
    X(MonoMethod *, mono_class_get_methods, (MonoClass *, void **)) \
    X(MonoProperty *, mono_class_get_properties, (MonoClass *, void **)) \
    X(MonoEvent *, mono_class_get_events, (MonoClass *, void **)) \
    X(MonoClass *, mono_class_get_nested_types, (MonoClass *, void **)) \
    X(MonoClass *, mono_class_get_interfaces, (MonoClass *, void **)) \
    X(MonoClassField *, mono_class_get_field_from_name, (MonoClass *, const char *)) \
    X(MonoMethod *, mono_class_get_method_from_name, (MonoClass *, const char *, int)) \
    X(MonoProperty *, mono_class_get_property_from_name, (MonoClass *, const char *)) \
    X(gboolean, mono_class_is_generic, (MonoClass *)) \
    X(gboolean, mono_class_is_inflated, (MonoClass *)) \
    X(gboolean, mono_class_is_blittable, (MonoClass *)) \
    X(int, mono_class_get_userdata_offset, (void)) \
    X(void, mono_class_set_userdata, (MonoClass *, void *)) \
    X(gboolean, mono_unity_class_is_abstract, (MonoClass *)) \
    X(gboolean, mono_unity_class_is_interface, (MonoClass *)) \
    X(uint32_t, mono_unity_class_get_data_size, (MonoClass *)) \
    X(void, mono_unity_class_for_each, (void (*)(MonoClass *, void *), void *)) \
    X(MonoVTable *, mono_class_vtable, (MonoDomain *, MonoClass *)) \
    X(MonoVTable *, mono_unity_class_try_get_vtable, (MonoDomain *, MonoClass *)) \
    X(void *, mono_vtable_get_static_field_data, (MonoVTable *)) \
    X(void, mono_runtime_class_init, (MonoVTable *)) \
    X(MonoObject *, mono_object_new, (MonoDomain *, MonoClass *)) \
    X(MonoClass *, mono_object_get_class, (MonoObject *)) \
    X(uint32_t, mono_object_get_size, (MonoObject *)) \
    X(MonoMethod *, mono_object_get_virtual_method, (MonoObject *, MonoMethod *)) \
    X(void *, mono_object_unbox, (MonoObject *)) \
    X(MonoObject *, mono_value_box, (MonoDomain *, MonoClass *, void *)) \
    X(MonoArray *, mono_array_new, (MonoDomain *, MonoClass *, uintptr_t)) \
    X(MonoArray *, mono_array_new_specific, (MonoVTable *, uintptr_t)) \
    X(MonoArray *, mono_array_new_full, (MonoDomain *, MonoClass *, uintptr_t *, intptr_t *)) \
    X(MonoClass *, mono_array_class_get, (MonoClass *, uint32_t)) \
    X(MonoClass *, mono_bounded_array_class_get, (MonoClass *, uint32_t, gboolean)) \
    X(uintptr_t, mono_array_length, (MonoArray *)) \
    X(MonoString *, mono_string_new, (MonoDomain *, const char *)) \
    X(MonoString *, mono_string_new_len, (MonoDomain *, const char *, uint32_t)) \
    X(MonoString *, mono_string_new_utf16, (MonoDomain *, const Il2CppChar *, int32_t)) \
    X(MonoString *, mono_string_new_wrapper, (const char *)) \
    X(MonoString *, mono_string_intern, (MonoString *)) \
    X(MonoString *, mono_string_is_interned, (MonoString *)) \
    X(char *, mono_string_to_utf8, (MonoString *)) \
    X(uint32_t, mono_field_get_flags, (MonoClassField *)) \
    X(const char *, mono_field_get_name, (MonoClassField *)) \
    X(MonoClass *, mono_field_get_parent, (MonoClassField *)) \
    X(uint32_t, mono_field_get_offset, (MonoClassField *)) \
    X(MonoType *, mono_field_get_type, (MonoClassField *)) \
    X(void, mono_field_get_value, (MonoObject *, MonoClassField *, void *)) \
    X(MonoObject *, mono_field_get_value_object, (MonoDomain *, MonoClassField *, MonoObject *)) \
    X(void, mono_field_set_value, (MonoObject *, MonoClassField *, void *)) \
    X(void, mono_field_static_get_value, (MonoVTable *, MonoClassField *, void *)) \
    X(void, mono_field_static_set_value, (MonoVTable *, MonoClassField *, void *)) \
    X(MonoObject *, mono_field_get_object, (MonoDomain *, MonoClass *, MonoClassField *)) \
    X(gboolean, mono_unity_class_field_is_literal, (MonoClassField *)) \
    X(const char *, mono_method_get_name, (MonoMethod *)) \
    X(MonoClass *, mono_method_get_class, (MonoMethod *)) \
    X(MonoMethodSignature *, mono_method_signature, (MonoMethod *)) \
    X(uint32_t, mono_signature_get_param_count, (MonoMethodSignature *)) \
    X(MonoType *, mono_signature_get_params, (MonoMethodSignature *, void **)) \
    X(MonoType *, mono_signature_get_return_type, (MonoMethodSignature *)) \
    X(uint32_t, mono_method_get_flags, (MonoMethod *, uint32_t *)) \
    X(uint32_t, mono_method_get_token, (MonoMethod *)) \
    X(void, mono_method_get_param_names, (MonoMethod *, const char **)) \
    X(MonoObject *, mono_method_get_object, (MonoDomain *, MonoMethod *, MonoClass *)) \
    X(gboolean, unity_mono_method_is_generic, (MonoMethod *)) \
    X(gboolean, unity_mono_method_is_inflated, (MonoMethod *)) \
    X(const char *, mono_property_get_name, (MonoProperty *)) \
    X(MonoMethod *, mono_property_get_get_method, (MonoProperty *)) \
    X(MonoMethod *, mono_property_get_set_method, (MonoProperty *)) \
    X(MonoClass *, mono_property_get_parent, (MonoProperty *)) \
    X(uint32_t, mono_property_get_flags, (MonoProperty *)) \
    X(MonoCustomAttrInfo *, mono_custom_attrs_from_class, (MonoClass *)) \
    X(MonoCustomAttrInfo *, mono_custom_attrs_from_method, (MonoMethod *)) \
    X(MonoCustomAttrInfo *, mono_custom_attrs_from_field, (MonoClass *, MonoClassField *)) \
    X(gboolean, mono_custom_attrs_has_attr, (MonoCustomAttrInfo *, MonoClass *)) \
    X(MonoObject *, mono_custom_attrs_get_attr, (MonoCustomAttrInfo *, MonoClass *)) \
    X(MonoArray *, mono_custom_attrs_construct, (MonoCustomAttrInfo *)) \
    X(void, mono_custom_attrs_free, (MonoCustomAttrInfo *)) \
    X(MonoObject *, mono_runtime_invoke, (MonoMethod *, void *, void **, MonoObject **)) \
    X(MonoObject *, mono_runtime_invoke_array, (MonoMethod *, void *, MonoArray *, MonoObject **)) \
    X(void, mono_runtime_object_init, (MonoObject *)) \
    X(void, mono_runtime_unhandled_exception_policy_set, (int)) \
    X(int, mono_runtime_set_main_args, (int, char **)) \
    X(void, mono_raise_exception, (MonoException *)) \
    X(MonoException *, mono_exception_from_name_msg, (MonoImage *, const char *, const char *, const char *)) \
    X(MonoException *, mono_get_exception_argument_null, (const char *)) \
    X(void, mono_unhandled_exception, (MonoObject *)) \
    X(uint32_t, mono_gchandle_new, (MonoObject *, gboolean)) \
    X(uint32_t, mono_gchandle_new_weakref, (MonoObject *, gboolean)) \
    X(MonoObject *, mono_gchandle_get_target, (uint32_t)) \
    X(void, mono_gchandle_free, (uint32_t)) \
    X(void *, mono_gchandle_new_v2, (MonoObject *, gboolean)) \
    X(void *, mono_gchandle_new_weakref_v2, (MonoObject *, gboolean)) \
    X(MonoObject *, mono_gchandle_get_target_v2, (void *)) \
    X(void, mono_gchandle_free_v2, (void *)) \
    X(void, mono_unity_gc_handles_foreach_get_target, (void (*)(void *, void *), void *)) \
    X(void, mono_gc_collect, (int)) \
    X(int, mono_gc_collect_a_little, (void)) \
    X(void, mono_unity_gc_enable, (void)) \
    X(void, mono_unity_gc_disable, (void)) \
    X(int, mono_unity_gc_is_disabled, (void)) \
    X(void, mono_unity_gc_set_mode, (int)) \
    X(int64_t, mono_gc_get_used_size, (void)) \
    X(int64_t, mono_gc_get_heap_size, (void)) \
    X(gboolean, mono_gc_is_incremental, (void)) \
    X(int64_t, mono_gc_get_max_time_slice_ns, (void)) \
    X(void, mono_gc_set_max_time_slice_ns, (int64_t)) \
    X(void, mono_gc_wbarrier_generic_nostore, (void *)) \
    X(void, mono_unity_gc_heap_foreach, (void (*)(void *, void *), void *)) \
    X(void, mono_unity_stop_gc_world, (void)) \
    X(void, mono_unity_start_gc_world, (void)) \
    X(int, mono_gc_register_root, (char *, size_t, void *, int, void *, const char *)) \
    X(void, mono_gc_deregister_root, (char *)) \
    X(uint32_t, mono_unity_object_header_size, (void)) \
    X(uint32_t, mono_unity_array_object_header_size, (void)) \
    X(uint32_t, mono_unity_offset_of_array_length_in_array_object_header, (void)) \
    X(uint32_t, mono_unity_offset_of_array_bounds_in_array_object_header, (void)) \
    X(uint32_t, mono_unity_allocation_granularity, (void)) \
    X(void *, mono_unity_liveness_calculation_begin, (MonoClass *, uint32_t, void *, void *, void *, void *)) \
    X(void, mono_unity_liveness_calculation_end, (void *)) \
    X(void *, mono_unity_liveness_allocate_struct, (MonoClass *, uint32_t, void *, void *, void *)) \
    X(void, mono_unity_liveness_finalize, (void *)) \
    X(void, mono_unity_liveness_free_struct, (void *)) \
    X(void, mono_unity_liveness_calculation_from_root, (MonoObject *, void *)) \
    X(void, mono_unity_liveness_calculation_from_statics, (void *)) \
    X(MonoThread *, mono_thread_current, (void)) \
    X(MonoThread *, mono_thread_attach, (MonoDomain *)) \
    X(void, mono_thread_detach, (MonoThread *)) \
    X(void, mono_stack_walk, (MonoStackWalk, void *)) \
    X(MonoObject *, mono_type_get_object, (MonoDomain *, MonoType *)) \
    X(int, mono_type_get_type, (MonoType *)) \
    X(char *, mono_type_get_name_full, (MonoType *, int)) \
    X(gboolean, mono_type_is_byref, (MonoType *)) \
    X(uint32_t, mono_type_get_attrs, (MonoType *)) \
    X(gboolean, mono_metadata_type_equal, (MonoType *, MonoType *)) \
    X(gboolean, mono_unity_type_is_static, (MonoType *)) \
    X(gboolean, mono_unity_type_is_pointer_type, (MonoType *)) \
    X(void, mono_unity_type_get_name_full_chunked, (MonoType *, void (*)(void *, void *), void *)) \
    X(MonoClass *, mono_type_get_class, (MonoType *)) \
    X(MonoType *, mono_type_get_ptr_type, (MonoType *)) \
    X(MonoClass *, mono_type_get_array_type_eclass, (MonoType *)) \
    X(void, mono_monitor_enter, (MonoObject *)) \
    X(gboolean, mono_monitor_try_enter, (MonoObject *, uint32_t)) \
    X(void, mono_monitor_exit, (MonoObject *)) \
    X(void *, mono_unity_capture_memory_snapshot, (void)) \
    X(void, mono_unity_free_captured_memory_snapshot, (void *)) \
    X(void, mono_set_find_plugin_callback, (const char *(*)(const char *))) \
    X(void, mono_unity_install_unitytls_interface, (const void *)) \
    X(void *, mono_dl_fallback_register, (void *, void *, void *, void *)) \
    X(void, mono_dllmap_insert, (MonoImage *, const char *, const char *, const char *, const char *)) \
    X(void, mono_trace_set_log_handler, (void (*)(const char *, const char *, const char *, int, void *), void *)) \
    X(void, mono_free, (void *))

#define DECL(r, n, a) static r (*m_##n) a;
MONO_FUNCS(DECL)
#undef DECL

// ---------------------------------------------------------------------------------------------
// state
static void *g_mono;                     // libmonobdwgc-2.0.so
static MonoDomain *g_domain;             // the root domain (IL2CPP has one)
static struct ua_mono_host g_host;
static int g_unity_major, g_unity_minor; // the player's Unity line: some IL2CPP structs grew with it
static int g_trace;                      // UA_MONO64_TRACE: log unusual calls
static int g_incremental;
static char g_data_dir[PATH_MAX], g_config_dir[PATH_MAX], g_managed[PATH_MAX], g_temp_dir[PATH_MAX];
static int g_argc;
static char **g_argv;
static const char *(*g_find_plugin)(const char *);
static void (*g_log_cb)(const char *);
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct { char *name; const void *fn; } icall;
static icall *g_icalls;
static int g_nicalls, g_capicalls;

static MonoAssembly **g_asm;             // what il2cpp_domain_get_assemblies returns
static size_t g_nasm, g_capasm;

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...)
{
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[mono64] %s\n", b);
}
#define TRACE(...) do { if (g_trace) logf_(__VA_ARGS__); } while (0)

static void unity_version(const char *v)
{
    if (v && *v) { g_unity_major = atoi(v); const char *d = strchr(v, '.'); g_unity_minor = d ? atoi(d + 1) : 0; }
}
static int unity_at_least(int major, int minor)
{
    return g_unity_major > major || (g_unity_major == major && g_unity_minor >= minor);
}

static char g_runtime_dir[PATH_MAX];   // the folder of libmonobdwgc-2.0.so
static int runtime_dir_file(const char *name, char *out, size_t n)
{
    struct stat st;
    snprintf(out, n, "%s/%s", g_runtime_dir, name);
    return stat(out, &st) == 0;
}

static void bind_mono(void)
{
    if (g_mono) return;
    char path[PATH_MAX];
    const char *e = getenv("UA_MONO64_RUNTIME");
    if (e && *e) snprintf(path, sizeof path, "%s", e);
    else {
        // libmonobdwgc-2.0.so next to this library
        Dl_info di;
        if (dladdr((void *)bind_mono, &di) && di.dli_fname) {
            snprintf(path, sizeof path, "%s", di.dli_fname);
            char *s = strrchr(path, '/');
            if (s) snprintf(s + 1, sizeof path - (size_t)(s + 1 - path), "libmonobdwgc-2.0.so");
            else snprintf(path, sizeof path, "libmonobdwgc-2.0.so");
        } else snprintf(path, sizeof path, "libmonobdwgc-2.0.so");
    }
    g_mono = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!g_mono) { logf_("cannot load Mono (%s): %s", path, dlerror()); abort(); }
    snprintf(g_runtime_dir, sizeof g_runtime_dir, "%s", path);
    char *slash = strrchr(g_runtime_dir, '/');
    if (slash) *slash = 0; else snprintf(g_runtime_dir, sizeof g_runtime_dir, ".");
#define BIND(r, n, a) m_##n = (r (*) a)dlsym(g_mono, #n);
    MONO_FUNCS(BIND)
#undef BIND
    if (!m_mono_jit_init_version || !m_mono_runtime_invoke || !m_mono_add_internal_call)
    { logf_("%s is not a Mono runtime", path); abort(); }
    logf_("Mono runtime %s", path);
}

__attribute__((constructor)) static void il2cpp_mono_load(void)
{
    g_trace = getenv("UA_MONO64_TRACE") != NULL;
    unity_version(getenv("UA_UNITY_VERSION"));
    bind_mono();
}

API int ua_mono_setup(const struct ua_mono_host *host)
{
    if (!host || host->size < (int)sizeof(struct ua_mono_host)) return -1;
    g_host = *host;
    unity_version(host->unity_version);
    if (host->managed_dir && *host->managed_dir) snprintf(g_managed, sizeof g_managed, "%s", host->managed_dir);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// threads: Mono needs a thread attached before it allocates or runs managed code; IL2CPP is
// lenient there, and Unity's own threads may call in first
static MonoDomain *dom(void) { return g_domain; }
static inline void attached(void)
{
    if (g_domain && m_mono_domain_get && !m_mono_domain_get()) {
        m_mono_thread_attach(g_domain);
        TRACE("thread %ld attached on its first managed call", (long)gettid());
    }
}

// ---------------------------------------------------------------------------------------------
// the game's P/Invokes: Android libraries (plugins) and libunity itself ("__Internal") through
// the loader, when glibc's dlopen cannot load them
static void *dl_load(const char *name, int flags, char **err, void *ud)
{
    (void)flags; (void)err; (void)ud;
    if (!g_host.dlopen) return NULL;
    size_t nl = name ? strlen(name) : 0;
    if (nl > 7 && !strcmp(name + nl - 7, ".dll.so")) return NULL;     // Mono's probe for AOT images: none
    if (!name || !strcmp(name, "__Internal") || !strcmp(name, "libunity.so")) return g_host.dlopen("libunity.so", 0);
    void *h = g_host.dlopen(name, 0);
    if (!h && !strchr(name, '/') && g_host.libdir) {
        char p[PATH_MAX];
        const char *base = name;
        if (!strncmp(base, "lib", 3)) snprintf(p, sizeof p, "%s/%s%s", g_host.libdir, base, strstr(base, ".so") ? "" : ".so");
        else snprintf(p, sizeof p, "%s/lib%s%s", g_host.libdir, base, strstr(base, ".so") ? "" : ".so");
        h = g_host.dlopen(p, 0);
    }
    TRACE("P/Invoke library %s -> %p", name, h);
    return h;
}
static void *dl_symbol(void *h, const char *name, char **err, void *ud)
{
    (void)err; (void)ud;
    return g_host.dlsym ? g_host.dlsym(h, name) : NULL;
}
static void *dl_close(void *h, void *ud) { (void)h; (void)ud; return NULL; }

static void mono_log(const char *domain, const char *level, const char *message, int fatal, void *ud)
{
    (void)ud;
    fprintf(stderr, "[mono] %s%s%s: %s\n", domain ? domain : "", domain ? " " : "", level ? level : "", message);
    if (g_log_cb) g_log_cb(message);
    if (fatal) abort();
}

// ---------------------------------------------------------------------------------------------
// runtime setup
API void il2cpp_set_config_dir(const char *p) { snprintf(g_config_dir, sizeof g_config_dir, "%s", p ? p : ""); TRACE("config dir %s", g_config_dir); }
API void il2cpp_set_data_dir(const char *p) { snprintf(g_data_dir, sizeof g_data_dir, "%s", p ? p : ""); TRACE("data dir %s", g_data_dir); }
API void il2cpp_set_temp_dir(const char *p) { snprintf(g_temp_dir, sizeof g_temp_dir, "%s", p ? p : ""); }
API void il2cpp_set_commandline_arguments(int argc, const char *const argv[], const char *basedir)
{
    (void)basedir;
    g_argc = argc;
    g_argv = calloc((size_t)argc + 1, sizeof *g_argv);
    for (int i = 0; i < argc; i++) g_argv[i] = strdup(argv[i]);
    if (g_domain && m_mono_runtime_set_main_args) m_mono_runtime_set_main_args(g_argc, g_argv);
}
static char *utf16_to_utf8(const Il2CppChar *s)
{
    size_t n = 0;
    while (s && s[n]) n++;
    char *o = malloc(n * 3 + 1), *d = o;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = s[i];
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < n && s[i + 1] >= 0xdc00 && s[i + 1] < 0xe000)
        { c = 0x10000 + ((c - 0xd800) << 10) + (s[i + 1] - 0xdc00); i++; }
        if (c < 0x80) *d++ = (char)c;
        else if (c < 0x800) { *d++ = (char)(0xc0 | c >> 6); *d++ = (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { *d++ = (char)(0xe0 | c >> 12); *d++ = (char)(0x80 | (c >> 6 & 63)); *d++ = (char)(0x80 | (c & 63)); }
        else { *d++ = (char)(0xf0 | c >> 18); *d++ = (char)(0x80 | (c >> 12 & 63)); *d++ = (char)(0x80 | (c >> 6 & 63)); *d++ = (char)(0x80 | (c & 63)); }
    }
    *d = 0;
    return o;
}
API void il2cpp_set_commandline_arguments_utf16(int argc, const Il2CppChar *const argv[], const char *basedir)
{
    char **a = calloc((size_t)argc + 1, sizeof *a);
    for (int i = 0; i < argc; i++) a[i] = utf16_to_utf8(argv[i]);
    il2cpp_set_commandline_arguments(argc, (const char *const *)a, basedir);
}
API void il2cpp_set_config(const char *exe) { (void)exe; }
API void il2cpp_set_config_utf16(const Il2CppChar *exe) { (void)exe; }
API void il2cpp_set_memory_callbacks(void *callbacks) { (void)callbacks; }   // Mono keeps malloc
API void il2cpp_set_default_thread_affinity(int64_t mask) { (void)mask; }
API void il2cpp_memory_pool_set_region_size(size_t size) { (void)size; }
API size_t il2cpp_memory_pool_get_region_size(void) { return 0; }

API void il2cpp_add_internal_call(const char *name, fnptr fn)
{
    pthread_mutex_lock(&g_lock);
    if (g_nicalls == g_capicalls) {
        g_capicalls = g_capicalls ? g_capicalls * 2 : 4096;
        g_icalls = realloc(g_icalls, (size_t)g_capicalls * sizeof *g_icalls);
    }
    g_icalls[g_nicalls].name = strdup(name);
    g_icalls[g_nicalls].fn = (const void *)fn;
    g_nicalls++;
    pthread_mutex_unlock(&g_lock);
    if (g_domain) m_mono_add_internal_call(name, (const void *)fn);
}
API fnptr il2cpp_resolve_icall(const char *name)
{
    for (int i = g_nicalls - 1; i >= 0; i--)
        if (!strcmp(g_icalls[i].name, name)) return (fnptr)g_icalls[i].fn;
    return NULL;
}

static int is_dir_with(const char *dir, const char *file)
{
    char p[PATH_MAX];
    struct stat st;
    snprintf(p, sizeof p, "%s/%s", dir, file);
    return stat(p, &st) == 0;
}

// the folder with the assemblies: ua_host's (UA_MONO64_MANAGED), else what libunity copied out of
// the APK's assets/bin/Data/Managed (the data dir it gives IL2CPP), else its parent's Managed
static void find_managed(void)
{
    if (g_managed[0] && is_dir_with(g_managed, "mscorlib.dll")) return;
    const char *e = getenv("UA_MONO64_MANAGED");
    if (e && *e) { snprintf(g_managed, sizeof g_managed, "%s", e); return; }
    if (g_data_dir[0] && is_dir_with(g_data_dir, "mscorlib.dll")) { snprintf(g_managed, sizeof g_managed, "%s", g_data_dir); return; }
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/Managed", g_data_dir);
    if (is_dir_with(p, "mscorlib.dll")) { snprintf(g_managed, sizeof g_managed, "%s", p); return; }
    snprintf(g_managed, sizeof g_managed, "%s", g_data_dir);
}

static void add_assembly(MonoAssembly *a)
{
    if (!a) return;
    for (size_t i = 0; i < g_nasm; i++) if (g_asm[i] == a) return;
    if (g_nasm == g_capasm) { g_capasm = g_capasm ? g_capasm * 2 : 256; g_asm = realloc(g_asm, g_capasm * sizeof *g_asm); }
    g_asm[g_nasm++] = a;
}

static int dll_name_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

// IL2CPP has every assembly of the build loaded once it runs (il2cpp_domain_get_assemblies lists
// them): load each one in the Managed folder, mscorlib first
static void load_all_assemblies(void)
{
    DIR *d = opendir(g_managed);
    if (!d) { logf_("no Managed folder %s", g_managed); return; }
    char **names = NULL;
    size_t n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 5 || strcasecmp(e->d_name + l - 4, ".dll")) continue;
        if (n == cap) { cap = cap ? cap * 2 : 128; names = realloc(names, cap * sizeof *names); }
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, n, sizeof *names, dll_name_cmp);
    add_assembly(m_mono_image_get_assembly(m_mono_get_corlib()));
    int failed = 0;
    for (size_t i = 0; i < n; i++) {
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s/%s", g_managed, names[i]);
        MonoAssembly *a = m_mono_domain_assembly_open(g_domain, p);
        if (a) add_assembly(a); else { failed++; TRACE("assembly %s did not load", names[i]); }
        free(names[i]);
    }
    free(names);
    logf_("%zu assemblies loaded from %s%s", g_nasm, g_managed, failed ? " (some did not load)" : "");
}

static void init_mono(const char *domain_name)
{
    find_managed();
    char etc[PATH_MAX];
    snprintf(etc, sizeof etc, "%s", g_config_dir[0] ? g_config_dir : g_managed);
    // Mono's config dir is the folder holding mono/config (dllmaps): IL2CPP builds name
    // <data>/etc as the config dir; a Mono build's assets/bin/Data/Managed/etc too
    char cfg[PATH_MAX + 16];
    snprintf(cfg, sizeof cfg, "%s/mono/config", etc);
    struct stat st;
    if (stat(cfg, &st) != 0) {
        snprintf(cfg, sizeof cfg, "%s/etc", g_managed);
        snprintf(etc, sizeof etc, "%s", stat(cfg, &st) == 0 ? cfg : g_managed);
    }
    m_mono_set_dirs(g_managed, etc);
    m_mono_set_assemblies_path(g_managed);
    if (m_mono_set_config_dir) m_mono_set_config_dir(etc);
    m_mono_config_parse(NULL);
    // the class libraries' native helpers are this runtime's, next to it (the desktop config
    // names them in $mono_libdir, the Managed folder here); later entries win
    static const char *const helpers[][2] = {
        { "MonoPosixHelper", "libMonoPosixHelper.so" }, { "System.Native", "libmono-native.so" },
        { "System.Net.Security.Native", "libmono-native.so" }, { "System.Globalization.Native", "libmono-native.so" },
    };
    for (size_t i = 0; i < sizeof helpers / sizeof helpers[0] && m_mono_dllmap_insert; i++) {
        char p[PATH_MAX];
        if (runtime_dir_file(helpers[i][1], p, sizeof p)) m_mono_dllmap_insert(NULL, helpers[i][0], NULL, p, NULL);
    }
    if (m_mono_set_signal_chaining) m_mono_set_signal_chaining(1);
    if (m_mono_trace_set_log_handler) m_mono_trace_set_log_handler(mono_log, NULL);
    if (m_mono_dl_fallback_register) m_mono_dl_fallback_register((void *)dl_load, (void *)dl_symbol, (void *)dl_close, NULL);
    if (m_mono_set_find_plugin_callback && g_find_plugin) m_mono_set_find_plugin_callback(g_find_plugin);
    const char *opts = getenv("UA_MONO64_OPTIONS");      // e.g. "--optimize=..."
    if (opts && *opts && m_mono_jit_parse_options) {
        char *copy = strdup(opts), *v[16];
        int n = 0;
        for (char *t = strtok(copy, " "); t && n < 16; t = strtok(NULL, " ")) v[n++] = t;
        m_mono_jit_parse_options(n, v);
    }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    g_domain = m_mono_jit_init_version(domain_name && *domain_name ? domain_name : "IL2CPP Root Domain", "v4.0.30319");
    if (!g_domain) { logf_("mono_jit_init_version failed"); abort(); }
    for (int i = 0; i < g_nicalls; i++) m_mono_add_internal_call(g_icalls[i].name, g_icalls[i].fn);
    if (g_argc && m_mono_runtime_set_main_args) m_mono_runtime_set_main_args(g_argc, g_argv);
    g_incremental = m_mono_gc_is_incremental && m_mono_gc_is_incremental();
    load_all_assemblies();
    clock_gettime(CLOCK_MONOTONIC, &t1);
    logf_("Mono up: %d internal calls, managed %s, config %s, %.0f ms", g_nicalls, g_managed, etc,
          (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
}

// Mono turns faults in JIT code into NullReferenceExceptions with its SIGSEGV handler (and chains
// to the one before it for other faults): one installed later by libunity takes them over
static void *g_mono_segv;
static void *signal_watch(void *arg)
{
    (void)arg;
    for (int i = 0; i < 60; i++) {
        sleep(1);
        struct sigaction sa;
        sigaction(SIGSEGV, NULL, &sa);
        if ((void *)sa.sa_sigaction != g_mono_segv) {
            Dl_info di;
            logf_("SIGSEGV's handler is no longer Mono's after %d s: %p (%s)", i + 1, (void *)sa.sa_sigaction,
                  dladdr((void *)sa.sa_sigaction, &di) && di.dli_fname ? di.dli_fname : "?");
            return NULL;
        }
    }
    logf_("SIGSEGV's handler is Mono's 60 s after il2cpp_init");
    return NULL;
}

API int il2cpp_init(const char *domain_name)
{
    init_mono(domain_name);
    struct sigaction sa;
    sigaction(SIGSEGV, NULL, &sa);
    g_mono_segv = (void *)sa.sa_sigaction;
    if (g_trace) { pthread_t t; pthread_create(&t, NULL, signal_watch, NULL); pthread_detach(t); }
    return 1;
}
API int il2cpp_init_utf16(const Il2CppChar *domain_name)
{
    char *n = utf16_to_utf8(domain_name);
    int r = il2cpp_init(n);
    free(n);
    return r;
}
API void il2cpp_shutdown(void) { }      // the process ends right after (ua_host); Mono's cleanup can block

API void *il2cpp_alloc(size_t size) { return malloc(size); }
API void il2cpp_free(void *p) { free(p); }   // Mono's strings come from eglib's g_malloc = malloc

// ---------------------------------------------------------------------------------------------
// domain, assemblies, images
API MonoDomain *il2cpp_domain_get(void) { return dom(); }
API MonoImage *il2cpp_get_corlib(void) { return m_mono_get_corlib(); }
API MonoImage *il2cpp_assembly_get_image(MonoAssembly *a) { return a ? m_mono_assembly_get_image(a) : NULL; }
API MonoAssembly **il2cpp_domain_get_assemblies(MonoDomain *d, size_t *size)
{
    (void)d;
    if (size) *size = g_nasm;
    return g_asm;
}
static const char *image_file_name(MonoImage *img)
{
    // IL2CPP's image names are the assemblies' file names ("UnityEngine.CoreModule.dll")
    const char *f = m_mono_image_get_filename(img);
    const char *s = f ? strrchr(f, '/') : NULL;
    return s ? s + 1 : f ? f : m_mono_image_get_name(img);
}
API MonoAssembly *il2cpp_domain_assembly_open(MonoDomain *d, const char *name)
{
    (void)d;
    if (!name) return NULL;
    char want[512];
    snprintf(want, sizeof want, "%s", name);
    size_t l = strlen(want);
    if (l > 4 && !strcasecmp(want + l - 4, ".dll")) want[l - 4] = 0;
    for (size_t i = 0; i < g_nasm; i++) {
        MonoImage *img = m_mono_assembly_get_image(g_asm[i]);
        if (!strcmp(m_mono_image_get_name(img), want)) return g_asm[i];
    }
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/%s.dll", g_managed, want);
    MonoAssembly *a = m_mono_domain_assembly_open(g_domain, p);
    if (a) add_assembly(a);
    TRACE("assembly_open(%s) -> %p", name, (void *)a);
    return a;
}
API MonoAssembly *il2cpp_image_get_assembly(MonoImage *img) { return m_mono_image_get_assembly(img); }
API const char *il2cpp_image_get_name(MonoImage *img) { return image_file_name(img); }
API const char *il2cpp_image_get_filename(MonoImage *img) { return image_file_name(img); }
API MonoMethod *il2cpp_image_get_entry_point(MonoImage *img)
{
    uint32_t t = m_mono_image_get_entry_point(img);
    return t ? m_mono_get_method(img, t, NULL) : NULL;
}
API size_t il2cpp_image_get_class_count(MonoImage *img) { return (size_t)m_mono_image_get_table_rows(img, 2 /* TYPEDEF */); }
API MonoClass *il2cpp_image_get_class(MonoImage *img, size_t index)
{
    uint32_t tok = 0x02000000u | (uint32_t)(index + 1);
    return m_mono_unity_class_get ? m_mono_unity_class_get(img, tok) : m_mono_class_get(img, tok);
}

// ---------------------------------------------------------------------------------------------
// classes
API MonoClass *il2cpp_class_from_name(MonoImage *img, const char *ns, const char *name) { return img ? m_mono_class_from_name(img, ns, name) : NULL; }
API MonoClass *il2cpp_class_from_type(MonoType *t) { return t ? m_mono_class_from_mono_type(t) : NULL; }
API MonoClass *il2cpp_class_from_il2cpp_type(MonoType *t) { return il2cpp_class_from_type(t); }
API MonoClass *il2cpp_class_from_system_type(MonoReflectionType *rt) { return rt ? m_mono_class_from_mono_type(m_mono_reflection_type_get_type(rt)) : NULL; }
API MonoType *il2cpp_class_get_type(MonoClass *k) { return m_mono_class_get_type(k); }
API const char *il2cpp_class_get_name(MonoClass *k) { return m_mono_class_get_name(k); }
API const char *il2cpp_class_get_namespace(MonoClass *k) { return m_mono_class_get_namespace(k); }
API MonoClass *il2cpp_class_get_parent(MonoClass *k) { return m_mono_class_get_parent(k); }
API MonoClass *il2cpp_class_get_declaring_type(MonoClass *k) { return m_mono_class_get_nesting_type(k); }
API MonoClass *il2cpp_class_get_element_class(MonoClass *k) { return m_mono_class_get_element_class(k); }
API MonoImage *il2cpp_class_get_image(MonoClass *k) { return m_mono_class_get_image(k); }
API const char *il2cpp_class_get_assemblyname(MonoClass *k) { return m_mono_image_get_name(m_mono_class_get_image(k)); }
API int il2cpp_class_get_flags(MonoClass *k) { return (int)m_mono_class_get_flags(k); }
API int il2cpp_class_get_rank(MonoClass *k) { return m_mono_class_get_rank(k); }
API uint32_t il2cpp_class_get_type_token(MonoClass *k) { return m_mono_class_get_type_token(k); }
API int32_t il2cpp_class_instance_size(MonoClass *k) { return m_mono_class_instance_size(k); }
API int32_t il2cpp_class_value_size(MonoClass *k, uint32_t *align) { return m_mono_class_value_size(k, align); }
API size_t il2cpp_class_num_fields(MonoClass *k) { return (size_t)m_mono_class_num_fields(k); }
API int il2cpp_class_is_valuetype(MonoClass *k) { return m_mono_class_is_valuetype(k) != 0; }
API int il2cpp_class_is_enum(MonoClass *k) { return m_mono_class_is_enum(k) != 0; }
API MonoType *il2cpp_class_enum_basetype(MonoClass *k) { return m_mono_class_enum_basetype(k); }
API int il2cpp_class_is_blittable(MonoClass *k) { return m_mono_class_is_blittable ? m_mono_class_is_blittable(k) != 0 : 0; }
API int il2cpp_class_is_generic(MonoClass *k) { return m_mono_class_is_generic ? m_mono_class_is_generic(k) != 0 : 0; }
API int il2cpp_class_is_inflated(MonoClass *k) { return m_mono_class_is_inflated ? m_mono_class_is_inflated(k) != 0 : 0; }
API int il2cpp_class_is_abstract(MonoClass *k) { return m_mono_unity_class_is_abstract ? m_mono_unity_class_is_abstract(k) != 0 : (m_mono_class_get_flags(k) & 0x80) != 0; }
API int il2cpp_class_is_interface(MonoClass *k) { return m_mono_unity_class_is_interface ? m_mono_unity_class_is_interface(k) != 0 : (m_mono_class_get_flags(k) & 0x20) != 0; }
API int il2cpp_class_is_subclass_of(MonoClass *k, MonoClass *c, int check_interfaces) { return m_mono_class_is_subclass_of(k, c, check_interfaces) != 0; }
// IL2CPP: klass is parent or derives from it (no interfaces)
API int il2cpp_class_has_parent(MonoClass *k, MonoClass *parent)
{
    for (MonoClass *c = k; c; c = m_mono_class_get_parent(c)) if (c == parent) return 1;
    return 0;
}
API int il2cpp_class_is_assignable_from(MonoClass *k, MonoClass *o) { return m_mono_class_is_assignable_from(k, o) != 0; }
API int il2cpp_class_array_element_size(MonoClass *k) { return m_mono_class_array_element_size(k); }
API int il2cpp_array_element_size(MonoClass *ac) { return m_mono_array_element_size(ac); }
API MonoClassField *il2cpp_class_get_fields(MonoClass *k, void **it) { return m_mono_class_get_fields(k, it); }
API MonoMethod *il2cpp_class_get_methods(MonoClass *k, void **it) { return m_mono_class_get_methods(k, it); }
API MonoProperty *il2cpp_class_get_properties(MonoClass *k, void **it) { return m_mono_class_get_properties(k, it); }
API MonoEvent *il2cpp_class_get_events(MonoClass *k, void **it) { return m_mono_class_get_events(k, it); }
API MonoClass *il2cpp_class_get_nested_types(MonoClass *k, void **it) { return m_mono_class_get_nested_types(k, it); }
API MonoClass *il2cpp_class_get_interfaces(MonoClass *k, void **it) { return m_mono_class_get_interfaces(k, it); }
API MonoClassField *il2cpp_class_get_field_from_name(MonoClass *k, const char *n) { return m_mono_class_get_field_from_name(k, n); }
API MonoProperty *il2cpp_class_get_property_from_name(MonoClass *k, const char *n) { return m_mono_class_get_property_from_name(k, n); }
// IL2CPP searches the class, then its parents (Mono's lookup stays in the class)
API MonoMethod *il2cpp_class_get_method_from_name(MonoClass *k, const char *n, int argc)
{
    for (MonoClass *c = k; c; c = m_mono_class_get_parent(c)) {
        MonoMethod *m = m_mono_class_get_method_from_name(c, n, argc);
        if (m) return m;
    }
    return NULL;
}
API int il2cpp_class_get_userdata_offset(void) { return m_mono_class_get_userdata_offset(); }
API void il2cpp_class_set_userdata(MonoClass *k, void *ud) { m_mono_class_set_userdata(k, ud); }
API uint32_t il2cpp_class_get_data_size(MonoClass *k) { return m_mono_unity_class_get_data_size ? m_mono_unity_class_get_data_size(k) : 0; }
API void *il2cpp_class_get_static_field_data(MonoClass *k)
{
    MonoVTable *vt = m_mono_class_vtable(g_domain, k);
    return vt ? m_mono_vtable_get_static_field_data(vt) : NULL;
}
API void il2cpp_class_for_each(void (*fn)(MonoClass *, void *), void *ud) { if (m_mono_unity_class_for_each) m_mono_unity_class_for_each(fn, ud); }
API int il2cpp_class_is_inited(MonoClass *k)
{
    // the static constructor has run: Mono's vtable exists once the class was initialized
    return m_mono_unity_class_try_get_vtable ? m_mono_unity_class_try_get_vtable(g_domain, k) != NULL : 1;
}
static int custom_has(MonoCustomAttrInfo *ci, MonoClass *attr)
{
    if (!ci) return 0;
    int r = m_mono_custom_attrs_has_attr(ci, attr) != 0;
    m_mono_custom_attrs_free(ci);
    return r;
}
API int il2cpp_class_has_attribute(MonoClass *k, MonoClass *attr) { return custom_has(m_mono_custom_attrs_from_class(k), attr); }

// MONO_TYPE_* = IL2CPP_TYPE_* (both are ECMA-335's element types)
enum { T_VOID = 1, T_BOOLEAN, T_CHAR, T_I1, T_U1, T_I2, T_U2, T_I4, T_U4, T_I8, T_U8, T_R4, T_R8, T_STRING, T_PTR,
       T_BYREF, T_VALUETYPE, T_CLASS, T_VAR, T_ARRAY, T_GENERICINST, T_TYPEDBYREF, T_I = 0x18, T_U, T_FNPTR = 0x1b,
       T_OBJECT, T_SZARRAY, T_MVAR };
static int type_is_ref(MonoType *t)
{
    if (m_mono_type_is_byref(t)) return 0;
    switch (m_mono_type_get_type(t)) {
    case T_STRING: case T_CLASS: case T_OBJECT: case T_SZARRAY: case T_ARRAY: return 1;
    case T_GENERICINST: return !m_mono_class_is_valuetype(m_mono_class_from_mono_type(t));
    default: return 0;
    }
}
static int class_has_refs(MonoClass *k, int depth)
{
    if (depth > 16) return 1;
    for (MonoClass *c = k; c; c = m_mono_class_get_parent(c)) {
        void *it = NULL;
        MonoClassField *f;
        while ((f = m_mono_class_get_fields(c, &it))) {
            if (m_mono_field_get_flags(f) & 0x10) continue;     // static
            MonoType *t = m_mono_field_get_type(f);
            if (type_is_ref(t)) return 1;
            int tt = m_mono_type_get_type(t);
            if ((tt == T_VALUETYPE || tt == T_GENERICINST) && !m_mono_type_is_byref(t)) {
                MonoClass *fc = m_mono_class_from_mono_type(t);
                if (fc != k && !m_mono_class_is_enum(fc) && class_has_refs(fc, depth + 1)) return 1;
            }
        }
        if (m_mono_class_is_valuetype(k)) break;
    }
    return 0;
}
API int il2cpp_class_has_references(MonoClass *k) { return class_has_refs(k, 0); }
// the instance's GC bitmap: one bit per pointer-sized word, set where a reference is
API size_t il2cpp_class_get_bitmap_size(MonoClass *k)
{
    size_t words = ((size_t)m_mono_class_instance_size(k) + sizeof(void *) - 1) / sizeof(void *);
    return (words + 8 * sizeof(size_t) - 1) / (8 * sizeof(size_t));
}
API void il2cpp_class_get_bitmap(MonoClass *k, size_t *bitmap)
{
    size_t n = il2cpp_class_get_bitmap_size(k);
    memset(bitmap, 0, n * sizeof *bitmap);
    for (MonoClass *c = k; c; c = m_mono_class_get_parent(c)) {
        void *it = NULL;
        MonoClassField *f;
        while ((f = m_mono_class_get_fields(c, &it))) {
            if (m_mono_field_get_flags(f) & 0x10) continue;
            if (!type_is_ref(m_mono_field_get_type(f))) continue;
            size_t w = m_mono_field_get_offset(f) / sizeof(void *);
            if (w / (8 * sizeof(size_t)) < n) bitmap[w / (8 * sizeof(size_t))] |= (size_t)1 << (w % (8 * sizeof(size_t)));
        }
    }
}

// ---------------------------------------------------------------------------------------------
// types
API int il2cpp_type_get_type(MonoType *t) { return m_mono_type_get_type(t); }
API MonoObject *il2cpp_type_get_object(MonoType *t) { attached(); return m_mono_type_get_object(g_domain, t); }
// IL2CPP: the element class for arrays, else the type's class
API MonoClass *il2cpp_type_get_class_or_element_class(MonoType *t)
{
    int tt = m_mono_type_get_type(t);
    if ((tt == T_ARRAY || tt == T_SZARRAY) && !m_mono_type_is_byref(t)) return m_mono_class_get_element_class(m_mono_class_from_mono_type(t));
    return m_mono_class_from_mono_type(t);
}
API char *il2cpp_type_get_name(MonoType *t) { return m_mono_type_get_name_full(t, 0 /* IL */); }
API char *il2cpp_type_get_reflection_name(MonoType *t) { return m_mono_type_get_name_full(t, 1 /* REFLECTION */); }
API char *il2cpp_type_get_assembly_qualified_name(MonoType *t) { return m_mono_type_get_name_full(t, 3 /* ASSEMBLY_QUALIFIED */); }
API void il2cpp_type_get_name_chunked(MonoType *t, void (*fn)(void *, void *), void *ud)
{
    if (m_mono_unity_type_get_name_full_chunked) { m_mono_unity_type_get_name_full_chunked(t, fn, ud); return; }
    char *n = il2cpp_type_get_name(t);
    fn(n, ud);
    free(n);
}
API int il2cpp_type_is_byref(MonoType *t) { return m_mono_type_is_byref(t) != 0; }
API uint32_t il2cpp_type_get_attrs(MonoType *t) { return m_mono_type_get_attrs(t); }
API int il2cpp_type_equals(MonoType *a, MonoType *b) { return m_mono_metadata_type_equal(a, b) != 0; }
API int il2cpp_type_is_static(MonoType *t) { return (m_mono_type_get_attrs(t) & 0x10) != 0; }
API int il2cpp_type_is_pointer_type(MonoType *t) { return m_mono_type_get_type(t) == T_PTR; }

// ---------------------------------------------------------------------------------------------
// fields
API int il2cpp_field_get_flags(MonoClassField *f) { return (int)m_mono_field_get_flags(f); }
API const char *il2cpp_field_get_name(MonoClassField *f) { return m_mono_field_get_name(f); }
API MonoClass *il2cpp_field_get_parent(MonoClassField *f) { return m_mono_field_get_parent(f); }
API size_t il2cpp_field_get_offset(MonoClassField *f) { return m_mono_field_get_offset(f); }
API MonoType *il2cpp_field_get_type(MonoClassField *f) { return m_mono_field_get_type(f); }
API void il2cpp_field_get_value(MonoObject *o, MonoClassField *f, void *v) { m_mono_field_get_value(o, f, v); }
API MonoObject *il2cpp_field_get_value_object(MonoClassField *f, MonoObject *o) { attached(); return m_mono_field_get_value_object(g_domain, f, o); }
API void il2cpp_field_set_value(MonoObject *o, MonoClassField *f, void *v) { m_mono_field_set_value(o, f, v); }
API void il2cpp_field_set_value_object(MonoObject *o, MonoClassField *f, MonoObject *v)
{
    if (type_is_ref(m_mono_field_get_type(f))) m_mono_field_set_value(o, f, v);
    else if (v) m_mono_field_set_value(o, f, m_mono_object_unbox(v));
}
static MonoVTable *field_vtable(MonoClassField *f) { attached(); return m_mono_class_vtable(g_domain, m_mono_field_get_parent(f)); }
API void il2cpp_field_static_get_value(MonoClassField *f, void *v) { m_mono_field_static_get_value(field_vtable(f), f, v); }
API void il2cpp_field_static_set_value(MonoClassField *f, void *v) { m_mono_field_static_set_value(field_vtable(f), f, v); }
API int il2cpp_field_is_literal(MonoClassField *f) { return (m_mono_field_get_flags(f) & 0x40) != 0; }
API int il2cpp_field_has_attribute(MonoClassField *f, MonoClass *attr) { return custom_has(m_mono_custom_attrs_from_field(m_mono_field_get_parent(f), f), attr); }
API MonoObject *il2cpp_field_get_object(MonoClassField *f, MonoClass *refclass)
{
    attached();
    return m_mono_field_get_object(g_domain, refclass ? refclass : m_mono_field_get_parent(f), f);
}
// MonoReflectionField { MonoObject object; MonoClass *klass; MonoClassField *field; ... }
API MonoClassField *il2cpp_field_get_from_reflection(MonoObject *rf) { return rf ? *(MonoClassField **)((char *)rf + 24) : NULL; }

// ---------------------------------------------------------------------------------------------
// methods, properties
API const char *il2cpp_method_get_name(MonoMethod *m) { return m_mono_method_get_name(m); }
API MonoClass *il2cpp_method_get_class(MonoMethod *m) { return m_mono_method_get_class(m); }
API MonoClass *il2cpp_method_get_declaring_type(MonoMethod *m) { return m_mono_method_get_class(m); }
API MonoType *il2cpp_method_get_return_type(MonoMethod *m) { return m_mono_signature_get_return_type(m_mono_method_signature(m)); }
API uint32_t il2cpp_method_get_param_count(MonoMethod *m) { return m_mono_signature_get_param_count(m_mono_method_signature(m)); }
API MonoType *il2cpp_method_get_param(MonoMethod *m, uint32_t index)
{
    void *it = NULL;
    MonoType *t;
    uint32_t i = 0;
    while ((t = m_mono_signature_get_params(m_mono_method_signature(m), &it))) if (i++ == index) return t;
    return NULL;
}
API const char *il2cpp_method_get_param_name(MonoMethod *m, uint32_t index)
{
    uint32_t n = il2cpp_method_get_param_count(m);
    if (index >= n) return NULL;
    const char **names = calloc(n, sizeof *names);
    m_mono_method_get_param_names(m, names);     // strings in the image's metadata: they outlive this
    const char *r = names[index];
    free(names);
    return r;
}
API uint32_t il2cpp_method_get_flags(MonoMethod *m, uint32_t *iflags) { return m_mono_method_get_flags(m, iflags); }
API uint32_t il2cpp_method_get_token(MonoMethod *m) { return m_mono_method_get_token(m); }
API int il2cpp_method_is_instance(MonoMethod *m) { return !(m_mono_method_get_flags(m, NULL) & 0x10 /* STATIC */); }
API int il2cpp_method_is_generic(MonoMethod *m) { return m_unity_mono_method_is_generic ? m_unity_mono_method_is_generic(m) != 0 : 0; }
API int il2cpp_method_is_inflated(MonoMethod *m) { return m_unity_mono_method_is_inflated ? m_unity_mono_method_is_inflated(m) != 0 : 0; }
API int il2cpp_method_has_attribute(MonoMethod *m, MonoClass *attr) { return custom_has(m_mono_custom_attrs_from_method(m), attr); }
API MonoObject *il2cpp_method_get_object(MonoMethod *m, MonoClass *refclass) { attached(); return m_mono_method_get_object(g_domain, m, refclass); }
// MonoReflectionMethod { MonoObject object; MonoMethod *method; ... }
API MonoMethod *il2cpp_method_get_from_reflection(MonoObject *rm) { return rm ? *(MonoMethod **)((char *)rm + 16) : NULL; }
API int il2cpp_debug_get_method_info(MonoMethod *m, void *info) { (void)m; (void)info; return 0; }
API void il2cpp_debug_foreach_method(void (*fn)(MonoMethod *, void *, void *), void *ud) { (void)fn; (void)ud; }

API uint32_t il2cpp_property_get_flags(MonoProperty *p) { return m_mono_property_get_flags(p); }
API MonoMethod *il2cpp_property_get_get_method(MonoProperty *p) { return m_mono_property_get_get_method(p); }
API MonoMethod *il2cpp_property_get_set_method(MonoProperty *p) { return m_mono_property_get_set_method(p); }
API const char *il2cpp_property_get_name(MonoProperty *p) { return m_mono_property_get_name(p); }
API MonoClass *il2cpp_property_get_parent(MonoProperty *p) { return m_mono_property_get_parent(p); }

// ---------------------------------------------------------------------------------------------
// custom attributes
API MonoCustomAttrInfo *il2cpp_custom_attrs_from_class(MonoClass *k) { return m_mono_custom_attrs_from_class(k); }
API MonoCustomAttrInfo *il2cpp_custom_attrs_from_method(MonoMethod *m) { return m_mono_custom_attrs_from_method(m); }
API MonoCustomAttrInfo *il2cpp_custom_attrs_from_field(MonoClassField *f) { return m_mono_custom_attrs_from_field(m_mono_field_get_parent(f), f); }
API int il2cpp_custom_attrs_has_attr(MonoCustomAttrInfo *ci, MonoClass *k) { return ci && m_mono_custom_attrs_has_attr(ci, k); }
API MonoObject *il2cpp_custom_attrs_get_attr(MonoCustomAttrInfo *ci, MonoClass *k) { attached(); return ci ? m_mono_custom_attrs_get_attr(ci, k) : NULL; }
API MonoArray *il2cpp_custom_attrs_construct(MonoCustomAttrInfo *ci) { attached(); return ci ? m_mono_custom_attrs_construct(ci) : NULL; }
API void il2cpp_custom_attrs_free(MonoCustomAttrInfo *ci) { if (ci) m_mono_custom_attrs_free(ci); }

// ---------------------------------------------------------------------------------------------
// objects, strings, arrays
API MonoClass *il2cpp_object_get_class(MonoObject *o) { return m_mono_object_get_class(o); }
API uint32_t il2cpp_object_get_size(MonoObject *o) { return m_mono_object_get_size(o); }
API MonoMethod *il2cpp_object_get_virtual_method(MonoObject *o, MonoMethod *m) { return m_mono_object_get_virtual_method(o, m); }
API MonoObject *il2cpp_object_new(MonoClass *k) { attached(); return m_mono_object_new(g_domain, k); }
API void *il2cpp_object_unbox(MonoObject *o) { return m_mono_object_unbox(o); }
API MonoObject *il2cpp_value_box(MonoClass *k, void *data) { attached(); return m_mono_value_box(g_domain, k, data); }
API uint32_t il2cpp_object_header_size(void) { return m_mono_unity_object_header_size ? m_mono_unity_object_header_size() : 2 * sizeof(void *); }
API uint32_t il2cpp_array_object_header_size(void) { return m_mono_unity_array_object_header_size ? m_mono_unity_array_object_header_size() : 4 * sizeof(void *); }
API uint32_t il2cpp_offset_of_array_length_in_array_object_header(void) { return m_mono_unity_offset_of_array_length_in_array_object_header ? m_mono_unity_offset_of_array_length_in_array_object_header() : 3 * sizeof(void *); }
API uint32_t il2cpp_offset_of_array_bounds_in_array_object_header(void) { return m_mono_unity_offset_of_array_bounds_in_array_object_header ? m_mono_unity_offset_of_array_bounds_in_array_object_header() : 2 * sizeof(void *); }
API uint32_t il2cpp_allocation_granularity(void) { return m_mono_unity_allocation_granularity ? m_mono_unity_allocation_granularity() : 2 * sizeof(void *); }

API int32_t il2cpp_string_length(MonoString *s) { return *(int32_t *)((char *)s + 2 * sizeof(void *)); }
API Il2CppChar *il2cpp_string_chars(MonoString *s) { return (Il2CppChar *)((char *)s + 2 * sizeof(void *) + 4); }
API MonoString *il2cpp_string_new(const char *s) { attached(); return m_mono_string_new(g_domain, s); }
API MonoString *il2cpp_string_new_len(const char *s, uint32_t len) { attached(); return m_mono_string_new_len(g_domain, s, len); }
API MonoString *il2cpp_string_new_utf16(const Il2CppChar *s, int32_t len) { attached(); return m_mono_string_new_utf16(g_domain, s, len); }
API MonoString *il2cpp_string_new_wrapper(const char *s) { attached(); return m_mono_string_new(g_domain, s); }
API MonoString *il2cpp_string_intern(MonoString *s) { attached(); return m_mono_string_intern(s); }
API MonoString *il2cpp_string_is_interned(MonoString *s) { return m_mono_string_is_interned(s); }

API MonoClass *il2cpp_array_class_get(MonoClass *e, uint32_t rank) { return m_mono_array_class_get(e, rank); }
API MonoClass *il2cpp_bounded_array_class_get(MonoClass *e, uint32_t rank, int bounded) { return m_mono_bounded_array_class_get(e, rank, bounded); }
API uint32_t il2cpp_array_length(MonoArray *a) { return (uint32_t)m_mono_array_length(a); }
API uint32_t il2cpp_array_get_byte_length(MonoArray *a)
{
    return (uint32_t)(m_mono_array_length(a) * (uintptr_t)m_mono_array_element_size(m_mono_object_get_class((MonoObject *)a)));
}
API MonoArray *il2cpp_array_new(MonoClass *e, uintptr_t n) { attached(); return m_mono_array_new(g_domain, e, n); }
API MonoArray *il2cpp_array_new_specific(MonoClass *ac, uintptr_t n) { attached(); return m_mono_array_new_specific(m_mono_class_vtable(g_domain, ac), n); }
API MonoArray *il2cpp_array_new_full(MonoClass *ac, uintptr_t *lengths, uintptr_t *lower)
{
    attached();
    return m_mono_array_new_full(g_domain, ac, lengths, (intptr_t *)lower);
}

// ---------------------------------------------------------------------------------------------
// invoking managed code, exceptions
API MonoObject *il2cpp_runtime_invoke(MonoMethod *m, void *obj, void **params, MonoObject **exc)
{
    attached();
    return m_mono_runtime_invoke(m, obj, params, exc);
}
API MonoObject *il2cpp_runtime_invoke_convert_args(MonoMethod *m, void *obj, MonoObject **params, int n, MonoObject **exc)
{
    attached();
    MonoArray *a = m_mono_array_new(g_domain, m_mono_class_from_name(m_mono_get_corlib(), "System", "Object"), (uintptr_t)n);
    for (int i = 0; i < n; i++) ((MonoObject **)((char *)a + 4 * sizeof(void *)))[i] = params[i];
    return m_mono_runtime_invoke_array(m, obj, a, exc);
}
API void il2cpp_runtime_class_init(MonoClass *k)
{
    attached();
    MonoVTable *vt = m_mono_class_vtable(g_domain, k);
    if (vt) m_mono_runtime_class_init(vt);
}
API void il2cpp_runtime_object_init(MonoObject *o) { attached(); m_mono_runtime_object_init(o); }
API void il2cpp_runtime_object_init_exception(MonoObject *o, MonoObject **exc)
{
    attached();
    if (exc) *exc = NULL;
    MonoMethod *ctor = m_mono_class_get_method_from_name(m_mono_object_get_class(o), ".ctor", 0);
    if (ctor) m_mono_runtime_invoke(ctor, o, NULL, exc);
}
API void il2cpp_runtime_unhandled_exception_policy_set(int p) { if (m_mono_runtime_unhandled_exception_policy_set) m_mono_runtime_unhandled_exception_policy_set(p); }

API void il2cpp_raise_exception(MonoException *e) { m_mono_raise_exception(e); }
API MonoException *il2cpp_exception_from_name_msg(MonoImage *img, const char *ns, const char *name, const char *msg)
{
    attached();
    return m_mono_exception_from_name_msg(img, ns, name, msg);
}
API MonoException *il2cpp_get_exception_argument_null(const char *arg) { attached(); return m_mono_get_exception_argument_null(arg); }
API void il2cpp_unhandled_exception(MonoObject *e) { m_mono_unhandled_exception(e); }

static char *exception_property(MonoObject *ex, const char *prop)
{
    for (MonoClass *c = m_mono_object_get_class(ex); c; c = m_mono_class_get_parent(c)) {
        MonoProperty *p = m_mono_class_get_property_from_name(c, prop);
        if (!p) continue;
        MonoMethod *g = m_mono_property_get_get_method(p);
        MonoObject *inner = NULL;
        MonoString *s = g ? (MonoString *)m_mono_runtime_invoke(g, ex, NULL, &inner) : NULL;
        return s && !inner ? m_mono_string_to_utf8(s) : NULL;
    }
    return NULL;
}
API void il2cpp_format_exception(MonoObject *ex, char *out, int size)
{
    if (!out || size <= 0) return;
    attached();
    MonoClass *k = m_mono_object_get_class(ex);
    char *msg = exception_property(ex, "Message");
    const char *ns = m_mono_class_get_namespace(k);
    snprintf(out, (size_t)size, "%s%s%s: %s", ns && *ns ? ns : "", ns && *ns ? "." : "", m_mono_class_get_name(k), msg ? msg : "");
    if (msg) m_mono_free(msg);
}
API void il2cpp_format_stack_trace(MonoObject *ex, char *out, int size)
{
    if (!out || size <= 0) return;
    attached();
    char *st = exception_property(ex, "StackTrace");
    snprintf(out, (size_t)size, "%s", st ? st : "");
    if (st) m_mono_free(st);
}
API void il2cpp_native_stack_trace(MonoObject *ex, uintptr_t **addresses, int *n, char **uuid, char **image)
{
    (void)ex;
    if (addresses) *addresses = NULL;
    if (n) *n = 0;
    if (uuid) *uuid = NULL;
    if (image) *image = NULL;
}

// ---------------------------------------------------------------------------------------------
// the collector
API void il2cpp_gc_collect(int gen) { attached(); m_mono_gc_collect(gen); }
API int32_t il2cpp_gc_collect_a_little(void) { attached(); return m_mono_gc_collect_a_little ? m_mono_gc_collect_a_little() : 0; }
API void il2cpp_gc_start_incremental_collection(void) { il2cpp_gc_collect_a_little(); }
API void il2cpp_gc_disable(void) { if (m_mono_unity_gc_disable) m_mono_unity_gc_disable(); }
API void il2cpp_gc_enable(void) { if (m_mono_unity_gc_enable) m_mono_unity_gc_enable(); }
API int il2cpp_gc_is_disabled(void) { return m_mono_unity_gc_is_disabled ? m_mono_unity_gc_is_disabled() != 0 : 0; }
API void il2cpp_gc_set_mode(int mode)
{
    // Il2CppGCMode: 0 disabled, 1 enabled, 2 manual
    if (m_mono_unity_gc_set_mode) { m_mono_unity_gc_set_mode(mode); return; }
    if (mode == 1) il2cpp_gc_enable(); else il2cpp_gc_disable();
}
API int64_t il2cpp_gc_get_used_size(void) { return m_mono_gc_get_used_size(); }
API int64_t il2cpp_gc_get_heap_size(void) { return m_mono_gc_get_heap_size(); }
API int il2cpp_gc_is_incremental(void) { return g_incremental; }
API int64_t il2cpp_gc_get_max_time_slice_ns(void) { return m_mono_gc_get_max_time_slice_ns ? m_mono_gc_get_max_time_slice_ns() : 0; }
API void il2cpp_gc_set_max_time_slice_ns(int64_t ns) { if (m_mono_gc_set_max_time_slice_ns) m_mono_gc_set_max_time_slice_ns(ns); }
// libunity stores every managed reference it keeps (on its stack too, obj == NULL) through this
API void il2cpp_gc_wbarrier_set_field(MonoObject *obj, void **target, void *value)
{
    *target = value;
    if (g_incremental && obj) m_mono_gc_wbarrier_generic_nostore(target);
}
API int il2cpp_gc_has_strict_wbarriers(void) { return 0; }
API void il2cpp_gc_set_external_allocation_tracker(void (*fn)(void *, size_t, int)) { (void)fn; }
API void il2cpp_gc_set_external_wbarrier_tracker(void (*fn)(void **)) { (void)fn; }
API void il2cpp_gc_foreach_heap(void (*fn)(void *, void *), void *ud) { if (m_mono_unity_gc_heap_foreach) m_mono_unity_gc_heap_foreach(fn, ud); }
API void il2cpp_stop_gc_world(void) { if (m_mono_unity_stop_gc_world) m_mono_unity_stop_gc_world(); }
API void il2cpp_start_gc_world(void) { if (m_mono_unity_start_gc_world) m_mono_unity_start_gc_world(); }
// memory the collector scans but never frees (IL2CPP: GC_MALLOC_UNCOLLECTABLE): a root
API void *il2cpp_gc_alloc_fixed(size_t size)
{
    char *p = calloc(1, size ? size : 1);
    if (p && m_mono_gc_register_root) m_mono_gc_register_root(p, size, NULL, 0 /* MONO_ROOT_SOURCE_EXTERNAL */, NULL, "unity");
    return p;
}
API void il2cpp_gc_free_fixed(void *p)
{
    if (p && m_mono_gc_deregister_root) m_mono_gc_deregister_root(p);
    free(p);
}

// GC handles: IL2CPP's are uint32_t to 2022.3 and pointer-sized (Il2CppGCHandle) from 2023.2
// (2023.1 not checked); Mono's of the same line where the runtime has both (its *_v2 handles are
// pointer-sized), so a handle always fits what libunity keeps of it
static int pointer_handles(void)
{
    return unity_at_least(2023, 1) && m_mono_gchandle_new_v2 && m_mono_gchandle_new_weakref_v2 && m_mono_gchandle_get_target_v2 && m_mono_gchandle_free_v2;
}
API uintptr_t il2cpp_gchandle_new(MonoObject *o, int pinned)
{
    attached();
    if (pointer_handles()) return (uintptr_t)m_mono_gchandle_new_v2(o, pinned);
    return m_mono_gchandle_new(o, pinned);
}
API uintptr_t il2cpp_gchandle_new_weakref(MonoObject *o, int track)
{
    attached();
    if (pointer_handles()) return (uintptr_t)m_mono_gchandle_new_weakref_v2(o, track);
    return m_mono_gchandle_new_weakref(o, track);
}
API MonoObject *il2cpp_gchandle_get_target(uintptr_t h)
{
    if (pointer_handles()) return m_mono_gchandle_get_target_v2((void *)h);
    return m_mono_gchandle_get_target((uint32_t)h);
}
API void il2cpp_gchandle_free(uintptr_t h)
{
    if (pointer_handles()) m_mono_gchandle_free_v2((void *)h);
    else m_mono_gchandle_free((uint32_t)h);
}
API void il2cpp_gchandle_foreach_get_target(void (*fn)(void *, void *), void *ud)
{
    if (m_mono_unity_gc_handles_foreach_get_target) m_mono_unity_gc_handles_foreach_get_target(fn, ud);
}

// Resources.UnloadUnusedAssets: which UnityEngine.Objects managed code still reaches
API void *il2cpp_unity_liveness_calculation_begin(MonoClass *filter, int max, void *cb, void *ud, void *started, void *stopped)
{
    attached();
    return m_mono_unity_liveness_calculation_begin(filter, (uint32_t)max, cb, ud, started, stopped);
}
API void il2cpp_unity_liveness_calculation_end(void *s) { m_mono_unity_liveness_calculation_end(s); }
API void *il2cpp_unity_liveness_allocate_struct(MonoClass *filter, int max, void *cb, void *ud, void *realloc_cb)
{
    attached();
    return m_mono_unity_liveness_allocate_struct(filter, (uint32_t)max, cb, ud, realloc_cb);
}
API void il2cpp_unity_liveness_finalize(void *s) { m_mono_unity_liveness_finalize(s); }
API void il2cpp_unity_liveness_free_struct(void *s) { m_mono_unity_liveness_free_struct(s); }
API void il2cpp_unity_liveness_calculation_from_root(MonoObject *root, void *s) { m_mono_unity_liveness_calculation_from_root(root, s); }
API void il2cpp_unity_liveness_calculation_from_statics(void *s) { m_mono_unity_liveness_calculation_from_statics(s); }

// ---------------------------------------------------------------------------------------------
// monitors
API void il2cpp_monitor_enter(MonoObject *o) { attached(); m_mono_monitor_enter(o); }
API int il2cpp_monitor_try_enter(MonoObject *o, uint32_t ms) { attached(); return m_mono_monitor_try_enter(o, ms) != 0; }
API void il2cpp_monitor_exit(MonoObject *o) { m_mono_monitor_exit(o); }
static MonoObject *monitor_call(const char *name, int argc, void **args)
{
    attached();
    MonoClass *k = m_mono_class_from_name(m_mono_get_corlib(), "System.Threading", "Monitor");
    MonoMethod *m = k ? m_mono_class_get_method_from_name(k, name, argc) : NULL;
    MonoObject *exc = NULL;
    return m ? m_mono_runtime_invoke(m, NULL, args, &exc) : NULL;
}
API void il2cpp_monitor_pulse(MonoObject *o) { void *a[] = { o }; monitor_call("Pulse", 1, a); }
API void il2cpp_monitor_pulse_all(MonoObject *o) { void *a[] = { o }; monitor_call("PulseAll", 1, a); }
API void il2cpp_monitor_wait(MonoObject *o) { void *a[] = { o }; monitor_call("Wait", 1, a); }
API int il2cpp_monitor_try_wait(MonoObject *o, uint32_t ms)
{
    int32_t t = (int32_t)ms;
    void *a[] = { o, &t };
    MonoObject *r = monitor_call("Wait", 2, a);
    return r ? *(uint8_t *)m_mono_object_unbox(r) : 0;
}

// ---------------------------------------------------------------------------------------------
// threads, stacks
static MonoThread **g_threads;
static size_t g_nthreads, g_capthreads;
API MonoThread *il2cpp_thread_current(void) { return m_mono_domain_get && m_mono_domain_get() ? m_mono_thread_current() : NULL; }
API MonoThread *il2cpp_thread_attach(MonoDomain *d)
{
    MonoThread *t = m_mono_thread_attach(d ? d : g_domain);
    pthread_mutex_lock(&g_lock);
    size_t i;
    for (i = 0; i < g_nthreads && g_threads[i] != t; i++) { }
    if (i == g_nthreads) {
        if (g_nthreads == g_capthreads) { g_capthreads = g_capthreads ? g_capthreads * 2 : 32; g_threads = realloc(g_threads, g_capthreads * sizeof *g_threads); }
        g_threads[g_nthreads++] = t;
    }
    pthread_mutex_unlock(&g_lock);
    return t;
}
API void il2cpp_thread_detach(MonoThread *t)
{
    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < g_nthreads; i++) if (g_threads[i] == t) { g_threads[i] = g_threads[--g_nthreads]; break; }
    pthread_mutex_unlock(&g_lock);
    if (m_mono_thread_detach) m_mono_thread_detach(t);
}
API MonoThread **il2cpp_thread_get_all_attached_threads(size_t *size) { if (size) *size = g_nthreads; return g_threads; }
API int il2cpp_is_vm_thread(MonoThread *t) { (void)t; return 0; }
API const char *il2cpp_thread_get_name(MonoThread *t, uint32_t *len) { (void)t; if (len) *len = 0; return NULL; }

// Il2CppStackFrameInfo: { method } to 2019.4, + raw_ip in 2020.x, + line, IL offset, file from 2021.1
static size_t frame_size(void)
{
    if (unity_at_least(2021, 1)) return 2 * sizeof(void *) + 8 + sizeof(void *);
    if (unity_at_least(2020, 1)) return 2 * sizeof(void *);
    return sizeof(void *);
}
typedef struct { void (*fn)(const void *, void *); void *ud; int skip, n, want; MonoMethod *found; int il; } walk_ctx;
static gboolean walk_cb(MonoMethod *m, int32_t native, int32_t il, gboolean managed, void *data)
{
    (void)native;
    walk_ctx *c = data;
    if (!managed || !m) return 0;
    if (c->fn) {
        unsigned char info[64];
        memset(info, 0, sizeof info);
        *(MonoMethod **)info = m;
        if (frame_size() > 2 * sizeof(void *)) *(int32_t *)(info + 2 * sizeof(void *) + 4) = il;
        c->fn(info, c->ud);
    }
    if (c->want >= 0 && c->n == c->want) { c->found = m; c->il = il; return 1; }
    c->n++;
    return 0;
}
static void fill_frame(void *frame, MonoMethod *m, int il)
{
    memset(frame, 0, frame_size());
    *(MonoMethod **)frame = m;
    if (frame_size() > 2 * sizeof(void *)) *(int32_t *)((char *)frame + 2 * sizeof(void *) + 4) = il;
}
API void il2cpp_current_thread_walk_frame_stack(void (*fn)(const void *, void *), void *ud)
{
    if (!il2cpp_thread_current()) return;
    walk_ctx c = { fn, ud, 0, 0, -1, NULL, 0 };
    m_mono_stack_walk(walk_cb, &c);
}
API int32_t il2cpp_current_thread_get_stack_depth(void)
{
    if (!il2cpp_thread_current()) return 0;
    walk_ctx c = { NULL, NULL, 0, 0, -1, NULL, 0 };
    m_mono_stack_walk(walk_cb, &c);
    return c.n;
}
API int il2cpp_current_thread_get_frame_at(int32_t offset, void *frame)
{
    if (!il2cpp_thread_current()) return 0;
    walk_ctx c = { NULL, NULL, 0, 0, offset, NULL, 0 };
    m_mono_stack_walk(walk_cb, &c);
    if (!c.found) return 0;
    fill_frame(frame, c.found, c.il);
    return 1;
}
API int il2cpp_current_thread_get_top_frame(void *frame) { return il2cpp_current_thread_get_frame_at(0, frame); }
// another thread's stack: only the calling thread's is walked
API void il2cpp_thread_walk_frame_stack(MonoThread *t, void (*fn)(const void *, void *), void *ud) { if (t == il2cpp_thread_current()) il2cpp_current_thread_walk_frame_stack(fn, ud); }
API int il2cpp_thread_get_top_frame(MonoThread *t, void *frame) { return t == il2cpp_thread_current() ? il2cpp_current_thread_get_top_frame(frame) : 0; }
API int il2cpp_thread_get_frame_at(MonoThread *t, int32_t off, void *frame) { return t == il2cpp_thread_current() ? il2cpp_current_thread_get_frame_at(off, frame) : 0; }
API int32_t il2cpp_thread_get_stack_depth(MonoThread *t) { return t == il2cpp_thread_current() ? il2cpp_current_thread_get_stack_depth() : 0; }
API void il2cpp_override_stack_backtrace(void *fn) { (void)fn; }

// ---------------------------------------------------------------------------------------------
// the rest: memory snapshots, plugins, logging, debugger, statistics
API void *il2cpp_capture_memory_snapshot(void) { attached(); return m_mono_unity_capture_memory_snapshot ? m_mono_unity_capture_memory_snapshot() : calloc(1, 256); }
API void il2cpp_free_captured_memory_snapshot(void *s) { if (m_mono_unity_free_captured_memory_snapshot) m_mono_unity_free_captured_memory_snapshot(s); else free(s); }
API void il2cpp_set_find_plugin_callback(const char *(*fn)(const char *))
{
    g_find_plugin = fn;
    if (g_domain && m_mono_set_find_plugin_callback) m_mono_set_find_plugin_callback(fn);
}
API void il2cpp_register_log_callback(void (*fn)(const char *)) { g_log_cb = fn; }
API void il2cpp_unity_install_unitytls_interface(const void *iface) { if (m_mono_unity_install_unitytls_interface) m_mono_unity_install_unitytls_interface(iface); }
API void il2cpp_unity_set_android_network_up_state_func(void *fn) { (void)fn; }
API void il2cpp_debugger_set_agent_options(const char *o) { (void)o; }
API int il2cpp_is_debugger_attached(void) { return 0; }
API void il2cpp_register_debugger_agent_transport(void *t) { (void)t; }
API int il2cpp_stats_dump_to_file(const char *path) { (void)path; return 0; }
API uint64_t il2cpp_stats_get_value(int stat) { (void)stat; return 0; }
