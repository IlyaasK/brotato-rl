/* Brotato Bull/Danger-5 numerical core - device-portable.
 *
 * This is the ONLY implementation of the simulation. The host wrapper in
 * brotato.h, the CUDA kernels and the Metal kernels all include this header and
 * call these functions on the shared BtState (see brotato_state.h). Nothing
 * here allocates, touches the filesystem, or uses libc: BtState is passed by
 * pointer, observations are written into a caller-provided array, and reset
 * loops replace memset.
 *
 * SOURCE STATUS (brotato_recovered v1.1.14.6 + brotato_abyss):
 *   source-exact: arena 2048x1536, cap 100 + kill-excess (no loot), player
 *   base 10 + Bull 20 = 30, enemy stat scaling on (wave-1) with round(),
 *   absolute speed jitter (unit.gd:81), all D0..D5 wave groups + repeating
 *   acceleration/chance/placement (wave_group_data.gd, entity_spawner.gd),
 *   entity_spawner.gd category queues (pop_back LIFO, SPAWN_DELAY 3-frame drain,
 *   QUEUE_LIMIT flood rule) + entity_birth.gd 60-frame birth countdown with the
 *   player-overlap restart, elite/horde schedule (waves 11-12/14-15/17-18, 40%
 *   horde, per-zone rosters), boss HP factors + HP/elapsed state transitions
 *   (boss.gd), explosion on ANY damage (player.gd:443) with radius 147.34 and
 *   damage 30 + 3*stat, item prices/rerolls (item_service.gd), XP=materials 1:1
 *   with curve (level+4)^2, item box = heal + deferred item, harvesting at wave
 *   end, DLC curse transform (dlc_1_data.gd:48-379) for every item branch whose
 *   target this core simulates, stat ids 0..18 (0..12 + curse + the five source
 *   upgrade stats), weapon/burning scaling from BT_ITEM_SCALING, PhysicsMath
 *   knockback, GodotPhysics contact solver (brotato_physics.h), and the
 *   structure/pet lifecycle: items only REGISTER their StructureEffect/
 *   TurretEffect/BuilderTurretEffect (Keys.structures_hash) and PetEffect
 *   (Keys.stat_pets_hash) instances, and _on_StructureTimer_timeout (1 Hz
 *   main.tscn StructureTimer, per-wave _base_*_spawned flags, MAX_STRUCTURES
 *   100 eviction, EntityService._restrict_turret_count ordering,
 *   WeaponService.apply_structure_attack_speed_effects cadence) queues
 *   spawn_cooldown-copies into the structure/pet queues that the same 3-frame
 *   poll drains into 60-frame EntityBirths. Every curse-transformed summon
 *   field is carried per owned instance (spawn_cooldown, boost_zone_scale,
 *   double_chance, revive_duration, health_boost). Structure/pet damage applies
 *   the source's percent-damage bonus at spawn (structure_percent_damage IN
 *   PLACE OF stat_percent_damage for a structure, on top of it for a
 *   structure-pet) plus the exploding_dmg_bonus of a row whose nested effects[]
 *   holds an ExplodingEffect, the range add is structure_range for a structure
 *   (stat_range for a ranged pet, half of it for a melee pet, all clamped to
 *   MIN_RANGE 25), structures take the player's stat_crit_chance only while
 *   structures_can_crit is owned and every pet/structure shot rolls crit per
 *   target hit before armor, the dlc_1_data.gd:71-97 curse branch order is exact
 *   (spawn_cooldown / weapon stats / nested BurningEffect scaling / own
 *   cooldown), a structure's or pet's own burning_data (turret_flame,
 *   Blazemander) rides its shots and burns the enemy hit on the unit.tscn
 *   BurningTimer cadence (0.5 s scaled by the reduce/increase buckets, one
 *   damage tick per timeout), and the Bull explosion's multiplier is
 *   stat_percent_damage + explosion_damage (weapon_service.gd:245-249,281-286).
 *
 * TARGET: one fully-unlocked, weaponless Bull at Danger 5, campaign or Abyss,
 * through wave 20. Shop actions buy slots 0..3, reroll (4), or leave (8);
 * recycling, locking, stealing, other characters, endless and coop are outside
 * that action/run contract.
 *
 * Enemy projectiles retain their source shapes and lifetime windows. Neutrals
 * have a separate geometry domain, hit-count death rules and tree-turret events.
 * Pets have their source movement/zones and an independent contact world.
 * Loot keeps its push-back/idle/attraction lifecycle; item explosions retain a
 * deferred, hit-once collision window. Effect consumers include temporal stats,
 * burning-kill thresholds/caps, enemy modifiers and consumed shop replacements.
 *
 * RNG: independent global and Utils PCG streams. Global rand_range preserves
 * the pinned engine's binary64 arithmetic (integer emulation on Metal).
 * Matching primitives does not establish whole-game seeded replay agreement:
 * draw ordering, source engine contact ordering and native transcendental
 * rounding remain separate differential checks. Do not claim transfer parity
 * from CPU/Metal agreement alone.
 *
 * ADDRESS SPACES: BtState lives in a device buffer (BT_DEVICE). The physics
 * world is a member of it; a backend that embeds the world this way must NOT
 * define BT_AS_WORLD/BT_AS_PAIRS (the enclosing object already carries the
 * address space).
 */
#pragma once

#include "brotato_state.h"

/* Implementation-capacity (port bound) status: see brotato_capacity.h.  Included
 * here, right after BtState is complete, so every later header in this TU (item
 * explosions, weapon projectiles, ...) can record a loss with BT_CAP_LATCH. */
#include "brotato_capacity.h"
#include "brotato_enemy_pool.h"
/* The structure/pet half of the same source pool: a per-scene LIFO over the
 * s->pet_* slot table (bt_spawn_summon_row / bt_enforce_structure_cap /
 * bt_clear_wave_summons / the landmine death / bt_core_step's pool tick). */
#include "brotato_structure_pool.h"
/* ---------------------------------------------------------------------------
 * Generated-op-list completion: scene-script `_init` repeats and the
 * non-behavior child `_ready` draws.
 *
 * Godot's GDScript constructor runs an Enemy-derived class chain's member
 * initializers once per SCRIPT LEVEL, so a kind whose scene root overrides the
 * script draws enemy.gd:25's `rand_range(1, 3)` once per level: a bare scene
 * (baby_alien.tscn: no script) once, a scripted kind (pursuer.tscn ->
 * enemy.tscn) twice, and a boss scene (kind.tscn -> boss.tscn -> enemy.tscn)
 * three times.  The generated op list carries that initializer exactly once
 * (the BT_INITSITE_MEMBER row of every fresh op span), so the extra repeats
 * are replayed by bt_enemy_init_scene_extra below, which the fresh branch of
 * bt_enemy_init_run calls in the member row's place (the LAST repeat wins the
 * stored value, exactly as the source's last constructor level does).
 *
 * A scene can also hold plain children whose own scripts draw while the node
 * tree enters it (`_ready`).  Those are not movement/attack behaviors, so the
 * generated list has no row for them: predator.tscn's `Pivot` carries nine
 * EnemyProjectileRotating children, each rolling
 * `rand_range(min_target_distance, max_target_distance)` in
 * EnemyProjectile._ready -> shoot (enemy_projectile.gd:5-6,
 * enemy_projectile_rotating.gd:11-14), and pivot.gd's `_ready` spends one
 * `Utils.get_rand_element([-1, 1])`; all ten draws land between the boss's
 * attack-behavior `_ready` cooldowns and its `Unit.init` speed jitter.
 * BT_KIND_SCENE_READY + BT_KIND_SCENE_READY_N replay them in that order.
 *
 * BT_KIND_INIT_REPEAT is derived mechanically, not by observation: for each
 * kind, walk BT_KIND_SCENE's root `instance=` chain down to entity.tscn and
 * count the `script = ExtResource(...)` overrides on the root node of every
 * scene above enemy.tscn (baby_alien.tscn -> enemy.tscn = 0, pursuer.tscn ->
 * enemy.tscn = 1, predator.tscn -> boss.tscn -> enemy.tscn = 2).  Re-run that
 * walk after a `BT_KIND_SCENE` change; the deep-start site log pins the three
 * pre-divergence cases (baby_alien 3 words, invoker 21, predator 49).
 * ------------------------------------------------------------------------- */
BT_TABLE unsigned char BT_KIND_INIT_REPEAT[BT_NKINDS] = {
    0, 1, 1, 0, 0, 1, 0, 0, 1, 0, 1,
    1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0,
    1, 1, 0, 0, 1, 0, 0, 1, 0, 1, 1,
    0, 0, 0, 0, 1, 1, 1, 0, 1, 1, 0,
    0, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2
};
/* { op, a, b }: op 0 = one global `rand_range(a, b)`; op 1 = one global
 * `Math::rand() % (unsigned)a` (Utils.get_rand_element over an a-element
 * array).  Shared list; BT_KIND_SCENE_READY_N indexes it. */
BT_TABLE float BT_KIND_SCENE_READY[][3] = {
    { 0.0f,  250.0f,  250.0f},   /* Pivot/EnemyProjectile    (predator.tscn) */
    { 0.0f,  300.0f,  300.0f},   /* Pivot/EnemyProjectile2 */
    { 0.0f,  350.0f,  350.0f},   /* Pivot/EnemyProjectile3 */
    { 0.0f,  600.0f,  600.0f},   /* Pivot/EnemyProjectile4 */
    { 0.0f,  650.0f,  650.0f},   /* Pivot/EnemyProjectile5 */
    { 0.0f,  700.0f,  700.0f},   /* Pivot/EnemyProjectile6 */
    { 0.0f,  950.0f,  950.0f},   /* Pivot/EnemyProjectile7 */
    { 0.0f, 1000.0f, 1000.0f},   /* Pivot/EnemyProjectile8 */
    { 0.0f, 1050.0f, 1050.0f},   /* Pivot/EnemyProjectile9 */
    { 1.0f,    2.0f,    0.0f},   /* Pivot/gd `_ready` direction */
};
BT_TABLE unsigned char BT_KIND_SCENE_READY_N[BT_NKINDS] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 10
};
/* Replay one `_init` or `_ready` row's draws: `member` repeats a member-bound
 * rand_range, `speed` (the fresh Unit.init jitter row) runs the kind's scene
 * child draws first, exactly where the post-order `_ready` walk puts them. */
BT_HD static void bt_enemy_init_scene_extra(BT_DEVICE BtState* s, int kind,
        unsigned char site, unsigned char dest, unsigned char state,
        bt_u64 lo, bt_u64 hi) {
    if (site == (unsigned char)BT_INITSITE_MEMBER && state == (unsigned char)BT_INIT_ROOT_STATE) {
        for (int q = (int)BT_KIND_INIT_REPEAT[kind]; q > 0; q--)
            (void)bt_rng_global_range_bits(&s->rng, lo, hi);
    } else if (site == (unsigned char)BT_INITSITE_SPEED && dest == (unsigned char)BT_INITDEST_SPEED_RAND) {
        int n = (int)BT_KIND_SCENE_READY_N[kind];
        for (int q = 0; q < n; q++) {
            float op = BT_KIND_SCENE_READY[q][0];
            if (op == 0.0f) (void)bt_rng_global_range_float(&s->rng, BT_KIND_SCENE_READY[q][1],
                                                           BT_KIND_SCENE_READY[q][2]);
            else (void)bt_rng_bounded(&s->rng, (unsigned int)BT_KIND_SCENE_READY[q][1]);
        }
    }
}

#include "brotato_enemy_init.h"
#include "brotato_birth_audio.h"
#include "brotato_player_visual.h"
#include "brotato_combat_audio.h"
/* Endless-mode run-level scaling helpers (bt_endless_*).  Needs only
 * brotato_state.h / brotato_waves.h + a forward decl of bt_loot_endless_factor,
 * so it is safe to pull in before the body (bt_spawn_wave_extras / bt_enter_wave
 * / the wave-end continuation hook use it). */
#include "brotato_endless.h"

#if defined(__METAL_VERSION__)
#include <metal_stdlib>
using namespace metal;
/* MSL `precise::` overloads are the correctly-rounded ones (Metal's default
 * sqrt/sin/cos/atan2 are not bit-comparable with libm). Match
 * brotato_physics_math.h's BT_SQRTF (metal::precise::sqrt). */
#define bt_sqrtf(x)    metal::precise::sqrt(x)
#define bt_fabsf(x)    fabs(x)
#define bt_roundf(x)   metal::precise::round(x)
#define bt_ceilf(x)    metal::precise::ceil(x)
#define bt_floorf(x)   metal::precise::floor(x)
#define bt_atan2f(y, x) metal::precise::atan2((y), (x))
#define bt_cosf(x)     metal::precise::cos(x)
#define bt_sinf(x)     metal::precise::sin(x)
#elif defined(__CUDACC__) || defined(__CUDA_ARCH__) || defined(BT_CUDA)
#include <math.h>
/* Included in BOTH passes, not just the device one: brotato_math_cuda.h defines
 * its tables as `static __device__ const` in the device pass, and cudafe1's
 * entity-registration stub (cudafe1.stub.c) references those symbols from the
 * host pass, which otherwise has no declaration to shadow
 * ("'::bt_mc_sincos_table' has not been declared"). The header's host branch
 * defines the same symbols as plain host consts. */
#include "brotato_math_cuda.h"
#if defined(__CUDA_ARCH__)
/* Device pass: the CUDA math library's sinf/cosf/atan2f are NOT bit-identical
 * to the host libm the CPU build links (glibc's sinf is the __sinf_fma variant,
 * 0.5607 ulp worst case, so even a correctly rounded device sinf would differ
 * in 0.16% of arguments). brotato_math_cuda.h cargo-cults glibc's own
 * algorithms onto the device so that both passes produce the same bits, sqrtf
 * included (its non-NaN result is the same correctly rounded value either way,
 * but glibc and __fsqrt_rn disagree about the NaN bits); the remaining four are
 * exact operations and stay intrinsic. See that header for the measurements and
 * the verification. */
#define bt_sqrtf(x)    bt_mc_sqrtf(x)  /* correctly rounded + glibc's NaN rules */
#define bt_fabsf(x)    fabsf(x)        /* exact */
#define bt_roundf(x)   roundf(x)       /* exact (result always representable) */
#define bt_ceilf(x)    ceilf(x)        /* exact */
#define bt_floorf(x)   floorf(x)       /* exact */
#define bt_atan2f(y, x) bt_mc_atan2f((y), (x))
#define bt_cosf(x)     bt_mc_cosf(x)
#define bt_sinf(x)     bt_mc_sinf(x)
#else
/* Host pass of a .cu: the same libm the CPU build calls, so the host half of a
 * CPU-vs-CUDA comparison is the CPU environment's own arithmetic. */
#define bt_sqrtf(x)    sqrtf(x)
#define bt_fabsf(x)    fabsf(x)
#define bt_roundf(x)   roundf(x)
#define bt_ceilf(x)    ceilf(x)
#define bt_floorf(x)   floorf(x)
#define bt_atan2f(y, x) atan2f((y), (x))
#define bt_cosf(x)     cosf(x)
#define bt_sinf(x)     sinf(x)
#endif
#else
#include <math.h>
#define bt_sqrtf(x)    sqrtf(x)
#define bt_fabsf(x)    fabsf(x)
#define bt_roundf(x)   roundf(x)
#define bt_ceilf(x)    ceilf(x)
#define bt_floorf(x)   floorf(x)
#define bt_atan2f(y, x) atan2f((y), (x))
#define bt_cosf(x)     cosf(x)
#define bt_sinf(x)     sinf(x)
#endif

/* MSL has no FP64 (brotato_rng.h BT_RNG_HAS_F64), so device code that must
 * reproduce an engine expression the engine itself evaluates in binary64
 * (Godot's `real_t` is float, but its GDScript operands and its libm calls are
 * double) can only do so where FP64 exists.  Guards the binary64 forms; the
 * float32 fallback is the documented Metal approximation. */
#if defined(__METAL_VERSION__)
#define BT_HAS_F64 0
#else
#define BT_HAS_F64 1
#endif

/* 8-way movement table; diagonals pre-normalised by 1/sqrt(2). */
BT_TABLE float BT_MOVE_X[BT_ACTIONS] = {0.0f, 0.0f, 0.70710678f, 1.0f, 0.70710678f, 0.0f, -0.70710678f, -1.0f, -0.70710678f};
BT_TABLE float BT_MOVE_Y[BT_ACTIONS] = {0.0f, -1.0f, -0.70710678f, 0.0f, 0.70710678f, 1.0f, 0.70710678f, 0.0f, -0.70710678f};

/* run_data.gd:30-48 primary_stats_list mapped to core stat ids, used by
 * "gain N random primary stats" effects (uniform pick over all 16). */
BT_TABLE unsigned char BT_PRIMARY_STATS[16] = {0, 2, 9, 10, 15, 8, 1, 17, 6, 5, 4, 16, 18, 7, 3, 12};

/* run_data.gd:1351-1358 get_armor_coef -- the PLAYER's damage taken multiplier
 * (player.gd:310-311 overrides unit.gd:502's flat subtraction), not a flat
 * subtraction.  Negative armor inverts the coefficient:
 *   p = 10 / (10 + |armor| / 1.5)
 *   if armor < 0: p = (1 - p) + 1        (more than 100% damage taken) */
BT_HD static float bt_armor_mult(float armor) {
    float p = 10.0f / (10.0f + bt_fabsf(armor) * 0.6666667f);
    if (armor < 0.0f) p = (1.0f - p) + 1.0f;
    return p;
}

/* run_data.gd:1364 get_hp_regeneration_timer. */
BT_HD static float bt_regen_period(float regen) {
    int whole = (int)regen;
    if (whole <= 0) return 99.0f;
    return 5.0f / (1.0f + bt_fabsf((float)(whole - 1)) / 2.25f);
}

/* player.gd:565-573 get_iframes -- MIN_IFRAMES 0.2, MAX_IFRAMES 0.4, each
 * DIVIDED by max(1, RunData.get_endless_factor()) (the run's current wave), so
 * post-wave-20 endless shrinks the invincibility window with the endless
 * scaling.  get_endless_factor is 0 for waves <= nb_of_waves, so waves 1..20
 * keep the plain 0.2..0.4 clamp. */
BT_HD static float bt_loot_endless_factor(int wave);   /* brotato_loot.h */
BT_HD static short bt_character_next_level_xp_pct(unsigned short character);  /* brotato_character_runtime.h */
BT_HD static float bt_iframes(BT_DEVICE BtState* s, float dmg, float max_hp) {
    float ef = bt_loot_endless_factor(s->wave);
    if (ef < 1.0f) ef = 1.0f;
    float lo = 0.2f / ef;
    float hi = 0.4f / ef;
    float f = ((dmg / max_hp) * hi) / 0.15f;
    if (f < lo) f = lo;
    if (f > hi) f = hi;
    return f;
}

/* player.gd:438 `_invincibility_timer.start(get_iframes(dmg_taken))` -- the
 * Player's InvincibilityTimer, converted to the number of RUNNING states the
 * core must report.
 *
 * Godot's Timer holds `double time_left` (scene/main/timer.h:45) while its
 * internal physics notification subtracts the float32 physics delta
 * (scene/main/timer.cpp:65-78; SceneTree::physics_process_time is a float), and
 * that notification precedes this iteration's script callbacks.  In binary64 the
 * 0.2f window goes negative on the 12th decrement (0.2f - 11*BT_DT =
 * +1.6666660085320473e-02, the 12th = -7.4505805969238282e-09), whereas a
 * float32 accumulator stays positive (+1.1175871e-08) and would need a 13th --
 * so the value stored here is binary64-derived, not a float32 countdown.
 *
 * The trace oracle (migration/game/rl_trace.contact161_normal.jsonl, first
 * player damage on tick 278) reads the timer as running on ticks 278..288 (11
 * rows) and stopped from 289.  The core's state for tick N corresponds to the
 * game's sample one iteration after the damage (the deferred
 * hurt_area_entered_deferred runs in the MessageQueue flush, after this frame's
 * snapshots), so the count kept here is one less than the number of double
 * decrements the Timer performs before time_left < 0.  A tick count is exact in
 * float32 for every window get_iframes can produce (<= 0.4 / BT_DT + 1). */
BT_HD static float bt_iframe_frames(float wait_time) {
    double t = (double)wait_time;
    const double d = (double)BT_DT;
    int n = 0;
    while (t >= 0.0 && n < 4096) { t -= d; n++; }
    return (float)(n > 1 ? n - 1 : n);
}

/* Godot's Timer (scene/main/timer.h:45 `double time_left`, timer.cpp:65-78
 * internal physics notification) subtracts the FLOAT32 physics delta from a
 * BINARY64 time_left and fires when it goes negative; a repeating timer then
 * does `time_left += wait_time`.  N = the number of decrements until that first
 * happens, i.e. the frame count the sim must run the countdown for.  A float32
 * seconds accumulator is one frame late for the scene wait_times in use
 * (measured: 61 ticks for 1.0 s and 241 for 4.0 s vs the engine's 60/240, and
 * the engine probe's autostart Timer with wait_time 1.0 fires at physics frame
 * 60), so the timers count FRAMES exactly like the invincibility Timer. */
BT_HD static float bt_timer_frames(float wait_time) {
    if (wait_time <= 0.0f) return 0.0f;
    double t = (double)wait_time;
    const double d = (double)BT_DT;
    int n = 0;
    while (t >= 0.0 && n < 65535) { t -= d; n++; }
    return (float)n;
}

BT_HD static void bt_zero_u8(BT_DEVICE unsigned char* p, int n) {
    for (int i = 0; i < n; i++) p[i] = 0;
}

/* Global GDScript rand_range evaluates its three-draw expression in binary64. */
BT_HD static float bt_rand_range(BT_DEVICE BtState* s, float lo, float hi) {
    return bt_rng_global_range_float(&s->rng, lo, hi);
}

/* utils.gd:397-403: only zero short-circuits; even certainty consumes a draw. */
BT_HD static int bt_chance(BT_DEVICE BtState* s, float chance) {
    return chance == 0.0f ? 0 : bt_math_randf(&s->rng) <= chance;
}

/* The same source helper where the CALLER builds the threshold as an integer
 * percent in GDScript binary64 (`Utils.get_chance_success(float(chance) / 100)`,
 * main.gd:254-260; `effect.value / 100.0`, unit.gd:292).  GDScript's floats are
 * binary64, so `randf() <= chance` compares the float32 draw against a binary64
 * quotient; dividing in float32 instead moves the threshold by up to 1 ulp and
 * flips the outcome when the draw lands exactly on it.  MSL has no FP64, so the
 * equivalence holds on every backend that reproduces the engine and the float32
 * form is the documented approximation there (BT_HAS_F64). */
BT_HD static int bt_chance_pct(BT_DEVICE BtState* s, int percent) {
    if (percent == 0) return 0;
    float r = bt_math_randf(&s->rng);
#if BT_HAS_F64
    return (double)r <= (double)percent / 100.0 ? 1 : 0;
#else
    return r <= (float)percent / 100.0f ? 1 : 0;
#endif
}

/* Unit stat table for a slot: BT_KINDS for enemies, BT_NEUTRALS for trees. */
BT_HD static BT_CONST_PTR(BtUnitKind) bt_kunit(BT_DEVICE BtState* s, int slot) {
    return s->eneutral[slot] ? &BT_NEUTRALS[s->ekind[slot]] : &BT_KINDS[s->ekind[slot]];
}

BT_HD static BT_CONST_PTR(BtBodyPhysics) bt_kgeom(BT_DEVICE BtState* s, int slot) {
    /* The per-slot row exists ONLY for a live self-boosted size: eboost_n > 0
     * means bt_egeom_refresh wrote this life's scaled row (it is zeroed by
     * every spawn and by bt_core_reset), and only the kinds whose scene sets
     * size_on_boost can ever have a scaled row.  Everything else -- neutral
     * trees, every other kind, and any slot that was never spawned this life
     * (fixtures that fill ekind/ealive by hand) -- resolves to the SAME
     * constant table row as before, so an uninitialised egeom entry can never
     * be read and the pointer identity of the kind rows is preserved. */
    if (!s->eneutral[slot] && s->eboost_n[slot] > 0 &&
        BT_KIND_SELFBOOST[s->ekind[slot]].size > 0.0f &&
        s->egeom_kind[slot] == s->ekind[slot] && s->egeom_n[slot] == s->eboost_n[slot])
        return &s->egeom[slot];
    return s->eneutral[slot] ? &BT_NEUTRAL_PHYSICS[s->ekind[slot]]
                            : &BT_PHYSICS[bt_geom_index(s->ekind[slot])];
}

/* pursuer.gd:75-83 set_size(p_size_boost): hitbox_scaling = size_boost * 0.25
 * is ADDED to the Collision (body), Hurtbox and Hitbox shape scales, so the
 * effective radii grow by (1 + 0.25 * size_on_boost * boosts_ever).  The
 * sprite's own scale/position are visual only.  Called at spawn and after every
 * self-boost; the kind's constant row is the base for every other kind. */
BT_HD static void bt_egeom_refresh(BT_DEVICE BtState* s, int slot) {
    BT_CONST_PTR(BtBodyPhysics) K = s->eneutral[slot]
        ? &BT_NEUTRAL_PHYSICS[s->ekind[slot]] : &BT_PHYSICS[bt_geom_index(s->ekind[slot])];
    float scale = 1.0f;
    if (!s->eneutral[slot] && BT_KIND_SELFBOOST[s->ekind[slot]].size > 0.0f)
        scale += 0.25f * BT_KIND_SELFBOOST[s->ekind[slot]].size * (float)s->eboost_n[slot];
    BtBodyPhysics R = *K;
    if (scale != 1.0f) {
        R.body_radius = K->body_radius * scale;
        R.hurt_radius = K->hurt_radius * scale;
        R.hit_radius = K->hit_radius * scale;
    }
    s->egeom[slot] = R;
    s->egeom_kind[slot] = s->ekind[slot];
    s->egeom_n[slot] = s->eboost_n[slot];
}

BT_HD static void bt_pet_world_init(BT_DEVICE BtState* s);
/* defined later (brotato_enemy_motion.h); resets a spawned instance's movement
 * node state on a fresh spawn and Unit.bonus_speed on every spawn */
BT_HD static void bt_enemy_motion_spawn(BT_DEVICE BtState* s, int slot, int fresh);
/* defined later (brotato_enemy_motion.h): links a freshly spawned iron_lung to
 * the stargazer whose source_spawner emitted it (stargazer.gd:46-54) */
BT_HD static void bt_enemy_motion_link_spawner(BT_DEVICE BtState* s, int spawned);
/* defined later (brotato_enemy_motion.h): the active MovementBehavior's vector,
 * read by the flying reaction volley's shoot_in_unit_direction */
BT_HD static BtVec2 bt_enemy_move(BT_DEVICE BtState* s, int slot);
/* defined later (brotato_enemy_lifecycle.h): the per-death linkage/release hook
 * and the per-frame lifecycle tick */
BT_HD static void bt_enemy_lifecycle_on_death(BT_DEVICE BtState* s, int slot);
BT_HD static float bt_enemy_death_gold_value(BT_DEVICE BtState* s, int slot, float base);

/* defined later; used by the wave-start bucket consumer */
BT_HD static void bt_core_stat_add(BT_DEVICE BtState* s, int id, float v);
/* defined later; temporary (TempStats) add/remove + re-derive the observable
 * stat fields from the raw/temp/link/gain layers */
BT_HD static void bt_core_stat_add_temp(BT_DEVICE BtState* s, int id, float v);
BT_HD static void bt_core_stat_sync(BT_DEVICE BtState* s);
BT_HD static void bt_core_stat_reset_temp(BT_DEVICE BtState* s);
/* defined later; deferred player LiveStats refresh + fresh-player reset
 * (player.gd:349-368 update_player_stats), used by bt_core_reset/bt_core_init */
BT_HD static void bt_player_stats_refresh(BT_DEVICE BtState* s);
BT_HD static void bt_player_stats_reset(BT_DEVICE BtState* s);
BT_HD static void bt_character_apply(BT_DEVICE BtState* s);
BT_HD static void bt_character_query_links(BT_DEVICE BtState* s);
BT_HD static void bt_character_starting_apply(BT_DEVICE BtState* s);
BT_HD static int bt_character_explode_on_hit(unsigned short character);
BT_HD static float bt_character_explode_chance(unsigned short character);
BT_HD static unsigned short bt_character_ban_group_mask(unsigned short character);
BT_HD static int bt_character_item_banned(unsigned short character, unsigned short item);
BT_HD static void bt_weapon_reset(BT_DEVICE BtState* s);
BT_HD static void bt_melee_reset(BT_DEVICE BtState* s);
BT_HD static void bt_ranged_reset(BT_DEVICE BtState* s);
BT_HD static void bt_wproj_reset(BT_DEVICE BtState* s);
BT_HD static void bt_wfx_reset(BT_DEVICE BtWeaponFxState* fx);
BT_HD static void bt_wfx_reset_wave(BT_DEVICE BtWeaponFxState* fx);
/* Wave boundary: a fresh game scene re-creates every player Weapon node, so each
 * copy gets a fresh wave-begin cooldown draw in slot order (brotato_weapon_
 * inventory.h dispatches bt_weapon_melee_wave_begin_slot / bt_ranged_wave_
 * begin_slot).  `player.gd reset_weapons_cd` has no caller in the recovered
 * sources and is not modelled. */
BT_HD static void bt_weapon_wave_begin(BT_DEVICE BtState* s);
BT_HD int bt_weapon_observe_append(BT_DEVICE BtState* s, BT_DEVICE float* o, int b);
/* defined later; wave start clears the structure/pet entities and restarts the
 * entity_spawner StructureTimer lifecycle flags */
BT_HD static void bt_clear_wave_summons(BT_DEVICE BtState* s);
/* curse helpers (defined with the curse block, used earlier by pets/burning) */
BT_HD static float bt_curse_modifier(BT_DEVICE BtState* s);
BT_HD static int bt_curse_stat(float value, float mod);
BT_HD static float bt_curse_value_f(float value, int eff_sign, float mod, int override_sign, int overridden_sign);
/* stats with no core stat id: scalar_effect buckets under their emitted keys
 * (defined with the summon/burning block, used earlier by bt_explode) */
BT_HD static float bt_structure_percent_damage(BT_DEVICE BtState* s);
BT_HD static float bt_structure_range(BT_DEVICE BtState* s);
BT_HD static float bt_explosion_damage(BT_DEVICE BtState* s);
BT_HD static int bt_structures_can_crit(BT_DEVICE BtState* s);
/* brotato_scalar_events.h consumers (included by this header after
 * brotato_progression.h, before bt_core_step; ScalarEvents owns them and
 * bt_explode uses them instead of duplicating the math) */
BT_HD static float bt_scalar_explosion_damage_mult(BT_DEVICE BtState* s);
BT_HD static void bt_drain_death_events(BT_DEVICE BtState* s);
BT_HD static void bt_scalar_bucket_register(BT_DEVICE BtState* s, int row, int value, int modifier);
BT_HD static int bt_scalar_gold_spawn_attract(BT_DEVICE BtState* s);
/* brotato_scalar_events.h is included AFTER brotato_pet_motion.h, which reads
 * the player's pickup_range scaled attract radius for its pet-owned gold. */
BT_HD static float bt_scalar_attract_radius(BT_DEVICE BtState* s);
BT_HD static void bt_scalar_on_dodge(BT_DEVICE BtState* s, int enemy_slot);
BT_HD static int bt_apply_atk_spd_mod_cd(int base_cd, float mod);
BT_HD static int bt_apply_structure_attack_speed(BT_DEVICE BtState* s, int base_cooldown);
BT_HD static int bt_scaling_damage(BT_DEVICE BtState* s, int damage, unsigned short first, unsigned short count, float mod);
BT_HD static void bt_row_variant_add(BT_DEVICE BtState* s, int row, int value, int value2);
BT_HD static float bt_row_sum_v(BT_DEVICE BtState* s, int row, BT_CONST_PTR(BtItemEff) E);
BT_HD static float bt_row_max_v2(BT_DEVICE BtState* s, int row, BT_CONST_PTR(BtItemEff) E);
BT_HD static int bt_row_dominant(BT_DEVICE BtState* s, int row, BT_CONST_PTR(BtItemEff) E, BT_THREAD int* v2_out);
BT_HD static int bt_row_owned(BT_DEVICE BtState* s, int row);
BT_HD static int bt_row_get(BT_DEVICE BtState* s, int row, BT_CONST_PTR(BtItemEff) E, int i,
                            BT_THREAD int* value, BT_THREAD int* value2);

BT_HD static float bt_loot_item_chance(BT_DEVICE BtState* s, BT_CONST_PTR(BtUnitKind) K,
                                      int items_spawned_this_wave, int wave);
BT_HD static int bt_loot_roll_consumable(BT_DEVICE BtState* s, BT_CONST_PTR(BtUnitKind) K,
                                        float item_chance, int is_boss, int wave);
BT_HD static int bt_loot_counts_toward_wave(int kind);
BT_HD static int bt_loot_gold_drop(BT_DEVICE BtState* s, BT_CONST_PTR(BtUnitKind) K,
                                  int neutral, int wave, int horde);
BT_HD static float bt_loot_gold_value(BT_DEVICE BtState* s, int neutral, float base_value);
BT_HD static int bt_loot_gold_spread(BT_CONST_PTR(BtUnitKind) K, float value);
BT_HD static void bt_loot_consumable_arc(BT_DEVICE BtState* s, float x, float y,
                                        int gold_spread, BT_DEVICE float* arc);
BT_HD static int bt_loot_item_step(BT_DEVICE float* x, BT_DEVICE float* y, BT_DEVICE float* arc,
                                   BT_DEVICE unsigned char* attracted, int attractor_dead,
                                   float tx, float ty, float delta);
BT_HD static void bt_loot_spawn_gold(BT_DEVICE BtState* s, float value, float x, float y, int spread);
/* brotato_enemy_modifiers.h (EnemyModifiers slice, G1 + the enemy final-stat
 * factors). Forward-declared here so brotato_pet_motion.h -- included BEFORE
 * that header -- can apply the G1 multiplier in its own melee/hurt handlers. */
BT_HD static float bt_enemy_incoming_damage(BT_DEVICE BtState* s, int slot, float value);
/* brotato_enemy_hit_effects.h (EnemyHitEffects): the enemy_percent_damage_taken
 * lifecycle helpers bt_spawn_kind_at / bt_core_reset call before that header is
 * included (brotato_core.h pulls it in beside brotato_enemy_modifiers.h). */
BT_HD static void bt_enemy_hit_reset(BT_DEVICE BtState* s);
BT_HD static void bt_enemy_hit_respawn(BT_DEVICE BtState* s, int j);
BT_HD static float bt_enemy_final_damage(BT_DEVICE BtState* s, float from_value, float percent_modifier);
BT_HD static float bt_enemy_final_health(BT_DEVICE BtState* s, float from_value, float percent_modifier);
BT_HD static float bt_enemy_final_speed(BT_DEVICE BtState* s, float from_value, float percent_modifier);
/* entity_spawner.gd:150-165 per-group-event perf cull (defined with the enemy
 * death machinery far below). */
BT_HD static void bt_mark_enemy_dead(BT_DEVICE BtState* s, int j, int allow_loot);
/* brotato_enemy_curse.h (DLC cursed enemies); included after that header, but
 * bt_spawn_kind_at / bt_mark_enemy_dead / bt_enter_wave are defined above it.
 * bt_enemy_cursed is the published accessor the weapon kill hooks use. */
BT_HD static int bt_enemy_cursed(BT_DEVICE BtState* s, int slot);
BT_HD static void bt_enemy_curse_roll(BT_DEVICE BtState* s, int slot, int kind);
BT_HD static float bt_enemy_curse_gold_value_mult(BT_DEVICE BtState* s, int slot);
/* brotato_item_explosions.h (included after the other slices): the shared
 * enemy-hit primitive and the position-based triggers. Forward-declared here
 * because bt_explode / bt_mark_enemy_dead / bt_enter_wave / bt_core_reset are
 * defined above the include. */
BT_HD static float bt_item_expl_hit_enemy(BT_DEVICE BtState* s, int j, float base, int is_crit,
                                          float crit_dmg, float burn_chance, float burn_dmg,
                                          float burn_dur, float proc_chance, float proc_dmg,
                                          int from_player_index, int has_from,
                                          BT_THREAD int* killed, BT_THREAD BtEnemyHitContext* ctx);
/* The CHARACTER's own explode_on_hit row, spawned into that same deferred pool
 * (items/characters/bull/bull_effect_4.tres).  Forward-declared because
 * bt_explode is defined above the include. */
BT_HD static int bt_item_expl_char_explode(BT_DEVICE BtState* s);
BT_HD static float bt_wfx_hit_bonus_pct(BT_DEVICE BtState* s, int slot,
        BT_CONST_PTR(unsigned short) rows, int count, int elemental, int burning);
/* brotato_weapon_effects.h: the burning-enemy COUNT source for
 * GainStatForEveryStat rows whose aux_key is BT_ISTR_BURNING_ENEMY
 * (fried_rice), consumed by bt_recompute_links below the include. */
BT_HD static int bt_wfx_burning_enemies(BT_DEVICE BtState* s);
BT_HD static float bt_wfx_burning_enemy_link_bonus(BT_DEVICE BtState* s, unsigned short row);
BT_HD static int bt_wfx_force_crit_when_burning(
        BT_CONST_PTR(unsigned short) rows, int count, int burning);
/* entity.gd:68-100: a killed node plays its 1.0 s death animation and only then
 * free_entity -> add_node_to_pool, so a neutral's node identity returns to its
 * kind's LIFO BT_POOL_DEATH_FRAMES frames after death -- the SAME ring cursor and
 * delay the enemy pool uses (bt_neutral_uid_step runs beside bt_enemy_pool_step).
 * At most BT_MAX live nodes can die in one frame, so the ring slot cannot
 * overflow; if it somehow did, object_uid_overflow is raised (never a silent
 * aliasing). */
BT_HD static void bt_neutral_uid_retire(BT_DEVICE BtState* s, int slot) {
    int r = s->pool_pos;
    int nk = (int)s->ekind[slot];
    if (nk < 0 || nk >= BT_NNEUTRALS || s->ndly_n[r] >= (unsigned char)BT_MAX) {
        if (nk >= 0 && nk < BT_NNEUTRALS) s->object_uid_overflow = 1;
        s->e_object_uid[slot] = 0u;
        return;
    }
    s->ndly_uid[r][s->ndly_n[r]] = s->e_object_uid[slot];
    s->ndly_kind[r][s->ndly_n[r]] = (unsigned char)nk;
    s->ndly_n[r]++;
    s->e_object_uid[slot] = 0u;
}
BT_HD static void bt_neutral_uid_step(BT_DEVICE BtState* s) {
    int r = s->pool_pos;
    for (int q = 0; q < (int)s->ndly_n[r]; q++) {
        int nk = (int)s->ndly_kind[r][q];
        if (s->neutral_uid_n[nk] < BT_MAX) {
            s->neutral_uid_pool[nk][s->neutral_uid_n[nk]++] = s->ndly_uid[r][q];
        } else {
            s->object_uid_overflow = 1;
        }
    }
    s->ndly_n[r] = 0;
}
BT_HD static int bt_enemy_charmed(BT_DEVICE BtState* s, int slot);
/* The per-wave source-object UID of the unit in `slot`, 1..BT_OBJECT_UID_MAX
 * (0 = no identity, object_uid_overflow set).  Use THIS as the
 * Hitbox.ignored_objects / hit-once-per-target key instead of e_instance: a fresh
 * boss/elite descriptor recycles its pool id but is a NEW node, a pooled enemy or
 * neutral respawn keeps its uid.  Key a per-projectile bitset of
 * ((BT_OBJECT_UID_MAX + 32) / 32) words by it. */
BT_HD static unsigned int bt_object_uid(BT_DEVICE BtState* s, int slot) {
    return s->e_object_uid[slot];
}
/* Mint one new uid, or 0 with object_uid_overflow when the per-wave space is
 * exhausted (never an aliased id and never a slot fallback). */
BT_HD static unsigned int bt_object_uid_mint(BT_DEVICE BtState* s) {
    if (s->object_uid_next >= (unsigned int)BT_OBJECT_UID_MAX) {
        s->object_uid_overflow = 1;
        return 0u;
    }
    return ++s->object_uid_next;
}
BT_HD static int bt_enemy_charm_absorb(BT_DEVICE BtState* s, int slot);
BT_HD static int bt_enemy_can_boost(BT_DEVICE BtState* s, int slot);
BT_HD static int bt_enemy_hits_enemies(BT_DEVICE BtState* s, int slot);
BT_HD static int bt_enemy_hurtbox_enabled(BT_DEVICE BtState* s, int slot);
BT_HD static int bt_enemy_boost_retargets(BT_DEVICE BtState* s, int slot);
BT_HD static unsigned int bt_enemy_boost_zone_mask(BT_DEVICE BtState* s, int slot);
BT_HD static int bt_enemy_lifecycle_charmed_by(BT_DEVICE BtState* s, int slot);
BT_HD static void bt_enemy_lifecycle_charm_spawn(BT_DEVICE BtState* s, int slot, int charmed_by);
BT_HD static void bt_enemy_lifecycle_charmed_boost(BT_DEVICE BtState* s, int buffer_slot);
BT_HD static void bt_enemy_lifecycle_heal_step(BT_DEVICE BtState* s);
BT_HD static float bt_enemy_lifecycle_charmed_bonus(BT_DEVICE BtState* s, int player_index);
BT_HD static void bt_item_expl_reset(BT_DEVICE BtState* s);
BT_HD static int bt_item_expl_on_key(BT_DEVICE BtState* s, unsigned short key, float x, float y);
BT_HD static int bt_item_expl_player_nb(BT_DEVICE BtState* s);
BT_HD static int bt_item_expl_player_hit(BT_DEVICE BtState* s, int nb_explosions);
BT_HD static void bt_temporal_bucket_register(BT_DEVICE BtState* s, int row, int nv, int mod_pct);
BT_HD static float bt_temporal_on_hit(BT_DEVICE BtState* s);
BT_HD static float bt_temporal_on_dodge(BT_DEVICE BtState* s);
BT_HD static void bt_temporal_apply_stats_next_wave(BT_DEVICE BtState* s);
BT_HD static float bt_temporal_scalar_effective(BT_DEVICE BtState* s, unsigned short key);
BT_HD static void bt_temporal_reset(BT_DEVICE BtState* s);

/* The run profile this simulator models: Danger 5 single-player campaign
 * (items/difficulties/5/difficulty_5.tres: danger_enemy_health +40,
 * danger_enemy_damage +40, double_boss = 1 via difficulty_5_effect_4).  It is
 * the default of BtState.difficulty (the RunData.current_difficulty parameter a
 * harness may override) and the switch the boss.gd:38-49 hp-factor chain reads:
 * the double_boss clause is live exactly while difficulty >= this value, so the
 * third (`difficulty <= 0`, DLC zone) clause is unreachable in a real D5 run
 * and reachable in a lower-difficulty DLC run. */
#define BT_RUN_DIFFICULTY 5

/* wave duration, seconds (BT_ZONE_WAVE_DUR[zone][wave-1]).
 * zone_service.gd:41-49: every wave past the zone's authored 20 is generated
 * with `wave.wave_duration = 60` (BT_ENDLESS_DURATION). */
BT_HD static float bt_wave_duration(int zone, int wave) {
    int i = wave - 1;
    if (i >= BT_WAVES_MAX) return (float)BT_ENDLESS_DURATION;
    if (i < 0) i = 0;
    if (zone < 0) zone = 0;
    if (zone >= BT_ZONES) zone = BT_ZONES - 1;
    return (float)BT_ZONE_WAVE_DUR[zone][i];
}

/* run_data.gd:775-783 get_endless_factor is defined in brotato_loot.h, which
 * this TU includes later; declared here so the early price helpers below share
 * the one run-level factor the source's get_value/get_reroll_price use (the
 * weapon path, bt_weapon_value_from in brotato_weapon_inventory.h, already
 * folds the same term). */
BT_HD static float bt_loot_endless_factor(int wave);

/* item_service.gd:701 get_value -- wave-scaled list price. */
BT_HD static float bt_item_price(int wave, int base) {
    float v = (float)base;
    float p = v + (float)wave + v * (float)wave * 0.1f;
    p = (float)(int)p;   /* item_service.gd:719 truncates, rather than rounds */
    return p < 1.0f ? 1.0f : p;
}

/* item_service.gd:701-719 get_value with is_weapon = false and
 * affected_by_items_price_stat = true, at an EXPLICIT wave: (base + wave +
 * base*wave*0.1) * (1 + items_price/100) * (1 + endless_factor/5), truncated
 * with a floor of 1.  get_value's own `wave` argument drives BOTH the wave term
 * and get_endless_factor(wave), so a shop offer priced at its creation wave is
 * reproduced exactly by passing that wave (shop_item.gd:129-140). */
BT_HD static float bt_shop_price_at(BT_DEVICE BtState* s, int wave, unsigned short item) {
    float base = (float)BT_ITEMS[item].price;
    float price = (base + (float)wave + base * (float)wave * 0.1f) *
        (1.0f + s->scalar_effect[BT_ISTR_ITEMS_PRICE] * 0.01f) *
        (1.0f + bt_loot_endless_factor(wave) / 5.0f);
    return bt_maxf(1.0f, (float)(int)price);
}
/* The run's-current-wave form (recycle values, item_service callers that pass
 * RunData.current_wave). */
BT_HD static float bt_shop_price(BT_DEVICE BtState* s, unsigned short item) {
    return bt_shop_price_at(s, (int)s->wave, item);
}

/* The observation's shop-slot PRICE (rl_obs.gd:1096-1123 + 580-594
 * shop_slot_features).  The bridge stores, per slot, the live ShopItem's
 * `value` -- the purchase price in the run's shop currency (shop_item.gd:132-140
 * divides the gold list price by 20 and ceils it under hp_shop) -- and falls
 * back to the plain BT_ITEMS[0].price (the table's BASE value, never
 * wave-scaled or currency-converted) for a slot with no live offer (an
 * un-offered/bought slot projects to item index 0 with live_price == -1).  An
 * offer that exists in this run's slot is priced by bt_wshop_price_units, the
 * SAME function the buy/recycle gate uses, so the observed flag can never
 * disagree with the action layer.  bt_wshop_price_units is defined in
 * brotato_weapon_shop.h (included further down this TU); declared here so the
 * observation block shares the one currency rule. */
BT_HD static float bt_wshop_price_units_at(BT_DEVICE BtState* s, int wave, int kind,
                                           unsigned short id);

/* One live slot's price: shop_item.gd:129-140 stores it when the offer's node
 * is created, at the wave `shop_price_wave[slot]` was stamped with (a carried
 * lock re-prices from its own entry's wave, shop_item.gd:244 +
 * base_shop.gd:439).  The wave-advancing GO frame still shows the open shop,
 * and that shop's prices are the STORED ones -- the reason the game keeps
 * `value`/`wave_value` on the node instead of recomputing them from
 * RunData.current_wave.  An unstamped slot (only reachable from a hand-built
 * state) falls back to the current wave. */
BT_HD static float bt_shop_slot_price(BT_DEVICE BtState* s, int slot) {
    if (slot < 0 || slot >= 4) return 0.0f;
    if (s->shop_kind[slot] == BT_SHOP_NONE) return 0.0f;
    int wave = (int)s->shop_price_wave[slot];
    if (wave <= 0) wave = (int)s->wave;
    return bt_wshop_price_units_at(s, wave, (int)s->shop_kind[slot], s->shop_id[slot]);
}

BT_HD static float bt_observe_shop_price(BT_DEVICE BtState* s, int slot) {
    int live = s->phase == 1 && (s->shop_available & (1u << slot)) != 0 &&
               s->shop_kind[slot] != BT_SHOP_NONE;
    if (live) return bt_shop_slot_price(s, slot);
    return (float)BT_ITEMS[0].price;
}

/* item_service.gd:726-730 get_reroll_price -- normal_price with factor 1, so
 * discounted == normal:
 *   delta = int(max(1.0, 0.4 * wave * sqrt(1 + endless_factor)))
 *   normal = int(wave * 0.75) + delta + delta * paid_reroll_count
 * The sqrt term is 1 for waves <= 20. */
BT_HD static int bt_reroll_cost(int wave, int reroll_count) {
    float d = 0.4f * (float)wave * bt_sqrtf(1.0f + bt_loot_endless_factor(wave));
    int delta = (int)(d < 1.0f ? 1.0f : d);
    return (int)((float)wave * 0.75f) + delta + delta * reroll_count;
}

/* run_data.gd:767-772: need for the NEXT level = get_xp_needed(level+1) =
 * (3 + level + 1)^2 = (level + 4)^2. */
BT_HD static int bt_xp_needed(int level) {
    int n = 4 + level;
    return n * n;
}

/* run_data.gd:767-768 get_next_level_xp_needed:
 *   get_xp_needed(level+1) * (1.0 + next_level_xp_needed/100.0)
 * evaluated in binary64 (the source's `/100.0` and `1.0 + ...` are doubles).
 * `next_level_xp_needed` is a character effect only (Baby +130, Mutant -66,
 * Technomage +75; bt_character_next_level_xp_pct), so the threshold is a
 * FRACTIONAL bound for those profiles; a 0-pct profile keeps the integer curve
 * exactly. */
BT_HD static float bt_xp_needed_pct(unsigned short character, int level) {
    float pct = (float)bt_character_next_level_xp_pct(character);
    if (pct == 0.0f) return (float)bt_xp_needed(level);
    BtRngF64 base = bt_rf64_from_float((float)bt_xp_needed(level));
    return bt_rf64_to_float(bt_rf64_mul(base, bt_rf64_one_plus_pct(pct)));
}

/* --- kind classification (BT_KIND_CLASS: 0 enemy, 1 elite, 2 boss) --- */
BT_HD static int bt_kind_class(int kind) { return (int)BT_KIND_CLASS[kind]; }
BT_HD static int bt_behavior(int kind) { return (int)BT_ATTACKS[kind].beh; }

/* A non-stat effect row identifies its mechanic by either its key or its
 * custom_key -- extract_items.py stores the source hash in whichever field the
 * resource used (e.g. remove_speed / giant_crit_damage / bonus_damage_against_
 * targets_above_hp / hp_regen_bonus / gain_random_primary_stats_on_go_to_next_
 * wave land in `key`, while consumable_stats_while_max / decaying_stats_* /
 * gain_stats_on_reroll land in `custom_key`). Matching both keeps every
 * mechanic reachable (several were dead when only custom_key was checked). */
BT_HD static int bt_eff_is(BT_CONST_PTR(BtItemEff) E, unsigned short k) {
    return E->key == k || E->custom_key == k;
}

/* StatCapEffect.key is the *_cap stat name (utils.gd:469-484 get_capped_stat
 * maps stat_max_hp->hp_cap, stat_speed->speed_cap, stat_dodge->dodge_cap), not a
 * core stat id: map it. crit_chance_cap has no interned key (no item uses it). */
BT_HD static int bt_cap_key_to_stat(unsigned short k) {
    switch (k) {
    case BT_ISTR_HP_CAP: return 0;
    case BT_ISTR_SPEED_CAP: return 3;
    case BT_ISTR_DODGE_CAP: return 4;
    default: return 255;
    }
}

/* --- physics world setup --- */
BT_HD static void bt_phys_walls(BT_DEVICE BtState* s) {
    float W = BT_ARENA_W, H = BT_ARENA_H, D = 4.0f * BT_TILE;
    s->phys.bodies[BT_MAX + 0] = bt_phys_body_rect(bt_v2(-D * 0.5f, H * 0.5f), D * 0.5f, H * 0.5f + D, BT_LAYER_WALL, BT_MASK_WALL);
    s->phys.bodies[BT_MAX + 1] = bt_phys_body_rect(bt_v2(W + D * 0.5f, H * 0.5f), D * 0.5f, H * 0.5f + D, BT_LAYER_WALL, BT_MASK_WALL);
    s->phys.bodies[BT_MAX + 2] = bt_phys_body_rect(bt_v2(W * 0.5f, -D * 0.5f), W * 0.5f + D, D * 0.5f, BT_LAYER_WALL, BT_MASK_WALL);
    s->phys.bodies[BT_MAX + 3] = bt_phys_body_rect(bt_v2(W * 0.5f, H + D * 0.5f), W * 0.5f + D, D * 0.5f, BT_LAYER_WALL, BT_MASK_WALL);
    s->phys.body_count = BT_MAX + 4;
}

/* --- spawn one unit into a slot --- */
BT_HD static void bt_spawn_kind_at(BT_DEVICE BtState* s, int slot, int kind, float x, float y, int neutral) {
    BT_CONST_PTR(BtUnitKind) K = neutral ? &BT_NEUTRALS[kind] : &BT_KINDS[kind];
    float w1 = (float)(s->wave - 1);
    /* entity_service.gd:11-53 get_final_enemy_health / _damage
     * (brotato_enemy_modifiers.h): danger_factor * effects_factor (the
     * enemy_health / enemy_damage keys). stats.get_base_health is typed `-> int`
     * and truncates (health + hp_grow*(wave-1)) before the factor; get_base_damage
     * is a float and does not. */
    float hp = bt_enemy_final_health(s, bt_floorf(K->hp + K->hp_grow * w1), 0.0f);
    float dmg = bt_enemy_final_damage(s, K->dmg + K->dmg_grow * w1, 0.0f);
    int cls = neutral ? 0 : bt_kind_class(kind);
    if (cls != 0) {
        /* boss.gd:38-49 hp factors, in source order:
         *   if current_wave <= 8:                                       *0.5
         *   elif (sum_all_player_effects(double_boss_hash) > 0 and
         *         current_wave == nb_of_waves) or current_wave <= 12:   *0.75
         *   elif current_wave == nb_of_waves and current_difficulty <= 0
         *        and current_zone != 0:                               *0.9
         * double_boss is Items/difficulties/5/difficulty_5_effect_4, i.e. part
         * of the difficulty-5 effect set, so it is on exactly while
         * current_difficulty >= 5 (bt_core_init's default, the D5 profile this
         * core models).  With it off, a final wave past 12 takes no *0.75 and
         * the third clause applies in a DLC zone. */
        if (s->wave <= 8) hp = bt_roundf(hp * 0.5f);
        else if ((s->difficulty >= BT_RUN_DIFFICULTY && s->wave == BT_WAVES) ||
                 s->wave <= 12)
            hp = bt_roundf(hp * 0.75f);
        else if (s->wave == BT_WAVES && s->difficulty <= 0 && s->zone != 0)
            hp = bt_roundf(hp * 0.9f);
    }
    if (hp < 1.0f) hp = 1.0f;
    if (dmg < 1.0f) dmg = 1.0f;
    s->emax[slot] = hp; s->ehp[slot] = hp;
    s->edmg[slot] = dmg;
    s->ex[slot] = x; s->ey[slot] = y;
    int fresh = 0;
    unsigned short instance = neutral ? (unsigned short)BT_POOL_NO_INST :
        bt_enemy_pool_acquire(s, kind, &fresh);
    if (!neutral && instance == BT_POOL_NO_INST) return;
    s->e_instance[slot] = instance;
    /* Stable source-node identity (weapon_projectiles.h bt_wproj_eid): mint ONLY
     * when the scene instantiates a NEW node.  An ordinary pooled respawn keeps
     * the descriptor's uid, a fresh boss/elite descriptor mints a new one even
     * though the descriptor id is recycled, and the core's neutral domain is not
     * pooled (per-wave fresh nodes), so a neutral mints per spawn. */
    if (neutral) {
        /* the source's neutral pool: reuse the most recently retired node of this
         * neutral kind (entity_spawner.gd:346 LIFO), else instantiate a new node. */
        int nk = (kind >= 0 && kind < BT_NNEUTRALS) ? kind : -1;
        if (nk >= 0 && s->neutral_uid_n[nk] > 0)
            s->e_object_uid[slot] = s->neutral_uid_pool[nk][--s->neutral_uid_n[nk]];
        else
            s->e_object_uid[slot] = bt_object_uid_mint(s);
    } else {
        if (fresh) s->pool_object_uid[instance] = bt_object_uid_mint(s);
        s->e_object_uid[slot] = s->pool_object_uid[instance];
    }
    /* unit.gd:81: _speed_rand_value = rand_range(-r, +r) as int, then
     * get_final_enemy_speed rounds (speed + rand) * factor (unit.gd:113-126).
     * The jitter is retained (espeed_rand) because the source's buffer boost
     * recomputes the speed from stats.speed + that same per-unit value. */
    s->ekbx[slot] = 0.0f; s->ekby[slot] = 0.0f;
    s->ecd[slot] = 0.0f;
    /* the slot's previous occupant cannot be dying here (bt_reserve_slot skips a
     * dying slot), but a reset-path spawn may land on one: a node arriving in
     * the slot has no death animation of its own. */
    bt_enemy_set_dying(s, slot, 0);
    s->edpos[slot] = 0.0f; s->edspd[slot] = 0.0f;
    /* a spawn with no death animation yet: if it dies without a resolvable pool
     * ledger row (neutral trees), the unlock-Timer writer is a no-op anyway. */
    s->edidl[slot] = 1.0f;
    s->edvx[slot] = 0.0f; s->edvy[slot] = 0.0f;
    s->edkx[slot] = 0.0f; s->edky[slot] = 0.0f;
    s->edkbx[slot] = 0.0f; s->edkby[slot] = 0.0f;
    s->epend_die[slot] = 0;
    s->eelite[slot] = (unsigned char)(cls == 0 ? 0 : (cls == 1 ? 1 : 2));
    s->ekind[slot] = (unsigned char)kind;
    s->eneutral[slot] = (unsigned char)(neutral ? 1 : 0);
    /* enemy.gd:24/227: source_spawner starts null (bt_spawn_kind_src writes the
     * real source + runs the link hook after this returns). */
    s->e_source[slot] = -1;
    s->ebeh[slot] = neutral ? 0 : (unsigned char)bt_behavior(kind);
    s->eatk[slot] = neutral ? 0.0f : (float)BT_ATTACKS[kind].initial_cd_f;   /* frames */
    s->echarge[slot] = 0.0f;
    s->echarge_dx[slot] = 0.0f; s->echarge_dy[slot] = 0.0f;
    s->echarge_spd[slot] = 0.0f;
    s->estate[slot] = !neutral && BT_NKIND_STATES[kind] > 0 ? BT_INIT_ROOT_STATE : 0;
    s->eelapsed[slot] = 0.0f;
    s->estate_timer[slot] = 0.0f;
    s->edecay[slot] = 0.0f;
    s->ebtimer[slot] = 0.0f;
    {
        /* buffer.gd's BoostTimer (initial_cd) and pursuer.gd's BoostTimer
         * (wait_time at autostart = the scene's wait_time on a FRESH node;
         * Pursuer.respawn:47 restarts it, i.e. time_left = boost_cooldown, which
         * is the same 1.0 s the campaign pursuer declares). */
        BT_CONST_PTR(BtSelfBoost) SB = &BT_KIND_SELFBOOST[kind];
        if (!neutral && SB->max_boosts > 0)
            s->ebtimer[slot] = bt_timer_frames(fresh ? SB->initial_cd : SB->cooldown);
        else if (!neutral && BT_KIND_BOOST[kind].radius > 0.0f)
            s->ebtimer[slot] = bt_timer_frames((float)BT_KIND_BOOST[kind].initial_cd);
    }
    s->enboost[slot] = 0;
    s->eflypend[slot] = 0;
    s->zone_n[slot] = 0;
    s->espawn_n[slot] = 0;
    s->eboosted[slot] = 0;
    s->elastspread[slot] = 0.0f;
    s->eshots[slot] = 0;
    s->ewind[slot] = 0;
    /* a unit spawns with a fresh `_burning` (unit.gd:56,618-646): a recycled
     * slot must not inherit the previous occupant's burning */
    s->eburn[slot] = 0.0f;
    s->eburndmg[slot] = 0.0f;
    /* the curse is per-life too (enemy_respawned re-rolls it): a recycled slot
     * must not inherit the previous occupant's cursed flag. */
    s->ecurse[slot] = 0;
    s->ehit[slot] = 0;
    /* a unit spawns with a fresh effect-behavior node set (Enemy.respawn ->
     * init_effect_behaviors), so the previous occupant's percent-damage-taken
     * debuff records and proc counts never carry over. */
    bt_enemy_hit_respawn(s, slot);
    if (neutral) {
        s->espeed_rand[slot] = (float)bt_rng_global_range_int(&s->rng,
            -(float)K->speed_rand, (float)K->speed_rand);
    } else {
        bt_enemy_init_run(s, slot, kind, instance, fresh);
        /* MovementBehavior node state (bt_mov_node targets / timers / detected
         * flag) is per pool instance: a fresh scene.instance() starts at the
         * member defaults, a pooled respawn() keeps it.  Unit.bonus_speed is
         * reset on every spawn (unit.gd:85). */
        bt_enemy_motion_spawn(s, slot, fresh);
    }
    s->espd[slot] = bt_enemy_final_speed(s, (float)K->speed + s->espeed_rand[slot], 0.0f);
    bt_enemy_set_alive(s, slot, 1);
    s->e_act[slot] = s->e_act_next++;   /* entity_spawner.gd push_back order */
    s->e_hold_tick[slot] = (unsigned short)s->ticks;
    s->ecount++;

    s->eboost_n[slot] = 0;
    s->mv_boost[slot] = 0;      /* Pursuer.respawn:46 movement_behavior_changed = false */
    s->egeom_kind[slot] = 0xFFu; s->egeom_n[slot] = 0xFFu;   /* invalidate the old row */
    bt_egeom_refresh(s, slot);
    BT_CONST_PTR(BtBodyPhysics) G = bt_kgeom(s, slot);
    s->emass[slot] = G->mass > 0.0f ? G->mass : 1.0f;
    s->phys.bodies[slot] = bt_phys_body_circle(bt_v2(x, y), bt_v2(G->body_x, G->body_y),
                          G->body_radius, s->emass[slot], neutral ? 1u : BT_LAYER_ENEMY,
                          neutral ? 2177u : BT_MASK_ENEMY, /* neutral.tscn root filters */
                          BT_PHYS_MODE_CHARACTER);
    /* DLC cursed enemy (curse_scene_effect_behavior.gd:50-65): the source runs
     * this on the spawner's `enemy_respawned` signal -- emitted AFTER every
     * spawn draw of the node and after set_source/charm (neither draws), i.e.
     * exactly the end of this activation.  Neutrals never emit it.  A zero curse
     * stat makes bt_chance draw nothing, so the baseline RNG stream is intact. */
    if (!neutral) bt_enemy_curse_roll(s, slot, kind);
}

/* Find a free enemy slot, culling a random live enemy (no loot, no credit) to
 * make room exactly like entity_spawner.gd:151-165. */
BT_HD static int bt_reserve_slot(BT_DEVICE BtState* s) {
    /* A dying slot still owns a node in the scene (the death animation is
     * playing), so it is NOT free: the game's spawn builds a new node, it never
     * repurposes the dying one. */
    for (int i = 0; i < BT_MAX; i++) if (!s->ealive[i] && s->edying[i] == 0) return i;
    /* full: kill a random live enemy that is not a boss to make room */
    int pick = -1, n = 0;
    for (int i = bt_enemy_next(s, 0, 1); i < (int)s->e_used; i = bt_enemy_next(s, i + 1, 1)) {
        if (!s->ealive[i]) continue;
        if (s->eneutral[i] || bt_kind_class(s->ekind[i]) != 0) continue;
        n++;
        if (bt_rng_bounded(&s->rng, (unsigned int)n) == 0u) pick = i;
    }
    if (pick < 0) return -1;
    /* Safety net only: entity_spawner.gd:151-165 culls the PLAIN list down to
     * max_enemies at every group event (bt_group_perf_cull), and BT_MAX leaves
     * room for every non-plain unit, so a legal run never evicts here.  The
     * eviction itself keeps the source's cull shape (a plain enemy, no loot),
     * but it fires OUTSIDE the source's per-group condition, so it is loud:
     *   - a boss/elite asking for a slot is impossible in the source (it has no
     *     slot ceiling), so the inline arena is out of capacity -> its own flag;
     *   - a plain/neutral spawn evicting is the plain-list cull shape ->
     *     pool_cull_fallback (the game-defined class of the capacity contract). */
    s->pool_cull_fallback = 1;
    bt_mark_enemy_dead(s, pick, 0);
    return pick;
}

/* bt_reserve_slot for a spawn that KNOWS what it is asking for: the fallback
 * inside is loud and split by cause -- a plain/neutral spawn evicting keeps
 * BtState.pool_cull_fallback (the source's cull shape), while a BOSS/ELITE
 * spawn evicting is pure implementation-capacity loss and is re-attributed to
 * BtState.elite_slot_pressure (the source has no slot ceiling, so that case
 * means the inline arena is out of capacity). */
BT_HD static int bt_reserve_slot_kind(BT_DEVICE BtState* s, int kind, int neutral) {
    unsigned char before = s->pool_cull_fallback;
    int slot = bt_reserve_slot(s);
    if (slot >= 0 && s->pool_cull_fallback != before && !neutral &&
        bt_kind_class(kind) != 0) {
        s->elite_slot_pressure = 1;
        s->pool_cull_fallback = before;   /* a boss/elite is not the plain-list cull */
    }
    return slot;
}

/* Immediate activation primitives: place a unit of `kind` into a slot right now.
 * The queued lifecycle reaches these only through bt_birth_flush (spawn_entity),
 * but they stay public for tools/tests that need a live enemy without waiting
 * out the birth countdown. */
BT_HD static int bt_spawn_kind(BT_DEVICE BtState* s, int kind, float x, float y) {
    int slot = bt_reserve_slot_kind(s, kind, 0);
    if (slot < 0) return 0;
    bt_spawn_kind_at(s, slot, kind, x, y, 0);
    return s->ealive[slot] != 0;
}
/* entity_spawner.gd:344-437 spawn_entity(..., source): the ENEMY branch calls
 * set_source(source) (enemy.gd:351) and then emits enemy_respawned, which the
 * stargazer listens to (stargazer.gd:46-54).  Everything else matches
 * bt_spawn_kind. */
BT_HD static int bt_spawn_kind_src(BT_DEVICE BtState* s, int kind, float x, float y, int src,
                                   int charmed_by) {
    int slot = bt_reserve_slot_kind(s, kind, 0);
    if (slot < 0) return 0;
    bt_spawn_kind_at(s, slot, kind, x, y, 0);
    if (!s->ealive[slot]) return 0;
    s->e_source[slot] = (short)src;
    bt_enemy_motion_link_spawner(s, slot);
    /* entity_spawner.gd:411-412: a child of a charmed enemy is charmed too. */
    bt_enemy_lifecycle_charm_spawn(s, slot, charmed_by);
    return 1;
}
BT_HD static int bt_spawn_neutral(BT_DEVICE BtState* s, int kind, float x, float y) {
    int slot = bt_reserve_slot_kind(s, kind, 1);
    if (slot < 0) return 0;
    bt_spawn_kind_at(s, slot, kind, x, y, 1);
    return 1;
}

/* --- placement (entity_spawner.gd + zone_service.gd) --- */
BT_HD static float bt_limit_edge(float edge) {
    float smaller = BT_ARENA_W < BT_ARENA_H ? BT_ARENA_W : BT_ARENA_H;
    if (edge / smaller > BT_DIST_TO_EDGE_MAX) return smaller * BT_DIST_TO_EDGE_MAX;
    return edge;
}

BT_HD static void bt_rand_pos(BT_DEVICE BtState* s, float edge, BT_THREAD float* ox, BT_THREAD float* oy) {
    edge = bt_limit_edge(edge);
    float min_x = BT_ARENA_W * 0.5f - 1.0f;
    float max_x = BT_ARENA_W * 0.5f + 1.0f;
    float min_y = BT_ARENA_H * 0.5f - 1.0f;
    float max_y = BT_ARENA_H * 0.5f + 1.0f;
    float e_x = BT_ARENA_W - edge, e_y = BT_ARENA_H - edge;
    if (edge < min_x) min_x = edge;
    if (e_x > max_x) max_x = e_x;
    if (edge < min_y) min_y = edge;
    if (e_y > max_y) max_y = e_y;
    *ox = bt_rand_range(s, min_x, max_x);
    *oy = bt_rand_range(s, min_y, max_y);
}

BT_HD static void bt_pos_in_area(BT_DEVICE BtState* s, float bx, float by, float area,
                                 float dist_from_edges, int edge_of_map,
                                 BT_THREAD float* ox, BT_THREAD float* oy) {
    float d = dist_from_edges;
    /* entity_spawner.gd:470-477: the spawn_edge_of_map branch returns
     * Utils.get_rand_pos_from_direction_at_distance(dir, _zone_min_pos,
     * _zone_max_pos, Utils.EDGE_MAP_DIST) and NEVER reads `spawn_dist_from_edges`
     * (the local `d` is only used by the area == -1 branch below).  The varying
     * axis spans [0 + EDGE_MAP_DIST, zone_size - EDGE_MAP_DIST]; the fixed axis
     * sits exactly on EDGE_MAP_DIST.  _possible_edge_spawns =
     * [TOP, LEFT, BOTTOM, RIGHT] indexed by Utils.randi() % 4, and direction
     * TOP/BOTTOM draw x first while LEFT/RIGHT draw only y (utils.gd:349-366). */
    if (edge_of_map) {
        int dir = (int)bt_rng_bounded(&s->utils_rng, 4u);
        if (dir == 0) { *ox = bt_rand_range(s, BT_EDGE_MAP_DIST, BT_ARENA_W - BT_EDGE_MAP_DIST); *oy = BT_EDGE_MAP_DIST; }
        else if (dir == 1) { *ox = BT_EDGE_MAP_DIST; *oy = bt_rand_range(s, BT_EDGE_MAP_DIST, BT_ARENA_H - BT_EDGE_MAP_DIST); }
        else if (dir == 2) { *ox = bt_rand_range(s, BT_EDGE_MAP_DIST, BT_ARENA_W - BT_EDGE_MAP_DIST); *oy = BT_ARENA_H - BT_EDGE_MAP_DIST; }
        else { *ox = BT_ARENA_W - BT_EDGE_MAP_DIST; *oy = bt_rand_range(s, BT_EDGE_MAP_DIST, BT_ARENA_H - BT_EDGE_MAP_DIST); }
        return;
    }
    if (area < 0.0f) {
        *ox = bt_rand_range(s, d, BT_ARENA_W - d);
        *oy = bt_rand_range(s, d, BT_ARENA_H - d);
        return;
    }
    float edge = bt_limit_edge(BT_EDGE_MAP_DIST);
    float min_x = edge + area * 0.5f, max_x = BT_ARENA_W - edge - area * 0.5f;
    float min_y = edge + area * 0.5f, max_y = BT_ARENA_H - edge - area * 0.5f;
    float cx = bt_clampf(bx, min_x, max_x), cy = bt_clampf(by, min_y, max_y);
    *ox = bt_rand_range(s, cx - area * 0.5f, cx + area * 0.5f);
    *oy = bt_rand_range(s, cy - area * 0.5f, cy + area * 0.5f);
}

/* ---------------------------------------------------------------------------
 * entity_spawner.gd category queues + entity_birth.gd lifecycle
 * ------------------------------------------------------------------------- */
/* Push one spawn onto a queue. A full queue sets pend_overflow -- an explicit,
 * observable failure (never a silent drop) so a caller/test can detect it. */
BT_HD static int bt_pend_push(BT_DEVICE BtState* s, BT_DEVICE BtPending* q, BT_DEVICE int* n,
                              int cap, int kind, int neutral, int is_boss, float x, float y,
                              int src, int charmed_by) {
    if (*n >= cap) { s->pend_overflow = 1; return 0; }
    q[*n].kind = (unsigned short)kind;
    q[*n].row = 0;
    q[*n].neutral = (unsigned char)(neutral ? 1 : 0);
    q[*n].is_boss = (unsigned char)(is_boss ? 1 : 0);
    q[*n].src = (short)src;
    q[*n].charmed_by = (short)charmed_by;
    q[*n].x = x; q[*n].y = y;
    (*n)++;
    return 1;
}
/* on_group_spawn_timing_reached: enemy -> queue_to_spawn, neutral (tree) ->
 * queue_to_spawn_trees, boss/elite -> queue_to_spawn_bosses.  These queue
 * entries carry no source (entity_spawner.gd:433 passes `source` only for the
 * wanted_to_spawn_an_enemy path). */
BT_HD static void bt_queue_enemy(BT_DEVICE BtState* s, int kind, float x, float y) {
    if (!bt_pend_push(s, s->pend_enemy, &s->n_pend_enemy, BT_MAX_PEND_ENEMY, kind, 0, 0, x, y, -1, -1))
        s->pend_drop_enemy++;
}
BT_HD static void bt_queue_tree(BT_DEVICE BtState* s, int kind, float x, float y) {
    if (!bt_pend_push(s, s->pend_tree, &s->n_pend_tree, BT_MAX_PEND_SMALL, kind, 1, 0, x, y, -1, -1))
        s->pend_drop_tree++;
}
BT_HD static void bt_queue_boss(BT_DEVICE BtState* s, int kind, float x, float y) {
    if (!bt_pend_push(s, s->pend_boss, &s->n_pend_boss, BT_MAX_PEND_SMALL, kind, 0, 1, x, y, -1, -1))
        s->pend_drop_boss++;
}
/* on_enemy_wanted_to_spawn_an_enemy (spawner death children, attack behaviors).
 * `src` is the emitting enemy's live slot (enemy.gd:333-334 passes `self`); a
 * spawner's own death children emit with no source. */
BT_HD static void bt_queue_summon_src(BT_DEVICE BtState* s, int kind, float x, float y, int src,
                                      int charmed_by) {
    if (!bt_pend_push(s, s->pend_summons, &s->n_pend_summons, BT_MAX_PEND_SMALL, kind, 0, 0, x, y, src, charmed_by))
        s->pend_drop_summons++;
}
BT_HD static void bt_queue_summon(BT_DEVICE BtState* s, int kind, float x, float y) {
    bt_queue_summon_src(s, kind, x, y, -1, -1);
}

/* forward declarations: the birth pipeline needs the structure cap and the
 * summon activator, both defined with the item/summon code further down. */
BT_HD static void bt_enforce_structure_cap(BT_DEVICE BtState* s);
BT_HD static int bt_spawn_summon_row(BT_DEVICE BtState* s, unsigned short row, float x, float y,
                                     int is_struct, float mod, float double_chance, float lm_cd);
/* pet_motion.h init hooks used by bt_spawn_summon_row (defined after that
 * header): a pet's stats/patrol geometry exist from the moment it is created
 * (unit.gd:63-76 Unit.init -> MovementBehavior.init; pet.gd:32-40 Pet.init ->
 * init_current_stats/update_target), never lazily on its first _physics_process. */
BT_HD static int bt_pet_ai_index(int opcode);
BT_HD static unsigned short bt_pet_pick_fx(BT_DEVICE BtState* s, int p);
BT_HD static void bt_pet_slot_init(BT_DEVICE BtState* s, int p, BT_CONST_PTR(BtPetAI) A);

/* queues_to_spawn_structures[player_index] / queues_to_spawn_pets[player_index].
 * `kind` is the summon_fx[] instance (or 0xFFFF for a runtime spawn such as a
 * BotOMine landmine); `row` is the effect row to instantiate. */
BT_HD static int bt_queue_struct(BT_DEVICE BtState* s, int fx, unsigned short row, float x, float y) {
    if (s->n_pend_struct >= BT_MAX_PEND_SMALL) { s->pend_overflow = 1; s->pend_drop_struct++; return 0; }
    s->pend_struct[s->n_pend_struct].kind = (unsigned short)fx;
    s->pend_struct[s->n_pend_struct].row = row;
    s->pend_struct[s->n_pend_struct].neutral = 0;
    s->pend_struct[s->n_pend_struct].is_boss = 0;
    s->pend_struct[s->n_pend_struct].src = -1;
    s->pend_struct[s->n_pend_struct].charmed_by = -1;
    s->pend_struct[s->n_pend_struct].x = x;
    s->pend_struct[s->n_pend_struct].y = y;
    s->n_pend_struct++;
    return 1;
}
BT_HD static int bt_queue_pet(BT_DEVICE BtState* s, int fx, unsigned short row, float x, float y) {
    if (s->n_pend_pet >= BT_MAX_PEND_SMALL) { s->pend_overflow = 1; s->pend_drop_pet++; return 0; }
    s->pend_pet[s->n_pend_pet].kind = (unsigned short)fx;
    s->pend_pet[s->n_pend_pet].row = row;
    s->pend_pet[s->n_pend_pet].neutral = 0;
    s->pend_pet[s->n_pend_pet].is_boss = 0;
    s->pend_pet[s->n_pend_pet].src = -1;
    s->pend_pet[s->n_pend_pet].charmed_by = -1;
    s->pend_pet[s->n_pend_pet].x = x;
    s->pend_pet[s->n_pend_pet].y = y;
    s->n_pend_pet++;
    return 1;
}

#include "brotato_neutral_events.h"

/* Pop one entry (LIFO, entity_spawner.gd:236 spawn() pop_back) and hand it to a
 * fresh EntityBirth with a full countdown. `bclass` selects the EntityType
 * branch (0 ENEMY/BOSS/NEUTRAL, 1 STRUCTURE, 2 PET). A structure birth first
 * runs the MAX_STRUCTURES eviction, exactly where the source does it
 * (spawn_entity_birth, entity_spawner.gd:292-303). EntityBirth.start's visual
 * rotation uses the same global RNG as placement, so its draw must be retained
 * even though this numerical simulation does not render the birth sprite. */
BT_HD static int bt_pop_to_birth(BT_DEVICE BtState* s, BT_DEVICE BtPending* q, BT_DEVICE int* n,
                                 int bclass) {
    if (*n <= 0) return 0;
    if (s->n_births >= BT_MAX_BIRTHS) { s->pend_drop_birth++; return 0; }
    BtPending e = q[--(*n)];
    if (bclass == 1) bt_enforce_structure_cap(s);   /* entity_spawner.gd:292-303 */
    int bi = s->n_births++;
    s->births[bi].kind = e.kind; s->births[bi].row = e.row;
    s->births[bi].neutral = e.neutral; s->births[bi].is_boss = e.is_boss;
    s->births[bi].bclass = (unsigned char)bclass;
    s->births[bi].active = 1;
    s->births[bi].src = e.src;
    s->births[bi].charmed_by = e.charmed_by;
    s->births[bi].x = e.x; s->births[bi].y = e.y;
    s->births[bi].timer = (float)BT_BIRTH_FRAMES;
    (void)bt_rand_range(s, 0.0f, 360.0f); /* entity_birth.gd:47 */
    s->draw_site[1] += 3u;
    bt_birth_audio_enqueue(s, 0);
    return 1;
}

/* entity_spawner.gd:52-75 _physics_process drain pass, run once every
 * SPAWN_DELAY frames: the per-player structure and pet queues, then trees,
 * bosses, summons (one each), then the ordinary queue nb_to_spawn times where
 * nb_to_spawn = 1 or int(clamp((size - QUEUE_LIMIT)/10, 1, 2)). */
BT_HD static void bt_spawn_poll(BT_DEVICE BtState* s) {
    /* Four owner queues are shuffled even in a solo run and when empty
     * (entity_spawner.gd:124; Array::shuffle in pinned Godot core/array.cpp).
     * Only owner 0 can contain entities here, so the order is immaterial, but
     * its three global RNG draws affect every later placement. */
    for (int i = 3; i > 0; i--) (void)bt_rng_next(&s->rng);
    s->draw_site[0] += 3u;
    bt_pop_to_birth(s, s->pend_struct, &s->n_pend_struct, 1);
    bt_pop_to_birth(s, s->pend_pet, &s->n_pend_pet, 2);
    bt_pop_to_birth(s, s->pend_tree, &s->n_pend_tree, 0);
    bt_pop_to_birth(s, s->pend_boss, &s->n_pend_boss, 0);
    bt_pop_to_birth(s, s->pend_summons, &s->n_pend_summons, 0);
    int nb = 1;
    if (s->n_pend_enemy >= BT_QUEUE_LIMIT) {
        int v = (s->n_pend_enemy - BT_QUEUE_LIMIT) / 10;
        if (v < 1) v = 1;
        if (v > 2) v = 2;
        nb = v;
    }
    for (int i = 0; i < nb; i++) bt_pop_to_birth(s, s->pend_enemy, &s->n_pend_enemy, 0);
}

/* entity_birth.gd:83-106: decrement every active birth by one frame; a finished
 * birth that still overlaps the player's body is relocated and restarted when it
 * is an ENEMY (`_colliding_with_player and type == EntityType.ENEMY`); every
 * other finished birth is only MARKED ready (active == 2). The source
 * `call_deferred("birth")`s the activation, so the real spawn_entity happens at
 * the deferred boundary (bt_birth_flush), which Godot flushes BETWEEN two
 * physics steps: bt_birth_flush therefore runs at the TOP of the following
 * frame, before bt_phys_step, so the newborn is integrated in that frame. */
BT_HD static void bt_birth_advance(BT_DEVICE BtState* s) {
    for (int i = 0; i < s->n_births; i++) {
        if (s->births[i].active != 1) continue;
        s->births[i].timer -= 1.0f;      /* _current_time_before_spawn -= 60 * delta */
        if (s->births[i].timer > 0.0f) continue;
        /* birth Area2D circle (r 72, mask = player layer) vs player body (r 24 @ (0,-12)) */
        float dx = s->births[i].x - s->px;
        float dy = s->births[i].y - (s->py + BT_PLAYER_BODY_Y);
        float rr = BT_BIRTH_RADIUS + BT_PLAYER_BODY_R;
        if (s->births[i].bclass == 0 && !s->births[i].neutral && !s->births[i].is_boss &&
            dx * dx + dy * dy < rr * rr) {
            float nx, ny;
            bt_rand_pos(s, BT_EDGE_MAP_DIST, &nx, &ny);   /* ZoneService.get_rand_pos() */
            s->draw_site[2] += 6u;
            s->births[i].x = nx; s->births[i].y = ny;
            s->births[i].timer = (float)BT_BIRTH_FRAMES;  /* restart the countdown */
        } else {
            s->births[i].active = 2;   /* ready: activated at the next frame's boundary */
            bt_birth_audio_enqueue(s, 1);
        }
    }
}

/* Deferred boundary: entity_spawner.on_entity_birth_timeout -> spawn_entity for
 * every ready birth.  The newborn is placed exactly at the birth position and
 * keeps a zero commanded velocity (no Unit update runs for it this frame, see
 * e_hold_tick), but it IS integrated by this frame's bt_phys_step -- Godot runs
 * Physics2DServer::step() before the frame's Node callbacks, so a wall overlap
 * is already solved on the newborn's first live snapshot (the recorded Bull
 * trace's fourth wave-1 enemy is born inside the right arena wall and reports
 * the depenetrated position on the tick it appears). */
BT_HD static void bt_birth_flush(BT_DEVICE BtState* s) {
    int w = 0;
    for (int i = 0; i < s->n_births; i++) {
        BtBirth b = s->births[i];
        if (b.active == 2) {
            if (b.bclass == 1 || b.bclass == 2) {
                int is_struct = (b.bclass == 1);
                if (b.kind == 0xFFFFu) {
                    bt_spawn_summon_row(s, b.row, b.x, b.y, is_struct, 0.0f, 0.0f, 0.0f);
                } else {
                    BtSummonFx fx = s->summon_fx[b.kind];
                    float lm = (fx.opcode == BT_ITEMEFF_BOT_O_MINE && fx.spawn_cd > 0)
                               ? (float)fx.spawn_cd / 60.0f : 0.0f;
                    bt_spawn_summon_row(s, b.row, b.x, b.y, is_struct, fx.mod,
                                        fx.is_pet ? fx.double_chance : 0.0f, lm);
                }
            }
            else if (b.neutral) bt_spawn_neutral(s, (int)b.kind, b.x, b.y);
            else (void)bt_spawn_kind_src(s, (int)b.kind, b.x, b.y, (int)b.src,
                                        (int)b.charmed_by);
            continue;                       /* consumed: birth returned to the pool */
        }
        s->births[w++] = b;
    }
    s->n_births = w;
}

/* group base position, then pushed away from the player (entity_spawner.gd:223-249). */
BT_HD static void bt_group_pos(BT_DEVICE BtState* s, BT_CONST_PTR(BtSpawnGroup) grp,
                               BT_THREAD float* ox, BT_THREAD float* oy) {
    float d = (float)grp->spawn_dist_away_from_edges;
    if ((int)grp->area != -1) bt_rand_pos(s, (float)grp->area * 0.5f + d, ox, oy);
    else bt_rand_pos(s, d, ox, oy);

    int min_dist = (int)BT_SPAWN_MIN_DIST + (int)grp->area / 2;
    int half_zone = (int)((BT_ARENA_W > BT_ARENA_H ? BT_ARENA_W : BT_ARENA_H) * 0.5f);
    if (min_dist > half_zone) min_dist = half_zone;
    for (;;) {
        float dx = *ox - s->px, dy = *oy - s->py;
        if (dx * dx + dy * dy >= (float)min_dist * (float)min_dist) break;
        if ((int)grp->area != -1) bt_rand_pos(s, (float)grp->area * 0.5f + d, ox, oy);
        else bt_rand_pos(s, d, ox, oy);
        min_dist = min_dist > 30 ? min_dist - 5 : 25;
        if (min_dist == 25) break;
    }
}

/* entity_spawner.gd:200-209: reject each unit position independently of the
 * group centre. The integer radius shrinks to 25, which bounds the loop. */
BT_HD static void bt_spawn_pos_away_from_player(BT_DEVICE BtState* s,
        BT_CONST_PTR(BtSpawnGroup) grp, float additional_distance,
        float bx, float by, BT_THREAD float* ox, BT_THREAD float* oy) {
    int min_dist = (int)(BT_SPAWN_MIN_DIST + additional_distance);
    for (;;) {
        float dx = *ox - s->px, dy = *oy - s->py;
        if (dx * dx + dy * dy >= (float)min_dist * (float)min_dist) break;
        bt_pos_in_area(s, bx, by, (float)grp->area,
                       (float)grp->spawn_dist_away_from_edges,
                       (int)grp->spawn_edge_of_map, ox, oy);
        min_dist = min_dist > 30 ? min_dist - 5 : 25;
        if (min_dist == 25) break;
    }
}

/* entity_spawner.gd:150-165 perf cull, run at the START of every group event
 * (including repeat emissions, which also pass through
 * on_group_spawn_timing_reached):
 *   max_enemies = int(wave.max_enemies + (players-1)*(wave.max_enemies/8.0))
 *   if enemies.size() > max_enemies:
 *       for i in enemies.size() - max_enemies:
 *           array_from = enemies_to_remove_in_priority if not empty else enemies
 *           en = Utils.get_rand_element(array_from); en.can_drop_loot = false; en.die()
 * The count is computed once, `died` is emitted synchronously by Entity.die
 * (entities/entity.gd:76), and `_on_enemy_died` erases the node from `enemies`
 * (entity_spawner.gd:450), so each removal shrinks the list the next
 * Utils.get_rand_element indexes -- exactly one global draw per removal.  Only
 * PLAIN enemies are in that list (bosses/elites/neutrals are separate lists and
 * are never culled), and e_act reproduces its append order.
 * `Utils.get_rand_element` is Array.pick_random == `array[Math::rand() % size]`
 * (core/array.cpp:420) -- the GLOBAL stream, one draw even for a 1-element
 * array.  `enemies_to_remove_in_priority` holds the same nodes in the same
 * spawn order, restricted to the kinds whose scene sets
 * Enemy.to_be_removed_in_priority (BT_KIND_PRIORITY_REMOVE, entity_spawner.gd:
 * 405): while it is non-empty the draw indexes THAT list, and the node is
 * erased from both lists. */
BT_HD static void bt_group_perf_cull(BT_DEVICE BtState* s) {
    /* `enemies` in append order == the live PLAIN enemies ordered by the
     * existing activation counter e_act (state.h; entity_spawner.gd push_back
     * order); `ordp` is the entity_spawner.gd:406 priority subset of it, same
     * order.  Bosses/elites (eelite != 0) and neutrals live in their own lists
     * and are never culled.  Insertion sort keeps the source's list order, and
     * each removal compacts the local copy exactly like Array.erase. */
    unsigned short ord[BT_MAX];
    unsigned short ordp[BT_MAX];
    int n = 0, np = 0;
    for (int i = bt_enemy_next(s, 0, 1); i < (int)s->e_used; i = bt_enemy_next(s, i + 1, 1)) {
        if (!s->ealive[i] || s->eneutral[i] || s->eelite[i] != 0) continue;
        int q = n++;
        while (q > 0 && s->e_act[ord[q - 1]] > s->e_act[i]) { ord[q] = ord[q - 1]; q--; }
        ord[q] = (unsigned short)i;
        if (BT_KIND_PRIORITY_REMOVE[s->ekind[i]]) {
            int r = np++;
            while (r > 0 && s->e_act[ordp[r - 1]] > s->e_act[i]) { ordp[r] = ordp[r - 1]; r--; }
            ordp[r] = (unsigned short)i;
        }
    }
    int over = n - (int)s->wave_max_enemies;      /* count read ONCE, before the loop */
    for (int i = 0; i < over; i++) {
        int slot;
        if (np > 0) {
            int pick = (int)bt_rng_bounded(&s->rng, (unsigned int)np);
            slot = (int)ordp[pick];
            for (int q = pick + 1; q < np; q++) ordp[q - 1] = ordp[q];
            np--;
            /* _on_enemy_died erases from BOTH lists (entity_spawner.gd:450-452) */
            for (int q = 0; q < n; q++) {
                if ((int)ord[q] != slot) continue;
                for (int z = q + 1; z < n; z++) ord[z - 1] = ord[z];
                n--;
                break;
            }
        } else {
            if (n <= 0) break;
            int pick = (int)bt_rng_bounded(&s->rng, (unsigned int)n);
            slot = (int)ord[pick];
            for (int q = pick + 1; q < n; q++) ord[q - 1] = ord[q];
            n--;
        }
        if (!s->ealive[slot]) continue;   /* a death effect already removed it */
        /* can_drop_loot = false -> bt_mark_enemy_dead(..., allow_loot = 0) */
        bt_mark_enemy_dead(s, slot, 0);
    }
}

/* Roll one group's unit counts/positions and QUEUE them (on_group_spawn_timing_
 * reached). Returns 1 if anything was queued. Nothing is activated here: the
 * 3-frame drain pass turns queue entries into EntityBirths. */
BT_HD static int bt_spawn_group(BT_DEVICE BtState* s, unsigned short grp_idx) {
    BT_CONST_PTR(BtSpawnGroup) grp = &BT_GROUPS[grp_idx];
    if (grp->unit_count == 0) return 0;
    bt_group_perf_cull(s);
    float bx, by;
    bt_group_pos(s, grp, &bx, &by);
    for (int u = 0; u < grp->unit_count; u++) {
        BT_CONST_PTR(BtSpawnUnit) su = BT_ZONE_UNIT(grp->zone, grp->unit_first + u);
        int dom = (int)su->domain;
        float number = (float)bt_rng_randi_range(&s->utils_rng, (int)su->min_n, (int)su->max_n);
        number = bt_neutral_group_number(s, grp->is_boss ? 2 : dom, grp->is_loot, 0.0f, number);
        float total = number * su->chance;
        int n = (int)total;
        if (bt_chance(s, total - (float)n)) n++;
        for (int k = 0; k < n; k++) {
            float x, y;
            bt_pos_in_area(s, bx, by, (float)grp->area, (float)grp->spawn_dist_away_from_edges,
                           (int)grp->spawn_edge_of_map, &x, &y);
            bt_spawn_pos_away_from_player(s, grp,
                su->additional_min_distance_from_player, bx, by, &x, &y);
            if (grp->is_boss) bt_queue_boss(s, (int)su->index, x, y);
            else if (dom == 1) bt_queue_tree(s, (int)su->index, x, y);
            else bt_queue_enemy(s, (int)su->index, x, y);
        }
    }
    return 1;
}

/* One ENDLESS generated group event (zone_service.gd:70-92 template copy).
 * The unit template (unit_endless.tres) has its min/max numbers overwritten per
 * copy with spawn_chance 1.0 and additional_min_distance_from_player 0.0, and
 * the group template keeps chance 1.0, repeating 999 with the DRAWN interval,
 * reduce 0, min_repeating_interval 1 and spawn_dist_away_from_edges 0;
 * per-copy timing/edge/area/kind come from the lg_* fields.
 * Draw order is entity_spawner.gd:175-205: Utils.randi_range(min,max), the
 * residual chance roll, then per unit the area position and the
 * away-from-player loop. */
BT_HD static int bt_spawn_dyn_group(BT_DEVICE BtState* s, int g) {
    BtSpawnGroup grp;
    grp.zone = (unsigned char)s->zone;
    grp.timing = 0; grp.chance = 0.0f; grp.repeat = 0;
    grp.repeat_interval = 0.0f; grp.reduce_interval = 0.0f; grp.min_interval = 0.0f;
    grp.area = (short)(int)s->lg_area[g];
    grp.spawn_edge_of_map = s->lg_edge[g];
    grp.spawn_dist_away_from_edges = 0;
    grp.is_neutral = 0; grp.is_boss = 0; grp.is_horde = 0; grp.is_loot = 0;
    grp.min_difficulty = 0; grp.min_wave = 0; grp.max_wave = 0;
    grp.unit_first = 0; grp.unit_count = 0; grp.group_id = 0;
    bt_group_perf_cull(s);
    float bx, by;
    bt_group_pos(s, &grp, &bx, &by);
    float number = (float)bt_rng_randi_range(&s->utils_rng, (int)s->lg_minn[g],
                                            (int)s->lg_maxn[g]);
    number = bt_neutral_group_number(s, 0, 0, 0.0f, number);
    float total = number * BT_ENDLESS_UNIT_TMPL[0].chance;
    int n = (int)total;
    if (bt_chance(s, total - (float)n)) n++;
    int queued = 0;
    for (int k = 0; k < n; k++) {
        float x, y;
        bt_pos_in_area(s, bx, by, (float)grp.area,
                       (float)grp.spawn_dist_away_from_edges,
                       (int)grp.spawn_edge_of_map, &x, &y);
        bt_spawn_pos_away_from_player(s, &grp,
            BT_ENDLESS_UNIT_TMPL[0].additional_min_distance_from_player, bx, by, &x, &y);
        bt_queue_enemy(s, (int)s->lg_kind[g], x, y);
        queued++;
    }
    return queued;
}

/* True when this wave's elite event rolled a HORDE (run_data elite schedule).
 * wave_manager.gd:140-147 appends zone_data.horde_groups to the wave's group
 * list, so the horde fires on its own per-group spawn_timing. */
BT_HD static int bt_wave_is_horde(BT_DEVICE BtState* s, int w) {
    for (int i = 0; i < BT_ELITE_EVENTS; i++)
        if ((int)s->elite_wave[i] == w && s->elite_type[i] == 1) return 1;
    return 0;
}

/* ENDLESS generated groups: ZoneService.get_additional_groups
 * (zone_service.gd:70-92).  One copy of the group template per i, with
 *   spawn_timing = max(min_spawn_timing, i*((duration-10)/nb) + 1) as int
 *   spawn_edge_of_map = randf() > 0.5
 *   area             = -1 if randf() > 0.5 else int(rand_range(200, 300))
 *   repeating_interval = int(round(rand_range(6, 8)))
 * and one copy of the unit template with
 *   unit_scene  = Utils.get_rand_element(zone.endless_enemy_scenes)
 *   min_number  = int(1 + bonus_enemies_per_group)
 *   max_number  = int(round(rand_range(min_number, min_number+1)) + bonus)
 * The two `Utils.get_rand_element` calls on the single-element template arrays
 * still consume one draw each: Utils.get_rand_element is Array.pick_random ==
 * `array[Math::rand() % size]` (core/array.cpp:420), i.e. ONE GLOBAL-stream
 * draw, even for a 1-element array.
 * Draw order: G pick, G randf, G randf, [G rand_range(200,300)],
 * G rand_range(6,8), G pick, G pick(scene), G rand_range(min,min+1).
 * nb and bonus_enemies_per_group are parameters: ZoneService.get_wave_data
 * calls get_additional_groups(nb, 60, nb/5.0) while main.gd:484-491's last-wave
 * transition calls get_additional_groups(nb, 90) with the DEFAULT bonus 1 (and
 * then overwrites every spawn_timing). */
BT_HD static void bt_build_endless_groups(BT_DEVICE BtState* s, int nb, int duration,
                                          int min_timing, float bonus, int* n) {
    if (nb <= 0) return;
    float span = ((float)duration - 10.0f) / (float)nb;
    int nk = (int)BT_ZONE_NENDLESS_KINDS[s->zone];
    for (int i = 0; i < nb; i++) {
        (void)bt_rng_bounded(&s->rng, 1u);          /* get_rand_element(endless_groups) */
        int timing = (int)((float)i * span + 1.0f);
        if (timing < min_timing) timing = min_timing;   /* max(min_spawn_timing, ...) */
        float edge_rand = bt_math_randf(&s->rng);   /* randf() spawn_edge_of_map */
        float area_rand = bt_math_randf(&s->rng);   /* randf() area */
        float area = area_rand > 0.5f ? -1.0f
                                      : (float)(int)bt_rand_range(s, 200.0f, 300.0f);
        int interval = (int)bt_roundf(bt_rand_range(s, 6.0f, 8.0f));
        (void)bt_rng_bounded(&s->rng, 1u);          /* get_rand_element(endless_unit_groups) */
        int kind = (int)BT_ZONE_ENDLESS_KINDS[s->zone]
                      [(int)bt_rng_bounded(&s->rng, (unsigned int)nk)];
        int min_n = (int)(1.0f + bonus);
        int max_n = (int)(bt_roundf(bt_rand_range(s, (float)min_n, (float)min_n + 1.0f)) + bonus);
        if (min_n < 1) min_n = 1;
        if (max_n < min_n) max_n = min_n;
        if (*n >= BT_MAX_LIVE_GROUPS) { s->group_overflow = 1; continue; }
        int g = (*n)++;
        s->live_group[g] = 0;                       /* dynamic slot: no BT_GROUPS row */
        s->lg_dynamic[g] = 1;
        s->lg_timing[g] = (unsigned short)timing;
        s->lg_area[g] = area;
        s->lg_interval[g] = (float)interval;
        s->lg_edge[g] = (unsigned char)(edge_rand > 0.5f);
        s->lg_kind[g] = (unsigned short)kind;
        s->lg_minn[g] = (unsigned char)min_n;
        s->lg_maxn[g] = (unsigned char)max_n;
    }
}

/* --- per-wave live-group list, in the SOURCE's iteration order ---
 * wave_manager.gd:init builds current_wave_data.groups_data as:
 *   (1) the wave resource's own groups_data
 *   (2) zone_data.groups_data_in_all_waves
 *   (3) extra_loot_aliens item effects (not modelled here)
 *   (4) extra_enemies_at_wave / extra_enemies_next_wave item effects
 *       (ExtraEnemyEffect, i.e. the bait item -> groups_to_add)
 *   (5) wave_data.conditional_groups_data (BT_NCOND == 0 for every shipped wave)
 *   (6) the elite / horde block          -> groups_to_add
 *   (7) dlc_data.groups_in_all_zones     -> groups_to_add, appended LAST
 * and `_physics_process` visits that array in order, so the order decides which
 * group consumes its draws first when two groups fire on the same frame. */
BT_HD static void bt_build_live_groups(BT_DEVICE BtState* s) {
    int wi = s->wave - 1;
    int endless = 0;
    /* zone_service.gd:41-47: past the authored waves the wave is built from a
     * BASE wave (indices 10..19, i.e. the zone's 11th..20th) and the base's
     * max_enemies is scaled by zone_service.gd:48. */
    if (wi >= BT_WAVES_MAX) {
        wi = (s->wave - 1) % BT_WAVES_MAX;
        if (wi < 10) wi += 10;
        endless = 1;
    }
    if (wi < 0) wi = 0;
    int n = 0;
    BT_CONST_PTR(BtWave) w = &BT_ZONE_WAVES[s->zone][wi];
    /* entity_spawner.gd:152: max_enemies = int(wave.max_enemies + (players-1) *
     * (wave.max_enemies / 8.0)); solo that is the wave's own cap (100 for the
     * authored waves, 135..144 for the generated endless ones). */
    s->wave_max_enemies = (unsigned short)(endless
        ? (int)((float)w->max_enemies *
                (BT_ENDLESS_MAXENEMY_BASE + (float)wi * BT_ENDLESS_MAXENEMY_PER_INDEX))
        : (int)w->max_enemies);
    for (int g = 0; g < BT_MAX_LIVE_GROUPS; g++) s->lg_dynamic[g] = 0;
    s->group_overflow = 0;
    /* (1) the wave's own groups.  A final-wave boss group is skipped by
     * bt_fire_groups (is_boss) and spawned by bt_spawn_wave_extras. */
    for (int i = 0; i < (int)w->n_groups && n < BT_MAX_LIVE_GROUPS; i++)
        s->live_group[n++] = (unsigned short)(w->group_first + i);
    /* (1b) the generated endless groups, appended to the wave's own list by
     * ZoneService.get_wave_data (zone_service.gd:51) BEFORE wave_manager.init
     * adds the all-waves / elite / DLC groups.
     *   nb = int((wave / 10.0) * 2), bonus_enemies_per_group = nb / 5.0 */
    if (endless) {
        int nb = (int)((float)s->wave * 0.2f);
        bt_build_endless_groups(s, nb, BT_ENDLESS_DURATION, 0, (float)nb / 5.0f, &n);
    }
    /* (2) groups_data_in_all_waves */
    for (int i = 0; i < (int)BT_ZONE_ALLWAVE_GROUPS_N[s->zone] && n < BT_MAX_LIVE_GROUPS; i++)
        s->live_group[n++] = BT_ZONE_ALLWAVE_GROUPS[s->zone][i];
    /* (3b) extra_loot_aliens (Lure): wave_manager.gd:47-70 appends one DUPLICATE
     * of the zone's loot_alien_groups per unit of the summed
     * extra_loot_aliens_next_wave (+ extra_loot_aliens) effect, each with
     * `spawn_timing = rand_range(5, wave_timer.time_left - 10)` -- the timer's
     * time_left is the full wave duration at build time -- then clears the
     * effect.  They land between the all-waves block and the item extra-group /
     * elite / DLC blocks, and a duplicate keeps every template field (repeating
     * -1, area -1, spawn_dist 200, is_loot 1) except the timing. */
    {
        int n_loot = (int)s->scalar_effect[BT_ISTR_EXTRA_LOOT_ALIENS_NEXT_WAVE];
        if (n_loot > 0) {
            s->scalar_effect[BT_ISTR_EXTRA_LOOT_ALIENS_NEXT_WAVE] = 0.0f;
            for (int i = 0; i < n_loot; i++) {
                float t = bt_rand_range(s, 5.0f, s->wave_duration - 10.0f);
                if (n >= BT_MAX_LIVE_GROUPS) { s->group_overflow = 1; continue; }
                int g = n++;
                s->live_group[g] = BT_ZONE_LOOT_GROUPS[s->zone][0];
                s->lg_dynamic[g] = 2;              /* table row, timing overridden */
                s->lg_timing[g] = (unsigned short)(int)t;
            }
        }
    }
    /* per-group runtime bookkeeping (must precede the merge below) */
    for (int i = 0; i < BT_MAX_LIVE_GROUPS; i++) {
        s->group_fired[i] = 0;
        s->group_repeating[i] = 0;
        s->group_interval[i] = 0.0f;
        s->group_next_elapsed[i] = 0.0f;
        s->live_mult[i] = 1;
    }
    /* (4) item-granted ExtraEnemyEffect groups apply to this (next) wave. Dedup by
     * BT_GROUPS id with an owned multiplicity so identical copies never
     * truncate and each copy still fires its own unit draws/counts/timings. */
    for (int a = 0; a < s->n_exg_active; a++) {
        int r = (int)s->exg_active[a];
        if (s->exg_count[r] == 0) continue;
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[r];
        if (E->extra_group >= BT_NITEM_EXTRA_GROUPS) continue;
        unsigned short gi = BT_ITEM_EXTRA_GROUPS[E->extra_group][s->zone];
        float exg_sum = bt_row_sum_v(s, r, E);
        unsigned int mult = (unsigned int)(exg_sum > 0.0f ? (int)exg_sum : 1);
        int found = -1;
        for (int q = 0; q < n; q++) if (s->live_group[q] == gi) { found = q; break; }
        if (found >= 0) {
            unsigned int vv = (unsigned int)s->live_mult[found] + mult;
            s->live_mult[found] = (unsigned short)(vv > 65535u ? 65535u : vv);
        } else if (n < BT_MAX_LIVE_GROUPS) {
            s->live_group[n] = gi;
            s->live_mult[n] = (unsigned short)(mult > 65535u ? 65535u : mult);
            n++;
        }
    }
    /* (6) the elite/horde block: zone_data.horde_groups filtered by the wave,
     * in horde_groups order (wave_manager.gd:140-147). */
    if (bt_wave_is_horde(s, s->wave)) {
        for (int i = 0; i < (int)BT_ZONE_HORDE_GROUPS_N[s->zone] && n < BT_MAX_LIVE_GROUPS; i++) {
            unsigned short gi = BT_ZONE_HORDE_GROUPS[s->zone][i];
            BT_CONST_PTR(BtSpawnGroup) g2 = &BT_GROUPS[gi];
            if (s->wave < (int)g2->min_wave || s->wave > (int)g2->max_wave) continue;
            s->live_group[n++] = gi;
        }
    }
    /* (7) DLC groups_in_all_zones, appended after the elite/horde block. */
    for (int i = 0; i < (int)BT_ZONE_ALLZONE_GROUPS_N[s->zone] && n < BT_MAX_LIVE_GROUPS; i++)
        s->live_group[n++] = BT_ZONE_ALLZONE_GROUPS[s->zone][i];
    s->n_live_groups = n;
}

/* wave manager _physics_process + add_group_to_repeat/emit_and_update_repeat_group */
BT_HD static void bt_fire_groups(BT_DEVICE BtState* s, float elapsed) {
    for (int g = 0; g < s->n_live_groups; g++) {
        int dyn = (int)s->lg_dynamic[g];
        /* dyn 1 = a generated endless group (template + lg_* fields); dyn 2 = a
         * table row with only its spawn_timing overridden (a Lure loot-alien
         * duplicate); dyn 0 = a plain BT_GROUPS row. */
        BT_CONST_PTR(BtSpawnGroup) grp = (dyn == 1) ? &BT_ENDLESS_GROUP_TMPL[0]
                                                    : &BT_GROUPS[s->live_group[g]];
        float timing = (dyn != 0) ? (float)s->lg_timing[g] : (float)grp->timing;
        float interval = (dyn == 1) ? s->lg_interval[g] : grp->repeat_interval;
        if (dyn == 0) {
            if (grp->unit_count == 0) continue;
            /* wave_manager.gd:181: RunData.current_difficulty >= min_difficulty */
            if ((int)grp->min_difficulty > (int)s->difficulty) continue;
            if (s->wave < (int)grp->min_wave || s->wave > (int)grp->max_wave) continue;
        }
        int fire = 0, first = 0;
        /* trees_start_wave is Explorer-only; Bull uses the group's own timing. */
        if (!s->group_fired[g]) {
            if (timing <= elapsed || grp->is_boss) {
                s->group_fired[g] = 1;
                fire = 1;
                first = 1;
                if (grp->repeat > 0) {
                    s->group_repeating[g] = (unsigned char)(grp->repeat > 255 ? 255 : grp->repeat);
                    s->group_interval[g] = interval;
                    s->group_next_elapsed[g] = timing + interval;
                }
            }
        } else if (s->group_repeating[g] > 0) {
            if (elapsed >= s->group_next_elapsed[g]) {
                fire = 1;
                s->group_repeating[g]--;
                s->group_interval[g] -= grp->reduce_interval;
                if (s->group_interval[g] < grp->min_interval) s->group_interval[g] = grp->min_interval;
                s->group_next_elapsed[g] += s->group_interval[g];
            }
        }
        if (!fire) continue;
        if (grp->is_boss) continue;   /* bosses spawned explicitly by wave */
        /* wave_manager.gd:171-181: Utils.get_chance_success(group_data.
         * spawn_chance) is rolled ONLY in the frame the group's timing is first
         * reached (the group is then erased from groups_data and, if
         * repeating > 0, moved to _groups_to_repeat).  Every later emission goes
         * through emit_and_update_repeat_group (:197-215), which emits
         * group_spawn_timing_reached directly -- NO spawn_chance roll.  Rolling
         * it again here would burn one extra global draw per repeat (every
         * shipped group has spawn_chance 1.0, so the outcome never changes but
         * the whole downstream G stream shifts). */
        if (first && !bt_chance(s, grp->chance)) continue;
        if (dyn == 1) { (void)bt_spawn_dyn_group(s, g); continue; }
        for (int m = 0; m < (int)s->live_mult[g]; m++) bt_spawn_group(s, s->live_group[g]);
    }
}
/* --- elite/horde schedule (run_data.gd:629-679, D5) --- */
BT_HD static void bt_schedule_elites(BT_DEVICE BtState* s) {
    for (int i = 0; i < BT_ELITE_EVENTS; i++) {
        s->elite_wave[i] = 0; s->elite_type[i] = 0; s->elite_kind[i] = 0; s->elite_fired[i] = 0;
    }
    int nb = (int)BT_ELITE_NB_BY_DIFFICULTY[5];
    int remaining = (int)BT_ZONE_NELITES[s->zone];
    unsigned short available[BT_MAX_ELITES];
    for (int i = 0; i < remaining; i++) available[i] = BT_ZONE_ELITES[s->zone][i];
    /* run_data.gd:651-670: wave numbers use Utils._rng; horde chances and
     * Array.pick_random use the global stream. Even the forced final elite
     * first consumes its horde roll, and selected elites are erased. */
    int wave = bt_rng_randi_range(&s->utils_rng,
        BT_ELITE_BASE_WAVE + BT_ELITE_WAVE_LO[0],
        BT_ELITE_BASE_WAVE + BT_ELITE_WAVE_HI[0]);
    for (int i = 0; i < nb && i < BT_ELITE_EVENTS; i++) {
        int horde = bt_chance(s, BT_ELITE_HORDE_CHANCE);
        if (i > 0) wave = bt_rng_randi_range(&s->utils_rng,
            BT_ELITE_BASE_WAVE + BT_ELITE_WAVE_LO[i],
            BT_ELITE_BASE_WAVE + BT_ELITE_WAVE_HI[i]);
        if (BT_ELITE_FORCED_ELITE[i]) horde = 0;
        s->elite_wave[i] = (unsigned char)wave;
        s->elite_type[i] = (unsigned char)horde;
        if (!horde) {
            int pick = (int)bt_rng_bounded(&s->rng, (unsigned int)remaining);
            s->elite_kind[i] = available[pick];
            for (int j = pick + 1; j < remaining; j++) available[j - 1] = available[j];
            remaining--;
        }
    }
}

/* run_data.gd:603-621: the final wave samples `bosses_spawn` without
 * replacement -- nb_bosses = 2 under difficulty_5_effect_4 (double_boss, on
 * exactly while current_difficulty >= 5), else 1. */
BT_HD static void bt_schedule_bosses(BT_DEVICE BtState* s) {
    unsigned short available[BT_MAX_BOSSES];
    int remaining = (int)BT_ZONE_NBOSSES[s->zone];
    for (int i = 0; i < remaining; i++) available[i] = BT_ZONE_BOSSES[s->zone][i];
    int nb = (s->difficulty >= BT_RUN_DIFFICULTY) ? 2 : 1;
    for (int i = 0; i < nb && remaining > 0; i++) {
        int pick = (int)bt_rng_bounded(&s->rng, (unsigned int)remaining);
        s->boss_kind[i] = available[pick];
        for (int j = pick + 1; j < remaining; j++) available[j - 1] = available[j];
        remaining--;
    }
}

/* One boss/elite group, fired exactly as entity_spawner.gd:148-205 runs an
 * ordinary group.  The group resource is `zones/common/elite/group_elite.tres`
 * for a scheduled elite event and for the ShopCompletion G6 purchase, and
 * `zones/zone_1/020/group_1.tres` (or its zone-3 twin) for the final wave's
 * boss set; all of them export area = -1, spawn_dist_away_from_edges = 0,
 * spawn_edge_of_map = true, is_boss = true, spawn_chance = 1.0.
 * wave_manager.gd:96-134 gives the group one WaveUnitData per elite/boss
 * (min_number == max_number == 1, spawn_chance 1.0), so the source path is
 *   Utils.get_chance_success(spawn_chance)                 1 global draw
 *   get_group_pos                -> ZoneService.get_rand_pos(0)   2 draws
 *   get_group_pos_away_from_players                         0..N x 2 draws
 *   per unit: Utils.randi_range(1,1)                        1 Utils draw
 *             get_spawn_pos_in_area (edge of map)           1 Utils + 1 draw
 *             get_spawn_pos_away_from_players               0..N x (1+1) draws
 * number_total is exactly 1.0, so the residual is 0.0 and
 * Utils.get_chance_success(0.0) returns false with NO draw (utils.gd:397-401). */
BT_HD static void bt_spawn_boss_group_kinds(BT_DEVICE BtState* s, const int* kinds, int n) {
    if (n <= 0) return;
    (void)bt_chance(s, 1.0f);       /* wave_manager.gd:180 group spawn_chance roll */
    /* Only geometry is read by the two placement helpers below. */
    BtSpawnGroup grp;
    grp.zone = (unsigned char)s->zone;
    grp.timing = 0; grp.chance = 0.0f; grp.repeat = 0;
    grp.repeat_interval = 0.0f; grp.reduce_interval = 0.0f; grp.min_interval = 0.0f;
    grp.area = -1; grp.spawn_edge_of_map = 1; grp.spawn_dist_away_from_edges = 0;
    grp.is_neutral = 0; grp.is_boss = 1; grp.is_horde = 0; grp.is_loot = 0;
    grp.min_difficulty = 0; grp.min_wave = 0; grp.max_wave = 0;
    grp.unit_first = 0; grp.unit_count = 0; grp.group_id = 0;
    float bx, by;
    bt_group_pos(s, &grp, &bx, &by);
    for (int i = 0; i < n; i++) {
        (void)bt_rng_randi_range(&s->utils_rng, 1, 1);   /* Utils.randi_range(1,1) */
        float x, y;
        bt_pos_in_area(s, bx, by, (float)grp.area,
                       (float)grp.spawn_dist_away_from_edges,
                       (int)grp.spawn_edge_of_map, &x, &y);
        bt_spawn_pos_away_from_player(s, &grp, 0.0f, bx, by, &x, &y);
        bt_queue_boss(s, kinds[i], x, y);
    }
}

/* Two-kind convenience wrapper (a scheduled elite event, the G6 purchase and
 * the final-wave double boss all spawn <= 2 units). */
BT_HD static void bt_spawn_boss_group(BT_DEVICE BtState* s, int k0, int k1, int n) {
    int kinds[2];
    kinds[0] = k0; kinds[1] = k1;
    bt_spawn_boss_group_kinds(s, kinds, n);
}

/* run_data.gd:629-679 init_elites_spawn(base_wave, horde_chance) for the endless
 * reschedule (main.gd:931-933 clean_up_room: init_elites_spawn(current_wave + 10,
 * 0.0) every 10th wave at wave >= 10).  Appends up to BT_ELITE_EVENTS events into
 * the rolling oe_* buffer, compacting already-fired entries first (the source
 * list only grows; dropping fired events is equivalent and keeps the buffer
 * bounded to the two live decades).  D5 => 3 events; event 2 is always an ELITE;
 * wave numbers use Utils._rng and the elite pick uses the global stream, exactly
 * like bt_schedule_elites. */
BT_HD static void bt_schedule_elites_endless(BT_DEVICE BtState* s, int base_wave, float horde_chance) {
    /* run_data.gd:init_elites_spawn: `if check_elite_generation.has(base_wave):
     * return` -- a base already generated is a no-op (no draws).  A wave replay
     * (hourglass) re-ends the same multiple-of-10 wave, so this guard prevents a
     * duplicate schedule (and the extra draws the source never makes). */
    if (base_wave == s->oe_last_base) return;
    s->oe_last_base = base_wave;
    /* Compact before appending.  An entry is dead once it has fired OR once its
     * wave has already passed (oe_wave <= the wave that just ended) -- the
     * source list only grows and its entries are harmless there, but this buffer
     * is bounded, so a scheduled wave that was never visited (a skippable /
     * jumped wave, or the 255 clamp of oe_wave) must still be reclaimable or
     * oe_n would grow 3 per decade and trip oe_overflow at deep waves.  With the
     * predicate, the live entries are exactly those scheduled ahead of s->wave;
     * schedules are 10 waves apart and span base+1..base+8, so at most two
     * decades (2*BT_ELITE_EVENTS) are ever outstanding. */
    int w = 0;
    for (int i = 0; i < (int)s->oe_n; i++) {
        if (s->oe_fired[i] || (int)s->oe_wave[i] <= s->wave) continue;
        s->oe_wave[w] = s->oe_wave[i];
        s->oe_type[w] = s->oe_type[i];
        s->oe_kind[w] = s->oe_kind[i];
        s->oe_fired[w] = 0;
        w++;
    }
    s->oe_n = (unsigned char)w;

    int nb = (int)BT_ELITE_NB_BY_DIFFICULTY[5];
    int remaining = (int)BT_ZONE_NELITES[s->zone];
    unsigned short available[BT_MAX_ELITES];
    for (int i = 0; i < remaining; i++) available[i] = BT_ZONE_ELITES[s->zone][i];
    int wave = bt_rng_randi_range(&s->utils_rng,
        base_wave + BT_ELITE_WAVE_LO[0], base_wave + BT_ELITE_WAVE_HI[0]);
    for (int i = 0; i < nb && i < BT_ELITE_EVENTS; i++) {
        /* the horde roll is evaluated for every event, then overridden to ELITE
         * for event 2 (run_data.gd:658-666). */
        int horde = bt_chance(s, horde_chance);
        if (i > 0) wave = bt_rng_randi_range(&s->utils_rng,
            base_wave + BT_ELITE_WAVE_LO[i], base_wave + BT_ELITE_WAVE_HI[i]);
        if (BT_ELITE_FORCED_ELITE[i]) horde = 0;
        if ((int)s->oe_n >= BT_ENDLESS_ELITE_SLOTS) { s->oe_overflow = 1; break; }
        int k = (int)s->oe_n;
        s->oe_wave[k] = (unsigned char)(wave > 255 ? 255 : (wave < 0 ? 0 : wave));
        s->oe_type[k] = (unsigned char)horde;
        s->oe_kind[k] = 0;
        s->oe_fired[k] = 0;
        if (!horde) {
            int pick = (int)bt_rng_bounded(&s->rng, (unsigned int)remaining);
            s->oe_kind[k] = available[pick];
            for (int j = pick + 1; j < remaining; j++) available[j - 1] = available[j];
            remaining--;
        }
        s->oe_n++;
    }
}

/* Endless-wave boss/elite additions (wave_manager.gd:76-90 + run_data.gd:603-621,
 * 785-788).  Only waves whose reused wave resource carries the is_boss group
 * spawn these: base index 19, i.e. endless waves 30, 40, 50, ...  Draw order
 * follows the source: the extra endless elite group (init_elite_group, one elite
 * per ceil((wave-20)/10)) is DRAWN first, then get_bosses_to_spawn(true) samples
 * the two bosses; the boss group is spawned first (it precedes the appended elite
 * group in the wave's group list). */
BT_HD static void bt_spawn_wave_extras_endless(BT_DEVICE BtState* s) {
    if (!bt_endless_wave_has_boss_group(s->wave)) return;

    int ne = bt_endless_extra_elites(s->wave);
    if (ne > BT_MAX_ELITES) ne = BT_MAX_ELITES;
    int ekinds[BT_MAX_ELITES];
    int erem = (int)BT_ZONE_NELITES[s->zone];
    unsigned short eavail[BT_MAX_ELITES];
    for (int i = 0; i < erem; i++) eavail[i] = BT_ZONE_ELITES[s->zone][i];
    for (int i = 0; i < ne; i++) {
        if (erem <= 0) { ne = i; break; }
        int pick = (int)bt_rng_bounded(&s->rng, (unsigned int)erem);
        ekinds[i] = (int)eavail[pick];
        for (int j = pick + 1; j < erem; j++) eavail[j - 1] = eavail[j];
        erem--;
    }

    int nboss = (int)BT_ZONE_NBOSSES[s->zone];
    if (nboss > 2) nboss = 2;
    int bkinds[2];
    {
        unsigned short bavail[BT_MAX_BOSSES];
        int brem = nboss;
        for (int i = 0; i < brem; i++) bavail[i] = BT_ZONE_BOSSES[s->zone][i];
        for (int i = 0; i < nboss; i++) {
            int pick = (int)bt_rng_bounded(&s->rng, (unsigned int)brem);
            bkinds[i] = (int)bavail[pick];
            for (int j = pick + 1; j < brem; j++) bavail[j - 1] = bavail[j];
            brem--;
        }
    }

    bt_spawn_boss_group_kinds(s, bkinds, nboss);           /* the reused boss group */
    if (ne > 0) bt_spawn_boss_group_kinds(s, ekinds, ne);  /* the extra endless elite group */
}

/* Fire this wave's elite events + the final-wave bosses. A HORDE event does not
 * spawn anything here: its zone horde groups were appended to this wave's group
 * list (bt_build_live_groups) and fire on their own spawn_timing.  Each elite
 * event, the G6 purchase and the final-wave boss set is ONE group
 * (wave_manager.gd:96-134), so it is ONE spawn_chance roll plus one group
 * placement.  The new wave's main.tscn creates the player at the zone centre
 * before the WaveManager callback runs (entity_spawner.gd:89-101), so the
 * away-from-players geometry of that frame sees the centre. */
BT_HD static void bt_spawn_wave_extras(BT_DEVICE BtState* s) {
    s->is_horde_wave = (unsigned char)bt_wave_is_horde(s, s->wave);
    s->px = BT_ARENA_W * 0.5f;
    s->py = BT_ARENA_H * 0.5f;
    if (s->pending_extra_elite >= 0) {
        bt_spawn_boss_group(s, (int)s->pending_extra_elite, -1, 1);
        s->pending_extra_elite = -1;
    }
    for (int i = 0; i < BT_ELITE_EVENTS; i++) {
        if (s->elite_fired[i] || (int)s->elite_wave[i] != s->wave) continue;
        s->elite_fired[i] = 1;
        if (s->elite_type[i] == 1) continue;   /* horde: handled via the group list */
        bt_spawn_boss_group(s, (int)s->elite_kind[i], -1, 1);
    }
    /* Waves 21+ rescheduled elite events (main.gd:931-933 -> bt_schedule_elites_endless
     * at the previous decade's wave end).  The source appends these to the SAME
     * RunData.elites_spawn list, so they fire after the initial events above. */
    for (int i = 0; i < (int)s->oe_n; i++) {
        if (s->oe_fired[i] || (int)s->oe_wave[i] != s->wave) continue;
        s->oe_fired[i] = 1;
        if (s->oe_type[i] == 1) continue;   /* horde: via the group list */
        bt_spawn_boss_group(s, (int)s->oe_kind[i], -1, 1);
    }
    if (s->wave == BT_WAVES) {
        /* run_data.gd:603-621 + wave_manager.gd:114-121: the final wave spawns
         * `bosses_spawn`, i.e. difficulty_5_effect_4's double_boss pair under
         * current_difficulty >= 5 and a single boss below it. */
        int nb = (int)BT_ZONE_NBOSSES[s->zone];
        if (nb > 2) nb = 2;
        if (s->difficulty < BT_RUN_DIFFICULTY) nb = 1;
        if (nb > 0) bt_spawn_boss_group(s, (int)s->boss_kind[0],
                                        nb > 1 ? (int)s->boss_kind[1] : -1, nb);
    } else if (s->endless) {
        /* Endless waves reuse the wave-20 resource every 10th wave (base index 19)
         * and then spawn 2 bosses + an extra endless-elite group, exactly like the
         * final-wave set above (wave_manager.gd:76-90). */
        bt_spawn_wave_extras_endless(s);
    }
}

/* Nearest-enemy-projectile cutoff for the observation's three projectile slots:
 * a projectile is reported only while its squared distance to the player is
 * below this.  The bridge mod's own observer uses exactly this value
 * (bridge/mod/.../rl_obs.gd PROJ_MAX_D2 = 70000.0, documented in
 * bridge/state_contract.json obs[68..76]); the two must stay equal or the policy
 * trained here would read different projectile slots in the game. */
#ifndef BT_OBS_PROJ_MAX_D2
#define BT_OBS_PROJ_MAX_D2 70000.0f
#endif

/* Effective-stat accessor (defined later in this file, next to the observable
 * field sync): the un-gained-in-storage, gained-on-read stat value that
 * Utils.get_stat(stat_hash, 0) returns.  Declared here because bt_core_observe
 * needs it for the armor field. */
BT_HD static float bt_core_stat_read(BT_DEVICE BtState* s, int id);

/* Curated guide items whose owned copy count the v4 observation reports
 * (obs[173..196], min(count,4)/4).  These are the ArosRising High-Endless plan
 * items: the economy core (bag, crown, hunting trophy, coupon, dangerous bunny,
 * recycling machine, lure), the pre-pivot staples (anvil, alien eyes) and the
 * Spicy Sauce build (garden, improved tools, explosive shells, plastic
 * explosive, alien tongue, sad tomato, torture, padding, tardigrade, silver
 * bullet, grind's magical leaf, vigilante ring, bloody hand, spicy sauce,
 * alien magic).  Indices are BT_ITEM_IDX_* (brotato_items.h), stable for the
 * shipped 247-item table. */
BT_TABLE unsigned short BT_OBS_GUIDE_ITEMS[BT_OBS_T_ITEMS] = {
    BT_ITEM_IDX_BAG, BT_ITEM_IDX_CROWN, BT_ITEM_IDX_HUNTING_TROPHY,
    BT_ITEM_IDX_COUPON, BT_ITEM_IDX_DANGEROUS_BUNNY,
    BT_ITEM_IDX_RECYCLING_MACHINE, BT_ITEM_IDX_LURE, BT_ITEM_IDX_ANVIL,
    BT_ITEM_IDX_ALIEN_EYES, BT_ITEM_IDX_GARDEN, BT_ITEM_IDX_IMPROVED_TOOLS,
    BT_ITEM_IDX_EXPLOSIVE_SHELLS, BT_ITEM_IDX_PLASTIC_EXPLOSIVE,
    BT_ITEM_IDX_ALIEN_TONGUE, BT_ITEM_IDX_SAD_TOMATO, BT_ITEM_IDX_TORTURE,
    BT_ITEM_IDX_PADDING, BT_ITEM_IDX_TARDIGRADE, BT_ITEM_IDX_SILVER_BULLET,
    BT_ITEM_IDX_GRINDS_MAGICAL_LEAF, BT_ITEM_IDX_VIGILANTE_RING,
    BT_ITEM_IDX_BLOODY_HAND, BT_ITEM_IDX_SPICY_SAUCE, BT_ITEM_IDX_ALIEN_MAGIC
};

/* item_service.gd:578-586 get_upgrade_data: when the run owns the
 * level_upgrades_modifications effect (barnacle +35, captain +100) the offer is
 * DUPLICATED and every effect value is replaced by
 *   int(effect.value * (1.0 + level_upgrades_modifications / 100.0))
 * at generation time, so the shown magnitude (obs[27..34]) AND the applied stat
 * both use the modified value.  The source's `/100.0` and `1.0 + ...` are
 * binary64, and int() truncates toward zero. */
BT_HD static float bt_upgrade_offer_value(BT_DEVICE BtState* s, float base) {
    float mod = s->scalar_effect[BT_ISTR_LEVEL_UPGRADES_MODIFICATIONS];
    if (mod == 0.0f) return base;
    BtRngF64 v = bt_rf64_mul(bt_rf64_from_float(base), bt_rf64_one_plus_pct(mod));
    return (float)bt_rf64_to_int(v);
}

/* Encode the observation vector: exactly BT_OBS_SIZE floats into `o`. */
BT_HD void bt_core_observe(BT_DEVICE BtState* s, BT_DEVICE float* o) {
    if (o == 0) return;
    for (int i = 0; i < BT_OBS_SIZE; i++) o[i] = 0.0f;

    int b = 0;
    /* obs[0] = get_player_current_health / get_player_max_health
     * (rl_obs.gd:1064-1065,1080).  run_data.gd:295-298: current health is
     * players_data.current_health only while a wave is in progress
     * (wave_in_progress is cleared by on_wave_end at the shop transition,
     * run_data.gd:253) and the stat-layer max otherwise, so the shop phase
     * (core phase 1) always observes full health.  s->hp is the deferred
     * players_data.current_health; s->max_hp is the immediate stat layer both
     * here and in the denominator. */
    o[b++] = (s->phase == 1 ? s->max_hp : s->hp) / s->max_hp;
    o[b++] = s->max_hp * 0.015625f;     /* /64 */
    o[b++] = s->regen * 0.0333333f;     /* /30 */
    /* armor / 20 with the ARMOR STAT (Utils.get_stat(stat_armor_hash, 0)), the
     * value the bridge mod reads at rl_obs.gd:974 - NOT the player's truncated
     * max_stats.armor (player.gd:353) the damage path uses.  These differ only
     * when the gained armor stat is fractional (a stat_gain modifier); the
     * damage path keeps using the truncated deferred p_armor (player.gd:353
     * max_stats.armor copied into current_stats.armor). */
    o[b++] = bt_core_stat_read(s, 2) * 0.05f;   /* /20 */
    o[b++] = s->px / BT_ARENA_W;
    o[b++] = s->py / BT_ARENA_H;
    o[b++] = s->iframe > 0.0f ? 1.0f : 0.0f;
    o[b++] = (float)s->phase;
    o[b++] = s->materials * 0.01f;
    o[b++] = (float)s->level * 0.05f;
    o[b++] = s->xp / (s->xp_need > 0.0f ? s->xp_need : 1.0f);
    for (int i = 0; i < 4; i++) {
        unsigned short index = s->shop_kind[i] == BT_SHOP_ITEM &&
            !(s->phase == 1 && !(s->shop_available & (1u << i))) ? s->shop_id[i] : 0;
        BT_CONST_PTR(BtItem) it = &BT_ITEMS[index];
        float price = bt_observe_shop_price(s, i);
        o[b++] = (float)it->tier / 4.0f;
        o[b++] = s->materials >= price ? 1.0f : 0.0f;
        int dom = 0; float mag = 0.0f;
        for (int e = 0; e < it->count; e++) {
            BT_CONST_PTR(BtItemEffect) ef = &BT_ITEM_EFFECTS[it->first + e];
            if (bt_fabsf(ef->value) > bt_fabsf(mag)) { mag = ef->value; dom = ef->stat; }
        }
        o[b++] = (float)dom / 10.0f;
        o[b++] = bt_clampf(mag * 0.1f, -1.0f, 1.0f);
    }
    for (int i = 0; i < 4; i++) {
        unsigned short off = s->lvl_offers[i];
        unsigned char st; float raw; int real_offer;
        int row = bt_lvl_offer_row(off);   /* off = BT_UPGRADES row + 1, 0 = absent */
        if (off == (unsigned short)BT_UPGRADE_SLOT_OFFER) {
            /* item_service.gd:550-556 single-offer case (weapon_slot_upgrades):
             * only slot 0 exists; the effect is a run-level scalar so it carries
             * BT_STAT_NONE and is never level_upgrades_modifications-scaled. */
            st = (unsigned char)BT_UPGRADE_SLOT_STAT; raw = BT_UPGRADE_SLOT_VALUE; real_offer = 1;
        } else if (row < 0) {
            /* absent slot (the source's hidden UI node): emit the core's own
             * empty-slot encoding, BT_UPGRADES[0]. */
            st = BT_UPGRADES[0].stat; raw = BT_UPGRADES[0].value; real_offer = 0;
        } else {
            st = BT_UPGRADES[row].stat; raw = BT_UPGRADES[row].value; real_offer = 1;
        }
        /* A real offer shown in phase 2 carries the modified value; the
         * empty-slot encoding (combat/shop) is the raw row. */
        float v = (s->phase == 2 && real_offer) ? bt_upgrade_offer_value(s, raw) : raw;
        o[b++] = (float)st / 12.0f;
        o[b++] = bt_clampf(v * 0.1f, -1.0f, 1.0f);
    }

    float cd[BT_MAX];
    int ci[BT_MAX];
    int n = 0;
    for (int i = bt_enemy_next(s, 0, 3); i < (int)s->e_used; i = bt_enemy_next(s, i + 1, 3)) {
        /* rl_obs.gd:417-425 `_collect` walks the SCENE TREE, so a node whose
         * death animation is still playing (health 0, still drifting) is
         * reported exactly like a live one: its ratio reads 0 and the enemy
         * count feature below counts it too (the trace's `enemy_nodes`). */
        if (!s->ealive[i] && s->edying[i] == 0) continue;
        float dx = s->ex[i] - s->px, dy = s->ey[i] - s->py;
        cd[n] = dx * dx + dy * dy; ci[n] = i; n++;
    }
    int out = n < BT_SLOTS ? n : BT_SLOTS;
    for (int sl = 0; sl < out; sl++) {
        int best = sl;
        for (int j = sl + 1; j < n; j++) if (cd[j] < cd[best]) best = j;
        if (best != sl) {
            float td = cd[sl]; cd[sl] = cd[best]; cd[best] = td;
            int ti = ci[sl]; ci[sl] = ci[best]; ci[best] = ti;
        }
        int e = ci[sl];
        o[b++] = (s->ex[e] - s->px) / BT_ARENA_W;
        o[b++] = (s->ey[e] - s->py) / BT_ARENA_H;
        o[b++] = s->ehp[e] / s->emax[e];
        o[b++] = s->edmg[e] * 0.1f;
        /* obs[39+5k] is the SOURCE's `enemy.is_elite` boolean
         * (rl_obs.gd:488-491 reads Enemy.is_elite; ItemEnemy.gd:5 declares it,
         * boss.gd:4 defaults it true), NOT the core's 3-valued kind class in
         * eelite[].  The two differ exactly for bosses: every shipped boss kind
         * overrides the inherited default off (predator.tscn:26 and
         * invoker.tscn:24 zone 1, eel.tscn:29 and dead_whale.tscn:29 zone 2 set
         * `is_elite = false`), while every elite kind's scene sets it true
         * (butcher/colossus/croc/gargoyle/mantis/mom/monk/rhino in the base
         * game + the 9 DLC elites).  Writing the raw class put a 2 into the
         * field for a wave-20 boss; the game writes 0. */
        o[b++] = (float)(s->eelite[e] == 1 ? 1 : 0);
    }
    b += (BT_SLOTS - out) * 5;
    o[b++] = (float)s->wave / (float)BT_WAVES;
    /* Godot's Timer internal physics notification precedes the bridge/player
     * callbacks. At a movement-state snapshot the live timer has already
     * advanced for the upcoming step; paused level-up timers have not. */
    /* No -BT_DT: the clock tracks the game's timer frame for frame (the run
     * prologue and every wave restart consume their first frame). */
    float observed_time = s->time_left;
    if (observed_time < 0.0f) observed_time = 0.0f;
    o[b++] = s->wave_duration > 0.0f ? observed_time / s->wave_duration : 0.0f;
    /* The live-enemy count feature keeps its historical 1/100 scale (the
     * authored cap), NOT 1/BT_MAX, which is now the endless pool bound. */
    o[b++] = (float)n * BT_ENEMY_COUNT_SCALE;

    for (int slot = 0; slot < 3; slot++) {
        int best = -1; float bd = 1e18f;
        for (int i = 0; i < (int)s->pr_used; i++) {
            /* pralive: 0 free, 1 alive, 2 consumed by an earlier slot in this
             * same observation (restored below) - a projectile already reported
             * must not fill a second slot (the bridge consumes each node). */
            if (s->pralive[i] != 1) continue;
            /* Enemy-projectile slots only: the game puts enemy shots in the
             * EnemyProjectiles container (shooting_attack_behavior.gd:203 ->
             * main.add_enemy_projectile) and everything the player owns - weapon
             * shots, pet/turret shots, item special projectiles - in
             * _player_projectiles (weapon_service.gd:392).  A pet/turret shot is
             * friendly with no source enemy (prsrc 0xFFFF); a charmed enemy's
             * shot is ALSO friendly (it hits enemies) but keeps its firing
             * enemy slot, and the game still parents it under EnemyProjectiles,
             * so it stays in.  bt_player_damage reads the same prsrc. */
            if (s->prfriendly[i] && s->prsrc[i] == 0xFFFFu) continue;
            float dx = s->prx[i] - s->px, dy = s->pry[i] - s->py;
            float d = dx * dx + dy * dy;
            if (d < bd) { bd = d; best = i; }
        }
        if (best >= 0 && bd < BT_OBS_PROJ_MAX_D2) {
            o[b++] = (s->prx[best] - s->px) / BT_ARENA_W;
            o[b++] = (s->pry[best] - s->py) / BT_ARENA_H;
            o[b++] = s->prdmg[best] * 0.1f;
            s->pralive[best] = 2;
        } else { o[b++] = 0; o[b++] = 0; o[b++] = 0; }
    }
    for (int i = 0; i < (int)s->pr_used; i++) if (s->pralive[i] == 2) s->pralive[i] = 1;

    {
        int best = -1; float bd = 1e18f;
        for (int i = 0; i < (int)s->con_used; i++) {
            if (!s->conalive[i]) continue;
            float dx = s->conx[i] - s->px, dy = s->cony[i] - s->py;
            float d = dx * dx + dy * dy;
            if (d < bd) { bd = d; best = i; }
        }
        if (best >= 0) {
            o[b++] = (s->conx[best] - s->px) / BT_ARENA_W;
            o[b++] = (s->cony[best] - s->py) / BT_ARENA_H;
        } else { o[b++] = 0; o[b++] = 0; }
    }
    int tb = bt_weapon_observe_append(s, o, b);
    /* ---- Demon/endless block (v4 + v5 tail, BT_OBS_TARGET floats) --------
     * 161..164 stats/scale, 165..172 shop-owned state (offer lock flags, crate
     * offer), 173 level-up reroll price, 174..197 curated guide-item owned
     * counts, 198 the ban token budget and 199..202 the four level-up offer
     * presence flags (v5). */
    o[tb++] = s->harvest * 0.01f;             /* harvesting / 100 */
    o[tb++] = s->luck * 0.01f;                /* luck / 100 */
    {
        /* RunData.get_endless_factor(): 0 through wave 20, then the endless
         * scaling; squashed to [0,1) so the value stays bounded in endless. */
        float ef = bt_loot_endless_factor(s->wave);
        if (ef < 0.0f) ef = 0.0f;
        o[tb++] = ef / (1.0f + ef);
    }
    o[tb++] = (float)s->wave * 0.02f;         /* wave / 50 (endless scale) */
    /* shop-owned state (AuditShop): offer lock flags, the held crate offer and
     * the level-up reroll price.  shop_locked[i] is rank+1 (0 = unlocked), so
     * the observable is a 0/1 flag; the crate kind uses the same 0/1/2
     * encoding as the typed-offer block; ready == the crate offer exists (the
     * sim has no separate purchase-delay timer for it). */
    for (int li = 0; li < BT_OBS_T_LOCKS; li++) o[tb++] = s->shop_locked[li] ? 1.0f : 0.0f;
    o[tb++] = (float)s->crate_kind;
    o[tb++] = s->crate_kind == BT_SHOP_ITEM
        ? (float)(s->crate_id + 1) / (float)(BT_NITEMS + 1) : 0.0f;
    o[tb++] = s->crate_kind != BT_SHOP_NONE ? (float)s->crate_cursed : 0.0f;
    o[tb++] = s->crate_kind != BT_SHOP_NONE ? 1.0f : 0.0f;
    o[tb++] = (float)s->lvl_reroll_price;
    for (int gi = 0; gi < BT_OBS_T_ITEMS; gi++) {
        unsigned short owned = s->item_owned[BT_OBS_GUIDE_ITEMS[gi]];
        if (owned > 4) owned = 4;
        o[tb++] = (float)owned * 0.25f;       /* min(owned,4) / 4 */
    }
    /* ---- v5 tail (BT_OBS_T_BAN + BT_OBS_T_LVLOFF) -----------------------
     * The two remaining bt_action_legal conditions the v4 width could not
     * express: the ban-button budget and the per-slot level-up offer presence.
     * ban tokens: 0 when ban_mode is off, else ban_tokens / BT_BAN_MAX_TOKEN
     * (nonzero iff bt_action_legal's ban branch can pass its token gate).
     * presence: 1.0 for a real offer (a BT_UPGRADES row or the weapon-slot
     * sentinel) - exactly bt_lvl_offer_present, the same decode the observer
     * writes above, so the empty-slot encoding never reads as present. */
    o[tb++] = s->ban_mode ? (float)s->ban_tokens / (float)BT_BAN_MAX_TOKEN : 0.0f;
    for (int i = 0; i < BT_OBS_T_LVLOFF; i++)
        o[tb++] = bt_lvl_offer_present(s, i) ? 1.0f : 0.0f;
}

/* Per-phase action mask for Agent.action_mask (one byte per id, 1 = legal).
 * The head is phase-factored (brotato_state.h), so the mask has to name the same
 * conditions the game's own handlers refuse on; it is defined at the END of this
 * header, where every predicate it needs is visible. */
BT_HD void bt_action_mask(BT_DEVICE BtState* s, BT_DEVICE unsigned char* mask);
BT_HD void bt_core_reset(BT_DEVICE BtState* s);

/* Configure a fresh run; profile and selected starting weapon persist on reset. */
BT_HD void bt_core_init(BT_DEVICE BtState* s, bt_u64 seed, int zone, int shop_steps,
        unsigned short character, unsigned short starting_weapon,
        unsigned char audio_rng = 1) {
    /* Init accepts uninitialized storage; subsequent resets only clear slots
     * that have actually been allocated. */
    s->e_used = BT_MAX;
    for (int word = 0; word < (BT_MAX + 31) / 32; word++)
        s->e_live_bits[word] = s->e_dying_bits[word] = 0u;
    s->pr_used = BT_MAX_PROJ;
    s->con_used = BT_MAX_CONS;
    s->mat_used = BT_MAX_MAT;
    s->wproj.used = BT_MAX_WPROJ;
    s->expl.used = BT_MAX_EXPL;
    s->character = character;
    s->starting_weapon = starting_weapon;
    s->audio_rng = audio_rng;   /* 1 = source audio draws; 0 = bridge-neutralized */
    s->player_speed = BT_PLAYER_SPEED;
    s->danger_hp = BT_DANGER_HP;
    s->danger_dmg = BT_DANGER_DMG;
    s->wave_ovr = 0.0f;   /* table wave durations unless a replay hook sets it */
    /* Particle-RNG model: 1 (legacy) = the sim consumes the source's
     * CPUParticles2D per-particle draws; 0 = the bridge cleared every
     * CPUParticles2D's emitting flag.  bt_core_reset never clears it, so a
     * caller (env/harness/replay) sets 0 once after bt_core_init; the prologue
     * draws none of them (the player's running-smoke node only emits after a
     * landed run), and the exported bt_core_init arity stays unchanged. */
    s->particles_rng = 1;
    /* DebugService.invulnerable: campaign off by default.  The host puf_init /
     * harness sets it from the optional `invulnerable` [env] key AFTER
     * bt_core_init, and bt_core_reset never clears it (like `endless`), so it
     * persists across episodes. */
    s->invulnerable = 0;
    s->spawn_rate = 3.0f;
    /* shop_items_container.gd:27-28 `_buy_delay_timer.wait_time = 0.05`: after a
     * completed purchase the container refuses every buy/steal for that Timer's
     * life, so the shop's post-purchase dead window is bt_timer_frames(0.05) = 4
     * frames (0.05 s at 60 Hz; the same value tests/brotato_determinism_hash.cpp
     * uses).  shop_steps <= 0 therefore selects the SOURCE delay, and an explicit
     * positive value overrides it. */
    s->shop_steps = shop_steps < 1 ? BT_SHOP_BUY_DELAY_TICKS : shop_steps;
    s->ban_mode = 1;   /* rl_env.gd:34 default_settings() forces
                        * ProgressData.settings.ban_mode_toggled = true for every RL
                        * run, and rl_run.gd:467 copies that into uses_ban, so the
                        * game's ban button is live (the trace's shop_state shows
                        * ban_tokens 8 and an applied ban).  A caller that wants the
                        * ban UI off must clear it explicitly. */
    s->lock_default_items = 0;   /* fully unlocked profile (see the field comment) */
    s->zone = (zone < 0 || zone >= BT_ZONES) ? 0 : zone;
    s->seed = seed;
    s->rng_ready = 0;
    /* Endless is a run option (RunData.is_endless_run); campaign by default.
     * The host puf_init / puf_vec_create set it from the `endless` [env] key
     * AFTER bt_core_init, and bt_core_reset never clears it. */
    s->endless = 0;
    /* Starting wave: campaign wave 1 by default (DebugService.starting_wave).
     * The host puf_init / puf_vec_create set it from the optional `start_wave`
     * [env] key AFTER bt_core_init, and bt_core_reset never clears it. */
    s->start_wave = 1;
    s->endless_max_wave = 0;
    s->all_last_wave_bosses_killed = 0;
    bt_capacity_clear(s);   /* a fresh run configuration starts with a clean latch */
    /* RunData.current_difficulty.  This core models the Danger-5 profile
     * (BT_RUN_DIFFICULTY) unless a harness overrides it; bt_core_reset never
     * clears it, like `endless`. */
    s->difficulty = BT_RUN_DIFFICULTY;
    s->log.perf = 0.0f; s->log.score = 0.0f; s->log.episode_return = 0.0f;
    s->log.episode_length = 0.0f; s->log.n = 0.0f;
    s->log.rw_total = 0.0f;
    for (int i = 0; i < BT_RW_NTERMS; i++) s->log.rw[i] = 0.0f;
    bt_core_reset(s);
}

/* Enter wave `w` (1-based): duration, groups, elite schedule, extras.
 *
 * Out of line on every GPU backend, not just Metal: the body is huge (~74k PTX
 * lines) and every call site is cold (bt_core_reset once per episode, and
 * bt_core_step once per wave).  Without it nvcc inlines a full second and third
 * copy into bt_gpu_init_kernel and bt_gpu_reset_kernel, so ptxas optimizes and
 * register-allocates the same cold code three times: that was ~22 min of ptxas
 * per brotato CUDA compile on this host (and the build was OOM-killed).  The
 * call boundary changes no arithmetic (fmad=false / -ffp-contract=off pinning
 * is untouched), and the call site in bt_core_step is already a call today. */
#if defined(__METAL_VERSION__) || defined(__CUDACC__)
__attribute__((noinline))
#endif
BT_HD static void bt_enter_wave(BT_DEVICE BtState* s, int w) {
    s->wave = w;
    if (w > s->endless_max_wave) s->endless_max_wave = w;
    /* player.gd:110 Player._ready: the per-wave hit_protection charges are
     * re-armed from the owned effect value (tardigrade; a run can own more
     * than one copy, max_nb -1). */
    {
        float hp_eff = s->scalar_effect[BT_ISTR_HIT_PROTECTION];
        int hp_n = (int)hp_eff;
        if (hp_n < 0) hp_n = 0;
        if (hp_n > 255) hp_n = 255;
        s->hit_prot = (unsigned char)hp_n;
    }
    s->items_spawned_this_wave = 0;
    s->wave_duration = bt_wave_duration(s->zone, w);
    if (s->wave_ovr > 0.0f) s->wave_duration = s->wave_ovr;   /* instant_waves trace */
    if (s->wave_duration <= 0.0f) s->wave_duration = 90.0f;
    s->time_left = s->wave_duration;
    s->tick_in_wave = 0;
    /* A fresh arena scene builds a fresh WaveTimer/TickTimer pair; the clock
     * starts it on its first sub-6 s frame (see bt_wave_tick_sound). */
    s->tick_sound_left = 0;
    bt_enemy_pool_reset(s);
    s->hot_cursor = 0; s->hot_pending = 0;
    for (int i = 0; i <= BT_HOT_MAX_FRAMES; i++) s->hot_due[i] = 0;
    for (int g = 0; g < s->n_gwb; g++) { s->gwb_kills[g] = 0; s->gwb_grants[g] = 0; }
    /* TempStats.reset() ran at the previous wave's end (bt_progress_end_wave ->
     * bt_core_stat_reset_temp); this wave only resets the consumable-stat
     * counters and drops stale decaying-stat bookkeeping. */
    for (int i = 0; i < BT_NITEM_EFF_ROWS; i++) s->cs_count[i] = 0;
    /* the per-row var_cs runs are exactly var_cs[0..rvar_used) */
    for (int q = 0; q < (int)s->rvar_used; q++) s->var_cs[q] = 0;
    for (int d = 0; d < s->n_dt; d++) {
        s->dt_val[d] = 0.0f; s->dt_remain[d] = 0.0f;
    }
    /* Main._ready chooses the background gradient before creating the player
     * (main.gd:139 -> ItemService.get_background_gradient_color -> pick_random).
     * The visual-only choice still advances the shared global stream. */
    (void)bt_rng_next(&s->rng);
    /* A fresh Main creates the player: Unit.init rolls speed jitter even
     * though player.tscn's speed_randomization is zero (unit.gd:77). */
    (void)bt_rand_range(s, 0.0f, 0.0f);
    bt_player_visual_reset(s);
    bt_wfx_reset_wave(&s->wfx);
    /* The run's FIRST wave creates the starting-weapon copies (the game grants
     * add_starting_items_and_weapons() at boot, before any wave).  For a
     * campaign run that is wave 1; a recording that starts later (the endless
     * `starting_wave` boot, run_data.gd:549) grants them on its own first wave
     * instead of skipping the grant and ending up weaponless. */
    if (w == (s->start_wave > 0 ? (int)s->start_wave : 1)) bt_character_starting_apply(s);
    /* Other waves re-create every player weapon node from the RunData loadout
     * (a fresh game scene per wave), which draws each weapon's fresh
     * get_next_cooldown(at_wave_begin = true) in slot order.  Wave 1's copies are
     * created by bt_character_starting_apply above, which already draws. */
    else bt_weapon_wave_begin(s);
    bt_build_live_groups(s);
    /* #1 gain_random_primary_stats_on_go_to_next_wave: [count, chance%]
     * (main.gd:254-260: `chance = gain_stat[1]` then
     * `Utils.get_chance_success(float(chance) / 100)` for EVERY row, THEN
     * `gain_stat[0]` draws of RunData.get_random_primary_stats()). */
    for (int a = 0; a < s->n_bk_active; a++) {
        int r = (int)s->bk_active[a];
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[r];
        if (!bt_eff_is(E, BT_ISTR_GAIN_RANDOM_PRIMARY_STATS_ON_GO_TO_NEXT_WAVE)) continue;
        for (int c = 0, nown = bt_row_owned(s, r); c < nown; c++) {   /* per owned copy */
            int v, v2;
            if (!bt_row_get(s, r, E, c, &v, &v2)) break;
            if (!bt_chance_pct(s, v2)) continue;
            for (int k = 0; k < v; k++)
                /* RunData.add_stat (run_data.gd:1400-1406) writes the PERMANENT
                 * effects[stat] layer, not TempStats -- the gain survives the
                 * wave boundary.  RunData.get_random_primary_stats is the
                 * 16-entry uniform pick.  (main.gd:262 also bumps the candy bag
                 * tracked value per stat; the core has no tracked-value sink for
                 * a key no stat scaling reads.) */
                bt_core_stat_add(s, (int)BT_PRIMARY_STATS[bt_rng_bounded(&s->rng, 16u)], 1.0f);
        }
    }
    bt_temporal_apply_stats_next_wave(s);
    /* main.gd:134 chooses the next gold recipient even for one player. */
    (void)bt_rng_next(&s->utils_rng);
    /* main.gd:156 _tile_map.init(current_zone) -> global/my_tile_map.gd:24-30:
     *   for i in zone.width: for j in zone.height: my_set_cell(i, j)
     * and my_set_cell's get_subtile_with_priority() ends in
     * `tile_array[Utils.randi() % tile_array.size()]` -- exactly ONE Utils._rng
     * draw per TILE, in row-major order, once per wave (every wave instantiates a
     * fresh main.tscn).  zone_1_data.tres exports width=32 height=24 for every
     * shipped zone, and the sim's arena is BT_ARENA_W/BT_TILE x
     * BT_ARENA_H/BT_TILE tiles, so this is 768 draws per wave.  They land AFTER
     * main.gd:134's gold-recipient draw and BEFORE the WaveManager's first
     * group-timing check (frame 1), which is where this loop now sits: without
     * them the whole Utils stream of the wave is offset by 768 and every group
     * count roll (Utils.randi_range in entity_spawner.gd:175) diverges. */
    {
        int tiles = (int)(BT_ARENA_W / BT_TILE) * (int)(BT_ARENA_H / BT_TILE);
        for (int t = 0; t < tiles; t++) (void)bt_rng_next(&s->utils_rng);
    }
    /* entity_spawner.clean_up_room: a new room starts with empty queues and no
     * pending births (nothing carries over between waves), and every
     * structure/pet entity dies (a fresh main.tscn per wave restarts the
     * StructureTimer's _base_*_spawned flags). */
    s->n_pend_enemy = 0; s->n_pend_tree = 0; s->n_pend_boss = 0; s->n_pend_summons = 0;
    s->pend_overflow = 0;
    s->pend_drop_enemy = 0; s->pend_drop_tree = 0; s->pend_drop_boss = 0;
    s->pend_drop_summons = 0; s->pend_drop_birth = 0;
    s->pr_overflow = 0;
    s->pr_dropped = 0;
    s->pr_cre_next = 0;
    s->n_births = 0;
    s->cur_spawn_delay = 0;
    bt_clear_wave_summons(s);
    bt_zero_u8(s->ealive, s->e_used);
    s->pool_cull_fallback = 0;
    s->elite_slot_pressure = 0;
    /* the previous wave's dying nodes belong to the scene that change_scene()d
     * away (main.gd:1727 queue_frees the whole pool), and no deferred death
     * survives the reset. */
    bt_zero_u8(s->edying, s->e_used);
    for (int i = 0; i < (int)s->e_used; i++) { s->edpos[i] = 0.0f; s->edspd[i] = 0.0f; s->edidl[i] = 1.0f; }
    bt_zero_u8(s->epend_die, s->e_used);
    s->n_kill_q = 0;
    s->n_kill_tq = 0;
    bt_zero_u8(s->eneutral, s->e_used);
    bt_zero_u8(s->ehit, s->e_used);
    bt_zero_u8(s->ecurse, s->e_used);
    s->epend = -1;                     /* no deferred contact survives a reset */
    for (int i = 0; i < (int)s->e_used; i++) {
        s->phys.bodies[i].active = 0;
        s->e_hold_tick[i] = 0xFFFFu;   /* no newborn hold (see e_hold_tick) */
        /* the old arena's enemies are freed with their `_burning` state, so no
         * burn may leak into a reused slot (unit.gd:618-646) */
        s->eburn[i] = 0.0f;
        s->eburndmg[i] = 0.0f;
        /* no boost size survives a reset: every slot starts from its kind row
         * (bt_kgeom reads egeom, so a stale scaled row must never leak) */
        s->eboost_n[i] = 0;
        s->mv_boost[i] = 0;
        s->egeom_kind[i] = 0xFFu; s->egeom_n[i] = 0xFFu;
        bt_egeom_refresh(s, i);
    }
    s->e_used = 0;
    for (int word = 0; word < (BT_MAX + 31) / 32; word++)
        s->e_live_bits[word] = s->e_dying_bits[word] = 0u;
    s->ecount = 0;
    bt_spawn_wave_extras(s);
}

/* Full episode reset. Seeds the PCG32 stream on the first call only
 * (rng_ready latch) and lets it run across episodes. Does not touch s->log.
 *
 * Out of line on every GPU backend for the same compile-time reason as
 * bt_enter_wave: bt_core_init calls this and bt_gpu_init_kernel inlines
 * bt_core_init, so without the attribute the whole reset path (bt_enter_wave
 * included) is expanded once into each of the two cold kernels. */
#if defined(__METAL_VERSION__) || defined(__CUDACC__)
__attribute__((noinline))
#endif
BT_HD void bt_core_reset(BT_DEVICE BtState* s) {
    if (!s->rng_ready) {
        bt_rng_seed(&s->rng, s->seed);
        bt_rng_seed(&s->utils_rng, s->seed);   /* Utils singleton stream */
        s->rng_ready = 1;
    }
    bt_birth_audio_reset(s);
    bt_main_audio_reset(s);
    bt_player_visual_reset(s);
    bt_wproj_reset(s);
    bt_wfx_reset(&s->wfx);
    /* brotato_weapon_set_runtime.h: no set is owned until a weapon set
     * item grants one (0xFF = unowned). */
    for (int ws = 0; ws < BT_NWEAPON_SETS; ws++) s->wpn_set_tier[ws] = 0xFF;
    bt_weapon_reset(s);
    bt_melee_reset(s);
    bt_ranged_reset(s);
    s->n_wtb = s->n_wcb = s->n_soh = s->n_wss = 0;
    s->n_min_wcd = s->n_max_wcd = s->n_mxp = 0;
    /* item-related weapon bonuses are dropped WITHOUT unapply: the stat layers
     * are zeroed below, so the next refresh re-derives from an empty inventory
     * with nothing to undo (brotato_item_weapon_bonus.h). */
    s->n_iwb = 0;
    s->n_iwb_app = 0;
    for (int i = 0; i < 4; i++) s->iwb_count[i] = 0;
    for (int i = 0; i < (int)s->e_used; i++) {
        s->e_charmed[i] = 0; s->e_charm_by[i] = -1;
        s->e_charm_timer[i] = 0.0f; s->e_charm_shield[i] = 0;
    }
    for (int i = 0; i < 4; i++) s->charm_count[i] = 0;
    /* Preset derived health before the profile's first stat synchronization. */
    s->max_hp = 0.0f; s->hp = 0.0f; s->regen = 0.0f;
    for (int i = 0; i < BT_NSTATS; i++) {
        s->stat_raw[i] = 0.0f; s->temp_stat[i] = 0.0f; s->link_stat[i] = 0.0f;
        s->stat_gain[i] = 1.0f; s->stat_cap[i] = BT_STAT_CAP_NONE;
    }
    bt_temporal_reset(s);
    s->stat_raw[0] = BT_BASE_HP;
    s->stat_cap[4] = BT_DODGE_CAP_DEFAULT;   /* player_run_data.init_stats */
    bt_core_stat_sync(s);
    s->hp = s->max_hp;
    s->px = BT_ARENA_W * 0.5f;
    s->py = BT_ARENA_H * 0.5f;
    s->ticks = 0; s->kills = 0; s->waves_cleared = 0;
    s->done = 0; s->won = 0; s->cleared_game = 0;
    s->episode_return = 0.0f; s->dmg_taken = 0.0f;
    s->iframe = 0.0f; s->phase = 0; s->dmg_stat = 0.0f;
    s->speed_bonus = 1.0f;
    s->regen_timer = bt_regen_period(s->regen);
    bt_zero_u8(s->pralive, s->pr_used);
    bt_zero_u8(s->prfriendly, s->pr_used);
    bt_zero_u8(s->pr_pet, s->pr_used);
    s->pr_overflow = 0;
    s->pr_dropped = 0;
    for (int i = 0; i < (int)s->pr_used; i++) {
        s->prburn_chance[i] = 0.0f; s->prburn_dmg[i] = 0.0f; s->prburn_dur[i] = 0.0f;
        s->prcrit_chance[i] = 0.0f; s->prcrit_dmg[i] = 0.0f;
    }
    bt_zero_u8(s->conalive, s->con_used);
    bt_zero_u8(s->matalive, s->mat_used);
    s->matcount = 0;
    s->n_live_groups = 0;
    s->is_horde_wave = 0;

    /* Progression block: episode-1 (creation) values. The observable stat
     * fields were derived by bt_core_stat_sync() above; crit 3% / crit_dmg 1.5
     * are bull_explosion_stats.tres, the Bull's only damage source (the weapon
     * base crit is added on top of the player stat at the explosion). */
    s->materials = BT_START_MATERIALS;   /* DebugService.starting_gold = 30 */
    s->pet_n = 0;
    s->burn_chance = 0.0f; s->burn_dmg = 0.0f; s->burn_dur = 0.0f;
    for (int i = 0; i < BT_NITEM_EFF_ROWS; i++) s->lk_count[i] = 0;
    s->n_tsi = 0; s->n_gwb = 0;
    s->stat_reg_overflow = 0;
    for (int i = 0; i < 8; i++) { s->dt_dur[i] = 0.0f; }
    bt_enemy_hit_reset(s);   /* enemy_percent_damage_taken debuff records */
    s->n_exg_active = 0; s->n_bk_active = 0; s->n_dt = 0;
    s->curse_apply = 0;
    s->curse_maxmod = 0.0f;
    for (int i = 0; i < 4; i++) s->shop_cursed[i] = 0;
    for (int i = 0; i < 4; i++) s->shop_locked[i] = 0;
    s->crate_kind = BT_SHOP_NONE; s->crate_id = 0; s->crate_cursed = 0;
    s->crate_extra_count = 0;
    s->lvl_reroll_count = 0; s->lvl_reroll_price = 0;
    s->ban_count = 0;
    /* run_data.gd:559-560: every run starts with BAN_MAX_TOKEN = 8 tokens; the
     * ban UI itself is gated by settings.ban_mode_toggled (run_data.gd:475,198). */
    s->ban_tokens = BT_BAN_MAX_TOKEN;
    s->shop_effects_checked = 0;
    for (int i = 0; i < BT_NITEM_EFF_ROWS; i++) {
        s->bk_count[i] = 0; s->exg_count[i] = 0; s->cs_count[i] = 0;
        s->nvar[i] = 0; s->rvar_off[i] = 0; s->rvar_cap[i] = 0;
    }
    s->rvar_used = 0;   /* the whole ragged variant pool is free again */
    s->curse_overflow = 0;
    s->link_refresh_timer = 0.5f;
    /* entity_spawner.gd clean_up_room + fresh wave: empty queues, no births */
    s->n_pend_enemy = 0; s->n_pend_tree = 0; s->n_pend_boss = 0; s->n_pend_summons = 0;
    s->pend_overflow = 0;
    s->pend_drop_enemy = 0; s->pend_drop_tree = 0; s->pend_drop_boss = 0;
    s->pend_drop_summons = 0; s->pend_drop_birth = 0;
    s->pr_overflow = 0;
    s->pr_dropped = 0;
    s->n_births = 0;
    s->cur_spawn_delay = 0;
    /* item-granted structures/pets: nothing is owned and no StructureTimer tick
     * has run yet */
    s->n_summon_fx = 0;
    s->summon_overflow = 0;
    s->base_struct_spawned = 0;
    s->base_pet_spawned = 0;
    s->n_pend_struct = 0;
    s->n_pend_pet = 0;
    s->structure_attack_speed = 0.0f;
    s->n_scdr = 0;
    for (int i = 0; i < BT_NITEM_STRS; i++) s->scalar_effect[i] = 0.0f;
    s->scalar_effect[BT_ISTR_CAN_ATTACK_WHILE_MOVING] = 1.0f;
    /* player_run_data.gd:501 seeds can_burn_enemies = true */
    if ((int)BT_ISTR_CAN_BURN_ENEMIES < BT_NITEM_STRS)
        s->scalar_effect[BT_ISTR_CAN_BURN_ENEMIES] = 1.0f;
    s->player_facing = 1;
    bt_character_apply(s);
    bt_character_query_links(s);
    bt_core_stat_sync(s);
    bt_player_stats_reset(s);   /* fresh player: LiveStats == stat layer, full HP */
    s->hp = s->max_hp;
    s->regen_timer = bt_regen_period(s->regen);
    for (int i = 0; i < BT_NITEMS; i++) s->item_owned[i] = 0;
    for (int i = 0; i < BT_MAX_PET; i++) {
        s->pet_alive[i] = 0; s->pet_perm[i] = 0; s->pet_isstruct[i] = 0;
        s->pet_double[i] = 0.0f; s->pet_lm_cd[i] = 0.0f; s->pet_lm_timer[i] = 0.0f;
        s->pet_burn_chance[i] = 0.0f; s->pet_burn_dmg[i] = 0.0f; s->pet_burn_dur[i] = 0.0f;
        s->pet_crit_chance[i] = 0.0f; s->pet_crit_dmg[i] = 0.0f;
        /* pet motion / interaction zones (bt_step_pet_motion rebuilds these the
         * first frame a slot is alive; zeroed here so a reset needs no slot pass) */
        s->pet_fx[i] = 0xFFFFu; s->pet_ai_row[i] = 0xFFFFu; s->pet_ai_init[i] = 0;
        s->pet_dead[i] = 0; s->pet_zone_in[i] = 0; s->pet_tree_in[i] = 0;
        s->pet_melee[i] = 0; s->pet_mad[i] = 0;
        s->pet_speed[i] = 0.0f; s->pet_hp[i] = 0.0f; s->pet_maxhp[i] = 0.0f;
        s->pet_float_hp[i] = 0.0f; s->pet_hurt_cd[i] = 0.0f; s->pet_revive_dur[i] = 0.0f;
        s->pet_melee_cd[i] = 0.0f;
        s->pet_tx[i] = 0.0f; s->pet_ty[i] = 0.0f;
        s->pet_pangle[i] = 0.0f; s->pet_pphase[i] = 0.0f; s->pet_pradius[i] = 0.0f;
    }
    for (int i = 0; i < (int)s->mat_used; i++) s->mat_by[i] = 0;
    /* the dropped-item lifecycle vectors (mat_arc/con_arc).  bt_core_reset is
     * defined above brotato_loot.h, so the row width is taken from the array
     * itself rather than BT_LOOT_ARC_N (same value, no include-order coupling). */
    {
        int arc_n = (int)(sizeof(s->mat_arc[0]) / sizeof(s->mat_arc[0][0]));
        for (int i = 0; i < (int)s->mat_used; i++) {
            s->mat_attr[i] = 0;
            for (int c = 0; c < arc_n; c++) s->mat_arc[i][c] = 0.0f;
        }
        for (int i = 0; i < (int)s->con_used; i++) {
            s->con_attr[i] = 0; s->conspd[i] = 0.0f;
            for (int c = 0; c < arc_n; c++) s->con_arc[i][c] = 0.0f;
        }
    }
    for (int i = 0; i < (int)s->e_used; i++) s->espeed_rand[i] = 0.0f;
    /* enemy lifecycle movement/source state (brotato_enemy_motion.h /
     * brotato_enemy_lifecycle.h) */
    for (int i = 0; i < (int)s->e_used; i++) {
        s->mv_lung[i] = (unsigned short)BT_POOL_NO_INST;
        s->mv_angry[i] = 0;
        s->e_source[i] = -1;
        s->emob_count[i] = 0;
        s->emob_evo[i] = 0;
        s->eboost_hp[i] = 0.0f; s->eboost_dmg[i] = 0.0f; s->eboost_spd[i] = 0.0f;
    }
    for (int i = 0; i < (int)s->e_used; i++) s->lk_q[i] = 0;
    s->n_lk_q = 0;
    /* main.gd:28/737/797 _active_golds ordering + RunData.bonus_gold */
    for (int i = 0; i < (int)s->mat_used; i++) {
        s->mat_seq[i] = 0u;
        s->mat_mob[i] = -1;
    }
    s->mat_seq_next = 0u;
    s->bonus_gold = 0;
    /* charm / Healer-Buffer BoostZone runtime + source-node identity */
    for (int i = 0; i < (int)s->e_used; i++) {
        s->e_charmed[i] = 0; s->e_charm_shield[i] = 0;
        s->e_charm_by[i] = -1; s->e_charm_timer[i] = 0.0f;
        s->e_object_uid[i] = 0u;
        for (int by = 0; by < BT_HEAL_BYTES; by++) s->heal_in[i][by] = 0;
    }
    s->pboost_active = 0; s->pboost_timer = 0.0f;
    s->pboost_hp = 0.0f; s->pboost_speed = 0.0f; s->pboost_aspeed = 0.0f;
    for (int i = 0; i < BT_MAX_PET; i++) {
        s->pstboost_active[i] = 0; s->pstboost_timer[i] = 0.0f;
        s->pstboost_dmg[i] = 0.0f; s->pstboost_rng[i] = 0.0f; s->pstboost_cd[i] = 0.0f;
    }
    for (int i = 0; i < 4; i++) s->charm_count[i] = 0;
    for (int i = 0; i < BT_POOL_ID_CAP; i++) s->pool_object_uid[i] = 0u;
    s->object_uid_next = 0u;
    s->object_uid_overflow = 0;
    for (int nk = 0; nk < BT_NNEUTRALS; nk++) {
        s->neutral_uid_n[nk] = 0;
        for (int i = 0; i < BT_MAX; i++) s->neutral_uid_pool[nk][i] = 0u;
    }
    for (int r = 0; r < BT_POOL_DEATH_FRAMES; r++) s->ndly_n[r] = 0;
    /* item explosions + the landmine trigger + the carried-explosion rows
     * (brotato_item_explosions.h) */
    bt_item_expl_reset(s);
    bt_zero_u8(s->pet_lm_pressed, BT_MAX_PET);
    for (int i = 0; i < (int)s->pr_used; i++) s->pr_expl[i] = BT_ITEM_EFF_NONE;
    /* the pending item FIFO is a run-scoped queue (main.gd's consumed-item
     * bookkeeping): reset here, never at a wave boundary */
    for (int i = 0; i < BT_MAX_PENDING; i++) s->pending_item_kind[i] = 0;
    /* ShopCompletion G6: the next-wave elite purchase (candy bag) is armed by a
     * shop action and consumed by the next bt_spawn_wave_extras, so a fresh run
     * starts with none armed.  Set BEFORE bt_enter_wave(1) below. */
    s->pending_extra_elite = -1;
    s->items_spawned_this_wave = 0;
    /* scalar-event player state (brotato_scalar_events.h) */
    s->n_death_events = 0;
    for (int i = 0; i < (int)s->mat_used; i++) s->mat_home[i] = 0;
    for (int i = 0; i < (int)s->pr_used; i++) s->prsrc[i] = 0xFFFFu;
    s->zone_hp_regen_bonus = 0.0f; s->zone_double_lifesteal = 0;
    s->zone_hp_regen_threshold = 100;
    bt_zero_u8(s->pet_alive, BT_MAX_PET);
    for (int i = 0; i < (int)s->e_used; i++) { s->eburn[i] = 0.0f; s->eburndmg[i] = 0.0f; }
    s->proc_chance = 0.0f; s->proc_dmg = 0.0f;
    for (int i = 0; i < 4; i++) {
        s->shop_kind[i] = BT_SHOP_NONE; s->shop_id[i] = 0;
        s->shop_price_wave[i] = 0;   /* 0 = unstamped (shop_item.gd:23 default is 1) */
        s->lvl_offers[i] = 0;
    }
    s->shop_bought = 0; s->shop_timer = 0; s->reroll_count = 0;
    s->shop_go_pending = 0; s->shop_go_wave_reset = 0;
    s->paid_rerolls = 0; s->free_rerolls_remaining = 0;
    s->initial_free_rerolls = 0; s->has_bonus_free_reroll = 0;
    for (int i = 0; i < 3; i++) { s->shop_copy_head[i] = 0; s->shop_copy_count[i] = 0; }
    s->pending_items = 0;
    s->pending_upgrades = 0;
    s->shop_available = 0;
    s->xp = 0.0f; s->level = 0;
    s->xp_need = bt_xp_needed_pct(s->character, 0);   /* (3+0+1)^2 * (1+pct/100) */
    s->end_wave_active = 0;
    s->end_wave_timer = 0; s->harvest_timer = 0;
    s->label_timer = 0; s->label_chars = 0;
    for (int i = 0; i < (int)s->con_used; i++) s->con_seq[i] = 0u;
    s->con_seq_next = 0u;
    s->pr_used = s->con_used = s->mat_used = 0;
    /* pinned canonical camera configuration for the gold-bag flight (the host
     * may override it with bt_loot_set_bag_view to match the bridge's meta) */
    s->bag_view_w = BT_LOOT_BAG_VIEW_W; s->bag_view_h = BT_LOOT_BAG_VIEW_H;
    s->bag_zoom = BT_LOOT_BAG_ZOOM;
    s->bag_hud_x = BT_LOOT_BAG_HUD_X; s->bag_hud_y = BT_LOOT_BAG_HUD_Y;

    bt_phys_world_init(&s->phys, BT_DT, BT_PHYS_SOLVER_ITERATIONS);
    bt_phys_walls(s);
    /* Default background=0 selects a background in RunData.reset_background
     * before the default-difficulty boss is chosen (run_data.gd:477,1388). */
    (void)bt_rng_next(&s->rng);
    /* Title-screen RunData.reset first selects one default-difficulty boss;
     * difficulty selection then replaces that unused choice (rl_run.gd). */
    (void)bt_rng_bounded(&s->rng, (unsigned int)BT_ZONE_NBOSSES[s->zone]);
    bt_schedule_elites(s);
    s->oe_n = 0; s->oe_overflow = 0;
    s->oe_last_base = 0;   /* bases are >= 20, so 0 never matches a real base */
    s->endless_max_wave = 0;
    bt_schedule_bosses(s);
    /* RunData.reset() enters the configured starting wave (run_data.gd:549);
     * campaign boots have start_wave == 1, so this is the historical call. */
    bt_enter_wave(s, s->start_wave > 0 ? (int)s->start_wave : 1);
    /* The recorded row 0 is written at the top of the game's first bridge frame,
     * i.e. before that frame's arena `_physics_process` walk but AFTER the
     * previously created Main scene's own IDLE phase: Godot's Timer defaults to
     * TIMER_PROCESS_IDLE, so the freshly started WaveTimer has already ticked
     * once (measured: the v5 s1 row-0 clock is duration - 1/60 and every later
     * row stays one frame ahead of an un-ticked clock).  The run prologue
     * consumes that same frame here, so s->time_left tracks the game's timer 1:1
     * from row 0 on and no observation compensates.  Nothing arena-side has run
     * at row 0 -- in particular the starting Weapon nodes are only created (by
     * bt_character_starting_apply, inside bt_enter_wave above), so their fresh
     * get_next_cooldown(true) draw is still whole (v5 s1 row 0 reads 1.0 and row
     * 1 the first decrement); the first bt_core_step is the wave's first arena
     * frame.  This is the same one-frame split every later wave gets from its GO
     * transition (bt_core_step's shop_go_pending exit). */
    s->time_left -= BT_DT;
    if (s->time_left < 0.0f) s->time_left = 0.0f;
    /* The fresh Main scene's player runs update_player_stats(true): the wave's
     * first LiveStats snapshot equals the stat layer (after every wave-entry
     * stat add above) at full health. */
    bt_player_stats_reset(s);
}

BT_HD void bt_core_add_log(BT_DEVICE BtState* s) {
    /* Primary objective: the wave REACHED.  The game records a run as
     * ProgressData's DifficultyScore (difficulty + wave): run_data.gd:1819
     * max_difficulty_beaten.set_info(current_difficulty, current_wave, ...) and,
     * past wave 20, main.gd:944 max_endless_wave_beaten.set_info(...,
     * RunData.current_wave, ...).  Log.perf therefore reports s->wave itself
     * (1..20 in the campaign, >20 in endless) so the sweep/eval metric IS the
     * game's record, instead of a waves/20 fraction that saturates at 1.0 and
     * cannot rank two endless runs.  score stays the kill count (the run
     * summary stat, not the record). */
    s->log.perf += (float)s->wave;
    if ((float)s->endless_max_wave > s->log.endless_wave)
        s->log.endless_wave = (float)s->endless_max_wave;
    s->log.score += (float)s->kills;
    s->log.episode_return += s->episode_return;
    s->log.episode_length += (float)s->ticks;
    s->log.n += 1.0f;
}
/* spawner.gd:9-18: a spawner's death emits its child spawns (forward decl). */
BT_HD static void bt_spawn_death_children(BT_DEVICE BtState* s, float x, float y, int kind, int src,
                                         int charmed_by);
/* Drop a gold material entity at (x,y) (items/materials/gold.gd, dropped by
 * main.gd spawn_gold from a kill's stats.value). */
BT_HD static void bt_spawn_material(BT_DEVICE BtState* s, float x, float y, int value) {
    if (value <= 0) return;
    int slot = -1;
    for (int i = 0; i < BT_MAX_MAT; i++) if (!s->matalive[i]) { slot = i; break; }
    if (slot < 0) return;
    /* main.gd:720-731 spawn_gold: a pooled coin is Gold.INITIAL_VALUE (1); while
     * RunData.bonus_gold > 0 it absorbs min(value, bonus_gold) and the ORIGINAL
     * value is removed from bonus_gold (run_data.gd:697-706 clamps to >= 0). */
    if (s->bonus_gold > 0) {
        int orig = value;
        int add = orig < s->bonus_gold ? orig : s->bonus_gold;
        s->bonus_gold -= orig;
        if (s->bonus_gold < 0) s->bonus_gold = 0;
        value = orig + add;
    }
    s->matx[slot] = x; s->maty[slot] = y;
    s->matval[slot] = (unsigned short)(value > 65535 ? 65535 : value);
    s->matspd[slot] = BT_ATTRACT_SPEED;
    s->mat_by[slot] = 0;                 /* item.gd: attracted_by = null */
    s->mat_attr[slot] = 0;               /* ... and no attractor yet        */
    s->mat_mob[slot] = -1;               /* not owned by an evil_mob yet */
    /* main.gd:737 push_back ordering for _active_golds */
    s->mat_seq[slot] = s->mat_seq_next++;
    s->mat_home[slot] = (unsigned char)bt_scalar_gold_spawn_attract(s);
    if (s->mat_home[slot]) {
        /* main.gd:740-746: the instant_gold_attracting roll, when the player
         * owns `stat_has_lootworm` AND a Lootworm node exists, hands the gold to
         * the LOOTWORM (attracted_by = lootworms[player_index]), not the player.
         * The gold is then pet-owned exactly like a claimed one: mat_by holds
         * slot+1 and mat_attr must be 2 so bt_pet_gold steps/picks it and the
         * player's own magnet pass keeps skipping it (mat_by != 0). */
        for (int p = s->pet_n - 1; p >= 0; p--) {
            if (!s->pet_alive[p] || s->pet_opcode[p] != BT_ITEMEFF_LOOTWORM) continue;
            s->mat_by[slot] = (unsigned char)(p + 1);
            s->mat_attr[slot] = 2;
            s->mat_home[slot] = 0;
            break;
        }
    }
    s->matalive[slot] = 1;
    if (slot >= (int)s->mat_used) s->mat_used = (unsigned short)(slot + 1);
    s->matcount++;
}

/* The (k+1)-th live gold in the source's insertion order.  main.gd:737
 * push_back / :797 erase keeps `_active_golds` insertion-ordered, and
 * bt_spawn_material records each gold's spawn sequence, so the k-th smallest
 * mat_seq is the source list's k-th entry.  The (seq, slot) key is total, so a
 * fixture that never wrote a sequence still gets a deterministic order.
 * Precondition: k < the number of live golds.  Shared by the player's
 * MAX_GOLDS boost pick (bt_loot_spawn_gold) and target_rand_gold_pos. */
BT_HD static int bt_loot_gold_ordered_slot(BT_DEVICE BtState* s, int k) {
    unsigned int last_seq = 0u;
    int last_slot = -1;
    int found = 0;
    for (int n = 0; n <= k; n++) {
        unsigned int best_seq = 0xFFFFFFFFu;
        int best_slot = -1;
        for (int m = 0; m < (int)s->mat_used; m++) {
            if (!s->matalive[m]) continue;
            unsigned int seq = s->mat_seq[m];
            if (n > 0 && (seq < last_seq || (seq == last_seq && m <= last_slot))) continue;
            if (seq < best_seq || (seq == best_seq && (best_slot < 0 || m < best_slot))) {
                best_seq = seq; best_slot = m;
            }
        }
        if (best_slot < 0) break;
        last_seq = best_seq; last_slot = best_slot;
        found = best_slot;
    }
    return found;
}

/* unit.gd:67-71: a unit's BurningTimer starts at the 0.5 s wait_time of
 * unit.tscn, scaled once when the unit is spawned by the summed
 * burning_cooldown_increase/reduction player effects:
 *   wait_time = max(0.1, 0.5 * (1 + increase - reduction))
 * Both buckets are plain effects with no core stat id (eyes_surgery and
 * frozen_heart key them), so they live in scalar_effect under their EMITTED
 * keys. The result is quantised to whole 60 Hz frames (Godot's Timer fires on a
 * frame boundary; the fractional remainder is a sub-frame effect the fixed-step
 * core cannot express). The source freezes the value at unit spawn; the core
 * re-derives it when a burning starts because it keeps no per-enemy timer
 * field. */
BT_HD static float bt_burn_period_frames(BT_DEVICE BtState* s) {
    float red = 0.0f, inc = 0.0f;
    if ((int)BT_ISTR_BURNING_COOLDOWN_REDUCTION < BT_NITEM_STRS)
        red = s->scalar_effect[BT_ISTR_BURNING_COOLDOWN_REDUCTION];
    if ((int)BT_ISTR_BURNING_COOLDOWN_INCREASE < BT_NITEM_STRS)
        inc = s->scalar_effect[BT_ISTR_BURNING_COOLDOWN_INCREASE];
    float wt = 0.5f * (1.0f + (inc - red) / 100.0f);
    if (wt < 0.1f) wt = 0.1f;
    float f = bt_roundf(wt * 60.0f);
    return f < 1.0f ? 1.0f : f;
}

/* unit.gd:618-646 apply_burning: applying a burning while one is already active
 * does NOT restart the BurningTimer -- damage and duration keep the max of the
 * old and the new BurningData (chance/spread likewise). eburn therefore holds
 * the remaining FRAMES of the burning (duration ticks * the BurningTimer
 * period) and each timeout deals the damage once and decrements the duration by
 * one (unit.gd:661-707). `dur_ticks` is BurningData.duration. */
BT_HD static void bt_apply_enemy_burn(BT_DEVICE BtState* s, int j, float dmg, float dur_ticks) {
    float frames = dur_ticks * bt_burn_period_frames(s);
    if (s->eburn[j] > 0.0f) {
        if (dmg > s->eburndmg[j]) s->eburndmg[j] = dmg;
        if (frames > s->eburn[j]) s->eburn[j] = frames;
    } else {
        bt_combat_audio_enemy_burn_apply(s, (int)s->ekind[j], (int)s->eneutral[j]);
        s->eburn[j] = frames;
        s->eburndmg[j] = dmg;
    }
}

/* main.gd:484-491: when the PLAYER kills a Boss -- an elite counts, its scene
 * instances boss.tscn and lives in the same `bosses` list -- on the final wave
 * and at most one boss/elite is still alive, the run's ending is decided:
 *   is_endless_run -> ZoneService.get_additional_groups(int((wave/10.0)*3), 90)
 *                     is appended LAST in the wave's group list with
 *                     spawn_timing = int(elapsed + i) per copy, and
 *                     RunData.all_last_wave_bosses_killed = true;
 *   else           -> the wave timer restarts with wait_time 0.1 s, i.e. the
 *                     wave ends ~0.1 s later (the campaign win path).
 * get_additional_groups(nb, wave_duration = 60, bonus_enemies_per_group = 1):
 * with those defaults it is nb = int((20/10.0)*3) = 6 copies at duration 90 and
 * bonus 1 (NOT nb/5), each copy still drawing its unit kind / area / edge /
 * interval, and only THEN is spawn_timing overwritten.
 * `elapsed` is the same wave-clock bt_fire_groups uses
 * (wave_timer.wait_time - wave_timer.time_left, one frame ahead in this core). */
BT_HD static void bt_last_wave_boss_transition(BT_DEVICE BtState* s) {
    int alive = 0;
    for (int i = bt_enemy_next(s, 0, 1); i < (int)s->e_used; i = bt_enemy_next(s, i + 1, 1))
        if (s->ealive[i] && !s->eneutral[i] && s->eelite[i] != 0) alive++;
    if (alive > 1) return;                 /* get_nb_bosses_and_elites_alive() <= 1 */
    if (s->endless) {
        int nb = (int)(((float)s->wave / 10.0f) * 3.0f);   /* 6 at wave 20 */
        float elapsed = s->wave_duration - s->time_left;
        int before = s->n_live_groups;
        int n = before;
        bt_build_endless_groups(s, nb, 90, 0, 1.0f, &n);
        for (int g = before; g < n; g++) {
            s->lg_timing[g] = (unsigned short)(int)(elapsed + (float)(g - before));
            s->group_fired[g] = 0;
            s->group_repeating[g] = 0;
            s->group_interval[g] = 0.0f;
            s->group_next_elapsed[g] = 0.0f;
            s->live_mult[g] = 1;
        }
        s->n_live_groups = n;
        s->all_last_wave_bosses_killed = 1;
    } else {
        s->time_left = 0.1f;               /* _wave_timer.wait_time = 0.1; start() */
    }
}

/* Drop gold/consumable + credit materials/XP for one killed enemy. Gold and XP
 * are 1:1 at pickup (main.gd:844-854) and derive from stats.value with the
 * kill drop chance (main.gd:604-620). Consumable rolls use the per-kind
 * item_drop_chance damped by items spawned this wave. */
BT_HD static void bt_mark_enemy_dead(BT_DEVICE BtState* s, int j, int allow_loot) {
    if (!s->ealive[j]) return;
    int kind = s->ekind[j];
    BT_CONST_PTR(BtUnitKind) K = bt_kunit(s, j);
    float x = s->ex[j], y = s->ey[j];
    /* enemy.gd:25 `_idle_playback_speed = rand_range(1, 3)` is drawn ONCE per
     * NODE (the member initializer), so it lives in the slot's pool ledger row,
     * not in a per-life field.  Sample it here -- the only consumer is the
     * death clip's playback_speed, and bt_enemy_pool_release below retires
     * e_instance[j], after which the row is no longer resolvable. */
    {
        bt_u64 idle_bits;
        if (bt_enemy_init_find(s, j, BT_INITDEST_IDLE_PLAYBACK, BT_INIT_ROOT_STATE, &idle_bits))
            s->edidl[j] = bt_rng_f64_to_float_bits(idle_bits);
    }
    /* a neutral's node identity is retired only when its death animation ends */
    if (s->eneutral[j]) bt_neutral_uid_retire(s, j);
    /* enemy-specific death linkage (brotato_enemy_lifecycle.h): clear
     * source_spawner, release the gold an evil_mob owned, kill a stargazer's
     * linked lung, enrage the stargazers linked to a lung that dies not full.
     * Runs BEFORE the pool release so the dying unit's instance is resolvable. */
    bt_enemy_lifecycle_on_death(s, j);
    if (!s->eneutral[j]) {
        unsigned short instance = s->e_instance[j];
        if (instance != BT_POOL_NO_INST) s->pool_shots[instance] = s->eshots[j];
        bt_enemy_pool_release(s, instance);
        s->e_instance[j] = (unsigned short)BT_POOL_NO_INST;
    }
    bt_enemy_set_alive(s, j, 0);
    s->phys.bodies[j].active = 0;
    /* The node stays in the scene: entity.gd:88-100 plays the death animation
     * and free_entity()/add_node_to_pool only remove it BT_POOL_DEATH_FRAMES
     * frames later (one more for a death the engine registers from its IDLE
     * pass, BT_POOL_DEATH_IDLE_EXTRA -- see bt_enemy_clean_up_room), so the
     * observation still sees it.  edkx/edky is die()'s stored knockback
     * (unit.gd:348, seeded from this hit's raw amount floored
     * at MIN_DEATH_KNOCKBACK_AMOUNT) and edvx/edvy continues the velocity the
     * slot had cached for the step after the death (the live body's
     * phys.bodies[j].vel, which bt_enemy_dying_step stops replacing only once
     * the death knockback takes over). */
    if (s->edying[j] == 0) {
        bt_enemy_set_dying(s, j, (unsigned char)BT_POOL_DEATH_FRAMES);
        /* entity.gd:73 die() plays the `death` clip at playback_speed 1.  A hit
         * kill is registered from the deferred boundary at the END of the
         * physics pass, so the SAME frame's idle pass still advances the freshly
         * started clip once: the node's first observable position is 1/60
         * (measured: a weapon kill's kill row reports current_animation_position
         * 0.016667).  bt_enemy_clean_up_room zeroes it for its idle-pass kills. */
        s->edpos[j] = BT_DT;
        s->edspd[j] = 1.0f;
        s->edkx[j] = s->edkbx[j];
        s->edky[j] = s->edkby[j];
        s->edvx[j] = s->phys.bodies[j].vel.x;
        s->edvy[j] = s->phys.bodies[j].vel.y;
    }
    s->epend_die[j] = 0;
    s->ecount--;
    s->kills++;
    if (!allow_loot) {
        if (!s->eneutral[j])
            bt_spawn_death_children(s, x, y, kind, j, bt_enemy_lifecycle_charmed_by(s, j));
        return;
    }
    /* main.gd:477-496 _on_enemy_died, in source order: the Boss branch of the
     * `enemy_killed_by_player` block runs BEFORE dmg_when_death / projectiles /
     * explode.  The spawner's `_on_boss_died` already erased the dying boss from
     * `bosses` (it connected first), and ealive[j] is already 0 here, so the
     * alive count below excludes it -- exactly the source's
     * get_nb_bosses_and_elites_alive(). */
    if (!s->eneutral[j] && bt_kind_class(kind) != 0 && s->wave == BT_WAVES)
        bt_last_wave_boss_transition(s);
    if (!s->eneutral[j]) {
        /* entity.die() emits "died" BEFORE the subclass extras, so main.gd's
         * handler -- explode_on_death at the DYING ENEMY's position
         * (main.gd:525-536 handle_explode_effect(explode_on_death,
         * enemy.global_position, ...)) -- runs before spawn_loot and before a
         * spawner's own child emits.  Only a player-owned row of that key
         * produces anything, and the call only queues a pool slot: the
         * explosion ticks later, so a chain of deaths never recurses.  A neutral
         * runs main.gd's tree branch instead. */
        (void)bt_item_expl_on_key(s, BT_ISTR_EXPLODE_ON_DEATH, x, y);
    }
    if (K->can_drop_consumables) {
        float item_chance = bt_loot_item_chance(s, K, s->items_spawned_this_wave, s->wave);
        int boss = !s->eneutral[j] && bt_kind_class(kind) == 2;
        int consumable = bt_loot_roll_consumable(s, K, item_chance, boss, s->wave);
        if (consumable >= 0) {
            if (bt_loot_counts_toward_wave(consumable)) s->items_spawned_this_wave++;
            int slot = -1;
            for (int c = 0; c < BT_MAX_CONS; c++) {
                if (!s->conalive[c]) { slot = c; break; }
            }
            if (slot < 0) { s->pend_overflow = 1; s->pend_drop_cons++; }
            else {
                s->conx[slot] = x; s->cony[slot] = y;
                s->conkind[slot] = (unsigned char)consumable;
                s->conalive[slot] = 1;
                if (slot >= (int)s->con_used) s->con_used = (unsigned short)(slot + 1);
                s->con_seq[slot] = s->con_seq_next++;
                /* consumable.gd is an Item: the same reset()+drop() state, the
                 * spawn draws of main.gd:631-645 (dist + push_back_destination,
                 * no rotation/sprite draw) and _current_speed = 500. */
                s->conspd[slot] = BT_ATTRACT_SPEED;
                s->con_attr[slot] = 0;                 /* attracted_by = null */
                bt_loot_consumable_arc(s, x, y, (int)K->gold_spread, s->con_arc[slot]);
            }
        }
    }
    if (bt_loot_gold_drop(s, K, s->eneutral[j], s->wave, s->is_horde_wave)) {
        float base_value = bt_enemy_death_gold_value(s, j, (float)K->value);
        float value = bt_loot_gold_value(s, s->eneutral[j], base_value);
        /* main.gd:777-780: the dying unit's effect-behavior gold modifier
         * (curse_enemy_effect_behavior.gd:20-21 -> +33%) multiplies the drop,
         * BEFORE gold_spread (main.gd:618). */
        value *= bt_enemy_curse_gold_value_mult(s, j);
        bt_loot_spawn_gold(s, value, x, y, bt_loot_gold_spread(K, value));
    }
    if (!s->eneutral[j]) {
        bt_spawn_death_children(s, x, y, kind, j, bt_enemy_lifecycle_charmed_by(s, j));
        if (s->n_death_events >= BT_MAX) { s->stat_reg_overflow = 1; return; }
        int event = s->n_death_events++;
        s->death_stack[event] = (unsigned short)j;
        s->death_phase[event] = 0;
    }
}

/* entity_spawner.gd:580-608 clean_up_room's kill pass, run from main.gd's
 * clean_up_room (_entity_spawner.clean_up_room()) BEFORE the next wave is built:
 * every live enemy/neutral dies with the spawner's `cleaning_up` die-args.  That
 * path (enemy.gd:224-229) runs the base Unit.die, disables the hitbox and then
 * RETURNS -- no reward, no kill credit, no loot -- and the spawner's own
 * _cleaning_up gates (entity_spawner.gd:118/148/259/322/434-449) discard any
 * child or queued spawn the deaths would have made.  The NODE itself stays in the
 * scene for the death animation (entity.gd's `death` key at 1.0 s), so
 * BtState.ecount -- the living count, which is what the trace's `enemies` field
 * carries -- snaps to 0 on this frame, while the observation still reports the
 * dying nodes (BtState.edying, obs[35..64] and obs[67]) for
 * BT_POOL_DEATH_FRAMES + BT_POOL_DEATH_IDLE_EXTRA frames.
 *
 * That extra frame is the whole reason this pass cannot reuse a hit kill's
 * edying: the game registers these deaths from the IDLE pass (the WaveTimer's
 * `timeout`, see BT_POOL_DEATH_IDLE_EXTRA in brotato_state.h), one idle frame
 * after the kill frame's own AnimationPlayer advance, while a weapon/contact hit
 * is registered at the end of the physics pass and therefore still catches it.
 * The game shows both windows exactly: a wave-end expiry keeps `enemy_nodes` /
 * obs[67] at the killed count for 61 rows (v4_demon_s1's 11 expired waves: kill
 * row 179 -> last node row 239, gone 240; 488 -> 549; 797 -> 858; 1106 -> 1167;
 * 1415 -> 1476; 1724 -> 1785; 2033 -> 2094; 2342 -> 2403; 2651 -> 2712; 2960 ->
 * 3021; 3269 -> 3330; the same 61 on probe_s1 and on deep-start rows 239/600/
 * 1680), while a weapon kill keeps its node 60 rows (norm_ref row 163 -> the
 * count drops on 223; match41 rows 321/427/544/665/784/871/1103 -> +60 each). */
BT_HD static void bt_enemy_clean_up_room(BT_DEVICE BtState* s) {
    int kills0 = s->kills;
    int pe = s->n_pend_enemy, pt = s->n_pend_tree;
    int pb = s->n_pend_boss, ps = s->n_pend_summons;
    for (int i = bt_enemy_next(s, 0, 1); i < (int)s->e_used; i = bt_enemy_next(s, i + 1, 1)) {
        if (!s->ealive[i]) continue;
        bt_mark_enemy_dead(s, i, 0);   /* allow_loot = 0 == args.cleaning_up */
        /* this death came from the idle pass: its node misses the kill frame's
         * AnimationPlayer advance and stays observable one frame longer. */
        if (s->edying[i] == (unsigned char)BT_POOL_DEATH_FRAMES) {
            bt_enemy_set_dying(s, i, (unsigned char)(BT_POOL_DEATH_FRAMES + BT_POOL_DEATH_IDLE_EXTRA));
            /* play("death") ran in the WaveTimer's `timeout`, i.e. AFTER this
             * frame's AnimationPlayer pass: the clip starts at 0, one advance
             * behind a hit kill's. */
            s->edpos[i] = 0.0f;
        }
    }
    /* cleaning_up credits no kill and lets no spawn out (the queue tails the
     * deaths appended are dropped; the next wave start clears them anyway). */
    s->kills = kills0;
    s->n_pend_enemy = pe; s->n_pend_tree = pt;
    s->n_pend_boss = pb; s->n_pend_summons = ps;
}

BT_HD static void bt_credit_kill(BT_DEVICE BtState* s, int j, int allow_loot) {
    bt_mark_enemy_dead(s, j, allow_loot);
    bt_drain_death_events(s);
}

/* unit.gd:344-356: a LETHAL hit does NOT run die() inline -- it call_deferred()s
 * it, and that deferred call is drained later in the SAME MessageQueue flush
 * (message_queue.cpp flush() swaps buffers at entry, so a call pushed while the
 * flush is running is picked up by the same flush).  Everything a hit does
 * (damage, the crit/burn rolls, hitbox.killed_something, heal/gold-on-kill)
 * therefore finishes for EVERY target of the flush before the first die() body
 * runs, and the die() bodies then run in the order the hits landed: loot drops,
 * gold, XP, the death-spawn children -- all of that frame's loot RNG.
 *
 * bt_queue_kill records the slot in that order; bt_drain_kill_q runs the kills
 * once, at the end of the frame's hit phases (the sim's equivalent of the
 * single flush).  epend_die is unit.gd:266-267: a lethal hit already queued
 * makes take_damage return [0, 0, false] for every later hit of the same
 * flush, and it also keeps a slot from being queued twice. */
BT_HD static void bt_queue_kill(BT_DEVICE BtState* s, int j) {
    if (j < 0 || j >= BT_MAX) return;
    if (s->epend_die[j] || !s->ealive[j]) return;
    s->epend_die[j] = 1;
    if (s->n_kill_q >= BT_MAX) { s->stat_reg_overflow = 1; return; }
    s->ekill_q[s->n_kill_q++] = (unsigned char)j;
}

/* bt_burning_kill_stats is defined with the kill-stat helpers far below; the
 * Timer rank of bt_drain_kill_q needs it early. */
BT_HD static void bt_burning_kill_stats(BT_DEVICE BtState* s);

/* unit.tscn's BurningTimer (unit.gd:661-707) fires in SceneTree::iteration's
 * "physics_process_internal" group, which runs BEFORE the "physics_process"
 * group and before that iteration's MessageQueue::flush (scene_tree.cpp:603-609).
 * Its take_damage therefore pushes the deferred die() into the READ buffer of
 * the coming flush, while the area hits' die()s are pushed INTO THE WRITE buffer
 * by the flush itself (each hurt_area_entered_deferred runs and queues its own
 * die).  The flush drains the read buffer first, so the order inside one frame is
 *   every area hit  ->  every burn die()  ->  every area die()
 * (unit.gd:344-356).  bt_queue_burn_kill records the first rank. */
BT_HD static void bt_queue_burn_kill(BT_DEVICE BtState* s, int j) {
    if (j < 0 || j >= BT_MAX) return;
    if (s->epend_die[j] || !s->ealive[j]) return;
    s->epend_die[j] = 1;
    if (s->n_kill_tq >= BT_MAX) { s->stat_reg_overflow = 1; return; }
    s->ekill_tq[s->n_kill_tq++] = (unsigned char)j;
}

/* Run every queued die() body: the Timer rank (burning) first, then the area
 * hits in hit order. */
BT_HD static void bt_drain_kill_q(BT_DEVICE BtState* s) {
    int tn = s->n_kill_tq;
    s->n_kill_tq = 0;
    for (int i = 0; i < tn; i++) {
        int j = (int)s->ekill_tq[i];
        bt_mark_enemy_dead(s, j, 1);
        if (!s->eneutral[j]) bt_burning_kill_stats(s);
        bt_drain_death_events(s);
    }
    int n = s->n_kill_q;
    s->n_kill_q = 0;
    for (int i = 0; i < n; i++) bt_credit_kill(s, (int)s->ekill_q[i], 1);
}

/* Bull explode_on_hit (player.gd:443-485): the CHARACTER's own
 * ItemExplodingEffect row (bull_effect_4.tres) -- fires on ANY damage taken, no
 * cooldown.  It now runs through the SAME deferred PlayerExplosion pool and
 * lifecycle as every item explosion (brotato_item_explosions.h
 * bt_item_expl_char_explode -> bt_item_expl_spawn_geom -> bt_item_expl_tick):
 * explosion.tscn's Hitbox CircleShape2D 147.34 @ (0,3) scaled by the node scale,
 * damage (30 + 3*stat) * the exploding multiplier, the source's two Utils._rng
 * offset draws, the node's own explosion_sounds once per freshly instantiated
 * node, and the crit rolled PER TARGET inside the tick through the one shared
 * enemy-explosion primitive.  No immediate per-target loop, no separate crit
 * policy and no count fold remain.
 *
 * Returns the pool slot (>= 0) or -1, like bt_item_expl_spawn. */
#if defined(__METAL_VERSION__)
__attribute__((noinline))
#endif
BT_HD static int bt_explode(BT_DEVICE BtState* s) {
    return bt_item_expl_char_explode(s);
}

/* item_service.gd:313-450: profile restrictions shared by shops and crates. */
BT_HD static int bt_item_shop_ok(unsigned short character, unsigned short it) {
    BT_CONST_PTR(BtItem) item = &BT_ITEMS[it];
    if (item->max_nb == 0) return 0;
    /* NOTE: template is_cursed is NOT a shop ban -- the DLC duplicates/curses
     * offers during generation (BT_DLC1_CURSE_*). Curse is tracked as a real
     * stat elsewhere; this filter only removes permanent pool members. */
    if (item->group_mask & bt_character_ban_group_mask(character)) return 0;
    if (bt_character_item_banned(character, it)) return 0;
    return 1;
}


/* Apply one item's stat effects to the player (shop buy / deferred item box). */
/* BT_ISTR_* interned stat key -> core stat id (0..18); 255 = unmapped. */
BT_HD static int bt_core_id_from_istr(unsigned short k) {
    switch (k) {
    case BT_ISTR_STAT_MAX_HP: return 0;
    case BT_ISTR_STAT_HP_REGENERATION: return 1;
    case BT_ISTR_STAT_ARMOR: return 2;
    case BT_ISTR_STAT_SPEED: return 3;
    case BT_ISTR_STAT_DODGE: return 4;
    case BT_ISTR_STAT_PERCENT_DAMAGE: return 5;
    case BT_ISTR_STAT_MELEE_DAMAGE: return 6;
    case BT_ISTR_STAT_RANGED_DAMAGE: return 7;
    case BT_ISTR_STAT_ELEMENTAL_DAMAGE: return 8;
    case BT_ISTR_STAT_CRIT_CHANCE: return 9;
    case BT_ISTR_STAT_LUCK: return 10;
    case BT_ISTR_STAT_HARVESTING: return 12;
    case BT_ISTR_STAT_CURSE: return 13;   /* DLC curse stat */
    /* Source upgrade-only stats. The shipped pool (UpgradePool measurement:
     * 16 stats x 4 tiers) covers ids 0..10, 12, 15, 16, 17, 18; accuracy (14)
     * and crit_damage (11) are NOT pooled, so id 14 currently has no producer
     * anywhere in the recovered sources (Keys.accuracy_hash == "accuracy" is a
     * weapon-effect key, and the "stat_accuracy" upgrade key is not interned
     * here because no item effect uses it). The interned "accuracy" key is still
     * mapped so item rows carrying it feed id 14.
     * NOTE: "stat_crit_damage" (-> core id 11) is likewise NOT interned by
     * extract_items.py -- the recovered items never key on it (only the
     * non-pooled crit_damage upgrade effect resources do), so no
     * BT_ISTR_STAT_CRIT_DAMAGE constant exists to add here. If a future
     * generation interns it, add `case ...: return 11;`. */
    case BT_ISTR_ACCURACY: return 14;               /* "accuracy" */
    case BT_ISTR_STAT_ATTACK_SPEED: return 15;
    case BT_ISTR_STAT_ENGINEERING: return 16;
    case BT_ISTR_STAT_LIFESTEAL: return 17;
    case BT_ISTR_STAT_RANGE: return 18;
    default: return 255;
    }
}

/* Core stat id 0..18 accessors (max_hp, regen, armor, speed, dodge, dmg_pct,
 * melee, ranged, elemental, crit, luck, crit_dmg, harvest, curse, accuracy,
 * attack_speed, engineering, lifesteal, range). Used by the generated stat
 * links and item effects.
 *
 * Source layering (run_data.gd:1305-1313, temp_stats.gd:37-42,
 * linked_stats.gd:12-78): Utils.get_stat = RunData.get_stat + TempStats.get_stat
 * + LinkedStats.get_stat, each of which is `raw * get_stat_gain`. The core
 * stores the three raw layers in stat_raw/temp_stat/link_stat and the gain
 * multiplier in stat_gain; bt_core_stat_read returns that UNGAINED-in-storage,
 * gained-on-read effective value (uncapped -- get_capped_stat is applied by
 * bt_core_stat_capped and the observable-field sync). */
BT_HD static float bt_core_stat_read(BT_DEVICE BtState* s, int id) {
    if (id < 0 || id >= BT_NSTATS) return 0.0f;
    return (s->stat_raw[id] + s->temp_stat[id] + s->link_stat[id]) * s->stat_gain[id];
}

/* utils.gd:469-484 get_capped_stat: only hp/speed/dodge/crit have a *_cap
 * effect (crit_chance_cap defaults to LARGE and no item keys it here). */
BT_HD static float bt_core_stat_capped(BT_DEVICE BtState* s, int id) {
    float v = bt_core_stat_read(s, id);
    switch (id) {
    case 0: case 3: case 4: case 9: {
        return bt_cap_apply(v, bt_stat_cap(id, s->stat_cap[id]));
    }
    default: return v;
    }
}

/* Derive every observable stat field from the raw/temp/link/gain layers. This
 * is player.gd:349-368 update_player_stats: max_hp = max(1, capped), speed uses
 * the capped speed stat, dodge the capped dodge /100, armor the plain stat; the
 * crit field carries the weapon's base crit_chance (bull_explosion_stats 3%)
 * plus the capped player stat, exactly as weapon_service.gd:253 does. Storage is
 * never clamped, so lowering a cap or expiring a temp restores the value. */
BT_HD static void bt_core_stat_sync(BT_DEVICE BtState* s) {
    float eff[BT_NSTATS];
    for (int i = 0; i < BT_NSTATS; i++)
        eff[i] = (s->stat_raw[i] + s->temp_stat[i] + s->link_stat[i]) * s->stat_gain[i];

    /* run_data.gd:301-302: get_player_max_health = max(1, get_capped_stat) as int
     * (truncated); the raw/gain layers stay float.  This is the IMMEDIATE
     * stat-layer value the observation reads (obs[1] max_hp/64, obs[0] the
     * denominator); the player's max_stats.health snapshot lives in p_max_hp and
     * is only moved by bt_player_stats_refresh (player.gd:349-368). */
    float m = bt_minf(eff[0], bt_stat_cap(0, s->stat_cap[0]));
    if (m < 1.0f) m = 1.0f;
    s->max_hp = (float)(int)m;

    float old_regen = s->regen;
    s->regen = eff[1];
    if (s->regen != old_regen) s->regen_timer = bt_regen_period(s->regen);
    /* player.gd:353: max_stats.armor = Utils.get_stat(armor) as int (truncated). */
    s->armor = (float)(int)eff[2];
    s->speed_pct = bt_core_stat_capped(s, 3);
    s->dodge = bt_core_stat_capped(s, 4) * 0.01f;
    s->dmg_pct = eff[5];
    s->melee = eff[6];
    s->ranged = eff[7];
    s->elemental = eff[8];
    s->dmg_stat = s->melee + s->ranged + s->elemental;
    s->crit = BT_BASE_CRIT + bt_core_stat_capped(s, 9) * 0.01f;
    /* crit_dmg is the weapon's crit_damage (bull_explosion_stats x1.5); the
     * player stat_crit_damage has no producer in the recovered sources (only the
     * unreachable crit_damage upgrade resources key it) and no reader. */
    s->crit_dmg = BT_BASE_CRIT_DMG;
    s->luck = eff[10];
    s->harvest = eff[12];
    s->curse = eff[13];
    s->accuracy = eff[14];
    s->attack_speed = eff[15];
    s->engineering = eff[16];
    s->lifesteal = eff[17];
    s->range_stat = eff[18];
    /* RunData.add_stat/remove_stat end with _are_player_stats_dirty[p] = true
     * (run_data.gd:1405/1414): every stat-layer change schedules the deferred
     * player stat refresh.  bt_player_stat_sync_step drains it. */
    s->stat_dirty = 1;
}

/* player.gd:349-368 update_player_stats(reset_current_health = false), the
 * deferred pass the StatsManager runs: the player's LiveStats are copied from
 * the CURRENT stat-layer values and, when max_stats.health grew, current health
 * follows by the increase (player.gd:361-363).  LiveStats.copy(max_stats,
 * set_health = false) does NOT clamp current health when the max shrinks
 * (unit/live_stats.gd:9-13), so a max decrease is left alone here too -- the
 * core's own `if (s->hp > s->p_max_hp)` in bt_player_stats_reset is only for the
 * fresh-player reset. */
BT_HD static void bt_player_stats_refresh(BT_DEVICE BtState* s) {
    float old_max = s->p_max_hp;
    s->p_max_hp = s->max_hp;
    s->p_speed_pct = s->speed_pct;
    s->p_armor = s->armor;
    s->p_dodge = s->dodge;
    if (s->p_max_hp > old_max) s->hp += s->p_max_hp - old_max;
    /* player.gd:368 check_hp_regen() closes update_player_stats; the regen gate
     * below reads p_max_hp/s->hp directly, so nothing is stored here. */
}

/* Fresh-player reset (player.gd:349-350 update_player_stats(true), run from
 * Player._ready and apply_items_effects at every wave's scene rebuild):
 * reset_current_health = true copies health from max_stats.health and applies no
 * heal, so the LiveStats snapshot equals the stat layer exactly. */
BT_HD static void bt_player_stats_reset(BT_DEVICE BtState* s) {
    s->p_max_hp = s->max_hp;
    s->p_speed_pct = s->speed_pct;
    s->p_armor = s->armor;
    s->p_dodge = s->dodge;
    s->hp = s->p_max_hp;
    s->stat_dirty = 0;
    s->stat_refresh_queued = 0;
}

/* One frame of the deferred player-stat pipeline.  Ordered exactly as the game
 * runs it, across two frame boundaries:
 *   1. run_data.gd:221-222 RunData._physics_process queues
 *      call_deferred("_emit_stats_updated") every physics frame; the deferred
 *      queue flushes at the END of the frame (main.cpp flushes the MessageQueue
 *      after the physics step), so the dirty flag any frame's change set before
 *      that flush is drained here, at the next frame boundary (this core's
 *      "top of frame", see bt_birth_flush).
 *   2. run_data.gd:1418-1427 _emit_stats_updated emits "stats_updated" for every
 *      dirty player and clears the flag; main.gd:339 on_stats_updated ->
 *      stats_manager.gd:105-126 reload_stats enqueues the player.
 *   3. stats_manager.gd:20-31 StatsManager._physics_process walks the queue on
 *      the NEXT physics frame and calls player.update_player_stats().
 * So a change made in frame T is visible in the player's LiveStats on frame T+2:
 * the drain lands at the top of T+1 and the refresh at the top of T+2, exactly
 * the two-row lag measured on v4_demon_s1/probe_s1/probe_s2 (demon_effect_1
 * conversion raises max HP at row 179, current health heals at row 181). */
BT_HD static void bt_player_stat_sync_step(BT_DEVICE BtState* s) {
    if (s->stat_refresh_queued) {
        bt_player_stats_refresh(s);
        s->stat_refresh_queued = 0;
    }
    if (s->stat_dirty) {
        s->stat_refresh_queued = 1;
        s->stat_dirty = 0;
    }
}

/* Permanent stat gain (RunData.add_stat / get_player_effect): item effects,
 * upgrades, end-of-wave and harvest bonuses. Storage stays raw: the observable
 * fields pick the new value up through bt_core_stat_sync. */
BT_HD static void bt_core_stat_add(BT_DEVICE BtState* s, int id, float v) {
    if (id < 0 || id >= BT_NSTATS) return;
    s->stat_raw[id] += v;
    s->wpn_dirty = 1;   /* a weapon-feeding stat changed: clamp cooldowns at bt_core_step */
    bt_core_stat_sync(s);
}

/* Temporary stat gain (TempStats.add_stat / remove_stat): TempStatsPerInterval,
 * DecayingStats (on hit / on consumable), PercentDamage stacks, "stats next
 * wave" and the pet boost zone. Held in a separate layer so it can expire and
 * so perm_stats_only links can ignore it. */
BT_HD static void bt_core_stat_add_temp(BT_DEVICE BtState* s, int id, float v) {
    if (id < 0 || id >= BT_NSTATS) return;
    s->temp_stat[id] += v;
    s->wpn_dirty = 1;   /* a weapon-feeding TEMP stat changed: clamp cooldowns too */
    bt_core_stat_sync(s);
}

/* TempStats.reset(): drop every temporary contribution at a wave boundary. */
BT_HD static void bt_core_stat_reset_temp(BT_DEVICE BtState* s) {
    for (int i = 0; i < BT_NSTATS; i++) s->temp_stat[i] = 0.0f;
    bt_core_stat_sync(s);
}

#include "brotato_character_runtime.h"

/* --- item-granted structures/pets: registration + instantiation ------------
 * global/entity_spawner.gd never instantiates a structure/pet when the item is
 * acquired: StructureEffect.apply / PetEffect.apply only append the effect
 * INSTANCE to Keys.structures_hash / Keys.stat_pets_hash, and
 * _on_StructureTimer_timeout (StructureTimer = main.tscn Timer, autostart,
 * default wait_time 1 s; the whole main.tscn -- spawner included -- is reloaded
 * at every wave, so the two _base_*_spawned flags restart false) then queues the
 * entities. This core keeps that split: bt_register_summon records the instance
 * (+ its curse-transformed fields) and bt_spawn_summon_row instantiates one
 * entity from an effect row, exactly as spawn_entity does for the scene. */

/* Effect classes whose `apply` targets Keys.stat_pets_hash rather than
 * Keys.structures_hash (PetEffect and every subclass). */
BT_HD static int bt_eff_is_pet_class(int opcode) {
    switch (opcode) {
    case BT_ITEMEFF_PET:
    case BT_ITEMEFF_LOOTWORM:
    case BT_ITEMEFF_DOC_MOTH:
    case BT_ITEMEFF_SCAPEGOAT:
    case BT_ITEMEFF_BONK_DOG:
    case BT_ITEMEFF_CATLING_GUN:
    case BT_ITEMEFF_RATZILLA:
    case BT_ITEMEFF_BOT_O_MINE:
    case BT_ITEMEFF_BLAZEMANDER:
        return 1;
    default:
        return 0;
    }
}

/* Register one owned structure/pet effect instance for item `it`. `mod` is the
 * per-effect curse modifier dlc_1_data.curse_item drew for this effect (0 = not
 * cursed). The spawnable scene must exist: PetEffect.apply() ignores an effect
 * whose scene is null (jellyshield_effect_1 is a pure stat item), and a
 * StructureEffect always ships a scene. */
BT_HD static void bt_register_summon_row(BT_DEVICE BtState* s, unsigned short row, float mod) {
    int cursed = (mod != 0.0f);
    /* one-shot loop so the row body's early `continue`s behave as before */
    for (int bt_once = 0; bt_once < 1; bt_once++) {
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[row];
        int op = (int)E->opcode;
        int is_pet;
        if (op == BT_ITEMEFF_STRUCTURE || op == BT_ITEMEFF_TURRET || op == BT_ITEMEFF_BUILDER_TURRET)
            is_pet = 0;
        else if (bt_eff_is_pet_class(op))
            is_pet = 1;
        else
            continue;
        if (E->scene == BT_ISTR_NONE) continue;   /* PetEffect.apply: scene == null */
        if (s->n_summon_fx >= BT_MAX_SUMMON_FX) { s->summon_overflow = 1; continue; }
        int fi = s->n_summon_fx++;
        s->summon_fx[fi].row = row;
        s->summon_fx[fi].opcode = (unsigned char)op;
        s->summon_fx[fi].is_pet = (unsigned char)is_pet;
        s->summon_fx[fi].is_turret =
            (unsigned char)(op == BT_ITEMEFF_TURRET || op == BT_ITEMEFF_BUILDER_TURRET);
        s->summon_fx[fi].cursed = (unsigned char)cursed;
        s->summon_fx[fi].copies = (unsigned char)(E->value > 0 ? (E->value > 255 ? 255 : E->value) : 0);
        s->summon_fx[fi].spawn_cd = 0;
        s->summon_fx[fi].mod = 0.0f;
        s->summon_fx[fi].boost_zone_scale = E->boost_zone_scale;
        s->summon_fx[fi].double_chance = E->double_chance;
        s->summon_fx[fi].revive_duration = E->revive_duration;
        s->summon_fx[fi].health_boost = E->health_boost;
        if (!is_pet) {
            /* dlc_1_data.gd:71-97 StructureEffect branch order. A wandering_bot
             * only gains the extra stat_speed (applied by the caller) and a
             * positive spawn_cooldown is cursed in place, so neither carries
             * `mod`; every remaining case (weapon stats, the nested
             * BurningEffect's scaling, the structure's own cooldown) hands the
             * modifier to bt_spawn_summon_row, which picks the branch from the
             * row's own scaling/burning fields exactly as the source does. */
            if (E->key == BT_ISTR_WANDERING_BOT) {
                s->summon_fx[fi].mod = 0.0f;
            } else if (E->spawn_cooldown > 0) {
                s->summon_fx[fi].spawn_cd = bt_apply_atk_spd_mod_cd(E->spawn_cooldown, cursed ? mod : 0.0f);
                s->summon_fx[fi].mod = 0.0f;
            } else {
                s->summon_fx[fi].mod = cursed ? mod : 0.0f;
            }
        } else {
            /* dlc_1_data.gd:99-134 PetEffect branch. */
            if (op == BT_ITEMEFF_DOC_MOTH) {
                s->summon_fx[fi].boost_zone_scale = E->boost_zone_scale * (1.0f + (cursed ? mod : 0.0f));
            } else if (op == BT_ITEMEFF_LOOTWORM) {
                s->summon_fx[fi].double_chance = E->double_chance * (1.0f + (cursed ? mod : 0.0f));
            } else if (op == BT_ITEMEFF_SCAPEGOAT) {
                s->summon_fx[fi].revive_duration = E->revive_duration / (1.0f + (cursed ? mod : 0.0f));
                s->summon_fx[fi].health_boost = E->health_boost + (cursed ? mod : 0.0f);
            } else if (op == BT_ITEMEFF_BOT_O_MINE) {
                /* weapon_stats damage boosted + the nested landmine effect's
                 * spawn_cooldown cursed (the pet's own landmine cadence). */
                s->summon_fx[fi].mod = cursed ? mod : 0.0f;
                if (E->child != BT_ITEM_EFF_NONE) {
                    BT_CONST_PTR(BtItemEff) C = &BT_ITEM_EFF_ROWS[E->child];
                    if (C->spawn_cooldown > 0)
                        s->summon_fx[fi].spawn_cd =
                            bt_apply_atk_spd_mod_cd(C->spawn_cooldown, cursed ? mod : 0.0f);
                }
            } else {
                /* BonkDog/CatlingGun/Ratzilla/Blazemander carry the weapon
                 * modifier; doc_moth/lootworm/scapegoat were handled above and
                 * deliberately get none (their curse only touched the fields). */
                if (op == BT_ITEMEFF_BONK_DOG || op == BT_ITEMEFF_CATLING_GUN ||
                    op == BT_ITEMEFF_RATZILLA || op == BT_ITEMEFF_BLAZEMANDER)
                    s->summon_fx[fi].mod = cursed ? mod : 0.0f;
            }
        }
    }

/* Fold an owned copy's summon-class effect rows into summon_fx[]: one per
 * ITEM copy (`RunData.add_item` -> apply_item_effects) and -- the same
 * aggregation in the source, which has no item/weapon distinction for the
 * player's effect bucket -- one per WEAPON copy carrying the row
 * (`run_data.gd:905-915 add_weapon` -> apply_item_effects ->
 * update_item_related_effects folds WeaponData.effects into the player's
 * effects too).  Shipped producer of the weapon half: weapon_pruner_1/2/3/4,
 * whose `effects = [ items/all/garden/garden_effect_1.tres ]` (a TurretEffect
 * spawning entities/structures/turret/garden/garden.tscn) is the garden that
 * the StructureTimer then spawns ~1 s into every wave -- v5_demon_s1's frame
 * 1914 module draws (entity_spawner.gd:537/544) come from exactly that, and the
 * core owned no garden because only items were scanned. */
}

BT_HD static void bt_register_summon(BT_DEVICE BtState* s, unsigned short it, float mod) {
    BT_CONST_PTR(BtItem) item = &BT_ITEMS[it];
    for (int e = 0; e < (int)item->eff_count; e++)
        bt_register_summon_row(s, (unsigned short)BT_ITEM_EFF_INDEX[item->eff_first + e], mod);
}

/* The weapon half of the same aggregation, one call per owned copy. */
BT_HD static void bt_weapon_register_summons(BT_DEVICE BtState* s, unsigned short wid, float mod) {
    if (wid >= BT_NWEAPONS) return;
    for (int e = 0; e < (int)BT_WEAPONS[wid].fx_count; e++) {
        int row = bt_weapon_effect_row(wid, (unsigned short)e);
        if (row == BT_WEAPON_NONE || row >= BT_NITEM_EFF_ROWS) continue;
        bt_register_summon_row(s, (unsigned short)row, mod);
    }
}

/* Child effect row of `E`'s effects[]/sub_effects[] array (sub_first is a slot in
 * BT_ITEM_EFF_INDEX). dlc_1_data.gd:92 tests `effect.effects[0] is BurningEffect`
 * for a cursed structure, so the source's effects[0] is read through this. */
BT_HD static BT_CONST_PTR(BtItemEff) bt_eff_sub(BT_CONST_PTR(BtItemEff) E, int i) {
    if (E->sub_count == BT_ITEM_EFF_NONE || i < 0 || i >= (int)E->sub_count) return 0;
    int idx = (int)E->sub_first + i;
    if (idx < 0 || idx >= BT_NITEM_EFF_INDEX) return 0;
    return &BT_ITEM_EFF_ROWS[BT_ITEM_EFF_INDEX[idx]];
}

/* Stats that have no core stat id live in scalar_effect under the effect key the
 * generator emits for them (see bt_apply_item's unmapped-key branch), so these
 * reads never hardcode an index and need no new state:
 *   structure_percent_damage  weapon_service.gd:241-243 (structures only)
 *   structure_range           weapon_service.gd:51,85 (structure range add)
 *   explosion_damage          weapon_service.gd:245-247,284-286 (+ bt_explode)
 *   structures_can_crit       weapon_service.gd:252 (get_player_effect_bool)
 * Every one of them is keyed by shipped items (dynamite/explosive_shells/honey,
 * pile_of_books) and stays 0 with none owned. */
BT_HD static float bt_structure_percent_damage(BT_DEVICE BtState* s) {
    return s->scalar_effect[BT_ISTR_STRUCTURE_PERCENT_DAMAGE];
}
BT_HD static float bt_structure_range(BT_DEVICE BtState* s) {
    return s->scalar_effect[BT_ISTR_STRUCTURE_RANGE];
}
BT_HD static float bt_explosion_damage(BT_DEVICE BtState* s) {
    return s->scalar_effect[BT_ISTR_EXPLOSION_DAMAGE];
}
BT_HD static int bt_structures_can_crit(BT_DEVICE BtState* s) {
    return s->scalar_effect[BT_ISTR_STRUCTURES_CAN_CRIT] > 0.0f;
}

/* init_base_stats builds new_stats from args.effects (weapon_service.gd:190-196):
 * an ExplodingEffect in that array sets is_exploding, and ItemExplodingEffect /
 * ItemExplodingWhenBelowHPEffect subclass it, so any of the three opcodes counts.
 * The rows that carry one are the rocket turret (:342), the landmines' and
 * BotOMine's nested exploding effects, and the Bull's own explode_on_hit stats
 * (player.gd:795 passes [ExplodingEffect.new()]). */
BT_HD static int bt_eff_is_exploding(BT_CONST_PTR(BtItemEff) E) {
    int n = (E->sub_count == BT_ITEM_EFF_NONE) ? 0 : (int)E->sub_count;
    for (int i = 0; i < n; i++) {
        BT_CONST_PTR(BtItemEff) C = bt_eff_sub(E, i);
        if (C == 0) break;
        if (C->opcode == BT_ITEMEFF_EXPLODING ||
            C->opcode == BT_ITEMEFF_ITEM_EXPLODING ||
            C->opcode == BT_ITEMEFF_ITEM_EXPLODING_WHEN_BELOW_HP) return 1;
    }
    return 0;
}

/* init_base_stats percent-damage bonus (weapon_service.gd:237-249):
 *   structure (StructureEffect is_pet false, TurretEffect, BuilderTurretEffect):
 *       1 + structure_percent_damage/100 in PLACE OF stat_percent_damage
 *   structure-pet (init_structure_pet_stats: StructureEffect.is_pet,
 *       BotOMineEffect and its landmine):
 *       1 + (stat_percent_damage + structure_percent_damage)/100
 *   every other pet and the ProjectileEffect: 1 + stat_percent_damage/100 */
BT_HD static float bt_summon_percent_bonus(BT_DEVICE BtState* s, int is_struct, int is_struct_pet) {
    if (is_struct && !is_struct_pet) return 1.0f + bt_structure_percent_damage(s) * 0.01f;
    float sp = is_struct ? bt_structure_percent_damage(s) : 0.0f;
    /* Utils.get_stat(stat_percent_damage): the layered raw+temp+link+gain read */
    return 1.0f + (bt_core_stat_read(s, 5) + sp) * 0.01f;
}

/* dlc_1_data.gd:71-97 StructureEffect curse branch, in source order:
 *   0 effect.key == wandering_bot   (+stat_speed only; no weapon/burning change)
 *   1 effect.spawn_cooldown > 0     (spawn cadence; applied by the caller)
 *   2 not effect.stats.scaling_stats.empty()  (_boost_weapon_stats_damage)
 *   3 effect.effects[0] is BurningEffect       (boost that burning's scaling)
 *   4 else                                     (the structure's own cooldown)
 * The row alone decides, so registration and instantiation share one rule. */
BT_HD static int bt_struct_curse_branch(BT_CONST_PTR(BtItemEff) E, BT_CONST_PTR(BtItemWeaponStats) W) {
    if (E->key == BT_ISTR_WANDERING_BOT) return 0;
    if (E->spawn_cooldown > 0) return 1;
    if (W != 0 && W->scaling_count > 0) return 2;
    BT_CONST_PTR(BtItemEff) C = bt_eff_sub(E, 0);
    if (C != 0 && C->opcode == BT_ITEMEFF_BURNING) return 3;
    return 4;
}

/* The BurningData a summoned entity's shots carry (weapon_service.gd:296-333
 * init_burning_data). The base is the row's own `burning` (BlazemanderEffect) or
 * the row's nested BurningEffect (turret_flame); the player's global burn bucket
 * contributes its RAW damage (global_burning.damage, :320), the entity's own
 * scaling stats scale the sum (their ratios are * (1+mod) under curse,
 * dlc_1_data.gd:90-93) and the init-time percent bonus is applied
 * (apply_structure_damage_bonus :330 for a structure, apply_damage_bonus :332
 * otherwise). Returns 0 when the entity carries no burning; chance and duration
 * come straight from the BT_ITEM_BURNING row. */
BT_HD static int bt_summon_burning(BT_DEVICE BtState* s, BT_CONST_PTR(BtItemEff) E,
                                   int is_struct, int is_struct_pet, float mod,
                                   BT_THREAD float* chance, BT_THREAD float* dmg,
                                   BT_THREAD float* dur) {
    int bi = BT_ITEM_EFF_NONE;
    if (E->burning != BT_ITEM_EFF_NONE) {
        bi = (int)E->burning;
    } else {
        BT_CONST_PTR(BtItemEff) C = bt_eff_sub(E, 0);
        if (C != 0 && C->opcode == BT_ITEMEFF_BURNING) bi = (int)C->burning;
    }
    if (bi == BT_ITEM_EFF_NONE || bi >= BT_NITEM_BURNING) return 0;
    BT_CONST_PTR(BtItemBurning) B = &BT_ITEM_BURNING[bi];
    /* global_burning.damage: the merge-additive raw damage of every owned burn
     * row, kept under the bucket's own emitted key (BT_ISTR_BURN_CHANCE). */
    float raw = (float)B->damage;
    if ((int)BT_ISTR_BURN_CHANCE < BT_NITEM_STRS) raw += s->scalar_effect[BT_ISTR_BURN_CHANCE];
    float v = (float)bt_scaling_damage(s, (int)raw, B->scaling_first, B->scaling_count, mod);
    v = bt_roundf(v * bt_summon_percent_bonus(s, is_struct, is_struct_pet));
    if (v < 1.0f) v = 1.0f;
    *chance = B->chance;
    *dmg = v;
    *dur = (float)B->duration;
    return 1;
}

/* Instantiate ONE structure/pet entity from effect row `row`, at the already
 * resolved spawn position. Mirrors WeaponService.init_structure_stats /
 * init_structure_pet_stats / init_melee|ranged_pet_stats + BT_ITEM_SCENES
 * geometry: the real WeaponStats (damage/cooldown/range/speed/count/spread),
 * the item scene's body radius and the projectile scene's hit radius.
 * `mod` is the instance's curse modifier; weapon_service.gd:221-226 zeroes the
 * attack-speed term for structures and pets, so apply_attack_speed_mod_to_cooldown
 * only enforces MIN_COOLDOWN. `is_struct` selects the structures_hash slot
 * (MAX_STRUCTURES accounting) vs the pets slot.
 * The source's own class choice and the dlc_1_data.gd:71-97 curse branch are
 * both derived from the row here: init_structure_stats / _structure_pet_stats for
 * a structures_hash effect (StructureEffect.is_pet) and for the BotOMine
 * effect + landmine, init_melee|ranged_pet_stats for the PetEffect subclasses
 * and init_ranged_stats for the ProjectileEffect; the branch is
 * bt_struct_curse_branch(row). */
#if defined(__METAL_VERSION__)
__attribute__((noinline))
#endif
BT_HD static int bt_spawn_summon_row(BT_DEVICE BtState* s, unsigned short row, float x, float y,
                                     int is_struct, float mod, float double_chance,
                                     float lm_cd) {
    BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[row];
    /* doc_moth/lootworm/scapegoat ship no WeaponStats row (their item effect
     * carries no `stats`): they are still real pets in the source, they just do
     * not shoot enemies, so they are created inert (no damage, no cooldown) and
     * only their own field-driven behaviour applies. */
    int has_w = (E->stats != BT_ITEM_EFF_NONE && E->stats < BT_NITEM_WSTATS);
    int op = (int)E->opcode;
    int pet_class = bt_eff_is_pet_class(op);
    /* structures_hash effect classes (StructureEffect/TurretEffect/
     * BuilderTurretEffect): the dlc_1_data.gd:71-97 StructureEffect curse branch
     * applies to these and only these. */
    int struct_hash = (!pet_class && (op == BT_ITEMEFF_STRUCTURE ||
                                      op == BT_ITEMEFF_TURRET ||
                                      op == BT_ITEMEFF_BUILDER_TURRET));
    /* is_structure for the source's init choice: every structures_hash effect
     * plus the BotOMineEffect (a PetEffect with is_structure, bot_o_mine_effect
     * .gd:12), and is_pet = StructureEffect.is_pet or the BotOMine effect. */
    int struct_like = struct_hash || op == BT_ITEMEFF_BOT_O_MINE;
    int struct_pet = struct_like && (pet_class || E->is_pet);
    BT_CONST_PTR(BtItemWeaponStats) W = has_w ? &BT_ITEM_WSTATS[E->stats] : 0;
    int branch = bt_struct_curse_branch(E, W);
    /* entity_spawner.gd:346-359: this scene's own pooled node (respawn path) or
     * a fresh scene.instance() (the class-member initializers and `_ready`). */
    int fresh = 1;
    int p = bt_struct_pool_acquire(s, E->scene, &fresh);
    if (p < 0) { s->summon_overflow = 1; return -1; }
    s->pet_alive[p] = 1;
    s->pet_isstruct[p] = (unsigned char)(is_struct ? 1 : 0);
    s->pet_lm_pressed[p] = 0;   /* landmine trigger: a fresh mine has met no body */
    s->pet_perm[p] = 0;
    s->pet_opcode[p] = (unsigned char)op;
    s->pet_row[p] = row;
    /* wandering_bot.gd:4-8 class-member initializers (`distance = rand_range
     * (100,200)`, `rotation_speed = rand_range(2,2.5)`, `_angle = rand_range
     * (0, 2*PI)`) run at scene.instance(), BEFORE entity.init(...) -- so these
     * three global draws precede every init draw, and a REUSED node (the pool's
     * respawn() branch) does NOT draw them at all: it still carries the values
     * its class-member initializers picked when it was first instantiated. */
    if (E->key == BT_ISTR_WANDERING_BOT && fresh) {
        s->pet_wb_dist[p] = bt_rand_range(s, 100.0f, 200.0f);
        s->pet_wb_rots[p] = bt_rand_range(s, 2.0f, 2.5f);
        s->pet_wb_angle[p] = bt_rand_range(s, 0.0f, 6.2831853f);
    }
    if (!has_w) {
        s->pet_dmg[p] = 0.0f;
        s->pet_range[p] = 0.0f;
        s->pet_cd[p] = 1.0e9f;
        s->pet_timer[p] = 1.0e9f;
        s->pet_pspeed[p] = 0.0f;
        s->pet_spread[p] = 0.0f;
        s->pet_nproj[p] = 0;
        s->pet_sap[p] = E->spawn_around_player;
        s->pet_sic[p] = E->spawn_in_center;
        s->pet_x[p] = x; s->pet_y[p] = y;
        s->pet_body_r[p] = (BT_ITEMS[E->item].scene_geom < BT_NITEM_SCENES) ?
                           BT_ITEM_SCENES[BT_ITEMS[E->item].scene_geom].body_radius : 0.0f;
        s->pet_proj_r[p] = BT_PROJ_R_DEFAULT;
        s->pet_double[p] = double_chance;
        s->pet_lm_cd[p] = lm_cd;
        s->pet_lm_timer[p] = lm_cd;
        /* an inert pet (no WeaponStats) can still carry burning_data; no shipped
         * row does, but the source's init_burning_data is unconditional */
        float bch0 = 0.0f, bdm0 = 0.0f, bdu0 = 0.0f;
        bt_summon_burning(s, E, struct_like, struct_pet, mod, &bch0, &bdm0, &bdu0);
        s->pet_burn_chance[p] = bch0;
        s->pet_burn_dmg[p] = bdm0;
        s->pet_burn_dur[p] = bdu0;
        s->pet_crit_chance[p] = 0.0f;
        s->pet_crit_dmg[p] = 0.0f;
        /* Pet.init/Unit.init run at instantiation (unit.gd:63-76, pet.gd:32-40):
         * the pet's movement-behaviour/health/patrol fields must exist before its
         * first _physics_process, exactly as for the shooting path below. */
        {
            int pai = bt_pet_ai_index(op);
            if (pai >= 0) { s->pet_fx[p] = bt_pet_pick_fx(s, p); bt_pet_slot_init(s, p, &BT_PET_AI[pai]); }
        }
        return p;
    }
    /* dlc_1_data.gd:71-97: only branch 2 hands the modifier to
     * _boost_weapon_stats_damage; branch 3 grows the nested BurningEffect's
     * scaling ratios and branch 4 shortens the structure's own cooldown. Pets
     * keep the PetEffect branch behaviour and the Blazemander only ever gets its
     * RANGED stats touched (:175-178). */
    float wmod = mod;
    if (mod != 0.0f) {
        if (struct_hash) {
            if (branch != 2) wmod = 0.0f;
        } else if (op == BT_ITEMEFF_BLAZEMANDER) {
            wmod = 0.0f;
        }
    }
    int base_dmg = (int)W->damage;
    int cd_ticks = (int)W->cooldown;
    float base_range = (float)(W->max_range > 0 ? W->max_range : 300);
    if (mod != 0.0f) {
        if (wmod != 0.0f) base_dmg = bt_curse_stat((float)base_dmg, wmod);  /* ceil(dmg*(1+mod)) */
        if (op == BT_ITEMEFF_BLAZEMANDER) {                   /* dlc_1_data.gd:175-178 */
            cd_ticks = cd_ticks / 2;
            base_range = base_range / (1.0f + mod);
        }
    }
    /* weapon_service.gd:237-249: apply_scaling_stats_to_damage, then the
     * init-time multiplier (set_bonus_dmg + percent_dmg_bonus +
     * exploding_dmg_bonus; no weapon sets exist here), computed once like
     * init_*_stats, never per shot. is_exploding is set by an ExplodingEffect in
     * the row's nested effects array (rocket turret, landmines/BotOMine bombs). */
    float dmgf = (float)bt_scaling_damage(s, base_dmg, W->scaling_first, W->scaling_count, wmod);
    float bonus = bt_summon_percent_bonus(s, struct_like, struct_pet);
    if (bt_eff_is_exploding(E)) bonus += bt_explosion_damage(s) * 0.01f;
    dmgf = bt_roundf(dmgf * bonus);
    if (dmgf < 1.0f) dmgf = 1.0f;
    s->pet_dmg[p] = dmgf;
    if (mod != 0.0f && struct_hash && branch == 4) {
        /* dlc_1_data.gd:94-96: cooldown = apply_attack_speed_mod_to_cooldown(
         * cooldown / TICKS_PER_SECOND, mod) * TICKS_PER_SECOND -- the whole-second
         * conversion happens BEFORE the modulo, then back to ticks. */
        cd_ticks = bt_apply_atk_spd_mod_cd(cd_ticks / 60, mod) * 60;
    }
    cd_ticks = bt_apply_atk_spd_mod_cd(cd_ticks, 0.0f);       /* MIN_COOLDOWN=2 */
    s->pet_cd[p] = (float)cd_ticks / 60.0f;
    /* the burning this entity's shots carry (weapon_service.gd:296-333) */
    {
        float bch = 0.0f, bdm = 0.0f, bdu = 0.0f;
        bt_summon_burning(s, E, struct_like, struct_pet, mod, &bch, &bdm, &bdu);
        s->pet_burn_chance[p] = bch;
        s->pet_burn_dmg[p] = bdm;
        s->pet_burn_dur[p] = bdu;
    }
    /* range addition per init_*_stats (weapon_service.gd:39,51,85): a structure
     * or structure-pet adds structure_range, a melee pet stat_range/2, a ranged
     * pet and the ProjectileEffect stat_range; every shipped stats row has
     * min_range 0, so the MIN_RANGE clamp is 25. */
    float r_add = struct_like ? bt_structure_range(s)
                              : bt_core_stat_read(s, 18) * (W->kind == 1 ? 0.5f : 1.0f);
    s->pet_range[p] = bt_maxf(25.0f, base_range + r_add);
    /* weapon_service.gd:251-253: the player's stat_crit_chance is added unless the
     * entity is a structure and structures_can_crit is not owned; the weapon's own
     * crit_chance/crit_damage always apply. unit.gd:285-300 rolls it per hit and
     * multiplies the value BEFORE armor. */
    float crit = W->crit_chance;
    if (!struct_like || bt_structures_can_crit(s))
        crit += bt_core_stat_capped(s, 9) * 0.01f;
    s->pet_crit_chance[p] = crit;
    s->pet_crit_dmg[p] = W->crit_damage;
    s->pet_pspeed[p] = (float)(W->projectile_speed > 0 ? W->projectile_speed : 500);
    s->pet_nproj[p] = (unsigned char)(W->nb_projectiles > 0 ? W->nb_projectiles : 1);
    s->pet_spread[p] = W->projectile_spread;
    /* Turret.gd: `var _cooldown: float = 0.0` -- a FRESH turret starts ready and
     * fires the moment a target is in range (the animation only delays the shot,
     * which this core does not model).  `_cooldown` is a class-member
     * initializer, so a REUSED node keeps whatever its previous life left there
     * (turret.gd:39 decrements it every frame it lives, so it is still <= 0 and
     * still ready -- the carry matters only for the value itself).  A non-turret
     * entity keeps a full first cooldown from the stats it just re-initialised
     * (Blazemander.gd:30 `_current_cooldown = _current_weapon_stats.cooldown`,
     * bot_o_mine.gd:37, catling_gun.gd:42-43, ratzilla.gd:23) on BOTH branches. */
    if (fresh || (op != BT_ITEMEFF_TURRET && op != BT_ITEMEFF_BUILDER_TURRET))
        s->pet_timer[p] = (op == BT_ITEMEFF_TURRET || op == BT_ITEMEFF_BUILDER_TURRET)
                          ? 0.0f : s->pet_cd[p];
    /* entity_spawner.gd:546-549: structures/pets are spawned at a random point
     * around the player (spawn_around_player) or the map centre (spawn_in_center),
     * else a random edge point; the position was resolved by the caller. */
    s->pet_sap[p] = E->spawn_around_player;
    s->pet_sic[p] = E->spawn_in_center;
    s->pet_x[p] = x; s->pet_y[p] = y;
    /* geometry from BT_ITEM_SCENES (body) + the wstats projectile scene */
    s->pet_body_r[p] = (BT_ITEMS[E->item].scene_geom < BT_NITEM_SCENES) ?
                       BT_ITEM_SCENES[BT_ITEMS[E->item].scene_geom].body_radius : 0.0f;
    s->pet_proj_r[p] = BT_PROJ_R_DEFAULT;
    for (int q = 0; q < BT_NITEM_SCENES; q++)
        if (BT_ITEM_SCENES[q].scene == W->projectile_scene && BT_ITEM_SCENES[q].hit_radius > 0.0f) {
            s->pet_proj_r[p] = BT_ITEM_SCENES[q].hit_radius;
            break;
        }
    /* LootwormEffect.double_chance in force + BotOMineEffect's landmine cadence
     * (initialised to the curse-transformed landmine spawn_cooldown, then
     * restarted to it on every landmine -- bot_o_mine.gd:53-54,107-113). */
    s->pet_double[p] = double_chance;
    s->pet_lm_cd[p] = lm_cd;
    s->pet_lm_timer[p] = lm_cd;
    /* Pet.init/Unit.init at instantiation (see the inert path above): resolve the
     * owned instance's curse-transformed fields BEFORE deriving health/patrol. */
    {
        int pai = bt_pet_ai_index(op);
        if (pai >= 0) { s->pet_fx[p] = bt_pet_pick_fx(s, p); bt_pet_slot_init(s, p, &BT_PET_AI[pai]); }
    }
    return p;
}

/* entity_spawner.gd:292-303: before a structure birth is created, random live
 * structures are removed while structures.size() > MAX_STRUCTURES.  The source
 * picks with Utils.get_rand_element, out of structures_to_remove_in_priority
 * whenever that array is non-empty (entity_spawner.gd:301-308) -- only
 * landmine.tscn sets StructureEffect.to_be_removed_in_priority -- else out of
 * `structures`.  A Landmine then dies at once while any other structure
 * deferred_dies; BOTH leave the `structures` array before the birth is created
 * (the deferred one emits `died` immediately, structure.gd:72-76), so it no
 * longer counts here, but its node stays alive 3.0 s and reaches the pool
 * 4.0 s after the eviction (bt_struct_pool_release's `deferred`). */
BT_HD static void bt_enforce_structure_cap(BT_DEVICE BtState* s) {
    int n = 0;
    for (int i = 0; i < s->pet_n; i++)
        if (s->pet_alive[i] && s->pet_isstruct[i] && !s->pet_pool_pending[i]) n++;
    while (n > BT_MAX_STRUCTS) {
        int prio = 0;
        for (int i = 0; i < s->pet_n; i++)
            if (s->pet_alive[i] && s->pet_isstruct[i] && !s->pet_pool_pending[i] &&
                bt_struct_pool_is_landmine(s, i)) prio++;
        int pick = (int)bt_rng_bounded(&s->rng, (unsigned int)(prio > 0 ? prio : n));
        int seen = 0, slot = -1;
        for (int i = 0; i < s->pet_n; i++) {
            if (!s->pet_alive[i] || !s->pet_isstruct[i] || s->pet_pool_pending[i]) continue;
            if (prio > 0 && !bt_struct_pool_is_landmine(s, i)) continue;
            if (seen++ == pick) { slot = i; break; }
        }
        if (slot < 0) break;
        int mine = bt_struct_pool_is_landmine(s, slot);
        bt_struct_pool_release(s, slot, mine ? 0 : 1);
        n--;
    }
}

/* EntityService.sort_turrets_by_strength ordering (entity_service.gd:83-91).
 * 255 = not considered a turret (garden / wandering_bot are excluded by
 * entity_service.gd:68-72), i.e. the effect stays in the non-turret group. */
BT_HD static int bt_turret_rank(unsigned short text_key) {
    switch (text_key) {
    case BT_ISTR_EFFECT_BUILDER_TURRET_ALT: return 0;
    case BT_ISTR_EFFECT_TURRET_ROCKET:      return 1;
    case BT_ISTR_EFFECT_TURRET_LASER:       return 2;
    case BT_ISTR_EFFECT_TYLER:              return 3;
    case BT_ISTR_EFFECT_TURRET_FLAME:       return 4;
    case BT_ISTR_EFFECT_TURRET:             return 5;
    case BT_ISTR_EFFECT_TURRET_HEALING:     return 6;
    default:                                return 255;
    }
}

/* entity_service.gd:68-72 is_considered_turret: a structures_hash effect is a
 * turret only when it is a TurretEffect AND its text_key is neither
 * effect_garden nor effect_wandering_bot. Those two are TurretEffect subclasses
 * but stay in the non-turret group in insertion order -- bt_turret_rank() is 255
 * for them (they are absent from the sort ordering), so the rank alone decides.
 * is_pet entries live in the pets hash, never the structures list. */
BT_HD static int bt_summon_considered_turret(BT_DEVICE BtState* s, int i) {
    if (!s->summon_fx[i].is_turret) return 0;
    return bt_turret_rank(BT_ITEM_EFF_ROWS[s->summon_fx[i].row].text_key) != 255;
}

/* Iteration order of Keys.structures_hash at StructureTimer time.
 * EntityService._restrict_turret_count (entity_spawner.gd:497-523) rebuilds the
 * list at every wave start as [non-turrets in insertion order] + [turrets sorted
 * by strength]. max_turret_count has no Bull producer (player_run_data.gd:605
 * defaults it to Utils.LARGE_NUMBER) so the slice drops nothing; only the order
 * changes. Returns the summon_fx[] index of the k-th structure effect, or -1. */
BT_HD static int bt_struct_slot(BT_DEVICE BtState* s, int k) {
    int seen = 0;
    for (int i = 0; i < s->n_summon_fx; i++) {
        if (s->summon_fx[i].is_pet || bt_summon_considered_turret(s, i)) continue;
        if (seen++ == k) return i;
    }
    for (int rank = 0; rank < 7; rank++)
        for (int i = 0; i < s->n_summon_fx; i++) {
            if (!bt_summon_considered_turret(s, i)) continue;
            if (bt_turret_rank(BT_ITEM_EFF_ROWS[s->summon_fx[i].row].text_key) != rank) continue;
            if (seen++ == k) return i;
        }
    return -1;
}

/* get_spawn_pos_in_area(base_pos, area) / ZoneService.get_rand_pos(edge) for one
 * queued structure or pet copy (entity_spawner.gd:536-563). group_structures_hash
 * is set only by the Engineer (engineer_effect_3), never for Bull, so the
 * group_structures branch -- and the base_pos/spawn_radius draws it alone needs --
 * is not taken. `base_x/base_y/spawn_radius` are the pet-branch position inputs. */
BT_HD static void bt_summon_pos(BT_DEVICE BtState* s, int fi, int is_pet,
                                float base_x, float base_y, float spawn_radius,
                                BT_THREAD float* ox, BT_THREAD float* oy) {
    BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[s->summon_fx[fi].row];
    if (is_pet) {
        bt_pos_in_area(s, base_x, base_y, spawn_radius, 0.0f, 0, ox, oy);
        return;
    }
    /* entity_spawner.gd:543-550: pos is drawn from get_rand_pos first
     * (group_structures is false for Bull/Demon), and a spawn_around_player /
     * spawn_in_center override then REPLACES it -- both rand_range pairs run. */
    bt_rand_pos(s, BT_EDGE_MAP_DIST * 2.5f, ox, oy);   /* ZoneService.get_rand_pos(160) */
    if (E->spawn_around_player != -1)
        bt_pos_in_area(s, s->px, s->py, (float)E->spawn_around_player, 0.0f, 0, ox, oy);
    else if (E->spawn_in_center != -1)
        bt_pos_in_area(s, BT_ARENA_W * 0.5f, BT_ARENA_H * 0.5f, (float)E->spawn_in_center, 0.0f, 0, ox, oy);
}

/* global/entity_spawner.gd:526-567 _on_StructureTimer_timeout. main.tscn's
 * StructureTimer is an autostart Timer with the default wait_time 1 s, so it
 * fires once per whole second of wave time; cur_time is
 * (wave_timer.wait_time - wave_timer.time_left) as int == tick_in_wave / 60 for
 * the exact 60 Hz tick. A brand-new main.tscn (and EntitySpawner) is instantiated
 * for every wave, so _base_structures_spawned / _base_pets_spawned start false
 * each wave and are set at the END of the wave's first timeout: every owned
 * structure (Effect.value copies) is created on that first tick, structures with
 * a positive spawn_cooldown again whenever cur_time % spawn_cooldown == 0, and
 * every owned pet exactly once on the first tick. */
BT_HD static void bt_structure_timer(BT_DEVICE BtState* s) {
    int cur_time = s->tick_in_wave / 60;
    int n_struct = 0;
    for (int i = 0; i < s->n_summon_fx; i++) if (!s->summon_fx[i].is_pet) n_struct++;
    float radius = 400.0f + (float)n_struct * 10.0f;
    if (radius > 600.0f) radius = 600.0f;
    int n_pet_fx = 0;
    for (int i = 0; i < s->n_summon_fx; i++)
        if (s->summon_fx[i].is_pet && s->summon_fx[i].copies > 0) { n_pet_fx = 1; break; }
    /* entity_spawner.gd:537: base_pos is drawn once per player per StructureTimer
     * tick whenever the player owns any structure effect, BEFORE the spawn gate,
     * so it runs even on ticks where nothing spawns.  group_structures is false
     * for Bull/Demon, so the drawn value is unused -- the two rand_range draws
     * still consume the stream in source order. */
    if (n_struct > 0) {
        float sbx, sby;
        bt_pos_in_area(s, BT_ARENA_W * 0.5f, BT_ARENA_H * 0.5f,
                       (float)((int)(BT_ARENA_W / 3.0f)), 0.0f, 0, &sbx, &sby);
    }
    for (int k = 0; ; k++) {
        int fi = bt_struct_slot(s, k);
        if (fi < 0) break;
        if (s->summon_fx[fi].copies == 0) continue;
        int spawn_cd = bt_apply_structure_attack_speed(s, s->summon_fx[fi].spawn_cd);
        int repeat = (spawn_cd > 0 && cur_time % spawn_cd == 0);
        if (!repeat && s->base_struct_spawned) continue;
        for (int c = 0; c < (int)s->summon_fx[fi].copies; c++) {
            float x, y;
            bt_summon_pos(s, fi, 0, 0.0f, 0.0f, radius, &x, &y);
            bt_queue_struct(s, fi, s->summon_fx[fi].row, x, y);
        }
    }
    /* entity_spawner.gd:558-560: the pet base_pos is drawn EVERY tick the player
     * owns any pet effect (not only on the first spawn tick); only the queueing
     * is gated on _base_pets_spawned. */
    if (n_pet_fx) {
        float pbx, pby;
        bt_pos_in_area(s, BT_ARENA_W * 0.5f, BT_ARENA_H * 0.5f,
                       (float)((int)(BT_ARENA_W / 3.0f)), 0.0f, 0, &pbx, &pby);
        if (!s->base_pet_spawned) {
            for (int i = 0; i < s->n_summon_fx; i++) {
                if (!s->summon_fx[i].is_pet || s->summon_fx[i].copies == 0) continue;
                for (int c = 0; c < (int)s->summon_fx[i].copies; c++) {
                    float x, y;
                    bt_summon_pos(s, i, 1, pbx, pby, radius, &x, &y);
                    bt_queue_pet(s, i, s->summon_fx[i].row, x, y);
                }
            }
        }
    }
    s->base_struct_spawned = 1;
    s->base_pet_spawned = 1;
}

/* Drop every dead pet/structure slot, preserving the live ones' order. */
BT_HD static void bt_pet_compact(BT_DEVICE BtState* s) {
    int w = 0;
    for (int i = 0; i < s->pet_n; i++) {
        if (!s->pet_alive[i]) continue;
        if (w != i) {
            s->pet_alive[w] = 1;
            s->pet_x[w] = s->pet_x[i]; s->pet_y[w] = s->pet_y[i];
            s->pet_dmg[w] = s->pet_dmg[i]; s->pet_range[w] = s->pet_range[i];
            s->pet_cd[w] = s->pet_cd[i]; s->pet_timer[w] = s->pet_timer[i];
            s->pet_pspeed[w] = s->pet_pspeed[i]; s->pet_spread[w] = s->pet_spread[i];
            s->pet_sap[w] = s->pet_sap[i]; s->pet_sic[w] = s->pet_sic[i];
            s->pet_nproj[w] = s->pet_nproj[i];
            s->pet_body_r[w] = s->pet_body_r[i]; s->pet_proj_r[w] = s->pet_proj_r[i];
            s->pet_isstruct[w] = s->pet_isstruct[i]; s->pet_perm[w] = s->pet_perm[i];
            s->pet_opcode[w] = s->pet_opcode[i]; s->pet_row[w] = s->pet_row[i];
            s->pet_double[w] = s->pet_double[i];
            s->pet_lm_cd[w] = s->pet_lm_cd[i]; s->pet_lm_timer[w] = s->pet_lm_timer[i];
            s->pet_burn_chance[w] = s->pet_burn_chance[i];
            s->pet_burn_dmg[w] = s->pet_burn_dmg[i];
            s->pet_burn_dur[w] = s->pet_burn_dur[i];
            s->pet_crit_chance[w] = s->pet_crit_chance[i];
            s->pet_crit_dmg[w] = s->pet_crit_dmg[i];
            s->pet_lm_pressed[w] = s->pet_lm_pressed[i];   /* landmine trigger state */
            /* the node pool's per-slot identity moves with the slot (the pool
             * itself is discarded right after, in bt_clear_wave_summons) */
            s->pet_pool_set[w] = s->pet_pool_set[i];
            s->pet_pool_scene[w] = s->pet_pool_scene[i];
            s->pet_pool_seq[w] = s->pet_pool_seq[i];
            s->pet_pool_avail[w] = s->pet_pool_avail[i];
            s->pet_pool_pending[w] = s->pet_pool_pending[i];
            s->pet_pool_die_at[w] = s->pet_pool_die_at[i];
            s->pet_alive[i] = 0;
        }
        w++;
    }
    s->pet_n = w;
}

/* entity_spawner.gd:580-608 clean_up_room: every structure/pet entity dies at
 * wave end (the next wave reloads main.tscn) and both queues are cleared. A
 * ProjectileEffect emitter is a PLAYER effect (Keys.projectiles_hash), not an
 * entity, so it has pet_perm set and survives. */
BT_HD static void bt_clear_wave_summons(BT_DEVICE BtState* s) {
    for (int i = 0; i < s->pet_n; i++)
        if (s->pet_alive[i] && !s->pet_perm[i]) s->pet_alive[i] = 0;
    bt_pet_compact(s);
    s->n_pend_struct = 0;
    s->n_pend_pet = 0;
    s->base_struct_spawned = 0;
    s->base_pet_spawned = 0;
    /* a fresh main.tscn per wave (see below) means a fresh main._explosions pool:
     * no explosion node of the old room survives, and every mine of the old room
     * is gone with its structure slot.  Dropped items DO persist (their arcs are
     * per slot and are re-armed on creation), so nothing else is cleared here. */
    bt_item_expl_reset(s);
    bt_wproj_reset(s);
    /* the structure/pet node pool is discarded with the old Main too
     * (main.gd:1727-1737 _exit_tree): every node of the old room is gone, so the
     * next wave's structures are instantiated fresh. */
    bt_struct_pool_reset(s);
    /* the uid space is per wave: every source node of the old room is gone, so
     * the counter restarts and the retained per-descriptor / per-neutral-kind
     * uids are dropped. */
    for (int tk = 0; tk < BT_POOL_ID_CAP; tk++) s->pool_object_uid[tk] = 0u;
    for (int tk = 0; tk < (int)s->e_used; tk++) s->e_object_uid[tk] = 0u;
    s->object_uid_next = 0u;
    s->object_uid_overflow = 0;
    for (int nk = 0; nk < BT_NNEUTRALS; nk++) s->neutral_uid_n[nk] = 0;
    for (int r = 0; r < BT_POOL_DEATH_FRAMES; r++) s->ndly_n[r] = 0;
    bt_zero_u8(s->pet_lm_pressed, BT_MAX_PET);
    bt_pet_world_init(s);
}

/* Merge one acquired item's BurningData into the player's global burning bucket.
 * `row` is the BurnChanceEffect/BurningEffect row carrying that BurningData and
 * `mod` is THAT effect's curse modifier (0 = not cursed; bt_apply_item draws
 * one per top-level effect, dlc_1_data.gd:82).
 * burn_chance_effect.gd:13 does effects[Keys.burn_chance_hash].merge(burning_data)
 * and BurningData.merge (burning_data.gd:20-24) ADDS chance and damage while
 * taking the MAX duration; merge() never rewrites scaling_stats, so the bucket
 * keeps BurningData.new()'s [["stat_elemental_damage", 1.0]] from
 * player_run_data.gd:515. init_burning_data's bucket path (weapon_service.gd:
 * 302-333) then puts the raw damage through apply_scaling_stats_to_damage and
 * applies the stat_percent_damage bonus (apply_damage_bonus). dlc_1_data.gd:
 * 196-208 ceil-boosts the cursed row's duration and damage before the merge.
 * The raw (pre-scaling) damage sum is kept in scalar_effect under the row's own
 * emitted key -- that key IS the bucket's Keys.burn_chance_hash -- so a
 * structure/pet burning can add global_burning.damage (weapon_service.gd:320)
 * without hardcoding an index. */
BT_HD static void bt_set_burn_from_item(BT_DEVICE BtState* s, unsigned short row, float mod) {
    BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[row];
    if (E->burning == BT_ITEM_EFF_NONE || E->burning >= BT_NITEM_BURNING) return;
    BT_CONST_PTR(BtItemBurning) B = &BT_ITEM_BURNING[E->burning];
    int dmg = (int)B->damage, dur = (int)B->duration;
    if (mod != 0.0f) {
        dmg = (int)bt_ceilf((float)dmg * (1.0f + mod));
        dur = (int)bt_ceilf((float)dur * (1.0f + mod));
    }
    int key = (E->key != BT_ISTR_NONE) ? (int)E->key : BT_ISTR_BURN_CHANCE;
    if (key >= BT_NITEM_STRS) return;                 /* never write out of bounds */
    s->scalar_effect[key] += (float)dmg;              /* merge: damage adds */
    s->burn_chance += B->chance;                      /* merge: chance adds */
    if ((float)dur > s->burn_dur) s->burn_dur = (float)dur;   /* merge: duration max */
    float scaled = s->scalar_effect[key] + bt_core_stat_read(s, 8);   /* bucket scaling: elemental 1:1 */
    if (scaled < 1.0f) scaled = 1.0f;
    float dmgf = bt_roundf(scaled * (1.0f + bt_core_stat_read(s, 5) * 0.01f));
    if (dmgf < 1.0f) dmgf = 1.0f;
    s->burn_dmg = dmgf;
}

/* ProjectileEffect (op25): an item-granted periodic projectile emitter. It uses
 * the same WeaponStats-driven firing as pets (damage/cooldown/range/speed/
 * spread/count -> real friendly projectiles). ProjectileEffect is a plain
 * Effect (Keys.projectiles_hash), NOT a structures_hash/stat_pets_hash entry, so
 * it is created immediately on acquisition and survives every wave (pet_perm);
 * `value` is the projectiles-per-emission count, so one emitter is created.
 * dlc_1_data.gd:210-211 boosts the projectile weapon stats damage on curse.
 * bt_spawn_summon_row already applies init_ranged_stats' terms (percent damage,
 * crit, max(MIN_RANGE, max_range + stat_range)); the remaining
 * _set_common_ranged_stats terms (projectile_spread + projectiles_effect*0.1,
 * nb_projectiles + projectiles_effect, piercing/bounce from the player's
 * piercing/bounce effects) have NO reachable producer in this sim -- the only
 * `projectiles` effect resource belongs to a character that is not in the pool,
 * and both shipped op25 rows (alien_eyes, baby_with_a_beard's
 * projectiles_on_death) are consumed by bt_scalar_bucket_register
 * (brotato_scalar_events.h) instead of here, where their shot count and aim come
 * from the effect value (player.gd:850-875 / main.gd:507-525). */
BT_HD static void bt_add_projectile_emitter(BT_DEVICE BtState* s, unsigned short it, float mod) {
    BT_CONST_PTR(BtItem) item = &BT_ITEMS[it];
    for (int e = 0; e < (int)item->eff_count; e++) {
        unsigned short row = (unsigned short)BT_ITEM_EFF_INDEX[item->eff_first + e];
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[row];
        if (E->opcode != BT_ITEMEFF_PROJECTILE) continue;
        if (E->stats == BT_ITEM_EFF_NONE || E->stats >= BT_NITEM_WSTATS) continue;
        float x, y;
        bt_rand_pos(s, BT_EDGE_MAP_DIST * 2.5f, &x, &y);
        int slot = bt_spawn_summon_row(s, row, x, y, 0, mod, 0.0f, 0.0f);
        if (slot >= 0) s->pet_perm[slot] = 1;
        return;
    }
}

/* linked_stats.gd:12-78: recompute every registered link from the CURRENT stats:
 * amount = int(value * (actual_stat / nb_stat_scaled)), signed, times the owned
 * multiplicity of that link row. The source is RunData.get_stat (permanent only)
 * plus TempStats.get_stat when the row's perm_stats_only is false -- both gained
 * by get_stat_gain and neither including the link layer itself. link_stat holds
 * the raw LinkedStats contribution; bt_core_stat_sync gains+caps it. */
BT_HD static void bt_recompute_links(BT_DEVICE BtState* s) {
    for (int id = 0; id < BT_NSTATS; id++) s->link_stat[id] = 0.0f;
    for (int r = 0; r < BT_NITEM_EFF_ROWS; r++) {
        if (s->lk_count[r] == 0) continue;
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[r];
        int t = bt_core_id_from_istr(E->key);
        int src = bt_core_id_from_istr(E->aux_key);
        if (t == 255 || E->nb_stat_scaled == 0) continue;
        if (src == 255) {
            /* linked_stats.gd:37-42: a GainStatForEveryStatEffect whose scaled
             * stat is the live burning-enemy COUNT instead of a stat
             * (fried_rice, aux_key BT_ISTR_BURNING_ENEMY); the bonus formula
             * stays in bt_wfx_burning_enemy_link_bonus.  Any other
             * BT_STAT_NONE source is not a link. */
            /* linked_stats.gd:29-32 + run_data.gd:1856-1857 get_scaling_bonus: the
             * materials_hash source is the player's collected gold, an int.  Its
             * only shipped producer is Padding (+1 max_hp per 80 materials); the
             * every-half-second refresh above lands on the same half-second
             * cadence linked_stats.gd:33 requests for this source. */
            if (E->aux_key == BT_ISTR_MATERIALS) {
                float gm = (float)(int)s->materials;
                if (s->nvar[r] == 0) {
                    s->link_stat[t] += bt_stat_link_gain((float)E->value, gm,
                                                         (float)E->nb_stat_scaled)
                                       * (float)s->lk_count[r];
                } else {
                    for (int i = 0; i < (int)s->nvar[r]; i++)
                        s->link_stat[t] += bt_stat_link_gain((float)bt_rvar(s,r)[i].value, gm,
                                                             (float)E->nb_stat_scaled)
                                           * (float)bt_rvar(s,r)[i].count;
                }
                continue;
            }
            if (E->aux_key != BT_ISTR_BURNING_ENEMY) continue;
            if (s->nvar[r] == 0) {
                s->link_stat[t] += bt_wfx_burning_enemy_link_bonus(s, (unsigned short)r)
                                   * (float)s->lk_count[r];
            } else {
                /* per owned copy: the curse-transformed tuple is used and the
                 * int() truncation happens per copy, exactly like the
                 * stat-source branch below. */
                float bn = (float)bt_wfx_burning_enemies(s);
                for (int i = 0; i < (int)s->nvar[r]; i++)
                    s->link_stat[t] += bt_stat_link_gain((float)bt_rvar(s,r)[i].value, bn,
                                                         (float)E->nb_stat_scaled)
                                       * (float)bt_rvar(s,r)[i].count;
            }
            continue;
        }
        float srcraw = s->stat_raw[src] + (E->perm_stats_only ? 0.0f : s->temp_stat[src]);
        float srcv = srcraw * s->stat_gain[src];
        if (s->nvar[r] == 0) {
            s->link_stat[t] += bt_stat_link_gain((float)E->value, srcv, (float)E->nb_stat_scaled)
                               * (float)s->lk_count[r];
        } else {
            /* per owned copy: the source recomputes each instance's link and the
             * int() truncation happens per copy, so copies cannot be summed first */
            for (int i = 0; i < (int)s->nvar[r]; i++)
                s->link_stat[t] += bt_stat_link_gain((float)bt_rvar(s,r)[i].value, srcv,
                                                     (float)E->nb_stat_scaled)
                                   * (float)bt_rvar(s,r)[i].count;
        }
    }
    bt_character_query_links(s);
    bt_core_stat_sync(s);
}

/* dlc_1_data.gd:353-357 _get_cursed_item_effect_modifier + :365-379
 * _boost_effect_value_positively. A fresh random modifier is drawn per effect
 * (and once more for a cursed WeaponData's own stats). */
/* items/global/effect.gd Sign enum (0..5) */
#define BT_SIGN_POSITIVE   0
#define BT_SIGN_NEGATIVE   1
#define BT_SIGN_NEUTRAL    2
#define BT_SIGN_FROM_VALUE 3
#define BT_SIGN_FROM_ARG   4
#define BT_SIGN_OVERRIDE   5

/* items/global/effect.gd:227-240 get_sign */
BT_HD static int bt_eff_sign(int from_sign, int from_value) {
    if (from_sign == BT_SIGN_FROM_VALUE || from_sign == BT_SIGN_FROM_ARG)
        return from_value > 0 ? BT_SIGN_POSITIVE : (from_value < 0 ? BT_SIGN_NEGATIVE : BT_SIGN_NEUTRAL);
    return from_sign;
}

BT_HD static float bt_curse_modifier(BT_DEVICE BtState* s) {
    /* dlc_1_data.gd:353-357 _get_cursed_item_effect_modifier:
     *   Utils.randi_range(-cursed_item_random_percent_modifier,
     *                      cursed_item_random_percent_modifier)   == (-30, 30)
     * Utils.randi_range goes through the Utils singleton RNG (_rng => utils_rng). */
    int r = bt_rng_randi_range(&s->utils_rng, -30, 30);
    float w = (float)(s->wave - 1);
    if (w > 20.0f) w = 20.0f;
    float p = BT_DLC1_CURSE_CURSED_ITEM_BASE_PERCENT_MODIFIER +
              BT_DLC1_CURSE_CURSED_ITEM_PERCENT_MODIFIER_INCREASE_EACH_WAVE * w + (float)r;
    if (p < 0.0f) p = 0.0f;
    return p / 100.0f;
}

/* dlc_1_data.gd:365-379 _boost_effect_value_positively: sign of `value` (or of
 * `override_sign` when the caller forces good/bad) decides whether the curse
 * magnifies (good: ceil(|v|*(1+mod))) or dampens (bad: max(1, floor(|v|/(1+mod))))
 * the magnitude; NEUTRAL is returned untouched. */
BT_HD static int bt_curse_value(int value, int eff_sign, float mod, int override_sign, int overridden_sign) {
    int sg = override_sign ? overridden_sign : bt_eff_sign(eff_sign, value);
    int is_good = (sg == BT_SIGN_POSITIVE || sg == BT_SIGN_OVERRIDE);
    int vsign = value > 0 ? 1 : (value < 0 ? -1 : 0);
    if (is_good) return vsign * (int)bt_ceilf(bt_fabsf((float)value) * (1.0f + mod));
    if (sg == BT_SIGN_NEUTRAL) return value;
    float d = bt_floorf(bt_fabsf((float)value) / (1.0f + mod));
    return vsign * (int)(d < 1.0f ? 1.0f : d);
}
/* float form (DoubleValueEffect.value2 / fractions); sign taken from `value`. */
BT_HD static float bt_curse_value_f(float value, int eff_sign, float mod, int override_sign, int overridden_sign) {
    int sg = override_sign ? overridden_sign : bt_eff_sign(eff_sign, (int)value);
    int is_good = (sg == BT_SIGN_POSITIVE || sg == BT_SIGN_OVERRIDE);
    int vsign = value > 0.0f ? 1 : (value < 0.0f ? -1 : 0);
    if (is_good) return (float)vsign * bt_ceilf(bt_fabsf(value) * (1.0f + mod));
    if (sg == BT_SIGN_NEUTRAL) return value;
    float d = bt_floorf(bt_fabsf(value) / (1.0f + mod));
    return (float)vsign * (d < 1.0f ? 1.0f : d);
}
/* dlc_1_data.gd:386-... _boost_weapon_stats_damage: damage, every scaling ratio
 * and (for ranged) piercing/bounce grow with the modifier; crit_damage/lifesteal
 * use the /5 and 1:1 variants. Only the fields this core consumes are returned;
 * `mod == 0` is the identity used for every non-cursed acquisition. */
BT_HD static int bt_curse_stat(float value, float mod) {
    if (mod == 0.0f) return (int)value;
    return (int)bt_ceilf(value * (1.0f + mod));
}
/* weapon_service.gd:568-571 apply_attack_speed_mod_to_cooldown (MIN_COOLDOWN=2) */
BT_HD static int bt_apply_atk_spd_mod_cd(int base_cd, float mod) {
    if (mod < 0.0f) { int v = (int)((float)base_cd * (1.0f + bt_fabsf(mod))); return v < 2 ? 2 : v; }
    int v = (int)((float)base_cd / (1.0f + mod));
    return v < 2 ? 2 : v;
}

/* weapon_service.gd:489-495 apply_scaling_stats_to_damage / sum_scaling_stat_values
 * then dlc_1_data.gd:381-386 _boost_scaling_sats (ratio * (1+mod)). */
BT_HD static int bt_scaling_damage(BT_DEVICE BtState* s, int damage,
                                   unsigned short first, unsigned short count, float mod) {
    float sum = 0.0f;
    for (int i = 0; i < (int)count; i++) {
        if ((int)(first + i) >= BT_NITEM_SCALING) break;
        BtItemScaling sc = BT_ITEM_SCALING[first + i];
        int id = bt_core_id_from_istr(sc.stat);
        if (id == 255) continue;
        sum += bt_core_stat_read(s, id) * (sc.ratio * (1.0f + mod));
    }
    int v = (int)((float)damage + sum);
    return v < 1 ? 1 : v;
}

/* weapon_service.gd:551-557 get_structure_attack_speed: the structure_attack_speed
 * stat plus sum_scaling_stat_values over the structures_cooldown_reduction bucket
 * (each entry is [stat, percent], so the added term is stat * pct/100). The
 * stat_levels special case of sum_scaling_stat_values has no producer here. */
BT_HD static float bt_get_structure_attack_speed(BT_DEVICE BtState* s) {
    float v = s->structure_attack_speed;
    for (int i = 0; i < s->n_scdr; i++) {
        int id = bt_core_id_from_istr(s->scdr_key[i]);
        if (id == 255) continue;
        v += bt_core_stat_read(s, id) * (s->scdr_pct[i] / 100.0f);
    }
    return v;
}

/* weapon_service.gd:560-565 apply_structure_attack_speed_effects. */
BT_HD static int bt_apply_structure_attack_speed(BT_DEVICE BtState* s, int base_cooldown) {
    if (base_cooldown <= 0) return base_cooldown;
    return bt_apply_atk_spd_mod_cd(base_cooldown, bt_get_structure_attack_speed(s) / 100.0f);
}


/* Per-copy effect values. The source creates one effect INSTANCE per owned copy,
 * so a canonical row may be owned several times with different (curse-modified)
 * value/value2 tuples. bt_row_variant_add records one copy's tuple (merging
 * exact duplicates); the accessors below aggregate the owned tuples exactly:
 *  - bt_row_sum_v   : the SUM over copies (SUM-storage buckets)
 *  - bt_row_max_v2  : the largest per-copy value2 (max-threshold semantics)
 *  - bt_row_dominant              : the copy with the largest |value| (used only
 *                                   where a single slot exists, e.g. the one
 *                                   decaying-stats slot per stat).
 * Rows that are not variant-tracked fall back to the generated table value. */
/* Ensure the row's run holds at least `need` variants.  Returns 0 (and sets the
 * LOUD curse_overflow) when the shared pool cannot supply the run.  Runs are
 * bump-allocated and grow by doubling, so a row's earlier entries are copied and
 * the old (smaller) run is left unused until bt_core_reset. */
BT_HD static int bt_rvar_reserve(BT_DEVICE BtState* s, int row, int need) {
    int cap, off, i;
    if ((int)s->rvar_cap[row] >= need) return 1;
    cap = s->rvar_cap[row] ? (int)s->rvar_cap[row] : BT_RVAR_MIN_CAP;
    while (cap < need) cap *= 2;
    if (cap > BT_MAX_VARIANTS) cap = BT_MAX_VARIANTS;
    if ((int)s->rvar_used + cap > BT_RVAR_POOL) { s->curse_overflow = 1; return 0; }
    off = (int)s->rvar_used;
    s->rvar_used = (unsigned short)(s->rvar_used + cap);
    for (i = 0; i < (int)s->nvar[row]; i++) {
        s->rvar[off + i] = s->rvar[s->rvar_off[row] + i];
        s->var_cs[off + i] = s->var_cs[s->rvar_off[row] + i];
    }
    s->rvar_off[row] = (unsigned short)off;
    s->rvar_cap[row] = (unsigned char)cap;
    return 1;
}

BT_HD static void bt_row_variant_add(BT_DEVICE BtState* s, int row, int value, int value2) {
    unsigned char n = s->nvar[row];
    BT_DEVICE BtRowVariant* rv = bt_rvar(s, row);
    for (int i = 0; i < (int)n; i++) {
        if (rv[i].value == value && rv[i].value2 == value2) {
            if (rv[i].count < 65535) rv[i].count++;
            return;
        }
    }
    if (n >= BT_MAX_VARIANTS) { s->curse_overflow = 1; return; }   /* explicit */
    if (!bt_rvar_reserve(s, row, (int)n + 1)) return;              /* loud (curse_overflow) */
    rv = bt_rvar(s, row);
    rv[n].value = value;
    rv[n].value2 = value2;
    rv[n].count = 1;
    s->nvar[row] = (unsigned char)(n + 1);
}
BT_HD static float bt_row_sum_v(BT_DEVICE BtState* s, int row, BT_CONST_PTR(BtItemEff) E) {
    if (s->nvar[row] == 0) return (float)E->value * (float)s->bk_count[row];
    float sum = 0.0f;
    for (int i = 0; i < (int)s->nvar[row]; i++)
        sum += (float)bt_rvar(s,row)[i].value * (float)bt_rvar(s,row)[i].count;
    return sum;
}
BT_HD static float bt_row_max_v2(BT_DEVICE BtState* s, int row, BT_CONST_PTR(BtItemEff) E) {
    if (s->nvar[row] == 0) return (float)E->value2;
    float m = 0.0f;
    for (int i = 0; i < (int)s->nvar[row]; i++)
        if ((float)bt_rvar(s,row)[i].value2 > m) m = (float)bt_rvar(s,row)[i].value2;
    return m;
}
BT_HD static int bt_row_dominant(BT_DEVICE BtState* s, int row, BT_CONST_PTR(BtItemEff) E, BT_THREAD int* v2_out) {
    if (s->nvar[row] == 0) { *v2_out = (int)E->value2; return (int)E->value; }
    int best = 0;
    for (int i = 1; i < (int)s->nvar[row]; i++)
        if (bt_fabsf((float)bt_rvar(s,row)[i].value) > bt_fabsf((float)bt_rvar(s,row)[best].value)) best = i;
    *v2_out = bt_rvar(s,row)[best].value2;
    return bt_rvar(s,row)[best].value;
}
/* Total owned copies of a variant-tracked row (rows with no recorded variants
 * fall back to the legacy per-opcode counter). */
BT_HD static int bt_row_owned(BT_DEVICE BtState* s, int row) {
    if (s->nvar[row] == 0) return (int)s->bk_count[row];
    int n = 0;
    for (int i = 0; i < (int)s->nvar[row]; i++) n += (int)bt_rvar(s,row)[i].count;
    return n;
}
/* i-th owned copy's (value,value2); returns 0 past the last copy. Rows without
 * recorded variants behave as `owned` identical table-valued copies. */
BT_HD static int bt_row_get(BT_DEVICE BtState* s, int row, BT_CONST_PTR(BtItemEff) E, int i,
                            BT_THREAD int* value, BT_THREAD int* value2) {
    if (s->nvar[row] > 0) {
        int k = 0;
        for (int v = 0; v < (int)s->nvar[row]; v++)
            for (int c = 0; c < (int)bt_rvar(s,row)[v].count; c++) {
                if (k == i) { *value = bt_rvar(s,row)[v].value; *value2 = bt_rvar(s,row)[v].value2; return 1; }
                k++;
            }
        return 0;
    }
    if (i < (int)s->bk_count[row]) { *value = (int)E->value; *value2 = (int)E->value2; return 1; }
    return 0;
}

/* main.gd:538-546; run_data.gd:239-244 resets the per-wave counters. */
BT_HD static void bt_burning_kill_stats(BT_DEVICE BtState* s) {
    for (int g = 0; g < s->n_gwb; g++) {
        if (s->gwb_grants[g] >= s->gwb_limit[g]) continue;
        s->gwb_kills[g]++;
        if (s->gwb_every[g] > 0 && s->gwb_kills[g] % s->gwb_every[g] == 0) {
            s->gwb_grants[g]++;
            bt_core_stat_add(s, s->gwb_id[g], (float)(s->gwb_gain[g] * s->gwb_count[g]));
        }
    }
}

/* Weapon runtime hooks.  brotato_weapon_stats.h pulls in
 * brotato_weapon_inventory.h, whose per-slot hook prototypes/dispatch are
 * selected by these macros AT THAT POINT, so they must be defined before the
 * first weapon_stats include or every hook call is compiled out. */
#define BT_WEAPON_MELEE_HOOKS 1
#define BT_WEAPON_RANGED_HOOKS 1
#define BT_WEAPON_FX_HOOKS 1

#include "brotato_weapon_stats.h"
#include "brotato_weapon_set_runtime.h"
/* Item-related weapon bonuses (unique_weapon_effects / additional_weapon_effects
 * / tier_i|iv_weapon_effects, run_data.gd:1085-1161; owner WeaponStats).  After
 * bt_weapon_set_runtime.h (same recompute family), after the stat helpers, and
 * before bt_apply_item, which routes the item bucket rows into it. */
#include "brotato_item_weapon_bonus.h"
/* Enemy hit-debuff runtime (enemy_percent_damage_taken; owner EnemyHitEffects,
 * brotato_enemy_hit_effects.h).  Placed here, after bt_row_owned/bt_row_get and
 * BtState but BEFORE brotato_weapon_inventory.h / brotato_progression.h /
 * brotato_item_explosions.h, so every consumer (Melee/Projectiles, the burn
 * block, bt_item_expl_hit_enemy's percent fold, the progression TempStats.reset
 * wipe) sees it with no extra forward declarations.  bt_spawn_kind_at and
 * bt_core_reset are defined earlier and use the two forward declarations in the
 * core's prototype block. */
#include "brotato_enemy_hit_effects.h"

/* weapon.gd:311-313 + 324-326 + 349-350 reset_cooldown, the init_stats(false)
 * path stats_manager.gd:36-45 drives when a weapon-feeding stat changed:
 *   _current_cooldown = min(_current_cooldown, current_stats.cooldown *
 *       (is_big_reload_active() ? additional_cooldown_multiplier : 1))
 * is_big_reload_active() = additional_cooldown_every_x_shots != -1 and
 * _nb_shots_taken % every == 0.  ZERO RNG: never get_next_cooldown, which would
 * consume a start-cooldown draw the source does not make on this path.
 * Defined HERE because BtPlayerWeaponStats is declared after
 * brotato_weapon_inventory.h in the TU; WeaponInventory's bt_weapon_stats_changed
 * (the stats-change notification the core drives from bt_core_step when
 * `wpn_dirty` is set) dispatches melee slots here and ranged slots to Ranged's
 * own bt_weapon_ranged_stats_changed, which applies the same formula. */
BT_HD static void bt_weapon_reset_cooldown(BT_DEVICE BtState* s, int slot) {
    if (slot < 0 || slot >= (int)s->wpn_count) return;
    unsigned short wid = s->wpn_id[slot];
    if (wid == BT_WEAPON_NONE || wid >= BT_NWEAPONS) return;
    BtPlayerWeaponStats st;
    if (!bt_weapon_stats(s, slot, &st)) return;
    int shots = (BT_WEAPONS[wid].kind == BT_WEAPON_KIND_MELEE)
                    ? (int)s->wpn_melee_shots[slot] : (int)s->wrng_shots[slot];
    int every = st.additional_cd_every;
    float mult = 1.0f;
    if (every != -1 && st.additional_cd_multiplier > 0.0f &&
        (every <= 0 || (shots % every) == 0))
        mult = st.additional_cd_multiplier;
    float cap = (float)st.cooldown * mult;
    if (s->wpn_cd[slot] > cap) s->wpn_cd[slot] = cap;
}

#if defined(__METAL_VERSION__)
/* Shared by shop purchases, deferred boxes and mirror copies. Do not expand
 * the entire effect dispatcher into every shader call site. */
__attribute__((noinline))
#endif
BT_HD static void bt_apply_item(BT_DEVICE BtState* s, unsigned short it) {
    BT_CONST_PTR(BtItem) item = &BT_ITEMS[it];
    float before[BT_NSTATS];
    for (int q = 0; q < BT_NSTATS; q++) before[q] = bt_core_stat_read(s, q);
    int curse_effect_present = 0;
    int consumed = it == BT_ITEM_IDX_MIRROR ? 0 :
        (it == BT_ITEM_IDX_GOLDFISH ? 1 : (it == BT_ITEM_IDX_HOURGLASS ? 2 : -1));
    BtShopItemCopy copy = {0, 0, 0, 0};
    /* ItemData pool bookkeeping: one owned copy per acquisition. A counter that
     * would wrap sets curse_overflow (explicit, never a silent loss). */
    if (s->item_owned[it] == 65535u) s->curse_overflow = 1;
    else s->item_owned[it]++;
    /* Ordered dispatch over the top-level effect graph (BT_ITEM_EFF_INDEX order
     * == the source `effects` array order). Under curse each top-level effect
     * draws its own modifier (dlc_1_data.gd:82), exactly like the source. */
    for (int e = 0; e < (int)item->eff_count; e++) {
        unsigned short row = (unsigned short)BT_ITEM_EFF_INDEX[item->eff_first + e];
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[row];
        int cursed = s->curse_apply;
        float mod = 0.0f;
        if (cursed) {
            mod = bt_curse_modifier(s);
            if (mod > s->curse_maxmod) s->curse_maxmod = mod;
        }
        /* boosted value/value2 for this effect (identity when not cursed) */
        int nv = E->value, nv2 = E->value2, nv3 = E->value3;
        if (cursed) {
            float em = mod;
            if ((it == BT_ITEM_IDX_BABY_GECKO && E->key == BT_ISTR_STAT_RANGE) ||
                (it == BT_ITEM_IDX_POTION && E->key == BT_ISTR_STAT_HP_REGENERATION) ||
                it == BT_ITEM_IDX_PILE_OF_BOOKS || it == BT_ITEM_IDX_POCKET_FACTORY)
                em = mod * BT_DLC1_CURSE_INCREASE_FACTOR_FOR_MEDIOCRE_BOOSTS;   /* :244 */
            if (E->key == BT_ISTR_DODGE_CAP) {
                nv = bt_rng_randi_range(&s->utils_rng, 72, 76);
            } else if (it == BT_ITEM_IDX_WILL_O_THE_WISP && E->key == BT_ISTR_STAT_ELEMENTAL_DAMAGE) {
                nv = bt_rng_randi_range(&s->utils_rng, 16, 25);
                nv3 = bt_rng_randi_range(&s->utils_rng, 5, 7);
            } else if (it == BT_ITEM_IDX_CANDY_BAG && E->key == BT_ISTR_EXTRA_ELITE_NEXT_WAVE_CHANCE) {
                nv = E->value;
            } else if (E->key == BT_ISTR_HP_START_NEXT_WAVE) {
                nv = -100;
            } else if (E->key == BT_ISTR_JELLYSHIELD_COUNT) {
                bt_core_stat_add(s, 2, (float)bt_curse_stat(1.0f, em));         /* +ceil(1*(1+mod)) armor */
            } else if (E->key == BT_ISTR_HIT_PROTECTION) {
                nv = 2;
            } else if (E->key == BT_ISTR_ONE_SHOT_TREES || E->key == BT_ISTR_TREE_TURRETS) {
                s->scalar_effect[BT_ISTR_TREES] += 1.0f;
            } else if (E->key == BT_ISTR_INSTANT_GOLD_ATTRACTING && E->value == 100) {
                bt_core_stat_add(s, 4, (float)bt_curse_stat(BT_DLC1_CURSE_SIFDS_RELIC_DODGE, em));
            } else if (E->key == BT_ISTR_HP_REGEN_BONUS) {
                float b = bt_curse_value_f((float)E->value2, E->sign, em, 1, BT_SIGN_POSITIVE);
                nv2 = (int)(b > 90.0f ? 90.0f : b);
            } else if (E->key == BT_ISTR_ITEM_MIRROR) {
                copy.price = (int)bt_curse_value_f(BT_DLC1_CURSE_MIRROR_ITEM_PRICE,
                    BT_SIGN_POSITIVE, em, 0, BT_SIGN_POSITIVE);
                s->scalar_effect[BT_ISTR_ITEMS_PRICE] += (float)copy.price;
            } else {
                int ovr = 0, ovr_sign = BT_SIGN_POSITIVE;
                if (bt_eff_is(E, BT_ISTR_CONSUMABLE_HEAL_OVER_TIME)) {
                    ovr = 1; ovr_sign = BT_SIGN_NEUTRAL;
                    bt_core_stat_add(s, 1, (float)bt_curse_stat(BT_DLC1_CURSE_JERKY_HP_REGEN, em));
                } else if (E->key == BT_ISTR_KNOCKBACK_AURA) { ovr = 1; ovr_sign = BT_SIGN_NEGATIVE; }
                else if (E->key == BT_ISTR_MODIFY_EVERY_X_PROJECTILE) { ovr = 1; ovr_sign = BT_SIGN_NEGATIVE; }
                else if (it == BT_ITEM_IDX_ESTYS_COUCH && E->key == BT_ISTR_STAT_SPEED) { ovr = 1; ovr_sign = BT_SIGN_POSITIVE; }
                else if (bt_eff_is(E, BT_ISTR_UPGRADE_RANDOM_WEAPON))
                    bt_core_stat_add(s, 2, (float)bt_curse_stat(BT_DLC1_CURSE_ANVIL_ARMOR, em));
                else if (bt_eff_is(E, BT_ISTR_EXTRA_ITEM_IN_CRATE) && E->key == BT_ISTR_RANDOM)
                    bt_core_stat_add(s, 10, (float)bt_curse_stat(BT_DLC1_CURSE_TREASURE_MAP_LUCK, em));
                nv = bt_curse_value(E->value, E->sign, em, ovr, ovr_sign);
                if (bt_eff_is(E, BT_ISTR_CONSUMABLE_STATS_WHILE_MAX))
                    nv2 = bt_curse_value(E->value2, E->sign, em, ovr, ovr_sign);
                else if (E->key == BT_ISTR_REMOVE_SPEED)
                    nv2 = nv * 4;
                else if (E->key == BT_ISTR_BURNING_ENEMY_HP_PERCENT_DAMAGE ||
                         E->key == BT_ISTR_GIANT_CRIT_DAMAGE)
                    nv2 = nv / 10;
                /* NOTE: the source also lists bonus_current_health_damage_hash here,
                 * but that hash is not interned by extract_items.py (no shipped
                 * item uses it), so BONUS_DAMAGE_AGAINST_TARGETS_ABOVE_HP keeps its
                 * own value2 threshold instead of value/10. */
                else if (E->key == BT_ISTR_GOLD_ON_CRIT_KILL) {
                    if (nv > 100) nv = 100;
                    if (it == BT_ITEM_IDX_HUNTING_TROPHY)
                        bt_core_stat_add(s, 9, (float)bt_curse_stat(BT_DLC1_CURSE_HUNTING_TROPHY_CRIT_CHANCE, em));
                }
            }
        }
        if (consumed >= 0 && E->key == BT_ISTR_HP_START_NEXT_WAVE) copy.hp_start += nv;
        switch (E->opcode) {
        /* ------------------------------------------------------------------
         * WEAPON-SCOPED ITEM MODIFIERS.  Each effect's `apply()` pushes its
         * tuple into the matching player-effect bucket (per owned copy), and
         * WeaponStats consumes those buckets when it resolves a live weapon:
         *   WeaponCooldownEffect (effects/items/weapon_cooldown_effect.gd)
         *     apply(): get_player_effects(pi)[key_hash].push_back(value); sort()
         *     key_hash = minimum_weapon_cooldowns | maximum_weapon_cooldowns,
         *     `value` in TICKS -> bt_wstats_min_cd_add / bt_wstats_max_cd_add,
         *     consumed by bt_ws_apply_min_cd / bt_ws_apply_max_cd
         *     (weapon_service.gd:558-600).  Shipped producer: row 452, item 184
         *     (`items/all/ball_and_chain/effects/ball_and_chain_effect_3.tres`,
         *     key minimum_weapon_cooldowns, value 45).  No shipped
         *     maximum_weapon_cooldowns row exists; the branch is the source's.
         *   EffectWithSubEffects with key modify_every_x_projectile (the DLC
         *     seashell item, row 565): apply() also pushes the PROJECTILES
         *     sub-effect counts; the interval is the effect's `value` and the
         *     extra projectiles are the sum of its PROJECTILES sub rows
         *     (weapon_service.gd:650-677 stats_every_x_shots[effect.value] with
         *     nb_projectile_effect += sub_effect.value).  Consumed by
         *     bt_weapon_stats_variants through s->mxp_x / s->mxp_nb.
         *   Effect with storage KEY_VALUE, custom_key weapon_scaling_stats
         *     (effect.gd apply(): effects[weapon_scaling_stats].push_back([key,
         *     value])): the stat `key` scales every live weapon's DAMAGE by
         *     `value` percent (weapon_service.gd:618-632).  bt_wstats_scaling_add,
         *     consumed at the weapon_scaling_stats rail of bt_weapon_stats.
         *     Shipped producers: items/all/frozen_heart effect_0
         *     (stat_elemental_damage 10) and items/all/nail effect_0b
         *     (stat_engineering 20); the branch is in the BT_ITEMEFF_EFFECT case.
         * The weapon_type_bonus / weapon_class_bonus buckets have NO shipped item
         * producer (their rows come from character effects and class sets, applied
         * by bt_character_apply / bt_weapon_set_runtime), so no branch is added
         * for them; wire_node below is the only other bucket producer site.
         * The slow_on_hit bucket (bt_wstats_slow_on_hit_add) likewise has NO
         * shipped producer: no item/weapon resource pushes [stat, value] into
         * Keys.slow_on_hit_hash (effect.gd would need storage KEY_VALUE,
         * custom_key slow_on_hit), so the helper is intentionally unreferenced.
         * ------------------------------------------------------------------ */
        case BT_ITEMEFF_WEAPON_COOLDOWN: {
            if (E->key == (unsigned short)BT_ISTR_MINIMUM_WEAPON_COOLDOWNS)
                bt_wstats_min_cd_add(s, nv);
            else if (E->key == (unsigned short)BT_ISTR_MAXIMUM_WEAPON_COOLDOWNS)
                bt_wstats_max_cd_add(s, nv);
            break;
        }
        case BT_ITEMEFF_EFFECT_WITH_SUB_EFFECTS: {
            if (E->key == (unsigned short)BT_ISTR_MODIFY_EVERY_X_PROJECTILE) {
                /* pure count of the PROJECTILES sub rows (no stat mutation here:
                 * apply/unapply of the subs happen around the variant init) */
                int nb_sub = 0;
                for (int q = 0; q < (int)E->sub_count; q++) {
                    int idx = (int)E->sub_first + q;
                    if (idx >= BT_NITEM_EFF_INDEX) break;
                    int srow = (int)BT_ITEM_EFF_INDEX[idx];
                    if (srow < 0 || srow >= BT_NITEM_EFF_ROWS) continue;
                    if (BT_ITEM_EFF_ROWS[srow].key == (unsigned short)BT_ISTR_PROJECTILES)
                        nb_sub += BT_ITEM_EFF_ROWS[srow].value;
                }
                bt_wstats_modify_x_add(s, nv > 0 ? nv : 1, nb_sub);
            }
            break;
        }
        case BT_ITEMEFF_EFFECT:
        case BT_ITEMEFF_NULL: {
            if (cursed && E->key == BT_ISTR_STAT_CURSE) curse_effect_present = 1;
            if ((E->storage == 1 || E->storage == 4) &&
                    (E->custom_key == BT_ISTR_DUPLICATE_ITEM ||
                     E->custom_key == BT_ISTR_INCREASE_TIER_ON_REROLL ||
                     E->custom_key == BT_ISTR_UPGRADE_RANDOM_WEAPON ||
                     E->custom_key == BT_ISTR_EXTRA_ITEM_IN_CRATE)) {
                int modifier = cursed ? (int)bt_roundf(mod * 100.0f) : 0;
                if (consumed >= 0) copy.modifier = modifier;
                bt_scalar_bucket_register(s, row, nv, modifier);
                break;
            }
            if (E->storage == 1 && E->custom_key == BT_ISTR_GOLD_ON_CRIT_KILL) {
                bt_scalar_bucket_register(s, row, nv, 0);
                break;
            }
            if (E->storage == 1 && (E->custom_key == BT_ISTR_STATS_NEXT_WAVE ||
                    E->custom_key == BT_ISTR_TEMP_STATS_ON_HIT ||
                    E->custom_key == BT_ISTR_TEMP_STATS_WHILE_NOT_MOVING)) {
                int modifier = E->custom_key == BT_ISTR_STATS_NEXT_WAVE ? 0 :
                    (cursed ? (int)bt_roundf(mod * 100.0f) : 0);
                bt_temporal_bucket_register(s, row, nv, modifier);
                break;
            }
            /* WeaponService.get_structure_attack_speed inputs (weapon_service.gd:551-557).
             * `structures_cooldown_reduction` is a KEY_VALUE bucket: effect.gd
             * apply() stores bucket[custom_key] += [key, value] and NEVER adds the
             * value to `key`'s stat, so improved_tools must not raise attack_speed. */
            if (E->storage == 1 /* KEY_VALUE */ &&
                E->custom_key == BT_ISTR_STRUCTURES_COOLDOWN_REDUCTION) {
                int found = -1;
                for (int i = 0; i < s->n_scdr; i++) if (s->scdr_key[i] == E->key) { found = i; break; }
                if (found < 0 && s->n_scdr < 8) {
                    found = s->n_scdr++;
                    s->scdr_key[found] = E->key;
                    s->scdr_pct[found] = 0.0f;
                }
                if (found >= 0) s->scdr_pct[found] += (float)nv;
                break;
            }
            if (E->key == BT_ISTR_STRUCTURE_ATTACK_SPEED) {
                s->structure_attack_speed += (float)nv;    /* SUM storage */
                break;
            }
            if (E->storage == 1 && (E->custom_key == BT_ISTR_STATS_ON_LEVEL_UP ||
                    E->custom_key == BT_ISTR_STATS_END_OF_WAVE)) {
                if (s->bk_count[row] == 0) s->bk_active[s->n_bk_active++] = row;
                s->bk_count[row]++;
                bt_row_variant_add(s, row, nv, nv2);
                break;
            }
            /* WeaponService._apply_weapon_scaling_stat_effects
             * (weapon_service.gd:618-632): an item Effect with storage
             * KEY_VALUE and custom_key weapon_scaling_stats pushes
             * [key_hash, value] into the player's weapon_scaling_stats bucket
             * (effect.gd apply: effects[custom_key].push_back([key, value])).
             * Every live weapon then resolves `existing.ratio += value/100` or
             * appends [stat, value/100]; WeaponStats folds that bucket in at its
             * weapon_scaling_stats rail (bt_ws_scaling_add_row -> bt_ws_scaling_sum),
             * so it scales the weapon's DAMAGE by a percentage of the player stat.
             * Shipped producers: items/all/frozen_heart effect_0
             * (stat_elemental_damage, 10) and items/all/nail effect_0b
             * (stat_engineering, 20), rows 443 / 459.  Without this branch both
             * fell through to the generic stat add below and granted a FLAT stat
             * instead of the damage-scaling bonus. */
            if (E->storage == 1 && E->custom_key == BT_ISTR_WEAPON_SCALING_STATS) {
                bt_wstats_scaling_add(s, E->key, (float)nv);
                break;
            }
            /* run_data.gd:1085-1161 update_item_related_effects buckets: a plain
             * Effect with storage KEY_VALUE whose custom_key names one of the
             * item-related weapon-bonus buckets (effect.gd apply pushes
             * [key_hash, value] into effects[custom_key]) registers its declared
             * pair; bt_item_wbonus_refresh (below, after the whole item) then
             * multiplies it by the current weapon count and applies it to the
             * player stat.  Shipped producers: focus (-3 attack_speed / unique
             * weapon), spider (+6 / unique), jelly (+1 max_hp / unique) and the
             * excalibur weapon row (-3 armor / owned weapon, registered by
             * bt_iwb_register_weapon).  Without this branch they fell through to
             * the generic stat add below and granted a FLAT stat. */
            if (E->storage == 1) {
                int iwb = bt_iwb_bucket_of_custom_key(E->custom_key);
                if (iwb >= 0) { bt_iwb_add(s, iwb, E->key, nv); break; }
            }
            /* A plain Effect whose key is a *_cap stat (items/all/ghost_outfit:
             * key dodge_cap, REPLACE 70) writes the SAME *_cap entry that
             * Utils.get_capped_stat reads (effect.gd apply: REPLACE sets
             * effects[key], SUM accumulates).  The core keeps those caps in
             * stat_cap[] (like StatCapEffect); without this branch the dodge cap
             * would stay pinned at the init default 60 and ghost_outfit's +10
             * cap would be lost. */
            if (E->key == BT_ISTR_HP_CAP || E->key == BT_ISTR_SPEED_CAP ||
                E->key == BT_ISTR_DODGE_CAP) {
                int cid = bt_cap_key_to_stat(E->key);
                if (cid != 255) {
                    if (E->storage == 2) s->stat_cap[cid] = (float)nv;
                    else s->stat_cap[cid] += (float)nv;
                    bt_core_stat_sync(s);
                }
                break;
            }
            /* Keys with no core stat id are preserved as scalars so the
             * shop/XP/harvest consumers can read them (items_price, xp_gain,
             * free_rerolls, reroll_price, level_upgrades_modifications,
             * harvesting_growth, ...). effect.gd apply(): SUM accumulates,
             * REPLACE overwrites. */
            if (E->key != BT_ISTR_NONE && bt_core_id_from_istr(E->key) == 255) {
                if (E->storage == 0) s->scalar_effect[E->key] += (float)nv;
                else if (E->storage == 2) s->scalar_effect[E->key] = (float)nv;
                break;
            }
            int id = bt_core_id_from_istr(E->key);
            if (id == 255) break;                       /* unmapped stat key */
            bt_core_stat_add(s, id, (float)nv);
            break;
        }
        case BT_ITEMEFF_GAIN_STAT_FOR_EVERY_STAT: {
            int t = bt_core_id_from_istr(E->key);
            /* A source key of 255 marks a NON-stat scaled source (materials,
             * burning_enemy, living_enemy, structure, pet, item_*).  The source's
             * get_scaling_bonus / linked_stats.gd resolve those by the raw key,
             * not by a core stat id, so the row must still be registered;
             * bt_recompute_links resolves the supported ones and gains nothing for
             * the rest, exactly like linked_stats.gd's `else: continue`.  Bailing
             * here would silently drop Padding's materials link. */
            if (t == 255 || E->nb_stat_scaled == 0) break;
            /* register the copy's variant; the link recomputes from final stats */
            bt_row_variant_add(s, row, nv, nv2);
            s->lk_count[row]++;
            break;
        }
        case BT_ITEMEFF_STAT_CAP: {
            int id = bt_cap_key_to_stat(E->key);
            if (id == 255) break;
            /* stat_cap_effect.gd apply: set_cap_to_current_stat (aux_key) pins the
             * cap to the CURRENT value of that stat, else the flat value. */
            int cur = bt_core_id_from_istr(E->aux_key);
            s->stat_cap[id] = (cur != 255) ? bt_core_stat_read(s, cur) : (float)nv;
            bt_core_stat_sync(s);
            break;
        }
        case BT_ITEMEFF_EXPLODING:
        case BT_ITEMEFF_ITEM_EXPLODING:
        case BT_ITEMEFF_ITEM_EXPLODING_WHEN_BELOW_HP:
            /* dlc_1_data.gd:146-150: an ItemExplodingEffect's curse branch boosts
             * a scale_with_missing_health effect's `value` and otherwise leaves
             * chance/scale alone; a WeaponData's ExplodingEffect boosts `chance`
             * (no such row keys the families below). */
            if (E->key == BT_ISTR_EXPLODE_ON_CONSUMABLE || E->key == BT_ISTR_EXPLODE_ON_DEATH) {
                /* Position-based families: main.gd:534-535 (explode_on_death at
                 * the dying enemy's position) and main.gd:693-696
                 * (explode_on_consumable at the consumable's position) both go
                 * through RunData.handle_explode_effect, whose per-player bucket
                 * is the effect-row bucket these copies register in
                 * (brotato_item_explosions.h bt_item_expl_on_key reads it). */
                bt_scalar_bucket_register(s, row, nv, (int)bt_roundf((cursed ? mod : 0.0f) * 100.0f));
            } else if (E->key == BT_ISTR_EXPLODE_ON_HIT || E->key == BT_ISTR_EXPLODE_WHEN_BELOW_HP) {
                bt_scalar_bucket_register(s, row, nv,
                    (int)bt_roundf((cursed ? mod : 0.0f) * 100.0f));
            }
            break;
        case BT_ITEMEFF_BURN_CHANCE:
        case BT_ITEMEFF_BURNING:
            bt_set_burn_from_item(s, row, cursed ? mod : 0.0f);
            break;
        case BT_ITEMEFF_PET:
        case BT_ITEMEFF_STRUCTURE:
        case BT_ITEMEFF_TURRET:
        case BT_ITEMEFF_BUILDER_TURRET:
        case BT_ITEMEFF_BUILDER_TURRET_UPGRADE:
        case BT_ITEMEFF_BLAZEMANDER:
        case BT_ITEMEFF_BONK_DOG:
        case BT_ITEMEFF_BOT_O_MINE:
        case BT_ITEMEFF_CATLING_GUN:
        case BT_ITEMEFF_RATZILLA:
        case BT_ITEMEFF_LOOTWORM:
        case BT_ITEMEFF_DOC_MOTH:
        case BT_ITEMEFF_SCAPEGOAT:
            /* wandering_bot gets an extra stat_speed under curse (:86-90) */
            if (cursed && E->opcode == BT_ITEMEFF_STRUCTURE && E->key == BT_ISTR_WANDERING_BOT)
                bt_core_stat_add(s, 3, (float)bt_curse_stat(BT_DLC1_CURSE_WANDERING_BOT_SPEED, mod));
            /* StructureEffect/TurretEffect/BuilderTurretEffect append to
             * Keys.structures_hash, PetEffect subclasses to Keys.stat_pets_hash;
             * the entities are created by the emulated StructureTimer. */
            bt_register_summon(s, it, cursed ? mod : 0.0f);
            break;
        case BT_ITEMEFF_TEMP_STATS_PER_INTERVAL: {
            int id = bt_core_id_from_istr(E->key);
            if (id == 255) break;
            float interval = E->interval > 0.0f ? E->interval : 1.0f;
            /* temp_stats_per_interval_effect.gd apply(): an existing entry with
             * the same (key, interval, reset_on_hit) SUMs the value instead of
             * taking a second slot. */
            int found = -1;
            for (int i = 0; i < s->n_tsi; i++)
                if ((int)s->tsi_id[i] == id && s->tsi_interval[i] == interval &&
                    s->tsi_reset[i] == E->reset_on_hit) { found = i; break; }
            if (found >= 0) { s->tsi_val[found] += (float)nv; break; }
            if (s->n_tsi >= 8) { s->stat_reg_overflow = 1; break; }
            int i = s->n_tsi++;
            s->tsi_id[i] = (unsigned char)id;
            s->tsi_val[i] = (float)nv;
            s->tsi_interval[i] = interval;
            s->tsi_timer[i] = interval;
            s->tsi_reset[i] = E->reset_on_hit;
            s->tsi_stacks[i] = 0.0f;
            break;
        }
        case BT_ITEMEFF_PERCENT_DAMAGE:
            /* percent_damage_effect.gd:46-47 apply(): the effect is pushed into
             * the player's enemy_percent_damage_taken BUCKET (RunData
             * effects[custom_key_hash]); the ENEMY's effect behavior
             * (percent_damage_taken_enemy_effect_behavior.gd) reads that bucket,
             * gates it by the hit's scaling stats, accumulates stacks with
             * max_stacks/max_procs and feeds get_bonus_damage into the single
             * enemy-damage formula.  nv is the curse-transformed value
             * (dlc_1_data.gd:261 _boost_effect_value_positively).  A repeat copy
             * of the same row stacks in the bucket, exactly like every other
             * bucket key; the retired player-scoped pd_* temp-stat reading was
             * the wrong pool (the debuff belongs to the ENEMY, not the player). */
            bt_scalar_bucket_register(s, row, nv, 0);
            break;
        case BT_ITEMEFF_SWAP_MAX_MIN_STAT: {
            /* swap_max_min_stat_effect.gd:17-35: each stat's PERMANENT raw is set
             * so its effective Utils.get_stat becomes the other stat's current
             * effective value, keeping this stat's own temp/link layers
             * (new_permanent = (other_eff - own_temp_eff - own_link_eff)/own_gain).
             * Under curse an extra axolotl_stats effect is added to both. */
            int a = bt_core_id_from_istr(E->key);
            int b = bt_core_id_from_istr(E->aux_key);
            if (a != 255 && b != 255) {
                float av = bt_core_stat_read(s, a), bv = bt_core_stat_read(s, b);
                float gan = s->stat_gain[a] != 0.0f ? s->stat_gain[a] : 1.0f;
                float gbn = s->stat_gain[b] != 0.0f ? s->stat_gain[b] : 1.0f;
                float newa = (bv - (s->temp_stat[a] + s->link_stat[a]) * gan) / gan;
                float newb = (av - (s->temp_stat[b] + s->link_stat[b]) * gbn) / gbn;
                bt_core_stat_add(s, a, newa - s->stat_raw[a]);
                bt_core_stat_add(s, b, newb - s->stat_raw[b]);
                if (cursed) {
                    float ax = (float)bt_curse_stat(BT_DLC1_CURSE_AXOLOTL_STATS, mod);
                    bt_core_stat_add(s, a, ax);
                    bt_core_stat_add(s, b, ax);
                }
            }
            break;
        }
        case BT_ITEMEFF_GAIN_STAT_FOR_KILLED_ENEMIES_WHILE_BURNING: {
            int id = bt_core_id_from_istr(E->key);
            if (id == 255) break;
            int found = -1;
            for (int i = 0; i < s->n_gwb; i++)
                if (s->gwb_row[i] == row && s->gwb_every[i] == nv &&
                    s->gwb_gain[i] == nv2 && s->gwb_limit[i] == nv3) { found = i; break; }
            if (found >= 0) {
                if (s->gwb_count[found] == 65535u) s->stat_reg_overflow = 1;
                else s->gwb_count[found]++;
                break;
            }
            if (s->n_gwb >= 31) { s->stat_reg_overflow = 1; break; }
            int i = s->n_gwb++;
            s->gwb_id[i] = (unsigned char)id; s->gwb_row[i] = (unsigned short)row;
            s->gwb_count[i] = 1; s->gwb_every[i] = nv; s->gwb_gain[i] = nv2;
            s->gwb_limit[i] = nv3; s->gwb_kills[i] = 0; s->gwb_grants[i] = 0;
            break;
        }
        case BT_ITEMEFF_EXTRA_ENEMY:
            if ((E->flags & BT_ITFLAG_EXTRA_ENEMY) && E->extra_group < BT_NITEM_EXTRA_GROUPS) {
                if (s->exg_count[row] == 0 && s->n_exg_active < BT_NITEM_EFF_ROWS)
                    s->exg_active[s->n_exg_active++] = row;
                s->exg_count[row]++;
                bt_row_variant_add(s, row, nv, nv2);
            }
            break;
        case BT_ITEMEFF_CHANCE_STAT_DAMAGE:
            bt_scalar_bucket_register(s, row, nv, 0);
            break;
        case BT_ITEMEFF_PROJECTILE:
            if (E->key == BT_ISTR_ALIEN_EYES || E->key == BT_ISTR_PROJECTILES_ON_DEATH)
                bt_scalar_bucket_register(s, row, nv, (int)bt_roundf((cursed ? mod : 0.0f) * 100.0f));
            else
                bt_add_projectile_emitter(s, it, cursed ? mod : 0.0f);
            break;
        case BT_ITEMEFF_DOUBLE_VALUE:
        case BT_ITEMEFF_DOUBLE_KEY_VALUE: {
            if (s->bk_count[row] == 0 && s->n_bk_active < BT_NITEM_EFF_ROWS)
                s->bk_active[s->n_bk_active++] = row;
            s->bk_count[row]++;
            bt_row_variant_add(s, row, nv, nv2);
            break;
        }
        default:
            break;
        }
    }
    s->dmg_stat = s->melee + s->ranged + s->elemental;
    /* #6 gain_stat_for_equipped_item_with_stat: [stat, delta, stat2, 0] -- when
     * this item raised stat2, add delta to stat (run_data.gd:1895-1907). */
    for (int a = 0; a < s->n_bk_active; a++) {
        int row = (int)s->bk_active[a];
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[row];
        if (!bt_eff_is(E, BT_ISTR_GAIN_STAT_FOR_EQUIPPED_ITEM_WITH_STAT)) continue;
        int t = bt_core_id_from_istr(E->key);
        int s2 = bt_core_id_from_istr(E->aux_key);
        if (t == 255 || s2 == 255) continue;
        if (bt_core_stat_read(s, s2) > before[s2])
            bt_core_stat_add(s, t, bt_row_sum_v(s, row, E));
    }
    /* run_data.gd:850-852 add_item: update_item_related_effects runs after
     * apply_item_effects and BEFORE LinkedStats.reset_player, so the recomputed
     * permanent layer feeds the links (bt_recompute_links below). */
    bt_item_wbonus_refresh(s);
    bt_recompute_links(s);   /* LinkedStats.reset_player: after the whole item */
    if (item->proc_d > 0.0f) {
        s->proc_chance = (float)item->proc_c > s->proc_chance ? (float)item->proc_c : s->proc_chance;
        s->proc_dmg += item->proc_d;
    }
    if (s->curse_apply && !curse_effect_present) {
        /* dlc_1_data.gd:340-343: curse gain = round(max(1, 0.04*value*(1+maxmod)))
         * appended only when the item did not already carry a stat_curse effect. */
        float cv = BT_DLC1_CURSE_CURSE_PER_ITEM_VALUE * (float)item->price *
                   (1.0f + s->curse_maxmod);
        if (cv < 1.0f) cv = 1.0f;
        bt_core_stat_add(s, 13, bt_roundf(cv));
        copy.curse = (int)bt_roundf(cv);
    }
    if (consumed >= 0) {
        int count = s->shop_copy_count[consumed];
        if (count == BT_MAX_PENDING) s->curse_overflow = 1;
        else {
            int slot = (s->shop_copy_head[consumed] + count) % BT_MAX_PENDING;
            s->shop_copies[consumed][slot] = copy;
            s->shop_copy_count[consumed]++;
        }
    }
}

/* run_data.gd:857-875. Only these consumable shop items can be removed by the
 * action contract (there is no recycle action). Preserve acquisition order,
 * including curse-derived price, curse, and starting-health contributions. */
BT_HD static void bt_item_remove(BT_DEVICE BtState* s, unsigned short it) {
    int type = it == BT_ITEM_IDX_MIRROR ? 0 :
        (it == BT_ITEM_IDX_GOLDFISH ? 1 : (it == BT_ITEM_IDX_HOURGLASS ? 2 : -1));
    if (type < 0 || !s->item_owned[it] || !s->shop_copy_count[type]) {
        s->curse_overflow = 1;
        return;
    }
    BtShopItemCopy copy = s->shop_copies[type][s->shop_copy_head[type]];
    s->shop_copy_head[type] = (s->shop_copy_head[type] + 1) % BT_MAX_PENDING;
    s->shop_copy_count[type]--;
    s->item_owned[it]--;
    s->scalar_effect[BT_ISTR_ITEMS_PRICE] -= (float)copy.price;
    s->scalar_effect[BT_ISTR_HP_START_NEXT_WAVE] -= (float)copy.hp_start;
    bt_core_stat_add(s, 13, -(float)copy.curse);
    if (type == 2) s->scalar_effect[BT_ISTR_ITEM_HOURGLASS] -= 1.0f;
    BT_CONST_PTR(BtItem) item = &BT_ITEMS[it];
    for (int e = 0; e < item->eff_count; e++) {
        int row = BT_ITEM_EFF_INDEX[item->eff_first + e];
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[row];
        if (E->custom_key != BT_ISTR_DUPLICATE_ITEM &&
                E->custom_key != BT_ISTR_INCREASE_TIER_ON_REROLL) continue;
        for (int v = 0; v < s->nvar[row]; v++) {
            if (bt_rvar(s,row)[v].value2 != copy.modifier) continue;
            if (--bt_rvar(s,row)[v].count == 0) {
                for (int w = v + 1; w < s->nvar[row]; w++)
                    bt_rvar(s,row)[w - 1] = bt_rvar(s,row)[w];
                s->nvar[row]--;
            }
            break;
        }
        if (s->bk_count[row] > 0) s->bk_count[row]--;
        if (s->bk_count[row] == 0) {
            int w = 0;
            for (int a = 0; a < s->n_bk_active; a++)
                if (s->bk_active[a] != row) s->bk_active[w++] = s->bk_active[a];
            s->n_bk_active = w;
        }
    }
    bt_recompute_links(s);
    if (item->replaced_by != BT_ITEM_NONE) {
        unsigned char cursed = s->curse_apply;
        float maxmod = s->curse_maxmod;
        s->curse_apply = 0; s->curse_maxmod = 0.0f;
        bt_apply_item(s, item->replaced_by);
        s->curse_apply = cursed; s->curse_maxmod = maxmod;
    }
}
/* ---------------------------------------------------------------------------
 * Enemy attack behaviours (each kind's attack_behaviors script) + projectile shapes.
 * ------------------------------------------------------------------------- */
#define BT_PI 3.14159265f

/* pillar/slash hitbox windows (brotato_combat.h).  A slash is a SHAPE whose
 * AnimationPlayer method keys enable the hitbox at 0.48 s and disable it at
 * 0.60 s (measured: the enabled window covers the physics frames whose visible
 * animation position is in [0.48, 0.60), i.e. the 0.6 key has already disabled
 * it), and `stop` fires at 0.63 s.  Both slash scenes share the keyframe grid. */
BT_HD static int bt_proj_shape_active(int shape, float age) {
    if (shape == BT_SHAPE_PILLAR) return (age >= BT_PILLAR_ON && age <= BT_PILLAR_OFF) ? 1 : 0;
    if (shape == BT_SHAPE_SLASH || shape == BT_SHAPE_SLASH2)
        return (age >= BT_SLASH_ON && age < BT_SLASH_OFF) ? 1 : 0;
    return 1;
}
BT_HD static float bt_proj_shape_stop(int shape) {
    if (shape == BT_SHAPE_PILLAR) return BT_PILLAR_STOP;
    if (shape == BT_SHAPE_SLASH || shape == BT_SHAPE_SLASH2) return BT_SLASH_STOP;
    return -1.0f;   /* circle bullets: only the leaving-screen rule stops them */
}
BT_HD static int bt_proj_shape_leaves(int shape) {
    /* slash_projectile.tscn:146 destroy_on_leaving_screen = false */
    return (shape == BT_SHAPE_SLASH || shape == BT_SHAPE_SLASH2) ? 0 : 1;
}
/* radius (circle/pillar) or half extents (slash) + root-local shape offset.
 * Takes the geometry scalars by value so the caller may pass either a table row
 * or a thread-local override without mixing MSL address spaces. */
BT_HD static void bt_proj_shape_geom(int shape, float pr, float phx, float phy,
                                     BT_THREAD float* rad, BT_THREAD float* hx, BT_THREAD float* hy,
                                     BT_THREAD float* ox, BT_THREAD float* oy) {
    float r = pr > 0.0f ? pr : BT_PROJ_R_DEFAULT;
    *rad = 0.0f; *hx = 0.0f; *hy = 0.0f; *ox = 0.0f; *oy = 0.0f;
    if (shape == BT_SHAPE_SLASH || shape == BT_SHAPE_SLASH2) {
        /* the shape's half extents are CONSTANT for the whole sweep (the
         * scene's extents track never applies -- brotato_combat.h bt_slash_shape) */
        *hx = phx > 0.0f ? phx : 16.0f;
        *hy = phy > 0.0f ? phy : 9.0f;
        *ox = BT_SLASH_OFF_X; *oy = BT_SLASH_OFF_Y;
    } else if (shape == BT_SHAPE_PILLAR) {
        *rad = r; *ox = BT_PILLAR_OFF_X; *oy = BT_PILLAR_OFF_Y;
    } else {
        *rad = r;
    }
}

/* Projectile vs player Hurtbox (21 @ (0,-12)): discrete overlap, current
 * transform only (projectile.tscn is a Hitbox Area2D; no swept test). */
BT_HD static int bt_proj_hits_player(BT_DEVICE BtState* s, int p) {
    /* RunData.invulnerable: the player's Hurtbox is disabled (player.gd:114-115
     * disable_hurtbox), so an enemy projectile's Hitbox never REPORTS it -- the
     * shot neither queues a hit nor stops (EnemyProjectile._on_Hitbox_hit_something
     * -> stop() runs only from the reported pair).  No draw, no damage. */
    if (s->invulnerable) return 0;
    float px = s->px, py = s->py + BT_PLAYER_HURT_Y;
    float cx = s->prx[p], cy = s->pry[p];
    int sh = s->prshape[p];
    if (sh == BT_SHAPE_SLASH || sh == BT_SHAPE_SLASH2) {
        float sc = bt_cosf(s->prrot[p]), ss = bt_sinf(s->prrot[p]);
        float lx0, ly0, hx, hy;
        bt_slash_shape(sh, s->prage[p], &lx0, &ly0, &hx, &hy);
        float ox = cx + (sc * lx0 - ss * ly0);
        float oy = cy + (ss * lx0 + sc * ly0);
        float dx = px - ox, dy = py - oy;
        float lx = sc * dx + ss * dy;
        float ly = -ss * dx + sc * dy;
        float qx = lx < -hx ? -hx : (lx > hx ? hx : lx);
        float qy = ly < -hy ? -hy : (ly > hy ? hy : ly);
        float ex = lx - qx, ey = ly - qy;
        return (ex * ex + ey * ey) <= BT_PLAYER_HURT_R * BT_PLAYER_HURT_R ? 1 : 0;
    }
    float ox = (sh == BT_SHAPE_PILLAR) ? BT_PILLAR_OFF_X : 0.0f;
    float oy = (sh == BT_SHAPE_PILLAR) ? BT_PILLAR_OFF_Y : 0.0f;
    float dx = (cx + ox) - px, dy = (cy + oy) - py;
    float rr = s->prrad[p] + BT_PLAYER_HURT_R;
    return (dx * dx + dy * dy) <= rr * rr ? 1 : 0;
}

/* Friendly (pet/turret) projectile vs an enemy's hurtbox circle. The core
 * takes the projectile centre explicitly so a parallel candidate pre-scan can
 * evaluate the SAME predicate at the projectile's predicted post-motion centre
 * (btf_combat_phase_eproj) without writing the state first. bt_proj_hits_enemy
 * is the unchanged serial call site. */
BT_HD static int bt_proj_hits_enemy_at(BT_DEVICE BtState* s, int p, int j, float cx, float cy) {
    BT_CONST_PTR(BtBodyPhysics) G = bt_kgeom(s, j);
    float px = s->ex[j] + G->hurt_x, py = s->ey[j] + G->hurt_y;
    int sh = s->prshape[p];
    if (sh == BT_SHAPE_SLASH || sh == BT_SHAPE_SLASH2) {
        float sc = bt_cosf(s->prrot[p]), ss = bt_sinf(s->prrot[p]);
        float lx0, ly0, hx, hy;
        bt_slash_shape(sh, s->prage[p], &lx0, &ly0, &hx, &hy);
        float ox = cx + (sc * lx0 - ss * ly0);
        float oy = cy + (ss * lx0 + sc * ly0);
        float dx = px - ox, dy = py - oy;
        float lx = sc * dx + ss * dy, ly = -ss * dx + sc * dy;
        float qx = lx < -hx ? -hx : (lx > hx ? hx : lx);
        float qy = ly < -hy ? -hy : (ly > hy ? hy : ly);
        float ex = lx - qx, ey = ly - qy;
        return (ex * ex + ey * ey) <= G->hurt_radius * G->hurt_radius ? 1 : 0;
    }
    float ox = (sh == BT_SHAPE_PILLAR) ? BT_PILLAR_OFF_X : 0.0f;
    float oy = (sh == BT_SHAPE_PILLAR) ? BT_PILLAR_OFF_Y : 0.0f;
    float dx = (cx + ox) - px, dy = (cy + oy) - py;
    float rr = s->prrad[p] + G->hurt_radius;
    return (dx * dx + dy * dy) <= rr * rr ? 1 : 0;
}

BT_HD static int bt_proj_hits_enemy(BT_DEVICE BtState* s, int p, int j) {
    return bt_proj_hits_enemy_at(s, p, j, s->prx[p], s->pry[p]);
}

/* Spawn a projectile into the first free slot. Returns that slot index, or -1
 * when every BT_MAX_PROJ slot is live (callers that only check truthiness keep
 * working: a slot index is 0 only for slot 0, so callers must test `>= 0`).
 * A recycled slot keeps whatever the previous occupant wrote, so the burning and
 * crit the owner carries (prburn_* and prcrit_*, written by the pet fire block)
 * are cleared here for every spawn -- an enemy shot must never look like a
 * burning or critting pet shot. */
BT_HD static int bt_spawn_projectile(BT_DEVICE BtState* s, float x, float y, float rot, float spd,
                                     float dmg, int shape, float rad, float hx, float hy, float prot,
                                     int friendly) {
    int pj = -1;
    for (int q = 0; q < BT_MAX_PROJ; q++) if (!s->pralive[q]) { pj = q; break; }
    if (pj < 0) {
        /* Loud pool exhaustion (same contract as pend_overflow): the shot is
         * dropped, but never silently -- the flag is the assertion surface and
         * pr_dropped counts how many spawns the pool could not host.
         * BT_MAX_PROJ is sized from the measured endless peak
         * (tests/brotato_state_size.cpp occupancy mode) with headroom, so a set
         * flag means the pool is undersized for this workload. */
        s->pr_overflow = 1;
        s->pr_dropped++;
        return -1;
    }
    s->prfriendly[pj] = (unsigned char)(friendly ? 1 : 0);
    s->pr_pet[pj] = 0;      /* only the pet fire block sets this (fly reaction) */
    s->prburn_chance[pj] = 0.0f;
    s->prburn_dmg[pj] = 0.0f;
    s->prburn_dur[pj] = 0.0f;
    s->prcrit_chance[pj] = 0.0f;
    s->prcrit_dmg[pj] = 0.0f;
    s->pr_expl[pj] = BT_ITEM_EFF_NONE;   /* no carried explosion until the owner names one */
    s->prsrc[pj] = 0xFFFFu;     /* no source enemy until an enemy shot names one */
    s->prx[pj] = x; s->pry[pj] = y;
    s->prvx[pj] = bt_cosf(rot) * spd;
    s->prvy[pj] = bt_sinf(rot) * spd;
    /* rotation is only written when rotate_projectile is set (shooting_attack_behavior.gd:198-199) */
    s->prdmg[pj] = dmg; s->prrot[pj] = prot; s->prage[pj] = 0.0f;
    s->prshape[pj] = (unsigned char)shape;
    s->prrad[pj] = rad; s->prhx[pj] = hx; s->prhy[pj] = hy;
    s->pralive[pj] = 1;
    if (pj >= (int)s->pr_used) s->pr_used = (unsigned short)(pj + 1);
    s->prcre[pj] = (unsigned short)(++s->pr_cre_next);   /* creation ordinal */
    return pj;
}

/* shooting_attack_behavior.gd:83-155 shoot(), with every flag from the table.
 * `A` is taken BY VALUE so a thread-local state override and a constant table
 * row share one code path without mixing MSL address spaces. */
BT_HD static void bt_fire_shot(BT_DEVICE BtState* s, int i, BtAttack A, float mvx, float mvy) {
    int nproj = A.nproj ? (int)A.nproj : 1;
    if (nproj > 24) nproj = 24;
    float ex = s->ex[i], ey = s->ey[i];
    /* enemy.gd:148-151 set_attack_behavior_damage: a ShootingAttackBehavior's
     * projectile_damage = get_final_enemy_damage(base_damage(wave), percent)
     * (brotato_enemy_modifiers.h -- the same path the contact hitbox uses). */
    float dmg = bt_enemy_final_damage(s, A.damage + A.damage_grow * (float)(s->wave - 1), 0.0f);
    if (dmg < 1.0f) dmg = 1.0f;

    float base_rand = bt_rand_range(s, -A.base_dir_rand, A.base_dir_rand);
    if (A.base_dir_const) {
        if (A.alt_base_dir_spread)
            base_rand = (s->elastspread[i] < 0.0f) ? A.base_dir_rand : -A.base_dir_rand;
        else
            base_rand = (bt_rng_bounded(&s->rng, 2u) == 0u) ? -A.base_dir_rand : A.base_dir_rand;
        s->elastspread[i] = base_rand;
    }
    float rand_rot = bt_rand_range(s, -A.random_rotation, A.random_rotation);

    float tx = s->px, ty = s->py;
    if (A.shoot_in_unit_dir) { tx = ex + mvx; ty = ey + mvy; }

    int shape = (int)A.proj_shape;
    float rad, hx, hy, sox, soy;
    bt_proj_shape_geom(shape, A.proj_radius, A.proj_half_x, A.proj_half_y, &rad, &hx, &hy, &sox, &soy);

    float base_pos = 0.0f;
    if (A.constant_spread_rand_base_pos > 0.0f)
        base_pos = bt_rand_range(s, 0.0f, A.constant_spread_rand_base_pos);

    for (int k = 0; k < nproj; k++) {
        /* get_projectile_spawn_pos */
        float posx = ex, posy = ey;
        if (A.spawn_target) { posx = tx; posy = ty; }
        if (A.spawn_only_borders) {
            float a = bt_rand_range(s, 0.0f, 2.0f * BT_PI);
            if (A.spread_const && nproj > 1) a = base_pos + (float)k * (2.0f * BT_PI / (float)nproj);
            if (A.n_degrees > 0 && k < (int)A.n_degrees &&
                (int)A.degree_first + k < BT_NDEGREES) {
                float deg = BT_ATTACK_DEGREES[(int)A.degree_first + k];
                a = deg * (BT_PI / 180.0f) + bt_atan2f(ty - ey, tx - ex);
            }
            posx += bt_cosf(a) * (float)A.spawn_spread * 0.5f;
            posy += bt_sinf(a) * (float)A.spawn_spread * 0.5f;
        } else if (!A.atleast_one_on_target || k != 0) {
            float hs = (float)A.spawn_spread * 0.5f;
            posx = bt_rand_range(s, posx - hs, posx + hs);
            posy = bt_rand_range(s, posy - hs, posy + hs);
        }

        float base_rot = bt_atan2f(ty - ey, tx - ex) + base_rand;
        float rot = bt_rand_range(s, base_rot - A.spread, base_rot + A.spread);
        float spd = (float)A.pspeed;
        if (A.random_dir) rot = bt_rand_range(s, -BT_PI, BT_PI);
        if (A.spread_const && nproj > 1) {
            float chunk = (2.0f * A.spread) / (float)(nproj - 1);
            rot = (base_rot - A.spread) + (float)k * chunk;
        }
        if (A.shoot_away_from_unit) {
            rot = bt_atan2f(posy - ey, posx - ex);
            if (rand_rot != 0.0f) rot += rand_rot;
        }
        if (A.shoot_towards_unit) rot = bt_atan2f(ey - posy, ex - posx);
        if (A.shoot_from_proj_pos) {
            float dxp = s->px - ex, dyp = s->py - ey;
            float len = bt_sqrtf(dxp * dxp + dyp * dyp);
            float ang = bt_atan2f(dyp, dxp) + rand_rot;
            float t2x = ex + bt_cosf(ang) * len;
            float t2y = ey + bt_sinf(ang) * len;
            rot = bt_atan2f(t2y - posy, t2x - posx);
        }
        if (A.speed_change_each != 0) spd += (float)A.speed_change_each * (float)k;
        float jit = (float)A.proj_speed_rand;
        if (jit > 0.0f) spd += bt_rand_range(s, -jit, jit);
        /* charm:88-91: a charmed unit's Hitbox/shots are on PET_PROJECTILES, so
         * its shots resolve on enemies (prfriendly). */
        int friendly = bt_enemy_hits_enemies(s, i);
        int pjx = bt_spawn_projectile(s, posx, posy, rot, spd, dmg, shape, rad, hx, hy,
                                      A.rotate_proj ? rot : 0.0f, friendly);
        if (pjx >= 0) s->prsrc[pjx] = (unsigned short)i;   /* dodge/riposte source */
    }
    s->eshots[i]++;
}
/* charging_attack_behavior.gd:77-107 set_target: the locked dash vector for one
 * charge, from the CURRENT player offset (dx,dy = player - self).  Called at the
 * animation start for Timing.START_SHOOT (the default) and at its `shoot` method
 * for Timing.SHOOT, so its two/three draws land in the frame the source makes
 * them.  TargetType.PLAYER (0) aims at the player itself; the two random
 * branches draw in the source's order (random_point first, then the point). */
BT_HD static void bt_charge_target_dir(BT_DEVICE BtState* s, int i, BtAttack A,
                                       float dx, float dy,
                                       BT_THREAD float* ddx, BT_THREAD float* ddy) {
    int ctarget = (int)A.charge_target;
    if (ctarget == 1) {                 /* RAND_POINT_AROUND_PLAYER */
        /* rand_size = (min(600, max_range / 5) if rand_target_size == -1 else
         * rand_target_size) as int -- integer division on max_range / 5. */
        float rs = A.rand_target_size == -1
            ? bt_floorf((float)A.max_range / 5.0f) : (float)A.rand_target_size;
        if (A.rand_target_size == -1 && rs > 600.0f) rs = 600.0f;
        float rx = bt_rand_range(s, -rs, rs);
        float ry = bt_rand_range(s, -rs, rs);
        float dl = bt_sqrtf(dx * dx + dy * dy);
        float ux = dl > 0.0f ? dx / dl : 0.0f;
        float uy = dl > 0.0f ? dy / dl : 0.0f;
        *ddx = (s->px + ux * rs + rx) - s->ex[i];
        *ddy = (s->py + uy * rs + ry) - s->ey[i];
    } else if (ctarget == 2) {          /* RAND_POINT */
        float tx, ty;
        bt_rand_pos(s, BT_EDGE_MAP_DIST, &tx, &ty);
        *ddx = tx - s->ex[i]; *ddy = ty - s->ey[i];
    } else {                            /* TargetType.PLAYER */
        *ddx = dx; *ddy = dy;
    }
}



/* Death-spawns (spawner.gd:9-18 and any kind with a die() emit): child kinds,
 * counts and radii come from BT_KIND_DEATH_SPAWN_* (generator). */

/* Death-spawns (spawner.gd:9-18 and any kind with a die() emit): child kinds,
 * counts and radii come from BT_KIND_DEATH_SPAWN_* (generator). */
BT_HD static void bt_spawn_death_children(BT_DEVICE BtState* s, float x, float y, int kind, int src,
                                         int charmed_by) {
    int n = (int)BT_KIND_NDEATH_SPAWNS[kind];
    if (n > BT_MAX_DEATH_SPAWNS) n = BT_MAX_DEATH_SPAWNS;
    for (int e = 0; e < n; e++) {
        short ck = BT_KIND_DEATH_SPAWN_KIND[kind][e];
        if (ck < 0 || ck >= BT_NKINDS) continue;
        int nb = (int)BT_KIND_DEATH_SPAWN_NB[kind][e];
        float rad = BT_KIND_DEATH_SPAWN_RADIUS[kind][e];
        for (int k = 0; k < nb; k++) {
            float cx, cy;
            bt_pos_in_area(s, x, y, rad, 0.0f, 0, &cx, &cy);
            bt_queue_summon_src(s, (int)ck, cx, cy, src, charmed_by);
        }
    }
}

/* buffer.gd:37-60 -- every BoostTimer timeout, boost up to nb_entities_boosted
 * _at_once still-unboosted enemies inside the BoostZone. All numbers come from
 * BT_KIND_BOOST (generator). enemy.gd:311-317 -> reset_health/damage/speed_stat
 * (percent) recomputes from the kind's base stats, so the boosted HP is refilled
 * to the new max. Bursting is not applied (enemy.gd:311-317). */
BT_HD static void bt_buffer_boost(BT_DEVICE BtState* s, int i, int kind) {
    BT_CONST_PTR(BtBoost) B = &BT_KIND_BOOST[kind];
    int want = B->nb_at_once ? (int)B->nb_at_once : 1;
    int n = (int)s->zone_n[i];
    /* buffer.gd:44 entities_in_zone.shuffle() -- pinned Godot's
     * Array::shuffle (core/array.cpp): `for (i = size-1; i > 0; i--)
     * { j = Math::rand() % (i+1); swap(i, j); }`, n-1 draws of the GLOBAL
     * stream (verified against the engine: seed(12345) + shuffle([0..9]) ==
     * j = randi() % (i+1)).  It shuffles the WHOLE membership list, including
     * members that are ineligible for the boost. */
    for (int k = n - 1; k > 0; k--) {
        unsigned int j = bt_rng_next(&s->rng) % (unsigned int)(k + 1);
        unsigned char tmp = s->zone_mem[i][k];
        s->zone_mem[i][k] = s->zone_mem[i][(int)j];
        s->zone_mem[i][(int)j] = tmp;
    }
    int boosted = 0;
    float w1 = (float)(s->wave - 1);
    for (int k = 0; k < n; k++) {
        int j = (int)s->zone_mem[i][k];
        if (j >= BT_MAX || !s->ealive[j] || s->eneutral[j]) continue;
        if (!bt_enemy_can_boost(s, j) || s->eboosted[j]) continue;
        s->eboosted[j] = 1;
        BT_CONST_PTR(BtUnitKind) K = bt_kunit(s, j);
        /* enemy.gd:311-317 boost -> reset_health/damage/speed_stat(percent), i.e.
         * the SAME get_final_* path recomputed from the kind BASE with the boost
         * as the percent modifier (never a multiply of the already-final stat):
         *   health = get_final_enemy_health(get_base_health(wave), hp_pct)
         *   damage = get_final_enemy_damage(get_base_damage(wave), dmg_pct)
         *   speed  = get_final_enemy_speed(stats.speed + _speed_rand_value, speed_pct)
         * (brotato_enemy_modifiers.h) */
        s->emax[j] = bt_enemy_final_health(s, bt_floorf(K->hp + K->hp_grow * w1), (float)B->hp_pct);
        s->ehp[j] = s->emax[j];
        s->edmg[j] = bt_enemy_final_damage(s, K->dmg + K->dmg_grow * w1, (float)B->dmg_pct);
        s->espd[j] = bt_enemy_final_speed(s, (float)K->speed + s->espeed_rand[j],
                                          (float)B->speed_pct);
        /* retained so a later get_angry can ADD its own percentages onto the
         * boost already in force (stargazer.gd:72-78 _boosted_args) */
        s->eboost_hp[j] = (float)B->hp_pct;
        s->eboost_dmg[j] = (float)B->dmg_pct;
        s->eboost_spd[j] = (float)B->speed_pct;
        boosted++;
        /* buffer.gd:71-73: keep boosting until nb_entities_boosted_at_once */
        if (boosted >= want) break;
    }
    (void)boosted;
}

/* --- pursuer.gd:55-73 boost_self() ---------------------------------------
 * The BoostTimer timeout raises bonus_speed by speed_on_boost (flat) and, when
 * the scene sets damage_on_boost, recomputes damage from the kind base with
 * `damage_on_boost * nb_times_boosted` percent (unit.gd reset_damage_stat), the
 * same percent path the buffer uses.  Scene values are 0 for the campaign
 * pursuer, so only the speed term is live there. */
BT_HD static void bt_enemy_self_boost(BT_DEVICE BtState* s, int i, int kind) {
    BT_CONST_PTR(BtSelfBoost) B = &BT_KIND_SELFBOOST[kind];
    /* RunData.current_run_accessibility_settings.speed == 1.0 (single-player
     * campaign defaults, see brotato_enemy_motion.h) */
    s->ebonus[i] += B->speed;
    s->enboost[i]++;
    if (B->size > 0.0f) { s->eboost_n[i]++; bt_egeom_refresh(s, i); }
    /* pursuer.gd:69-72: once nb_times_boosted reaches
     * change_movement_behavior_after_x_boosts the active MovementBehavior is
     * swapped ONCE and a flat bonus_speed_on_change_movement_behavior is added
     * (accessibility speed 1.0).  movement_behavior_changed is sticky, so a
     * later hit reset (which zeroes nb_times_boosted) does not switch back. */
    if (B->change_after >= 0 && (int)s->enboost[i] >= (int)B->change_after && !s->mv_boost[i]) {
        s->mv_boost[i] = 1;
        s->ebonus[i] += B->bonus_on_change;
    }
    if (B->dmg_pct > 0) {
        BT_CONST_PTR(BtUnitKind) K = bt_kunit(s, i);
        float w1 = (float)(s->wave - 1);
        s->edmg[i] = bt_enemy_final_damage(s, K->dmg + K->dmg_grow * w1,
                                           (float)((int)B->dmg_pct * (int)s->enboost[i]));
    }
}

/* --- fly.gd:6-11 _on_Hurtbox_area_entered reaction ------------------------
 * When a RangedWeapon (or a Pet with shoot_projectiles) hitbox enters the
 * hurtbox, Utils.get_chance_success(proj_chance) rolls ONCE on the global
 * stream and, on success, `_attack_behavior.call_deferred("shoot")` queues a
 * volley that ignores the behavior's cooldown.  The roll happens at the entry
 * (the sim's hit resolution), the volley at the deferred boundary -- the same
 * one-frame queue the player's hurt_area_entered_deferred uses (`epend`). */
BT_HD static void bt_enemy_fly_react(BT_DEVICE BtState* s, int i, int from_ranged_or_pet) {
    if (!from_ranged_or_pet) return;
    float chance = BT_KIND_PROJ_CHANCE[s->ekind[i]];
    if (chance == 0.0f) return;
    if (bt_chance(s, chance)) s->eflypend[i] = 1;
}

/* Deferred boundary for the queued reactions: the attack behavior's shoot(),
 * i.e. the same volley path the animation's method key takes, with the unit's
 * current movement vector (shoot_in_unit_direction reads it). */
BT_HD static void bt_enemy_fly_flush(BT_DEVICE BtState* s) {
    for (int i = 0; i < (int)s->e_used; i++) {
        if (!s->eflypend[i]) continue;
        s->eflypend[i] = 0;
        if (!s->ealive[i] || s->eneutral[i]) continue;
        int kind = s->ekind[i];
        if (BT_KIND_PROJ_CHANCE[kind] == 0.0f) continue;
        BT_CONST_PTR(BtAttack) atk = &BT_ATTACKS[kind];
        int ns = (int)BT_NKIND_STATES[kind];
        if (ns > 0 && (int)s->estate[i] < ns) {
            BT_CONST_PTR(BtAtkState) sst = &BT_ATTACK_STATES[kind][s->estate[i]];
            if (sst->atk.beh == BT_BEH_SHOOT) atk = &sst->atk;
        }
        if ((int)atk->beh != BT_BEH_SHOOT) continue;
        BtVec2 mv = bt_enemy_move(s, i);
        bt_fire_shot(s, i, *atk, mv.x, mv.y);
    }
}

/* --- buffer.gd:17,26-28,39,44,82-83 entities_in_zone --------------------- */
/* body_entered pushes the body when `not dead and body.can_be_boosted`;
 * body_exited erases it; die() clears the list.  Bodies are ordered by Godot's
 * body_map (ascending ObjectID) when several enter in the same step, which the
 * pool instance's object uid reproduces (it is minted at node instantiation). */
BT_HD static int bt_zone_find(BT_DEVICE BtState* s, int b, int slot) {
    for (int k = 0; k < (int)s->zone_n[b]; k++) if (s->zone_mem[b][k] == (unsigned char)slot) return k;
    return -1;
}
BT_HD static void bt_zone_insert_sorted(BT_DEVICE BtState* s, int b, int slot) {
    int n = (int)s->zone_n[b];
    if (n >= BT_MAX) return;
    int pos = n;
    for (int k = 0; k < n; k++) {
        int other = (int)s->zone_mem[b][k];
        if (other < BT_MAX && s->ealive[other] && !s->eneutral[other] &&
            s->e_object_uid[other] > s->e_object_uid[slot]) { pos = k; break; }
    }
    for (int k = n; k > pos; k--) s->zone_mem[b][k] = s->zone_mem[b][k - 1];
    s->zone_mem[b][pos] = (unsigned char)slot;
    s->zone_n[b] = (unsigned char)(n + 1);
}
BT_HD static void bt_zone_erase(BT_DEVICE BtState* s, int b, int at) {
    int n = (int)s->zone_n[b];
    for (int k = at; k + 1 < n; k++) s->zone_mem[b][k] = s->zone_mem[b][k + 1];
    if (n > 0) s->zone_n[b] = (unsigned char)(n - 1);
}
/* Entity.can_be_boosted (buffer.tscn's Buffer itself sets it false; enemy.tscn
 * sets it true and the DLC stargazer can flip it) and `not dead`: the entry
 * filter of buffer.gd:27.  bt_enemy_can_boost already models both. */
BT_HD static int bt_zone_member_ok(BT_DEVICE BtState* s, int b, int slot) {
    if (slot == b || !s->ealive[slot] || s->eneutral[slot]) return 0;
    float bx = s->ex[b], by = s->ey[b];
    BT_CONST_PTR(BtBoost) B = &BT_KIND_BOOST[s->ekind[b]];
    BT_CONST_PTR(BtBodyPhysics) G = bt_kgeom(s, slot);
    float dx = s->ex[slot] - bx, dy = s->ey[slot] - by;
    float rr = B->radius + G->body_radius;
    if (dx * dx + dy * dy > rr * rr) return 0;
    return bt_enemy_can_boost(s, slot);
}

/* Zone membership pass for one Buffer (buffer.gd body_entered/body_exited):
 * append the bodies that newly overlap in object-uid order, drop the ones that
 * left.  Runs once per frame in the enemy loop, before the timer tick. */
BT_HD static void bt_zone_update(BT_DEVICE BtState* s, int b) {
    for (int k = (int)s->zone_n[b] - 1; k >= 0; k--) {
        int slot = (int)s->zone_mem[b][k];
        if (!bt_zone_member_ok(s, b, slot)) bt_zone_erase(s, b, k);
    }
    for (int j = bt_enemy_next(s, 0, 1); j < (int)s->e_used; j = bt_enemy_next(s, j + 1, 1)) {
        if (!bt_zone_member_ok(s, b, j)) continue;
        if (bt_zone_find(s, b, j) >= 0) continue;
        bt_zone_insert_sorted(s, b, j);
    }
}


BT_HD static float bt_player_damage(BT_DEVICE BtState* s, float raw_dmg, int dodgeable,
        int attacker_slot, BT_THREAD float* reward) {
    if (s->iframe > 0.0f) return 0.0f;
    /* player.gd:302 `if dodgeable and randf() < current_stats.dodge:` -- the raw
     * global randf() draw happens whenever the hit is dodgeable, even at dodge
     * == 0 (no zero guard), and the comparison is strict `<` (bt_chance would
     * skip the draw at 0 and use `<=`), so the global stream stays aligned. */
    if (dodgeable && bt_math_randf(&s->rng) < s->p_dodge) {
        *reward += 0.05f;
        /* player.gd:438: the dodge branch still starts the timer through
         * get_iframes(dmg_taken), and a dodge makes dmg_taken == 0 -- so the
         * LOWER clamp bound (0.2 / endless factor), not a flat 0.2. */
        if (dodgeable) s->iframe = bt_iframe_frames(bt_iframes(s, 0.0f, s->p_max_hp));
        bt_combat_audio_player_hurt(s, 1);
        bt_scalar_on_dodge(s, attacker_slot);
        (void)bt_temporal_on_dodge(s);
        return 0.0f;
    }
    /* player.gd:305-308 get_damage_value: once the dodge roll has failed, a
     * remaining hit_protection charge (tardigrade) absorbs the whole hit and
     * is spent.  dmg_taken stays 0, so the hurt sound plays but no flash /
     * explode_on_hit / temp-stats-on-hit runs; iframes start on the same
     * get_iframes(0) a dodge uses. */
    if (s->hit_prot > 0) {
        s->hit_prot--;
        if (dodgeable) s->iframe = bt_iframe_frames(bt_iframes(s, 0.0f, s->p_max_hp));
        bt_combat_audio_player_hurt(s, 0);
        return 0.0f;
    }
    float taken = bt_roundf(raw_dmg * bt_armor_mult(s->p_armor));
    if (taken < 1.0f) taken = 1.0f;
    taken = bt_minf(taken, s->hp);
    s->hp -= taken;
    bt_combat_audio_player_hurt(s, 0);
    /* TempStatsPerIntervalEffect.reset_on_hit: clear accumulated temp stacks */
    for (int t = 0; t < s->n_tsi; t++) {
        if (!s->tsi_reset[t] || s->tsi_stacks[t] <= 0.0f) continue;
        bt_core_stat_add_temp(s, (int)s->tsi_id[t], -(s->tsi_val[t] * s->tsi_stacks[t]));
        s->tsi_stacks[t] = 0.0f;
    }
    /* #10 decaying_stats_on_hit (saltwater): temp stat for `value2` seconds */
    for (int a = 0; a < s->n_bk_active; a++) {
        int r = (int)s->bk_active[a];
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[r];
        if (!bt_eff_is(E, BT_ISTR_DECAYING_STATS_ON_HIT)) continue;
        int id = bt_core_id_from_istr(E->key);
        if (id == 255) continue;
        int d2 = 0;
        float dv = (float)bt_row_dominant(s, r, E, &d2);
        int found = -1;
        for (int d = 0; d < s->n_dt; d++)
            if (s->dt_id[d] == id && s->dt_onhit[d] && s->dt_dur[d] == (float)d2) { found = d; break; }
        if (found < 0) {
            if (s->n_dt >= 8) { s->stat_reg_overflow = 1; continue; }
            found = s->n_dt++;
            s->dt_id[found] = (unsigned char)id; s->dt_onhit[found] = 1;
            s->dt_val[found] = 0.0f; s->dt_dur[found] = (float)d2;
        }
        if (s->dt_val[found] != dv) {
            bt_core_stat_add_temp(s, id, dv - s->dt_val[found]);
            s->dt_val[found] = dv;
        }
        s->dt_remain[found] = (float)d2;
    }
    if (taken > 0.0f && !s->done) {
        int nb = bt_item_expl_player_nb(s);
        if (bt_character_explode_on_hit(s->character)) (void)bt_explode(s);
        if (nb > 0) (void)bt_item_expl_player_hit(s, nb);
    }
    if (taken > 0.0f) (void)bt_temporal_on_hit(s);
    s->dmg_taken += taken;
    if (dodgeable) s->iframe = bt_iframe_frames(bt_iframes(s, taken, s->p_max_hp));
    return taken;
}

#include "brotato_loot.h"
/* Structure-item runtime (Garden's is_spawning turret): bt_structure_spawning_tick,
 * called from the pet block in bt_core_step.  Needs the loot consumable helpers. */
#include "brotato_structures.h"
#include "brotato_weapon_inventory.h"
#include "brotato_weapon_shop.h"
#include "brotato_progression.h"
/* Pet motion + interaction zones (BT_PET_AI): bt_step_pet_motion. Included
 * last so it can call the whole core helper set it builds on. */
#include "brotato_pet_motion.h"
/* Enemy MovementBehavior dispatch (bt_enemy_move / bt_enemy_motion_spawn):
 * the source get_movement() vectors, driven by BT_MOV_ROOT/BT_MOV_STATE and the
 * retained BT_ENEMY_INIT_OPS draws. */
#include "brotato_enemy_motion.h"
#include "brotato_scalar_events.h"
#include "brotato_temporal_events.h"
#include "brotato_enemy_modifiers.h"
/* DLC cursed enemies (brotato_enemy_curse.h): needs bt_enemy_final_* (above) and
 * bt_loot_endless_factor (brotato_loot.h, above). */
#include "brotato_enemy_curse.h"
#include "brotato_item_explosions.h"
/* Enemy-specific lifecycle (brotato_enemy_lifecycle.h: stargazer/iron_lung and
 * evil_mob gold).  Included last: it uses bt_enemy_final_health/damage/speed
 * from brotato_enemy_modifiers.h and the movement helpers. */
#include "brotato_enemy_lifecycle.h"

BT_HD static float bt_enemy_death_gold_value(BT_DEVICE BtState* s, int slot, float base) {
    if (!s->eneutral[slot] && (BT_KIND_MOVK[s->ekind[slot]] & BT_MOVK_EVIL_MOB))
        return (float)((unsigned int)s->emob_count[slot] * (unsigned int)(s->emob_evo[slot] + 1));
    return base;
}
#include "brotato_weapon_effects.h"
#include "brotato_weapon_projectiles.h"
#include "brotato_weapon_melee.h"
#include "brotato_weapon_ranged.h"
#include "brotato_weapon_observe.h"
/* Potential-based shaping (Phi + F = gamma*Phi(s') - Phi(s)) and its ini
 * config.  Included here: it reads BT_WEAPONS/BT_ITEMS and bt_core_stat_read. */
#include "brotato_reward.h"

/* A killed enemy's damage proc completes (including any descendants) before
 * its projectile-on-death callback. This is the source signal's depth-first
 * order, expressed without recursive functions on CUDA/Metal. */
#if defined(__METAL_VERSION__)
__attribute__((noinline))
#endif
BT_HD static void bt_drain_death_events(BT_DEVICE BtState* s) {
    while (s->n_death_events > 0) {
        int event = s->n_death_events - 1;
        int enemy = (int)s->death_stack[event];
        if (s->death_phase[event] == 0) {
            s->death_phase[event] = 1;
            (void)bt_scalar_on_kill(s);
        } else {
            (void)bt_scalar_proj_on_death(s, s->ex[enemy], s->ey[enemy]);
            s->n_death_events--;
        }
    }
}

/* ---------------------------------------------------------------------------
 * Burning tick (unit.tscn BurningTimer, unit.gd:661-707).
 *
 * eburn counts the remaining FRAMES and eburndmg is the damage of one tick; the
 * timer deals damage once per bt_burn_period_frames().
 *
 * bt_burn_scan_range  order-free half: which enemies tick THIS frame (bit set)
 *                     over enemy slots [i_lo, i_hi), WITHOUT touching state.
 * bt_burn_scan        the whole range (serial reference partitioning).
 * bt_burn_tick_one    one enemy's tick body -- audio, on_burned records, damage,
 *                     the neutral resolve hook and the kill callbacks.  Order-locked
 *                     (shared RNG / death queue / loot).
 * bt_burn_tick        the serial reference: decrement + modulo test inline.
 * bt_burn_tick_cand   the same pass reading a precomputed tick set.
 * ------------------------------------------------------------------------- */
BT_HD static void bt_burn_tick_one(BT_DEVICE BtState* s, int i, BT_THREAD float* r) {
    bt_combat_audio_enemy_burn_tick(s, (int)s->ekind[i], (int)s->eneutral[i]);
    /* unit.gd:661-668: the effect behaviors' on_burned runs BEFORE the
     * burn tick's damage, with the BurningData's own scaling stats (the
     * resource default stat_elemental_damage), so a burn tick applies
     * and benefits from enemy_percent_damage_taken in the same tick. */
    bt_enemy_hit_burn_apply(s, i);
    /* get_damage_value (TakeDamageArgs.armor_applied == false for the
     * burn tick): round(dmg * (1 + total_bonus_damage/100)), no armor. */
    float burn_v = bt_enemy_incoming_damage(s, i, s->eburndmg[i]);
    burn_v = bt_roundf(burn_v * (1.0f + (float)bt_enemy_hit_percent_taken(s, i) * 0.01f));
    s->ehp[i] -= burn_v;
    /* unit.gd:305-306 max(0.0, health - value): the game's health floors at 0 */
    if (s->ehp[i] < 0.0f) s->ehp[i] = 0.0f;
    int neutral_death = bt_neutral_resolve_hit(s, i, -1, 0);
    if (s->ehp[i] <= 0.0f || neutral_death) {
        /* curse_enemy_effect_behavior.gd:24-41 pays gold_on_cursed_enemy_kill
         * on a LETHAL hit whose from_player_index != -1; the burn tick's
         * TakeDamageArgs carries Unit._burning_player_index (unit.gd:669).
         * Every burn in this core is player-0 sourced, so that owner is
         * always 0: bt_apply_enemy_burn's only callers are the player
         * hit paths -- bt_item_expl_hit_enemy from the weapon / item /
         * pet / turret hits (all pass from_player_index 0; the one -1
         * caller, the charmed-enemy contact block above, passes
         * burn_chance 0 so it never burns) and bt_wfx_weapon_burn_apply
         * (a player weapon) -- and this tick, which is the only -1
         * from_player_index caller, never applies a burn.
         * (apply_burning's weaker-replacement ownership rule,
         * unit.gd:624-646, is therefore vacuous: there is one owner.)
         * Neutral targets are excluded with the source's curse domain. */
        if (!s->eneutral[i]) (void)bt_enemy_curse_player_kill_gold(s, i);
        /* unit.gd:344-356: the burn tick's die() is deferred too, and it is
         * drained in the flush's Timer rank -- after every area hit, before the
         * area hits' own dies (see bt_queue_burn_kill). */
        bt_queue_burn_kill(s, i);
        *r += R_KILL;
    }
}

/* Order-free half: bit (i>>5, i&31) of tick_words -- CEIL(BT_MAX/32) unsigned
 * ints the CALLER zeroes -- for every live burning enemy in [i_lo, i_hi) that
 * ticks this frame.  Mutates nothing. */
BT_HD static void bt_burn_scan_range(BT_DEVICE BtState* s, BT_DEVICE unsigned int* tick_words,
                                     int i_lo, int i_hi) {
    if (i_lo < 0) i_lo = 0;
    if (i_hi > (int)s->e_used) i_hi = s->e_used;
    int burn_per = (int)bt_burn_period_frames(s);
    for (int i = bt_enemy_next(s, i_lo, 1); i < i_hi; i = bt_enemy_next(s, i + 1, 1)) {
        if (!s->ealive[i] || s->eburn[i] <= 0.0f) continue;
        int rem = (int)s->eburn[i] - 1;
        if (rem < 0) rem = 0;
        if (burn_per < 1 || rem % burn_per != 0) continue;
        tick_words[i >> 5] |= 1u << (i & 31u);
    }
}

/* The whole enemy range (the serial reference partitioning). */
BT_HD static void bt_burn_scan(BT_DEVICE BtState* s, BT_DEVICE unsigned int* tick_words) {
    bt_burn_scan_range(s, tick_words, 0, s->e_used);
}

/* `tick_words` non-NULL = a precomputed bt_burn_scan mask (the modulo test is
 * then a bit read); NULL = the serial reference.  Either way eburn is
 * decremented here, exactly once per live burning enemy. */
BT_HD static void bt_burn_tick_cand(BT_DEVICE BtState* s, BT_CONST_PTR(unsigned int) tick_words,
                                    BT_THREAD float* r) {
    int burn_per = (int)bt_burn_period_frames(s);
    for (int i = bt_enemy_next(s, 0, 1); i < (int)s->e_used; i = bt_enemy_next(s, i + 1, 1)) {
        if (!s->ealive[i] || s->eburn[i] <= 0.0f) continue;
        int rem = (int)s->eburn[i] - 1;
        if (rem < 0) rem = 0;
        s->eburn[i] = (float)rem;
        int fires = (burn_per >= 1 && (rem % burn_per) == 0);
        if (fires && tick_words) fires = (int)((tick_words[i >> 5] >> (i & 31u)) & 1u);
        if (!fires) continue;
        bt_burn_tick_one(s, i, r);
    }
}

/* Serial reference: the modulo test is evaluated inline (tick_words == NULL). */
BT_HD static void bt_burn_tick(BT_DEVICE BtState* s, BT_THREAD float* r) {
    bt_burn_tick_cand(s, 0, r);
}

/* ---------------------------------------------------------------------------
 * Item / structure / burning / explosion phase of one combat tick.
 *
 * This is the whole phase in the serial core's own order and is THE definition
 * of it: bt_core_step calls exactly this.  A parallel backend (e.g.
 * ocean/brotato/brotato_gpu_itemsfx.cuh) overrides the BT_ITEMSFX_PHASE macro
 * BEFORE including brotato_core.h to substitute its own schedule of the same
 * per-item functions; the default expansion is this serial function, so a
 * backend that does nothing keeps the byte-identical original behaviour.
 * ------------------------------------------------------------------------- */
BT_HD static void bt_itemsfx_phase_serial(BT_DEVICE BtState* s, BT_THREAD float* r) {
    bt_structure_spawning_tick(s);
    (void)bt_item_expl_landmine_tick(s);
    /* The burning tick is NOT here: its Timer runs in the internal pass, BEFORE
     * the group pass -- see BT_TIMER_PHASE in bt_core_step.  Removing it from
     * this phase (and from the parallel itemsfx schedule that mirrors it) is what
     * keeps ONE tick per frame. */
    (void)bt_item_expl_tick(s, r);
}

/* ---------------------------------------------------------------------------
 * Step-scheduling hooks (FALLBACK definitions).
 *
 * bt_core_step is a sequence of spans. A span is either SERIAL - exactly one
 * lane may execute it, because it does read-modify-write on shared
 * scalars/queues/counters (s->rng, episode_return, pool ordinals, the reward
 * ledger) and concurrent RMW is a data race under independent thread scheduling
 * and the C++ memory model, even when every writer computes the same value - or
 * COOP - all lanes of the group enter the phase together and cooperate, with a
 * group barrier released at the boundary.
 *
 * These defaults make every span run inline with no barrier: exactly what the
 * CPU build, the Metal build, the CPU trainer, the land gate and the plain
 * one-thread-per-env CUDA kernel need. They are CPU-NEUTRAL by construction
 * (BT_SERIAL(x) is just `x` in its original position, so no operation is
 * reordered and no FP/RNG result changes). A GPU dispatcher defines the real
 * forms BEFORE including this header and these are skipped; see
 * ocean/brotato/brotato_gpu_framework.cuh (host-armed btf_group_bp: 1 = every
 * thread is a leader, 32 = one warp per env with lane 0 owning every serial
 * span).
 * ------------------------------------------------------------------------- */
#ifndef BTF_IS_LEADER
#define BTF_IS_LEADER() 1
#endif
#ifndef BT_SERIAL
#define BT_SERIAL(...) do { __VA_ARGS__ } while (0)
#endif
#ifndef BT_COOP
#define BT_COOP(...) do { __VA_ARGS__ } while (0)
#endif

/* The default bodies below are the serial core functions, so each runs on lane 0
 * only (BT_SERIAL) inside the cooperative phase boundary that bt_core_step opens
 * with BT_COOP. A backend that pre-defines the macro (ocean/brotato/
 * brotato_gpu_combat.cuh, brotato_gpu_itemsfx.cuh) replaces the whole thing. */
#ifndef BT_ITEMSFX_PHASE
#define BT_ITEMSFX_PHASE(s, r) BT_SERIAL( bt_itemsfx_phase_serial((s), (r)); )
#endif
/* Timer rank of the frame: unit.tscn's BurningTimer is a physics Timer whose
 * `timeout` runs in SceneTree::iteration's "physics_process_internal" pass --
 * BEFORE the "physics_process" group pass and before that iteration's
 * MessageQueue::flush (scene_tree.cpp:603-609).  bt_core_step calls this before
 * its group pass, so the burn tick's take_damage lands before every
 * Unit._physics_process and before the deferred area hits, and its
 * call_deferred("die") is queued ahead of the dies the flush itself queues
 * (unit.gd:344-356) -- bt_queue_burn_kill / bt_drain_kill_q keep that rank.  A
 * backend with a parallel burn schedule pre-defines BT_TIMER_PHASE; it must NOT
 * also tick the burn inside its BT_ITEMSFX_PHASE, or the frame burns twice. */
#ifndef BT_TIMER_PHASE
#define BT_TIMER_PHASE(s, r) BT_SERIAL( bt_burn_tick((s), (r)); )
#endif

/* Queue one enemy/neutral shot -> PLAYER pair for the next frame's flush.  Same
 * fail-closed contract as the projectile half: a pair that does not fit is
 * DROPPED and BT_CAP_HITQ latches (never a silent reorder or a wrapped count).
 * The shot is NOT stopped here -- the source's `stop()` runs from its
 * `hit_something` signal, which the deferred handler emits at the flush
 * (enemy_projectile.gd:12-13). */
BT_HD static void bt_player_hitq_enqueue(BT_DEVICE BtState* s, int i) {
    if ((int)s->hitq.np >= BT_HITQ_CAP) {
        s->hitq.overflow = 1;
        BT_CAP_LATCH(s, BT_CAP_HITQ);
        return;
    }
    int q = (int)s->hitq.np++;
    s->hitq.pa[q] = (short)i;
    s->hitq.pa_cre[q] = s->prcre[i];
}

/* --- #18 the enemy/neutral projectile list step (projectile.gd:28-64).
 *
 * Extracted verbatim from bt_core_step so the harness and the intra-env
 * parallel schedule can run it standalone. `first_hit` is an optional
 * per-projectile hint (a pre-step snapshot lower bound on the first enemy slot
 * a *friendly* shot's Hitbox can touch; -1 = none). It is a pure speed-up: the
 * body re-tests every j from first_hit[i] on, and every skipped j had a false
 * predicate at the snapshot that cannot become true during the step (enemy
 * positions are frozen for the whole step and `ealive` only loses entries).
 * The serial reference is bt_eproj_phase(s, r, 0). --- */
BT_HD static void bt_eproj_phase(BT_DEVICE BtState* s, BT_THREAD float* r,
                                 const int* first_hit) {
    for (int i = 0; i < (int)s->pr_used; i++) {
        if (!s->pralive[i]) continue;
        s->prx[i] += s->prvx[i] * BT_DT;
        s->pry[i] += s->prvy[i] * BT_DT;
        s->prage[i] += BT_DT;
        int sh = (int)s->prshape[i];
        float tstop = bt_proj_shape_stop(sh);
        if (tstop >= 0.0f && s->prage[i] >= tstop) { s->pralive[i] = 0; continue; }
        /* projectile.gd:43-44 destroy_on_leaving_screen tests
         * ZoneService.current_zone_max_camera_rect.has_point(position), i.e.
         * Rect2(-96,-192,2240,1824) for the campaign zone -- NOT the
         * 0..BT_ARENA_W/H box (brotato_combat.h has the full derivation). */
        if (bt_proj_shape_leaves(sh) && !bt_cam_rect_has_point(s->prx[i], s->pry[i])) {
            s->pralive[i] = 0;
            continue;
        }
        if (!bt_proj_shape_active(sh, s->prage[i])) continue;
        if (s->prfriendly[i]) {
            /* pet/turret shot: damage enemies */
            int jstart = first_hit ? first_hit[i] : 0;
            if (jstart < 0) jstart = BT_MAX;   /* snapshot found no candidate */
            for (int j = bt_enemy_next(s, jstart, 1); j < (int)s->e_used; j = bt_enemy_next(s, j + 1, 1)) {
                if (!s->ealive[j] || !bt_proj_hits_enemy(s, i, j)) continue;
                /* fly.gd:6-11: the target's Hurtbox area_entered handler runs
                 * BEFORE the deferred damage (unit.gd:610-611 defers the hurt),
                 * so a shooting Pet's shot rolls the reaction chance here,
                 * ahead of every damage draw below. */
                bt_enemy_fly_react(s, j, (int)s->pr_pet[i]);
                /* unit.gd:552-578: a hitbox carrying an ExplodingEffect (the
                 * rocket turret's shot, turret.gd:115) spawns the explosion
                 * AT THE TARGET and deals no direct damage. */
                if (bt_item_expl_projectile_hit(s, i, j)) { s->pralive[i] = 0; break; }
                /* unit.gd:285-300 + get_damage_value + boss.gd:52-58: the shared
                 * enemy-hit primitive applies G1 on the PRE-crit value, the
                 * per-target crit, the above-hp bonus %, FLAT armor and the
                 * neutral resolve hook.  The shot's own burning_data
                 * (weapon_service.gd:296-333, carried by prburn_*) rides the same
                 * call; a pet shot carries no ChanceStatDamageEffect proc, so
                 * proc is 0.  NOTE: the burn roll is deliberately left INSIDE the
                 * primitive (applied after the damage) because moving it before
                 * the crit roll shifts the stored burn frame count by one
                 * (tests/test_brotato_curse_weapons.cpp); reordering needs the
                 * engine frame semantics of the burn tick, tracked separately. */
                int is_crit = (s->prcrit_chance[i] > 0.0f &&
                               (float)bt_rng_bounded(&s->rng, 1000u) * 0.001f <
                               s->prcrit_chance[i]) ? 1 : 0;
                int k = 0;
                /* unit.gd:348: the pet/turret shot's Hitbox carries
                 * knockback_direction = its travel direction (weapon_service.gd
                 * :360) with the sim's unmodelled amount treated as 0, so die()
                 * seeds the death drift with the 0.2/0.8 weighted direction at
                 * MIN_DEATH_KNOCKBACK floor.  The victim sample is the lagged
                 * Node transform (s->ehx/s->ehy), as in bt_melee_knockback. */
                {
                    BtVec2 hbdir = bt_v2normalized(bt_v2(s->prvx[i], s->prvy[i]));
                    BtVec2 dir = bt_knockback_direction_weighted(hbdir, bt_v2(s->px, s->py),
                                                                bt_v2(s->ehx[j], s->ehy[j]));
                    BtVec2 dkb = bt_death_knockback(dir, 0.0f);
                    s->edkbx[j] = dkb.x; s->edkby[j] = dkb.y;
                }
                (void)bt_item_expl_hit_enemy(s, j, s->prdmg[i], is_crit, s->prcrit_dmg[i],
                                             s->prburn_chance[i], s->prburn_dmg[i],
                                             s->prburn_dur[i], 0.0f, 0.0f, 0, 1, &k, 0);
                if (k) *r += R_KILL;
                s->pralive[i] = 0;
                break;
            }
            continue;
        }
        if (bt_proj_hits_player(s, i)) {
            /* The Area2D pair is only REPORTED here; the player's own handler is
             * deferred (unit.gd:610-611 via unit.tscn's Hurtbox connection), so
             * this frame neither damages nor stops the shot: both land at the
             * next frame's flush (bt_player_hitq_apply), exactly like the
             * contact hit.  Every shot that reports the player this frame is
             * queued (there is no one-hit-per-frame gate: the source runs one
             * deferred handler per reported pair, and the iframe guard inside
             * bt_player_damage is what turns the later ones away). */
            bt_player_hitq_enqueue(s, i);
        }
    }
}

/* --- #20 player/enemy contact. Two independent steps: (a) apply the hit the
 * previous step queued (RNG + serial, uses bt_player_damage), (b) probe this
 * step's pair and queue the next. The probe is a pure, order-free scan -- the
 * LOWEST matching slot wins -- so a lane group can evaluate candidates in
 * parallel and take the min index (btf_combat_phase_player_contact), while the
 * serial reference below runs the same probe in slot order. Extracted verbatim
 * from bt_core_step. --- */
/* The PLAYER half of the frame's MessageQueue::flush: every enemy/neutral
 * projectile that reported the player at the previous step's
 * Physics2DServer::step resolves here, in report order (the slot scan, which is
 * the serial reference for the physics server's pair order).  This is the same
 * flush the contact hit above uses -- the source runs ONE
 * `deferred_hurt_handler` queue for both, and the player's Hurtbox handler is
 * the same unit.gd one an enemy's is.  A pair whose shot already returned to
 * the pool (unit.gd:537 `hitbox.active` == `pralive`) is dropped, and the hit
 * stops its shot whether or not it dealt damage (`EnemyProjectile.
 * _on_Hitbox_hit_something` -> `stop()`, enemy_projectile.gd:12-13, runs from
 * the hitbox's `hit_something` at the END of the handler). */
BT_HD static void bt_player_hitq_apply(BT_DEVICE BtState* s, BT_THREAD float* r) {
    int n = (int)s->hitq.np;
    s->hitq.np = 0;
    /* one victim (the player), so the resolution order is the HITBOX creation
     * order: the bridge's `_hurt_before` key with the victim term constant. */
    for (int a = 1; a < n; a++) {
        unsigned short cr = s->hitq.pa_cre[a]; short pi = s->hitq.pa[a];
        int b = a - 1;
        while (b >= 0 && s->hitq.pa_cre[b] > cr) {
            s->hitq.pa_cre[b + 1] = s->hitq.pa_cre[b]; s->hitq.pa[b + 1] = s->hitq.pa[b];
            b--;
        }
        s->hitq.pa_cre[b + 1] = cr; s->hitq.pa[b + 1] = pi;
    }
    for (int q = 0; q < n; q++) {
        int i = (int)s->hitq.pa[q];
        if (i < 0 || i >= BT_MAX_PROJ) continue;
        if (!s->pralive[i]) continue;  /* `hitbox.active` false: stopped/returned */
        float taken = bt_player_damage(s, s->prdmg[i], 1, (int)s->prsrc[i], r);
        s->pralive[i] = 0;             /* enemy_projectile.gd:12-13: stop on hit */
        if (taken > 0.0f) *r += R_DMG * taken;
    }
}

BT_HD static void bt_player_contact_apply(BT_DEVICE BtState* s, BT_THREAD float* r) {
    if (s->epend < 0) return;
    int i = (int)s->epend;
    s->epend = -1;
    if (s->ealive[i]) {
        float taken = bt_player_damage(s, s->edmg[i], 1, i, r);
        if (taken > 0.0f) {
            *r += R_DMG * taken;
            /* enemy.gd:321-322 _on_hit_something: decaying slow on contact */
            float dec = bt_maxf(BT_ENEMY_HIT_DECAY_MIN, s->espd[i] * BT_ENEMY_HIT_DECAY_PCT);
            s->edecay[i] = bt_add_decaying_speed((int)(-dec), s->espd[i], s->edecay[i]);
        }
        /* pursuer.gd:94-99 _on_hit_something: the base handler above (Enemy's
         * decaying slow) then, when the scene sets
         * reset_nb_times_boosted_on_hit, nb_times_boosted = 0 and
         * bonus_speed = 0 -- the WHOLE bonus, not just the self-boost part (the
         * campaign pursuer sets the flag true; dlc_1 viperfish sets it false).
         * The hitbox signal fires on the hurt handler's completion, so this runs
         * whether or not the damage was dodged/absorbed (unit.gd:612). */
        BT_CONST_PTR(BtSelfBoost) SB = &BT_KIND_SELFBOOST[s->ekind[i]];
        if (SB->max_boosts > 0 && SB->reset_on_hit) {
            s->enboost[i] = 0;
            s->ebonus[i] = 0.0f;
        }
    }
}

/* One flush: the contact hit, then the projectile hits (their pairs were formed
 * earlier in the frame than the contact probe's).  This is the default body of
 * the BT_CONTACT_APPLY_PHASE hook and the serial reference for it. */
BT_HD static void bt_player_hurt_flush(BT_DEVICE BtState* s, BT_THREAD float* r) {
    bt_player_contact_apply(s, r);
    bt_player_hitq_apply(s, r);
}

/* Does enemy `i`'s contact Hitbox overlap the player Hurtbox this step? Pure:
 * reads only the step's frozen enemy snapshot (ehx/ehy), the player Hurtbox and
 * the static geometry. */
BT_HD static int bt_player_contact_probe(BT_DEVICE BtState* s, int i) {
    /* RunData.invulnerable: the player's Hurtbox is disabled (player.gd:114-115
     * disable_hurtbox), so the enemy's contact Hitbox never reports it -- the
     * overlap test is false for every enemy and NO hit (hence no iframe/dodge
     * roll, no decaying-slow on the enemy) is registered.  Pure early-out: the
     * tag does not touch the step's physics or RNG. */
    if (s->invulnerable) return 0;
    if (!s->ealive[i] || s->esup[i]) return 0;
    /* charm:88: a charmed unit's Hitbox is on PET_PROJECTILES, invisible
     * to the player's Hurtbox mask (player.tscn 20). */
    if (bt_enemy_charmed(s, i)) return 0;
    BT_CONST_PTR(BtBodyPhysics) G = bt_kgeom(s, i);
    if (!G->has_hitbox) return 0;
    float hurt_x = s->px, hurt_y = s->py + BT_PLAYER_HURT_Y;
    float hx = s->ehx[i] + G->hit_x, hy = s->ehy[i] + G->hit_y;
    float dx = hx - hurt_x, dy = hy - hurt_y;
    float rr = G->hit_radius + BT_PLAYER_HURT_R;
    return (dx * dx + dy * dy <= rr * rr);
}

BT_HD static void bt_player_contact_phase(BT_DEVICE BtState* s, BT_THREAD float* r) {
    /* the same flush the BT_CONTACT_APPLY_PHASE hook runs (a no-op for the
     * parts the earlier call already consumed) */
    bt_player_hurt_flush(s, r);
    for (int i = bt_enemy_next(s, 0, 1); i < (int)s->e_used; i = bt_enemy_next(s, i + 1, 1)) {
        if (bt_player_contact_probe(s, i)) { s->epend = (short)i; break; }
    }
}

/* Further `death`-clip advances until its method key fires: the first m >= 1
 * whose position reaches the 1.0 s clip length, in the SAME float32 arithmetic
 * the AnimationPlayer uses (`pos + playback_speed * delta`, delta = the fixed
 * 1/60 idle step).  With playback_speed 1 the float32 sum of sixty 1/60 steps is
 * 0.99999970, so m = 61 -- which is exactly why a hit kill observes its node for
 * 60 rows (it starts one advance in) and an idle-pass kill for 61.  Used to
 * rescale a dying node's window when the clip's playback_speed changes. */
BT_HD static int bt_death_frames_left(float pos, float speed) {
    int m = 0;
    while (pos < 1.0f && m < BT_POOL_DEATH_FRAMES + BT_POOL_DEATH_IDLE_EXTRA + 1) {
        pos = pos + speed * BT_DT;
        m++;
    }
    return m;
}

/* One frame of a killed unit's death animation, run exactly where the live
 * enemy loop runs (its velocity for the NEXT step is what the loop caches) and
 * exactly ONCE per frame -- the wave-end window does not tick it again.
 * Entity.death_animation_finished -> free_entity -> add_node_to_pool removes
 * the node after that many frames of the animation (entity.tscn's `death`
 * method key at t=1.0 s): BT_POOL_DEATH_FRAMES for a death the engine
 * registered from the physics pass, one more (BT_POOL_DEATH_IDLE_EXTRA) for one
 * it registered from the IDLE pass, which misses the kill frame's own
 * AnimationPlayer advance -- see bt_enemy_clean_up_room.  Until then the node
 * keeps moving: Unit._physics_process still runs (Godot dispatches the script
 * virtual multilevel) with _can_move false, so its only velocity is
 * get_next_knockback_value() -- the stored knockback_vector decayed by
 * linear_interpolate(ZERO, 0.1) once per frame and
 * scaled by 100 - resistance*100 (unit.gd:230-239).  Enemy._physics_process
 * returns early on dead, so no attack/target work happens.  The node's
 * CollisionShape and Hurtbox are disabled, so this integrates the drift
 * directly instead of through the contact solver.
 *
 * The clip itself advances here too: AnimationPlayer defaults to
 * playback_process_mode = IDLE and moves `current_animation_position` by
 * playback_speed * delta per idle frame in float32 (animation_player.cpp
 * `PlaybackData::pos`, `_animation_process_data`: delta = p_delta * speed_scale
 * * cd.speed_scale, next_pos = pos + delta, clamped to the length), and the
 * method key at t=1.0 fires on the first frame whose advance reaches the
 * length, in that same idle pass's deferred flush.  `edying` is the resulting
 * OBSERVABLE-frame countdown (it already carries the kill-path offset), so
 * edpos/edspd only have to answer "when does the window END": the position is
 * advanced with the current speed, and a writer that changes that speed in this
 * frame rescales the countdown from the position reached.  Only one writer can
 * still fire on a dead node -- a behavior Timer, i.e. a plain child node:
 * charging_attack_behavior.gd:124-127 (`on_unlock_move_timer_timeout`) sets
 * playback_speed = _idle_playback_speed.  Its clock is `echarge` (the same
 * unlock Timer, in frames), which the live loop stops ticking at the kill, so
 * it is ticked here for as long as the dash's timer is still running. */
BT_HD static void bt_enemy_dying_step(BT_DEVICE BtState* s) {
    for (int j = bt_enemy_next(s, 0, 2); j < (int)s->e_used; j = bt_enemy_next(s, j + 1, 2)) {
        if (s->edying[j] == 0) continue;
        /* this frame's displacement: the velocity cached last frame (for the
         * death frame that is still the live body's cached velocity, which is
         * why the node moves exactly like a live unit one more step). */
        float nx = s->ex[j] + s->edvx[j] * BT_DT;
        float ny = s->ey[j] + s->edvy[j] * BT_DT;
        /* unit.gd:_integrate_forces clamps the next position into the zone
         * rect; the arena is the whole zone for every shipped wave. */
        if (nx < 0.0f) nx = 0.0f; else if (nx > BT_ARENA_W) nx = BT_ARENA_W;
        if (ny < 0.0f) ny = 0.0f; else if (ny > BT_ARENA_H) ny = BT_ARENA_H;
        s->ex[j] = nx; s->ey[j] = ny;
        /* cache the NEXT step's velocity: the death knockback, decayed once. */
        BT_CONST_PTR(BtBodyPhysics) G = bt_kgeom(s, j);
        BtKnockbackTick tick = bt_knockback_advance(bt_v2(s->edkx[j], s->edky[j]),
                                                    G->knockback_resistance);
        s->edkx[j] = tick.stored.x; s->edky[j] = tick.stored.y;
        s->edvx[j] = tick.velocity.x; s->edvy[j] = tick.velocity.y;
        /* this frame's idle-pass advance of the `death` clip. */
        s->edpos[j] = s->edpos[j] + s->edspd[j] * BT_DT;
        if (s->echarge[j] > 0.0f) {
            s->echarge[j] -= 1.0f;
            if (s->echarge[j] <= 0.0f) {
                /* the Timer's timeout ran in THIS frame's idle pass, AFTER the
                 * AnimationPlayer's advance, so the new speed takes effect from
                 * the next advance and the frames left are rescaled from the
                 * position reached (measured: seed-1 wave-20 predator, timer
                 * 0.366667 s = 22 frames left at the kill row, expires on row
                 * 261 at position 0.366667, playback_speed -> 2.398599, node
                 * gone on row 277 = kill 239 + 38 instead of 61). */
                s->edspd[j] = s->edidl[j];
                bt_enemy_set_dying(s, j, (unsigned char)bt_death_frames_left(s->edpos[j], s->edspd[j]));
                continue;   /* this frame is counted by the rescaled window */
            }
        }
        if (--s->edying[j] == 0) {
            bt_enemy_set_dying(s, j, 0);
            /* the animation method key fires: free_entity()/add_node_to_pool
             * takes the node out of the tree, so the observation stops seeing
             * it from this frame on. */
            s->edvx[j] = 0.0f; s->edvy[j] = 0.0f;
            s->edkx[j] = 0.0f; s->edky[j] = 0.0f;
            s->edpos[j] = 0.0f; s->edspd[j] = 0.0f;
        }
    }
}

/* Overridable phase hooks. The default (`bt_core_step` as written in the core)
 * is the serial path; an includer that wants a phase routed through the
 * intra-env parallel schedule pre-defines these before including this header
 * (ocean/brotato/brotato_gpu_combat.cuh does exactly that, mirroring the
 * bt_phys_step hook in brotato_gpu_framework.cuh). Defining them elsewhere
 * changes NOTHING when no parallel context is armed. */
#ifndef BT_EPROJ_PHASE
#define BT_EPROJ_PHASE(s, r) BT_SERIAL( bt_eproj_phase((s), (r), 0); )
#endif
#ifndef BT_WPROJ_PHASE
#define BT_WPROJ_PHASE(s, r) BT_SERIAL( (void)bt_wproj_step((s), (r)); )
#endif
#ifndef BT_CONTACT_PHASE
#define BT_CONTACT_PHASE(s, r) BT_SERIAL( bt_player_contact_phase((s), (r)); )
#endif
/* The (serial, RNG-consuming) apply half of the contact phase, hoisted on its
 * own because bt_core_step calls it BEFORE BT_ITEMSFX_PHASE: the engine's
 * MessageQueue::flush (SceneTree::iteration, scene/main/scene_tree.cpp:609)
 * runs the queued hurt_area_entered_deferred BEFORE Physics2DServer::step()
 * resolves that frame's Area2D pairs (main/main.cpp:2383-2410), and the
 * explosion a Bull hurt spawns must therefore already exist when the frame's
 * explosion tick runs. A backend that pre-defines BT_CONTACT_PHASE for the
 * parallel probe may leave this default in place: bt_player_contact_phase then
 * calls bt_player_contact_apply again, which is a no-op because epend was
 * already consumed here. */
#ifndef BT_CONTACT_APPLY_PHASE
#define BT_CONTACT_APPLY_PHASE(s, r) BT_SERIAL( bt_player_hurt_flush((s), (r)); )
#endif

/* WaveTimer's TickTimer, modelled here because its timeout is a global RNG
 * consumer: ui/hud/wave_timer.gd:14-17 starts the (repeating, wait_time 1.0;
 * main.tscn:882 declares no properties) TickTimer on the first frame the wave
 * clock reads below 6.0 s, and :29-31 plays
 * SoundManager.play(Utils.get_rand_element(tick_sounds), 0, 0.1, true) on every
 * timeout.  `tick_sounds` is the scene's OWN one-element export array
 * (ui/sounds/clock_tick_01.wav), not one of the sound arrays the bridge empties
 * for audio_rng == 0: the Array.pick_random draw is therefore ONE global word in
 * BOTH audio modes, exactly like the player's step_sounds pick in
 * bt_player_visual_emit_steps.  The `true` is always_play, the main queue's
 * force-admit path; the playback pitch itself is drawn only under
 * audio_rng == 1 (inside bt_main_audio_step).  Godot's Timer fires on the
 * decrement that takes time_left negative (scene/main/timer.cpp:65-78), i.e.
 * every 60 frames for wait_time 1.0 with the f32 physics delta
 * ((float)(1/60) * 60 > 1.0, the same arithmetic bt_timer_frames pins), and
 * `time_left += wait_time` keeps that spacing on every repeat. */
#define BT_TICK_SOUND_COUNT 1u
/* ui/sounds/clock_tick_01.wav: 48000 Hz, 2 channels, 16-bit, 96160 data bytes
 * -> AudioStreamSample::get_length() = float(96160 / (2 * 2)) / 48000; the
 * literal below is that value's exact binary32 (0x3F00369D). */
#define BT_TICK_SOUND_DURATION 0.5008333325386047f
#define BT_TICK_TIMER_TICKS 60

BT_HD static void bt_wave_tick_sound(BT_DEVICE BtState* s) {
    (void)bt_rng_bounded(&s->rng, BT_TICK_SOUND_COUNT);
    bt_main_audio_enqueue_force(s, BT_TICK_SOUND_DURATION, BT_AUDIO_PITCH_RANGE_01);
}

/* One frame of that timer (WaveTimer._physics_process + the TickTimer's own
 * internal physics process, which run later in the frame than the spawner's
 * drain pass).  The frame the wave clock expires stops the timer instead of
 * firing it (WaveTimer.stop() runs in that same timeout frame, and no tick
 * sound is drawn on it). */
BT_HD static void bt_wave_tick_timer_step(BT_DEVICE BtState* s) {
    if (s->time_left <= 0.0f) return;
    if (s->tick_sound_left == 0) {
        if (s->time_left < 6.0f && s->time_left > 0.1f) s->tick_sound_left = BT_TICK_TIMER_TICKS;
        else return;
    }
    s->tick_sound_left--;
    if (s->tick_sound_left == 0) {
        bt_wave_tick_sound(s);
        s->tick_sound_left = BT_TICK_TIMER_TICKS;
    }
}

/* One tick. action: 0 hold/1..8 move during combat; 0..3 buy, 4 reroll in the
 * shop (8 skips); 0..3 chooses a queued end-of-wave upgrade. */
BT_HD void bt_core_step(BT_DEVICE BtState* s, int action, BT_DEVICE float* obs,
                        BT_THREAD float* reward, BT_THREAD float* terminal) {
    float r = 0.0f;
    /* Frame boundary: the deferred MessageQueue flush of the RunData
     * _physics_process emit and the StatsManager _physics_process refresh of the
     * previous frame (bt_player_stat_sync_step).  Runs before the shop/level-up
     * early return so the pipeline advances in every phase, exactly like the
     * RunData autoload's _physics_process. */
    BT_SERIAL( bt_player_stat_sync_step(s); );
    BT_SERIAL( if (s->hp < 0.0f) s->hp = 0.0f; );

    /* Potential-based shaping, start-of-transition Phi (see brotato_reward.h).
     * bt_reward_phi writes the per-group decomposition into rw_p0; the matching
     * F = gamma*Phi(s') - Phi(s) is added at each exit below. With no configured
     * weights bt_reward_shaping returns exactly 0.0f. */
    float rw_p0[BT_RW_NTERMS];
    (void)bt_reward_phi(s, rw_p0);

    /* A GO pressed on a previous shop frame.  base_shop.gd:339-349 ends the
     * handler chain in get_tree().change_scene(MenuData.game_scene), and
     * SceneTree::change_scene_to (scene/main/scene_tree.cpp:1550-1558) defers it
     * with call_deferred("_change_scene", ...): the new Main is instanced right
     * away but only ENTERS THE TREE in SceneTree::iteration's MessageQueue flush
     * (scene/main/scene_tree.cpp:605-607), i.e. after that frame's
     * `physics_process` walk.  So the GO frame's physics belongs to the scene
     * being torn down -- the Shop, which has no player and no arena -- and every
     * node of the freshly built Main (the Player, its Weapon nodes, the spawner,
     * the enemy pool) runs its first `_physics_process` in the NEXT frame.  This
     * tick is that GO frame: it reproduces the scene swap and NO arena frame,
     * exactly like bt_core_reset for a run's first wave.
     *
     * The swap does carry the new scene's idle (`_process`) phase into this
     * frame's row, because that phase runs after the flush: Godot's Timer
     * defaults to TIMER_PROCESS_IDLE, so the freshly started WaveTimer has
     * already ticked once when the next frame observes it (measured on
     * v5_demon_s1 wave 2: row 309 reads wave_time_left = duration - 1/60 with a
     * weapon cooldown of 1.0 -- the untouched wave-begin draw -- and row 310 is
     * the first row showing one decrement, 12/13).  Consuming exactly one clock
     * frame here keeps obs[66] on the game's timer and leaves tick_in_wave at 0
     * for the wave's first arena frame, i.e. the same (time_left, tick_in_wave)
     * relation bt_core_reset produces for wave 1. */
    if (s->shop_go_pending && !s->done) {
        BT_SERIAL( bt_progress_shop_go(s); );
        /* WARP-UNIFORM: like the shop/level-up exit below, the guard reads
         * s->shop_go_pending/s->done, which are identical on every lane, so the
         * whole tick leaves through the same branch. */
        BT_SERIAL({
            s->time_left -= BT_DT;
            if (s->time_left < 0.0f) s->time_left = 0.0f;
            *reward = bt_reward_shaping(s, rw_p0, 0);
            s->episode_return += *reward;
            /* Same fail-closed latch as the shop and combat paths: the frame
             * that builds a new arena must not hide a capacity loss. */
            bt_capacity_latch_scan(s);
        });
        *terminal = 0.0f;
        BT_SERIAL( bt_core_observe(s, obs); );
        return;
    }
    if ((s->phase == 1 || s->phase == 2) && !s->done) {
        /* WARP-UNIFORM: the guard reads s->phase/s->done, identical on every
         * lane, so every lane leaves the tick the same way. The purchase and the
         * shaping ledger draw RNG and mutate shared state (s->episode_return,
         * s->log), so lane 0 owns them; the capacity latch and the observation
         * then run at the phase boundary before the uniform return. */
        BT_SERIAL({
            *reward = (s->phase == 2 ? bt_progress_choose_upgrade(s, action) :
                bt_progress_shop_action(s, action)) + bt_reward_shaping(s, rw_p0, 0);
            s->episode_return += *reward;
            /* Same fail-closed latch as the combat path below: a shop/level-up frame
             * must not be the one that hides a capacity loss. */
            bt_capacity_latch_scan(s);
        });
        *terminal = 0.0f;
        BT_SERIAL( bt_core_observe(s, obs); );
        return;
    }
    if (action < 0) action = 0;
    if (action > BT_ACTIONS - 1) action = BT_ACTIONS - 1;

    /* Deferred birth boundary (entity_spawner.gd:319-331 call_deferred("birth")
     * -> on_entity_birth_timeout -> spawn_entity): the source flushes the
     * deferred queue at the END of a frame, i.e. BETWEEN the physics step of
     * frame T-1 and the step of frame T.  In this single-pass core the frame
     * boundary is the top of the frame, so a birth that counts out in frame
     * T-1 activates here, BEFORE bt_phys_step -- the newborn's first live
     * snapshot is therefore the post-step position (Godot integrates it in the
     * same step as every older actor; a wall overlap is solved on that first
     * snapshot instead of one frame later).  bt_birth_advance (below) runs in
     * the frame the birth countdown reaches zero, exactly as the source's
     * EntityBirth._physics_process does.
     *
     * The spawner's own drain pass (entity_spawner.gd:52-75) runs FIRST.  Both
     * events belong to the spawner's frame -- the drain inside its
     * _physics_process walk, the boundary in the deferred `birth()` call that
     * walk queues -- so the drain's global words (the four owner-queue shuffles
     * plus the EntityBirth rotation draws of whatever it drains) are consumed
     * BEFORE every instance()/_ready/init draw of the enemies this boundary
     * activates.  Draining after the boundary (the historical order here) took
     * a flushed enemy's init draws from three words early on every frame whose
     * drain lands on the same tick: with the Demon's Ghost Scepter that put the
     * first wave-1 enemy's `_speed_rand_value` on 22 instead of the source's 12
     * (speed 272 instead of the captured 262) while every per-frame draw COUNT
     * still matched. */
    BT_SERIAL({
    /* entity_spawner.gd:117-119 _physics_process early-returns on _cleaning_up,
     * which clean_up_room (:580-581) sets for the whole wave-end window
     * (end_wave_active here).  The spawner's drain pass and its EntityBirth
     * children (freed by the same clean-up) therefore consume NOTHING from the
     * wave clock's expiry until the next wave's fresh scene: without this gate
     * the four owner-queue shuffles below still drew 3 global words every
     * BT_SPAWN_DELAY frames (measured: 119 extra draws by the shop transition
     * at tick 299 of v4_demon_s1, which moved the whole shop roll ~120 draws
     * down the stream and picked different offers).  cur_spawn_delay is
     * deliberately left un-advanced too, matching the early return. */
    if (!s->end_wave_active) {
    s->cur_spawn_delay += 1;
    if (s->cur_spawn_delay >= BT_SPAWN_DELAY) {
        bt_spawn_poll(s);
        s->cur_spawn_delay = 0;
    }
    bt_birth_flush(s);
    }

    bt_enemy_pool_step(s);
    bt_neutral_uid_step(s);   /* same 1.0 s ring as the enemy pool (entity.gd:68-100) */
    /* the structure/pet half of the same pool: a Structure.deferred_die() node
     * dies when its 3.0 s timer elapses (structure.gd:69-76), so a cap-evicted
     * structure reaches its scene's stack 4.0 s after the eviction. */
    bt_struct_pool_tick(s);
    });
    /* Godot advances bodies before Node._physics_process. Unit's previous
     * _integrate_forces_velocity therefore drives this step; AI below caches
     * the NEXT step's velocity. A fresh body starts with zero velocity. */
    BT_COOP( bt_phys_step(&s->phys); );
    BT_SERIAL({
    for (int i = bt_enemy_next(s, 0, 1); i < (int)s->e_used; i = bt_enemy_next(s, i + 1, 1)) {
        if (!s->ealive[i]) continue;
        /* The enemy contact-Hitbox AREA (a child of a MODE_CHARACTER body) is
         * still at the position this step's integration started from -- the
         * Node transform the server has not pushed back yet (BtState.ehx). */
        s->ehx[i] = s->ex[i]; s->ehy[i] = s->ey[i];
        s->ex[i] = s->phys.bodies[i].pos.x;
        s->ey[i] = s->phys.bodies[i].pos.y;
    }
    });
    /* --- player movement: RigidBody2D KINEMATIC, two test_motion calls --- */
    BT_SERIAL({
        /* player.gd:807-822 _clean_up() (run from main.gd clean_up_room's
         * player.on_room_cleanup(), i.e. the expiry frame, for the frame AFTER
         * it): cleaning_up = true, _can_move = false, _current_movement = ZERO and
         * set_physics_process(false).  unit.gd:137 then yields a zero
         * _current_movement, so the player does not move at all until the next
         * wave builds a fresh Main/Player.  The expiry frame itself still
         * integrates this frame's action (the timer callback runs after the
         * physics step), which is why this tests end_wave_active -- it is set by
         * bt_progress_end_wave at the END of that frame.  The moving/not-moving
         * latch is frozen too: the source pauses _moving_timer/_not_moving_timer. */
        if (!s->end_wave_active)
            bt_temporal_set_moving(s, BT_MOVE_X[action] != 0.0f || BT_MOVE_Y[action] != 0.0f);
        /* player.gd:352 max_stats.speed / unit.gd:615 get_movement speed: the
         * player moves at the DEFERRED LiveStats speed (p_speed_pct), which
         * lags a stat_speed change by the stat-sync pipeline. */
        float spd = bt_maxf(0.0f, BT_PLAYER_SPEED * (1.0f + s->p_speed_pct * 0.01f));
        BtVec2 vel = s->end_wave_active ? bt_v2(0.0f, 0.0f)
                                        : bt_v2mul(bt_v2(BT_MOVE_X[action], BT_MOVE_Y[action]), spd);
        BtPhysicsBody player = bt_phys_body_circle(bt_v2(s->px, s->py), bt_v2(0.0f, BT_PLAYER_BODY_Y),
                                     BT_PLAYER_BODY_R, 1.0f, BT_LAYER_PLAYER, BT_MASK_PLAYER,
                                     BT_PHYS_MODE_KINEMATIC);
        BtPhysMotionResult res;
        BtVec2 motion = bt_v2mul(vel, BT_DT);
        /* unit.gd:148-154 applies `global_position += result.motion` after EACH
         * test_motion call, colliding or not.  The engine's result.motion is
         * `p_motion` PLUS the shift the STEP-1 free-body recovery made to
         * body_transform (space_2d_sw.cpp:1207-1209), not the raw p_motion: a body
         * resting inside the 0.08 test-motion margin of a wall is recovered away
         * from it every frame, and on the frame it moves off that wall the rest
         * query then reports nothing (safe stays 1, best_len 0 -> collided false)
         * while the recovery term is still in result.motion.  Adding raw `motion`
         * in the not-colliding branch dropped it: measured on demon1b_full tick
         * 199 (player leaving the top wall) the game moved y +5.311707 and the
         * sim +5.303303, and that 0.008404 px rode the rest of the episode.  Both
         * calls therefore take res.motion unconditionally. */
        bt_phys_test_body_motion(&s->phys, &player, motion, 0.08f, &res);
        player.pos = bt_v2add(player.pos, res.motion);
        if (res.collided) {
            BtVec2 slide = bt_v2sub(vel, bt_v2mul(res.collision_normal, bt_v2dot(vel, res.collision_normal)));
            bt_phys_test_body_motion(&s->phys, &player, bt_v2mul(slide, BT_DT), 0.08f, &res);
            player.pos = bt_v2add(player.pos, res.motion);
        }
        s->px = player.pos.x; s->py = player.pos.y;
    });

    /* player.gd:491-492, 714-715: the regeneration timer starts on damage
     * and stops at full health; it does not run throughout full-health combat.
     * player.gd:588-591 set_hp_regen_timer_value: the `torture` effect
     * (items/all/torture) pins the interval to 1 s and player.gd:704-708 then
     * heals `torture_effect` per loop instead of `_hp_regen_val*(1+bonus)`, with
     * from_torture=true so heal()'s gate lets it through while every OTHER heal
     * is absorbed (bt_scalar_heal). */
    BT_SERIAL({
    float torture = s->scalar_effect[BT_ISTR_TORTURE];
    float regen_period = torture > 0.0f ? 1.0f : bt_regen_period(s->regen);
    /* player.gd:584/714 check_hp_regen + the regen loop compare
     * current_stats.health against max_stats.health: both are the deferred
     * LiveStats snapshot, so the gate uses p_max_hp. */
    /* player.gd:807-822 _clean_up() pauses _health_regen_timer, so the wave-end
     * window neither counts the regen period down nor heals (the node's
     * _physics_process is off); the countdown resumes in the next wave. */
    if (s->end_wave_active) { /* timer paused */ }
    else if (s->hp >= s->p_max_hp) s->regen_timer = regen_period;
    else s->regen_timer -= BT_DT;
    if (!s->end_wave_active && s->hp < s->p_max_hp && s->regen_timer <= 0.0f) {
        s->regen_timer = regen_period;
        if (torture > 0.0f) {
            (void)bt_scalar_heal_raw(s, torture);
        } else {
        float mult = 0.0f;   /* #5 hp_regen_bonus: sum while below the HP threshold */
        for (int a = 0; a < s->n_bk_active; a++) {
            int r = (int)s->bk_active[a];
            BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[r];
            if (!bt_eff_is(E, BT_ISTR_HP_REGEN_BONUS)) continue;
            for (int c = 0, nown = bt_row_owned(s, r); c < nown; c++) {
                int v, v2;
                if (!bt_row_get(s, r, E, c, &v, &v2)) break;
                if (s->hp < s->p_max_hp * ((float)v2 * 0.01f)) mult += (float)v;
            }
        }
        /* DocMoth BoostZone (bt_step_pet_motion): while a DocMoth's zone covers
         * the player its own hp_regen_bonus effect is in force
         * (doc_moth_effect_1: value 1, value2 100, SUM_VALUE_1 -> +1 while below
         * 100% HP, exactly the consumer rule player.gd:695-702 applies). */
        if (s->zone_hp_regen_bonus > 0.0f &&
            s->hp < s->p_max_hp * ((float)s->zone_hp_regen_threshold * 0.01f))
            mult += s->zone_hp_regen_bonus;
        (void)bt_scalar_heal(s, 1.0f * (1.0f + mult));
        }
    }
    /* s->iframe is a RUNNING-state count, not seconds: see bt_iframe_frames.
     * player.gd:807-822 _clean_up() stops and pauses _invincibility_timer, so the
     * wave-end window freezes the count (obs[6] keeps reading 1) instead of
     * counting it down. */
    if (!s->end_wave_active && s->iframe > 0.0f) s->iframe -= 1.0f;
    /* linked_stats.gd: dynamic sources refresh on the half-second flag */
    s->link_refresh_timer -= BT_DT;
    if (s->link_refresh_timer <= 0.0f) {
        s->link_refresh_timer = 0.5f;
        bt_recompute_links(s);
    }
    if (s->hp < s->p_max_hp * 0.85f) r += R_REGEN;
    });

    /* entity_birth.gd:83-106 countdown.  The drain pass that feeds new births
     * runs at the TOP of this frame (entity_spawner.gd's own _physics_process,
     * ahead of the deferred birth boundary it queues), and it still precedes
     * this frame's group firing, so a group that fires now is collected by the
     * next poll (source ordering: a wave-1 group at timing 1 s fires on frame
     * 60, its first birth starts on frame 63 and activates on frame 123).  The
     * countdown itself stays after the drain: the source's EntityBirth nodes
     * are walked after the spawner's callback. */
    BT_SERIAL({
    (void)bt_scalar_hot_tick(s);
    (void)bt_scalar_lose_hp_tick(s);
    (void)bt_scalar_alien_eyes_tick(s);
    /* entity_birth.gd:83-106 countdown, run AFTER the drain pass.  The source's
     * EntityBirth node is added to the tree from EntitySpawner._physics_process
     * and is visited by that same callback walk, so its first decrement is the
     * frame it was created in; a birth queued on frame F is ready on F+59. */
    bt_birth_advance(s);

    const float elapsed = s->wave_duration - s->time_left;
    if (!s->end_wave_active) bt_fire_groups(s, elapsed);
    });

    /* --- Timer rank (SceneTree::iteration's "physics_process_internal" pass):
     * the burning tick, BEFORE this frame's group pass and BEFORE the deferred
     * area hits.  See the BT_TIMER_PHASE definition. --- */
    BT_COOP( BT_TIMER_PHASE(s, &r); );

    /* --- enemy update: cached velocity (PhysicsMath) -> body.vel --- */
    BT_SERIAL({
    for (int i = bt_enemy_next(s, 0, 1); i < (int)s->e_used; i = bt_enemy_next(s, i + 1, 1)) {
        if (!s->ealive[i]) continue;
        /* Newborn hold (entity_spawner.gd:319-331 spawn_entity from the deferred
         * birth boundary): Godot does not visit the freshly added node in this
         * frame's _physics_process walk, so the source runs no Unit update for
         * it yet -- no movement input and no attack-timer tick.  Its body has
         * already been integrated by this frame's bt_phys_step (commanded
         * velocity 0 + the contact solver), so it keeps that post-step position
         * and a zero commanded velocity; the first Unit update follows next
         * frame, exactly one frame after the source's first step. */
        if (s->e_hold_tick[i] == (unsigned short)s->ticks) {
            s->phys.bodies[i].vel = bt_v2zero();
            s->phys.bodies[i].active = 1;
            continue;
        }
        if (s->eneutral[i]) {
            s->phys.bodies[i].vel = bt_v2zero();
            continue; /* Neutral has no enemy attack/state/boost behavior. */
        }
        int kind = s->ekind[i];
        float dx = s->px - s->ex[i], dy = s->py - s->ey[i];
        float d2 = dx * dx + dy * dy;
        int beh = (int)s->ebeh[i];
        /* Unit.get_move_speed() = max(0, current_stats.speed + bonus_speed +
         * decaying_bonus_speed); bonus_speed is the Unit member the
         * detect_target detection adds to and a charge dash SETS to
         * charge_speed (charging_attack_behavior.gd:117) before
         * on_unlock_move_timer_timeout() zeroes it (:125). */
        float bonus = s->ebonus[i];
        int can_move = 1;
        int self_die = 0;   /* spawning behaviour whose root dies on the emit */

        /* The attack clock `eatk` counts FRAMES of the behaviour's own clock,
         * not seconds.  charging_attack_behavior.gd:63-67 (and enemy.gd:95 for
         * `_current_attack_cd`) do `_current_cd = max(_current_cd - 60 * delta, 0)`
         * with the counter in frames from get_cd(), and every attack behaviour's
         * animation clock is `shoot_at_f`/`anim_len_f` frames, so ONE decrement
         * per physics frame is the exact model.  A float32 SECONDS accumulator
         * (eatk = frames/60, decremented by BT_DT) is one frame late whenever the
         * exact remaining lands on 0: the accumulated rounding residue stays
         * POSITIVE (measured on the deep-start capture: the wave-20 predator's
         * 120-frame cooldown left +1.233e-6 s after its 120th decrement, so the
         * charge telegraph -- and with it start_shoot's `_can_move = false` --
         * began one frame after the source's, and the capture's first divergence
         * at tick 185 obs[35] was that extra step).  The Godot-Timer clocks in
         * this file (bt_timer_frames, ebtimer) count frames for the same reason. */
        s->eatk[i] -= 1.0f;
        /* boss.gd:92-101 / boss.tscn:32-34: CheckStateTimer (autostart, default
         * 1.0 s) increments elapsed_time by 1 -> the state clock is 1 Hz. */
        int check_state = 0;
        if (BT_NKIND_STATES[kind] > 0) {
            s->estate_timer[i] += BT_DT;
            if (s->estate_timer[i] >= BT_BOSS_STATE_PERIOD) {
                s->estate_timer[i] -= BT_BOSS_STATE_PERIOD;
                s->eelapsed[i] += 1.0f;
                check_state = 1;
            }
        }

        /* boss/elite state escalation: hp threshold OR elapsed (boss.gd) */
        {
            int ns = (int)BT_NKIND_STATES[kind];
            if (ns > 0 && check_state) {
                int idx = s->estate[i] == BT_INIT_ROOT_STATE ? -1 : (int)s->estate[i];
                float emax = s->emax[i] > 0.0f ? s->emax[i] : 1.0f;
                for (int st = idx + 1; st < ns; st++) {
                    BT_CONST_PTR(BtAtkState) ss = &BT_ATTACK_STATES[kind][st];
                    if (s->ehp[i] <= emax * ss->hp_start || s->eelapsed[i] >= ss->timer_start) {
                        s->estate[i] = (unsigned char)st;
                        s->echarge[i] = 0.0f; s->ewind[i] = 0;
                        s->eshots[i] = 0; s->espawn_n[i] = 0;
                        float initial_cd = (float)ss->atk.initial_cd_f;
                        bt_u64 sampled_cd;
                        if (bt_enemy_init_find(s, i, BT_INITDEST_ATK_CD, st, &sampled_cd))
                            initial_cd += bt_rng_f64_to_float_bits(sampled_cd);
                        s->eatk[i] = initial_cd;   /* frames */
                    }
                }
            }
        }

        /* unit.gd:214-215 get_movement(): the active MovementBehavior's vector
         * for the movement node selected AFTER this frame's state escalation
         * (a Timer's internal physics notification precedes node callbacks, so
         * the source switches _current_movement_behavior before it integrates).
         * The attack branches keep dx/dy (player - self) for their own target
         * math and receive this vector exactly as the source's
         * shoot_in_unit_direction case reads _parent.get_movement(). */
        BtVec2 movement = bt_enemy_move(s, i);

        BT_CONST_PTR(BtAttack) atk = &BT_ATTACKS[kind];
        BT_CONST_PTR(BtAtkState) sst = 0;
        int ns = (int)BT_NKIND_STATES[kind];
        if (ns > 0 && (int)s->estate[i] < ns) sst = &BT_ATTACK_STATES[kind][s->estate[i]];
        if (sst) {
            atk = &sst->atk;
            beh = (int)sst->atk.beh;
            s->ebeh[i] = (unsigned char)beh;
        }

        if (beh == BT_BEH_SHOOT) {
            float minr = (float)atk->min_range;
            float maxr = (float)atk->max_range;
            /* shooting_attack_behavior.gd:70-81: the cooldown expiring (and the
             * range test) starts the scene's `shoot` animation; enemy.gd:325-330
             * arms the next cooldown with get_cd() when it finishes.  The
             * animation's "." method track calls shoot() at shoot_at_f frames and
             * the AnimationPlayer fires animation_finished at anim_len_f, so a
             * `shoot` key that sits before the animation end (the default
             * shoot.tres: 0.8 of 1.0 s) both delays the volley and leaves a tail
             * of animation still playing.  ewind: 1 = animation running (the
             * volley has not fired yet), 2 = the tail after the volley; only one
             * behaviour is active for a slot at a time.
             *
             * A tail of zero frames (the charge-prep animation the slashers and
             * the Butcher use: shoot == length) arms the cooldown in the same
             * frame the method fires, exactly as the AnimationPlayer emits both
             * in that frame. */
            if (s->ewind[i] != 0) {
                if (s->eatk[i] <= 0.0f) {
                    int fired = 0;
                    if (s->ewind[i] == 1) {
                        bt_fire_shot(s, i, *atk, movement.x, movement.y);
                        fired = 1;
                    }
                    int tail = (int)atk->anim_len_f - (int)atk->shoot_at_f;
                    if (fired && tail > 0) {
                        s->ewind[i] = 2;
                        s->eatk[i] = (float)tail - 0.5f;   /* frames */
                    } else {
                        BtAttack A = *atk;
                        float cd_f;
                        if (A.long_cd_every != 0 && (int)s->eshots[i] >= (int)A.long_cd_every) {
                            s->eshots[i] = 0;
                            cd_f = (float)A.long_cooldown;
                        } else {
                            float cd_r = (float)A.max_cd_rand;
                            cd_f = bt_rand_range(s, bt_maxf(1.0f, (float)A.cooldown_f - cd_r),
                                                    (float)A.cooldown_f + cd_r);
                        }
                        s->eatk[i] = cd_f;   /* frames */
                        s->ewind[i] = 0;
                    }
                }
            } else if (s->eatk[i] <= 0.0f && d2 >= minr * minr && d2 <= maxr * maxr) {
                s->ewind[i] = 1;
                s->eatk[i] = (float)atk->shoot_at_f - 0.5f;   /* frames (the -0.5 half-frame key window) */
            }
        } else if (beh == BT_BEH_CHARGE) {
            float cdur = atk->charge_duration;
            float cspd = atk->charge_speed;
            float cmaxr = (float)atk->max_range;
            if (s->echarge[i] > 0.0f) {
                /* charging: timed directional dash, direction sampled once */
                s->echarge[i] -= 1.0f;   /* frames: the unlock Timer's own countdown */
                movement = bt_v2(s->echarge_dx[i], s->echarge_dy[i]);
                bonus = cspd;
                if (s->echarge[i] <= 0.0f) {
                    float m = s->emass[i];
                    s->phys.bodies[i].mass = m; s->phys.bodies[i].inv_mass = 1.0f / m;
                    /* on_unlock_move_timer_timeout (charging:124-125) zeroes
                     * bonus_speed, wiping any detect_target bonus too. */
                    s->ebonus[i] = 0.0f;
                }
            } else if (s->ewind[i] == 1) {
                /* telegraph prep: movement input suppressed, mass x5, knockback stays */
                can_move = 0;
                if (s->eatk[i] <= 0.0f) {
                    /* the animation's `shoot` method: lock the direction (for
                     * Timing.SHOOT), unlock the dash and arm the cooldown.
                     * charging_attack_behavior.gd:110-121:
                     *   _current_cd = get_cd() + charge_duration * 60
                     * (the EXPORT charge_duration, not the scaled dash time). */
                    s->ewind[i] = 0;
                    if (atk->charge_timing != 0)   /* Timing.SHOOT */
                        bt_charge_target_dir(s, i, *atk, dx, dy,
                                             &s->echarge_dx[i], &s->echarge_dy[i]);
                    float ccd_r = (float)atk->max_cd_rand;
                    float ccd_f = bt_rand_range(s, bt_maxf(1.0f, (float)atk->cooldown_f - ccd_r),
                                                    (float)atk->cooldown_f + ccd_r);
                    s->eatk[i] = ccd_f + cdur * 60.0f;   /* frames */
                    /* _unlock_move_timer.wait_time = charge_duration
                     * (charging_attack_behavior.gd:43-46), raised by
                     * length/250/60 under scale_charge_duration_with_range
                     * (:96-98).  Its fire frame is the Timer's own countdown, so
                     * the dash length is bt_timer_frames(wait_time) FRAMES. */
                    float wait = cdur;
                    if (atk->scale_charge_with_range)
                        wait += bt_sqrtf(s->echarge_dx[i] * s->echarge_dx[i] +
                                         s->echarge_dy[i] * s->echarge_dy[i]) / 250.0f / 60.0f;
                    s->echarge[i] = bt_timer_frames(wait);
                }
            } else if (s->eatk[i] <= 0.0f && d2 < cmaxr * cmaxr) {
                /* start_shoot (charging:68-75): the animation's t=0 method sets
                 * _can_move = false, mass x5, and locks the direction for
                 * Timing.START_SHOOT -- the two/three set_target draws land in
                 * THIS frame, not at the dash. */
                s->ewind[i] = 1;
                s->eatk[i] = (float)atk->shoot_at_f - 0.5f;   /* frames (the -0.5 half-frame key window) */
                if (atk->charge_timing == 0)
                    bt_charge_target_dir(s, i, *atk, dx, dy,
                                         &s->echarge_dx[i], &s->echarge_dy[i]);
                float m = s->emass[i] * 5.0f;   /* charging_attack_behavior mass_multiplier */
                s->phys.bodies[i].mass = m; s->phys.bodies[i].inv_mass = 1.0f / m;
            }
        } else if (beh == BT_BEH_SPAWN) {
            int maxn = (int)(sst ? sst->atk.max_nb_spawns : atk->max_nb_spawns);
            if (maxn == 0 && sst) maxn = (int)atk->max_nb_spawns;
            int can = (maxn < 0) || (maxn > 0 && (int)s->espawn_n[i] < maxn);
            /* spawning_attack_behavior.gd:28-34,55-60: the same animation
             * structure as a shooting behaviour (trigger -> `shoot` method at
             * shoot_at_f -> animation_finished arms the cooldown, or
             * Utils.LARGE_NUMBER once `_current_nb_of_spawns` is spent). */
            if (s->ewind[i] != 0) {
                if (s->eatk[i] <= 0.0f) {
                    int spawned = 0;
                    if (s->ewind[i] == 1) {
                        BtAttack A = *atk;
                        int nb = (int)A.nproj;
                        short sk = A.spawn_kind;
                        for (int k2 = 0; k2 < nb; k2++) {
                            float sx = s->ex[i], sy = s->ey[i];
                            if (A.spawn_at_random_pos) bt_rand_pos(s, BT_EDGE_MAP_DIST, &sx, &sy);
                            else if ((int)A.spawn_radius != -1)
                                bt_pos_in_area(s, s->ex[i], s->ey[i], (float)A.spawn_radius, 0.0f, 0, &sx, &sy);
                            if (sk >= 0 && sk < BT_NKINDS) {
                                /* enemy.gd:333-334 emits wanted_to_spawn_an_enemy with
                                 * `source = self`: the spawner's live slot. */
                                bt_queue_summon_src(s, (int)sk, sx, sy, i,
                                                    bt_enemy_lifecycle_charmed_by(s, i));
                                s->espawn_n[i]++;
                            }
                        }
                        spawned = 1;
                    }
                    int tail = (int)atk->anim_len_f - (int)atk->shoot_at_f;
                    if (spawned && tail > 0) {
                        s->ewind[i] = 2;
                        s->eatk[i] = (float)tail - 0.5f;   /* frames */
                    } else {
                        BtAttack A = *atk;
                        if (maxn > 0 && (int)s->espawn_n[i] >= maxn) {
                            s->eatk[i] = 1.0e9f;   /* Utils.LARGE_NUMBER frames */
                        } else {
                            float sr = (float)A.max_cd_rand;
                            float cd_f = bt_rand_range(s, bt_maxf(1.0f, (float)A.cooldown_f - sr),
                                                          (float)A.cooldown_f + sr);
                            s->eatk[i] = cd_f;   /* frames */
                        }
                        s->ewind[i] = 0;
                        /* slasher_egg.gd:4-6 / iron_lung.gd:27-30: the root
                         * script dies inside its wanted_to_spawn handler, right
                         * after the emit -- ONE child per egg, not a volley. */
                        self_die = (int)atk->kill_self_on_spawn;
                    }
                }
            } else if (can && s->eatk[i] <= 0.0f) {
                s->ewind[i] = 1;
                s->eatk[i] = (float)atk->shoot_at_f - 0.5f;   /* frames (the -0.5 half-frame key window) */
            }
        } else if (BT_KIND_BOOST[kind].radius > 0.0f) {
            if (BT_KIND_MOVK[kind] & BT_MOVK_HEALER) {
                /* healer.gd has NO BoostTimer: its BoostZone heals on
                 * body_entered (bt_enemy_lifecycle_heal_step, once per frame). */
            } else {
                /* buffer.gd:31-60: BoostTimer (initial_cd, then boost_cooldown);
                 * a charmed Buffer's zone holds the player/structures, so the
                 * sample goes to the retargeted boost instead. */
                bt_zone_update(s, i);
                s->ebtimer[i] -= 1.0f;   /* frames, not seconds (bt_timer_frames) */
                if (s->ebtimer[i] <= 0.0f) {
                    s->ebtimer[i] = bt_timer_frames((float)BT_KIND_BOOST[kind].cooldown);
                    if (bt_enemy_charmed(s, i)) bt_enemy_lifecycle_charmed_boost(s, i);
                    else bt_buffer_boost(s, i, kind);
                }
            }
        }

        /* pursuer.gd:50-52 _on_BoostTimer_timeout: an INDEPENDENT autostart
         * BoostTimer (its own child node), so it ticks whatever the attack
         * behavior does.  It fires every boost_cooldown seconds and boosts while
         * nb_times_boosted < max_boosts and the unit is alive.  The timer
         * reloads BEFORE the handler runs (Godot's Timer adds wait_time before
         * emitting timeout), so the next fire is one cooldown later either way. */
        if (BT_KIND_SELFBOOST[kind].max_boosts > 0) {
            s->ebtimer[i] -= 1.0f;   /* frames (bt_timer_frames) */
            if (s->ebtimer[i] <= 0.0f) {
                s->ebtimer[i] = bt_timer_frames((float)BT_KIND_SELFBOOST[kind].cooldown);
                if ((int)s->enboost[i] < (int)BT_KIND_SELFBOOST[kind].max_boosts) {
                    bt_enemy_self_boost(s, i, kind);
                }
            }
        }

        /* contact-damage suppression (enemy.gd:99-105): the scaled stored
         * knockback magnitude strictly exceeds the movement-input magnitude. */
        BT_CONST_PTR(BtBodyPhysics) G = bt_kgeom(s, i);
        BtVec2 mv = can_move ? bt_move_input(movement, s->espd[i], bonus, 0.0f) : bt_v2zero();
        BtVec2 kb = bt_v2mul(bt_v2(s->ekbx[i], s->ekby[i]), bt_knockback_scale(G->knockback_resistance));
        s->esup[i] = (bt_v2lensq(kb) > bt_v2lensq(mv)) ? 1 : 0;

        /* cached velocity: movement input + decayed stored knockback */
        BtVec2 stored = bt_v2(s->ekbx[i], s->ekby[i]);
        BtVec2 out_stored;
        BtVec2 vel = bt_next_velocity(can_move ? movement : bt_v2zero(), s->espd[i], bonus,
                                      s->edecay[i], G->knockback_resistance, stored, &out_stored);
        s->ekbx[i] = out_stored.x; s->ekby[i] = out_stored.y;
        /* unit.gd:156-162: bonus-speed decay AFTER this tick's velocity */
        if (s->edecay[i] > 0.0f) s->edecay[i] = bt_maxf(s->edecay[i] - 60.0f * BT_DT, 0.0f);
        else if (s->edecay[i] < 0.0f) s->edecay[i] = bt_minf(s->edecay[i] + 60.0f * BT_DT * 5.0f, 0.0f);
        s->phys.bodies[i].pos = bt_v2(s->ex[i], s->ey[i]);
        s->phys.bodies[i].vel = vel;
        s->phys.bodies[i].active = 1;
        /* the deferred death boundary has already queued the spawned child, so
         * the egg dies after this frame's slot update (slasher_egg.gd:4-6). */
        if (self_die) { bt_credit_kill(s, i, 1); self_die = 0; }
    }

    /* Killed units keep their node for BT_POOL_DEATH_FRAMES and keep drifting
     * on the death knockback; their cached velocity for the next step is
     * produced here, exactly where a live slot's is. */
    bt_enemy_dying_step(s);
    });

    BT_SERIAL( if (BT_MOVE_X[action] != 0.0f) s->player_facing = BT_MOVE_X[action] > 0.0f ? 1 : -1; );
    /* A stat that feeds a weapon changed (item acquired, upgrade, temp stat
     * expired, class-set tier): stats_manager.gd:36-45's deferred re-init of
     * every weapon reduces to ONE reset_cooldown clamp per live weapon -- ZERO
     * RNG (never get_next_cooldown) and only on frames a stat actually changed,
     * since `wpn_dirty` is set by bt_core_stat_add/_add_temp and by every
     * loadout mutation.  Placed before this frame's shots so a faster attack
     * speed takes effect immediately, like the next StatsManager frame did. */
    BT_SERIAL( if (s->wpn_dirty) { bt_weapon_stats_changed(s); s->wpn_dirty = 0; } );
    BT_COOP( BT_WPROJ_PHASE(s, &r); );
    BT_SERIAL({
        bt_melee_step(s, BT_MOVE_X[action] != 0.0f || BT_MOVE_Y[action] != 0.0f, BT_MOVE_X[action]);
        bt_ranged_step(s, BT_MOVE_X[action] != 0.0f || BT_MOVE_Y[action] != 0.0f, s->player_facing);
    });

    /* Player/enemy contact damage is resolved at the END of this frame, after
     * every _physics_process system, because the engine forms the Area2D pair
     * in Physics2DServer::step and only delivers it (deferred) one frame later;
     * see the block just before the death check. */

    /* --- a charmed unit's contact hits OTHER ENEMIES: its Hitbox sits on
     * PET_PROJECTILES (charm:88), which enemy.tscn's Hurtbox mask (1032) sees, so
     * the contact resolves through the shared enemy-hit primitive with the
     * charm-boosted current_stats.damage.  An Enemy's player_index is -1, so the
     * hit carries no player attribution and no effect-behavior modifiers
     * (from_player_index -1, has_from 0). --- */
    BT_SERIAL({
    for (int i = bt_enemy_next(s, 0, 1); i < (int)s->e_used; i = bt_enemy_next(s, i + 1, 1)) {
        if (!s->ealive[i] || s->eneutral[i] || s->esup[i]) continue;
        if (!bt_enemy_charmed(s, i)) continue;
        BT_CONST_PTR(BtBodyPhysics) G = bt_kgeom(s, i);
        if (!G->has_hitbox) continue;
        for (int j = bt_enemy_next(s, 0, 1); j < (int)s->e_used; j = bt_enemy_next(s, j + 1, 1)) {
            if (j == i || !s->ealive[j] || s->eneutral[j]) continue;
            BT_CONST_PTR(BtBodyPhysics) H = bt_kgeom(s, j);
            if (!H->has_hurtbox) continue;
            if (!bt_circle_overlap_offset(bt_v2(s->ex[i], s->ey[i]), bt_v2(G->hit_x, G->hit_y),
                                          G->hit_radius,
                                          bt_v2(s->ex[j], s->ey[j]), bt_v2(H->hurt_x, H->hurt_y),
                                          H->hurt_radius)) continue;
            int k = 0;
            /* unit.gd:348: the enemy contact Hitbox leaves knockback_direction
             * (0,0) and amount 0, so a killed target still drifts on the
             * weighted direction floored at MIN_DEATH_KNOCKBACK_AMOUNT.  The
             * victim sample is the lagged Node transform (s->ehx/s->ehy). */
            {
                BtVec2 dir = bt_knockback_direction_weighted(bt_v2zero(),
                                                             bt_v2(s->px, s->py),
                                                             bt_v2(s->ehx[j], s->ehy[j]));
                BtVec2 dkb = bt_death_knockback(dir, 0.0f);
                s->edkbx[j] = dkb.x; s->edkby[j] = dkb.y;
            }
            (void)bt_item_expl_hit_enemy(s, j, (float)s->edmg[i], 0, 1.0f,
                                         0.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1, 0, &k, 0);
            if (k) r += R_KILL;
        }
    }
    });

    /* --- projectiles (projectile.gd:28-64; shapes from the attack table) --- */
    BT_COOP( BT_EPROJ_PHASE(s, &r); );

    /* --- consumables: the Item lifecycle + pickup (consumable.gd extends Item) ---
     * During the wave-end window the attraction/pickup is driven by
     * bt_loot_wave_end_step (gold flies to the bag, consumables to the player with
     * the health gate bypassed), so the ordinary player-driven loop is skipped. */
    BT_SERIAL( if (!s->end_wave_active)
    for (int i = 0; i < (int)s->con_used; i++) {
        if (!s->conalive[i]) continue;
        /* item.gd:26-70 for a dropped consumable: push back to its destination,
         * 10-frame idle -> monitorable, then the attract step.  Only the player
         * attracts a consumable (no pet/Area2D does), and a pickup needs
         * monitorable.  player.gd:1005-1013 (_attract_nearby_consumables) and
         * player.gd:1015-1025 (_on_ItemAttractArea_area_entered) both require
         * the player to be MISSING HEALTH: every consumable carries a
         * ConsumableHealingEffect (fruit/item_box via fruit_effect 3,
         * legendary_item_box 100), so `is_heal and current_stats.health <
         * max_stats.health` is the gate on entering the ItemAttractArea, and
         * _attract_nearby_consumables only runs from the take-damage path
         * (dmg_taken > 0, health already reduced). */
        float ax = s->px, ay = s->py + BT_ATTRACT_Y;
        float dx = ax - s->conx[i], dy = ay - s->cony[i];
        float d2 = dx * dx + dy * dy;
        float attract = bt_scalar_attract_radius(s);
        if (s->hp < s->p_max_hp && d2 <= attract * attract) s->con_attr[i] = 1;
        (void)bt_loot_item_step(&s->conx[i], &s->cony[i], s->con_arc[i], &s->con_attr[i],
                                0, ax, ay, BT_DT);
        s->conspd[i] = s->con_arc[i][BT_LOOT_ARC_SPEED];   /* _current_speed */
        if (s->con_arc[i][BT_LOOT_ARC_MON] == 0.0f) continue;
        if (s->con_attr[i] != 0 && s->con_attr[i] != 1) continue;
        /* the pickup circle is ItemPickupArea @ (0,-24), distinct from the
         * magnet area which is centred on the player root */
        float pdx = s->px - s->conx[i], pdy = (s->py + BT_PICKUP_Y) - s->cony[i];
        if (pdx * pdx + pdy * pdy < BT_PICKUP_R * BT_PICKUP_R) {
            /* consumable.gd:9-11 pickup() + main.gd:667-698
             * on_consumable_picked_up: the shared take helper (brotato_loot.h)
             * so the wave-end clean-up (main.gd:1005-1010) takes exactly the
             * same draws and applies the same effects as a walk-over pickup. */
            bt_loot_consumable_take(s, i);
        }
    }
    );

    /* --- materials (gold entities): the item.gd lifecycle + pickup ---
     * item.gd:26-70 for every dropped gold: push-back to its drop destination,
     * then the 10-frame idle phase that turns `monitorable` on, then the
     * attract step toward `attracted_by` with _current_speed += 20 per frame.
     * An item is only pickable while it is monitorable and its attractor is
     * nobody or the player.  Gold owned by a pet (mat_by != 0, mat_attr == 2) is
     * stepped and picked up by the Lootworm's own areas (bt_pet_gold), never
     * here; gold owned by an evil_mob (mat_mob >= 0) is stepped and eaten by
     * bt_enemy_lifecycle_tick, except that the player's area STEALS it when the
     * player is strictly closer than the mob (player.gd:1028-1029).
     * Gold/XP are 1:1 at pickup (main.gd:844-854). */
    BT_SERIAL({
    bt_enemy_lifecycle_tick(s);
    /* The wave-end window uses bt_loot_wave_end_step instead: the gold target is
     * the GoldBag and the pickup banks bonus_gold (no draw, no XP). */
    if (!s->end_wave_active)
    {
        float ax = s->px, ay = s->py + BT_ATTRACT_Y;
        float attract = bt_scalar_attract_radius(s);
        for (int m = 0; m < (int)s->mat_used; m++) {
            if (!s->matalive[m]) continue;
            if (s->mat_by[m] != 0 || s->mat_attr[m] == 2) continue;   /* a pet owns it */
            float dx = ax - s->matx[m], dy = ay - s->maty[m];
            float d2 = dx * dx + dy * dy;
            if (s->mat_mob[m] >= 0) {
                int om = (int)s->mat_mob[m];
                if (!s->ealive[om] || s->eneutral[om]) {
                    /* item.gd:62-66: the attractor died -> released (a culled
                     * mob never ran the death hook). */
                    s->mat_mob[m] = -1;
                    s->mat_attr[m] = 0;
                    s->mat_arc[m][BT_LOOT_ARC_SPEED] = BT_LOOT_ITEM_ATTRACT_SPEED;
                } else if (d2 <= attract * attract) {
                    float mdx = s->ex[om] - s->matx[m], mdy = s->ey[om] - s->maty[m];
                    if (d2 < mdx * mdx + mdy * mdy) {
                        s->mat_mob[m] = -1;      /* the player steals it */
                        s->mat_attr[m] = 0;
                    }
                }
            }
            if (s->mat_mob[m] >= 0) continue;   /* an evil_mob owns and steps it */
            /* the player's ItemAttractArea (radius 150) claims it; an
             * instant_gold_attracting drop (mat_home) homes from any distance */
            if (s->mat_home[m] || d2 <= attract * attract) s->mat_attr[m] = 1;
            (void)bt_loot_item_step(&s->matx[m], &s->maty[m], s->mat_arc[m], &s->mat_attr[m],
                                    0, ax, ay, BT_DT);
            if (s->mat_arc[m][BT_LOOT_ARC_MON] == 0.0f) continue;    /* not monitorable yet */
            if (s->mat_attr[m] != 0 && s->mat_attr[m] != 1) continue;
            /* ItemPickupArea @ (0,-24) (distinct from the magnet area at the
             * player root) */
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
    });
    /* --- StructureTimer (global/entity_spawner.gd:526-567
     * _on_StructureTimer_timeout): main.tscn's StructureTimer is an autostart
     * Timer with the default 1 s wait_time, so it fires once per whole second of
     * wave time (tick_in_wave % 60 == 0, cur_time = tick_in_wave / 60). The tick
     * queues the entities; nothing is instantiated until the spawner poll. --- */
    BT_SERIAL( if (!s->end_wave_active && s->tick_in_wave > 0 && s->tick_in_wave % 60 == 0)
        bt_structure_timer(s); );

    /* pet motion + interaction zones (brotato_pet_motion.h): movement for every
     * pet, the DocMoth BoostZone on the player, Scapegoat damage/revive, the
     * Lootworm's own gold attract/pickup and tree melee, CatlingGun mad mode.
     * Runs after the player's magnet pass and before the firing block, so the
     * positions and zones this frame's shots use are the current ones. */
    BT_SERIAL( bt_step_pet_motion(s); );

    /* pets/turrets fire on their real WeaponStats cooldown at the nearest enemy
     * in range (BT_ITEM_WSTATS damage/cooldown/range; no scalar DPS). */
    BT_SERIAL({
    for (int p = 0; p < s->pet_n; p++) {
        if (!s->pet_alive[p]) continue;
        /* A plain StructureEffect entity (the landmines item's mine and the
         * BotOMine's dropped bombs) extends Structure, which has no attack: only
         * Turret.shoot fires projectiles.  The mine is armed and triggered by its
         * own Area2D instead (bt_item_expl_landmine_tick below). */
        if (s->pet_opcode[p] == BT_ITEMEFF_STRUCTURE) continue;
        /* Garden: an is_spawning TurretEffect shoots no projectile; it emits a
         * fruit on its own cooldown with no target requirement (garden.gd:5-11).
         * Owned by bt_structure_spawning_tick (brotato_structures.h) below. */
        if (s->pet_opcode[p] == BT_ITEMEFF_TURRET &&
            BT_ITEM_EFF_ROWS[s->pet_row[p]].is_spawning) continue;
        float ox = s->pet_x[p], oy = s->pet_y[p];
        /* BotOMineEffect: the pet drops a landmine (its nested effect's scene)
         * into the structures queue every landmine spawn_cooldown
         * (bot_o_mine.gd:53-54,107-113). The cooldown was initialised to the
         * curse-transformed landmine spawn_cooldown at spawn. */
        if (s->pet_opcode[p] == BT_ITEMEFF_BOT_O_MINE && s->pet_lm_cd[p] > 0.0f) {
            s->pet_lm_timer[p] -= BT_DT;
            if (s->pet_lm_timer[p] <= 0.0f) {
                s->pet_lm_timer[p] = s->pet_lm_cd[p];
                BT_CONST_PTR(BtItemEff) PE = &BT_ITEM_EFF_ROWS[s->pet_row[p]];
                if (PE->child != BT_ITEM_EFF_NONE) {
                    float lx, ly;
                    bt_pos_in_area(s, ox, oy, 100.0f, 0.0f, 0, &lx, &ly);
                    bt_queue_struct(s, 0xFFFF, PE->child, lx, ly);
                }
            }
        }
        s->pet_timer[p] -= BT_DT;
        /* Turret.should_shoot refuses while `_pending_die` (turret.gd:78), but
         * turret.gd:39 keeps decrementing `_cooldown` every frame the node is
         * alive: a structure killed by MAX_STRUCTURES only plays its death
         * animation 3.0 s later (structure.gd:69-76), so its cooldown keeps
         * running until then, it just cannot fire.  A Garden overrides
         * should_shoot without that test (garden.gd:9-10) and is handled by
         * bt_structure_spawning_tick below. */
        if (s->pet_pool_pending[p]) continue;
        if (s->pet_timer[p] > 0.0f) continue;
        int tgt = -1; float bd = s->pet_range[p] * s->pet_range[p];
        /* the pet TargetBehavior scans the ENEMY list only; neutral trees are
         * never a shooting target (the Lootworm reaches them through its own
         * TreeArea2D melee in pet_motion). Friendly shots still collide with a
         * tree Hurtbox if one lies along the path. */
        for (int i = bt_enemy_next(s, 0, 1); i < (int)s->e_used; i = bt_enemy_next(s, i + 1, 1)) {
            if (!s->ealive[i] || s->eneutral[i]) continue;
            float dx = s->ex[i] - ox, dy = s->ey[i] - oy;
            float d2 = dx * dx + dy * dy;
            if (d2 < bd) { bd = d2; tgt = i; }
        }
        if (tgt < 0) continue;
        /* weapon_service.gd:237-249 applied the percent-damage bonus once, at
         * init: the shot carries pet_dmg as spawned, never a per-shot
         * stat_percent_damage multiply. */
        float dmg = s->pet_dmg[p];
        if (dmg < 1.0f) dmg = 1.0f;
        /* A Turret / BuilderTurret fires through turret.gd:88-104: the volley
         * draws the accuracy rotation (:94, 3 G), the shooting-sounds pick
         * (:96, 1 G) and one spread draw per projectile (:113, 3 G each), and
         * the animation-finished _get_next_cooldown (:155-157 -> :121-128, 3 G)
         * then sets the timer.  Every other pet class fires on its own fixed
         * cooldown with a deterministic fan here. */
        int is_turret_shoot = (s->pet_opcode[p] == BT_ITEMEFF_TURRET ||
                               s->pet_opcode[p] == BT_ITEMEFF_BUILDER_TURRET);
        float next_rot = bt_atan2f(s->ey[tgt] - oy, s->ex[tgt] - ox);
        if (is_turret_shoot) {
            BT_CONST_PTR(BtItemWeaponStats) W =
                &BT_ITEM_WSTATS[BT_ITEM_EFF_ROWS[s->pet_row[p]].stats];
            next_rot += bt_rand_range(s, -1.0f + W->accuracy, 1.0f - W->accuracy);
            (void)bt_rng_bounded(&s->rng, 1u);   /* shooting_sounds pick (1 draw) */
        } else {
            s->pet_timer[p] = s->pet_cd[p];
        }
        /* real projectile, not direct damage: pet fires at the target on its
         * WeaponStats speed/spread/count (player-style, hits enemies). The
         * weapon's burning_data rides the shot (init_burning_data at spawn,
         * weapon_service.gd:296-333): the hitbox applies it to the target. */
        int np = s->pet_nproj[p] ? (int)s->pet_nproj[p] : 1;
        for (int k = 0; k < np && k < 12; k++) {
            float a;
            if (is_turret_shoot) {
                a = bt_rand_range(s, next_rot - s->pet_spread[p], next_rot + s->pet_spread[p]);
            } else {
                a = next_rot;
                if (np > 1) a += s->pet_spread[p] * ((float)k / (float)(np - 1) - 0.5f) * 2.0f;
            }
            int pj = bt_spawn_projectile(s, ox, oy, a, s->pet_pspeed[p], dmg, BT_SHAPE_CIRCLE,
                                         s->pet_proj_r[p], 0.0f, 0.0f, a, 1);
            if (pj >= 0) {
                /* fly.gd:10 keys on `hitbox.from is Pet and
                 * from.shoot_projectiles`: pet.gd:6's export is true only for
                 * catling_gun.tscn / bot_o_mine.tscn, i.e. exactly these two
                 * effect classes (a Turret is a Structure and never triggers). */
                if (s->pet_opcode[p] == BT_ITEMEFF_CATLING_GUN ||
                    s->pet_opcode[p] == BT_ITEMEFF_BOT_O_MINE)
                    s->pr_pet[pj] = 1;
                s->prburn_chance[pj] = s->pet_burn_chance[p];
                s->prburn_dmg[pj] = s->pet_burn_dmg[p];
                s->prburn_dur[pj] = s->pet_burn_dur[p];
                s->prcrit_chance[pj] = s->pet_crit_chance[p];
                s->prcrit_dmg[pj] = s->pet_crit_dmg[p];
                /* the ExplodingEffect this entity hands to its shots (turret.gd
                 * :115 `_spawn_projectile_args.effects = effects`): the impact
                 * spawns it AT THE TARGET instead of the direct hit
                 * (unit.gd:552-578).  Empty for every other shooter. */
                s->pr_expl[pj] = bt_item_expl_shot_effect(s, p);
            }
        }
        /* turret.gd:155-157 animation-finished -> :121-128 _get_next_cooldown:
         *   rand_range(max(1, mc * 0.7), mc * 1.3),
         * mc = apply_structure_attack_speed_effects(stats.cooldown).  The shoot
         * animation is not modelled (see the block comment above), so the draw
         * is taken immediately after the volley, as for Garden. */
        if (is_turret_shoot) {
            BT_CONST_PTR(BtItemWeaponStats) W =
                &BT_ITEM_WSTATS[BT_ITEM_EFF_ROWS[s->pet_row[p]].stats];
            float mc = (float)bt_apply_structure_attack_speed(s, (int)W->cooldown);
            float lo = mc * 0.7f;
            if (lo < 1.0f) lo = 1.0f;
            float hi = mc * 1.3f;
            s->pet_timer[p] = bt_rand_range(s, lo, hi) / 60.0f;
        }
    }
    });

    /* structures that move: wandering_bot.gd:18-21 (_physics_process directly
     * assigns global_position = players[player_index].global_position +
     * Vector2(cos(_angle), sin(_angle)) * distance, with `_angle += delta *
     * rotation_speed`).  This runs AFTER the turret firing loop above so the
     * turret targets from the bot's PRE-move position -- Godot calls
     * Turret._physics_process before WanderingBot._physics_process inside the
     * node's call_multilevel (base script level before the derived level), and
     * the shared core's pet_motion step runs before this firing block. */
    BT_SERIAL({
    for (int p = 0; p < s->pet_n; p++) {
        if (!s->pet_alive[p]) continue;
        if (bt_pet_ai_index((int)s->pet_opcode[p]) >= 0) continue;   /* not a structure */
        if (BT_ITEM_EFF_ROWS[s->pet_row[p]].key != BT_ISTR_WANDERING_BOT) continue;
        s->pet_wb_angle[p] += BT_DT * s->pet_wb_rots[p];
        s->pet_x[p] = s->px + bt_cosf(s->pet_wb_angle[p]) * s->pet_wb_dist[p];
        s->pet_y[p] = s->py + bt_sinf(s->pet_wb_angle[p]) * s->pet_wb_dist[p];
    }
    });

    /* Garden: the is_spawning turrets emit a fruit on their own cooldown (no
     * target requirement), handled after the projectile-firing block. */
    /* landmines: the trigger Area2D arms the mine on body_entered and explodes
     * it at its own position on body_exited (landmine.gd:29-46).  Runs after the
     * firing block so this frame's positions are the current ones. */
    /* burning on hit (BT_ITEM_BURNING): eburn counts the remaining FRAMES and
     * eburndmg is the damage of one tick; unit.tscn's BurningTimer deals it once
     * per bt_burn_period_frames() and decrements the duration (unit.gd:661-707).
     * The body lives in bt_burn_tick() so a parallel schedule can precompute the
     * tick set (bt_burn_scan) and run the ordered RNG/kill side through the same
     * per-enemy function (bt_burn_tick_one). */
    /* --- the deferred half of the player/enemy contact (BtState.epend): the
     * queued hurt_area_entered_deferred call, applied BEFORE this frame's
     * explosion tick.
     *
     * ENGINE ORDER.  One physics iteration is (main/main.cpp:2383-2410)
     *   1. iteration_prepare
     *   2. PhysicsServer::flush_queries / Physics2DServer::sync /
     *      Physics2DServer::flush_queries -- the pairs the PREVIOUS step's
     *      _update_shapes queued are delivered HERE (Space2DSW::call_queries ->
     *      Area2DSW::call_queries -> Area2D::_area_inout -> the script's
     *      area_entered handler unit.gd:610-611, which call_deferred()s
     *      hurt_area_entered_deferred),
     *   3. main_loop->iteration() -- SceneTree::iteration: the
     *      "physics_process" group pass (every _physics_process, tree order) and
     *      then MessageQueue::flush() (scene/main/scene_tree.cpp:603-609), which
     *      RUNS hurt_area_entered_deferred,
     *   4. Physics2DServer::step -- the collide/pair resolution
     *      (Step2DSW::step -> Space2DSW::update + island setup ->
     *      AreaPair2DSW::setup does the overlap test and queues the NEXT
     *      signal).
     *
     * So inside ONE frame the deferred contact hit is applied BEFORE that
     * frame's Area2D pair resolution, never after it.  The previous placement
     * (the frame's tail, after BT_ITEMSFX_PHASE, inside BT_CONTACT_PHASE) gave
     * every effect the player's hurt path spawns a one-frame phase error: the
     * Bull's explode_on_hit (player.gd:475-480 -> bt_explode) instantiates the
     * explosion node from this path, and an explosion spawned AFTER the
     * explosion tick can only have its deferred-collision age-0 frame consumed
     * by the NEXT frame's bt_item_expl_tick, so it silently loses the first of
     * its three overlap frames (brotato_item_explosions.h:528-580).  The trace
     * oracle pins the phase: migration/game/rl_trace.contact161_normal.jsonl
     * has the contact damage first visible on tick 278 (hp 30 -> 29) and both
     * Bull-explosion kills on tick 279 (5 -> 3 enemies) -- exactly ONE frame
     * apart, never two.
     *
     * Same MessageQueue boundary bt_birth_flush is placed at the top of the
     * frame for (entity_spawner.gd:319-331 call_deferred("birth")).  This block
     * stays inside the frame's tail rather than at the top because its caller,
     * SceneTree::iteration's flush, itself follows every _physics_process of the
     * iteration (scene_tree.cpp:603-609): the explosion must spawn at the
     * player's post-move position, exactly as the source's flush uses it. --- */
    BT_COOP( BT_CONTACT_APPLY_PHASE(s, &r); );

    /* position-based item explosions (rocket turret / landmines / BotOMine
     * bombs / explode_on_consumable / explode_on_death): age the pool, expire
     * past the 3-frame window and hit every target once
     * (brotato_item_explosions.h).  Runs after every producer of this frame.
     * The garden, landmine, burning and explosion calls are ONE phase
     * (bt_itemsfx_phase_serial) with a single re-schedulable entry point. */
    BT_COOP( BT_ITEMSFX_PHASE(s, &r); );

    /* TempStatsPerIntervalEffect: grant `value` of `key` every `interval` s
     * (stacks accumulate; reset_on_hit clears them via bt_player_damage). */
    BT_SERIAL({
    for (int t = 0; t < s->n_tsi; t++) {
        s->tsi_timer[t] -= BT_DT;
        if (s->tsi_timer[t] <= 0.0f) {
            s->tsi_timer[t] += s->tsi_interval[t];
            s->tsi_stacks[t] += 1.0f;
            bt_core_stat_add_temp(s, (int)s->tsi_id[t], s->tsi_val[t]);
        }
    }
    });
    /* enemy_percent_damage_taken debuff records: percent_damage_taken_enemy_
     * effect_behavior.gd:23-28 _process -- each active record's timer counts
     * FRAMES and on expiry only that record's own added percent is removed
     * (:121-137).  Runs once per step, after every producer of this frame. */
    BT_SERIAL( bt_enemy_hit_tick(s); );

    /* #9/#10 decaying temp stats: expire and remove */
    BT_SERIAL({
    for (int d = 0; d < s->n_dt; d++) {
        if (s->dt_remain[d] <= 0.0f) continue;
        s->dt_remain[d] -= BT_DT;
        if (s->dt_remain[d] <= 0.0f) bt_core_stat_add_temp(s, (int)s->dt_id[d], -s->dt_val[d]);
    }
    });

    /* --- player/enemy contact: player Hurtbox (21 @ (0,-12)) vs enemy contact
     * Hitbox (real per-kind radius/offset).  This is the discrete Area2D overlap
     * Physics2DServer::step resolves, so it runs at the frame's end, after every
     * _physics_process system -- and its two engine timing rules (BtState.ehx /
     * BtState.epend) are:
     *   1. the enemy Hitbox AREA is sampled at the enemy's PREVIOUS-step
     *      position (ehx/ehy) -- a MODE_CHARACTER body's Node transform is one
     *      step behind the integrated body -- while the player's MODE_KINEMATIC
     *      Hurtbox is already at this step's position (px/py);
     *   2. the pair only QUEUES the hit: unit.gd:610-611's area_entered handler
     *      call_deferred()s hurt_area_entered_deferred, which runs in the NEXT
     *      frame's flush -- and, inside that frame, BEFORE its explosion tick
     *      (BT_CONTACT_APPLY_PHASE above), never after it: the Bull explosion
     *      the hit spawns must not lose the age-0 promotion of its first frame.
     * The player and enemy BODIES never physically collide (masks exclude it),
     * which the solver already enforces. --- */
    /* fly.gd:11 call_deferred("shoot"): the reaction volley a hitbox entry
     * queued during this frame's hit resolution fires at the deferred boundary,
     * one step after the roll -- the same boundary the queued contact hurt
     * above crosses. */
    BT_SERIAL( bt_enemy_fly_flush(s); );
    BT_COOP( BT_CONTACT_PHASE(s, &r); );

    BT_SERIAL( if (s->hp < 0.0f) s->hp = 0.0f; );
    if (s->hp <= 0.0f) {
        BT_SERIAL({
            bt_combat_audio_run_lost(s);
            bt_combat_audio_end_wave(s);
            s->done = 1;
        });
        r += R_DEATH;
    }
    BT_SERIAL( if (!s->done) bt_progress_award_levels(s); );

    BT_SERIAL( s->ticks++; );
    BT_SERIAL({
    if (s->end_wave_active) {
        /* Wave-end window (main.gd:1178-1234 then the 2.0 s EndWaveTimer): the
         * WaveTimer is stopped (time_left stays 0), the room is empty and the
         * spawners are off (entity_spawner.gd:580-608 / wave_manager.gd:259-263
         * clean_up_room), so only the player, the window timers and the
         * observation advance.  obs[7] phase stays 0 until the EndWaveTimer
         * expires (bt_progress_wave_end_finish flips it). */
        bt_progress_wave_end_step(s);
    } else {
        s->tick_in_wave++;
        s->time_left -= BT_DT;
        /* The bridge observes the game's WaveTimer AFTER this frame's decrement
         * (the observed_time convention above: "the live timer has already
         * advanced for the upcoming step"), so the game's timeout -- its binary64
         * time_left reaching 0, RunData.on_wave_end / the clean_up_room kill --
         * corresponds to THIS clock being one frame from 0: fire at BT_DT, not at
         * 0.  Measured on v4_demon_s1 wave 1: the game's timer reads 0 and
         * RunData.current_living_enemies snaps 5 -> 0 on row 179, while this
         * clock's raw value there is BT_DT + float dust; the old `<= 0` test fired
         * two rows later.  The tolerance covers that accumulation dust (1e-4 s is
         * well under half a tick). */
        if (s->time_left <= 1e-4f && !s->done) {
        s->waves_cleared++;
        s->birth_audio_head = s->birth_audio_count = 0; /* main.gd:1078-1079 */
        s->main_audio_head = s->main_audio_count = 0;
        r += bt_reward_wave_bonus(s);
        if (s->wave == BT_WAVES && !s->endless) {
            s->cleared_game = 1;
            s->won = 1;
            /* main.gd:909-933 clean_up_room still runs on a wave-20 win: it
             * appends the next decade's elite schedule (init_elites_spawn(
             * current_wave + 10, 0.0)) BEFORE the end_wave sound pick. */
            if (s->wave % 10 == 0 && s->wave >= 10)
                bt_schedule_elites_endless(s, s->wave + 10, 0.0f);
            bt_combat_audio_end_wave(s);
            bt_combat_audio_run_won(s);
            s->done = 1;
            r += R_WIN;
        } else {
            /* Campaign waves 1..19, and (in endless) wave 20 and every wave past
             * it: main.gd:_set_run_states leaves _is_run_won false for an endless
             * run with live players, so the run continues into the next wave
             * forever (it ends only when every player dies). */
            bt_progress_end_wave(s);
        }
        }
        bt_wave_tick_timer_step(s);
    }
    });

    /* (birth flush moved to the top of this frame: the source's deferred
     * boundary sits between two physics steps, so the newborn must be stepped
     * by this frame's bt_phys_step before the snapshot is taken.) */
    BT_SERIAL({
    bt_main_audio_step(s);
    bt_birth_audio_step(s);

    int visual_steps = bt_player_visual_step(s,
        BT_MOVE_X[action] != 0.0f || BT_MOVE_Y[action] != 0.0f,
        bt_maxf(0.0f, 1.0f + s->p_speed_pct * 0.01f));
    bt_player_visual_emit_steps(s, visual_steps);
    });
    /* F = gamma*Phi(s') - Phi(s) for this transition (brotato_reward.h).  The
     * next statement's state is still the end-of-transition state: bt_core_reset
     * only runs below, so Phi(s') is read here.  On an episode end Phi is taken
     * as 0 (episodic convention) and the transition pays -Phi(s). */
    BT_SERIAL({
        r += bt_reward_shaping(s, rw_p0, s->done);
        *reward = r;
        s->episode_return += r;
        bt_core_observe(s, obs);

        /* Latch EVERY capacity loss BEFORE the end-of-episode reset below zeroes the
         * per-episode flags: a loss in this step (including a per-entity refusal whose
         * shot died in the same tick) must survive the reset, because the wrapper
         * reads bt_capacity_mask() only after bt_core_step returns. */
        bt_capacity_latch_scan(s);
    });
    *terminal = 0.0f;

    if (s->done) {
        BT_SERIAL({
            bt_core_add_log(s);
            bt_core_reset(s);
            bt_core_observe(s, obs);
        });
        *terminal = 1.0f;
    }
}

/* Per-id action legality - the SINGLE source of truth for the legality row.
 * bt_action_mask (CPU env) and btgo_action_mask_warp (the lane-strided GPU
 * observation writer) both call exactly this, so the row can never drift
 * between backends.  Conditions mirror what the game's own handlers accept
 * (see the per-branch comments). */
BT_HD int bt_action_legal(BT_DEVICE BtState* s, int id) {
    if (id < 0 || id >= BT_ACTIONS) return 0;
    if (s->phase == 0) return id < BT_ACT_MOVE_COUNT;
    if (s->phase == 2) {
        /* crate: one consumable is shown, take / recycle only */
        if (s->crate_kind != BT_SHOP_NONE)
            return id == BT_ACT_CRATE_TAKE || id == BT_ACT_CRATE_RECYCLE;
        /* level-up: only a present offer; reroll needs >1 offer and gold
         * (upgrades_ui_player_container.gd:75 hides the button for one offer). */
        if (id >= 0 && id < 4) return bt_lvl_offer_present(s, id);
        if (id == BT_ACT_REROLL) {
            int offers = 0;
            for (int i = 0; i < 4; i++) offers += bt_lvl_offer_present(s, i);
            int price = s->lvl_reroll_price > 0 ? s->lvl_reroll_price : 0;
            return offers > 1 && s->materials >= (float)price;
        }
        return 0;
    }
    /* phase 1: the shop */
    int ready = s->shop_timer <= 0;
    if (id <= 3) {
        int slot = id;
        if (!ready) return 0;                      /* purchase delay */
        unsigned char kind = s->shop_kind[slot];
        unsigned short oid = s->shop_id[slot];
        float price;
        if (kind == BT_SHOP_ITEM && (int)oid < BT_NITEMS) {
            price = bt_shop_slot_price(s, slot);
        } else if (kind == BT_SHOP_WEAPON && bt_weapon_valid(oid)) {
            if (!bt_wshop_can_buy(s, oid)) return 0;
            price = bt_shop_slot_price(s, slot);
        } else {
            return 0;                              /* empty / unsupported offer */
        }
        return bt_wshop_currency(s) >= price;
    }
    if (id == BT_ACT_REROLL) {
        /* the buy delay gates BUYS only (the container's handlers), never the
         * reroll button (base_shop.gd:400-410). */
        int free_reroll = s->free_rerolls_remaining > 0 || s->has_bonus_free_reroll;
        if (free_reroll) return 1;
        float rr = bt_ceilf((float)bt_reroll_cost(s->wave, s->paid_rerolls) *
            bt_maxf(0.1f, 1.0f + s->scalar_effect[BT_ISTR_REROLL_PRICE] * 0.01f));
        return s->materials >= rr;
    }
    if (id == BT_ACT_SKIP) return 1;
    if (id >= BT_ACT_RECYCLE0 && id < BT_ACT_RECYCLE0 + 6) {
        int slot = id - BT_ACT_RECYCLE0;
        return slot < (int)s->wpn_count && s->wpn_id[slot] != BT_WEAPON_NONE;
    }
    if (id >= BT_ACT_COMBINE0 && id < BT_ACT_COMBINE0 + 6) {
        int slot = id - BT_ACT_COMBINE0;
        if (slot >= (int)s->wpn_count || s->wpn_id[slot] == BT_WEAPON_NONE) return 0;
        return bt_weapon_can_combine(s, slot);     /* the game's own can_combine */
    }
    if (id >= BT_ACT_LOCK0 && id < BT_ACT_LOCK0 + 4) {
        /* the lock button lives on the live ShopItem node, so a slot with no
         * offer has nothing to lock; no reached profile carries disable_item_locking */
        return s->shop_kind[id - BT_ACT_LOCK0] != BT_SHOP_NONE;
    }
    if (id >= BT_ACT_BAN0 && id < BT_ACT_BAN0 + 4) {
        int slot = id - BT_ACT_BAN0;
        if (s->shop_kind[slot] != BT_SHOP_ITEM) return 0;   /* weapons cannot be banned */
        return s->ban_mode && s->ban_tokens > 0;            /* shop_item.gd:106-119 */
    }
    return 0;
}

/* The whole legality row (CPU env, and any backend that writes it in one go). */
BT_HD void bt_action_mask(BT_DEVICE BtState* s, BT_DEVICE unsigned char* mask) {
    if (mask == 0) return;
    for (int i = 0; i < BT_ACTIONS; i++) mask[i] = (unsigned char)(bt_action_legal(s, i) ? 1 : 0);
}
