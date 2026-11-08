/* Source weapon shop: item/weapon offer mixture, guarantee/class/same-weapon
 * weighting, tier/curse/pricing, eligibility/bans, full-slot auto-combination,
 * recycle/combine and the character shop currency (Demon max-HP).
 *
 * SOURCE (all under /Users/leelachebiyyam/brotato_recovered, v1.1.14.6, plus
 * brotato_abyss for the DLC curse roll already modelled by brotato_core.h):
 *   singletons/item_service.gd:22-31   CHANCE_WEAPON / CHANCE_SAME_WEAPON /
 *                                      CHANCE_SAME_WEAPON_SET /
 *                                      MAX_WAVE_{TWO,ONE}_WEAPON(S)_GUARANTEED /
 *                                      BONUS_CHANCE_SAME_WEAPON(_SET)
 *   singletons/item_service.gd:221-275 get_player_shop_items (type selection,
 *                                      guarantees, no-slot force-to-items)
 *   singletons/item_service.gd:300-460 _get_rand_item_for_wave weapon branch
 *                                      (tier clamp, bans, same-weapon/set
 *                                      weighting, pool -> backup -> raw pool)
 *   singletons/item_service.gd:701-744 get_value / get_recycling_value
 *   singletons/item_service.gd:861-995 is_weapon_id / is_same_weapon
 *   ui/menus/shop/shop_items_container.gd:97-135 _can_weapon_be_bought
 *   ui/menus/shop/base_shop.gd:602-620 buy_weapon (full-slot auto-combine)
 *   ui/menus/shop/base_shop.gd:622-703 _combine_weapon (2 -> 1 upgrades_into)
 *   ui/menus/shop/base_shop.gd:706-746 _on_item_discard_button_pressed (recycle)
 *   ui/menus/shop/shop_item.gd:132-140 value / hp_shop currency
 *   ui/menus/shop/shop_items_container.gd:58-68 currency affordability gate
 *   singletons/run_data.gd:1262-1290 has_weapon_slot_available /
 *                                      player_has_weapon_slots
 *   singletons/run_data.gd:1317-1327 can_combine
 *   singletons/run_data.gd:1429-1437 get_player_currency / remove_currency
 *   singletons/run_data.gd:1043-1073 update_sets (active sets == >=1 weapon)
 *   items/characters/{bull,knight,demon}/ (character *_data.tres + *_effect_*)
 *
 * CONSUMES (owned elsewhere; included earlier in the same TU by brotato_core.h):
 *   brotato_weapons.h / brotato_weapon_types.h   (WeaponData)
 *       BtWeaponDef BT_WEAPONS[BT_NWEAPONS], BT_WEAPON_NONE, BT_NWEAPONS,
 *       BT_WEAPON_KIND_{MELEE,RANGED}, BT_WEAPON_SET_IDS.
 *   brotato_weapon_inventory.h                   (WeaponInventory)
 *       bt_weapon_count / bt_weapon_id / bt_weapon_tier / bt_weapon_kind /
 *       bt_weapon_family / bt_weapon_upgrades / bt_weapon_tier_clamp /
 *       bt_weapon_no_duplicate / bt_weapon_slot_available / bt_weapon_buy /
 *       bt_weapon_can_combine / bt_weapon_combine / bt_weapon_recycle_value /
 *       bt_weapon_price
 *   brotato_characters.h + brotato_character_runtime.h   (WeaponCharacters)
 *       BT_CHARACTERS / BT_CHARACTER_BULL, bt_character_index,
 *       bt_character_{weapon_slots,no_melee,no_ranged,min_weapon_tier,
 *       max_weapon_tier,hp_shop,currency,currency_remove,item_banned,
 *       ban_group_mask,wants_item,convert_end_of_wave}
 *   brotato_core.h helpers: bt_chance / bt_math_randf / bt_rng_bounded /
 *       bt_shop_price / bt_ceilf / bt_maxf.
 *   brotato_progression.h: bt_progress_item_tier (forward-declared below; it is
 *       defined by brotato_progression.h, which includes this header first).
 *
 * STATE CONTRACT (parent-owned brotato_state.h; WeaponSchema consumes these):
 *   unsigned char  shop_kind[4];  BT_SHOP_NONE / BT_SHOP_ITEM / BT_SHOP_WEAPON
 *   unsigned short shop_id[4];    canonical BT_ITEMS idx (ITEM) / BT_WEAPONS idx
 *                                 (WEAPON); 0 when NONE. The item-only obs block
 *                                 derives a NONE/WEAPON offer's empty index
 *                                 (0) from shop_kind, so no separate
 *                                 ITEM-only mirror array exists.
 *   unsigned char  shop_cursed[4]; existing DLC offer curse flag (unchanged)
 *   unsigned short character;     BT_CHARACTER_* profile
 */
