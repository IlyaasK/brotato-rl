// brotato_gpu_spawn.cuh -- intra-environment GPU scheduling of the Brotato
// SPAWN phases. OWNER: GpuSpawn.
//
// SCOPE (bt_core_step phase map, brotato_gpu_framework.cuh section 7)
//   1  bt_birth_flush            deferred boundary: ready births -> units
//   2  bt_enemy_pool_step        pool descriptor ring + per-kind free stacks
//   2b bt_struct_pool_tick       deferred structure/pet node dies
//   9  bt_spawn_poll             EntitySpawner drain -> births
//  10  bt_birth_advance          EntityBirth countdown + reloc + ready
//  19  consumable/material loot  Item lifecycle motion + pickup
//  21  deaths -> loot            bt_mark_enemy_dead in slot order
//  +   cull at max_enemies       bt_group_perf_cull (entity_spawner.gd:150-165)
//  +   reserve-slot cull          bt_reserve_slot full-branch safety net
//
// THE ONE RULE (framework): every kernel here is BIT-EXACT against the CPU
// core. Nothing is re-implemented: each phase calls the very same BT_HD
// functions bt_core_step calls, from a different schedule. A schedule is legal
// iff it preserves, for every mutable object, the serial mutation order.
//
// SCHEDULES
// MAINTENANCE: every draw site this file re-schedules (birth relocation, the
// two culls) must mirror any instrumentation the core adds at the same point
// (e.g. BtState.draw_site[]). The shared helpers it calls carry their own
// instrumentation automatically.
//
//   * Wide, order-free work runs on all 32 lanes of an env's warp. The narrow
//     order-dependent tail (RNG draws, free-list pops, ring bookkeeping) is a
//     commit walked in strictly ascending index order by lane 0; the finished/
//     picked/dead sets that commit consumes are built with btg_compact_ordered
//     (the framework's order-preserving count/scan/write pattern), so the commit
//     sees exactly the serial order.
//   * Where a phase's per-consumer draw COUNT is fixed by pre-phase state (the
//     max_enemies cull: exactly one bt_rng_bounded per eligible enemy, see
//     GpuRng's bt_rngcount_bounded), the draws run on all lanes via
//     btf_draw_parallel (pcg32 jump-ahead through btf_rng_advance) and only the
//     winning reduction is ordered.
//   * Phases whose RNG stream is draw-count-coupled (EntitySpawner pops and the
//     birth relocation both call bt_rand_range, whose count is 3-or-1 --
//     GpuRng's 2^-32 caveat) keep lane 0 as the RNG sequencer and call the same
//     serial function. They are exact-by-construction; no intra-env parallelism
//     is available there.
//
// ONE WARP PER ENV (BTF_MODE A, blockDim = 32): BTF_MEM(32) == lane id and
// BTF_SLOT == 0, so the framework's single-scratch-slice btf_scan_excl and
// btf_draw_parallel are race-free with no cross-warp group-total sharing. The
// batch keeps full env occupancy (grid.x = n envs); the wide passes give the
// intra-env speedup the smaller env sizes would otherwise hide.
//
// This header includes brotato_gpu_framework.cuh (which includes the shared
// core) and adds kernels, device entries and host launchers only; it edits no
// shared header.

#ifndef PUFFER_BROTATO_GPU_SPAWN_CUH
#define PUFFER_BROTATO_GPU_SPAWN_CUH

#include "brotato_gpu_framework.cuh"

// ---------------------------------------------------------------------------
// pcg32 jump-ahead (btf_rng_advance). GpuRng owns brotato_rng_jump.h; include it
// when present (it defines btf_rng_advance and the bt_rngcount_* draw counts).
// The fallback below keeps this header self-contained when that file is absent.
// ---------------------------------------------------------------------------
#if defined(__has_include)
#  if __has_include("brotato_rng_jump.h")
#    include "brotato_rng_jump.h"
#  endif
#endif
#ifndef BTF_RNG_JUMP_DEFINED
/* Advance the PCG32 stream by `delta` bt_rng_next() calls in O(log delta):
 * state' = state*M^delta + inc*(1 + M + ... + M^(delta-1)); the running
 * (acc_mult, acc_plus) pair is the reference pcg_advance_lcg_64 fold. */
BT_HD static inline BtRng btf_rng_advance(BtRng r, bt_u64 delta) {
    bt_u64 acc_mult = (bt_u64)1;
    bt_u64 acc_plus = (bt_u64)0;
    bt_u64 cur_mult = BT_RNG_MULT;
    bt_u64 cur_plus = r.inc;
    while (delta > (bt_u64)0) {
        if ((delta & (bt_u64)1) != (bt_u64)0) {
            acc_mult *= cur_mult;
            acc_plus = acc_plus * cur_mult + cur_plus;
        }
        cur_plus = (cur_mult + (bt_u64)1) * cur_plus;
        cur_mult *= cur_mult;
        delta >>= 1;
    }
    r.state = acc_mult * r.state + acc_plus;
    return r;
}
/* bt_rng_bounded consumes exactly one word (GpuRng section 3). */
BT_HD static inline unsigned int btg_rngcount_bounded(void) { return 1u; }
/* one bounded word per eligible enemy (GpuRng section 4g). */
BT_HD static inline unsigned int btg_rngcount_enemy_cull(int n) {
    return n > 0 ? (unsigned int)n : 0u;
}
#else
#define btg_rngcount_bounded()      bt_rngcount_bounded()
#define btg_rngcount_enemy_cull(n)  bt_rngcount_enemy_cull(n)
#endif

