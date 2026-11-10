// gen.h - how gen.c and gen_xopen.c write a line of darwin_abi.h
#include <stddef.h>
#include <stdio.h>

#define C(name) printf("#define DARWIN_%-28s %lld\n", #name, (long long)(name));
#define X(name) printf("#define DARWIN_%-28s 0x%llx\n", #name, (unsigned long long)(name));
#define XN(tag, v) printf("#define DARWIN_%-28s 0x%llx\n", #tag, (unsigned long long)(v));
#define SZ(type, tag) printf("#define DARWIN_SIZEOF_%-21s %zu\n", #tag, sizeof(type));
#define OFF(type, tag, field) printf("#define DARWIN_%s_%-*s %zu\n", #tag, (int)(27 - sizeof #tag), #field, offsetof(type, field));
#define SIG(tag, init_type, init) { init_type v = init; printf("#define DARWIN_%-28s 0x%lx\n", #tag, *(long *)&v); }
#define H(text) printf("\n// %s\n", text);

// gen_xopen.c: what the SDK's headers show only with _XOPEN_SOURCE defined
void gen_xopen(void);