#pragma once

/* Defined by brotato_progression.h (included after this header). */
BT_HD static int bt_progress_item_tier(BT_DEVICE BtState* s, int wave);


/* --- weapon schema access -------------------------------------------------
 * tier/kind/family/upgrades and the profile restrictions come from the
 * WeaponInventory accessors (bt_weapon_tier/kind/family/upgrades/valid,
 * bt_weapon_tier_clamp, bt_weapon_no_duplicate) and the WeaponCharacters
 * accessors (bt_character_*); only the three BtWeaponDef fields with no
 * accessor (can_be_looted / spawns_structure / sets) are read directly.
 * BtWeaponDef.kind is WeaponData.Type (BT_WEAPON_KIND_MELEE/RANGED). */
#define BTW_LOOTABLE(w)  ((int)BT_WEAPONS[(w)].can_be_looted)
#define BTW_STRUCT(w)    ((int)BT_WEAPONS[(w)].spawns_structure)
#define BTW_SET_FIRST(w) ((int)BT_WEAPONS[(w)].set_first)
#define BTW_SET_COUNT(w) ((int)BT_WEAPONS[(w)].set_count)

/* --- source constants (item_service.gd:22-31) ----------------------------- */
#define BT_WSHOP_NB_ITEMS 4
#define BT_WSHOP_CHANCE_WEAPON               0.35f
#define BT_WSHOP_CHANCE_SAME_WEAPON          0.20f
#define BT_WSHOP_CHANCE_SAME_WEAPON_SET      0.35f
#define BT_WSHOP_MAX_WAVE_TWO_WEAPONS_GUARANTEED 2
#define BT_WSHOP_MAX_WAVE_ONE_WEAPON_GUARANTEED  5
#define BT_WSHOP_BONUS_CHANCE_SAME_WEAPON_SET 0.20f
#define BT_WSHOP_BONUS_CHANCE_SAME_WEAPON     0.05f

/* Source effect keys whose canonical BT_ISTR_* macros WeaponData added (ids
 * stable). They are plain scalars in the core's effect store; `remove_shop_items`
 * is array-valued in the source but has no producer on any reachable profile, so
 * its scalar read is the empty default. */
BT_HD static int bt_wshop_minimum_weapons_in_shop(BT_DEVICE BtState* s) {
    return (int)s->scalar_effect[BT_ISTR_MINIMUM_WEAPONS_IN_SHOP];
}
BT_HD static int bt_wshop_no_structures(BT_DEVICE BtState* s) {
    return s->scalar_effect[BT_ISTR_REMOVE_SHOP_ITEMS] > 0.0f;
}

/* item_service.gd:398-408: the character's wanted-tag bias only filters when
 * the character HAS wanted tags (Demon has none, so it never filters). */
BT_HD static int bt_wshop_char_wants(BT_DEVICE BtState* s, int item) {
    if (BT_CHARACTERS[bt_character_index(s->character)].want_count == 0) return 1;
    return bt_character_wants_item(s->character, (unsigned short)item);
}

/* dlcs/dlc_1/dlc_1_data.gd:33-43 update_item_effects (item_service.gd:500-516
 * apply_item_effect_modifications calls it on EVERY generated offer, weapons
 * included):
 *   curse  = max(base_item_curse_chance*100, stat_curse)
 *   chance = max_curse_item_chance * (1 - 1/(1 + curse/50))
 * Both constants are the SHIPPED dlcs/dlc_1/dlc_data.tres values (see
 * brotato_item_effects.h): base 0.0 and max 0.15, so a 0-curse run's chance is
 * exactly 0.0 and Utils.get_chance_success (:397-403) returns false WITHOUT
 * consuming a draw -- the offer then carries no curse this wave.  Curse above 0
 * makes the chance positive and the draw happens (once per live offer), which is
 * why the flag must stay conditional rather than dropped. */
BT_HD static int bt_wshop_offer_banned(BT_DEVICE BtState* s, int kind, unsigned short id);
BT_HD static void bt_wshop_sound_draw(BT_DEVICE BtState* s);

