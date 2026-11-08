// brotato_gpu_combat.cuh -- intra-environment parallel COMBAT phases for the
// Brotato GREED core. OWNER: GpuCombat.
//
// Built on brotato_gpu_framework.cuh (lane mapping, group sync, scan /
// compaction / min primitives) and brotato_rng_jump.h (pcg32 jump-ahead). This
// file NEVER re-implements a line of simulation: every phase below re-schedules
// the very same BT_HD body the serial bt_core_step calls, so every phase is
// bit-exact against the CPU core by construction. The only thing that changes
// is WHO evaluates the per-entity predicate and in what order the shared-state
// mutation lands.
//
// INCLUSION CONTRACT
//   Include THIS header (not brotato_gpu_framework.cuh) when you want the
//   combat phases hooked. Alongside the framework's bt_phys_step hook it
//   pre-defines BT_WPROJ_PHASE / BT_EPROJ_PHASE / BT_CONTACT_PHASE before
//   including the core, so bt_core_step's three combat call sites route
//   through btf_combat_hook_*. With NO context set (btf_combat_set_context
//   never called, or bp <= 1) every hook is exactly the serial core, so
//   merely including this header changes no behaviour.
//
// ---------------------------------------------------------------------------
// THE SCHEME: "snapshot predicate scan + serial ordered resolve"
// ---------------------------------------------------------------------------
// The combat phases this file parallelises are dominated by an O(entities x
// enemies) overlap scan whose result (the FIRST enemy each entity can hit this
// step) is a pure function of the step's frozen snapshot:
//
//   pass 1 (parallel, one lane per entity)  evaluate the overlap predicate for
//           every entity at its PREDICTED post-motion centre and record
//           first_hit[i] = the lowest enemy slot it can touch (-1 = none);
//   pass 2 (serial, one lane)               run the ORIGINAL phase body with
//           the hint: it re-tests every slot from first_hit[i] on, so it lands
//           the exact same hits, in the exact same order, with the exact same
//           RNG draws and the exact same float updates as the serial core.
//
// first_hit[] is a LOWER BOUND, so pass 2 skipping the slots below it is
// sound: for such a slot the predicate was false at the snapshot, and nothing
// inside the step can make it true (enemy positions are frozen for the whole
// step; `ealive`/the projectile ignore-set only lose entries; charm is only
// ever SET). This keeps the RNG-bearing, order-dependent mutation (hp float
// subtractions, kills, pierce/bounce counts, loot spawns, pool slots, weapon
// breaks) in the serial core's own order -- no reordering, no reassociation.
//
// The predicted centre is computed with the SAME FP expressions the body uses
// (x + vx*DT), so pass 1 and pass 2 agree bit-for-bit; the body still writes
// the state, pass 1 never does.
//
// ---------------------------------------------------------------------------
// PHASE CLASSIFICATION (which phases this file does / does not parallelise)
// ---------------------------------------------------------------------------
//  #14 bt_wproj_step   PARALLEL (this file): scan hint. The hit APPLICATION
//      (bt_item_expl_hit_enemy, burn/crit rolls, on-hit children, loot, weapon
//      break) stays on one lane in slot order -- it is class (b)/(c) RNG.
//  #18 enemy projectile loop PARALLEL (this file): scan hint. Enemy->player
//      damage and friendly->enemy damage stay serial.
//  #20 player/enemy contact  PARALLEL (this file): the PROBE is an order-free
//      scan (lowest slot wins) -- fully parallel. The queued hit's APPLY
//      (bt_player_damage, RNG dodge) stays serial; after the frame-order change
//      it runs mid-step through BT_CONTACT_APPLY_PHASE, so this phase's own
//      lane-0 apply is then a no-op (epend is already -1) and the phase reduces
//      to the probe.
//  #12 enemy AI        SERIAL. The per-enemy body draws RNG at several values
//      (state-escalation init_cd, shoot cd, charge/spawn positions, boost) AND
//      appends to ordered shared structures (the enemy-projectile pool via
//      bt_fire_shot, the summon queue, and other enemies' boost state via
//      bt_buffer_boost). A pass-1 count function cannot see the shoot branch's
//      1-vs-2-word randf draws or the retarget-behavior draws inside
//      bt_enemy_move, so the phase is class (b)/(c) and must keep its serial
//      order. (GpuRng supplies the shoot-branch count, but the phase's total is
//      not a pre-state function.)
//  #15 melee step      SERIAL. The slot loop is INHERENTLY serial across slots:
//      a break_on_hit removal compacts wpn_count and every later slot reads the
//      post-removal inventory, and each swing start/landing draws RNG.
//  #16 ranged step     SERIAL for the same reason (RNG per shot; inventory
//      mutations).
// ---------------------------------------------------------------------------
#ifndef PUFFER_BROTATO_GPU_COMBAT_CUH
#define PUFFER_BROTATO_GPU_COMBAT_CUH

