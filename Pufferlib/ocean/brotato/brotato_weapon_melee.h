/* brotato_weapon_melee.h -- source player MELEE weapons for the shared core.
 *
 * Owner: WeaponMelee.  Included by brotato_core.h after brotato_weapon_stats.h,
 * brotato_weapon_effects.h and brotato_weapon_inventory.h and before bt_core_step,
 * so every helper it calls (bt_weapon_stats, bt_weapon_uid, s->wpn_fxcurse,
 * bt_weapon_remove, bt_wfx_*, bt_item_expl_hit_enemy, bt_phys_math, the RNG
 * wrappers) is already defined.  It only reads/writes BtState; nothing allocates
 * and nothing touches libc/stdio.
 *
 * SOURCE (under /Users/leelachebiyyam/brotato_recovered unless DLC)
 *   weapons/melee/melee_weapon.gd                     attack-type alternation
 *   weapons/weapon.gd                                 targeting, should_shoot,
 *                                                     cooldown, spread, knockback
 *   weapons/shooting_behaviors/melee_weapon_shooting_behavior.gd
 *   weapons/shooting_behaviors/melee_shooting_data.gd durations
 *   weapons/weapon_stats/melee_weapon_stats.gd        deal_dmg_on_return / types
 *   entities/units/unit/unit.gd                       hurt_area_entered_deferred,
 *                                                     take_damage, knockback
 *   entities/units/player/weapons_container.gd + player.tscn  attach offsets
 *   dlcs/dlc_1/weapons/melee/                       Abyss DLC melee scenes
 *
 * WHAT THIS FILE OWNS
 *   * the melee swing state machine (recoil / out / return) with the EXACT
 *     Godot 3 Tween easing the behavior uses (TRANS_EXPO/EASE_OUT for the
 *     recoil and return, TRANS_LINEAR for the two sweep halves) and the engine's
 *     own frame stepping: a segment needs `bt_melee_frames(dur)` tween steps
 *     (the float32 accumulation `elapsed += p_delta` until `elapsed > duration`),
 *     the tween_all_completed emit lands ONE frame after the completing step, and
 *     enable_hitbox/disable_hitbox are `set_deferred`, i.e. effective on the next
 *     frame's physics step -- the three additive frames are spelled out at the
 *     segment boundaries in bt_melee_anim,
 *   * the animated oriented-rectangle / circle hitbox (sprite position +
 *     rotation composed with the Hitbox/Collision offset) and its per-frame
 *     collision against an enemy Hurtbox circle,
 *   * per-target hit gating (weapon.gd ignored_objects: each enemy hit at most
 *     once per swing) keyed on the source-OBJECT identity (bt_object_uid), not
 *     the reusable live slot, range targeting, THRUST/SWEEP alternation,
 *   * the melee hit consequence chain in the source order of unit.gd
 *     hurt_area_entered_deferred + take_damage + weapon.gd _on_Hitbox_hit_
 *     something: the charm_on_hit behavior roll FIRST (unit.gd:548-556), the
 *     exploding/burn/damage rolls, the kill hooks + DROPPED critical/cursed kill
 *     gold, the ProjectilesOnHitEffect children (spawned through the SHARED
 *     Projectiles path, bt_wproj_spawn_onhit_n), then the weapon-side lifesteal /
 *     tally / break_on_hit / custom_hit_sounds,
 *   * weapon.gd:299-301 reload_turrets_on_shoot (Weapon.shoot is shared by melee),
 *   * the cooldown decrement / re-arm in TICKS, and the per-slot runtime hooks
 *     (bt_weapon_melee_slot_reset/move/erase) for dense compaction.
 *
 * WHAT IT DOES NOT OWN (it calls these, never re-implements them)
 *   * weapon identity / cooldown slot arrays (WeaponInventory): wpn_* fields,
 *   * resolved per-slot stats   (WeaponStats): bt_weapon_stats,
 *   * geometry metadata         (WeaponData): BT_WEAPON_GEOM via BtWeaponDef.geom,
 *   * effect / event callbacks  (WeaponEffects): bt_wfx_* (lifesteal, on-hit,
 *     on-crit, on-kill, weapon explode, burn),
 *   * weapon audio (WeaponAudioRuntime, brotato_weapon_audio.h):
 *     bt_weapon_audio_shoot() on the recoil-end / hitbox-enable frame,
 *     bt_weapon_audio_hit() at the tail of each hit,
 *   * THE single enemy damage primitive bt_item_expl_hit_enemy
 *     (brotato_item_explosions.h), which takes a final thread-local
 *     BtEnemyHitContext (canonical ushort rows, is_elemental, one_shot,
 *     current_health_damage, owner_uid, attack_id, was_crit OUT) and applies G1,
 *     the crit, the full weapon hit bonus (bt_wfx_hit_bonus_pct over ctx->rows),
 *     force-crit-when-burning, one-shot / current-health damage, flat armor, the
 *     neutral hook and the kill.  bt_melee_primitive_hit() is the single call
 *     site.
 *
 * captains_sword's `stat_damage` PlayerHealthStatEffect bonus
 * (weapon_service.gd:504-508 apply_inverted_health_bonus) is applied here on the
 * pre-crit base, exactly as unit.gd:574 does.  Targets already charmed
 * (s->e_charmed[j]) are skipped BEFORE any crit/hit RNG, as unit.gd does for a
 * charm weapon's own follow-up hits.
 *
 * ---------------------------------------------------------------------------
 * BtState FIELDS REQUIRED (identity + events landed by Inventory/WeaponEffects)
 * ---------------------------------------------------------------------------
 *   WeaponInventory: wpn_id/wpn_tier/wpn_cursed/wpn_curse/wpn_wmod/wpn_fxcurse/
 *     wpn_cd (TICKS)/wpn_tracked/wpn_uid/wpn_uid_next/wpn_count/wpn_overflow/
 *     wpn_dirty + the starting queue.
 *   WeaponEffects: BtWeaponFxState wfx; float player_lifesteal_cd;
 *   melee runtime (this file):
 *     float          wpn_melee_t[BT_SLOTS];          swing elapsed FRAMES
 *     float          wpn_melee_atk_dur[BT_SLOTS];    shooting_data.atk_duration
 *     float          wpn_melee_back_dur[BT_SLOTS];   shooting_data.back_duration
 *     float          wpn_melee_recoil_dur[BT_SLOTS]; stats.recoil_duration
 *     float          wpn_melee_recoil[BT_SLOTS];     stats.recoil
 *     float          wpn_melee_maxrange[BT_SLOTS];   stats.max_range
 *     float          wpn_melee_atkdist[BT_SLOTS];    sweep atk_distance
 *     float          wpn_melee_rot[BT_SLOTS];        weapon facing angle phi
 *     float          wpn_melee_tdist[BT_SLOTS];      _current_target[1]
 *     unsigned short wpn_melee_target[BT_SLOTS];     live enemy slot, 0xFFFF none
 *     unsigned int   wpn_melee_attack_id[BT_SLOTS];  _get_next_attack_id()
 *     unsigned int   wpn_melee_hit[BT_SLOTS][BT_WPROJ_OBJECT_WORDS];
 *     unsigned short wpn_melee_shots[BT_SLOTS];      _nb_shots_taken
 *     unsigned char  wpn_melee_atk[BT_SLOTS];        next_attack_type
 *     unsigned char  wpn_melee_anim[BT_SLOTS];       0 idle, 1 thrust, 2 sweep
 *     unsigned char  wpn_melee_phase[BT_SLOTS];      0 idle,1 recoil,2 out,3 out2,4 back
 *     unsigned char  wpn_melee_hit_on[BT_SLOTS];     enable_hitbox window
 *     unsigned char  wpn_melee_shooting[BT_SLOTS];   _is_shooting
 *     unsigned char  wpn_melee_pending[BT_SLOTS];    retained for the BtState
 *                                                    layout; the deferred-shoot
 *                                                    frame no longer exists
 *                                                    (the swing starts in the
 *                                                    should_shoot tick)
 *     unsigned char  wpn_melee_ddr[BT_SLOTS];        deal_dmg_on_return
 *     unsigned char  wpn_melee_facing[BT_SLOTS];     player facing sign (idle angle)
 *
 * HOOKS (parent)
 *   bt_core_reset: bt_melee_reset(s)
 *   bt_enter_wave: bt_weapon_wave_begin(s) (WeaponInventory) dispatches
 *                  bt_weapon_melee_wave_begin_slot(s, slot) in slot order for a
 *                  melee copy -- the fresh-scene-per-wave node re-creation, ONE
 *                  get_next_cooldown(true) draw per copy.
 *   bt_core_step : bt_melee_step(s, player_moving, move_dir_x) once per combat
 *                  tick AFTER enemy SoA positions update and BEFORE the
 *                  projectile / explosion ticks, so a kill queued here drains
 *                  the same frame like every other hit site.
 *                  player_moving = (BT_MOVE_X[action] != 0 || BT_MOVE_Y[action] != 0);
 *                  move_dir_x = BT_MOVE_X[action] (facing only).
 *   define BT_WEAPON_MELEE_HOOKS so bt_weapon_melee_slot_reset/move/erase are
 *   emitted and dispatched by bt_weapon_rt_*.
 *
 * NOT SIMULATED HERE (owned elsewhere): the manual-aim path
 * (Utils.is_manual_aim == false here, so should_shoot needs a live target) and
 * structure/pet weapons.
 */
#pragma once

#include "brotato_weapon_types.h"
#include "brotato_weapons.h"
#include "brotato_weapon_audio.h"
#include "brotato_weapon_stats.h"
#include "brotato_weapon_effects.h"
#include "brotato_weapon_inventory.h"
/* the shared nested-child spawn (bt_wproj_spawn_onhit_n / bt_wproj_onhit_wrow)
 * and the object-identity bitset bound; included by brotato_core.h just before
 * this file, and explicitly here so the call sites are self-contained. */
#include "brotato_weapon_projectiles.h"

