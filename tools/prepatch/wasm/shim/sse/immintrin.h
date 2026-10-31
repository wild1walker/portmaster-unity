/* SSE2..SSE4.1 intrinsics on WebAssembly SIMD (wasm_simd128.h), with SSE's semantics, for building
 * astc-encoder's own SSE vector layer (astcenc_vecmathlib_sse_4.h, ASTCENC_SSE=41, no AVX/F16C/POPCNT)
 * into pmucore.wasm. Only what astc-encoder uses. Where WebAssembly differs from SSE the SSE result is
 * produced: min/max (NaN and signed zero: the second operand), float->int truncation (0x80000000 out
 * of range), pshufb (only bit 7 zeroes a lane), shifts by 32 or more (zero / sign fill), byte shifts. */
#pragma once
#include <wasm_simd128.h>
#include <stdint.h>
#include <string.h>

typedef float __m128 __attribute__((__vector_size__(16), __aligned__(16)));
typedef long long __m128i __attribute__((__vector_size__(16), __aligned__(16)));
typedef float __m128_u __attribute__((__vector_size__(16), __aligned__(1)));

#define V(x) ((v128_t)(x))
#define F(x) ((__m128)(x))
#define I(x) ((__m128i)(x))
#define SI static inline __attribute__((always_inline))

/* casts, sets, loads, stores */
SI __m128 _mm_castsi128_ps(__m128i a) { return F(a); }
SI __m128i _mm_castps_si128(__m128 a) { return I(a); }
SI __m128 _mm_setzero_ps(void) { return F(wasm_f32x4_splat(0.0f)); }
SI __m128i _mm_setzero_si128(void) { return I(wasm_i32x4_splat(0)); }
SI __m128 _mm_set1_ps(float a) { return F(wasm_f32x4_splat(a)); }
SI __m128 _mm_set_ps(float d, float c, float b, float a) { return F(wasm_f32x4_make(a, b, c, d)); }
SI __m128i _mm_set1_epi32(int a) { return I(wasm_i32x4_splat(a)); }
SI __m128i _mm_set1_epi16(short a) { return I(wasm_i16x8_splat(a)); }
SI __m128i _mm_set1_epi8(char a) { return I(wasm_i8x16_splat(a)); }
SI __m128i _mm_set_epi32(int d, int c, int b, int a) { return I(wasm_i32x4_make(a, b, c, d)); }
SI __m128i _mm_set_epi8(char e15, char e14, char e13, char e12, char e11, char e10, char e9, char e8,
                        char e7, char e6, char e5, char e4, char e3, char e2, char e1, char e0) {
    return I(wasm_i8x16_make(e0, e1, e2, e3, e4, e5, e6, e7, e8, e9, e10, e11, e12, e13, e14, e15));
}
SI __m128 _mm_loadu_ps(const float *p) { return F(wasm_v128_load(p)); }
SI __m128 _mm_load_ps(const float *p) { return F(wasm_v128_load(p)); }
SI __m128 _mm_load_ps1(const float *p) { return F(wasm_f32x4_splat(*p)); }
SI __m128i _mm_loadu_si128(const __m128i *p) { return I(wasm_v128_load(p)); }
SI __m128i _mm_load_si128(const __m128i *p) { return I(wasm_v128_load(p)); }
SI __m128i _mm_lddqu_si128(const __m128i *p) { return I(wasm_v128_load(p)); }
SI __m128i _mm_loadu_si32(const void *p) { int32_t v; memcpy(&v, p, 4); return I(wasm_i32x4_make(v, 0, 0, 0)); }
SI __m128i _mm_loadu_si64(const void *p) { int64_t v; memcpy(&v, p, 8); return I(wasm_i64x2_make(v, 0)); }
SI void _mm_storeu_ps(float *p, __m128 a) { wasm_v128_store(p, V(a)); }
SI void _mm_store_ps(float *p, __m128 a) { wasm_v128_store(p, V(a)); }
SI void _mm_store_ss(float *p, __m128 a) { *p = wasm_f32x4_extract_lane(V(a), 0); }
SI void _mm_store_si128(__m128i *p, __m128i a) { wasm_v128_store(p, V(a)); }
SI void _mm_storeu_si64(void *p, __m128i a) { int64_t v = wasm_i64x2_extract_lane(V(a), 0); memcpy(p, &v, 8); }
SI void _mm_storel_epi64(__m128i *p, __m128i a) { int64_t v = wasm_i64x2_extract_lane(V(a), 0); memcpy(p, &v, 8); }
SI void _mm_maskmoveu_si128(__m128i a, __m128i mask, char *p) {
    uint8_t d[16], m[16]; wasm_v128_store(d, V(a)); wasm_v128_store(m, V(mask));
    for (int i = 0; i < 16; i++) if (m[i] & 0x80) p[i] = (char)d[i];
}
SI float _mm_cvtss_f32(__m128 a) { return wasm_f32x4_extract_lane(V(a), 0); }
SI int _mm_cvtsi128_si32(__m128i a) { return wasm_i32x4_extract_lane(V(a), 0); }
SI __m128i _mm_cvtsi32_si128(int a) { return I(wasm_i32x4_make(a, 0, 0, 0)); }
SI __m128i _mm_cvtsi64_si128(long long a) { return I(wasm_i64x2_make(a, 0)); }

