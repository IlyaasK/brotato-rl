/* Brotato Bull/Danger-5 simulation state and constants - shared core.
 *
 * OWNERSHIP / CONTRACT
 *   BtState is the single per-environment simulation store for the shared
 *   Brotato core. CPU C/C++, CUDA and Metal step the SAME struct through the
 *   same BT_HD functions in brotato_core.h; no backend re-implements the sim
 *   and no backend copies it per step (it is always passed by pointer). It is
 *   a POD: fixed arrays, scalars, a PCG32 state, the numeric log, and the
 *   persistent GodotPhysics contact world. No host pointers, no allocation,
 *   trivially copyable, so a device backend may place one BtState per env
 *   directly in a buffer.
 *
 *   The contact world (brotato_physics.h, owned by ContactSolver) lives inside
 *   BtState by value: it IS the persistent contact cache / warm-started impulse
 *   store, so one BtState per env remains the whole simulation.
 *
 * This header is host-independent: it includes only the portable RNG, the
 * generated numeric tables, the physics geometry and the contact solver, never
 * raylib/pufferenv/stdio/stdlib.
 *
 * SOURCE-PARITY STATUS
 *   The numbers below are resolved from /Users/leelachebiyyam/brotato_recovered
 *   (v1.1.14.6) and brotato_abyss (DLC). Behaviours that the recovered sources
 *   do not pin down, or that are deliberately approximated for training, are
 *   listed in brotato_core.h's header and in the final hand-off report; the
 *   remaining unsupported behaviours are named there rather than claimed.
 */
#pragma once

#include "brotato_rng.h"
#include "brotato_cuda_layout.h"

/* Generated tables. brotato_waves.h / brotato_attacks.h / brotato_items.h /
 * brotato_upgrades.h are all device-portable (BT_TABLE storage + BT_HOST_ONLY
 * names), so every backend includes the same canonical headers. */
#include "brotato_waves.h"
#include "brotato_attacks.h"
#include "brotato_items.h"
#include "brotato_characters.h"
#include "brotato_item_effects.h"
/* Canonical identity metadata (BT_EHE_*) for the enemy_percent_damage_taken
 * effect rows; the runtime is brotato_enemy_hit_effects.h (EnemyHitEffects). */
#include "brotato_enemy_hit_effect_data.h"
#include "brotato_weapons.h"
#include "brotato_upgrades.h"

/* Physics geometry (BT_PHYSICS[kind], BT_PLAYER_PHYSICS) and the GodotPhysics
 * contact solver (BtPhysWorld). Both are POD / BT_HD. */
#include "brotato_geometry.h"
#include "brotato_physics.h"
/* Combat metadata the generated tables do not carry (projectile-shape windows,
 * buffer boost, spawner death-spawn, boss state clock). */
#include "brotato_combat.h"
/* Item/status effect mechanics (caps, stat links). */
#include "brotato_effects.h"

/* Zone unit roster accessor. The generated tables flatten every zone's unit
 * span into BT_ZONE_UNITS_ALL + BT_ZONE_UNITS_OFF, so one accessor works on
 * host and device (the host-only pointer array is not used here). */
#define BT_ZONE_UNIT(zone, idx) (&BT_ZONE_UNITS_ALL[BT_ZONE_UNITS_OFF[(zone)] + (idx)])

/* attack_behaviors: 0 none, 1 shooting, 2 charging, 3 spawning (extract_gameplay.py). */
#define BT_BEH_NONE   0
#define BT_BEH_SHOOT  1
#define BT_BEH_CHARGE 2
#define BT_BEH_SPAWN  3

/* --- entity_spawner.gd queue + entity_birth.gd lifecycle ----------------
 * global/entity_spawner.gd buffers every group/summon/death-spawn into
 * category queues and drains them on ONE pass every SPAWN_DELAY physics
 * frames; each drained entry becomes an EntityBirth that counts down
 * time_before_spawn = 60 frames before the entity is activated. */
#define BT_SPAWN_DELAY  3      /* entity_spawner.gd:14 SPAWN_DELAY              */
#define BT_BIRTH_FRAMES 60     /* entity_birth.gd:8 time_before_spawn          */
#define BT_BIRTH_RADIUS 72.0f  /* entity_birth.tscn CircleShape2D radius       */
#define BT_QUEUE_LIMIT  100    /* entity_spawner.gd:15 QUEUE_LIMIT             */

/* POD queue/birth bounds. The source Arrays are unbounded; the largest
 * same-spawn_timing fire batch over the shipped BT_GROUPS tables (sum of every
 * unit's max_number for the groups that share a timing) is under 300, so
 * BT_MAX_PENDING can never reject a real enqueue. A rejected push sets
 * pend_overflow (explicit failure, never a silent drop). Concurrent births are
 * bounded by (BT_BIRTH_FRAMES / BT_SPAWN_DELAY) = 20 open polls x (2 ordinary
 * + 1 tree + 1 boss + 1 summon + 1 structure + 1 pet) = 140, doubled for the
 * player-overlap restart. */
/* Bound for the shop item-box copy rings and the pending-item-kind FIFO.  The
 * five shallow SPAWN queues use BT_MAX_PEND_SMALL and the deep ordinary-enemy
 * queue BT_MAX_PEND_ENEMY (both below). */
#define BT_MAX_PENDING 512
/* The ORDINARY-ENEMY queue is the one queue that genuinely overruns
 * BT_MAX_PENDING: the DEEP endless probe (Demon, endless, EVERY item granted,
 * 6 runs, tests/brotato_state_size.cpp) peaks at 522 queued enemies at the
 * BT_MAX=192 bound with a 4096-slot probe queue (616 at BT_MAX=100), while the
 * other five category queues peak at <= 39 (structures) and <= 29 (pets).
 * Sized 2048 with a loud pend_overflow. */
#define BT_MAX_PEND_ENEMY 2048
/* The five OTHER category queues are one-shot-per-event and never deep: with a
 * 4096-slot probe queue the DEEP endless probe (wave 554, every item) peaks at
 * <= 39 (structures) and <= 29 (pets), with trees/bosses/summons smaller still,
 * so 256 keeps >= 6x.  Loud pend_overflow + the per-queue drop counters. */
#define BT_MAX_PEND_SMALL 256
#define BT_MAX_BIRTHS  256

/* --- source enemy-instance pool (main.gd `_pool`) ------------------------
 * entity_spawner.gd:346-420 either pops a dead enemy node from the per-scene
 * pool and calls respawn(), or builds a fresh node with scene.instance() and
 * init(). A dead node reaches the pool from Entity.death_animation_finished,
 * the method key of entity.tscn's `death` animation at t=1.0 s (Entity.die
 * forces playback_speed = 1), i.e. exactly 1.0 s = 60 frames at the project's
 * 60 Hz physics tick. Bosses/elites (BT_KIND_CLASS != 0) queue_free instead
 * (boss.gd:81) and never pool. See brotato_enemy_pool.h. */
#define BT_POOL_DEATH_FRAMES 60

/* A death the engine registers from its IDLE pass stays observable ONE frame
 * longer than a physics-pass kill.  entity.gd:88-100's death animation is
 * advanced once per IDLE frame (AnimationPlayer playback_process_mode IDLE is
 * the engine default, animation_player.cpp:1709) and the frame the die() ran in
 * counts too whenever the die() ran BEFORE that frame's idle pass -- i.e. for
 * every weapon/contact hit, whose call_deferred("die") is flushed at the end of
 * SceneTree::iteration's physics pass (scene_tree.cpp:609), which precedes
 * Main::iteration's idle pass (main.cpp:2394 then 2424).  main.gd's
 * clean_up_room (main.gd:1232) instead runs from _on_WaveTimer_timeout, i.e.
 * from the WaveTimer's `timeout` inside that same IDLE pass; SceneTree walks a
 * process group in scene-tree order (scene_tree.cpp:219-237 sorts by
 * Node::is_greater_than, node.cpp:1574-1627), and main.tscn declares WaveTimer
 * (line 877) AFTER Entities (line 244), so every enemy's AnimationPlayer slot
 * was already visited: that frame's advance is missed and the node leaves the
 * tree one idle frame later.  bt_enemy_clean_up_room adds this to edying. */
#define BT_POOL_DEATH_IDLE_EXTRA 1

/* Structure.gd:9 PENDING_DIE_DURATION = 3.0 s: Structure.deferred_die() leaves
 * `structures` at once (it emits `died`, structure.gd:72-76) but only plays the
 * death animation 3.0 s later, so its node reaches the pool
 * BT_POOL_PENDING_FRAMES + BT_POOL_DEATH_FRAMES ticks after the eviction. */
#define BT_POOL_PENDING_FRAMES 180

/* Live structure/pet entities (entity_spawner.gd:10 MAX_STRUCTURES) and owned
 * structure/pet EFFECT instances (Keys.structures_hash + Keys.stat_pets_hash).
 * The source Arrays are unbounded; MAX_STRUCTURES itself evicts random
 * structures when a structure birth would exceed it, so 100 is a real source
 * bound for structures. Owned instances are bounded by the number of items a
 * run can acquire; a rejected registration sets summon_overflow (explicit). */
#define BT_MAX_STRUCTS      100
/* Registered-but-not-shown structure/pet summon rows.  The DEEP endless probe
 * peaks at 61 with a 1024-slot probe table (64 saturated), so 128 carries ~2x. */
#define BT_MAX_SUMMON_FX     128

/* One queued spawn: the source stores [type, scene, pos, data, source,
 * charmed_by]; kind + neutral (tree) + is_boss reproduce the unit identity and
 * EntityType the lifecycle branches on. For the structure/pet queues `kind` is
 * the summon_fx[] instance index (0xFFFF = none) and `row` the BT_ITEM_EFF_ROWS
 * row to instantiate; for the enemy queues `kind` is the unit kind and `row` 0.
 * `src` is the summoning enemy's live slot (enemy.gd:334 source_spawner), -1 for
 * a queue spawn with no source (wave groups, spawner death children). */
typedef struct { unsigned short kind; unsigned short row; unsigned char neutral, is_boss; short src, charmed_by; float x, y; } BtPending;
/* One active EntityBirth: kind/neutral/is_boss + position + the frame countdown.
 * `bclass` is the EntityType branch entity_birth.gd takes: 0 = ENEMY/BOSS/
 * NEUTRAL (player-overlap restart, enemy/neutral spawn), 1 = STRUCTURE, 2 = PET
 * (never restarted; activates through bt_spawn_summon_row). For 1/2 `row` is the
 * effect row and `kind` the summon_fx[] instance (0xFFFF = runtime spawn). */
typedef struct { unsigned short kind; unsigned short row; unsigned char neutral, is_boss, active, bclass; short src, charmed_by; float x, y, timer; } BtBirth;

/* One owned structure/pet effect INSTANCE. The source appends one instance per
 * item acquisition to Keys.structures_hash (StructureEffect/TurretEffect/
 * BuilderTurretEffect) or Keys.stat_pets_hash (PetEffect subclasses with a
 * scene) and instantiates NOTHING then: global/entity_spawner.gd
 * _on_StructureTimer_timeout creates `copies` entities on the wave's first
 * timer tick and again whenever cur_time % spawn_cooldown == 0. `mod` is the
 * per-effect curse modifier dlc_1_data.curse_item drew for this instance, and
 * the four non-stat fields are that instance's curse-transformed values
 * (doc_moth/lootworm/scapegoat); `spawn_cd` is the curse-transformed
 * StructureEffect.spawn_cooldown (or a BotOMineEffect's landmine cooldown). */
typedef struct {
    unsigned short row;      /* BT_ITEM_EFF_ROWS index of the applied effect */
    unsigned char opcode;    /* BT_ITEMEFF_* of that row                       */
    unsigned char is_pet;    /* 1 = Keys.stat_pets_hash, 0 = structures_hash   */
    unsigned char is_turret; /* TurretEffect/BuilderTurretEffect (ordering)    */
    unsigned char cursed;    /* dlc_1_data curse marked this instance          */
    unsigned char copies;    /* Effect.value (entities per base spawn)         */
    int           spawn_cd;  /* curse-transformed spawn cooldown (0 = once)    */
    float         mod;       /* curse modifier for this instance (0 = none)    */
    float         boost_zone_scale; /* DocMothEffect.boost_zone_scale           */
    float         double_chance;    /* LootwormEffect.double_chance             */
    float         revive_duration;  /* ScapegoatEffect.revive_duration          */
    float         health_boost;     /* ScapegoatEffect.health_boost             */
} BtSummonFx;

/* One owned VARIANT of a canonical effect row. The source applies one effect
 * INSTANCE per owned copy and each instance carries its own (possibly curse-
 * modified) value/value2, so a row can be owned many times with different
 * tuples. Distinct tuples are kept separately with their multiplicity; exact
 * duplicates merge. Overflow is an explicit flag (never a silent drop). */
/* Curse percent is 40 + 2*min(wave-1,20) + randi_range(-30,30):
 * 101 possible cursed percentages (10..110), plus the uncursed value. */
#define BT_MAX_VARIANTS 102
typedef struct { int value, value2; unsigned short count; } BtRowVariant;
/* Ragged variant pool: first run allocated per row, then doubled up to
 * BT_MAX_VARIANTS.  Exhaustion sets curse_overflow (loud, never a silent drop).
 * Sized from the measured peak: the DEEP probe in tests/brotato_state_size.cpp
 * (Demon, endless, EVERY item granted -- the worst case for variant rows, since
 * variants are per owned ITEM row at BT_NITEM_EFF_ROWS=903, not per enemy)
 * peaks at 62..68 live variants, so 1024 entries carries ~15x headroom. */
#ifndef BT_RVAR_POOL
#define BT_RVAR_POOL 1024
#endif
#define BT_RVAR_MIN_CAP 4
/* Acquisition-order values for the three consumable shop items. */
typedef struct { int price, curse, hp_start, modifier; } BtShopItemCopy;

/* Core stat ids: 0..12 unchanged, 13 = DLC curse, 14..18 the five stats the
 * source upgrade pool also offers (accuracy, attack speed, engineering,
 * lifesteal, range).  See bt_core_stat_read/bt_core_stat_add. */
#define BT_NSTATS 19

/* enemy_charge_prep_animation duration, in seconds. */
#define BT_CHARGE_WINDUP 0.4f

/* Per-unit geometry lookup with a bounds guard (rows and BT_KINDS order match). */
#if !defined(BT_GEOM_NKINDS)
#define BT_GEOM_NKINDS BT_PHYSICS_NKINDS
#endif
BT_HD static inline int bt_geom_index(int kind) {
    if (kind < 0) return 0;
    if (kind >= BT_GEOM_NKINDS) return BT_GEOM_NKINDS - 1;
    return kind;
}

#define BT_WAVES 20

/* Endless elite-reschedule buffer depth (see BtState.oe_*): two live decades of
 * BT_ELITE_EVENTS events (the current window plus the one scheduled a decade
 * ahead). */
#define BT_ENDLESS_ELITE_SLOTS (2 * BT_ELITE_EVENTS)

/* Enemy slot pool.  The source has no fixed pool: it caps the PLAIN enemy list
 * at entity_spawner.gd:152 max_enemies (100 authored, 135..144 for the
 * generated endless waves via zone_service.gd:48) and keeps bosses/elites/
 * neutrals in separate lists.  100 therefore saturates as soon as an elite or
 * boss is alive (DemonBaseline measures live enemies pinned at BT_MAX on deep
 * runs), so every list is now held at once: 144 plain + 12 transient group
 * overshoot + 2 bosses (difficulty_5_effect_4 double_boss) + 6 trees leaves
 * BT_MAX - 164 = 28 slots for concurrently LIVE elites.
 *
 * SUPPORTED RANGE (explicit, not "unbounded"): the elite demand is
 *   per endless elite event: 1 + bt_endless_extra_elites(w)
 *                             = 1 + ceil((w - 20) / 10)
 * (run_data.gd:785-788), so a single event at a time -- the usual case, one
 * scheduled event per window -- is exact through wave 290 (1+27 = 28) and
 * overflows from wave 300 on (1+28 = 29).  Several events landing on the same
 * wave, or the wave-10k boss+elite set, lower that ceiling proportionally
 * (6 events at one wave are exact only to wave ~60).  Beyond the ceiling the
 * sim must NOT clamp a boss/elite: bt_reserve_slot always yields its slot and
 * evicts a PLAIN enemy instead.  That eviction is loud and split by cause --
 * BtState.pool_cull_fallback when a plain/neutral spawn did it (the source's
 * cull shape), BtState.elite_slot_pressure when a BOSS/ELITE spawn did (the
 * source has no slot ceiling, so this is pure implementation-capacity loss, with
 * its own BT_CAP bit).  Exact spill for those runs belongs to the
 * implementation-capacity layer (brotato_capacity.h: bt_capacity_mask /
 * bt_capacity_require_clean), not to a bigger inline array.
 * The GENERATED-GROUP side has the same shape: nb = int(w * 0.2) grows without
 * bound, the generated copies keep landing until the WHOLE live list is full
 * (BT_MAX_LIVE_GROUPS), and every group past that cap is dropped with
 * BtState.group_overflow set -- loud, never a silent clamp (see
 * brotato_waves.h).
 * bt_group_perf_cull below is the source's own cap enforcement (plain list
 * only, drawn once per group event), so a legal run never reaches the
 * pool-full fallback; brotato_physics.h's BT_PHYS_MAX_BODIES /
 * BT_PHYS_PAIR_CAPACITY track this bound (BT_MAX + 4 arena bodies). */