#define BT_MELEE_THRUST BT_WEAPON_ATTACK_THRUST
#define BT_MELEE_SWEEP  BT_WEAPON_ATTACK_SWEEP
#define BT_MELEE_ANIM_IDLE   0
#define BT_MELEE_ANIM_THRUST 1
#define BT_MELEE_ANIM_SWEEP  2
#define BT_MELEE_NO_TARGET 0xFFFFu

#ifndef BT_PI
#define BT_PI 3.14159265f
#endif
#define BT_MELEE_DEG_PER_RAD 57.2957795f
#define BT_MELEE_ATTACK_SPEED_STAT 15   /* core stat id: attack_speed */
#define BT_MELEE_RANGE_PROBE 50.0f      /* should_shoot: max_range + 50 */

/* The engine's own `pow(2, <float>)`: the pinned 3.7-dev1 build links the
 * binary64 `pow@GLIBC` (not powf/exp2f) because `pow(2, t)`'s second argument
 * is `real_t` = float and the float overload is not what the expression
 * selects; the exponent `-10 * t / d` is evaluated in float32 and widened
 * exactly.  Verified against the running pinned engine: an EXPO/EASE_OUT tween
 * of a REAL property reproduces this double expression's values bit-exactly on
 * every interpolation frame (11/11), while the float32 exp2f form differs on
 * 9/11 (tests/test_brotato_weapon_melee.cpp pins both). */
#if BT_HAS_F64
#define bt_melee_pow2(x) pow(2.0, (double)(x))
#endif


/* ---------------------------------------------------------------------------
 * TRANSCENDENTAL LEDGER -- how the pinned Godot 3.7 build evaluates every
 * trig/transcendental this sim mirrors, and where the sim's form differs.
 *
 * Verified in ~/tools/godot-src (3.7-dev1) and against the running pinned
 * binary ~/tools/bin/godot-3.7-dev1:
 *
 * 1. `real_t` is FLOAT32: core/math/math_defs.h typedefs float unless
 *    REAL_T_IS_DOUBLE, and the shipped/built engine is the default.  Every
 *    real_t operand (Vector2/Transform2D components, Node2D position/rotation,
 *    Tween elapsed/duration/delay, the easing equations' parameters) is float32.
 * 2. core/math/math_funcs.h OVERLOADS sin/cos/atan2/sqrt/pow on float and
 *    double, so the domain is decided per call site by its ARGUMENT type:
 *      * C++ Vector2/Transform2D methods take real_t -> the FLOAT forms:
 *          Vector2::angle()                        -> atan2f(y, x)
 *          Transform2D(rot,pos) / Vector2.rotated -> cosf/sinf(rot)
 *          Vector2::length()/distance_to()         -> sqrtf(float32 sum)
 *          Vector2::distance_squared_to()          -> float32 mul/add
 *      * GDScript builtins (cos/sin/atan2/sqrt/pow) receive Variant::REAL =
 *        binary64 -> the DOUBLE forms (::cos/::sin/::atan2/::sqrt/::pow).
 *      * easing_equations.h `pow(2, -10*t/d)`: the int `2` with a float32
 *        exponent selects the DOUBLE overload (verified: `nm` on a transcription
 *        shows U pow@GLIBC, and the pinned engine's own Tween values match it
 *        bit-for-bit).
 * 3. scene/animation/tween.cpp: real_t elapsed (`elapsed += p_delta`, completes
 *    on strictly `>`), real_t duration/delay, expo::out's tail in binary64 with
 *    one narrowing on return, `_calc_delta_val` subtracting the real_t endpoints.
 * 4. This module's sites -> engine counterpart, status:
 *      bt_melee_ease_expo_out   expo::out (binary64 + pow(2,double))  FIXED
 *      bt_melee_elapsed         Tween elapsed float32 accumulation    FIXED
 *      bt_atan2f (wpn_melee_rot) weapon.gd:253/261 `.angle()`         float32 OK
 *      bt_cosf/sinf (hitbox rect)Transform2D / rotated()              float32 OK
 *      bt_cosf/sinf (knockback) weapon.gd:358-359 GDScript cos/sin    FIXED (f64)
 *      bt_sqrtf (out_dist)      utils.gd:279 GDScript sqrt            binary64 in
 *                               the engine, float32 here: differs only when the
 *                               squared distance sits ~1 float32 ulp from a
 *                               range bound (documented residual)
 *      sweep angle              `0.9 * PI` (GDScript binary64)       FIXED exact
 *      durations (atk/back)     MeleeShootingData in GDScript binary64, narrowed
 *                               once at interpolate_property: the sim computes
 *                               them in float32 from float32 stats, so they can
 *                               differ by 1 float32 ulp (frame counts only at a
 *                               segment boundary) -- see the stats-precision note
 *      Vector2::rotated (proj)  Vector2.RIGHT.rotated(rot)*speed      float32 OK
 *   (The swing's own hitbox geometry uses bt_cosf/bt_sinf of the float32 sprite
 *    rotation, which is Transform2D's real_t cosf/sinf -- float32, correct.)
 * 5. BACKENDS: host and CUDA share glibc's float sinf/cosf/atan2f algorithms
 *    (brotato_math_cuda.h cargo-cults them, bit-exact by construction), so the
 *    float32 sites above are exact on both.  The DEVICE binary64 pow(2,x) is
 *    NOT glibc's: over 4.19e6 sampled float32 exponents, CUDA's exp2 differs
 *    from glibc's pow(2,.) by 1 ulp of double on 3.1 % and its pow on 11.8 %,
 *    which moves the FINAL float32 by <= 1 ulp with probability ~2^-29 per
 *    evaluation (measured: 0 float32 mismatches on 4.19e6 sampled (b,c,t,d) of
 *    this expression, device vs host).  MSL has no FP64 at all, so the Metal
 *    path uses the float32 exp2f form (BT_HAS_F64 == 0) and keeps the ~1 float32
 *    ulp difference the Metal host test already tolerates.
 * ------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------
 * Godot 3 Tween easing (scene/animation/tween.cpp _run_equation).
 * ------------------------------------------------------------------------- */
BT_HD static float bt_melee_ease_linear(float b, float c, float t, float d) {
    if (d <= 0.0f) return b + c;
    if (t <= 0.0f) return b;
    if (t >= d) return b + c;
    return c * t / d + b;
}
/* TRANS_EXPO, EASE_OUT, EXACTLY Godot 3 `expo::out`
 * (scene/animation/easing_equations.h:172-177):
 *
 *   static real_t out(real_t t, real_t b, real_t c, real_t d) {
 *       if (t == d) return b + c;
 *       return c * 1.001 * (-pow(2, -10 * t / d) + 1) + b;
 *   }
 *
 * The `1.001` is the engine's deliberate EXP overshoot: on every interpolated
 * frame the eased offset is 0.1 % LARGER than the plain `1 - 2^(-10 t/d)`, i.e.
 * up to 0.17 px of sprite travel and 0.0028 rad of sweep rotation ahead of a
 * naive exponential (both tween halves of every melee swing use it).  The
 * `t >= d` clamp is the caller's final-value override (`Tween::_tween_process`
 * applies `_get_final_val` on the completing frame), so it returns b + c.
 *
 * THE ARITHMETIC IS BINARY64, not float32: the engine's `real_t` is float
 * (core/math/math_defs.h without REAL_T_IS_DOUBLE), but `pow(2, <float>)`
 * resolves to the binary64 `pow(double, double)` overload, and the `1.001`
 * literal is a double, so `c * 1.001 * (-pow(...) + 1) + b` promotes the whole
 * tail to binary64 and the function's return converts to real_t ONCE.  The
 * exponent `-10 * t / d` stays float32 (int * real_t / real_t).  Verified
 * against the running pinned engine: an EXPO/EASE_OUT Tween of a REAL property
 * matches this expression bit-for-bit on 11/11 interpolation frames and the
 * float32 exp2f form on 2/11 (tests/test_brotato_weapon_melee.cpp).
 * On MSL (no FP64) the float32 form is kept; the Metal host test tolerates the
 * resulting ~1 float32 ulp difference (tests/test_brotato_metal.mm). */
BT_HD static float bt_melee_ease_expo_out(float b, float c, float t, float d) {
    if (d <= 0.0f) return b + c;
    if (t >= d) return b + c;
    /* t == 0 has no special case in the engine: c * 1.001 * (1 - 1) + b == b. */
#if BT_HAS_F64
    return (float)((double)b + (double)c * 1.001 * (1.0 - bt_melee_pow2(-10.0f * t / d)));
#else
    return b + c * 1.001f * (1.0f - metal::precise::exp2(-10.0f * t / d));
#endif
}

/* The engine's Tween accumulates `data.elapsed += p_delta` in real_t (float32;
 * tween.cpp:752-753, tween.h InterpolateData::elapsed), so after `frames` steps
 * of a segment the elapsed time is the float32 SUM of the deltas, NOT the float32
 * product `frames * BT_DT`.  They diverge from the 6th step on
 * (6 * BT_DT == 0.10000000894f vs the accumulated 0.10000000149f), which shifts
 * every eased value on the later frames of each segment -- measured against the
 * pinned engine (tests/test_brotato_weapon_melee.cpp pins the two sequences).
 * `elapsed` is cast to real_t at the run_equation call, so the segment-relative
 * frame count is all the caller needs. */
BT_HD static float bt_melee_elapsed(int frames) {
    float acc = 0.0f;
    for (int i = 0; i < frames; i++) acc += BT_DT;
    return acc;
}

/* ---------------------------------------------------------------------------
 * Weapon attach offsets (player.tscn Weapons/<count>/Attach<i>.position), the
 * layouts weapons_container.gd update_weapons_positions rebuilds.
 * ------------------------------------------------------------------------- */
