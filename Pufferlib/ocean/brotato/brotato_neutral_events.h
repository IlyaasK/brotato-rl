/* brotato_neutral_events.h -- neutral (tree) event consumers (NeutralEvents slice).
 *
 * OWNERSHIP
 *   This header owns the *neutral-unit* event rules that no other core slice
 *   implements:
 *     1. the per-tree hit tally and the one_shot_trees death rule
 *        (entities/units/neutral/neutral.gd:4-6,14-17,20-35);
 *     2. the tree_turrets structures queued when a tree dies (main.gd:578-593);
 *     3. the entity_spawner.gd:170-182 spawn-group unit-count modifiers
 *        (number_of_enemies / trees) that the wave group spawner applies.
 *
 *   It never re-implements combat. A caller has ALREADY rolled the hit, applied
 *   armor/knockback/burning and subtracted the damage (s->ehp[slot] -= taken);
 *   this header only performs the neutral bookkeeping (hit tally, one_shot_trees,
 *   death-turret queue, group-count modifiers). It borrows no enemy behaviour or
 *   boost: a neutral is identified by s->eneutral[slot] (the BT_NEUTRALS domain,
 *   via bt_kunit/bt_kgeom) and its count/eelite/ebehavior fields are already 0.
 *
 * ACYCLIC DEATH (CUDA/Metal)
 *   bt_neutral_on_damage only *signals* "this neutral should die now" (returns 1).
 *   It NEVER calls bt_credit_kill / bt_mark_enemy_dead / bt_drain_death_events,
 *   so the depth-first death dispatcher can never re-enter a damage path through
 *   this slice. The caller routes the kill through bt_credit_kill itself.
 *   bt_neutral_death_turrets is a pure queue push (bt_queue_struct): no dispatch,
 *   no recursion.
 *
 * PLACEMENT
 *   Included by brotato_core.h immediately AFTER the entity_spawner queue helpers
 *   it calls (bt_pend_push / bt_queue_struct / bt_pos_in_area are all defined by
 *   then) and BEFORE every damage path -- bt_explode, the projectile pass, the
 *   burn tick -- and before brotato_pet_motion.h, so the lootworm tree melee can
 *   call it too (all five call sites listed in the hand-off below).
 *
 * REQUIRED STATE ADDITION (parent-owned)
 *   unsigned char ehit[BT_MAX];    per-slot hit tally (neutral.gd:6
 *                                  current_number_of_hits). The parent adds it to
 *                                  BtState and:
 *                                    - zeroes it in bt_spawn_kind_at next to
 *                                      eburn/eburndmg (neutral.gd:17 respawn()
 *                                      resets current_number_of_hits on a
 *                                      recycled slot), and
 *                                    - zeroes it in bt_core_reset
 *                                      (bt_zero_u8(s->ehit, BT_MAX)).
 *   The field is read/written through the BT_NEUTRAL_HITS_FIELD macro below, so
 *   if the parent names it differently it only needs to
 *   `#define BT_NEUTRAL_HITS_FIELD <name>` before including this header.
 *
 * SOURCE PROVENANCE (brotato_recovered v1.1.14.6 + brotato_abyss)
 *   entities/units/neutral/neutral.gd:4     number_of_hits_before_dying = 8
 *   entities/units/neutral/neutral.gd:6     current_number_of_hits
 *   entities/units/neutral/neutral.gd:14-17 respawn() -> reset current_number_of_hits
 *   entities/units/neutral/neutral.gd:20-35 take_damage hit rule
 *   entities/units/neutral/neutral.gd:38-40 die() -> trees_killed (ProgressData)
 *   entities/units/neutral/neutral.tscn:16  number_of_hits_before_dying = 8
 *   entities/units/neutral/tree.tscn        instances neutral.tscn, no override
 *   dlcs/dlc_1/neutrals/tree/tree.tscn      instances neutral.tscn, no override
 *   singletons/debug_service.gd:9           one_shot_enemies = false (default)
 *   singletons/debug_service.gd:33          nb_enemies_mult = 1.0
 *   singletons/run_data.gd:427-435          get_player_effect / _bool / sum_all
 *   main.gd:578-581                         current_living_trees -= 1 (no
 *                                           Bull-reachable consumer: the only
 *                                           living_tree producer is the cryptid
 *                                           CHARACTER, not in the item pool)
 *   main.gd:583-593                         tree_turrets structures on tree death
 *   main.gd:1439                            neutral respawn -> living_trees += 1
 *   global/entity_spawner.gd:170-182        group unit-count modifiers
 *   global/entity_spawner.gd:468-476        get_spawn_pos_in_area(base, 200)
 *   singletons/zone_service.gd:117-125      get_rand_pos_in_area
 *   main.tscn:124                           turret_effect = id 99
 *   items/all/turret/turret_effect_1.tres   the resource main.gd:590 queues
 *   items/all/lumberjack_shirt/lumberjack_shirt_effect_1.tres
 *                                           one_shot_trees value 1, SUM storage
 *   items/all/pocket_factory/pocket_factory_effect_1.tres
 *                                           tree_turrets value 1, SUM storage
 *   items/all/candle/candle_effect_3.tres   number_of_enemies -10, SUM
 *   items/all/mouse/mouse_effect_2.tres     number_of_enemies +10, SUM
 *   items/all/white_flag/white_flag_effect_2.tres   number_of_enemies -5, SUM
 *   items/all/gentle_alien/gentle_alien_effect_2.tres number_of_enemies +5, SUM
 *   items/all/tree/tree_effect_1.tres       trees +1, SUM
 *   (lumberjack_shirt=79, candle=30, mouse=87, white_flag=142, gentle_alien=56,
 *    tree=127 and pocket_factory=173 are all outside BT_BULL_BANNED_ITEMS
 *    {36,157,216,57,180,186,232,152}, so every key here is Bull-reachable.)
 *
 * AUDIT -- what is NOT done here
 *   - main.gd:580/1439 RunData.current_living_trees has no Bull-reachable
 *     consumer (only the cryptid character keys "living_tree"), so it is
 *     deliberately not tracked.
 *   - neutral.gd:40 ProgressData "trees_killed" is a persistent cross-run stat,
 *     which this simulation has no analogue for.
 *   - DebugService.one_shot_enemies (neutral.gd:32) defaults false and is folded
 *     to 0; there is no debug switch in this core.
 *   - The neutral's own gold/consumable drop (main.gd:597-638 spawn_loot called
 *     with EntityType.NEUTRAL) is the core event dispatcher's (bt_mark_enemy_dead);
 *     that path must NOT apply the enemy wave<5 / drop-chance gate to a neutral
 *     (main.gd:614) and must honour gold_drops / neutral_gold_drops
 *     (main.gd:751-789). Flagged for the parent, not implemented here.
 *
 * RNG ORDER
 *   bt_neutral_death_turrets draws its spawn positions from s->rng
 *   (bt_rand_range -> bt_rng_global_range_float), the stream the source's global
 *   rand_range() uses. The source queues the turrets synchronously at death and
 *   call_deferred()s spawn_loot, so queue the turrets BEFORE the caller's
 *   bt_credit_kill to keep the same draw order.
 */
