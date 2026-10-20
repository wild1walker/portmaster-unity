// libsyn_media.so: a synthetic library linked against libmediandk.so (DT_NEEDED), as Unity 2022.3's
// libunity is, importing a function and an AMEDIAFORMAT_KEY_* data object from it. The loader serves
// libmediandk.so from shims (an Android-only library): the DT_NEEDED entry is not loaded from disk and
// the imports bind to the test's shim table. Built freestanding like syn_lib.c; see test_syn.c.
void *AMediaExtractor_new(void);
extern const char *AMEDIAFORMAT_KEY_MIME;

void *syn_media_extractor(void) { return AMediaExtractor_new(); }
const char *syn_media_mime(void) { return AMEDIAFORMAT_KEY_MIME; }
