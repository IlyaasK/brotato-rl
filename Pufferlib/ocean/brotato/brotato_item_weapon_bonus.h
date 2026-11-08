/* Item-related weapon bonuses -- owner: WeaponStats.
 *
 * Implements run_data.gd:1085-1161 update_item_related_effects: four buckets of
 * declared [stat_hash, value] pairs, each multiplied by a weapon-count and
 * applied to the PLAYER's stat effects (permanent layer).  The buckets are
 * filled by generic Effect.apply() (items/global/effect.gd:65-72, StorageMethod
 * KEY_VALUE): `effects[custom_key_hash].push_back([key_hash, value])`, once per
 * owned item copy (bt_apply_item) and once per owned weapon copy
 * (run_data.gd:914 add_weapon -> apply_item_effects on the WeaponData).
 *   unique_weapon_effects        x #distinct owned weapon families
 *                                (run_data.gd:1093-1112, get_unique_weapon_ids)
 *   additional_weapon_effects    x #owned weapons       (:1114-1128)
 *   tier_iv_weapon_effects       x #owned tier>=LEGENDARY weapons (:1130-1147)
 *   tier_i_weapon_effects        x #owned tier<=COMMON weapons    (:1149-1166)
 *
 * Every runner is a pure recompute: unapply what was applied last time (the
 * stored per-bucket multiplier), then re-derive from the current inventory and
 * reapply.  Like update_sets (brotato_weapon_set_runtime.h), the state is NEVER
 * cached as membership: it is recomputed on every item acquisition and every
 * inventory mutation, so equipping / unequipping / combining / upgrading a
 * weapon changes it exactly, and LinkedStats.reset_player then folds the
 * resulting permanent layer into the derived stats.
 *
 * SHIPPED PRODUCERS in this pool (247 campaign+DLC items, 253 weapon variants):
 *   unique  : items/all/focus (focus_effect_2, stat_attack_speed -3),
 *             items/all/spider (spider_effect_2, stat_attack_speed +6),
 *             items/all/jelly (jelly_effect_0, stat_max_hp +1)
 *   additional: weapons/melee/excalibur (excalibur_effect_1, stat_armor -3)
 *   tier_i / tier_iv: NO shipped producer -- only the King character keys
 *     tier_i|iv_weapon_effects, and extract_items.py does not intern those
 *     custom_key names (no BT_ISTR_TIER_*_WEAPON_EFFECTS exists), so no
 *     BT_ITEM_EFF_ROWS row can carry them.  The count/apply logic below is the
 *     source's and is exercised directly; wiring a future producer is then a
 *     one-line case in bt_iwb_bucket_of_custom_key.
 *
 * WHERE TO INCLUDE (parent): brotato_core.h, AFTER bt_core_stat_add /
 * bt_core_id_from_istr / bt_curse_stat, AFTER brotato_weapon_inventory.h (for
 * bt_weapon_unique_families) and BEFORE bt_apply_item.  brotato_weapon_inventory.h
 * forward-declares bt_iwb_register_weapon / bt_iwb_unregister_weapon /
 * bt_item_wbonus_refresh for its mutations, exactly like bt_weapon_set_refresh.
 */
#pragma once

#include "brotato_state.h"
#include "brotato_item_effects.h"
#include "brotato_weapons.h"
#include "brotato_weapon_inventory.h"
#ifndef BT_WEAPON_FX_CURSE_MAX
#define BT_WEAPON_FX_CURSE_MAX 2
#endif

/* Bucket ordinals (iwb_bucket[] and iwb_count[]). */
#define BT_IWB_UNIQUE 0
#define BT_IWB_ADDITIONAL 1
#define BT_IWB_TIER_I 2
#define BT_IWB_TIER_IV 3
#define BT_IWB_KINDS 4
#define BT_IWB_MAX 32   /* declared pairs across every bucket; overflow is explicit */

