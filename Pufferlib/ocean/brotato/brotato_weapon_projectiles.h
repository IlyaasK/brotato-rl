/* brotato_weapon_projectiles.h -- the PLAYER RANGED WEAPON projectile graph.
 *
 * This module owns the shots a Ranged weapon (weapon.gd -> RangedWeapon ->
 * RangedWeaponShootingBehavior.shoot_projectile -> WeaponService.spawn_projectile)
 * and the item ProjectileEffect emit, plus the nested projectiles a hit spawns
 * (ProjectilesOnHitEffect).  Everything is derived from the recovered Brotato
 * v1.1.14.6 (campaign) + brotato_abyss dlc_1 (Abyss) sources:
 *
 *   projectiles/projectile.gd:28-67,70-90     motion, stop_delay drift,
 *                                             destroy_on_leaving_screen, stop()
 *                                             -> _return_to_pool()
 *   projectiles/player_projectile.gd:4-7,28-34 rotation_speed +25 deg/frame,
 *                                             range lifetime stop
 *   projectiles/player_projectile.gd:37-64    shoot_ex: velocity/rotation/
 *                                             damage/knockback/effect_scale
 *   projectiles/player_projectile.gd:74-79    _time_until_max_range =
 *                                             (max_range + 100)/projectile_speed
 *                                             (50 with manual aim)
 *   projectiles/player_projectile.gd:103-117  hit-once-per-target (ignored_objects),
 *                                             bounce > pierce > stop, piercing
 *                                             damage decay
 *   projectiles/player_projectile.gd:119-129  bounce(): retarget a random OTHER
 *                                             enemy, INFINITE_RANGE, reset
 *                                             knockback, bounce damage decay
 *   projectiles/player_projectile.gd:132-154  critically_hit_something ->
 *                                             pierce_on_crit / bounce_on_crit
 *   projectiles/delayed_player_projectile.gd  random delay before the hitbox arms
 *   projectiles/bullet_taser,dlc bullet_harpoon,particle_accelerator
 *                                             second slow hitbox (add_decaying_speed)
 *   entities/units/unit/unit.gd:534-608       hurt_area_entered_deferred: an
 *                                             ExplodingEffect replaces the direct
 *                                             hit; projectiles_on_hit children;
 *                                             speed_percent_modifier
 *   entities/units/unit/unit.gd:275-333       per-hit direction/knockback
 *   overlap/hitbox.gd                         ignored_objects, hit_something
 *   singletons/weapon_service.gd:34-135,366-435  init_ranged_stats + spawn
 *   singletons/entity_spawner.gd:480-490      get_rand_enemy (bounce/auto target)
 *   weapons/weapon_stats/ranged_weapon_stats.gd  piercing/bounce/max_range/speed
 *   dlc_1_data.gd                             DLC ranged weapons share the path
 *
 * The generated scene table already carries each projectile scene's Hitbox
 * circle (BT_ITEM_SCENES[].hit_radius/hit_x/hit_y), so the callers pass the
 * resolved geometry; this module invents no defaults and no independent table.
 *
 * REUSE: the per-target damage is the SHARED enemy-hit primitive
 * (brotato_item_explosions.h bt_item_expl_hit_enemy) -- G1 boss multiplier,
 * per-target crit, above-hp bonus %, FLAT armor, burning, and the iterative
 * death callback (bt_neutral_resolve_hit + bt_credit_kill).  Explosions go
 * through bt_item_expl_spawn.  Knockback and speed decay go through
 * brotato_physics_math.h (bt_stored_knockback / bt_piercing_knockback_amount /
 * bt_pull_knockback_amount / bt_add_decaying_speed_amt).  No second damage,
 * armor, burn, explosion or death formula exists here, and nothing recurses.
 *
 * STATE REQUIREMENT:
 *   brotato_state.h: `#include "brotato_weapon_projectile_types.h"` (placed
 *     after BT_MAX is defined) and two BtState members:
 *       BtWProjPool wproj;               -- the pool (declared capacity)
 *       float       player_lifesteal_cd; -- the player LifestealTimer (0.1 s)
 *   The types (BT_MAX_WPROJ, BT_WPROJ_F_* flags, BtWProj / BtWProjPool /
 *   BtWProjArgs / BtWProjChild) live in brotato_weapon_projectile_types.h, which
 *   this header also includes.
 *
 * Capacity (see the types header): the unmodified source loadout that maximises
 * concurrent shots is 6x chain_gun tier 4 (nb_projectiles 3, cooldown 2 ticks,
 * max_range 500, projectile_speed 3000 -> ceil(((500+100)/3000)*60) = 12 frames
 * of flight each => 3 * 12/2 = 18 in flight per weapon, 108 for six).  256
 * covers that with a >2x margin for the DLC ranged weapons plus pierce
 * survivors; a bounce (range reset to INFINITE 10000 -> 202 frames) can still
 * exhaust any finite pool, and then bt_wproj_spawn FAILS EXPLICITLY (returns -1
 * and increments wproj.n_overflow) -- an observable failure, never a silent
 * eviction.
 *
 * Hooks (brotato_core.h):
 *   bt_core_reset:        bt_wproj_reset(s);
 *   bt_clear_wave_summons: bt_wproj_reset(s);   (a fresh main.tscn per wave)
 *   bt_core_step:         (void)bt_wproj_step(s, &r);
 *   include this header AFTER brotato_item_explosions.h.
 *
 * Over the declared capacity a spawn FAILS EXPLICITLY: bt_wproj_spawn returns -1
 * and increments wproj.n_overflow.  A LIVE projectile is never overwritten.
 *
 * POD + BT_HD only, no allocation, no host API -> CPU/CUDA/MSL portable.
 */
#pragma once

/* ---------------------------------------------------------------------------
 * Spawn-argument structs (canonical API for Ranged weapons + the Effects
 * module) live in brotato_weapon_projectile_types.h; the function bodies below
 * use them.  Values are the caller's already init_ranged_stats()-resolved
 * numbers; the carried effect rows are the fx_first/fx_count canonical slice.
 * ------------------------------------------------------------------------- */
#include "brotato_weapon_projectile_types.h"
/* BtEnemyHitContext (WeaponEffects, dependency-free POD types header): handed to
 * the shared enemy-hit primitive so a player shot's own effect rows reach the
 * single damage formula. */
#include "brotato_weapon_effect_types.h"
/* bt_combat_audio_special_projectile: the ONCE-PER-CHILD nested-shot pick +
 * positional SoundManager2D play (0, 0.2) that manage_special_spawn_projectile
 * performs (weapon_service.gd:343-344).  Audio call sites live with their owner
 * (CombatAudioRuntime); this module only invokes the shared helper at the exact
 * source point -- no duplicated audio formula. */
#include "brotato_combat_audio.h"


/* WeaponEffects helpers (brotato_weapon_effects.h), forward-declared so this
 * header's include order is free; the definitions are static in the same TU. */
BT_HD static int bt_wfx_weapon_explode(BT_DEVICE BtState* s, unsigned short row,
                                       float x, float y, float dmg,
                                       float crit_chance, float crit_dmg,
                                       float burn_chance, float burn_dmg, float burn_dur,
                                       float curse_mod);
BT_HD static int bt_wfx_one_shot(BT_DEVICE BtState* s,
                                 BT_CONST_PTR(unsigned short) rows, int n_rows);
BT_HD static float bt_wfx_current_health_damage(BT_DEVICE BtState* s, int enemy_slot,
                                                BT_CONST_PTR(unsigned short) rows, int n_rows,
                                                float factor, float endless_factor);
BT_HD static float bt_wfx_lifesteal(BT_DEVICE BtState* s, BT_DEVICE float* cd_seconds,
                                    float lifesteal);

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

/* A pooled respawn retains its Node identity; a new object in the same actor
 * slot does not. The core resets the identity arena with these projectiles. */
BT_HD static unsigned int bt_wproj_eid(BT_DEVICE BtState* s, int j) {
    return bt_object_uid(s, j);
}
BT_HD static int bt_wproj_ignored(BT_DEVICE const BtWProj* p, unsigned int id) {
    if (id == 0u || id > BT_OBJECT_UID_MAX) return 0;
    return bt_uidpair_has(&p->target, id >> 5, 1u << (id & 31u));
}
BT_HD static int bt_wproj_mark_target(BT_DEVICE BtState* s, BT_DEVICE BtWProj* p,
                                      unsigned int id, int slow) {
    int r;
    if (id == 0u || id > BT_OBJECT_UID_MAX) {
        p->ign_overflow = 1;
        BT_CAP_LATCH(s, BT_CAP_WPROJ_SET);
        return 0;
    }
    r = bt_uidpair_set(&p->target, id >> 5, 1u << (id & 31u), slow);
    if (r < 0) { p->ign_overflow = 1; BT_CAP_LATCH(s, BT_CAP_WPROJ_SET); return 0; }  /* loud: set capacity exhausted */
    return r;
}
BT_HD static void bt_wproj_ignore(BT_DEVICE BtState* s, BT_DEVICE BtWProj* p, unsigned int id) {
    (void)bt_wproj_mark_target(s, p, id, 0);
}
BT_HD static void bt_wproj_clear_targets(BT_DEVICE BtWProj* p) {
    p->target.n = 0;
    p->ign_overflow = 0;
}