// ---------------------------------------------------------------------------
// Per-env scratch. One warp per env -> BTF_SLOT == 0 and a single scratch slice
// serves the env's whole group; the arrays are big enough for every phase's
// widest object space (BTG_FLAGS is derived from BT_MAX / BT_MAX_BIRTHS below).
// ---------------------------------------------------------------------------
/* Capacity of the per-env staging arrays, derived from the core caps so a
 * BT_MAX / BT_MAX_BIRTHS growth is absorbed with no edit here. Flags/list hold
 * one entry per enemy slot or birth; the birth bucket span is 32*ceil(n/32),
 * so round up to a multiple of 32. */
#define BTG_FLAGS  ((((BT_MAX) > (BT_MAX_BIRTHS) ? (BT_MAX) : (BT_MAX_BIRTHS)) + 31) & ~31)
#define BTG_LANES  32
static_assert(BTG_FLAGS >= BT_MAX, "BtgShared must hold every enemy slot");
static_assert(BTG_FLAGS >= BT_MAX_BIRTHS, "BtgShared must hold every birth");
static_assert(BTG_FLAGS >= BT_MAX_MAT, "BtgShared must hold every material");

typedef struct BtgShared {
    BtfScanScratch sc;
    unsigned char  flags[BTG_FLAGS];        /* finished/pick/predicate staging */
    int            list[BTG_FLAGS];         /* compacted ordered index list     */
    int            prio[BTG_FLAGS];         /* priority-remove append order     */
    unsigned int   counts[BT_MAX];          /* btf_draw_parallel draw counts    */
    unsigned int   draws[BT_MAX];           /* btf_draw_parallel drawn outputs  */
    int            aux[2];                  /* free-slot reduction sentinel     */
} BtgShared;

// Ordered compaction, bp == 32 specialisation. Member m owns the CONTIGUOUS
// chunk [m*chunk,(m+1)*chunk) of [0,n) (contiguous -- not strided -- is what
// makes the output ordered) and appends the chunk's flagged indices to its own
// bucket at out[m*chunk + k], recording the bucket length in cnt[m]. Walking the
// buckets in member order emits the flagged indices in ascending index order --
// the same list the framework's count/scan/write pattern emits, but in ONE pass
// over [0,n) with no cross-lane scan (the bucket layout already encodes the
// prefix). cnt must hold `bp` bytes and out `n` (or chunk*bp) ints.
BT_HD static inline int btg_bucket_begin(int chunk, int* lo, int* hi, int n, int mem) {
    *lo = mem * chunk; *hi = *lo + chunk;
    if (*hi > n) *hi = n;
    return chunk;
}

// ===========================================================================
// Phase 10: bt_birth_advance
//
// serial: for each active birth in index order, timer -= 1; on the frame the
// countdown reaches 0 the ENEMY overlapping the player is relocated with
// bt_rand_pos (global RNG: 2 rand_range draws) and restarted, every other
// finished birth is marked ready (active = 2) and enqueues a birth sound.
//
// schedule: the countdown (n_births <= 256 FP decrements) runs on the warp, and
// the FINISHED indices are bucketed in the same pass (ascending order by
// construction). Lane 0 walks the buckets and performs the relocation RNG + the
// ordered audio enqueue exactly as the serial loop does, so the relocation draws
// stay in ascending birth-index order and every non-finished birth has only its
// own (identical) decrement.
// ===========================================================================
BT_HD static inline void btg_birth_advance_par(BtState* s, int mem, BtgShared* sh) {
    int nb = s->n_births;
    if (nb > BTG_FLAGS) nb = BTG_FLAGS;
    int chunk = (nb + 31) / 32;
    unsigned char* cnt = (unsigned char*)sh->counts;   /* 32 bytes reused */
    int lo, hi;
    btg_bucket_begin(chunk, &lo, &hi, nb, mem);
    int k = 0;
    for (int i = lo; i < hi; i++) {
        BtBirth* b = &s->births[i];
        if (b->active != 1) continue;
        b->timer -= 1.0f;
        if (b->timer <= 0.0f) sh->list[mem * chunk + k++] = i;
    }
    cnt[mem] = (unsigned char)k;
    btf_group_sync(32);
    if (mem == 0) {
        for (int w = 0; w < 32; w++) {
            int wn = (int)cnt[w];
            for (int q = 0; q < wn; q++) {
                int i = sh->list[w * chunk + q];
                BtBirth* b = &s->births[i];
                float dx = b->x - s->px;
                float dy = b->y - (s->py + BT_PLAYER_BODY_Y);
                float rr = BT_BIRTH_RADIUS + BT_PLAYER_BODY_R;
                if (b->bclass == 0 && !b->neutral && !b->is_boss &&
                    dx * dx + dy * dy < rr * rr) {
                    float nx, ny;
                    bt_rand_pos(s, BT_EDGE_MAP_DIST, &nx, &ny);   /* get_rand_pos */
                    s->draw_site[2] += 6u;   /* the core's relocation-site count */
                    b->x = nx; b->y = ny;
                    b->timer = (float)BT_BIRTH_FRAMES;
                } else {
                    b->active = 2;
                    bt_birth_audio_enqueue(s, 1);
                }
            }
        }
    }
    btf_group_sync(32);
}