// State + RNG prelude first (BtState, BT_MAX, BT_MAX_WPROJ, BT_HD); the core
// itself comes in through the framework below, and brotato_rng_jump.h (which
// needs the core's math helpers) comes after that.
#include "brotato_rng.h"
#include "brotato_state.h"
// ---------------------------------------------------------------------------
// Per-env scratch. One instance per cooperating env (warp or block).
// ---------------------------------------------------------------------------
/* first_hit[] must cover BOTH projectile phases: #14 indexes BT_MAX_WPROJ
 * entries and #18 indexes BT_MAX_PROJ. The two capacities are independent (a
 * peer may raise BT_MAX_PROJ past BT_MAX_WPROJ), so size to the larger. */
#ifndef BTF_COMBAT_MAX_FIRST_HIT
#define BTF_COMBAT_MAX_FIRST_HIT (BT_MAX_PROJ > BT_MAX_WPROJ ? BT_MAX_PROJ : BT_MAX_WPROJ)
#endif
typedef struct BtfCombatArena {
    float reward;                        /* accumulated phase reward (R_KILL/R_DMG) */
    int   first_hit[BTF_COMBAT_MAX_FIRST_HIT]; /* #14/#18 candidate hint (-1 = none) */
    unsigned char probe[BT_MAX];         /* #20 contact candidate flags */
    int   sred[1024];                    /* group min-reduction scratch (max block lanes) */
} BtfCombatArena;

// ---------------------------------------------------------------------------
// Deployment contract: WARP-PER-ENV, bp = 32.
//
// The ONLY cross-kernel state is the address of the per-env arena ARRAY, set
// from the HOST with btf_combat_set_arena() (cudaMemcpyToSymbol) before the
// launch -- never written by a kernel, so there is no race. Inside the kernel
// every hook derives its own coordinate from the launch geometry, exactly as the
// framework's BTF_WARP_PER_ENV mapping defines it:
//
//     lane32 = threadIdx.x & 31
//     env    = blockIdx.x * (blockDim.x >> 5) + (threadIdx.x >> 5)   // warp id
//     arena  = btf_combat_ctx_arena + env
//
// A launch with blockDim.x < 32 (or with no arena armed) keeps every hook on the
// serial core, so the baseline/crowd kernels are unaffected.
//
// bp is fixed at 32 (one warp per env). There is deliberately NO block-per-env
// path: bt_core_step's serial scalar half runs on every lane, which is sound
// inside one warp (lockstep, identical bytes) but would race across warps on the
// shared Rng/episode_return read-modify-writes.
// ---------------------------------------------------------------------------
#define BTF_COMBAT_BP 32

#if defined(__CUDACC__) || defined(__CUDA_ARCH__)
__device__ BtfCombatArena* btf_combat_ctx_arena = 0;   /* the ARRAY base; host-copied */
#endif

/* Host-side arming. Declared in every pass so device-pass parses of host code
 * (clang CUDA) still resolve the name; the body is a no-op outside nvcc's host
 * pass. `base` must point at an array of BtfCombatArena, one per env, addressed
 * by warp id. */
