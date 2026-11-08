/* brotato_weapon_projectile_types.h -- POD types for the player weapon
 * projectile graph (owner: WeaponProjectiles).
 *
 * Split out of brotato_weapon_projectiles.h so brotato_state.h can embed the
 * pool by value while the functions live in brotato_weapon_projectiles.h:
 *   brotato_state.h        includes THIS header (after BT_MAX is available, i.e.
 *                          anywhere from the projectile-pool constants down)
 *   brotato_weapon_projectiles.h includes THIS header (see the #if BT_MAX guard)
 *
 * Pure POD: fixed arrays, scalars, trivially copyable, no host pointers, so a
 * device backend may place one BtState per env directly in a buffer.
 *
 * IMMUTABLE SOURCE VIEW: a shot carries its firing weapon's canonical id
 * (src_weapon_id), the per-copy curse modifier (src_curse) and the canonical
 * effect-row span (fx_first/fx_count/is_elemental) captured at spawn.  These are
 * NOT re-resolved from the inventory, so a recycled/combined weapon slot can
 * never change what a live shot does.  owner_uid is kept ONLY so the per-copy
 * live weapon callbacks can be located (bt_weapon_slot_from_uid); a -1 result
 * means the owning copy is gone and does NOT clear the shot's effect view.
 */
#pragma once

/* Declared capacity of the player projectile pool.  The unmodified loadout that
 * maximises concurrent shots is 6x chain_gun tier 4 (nb_projectiles 3,
 * cooldown 2 ticks, max_range 500, projectile_speed 3000 -> 12 flight frames
 * each -> 3*12/2 = 18 in flight per weapon, 108 for six).  256 covers that with
 * a >2x margin for the DLC ranged weapons plus pierce survivors; a bounce
 * (range reset to INFINITE 10000 = 202 frames) can still exhaust any finite
 * pool, and then bt_wproj_spawn FAILS EXPLICITLY (returns -1, wproj.n_overflow++).
 * NEVER a silent eviction of a live shot. */
#define BT_MAX_WPROJ 256

/* Both hitboxes remember source Node identities, including objects no longer
 * alive. The shared per-wave identity arena bounds the bitsets. */
#define BT_WPROJ_OBJECT_WORDS ((BT_OBJECT_UID_MAX + 32) / 32)

/* ---------------------------------------------------------------------------
 * Compact sparse identity sets
 * ---------------------------------------------------------------------------
 * A projectile's ignored/slowed set and an explosion's hit set are bitmaps over
 * the source-object uid space (BT_OBJECT_UID_MAX + 1 bits = 194 words).  The
 * dense bitmap is ~97% empty on every live shot/explosion, so only the words
 * that are actually non-zero are stored, as (word, bits) entries in insertion
 * order.  This is an EXACT set: membership, insertion and clearing produce
 * exactly the same results as the dense bitmap, so behaviour is bit-identical;
 * clearing is O(touched) and reset is O(1) (no 194-word sweep).  BT_UID_SET_CAP
 * entries bound the touched-word count; a set that would exceed it reports
 * failure and the caller raises its LOUD *overflow flag -- never a silent
 * dropped target.  Measured peak touched words over long Demon runs:
 * 5 (projectiles) / 8 (explosions) vs a 194-word arena; the DEEP endless probe
 * with EVERY item granted (tests/brotato_state_size.cpp, wave 113) peaks at 13
 * touched words for both a projectile's ignored/slowed set and an explosion's
 * hit set, so 32 carries ~2.5x headroom while the word table already bounds at
 * BT_OBJECT_UID_MAX+1 words.
 */
#ifndef BT_UID_SET_CAP
#define BT_UID_SET_CAP 32
#endif

/* One set (explosions, melee swings): (word, bits) entries. */
typedef struct {
    unsigned short word[BT_UID_SET_CAP];
    unsigned int   bits[BT_UID_SET_CAP];
    unsigned char  n;
} BtUidSet;

/* Two disjoint halves sharing one word list (a projectile's ignored + slowed). */
typedef struct {
    unsigned short word[BT_UID_SET_CAP];
    unsigned int   ign[BT_UID_SET_CAP];
    unsigned int   slow[BT_UID_SET_CAP];
    unsigned char  n;
} BtUidPairSet;

