/* Source progression: run_data.gd add_xp/level_up/remove_item, main.gd
 * end-wave upgrade queue/harvesting/wave-start gold, item_service.gd tiers and
 * base_shop.gd transactions + reroll/Go consumers.
 * Included by brotato_core.h after item/stat application helpers and after
 * brotato_loot.h (the loot selection helpers are called from here).
 *
 * INCLUDED BY brotato_core.h AFTER brotato_loot.h AND AFTER
 * brotato_weapon_shop.h (that header owns the weapon offer mixture, tier/curse
 * pricing, recycle/combine and the character shop currency; this file calls
 * bt_wshop_* below and it forward-declares bt_progress_item_tier).
 *
 * Owned consumers (all source-cited at their sites):
 *   G2 duplicate_item      mirror buy duplication + consume/replace
 *   G3 increase_tier_on_reroll goldfish consume on reroll + offer tier raise
 *   G5 gain_pct_gold_start_wave piggy bank wave-start gold
 *   G6 extra_elite_next_wave_chance candy bag zone elite queued for next wave
 *   G8 item_hourglass      Go-button wave replay + consume/replace
 *   free-reroll acquisition delta + emptied-shop bonus reroll
 *   item tier domain (items are source Tier 0..3, not the upgrade 1..4)
 *   extra_item_in_crate    pearl chance + treasure_map random, one roll per crate
 *   deferred item-box grant via the real (tiered/limited) loot selection
 *   shop offer mixture + kind-discriminated buy, recycle (9..14) and combine
 *   (15..20) actions (item_service.gd:221-460, base_shop.gd:602-746)
 */
#pragma once

/* The gameplay-RNG audio events this slice fires (CombatAudioRuntime):
 * bt_progress_award_levels -> on_levelled_up's level_up_sound, bt_progress_end_wave
 * -> clean_up_room's end_wave pick.  Included: brotato_combat_audio.h needs only
 * state + the generated audio table + the two queue headers. */
#include "brotato_combat_audio.h"

/* Core APIs this slice calls but does not define.
 *   bt_item_remove: run_data.gd:857-882 remove_item -- reverse ONE owned copy
 *     (exact inverse of bt_apply_item) and then apply BT_ITEMS[it].replaced_by.
 *     Parent-owned; called only for mirror / goldfish / hourglass.
 *   bt_loot_*: brotato_loot.h selection helpers (that header is included before
 *     this one by brotato_core.h; declared here so this file compiles against
 *     the documented contract even if include order shifts).
 *   bt_temporal_reset / bt_temporal_scalar_effective: TemporalStatEvents
 *     (brotato_temporal_events.h, included later in the same TU).
 *   BtState.pending_extra_elite (int, -1 = none): parent-owned state for G6.
 *     action==8 sets the selected zone elite kind; the core's wave-start hook
 *     (bt_spawn_wave_extras, at the TOP, before the scheduled elites) must
 *     bt_rand_pos + bt_queue_boss it through the ordinary birth lifecycle and
 *     reset it to -1. Reset to -1 in bt_core_reset too. */
BT_HD static void bt_item_remove(BT_DEVICE BtState* s, unsigned short it);
BT_HD static float bt_progress_crate_take(BT_DEVICE BtState* s);
BT_HD static float bt_progress_crate_recycle(BT_DEVICE BtState* s);
BT_HD static int bt_loot_pick_box_item(BT_DEVICE BtState* s, int wave, int fixed_tier);
BT_HD static int bt_loot_item_cursed(BT_DEVICE BtState* s);
BT_HD static int bt_loot_item_banned(BT_DEVICE BtState* s, int it, int wave);
BT_HD static void bt_loot_wave_end_begin(BT_DEVICE BtState* s);
BT_HD static void bt_loot_wave_end_step(BT_DEVICE BtState* s);
BT_HD static void bt_loot_wave_end_drop_remaining(BT_DEVICE BtState* s);
BT_HD static void bt_temporal_reset(BT_DEVICE BtState* s);
BT_HD static float bt_temporal_scalar_effective(BT_DEVICE BtState* s, unsigned short key);

/* run_data.gd:18 ENDLESS_HARVESTING_DECREASE: past the last wave the wave-end
 * harvesting growth is replaced by this percentage LOSS per wave. */
#define BT_ENDLESS_HARVESTING_DECREASE 20

BT_HD static int bt_progress_tier(BT_DEVICE BtState* s, int wave) {
    float draw = bt_rand_range(s, 0.0f, 1.0f);
    float luck = s->luck * 0.01f;
    for (int tier = 4; tier >= 2; tier--) {
        int min_wave = tier == 4 ? 6 : (tier == 3 ? 2 : 0);
        float bonus = tier == 4 ? 0.0023f : (tier == 3 ? 0.02f : 0.06f);
        float limit = tier == 4 ? 0.08f : (tier == 3 ? 0.25f : 0.6f);
        float chance = bt_maxf(0.0f, (float)(wave - 1 - min_wave) * bonus);
        chance = luck >= 0.0f ? chance * (1.0f + luck) : chance / (1.0f - luck);
        if (draw <= bt_minf(chance, limit)) return tier;
    }
    return 1;
}

/* The item pools use the SOURCE Tier enum 0..3 (item_service.gd:60-203
 * _tiers_data; ItemParentData.tier), while bt_progress_tier returns the upgrade
 * table's 1..4 (resource tier + 1 -- see brotato_upgrades.h). The shop must roll
 * the item domain, so subtract one; bt_progress_roll_upgrade keeps calling
 * bt_progress_tier directly. */
BT_HD static int bt_progress_item_tier(BT_DEVICE BtState* s, int wave) {
    int tier = bt_progress_tier(s, wave) - 1;
    if (tier < 0) tier = 0;
    return tier;
}

/* Effect.key holding an "item_<id>" interned string -> BT_ITEMS index. Only the
 * keys the shipped pool actually targets are listed (mirror_effect_0 key
 * item_mirror, goldfish_effect_0 key item_goldfish, pearl_effect_1 key
 * item_pearl); no other item-key has a producer, and every one resolves to
 * BT_ITEM_NONE. (The generated tables carry no generic item-key -> index map.) */
BT_HD static int bt_progress_item_index_from_istr(unsigned short key) {
    switch (key) {
    case BT_ISTR_ITEM_MIRROR:   return BT_ITEM_IDX_MIRROR;
    case BT_ISTR_ITEM_GOLDFISH: return BT_ITEM_IDX_GOLDFISH;
    case BT_ISTR_ITEM_PEARL:    return BT_ITEM_IDX_PEARL;
    default: return (int)BT_ITEM_NONE;
    }
}

/* item_service.gd:837-848 get_random_elite_id_hash_from_zone:
 *   Utils.get_rand_element(get_elites_from_zone(zone)).my_id_hash
 * get_elites_from_zone filters ItemService.elites by zone_id (all zone 0 elites
 * have zone_id 0) and Utils.get_rand_element is Array.pick_random -- one global
 * draw over the per-zone roster. Returns the BT_KINDS index of the chosen elite
 * (the core resolves ItemService.elites order through BT_ZONE_ELITES), or -1 for
 * an empty roster (the source would return null). */
BT_HD static int bt_progress_random_elite(BT_DEVICE BtState* s) {
    int nel = (int)BT_ZONE_NELITES[s->zone];
    if (nel <= 0) return -1;
    int pick = (int)bt_rng_bounded(&s->rng, (unsigned int)nel);
    return (int)BT_ZONE_ELITES[s->zone][pick];
}

