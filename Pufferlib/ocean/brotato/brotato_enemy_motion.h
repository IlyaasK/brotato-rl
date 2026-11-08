/* brotato_enemy_motion.h -- enemy MovementBehavior dispatch for the shared core.
 *
 * Included by brotato_core.h just before bt_core_step, AFTER brotato_enemy_init.h
 * (bt_enemy_init_find) and brotato_enemy_pool.h, so every helper it calls
 * (bt_rand_range, bt_math_randf, bt_rng_bounded, bt_clampf, bt_sqrtf,
 * bt_enemy_init_find, bt_kgeom ...) is already defined.  It only reads and
 * writes BtState; nothing allocates and nothing touches libc/stdio.
 *
 * SOURCE
 *   entities/units/movement_behaviors/*.gd, one .tscn per enemy kind, and the
 *   boss/elite States/StateN/MovementBehavior chain.
 *
 *   * unit.gd:214-215 get_movement() = _current_movement_behavior.get_movement();
 *     get_move_input() = current_movement.normalized() * get_move_speed(), with
 *     get_move_speed() = max(0, current_stats.speed + bonus_speed +
 *     decaying_bonus_speed) (unit.gd:218-224).  The core feeds the returned
 *     vector into bt_move_input (normalize * speed) exactly as before, so this
 *     module only has to produce the behaviour's raw target-minus-self vector.
 *   * enemy.gd:88-97 _physics_process picks current_target from the targetable
 *     pets + players every 0.25 s; the core models the player as the only
 *     target (its existing chase used player - self), so every
 *     `current_target.global_position` below is (s->px, s->py).
 *     Godot dispatches the script virtual multilevel, so Unit._physics_process
 *     (the integration) runs after Enemy._physics_process, and the latter's
 *     is_being_knocked_back probe (enemy.gd:100) calls get_move_input() too:
 *     the source asks for get_movement() twice per frame while the core asks
 *     once.  That is identical unless a redraw lands the new target within
 *     EQUALITY_PRECISION of the unit, when the source would draw again.
 *   * enemy.gd:99 _current_attack_cd and the attack branch are untouched:
 *     the movement vector is what Unit.get_movement() returns, which is also
 *     what a ShootingAttackBehavior's shoot_in_unit_direction case reads
 *     (shooting_attack_behavior.gd:98-99) -- previously that case received the
 *     raw chase vector.
 *   * entity.gd:34-43 _min_pos/_max_pos = zone rect inset by half the
 *     Animation/Sprite texture; every *_rand_pos behaviour samples and clamps
 *     against those bounds, so the generator resolves the per-kind half extents
 *     (BT_KIND_HALF_W/H) from the scene textures.
 *   * The retained init draws (stay_in_range `_actual_target_range`,
 *     follow_rand `_actual`/`_distance_from_player`, patrol_edges `_actual`,
 *     target_rand_around `_actual`) are read back from BT_ENEMY_INIT_OPS via
 *     bt_enemy_init_find at the SAME state ordinal the behaviour node was
 *     initialised under, so a fresh-vs-reuse replay never re-draws them.
 *
 * PER-INSTANCE STATE
 *   Each MovementBehavior node keeps its own `_current_target` /
 *   `current_check_update` / `_detected_player`.  A pooled enemy reuses its node
 *   instances (Enemy.respawn only re-points _current_movement_behavior), so the
 *   state must SURVIVE a respawn -- and it is keyed by the POOL INSTANCE, not
 *   the live slot (bt_enemy_pool_acquire may hand the same instance to a
 *   different slot).  `bonus_speed` is a Unit member, so it is per live slot and
 *   Unit.respawn (unit.gd:85) resets it to 0 on every spawn.
 *   detect_target's two child nodes have their own `_current_target` in source;
 *   detect_target is only ever the kind ROOT (anglerfish) and only its
 *   before-detection child (target_rand_pos) keeps a target, so the active child
 *   shares the detect node's slot -- exact for the kinds the generator finds.
 *
 * SCOPE OF THE SPLIT
 *   * MovementBehaviour.get_movement() is dispatched for every behaviour any
 *     enemy kind/state reaches: follow_target, stay_in_range_from_player,
 *     follow_rand_pos_around_player, target_rand_pos (+ the DLC stargazer
 *     subclass), target_rand_pos_around_player, patrol_edges_of_map,
 *     detect_target, go_towards_map_center, target_rand_gold_pos.
 *   * movement_behavior (base), patrol_around_the_map, copy_nearby_entities and
 *     player_movement_behavior are implemented as their source returns
 *     Vector2.ZERO only where a kind could reach them: NO enemy kind/state
 *     instantiates them (extract_movement.py asserts the resolved behaviour set
 *     is a subset of the dispatched one), so BT_INITBEH_MOV_BASE/PATROL_AROUND_MAP/
 *     COPY_NEARBY/PLAYER fall through to zero movement and are unreachable.
 *   * charm (dlc_1 charm_enemy_effect_behavior.gd): bt_mov_beh targets the
 *     nearest other enemy for a charmed unit (:139-147) and swaps a charmed
 *     stay_in_range / follow_rand_pos_around_player root to target_rand_pos
 *     (:120-124); the charm state, stats, speed bonus and timer live in
 *     brotato_enemy_lifecycle.h.  The collision-layer swaps, the disabled
 *     Hurtbox, the pet-projectile layer and the Healer/Buffer BoostZone mask
 *     retarget are combat, and the caller gates them on bt_enemy_charmed() /
 *     bt_enemy_charm_absorb().
 *   * can_be_boosted: bt_enemy_can_boost() is the guard a boost must apply
 *     (buffer.gd:46, Enemy.boost enemy.gd:311-312); it folds in the DLC
 *     stargazer's angry override.
 *   * follow_target's stop_close_to_target branch is inert for enemies: the
 *     enemy scenes carry no TargetBehavior node, so FollowTargetMovementBehavior
 *     `_on_TargetBehavior_*` never fires and `_target_player` stays false
 *     (follow_target_movement_behavior.gd:9-16,23,27).
 *   * target_rand_gold_pos resolves `Utils.get_rand_element(main._active_golds)`
 *     over the live material slots (s->matalive) in slot order.  main.gd:737/797
 *     keeps `_active_golds` as an INSERTION-ordered array while the core's gold
 *     pool is first-free-slot, so the chosen index maps onto the slot order, not
 *     the source insertion order.  The draw count/value is identical; the picked
 *     gold's position matches while no gold has been picked up.  Reproducing the
 *     source list order needs an ordered gold list in BtState (prerequisite).
 *   * stargazer_movement_behavior's lung link (add_lung), the OnTriggered
 *     follow_target "angry" switch and IronLung.is_full need the spawner owner
 *     (entity_spawner.gd "enemy_respawned" + IronLung.DetectionZone) the core
 *     does not track; with no linked lung the source itself falls back to
 *     TargetRandPosMovementBehavior.get_new_target(), which is what runs here.
 */
#pragma once
#include "brotato_state.h"

/* MovementBehavior.EQUALITY_PRECISION (movement_behavior.gd:5). */
#define BT_MOV_PREC 10.0f

/* ===================== BEGIN GENERATED =====================
 * extract_movement.py -- DO NOT EDIT below; regenerate with
 *   python3 ocean/brotato/extract_movement.py \
 *       --campaign-root <recovered> --dlc-root <abyss> --write <this file>
 *
 * BT_MOV_ROOT / BT_MOV_STATE carry only the movement-runtime fields of
 * each scene's MovementBehavior chain; the init draws those behaviours
 * sample live in BT_ENEMY_INIT_OPS (brotato_waves.h), keyed by the same
 * state ordinal.  BT_KIND_HALF_W/H are the Animation/Sprite texture
 * half extents entity.gd:34-43 folds into _min_pos/_max_pos.
 * =========================================================== */
#define BT_MOVFK_ALLOW_WITHIN 1u

/* BT_KINDS rows whose scene root carries a script the movement/lifecycle
 * slice must special-case (stargazer.gd, evil_mob.gd, iron_lung.gd). */