BT_HD static void bt_wproj_reset(BT_DEVICE BtState* s) {
    for (int i = 0; i < (int)s->wproj.used; i++) {
        BT_DEVICE BtWProj* p = &s->wproj.p[i];
        p->x = 0.0f; p->y = 0.0f; p->vx = 0.0f; p->vy = 0.0f;
        p->rot = 0.0f; p->rad = 0.0f; p->offx = 0.0f; p->offy = 0.0f;
        p->dmg = 0.0f; p->range_timer = 0.0f; p->speed = 0.0f;
        p->crit_chance = 0.0f; p->crit_dmg = 0.0f;
        p->burn_chance = 0.0f; p->burn_dmg = 0.0f; p->burn_dur = 0.0f;
        p->kb_dx = 0.0f; p->kb_dy = 0.0f; p->kb_amt = 0.0f; p->kb_pierce = 0.0f;
        p->speed_pct = 0.0f;
        p->slow_rad = 0.0f; p->slow_amt = 0.0f;
        p->rot_speed = 0.0f; p->stop_delay = 0.0f; p->stop_elapsed = 0.0f;
        p->delay = 0.0f; p->lifesteal = 0.0f;
        p->piercing = 0; p->bounce = 0;
        p->pierce_reduce = 0.0f; p->bounce_reduce = 0.0f;
        p->crit_pierce = 0; p->crit_bounce = 0;
        p->attack_id = 0;
        p->n_hits = 0u; p->hit_dmg = 0.0f; p->hit_crit = 0;
        p->target.n = 0; p->ign_overflow = 0;
        p->owner_uid = 0xFFFFFFFFu;
        p->src_weapon_id = 0xFFFFu; p->src_curse = 0.0f;
        for (int c = 0; c < BT_WEAPON_FX_CURSE_MAX; c++) p->src_fxcurse[c] = 0.0f;
        p->fx_first = 0; p->fx_count = 0; p->is_elemental = 0;
        p->s_nscaling = 0;
        for (int q = 0; q < BT_WPROJ_MAX_SKEY; q++) p->s_key[q] = 0;
        p->onhit_used = 0;
        p->onhit_n = 0; p->onhit_auto = 0;
        p->onhit_dmg = 0.0f; p->onhit_crit_chance = 0.0f; p->onhit_crit_dmg = 0.0f;
        p->onhit_burn_chance = 0.0f; p->onhit_burn_dmg = 0.0f; p->onhit_burn_dur = 0.0f;
        p->onhit_range = 0.0f; p->onhit_speed = 0.0f;
        p->onhit_rad = 0.0f; p->onhit_offx = 0.0f; p->onhit_offy = 0.0f;
        p->onhit_pierce_reduce = 0.0f; p->onhit_bounce_reduce = 0.0f;
        p->onhit_piercing = 0; p->onhit_bounce = 0;
        p->shape = BT_SHAPE_CIRCLE; p->flags = 0; p->armed = 0; p->alive = 0;
    }
    for (int q = 0; q < (int)s->wproj.used; q++) {
        s->wproj.cache_res[q] = 0;
        s->wproj.cache_scene[q] = 0;
        s->wproj.cache_delay[q] = 0.0f;
        s->wproj.cache_seq[q] = 0u;
    }
    s->wproj.seq_next = 0u;
    s->wproj.cre_next = 0u;
    for (int i = 0; i < (int)s->wproj.used; i++) s->wproj.cre[i] = 0;
    s->wproj.n_spawned = 0;
    s->wproj.n_overflow = 0;
    s->wproj.n_live = 0;
    s->wproj.used = 0;
    /* the deferred hit queue lives beside the pool: a reset must not leave
     * pairs that name slots of the previous wave (the drain's identity tokens
     * would reject them, but nothing should be queued at all). */
    s->hitq.n = 0;
    s->hitq.overflow = 0;
    s->player_lifesteal_cd = 0.0f;
}

/* _return_to_pool (projectile.gd:70-90): velocity zeroed, slot freed. */
BT_HD static void bt_wproj_return(BT_DEVICE BtState* s, int i) {
    bt_wproj_clear_targets(&s->wproj.p[i]);
    s->wproj.p[i].vx = 0.0f;
    s->wproj.p[i].vy = 0.0f;
    s->wproj.p[i].alive = 0;
    if (s->wproj.n_live > 0u) s->wproj.n_live--;
    s->wproj.cache_res[i] = 1;                        /* resident node */
    s->wproj.cache_seq[i] = ++s->wproj.seq_next;      /* free-order ordinal */
}

/* Projectile.stop (projectile.gd:56-67): hitbox off, ignored_objects cleared;
 * with stop_delay > 0 it drifts stop_delay frames before returning to the pool,
 * otherwise it returns immediately. */
BT_HD static void bt_wproj_stop(BT_DEVICE BtState* s, int i) {
    BT_DEVICE BtWProj* p = &s->wproj.p[i];
    if (p->flags & BT_WPROJ_F_STOPPING) return;
    if (p->stop_delay > 0.0f) {
        p->flags |= BT_WPROJ_F_STOPPING;
        p->stop_elapsed = 0.0f;
        bt_wproj_clear_targets(p);
        return;
    }
    bt_wproj_return(s, i);
}

/* _set_time_until_max_range (player_projectile.gd:74-79).  Auto-aim uses the
 * full PROJECTILE_ADDITIONAL_DISTANCE; the core trains auto-aim, so +100. */
#define BT_WPROJ_EXTRA_DIST 100.0f
#define BT_WPROJ_INFINITE_RANGE 10000.0f

BT_HD static float bt_wproj_range_time(float max_range, float speed) {
    float sp = speed > 0.0f ? speed : 1.0f;
    return (max_range + BT_WPROJ_EXTRA_DIST) / sp;
}

/* Forward declarations for the deferred-pair helpers (defined below, after the
 * overlap predicate they share).  A single TU, so a declaration + static inline
 * definition pair is all the CUDA/MSL backends need. */
BT_HD static void bt_wproj_enqueue_hit(BT_DEVICE BtState* s, int i, int j);
BT_HD static void bt_wproj_detect(BT_DEVICE BtState* s, int i, int first_hit_slot);
BT_HD static void bt_wproj_apply_hit(BT_DEVICE BtState* s, int i, int j,
                                     BT_THREAD float* reward, BT_THREAD int* killed_total);

/* Canonical spawn (WeaponService.spawn_projectile -> projectile.shoot_ex).
 * Fills the first free slot; returns the slot index, or -1 when every
 * BT_MAX_WPROJ slot is live (wproj.n_overflow++ -- a live shot is never
 * overwritten).  `ignore_slot >= 0` marks a nested projectiles_on_hit child
 * (unit.gd:598-604: the child is told to ignore the unit whose deferred hurt
 * spawned it, BEFORE its hitbox can report anything); a top-level shot passes
 * -1.  `in_phase` marks a spawn that happens INSIDE the projectile phase (the
 * deferred-hurt flush of a wproj hit); everything else (weapon/pet/itemsfx and
 * the melee hitbox, all of which run in a later phase) passes 0.
 *   * Hitbox.enable() is set_deferred (hitbox.gd:104-106) and
 *     MessageQueue::flush (SceneTree::iteration, scene_tree.cpp:609) drains it
 *     in the SAME frame, before that frame's Physics2DServer::step -- so the
 *     spawn frame's step already tests the hitbox at the SPAWN position and the
 *     pair is queued here;
 *   * an OUT-OF-PHASE node is in the tree before this frame's step and gets its
 *     first _physics_process (the first move) with the next projectile phase
 *     (armed = 1);
 *   * an IN-PHASE node joins the tree at this frame's flush, i.e. during the
 *     projectile phase itself: the phase still pairs it (this spawn is that
 *     pairing -- the caller's pass does not run a second scan for it) but its
 *     first move is the next frame (armed = 0).
 * Either way nothing here damages: the victim's handler is deferred
 * (unit.gd:536,610-611) and the pair is drained by the next projectile phase. */
