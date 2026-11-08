/* brotato_scalar_events.h -- player-event consumers for the non-primary item
 * effect keys (ScalarEvents slice).
 *
 * OWNERSHIP
 *   This header owns the *player-event* consumers of the item effect graph:
 *   the keys that `bt_apply_item` preserves into `scalar_effect[]` (or into a
 *   KEY_VALUE bucket) but that no core code reads yet, plus the one
 *   ChanceStatDamageEffect opcode the core switch dropped entirely.
 *   It never re-implements combat: player damage, explosions, stat mutation
 *   and enemy damage all go back through the core helpers
 *   (bt_core_stat_read / bt_core_stat_add / bt_explode / bt_credit_kill /
 *   bt_armor_mult / bt_kunit / bt_chance / bt_rng_bounded).
 *
 *   Placement: included by brotato_core.h AFTER brotato_progression.h and
 *   BEFORE bt_core_step, so every helper it calls is already defined.
 *
 * SOURCE PROVENANCE (brotato_recovered v1.1.14.6 + brotato_abyss)
 *   player.gd:105-106     ItemAttractArea.apply_pickup_range_effect
 *   player.gd:125-126     LoseHealthTimer.start() when the effect is owned
 *   player.gd:411-535     take_damage (dodge / armor / iframes / explode_on_hit)
 *   player.gd:436-471     dodge branch: dmg_on_dodge, heal_on_dodge,
 *                         temp_stats_on_dodge
 *   player.gd:475-480     dmg_taken > 0 -> explode_on_hit effects fire
 *   player.gd:700-756     hp regen tick, on_healing_effect, on_heal_over_time
 *   player.gd:767-786     heal(): dead / no_heal gate
 *   player.gd:833-839     _on_LoseHealthTimer_timeout
 *   player.tscn:1377      LoseHealthTimer (Timer defaults: wait_time 1.0,
 *                         repeating, autostart off)
 *   main.gd:499-503       _on_enemy_died -> dmg_when_death
 *   main.gd:659-693       on_consumable_picked_up -> item_box_gold
 *   main.gd:740-746       gold spawn -> instant_gold_attracting
 *   main.gd:798-854       on_gold_picked_up
 *   main.gd:1500-1571     handle_stat_damages
 *   run_data.gd:829-835   apply_common_gold_pickup_effects (chance_double_gold)
 *   effects/consumables/consumable_healing_effect.gd:5-14
 *   effects/items/chance_stat_damage_effect.gd apply()
 *   singletons/utils.gd:391-403 get_rand_element / get_chance_success
 *   singletons/item_attract_area.gd:6-7 BASE_RADIUS 150, floor 30
 *   singletons/weapon_service.gd:493-495 apply_damage_bonus
 *
 * ---------------------------------------------------------------------------
 * AUDIT -- emitted scalar keys and their consumers
 * ---------------------------------------------------------------------------
 * A key is only "implemented" when a consumer reads it. Everything the core
 * currently writes into scalar_effect[] and nobody reads is listed below.
 *
 * (1) IMPLEMENTED HERE (Bull-reachable, previously dropped)
 *   lose_hp_per_second        blood_donation            bt_scalar_lose_hp_tick
 *   consumable_heal           lemonade/weird_food/      bt_scalar_consumable_heal
 *                             alien_worm (all/)
 *   consumable_heal_over_time dlc jerky (REPLACE=4)     bt_scalar_consumable_heal
 *   heal_when_pickup_gold     cute_monkey               bt_scalar_gold_pickup
 *   dmg_when_pickup_gold      baby_elephant             bt_scalar_gold_pickup
 *   dmg_when_death            cyberball                 bt_scalar_on_kill
 *   dmg_on_dodge              riposte                   bt_scalar_on_dodge
 *   heal_on_dodge             adrenaline                bt_scalar_on_dodge
 *   chance_double_gold        metal_detector            bt_scalar_gold_pickup
 *   item_box_gold             bag                       bt_scalar_item_box_gold
 *   pickup_range              alien_tongue/gnome/       bt_scalar_attract_radius
 *                             little_frog
 *   instant_gold_attracting   baby_gecko/sifds_relic    bt_scalar_gold_spawn_attract
 *   explosion_damage          dynamite/explosive_shells bt_scalar_explosion_damage_mult
 *                             /honey (all/)
 *   explosion_size            plastic_explosive/        bt_item_expl_node_scale
 *                             explosive_shells/honey     (item_explosions.h)
 *   alien_eyes                alien_eyes (op25)         bt_scalar_alien_eyes_tick
 *   projectiles_on_death      baby_with_a_beard (op25)  bt_scalar_proj_on_death
 *
 * (2) ALREADY CONSUMED elsewere in the core (unchanged, listed for the map)
 *   xp_gain, harvesting_growth, level_upgrades_modifications, free_rerolls,
 *   reroll_price, items_price, hp_start_wave, hp_start_next_wave,
 *   burning_cooldown_reduction, burning_cooldown_increase,
 *   structures_cooldown_reduction, structure_attack_speed, hp_regen_bonus,
 *   remove_speed, bonus_damage_against_targets_above_hp, giant_crit_damage,
 *   burning_enemy_hp_percent_damage, gold_on_crit_kill,
 *   gain_random_primary_stats_on_go_to_next_wave, instant_gold_attracting
 *   (its Sifd'sRelic +1-dodge curse branch), and the stat caps
 *   hp_cap/speed_cap/dodge_cap/crit_chance_cap (stat_cap[]).
 *   Bucket keys with a core consumer: stats_end_of_wave, stats_on_level_up,
 *   consumable_stats_while_max, decaying_stats_on_consumable/hit,
 *   gain_stat_for_killed_enemies_while_burning.
 *
 * (3) WEAPON-ONLY (Bull has bull_effect_5 weapon_slot = 0 and no weapons are
 *     simulated, so these can never fire in this core)
 *   accuracy, all_weapons_count_for_sets, bonus_damage_against_targets_*,
 *   bonus_non_elemental_damage_against_burning_targets, bounce,
 *   burn_chance/burning_spread/burning_enemy_speed/can_burn_enemies,
 *   enemy_percent_damage_taken, explode_on_overkill,
 *   gold_on_crit_kill, heal_on_crit_kill, heal_on_kill, knockback,
 *   loot_alien_speed, maximum/minimum_weapon_cooldowns, min/max_weapon_tier,
 *   modify_every_x_projectile, negative_knockback, piercing, piercing_damage,
 *   projectiles (its ONLY producer is items/characters/renegade, a character and
 *   so not in the pool; it is read by _set_common_ranged_stats which only
 *   ranged WEAPON inits call -- no reachable producer at all here),
 *   reload_when_pickup_gold, scale_materials_with_distance, slow_on_hit,
 *   specific_items_price, weapon_class_bonus, weapon_scaling_stats,
 *   weapon_slot/weapon_slot_upgrades, weapon_type_bonus, stat_beast_master_effect,
 *   stat_boosted_wanted_item_tag, stat_double_lifesteal_bonus, one_shot_on_hit,
 *   exploding_* weapon variants, upgrade_random_weapon,
 *   gain_stat_when_attack_killed_enemies, curse_locked_items,
 *   weapons_price (shop-owned), upgraded_baits, traveling items.
 *   PET/STRUCTURE keys are PetMotionZones' (stat_pets, structures,
 *   structure_range, structures_can_crit, stat_has_lootworm, items price of
 *   lootworm...).
 *
 * (4) SHOP / PROGRESSION (owned by brotato_progression.h)
 *   items_price, reroll_price, free_rerolls, level_upgrades_modifications,
 *   harvesting_growth, gain_pct_gold_start_wave, recycling_gains,
 *   increase_tier_on_reroll, guaranteed_shop_items, remove_shop_items,
 *   duplicate_item, extra_item_in_crate, extra_item_in_crate(RANDOM),
 *   minimum_weapons_in_shop, stats_next_wave, convert_stats_end_of_wave
 *   (demon char), convert_stats_half_wave (cyborg char), stats_on_level_up,
 *   xp_gain.
 *
 * (5) OUT OF TARGET -- CHARACTER-ONLY producers. Their keys are not even
 *     interned into BT_ITEM_STR_NAMES because the item pool is items/all + DLC
 *     (characters are applied by bt_core_reset, not bt_apply_item):
 *   hp_shop (Demon: get_player_currency = max_hp instead of gold),
 *   no_heal (Golem), pacifist, cryptid,
 *   dmg_when_heal (Lich), stats_below_half_health (Golem),
 *   next_level_xp_needed (Baby/Mutant/Technomage), percent_materials
 *   (Streamer), no_melee_weapons /
 *   no_ranged_weapons / no_duplicate_weapons (character restrictions),
 *   enemy_gold_drops (cryptid/explorer/fisherman), trees_start_wave
 *   (explorer), disabled item locking, destroy_weapons, item_steals,
 *   hit_protection (its only producer, tardigrade, is in
 *   BT_BULL_BANNED_ITEMS, so the Bull never sees it either).
 *   NOTE: `torture` is NOT in this list -- items/all/torture really does
 *   produce it (torture_effect_2.tres, key torture, value 5), so it is
 *   implemented here (bt_scalar_heal's gate) and in the core regen tick.
 *
 * (6) NO PRODUCER ANYWHERE in the shipped item pool (dead keys; a producer
 *     would have to live in a character/weapon that is not in the pool):
 *   increase_material_value, materials_per_living_enemy, stats_on_fruit,
 *   convert_bonus_gold, temp_consumable_stats_while_max,
 *   explode_on_consumable_burning, more_enemies, less_enemies,
 *   neutral_gold_drops, bonus_current_health_damage,
 *   landmines_on_death_chance (no .tres anywhere; its consumer
 *   weapon_service.gd:535 main.gd:573 additionally requires a WEAPON hitbox
 *   and `burning_did_stat_damage` for stat_elemental_damage, neither of which
 *   the Bull's explosion provides, so it is doubly unreachable).
 *
 * (7) NAMED RESIDUALS -- Bull-reachable keys whose consumer sits in a
 *     subsystem this slice does not own, with the exact source site:
 *   explosion_damage / explosion_size  all/dynamite, explosive_shells, honey,
 *                     plastic_explosive -> the damage MULTIPLIER is here
 *                     (bt_scalar_explosion_damage_mult) and bt_explode /
 *                     bt_item_expl_on_key / bt_item_expl_player_copy call it
 *                     (weapon_service.gd:247,285); the SIZE term is applied by
 *                     bt_item_expl_node_scale on the pooled explosion node
 *                     (projectiles/player_explosion.gd:70).
 *   damage_against_bosses  all/silver_bullet
 *                     -> enemies/boss/boss.gd:55 (any hitbox carrying
 *                        from_player_index, the Bull explosion included).
 *   gold_drops        all/evil_hat  -> main.gd:757 spawn_gold extra drops.
 *   enemy_fruit_drops all/fruit_basket -> item_service.gd:184 drop chance.
 *   number_of_enemies all/candle, gentle_alien, mouse, white_flag
 *                     -> entity_spawner.gd:171 / spawning_attack_behavior.gd:40.
 *   trees             all/tree -> entity_spawner.gd:172 wave-start tree count.
 *   one_shot_trees    all/lumberjack_shirt -> neutral.gd:31.
 *   tree_turrets      all/pocket_factory -> structure slice (PetMotionZones).
 *   explode_on_consumable  all/spicy_sauce
 *                     -> main.gd:696 RunData.handle_explode_effect spawns an
 *                        explosion AT the consumable's position; the core's
 *                        bt_explode is player-centred, so this needs a
 *                        position-based explosion primitive.
 *   temp_stats_on_hit / temp_stats_while_moving / temp_stats_while_not_moving /
 *   temp_stats_on_dodge (all/triangle_of_power, all/adrenaline)
 *                     -> TempStats via bt_core_stat_add_temp (StatSemantics);
 *                        the trigger points are player.gd:471-505.
 *   danger_enemy_health / danger_enemy_damage
 *                     -> difficulty effects only; already folded into the
 *                        fixed danger_hp / danger_dmg at bt_core_init.
 *
 * RNG-ORDER NOTE: every helper draws from the SAME PCG32 stream the source's
 * global randf()/randi() used (bt_chance -> bt_math_randf(&s->rng);
 * bt_rng_bounded(&s->rng, n) for pick_random), in the source call order.
 *
 * NO REWARD: the source's scalar events pay no reward, and a kill they cause
 * is credited exactly like an explosion kill (bt_explode has no reward
 * parameter either), so no helper takes a BT_THREAD float* reward.
 */
