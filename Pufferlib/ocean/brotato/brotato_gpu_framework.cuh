// brotato_gpu_framework.cuh -- shared intra-environment GPU execution framework
// for the Brotato core. OWNER: PerfGpuKernel (architecture lead).
// STATUS: published; GpuSolver's measured adjustments are folded in (fused
// predicate+compaction instead of a flags staging array; level bucketing instead
// of per-level rescans; caller-provided scratch).
//
// Consumers:
//   * ocean/brotato/brotato_gpu_physics.cuh  (GpuSolver)  wavefront solver +
//     broadphase, built on btf_scan_excl + the count/scan/write pattern below.
//   * ocean/brotato/brotato_rng_jump.h       (GpuRng)     pcg32 jump-ahead +
//     per-phase draw counts, built on the btf_draw_parallel contract.
//   * ocean/brotato/brotato_gpu_combat.cuh   (GpuCombat)  per-entity parallel
//     compute + ordered apply, built on btf_count_by_key / btf_bin_by_key.
//   * ocean/brotato/brotato_gpu_fast.cuh     (PerfGpuKernel) prototype kernels
//     and the baseline/throughput harness.
//
// ---------------------------------------------------------------------------
// THE ONE RULE
// ---------------------------------------------------------------------------
// Every kernel built on this framework is BIT-EXACT against the CPU core. The
// core is the definition: no backend re-implements the simulation, it
// re-schedules calls to the very same BT_HD functions. A schedule is legal iff
// it preserves, for every mutable object, the ORDER of the mutations the serial
// bt_core_step performs on it. Nothing else may change: no FP reassociation, no
// RNG draw reordering, no changed solver constraint order. Every primitive here
// is designed so the parallel schedule it expresses is order-preserving by
// construction, and the harness proves it per tick by digest comparison.
//
// ---------------------------------------------------------------------------
// LANE MAPPING (two deployment modes, both supported)
// ---------------------------------------------------------------------------
//   A. BTF_WARP_PER_ENV   blockDim = 32*K warps; warp w of block b owns env
//      (b*K + w), group size bp = 32. Keeps the env COUNT high (whole batch
//      resident), so per-env scratch lives in GLOBAL memory.
//   B. BTF_BLOCK_PER_ENV  blockDim = BTF_BLOCK_LANES; one env per block, bp =
//      blockDim, scratch can be dynamic shared (~39 KB per world for the
//      solver), but only 48-96 envs are resident at once.
// Mode A is the default: env-level parallelism is free, intra-env parallelism
// costs residency, and the measured per-env per-tick DRAM working set decides.
//
// Every primitive is written for a lane GROUP of size bp. The caller passes
//     mem  = group member index in [0,bp)   -> BTF_MEM(bp)
//     slot = a per-warp scratch slice unique within the BLOCK -> BTF_SLOT
// so the shared-memory scratch never collides when several warps in one block
// run different envs concurrently. Group sync is bar.warp.sync for bp == 32 and
// the block barrier for bp > 32; a group must not mix them.
#ifndef PUFFER_BROTATO_GPU_FRAMEWORK_CUH
#define PUFFER_BROTATO_GPU_FRAMEWORK_CUH

#define BTF_WARP_PER_ENV  0
#define BTF_BLOCK_PER_ENV 1
#ifndef BTF_MODE
#define BTF_MODE BTF_WARP_PER_ENV
#endif
#ifndef BTF_BLOCK_LANES
#define BTF_BLOCK_LANES 256
#endif

// cuda_runtime.h is only needed for the CUDA keywords/types; a plain host TU
// (g++) including this header must be able to skip it.
#if defined(__CUDACC__) || defined(__CUDA_ARCH__)
#include <cuda_runtime.h>
#endif
#include <cstddef>
#include <cstdint>
#include <cstdio>

#ifndef BT_CUDA
#define BT_CUDA 1
#endif

#include "brotato_rng.h"
#include "brotato_state.h"