BT_HD static int bt_wproj_spawn_impl(BT_DEVICE BtState* s, BtWProjArgs a,
                                     int ignore_slot, int in_phase) {
    /* Source node pool (main._player_projectiles, keyed by the projectile
     * scene): reuse the MOST RECENTLY RETURNED resident node of the SAME scene;
     * else a never-instantiated slot; never take another scene's resident node.
     * No descriptor available -> explicit overflow (a live shot is never
     * overwritten and another scene's cache survives until the wave reset). */
    int i = -1;
    int fresh = 0;
    unsigned int best_seq = 0u;
    for (int q = 0; q < (int)s->wproj.used; q++) {
        if (s->wproj.p[q].alive || !s->wproj.cache_res[q]) continue;
        if (s->wproj.cache_scene[q] != a.scene_istr) continue;
        if (i < 0 || s->wproj.cache_seq[q] > best_seq) { i = q; best_seq = s->wproj.cache_seq[q]; }
    }
    if (i < 0) {
        for (int q = 0; q < BT_MAX_WPROJ; q++)
            if (!s->wproj.p[q].alive && !s->wproj.cache_res[q]) { i = q; fresh = 1; break; }
    }
    if (i < 0) { s->wproj.n_overflow++; return -1; }
    s->wproj.cache_res[i] = 0;
    s->wproj.cache_scene[i] = a.scene_istr;
    BT_DEVICE BtWProj* p = &s->wproj.p[i];
    float sp = a.speed;
    p->x = a.x; p->y = a.y;
    p->vx = bt_cosf(a.rot) * sp;
    p->vy = bt_sinf(a.rot) * sp;
    p->rot = a.rot;
    p->speed = sp;
    p->rad = a.rad; p->offx = a.offx; p->offy = a.offy;
    p->dmg = a.dmg;
    p->crit_chance = a.crit_chance; p->crit_dmg = a.crit_dmg;
    p->burn_chance = a.burn_chance; p->burn_dmg = a.burn_dmg; p->burn_dur = a.burn_dur;
    p->kb_dx = a.knockback != 0.0f ? bt_cosf(a.rot) : 0.0f;
    p->kb_dy = a.knockback != 0.0f ? bt_sinf(a.rot) : 0.0f;
    p->kb_amt = a.knockback; p->kb_pierce = a.knockback_piercing;
    p->speed_pct = a.speed_pct;
    p->slow_rad = a.slow_rad; p->slow_amt = a.slow_amt;
    p->rot_speed = a.rot_speed;
    p->stop_delay = a.stop_delay; p->stop_elapsed = 0.0f;
    /* DelayedPlayerProjectile.delay is drawn once per freshly instantiated
     * scene node and retained by that node's scene-keyed resident cache. */
    if ((a.flags & BT_WPROJ_F_DELAYED) != 0u) {
        if (fresh) s->wproj.cache_delay[i] = bt_rand_range(s, 0.0f, 3.0f);
        p->delay = s->wproj.cache_delay[i];
    } else {
        p->delay = a.delay;
    }
    p->lifesteal = a.lifesteal;
    p->piercing = a.piercing; p->pierce_reduce = a.pierce_reduce;
    p->bounce = a.bounce; p->bounce_reduce = a.bounce_reduce;
    p->crit_pierce = a.crit_pierce; p->crit_bounce = a.crit_bounce;
    p->attack_id = a.attack_id;
    p->n_hits = 0u; p->hit_dmg = 0.0f; p->hit_crit = 0;
    bt_wproj_clear_targets(p);
    p->owner_uid = a.owner_uid;
    p->src_weapon_id = a.src_weapon_id; p->src_curse = a.src_curse;
    for (int c = 0; c < BT_WEAPON_FX_CURSE_MAX; c++) p->src_fxcurse[c] = a.src_fxcurse[c];
    p->fx_first = a.fx_first; p->fx_count = a.fx_count; p->is_elemental = a.is_elemental;
    /* the immutable hitbox scaling view (only when the caller raised the flag, so
     * an unset args member is never read) */
    p->s_nscaling = 0;
    for (int q = 0; q < BT_WPROJ_MAX_SKEY; q++) p->s_key[q] = 0;
    if ((a.flags & BT_WPROJ_F_SKEY) != 0u) {
        int ns = (int)a.s_nscaling;
        if (ns > BT_WPROJ_MAX_SKEY) ns = 0;   /* impossible for the POD type */
        p->s_nscaling = (unsigned char)ns;
        for (int q = 0; q < ns; q++) p->s_key[q] = a.s_key[q];
    }
    p->onhit_used = 0;
    p->shape = (unsigned char)a.shape;
    p->flags = a.flags;
    if (a.slow_rad > 0.0f) p->flags |= BT_WPROJ_F_SLOW;
    p->onhit_n = a.onhit_n; p->onhit_auto = a.onhit_auto;
    p->onhit_dmg = a.child.dmg;
    p->onhit_crit_chance = a.child.crit_chance; p->onhit_crit_dmg = a.child.crit_dmg;
    p->onhit_burn_chance = a.child.burn_chance; p->onhit_burn_dmg = a.child.burn_dmg;
    p->onhit_burn_dur = a.child.burn_dur;
    p->onhit_range = a.child.max_range; p->onhit_speed = a.child.speed;
    p->onhit_rad = a.child.rad; p->onhit_offx = a.child.offx; p->onhit_offy = a.child.offy;
    p->onhit_pierce_reduce = a.child.pierce_reduce; p->onhit_bounce_reduce = a.child.bounce_reduce;
    p->onhit_piercing = a.child.piercing; p->onhit_bounce = a.child.bounce;
    p->range_timer = bt_wproj_range_time(a.max_range, sp);
    /* See the header comment: an out-of-phase shot is already in the tree for
     * this frame's step (armed = 1), an in-phase child is added at this frame's
     * flush and only moves from the next frame on (armed = 0). */
    p->armed = (unsigned char)(in_phase ? 0 : 1);
    p->alive = 1;
    if (i >= (int)s->wproj.used) s->wproj.used = (unsigned short)(i + 1);
    /* the shot's creation ordinal: close enough to a Godot instance id for the
     * (victim creation, hitbox creation) resolution order. */
    s->wproj.cre[i] = (unsigned short)(++s->wproj.cre_next);
    s->wproj.n_spawned++;
    s->wproj.n_live++;
    if (ignore_slot >= 0) {
        /* unit.gd:602-604 set_ignored_objects([self]) on the fresh child: the
         * token is registered BEFORE the child's first detection pass, so the
         * unit whose hurt spawned it (its own Hurtbox overlaps the child at the
         * spawn position) is never paired. */
        bt_wproj_ignore(s, p, bt_wproj_eid(s, ignore_slot));
    }
    /* This frame's step already tests the fresh hitbox at the SPAWN position
     * (see the header comment), so pair it here with the full scan: the enqueue
     * is what the camera-side `area_entered` would have reported. */
    bt_wproj_detect(s, i, 0);
    return i;
}

/* Top-level spawn: WeaponService.spawn_projectile from the weapon/pet/itemsfx
 * phases (all of which run after the projectile phase), so nothing is ignored
 * and the shot is in the tree before this frame's step. */
BT_HD static int bt_wproj_spawn(BT_DEVICE BtState* s, BtWProjArgs a) {
    return bt_wproj_spawn_impl(s, a, -1, 0);
}

/* ---------------------------------------------------------------------------
 * Targeting (entity_spawner.gd:480-490 get_rand_enemy + Utils.get_rand_element
 * -> Array.pick_random, i.e. the GLOBAL rng; the core models it with s->rng).
 * Live enemies only (the source `enemies` array excludes neutral trees).
 * Returns a live slot, or -1.  The re-roll loop mirrors the source exactly. */
BT_HD static int bt_wproj_rand_enemy(BT_DEVICE BtState* s, int exclude) {
    int n = 0;
    for (int j = bt_enemy_next(s, 0, 1); j < (int)s->e_used; j = bt_enemy_next(s, j + 1, 1)) if (s->ealive[j] && !s->eneutral[j]) n++;
    if (n <= 0 || (n <= 1 && exclude >= 0)) return -1;
    int pick = -1;
    do {
        int r = (int)bt_rng_bounded(&s->rng, (unsigned int)n);
        int seen = 0;
        for (int j = bt_enemy_next(s, 0, 1); j < (int)s->e_used; j = bt_enemy_next(s, j + 1, 1)) {
            if (!s->ealive[j] || s->eneutral[j]) continue;
            if (seen == r) { pick = j; break; }
            seen++;
        }
    } while (exclude >= 0 && n > 1 && pick == exclude);
    return pick;
}

/* ---------------------------------------------------------------------------
 * Per-target hit consequences
 * ------------------------------------------------------------------------- */

/* unit.gd:275-333 knockback: the 0.2/0.8 weighted direction, the piercing
 * correction, then the stored knockback vector is REPLACED (bt_stored_knockback).
 * The pull distance term uses the victim's cached contact velocity
 * (s->phys.bodies[j].vel) as its movement input, the only per-unit movement the
 * core retains. */
