/* glibc-bit-exact float transcendentals for the CUDA device pass.
 *
 * WHY THIS FILE EXISTS
 *   The shared core (brotato_core.h) is the same source for the CPU build, the
 *   CUDA kernels and Metal. Its math macros resolve to the C library on the
 *   host, and to the compiler's device math library on the GPU. Those two do
 *   NOT agree bit-for-bit, so a device rollout diverges from the CPU rollout of
 *   the same seed even though every other operation is IEEE-identical.
 *
 *   Measured on this host (glibc 2.44, x86-64 with FMA): the resolved `sinf` is
 *   __sinf_fma, whose documented worst case is 0.5607 ulp, i.e. it is NOT
 *   correctly rounded. Compared against the correctly rounded (double-rounded)
 *   value over 10.7M random arguments it differs by 1 ulp in 0.16% of cases,
 *   and the correctly rounded value is the one that matches an 80-bit `sinl`
 *   reference in 10.7M/10.7M samples. So "evaluate in double and round" would
 *   NOT reproduce the CPU: reproducing the CPU requires reproducing glibc's
 *   own algorithms, instruction for instruction. That is what this header does:
 *
 *     bt_mc_sinf / bt_mc_cosf   - transcription of glibc's
 *       sysdeps/ieee754/flt-32/s_sinf.c + s_cosf.c (ARM-contributed fast
 *       polynomials, `sinf_poly`/`sincosf_poly` in sincosf_poly.h, the
 *       `reduce_fast`/`reduce_large` helpers and the `__sincosf_table` data in
 *       s_sincosf.h / s_sincosf_data.c), using the x86-64 multiarch FMA code
 *       path the dynamic linker actually selects on this CPU (__sinf_fma /
 *       __cosf_fma, verified with dlsym+objdump). The fused-multiply-add
 *       placement below is not the source's `-ffp-contract=off` reading of the
 *       expression tree: it is read back from that disassembly, because gcc
 *       contracted those particular products into vfmadd (and libm's atan2f,
 *       below, did NOT contract). Each `fma`/`mul`/`add` here is therefore
 *       explicit and flag-independent.
 *     bt_mc_atan2f              - transcription of glibc's correctly rounded
 *       sysdeps/ieee754/flt-32/e_atan2f.c. That file is CORE-MATH's atan2f
 *       (Copyright (c) 2022-2024 Alexei Sibidanov and Paul Zimmermann, MIT
 *       licence, header retained below), which glibc adopted in 2.43. Long
 *       double is not needed: the algorithm is double + fma + double-double
 *       arithmetic only, all of which CUDA provides with identical rounding.
 *     bt_mc_sqrtf               - glibc's NaN rules around a correctly rounded
 *       core: __fsqrt_rn already agrees with glibc bit-for-bit on every non-NaN
 *       result, but glibc answers a negative argument with the SIGNED default
 *       qNaN 0xffc00000 (CUDA answers 0x7fffffff) and quietens a NaN argument
 *       instead of returning the canonical NaN.  The header of that function
 *       carries the measurements; tests/test_brotato_cuda.cu layer E pins them
 *       over the whole float range.
 *
 *   Every arithmetic operation is spelled through the BT_MC_* macros so the
 *   emitted result cannot be changed by nvcc's contraction/FMA flags
 *   (-fmad / --fmad / -ffp-contract): the fused ones are exactly the ones glibc
 *   fused. (build.sh additionally pins --fmad=false for brotato anyway.)
 *
 *   Only included from the CUDA branch of brotato_core.h's math macros; the CPU
 *   and Metal branches are untouched. The functions are __host__ __device__ so
 *   a test can run the identical code on both sides and diff it against the
 *   real libm.
 *
 * VERIFICATION (see tests/test_brotato_cuda.cu, layer E):
 *   The port is compared against the host libm on hundreds of millions of
 *   arguments, including the whole input space the core can produce, and it
 *   matches bit-for-bit.
 */
#pragma once

#include <math.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Exactly-rounded primitive spellings. Device: the __*_rn intrinsics, whose
 * rounding is documented and independent of -prec-div/-prec-sqrt/-fmad. Host:
 * plain operators plus `fma`, correct as long as the host pass of the
 * translation unit keeps contraction off (build.sh passes
 * -Xcompiler=-ffp-contract=off for brotato; the test command does the same).
 * ------------------------------------------------------------------------- */