static inline void btf_combat_set_arena(const BtfCombatArena* base) {
#if defined(__CUDACC__) && !defined(__CUDA_ARCH__)
    void* p = (void*)base;
    cudaMemcpyToSymbol(btf_combat_ctx_arena, &p, sizeof(void*));
#else
    (void)base;
#endif
}
static inline void btf_combat_clear_arena(void) { btf_combat_set_arena(0); }

// Hook prototypes (definitions after the core, whose phase bodies they call).
BT_HD static void btf_combat_hook_wproj(BtState* s, float* r);
BT_HD static void btf_combat_hook_eproj(BtState* s, float* r);
BT_HD static void btf_combat_hook_contact(BtState* s, float* r);

// Install the hooks into bt_core_step BEFORE the core is included.
#define BT_WPROJ_PHASE(s, r) btf_combat_hook_wproj((s), (r))
#define BT_EPROJ_PHASE(s, r) btf_combat_hook_eproj((s), (r))
#define BT_CONTACT_PHASE(s, r) btf_combat_hook_contact((s), (r))


#include "brotato_gpu_framework.cuh"
#include "brotato_rng_jump.h"

// ===========================================================================
// Group minimum
// ===========================================================================
BT_HD static inline int btf_group_min(int v, int* sred, int mem, int bp) {
#if defined(__CUDA_ARCH__)
    sred[mem] = v;
    btf_group_sync(bp);
    if (mem == 0) {
        int m = sred[0];
        for (int k = 1; k < bp; k++) if (sred[k] < m) m = sred[k];
        sred[0] = m;
    }
    btf_group_sync(bp);
    return sred[0];
#else
    (void)sred; (void)mem; (void)bp;
    return v;
#endif
}

// ===========================================================================
// #14 bt_wproj_step -- parallel scan hint
// ===========================================================================

/* Lowest enemy slot the live player projectile `i`'s MAIN hitbox can touch
 * this step, evaluated at the projectile's predicted post-motion centre and
 * gated exactly as bt_wproj_step_hint gates its scan (armed, stopping, leaving,
 * range lifetime, delayed arming). -1 = no candidate (or no scan this step).
 * Mirrors brotato_weapon_projectiles.h:bt_wproj_step_hint motion + gates with
 * the SAME expressions, and uses the SAME bt_wproj_overlaps predicate. */
BT_HD static int btf_combat_wproj_first_hit(BT_DEVICE BtState* s, int i) {
    BT_DEVICE const BtWProj* p = &s->wproj.p[i];
    if (!p->alive) return -1;
    if (!p->armed) return -1;                       /* spawn frame only arms */
    float rot = p->rot;
    if (p->rot_speed != 0.0f) rot += 25.0f * (BT_PI / 180.0f);
    float x = p->x + p->vx * BT_DT;
    float y = p->y + p->vy * BT_DT;
    if (p->flags & BT_WPROJ_F_STOPPING) return -1;
    if ((p->flags & BT_WPROJ_F_LEAVE) && !bt_cam_rect_has_point(x, y)) return -1;
    if (p->range_timer <= 0.0f) return -1;          /* stop() -> no scan */
    if (p->flags & BT_WPROJ_F_DELAYED) {
        if ((p->delay - 1.0f) > 0.0f) return -1;    /* still arming */
    }
    float sc = bt_cosf(rot), ss = bt_sinf(rot);
    float cx = x + (sc * p->offx - ss * p->offy);
    float cy = y + (ss * p->offx + sc * p->offy);
    for (int j = 0; j < BT_MAX; j++) {
        if (!s->ealive[j]) continue;
        if (s->e_charmed[j]) continue;
        if (bt_wproj_ignored(p, bt_wproj_eid(s, j))) continue;
        if (bt_wproj_overlaps(s, i, j, cx, cy)) return j;
    }
    return -1;
}

