#ifndef PS2_RUNTIME_MACROS_H
#define PS2_RUNTIME_MACROS_H
#include <cstdint>
#include <cmath>
#include <cstring>
#include <bit>
#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(USE_SSE2NEON)
#include "sse2neon.h"
#else
#include <immintrin.h> // For SSE/AVX intrinsics
#endif

#include "ps2_runtime.h"

static inline int32_t Ps2ExtractEpi32(__m128i v, int index)
{
    switch (index & 3)
    {
    case 0:
        return _mm_extract_epi32(v, 0);
    case 1:
        return _mm_extract_epi32(v, 1);
    case 2:
        return _mm_extract_epi32(v, 2);
    default:
        return _mm_extract_epi32(v, 3);
    }
}

static inline int64_t Ps2ExtractEpi64(__m128i v, int index)
{
    if ((index & 1) == 0)
    {
        return _mm_cvtsi128_si64(v);
    }
    else
    {
        return _mm_extract_epi64(v, 1);
    }
}

static inline uint32_t ps2_clz32(uint32_t x)
{
    return static_cast<uint32_t>(std::countl_zero(x));
}

static inline uint64_t Ps2HiLoToU64(uint64_t hi, uint64_t lo)
{
    return ((hi & 0xFFFFFFFFull) << 32) | (lo & 0xFFFFFFFFull);
}

static inline uint64_t Ps2SignExt32ToU64(uint32_t v)
{
    return (uint64_t)(int64_t)(int32_t)v;
}

// PLZCW: Count leading bits that match the sign bit, minus 1.
// For positive values: count leading zeros minus 1 (excludes sign bit).
// For negative values: count leading ones minus 1 (excludes sign bit).
// Special cases: 0x00000000 -> 31, 0xFFFFFFFF -> 31.
static inline uint32_t ps2_plzcw32(uint32_t x)
{
    if (x == 0 || x == 0xFFFFFFFF)
        return 31;
    if (x & 0x80000000u)
        x = ~x; // If sign bit set, invert to count leading ones as zeros
    return static_cast<uint32_t>(std::countl_zero(x)) - 1;
}

#define PS2_BLENDV_PS(a, b, mask) _mm_blendv_ps((a), (b), (mask))
#define PS2_MIN_EPI32(a, b) _mm_min_epi32((a), (b))
#define PS2_MAX_EPI32(a, b) _mm_max_epi32((a), (b))
#define PS2_SHUFFLE_EPI8(v, mask) _mm_shuffle_epi8((v), (mask))

#define PS2_EXTRACT_EPI32(v, i) Ps2ExtractEpi32((v), (i))
#define PS2_EXTRACT_EPI64(v, i) Ps2ExtractEpi64((v), (i))

#define PS2_EXTRACT_EPI32_0(v) Ps2ExtractEpi32((v), 0)
#define PS2_EXTRACT_EPI32_1(v) Ps2ExtractEpi32((v), 1)
#define PS2_EXTRACT_EPI32_2(v) Ps2ExtractEpi32((v), 2)
#define PS2_EXTRACT_EPI32_3(v) Ps2ExtractEpi32((v), 3)

#define PS2_EXTRACT_EPI64_0(v) Ps2ExtractEpi64((v), 0)
#define PS2_EXTRACT_EPI64_1(v) Ps2ExtractEpi64((v), 1)

// Basic MIPS arithmetic operations
#define ADD32(a, b) ((uint32_t)((a) + (b)))
#define ADD32_OV(rs, rt, result32, overflow)              \
    do                                                    \
    {                                                     \
        int32_t _a = (int32_t)(rs);                       \
        int32_t _b = (int32_t)(rt);                       \
        int32_t _r = _a + _b;                             \
        overflow = (((_a ^ _b) >= 0) && ((_a ^ _r) < 0)); \
        result32 = (uint32_t)_r;                          \
    } while (0);
#define SUB32(a, b) ((uint32_t)((a) - (b)))
#define SUB32_OV(rs, rt, result32, overflow)             \
    do                                                   \
    {                                                    \
        int32_t _a = (int32_t)(rs);                      \
        int32_t _b = (int32_t)(rt);                      \
        int32_t _r = _a - _b;                            \
        overflow = (((_a ^ _b) < 0) && ((_a ^ _r) < 0)); \
        result32 = (uint32_t)_r;                         \
    } while (0);
#define MUL32(a, b) ((uint32_t)((a) * (b)))
#define DIV32(a, b) ((uint32_t)((a) / (b)))
#define AND32(a, b) ((uint32_t)((a) & (b)))
#define OR32(a, b) ((uint32_t)((a) | (b)))
#define XOR32(a, b) ((uint32_t)((a) ^ (b)))
#define NOR32(a, b) ((uint32_t)(~((a) | (b))))
#define SLL32(a, b) ((uint32_t)((a) << (b)))
#define SRL32(a, b) ((uint32_t)((a) >> (b)))
#define SRA32(a, b) ((uint32_t)((int32_t)(a) >> (b)))
#define SLT32(a, b) ((uint32_t)((int32_t)(a) < (int32_t)(b) ? 1 : 0))
#define SLTU32(a, b) ((uint32_t)((a) < (b) ? 1 : 0))

// PS2-specific 128-bit MMI operations
#define PS2_PEXTLW(a, b) _mm_unpacklo_epi32((__m128i)(b), (__m128i)(a))
#define PS2_PEXTUW(a, b) _mm_unpackhi_epi32((__m128i)(b), (__m128i)(a))
#define PS2_PEXTLH(a, b) _mm_unpacklo_epi16((__m128i)(b), (__m128i)(a))
#define PS2_PEXTUH(a, b) _mm_unpackhi_epi16((__m128i)(b), (__m128i)(a))
#define PS2_PEXTLB(a, b) _mm_unpacklo_epi8((__m128i)(b), (__m128i)(a))
#define PS2_PEXTUB(a, b) _mm_unpackhi_epi8((__m128i)(b), (__m128i)(a))
#define PS2_PADDW(a, b) _mm_add_epi32((__m128i)(a), (__m128i)(b))
#define PS2_PSUBW(a, b) _mm_sub_epi32((__m128i)(a), (__m128i)(b))
#define PS2_PMAXW(a, b) PS2_MAX_EPI32((__m128i)(a), (__m128i)(b))
#define PS2_PMINW(a, b) PS2_MIN_EPI32((__m128i)(a), (__m128i)(b))
#define PS2_PADDH(a, b) _mm_add_epi16((__m128i)(a), (__m128i)(b))
#define PS2_PSUBH(a, b) _mm_sub_epi16((__m128i)(a), (__m128i)(b))
#define PS2_PMAXH(a, b) _mm_max_epi16((__m128i)(a), (__m128i)(b))
#define PS2_PMINH(a, b) _mm_min_epi16((__m128i)(a), (__m128i)(b))
#define PS2_PADDB(a, b) _mm_add_epi8((__m128i)(a), (__m128i)(b))
#define PS2_PSUBB(a, b) _mm_sub_epi8((__m128i)(a), (__m128i)(b))
#define PS2_PAND(a, b) _mm_and_si128((__m128i)(a), (__m128i)(b))
#define PS2_POR(a, b) _mm_or_si128((__m128i)(a), (__m128i)(b))
#define PS2_PXOR(a, b) _mm_xor_si128((__m128i)(a), (__m128i)(b))
#define PS2_PNOR(a, b) _mm_xor_si128(_mm_or_si128((__m128i)(a), (__m128i)(b)), _mm_set1_epi32(0xFFFFFFFF))

// PS2 float semantics (EE FPU and VU0 macro mode). The PS2 format has no Inf, NaN or denormals:
//   - exponent 255 is an ordinary exponent, so 0x7F800000 is 2^128 and the largest value is 0x7FFFFFFF;
//   - exponent 0 is zero whatever the mantissa holds;
//   - a result of 2^129 or more saturates to +-0x7FFFFFFF, one below 2^-126 becomes +-0;
//   - x/0 gives +-0x7FFFFFFF.
// A host float cannot hold the exponent-255 values, so anything that touches them is computed in double.
// That path is rare and kept out of line; the inline part is the host operation and a range check.
// One case is left to the host: ADD.S/SUB.S/MUL.S and the vector operations do not look for an operand with
// exponent 0 and a non-zero mantissa, so it takes part with its tiny host value instead of as zero.
// Checked against the hardware results in ps2autotests (tests/cpu/ee_fpu).
#if defined(_MSC_VER)
#define PS2_FLOAT_NOINLINE __declspec(noinline)
#else
#define PS2_FLOAT_NOINLINE __attribute__((noinline))
#endif

constexpr uint32_t PS2_FLOAT_MAX_BITS = 0x7FFFFFFFu;
constexpr uint32_t PS2_FLOAT_EXP_MASK = 0x7F800000u;
constexpr uint32_t PS2_FLOAT_SIGN_MASK = 0x80000000u;

// True when a host float operation cannot be trusted with x: exponent 255, or exponent 0 with a mantissa.
inline bool ps2_fspecial(float x)
{
    const uint32_t t = std::bit_cast<uint32_t>(x) & 0x7FFFFFFFu;
    return t != 0u && (t - 0x00800000u) >= 0x7F000000u;
}