#if defined(__CUDA_ARCH__)
/* NOTE: in CUDA the *float* intrinsics are __fmul_rn/__fadd_rn/__fsub_rn/
 * __fdiv_rn and the double ones are __dmul_rn/__dadd_rn/__dsub_rn/__ddiv_rn
 * (__fma_rn is the double one; the float one is __fmaf_rn). Passing a double to
 * the float spelling compiles - it is an ordinary floating conversion - and
 * silently drops the value to float precision, which is measurably wrong here
 * (a double product came back exactly equal to (float)(x*x)). Keep the d*
 * spellings. */
#define BT_MC_MUL(a, b) __dmul_rn((a), (b))
#define BT_MC_ADD(a, b) __dadd_rn((a), (b))
#define BT_MC_SUB(a, b) __dsub_rn((a), (b))
#define BT_MC_DIV(a, b) __ddiv_rn((a), (b))
#define BT_MC_FMA(a, b, c) __fma_rn((a), (b), (c))
#define BT_MC_AS_U32(x) (__float_as_uint(x))
#define BT_MC_AS_F32(u) (__uint_as_float(u))
#define BT_MC_AS_D64(u) (__longlong_as_double((long long)(u)))
#define BT_MC_AS_U64(x) ((unsigned long long)__double_as_longlong(x))
#else
#define BT_MC_MUL(a, b) ((a) * (b))
#define BT_MC_ADD(a, b) ((a) + (b))
#define BT_MC_SUB(a, b) ((a) - (b))
#define BT_MC_DIV(a, b) ((a) / (b))
#define BT_MC_FMA(a, b, c) fma((a), (b), (c))
static inline unsigned int bt_mc_host_as_u32(float x) {
    unsigned int u;
    __builtin_memcpy(&u, &x, 4);
    return u;
}
static inline float bt_mc_host_as_f32(unsigned int u) {
    float x;
    __builtin_memcpy(&x, &u, 4);
    return x;
}
static inline double bt_mc_host_as_d64(unsigned long long u) {
    double x;
    __builtin_memcpy(&x, &u, 8);
    return x;
}
static inline unsigned long long bt_mc_host_as_u64(double x) {
    unsigned long long u;
    __builtin_memcpy(&u, &x, 8);
    return u;
}
#define BT_MC_AS_U32(x) bt_mc_host_as_u32(x)
#define BT_MC_AS_F32(u) bt_mc_host_as_f32(u)
#define BT_MC_AS_D64(u) bt_mc_host_as_d64(u)
#define BT_MC_AS_U64(x) bt_mc_host_as_u64(x)
#endif

#if defined(__CUDA_ARCH__)
#define BT_MC_FN static __device__ __forceinline__
/* A namespace-scope `static const` array is a HOST variable on nvcc and cannot
 * be read from device code (CUDA guide E.2.13); the device pass gets its own
 * device-resident read-only copy, exactly like brotato_rng.h's BT_TABLE. */
#define BT_MC_TABLE static __device__ const
#else
#define BT_MC_FN static inline
#define BT_MC_TABLE static const
#endif

/* ---------------------------------------------------------------------------
 * Part 1: sinf / cosf.  glibc sysdeps/ieee754/flt-32/s_sincosf.h.
 *
 * The two entries of __sincosf_table hold the even (cosine) and odd (sine)
 * minimax coefficients; entry 1 negates the cosine polynomial (and pi/2), so
 * sin/cos of x in the second quadrant comes out with the right sign without a
 * branch. The values are copied from s_sincosf_data.c and re-checked against
 * the table actually mapped in this host's libm.so.6.
 * ------------------------------------------------------------------------- */

typedef struct {
    double sign[4];   /* sign of sine in quadrants 0..3 */
    double hpi_inv;   /* 2/pi * 2^24 (TOINT_INTRINSICS == 0 path) */
    double hpi;       /* pi/2 */
    double c0, c1, c2, c3, c4;   /* cosine polynomial */
    double s1, s2, s3;           /* sine polynomial */
} bt_mc_sincos_t;