BT_HD static int bt_wshop_offer_cursed(BT_DEVICE BtState* s) {
    float curse = bt_maxf(s->curse, BT_DLC1_CURSE_BASE_ITEM_CURSE_CHANCE * 100.0f);
    float chance = BT_DLC1_CURSE_MAX_CURSE_ITEM_CHANCE * (1.0f - 1.0f / (1.0f + curse / 50.0f));
    return bt_chance(s, chance);
}

/* base_shop.gd:479-481 set_reroll_button_price: the stored button price for the
 * CURRENT shop (0 while a free reroll remains, else the discounted
 * get_reroll_price at the run's wave-now and paid-reroll count). */
BT_HD static void bt_wshop_refresh_reroll_price(BT_DEVICE BtState* s) {
    int free = s->free_rerolls_remaining > 0 || s->has_bonus_free_reroll;
    s->shop_reroll_price = free ? 0 :
        (int)bt_ceilf((float)bt_reroll_cost(s->wave, s->paid_rerolls) *
        bt_maxf(0.1f, 1.0f + s->scalar_effect[BT_ISTR_REROLL_PRICE] * 0.01f));
}

/* --- currency (run_data.gd:1429-1437, shop_item.gd:132-140) --------------
 * Demon (hp_shop) prices in max-HP units: value = ceil(gold_value / 20) and
 * the currency is stat_max_hp; everyone else pays gold (materials). */
BT_HD static float bt_wshop_currency(BT_DEVICE BtState* s) {
    return bt_character_currency(s);
}
BT_HD static int bt_wshop_hp_shop(BT_DEVICE BtState* s) {
    return bt_character_hp_shop(s->character) ? 1 : 0;
}
BT_HD static float bt_wshop_price_units_at(BT_DEVICE BtState* s, int wave, int kind,
                                           unsigned short id) {
    float gold = (kind == BT_SHOP_ITEM) ? bt_shop_price_at(s, wave, id)
                                        : (float)bt_weapon_price_at(s, wave, id);
    if (bt_character_hp_shop(s->character)) return bt_ceilf(gold / 20.0f);   /* shop_item.gd:138 */
    return gold;
}
/* The current-wave form: an offer's ShopItem stores its price at creation
 * (shop_item.gd:129-140), so the STORED slot price (bt_shop_slot_price) is what
 * the observation, the buy gate and the legality row read; this live form stays
 * for callers the source itself evaluates at RunData.current_wave. */
BT_HD static float bt_wshop_price_units(BT_DEVICE BtState* s, int kind, unsigned short id) {
    return bt_wshop_price_units_at(s, (int)s->wave, kind, id);
}
BT_HD static void bt_wshop_pay(BT_DEVICE BtState* s, float units) {
    bt_character_currency_remove(s, units);   /* remove_stat(max_hp) or remove_gold */
}

/* --- own the shop-open side effects (base_shop.gd:53-78) -------------------
 * destroy_weapons (remove every owned weapon, once per run) and
 * upgrade_random_weapon (anvil: combine a random owned weapon) are both
 * produced only by keys that no Bull/Knight/Demon-profile effect carries, so
 * neither branch is reachable in this task; they are NOT modelled rather than
 * added as fake hooks (add here when a profile produces them). */

/* --- guarantees (item_service.gd:224-239) -------------------------------- */
BT_HD static int bt_wshop_weapons_guaranteed(BT_DEVICE BtState* s) {
    int g = 0;
    if (s->wave <= BT_WSHOP_MAX_WAVE_TWO_WEAPONS_GUARANTEED) g = 2;
    else if (s->wave <= BT_WSHOP_MAX_WAVE_ONE_WEAPON_GUARANTEED) g = 1;
    int m = bt_wshop_minimum_weapons_in_shop(s);
    if (m > g) g = m;
    return g;
}

/* run_data.gd:1262-1280 has_weapon_slot_available (no_duplicate + type caps +
 * capacity) is owned by WeaponInventory; the pool only needs its own bans. */
BT_HD static int bt_wshop_family_owned(BT_DEVICE BtState* s, int w) {
    int n = bt_weapon_count(s);
    for (int i = 0; i < n; i++) {
        int ow = (int)bt_weapon_id(s, i);
        if (ow == (int)BT_WEAPON_NONE) continue;
        if (bt_weapon_family((unsigned short)w) == bt_weapon_family((unsigned short)ow)) return 1;
    }
    return 0;
}
/* run_data.gd:1043-1073: active_sets keys are every set carried by >=1 owned
 * weapon (the >=2 check only gates the bonus effects, not the key set). */
