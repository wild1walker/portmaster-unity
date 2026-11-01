// A C interface to the vendored astc-encoder (setup-rs/vendor/astc-encoder) for texcodec, doing
// what astc_encoder_py 0.1.12's ASTCConfig / ASTCContext.compress do for textures.py: an LDR config
// from a quality preset and flags, a few search limits lowered after astcenc_config_init (as
// textures.py's setattr(cfg, k, min(v, getattr(cfg, k)))), a context for N threads, and the
// compression of one RGBA8 image with the RGBA swizzle, each thread calling in with its index.
//
// Also the few C++ runtime functions the encoder's code refers to (operator new and delete, the
// condition variable of its ParallelManager, the throw helpers its inline library code names),
// so the static musl binary needs no libstdc++.
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>

#include "astcenc.h"

extern "C" {

/// A context for blocks bw x bh at `quality` (astcenc's preset value, e.g. 10 = fast) with `flags`;
/// partition_limit / index2_limit: when >= 0, tune_partition_count_limit /
/// tune_2partition_index_limit are lowered to them (min with the preset's). NULL on error (status).
astcenc_context* tc_astc_new(unsigned bw, unsigned bh, float quality, unsigned flags, int partition_limit,
                             int index2_limit, unsigned threads, int* status)
{
	astcenc_config config;
	astcenc_error err = astcenc_config_init(ASTCENC_PRF_LDR, bw, bh, 1, quality, flags, &config);
	if (err == ASTCENC_SUCCESS)
	{
		if (partition_limit >= 0 && static_cast<unsigned>(partition_limit) < config.tune_partition_count_limit)
		{
			config.tune_partition_count_limit = static_cast<unsigned>(partition_limit);
		}
		if (index2_limit >= 0 && static_cast<unsigned>(index2_limit) < config.tune_2partition_index_limit)
		{
			config.tune_2partition_index_limit = static_cast<unsigned>(index2_limit);
		}
	}
	astcenc_context* ctx = nullptr;
	if (err == ASTCENC_SUCCESS)
	{
		err = astcenc_context_alloc(&config, threads, &ctx);
	}
	*status = err;
	return err == ASTCENC_SUCCESS ? ctx : nullptr;
}

/// One thread's share of compressing a w x h RGBA8 image (rows as given) into out (16 bytes a
/// block); every thread index 0..threads-1 must call it, then tc_astc_reset once.
int tc_astc_compress(astcenc_context* ctx, const unsigned char* rgba, unsigned w, unsigned h, unsigned char* out,
                     size_t out_len, unsigned thread_index)
{
	void* slices[1] = {const_cast<unsigned char*>(rgba)};
	astcenc_image image;
	image.dim_x = w;
	image.dim_y = h;
	image.dim_z = 1;
	image.data_type = ASTCENC_TYPE_U8;
	image.data = slices;
	const astcenc_swizzle swizzle{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
	return astcenc_compress_image(ctx, &image, &swizzle, out, out_len, thread_index);
}

int tc_astc_reset(astcenc_context* ctx)
{
	return astcenc_compress_reset(ctx);
}

void tc_astc_free(astcenc_context* ctx)
{
	astcenc_context_free(ctx);
}

const char* tc_astc_error(int status)
{
	return astcenc_get_error_string(static_cast<astcenc_error>(status));
}

} // extern "C"

// ---------------------------------------------------------------- the C++ runtime it needs

void* operator new(std::size_t n)
{
	void* p = std::malloc(n ? n : 1);
	if (!p)
	{
		std::abort();
	}
	return p;
}

void* operator new[](std::size_t n)
{
	return operator new(n);
}

void* operator new(std::size_t n, const std::nothrow_t&) noexcept
{
	return std::malloc(n ? n : 1);
}

void* operator new[](std::size_t n, const std::nothrow_t&) noexcept
{
	return std::malloc(n ? n : 1);
}

void operator delete(void* p) noexcept
{
	std::free(p);
}

void operator delete[](void* p) noexcept
{
	std::free(p);
}

void operator delete(void* p, std::size_t) noexcept
{
	std::free(p);
}

void operator delete[](void* p, std::size_t) noexcept
{
	std::free(p);
}

// (as libstdc++'s condition_variable.cc: a pthread condition variable)
std::condition_variable::condition_variable() noexcept = default;
std::condition_variable::~condition_variable() noexcept = default;

void std::condition_variable::wait(std::unique_lock<std::mutex>& lock)
{
	_M_cond.wait(*lock.mutex());
}

void std::condition_variable::notify_one() noexcept
{
	_M_cond.notify_one();
}

void std::condition_variable::notify_all() noexcept
{
	_M_cond.notify_all();
}

namespace std
{
void __throw_system_error(int)
{
	std::abort();
}

void __throw_bad_function_call()
{
	std::abort();
}

void __throw_bad_alloc()
{
	std::abort();
}

void __throw_length_error(const char*)
{
	std::abort();
}
} // namespace std

extern "C" void __cxa_pure_virtual()
{
	std::abort();
}