#ifndef BT_MAX
#define BT_MAX 192
#endif
/* brotato_physics.h cannot see BT_MAX (it is included above), so it carries the
 * same bound as a literal; fail loudly if the two ever drift. */
#if BT_PHYS_MAX_BODIES < (BT_MAX + 4)
#error "brotato_physics.h BT_PHYS_MAX_BODIES must hold BT_MAX enemies + 4 arena walls"
#endif
/* The live-enemy-count OBSERVATION feature keeps its historical 1/100 scale
 * (the authored cap), NOT 1/BT_MAX, so a later pool-bound raise cannot silently
 * rescale it; today the two are bit-identical. */
#define BT_ENEMY_COUNT_SCALE (1.0f / 100.0f)
/* BT_MAX target bits per healer for the BoostZone membership (healer.gd heals
 * on body_entered, so the core retains the overlap set to detect an entry). */
#define BT_HEAL_BYTES ((BT_MAX + 7) / 8)
#define BT_SLOTS 6
/* Discrete one-head action space. Ids are PHASE-FACTORED (the same id means
 * different things in the combat / shop / level-up / crate contexts) and the
 * env publishes a per-phase mask through Agent.action_mask (bt_action_mask
 * below); the trainer masks the logits with it. 0..20 keep their v3 meaning so
 * every recorded trace stays replayable.
 *
 *   combat (phase 0):   0 hold, 1..8 N,NE,E,SE,S,SW,W,NW
 *   shop (phase 1):     0..3 buy offer i, 4 reroll, 8 go to the next wave,
 *                       9..14 recycle owned weapon slot (id-9),
 *                       15..20 combine owned weapon slot (id-15),
 *                       21..24 toggle lock on offer (id-21),
 *                       25..28 ban offer (id-25)
 *   level-up (phase 2): 0..3 choose offer i, 4 reroll the offers
 *   crate (phase 2 with an open item box): 29 take the crate item,
 *                       30 recycle the crate item
 * Masked ids must be ignored by the core as well as by the policy. */
#define BT_ACTIONS 31
#define BT_ACT_HOLD 0
#define BT_ACT_MOVE_N 1
#define BT_ACT_MOVE_COUNT 9        /* combat ids 1..8 */
#define BT_ACT_BUY0 0              /* shop/level-up ids 0..3 */
#define BT_ACT_REROLL 4            /* shop and level-up reroll */
#define BT_ACT_SKIP 8              /* shop: go to the next wave */
#define BT_ACT_RECYCLE0 9          /* shop ids 9..14: recycle weapon slot id-9 */
#define BT_ACT_COMBINE0 15         /* shop ids 15..20: combine weapon slot id-15 */
#define BT_ACT_LOCK0 21            /* shop ids 21..24: toggle lock on offer id-21 */
#define BT_ACT_BAN0 25             /* shop ids 25..28: ban offer id-25 */
#define BT_ACT_CRATE_TAKE 29
#define BT_ACT_CRATE_RECYCLE 30
#define BT_SHOP_NONE 0
/* shop_items_container.gd:27-28 `_buy_delay_timer.wait_time = 0.05` -> the
 * number of 60 Hz frames Godot's Timer needs to reach its timeout
 * (bt_timer_frames(0.05)): the shop refuses buys/steals for this many ticks
 * after a completed purchase, including the purchase tick itself. */
#define BT_SHOP_BUY_DELAY_TICKS 4
#define BT_SHOP_ITEM 1
#define BT_SHOP_WEAPON 2
#define BT_WEAPON_START_MAX 8
#define BT_WEAPON_UID_NONE 0xFFFFFFFFu
#include "brotato_weapon_effect_types.h"

/* Enemy-instance pool bounds. A poolable instance is created (fresh) only while
 * its kind's pool is empty, so the instances alive at once are the BT_MAX live
 * slots plus every death still inside its BT_POOL_DEATH_FRAMES animation. With
 * BT_MAX deaths admitted per frame the worst case is BT_MAX * (DELAY + 1)
 * instances; that is a single shared id space (no per-kind multiplication). */
#define BT_POOL_INST_MAX (BT_MAX * (BT_POOL_DEATH_FRAMES + 1))
#define BT_POOL_NO_INST  0xFFFFu   /* e_instance / free-head sentinel */
/* STORAGE bound for the per-instance-id arrays.  BT_POOL_INST_MAX is the
 * SOURCE-side id space (theoretical worst case: BT_MAX deaths in every frame of
 * the 60-frame death animation).  Real runs allocate far fewer ids: the DEEP
 * probe in tests/brotato_state_size.cpp (Demon, endless, every item granted,
 * wave 69, BT_MAX=100) peaks at 261 allocated instance ids -- about 2.6 ids per
 * live enemy slot, not 61.  The per-id arrays (pool_next / pool_kind /
 * pool_shots / pool_mv_state / pool_lungfull /
 * pool_object_uid and the per-instance movement-node state pool_mv*) are
 * dimensioned BT_POOL_ID_CAP instead, with the same >=4x margin the physics pair
 * pool carries (261 measured at BT_MAX=100 -> ~500 projected at the BT_MAX=192
 * bound).  An allocation past the cap is REJECTED and sets pool_id_overflow --
 * LOUD, never a silent drop, and never a write outside the array (acquire
 * returns BT_POOL_NO_INST, which every caller already treats as "no instance"). */
#ifndef BT_POOL_ID_CAP
#define BT_POOL_ID_CAP 1536
#endif
/* Source-object UID space for one wave: every pool descriptor (BT_POOL_INST_MAX
 * ordinary pooled nodes) plus a fresh uid for each of the BT_MAX resident slots
 * that can host a fresh non-pooled node (a boss/elite descriptor recycles its
 * pool id; neutrals use their own retained pool).  Uid 0 is the sentinel ("no
 * identity"), so uids run 1..BT_OBJECT_UID_MAX and a projectile's ignored bitset
 * needs ((BT_OBJECT_UID_MAX + 32) / 32) words.  Minting past the bound sets
 * object_uid_overflow and yields 0 -- never an aliased or slot-fallback id. */
#define BT_OBJECT_UID_MAX (BT_POOL_INST_MAX + BT_MAX)
#if BT_OBJECT_UID_MAX > 65534
#error "object uids no longer fit the 16-bit per-slot uid stores (e_object_uid / pool_object_uid / ndly_uid / neutral_uid_pool)"
#endif
#include "brotato_weapon_projectile_types.h"


#define BT_ARENA_W 2048.0f   /* zone_1_data.tres 32 tiles * Utils.TILE_SIZE 64 */
#define BT_ARENA_H 1536.0f   /* 24 tiles * 64 */

/* Godot's default physics tick is 60 Hz and project.godot does not override it,
 * so the env steps at the same rate: every timed constant (i-frames, attack
 * cooldowns, regen period, wave durations) lands on the same frame count. */
#define BT_DT (1.0f / 60.0f)

#define BT_BASE_HP 10.0f     /* PlayerRunData.DEFAULT_MAX_HP */
#define BT_BULL_HP 20.0f     /* bull_effect_1: stat_max_hp +20        -> 30 total */
#define BT_BULL_REGEN 15.0f  /* bull_effect_2: stat_hp_regeneration +15 */
#define BT_REGEN_GAIN 1.5f   /* bull_effect_3b: gain_stat_hp_regeneration +50% */
#define BT_BULL_ARMOR 10.0f  /* bull_effect_3: stat_armor +10 */
/* player_run_data.gd set_player_count (run_data.gd:275-284) sets gold =
 * DebugService.starting_gold (debug_service.gd:6 = 30). */
#define BT_START_MATERIALS 30.0f
#define BT_PLAYER_SPEED 450.0f  /* player_stats.tres speed = 450 */
#define BT_DANGER_HP 1.40f      /* difficulty_5_effect_2 */
#define BT_DANGER_DMG 1.40f     /* difficulty_5_effect_5 */

/* bull_explosion_stats.tres */
#define BT_EXPLODE_R 147.34f    /* explosion.tscn Hitbox CircleShape2D radius */
#define BT_EXPLODE_OFF_X 0.0f
#define BT_EXPLODE_OFF_Y 3.0f
#define BT_EXPLODE_DMG 30.0f
#define BT_EXPLODE_SCALE_STAT 3.0f    /* scaling_stats: stat_*_damage x3 */
#define BT_BASE_CRIT 0.03f           /* bull_explosion_stats crit_chance */
#define BT_BASE_CRIT_DMG 1.5f        /* bull_explosion_stats crit_damage */

/* Player geometry (player.tscn) */
#define BT_PLAYER_BODY_R 24.0f
#define BT_PLAYER_BODY_Y (-12.0f)
#define BT_PLAYER_HURT_R 21.0f
#define BT_PLAYER_HURT_Y (-12.0f)

/* Collision layers (project.godot names) */
#define BT_LAYER_PLAYER 2u
#define BT_MASK_PLAYER 130u   /* 128 walls + 2 player */
#define BT_LAYER_ENEMY 4u
#define BT_MASK_ENEMY 132u    /* 128 walls + 4 enemy */
#define BT_LAYER_WALL 128u
#define BT_MASK_WALL 0u

/* Live enemy/pet projectiles (the `pr*` pool). Sized from the measured endless
 * peak: 27 simultaneous live projectiles at 100 live enemies (waves 40-60,
 * Demon, tests/brotato_state_size.cpp occupancy mode with a deep-wave jump) --
 * see the run in the commit message.  A spawn with every slot live sets
 * proj_overflow and increments proj_dropped -- never a silent drop. */
/* Live enemy projectiles (bt_spawn_projectile).  The source has NO cap and 64
 * saturated from wave ~34.  The DEEP endless probe with EVERY item granted peaks
 * at 123 live at BT_MAX=100 and 344 at the BT_MAX=192 bound (2048-slot probe
 * pool), so 1024 carries ~3x; pr_overflow stays the loud guard. */
#ifndef BT_MAX_PROJ
#define BT_MAX_PROJ 1024
#endif
/* Live consumable pickups (item boxes / garden fruit / kill drops).  The source
 * has NO cap (brotato_loot.h: "boxes accumulate across a wave as pickups free
 * consumable slots"); 16 saturates from wave ~9 and was the real pend_overflow
 * writer (60 dropped consumables in the every-item load).  The DEEP endless
 * probe peaks at 113 live with a 4096-slot probe table, so 256 carries ~2x. */
#ifndef BT_MAX_CONS
#define BT_MAX_CONS 256
#endif
/* Materials are entities (items/materials/gold.gd + items/global/item.gd), not
 * auto-collected: killed enemies drop gold that must be walked over.
 * player.tscn ItemPickupArea/CollisionShape2D radius 32 @ (0,-24); the
 * ItemAttractArea (magnet) CollisionShape2D has NO position, so at radius 150 it
 * is centred on the player ROOT, and an attracted item homes to
 * attracted_by.global_position (the player root), NOT the pickup offset;
 * item.gd INITIAL_ATTRACT_SPEED 500, ATTRACT_ACCELERATION 20 per frame. */
#define BT_MAX_MAT 64
/* Live structure+pet entities. The source keeps `structures` (capped at
 * MAX_STRUCTURES = 100) and `pets` in separate Arrays; both live here, each
 * slot tagged by pet_isstruct, and the MAX_STRUCTURES eviction only counts the
 * structure slots still inside `structures` (entity_spawner.gd:292-303) -- a
 * Structure.deferred_die() node leaves that Array at once while its node lives
 * on, see pet_pool_pending below. 128 covers 100 structures plus the owned pet
 * copies plus the pending-death ones; an overflow sets summon_overflow
 * (explicit). */
/* ...192 covers the 100-structure MAX_STRUCTURES source cap plus the owned pet
 * copies: the DEEP endless probe peaks at 104 structures + 29 pets = 133 live
 * (128 set summon_overflow).  The pet physics world indexes
 * bodies[BT_PET_BODY_BASE + slot], so BT_PHYS_MAX_BODIES must cover
 * BT_PET_BODY_BASE + BT_MAX_PET. */
#ifndef BT_MAX_PET
#define BT_MAX_PET 192
#endif

#define BT_EXPL_FRAMES 3
/* Live explosion nodes.  DECOUPLED from BT_MAX_PROJ + BT_MAX_PET: the source
 * pools one explosion node per spawning scene (not one per live projectile), and
 * the DEEP endless probe with every item granted peaks at 132 live explosions
 * (2048-slot probe pool).  256 is ~2x that; the loud expl.n_overflow guards the
 * (source-unbounded) pool. */
#define BT_MAX_EXPL 256
#if BT_MAX > 65535 || BT_MAX_PROJ > 65535 || BT_MAX_CONS > 65535 || \
    BT_MAX_MAT > 65535 || BT_MAX_WPROJ > 65535 || BT_MAX_EXPL > 65535
#error "Brotato pool used bounds require unsigned short capacities"
#endif
#define BT_EXPL_HIT_WORDS ((BT_OBJECT_UID_MAX + 32) / 32)
typedef struct {
    float x, y, ox, oy, rad, dmg, crit_chance, crit_dmg;
    float burn_chance, burn_dmg, burn_dur, scale;
    int age;
    BtUidSet hit;               /* sparse identity set (see BtUidSet) */
    unsigned char hit_overflow; /* a target exceeded the set capacity (loud) */
    unsigned char alive;
    /* Firing-weapon attribution (unit.gd:569: the weapon connects the spawned
     * explosion's hit_something/killed_something to itself, so an explosion kill
     * is credited to the weapon's on_killed_something/on_weapon_hit_something).
     * Zeroed for every non-weapon source (item explosion, landmine, tree turret,
     * character) -> has_src == 0 -> no weapon credit.  Set by
     * bt_item_expl_set_source from the weapon-projectile spawn. */
    unsigned int src_uid;
    unsigned short src_fx_first, src_fx_count, src_slot;
    unsigned char has_src;
} BtExplosion;
typedef struct {
    BtExplosion e[BT_MAX_EXPL];
    unsigned int n_spawned, n_overflow, n_missing;
    unsigned short used;       /* highest resident explosion slot + 1 */
    /* Scene-keyed resident explosion nodes; return order selects the LIFO head. */
    unsigned char cache_res[BT_MAX_EXPL], cache_row[BT_MAX_EXPL];
    unsigned int cache_seq[BT_MAX_EXPL], seq_next;
    unsigned char snd_pending[BT_MAX_EXPL];
    /* One attempt per owned below-HP effect copy, re-armed with each new player.
     * The shipped producer is sunken_bell (max_nb=1); canonical rows bound
     * storage conservatively without inventing a second item-copy namespace. */
    unsigned char expl_trig[BT_NITEM_EFF_ROWS], expl_trig_overflow;
} BtItemExplosions;

/* Per-enemy enemy_percent_damage_taken debuff records
 * (percent_damage_taken_enemy_effect_behavior.gd; runtime +
 * semantics in brotato_enemy_hit_effects.h, owner EnemyHitEffects).
 * One record per distinct effect source_id (the canonical interned aux_key),
 * holding the source's active sub-state: _percent_damage_taken sums into
 * `total`, each record keeps the source's own `added` total so expiry removes
 * only that contribution. `proc` is the source's _effects_proc_count: it
 * RESTARTS at 1 when a record's effect is (re)created and it persists while
 * the record lives (cleared per life with the record, like the source's fresh
 * behavior node on Enemy.respawn).
 * Bound: one record per canonical distinct source (BT_EHE_NSOURCES = ice_cube
 * + lute); a new source with no free record sets `overflow` (explicit, never a
 * silent drop). `dur`/`timer` are FRAMES.  The clock is the MEASURED engine
 * behavior, not an inference: a pinned Godot 3.7 run at --fixed-fps 60 reports
 * _process(delta) delta = 0.016667 (binary32-wide, not an exact 1/60 binary64),
 * so 3 s expires on frame 180 with a NEGATIVE residual -- i.e. exactly
 * duration_secs * 60 frames for the duration_secs the shipped rows carry. */