BT_HD static inline int bt_uidpair_has(BT_DEVICE const BtUidPairSet* s, unsigned int word, unsigned int bit) {
    for (int i = 0; i < (int)s->n; i++)
        if (s->word[i] == word) return (int)((s->ign[i] | s->slow[i]) & bit);
    return 0;
}
/* 1 = bit newly set, 0 = already present, -1 = touched-word capacity exhausted. */
BT_HD static inline int bt_uidpair_set(BT_DEVICE BtUidPairSet* s, unsigned int word, unsigned int bit, int slow) {
    int i = 0;
    for (; i < (int)s->n; i++) if (s->word[i] == word) break;
    if (i == (int)s->n) {
        if (s->n >= BT_UID_SET_CAP) return -1;
        s->word[i] = (unsigned short)word; s->ign[i] = 0u; s->slow[i] = 0u;
        s->n++;
    }
    if ((slow ? s->slow[i] : s->ign[i]) & bit) return 0;
    if (slow) s->slow[i] |= bit; else s->ign[i] |= bit;
    return 1;
}

BT_HD static inline int bt_uidset_has(BT_DEVICE const BtUidSet* s, unsigned int word, unsigned int bit) {
    for (int i = 0; i < (int)s->n; i++)
        if (s->word[i] == word) return (int)(s->bits[i] & bit);
    return 0;
}
/* 1 = bit newly set, 0 = already present, -1 = touched-word capacity exhausted. */
BT_HD static inline int bt_uidset_set(BT_DEVICE BtUidSet* s, unsigned int word, unsigned int bit) {
    int i = 0;
    for (; i < (int)s->n; i++) if (s->word[i] == word) break;
    if (i == (int)s->n) {
        if (s->n >= BT_UID_SET_CAP) return -1;
        s->word[i] = (unsigned short)word; s->bits[i] = 0u;
        s->n++;
    }
    if (s->bits[i] & bit) return 0;
    s->bits[i] |= bit;
    return 1;
}

/* Behaviour flags (BtWProj.flags). */
#define BT_WPROJ_F_LEAVE    1u  /* Projectile.destroy_on_leaving_screen       */
#define BT_WPROJ_F_DELAYED  2u  /* DelayedPlayerProjectile (hitbox arms late) */
#define BT_WPROJ_F_SLOW     4u  /* a second slow hitbox exists (taser/harpoon)*/
#define BT_WPROJ_F_STOPPING 8u  /* stop() with stop_delay > 0: drifting out    */
#define BT_WPROJ_F_SKEY     16u /* the immutable hitbox scaling-key snapshot
                                 * (s_nscaling/s_key[]) is valid.  A caller that
                                 * does not raise it never has those members
                                 * read, so an unset view can never be read. */

/* Capacity of a shot's immutable hitbox scaling-key snapshot ==
 * BT_WSTATS_MAX_SCALING in brotato_weapon_stats.h.  Kept as a literal: that
 * header is included far later in the TU than this one (state.h pulls this
 * header at BT_OBJECT_UID_MAX, weapon_stats.h only near bt_apply_item). */
#define BT_WPROJ_MAX_SKEY 16

/* Max top-level effect curses a weapon copy can carry (WeaponInventory draws
 * one per effect; Data's canonical max is 2). */
#ifndef BT_WEAPON_FX_CURSE_MAX
#define BT_WEAPON_FX_CURSE_MAX 2
#endif

/* --- spawned by Ranged weapons / the item ProjectileEffect --------------
 * All values are the caller's already init_ranged_stats()-resolved numbers.
 * `x,y` is the muzzle position; `rot` is radians and is also the velocity
 * angle and the knockback direction.  The carried effect rows are the canonical
 * BT_WEAPON_EFF_INDEX slice [fx_first, fx_first+fx_count) into BT_ITEM_EFF_ROWS
 * (Data appends weapon effect graphs to that namespace); an ExplodingEffect among
 * them is what bt_wfx_weapon_explode rolls on a hit.  owner_uid is
 * BT_WEAPON_UID_NONE (0xFFFFFFFF) for an item/effect shot; the source weapon id is
 * BT_WEAPON_NONE for one too. */

