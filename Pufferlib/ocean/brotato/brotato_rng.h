/* Portable deterministic RNG + device-portability prelude for the shared
 * Brotato Bull/Danger-5 numerical core.
 *
 * This is the lowest-level header of the shared core. It defines the
 * qualifier/address-space/table macros every other core header uses, then the
 * RNG.
 *
 * INTENTIONAL RNG CUTOVER (documented divergence, not a regression):
 *   The CPU env used glibc rand_r() seeded once from `1 + tag`. rand_r is not
 *   portable - glibc/BSD/musl/WASM produce different streams - and has no
 *   device implementation, so CUDA and Metal could never share a rollout with
 *   the CPU env. The shared core replaces it with PCG32 (M. O'Neill), using
 *   Godot 3.7-dev1 RandomPCG's default sequence (PCG_DEFAULT_INC_64).
 *   Integer outputs and inclusive integer-range reduction can therefore be
 *   checked directly against RandomNumberGenerator in the pinned engine.
 *
 *   Rollouts/checkpoints from rand_r or the earlier 0xda3e39cb94b95bdb
 *   sequence are invalidated. Primitive agreement does NOT imply whole-game
 *   replay agreement: the game has independent global and Utils RNG streams,
 *   and their draw ordering must also match.
 *
 * FLOAT PRIMITIVES: the floating-point draws were a 1/10000-step surrogate and
 * are now exact transcriptions of the pinned engine's random_pcg.h/.cpp,
 * random_number_generator.h, math_funcs.h/.cpp and GDScript built-in dispatch.
 * See the block below bt_rng_bounded for per-function sources and draw counts.
 * The portable global range helpers use integer binary64 arithmetic on Metal.
 *
 * Deterministic seed method (exact, reproducible on every backend):
 *   bt_rng_seed(r, seed):
 *       r->state = 0;
 *       r->inc   = (BT_RNG_SEQ << 1) | 1;   // odd => full 2^64 period
 *       (void)bt_rng_next(r);               // discard 1
 *       r->state += seed;                   // 64-bit wraparound
 *       (void)bt_rng_next(r);               // discard the seeded state
 *   BT_RNG_SEQ = 1442695040888963407, as pinned in Godot commit 0a9011743,
 *   thirdparty/misc/pcg.h and core/math/random_pcg.h. Seed 0 is valid.
 *
 *   The env seeds once per run from its creation seed and then lets the stream
 *   run across episodes (matching the old rand_r behaviour, which reset() never
 *   re-seeded). BtState.rng_ready makes that one-shot seeding explicit.
 */
#pragma once

/* ---------------------------------------------------------------------------
 * Device portability prelude.
 *
 * Host C/C++ (clang/gcc)   : qualifiers empty; tables are `static const`.
 * CUDA (nvcc/clang -x cuda): BT_HD = __host__ __device__; tables `static const`.
 * Metal Shading Language   : BT_HD empty; BT_DEVICE/BT_THREAD/BT_CONSTANT are
 *   MSL address spaces, BT_TABLE puts program-scope tables in `constant`
 *   (MSL rejects any other address space at program scope), and BT_CONST_PTR
 *   gives a pointer-to-constant-table element the required address space.
 *
 * bt_u64: MSL rejects `long long`/`unsigned long long`, so device 64-bit is
 * `unsigned long` (64-bit on Apple/LP64 and CUDA); host uses `unsigned long
 * long`. Keep all 64-bit core scalars in bt_u64 - never spell the type.
 * ------------------------------------------------------------------------- */
#if defined(__METAL_VERSION__)
#ifndef BT_HD
#define BT_HD
#endif
#define BT_DEVICE device
#define BT_THREAD thread
#define BT_CONSTANT constant
#define BT_TABLE static constant
#define BT_CONST_PTR(T) constant T*
/* MSL carries no name/pointer tables: drop the contents. */
#define BT_HOST_ONLY(...)
#ifndef BT_U64_DEFINED
#define BT_U64_DEFINED
typedef unsigned long bt_u64;
#endif
#elif defined(__CUDACC__) || defined(__CUDA_ARCH__) || defined(BT_CUDA)
#ifndef BT_HD
#define BT_HD __host__ __device__
#endif
#define BT_DEVICE
#define BT_THREAD
#define BT_CONSTANT
/* nvcc: a namespace-scope `const` array is a HOST variable and cannot be
 * referenced/taken by address from device code (CUDA guide E.2.13). Give the
 * device pass its own device-resident read-only copy (no 64 KB __constant__
 * budget, no constant->generic reliance); the host pass keeps a plain const
 * copy. Both passes therefore define the same symbol consistently. */
