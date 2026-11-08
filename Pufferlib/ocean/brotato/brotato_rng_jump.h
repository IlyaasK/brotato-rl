/* brotato_rng_jump.h -- PCG32 jump-ahead for the shared Brotato core, plus the
 * per-phase draw-count functions and the counts -> offsets -> per-lane-stream
 * device helper that make an intra-env RNG phase runnable in parallel.
 *
 * OWNERSHIP (GpuRng)
 *   This file never re-implements a line of simulation: every count function
 *   reads the same predicates (and cites the same file:line) as the serial
 *   core function it mirrors, and the jump-ahead is the standard PCG LCG
 *   advance applied to the engine's own update (brotato_rng.h:
 *   state' = state * BT_RNG_MULT + inc). Both core streams -- s->rng (the
 *   Godot global Math::default_rand) and s->utils_rng (the Utils singleton
 *   RandomNumberGenerator) -- advance with that same recurrence, so ONE jump
 *   function covers both.
 *
 * INTEGRATION CONTRACT (see ocean/brotato/brotato_gpu_framework.cuh)
 *   BT_HD BtRng btf_rng_advance(BtRng r, bt_u64 delta);
 *   returns the stream state AFTER `delta` bt_rng_next() calls, in O(log
 *   delta). btf_draw_parallel uses it for pass-2 lane landing and for the
 *   final `*rng = btf_rng_advance(*rng, total)`.
 *
 * WHAT MAKES A PHASE PARALLELISABLE (framework rule, restated precisely)
 *   (a) PARALLEL      consumer i's draw COUNT is a function of pre-phase state
 *                     only: not of a value another consumer draws this phase,
 *                     not of state another consumer mutates this phase.
 *   (b) SERIAL_CROSS  the count (or the values) depend on another consumer's
 *                     drawn values or on same-phase cross-body mutation ->
 *                     the ordered apply must resolve it.
 *   (c) SERIAL_SELF   the count is a function of the consumer's OWN drawn
 *                     values: condition (a) technically allows it, but a
 *                     pass-1 count_fn cannot see pass-2 draws, so this runner
 *                     cannot schedule it. It must run serial (or be split so
 *                     the value-dependent part is a second, ordered pass).
 *
 * KNOWN, DOCUMENTED CAVEAT (affects EVERY global-range consumer)
 *   bt_rand_range -> bt_rng_global_range_float -> bt_rng_random_d -> bt_rng_randd
 *   draws 3 words normally but only 1 when the first draw is 0 (probability
 *   2^-32; brotato_rng.h documents this). A count that is a constant 3 is
 *   therefore exact except for that 2^-32 event. bt_rng_randd is the ONLY
 *   variable-count primitive; every other draw is 1 word (bt_rng_next/
 *   bt_rng_bounded/bt_math_randf/bt_rng_randi_range). Count functions here
 *   return the normal-case value (BT_RNG_JUMP_RANGE_DRAWS); a phase using
 *   global rand_range is "count-exact modulo 2^-32 per draw".
 *
 *   AGREED EXACTNESS PIPELINE (with PerfGpuKernel; see
 *   local://btf_rng_phase_map.md): the fast path counts every bt_rand_range as
 *   BT_RNG_JUMP_RANGE_DRAWS; each consumer tests the FIRST u32 of every
 *   bt_rand_range call and, on zero (it consumed 1, not 3), atomicOr's a
 *   device-wide deviation flag and stops touching state; the driver then re-runs
 *   ONLY that env's phase for that tick through the serial core function (the
 *   definition of correct). Loud, exact, never silent. Phases whose counts are
 *   ALL pre-phase-decidable (every term is bt_chance(p!=0)/bt_rng_bounded(b>0)/
 *   bt_math_randf/bt_rng_randi_range, or a fixed constant) never take the slow
 *   path.
 *
 * No allocation, no I/O, no libc. Integer-only. Host + CUDA + MSL safe.
 */
#pragma once

/* Include sentinel for consumers (e.g. brotato_gpu_spawn.cuh) that must NOT
 * define a fallback btf_rng_advance: if BTF_RNG_JUMP_DEFINED is defined then
 * this header was included, so btf_rng_advance and BT_RNG_JUMP_RANGE_DRAWS are
 * available. Test EITHER macro. */
#define BTF_RNG_JUMP_DEFINED 1

#include "brotato_rng.h"          /* BtRng, BT_HD, BT_RNG_MULT, bt_rng_next */
#include "brotato_state.h"        /* BtState, BT_WEAPONS, BT_ATTACKS, caps */