#define BT_MOVK_STARGAZER 1u
#define BT_MOVK_EVIL_MOB  2u
#define BT_MOVK_IRON_LUNG 4u
#define BT_MOVK_HEALER    8u   /* scene root is healer.gd */
#define BT_MOVK_BUFFER   16u   /* scene root is buffer.gd */
BT_TABLE unsigned char BT_KIND_MOVK[BT_NKINDS] = {
    0u,  /* anemone */
    0u,  /* anglerfish */
    0u,  /* blobfish */
    0u,  /* clam */
    0u,  /* colossal_squid */
    0u,  /* crab */
    0u,  /* diplocaulus */
    0u,  /* dragonfish */
    2u,  /* evil_mob_swimmer */
    0u,  /* goblin_shark */
    8u,  /* hermit */
    4u,  /* iron_lung */
    0u,  /* lobster */
    0u,  /* looting_pig */
    0u,  /* narwhal */
    0u,  /* plankton */
    0u,  /* pufferfish */
    0u,  /* sea_pig */
    0u,  /* shrimp */
    1u,  /* stargazer */
    16u,  /* stonefish */
    0u,  /* vampire_squid */
    0u,  /* viperfish */
    0u,  /* walrus */
    0u,  /* baby_alien */
    0u,  /* bruiser */
    16u,  /* buffer */
    0u,  /* charger */
    0u,  /* chaser */
    2u,  /* evil_mob */
    0u,  /* fin_alien */
    0u,  /* fly */
    8u,  /* healer */
    0u,  /* helmet_alien */
    0u,  /* horned_bruiser */
    0u,  /* horned_charger */
    0u,  /* junkie */
    0u,  /* lamprey */
    0u,  /* looter */
    0u,  /* pursuer */
    0u,  /* slasher */
    0u,  /* slasher_egg */
    0u,  /* spawner */
    0u,  /* spitter */
    0u,  /* tentacle */
    0u,  /* bat */
    0u,  /* giant */
    0u,  /* giant_isopod */
    0u,  /* impaled_worm */
    0u,  /* jellyfish */
    0u,  /* megalodon */
    0u,  /* prisoner */
    0u,  /* spider_crab */
    0u,  /* turtle */
    0u,  /* butcher */
    0u,  /* colossus */
    0u,  /* croc */
    0u,  /* gargoyle */
    0u,  /* mantis */
    0u,  /* mom */
    0u,  /* monk */
    0u,  /* rhino */
    0u,  /* dead_whale */
    0u,  /* eel */
    0u,  /* invoker */
    0u,  /* predator */
};

/* evil_mob.gd exports: decrease_speed_by_evolve (bonus_speed = -(dec*evo))
 * and quantity_to_evolve (the gold_count thresholds). */
BT_TABLE unsigned char BT_KIND_MOB_DECREASE[BT_NKINDS] = {
    0u,  /* anemone */
    0u,  /* anglerfish */
    0u,  /* blobfish */
    0u,  /* clam */
    0u,  /* colossal_squid */
    0u,  /* crab */
    0u,  /* diplocaulus */
    0u,  /* dragonfish */
    125u,  /* evil_mob_swimmer */
    0u,  /* goblin_shark */
    0u,  /* hermit */
    0u,  /* iron_lung */
    0u,  /* lobster */
    0u,  /* looting_pig */
    0u,  /* narwhal */
    0u,  /* plankton */
    0u,  /* pufferfish */
    0u,  /* sea_pig */
    0u,  /* shrimp */
    0u,  /* stargazer */
    0u,  /* stonefish */
    0u,  /* vampire_squid */
    0u,  /* viperfish */
    0u,  /* walrus */
    0u,  /* baby_alien */
    0u,  /* bruiser */
    0u,  /* buffer */
    0u,  /* charger */
    0u,  /* chaser */
    125u,  /* evil_mob */
    0u,  /* fin_alien */
    0u,  /* fly */
    0u,  /* healer */
    0u,  /* helmet_alien */
    0u,  /* horned_bruiser */
    0u,  /* horned_charger */
    0u,  /* junkie */
    0u,  /* lamprey */
    0u,  /* looter */
    0u,  /* pursuer */
    0u,  /* slasher */
    0u,  /* slasher_egg */
    0u,  /* spawner */
    0u,  /* spitter */
    0u,  /* tentacle */
    0u,  /* bat */
    0u,  /* giant */
    0u,  /* giant_isopod */
    0u,  /* impaled_worm */
    0u,  /* jellyfish */
    0u,  /* megalodon */
    0u,  /* prisoner */
    0u,  /* spider_crab */
    0u,  /* turtle */
    0u,  /* butcher */
    0u,  /* colossus */
    0u,  /* croc */
    0u,  /* gargoyle */
    0u,  /* mantis */
    0u,  /* mom */
    0u,  /* monk */
    0u,  /* rhino */
    0u,  /* dead_whale */
    0u,  /* eel */
    0u,  /* invoker */
    0u,  /* predator */
};
BT_TABLE unsigned short BT_KIND_MOB_QTY[BT_NKINDS][3] = {
    { 0u, 0u, 0u },  /* anemone */
    { 0u, 0u, 0u },  /* anglerfish */
    { 0u, 0u, 0u },  /* blobfish */
    { 0u, 0u, 0u },  /* clam */
    { 0u, 0u, 0u },  /* colossal_squid */
    { 0u, 0u, 0u },  /* crab */
    { 0u, 0u, 0u },  /* diplocaulus */
    { 0u, 0u, 0u },  /* dragonfish */
    { 0u, 10u, 20u },  /* evil_mob_swimmer */
    { 0u, 0u, 0u },  /* goblin_shark */
    { 0u, 0u, 0u },  /* hermit */
    { 0u, 0u, 0u },  /* iron_lung */
    { 0u, 0u, 0u },  /* lobster */
    { 0u, 0u, 0u },  /* looting_pig */
    { 0u, 0u, 0u },  /* narwhal */
    { 0u, 0u, 0u },  /* plankton */
    { 0u, 0u, 0u },  /* pufferfish */
    { 0u, 0u, 0u },  /* sea_pig */
    { 0u, 0u, 0u },  /* shrimp */
    { 0u, 0u, 0u },  /* stargazer */
    { 0u, 0u, 0u },  /* stonefish */
    { 0u, 0u, 0u },  /* vampire_squid */
    { 0u, 0u, 0u },  /* viperfish */
    { 0u, 0u, 0u },  /* walrus */
    { 0u, 0u, 0u },  /* baby_alien */
    { 0u, 0u, 0u },  /* bruiser */
    { 0u, 0u, 0u },  /* buffer */
    { 0u, 0u, 0u },  /* charger */
    { 0u, 0u, 0u },  /* chaser */
    { 0u, 10u, 20u },  /* evil_mob */
    { 0u, 0u, 0u },  /* fin_alien */
    { 0u, 0u, 0u },  /* fly */
    { 0u, 0u, 0u },  /* healer */
    { 0u, 0u, 0u },  /* helmet_alien */
    { 0u, 0u, 0u },  /* horned_bruiser */
    { 0u, 0u, 0u },  /* horned_charger */
    { 0u, 0u, 0u },  /* junkie */
    { 0u, 0u, 0u },  /* lamprey */
    { 0u, 0u, 0u },  /* looter */
    { 0u, 0u, 0u },  /* pursuer */
    { 0u, 0u, 0u },  /* slasher */
    { 0u, 0u, 0u },  /* slasher_egg */
    { 0u, 0u, 0u },  /* spawner */
    { 0u, 0u, 0u },  /* spitter */
    { 0u, 0u, 0u },  /* tentacle */
    { 0u, 0u, 0u },  /* bat */
    { 0u, 0u, 0u },  /* giant */
    { 0u, 0u, 0u },  /* giant_isopod */
    { 0u, 0u, 0u },  /* impaled_worm */
    { 0u, 0u, 0u },  /* jellyfish */
    { 0u, 0u, 0u },  /* megalodon */
    { 0u, 0u, 0u },  /* prisoner */
    { 0u, 0u, 0u },  /* spider_crab */
    { 0u, 0u, 0u },  /* turtle */
    { 0u, 0u, 0u },  /* butcher */
    { 0u, 0u, 0u },  /* colossus */
    { 0u, 0u, 0u },  /* croc */
    { 0u, 0u, 0u },  /* gargoyle */
    { 0u, 0u, 0u },  /* mantis */
    { 0u, 0u, 0u },  /* mom */
    { 0u, 0u, 0u },  /* monk */
    { 0u, 0u, 0u },  /* rhino */
    { 0u, 0u, 0u },  /* dead_whale */
    { 0u, 0u, 0u },  /* eel */
    { 0u, 0u, 0u },  /* invoker */
    { 0u, 0u, 0u },  /* predator */
};

/* Scene-root flags for the boost/charm guards (entity.gd:7/107-110
 * can_be_boosted, enemy.gd:11-12 can_be_charmed / is_loot). */
BT_TABLE unsigned char BT_KIND_BOOSTABLE[BT_NKINDS] = {
    1u,  /* anemone */
    1u,  /* anglerfish */
    1u,  /* blobfish */
    1u,  /* clam */
    1u,  /* colossal_squid */
    1u,  /* crab */
    1u,  /* diplocaulus */
    1u,  /* dragonfish */
    0u,  /* evil_mob_swimmer */
    1u,  /* goblin_shark */
    1u,  /* hermit */
    1u,  /* iron_lung */
    1u,  /* lobster */
    1u,  /* looting_pig */
    1u,  /* narwhal */
    1u,  /* plankton */
    1u,  /* pufferfish */
    0u,  /* sea_pig */
    1u,  /* shrimp */
    1u,  /* stargazer */
    0u,  /* stonefish */
    1u,  /* vampire_squid */
    1u,  /* viperfish */
    1u,  /* walrus */
    1u,  /* baby_alien */
    1u,  /* bruiser */
    0u,  /* buffer */
    1u,  /* charger */
    1u,  /* chaser */
    0u,  /* evil_mob */
    1u,  /* fin_alien */
    1u,  /* fly */
    1u,  /* healer */
    1u,  /* helmet_alien */
    1u,  /* horned_bruiser */
    1u,  /* horned_charger */
    1u,  /* junkie */
    1u,  /* lamprey */
    1u,  /* looter */
    1u,  /* pursuer */
    1u,  /* slasher */
    1u,  /* slasher_egg */
    1u,  /* spawner */
    1u,  /* spitter */
    1u,  /* tentacle */
    0u,  /* bat */
    0u,  /* giant */
    0u,  /* giant_isopod */
    0u,  /* impaled_worm */
    0u,  /* jellyfish */
    0u,  /* megalodon */
    0u,  /* prisoner */
    0u,  /* spider_crab */
    0u,  /* turtle */
    0u,  /* butcher */
    0u,  /* colossus */
    0u,  /* croc */
    0u,  /* gargoyle */
    0u,  /* mantis */
    0u,  /* mom */
    0u,  /* monk */
    0u,  /* rhino */
    0u,  /* dead_whale */
    0u,  /* eel */
    0u,  /* invoker */
    0u,  /* predator */
};
/* healer.gd BoostZone exports: heal/heal_grow (enemies) and
 * player_heal/player_heal_grow (the player, charmed only). */