// ---------------------------------------------------------------------------
// Physics-stage hook + backend dispatch point
//
// bt_core_step calls bt_phys_step(&s->phys) and bt_phys_step(BT_PET_WORLD(s)).
// The framework substitutes those two call sites for btf_phys_step_hook(w), so
// no shared-core edit is needed and the two sites stay distinct (the caller's
// dispatch tests which world it received against btf_cur_state).
//
// Default behaviour is the measurement hook below, which runs a runtime-selected
// subset of the serial stages (only BTF_PHYS_FULL is a faithful simulation; the
// rest measure stage cost and are never used for parity):
//   BTF_PHYS_FULL  begin+scan+setup+solve+integrate (== bt_phys_step)
//   BTF_PHYS_NO_SCAN   begin+setup+solve+integrate
//   BTF_PHYS_SCAN_ONLY begin+scan
//   BTF_PHYS_NONE      nothing
// The host pass always runs FULL, so the CPU parity reference is unaffected.
//
// A parallel backend is attached WITHOUT macros, so include order stays free
// (GpuCombat's header must precede the core; GpuSolver's does not):
//   compile with -DBTF_PHYS_HAVE_BACKEND=1, set btf_phys_backend=1 and
//   btf_cur_state/btf_scratch/btf_bp from the driving step kernel, and provide
//   btf_phys_backend_main/_pet as thin wrappers over the backend entry points.
// The default hook then routes &s->phys and BT_PET_WORLD(s) to the backend and
// everything else to the serial stages, and one binary can A/B both paths.
// Overriding BTF_PHYS_HOOK_BODY(w) by hand is still supported.
#define BTF_PHYS_FULL      0
#define BTF_PHYS_NO_SCAN   1
#define BTF_PHYS_SCAN_ONLY 2
#define BTF_PHYS_NONE      3

// CUDA-only state. Guarded so this header (and every header built on it) is
// includable from a plain host TU (g++), which lets the peers' host-side
// equivalence tests be plain .cpp tests instead of requiring nvcc. Nothing reads
// these from host code: btf_phys_stage_select and btf_phys_step_hook_default
// only touch them under __CUDA_ARCH__.
#if defined(__CUDACC__) || defined(__CUDA_ARCH__)
__device__ int btf_phys_mode = BTF_PHYS_FULL;
/* Per-env backend context.
 *
 * A __device__ global CANNOT carry per-env state when several blocks run
 * concurrently: every resident block would race on the same pointer, so a
 * block's hook can be handed ANOTHER env's state (found in review). __shared__ is
 * block-scoped, which is exactly the scope one-env-per-block needs, so the
 * context lives there: set it once from the driving kernel with
 * btf_phys_ctx_set(s, scratch, bp) and read it from the hook.
 *
 * The three legacy __device__ globals below are kept only so existing consumers
 * keep compiling; they are UNSAFE for concurrent blocks and must not be used by
 * a new driver. */
typedef struct BtfPhysCtx { BtState* state; void* scratch; int bp; } BtfPhysCtx;

BT_HD static inline BtfPhysCtx* btf_phys_ctx(void) {
#if defined(__CUDA_ARCH__)
    static __shared__ BtfPhysCtx ctx;   /* one per BLOCK: no cross-block race */
    return &ctx;
#else
    static BtfPhysCtx host_dummy = {0, 0, 32};   /* host paths never use it */
    return &host_dummy;
#endif
}

BT_HD static inline void btf_phys_ctx_set(BtState* s, void* scratch, int bp) {
#if defined(__CUDA_ARCH__)
    BtfPhysCtx* c = btf_phys_ctx();
    c->state = s;
    c->scratch = scratch;
    c->bp = bp;
#else
    (void)s; (void)scratch; (void)bp;
#endif
}

/* The context is reached through accessors, never through mutable device
 * globals: a __device__ global is shared by every concurrently resident block,
 * so a block's hook could be handed another env's state (review finding). There
 * is no deprecated storage left; the names below resolve to the block-local
 * context, which keeps existing consumers correct without a separate API.
 * Writes must be single-writer + a barrier (btf_phys_ctx_set by one lane, then
 * btf_group_sync), and reads inside a cooperative phase are stable because the
 * context is written once per kernel before any hook runs. */
#define btf_cur_state (btf_phys_ctx()->state)
#define btf_scratch   (btf_phys_ctx()->scratch)
#define btf_bp        (btf_phys_ctx()->bp)
#else
#define BTF_PHYS_FULL_HOST_ONLY 1
#endif

// ===========================================================================
// 0b. Cooperative step-scheduling dispatch (MUST precede both the hook below
//     and brotato_core.h)
//
// bt_core_step is written as a sequence of spans. A span is either SERIAL -
// exactly ONE lane of the warp may execute it, because it does read-modify-write
// on shared scalars/queues/counters (s->rng, episode_return, pool ordinals, the
// reward ledger) and concurrent RMW is a data race under independent thread
// scheduling and the C++ memory model, even when every writer would compute the
// same value - or COOP - ALL lanes enter the phase together and cooperate, with
// a group barrier released at the boundary.
//
// These macro bodies EXPAND AT THEIR USE SITE (inside bt_core_step, parsed when
// brotato_core.h is included below, and inside btf_phys_step_hook_default just
// below), so every name they mention must already be defined HERE.
//
// btf_group_bp is the HOST-ARMED dispatch mode and is read-only in kernels:
//   1  (default) every thread is a leader and the barrier is a no-op, which is
//      exactly what the plain one-thread-per-env kernel, the CPU build, the
//      Metal build and the CPU trainer need;
//   32 one warp per env: lane 0 owns every serial span, all lanes meet at every
//      phase boundary.
// It is never written from a kernel (a write from every block would be a
// concurrent write to one location): brotato.cu arms it with
// cudaMemcpyToSymbol next to btf_phys_backend.
//
// A build that includes the core WITHOUT this header (CPU, Metal, the land
// gate) gets the identity fallbacks declared in brotato_core.h instead, so the
// core stays a standalone translation unit.
// ===========================================================================
#if defined(__CUDACC__) || defined(__CUDA_ARCH__)
__device__ int btf_group_bp = 1;
#endif
// Diagnostic only (Main / LaneReview): with -DBTFW_DIAG_SYNCTHREADS=1 the 32-lane
// barrier becomes __syncthreads(), so a SINGLE 32-thread block can tell a real
// memory-ordering defect from a bare bar.warp.sync. Never a production setting:
// with more than one warp per block the block barrier is the wrong set.
#ifndef BTFW_DIAG_SYNCTHREADS
#define BTFW_DIAG_SYNCTHREADS 0
#endif