BT_HD static void bt_wproj_knockback(BT_DEVICE BtState* s, int i, int j) {
    BT_DEVICE BtWProj* p = &s->wproj.p[i];
    float amount = p->kb_amt;
    /* unit.gd:281-282 weights the hitbox direction with the attacker's and the
     * victim's Node positions whenever the hit carries a valid player index, and
     * unit.gd:333 then uses that direction for the stored vector WHATEVER the
     * amount's sign -- the 0.2/0.8 form is not a pull-only rule (the melee path
     * already reads it the same way).  The victim's own `global_position` is the
     * MODE_CHARACTER Node transform, one step behind s->ex/s->ey, and the player
     * is MODE_KINEMATIC (already this frame's s->px/s->py). */
    BtVec2 victim = bt_v2(s->ehx[j], s->ehy[j]);
    BtVec2 dir = bt_knockback_direction_weighted(bt_v2(p->kb_dx, p->kb_dy),
                                                 bt_v2(s->px, s->py), victim);
    /* unit.gd:348: die() seeds the stored vector with the WEIGHTED direction
     * (0.2*hitbox + 0.8*attacker->victim, unit.gd:281) times the RAW hitbox
     * amount floored at MIN_DEATH_KNOCKBACK_AMOUNT.  Recorded before the
     * amount==0 early-out: an explosion/kb-free projectile can still kill. */
    {
        BtVec2 dkb = bt_death_knockback(dir, p->kb_amt);
        s->edkbx[j] = dkb.x; s->edkby[j] = dkb.y;
    }
    if (amount == 0.0f) return;
    BT_CONST_PTR(BtBodyPhysics) G = bt_kgeom(s, j);
    float res = G->knockback_resistance;
    if (amount < 0.0f) {
        amount = bt_pull_knockback_amount(amount,
                                          victim, G->hurt_radius,
                                          s->espd[j], s->phys.bodies[j].vel,
                                          bt_v2(p->x, p->y), p->rad,
                                          p->speed, bt_v2(p->vx, p->vy),
                                          p->kb_pierce, res);
    } else if (p->kb_pierce > 0.0f) {
        amount = bt_piercing_knockback_amount(amount, p->kb_pierce, res);
    }
    BtVec2 stored = bt_stored_knockback(dir, amount);
    s->ekbx[j] = stored.x;
    s->ekby[j] = stored.y;
}

/* RunData.manage_life_steal (run_data.gd:1297-1302) via the canonical WeaponEffects
 * helper, gated by the shared player LifestealTimer (0.1 s, one_shot).  The
 * countdown (s->player_lifesteal_cd) is reset by bt_wproj_reset and advanced
 * EXACTLY ONCE per step by bt_wproj_step, in seconds. */
BT_HD static void bt_wproj_lifesteal(BT_DEVICE BtState* s, int i) {
    (void)bt_wfx_lifesteal(s, &s->player_lifesteal_cd, s->wproj.p[i].lifesteal);
}

/* PlayerProjectile.bounce (player_projectile.gd:119-129). */
BT_HD static void bt_wproj_bounce(BT_DEVICE BtState* s, int i, int hit) {
    BT_DEVICE BtWProj* p = &s->wproj.p[i];
    p->bounce--;
    int tgt = bt_wproj_rand_enemy(s, hit);
    float dir;
    if (tgt >= 0) dir = bt_atan2f(s->ey[tgt] - p->y, s->ex[tgt] - p->x);
    else          dir = bt_rand_range(s, -BT_PI, BT_PI);
    float mag = bt_sqrtf(p->vx * p->vx + p->vy * p->vy);
    p->vx = bt_cosf(dir) * mag;
    p->vy = bt_sinf(dir) * mag;
    p->rot = bt_atan2f(p->vy, p->vx);
    p->range_timer = bt_wproj_range_time(BT_WPROJ_INFINITE_RANGE, p->speed);
    p->kb_amt = 0.0f; p->kb_pierce = 0.0f; p->kb_dx = 0.0f; p->kb_dy = 0.0f;
    if (p->dmg > 0.0f) {
        float v = p->dmg - p->dmg * p->bounce_reduce;
        if (v < 1.0f) v = 1.0f;
        p->dmg = (float)bt_float_to_int(v);   /* Hitbox.damage is an int */
    }
}

/* ---------------------------------------------------------------------------
 * Nested on-hit children (ProjectilesOnHitEffect): unit.gd:586-604 +
 * weapon_service.gd:339-380.
 *
 * weapon.gd:134-140 captures the IMMUTABLE triple (effect.value = child COUNT,
 * the nested RangedWeaponStats, effect.auto_target_enemy).  The nested stats are
 * the canonical BT_ITEM_WSTATS row the effect row carries as BtItemEff.stats, so
 * the child's scene, geometry, slow hitbox, delayed flag, elemental type and
 * shooting sounds are PURE FUNCTIONS of one canonical row: nothing here is
 * re-resolved from the live inventory, so a recycled/combined weapon slot can
 * never change what a live child does.  Both a live projectile (which captures
 * the row through its carried canonical fx span) and a melee Hitbox hit (which
 * captures it through its weapon's canonical fx span) go through one path.
 * ------------------------------------------------------------------------- */

/* The canonical BT_ITEM_WSTATS row of the LAST ProjectilesOnHitEffect row in a
 * canonical fx span (weapon.gd's loop OVERWRITES projectiles_on_hit per match),
 * or BT_WEAPON_NONE.  ZERO RNG. */
BT_HD static unsigned short bt_wproj_onhit_wrow(unsigned short fx_first, unsigned short fx_count) {
    unsigned short out = BT_WEAPON_NONE;
    for (int e = 0; e < (int)fx_count; e++) {
        int idx = (int)fx_first + e;
        if (idx >= BT_NWEAPON_EFF_INDEX) break;
        int row = (int)BT_WEAPON_EFF_INDEX[idx];
        if (row < 0 || row >= BT_NITEM_EFF_ROWS) continue;
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[row];
        if (E->opcode != BT_ITEMEFF_PROJECTILES_ON_HIT) continue;
        if ((int)E->stats < BT_NITEM_WSTATS) out = E->stats;
    }
    return out;
}

/* The nested stats row's effective scaling keys == the CHILD hitbox's
 * scaling_stats (weapon_service.gd spawn_projectile builds HitboxArgs with
 * `set_from_weapon_stats(weapon_stats)` where weapon_stats is the nested
 * RangedWeaponStats), and the child's elemental damage type.  Fills up to `max`
 * keys into `keys`, returns 1 when any of them is stat_elemental_damage.
 * ZERO RNG, and a pure function of the immutable row. */
BT_HD static int bt_wproj_onhit_scaling(unsigned short wrow, BT_THREAD unsigned short* keys,
                                        int max, BT_THREAD int* n_out) {
    int n = 0, elem = 0;
    if (wrow < BT_NITEM_WSTATS) {
        BT_CONST_PTR(BtItemWeaponStats) W = &BT_ITEM_WSTATS[wrow];
        for (int i = 0; i < (int)W->scaling_count && n < max; i++) {
            unsigned int si = (unsigned int)W->scaling_first + (unsigned int)i;
            if (si >= BT_NITEM_SCALING) break;
            unsigned short k = BT_ITEM_SCALING[si].stat;
            keys[n++] = k;
            if (k == BT_ISTR_STAT_ELEMENTAL_DAMAGE) elem = 1;
        }
    }
    if (n_out) *n_out = n;
    return elem;
}

/* Scenes whose root script is DelayedPlayerProjectile: `var delay =
 * rand_range(0.0, 3.0)` is a NODE MEMBER initialiser, drawn once per node
 * construction and retained by that node's scene-keyed pool entry (never per
 * shot).  The four recovered scenes are the base delayed_player_projectile.gd
 * (laser / lightning / spike) plus the bullet_taser subclass.  (Ranged's
 * bt_ranged_scene_is_delayed keeps its own list for the ordinary shot path; this
 * one is the projectile/child list.) */
BT_HD static int bt_wproj_scene_is_delayed(unsigned short scene) {
    return scene == (unsigned short)BT_ISTR_RES___PROJECTILES_BULLET_LASER_DELAYED_BULLET_LASER_TSCN ||
           scene == (unsigned short)BT_ISTR_RES___PROJECTILES_BULLET_LIGHTNING_DELAYED_LIGHTNING_PROJECTILE_TSCN ||
           scene == (unsigned short)BT_ISTR_RES___PROJECTILES_SPIKE_BULLET_SPIKE_TSCN ||
           scene == (unsigned short)BT_ISTR_RES___PROJECTILES_BULLET_TASER_DELAYED_TASER_PROJECTILE_TSCN;
}

/* unit.gd:586-604 + weapon_service.gd:339-380: spawn `count` nested on-hit shots
 * at (x,y).  ONE boring path shared by the ranged projectile hit and the melee
 * Hitbox hit.  Source RNG order PER CHILD (NOT per hit):
 *   (1) the caller's rand_range(-PI, PI) direction (unit.gd:589),
 *   (2) manage_special_spawn_projectile's guarded nested shooting_sounds pick +
 *       SoundManager2D.play(clip, pos, 0, 0.2) (weapon_service.gd:343-344),
 *   (3) entity_spawner.get_rand_enemy(self) when auto_target_enemy (:346-348),
 * then the spawn itself -- a delayed child scene's own rand_range(0, 3) lives in
 * bt_wproj_spawn's scene-keyed node pool and is drawn at the instantiation the
 * source runs inside spawn_projectile, i.e. AFTER all of the above.
 *
 * `wrow` is the immutable nested stats row; `ch` the resolved child stats
 * (bt_weapon_stats_onhit).  `ignore` is the source-object identity of the enemy
 * that spawned the child; each child ignores it (set_ignored_objects([self])).
 * `in_phase` is 1 for the projectiles_on_hit children of a weapon shot's own
 * deferred hurt (spawned inside the projectile phase) and 0 for the melee
 * hitbox's children (the melee phase runs after it) -- see bt_wproj_spawn_impl. */
