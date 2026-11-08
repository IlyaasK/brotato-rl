// brotato_gpu_itemsfx.cuh -- intra-environment GPU schedule for the item /
// structure / burning / explosion phase of one Brotato combat tick.
// OWNER: GpuItemsFx.
//
// WHAT THIS SCHEDULES (two hooks, both in bt_core_step's own order):
//   BT_TIMER_PHASE (the frame's Timer rank, before the group pass):
//     0. bt_burn_tick                one burning tick per burning enemy
//   BT_ITEMSFX_PHASE (after the group pass and the area hits):
//     1. bt_structure_spawning_tick  Garden's is_spawning turrets emit a fruit
//     2. bt_item_expl_landmine_tick  the landmines / BotOMine trigger Area2D
//     3. bt_item_expl_tick           the 3-frame item-explosion Hitbox pool
// (Alien Eyes volleys and the temporal/scalar player events are NOT part of these
// blocks; see "SCOPE" below.)
//
// THE ONE RULE (framework section 6)
//   Every kernel here is bit-exact against the CPU core because it only
//   RE-SCHEDULES calls to the very same BT_HD functions -- nothing is
//   re-implemented:
//     * the order-FREE half of each phase (which targets a Hitbox overlaps,
//       which structures are ready, which enemies burn) is precomputed in
//       parallel by the core's own *_scan_range functions;
//     * the ORDER-LOCKED half (RNG draws, damage, kills, loot, pool-slot
//       allocation, audio submission) is run by ONE lane per env in the core's
//       own per-item functions (*_cand / bt_burn_tick_one), in the serial
//       order. A parallel backend never reorders a mutation of a shared object.
//
// WHY THE SPLIT IS EXACT
//   * bt_item_expl_scan_range evaluates, for every (explosion, target slot)
//     pair, the SAME Hurtbox-circle test and the SAME uid/hit gating the serial
//     tick evaluates inline; bt_item_expl_tick_cand then reads that predicate as
//     a bit. The candidate set is SLOT-keyed (BT_EXPL_CAND_WORDS per explosion),
//     target slot being unique within one tick.
//   * the scan mutates nothing but the caller's mask (and the object_uid_overflow
//     flag, which it can only ever raise to 1, exactly like the serial loop).
//   * bt_structure_spawning_tick_cand / bt_item_expl_landmine_tick_cand /
//     bt_burn_tick_cand read a precomputed mask in place of their inline
//     predicate, and still do all their state writes (timer decrement, pressed
//     flag, burn countdown) exactly once, in item order.
//   * therefore: same draws, same order, same bytes. Verified per tick by
//     digest comparison (tests/test_brotato_gpu_itemsfx.cu).
//
// FRAME RELATION (must be preserved by any schedule)
//   Both hooks keep the neighbours the serial core gives them:
//     ... birth advance / fire groups ...
//     BT_TIMER_PHASE(s, r)                            <-- THIS FILE (burn rank;
//                                                         Timer rank of the
//                                                         frame, BEFORE the
//                                                         group pass)
//     ... enemy AI / projectiles / melee / consumables ...
//     BT_CONTACT_APPLY_PHASE(s, r)                    (deferred contact hit --
//                                                      the Bull/Demon
//                                                      explode_on_hit node it
//                                                      spawns must exist BEFORE
//                                                      the itemsfx phase's age-0
//                                                      promotion; see
//                                                      brotato_core.h)
//     BT_ITEMSFX_PHASE(s, r)                          <-- THIS FILE (garden /
//                                                         landmine / explosion;
//                                                         its bt_item_expl_tick
//                                                         drain ends the frame's
//                                                         kill queues, Timer rank
//                                                         first)
//     TempStatsPerInterval / enemy-hit-effect ticks
//   So: burn rank -> ... -> contact apply -> itemsfx phase, in one frame.
//   The burn tick's queued kills are drained by bt_item_expl_tick_cand (inside
//   the itemsfx phase), so a backend must not add a drain of its own.
//
// LANE MODEL
//   A lane group of `bp` lanes (framework: bp == 32 is one warp, bp > 32 the
//   whole block) owns ONE env. Lane `mem` runs bt_core_step for that env; at the
//   phase hook all lanes cooperate, then lane 0 runs the ordered apply. The
//   phase's kill reward is broadcast to every lane because bt_core_step folds
//   `r` into s->episode_return -- a per-lane difference would fork the state.
//
// INTEGRATION (the consumer supplies the hooks, exactly like BTF_PHYS_HOOK_BODY)
//   #define BTFX_SCRATCH_OF(s) ((BtfxScratch*)((char*)(s) + MY_ENV_SLOT - sizeof(BtfxScratch)))
//   #define BTFX_HOOK_ITEMSFX_PHASE 1      // defines BT_ITEMSFX_PHASE
//   #define BTFX_HOOK_TIMER_PHASE   1      // defines BT_TIMER_PHASE (the burn rank)
//   #include "ocean/brotato/brotato_gpu_itemsfx.cuh"
// `btfx_scratch_bytes()` is the per-env scratch size to reserve. The scratch must
// be ZEROED once at allocation (only the masks the scans write are re-zeroed
// each tick; the rest is unused). BTFX_BP selects the group size (default 32).
// Set only BTFX_HOOK_TIMER_PHASE to parallelise the burn rank alone; the default
// expansion of BT_TIMER_PHASE is bt_burn_tick, exactly as the core has it.
//
// SCOPE -- what is deliberately NOT here, and why:
//   * Alien Eyes volleys (bt_scalar_alien_eyes_tick): the volley is written into
//     the projectile pool through bt_spawn_projectile's first-free-slot scan, so
//     its ORDER is the row/k order -- it is pure ordered apply. The per-row
//     parameter computation is O(rows owned) and runs only on the timer's own
//     frame, so there is no order-free work worth moving off the critical path.
//     A backend covers it by calling the core function on the ordered lane.
//   * temporal/scalar player events (bt_temporal_set_moving, bt_scalar_lose_hp_
//     tick): scalar transitions of player state; no per-item object space.
//
// MEASUREMENT
//   tests/test_brotato_gpu_itemsfx.cu reports, for a scripted Demon (Danger 5)
//   state with many structures and explosions: the per-tick serial phase cost vs
//   the device phase cost, and the fraction of the phase's predicate work moved
//   off the ordered path. Bit-exactness is asserted first; a timing is only
//   printed for a schedule that already passed the digest comparison.
#ifndef PUFFER_BROTATO_GPU_ITEMSFX_CUH
#define PUFFER_BROTATO_GPU_ITEMSFX_CUH