inline double ps2_to_double(float x)
{
    uint32_t b = std::bit_cast<uint32_t>(x);
    const uint32_t exponent = b & PS2_FLOAT_EXP_MASK;
    if (exponent == 0u)
        return static_cast<double>(std::bit_cast<float>(b & PS2_FLOAT_SIGN_MASK));
    if (exponent == PS2_FLOAT_EXP_MASK)
        return static_cast<double>(std::bit_cast<float>(b - 0x00800000u)) * 2.0;
    return static_cast<double>(x);
}

inline float ps2_from_double(double d)
{
    const double magnitude = std::fabs(d);
    const uint32_t sign = std::signbit(d) ? PS2_FLOAT_SIGN_MASK : 0u;
    if (magnitude >= 0x1p129)
        return std::bit_cast<float>(sign | PS2_FLOAT_MAX_BITS);
    if (magnitude < 0x1p-126)
        return std::bit_cast<float>(sign);
    if (magnitude >= 0x1p128)
    {
        // Convert one exponent lower, then put the exponent back as 255.
        const uint32_t b = std::bit_cast<uint32_t>(static_cast<float>(d * 0.5));
        if ((b & PS2_FLOAT_EXP_MASK) == PS2_FLOAT_EXP_MASK)
            return std::bit_cast<float>(sign | PS2_FLOAT_MAX_BITS);
        return std::bit_cast<float>(b + 0x00800000u);
    }
    // A value that rounds up to 2^128 comes out as the host's Inf pattern, which is 2^128 on the PS2.
    return static_cast<float>(d);
}

PS2_FLOAT_NOINLINE inline float ps2_fadd_slow(float a, float b)
{
    // Zero operands and results end up here too; the host result is right for them.
    const float r = a + b;
    if (!(ps2_fspecial(a) | ps2_fspecial(b) | ps2_fspecial(r)))
        return r;
    return ps2_from_double(ps2_to_double(a) + ps2_to_double(b));
}

PS2_FLOAT_NOINLINE inline float ps2_fsub_slow(float a, float b)
{
    // Zero operands and results end up here too; the host result is right for them.
    const float r = a - b;
    if (!(ps2_fspecial(a) | ps2_fspecial(b) | ps2_fspecial(r)))
        return r;
    return ps2_from_double(ps2_to_double(a) - ps2_to_double(b));
}

PS2_FLOAT_NOINLINE inline float ps2_fmul_slow(float a, float b)
{
    // Zero operands and results end up here too; the host result is right for them.
    const float r = a * b;
    if (!(ps2_fspecial(a) | ps2_fspecial(b) | ps2_fspecial(r)))
        return r;
    return ps2_from_double(ps2_to_double(a) * ps2_to_double(b));
}

// A zero divisor gives +-max with the sign of fs xor ft.
PS2_FLOAT_NOINLINE inline float ps2_fdiv_slow(float a, float b)
{
    const uint32_t ua = std::bit_cast<uint32_t>(a);
    const uint32_t ub = std::bit_cast<uint32_t>(b);
    if ((ub & PS2_FLOAT_EXP_MASK) == 0u)
        return std::bit_cast<float>(((ua ^ ub) & PS2_FLOAT_SIGN_MASK) | PS2_FLOAT_MAX_BITS);
    const float r = a / b;
    if (!(ps2_fspecial(a) | ps2_fspecial(b) | ps2_fspecial(r)))
        return r;
    return ps2_from_double(ps2_to_double(a) / ps2_to_double(b));
}

// sqrt(|ft|); the result is never negative, sqrt(-0) is +0.
PS2_FLOAT_NOINLINE inline float ps2_fsqrt_slow(float a)
{
    if ((std::bit_cast<uint32_t>(a) & PS2_FLOAT_EXP_MASK) == 0u)
        return 0.0f;
    return ps2_from_double(std::sqrt(std::fabs(ps2_to_double(a))));
}

// fs / sqrt(|ft|). The sign of ft is ignored everywhere, so a zero ft gives +-max with the sign of fs alone.
PS2_FLOAT_NOINLINE inline float ps2_frsqrt_slow(float a, float b)
{
    if ((std::bit_cast<uint32_t>(b) & PS2_FLOAT_EXP_MASK) == 0u)
        return std::bit_cast<float>((std::bit_cast<uint32_t>(a) & PS2_FLOAT_SIGN_MASK) | PS2_FLOAT_MAX_BITS);
    // The root is rounded to a float before the division, as on the hardware.
    const float root = ps2_from_double(std::sqrt(std::fabs(ps2_to_double(b))));
    return ps2_from_double(ps2_to_double(a) / static_cast<double>(root));
}

// True when a, b and the host result r are all normal numbers, so r is also the PS2 result.
inline bool ps2_fall_normal(float a, float b, float r)
{
    // Doubling drops the sign; adding one exponent step wraps exponent 255 round to 0 next to exponent 0.
    const uint32_t sa = std::bit_cast<uint32_t>(a) * 2u + 0x01000000u;
    const uint32_t sb = std::bit_cast<uint32_t>(b) * 2u + 0x01000000u;
    const uint32_t sr = std::bit_cast<uint32_t>(r) * 2u + 0x01000000u;
    uint32_t lowest = sa < sb ? sa : sb;
    lowest = lowest < sr ? lowest : sr;
    return lowest >= 0x02000000u;
}

// The same test on the result alone. An exponent-255 operand always leaves exponent 255 in the host result
// of an addition, subtraction or multiplication (Inf or NaN), so this finds those operands as well.
inline bool ps2_fnormal(float r)
{
    return (std::bit_cast<uint32_t>(r) * 2u + 0x01000000u) >= 0x02000000u;
}

inline float ps2_fadd(float a, float b)
{
    const float r = a + b;
    return ps2_fnormal(r) ? r : ps2_fadd_slow(a, b);
}

inline float ps2_fsub(float a, float b)
{
    const float r = a - b;
    return ps2_fnormal(r) ? r : ps2_fsub_slow(a, b);
}

inline float ps2_fmul(float a, float b)
{
    const float r = a * b;
    return ps2_fnormal(r) ? r : ps2_fmul_slow(a, b);
}

inline float ps2_fdiv(float a, float b)
{
    const float r = a / b;
    return ps2_fall_normal(a, b, r) ? r : ps2_fdiv_slow(a, b);
}

inline float ps2_fsqrt(float a)
{
    const float r = std::sqrt(a);
    return ps2_fall_normal(a, a, r) ? r : ps2_fsqrt_slow(a);
}

inline float ps2_frsqrt(float a, float b)
{
    const float r = a / std::sqrt(b);
    return ps2_fall_normal(a, b, r) ? r : ps2_frsqrt_slow(a, b);
}

enum class Ps2VecOp
{
    Add,
    Sub,
    Mul
};

// Lane by lane through the scalar helpers; only reached when a result lane has exponent 255.
PS2_FLOAT_NOINLINE inline __m128 ps2_vslow(__m128 a, __m128 b, Ps2VecOp op)
{
    alignas(16) float la[4], lb[4], lr[4];
    _mm_store_ps(la, a);
    _mm_store_ps(lb, b);
    for (int i = 0; i < 4; ++i)
    {
        switch (op)
        {
        case Ps2VecOp::Add:
            lr[i] = ps2_fadd_slow(la[i], lb[i]);
            break;
        case Ps2VecOp::Sub:
            lr[i] = ps2_fsub_slow(la[i], lb[i]);
            break;
        default:
            lr[i] = ps2_fmul_slow(la[i], lb[i]);
            break;
        }
    }
    return _mm_load_ps(lr);
}

// r is the host result of a op b. As in ps2_fnormal, testing r alone finds every lane that needs the double
// path. Results with exponent 0 are flushed to +-0.
inline __m128 ps2_vfinish(__m128 a, __m128 b, __m128 r, Ps2VecOp op)
{
    const __m128i expMask = _mm_set1_epi32(0x7F800000);
    const __m128i rexp = _mm_and_si128(_mm_castps_si128(r), expMask);
    if (_mm_movemask_epi8(_mm_cmpeq_epi32(rexp, expMask)) != 0)
        return ps2_vslow(a, b, op);
    const __m128i flush = _mm_and_si128(_mm_cmpeq_epi32(rexp, _mm_setzero_si128()), _mm_set1_epi32(0x7FFFFFFF));
    return _mm_castsi128_ps(_mm_andnot_si128(flush, _mm_castps_si128(r)));
}

inline __m128 ps2_vadd(__m128 a, __m128 b)
{
    return ps2_vfinish(a, b, _mm_add_ps(a, b), Ps2VecOp::Add);
}

inline __m128 ps2_vsub(__m128 a, __m128 b)
{
    return ps2_vfinish(a, b, _mm_sub_ps(a, b), Ps2VecOp::Sub);
}

inline __m128 ps2_vmul(__m128 a, __m128 b)
{
    return ps2_vfinish(a, b, _mm_mul_ps(a, b), Ps2VecOp::Mul);
}

// The PS2 has no Inf/NaN: exponent 255 is an ordinary (largest) exponent, so 0x7FFFFFFF is the most
// positive value and 0xFFFFFFFF the most negative. Flipping the magnitude bits of negative values
// turns the sign-magnitude encoding into an integer order that holds for every bit pattern.
inline int32_t ps2_fkey(float f)
{
    const uint32_t b = std::bit_cast<uint32_t>(f);
    return static_cast<int32_t>(b ^ (static_cast<uint32_t>(static_cast<int32_t>(b) >> 31) & 0x7FFFFFFFu));
}