#define BT_MELEE_MAX_ATTACH 6
typedef struct { float x, y; } BtMeleeAttach;
BT_TABLE BtMeleeAttach BT_MELEE_ATTACH[7][BT_MELEE_MAX_ATTACH] = {
    { {0,0},{0,0},{0,0},{0,0},{0,0},{0,0} },
    { {0,30},{0,0},{0,0},{0,0},{0,0},{0,0} },            /* 1 weapon  */
    { {40,20},{-40,20},{0,0},{0,0},{0,0},{0,0} },        /* 2 weapons */
    { {40,20},{-40,20},{0,-45},{0,0},{0,0},{0,0} },      /* 3 weapons */
    { {40,35},{-40,35},{-40,-30},{40,-30},{0,0},{0,0} }, /* 4 weapons */
    { {35,35},{-35,35},{-55,-20},{55,-20},{0,-55},{0,0} },/* 5 weapons */
    { {35,45},{-35,45},{-60,0},{60,0},{-35,-45},{35,-45} }/* 6 weapons */
};
#define BT_MELEE_WEAPONS_OX 0.0f
#define BT_MELEE_WEAPONS_OY (-24.0f)

BT_HD static BT_CONST_PTR(BtMeleeAttach) bt_melee_attach_of(int count, int slot) {
    int c = count < 1 ? 1 : (count > 6 ? 6 : count);
    int s = slot < 0 ? 0 : (slot > 5 ? 5 : slot);
    return &BT_MELEE_ATTACH[c][s];
}

/* weapon.gd attach(): weapon.position = attach_to - _attach.position. */
BT_HD static void bt_melee_origin(BT_DEVICE const BtState* s, int count, int slot,
                                  BT_CONST_PTR(BtWeaponGeom) G,
                                  BT_THREAD float* ox, BT_THREAD float* oy) {
    BT_CONST_PTR(BtMeleeAttach) a = bt_melee_attach_of(count, slot);
    *ox = s->px + BT_MELEE_WEAPONS_OX + a->x - G->attach_x;
    *oy = s->py + BT_MELEE_WEAPONS_OY + a->y - G->attach_y;
}

/* ---------------------------------------------------------------------------
 * MeleeShootingData (weapons/shooting_behaviors/melee_shooting_data.gd).
 * ------------------------------------------------------------------------- */
BT_HD static float bt_melee_range_factor(float distance, float atk_spd) {
    float denom = 70.0f * (1.0f + atk_spd / 3.0f);
    if (denom < 70.0f) denom = 70.0f;
    if (denom > 120.0f) denom = 120.0f;
    float f = distance / denom;
    return f > 0.0f ? f : 0.0f;
}
BT_HD static float bt_melee_atk_duration(float atk_spd, float range_factor) {
    float base = BT_WEAPON_MELEE_BASE_ATK_DURATION - atk_spd / 10.0f;
    if (base < 0.01f) base = 0.01f;
    return base + range_factor * 0.15f;
}
BT_HD static float bt_melee_back_duration(float atk_spd) {
    if (atk_spd > 0.0f) return BT_WEAPON_MELEE_BASE_ATK_DURATION / (1.0f + atk_spd * 3.0f);
    return BT_WEAPON_MELEE_BASE_ATK_DURATION;
}

/* ---------------------------------------------------------------------------
 * Targeting: weapon.gd get_nearest_no_max over the Range Area (max_range + 200)
 * then should_shoot's is_between(tdist, min_range, max_range + 50).
 *
 * MEMBERSHIP is a circle-vs-BODY-shape overlap, not a centre distance test:
 * weapon.tscn's Range is an Area2D at the weapon origin with collision_mask 5
 * (layer_1 "neutral" + layer_3 "enemies") whose CollisionShape2D radius is set
 * to `max_range + DETECTION_RANGE` by weapon.gd:155, and it detects the unit's
 * own body CollisionShape2D (CircleShape2D radius body_radius at body_x/body_y
 * root-local: enemy.tscn's Unit "Collision" / neutral tree.tscn's).  So neutral
 * trees (neutral.tscn collision_layer = 1) are targets too, exactly as for the
 * ranged weapons (bt_ranged_pick_target, brotato_weapon_ranged.h:257-283).
 * The chosen target is then the nearest by CENTRE distance with `dist^2`
 * STRICTLY greater than min_range^2 (utils.gd:272-275), ties keeping the lowest
 * slot.  `*out_dist` is the engine's `sqrt(distance_squared_to(from))` --
 * utils.gd:279 square-roots the FLOAT32 squared distance in GDScript binary64;
 * the sim keeps it float32 (see the transcendental ledger in this header). */
BT_HD static int bt_melee_pick_target(BT_DEVICE BtState* s, float ox, float oy,
                                      float min_range, float detect_range,
                                      BT_THREAD unsigned short* out_slot,
                                      BT_THREAD float* out_dist) {
    int best = -1;
    float best_d2 = 0.0f;
    float min2 = min_range * min_range;
    for (int j = bt_enemy_next(s, 0, 1); j < (int)s->e_used; j = bt_enemy_next(s, j + 1, 1)) {
        if (!s->ealive[j]) continue;
        float dx = s->ex[j] - ox, dy = s->ey[j] - oy;
        float d2 = dx * dx + dy * dy;
        /* Range-circle vs body-circle overlap (exact for two circles). */
        BT_CONST_PTR(BtBodyPhysics) G = bt_kgeom(s, j);
        float bx = dx + G->body_x, by = dy + G->body_y;
        float reach = detect_range + G->body_radius;
        if (bx * bx + by * by > reach * reach) continue;
        if (d2 <= min2) continue;
        if (best < 0 || d2 < best_d2) { best = j; best_d2 = d2; }
    }
    if (best < 0) { *out_slot = (unsigned short)BT_MELEE_NO_TARGET; *out_dist = 0.0f; return 0; }
    *out_slot = (unsigned short)best;
    *out_dist = bt_sqrtf(best_d2);
    return 1;
}

/* can_attack_while_moving: RunData.get_player_effect_bool.  player_run_data.gd:485
 * INITIALISES it to 1, so every profile attacks while moving by default; only an
 * effect that zeroes the key (Soldier) disables it.  The parent seeds the scalar,
 * so this reads the seeded value. */
BT_HD static int bt_melee_can_move_attack(BT_DEVICE BtState* s) {
    return bt_wfx_scalar_keyed(s, BT_ISTR_CAN_ATTACK_WHILE_MOVING) > 0.0f ? 1 : 0;
}

/* Weapon effect rows for a weapon id (weapon.gd hitbox.effects) are the
 * CONTIGUOUS canonical slice BT_WEAPON_EFF_INDEX[fx_first .. fx_first+fx_count);
 * callers pass it directly as BT_CONST_PTR(unsigned short) (a constant pointee),
 * alongside the copy's own BT_DEVICE curse array (s->wpn_fxcurse[slot], read
 * straight from the stored draws, no accessor copy).  fx_count is bounded by the
 * widest shipped weapon (2 top-level effects == BT_WEAPON_FX_CURSE_MAX) with
 * room to spare. */
#define BT_MELEE_MAX_FX 8

/* is_facing_right(float rotation_degrees) == degrees in (-90, 90). */
BT_HD static int bt_melee_facing_right(float rot) {
    float deg = rot * BT_MELEE_DEG_PER_RAD;
    while (deg <= -180.0f) deg += 360.0f;
    while (deg > 180.0f) deg -= 360.0f;
    return (deg > -90.0f && deg < 90.0f) ? 1 : 0;
}

/* The number of Tween steps a segment of `dur` seconds needs: the smallest k
 * with `k * BT_DT > dur` in the engine's OWN float32 accumulation.  Tween::
 * _tween_process does `data.elapsed += p_delta` and completes on
 * `data.elapsed > (delay + duration)`, a STRICTLY greater test, so a duration
 * that is an exact multiple of the frame delta takes one step MORE than the
 * exact quotient (0.1 s -> 6*BT_DT == 0.1f -> NOT complete -> 7 steps; the
 * trailing 0.2 s return takes 13).  Accumulating BT_DT the way the engine does
 * keeps the count bit-identical on CPU/CUDA/Metal and exact at that boundary;
 * the emit/deferred frames are added at the segment boundaries in bt_melee_anim.
 *
 * Verified against the pinned-Godot oracle: the thrust claw's hitbox is live on
 * engine frames 8..24 and the sweep chopper's on 8..25, with attack_frames
 * 39 / 40. */
BT_HD static int bt_melee_frames(float dur) {
    if (dur <= 0.0f) return 0;
    float acc = 0.0f;
    int k = 0;
    while (k < 600 && !(acc > dur)) { acc += BT_DT; k++; }   /* 600 = 10 s bound */
    return k;
}

/* Animated sprite transform for the current swing frame.  `wpn_melee_t` counts
 * whole frames since the swing began; phase 0 is returned once the swing ends. */