#if defined(__CUDA_ARCH__)
#define BT_TABLE static __device__ const
#else
#define BT_TABLE static const
#endif
#define BT_CONST_PTR(T) const T*
/* Only the device pass omits names. CUDA's host renderer reads the same
 * generated stat/name contract as the CPU renderer. */
#if defined(__CUDA_ARCH__)
#define BT_HOST_ONLY(...)
#else
#define BT_HOST_ONLY(...) __VA_ARGS__
#endif
#ifndef BT_U64_DEFINED
#define BT_U64_DEFINED
typedef unsigned long long bt_u64;
#endif
#else
#ifndef BT_HD
#define BT_HD
#endif
#define BT_DEVICE
#define BT_THREAD
#define BT_CONSTANT
#define BT_TABLE static const
#define BT_CONST_PTR(T) const T*
/* Function-like so generated headers can wrap a whole host-only declaration
 * (e.g. `BT_HOST_ONLY(static const char* const NAMES[] = {...};)`); the host
 * keeps it, device passes drop it. */
#define BT_HOST_ONLY(...) __VA_ARGS__
#ifndef BT_U64_DEFINED
#define BT_U64_DEFINED
typedef unsigned long long bt_u64;
#endif
#endif

/* Portable RNG state. Two 64-bit words; POD, no pointers, trivially copyable.
 * Lives inside BtState, so it is BT_DEVICE on MSL. */
typedef struct BtRng {
    bt_u64 state;
    bt_u64 inc;
} BtRng;

/* 64-bit constants spelled without an `ull` suffix so MSL accepts them
 * (6364136223846793005 fits a 64-bit signed long; the hex value needs
 * unsigned long). Cast through bt_u64 for the same width on every backend. */
#define BT_RNG_MULT ((bt_u64)6364136223846793005)
#define BT_RNG_SEQ  ((bt_u64)1442695040888963407)

/* One PCG32 output. `mask` is computed before use so the rot == 0 case is
 * well-defined. */
BT_HD static unsigned int bt_rng_next(BT_DEVICE BtRng* r) {
    bt_u64 old = r->state;
    r->state = old * BT_RNG_MULT + r->inc;
    unsigned int xorshifted = (unsigned int)(((old >> 18) ^ old) >> 27);
    unsigned int rot = (unsigned int)(old >> 59);
    unsigned int mask = (unsigned int)((0u - rot) & 31u);
    return (xorshifted >> rot) | (xorshifted << mask);
}

/* Exact deterministic seed method; see the file header. */
BT_HD static void bt_rng_seed(BT_DEVICE BtRng* r, bt_u64 seed) {
    r->state = (bt_u64)0;
    r->inc = (BT_RNG_SEQ << 1) | (bt_u64)1;
    (void)bt_rng_next(r);
    r->state += seed;
    (void)bt_rng_next(r);
}

/* Uniform in [0, bound). Godot 3.7 RandomNumberGenerator::randi_range uses
 * the same modulo reduction, then adds the inclusive lower endpoint.
 * Even bound == 1 consumes an output. bound == 0 is invalid and yields 0. */
BT_HD static unsigned int bt_rng_bounded(BT_DEVICE BtRng* r, unsigned int bound) {
    if (bound == 0u) return 0u;
    return bt_rng_next(r) % bound;
}