/* The jump-ahead (section 1) and the counts -> offsets helper (section 2) are
 * dependency-free: include this header anywhere, including a bare CUDA/MSL TU
 * that only needs btf_rng_advance.
 *
 * The per-phase count functions (sections 3-4) read BtState + the generated
 * tables, which brotato_state.h provides. The two WEAPON-phase adapters at the
 * end additionally need BtPlayerWeaponStats (bt_weapon_stats /
 * bt_weapon_stats_variants), which live in brotato_weapon_stats.h and are only
 * valid once the core's helpers exist. Define BT_RNG_JUMP_HAVE_CORE_STATS
 * before including this header (in a TU that has already included
 * brotato_core.h, e.g. brotato_gpu_framework.cuh) to get them; the pure
 * `_terms` forms are ALWAYS available and are what those adapters call. */
#ifdef BT_RNG_JUMP_HAVE_CORE_STATS
#include "brotato_weapon_stats.h"
#endif

/* Caps that live in headers the core includes later; identical values, so a
 * redefinition by the real header is legal and silent. */
#ifndef BT_LOOT_MAX_GOLDS
#define BT_LOOT_MAX_GOLDS 50
#endif
#ifndef BT_GARDEN_SOUNDS
#define BT_GARDEN_SOUNDS 3
#endif

/* Normal-case draw count of bt_rand_range (FP64 randd = 3 words). Guarded so a
 * consumer that pre-defines it (or defines a fallback) never redefines it. */
#ifndef BT_RNG_JUMP_RANGE_DRAWS
#define BT_RNG_JUMP_RANGE_DRAWS 3u
#endif

/* ===========================================================================
 * 1. PCG32 jump-ahead
 * ========================================================================= */

/* The engine's LCG: state_{n+1} = state_n * M + inc, M = BT_RNG_MULT. Hence
 *   state_{n+k} = state_n * M^k + inc * (M^{k-1} + ... + M + 1)
 * The standard forward-advance accumulates (acc_mult, acc_plus) = (M^k, the
 * geometric sum) in O(log k) squarings; `inc` is fixed for the life of the
 * stream (bt_rng_seed sets it once), so only `state` moves. Exact in 64-bit
 * wraparound arithmetic: the CPU, CUDA and MSL forms are bit-identical. */
BT_HD static BtRng btf_rng_advance(BtRng r, bt_u64 delta) {
    bt_u64 acc_mult = (bt_u64)1;
    bt_u64 acc_plus = (bt_u64)0;
    bt_u64 cur_mult = BT_RNG_MULT;
    bt_u64 cur_plus = r.inc;
    while (delta > (bt_u64)0) {
        if ((delta & (bt_u64)1) != (bt_u64)0) {
            acc_mult = acc_mult * cur_mult;
            acc_plus = acc_plus * cur_mult + cur_plus;
        }
        cur_plus = (cur_mult + (bt_u64)1) * cur_plus;
        cur_mult = cur_mult * cur_mult;
        delta >>= 1;
    }
    r.state = acc_mult * r.state + acc_plus;
    return r;
}

/* In-place convenience wrapper (same semantics). */
BT_HD static void bt_rng_advance(BtRng* r, bt_u64 delta) {
    *r = btf_rng_advance(*r, delta);
}

/* A copy of `base` positioned so its NEXT output is the (offset+1)-th output of
 * `base` (offset 0 == base). This is the per-lane stream the runner draws from. */
BT_HD static BtRng bt_rng_copy_at(BtRng base, bt_u64 offset) {
    return btf_rng_advance(base, offset);
}

/* ===========================================================================
 * 2. Device helper: counts -> exclusive prefix sum -> per-lane stream
 *
 * bt_rng_prefix_offsets is the exact serial reference (single thread / one
 * lane walking the whole array). btf_scan_excl in brotato_gpu_framework.cuh is
 * the cooperative over-a-block version used in production; both must produce
 * the SAME offsets, because the runner's output order depends on them.
 * ========================================================================= */

/* Exclusive prefix sum: offsets[i] = sum(counts[0..i)), returns the total.
 * offsets must hold at least n entries. */
BT_HD static bt_u64 bt_rng_prefix_offsets(const unsigned int* counts, int n,
                                          bt_u64* offsets) {
    bt_u64 acc = (bt_u64)0;
    for (int i = 0; i < n; i++) {
        offsets[i] = acc;
        acc += (bt_u64)counts[i];
    }
    return acc;
}

/* Inclusive-then-shift scan with caller scratch (`tmp` >= n entries), usable in
 * any address space (no shared-memory assumption). Deterministic: identical
 * results to bt_rng_prefix_offsets. Offsets may alias nothing. */
BT_HD static bt_u64 bt_rng_scan_hillis(const unsigned int* counts, int n,
                                       bt_u64* tmp, bt_u64* offsets) {
    if (n <= 0) return (bt_u64)0;
    for (int i = 0; i < n; i++) tmp[i] = (bt_u64)counts[i];
    for (int s = 1; s < n; s <<= 1) {
        for (int i = n - 1; i >= s; i--) tmp[i] += tmp[i - s];
    }
    bt_u64 total = tmp[n - 1];
    for (int i = n - 1; i > 0; i--) offsets[i] = tmp[i - 1];
    offsets[0] = (bt_u64)0;
    return total;
}

