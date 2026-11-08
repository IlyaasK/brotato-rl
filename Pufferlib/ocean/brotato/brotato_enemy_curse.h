/* brotato_enemy_curse.h -- DLC cursed-enemy runtime for the shared Brotato core.
 *
 * OWNERSHIP
 *   The whole cursed-enemy slice: the per-enemy curse roll/attach/boost on
 *   spawn, the cursed gold drop value modifier, the `can_be_boosted = false`
 *   consequence and the DIRECT gold_on_cursed_enemy_kill payout.  It owns its
 *   own generated per-kind tables (BT_KIND_CAN_BE_CURSED / BT_KIND_CURSE_ROLLS)
 *   and tuning constants, produced by the sibling extract_enemy_curse.py.
 *
 *   It does NOT re-implement the enemy stat layer: the boost recomputes the
 *   health / damage / speed through the SAME entity_service.gd path the core's
 *   spawn and buffer boost use (brotato_enemy_modifiers.h
 *   bt_enemy_final_health / _damage / _speed), exactly like Enemy.boost's
 *   reset_health/damage/speed_stat(percent).
 *
 * PLACEMENT
 *   Included by brotato_core.h AFTER brotato_enemy_modifiers.h (it calls
 *   bt_enemy_final_*) and after brotato_loot.h (it reuses bt_loot_endless_factor),
 *   and BEFORE brotato_item_explosions.h / the weapon headers.  Because
 *   bt_spawn_kind_at / bt_mark_enemy_dead / bt_buffer_boost run earlier in the
 *   TU, brotato_core.h forward-declares bt_enemy_cursed / bt_enemy_curse_roll /
 *   bt_enemy_curse_gold_value_mult next to the other late helpers.
 *
 * ---------------------------------------------------------------------------
 * SOURCE PROVENANCE (brotato_recovered v1.1.14.6 + brotato_abyss dlc_1)
 * ---------------------------------------------------------------------------
 *   dlcs/dlc_1/effect_behaviors/scene/curse_scene_effect_behavior.gd
 *     :15-20  _ready connects every EntitySpawner `enemy_respawned`
 *     :50-65  _on_EntitySpawner_enemy_respawned(enemy):
 *               if enemy is Boss or enemy.stats in _loot_alien_stats: return
 *               curse = sum over players of Utils.get_stat(stat_curse)
 *               curse_chance = sum over players of
 *                   min(1, (Utils.get_curse_factor(stat)/100/2)
 *                          * (1 + RunData.get_endless_factor()/2))
 *               if (Utils.get_chance_success(curse_chance / nb_players)
 *                   or DebugService.always_curse or curse_enemy_spawn)
 *                  and enemy.can_be_cursed: _curse_enemy(enemy, curse)
 *             (the roll is the LEFT `and` operand: it draws even when
 *              can_be_cursed is false; a Boss/loot alien returns before the
 *              chance is even computed, i.e. draws nothing)
 *     :8      MAX_CURSE_HP_BOOST = 300
 *     :22-35  _curse_enemy: attach curse_enemy_effect_behavior_data, then
 *               boost_args.hp_boost     = hp_boost + min(curse, 300) * 2
 *               boost_args.damage_boost = damage_boost
 *               boost_args.speed_boost  = speed_boost
 *               boost_args.show_outline = false
 *               enemy.boost(boost_args)     # reset_health/damage/speed(percent)
 *               enemy.can_be_boosted = false
 *   curse_scene_effect_behavior.tscn  hp_boost = 150, damage_boost = 25,
 *                                     speed_boost = 15 (override the .gd
 *                                     defaults 200/50/75)
 *   dlcs/dlc_1/effect_behaviors/enemy/curse_enemy_effect_behavior.gd
 *     :4      CURSED_ENEMIES_BONUS_GOLD = 0.33
 *     :20-21  get_gold_value_modifier() -> 0.33   (main.gd:777-780 sums the
 *             dying unit's effect-behavior modifiers and multiplies the drop)
 *     :24-41  on_taken_damage(args): only when the hit is LETHAL and came from
 *             a player (args.from_player_index != -1, health <= 0):
 *               if gold_on_cursed_enemy_kill > 0:
 *                   RunData.add_gold(value, pid)      # DIRECT: no coin, no XP
 *                   add_tracked_value(item_black_flag, value)
 *                   return HitType.GOLD_ON_CURSED_KILL
 *     :44-45  on_death: _parent.can_be_boosted = true   (per-life: a pooled
 *             node's curse ends with the life and is re-rolled on the next
 *             enemy_respawned)
 *   singletons/utils.gd:406-411 get_curse_factor(value, max_val = 100):
 *               max_val * (1 - 1 / (1 + value / 50))
 *   singletons/run_data.gd:775-783 get_endless_factor(current_wave)
 *   entities/units/enemies/enemy.gd:11 + enemy.tscn:52 can_be_cursed (default
 *             true); the DLC scenes evil_mob / evil_mob_swimmer / looter /
 *             looting_pig / iron_lung / sea_pig export false.
 *
 *   Player count is 1 (coop is outside this core, see brotato_core.h): the
 *   per-player sum/division collapses to the single player's chance.  The
 *   DebugService.always_curse / curse_enemy_spawn flags are debug-only (off),
 *   so the source reduces to `get_chance_success(chance) and can_be_cursed`.
 *
 *   The `bonus_weapon_class_damage_against_cursed_enemies` half of
 *   curse_enemy_effect_behavior.gd:12-19 is NOT produced by any item in this
 *   core's reachable pool: its only source producer is the DLC character
 *   sailor_effect_1.tres, and this core's characters are Bull/Knight/Demon
 *   (no Sailor), so RunData.get_player_effect returns an empty array and
 *   get_bonus_damage is always 0.  Kept out (not a flag-only approximation).
 */