BT_HD static void bt_wproj_spawn_onhit_n(BT_DEVICE BtState* s, float x, float y, int ignore,
                                         int in_phase,
                                         int count, int auto_target, unsigned short wrow,
                                         BT_THREAD const BtWProjChild* ch,
                                         unsigned int owner_uid, unsigned short src_weapon_id,
                                         float src_curse, float lifesteal) {
    if (count <= 0) return;
    unsigned short cscene = (wrow < BT_NITEM_WSTATS)
        ? BT_ITEM_WSTATS[wrow].projectile_scene : (unsigned short)BT_ISTR_NONE;
    BT_CONST_PTR(BtWeaponProjSpec) cs = bt_weapon_proj_spec(cscene);
    unsigned short ckeys[BT_WPROJ_MAX_SKEY];
    int cn = 0;
    unsigned char is_elem = (unsigned char)(bt_wproj_onhit_scaling(wrow, ckeys,
                                                                  BT_WPROJ_MAX_SKEY, &cn) ? 1 : 0);
    unsigned char cflags = (unsigned char)(BT_WPROJ_F_LEAVE | BT_WPROJ_F_SKEY |
                          (bt_wproj_scene_is_delayed(cscene) ? BT_WPROJ_F_DELAYED : 0u));
    for (int k = 0; k < count; k++) {
        float dir = bt_rand_range(s, -BT_PI, BT_PI);
        bt_combat_audio_special_projectile(s, (int)wrow);
        if (auto_target) {
            int t = bt_wproj_rand_enemy(s, ignore);
            if (t >= 0) dir = bt_atan2f(s->ey[t] - y, s->ex[t] - x);
        }
        BtWProjArgs c;
        c.x = x; c.y = y; c.rot = dir;
        c.speed = ch->speed;
        c.dmg = ch->dmg;
        c.crit_chance = ch->crit_chance; c.crit_dmg = ch->crit_dmg;
        c.burn_chance = ch->burn_chance; c.burn_dmg = ch->burn_dmg; c.burn_dur = ch->burn_dur;
        c.max_range = ch->max_range;
        c.knockback = 0.0f; c.knockback_piercing = 0.0f; c.speed_pct = 0.0f;
        c.piercing = ch->piercing; c.pierce_reduce = ch->pierce_reduce;
        c.bounce = ch->bounce; c.bounce_reduce = ch->bounce_reduce;
        /* the child's OWN scene geometry (the nested stats row's scene). */
        c.rad = cs->hit_radius; c.offx = cs->hit_x; c.offy = cs->hit_y;
        c.shape = BT_SHAPE_CIRCLE;
        c.crit_pierce = 0; c.crit_bounce = 0;
        /* source never sets the child hitbox's player_attack_id (only the
         * ordinary ranged shot path does), so the weapon's on_weapon_hit_something
         * early-returns for a child hit: no attack tally, no break_on_hit. */
        c.attack_id = -1;
        c.rot_speed = cs->rot_speed; c.stop_delay = cs->stop_delay; c.delay = cs->delay;
        c.scene_istr = cscene;
        c.slow_rad = cs->slow_rad; c.slow_amt = cs->slow_amt;
        c.lifesteal = lifesteal;
        c.src_weapon_id = src_weapon_id; c.owner_uid = owner_uid; c.src_curse = src_curse;
        /* unit.gd:586-604 children carry effects [] -> no per-effect curse. */
        for (int q = 0; q < BT_WEAPON_FX_CURSE_MAX; q++) c.src_fxcurse[q] = 0.0f;
        c.fx_first = 0; c.fx_count = 0;
        c.is_elemental = is_elem;
        /* the child hitbox's own scaling view (its nested stats row). */
        c.s_nscaling = (unsigned char)cn;
        for (int q = 0; q < BT_WPROJ_MAX_SKEY; q++) c.s_key[q] = 0;
        for (int q = 0; q < cn; q++) c.s_key[q] = ckeys[q];
        c.flags = cflags;
        c.onhit_n = 0; c.onhit_auto = 0;   /* one level deep */
        c.child.dmg = 0.0f; c.child.crit_chance = 0.0f; c.child.crit_dmg = 0.0f;
        c.child.burn_chance = 0.0f; c.child.burn_dmg = 0.0f; c.child.burn_dur = 0.0f;
        c.child.max_range = 0.0f; c.child.speed = 0.0f;
        c.child.rad = 0.0f; c.child.offx = 0.0f; c.child.offy = 0.0f;
        c.child.piercing = 0; c.child.pierce_reduce = 0.0f;
        c.child.bounce = 0; c.child.bounce_reduce = 0.0f;
        c.child.shape = BT_SHAPE_CIRCLE;
        /* the child is told to ignore the unit it spawned from (unit.gd:602-604)
         * as part of the spawn, BEFORE its first detection pass. */
        (void)bt_wproj_spawn_impl(s, c, ignore, in_phase);
    }
}

/* unit.gd:586-604 for a live projectile hit: spawn the shot's captured
 * ProjectilesOnHitEffect children.  The nested stats row is a pure function of
 * the shot's IMMUTABLE carried fx span, so a recycled owning slot cannot change
 * a live child. */
BT_HD static void bt_wproj_spawn_onhit(BT_DEVICE BtState* s, int pi, float x, float y, int ignore) {
    BT_DEVICE BtWProj* p = &s->wproj.p[pi];
    if (p->onhit_n == 0) return;
    BtWProjChild ch;
    ch.dmg = p->onhit_dmg;
    ch.crit_chance = p->onhit_crit_chance; ch.crit_dmg = p->onhit_crit_dmg;
    ch.burn_chance = p->onhit_burn_chance; ch.burn_dmg = p->onhit_burn_dmg;
    ch.burn_dur = p->onhit_burn_dur;
    ch.max_range = p->onhit_range; ch.speed = p->onhit_speed;
    ch.rad = p->onhit_rad; ch.offx = p->onhit_offx; ch.offy = p->onhit_offy;
    ch.piercing = p->onhit_piercing; ch.pierce_reduce = p->onhit_pierce_reduce;
    ch.bounce = p->onhit_bounce; ch.bounce_reduce = p->onhit_bounce_reduce;
    ch.shape = BT_SHAPE_CIRCLE;
    unsigned short wrow = bt_wproj_onhit_wrow(p->fx_first, p->fx_count);
    /* 1: this runs from the projectile phase's deferred-hurt flush. */
    bt_wproj_spawn_onhit_n(s, x, y, ignore, 1, (int)p->onhit_n, (int)p->onhit_auto, wrow, &ch,
                           p->owner_uid, p->src_weapon_id, p->src_curse, p->lifesteal);
}

/* The slow hitbox (bullet_taser / bullet_harpoon / particle_accelerator): a
 * SEPARATE Area2D with its own (larger) circle; each enemy it overlaps is slowed
 * once per projectile (deals no damage).  tracked by the per-projectile
 * `slowed` bitset. */
BT_HD static void bt_wproj_slow(BT_DEVICE BtState* s, int i) {
    BT_DEVICE BtWProj* p = &s->wproj.p[i];
    if (!(p->flags & BT_WPROJ_F_SLOW)) return;
    float cx = p->x, cy = p->y;
    for (int j = bt_enemy_next(s, 0, 1); j < (int)s->e_used; j = bt_enemy_next(s, j + 1, 1)) {
        if (!s->ealive[j]) continue;
        BT_CONST_PTR(BtBodyPhysics) G = bt_kgeom(s, j);
        float dx = cx - (s->ex[j] + G->hurt_x), dy = cy - (s->ey[j] + G->hurt_y);
        float rr = p->slow_rad + G->hurt_radius;
        if (dx * dx + dy * dy > rr * rr) continue;
        if (!bt_wproj_mark_target(s, p, bt_wproj_eid(s, j), 1)) continue;
        /* thing_hit.add_decaying_speed(-200 / -400) */
        s->edecay[j] = bt_add_decaying_speed(bt_float_to_int(p->slow_amt), s->espd[j], s->edecay[j]);
    }
}

/* ---------------------------------------------------------------------------
 * One step of every live player projectile (call once per step, after the
 * player/weapon fire and BEFORE the explosion tick).  Returns the number of
 * kills; *reward gains R_KILL for each.
 * ------------------------------------------------------------------------- */
/* Projectile-vs-enemy Hurtbox overlap at an explicit projectile centre (cx,cy).
 * Shared by the serial scan below and the parallel candidate pre-scan
 * (btf_combat_phase_wproj), so both evaluate the identical predicate -- the
 * only difference is which projectile centre the caller passes. */
BT_HD static int bt_wproj_overlaps(BT_DEVICE BtState* s, int i, int j, float cx, float cy) {
    BT_CONST_PTR(BtBodyPhysics) G = bt_kgeom(s, j);
    float dx = cx - (s->ex[j] + G->hurt_x);
    float dy = cy - (s->ey[j] + G->hurt_y);
    float rr = s->wproj.p[i].rad + G->hurt_radius;
    return (dx * dx + dy * dy <= rr * rr) ? 1 : 0;
}