BT_HD static int bt_wshop_set_shared(BT_DEVICE BtState* s, int w) {
    int sf = BTW_SET_FIRST(w), sc = BTW_SET_COUNT(w);
    if (sc <= 0) return 0;
    int n = bt_weapon_count(s);
    for (int i = 0; i < n; i++) {
        int ow = (int)bt_weapon_id(s, i);
        if (ow == (int)BT_WEAPON_NONE) continue;
        int of = BTW_SET_FIRST(ow), oc = BTW_SET_COUNT(ow);
        for (int a = 0; a < sc; a++)
            for (int b = 0; b < oc; b++)
                if (BT_WEAPON_SET_IDS[sf + a] == BT_WEAPON_SET_IDS[of + b]) return 1;
    }
    return 0;
}
/* _get_rand_item_for_wave weapon branch: the same-weapon branch wins over the
 * same-set branch, and both are skipped once rand_wanted >= CHANCE_SAME_SET. */
BT_HD static int bt_wshop_pref_ok(BT_DEVICE BtState* s, int w, float rand_wanted,
                                  float c_same, float c_set) {
    if (rand_wanted < c_same) return bt_wshop_family_owned(s, w);
    if (rand_wanted < c_set)  return bt_wshop_set_shared(s, w);
    return 1;
}
/* Every removal the source applies to BOTH pool and backup (bans + excluded). */
BT_HD static int bt_wshop_weapon_ban(BT_DEVICE BtState* s, int w,
        BT_THREAD const unsigned char* prev_kind, BT_THREAD const unsigned short* prev_id,
        unsigned int prev_mask, int filled) {
    int kind = bt_weapon_kind((unsigned short)w);
    /* players_data[].banned_items filters BOTH the pool and the backup pool
     * (item_service.gd:316-330); this helper serves modes 0 and 1 only, so the
     * raw-tier fallback stays unfiltered, exactly as in the source. */
    if (bt_wshop_offer_banned(s, BT_SHOP_WEAPON, (unsigned short)w)) return 1;
    if (bt_character_no_melee(s->character) && kind == BT_WEAPON_KIND_MELEE) return 1;
    if (bt_character_no_ranged(s->character) && kind == BT_WEAPON_KIND_RANGED) return 1;
    /* args.excluded_items = prev_items + new_items (removed from both pools) */
    for (int j = 0; j < BT_WSHOP_NB_ITEMS; j++)
        if ((prev_mask & (1u << j)) && prev_kind[j] == BT_SHOP_WEAPON &&
            prev_id[j] == (unsigned short)w) return 1;
    for (int j = 0; j < filled; j++)
        if (s->shop_kind[j] == BT_SHOP_WEAPON && s->shop_id[j] == (unsigned short)w) return 1;
    /* no_duplicate_weapons (item_service.gd:356-372), faithful to the LAST
     * owned representative per family (unique_weapon_ids values). */
    if (bt_weapon_no_duplicate(s)) {
        int n = bt_weapon_count(s), rep = -1;
        for (int i = 0; i < n; i++) {
            int ow = (int)bt_weapon_id(s, i);
            if (ow == (int)BT_WEAPON_NONE) continue;
            if (bt_weapon_family((unsigned short)w) == bt_weapon_family((unsigned short)ow)) rep = ow;
        }
        if (rep >= 0) {
            if (bt_weapon_tier((unsigned short)w) < bt_weapon_tier((unsigned short)rep)) return 1;
            if (w == rep && bt_weapon_upgrades((unsigned short)rep) == BT_WEAPON_NONE) return 1;
        }
    }
    /* remove_shop_items contains the structure tag (EntityService.is_weapon_
     * spawning_structure). */
    if (bt_wshop_no_structures(s) && BTW_STRUCT(w)) return 1;
    return 0;
}

/* _get_rand_item_for_wave weapon branch: rand_wanted is the caller's draw (the
 * draw order is type-selection, rand_wanted, tier, pick, curse; the item branch
 * consumes an additional wanted-tag roll that the weapon branch does not). */
