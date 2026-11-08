/* Brotato weapon / shop / profile observation block - shared core.
 *
 * OWNERSHIP / CONTRACT
 *   This header owns the ENCODING of the 82 floats that extend bt_core_observe
 *   from 79 to 161 (shared contract: brotato-weapons-contract.md).  The first 79
 *   features keep their existing order and meaning; bt_core_observe
 *   (brotato_core.h, parent-owned) appends this block with one call to
 *   bt_weapon_observe_append(s, o, b) at the end.
 *
 *   [79 .. 126]  6 weapon records x 8 fields    BT_OBS_WEAPONS   = 48
 *   [127.. 138]  4 shop offers  x 3 fields      BT_OBS_SHOP_EXT  = 12
 *   [139.. 140]  2 profile fields               BT_OBS_PROFILE   =  2
 *   [141.. 160]  actionable shop state           BT_OBS_SHOP_ACTION = 20
 *                                        BT_OBS_WEAPON_SIZE    = 82
 *
 * NORMALIZATION (source-driven; every float is a real simulated value or the
 * documented empty encoding - never a pad, never an invented default):
 *
 *   weapon slot i, base 79 + 8*i, fields in order:
 *     +0 id     empty -> 0.0; else (BT_WEAPONS index + 1)/(BT_NWEAPONS + 1).
 *               The no-weapon sentinel is BT_WEAPON_NONE (0xFFFF); weapon table
 *               index 0 is a REAL weapon and emits 1/(BT_NWEAPONS+1) != 0.0, so
 *               0.0 unambiguously means "empty slot".
 *     +1 tier   empty -> 0.0; else wpn_tier[i] / 4.0   (the owned copy's
 *               WeaponData.tier, ItemParentData.Tier 0 COMMON .. 5 DANGER_5 -
 *               the same raw domain and divisor the unchanged item block
 *               already uses: it->tier / 4.0f)
 *     +2 kind   empty -> 0.0; else (float)BtWeaponDef.kind (BT_WEAPON_KIND_MELEE
 *               0 / BT_WEAPON_KIND_RANGED 1 == WeaponData.Type).  Meaningful only
 *               when id != 0.0.
 *     +3 cd     empty -> 0.0; else clamp(cooldown_left / cooldown, 0, 1).  Both
 *               are WeaponStats TICKS (weapon_stats.gd "60 ticks per second",
 *               Weapon._current_cooldown); 0 = ready to fire, 1 = just fired.
 *               A non-positive period emits 0.0 (a zero-cooldown weapon is
 *               always ready).  In the SHOP phase (phase == 1) it is always
 *               0.0: the shop scene has no live Weapon node, so there is no
 *               attack clock to read; the stored counter is never reset.
 *     +4 damage empty -> 0.0; else current_stats.damage * 0.1   (the core's
 *               existing damage convention: edmg / projectile damage * 0.1)
 *     +5 range  empty -> 0.0; else current_stats.max_range / BT_ARENA_W
 *               (pixels -> arena widths, the same scale as obs[4]/[35..])
 *     +6 crit   empty -> 0.0; else current_stats.crit_chance  (WeaponStats keeps
 *               it as a 0..1 fraction already; no divisor)
 *     +7 cursed empty -> 0.0; else Weapon.is_cursed ? 1.0 : 0.0
 *   An empty slot emits eight 0.0.
 *
 *   shop offer i, base 127 + 3*i:
 *     +0 type   0.0 BT_SHOP_NONE (empty / unavailable), 1.0 BT_SHOP_ITEM,
 *               2.0 BT_SHOP_WEAPON
 *     +1 id     empty -> 0.0; ITEM -> (BT_ITEMS index + 1)/(BT_NITEMS + 1);
 *               WEAPON -> (BT_WEAPONS index + 1)/(BT_NWEAPONS + 1)
 *     +2 cursed 1.0 when the offer is cursed (shop_cursed[i]), else 0.0
 *   The unchanged item block obs[11..26] stays items-only: WeaponShop keeps
 *   shop[i] = 0 for a weapon or empty offer, so that block keeps emitting the
 *   core's documented empty encoding and carries no fabricated item.
 *
 *   profile:
 *     [139] character  (character + 1) / (BT_NCHARACTERS + 1)   (BT_CHARACTERS
 *                      index; Bull = 0 -> 1/(N+1))
 *     [140] slot limit weapon_slot_limit / BT_CHARACTER_SLOT_DEFAULT  (source
 *                      weapon_slot effect; player_run_data.gd:434 default 6,
 *                      Bull's weapon_slot effect value 0 -> 0.0)
 *
 * Every read is live BtState, so CPU, CUDA and Metal produce identical floats.
 */