__global__ void __launch_bounds__(BTG_LANES)
btg_birth_advance_kernel(BtState* __restrict__ base, size_t stride, int n) {
    __shared__ BtgShared sh;
    int env = blockIdx.x;
    if (env >= n) return;
    btg_birth_advance_par(BTF_AT(base, stride, env), BTF_LANE32, &sh);
}

// ===========================================================================
// Phase 2: bt_enemy_pool_step
//
// Pure linked-list bookkeeping (60-slot ring -> per-kind LIFO free stacks) with
// NO RNG. The ring chain is a pointer chase, so lane 0 walks it; the phase is
// the serial function. (Serial-bound; kept as a kernel so the fused step has one
// launch per phase.)
// ===========================================================================
__global__ void __launch_bounds__(BTG_LANES)
btg_pool_step_kernel(BtState* __restrict__ base, size_t stride, int n) {
    int env = blockIdx.x;
    if (env >= n) return;
    if (BTF_LANE32 != 0) return;
    bt_enemy_pool_step(BTF_AT(base, stride, env));
}

// ===========================================================================
// Phase 2b: bt_struct_pool_tick (bt_core_step calls it immediately after
// bt_neutral_uid_step, beside bt_enemy_pool_step). Deferred structure/pet node
// dies: landmine-style immediate evictions vs Structure.deferred_die windows.
// Lane-independent, ordered, no RNG -> lane 0.
// ===========================================================================
__global__ void __launch_bounds__(BTG_LANES)
btg_struct_pool_tick_kernel(BtState* __restrict__ base, size_t stride, int n) {
    int env = blockIdx.x;
    if (env >= n) return;
    if (BTF_LANE32 != 0) return;
    bt_struct_pool_tick(BTF_AT(base, stride, env));
}

// ===========================================================================
// Phase 9: bt_spawn_poll
//
// Three global discards, then each owner queue pops LIFO into births, then the
// ordinary enemy queue nb_to_spawn times. Every pop draws a rand_range rotation
// (draw count RNG-coupled: 3 or 1), so lane 0 is the RNG sequencer and calls the
// same bt_spawn_poll (exact by construction).
// ===========================================================================
__global__ void __launch_bounds__(BTG_LANES)
btg_spawn_poll_kernel(BtState* __restrict__ base, size_t stride, int n) {
    int env = blockIdx.x;
    if (env >= n) return;
    if (BTF_LANE32 != 0) return;
    bt_spawn_poll(BTF_AT(base, stride, env));
}

// ===========================================================================
// Phase 1: bt_birth_flush
//
// Spawns every ready (active == 2) birth in index order and compacts the array.
// The spawn path draws per-unit RNG and allocates pool ids in birth order, so
// lane 0 is the sequencer (exact by calling the same bt_birth_flush).
// ===========================================================================
__global__ void __launch_bounds__(BTG_LANES)
btg_birth_flush_kernel(BtState* __restrict__ base, size_t stride, int n) {
    int env = blockIdx.x;
    if (env >= n) return;
    if (BTF_LANE32 != 0) return;
    bt_birth_flush(BTF_AT(base, stride, env));
}

// ===========================================================================
// Cull at max_enemies: the reservoir half of bt_reserve_slot.
//
// entity_spawner.gd:151-165 (== bt_reserve_slot's full branch) walks live
// enemies in SLOT order and keeps candidate i iff bt_rng_bounded(&rng, rank+1)
// == 0, so the winner is the LAST such i. Every eligible enemy consumes EXACTLY
// ONE draw and its bound (rank+1) is a pure function of pre-phase state, so the
// draw sequence qualifies for btf_draw_parallel: lane l draws its own eligible
// slots' outputs from a jump-ahead stream copy, then lane 0 reduces the winner
// in ascending slot order. The final RNG state is the serial state.
// Returns the killed slot (>= 0) or -1.
// ===========================================================================
BT_HD static inline int btg_eligible_cull(BtState* s, int i) {
    if (!s->ealive[i]) return 0;
    if (s->eneutral[i]) return 0;
    if (bt_kind_class(s->ekind[i]) != 0) return 0;
    return 1;
}

BT_HD static inline void btg_cull_kill(BtState* s, int pick) {
    if (pick < 0) return;
    /* Mirrors the live bt_reserve_slot full branch (brotato_core.h:662-680): the
     * safety-net cull is loud and delegates the death to bt_mark_enemy_dead with
     * allow_loot = 0. Keep this tail byte-in-sync with that function. */
    s->pool_cull_fallback = 1;
    bt_mark_enemy_dead(s, pick, 0);
}