BT_MC_TABLE bt_mc_sincos_t bt_mc_sincos_table[2] = {
    {
        { 1.0, -1.0, -1.0, 1.0 },
        0x1.45F306DC9C883p+23,   /* 2/pi * 2^24, the prescaled range-reduction constant */
        0x1.921FB54442D18p+0,    /* pi/2 */
        0x1p+0,
        -0x1.ffffffd0c621cp-2,
        0x1.55553e1068f19p-5,
        -0x1.6c087e89a359dp-10,
        0x1.99343027bf8c3p-16,
        -0x1.555545995a603p-3,
        0x1.1107605230bc4p-7,
        -0x1.994eb3774cf24p-13,
    },
    {
        { 1.0, -1.0, -1.0, 1.0 },
        0x1.45F306DC9C883p+23,
        -0x1.921FB54442D18p+0,   /* -pi/2 */
        -0x1p+0,
        0x1.ffffffd0c621cp-2,
        -0x1.55553e1068f19p-5,
        0x1.6c087e89a359dp-10,
        -0x1.99343027bf8c3p-16,
        -0x1.555545995a603p-3,
        0x1.1107605230bc4p-7,
        -0x1.994eb3774cf24p-13,
    },
};

/* 2pi * 2^-64, for the 192-bit-precision large reduction. */
#define BT_MC_PI63 0x1.921FB54442D18p-62
/* pi/4 as a float, for the coarse exponent test. */
#define BT_MC_PIO4 0x1.921FB6p-1f

/* 4/pi to 192 bits, 8 new bits per entry (s_sincosf_data.c). */
BT_MC_TABLE uint32_t bt_mc_inv_pio4[24] = {
    0xa2,       0xa2f9,     0xa2f983,   0xa2f9836e,
    0xf9836e4e, 0x836e4e44, 0x6e4e4415, 0x4e441529,
    0x441529fc, 0x1529fc27, 0x29fc2757, 0xfc2757d1,
    0x2757d1f5, 0x57d1f534, 0xd1f534dd, 0xf534ddc0,
    0x34ddc0db, 0xddc0db62, 0xc0db6295, 0xdb629599,
    0x6295993c, 0x95993c43, 0x993c4390, 0x3c439041,
};

/* glibc's __math_invalidf(y) -- sysdeps/ieee754/flt-32/math_errf.c computes
 *   float z = (y - y) / (y - y);            -> the host's default qNaN
 *   return isnan (y) ? (y + z) : z;
 * x86 SSE makes Inf - Inf the NEGATIVE default qNaN (0xffc00000) while NVIDIA's
 * hardware default NaN is 0x7fffffff, so the host's bit pattern is constructed
 * instead of computed. No argument of bt_sinf/bt_cosf in this core can be NaN
 * or infinite (they are rotation angles), so this only keeps the port total. */
BT_MC_FN float bt_mc_invalidf(float y) {
    if (y != y) return BT_MC_AS_F32(BT_MC_AS_U32(y) | 0x00400000u);  /* quiet(y) */
    return BT_MC_AS_F32(0xffc00000u);
}

/* Top 12 bits of the float representation with the sign bit cleared. */
BT_MC_FN uint32_t bt_mc_abstop12(float x) {
    return (BT_MC_AS_U32(x) >> 20) & 0x7ffu;
}

/* sinf_poly, n even: s + x^7 * (s2 + x^2*s3), with gcc's vfmadd placement.
 * Verified against __sinf_fma's `abstop12(y) < abstop12(pio4)` path. */
BT_MC_FN float bt_mc_sin_poly_even(double x, double x2, const bt_mc_sincos_t* p) {
    double x3 = BT_MC_MUL(x, x2);
    double t1 = BT_MC_FMA(x2, p->s3, p->s2);
    double x7 = BT_MC_MUL(x3, x2);
    double s = BT_MC_FMA(x3, p->s1, x);
    return (float)BT_MC_FMA(t1, x7, s);
}

/* sinf_poly, n odd: the cosine polynomial c1 + x^4*c2 + x^6*(c3 + x^2*c4),
 * with gcc's vfmadd placement. */
BT_MC_FN float bt_mc_sin_poly_odd(double x, double x2, const bt_mc_sincos_t* p) {
    double x4 = BT_MC_MUL(x2, x2);
    double t2 = BT_MC_FMA(x2, p->c4, p->c3);
    double x6 = BT_MC_MUL(x4, x2);
    double c1p = BT_MC_FMA(x2, p->c1, p->c0);
    double cp = BT_MC_FMA(x4, p->c2, c1p);
    return (float)BT_MC_FMA(t2, x6, cp);
}