/* `first_hit` is an optional per-projectile hint: first_hit[i] is a lower bound
 * on the first enemy slot j (0..BT_MAX) the projectile's main hitbox can touch
 * this step, computed from a pre-step snapshot by a parallel scan. It is a
 * pure speed-up: the body still re-tests every j from first_hit[i] on, and
 * every predicate it skips (j < first_hit[i]) was false at the snapshot and
 * cannot become true during the step (enemy positions are frozen for the whole
 * step, `alive`/`ignored` only lose entries, charm is only ever SET inside this
 * module). first_hit == 0 (or NULL) is the serial reference. */
/* One DEFERRED Hurtbox hit: everything unit.gd's hurt handler does for a target,
 * run at the frame the engine flushes the reported pair (unit.gd:610-611
 * call_deferred).  `i` is the projectile slot, `j` the target enemy slot; both
 * have already been re-validated against the queue's identity tokens. */
BT_HD static void bt_wproj_apply_hit(BT_DEVICE BtState* s, int i, int j,
                                     BT_THREAD float* reward, BT_THREAD int* killed_total) {
    BT_DEVICE BtWProj* p = &s->wproj.p[i];
    /* fly.gd:6-11: the target's Hurtbox area_entered handler rolls the
     * reaction chance when the entering hitbox's `from` IS a
     * RangedWeapon (or a shooting Pet -- those shots live in the `pr*`
     * pool and are handled there).  The hurt itself is deferred
     * (unit.gd:610-611), but the ROLL happens in the handler, so it
     * precedes every damage draw below; a nested projectiles_on_hit
     * child inherits the parent weapon's canonical id
     * (bt_wproj_spawn_onhit_n), so it triggers exactly when its own
     * weapon is a RangedWeapon. */
    {
        int src_wid = (int)p->src_weapon_id;
        int from_ranged = (src_wid != (int)BT_WEAPON_NONE &&
                           src_wid >= 0 && src_wid < BT_NWEAPONS &&
                           (int)BT_WEAPONS[src_wid].kind == BT_WEAPON_KIND_RANGED);
        bt_enemy_fly_react(s, j, from_ranged);
    }
    int exploded = 0;
    int is_crit = 0;
    float taken = 0.0f;
    /* The shot's canonical effect-row view (Data appends weapon effect
     * graphs to BT_ITEM_EFF_ROWS; BT_WEAPON_EFF_INDEX[fid] is the row id).
     * The pointer stays in the constant table, so it is MSL-legal as the
     * BtEnemyHitContext.rows view. */
    BT_CONST_PTR(unsigned short) rows = (p->fx_count > 0u)
        ? (BT_CONST_PTR(unsigned short))(&BT_WEAPON_EFF_INDEX[(int)p->fx_first]) : 0;
    int n_rows = (int)p->fx_count;
    /* (0) unit.gd:548-556: EVERY hurt first runs the enemy effect
     * behaviors' on_hurt, so charm_on_hit rolls ONCE per hurt BEFORE the
     * exploding roll and the damage.  A successful charm sets e_charmed,
     * and bt_item_expl_hit_enemy then absorbs the hit (0 damage), exactly
     * like the source's charm() setting _absorb_next_damage before
     * take_damage.  Same consumer and the same point as the melee Hitbox
     * hit; the enemy-side timer/damage is EnemyMotionIntegration's. */
    if (bt_wfx_charm_roll(s, j, (int)s->charm_count[0], 999, rows, n_rows, 1.0f))
        bt_enemy_lifecycle_charm(s, j, 0);
    /* (0b) the same on_hurt phase feeds enemy_percent_damage_taken from
     * this hitbox's own rows gated by its own immutable scaling view (a
     * shot with no view passes 0/0, so only a stat_all row applies --
     * exactly the _default_spawn_projectile_args case).  No RNG here; the
     * per-enemy timers tick once per step in bt_core_step and the bonus is
     * folded once inside bt_item_expl_hit_enemy. */
    {
        /* the shot's own scaling view as a MASK (it carries the firing
         * weapon's immutable keys while BT_WPROJ_F_SKEY is set), then the
         * source-order application: bucket first, then the hitbox rows
         * with each row's curse scalar. */
        unsigned int pdt_mask = 0u;
        if ((p->flags & BT_WPROJ_F_SKEY) != 0u)
            for (int q = 0; q < (int)p->s_nscaling; q++)
                pdt_mask = bt_enemy_hit_mask_add(pdt_mask, p->s_key[q]);
        bt_enemy_hit_bucket_apply(s, j, pdt_mask);
        for (int i = 0; i < n_rows; i++)
            (void)bt_enemy_hit_apply_row(s, j, (int)rows[i],
                i < BT_WEAPON_FX_CURSE_MAX ? p->src_fxcurse[i] : 0.0f, pdt_mask);
    }
    /* (1) carried ExplodingEffect rows (unit.gd:552-570): each rolls its
     * chance; on success the explosion spawns AT THE TARGET and the
     * target takes NO direct damage.  bt_wfx_weapon_explode delegates to
     * the shared bt_item_expl_spawn (no duplicated pool math). */
    for (int k = 0; k < n_rows && !exploded; k++) {
        float fc = (k < BT_WEAPON_FX_CURSE_MAX) ? p->src_fxcurse[k] : 0.0f;
        int pj = bt_wfx_weapon_explode_src(s, BT_WEAPON_EFF_INDEX[(int)p->fx_first + k],
                                           s->ex[j], s->ey[j], p->dmg,
                                           p->crit_chance, p->crit_dmg,
                                           p->burn_chance, p->burn_dmg, p->burn_dur,
                                           fc);
        if (pj >= 0) {
            /* unit.gd:569 `if from != null and from is Weapon:
             * explosion.connect("hit_something"/"killed_something", from,
             * ...)`: a kill this explosion causes is credited to the
             * firing weapon copy (weapon.gd:203-230 wave tally +
             * GainStatEveryKilledEnemiesEffect).  Only while a slot still
             * owns the uid (a freed/recycled weapon gets no credit). */
            int eslot = bt_weapon_slot_from_uid(s, p->owner_uid);
            if (eslot >= 0)
                bt_item_expl_set_source(s, pj, p->owner_uid, (int)p->fx_first,
                                        (int)p->fx_count, eslot);
            exploded = 1;
        }
    }
    if (!exploded) {
        /* unit.gd:581-585: the hitbox's burning_data roll + apply_burning run
         * BEFORE take_damage (unit.gd:262-303), so the damage/crit step sees
         * the target ALREADY burning (crit_on_hitting_burning_target makes it
         * a crit, bonus_non_elemental_damage_against_burning_targets applies).
         * can_burn_enemies is seeded > 0 for every player
         * (player_run_data.gd:501), so the source gate never blocks it; the
         * primitive is then called with burn = 0 so it cannot roll twice. */
        (void)bt_wfx_weapon_burn_apply(s, j,
                bt_wfx_scalar_keyed(s, BT_ISTR_CAN_BURN_ENEMIES),
                p->burn_chance, p->burn_dmg, p->burn_dur);
        /* unit.gd:280 `Utils.get_chance_success(crit_chance)` ==
         * utils.gd:397-403: chance == 0 -> no draw, false; else
         * randf() <= chance. The canonical helper (identical to every
         * other source chance roll) -- NOT a bounded(1000) surrogate. */
        is_crit = bt_chance(s, p->crit_chance) ? 1 : 0;
        int one_shot = bt_wfx_one_shot(s, rows, n_rows);
        /* unit.gd:288-294: the OneShotOnHitEffect sound precedes
         * take_damage's hurt/crit pick (inside the shared primitive). */
        if (one_shot) bt_combat_audio_one_shot(s);
        float factor = (bt_kind_class(s->ekind[j]) == 2) ? 1000.0f : 100.0f;
        float chd = bt_wfx_current_health_damage(s, j, rows, n_rows, factor, 1.0f);
        BtEnemyHitContext ctx;
        ctx.rows = rows;
        ctx.count = n_rows;
        ctx.is_elemental = (int)p->is_elemental;
        ctx.one_shot = one_shot;
        ctx.current_health_damage = chd;
        ctx.owner_uid = p->owner_uid;
        ctx.attack_id = p->attack_id;
        ctx.was_crit = 0;
        int k = 0;
        /* the modified shared primitive folds force-crit-when-burning, the
         * full weapon hit bonus (bt_wfx_hit_bonus_pct over ctx.rows),
         * one-shot / current-health damage, G1, flat armor, the neutral
         * hook and the iterative death; ctx.was_crit returns the resolved
         * crit.  burning is rolled+applied by the caller above
         * (unit.gd:581 before unit.gd:285), so burn args are 0 here. */
        taken = bt_item_expl_hit_enemy(s, j, p->dmg, is_crit, p->crit_dmg,
                                       0.0f, 0.0f, 0.0f,
                                       0.0f, 0.0f, 0, 1, &k, &ctx);
        is_crit = ctx.was_crit;
        if (k) { (*killed_total)++; *reward += R_KILL; }
        if (is_crit) {
            if (p->crit_pierce > 0) { p->piercing++; p->crit_pierce--; }
            if (p->crit_bounce > 0) { p->bounce++; p->crit_bounce--; }
        }
        bt_wproj_knockback(s, i, j);

        /* weapon.gd on_killed_something + unit.gd kill consumers: the
         * player-kill hooks fire even when the owning weapon copy is
         * gone (heal_on_kill, explode_on_overkill, gold_on_crit_kill);
         * the per-LIVE-weapon tally/kill callbacks resolve the uid ONLY
         * while a slot still owns it (never a recycled slot). */
        /* uid -> current slot, re-resolved after compaction; -1 is fine
         * (the tally is uid-scoped, only the block hint uses the slot). */
        int wslot = bt_weapon_slot_from_uid(s, p->owner_uid);
        if (k) {
            int gold_added = 0;
            bt_wfx_on_player_kill(s, j, is_crit, taken, rows, n_rows, &gold_added);
            /* unit.gd:367-382 gold_on_crit_kill: RunData.add_gold is a
             * DIRECT purse credit (run_data.gd:810-818) -- no coin, no XP,
             * no pickup path, so not bt_loot_spawn_gold. */
            if (gold_added > 0) s->materials += (float)gold_added;
            if (wslot >= 0)
                bt_wfx_on_weapon_kill(s, &s->wfx, wslot, p->owner_uid, p->attack_id,
                                      rows, n_rows, p->src_fxcurse);
        }
        /* weapon.gd:440-465 on_weapon_hit_something (per-attack HIT
         * tally + break_on_hit) is NOT here: unit.gd:608 calls
         * hitbox.hit_something UNCONDITIONALLY -- also for an EXPLODED
         * hit, with damage_dealt 0 -- so it runs in the shared tail
         * below, after the children and speed_percent_modifier. */
    }
    /* ---- unit.gd:586-608 + player_projectile.gd:103-117 shared tail ----
     * Source order for EVERY damaging hit (exploded or not):
     *   projectiles_on_hit children      unit.gd:586-604
     *   speed_percent_modifier           unit.gd:605-606
     *   hitbox.hit_something(self, dmg_taken[1])   unit.gd:608
     *     -> player_projectile._on_Hitbox_hit_something: FIRST emits its
     *        own hit_something (ranged_weapon.gd:19 connects the weapon to
     *        it), so weapon.gd:440-465 on_weapon_hit_something runs BEFORE
     *        lifesteal / ignored_objects / bounce / pierce / stop. */

    /* unit.gd:586-604 projectiles_on_hit children (RNG per child) */
    bt_wproj_spawn_onhit(s, i, s->ex[j], s->ey[j], j);

    /* unit.gd:605-606 speed_percent_modifier runs for any damaging hitbox
     * (the exploding branch above is still inside `if deals_damage`). */
    if (p->speed_pct != 0.0f)
        s->edecay[j] = bt_add_decaying_speed_amt(s->espd[j] * p->speed_pct * 0.01f,
                                                 s->espd[j], s->edecay[j]);

    /* hit_something event record (damage BEFORE this hit's pierce/bounce
     * branch, which the oracle compares): the emitted value is the
     * post-armor damage dealt (0 when the hit exploded). */
    p->n_hits++;
    p->hit_dmg = taken;
    p->hit_crit = (unsigned char)is_crit;

    /* weapon.gd:440-465 on_weapon_hit_something: per-attack HIT tally +
     * break_on_hit.  A break drops gold and Inventory removes the owning
     * UID (never a different slot).  Runs for an exploded hit too. */
    {
        int wslot2 = bt_weapon_slot_from_uid(s, p->owner_uid);
        int break_gold = bt_wfx_on_weapon_hit(s, &s->wfx, wslot2, p->owner_uid,
                                              p->attack_id, rows, n_rows, p->src_fxcurse);
        if (break_gold > 0) {
            /* break gold IS a coin (wanted_to_break ->
             * wanted_to_spawn_gold -> main.gd spawn_gold), unlike the
             * direct crit-kill credit above. */
            bt_loot_spawn_gold(s, (float)break_gold, s->ex[j], s->ey[j], 1);
            if (wslot2 >= 0) {
                /* the LIVE owning copy breaks (player.gd:395-408):
                 * gold first, then ONE breaking-sounds pick on the
                 * main SoundManager always_play, then the weapon data
                 * is removed. */
                bt_combat_audio_weapon_break(s);
                bt_weapon_remove(s, wslot2);
            }
        }
    }

    /* player_projectile.gd:105 lifesteal on hit */
    bt_wproj_lifesteal(s, i);

    /* player_projectile.gd:107-117 bounce > pierce > stop. */
    if (p->bounce > 0) {
        bt_wproj_bounce(s, i, j);
    } else if (p->piercing <= 0) {
        bt_wproj_stop(s, i);
        /* no `break`: the engine's queued handlers of this frame all resolve */
    } else {
        p->piercing--;
        if (p->dmg > 0.0f) {
            float v = p->dmg - p->dmg * p->pierce_reduce;
            if (v < 1.0f) v = 1.0f;
            p->dmg = (float)bt_float_to_int(v);   /* Hitbox.damage is an int */
        }
    }
    /* NOTE: unlike the old same-frame scan, a stop()/bounce here does NOT stop
     * the queue: the engine already queued every reported handler of the frame,
     * and each runs its own tail. */
}

