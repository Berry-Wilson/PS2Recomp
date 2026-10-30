#ifndef PS2_VU1_NATIVE_H
#define PS2_VU1_NATIVE_H

// Support code for VU0/VU1 microprograms translated ahead of time to C++ (tools/vu1_recomp.py).
//
// Translated blocks reproduce VU1Interpreter results bit for bit: operands and results are
// normalized the same way, FMAC values and MAC flags come from the same float/double
// computations, and the cycle count is tracked at runtime with the interpreter's stall rules
// so that latency-dependent reads (MAC/clip flags, Q, P, XGKICK packet reads) see the same
// values. VF/VI/ACC writes are applied immediately: every reader stalls until a write is
// ready, so delayed writeback is only observable through the stall itself.

#include "runtime/ps2_vu1.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <emmintrin.h>

#if defined(_MSC_VER)
#define VU1N_INLINE __forceinline
#else
#define VU1N_INLINE inline __attribute__((always_inline))
#endif

namespace vu1n
{
    // Sizes are VU1's; VU0 code and data memories are 4KB and wrap at 0x1000.
    constexpr uint32_t kDataSize = 0x4000u;
    constexpr uint32_t kPcMask = 0x3FFFu;

    enum class Unit : uint8_t
    {
        VU0,
        VU1
    };
    // Block return values other than a pc.
    constexpr uint32_t kEnd = 0xFFFFFFFFu;      // E bit + delay slot executed; Ctx::endPc holds the resume pc
    constexpr uint32_t kBailFlag = 0x80000000u; // untranslatable here: interpreter continues at (ret & kPcMask)

    using KickSink = void (*)(void *user, const uint8_t *packet, uint32_t bytes);

    struct alignas(16) Ctx
    {
        float vf[32][4];
        float acc[4];
        int32_t vi[16];
        float q;
        float p;
        float i;

        uint8_t *mem;
        uint32_t top;
        uint32_t cyc;
        uint32_t endPc;

        uint32_t vfReady[32][4];
        uint32_t viReady[16];

        // MAC / clip flag pipelines: ring of the last four issues; older values folded into *Base.
        uint32_t macCyc[4], macVal[4], macHead, macCount, macBase;
        uint32_t clipCyc[4], clipVal[4], clipHead, clipCount, clipBase, workingClip;

        bool qPending;
        uint32_t qReady;
        float qVal;

        bool pPending[2];
        uint32_t pReady[2];
        float pVal[2];
        uint32_t efuReady;

        // XGKICK: the packet is captured at issue and patched by stores that the
        // interpreter's two-cycles-per-qword PATH1 transfer would still have observed.
        bool kickActive;
        uint32_t kickIssue;
        uint32_t kickSrc;
        uint32_t kickBytes;
        uint32_t kickDone;
        KickSink kickSink;
        void *kickUser;

        // Branches read the pre-write value of a VI written by the previous pair's IALU op.
        uint32_t bkReg;
        int32_t bkVal;

        alignas(16) uint8_t kickBuf[0x10000];
    };

    using BlockFn = uint32_t (*)(Ctx &);

    struct BlockDesc
    {
        uint32_t pc;
        uint32_t pairs;
        const uint32_t *words; // expected micro memory contents (lower, upper per pair)
        BlockFn fn;
    };

    void registerBlocks(Unit unit, const BlockDesc *blocks, size_t count);

    struct Registrar
    {
        Registrar(Unit unit, const BlockDesc *blocks, size_t count) { registerBlocks(unit, blocks, count); }
    };

    enum class RunResult
    {
        NotTranslated, // nothing executed
        Done,
        Bailed // state written back; VU1State::pc is where the interpreter must resume
    };

    // Runs translated code starting at startPc against the given micro/data memory. maxCycles is
    // the interpreter's cycle budget; it is only checked between blocks, so a program that runs
    // past it is logged and stopped at the next block start (the interpreter stops mid-block).
    RunResult run(Unit unit, VU1State &state, const uint8_t *code, uint64_t codeGeneration, uint8_t *data,
                  uint32_t startPc, uint32_t top, uint32_t itop, uint32_t maxCycles, KickSink sink, void *sinkUser);

