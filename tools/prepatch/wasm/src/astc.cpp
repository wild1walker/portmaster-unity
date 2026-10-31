// ARM's astc-encoder as astc_encoder_py drives it for pmunity's textures step: LDR profile,
// USE_DECODE_UNORM8, a quality preset with optional limits (textures.py Astc._context), RGBA swizzle,
// one thread (pmucore.wasm runs one per Web Worker).
#include <cstdint>
#include <cstdlib>
#include "astcenc.h"

#define EXPORT(n) __attribute__((export_name(n)))

// quality: 0 fastest, 10 fast, 60 medium, 98 thorough (ASTCENC_PRE_*); a limit < 0 keeps the preset's.
extern "C" EXPORT("pm_astc_ctx") void *pm_astc_ctx(int bw, int bh, float quality, int partition_count_limit,
                                                   int partition2_index_limit)
{
    astcenc_config cfg;
    if (astcenc_config_init(ASTCENC_PRF_LDR, bw, bh, 1, quality, ASTCENC_FLG_USE_DECODE_UNORM8, &cfg) != ASTCENC_SUCCESS)
        return nullptr;
    if (partition_count_limit >= 0 && (unsigned)partition_count_limit < cfg.tune_partition_count_limit)
        cfg.tune_partition_count_limit = partition_count_limit;
    if (partition2_index_limit >= 0 && (unsigned)partition2_index_limit < cfg.tune_2partition_index_limit)
        cfg.tune_2partition_index_limit = partition2_index_limit;
    astcenc_context *ctx = nullptr;
    if (astcenc_context_alloc(&cfg, 1, &ctx) != ASTCENC_SUCCESS) return nullptr;
    return ctx;
}

// rgba: w x h RGBA8; out: one 16-byte block per footprint. 0 on success.
extern "C" EXPORT("pm_astc_compress") int pm_astc_compress(void *ctx, uint8_t *rgba, int w, int h, int bw, int bh, uint8_t *out)
{
    astcenc_image img;
    void *slices[1] = {rgba};
    img.dim_x = w; img.dim_y = h; img.dim_z = 1;
    img.data_type = ASTCENC_TYPE_U8;
    img.data = slices;
    astcenc_swizzle swz = {ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    size_t len = (size_t)((w + bw - 1) / bw) * ((h + bh - 1) / bh) * 16;
    astcenc_error e = astcenc_compress_image((astcenc_context *)ctx, &img, &swz, out, len, 0);
    astcenc_compress_reset((astcenc_context *)ctx);
    return e == ASTCENC_SUCCESS ? 0 : -(int)e;
}