#pragma once

/* neutral.gd:4 / neutral.tscn:16 number_of_hits_before_dying. BOTH tree scenes
 * (campaign + DLC) instance neutral.tscn without overriding it, so one constant
 * is exact for every shipped neutral. */
#define BT_NEUTRAL_HITS_BEFORE_DYING 8

/* main.gd:589 _entity_spawner.get_spawn_pos_in_area(neutral.global_position, 200)
 * -> ZoneService.get_rand_pos_in_area(base, 200) (zone_service.gd:117). */
#define BT_NEUTRAL_TURRET_SPAWN_AREA 200.0f

/* DebugService.nb_enemies_mult (debug_service.gd:33 = 1.0). */
#define BT_NB_ENEMIES_MULT 1.0f

/* The effect row of res://items/all/turret/turret_effect_1.tres -- the resource
 * main.gd:590 queues (main.tscn:124 turret_effect = id 99). It is the single
 * effect row of the "turret" item, so this derivation from the generated pool is
 * exact (currently 336); no value is invented. Generator requirement: emit
 * BT_ITEM_EFF_ROW_TREE_TURRET = 336 to name it directly; when present it wins. */
#ifndef BT_ITEM_EFF_ROW_TREE_TURRET
#define BT_ITEM_EFF_ROW_TREE_TURRET \
    ((unsigned short)BT_ITEM_EFF_INDEX[BT_ITEMS[BT_ITEM_IDX_TURRET].eff_first])
#endif