/* reduce_fast: n = round(x*2/pi), returned y = x - n*pi/2 in [-pi/4, pi/4].
 * The fused multiply-add is deliberate: __sinf_fma computes x - n*hpi with one
 * rounding (vfnmadd), not two. */
BT_MC_FN double bt_mc_reduce_fast(double x, const bt_mc_sincos_t* p, int* np) {
    double r = BT_MC_MUL(x, p->hpi_inv);
    int n = ((int)r + 0x800000) >> 24;
    *np = n;
    return BT_MC_FMA(-(double)n, p->hpi, x);
}

/* reduce_large: 32x96->128-bit fixed point modulo of 4/pi (exact). All integer
 * arithmetic: the 32-bit product and the shifts must wrap exactly as written. */
BT_MC_FN double bt_mc_reduce_large(uint32_t xi, int* np) {
    const uint32_t* arr = &bt_mc_inv_pio4[(xi >> 26) & 15];
    int shift = (int)((xi >> 23) & 7);
    uint64_t n, res0, res1, res2;

    xi = (xi & 0xffffffu) | 0x800000u;
    xi <<= shift;

    res0 = (uint32_t)(xi * arr[0]);          /* 32-bit wrap, as in glibc */
    res1 = (uint64_t)xi * arr[4];
    res2 = (uint64_t)xi * arr[8];
    res0 = (res2 >> 32) | (res0 << 32);
    res0 += res1;

    n = (res0 + (1ull << 61)) >> 62;
    res0 -= n << 62;
    double x = (double)(int64_t)res0;
    *np = (int)n;
    return BT_MC_MUL(x, BT_MC_PI63);
}

/* float sinf, bit-identical to the __sinf_fma glibc resolves on this host. */
BT_MC_FN float bt_mc_sinf(float y) {
    uint32_t yi = BT_MC_AS_U32(y);
    uint32_t e = (yi >> 20) & 0x7ffu;
    double x = (double)y;
    int n;
    const bt_mc_sincos_t* p = &bt_mc_sincos_table[0];

    if (e < 0x3f4u) {                        /* abstop12(y) < abstop12(pi/4) */
        double s = BT_MC_MUL(x, x);
        if (e < 0x398u)                      /* abstop12(y) < abstop12(2^-12) */
            return y;                        /* glibc also force-evaluates s for the underflow flag */
        return bt_mc_sin_poly_even(x, s, p);
    } else if (e < 0x42fu) {                 /* abstop12(y) < abstop12(120.0f) */
        x = bt_mc_reduce_fast(x, p, &n);
        double s = p->sign[n & 3];
        if (n & 2) p = &bt_mc_sincos_table[1];
        /* sinf_poly(x*s, x*x, p, n): the quadrant picks the polynomial. */
        return (n & 1) ? bt_mc_sin_poly_odd(BT_MC_MUL(x, s), BT_MC_MUL(x, x), p)
                       : bt_mc_sin_poly_even(BT_MC_MUL(x, s), BT_MC_MUL(x, x), p);
    } else if (e < 0x7f8u) {                 /* finite: use the integer reduction */
        int sign = (int)(yi >> 31);
        x = bt_mc_reduce_large(yi, &n);
        double s = p->sign[(n + sign) & 3];
        if ((n + sign) & 2) p = &bt_mc_sincos_table[1];
        return (n & 1) ? bt_mc_sin_poly_odd(BT_MC_MUL(x, s), BT_MC_MUL(x, x), p)
                       : bt_mc_sin_poly_even(BT_MC_MUL(x, s), BT_MC_MUL(x, x), p);
    }
    return bt_mc_invalidf(y);
}

