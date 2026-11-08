/* brotato_weapon_ranged.h -- RANGED player-weapon firing for the shared
 * Brotato Bull/Danger-5 core.
 *
 * Owner: WeaponRanged.  A faithful transcription of the ranged half of
 * weapons/weapon.gd + weapons/shooting_behaviors/ranged_weapon_shooting_behavior.gd
 * + weapons/ranged/ranged_weapon.gd:
 *
 *   weapon.gd:239-241,413-414 attach/muzzle transform + DETECTION_RANGE probe
 *   weapon.gd:244-266 _physics_process cooldown tick, rotation, deferred shoot
 *   weapon.gd:268-291 get_direction_and_calculate_target / get_direction /
 *                  should_shoot (static-fire gate, target band)
 *   weapon.gd:293-316 shoot(): every-x-shots stats swap, reload_turrets,
 *                  update_current_spread, update_knockback, behaviour.shoot
 *   weapon.gd:318-347 reset_cooldown / get_next_cooldown / get_max_rand_cooldown
 *                  / is_big_reload_active / update_current_spread / update_knockback
 *   weapon.gd:349-358 set_shooting
 *   weapon.gd:367-370 get_max_range (= max_range + 50)
 *   weapon.gd:372-380 sprite flipv / idle angle
 *   ranged_weapon_shooting_behavior.gd:9-44  shoot(): sound pick, attack id,
 *                  the per-projectile spread loop, the two recoil tweens
 *   singletons/utils.gd:266-280  get_nearest_no_max (strict min_range, strict <)
 *   singletons/utils.gd:391-403  get_rand_element / get_chance_success
 *   entities/units/player/weapons_container.gd + player.tscn attach ring
 *   entities/units/player/weapon.tscn Range Area2D mask 5 = neutral + enemies
 *
 * RNG (the GLOBAL stream s->rng -- the stream GDScript rand_range/randi use).
 * The exact draw order of ONE shot is preserved and nothing else draws:
 *     (1) update_current_spread   rand_range(-1+accuracy, 1-accuracy)
 *     (2) shooting sound          Utils.get_rand_element(shooting_sounds)
 *                                 = ONE bounded draw, skipped entirely when the
 *                                   array is empty (utils.gd:391-393)
 *     (3) nb_projectiles          rand_range(rot - spread, rot + spread)
 *     (4) get_next_cooldown       rand_range(lo, hi), or NO draw at all on the
 *                                 big-reload shot (weapon.gd:319-322)
 * A weapon ADD draws exactly ONE rand_range (init_stats(at_wave_begin=true) ->
 * get_next_cooldown(true), weapon.gd:137-142).  reset_cooldown draws nothing.
 * The sound draw is a REAL one on the source stream: bt_weapon_audio_shoot()
 * takes it (ONE bounded draw) and submits the selected clip to the main
 * SoundManager queue (see brotato_weapon_audio.h).  It must never be omitted,
 * doubled, or padded, and the playback pitch draw stays at playback.
 *
 * COOLDOWN MODEL.  Weapon._current_cooldown counts down 1 tick/frame ONLY while
 * `_is_shooting` is false (weapon.gd:255-256), and `_is_shooting` stays true for
 * the whole two-tween recoil animation (2 * recoil_duration seconds,
 * RangedShootingData.get_shooting_total_duration).  A `recoil_duration` of d
 * seconds therefore freezes the cooldown for `2 * ceil(d * 60) - 1` frames: the
 * ceil of each of the two sequential tweens minus the frame on which Tween
 * emits tween_all_completed (that frame's _physics_process already ran with
 * _is_shooting still set).  bt_ranged_recoil_freeze_frames().
 *
 * TARGETING.  The weapon's Range Area2D (weapon.tscn:39-45) has mask 5 =
 * layer_1 "neutral" + layer_3 "enemies", so live enemies AND live neutral trees
 * are candidates; the nearest with dist^2 > min_range^2 wins (ties -> the lowest
 * slot, Utils.get_nearest_no_max keeps the first strict minimum).  should_shoot
 * additionally requires dist <= max_range + 50.
 *
 * FIRE GATE.  should_shoot requires `_parent._current_movement == Vector2.ZERO`
 * UNLESS the can_attack_while_moving effect is positive.  player_run_data.gd:485
 * seeds that effect at 1 for every player, so a weapon normally fires while the
 * player moves; the effect must be zeroed (the Soldier's character effect) for
 * the static-fire gate to bite.
 *
 * CONSUMES (owned elsewhere; included earlier in the same TU by brotato_core.h):
 *   brotato_weapons.h / brotato_weapon_types.h (WeaponData)
 *       BtWeaponDef BT_WEAPONS[BT_NWEAPONS] (kind/geom/fx_first/fx_count/stats),
 *       BT_WEAPON_NONE, BT_WEAPON_KIND_*, BT_WEAPON_DETECTION_RANGE,
 *       BtWeaponGeom BT_WEAPON_GEOM[] (muzzle_x/muzzle_y/attach_x/attach_y),
 *       BT_WEAPON_EFF_INDEX (weapon fx spans, indexing BT_ITEM_EFF_ROWS),
 *       BT_WEAPON_PROJ_SPEC / bt_weapon_proj_spec(), BtWeaponOnHit,
 *       BT_ISTR_CAN_ATTACK_WHILE_MOVING.
 *   brotato_weapon_stats.h (WeaponStats)
 *       BtPlayerWeaponStats (including the resolved is_elemental),
 *       bt_weapon_stats(), bt_weapon_stats_variants(), bt_weapon_stats_onhit(),
 *       bt_ws_shooting_frames().
 *   brotato_weapon_inventory.h (WeaponInventory)
 *       wpn_id/wpn_cursed/wpn_curse/wpn_cd/wpn_count and bt_weapon_uid(), plus
 *       the slot reset/move/erase hooks this header defines
 *       (BT_WEAPON_RANGED_HOOKS).
 *   brotato_weapon_effects.h (WeaponEffects)
 *       bt_wfx_next_cooldown(), bt_wfx_accuracy_spread(), bt_wfx_reload_turrets().
 *   brotato_weapon_projectiles.h (WeaponProjectiles)
 *       BtWProjArgs + bt_wproj_spawn().
 *   brotato_weapon_audio.h (WeaponAudioRuntime)
 *       bt_weapon_audio_shoot() -- the one global shooting-sound draw + main
 *       SoundManager submission.
 *   brotato_item_effects.h  BT_NITEM_EFF_ROWS (bound of the weapon fx spans
 *       that index BT_ITEM_EFF_ROWS), BT_ISTR_PIERCE_ON_CRIT,
 *       BT_ISTR_BOUNCE_ON_CRIT, BT_NITEM_STRS.
 *
 * BtState FIELDS THIS MODULE OWNS (parent adds them to brotato_state.h; dense,
 * parallel to wpn_id[], moved/erased with the copy by the hooks below):
 *   unsigned char wrng_shoot[BT_SLOTS];      Weapon._is_shooting
 *   unsigned char wrng_phase[BT_SLOTS];      recoil tween stage (0 out, 1 back)
 *   float         wrng_anim[BT_SLOTS];       ticks left of the recoil animation
 *   float         wrng_spread[BT_SLOTS];     Weapon._current_shoot_spread
 *   float         wrng_rot[BT_SLOTS];        Weapon.rotation (radians)
 *   int           wrng_shots[BT_SLOTS];      Weapon._nb_shots_taken
 *   int           wrng_attack_id[BT_SLOTS];  ShootingBehavior.__next_attack_id
 * It writes s->wpn_cd[slot] in TICKS (the shared per-slot cooldown clock) and
 * nothing else in the shared slot record.
 *
 * INTEGRATION (parent, brotato_core.h):
 *   #define BT_WEAPON_RANGED_HOOKS   before including brotato_weapon_inventory.h
 *   include this header AFTER brotato_weapon_projectiles.h;
 *   bt_core_reset:  bt_ranged_reset(s);
 *   bt_enter_wave:  for w > 1, bt_ranged_wave_begin(s) -- a fresh game scene per
 *                   wave recreates every Weapon node (see that function); for
 *                   w == 1 the copies are created by bt_character_starting_apply
 *                   -> bt_weapon_insert, which already draws in slot order.
 *   bt_core_step:   just AFTER the player-movement block (s->px/s->py final for
 *                   this frame) and BEFORE the projectile step:
 *       bt_ranged_step(s, BT_MOVE_X[action] != 0.0f || BT_MOVE_Y[action] != 0.0f,
 *                      player_facing);
 *                   player_facing is the Unit sprite scale.x sign (+1/-1,
 *                   unit.gd:242); it only picks the idle angle.
 *   Inventory calls bt_weapon_ranged_slot_reset/move/erase (declared in
 *   brotato_weapon_inventory.h under BT_WEAPON_RANGED_HOOKS).
 *   bt_weapon_ranged_stats_changed must be called when a stat that feeds a weapon
 *   actually changed (stats_manager.gd:36-45 init_stats(false) -> reset_cooldown,
 *   NO draw).
 *   DELIBERATELY NOT CALLED: player.gd:269 reset_weapons_cd -- it has no caller
 *   anywhere in the recovered sources (verified by grep), so a per-wave cooldown
 *   clamp would be an invention.  Weapons keep their cooldown across waves.
 *
 * POD + BT_HD only, no allocation, no host API -> CPU/CUDA/MSL portable.
 */