/* Consumer `lane`'s private stream: `base` advanced by that lane's exclusive
 * offset, so drawing counts[lane] values from it is byte-identical to the
 * serial in-order draws for that consumer. */
BT_HD static BtRng bt_rng_lane_stream(BtRng base, const bt_u64* offsets, int lane) {
    return btf_rng_advance(base, offsets[lane]);
}

/* ===========================================================================
 * 3. Pure draw-count primitives
 * ========================================================================= */

/* bt_chance: one global randf draw iff chance != 0 (brotato_core.h:229). */
BT_HD static unsigned int bt_rngcount_chance(float chance) {
    return chance == 0.0f ? 0u : 1u;
}

/* bt_rand_range / bt_rng_global_range_* : 3 words (see the 2^-32 caveat). */
BT_HD static unsigned int bt_rngcount_global_range(void) {
    return BT_RNG_JUMP_RANGE_DRAWS;
}

/* RandomNumberGenerator::randi_range via the Utils stream: exactly 1 word. */
BT_HD static unsigned int bt_rngcount_utils_randi_range(void) {
    return 1u;
}

/* bt_rng_bounded (Utils.get_rand_element / Array.pick_random): 1 word, drawn
 * even when the pool has a single element. */
BT_HD static unsigned int bt_rngcount_bounded(void) {
    return 1u;
}

/* ===========================================================================
 * 4. Per-phase count functions
 *
 * Each returns the number of GLOBAL-stream words the serial core consumes for
 * that consumer. Utils-stream draws are flagged separately where they occur
 * (the two streams are independent; a phase parallelised on one must NOT
 * reorder the other).
 * ========================================================================= */

/* ---------------------------------------------------------------------------
 * 4a. RANGED weapon shot (one firing slot).
 *     Class (a): every term is a function of pre-phase state.
 *     Mirror of bt_ranged_shoot (brotato_weapon_ranged.h:349-521).
 *       (1) update_current_spread            bt_rand_range            3
 *       (2) shoot sound pick                 1 iff shoot_audio_count>0
 *       (3) per projectile: spread           bt_rand_range            3 * np
 *       (4) _current_cooldown                bt_rand_range            3 unless
 *           is_big_reload_active (nb_shots % additional_cd_every == 0)
 *     `delayed_fresh` is the number of this shot's projectiles that land in a
 *     FRESH DelayedPlayerProjectile cache slot (weapon_projectiles.h:293-296):
 *     each adds one rand_range (3). 0 for every non-delayed scene.
 *     Returns 0 (does not fire) when the slot is not a ready ranged weapon.
 *     The caller ANDs in should_shoot's `in_band` / `(!moving ||
 *     can_move_attack)` terms, which are pre-state and draw nothing.
 * ------------------------------------------------------------------------- */
/* Pure term form (ALWAYS available): the caller resolves the effective stats
 * row (`cur` after the every-x-shots swap) and passes its fields.
 *   shoot_audio_count   BT_WEAPONS[wid].shoot_audio_count
 *   nb_projectiles      cur->nb_projectiles
 *   is_big_reload       add_every != -1 && add_every != 0 && shots % add_every == 0
 *   delayed_fresh       as below
 * Returns 0 when the weapon cannot fire (the caller owns the gate). */
BT_HD static unsigned int bt_rngcount_ranged_shot_terms(int shoot_audio_count,
                                                        int nb_projectiles, int is_big_reload,
                                                        int delayed_fresh) {
    unsigned int c = BT_RNG_JUMP_RANGE_DRAWS;                 /* (1) spread  */
    if (shoot_audio_count > 0) c += 1u;                       /* (2) sound   */
    if (nb_projectiles > 0) c += (unsigned int)nb_projectiles * BT_RNG_JUMP_RANGE_DRAWS;
    if (!is_big_reload) c += BT_RNG_JUMP_RANGE_DRAWS;         /* (4) cd      */
    if (delayed_fresh > 0) c += (unsigned int)delayed_fresh * BT_RNG_JUMP_RANGE_DRAWS;
    return c;
}

/* Pure term form (ALWAYS available) for 4b: is_big_reload as above. */
BT_HD static unsigned int bt_rngcount_melee_swing_terms(int has_target, int is_big_reload) {
    unsigned int c = 0u;
    if (has_target) c += BT_RNG_JUMP_RANGE_DRAWS;             /* accuracy spread */
    if (!is_big_reload) c += BT_RNG_JUMP_RANGE_DRAWS;         /* cooldown        */
    return c;
}

#ifdef BT_RNG_JUMP_HAVE_CORE_STATS
/* State adapter: resolves `cur` exactly as bt_ranged_shoot does and applies the
 * pure term form above. */