BT_HD static void bt_progress_roll_upgrade(BT_DEVICE BtState* s) {
    /* item_service.gd:550-556 get_upgrades early return: when the run owns the
     * weapon_slot_upgrades effect and the current weapon_slot effect is BELOW
     * it, the picker returns the single `upgrade_weapon_slot` offer and shows
     * ONE choice node (upgrades_ui_player_container.gd:61-64 loops i <
     * upgrades.size(), so only slot 0 is visible and the reroll button
     * (`visible = upgrades.size() > 1`, :71) is hidden). */
    float slot_up = s->scalar_effect[BT_ISTR_WEAPON_SLOT_UPGRADES];
    if (slot_up > 0.0f && s->scalar_effect[BT_ISTR_WEAPON_SLOT] < slot_up) {
        s->lvl_offers[0] = (unsigned short)BT_UPGRADE_SLOT_OFFER;
        for (int i = 1; i < 4; i++) s->lvl_offers[i] = 0;   /* absent (row+1 encoding) */
        for (int i = 0; i < 4; i++) s->lvl_prev[i] = s->lvl_offers[i];
        return;
    }
    int level = s->level - s->pending_upgrades + 1;
    for (int slot = 0; slot < 4; slot++) {
        for (int tries = 0; ; tries++) {
            int tier = bt_progress_tier(s, level);
            if (level == 5) tier = 2;
            else if (level == 10 || level == 15 || level == 20) tier = 3;
            else if (level % 5 == 0) tier = 4;
            int n = 0;
            for (int i = 0; i < BT_NUPGRADES; i++) if (BT_UPGRADES[i].tier == tier) n++;
            int pick = (int)bt_rng_bounded(&s->rng, (unsigned int)n);
            int index = 0;
            for (int i = 0; i < BT_NUPGRADES; i++) {
                if (BT_UPGRADES[i].tier != tier) continue;
                if (pick-- == 0) { index = i; break; }
            }
            /* item_service.gd:561-565 is_upgrade_already_added: the offer is
             * redrawn while it is already in this call's set (slots < slot) or in
             * old_upgrades -- the four offers shown at the PREVIOUS level-up of
             * this session (upgrades_ui_player_container.gd:37,65-66; the
             * container lives in main.tscn, so the set starts empty every wave).
             * The comparison is on upgrade_id_hash, and UpgradeData.upgrade_id
             * names the STAT (all four tiers of a stat share it, e.g.
             * "upgrade_lifesteal"), so the test is "same BT_UPGRADES stat" in
             * both sets -- a different TIER of a stat offered earlier this
             * session is still a duplicate. */
            int duplicate = 0;
            for (int j = 0; j < slot; j++) {
                int rj = bt_lvl_offer_row(s->lvl_offers[j]);
                if (rj >= 0 && BT_UPGRADES[rj].stat == BT_UPGRADES[index].stat) duplicate = 1;
            }
            for (int j = 0; j < 4; j++) {
                int rj = bt_lvl_offer_row(s->lvl_prev[j]);
                if (rj >= 0 && BT_UPGRADES[rj].stat == BT_UPGRADES[index].stat) duplicate = 1;
            }
            s->lvl_offers[slot] = (unsigned short)(index + 1);
            if (!duplicate || tries >= 50) break;   /* item_service.gd:563 */
        }
    }
    /* upgrades_ui_player_container.gd:66 _old_upgrades = upgrades */
    for (int i = 0; i < 4; i++) s->lvl_prev[i] = s->lvl_offers[i];
}

BT_HD static void bt_progress_award_levels(BT_DEVICE BtState* s) {
    while (s->xp >= (float)s->xp_need) {
        s->xp -= (float)s->xp_need;
        s->level++;
        /* main.gd:863 on_levelled_up: a DIRECT Resource play (no pick draw) with
         * always_play true, once per level gained. */
        bt_combat_audio_level_up(s);
        s->pending_upgrades++;
        s->xp_need = bt_xp_needed_pct(s->character, s->level);
        bt_core_stat_add(s, 0, 1.0f);   /* main.gd:874, before choosing an upgrade */
        for (int a = 0; a < s->n_bk_active; a++) {
            int row = (int)s->bk_active[a];
            BT_CONST_PTR(BtItemEff) effect = &BT_ITEM_EFF_ROWS[row];
            if (effect->custom_key != BT_ISTR_STATS_ON_LEVEL_UP) continue;
            float gain = bt_row_sum_v(s, row, effect);
            int id = bt_core_id_from_istr(effect->key);
            if (id != 255) bt_core_stat_add(s, id, gain);
            else s->scalar_effect[effect->key] += gain;
        }
        bt_recompute_links(s);
    }
}

BT_HD static void bt_progress_add_xp(BT_DEVICE BtState* s, float value) {
    if (value <= 0.0f) return;
    /* run_data.gd:747-752 add_xp: `value` is declared `value: int`, so every
     * caller's float (the manage_harvesting total, a gold pickup's value) is
     * truncated toward zero on the way in; the multiply is then
     * value * (1 + xp_gain/100). */
    value = (float)(int)value;
    if (value <= 0.0f) return;
    /* xp_gain has no core stat id; the effective value is the permanent SUM
     * effect plus this wave's stats_next_wave temp (celery_tea / peacock). */
    float xp_gain = bt_temporal_scalar_effective(s, BT_ISTR_XP_GAIN);
    s->xp += value * (1.0f + xp_gain * 0.01f);
    bt_progress_award_levels(s);
}

BT_HD static int bt_progress_item_candidate(BT_DEVICE BtState* s, int item, int tier,
        int wanted, int pass, BT_THREAD const unsigned char* previous_kind,
        BT_THREAD const unsigned short* previous_id, unsigned int previous_mask,
        int filled) {
    BT_CONST_PTR(BtItem) candidate = &BT_ITEMS[item];
    if ((int)candidate->tier != tier) return 0;
    /* Every pool level in _get_rand_item_for_wave is a subset of
     * _tiers_data[tier][ITEMS], i.e. the lootable pool (item_service.gd:105-135
     * init_unlocked_pool under the full-unlock profile), so this holds in every
     * pass. */
    if (!bt_loot_item_pool_ok(s, (unsigned short)item)) return 0;
    if (pass < 2) {
        /* players_data[].banned_items is removed from BOTH pools at the TOP of
         * _get_rand_item_for_wave (item_service.gd:316-330), so it filters every
         * pass level; only the ban action (shop_item.gd:204-210) fills it. */
        if (bt_wshop_offer_banned(s, BT_SHOP_ITEM, (unsigned short)item)) return 0;
        if (candidate->max_nb >= 0 && (int)s->item_owned[item] >= (int)candidate->max_nb) return 0;
        /* args.excluded_items = prev_items + new_items; a WEAPON offer excludes
         * no item, so the exclusion is kind-discriminated (WeaponShop). */
        for (int j = 0; j < 4; j++)
            if ((previous_mask & (1u << j)) && previous_kind[j] == BT_SHOP_ITEM &&
                previous_id[j] == (unsigned short)item) return 0;
        for (int j = 0; j < filled; j++)
            if (s->shop_kind[j] == BT_SHOP_ITEM && s->shop_id[j] == (unsigned short)item) return 0;
    }
    if (pass == 0) {
        /* item_service.gd:419-443: the pool's ban list is the ACTIVE profile's
         * banned_item_groups + banned_items while current_wave < nb_of_waves
         * (20) and banned_items_for_endless (item_piggy_bank, item_crown) in the
         * else branch past it; the character ban list is NOT applied in endless.
         * That whole expression is bt_loot_item_banned (brotato_loot.h:456-460),
         * the same helper the crate path uses, so the shop and the loot pool
         * share one source rule. `bt_loot_item_pool_ok` above covers
         * can_be_looted + max_nb != 0. */
        if (bt_loot_item_banned(s, item, s->wave)) return 0;
        if (wanted && !bt_wshop_char_wants(s, item)) return 0;
    }
    return 1;
}

/* `increase_tier` is base_shop.gd:359-373's args.increase_tier: 0 at shop open,
 * +N goldfish value on a reroll. get_tier_from_wave clamps tier+N to Tier
 * COMMON..LEGENDARY (0..3) for every generated offer.
 *
 * Offer mixture (item_service.gd:221-275): the type-selection draw, the
 * guarantee count and the no-slot force-to-items are the ONLY differences
 * between a weapon-capable profile and Bull; the per-slot draw order below is
 * identical in both branches (type draw, rand_wanted, tier, [item: wanted-tag,
 * pick, axolotl 50% reroll], curse), so Bull's stream is byte-identical. */
