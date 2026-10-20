// libsyn_dep.so: the DT_NEEDED dependency of the synthetic Android library (syn_lib.c). Built
// freestanding (no glibc headers) like syn_lib.c; see test_syn.c.
int test_next_order(void);              // from the test program's shim table

int dep_value = 42;
int dep_array[8] = { 0, 10, 20, 30, 40, 50, 60, 70 };
const char *dep_name = "dep";           // a relative relocation of its own
int dep_init_order;

int dep_twice(int x) { return 2 * x; }

__attribute__((constructor)) static void dep_init(void) { dep_init_order = test_next_order(); }