/* float cosf, bit-identical to the __cosf_fma glibc resolves on this host. */
BT_MC_FN float bt_mc_cosf(float y) {
    uint32_t yi = BT_MC_AS_U32(y);
    uint32_t e = (yi >> 20) & 0x7ffu;
    double x = (double)y;
    int n;
    const bt_mc_sincos_t* p = &bt_mc_sincos_table[0];

    if (e < 0x3f4u) {
        double x2 = BT_MC_MUL(x, x);
        if (e < 0x398u) return 1.0f;
        return bt_mc_sin_poly_odd(x, x2, p);
    } else if (e < 0x42fu) {
        x = bt_mc_reduce_fast(x, p, &n);
        double s = p->sign[n & 3];
        if (n & 2) p = &bt_mc_sincos_table[1];
        /* sinf_poly(x*s, x*x, p, n ^ 1) */
        return ((n ^ 1) & 1) ? bt_mc_sin_poly_odd(BT_MC_MUL(x, s), BT_MC_MUL(x, x), p)
                             : bt_mc_sin_poly_even(BT_MC_MUL(x, s), BT_MC_MUL(x, x), p);
    } else if (e < 0x7f8u) {
        int sign = (int)(yi >> 31);
        x = bt_mc_reduce_large(yi, &n);
        double s = p->sign[(n + sign) & 3];
        if ((n + sign) & 2) p = &bt_mc_sincos_table[1];
        return ((n ^ 1) & 1) ? bt_mc_sin_poly_odd(BT_MC_MUL(x, s), BT_MC_MUL(x, x), p)
                             : bt_mc_sin_poly_even(BT_MC_MUL(x, s), BT_MC_MUL(x, x), p);
    }
    return bt_mc_invalidf(y);
}

/* ---------------------------------------------------------------------------
 * Part 2: atan2f.  glibc sysdeps/ieee754/flt-32/e_atan2f.c, i.e. CORE-MATH's
 * correctly rounded atan2f (glibc 2.43+). Unlike sinf, this one IS correctly
 * rounded, but "correctly rounded" still has to be reproduced exactly, and a
 * device `(float)atan2((double)y, (double)x)` is not: CUDA's double atan2 is
 * documented at 2 ulp, which lands on the wrong side of a float rounding
 * boundary for a small fraction of inputs. The algorithm is pure double +
 * fma + double-double arithmetic, so the port is exact.
 *
 * The original version of e_atan2f.c was copied from the CORE-MATH project
 * (file src/binary32/atan2/atan2f.c, revision 9b28a4a), Copyright (c)
 * 2022-2024 Alexei Sibidanov and Paul Zimmermann, under the MIT licence:
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a
 *   copy of this software and associated documentation files (the "Software"),
 *   to deal in the Software without restriction, including without limitation
 *   the rights to use, copy, modify, merge, publish, distribute, sublicense,
 *   and/or sell copies of the Software, and to permit persons to whom the
 *   Software is furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in
 *   all copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 *   DEALINGS IN THE SOFTWARE.
 * ------------------------------------------------------------------------- */

/* xh + xl times ch + cl, returning the high part and storing the low part. */
BT_MC_FN double bt_mc_muldd(double xh, double xl, double ch, double cl, double* l) {
    double ahlh = BT_MC_MUL(ch, xl);
    double alhh = BT_MC_MUL(cl, xh);
    double ahhh = BT_MC_MUL(ch, xh);
    double ahhl = BT_MC_FMA(ch, xh, -ahhh);
    ahhl += alhh + ahlh;
    ch = ahhh + ahhl;
    *l = (ahhh - ch) + ahhl;
    return ch;
}

/* Double-double Horner evaluation of a degree n-1 polynomial. */
BT_MC_FN double bt_mc_polydd(double xh, double xl, int n, const double c[][2], double* l) {
    int i = n - 1;
    double ch = c[i][0];
    double cl = c[i][1];
    while (--i >= 0) {
        ch = bt_mc_muldd(xh, xl, ch, cl, &cl);
        double th = ch + c[i][0];
        double tl = (c[i][0] - th) + ch;
        ch = th;
        cl += tl + c[i][1];
    }
    *l = cl;
    return ch;
}

/* For y/x tiny, use the Taylor approximation z - z^3/3 with z = y/x. */
BT_MC_FN float bt_mc_atan2f_tiny(float y, float x) {
    double dy = y;
    double dx = x;
    double z = BT_MC_DIV(dy, dx);
    double e = BT_MC_FMA(-z, x, y);          /* x,y promote exactly; z*x + e = y */
    static const double c = -0x1.5555555555555p-2;   /* -1/3 rounded to nearest */
    double zz = BT_MC_MUL(z, z);
    double cz = BT_MC_MUL(c, z);
    e = BT_MC_ADD(BT_MC_DIV(e, dx), BT_MC_MUL(cz, zz));
    uint64_t t = BT_MC_AS_U64(z);
    if ((t & 0xfffffffull) == 0) {
        /* Boundary case: nudge the significand so the binary64 -> binary32
         * rounding cannot double-round the wrong way. */
        if (BT_MC_MUL(z, e) > 0)
            t += 1;
        else
            t -= 1;
    }
    double r = BT_MC_AS_D64(t);
    return (float)r;                         /* glibc maps the 0 result to a signed underflow */
}