BT_HD static void bt_progress_roll_shop(BT_DEVICE BtState* s, int increase_tier) {
    unsigned char previous_kind[4];
    unsigned short previous_id[4];
    unsigned int previous_mask = s->shop_available;
    for (int i = 0; i < 4; i++) {
        previous_kind[i] = s->shop_kind[i];
        previous_id[i] = s->shop_id[i];   /* canonical id for either kind */
    }
    /* base_shop.gd:355-380 fill_shop_items: locked entries stay, in the order
     * lock_player_shop_item appended them, at the FRONT of the new list; the
     * remaining 4 - locked slots are regenerated at the tail. nb_locked_weapons
     * (item_service.gd:227-231) counts the locked WEAPON offers. */
    unsigned char lk_kind[4], lk_cursed[4], lk_avail[4];
    unsigned short lk_id[4];
    int nl = 0, nb_locked_weapons = 0;
    /* Walk the DESTINATION slots and pick the offer that belongs in each: the
     * k-th locked slot in (rank, slot) order is exactly what the previous form
     * emitted in its (rank, slot) walk.  Every lk_* index below is now the loop
     * variable k of a `k < 4` loop, the same shape as the previous_kind/
     * previous_id copy above, so no lk_* store is addressed by a data-dependent
     * value and `nl` can never exceed the 4-slot snapshot (the fill loop and the
     * shop_locked renumbering below both read lk_*[k] for k < nl).
     *
     * The earlier form indexed the snapshot with the running match counter (nl)
     * and the CUDA device build mis-addressed that store: compute-sanitizer
     * (with -lineinfo) reported "Invalid __local__ write of size 1 bytes ...
     * bt_progress_roll_shop+0x72f130 in brotato_progression.h:277" -- a write
     * past lk_kind, i.e. index 4 -- from the shop reroll
     * (bt_progress_shop_action -> bt_progress_roll_shop, :711), in exactly the
     * state where the counter's largest value addresses the last element: a
     * fully locked 4-slot shop.  No host-side build reproduces it (the CPU core
     * is clean under ASan/UBSan on the same states, including the non-contiguous
     * rank state a ban leaves: tests/test_brotato_shop_lock.cpp). */
    for (int k = 0; k < 4; k++) {
        int src = -1;
        for (int i = 0; i < 4 && src < 0; i++) {
            int ri = (int)s->shop_locked[i];
            if (ri == 0) continue;
            int before = 0;                  /* locked slots ahead of slot i */
            for (int j = 0; j < 4; j++) {
                int rj = (int)s->shop_locked[j];
                if (rj != 0 && (rj < ri || (rj == ri && j < i))) before++;
            }
            if (before == k) src = i;
        }
        if (src < 0) break;                  /* fewer than k + 1 locked slots */
        lk_kind[k] = s->shop_kind[src];
        lk_id[k] = s->shop_id[src];
        lk_cursed[k] = s->shop_cursed[src];
        lk_avail[k] = (unsigned char)((s->shop_available >> src) & 1u);
        if (lk_kind[k] == BT_SHOP_WEAPON) nb_locked_weapons++;
        nl++;
    }
    unsigned char relock[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < 4; i++) { s->shop_kind[i] = BT_SHOP_NONE; s->shop_id[i] = 0; s->shop_cursed[i] = 0; }
    s->shop_available = 0;
    for (int k = 0; k < nl; k++) {
        s->shop_kind[k] = lk_kind[k];
        s->shop_id[k] = lk_id[k];
        s->shop_cursed[k] = lk_cursed[k];
        if (lk_avail[k]) s->shop_available |= (unsigned char)(1u << k);
        relock[k] = (unsigned char)(k + 1);
    }
    int can_weapon = bt_character_weapon_slots(s->character) > 0;
    int weapons_guaranteed = bt_wshop_weapons_guaranteed(s);
    int weapons_offered = nb_locked_weapons;
    for (int slot = nl; slot < 4; slot++) {
        int kind;
        if (s->wave <= BT_WSHOP_MAX_WAVE_TWO_WEAPONS_GUARANTEED) {
            /* Guaranteed branch: no CHANCE_WEAPON draw (item_service.gd:247-249) */
            kind = (weapons_offered < weapons_guaranteed) ? BT_SHOP_WEAPON : BT_SHOP_ITEM;
        } else {
            /* else branch: get_chance_success(CHANCE_WEAPON) is evaluated first
             * (short-circuit `or`), so the draw always happens (:251-252) */
            int roll = bt_chance(s, BT_WSHOP_CHANCE_WEAPON);
            kind = (roll || weapons_offered < weapons_guaranteed) ? BT_SHOP_WEAPON : BT_SHOP_ITEM;
        }
        /* The source increments nb_weapons_added BEFORE the no-slot override
         * (:245-259), even when Bull then forces the offer back to items. */
        if (kind == BT_SHOP_WEAPON) weapons_offered++;
        if (!can_weapon) kind = BT_SHOP_ITEM;
        float rand_wanted = bt_math_randf(&s->rng);   /* _get_rand_item_for_wave rand_wanted */
        if (kind == BT_SHOP_WEAPON) {
            bt_wshop_generate(s, slot, increase_tier, rand_wanted,
                              previous_kind, previous_id, previous_mask, slot);
        } else {
            int tier = bt_progress_item_tier(s, s->wave) + increase_tier;
            if (tier < BT_LOOT_TIER_COMMON) tier = BT_LOOT_TIER_COMMON;
            if (tier > BT_LOOT_TIER_LEGENDARY) tier = BT_LOOT_TIER_LEGENDARY;
            int wanted = bt_chance(s, 0.05f);
            int count = 0, pass = 0;
            for (; pass < 3; pass++) {
                count = 0;
                for (int i = 0; i < BT_NITEMS; i++)
                    count += bt_progress_item_candidate(s, i, tier, wanted, pass,
                            previous_kind, previous_id, previous_mask, slot);
                if (count) break;
            }
            if (count <= 0) {
                /* The source's final fallback is _tiers_data[tier][ITEMS]; every
                 * source Tier 0..3 pool is non-empty, but an empty pool must be
                 * an explicit empty offer, never a stale id in this slot. */
                s->shop_kind[slot] = BT_SHOP_NONE;
                s->shop_id[slot] = 0;
            } else {
                int pick = (int)bt_rng_bounded(&s->rng, (unsigned int)count);
                int chosen = -1, seen = 0;
                for (int i = 0; i < BT_NITEMS; i++) {
                    if (!bt_progress_item_candidate(s, i, tier, wanted, pass,
                            previous_kind, previous_id, previous_mask, slot)) continue;
                    if (seen++ == pick) { chosen = i; break; }
                }
                /* item_service.gd:463-465: only the PRIMARY pool (pass 0) gives
                 * the picked axolotl a 50% chance to be rerolled, once, with a
                 * second pick from that same pool; the backup/raw fallbacks draw
                 * nothing. Identical to bt_loot_pick_box_item. */
                if (pass == 0 && chosen == BT_ITEM_IDX_AXOLOTL &&
                    bt_math_randf(&s->rng) < 0.5f) {
                    int repick = (int)bt_rng_bounded(&s->rng, (unsigned int)count);
                    int rseen = 0;
                    for (int i = 0; i < BT_NITEMS; i++) {
                        if (!bt_progress_item_candidate(s, i, tier, wanted, pass,
                                previous_kind, previous_id, previous_mask, slot)) continue;
                        if (rseen++ == repick) { chosen = i; break; }
                    }
                }
                if (chosen < 0) { s->shop_kind[slot] = BT_SHOP_NONE; s->shop_id[slot] = 0; }
                else {
                    s->shop_kind[slot] = BT_SHOP_ITEM;
                    s->shop_id[slot] = (unsigned short)chosen;
                }
            }
        }
        if (s->shop_kind[slot] != BT_SHOP_NONE)
            s->shop_cursed[slot] = (unsigned char)bt_wshop_offer_cursed(s);
        else
            s->shop_cursed[slot] = 0;   /* no offer: no curse draw */
    }
    for (int i = 0; i < 4; i++) s->shop_locked[i] = relock[i];
    /* shop_item.gd:129-140: EVERY ShopItem node of this fill is created through
     * set_shop_item(data, wave_value) with the wave the slot's offer belongs to
     * -- RunData.current_wave for a fresh offer, the lock entry's own stored
     * wave_value for a carried one (shop_item.gd:244 lock_player_shop_item
     * stores [item, wave_value]; base_shop.gd:439 re-runs set_shop_items).
     * keep_lock is off in the RL configuration (bt_progress_open_shop clears
     * shop_locked on entry), so a carried lock can only have been taken inside
     * THIS shop, i.e. its wave_value is s->wave -- which is why every slot is
     * stamped with it here.  The stamp is what keeps the price correct on the
     * GO frame, where current_wave has already advanced but the open shop's
     * prices are still its own (shop_reroll_price models the same freeze). */
    for (int i = 0; i < 4; i++) s->shop_price_wave[i] = s->wave;
    s->shop_available |= (unsigned char)(15u & (unsigned char)~((1u << nl) - 1u));
    /* A weapon tier with no weapon at all is an explicit empty offer (never a
     * silent wrong slot): its buy bit is cleared. */
    for (int i = 0; i < 4; i++)
        if (s->shop_kind[i] == BT_SHOP_NONE) s->shop_available &= (unsigned char)~(1u << i);
}