BT_HD static void bt_wshop_generate(BT_DEVICE BtState* s, int slot, int increase_tier,
        float rand_wanted, BT_THREAD const unsigned char* prev_kind,
        BT_THREAD const unsigned short* prev_id, unsigned int prev_mask, int filled) {
    int tier = bt_weapon_tier_clamp(s, bt_progress_item_tier(s, s->wave) + increase_tier);
    if (tier < BT_LOOT_TIER_COMMON) tier = BT_LOOT_TIER_COMMON;
    if (tier > BT_LOOT_TIER_LEGENDARY) tier = BT_LOOT_TIER_LEGENDARY;
    float bonus = (float)(BT_WSHOP_MAX_WAVE_ONE_WEAPON_GUARANTEED + 1 - s->wave);
    if (bonus < 0.0f) bonus = 0.0f;
    float c_same = BT_WSHOP_CHANCE_SAME_WEAPON +
        bonus * (BT_WSHOP_BONUS_CHANCE_SAME_WEAPON / (float)BT_WSHOP_MAX_WAVE_ONE_WEAPON_GUARANTEED);
    float c_set = BT_WSHOP_CHANCE_SAME_WEAPON_SET +
        bonus * (BT_WSHOP_BONUS_CHANCE_SAME_WEAPON_SET / (float)BT_WSHOP_MAX_WAVE_ONE_WEAPON_GUARANTEED);

    int npool = 0, nback = 0, nraw = 0;
    for (int w = 0; w < BT_NWEAPONS; w++) {
        if (bt_weapon_tier((unsigned short)w) != tier || !BTW_LOOTABLE(w)) continue;
        nraw++;
        if (bt_wshop_weapon_ban(s, w, prev_kind, prev_id, prev_mask, filled)) continue;
        nback++;
        if (bt_wshop_pref_ok(s, w, rand_wanted, c_same, c_set)) npool++;
    }
    /* pool -> backup (bans only) -> raw tier pool (source's final fallback) */
    int mode = npool ? 0 : (nback ? 1 : (nraw ? 2 : -1));
    if (mode < 0) {   /* no weapon exists at this tier: explicit empty offer */
        s->shop_kind[slot] = BT_SHOP_NONE; s->shop_id[slot] = 0;
        return;
    }
    int count = mode == 0 ? npool : (mode == 1 ? nback : nraw);
    int pick = (int)bt_rng_bounded(&s->rng, (unsigned int)count);
    for (int w = 0; w < BT_NWEAPONS; w++) {
        if (bt_weapon_tier((unsigned short)w) != tier || !BTW_LOOTABLE(w)) continue;
        int member;
        if (mode == 0) member = !bt_wshop_weapon_ban(s, w, prev_kind, prev_id, prev_mask, filled) &&
                                bt_wshop_pref_ok(s, w, rand_wanted, c_same, c_set);
        else if (mode == 1) member = !bt_wshop_weapon_ban(s, w, prev_kind, prev_id, prev_mask, filled);
        else member = 1;
        if (!member) continue;
        if (pick-- == 0) {
            s->shop_kind[slot] = BT_SHOP_WEAPON;
            s->shop_id[slot] = (unsigned short)w;
            return;
        }
    }
    s->shop_kind[slot] = BT_SHOP_NONE; s->shop_id[slot] = 0;
}

/* shop_items_container.gd:97-135 _can_weapon_be_bought. lock_current_weapons is
 * not modelled (no lock scope; no Bull/Knight/Demon effect produces it).
 * Currency affordability is checked by the caller first (container:58-62). */
BT_HD static int bt_wshop_can_buy(BT_DEVICE BtState* s, unsigned short id) {
    if (!bt_weapon_valid(id)) return 0;
    int mn = (int)bt_character_min_weapon_tier(s->character);
    int mx = (int)bt_character_max_weapon_tier(s->character);
    int tier = bt_weapon_tier(id);
    if (tier > mx || tier < mn) return 0;
    int kind = bt_weapon_kind(id);
    if (bt_character_no_melee(s->character) && kind == BT_WEAPON_KIND_MELEE) return 0;
    if (bt_character_no_ranged(s->character) && kind == BT_WEAPON_KIND_RANGED) return 0;
    int avail = bt_weapon_slot_available(s, id);
    int has_same = 0;
    int n = bt_weapon_count(s);
    for (int i = 0; i < n; i++) if (bt_weapon_id(s, i) == id) { has_same = 1; break; }
    if (has_same && !avail) {
        unsigned short up = bt_weapon_upgrades(id);
        if (up != BT_WEAPON_NONE && bt_weapon_tier(up) <= mx) return 1;   /* auto-combine on buy */
    }
    return avail;
}