inline __m128i ps2_vkey(__m128 v)
{
    const __m128i b = _mm_castps_si128(v);
    return _mm_xor_si128(b, _mm_and_si128(_mm_srai_epi32(b, 31), _mm_set1_epi32(0x7FFFFFFF)));
}

// VU MAX/MINI: the larger/smaller operand bit for bit, with no IEEE NaN rules.
inline __m128 ps2_vmax(__m128 a, __m128 b)
{
    const __m128i aGreater = _mm_cmpgt_epi32(ps2_vkey(a), ps2_vkey(b));
    return _mm_castsi128_ps(_mm_or_si128(_mm_and_si128(aGreater, _mm_castps_si128(a)),
                                         _mm_andnot_si128(aGreater, _mm_castps_si128(b))));
}

inline __m128 ps2_vmini(__m128 a, __m128 b)
{
    const __m128i aLess = _mm_cmplt_epi32(ps2_vkey(a), ps2_vkey(b));
    return _mm_castsi128_ps(_mm_or_si128(_mm_and_si128(aLess, _mm_castps_si128(a)),
                                         _mm_andnot_si128(aLess, _mm_castps_si128(b))));
}

inline float ps2_fmax(float a, float b)
{
    return ps2_fkey(a) > ps2_fkey(b) ? a : b;
}

inline float ps2_fmin(float a, float b)
{
    return ps2_fkey(a) < ps2_fkey(b) ? a : b;
}

// FPU compares. Values with exponent 255 are ordinary numbers on the PS2, so they are ordered by
// ps2_fkey; everything else keeps the host comparison (+0 == -0).
inline bool ps2_fexp255(float a, float b)
{
    return ((std::bit_cast<uint32_t>(a) & 0x7F800000u) == 0x7F800000u) ||
           ((std::bit_cast<uint32_t>(b) & 0x7F800000u) == 0x7F800000u);
}

inline bool ps2_fcmp_eq(float a, float b)
{
    return ps2_fexp255(a, b) ? std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b) : a == b;
}

inline bool ps2_fcmp_lt(float a, float b)
{
    return ps2_fexp255(a, b) ? ps2_fkey(a) < ps2_fkey(b) : a < b;
}

inline bool ps2_fcmp_le(float a, float b)
{
    return ps2_fexp255(a, b) ? ps2_fkey(a) <= ps2_fkey(b) : a <= b;
}

// MAC and status flags left by a COP2 macro FMAC instruction (ADD/SUB/MUL/MADD/MSUB in all forms,
// OPMULA/OPMSUB). `dest` is the VU destination mask (x = 8 ... w = 1); lanes not written report no
// flags. MAC: zero bits 0-3, sign 4-7, underflow 8-11, overflow 12-15, each with x in the top bit of
// its nibble. Status: Z/S/U/O in bits 0-3 and their sticky copies in bits 6-9; I/D (bits 4, 5, 10,
// 11) belong to the divider and are preserved. Denormal results count as zero plus underflow.
inline void ps2_vu0_fmac_flags(R5900Context *ctx, __m128 res, uint32_t dest)
{
    const __m128i bits = _mm_castps_si128(res);
    const __m128i expo = _mm_and_si128(bits, _mm_set1_epi32(0x7F800000));
    const __m128i expZero = _mm_cmpeq_epi32(expo, _mm_setzero_si128());
    const __m128i mantZero = _mm_cmpeq_epi32(_mm_and_si128(bits, _mm_set1_epi32(0x007FFFFF)), _mm_setzero_si128());
    const uint32_t zero = static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(expZero)));
    const uint32_t sign = static_cast<uint32_t>(_mm_movemask_ps(res));
    const uint32_t under = static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(_mm_andnot_si128(mantZero, expZero))));
    const uint32_t over = static_cast<uint32_t>(
        _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(expo, _mm_set1_epi32(0x7F800000)))));
    // movemask puts x in bit 0; MAC nibbles put x in bit 3.
    static constexpr uint8_t kReverse[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};
    dest &= 0xFu;
    const uint32_t z = kReverse[zero] & dest, s = kReverse[sign] & dest;
    const uint32_t u = kReverse[under] & dest, o = kReverse[over] & dest;
    ctx->vu0_mac_flags = z | (s << 4) | (u << 8) | (o << 12);
    const uint32_t now = (z ? 1u : 0u) | (s ? 2u : 0u) | (u ? 4u : 0u) | (o ? 8u : 0u);
    ctx->vu0_status = static_cast<uint16_t>((ctx->vu0_status & 0xFF0u) | now | (now << 6));
}

// FDIV unit (DIV/SQRT/RSQRT into Q). Each op replaces status I (bit 4, invalid) and D (bit 5, divide
// by zero) and ORs them into the sticky bits 10/11. The PS2 has no Inf/NaN: a zero divisor gives
// +-max, negative roots use the magnitude, and denormal operands count as zero.
inline bool ps2_fzero(float f)
{
    return (std::bit_cast<uint32_t>(f) & 0x7F800000u) == 0u;
}

inline float ps2_fsigned_max(bool negative)
{
    return std::bit_cast<float>(negative ? 0xFFFFFFFFu : 0x7FFFFFFFu);
}

// Exponent-255 PS2 values as the nearest host float, so host arithmetic never sees Inf/NaN.
inline float ps2_fhost(float f)
{
    const uint32_t b = std::bit_cast<uint32_t>(f);
    return (b & 0x7F800000u) == 0x7F800000u ? std::bit_cast<float>((b & 0x80000000u) | 0x7F7FFFFFu) : f;
}

// Host results that overflowed to Inf become the PS2's +-max.
inline float ps2_fresult(float f)
{
    const uint32_t b = std::bit_cast<uint32_t>(f);
    return (b & 0x7F800000u) == 0x7F800000u ? ps2_fsigned_max((b & 0x80000000u) != 0u) : f;
}

inline void ps2_vu0_fdiv_status(R5900Context *ctx, bool invalid, bool divZero)
{
    const uint32_t now = (invalid ? 0x10u : 0u) | (divZero ? 0x20u : 0u);
    ctx->vu0_status = static_cast<uint16_t>((ctx->vu0_status & ~0x30u) | now | (now << 6));
}

inline void ps2_vu0_div(R5900Context *ctx, float fs, float ft)
{
    if (ps2_fzero(ft))
    {
        // 0/0 is invalid, x/0 is divide by zero; both give +-max with the sign of fs ^ ft.
        ctx->vu0_q = ps2_fsigned_max(std::signbit(fs) != std::signbit(ft));
        ps2_vu0_fdiv_status(ctx, ps2_fzero(fs), !ps2_fzero(fs));
        return;
    }
    ctx->vu0_q = ps2_fresult(ps2_fhost(fs) / ps2_fhost(ft));
    ps2_vu0_fdiv_status(ctx, false, false);
}

inline void ps2_vu0_sqrt(R5900Context *ctx, float ft)
{
    ctx->vu0_q = ps2_fzero(ft) ? 0.0f : std::sqrt(std::fabs(ps2_fhost(ft)));
    ps2_vu0_fdiv_status(ctx, !ps2_fzero(ft) && std::signbit(ft), false);
}

inline void ps2_vu0_rsqrt(R5900Context *ctx, float fs, float ft)
{
    if (ps2_fzero(ft))
    {
        ctx->vu0_q = ps2_fsigned_max(std::signbit(fs) != std::signbit(ft));
        ps2_vu0_fdiv_status(ctx, ps2_fzero(fs), !ps2_fzero(fs));
        return;
    }
    ctx->vu0_q = ps2_fresult(ps2_fhost(fs) / std::sqrt(std::fabs(ps2_fhost(ft))));
    ps2_vu0_fdiv_status(ctx, std::signbit(ft), false);
}

// EE MMI multiply/divide. LO and HI are 128 bits on the EE: words 0-1 live in lo/hi (shared with
// MULT/DIV) and words 2-3 in lo1/hi1 (shared with MULT1/DIV1). The parallel ops write separate
// products into separate words, and PMFHL picks words or halfwords back out of both registers.
struct Ps2LoHi
{
    int32_t lo[4];
    int32_t hi[4];
};

inline Ps2LoHi ps2_lohi_get(const R5900Context *ctx)
{
    Ps2LoHi r;
    r.lo[0] = static_cast<int32_t>(ctx->lo);
    r.lo[1] = static_cast<int32_t>(ctx->lo >> 32);
    r.lo[2] = static_cast<int32_t>(ctx->lo1);
    r.lo[3] = static_cast<int32_t>(ctx->lo1 >> 32);
    r.hi[0] = static_cast<int32_t>(ctx->hi);
    r.hi[1] = static_cast<int32_t>(ctx->hi >> 32);
    r.hi[2] = static_cast<int32_t>(ctx->hi1);
    r.hi[3] = static_cast<int32_t>(ctx->hi1 >> 32);
    return r;
}