/* A/B diagnostic (PerfGpuKernel / PerfTrainer): fold every guard to identity so
 * this TU carries no barrier arm and no fence at all. The identity expansion is
 * EXACTLY what the host branch below and brotato_core.h's own #ifndef fallbacks
 * already produce, so operation order, FP and RNG are untouched - it only removes
 * code a one-thread-per-env kernel never needed. It must NOT be combined with the
 * cooperative schedule: folding the guards in a TU that runs the warp kernel would
 * silently drop the lane discipline. */
#if defined(BTFW_GUARDS_FOLDED) && defined(BTFW_EXPERIMENTAL_LANE_SCHED)
#error "BTFW_GUARDS_FOLDED (identity guards) and BTFW_EXPERIMENTAL_LANE_SCHED (cooperative lane schedule) are mutually exclusive"
#endif
#if defined(BTFW_GUARDS_FOLDED)
#define BTF_IS_LEADER() 1
#define BTF_GRP_BP 1
#define btf_group_sync(bp) do { (void)(bp); } while (0)
#define BT_SERIAL(...) do { __VA_ARGS__ } while (0)
#define BT_COOP(...)   do { __VA_ARGS__ } while (0)
#elif defined(__CUDA_ARCH__)
// Leader of a lane group:
//   bp <= 1  -> every thread is a leader (the plain one-thread-per-env kernel;
//               each thread owns a different env, so nobody may skip a span);
//   bp >  32 -> the whole BLOCK is the group (block-per-env), so the leader is
//               block-lane 0 - NOT `threadIdx.x & 31`, which would make every
//               warp leader and run each serial span once per warp;
//   bp == 32 -> one warp per env, so lane 0 of that warp.
#define BTF_IS_LEADER()                                                     \
    (((btf_group_bp) <= 1) ? 1 :                                            \
     (((btf_group_bp) > 32) ? (((int)threadIdx.x) == 0)                     \
                            : (((int)threadIdx.x & 31) == 0)))
#define BTF_GRP_BP btf_group_bp
// bp <= 1 is a NO-OP: in the plain one-thread-per-env kernel a warp holds 32
// DIFFERENT envs and the tail of the grid exits early, so a bar.warp.sync with
// the default full mask there is illegal. bp > 32 is one env per block and uses
// the block barrier; bp == 32 is one env per warp.
#if BTFW_DIAG_SYNCTHREADS
#define BTF_GRP_SYNC_LANE() __syncthreads()   /* DIAGNOSTIC: single 32-lane block */
#else
// Publication step for the cooperative warp branch. DEFAULT ON, because a
// REPRODUCED, DISCRIMINATING A/B says global-scratch hand-offs across the bare
// warp barrier are unreliable on sm_120 (WarpForkFault, physics-only TU, bp=32,
// 20 steps): scratch in DYNAMIC SHARED -> scan and whole tick bit-exact; scratch
// in GLOBAL -> btf_scan_excl's cross-lane prefix chain non-monotone (0,3,7,10,8,
// 5,... total 36 -> 9), the compacted event list non-ascending, a monotone-hint
// decode of j=-3, and bt_phys_pool_claim storing b=-3 so btgp_level_build reads
// w->bodies[-3].inv_mass (memcheck: read 196 B before the world allocation);
// scratch in GLOBAL + __threadfence_block() -> exact again. Main's 8.39M-check
// probe only covered a SINGLE store->barrier->load, which the bare sync does
// order; it is the multi-round RMW chain through GLOBAL scratch that is not.
// Every warp-mode consumer hands off through that scratch (btf_scan_excl's
// callers: physics classify/compact/level, GpuCombat's count/bin, GpuRng's draw
// runner), so the fence is a correctness requirement, not a defensive extra.
// It costs one MEMBAR.CTA per group barrier on the cooperative branch only; the
// plain path never reaches here (identity fold / plain core), and the production
// warp path is hard-disabled, so nothing in the shipped tick pays it.
// -DBTFW_WARP_FENCE=0 removes it for a cost A/B once a shared-scratch or
// __shfl_*_sync-based consumer no longer needs it.
#ifndef BTFW_WARP_FENCE
#define BTFW_WARP_FENCE 1
#endif
#if BTFW_WARP_FENCE
#define BTF_GRP_SYNC_LANE() do { __threadfence_block(); __syncwarp(); } while (0)
#else
#define BTF_GRP_SYNC_LANE() __syncwarp()
#endif
#endif
#define btf_group_sync(bp) \
    do { int _btf_bp = (bp); \
         if (_btf_bp <= 1) { } \
         else if (_btf_bp > 32) __syncthreads(); \
         else BTF_GRP_SYNC_LANE(); } while (0)