#include <cuda_runtime.h>
#include <cstdint>
#include <cstdio>

#ifndef BT_CUDA
#define BT_CUDA 1
#endif
// BT_HD / BT_DEVICE / BT_CONST_PTR, before anything else is parsed.
#include "brotato_rng.h"

// ---------------------------------------------------------------------------
// Pre-core declarations.
//
// bt_core_step expands BT_ITEMSFX_PHASE and BT_TIMER_PHASE, which the consumer
// may already have pointed at the hooks below; those declarations therefore have
// to be visible before brotato_core.h is parsed.
//
// BT_TIMER_PHASE is the Timer rank of the frame (unit.tscn's BurningTimer runs in
// SceneTree::iteration's "physics_process_internal" pass, BEFORE the group pass
// and before that iteration's deferred flush): bt_core_step calls it just before
// its group pass, and it is NOT part of bt_itemsfx_phase_serial. The default is
// bt_burn_tick; BTFX_HOOK_TIMER_PHASE substitute the parallel burn (the same
// per-enemy body, run by one lane) plus its order-free tick scan.
// ---------------------------------------------------------------------------
struct BtState;
typedef struct BtfxScratch BtfxScratch;

// Per-env scratch address inside the env slot (REQUIRED when the hook is used;
// the default is unusable and only exists so a plain include parses).
#ifndef BTFX_SCRATCH_OF
#define BTFX_SCRATCH_OF(s) ((BtfxScratch*)0)
#endif
// Lane group size of the driving step kernel.
#ifndef BTFX_BP
#define BTFX_BP 32
#endif
// Slot of a lane inside its group (framework: warp id for bp == 32, ignored for
// bp > 32).
#ifndef BTFX_SLOT
#define BTFX_SLOT ((int)(threadIdx.x >> 5))
#endif

BT_HD static void btfx_itemsfx_phase_hook(struct BtState* s, float* r);
BT_HD static void btfx_burn_phase_hook(struct BtState* s, float* r);

#ifdef BTFX_HOOK_ITEMSFX_PHASE
#ifndef BT_ITEMSFX_PHASE
#define BT_ITEMSFX_PHASE(s, r) btfx_itemsfx_phase_hook((s), (r))
#endif
#endif
#ifdef BTFX_HOOK_TIMER_PHASE
#ifndef BT_TIMER_PHASE
#define BT_TIMER_PHASE(s, r) btfx_burn_phase_hook((s), (r))
#endif
#endif

