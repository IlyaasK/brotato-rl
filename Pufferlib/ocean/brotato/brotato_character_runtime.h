/* brotato_character_runtime.h -- selectable character profile runtime.
 *
 * Owner: WeaponCharacters.  Implements the recovered Bull/Knight/Demon profile
 * behaviour on top of the portable BtState core:
 *   items/characters/bull/bull_effect_1..5.tres      base stats, regen gain,
 *                                                    explode-on-hit, weapon_slot
 *   items/characters/knight/knight_effect_1..5,7     melee-per-armor link,
 *                                                    armor, no_ranged_weapons,
 *                                                    min_weapon_tier, gain cuts
 *   items/characters/demon/demon_effect_1..2         hp_shop, materials->max_hp
 *   effects/items/stat_gains_modification_effect.gd  gain_<stat> += value
 *   effects/items/gain_stat_for_every_stat_effect.gd LinkedStats link
 *   effects/items/convert_stat_effect.gd             Utils.convert_stats
 *   utils.gd:559-588                                 end-of-wave conversion
 *   singletons/run_data.gd:1428-1439                 hp_shop currency
 *   singletons/linked_stats.gd:12-78                 link recompute
 *   singletons/player_run_data.gd:434,488,489        defaults 6 slots / tier 0..99
 *
 * LIKE THE OTHER CORE SLICES (brotato_scalar_events.h, brotato_loot.h, ...)
 * this header is included by brotato_core.h AFTER the core stat helpers
 * (bt_core_stat_add / bt_core_stat_sync / bt_core_read) exist; it only adds
 * BT_HD static functions over BtState.  POD, no allocation, host- and
 * device-safe.
 *
 * Integration contract (parent wires these):
 *   - bt_core_init stores the numeric profile in s->character (BT_CHARACTERS
 *     index) and the resolved loadout in s->starting_weapon.
 *   - bt_core_reset, after zeroing the stat layers and scalar_effect[], calls
 *     bt_character_apply(s) to seed the profile.
 *   - bt_recompute_links, after the item link loop, calls
 *     bt_character_query_links(s) before its bt_core_stat_sync(s).
 *   - bt_progress_end_wave calls bt_character_convert_end_of_wave(s) (Demon).
 *   - Shop/Inventory read the restriction accessors below and call
 *     bt_character_currency / bt_character_currency_remove instead of touching
 *     s->materials directly.  There is no currency ADD: the source has no
 *     add_currency (gold pickups/harvesting/recycling always credit `materials`
 *     via RunData.add_gold, even under hp_shop; only the end-of-wave conversion
 *     raises stat_max_hp).
 */
#pragma once

#include "brotato_characters.h"
#include "brotato_items.h"
#include "brotato_item_effects.h"

/* WeaponData owns BT_WEAPON_NONE (0xFFFF, "no weapon"); keep this header
 * usable standalone.  Weapon id 0 is a VALID id. */
#if !defined(BT_WEAPON_NONE)
#define BT_CHARACTER_NO_WEAPON ((unsigned short)0xFFFFu)
#else
#define BT_CHARACTER_NO_WEAPON ((unsigned short)BT_WEAPON_NONE)
#endif

/* Core stat-layer helpers (defined in brotato_core.h).  Declared here so this
 * header may be included directly after brotato_state.h, before bt_core_reset
 * is defined (the core's own forward-declaration pattern). */
BT_HD static float bt_core_stat_read(BT_DEVICE BtState* s, int id);
BT_HD static void bt_core_stat_add(BT_DEVICE BtState* s, int id, float v);
BT_HD static void bt_core_stat_sync(BT_DEVICE BtState* s);

/* brotato_core.h defines the portable bt_floorf/bt_roundf (libm on host,
 * precise:: on Metal) before its include of this header; keep the header
 * compilable on its own otherwise. */
#if !defined(bt_floorf)
#include <math.h>
#define bt_floorf(x) floorf(x)
#endif
#if !defined(bt_roundf)
#include <math.h>
#define bt_roundf(x) roundf(x)
#endif

/* Defensive clamp: an out-of-range id behaves as the default Bull so no code
 * path can index past BT_CHARACTERS.  Host callers validate the config token
 * through bt_character_id_from_name first. */
BT_HD static unsigned short bt_character_index(unsigned short character) {
    return character < (unsigned short)BT_NCHARACTERS ? character
                                                      : (unsigned short)BT_CHARACTER_BULL;
}

