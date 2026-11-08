/* brotato_enemy_modifiers.h -- enemy-side stat modifiers (EnemyModifiers slice).
 *
 * OWNERSHIP
 *   G7 -- the source consumers of the three enemy-model stat keys that the item
 *         graph carries but no core code read:
 *             enemy_damage (ISTR 170), enemy_speed (ISTR 102), enemy_health (ISTR 5)
 *         consumed by entity_service.gd get_final_enemy_damage / _health /
 *         _speed through unit.gd reset_damage_stat / reset_health_stat /
 *         reset_speed_stat and enemy.gd set_attack_behavior_damage.
 *   G1 -- the boss.gd:52-58 `damage_against_bosses` receive multiplier, applied
 *         at EVERY player-sourced damage path that targets an elite/boss unit
 *         (the source wraps Unit.take_damage, so the multiplier is not specific
 *         to the Bull explosion).
 *
 *   It never re-implements the stat layer or a second temp store: every stat
 *   value is read through brotato_temporal_events.h
 *   (`bt_temporal_scalar_effective` = permanent SumData effect + this wave's
 *   stats_next_wave temp; `bt_temporal_target_damage_mult` = the boss
 *   multiplier), which the parent already integrates. enemy_damage /
 *   enemy_speed / enemy_health / damage_against_bosses are NOT core stat ids
 *   (bt_core_id_from_istr == 255), so the raw SUM lives in `scalar_effect[]`.
 *
 *   Placement: include AFTER brotato_temporal_events.h (this header includes it
 *   itself, so one include is enough) and BEFORE bt_core_step.
 *
 * ---------------------------------------------------------------------------
 * SOURCE PROVENANCE (brotato_recovered v1.1.14.6 + brotato_abyss DLC)
 * ---------------------------------------------------------------------------
 *   entity_service.gd:11-31  get_final_enemy_damage(from_value, percent):
 *       effects_factor  = max(0.01, 1 + Utils.sum_all_player_stats(enemy_damage)/100)
 *       danger_factor   = max(0.01, 1 + get_player_effect(danger_enemy_damage,0)/100)
 *       coop/accessibility/endless: single-player campaign defaults all == 1.0
 *       factor = danger * accessibility * coop * effects * endless
 *       return round(from_value * factor * max(0.01, 1 + percent/100)) as int
 *   entity_service.gd:33-53  get_final_enemy_health: identical shape with the
 *       enemy_health / danger_enemy_health keys (curse/endless == 1.0 here).
 *   entity_service.gd:55-66  get_final_enemy_speed(from_value, effects_factor,
 *       percent): factor = effects_factor * accessibility * endless; the caller
 *       (unit.gd:113-117 get_speed_effect_mods) builds effects_factor as the
 *       per-player product of (1 + Utils.get_stat(enemy_speed)/100); single
 *       player => 1 + stat/100. boost = 1 + percent/100 (NO lower clamp).
 *   unit.gd:104-111  reset_health_stat / reset_damage_stat(percent) ->
 *       get_base_health/get_base_damage(RunData.current_wave) -> get_final_*.
 *   unit.gd:113-126  reset_speed_stat: speed = round((stats.speed +
 *       _speed_rand_value) * effects_factor * accessibility * endless * boost).
 *   enemy.gd:148-151 set_attack_behavior_damage: a ShootingAttackBehavior's
 *       projectile_damage = get_final_enemy_damage(attack.damage +
 *       attack.damage_increase_each_wave*(wave-1), percent).
 *   enemy.gd:64,83,129,208 the contact/shoot hitbox damage = current_stats.damage
 *       (the value reset_damage_stat produced).
 *   boss.gd:52-58    take_damage(value, args):
 *       dmg_value = value
 *       d = RunData.get_player_effect(damage_against_bosses, from_player_index)
 *       if d > 0: dmg_value = int(value * (1 + Utils.get_stat(key)/100))
 *       return .take_damage(dmg_value, args)      # crit + bonus% + armor follow
 *
 * Bull-reachable producers (the whole items/all + DLC pool; characters/weapons
 * are NOT purchasable by the Bull):
 *   enemy_damage  black_flag_effect_2  (DLC tier 2, max_nb 1, unlocked 1) +10 SUM
 *                 starfish_effect_1    (DLC tier 2, max_nb 3, unlocked 0) +15 SUM
 *                 peacock_effect_3     (tier 2, max_nb -1, unlocked 1) +50 stats_next_wave
 *   enemy_speed   snail_effect_1       (tier 1, max_nb 1, unlocked 0)  -8 SUM
 *                 scarf_effect_3       (DLC tier 2, max_nb -1, unlocked 0) +25 stats_next_wave
 *   enemy_health  black_flag_effect_1  (DLC tier 2, max_nb 1, unlocked 1) +10 SUM
 *                 celery_tea_effect_1  (tier 1, max_nb -1, unlocked 0) +100 stats_next_wave
 *   damage_against_bosses  silver_bullet_effect_1 (tier 2, max_nb -1, unlocked 1) +25 SUM
 * (the core ignores `unlocked_by_default` -- bt_item_shop_ok -- so the tier-0
 * unlocks are reachable in-core; stated as the environment-model assumption.)
 *
 * NO-DEAD / NEUTRAL BEHAVIOUR
 *   - The three final-stat helpers are pure factors: with nothing owned every
 *     factor is exactly 1.0 (the damage/health clamp is max(0.01, 1.0) == 1.0),
 *     so every site reproduces today's numbers bit-for-bit when the keys are
 *     untouched.
 *   - bt_enemy_incoming_damage returns `value` UNCHANGED for a neutral slot and
 *     for a plain enemy (eelite 0), and unchanged when damage_against_bosses is
 *     not owned -- it never introduces the int() truncation unless the source
 *     gate fired. Only elite(1)/boss(2) slots are scaled.
 */