    // ---------------------------------------------------------------------
    // Helpers used by generated code
    // ---------------------------------------------------------------------

    constexpr uint8_t kRev4[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};

    VU1N_INLINE void stall(uint32_t &r, uint32_t t)
    {
        if (t > r)
            r = t;
    }

    VU1N_INLINE float norm1(float v)
    {
        uint32_t b;
        std::memcpy(&b, &v, 4);
        const uint32_t e = b & 0x7F800000u;
        if (e == 0u)
            b &= 0x80000000u;
        else if (e == 0x7F800000u)
            b = (b & 0x80000000u) | 0x7F7FFFFFu;
        std::memcpy(&v, &b, 4);
        return v;
    }

    VU1N_INLINE __m128 norm(__m128 v)
    {
        const __m128i b = _mm_castps_si128(v);
        const __m128i expMask = _mm_set1_epi32(0x7F800000);
        const __m128i e = _mm_and_si128(b, expMask);
        const __m128i zeroExp = _mm_cmpeq_epi32(e, _mm_setzero_si128());
        const __m128i maxExp = _mm_cmpeq_epi32(e, expMask);
        const __m128i sign = _mm_and_si128(b, _mm_set1_epi32(static_cast<int>(0x80000000u)));
        __m128i r = _mm_or_si128(_mm_andnot_si128(zeroExp, b), _mm_and_si128(zeroExp, sign));
        r = _mm_or_si128(_mm_andnot_si128(maxExp, r),
                         _mm_and_si128(maxExp, _mm_or_si128(sign, _mm_set1_epi32(0x7F7FFFFF))));
        return _mm_castsi128_ps(r);
    }

    VU1N_INLINE __m128 ldv(const Ctx &c, int reg) { return _mm_load_ps(c.vf[reg]); }
    VU1N_INLINE __m128 ldacc(const Ctx &c) { return _mm_load_ps(c.acc); }

    template <int Lane>
    VU1N_INLINE __m128 bcast(__m128 v)
    {
        return _mm_shuffle_ps(v, v, _MM_SHUFFLE(Lane, Lane, Lane, Lane));
    }

    // dest is in VU order (x = 8, y = 4, z = 2, w = 1).
    template <int Dest>
    VU1N_INLINE __m128 destMask()
    {
        return _mm_castsi128_ps(_mm_set_epi32((Dest & 1) ? -1 : 0, (Dest & 2) ? -1 : 0,
                                              (Dest & 4) ? -1 : 0, (Dest & 8) ? -1 : 0));
    }

    template <int Dest>
    VU1N_INLINE void stv(float *dst, __m128 v)
    {
        if constexpr (Dest == 0xF)
            _mm_store_ps(dst, v);
        else
        {
            const __m128 m = destMask<Dest>();
            _mm_store_ps(dst, _mm_or_ps(_mm_and_ps(m, v), _mm_andnot_ps(m, _mm_load_ps(dst))));
        }
    }

    // --- flag pipelines -------------------------------------------------------

    VU1N_INLINE void pushMac(Ctx &c, uint32_t mac)
    {
        c.macHead = (c.macHead + 1u) & 3u;
        if (c.macCount == 4u)
            c.macBase = c.macVal[c.macHead];
        else
            ++c.macCount;
        c.macCyc[c.macHead] = c.cyc;
        c.macVal[c.macHead] = mac;
    }

    VU1N_INLINE uint32_t readMac(const Ctx &c)
    {
        for (uint32_t k = 0; k < c.macCount; ++k)
        {
            const uint32_t idx = (c.macHead - k) & 3u;
            if (c.macCyc[idx] + 4u <= c.cyc)
                return c.macVal[idx];
        }
        return c.macBase;
    }

    VU1N_INLINE void pushClip(Ctx &c, uint32_t clip)
    {
        c.clipHead = (c.clipHead + 1u) & 3u;
        if (c.clipCount == 4u)
            c.clipBase = c.clipVal[c.clipHead];
        else
            ++c.clipCount;
        c.clipCyc[c.clipHead] = c.cyc;
        c.clipVal[c.clipHead] = clip;
    }