BT_HD static unsigned char bt_character_hp_shop(unsigned short character) {
    return BT_CHARACTERS[bt_character_index(character)].hp_shop;
}

/* run_data.gd:767-768 get_next_level_xp_needed:
 *   need = get_xp_needed(level + 1) * (1.0 + next_level_xp_needed/100.0)
 * `next_level_xp_needed` is a plain summed int effect (storage SUM) whose only
 * producers are character effects -- character_baby +130, character_mutant -66,
 * character_technomage +75 -- so it rides a BtCharacterDef field instead of an
 * interned BT_ISTR_ key (no item or weapon uses it; see brotato_scalar_events.h
 * "(5) OUT OF TARGET").  Every profile selectable today (Bull/Knight/Demon) has
 * no such effect, so the accessor returns 0 and the multiplier is exactly 1. */
BT_HD static short bt_character_next_level_xp_pct(unsigned short character) {
    return BT_CHARACTERS[bt_character_index(character)].next_level_xp_pct;
}

/* stat_gains_modification_effect.gd / effect.gd SUM on a stat key:
 * profile stat adds land in the permanent stat_raw layer and gain deltas in
 * the gain multiplier the core derives (stat_gain = 1 + gain/100). */
BT_HD static void bt_character_apply(BT_DEVICE BtState* s) {
    unsigned short c = bt_character_index(s->character);
    BT_CONST_PTR(BtCharacterDef) d = &BT_CHARACTERS[c];
    for (int i = 0; i < (int)d->stat_count; i++) {
        BT_CONST_PTR(BtCharacterStat) row = &BT_CHARACTER_STATS[d->stat_first + i];
        if (row->stat >= BT_NSTATS) continue;
        if (row->kind == 0) s->stat_raw[row->stat] += row->value;
        else s->stat_gain[row->stat] += row->value * 0.01f;
    }
    s->weapon_slot_limit = d->weapon_slots;
    /* If the item interner exposes the character scalar keys, mirror the
     * profile into the same scalar_effect[] bucket item effects use, so a
     * consumer may read either path.  Absent keys leave the accessors below as
     * the only reader. */
#if defined(BT_ISTR_WEAPON_SLOT)
    if ((unsigned short)BT_ISTR_WEAPON_SLOT < BT_NITEM_STRS)
        s->scalar_effect[BT_ISTR_WEAPON_SLOT] = (float)d->weapon_slots;
#endif
#if defined(BT_ISTR_NO_MELEE_WEAPONS)
    if ((unsigned short)BT_ISTR_NO_MELEE_WEAPONS < BT_NITEM_STRS)
        s->scalar_effect[BT_ISTR_NO_MELEE_WEAPONS] = (float)d->no_melee_weapons;
#endif
#if defined(BT_ISTR_NO_RANGED_WEAPONS)
    if ((unsigned short)BT_ISTR_NO_RANGED_WEAPONS < BT_NITEM_STRS)
        s->scalar_effect[BT_ISTR_NO_RANGED_WEAPONS] = (float)d->no_ranged_weapons;
#endif
#if defined(BT_ISTR_MIN_WEAPON_TIER)
    if ((unsigned short)BT_ISTR_MIN_WEAPON_TIER < BT_NITEM_STRS)
        s->scalar_effect[BT_ISTR_MIN_WEAPON_TIER] = (float)d->min_weapon_tier;
#endif
#if defined(BT_ISTR_MAX_WEAPON_TIER)
    if ((unsigned short)BT_ISTR_MAX_WEAPON_TIER < BT_NITEM_STRS)
        s->scalar_effect[BT_ISTR_MAX_WEAPON_TIER] = (float)d->max_weapon_tier;
#endif
#if defined(BT_ISTR_HP_SHOP)
    if ((unsigned short)BT_ISTR_HP_SHOP < BT_NITEM_STRS)
        s->scalar_effect[BT_ISTR_HP_SHOP] = (float)d->hp_shop;
#endif
}

/* linked_stats.gd:12-78: the Knight's gain_stat_for_every_stat link is a
 * character effect, not an item row, so the core link recompute must add it
 * after its item rows and before bt_core_stat_sync.  amount = int(value *
 * (source / nb)) where source is RunData.get_stat (+ TempStats when the row is
 * not permanent-only), i.e. the gained source value. */