#define BT_NEH_MAX_ACTIVE BT_EHE_NSOURCES
typedef struct {
    short          total[BT_MAX];      /* _percent_damage_taken, per live slot */
    unsigned char  n_rec[BT_MAX];
    unsigned short src[BT_MAX][BT_NEH_MAX_ACTIVE];       /* source_id (aux_key)   */
    unsigned char  active[BT_MAX][BT_NEH_MAX_ACTIVE];
    unsigned char  stacks[BT_MAX][BT_NEH_MAX_ACTIVE];
    unsigned char  maxstacks[BT_MAX][BT_NEH_MAX_ACTIVE];
    short          pct[BT_MAX][BT_NEH_MAX_ACTIVE];       /* percent per stack     */
    short          added[BT_MAX][BT_NEH_MAX_ACTIVE];     /* own added total       */
    unsigned short timer[BT_MAX][BT_NEH_MAX_ACTIVE];     /* frames left           */
    unsigned short dur[BT_MAX][BT_NEH_MAX_ACTIVE];       /* duration in frames    */
    unsigned char  proc[BT_MAX][BT_NEH_MAX_ACTIVE];      /* _effects_proc_count   */
    unsigned char  overflow;
} BtEnemyHitFx;

#define BT_PICKUP_R 32.0f
#define BT_ATTRACT_R 150.0f
/* player.tscn: ItemAttractArea/CollisionShape2D has NO position offset (the
 * magnet circle is centred on the player root), while
 * ItemPickupArea/CollisionShape2D sits at (0,-24).  An attracted item homes to
 * attracted_by.global_position == the player root, so BT_ATTRACT_Y is 0. */
#define BT_ATTRACT_Y 0.0f        /* ItemAttractArea centre offset (player root) */
#define BT_PICKUP_Y (-24.0f)     /* ItemPickupArea circle offset                */
#define BT_ATTRACT_SPEED 500.0f
#define BT_ATTRACT_ACCEL 20.0f
#define BT_HEAL 3.0f             /* fruit_effect value */
#define BT_LEGENDARY_HEAL 100.0f /* legendary_item_box_effect value */
#define BT_PICKUP_R2 (22.0f * 22.0f)
#define BT_PROJ_HIT_R2 (17.0294f * 17.0294f)  /* enemy_projectile radius */
#define BT_SPAWN_MIN_DIST 300.0f /* entity_spawner INITIAL_MIN_DIST_FROM_PLAYER */
#define BT_TILE 64.0f
#define BT_EDGE_MAP_DIST 64.0f   /* Utils.EDGE_MAP_DIST */
#define BT_DIST_TO_EDGE_MAX 0.2f /* ZoneService.DIST_TO_EDGE_MAX_FACTOR */

/* reward weights: TRAINING SHAPING, deliberately separate from source physics */
#define R_KILL 1.0f
#define R_HERD 0.15f
#define R_DMG (-0.08f)
#define R_REGEN 0.002f
#define R_WAVE 8.0f
#define R_WIN 50.0f
#define R_DEATH (-5.0f)

/* ---- potential-based reward shaping (ocean/brotato/brotato_reward.h) --------
 * Ng, Harada & Russell (1999): adding F(s,s') = gamma*Phi(s') - Phi(s) to a
 * reward stream leaves the optimal policy unchanged for ANY Phi.  These terms
 * therefore only redistribute credit over an episode; they never redefine the
 * task, which stays "reach the deepest wave".  Phi is a linear map over
 * guide-relevant progress groups plus per-item counts, configured from the
 * [env] ini section by bt_reward_cfg_init(); every weight defaults to 0, so
 * with an empty config the shaping term is exactly 0.0f and the reward stream
 * is bit-identical to the unshaped one.  BtReward is POD and sits at the END
 * of BtState so CUDA/Metal layouts only grow. */
#define BT_RW_NTERMS 6      /* Phi groups; each has its own Log field */
#define BT_RW_NITEMS 16     /* key-item counts (table in brotato_reward.h) */
#define BT_RW_NPIVOT 3      /* Pruner / Potato Thrower / Excalibur */
struct BtReward {
    float gamma;            /* MUST equal the trainer's [train] gamma */
    float hp;               /* sqrt (log-shaped) max-HP progress */
    float harvest;          /* stat_harvesting */
    float luck;             /* stat_luck */
    float materials;        /* banked materials, only from `late_wave` on */
    float items;            /* fallback weight for every BT_RW_ITEM_IDX row */
    float pivot;            /* owned pivot-weapon credit, weighted by tier */
    float wave_scale;       /* 0 = off; extra R_WAVE multiplier past the campaign */
    float item_w[BT_RW_NITEMS];
    unsigned short pivot_family[BT_RW_NPIVOT];  /* BT_WEAPONS family row, 0xFFFF = unset */
    int late_wave;          /* wave the materials term starts paying from */
    int on;                 /* 0 = shaping disabled (the default) */
};

/* Ragged per-instance init-value pool (see pool_val[] in BtState).  A POOLABLE kind's id gets
 * one run of BT_MAX_KIND_INIT_OPS binary64 words -- the DENSE table's stride,
 * so the bump pool places id `n`'s run at exactly 9*n and a recycled descriptor
 * inherits exactly the bits the dense table would have kept.  The placement has
 * to match word for word: a reuse op may read a row its current life never
 * wrote, and the dense table served the PREVIOUS wave's bits there.
 * Sized from the measured peak: the DEEP probe in tests/brotato_state_size.cpp
 * (Demon, endless, EVERY item granted, wave 113) allocates 386 instance ids,
 * i.e. 3474 words, so 18432 words (= 9 x the 2048-id cap) carries ~5x headroom
 * at BT_MAX=100 and ~2.5x at the BT_MAX=192 bound.  The budget is exactly the
 * id cap's worth of runs, so pool_val_overflow is unreachable by construction
 * while BT_POOL_ID_CAP still admits an id.  BT_POOL_VAL_SCRATCH absorbs the
 * defensive scratch write so it can never leave the array. */
#ifndef BT_POOL_VAL_WORDS
#define BT_POOL_VAL_WORDS 13824
#endif
#define BT_POOL_VAL_SCRATCH BT_MAX_KIND_INIT_OPS

/* Numeric episode log. Kept first in BtState so the host Env can alias it. */
struct Log {
    float perf;
    float score;
    float episode_return;
    float episode_length;
    float n;
    /* Endless mode: the highest wave this run reached (0 in a campaign run).
     * bt_core_add_log latches BtState.endless_max_wave here so the harness can
     * report "how far into endless" a policy got; perf = s->wave keeps rising
     * past 20 for an endless run. */
    float endless_wave;
    /* Shaping ledger: `rw_total` is the episode's summed F, `rw[i]` the summed
     * contribution of Phi group i (order fixed by brotato_reward.h).  They are
     * plain floats so the harness's (const float*)log aggregation covers them. */
    float rw_total;
    float rw[BT_RW_NTERMS];
};

/* gain_stat_for_every_stat_effect mechanics live in brotato_item_effects.h:
 * BT_ITEM_STAT_LINKS[BT_NITEM_STAT_LINKS] (15 rows, item/target/source/value/nb
 * with nb = -1 meaning per-unit). The core iterates that table directly; the
 * old hardcoded 3-row subset is gone (its indices were invalid after the
 * 247-item reindex). */

/* ---------------------------------------------------------------------------
 * Observation schema. BT_OBS_SIZE is the exact number of floats written by
 * bt_core_observe - there is no padding.
 * ------------------------------------------------------------------------- */
#define BT_OBS_PLAYER    11
#define BT_OBS_ITEMS     (4 * 4)
#define BT_OBS_UPGRADES  (4 * 2)
#define BT_OBS_ENEMIES   (BT_SLOTS * 5)
#define BT_OBS_WAVE      3
#define BT_OBS_PROJ      (3 * 3)
#define BT_OBS_CONS      2
#define BT_OBS_WEAPONS (BT_SLOTS * 8)
#define BT_OBS_SHOP_EXT 12
#define BT_OBS_PROFILE 2
#define BT_OBS_SHOP_ACTION 20
/* Demon/endless block (v4), appended after the v3 161 floats so 0..160 keep
 * their indices (an artifact trained on the v3 layout stays field-comparable).
 * BT_OBS_T_LOCKS / T_CRATE / T_LVLRR are owned by the shop module
 * (offer lock flags, open crate offer, level-up reroll price).  The v5 tail
 * (BT_OBS_T_BAN / T_LVLOFF) carries the last two legality conditions the
 * artifact could not express from the v4 width: the remaining ban tokens and
 * the per-slot level-up offer presence. */
#define BT_OBS_T_STATS 4   /* harvesting/100, luck/100, endless squashed, wave/50 */
#define BT_OBS_T_LOCKS 4   /* shop offer lock flag, per slot */
#define BT_OBS_T_CRATE 4   /* crate offer kind / id / cursed / ready */
#define BT_OBS_T_LVLRR 1   /* level-up reroll price (0 = free or not offered) */
#define BT_OBS_T_ITEMS 24  /* curated guide-item owned counts, min(n,4)/4 */
#define BT_OBS_T_BAN 1     /* ban tokens / BT_BAN_MAX_TOKEN (0 when ban_mode off) */
#define BT_OBS_T_LVLOFF 4  /* level-up offer present flag, per slot */
#define BT_OBS_TARGET (BT_OBS_T_STATS + BT_OBS_T_LOCKS + BT_OBS_T_CRATE + \
                       BT_OBS_T_LVLRR + BT_OBS_T_ITEMS + BT_OBS_T_BAN + \
                       BT_OBS_T_LVLOFF)
#define BT_OBS_SIZE (BT_OBS_PLAYER + BT_OBS_ITEMS + BT_OBS_UPGRADES + \
                     BT_OBS_ENEMIES + BT_OBS_WAVE + BT_OBS_PROJ + BT_OBS_CONS + \
                     BT_OBS_WEAPONS + BT_OBS_SHOP_EXT + BT_OBS_PROFILE + \
                     BT_OBS_SHOP_ACTION + BT_OBS_TARGET)
/* Literal pin of the composed width for consumers that cannot evaluate the
 * expression: gen_obs_tables.py emits it as core_revision.obs_constants
 * BT_OBS_SIZE_PINNED, rl_obs.gd asserts its own OBS_SIZE against it, and
 * tests/test_brotato_weapon_observe.cpp asserts BT_OBS_SIZE == this pin, so the
 * bridge can never drift from the sim.  Bump both together. */
#define BT_OBS_SIZE_PINNED 203

/* run_data.gd:19 BAN_MAX_TOKEN: the ban-button budget, and the divisor of the
 * BT_OBS_T_BAN observation field (ban_tokens / BT_BAN_MAX_TOKEN). */
#define BT_BAN_MAX_TOKEN 8
BT_HOT_TYPE(float, BtHotEx, BT_MAX, BT_HOT_EX);
BT_HOT_TYPE(float, BtHotEy, BT_MAX, BT_HOT_EY);
BT_HOT_TYPE(float, BtHotEhp, BT_MAX, BT_HOT_EHP);
BT_HOT_TYPE(float, BtHotEmax, BT_MAX, BT_HOT_EMAX);
BT_HOT_TYPE(float, BtHotEhx, BT_MAX, BT_HOT_EHX);
BT_HOT_TYPE(float, BtHotEhy, BT_MAX, BT_HOT_EHY);
BT_HOT_TYPE(unsigned char, BtHotEalive, BT_MAX, BT_HOT_EALIVE);
BT_HOT_TYPE(unsigned char, BtHotEkind, BT_MAX, BT_HOT_EKIND);
BT_HOT_TYPE(unsigned char, BtHotEdying, BT_MAX, BT_HOT_EDYING);