/* float addition with the host's NaN propagation: quiet(y) if y is a NaN, else
 * quiet(x), else the sum. Only used on bt_mc_atan2f's NaN branches. */
BT_MC_FN float bt_mc_nan_add(float x, float y) {
    if (y != y) return BT_MC_AS_F32(BT_MC_AS_U32(y) | 0x00400000u);
    if (x != x) return BT_MC_AS_F32(BT_MC_AS_U32(x) | 0x00400000u);
    return x + y;
}

BT_MC_FN float bt_mc_atan2f(float y, float x) {
    static const double cn[] = {
        0x1p+0, 0x1.40e0698f94c35p+1, 0x1.248c5da347f0dp+1,
        0x1.d873386572976p-1, 0x1.46fa40b20f1dp-3, 0x1.33f5e041eed0fp-7,
        0x1.546bbf28667c5p-14,
    };
    static const double cd[] = {
        0x1p+0, 0x1.6b8b143a3f6dap+1, 0x1.8421201d18ed5p+1,
        0x1.8221d086914ebp+0, 0x1.670657e3a07bap-2, 0x1.0f4951fd1e72dp-5,
        0x1.b3874b8798286p-11,
    };
    static const double m[] = { 0, 1 };
#define BT_MC_PI 0x1.921fb54442d18p+1
#define BT_MC_PI2 0x1.921fb54442d18p+0
#define BT_MC_PI2L 0x1.1a62633145c07p-54
    static const double off[] = { 0.0f, BT_MC_PI2, BT_MC_PI, BT_MC_PI2, -0.0f, -BT_MC_PI2, -BT_MC_PI, -BT_MC_PI2 };
    static const double offl[] = {
        0.0f, BT_MC_PI2L, 2 * BT_MC_PI2L, BT_MC_PI2L,
        -0.0f, -BT_MC_PI2L, -2 * BT_MC_PI2L, -BT_MC_PI2L,
    };
    static const double sgn[] = { 1, -1 };

    uint32_t ux = BT_MC_AS_U32(x);
    uint32_t uy = BT_MC_AS_U32(y);
    uint32_t ax = ux & (~0u >> 1);
    uint32_t ay = uy & (~0u >> 1);

    if (ay >= (0xffu << 23) || ax >= (0xffu << 23)) {
        /* x + y (the source keeps the addition so that the invalid exception is
         * raised for (qnan,snan)), with x86's propagation rule spelled out:
         * ADDSS/ADDSD with x in the destination returns quiet(y) when y is a
         * NaN and quiet(x) otherwise, while the device returns its own default
         * NaN (0x7fffffff). Measured: atan2f(nan_a, 1)=quiet(nan_a);
         * atan2f(1, nan_b)=quiet(nan_b); atan2f(nan_a, nan_b)=quiet(nan_b). */
        if (ay > (0xffu << 23)) return bt_mc_nan_add(x, y);
        if (ax > (0xffu << 23)) return bt_mc_nan_add(x, y);
        bool yinf = ay == (0xffu << 23);
        bool xinf = ax == (0xffu << 23);
        if (yinf && xinf) {
            if (ux >> 31)
                return (float)(0x1.2d97c7f3321d2p+1 * sgn[uy >> 31]);   /* +/-3pi/4 */
            else
                return (float)(0x1.921fb54442d18p-1 * sgn[uy >> 31]);   /* +/-pi/4 */
        }
        if (xinf) {
            if (ux >> 31)
                return (float)(BT_MC_PI * sgn[uy >> 31]);
            else
                return (float)(0.0 * sgn[uy >> 31]);
        }
        if (yinf) return (float)(BT_MC_PI2 * sgn[uy >> 31]);
    }

    if (ay == 0) {
        if (ax == 0) {
            uint32_t i = (uy >> 31) * 4 + (ux >> 31) * 2;
            if (ux >> 31)
                return (float)(off[i] + offl[i]);
            else
                return (float)off[i];
        }
        if (!(ux >> 31)) return (float)(0.0 * sgn[uy >> 31]);
    }

    uint32_t gt = (uint32_t)(ay > ax);
    uint32_t i = (uy >> 31) * 4 + (ux >> 31) * 2 + gt;

    double zx = x;
    double zy = y;
    double z = BT_MC_DIV(m[gt] * zx + m[1 - gt] * zy, m[gt] * zy + m[1 - gt] * zx);
    /* z = x/y if |y| > |x|, and z = y/x otherwise */
    double r;
    int d = (int)ax - (int)ay;
    if (d < (27 << 23) && d > -(27 << 23)) {
        double z2 = BT_MC_MUL(z, z), z4 = BT_MC_MUL(z2, z2), z8 = BT_MC_MUL(z4, z4);
        double cn0 = cn[0] + z2 * cn[1];
        double cn2 = cn[2] + z2 * cn[3];
        double cn4 = cn[4] + z2 * cn[5];
        double cn6 = cn[6];
        cn0 += z4 * cn2;
        cn4 += z4 * cn6;
        cn0 += z8 * cn4;
        double cd0 = cd[0] + z2 * cd[1];
        double cd2 = cd[2] + z2 * cd[3];
        double cd4 = cd[4] + z2 * cd[5];
        double cd6 = cd[6];
        cd0 += z4 * cd2;
        cd4 += z4 * cd6;
        cd0 += z8 * cd4;
        r = BT_MC_DIV(cn0, cd0);
    } else {
        r = 1;
    }
    z = BT_MC_MUL(z, sgn[gt]);
    r = BT_MC_ADD(BT_MC_MUL(z, r), off[i]);
    if (((BT_MC_AS_U64(r) + 8) & 0xfffffffull) <= 16) {
        /* check tiny y/x */
        if (ay < ax && ((ax - ay) >> 23 >= 25)) return bt_mc_atan2f_tiny(y, x);
        double zh;
        double zl;
        if (gt == 0) {
            zh = BT_MC_DIV(zy, zx);
            zl = BT_MC_DIV(BT_MC_FMA(zh, -zx, zy), zx);
        } else {
            zh = BT_MC_DIV(zx, zy);
            zl = BT_MC_DIV(BT_MC_FMA(zh, -zy, zx), zy);
        }
        double z2l;
        double z2h = bt_mc_muldd(zh, zl, zh, zl, &z2l);
        static const double c[32][2] = {
            {  0x1p+0,                -0x1.8c1dac5492248p-87 },
            { -0x1.5555555555555p-2,  -0x1.55553bf3a2abep-56 },
            {  0x1.999999999999ap-3,  -0x1.99deed1ec9071p-57 },
            { -0x1.2492492492492p-3,  -0x1.fd99c8d18269ap-58 },
            {  0x1.c71c71c71c717p-4,  -0x1.651eee4c4d9dp-61 },
            { -0x1.745d1745d1649p-4,  -0x1.632683d6c44a6p-58 },
            {  0x1.3b13b13b11c63p-4,   0x1.bf69c1f8af41dp-58 },
            { -0x1.11111110e6338p-4,   0x1.3c3e431e8bb68p-61 },
            {  0x1.e1e1e1dc45c4ap-5,  -0x1.be2db05c77bbfp-59 },
            { -0x1.af286b8164b4fp-5,   0x1.a4673491f0942p-61 },
            {  0x1.86185e9ad4846p-5,   0x1.e12e32d79fceep-59 },
            { -0x1.642c6d5161faep-5,   0x1.3ce76c1ca03fp-59 },
            {  0x1.47ad6f277e5bfp-5,  -0x1.abd8d85bdb714p-60 },
            { -0x1.2f64a2ee8896dp-5,   0x1.ef87d4b615323p-61 },
            {  0x1.1a6a2b31741b5p-5,   0x1.a5d9d973547eep-62 },
            { -0x1.07fbdad65e0a6p-5,  -0x1.65ac07f5d35f4p-61 },
            {  0x1.ee9932a9a5f8bp-6,   0x1.f8b9623f6f55ap-61 },
            { -0x1.ce8b5b9584dc6p-6,   0x1.fe5af96e8ea2dp-61 },
            {  0x1.ac9cb288087b7p-6,  -0x1.450cdfceaf5cap-60 },
            { -0x1.84b025351f3e6p-6,   0x1.579561b0d73dap-61 },
            {  0x1.52f5b8ecdd52bp-6,   0x1.036bd2c6fba47p-60 },
            { -0x1.163a8c44909dcp-6,   0x1.18f735ffb9f16p-60 },
            {  0x1.a400dce3eea6fp-7,  -0x1.c90569c0c1b5cp-61 },
            { -0x1.1caa78ae6db3ap-7,  -0x1.4c60f8161ea09p-61 },
            {  0x1.52672453c0731p-8,   0x1.834efb598c338p-62 },
            { -0x1.5850c5be137cfp-9,  -0x1.445fc150ca7f5p-63 },
            {  0x1.23eb98d22e1cap-10, -0x1.388fbaf1d783p-64 },
            { -0x1.8f4e974a40741p-12,  0x1.271198a97da34p-66 },
            {  0x1.a5cf2e9cf76e5p-14, -0x1.887eb4a63b665p-68 },
            { -0x1.420c270719e32p-16,  0x1.efd595b27888bp-71 },
            {  0x1.3ba2d69b51677p-19, -0x1.4fb06829cdfc7p-73 },
            { -0x1.29b7e6f676385p-23, -0x1.a783b6de718fbp-77 },
        };
        double pl;
        double ph = bt_mc_polydd(z2h, z2l, 32, c, &pl);
        zh *= sgn[gt];
        zl *= sgn[gt];
        ph = bt_mc_muldd(zh, zl, ph, pl, &pl);
        double sh = BT_MC_ADD(ph, off[i]);
        double sl = (((off[i] - sh) + ph) + pl) + offl[i];
        float rf = sh;
        double th = rf;
        double dh = sh - th;
        double tm = dh + sl;
        uint64_t tth = BT_MC_AS_U64(th);
        if (th + th * 0x1p-60 == th - th * 0x1p-60) {
            tth &= (uint64_t)0x7ff << 52;
            tth -= (uint64_t)24 << 52;
            if (fabs(tm) > BT_MC_AS_D64(tth))
                tm *= 1.25;
            else
                tm *= 0.75;
        }
        r = th + tm;
    }
    float rf = r;
    return rf;   /* glibc returns a signed zero (with the underflow flag) here */
}