/* float arithmetic */
SI __m128 _mm_add_ps(__m128 a, __m128 b) { return F(wasm_f32x4_add(V(a), V(b))); }
SI __m128 _mm_sub_ps(__m128 a, __m128 b) { return F(wasm_f32x4_sub(V(a), V(b))); }
SI __m128 _mm_mul_ps(__m128 a, __m128 b) { return F(wasm_f32x4_mul(V(a), V(b))); }
SI __m128 _mm_div_ps(__m128 a, __m128 b) { return F(wasm_f32x4_div(V(a), V(b))); }
SI __m128 _mm_sqrt_ps(__m128 a) { return F(wasm_f32x4_sqrt(V(a))); }
/* SSE min(a, b) = a < b ? a : b and max(a, b) = a > b ? a : b: WebAssembly's pmin/pmax, operands swapped */
SI __m128 _mm_min_ps(__m128 a, __m128 b) { return F(wasm_f32x4_pmin(V(b), V(a))); }
SI __m128 _mm_max_ps(__m128 a, __m128 b) { return F(wasm_f32x4_pmax(V(b), V(a))); }
SI __m128 _mm_add_ss(__m128 a, __m128 b) { a[0] = a[0] + b[0]; return a; }
SI __m128 _mm_min_ss(__m128 a, __m128 b) { a[0] = a[0] < b[0] ? a[0] : b[0]; return a; }
SI __m128 _mm_max_ss(__m128 a, __m128 b) { a[0] = a[0] > b[0] ? a[0] : b[0]; return a; }
SI __m128 _mm_hadd_ps(__m128 a, __m128 b) { return (__m128){a[0] + a[1], a[2] + a[3], b[0] + b[1], b[2] + b[3]}; }
#define _MM_FROUND_TO_NEAREST_INT 0x00
#define _MM_FROUND_NO_EXC 0x08
SI __m128 _mm_round_ps_nearest(__m128 a) { return F(wasm_f32x4_nearest(V(a))); }
#define _mm_round_ps(a, mode) (((mode) & 3) == 0 ? _mm_round_ps_nearest(a) : (__builtin_trap(), (a)))

/* comparisons (all false for NaN, except neq) */
SI __m128 _mm_cmpeq_ps(__m128 a, __m128 b) { return F(wasm_f32x4_eq(V(a), V(b))); }
SI __m128 _mm_cmpneq_ps(__m128 a, __m128 b) { return F(wasm_f32x4_ne(V(a), V(b))); }
SI __m128 _mm_cmplt_ps(__m128 a, __m128 b) { return F(wasm_f32x4_lt(V(a), V(b))); }
SI __m128 _mm_cmple_ps(__m128 a, __m128 b) { return F(wasm_f32x4_le(V(a), V(b))); }
SI __m128 _mm_cmpgt_ps(__m128 a, __m128 b) { return F(wasm_f32x4_gt(V(a), V(b))); }
SI __m128 _mm_cmpge_ps(__m128 a, __m128 b) { return F(wasm_f32x4_ge(V(a), V(b))); }
SI __m128i _mm_cmpeq_epi32(__m128i a, __m128i b) { return I(wasm_i32x4_eq(V(a), V(b))); }
SI __m128i _mm_cmplt_epi32(__m128i a, __m128i b) { return I(wasm_i32x4_lt(V(a), V(b))); }
SI __m128i _mm_cmpgt_epi32(__m128i a, __m128i b) { return I(wasm_i32x4_gt(V(a), V(b))); }
SI int _mm_movemask_ps(__m128 a) { return (int)wasm_i32x4_bitmask(V(a)); }