BT_TABLE float BT_KIND_HEAL[BT_NKINDS][4] = {
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* anemone */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* anglerfish */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* blobfish */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* clam */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* colossal_squid */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* crab */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* diplocaulus */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* dragonfish */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* evil_mob_swimmer */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* goblin_shark */
    { 100.0f, 10.0f, 1.0f, 0.5f },  /* hermit */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* iron_lung */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* lobster */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* looting_pig */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* narwhal */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* plankton */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* pufferfish */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* sea_pig */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* shrimp */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* stargazer */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* stonefish */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* vampire_squid */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* viperfish */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* walrus */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* baby_alien */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* bruiser */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* buffer */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* charger */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* chaser */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* evil_mob */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* fin_alien */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* fly */
    { 100.0f, 10.0f, 1.0f, 0.5f },  /* healer */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* helmet_alien */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* horned_bruiser */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* horned_charger */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* junkie */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* lamprey */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* looter */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* pursuer */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* slasher */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* slasher_egg */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* spawner */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* spitter */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* tentacle */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* bat */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* giant */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* giant_isopod */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* impaled_worm */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* jellyfish */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* megalodon */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* prisoner */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* spider_crab */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* turtle */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* butcher */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* colossus */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* croc */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* gargoyle */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* mantis */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* mom */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* monk */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* rhino */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* dead_whale */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* eel */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* invoker */
    { 0.0f, 0.0f, 0.0f, 0.0f },  /* predator */
};
BT_TABLE unsigned char BT_KIND_CHARMABLE[BT_NKINDS] = {
    1u,  /* anemone */
    1u,  /* anglerfish */
    1u,  /* blobfish */
    1u,  /* clam */
    1u,  /* colossal_squid */
    1u,  /* crab */
    1u,  /* diplocaulus */
    1u,  /* dragonfish */
    0u,  /* evil_mob_swimmer */
    1u,  /* goblin_shark */
    1u,  /* hermit */
    1u,  /* iron_lung */
    1u,  /* lobster */
    0u,  /* looting_pig */
    1u,  /* narwhal */
    1u,  /* plankton */
    1u,  /* pufferfish */
    0u,  /* sea_pig */
    1u,  /* shrimp */
    1u,  /* stargazer */
    1u,  /* stonefish */
    1u,  /* vampire_squid */
    1u,  /* viperfish */
    1u,  /* walrus */
    1u,  /* baby_alien */
    1u,  /* bruiser */
    1u,  /* buffer */
    1u,  /* charger */
    1u,  /* chaser */
    0u,  /* evil_mob */
    1u,  /* fin_alien */
    1u,  /* fly */
    1u,  /* healer */
    1u,  /* helmet_alien */
    1u,  /* horned_bruiser */
    1u,  /* horned_charger */
    1u,  /* junkie */
    1u,  /* lamprey */
    0u,  /* looter */
    1u,  /* pursuer */
    1u,  /* slasher */
    1u,  /* slasher_egg */
    1u,  /* spawner */
    1u,  /* spitter */
    1u,  /* tentacle */
    1u,  /* bat */
    1u,  /* giant */
    1u,  /* giant_isopod */
    1u,  /* impaled_worm */
    1u,  /* jellyfish */
    1u,  /* megalodon */
    1u,  /* prisoner */
    1u,  /* spider_crab */
    1u,  /* turtle */
    1u,  /* butcher */
    1u,  /* colossus */
    1u,  /* croc */
    1u,  /* gargoyle */
    1u,  /* mantis */
    1u,  /* mom */
    1u,  /* monk */
    1u,  /* rhino */
    1u,  /* dead_whale */
    1u,  /* eel */
    1u,  /* invoker */
    1u,  /* predator */
};
BT_TABLE unsigned char BT_KIND_IS_LOOT[BT_NKINDS] = {
    0u,  /* anemone */
    0u,  /* anglerfish */
    0u,  /* blobfish */
    0u,  /* clam */
    0u,  /* colossal_squid */
    0u,  /* crab */
    0u,  /* diplocaulus */
    0u,  /* dragonfish */
    1u,  /* evil_mob_swimmer */
    0u,  /* goblin_shark */
    0u,  /* hermit */
    0u,  /* iron_lung */
    0u,  /* lobster */
    1u,  /* looting_pig */
    0u,  /* narwhal */
    0u,  /* plankton */
    0u,  /* pufferfish */
    0u,  /* sea_pig */
    0u,  /* shrimp */
    0u,  /* stargazer */
    0u,  /* stonefish */
    0u,  /* vampire_squid */
    0u,  /* viperfish */
    0u,  /* walrus */
    0u,  /* baby_alien */
    0u,  /* bruiser */
    0u,  /* buffer */
    0u,  /* charger */
    0u,  /* chaser */
    1u,  /* evil_mob */
    0u,  /* fin_alien */
    0u,  /* fly */
    0u,  /* healer */
    0u,  /* helmet_alien */
    0u,  /* horned_bruiser */
    0u,  /* horned_charger */
    0u,  /* junkie */
    0u,  /* lamprey */
    1u,  /* looter */
    0u,  /* pursuer */
    0u,  /* slasher */
    0u,  /* slasher_egg */
    0u,  /* spawner */
    0u,  /* spitter */
    0u,  /* tentacle */
    0u,  /* bat */
    0u,  /* giant */
    0u,  /* giant_isopod */
    0u,  /* impaled_worm */
    0u,  /* jellyfish */
    0u,  /* megalodon */
    0u,  /* prisoner */
    0u,  /* spider_crab */
    0u,  /* turtle */
    0u,  /* butcher */
    0u,  /* colossus */
    0u,  /* croc */
    0u,  /* gargoyle */
    0u,  /* mantis */
    0u,  /* mom */
    0u,  /* monk */
    0u,  /* rhino */
    0u,  /* dead_whale */
    0u,  /* eel */
    0u,  /* invoker */
    0u,  /* predator */
};

/* dlc_1 charm_enemy_effect_behavior.tscn (max_charm_count/damage_increase;
 * the .gd defaults 5/50 are overridden in the scene) and the CharmTimer
 * wait_time = Utils.CHARM_DURATION (singletons/utils.gd:35). */
#define BT_CHARM_DURATION 8.0f
#define BT_CHARM_MAX_COUNT 999
#define BT_CHARM_DAMAGE_INCREASE 200

typedef struct {
    unsigned char beh;   /* BT_INITBEH_MOV_* of the active node */
    unsigned char pre;   /* detect_target: before-detection child */
    unsigned char sub;   /* detect_target: after-detection child */
    unsigned char flags; /* BT_MOVFK_* */
    float p0;            /* rand_around: update_every_x_frames; detect: detection_range */
    float p1;            /* detect: speed_bonus_on_target_detection */
} BtMovSpec;