BT_HD static unsigned int bt_rngcount_ranged_shot(BT_DEVICE BtState* s, int slot,
                                                  int delayed_fresh) {
    if (slot < 0 || slot >= BT_SLOTS) return 0u;
    unsigned short wid = s->wpn_id[slot];
    if (wid == BT_WEAPON_NONE || wid >= BT_NWEAPONS) return 0u;
    if (BT_WEAPONS[wid].kind != BT_WEAPON_KIND_RANGED) return 0u;
    if (s->wrng_shoot[slot]) return 0u;          /* already mid-shot */
    if (s->wpn_cd[slot] > 0.0f) return 0u;       /* cooldown gate */
    BtPlayerWeaponStats st;
    if (!bt_weapon_stats(s, slot, &st)) return 0u;
    if (st.kind != BT_WEAPON_KIND_RANGED) return 0u;

    int shots = (int)s->wrng_shots[slot] + 1;    /* weapon.gd:294 pre-increment */
    BtPlayerWeaponStats var[BT_WSTATS_MAX_BONUS];
    int vx[BT_WSTATS_MAX_BONUS];
    int nv = bt_weapon_stats_variants(s, slot, vx, var, BT_WSTATS_MAX_BONUS);
    BT_CONST_PTR(BtPlayerWeaponStats) cur = &st;
    for (int v = 0; v < nv; v++)
        if (vx[v] > 0 && (shots % vx[v]) == 0) cur = &var[v];

    int big = (cur->additional_cd_every != -1 && cur->additional_cd_every != 0 &&
               (shots % cur->additional_cd_every) == 0);
    return bt_rngcount_ranged_shot_terms((int)BT_WEAPONS[wid].shoot_audio_count,
                                         (int)cur->nb_projectiles, big, delayed_fresh);
}

/* State adapter for 4b. */
BT_HD static unsigned int bt_rngcount_melee_swing(BT_DEVICE BtState* s, int slot,
                                                  int has_target) {
    if (slot < 0 || slot >= BT_SLOTS) return 0u;
    unsigned short wid = s->wpn_id[slot];
    if (wid == BT_WEAPON_NONE || wid >= BT_NWEAPONS) return 0u;
    if (BT_WEAPONS[wid].kind != BT_WEAPON_KIND_MELEE) return 0u;
    if (s->wpn_melee_shooting[slot]) return 0u;
    if (s->wpn_cd[slot] > 0.0f) return 0u;
    BtPlayerWeaponStats st;
    if (!bt_weapon_stats(s, slot, &st)) return 0u;
    int shots = (int)s->wpn_melee_shots[slot] + 1;
    int big = (st.additional_cd_every != -1 && st.additional_cd_every != 0 &&
               (shots % st.additional_cd_every) == 0);
    return bt_rngcount_melee_swing_terms(has_target, big);
}
#endif /* BT_RNG_JUMP_HAVE_CORE_STATS */

/* ---------------------------------------------------------------------------
 * 4b. MELEE swing start (one slot). Class (a).
 *     Mirror of bt_melee_begin_swing (brotato_weapon_melee.h:665-735).
 *       accuracy spread  bt_rand_range  3  (only with a live target)
 *       cooldown         bt_rand_range  3  (unless big-reload)
 *     The per-enemy hits it later produces are a SEPARATE phase (class (c)).
 *     bt_rngcount_melee_swing_terms / bt_rngcount_melee_swing (adapter) above.
 * ------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------
 * 4c. ENEMY shot (one shoot-behavior enemy). Class (a) for the shot itself.
 *     Mirror of the BT_BEH_SHOOT branch of bt_core_step (brotato_core.h:
 *     4283-4299) + bt_fire_shot (brotato_core.h:3783-3862).
 *       cooldown   bt_rand_range  3 (0 when eshots >= long_cd_every)
 *       base_rand  bt_rand_range  3 (always)
 *       +1 bounded when base_dir_const && !alt_base_dir_spread
 *       rand_rot   bt_rand_range  3 (always)
 *       +3 when constant_spread_rand_base_pos > 0
 *       per projectile k: border case 3 (angle = rand_range) else (k != 0 ||
 *         !atleast_one_on_target) ? 6 : 0;  rot 3;  +3 random_dir;  +3 speed jit
 *     `in_range` is d2 in [min_range^2, max_range^2]. `delayed_fresh` as 4a.
 *     `BT_BEH_SHOOT` and the per-kind state row are read exactly as the core's
 *     pre-escalation snapshot; the escalation's own init_cd draw is owned by
 *     the enemy-init phase (see the table in the report / the test).
 * ------------------------------------------------------------------------- */
