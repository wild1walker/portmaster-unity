// a shared object with no DT_NEEDED at all and data only, as Mono's AOT images are (test_syn.c: served by
// so_set_dlopen_rewrite, loaded by so_loader although it does not look like an Android library)
int plain_value = 77;
const char *plain_name = "plain";       /* a relative relocation */