/* One nested on-hit projectile's stats (ProjectilesOnHitEffect.weapon_stats after
 * init_ranged_stats); one level deep -- a child carries no effect rows of its own
 * (unit.gd:586-604 uses _default_spawn_projectile_args with effects []). */
typedef struct {
    float dmg, crit_chance, crit_dmg;
    float burn_chance, burn_dmg, burn_dur;
    float max_range, speed, rad, offx, offy;
    int   piercing; float pierce_reduce;
    int   bounce;   float bounce_reduce;
    int   shape;
} BtWProjChild;

typedef struct {
    float x, y, rot;
    float speed;                     /* RangedWeaponStats.projectile_speed   */
    float dmg, crit_chance, crit_dmg;
    float burn_chance, burn_dmg, burn_dur;
    float max_range;
    float knockback, knockback_piercing;
    float speed_pct;                 /* speed_percent_modifier               */
    int   piercing;  float pierce_reduce;   /* piercing_dmg_reduction        */
    int   bounce;    float bounce_reduce;   /* bounce_dmg_reduction          */
    float rad, offx, offy;           /* hit circle: centre pos+rotate(offx,offy) */
    int   shape;                     /* BT_SHAPE_* (player shots: circle)    */
    int   crit_pierce, crit_bounce;  /* player pierce_on_crit/bounce_on_crit charges */
    int   attack_id;                 /* ShootingBehavior._get_next_attack_id */
    float rot_speed;                 /* PlayerProjectile.rotation_speed      */
    float stop_delay;                /* Projectile.stop_delay, frames        */
    float delay;                     /* DelayedPlayerProjectile.delay, frames*/
    unsigned short scene_istr;       /* canonical projectile PackedScene istr
                                      * (node-pool identity for the delay)   */
    float slow_rad, slow_amt;        /* second slow hitbox (0 rad = none)    */
    float lifesteal;                 /* WeaponStats.lifesteal                */
    unsigned short src_weapon_id;    /* canonical weapon id, or BT_WEAPON_NONE */
    unsigned int   owner_uid;        /* wpn_uid of the firing copy, or BT_WEAPON_UID_NONE */
    float          src_curse;        /* the copy's curse modifier            */
    unsigned short fx_first;         /* canonical BT_WEAPON_EFF_INDEX start  */
    unsigned char  fx_count;         /* canonical effect-row count           */
    unsigned char  is_elemental;     /* elemental damage flag                */
    unsigned char  flags;            /* BT_WPROJ_F_*                         */
    /* per-top-level-effect curse modifiers (wpn_fxcurse order): one draw per
     * effect at acquisition, so an individual effect's curse survives the copy
     * being recycled.  src_curse (above) is the aggregate/floor only. */
    float          src_fxcurse[BT_WEAPON_FX_CURSE_MAX];
    /* Immutable HITBOX scaling-stat view: player_projectile.gd's shoot_ex
     * receives HitboxArgs built by `set_from_weapon_stats(weapon_stats)`, which
     * copies `scaling_stats` onto the shot's Hitbox, so a live shot keeps the
     * firing weapon's effective scaling keys even after the copy is recycled.
     * Valid ONLY while BT_WPROJ_F_SKEY is set in `flags`; an unset view is never
     * read.  Children overwrite it from their own nested stats row. */
    unsigned char  s_nscaling;       /* number of valid s_key[] entries      */
    unsigned short s_key[BT_WPROJ_MAX_SKEY];  /* BT_ISTR_* stat ids          */
    /* projectiles_on_hit children (ProjectilesOnHitEffect); one level deep. */
    unsigned char onhit_n, onhit_auto;
    BtWProjChild  child;
} BtWProjArgs;