BT_HD static void bt_progress_open_shop(BT_DEVICE BtState* s) {
    s->phase = 1;
    s->shop_timer = 0;
    s->reroll_count = 0;
    s->paid_rerolls = 0;
    /* base_shop.gd:48-51: _initial_free_rerolls and _free_rerolls both start at
     * the owned free_rerolls total; a later acquisition raises the total and the
     * delta is added to the current shop (on_shop_item_bought, :530-536). */
    s->free_rerolls_remaining = (int)s->scalar_effect[BT_ISTR_FREE_REROLLS];
    s->initial_free_rerolls = s->free_rerolls_remaining;
    s->has_bonus_free_reroll = 0;   /* shop is freshly filled, never empty */
    bt_wshop_refresh_reroll_price(s);   /* base_shop.gd:132 _ready */
    /* base_shop.gd:110-120 _ready: without settings.keep_lock the per-shop lock
     * lists are cleared on entry, so each fresh shop starts fully unlocked. */
    for (int i = 0; i < 4; i++) s->shop_locked[i] = 0;
    s->crate_kind = BT_SHOP_NONE; s->crate_id = 0; s->crate_cursed = 0;
    s->crate_extra_count = 0;
    s->lvl_reroll_count = 0;
    s->lvl_reroll_price = 0;
    /* base_shop.gd:53-82 once-per-run shop entry (anvil upgrade_random_weapon). */
    bt_wshop_shop_entry(s);
    /* Fresh shop: no previous offer survives into the fill (the source's
     * prev_items is player_locked_items, empty without locking). */
    for (int i = 0; i < 4; i++) { s->shop_kind[i] = BT_SHOP_NONE; s->shop_id[i] = 0; }
    bt_progress_roll_shop(s, 0);
    for (int i = 0; i < 4; i++) s->lvl_offers[i] = 0;
}

/* upgrades_ui_player_container.gd:16,57-75,175-199: the level-up container has
 * its OWN gold-priced reroll counter (the shop's paid_rerolls is not
 * consumed); the price is ItemService.get_reroll_price(current_wave, count)
 * with the reroll_price stat discount, exactly like the shop's. */
BT_HD static int bt_progress_lvl_reroll_price(BT_DEVICE BtState* s) {
    return (int)bt_ceilf((float)bt_reroll_cost(s->wave, s->lvl_reroll_count) *
           bt_maxf(0.1f, 1.0f + s->scalar_effect[BT_ISTR_REROLL_PRICE] * 0.01f));
}
BT_HD static float bt_progress_choose_upgrade(BT_DEVICE BtState* s, int action) {
    /* Crate offers (BT_ACT_CRATE_TAKE/RECYCLE) are held in phase 2 while
     * crate_kind != BT_SHOP_NONE (upgrades_ui.gd:88-115 shows one at a time). */
    if (s->crate_kind != BT_SHOP_NONE) {
        if (action == BT_ACT_CRATE_TAKE) return bt_progress_crate_take(s);
        if (action == BT_ACT_CRATE_RECYCLE) return bt_progress_crate_recycle(s);
        return 0.0f;
    }
    /* item_service.gd:550-556 single-offer case: only the weapon-slot offer is
     * visible, so the reroll button is hidden (`visible = upgrades.size() > 1`)
     * and actions 1..3 have no choice node. */
    if (s->lvl_offers[0] == (unsigned short)BT_UPGRADE_SLOT_OFFER) {
        if (action != 0) return 0.0f;
        /* RunData.apply_item_effects(weapon_slot_upgrade_data): the single effect
         * is `weapon_slot` +1, a run-level scalar with no core stat id. */
        s->scalar_effect[BT_ISTR_WEAPON_SLOT] += BT_UPGRADE_SLOT_VALUE;
        bt_recompute_links(s);
        s->pending_upgrades--;
        if (s->pending_upgrades > 0) bt_progress_roll_upgrade(s);
        else bt_progress_open_shop(s);
        return 0.5f;
    }
    if (action == BT_ACT_REROLL) {
        /* The button is visible only with more than one offer
         * (upgrades_ui_player_container.gd:71), and bt_action_legal refuses the
         * id otherwise; mirror that gate so a rejected reroll is a no-op. */
        int offers = 0;
        for (int i = 0; i < 4; i++) offers += bt_lvl_offer_present(s, i);
        if (offers <= 1) return 0.0f;
        int cost = bt_progress_lvl_reroll_price(s);
        if (s->materials < (float)cost) return 0.0f;
        s->materials -= (float)cost;
        s->lvl_reroll_count++;
        bt_progress_roll_upgrade(s);   /* show_upgrades_for_level(_level) */
        return 0.0f;
    }
    if (action < 0 || action > 3) return 0.0f;
    /* Absent slot (0) or an out-of-range value: no choice node, no-op. */
    int row = bt_lvl_offer_row(s->lvl_offers[action]);
    if (row < 0) return 0.0f;
    BT_CONST_PTR(BtUpgrade) up = &BT_UPGRADES[row];
    float value = bt_upgrade_offer_value(s, up->value);   /* int(value*(1+mod/100)) */
    bt_core_stat_add(s, (int)up->stat, value);
    bt_recompute_links(s);
    s->pending_upgrades--;
    if (s->pending_upgrades > 0) bt_progress_roll_upgrade(s);
    else bt_progress_open_shop(s);
    return 0.5f;
}

/* base_shop.gd:560-585 buy_item. KEY_VALUE storage merges duplicate_item
 * entries by key into ONE [source_item, summed_value] entry, so the summed
 * owned value is the duplication count cap; APPEND_KEY_VALUE (goldfish) keeps
 * one entry per copy and only the first entry is read (:359-373).
 *
 * Source buy_item duplicates the SAME ALREADY-CURSE-BAKED item_data N times and
 * adds it (plus one normal copy) with NO further RNG: every copy must carry the
 * identical per-effect curse modifiers. The core rolls those modifiers inside
 * bt_apply_item (its only RNG use on the acquisition path -- verified: no
 * helper it calls draws), so snapshot the construction RNG state once and
 * replay it before every copy. The mirror removal itself draws nothing
 * (broken_mirror is uncursed), so the stream is left exactly ONCE advanced,
 * which is what a single real construction of the shared item_data consumes. */
