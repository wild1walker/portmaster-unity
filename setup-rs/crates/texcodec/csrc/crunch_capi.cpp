// A C interface to texture2ddecoder 1.0.6's crunch unpackers (setup-rs/vendor/texture2ddecoder),
// as its unpack_crunch / unpack_unity_crunch do for UnityPy: a crunched texture's first level as
// DXT blocks. unity: Unity's crunch format (2017.3 and later) rather than the original.
#include <cstdint>

#include "crunch.h"
#include "unitycrunch.h"

extern "C" {

/// The level's DXT blocks in *out (free with tc_crunch_free), their size in *out_len; false when
/// the data does not unpack.
bool tc_crunch_unpack(const uint8_t* data, uint32_t len, int unity, uint8_t** out, uint32_t* out_len)
{
	void* ret = nullptr;
	bool ok = unity ? unity_crunch_unpack_level(data, len, 0, &ret, out_len) : crunch_unpack_level(data, len, 0, &ret, out_len);
	if (!ok)
	{
		delete[] static_cast<uint8_t*>(ret);
		*out = nullptr;
		return false;
	}
	*out = static_cast<uint8_t*>(ret);
	return true;
}

void tc_crunch_free(uint8_t* p)
{
	delete[] p;
}

} // extern "C"