/* base_shop.gd:602-615 buy_weapon: add, or (no slot + a same-id copy with an
 * upgrade) add-then-combine. Returns the resulting slot or -1. */
BT_HD static int bt_wshop_buy(BT_DEVICE BtState* s, unsigned short id, int cursed) {
    if (!bt_wshop_can_buy(s, id)) return -1;
    /* base_shop.gd:602-615 buy_weapon: a purchase with no free slot routes
     * through _combine_weapon, which ends with the combine sound (:703). */
    int slot_available = bt_weapon_slot_available(s, id);
    int r = bt_weapon_buy(s, id, cursed, 0.0f);
    if (!slot_available && r >= 0) bt_wshop_sound_draw(s);
    return r;
}

/* base_shop.gd:622-703 player combine (2 same-id -> upgrades_into, curse merged
 * by WeaponInventory) and base_shop.gd:706-746 recycle. Both act on an OWNED
 * weapon slot (0..bt_weapon_count-1) during the shop. */
BT_HD static float bt_wshop_combine(BT_DEVICE BtState* s, int slot) {
    if (s->phase != 1) return 0.0f;
    if (slot < 0 || slot >= bt_weapon_count(s)) return 0.0f;
    if (!bt_weapon_can_combine(s, slot)) return 0.0f;
    (void)bt_weapon_combine(s, slot);
    bt_wshop_sound_draw(s);   /* base_shop.gd:703 */
    return 0.0f;
}
BT_HD static float bt_wshop_recycle(BT_DEVICE BtState* s, int slot) {
    if (s->phase != 1) return 0.0f;
    if (slot < 0 || slot >= bt_weapon_count(s)) return 0.0f;
    /* item_service.gd:732 get_recycling_value(wave, weapon.value * specific
     * factor, is_weapon=true); WeaponInventory owns the formula. */
    int value = bt_weapon_recycle_value(s, slot);
    (void)bt_weapon_remove(s, slot);
    /* base_shop.gd:735 RunData.add_gold -- GOLD, never hp, even for Demon. */
    s->materials += (float)value;
    bt_wshop_sound_draw(s);   /* base_shop.gd:744 */
    return 0.0f;
}

/* Actions 9..14 recycle owned weapon slot (action-9); 15..20 combine owned
 * weapon slot (action-15). Shop-only. */
BT_HD static float bt_wshop_owned_action(BT_DEVICE BtState* s, int action) {
    if (action >= 9 && action <= 14) return bt_wshop_recycle(s, action - 9);
    if (action >= 15 && action <= 20) return bt_wshop_combine(s, action - 15);
    return 0.0f;
}

/* Clear one offer slot (bought or emptied). */
BT_HD static void bt_wshop_clear_offer(BT_DEVICE BtState* s, int slot) {
    s->shop_kind[slot] = BT_SHOP_NONE;
    s->shop_id[slot] = 0;
}

/* --- offers: lock / ban (shop_item.gd:204-252) -----------------------------
 * change_lock_status TOGGLES one offer; lock_player_shop_item push_backs the
 * [item, wave] entry, so the lock ORDER decides the slot layout after a reroll
 * (base_shop.gd:360-380). `shop_locked[i]` stores rank+1 (0 = unlocked).
 * disable_item_locking (shop_item.gd:239) has no producer on any reached
 * profile and is not interned, so locking is always available. */
BT_HD static int bt_wshop_is_locked(BT_DEVICE BtState* s, int slot) {
    return slot >= 0 && slot < 4 && s->shop_locked[slot] != 0;
}
/* Drop a lock and keep the push_back order dense: the bought/banned offer's
 * ShopItem is deactivated (shop_item.gd deactivate(): `locked = false`) and its
 * entry leaves the container's item list (base_shop.gd:494-518), so the lock
 * list must forget it exactly like an explicit unlock (base_shop.gd:360-380). */