#pragma once

/* The gameplay-RNG audio events this slice fires (CombatAudioRuntime):
 * bt_scalar_heal -> on_healing_effect's hp_regen pick, bt_scalar_gold_pickup ->
 * on_gold_picked_up's gold pick, bt_scalar_alien_eyes_fire -> on_alien_eyes_
 * timeout's alien pick.  Included (not forward-declared): brotato_combat_audio.h
 * only needs state + the generated audio table + the two queue headers, so it
 * carries no include-order hazard. */
#include "brotato_combat_audio.h"

/* brotato_item_explosions.h (included LATER in the same TU, after this slice):
 * the player-centred explode_on_hit / explode_when_below_hp pass the self-damage
 * tick must take (brotato_scalar_lose_hp_tick), and its entry count. */
BT_HD static int bt_item_expl_player_nb(BT_DEVICE BtState* s);
BT_HD static int bt_item_expl_player_hit(BT_DEVICE BtState* s, int nb_explosions);

/* consumed by bt_scalar_gold_pickup below */
BT_HD static float bt_scalar_chance_damages(BT_DEVICE BtState* s, unsigned short bucket);
BT_HD static float bt_enemy_incoming_damage(BT_DEVICE BtState* s, int slot, float value);

/* ---------------------------------------------------------------------------
 * Player healing.
 * player.gd:730-745 on_healing_effect: actual = min(value, max_hp - health);
 * player.gd:767-786 heal(): a dead player or no_heal absorbs nothing.
 * Returns the HP actually restored.
 * ------------------------------------------------------------------------- */