BT_HD static void bt_melee_anim(BT_DEVICE const BtState* s, int slot, int anim,
                                BT_CONST_PTR(BtWeaponGeom) G,
                                BT_THREAD float* px, BT_THREAD float* py,
                                BT_THREAD float* rot, BT_THREAD int* hit_on,
                                BT_THREAD int* phase) {
    float sx = G->sprite_x, sy = G->sprite_y;
    float t = s->wpn_melee_t[slot];              /* frames */
    float rec = s->wpn_melee_recoil_dur[slot];
    float atk = s->wpn_melee_atk_dur[slot];
    float back = s->wpn_melee_back_dur[slot];
    float rl = s->wpn_melee_recoil[slot];
    float mrange = s->wpn_melee_maxrange[slot];
    int ddr = (int)s->wpn_melee_ddr[slot];
    int rec_f = bt_melee_frames(rec);
    int back_f = bt_melee_frames(back);
    *px = sx; *py = sy; *rot = 0.0f; *hit_on = 0; *phase = 0;

    if (anim == BT_MELEE_ANIM_THRUST) {
        int out_f = bt_melee_frames(atk * 0.5f);
        int hit_start = rec_f + 1;
        int hit_end = hit_start + out_f + 1;
        int swing_end = hit_end + back_f;
        float half_dur = atk * 0.5f;
        if (t < (float)rec_f) {
            *px = bt_melee_ease_expo_out(sx, -rl, bt_melee_elapsed((int)t), rec);
            *hit_on = 0; *phase = 1;
        } else if (t < (float)hit_start) {
            *px = sx - rl; *hit_on = 0; *phase = 1;   /* recoil done, awaiting yield */
        } else if (t < (float)hit_end) {
            float from = sx - rl;
            *px = bt_melee_ease_expo_out(from, (sx + mrange) - from,
                                         bt_melee_elapsed((int)t - hit_start), half_dur);
            *hit_on = 1; *phase = 2;
        } else if (t < (float)swing_end) {
            float from = sx + mrange;
            *px = bt_melee_ease_expo_out(from, sx - from,
                                         bt_melee_elapsed((int)t - hit_end), back);
            *hit_on = ddr; *phase = 3;
        }
        return;
    }

    if (anim != BT_MELEE_ANIM_SWEEP) return;   /* idle / unknown: keep initial */

    float atk_distance = s->wpn_melee_atkdist[slot];
    float side_range = atk_distance * 0.5f;
    float sweep_angle = BT_WEAPON_MELEE_SWEEP_ANGLE;
    float shd = atk / (float)BT_WEAPON_MELEE_SWEEP_HALF_DIVISOR;
    int shd_f = bt_melee_frames(shd);
    int hit_start = rec_f + 1;
    int out2_start = hit_start + shd_f + 1;
    int hit_end = out2_start + shd_f + 1;
    int swing_end = hit_end + back_f;
    float side_a, side_b, angle_a, angle_b;
    if (bt_melee_facing_right(s->wpn_melee_rot[slot])) {
        side_a = -side_range; side_b = side_range;
        angle_a = -sweep_angle; angle_b = sweep_angle;
    } else {
        side_a = side_range; side_b = -side_range;
        angle_a = sweep_angle; angle_b = -sweep_angle;
    }
    float x_out = sx + (float)(int)atk_distance * 0.75f;
    if (t < (float)rec_f) {
        float lt = bt_melee_elapsed((int)t);
        *px = bt_melee_ease_expo_out(sx, -rl, lt, rec);
        *py = bt_melee_ease_expo_out(sy, side_a, lt, rec);
        *rot = bt_melee_ease_expo_out(0.0f, angle_a, lt, rec);
        *hit_on = 0; *phase = 1;
    } else if (t < (float)hit_start) {
        *px = sx - rl; *py = sy + side_a; *rot = angle_a; *hit_on = 0; *phase = 1;
    } else if (t < (float)out2_start) {
        float lt = bt_melee_elapsed((int)t - hit_start);
        *px = bt_melee_ease_linear(sx - rl, x_out - (sx - rl), lt, shd);
        *py = bt_melee_ease_linear(sy + side_a, -side_a, lt, shd);
        *rot = bt_melee_ease_linear(angle_a, -angle_a, lt, shd);
        *hit_on = 1; *phase = 2;
    } else if (t < (float)hit_end) {
        float lt = bt_melee_elapsed((int)t - out2_start);
        *px = bt_melee_ease_linear(x_out, (sx - rl) - x_out, lt, shd);
        *py = bt_melee_ease_linear(sy, side_b, lt, shd);
        *rot = bt_melee_ease_linear(0.0f, angle_b, lt, shd);
        *hit_on = 1; *phase = 3;
    } else if (t < (float)swing_end) {
        float lt = bt_melee_elapsed((int)t - hit_end);
        *px = bt_melee_ease_expo_out(sx - rl, rl, lt, back);
        *py = bt_melee_ease_expo_out(sy + side_b, -side_b, lt, back);
        *rot = bt_melee_ease_expo_out(angle_b, -angle_b, lt, back);
        *hit_on = ddr; *phase = 4;
    }
}

/* Oriented-rect / circle overlap (the clamp kernel bt_proj_hits_enemy uses). */
BT_HD static int bt_melee_rect_hits_circle(float cx, float cy, float ang,
                                           float hx, float hy,
                                           float px, float py, float pr) {
    float sc = bt_cosf(ang), ss = bt_sinf(ang);
    float dx = px - cx, dy = py - cy;
    float lx = sc * dx + ss * dy;
    float ly = -ss * dx + sc * dy;
    float qx = lx < -hx ? -hx : (lx > hx ? hx : lx);
    float qy = ly < -hy ? -hy : (ly > hy ? hy : ly);
    float ex = lx - qx, ey = ly - qy;
    return (ex * ex + ey * ey) <= pr * pr ? 1 : 0;
}

/* Knockback (unit.gd take_damage): 0.2*hitbox_dir + 0.8*attacker->victim times
 * the final amount; negative -> pull formula (+ decaying speed), piercing ->
 * bt_piercing_knockback_amount.  Every shipped melee weapon has knockback >= 0.
 *
 * unit.gd:276-283: the weighted direction uses the ATTACKER NODE's global
 * position -- `players_ref[from_player_index].global_position`, i.e. the PLAYER
 * centre, NOT the weapon's attach point (the 0.2/0.8 mix and the pull formula's
 * direction+distance are all measured from the player; the projectile path uses
 * the same convention, brotato_weapon_projectiles.h:382).  The victim's movement
 * input is the unit's `_current_movement`; the only per-unit movement the core
 * retains is the cached contact velocity of the physics body, which is also what
 * the projectile path feeds (brotato_weapon_projectiles.h:388).
 *
 * `hbdir` is weapon.gd:358-359 `Vector2(cos(rotation), sin(rotation))`: GDScript's
 * cos/sin take a GDScript float (binary64) and the Vector2 constructor narrows
 * each component ONCE to float32 -- so the direction is the BINARY64 cos/sin
 * narrowed, not the float32 cosf/sinf (the float32 form is the MSL fallback;
 * every melee weapon ships knockback >= 0, so this is the only form used here).
 * `rot` is the float32 aim angle, widened exactly. */
BT_HD static void bt_melee_knockback(BT_DEVICE BtState* s, int enemy, float ox, float oy,
                                     float rot, float kb, float kb_pierce,
                                     BtVec2 attacker_movement) {
#if BT_HAS_F64
    BtVec2 hbdir = bt_v2((float)cos((double)rot), (float)sin((double)rot));
#else
    BtVec2 hbdir = bt_v2(bt_cosf(rot), bt_sinf(rot));
#endif
    /* unit.gd:281 measures BOTH the weighted direction and the pull distance
     * term from the victim's own `global_position` -- the MODE_CHARACTER enemy
     * Node transform, which the server only writes back one frame later, so at
     * this frame's hit it still reads the PRE-step position (s->ehx/s->ehy),
     * exactly like the enemy contact Hitbox area test (brotato_state.h:749-765).
     * The player is MODE_KINEMATIC and writes global_position in
     * _physics_process, so s->px/s->py is already this frame's value. */
    BtVec2 victim = bt_v2(s->ehx[enemy], s->ehy[enemy]);
    BtVec2 attacker = bt_v2(s->px, s->py);
    BtVec2 dir = bt_knockback_direction_weighted(hbdir, attacker, victim);
    /* unit.gd:348: die() seeds its stored vector with the WEIGHTED direction
     * times the RAW hitbox amount floored at MIN_DEATH_KNOCKBACK_AMOUNT -- not
     * the pull/piercing-adjusted amount below.  Kept per slot so a lethal hit
     * of this frame can start the node's death drift with it. */
    BtVec2 dkb = bt_death_knockback(dir, kb);
    s->edkbx[enemy] = dkb.x; s->edkby[enemy] = dkb.y;
    BT_CONST_PTR(BtBodyPhysics) G = bt_kgeom(s, enemy);
    float amount = kb;
    if (amount < 0.0f) {
        float adj = bt_pull_knockback_amount(amount, victim, G->hurt_radius,
                                             s->espd[enemy], s->phys.bodies[enemy].vel,
                                             attacker, BT_PLAYER_HURT_R,
                                             s->player_speed, attacker_movement,
                                             kb_pierce, G->knockback_resistance);
        s->edecay[enemy] = bt_add_decaying_speed((int)(-adj), s->espd[enemy], s->edecay[enemy]);
        amount = adj;
    } else if (kb_pierce > 0.0f) {
        amount = bt_piercing_knockback_amount(amount, kb_pierce, G->knockback_resistance);
    }
    s->ekbx[enemy] = dir.x * amount;
    s->ekby[enemy] = dir.y * amount;
}

/* weapon_service.gd:504-508 apply_inverted_health_bonus for a weapon
 * PlayerHealthStatEffect row (captains_sword `stat_damage`). */
BT_HD static float bt_melee_health_stat_bonus(BT_DEVICE BtState* s,
                                              BT_CONST_PTR(unsigned short) rows, int n_rows) {
    float bonus = 0.0f;
    float maxhp = s->max_hp;
    if (maxhp == 0.0f) return 0.0f;
    for (int i = 0; i < n_rows; i++) {
        int ri = (int)rows[i];
        if (ri < 0 || ri >= BT_NITEM_EFF_ROWS) continue;
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[ri];
        if (!bt_eff_is(E, BT_ISTR_STAT_DAMAGE)) continue;
        float per = (float)(E->nb_stat_scaled > 0 ? E->nb_stat_scaled : 1);
        float missing = 1.0f - s->hp / maxhp;
        if (missing < 0.0f) missing = 0.0f;
        bonus += bt_roundf((float)E->value * (missing * 100.0f / per));
    }
    return bonus;
}

/* The single enemy-damage call site.  ctx carries the canonical weapon-row view
 * and returns the resolved crit; the primitive owns G1 / crit / bonus% /
 * force-crit / one-shot / current-health / armor / neutral / kill. */
BT_HD static float bt_melee_primitive_hit(BT_DEVICE BtState* s, int enemy, float base,
                                          int is_crit, float crit_dmg,
                                          BT_THREAD BtEnemyHitContext* ctx,
                                          BT_THREAD int* killed) {
    return bt_item_expl_hit_enemy(s, enemy, base, is_crit, crit_dmg,
                                  0.0f, 0.0f, 0.0f,   /* burning applied separately */
                                  0.0f, 0.0f,         /* no ChanceStatDamageEffect proc */
                                  0, 1, killed, ctx);
}