BT_HD static void bt_wshop_unlock_offer(BT_DEVICE BtState* s, int slot) {
    if (slot < 0 || slot >= 4 || s->shop_locked[slot] == 0) return;
    int gone = s->shop_locked[slot];
    s->shop_locked[slot] = 0;
    for (int i = 0; i < 4; i++)                              /* erase() keeps order */
        if ((int)s->shop_locked[i] > gone) s->shop_locked[i]--;
}
BT_HD static void bt_wshop_toggle_lock(BT_DEVICE BtState* s, int slot) {
    if (slot < 0 || slot >= 4) return;
    if (!(s->shop_available & (1u << slot)) || s->shop_kind[slot] == BT_SHOP_NONE) return;
    if (s->shop_locked[slot] == 0) {
        int rank = 0;
        for (int i = 0; i < 4; i++) if ((int)s->shop_locked[i] > rank) rank = (int)s->shop_locked[i];
        s->shop_locked[slot] = (unsigned char)(rank + 1);   /* push_back */
    } else {
        bt_wshop_unlock_offer(s, slot);
    }
}
/* shop_item.gd:204-210 ban_item + shop_items_container.gd:126-135: push the
 * offer's my_id_hash into players_data[].banned_items, spend one of the run's
 * BAN_MAX_TOKEN = 8 tokens and deactivate the offer. base_shop.gd:906-916 then
 * drops it and re-arms the emptied-shop bonus reroll. The ban UI is shown only
 * while `ban_mode` is on -- settings.ban_mode_toggled (run_data.gd:475,198),
 * which the RL bridge's default settings force true (rl_env.gd:34) -- and it is
 * ITEMS-only (shop_item.gd:108-124); the SHOP ban credits NO gold
 * (shop_item.gd:204-210 -- the crate ban at main.gd:1153 does). */
BT_HD static int bt_wshop_offer_banned(BT_DEVICE BtState* s, int kind, unsigned short id) {
    for (int i = 0; i < (int)s->ban_count; i++)
        if ((int)s->ban_kind[i] == kind && s->ban_id[i] == id) return 1;
    return 0;
}
BT_HD static void bt_wshop_ban(BT_DEVICE BtState* s, int slot) {
    if (slot < 0 || slot >= 4) return;
    if (!s->ban_mode || s->ban_tokens == 0) return;
    if (!(s->shop_available & (1u << slot)) || s->shop_kind[slot] == BT_SHOP_NONE) return;
    if (s->shop_kind[slot] == BT_SHOP_WEAPON) return;
    if (s->ban_count < 8 && !bt_wshop_offer_banned(s, BT_SHOP_ITEM, s->shop_id[slot])) {
        s->ban_kind[s->ban_count] = BT_SHOP_ITEM;
        s->ban_id[s->ban_count] = s->shop_id[slot];
        s->ban_count++;
    }
    s->ban_tokens--;
    bt_wshop_unlock_offer(s, slot);
    s->shop_available &= (unsigned char)~(1u << slot);
    bt_wshop_clear_offer(s, slot);
    s->has_bonus_free_reroll = (s->shop_available == 0) ? 1 : 0;
    bt_wshop_refresh_reroll_price(s);   /* base_shop.gd:851 on_shop_item_banned */
}

/* --- item recycle value (item_service.gd:701-742, is_weapon = false) -------
 * The crate RECYCLE path (main.gd:1145) pays this in gold; get_value with
 * is_weapon = false drops the weapons_price factor (bt_weapon_value_from is the
 * weapon twin). */
BT_HD static int bt_wshop_item_value(BT_DEVICE BtState* s, float base, int affected) {
    float ipf = affected ? 1.0f + s->scalar_effect[BT_ISTR_ITEMS_PRICE] * 0.01f : 1.0f;
    float ef = affected ? bt_loot_endless_factor(s->wave) / 5.0f : 0.0f;
    float v = (base + (float)s->wave + base * (float)s->wave * 0.1f) * ipf * (1.0f + ef);
    int r = (int)v;
    return r < 1 ? 1 : r;
}
BT_HD static int bt_wshop_item_recycle_value(BT_DEVICE BtState* s, unsigned short item) {
    if (item >= BT_NITEMS) return 0;
    int from = (int)BT_ITEMS[item].price;
    if (from == 1) return 1;                          /* get_recycling_value :743 */
    int affected = (s->wave <= BT_WAVES) ? 1 : 0;     /* :733 current_wave <= nb_of_waves */
    int shop_value = bt_wshop_item_value(s, (float)from, 1);
    float rv = (float)bt_wshop_item_value(s, (float)from, affected);
    float f = 0.25f + s->scalar_effect[BT_ISTR_RECYCLING_GAINS] * 0.01f;
    if (f < 0.01f) f = 0.01f;
    if (f > 1.0f) f = 1.0f;
    rv *= f;
    if (rv < 1.0f) rv = 1.0f;
    int rec = (int)rv;
    return shop_value < rec ? shop_value : rec;
}