#else
#define BTF_IS_LEADER() 1
#define BTF_GRP_BP 1
#define btf_group_sync(bp) do { (void)(bp); } while (0)
#endif

// SERIAL span: lane 0 runs the body, every lane releases at the group barrier.
// COOP phase: every lane runs the body (a cooperative implementation uses the
// lanes; a serial default body is itself lane-guarded with BT_SERIAL) and then
// all lanes release at the boundary.
#define BT_SERIAL(...) do { if (BTF_IS_LEADER()) { __VA_ARGS__ } btf_group_sync(BTF_GRP_BP); } while (0)
#define BT_COOP(...)   do { __VA_ARGS__ btf_group_sync(BTF_GRP_BP); } while (0)

// Templated on the world type: today both worlds are BtPhysWorld, and after
// StateCompact splits them into BtPhysWorldT<N> instantiations the same hook
// serves the enemy world and the pet world. The callees (bt_phys_*) are
// templated by GpuSolver; W deduces from the pointer at the call site.
template <class W>
BT_HD static inline void btf_phys_stage_select(W* w) {
#ifdef __CUDA_ARCH__
    int m = btf_phys_mode;
#else
    int m = BTF_PHYS_FULL;
#endif
    if (m == BTF_PHYS_NONE) return;
    bt_phys_begin_step(w);
    if (m == BTF_PHYS_FULL || m == BTF_PHYS_SCAN_ONLY) bt_phys_update_pairs(w);
    if (m == BTF_PHYS_SCAN_ONLY) return;
    bt_phys_setup_pairs(w);
    bt_phys_solve_pairs(w);
    bt_phys_integrate_velocities(w);
}

#ifndef BTF_PHYS_HOOK_BODY
#define BTF_PHYS_HOOK_BODY(w) btf_phys_step_hook_default(w)
#endif

// Optional parallel physics backend (ocean/brotato/brotato_gpu_physics.cuh).
// Declared here so the default hook can dispatch at RUNTIME: that keeps include
// order irrelevant (GpuCombat's header must come before the core, GpuSolver's
// does not) and lets ONE binary A/B the serial stages against the wavefront on
// identical worlds. Enable with -DBTF_PHYS_HAVE_BACKEND=1 before including this
// header and provide the two thin wrappers in the translation unit.
#ifndef BTF_PHYS_HAVE_BACKEND
#define BTF_PHYS_HAVE_BACKEND 0
#endif

#if BTF_PHYS_HAVE_BACKEND
#if defined(__CUDACC__) || defined(__CUDA_ARCH__)
__device__ int btf_phys_backend = 0;   /* 0 = serial stages, 1 = the backend */
#endif
BT_HD static inline void btf_phys_backend_main(BtState* s, void* scratch, int bp);
BT_HD static inline void btf_phys_backend_pet(BtState* s, void* scratch, int bp);
#endif

template <class W>
BT_HD static inline void btf_phys_step_hook_default(W* w) {
#if defined(__CUDA_ARCH__) && BTF_PHYS_HAVE_BACKEND
    // NOTE: this hook is defined BEFORE brotato_core.h (and therefore before
    // brotato_pet_motion.h defines BT_PET_WORLD), so the pet world is addressed
    // as &BtState::phys_pet, which brotato_state.h already declares.
    //
    // ONLY the main world is routed to the cooperative backend. bt_core_step
    // wraps that call site in BT_COOP, so every lane reaches here and the
    // wavefront schedule (which is built on group barriers) is legal. The pet
    // world's call site lives inside bt_step_pet_motion, which bt_core_step runs
    // as a SERIAL span (lane 0 only), so it must NOT enter a barrier schedule:
    // it falls through to the serial stages below and lane 0 runs them.
    if (btf_phys_backend != 0) {
        BtfPhysCtx* c = btf_phys_ctx();
        BtState* s = c->state;
        // Type-agnostic address compare (BtPhysWorldT<N> split): both worlds are
        // members of the same BtState, so their addresses are unique.
        if (s != 0 && (const void*)w == (const void*)&s->phys) {
            btf_phys_backend_main(s, c->scratch, c->bp);
            return;
        }
    }
#endif
#if defined(__CUDA_ARCH__)
    // The serial-stage fallback can be reached from a cooperative boundary (all
    // lanes) when a warp-per-env driver is running with no backend armed for this
    // world; the stages are a serial RMW over the world, so exactly one lane may
    // run them. Inside a BT_SERIAL span the other lanes never get here anyway.
    if (btf_group_bp > 1 && !BTF_IS_LEADER()) return;
#endif
    btf_phys_stage_select(w);
}