inline void ps2_lohi_set(R5900Context *ctx, const Ps2LoHi &r)
{
    auto pack = [](int32_t a, int32_t b)
    { return static_cast<uint64_t>(static_cast<uint32_t>(a)) | (static_cast<uint64_t>(static_cast<uint32_t>(b)) << 32); };
    ctx->lo = pack(r.lo[0], r.lo[1]);
    ctx->lo1 = pack(r.lo[2], r.lo[3]);
    ctx->hi = pack(r.hi[0], r.hi[1]);
    ctx->hi1 = pack(r.hi[2], r.hi[3]);
}

inline __m128i ps2_words_vec(int32_t w0, int32_t w1, int32_t w2, int32_t w3)
{
    return _mm_set_epi32(w3, w2, w1, w0);
}

// Signed 16x16 products of all eight halfword lanes.
inline void ps2_halfword_products(__m128i rs, __m128i rt, int32_t p[8])
{
    alignas(16) int16_t a[8], b[8];
    _mm_store_si128(reinterpret_cast<__m128i *>(a), rs);
    _mm_store_si128(reinterpret_cast<__m128i *>(b), rt);
    for (int i = 0; i < 8; ++i)
        p[i] = static_cast<int32_t>(a[i]) * static_cast<int32_t>(b[i]);
}

// PMULTH/PMADDH/PMSUBH: products 0,1,4,5 go to LO words 0-3 and 2,3,6,7 to HI words 0-3;
// rd receives LO0, HI0, LO2, HI2. sign: 0 = multiply, +1 = add, -1 = subtract.
inline __m128i ps2_pmulth_family(R5900Context *ctx, __m128i rs, __m128i rt, int sign)
{
    int32_t p[8];
    ps2_halfword_products(rs, rt, p);
    Ps2LoHi r = sign == 0 ? Ps2LoHi{} : ps2_lohi_get(ctx);
    static constexpr int kLo[4] = {0, 1, 4, 5}, kHi[4] = {2, 3, 6, 7};
    for (int w = 0; w < 4; ++w)
    {
        const int32_t pl = p[kLo[w]], ph = p[kHi[w]];
        r.lo[w] = sign < 0 ? r.lo[w] - pl : sign > 0 ? r.lo[w] + pl : pl;
        r.hi[w] = sign < 0 ? r.hi[w] - ph : sign > 0 ? r.hi[w] + ph : ph;
    }
    ps2_lohi_set(ctx, r);
    return ps2_words_vec(r.lo[0], r.hi[0], r.lo[2], r.hi[2]);
}

// PHMADH (sub = false) / PHMSBH (sub = true): pairwise sums (or odd - even differences) into
// LO0, HI0, LO2, HI2; rd receives the same four words.
inline __m128i ps2_phmadh(R5900Context *ctx, __m128i rs, __m128i rt, bool sub)
{
    int32_t p[8];
    ps2_halfword_products(rs, rt, p);
    Ps2LoHi r = ps2_lohi_get(ctx);
    auto pair = [&](int i)
    { return sub ? p[i + 1] - p[i] : p[i + 1] + p[i]; };
    r.lo[0] = pair(0);
    r.hi[0] = pair(2);
    r.lo[2] = pair(4);
    r.hi[2] = pair(6);
    ps2_lohi_set(ctx, r);
    return ps2_words_vec(r.lo[0], r.hi[0], r.lo[2], r.hi[2]);
}

// PMULTW/PMULTUW/PMADDW/PMADDUW/PMSUBW: 32x32 products of words 0 and 2. Each 64-bit result
// (accumulated with HI:LO of its lane) is split into LO and HI words, each sign-extended to 64
// bits; rd receives the two 64-bit results.
inline __m128i ps2_pmultw_family(R5900Context *ctx, __m128i rs, __m128i rt, bool isUnsigned, int sign)
{
    alignas(16) int32_t a[4], b[4];
    _mm_store_si128(reinterpret_cast<__m128i *>(a), rs);
    _mm_store_si128(reinterpret_cast<__m128i *>(b), rt);
    Ps2LoHi r = ps2_lohi_get(ctx);
    int64_t out[2];
    for (int lane = 0; lane < 2; ++lane)
    {
        const int w = lane * 2;
        const int64_t prod = isUnsigned
                                 ? static_cast<int64_t>(static_cast<uint64_t>(static_cast<uint32_t>(a[w])) *
                                                        static_cast<uint64_t>(static_cast<uint32_t>(b[w])))
                                 : static_cast<int64_t>(a[w]) * static_cast<int64_t>(b[w]);
        const int64_t acc = static_cast<int64_t>((static_cast<uint64_t>(static_cast<uint32_t>(r.hi[w])) << 32) |
                                                 static_cast<uint32_t>(r.lo[w]));
        const int64_t res = sign == 0 ? prod : sign > 0 ? acc + prod : acc - prod;
        r.lo[w] = static_cast<int32_t>(res);
        r.lo[w + 1] = r.lo[w] < 0 ? -1 : 0;
        r.hi[w] = static_cast<int32_t>(res >> 32);
        r.hi[w + 1] = r.hi[w] < 0 ? -1 : 0;
        out[lane] = res;
    }
    ps2_lohi_set(ctx, r);
    return _mm_set_epi64x(out[1], out[0]);
}

// PDIVW/PDIVUW: words 0 and 2 of rs divided by the same words of rt; quotient to LO, remainder
// to HI, each sign-extended to 64 bits. Division by zero leaves the EE's documented results.
inline void ps2_pdivw(R5900Context *ctx, __m128i rs, __m128i rt, bool isUnsigned)
{
    alignas(16) int32_t a[4], b[4];
    _mm_store_si128(reinterpret_cast<__m128i *>(a), rs);
    _mm_store_si128(reinterpret_cast<__m128i *>(b), rt);
    Ps2LoHi r = ps2_lohi_get(ctx);
    for (int w = 0; w < 4; w += 2)
    {
        int32_t q, m;
        if (isUnsigned)
        {
            const uint32_t ua = static_cast<uint32_t>(a[w]), ub = static_cast<uint32_t>(b[w]);
            q = ub ? static_cast<int32_t>(ua / ub) : -1;
            m = ub ? static_cast<int32_t>(ua % ub) : a[w];
        }
        else if (b[w] == 0)
        {
            q = a[w] < 0 ? 1 : -1;
            m = a[w];
        }
        else if (b[w] == -1 && a[w] == INT32_MIN)
        {
            q = INT32_MIN;
            m = 0;
        }
        else
        {
            q = a[w] / b[w];
            m = a[w] % b[w];
        }
        r.lo[w] = q;
        r.lo[w + 1] = q < 0 ? -1 : 0;
        r.hi[w] = m;
        r.hi[w + 1] = m < 0 ? -1 : 0;
    }
    ps2_lohi_set(ctx, r);
}

// PDIVBW: all four words of rs divided by halfword 0 of rt; quotients to LO words, remainders to
// HI words.
inline void ps2_pdivbw(R5900Context *ctx, __m128i rs, __m128i rt)
{
    alignas(16) int32_t a[4];
    _mm_store_si128(reinterpret_cast<__m128i *>(a), rs);
    const int32_t d = static_cast<int16_t>(_mm_cvtsi128_si32(rt));
    Ps2LoHi r;
    for (int w = 0; w < 4; ++w)
    {
        if (d == 0)
        {
            r.lo[w] = a[w] < 0 ? 1 : -1;
            r.hi[w] = a[w];
        }
        else if (d == -1 && a[w] == INT32_MIN)
        {
            r.lo[w] = INT32_MIN;
            r.hi[w] = 0;
        }
        else
        {
            r.lo[w] = a[w] / d;
            r.hi[w] = a[w] % d;
        }
    }
    ps2_lohi_set(ctx, r);
}

inline int32_t ps2_sat32(int64_t v)
{
    return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : static_cast<int32_t>(v);
}