/* Candidates collected per scan of one shot's target scan: one batch is the
 * common case (a shot newly frames a handful of Hurtboxes), so the walk stays a
 * single pass; a denser frame needs one extra pass per BT_WPROJ_ORDER_BATCH
 * victims (the measured dense-crowd peak is 60 pairs in a whole frame, for all
 * shots together). */
#ifndef BT_WPROJ_ORDER_BATCH
#define BT_WPROJ_ORDER_BATCH 16
#endif

/* Pair detection for one live projectile at its CURRENT position: the area
 * `area_entered` event -- the ignored-object token is appended and each newly
 * overlapped Hurtbox is queued for the next frame's flush.  Nothing here draws
 * or damages: the source's handler is deferred (unit.gd:610-611).  `first_hit` is
 * the parallel pre-scan's lower bound (0 = the serial reference). */
BT_HD static void bt_wproj_detect(BT_DEVICE BtState* s, int i, int first_hit_slot) {
    BT_DEVICE BtWProj* p = &s->wproj.p[i];
    /* hitbox.gd:104-110 disable()/enable(): the collision shape is set_deferred
     * and stays DISABLED while a DelayedPlayerProjectile counts its delay down
     * (delayed_player_projectile.gd:8-17) and once Projectile.stop() has run
     * (projectile.gd:56-67, which also clears ignored_objects).  An Area2D with
     * a disabled collision shape reports no pair, so a disabled hitbox neither
     * pairs nor is paired. */
    if (p->flags & (BT_WPROJ_F_DELAYED | BT_WPROJ_F_STOPPING)) return;
    float sc = bt_cosf(p->rot), ss = bt_sinf(p->rot);
    float cx = p->x + (sc * p->offx - ss * p->offy);
    float cy = p->y + (ss * p->offx + sc * p->offy);
    int jstart = first_hit_slot;
    if (jstart < 0) jstart = BT_MAX;
    /* SIMULTANEOUS VICTIMS RESOLVE IN CREATION ORDER, not in slot order.  The
     * bridge resolves one step's Hurtbox `area_entered` events by ascending
     * (victim instance id, hitbox instance id) (`BROTATO_RL_ORDER_HURTS`,
     * mod_main.gd `_hurt_before`), and a Godot instance id increases with object
     * creation order -- which is exactly what `bt_wproj_eid` (the per-slot
     * `e_object_uid` = the pooled instance's uid, minted once per fresh node and
     * retained across a pooled reuse) encodes.  So the per-shot victims are
     * queued in ascending uid.  The batch keeps the common case ONE scan: the
     * scan collects up to BT_WPROJ_ORDER_BATCH candidates, the batch is sorted
     * by uid, and `ign` (already recorded per taken victim) lets the loop stop
     * as soon as a rescan finds none left. */
    for (;;) {
        short cand[BT_WPROJ_ORDER_BATCH];
        unsigned int cuid[BT_WPROJ_ORDER_BATCH];
        int nc = 0;
        for (int j = bt_enemy_next(s, jstart, 1); j < (int)s->e_used && nc < BT_WPROJ_ORDER_BATCH; j = bt_enemy_next(s, j + 1, 1)) {
            if (!s->ealive[j]) continue;
            if (s->e_charmed[j]) continue;
            unsigned int id = bt_wproj_eid(s, j);
            if (id == 0u) continue;   /* no token: taken by the pass below */
            if (bt_wproj_ignored(p, id)) continue;
            if (!bt_wproj_overlaps(s, i, j, cx, cy)) continue;
            cand[nc] = (short)j; cuid[nc] = id; nc++;
        }
        if (nc == 0) break;
        for (int a = 1; a < nc; a++) {          /* insertion sort by creation order */
            short sj = cand[a]; unsigned int su = cuid[a];
            int b = a - 1;
            while (b >= 0 && cuid[b] > su) { cand[b + 1] = cand[b]; cuid[b + 1] = cuid[b]; b--; }
            cand[b + 1] = sj; cuid[b + 1] = su;
        }
        for (int a = 0; a < nc; a++) {
            bt_wproj_ignore(s, p, cuid[a]);
            bt_wproj_enqueue_hit(s, i, (int)cand[a]);
        }
    }
    /* A target whose identity space is exhausted (id 0) can never be recorded,
     * so it is taken once per frame, in slot order, after the identified ones
     * -- the same single-per-frame behaviour this scan always had. */
    for (int j = bt_enemy_next(s, jstart, 1); j < (int)s->e_used; j = bt_enemy_next(s, j + 1, 1)) {
        if (!s->ealive[j]) continue;
        if (s->e_charmed[j]) continue;
        if (bt_wproj_eid(s, j) != 0u) continue;
        if (!bt_wproj_overlaps(s, i, j, cx, cy)) continue;
        bt_wproj_ignore(s, p, 0u);
        bt_wproj_enqueue_hit(s, i, j);
    }
}