#pragma once

#include "brotato_temporal_events.h"
#include "brotato_endless.h"

/* ---------------------------------------------------------------------------
 * G7 effects factors (entity_service.gd + unit.gd). Utils.sum_all_player_stats
 * for a key with no core stat id == bt_temporal_scalar_effective (permanent SUM
 * + this wave's stats_next_wave temp); a single player is summed directly.
 * ------------------------------------------------------------------------- */

/* get_final_enemy_damage's effects_factor: max(0.01, 1 + enemy_damage/100). */
BT_HD static float bt_enemy_damage_effects_factor(BT_DEVICE BtState* s) {
    float f = 1.0f + bt_temporal_scalar_effective(s, BT_ISTR_ENEMY_DAMAGE) * 0.01f;
    return f > 0.01f ? f : 0.01f;
}

/* get_final_enemy_health's effects_factor: max(0.01, 1 + enemy_health/100). */
BT_HD static float bt_enemy_health_effects_factor(BT_DEVICE BtState* s) {
    float f = 1.0f + bt_temporal_scalar_effective(s, BT_ISTR_ENEMY_HEALTH) * 0.01f;
    return f > 0.01f ? f : 0.01f;
}

/* unit.gd get_speed_effect_mods: 1 + enemy_speed/100 (NO lower clamp -- a snail
 * makes enemies slower). Single player => one factor, not a product. */
BT_HD static float bt_enemy_speed_effects_factor(BT_DEVICE BtState* s) {
    return 1.0f + bt_temporal_scalar_effective(s, BT_ISTR_ENEMY_SPEED) * 0.01f;
}

/* ---------------------------------------------------------------------------
 * G7 final enemy stats -- the entity_service.gd functions the unit reset paths
 * call. `from_value` is the source's base value:
 *   damage : stats.get_base_damage(wave)     = damage + damage_grow*(wave-1)   (float)
 *   health : stats.get_base_health(wave)     = int(health + hp_grow*(wave-1))  (int!)
 *   speed  : stats.speed + _speed_rand_value (both int)
 * `percent_modifier` is the source's int percent (buffer hp/damage/speed boost;
 * 0 at spawn). danger_hp/danger_dmg already hold the evaluated danger_factor
 * (D5: 1.40); accessibility / coop / endless are the single-player campaign
 * defaults (1.0), so factor == danger * effects exactly as the source evaluates
 * it (x1.0 is exact in fp32).
 * ------------------------------------------------------------------------- */
BT_HD static float bt_enemy_final_damage(BT_DEVICE BtState* s, float from_value, float percent_modifier) {
    /* entity_service.gd:24 endless term = max(0.01, 1 + E*(1+curse)); single
     * player, no curse.  1.0 exactly for waves <= 20. */
    float factor = s->danger_dmg * bt_enemy_damage_effects_factor(s) *
                   bt_endless_enemy_damage_mult(s->wave);
    float boost = 1.0f + percent_modifier * 0.01f;
    if (boost < 0.01f) boost = 0.01f;                  /* get_final_enemy_damage */
    return bt_roundf(from_value * factor * boost);
}

BT_HD static float bt_enemy_final_health(BT_DEVICE BtState* s, float from_value, float percent_modifier) {
    /* entity_service.gd:46 endless term = max(0.01, 1 + E*2.25*(1+curse)). */
    float factor = s->danger_hp * bt_enemy_health_effects_factor(s) *
                   bt_endless_enemy_health_mult(s->wave);
    float boost = 1.0f + percent_modifier * 0.01f;
    if (boost < 0.01f) boost = 0.01f;                  /* get_final_enemy_health */
    return bt_roundf(from_value * factor * boost);
}