template <class W>
BT_HD static inline void btf_phys_step_hook(W* w) { BTF_PHYS_HOOK_BODY(w); }

// ===========================================================================
// 0b. Cooperative step-scheduling dispatch: the macro block is defined ABOVE
//     (immediately before btf_phys_stage_select) because btf_phys_step_hook_default
//     uses BTF_IS_LEADER()/btf_group_bp, and the core's spans use BT_SERIAL/BT_COOP.
//     Nothing here needs to define them a second time.
// ===========================================================================

#define bt_phys_step(w) btf_phys_step_hook(w)
#include "brotato_core.h"
#undef bt_phys_step

#define BTF_ACTIONS BT_ACTIONS
#define BTF_OBS_SIZE BT_OBS_SIZE

// nvcc parses this TU's host code in the device pass too, where BT_HOST_ONLY has
// already erased the generated name resolvers. Anything resolving a profile
// token from a string is host code; the device pass needs a declaration so the
// parse succeeds. Define BTF_NO_NAME_STUBS when the includer already has them.
#ifndef BTF_NO_NAME_STUBS
#if defined(__CUDA_ARCH__)
static inline unsigned short bt_character_id_from_name(const char* name) { (void)name; return 0xFFFFu; }
static inline unsigned short bt_weapon_id_from_name(const char* name) { (void)name; return 0xFFFFu; }
static inline const char* bt_stat_display_name(int stat) { (void)stat; return "?"; }
#endif
#endif

// Address env i in a strided BtState array (works for BtState[] and for Env[],
// whose BtState is at offset 0).
#define BTF_AT(base, stride, i) \
    ((BtState*)((char*)(base) + (size_t)(i) * (size_t)(stride)))

// ===========================================================================
// 1. Lane + sync primitives
// ===========================================================================
#if defined(__CUDA_ARCH__)
#define BTF_LANE32    ((int)(threadIdx.x & 31u))
#define BTF_SLOT      ((int)(threadIdx.x >> 5))   /* per-warp scratch slice */
#define BTF_BLOCKLANE ((int)threadIdx.x)
#else
#define BTF_LANE32    (0)
#define BTF_SLOT      (0)
#define BTF_BLOCKLANE (0)
#endif

// Group member index for a group of size bp: for bp == 32 the group is one warp
// and the member index is the lane id; for bp > 32 the group is the whole block
// (whose first thread is 0).
#define BTF_MEM(bp)   (((bp) == 32) ? BTF_LANE32 : BTF_BLOCKLANE)

#define BTF_WARP_MAX 32

// Group barrier. bar.warp.sync carries memory ordering among the participating
// lanes; the block barrier implies the needed visibility for the env's global
// BtState as well. DEFINED in section 0b, before brotato_core.h, because the
// BT_SERIAL/BT_COOP bodies expand at their use site inside the core and must be
// able to name it: bp <= 1 is a no-op (the plain one-thread-per-env kernel's warp
// spans 32 different envs and threads exit early, so bar.warp.sync there is
// illegal), bp > 32 is the block barrier, bp == 32 is __syncwarp.

// Strided parallel loop over `n` items on a group of `bp` lanes.
#define BTF_FOR(i, n, mem, bp) \
    for (int i = (mem); (i) < (n); (i) += (bp))

// ===========================================================================
// 2. Scan + the ordered count/scan/write pattern
//
// Exclusive scan of `v` over a group of `bp` lanes (bp in {32,64,...,1024}).
// `mem` is the group member (BTF_MEM(bp)), `slot` the per-warp scratch slice
// (BTF_SLOT; unique within the block, so concurrent warps cannot collide).
// Returns the member's exclusive prefix; read the group total with
// btf_scan_total(sc, bp, slot).
// ===========================================================================
// Per-slot: v0/v1/wsum/woff/total are indexed by `slot` (the per-warp scratch
// slice, BTF_SLOT) so that several warps of one block running different envs can
// scan concurrently without racing. For bp > 32 the group is the whole block and
// its first warp is slot 0, so the cross-warp offsets live in slot 0's entries;
// read the group total with btf_scan_total(sc, bp, slot) rather than sc->total.
typedef struct BtfScanScratch {
    int v0[BTF_WARP_MAX][32];
    int v1[BTF_WARP_MAX][32];
    int wsum[BTF_WARP_MAX];
    int woff[BTF_WARP_MAX];
    int total[BTF_WARP_MAX];
} BtfScanScratch;