BT_HD static unsigned int bt_rngcount_enemy_fire(BT_CONST_PTR(BtAttack) A, int eshots,
                                                 int delayed_fresh) {
    unsigned int c = 0u;
    int long_reload = (A->long_cd_every != 0 && eshots >= (int)A->long_cd_every);
    if (!long_reload) c += BT_RNG_JUMP_RANGE_DRAWS;                  /* cooldown */
    c += BT_RNG_JUMP_RANGE_DRAWS;                                    /* base_rand */
    if (A->base_dir_const && !A->alt_base_dir_spread) c += 1u;
    c += BT_RNG_JUMP_RANGE_DRAWS;                                    /* rand_rot */
    if (A->constant_spread_rand_base_pos > 0.0f) c += BT_RNG_JUMP_RANGE_DRAWS;
    int nproj = A->nproj ? (int)A->nproj : 1;
    if (nproj > 24) nproj = 24;
    for (int k = 0; k < nproj; k++) {
        if (A->spawn_only_borders) c += BT_RNG_JUMP_RANGE_DRAWS;      /* a = rand_range */
        else if (!A->atleast_one_on_target || k != 0) c += 6u;        /* posx, posy */
        c += BT_RNG_JUMP_RANGE_DRAWS;                                /* rot */
        if (A->random_dir) c += BT_RNG_JUMP_RANGE_DRAWS;
        if (A->proj_speed_rand > 0) c += BT_RNG_JUMP_RANGE_DRAWS;
    }
    if (delayed_fresh > 0) c += (unsigned int)delayed_fresh * BT_RNG_JUMP_RANGE_DRAWS;
    return c;
}

BT_HD static unsigned int bt_rngcount_enemy_shoot(BT_DEVICE BtState* s, int i,
                                                  int in_range, int delayed_fresh) {
    if (i < 0 || i >= BT_MAX || !s->ealive[i] || s->eneutral[i]) return 0u;
    int kind = (int)s->ekind[i];
    BT_CONST_PTR(BtAttack) A = &BT_ATTACKS[kind];
    int ns = (int)BT_NKIND_STATES[kind];
    if (ns > 0 && (int)s->estate[i] < ns) A = &BT_ATTACK_STATES[kind][s->estate[i]].atk;
    if ((int)A->beh != BT_BEH_SHOOT) return 0u;
    if (s->eatk[i] > 0.0f) return 0u;
    if (!in_range) return 0u;
    return bt_rngcount_enemy_fire(A, (int)s->eshots[i], delayed_fresh);
}

/* ENEMY charge retarget (brotato_core.h:4322-4349). Class (a): only the
 * telegraph-expiry branch samples a direction, and the draw count is fixed by
 * the table's charge_target (0 PLAYER = none, 1 AROUND_PLAYER = 2 rand_range,
 * 2 RAND_POINT = bt_rand_pos = 2 rand_range). */
BT_HD static unsigned int bt_rngcount_enemy_charge_retarget(int ctarget) {
    return (ctarget == 1 || ctarget == 2) ? (2u * BT_RNG_JUMP_RANGE_DRAWS) : 0u;
}

/* ENEMY spawn attack (brotato_core.h:4350-4374). Class (a): cooldown 3, then
 * per spawned unit either bt_rand_pos (spawn_at_random_pos) or bt_pos_in_area
 * (spawn_radius != -1) = 2 rand_range each, else 0. */
BT_HD static unsigned int bt_rngcount_enemy_spawn_attack(BT_CONST_PTR(BtAttack) A) {
    unsigned int c = BT_RNG_JUMP_RANGE_DRAWS;
    int nb = (int)A->nproj;
    for (int k = 0; k < nb; k++) {
        if (A->spawn_at_random_pos) c += 2u * BT_RNG_JUMP_RANGE_DRAWS;
        else if ((int)A->spawn_radius != -1) c += 2u * BT_RNG_JUMP_RANGE_DRAWS;
    }
    return c;
}

/* ENEMY buffer boost (brotato_core.h:3893-3940). The BoostTimer timeout runs a
 * reservoir pick whose draw count is one bounded word per SCANNED eligible
 * enemy, and each successful pick flips that enemy's eboosted flag -- changing
 * the next pick's scan. Class (b)/(c): the count is cross-consumer AND
 * own-value dependent. `eligible_scanned` is the first pick's scan length. */
BT_HD static unsigned int bt_rngcount_buffer_boost(int eligible_scanned) {
    return eligible_scanned > 0 ? (unsigned int)eligible_scanned : 0u;
}

/* ---------------------------------------------------------------------------
 * 4d. ROLLS -- crit / dodge / lifesteal / burn. All class (a) for the ROLL
 *     itself (the count never depends on a draw); the effects they branch into
 *     are (b)/(c) and belong to the ordered apply.
 * ------------------------------------------------------------------------- */

/* unit.gd:285 / :292 / :581 -- bt_chance(crit_chance). */
BT_HD static unsigned int bt_rngcount_crit(float crit_chance) {
    return bt_rngcount_chance(crit_chance);
}