BT_HD static void bt_progress_buy_item(BT_DEVICE BtState* s, unsigned short it) {
    for (int a = 0; a < s->n_bk_active; a++) {
        int row = (int)s->bk_active[a];
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[row];
        if (!bt_eff_is(E, BT_ISTR_DUPLICATE_ITEM)) continue;
        int src = bt_progress_item_index_from_istr(E->key);
        int total = (int)bt_row_sum_v(s, row, E);   /* KEY_VALUE merge = sum */
        if (src == (int)BT_ITEM_NONE || total <= 0) break;
        int maxn = (int)BT_ITEMS[it].max_nb;
        int remaining = (maxn < 0) ? 0x3FFFFFFF : maxn - (int)s->item_owned[it];
        if (remaining < 0) remaining = 0;
        int dup = total < remaining - 1 ? total : remaining - 1;
        if (dup > 0) {
            BtRng rng0 = s->rng;
            BtRng utils0 = s->utils_rng;
            float maxmod0 = s->curse_maxmod;
            for (int d = 0; d < dup; d++) {
                if ((int)s->item_owned[src] <= 0) break;
                unsigned char curse = s->curse_apply;
                s->curse_apply = 0;                       /* replacement is not cursed */
                bt_item_remove(s, (unsigned short)src);   /* mirror -> broken_mirror */
                s->curse_apply = curse;
                s->rng = rng0;                            /* replay the baked phenotype */
                s->utils_rng = utils0;
                s->curse_maxmod = maxmod0;
                bt_apply_item(s, it);
            }
            s->rng = rng0;                                /* normal copy: same playback */
            s->utils_rng = utils0;
            s->curse_maxmod = maxmod0;
        }
        break;
    }
    bt_apply_item(s, it);                                 /* the normal purchase */
}

/* base_shop.gd:359-373 (fill_shop_items, reroll only): the first owned
 * increase_tier_on_reroll entry sets args.increase_tier; the matching owned
 * item is then removed (goldfish -> goldfish_used). One item per reroll. */
BT_HD static int bt_progress_consume_reroll_tier(BT_DEVICE BtState* s) {
    for (int a = 0; a < s->n_bk_active; a++) {
        int row = (int)s->bk_active[a];
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[row];
        if (!bt_eff_is(E, BT_ISTR_INCREASE_TIER_ON_REROLL)) continue;
        int v = 0, v2 = 0;
        if (!bt_row_get(s, row, E, 0, &v, &v2)) return 0;
        int src = bt_progress_item_index_from_istr(E->key);
        if (src != (int)BT_ITEM_NONE && (int)s->item_owned[src] > 0) {
            unsigned char curse = s->curse_apply;
            s->curse_apply = 0;
            bt_item_remove(s, (unsigned short)src);
            s->curse_apply = curse;
        }
        return v;
    }
    return 0;
}

/* main.gd:1386-1400: at wave start gold is scaled by gain_pct_gold_start_wave
 * when (pct > 0 and wave <= nb_of_waves) or pct < 0; past the last wave a
 * negative pct is pinned to -100. add_gold's int parameter truncates the float
 * product (run_data.gd:810-815). */
BT_HD static void bt_progress_piggy_bank(BT_DEVICE BtState* s) {
    float pct = s->scalar_effect[BT_ISTR_GAIN_PCT_GOLD_START_WAVE];
    int apply = (pct > 0.0f && s->wave <= BT_WAVES) || pct < 0.0f;
    if (pct < 0.0f && s->wave > BT_WAVES) pct = -100.0f;
    if (apply) s->materials += (float)(int)(s->materials * pct * 0.01f);
}

/* upgrades_ui.gd:130-138: per crate, roll every extra_item_in_crate entry
 * (KEY_VALUE merge -> one entry per distinct key, summed percent). The rolls
 * are drawn BEFORE the crate's own item is generated (:114 precedes
 * show_consumable_data), so the caller must run this first. */
BT_HD static int bt_progress_crate_extra_roll(BT_DEVICE BtState* s, BT_THREAD unsigned short* keys,
                                              int cap) {
    int n = 0;
    for (int a = 0; a < s->n_bk_active; a++) {
        int row = (int)s->bk_active[a];
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[row];
        if (E->custom_key != BT_ISTR_EXTRA_ITEM_IN_CRATE) continue;
        if (E->key == BT_ISTR_NONE) continue;
        float chance = bt_row_sum_v(s, row, E) * 0.01f;
        if (!bt_chance(s, chance)) continue;
        if (n < cap) keys[n++] = E->key;
        else { s->pend_overflow = 1; s->pend_drop_crate++; }
    }
    return n;
}

/* upgrades_ui.gd:141-178 _get_extra_crate_item: one granted key per call,
 * resolved at SHOW time.  `random` picks a full-pool item (get_rand_item_for_wave
 * -> curse rolled); a concrete item key duplicates it with value 1 and is NOT
 * curse-rolled (get_item_from_id).  An item already at max_nb is dropped. */
BT_HD static void bt_progress_crate_offer(BT_DEVICE BtState* s, int kind,
                                          unsigned short id, int cursed) {
    s->crate_kind = (unsigned char)kind;
    s->crate_id = id;
    s->crate_cursed = (unsigned char)cursed;
}

/* Advance to the next offer the level-up UI would show (upgrades_ui.gd:88-115):
 * the extras granted for the CURRENT box first (:100-105), then the next queued
 * box's crate item (show_consumable_data -> :113 ItemService.process_item_box,
 * which rolls the box's extra keys BEFORE generating the item, :114). */
BT_HD static void bt_progress_next_crate_offer(BT_DEVICE BtState* s) {
    if (s->crate_extra_count > 0) {
        unsigned short key = s->crate_extra_key[0];
        for (int i = 1; i < (int)s->crate_extra_count; i++)
            s->crate_extra_key[i - 1] = s->crate_extra_key[i];
        s->crate_extra_count--;
        if (key == BT_ISTR_RANDOM) {
            int it = bt_loot_pick_box_item(s, s->wave, -1);
            if (it == (int)BT_ITEM_NONE) { bt_progress_crate_offer(s, BT_SHOP_NONE, 0, 0); return; }
            bt_progress_crate_offer(s, BT_SHOP_ITEM, (unsigned short)it, bt_loot_item_cursed(s));
            return;
        }
        int idx = bt_progress_item_index_from_istr(key);
        if (idx == (int)BT_ITEM_NONE) { bt_progress_crate_offer(s, BT_SHOP_NONE, 0, 0); return; }
        int maxn = (int)BT_ITEMS[idx].max_nb;
        if (maxn >= 0 && (int)s->item_owned[idx] >= maxn) { bt_progress_crate_offer(s, BT_SHOP_NONE, 0, 0); return; }
        bt_progress_crate_offer(s, BT_SHOP_ITEM, (unsigned short)idx, 0);
        return;
    }
    if (s->pending_items <= 0) { bt_progress_crate_offer(s, BT_SHOP_NONE, 0, 0); return; }
    int kind = (int)s->pending_item_kind[0];
    for (int i = 1; i < s->pending_items; i++) s->pending_item_kind[i - 1] = s->pending_item_kind[i];
    s->pending_items--;
    int n = bt_progress_crate_extra_roll(s, s->crate_extra_key, 8);
    s->crate_extra_count = (unsigned char)n;
    int fixed = (kind == BT_CONS_LEGENDARY_BOX) ? BT_LOOT_TIER_LEGENDARY : -1;
    int it = bt_loot_pick_box_item(s, s->wave, fixed);
    if (it == (int)BT_ITEM_NONE) { bt_progress_crate_offer(s, BT_SHOP_NONE, 0, 0); return; }
    bt_progress_crate_offer(s, BT_SHOP_ITEM, (unsigned short)it, bt_loot_item_cursed(s));
}

/* After the last crate offer: the queued level-up choices, else the shop
 * (main.gd:1118-1131 runs the boxes before the upgrades). */
BT_HD static void bt_progress_after_crates(BT_DEVICE BtState* s) {
    s->crate_kind = BT_SHOP_NONE;
    if (s->pending_upgrades > 0) {
        s->phase = 2;
        bt_progress_roll_upgrade(s);
    } else {
        bt_progress_open_shop(s);
    }
}

/* upgrades_ui_player_container.gd:183-190 _on_TakeButton_pressed ->
 * main.gd:1141 add_item(item_data): the crate item is taken with its baked curse
 * phenotype, then the next offer is shown. */