/* Effect.custom_key -> bucket ordinal, or -1 when the key names no bucket.
 * Only the two interned shipped keys have a case; the tier_i/iv keys are not
 * interned by extract_items.py (see the header note). */
BT_HD static int bt_iwb_bucket_of_custom_key(unsigned short k) {
#if defined(BT_ISTR_UNIQUE_WEAPON_EFFECTS)
    if (k == (unsigned short)BT_ISTR_UNIQUE_WEAPON_EFFECTS) return BT_IWB_UNIQUE;
#endif
#if defined(BT_ISTR_ADDITIONAL_WEAPON_EFFECTS)
    if (k == (unsigned short)BT_ISTR_ADDITIONAL_WEAPON_EFFECTS) return BT_IWB_ADDITIONAL;
#endif
    return -1;
}

/* Register one declared pair (per acquired copy). */
BT_HD static void bt_iwb_add(BT_DEVICE BtState* s, int bucket, unsigned short key, int value) {
    if (bucket < 0 || bucket >= BT_IWB_KINDS) return;
    if ((int)s->n_iwb >= BT_IWB_MAX) { s->stat_reg_overflow = 1; return; }
    int i = (int)s->n_iwb++;
    s->iwb_bucket[i] = (unsigned char)bucket;
    s->iwb_key[i] = key;
    s->iwb_val[i] = value;
}

/* Remove the first matching declared pair (weapon removal). */
BT_HD static void bt_iwb_remove(BT_DEVICE BtState* s, int bucket, unsigned short key, int value) {
    for (int i = 0; i < (int)s->n_iwb; i++) {
        if ((int)s->iwb_bucket[i] != bucket || s->iwb_key[i] != key || s->iwb_val[i] != value) continue;
        int last = (int)s->n_iwb - 1;
        s->iwb_bucket[i] = s->iwb_bucket[last];
        s->iwb_key[i] = s->iwb_key[last];
        s->iwb_val[i] = s->iwb_val[last];
        s->n_iwb = (unsigned char)last;
        return;
    }
}

/* The weapon-count multiplier of one bucket (run_data.gd:1093-1166). */
BT_HD static int bt_iwb_count(BT_DEVICE BtState* s, int bucket) {
    int n = (int)s->wpn_count;
    if (n > BT_SLOTS) n = BT_SLOTS;
    switch (bucket) {
    case BT_IWB_UNIQUE:
        return bt_weapon_unique_families(s);
    case BT_IWB_ADDITIONAL:
        return n;
    case BT_IWB_TIER_I: {
        int c = 0;
        for (int q = 0; q < n; q++) if ((int)s->wpn_tier[q] <= 0) c++;
        return c;
    }
    case BT_IWB_TIER_IV: {
        int c = 0;
        for (int q = 0; q < n; q++) if ((int)s->wpn_tier[q] >= 3) c++;
        return c;
    }
    }
    return 0;
}

/* effects[stat] += delta.  A core stat key feeds the raw/permanent layer; a
 * non-core key rides the SUM scalar bucket exactly like effect.gd's
 * `effects[key_hash] += value` (the fired target key's own storage_method is
 * irrelevant -- the bucket runner always SUMs onto it). */
BT_HD static void bt_iwb_apply(BT_DEVICE BtState* s, unsigned short key, long long delta) {
    if (delta == 0) return;
    int id = bt_core_id_from_istr(key);
    if (id != 255) { bt_core_stat_add(s, id, (float)delta); return; }
    if (key == BT_ISTR_NONE) return;
    s->scalar_effect[key] += (float)delta;
}

/* update_item_related_effects: unapply exactly what was applied last time (the
 * recorded contributions), then re-derive every bucket from the current
 * inventory and reapply.  The applied list -- not a per-bucket multiplier --
 * is what makes a NEW copy's pair safe: the source stores and erases the exact
 * pushed list too (run_data.gd:1096-1111), so a pair that appeared since the
 * last refresh is never subtracted before it was ever added. */