#define btf_scan_total(sc, bp, slot) \
    (((bp) == 32) ? (sc)->total[(slot)] : (sc)->total[0])

BT_HD static inline int btf_scan_excl(int v, BtfScanScratch* sc, int mem, int slot,
        int bp) {
    int nwarp = bp >> 5;
    int wg = mem >> 5;                 /* warp index inside the group */
    int lane = mem & 31;
    /* Within-warp inclusive scan, REGISTER-ONLY (Main's directive): a shuffle
     * chain, so no lane ever reads another lane's scratch store. The previous
     * Hillis-Steele form staged every round through shared v0/v1 with four group
     * barriers per round, which made every consumer (physics classify/compact/
     * level, GpuCombat's btf_count_by_key/btf_bin_by_key, GpuRng's
     * btf_draw_parallel) depend on a cross-lane RMW chain through scratch;
     * with the scratch in GLOBAL that chain was measured unreliable on sm_120
     * (WarpForkFault: non-monotone prefixes, j=-3 pool claim), and the
     * __threadfence_block() in BTF_GRP_SYNC_LANE is only an empirical workaround
     * for that. The shuffle removes the dependency for every bp; the cross-warp
     * combine below is unchanged. */
    int incl = v;
#if defined(__CUDA_ARCH__)
    for (int off = 1; off < 32; off <<= 1) {
        int t = __shfl_up_sync(0xffffffffu, incl, off);
        if (lane >= off) incl += t;
    }
#else
    (void)lane;   /* host: a one-lane group's inclusive scan is its own value */
#endif
    int excl = incl - v;
    if (lane == 31) sc->wsum[slot] = incl;
    btf_group_sync(bp);
    if (nwarp == 1) {
        if (mem == 0) { sc->woff[slot] = 0; sc->total[slot] = sc->wsum[slot]; }
        btf_group_sync(bp);
        return excl;
    }
    // cross-warp: one lane scans the per-group-warp totals (<= 32). For bp > 32
    // the group starts at block lane 0, so wg == slot and warp w wrote wsum[w].
    if (wg == 0 && (mem & 31) == 0) {
        int acc = 0;
        for (int w = 0; w < nwarp; w++) {
            sc->woff[w] = acc;
            acc += sc->wsum[w];
        }
        sc->total[0] = acc;
    }
    btf_group_sync(bp);
    return excl + sc->woff[wg];
}

// ---------------------------------------------------------------------------
// ORDERED COMPACTION PATTERN (used by the broadphase and by every enumeration
// that the serial core performs by appending in scan order).
//
// The serial bt_phys_update_pairs appends live pair indices to active_pairs in
// strictly ascending pair-index order; any parallel replacement must reproduce
// that list and its count EXACTLY. GpuSolver's measured implementation, which
// this framework endorses over a flags staging array:
//
//   member m owns the CONTIGUOUS chunk [m*chunk, (m+1)*chunk) of [0,n)
//   (contiguous chunks -- not strided -- are what makes the output ordered);
//   pass 1  count = number of predicate-true items in the chunk; btf_scan_excl
//   pass 2  re-walk the chunk writing each true item at base + local_prefix
// The chunk walk evaluates the predicate itself (no npairs-sized staging array),
// so the predicate costs ~2x flops and the O(n^2) scan is not the bottleneck.
//
// A shared/compact SoA predicate array is still worth it when the predicate is a
// single load probe (the broadphase stamp test): the serial scan reads
// pairs[i].stamp with a 204-byte stride (one 32B sector per pair, ~1.1 MB/env/
// tick over 8646 pairs), while a compacted stamp[] array is fully coalesced
// (~34 KB/env/tick). That layout change is PerfMemory + GpuSolver's call; the
// ordered compaction above is what keeps it bit-exact either way.
// ---------------------------------------------------------------------------

// ===========================================================================
// 3. Wavefront runner (order-preserving DAG levelization)
//
// level(op) = 1 + max(last[a], last[b]) over the MUTABLE bodies a,b the op
// touches (a body with inv_mass == 0 never changes value -- apply_impulse adds
// j*0 -- so it contributes no edge; verified for the four STATIC walls in
// bt_phys_pair_setup/solve). Two ops at the same level share no writable object,
// so executing level by level, any order inside a level, is a topological order
// of the per-object dependency DAG and yields byte-identical results.
//
// GpuSolver measured that re-scanning level_of per level costs a group sync per
// level per pass (9 passes x ~50 levels on a dense crowd) and therefore buckets
// ops by level in scratch instead. This runner is the simple reference form;
// the bucketed form is btf_wavefront_run_bucketed's job in physics.cuh.
// ===========================================================================
template <typename ExecFn>
BT_HD static inline void btf_wavefront_run(const unsigned short* level_of, int nops,
        int nlevels, int mem, int bp, ExecFn exec) {
    for (int L = 1; L <= nlevels; L++) {
        BTF_FOR(k, nops, mem, bp) if ((int)level_of[k] == L) exec(k);
        btf_group_sync(bp);
    }
}