/* Apply one melee hit to one enemy, in the source order of unit.gd
 * hurt_area_entered_deferred + take_damage, then weapon.gd
 * _on_Hitbox_hit_something.  Returns the break gold (>0 => the caller removes
 * the weapon slot). */
BT_HD static int bt_melee_hit_enemy(BT_DEVICE BtState* s, int slot, unsigned short wid,
                                    BT_THREAD const BtPlayerWeaponStats* st,
                                    int enemy, float ox, float oy, float rot,
                                    BT_CONST_PTR(unsigned short) rows,
                                    BT_DEVICE const float* row_curse,
                                    int n_rows) {
    float ex = s->ex[enemy], ey = s->ey[enemy];
    float base = (float)st->damage;
    float crit_dmg = st->crit_damage;
    unsigned int uid = s->wpn_uid[slot];
    int attack_id = (int)s->wpn_melee_attack_id[slot];
    int is_elem = st->is_elemental ? 1 : 0;

    /* (0) unit.gd:548-556: EVERY hurt first runs the enemy effect behaviors'
     *     on_hurt, so the charm_on_hit behavior rolls ONCE per hurt BEFORE the
     *     exploding / damage rolls.  The behavior only exists while a charm
     *     source is owned; with none the accumulated chance is 0 and bt_chance
     *     draws nothing (the charm_enemy_effect_behavior.tscn max_charm_count is
     *     999).  Same consumer the projectile hit uses. */
    if (bt_wfx_charm_roll(s, enemy, (int)s->charm_count[0], 999, rows, n_rows, 1.0f))
        bt_enemy_lifecycle_charm(s, enemy, 0);

    /* (0b) the SAME on_hurt phase also feeds the enemy_percent_damage_taken
     *      behavior (the melee lute rows 857/863/864/865, key stat_all; the
     *      ice_cube item row is scanned internally from bk_active with the same
     *      scaling gate), so the bonus is live for THIS hit's damage.
     *      WeaponInteractionRuntime owns bt_enemy_hit_* (brotato_enemy_hit_
     *      effects.h, included by brotato_core.h): no RNG here, its per-enemy
     *      timers tick once per step in bt_core_step (never here), and the
     *      get_bonus_damage term is folded once inside bt_item_expl_hit_enemy
     *      (never added by this module).  st->sc_key is the hitbox's own
     *      effective scaling view. */
    {
        /* the hit's own scaling view as a MASK over core stat ids (one scalar OR
         * per key: no array copy, no pointer across an address space), then the
         * source-order application -- the item bucket first, then this hitbox's
         * own rows with each row's curse scalar. */
        unsigned int pdt_mask = 0u;
        for (int q = 0; q < (int)st->n_scaling; q++)
            pdt_mask = bt_enemy_hit_mask_add(pdt_mask, st->sc_key[q]);
        bt_enemy_hit_bucket_apply(s, enemy, pdt_mask);
        for (int i = 0; i < n_rows; i++)
            (void)bt_enemy_hit_apply_row(s, enemy, (int)rows[i],
                                         (row_curse != 0 && i < BT_WEAPON_FX_CURSE_MAX)
                                             ? row_curse[i] : 0.0f, pdt_mask);
    }

    /* (1) unit.gd:552-570: an ExplodingEffect rolls `chance`; on success the
     *     explosion spawns at the target and the target takes NO direct damage.
     *     unit.gd:569 `if from != null and from is Weapon:` then connects the
     *     spawned explosion's hit_something/killed_something to that weapon, so
     *     every kill the explosion causes is credited to the firing copy
     *     (weapon.gd:203-230 wave tally + GainStatEveryKilledEnemiesEffect).  A
     *     melee hitbox's `from` IS its Weapon node (weapon.gd:148
     *     `_hitbox.from = self`), so this unit-side roll needs the same source
     *     attribution the ranged path sets (bt_item_expl_set_source from
     *     bt_wproj_step); a copy that no longer owns the uid credits nobody.
     *     The WEAPON-side roll below (weapon.gd:416-427) never connects --
     *     weapon.gd builds its `_explosion_args` without `from` and spawns in the
     *     weapon's own handler -- so it stays on bt_wfx_weapon_explode's boolean
     *     form and credits nobody. */
    int exploding = 0;
    for (int i = 0; i < n_rows && !exploding; i++) {
        int pj = bt_wfx_weapon_explode_src(s, rows[i], ex, ey, base, st->crit_chance, crit_dmg,
                                           st->burn_chance, st->burn_dmg, st->burn_dur,
                                           (row_curse != 0 && i < BT_WEAPON_FX_CURSE_MAX)
                                               ? row_curse[i] : 0.0f);
        if (pj >= 0) {
            int eslot = bt_weapon_slot_from_uid(s, uid);
            if (eslot >= 0)
                bt_item_expl_set_source(s, pj, uid, (int)BT_WEAPONS[wid].fx_first,
                                        (int)BT_WEAPONS[wid].fx_count, eslot);
            exploding = 1;
        }
    }

    /* (2) burning (unit.gd:581-585, inside `if not is_exploding`): the chance roll
     *     comes FIRST (GDScript short-circuits left to right), then the
     *     `not hitbox.is_healing` and `can_burn_enemies > 0` gates -- so an
     *     is_healing hitbox still consumes its burn roll and applies nothing. */
    if (!exploding)
        (void)bt_wfx_weapon_burn_apply(s, enemy,
                                       st->is_healing ? 0.0f
                                                      : bt_wfx_scalar_keyed(s, BT_ISTR_CAN_BURN_ENEMIES),
                                       st->burn_chance, st->burn_dmg, st->burn_dur);

    int was_crit = 0, one_shot = 0, killed = 0;
    float killing_blow = 0.0f;
    if (!exploding) {
        /* (3) take_damage: stat_damage bonus, crit roll, one-shot roll, then the
         *     shared primitive. */
        float hp_bonus = bt_melee_health_stat_bonus(s, rows, n_rows);
        int crit_roll = bt_chance(s, st->crit_chance) ? 1 : 0;
        one_shot = bt_wfx_one_shot(s, rows, n_rows);
        /* unit.gd:288-294: a successful OneShotOnHitEffect plays its own sound
         * (line 294, before take_damage's hurt/crit pick at 298) -- i.e. BEFORE
         * the shared primitive, which owns that pick. */
        if (one_shot) bt_combat_audio_one_shot(s);
        float factor = (bt_kind_class(s->ekind[enemy]) == 2) ? 1000.0f : 100.0f;
        float cur_hp_dmg = bt_wfx_current_health_damage(s, enemy, rows, n_rows, factor, 1.0f);

        BtEnemyHitContext ctx;
        ctx.rows = rows;
        ctx.count = n_rows;
        ctx.is_elemental = is_elem;
        ctx.one_shot = one_shot;
        ctx.current_health_damage = cur_hp_dmg;
        ctx.owner_uid = uid;
        ctx.attack_id = attack_id;
        ctx.was_crit = 0;

        killing_blow = bt_melee_primitive_hit(s, enemy, base + hp_bonus, crit_roll, crit_dmg,
                                              &ctx, &killed);
        was_crit = ctx.was_crit;
    }

    /* ---------------------------------------------------------------------
     * unit.gd hurt_area_entered_deferred tail (the UNIT side, before the
     * hitbox signal) then weapon.gd _on_Hitbox_hit_something (the WEAPON side).
     * The two sides are ordered by unit.gd:612 `hitbox.hit_something(...)`,
     * which is the LAST statement of the unit-side handler, so every draw here
     * lands in this order (unit.gd:328-333 knockback, :344-382 kill branch,
     * :588-608 projectiles_on_hit children, then weapon.gd:412 lifesteal,
     * :417-427 exploding rows, :430-431 tally/break, :432-433 hit sound).
     * --------------------------------------------------------------------- */

    /* (4) knockback (unit.gd:324-335: computed inside take_damage, after the
     *     health write and before the kill branch). */
    bt_melee_knockback(s, enemy, ox, oy, rot, (float)st->knockback,
                       st->knockback_piercing, bt_v2(0.0f, 0.0f));

    /* (5) kill hooks (weapon.gd on_killed_something + unit.gd:344-382). */
    if (killed) {
        int gold_added = 0;
        bt_wfx_on_player_kill(s, enemy, was_crit, killing_blow, rows, n_rows, &gold_added);
        bt_wfx_on_weapon_kill(s, &s->wfx, slot, uid, attack_id, rows, n_rows, row_curse);
        /* unit.gd:367-382: gold_on_crit_kill is paid with RunData.add_gold
         * (run_data.gd:810-818) -- a DIRECT purse credit: no coin entity, no XP,
         * and none of the player.gd/main.gd gold-pickup path, so NOT
         * bt_loot_spawn_gold (that is the coin consumer).  Cursed-kill gold is
         * paid by the curse runtime inside bt_item_expl_hit_enemy, not here. */
        if (gold_added > 0) s->materials += (float)gold_added;
    }

    /* (6) unit.gd:586-604 projectiles_on_hit: a melee Hitbox carries its own
     *      ProjectilesOnHitEffect children (weapon.gd:134-140 sets
     *      hitbox.projectiles_on_hit = [effect.value, nested stats, auto_target]).
     *      They spawn at the target AFTER take_damage -- so even a killing hit
     *      spawns them -- and BEFORE the weapon-side handler (unit.gd:612 is the
     *      last statement of the unit handler), through the SAME shared
     *      construction the ranged projectile hit uses
     *      (Projectiles' bt_wproj_spawn_onhit_n).  Each child draws its own
     *      rand_range(-PI, PI) (unit.gd:592), so this block MUST precede the
     *      weapon-side lifesteal draw below (weapon.gd:412). */
    {
        BtWeaponOnHit onhit;
        if (bt_weapon_stats_onhit(s, slot, &onhit) && onhit.count > 0) {
            BtWProjChild ch;
            ch.dmg = onhit.dmg;
            ch.crit_chance = onhit.crit_chance; ch.crit_dmg = onhit.crit_dmg;
            ch.burn_chance = onhit.burn_chance; ch.burn_dmg = onhit.burn_dmg;
            ch.burn_dur = onhit.burn_dur;
            ch.max_range = onhit.max_range; ch.speed = onhit.speed;
            ch.rad = 0.0f; ch.offx = 0.0f; ch.offy = 0.0f;
            ch.piercing = onhit.piercing; ch.pierce_reduce = onhit.pierce_reduce;
            ch.bounce = onhit.bounce; ch.bounce_reduce = onhit.bounce_reduce;
            ch.shape = BT_SHAPE_CIRCLE;
            /* the immutable nested stats row (the child's scene/audio/etc.) from
             * the melee weapon's own canonical fx span. */
            unsigned short wrow = bt_wproj_onhit_wrow(BT_WEAPONS[wid].fx_first,
                                                      BT_WEAPONS[wid].fx_count);
            /* 0: the melee phase runs after the projectile phase, so the child
             * is in the tree before this frame's step and moves with the next
             * projectile phase. */
            bt_wproj_spawn_onhit_n(s, ex, ey, enemy, 0, (int)onhit.count, (int)onhit.auto_target,
                                   wrow, &ch, uid, wid,
                                   s->wpn_cursed[slot] ? s->wpn_curse[slot] : 0.0f,
                                   (float)st->lifesteal);
        }
    }

    /* (7) weapon.gd:412 (FIRST statement of _on_Hitbox_hit_something):
     *     RunData.manage_life_steal -> `randf() < lifesteal` BEFORE the shared
     *     LifestealTimer gate in player.gd:724-728, so one global draw happens
     *     on every weapon hit. */
    (void)bt_wfx_lifesteal(s, &s->player_lifesteal_cd, st->lifesteal);

    /* (8) weapon.gd:416-427 _on_Hitbox_hit_something rolls each ExplodingEffect
     *     row independently of the unit-side roll above (two chance rolls,
     *     source) -- BEFORE the tally/break below (weapon.gd:430-431). */
    for (int i = 0; i < n_rows; i++) {
        (void)bt_wfx_weapon_explode(s, rows[i], ex, ey, base, st->crit_chance, crit_dmg,
                                    st->burn_chance, st->burn_dmg, st->burn_dur,
                                    (row_curse != 0 && i < BT_WEAPON_FX_CURSE_MAX)
                                        ? row_curse[i] : 0.0f);
    }

    /* (9) weapon.gd on_weapon_hit_something: per-attack tally + break_on_hit.
     *     A non-zero return drops gold and Inventory removes the slot. */
    int break_gold = bt_wfx_on_weapon_hit(s, &s->wfx, slot, uid, attack_id, rows, n_rows, row_curse);

    /* (10) weapon.gd:432-433 _on_Hitbox_hit_something tail: with a non-empty
     *      custom_hit_sounds, SoundManager2D.play(get_rand_element(...), pos, -2,
     *      0.1) -- ONE bounded global draw, LAST in the handler.  Only the Abyss
     *      DLC lute ships custom_hit_sounds, so this is a no-op elsewhere. */
    bt_weapon_audio_hit(s, wid);
    return break_gold;
}