/* player.gd:302 -- `dodgeable and randf() < dodge`: the raw draw happens
 * whenever the hit is dodgeable, even at dodge == 0 (no zero guard). */
BT_HD static unsigned int bt_rngcount_dodge(int dodgeable) {
    return dodgeable ? 1u : 0u;
}

/* weapon.gd:412 (melee) / player_projectile.gd:105 (ranged) -- one randf per
 * weapon hit, unconditional (never short-circuited by lifesteal == 0 or an
 * active LifestealTimer). */
BT_HD static unsigned int bt_rngcount_lifesteal(void) {
    return 1u;
}

/* unit.gd:543-546 -- bt_wfx_weapon_burn_apply: draws iff can_burn > 0 AND the
 * shot's own burn chance is non-zero. */
BT_HD static unsigned int bt_rngcount_burn(float can_burn, float burn_chance) {
    return (can_burn > 0.0f && burn_chance != 0.0f) ? 1u : 0u;
}

/* the friendly-projectile burn gate in bt_core_step (brotato_core.h:4498) has
 * no can_burn term: bt_chance(prburn_chance). */
BT_HD static unsigned int bt_rngcount_projectile_burn(float burn_chance) {
    return burn_chance != 0.0f ? 1u : 0u;
}

/* unit.gd:282-323 take_damage's sound selection, reached by every weapon / item
 * / pet hit: the hurt clip pick ALWAYS (0 when the clip span is empty) and an
 * ADDITIONAL crit clip pick when the hit crits. The positional play uses the
 * fixed BT_AUDIO_PITCH_RANGE_02 constant, so it draws nothing. Because the crit
 * flag is a drawn value, the count is class (c) for the whole hit procedure --
 * `is_crit` is passed in for callers that already resolved it. */
BT_HD static unsigned int bt_rngcount_enemy_hit_sound(int is_crit, int hurt_count,
                                                      int crit_count) {
    unsigned int c = hurt_count > 0 ? 1u : 0u;
    if (is_crit && crit_count > 0) c += 1u;
    return c;
}

/* ---------------------------------------------------------------------------
 * 4e. LOOT / DROPS.
 *
 * The gold path is split because only the two ROLLS are class (a); the number
 * of coin entities is class (c) (it depends on the drawn residual's value).
 * ------------------------------------------------------------------------- */

/* main.gd:600-620 -- bt_loot_gold_drop: neutral returns 1 with NO draw; every
 * enemy roll draws once (chance is always >= 0.5). */
BT_HD static unsigned int bt_rngcount_gold_drop(int neutral) {
    return neutral ? 0u : 1u;
}

/* main.gd:700-704 -- the two rolls' draw count is deterministic pre-state:
 * the drop roll (1, enemy) + the residual roll (1 iff value has a fraction).
 * The coin COUNT afterwards is value-dependent -> class (c). */
BT_HD static unsigned int bt_rngcount_gold_rolls(int neutral, float value) {
    unsigned int c = bt_rngcount_gold_drop(neutral);
    /* gold value is never negative, so the GDScript int() truncation equals the
     * floor (main.gd:700 `int(value)`); avoid bt_floorf so this header needs no
     * core-only math helper. */
    if (!neutral && value != (float)(int)value) c += 1u;
    return c;
}

/* main.gd:732-746 -- bt_loot_gold_arc: sprite pick (1) + dist (3) +
 * get_rand_pos_in_area (6) + rotation (3) = 13 per created coin. Class (a)
 * when the coin count is fixed (residual == 0). */
BT_HD static unsigned int bt_rngcount_gold_arc(void) {
    return 1u + BT_RNG_JUMP_RANGE_DRAWS + 2u * BT_RNG_JUMP_RANGE_DRAWS
           + BT_RNG_JUMP_RANGE_DRAWS;
}

/* main.gd:706-714 -- past MAX_GOLDS a random live coin is boosted instead:
 * one bounded pick, no arc draws. */
BT_HD static unsigned int bt_rngcount_gold_boost(void) {
    return 1u;
}

/* item_service.gd:157-203 -- bt_loot_roll_consumable. This is the BRANCH-SET
 * count, i.e. class (c): the drop roll (1) always draws, then EITHER the
 * success branch (Utils.randi_range 1 on the U stream + item roll 1 iff its
 * chance is non-zero + the tier pick 1 when the drawn tier's pool is non-empty;
 * tier UNCOMMON is empty) OR, on failure, the enemy_fruit_drops roll (1 iff
 * non-zero). The consumer must take the ordered/serial path. `drop_chance` is
 * the pre-phase chance; the caller supplies the flags. */