__device__ int btg_cull_parallel(BtState* s, int mem, BtgShared* sh) {
    unsigned int* counts = sh->counts;
    unsigned int* draws = sh->draws;
    int* bound = sh->list;      /* per-slot draw bound = rank + 1 (pure pre-phase) */
    BTF_FOR(i, BT_MAX, mem, 32) {
        counts[i] = (unsigned int)(btg_eligible_cull(s, i) ? 1 : 0);
        draws[i] = 0u;
    }
    btf_group_sync(32);
    /* rank base for this lane's contiguous chunk of [0,BT_MAX): exclusive scan
     * of the chunk eligible counts. btf_draw_parallel then scans the same counts
     * again for the draw offsets -- two O(n) scans, no O(n^2). */
    {
        int chunk = (BT_MAX + 31) / 32;
        int lo = mem * chunk, hi = lo + chunk;
        if (hi > BT_MAX) hi = BT_MAX;
        int mine = 0;
        for (int i = lo; i < hi; i++) mine += (int)counts[i];
        int base = btf_scan_excl(mine, &sh->sc, mem, 0, 32);
        int r = base;
        for (int i = lo; i < hi; i++) { if (counts[i]) r++; bound[i] = r; }
        btf_group_sync(32);
    }
    /* one bt_rng_bounded word per eligible enemy; the phase total therefore
     * equals bt_rngcount_enemy_cull(<eligible count>) (GpuRng section 4g). */
    auto count_fn = [counts](int i) -> unsigned int {
        return counts[i] ? btg_rngcount_bounded() : 0u;
    };
    auto draw_fn = [draws, bound](int i, int j, BtRng* r) {
        (void)j;
        if (!bound[i]) return;                 /* not eligible */
        draws[i] = bt_rng_bounded(r, (unsigned int)bound[i]);
    };
    btf_draw_parallel(&s->rng, counts, BT_MAX, &sh->sc, mem, 0, 32, count_fn, draw_fn);
    btf_group_sync(32);
    int pick = -1;
    if (mem == 0) {
        for (int i = 0; i < BT_MAX; i++) {
            if (!counts[i]) continue;
            if (draws[i] == 0u) pick = i;
        }
        btg_cull_kill(s, pick);
    }
    btf_group_sync(32);
    return pick;
}

// bt_reserve_slot scheduled: lowest free slot, else the parallel cull.
__device__ int btg_reserve_slot_par(BtState* s, int mem, BtgShared* sh) {
    BTF_FOR(i, BT_MAX, mem, 32) {
        if (!s->ealive[i]) {
#if defined(__CUDA_ARCH__)
            atomicMin(&sh->aux[0], i);   /* shared reduction of the lowest free slot */
#endif
        }
    }
    btf_group_sync(32);
    if (mem == 0)
        sh->aux[0] = (sh->aux[0] == BT_MAX) ? -1 : sh->aux[0];   /* BT_MAX == none */
    btf_group_sync(32);
    int free_slot = sh->aux[0];
    if (free_slot >= 0) return free_slot;
    return btg_cull_parallel(s, mem, sh);
}

__global__ void __launch_bounds__(BTG_LANES)
btg_reserve_slot_kernel(BtState* __restrict__ base, size_t stride, int n,
                        int* __restrict__ out_slot) {
    __shared__ BtgShared sh;
    int env = blockIdx.x;
    if (env >= n) return;
    sh.aux[0] = BT_MAX;             /* sentinel: no free slot */
    btf_group_sync(32);
    int r = btg_reserve_slot_par(BTF_AT(base, stride, env), BTF_LANE32, &sh);
    if (BTF_LANE32 == 0) out_slot[env] = r;
}

// ===========================================================================
// Cull at max_enemies: bt_group_perf_cull (entity_spawner.gd:150-165)
//
// serial: build `ord` (the live PLAIN enemies in append order == ascending
// e_act, ties by slot, an insertion sort); over = n - wave_max_enemies; then
// `over` times draw bt_rng_bounded(&rng, n) with n shrinking by one, erase that
// entry from ord and kill the slot.
//
// schedule: BOTH append-order lists (`ord` = every plain enemy, `ordp` = the
// subset whose kind sets Enemy.to_be_removed_in_priority, entity_spawner.gd:405)
// are built in ONE parallel pass with a stable rank by (e_act, slot) -- exactly
// the insertion sort's order -- so the O(n^2) sorts leave the critical path.
// The removal loop itself stays lane-0 serial: each removal's
// bt_mark_enemy_dead can draw additional RNG between two removals, so the draws
// are NOT class (a) and cannot be jump-ahead hoisted. While `ordp` is non-empty
// the source picks from it (get_rand_element(enemies_to_remove_in_priority)),
// and each die() erases the node from BOTH lists (entity_spawner.gd:450-452).
// ===========================================================================
/* entity_spawner.gd:405/150-165 + _on_enemy_died: the kinds whose scene sets
 * Enemy.to_be_removed_in_priority form `enemies_to_remove_in_priority`, which
 * the perf cull drains first. WavesGaps owns the generated table
 * BT_KIND_PRIORITY_REMOVE[BT_NKINDS]; until it is present no kind is priority,
 * which is exactly the pre-priority schedule. */
#if defined(BT_KIND_PRIORITY_REMOVE_N)
BT_HD static inline int btg_prio_kind(int kind) {
    if (kind < 0 || kind >= BT_NKINDS) return 0;
    return (int)BT_KIND_PRIORITY_REMOVE[kind];
}
#else
BT_HD static inline int btg_prio_kind(int kind) { (void)kind; return 0; }
#endif

BT_HD static inline int btg_plain_live(BtState* s, int i) {
    if (!s->ealive[i]) return 0;
    if (s->eneutral[i]) return 0;
    if (s->eelite[i] != 0) return 0;
    return 1;
}