/* Begin one swing (weapon.gd shoot + MeleeWeapon.shoot). */
BT_HD static void bt_melee_begin_swing(BT_DEVICE BtState* s, int slot, int count,
                                       unsigned short wid,
                                       BT_THREAD const BtPlayerWeaponStats* st,
                                       BT_CONST_PTR(BtWeaponGeom) G) {
    /* weapon.gd:299-301 (Weapon.shoot, shared by MeleeWeapon and RangedWeapon):
     * every reload_turrets_on_shoot effect row emits wanted_to_reset_turrets_
     * cooldown, which entity_spawner.gd:570 turns into a zeroed BuilderTurret
     * cooldown.  ZERO RNG (the melee carrier is the Abyss war_hammer). */
    {
        unsigned short fr[BT_MELEE_MAX_FX];
        int n = bt_wfx_fx_rows(wid, fr, BT_MELEE_MAX_FX);
        if (n > 0) (void)bt_wfx_reload_turrets(s, fr, n);
    }

    s->wpn_melee_shots[slot]++;

    float ox, oy;
    bt_melee_origin(s, count, slot, G, &ox, &oy);
    unsigned short tgt = s->wpn_melee_target[slot];
    float phi, dist;
    if (tgt != BT_MELEE_NO_TARGET && tgt < BT_MAX && s->ealive[tgt]) {
        float spread = bt_wfx_accuracy_spread(s, st->accuracy);
        phi = bt_atan2f(s->ey[tgt] - oy, s->ex[tgt] - ox) + spread;
        dist = bt_v2dist(bt_v2(ox, oy), bt_v2(s->ex[tgt], s->ey[tgt]));
    } else {
        phi = s->wpn_melee_facing[slot] ? 0.0f : BT_PI;
        dist = (float)st->max_range;
    }
    s->wpn_melee_rot[slot] = phi;
    s->wpn_melee_attack_id[slot] += 1u;

    float atk_spd = st->atk_spd;
    s->wpn_melee_recoil[slot] = (float)st->recoil;
    s->wpn_melee_recoil_dur[slot] = st->recoil_duration;
    s->wpn_melee_maxrange[slot] = (float)st->max_range;
    s->wpn_melee_ddr[slot] = st->deal_dmg_on_return ? 1 : 0;
    s->wpn_melee_back_dur[slot] = bt_melee_back_duration(atk_spd);

    if (s->wpn_melee_atk[slot] == BT_MELEE_SWEEP) {
        float atk_distance = (float)st->max_range;
        float d = dist;
        if (d < (float)BT_WEAPON_MELEE_MIN_SWEEP_DISTANCE) d = (float)BT_WEAPON_MELEE_MIN_SWEEP_DISTANCE;
        if (d > atk_distance) d = atk_distance;
        atk_distance = d;
        s->wpn_melee_atkdist[slot] = atk_distance;
        s->wpn_melee_atk_dur[slot] = bt_melee_atk_duration(atk_spd, bt_melee_range_factor(atk_distance, atk_spd));
        s->wpn_melee_anim[slot] = BT_MELEE_ANIM_SWEEP;
    } else {
        s->wpn_melee_atkdist[slot] = (float)st->max_range;
        s->wpn_melee_atk_dur[slot] = bt_melee_atk_duration(atk_spd, bt_melee_range_factor((float)st->max_range, atk_spd));
        s->wpn_melee_anim[slot] = BT_MELEE_ANIM_THRUST;
    }

    for (int w = 0; w < BT_WPROJ_OBJECT_WORDS; w++) s->wpn_melee_hit[slot][w] = 0u;
    s->wpn_melee_t[slot] = 0.0f;
    s->wpn_melee_phase[slot] = 1;

    s->wpn_cd[slot] = bt_wfx_next_cooldown(s, st->cooldown, st->additional_cd_every,
                                           (int)s->wpn_melee_shots[slot], count, 0,
                                           st->additional_cd_multiplier);

    /* MeleeWeapon.shoot flips next_attack_type after the swing starts. */
    if (st->alternate_attack_type)
        s->wpn_melee_atk[slot] = (s->wpn_melee_atk[slot] == BT_MELEE_SWEEP)
                                 ? BT_MELEE_THRUST : BT_MELEE_SWEEP;
}

/* ---------------------------------------------------------------------------
 * reset / hooks / per-wave cooldown reset.
 * ------------------------------------------------------------------------- */
BT_HD static void bt_melee_reset(BT_DEVICE BtState* s) {
    for (int i = 0; i < BT_SLOTS; i++) {
        s->wpn_cd[i] = 0.0f;
        s->wpn_melee_t[i] = 0.0f;
        s->wpn_melee_atk_dur[i] = 0.0f;
        s->wpn_melee_back_dur[i] = 0.0f;
        s->wpn_melee_recoil_dur[i] = 0.0f;
        s->wpn_melee_recoil[i] = 0.0f;
        s->wpn_melee_maxrange[i] = 0.0f;
        s->wpn_melee_atkdist[i] = 0.0f;
        s->wpn_melee_rot[i] = 0.0f;
        s->wpn_melee_tdist[i] = 0.0f;
        s->wpn_melee_target[i] = (unsigned short)BT_MELEE_NO_TARGET;
        s->wpn_melee_attack_id[i] = 0u;
        s->wpn_melee_shots[i] = 0;
        s->wpn_melee_atk[i] = BT_MELEE_THRUST;
        s->wpn_melee_anim[i] = BT_MELEE_ANIM_IDLE;
        s->wpn_melee_phase[i] = 0;
        s->wpn_melee_hit_on[i] = 0;
        s->wpn_melee_shooting[i] = 0;
        s->wpn_melee_pending[i] = 0;
        s->wpn_melee_ddr[i] = 0;
        s->wpn_melee_facing[i] = 1;
        for (int w = 0; w < BT_WPROJ_OBJECT_WORDS; w++) s->wpn_melee_hit[i][w] = 0u;
    }
}

/* One copy's creation-time runtime state: Weapon._ready -> init_stats(true) ->
 * get_next_cooldown(true) (ONE rand_range draw) then reset_cooldown().  Shared
 * by the two source paths that create a copy's runtime: a fresh node entering
 * the tree (`add_weapon`, bt_weapon_melee_slot_reset) and a wave boundary,
 * which re-creates every weapon node from the persistent RunData loadout
 * (bt_weapon_melee_wave_begin_slot).
 *
 * `weapon_count` is the `_parent.get_nb_weapons()` the draw resolves against:
 * "the number of weapon nodes ALREADY in current_weapons".  player.gd:376-390
 * calls `_weapons_container.add_child(instance)` FIRST and `current_weapons.
 * push_back(instance)` AFTER, and the node's _ready (hence
 * init_stats -> get_next_cooldown -> get_max_rand_cooldown) runs inside
 * add_child, so the bound EXCLUDES the copy being created -- it is that copy's
 * dense slot index, not the post-insert count.  With the post-insert count,
 * slot 0 drew rand_range(cb - max_rand, cb + max_rand) with
 * get_max_rand_cooldown = min(1*cb/5, 5) instead of the source's exact
 * rand_range(cb, cb). */