/* logic */
SI __m128 _mm_and_ps(__m128 a, __m128 b) { return F(wasm_v128_and(V(a), V(b))); }
SI __m128 _mm_or_ps(__m128 a, __m128 b) { return F(wasm_v128_or(V(a), V(b))); }
SI __m128 _mm_xor_ps(__m128 a, __m128 b) { return F(wasm_v128_xor(V(a), V(b))); }
SI __m128 _mm_andnot_ps(__m128 a, __m128 b) { return F(wasm_v128_andnot(V(b), V(a))); }      /* ~a & b */
SI __m128i _mm_and_si128(__m128i a, __m128i b) { return I(wasm_v128_and(V(a), V(b))); }
SI __m128i _mm_or_si128(__m128i a, __m128i b) { return I(wasm_v128_or(V(a), V(b))); }
SI __m128i _mm_xor_si128(__m128i a, __m128i b) { return I(wasm_v128_xor(V(a), V(b))); }
SI __m128i _mm_andnot_si128(__m128i a, __m128i b) { return I(wasm_v128_andnot(V(b), V(a))); }
/* blends: b where the mask's top bit is set */
SI __m128 _mm_blendv_ps(__m128 a, __m128 b, __m128 m) { return F(wasm_v128_bitselect(V(b), V(a), wasm_i32x4_shr(V(m), 31))); }
SI __m128i _mm_blendv_epi8(__m128i a, __m128i b, __m128i m) { return I(wasm_v128_bitselect(V(b), V(a), wasm_i8x16_shr(V(m), 7))); }

/* integers */
SI __m128i _mm_add_epi32(__m128i a, __m128i b) { return I(wasm_i32x4_add(V(a), V(b))); }
SI __m128i _mm_sub_epi32(__m128i a, __m128i b) { return I(wasm_i32x4_sub(V(a), V(b))); }
SI __m128i _mm_sub_epi8(__m128i a, __m128i b) { return I(wasm_i8x16_sub(V(a), V(b))); }
SI __m128i _mm_mullo_epi32(__m128i a, __m128i b) { return I(wasm_i32x4_mul(V(a), V(b))); }
SI __m128i _mm_mul_epu32(__m128i a, __m128i b) {               /* lanes 0 and 2, unsigned, 64-bit products */
    v128_t x = wasm_i32x4_shuffle(V(a), V(a), 0, 2, 0, 2), y = wasm_i32x4_shuffle(V(b), V(b), 0, 2, 0, 2);
    return I(wasm_u64x2_extmul_low_u32x4(x, y));
}
SI __m128i _mm_min_epi32(__m128i a, __m128i b) { return I(wasm_i32x4_min(V(a), V(b))); }
SI __m128i _mm_max_epi32(__m128i a, __m128i b) { return I(wasm_i32x4_max(V(a), V(b))); }
SI __m128i _mm_slli_epi32(__m128i a, int n) { return n > 31 ? _mm_setzero_si128() : I(wasm_i32x4_shl(V(a), n)); }
SI __m128i _mm_srli_epi32(__m128i a, int n) { return n > 31 ? _mm_setzero_si128() : I(wasm_u32x4_shr(V(a), n)); }
SI __m128i _mm_srai_epi32(__m128i a, int n) { return I(wasm_i32x4_shr(V(a), n > 31 ? 31 : n)); }
SI __m128i _mm_packs_epi32(__m128i a, __m128i b) { return I(wasm_i16x8_narrow_i32x4(V(a), V(b))); }
SI __m128i _mm_cvtepu8_epi32(__m128i a) { return I(wasm_u32x4_extend_low_u16x8(wasm_u16x8_extend_low_u8x16(V(a)))); }
SI __m128i _mm_cvtepu16_epi32(__m128i a) { return I(wasm_u32x4_extend_low_u16x8(V(a))); }
SI __m128 _mm_cvtepi32_ps(__m128i a) { return F(wasm_f32x4_convert_i32x4(V(a))); }
SI __m128i _mm_cvttps_epi32(__m128 a) {                          /* out of range or NaN: 0x80000000 */
    v128_t t = wasm_i32x4_trunc_sat_f32x4(V(a));
    v128_t ok = wasm_f32x4_lt(wasm_f32x4_abs(V(a)), wasm_f32x4_splat(2147483648.0f));
    return I(wasm_v128_bitselect(t, wasm_i32x4_splat((int)0x80000000), ok));
}
/* pshufb: a byte with bit 7 set gives 0, else the low 4 bits pick (swizzle zeroes indexes >= 16) */
SI __m128i _mm_shuffle_epi8(__m128i a, __m128i b) { return I(wasm_i8x16_swizzle(V(a), wasm_v128_and(V(b), wasm_i8x16_splat((char)0x8F)))); }
SI __m128i _mm_unpacklo_epi8(__m128i a, __m128i b) { return I(wasm_i8x16_shuffle(V(a), V(b), 0, 16, 1, 17, 2, 18, 3, 19, 4, 20, 5, 21, 6, 22, 7, 23)); }
SI __m128i _mm_unpackhi_epi8(__m128i a, __m128i b) { return I(wasm_i8x16_shuffle(V(a), V(b), 8, 24, 9, 25, 10, 26, 11, 27, 12, 28, 13, 29, 14, 30, 15, 31)); }
SI __m128i _mm_unpacklo_epi16(__m128i a, __m128i b) { return I(wasm_i16x8_shuffle(V(a), V(b), 0, 8, 1, 9, 2, 10, 3, 11)); }
SI __m128i _mm_unpacklo_epi32(__m128i a, __m128i b) { return I(wasm_i32x4_shuffle(V(a), V(b), 0, 4, 1, 5)); }
SI __m128 _mm_movehl_ps(__m128 a, __m128 b) { return F(wasm_i32x4_shuffle(V(a), V(b), 6, 7, 2, 3)); }