#ifndef BROTATO_WEAPON_RANGED_H
#define BROTATO_WEAPON_RANGED_H

#include "brotato_weapon_types.h"
#include "brotato_weapons.h"
#include "brotato_weapon_audio.h"
#include "brotato_weapon_stats.h"
#include "brotato_weapon_effects.h"
#include "brotato_weapon_projectiles.h"

/* weapon.gd:20 Weapon.DETECTION_RANGE (BT_WEAPON_DETECTION_RANGE, weapon_types.h)
 * -- the Range Area2D radius is current_stats.max_range + DETECTION_RANGE
 * (weapon.gd:155). */
#define BT_RANGED_DETECTION_RANGE ((float)BT_WEAPON_DETECTION_RANGE)
/* weapon.gd:367-370 get_max_range() = max_range + 50, the should_shoot band. */
#define BT_RANGED_RANGE_PROBE 50.0f
#define BT_RANGED_NO_TARGET 0xFFFFu
#define BT_RANGED_MAX_FX 8

#ifndef BT_PI
#define BT_PI 3.14159265f
#endif

#if defined(__METAL_VERSION__)
#define bt_ranged_exp2(x) metal::precise::exp2(x)
#else
#define bt_ranged_exp2(x) exp2f(x)
#endif

/* ---------------------------------------------------------------------------
 * Attach ring.  player.tscn Weapons/<count>/Attach<i>.position (the Weapons
 * child of the player sits at (0,-24)); weapons_container.gd
 * update_weapons_positions rebuilds it on every add/remove, so the offset is a
 * function of the LIVE weapon count.
 * ------------------------------------------------------------------------- */
#define BT_RANGED_MAX_ATTACH 6
typedef struct { float x, y; } BtRangedAttach;
BT_TABLE BtRangedAttach BT_RANGED_ATTACH[7][BT_RANGED_MAX_ATTACH] = {
    { {0,0},{0,0},{0,0},{0,0},{0,0},{0,0} },
    { {0,30},{0,0},{0,0},{0,0},{0,0},{0,0} },             /* 1 weapon  */
    { {40,20},{-40,20},{0,0},{0,0},{0,0},{0,0} },         /* 2 weapons */
    { {40,20},{-40,20},{0,-45},{0,0},{0,0},{0,0} },       /* 3 weapons */
    { {40,35},{-40,35},{-40,-30},{40,-30},{0,0},{0,0} },  /* 4 weapons */
    { {35,35},{-35,35},{-55,-20},{55,-20},{0,-55},{0,0} },/* 5 weapons */
    { {35,45},{-35,45},{-60,0},{60,0},{-35,-45},{35,-45} }/* 6 weapons */
};
#define BT_RANGED_WEAPONS_OX 0.0f
#define BT_RANGED_WEAPONS_OY (-24.0f)

BT_HD static BT_CONST_PTR(BtRangedAttach) bt_ranged_attach_of(int count, int slot) {
    int c = count < 1 ? 1 : (count > 6 ? 6 : count);
    int s = slot < 0 ? 0 : (slot > 5 ? 5 : slot);
    return &BT_RANGED_ATTACH[c][s];
}

/* weapon.gd:239-241 attach(): weapon.position = attach_to - _attach.position, so
 * the weapon node's global_position is player + Weapons(0,-24) + ring - the
 * scene's own Attach offset. */