BT_HD static float bt_enemy_final_speed(BT_DEVICE BtState* s, float from_value, float percent_modifier) {
    /* entity_service.gd:60 endless term = 1 + min(1.75, E/13.33). */
    float factor = bt_enemy_speed_effects_factor(s) *
                   bt_endless_enemy_speed_mult(s->wave);   /* * accessibility */
    float boost = 1.0f + percent_modifier * 0.01f;     /* get_final_enemy_speed: no clamp */
    return bt_roundf(from_value * factor * boost);
}

/* ---------------------------------------------------------------------------
 * G1 -- boss.gd:52-58 damage_against_bosses receive multiplier.
 *
 *   value_after = value                                       # effect not owned / not a boss
 *   value_after = int(value * (1 + damage_against_bosses/100)) # d > 0 and elite/boss
 *
 * `slot` is the TARGET unit. eelite[] holds the kind class the core computed
 * (bt_kind_class: 0 enemy/neutral, 1 elite, 2 boss), i.e. exactly the units the
 * source builds from boss.tscn (elites instance it, is_elite default true).
 * The multiplier is applied to the PRE-crit, PRE-armor hit value: the source
 * mutates `value` before delegating to Unit.take_damage, which then rolls crit
 * and applies the bonus-% / armor coefficient. Callers must therefore scale the
 * incoming value BEFORE their own crit/bonus/armor steps.
 * ------------------------------------------------------------------------- */
BT_HD static float bt_enemy_incoming_damage(BT_DEVICE BtState* s, int slot, float value) {
    if (slot < 0 || slot >= BT_MAX) return value;
    if (s->eelite[slot] == 0) return value;            /* enemy(0) / neutral(0) */
    float mult = bt_temporal_target_damage_mult(s, BT_ISTR_DAMAGE_AGAINST_BOSSES);
    if (mult == 1.0f) return value;                    /* effect not owned: value unchanged */
    return (float)(int)(value * mult);                 /* boss.gd:57 int(value * mult) */
}