BT_HD static void bt_item_wbonus_refresh(BT_DEVICE BtState* s) {
    for (int i = 0; i < (int)s->n_iwb_app; i++)
        bt_iwb_apply(s, s->iwb_app_key[i], -(long long)s->iwb_app_delta[i]);
    s->n_iwb_app = 0;

    for (int b = 0; b < BT_IWB_KINDS; b++) {
        int c = bt_iwb_count(s, b);
        s->iwb_count[b] = c;
        if (c == 0) continue;
        for (int i = 0; i < (int)s->n_iwb; i++) {
            if ((int)s->iwb_bucket[i] != b) continue;
            long long d = (long long)s->iwb_val[i] * (long long)c;
            if (d == 0) continue;
            bt_iwb_apply(s, s->iwb_key[i], d);
            if ((int)s->n_iwb_app < BT_IWB_MAX) {
                s->iwb_app_key[s->n_iwb_app] = s->iwb_key[i];
                s->iwb_app_delta[s->n_iwb_app] = (int)d;
                s->n_iwb_app++;
            } else {
                s->stat_reg_overflow = 1;
            }
        }
    }
}

/* Register a weapon's own bucket effects at acquisition (run_data.gd:914
 * add_weapon -> apply_item_effects(new_weapon)).  `fxcurse` is the weapon
 * slot's per-top-level-effect curse modifier array (NULL when not cursed); each
 * effect's value is curse-boosted exactly as the weapon-stats rail does
 * (bt_curse_stat, dlc_1_data.gd generic branch). */
BT_HD static void bt_iwb_register_weapon(BT_DEVICE BtState* s, unsigned short id,
                                         const float* fxcurse) {
    if (!bt_weapon_valid(id)) return;
    BT_CONST_PTR(BtWeaponDef) D = &BT_WEAPONS[id];
    for (int k = 0; k < (int)D->fx_count; k++) {
        int idx = (int)D->fx_first + k;
        if (idx >= BT_NWEAPON_EFF_INDEX) break;
        int row = (int)BT_WEAPON_EFF_INDEX[idx];
        if (row < 0 || row >= BT_NITEM_EFF_ROWS) continue;
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[row];
        if (E->opcode != BT_ITEMEFF_EFFECT || E->storage != 1) continue;
        int b = bt_iwb_bucket_of_custom_key(E->custom_key);
        if (b < 0) continue;
        float em = (fxcurse && k < BT_WEAPON_FX_CURSE_MAX) ? fxcurse[k] : 0.0f;
        bt_iwb_add(s, b, E->key, bt_curse_stat((float)E->value, em));
    }
}

/* run_data.gd:1000-1009 after_weapon_removed -> unapply_item_effects. */
BT_HD static void bt_iwb_unregister_weapon(BT_DEVICE BtState* s, unsigned short id,
                                           const float* fxcurse) {
    if (!bt_weapon_valid(id)) return;
    BT_CONST_PTR(BtWeaponDef) D = &BT_WEAPONS[id];
    for (int k = 0; k < (int)D->fx_count; k++) {
        int idx = (int)D->fx_first + k;
        if (idx >= BT_NWEAPON_EFF_INDEX) break;
        int row = (int)BT_WEAPON_EFF_INDEX[idx];
        if (row < 0 || row >= BT_NITEM_EFF_ROWS) continue;
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[row];
        if (E->opcode != BT_ITEMEFF_EFFECT || E->storage != 1) continue;
        int b = bt_iwb_bucket_of_custom_key(E->custom_key);
        if (b < 0) continue;
        float em = (fxcurse && k < BT_WEAPON_FX_CURSE_MAX) ? fxcurse[k] : 0.0f;
        bt_iwb_remove(s, b, E->key, bt_curse_stat((float)E->value, em));
    }
}