BT_HD static void bt_ranged_origin(BT_DEVICE const BtState* s, int count, int slot,
                                   BT_CONST_PTR(BtWeaponGeom) G,
                                   BT_THREAD float* ox, BT_THREAD float* oy) {
    BT_CONST_PTR(BtRangedAttach) a = bt_ranged_attach_of(count, slot);
    *ox = s->px + BT_RANGED_WEAPONS_OX + a->x - G->attach_x;
    *oy = s->py + BT_RANGED_WEAPONS_OY + a->y - G->attach_y;
}

/* Muzzle.global_position = origin + R(rotation) * (muzzle_x, muzzle_y), with
 * BT_WEAPON_GEOM storing the muzzle already in the weapon-root frame
 * (Sprite.position + Muzzle.position).  The recoil tween never moves the muzzle
 * at shot time: shoot() samples it while Sprite.position is still the tween's
 * start value (the previous animation returned it to base). */
BT_HD static void bt_ranged_muzzle(BT_CONST_PTR(BtWeaponGeom) G, float ox, float oy, float rot,
                                   BT_THREAD float* mx, BT_THREAD float* my) {
    float c = bt_cosf(rot), sn = bt_sinf(rot);
    *mx = ox + (c * G->muzzle_x - sn * G->muzzle_y);
    *my = oy + (sn * G->muzzle_x + c * G->muzzle_y);
}

/* ---------------------------------------------------------------------------
 * RangedWeaponShootingBehavior.shoot()'s two chained recoil tweens and the
 * cooldown freeze they impose.
 *
 * engine/scene/animation/tween.cpp:752-766 -- a Tween completes an interpolation
 * on the first internal process (IDLE) step where `elapsed > delay + duration`,
 * with `elapsed` the engine's real_t accumulator of the fixed idle delta.  The
 * pinned Linux build is a real_t == float (binary32) build, so the accumulation
 * is a SEQUENTIAL float32 sum of delta = (float)(1.0/60.0): `k` steps are needed
 * with k = min{ k : sum_k > recoil_duration } -- e.g. recoil_duration 0.05 needs
 * only 3 steps (3 * 0.016666668 == 0.050000004 > 0.05), while a binary64 sum
 * would need 4.
 *
 * The behavior chains TWO such tweens (ranged_weapon_shooting_behavior.gd:25-42)
 * and only clears `_is_shooting` after the second one completes
 * (weapon.gd:255-256 stops the countdown while `_is_shooting`).  Tween::start()
 * re-registers the node in the "idle_process_internal" group from inside the
 * first tween's OWN notification (tween.cpp:1300-1310 is a no-op re-entry:
 * `pending_update == 0` at that point), so the second tween's first step lands
 * on the frame AFTER the first tween's last step.  Measured on the pinned engine
 * (tests/brotato_weapon_oracle_project/weapon_recoil_oracle.gd): the freeze is
 * `2*k + 1` additional frames after the shot frame, and the cooldown resumes on
 * frame `2*k + 2`.
 * ------------------------------------------------------------------------- */
BT_HD static float bt_ranged_recoil_freeze_frames(float recoil_duration) {
    if (!(recoil_duration > 0.0f)) return 0.0f;
    float dt = (float)(1.0 / 60.0);
    float elapsed = 0.0f;
    int k = 0;
    while (elapsed <= recoil_duration && k < 4096) { elapsed += dt; k++; }
    return (float)(2 * k + 1);
}

/* bt_ws_shooting_frames() (brotato_weapon_stats.h) is the source's
 * get_shooting_total_duration in 60 Hz frames and stays the min/max-cooldown
 * input (weapon_service.gd:558-600); it is NOT the frame count the cooldown
 * clock observes -- that is bt_ranged_recoil_freeze_frames() above. */

/* Godot 3 Tween TRANS_EXPO / EASE_OUT (scene/animation/tween.cpp
 * _run_equation): b + c * (1 - 2^(-10 t/d)). */
BT_HD static float bt_ranged_ease_expo_out(float b, float c, float t, float d) {
    if (d <= 0.0f) return b + c;
    if (t <= 0.0f) return b;
    if (t >= d) return b + c;
    return b + c * (1.0f - bt_ranged_exp2(-10.0f * t / d));
}

/* ---------------------------------------------------------------------------
 * can_attack_while_moving (weapon.gd:283-291).  run_data.gd / player_run_data.gd
 * seeds this effect at 1 for EVERY player (player_run_data.gd:485), so a weapon
 * DOES fire while the player moves by default; an effect can zero it (the
 * Soldier's character effect), which is the only way the static-fire gate bites.
 * The core registers owned integer effects into scalar_effect[]; the reader is
 * `> 0`, exactly like WeaponMelee. */
BT_HD static int bt_ranged_can_move_attack(BT_DEVICE BtState* s) {
#if defined(BT_ISTR_CAN_ATTACK_WHILE_MOVING)
    if ((int)BT_ISTR_CAN_ATTACK_WHILE_MOVING < BT_NITEM_STRS)
        return s->scalar_effect[BT_ISTR_CAN_ATTACK_WHILE_MOVING] > 0.0f ? 1 : 0;
#else
    (void)s;
#endif
    return 0;
}

/* RunData.get_player_effect(key) for an integer effect: the core sums owned
 * effect rows of that key into scalar_effect[]. */
BT_HD static float bt_ranged_player_effect(BT_DEVICE BtState* s, unsigned short key) {
    return ((int)key < BT_NITEM_STRS) ? s->scalar_effect[key] : 0.0f;
}

/* ---------------------------------------------------------------------------
 * Utils.get_nearest_no_max over the weapon's Range Area (utils.gd:266-280):
 * every live unit whose BODY overlaps the Range circle is a candidate (live
 * enemies AND live neutral trees -- weapon.tscn:39-45 keeps the Range Area2D at
 * the weapon origin with mask 5 = layer_1 "neutral" + layer_3 "enemies", and
 * weapon.gd:155 sets its CollisionShape2D radius to max_range + DETECTION_RANGE).
 * The Area2D is a circle-vs-BODY-shape overlap, so the membership test is
 * circle-circle overlap against the unit's own body CollisionShape2D (radius
 * body_radius at body_x/body_y, root-local) -- NOT a centre-distance test.
 * The chosen target is then the nearest by CENTRE distance (get_nearest_no_max
 * measures target.global_position), with `dist^2 STRICTLY greater than
 * min_range^2`, ties keeping the lowest slot.
 * ------------------------------------------------------------------------- */