// The shared framework: BT_HD primitives, the lane model, and the full core
// (brotato_state.h + brotato_core.h, whose bt_core_step calls BT_ITEMSFX_PHASE).
#include "brotato_gpu_framework.cuh"

// ---------------------------------------------------------------------------
// Scratch
// ---------------------------------------------------------------------------
#define BTFX_PET_WORDS   ((BT_MAX_PET + 31) / 32)
#define BTFX_ENEMY_WORDS ((BT_MAX + 31) / 32)

// One per env. The four masks are the entire output of the order-free pass; the
// ordered apply reads them and touches nothing else here.
typedef struct BtfxScratch {
    unsigned int struct_ready[BTFX_PET_WORDS];                 // bt_structure_spawning_scan
    unsigned int lm_inside[BTFX_PET_WORDS];                    // bt_item_expl_landmine_scan
    unsigned int burn_tick[BTFX_ENEMY_WORDS];                  // bt_burn_scan
    unsigned int expl_cand[BT_MAX_EXPL * BT_EXPL_CAND_WORDS];   // bt_item_expl_scan
} BtfxScratch;

// Host-side allocation helper (also the per-env slot padding to reserve).
static inline size_t btfx_scratch_bytes(void) { return sizeof(BtfxScratch); }

#define BTFX_CAND_WORDS (BT_MAX_EXPL * BT_EXPL_CAND_WORDS)

// ---------------------------------------------------------------------------
// The order-free passes.
//
// PARTITIONING RULE: every mask WORD has exactly one writer, so no atomic and no
// scheduling nondeterminism is possible. The core's scan writers are plain `|=`,
// so a lane must never share a word with another lane:
//   * the pet spaces are chunked BY MASK WORD (one lane owns 32 pet slots, whose
//     bits all land in that lane's own word);
//   * the burn space is chunked the same way over enemy slots;
//   * the explosion candidate set is chunked by explosion SLOT (a whole explosion
//     owns its BT_EXPL_CAND_WORDS words), and its overlap tests are independent
//     per explosion.
// The masks are zeroed here, each tick: the previous tick's bits must never leak
// into this one.
//
// The burn mask is scanned by btfx_burn_scans, NOT here: the burning tick belongs
// to the frame's Timer rank (BT_TIMER_PHASE), which bt_core_step runs before its
// group pass, well before this phase.
// ---------------------------------------------------------------------------
__device__ inline void btfx_burn_scans(BtState* s, BtfxScratch* sc, int mem, int bp) {
    BTF_FOR(w, BTFX_ENEMY_WORDS, mem, bp) sc->burn_tick[w] = 0u;
    btf_group_sync(bp);
    // enemies (burn): one lane per 32-slot mask word, so no word is shared
    BTF_FOR(w, BTFX_ENEMY_WORDS, mem, bp) {
        int lo = w * 32, hi = lo + 32;
        if (hi > BT_MAX) hi = BT_MAX;
        bt_burn_scan_range(s, sc->burn_tick, lo, hi);
    }
    btf_group_sync(bp);
}

__device__ inline void btfx_itemsfx_scans(BtState* s, BtfxScratch* sc, int mem, int bp) {
    BTF_FOR(w, BTFX_PET_WORDS, mem, bp) sc->struct_ready[w] = 0u;
    BTF_FOR(w, BTFX_PET_WORDS, mem, bp) sc->lm_inside[w] = 0u;
    BTF_FOR(w, BTFX_CAND_WORDS, mem, bp) sc->expl_cand[w] = 0u;
    btf_group_sync(bp);

    // pets: 32 slots per word, so a lane's [lo,hi) touches only its own words
    BTF_FOR(w, BTFX_PET_WORDS, mem, bp) {
        int lo = w * 32, hi = lo + 32;
        if (hi > BT_MAX_PET) hi = BT_MAX_PET;
        bt_structure_spawning_scan_range(s, sc->struct_ready, lo, hi);
        bt_item_expl_landmine_scan_range(s, sc->lm_inside, lo, hi);
    }
    // explosions: a lane owns whole explosion slots (hence whole cand words)
    BTF_FOR(i, BT_MAX_EXPL, mem, bp) bt_item_expl_scan_range(s, sc->expl_cand, i, i + 1);

    btf_group_sync(bp);
}