BT_HD static float bt_progress_crate_take(BT_DEVICE BtState* s) {
    if (s->crate_kind != BT_SHOP_ITEM) return 0.0f;
    s->curse_apply = s->crate_cursed ? 1 : 0;
    s->curse_maxmod = 0.0f;
    bt_apply_item(s, s->crate_id);
    s->curse_apply = 0;
    bt_progress_next_crate_offer(s);
    if (s->crate_kind == BT_SHOP_NONE) bt_progress_after_crates(s);
    return 0.0f;
}

/* upgrades_ui_player_container.gd:192-199 _on_DiscardButton_pressed ->
 * main.gd:1145 on_item_box_discard_button_pressed: gold =
 * get_recycling_value(current_wave, item_data.value, player) with
 * is_weapon = false, and the item is NOT taken. */
BT_HD static float bt_progress_crate_recycle(BT_DEVICE BtState* s) {
    if (s->crate_kind != BT_SHOP_ITEM) return 0.0f;
    int value = bt_wshop_item_recycle_value(s, s->crate_id);
    s->materials += (float)value;
    bt_progress_next_crate_offer(s);
    if (s->crate_kind == BT_SHOP_NONE) bt_progress_after_crates(s);
    return 0.0f;
}

/* The deferred half of base_shop.gd:339-349 _on_GoButton_pressed: the frame
 * that pressed GO ran the hourglass/extra-elite rolls and then asked the tree
 * for a scene change, which change_scene_to defers with call_deferred -- so the
 * new Main enters the tree in SceneTree::iteration's MessageQueue flush, i.e.
 * after that frame's physics walk (equivalent to that frame's END).  This runs
 * at the top of the following tick, which is therefore the GO frame itself: it
 * reproduces the fresh Main scene's wave entry (run_data.gd's current_wave
 * increment + the player rebuild) and the caller then returns WITHOUT running an
 * arena frame, exactly as the GO frame's own physics belongs to the Shop scene
 * (see bt_core_step).  The new scene's first arena frame is the NEXT tick. */
BT_HD static void bt_progress_shop_go(BT_DEVICE BtState* s) {
    s->shop_go_pending = 0;
    s->phase = 0;
    s->shop_available = 0;
    for (int i = 0; i < 4; i++) { s->shop_kind[i] = BT_SHOP_NONE; s->shop_id[i] = 0; }
    bt_enter_wave(s, s->wave);   /* the GO frame already advanced it */
    s->shop_go_wave_reset = 0;
    /* A fresh Main scene recreates the single player at the zone centre
     * (entity_spawner.gd:89-101), retaining the run's permanent stats: its
     * Player._ready runs update_player_stats(true), so the LiveStats
     * snapshot is the stat layer as of this wave's entry (after every
     * wave-entry stat add above). */
    bt_player_stats_reset(s);
    s->px = BT_ARENA_W * 0.5f;
    s->py = BT_ARENA_H * 0.5f;
    float percent = 100.0f + s->scalar_effect[BT_ISTR_HP_START_WAVE];
    if (s->scalar_effect[BT_ISTR_HP_START_NEXT_WAVE] != 0.0f)
        percent = 100.0f + s->scalar_effect[BT_ISTR_HP_START_NEXT_WAVE];
    s->hp = bt_maxf(1.0f, (float)(int)(s->max_hp * percent * 0.01f));
    s->scalar_effect[BT_ISTR_HP_START_NEXT_WAVE] = 0.0f;
    bt_progress_piggy_bank(s);
    s->iframe = 0.0f;
    s->regen_timer = bt_regen_period(s->regen);
}

BT_HD static float bt_progress_shop_action(BT_DEVICE BtState* s, int action) {
    /* The container's post-purchase buy delay (shop_items_container.gd:27-28,
     * 0.05 s): it ticks down once per shop frame and refuses BUY/STEAL ids only
     * -- the delay lives in on_shop_item_buy_button_pressed /
     * on_shop_item_steal_button_pressed, so reroll / skip / lock / ban / recycle
     * / combine all stay live while it runs (the v5 s1 row for tick 308 shows a
     * successful skip inside the window). */
    if (s->shop_timer > 0) {
        s->shop_timer--;
        if (action >= 0 && action <= 3) return 0.0f;
    }
    if (action == 8) {
        /* base_shop.gd:339-349 _on_GoButton_pressed: the hourglass/extra-elite
         * rolls happen HERE, in the frame the button is pressed, but the scene
         * swap it ends with is deferred to that frame's end -- the traced row for
         * this action still observes the Shop (obs[7] == 1, offers intact) and
         * the new wave's arena appears on the next tick (bt_progress_shop_go). */
        /* each owned hourglass adds
         * one wave replay and is consumed (hourglass -> broken_hourglass, whose
         * hp_start_next_wave -100 survives into the replay). max_nb == 1, so a
         * single remove_item covers the counter. */
        int wave_reset = 0;
        if ((int)s->item_owned[BT_ITEM_IDX_HOURGLASS] > 0) {
            wave_reset = (int)s->item_owned[BT_ITEM_IDX_HOURGLASS];
            s->curse_apply = 0;
            bt_item_remove(s, (unsigned short)BT_ITEM_IDX_HOURGLASS);
        }
        /* G6 base_shop.gd:340-343: extra_elite_next_wave_chance (candy bag),
         * rolled AFTER the hourglass loop and BEFORE the wave increments. On
         * success Utils.get_rand_element picks a zone elite and the group is
         * appended to extra_enemies_next_wave; it is materialised when the next
         * wave is built, so the core stores the kind here and the wave-start
         * hook queues it through the ordinary birth lifecycle (never an
         * immediate enemy). */
        s->pending_extra_elite = -1;
        if (bt_chance(s, s->scalar_effect[BT_ISTR_EXTRA_ELITE_NEXT_WAVE_CHANCE] * 0.01f))
            s->pending_extra_elite = bt_progress_random_elite(s);
        /* RunData.current_wave advances in the GO frame itself (the v5 row for
         * this action already reads wave 2 while the Shop scene is still up). */
        s->wave = s->wave + 1 - wave_reset;
        s->shop_go_pending = 1;
        s->shop_go_wave_reset = (unsigned char)(wave_reset > 255 ? 255 : wave_reset);
        return 0.0f;
    }
    if (action == 4) {
        /* set_reroll_button_price (:479-481): the price is 0 while free rerolls
         * remain OR the bonus-emptied-shop reroll is pending. */
        int free = s->free_rerolls_remaining > 0 || s->has_bonus_free_reroll;
        int cost = free ? 0 : s->shop_reroll_price;   /* the stored button price */
        if (s->materials < (float)cost) return 0.0f;
        s->materials -= (float)cost;
        /* _on_RerollButton_pressed :443-449: free rerolls are consumed first;
         * the bonus reroll only when no free reroll remains. */
        if (s->free_rerolls_remaining > 0 && !s->has_bonus_free_reroll) s->free_rerolls_remaining--;
        else if (s->has_bonus_free_reroll) s->has_bonus_free_reroll = 0;
        else s->paid_rerolls++;
        s->reroll_count++;
        for (int a = 0; a < s->n_bk_active; a++) {
            int row = (int)s->bk_active[a];
            BT_CONST_PTR(BtItemEff) effect = &BT_ITEM_EFF_ROWS[row];
            if (!bt_eff_is(effect, BT_ISTR_GAIN_STATS_ON_REROLL)) continue;
            int id = bt_core_id_from_istr(effect->key);
            if (id == 255) continue;
            for (int c = 0, copies = bt_row_owned(s, row); c < copies; c++) {
                int value, chance;
                if (!bt_row_get(s, row, effect, c, &value, &chance)) break;
                if (bt_chance(s, (float)chance * 0.01f)) bt_core_stat_add(s, id, (float)value);
            }
        }
        /* fill_shop_items reroll branch: consume the goldfish and raise tiers. */
        int increase = bt_progress_consume_reroll_tier(s);
        bt_recompute_links(s);
        bt_progress_roll_shop(s, increase);
        bt_wshop_refresh_reroll_price(s);   /* base_shop.gd:465 _on_RerollButton_pressed */
        /* NO buy delay here: the container's 0.05 s timer is started only by
         * on_shop_item_buy_button_pressed / on_shop_item_steal_button_pressed
         * (shop_items_container.gd:57-73,80-94) and base_shop's reroll button
         * never touches it -- the v5 s1 row for tick 306 buys ONE frame after the
         * tick-305 reroll, and the game accepts it. */
        return 0.0f;
    }
    /* WeaponShop: actions 9..14 recycle owned weapon slot (action-9);
     * 15..20 combine owned weapon slot (action-15). Shop-only (phase 1). */
    if (action >= 9 && action <= 20) return bt_wshop_owned_action(s, action);
    /* v4 contract: 21..24 toggle the lock of offer (action-21); 25..28 ban
     * offer (action-25).  Both act on the OFFER slots, not the loadout. */
    if (action >= BT_ACT_LOCK0 && action < BT_ACT_LOCK0 + 4) {
        bt_wshop_toggle_lock(s, action - BT_ACT_LOCK0);
        return 0.0f;
    }
    if (action >= BT_ACT_BAN0 && action < BT_ACT_BAN0 + 4) {
        bt_wshop_ban(s, action - BT_ACT_BAN0);
        return 0.0f;
    }
    if (action < 0 || action > 3 || !(s->shop_available & (1u << action))) return 0.0f;
    int kind = s->shop_kind[action];
    if (kind == BT_SHOP_NONE) return 0.0f;
    unsigned short offer = s->shop_id[action];
    /* shop_items_container.gd:58-65: currency first, then (weapons only)
     * _can_weapon_be_bought; on_shop_item_bought removes the currency after
     * that gate admits the purchase. */
    if (kind == BT_SHOP_WEAPON && !bt_wshop_can_buy(s, offer)) return 0.0f;
    /* the node's STORED price (shop_item.gd:129-140): the buy gate compares the
     * currency against `value`, never against a re-derived current-wave price */
    float price = bt_shop_slot_price(s, action);
    if (bt_wshop_currency(s) < price) return 0.0f;
    bt_wshop_pay(s, price);
    if (kind == BT_SHOP_WEAPON) {
        (void)bt_wshop_buy(s, offer, s->shop_cursed[action] ? 1 : 0);
    } else {
        s->curse_apply = s->shop_cursed[action] ? 1 : 0;
        s->curse_maxmod = 0.0f;
        bt_progress_buy_item(s, offer);     /* mirror duplication + the purchase */
        s->curse_apply = 0;
    }
    s->shop_available &= (unsigned char)~(1u << action);
    /* The bought offer's node is deactivated (its `locked` flag and its lock
     * list entry go with it: shop_item.gd deactivate + base_shop.gd:494-518), so
     * the observation's lock flag for that slot reads 0 from this frame on. */
    bt_wshop_unlock_offer(s, action);
    bt_wshop_clear_offer(s, action);
    /* on_shop_item_bought :530-538: a free-reroll item bought mid-shop raises
     * the current shop's reroll count by the delta; an emptied shop arms the
     * bonus free reroll. */
    int total_free = (int)s->scalar_effect[BT_ISTR_FREE_REROLLS];
    if (total_free > s->initial_free_rerolls) {
        s->free_rerolls_remaining += total_free - s->initial_free_rerolls;
        s->initial_free_rerolls = total_free;
    }
    s->has_bonus_free_reroll = (s->shop_available == 0) ? 1 : 0;
    bt_wshop_refresh_reroll_price(s);   /* base_shop.gd:539 on_shop_item_bought */
    s->shop_timer = s->shop_steps - 1;
    return 0.5f;
}