inline int16_t ps2_sat16(int32_t v)
{
    return static_cast<int16_t>(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

// PMFHL: variant 0 LW, 1 UW, 2 SLW, 3 LH, 4 SH.
inline __m128i ps2_pmfhl(const R5900Context *ctx, int variant)
{
    const Ps2LoHi r = ps2_lohi_get(ctx);
    switch (variant)
    {
    case 0:
        return ps2_words_vec(r.lo[0], r.hi[0], r.lo[2], r.hi[2]);
    case 1:
        return ps2_words_vec(r.lo[1], r.hi[1], r.lo[3], r.hi[3]);
    case 2:
    {
        auto lane = [&](int w)
        {
            const int64_t v = static_cast<int64_t>((static_cast<uint64_t>(static_cast<uint32_t>(r.hi[w])) << 32) |
                                                   static_cast<uint32_t>(r.lo[w]));
            return static_cast<int64_t>(ps2_sat32(v));
        };
        return _mm_set_epi64x(lane(2), lane(0));
    }
    case 3:
        return _mm_setr_epi16(static_cast<int16_t>(r.lo[0]), static_cast<int16_t>(r.lo[1]),
                              static_cast<int16_t>(r.hi[0]), static_cast<int16_t>(r.hi[1]),
                              static_cast<int16_t>(r.lo[2]), static_cast<int16_t>(r.lo[3]),
                              static_cast<int16_t>(r.hi[2]), static_cast<int16_t>(r.hi[3]));
    default:
        return _mm_setr_epi16(ps2_sat16(r.lo[0]), ps2_sat16(r.lo[1]), ps2_sat16(r.hi[0]), ps2_sat16(r.hi[1]),
                              ps2_sat16(r.lo[2]), ps2_sat16(r.lo[3]), ps2_sat16(r.hi[2]), ps2_sat16(r.hi[3]));
    }
}

// PMTHL.LW: rs words 0-3 to LO0, HI0, LO2, HI2.
inline void ps2_pmthl_lw(R5900Context *ctx, __m128i rs)
{
    alignas(16) int32_t a[4];
    _mm_store_si128(reinterpret_cast<__m128i *>(a), rs);
    Ps2LoHi r = ps2_lohi_get(ctx);
    r.lo[0] = a[0];
    r.hi[0] = a[1];
    r.lo[2] = a[2];
    r.hi[2] = a[3];
    ps2_lohi_set(ctx, r);
}

// VFTOI: float to int32 with saturation (the host conversion returns 0x80000000 for every overflow).
inline __m128i ps2_vftoi(__m128 v)
{
    const __m128i res = _mm_cvttps_epi32(v);
    const __m128i overflow = _mm_cmpeq_epi32(res, _mm_set1_epi32(static_cast<int32_t>(0x80000000u)));
    const __m128i positive = _mm_cmpgt_epi32(_mm_castps_si128(v), _mm_set1_epi32(-1));
    return _mm_xor_si128(res, _mm_and_si128(_mm_and_si128(overflow, positive), _mm_set1_epi32(-1)));
}

// PS2 VU (Vector Unit) operations
#define PS2_VMAX(a, b) ps2_vmax((__m128)(a), (__m128)(b))
#define PS2_VMINI(a, b) ps2_vmini((__m128)(a), (__m128)(b))
#define PS2_VADD(a, b) ps2_vadd((__m128)(a), (__m128)(b))
#define PS2_VSUB(a, b) ps2_vsub((__m128)(a), (__m128)(b))
#define PS2_VMUL(a, b) ps2_vmul((__m128)(a), (__m128)(b))
#define PS2_VDIV(a, b) _mm_div_ps((__m128)(a), (__m128)(b))
#define PS2_VMULQ(a, q) _mm_mul_ps((__m128)(a), _mm_set1_ps(q))
#define PS2_VBLEND(a, b, mask) PS2_BLENDV_PS((__m128)(a), (__m128)(b), (__m128)(mask))

// Memory access helpers - Hybrid Fast/Slow Path
// Fast path: Direct RDRAM access (masked).
// Slow path: Full runtime->Load/Store

static inline bool Ps2FastRangeIsContiguous(uint32_t offset, uint32_t bytes)
{
    return offset <= (PS2_RAM_SIZE - bytes);
}

static inline uint8_t Ps2FastRead8(const uint8_t *rdram, uint32_t addr)
{
    return rdram[addr & PS2_RAM_MASK];
}

static inline uint16_t Ps2FastRead16(const uint8_t *rdram, uint32_t addr)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(uint16_t)))
    {
        uint8_t wrapped[sizeof(uint16_t)];
        for (uint32_t i = 0; i < sizeof(uint16_t); ++i)
        {
            wrapped[i] = rdram[(offset + i) & PS2_RAM_MASK];
        }
        uint16_t value;
        std::memcpy(&value, wrapped, sizeof(value));
        return value;
    }

    uint16_t value;
    std::memcpy(&value, rdram + offset, sizeof(value));
    return value;
}

static inline uint32_t Ps2FastRead32(const uint8_t *rdram, uint32_t addr)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(uint32_t)))
    {
        uint8_t wrapped[sizeof(uint32_t)];
        for (uint32_t i = 0; i < sizeof(uint32_t); ++i)
        {
            wrapped[i] = rdram[(offset + i) & PS2_RAM_MASK];
        }
        uint32_t value;
        std::memcpy(&value, wrapped, sizeof(value));
        return value;
    }

    uint32_t value;
    std::memcpy(&value, rdram + offset, sizeof(value));
    return value;
}

static inline uint64_t Ps2FastRead64(const uint8_t *rdram, uint32_t addr)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(uint64_t)))
    {
        uint8_t wrapped[sizeof(uint64_t)];
        for (uint32_t i = 0; i < sizeof(uint64_t); ++i)
        {
            wrapped[i] = rdram[(offset + i) & PS2_RAM_MASK];
        }
        uint64_t value;
        std::memcpy(&value, wrapped, sizeof(value));
        return value;
    }

    uint64_t value;
    std::memcpy(&value, rdram + offset, sizeof(value));
    return value;
}

static inline __m128i Ps2FastRead128(const uint8_t *rdram, uint32_t addr)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(__m128i)))
    {
        alignas(16) uint8_t wrapped[sizeof(__m128i)];
        for (uint32_t i = 0; i < sizeof(__m128i); ++i)
        {
            wrapped[i] = rdram[(offset + i) & PS2_RAM_MASK];
        }
        __m128i value;
        std::memcpy(&value, wrapped, sizeof(value));
        return value;
    }

    __m128i value;
    std::memcpy(&value, rdram + offset, sizeof(value));
    return value;
}

static inline void Ps2FastWrite8(uint8_t *rdram, uint32_t addr, uint8_t value)
{
    rdram[addr & PS2_RAM_MASK] = value;
}

static inline void Ps2FastWrite16(uint8_t *rdram, uint32_t addr, uint16_t value)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(uint16_t)))
    {
        uint8_t wrapped[sizeof(uint16_t)];
        std::memcpy(wrapped, &value, sizeof(value));
        for (uint32_t i = 0; i < sizeof(uint16_t); ++i)
        {
            rdram[(offset + i) & PS2_RAM_MASK] = wrapped[i];
        }
        return;
    }
    std::memcpy(rdram + offset, &value, sizeof(value));
}

static inline void Ps2FastWrite32(uint8_t *rdram, uint32_t addr, uint32_t value)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(uint32_t)))
    {
        uint8_t wrapped[sizeof(uint32_t)];
        std::memcpy(wrapped, &value, sizeof(value));
        for (uint32_t i = 0; i < sizeof(uint32_t); ++i)
        {
            rdram[(offset + i) & PS2_RAM_MASK] = wrapped[i];
        }
        return;
    }
    std::memcpy(rdram + offset, &value, sizeof(value));
}

static inline void Ps2FastWrite64(uint8_t *rdram, uint32_t addr, uint64_t value)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(uint64_t)))
    {
        uint8_t wrapped[sizeof(uint64_t)];
        std::memcpy(wrapped, &value, sizeof(value));
        for (uint32_t i = 0; i < sizeof(uint64_t); ++i)
        {
            rdram[(offset + i) & PS2_RAM_MASK] = wrapped[i];
        }
        return;
    }
    std::memcpy(rdram + offset, &value, sizeof(value));
}

static inline void Ps2FastWrite128(uint8_t *rdram, uint32_t addr, __m128i value)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(__m128i)))
    {
        alignas(16) uint8_t wrapped[sizeof(__m128i)];
        std::memcpy(wrapped, &value, sizeof(value));
        for (uint32_t i = 0; i < sizeof(__m128i); ++i)
        {
            rdram[(offset + i) & PS2_RAM_MASK] = wrapped[i];
        }
        return;
    }
    std::memcpy(rdram + offset, &value, sizeof(value));
}

#define FAST_READ8(addr) Ps2FastRead8(rdram, (uint32_t)(addr))
#define FAST_READ16(addr) Ps2FastRead16(rdram, (uint32_t)(addr))
#define FAST_READ32(addr) Ps2FastRead32(rdram, (uint32_t)(addr))
#define FAST_READ64(addr) Ps2FastRead64(rdram, (uint32_t)(addr))
#define FAST_READ128(addr) Ps2FastRead128(rdram, (uint32_t)(addr))

#define FAST_WRITE8(addr, val) Ps2FastWrite8(rdram, (uint32_t)(addr), (uint8_t)(val))
#define FAST_WRITE16(addr, val) Ps2FastWrite16(rdram, (uint32_t)(addr), (uint16_t)(val))
#define FAST_WRITE32(addr, val) Ps2FastWrite32(rdram, (uint32_t)(addr), (uint32_t)(val))
#define FAST_WRITE64(addr, val) Ps2FastWrite64(rdram, (uint32_t)(addr), (uint64_t)(val))
#define FAST_WRITE128(addr, val) Ps2FastWrite128(rdram, (uint32_t)(addr), (val))

#define READ8(addr) ([&]() -> uint8_t {                       \
    uint32_t _addr = (uint32_t)(addr);                        \
    return PS2Runtime::isSpecialAddress(_addr)                \
        ? runtime->Load8(rdram, ctx, _addr)                   \
        : FAST_READ8(_addr); }())

#define READ16(addr) ([&]() -> uint16_t {                     \
    uint32_t _addr = (uint32_t)(addr);                        \
    return PS2Runtime::isSpecialAddress(_addr)                \
        ? runtime->Load16(rdram, ctx, _addr)                  \
        : FAST_READ16(_addr); }())

#define READ32(addr) ([&]() -> uint32_t {                     \
    uint32_t _addr = (uint32_t)(addr);                        \
    return PS2Runtime::isSpecialAddress(_addr)                \
        ? runtime->Load32(rdram, ctx, _addr)                  \
        : FAST_READ32(_addr); }())