/* The raw heal body (no torture gate).  player.gd:774-776 heal() adds the value
 * only when `torture <= 0 or is_from_torture or cleaning_up`, so the regen tick
 * under torture is the ONLY caller that bypasses the gate. */
BT_HD static float bt_scalar_heal_raw(BT_DEVICE BtState* s, float value) {
    if (value <= 0.0f || s->done || s->hp <= 0.0f) return 0.0f;
    /* player.gd:709/732 heal(): room = max_stats.health - current_stats.health,
     * the deferred LiveStats. */
    float room = s->p_max_hp - s->hp;
    if (room <= 0.0f) return 0.0f;
    float actual = value < room ? value : room;
    s->hp += actual;
    /* player.gd:736-738: on_healing_effect plays the hp_regen pick exactly when
     * heal() restored more than 0 HP (this `actual > 0` return), i.e. for every
     * heal this helper performs. */
    bt_combat_audio_player_heal(s);
    return actual;
}

/* player.gd:767-786 heal(): when the `torture` effect is owned (items/all/
 * torture, EFFECT_TORTURE "Restore N HP per second. Cannot heal any other way"),
 * every heal EXCEPT the regen tick's own is absorbed.  The regen tick calls
 * bt_scalar_heal_raw directly. */
BT_HD static float bt_scalar_heal(BT_DEVICE BtState* s, float value) {
    if (s->scalar_effect[BT_ISTR_TORTURE] > 0.0f) return 0.0f;
    return bt_scalar_heal_raw(s, value);
}

/* ---------------------------------------------------------------------------
 * Consumable pickup heal (consumable_healing_effect.gd:5-14).
 *   total = max(0, value + consumable_heal)            -- int maths
 *   if consumable_heal_over_time > 0: heal over time (duration seconds, one HP
 *   per duration/total);
 *   else: heal `total` instantly.
 * `base_heal` is the consumable's own effect value (fruit/item box 3, legendary
 * box 100). Returns HP restored instantly (0 while a heal-over-time chain runs).
 * ------------------------------------------------------------------------- */
BT_HD static float bt_scalar_consumable_heal(BT_DEVICE BtState* s, float base_heal) {
    float total = bt_floorf(base_heal + s->scalar_effect[BT_ISTR_CONSUMABLE_HEAL]);
    if (total <= 0.0f) return 0.0f;
    float duration = s->scalar_effect[BT_ISTR_CONSUMABLE_HEAL_OVER_TIME];
    if (duration > 0.0f) {
        int frames = (int)bt_roundf(duration * 60.0f);
        if (frames > BT_HOT_MAX_FRAMES) { s->stat_reg_overflow = 1; return 0.0f; }
        int healing = (int)total;
        for (int i = 1; i <= healing; i++) {
            int delay = (frames * i + healing - 1) / healing;
            if (delay < 1) delay = 1;
            int index = (s->hot_cursor + delay) % (BT_HOT_MAX_FRAMES + 1);
            s->hot_due[index]++;
        }
        s->hot_pending += healing;
        return 0.0f;
    }
    return bt_scalar_heal(s, total);
}

/* Independent SceneTreeTimer deadlines, quantized to the simulation's 60 Hz
 * frame boundaries. Integer rational deadlines avoid accumulating FP32 drift.
 * player.gd:747-757 creates ONE SceneTreeTimer per point of healing, each
 * calling on_healing_effect(1) -> heal(1) -> the hp_regen pick, so a frame in
 * which several of those deadlines land takes ONE draw+heal PER HEALING POINT,
 * in deadline order (bounded by the scheduled point count; no allocation). */