typedef struct {
    float x, y, vx, vy;
    float rot;                  /* Node2D rotation (radians)               */
    float rad, offx, offy;      /* hitbox circle: radius + root-local centre */
    float dmg;                  /* current Hitbox.damage (pierce/bounce decay) */
    float range_timer;          /* _time_until_max_range (seconds)          */
    float speed;                /* velocity magnitude (bounce keeps it)     */
    float crit_chance, crit_dmg;
    float burn_chance, burn_dmg, burn_dur;
    float kb_dx, kb_dy, kb_amt, kb_pierce;   /* Hitbox knockback           */
    float speed_pct;            /* Hitbox.speed_percent_modifier            */
    float slow_rad, slow_amt;   /* second slow hitbox (0 = none)            */
    float rot_speed;            /* PlayerProjectile.rotation_speed (>0 = spin) */
    float stop_delay;           /* Projectile.stop_delay (frames; 0 = instant) */
    float stop_elapsed;         /* _elapsed_delay while stopping            */
    float delay;                /* DelayedPlayerProjectile countdown (frames) */
    float lifesteal;
    int   piercing;
    float pierce_reduce;
    int   bounce;
    float bounce_reduce;
    int   crit_pierce, crit_bounce;
    int   attack_id;
    /* Stable source-object identities, not reusable live actor slots: the
     * sparse ignored/slowed sets (cleared per shot, O(touched)). */
    BtUidPairSet target;
    unsigned short src_weapon_id;   /* canonical weapon id, or BT_WEAPON_NONE */
    unsigned int   owner_uid;       /* wpn_uid, or BT_WEAPON_UID_NONE        */
    float          src_curse;       /* per-copy curse modifier snapshot      */
    float          src_fxcurse[BT_WEAPON_FX_CURSE_MAX]; /* per-effect curse    */
    unsigned short fx_first;        /* canonical BT_WEAPON_EFF_INDEX start   */
    unsigned char  fx_count;        /* canonical effect-row count            */
    unsigned char  is_elemental;
    /* Immutable hitbox scaling keys (valid only while BT_WPROJ_F_SKEY is set in
     * flags), so the pdt gate `find_scaling_stat(row.key, scaling_stats)` sees
     * the view the shot was fired with, never the owner's current one. */
    unsigned char  s_nscaling;
    unsigned short s_key[BT_WPROJ_MAX_SKEY];
    unsigned char onhit_n, onhit_auto, onhit_used;
    float onhit_dmg, onhit_crit_chance, onhit_crit_dmg;
    float onhit_burn_chance, onhit_burn_dmg, onhit_burn_dur;
    float onhit_range, onhit_speed, onhit_rad, onhit_offx, onhit_offy;
    float onhit_pierce_reduce, onhit_bounce_reduce;
    int   onhit_piercing, onhit_bounce;
    unsigned char shape;        /* BT_SHAPE_* (player shots: circle)         */
    unsigned char flags;        /* BT_WPROJ_F_*                              */
    unsigned char ign_overflow; /* invalid/exhausted source-object identity */
    unsigned char armed;        /* 0 on the spawn frame (deferred add)       */
    unsigned char alive;
    /* hit_something event record (damage BEFORE this hit's pierce/bounce). */
    unsigned int  n_hits;
    float         hit_dmg;
    unsigned char hit_crit;
} BtWProj;

#ifndef BT_HITQ_CAP
#define BT_HITQ_CAP 256
#endif
/* The count field must hold BT_HITQ_CAP: an 8-bit `n` would silently wrap (and,
 * with a cap above 255, the enqueue's `n >= cap` test would read a truncated 0
 * and reject EVERY pair).  Keep the two in step. */
#if BT_HITQ_CAP > 65535
#error "BtHitQueue.n is 16-bit; split the pair arrays before raising the cap"
#endif

/* Deferred Hitbox -> Hurtbox hit pairs.
 *
 * The engine reports an Area2D pair in the NEXT frame's
 * Physics2DServer::flush_queries and the victim's own handler is itself
 * call_deferred (unit.gd:610-611), so a hurt's rolls, damage and die() land one
 * frame after the overlap.  Each reported pair is one queued handler, so the
 * queue holds every pair of a frame (several projectiles, and one piercing shot
 * hitting several hurtboxes), drained in report order: the core's projectile
 * phase scans slots in order, which is the serial reference for the physics
 * server's pair order.
 *
 * Entries are validated at the drain: the projectile slot must still hold the
 * same shot (owner uid) and the target slot the same source object (bt_wproj_eid
 * token), so a recycled slot is never hurt.  Over capacity the pair is DROPPED
 * and BT_CAP_HITQ latches: fail-closed and reported, never a silent reorder.
 *
 * BOTH directions use it.  The `np`/`pa` half is the enemy/neutral projectile ->
 * PLAYER pair: `enemy_projectile.gd` extends `Projectile`, so its hitbox is
 * enabled through the same `Hitbox.enable()` (set_deferred) path, and the
 * player's Hurtbox emits the SAME `area_entered` -> `unit.gd:610-611`
 * call_deferred handler an enemy's does (`unit.tscn`'s connection, inherited by
 * player.tscn), so that hit is deferred one frame too and is drained by the same
 * frame's flush (bt_player_hitq_apply).
 *
 * CAPACITY: a pair is produced only for a NEW (shot, target) overlap, because
 * the shot's ignored_objects records every pair it has taken, so the per-frame
 * count is "shots entering a crowd x fresh hurtboxes each".  The DEEP
 * peak-occupancy probe (tests/brotato_state_size.cpp, `hitq_peak`, 13 runs
 * including the waves 40/50/60 endless entries and the long Demon+wand kites,
 * peak enemy occupancy 144) peaks at 60 pairs in one frame -- with that
 * workload's single live shot.  256 carries >4x over it and at least one full
 * BT_MAX crowd of pairs, and costs 12 bytes per pair: the drain walks only the
 * pairs actually queued, so the bound never costs per-frame work. */