__device__ void btg_group_perf_cull_par(BtState* s, int mem, BtgShared* sh) {
    unsigned char* pl = sh->flags;          /* plain-live flags               */
    int* ord = sh->list;                    /* all-plain append order         */
    int* ordp = sh->prio;                   /* priority-subset append order   */
    BTF_FOR(i, BT_MAX, mem, 32) pl[i] = (unsigned char)(btg_plain_live(s, i) ? 1 : 0);
    btf_group_sync(32);
    int chunk = (BT_MAX + 31) / 32;
    int lo = mem * chunk, hi = lo + chunk;
    if (hi > BT_MAX) hi = BT_MAX;
    /* n = |enemies|, np = |enemies_to_remove_in_priority| (both pre-cull). */
    int mine = 0, minep = 0;
    for (int i = lo; i < hi; i++) {
        if (!pl[i]) continue;
        mine++;
        if (btg_prio_kind((int)s->ekind[i])) minep++;
    }
    (void)btf_scan_excl(mine, &sh->sc, mem, 0, 32);
    int n = btf_scan_total(&sh->sc, 32, 0);
    (void)btf_scan_excl(minep, &sh->sc, mem, 0, 32);
    int np = btf_scan_total(&sh->sc, 32, 0);
    /* stable rank by (e_act, slot) inside each list: the insertion-sort order. */
    for (int i = lo; i < hi; i++) {
        if (!pl[i]) continue;
        int r = 0;
        for (int j = 0; j < BT_MAX; j++) {
            if (!pl[j]) continue;
            if (s->e_act[j] < s->e_act[i] ||
                (s->e_act[j] == s->e_act[i] && j < i)) r++;
        }
        ord[r] = i;
        if (btg_prio_kind((int)s->ekind[i])) {
            int rp = 0;
            for (int j = 0; j < BT_MAX; j++) {
                if (!pl[j] || !btg_prio_kind((int)s->ekind[j])) continue;
                if (s->e_act[j] < s->e_act[i] ||
                    (s->e_act[j] == s->e_act[i] && j < i)) rp++;
            }
            ordp[rp] = i;
        }
    }
    btf_group_sync(32);

    /* The removal loop is RNG-order-coupled, so lane 0 replays the exact serial
     * loop (brotato_core.h bt_group_perf_cull == entity_spawner.gd:150-165 +
     * _on_enemy_died): pick from `ordp` while it is non-empty, else from `ord`;
     * each removal erases the node from BOTH lists. */
    if (mem == 0) {
        int nn = n, npr = np;
        int over = n - (int)s->wave_max_enemies;
        for (int i = 0; i < over; i++) {
            int slot;
            if (npr > 0) {
                int pick = (int)bt_rng_bounded(&s->rng, (unsigned int)npr);
                slot = ordp[pick];
                for (int q = pick + 1; q < npr; q++) ordp[q - 1] = ordp[q];
                npr--;
                /* entity_spawner.gd:450-452 erases the died node from BOTH lists;
                 * `n` only drops when the node is actually found in `ord`. */
                for (int q = 0; q < nn; q++) {
                    if (ord[q] != slot) continue;
                    for (int z = q + 1; z < nn; z++) ord[z - 1] = ord[z];
                    nn--;
                    break;
                }
            } else {
                if (nn <= 0) break;
                int pick = (int)bt_rng_bounded(&s->rng, (unsigned int)nn);
                slot = ord[pick];
                for (int q = pick + 1; q < nn; q++) ord[q - 1] = ord[q];
                nn--;
            }
            if (!s->ealive[slot]) continue;
            bt_mark_enemy_dead(s, slot, 0);          /* can_drop_loot = false */
        }
    }
    btf_group_sync(32);
}

__global__ void __launch_bounds__(BTG_LANES)
btg_group_perf_cull_kernel(BtState* __restrict__ base, size_t stride, int n) {
    __shared__ BtgShared sh;
    int env = blockIdx.x;
    if (env >= n) return;
    btg_group_perf_cull_par(BTF_AT(base, stride, env), BTF_LANE32, &sh);
}

// ===========================================================================
// Phase 19: consumable + material Item lifecycle motion, then pickup.
//
// The motion (bt_loot_item_step + the evil_mob steal + the attract claim) is
// per-item and reads only the item's own fields, the frame's player position
// and bt_scalar_attract_radius -- all frame constants. It runs on the warp. The
// pickup tail mutates shared player state (materials, XP, hp, the death-event
// drain) and draws RNG, so it is committed in ascending slot order.
//
// EXACTNESS GUARDS (fallback to the serial loop for that one env):
//   * a material pickup runs bt_drain_death_events, which can kill/spawn an
//     enemy an evil_mob (mat_mob >= 0) later material tracks -> if a pickup
//     happens AND any material is mob-owned, the material loop is serial;
//   * a consumable pickup heals, and a later consumable's attract claim reads
//     hp -> below max HP the consumable loop is serial;
//   * a material spawned mid-commit by a drain is stepped inline when the serial
//     loop would reach it (its slot index not yet passed).
// ===========================================================================
BT_HD static inline void btg_loot_serial_materials(BtState* s) {
    float ax = s->px, ay = s->py + BT_ATTRACT_Y;
    float attract = bt_scalar_attract_radius(s);
    float attract2 = attract * attract;
    for (int m = 0; m < BT_MAX_MAT; m++) {
        if (!s->matalive[m]) continue;
        if (s->mat_by[m] != 0 || s->mat_attr[m] == 2) continue;
        float dx = ax - s->matx[m], dy = ay - s->maty[m];
        float d2 = dx * dx + dy * dy;
        if (s->mat_mob[m] >= 0) {
            int om = (int)s->mat_mob[m];
            if (!s->ealive[om] || s->eneutral[om]) {
                s->mat_mob[m] = -1;
                s->mat_attr[m] = 0;
                s->mat_arc[m][BT_LOOT_ARC_SPEED] = BT_LOOT_ITEM_ATTRACT_SPEED;
            } else if (d2 <= attract2) {
                float mdx = s->ex[om] - s->matx[m], mdy = s->ey[om] - s->maty[m];
                if (d2 < mdx * mdx + mdy * mdy) {
                    s->mat_mob[m] = -1;
                    s->mat_attr[m] = 0;
                }
            }
        }
        if (s->mat_mob[m] >= 0) continue;
        if (s->mat_home[m] || d2 <= attract2) s->mat_attr[m] = 1;
        (void)bt_loot_item_step(&s->matx[m], &s->maty[m], s->mat_arc[m], &s->mat_attr[m],
                                0, ax, ay, BT_DT);
        if (s->mat_arc[m][BT_LOOT_ARC_MON] == 0.0f) continue;
        if (s->mat_attr[m] != 0 && s->mat_attr[m] != 1) continue;
        float pdx = s->px - s->matx[m], pdy = (s->py + BT_PICKUP_Y) - s->maty[m];
        if (pdx * pdx + pdy * pdy > BT_PICKUP_R * BT_PICKUP_R) continue;
        int val = bt_scalar_gold_pickup(s, (int)s->matval[m]);
        bt_drain_death_events(s);
        s->materials += (float)val;
        bt_progress_add_xp(s, (float)val);
        s->matalive[m] = 0;
        s->matcount--;
        s->mat_attr[m] = 0;
        s->mat_by[m] = 0;
        s->mat_home[m] = 0;
    }
}

