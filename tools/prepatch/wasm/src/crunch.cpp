// texture2ddecoder's crunch unpackers (MIT): crunched DXT -> plain DXT blocks of the first level, as
// UnityPy calls them (unity: Unity 2017.3+'s variant, else the original crunch).
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include "crunch.h"
#include "unitycrunch.h"

#define EXPORT(n) __attribute__((export_name(n)))

// returns the unpacked bytes (malloc'd; *out_len their size), or nullptr
extern "C" EXPORT("pm_crunch") uint8_t *pm_crunch(int unity, const uint8_t *src, uint32_t len, uint32_t *out_len)
{
    void *ret = nullptr;
    uint32_t size = 0;
    bool ok = unity ? unity_crunch_unpack_level(src, len, 0, &ret, &size) : crunch_unpack_level(src, len, 0, &ret, &size);
    if (!ok || !ret) return nullptr;
    uint8_t *copy = (uint8_t *)malloc(size ? size : 1);
    if (copy) memcpy(copy, ret, size);
    delete[] (uint8_t *)ret;
    *out_len = size;
    return copy;
}