/* ---------------------------------------------------------------------------
 * Exact Godot floating-point draws (pinned commit 0a9011743).
 *
 * Every function below is a transcription of the pinned engine and takes an
 * explicit stream (BtRng*), so the caller drives whichever stream the source
 * used.
 *
 * Sources:
 *   core/math/random_pcg.h          RandomPCG::randf / randd
 *   core/math/random_pcg.cpp        RandomPCG::random(float,float) / random(double,double)
 *   core/math/random_number_generator.h
 *                                   RandomNumberGenerator::randf / randf_range / randi_range
 *   core/math/math_funcs.h/.cpp     Math::rand / Math::randf,
 *                                   Math::random(double,double) on Math::default_rand
 *   modules/gdscript/gdscript_functions.cpp
 *                                   MATH_RAND -> Math::rand; MATH_RANDF -> Math::randf;
 *                                   MATH_RANDOM -> Math::random((double)from,(double)to)
 *
 * GDScript global surface (stream = Math::default_rand) -- what the recovered
 * game calls:
 *   randi() / rand()  -> Math::rand()                        1 draw, value = the draw
 *   randf()           -> Math::randf()                       1 draw, (float)draw / (float)0xFFFFFFFF
 *   rand_range(a, b)  -> Math::random((double)a, (double)b)  3 draws (1 when the
 *                        first draw is 0), value = randd()*(b-a)+a in FP64;
 *                        Variant stores REAL as `double _real`, so the FP64
 *                        result is not narrowed on the way out
 *   seed(x)/randomize() -> Math::seed / Math::randomize (explicit / time-based)
 *
 * RandomNumberGenerator methods (Utils.gd keeps its own instance, Utils._rng --
 * a second, independent PCG32 stream):
 *   randi()       -> randbase.rand()            1 draw
 *   randi_range() -> randbase.rand()            1 draw, inclusive, reversal-tolerant
 *   randf()       -> randbase.randf()           2 draws
 *   randf_range() -> randbase.random(from,to)   2 draws, FP32 math in a float build
 *
 * The two streams are independent and Utils._rng is seeded with randomize() in
 * Utils._ready() (overridable via Utils.set_rng_seed), so primitive parity does
 * NOT establish whole-game draw-order parity: the core must also route each
 * source call to the correct stream and know Utils._rng's seed.
 *
 * MSL has no native FP64. The portable global_range_float/global_range_int
 * helpers below use brotato_rng_f64.h there, retaining binary64 operation
 * rounding, draw counts, and truncation-before-narrowing without a fallback.
 * Raw double-returning helpers are available only on host/CUDA.
 * ------------------------------------------------------------------------- */

#if defined(__METAL_VERSION__)
#include <metal_stdlib>
#else
#include <math.h>
#endif

#if defined(__METAL_VERSION__)
#define BT_RNG_HAS_F64 0
#else
#define BT_RNG_HAS_F64 1
typedef double bt_f64;
#endif

/* CLZ32 in random_pcg.h. Both engine callers guard x != 0; this definition is
 * total so the helpers stay safe in isolation. */
BT_HD static unsigned int bt_rng_clz32(unsigned int x) {
    if (x == 0u) return 32u;
#if defined(__METAL_VERSION__)
    return (unsigned int)metal::clz(x);
#elif defined(__CUDA_ARCH__)
    return (unsigned int)__clz(x);
#elif defined(__GNUC__) || defined(__clang__)
    return (unsigned int)__builtin_clz(x);
#else
    unsigned int n = 0u;
    while ((x & 0x80000000u) == 0u) { x <<= 1; n++; }
    return n;
#endif
}

/* LDEXPF in random_pcg.h. Callers only pass exponents in [-95,-32], so the
 * result is normal and a power-of-two scaling is exact; MSL's ldexp is
 * documented as correctly rounded, so it is exact here too. */
BT_HD static float bt_rng_ldexpf(float x, int e) {
#if defined(__METAL_VERSION__)
    return metal::ldexp(x, e);
#else
    return ldexpf(x, e);
#endif
}

/* RandomPCG::randf -- core/math/random_pcg.h. Two draws; one when the first is
 * 0, in which case the engine returns +0.0f. */
BT_HD static float bt_rng_randf(BT_DEVICE BtRng* r) {
    unsigned int proto_exp_offset = bt_rng_next(r);
    if (proto_exp_offset == 0u) return 0.0f;
    unsigned int sig = bt_rng_next(r) | 0x80000001u;
    return bt_rng_ldexpf((float)sig, -32 - (int)bt_rng_clz32(proto_exp_offset));
}

/* RandomPCG::randd -- core/math/random_pcg.h. Three draws; one when the first
 * is 0, in which case the engine returns +0.0. */
#if BT_RNG_HAS_F64
BT_HD static bt_f64 bt_rng_randd(BT_DEVICE BtRng* r) {
    unsigned int proto_exp_offset = bt_rng_next(r);
    if (proto_exp_offset == 0u) return (bt_f64)0.0;
    unsigned int high = bt_rng_next(r);
    unsigned int low = bt_rng_next(r);
    bt_u64 sig = (((bt_u64)high << 32) | (bt_u64)low) | (bt_u64)0x8000000000000001;
    int e = -64 - (int)bt_rng_clz32(proto_exp_offset);
    return (bt_f64)ldexp((double)sig, e);
}
#endif

/* RandomPCG::random(float,float) = RandomNumberGenerator.randf_range. */
BT_HD static float bt_rng_random_f(BT_DEVICE BtRng* r, float from, float to) {
    return bt_rng_randf(r) * (to - from) + from;
}

/* RandomPCG::random(double,double) = Math::random(double,double) = the GDScript
 * global rand_range(). Pass bt_f64 bounds to keep the FP64 arithmetic the
 * engine uses on a float build. */