BT_HD static inline void btg_loot_serial_consumables(BtState* s) {
    for (int i = 0; i < BT_MAX_CONS; i++) {
        if (!s->conalive[i]) continue;
        float ax = s->px, ay = s->py + BT_ATTRACT_Y;
        float dx = ax - s->conx[i], dy = ay - s->cony[i];
        float d2 = dx * dx + dy * dy;
        float attract = bt_scalar_attract_radius(s);
        if (s->hp < s->max_hp && d2 <= attract * attract) s->con_attr[i] = 1;
        (void)bt_loot_item_step(&s->conx[i], &s->cony[i], s->con_arc[i], &s->con_attr[i],
                                0, ax, ay, BT_DT);
        s->conspd[i] = s->con_arc[i][BT_LOOT_ARC_SPEED];
        if (s->con_arc[i][BT_LOOT_ARC_MON] == 0.0f) continue;
        if (s->con_attr[i] != 0 && s->con_attr[i] != 1) continue;
        float pdx = s->px - s->conx[i], pdy = (s->py + BT_PICKUP_Y) - s->cony[i];
        if (pdx * pdx + pdy * pdy < BT_PICKUP_R * BT_PICKUP_R) {
            bt_loot_consumable_take(s, i);
        }
    }
}

// per-env material loop: guarded parallel motion + ordered pickup commit.
__device__ void btg_loot_materials(BtState* s, int mem, BtgShared* sh) {
    float ax = s->px, ay = s->py + BT_ATTRACT_Y;
    float attract = bt_scalar_attract_radius(s);
    float attract2 = attract * attract;

    int probe_pick = 0, probe_mob = 0;
    BTF_FOR(m, BT_MAX_MAT, mem, 32) {
        if (!s->matalive[m]) continue;
        if (s->mat_by[m] != 0 || s->mat_attr[m] == 2) continue;
        if (s->mat_mob[m] >= 0) {
            probe_mob = 1;
            int om = (int)s->mat_mob[m];
            if (s->ealive[om] && !s->eneutral[om]) continue;   /* mob owns it this frame */
        }
        float dx = ax - s->matx[m], dy = ay - s->maty[m];
        if (s->mat_home[m] || dx * dx + dy * dy <= attract2) probe_pick = 1;
    }
    btf_group_sync(32);
    int any_pick = 0, any_mob = 0;
#if defined(__CUDA_ARCH__)
    any_pick = __any_sync(0xffffffffu, probe_pick);
    any_mob  = __any_sync(0xffffffffu, probe_mob);
#else
    any_pick = probe_pick; any_mob = probe_mob;
#endif
    if (any_pick && any_mob) {
        if (mem == 0) btg_loot_serial_materials(s);
        btf_group_sync(32);
        return;
    }

    unsigned char* pick = sh->flags;
    unsigned char* alive0 = (unsigned char*)sh->list;   /* 256 bytes >= BT_MAX_MAT */
    BTF_FOR(m, BT_MAX_MAT, mem, 32) {
        pick[m] = 0; alive0[m] = 0;
        if (!s->matalive[m]) continue;
        alive0[m] = 1;
        if (s->mat_by[m] != 0 || s->mat_attr[m] == 2) continue;
        float dx = ax - s->matx[m], dy = ay - s->maty[m];
        float d2 = dx * dx + dy * dy;
        if (s->mat_mob[m] >= 0) {
            int om = (int)s->mat_mob[m];
            if (!s->ealive[om] || s->eneutral[om]) {
                s->mat_mob[m] = -1;
                s->mat_attr[m] = 0;
                s->mat_arc[m][BT_LOOT_ARC_SPEED] = BT_LOOT_ITEM_ATTRACT_SPEED;
            } else if (d2 <= attract2) {
                float mdx = s->ex[om] - s->matx[m], mdy = s->ey[om] - s->maty[m];
                if (d2 < mdx * mdx + mdy * mdy) {
                    s->mat_mob[m] = -1;
                    s->mat_attr[m] = 0;
                }
            }
        }
        if (s->mat_mob[m] >= 0) continue;
        if (s->mat_home[m] || d2 <= attract2) s->mat_attr[m] = 1;
        (void)bt_loot_item_step(&s->matx[m], &s->maty[m], s->mat_arc[m], &s->mat_attr[m],
                                0, ax, ay, BT_DT);
        if (s->mat_arc[m][BT_LOOT_ARC_MON] == 0.0f) continue;
        if (s->mat_attr[m] != 0 && s->mat_attr[m] != 1) continue;
        float pdx = s->px - s->matx[m], pdy = (s->py + BT_PICKUP_Y) - s->maty[m];
        if (pdx * pdx + pdy * pdy > BT_PICKUP_R * BT_PICKUP_R) continue;
        pick[m] = 1;
    }
    btf_group_sync(32);

    if (mem == 0) {
        for (int m = 0; m < BT_MAX_MAT; m++) {
            if (!s->matalive[m]) continue;
            if (!alive0[m]) {
                /* a drain spawned this slot after the parallel pass: the serial
                 * loop reaches it now, so run its whole body inline. */
                if (s->mat_by[m] != 0 || s->mat_attr[m] == 2) continue;
                float dx = ax - s->matx[m], dy = ay - s->maty[m];
                float d2 = dx * dx + dy * dy;
                if (s->mat_mob[m] >= 0) {
                    int om = (int)s->mat_mob[m];
                    if (!s->ealive[om] || s->eneutral[om]) {
                        s->mat_mob[m] = -1;
                        s->mat_attr[m] = 0;
                        s->mat_arc[m][BT_LOOT_ARC_SPEED] = BT_LOOT_ITEM_ATTRACT_SPEED;
                    } else if (d2 <= attract2) {
                        float mdx = s->ex[om] - s->matx[m], mdy = s->ey[om] - s->maty[m];
                        if (d2 < mdx * mdx + mdy * mdy) {
                            s->mat_mob[m] = -1;
                            s->mat_attr[m] = 0;
                        }
                    }
                }
                if (s->mat_mob[m] >= 0) continue;
                if (s->mat_home[m] || d2 <= attract2) s->mat_attr[m] = 1;
                (void)bt_loot_item_step(&s->matx[m], &s->maty[m], s->mat_arc[m],
                                        &s->mat_attr[m], 0, ax, ay, BT_DT);
                if (s->mat_arc[m][BT_LOOT_ARC_MON] == 0.0f) continue;
                if (s->mat_attr[m] != 0 && s->mat_attr[m] != 1) continue;
                float pdx = s->px - s->matx[m], pdy = (s->py + BT_PICKUP_Y) - s->maty[m];
                if (pdx * pdx + pdy * pdy > BT_PICKUP_R * BT_PICKUP_R) continue;
            } else {
                if (s->mat_by[m] != 0 || s->mat_attr[m] == 2) continue;
                if (s->mat_mob[m] >= 0) continue;
                if (!pick[m]) continue;
            }
            int val = bt_scalar_gold_pickup(s, (int)s->matval[m]);
            bt_drain_death_events(s);
            s->materials += (float)val;
            bt_progress_add_xp(s, (float)val);
            s->matalive[m] = 0;
            s->matcount--;
            s->mat_attr[m] = 0;
            s->mat_by[m] = 0;
            s->mat_home[m] = 0;
        }
    }
    btf_group_sync(32);
}