#pragma once

#include "brotato_state.h"          /* BtState, BT_ARENA_W, BT_NITEMS, BT_CONST_PTR, BT_HD */
#include "brotato_weapon_types.h"   /* BT_WEAPON_SLOTS, BT_WEAPON_NONE */
#include "brotato_weapons.h"        /* BtWeaponDef, BT_WEAPONS, BT_NWEAPONS */
#include "brotato_weapon_stats.h"   /* BtPlayerWeaponStats, bt_weapon_stats */
#include "brotato_weapon_shop.h"    /* BT_SHOP_NONE / BT_SHOP_ITEM / BT_SHOP_WEAPON */
#include "brotato_characters.h"     /* BT_NCHARACTERS */

/* --- size contract (brotato_state.h folds BT_OBS_WEAPON_SIZE into BT_OBS_SIZE) --- */
#ifndef BT_OBS_WEAPONS
#define BT_OBS_WEAPONS 48
#endif
#ifndef BT_OBS_SHOP_EXT
#define BT_OBS_SHOP_EXT 12
#endif
#ifndef BT_OBS_PROFILE
#define BT_OBS_PROFILE 2
#endif
/* Actionable shop state appended after the profile block (see below): one
 * currency flag + four offers x (price, eligible, affordable, buyable) + three
 * reroll fields = 20. */
#ifndef BT_OBS_SHOP_ACTION
#define BT_OBS_SHOP_ACTION (1 + 4 * 4 + 3)
#endif
#define BT_OBS_WEAPON_SIZE (BT_OBS_WEAPONS + BT_OBS_SHOP_EXT + BT_OBS_PROFILE + BT_OBS_SHOP_ACTION)

/* Slot count is BT_WEAPON_SLOTS (brotato_weapon_types.h: six weapon slots, dense
 * source order - WeaponInventory owns the wpn_* layout: wpn_id / wpn_tier /
 * wpn_cursed, plus float wpn_cd[] which WeaponStats reads for cooldown_left). */
#ifndef BT_WEAPON_SLOTS
#define BT_WEAPON_SLOTS 6
#endif
#ifndef BT_WEAPON_NONE
#define BT_WEAPON_NONE 0xFFFFu
#endif
/* Source default weapon slot count (player_run_data.gd:434); WeaponCharacters
 * owns BT_CHARACTER_SLOT_DEFAULT in brotato_characters.h. */
#ifndef BT_CHARACTER_SLOT_DEFAULT
#define BT_CHARACTER_SLOT_DEFAULT 6
#endif

#if BT_OBS_WEAPONS != (BT_WEAPON_SLOTS * 8)
#error "brotato_weapon_observe.h: BT_OBS_WEAPONS must be BT_WEAPON_SLOTS * 8"
#endif
#if BT_OBS_SHOP_EXT != 12
#error "brotato_weapon_observe.h: BT_OBS_SHOP_EXT must be 4 shop offers * 3 fields"
#endif
#if BT_OBS_PROFILE != 2
#error "brotato_weapon_observe.h: BT_OBS_PROFILE must be 2 profile fields"
#endif
#if BT_OBS_SHOP_ACTION != 20
#error "brotato_weapon_observe.h: BT_OBS_SHOP_ACTION must be 1 + 4 offers * 4 fields + 3 reroll fields"
#endif