typedef struct {
    unsigned short n;                /* player-shot -> enemy Hurtbox pairs      */
    unsigned short np;               /* enemy/neutral shot -> PLAYER pairs      */
    unsigned char overflow;          /* either section refused an entry         */
    short a[BT_HITQ_CAP];            /* player-projectile slot                  */
    unsigned int a_uid[BT_HITQ_CAP]; /* the shot's identity (owner_uid)         */
    short b[BT_HITQ_CAP];            /* enemy slot of the target Hurtbox         */
    unsigned int b_uid[BT_HITQ_CAP]; /* the target's source-object token        */
    /* The PLAYER half: the unit there is a singleton, so the entry is just the
     * shot's slot.  No identity token is needed: the source's own guard is
     * `hitbox.active` (unit.gd:537), which is exactly `pralive` -- a shot that
     * returned to the pool cannot hurt, and a slot the pool has ALREADY reused
     * is that same node instance in the source, so the handler reads the live
     * slot (its damage, its source) exactly as the drain below does. */
    short pa[BT_HITQ_CAP];           /* enemy/neutral projectile slot           */
    /* SHOT creation ordinals (one mint per spawn, monotone per episode).  The
     * simultaneous pairs of one step are resolved by ascending (victim creation,
     * shot creation), the order the bridge's BROTATO_RL_ORDER_HURTS comparator
     * uses: a Godot instance id increases with creation order for both the
     * victim (e_object_uid / bt_wproj_eid) and the hurtbox
     * (wproj.cre / prcre here). */
    unsigned short a_cre[BT_HITQ_CAP];   /* player-shot creation ordinal        */
    unsigned short pa_cre[BT_HITQ_CAP];  /* enemy/neutral shot creation ordinal */
} BtHitQueue;

typedef struct {
    BtWProj p[BT_MAX_WPROJ];
    unsigned short used;       /* highest resident projectile slot + 1 */
    /* Per-scene resident NODE cache (source main._player_projectiles pools a node
     * per projectile scene): cache_res[i] = the slot has been instantiated;
     * cache_scene[i] = its scene istr; cache_delay[i] = its retained member
     * initializer delay; cache_seq[i] = its return order (LIFO pick).  A shot
     * reuses the most recently returned node of its OWN scene, else a
     * never-instantiated slot; another scene's resident node is never taken. */
    unsigned char   cache_res[BT_MAX_WPROJ];
    unsigned short  cache_scene[BT_MAX_WPROJ];
    float           cache_delay[BT_MAX_WPROJ];
    unsigned short  cre[BT_MAX_WPROJ];  /* this shot's creation ordinal        */
    unsigned int    cre_next;  /* monotone spawn ordinal (per episode)          */
    unsigned int    cache_seq[BT_MAX_WPROJ];
    unsigned int    seq_next;  /* free-order ordinal (monotonic, per episode)   */
    unsigned int n_spawned;    /* total slots filled this episode            */
    unsigned int n_overflow;   /* EXPLICIT spawn failures (pool full)        */
    unsigned int n_live;       /* live shots (spawn/return maintained); a step
                                * with n_live==0 does no per-slot scanning     */
} BtWProjPool;
