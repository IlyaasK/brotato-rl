// brotato_gpu_warp.cuh -- integrated warp-per-env driver for the parallel
// Brotato tick. OWNER: PerfGpuKernel.
//
// One WARP per environment (blockDim = 32, so BTF_SLOT == 0 and no two warps of
// a block share the per-kernel dispatch globals), the peer modules armed through
// their own hooks, and bt_core_step as the only definition of the tick:
//
//   - physics:    brotato_gpu_physics.cuh (GpuSolver) via the framework's
//                 RUNTIME backend dispatch (btf_phys_backend = 1). The wavefront
//                 schedule is a topological order of the per-body dependency
//                 DAG, verified bit-exact against serial bt_phys_step.
//   - combat:     brotato_gpu_combat.cuh (GpuCombat) if present. Its phase hooks
//                 are inert until btf_combat_set_context is called, and this
//                 driver deliberately does NOT arm them (their context is a
//                 per-kernel device global; arming it here would need one
//                 context per env). Include it first because it must define its
//                 phase macros before brotato_core.h is parsed.
//   - spawn/obs:  brotato_gpu_spawn.cuh / brotato_gpu_observe.cuh arm through
//                 their own entry points; not attached here (see the note in
//                 the report).
//
// Scratch: GpuSolver's BtgpScratch is ~50 KB per env in warp mode (btgp_scratch_bytes()),
// so it lives in GLOBAL memory, one block-sized region per env: with one warp per
// block that is exactly one btgp_scratch_bytes() region per block.
//
// Integration into brotato.cu: include this header after the core, allocate
// n * btgp_scratch_bytes() once in puf_vec_create, set btf_phys_backend = 1 in
// puf_step, and launch btfw_step_kernel instead of bt_gpu_step_kernel. The Env
// stride argument is sizeof(Env) so &envs[0].state works unchanged.
#ifndef PUFFER_BROTATO_GPU_WARP_CUH
#define PUFFER_BROTATO_GPU_WARP_CUH

// Decide the physics backend availability BEFORE anything includes the
// framework (combat pulls it in), so BTF_PHYS_HAVE_BACKEND and the backend
// wrappers are consistent: the macro must only be on when the backend header
// really exists, otherwise the framework declares entry points nobody defines.
#if defined(__has_include)
#  if __has_include("brotato_gpu_physics.cuh") && !defined(BTFW_NO_PHYSICS)
#    define BTFW_HAVE_PHYSICS 1
#  endif
#endif
#if defined(BTFW_HAVE_PHYSICS) && !defined(BTF_PHYS_HAVE_BACKEND)
#define BTF_PHYS_HAVE_BACKEND 1
#endif

#if defined(__has_include)
#  if __has_include("brotato_gpu_combat.cuh") && !defined(BTFW_NO_COMBAT)
#    define BTFW_HAVE_COMBAT 1
#    include "brotato_gpu_combat.cuh"     /* defines its phase hooks, then the framework */
#  endif
#endif

/* Without the combat header this TU never instantiates the parallel draw runner,
 * so drop the framework's btf_rng_advance declaration + runner: that is what
 * keeps brotato.cu free of a declared-but-undefined static rng helper (and of the
 * btf_draw_parallel code it does not use). TUs that DO use the runner include the
 * combat header (or brotato_rng_jump.h) themselves, so the macro stays undefined
 * there. */
#if !defined(BTFW_HAVE_COMBAT) && !defined(BTF_NO_RNG_JUMP_DECL)
#define BTF_NO_RNG_JUMP_DECL 1
#endif

#include "brotato_gpu_framework.cuh"        /* no-op if combat already pulled it in */

#if defined(BTFW_HAVE_PHYSICS)
#  include "brotato_gpu_physics.cuh"
#endif