// per-env consumable loop: parallel motion only when full HP (no hp coupling).
__device__ void btg_loot_consumables(BtState* s, int mem, BtgShared* sh) {
    if (s->hp < s->max_hp) {
        if (mem == 0) btg_loot_serial_consumables(s);
        btf_group_sync(32);
        return;
    }
    float ax = s->px, ay = s->py + BT_ATTRACT_Y;
    unsigned char* pick = sh->flags;
    BTF_FOR(i, BT_MAX_CONS, mem, 32) {
        pick[i] = 0;
        if (!s->conalive[i]) continue;
        /* hp >= max_hp: the attract claim `hp < max_hp && ...` is false for every
         * consumable and no take lowers hp, so con_attr is left untouched. */
        (void)bt_loot_item_step(&s->conx[i], &s->cony[i], s->con_arc[i], &s->con_attr[i],
                                0, ax, ay, BT_DT);
        s->conspd[i] = s->con_arc[i][BT_LOOT_ARC_SPEED];
        if (s->con_arc[i][BT_LOOT_ARC_MON] == 0.0f) continue;
        if (s->con_attr[i] != 0 && s->con_attr[i] != 1) continue;
        float pdx = s->px - s->conx[i], pdy = (s->py + BT_PICKUP_Y) - s->cony[i];
        if (pdx * pdx + pdy * pdy < BT_PICKUP_R * BT_PICKUP_R) pick[i] = 1;
    }
    btf_group_sync(32);
    if (mem == 0) {
        for (int i = 0; i < BT_MAX_CONS; i++) {
            if (!s->conalive[i] || !pick[i]) continue;
            bt_loot_consumable_take(s, i);
        }
    }
    btf_group_sync(32);
}

/* The core runs bt_enemy_lifecycle_tick BETWEEN the consumable loop and the
 * material loop, and that tick belongs to the enemy/combat slice -- not this
 * phase. The dispatcher therefore calls these two kernels with the tick in
 * between; btg_loot_kernel runs both back to back for the benchmark and the
 * phase differential test only. */