typedef struct BtState {
    /* numeric episode log; read by the harness, never touched by bt_core_reset */
    struct Log log;

    /* portable PCG32 stream + one-shot seed bookkeeping (see brotato_rng.h).
     * `utils_rng` is a SECOND stream for the calls the original bytecode routed
     * through the Utils singleton helpers (Utils.randi / randi_range / randf /
     * rand_range), which Godot seeds independently of a node's own rand_range.
     * Both are seeded from s->seed once and then run across episodes. */
    BtRng rng;
    BtRng utils_rng;
    bt_u64 seed;
    int rng_ready;
    unsigned short character, starting_weapon;
    unsigned char weapon_slot_limit;
    unsigned short wpn_id[BT_SLOTS];
    unsigned char wpn_tier[BT_SLOTS], wpn_cursed[BT_SLOTS];
    float wpn_curse[BT_SLOTS], wpn_cd[BT_SLOTS];
    float wpn_wmod[BT_SLOTS], wpn_fxcurse[BT_SLOTS][BT_WEAPON_FX_CURSE_MAX];
    unsigned char wpn_set_tier[BT_NWEAPON_SETS];
    signed char player_facing;
    int wpn_tracked[BT_SLOTS];
    unsigned int wpn_uid[BT_SLOTS], wpn_uid_next;
    unsigned char wpn_count, wpn_overflow, wpn_dirty;
    unsigned short wpn_start_id[BT_WEAPON_START_MAX];
    unsigned char wpn_start_cursed[BT_WEAPON_START_MAX];
    unsigned char wpn_start_count, wpn_start_overflow;
    unsigned char shop_kind[4];
    unsigned short shop_id[4];
    /* shop_item.gd:129-140 set_shop_item(p_item_data, p_wave_value =
     * RunData.current_wave) stores `value = ItemService.get_value(wave_value,
     * ...)` -- the offer's price is FROZEN at creation and re-derived only from
     * the entry's OWN wave_value: a carried lock keeps the [item, wave_value]
     * pair (shop_item.gd:244 lock_player_shop_item), and the container re-runs
     * set_shop_item with that stored wave (base_shop.gd:439 set_shop_items).
     * One stamp per slot, set wherever the slot's offer is created
     * (bt_progress_roll_shop); shop_reroll_price below stores the same class of
     * value for the reroll button.  0 = never stamped (a hand-built state). */
    unsigned short shop_price_wave[4];
    float wpn_melee_t[BT_SLOTS], wpn_melee_atk_dur[BT_SLOTS];
    float wpn_melee_back_dur[BT_SLOTS], wpn_melee_recoil_dur[BT_SLOTS];
    float wpn_melee_recoil[BT_SLOTS], wpn_melee_maxrange[BT_SLOTS];
    float wpn_melee_atkdist[BT_SLOTS], wpn_melee_rot[BT_SLOTS], wpn_melee_tdist[BT_SLOTS];
    unsigned short wpn_melee_target[BT_SLOTS], wpn_melee_shots[BT_SLOTS];
    /* Per-swing per-target gate keyed by the source-OBJECT identity
     * (bt_object_uid), not the reusable live slot: a weapon.gd ignored_objects
     * equivalent.  BT_WPROJ_OBJECT_WORDS is the shared ((BT_OBJECT_UID_MAX + 32)
     * / 32) arena bound; the melee step clears the whole per-slot set when a
     * swing begins, a safe per-swing bounded clear. */
    unsigned int wpn_melee_attack_id[BT_SLOTS], wpn_melee_hit[BT_SLOTS][BT_WPROJ_OBJECT_WORDS];
    unsigned char wpn_melee_atk[BT_SLOTS], wpn_melee_anim[BT_SLOTS], wpn_melee_phase[BT_SLOTS];
    unsigned char wpn_melee_hit_on[BT_SLOTS], wpn_melee_shooting[BT_SLOTS];
    unsigned char wpn_melee_pending[BT_SLOTS], wpn_melee_ddr[BT_SLOTS], wpn_melee_facing[BT_SLOTS];
    unsigned char wtb_type[16];
    unsigned short wtb_key[16]; int wtb_val[16], n_wtb;
    unsigned short wcb_set[16], wcb_key[16]; int wcb_val[16], n_wcb;
    unsigned short soh_key[16]; int soh_val[16], n_soh;
    unsigned short wss_key[16]; float wss_pct[16]; int n_wss;
    int min_wcd[16], n_min_wcd, max_wcd[16], n_max_wcd;
    int mxp_x[16], mxp_nb[16], n_mxp;
    /* run_data.gd:1085-1161 update_item_related_effects buckets: declared
     * [stat_key, value] pairs registered per acquired copy by bt_apply_item
     * (item effects) and bt_iwb_register_weapon (weapon effects) whose
     * Effect.custom_key names the bucket -- unique_weapon_effects /
     * additional_weapon_effects / tier_i|iv_weapon_effects.  iwb_count[b] is the
     * weapon-count multiplier of the last refresh (observability); the
     * iwb_app_* list is the exact contribution applied last time, erased before
     * re-deriving, so a pair added since the last refresh is never subtracted.
     * Owned by brotato_item_weapon_bonus.h. */
    unsigned char iwb_bucket[32];
    unsigned short iwb_key[32];
    int iwb_val[32];
    unsigned char n_iwb;
    int iwb_count[4];
    unsigned short iwb_app_key[32];
    int iwb_app_delta[32];
    unsigned char n_iwb_app;
    BtWeaponFxState wfx;
    BtWProjPool wproj;
    float player_lifesteal_cd;
    /* Hitbox -> Hurtbox pairs awaiting the engine's next-frame flush (see
     * BtHitQueue): the player-shot -> enemy half is filled by the projectile
     * phase and drained at the start of the next frame's projectile phase; the
     * enemy-shot -> player half is filled by the enemy-projectile phase and
     * drained by the next frame's player-hurt flush (bt_player_hitq_apply),
     * the same MessageQueue boundary the contact hit uses. */
    BtHitQueue hitq;
    unsigned char wrng_shoot[BT_SLOTS], wrng_phase[BT_SLOTS];
    float wrng_anim[BT_SLOTS], wrng_spread[BT_SLOTS], wrng_rot[BT_SLOTS];
    int wrng_shots[BT_SLOTS], wrng_attack_id[BT_SLOTS];
    unsigned char e_charmed[BT_MAX], e_charm_shield[BT_MAX];
    short e_charm_by[BT_MAX];
    float e_charm_timer[BT_MAX];
    unsigned short charm_count[4];
    /* Per-wave source-object token for Hitbox.ignored_objects / hit-once-per-
     * target keys (weapon_projectiles.h).  A token is minted ONLY when the scene
     * instantiates a NEW node: an ordinary pooled respawn retains its
     * descriptor's token, a fresh boss/elite descriptor (recycled id) and a
     * fresh neutral mint a new one.  Tokens are 1..BT_ENEMY_OBJECT_MAX and 0
     * means "no identity" (mint refused, token_overflow set -- never a silent
     * slot fallback).  The counter resets with the weapon-projectile world at
     * every wave start (bt_clear_wave_summons), when every source node is gone. */
    /* Uids are 1..BT_OBJECT_UID_MAX (<= 11904 at the BT_MAX=192 bound), so the
     * per-slot/per-descriptor stores are 16-bit; object_uid_next (the minting
     * counter) stays 32-bit.  The guard below fails the build if the uid space
     * ever outgrows that. */
    unsigned short e_object_uid[BT_MAX];
    unsigned short pool_object_uid[BT_POOL_ID_CAP];
    unsigned int object_uid_next;
    unsigned char object_uid_overflow;
    /* The source pools NEUTRALS too (entity_spawner.gd:346 get_node_from_pool is
     * type-agnostic, keyed by the scene instance id), so a reused neutral tree
     * keeps its node identity: a per-neutral-kind LIFO of retired uids, in the
     * pool's pop order, reset per wave with the rest of the uid space. */
    unsigned short neutral_uid_pool[BT_NNEUTRALS][BT_MAX];
    unsigned short neutral_uid_n[BT_NNEUTRALS];
    /* entity.gd:68-100: a killed node plays its 1.0 s death animation and only
     * then free_entity -> add_node_to_pool, so a neutral's uid returns to its
     * kind's LIFO BT_POOL_DEATH_FRAMES frames after death.  Keyed by the SAME
     * pool ring cursor as the enemy descriptors, so the delay is identical. */
    unsigned short ndly_uid[BT_POOL_DEATH_FRAMES][BT_MAX];
    unsigned char ndly_kind[BT_POOL_DEATH_FRAMES][BT_MAX];
    unsigned char ndly_n[BT_POOL_DEATH_FRAMES];
    /* Healer BoostZone membership per healer slot (target bitset). */
    unsigned char heal_in[BT_MAX][BT_HEAL_BYTES];
    /* player.gd:1100-1128 boost from a charmed Healer/Buffer: the player.tscn
     * BoostTimer (5.0 s) and the exact temporary deltas to remove. */
    unsigned char pboost_active;
    float pboost_timer, pboost_hp, pboost_speed, pboost_aspeed;
    /* turret.gd:162-176 / landmine.gd:62-71 boost/boost_ended for structures:
     * base damage and range scaled, cooldown shrunk, then restored. */
    unsigned char pstboost_active[BT_MAX_PET];
    float pstboost_timer[BT_MAX_PET], pstboost_dmg[BT_MAX_PET];
    float pstboost_rng[BT_MAX_PET], pstboost_cd[BT_MAX_PET];

    /* --- structure of arrays: hot data, densely packed --- */
    BtHotEx ex;
    BtHotEy ey;
    BtHotEhp ehp;
    BtHotEmax emax;
    /* Enemy contact-Hitbox AREA sample for the player-contact block in
     * brotato_core.h.  enemy.tscn's Hitbox is a child Area2D of the enemy Unit
     * and the enemy Unit is a RigidBody2D in MODE_CHARACTER (entity.tscn
     * mode=2): main.cpp's per-frame order is
     *   Physics2DServer::sync -> flush_queries -> Node._physics_process ->
     *   end_sync -> Physics2DServer::step
     * so the server integrates the body during step(t) but only writes the new
     * transform back into the Node -- and therefore into every child Area2D's
     * transform -- at frame (t+1)'s flush_queries.  Step(t) therefore tests the
     * enemy Hitbox at the position the enemy had at the END of step(t-1), i.e.
     * one step behind ex/ey (which bt_core_step refreshes from the just
     * integrated body).  The player Hurtbox does NOT lag: the player is
     * MODE_KINEMATIC (player.tscn mode=3) and unit.gd writes global_position in
     * _physics_process, so its Area2D transform reaches the server before the
     * same frame's step.  bt_core_step records the pre-step (Node-visible)
     * position here for every live slot. */
    BtHotEhx ehx;
    BtHotEhy ehy;
    /* Queued player contact (unit.gd:610-611: _on_Hurtbox_area_entered ->
     * call_deferred("hurt_area_entered_deferred", hitbox)).  The AreaPair2DSW
     * formed by step(t) is emitted as `area_entered` at frame (t+1)'s
     * flush_queries, and call_deferred pushes the take_damage to that frame's
     * MessageQueue flush -- AFTER every _physics_process of frame t+1.  The
     * contact block at the end of bt_core_step queues the enemy slot here and
     * applies it at the end of the NEXT step, so hp/iframes first read one row
     * later.  -1 = nothing queued. */
    short epend;
    float edmg[BT_MAX], ecd[BT_MAX], espd[BT_MAX];
    float espeed_rand[BT_MAX]; /* Unit._speed_rand_value, retained across boosts */
    float ekbx[BT_MAX], ekby[BT_MAX];        /* stored Unit knockback vector */
    /* A killed unit's NODE outlives its `enemies`-list entry.  entity.gd:88-100
     * plays entity.tscn's `death` animation (length 1.05 s, its method key calls
     * death_animation_finished at t = 1.0 s = BT_POOL_DEATH_FRAMES ticks of the
     * animation) and only then free_entity() -> main.gd:1678-1690
     * _add_node_to_pool, whose remove_child takes the node OUT of the tree.  The
     * observer sees it for BT_POOL_DEATH_FRAMES rows after the kill row, or one
     * row more when the engine registered that die() from its IDLE pass
     * (BT_POOL_DEATH_IDLE_EXTRA: the WaveTimer-timeout clean_up_room path, which
     * misses the kill frame's own AnimationPlayer advance).  Until that frame
     * rl_obs.gd:417-425 `_collect` still walks the scene tree and reports the
     * NODE in the nearest-enemy slots: health 0 (so ehp/emax = 0), its stats
     * unchanged, drifting on the death knockback (unit.gd:230-239
     * get_next_knockback_value + unit.gd:344-352 die()'s stored vector).  Its
     * Area2D Hurtbox/CollisionShape are disabled (:527-531) and dead=true stops
     * Enemy._physics_process (:89), so it neither collides nor fights.
     *   edying[slot] > 0 = frames this dying node has left in the slot
     *   (0 = none); it is the OBSERVABLE-frame countdown of the `death` clip
     *   (edpos/edspd below), so it is rescaled whenever the clip's
     *   playback_speed changes; edkx/edky = its stored knockback_vector;
     *   edvx/edvy = the
     *   velocity cached for its next integration (a dying node is not in the
     *   contact world, so bt_enemy_dying_step integrates it directly);
     *   edkbx/edkby = the seed die() copies into edkx/edky --
     *   bt_death_knockback of the LAST hit's weighted direction and its RAW
     *   hitbox amount (unit.gd:348), refreshed by every hit primitive.
     * The slot is NOT free for a new spawn while edying > 0 (the game's node
     * still exists), and BT_POOL_DEATH_FRAMES ties this to the pool release so
     * the node is recycled exactly when it leaves the tree. */
    BtHotEdying edying;
    float edvx[BT_MAX], edvy[BT_MAX];
    float edkx[BT_MAX], edky[BT_MAX];
    float edkbx[BT_MAX], edkby[BT_MAX];
    float emass[BT_MAX];                     /* base body mass (charge x5)   */
    float echarge_dx[BT_MAX], echarge_dy[BT_MAX]; /* locked charge direction */
    BtHotEalive ealive;
    unsigned char eelite[BT_MAX];
    BtHotEkind ekind;
    /* Monotonic slot bounds until their pool is reset. Holes are retained:
     * ascending slot order (including dying nodes) is part of the simulation. */
    unsigned short e_used, pr_used, con_used, mat_used;
    unsigned int e_live_bits[(BT_MAX + 31) / 32];
    unsigned int e_dying_bits[(BT_MAX + 31) / 32];
    /* entity_spawner.gd:398-411 appends every spawned enemy to `enemies` and
     * every boss to `bosses`, erasing on death: get_all_enemies(false) is
     * `enemies + bosses` minus charmed.  e_act is that array position (a
     * monotonic activation counter; the game's list is append-ordered), used by
     * the pet "first enemy" fallback. */
    unsigned int e_act[BT_MAX];
    unsigned int e_act_next;                /* append sequence for e_act */
    /* Pool instance id backing this live slot (BT_POOL_NO_INST = neutral tree or
     * a boss/elite whose descriptor the parent did not retain). The parent
     * writes it at spawn from bt_enemy_pool_acquire and reads it back at death
     * for bt_enemy_pool_release; bt_enemy_pool_reset neutralises every entry. */
    unsigned short e_instance[BT_MAX];
    /* Frame (`ticks`) a slot was born.  entity_spawner.gd:319-331 spawn_entity
     * adds the enemy node from the DEFERRED birth boundary; Godot registers a
     * node added there for the NEXT frame's callback walk, so the newborn gets
     * its first `Physics2DServer::step` (bt_phys_step) but no `_physics_process`
     * during the frame it appears in.  The enemy update loop skips a slot whose
     * hold tick equals the current frame, exactly one frame. */
    unsigned short e_hold_tick[BT_MAX];
    unsigned char ebeh[BT_MAX];
    unsigned char ewind[BT_MAX];
    float eatk[BT_MAX], echarge[BT_MAX], echarge_spd[BT_MAX];
    unsigned char estate[BT_MAX];
    unsigned char esup[BT_MAX];                /* contact hitbox suppressed by knockback */
    unsigned char eneutral[BT_MAX];            /* 1 = BT_NEUTRALS-indexed (tree) */
    unsigned char ehit[BT_MAX];                /* Neutral.current_number_of_hits */
    /* DLC cursed enemy (brotato_enemy_curse.h): 1 = the source curse roll
     * (curse_scene_effect_behavior.gd:50-65, via bt_spawn_kind_at) cursed this
     * life.  Per-life: rewritten by every spawn into the slot, zeroed per wave,
     * and deliberately kept after the kill so the killer's weapon hook can read
     * it (the source's curse-ends-on-death is per-node-life). */
    unsigned char ecurse[BT_MAX];
    float eelapsed[BT_MAX];                  /* boss/attack state clock (WHOLE seconds) */
    float estate_timer[BT_MAX];              /* 1 Hz CheckStateTimer accumulator */
    float edecay[BT_MAX];                    /* decaying_bonus_speed (contact slow) */
    float ebtimer[BT_MAX];                   /* buffer/pursuer BoostTimer, FRAMES left */
    unsigned char espawn_n[BT_MAX];          /* spawner children emitted so far */
    unsigned char eboosted[BT_MAX];          /* entity.is_boosted (buffer) */
    /* pursuer.gd:14,45,55-58 BoostTimer life: nb_times_boosted plus the shared
     * BoostTimer countdown in ebtimer (a kind has either ebtimer as a
     * Buffer's BoostZone timer or as the Pursuer's self-boost timer, never
     * both: buffer-like kinds have no boost_self and vice versa). */
    unsigned char enboost[BT_MAX];
    /* pursuer.gd:59,75-83 boost_self -> set_size: every boost adds
     * 0.25*size_on_boost to the Collision/Hurtbox/Hitbox shape SCALES, and
     * set_size is never undone (reset_size is called by the DLC anglerfish's
     * reset_attack_data only), so the growing size needs a monotone per-life
     * counter -- the hit reset zeroes nb_times_boosted without shrinking the
     * shape.  egeom is the live geometry EVERY radius read uses (bt_kgeom), so
     * the scaled radii reach the body, the hurtbox and the contact hitbox in
     * one place instead of at every call site. */
    unsigned char eboost_n[BT_MAX];
    BtBodyPhysics egeom[BT_MAX];
    /* The kind and boost count egeom[slot] was BUILT for: bt_kgeom only trusts
     * the row when both still match the live slot, so a stale/zeroed row can
     * never be read (a hand-filled fixture slot with a matching stale count
     * cannot exist, and any other stale case falls back to the constant row). */
    unsigned char egeom_kind[BT_MAX];
    unsigned char egeom_n[BT_MAX];
    /* fly.gd:6-11 reaction shot: 1 = the hurtbox-entry roll succeeded this frame
     * and the attack behavior's shoot() is queued for the deferred boundary
     * (call_deferred, exactly like unit.gd:610-611's hurt_area_entered). */
    unsigned char eflypend[BT_MAX];
    /* buffer.gd:17,26-28,39,44,82-83 entities_in_zone: the BoostZone's
     * body_entered INSERTION-ORDER membership list of enemy slots (the zone mask
     * is the enemy layer, buffer.tscn:56) plus its length.  A member that leaves
     * the zone (or dies) is erased, exactly like Array.erase on body_exited. */
    unsigned char zone_mem[BT_MAX][BT_MAX];
    unsigned char zone_n[BT_MAX];
    float elastspread[BT_MAX];               /* base_direction_constant_spread alternation */
    unsigned char eshots[BT_MAX];            /* shots taken (long-cooldown cadence) */
    /* --- enemy special lifecycle (brotato_enemy_motion.h /
     * brotato_enemy_lifecycle.h) ------------------------------------------ */
    /* stargazer.gd:10,24-28,46-54 + stargazer_movement_behavior.gd:4-16: the
     * linked IronLung's POOL INSTANCE id (BT_POOL_NO_INST = none) and the angry
     * OnTriggeredMovementBehavior switch.  Both are cleared on every spawn
     * (Stargazer.respawn:24-28). */
    unsigned short mv_lung[BT_MAX];
    unsigned char mv_angry[BT_MAX];
    /* pursuer.gd:16,69-72 movement_behavior_changed: the ONE-WAY switch to
     * $MovementBehaviorOnBoost once nb_times_boosted reaches
     * change_movement_behavior_after_x_boosts.  Sticky per life (the hit reset
     * zeroes nb_times_boosted but never switches back), cleared on every spawn
     * (Pursuer.respawn:46). */
    unsigned char mv_boost[BT_MAX];
    /* enemy.gd:24,227,351 source_spawner: live slot of the enemy that emitted
     * wanted_to_spawn_an_enemy (-1 = none).  The spawn-attack path sets it; a
     * death clears it. */
    short e_source[BT_MAX];
    /* evil_mob.gd:11,32-55 gold_count / evolution, per live slot. */
    unsigned short emob_count[BT_MAX];
    unsigned char emob_evo[BT_MAX];
    /* The percent modifiers of the boost currently in force (bt_buffer_boost),
     * kept so get_angry can ADD its own before recomputing (stargazer.gd:72-78
     * combines with _boosted_args).  Cleared on every spawn (Enemy.respawn ->
     * init_current_stats -> reset_*_stat(0)). */
    float eboost_hp[BT_MAX], eboost_dmg[BT_MAX], eboost_spd[BT_MAX];
    /* Deferred stargazer-death -> linked-lung death queue (iron_lung.gd:47-50);
     * drained once per frame by the lifecycle tick, never recursively. */
    unsigned short lk_q[BT_MAX];
    unsigned char n_lk_q;
    /* --- player visual/footstep state (owner: brotato_player_visual.h) ----- */
    float player_fx_time, player_fx_inactive, player_fx_age[10];
    unsigned int player_fx_cycle;
    unsigned short player_fx_active;
    unsigned char player_fx_processing, player_anim_mode;
    bt_u64 player_anim_time;
    float prx[BT_MAX_PROJ], pry[BT_MAX_PROJ], prvx[BT_MAX_PROJ], prvy[BT_MAX_PROJ], prdmg[BT_MAX_PROJ];
    float prrot[BT_MAX_PROJ], prage[BT_MAX_PROJ];
    float prrad[BT_MAX_PROJ], prhx[BT_MAX_PROJ], prhy[BT_MAX_PROJ];
    unsigned char pralive[BT_MAX_PROJ];
    unsigned short prcre[BT_MAX_PROJ];   /* this shot's creation ordinal        */
    unsigned char prshape[BT_MAX_PROJ];      /* BT_SHAPE_* */
    unsigned char prfriendly[BT_MAX_PROJ];   /* 1 = pet/turret shot (hits enemies) */
    /* 1 = the shot came from a PET whose scene sets shoot_projectiles=true
     * (pet.gd:6; only catling_gun.tscn and bot_o_mine.tscn do), which is what
     * fly.gd:10 tests (`hitbox.from is Pet and hitbox.from.shoot_projectiles`).
     * Turret/structure shots are prfriendly but NOT this. */
    unsigned char pr_pet[BT_MAX_PROJ];
    /* burning_data a pet's shot carries (set at spawn from its firing pet's
     * pet_burn_*); rolled on the friendly-projectile hit. */
    float prburn_chance[BT_MAX_PROJ];
    float prburn_dmg[BT_MAX_PROJ];
    float prburn_dur[BT_MAX_PROJ];
    /* crit riding a pet shot (from the firing pet's pet_crit_*), rolled per hit
     * on the friendly-projectile hit (unit.gd:285-300) */
    float prcrit_chance[BT_MAX_PROJ];
    float prcrit_dmg[BT_MAX_PROJ];
    unsigned short pr_expl[BT_MAX_PROJ];
    BtItemExplosions expl;
    float conx[BT_MAX_CONS], cony[BT_MAX_CONS];
    unsigned char conalive[BT_MAX_CONS], conkind[BT_MAX_CONS];
    float con_arc[BT_MAX_CONS][6], conspd[BT_MAX_CONS];
    unsigned char con_attr[BT_MAX_CONS];
    /* main.gd:637 push_back / :797 erase keeps `_consumables` insertion-ordered;
     * con_seq records each consumable's spawn sequence so the wave-end cleanup
     * (main.gd:1005-1010) can walk that order instead of slot order.  The
     * (seq, slot) key is total, so a fixture that never wrote a sequence still
     * gets a deterministic order (mirrors mat_seq). */
    unsigned int con_seq[BT_MAX_CONS];
    unsigned int con_seq_next;
    float matx[BT_MAX_MAT], maty[BT_MAX_MAT], matspd[BT_MAX_MAT];
    unsigned short matval[BT_MAX_MAT];
    unsigned char matalive[BT_MAX_MAT];
    float mat_arc[BT_MAX_MAT][6];
    unsigned char mat_attr[BT_MAX_MAT];
    /* Gold owned by an evil_mob live slot (evil_mob.gd:14-27 attracted_by); -1
     * when nobody but a pet/player/itself owns it (mat_by holds pet owners). */
    short mat_mob[BT_MAX_MAT];
    /* main.gd:28,737,797 `_active_golds` is INSERTION-ordered: the spawn
     * sequence of the gold now in a slot (ascending == the source list order
     * across erases/reuse) and the monotonic counter. */
    unsigned int mat_seq[BT_MAX_MAT];
    unsigned int mat_seq_next;
    /* RunData.bonus_gold (run_data.gd:179,697-706): banked gold that the next
     * dropped coin absorbs (main.gd:720-731) and that an evil_mob's pickup banks
     * (main.gd:854 with player_index -1).  Clamped to >= 0 on write. */
    int bonus_gold;
    int matcount;
    /* pets/turrets/structures from items: real WeaponStats (damage/cooldown/
     * range/scaling) from BT_ITEM_WSTATS, not a scalar DPS. */
    float pet_x[BT_MAX_PET], pet_y[BT_MAX_PET];
    unsigned char pet_lm_pressed[BT_MAX_PET];
    float pet_dmg[BT_MAX_PET], pet_range[BT_MAX_PET];
    float pet_cd[BT_MAX_PET], pet_timer[BT_MAX_PET];
    float pet_pspeed[BT_MAX_PET], pet_spread[BT_MAX_PET];
    short pet_sap[BT_MAX_PET], pet_sic[BT_MAX_PET];   /* spawn_around_player / spawn_in_center */
    unsigned char pet_nproj[BT_MAX_PET];
    unsigned char pet_alive[BT_MAX_PET];
    unsigned char pet_isstruct[BT_MAX_PET]; /* 1 = structures_hash entity (MAX_STRUCTURES) */
    unsigned char pet_perm[BT_MAX_PET];     /* 1 = player effect emitter, survives a wave */
    unsigned char pet_opcode[BT_MAX_PET];   /* BT_ITEMEFF_* of the source effect */
    unsigned short pet_row[BT_MAX_PET];     /* BT_ITEM_EFF_ROWS row instantiated */
    float pet_double[BT_MAX_PET];           /* LootwormEffect.double_chance in force */
    float pet_lm_cd[BT_MAX_PET];            /* BotOMineEffect landmine cadence (s) */
    float pet_lm_timer[BT_MAX_PET];         /* BotOMineEffect landmine countdown (s) */
    /* per-pet burning carried by the pet's own shots (turret_flame /
     * Blazemander); written at spawn, read by the friendly-projectile hit. */
    float pet_burn_chance[BT_MAX_PET];
    float pet_burn_dmg[BT_MAX_PET];
    float pet_burn_dur[BT_MAX_PET];
    /* WeaponStats crit chance/damage in force on this entity's shots
     * (weapon_service.gd:251-253: the player's stat_crit_chance is added unless
     * this is a structure and structures_can_crit is not owned). */
    float pet_crit_chance[BT_MAX_PET];
    float pet_crit_dmg[BT_MAX_PET];
    int pet_n;
    /* --- source node pool for structures/pets (main.gd `_pool`) -------------
     * entity_spawner.gd:346-359 pops a node of the SAME scene out of
     * main._pool[scene.get_instance_id()] (LIFO: main.gd:1662 pop_back) and
     * calls ONLY respawn(); every other spawn is scene.instance(). A pooled
     * node therefore keeps every member the source never redraws on
     * respawn()+set_data(): a Turret's `_cooldown` (turret.gd:11) and
     * WanderingBot's class-member initializers `distance`/`rotation_speed`/
     * `_angle` (wandering_bot.gd:4-8).  A structure node re-enters its scene's
     * stack 1.0 s after death (entity.tscn's `death` animation calls
     * death_animation_finished at t=1.0 s -> free_entity -> add_node_to_pool)
     * and 4.0 s after a Structure.deferred_die() (PENDING_DIE_DURATION 3.0 s
     * then that same death).  The pool is per WAVE: Main is rebuilt every wave
     * (main.gd:1304 -> ui/menus/shop/base_shop.gd:346) and Main._exit_tree
     * (main.gd:1727-1737) queue_frees the whole pool, so
     * bt_struct_pool_reset (bt_clear_wave_summons) empties it at every wave
     * start.
     *   pet_pool_set[p]     1 = this slot holds a node already instantiated this
     *                       wave, whose scene is pet_pool_scene[p].
     *   pet_pool_scene[p]   BT_ITEM_EFF_ROWS[].scene of that node (the pool key:
     *                       main.gd keys on scene.get_instance_id()).
     *   pet_pool_seq[p]     LIFO stamp; 0 while the node is live, else the stamp
     *                       of the return -- the highest stamped node is the one
     *                       the source's pop_back hands out first.
     *   pet_pool_avail[p]   first s->ticks on which the returned node is back in
     *                       its scene's stack (death + BT_POOL_DEATH_FRAMES).
     *   pet_pool_pending[p] 1 = Structure.deferred_die() in flight: the node is
     *                       still alive and still in the world, but it already
     *                       emitted `died` (structure.gd:72-76), so it left
     *                       `structures` and no longer counts for
     *                       MAX_STRUCTURES; a Turret stops shooting while
     *                       pending (turret.gd:78 `not _pending_die`) but a
     *                       Garden does NOT (garden.gd:9-10).
     *   pet_pool_die_at[p]  the s->ticks bt_struct_pool_tick calls die() on
     *                       (3.0 s after the eviction).
     *   pet_pool_stamp      monotonic pet_pool_seq allocation stamp.
     *   pet_pool_overflow   1 = no free slot could host the new instance. The
     *                       source's pool is unbounded (scene.instance() on a
     *                       miss); this sim's pet table is BT_MAX_PET wide, so
     *                       the miss is explicit (mirrored by summon_overflow),
     *                       never a silent drop. */
    unsigned char  pet_pool_set[BT_MAX_PET];
    unsigned short pet_pool_scene[BT_MAX_PET];
    unsigned int   pet_pool_seq[BT_MAX_PET];
    unsigned int   pet_pool_avail[BT_MAX_PET];
    unsigned char  pet_pool_pending[BT_MAX_PET];
    unsigned int   pet_pool_die_at[BT_MAX_PET];
    unsigned int   pet_pool_stamp;
    unsigned char  pet_pool_overflow;
    /* --- pet motion + interaction zones (brotato_pet_motion.h) -------------
     * Per-slot AI state for the live pet/structure entities, built from the
     * pet's BT_PET_AI row the first frame a slot is seen alive and dropped the
     * first frame it is not. `pet_ai_row` guards a slot reused by a different
     * pet row inside one wave (bt_pet_compact only runs at a wave boundary,
     * where every non-permanent pet is already dead). */
    unsigned short pet_fx[BT_MAX_PET];      /* summon_fx[] instance feeding it (0xFFFF none) */
    unsigned short pet_ai_row[BT_MAX_PET];  /* pet_row this AI state was built from */
    unsigned char pet_ai_init[BT_MAX_PET];  /* 1 = per-slot state initialised */
    unsigned char pet_dead[BT_MAX_PET];     /* Scapegoat: unit dead (reviving) */
    unsigned char pet_zone_in[BT_MAX_PET];  /* DocMoth: boost zone covers the player */
    unsigned char pet_tree_in[BT_MAX_PET];  /* Lootworm: TreeArea2D holds >=1 tree */
    unsigned char pet_melee[BT_MAX_PET];    /* Lootworm: melee swing in progress */
    unsigned char pet_mad[BT_MAX_PET];      /* CatlingGun: player/pet inside PlayerTriggerZone */
    float pet_speed[BT_MAX_PET];            /* current_stats.speed */
    float pet_hp[BT_MAX_PET], pet_maxhp[BT_MAX_PET];  /* Scapegoat health */
    float pet_float_hp[BT_MAX_PET];         /* Scapegoat _floating_health */
    float pet_hurt_cd[BT_MAX_PET];          /* Scapegoat INVULNERABLE_COOLDOWN */
    float pet_revive_dur[BT_MAX_PET];       /* Scapegoat revive_duration (this instance) */
    float pet_melee_cd[BT_MAX_PET];         /* Lootworm melee cooldown (s) */
    float pet_hit_dmg[BT_MAX_PET];          /* Lootworm _hitbox.damage (persists within a wave) */
    short pet_melee_tgt[BT_MAX_PET];        /* Lootworm: tree slot for the deferred hit (-1 none) */
    float pet_wb_dist[BT_MAX_PET];          /* WanderingBot: class-init `distance` (rand_range 100..200) */
    float pet_wb_rots[BT_MAX_PET];          /* WanderingBot: class-init `rotation_speed` (2..2.5) */
    float pet_wb_angle[BT_MAX_PET];         /* WanderingBot: class-init `_angle` (0..2*PI) */
    float pet_tx[BT_MAX_PET], pet_ty[BT_MAX_PET];  /* movement target */
    float pet_pangle[BT_MAX_PET];           /* DocMoth patrol angle */
    float pet_pphase[BT_MAX_PET];           /* DocMoth patrol phase */
    float pet_pradius[BT_MAX_PET];          /* DocMoth patrol radius */
    unsigned char mat_by[BT_MAX_MAT];       /* gold attracted_by: 0 none/player, else pet slot+1 */
    float zone_hp_regen_bonus;              /* DocMoth BoostZone hp_regen_bonus in force */
    int   zone_hp_regen_threshold;          /* its value2: percent of max_hp threshold  */
    unsigned char zone_double_lifesteal;    /* DocMoth BoostZone double_lifesteal_bonus in force */
    /* --- item-granted structures/pets: the entity_spawner.gd lifecycle -----
     * Registered (not instantiated) on acquisition, created by the emulated
     * StructureTimer. `summon_fx[]` is Keys.structures_hash + stat_pets_hash;
     * pend_struct/pend_pet are queues_to_spawn_structures/queues_to_spawn_pets;
     * base_*_spawned are the entity_spawner _base_structures/pets_spawned flags
     * (false at every wave start, set true at the end of the wave's first
     * timer tick). */
    BtSummonFx summon_fx[BT_MAX_SUMMON_FX];
    int n_summon_fx;
    unsigned char summon_overflow;      /* 1 = a registration was rejected */
    unsigned char base_struct_spawned;  /* entity_spawner _base_structures_spawned */
    unsigned char base_pet_spawned;     /* entity_spawner _base_pets_spawned */
    BtPending pend_struct[BT_MAX_PEND_SMALL], pend_pet[BT_MAX_PEND_SMALL];
    int n_pend_struct, n_pend_pet;
    /* WeaponService.get_structure_attack_speed inputs: the structure_attack_speed
     * stat (Effect key=structure_attack_speed, SUM) and the
     * structures_cooldown_reduction KEY_VALUE bucket ([stat key, percent]
     * entries, duplicate keys accumulate like effect.gd apply). */
    float structure_attack_speed;
    unsigned short scdr_key[8];
    float scdr_pct[8];
    int n_scdr;
    /* Scalars for interned effect keys that have no core stat id (items_price,
     * xp_gain, free_rerolls, reroll_price, level_upgrades_modifications,
     * harvesting_growth, ...). SUM-storage Effects accumulate, REPLACE-storage
     * ones overwrite; the shop/XP/harvest consumers read them from here. */
    float scalar_effect[BT_NITEM_STRS];
    /* --- scalar-event player state (brotato_scalar_events.h, ScalarEvents) ---
     * player.gd:746-755 creates an independent timer per healed HP. The ring
     * covers the complete source-derived duration, preserving overlapping
     * pickups without per-step allocation or a fixed number of heal chains. */
    int hot_due[BT_HOT_MAX_FRAMES + 1];
    int hot_cursor, hot_pending;
    unsigned char mat_home[BT_MAX_MAT];  /* gold homing to the player from any distance */
    unsigned short prsrc[BT_MAX_PROJ];   /* enemy slot that fired the enemy projectile */
    /* Depth-first death callbacks without GPU-incompatible recursion. Each
     * enemy can enter this stack once before the deferred spawn boundary. */
    unsigned short death_stack[BT_MAX];
    unsigned char death_phase[BT_MAX];
    int n_death_events;
    /* --- deferred death (unit.gd:344-356) --------------------------------
     * take_damage's `call_deferred("die", _die_args_unit)` runs inside the SAME
     * MessageQueue flush that runs hurt_area_entered_deferred (message_queue.cpp
     * flush() swaps buffers at entry, so a call pushed during the flush is
     * drained by the same flush).  Every hit of one flush therefore applies
     * FIRST -- damage, crit/burn rolls, hitbox.killed_something -- and only then
     * do the queued die() bodies run, in the order the hits landed (the spawner's
     * `died` handler, spawn_loot / XP, the death-spawn children: all the
     * loot/drop RNG).  bt_queue_kill appends the slot here and bt_drain_kill_q
     * runs the kills; epend_die marks a slot whose lethal hit is already in the
     * queue, which is unit.gd:266-267 `if _pending_die: return [0, 0, false]`
     * (a second hit in the same flush deals NOTHING and cannot queue twice). */
    unsigned char ekill_q[BT_MAX];
    /* Timer-driven (burning) lethals, drained BEFORE ekill_q: unit.tscn's
     * BurningTimer runs in SceneTree::iteration's "physics_process_internal"
     * pass, so its deferred die() is pushed into the SAME MessageQueue flush
     * AHEAD of the die()s that flush itself queues from the area hits -- see
     * bt_queue_burn_kill. */
    unsigned char ekill_tq[BT_MAX];
    unsigned char epend_die[BT_MAX];
    int n_kill_q;
    int n_kill_tq;
    /* Owned multiplicity per item index (ItemData pool bookkeeping: max_nb
     * limits, recycling, ...). Exact, so a purchase is never lost. */
    unsigned short item_owned[BT_NITEMS];
    /* --- source stat layering (run_data.gd:1305-1313 get_stat/get_stat_gain,
     * temp_stats.gd:37-42, linked_stats.gd:12-78) -------------------------------
     * Every stat is stored raw (no gain, no cap) in three sources, exactly like
     * the source singletons:
     *   stat_raw  = RunData.get_player_effect  (base + items + upgrades) PERMANENT
     *   temp_stat = TempStats                  (timed buffs)               TEMPORARY
     *   link_stat = LinkedStats                 (GainStatForEveryStat rows) LINK
     * plus stat_gain = RunData.get_stat_gain multiplier (1 + gain_<stat>/100).
     * The observable stat fields below (max_hp/hp, regen, speed_pct, dodge,
     * dmg_pct, ..., crit, curse, ...) hold the DERIVED value
     *   (raw + temp + link) * gain
     * capped by the *_cap effect for hp/speed/dodge/crit, and are recomputed by
     * bt_core_stat_sync() after every change. Storage is never clamped, so a
     * cap or a temporary buff is always reversible. */
    float stat_raw[BT_NSTATS];
    float stat_gain[BT_NSTATS];
    /* stat caps (StatCapEffect / init_stats): hp, speed, dodge(abs %), crit(abs %)
     * indexed by core stat id 0..BT_NSTATS-1 (14..18 default to BT_STAT_CAP_NONE) */
    float stat_cap[BT_NSTATS];
    /* Persisted stat links from owned items (linked_stats.gd:12-78): registered
     * on item acquire, then recomputed from the CURRENT permanent (+temp when the
     * row's perm_stats_only is false) stats after every item/upgrade and source
     * change -- never applied inline. Stored as owned multiplicity per effect
     * ROW (exact, bounded by BT_NITEM_EFF_ROWS, so no item/link is ever
     * dropped); link_stat[t] is the RAW LinkedStats contribution. */
    unsigned short lk_count[BT_NITEM_EFF_ROWS];
    float link_stat[BT_NSTATS];
    float link_refresh_timer;   /* linked_stats.gd half-second refresh flag */
    /* TempStatsPerIntervalEffect (temp_stats_per_interval_effect.gd apply):
     * source merges by (key, interval, reset_on_hit) and SUMs value, so repeat
     * copies of one effect never consume a second slot. */
    unsigned char tsi_id[8], tsi_reset[8];
    float tsi_val[8], tsi_interval[8], tsi_timer[8], tsi_stacks[8];
    int n_tsi;
    float temp_stat[BT_NSTATS];
    float temp_scalar[BT_NITEM_STRS]; /* TempStats keys outside the core stat IDs */
    unsigned char moving_bonuses_applied, not_moving_bonuses_applied;
    /* enemy_percent_damage_taken debuff records (brotato_enemy_hit_effects.h).
     * The retired player-scoped `pd_*` reading of PercentDamageEffect (a temp
     * stat on the PLAYER from an item pickup) was the wrong pool: the source
     * pushes the effect into the player's enemy_percent_damage_taken BUCKET and
     * the ENEMY's effect behavior accumulates it.  The item row now registers
     * through bt_scalar_bucket_register like every other bucket key. */
    BtEnemyHitFx ehitfx;
    /* One ordinary wisp tuple plus 10 cursed intervals * 3 grant caps. */
    unsigned char gwb_id[31];
    unsigned short gwb_row[31], gwb_count[31];
    int gwb_every[31], gwb_gain[31], gwb_limit[31];
    int gwb_kills[31], gwb_grants[31];
    int n_gwb;
    /* decaying_stats_on_hit/on_consumable merge key: (stat id, duration), as
     * player.gd _start_decaying_stats_effect_timer matches on both. */
    float dt_dur[8];
    /* 1 = a temporary-effect registry (tsi/pd/dt/gwb) was genuinely full and a
     * DISTINCT effect had to be dropped. Repeat copies merge, so this only means
     * real capacity exhaustion (never a silent per-copy loss). */
    unsigned char stat_reg_overflow;
    /* burning on hit (BT_ITEM_BURNING): per-enemy remaining FRAMES (BurningData
     * duration * the unit.tscn BurningTimer period) + the damage of one tick */
    float eburn[BT_MAX], eburndmg[BT_MAX];
    float burn_chance, burn_dmg, burn_dur;   /* player's burn chance/damage/ticks */
    int ecount;

    float px, py, hp, max_hp, regen, armor;
    float iframe, regen_timer;
    /* Deferred player-node stat scope (player.gd:349-368 update_player_stats).
     * `max_hp`, `armor`, `speed_pct` and `dodge` above are the IMMEDIATE
     * stat-layer reads (RunData.get_stat / Utils.get_stat) the observation and
     * the effect system use.  The player's own LiveStats (max_stats.health,
     * current_stats.speed/armor/dodge, and the heal that follows a max_hp
     * increase) are refreshed only by the deferred stats_updated pass
     * (run_data.gd:1400-1406 add_stat sets _are_player_stats_dirty ->
     * run_data.gd:221-222 _physics_process call_deferred("_emit_stats_updated")
     * -> run_data.gd:1418-1427 emit "stats_updated" -> main.gd:339
     * on_stats_updated -> global/stats_manager.gd:105-126 reload_stats queues the
     * player -> stats_manager.gd:20-31 _physics_process calls
     * player.update_player_stats()).  bt_player_stat_sync_step below models that
     * pipeline; the p_* fields are that LiveStats snapshot. */
    float p_max_hp, p_speed_pct, p_armor, p_dodge;
    /* _are_player_stats_dirty (any stat layer changed) and the StatsManager
     * _player_queue entry (the deferred emit drained it). */
    unsigned char stat_dirty, stat_refresh_queued;
    int zone;               /* 0 = zone_1 campaign, 1 = abyss DLC */
    int phase;              /* 0 = combat, 1 = shop between waves, 2 = level up */
    float dmg_stat;         /* explosion scales x3 this */
    float speed_bonus;
    float materials;
    float speed_pct, dodge, dmg_pct, crit, luck;
    float melee, ranged, elemental;
    /* core stat ids 14..18 (source upgrade pool: accuracy, attack_speed,
     * engineering, lifesteal, range). None have a finite source cap. */
    float accuracy, attack_speed, engineering, lifesteal, range_stat;
    unsigned char shop_bought;
    /* player.gd:32,110,305-308: Player._hit_protection, re-armed from the
     * owned hit_protection effect at each wave's Player._ready (the player
     * node is rebuilt every wave) and spent one charge per non-dodged hit
     * that reaches get_damage_value (tardigrade). */
    unsigned char hit_prot;
    float crit_dmg, harvest;
    float proc_chance, proc_dmg;
    /* Current level-up offers, encoded as (BT_UPGRADES row + 1) so 0 means "no
     * offer" (bt_lvl_offer_row / bt_lvl_offer_present above); the one non-table
     * offer uses BT_UPGRADE_SLOT_OFFER.  bt_progress_roll_upgrade fills it. */
    unsigned short lvl_offers[4];
    /* UpgradesUIPlayerContainer._old_upgrades (upgrades_ui_player_container.gd:
     * 37,65-66): the four offers shown at the PREVIOUS level-up of this
     * end-of-wave session.  The container is a node of main.tscn, which is
     * rebuilt every wave (base_shop.gd:346), so the set is empty at the first
     * level-up of a wave and is refreshed after every get_upgrades() call.
     * bt_progress_roll_upgrade excludes any offer whose BT_UPGRADES stat matches
     * one of these (is_upgrade_already_added compares upgrade_id_hash, and
     * upgrade_id identifies the STAT, not stat+tier).  Same row+1 encoding as
     * lvl_offers[] (0 = no offer). */
    unsigned short lvl_prev[4];
    int shop_timer;
    int shop_steps;
    int reroll_count;       /* per-shop reroll counter (item_service) */
    /* item_service.gd reroll bookkeeping (parent progression owns the
     * consumers). `free_rerolls_remaining` is re-armed at each shop open from
     * the owned free-reroll scalar; `paid_rerolls` counts the paid rerolls of
     * the current shop. */
    int paid_rerolls;
    int free_rerolls_remaining;
    int initial_free_rerolls;
    unsigned char has_bonus_free_reroll;
    BtShopItemCopy shop_copies[3][BT_MAX_PENDING];
    unsigned short shop_copy_head[3], shop_copy_count[3];
    int pending_items;      /* item boxes deferred to wave end      */
    unsigned char pending_item_kind[BT_MAX_PENDING]; /* FIFO; legendary boxes retain tier */
    int items_spawned_this_wave;
    int pending_upgrades;   /* level-ups queued at wave end (main.gd:1114-1125) */
    int pending_extra_elite; /* -1 none; candy bag's next-wave elite */
    unsigned char shop_available; /* 4-bit mask of the shop slots that are still buyable */
    /* Per-offer lock flag (shop_item.gd:238-252 change_lock_status ->
     * run_data.gd:461-469 lock/unlock_player_shop_item). A locked offer keeps
     * its [item, wave] entry across a reroll (base_shop.gd:360-380
     * fill_shop_items keeps player_locked_items and fills only the rest), so
     * locked[i] == 1 means slot i still holds its original offer. Gated by the
     * disable_item_locking effect (no producer on any reached profile). */
    unsigned char shop_locked[4];
    /* Crate (item box) offer held during the level-up phase instead of being
     * auto-taken: upgrades_ui_player_container.gd:88-95 show_consumable_data ->
     * ItemService.process_item_box, taken by :183-190 or recycled by :192-199
     * (main.gd:1145 on_item_box_discard_button_pressed). kind uses the
     * BT_SHOP_NONE/ITEM/WEAPON domain; 0 when no crate is pending. */
    unsigned char crate_kind;
    unsigned short crate_id;
    unsigned char crate_cursed;
    /* UpgradesUIPlayerContainer._reroll_count/_reroll_price
     * (upgrades_ui_player_container.gd:16,57-75): the level-up reroll is charged
     * in gold at ItemService.get_reroll_price(current_wave, _reroll_count) and
     * its counter is per main.tscn instance, i.e. per wave. */
    int lvl_reroll_count;
    int lvl_reroll_price;
    /* base_shop.gd:479-481 set_reroll_button_price stores the button's price;
     * it is recomputed (with RunData.current_wave AT THAT MOMENT) when the shop
     * opens (:132), after a reroll (:465), after a purchase (:539) and after a
     * ban (:851).  Storing it keeps the trailing shop frame correct after GO has
     * already incremented current_wave (v5 s1 row 308: wave 2, reroll price still
     * the wave-1 value). */
    int shop_reroll_price;
    /* base_shop.gd:339-349 _on_GoButton_pressed -> get_tree().change_scene():
     * the scene swap is DEFERRED to the end of the frame, so the frame that
     * presses GO still shows the Shop (the traced row's obs[7] is 1 there) and
     * the next wave's arena exists only from the following tick.  The skip
     * action's immediate rolls (hourglass, extra-elite) happen in this frame;
     * the transition itself lands on the next tick through these two fields. */
    unsigned char shop_go_pending;
    unsigned char shop_go_wave_reset;
    /* run_data.gd:19,559-560 BAN_MAX_TOKEN: every run starts with 8 ban tokens
     * (BT_BAN_MAX_TOKEN); the observe writer normalizes ban_tokens by that.
     * `ban_mode` folds the gates the source's ban button needs: settings.
     * ban_mode_toggled (run_data.gd:475,198 is_ban_mode_active) and the run's
     * uses_ban.  It defaults ON because the RL bridge's default settings force
     * ban_mode_toggled = true (rl_env.gd:34) and rl_run.gd:467 mirrors it into
     * uses_ban; a caller wanting the ban UI off clears it explicitly. */
    unsigned char ban_tokens;
    unsigned char ban_mode;
    /* Audio-RNG model for the sound sites the RL bridge can neutralize.
     * 1 (default) = every source draw is consumed: each sound-element pick
     * (Array.pick_random over a gameplay sound array, utils.gd:391-394) AND the
     * SoundManager/SoundManager2D playback-pitch rand_range
     * (sound_manager_parent.gd:45).  0 = the bridge's audiovisual neutralization
     * is in effect: mod_main.gd clears both sound QUEUES before their own
     * _process pops them (`_clear_sound_queues`) and pins the floating-text
     * manager (emptying only ITS four sound arrays), so the playback-pitch draws
     * and the floating-text picks do not happen.  The GAMEPLAY picks still do --
     * a weapon's shooting_sounds / custom_hit_sounds and a unit's hurt sounds
     * are drawn BEFORE `SoundManager*.play` is called and the bridge leaves
     * those arrays in place.  Only draws are skipped, never gameplay logic.
     * Set from the `audio_rng` config key / the trace's `audio_rng` metadata. */
    unsigned char audio_rng;
    /* Particle-RNG model for the CPUParticles2D per-particle draws the RL
     * bridge can suppress (brotato_player_visual.h models the source's
     * running-smoke node: Particle.seed + 9 randf per restarted particle).
     * 1 (default) = the source's draws are consumed; 0 = the bridge clears
     * every CPUParticles2D emitting flag, so the engine takes its
     * `if (!emitting) { p.active = false; continue; }` branch and consumes no
     * per-particle randomness (cpu_particles_2d.cpp:838-841).  Set from the
     * `particles_rng` config key / trace metadata. */
    unsigned char particles_rng;
    /* DebugService.invulnerable (run_data.gd:556 `invulnerable =
     * DebugService.invulnerable`, consumed by player.gd:114-115 `if
     * RunData.invulnerable: disable_hurtbox()`): the player's Hurtbox Area2D is
     * disabled, so no enemy contact Hitbox and no enemy projectile ever REPORTS
     * the player (unit.tscn's Hurtbox signal).  The gate is therefore at the two
     * hurtbox overlap tests (bt_player_contact_probe / bt_proj_hits_player), NOT
     * at the damage path: the sim consumes exactly the draws the game does, with
     * no iframe/dodge roll, no hurt effect, no dmg_taken and no explode-on-hit
     * pass from a player hurt.  Enemy AI, wave RNG and projectile motion are
     * unchanged, and a shot that reaches the (disabled) hurtbox keeps flying
     * instead of stopping.  0 (default) = the normal player hurtbox.  Set after
     * bt_core_init from the optional `invulnerable` config key / trace metadata;
     * bt_core_reset never clears it. */
    unsigned char invulnerable;
    /* base_shop.gd:53-82: destroy_weapons / upgrade_random_weapon (anvil) run
     * once per run, at the first shop entry. */
    unsigned char shop_effects_checked;
    /* Loot pool unlock profile.  0 (default) = ProgressData.unlock_all_items()
     * -- every can_be_looted item is in _tiers_data (the guide's setup); 1
     * models a FRESH save, where items_unlocked holds only items with
     * ItemParentData.unlocked_by_default (progress_data.gd:884-895).  Only this
     * single bit of ProgressData.items_unlocked is representable here. */
    unsigned char lock_default_items;
    /* Banned offer identities (run_data.gd:475,559-560 + shop_item.gd:204-210):
     * players_data[].banned_items, keyed by my_id_hash; the offer is dropped
     * from BOTH _get_rand_item_for_wave pools (:316-330).  Stored as canonical
     * (kind, idx) because item and weapon hashes live in different spaces. */
    unsigned char ban_kind[8];
    unsigned short ban_id[8];
    unsigned char ban_count;
    /* Granted-but-not-shown extra crate items (upgrades_ui.gd:100-105,141-159):
     * after the crate item, EACH granted key becomes its own take/recycle
     * offer, resolved at show time. */
    unsigned short crate_extra_key[8];
    unsigned char crate_extra_count;
    float xp;
    int level;
    /* run_data.gd:767-768 get_next_level_xp_needed: a binary64 threshold
     * (level + 4)^2 * (1 + next_level_xp_needed/100), NEVER 0 after reset.  It is
     * float because a profile carrying the next_level_xp_needed effect (Baby
     * +130, Mutant -66, Technomage +75) levels at a FRACTIONAL XP bound. */
    float xp_need;

    int wave, ticks, kills, waves_cleared, done, won, tick_in_wave, cleared_game;
    float time_left, spawn_timer, wave_duration, episode_return, dmg_taken;

    /* WaveTimer's own TickTimer (ui/hud/wave_timer.gd:14-17,31; its main.tscn
     * node carries NO properties, so wait_time 1.0 and one_shot false).  The
     * WaveTimer script starts it on the first frame the wave clock reads below
     * 6.0 s (and above 0.1 s), and every timeout runs
     * SoundManager.play(Utils.get_rand_element(tick_sounds), 0, 0.1, true).
     * `tick_sounds` is the scene's own one-clip export array
     * (ui/sounds/clock_tick_01.wav), NOT one of the sound arrays the bridge
     * empties for audio_rng == 0, so that Array.pick_random draw happens in BOTH
     * audio modes (exactly like the player's step_sounds pick).  Frames left
     * until the next timeout; 0 = the timer is stopped. */
    short tick_sound_left;

    /* --- wave-end window (main.gd:1178-1234 -> :1075-1133) -----------------
     * The WaveTimer timeout frame runs the whole end-of-wave sequence but the
     * run's shop/level-up transition (RunData.on_wave_end) only fires when the
     * 2.0 s EndWaveTimer expires, so obs[7] phase stays 0 for 120 more ticks
     * (measured: game trace wave timer 0 at tick 420, Shop at tick 540).  While
     * `end_wave_active` the players/enemies/weapons are already cleaned up
     * (clean_up_room ran in the expiry frame) and the player may still move.
     *   end_wave_timer  ticks left until on_wave_end (starts 120)
     *   harvest_timer   ticks left until the HarvestingTimer callback (0.66 s =
     *                   40 ticks; 0 = not armed) -- it GROWS/SHRINKS the
     *                   harvesting stat (main.gd:1464-1492); the gold/XP grant
     *                   happened in the expiry frame (manage_harvesting)
     *   label_timer     ticks left until the next WaveClearedLabel character
     *   label_chars     characters still to reveal (one sound draw each,
     *                   wave_cleared_label.gd:_on_Timer_timeout) */
    unsigned char end_wave_active;
    short end_wave_timer, harvest_timer, label_timer, label_chars;
    /* Pinned canonical camera configuration for the wave-end gold-bag flight
     * (main.gd:1064-1072 get_gold_bag_pos).  The bridge records the same pair in
     * the run meta; bt_core_reset seeds these from the BT_LOOT_BAG_* defaults and
     * the host may overwrite them (bt_loot_set_bag_view) so the simulation reads
     * one pinned configuration.  bag_x/y are derived per frame from these and the
     * camera-follow geometry.  Defaults: the retail project viewport
     * (project.godot:2665-2666), the probe's zoom (1,1) and the measured
     * UI/HUD/LifeContainerP1/UIBonusGold/Position2D anchor (see the
     * BT_LOOT_BAG_HUD_* comment); overridable so a different pinned pair can be
     * injected. */
#ifndef BT_LOOT_BAG_VIEW_W
#define BT_LOOT_BAG_VIEW_W 1920.0f
#endif
#ifndef BT_LOOT_BAG_VIEW_H
#define BT_LOOT_BAG_VIEW_H 1080.0f
#endif
#ifndef BT_LOOT_BAG_ZOOM
#define BT_LOOT_BAG_ZOOM 1.0f
#endif
#ifndef BT_LOOT_BAG_HUD_X
/* MEASURED (run_match41 probe, 2026-11-08): meta.environment.viewport_probe
 * reports visible_rect 1920x1080, canvas_origin (-64,-228), zoom (1,1),
 * camera (1024,768), ui_bonus_gold_pos (56,56) and gold_bag_pos (120,284) --
 * and (56,56) - (-64,-228) = (120,284), i.e. the recorded bag is exactly
 * camera + (hud - viewport/2).  The anchor is a static RectPosition: the HUD's
 * Life/XP/Gold rows are `visible = false` in main.tscn, so the VBoxContainer
 * lays UIBonusGold out at the container top and the Position2D's own (32,32)
 * (ui/hud/ui_bonus_gold.tscn:39-40) lands it at (56,56).  That reading is the
 * FIRST-ARENA row: the bridge re-measures the same probe at the wave-end window
 * frames per run (BridgeGaps), and if the anchor moves there the per-row values
 * drive bag_hud_x/y (bt_loot_set_bag_view) instead of this constant.  A
 * different pinned pair is injected with bt_loot_set_bag_view. */
#define BT_LOOT_BAG_HUD_X 56.0f
#endif
#ifndef BT_LOOT_BAG_HUD_Y
#define BT_LOOT_BAG_HUD_Y 56.0f
#endif
    float bag_view_w, bag_view_h, bag_zoom, bag_hud_x, bag_hud_y;
    /* Trace-replay hook for the bridge's debug knob `instant_waves`: a recorded
     * instant-wave run carries 1.0 s waves in its own wave_duration column, which
     * the generated table cannot express.  > 0 replaces every wave's generated
     * duration; 0 (every normal caller, bt_core_init clears it) keeps the table.
     * replay needs it applied before the wave-1 entry, so tools/brotato_core_trace.cpp
     * sets it after bt_core_init and re-runs bt_core_reset. */
    float wave_ovr;
    /* Endless mode (run_data.gd:196-197 is_endless_run, the run-options toggle).
     * 1 = the run continues past the wave-20 clear instead of ending; the core
     * then applies the endless stat/price/iframe scaling and the wave-10k elite
     * reschedule. Set from the `endless` [env] config by puf_init/puf_vec_create
     * (bt_core_init defaults it to 0). */
    unsigned char endless;
    /* Wave bt_core_reset enters (RunData.reset(): `current_wave =
     * DebugService.starting_wave`, run_data.gd:549 -- a plain index assignment
     * with no catch-up gold/items/weapons/level/XP and no draws for the skipped
     * waves). Set from the `start_wave` [env] config by puf_init/puf_vec_create
     * (bt_core_init defaults it to 1); bt_core_reset never clears it, like
     * `endless`, so it persists across episodes. */
    unsigned short start_wave;
    int endless_max_wave;   /* highest wave entered; latched into log.endless_wave */
    /* STICKY implementation-capacity latch (brotato_capacity.h).  bt_core_step
     * ORs every tripped capacity loss into this BEFORE its own episode-reset on
     * `done`, and bt_core_reset NEVER clears it, so a loss can never be hidden by
     * a same-tick death (a shot that overflows its identity set and dies in the
     * same tick) or by the end-of-episode auto-reset.  Only bt_core_init (a fresh
     * run configuration) and an explicit bt_capacity_clear() zero it. */
    unsigned long long capacity_latch;
    /* RunData.all_last_wave_bosses_killed (main.gd:491): on an endless run the
     * player cleared the wave-20 boss set, so a later total wipe counts as a run
     * WIN (main.gd:1048) instead of a loss. */
    unsigned char all_last_wave_bosses_killed;
    /* RunData.current_difficulty: the run's difficulty index (5 = Danger 5).
     * Read by the group gate (wave_manager.gd:181 `current_difficulty >=
     * group_data.min_difficulty`) and boss.gd:38-45's hp chain (whose
     * double_boss term is Items/difficulties/5/difficulty_5_effect_4 and whose
     * third clause needs `difficulty <= 0`).  Set by bt_core_init (5) and
     * deliberately NOT cleared by bt_core_reset, like `endless`. */
    unsigned char difficulty;

    /* wave group scheduling (BT_GROUPS spans via BT_ZONE_WAVES) */
    unsigned short live_group[BT_MAX_LIVE_GROUPS];  /* BT_GROUPS index per live slot */
    unsigned short live_mult[BT_MAX_LIVE_GROUPS];   /* owned multiplicity (extra groups) */
    unsigned char group_fired[BT_MAX_LIVE_GROUPS];
    unsigned char group_repeating[BT_MAX_LIVE_GROUPS]; /* remaining repeat count (0 = one-shot) */
    float group_interval[BT_MAX_LIVE_GROUPS];       /* accelerated repeating interval */
    float group_next_elapsed[BT_MAX_LIVE_GROUPS];   /* wave-elapsed time of next fire */
    int n_live_groups;
    /* ENDLESS generated groups (zone_service.gd:40-92).  A live slot whose
     * lg_dynamic flag is set is built at wave start by bt_build_endless_groups
     * instead of pointing at a BT_GROUPS row; its get_additional_groups fields
     * live here (timing / edge flag / area / interval / unit kind / counts are
     * the per-copy values that override BT_ENDLESS_GROUP_TMPL and
     * BT_ENDLESS_UNIT_TMPL).  lg_area is the source's `int(rand_range(200,300))`
     * or -1. */
    unsigned char lg_dynamic[BT_MAX_LIVE_GROUPS];
    unsigned short lg_timing[BT_MAX_LIVE_GROUPS];   /* spawn_timing (s) */
    float lg_area[BT_MAX_LIVE_GROUPS];              /* -1 = anywhere */
    float lg_interval[BT_MAX_LIVE_GROUPS];          /* repeating_interval */
    unsigned char lg_edge[BT_MAX_LIVE_GROUPS];      /* spawn_edge_of_map */
    unsigned short lg_kind[BT_MAX_LIVE_GROUPS];     /* BT_KINDS index */
    unsigned char lg_minn[BT_MAX_LIVE_GROUPS], lg_maxn[BT_MAX_LIVE_GROUPS];
    /* entity_spawner.gd:152 + zone_service.gd:48-49: this wave's plain-enemy cap
     * (100 for the authored waves, 135..144 for the generated endless ones). */
    unsigned short wave_max_enemies;
    /* 1 = a generated group did not fit BT_MAX_LIVE_GROUPS (loud, never silent:
     * the supported endless bound is sorted in the generated header). */
    unsigned char group_overflow;
    /* 1 = bt_reserve_slot had to evict a live enemy for want of a free slot.
     * A legal run never reaches this: bt_group_perf_cull keeps the plain list at
     * or below wave_max_enemies and BT_MAX leaves room for every other list. */
    unsigned char pool_cull_fallback;
    /* 1 = the slot above was taken by a BOSS/ELITE spawn.  The source has no
     * slot ceiling at all, so an elite forced to evict a plain enemy means the
     * inline arena is out of capacity (implementation capacity loss, distinct
     * from the plain-list cull shape): the endless extra-elite load is
     * 1 + ceil((w-20)/10) per event, so it happens once the concurrent elite
     * count exceeds the BT_MAX headroom (28 slots).  Loud, and the capacity
     * contract maps it to its own BT_CAP bit. */
    unsigned char elite_slot_pressure;
    /* Per-queue drop counters (loud diagnostics beside pend_overflow): how many
     * enqueues each category had to reject because its queue was full
     * (BT_MAX_PENDING / BT_MAX_BIRTHS).  Reset with pend_overflow each wave. */
    unsigned short pend_drop_enemy, pend_drop_tree, pend_drop_boss;
    unsigned short pend_drop_summons, pend_drop_birth;
    /* Same, for the other two pending pools and the deferred item-box grant
     * (bt_queue_struct / bt_queue_pet / the level-up box slot path), so a
     * pend_overflow flip always names its site. */
    unsigned short pend_drop_struct, pend_drop_pet, pend_drop_cons;
    /* The three remaining pend_overflow sites outside the spawn queues: the
     * material pool (BT_MAX_MAT), a picked-up item box (BT_MAX_PENDING), and
     * the extra-in-crate key list (its own cap). */
    unsigned short pend_drop_mat, pend_drop_box, pend_drop_crate;
    /* Per-site GLOBAL (Math::default_rand) draw counters, cumulative and never
     * reset (zero on a fresh BtState) so a harness can take per-tick deltas and
     * localise an unexpected +3 (one rand_range) in the spawn/birth frames.
     * Unit: global PCG outputs, i.e. 3 per rand_range/randd, 1 per randf.
     *   [0] bt_spawn_poll's Utils.shuffled_range(4) shuffle  (3 per poll frame)
     *   [1] the per-birth pop rotation entity_birth.gd:47  (3 per popped birth)
     *   [2] a finished birth's overlap relocation, entity_birth.gd:102 via
     *       ZoneService.get_rand_pos() -> bt_rand_pos            (6 per event)
     *   [3] the enemy-init op draws (unit.gd:77 / enemy.gd:25 / movement and
     *       attack get_cd ops) — 3 per executed op, 0 for the COND_SHOTS
     *       long-cooldown branch that samples nothing.  Where this lands
     *       (activation frame vs pop frame) is the question it answers.
     * Sites that draw nothing here (bt_birth_audio_enqueue, the positional
     * audio pitch) are deliberately unwired: if the four deltas do not account
     * for a frame's global draws, the residual is that site. */
    unsigned int draw_site[4];
    /* entity_spawner.gd category queues + entity_birth.gd countdown. Drained in
     * source order (trees, bosses, summons, then ordinary x nb_to_spawn) with
     * pop_back (LIFO) per queue, one pass every BT_SPAWN_DELAY frames. */
    BtPending pend_enemy[BT_MAX_PEND_ENEMY];
    BtPending pend_tree[BT_MAX_PEND_SMALL];
    BtPending pend_boss[BT_MAX_PEND_SMALL];
    BtPending pend_summons[BT_MAX_PEND_SMALL];
    int n_pend_enemy, n_pend_tree, n_pend_boss, n_pend_summons;
    unsigned char pend_overflow;    /* 1 = an enqueue was rejected (never silent) */
    unsigned char pr_overflow;      /* 1 = the projectile pool was full (loud) */
    unsigned int pr_dropped;   /* total dropped spawns this episode (counter to pr_overflow) */
    unsigned int pr_cre_next;  /* monotone projectile-creation ordinal (per episode) */
    BtBirth births[BT_MAX_BIRTHS];
    int n_births;
    /* SoundManager2D: 12 players, FIFO of at most MAX_SOUNDS (16).
     * Birth, healer and weapon-hit audio share SoundManager2D's pool and RNG. */
    float birth_audio_busy[12], birth_audio_dur[16];
    bt_u64 birth_audio_pitch[16];
    unsigned char birth_audio_head, birth_audio_count;
    /* SoundManager is shared by footsteps and weapon shooting sounds. */
    float main_audio_busy[12], main_audio_dur[16];
    bt_u64 main_audio_pitch[16];
    unsigned char main_audio_head, main_audio_count;
    int cur_spawn_delay;            /* entity_spawner.gd cur_spawn_delay */
    /* --- source enemy-instance pool (main.gd:56-61,1649-1699 `_pool`) --------
     * entity_spawner.gd:346-420 pops a dead node for the SAME scene and calls
     * respawn() (LIFO: main.gd get_node_from_pool pop_back), else builds a fresh
     * scene.instance()+init(). The pool is keyed by scene == BT_KINDS row, so
     * instances never cross kinds. Main is rebuilt every wave (main.gd:1304 /
     * ui/menus/shop/base_shop.gd:346) and Main._exit_tree queue_frees the pool
     * (main.gd:1727-1737), so bt_enemy_pool_reset() (from bt_enter_wave) empties
     * everything.
     *   pool_free_head[k]      LIFO top of kind k's reusable ids (NO_INST = empty).
     *   pool_free_n[k]         reusable instance count of kind k.
     *   pool_next[id]          intrusive link: free-stack next or delay-bucket next.
     *   pool_kind[id]          owning kind, fixed for the instance's life.
     *   pool_val[id][o]        the instance's retained init values, exact binary64
     *                          bit patterns indexed by the kind's init program in
     *                          order (fresh row i at [i], reuse row j at
     *                          [BT_KIND_INIT_FRESH_N[kind] + j]); the reuse half
     *                          never aliases the fresh half, so a value drawn on
     *                          the fresh branch (_actual, _phase, _distance, ...)
     *                          stays live across a pooled respawn. Dimensioned
     *                          BT_MAX_KIND_INIT_OPS (the widest program).
     *   pool_dly_head/tail[r]  FIFO of instances released into ring slot r whose
     *                          1.0 s death animation is unfinished; step() matures
     *                          slot r exactly BT_POOL_DEATH_FRAMES ticks after the
     *                          release by pushing its FIFO, oldest first, onto the
     *                          kind free stack -- so the LAST released is reused first.
     *   pool_alloc             next never-used instance id (high-water mark).
     *   pool_np_free           recycled boss/elite descriptor head (boss.gd:81
     *                          queue_free); a boss/elite acquire pops one but is
     *                          ALWAYS reported fresh, never a respawn.
     *   pool_pos               release() writes this ring slot; step() advances it,
     *                          then matures the new slot.
     *   pool_overflow          1 = the BT_POOL_INST_MAX id space was exhausted
     *                          (explicit, never a silent drop). */
    unsigned short pool_free_head[BT_NKINDS];
    unsigned short pool_free_n[BT_NKINDS];
    unsigned short pool_next[BT_POOL_ID_CAP];
    unsigned char pool_kind[BT_POOL_ID_CAP];
    unsigned char pool_shots[BT_POOL_ID_CAP]; /* retained AttackBehavior._shots_taken at death */
    /* Fixed-stride per-instance init values. Allocation is monotonically by id
     * and every run has BT_MAX_KIND_INIT_OPS words, so its offset is exactly
     * id*BT_MAX_KIND_INIT_OPS. Retain the dense table's placement (including
     * previous-wave bits) without redundant offset/stride/used metadata or a
     * dependent memory read on every init-value lookup. The scratch run at
     * BT_POOL_VAL_WORDS absorbs a failed allocation; pool_val_overflow stays loud. */
    bt_u64 pool_val[BT_POOL_VAL_WORDS + BT_POOL_VAL_SCRATCH];
    /* Root MovementBehavior belongs to the pooled instance. Only Boss subclasses
     * have state movement nodes (BT_NKIND_STATES; boss.gd state_scenes), and
     * boss.gd:81 queue_frees rather than pooling. Their descriptors are recycled
     * by pool_np_free, so at most BT_MAX distinct boss descriptors exist: each
     * new descriptor requires a live enemy slot, and a freed one is reused first.
     * Keep all BT_MAX_STATES nodes for each such descriptor, not for every normal
     * pooled enemy. No float changes precision and no source capacity is reduced. */
    float pool_mvtx[BT_POOL_ID_CAP], pool_mvty[BT_POOL_ID_CAP];
    float pool_mvtime[BT_POOL_ID_CAP];
    unsigned char pool_mvflag[BT_POOL_ID_CAP];
    unsigned short pool_mv_state[BT_POOL_ID_CAP], pool_mv_state_n;
    float boss_mvtx[BT_MAX][BT_MAX_STATES], boss_mvty[BT_MAX][BT_MAX_STATES];
    float boss_mvtime[BT_MAX][BT_MAX_STATES];
    unsigned char boss_mvflag[BT_MAX][BT_MAX_STATES];
    /* iron_lung.gd:6,20,34-42 IronLung.is_full: a node member, so per pool
     * instance; false at spawn, set when a stargazer enters DetectionZone. */
    unsigned char pool_lungfull[BT_POOL_ID_CAP];
    float ebonus[BT_MAX];
    unsigned short pool_dly_head[BT_POOL_DEATH_FRAMES];
    unsigned short pool_dly_tail[BT_POOL_DEATH_FRAMES];
    unsigned short pool_alloc;
    unsigned short pool_np_free;
    int pool_pos;
    unsigned char pool_overflow;
    unsigned char pool_val_overflow;  /* ragged init-value pool exhausted (loud) */
    /* item ExtraEnemyEffect groups pending for the NEXT wave (wave_manager.gd:114-121) */
    /* Owned multiplicity per CANONICAL effect row (BT_ITEM_EFF_ROWS) for the two
     * row-indexed mechanics -- exact, no cap, nothing dropped. All row data
     * (key/aux_key/value/value2/custom_key) is read back from BT_ITEM_EFF_ROWS
     * at the consumer, honouring StorageMethod/SumStrategy merges. */
    unsigned short exg_count[BT_NITEM_EFF_ROWS];   /* ExtraEnemyEffect rows */
    unsigned short bk_count[BT_NITEM_EFF_ROWS];    /* bucket rows (DOUBLE_*) */
    /* Per OWNED VARIANT of the rows whose values are read back later (Double
     * Value/KeyValue buckets, ExtraEnemy multiplicity, stat-link rows). Each
     * owned copy registers its own (value,value2) tuple, so mixed cursed /
     * uncursed copies and different curse rolls each keep their own numbers. */
    /* Ragged per-row variant storage.  A row that owns variants keeps ONE
     * contiguous run in the shared pool: rvar_off[row] is its start and
     * rvar_cap[row] its capacity (0 = no run yet); nvar[row] is the live count.
     * The dense [903][102] tables were 99.99% empty (measured: <=3 live variants
     * over long runs) at 1.29 MiB, while the bump pool below is a few tens of KB.
     * The per-row CAP stays the source bound BT_MAX_VARIANTS (101 cursed
     * percentages + uncursed); a row that needs more, or a pool that runs out,
     * sets curse_overflow -- the existing LOUD flag, never a silent drop. */
    unsigned char nvar[BT_NITEM_EFF_ROWS];
    unsigned short rvar_off[BT_NITEM_EFF_ROWS];  /* run start in rvar[]    */
    unsigned char rvar_cap[BT_NITEM_EFF_ROWS];   /* run capacity (0 = none) */
    unsigned short rvar_used;                    /* bump high-water in rvar[] */
    BtRowVariant rvar[BT_RVAR_POOL];
    unsigned char curse_overflow;   /* 1 = a variant was rejected (never silent) */
    /* per-variant application counter for consumable_stats_while_max */
    unsigned short var_cs[BT_RVAR_POOL];
    /* canonical row indices with count>0, appended once on first ownership; the
     * hot paths iterate only these (data-bounded by BT_NITEM_EFF_ROWS). */
    unsigned short exg_active[BT_NITEM_EFF_ROWS];
    unsigned short bk_active[BT_NITEM_EFF_ROWS];
    int n_exg_active, n_bk_active;
    /* #2 consumable_stats_while_max: applied counter per canonical row */
    unsigned short cs_count[BT_NITEM_EFF_ROWS];
    /* #9/#10 decaying_stats_on_consumable(0)/on_hit(1): temp stat + value for secs */
    unsigned char dt_id[8], dt_onhit[8];
    float dt_val[8], dt_remain[8];
    int n_dt;
    /* curse (DLC): tracked stat + per-offer cursed flag/value scale */
    float curse;
    unsigned char shop_cursed[4];
    unsigned char curse_apply;   /* 1 while applying a cursed item */
    float curse_maxmod;          /* max per-effect modifier (dlc_1_data.gd:345) */
    /* pet geometry from BT_ITEM_SCENES */
    float pet_body_r[BT_MAX_PET], pet_proj_r[BT_MAX_PET];
    unsigned char is_horde_wave;

    /* elite/horde schedule (run_data.gd:init_elites_spawn) */
    unsigned char elite_wave[BT_ELITE_EVENTS];   /* scheduled wave per event */
    unsigned char elite_type[BT_ELITE_EVENTS];   /* 0 elite, 1 horde */
    unsigned short elite_kind[BT_ELITE_EVENTS];
    unsigned char elite_fired[BT_ELITE_EVENTS];
    unsigned short boss_kind[2];   /* Danger-5 boss picks, in source selection order */
    /* Endless elite reschedule (main.gd:931-933 clean_up_room calls
     * RunData.init_elites_spawn(current_wave + 10, 0.0) every 10th wave at wave
     * >= 10).  The game APPENDS to its growing elites_spawn list, so up to two
     * decades of events are outstanding at once (window base+1..base+8, bases
     * 10 apart, non-overlapping but scheduled a decade ahead).  Dead entries are
     * compacted out on each new schedule: fired ones AND any whose wave has
     * already passed (oe_wave <= the wave that just ended), so a scheduled wave
     * that was never visited (a skipped/jumped wave, or the oe_wave 255 clamp)
     * cannot accumulate.  With that predicate the live entries are exactly those
     * scheduled ahead of the current wave, so BT_ENDLESS_ELITE_SLOTS (two live
     * decades = 2 * BT_ELITE_EVENTS) is provably always enough; the guard below
     * is a defensive backstop (oe_overflow -- explicit, never a silent drop). */
    unsigned char oe_wave[BT_ENDLESS_ELITE_SLOTS];
    unsigned char oe_type[BT_ENDLESS_ELITE_SLOTS];
    unsigned short oe_kind[BT_ENDLESS_ELITE_SLOTS];
    unsigned char oe_fired[BT_ENDLESS_ELITE_SLOTS];
    unsigned char oe_n;
    unsigned char oe_overflow;
    /* run_data.gd:init_elites_spawn guards on check_elite_generation: a base_wave
     * already generated returns immediately (no draws, no append).  A wave replay
     * re-ends the same multiple-of-10 wave, so the guard stops a duplicate
     * schedule (which would also burn the source's draws a second time). */
    int oe_last_base;

    float player_speed, danger_hp, danger_dmg, spawn_rate;

    /* Persistent GodotPhysics contact world: enemies 0..BT_MAX-1, walls after.
     * One per env; never reallocated. ~1.03 MiB. */
    BtPhysWorld phys;
    /* Second, disjoint GodotPhysics world for pets (layer 512 / mask 640 vs the
     * enemy world's 4/132; no shared feedback), so pets resolve against walls
     * without inflating the enemy cache. bt_pet_world_init(s) fills it. */
    BtPhysWorld phys_pet;
    /* Potential-based shaping configuration (brotato_reward.h). Last field on
     * purpose: CUDA/Metal state layouts are prefix-stable. */
    struct BtReward rew;

    /* ---- death-animation clock (bt_enemy_dying_step) -----------------------
     * Appended after rew for the same reason rew sits last: every pre-existing
     * offset stays put and the layout only GROWS, so a CUDA/Metal image built
     * against the older size still has all of its fields at the same offsets.
     *
     * Entity.die() rewinds the node to entity.tscn's `death` clip (1.0 s,
     * method key at t=1.0) at playback_speed 1.  AnimationPlayer defaults to
     * playback_process_mode = IDLE, so the position advances once per idle frame
     * by playback_speed * delta in float32 (scene/animation/animation_player.cpp
     * `PlaybackData::pos` is a `float`), and the key fires on the first frame
     * whose advance reaches the clip length; that frame's deferred call runs
     * death_animation_finished -> free_entity/add_node_to_pool, so the node
     * leaves the tree in that same idle pass.  A dying unit's behavior Timers
     * are CHILD nodes and keep firing, and charging_attack_behavior.gd:124-127
     * re-writes playback_speed to _idle_playback_speed -- which SHORTENS the
     * window (recorded in the deep-start captures).
     *   edpos = that clip position (float32, exactly the AnimationPlayer's)
     *   edspd = the clip's live playback_speed (1.0 from die(), then edidl)
     *   edidl = _idle_playback_speed (enemy.gd:25 rand_range(1,3), drawn once
     *           per NODE), sampled at the kill while the slot's pool ledger row
     *           is still resolvable
     * See bt_enemy_dying_step for the frame accounting. */
    float edpos[BT_MAX];
    float edspd[BT_MAX];
    float edidl[BT_MAX];
} BtState;

