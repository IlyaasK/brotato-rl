/* Weapon class set runtime -- owner: WeaponStats.
 *
 * Implements run_data.gd:1043-1072 update_sets: with n >= 2 weapons sharing a
 * set, the set's tier `min(n-2, nb_tiers-1)` bonuses are applied to the PLAYER
 * (plain Effect resources -- mostly SUM core stats; explosion_size /
 * explosion_damage are interned scalar buckets). The bonus set is fully
 * recomputed on every inventory mutation, so equipment is reversible: the
 * previously applied tier is unapplied first, then the current state is
 * re-derived from wpn_id[]/BT_WEAPONS[].sets (the weapon metadata is the single
 * source of truth; no cached membership).
 *
 * Source:
 *   run_data.gd:1043-1046  unapply every active_set_effects entry
 *   run_data.gd:1054-1062  count weapons per set (all_weapons_count_for_sets -> weapons.size())
 *   run_data.gd:1064-1072  tier = min(count-2, nb_tiers-1); apply set_bonuses[tier]
 *
 * REQUIRED BtState FIELD (parent adds):
 *   unsigned char wpn_set_tier[BT_NWEAPON_SETS];  (applied tier, 0xFF = none)
 * REQUIRED CALLSITE (parent):
 *   bt_weapon_set_refresh(s) after bt_core_reset's loadout init and after every
 *   inventory mutation (add / remove / combine / upgrade / start-apply).
 *
 * Include by brotato_core.h AFTER bt_core_id_from_istr (~1620) and
 * bt_core_stat_add (~1731); before any caller (step / reset).
 */
#pragma once

#include "brotato_state.h"
#include "brotato_weapon_types.h"
#include "brotato_weapons.h"

/* One set-bonus effect row applied (dir=1) or unapplied (dir=0) to the player,
 * mirroring item effect.gd apply/unapply for the key shapes the shipped set
 * bonuses use: a core stat key -> bt_core_stat_add, any other interned key ->
 * the SUM scalar bucket (effect.storage == 2 overwrites; no shipped set bonus
 * uses it). Every shipped set bonus is storage_method 0 (SUM), so the unapply is
 * an exact inverse. */
BT_HD static void bt_weapon_set_effect_apply(BT_DEVICE BtState* s, unsigned short row, int dir) {
    BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[row];
    int id = bt_core_id_from_istr(E->key);
    if (id != 255) { bt_core_stat_add(s, id, (float)(dir ? E->value : -E->value)); return; }
    if (E->key == BT_ISTR_NONE) return;
    if (E->storage == 2) s->scalar_effect[E->key] = dir ? (float)E->value : 0.0f;
    else s->scalar_effect[E->key] += (float)(dir ? E->value : -E->value);
}

/* Recompute every weapon class set bonus. Idempotent; safe to call at reset and
 * after any slot mutation. Never returns early: an unapplied/reapplied set still
 * leaves the player stats consistent because SUM stats are exact inverses. */
BT_HD static void bt_weapon_set_refresh(BT_DEVICE BtState* s) {
    /* 1. unapply everything currently applied */
    for (int i = 0; i < BT_NWEAPON_SETS; i++) {
        int t = s->wpn_set_tier[i];
        if (t == 0xFF) continue;
        BT_CONST_PTR(BtWeaponSet) set = &BT_WEAPON_SETS[i];
        if (t < (int)set->nb_tiers) {
            BtWeaponSetTier st = BT_WEAPON_SET_TIERS[set->tier_first + t];
            for (int k = 0; k < (int)st.count; k++) {
                int e = (int)st.first + k;
                if (e >= BT_NWEAPON_SET_EFFECTS) break;
                bt_weapon_set_effect_apply(s, BT_WEAPON_SET_EFFECTS[e], 0);
            }
        }
        s->wpn_set_tier[i] = 0xFF;
    }

    /* 2. count weapons per set over the dense live slots */
    int n = (int)s->wpn_count;
    if (n > BT_SLOTS) n = BT_SLOTS;
    int total = 0;
    for (int q = 0; q < n; q++)
        if (s->wpn_id[q] != BT_WEAPON_NONE) total++;

    unsigned char cnt[BT_NWEAPON_SETS];
    for (int i = 0; i < BT_NWEAPON_SETS; i++) cnt[i] = 0;

    int all_count = 0;
    if ((int)BT_ISTR_ALL_WEAPONS_COUNT_FOR_SETS < BT_NITEM_STRS)
        all_count = (s->scalar_effect[BT_ISTR_ALL_WEAPONS_COUNT_FOR_SETS] > 0.0f);

    for (int q = 0; q < n; q++) {
        unsigned short wid = s->wpn_id[q];
        if (wid == BT_WEAPON_NONE || (int)wid >= BT_NWEAPONS) continue;
        BT_CONST_PTR(BtWeaponDef) W = &BT_WEAPONS[wid];
        for (int k = 0; k < (int)W->set_count; k++) {
            int sidx = (int)BT_WEAPON_SET_IDS[W->set_first + k];
            if (sidx < 0 || sidx >= BT_NWEAPON_SETS) continue;
            if (all_count) { if (total < 255) cnt[sidx] = (unsigned char)total; }
            else if (cnt[sidx] < 255) cnt[sidx]++;
        }
    }

    /* 3. apply the tier for every set with >= 2 weapons */
    for (int i = 0; i < BT_NWEAPON_SETS; i++) {
        int c = (int)cnt[i];
        if (c < 2) continue;
        BT_CONST_PTR(BtWeaponSet) set = &BT_WEAPON_SETS[i];
        int nb = (int)set->nb_tiers;
        if (nb <= 0) continue;
        int tier = c - 2;
        if (tier > nb - 1) tier = nb - 1;
        BtWeaponSetTier st = BT_WEAPON_SET_TIERS[set->tier_first + tier];
        for (int k = 0; k < (int)st.count; k++) {
            int e = (int)st.first + k;
            if (e >= BT_NWEAPON_SET_EFFECTS) break;
            bt_weapon_set_effect_apply(s, BT_WEAPON_SET_EFFECTS[e], 1);
        }
        s->wpn_set_tier[i] = (unsigned char)tier;
    }
}

/* Applied tier of set `i` (0xFF = none) -- for observations/tests. */
BT_HD static int bt_weapon_set_tier(BT_CONST_PTR(BtState) s, int i) {
    if (i < 0 || i >= BT_NWEAPON_SETS) return 0xFF;
    return (int)s->wpn_set_tier[i];
}