__global__ void __launch_bounds__(BTG_LANES)
btg_loot_consumables_kernel(BtState* __restrict__ base, size_t stride, int n) {
    __shared__ BtgShared sh;
    int env = blockIdx.x;
    if (env >= n) return;
    btg_loot_consumables(BTF_AT(base, stride, env), BTF_LANE32, &sh);
}

__global__ void __launch_bounds__(BTG_LANES)
btg_loot_materials_kernel(BtState* __restrict__ base, size_t stride, int n) {
    __shared__ BtgShared sh;
    int env = blockIdx.x;
    if (env >= n) return;
    btg_loot_materials(BTF_AT(base, stride, env), BTF_LANE32, &sh);
}

__global__ void __launch_bounds__(BTG_LANES)
btg_loot_kernel(BtState* __restrict__ base, size_t stride, int n) {
    __shared__ BtgShared sh;
    int env = blockIdx.x;
    if (env >= n) return;
    BtState* s = BTF_AT(base, stride, env);
    btg_loot_consumables(s, BTF_LANE32, &sh);
    btg_loot_materials(s, BTF_LANE32, &sh);
}

// ===========================================================================
// Phase 21: deaths -> loot (ordered apply)
//
// A parallel combat pass masks the slots whose hp reached 0. The loot side must
// run in ascending slot order (loot RNG, death children, the death event stack),
// so lane 0 drives bt_mark_enemy_dead for each masked slot in order; the per-slot
// allow_loot policy rides the mask (2 == no loot).
// ===========================================================================
__global__ void __launch_bounds__(BTG_LANES)
btg_deaths_kernel(BtState* __restrict__ base, size_t stride, int n,
                  const unsigned char* __restrict__ mask) {
    int env = blockIdx.x;
    if (env >= n) return;
    if (BTF_LANE32 != 0) return;
    BtState* s = BTF_AT(base, stride, env);
    const unsigned char* mk = mask + (size_t)env * BT_MAX;
    for (int i = 0; i < BT_MAX; i++) {
        if (!mk[i]) continue;
        if (!s->ealive[i]) continue;
        bt_mark_enemy_dead(s, i, (int)mk[i] == 2 ? 0 : 1);
    }
}

// ===========================================================================
// Host launchers. `base` = &envs[0].state, `stride` = sizeof(Env) (framework
// BTF_AT addressing). Grid.x = n envs, block = 32 (one warp per env).
// ===========================================================================
static inline void btg_launch_birth_flush(BtState* base, size_t stride, int n,
        cudaStream_t st) {
    if (n > 0) btg_birth_flush_kernel<<<n, BTG_LANES, 0, st>>>(base, stride, n);
}
static inline void btg_launch_pool_step(BtState* base, size_t stride, int n,
        cudaStream_t st) {
    if (n > 0) btg_pool_step_kernel<<<n, BTG_LANES, 0, st>>>(base, stride, n);
}
static inline void btg_launch_spawn_poll(BtState* base, size_t stride, int n,
        cudaStream_t st) {
    if (n > 0) btg_spawn_poll_kernel<<<n, BTG_LANES, 0, st>>>(base, stride, n);
}
static inline void btg_launch_birth_advance(BtState* base, size_t stride, int n,
        cudaStream_t st) {
    if (n > 0) btg_birth_advance_kernel<<<n, BTG_LANES, 0, st>>>(base, stride, n);
}
static inline void btg_launch_loot(BtState* base, size_t stride, int n,
        cudaStream_t st) {
    if (n > 0) btg_loot_kernel<<<n, BTG_LANES, 0, st>>>(base, stride, n);
}
/* Dispatcher entry points (run bt_enemy_lifecycle_tick between them). */
static inline void btg_launch_loot_consumables(BtState* base, size_t stride, int n,
        cudaStream_t st) {
    if (n > 0) btg_loot_consumables_kernel<<<n, BTG_LANES, 0, st>>>(base, stride, n);
}
static inline void btg_launch_loot_materials(BtState* base, size_t stride, int n,
        cudaStream_t st) {
    if (n > 0) btg_loot_materials_kernel<<<n, BTG_LANES, 0, st>>>(base, stride, n);
}
static inline void btg_launch_deaths(BtState* base, size_t stride, int n,
        const unsigned char* mask, cudaStream_t st) {
    if (n > 0) btg_deaths_kernel<<<n, BTG_LANES, 0, st>>>(base, stride, n, mask);
}
static inline void btg_launch_reserve_slot(BtState* base, size_t stride, int n,
        int* out_slot, cudaStream_t st) {
    if (n > 0) btg_reserve_slot_kernel<<<n, BTG_LANES, 0, st>>>(base, stride, n, out_slot);
}
static inline void btg_launch_group_perf_cull(BtState* base, size_t stride, int n,
        cudaStream_t st) {
    if (n > 0) btg_group_perf_cull_kernel<<<n, BTG_LANES, 0, st>>>(base, stride, n);
}
static inline void btg_launch_struct_pool_tick(BtState* base, size_t stride, int n,
        cudaStream_t st) {
    if (n > 0) btg_struct_pool_tick_kernel<<<n, BTG_LANES, 0, st>>>(base, stride, n);
}

#endif // PUFFER_BROTATO_GPU_SPAWN_CUH