/* Append the 82-float weapon/shop/profile/shop-action block. `b` is the index
 * the caller has written up to (79 when called from bt_core_observe); returns
 * the new index (161). Writes exactly BT_OBS_WEAPON_SIZE floats. */
BT_HD int bt_weapon_observe_append(BT_DEVICE BtState* s, BT_DEVICE float* o, int b) {
    /* --- 6 weapon records -------------------------------------------------- */
    for (int slot = 0; slot < BT_WEAPON_SLOTS; slot++) {
        unsigned short id = s->wpn_id[slot];
        if (id == BT_WEAPON_NONE) {
            o[b++] = 0.0f; o[b++] = 0.0f; o[b++] = 0.0f; o[b++] = 0.0f;
            o[b++] = 0.0f; o[b++] = 0.0f; o[b++] = 0.0f; o[b++] = 0.0f;
            continue;
        }
        BT_CONST_PTR(BtWeaponDef) def = &BT_WEAPONS[id];
        BtPlayerWeaponStats st;
        (void)bt_weapon_stats(s, slot, &st);

        float cd = st.cooldown > 0 ? st.cooldown_left / (float)st.cooldown : 0.0f;
        if (cd < 0.0f) cd = 0.0f;
        else if (cd > 1.0f) cd = 1.0f;
        /* The shop scene has no live Weapon node and therefore no attack clock
         * to tick Weapon._current_cooldown, so every weapon is reported ready
         * (0.0) in phase 1.  The counter itself is NOT reset. */
        if (s->phase == 1) cd = 0.0f;

        o[b++] = (float)(id + 1) / (float)(BT_NWEAPONS + 1);
        o[b++] = (float)s->wpn_tier[slot] * 0.25f;   /* /4 */
        o[b++] = (float)def->kind;
        o[b++] = cd;
        o[b++] = (float)st.damage * 0.1f;
        o[b++] = (float)st.max_range / BT_ARENA_W;
        o[b++] = st.crit_chance;
        o[b++] = s->wpn_cursed[slot] ? 1.0f : 0.0f;
    }

    /* --- 4 typed shop offers ---------------------------------------------- */
    for (int i = 0; i < 4; i++) {
        unsigned char kind = s->shop_kind[i];
        o[b++] = (float)kind;
        if (kind == BT_SHOP_ITEM) {
            o[b++] = (float)(s->shop_id[i] + 1) / (float)(BT_NITEMS + 1);
        } else if (kind == BT_SHOP_WEAPON) {
            o[b++] = (float)(s->shop_id[i] + 1) / (float)(BT_NWEAPONS + 1);
        } else {
            o[b++] = 0.0f;
        }
        o[b++] = s->shop_cursed[i] ? 1.0f : 0.0f;
    }

    /* --- 2 profile fields -------------------------------------------------- */
    o[b++] = (float)(s->character + 1) / (float)(BT_NCHARACTERS + 1);
    o[b++] = (float)s->weapon_slot_limit / (float)BT_CHARACTER_SLOT_DEFAULT;

    /* --- actionable shop state (BT_OBS_SHOP_ACTION, base 141) --------------
     * Exact per-offer economics, so a policy can price every purchase without
     * re-deriving the shop rules.  Everything is the live BtState read through
     * the owner's own helpers, so CPU/CUDA/Metal emit identical floats.
     *
     * [141] currency_is_hp: 1.0 when the run's shop currency is stat_max_hp
     *       (Demon hp_shop, run_data.gd:1429-1437 get_player_currency), else
     *       0.0 for gold.  It describes the four offer prices below.
     * offer i base 142 + 4*i:
     *   +0 price      exact price in the run's shop currency units (bt_wshop_
     *                 price_units: item bt_shop_price / weapon bt_weapon_price,
     *                 ceil(/20) under hp_shop; shop_item.gd:138). 0.0 when the
     *                 offer is absent (kind NONE).
     *   +1 eligible   1.0 when the offer passes its NON-currency gate:
     *                 weapon -> bt_wshop_can_buy (shop_items_container.gd:97-135,
     *                 tier/type/no-duplicate/slot/auto-combine); item -> 1.0
     *                 when present (items gate on currency only, :56-65).
     *   +2 affordable 1.0 when bt_wshop_currency(s) >= price (max_hp under
     *                 hp_shop, else materials); 0.0 otherwise or absent.
     *   +3 buyable    1.0 when eligible, affordable, and the shop action timer
     *                 permits a purchase; 0.0 otherwise.
     * reroll (base 158), shop-only (0.0 in phases 0/2 - the empty encoding):
     *   +0 price      exact gold price; 0.0 while a free reroll is pending.
     *   +1 free       1.0 when free_rerolls_remaining > 0 or the bonus-emptied
     *                 -shop reroll is pending (base_shop.gd:479-483).
     *   +2 available  1.0 when the reroll can be pressed (free or materials >=
     *                 price, base_shop.gd:400-410). */
    o[b++] = s->phase == 1 && bt_character_hp_shop(s->character) ? 1.0f : 0.0f;
    {
        float currency = s->phase == 1 ? bt_wshop_currency(s) : 0.0f;
        for (int i = 0; i < 4; i++) {
            unsigned char kind = s->phase == 1 ? s->shop_kind[i] : BT_SHOP_NONE;
            unsigned short id = s->shop_id[i];
            int present = 0, eligible = 0;
            float price = 0.0f;
            if (kind == BT_SHOP_ITEM && (int)id < BT_NITEMS) {
                present = 1; eligible = 1;
                price = bt_shop_slot_price(s, i);
            } else if (kind == BT_SHOP_WEAPON && bt_weapon_valid(id)) {
                present = 1;
                eligible = bt_wshop_can_buy(s, id);
                price = bt_shop_slot_price(s, i);
            }
            int affordable = present && currency >= price;
            int buyable = present && eligible && affordable && s->shop_timer <= 0;
            o[b++] = present ? price : 0.0f;
            o[b++] = eligible ? 1.0f : 0.0f;
            o[b++] = affordable ? 1.0f : 0.0f;
            o[b++] = buyable ? 1.0f : 0.0f;
        }
    }
    {
        /* Reroll is ALWAYS paid in gold, even for Demon: base_shop.gd:406-410
         * tests/removes get_player_gold, never get_player_currency.  The price
         * mirrors bt_progress_shop_action's action 4 exactly. */
        float rr_price = 0.0f;
        int rr_free = 0, rr_avail = 0;
        if (s->phase == 1) {
            rr_free = (s->free_rerolls_remaining > 0 || s->has_bonus_free_reroll) ? 1 : 0;
            /* The button's STORED price (base_shop.gd:479-481/132), not a live
             * recomputation: it is refreshed at the shop open, after a reroll, a
             * purchase and a ban, all before GO's current_wave bump, so the
             * trailing shop frame keeps the pre-bump value. */
            rr_price = rr_free ? 0.0f : (float)s->shop_reroll_price;
            /* No buy-delay term: the container's 0.05 s timer gates BUYS/STEALS
             * only (shop_items_container.gd:57-73,80-94), so the reroll button
             * stays enabled while it runs (v5 s1 row 306: reroll available 1 one
             * frame after a purchase). */
            rr_avail = (rr_free || s->materials >= rr_price) ? 1 : 0;
        }
        o[b++] = rr_price;
        o[b++] = rr_free ? 1.0f : 0.0f;
        o[b++] = rr_avail ? 1.0f : 0.0f;
    }

    return b;
}