/* --- wave-end window timing (main.tscn timers) ---------------------------
 *   EndWaveTimer     wait_time 2.0   (main.tscn:869-871) -> 120 ticks
 *   HarvestingTimer  wait_time 0.66  (main.tscn:873-875) -> 40 ticks
 *   WaveClearedLabel/Timer wait_time 0.1 (main.tscn:361-363) -> 6 ticks per
 *   character, "WAVE_COMPLETED" = 14 characters (one sound draw each). */
#define BT_END_WAVE_TICKS 120
#define BT_HARVEST_TICKS 40
#define BT_LABEL_TICKS 6
#define BT_LABEL_CHARS 14

/* main.gd:1464-1492 _on_HarvestingTimer_timeout (the 0.66 s callback arm'ed by
 * manage_harvesting when harvesting_stat > 0): the stat GROWS by
 * ceil(stat * harvesting_growth%) -- or past the last wave SHRINKS by
 * ceil(stat * ENDLESS_HARVESTING_DECREASE%) (run_data.gd:18 = 20).  The percent
 * is evaluated in binary64 like the source (`harvesting_growth / 100.0`), which
 * changes the ceil at the float boundary. */
BT_HD static void bt_progress_harvest_timer(BT_DEVICE BtState* s) {
    if (s->harvest <= 0.0f) return;
    if (s->wave > BT_WAVES) {
        BtRngF64 pct = bt_rf64_div_ff((float)BT_ENDLESS_HARVESTING_DECREASE, 100.0f);
        float q = bt_rf64_to_float(bt_rf64_mul(bt_rf64_from_float(s->harvest), pct));
        float dec = bt_ceilf(q);
        if (dec > 0.0f) bt_core_stat_add(s, 12, -dec);
    } else {
        float growth = 5.0f + s->scalar_effect[BT_ISTR_HARVESTING_GROWTH];
        BtRngF64 pct = bt_rf64_div_ff(growth, 100.0f);
        float q = bt_rf64_to_float(bt_rf64_mul(bt_rf64_from_float(s->harvest), pct));
        float increment = bt_ceilf(q);
        if (increment > 0.0f) bt_core_stat_add(s, 12, increment);
    }
}