BT_HD static void bt_melee_create_runtime(BT_DEVICE BtState* s, int slot,
                                          BT_THREAD const BtPlayerWeaponStats* st,
                                          int weapon_count) {
    s->wpn_melee_atk[slot] = (unsigned char)st->attack_type;
    s->wpn_melee_anim[slot] = BT_MELEE_ANIM_IDLE;
    s->wpn_melee_phase[slot] = 0;
    s->wpn_melee_hit_on[slot] = 0;
    s->wpn_melee_shooting[slot] = 0;
    s->wpn_melee_pending[slot] = 0;
    s->wpn_melee_facing[slot] = 1;
    s->wpn_melee_t[slot] = 0.0f;
    s->wpn_melee_target[slot] = (unsigned short)BT_MELEE_NO_TARGET;
    s->wpn_melee_attack_id[slot] = 0u;
    s->wpn_melee_shots[slot] = 0;
    for (int w = 0; w < BT_WPROJ_OBJECT_WORDS; w++) s->wpn_melee_hit[slot][w] = 0u;
    s->wpn_cd[slot] = bt_wfx_next_cooldown(s, st->cooldown, st->additional_cd_every,
                                           0, weapon_count, 1, st->additional_cd_multiplier);
    /* reset_cooldown (weapon.gd:324-326 + 349-350): the ONE shared clamp in
     * brotato_core.h (bt_weapon_reset_cooldown), which is also what the
     * stat-change dispatch runs; at creation _nb_shots_taken == 0, so its
     * every-x-shots big-reload branch is always active -- identical to the old
     * inline clamp, and ZERO RNG (it never rolls get_next_cooldown again). */
    bt_weapon_reset_cooldown(s, slot);
}

#ifdef BT_WEAPON_MELEE_HOOKS
/* The copy that just landed in `slot`: Weapon._ready -> init_stats(true) ->
 * get_next_cooldown(true) (ONE rand_range draw) then reset_cooldown. */
BT_HD static void bt_weapon_melee_slot_reset(BT_DEVICE BtState* s, int slot) {
    if (slot < 0 || slot >= BT_SLOTS) return;
    unsigned short wid = s->wpn_id[slot];
    if (wid >= BT_NWEAPONS || BT_WEAPONS[wid].kind != BT_WEAPON_KIND_MELEE) return;
    BtPlayerWeaponStats st;
    if (!bt_weapon_stats(s, slot, &st)) return;
    /* The node was inserted at `slot` (bt_weapon_insert sets wpn_count before
     * calling this), so current_weapons.size() at _ready time == slot. */
    bt_melee_create_runtime(s, slot, &st, slot);
}

BT_HD static void bt_weapon_melee_slot_move(BT_DEVICE BtState* s, int from, int to) {
    if (from < 0 || from >= BT_SLOTS || to < 0 || to >= BT_SLOTS) return;
    s->wpn_melee_t[to] = s->wpn_melee_t[from];
    s->wpn_melee_atk_dur[to] = s->wpn_melee_atk_dur[from];
    s->wpn_melee_back_dur[to] = s->wpn_melee_back_dur[from];
    s->wpn_melee_recoil_dur[to] = s->wpn_melee_recoil_dur[from];
    s->wpn_melee_recoil[to] = s->wpn_melee_recoil[from];
    s->wpn_melee_maxrange[to] = s->wpn_melee_maxrange[from];
    s->wpn_melee_atkdist[to] = s->wpn_melee_atkdist[from];
    s->wpn_melee_rot[to] = s->wpn_melee_rot[from];
    s->wpn_melee_tdist[to] = s->wpn_melee_tdist[from];
    s->wpn_melee_target[to] = s->wpn_melee_target[from];
    s->wpn_melee_attack_id[to] = s->wpn_melee_attack_id[from];
    s->wpn_melee_shots[to] = s->wpn_melee_shots[from];
    s->wpn_melee_atk[to] = s->wpn_melee_atk[from];
    s->wpn_melee_anim[to] = s->wpn_melee_anim[from];
    s->wpn_melee_phase[to] = s->wpn_melee_phase[from];
    s->wpn_melee_hit_on[to] = s->wpn_melee_hit_on[from];
    s->wpn_melee_shooting[to] = s->wpn_melee_shooting[from];
    s->wpn_melee_pending[to] = s->wpn_melee_pending[from];
    s->wpn_melee_ddr[to] = s->wpn_melee_ddr[from];
    s->wpn_melee_facing[to] = s->wpn_melee_facing[from];
    for (int w = 0; w < BT_WPROJ_OBJECT_WORDS; w++) s->wpn_melee_hit[to][w] = s->wpn_melee_hit[from][w];
}

BT_HD static void bt_weapon_melee_slot_erase(BT_DEVICE BtState* s, int slot) {
    if (slot < 0 || slot >= BT_SLOTS) return;
    s->wpn_melee_t[slot] = 0.0f;
    s->wpn_melee_atk_dur[slot] = 0.0f;
    s->wpn_melee_back_dur[slot] = 0.0f;
    s->wpn_melee_recoil_dur[slot] = 0.0f;
    s->wpn_melee_recoil[slot] = 0.0f;
    s->wpn_melee_maxrange[slot] = 0.0f;
    s->wpn_melee_atkdist[slot] = 0.0f;
    s->wpn_melee_rot[slot] = 0.0f;
    s->wpn_melee_tdist[slot] = 0.0f;
    s->wpn_melee_target[slot] = (unsigned short)BT_MELEE_NO_TARGET;
    s->wpn_melee_attack_id[slot] = 0u;
    s->wpn_melee_shots[slot] = 0;
    s->wpn_melee_atk[slot] = BT_MELEE_THRUST;
    s->wpn_melee_anim[slot] = BT_MELEE_ANIM_IDLE;
    s->wpn_melee_phase[slot] = 0;
    s->wpn_melee_hit_on[slot] = 0;
    s->wpn_melee_shooting[slot] = 0;
    s->wpn_melee_pending[slot] = 0;
    s->wpn_melee_ddr[slot] = 0;
    s->wpn_melee_facing[slot] = 1;
    for (int w = 0; w < BT_WPROJ_OBJECT_WORDS; w++) s->wpn_melee_hit[slot][w] = 0u;
}
#endif /* BT_WEAPON_MELEE_HOOKS */

/* The MELEE half of the wave-begin node re-creation, one slot in order: a wave
 * boundary loads a FRESH game scene (base_shop.gd:345-346), so every Weapon node
 * is re-created from the persistent RunData loadout (player.gd:315-320
 * apply_items_effects -> player.gd:376-390 add_weapon -> weapon.gd:69-88 _ready
 * -> weapon.gd:137-142 init_stats(at_wave_begin = true) -> get_next_cooldown
 * (true), ONE rand_range draw per weapon in slot order, then reset_cooldown()).
 * Identical to bt_weapon_melee_slot_reset's body, including the draw's
 * weapon_count bound == the copy's slot index (see bt_melee_create_runtime).
 *
 * `player.gd:269 reset_weapons_cd` (a per-live-weapon reset_cooldown clamp) has
 * NO caller in the recovered sources and is NOT this path.
 *
 * The scene swap that creates these nodes lands in the GO frame's MessageQueue
 * flush (see bt_core_step), so the created nodes' first `_physics_process` --
 * the first decrement of the fresh cooldown -- is the NEXT tick; the row of the
 * tick that runs this creation reads the draw untouched, like a run's row 0. */
BT_HD static void bt_weapon_melee_wave_begin_slot(BT_DEVICE BtState* s, int slot) {
    if (slot < 0 || slot >= BT_SLOTS) return;
    unsigned short wid = s->wpn_id[slot];
    if (wid >= BT_NWEAPONS || BT_WEAPONS[wid].kind != BT_WEAPON_KIND_MELEE) return;
    BtPlayerWeaponStats st;
    if (!bt_weapon_stats(s, slot, &st)) return;
    if (st.kind != BT_WEAPON_KIND_MELEE) return;
    bt_melee_create_runtime(s, slot, &st, slot);
}

/* ---------------------------------------------------------------------------
 * One step over every melee slot.
 * ------------------------------------------------------------------------- */