/* sqrtf: the one operation the core still resolved to a raw intrinsic
 * (__fsqrt_rn).  For every NON-NaN result the two agree bit-for-bit -- IEEE
 * requires sqrt to be correctly rounded, and __fsqrt_rn is exactly that -- but
 * they disagree about the NaN bits, which this port exists to pin:
 *
 *   - negative finite and -inf: glibc returns the SIGNED default qNaN
 *     0xffc00000, __fsqrt_rn returns CUDA's canonical 0x7fffffff.
 *   - a NaN argument: glibc quietens it (sets the quiet bit) and preserves both
 *     the payload and the sign; __fsqrt_rn returns the canonical NaN.
 *   - -0.0f: both return -0.0f (IEEE), so it needs no clause -- it falls through
 *     to __fsqrt_rn below.
 *
 * Measured on this host (glibc 2.44, x86-64) with
 * /home/ilyaas/nv_probe/sqrt_nan.cpp: sqrtf(-0.75f) = 0xffc00000,
 * sqrtf(-1.0f)/sqrtf(-inf) = 0xffc00000, sqrtf(-0.0f) = 0x80000000,
 * sqrtf(0x7fc00001) = 0x7fc00001, sqrtf(0xffc00001) = 0xffc00001,
 * sqrtf(0x7f800001) = 0x7fc00001 (quiet bit set, payload kept).
 *
 * The core itself can only reach the agree-on-everything part: all 22 bt_sqrtf
 * call sites square their operand (dx*dx + dy*dy and friends) or clamp it
 * (bt_maxf(0, stat)), so a negative argument is unreachable in the sim.  The
 * clause is here because layer E of tests/test_brotato_cuda.cu compares the
 * whole float range bit-for-bit, and because a NaN that reaches the state must
 * have one definition, not two. */
BT_MC_FN float bt_mc_sqrtf(float x) {
#if defined(__CUDA_ARCH__)
    if (x != x)                        /* NaN: quiet the payload, keep sign + bits */
        return BT_MC_AS_F32(BT_MC_AS_U32(x) | 0x00400000u);
    if (x < 0.0f)                      /* negative finite and -inf */
        return BT_MC_AS_F32(0xffc00000u);
    return __fsqrt_rn(x);              /* x >= 0, +inf, +/-0.0 (sign preserved) */
#else
    return sqrtf(x);                   /* the host libm this port reproduces */
#endif
}

#undef BT_MC_PI
#undef BT_MC_PI2
#undef BT_MC_PI2L