#define READ64(addr) ([&]() -> uint64_t {                     \
    uint32_t _addr = (uint32_t)(addr);                        \
    return PS2Runtime::isSpecialAddress(_addr)                \
        ? runtime->Load64(rdram, ctx, _addr)                  \
        : FAST_READ64(_addr); }())

#define READ128(addr) ([&]() -> __m128i {                     \
    uint32_t _addr = (uint32_t)(addr);                        \
    return PS2Runtime::isSpecialAddress(_addr)                \
        ? runtime->Load128(rdram, ctx, _addr)                 \
        : FAST_READ128(_addr); }())

#define WRITE8(addr, val)                                                            \
    do                                                                               \
    {                                                                                \
        uint32_t _addr = (addr);                                                     \
        if (PS2Runtime::isSpecialAddress(_addr))                                     \
            runtime->Store8(rdram, ctx, _addr, (val));                               \
        else                                                                         \
        {                                                                            \
            ps2TraceGuestWrite(rdram, _addr, 1u, (uint8_t)(val), 0u, "WRITE8", ctx); \
            FAST_WRITE8(_addr, (val));                                               \
        }                                                                            \
    } while (0)

#define WRITE16(addr, val)                                                             \
    do                                                                                 \
    {                                                                                  \
        uint32_t _addr = (addr);                                                       \
        if (PS2Runtime::isSpecialAddress(_addr))                                       \
            runtime->Store16(rdram, ctx, _addr, (val));                                \
        else                                                                           \
        {                                                                              \
            ps2TraceGuestWrite(rdram, _addr, 2u, (uint16_t)(val), 0u, "WRITE16", ctx); \
            FAST_WRITE16(_addr, (val));                                                \
        }                                                                              \
    } while (0)

#define WRITE32(addr, val)                                                             \
    do                                                                                 \
    {                                                                                  \
        uint32_t _addr = (addr);                                                       \
        if (PS2Runtime::isSpecialAddress(_addr))                                       \
            runtime->Store32(rdram, ctx, _addr, (val));                                \
        else                                                                           \
        {                                                                              \
            ps2TraceGuestWrite(rdram, _addr, 4u, (uint32_t)(val), 0u, "WRITE32", ctx); \
            FAST_WRITE32(_addr, (val));                                                \
        }                                                                              \
    } while (0)

#define WRITE64(addr, val)                                                             \
    do                                                                                 \
    {                                                                                  \
        uint32_t _addr = (addr);                                                       \
        if (PS2Runtime::isSpecialAddress(_addr))                                       \
            runtime->Store64(rdram, ctx, _addr, (val));                                \
        else                                                                           \
        {                                                                              \
            ps2TraceGuestWrite(rdram, _addr, 8u, (uint64_t)(val), 0u, "WRITE64", ctx); \
            FAST_WRITE64(_addr, (val));                                                \
        }                                                                              \
    } while (0)

#define WRITE128(addr, val)                                                          \
    do                                                                               \
    {                                                                                \
        uint32_t _addr = (addr);                                                     \
        __m128i _value = (val);                                                      \
        if (PS2Runtime::isSpecialAddress(_addr))                                     \
            runtime->Store128(rdram, ctx, _addr, _value);                            \
        else                                                                         \
        {                                                                            \
            const uint64_t _lo = static_cast<uint64_t>(PS2_EXTRACT_EPI64_0(_value)); \
            const uint64_t _hi = static_cast<uint64_t>(PS2_EXTRACT_EPI64_1(_value)); \
            ps2TraceGuestWrite(rdram, _addr, 16u, _lo, _hi, "WRITE128", ctx);        \
            FAST_WRITE128(_addr, _value);                                            \
        }                                                                            \
    } while (0)

// Packed Compare Greater Than (PCGT)
#define PS2_PCGTW(a, b) _mm_cmpgt_epi32((__m128i)(a), (__m128i)(b))
#define PS2_PCGTH(a, b) _mm_cmpgt_epi16((__m128i)(a), (__m128i)(b))
#define PS2_PCGTB(a, b) _mm_cmpgt_epi8((__m128i)(a), (__m128i)(b))

// Packed Add with Signed Saturation Word (PADDSW)
inline __m128i ps2_paddsw(__m128i a, __m128i b)
{

    __m128i sum = _mm_add_epi32(a, b);
    // Check for over/underflow. Clamp to either INT32_MIN/INT32_MAX.
    __m128i overflow = _mm_and_si128(_mm_xor_si128(a, sum),
                                     _mm_xor_si128(b, sum));
    // Extract input sign.
    overflow = _mm_srai_epi32(overflow, 31);
    __m128i input_sign = _mm_srai_epi32(a, 31);
    // Select saturation value based on overflow sign.
    #if defined(__SSE4_1__)
    __m128i sat = _mm_blendv_epi8(
        _mm_set1_epi32(INT32_MAX),
        _mm_set1_epi32(INT32_MIN),
        input_sign);
    return _mm_blendv_epi8(sum, sat, overflow);
    #else
    __m128i sat = _mm_or_si128(_mm_and_si128(input_sign, _mm_set1_epi32(INT32_MIN)),
                               _mm_andnot_si128(input_sign, _mm_set1_epi32(INT32_MAX)));
    return _mm_or_si128(_mm_and_si128(overflow, sat),
                        _mm_andnot_si128(overflow, sum));
    #endif
}
#define PS2_PADDSW(a, b) ps2_paddsw((__m128i)(a), (__m128i)(b))

// Packed Subtract with Signed Saturation Word (PSUBSW)
inline __m128i ps2_psubsw(__m128i a, __m128i b)
{
    __m128i diff = _mm_sub_epi32(a, b);
    // Check for over/underflow. Clamp to either INT32_MIN/INT32_MAX.
    __m128i overflow = _mm_and_si128(_mm_xor_si128(a, b),
                                     _mm_xor_si128(a, diff));
    // Extract input sign.
    overflow = _mm_srai_epi32(overflow, 31);
    __m128i input_sign = _mm_srai_epi32(a, 31);
    // Select saturation value based on overflow sign.
    #if defined(__SSE4_1__)
    __m128i sat = _mm_blendv_epi8(
        _mm_set1_epi32(INT32_MAX),
        _mm_set1_epi32(INT32_MIN),
        input_sign);
    return _mm_blendv_epi8(diff, sat, overflow);
    #else
    __m128i sat = _mm_or_si128(_mm_and_si128(input_sign, _mm_set1_epi32(INT32_MIN)),
                               _mm_andnot_si128(input_sign, _mm_set1_epi32(INT32_MAX)));
    return _mm_or_si128(_mm_and_si128(overflow, sat),
                        _mm_andnot_si128(overflow, diff));
    #endif

}
#define PS2_PSUBSW(a, b) ps2_psubsw((__m128i)(a), (__m128i)(b))

// Packed Compare Equal (PCEQ)
#define PS2_PCEQW(a, b) _mm_cmpeq_epi32((__m128i)(a), (__m128i)(b))
#define PS2_PCEQH(a, b) _mm_cmpeq_epi16((__m128i)(a), (__m128i)(b))
#define PS2_PCEQB(a, b) _mm_cmpeq_epi8((__m128i)(a), (__m128i)(b))

// Packed Absolute (PABS)
// PABSW/PABSH saturate: the absolute value of the most negative number is the most positive one.
#define PS2_PABSW(a) _mm_min_epu32(_mm_abs_epi32((__m128i)(a)), _mm_set1_epi32(0x7FFFFFFF))
#define PS2_PABSH(a) _mm_min_epu16(_mm_abs_epi16((__m128i)(a)), _mm_set1_epi16(0x7FFF))
#define PS2_PABSB(a) _mm_abs_epi8((__m128i)(a))

// Packed Pack (PPAC) - Packs larger elements into smaller ones
inline __m128i ps2_paddu32(__m128i a, __m128i b)
{
    __m128i sum = _mm_add_epi32(a, b);
    __m128i overflow = _mm_cmpgt_epi32(_mm_xor_si128(a, _mm_set1_epi32(INT32_MIN)),
                                       _mm_xor_si128(sum, _mm_set1_epi32(INT32_MIN)));
    return _mm_or_si128(sum, overflow); // overflow lanes become all-1s
}
inline __m128i ps2_psubu32(__m128i a, __m128i b)
{
    __m128i diff = _mm_sub_epi32(a, b);
    // Underflow if a < b (unsigned). Clamp to 0.
    __m128i underflow = _mm_cmpgt_epi32(_mm_xor_si128(b, _mm_set1_epi32(INT32_MIN)),
                                        _mm_xor_si128(a, _mm_set1_epi32(INT32_MIN)));
    return _mm_andnot_si128(underflow, diff); // underflow lanes become 0
}

inline __m128i ps2_ppacw(__m128i rs, __m128i rt)
{
    // rs = [rs3 rs2 rs1 rs0], rt = [rt3 rt2 rt1 rt0]
    return _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(rt), _mm_castsi128_ps(rs), _MM_SHUFFLE(2, 0, 2, 0)));
}
#define PS2_PPACW(a, b) ps2_ppacw((__m128i)(a), (__m128i)(b))