/* Keep membership and the source-compatible byte flags together. Callers that
 * hand-build fixtures must use these setters too. Bounds retain written dead
 * slots so reset still clears every touched per-slot field. */
BT_HD static unsigned char bt_enemy_set_alive(BT_DEVICE BtState* s, int slot,
                                              unsigned char value) {
    unsigned int bit = 1u << (slot & 31);
    if (value) s->e_live_bits[slot >> 5] |= bit;
    else s->e_live_bits[slot >> 5] &= ~bit;
    if (slot >= (int)s->e_used) s->e_used = (unsigned short)(slot + 1);
    return s->ealive[slot] = value;
}
BT_HD static unsigned char bt_enemy_set_dying(BT_DEVICE BtState* s, int slot,
                                              unsigned char value) {
    unsigned int bit = 1u << (slot & 31);
    if (value) s->e_dying_bits[slot >> 5] |= bit;
    else s->e_dying_bits[slot >> 5] &= ~bit;
    if (slot >= (int)s->e_used) s->e_used = (unsigned short)(slot + 1);
    return s->edying[slot] = value;
}
/* modes: 1 = live, 2 = dying, 3 = scene-tree nodes (observation). Re-read the
 * current word after each slot: callbacks can spawn/kill a later slot while a
 * scan is running, and that must remain visible in this same ordered pass. */