/* Animation/Sprite texture half width (entity.gd:36-38 _min_pos inset). */
BT_TABLE float BT_KIND_HALF_W[BT_NKINDS] = {
    75.0f,  /* anemone */
    75.0f,  /* anglerfish */
    75.0f,  /* blobfish */
    75.0f,  /* clam */
    110.0f,  /* colossal_squid */
    60.0f,  /* crab */
    112.5f,  /* diplocaulus */
    125.0f,  /* dragonfish */
    125.0f,  /* evil_mob_swimmer */
    76.0f,  /* goblin_shark */
    60.0f,  /* hermit */
    87.5f,  /* iron_lung */
    82.5f,  /* lobster */
    90.0f,  /* looting_pig */
    85.0f,  /* narwhal */
    50.0f,  /* plankton */
    60.0f,  /* pufferfish */
    80.0f,  /* sea_pig */
    60.0f,  /* shrimp */
    75.0f,  /* stargazer */
    75.0f,  /* stonefish */
    64.0f,  /* vampire_squid */
    55.0f,  /* viperfish */
    90.0f,  /* walrus */
    50.0f,  /* baby_alien */
    112.5f,  /* bruiser */
    50.0f,  /* buffer */
    50.0f,  /* charger */
    50.0f,  /* chaser */
    125.0f,  /* evil_mob */
    75.0f,  /* fin_alien */
    50.0f,  /* fly */
    50.0f,  /* healer */
    60.0f,  /* helmet_alien */
    112.5f,  /* horned_bruiser */
    75.0f,  /* horned_charger */
    60.0f,  /* junkie */
    65.0f,  /* lamprey */
    60.0f,  /* looter */
    60.0f,  /* pursuer */
    112.5f,  /* slasher */
    50.0f,  /* slasher_egg */
    75.0f,  /* spawner */
    50.0f,  /* spitter */
    70.0f,  /* tentacle */
    125.0f,  /* bat */
    125.0f,  /* giant */
    125.0f,  /* giant_isopod */
    125.0f,  /* impaled_worm */
    125.0f,  /* jellyfish */
    125.0f,  /* megalodon */
    125.0f,  /* prisoner */
    125.0f,  /* spider_crab */
    140.0f,  /* turtle */
    125.0f,  /* butcher */
    110.0f,  /* colossus */
    125.0f,  /* croc */
    110.0f,  /* gargoyle */
    125.0f,  /* mantis */
    110.0f,  /* mom */
    125.0f,  /* monk */
    115.0f,  /* rhino */
    125.0f,  /* dead_whale */
    125.0f,  /* eel */
    125.0f,  /* invoker */
    125.0f,  /* predator */
};

BT_TABLE float BT_KIND_HALF_H[BT_NKINDS] = {
    75.0f,  /* anemone */
    75.0f,  /* anglerfish */
    75.0f,  /* blobfish */
    75.0f,  /* clam */
    110.0f,  /* colossal_squid */
    60.0f,  /* crab */
    112.5f,  /* diplocaulus */
    125.0f,  /* dragonfish */
    125.0f,  /* evil_mob_swimmer */
    76.0f,  /* goblin_shark */
    60.0f,  /* hermit */
    87.5f,  /* iron_lung */
    82.5f,  /* lobster */
    90.0f,  /* looting_pig */
    85.0f,  /* narwhal */
    50.0f,  /* plankton */
    60.0f,  /* pufferfish */
    80.0f,  /* sea_pig */
    60.0f,  /* shrimp */
    75.0f,  /* stargazer */
    75.0f,  /* stonefish */
    64.0f,  /* vampire_squid */
    55.0f,  /* viperfish */
    90.0f,  /* walrus */
    50.0f,  /* baby_alien */
    112.5f,  /* bruiser */
    50.0f,  /* buffer */
    50.0f,  /* charger */
    50.0f,  /* chaser */
    125.0f,  /* evil_mob */
    75.0f,  /* fin_alien */
    75.0f,  /* fly */
    50.0f,  /* healer */
    60.0f,  /* helmet_alien */
    112.5f,  /* horned_bruiser */
    75.0f,  /* horned_charger */
    60.0f,  /* junkie */
    65.0f,  /* lamprey */
    60.0f,  /* looter */
    60.0f,  /* pursuer */
    112.5f,  /* slasher */
    50.0f,  /* slasher_egg */
    75.0f,  /* spawner */
    50.0f,  /* spitter */
    70.0f,  /* tentacle */
    130.0f,  /* bat */
    130.0f,  /* giant */
    125.0f,  /* giant_isopod */
    130.0f,  /* impaled_worm */
    125.0f,  /* jellyfish */
    125.0f,  /* megalodon */
    130.0f,  /* prisoner */
    125.0f,  /* spider_crab */
    140.0f,  /* turtle */
    130.0f,  /* butcher */
    114.5f,  /* colossus */
    130.0f,  /* croc */
    114.5f,  /* gargoyle */
    130.0f,  /* mantis */
    114.5f,  /* mom */
    130.0f,  /* monk */
    120.0f,  /* rhino */
    125.0f,  /* dead_whale */
    125.0f,  /* eel */
    130.0f,  /* invoker */
    130.0f,  /* predator */
};

/* Kind root $MovementBehavior (Unit._ready / Enemy.init, state 0xFF). */
BT_TABLE BtMovSpec BT_MOV_ROOT[BT_NKINDS] = {
    { 12u, 0u, 0u, 0u, 0.0f, 0.0f },  /* anemone */
    { 6u, 12u, 7u, 0u, 340.0f, 350.0f },  /* anglerfish */
    { 12u, 0u, 0u, 0u, 0.0f, 0.0f },  /* blobfish */
    { 1u, 0u, 0u, 0u, 0.0f, 0.0f },  /* clam */
    { 5u, 0u, 0u, 0u, 120.0f, 0.0f },  /* colossal_squid */
    { 7u, 0u, 0u, 0u, 0.0f, 0.0f },  /* crab */
    { 12u, 0u, 0u, 0u, 0.0f, 0.0f },  /* diplocaulus */
    { 12u, 0u, 0u, 0u, 0.0f, 0.0f },  /* dragonfish */
    { 11u, 0u, 0u, 0u, 0.0f, 0.0f },  /* evil_mob_swimmer */
    { 12u, 0u, 0u, 0u, 0.0f, 0.0f },  /* goblin_shark */
    { 12u, 0u, 0u, 0u, 0.0f, 0.0f },  /* hermit */
    { 1u, 0u, 0u, 0u, 0.0f, 0.0f },  /* iron_lung */
    { 7u, 0u, 0u, 0u, 0.0f, 0.0f },  /* lobster */
    { 12u, 0u, 0u, 0u, 0.0f, 0.0f },  /* looting_pig */
    { 1u, 0u, 0u, 0u, 0.0f, 0.0f },  /* narwhal */
    { 7u, 0u, 0u, 0u, 0.0f, 0.0f },  /* plankton */
    { 4u, 0u, 0u, 0u, 0.0f, 0.0f },  /* pufferfish */
    { 4u, 0u, 0u, 0u, 0.0f, 0.0f },  /* sea_pig */
    { 7u, 0u, 0u, 0u, 0.0f, 0.0f },  /* shrimp */
    { 13u, 0u, 0u, 0u, 0.0f, 0.0f },  /* stargazer */
    { 12u, 0u, 0u, 0u, 0.0f, 0.0f },  /* stonefish */
    { 7u, 0u, 0u, 0u, 0.0f, 0.0f },  /* vampire_squid */
    { 12u, 0u, 0u, 0u, 0.0f, 0.0f },  /* viperfish */
    { 2u, 0u, 0u, 1u, 0.0f, 0.0f },  /* walrus */
    { 7u, 0u, 0u, 0u, 0.0f, 0.0f },  /* baby_alien */
    { 12u, 0u, 0u, 0u, 0.0f, 0.0f },  /* bruiser */
    { 1u, 0u, 0u, 0u, 0.0f, 0.0f },  /* buffer */
    { 7u, 0u, 0u, 0u, 0.0f, 0.0f },  /* charger */
    { 7u, 0u, 0u, 0u, 0.0f, 0.0f },  /* chaser */
    { 11u, 0u, 0u, 0u, 0.0f, 0.0f },  /* evil_mob */
    { 7u, 0u, 0u, 0u, 0.0f, 0.0f },  /* fin_alien */
    { 5u, 0u, 0u, 0u, -1.0f, 0.0f },  /* fly */
    { 12u, 0u, 0u, 0u, 0.0f, 0.0f },  /* healer */
    { 7u, 0u, 0u, 0u, 0.0f, 0.0f },  /* helmet_alien */
    { 7u, 0u, 0u, 0u, 0.0f, 0.0f },  /* horned_bruiser */
    { 7u, 0u, 0u, 0u, 0.0f, 0.0f },  /* horned_charger */
    { 12u, 0u, 0u, 0u, 0.0f, 0.0f },  /* junkie */
    { 7u, 0u, 0u, 0u, 0.0f, 0.0f },  /* lamprey */
    { 12u, 0u, 0u, 0u, 0.0f, 0.0f },  /* looter */
    { 7u, 0u, 0u, 0u, 0.0f, 0.0f },  /* pursuer */
    { 2u, 0u, 0u, 1u, 0.0f, 0.0f },  /* slasher */
    { 1u, 0u, 0u, 0u, 0.0f, 0.0f },  /* slasher_egg */
    { 2u, 0u, 0u, 0u, 0.0f, 0.0f },  /* spawner */
    { 1u, 0u, 0u, 0u, 0.0f, 0.0f },  /* spitter */
    { 2u, 0u, 0u, 1u, 0.0f, 0.0f },  /* tentacle */
    { 5u, 0u, 0u, 0u, 120.0f, 0.0f },  /* bat */
    { 7u, 0u, 0u, 0u, 0.0f, 0.0f },  /* giant */
    { 5u, 0u, 0u, 0u, 60.0f, 0.0f },  /* giant_isopod */
    { 7u, 0u, 0u, 0u, 0.0f, 0.0f },  /* impaled_worm */
    { 7u, 0u, 0u, 0u, 0.0f, 0.0f },  /* jellyfish */
    { 5u, 0u, 0u, 0u, 60.0f, 0.0f },  /* megalodon */
    { 7u, 0u, 0u, 0u, 0.0f, 0.0f },  /* prisoner */
    { 5u, 0u, 0u, 0u, -1.0f, 0.0f },  /* spider_crab */
    { 7u, 0u, 0u, 0u, 0.0f, 0.0f },  /* turtle */
    { 7u, 0u, 0u, 0u, 0.0f, 0.0f },  /* butcher */
    { 7u, 0u, 0u, 0u, 0.0f, 0.0f },  /* colossus */
    { 7u, 0u, 0u, 0u, 0.0f, 0.0f },  /* croc */
    { 12u, 0u, 0u, 0u, 0.0f, 0.0f },  /* gargoyle */
    { 7u, 0u, 0u, 0u, 0.0f, 0.0f },  /* mantis */
    { 7u, 0u, 0u, 0u, 0.0f, 0.0f },  /* mom */
    { 12u, 0u, 0u, 0u, 0.0f, 0.0f },  /* monk */
    { 7u, 0u, 0u, 0u, 0.0f, 0.0f },  /* rhino */
    { 12u, 0u, 0u, 0u, 0.0f, 0.0f },  /* dead_whale */
    { 7u, 0u, 0u, 0u, 0.0f, 0.0f },  /* eel */
    { 7u, 0u, 0u, 0u, 0.0f, 0.0f },  /* invoker */
    { 7u, 0u, 0u, 0u, 0.0f, 0.0f },  /* predator */
};