/* The weapon half of `apply_item_effects` that lands in the PLAYER's stat layer
 * (run_data.gd:905-915 add_weapon -> :1183-1193 apply_item_effects, and
 * :1000-1009 after_weapon_removed -> :1203-1207 unapply_item_effects).
 *
 * A weapon's own plain `Effect` rows with SUM storage and a stat key are the
 * per-weapon stat bonuses the shop tooltip shows (weapons/** `key = "stat_*"`,
 * e.g. jousting_lance_effect_1 stat_speed +2, hand_effect_1 stat_harvesting +3,
 * rock_3_effect_1 stat_max_hp +2).  The (stat, value, sign) triple is the
 * canonical BT_ITEM_EFF_ROWS row the copy's BT_WEAPONS[id].fx_first/fx_count
 * slice names -- the ONE effect namespace, so no second per-weapon table exists
 * and none is added here.
 *
 * effect.gd:59-72 apply(): `effects[key_hash] += value` (StorageMethod.SUM);
 * unapply(): `effects[key_hash] -= value`; both end in Utils.reset_stat_cache ->
 * bt_core_stat_sync.  The stat LAYER therefore moves at once, while the player's
 * LiveStats snapshot (p_speed_pct, p_armor, ...) follows the ordinary
 * bt_player_stat_sync_step pipeline and is re-based at every wave's scene build
 * (bt_player_stats_reset == player.gd:349-350 update_player_stats(true)); both
 * callers below only mutate the layer, so the timing is the source's.
 *
 * `dir` = +1 at acquisition (bt_weapon_insert: bought / starting weapon / the
 * variant a combine or upgrade produces) and -1 at removal (bt_weapon_erase:
 * sell, recycle, a consumed combine source, bt_weapon_remove_all).  `fxcurse` is
 * the copy's per-top-level-effect curse modifier array (NULL for an uncursed
 * copy); the value is curse-boosted with the item rail's exact call
 * bt_curse_value(value, sign, mod, 0, BT_SIGN_POSITIVE) -- the identity at
 * mod == 0, and it honours a NEGATIVE effect_sign row (magnitude shrinks)
 * unlike the positive-only bt_curse_stat.
 *
 * The KEY_VALUE rows of the same weapon files stay on their own rails, exactly
 * as the item rail routes them: additional_weapon_effects (excalibur_effect_1)
 * by bt_iwb_register_weapon above, temp_stats_while_not_moving (jousting_lance
 * effect_2) / temp_stats_on_hit (scythe_effect_2) / temp_stats_per_interval
 * (drill_effect_2) by the temporal buckets, and GainStatForEveryStatEffect
 * (sharp_tooth_effect_0) by the link registry.  This function touches storage
 * SUM only, so nothing is applied twice. */
BT_HD static void bt_weapon_statfx_apply(BT_DEVICE BtState* s, unsigned short id,
                                         const float* fxcurse, int dir) {
    if (!bt_weapon_valid(id)) return;
    BT_CONST_PTR(BtWeaponDef) D = &BT_WEAPONS[id];
    for (int k = 0; k < (int)D->fx_count; k++) {
        int idx = (int)D->fx_first + k;
        if (idx >= BT_NWEAPON_EFF_INDEX) break;
        int row = (int)BT_WEAPON_EFF_INDEX[idx];
        if (row < 0 || row >= BT_NITEM_EFF_ROWS) continue;
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[row];
        if (E->opcode != BT_ITEMEFF_EFFECT || E->storage != 0) continue;
        int sid = bt_core_id_from_istr(E->key);
        if (sid == 255) continue;                /* no core stat id: not this rail */
        float em = (fxcurse && k < BT_WEAPON_FX_CURSE_MAX) ? fxcurse[k] : 0.0f;
        float nv = (float)bt_curse_value(E->value, E->sign, em, 0, BT_SIGN_POSITIVE);
        bt_core_stat_add(s, sid, dir >= 0 ? nv : -nv);
    }
}