BT_HD static void bt_character_query_links(BT_DEVICE BtState* s) {
    unsigned short c = bt_character_index(s->character);
    BT_CONST_PTR(BtCharacterDef) d = &BT_CHARACTERS[c];
    int t = (int)d->link_target;
    int src = (int)d->link_source;
    if (t >= BT_NSTATS || src >= BT_NSTATS || d->link_nb == 0) return;
    float srcraw = s->stat_raw[src] + (d->link_perm_only ? 0.0f : s->temp_stat[src]);
    float srcv = srcraw * s->stat_gain[src];
    s->link_stat[t] += bt_stat_link_gain((float)d->link_value, srcv, (float)d->link_nb);
}

/* run_data.gd:1259 get_free_weapon_slots / :1271 has_weapon_slot_available. */
BT_HD static unsigned char bt_character_weapon_slots(unsigned short character) {
    return BT_CHARACTERS[bt_character_index(character)].weapon_slots;
}

BT_HD static unsigned char bt_character_no_melee(unsigned short character) {
    return BT_CHARACTERS[bt_character_index(character)].no_melee_weapons;
}

BT_HD static unsigned char bt_character_no_ranged(unsigned short character) {
    return BT_CHARACTERS[bt_character_index(character)].no_ranged_weapons;
}

/* item_service.gd:309 / ui/menus/shop/shop_items_container.gd:98: the weapon
 * pool tier is clamped to [min_weapon_tier, max_weapon_tier]. */
BT_HD static unsigned char bt_character_min_weapon_tier(unsigned short character) {
    return BT_CHARACTERS[bt_character_index(character)].min_weapon_tier;
}

BT_HD static unsigned char bt_character_max_weapon_tier(unsigned short character) {
    return BT_CHARACTERS[bt_character_index(character)].max_weapon_tier;
}

/* item_service.gd:313-450: the character's banned item groups / items are
 * removed from the shop pool; wanted tags bias the pool roll. */
BT_HD static unsigned short bt_character_ban_group_mask(unsigned short character) {
    return BT_CHARACTERS[bt_character_index(character)].ban_group_mask;
}

BT_HD static int bt_character_item_banned(unsigned short character, unsigned short item) {
    BT_CONST_PTR(BtCharacterDef) d = &BT_CHARACTERS[bt_character_index(character)];
    if (d->ban_count == 0) return 0;
    for (int i = 0; i < (int)d->ban_count; i++)
        if (BT_CHARACTER_BAN_ITEMS[d->ban_first + i] == item) return 1;
    return 0;
}

BT_HD static int bt_character_wants_item(unsigned short character, unsigned short item) {
    BT_CONST_PTR(BtCharacterDef) d = &BT_CHARACTERS[bt_character_index(character)];
    if (d->want_count == 0) return 0;
    if (item >= BT_NITEMS) return 0;
    BT_CONST_PTR(BtItem) it = &BT_ITEMS[item];
    for (int t = 0; t < (int)it->tag_count; t++) {
        unsigned short tag = BT_ITEM_TAGS[it->tag_first + t];
        for (int w = 0; w < (int)d->want_count; w++)
            if (BT_CHARACTER_WANT_TAGS[d->want_first + w] == tag) return 1;
    }
    return 0;
}

/* run_data.gd:1428-1439: with hp_shop the shop currency IS stat_max_hp,
 * otherwise it is the run's gold (materials).
 *
 * get_player_currency returns `get_stat(Keys.stat_max_hp_hash) as int`.
 * RunData.get_stat (run_data.gd:1305-1307) is `get_player_effect(stat) *
 * get_stat_gain(stat)` -- the PERMANENT effects layer times its gain, with
 * NEITHER TempStats NOR LinkedStats folded in (those only join in Utils.get_stat,
 * utils.gd:461-466, which get_player_currency does not call). It is also
 * UNCAPPED: hp_cap (Handcuffs) limits get_player_max_health, never the shop
 * currency, so a capped Demon still spends its true stat_max_hp. GDScript
 * `as int` truncates toward zero. The core's stat_raw/stat_gain ARE those two
 * layers, so read them directly rather than bt_core_stat_read (which adds the
 * temp/link layers).
 *
 * HP-shop rules (shop_item.gd:132-140, shop_items_container.gd:58-68,
 * base_shop.gd:388-410,499-500):
 *   - an offer's price is the ordinary gold price divided by 20 and rounded UP
 *     (bt_wshop_price_units -> ceil(value / 20)); the minimum price is 1;
 *   - a purchase is allowed iff currency >= price, so max_hp can be spent down
 *     to 0: there is NO death-by-shop and no minimum-HP floor on the currency.
 *     get_player_max_health (run_data.gd:301-302) floors *health* at max(1, ...)
 *     separately, so the player survives at 1 HP;
 *   - the price is paid with remove_stat(stat_max_hp) -- exactly `price` points
 *     off the RAW/permanent layer (not scaled by gain). Currency is derived from
 *     raw*gain, so the currency only falls by price*gain; with gain == 1 (the
 *     Demon default) it falls by exactly the price.
 *
 *   - rerolls (base_shop.gd:406-410) and weapon/item recycling
 *     (base_shop.gd:727, RunData.add_gold) ALWAYS use gold, never max HP;
 *   - gold pickups, harvesting and gold-on-kill still credit `materials`
 *     (add_gold), they never become max HP directly -- only the end-of-wave
 *     conversion below turns gold into Max HP. */