inline __m128i ps2_ppach(__m128i rs, __m128i rt)
{
    const __m128i mask = _mm_setr_epi8(
        0, 1, 4, 5, 8, 9, 12, 13,  // from rt: halfwords 0,2,4,6
        0, 1, 4, 5, 8, 9, 12, 13); // from rs: halfwords 0,2,4,6
    __m128i lo = _mm_shuffle_epi8(rt, mask);
    __m128i hi = _mm_shuffle_epi8(rs, mask);
    return _mm_unpacklo_epi64(lo, hi);
}
#define PS2_PPACH(a, b) ps2_ppach((__m128i)(a), (__m128i)(b))

inline __m128i ps2_ppacb(__m128i rs, __m128i rt)
{
    const __m128i mask = _mm_setr_epi8(
        0, 2, 4, 6, 8, 10, 12, 14,  // from rt: bytes 0,2,4,6,8,10,12,14
        0, 2, 4, 6, 8, 10, 12, 14); // from rs
    __m128i lo = _mm_shuffle_epi8(rt, mask);
    __m128i hi = _mm_shuffle_epi8(rs, mask);
    return _mm_unpacklo_epi64(lo, hi);
}
#define PS2_PPACB(a, b) ps2_ppacb((__m128i)(a), (__m128i)(b))

// Packed Interleave (PINT)
// PINTH rd, rs, rt: rt's lower four halfwords interleaved with rs's upper four
// (rd = rt.h0, rs.h4, rt.h1, rs.h5, ...).
#define PS2_PINTH(a, b) _mm_unpacklo_epi16((__m128i)(b), _mm_srli_si128((__m128i)(a), 8))
// PINTEH rd, rs, rt: the even halfwords of rt and rs interleaved (rd.h2i = rt.h2i, rd.h2i+1 = rs.h2i).
#define PS2_PINTEH(a, b) _mm_or_si128(_mm_and_si128((__m128i)(b), _mm_set1_epi32(0xFFFF)), _mm_slli_epi32((__m128i)(a), 16))

// Packed Multiply-Add (PMADD)
#define PS2_PMADDW(a, b) _mm_add_epi32(_mm_mullo_epi32(_mm_shuffle_epi32((__m128i)(a), _MM_SHUFFLE(1, 0, 3, 2)), _mm_shuffle_epi32((__m128i)(b), _MM_SHUFFLE(1, 0, 3, 2))), _mm_mullo_epi32(_mm_shuffle_epi32((__m128i)(a), _MM_SHUFFLE(3, 2, 1, 0)), _mm_shuffle_epi32((__m128i)(b), _MM_SHUFFLE(3, 2, 1, 0))))

// Packed Variable Shifts
// PSLLVW/PSRLVW/PSRAVW rd, rt, rs: words 0 and 2 of rt shifted by the low 5 bits of the same words
// of rs; each 32-bit result is sign-extended into its 64-bit half. Call sites pass (rs, rt).
// kind: 0 = left logical, 1 = right logical, 2 = right arithmetic.
inline __m128i ps2_pshiftvw(__m128i rs, __m128i rt, int kind)
{
    alignas(16) int32_t v[4], c[4];
    _mm_store_si128(reinterpret_cast<__m128i *>(v), rt);
    _mm_store_si128(reinterpret_cast<__m128i *>(c), rs);
    int64_t out[2];
    for (int lane = 0; lane < 2; ++lane)
    {
        const int w = lane * 2;
        const uint32_t s = static_cast<uint32_t>(c[w]) & 0x1Fu;
        int32_t r;
        if (kind == 0)
            r = static_cast<int32_t>(static_cast<uint32_t>(v[w]) << s);
        else if (kind == 1)
            r = static_cast<int32_t>(static_cast<uint32_t>(v[w]) >> s);
        else
            r = v[w] >> s;
        out[lane] = r;
    }
    return _mm_set_epi64x(out[1], out[0]);
}
#define PS2_PSLLVW(rs, rt) ps2_pshiftvw((__m128i)(rs), (__m128i)(rt), 0)
#define PS2_PSRLVW(rs, rt) ps2_pshiftvw((__m128i)(rs), (__m128i)(rt), 1)
#define PS2_PSRAVW(rs, rt) ps2_pshiftvw((__m128i)(rs), (__m128i)(rt), 2)

inline __m128i _mm_custom_sllv_epi32(__m128i a, __m128i count)
{
    alignas(16) int32_t a_arr[4];
    alignas(16) int32_t count_arr[4];
    alignas(16) int32_t result[4];

    std::memcpy(a_arr, &a, sizeof(a));
    std::memcpy(count_arr, &count, sizeof(count));

    for (int i = 0; i < 4; i++)
    {
        result[i] = a_arr[i] << (count_arr[i] & 0x1F);
    }

    __m128i out;
    std::memcpy(&out, result, sizeof(out));
    return out;
}

inline __m128i _mm_custom_srlv_epi32(__m128i a, __m128i count)
{
    int32_t a_arr[4], count_arr[4], result[4];
    _mm_storeu_si128((__m128i *)a_arr, a);
    _mm_storeu_si128((__m128i *)count_arr, count);
    for (int i = 0; i < 4; i++)
    {
        result[i] = (uint32_t)a_arr[i] >> (count_arr[i] & 0x1F);
    }
    return _mm_loadu_si128((__m128i *)result);
}

inline __m128i _mm_custom_srav_epi32(__m128i a, __m128i count)
{
    int32_t a_arr[4], count_arr[4], result[4];
    _mm_storeu_si128((__m128i *)a_arr, a);
    _mm_storeu_si128((__m128i *)count_arr, count);
    for (int i = 0; i < 4; i++)
    {
        result[i] = a_arr[i] >> (count_arr[i] & 0x1F);
    }
    return _mm_loadu_si128((__m128i *)result);
}

// PMFHL function implementations
inline __m128i ps2_u64_to_epi64_pair(uint64_t value)
{
    return _mm_set1_epi64x(static_cast<long long>(value));
}

#define PS2_PMFHL_LW(hi, lo) _mm_unpacklo_epi64(ps2_u64_to_epi64_pair(lo), ps2_u64_to_epi64_pair(hi))
#define PS2_PMFHL_UW(hi, lo) _mm_unpackhi_epi64(ps2_u64_to_epi64_pair(lo), ps2_u64_to_epi64_pair(hi))
#define PS2_PMFHL_SLW(hi, lo) _mm_packs_epi32(ps2_u64_to_epi64_pair(lo), ps2_u64_to_epi64_pair(hi))
#define PS2_PMFHL_LH(hi, lo) _mm_shuffle_epi32(_mm_packs_epi32(ps2_u64_to_epi64_pair(lo), ps2_u64_to_epi64_pair(hi)), _MM_SHUFFLE(3, 1, 2, 0))
#define PS2_PMFHL_SH(hi, lo) _mm_shufflehi_epi16(_mm_shufflelo_epi16(_mm_packs_epi32(ps2_u64_to_epi64_pair(lo), ps2_u64_to_epi64_pair(hi)), _MM_SHUFFLE(3, 1, 2, 0)), _MM_SHUFFLE(3, 1, 2, 0))

// FPU (COP1) operations
#define FPU_SET_ACC(ctx, res) (ctx->f_acc = res)
#define FPU_ADD_S(a, b) ps2_fadd((float)(a), (float)(b))
#define FPU_SUB_S(a, b) ps2_fsub((float)(a), (float)(b))
#define FPU_MUL_S(a, b) ps2_fmul((float)(a), (float)(b))
#define FPU_DIV_S(a, b) ps2_fdiv((float)(a), (float)(b))
#define FPU_RSQRT_S(a, b) ps2_frsqrt((float)(a), (float)(b))
#define FPU_SQRT_S(a) ps2_fsqrt((float)(a))
#define FPU_ABS_S(a) fabsf((float)(a))
#define FPU_MOV_S(a) ((float)(a))
#define FPU_NEG_S(a) (-(float)(a))
#define FPU_ROUND_L_S(a) ((int64_t)roundf((float)(a)))
#define FPU_TRUNC_L_S(a) ((int64_t)(float)(a))
#define FPU_CEIL_L_S(a) ((int64_t)ceilf((float)(a)))
#define FPU_FLOOR_L_S(a) ((int64_t)floorf((float)(a)))
#define FPU_ROUND_W_S(a) ((int32_t)nearbyintf((float)(a)))
#define FPU_TRUNC_W_S(a) ((int32_t)(float)(a))
#define FPU_CEIL_W_S(a) ((int32_t)ceilf((float)(a)))
#define FPU_FLOOR_W_S(a) ((int32_t)floorf((float)(a)))
#define FPU_CVT_S_W(a) ((float)(int32_t)(a))
#define FPU_CVT_S_L(a) ((float)(int64_t)(a))
// R5900 CVT.W.S always truncates toward zero (it ignores the FCR31 rounding mode) and
// saturates out-of-range inputs; the EE FPU has no NaN/Inf, so exponent-255 values
// saturate by sign. Rounding to nearest breaks loops like gap = (int)(gap / 1.3f).
inline int32_t ps2_fpu_cvt_w_s(float value)
{
    if (value >= 2147483648.0f || (std::isnan(value) && !std::signbit(value)))
        return INT32_MAX;
    if (value <= -2147483648.0f || std::isnan(value))
        return INT32_MIN;
    return static_cast<int32_t>(value);
}
#define FPU_CVT_W_S(a) ps2_fpu_cvt_w_s((float)(a))
#define FPU_CVT_L_S(a) ((int64_t)(float)(a))
// The EE FPU has no unordered results (no NaN), so the U*/NG* forms equal their ordered ones.
#define FPU_C_F_S(a, b) (0)
#define FPU_C_UN_S(a, b) (0)
#define FPU_C_EQ_S(a, b) ps2_fcmp_eq((float)(a), (float)(b))
#define FPU_C_UEQ_S(a, b) ps2_fcmp_eq((float)(a), (float)(b))
#define FPU_C_OLT_S(a, b) ps2_fcmp_lt((float)(a), (float)(b))
#define FPU_C_ULT_S(a, b) ps2_fcmp_lt((float)(a), (float)(b))
#define FPU_C_OLE_S(a, b) ps2_fcmp_le((float)(a), (float)(b))
#define FPU_C_ULE_S(a, b) ps2_fcmp_le((float)(a), (float)(b))
#define FPU_C_SF_S(a, b) (0)
#define FPU_C_NGLE_S(a, b) (0)
#define FPU_C_SEQ_S(a, b) ps2_fcmp_eq((float)(a), (float)(b))
#define FPU_C_NGL_S(a, b) ps2_fcmp_eq((float)(a), (float)(b))
#define FPU_C_LT_S(a, b) ps2_fcmp_lt((float)(a), (float)(b))
#define FPU_C_NGE_S(a, b) ps2_fcmp_lt((float)(a), (float)(b))
#define FPU_C_LE_S(a, b) ps2_fcmp_le((float)(a), (float)(b))
#define FPU_C_NGT_S(a, b) ps2_fcmp_le((float)(a), (float)(b))

