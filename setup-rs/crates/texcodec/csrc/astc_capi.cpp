// The C name for texture2ddecoder's ASTC decoder (astc.cpp declares it as a C++ function): the
// recorder of pmu-dev astc-guide decodes each tier's blocks with it, as astc_guide.py did through
// texture2ddecoder.decode_astc (BGRA out, rows as they come).
#include "astc.h"

extern "C" int tc_decode_astc(const uint8_t *data, long w, long h, int bw, int bh, uint32_t *image) {
    return decode_astc(data, w, h, bw, bh, image);
}