#pragma once

#include "brotato_state.h"

/* ===================== BEGIN GENERATED =====================
 * extract_enemy_curse.py -- DO NOT EDIT below; regenerate with
 *   python3 ocean/brotato/extract_enemy_curse.py \
 *       --campaign-root <recovered> --dlc-root <abyss> --write <this file>
 * =========================================================== */

/* curse_scene_effect_behavior.tscn (the .gd defaults 200/50/75 are
 * overridden by the scene) + .gd MAX_CURSE_HP_BOOST +
 * curse_enemy_effect_behavior.gd CURSED_ENEMIES_BONUS_GOLD. */
#define BT_CURSE_HP_BOOST 150
#define BT_CURSE_DAMAGE_BOOST 25
#define BT_CURSE_SPEED_BOOST 15
#define BT_CURSE_MAX_CURSE_HP_BOOST 300
#define BT_CURSED_ENEMIES_BONUS_GOLD 0.33f

/* entities/units/enemies/enemy.gd:11 can_be_cursed (scene root;
 * enemy.tscn:52 default true). */
BT_TABLE unsigned char BT_KIND_CAN_BE_CURSED[BT_NKINDS] = {
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
    0u,  /* iron_lung */
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

/* curse_scene_effect_behavior.gd:51 the roll/RNG gate: `if enemy is
 * Boss or enemy.stats in _loot_alien_stats: return` -> class 0 and not
 * a loot alien.  1 = _curse_enemy's chance roll runs for this kind. */
BT_TABLE unsigned char BT_KIND_CURSE_ROLLS[BT_NKINDS] = {
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
    1u,  /* sea_pig */
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
/* ====================== END GENERATED ====================== */

/* Is this live slot a cursed enemy?  Per-life: written at spawn, kept after
 * the kill so the killer's weapon hook can still read it, overwritten (to 0 or
 * 1) by the next spawn into the slot. */
BT_HD static int bt_enemy_cursed(BT_DEVICE BtState* s, int slot) {
    return (slot >= 0 && slot < BT_MAX) ? (int)s->ecurse[slot] : 0;
}

/* singletons/utils.gd:406-411 get_curse_factor(value, 100).
 * (DebugService.always_curse is a debug flag and is off.) */
BT_HD static float bt_enemy_curse_factor(float value) {
    return 100.0f * (1.0f - 1.0f / (1.0f + value / 50.0f));
}

/* curse_scene_effect_behavior.gd:55-63, single player (nb_players == 1):
 *   player_curse_chance = min(1, (get_curse_factor(curse)/100/2)
 *                                * (1 + get_endless_factor()/2))
 *   curse_chance / nb_players == that value.
 * The source's get_chance_success(0.0) short-circuits to false WITHOUT a draw,
 * which bt_chance already reproduces (`chance == 0.0f ? 0 : randf() <= chance`),
 * so a zero-curse run consumes no RNG here. */
BT_HD static float bt_enemy_curse_chance(BT_DEVICE BtState* s) {
    float per = bt_enemy_curse_factor(s->curse) / 100.0f / 2.0f;
    per *= 1.0f + bt_loot_endless_factor(s->wave) / 2.0f;
    if (per > 1.0f) per = 1.0f;
    return per;
}

/* gd:22-35 _curse_enemy; defined below, used by bt_enemy_curse_roll. */
BT_HD static void bt_enemy_curse_apply(BT_DEVICE BtState* s, int slot, int kind);

/* entity.gd:100-113 boost (Player: hp/damage/speed percentages) and enemy.gd:
 * 311-317: recompute from the kind BASE with the boost as the percent modifier.
 * The source assigns the sums to the INT BoostArgs fields first, so the hp
 * percentage truncates (hp_boost + min(curse, 300) * 2), exactly as here. */
BT_HD static void bt_enemy_curse_roll(BT_DEVICE BtState* s, int slot, int kind) {
    if (slot < 0 || slot >= BT_MAX) return;
    if (kind < 0 || kind >= BT_NKINDS) return;
    /* gd:51 Boss/elite or loot alien: return BEFORE the chance is computed, so
     * nothing is drawn (BT_KIND_CURSE_ROLLS folds class != 0 and is_loot). */
    if (!BT_KIND_CURSE_ROLLS[kind]) return;
    /* gd:64: the roll happens even when the scene refuses the curse. */
    if (!bt_chance(s, bt_enemy_curse_chance(s))) return;
    if (!BT_KIND_CAN_BE_CURSED[kind]) return;
    bt_enemy_curse_apply(s, slot, kind);
}

/* gd:22-35 _curse_enemy.  Called only for a class-0, non-loot kind (so the
 * boss/elite hp factor does not apply). */
BT_HD static void bt_enemy_curse_apply(BT_DEVICE BtState* s, int slot, int kind) {
    s->ecurse[slot] = 1;
    /* Enemy.boost returns immediately when can_be_boosted is false
     * (enemy.gd:311-312), so a cursed kind whose SCENE exports
     * can_be_boosted = false (entity.gd:7 -> BT_KIND_BOOSTABLE; here stonefish
     * and buffer) keeps its spawn stats: the curse still marks it (gold/kill
     * effects) but the boost is a no-op. */
    if (!BT_KIND_BOOSTABLE[kind]) return;
    float w1 = (float)(s->wave - 1);
    BT_CONST_PTR(BtUnitKind) K = bt_kunit(s, slot);
    float curse = s->curse;
    float mh = curse < (float)BT_CURSE_MAX_CURSE_HP_BOOST
                   ? curse : (float)BT_CURSE_MAX_CURSE_HP_BOOST;
    int hp_pct = (int)((float)BT_CURSE_HP_BOOST + mh * 2.0f);
    /* Enemy.boost -> reset_health/damage/speed_stat(percent): recompute from the
     * kind base through get_final_enemy_* (never a multiply of the final stat),
     * with the same hp<1 / dmg<1 floors bt_spawn_kind_at applies. */
    float hp = bt_enemy_final_health(s, bt_floorf(K->hp + K->hp_grow * w1), (float)hp_pct);
    if (hp < 1.0f) hp = 1.0f;
    float dmg = bt_enemy_final_damage(s, K->dmg + K->dmg_grow * w1, (float)BT_CURSE_DAMAGE_BOOST);
    if (dmg < 1.0f) dmg = 1.0f;
    s->emax[slot] = hp; s->ehp[slot] = hp;
    s->edmg[slot] = dmg;
    s->espd[slot] = bt_enemy_final_speed(s, (float)K->speed + s->espeed_rand[slot],
                                         (float)BT_CURSE_SPEED_BOOST);
    /* Entity.boost sets is_boosted = true and stores the args; gd:33-34 then set
     * can_be_boosted = false.  Recording the percentages as the boost in force
     * (a) keeps the DLC stargazer's get_angry (stargazer.gd:69-79) ADDING its own
     *     onto the curse boost, and
     * (b) makes the buffer's `not is_boosted` guard refuse a cursed target. */
    s->eboosted[slot] = 1;
    s->eboost_hp[slot] = (float)hp_pct;
    s->eboost_dmg[slot] = (float)BT_CURSE_DAMAGE_BOOST;
    s->eboost_spd[slot] = (float)BT_CURSE_SPEED_BOOST;
}

/* main.gd:777-780 get_gold_value: the dying unit's effect-behavior gold
 * modifiers are summed and multiply the drop value; a cursed unit contributes
 * curse_enemy_effect_behavior.gd:20-21 -> 0.33.  Returns the multiplier. */
BT_HD static float bt_enemy_curse_gold_value_mult(BT_DEVICE BtState* s, int slot) {
    return bt_enemy_cursed(s, slot) ? (1.0f + BT_CURSED_ENEMIES_BONUS_GOLD) : 1.0f;
}

/* curse_enemy_effect_behavior.gd:24-41 on_taken_damage: a cursed enemy killed
 * by a player pays gold_on_cursed_enemy_kill DIRECTLY into the run
 * (RunData.add_gold: no coin entity, no XP; run_data.gd:810-818).  The caller
 * supplies the lethal + from_player_index >= 0 test the source applies
 * (`health > 0` / `from_player_index == -1` return early).  Two callers:
 *   * bt_item_expl_hit_enemy (all weapon / projectile / explosion hits), and
 *   * the burn-tick kill in bt_core_step (unit.gd:661-707 take_damage carries
 *     Unit._burning_player_index, which in this single-player core is always
 *     player 0 -- see the note at that call site).
 * Returns the gold paid (0 = not cursed, or no producer owned). */
BT_HD static int bt_enemy_curse_player_kill_gold(BT_DEVICE BtState* s, int slot) {
    if (!bt_enemy_cursed(s, slot)) return 0;
    float g = ((int)BT_ISTR_GOLD_ON_CURSED_ENEMY_KILL < BT_NITEM_STRS)
                  ? s->scalar_effect[BT_ISTR_GOLD_ON_CURSED_ENEMY_KILL] : 0.0f;
    if (g <= 0.0f) return 0;
    int gold = (int)g;
    s->materials += (float)gold;
    /* RunData.add_tracked_value(pid, item_black_flag, gold): the black flag's
     * own tracking has no observable reader in this core, so it is not stored. */
    return gold;
}