// QFSRV: Quadword Funnel Shift Right Variable
// Concatenates rs || rt (256 bits) and right-shifts by SA bits, taking lower 128 bits.
inline __m128i ps2_qfsrv(__m128i rs, __m128i rt, uint32_t sa)
{
    if (sa == 0)
        return rt;
    if (sa >= 128)
    {
        if (sa >= 256)
            return _mm_setzero_si128();
        uint32_t shift = sa - 128;
        if (shift == 0)
            return rs;
        // Shift rs right by (sa-128) bits
        uint32_t byteShift = shift / 8;
        uint32_t bitShift = shift % 8;
        // Byte shift rs right
        alignas(16) uint8_t buf[16] = {};
        alignas(16) uint8_t src[16];
        _mm_store_si128((__m128i *)src, rs);
        for (uint32_t i = 0; i + byteShift < 16; i++)
            buf[i] = src[i + byteShift];
        __m128i result = _mm_load_si128((__m128i *)buf);
        if (bitShift > 0)
            result = _mm_or_si128(_mm_srli_epi64(result, bitShift),
                                  _mm_slli_epi64(_mm_bsrli_si128(result, 8), 64 - bitShift));
        return result;
    }
    // sa is 1..127: result = (rs || rt) >> sa, lower 128 bits
    uint32_t byteShift = sa / 8;
    uint32_t bitShift = sa % 8;
    alignas(16) uint8_t combined[32];
    _mm_store_si128((__m128i *)(combined), rt);      // low 128 bits
    _mm_store_si128((__m128i *)(combined + 16), rs); // high 128 bits
    // Shift right by byteShift bytes
    alignas(16) uint8_t shifted[16];
    for (uint32_t i = 0; i < 16; i++)
        shifted[i] = (i + byteShift < 32) ? combined[i + byteShift] : 0;
    __m128i result = _mm_load_si128((__m128i *)shifted);
    if (bitShift > 0)
    {
        uint8_t extra = (byteShift + 16 < 32) ? combined[byteShift + 16] : 0;
        __m128i hi_byte = _mm_insert_epi8(_mm_setzero_si128(), extra, 15);
        alignas(16) uint8_t src32[32];
        for (uint32_t i = 0; i < 32; i++)
            src32[i] = combined[i];
        uint64_t lo0, lo1, hi0, hi1;
        std::memcpy(&lo0, src32, 8);
        std::memcpy(&lo1, src32 + 8, 8);
        std::memcpy(&hi0, src32 + 16, 8);
        std::memcpy(&hi1, src32 + 24, 8);
        // 256-bit right shift by sa bits
        uint64_t r0, r1;
        if (sa < 64)
        {
            r0 = (lo0 >> sa) | (lo1 << (64 - sa));
            r1 = (lo1 >> sa) | (hi0 << (64 - sa));
        }
        else if (sa < 128)
        {
            uint32_t s = sa - 64;
            if (s == 0)
            {
                r0 = lo1;
                r1 = hi0;
            }
            else
            {
                r0 = (lo1 >> s) | (hi0 << (64 - s));
                r1 = (hi0 >> s) | (hi1 << (64 - s));
            }
        }
        else
        {
            r0 = 0;
            r1 = 0; // handled above
        }
        result = _mm_set_epi64x((long long)r1, (long long)r0);
    }
    return result;
}
#define PS2_QFSRV(rs, rt, sa) ps2_qfsrv((__m128i)(rs), (__m128i)(rt), (uint32_t)(sa))
#define PS2_PCPYLD(rs, rt) _mm_unpacklo_epi64(rt, rs)
// PEXEH swaps halfwords 0<->2 and 4<->6; PEXEW swaps words 0<->2; PROT3W rotates words 0-2
// (rd = rt.w1, rt.w2, rt.w0, rt.w3).
#define PS2_PEXEH(rs) _mm_shufflelo_epi16(_mm_shufflehi_epi16(rs, _MM_SHUFFLE(3, 0, 1, 2)), _MM_SHUFFLE(3, 0, 1, 2))
#define PS2_PEXEW(rs) _mm_shuffle_epi32(rs, _MM_SHUFFLE(3, 0, 1, 2))
#define PS2_PROT3W(rs) _mm_shuffle_epi32(rs, _MM_SHUFFLE(3, 0, 2, 1))

// Additional VU0 operations
#define PS2_VSQRT(x) sqrtf(x)
#define PS2_VRSQRT(x) (1.0f / sqrtf(x))

#define GPR_U32(ctx_ptr, reg_idx) ((reg_idx == 0) ? 0U : static_cast<uint32_t>(PS2_EXTRACT_EPI32_0(ctx_ptr->r[reg_idx])))
#define GPR_S32(ctx_ptr, reg_idx) ((reg_idx == 0) ? 0 : PS2_EXTRACT_EPI32_0(ctx_ptr->r[reg_idx]))
#define GPR_U64(ctx_ptr, reg_idx) ((reg_idx == 0) ? 0ULL : static_cast<uint64_t>(PS2_EXTRACT_EPI64_0(ctx_ptr->r[reg_idx])))
#define GPR_S64(ctx_ptr, reg_idx) ((reg_idx == 0) ? 0LL : PS2_EXTRACT_EPI64_0(ctx_ptr->r[reg_idx]))
#define GPR_VEC(ctx_ptr, reg_idx) ((reg_idx == 0) ? _mm_setzero_si128() : ctx_ptr->r[reg_idx])

static inline void Ps2SetGprLow64(R5900Context *ctx, int reg, __m128i new_low)
{
    if (reg != 0)
    {
        ctx->r[reg] = _mm_castpd_si128(_mm_move_sd(_mm_castsi128_pd(ctx->r[reg]), _mm_castsi128_pd(new_low)));
    }
}

#define SET_GPR_U32(ctx_ptr, reg_idx, val)                                \
    do                                                                    \
    {                                                                     \
        if ((reg_idx) != 0)                                               \
        {                                                                 \
            __m128i _newVal = _mm_cvtsi64_si128((int64_t)(int32_t)(val)); \
                                                                          \
            Ps2SetGprLow64(ctx_ptr, reg_idx, _newVal);                    \
        }                                                                 \
    } while (0)


#define SET_GPR_ZE32(ctx_ptr, reg_idx, val)                               \
    do                                                                    \
    {                                                                     \
        if ((reg_idx) != 0)                                               \
        {                                                                 \
            __m128i _newVal = _mm_cvtsi64_si128((int64_t)(uint32_t)(val)); \
            Ps2SetGprLow64(ctx_ptr, reg_idx, _newVal);                    \
        }                                                                 \
    } while (0)

#define SET_GPR_S32(ctx_ptr, reg_idx, val)                                \
    do                                                                    \
    {                                                                     \
        if ((reg_idx) != 0)                                               \
        {                                                                 \
            __m128i _newVal = _mm_cvtsi64_si128((int64_t)(int32_t)(val)); \
            Ps2SetGprLow64(ctx_ptr, reg_idx, _newVal);                    \
        }                                                                 \
    } while (0)

#define SET_GPR_U64(ctx_ptr, reg_idx, val)                       \
    do                                                           \
    {                                                            \
        if ((reg_idx) != 0)                                      \
        {                                                        \
            __m128i _newVal = _mm_cvtsi64_si128((int64_t)(val)); \
            Ps2SetGprLow64(ctx_ptr, reg_idx, _newVal);           \
        }                                                        \
    } while (0)

#define SET_GPR_S64(ctx_ptr, reg_idx, val) SET_GPR_U64(ctx_ptr, reg_idx, val)

#define SET_GPR_VEC(ctx_ptr, reg_idx, val) \
    do                                     \
    {                                      \
        if (reg_idx != 0)                  \
            ctx_ptr->r[reg_idx] = (val);   \
    } while (0)

#endif // PS2_RUNTIME_MACROS_H