BT_HD static int bt_ranged_pick_target(BT_DEVICE BtState* s, float ox, float oy,
                                       float min_range, float detect_range,
                                       BT_THREAD unsigned short* out_slot,
                                       BT_THREAD float* out_dist) {
    int best = -1;
    float best_d2 = 0.0f;
    float min2 = min_range * min_range;
    for (int j = bt_enemy_next(s, 0, 1); j < (int)s->e_used; j = bt_enemy_next(s, j + 1, 1)) {
        if (!s->ealive[j]) continue;
        /* weapon.gd:248 `Utils.get_nearest_no_max(_targets_in_range,
         * global_position, min_range)` measures `target.global_position`, i.e.
         * the target's NODE transform.  A MODE_CHARACTER enemy's Node transform
         * is one physics step behind the integrated body (brotato_state.h:749-765),
         * so the distance (and therefore the max_range + 50 band gate) reads
         * s->ehx/s->ehy, exactly like the aim angle above -- NOT this frame's
         * s->ex/s->ey, which the server has not pushed back yet. */
        float dx = s->ehx[j] - ox, dy = s->ehy[j] - oy;
        float d2 = dx * dx + dy * dy;
        /* Range-circle vs body-circle overlap (exact for two circles). */
        BT_CONST_PTR(BtBodyPhysics) G = bt_kgeom(s, j);
        float bx = dx + G->body_x, by = dy + G->body_y;
        float reach = detect_range + G->body_radius;
        if (bx * bx + by * by > reach * reach) continue;
        if (d2 <= min2) continue;
        if (best < 0 || d2 < best_d2) { best = j; best_d2 = d2; }
    }
    if (best < 0) { *out_slot = (unsigned short)BT_RANGED_NO_TARGET; *out_dist = 0.0f; return 0; }
    *out_slot = (unsigned short)best;
    *out_dist = bt_sqrtf(best_d2);
    return 1;
}

/* ---------------------------------------------------------------------------
 * Projectile-scene metadata (WeaponData's BT_WEAPON_PROJ_SPEC).  Every
 * projectiles/<scene>.tscn value the caller needs is resolved there: the Hitbox
 * circle plus the script exports rotation_speed (shuriken 25) and stop_delay
 * (fireball 20).  A weapon whose stats row names no scene uses
 * WeaponService.DEFAULT_PROJECTILE_SCENE
 * (res://projectiles/bullet/bullet_projectile.tscn), carried as
 * BT_WEAPON_PROJ_SPEC[BT_WEAPON_PROJ_DEFAULT] with scene == BT_ISTR_NONE.
 *
 * DELAYED PROJECTILES: the four DelayedPlayerProjectile scenes (bullet_laser/
 * delayed_bullet_laser, bullet_lightning/delayed_lightning_projectile,
 * spike/bullet_spike, bullet_taser/delayed_taser_projectile) initialise
 * `var delay = rand_range(0.0, 3.0)` as a NODE MEMBER initialiser
 * (delayed_player_projectile.gd:4), i.e. the global-RNG draw happens once per
 * node construction / pool pull and is retained across same-scene reuse.  This
 * module therefore sets a.scene_istr to the canonical PackedScene istr and
 * raises BT_WPROJ_F_DELAYED for those scenes, and draws NOTHING: the delay value
 * and its draw live in WeaponProjectiles' node pool, which owns BT_WPROJ_F_DELAYED
 * and the armed flag.  BtWeaponProjSpec.delay carries only a scene's AUTHORED
 * override and is passed through as a.delay.
 * ------------------------------------------------------------------------- */
BT_HD static void bt_ranged_proj_geom(unsigned short scene,
                                      BT_THREAD float* rad, BT_THREAD float* offx,
                                      BT_THREAD float* offy) {
    BT_CONST_PTR(BtWeaponProjSpec) sp = bt_weapon_proj_spec(scene);
    *rad = sp->hit_radius; *offx = sp->hit_x; *offy = sp->hit_y;
}

/* Scenes whose root script is DelayedPlayerProjectile (delayed_player_projectile.gd
 * extends PlayerProjectile).  Its `var delay = rand_range(0.0, 3.0)` is a NODE
 * member initialiser, so the shot only has to (a) name the scene identity and
 * (b) raise BT_WPROJ_F_DELAYED; WeaponProjectiles draws the delay once per node
 * construction/pool pull and retains it across same-scene reuse.  The canonical
 * scene list lives with the node pool (bt_wproj_scene_is_delayed,
 * brotato_weapon_projectiles.h) so the projectile spawn and this ranged flag can
 * NEVER disagree; if WeaponData ever puts a `delayed` bit on BtWeaponProjSpec
 * both collapse into one field read. */
BT_HD static int bt_ranged_scene_is_delayed(unsigned short scene) {
    return bt_wproj_scene_is_delayed(scene);
}

/* A weapon's own effect rows come from the shared bt_wfx_fx_rows()
 * (brotato_weapon_effects.h): the same canonical BT_WEAPON_EFF_INDEX slice
 * bt_wfx_reload_turrets()/bt_eff_is() consume. */

/* reset_cooldown (weapon.gd:311-313): min(cd, cooldown * (big_reload ? mult : 1)).
 * is_big_reload_active() = additional_cooldown_every_x_shots != -1 and
 * _nb_shots_taken % every == 0 (weapon.gd:338-340).  every <= 0 cannot occur in
 * the shipped tables (it is -1 or >= 2), so the `%` is guarded. */
BT_HD static void bt_ranged_reset_cooldown(BT_DEVICE BtState* s, int slot,
                                           BT_THREAD const BtPlayerWeaponStats* st) {
    int every = st->additional_cd_every;
    float mult = 1.0f;
    if (every != -1 && st->additional_cd_multiplier > 0.0f &&
        (every <= 0 || (s->wrng_shots[slot] % every) == 0))
        mult = st->additional_cd_multiplier;
    float cap = (float)st->cooldown * mult;
    if (s->wpn_cd[slot] > cap) s->wpn_cd[slot] = cap;
}

/* ---------------------------------------------------------------------------
 * One shot: weapon.gd:293-316 shoot() + ranged_weapon_shooting_behavior.gd:9-44.
 * RNG order is (1) accuracy, (2) sound, (3) per projectile, (4) cooldown.
 * ------------------------------------------------------------------------- */