BT_HD static unsigned int bt_rngcount_consumable_rolls(float drop_chance,
                                                       int item_chance_nonzero,
                                                       int tier_pool_nonempty,
                                                       float fruit_chance) {
    unsigned int c = bt_rngcount_chance(drop_chance);   /* the drop roll   */
    c += 1u;                                   /* Utils.randi_range (U)     */
    if (item_chance_nonzero) c += 1u;          /* item roll (success branch)*/
    if (tier_pool_nonempty) c += 1u;           /* tier pick (success branch)*/
    if (fruit_chance != 0.0f) c += 1u;         /* elif roll (failure branch)*/
    return c;
}

/* main.gd:631-645 -- bt_loot_consumable_arc: dist (3) + area (6) = 9. */
BT_HD static unsigned int bt_rngcount_consumable_arc(void) {
    return BT_RNG_JUMP_RANGE_DRAWS + 2u * BT_RNG_JUMP_RANGE_DRAWS;
}

/* ---------------------------------------------------------------------------
 * 4f. STRUCTURES / TURRETS.
 *
 * The turret PROJECTILE volley itself draws NOTHING (brotato_core.h:4648-4717:
 * nearest-target scan + bt_spawn_projectile). Only two structure paths draw.
 * ------------------------------------------------------------------------- */

/* BotOMineEffect landmine drop (brotato_core.h:4662-4673): bt_pos_in_area draws
 * 2 rand_range = 6. Class (a) per drop. */
BT_HD static unsigned int bt_rngcount_landmine_drop(void) {
    return 2u * BT_RNG_JUMP_RANGE_DRAWS;
}

/* Garden emit (brotato_structures.h:51-71): tier pick (1) + dist (3) + two
 * placement rand_range (6) = 10. Class (a). */
BT_HD static unsigned int bt_rngcount_garden_emit(void) {
    return 1u + 3u * BT_RNG_JUMP_RANGE_DRAWS;
}

/* Garden tick (brotato_structures.h:83-105): sound pick (1) + emit (10) +
 * next-cooldown rand_range (3) = 14. Class (a). */
BT_HD static unsigned int bt_rngcount_garden_tick(void) {
    return 1u + bt_rngcount_garden_emit() + BT_RNG_JUMP_RANGE_DRAWS;
}

/* ---------------------------------------------------------------------------
 * 4g. CULLS -- both class (a): the draw count is the reservoir scan length,
 *     a pure function of the pre-phase live-set size.
 * ------------------------------------------------------------------------- */

/* entity_spawner.gd:151-165 RESERVOIR SCAN length: one bounded word per
 * eligible live enemy. TWO callers share this scan shape:
 *   bt_reserve_slot (core.h:650-676)  -- the scan is the ONLY drawing part (the
 *     single kill follows it), so that cull is class (a) end to end;
 *   bt_group_perf_cull (core.h:1052-1081) -- `over` removals, each a scan draw
 *     PLUS bt_mark_enemy_dead(slot, 0) between removals, whose death children
 *     draw bt_pos_in_area (6 words) per spawned child. Which slot is picked is a
 *     drawn value, so the perf cull's PHASE TOTAL is NOT a pre-phase function.
 *     Use bt_rngcount_perf_cull(over, total_death_children) for it. */
BT_HD static unsigned int bt_rngcount_enemy_cull(int eligible_n) {
    return eligible_n > 0 ? (unsigned int)eligible_n : 0u;
}

/* bt_spawn_death_children (core.h:4388-4405): one bt_pos_in_area (2 rand_range =
 * 6 words) per spawned child of the dying kind. */
BT_HD static unsigned int bt_rngcount_death_children(int n_children) {
    return (unsigned int)(n_children > 0 ? n_children : 0)
           * (2u * BT_RNG_JUMP_RANGE_DRAWS);
}

/* bt_group_perf_cull total: `over` reservoir picks (1 word each) + one
 * bt_spawn_death_children term per removal (6 words per child of the removed
 * kind). EXACT once the removed set is known; NOT pre-phase-decidable, because
 * the removed kinds are drawn -- jump-ahead must keep this phase serial/ordered
 * (`over + 6 * total_death_children`). */
BT_HD static unsigned int bt_rngcount_perf_cull(int over, int total_death_children) {
    unsigned int c = over > 0 ? (unsigned int)over : 0u;
    c += bt_rngcount_death_children(total_death_children);
    return c;
}

/* entity_spawner.gd:292-303 -- bt_enforce_structure_cap: one bounded pick per
 * structure removed = max(0, n_struct - BT_MAX_STRUCTS). */
BT_HD static unsigned int bt_rngcount_structure_cull(int n_struct) {
    int over = n_struct - BT_MAX_STRUCTS;
    return over > 0 ? (unsigned int)over : 0u;
}

/* ---------------------------------------------------------------------------
 * 4h. SPAWN phases (bt_spawn_poll / bt_birth_advance / bt_spawn_group).
 *     Class (a) for the fixed terms; the away-from-player RETRY loops in
 *     bt_spawn_group depend on the positions they just drew -> the retry count
 *     is own-value-dependent (class (c)); pass it in.
 * ------------------------------------------------------------------------- */