// ===========================================================================
// 4. Parallel RNG draws (pcg32 jump-ahead)
//
// Required symbol, provided by ocean/brotato/brotato_rng_jump.h (GpuRng), whose
// definition is `BT_HD static BtRng btf_rng_advance(BtRng, bt_u64)`. There is
// EXACTLY ONE declaration here and it uses the definer's specifier (non-inline):
// a second, inline declaration after that definition is ill-formed and makes
// every call ambiguous.
//
// A TU that never instantiates the draw runner and does not carry the jump
// header (brotato.cu, which excludes the combat header) defines
// BTF_NO_RNG_JUMP_DECL before including this header: the declaration and the
// runner below are then dropped, so the TU ends with no declared-but-undefined
// static. Consumers that DO use the runner (GpuSpawn, GpuRng, GpuCombat and the
// benches) leave the macro undefined and include brotato_rng_jump.h themselves.
//
// A phase qualifies for this runner iff, for every consumer i in [0,n):
//   (a) consumer i's draw COUNT is a function of pre-phase state only -- not of
//       a value drawn by another consumer in this phase and not of state another
//       consumer mutates in this phase, and
//   (b) the drawn values only affect consumer i's own outputs for the rest of
//       the phase; cross-consumer effects must go through the ordered apply.
// Then: pass 1 counts (parallel) -> exscan -> pass 2 each consumer advances a
// private stream copy by its offset, draws, writes its own outputs -> finally
// *rng = btf_rng_advance(*rng, total). Values AND final stream state match the
// serial in-order draws because a PCG32 output depends only on (state, inc).
// ===========================================================================
#ifndef BTF_NO_RNG_JUMP_DECL
BT_HD static BtRng btf_rng_advance(BtRng r, bt_u64 delta);

template <typename CountFn, typename DrawFn>
BT_HD static inline unsigned long long btf_draw_parallel(BtRng* rng,
        unsigned int* counts, int n, BtfScanScratch* sc, int mem, int slot, int bp,
        CountFn count_fn, DrawFn draw) {
    int chunk = (n + bp - 1) / bp;
    int lo = mem * chunk, hi = lo + chunk;
    if (hi > n) hi = n;
    unsigned int sum = 0;
    for (int i = lo; i < hi; i++) { unsigned int c = count_fn(i); counts[i] = c; sum += c; }
    int off = btf_scan_excl((int)sum, sc, mem, slot, bp);
    BtRng local = btf_rng_advance(*rng, (bt_u64)off);
    for (int i = lo; i < hi; i++) {
        unsigned int c = counts[i];
        for (unsigned int j = 0; j < c; j++) draw(i, (int)j, &local);
    }
    btf_group_sync(bp);
    unsigned long long total = (unsigned long long)btf_scan_total(sc, bp, slot);
    if (mem == 0) *rng = btf_rng_advance(*rng, total);
    btf_group_sync(bp);
    return total;
}
#endif // BTF_NO_RNG_JUMP_DECL

// ---------------------------------------------------------------------------
// ORDERED APPLY PATTERN (contract only; no primitive is exported for it yet).
//
// Parallel phases compute effect records (damage, loot, death, spawn, ...).
// Where an effect mutates shared state the serial core applies them in a fixed
// order; float updates are not associative, so the apply must be reproduced:
//   record -> key (target identity) + producing order (the record's index when
//   the producer writes records in the serial loop's order)
//   stable counting sort by (key, record index); per key, apply in that order.
//
// NOT exported as a helper on purpose: a naive `atomicAdd(&cursor[key],1)`
// binning is NOT stable under a strided lane mapping -- the atomic order is
// nondeterministic, so items of one key land in a nondeterministic order and the
// apply order can silently differ from the serial one. A correct parallel form
// needs per-key ranking (blocked chunks + per-chunk per-key histograms, or a
// sequential per-key rank), and most phases here do not need binning at all
// because the producer keeps the mutating body serial (see GpuCombat: parallel
// predicate scan, lane 0 runs the original core body). If a phase genuinely
// needs a parallel ordered apply, add it here with its ranking scheme and a
// serial-vs-parallel digest test; do not improvise one at a call site.
// ---------------------------------------------------------------------------