BT_HD static void bt_ranged_shoot(BT_DEVICE BtState* s, int slot, int count,
                                  unsigned short wid, BT_THREAD const BtPlayerWeaponStats* st,
                                  float rot) {
    /* _nb_shots_taken += 1 happens BEFORE the every-x-shots swap (weapon.gd:294). */
    int shots = s->wrng_shots[slot] + 1;
    s->wrng_shots[slot] = shots;

    /* _stats_every_x_shots (weapon_service.gd:650-680).  Empty for every shipped
     * base + Abyss ranged weapon; kept so a DLC item's modify_every_x_projectile
     * row (sub_effects) still swaps in its variant, LAST matching entry wins
     * exactly like the source's insertion-ordered dict loop. */
    BtPlayerWeaponStats var[BT_WSTATS_MAX_BONUS];
    int vx[BT_WSTATS_MAX_BONUS];
    int nv = bt_weapon_stats_variants(s, slot, vx, var, BT_WSTATS_MAX_BONUS);
    BT_THREAD const BtPlayerWeaponStats* cur = st;
    for (int v = 0; v < nv; v++) {
        if (vx[v] > 0 && (shots % vx[v]) == 0) cur = &var[v];
    }

    /* reload_turrets_on_shoot (weapon.gd:300-302 -> entity_spawner.gd:570).  Only
     * the DLC melee war_hammer carries the row today; the ranged path keeps the
     * exact emit point so a ranged weapon that ever gains it still fires it. */
    {
        unsigned short fx_rows[BT_RANGED_MAX_FX];
        int n_effs = bt_wfx_fx_rows(wid, fx_rows, BT_RANGED_MAX_FX);
        if (n_effs > 0) (void)bt_wfx_reload_turrets(s, fx_rows, n_effs);
    }

    /* (1) update_current_spread (weapon.gd:344-346). */
    float spread_add = bt_wfx_accuracy_spread(s, cur->accuracy);
    s->wrng_spread[slot] = spread_add;
    rot += spread_add;

    /* (2) SoundManager.play(Utils.get_rand_element(current_stats.shooting_sounds),
     * current_stats.sound_db_mod, 0.2) -- ranged_weapon_shooting_behavior.gd:10.
     * ONE bounded global draw over BT_WEAPONS[wid].shoot_audio_count, none at all
     * when the array is empty (utils.gd:391-393); the selected clip is queued to
     * the main SoundManager pool and its pitch is drawn at playback. */
    bt_weapon_audio_shoot(s, wid);

    /* _parent.set_shooting(true) + the attack id shared by every projectile. */
    s->wrng_shoot[slot] = 1;
    s->wrng_phase[slot] = 0;
    s->wrng_anim[slot] = bt_ranged_recoil_freeze_frames(cur->recoil_duration);
    s->wrng_rot[slot] = rot;   /* update_current_spread mutates rotation */
    int attack_id = s->wrng_attack_id[slot];
    s->wrng_attack_id[slot] = attack_id + 1;

    /* (3) the per-projectile loop. */
    float ox, oy, mx, my;
    BT_CONST_PTR(BtWeaponGeom) G = &BT_WEAPON_GEOM[BT_WEAPONS[wid].geom];
    bt_ranged_origin(s, count, slot, G, &ox, &oy);
    bt_ranged_muzzle(G, ox, oy, rot, &mx, &my);

    float p_rad = 0.0f, p_offx = 0.0f, p_offy = 0.0f;
    bt_ranged_proj_geom(cur->projectile_scene, &p_rad, &p_offx, &p_offy);
    BT_CONST_PTR(BtWeaponProjSpec) spec = bt_weapon_proj_spec(cur->projectile_scene);

    BtWeaponOnHit onhit;
    int has_onhit = bt_weapon_stats_onhit(s, slot, &onhit);

    /* elemental damage flag: resolved by WeaponStats from the EFFECTIVE
     * scaling rows (weapon_service.gd:109-112 damage_type assignment).  The
     * player base scaling span indexes BT_WEAPON_SCALING, not BT_ITEM_SCALING,
     * so bt_wfx_is_elemental() must NOT be fed these indices. */
    int is_elemental = cur->is_elemental ? 1 : 0;
    float src_curse = s->wpn_cursed[slot] ? s->wpn_curse[slot] : 0.0f;
    unsigned int owner_uid = bt_weapon_uid(s, slot);
    int crit_pierce = (int)bt_ranged_player_effect(s, BT_ISTR_PIERCE_ON_CRIT);
    int crit_bounce = (int)bt_ranged_player_effect(s, BT_ISTR_BOUNCE_ON_CRIT);

    int np = cur->nb_projectiles;
    if (np < 0) np = 0;
    for (int i = 0; i < np; i++) {
        float pr = bt_rand_range(s, rot - cur->projectile_spread,
                                     rot + cur->projectile_spread);
        BtWProjArgs a;
        a.x = mx; a.y = my; a.rot = pr;
        a.speed = (float)cur->projectile_speed;
        a.dmg = (float)cur->damage;
        a.crit_chance = cur->crit_chance;
        a.crit_dmg = cur->crit_damage;
        a.burn_chance = cur->burn_chance;
        a.burn_dmg = cur->burn_dmg;
        a.burn_dur = (float)cur->burn_dur;
        a.max_range = (float)cur->max_range;
        a.knockback = (float)cur->knockback;
        a.knockback_piercing = cur->knockback_piercing;
        a.speed_pct = (float)cur->speed_percent_modifier;
        a.piercing = cur->piercing;
        a.pierce_reduce = cur->piercing_dmg_reduction;
        a.bounce = cur->bounce;
        a.bounce_reduce = cur->bounce_dmg_reduction;
        a.rad = p_rad; a.offx = p_offx; a.offy = p_offy;
        a.shape = BT_SHAPE_CIRCLE;
        /* pierce_on_crit / bounce_on_crit player charges (weapon_service.gd:366-435
         * + player_projectile.gd:132-154): the resolved player-effect total rides
         * each shot and is spent one per crit. */
        a.crit_pierce = crit_pierce;
        a.crit_bounce = crit_bounce;
        a.attack_id = attack_id;
        a.rot_speed = spec->rot_speed;
        a.stop_delay = spec->stop_delay;
        a.delay = spec->delay;
        a.slow_rad = spec->slow_rad;
        a.slow_amt = spec->slow_amt;
        a.lifesteal = cur->lifesteal;
        a.src_weapon_id = wid;
        a.owner_uid = owner_uid;
        a.src_curse = src_curse;
        /* Per-top-level-effect curse draws (Inventory wpn_fxcurse[slot][k], one
         * draw per BT_WEAPONS[wid] top-level fx row, read through the canonical
         * accessor): the shot must carry them verbatim so an effect still
         * resolves correctly after the copy is recycled/combined.  The aggregate
         * src_curse is NOT a substitute.  The on-hit CHILD carries no effects of
         * its own (unit.gd:586-604 spawns it with _default_spawn_projectile_args
         * and an empty effect list), so it needs no fx-curse slice. */
        for (int k = 0; k < BT_WEAPON_FX_CURSE_MAX; k++)
            a.src_fxcurse[k] = bt_weapon_fx_curse(s, slot, k);
        a.fx_first = BT_WEAPONS[wid].fx_first;
        a.fx_count = (unsigned char)(BT_WEAPONS[wid].fx_count > 255
                                         ? 255 : BT_WEAPONS[wid].fx_count);
        a.is_elemental = (unsigned char)(is_elemental ? 1 : 0);
        /* Immutable hitbox scaling-key snapshot (hitbox.gd HitboxArgs._set_from
         * copies source.scaling_stats onto the projectile's hitbox): the shot
         * carries the EFFECTIVE scaling keys of the stats row it actually fired
         * with (`cur`, so an every-x-shots variant is honoured), so an on-hit
         * enemy debuff (enemy_percent_damage_taken) can gate by the hitbox's own
         * scaling even after the owning copy is removed.  Valid only while
         * BT_WPROJ_F_SKEY is set; n_scaling <= BT_WSTATS_MAX_SCALING ==
         * BT_WPROJ_MAX_SKEY, so nothing truncates. */
        a.s_nscaling = (unsigned char)cur->n_scaling;
        for (int q = 0; q < BT_WPROJ_MAX_SKEY; q++) a.s_key[q] = 0;
        for (int q = 0; q < (int)a.s_nscaling && q < BT_WPROJ_MAX_SKEY; q++)
            a.s_key[q] = cur->sc_key[q];
        /* node-pool identity: the canonical projectile PackedScene istr, plus the
         * DelayedPlayerProjectile flag for the delayed scenes (the delay VALUE is
         * drawn and retained by WeaponProjectiles' node pool, not here). */
        a.scene_istr = (unsigned short)cur->projectile_scene;
        a.flags = (unsigned char)(BT_WPROJ_F_LEAVE | BT_WPROJ_F_SKEY);
        if (bt_ranged_scene_is_delayed((unsigned short)cur->projectile_scene))
            a.flags |= BT_WPROJ_F_DELAYED;
        a.onhit_n = has_onhit ? (unsigned char)onhit.count : 0;
        a.onhit_auto = has_onhit ? (unsigned char)onhit.auto_target : 0;
        a.child.dmg = has_onhit ? onhit.dmg : 0.0f;
        a.child.crit_chance = has_onhit ? onhit.crit_chance : 0.0f;
        a.child.crit_dmg = has_onhit ? onhit.crit_dmg : 0.0f;
        a.child.burn_chance = has_onhit ? onhit.burn_chance : 0.0f;
        a.child.burn_dmg = has_onhit ? onhit.burn_dmg : 0.0f;
        a.child.burn_dur = has_onhit ? onhit.burn_dur : 0.0f;
        a.child.max_range = has_onhit ? onhit.max_range : 0.0f;
        a.child.speed = has_onhit ? onhit.speed : 0.0f;
        a.child.piercing = has_onhit ? onhit.piercing : 0;
        a.child.pierce_reduce = has_onhit ? onhit.pierce_reduce : 0.0f;
        a.child.bounce = has_onhit ? onhit.bounce : 0;
        a.child.bounce_reduce = has_onhit ? onhit.bounce_reduce : 0.0f;
        a.child.shape = BT_SHAPE_CIRCLE;
        if (has_onhit) {
            float crad = 0.0f, coffx = 0.0f, coffy = 0.0f;
            bt_ranged_proj_geom(onhit.projectile_scene, &crad, &coffx, &coffy);
            a.child.rad = crad; a.child.offx = coffx; a.child.offy = coffy;
        } else {
            a.child.rad = 0.0f; a.child.offx = 0.0f; a.child.offy = 0.0f;
        }
        (void)bt_wproj_spawn(s, a);
    }

    /* (4) _current_cooldown = get_next_cooldown() -- weapon.gd:309. */
    s->wpn_cd[slot] = bt_wfx_next_cooldown(s, cur->cooldown, cur->additional_cd_every,
                                           shots, count, 0, cur->additional_cd_multiplier);
}