BT_HD static void bt_progress_end_wave(BT_DEVICE BtState* s) {
    /* The wave's end-of-wave upgrade UI runs in the wave's OWN main.tscn, which
     * is rebuilt every wave (base_shop.gd:346), so its _old_upgrades list starts
     * empty: the first level-up of this session may offer any upgrade. */
    for (int i = 0; i < 4; i++) s->lvl_prev[i] = 0;   /* row+1 encoding: 0 = no offer */
    for (int a = 0; a < s->n_bk_active; a++) {
        int row = (int)s->bk_active[a];
        BT_CONST_PTR(BtItemEff) effect = &BT_ITEM_EFF_ROWS[row];
        if (effect->custom_key != BT_ISTR_STATS_END_OF_WAVE) continue;
        float gain = bt_row_sum_v(s, row, effect);
        int id = bt_core_id_from_istr(effect->key);
        if (id != 255) bt_core_stat_add(s, id, gain);
        else s->scalar_effect[effect->key] += gain;
    }
    /* main.gd:1225 Utils.convert_stats(convert_stats_end_of_wave, player) runs
     * after the stats_end_of_wave tracked-value loop and before
     * manage_harvesting: Demon converts 50% of its gold into max HP in chunks
     * of 13 (bt_character_convert_end_of_wave; a no-op for Bull/Knight). */
    bt_character_convert_end_of_wave(s);
    /* main.gd:1246-1284 manage_harvesting (wave end, after end_of_the_wave and
     * before clean_up_room) plus the 0.66 s HarvestingTimer callback
     * (main.gd:1464-1492).  The per-player total is
     *   val = harvesting_stat + pacifist_bonus + cryptid_bonus
     *       + _elite_killed_bonus + living_enemy_bonus + charmed_enemy_bonus
     * (pacifist/cryptid/materials_per_living_enemy are character talents of
     * other characters and _elite_killed_bonus is never assigned in the
     * recovered build, so every term but the last is 0 here) and the WHOLE total
     * is granted with RunData.add_gold AND RunData.add_xp when >= 0, or removed
     * as gold when < 0 -- both take an `int` parameter, so the float total is
     * truncated toward zero once for each.  Gold and XP are 1:1, exactly like a
     * gold pickup (there is no "harvesting is gold-only" special case). */
    float charmed_bonus = bt_enemy_lifecycle_charmed_bonus(s, 0);
    float harvest_val = s->harvest + charmed_bonus;
    if (harvest_val >= 0.0f) {
        s->materials += (float)(int)harvest_val;   /* RunData.add_gold(value: int) */
        bt_progress_add_xp(s, harvest_val);        /* RunData.add_xp(value: int) */
    } else {
        /* RunData.remove_gold(abs(val) as int) -- the abs is truncated too. */
        s->materials = bt_maxf(0.0f, s->materials - (float)(int)bt_fabsf(harvest_val));
    }
    /* main.gd:1277-1278 `if harvesting_stat > 0: _harvesting_timer.start()`: the
     * growth/shrink runs 0.66 s (40 ticks) LATER, inside the window. */
    if (s->harvest > 0.0f) s->harvest_timer = BT_HARVEST_TICKS;
    /* main.gd:1232 clean_up_room(): runs after manage_harvesting and before
     * TempStats.reset().  Order inside it: _end_wave_timer.start() (the
     * shop/level-up transition happens at ITS timeout, not here), then
     * RunData.init_elites_spawn(current_wave + 10, 0.0) for
     * current_wave % 10 == 0 >= 10 (main.gd:931-933; bt_schedule_elites_endless
     * owns that second schedule and its draws), then the end_wave sound pick,
     * then the gold-bag / consumable attraction (main.gd:963-1010) and the
     * entity teardown (_entity_spawner.clean_up_room / _wave_manager.clean_up_room
     * / player.on_room_cleanup), then _wave_cleared_label.start(). */
    if (s->wave % 10 == 0 && s->wave >= 10)
        bt_schedule_elites_endless(s, s->wave + 10, 0.0f);
    bt_combat_audio_end_wave(s);        /* main.gd:958 end_wave_sounds pick */
    /* _entity_spawner.clean_up_room (entity_spawner.gd:580-608) + the
     * wave_manager clean-up: stop the spawner, drop every queue and kill every
     * live enemy/neutral with cleaning_up = true -- no loot, no reward, no kill
     * credit -- while the killed NODES stay observable for their death animation,
     * which is what the game shows (living 0 / enemy_nodes 5 for ~64 ticks).  The
     * room is empty of spawns from this frame on. */
    s->n_pend_enemy = 0; s->n_pend_tree = 0; s->n_pend_boss = 0; s->n_pend_summons = 0;
    s->n_births = 0;
    s->cur_spawn_delay = 0;
    bt_enemy_clean_up_room(s);
    s->epend = -1;
    bt_clear_wave_summons(s);
    /* main.gd:963-1010: arm the wave-end attraction (gold -> the GoldBag,
     * consumables -> the player).  The items fly and are resolved frame by frame
     * inside the 2 s window (bt_loot_wave_end_step), so the consumable pickup
     * draws interleave with the WaveClearedLabel draws exactly as in the source;
     * anything still unclaimed when the room is torn down is lost. */
    bt_loot_wave_end_begin(s);
    /* main.gd:1234 TempStats.reset: temporary combat gains do not leak into
     * another wave or count as permanent item/upgrade stats. */
    for (int i = 0; i < s->n_tsi; i++) {
        s->tsi_stacks[i] = 0.0f;
        s->tsi_timer[i] = s->tsi_interval[i];
    }
    /* main.gd:1234 TempStats.reset: temporary combat gains do not leak into
     * another wave or count as permanent item/upgrade stats.  The
     * enemy_percent_damage_taken debuff records are per-life too: the wave's
     * enemies (and their effect-behavior nodes) are gone when main.tscn is
     * rebuilt, so their pending debuffs go with them.  The run-level `overflow`
     * diagnostic is deliberately left sticky. */
    for (int i = 0; i < BT_MAX; i++) bt_enemy_hit_clear_enemy(&s->ehitfx, i);
    for (int i = 0; i < s->n_dt; i++) s->dt_remain[i] = 0.0f;
    bt_core_stat_reset_temp(s);
    /* TempStats.reset() also clears the movement flags and the stats_next_wave
     * temp scalars (TemporalStatEvents); must pair with the wipe above. */
    bt_temporal_reset(s);
    bt_recompute_links(s);
    /* _wave_cleared_label.start(): text "WAVE_COMPLETED" (14 characters), one
     * character revealed every 0.1 s with one sound draw each
     * (wave_cleared_label.gd:10-25). */
    s->label_chars = BT_LABEL_CHARS;
    s->label_timer = BT_LABEL_TICKS;
    /* The 2.0 s EndWaveTimer (main.tscn:869-871): RunData.on_wave_end() and the
     * level-up/shop transition run at its timeout, so obs[7] phase stays 0 for
     * 120 more ticks (measured: game trace WaveTimer 0 at tick 420, Shop 540). */
    s->time_left = 0.0f;
    s->end_wave_active = 1;
    s->end_wave_timer = BT_END_WAVE_TICKS;
}

/* main.gd:1075-1133 _on_EndWaveTimer_timeout: RunData.on_wave_end() +
 * LinkedStats.reset(), then for a live run the item boxes / upgrade choices
 * (the upgrades UI, phase 2), else the shop scene (phase 1). */
BT_HD static void bt_progress_wave_end_finish(BT_DEVICE BtState* s) {
    s->end_wave_active = 0;
    s->end_wave_timer = 0;
    /* The room is torn down with the scene change: items that never reached their
     * attractor inside the 2 s window are freed and LOST (no bank, no pickup). */
    bt_loot_wave_end_drop_remaining(s);
    /* A fresh main.tscn per wave resets the level-up container's own reroll
     * counter (upgrades_ui_player_container.gd:16,57-75). */
    s->lvl_reroll_count = 0;
    s->lvl_reroll_price = 0;
    /* Item boxes (main.gd:1118-1131) are accepted BEFORE the upgrade choices;
     * the first box's crate item is held as the phase-2 offer and the rest
     * follow on take/recycle (upgrades_ui.gd:88-115). */
    bt_progress_next_crate_offer(s);
    if (s->crate_kind != BT_SHOP_NONE) s->phase = 2;
    else if (s->pending_upgrades > 0) { s->phase = 2; bt_progress_roll_upgrade(s); }
    else bt_progress_open_shop(s);
}

/* One frame of the wave-end window: the 2.0 s EndWaveTimer, the 0.66 s
 * HarvestingTimer and the WaveClearedLabel 0.1 s Timer.  Called from
 * bt_core_step while s->end_wave_active, and only from there.
 *
 * It does NOT tick the killed units' death animation: bt_core_step's enemy
 * block (the same span that caches a live slot's velocity, brotato_core.h
 * bt_enemy_dying_step call) already runs on every tick of the window -- the
 * wave-end window leaves the block's `ealive` loop body empty, not the block.
 * Ticking the dying nodes here as well ran bt_enemy_dying_step TWICE per window
 * tick, which halved every window death (a killed node stayed observable 30
 * ticks instead of its true window) and doubled the death-knockback drift.
 * Exactly one call per frame, like the burning tick's single rank). */
BT_HD static void bt_progress_wave_end_step(BT_DEVICE BtState* s) {
    if (s->harvest_timer > 0 && --s->harvest_timer == 0) bt_progress_harvest_timer(s);
    if (s->label_chars > 0 && --s->label_timer <= 0) {
        /* wave_cleared_label.gd:22-25 _on_Timer_timeout: Utils.get_rand_element(
         * character_appearing_sounds) (2 entries) -> one global draw.  The label's
         * Timer is a child of UI, which precedes the Consumables container in the
         * scene tree, so its draw is taken before this frame's item pickups. */
        (void)bt_rng_bounded(&s->rng, 2u);
        s->label_chars--;
        if (s->label_chars > 0) s->label_timer = BT_LABEL_TICKS;
    }
    bt_loot_wave_end_step(s);           /* item.gd flight + pickup/bank on arrival */
    if (--s->end_wave_timer <= 0) bt_progress_wave_end_finish(s);
}