// The runtime-dispatch wrappers btf_phys_backend_main/_pet are owned by
// brotato_gpu_physics.cuh (GpuSolver), which defines them under the same
// `#if BTF_PHYS_HAVE_BACKEND && !defined(BTF_PHYS_BACKEND_WRAPPERS)` guard it
// provides for its own consumers. This driver must NOT define a second copy:
// brotato.cu includes the warp header (and therefore the physics header), and a
// duplicate definition is a hard compile error. When the physics header is
// absent BTFW_HAVE_PHYSICS is undefined, so BTF_PHYS_HAVE_BACKEND is 0 and the
// framework's declarations are never ODR-used (btfw_scratch_bytes() is 0 and the
// backend is never armed) — nothing is lost by not defining them here.

// ---------------------------------------------------------------------------
// Kiting policy used to drive deep-wave fixtures (measurement only). It reads
// the player observation block written by bt_core_observe, so it carries no
// parity risk: it only chooses the action.
// ---------------------------------------------------------------------------
static __device__ int btfw_kite_action(BtState* s, const float* o) {
    if (s->phase == 1) return 8;      /* base_shop "Go" */
    if (s->phase == 2) return 0;      /* first queued upgrade */
    const int EN = 11 + 16 + 8;       /* BT_OBS_PLAYER + ITEMS + UPGRADES */
    float wx = 0.0f, wy = 0.0f;
    for (int sl = 0; sl < BT_SLOTS; sl++) {
        float dx = o[EN + sl * 5 + 0] * BT_ARENA_W;
        float dy = o[EN + sl * 5 + 1] * BT_ARENA_H;
        float d2 = dx * dx + dy * dy;
        if (d2 <= 1.0f) continue;
        float inv = 1.0f / sqrtf(d2);
        wx -= dx * inv * inv;
        wy -= dy * inv * inv;
    }
    if (s->px < 300.0f) wx += 1.0f;
    if (s->px > BT_ARENA_W - 300.0f) wx -= 1.0f;
    if (s->py < 300.0f) wy += 1.0f;
    if (s->py > BT_ARENA_H - 300.0f) wy -= 1.0f;
    int best = 0;
    float bd = -1e30f;
    for (int a = 1; a <= 8; a++) {
        float d = BT_MOVE_X[a] * wx + BT_MOVE_Y[a] * wy;
        if (d > bd) { bd = d; best = a; }
    }
    return best;
}

// Per-env scratch the driver hands to the physics backend.
//
// PRODUCTION IS HARD-DISABLED (Main's directive): this returns 0 unless the
// build defines BTFW_EXPERIMENTAL_LANE_SCHED, so brotato.cu's `[env] warp_step`
// opt-in is forced back to the plain one-thread-per-env kernel and can never
// select the cooperative tick. The gate is lifted only for a HARNESS build that
// also passes the multi-env repeated per-tick full-state proof (obs / reward /
// terminal / action mask every tick, whole BtState every 50, several repeats and
// seed salts, residency spanning many SMs). By inspection the lane contract
// holds - every serial span of bt_core_step is BT_SERIAL (lane 0 + span barrier),
// every hook phase is a BT_COOP boundary entered by all lanes, observe/add_log/
// reset are inside BT_SERIAL, outputs are written by lane 0 only and every
// control return is a function of read-only state - but inspection is not
// evidence, so the default stays closed.
BT_HD static inline size_t btfw_scratch_bytes(void) {
#if defined(BTFW_HAVE_PHYSICS) && defined(BTFW_EXPERIMENTAL_LANE_SCHED)
    return btgp_scratch_bytes();
#else
    return 0;
#endif
}

// Per-env action mask write, identical to brotato.cu's bt_gpu_write_mask: the
// trainer's mask row is BT_ACTIONS bytes from bt_action_mask plus zeroed
// reserved ids. Written once per tick, after the step, exactly like the plain
// kernel, so the trainer's action-space contract is unchanged.
static __device__ void btfw_write_mask(BtState* s, unsigned char* row, int mask_size) {
    if (row == 0 || mask_size <= 0) return;
    bt_action_mask(s, row);
    for (int a = BT_ACTIONS; a < mask_size; a++) row[a] = 0;
}

