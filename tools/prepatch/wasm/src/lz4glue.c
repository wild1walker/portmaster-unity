/* LZ4 as UnityPy uses it for bundles (python-lz4 4.4.5's bundled lz4 library, BSD-2-Clause):
 * lz4.block.compress(mode="high_compression", compression=9, store_size=False) and decompress; android-data's
 * same-length re-encoding also takes levels 12 and 4 and the default mode. */
#include "lz4.h"
#include "lz4hc.h"

#define EXPORT(n) __attribute__((export_name(n)))

/* -> compressed size, or 0 when it does not fit in cap. level > 0: LZ4HC at that level (mode="high_compression");
 * level <= 0: LZ4's fast compressor with acceleration 1 (python-lz4's mode="default") */
EXPORT("pm_lz4hc")
int pm_lz4hc(const char *src, int len, char *dst, int cap, int level)
{
    if (level <= 0)
        return LZ4_compress_default(src, dst, len, cap);
    return LZ4_compress_HC(src, dst, len, cap, level);
}

EXPORT("pm_lz4_bound")
int pm_lz4_bound(int len) { return LZ4_compressBound(len); }

/* -> decompressed size, or < 0 for corrupt data */
EXPORT("pm_unlz4")
int pm_unlz4(const char *src, int clen, char *dst, int ulen)
{
    return LZ4_decompress_safe(src, dst, clen, ulen);
}