BT_HD static float bt_scalar_hot_tick(BT_DEVICE BtState* s) {
    if (s->hot_pending <= 0) return 0.0f;
    s->hot_cursor = (s->hot_cursor + 1) % (BT_HOT_MAX_FRAMES + 1);
    int due = s->hot_due[s->hot_cursor];
    s->hot_due[s->hot_cursor] = 0;
    s->hot_pending -= due;
    float healed = 0.0f;
    for (int i = 0; i < due; i++) {
        /* one point per callback, and its own sound draw when it actually heals */
        healed += bt_scalar_heal(s, 1.0f);
    }
    return healed;
}

/* ---------------------------------------------------------------------------
 * Item-box gold (main.gd:665-668): picking up an item box or a legendary item
 * box grants `item_box_gold` materials (bag). Materials only -- no XP.
 * Returns the gold to credit (0 when not a box / nothing owned).
 * ------------------------------------------------------------------------- */
BT_HD static float bt_scalar_item_box_gold(BT_DEVICE BtState* s, int conkind) {
    if (conkind != 1 && conkind != 2) return 0.0f;
    float g = s->scalar_effect[BT_ISTR_ITEM_BOX_GOLD];
    return g > 0.0f ? g : 0.0f;
}

/* ---------------------------------------------------------------------------
 * Gold pickup (main.gd:798-854 + run_data.gd:829-835).
 * Source order: (increase_material_value -- no producer, skipped) ->
 * apply_common_gold_pickup_effects (chance_double_gold x2) ->
 * heal_when_pickup_gold (chance -> heal 1) -> dmg_when_pickup_gold
 * (handle_stat_damages) -> reload_when_pickup_gold (weapon-only) ->
 * materials += value, xp += value.
 * Returns the value to credit to materials AND xp (1:1).
 * ------------------------------------------------------------------------- */
BT_HD static int bt_scalar_gold_pickup(BT_DEVICE BtState* s, int base_value) {
    /* main.gd:801-804: the pickup SOUND pick precedes every other draw of
     * on_gold_picked_up (apply_common_gold_pickup_effects / heal / dmg). */
    bt_combat_audio_gold_pickup(s);
    int boost = 1;
    if (bt_chance(s, s->scalar_effect[BT_ISTR_CHANCE_DOUBLE_GOLD] * 0.01f)) boost = 2;
    int value = base_value * boost;
    if (bt_chance(s, s->scalar_effect[BT_ISTR_HEAL_WHEN_PICKUP_GOLD] * 0.01f))
        (void)bt_scalar_heal(s, 1.0f);
    (void)bt_scalar_chance_damages(s, BT_ISTR_DMG_WHEN_PICKUP_GOLD);
    /* main.gd:817-834 on_gold_picked_up: reload_when_pickup_gold zeroes the
     * cooldown of the eligible weapon (the one with the largest _current_cooldown
     * that is not shooting), of every BuilderTurret carrying the key, and -- the
     * player-level buccaneer boolean -- of every weapon.  Both pickup paths (the
     * player's attractor in brotato_core.h and the Lootworm in brotato_pet_motion.h)
     * call this helper exactly once per pickup, so this is the single hook the
     * shared source handler needs, and it sits exactly where the source orders it:
     * after dmg_when_pickup_gold, before materials/xp.  Zero RNG. */
    bt_weapon_gold_pickup_reload(s);
    return value;
}

/* ---------------------------------------------------------------------------
 * Gold spawned with instant_gold_attracting (main.gd:740-746): a per-gold
 * chance that the player's ItemAttractArea claims it at spawn, so the gold
 * homes in from ANY distance (item.gd:52-70 attracted_by has no range gate).
 * Returns 1 when the roll succeeded. The caller routes the attractor: the
 * source prefers an existing LOOTWORM (main.gd:742-745), which is
 * PetMotionZones' state (mat_by / lootworm slot) -- this helper only performs
 * the roll and must be called once per spawned gold, before mat_by is set.
 * ------------------------------------------------------------------------- */