BT_HD static float bt_character_currency(BT_DEVICE BtState* s) {
    if (BT_CHARACTERS[bt_character_index(s->character)].hp_shop)
        return (float)(int)(s->stat_raw[0] * s->stat_gain[0]);
    return s->materials;
}

/* remove_currency: remove_stat(stat_max_hp) under hp_shop, remove_gold (clamped
 * to >= 0, run_data.gd:821-823) otherwise. */
BT_HD static void bt_character_currency_remove(BT_DEVICE BtState* s, float value) {
    if (BT_CHARACTERS[bt_character_index(s->character)].hp_shop) {
        bt_core_stat_add(s, 0, -value);
    } else {
        s->materials -= value;
        if (s->materials < 0.0f) s->materials = 0.0f;
    }
}

/* utils.gd:559-588 convert_stats, driven by demon_effect_1
 * (key="materials", pct_converted=50.0, value=13, to_stat="stat_max_hp",
 * to_value=1), run at the end of every wave from main.gd:1225 -- AFTER the
 * stats_end_of_wave grant loop and BEFORE manage_harvesting, so harvesting gold
 * is not converted in the same wave.
 *
 *   value         = get_player_gold()          (materials; run_data.gd:818-825
 *                                              get_player_gold is the int gold)
 *   chunks        = max(0, floor(value * 0.50 / 13))
 *   chunks == 0   -> nothing happens (no gold removed, no max HP added)
 *   removed       = round(chunks * 13   / get_stat_gain("materials"))
 *   added         = round(chunks * 1    / get_stat_gain("stat_max_hp"))
 *   remove_gold(removed); add_stat(stat_max_hp, added)
 *
 * i.e. 50% of the run's gold, in chunks of 13 (the guide's "13 materials = 1
 * Max HP"), each chunk +1 stat_max_hp. The gains are the source's
 * get_stat_gain(): `gain_materials` has no producer so its gain is always 1,
 * `gain_stat_max_hp` is the summed item/scalar modifier (1 + x/100). Gold is
 * ALWAYS gold here even under hp_shop; the conversion is what turns it into Max
 * HP (gold pickups/harvesting never do). The `remove_gold` clamp to >= 0 only
 * matters if the amount were fractional; get_player_gold is an int. */
BT_HD static void bt_character_convert_end_of_wave(BT_DEVICE BtState* s) {
    BT_CONST_PTR(BtCharacterDef) d = &BT_CHARACTERS[bt_character_index(s->character)];
    if (d->convert_value <= 0 || d->convert_pct == 0) return;
    int from = (int)d->convert_from;
    float src = (from == BT_CHARACTER_STAT_NONE) ? s->materials : bt_core_stat_read(s, from);
    float chunks_f = bt_floorf((src * ((float)d->convert_pct * 0.01f)) / (float)d->convert_value);
    int chunks = (int)chunks_f;
    if (chunks < 0) chunks = 0;
    if (chunks == 0) return;
    float removed_gain = (from == BT_CHARACTER_STAT_NONE) ? 1.0f : s->stat_gain[from];
    int to = (int)d->convert_to;
    float added_gain = (to == BT_CHARACTER_STAT_NONE) ? 1.0f : s->stat_gain[to];
    float base_remove = (float)chunks * (float)d->convert_value;
    float base_add = (float)d->convert_to_value * (float)chunks;
    float actual_remove = removed_gain > 0.0f ? bt_roundf(base_remove / removed_gain) : base_remove;
    float actual_add = added_gain > 0.0f ? bt_roundf(base_add / added_gain) : base_add;
    if (from == BT_CHARACTER_STAT_NONE) {
        s->materials -= actual_remove;                 /* remove_gold (clamped) */
        if (s->materials < 0.0f) s->materials = 0.0f;
    } else {
        bt_core_stat_add(s, from, -actual_remove);
    }
    if (to == BT_CHARACTER_STAT_NONE) {
        s->materials += actual_add;
    } else {
        bt_core_stat_add(s, to, actual_add);
    }
}