/* Per-slot neutral hit tally field on BtState (see REQUIRED STATE ADDITION). */
#ifndef BT_NEUTRAL_HITS_FIELD
#define BT_NEUTRAL_HITS_FIELD ehit
#endif
#define BT_NEUTRAL_HITS(s, slot) ((s)->BT_NEUTRAL_HITS_FIELD[(slot)])

/* neutral.gd:23 current_number_of_hits += 1. Runs on EVERY damage instance that
 * reaches a live neutral -- including the ones the neutral.gd:27-28 early return
 * suppresses below. Clamped so a long-lived neutral can never wrap the counter. */
BT_HD static void bt_neutral_hit_tally(BT_DEVICE BtState* s, int slot) {
    if (BT_NEUTRAL_HITS(s, slot) < 255) BT_NEUTRAL_HITS(s, slot)++;
}

/* RunData.get_player_effect_bool(one_shot_trees, player_index) (run_data.gd:432):
 * the per-player accumulated effect value > 0. The item Effect has SUM storage
 * (items/all/lumberjack_shirt/lumberjack_shirt_effect_1.tres), which the unmapped
 * key branch of bt_apply_item accumulates into scalar_effect[BT_ISTR_ONE_SHOT_TREES].
 * The core simulates one player, so scalar_effect holds that player's value. */
BT_HD static int bt_neutral_one_shot_trees(BT_DEVICE BtState* s, int player_index) {
    return player_index >= 0 && s->scalar_effect[BT_ISTR_ONE_SHOT_TREES] > 0.0f;
}

/* neutral.gd:20-35 take_damage, expressed without killing the slot.
 *
 * Precondition: `slot` holds a live neutral whose `taken` damage has ALREADY been
 * subtracted from s->ehp[slot]. `from_player_index` is the hit's
 * TakeDamageArgs.from_player_index (-1 when the source has none) and `has_from`
 * is 1 when args.hitbox != null && args.hitbox.from != null.
 *
 * Returns 1 when the caller must kill the slot via bt_credit_kill(s, slot, 1);
 * returns 0 otherwise. A hit that dropped HP to <= 0 also returns 0 -- the caller
 * credits that death through its own ehp <= 0 test, exactly like neutral.gd:25-26
 * ("if dead: return result" after the parent .take_damage already scheduled die).
 * This function never kills, never credits gold and never touches the death
 * dispatcher. */
BT_HD static int bt_neutral_on_damage(BT_DEVICE BtState* s, int slot,
                                      int from_player_index, int has_from) {
    if (!s->ealive[slot] || !s->eneutral[slot]) return 0;
    bt_neutral_hit_tally(s, slot);                 /* neutral.gd:23          */
    if (s->ehp[slot] <= 0.0f) return 0;            /* neutral.gd:25-26 (HP death) */
    /* neutral.gd:27-28: a hit whose `from` exists but is not a player-indexed
     * Object (no "player_index", or player_index == -1) never triggers the
     * count / one_shot_trees death -- the hit is still tallied above. */
    if (has_from && from_player_index < 0) return 0;
    if (BT_NEUTRAL_HITS(s, slot) >= BT_NEUTRAL_HITS_BEFORE_DYING) return 1;  /* :30 */
    /* neutral.gd:31 (the :32 DebugService.one_shot_enemies term is false here). */
    if (has_from && bt_neutral_one_shot_trees(s, from_player_index)) return 1;
    return 0;
}

/* main.gd:583-593 _on_neutral_died: for every owned tree_turrets effect, queue
 * one turret STRUCTURE at a random point within 200 of the tree. The turret is
 * the canonical pocket_factory turret (BT_ITEM_EFF_ROW_TREE_TURRET), queued as a
 * runtime entity (kind 0xFFFF -> bt_spawn_summon_row via the EntityBirth
 * lifecycle); it is never registered in structures_hash and carries no curse
 * modifier, exactly like the source's exported main.tscn turret_effect. The
 * emulated StructureTimer birth and the MAX_STRUCTURES eviction
 * (bt_enforce_structure_cap) then apply unchanged, and the turret starts ready
 * with its real WeaponStats (pet_timer 0, bt_spawn_summon_row) -- no immediate
 * fake-DPS activation.
 *
 * Safe to call after the slot has died: only s->ex[slot] / s->ey[slot] are read.
 * Returns the number of turrets queued (0 when tree_turrets is not owned or the
 * structure queue is full -- a full queue sets s->pend_overflow, never a silent
 * drop). */