BT_HD static int bt_enemy_next(BT_DEVICE const BtState* s, int start, int mode) {
    int word = start >> 5;
    int words = ((int)s->e_used + 31) >> 5;
    unsigned int keep = 0xFFFFFFFFu << (start & 31);
    for (; word < words; word++, keep = 0xFFFFFFFFu) {
        unsigned int bits = 0u;
        if (mode & 1) bits = s->e_live_bits[word];
        if (mode & 2) bits |= s->e_dying_bits[word];
        bits &= keep;
        if (bits) {
#if defined(__METAL_VERSION__)
            int first = (int)metal::ctz(bits);
#elif defined(__CUDA_ARCH__)
            int first = __ffs((int)bits) - 1;
#elif defined(__GNUC__) || defined(__clang__)
            int first = __builtin_ctz(bits);
#else
            int first = 31 - (int)bt_rng_clz32(bits & (0u - bits));
#endif
            return (word << 5) + first;
        }
    }
    return s->e_used;
}

/* Root state is instance-retained; boss state nodes use the descriptor's
 * separately allocated bank. pool_mv_state is stable across descriptor reuse. */
BT_HD static inline BT_DEVICE float* bt_pool_mvtx(BT_DEVICE BtState* s, unsigned short id, int node) {
    return node == BT_MAX_STATES ? &s->pool_mvtx[id] : &s->boss_mvtx[s->pool_mv_state[id]][node];
}
BT_HD static inline BT_DEVICE float* bt_pool_mvty(BT_DEVICE BtState* s, unsigned short id, int node) {
    return node == BT_MAX_STATES ? &s->pool_mvty[id] : &s->boss_mvty[s->pool_mv_state[id]][node];
}
BT_HD static inline BT_DEVICE float* bt_pool_mvtime(BT_DEVICE BtState* s, unsigned short id, int node) {
    return node == BT_MAX_STATES ? &s->pool_mvtime[id] : &s->boss_mvtime[s->pool_mv_state[id]][node];
}
BT_HD static inline BT_DEVICE unsigned char* bt_pool_mvflag(BT_DEVICE BtState* s, unsigned short id, int node) {
    return node == BT_MAX_STATES ? &s->pool_mvflag[id] : &s->boss_mvflag[s->pool_mv_state[id]][node];
}