/* Bull explode_on_hit (player.gd:443-485, bull_effect_4 ItemExplodingEffect):
 * the core's bt_explode fires only while the profile enables it and rolls
 * `explode_chance`; the damage/crit/range/scaling values are read straight from
 * the profile's BtCharacterDef (bull_explosion_stats.tres) by the explosion
 * slice, so no accessor wraps them. */
BT_HD static int bt_character_explode_on_hit(unsigned short character) {
    return (int)BT_CHARACTERS[bt_character_index(character)].explode_on_hit;
}

BT_HD static float bt_character_explode_chance(unsigned short character) {
    return BT_CHARACTERS[bt_character_index(character)].explode_chance;
}

/* --------------------------------------------------------------------------
 * Episode bootstrap: enqueue the profile's starting loadout into the inventory
 * pending queue and apply it.  Owned by WeaponInventory (forward-declared here
 * like the core forward-declares its other slices); bt_core_reset calls
 * bt_character_starting_apply(s) after bt_character_apply(s), so every episode
 * re-creates the configured loadout.
 *
 * The three supported profiles carry NO character-owned starting_weapon /
 * cursed_starting_weapon effects (those belong to brawler/crazy/cyborg/
 * engineer/ranger/wildling), so the loadout is the explicit selection:
 * s->starting_weapon (a resolved BT_WEAPONS id, BT_CHARACTER_NO_WEAPON = none),
 * which the backend validates against the profile's pool (host-only
 * bt_character_start_index).  No selection means zero starting weapons -- the
 * source default for Bull and the unselected Knight/Demon case.
 *
 * bt_weapon_reset (core reset) clears the pending queue, so this must run once
 * per reset, after bt_character_apply.  bt_weapon_starting_apply appends every
 * queued copy unconditionally (run_data.gd:1547-1567 add_weapon) and returns
 * the number added. */
BT_HD static void bt_weapon_starting_enqueue(BT_DEVICE BtState* s,
                                             unsigned short weapon, int cursed);
BT_HD static int bt_weapon_starting_apply(BT_DEVICE BtState* s);

BT_HD static void bt_character_starting_apply(BT_DEVICE BtState* s) {
    if (s->starting_weapon != BT_CHARACTER_NO_WEAPON)
        bt_weapon_starting_enqueue(s, s->starting_weapon, 0);
    (void)bt_weapon_starting_apply(s);
}

/* --------------------------------------------------------------------------
 * Host-only: starting-weapon pool resolution (weapon_selection.gd:92-117).
 * The device never reads the my_id strings; the config resolves the selected
 * weapon to a BT_WEAPONS id and passes it as bt_core_init's starting_weapon.
 * ----------------------------------------------------------------------- */
BT_HOST_ONLY(static inline int bt_character_start_count(unsigned short character) {
    return (int)BT_CHARACTERS[bt_character_index(character)].start_count;
})

BT_HOST_ONLY(static inline const char* bt_character_start_name(unsigned short character,
                                                               int index) {
    BT_CONST_PTR(BtCharacterDef) d = &BT_CHARACTERS[bt_character_index(character)];
    if (index < 0 || index >= (int)d->start_count) return 0;
    return BT_CHARACTER_START_WEAPONS[d->start_first + index];
})

BT_HOST_ONLY(static inline int bt_character_start_type(unsigned short character, int index) {
    BT_CONST_PTR(BtCharacterDef) d = &BT_CHARACTERS[bt_character_index(character)];
    if (index < 0 || index >= (int)d->start_count) return -1;
    return (int)BT_CHARACTER_START_TYPE[d->start_first + index];
})

BT_HOST_ONLY(static inline int bt_character_start_index(unsigned short character,
                                                        const char* weapon_id) {
    BT_CONST_PTR(BtCharacterDef) d = &BT_CHARACTERS[bt_character_index(character)];
    if (weapon_id == 0) return -1;
    for (int i = 0; i < (int)d->start_count; i++) {
        const char* cand = BT_CHARACTER_START_WEAPONS[d->start_first + i];
        int k = 0;
        while (cand[k] != 0 && cand[k] == weapon_id[k]) k++;
        if (cand[k] == 0 && weapon_id[k] == 0) return i;
    }
    return -1;
})