BT_HD static int bt_neutral_death_turrets(BT_DEVICE BtState* s, int slot) {
    int n = (int)s->scalar_effect[BT_ISTR_TREE_TURRETS];
    if (n <= 0) return 0;
    int queued = 0;
    for (int i = 0; i < n; i++) {
        float x, y;
        /* entity_spawner.gd:468-476 get_spawn_pos_in_area(pos, 200). */
        bt_pos_in_area(s, s->ex[slot], s->ey[slot],
                       BT_NEUTRAL_TURRET_SPAWN_AREA, 0.0f, 0, &x, &y);
        if (bt_queue_struct(s, 0xFFFF, BT_ITEM_EFF_ROW_TREE_TURRET, x, y)) queued++;
    }
    return queued;
}

/* Recommended single entry point for a damage site. Call it right after the hit
 * has been applied to s->ehp[slot], for EVERY slot (it no-ops on an enemy or a
 * dead slot). It performs the whole neutral resolution in the source order and
 * returns 1 when the caller must credit the kill through bt_credit_kill(s, slot, 1);
 * when it returns 1 the tree_turrets structures are ALREADY queued (main.gd:583-593
 * queues them at death, before the deferred spawn_loot), so the site is just:
 *
 *     int kill = bt_neutral_resolve_hit(s, j, from_player_index, has_from);
 *     if (s->ehp[j] <= 0.0f || kill) bt_credit_kill(s, j, 1);
 *
 * Using this instead of open-coding bt_neutral_on_damage keeps the tally from
 * being skipped by a `s->ehp[j] <= 0.0f ||` short-circuit (neutral.gd:23 runs
 * before the dead check). It never calls bt_credit_kill itself. */
BT_HD static int bt_neutral_resolve_hit(BT_DEVICE BtState* s, int slot,
                                        int from_player_index, int has_from) {
    if (!s->eneutral[slot]) return 0;
    int die = bt_neutral_on_damage(s, slot, from_player_index, has_from);
    if (s->ehp[slot] <= 0.0f || die) {
        bt_neutral_death_turrets(s, slot);
        return 1;
    }
    return 0;
}

/* entity_spawner.gd:171 enemy_modifier = sum_all_player_effects(number_of_enemies)/100.0.
 * Read from the scalar bucket the unmapped key branch of bt_apply_item fills. */
BT_HD static float bt_neutral_enemy_count_modifier(BT_DEVICE BtState* s) {
    return s->scalar_effect[BT_ISTR_NUMBER_OF_ENEMIES] * 0.01f;
}

/* entity_spawner.gd:172 tree_modifier = sum_all_player_effects(trees). */
BT_HD static float bt_neutral_tree_count_modifier(BT_DEVICE BtState* s) {
    return s->scalar_effect[BT_ISTR_TREES];
}

/* entity_spawner.gd:175-182 unit-count transform for one WaveUnitData roll:
 *     number  = randi_range(min_number, max_number) as float
 *     number *= DebugService.nb_enemies_mult
 *     ENEMY and not group_data.is_loot:
 *         number += number * coop_factor
 *         number  = max(1.0, number + number * enemy_modifier)
 *     NEUTRAL:
 *         number += tree_modifier
 * `domain` is BtSpawnUnit.domain (0 = BT_KINDS enemy, 1 = BT_NEUTRALS tree),
 * `coop_factor` = (get_player_count()-1) * CoopService.additional_enemies_per_coop_player
 * (0 for the single-player env) and `number` is the raw randi_range draw. The
 * spawn_chance floor/residual roll (entity_spawner.gd:184-187) is NOT applied
 * here; it stays in the group spawner. No RNG is consumed: the modifier is
 * applied to the already-rolled count, exactly where the source applies it. */
BT_HD static float bt_neutral_group_number(BT_DEVICE BtState* s, int domain, int is_loot,
                                           float coop_factor, float number) {
    number *= BT_NB_ENEMIES_MULT;                              /* :176 */
    if (domain == 0 && !is_loot) {
        number += number * coop_factor;                        /* :179 */
        float m = bt_neutral_enemy_count_modifier(s);
        number = number + number * m;                          /* :180 */
        if (number < 1.0f) number = 1.0f;                      /* max(1.0, ...) */
    } else if (domain == 1) {
        number += bt_neutral_tree_count_modifier(s);           /* :182 */
    }
    return number;
}