BT_HD static void bt_melee_step(BT_DEVICE BtState* s, int player_moving, float move_dir_x) {
    if ((int)s->wpn_count <= 0) return;
    int can_move_attack = bt_melee_can_move_attack(s);

    for (int slot = 0; slot < (int)s->wpn_count && slot < BT_SLOTS; slot++) {
        /* `count` is the CURRENT dense layout: a break_on_hit removal below
         * compacts the tail down, so every later read (attach index, cooldown
         * weapon count) reflects the post-removal inventory of this frame. */
        int count = (int)s->wpn_count;
        unsigned short wid = s->wpn_id[slot];
        if (wid == BT_WEAPON_NONE || wid >= BT_NWEAPONS) continue;
        if (BT_WEAPONS[wid].kind != BT_WEAPON_KIND_MELEE) continue;

        BtPlayerWeaponStats st;
        if (!bt_weapon_stats(s, slot, &st)) continue;
        if (st.kind != BT_WEAPON_KIND_MELEE) continue;

        BT_CONST_PTR(BtWeaponGeom) G = &BT_WEAPON_GEOM[BT_WEAPONS[wid].geom];
        /* rows is a CONSTANT canonical slice (BT_WEAPON_EFF_INDEX through the
         * weapon's fx_first/fx_count); row_curse is the copy's own BT_DEVICE
         * stored per-top-level-effect modifier array, index-parallel to rows
         * (zeroed for an uncursed copy, so no uncursed branch is needed). */
        int n_effs = (int)BT_WEAPONS[wid].fx_count;
        if (n_effs > BT_MELEE_MAX_FX) n_effs = BT_MELEE_MAX_FX;
        BT_CONST_PTR(unsigned short) fx_rows = &BT_WEAPON_EFF_INDEX[0];
        if (n_effs > 0 && (int)BT_WEAPONS[wid].fx_first < BT_NWEAPON_EFF_INDEX)
            fx_rows = &BT_WEAPON_EFF_INDEX[(int)BT_WEAPONS[wid].fx_first];
        BT_DEVICE const float* fx_curse = s->wpn_fxcurse[slot];

        if (move_dir_x > 0.0f) s->wpn_melee_facing[slot] = 1;
        else if (move_dir_x < 0.0f) s->wpn_melee_facing[slot] = 0;

        /* weapon.gd _physics_process: should_shoot() sets _is_shooting = true and
         * call_deferred("deferred_shoot"); that deferred call is flushed by the
         * SAME physics iteration (SceneTree::iteration's MessageQueue::flush,
         * scene/main/scene_tree.cpp:609), i.e. before the frame's idle step and
         * before the idle-mode Tween step (scene_tree.cpp:666-667).  So shoot()
         * and the tween's FIRST step land in the frame of the should_shoot
         * decision: this tick renders wpn_melee_t = 0 (the idle pose, which is
         * exactly the swing's t = 0 pose, so nothing changes visually or for the
         * hitbox -- it is still disabled) and t = 1 is the NEXT tick, which is
         * the pose the engine's frame-start holds then. */
        if (!s->wpn_melee_shooting[slot]) {
            if (s->wpn_cd[slot] > 0.0f) {
                s->wpn_cd[slot] -= 1.0f;
                if (s->wpn_cd[slot] < 0.0f) s->wpn_cd[slot] = 0.0f;
            }
            float ox, oy;
            bt_melee_origin(s, count, slot, G, &ox, &oy);
            float detect = (float)st.max_range + (float)BT_WEAPON_DETECTION_RANGE;
            unsigned short tgt; float tdist;
            int has = bt_melee_pick_target(s, ox, oy, (float)st.min_range, detect, &tgt, &tdist);
            s->wpn_melee_target[slot] = tgt;
            s->wpn_melee_tdist[slot] = tdist;
            int in_range = has && (tdist <= (float)st.max_range + BT_MELEE_RANGE_PROBE);
            if (s->wpn_cd[slot] <= 0.0f && (!player_moving || can_move_attack) && in_range) {
                s->wpn_melee_shooting[slot] = 1;
                bt_melee_begin_swing(s, slot, count, wid, &st, G);
            }
        }

        if (!s->wpn_melee_shooting[slot]) {
            s->wpn_melee_anim[slot] = BT_MELEE_ANIM_IDLE;
            s->wpn_melee_phase[slot] = 0;
            s->wpn_melee_hit_on[slot] = 0;
            continue;
        }

        /* Render the current swing frame (wpn_melee_t = whole frames since the
         * swing began), apply its hits, then advance one frame.  phase 0 means
         * the swing is over. */
        float px, py, rot; int hit_on, phase;
        int prev_phase = (int)s->wpn_melee_phase[slot];
        bt_melee_anim(s, slot, (int)s->wpn_melee_anim[slot], G, &px, &py, &rot, &hit_on, &phase);
        s->wpn_melee_phase[slot] = (unsigned char)phase;
        s->wpn_melee_hit_on[slot] = (unsigned char)hit_on;

        /* MeleeWeaponShootingBehavior plays the shooting sound (ONE global draw)
         * on the frame the recoil tween's tween_all_completed resumes -- the
         * frame the hitbox is enabled, i.e. the first phase-2 frame of BOTH the
         * thrust and the sweep.  It precedes this frame's hit applications. */
        if (prev_phase == 1 && phase == 2)
            bt_weapon_audio_shoot(s, wid);

        if (hit_on) {
            float ox, oy;
            bt_melee_origin(s, count, slot, G, &ox, &oy);
            float phi = s->wpn_melee_rot[slot];
            float sc = bt_cosf(phi), ss = bt_sinf(phi);
            /* BtWeaponGeom hit_x/hit_y are the Hitbox centre in the weapon ROOT
             * frame; with the sprite animated to (px,py,rot) the collision
             * centre is  root + R(phi) * ( sprite_pos + R(rot) * hit_sprite ),
             * where hit_sprite = (hit_x - sprite_x, hit_y - sprite_y). */
            float hox = G->hit_x - G->sprite_x;
            float hoy = G->hit_y - G->sprite_y;
            float lx = px + (bt_cosf(rot) * hox - bt_sinf(rot) * hoy);
            float ly = py + (bt_sinf(rot) * hox + bt_cosf(rot) * hoy);
            float cx = ox + (sc * lx - ss * ly);
            float cy = oy + (ss * lx + sc * ly);
            float ang = phi + rot;

            int break_slot = 0;
            for (int j = bt_enemy_next(s, 0, 1); j < (int)s->e_used; j = bt_enemy_next(s, j + 1, 1)) {
                /* Neutral trees are hitbox targets too: the swing's Hitbox is an
                 * Area2D on layer 4 and every tree's Hurtbox monitors mask 1032
                 * (neutral.tscn), so a tree inside the swing takes the hit --
                 * Unit.take_damage runs for it (killed_something -> the weapon's
                 * on_killed_something, heal_on_kill, gold_on_crit_kill) and the
                 * NEUTRAL-specific part (hit tally / one_shot_trees /
                 * number_of_hits_before_dying / tree_turrets) is resolved by the
                 * shared primitive through bt_neutral_resolve_hit, exactly like
                 * an explosion hit. */
                if (!s->ealive[j]) continue;
                if (s->e_charmed[j]) continue;   /* skip charmed BEFORE any RNG */
                /* Per-swing hit gate on the source-OBJECT identity (the
                 * weapon.gd ignored_objects equivalent), NOT the reusable live
                 * slot: a fresh node in a recycled actor slot is a new target,
                 * while the same node is never hit twice in one swing.  uid 0 is
                 * the source-identity-overflow sentinel and cannot be gated. */
                unsigned int eid = bt_object_uid(s, j);
                if (eid != 0u && (s->wpn_melee_hit[slot][eid >> 5] & (1u << (eid & 31u))))
                    continue;
                BT_CONST_PTR(BtBodyPhysics) B = bt_kgeom(s, j);
                float hx = s->ex[j] + B->hurt_x, hy = s->ey[j] + B->hurt_y;
                int hit;
                if (G->shape == BT_WEAPON_SHAPE_CIRCLE)
                    hit = bt_circle_overlap(bt_v2(cx, cy), G->hit_circle_r,
                                            bt_v2(hx, hy), B->hurt_radius);
                else
                    hit = bt_melee_rect_hits_circle(cx, cy, ang,
                                                    G->hit_rect_hx, G->hit_rect_hy,
                                                    hx, hy, B->hurt_radius);
                if (!hit) continue;
                if (eid != 0u) s->wpn_melee_hit[slot][eid >> 5] |= (1u << (eid & 31u));
                int bg = bt_melee_hit_enemy(s, slot, wid, &st, j, ox, oy, phi,
                                            fx_rows, fx_curse, n_effs);
                if (bg > 0) {
                    /* break_on_hit gold IS a coin: weapon.gd:462-464 emits
                     * wanted_to_break(value2) -> player.gd:396-400
                     * wanted_to_spawn_gold -> main.gd:593-600 spawn_gold.  (Only
                     * the crit-kill gold above is a direct credit.) */
                    bt_loot_spawn_gold(s, (float)bg, s->ex[j], s->ey[j], 1);
                    break_slot = 1;
                }
            }
            if (break_slot) {
                /* player.gd:395-408 on_weapon_wanted_to_break, in source order:
                 * the gold dropped above (wanted_to_spawn_gold) comes FIRST, then
                 * the weapon is removed/compacted, then exactly ONE breaking-
                 * sounds pick plays on the main SoundManager with always_play
                 * while the copy is still live (the node is only queue_free'd
                 * after).  So the sound belongs here at the real break site,
                 * never inside the generic bt_weapon_remove.
                 * Removal compacts EVERY later copy down one slot: re-enter the
                 * SAME index (now the next copy) so it is neither skipped nor
                 * double-processed, and never keep using the removed copy's
                 * rows/stats (the loop body re-reads wid/st/G/fx). */
                bt_combat_audio_weapon_break(s);
                bt_weapon_remove(s, slot);
                slot--;
                continue;
            }
        }

        s->wpn_melee_t[slot] += 1.0f;
        if (phase == 0) {
            s->wpn_melee_shooting[slot] = 0;
            s->wpn_melee_anim[slot] = BT_MELEE_ANIM_IDLE;
            s->wpn_melee_phase[slot] = 0;
            s->wpn_melee_hit_on[slot] = 0;
            s->wpn_melee_t[slot] = 0.0f;
        }
    }
}

/* ---------------------------------------------------------------------------
 * Visual surface for WeaponBackends: sprite world offset from the player plus
 * the sprite rotation for the current swing frame.
 * ------------------------------------------------------------------------- */
BT_HD static void bt_melee_visual(BT_DEVICE const BtState* s, int slot,
                                  BT_THREAD float* ox, BT_THREAD float* oy,
                                  BT_THREAD float* rot) {
    *ox = 0.0f; *oy = 0.0f; *rot = 0.0f;
    if (slot < 0 || slot >= BT_SLOTS) return;
    unsigned short wid = s->wpn_id[slot];
    if (wid == BT_WEAPON_NONE || wid >= BT_NWEAPONS) return;
    if (BT_WEAPONS[wid].kind != BT_WEAPON_KIND_MELEE) return;
    if (!s->wpn_melee_shooting[slot]) return;
    BT_CONST_PTR(BtWeaponGeom) G = &BT_WEAPON_GEOM[BT_WEAPONS[wid].geom];
    float px, py, r; int hit_on, phase;
    bt_melee_anim(s, slot, (int)s->wpn_melee_anim[slot], G, &px, &py, &r, &hit_on, &phase);
    float phi = s->wpn_melee_rot[slot];
    float sc = bt_cosf(phi), ss = bt_sinf(phi);
    *ox = sc * px - ss * py;
    *oy = ss * px + sc * py;
    *rot = phi + r;
}