/* Parallel phase: scan in parallel, resolve on lane 0 with the shared body. */
BT_HD static void btf_combat_phase_wproj(BT_DEVICE BtState* s, BtfCombatArena* A,
                                         int warp, int lane32, int bp) {
#if defined(__CUDA_ARCH__)
    int mem = warp * 32 + lane32;
    BTF_FOR(i, BT_MAX_WPROJ, mem, bp) A->first_hit[i] = btf_combat_wproj_first_hit(s, i);
    btf_group_sync(bp);
    if (mem == 0) (void)bt_wproj_step_hint(s, &A->reward, A->first_hit);
    btf_group_sync(bp);
#else
    (void)warp; (void)lane32; (void)bp;
    (void)bt_wproj_step_hint(s, &A->reward, 0);
#endif
}

BT_HD static void btf_combat_wproj_serial(BT_DEVICE BtState* s, BT_THREAD float* r) {
    (void)bt_wproj_step(s, r);
}

// ===========================================================================
// #18 enemy/neutral projectile loop -- parallel scan hint
// ===========================================================================

/* Lowest enemy slot a FRIENDLY enemy-projectile `i` can touch this step, at
 * its predicted post-motion centre, gated exactly as bt_eproj_phase gates its
 * friendly scan. -1 = no candidate / no scan. */
BT_HD static int btf_combat_eproj_first_hit(BT_DEVICE BtState* s, int i) {
    if (!s->pralive[i] || !s->prfriendly[i]) return -1;
    float px = s->prx[i] + s->prvx[i] * BT_DT;
    float py = s->pry[i] + s->prvy[i] * BT_DT;
    float age = s->prage[i] + BT_DT;
    int sh = (int)s->prshape[i];
    float tstop = bt_proj_shape_stop(sh);
    if (tstop >= 0.0f && age >= tstop) return -1;
    if (bt_proj_shape_leaves(sh) && !bt_cam_rect_has_point(px, py)) return -1;
    if (!bt_proj_shape_active(sh, age)) return -1;
    for (int j = 0; j < BT_MAX; j++) {
        if (!s->ealive[j]) continue;
        if (bt_proj_hits_enemy_at(s, i, j, px, py)) return j;
    }
    return -1;
}

BT_HD static void btf_combat_phase_eproj(BT_DEVICE BtState* s, BtfCombatArena* A,
                                         int warp, int lane32, int bp) {
#if defined(__CUDA_ARCH__)
    int mem = warp * 32 + lane32;
    BTF_FOR(i, BT_MAX_PROJ, mem, bp) A->first_hit[i] = btf_combat_eproj_first_hit(s, i);
    btf_group_sync(bp);
    if (mem == 0) bt_eproj_phase(s, &A->reward, A->first_hit);
    btf_group_sync(bp);
#else
    (void)warp; (void)lane32; (void)bp;
    bt_eproj_phase(s, &A->reward, 0);
#endif
}

BT_HD static void btf_combat_eproj_serial(BT_DEVICE BtState* s, BT_THREAD float* r) {
    bt_eproj_phase(s, r, 0);
}

// ===========================================================================
// #20 player/enemy contact -- fully parallel probe
// ===========================================================================
BT_HD static void btf_combat_phase_player_contact(BT_DEVICE BtState* s, BtfCombatArena* A,
                                                  int warp, int lane32, int bp) {
#if defined(__CUDA_ARCH__)
    int mem = warp * 32 + lane32;
    if (mem == 0) bt_player_hurt_flush(s, &A->reward);   /* RNG + serial: contact + projectile halves */
    btf_group_sync(bp);
    BTF_FOR(i, BT_MAX, mem, bp) A->probe[i] = (unsigned char)bt_player_contact_probe(s, i);
    btf_group_sync(bp);
    int best = BT_MAX;
    BTF_FOR(i, BT_MAX, mem, bp) if (A->probe[i] && i < best) best = i;
    best = btf_group_min(best, A->sred, mem, bp);
    if (mem == 0) s->epend = (best < BT_MAX) ? (short)best : (short)-1;
    btf_group_sync(bp);
#else
    (void)warp; (void)lane32; (void)bp;
    bt_player_contact_phase(s, &A->reward);
#endif
}

BT_HD static void btf_combat_player_contact_serial(BT_DEVICE BtState* s, BT_THREAD float* r) {
    bt_player_contact_phase(s, r);
}