BT_HD static int bt_scalar_gold_spawn_attract(BT_DEVICE BtState* s) {
    float c = s->scalar_effect[BT_ISTR_INSTANT_GOLD_ATTRACTING];
    if (c == 0.0f) return 0;
    return bt_math_randf(&s->rng) < c * 0.01f ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * ItemAttractArea radius (player.gd:105-106 + item_attract_area.gd:6-7):
 *   radius = max(30, BASE_RADIUS 150 * (1 + pickup_range/100))
 * ------------------------------------------------------------------------- */
BT_HD static float bt_scalar_attract_radius(BT_DEVICE BtState* s) {
    float r = BT_ATTRACT_R * (1.0f + s->scalar_effect[BT_ISTR_PICKUP_RANGE] * 0.01f);
    return r > 30.0f ? r : 30.0f;
}

/* ---------------------------------------------------------------------------
 * Explosion damage modifier -- the Bull's whole identity, and "explosive" is
 * one of its wanted_tags.
 *   weapon_service.gd:241-249 init_base_stats (is_exploding true for the Bull's
 *     init_exploding_stats):
 *       damage = max(1, round(damage * (set_bonus + (1 + dmg_pct/100)
 *                                       + explosion_damage/100)))
 *     i.e. explosion_damage ADDS INTO the percent-damage multiplier (it does
 *     not multiply it again).
 * Consumers: bt_item_expl_on_key / bt_item_expl_player_copy / the Bull's own
 * bt_item_expl_char_explode (brotato_item_explosions.h) all fold it into the
 * spawn damage.
 *
 * The SIZE half of the same feature -- projectiles/player_explosion.gd:68-71
 * set_area(effect.scale): `scale = max(0.1, effect.scale * (1 +
 * explosion_size/100))`, with explosion.tscn's Hitbox radius 147.34 scaling with
 * the node scale -- is consumed by bt_item_expl_node_scale
 * (brotato_item_explosions.h) for every pooled explosion node, the Bull's
 * character row included, so no separate radius helper exists.
 *
 * Producers (all Bull-reachable, none banned): dynamite (+15 damage),
 * explosive_shells (+60 damage, +15 size), honey (+10 damage, +5 size).
 * ------------------------------------------------------------------------- */
BT_HD static float bt_scalar_explosion_damage_mult(BT_DEVICE BtState* s) {
    return 1.0f + s->dmg_pct * 0.01f + s->scalar_effect[BT_ISTR_EXPLOSION_DAMAGE] * 0.01f;
}

/* ---------------------------------------------------------------------------
 * ChanceStatDamageEffect bucket consumer (main.gd:1500-1571 handle_stat_damages).
 * Source order:
 *   1. empty bucket -> return (no RNG);
 *   2. one RANDOM live enemy is picked (Utils.get_rand_element / pick_random),
 *      and if there is none -> return;
 *   3. per bucket entry: `randf() >= chance/100 -> continue` (the draw happens
 *      even at chance 0), then
 *        base = floor(max(1, value/100 * stat(key)))
 *        dmg  = round(base * (1 + stat_percent_damage/100))
 *      (the source caches this per (stat,value); recomputing is identical
 *       because no stat changes inside the call);
 *   4. the summed damage is dealt to that one enemy through the enemy armor
 *      coefficient (max(1, round(total * armor_mult))).
 * `bucket` is the entry's custom_key (dmg_when_pickup_gold / dmg_when_death).
 * Returns the damage actually dealt.
 * ------------------------------------------------------------------------- */

/* unit.gd:493-503 (get_damage_value): the ITEM-sourced
 * bonus_damage_against_targets_above_hp / _below_hp pool -- every owned
 * DOUBLE_VALUE row whose value2% threshold the target's CURRENT health crosses,
 * summed as a percent (they are DoubleValueEffect rows, so bt_apply_item
 * registers them in bk_active, exactly like bt_item_expl_hit_enemy reads them).
 * `hp`/`maxhp` are the target's values BEFORE this blow.  The hitbox-sourced
 * copy of this pool (unit.gd:472-479) needs a hitbox and does not apply here:
 * handle_stat_damages and the dodge riposte both hit through a null hitbox. */
BT_HD static float bt_scalar_target_bonus_pct(BT_DEVICE BtState* s, int j) {
    float pct = 0.0f;
    float hp = s->ehp[j], maxhp = s->emax[j];
    for (int a = 0; a < s->n_bk_active; a++) {
        int r = (int)s->bk_active[a];
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[r];
        int above = bt_eff_is(E, BT_ISTR_BONUS_DAMAGE_AGAINST_TARGETS_ABOVE_HP);
        int below = bt_eff_is(E, BT_ISTR_BONUS_DAMAGE_AGAINST_TARGETS_BELOW_HP);
        if (!above && !below) continue;
        for (int c = 0, n = bt_row_owned(s, r); c < n; c++) {
            int bv = 0, bv2 = 0;
            if (!bt_row_get(s, r, E, c, &bv, &bv2)) break;
            float thr = maxhp * ((float)bv2 * 0.01f);
            if ((above && hp >= thr) || (below && hp <= thr)) pct += (float)bv;
        }
    }
    return pct;
}

/* unit.gd:498: `dmg_value = round(dmg_value * (1 + total_bonus_damage/100))`
 * runs AFTER boss.gd's damage_against_bosses int() multiply and BEFORE the flat
 * armor subtraction, so the second multiplier folds the item above/below-hp
 * pool and the target's own effect-behavior percent together. */
BT_HD static float bt_scalar_apply_target_bonus(BT_DEVICE BtState* s, int j, float value) {
    float pct = bt_scalar_target_bonus_pct(s, j) + (float)bt_enemy_hit_percent_taken(s, j);
    float v = bt_enemy_incoming_damage(s, j, value);
    if (pct != 0.0f) v = bt_roundf(v * (1.0f + pct * 0.01f));
    return v;
}

BT_HD static float bt_scalar_chance_damages(BT_DEVICE BtState* s, unsigned short bucket) {
    int entries = 0;
    for (int a = 0; a < s->n_bk_active; a++) {
        int r = (int)s->bk_active[a];
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[r];
        if (E->opcode != BT_ITEMEFF_CHANCE_STAT_DAMAGE) continue;
        if (E->custom_key != bucket) continue;
        entries += bt_row_owned(s, r);
    }
    if (entries == 0) return 0.0f;

    int count = 0;
    for (int j = bt_enemy_next(s, 0, 1); j < (int)s->e_used; j = bt_enemy_next(s, j + 1, 1)) if (s->ealive[j] && !s->eneutral[j]) count++;
    if (count == 0) return 0.0f;
    /* Utils.get_rand_element == Array.pick_random == ONE randi() % size draw. */
    int pick = (int)bt_rng_bounded(&s->rng, (unsigned int)count);
    int target = -1;
    for (int j = bt_enemy_next(s, 0, 1); j < (int)s->e_used; j = bt_enemy_next(s, j + 1, 1)) {
        if (!s->ealive[j] || s->eneutral[j]) continue;
        if (pick-- == 0) { target = j; break; }
    }
    if (target < 0) return 0.0f;

    float pct_bonus = 1.0f + s->dmg_pct * 0.01f;
    float total = 0.0f;
    for (int a = 0; a < s->n_bk_active; a++) {
        int r = (int)s->bk_active[a];
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[r];
        if (E->opcode != BT_ITEMEFF_CHANCE_STAT_DAMAGE) continue;
        if (E->custom_key != bucket) continue;
        for (int c = 0, n = bt_row_owned(s, r); c < n; c++) {
            int v = 0, v2 = 0;
            if (!bt_row_get(s, r, E, c, &v, &v2)) break;
            if (bt_math_randf(&s->rng) >= (float)E->chance_pct * 0.01f) continue;
            int id = bt_core_id_from_istr(E->key);
            float stat_val = (id != 255) ? bt_core_stat_read(s, id) : 0.0f;
            float base = bt_floorf(bt_maxf(1.0f, (float)v * 0.01f * stat_val));
            total += bt_roundf(base * pct_bonus);
        }
    }
    if (total <= 0.0f) return 0.0f;

    BT_CONST_PTR(BtUnitKind) K = bt_kunit(s, target);
    float armor = K->armor + K->armor_grow * (float)(s->wave - 1);
    float taken = bt_scalar_apply_target_bonus(s, target, total) - armor;
    if (taken < 1.0f) taken = 1.0f;
    s->ehp[target] -= taken;
    if (s->ehp[target] <= 0.0f) bt_mark_enemy_dead(s, target, 1);
    return taken;
}

/* ---------------------------------------------------------------------------
 * Dodge events (player.gd:436-471). Fired only when the dodge roll actually
 * succeeded (bt_player_damage's `dodgeable && randf() < dodge` branch).
 *   dmg_on_dodge (riposte): per entry, chance% -> max(1, value% * stat(key))
 *     raised by WeaponService.apply_damage_bonus, summed, dealt to the
 *     ATTACKER (`hitbox.from`, enemy_slot). Only fires with a valid attacker.
 *   heal_on_dodge (adrenaline): per entry, chance% -> += value, then one heal.
 * ------------------------------------------------------------------------- */
BT_HD static void bt_scalar_on_dodge(BT_DEVICE BtState* s, int enemy_slot) {
    int have_target = (enemy_slot >= 0 && enemy_slot < BT_MAX && s->ealive[enemy_slot]);
    float pct_bonus = 1.0f + s->dmg_pct * 0.01f;

    /* dmg_on_dodge */
    float total_dmg = 0.0f;
    for (int a = 0; a < s->n_bk_active; a++) {
        int r = (int)s->bk_active[a];
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[r];
        if (E->opcode != BT_ITEMEFF_CHANCE_STAT_DAMAGE) continue;
        if (E->custom_key != BT_ISTR_DMG_ON_DODGE) continue;
        for (int c = 0, n = bt_row_owned(s, r); c < n; c++) {
            int v = 0, v2 = 0;
            if (!bt_row_get(s, r, E, c, &v, &v2)) break;
            if (bt_math_randf(&s->rng) >= (float)E->chance_pct * 0.01f) continue;
            int id = bt_core_id_from_istr(E->key);
            float stat_val = (id != 255) ? bt_core_stat_read(s, id) : 0.0f;
            float base = bt_maxf(1.0f, (float)v * 0.01f * stat_val);
            float dmg = bt_maxf(1.0f, bt_roundf(bt_maxf(1.0f, base) * pct_bonus));
            total_dmg += dmg;
        }
    }
    if (have_target && total_dmg > 0.0f) {
        BT_CONST_PTR(BtUnitKind) K = bt_kunit(s, enemy_slot);
        float armor = K->armor + K->armor_grow * (float)(s->wave - 1);
        float taken = bt_scalar_apply_target_bonus(s, enemy_slot, total_dmg) - armor;
        if (taken < 1.0f) taken = 1.0f;
        s->ehp[enemy_slot] -= taken;
        if (s->ehp[enemy_slot] <= 0.0f) bt_credit_kill(s, enemy_slot, 1);
    }

    /* heal_on_dodge */
    float total_heal = 0.0f;
    for (int a = 0; a < s->n_bk_active; a++) {
        int r = (int)s->bk_active[a];
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[r];
        if (E->opcode != BT_ITEMEFF_CHANCE_STAT_DAMAGE) continue;
        if (E->custom_key != BT_ISTR_HEAL_ON_DODGE) continue;
        for (int c = 0, n = bt_row_owned(s, r); c < n; c++) {
            int v = 0, v2 = 0;
            if (!bt_row_get(s, r, E, c, &v, &v2)) break;
            if (bt_math_randf(&s->rng) >= (float)E->chance_pct * 0.01f) continue;
            total_heal += (float)v;
        }
    }
    if (total_heal > 0.0f) (void)bt_scalar_heal(s, total_heal);
}

/* ---------------------------------------------------------------------------
 * Enemy died by the player (main.gd:499-503 _on_enemy_died): dmg_when_death
 * runs handle_stat_damages against one random live enemy. Called once per
 * credited player kill.
 * ------------------------------------------------------------------------- */
BT_HD static float bt_scalar_on_kill(BT_DEVICE BtState* s) {
    return bt_scalar_chance_damages(s, BT_ISTR_DMG_WHEN_DEATH);
}

/* ---------------------------------------------------------------------------
 * LoseHealthTimer tick -> self damage (player.gd:125-126, 833-839, 411-535).
 * The Timer has Timer defaults (player.tscn:1377): wait_time 1.0 s, repeating,
 * started in _ready, so the first tick lands 1.0 s into the wave and every
 * second after. The core derives the phase from tick_in_wave (same convention
 * as entity_spawner's StructureTimer).
 * take_damage is called with dodgeable=false, armor_applied=false,
 * bypass_invincibility=true:
 *   - NO dodge roll, NO armor coefficient, and NO iframe is consumed or
 *     respected (the player takes the raw value even mid-invincibility);
 *   - hp -= clamp(value, 0, hp)  -> the blow can kill;
 *   - dmg_taken > 0 fires explode_on_hit, i.e. the Bull's REAL explosion
 *     (player.gd:475-480 -> explode() -> explosion.tscn), not a DPS grant.
 * Returns the HP lost (0 when the effect is not owned or the frame is off).
 *
 * DELIBERATE NON-REPRODUCTION: the source keeps ONE shared `_take_damage_args`
 * and only ever sets bypass_invincibility=true, so once this timer has fired,
 * every later `damage_effect` (consumable damage etc.) also bypasses iframes.
 * That leak is not reproduced: enemy/projectile damage keeps its iframe rules.
 * ------------------------------------------------------------------------- */
BT_HD static float bt_scalar_lose_hp_tick(BT_DEVICE BtState* s) {
    if (s->phase != 0 || s->done) return 0.0f;
    if (s->tick_in_wave <= 0 || (s->tick_in_wave % 60) != 0) return 0.0f;
    float value = bt_floorf(s->scalar_effect[BT_ISTR_LOSE_HP_PER_SECOND]);
    if (value <= 0.0f) return 0.0f;
    float taken = value;
    if (taken > s->hp) taken = s->hp;
    if (taken <= 0.0f) return 0.0f;
    s->hp -= taken;
    s->dmg_taken += taken;
    /* player.gd:475-485 (dmg_taken > 0): the explode_on_hit / explode_when_below_hp
     * pass.  This tick damages the player DIRECTLY (bypass_invincibility, no
     * bt_player_damage call), so the pass is taken here: the character's own row
     * first, then every owned item copy through the canonical list -- never the
     * removed `extra_explosions` repeat. */
    {
        int nb = bt_item_expl_player_nb(s);
        if (bt_character_explode_on_hit(s->character)) (void)bt_explode(s);
        if (nb > 0) (void)bt_item_expl_player_hit(s, nb);
    }
    return taken;
}

/* ---------------------------------------------------------------------------
 * BUCKET REGISTRATION HOOK -- call from bt_apply_item's opcode switch for the
 * rows this slice consumes:
 *
 *     case BT_ITEMEFF_CHANCE_STAT_DAMAGE:
 *         bt_scalar_bucket_register(s, row, nv, 0);
 *         break;
 *     case BT_ITEMEFF_PROJECTILE:
 *         if (E->key == BT_ISTR_ALIEN_EYES || E->key == BT_ISTR_PROJECTILES_ON_DEATH)
 *             bt_scalar_bucket_register(s, row, nv, (int)bt_roundf((cursed ? mod : 0.0f) * 100.0f));
 *         else
 *             bt_add_projectile_emitter(s, it, cursed ? mod : 0.0f);
 *         break;
 *
 * `nv` is the (possibly curse-boosted) Effect.value; `mod_pct` is that
 * effect's curse modifier in percent, or 0. ChanceStatDamageEffect.apply
 * (effects/items/chance_stat_damage_effect.gd:16) pushes
 * [key, value, chance, tracking] into effects[custom_key] and
 * ProjectileEffect.apply (effects/items/projectile_effect.gd:14-24) accumulates
 * value into effects[key] -- both are stored in the existing bucket arrays
 * (bk_active / bk_count / rowvar), so every owned copy keeps its own
 * curse-transformed value and the consumers read key/chance/tracking/scaling
 * straight off the row.
 * ------------------------------------------------------------------------- */
BT_HD static void bt_scalar_bucket_register(BT_DEVICE BtState* s, int row, int nv, int mod_pct) {
    if (s->bk_count[row] == 0 && s->n_bk_active < BT_NITEM_EFF_ROWS)
        s->bk_active[s->n_bk_active++] = (unsigned short)row;
    s->bk_count[row]++;
    bt_row_variant_add(s, row, nv, mod_pct);
}

/* ---------------------------------------------------------------------------
 * ProjectileEffect rows whose behaviour is NOT "fire at the nearest enemy"
 * (which is how bt_add_projectile_emitter models the plain `projectiles` key):
 *   alien_eyes          player.gd:131-137 + 850-875
 *   projectiles_on_death main.gd:507-525
 * Both are Bull-reachable campaign items (items/all/alien_eyes,
 * items/all/baby_with_a_beard) and both must go through the core projectile
 * store (bt_spawn_projectile), never a synthetic DPS.
 * ------------------------------------------------------------------------- */

/* weapon_service.gd:489-495 + init_base_stats: the damage a
 * manage_special_spawn_projectile shot carries:
 *   max(1, round(max(1, base + sum_scaling_stat_values(scaling)) * (1 + pct_dmg/100)))
 * (set_bonus_dmg is the unsimulated set/weapon-type term). */
BT_HD static float bt_scalar_special_proj_damage(BT_DEVICE BtState* s, unsigned short wrow, float mod) {
    if (wrow >= BT_NITEM_WSTATS) return 1.0f;
    BT_CONST_PTR(BtItemWeaponStats) W = &BT_ITEM_WSTATS[wrow];
    /* dlc_1_data.gd:379-383: under curse the BASE damage is ceil(damage*(1+mod))
     * before apply_scaling_stats_to_damage; bt_scaling_damage only folds the
     * ratio boost into each scaling row. */
    int base_dmg = bt_curse_stat((float)(int)W->damage, mod);
    float d = (float)bt_scaling_damage(s, base_dmg, W->scaling_first, W->scaling_count, mod);
    d = bt_roundf(d * bt_summon_percent_bonus(s, 0, 0));
    return d < 1.0f ? 1.0f : d;
}

/* init_ranged_stats -> _set_common_ranged_stats: the projectile speed a special
 * spawn uses (the scene radius comes from BT_ITEM_SCENES when the WeaponStats
 * names one, else the default player projectile). */
BT_HD static float bt_scalar_special_proj_speed(BT_DEVICE BtState* s, unsigned short wrow) {
    if (wrow >= BT_NITEM_WSTATS) return 500.0f;
    BT_CONST_PTR(BtItemWeaponStats) W = &BT_ITEM_WSTATS[wrow];
    float speed = (float)(W->projectile_speed > 0 ? W->projectile_speed : 500);
    if (W->increase_proj_speed) {
        float rs = bt_core_stat_read(s, 18);
        speed = speed + (speed / 300.0f) * rs;
        if (speed < 50.0f) speed = 50.0f;
        if (speed > 6000.0f) speed = 6000.0f;
    }
    return speed;
}
BT_HD static float bt_scalar_special_proj_radius(unsigned short wrow) {
    if (wrow >= BT_NITEM_WSTATS) return BT_PROJ_R_DEFAULT;
    unsigned short scene = BT_ITEM_WSTATS[wrow].projectile_scene;
    if (scene != BT_ISTR_NONE)
        for (int q = 0; q < BT_NITEM_SCENES; q++)
            if (BT_ITEM_SCENES[q].scene == scene && BT_ITEM_SCENES[q].hit_radius > 0.0f)
                return BT_ITEM_SCENES[q].hit_radius;
    return BT_PROJ_R_DEFAULT;
}
/* per-row owned count + the curse modifier recorded at registration */
BT_HD static int bt_scalar_proj_row_params(BT_DEVICE BtState* s, int row,
                                           BT_CONST_PTR(BtItemEff) E, BT_THREAD float* mod_out) {
    int count = 0;
    float mod = 0.0f;
    if (s->nvar[row] == 0) {
        count = (int)E->value * (int)s->bk_count[row];
    } else {
        for (int i = 0; i < (int)s->nvar[row]; i++) {
            count += (int)bt_rvar(s,row)[i].value * (int)bt_rvar(s,row)[i].count;
            float m = (float)bt_rvar(s,row)[i].value2 * 0.01f;
            if (bt_fabsf(m) > bt_fabsf(mod)) mod = m;
        }
    }
    *mod_out = mod;
    return count;
}

/* One alien-eyes emission (player.gd:850-875): `count` projectiles evenly
 * spaced around the player, auto_target_enemy false. Returns the shots spawned. */
BT_HD static float bt_scalar_alien_eyes_fire(BT_DEVICE BtState* s) {
    /* player.gd:853-857: init_ranged_stats draws nothing, the alien_sounds pick
     * then happens ONCE before the per-projectile loop.  The caller
     * (bt_scalar_alien_eyes_tick) only reaches here while alien_eyes is owned,
     * which is exactly when the source creates the timer. */
    bt_combat_audio_alien_eyes(s);
    int fired = 0;
    for (int a = 0; a < s->n_bk_active; a++) {
        int r = (int)s->bk_active[a];
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[r];
        if (E->opcode != BT_ITEMEFF_PROJECTILE || E->key != BT_ISTR_ALIEN_EYES) continue;
        if (E->stats == BT_ITEM_EFF_NONE || E->stats >= BT_NITEM_WSTATS) continue;
        float mod = 0.0f;
        int count = bt_scalar_proj_row_params(s, r, E, &mod);
        if (count <= 0) continue;
        float dmg = bt_scalar_special_proj_damage(s, E->stats, mod);
        float speed = bt_scalar_special_proj_speed(s, E->stats);
        float rad = bt_scalar_special_proj_radius(E->stats);
        for (int i = 0; i < count; i++) {
            float ang = (2.0f * BT_PI / (float)count) * (float)i;
            (void)bt_spawn_projectile(s, s->px, s->py, ang, speed, dmg, BT_SHAPE_CIRCLE,
                                      rad, 0.0f, 0.0f, ang, 1);
            fired++;
        }
    }
    return (float)fired;
}

/* AlienEyesTimer cadence (player.gd:131-137): Timer.new() with
 * wait_time = ProjectileEffect.cooldown (the row's projectile_cooldown, in
 * SECONDS) and autostart off, .start()ed in _ready -- so it repeats and the
 * first emission lands one wait_time into the wave. The core derives the phase
 * from tick_in_wave, the same convention the LoseHealthTimer tick uses. */
BT_HD static float bt_scalar_alien_eyes_tick(BT_DEVICE BtState* s) {
    if (s->phase != 0 || s->done) return 0.0f;
    int cd_secs = 0;
    for (int a = 0; a < s->n_bk_active; a++) {
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[s->bk_active[a]];
        if (E->opcode != BT_ITEMEFF_PROJECTILE || E->key != BT_ISTR_ALIEN_EYES) continue;
        cd_secs = (int)E->projectile_cooldown;
        break;
    }
    if (cd_secs <= 0) return 0.0f;
    int period = cd_secs * 60;
    if (s->tick_in_wave <= 0 || (s->tick_in_wave % period) != 0) return 0.0f;
    return bt_scalar_alien_eyes_fire(s);
}

/* ProjectilesOnDeath (main.gd:507-525): on a player enemy kill, `value`
 * projectiles spawn AT the dying enemy with a random direction
 * (rand_range(-PI, PI)). Called once per credited kill, AFTER bt_scalar_on_kill
 * (the source runs dmg_when_death before the projectile loop). */
BT_HD static float bt_scalar_proj_on_death(BT_DEVICE BtState* s, float x, float y) {
    int fired = 0;
    for (int a = 0; a < s->n_bk_active; a++) {
        int r = (int)s->bk_active[a];
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[r];
        if (E->opcode != BT_ITEMEFF_PROJECTILE || E->key != BT_ISTR_PROJECTILES_ON_DEATH) continue;
        if (E->stats == BT_ITEM_EFF_NONE || E->stats >= BT_NITEM_WSTATS) continue;
        float mod = 0.0f;
        int count = bt_scalar_proj_row_params(s, r, E, &mod);
        if (count <= 0) continue;
        float dmg = bt_scalar_special_proj_damage(s, E->stats, mod);
        float speed = bt_scalar_special_proj_speed(s, E->stats);
        float rad = bt_scalar_special_proj_radius(E->stats);
        for (int i = 0; i < count; i++) {
            float ang = bt_rand_range(s, -BT_PI, BT_PI);
            /* main.gd:507-525: manage_special_spawn_projectile runs once per
             * child, so its shooting_sounds pick falls between this iteration's
             * direction draw and the auto-target draw. */
            bt_combat_audio_special_projectile(s, (int)E->stats);
            (void)bt_spawn_projectile(s, x, y, ang, speed, dmg, BT_SHAPE_CIRCLE,
                                      rad, 0.0f, 0.0f, ang, 1);
            fired++;
        }
    }
    return (float)fired;
}