/* ---------------------------------------------------------------------------
 * One step over every ranged slot (weapon.gd:244-266 _physics_process).
 * ------------------------------------------------------------------------- */
BT_HD static void bt_ranged_step(BT_DEVICE BtState* s, int moving, int facing) {
    int count = (int)s->wpn_count;
    if (count <= 0) return;
    int can_move_attack = bt_ranged_can_move_attack(s);

    for (int slot = 0; slot < count && slot < BT_SLOTS; slot++) {
        unsigned short wid = s->wpn_id[slot];
        if (wid == BT_WEAPON_NONE || wid >= BT_NWEAPONS) continue;
        if (BT_WEAPONS[wid].kind != BT_WEAPON_KIND_RANGED) continue;

        BtPlayerWeaponStats st;
        if (!bt_weapon_stats(s, slot, &st)) continue;
        if (st.kind != BT_WEAPON_KIND_RANGED) continue;

        BT_CONST_PTR(BtWeaponGeom) G = &BT_WEAPON_GEOM[BT_WEAPONS[wid].geom];

        /* weapon.gd:255-258: the cooldown only runs while not shooting.  The
         * frame the animation ends still had _is_shooting set during its
         * _physics_process (it is cleared in the same frame's idle), so it does
         * NOT decrement either -- the next frame resumes the countdown. */
        int shooting = s->wrng_shoot[slot] ? 1 : 0;
        if (shooting) {
            s->wrng_anim[slot] -= 1.0f;
            /* recoil tween stage for the renderer: the first tween is
             * ceil((freeze+1)/2) frames, the second the rest. */
            float f1 = bt_ceilf((s->wrng_anim[slot] + 1.0f) * 0.5f);
            s->wrng_phase[slot] = (s->wrng_anim[slot] > f1) ? 0 : 1;
            if (s->wrng_anim[slot] <= 0.0f) {
                s->wrng_shoot[slot] = 0;
                s->wrng_phase[slot] = 0;
                s->wrng_anim[slot] = 0.0f;
            }
        } else if (s->wpn_cd[slot] > 0.0f) {
            s->wpn_cd[slot] -= 1.0f;
            if (s->wpn_cd[slot] < 0.0f) s->wpn_cd[slot] = 0.0f;
        }

        /* weapon.gd:244-253 rotation: the target direction while idle, the
         * recorded target+spread while shooting. */
        float ox, oy;
        bt_ranged_origin(s, count, slot, G, &ox, &oy);
        float detect = (float)st.max_range + BT_RANGED_DETECTION_RANGE;
        unsigned short tgt = (unsigned short)BT_RANGED_NO_TARGET;
        float tdist = 0.0f;
        int has = bt_ranged_pick_target(s, ox, oy, (float)st.min_range, detect, &tgt, &tdist);
        /* weapon.gd:243-262 rotation: `(_current_target[0].global_position -
         * global_position).angle()`, i.e. the TARGET's Node transform.  A
         * MODE_CHARACTER enemy's Node transform is one physics step behind the
         * integrated body (brotato_state.h:749-765), so the aim angle is measured
         * from s->ehx/s->ehy, not from this frame's s->ex/s->ey.  The origin is
         * the weapon's own `global_position`. */
        float rot;
        if (shooting) {
            rot = has ? bt_atan2f(s->ehy[tgt] - oy, s->ehx[tgt] - ox) + s->wrng_spread[slot]
                      : s->wrng_rot[slot];
        } else if (has) {
            rot = bt_atan2f(s->ehy[tgt] - oy, s->ehx[tgt] - ox);
        } else {
            /* _current_idle_angle: _idle_angle(0) when facing right, PI otherwise. */
            rot = (facing >= 0) ? 0.0f : BT_PI;
        }
        s->wrng_rot[slot] = rot;

        /* weapon.gd:268-291 should_shoot.  The band test is
         * `Utils.is_between(_current_target[1], min_range, max_range + 50)`, and
         * `is_between(number: int, min_value: int, max_value: int)` declares its
         * arguments INT, so GDScript narrows the float distance
         * `_current_target[1]` (get_nearest_no_max returns sqrt(d2)) toward zero
         * before the comparison: a target at 350.64 px inside a max_range 300
         * band compares as `350 <= 350` and FIRES.  Verified on the pinned
         * 3.7-dev1 engine (is_between(350.639496, 0, 350) == true,
         * is_between(350.5, 0, 350) == true, is_between(351.4, 0, 350) == false).
         * The narrowing is reproduced with a C int cast, not a float compare. */
        int in_band = has &&
            ((int)tdist <= (int)((float)st.max_range + BT_RANGED_RANGE_PROBE));
        if (!shooting && s->wpn_cd[slot] <= 0.0f &&
            (!moving || can_move_attack) && in_band) {
            bt_ranged_shoot(s, slot, count, wid, &st, rot);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Runtime hooks (BT_WEAPON_RANGED_HOOKS).  Each no-ops when the slot does not
 * hold a ranged weapon.
 * ------------------------------------------------------------------------- */
BT_HD static void bt_ranged_clear_slot(BT_DEVICE BtState* s, int slot) {
    s->wrng_shoot[slot] = 0;
    s->wrng_phase[slot] = 0;
    s->wrng_anim[slot] = 0.0f;
    s->wrng_spread[slot] = 0.0f;
    s->wrng_rot[slot] = 0.0f;
    s->wrng_shots[slot] = 0;
    s->wrng_attack_id[slot] = 1;   /* ShootingBehavior.__next_attack_id starts at 1 */
}

/* Weapon._ready -> init_stats(at_wave_begin=true).  The source's _ready runs
 * from add_child BEFORE current_weapons.push_back, so get_nb_weapons() (and the
 * get_max_rand_cooldown bound) sees the PRE-ADD count = wpn_count - 1.  Inventory
 * has already stored the copy and incremented wpn_count when it calls this.
 * Consumes EXACTLY ONE rand_range (weapon.gd:137-142 + :318-336). */
BT_HD static void bt_weapon_ranged_slot_reset(BT_DEVICE BtState* s, int slot) {
    if (slot < 0 || slot >= BT_SLOTS) return;
    unsigned short wid = s->wpn_id[slot];
    if (wid == BT_WEAPON_NONE || wid >= BT_NWEAPONS) return;
    if (BT_WEAPONS[wid].kind != BT_WEAPON_KIND_RANGED) return;

    bt_ranged_clear_slot(s, slot);

    BtPlayerWeaponStats st;
    if (!bt_weapon_stats(s, slot, &st)) return;
    int count = (int)s->wpn_count - 1;
    if (count < 0) count = 0;

    s->wpn_cd[slot] = bt_wfx_next_cooldown(s, st.cooldown, st.additional_cd_every,
                                           0, count, 1, st.additional_cd_multiplier);
    bt_ranged_reset_cooldown(s, slot, &st);
}

/* ---------------------------------------------------------------------------
 * Wave begin: the game loads a FRESH game scene every wave
 * (`base_shop.gd:345-346`: current_wave += 1 then
 * get_tree().change_scene(MenuData.game_scene); main.gd:1134 _change_scene), so
 * the player node -- and therefore EVERY Weapon node -- is re-created from the
 * persistent RunData loadout at the start of each wave:
 *   player.gd:315-320 apply_items_effects: for i in weapons.size(): add_weapon(...)
 *   player.gd:376-390 add_weapon: instance = weapon.scene.instance() ... add_child
 *   weapon.gd:69-88 _ready -> init_stats() with at_wave_begin = true
 *   weapon.gd:137-142 `if at_wave_begin: _current_cooldown = get_next_cooldown(true)`
 *   weapon.gd:318-336 get_next_cooldown(at_wave_begin = true): cooldown_basis is
 *     clamped to 180, is_big_reload_active() is false, and ONE rand_range draw
 *     picks the fresh cooldown; reset_cooldown() then clamps it.
 * get_nb_weapons() (player.gd:288 = current_weapons.size()) is read from
 * get_max_rand_cooldown BEFORE the push_back of the node being created, i.e.
 * weapon `i` draws with weapon_count == i.
 *
 * So a wave begin = node re-creation: reset the copy's runtime state and draw a
 * FRESH cooldown, in slot order, exactly one global-stream rand_range draw per
 * ranged weapon.  `player.gd:269 reset_weapons_cd` (the only other cooldown
 * touch at a boundary) has NO caller anywhere in the recovered sources, so this
 * is not a reset_cooldown clamp.
 *
 * For wave 1 the same thing happens, but the copies are created by
 * bt_character_starting_apply -> bt_weapon_insert -> bt_weapon_rt_reset, which
 * already draws in slot order (wpn_count - 1 == i); the caller must therefore
 * NOT call this for w == 1 (it would double-draw the same weapon).
 *
 * The nodes are created by the scene swap, which lands in the GO frame's
 * MessageQueue flush (see bt_core_step), so their first `_physics_process` --
 * and therefore the first decrement of this fresh cooldown -- is the NEXT tick:
 * the row of the tick that runs this creation reads the draw untouched, exactly
 * like a run's row 0.
 * ------------------------------------------------------------------------- */
BT_HD static void bt_ranged_wave_begin_slot(BT_DEVICE BtState* s, int slot) {
    if (slot < 0 || slot >= BT_SLOTS) return;
    unsigned short wid = s->wpn_id[slot];
    if (wid == BT_WEAPON_NONE || wid >= BT_NWEAPONS) return;
    if (BT_WEAPONS[wid].kind != BT_WEAPON_KIND_RANGED) return;
    BtPlayerWeaponStats st;
    if (!bt_weapon_stats(s, slot, &st)) return;
    if (st.kind != BT_WEAPON_KIND_RANGED) return;

    bt_ranged_clear_slot(s, slot);
    /* weapon_count == slot: the node being created is the (slot)-th, and
     * player.get_nb_weapons() (player.gd:288) counts only the nodes already
     * pushed.  at_wave_begin == 1 disables the big-reload short circuit. */
    s->wpn_cd[slot] = bt_wfx_next_cooldown(s, st.cooldown, st.additional_cd_every,
                                           0, slot, 1, st.additional_cd_multiplier);
    bt_ranged_reset_cooldown(s, slot, &st);
}

/* Every ranged slot in order (the source create loop is global, so the core's
 * bt_weapon_wave_begin dispatcher calls the per-slot hook in slot order). */
BT_HD static void bt_ranged_wave_begin(BT_DEVICE BtState* s) {
    int count = (int)s->wpn_count;
    for (int slot = 0; slot < count && slot < BT_SLOTS; slot++)
        bt_ranged_wave_begin_slot(s, slot);
}

BT_HD static void bt_weapon_ranged_slot_move(BT_DEVICE BtState* s, int from, int to) {
    if (from < 0 || from >= BT_SLOTS || to < 0 || to >= BT_SLOTS || from == to) return;
    s->wrng_shoot[to] = s->wrng_shoot[from];
    s->wrng_phase[to] = s->wrng_phase[from];
    s->wrng_anim[to] = s->wrng_anim[from];
    s->wrng_spread[to] = s->wrng_spread[from];
    s->wrng_rot[to] = s->wrng_rot[from];
    s->wrng_shots[to] = s->wrng_shots[from];
    s->wrng_attack_id[to] = s->wrng_attack_id[from];
}

/* Vacated slot: zero the runtime arrays; never a reset draw (Inventory: removes
 * and combines only erase -- an appended result gets its own reset). */
BT_HD static void bt_weapon_ranged_slot_erase(BT_DEVICE BtState* s, int slot) {
    if (slot < 0 || slot >= BT_SLOTS) return;
    bt_ranged_clear_slot(s, slot);
}

/* stats_manager.gd:36-45 -> Weapon.init_stats(at_wave_begin=false) ->
 * reset_cooldown() only.  ZERO RNG draws.  Call it only when a stat that feeds
 * the weapon actually changed (otherwise cooldowns shorten needlessly). */
BT_HD static void bt_weapon_ranged_stats_changed(BT_DEVICE BtState* s, int slot) {
    if (slot < 0 || slot >= BT_SLOTS) return;
    unsigned short wid = s->wpn_id[slot];
    if (wid == BT_WEAPON_NONE || wid >= BT_NWEAPONS) return;
    if (BT_WEAPONS[wid].kind != BT_WEAPON_KIND_RANGED) return;
    BtPlayerWeaponStats st;
    if (!bt_weapon_stats(s, slot, &st)) return;
    bt_ranged_reset_cooldown(s, slot, &st);
}

/* bt_core_reset / a fresh main.tscn: every ranged runtime array zeroed. */
BT_HD static void bt_ranged_reset(BT_DEVICE BtState* s) {
    for (int i = 0; i < BT_SLOTS; i++) bt_ranged_clear_slot(s, i);
}

/* ---------------------------------------------------------------------------
 * Sprite surface for the backends: the recoil tween moves Sprite.position
 * (ranged_weapon_shooting_behavior.gd:25-42), and the muzzle rides the sprite.
 * dx/dy are in the weapon's local frame -- rotate by wrng_rot to place them in
 * the world.  Visual only: nothing in the simulation reads it.
 * ------------------------------------------------------------------------- */
BT_HD static void bt_ranged_sprite_offset(BT_DEVICE BtState* s, int slot,
                                          BT_THREAD float* dx, BT_THREAD float* dy) {
    *dx = 0.0f; *dy = 0.0f;
    if (slot < 0 || slot >= BT_SLOTS) return;
    if (!s->wrng_shoot[slot]) return;
    unsigned short wid = s->wpn_id[slot];
    if (wid == BT_WEAPON_NONE || wid >= BT_NWEAPONS) return;
    if (BT_WEAPONS[wid].kind != BT_WEAPON_KIND_RANGED) return;
    BtPlayerWeaponStats st;
    if (!bt_weapon_stats(s, slot, &st)) return;
    float freeze = bt_ranged_recoil_freeze_frames(st.recoil_duration);
    if (freeze <= 0.0f) return;
    float half_dur = bt_ceilf((freeze + 1.0f) * 0.5f);
    float elapsed = freeze - s->wrng_anim[slot];
    if (elapsed < 0.0f) elapsed = 0.0f;
    float rec = (float)st.recoil;
    float d = st.recoil_duration * 60.0f;   /* tween duration in ticks */
    if (elapsed < half_dur) *dx = bt_ranged_ease_expo_out(0.0f, -rec, elapsed, d);
    else *dx = bt_ranged_ease_expo_out(-rec, rec, elapsed - half_dur, d);
    *dy = 0.0f;
}

#endif /* BROTATO_WEAPON_RANGED_H */