    VU1N_INLINE uint32_t readClip(const Ctx &c)
    {
        for (uint32_t k = 0; k < c.clipCount; ++k)
        {
            const uint32_t idx = (c.clipHead - k) & 3u;
            if (c.clipCyc[idx] + 4u <= c.cyc)
                return c.clipVal[idx];
        }
        return c.clipBase;
    }

    // --- FMAC ---------------------------------------------------------------

    enum FmacOp
    {
        kAdd,
        kSub,
        kMul,
        kMadd,
        kMsub
    };

    // a, b, acc must already be normalized. Returns the result for all lanes (only Dest
    // lanes are meaningful) and pushes the MAC flags the interpreter would compute.
    template <int Op, int Dest>
    VU1N_INLINE __m128 fmac(Ctx &c, __m128 a, __m128 b, __m128 acc)
    {
        const __m128d alo = _mm_cvtps_pd(a), ahi = _mm_cvtps_pd(_mm_movehl_ps(a, a));
        const __m128d blo = _mm_cvtps_pd(b), bhi = _mm_cvtps_pd(_mm_movehl_ps(b, b));
        __m128 r;
        __m128d elo, ehi;
        if constexpr (Op == kAdd)
        {
            r = _mm_add_ps(a, b);
            elo = _mm_add_pd(alo, blo);
            ehi = _mm_add_pd(ahi, bhi);
        }
        else if constexpr (Op == kSub)
        {
            r = _mm_sub_ps(a, b);
            elo = _mm_sub_pd(alo, blo);
            ehi = _mm_sub_pd(ahi, bhi);
        }
        else if constexpr (Op == kMul)
        {
            r = _mm_mul_ps(a, b);
            elo = _mm_mul_pd(alo, blo);
            ehi = _mm_mul_pd(ahi, bhi);
        }
        else
        {
            const __m128d clo = _mm_cvtps_pd(acc), chi = _mm_cvtps_pd(_mm_movehl_ps(acc, acc));
            if constexpr (Op == kMadd)
            {
                r = _mm_add_ps(acc, _mm_mul_ps(a, b));
                elo = _mm_add_pd(clo, _mm_mul_pd(alo, blo));
                ehi = _mm_add_pd(chi, _mm_mul_pd(ahi, bhi));
            }
            else
            {
                r = _mm_sub_ps(acc, _mm_mul_ps(a, b));
                elo = _mm_sub_pd(clo, _mm_mul_pd(alo, blo));
                ehi = _mm_sub_pd(chi, _mm_mul_pd(ahi, bhi));
            }
        }

        constexpr int DS = kRev4[Dest]; // SSE lane order (bit0 = x)
        const __m128d absMask = _mm_castsi128_pd(_mm_set1_epi64x(0x7FFFFFFFFFFFFFFFll));
        const __m128d mlo = _mm_and_pd(elo, absMask), mhi = _mm_and_pd(ehi, absMask);
        const __m128d fmax = _mm_set1_pd(3.4028234663852886e+38);
        const __m128d fmin = _mm_set1_pd(1.1754943508222875e-38);
        const __m128d zero = _mm_setzero_pd();
        const int zeroM = (_mm_movemask_pd(_mm_cmpeq_pd(mlo, zero)) | (_mm_movemask_pd(_mm_cmpeq_pd(mhi, zero)) << 2)) & DS;
        const int overM = (_mm_movemask_pd(_mm_cmpgt_pd(mlo, fmax)) | (_mm_movemask_pd(_mm_cmpgt_pd(mhi, fmax)) << 2)) & DS;
        const int underM = (_mm_movemask_pd(_mm_cmplt_pd(mlo, fmin)) | (_mm_movemask_pd(_mm_cmplt_pd(mhi, fmin)) << 2)) & DS & ~zeroM;
        const int negM = (_mm_movemask_pd(elo) | (_mm_movemask_pd(ehi) << 2)) & DS;

        if (zeroM | overM | underM)
        {
            alignas(16) uint32_t bits[4];
            _mm_store_ps(reinterpret_cast<float *>(bits), r);
            for (int lane = 0; lane < 4; ++lane)
            {
                const uint32_t sign = (negM >> lane) & 1 ? 0x80000000u : 0u;
                if ((overM >> lane) & 1)
                    bits[lane] = sign | 0x7F7FFFFFu;
                else if (((zeroM | underM) >> lane) & 1)
                    bits[lane] = sign;
            }
            r = _mm_load_ps(reinterpret_cast<const float *>(bits));
        }

        if constexpr (Dest != 0)
        {
            const uint32_t mac = static_cast<uint32_t>(kRev4[zeroM | underM]) |
                                 (static_cast<uint32_t>(kRev4[negM]) << 4) |
                                 (static_cast<uint32_t>(kRev4[underM]) << 8) |
                                 (static_cast<uint32_t>(kRev4[overM]) << 12);
            pushMac(c, mac);
        }
        return r;
    }