/* entity_spawner.gd:52-75 + entity_birth.gd:47 -- one popped queue entry turned
 * into an EntityBirth: ONE visual-rotation rand_range (3), plus the structure cap
 * when this is a structure pop (bclass == 1). bt_birth_audio_enqueue stores a
 * CONSTANT pitch range (brotato_birth_audio.h:54), so it draws nothing here; the
 * pitch is drawn later in bt_birth_audio_step. */
BT_HD static unsigned int bt_rngcount_birth_pop(int is_structure_birth, int n_struct) {
    unsigned int c = BT_RNG_JUMP_RANGE_DRAWS;
    if (is_structure_birth) c += bt_rngcount_structure_cull(n_struct);
    return c;
}

/* brotato_birth_audio.h:61-75 bt_birth_audio_step: one global_range draw (3) per
 * sound actually dequeued (requires a free bus and count > 0). */
BT_HD static unsigned int bt_rngcount_birth_audio_step(int dequeued) {
    return (unsigned int)(dequeued > 0 ? dequeued : 0) * BT_RNG_JUMP_RANGE_DRAWS;
}

/* entity_spawner.gd:124 + the drain pass: 3 owner-shuffle draws, then the
 * per-queue pop cost. `popped_entries` counts every queue pop this poll;
 * `struct_pops` is how many of those were structure pops, and `n_struct` the
 * live structure count for their cap. */
BT_HD static unsigned int bt_rngcount_spawn_poll(int popped_entries, int struct_pops, int n_struct) {
    unsigned int c = 3u;
    c += (unsigned int)(popped_entries > 0 ? popped_entries : 0)
         * (2u * BT_RNG_JUMP_RANGE_DRAWS);
    if (struct_pops > 0) c += (unsigned int)struct_pops * bt_rngcount_structure_cull(n_struct);
    return c;
}

/* entity_birth.gd:83-106 -- a finished birth either RELOCATES its position with
 * ZoneService.get_rand_pos (2 rand_range = 6) or is marked ready; the ready
 * branch draws NOTHING (its birth-audio enqueue stores a constant pitch). Chosen
 * from pre-phase geometry. */
BT_HD static unsigned int bt_rngcount_birth_advance_finished(int relocated) {
    return relocated ? (2u * BT_RNG_JUMP_RANGE_DRAWS) : 0u;
}

/* entity_spawner.gd:175-209 -- one group unit row: Utils.randi_range (1, U
 * stream) + the residual chance roll (1 iff the pre-phase total has a fraction),
 * then per queued unit a bt_pos_in_area (6) and `retries` away-from-player
 * redraws (6 each; the retry count is own-value-dependent). */
BT_HD static unsigned int bt_rngcount_group_unit(int residual_nonzero, int units_queued,
                                                 int retries) {
    unsigned int c = 1u;                              /* Utils.randi_range */
    if (residual_nonzero) c += 1u;
    if (units_queued > 0) c += (unsigned int)units_queued * (2u * BT_RNG_JUMP_RANGE_DRAWS);
    if (retries > 0) c += (unsigned int)retries * (2u * BT_RNG_JUMP_RANGE_DRAWS);
    return c;
}

/* entity_spawner.gd:223-249 -- bt_group_pos: one bt_rand_pos (6) plus one more
 * per away-from-player retry of the GROUP centre (own-value-dependent). */
BT_HD static unsigned int bt_rngcount_group_pos(int retries) {
    return (unsigned int)(1 + (retries > 0 ? retries : 0)) * (2u * BT_RNG_JUMP_RANGE_DRAWS);
}

/* ---------------------------------------------------------------------------
 * 4i. Scalar / tick rolls that are simple, fixed-per-consumer counts.
 * ------------------------------------------------------------------------- */

/* player.gd:736-738 -- on_healing_effect's hp_regen pick: one draw per heal
 * that actually restored HP AND a non-empty hp_regen sound pool. */
BT_HD static unsigned int bt_rngcount_heal_tick(int restored, int heal_audio_count) {
    return (restored && heal_audio_count > 0) ? 1u : 0u;
}

/* player.gd:850-875 -- alien-eyes emission: one sound pick; the projectiles are
 * evenly spaced (no per-projectile draw). */
BT_HD static unsigned int bt_rngcount_alien_eyes_fire(void) {
    return 1u;
}

/* main.gd:507-525 -- projectiles_on_death: per child, dir rand_range (3) then a
 * special-projectile sound pick (0/1). Class (a) for a fixed child count. */
BT_HD static unsigned int bt_rngcount_proj_on_death_child(int sound_count_nonzero) {
    return BT_RNG_JUMP_RANGE_DRAWS + (sound_count_nonzero ? 1u : 0u);
}