#if BT_RNG_HAS_F64
BT_HD static bt_f64 bt_rng_random_d(BT_DEVICE BtRng* r, bt_f64 from, bt_f64 to) {
    return bt_rng_randd(r) * (to - from) + from;
}
#endif

/* Math::randf -- the global GDScript randf(). One draw. Distinct from
 * RandomPCG::randf: the divisor is (float)0xFFFFFFFF, which rounds to 2^32, so
 * the result can reach exactly 1.0f. */
BT_HD static float bt_math_randf(BT_DEVICE BtRng* r) {
    return (float)bt_rng_next(r) / (float)0xFFFFFFFFu;
}

/* RandomNumberGenerator::randi_range -- inclusive, reversal-tolerant, one draw.
 * (bt_rng_bounded above is the from == 0 special case.) */
BT_HD static int bt_rng_randi_range(BT_DEVICE BtRng* r, int from, int to) {
    unsigned int ret = bt_rng_next(r);
    if (to < from) {
        return (int)(ret % (unsigned int)(from - to + 1)) + to;
    }
    return (int)(ret % (unsigned int)(to - from + 1)) + from;
}

#include "brotato_rng_f64.h"

/* Force the software path in native differential checks; production host/CUDA
 * uses its real double instructions. Neither branch contracts the multiply/add. */
#if defined(__METAL_VERSION__)
/* Preserve one shared software-binary64 implementation across call sites. */
__attribute__((noinline))
#endif
BT_HD static float bt_rng_global_range_float(BT_DEVICE BtRng* r, float from, float to) {
#if BT_RNG_HAS_F64 && !defined(BT_RNG_FORCE_SOFT_F64)
    return (float)bt_rng_random_d(r, (double)from, (double)to);
#else
    return bt_rf64_to_float(bt_rf64_range(r, from, to));
#endif
}

#if defined(__METAL_VERSION__)
__attribute__((noinline))
#endif
BT_HD static int bt_rng_global_range_int(BT_DEVICE BtRng* r, float from, float to) {
#if BT_RNG_HAS_F64 && !defined(BT_RNG_FORCE_SOFT_F64)
    return (int)bt_rng_random_d(r, (double)from, (double)to);
#else
    return bt_rf64_to_int(bt_rf64_range(r, from, to));
#endif
}

/* Script members retain binary64 results. Table bounds use raw IEEE bits so
 * PI/TAU and decimal literals are not rounded to binary32 before sampling. */
#if defined(__METAL_VERSION__)
__attribute__((noinline))
#endif
BT_HD static bt_u64 bt_rng_global_range_bits(BT_DEVICE BtRng* r, bt_u64 from, bt_u64 to) {
#if BT_RNG_HAS_F64 && !defined(BT_RNG_FORCE_SOFT_F64)
    union { bt_u64 u; bt_f64 f; } a, b, result;
    a.u = from; b.u = to;
    result.f = bt_rng_random_d(r, a.f, b.f);
    return result.u;
#else
    return bt_rf64_to_bits(bt_rf64_range_bounds(r, bt_rf64_from_bits(from), bt_rf64_from_bits(to)));
#endif
}

BT_HD static bt_u64 bt_rng_f64_add_bits(bt_u64 a, bt_u64 b) {
#if BT_RNG_HAS_F64 && !defined(BT_RNG_FORCE_SOFT_F64)
    union { bt_u64 u; bt_f64 f; } x, y, result;
    x.u = a; y.u = b; result.f = x.f + y.f;
    return result.u;
#else
    return bt_rf64_to_bits(bt_rf64_add(bt_rf64_from_bits(a), bt_rf64_from_bits(b)));
#endif
}

BT_HD static bt_u64 bt_rng_f64_trunc_bits(bt_u64 value) {
#if BT_RNG_HAS_F64 && !defined(BT_RNG_FORCE_SOFT_F64)
    union { bt_u64 u; bt_f64 f; } result;
    result.u = value; result.f = (bt_f64)(int)result.f;
    return result.u;
#else
    return bt_rf64_to_bits(bt_rf64_trunc(bt_rf64_from_bits(value)));
#endif
}

BT_HD static float bt_rng_f64_to_float_bits(bt_u64 value) {
#if BT_RNG_HAS_F64 && !defined(BT_RNG_FORCE_SOFT_F64)
    union { bt_u64 u; bt_f64 f; } result;
    result.u = value;
    return (float)result.f;
#else
    return bt_rf64_to_float(bt_rf64_from_bits(value));
#endif
}