// ===========================================================================
// Hooks installed into bt_core_step
//
// Every hook derives (lane, env, arena) from the launch geometry + the
// host-copied arena base, so no kernel writes any shared context. With
// blockDim.x == 32 (one warp per env) env == blockIdx.x and lane32 ==
// threadIdx.x & 31; with a narrower block the hook stays on the serial core.
// ===========================================================================
#if defined(__CUDA_ARCH__)
#define BTF_COMBAT_HOOK_PROLOGUE(A, lane, bp)                                  \
    BtfCombatArena* A = 0;                                                     \
    int lane = 0, bp = 0;                                                      \
    if (btf_combat_ctx_arena != 0 && blockDim.x >= BTF_COMBAT_BP) {            \
        bp = BTF_COMBAT_BP;                                                    \
        lane = (int)(threadIdx.x & 31u);                                       \
        int env = (int)(blockIdx.x * (blockDim.x >> 5) + (threadIdx.x >> 5));  \
        A = btf_combat_ctx_arena + env;                                        \
    }
/* The inert (unaligned) path must run on ONE lane only: a warp-per-env launch
 * that includes this header without arming the arena otherwise executes the
 * serial phase body on all 32 lanes, racing on the shared Rng/wproj/enemy
 * state. A launch narrower than a warp (my own single-lane kernels) is
 * unaffected. NOTE: the launch contract is still "warp-per-env => arm the
 * arena"; without arming, the core's OWN scalar half is redundant across the
 * warp and races regardless of these hooks, so prefer BTFW_NO_COMBAT (or
 * blockDim 1) for un-hooked warp launches. */
#define BTF_COMBAT_INERT_LEADER() (blockDim.x < BTF_COMBAT_BP || (threadIdx.x & 31u) == 0u)
#else
#define BTF_COMBAT_HOOK_PROLOGUE(A, lane, bp)                                  \
    BtfCombatArena* A = 0;                                                     \
    int lane = 0, bp = 0;                                                      \
    (void)A; (void)lane; (void)bp;
#define BTF_COMBAT_INERT_LEADER() 1
#endif

BT_HD static void btf_combat_hook_wproj(BtState* s, float* r) {
    BTF_COMBAT_HOOK_PROLOGUE(A, lane, bp);
    if (bp > 0) {
        if (lane == 0) A->reward = 0.0f;
        btf_combat_phase_wproj(s, A, 0, lane, bp);
        *r += A->reward;
        return;
    }
    if (!BTF_COMBAT_INERT_LEADER()) return;
    (void)bt_wproj_step(s, r);
}

BT_HD static void btf_combat_hook_eproj(BtState* s, float* r) {
    BTF_COMBAT_HOOK_PROLOGUE(A, lane, bp);
    if (bp > 0) {
        if (lane == 0) A->reward = 0.0f;
        btf_combat_phase_eproj(s, A, 0, lane, bp);
        *r += A->reward;
        return;
    }
    if (!BTF_COMBAT_INERT_LEADER()) return;
    bt_eproj_phase(s, r, 0);
}

BT_HD static void btf_combat_hook_contact(BtState* s, float* r) {
    BTF_COMBAT_HOOK_PROLOGUE(A, lane, bp);
    if (bp > 0) {
        if (lane == 0) A->reward = 0.0f;
        btf_combat_phase_player_contact(s, A, 0, lane, bp);
        *r += A->reward;
        return;
    }
    if (!BTF_COMBAT_INERT_LEADER()) return;
    bt_player_contact_phase(s, r);
}

// ===========================================================================
// Verification helper: a full-struct FNV-1a digest, for comparing a serial run
// against a parallel run (a stray write to ANY byte is a failure, not a gain).
// ===========================================================================
BT_HD static unsigned int btf_combat_state_digest(const BtState* s) {
    return btf_fnv1a((const unsigned char*)s, sizeof(BtState), 2166136261u);
}

#endif // PUFFER_BROTATO_GPU_COMBAT_CUH