/* --- lvl_offers[] / lvl_prev[] encoding ------------------------------------
 * A level-up slot stores its BT_UPGRADES row as `row + 1` so that 0 is the
 * unambiguous "no offer" marker; the empty-observation encoding can no longer
 * collide with the REAL BT_UPGRADES[0] row (upgrade_max_hp_1, stat 0 value 3.0).
 * BT_UPGRADE_SLOT_OFFER stays the one offer outside the table
 * (brotato_progression.h roll_upgrade), and BT_UPGRADE_NONE is no longer
 * written by the progression path.
 *   bt_lvl_offer_row(off)   -> BT_UPGRADES row, or -1 when the slot carries no
 *                              table row (absent, the weapon-slot sentinel, or
 *                              an out-of-range value).
 *   bt_lvl_offer_present()  -> 1 for any real offer (a table row or the
 *                              sentinel), 0 for an absent slot. */
BT_HD static int bt_lvl_offer_row(unsigned short off) {
    if (off == 0 || off == (unsigned short)BT_UPGRADE_SLOT_OFFER) return -1;
    if (off > (unsigned short)BT_NUPGRADES) return -1;
    return (int)off - 1;
}
BT_HD static int bt_lvl_offer_present(BT_DEVICE BtState* s, int slot) {
    unsigned short off = s->lvl_offers[slot];
    if (off == (unsigned short)BT_UPGRADE_SLOT_OFFER) return 1;
    return off != 0 && off <= (unsigned short)BT_NUPGRADES;
}