/* Queue a pair for the next frame's flush.  Bounded and fail-closed: a pair that
 * does not fit is DROPPED and BT_CAP_HITQ latches (no silent reordering). */
BT_HD static void bt_wproj_enqueue_hit(BT_DEVICE BtState* s, int i, int j) {
    if ((int)s->hitq.n >= BT_HITQ_CAP) {
        s->hitq.overflow = 1;
        BT_CAP_LATCH(s, BT_CAP_HITQ);
        return;
    }
    int q = (int)s->hitq.n++;
    s->hitq.a[q] = (short)i;
    s->hitq.a_uid[q] = s->wproj.p[i].owner_uid;
    s->hitq.a_cre[q] = s->wproj.cre[i];
    s->hitq.b[q] = (short)j;
    s->hitq.b_uid[q] = bt_wproj_eid(s, j);
}

BT_HD static int bt_wproj_step_hint(BT_DEVICE BtState* s, BT_THREAD float* reward,
                                    const int* first_hit) {
    if (s->player_lifesteal_cd > 0.0f) {
        s->player_lifesteal_cd -= BT_DT;
        if (s->player_lifesteal_cd < 0.0f) s->player_lifesteal_cd = 0.0f;
    }
    /* default weaponless Bull: no live shot -> no per-slot scan at all. */
    if (s->wproj.n_live == 0u && s->hitq.n == 0u) return 0;
    int killed_total = 0;

    /* (0) THE ENGINE'S MESSAGE FLUSH.  Every pair the previous frame's
     * Physics2DServer::step reported resolves here, in report order: the
     * victim's handler is deferred (unit.gd:610-611) and runs at this frame's
     * flush, which is after this frame's `_physics_process` moves and before
     * that frame's step -- i.e. right here, before this frame's detection pass.
     * Identity-checked entries only, so a recycled slot is never hurt. */
    int qn = (int)s->hitq.n;
    /* RESOLUTION ORDER: ascending (victim creation, hitbox creation).  The
     * scan above enqueued shot by shot in wproj SLOT order, which is not
     * creation order (a freed slot is handed to the next spawn), so the drained
     * prefix is sorted here by (victim uid, shot ordinal) -- the bridge's
     * `_hurt_before` key.  Insertion sort, bounded by the pairs of one step. */
    for (int a = 1; a < qn; a++) {
        unsigned int bu = s->hitq.b_uid[a], au = s->hitq.a_uid[a], ac = s->hitq.a_cre[a];
        short ba = s->hitq.b[a], aa = s->hitq.a[a];
        int b = a - 1;
        while (b >= 0 && (s->hitq.b_uid[b] > bu ||
                          (s->hitq.b_uid[b] == bu && s->hitq.a_cre[b] > ac))) {
            s->hitq.b[b + 1] = s->hitq.b[b]; s->hitq.b_uid[b + 1] = s->hitq.b_uid[b];
            s->hitq.a[b + 1] = s->hitq.a[b]; s->hitq.a_uid[b + 1] = s->hitq.a_uid[b];
            s->hitq.a_cre[b + 1] = s->hitq.a_cre[b];
            b--;
        }
        s->hitq.b[b + 1] = ba; s->hitq.b_uid[b + 1] = bu;
        s->hitq.a[b + 1] = aa; s->hitq.a_uid[b + 1] = au; s->hitq.a_cre[b + 1] = (unsigned short)ac;
    }
    for (int q = 0; q < qn; q++) {
        int hi = (int)s->hitq.a[q];
        int hj = (int)s->hitq.b[q];
        if (hi < 0 || hi >= BT_MAX_WPROJ || hj < 0 || hj >= BT_MAX) continue;
        /* unit.gd:537-539: the victim's own guard.  A shot that this drain
         * already stopped (hitbox `active` false, projectile.gd:56-67) or whose
         * slot was recycled (owner_uid) never hurts; a recycled Hurtbox owner
         * (bt_wproj_eid) never does either. */
        if (!s->wproj.p[hi].alive) continue;
        if (s->wproj.p[hi].flags & BT_WPROJ_F_STOPPING) continue;
        if (s->wproj.p[hi].owner_uid != s->hitq.a_uid[q]) continue;
        if (!s->ealive[hj] || bt_wproj_eid(s, hj) != s->hitq.b_uid[q]) continue;
        bt_wproj_apply_hit(s, hi, hj, reward, &killed_total);
    }
    /* A nested child spawned by one of those hurts is added at this frame's
     * flush, so its hitbox is enabled for THIS frame's step: the spawn site
     * paired it (see bt_wproj_spawn_impl) and those entries are this frame's
     * pairs, resolved by the NEXT frame's flush.  Keep them, drop the resolved
     * prefix. */
    for (int q = qn; q < (int)s->hitq.n; q++) {
        int d = q - qn;
        s->hitq.a[d] = s->hitq.a[q];
        s->hitq.a_uid[d] = s->hitq.a_uid[q];
        s->hitq.b[d] = s->hitq.b[q];
        s->hitq.b_uid[d] = s->hitq.b_uid[q];
    }
    s->hitq.n = (unsigned short)((int)s->hitq.n - qn);

    for (int i = 0; i < (int)s->wproj.used; i++) {
        BT_DEVICE BtWProj* p = &s->wproj.p[i];
        if (!p->alive) continue;

        /* A projectile spawned INSIDE this phase (an on-hit child of the flush
         * above) joins the tree at this frame's flush, so this frame's step
         * still pairs it (armed = 0 from bt_wproj_spawn_impl, and the spawn site
         * ALREADY paired it with a full scan) but it neither moves nor ages
         * until the next frame.  Every other live shot was in the tree before
         * this frame's step and runs its own _physics_process here. */
        if (!p->armed) {
            p->armed = 1;
            if (p->flags & BT_WPROJ_F_SLOW) bt_wproj_slow(s, i);
            continue;
        }
        /* PlayerProjectile._physics_process:29-30 (+25 deg/frame spin). */
        if (p->rot_speed != 0.0f) p->rot += 25.0f * (BT_PI / 180.0f);

        /* Projectile._physics_process:34 position += velocity * delta */
        p->x += p->vx * BT_DT;
        p->y += p->vy * BT_DT;

        /* Projectile._physics_process:36-41 stop-delay drift. */
        if (p->flags & BT_WPROJ_F_STOPPING) {
            p->stop_elapsed += 1.0f;
            if (p->stop_elapsed >= p->stop_delay) bt_wproj_return(s, i);
            continue;
        }

        /* projectile.gd:43-44 destroy_on_leaving_screen: the despawn box is
         * ZoneService.current_zone_max_camera_rect == Rect2(-96,-192,2240,1824),
         * not the 0..BT_ARENA_W/H arena (brotato_combat.h derivation); a
         * weapon projectile keeps the Projectile default true, so it stops at
         * the CAMERA edge. */
        if ((p->flags & BT_WPROJ_F_LEAVE) &&
            !bt_cam_rect_has_point(p->x, p->y)) {
            bt_wproj_stop(s, i);
            continue;
        }

        /* PlayerProjectile._physics_process:32-34 range lifetime. */
        if (p->range_timer <= 0.0f) bt_wproj_stop(s, i);
        p->range_timer -= BT_DT;
        if (!p->alive) continue;                       /* immediate return */
        if (p->flags & BT_WPROJ_F_STOPPING) continue;  /* armed the drift delay */

        /* DelayedPlayerProjectile.gd:14-17 hitbox arms after `delay` frames. */
        if (p->flags & BT_WPROJ_F_DELAYED) {
            p->delay -= 1.0f;
            if (p->delay > 0.0f) continue;
            p->flags &= (unsigned char)~BT_WPROJ_F_DELAYED;
        }
        /* Pair detection at this frame's position (the engine's step): the
         * Hurtbox overlap, the ignored_objects append and the deferred queue. */
        bt_wproj_detect(s, i, first_hit ? first_hit[i] : 0);

        if (p->alive && !(p->flags & BT_WPROJ_F_STOPPING)) bt_wproj_slow(s, i);
    }
    return killed_total;
}

/* Serial reference: the core's bt_core_step call site. Identical to
 * bt_wproj_step_hint with no hint (first_hit == NULL => scan every slot). */
BT_HD static int bt_wproj_step(BT_DEVICE BtState* s, BT_THREAD float* reward) {
    return bt_wproj_step_hint(s, reward, 0);
}