// ---------------------------------------------------------------------------
// The burn phase: the Timer rank's ordered apply (bt_burn_tick_cand) plus its
// order-free tick scan. Wire it through BT_TIMER_PHASE (BTFX_HOOK_TIMER_PHASE).
// ---------------------------------------------------------------------------
__device__ inline void btfx_burn_phase(BtState* s, float* reward, BtfxScratch* sc,
                                       int mem, int slot, int bp) {
    __shared__ float rbuf[BTF_WARP_MAX];
    btfx_burn_scans(s, sc, mem, bp);
    float delta = 0.0f;
    if (mem == 0) bt_burn_tick_cand(s, sc->burn_tick, &delta);
    int cell = (bp == 32) ? slot : 0;
    if (mem == 0) rbuf[cell] = delta;
    btf_group_sync(bp);
    *reward += rbuf[cell];
    btf_group_sync(bp);
}

// ---------------------------------------------------------------------------
// The whole phases: order-free pass in parallel, then the ordered apply.
//
// `mem`/`slot`/`bp` are the framework lane coordinates (BTF_MEM / BTF_SLOT).
// `reward` is this lane's reward accumulator; every lane's copy receives the
// phase's kill reward (see the header note), so all lanes stay in lockstep for
// bt_core_step's `s->episode_return += r`.
//
// The burning tick is NOT here: it is the Timer rank (BT_TIMER_PHASE), which
// bt_core_step runs before its group pass. Its queued kills are drained by
// bt_item_expl_tick_cand at the end of THIS phase, exactly as in the serial core.
// ---------------------------------------------------------------------------
__device__ inline void btfx_itemsfx_phase(BtState* s, float* reward, BtfxScratch* sc,
                                         int mem, int slot, int bp) {
    // Per-group broadcast cell for the phase reward. Static shared inside a
    // device function: one array per kernel, indexed by the group's slot.
    __shared__ float rbuf[BTF_WARP_MAX];

    btfx_itemsfx_scans(s, sc, mem, bp);

    float delta = 0.0f;
    if (mem == 0) {
        // Ordered apply: the core's own per-item functions, in the core's order.
        bt_structure_spawning_tick_cand(s, sc->struct_ready);
        (void)bt_item_expl_landmine_tick_cand(s, sc->lm_inside);
        (void)bt_item_expl_tick_cand(s, &delta, sc->expl_cand);
    }
    int cell = (bp == 32) ? slot : 0;
    if (mem == 0) rbuf[cell] = delta;
    btf_group_sync(bp);
    *reward += rbuf[cell];
    btf_group_sync(bp);
}

// ---------------------------------------------------------------------------
// Serial reference for callers that want the phase without a lane group (tests,
// the CPU side of a parity comparison).
// ---------------------------------------------------------------------------
BT_HD static void btfx_itemsfx_phase_ref(BT_DEVICE BtState* s, BT_THREAD float* r) {
    bt_itemsfx_phase_serial(s, r);
}

// ---------------------------------------------------------------------------
// Hook body: what BT_ITEMSFX_PHASE expands to when BTFX_HOOK_ITEMSFX_PHASE is
// set. The host pass (which also parses this function for the CPU reference)
// must keep the plain serial phase, exactly like the framework's phys hook.
// ---------------------------------------------------------------------------
#ifdef BTFX_HOOK_ITEMSFX_PHASE
BT_HD static void btfx_itemsfx_phase_hook(struct BtState* s, float* r) {
#if defined(__CUDA_ARCH__)
    btfx_itemsfx_phase(s, r, BTFX_SCRATCH_OF(s), BTF_MEM(BTFX_BP), BTFX_SLOT, BTFX_BP);
#else
    bt_itemsfx_phase_serial(s, r);
#endif
}
#endif

// ---------------------------------------------------------------------------
// Hook body: what BT_TIMER_PHASE expands to when BTFX_HOOK_TIMER_PHASE is set --
// the frame's Timer rank, called by bt_core_step BEFORE its group pass. Same
// shape as the itemsfx hook: the device pass runs the lane-group burn (order-free
// tick scan + one-lane bt_burn_tick_cand), the host pass keeps the serial burn so
// the CPU reference is unchanged.
// ---------------------------------------------------------------------------
#ifdef BTFX_HOOK_TIMER_PHASE
BT_HD static void btfx_burn_phase_hook(struct BtState* s, float* r) {
#if defined(__CUDA_ARCH__)
    btfx_burn_phase(s, r, BTFX_SCRATCH_OF(s), BTF_MEM(BTFX_BP), BTFX_SLOT, BTFX_BP);
#else
    bt_burn_tick(s, r);
#endif
}
#endif

#endif // PUFFER_BROTATO_GPU_ITEMSFX_CUH