/* --- combine / recycle sound (base_shop.gd:703,744) ------------------------
 * SoundManager.play(Utils.get_rand_element(combine_sounds | recycle_sounds),
 * 0, 0.1, true): shop.tscn:59-60 exports exactly ONE sound each, so
 * Utils.get_rand_element (utils.gd:391-395 -> Array.pick_random) takes ONE
 * global draw, then the clip's playback pitch draw
 * (sound_manager_parent.gd:45 `1.0 + rand_range(-0.1, 0.1)` = one
 * Math::random(double,double) = 3 draws) fires in the shop UI frame that starts
 * the clip -- the same frame the submission is drained (ONE clip per idle
 * frame).  This phase is action-abstracted (no frame loop) and the shared
 * audio queue is stepped only in combat, so BOTH draws are consumed at the
 * submit action, before any later shop action draws. */
BT_HD static void bt_wshop_sound_draw(BT_DEVICE BtState* s) {
    (void)bt_rng_bounded(&s->rng, 1u);                        /* the sound pick */
    /* The pitch roll is played when the clip is POPPED by SoundManager's own
     * _process (sound_manager_parent.gd:45).  The bridge neutralizes the shop
     * audio under audio_rng == 0 by emptying every queue before that pass, so
     * the pop -- and this draw -- does not happen there; the pick above still
     * does (it is the Array.pick_random argument, evaluated at play() time).
     * Modelling both unconditionally over-drew the shop by 3 words per action
     * sound (measured: the v5 s1 recycle consumed 4 words instead of 1). */
    if (s->audio_rng) (void)bt_rng_global_range_float(&s->rng, -0.1f, 0.1f);
}

/* --- shop entry, once per run (base_shop.gd:53-82) -------------------------
 * destroy_weapons (remove_all_weapons) has no producer on any reached profile.
 * upgrade_random_weapon IS produced by the anvil item (anvil_effect_0.tres:
 * KEY_VALUE, key stat_armor, value 2, custom_key upgrade_random_weapon), and
 * the source runs this whole block only while RunData.shop_effects_checked is
 * false, i.e. at the run's FIRST shop entry (:53,82 + run_data.gd:190,536). */
BT_HD static int bt_wshop_lock_current_weapons(BT_DEVICE BtState* s) {
    return s->scalar_effect[BT_ISTR_LOCK_CURRENT_WEAPONS] > 0.0f;
}
BT_HD static int bt_wshop_anvil_upgrade(BT_DEVICE BtState* s) {
    int n = bt_weapon_count(s), count = 0;
    for (int i = 0; i < n; i++) {
        unsigned short id = bt_weapon_id(s, i);
        if (bt_weapon_upgrades(id) == BT_WEAPON_NONE) continue;
        if (bt_weapon_tier(id) >= bt_weapon_max_tier(s)) continue;
        count++;
    }
    if (count <= 0) return 0;
    int pick = (int)bt_rng_bounded(&s->rng, (unsigned int)count);
    for (int i = 0; i < n; i++) {
        unsigned short id = bt_weapon_id(s, i);
        if (bt_weapon_upgrades(id) == BT_WEAPON_NONE) continue;
        if (bt_weapon_tier(id) >= bt_weapon_max_tier(s)) continue;
        if (pick-- == 0) { (void)bt_weapon_upgrade(s, i); return 1; }
    }
    return 0;
}
BT_HD static void bt_wshop_shop_entry(BT_DEVICE BtState* s) {
    if (s->shop_effects_checked) return;
    s->shop_effects_checked = 1;
    for (int a = 0; a < s->n_bk_active; a++) {
        int row = (int)s->bk_active[a];
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[row];
        if (!bt_eff_is(E, BT_ISTR_UPGRADE_RANDOM_WEAPON)) continue;
        int stat = bt_core_id_from_istr(E->key);   /* anvil key = stat_armor */
        for (int c = 0, nown = bt_row_owned(s, row); c < nown; c++) {
            int v = 0, v2 = 0;
            if (!bt_row_get(s, row, E, c, &v, &v2)) break;
            if (!bt_wshop_lock_current_weapons(s) && bt_wshop_anvil_upgrade(s)) continue;
            if (stat != 255) bt_core_stat_add(s, stat, (float)v);
        }
    }
    bt_recompute_links(s);
}