/* --- ragged pool_val accessor (see pool_val[] in BtState) -------------------
 * bt_pool_val(s,id) is instance `id`'s init-value run (BT_MAX_KIND_INIT_OPS
 * binary64 words, the dense table's stride). */
BT_HD static inline BT_DEVICE bt_u64* bt_pool_val(BT_DEVICE BtState* s, unsigned short id) {
    /* id >= BT_POOL_ID_CAP cannot come from bt_enemy_pool_acquire (it rejects
     * such an id and the caller returns early), but a defensive clamp keeps a
     * stray id on the scratch run instead of outside the array. */
    unsigned int off = (unsigned int)id * (unsigned int)BT_MAX_KIND_INIT_OPS;
    if (id >= (unsigned short)BT_POOL_ID_CAP ||
        off + BT_MAX_KIND_INIT_OPS > (unsigned int)BT_POOL_VAL_WORDS)
        off = (unsigned int)BT_POOL_VAL_WORDS;
    return &s->pool_val[off];
}

/* --- ragged variant accessors (see the rvar[] comment in BtState) ----------
 * bt_rvar(s,row) is the row's first BtRowVariant and bt_varcs(s,row) its first
 * per-variant counter; both are valid for i < nvar[row]. */
BT_HD static inline BT_DEVICE BtRowVariant* bt_rvar(BT_DEVICE BtState* s, int row) {
    return &s->rvar[s->rvar_off[row]];
}
BT_HD static inline BT_DEVICE unsigned short* bt_varcs(BT_DEVICE BtState* s, int row) {
    return &s->var_cs[s->rvar_off[row]];
}