/* immediates (compile-time constants) */
#define _MM_SHUFFLE(z, y, x, w) (((z) << 6) | ((y) << 4) | ((x) << 2) | (w))
#define _mm_shuffle_epi32(a, imm) \
    I(wasm_i32x4_shuffle(V(a), V(a), (imm) & 3, ((imm) >> 2) & 3, ((imm) >> 4) & 3, ((imm) >> 6) & 3))
#define _mm_shuffle_ps(a, b, imm) \
    F(wasm_i32x4_shuffle(V(a), V(b), (imm) & 3, ((imm) >> 2) & 3, (((imm) >> 4) & 3) + 4, (((imm) >> 6) & 3) + 4))
#define _mm_insert_epi32(a, x, i) I(wasm_i32x4_replace_lane(V(a), (i), (x)))
/* insert_ps: lane (imm >> 6) of b into lane (imm >> 4) & 3 of a, then lanes of imm & 15 zeroed */
#define _mm_insert_ps(a, b, imm) __extension__({ __m128 _r = (a); _r[((imm) >> 4) & 3] = (b)[((imm) >> 6) & 3]; \
    for (int _i = 0; _i < 4; _i++) if ((imm) & (1 << _i)) _r[_i] = 0.0f; _r; })
/* byte shifts of the whole register (n >= 16: zero) */
#define _PM_S(k, n) ((k) + (n) < 16 ? (k) + (n) : 16)
#define _mm_srli_si128(a, n) I(wasm_i8x16_shuffle(V(a), wasm_i32x4_splat(0), _PM_S(0, n), _PM_S(1, n), _PM_S(2, n), _PM_S(3, n), \
    _PM_S(4, n), _PM_S(5, n), _PM_S(6, n), _PM_S(7, n), _PM_S(8, n), _PM_S(9, n), _PM_S(10, n), _PM_S(11, n), _PM_S(12, n), \
    _PM_S(13, n), _PM_S(14, n), _PM_S(15, n)))
#define _PM_L(k, n) ((k) >= (n) ? (k) - (n) : 16)
#define _mm_bslli_si128(a, n) I(wasm_i8x16_shuffle(V(a), wasm_i32x4_splat(0), _PM_L(0, n), _PM_L(1, n), _PM_L(2, n), _PM_L(3, n), \
    _PM_L(4, n), _PM_L(5, n), _PM_L(6, n), _PM_L(7, n), _PM_L(8, n), _PM_L(9, n), _PM_L(10, n), _PM_L(11, n), _PM_L(12, n), \
    _PM_L(13, n), _PM_L(14, n), _PM_L(15, n)))
#define _mm_slli_si128 _mm_bslli_si128