// ===========================================================================
// 6. Verification contract
//
// The harness (tests/bench_brotato_cuda.cu) verifies every prototype bit-exactly
// by, per tick: snapshot the env BtState, run the SERIAL bt_core_step on one copy
// and the parallel schedule on another from identical prior state/actions, then
// compare FNV-1a digests over the mutated regions AND the whole struct. A
// prototype must not change any unmutated byte either -- a stray write is a
// failure, not a gain.
// ===========================================================================
BT_HD static inline unsigned int btf_fnv1a(const unsigned char* p, size_t n,
        unsigned int h) {
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned int)p[i];
        h *= 16777619u;
    }
    return h;
}

// Digest of the whole physics world. Layout-agnostic on purpose: the world was
// refactored from a dense pairs[] table to a compact pool (pair_map + pool_free
// + active_pairs of pool slots) by PerfMemory/GpuSolver, and a field list would
// go stale. Serial-vs-parallel copies start as byte-identical memcpys, so a
// whole-struct hash is a valid equality test (padding included).
template <class W>
BT_HD static inline unsigned int btf_world_digest(const W* w) {
    return btf_fnv1a((const unsigned char*)w, sizeof(W), 2166136261u);
}

BT_HD static inline unsigned int btf_state_digest(const BtState* s) {
    return btf_fnv1a((const unsigned char*)s, sizeof(BtState), 2166136261u);
}

// ===========================================================================
// 7. Phase map of bt_core_step (the schedule the parallel kernels must preserve)
//
// Order is the call order of bt_core_step. PAR = the intra-env parallelism the
// phase admits without changing any per-object mutation order.
//
//  #  phase                          object space        PAR kind      notes
//  -- ------------------------------ ------------------- ------------- ----------------
//  1  bt_birth_flush                births[] (<=256)    wide
//  2  bt_enemy_pool_step            pool descriptors    wide + scan   ordered free-list pop
//  3  bt_neutral_uid_step           neutral uid ring    wide
//  4  bt_phys_step                  bodies + pairs      wavefront     GpuSolver; scan is
//                                                                     ordered compaction
//  5  enemy position snapshot       ealive[100]         wide
//  6  player movement               scalar              serial
//  7  regen/iframe/link refresh     scalar              serial
//  8  scalar hot/lose_hp/alien_eyes item rows           wide          RNG: per-row U draws
//  9  bt_spawn_poll                 queues (<=512)      wide/ordered  drains -> births
// 10  bt_birth_advance              births[]            wide
// 11  bt_fire_groups                group table         serial-ish    RNG: spawn timing
// 12  enemy AI loop                 ealive[100]         wavefront*    RNG: cd + target picks
// 13  wpn_dirty -> weapon stats     weapons[]           wide
// 14  bt_wproj_step                 projectiles         wavefront*
// 15  bt_melee_step                 melee slots         wavefront*
// 16  bt_ranged_step                ranged slots        wavefront*
// 17  charmed contact loop          enemy pairs         wide + apply
// 18  projectile loop               proj x enemies      wide + apply
// 19  consumables                   cons[]              wide
// 20  player contact damage         scalar              serial        RNG: dodge
// 21  death check                   enemies             ordered        RNG: loot rolls
// 22  pet/structure motion          own BtPhysWorld     wavefront
// 23  structure spawning/landmine   structures[]        wide
// 24  burn ticks                    ealive[100]         wide + apply
// 25  bt_core_observe               -> obs[161]         wide
//
//  * wavefront only where the loop body writes just its own entity's slot; any
//    enumeration above the loop (ordinal/allocation counters) and any immediate
//    cross-entity damage must become records + the ordered apply. That
//    conversion is GpuCombat's deliverable; btf_bin_by_key is its half.
//
//  NON-COMBAT EXITS: bt_core_step leaves through an early return before phase 1
//    on a shop frame (phase 1), a level-up frame (phase 2) and the GO frame's
//    transition tick (shop_go_pending), which builds the next wave's arena with
//    one frame of wave clock and no arena work at all -- the freshly loaded Main
//    scene's first _physics_process is the NEXT tick (see that block's comment).
//    Every one of the three guards reads only shared state, so the exit is
//    warp-uniform; a lane-scheduled kernel must take it as one unit and must not
//    run any phase below for that tick.
// ===========================================================================
#define BTF_PHASE_COUNT 25

// DRIFT NOTE (FrameOrder landed this after the table above was written): the
// player/enemy contact APPLY half is now a separate serial section
// BT_CONTACT_APPLY_PHASE placed immediately BEFORE BT_ITEMSFX_PHASE, while
// BT_CONTACT_PHASE (probe) keeps its old position. So the row 20/24 area has
// split into (probe, apply) around the itemsfx phase; the number of serial
// sections is unchanged. Any kernel that reorders work relative to the tick must
// respect these two hook boundaries, not the old single contact block.

#endif // PUFFER_BROTATO_GPU_FRAMEWORK_CUH