/* States/StateN/MovementBehavior (Boss._on_CheckStateTimer_timeout).
 * Slots >= BT_NKIND_STATES[kind] are unreachable (estate < ns guard). */
BT_TABLE BtMovSpec BT_MOV_STATE[BT_NKINDS][BT_MAX_STATES] = {
    { { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* anemone (0 states) */
    { { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* anglerfish (0 states) */
    { { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* blobfish (0 states) */
    { { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* clam (0 states) */
    { { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* colossal_squid (0 states) */
    { { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* crab (0 states) */
    { { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* diplocaulus (0 states) */
    { { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* dragonfish (0 states) */
    { { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* evil_mob_swimmer (0 states) */
    { { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* goblin_shark (0 states) */
    { { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* hermit (0 states) */
    { { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* iron_lung (0 states) */
    { { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* lobster (0 states) */
    { { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* looting_pig (0 states) */
    { { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* narwhal (0 states) */
    { { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* plankton (0 states) */
    { { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* pufferfish (0 states) */
    { { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* sea_pig (0 states) */
    { { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* shrimp (0 states) */
    { { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* stargazer (0 states) */
    { { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* stonefish (0 states) */
    { { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* vampire_squid (0 states) */
    { { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* viperfish (0 states) */
    { { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* walrus (0 states) */
    { { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* baby_alien (0 states) */
    { { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* bruiser (0 states) */
    { { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* buffer (0 states) */
    { { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* charger (0 states) */
    { { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* chaser (0 states) */
    { { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* evil_mob (0 states) */
    { { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* fin_alien (0 states) */
    { { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* fly (0 states) */
    { { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* healer (0 states) */
    { { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* helmet_alien (0 states) */
    { { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* horned_bruiser (0 states) */
    { { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* horned_charger (0 states) */
    { { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* junkie (0 states) */
    { { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* lamprey (0 states) */
    { { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* looter (0 states) */
    { { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* pursuer (0 states) */
    { { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* slasher (0 states) */
    { { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* slasher_egg (0 states) */
    { { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* spawner (0 states) */
    { { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* spitter (0 states) */
    { { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* tentacle (0 states) */
    { { 5u, 0u, 0u, 0u, 60.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* bat (1 states) */
    { { 7u, 0u, 0u, 0u, 0.0f, 0.0f }, { 7u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* giant (2 states) */
    { { 7u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* giant_isopod (1 states) */
    { { 8u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* impaled_worm (1 states) */
    { { 8u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* jellyfish (1 states) */
    { { 5u, 0u, 0u, 0u, 60.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* megalodon (1 states) */
    { { 5u, 0u, 0u, 0u, 120.0f, 0.0f }, { 7u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* prisoner (2 states) */
    { { 5u, 0u, 0u, 0u, 120.0f, 0.0f }, { 5u, 0u, 0u, 0u, 60.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* spider_crab (2 states) */
    { { 5u, 0u, 0u, 0u, 120.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* turtle (1 states) */
    { { 7u, 0u, 0u, 0u, 0.0f, 0.0f }, { 5u, 0u, 0u, 0u, -1.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* butcher (2 states) */
    { { 12u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* colossus (1 states) */
    { { 1u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* croc (1 states) */
    { { 7u, 0u, 0u, 0u, 0.0f, 0.0f }, { 5u, 0u, 0u, 0u, -1.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* gargoyle (2 states) */
    { { 7u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* mantis (1 states) */
    { { 1u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* mom (1 states) */
    { { 12u, 0u, 0u, 0u, 0.0f, 0.0f }, { 12u, 0u, 0u, 0u, 0.0f, 0.0f }, { 12u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* monk (3 states) */
    { { 7u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* rhino (1 states) */
    { { 7u, 0u, 0u, 0u, 0.0f, 0.0f }, { 5u, 0u, 0u, 0u, 60.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* dead_whale (2 states) */
    { { 12u, 0u, 0u, 0u, 0.0f, 0.0f }, { 7u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* eel (2 states) */
    { { 7u, 0u, 0u, 0u, 0.0f, 0.0f }, { 12u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* invoker (2 states) */
    { { 5u, 0u, 0u, 0u, -1.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f }, { 255u, 0u, 0u, 0u, 0.0f, 0.0f } },  /* predator (1 states) */
};
/* ====================== END GENERATED ====================== */

/* Movement node index of a live slot: the state ordinal, or BT_MAX_STATES for
 * the kind root $MovementBehavior (estate == BT_INIT_ROOT_STATE). */
BT_HD static int bt_mov_node(BT_DEVICE BtState* s, int slot) {
    int ns = (int)BT_NKIND_STATES[s->ekind[slot]];
    int es = (int)s->estate[slot];
    return (es != BT_INIT_ROOT_STATE && es < ns) ? es : BT_MAX_STATES;
}

/* bt_enemy_init_find state key of the active movement node (root -> 0xFF). */
BT_HD static int bt_mov_key(int node) {
    return node == BT_MAX_STATES ? BT_INIT_ROOT_STATE : node;
}

/* A retained init value (binary64 bits in the instance's pool row) as float.
 * Missing row -> 0 (a behaviour with no draw at that node never asks). */
BT_HD static float bt_mov_val(BT_DEVICE BtState* s, int slot, int node, int dest) {
    bt_u64 bits;
    if (!bt_enemy_init_find(s, slot, dest, bt_mov_key(node), &bits)) return 0.0f;
    return bt_rng_f64_to_float_bits(bits);
}

/* entity.gd:34-43 _min_pos/_max_pos: zone rect inset by the sprite half extent. */
BT_HD static float bt_mov_min_x(int kind) { return BT_KIND_HALF_W[kind]; }
BT_HD static float bt_mov_max_x(int kind) { return BT_ARENA_W - BT_KIND_HALF_W[kind]; }
BT_HD static float bt_mov_min_y(int kind) { return BT_KIND_HALF_H[kind]; }
BT_HD static float bt_mov_max_y(int kind) { return BT_ARENA_H - BT_KIND_HALF_H[kind]; }

/* Utils.vectors_approx_equal(a, b, 10) -- distance (not squared) <= precision
 * (utils.gd:321-322). */
BT_HD static int bt_mov_approx(float ax, float ay, float bx, float by) {
    float dx = ax - bx, dy = ay - by;
    return bt_sqrtf(dx * dx + dy * dy) <= BT_MOV_PREC;
}

/* Lifecycle: clear the instance's movement-node state on a FRESH spawn only
 * (scene.instance() member defaults are all zero) and reset the Unit
 * bonus_speed on every spawn (Unit.respawn, unit.gd:85).  A pooled respawn
 * preserves the node state, exactly like the reused scene instance.  Called
 * from bt_spawn_kind_at right after bt_enemy_init_run, non-neutral kinds only. */
BT_HD static void bt_enemy_motion_spawn(BT_DEVICE BtState* s, int slot, int fresh) {
    unsigned short inst = s->e_instance[slot];
    if (fresh) {
        if (inst != (unsigned short)BT_POOL_NO_INST) {
            int first = s->pool_mv_state[inst] == BT_POOL_NO_INST ? BT_MAX_STATES : 0;
            for (int n = first; n <= BT_MAX_STATES; n++) {
                *bt_pool_mvtx(s, inst, n) = 0.0f;
                *bt_pool_mvty(s, inst, n) = 0.0f;
                *bt_pool_mvtime(s, inst, n) = 0.0f;
                *bt_pool_mvflag(s, inst, n) = 0;
            }
        }
    }
    if (inst != (unsigned short)BT_POOL_NO_INST) {
        /* iron_lung.gd:20 respawn() clears is_full; a fresh node's member
         * default is false too, so every spawn clears it. */
        s->pool_lungfull[inst] = 0;
    }
    /* Stargazer members (stargazer.gd:24-28): _target_lung and the angry switch
     * are both null/false at spawn; Unit.respawn clears bonus_speed (unit.gd:85).
     * enemy.gd:24 source_spawner is null outside the spawn-attack path. */
    s->mv_lung[slot] = (unsigned short)BT_POOL_NO_INST;
    s->mv_angry[slot] = 0;
    s->e_source[slot] = -1;
    s->ebonus[slot] = 0.0f;
    /* charm: a respawned unit is uncharmed (the charm behavior instance dies
     * with the unit) and a respawned Healer's BoostZone membership is re-derived
     * by the shape re-enabling (healer.gd respawn re-enables the collision). */
    s->e_charmed[slot] = 0;
    s->e_charm_shield[slot] = 0;
    s->e_charm_by[slot] = -1;
    s->e_charm_timer[slot] = 0.0f;
    for (int by = 0; by < BT_HEAL_BYTES; by++) s->heal_in[slot][by] = 0;
    /* Enemy.respawn -> init_current_stats -> reset_*_stat(0) clears the boost
     * percentages in force. */
    s->eboost_hp[slot] = 0.0f; s->eboost_dmg[slot] = 0.0f; s->eboost_spd[slot] = 0.0f;
}

/* -------------------------------------------------------------------------
 * get_new_target() for the lazy-target behaviours.
 * ---------------------------------------------------------------------- */
/* TargetRandPosMovementBehavior.get_new_target (target_rand_pos:18-25) and the
 * stargazer subclass fallback: one rand_range per axis over [_min,_max]. */
BT_HD static BtVec2 bt_mov_rand_pos(BT_DEVICE BtState* s, int kind) {
    float x = bt_rand_range(s, bt_mov_min_x(kind), bt_mov_max_x(kind));
    float y = bt_rand_range(s, bt_mov_min_y(kind), bt_mov_max_y(kind));
    return bt_v2(x, y);
}

/* TargetRandPosAroundPlayerMovementBehavior.get_new_target (…around_player:37-44):
 * player + per-axis +-rand(_actual), clamped into [_min,_max]. */
BT_HD static BtVec2 bt_mov_rand_around_player(BT_DEVICE BtState* s, int kind,
                                              float player_x, float player_y, float actual) {
    float x = player_x + bt_rand_range(s, -actual, actual);
    float y = player_y + bt_rand_range(s, -actual, actual);
    x = bt_clampf(x, bt_mov_min_x(kind), bt_mov_max_x(kind));
    y = bt_clampf(y, bt_mov_min_y(kind), bt_mov_max_y(kind));
    return bt_v2(x, y);
}

/* Utils.get_direction_from_side (utils.gd:340-347): the corner branches roll
 * randf() > 0.5 to swap to TOP/BOTTOM, then report the edge. */
BT_HD static int bt_mov_dir_side(BT_DEVICE BtState* s, int side,
                                 float pos_y, float min_y, float max_y, float distance) {
    if (pos_y <= min_y + distance)
        return bt_math_randf(&s->rng) > 0.5f ? 0 /*TOP*/ : side;
    if (pos_y >= max_y - distance)
        return bt_math_randf(&s->rng) > 0.5f ? 2 /*BOTTOM*/ : side;
    return side;
}

/* Utils.get_direction_from_pos (utils.gd:325-338); NONE = 4. */
BT_HD static int bt_mov_direction(BT_DEVICE BtState* s,
                                  float pos_x, float pos_y,
                                  float min_x, float min_y, float max_x, float max_y,
                                  float distance) {
    if (pos_x <= min_x + distance)
        return bt_mov_dir_side(s, 1 /*LEFT*/, pos_y, min_y, max_y, distance);
    if (pos_x >= max_x - distance)
        return bt_mov_dir_side(s, 3 /*RIGHT*/, pos_y, min_y, max_y, distance);
    if (pos_y <= min_y + distance) return 0; /*TOP*/
    if (pos_y >= max_y - distance) return 2; /*BOTTOM*/
    return 4; /*NONE*/
}

/* Utils.get_rand_pos_from_direction_within_distance (utils.gd:368-389).
 * EDGE_MAP_DIST / 2 = 32; NONE picks from [BOTTOM,RIGHT,LEFT,TOP] (one global
 * Array.pick_random draw -- utils.gd:391-394). */
BT_HD static BtVec2 bt_mov_rand_from_direction(BT_DEVICE BtState* s, int direction,
                                               float min_x, float min_y,
                                               float max_x, float max_y, float distance) {
    const float md = BT_EDGE_MAP_DIST * 0.5f;
    float x = 0.0f, y = 0.0f;
    if (direction == 4) {
        /* Utils.get_rand_element([BOTTOM, RIGHT, LEFT, TOP]): BOTTOM=2,
         * RIGHT=3, LEFT=1, TOP=0. */
        int pick = (int)bt_rng_bounded(&s->rng, 4u);
        direction = pick == 0 ? 2 : (pick == 1 ? 3 : (pick == 2 ? 1 : 0));
    }
    if (direction == 0) {          /* TOP */
        x = bt_rand_range(s, min_x + md, max_x - md);
        y = bt_rand_range(s, min_y + md, min_y + distance);
    } else if (direction == 2) {   /* BOTTOM */
        x = bt_rand_range(s, min_x + md, max_x - md);
        y = bt_rand_range(s, max_y - md, max_y - distance);
    } else if (direction == 3) {   /* RIGHT */
        x = bt_rand_range(s, max_x - distance, max_x - md);
        y = bt_rand_range(s, min_y + md, max_y - md);
    } else {                       /* LEFT */
        x = bt_rand_range(s, min_x + md, min_x + distance);
        y = bt_rand_range(s, min_y + md, max_y - md);
    }
    return bt_v2(x, y);
}

/* PatrolEdgesOfMapMovementBehavior.get_new_target (patrol_edges_of_map:22-27,33-44).
 * `actual_target = direction*max_target_distance` is overwritten by the clamp,
 * so max_target_distance is dead and the target is clamp(new_target, _min,_max). */
BT_HD static BtVec2 bt_mov_new_patrol_edges(BT_DEVICE BtState* s, int kind,
                                            float self_x, float self_y, float actual) {
    float min_x = bt_mov_min_x(kind), min_y = bt_mov_min_y(kind);
    float max_x = bt_mov_max_x(kind), max_y = bt_mov_max_y(kind);
    int dir = bt_mov_direction(s, self_x, self_y, min_x, min_y, max_x, max_y, actual);
    BtVec2 nt = bt_mov_rand_from_direction(s, dir, min_x, min_y, max_x, max_y, actual);
    return bt_v2(bt_clampf(nt.x, min_x, max_x), bt_clampf(nt.y, min_y, max_y));
}

/* TargetRandGoldPosBehavior.get_new_target (target_rand_gold_pos_behavior:25-39):
 * with no live gold, a uniform point in [_min,_max]; otherwise one global
 * Array.pick_random draw over the live gold list, which bt_loot_gold_ordered_slot
 * resolves in the source's insertion order (main.gd:737/797). */
BT_HD static BtVec2 bt_mov_new_gold_target(BT_DEVICE BtState* s, int kind) {
    int n = 0;
    for (int m = 0; m < (int)s->mat_used; m++) if (s->matalive[m]) n++;
    if (n <= 0) return bt_mov_rand_pos(s, kind);
    int pick = (int)bt_rng_bounded(&s->rng, (unsigned int)n);
    int m = bt_loot_gold_ordered_slot(s, pick);
    return bt_v2(s->matx[m], s->maty[m]);
}

/* The live non-neutral slot whose pool instance is `inst` (-1 when none).  The
 * source keeps a NODE reference across pooling; the pool instance is this
 * core's equivalent identity. */
BT_HD static int bt_mov_slot_of_instance(BT_DEVICE BtState* s, unsigned short inst) {
    if (inst == (unsigned short)BT_POOL_NO_INST) return -1;
    for (int m = bt_enemy_next(s, 0, 1); m < (int)s->e_used; m = bt_enemy_next(s, m + 1, 1))
        if (s->ealive[m] && !s->eneutral[m] && s->e_instance[m] == inst) return m;
    return -1;
}

/* Can this unit be boost()ed?  buffer.gd:46 (and Enemy.boost, enemy.gd:311-312)
 * refuse a target whose `can_be_boosted` is false.  The scene flag is
 * BT_KIND_BOOSTABLE; the only runtime writer is the DLC stargazer, whose
 * get_angry (stargazer.gd:80) sets it false and whose respawn (stargazer.gd:26)
 * restores it -- mv_angry is cleared on every spawn, so the flag follows. */
BT_HD static int bt_enemy_can_boost(BT_DEVICE BtState* s, int slot) {
    int kind = (int)s->ekind[slot];
    if (!BT_KIND_BOOSTABLE[kind]) return 0;
    if ((BT_KIND_MOVK[kind] & BT_MOVK_STARGAZER) && s->mv_angry[slot]) return 0;
    return 1;
}

/* Can this unit be charmed?  charm_enemy_effect_behavior.gd:36-39 on_hurt
 * refuses loot, Bosses (elites extend Boss) and can_be_charmed == false; the
 * gates live in the scene flags (BT_KIND_CHARMABLE already excludes loot). */
BT_HD static int bt_enemy_can_charm(BT_DEVICE BtState* s, int slot) {
    int kind = (int)s->ekind[slot];
    if (!BT_KIND_CHARMABLE[kind]) return 0;
    return bt_kind_class(kind) == 0;
}

/* charm_enemy_effect_behavior.gd:139-147 update_target: a charmed enemy targets
 * the NEAREST other enemy whose collision_layer is still ENEMIES_BIT -- a
 * charmed unit sits on PETS_BIT, so other charmed enemies are skipped.  With no
 * such enemy the pre-existing (player/pet) target stands. */
BT_HD static void bt_enemy_charm_target(BT_DEVICE BtState* s, int slot,
                                        BT_THREAD float* tx, BT_THREAD float* ty) {
    float best = 0.0f;
    int found = -1;
    for (int j = bt_enemy_next(s, 0, 1); j < (int)s->e_used; j = bt_enemy_next(s, j + 1, 1)) {
        if (j == slot || !s->ealive[j] || s->eneutral[j] || s->e_charmed[j]) continue;
        float dx = s->ex[j] - s->ex[slot], dy = s->ey[j] - s->ey[slot];
        float d2 = dx * dx + dy * dy;
        if (found < 0 || d2 < best) { found = j; best = d2; }
    }
    if (found >= 0) { *tx = s->ex[found]; *ty = s->ey[found]; }
}

/* TargetRandPosMovementBehavior.get_movement with the stargazer's
 * get_new_target override (stargazer_movement_behavior.gd:12-17): the target is
 * the linked lung's position while that instance is alive and not full,
 * otherwise the inherited uniform point; stargazer.gd:69-70 get_angry
 * permanently swaps to the OnTriggered follow_target. */
BT_HD static BtVec2 bt_mov_stargazer(BT_DEVICE BtState* s, int slot, int node, int kind,
                                     float self_x, float self_y) {
    if (s->mv_angry[slot]) return bt_v2(s->px - self_x, s->py - self_y);
    unsigned short inst = s->e_instance[slot];
    float tx = *bt_pool_mvtx(s, inst, node), ty = *bt_pool_mvty(s, inst, node);
    if ((tx == 0.0f && ty == 0.0f) || bt_mov_approx(tx, ty, self_x, self_y)) {
        unsigned short linst = s->mv_lung[slot];
        int lung = bt_mov_slot_of_instance(s, linst);
        if (lung >= 0 && !s->pool_lungfull[linst]) {
            tx = s->ex[lung]; ty = s->ey[lung];
        } else {
            BtVec2 nt = bt_mov_rand_pos(s, kind);
            tx = nt.x; ty = nt.y;
        }
        *bt_pool_mvtx(s, inst, node) = tx; *bt_pool_mvty(s, inst, node) = ty;
    }
    return bt_v2(tx - self_x, ty - self_y);
}

/* stargazer_movement_behavior.gd:7-9 add_lung: record the link and seed the
 * movement target with the lung's position (the source's _current_target). */
BT_HD static void bt_enemy_motion_stargazer_link(BT_DEVICE BtState* s, int st_slot, int lung_slot) {
    s->mv_lung[st_slot] = s->e_instance[lung_slot];
    unsigned short inst = s->e_instance[st_slot];
    if (inst == (unsigned short)BT_POOL_NO_INST) return;
    int node = bt_mov_node(s, st_slot);
    *bt_pool_mvtx(s, inst, node) = s->ex[lung_slot];
    *bt_pool_mvty(s, inst, node) = s->ey[lung_slot];
}

/* stargazer.gd:69-70 get_angry: the permanent OnTriggered follow_target switch
 * (the hp/damage/speed boost is the caller's). */
BT_HD static void bt_enemy_motion_stargazer_angry(BT_DEVICE BtState* s, int slot) {
    s->mv_angry[slot] = 1;
}

/* entity_spawner.gd:405-407 emits enemy_respawned for every enemy spawn right
 * after set_source; stargazer.gd:46-54 links a freshly spawned iron_lung whose
 * source_spawner is a live stargazer.  Called right after e_source[spawned] is
 * written. */
BT_HD static void bt_enemy_motion_link_spawner(BT_DEVICE BtState* s, int spawned) {
    if (!(BT_KIND_MOVK[s->ekind[spawned]] & BT_MOVK_IRON_LUNG)) return;
    int src = (int)s->e_source[spawned];
    if (src < 0 || src >= BT_MAX) return;
    if (!s->ealive[src] || s->eneutral[src]) return;
    if (!(BT_KIND_MOVK[s->ekind[src]] & BT_MOVK_STARGAZER)) return;
    bt_enemy_motion_stargazer_link(s, src, spawned);
}

/* TargetRandPosMovementBehavior.get_movement (target_rand_pos:7-12,18-25) and the
 * DLC stargazer subclass fallback: hold the stored uniform point, redrawing it
 * when it is ZERO or within EQUALITY_PRECISION of the unit.  Also the
 * before-detection child of detect_target (see bt_mov_detect_child). */
BT_HD static BtVec2 bt_mov_target_rand_pos(BT_DEVICE BtState* s, int slot, int node,
                                           int kind, float self_x, float self_y) {
    unsigned short inst = s->e_instance[slot];
    float tx = *bt_pool_mvtx(s, inst, node), ty = *bt_pool_mvty(s, inst, node);
    if ((tx == 0.0f && ty == 0.0f) || bt_mov_approx(tx, ty, self_x, self_y)) {
        BtVec2 nt = bt_mov_rand_pos(s, kind);
        tx = nt.x; ty = nt.y;
        *bt_pool_mvtx(s, inst, node) = tx; *bt_pool_mvty(s, inst, node) = ty;
    }
    return bt_v2(tx - self_x, ty - self_y);
}

/* DetectTargetMovementBehavior's two child nodes (detect_target:17-21).  The
 * generator asserts every scene's children are target_rand_pos /
 * follow_target, both parameter-free and non-recursive, so the dispatch is a
 * flat two-way select on the constant child code -- MSL has no recursion and a
 * thread-local spec could not be passed as a constant pointer. */
BT_HD static BtVec2 bt_mov_detect_child(BT_DEVICE BtState* s, int slot, int node,
                                        int beh, int kind, float self_x, float self_y) {
    if (beh == BT_INITBEH_MOV_TARGET_RAND_POS)
        return bt_mov_target_rand_pos(s, slot, node, kind, self_x, self_y);
    if (beh == BT_INITBEH_MOV_FOLLOW_TARGET)
        return bt_v2(s->px - self_x, s->py - self_y);
    return bt_v2zero();
}

/* -------------------------------------------------------------------------
 * Dispatch.  Returns the source get_movement() vector for the slot's active
 * movement node (root or boss/elite state).  May mutate the node's target /
 * timer / detected flag and add to s->ebonus[slot] (detect_target).
 * ---------------------------------------------------------------------- */
BT_HD static BtVec2 bt_mov_beh(BT_DEVICE BtState* s, int slot, int node,
                               BT_CONST_PTR(BtMovSpec) spec, int kind,
                               float self_x, float self_y) {
    float tgt_x = s->px, tgt_y = s->py;   /* current_target == the player */
    if (s->e_charmed[slot]) bt_enemy_charm_target(s, slot, &tgt_x, &tgt_y);
    int beh = (int)spec->beh;
    /* charm_enemy_effect_behavior.gd:120-124: a charmed enemy whose
     * _movement_behavior is stay_in_range / follow_rand_pos_around_player is
     * given a fresh TargetRandPosMovementBehavior.  Only plain enemies are
     * charmable (charm refuses Boss/elite), so only the root behaviour can
     * match; the fresh node's _current_target starts ZERO, which the charm
     * helper re-seeds by clearing the node's stored target. */
    if (s->e_charmed[slot] &&
        (beh == BT_INITBEH_MOV_STAY_IN_RANGE || beh == BT_INITBEH_MOV_FOLLOW_RAND_AROUND))
        beh = BT_INITBEH_MOV_TARGET_RAND_POS;
    switch (beh) {
    case BT_INITBEH_MOV_FOLLOW_TARGET:
        /* FollowTargetMovementBehavior.get_movement: player - self (the
         * stop_close_to_target branch is inert for enemies). */
        return bt_v2(tgt_x - self_x, tgt_y - self_y);

    case BT_INITBEH_MOV_STAY_IN_RANGE: {
        /* stay_in_range: target = player + _actual_target_range * dir, where
         * dir = (self - player).normalized(); ZERO when already within
         * EQUALITY_PRECISION (stay_in_range:14-20). */
        float r = bt_mov_val(s, slot, node, BT_INITDEST_MOV_ACTUAL_RANGE);
        float dx = self_x - tgt_x, dy = self_y - tgt_y;
        float len = bt_sqrtf(dx * dx + dy * dy);
        float ux = len > 0.0f ? dx / len : 0.0f;
        float uy = len > 0.0f ? dy / len : 0.0f;
        float tx = tgt_x + r * ux, ty = tgt_y + r * uy;
        if (bt_mov_approx(tx, ty, self_x, self_y)) return bt_v2zero();
        return bt_v2(tx - self_x, ty - self_y);
    }

    case BT_INITBEH_MOV_FOLLOW_RAND_AROUND: {
        /* follow_rand_pos_around_player: target = player + _distance_from_player,
         * ZERO within EQUALITY_PRECISION (follow_rand:23-30).  _distance is the
         * two init draws, normalised*_actual when allow_within is false. */
        float ox = bt_mov_val(s, slot, node, BT_INITDEST_MOV_DIST_X);
        float oy = bt_mov_val(s, slot, node, BT_INITDEST_MOV_DIST_Y);
        if (!(spec->flags & BT_MOVFK_ALLOW_WITHIN)) {
            float a = bt_mov_val(s, slot, node, BT_INITDEST_MOV_ACTUAL);
            float len = bt_sqrtf(ox * ox + oy * oy);
            if (len > 0.0f) { ox = ox / len * a; oy = oy / len * a; }
            else { ox = 0.0f; oy = 0.0f; }
        }
        float tx = tgt_x + ox, ty = tgt_y + oy;
        if (bt_mov_approx(tx, ty, self_x, self_y)) return bt_v2zero();
        return bt_v2(tx - self_x, ty - self_y);
    }

    case BT_INITBEH_MOV_TARGET_RAND_POS:
        return bt_mov_target_rand_pos(s, slot, node, kind, self_x, self_y);

    case BT_INITBEH_MOV_STARGAZER:
        return bt_mov_stargazer(s, slot, node, kind, self_x, self_y);

    case BT_INITBEH_MOV_TARGET_RAND_AROUND: {
        /* target_rand_pos_around_player: _physics_process accumulates
         * current_check_update by 60*delta when update_every_x_frames != -1;
         * get_movement redraws when the timer fires, the target is ZERO, or it
         * is within EQUALITY_PRECISION (…around_player:20-31). */
        unsigned short inst = s->e_instance[slot];
        float actual = bt_mov_val(s, slot, node, BT_INITDEST_MOV_ACTUAL);
        int every = (int)spec->p0;
        float timer = *bt_pool_mvtime(s, inst, node);
        if (every != -1) {
            timer += 60.0f * BT_DT;
            *bt_pool_mvtime(s, inst, node) = timer;
        }
        float tx = *bt_pool_mvtx(s, inst, node), ty = *bt_pool_mvty(s, inst, node);
        if ((every != -1 && timer >= (float)every) ||
            (tx == 0.0f && ty == 0.0f) || bt_mov_approx(tx, ty, self_x, self_y)) {
            *bt_pool_mvtime(s, inst, node) = 0.0f;
            BtVec2 nt = bt_mov_rand_around_player(s, kind, tgt_x, tgt_y, actual);
            tx = nt.x; ty = nt.y;
            *bt_pool_mvtx(s, inst, node) = tx; *bt_pool_mvty(s, inst, node) = ty;
        }
        return bt_v2(tx - self_x, ty - self_y);
    }

    case BT_INITBEH_MOV_PATROL_EDGES: {
        unsigned short inst = s->e_instance[slot];
        float tx = *bt_pool_mvtx(s, inst, node), ty = *bt_pool_mvty(s, inst, node);
        if ((tx == 0.0f && ty == 0.0f) || bt_mov_approx(tx, ty, self_x, self_y)) {
            float actual = bt_mov_val(s, slot, node, BT_INITDEST_MOV_ACTUAL);
            BtVec2 nt = bt_mov_new_patrol_edges(s, kind, self_x, self_y, actual);
            tx = nt.x; ty = nt.y;
            *bt_pool_mvtx(s, inst, node) = tx; *bt_pool_mvty(s, inst, node) = ty;
        }
        return bt_v2(tx - self_x, ty - self_y);
    }

    case BT_INITBEH_MOV_TARGET_RAND_GOLD: {
        unsigned short inst = s->e_instance[slot];
        float tx = *bt_pool_mvtx(s, inst, node), ty = *bt_pool_mvty(s, inst, node);
        if ((tx == 0.0f && ty == 0.0f) || bt_mov_approx(tx, ty, self_x, self_y)) {
            BtVec2 nt = bt_mov_new_gold_target(s, kind);
            tx = nt.x; ty = nt.y;
            *bt_pool_mvtx(s, inst, node) = tx; *bt_pool_mvty(s, inst, node) = ty;
        }
        return bt_v2(tx - self_x, ty - self_y);
    }

    case BT_INITBEH_MOV_GO_MAP_CENTER:
        /* GoTowardsMapCenterMovementBehavior.get_target_position:
         * ZoneService.get_map_center() = (W/2, H/2). */
        return bt_v2(BT_ARENA_W * 0.5f - self_x, BT_ARENA_H * 0.5f - self_y);

    case BT_INITBEH_MOV_DETECT_TARGET: {
        /* DetectTargetMovementBehavior: on first entry within detection_range,
         * add speed_bonus_on_target_detection to bonus_speed and switch to the
         * after-detection child for good; otherwise run the before child. */
        unsigned short inst = s->e_instance[slot];
        float dx = tgt_x - self_x, dy = tgt_y - self_y;
        if (!(*bt_pool_mvflag(s, inst, node) & 1u) &&
            bt_sqrtf(dx * dx + dy * dy) <= spec->p0) {
            *bt_pool_mvflag(s, inst, node) |= 1u;
            /* RunData.current_run_accessibility_settings.speed == 1.0 */
            s->ebonus[slot] += spec->p1;
        }
        int sub = (*bt_pool_mvflag(s, inst, node) & 1u) ? (int)spec->sub : (int)spec->pre;
        return bt_mov_detect_child(s, slot, node, sub, kind, self_x, self_y);
    }

    default:
        /* BT_INITBEH_MOV_BASE / PATROL_AROUND_MAP / COPY_NEARBY / PLAYER: no
         * enemy kind or state instantiates these (generator-asserted). */
        return bt_v2zero();
    }
}

/* Movement vector for a live enemy slot (unit.gd:214-215 get_movement()).
 * The slot's instance is valid: bt_spawn_kind_at only marks a non-neutral slot
 * alive after bt_enemy_pool_acquire returned a real id (or it returns early).
 *
 * SELF POSITION IS THE PRE-STEP ONE (s->ehx/s->ehy), NOT s->ex/s->ey.
 * Main::iteration (main/main.cpp:2373-2411 @0a9011743) runs, per physics step:
 *   sync(); flush_queries(); main_loop->iteration(); ...; end_sync(); step();
 * so the order inside one iteration is
 *   1. flush_queries -> Space2DSW::call_queries -> Body2DSW::call_queries ->
 *      RigidBody2D::_direct_state_changed (scene/2d/physics_body_2d.cpp:378-395)
 *      which pushes the state left by the PREVIOUS step onto the node
 *      (set_global_transform(state->get_transform())) and then calls the
 *      script's _integrate_forces;
 *   2. the script _physics_process walk (this is where MovementBehavior
 *      .get_movement() evaluates _parent.global_position);
 *   3. Physics2DServer::step() -> Body2DSW::integrate_velocities
 *      (servers/physics_2d/body_2d_sw.cpp:535-561) which moves the body.
 * The node transform read at 2 is therefore always the position the UPCOMING
 * step starts from, while the velocity cached at 2 is only committed to
 * state.linear_velocity by _integrate_forces at 1 of the NEXT iteration
 * (body_2d_sw.cpp:568 integrate_velocities -> X = X + linear_velocity * h).
 * Net effect, for the increment Y_k -> Y_{k+1}, the direction is
 *     normalize(player_position_at_k - Y_{k-1}) * speed,
 * i.e. one step of self lag and no player lag (the player is a MODE_KINEMATIC
 * body whose node transform is already moved by its own Unit._physics_process
 * when the enemy is visited).  Replaying contact161_normal with the newest
 * self position drifts 0.019 px by tick 200; with s->ehx/s->ehy the enemy
 * trajectory matches the live trace to the trace's 1e-3 px print resolution
 * out to the arrival event. */
BT_HD static BtVec2 bt_enemy_move(BT_DEVICE BtState* s, int slot) {
    int kind = (int)s->ekind[slot];
    /* pursuer.gd:69-72 change_movement_behavior_after_x_boosts: after the switch
     * the active MovementBehavior IS $MovementBehaviorOnBoost.  For the only
     * kind that sets it (the DLC viperfish, viperfish.tscn:52-53) that node is
     * follow_target_movement_behavior.gd with every property at its script
     * default, whose raw vector is `current_target - self` -- the same
     * expression the stargazer's angry override returns (this file's
     * bt_mov_stargazer) and the same the FOLLOW_TARGET case below returns. */
    if (s->mv_boost[slot]) return bt_v2(s->px - s->ehx[slot], s->py - s->ehy[slot]);
    int node = bt_mov_node(s, slot);
    BT_CONST_PTR(BtMovSpec) spec = (node == BT_MAX_STATES)
        ? &BT_MOV_ROOT[kind] : &BT_MOV_STATE[kind][node];
    return bt_mov_beh(s, slot, node, spec, kind, s->ehx[slot], s->ehy[slot]);
}