    template <int Scale>
    VU1N_INLINE __m128 itof(__m128 raw)
    {
        const __m128 f = _mm_cvtepi32_ps(_mm_castps_si128(raw));
        if constexpr (Scale == 0)
            return f;
        else
            return _mm_mul_ps(f, _mm_set1_ps(1.0f / static_cast<float>(1 << Scale)));
    }

    template <int Scale>
    VU1N_INLINE __m128 ftoi(__m128 v) // v normalized
    {
        __m128 s = v;
        if constexpr (Scale != 0)
            s = _mm_mul_ps(v, _mm_set1_ps(static_cast<float>(1 << Scale)));
        const __m128i t = _mm_cvttps_epi32(s);
        const __m128 posOverflow = _mm_cmpge_ps(s, _mm_set1_ps(2147483648.0f));
        const __m128i fixed = _mm_or_si128(_mm_andnot_si128(_mm_castps_si128(posOverflow), t),
                                           _mm_and_si128(_mm_castps_si128(posOverflow), _mm_set1_epi32(0x7FFFFFFF)));
        return _mm_castsi128_ps(fixed);
    }

    VU1N_INLINE __m128 vabs(__m128 v)
    {
        return _mm_and_ps(v, _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF)));
    }

    VU1N_INLINE void clip(Ctx &c, int fs, int ft)
    {
        uint32_t wBits;
        std::memcpy(&wBits, &c.vf[ft][3], 4);
        const int32_t limit = (wBits & 0x7F800000u) != 0u ? static_cast<int32_t>(wBits & 0x7FFFFFFFu) : 0x007FFFFF;
        uint32_t flags = 0u;
        for (int lane = 0; lane < 3; ++lane)
        {
            uint32_t bits;
            std::memcpy(&bits, &c.vf[fs][lane], 4);
            if (static_cast<int32_t>(bits) > limit)
                flags |= 1u << (lane * 2);
            if (static_cast<int32_t>(bits ^ 0x80000000u) > limit)
                flags |= 2u << (lane * 2);
        }
        c.workingClip = ((c.workingClip << 6) | flags) & 0xFFFFFFu;
        pushClip(c, c.workingClip);
    }

    // --- Q / P ---------------------------------------------------------------

    VU1N_INLINE float readQ(Ctx &c)
    {
        if (c.qPending && c.qReady <= c.cyc)
        {
            c.q = c.qVal;
            c.qPending = false;
        }
        return c.q;
    }

    VU1N_INLINE void queueQ(Ctx &c, float value, uint32_t latency)
    {
        readQ(c);
        c.qPending = true;
        c.qReady = c.cyc + latency;
        c.qVal = norm1(value);
    }

    VU1N_INLINE void vdiv(Ctx &c, float num, float den)
    {
        num = norm1(num);
        den = norm1(den);
        float result;
        if (den == 0.0f)
            result = std::signbit(num) != std::signbit(den) ? -3.40282347e+38f : 3.40282347e+38f;
        else
            result = num / den;
        queueQ(c, result, 7u);
    }

    VU1N_INLINE void vsqrt(Ctx &c, float value)
    {
        queueQ(c, std::sqrt(std::fabs(norm1(value))), 7u);
    }

    VU1N_INLINE void vrsqrt(Ctx &c, float num, float radicand)
    {
        num = norm1(num);
        const float den = std::sqrt(std::fabs(norm1(radicand)));
        float result;
        if (den == 0.0f)
            result = std::signbit(num) ? -3.40282347e+38f : 3.40282347e+38f;
        else
            result = num / den;
        queueQ(c, result, 13u);
    }

    VU1N_INLINE void commitP(Ctx &c)
    {
        // Entries become ready in issue order (EFU throughput keeps them apart).
        for (;;)
        {
            int best = -1;
            for (int k = 0; k < 2; ++k)
                if (c.pPending[k] && c.pReady[k] <= c.cyc && (best < 0 || c.pReady[k] < c.pReady[best]))
                    best = k;
            if (best < 0)
                return;
            c.p = c.pVal[best];
            c.pPending[best] = false;
        }
    }

    VU1N_INLINE float readP(Ctx &c)
    {
        commitP(c);
        return c.p;
    }

    VU1N_INLINE bool queueP(Ctx &c, float value, uint32_t latency)
    {
        commitP(c);
        for (int k = 0; k < 2; ++k)
        {
            if (!c.pPending[k])
            {
                c.pPending[k] = true;
                c.pReady[k] = c.cyc + latency;
                c.pVal[k] = norm1(value);
                c.efuReady = c.cyc + latency - 1u;
                return true;
            }
        }
        return false;
    }

    VU1N_INLINE float esin(float value)
    {
        constexpr float coefficients[] = {
            1.0f,
            -0.166666567325592f,
            0.008333025500178f,
            -0.000198074136279f,
            0.000002601886990f};
        const float squared = value * value;
        float polynomial = coefficients[4];
        for (int index = 3; index >= 0; --index)
            polynomial = coefficients[index] + squared * polynomial;
        return value * polynomial;
    }

    // --- memory -------------------------------------------------------------

    template <uint32_t Size = kDataSize>
    VU1N_INLINE uint32_t qaddr(int32_t base, int32_t imm)
    {
        return (static_cast<uint32_t>(base + imm) * 16u) & (Size - 1u);
    }

    void kickStorePatch(Ctx &c, uint32_t addr, const uint32_t *words, int sseLaneMask);

    template <int Dest>
    VU1N_INLINE void sq(Ctx &c, uint32_t addr, __m128 v)
    {
        stv<Dest>(reinterpret_cast<float *>(c.mem + addr), v);
        if (c.kickActive)
        {
            alignas(16) uint32_t w[4];
            _mm_store_ps(reinterpret_cast<float *>(w), v);
            kickStorePatch(c, addr, w, kRev4[Dest]);
        }
    }

    template <int Dest>
    VU1N_INLINE void isw(Ctx &c, uint32_t addr, int32_t vi)
    {
        const uint32_t val = static_cast<uint16_t>(vi);
        const uint32_t w[4] = {val, val, val, val};
        uint32_t *dst = reinterpret_cast<uint32_t *>(c.mem + addr);
        for (int lane = 0; lane < 4; ++lane)
            if ((kRev4[Dest] >> lane) & 1)
                dst[lane] = val;
        if (c.kickActive)
            kickStorePatch(c, addr, w, kRev4[Dest]);
    }

    template <int Dest>
    VU1N_INLINE int32_t ilw(const Ctx &c, uint32_t addr)
    {
        constexpr int comp = (Dest & 8) ? 0 : (Dest & 4) ? 1 : (Dest & 2) ? 2 : 3;
        uint32_t v;
        std::memcpy(&v, c.mem + addr + comp * 4, 4);
        return static_cast<int16_t>(v & 0xFFFFu);
    }

    VU1N_INLINE __m128 lq(const Ctx &c, uint32_t addr)
    {
        return _mm_load_ps(reinterpret_cast<const float *>(c.mem + addr));
    }

    void kickSubmit(Ctx &c);
    void xgkick(Ctx &c, int32_t vi);

    VU1N_INLINE int32_t s16(int32_t v) { return static_cast<int16_t>(v); }

    VU1N_INLINE uint32_t bits(float v)
    {
        uint32_t b;
        std::memcpy(&b, &v, 4);
        return b;
    }

    VU1N_INLINE float fromBits(uint32_t b)
    {
        float v;
        std::memcpy(&v, &b, 4);
        return v;
    }

    VU1N_INLINE int32_t branchVi(const Ctx &c, uint32_t reg)
    {
        return c.bkReg == reg ? c.bkVal : c.vi[reg];
    }
}

#endif