// One warp per env. H ticks per launch; use_policy replaces the action with the
// fixture policy (timing runs only). rewards/terminals/obs are [H][n] slices.
// `cycles` (optional) receives the summed clock64 cycles per env for the H ticks.
// `masks`/`mask_size` mirror the plain kernel's action-mask write (pass NULL/0
// when the trainer has not bound a mask, e.g. in benchmarks).
__global__ void btfw_step_kernel(BtState* __restrict__ base, size_t stride,
        const float* __restrict__ actions, float* __restrict__ obs,
        float* __restrict__ rewards, float* __restrict__ terminals, int n, int H,
        int use_policy, void* __restrict__ scratch,
        unsigned long long* __restrict__ cycles,
        unsigned char* __restrict__ masks, int mask_size) {
    int env = blockIdx.x;
    if (env >= n) return;
    BtState* s = BTF_AT(base, stride, env);
    float* o = obs + (size_t)env * BTF_OBS_SIZE;   /* obs is per-env in timing runs */
    // Block-local context, SINGLE WRITER + barrier (not all lanes writing the
    // same location: concurrent RMW is a data race even when the value matches).
    // The context is __shared__, so concurrent blocks cannot see each other's
    // env pointer; a __device__ global here was the review finding.
#if defined(__CUDA_ARCH__)
    if (BTF_LANE32 == 0) {
        void* sc = 0;
#  if defined(BTFW_HAVE_PHYSICS)
        sc = (char*)scratch + (size_t)blockIdx.x * btgp_scratch_bytes();
#  else
        (void)scratch;
#  endif
        btf_phys_ctx_set(s, sc, 32);
    }
    __syncwarp();   /* broadcast the context to every lane before any hook */
#endif
    unsigned long long c0 = cycles ? clock64() : 0;
    // EVERY lane runs the tick. bt_core_step's serial spans are BT_SERIAL (lane
    // 0 executes, the warp releases at the span barrier) and its cooperative
    // phases are entered by the whole warp, so the warp stays in lockstep and
    // lane 0 is the only lane that holds authoritative state and produces the
    // outputs. Non-leader lanes compute the same control decisions from the same
    // read-only state (the action from `actions`/the observation block) and write
    // nothing: the tick's control flow (shop / end-wave / reset / terminal) is a
    // pure function of state all lanes read identically.
    for (int h = 0; h < H; h++) {
        int a = use_policy ? btfw_kite_action(s, o)
                           : (int)actions[(size_t)h * (size_t)n + (size_t)env];
        float r = 0.0f, t = 0.0f;
        bt_core_step(s, a, o, &r, &t);
        if (BTF_LANE32 == 0) {
            if (rewards) rewards[(size_t)h * (size_t)n + (size_t)env] = r;
            if (terminals) terminals[(size_t)h * (size_t)n + (size_t)env] = t;
            if (masks) btfw_write_mask(s, masks + (size_t)env * (size_t)mask_size, mask_size);
            /* Same fail-closed capacity check as brotato.cu's plain kernels: the
             * warp path must not hand a truncated env to the learner either. The
             * trap aborts the kernel, so the host's next checked CUDA call fails.
             * Self-contained on purpose: the two host-visible latch words live in
             * brotato.cu, and a shared header for them is owned by
             * CudaBoundaryReview's pending change to that block. */
            unsigned long long cm = bt_capacity_mask(s);
            if (cm != 0ull) {
                printf("brotato: capacity loss (warp) env=%d wave=%d mask=0x%016llx\n",
                       env, (int)s->wave, cm);
                __trap();
            }
        }
    }
    if (cycles && BTF_LANE32 == 0) cycles[env] = clock64() - c0;
}

__global__ void btfw_init_kernel(BtState* __restrict__ base, size_t stride, int n,
        int zone, int shop_steps, unsigned short character, unsigned short weapon,
        unsigned char audio_rng, unsigned char particles_rng) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    unsigned int rng = (unsigned int)i;
    if (rng == 0u) rng = 1u;
    bt_core_init(BTF_AT(base, stride, i), (bt_u64)rng, zone, shop_steps, character, weapon,
                 audio_rng);
    BTF_AT(base, stride, i)->particles_rng = particles_rng;
}

#endif // PUFFER_BROTATO_GPU_WARP_CUH