/* ===========================================================================
 * PARENT REQUIREMENT -- exact integration (no core/state edits are made by this
 * slice; the owner of brotato_core.h / brotato_progression.h applies the
 * following). It assumes the brotato_temporal_events.h state additions
 * (`temp_scalar[]`, the two bonus flags) are already in.
 *
 * 1. brotato_core.h, after `#include "brotato_temporal_events.h"`:
 *        #include "brotato_enemy_modifiers.h"
 *
 * 2. brotato_core.h, in the forward-declaration block (beside bt_scalar_on_dodge):
 *        BT_HD static float bt_enemy_final_damage(BT_DEVICE BtState* s, float from_value, float percent_modifier);
 *        BT_HD static float bt_enemy_final_health(BT_DEVICE BtState* s, float from_value, float percent_modifier);
 *        BT_HD static float bt_enemy_final_speed(BT_DEVICE BtState* s, float from_value, float percent_modifier);
 *        BT_HD static float bt_enemy_incoming_damage(BT_DEVICE BtState* s, int slot, float value);
 *
 * 3. SPAWN -- brotato_core.h bt_spawn_kind_at, replace the three stat lines
 *    (keep the boss/elite hp factors and the hp<1 / dmg<1 floors that follow):
 *
 *        float w1 = (float)(s->wave - 1);
 *        float hp  = bt_enemy_final_health(s, K->hp + K->hp_grow * w1, 0.0f);
 *        float dmg = bt_enemy_final_damage(s, K->dmg + K->dmg_grow * w1, 0.0f);
 *        ...
 *        s->espd[slot] = bt_enemy_final_speed(s, (float)K->speed + jitter, 0.0f);
 *
 *    (`bt_spawn_kind_at` currently computes
 *       hp  = round((K->hp + K->hp_grow*w1) * danger_hp);
 *       dmg = round((K->dmg + K->dmg_grow*w1) * danger_dmg);
 *       espd= round(K->speed + jitter);
 *     -- with nothing owned the helpers return round(base*danger_*) and
 *     round(base*1.0) respectively, so this is a pure factor insertion.)
 *    NOTE (health base type): the source's get_base_health is typed `-> int`
 *    and truncates health + hp_grow*(wave-1) before get_final_enemy_health.
 *    Today's core does not truncate; if BT_KINDS hp_grow is fractional the
 *    parent may pass bt_floorf(K->hp + K->hp_grow*w1) to match. get_base_damage
 *    is `-> float` and is NOT truncated.
 *
 * 4. BUFFER BOOST -- brotato_core.h bt_buffer_boost, replace the hp/dmg/speed
 *    recompute (the source boost() calls reset_health/damage/speed_stat(percent),
 *    i.e. the SAME get_final_* path, recomputed from the kind BASE -- not a
 *    multiply of the already-final stat):
 *
 *        s->emax[pick] = bt_enemy_final_health(s, K->hp + K->hp_grow * w1, (float)B->hp_pct);
 *        s->ehp[pick]  = s->emax[pick];
 *        s->edmg[pick] = bt_enemy_final_damage(s, K->dmg + K->dmg_grow * w1, (float)B->dmg_pct);
 *        s->espd[pick] = bt_enemy_final_speed(s, (float)BT_KINDS[s->ekind[pick]].speed
 *                                                + <the slot's stored _speed_rand_value>,
 *                                             (float)B->speed_pct);
 *    (the current core multiplies `s->espd[pick] * ratio_sp`, which compounds a
 *     previous boost; the source recomputes from stats.speed + the unit's fixed
 *     _speed_rand_value. If the core does not retain that jitter per slot, the
 *     parent must add it or accept the existing approximation -- flagged, not
 *     guessed.)
 *
 * 5. ENEMY ATTACK DAMAGE -- brotato_core.h bt_fire_shot, replace
 *        float dmg = bt_roundf((A.damage + A.damage_grow * (float)(s->wave - 1)) * s->danger_dmg);
 *    with
 *        float dmg = bt_enemy_final_damage(s, A.damage + A.damage_grow * (float)(s->wave - 1), 0.0f);
 *    (enemy.gd:148-151 set_attack_behavior_damage routes a ShootingAttackBehavior
 *     projectile through the same get_final_enemy_damage.)
 *
 * 6. G1 -- apply bt_enemy_incoming_damage(s, target_slot, value) to the PRE-crit
 *    value at every player-sourced damage path that can target an elite/boss
 *    (boss.gd take_damage wraps all of them):
 *
 *    (a) bt_explode per-target loop: the core rolls crit once for the whole
 *        explosion (`dmg = crit ? round(base*crit_dmg) : base`). The source
 *        applies the multiplier to the PRE-crit value, so compute it per target:
 *            float b  = bt_enemy_incoming_damage(s, j, base);
 *            float dj = crit ? bt_roundf(b * s->crit_dmg) : b;
 *            float taken = bt_roundf(dj * (1.0f + pct * 0.01f) * bt_armor_mult(armor));
 *        (RNG order is unchanged -- the crit draw stays outside the loop.)
 *        The same loop's proc hit becomes
 *            s->ehp[j] -= bt_enemy_incoming_damage(s, j, s->proc_dmg);
 *
 *    (b) friendly pet/turret projectile vs enemy (~line 3399, `s->ehp[j] -= d;`):
 *            float v = bt_enemy_incoming_damage(s, j, s->prdmg[i]);
 *            if (<per-target crit roll>) v = bt_roundf(v * s->prcrit_dmg[i]);
 *            float d = bt_roundf(v * bt_armor_mult(armor));
 *        (unit.gd:285-300: the crit is rolled per target, before armor.)
 *
 *    (c) burn tick (~line 3609, `s->ehp[i] -= s->eburndmg[i];`): the source burn
 *        tick calls take_damage with armor_applied=false (unit.gd:661-672), so:
 *            s->ehp[i] -= bt_enemy_incoming_damage(s, i, s->eburndmg[i]);
 *
 *    (d) brotato_scalar_events.h bt_scalar_chance_damages (~line 382): the
 *        source handle_stat_damages calls other_enemy.take_damage(total), and
 *        the core computes `taken = bt_roundf(total * bt_armor_mult(armor))`.
 *        Scale the PRE-armor total, then the armor step:
 *            float pre = bt_enemy_incoming_damage(s, target, total);
 *            float taken = bt_roundf(pre * bt_armor_mult(armor));
 *
 *    (e) brotato_scalar_events.h bt_scalar_on_dodge dmg_on_dodge (~line 422):
 *        the source hitbox.from.take_damage => the same insert on the
 *        pre-armor value:
 *            float pre = bt_enemy_incoming_damage(s, enemy_slot, total_dmg);
 *            float taken = bt_roundf(pre * bt_armor_mult(armor));
 *
 *    Any new `s->ehp[slot] -= dmg` path the parent adds later must route its
 *    pre-armor value through bt_enemy_incoming_damage the same way.
 *
 *    NOT in scope for G1 (no producer / not a player hit): neutral/tree damage
 *    (bt_neutral_resolve_hit), and enemy-vs-enemy damage (not modelled).
 * ======================================================================== */
