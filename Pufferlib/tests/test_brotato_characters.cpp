/* tests/test_brotato_characters.cpp
 *
 * Source-backed tests for the selectable character profiles and their runtime
 * (ocean/brotato/brotato_characters.h + brotato_character_runtime.h):
 *   items/characters/bull/bull_effect_1..5.tres     +20 max_hp / +15 regen /
 *       +10 armor, gain_stat_hp_regeneration +50%, explode_on_hit,
 *       weapon_slot REPLACE 0
 *   items/characters/knight/knight_effect_1..5,7    melee +2 per armor,
 *       +3 armor, no_ranged_weapons=1, min_weapon_tier=1, gain cuts
 *   items/characters/demon/demon_effect_1..2        hp_shop, 50% of gold ->
 *       max_hp in chunks of 13 at the end of a wave
 *   utils.gd:559-588 / run_data.gd:1428-1439        conversion + currency
 *   linked_stats.gd:12-78                           the Knight link
 *
 * Build and run:
 *     c++ -O2 -o build/test_brotato_characters tests/test_brotato_characters.cpp -lm
 *     ./build/test_brotato_characters
 *
 * Exit code 0 = all checks passed.
 */

#include "../ocean/brotato/brotato_core.h"
#include "../ocean/brotato/brotato_character_runtime.h"

#include <stdio.h>

#define BT_START_WEAPON_NONE ((unsigned short)0xFFFFu)   /* BT_WEAPON_NONE */

static int g_failures = 0;
static int g_checks = 0;

static void check(int cond, const char* msg) {
    g_checks++;
    if (!cond) { g_failures++; printf("  FAIL: %s\n", msg); }
}
static void check_near(float got, float want, float tol, const char* msg) {
    g_checks++;
    if (!(bt_fabsf(got - want) <= tol)) {
        g_failures++;
        printf("  FAIL: %s (got %.6f want %.6f tol %g)\n", msg, got, want, tol);
    }
}

/* BtState embeds two physics worlds; keep the big state static. */
static BtState A;

/* The BT_ITEMS index whose host-only name matches `name` (icon-name form, no
 * `item_` prefix), or -1. */
static int item_by_name(const char* name) {
    for (int i = 0; i < BT_NITEMS; i++) {
        const char* n = BT_ITEMS[i].name;
        int k = 0;
        while (n[k] != 0 && n[k] == name[k]) k++;
        if (n[k] == 0 && name[k] == 0) return i;
    }
    return -1;
}

/* The first item carrying interned tag `tag`. */
static int item_with_tag(unsigned short tag) {
    for (int i = 0; i < BT_NITEMS; i++) {
        BT_CONST_PTR(BtItem) it = &BT_ITEMS[i];
        for (int t = 0; t < (int)it->tag_count; t++)
            if (BT_ITEM_TAGS[it->tag_first + t] == tag) return i;
    }
    return -1;
}

static void init(unsigned short character) {
    bt_core_init(&A, 5000u + character, 0, 1, character, BT_START_WEAPON_NONE);
}

static void init_weapon(unsigned short character, unsigned short starting_weapon) {
    bt_core_init(&A, 5100u + character, 0, 1, character, starting_weapon);
}

/* ---------------------------------------------------------------------------
 * Scripted Demon episode (driver). Drives several real end-of-wave -> shop
 * cycles through the core paths: bt_progress_end_wave (materials -> Max HP
 * conversion + shop fill), bt_progress_shop_action (buy / reroll / leave).
 * Prints every step so the Demon mechanics are visible, and asserts the
 * invariants it exercises.
 * ------------------------------------------------------------------------- */
static void demon_episode(void) {
    printf("[14] scripted Demon episode: %d shops (driver output)\n", 6);
    BtState* s = &A;
    unsigned short gs = bt_weapon_id_from_name("weapon_ghost_scepter_1");
    bt_core_init(s, 0xD3E3u, 0, 1, BT_CHARACTER_DEMON, gs);
    check(s->character == BT_CHARACTER_DEMON && s->weapon_slot_limit == 6,
          "driver: demon selected with 6 weapon slots");
    check(s->wpn_count == 1 && s->wpn_id[0] == gs, "driver: ghost scepter starts equipped");
    printf("  start: demon, ghost scepter, slots=%u max_hp=%.0f hp=%.0f gold=%.0f currency=%.0f HP\n",
           s->weapon_slot_limit, s->max_hp, s->hp, s->materials, bt_wshop_currency(s));

    for (int w = 0; w < 6; w++) {
        s->materials += 30.0f * (float)s->wave;      /* the wave's gold income */
        float m0 = s->materials, hp0 = s->max_hp;
        bt_progress_end_wave(s);
        /* the shop/level-up transition fires at the 2.0 s EndWaveTimer; each
         * window frame also runs the killed nodes' death animation, which
         * bt_core_step ticks in its enemy block (once per frame). */
        for (int t = 0; t < BT_END_WAVE_TICKS; t++) {
            bt_enemy_dying_step(s);
            bt_progress_wave_end_step(s);
        }
        /* The conversion is the driver's own output; the LEVEL-UP choices that
         * follow can add max_hp too (an offered stat_max_hp upgrade), and which
         * offers appear depends on the run's RNG.  So measure the conversion on
         * its own: `hp_conv` is taken right after the wave-end window and before
         * the end-of-wave UI drain. */
        float hp_conv = s->max_hp;
        /* drain the end-of-wave UI: an open crate offer (upgrades_ui.gd:88-115)
         * is taken first, otherwise a level-up choice */
        while (s->phase == 2) {
            if (s->crate_kind != BT_SHOP_NONE) bt_progress_choose_upgrade(s, BT_ACT_CRATE_TAKE);
            else bt_progress_choose_upgrade(s, 0);
        }
        check(s->phase == 1, "driver: the wave end opened the shop");
        int removed = (int)(m0 - s->materials);
        int converted = (int)(hp_conv - hp0);
        int added = (int)(s->max_hp - hp0);
        int chunks = (int)bt_floorf((m0 * 0.5f) / 13.0f);
        printf("  wave %d end: gold %.0f -> %.0f (removed %d), max_hp %.0f -> %.0f (+%d conv, +%d with level-ups)\n",
               s->wave, m0, s->materials, removed, hp0, s->max_hp, converted, added);
        check(removed >= 0, "driver: conversion never adds gold");
        /* Coarse invariant: the driver converts 50% of the gold in 13-gold
         * chunks.  Which gold the conversion sees is the wave-end balance, and
         * the end-of-wave sequence can move materials before it (an item's
         * gain_stat_end_of_wave, the spend of the shops that preceded) -- so a
         * balance 26 gold higher crosses the next chunk boundary and yields one
         * more chunk.  The exact arithmetic (floor over the conversion-time
         * source, the gain scaling) is pinned by section [13]. */
        check(converted == chunks || converted == chunks + 1,
              "driver: 50% of gold, in 13-chunks, as Max HP");
        /* The conversion itself removes exactly 13 per chunk; the delta measured
         * here can also carry another wave-end gold movement (an item's
         * gain_stat_end_of_wave on materials), so allow one chunk of slack -- the
         * exact conversion math is pinned by section [13]. */
        check(removed <= 13 * added + 13,
              "driver: conversion removes at most 13 gold per Max HP (+ wave-end item effects)");

        for (int i = 0; i < 4; i++) {
            if (s->phase != 1) break;
            if (!(s->shop_available & (1u << i))) continue;
            int kind = s->shop_kind[i];
            if (kind == BT_SHOP_NONE) continue;
            unsigned short id = s->shop_id[i];
            const char* name = (kind == BT_SHOP_WEAPON) ? BT_WEAPONS[id].my_id
                                                        : BT_ITEMS[id].name;
            float price = bt_wshop_price_units(s, kind, id);
            float cur = bt_wshop_currency(s);
            unsigned char avail0 = s->shop_available;
            float r = bt_progress_shop_action(s, i);
            int bought = (avail0 & (1u << i)) && !(s->shop_available & (1u << i));
            printf("    offer[%d] %-6s %-26s price=%4.0f HP  currency=%4.0f HP  %s -> max_hp %.0f\n",
                   i, (kind == BT_SHOP_WEAPON) ? "weapon" : "item", name, price, cur,
                   bought ? "bought" : "skipped", s->max_hp);
            if (bought) check(r > 0.0f, "driver: a purchase awards the positive shop reward");
        }
        if (s->free_rerolls_remaining == 0 && !s->has_bonus_free_reroll) {
            float gm = s->materials, gh = s->max_hp;
            bt_progress_shop_action(s, 4);
            printf("    reroll: gold %.0f -> %.0f, max_hp stays %.0f (rerolls cost gold)\n",
                   gm, s->materials, s->max_hp);
            check(s->materials < gm && s->max_hp == gh, "driver: a reroll is paid in gold");
        }
        bt_progress_shop_action(s, 8);               /* leave the shop -> next wave */
        /* base_shop.gd:339-349 ends in change_scene(), applied at the frame's end:
         * the driver (which never runs a step here) must drive the deferred
         * arena half explicitly, exactly as the core's next tick does. */
        bt_progress_shop_go(s);
        printf("  -> wave %d: max_hp=%.0f hp=%.0f (hp_start_wave) gold=%.0f currency=%.0f HP\n",
               s->wave, s->max_hp, s->hp, s->materials, bt_wshop_currency(s));
    }
}

int main(void) {
    printf("[1] default Bull: zero slots, source base stats, regen gain\n");
    {
        init(BT_CHARACTER_BULL);
        check(A.character == BT_CHARACTER_BULL, "default character is Bull (0)");
        check(A.weapon_slot_limit == 0, "Bull weapon_slot_limit = 0 (bull_effect_5)");
        check(bt_character_weapon_slots(BT_CHARACTER_BULL) == 0, "Bull accessor slots = 0");
        check_near(A.stat_raw[0], 30.0f, 0.0f, "raw max_hp = 10 base + Bull 20");
        check_near(A.max_hp, 30.0f, 0.0f, "observed max_hp 30");
        check_near(A.stat_raw[1], 15.0f, 0.0f, "raw regen = Bull 15");
        check_near(A.stat_gain[1], 1.5f, 1e-6f, "regen gain = 1 + 50/100");
        check_near(bt_core_stat_read(&A, 1), 22.5f, 0.0f, "regen effective 15 * 1.5");
        check_near(A.armor, 10.0f, 0.0f, "armor = Bull 10");
        check(bt_character_no_ranged(BT_CHARACTER_BULL) == 0, "Bull has no ranged ban");
        check(bt_character_min_weapon_tier(BT_CHARACTER_BULL) == 0, "Bull min tier 0");
        check(bt_character_max_weapon_tier(BT_CHARACTER_BULL) == 99, "max tier 99");
        check(bt_character_hp_shop(BT_CHARACTER_BULL) == 0, "Bull pays gold");
        check_near(bt_character_currency(&A), A.materials, 0.0f, "Bull currency is gold");
        check(bt_character_explode_on_hit(BT_CHARACTER_BULL) == 1, "Bull explodes on hit");
        check_near(bt_character_explode_chance(BT_CHARACTER_BULL), 1.0f, 0.0f,
                   "Bull explosion chance 100%");
        check(bt_character_explode_on_hit(BT_CHARACTER_KNIGHT) == 0,
              "Knight has no explode_on_hit");
        check(BT_CHARACTERS[BT_CHARACTER_BULL].explode_damage == 30,
              "Bull explosion damage 30");
        check(BT_CHARACTERS[BT_CHARACTER_BULL].explode_range == 150,
              "Bull explosion range 150");
        check(BT_CHARACTERS[BT_CHARACTER_BULL].explode_crit_chance > 0.0299f &&
              BT_CHARACTERS[BT_CHARACTER_BULL].explode_crit_chance < 0.0301f,
              "Bull explosion crit 3%");
    }

    printf("[2] Knight: armor, melee-per-armor link, restrictions\n");
    {
        init(BT_CHARACTER_KNIGHT);
        check(A.character == BT_CHARACTER_KNIGHT, "character stored");
        check(A.weapon_slot_limit == 6, "Knight slots = default 6");
        check_near(A.armor, 3.0f, 0.0f, "Knight armor = 3");
        check(bt_character_no_ranged(BT_CHARACTER_KNIGHT) == 1, "no_ranged_weapons = 1");
        check(bt_character_no_melee(BT_CHARACTER_KNIGHT) == 0, "melee weapons allowed");
        check(bt_character_min_weapon_tier(BT_CHARACTER_KNIGHT) == 1, "min_weapon_tier = 1");
        check_near(A.stat_gain[15], 0.5f, 1e-6f, "attack_speed gain cut to -50%");
        check_near(A.stat_gain[12], 0.2f, 1e-6f, "harvesting gain cut to -80%");
        check_near(bt_core_stat_read(&A, 12), 0.0f, 0.0f, "no harvesting source yet");

        /* linked_stats.gd:12-78: melee += int(2 * armor / 1), non-perm-only. */
        bt_recompute_links(&A);
        check_near(bt_core_stat_read(&A, 6), 6.0f, 0.0f, "melee = 2 * armor(3)");
        bt_core_stat_add(&A, 2, 7.0f);        /* armor 10 */
        bt_recompute_links(&A);
        check_near(A.armor, 10.0f, 0.0f, "armor 3 + 7");
        check_near(bt_core_stat_read(&A, 6), 20.0f, 0.0f, "melee follows armor (10 -> 20)");

        /* temp armor counts: knight_effect_1 perm_stats_only = false */
        bt_core_stat_add_temp(&A, 2, 5.0f);   /* TempStats armor +5 */
        bt_recompute_links(&A);
        check_near(bt_core_stat_read(&A, 6), 30.0f, 0.0f,
                   "non-perm-only link includes temp armor (15 -> 30)");
    }

    printf("[3] Demon: hp_shop currency and end-of-wave conversion\n");
    {
        init(BT_CHARACTER_DEMON);
        check(A.character == BT_CHARACTER_DEMON, "character stored");
        check(A.weapon_slot_limit == 6, "Demon slots = 6");
        check(bt_character_hp_shop(BT_CHARACTER_DEMON) == 1, "hp_shop = 1");
        check_near(A.max_hp, 10.0f, 0.0f, "Demon has no base stat bonus (10 hp)");
        check_near(bt_character_currency(&A), 10.0f, 0.0f,
                   "Demon currency is stat_max_hp, not gold");

        bt_character_currency_remove(&A, 4.0f);
        check_near(A.max_hp, 6.0f, 0.0f, "paying under hp_shop lowers max_hp");
        bt_core_stat_add(&A, 0, 2.0f);
        check_near(A.max_hp, 8.0f, 0.0f, "earning max_hp (level-up/item) raises the currency");
        check(A.materials == BT_START_MATERIALS, "hp_shop currency never touched gold");

        /* utils.gd:559-588: 50% of gold in chunks of 13, each +1 max_hp. */
        A.materials = 100.0f;
        bt_character_convert_end_of_wave(&A);
        check_near(A.materials, 61.0f, 0.0f, "removed floor(100*0.5/13)*13 = 39 gold");
        check_near(A.max_hp, 11.0f, 0.0f, "added 3 max_hp (8 -> 11)");

        A.materials = 20.0f;                  /* floor(10/13) = 0 chunks */
        bt_character_convert_end_of_wave(&A);
        check_near(A.materials, 20.0f, 0.0f, "below one chunk: no gold removed");
        check_near(A.max_hp, 11.0f, 0.0f, "below one chunk: no max_hp added");

        A.materials = 52.0f;                  /* floor(26/13) = exactly two chunks */
        bt_character_convert_end_of_wave(&A);
        check_near(A.materials, 26.0f, 0.0f, "two chunks: 26 gold consumed");
        check_near(A.max_hp, 13.0f, 0.0f, "two chunks: +2 max_hp");
    }

    printf("[4] Bull conversion/currency stay off the Demon path\n");
    {
        init(BT_CHARACTER_BULL);
        A.materials = 100.0f;
        bt_character_convert_end_of_wave(&A);
        check_near(A.materials, 100.0f, 0.0f, "Bull never converts gold");
        check_near(A.max_hp, 30.0f, 0.0f, "Bull max_hp untouched by conversion");
        bt_character_currency_remove(&A, 10.0f);
        check_near(A.materials, 90.0f, 0.0f, "Bull spends gold");
        check_near(A.max_hp, 30.0f, 0.0f, "Bull max_hp unaffected by spending");
    }

    printf("[5] shop eligibility: bans, wanted tags, groups\n");
    {
        int seashell = item_by_name("seashell");
        check(seashell >= 0, "item_seashell is in the pool");
        check(bt_character_item_banned(BT_CHARACTER_KNIGHT, (unsigned short)seashell) == 1,
              "Knight bans item_seashell");
        check(bt_character_item_banned(BT_CHARACTER_BULL, (unsigned short)seashell) == 1,
              "Bull also bans item_seashell");
        int coffee = item_by_name("coffee");
        check(coffee >= 0, "item_coffee is in the pool");
        check(bt_character_item_banned(BT_CHARACTER_BULL, (unsigned short)coffee) == 1,
              "Bull bans item_coffee");
        check(bt_character_item_banned(BT_CHARACTER_DEMON, (unsigned short)coffee) == 0,
              "Demon bans nothing");
        check(bt_character_item_banned(BT_CHARACTER_KNIGHT, (unsigned short)coffee) == 0,
              "Knight only bans item_seashell");

        check((bt_character_ban_group_mask(BT_CHARACTER_BULL) & BT_ITEM_GROUP_LIFESTEAL) != 0,
              "Bull bans the lifesteal group");
        check((bt_character_ban_group_mask(BT_CHARACTER_KNIGHT) & BT_ITEM_GROUP_HARVESTING) != 0 &&
              (bt_character_ban_group_mask(BT_CHARACTER_KNIGHT) & BT_ITEM_GROUP_RANGED_DAMAGE) != 0,
              "Knight bans harvesting + ranged_damage groups");
        check(bt_character_ban_group_mask(BT_CHARACTER_DEMON) == 0, "Demon bans no groups");

        int armor_item = item_with_tag(BT_ISTR_STAT_ARMOR);
        check(armor_item >= 0, "an item carries the stat_armor tag");
        check(bt_character_wants_item(BT_CHARACTER_BULL, (unsigned short)armor_item) == 1,
              "Bull wants stat_armor items");
        check(bt_character_wants_item(BT_CHARACTER_KNIGHT, (unsigned short)armor_item) == 1,
              "Knight wants stat_armor items");
        check(bt_character_wants_item(BT_CHARACTER_DEMON, (unsigned short)armor_item) == 0,
              "Demon wants no tags");
    }

    printf("[6] starting-weapon pools (host-only resolution)\n");
    {
        check(bt_character_start_count(BT_CHARACTER_BULL) == 0, "Bull has no starting weapons");
        check(bt_character_start_count(BT_CHARACTER_KNIGHT) == 13,
              "Knight offers 13 tier-2 melee starting weapons");
        check(bt_character_start_count(BT_CHARACTER_DEMON) == 12,
              "Demon offers 12 tier-1 starting weapons");
        int sword = bt_character_start_index(BT_CHARACTER_KNIGHT, "weapon_sword_2");
        check(sword >= 0, "Knight pool contains weapon_sword_2");
        check(bt_character_start_type(BT_CHARACTER_KNIGHT, sword) == 0,
              "weapon_sword_2 is melee");
        check(bt_character_start_index(BT_CHARACTER_KNIGHT, "weapon_wand_1") == -1,
              "Knight pool excludes the ranged wand");
        int wand = bt_character_start_index(BT_CHARACTER_DEMON, "weapon_wand_1");
        check(wand >= 0, "Demon pool contains weapon_wand_1");
        check(bt_character_start_type(BT_CHARACTER_DEMON, wand) == 1,
              "weapon_wand_1 is ranged");
        check(bt_character_start_index(BT_CHARACTER_BULL, "weapon_sword_2") == -1,
              "Bull has no pool to resolve against");
        check(bt_character_start_name(BT_CHARACTER_KNIGHT, 99) == 0, "out-of-range index is null");
        check(bt_character_start_type(BT_CHARACTER_BULL, 0) == -1, "empty pool yields no type");
    }

    printf("[7] config-token resolution and defaults\n");
    {
        check(BT_NCHARACTERS == 3, "three selectable profiles");
        check(BT_CHARACTER_BULL == 0 && BT_CHARACTER_KNIGHT == 1 && BT_CHARACTER_DEMON == 2,
              "canonical ids Bull 0 / Knight 1 / Demon 2");
        check(bt_character_id_from_name("bull") == BT_CHARACTER_BULL, "token bull");
        check(bt_character_id_from_name("character_bull") == BT_CHARACTER_BULL, "my_id bull");
        check(bt_character_id_from_name("knight") == BT_CHARACTER_KNIGHT, "token knight");
        check(bt_character_id_from_name("character_demon") == BT_CHARACTER_DEMON, "my_id demon");
        check(bt_character_id_from_name(0) == BT_CHARACTER_BULL, "NULL resolves to Bull");
        check(bt_character_id_from_name("ogre") == BT_CHARACTER_BULL,
              "an unsupported character falls back to the default Bull");
        /* A wrong/out-of-range id must behave as Bull, never read past the table. */
        check(bt_character_weapon_slots((unsigned short)99) == 0,
              "out-of-range character clamps to Bull");
        /* run_data.gd:767-768: the next-level XP threshold multiplier is
         * (1 + next_level_xp_needed/100); its only producers are the Baby
         * (+130), Mutant (-66) and Technomage (+75) character effects, so every
         * profile here scales it by exactly 1.0. */
        check(bt_character_next_level_xp_pct(BT_CHARACTER_BULL) == 0 &&
              bt_character_next_level_xp_pct(BT_CHARACTER_KNIGHT) == 0 &&
              bt_character_next_level_xp_pct(BT_CHARACTER_DEMON) == 0,
              "no profile scales the next-level XP threshold");
        check(bt_character_next_level_xp_pct((unsigned short)99) == 0,
              "out-of-range character clamps to Bull's zero pct");
    }

    printf("[8] starting-loadout bootstrap (bt_character_starting_apply)\n");
    {
        /* No selection: Bull/Knight/Demon start weaponless (source default). */
        init(BT_CHARACTER_KNIGHT);
        check(A.wpn_count == 0, "unselected Knight starts with no weapons");
        init(BT_CHARACTER_BULL);
        check(A.wpn_count == 0, "Bull starts with no weapons");

        /* The host resolves a pool weapon name to its canonical BT_WEAPONS id. */
        unsigned short sword = bt_weapon_id_from_name("weapon_sword_2");
        check(sword != BT_START_WEAPON_NONE, "weapon_sword_2 resolves to a weapon id");
        check(bt_character_start_index(BT_CHARACTER_KNIGHT, "weapon_sword_2") >= 0,
              "weapon_sword_2 is in the Knight pool");
        init_weapon(BT_CHARACTER_KNIGHT, sword);
        check(A.wpn_count == 1, "the Knight's selected weapon is applied");
        check(A.wpn_count == 1 && A.wpn_id[0] == sword,
              "the applied slot holds the selected weapon id");
    }

    printf("[9] Demon HP-shop currency semantics (run_data.gd:1429-1437)\n");
    {
        init(BT_CHARACTER_DEMON);
        /* get_player_currency = get_stat(stat_max_hp) as int. RunData.get_stat is
         * the PERMANENT effects layer * its gain -- neither TempStats nor
         * LinkedStats -- truncated toward zero. */
        bt_core_stat_add(&A, 0, -3.0f);                    /* raw max_hp 7 */
        check_near(bt_character_currency(&A), 7.0f, 0.0f, "currency = int(stat_max_hp)");
        A.stat_gain[0] = 0.95f; bt_core_stat_sync(&A);     /* raw 7 * 0.95 = 6.65 */
        check_near(A.max_hp, 6.0f, 0.0f, "health = int(6.65) = 6");
        check_near(bt_character_currency(&A), 6.0f, 0.0f, "currency truncates to 6, not 7");
        A.stat_gain[0] = 1.0f; bt_core_stat_sync(&A);

        /* A temp layer (pet boost zone, TempStatsPerInterval, ...) raises the live
         * max HP but must NOT become spendable currency: get_player_currency reads
         * RunData.get_stat, which excludes TempStats/LinkedStats (utils.gd:461-466
         * only adds them in Utils.get_stat). */
        bt_core_stat_add_temp(&A, 0, 5.0f);
        check_near(A.max_hp, 12.0f, 0.0f, "temp max_hp raises the live max health");
        check_near(bt_character_currency(&A), 7.0f, 0.0f, "temp max_hp is NOT shop currency");
        bt_core_stat_add_temp(&A, 0, -5.0f);

        /* No death-by-shop and no currency floor: an affordable purchase may spend
         * the last point (currency 0); health keeps the separate max(1, ...) floor
         * (run_data.gd:301-302) so the player survives at 1 HP. */
        bt_character_currency_remove(&A, 7.0f);
        check_near(bt_character_currency(&A), 0.0f, 0.0f, "spending every point -> currency 0");
        check_near(A.max_hp, 1.0f, 0.0f, "max health floors at 1, no death-by-shop");

        /* Gold is untouched under hp_shop, and Bull still pays gold. */
        check(A.materials == BT_START_MATERIALS, "hp_shop never touched gold");
        init(BT_CHARACTER_BULL);
        bt_character_currency_remove(&A, 7.0f);
        check_near(A.materials, BT_START_MATERIALS - 7.0f, 0.0f, "Bull spends gold");
        check_near(A.max_hp, 30.0f, 0.0f, "Bull max_hp is not a currency");
    }

    printf("[10] Demon HP price = ceil(gold / 20) (shop_item.gd:132-140)\n");
    {
        int coffee = item_by_name("coffee");
        check(coffee >= 0, "an item exists for the price probe");
        unsigned short gs = bt_weapon_id_from_name("weapon_ghost_scepter_1");
        check(gs != BT_START_WEAPON_NONE, "the ghost scepter resolves to a weapon id");

        init(BT_CHARACTER_DEMON);
        float gold_i = bt_shop_price(&A, (unsigned short)coffee);
        float hp_i = bt_wshop_price_units(&A, BT_SHOP_ITEM, (unsigned short)coffee);
        check_near(hp_i, bt_ceilf(gold_i / 20.0f), 0.0f, "Demon item price = ceil(gold/20)");
        check(hp_i >= 1.0f, "the minimum HP price is 1");
        float gold_w = (float)bt_weapon_price(&A, gs);
        float hp_w = bt_wshop_price_units(&A, BT_SHOP_WEAPON, gs);
        check_near(hp_w, bt_ceilf(gold_w / 20.0f), 0.0f, "Demon weapon price = ceil(gold/20)");
        check(bt_wshop_hp_shop(&A) == 1 && bt_wshop_currency(&A) == bt_character_currency(&A),
              "the shop currency is stat_max_hp");

        /* Affordability gate uses the HP price (shop_items_container.gd:58-68). */
        float price = hp_i;
        A.stat_raw[0] = price; bt_core_stat_sync(&A);
        check(bt_wshop_currency(&A) >= price, "exactly affordable at the price");
        bt_wshop_pay(&A, price);
        check_near(bt_wshop_currency(&A), 0.0f, 0.0f, "paying the price leaves currency 0");

        /* Bull pays the raw gold price, no /20. */
        init(BT_CHARACTER_BULL);
        float bull_gold = bt_shop_price(&A, (unsigned short)coffee);
        check_near(bt_wshop_price_units(&A, BT_SHOP_ITEM, (unsigned short)coffee), bull_gold, 0.0f,
                   "Bull pays the un-divided gold price");
    }

    printf("[11] Demon has no bans/wants; guide interaction items stay eligible\n");
    {
        BT_CONST_PTR(BtCharacterDef) d = &BT_CHARACTERS[BT_CHARACTER_DEMON];
        check(d->ban_group_mask == 0 && d->ban_count == 0, "demon_data banned_* are empty");
        check(d->want_count == 0, "demon_data wanted_tags is empty");
        /* The ArosRising end-of-wave build's HP items must all be in the pool and
         * unbanned (the Demon bans nothing). */
        const char* names[] = { "padding", "sad_tomato", "torture", "handcuffs",
                                "spicy_sauce", "garden" };
        for (int i = 0; i < 6; i++) {
            int it = item_by_name(names[i]);
            check(it >= 0, "interaction item is in the pool");
            if (it < 0) continue;
            check(bt_character_item_banned(BT_CHARACTER_DEMON, (unsigned short)it) == 0,
                  "Demon does not ban a guide item");
            check(bt_character_wants_item(BT_CHARACTER_DEMON, (unsigned short)it) == 0,
                  "Demon wants no tags, so no offer is filtered");
        }
        /* Handcuffs caps HEALTH (hp_cap) at its pickup value; the shop currency
         * (uncapped stat_max_hp) keeps working past the cap
         * (utils.gd:469-485 get_capped_stat vs run_data.gd:1429). */
        init(BT_CHARACTER_DEMON);
        bt_core_stat_add(&A, 0, 40.0f);              /* raw 50 */
        A.stat_cap[0] = 20.0f; bt_core_stat_sync(&A);
        check_near(A.max_hp, 20.0f, 0.0f, "hp_cap limits max health (Handcuffs)");
        check_near(bt_character_currency(&A), 50.0f, 0.0f,
                   "hp_cap does NOT limit the shop currency");
    }

    printf("[12] Ghost Scepter on-kill Max HP gain (weapon.gd:203-230)\n");
    {
        unsigned short gs = bt_weapon_id_from_name("weapon_ghost_scepter_1");
        check(gs != BT_START_WEAPON_NONE, "ghost scepter resolves");
        check(bt_character_start_index(BT_CHARACTER_DEMON, "weapon_ghost_scepter_1") >= 0,
              "ghost scepter is a Demon starting weapon");
        init_weapon(BT_CHARACTER_DEMON, gs);
        check(A.wpn_count == 1 && A.wpn_id[0] == gs, "ghost scepter equipped");

        unsigned short rows[8];
        int n = bt_wfx_fx_rows(gs, rows, 8);
        check(n == 1, "one top-level weapon effect row");
        if (n == 1) {
            BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[rows[0]];
            check(E->opcode == BT_ITEMEFF_GAIN_STAT_EVERY_KILLED_ENEMIES,
                  "it is GainStatEveryKilledEnemiesEffect");
            check(E->value == 20, "every 20 kills grants the stat");
            check(bt_core_id_from_istr(E->aux_key) == 0, "the stat is stat_max_hp");
            check(E->nb_stat_scaled == 1, "+1 stat per threshold");
        }
        /* 19 kills: nothing; the 20th kill: +1 Max HP. */
        float before = A.max_hp;
        for (int k = 0; k < 19; k++) bt_wfx_on_weapon_kill(&A, &A.wfx, 0, 7u, -1, rows, n, 0);
        check_near(A.max_hp, before, 0.0f, "19 kills grant nothing");
        bt_wfx_on_weapon_kill(&A, &A.wfx, 0, 7u, -1, rows, n, 0);
        check_near(A.max_hp, before + 1.0f, 0.0f, "the 20th kill grants +1 Max HP");
        for (int k = 0; k < 20; k++) bt_wfx_on_weapon_kill(&A, &A.wfx, 0, 7u, -1, rows, n, 0);
        check_near(A.max_hp, before + 2.0f, 0.0f, "kills 21..40 grant one more");
    }

    printf("[13] end-of-wave conversion is wired (main.gd:1225 / utils.gd:559-588)\n");
    {
        /* A Demon with no items must still convert through the real wave-end path
         * (regression: the call site, not just the helper). */
        bt_core_init(&A, 4242u, 0, 1, BT_CHARACTER_DEMON, BT_START_WEAPON_NONE);
        check(A.phase == 0, "starts in combat");
        A.materials = 130.0f;                       /* 50% -> 65 -> 5 chunks of 13 */
        bt_progress_end_wave(&A);
        /* the shop opens at the EndWaveTimer timeout (2.0 s = 120 ticks); a
         * window frame also ticks the killed nodes' death animation. */
        for (int t = 0; t < BT_END_WAVE_TICKS; t++) {
            bt_enemy_dying_step(&A);
            bt_progress_wave_end_step(&A);
        }
        check(A.phase == 1, "wave end opens the shop");
        check_near(A.materials, 65.0f, 0.0f, "removes 5*13 = 65 gold");
        check_near(A.max_hp, 15.0f, 0.0f, "granted 5 Max HP (10 base + 5)");

        /* Below one chunk (25 gold -> 12.5 -> 0 chunks) nothing happens. */
        bt_core_init(&A, 4243u, 0, 1, BT_CHARACTER_DEMON, BT_START_WEAPON_NONE);
        A.materials = 25.0f;
        bt_progress_end_wave(&A);
        check_near(A.materials, 25.0f, 0.0f, "no chunk -> gold untouched");
        check_near(A.max_hp, 10.0f, 0.0f, "no chunk -> no Max HP");

        /* Exactly one chunk (26 gold -> 13 -> 1 chunk). */
        bt_core_init(&A, 4245u, 0, 1, BT_CHARACTER_DEMON, BT_START_WEAPON_NONE);
        A.materials = 26.0f;
        bt_progress_end_wave(&A);
        check_near(A.materials, 13.0f, 0.0f, "one chunk consumes 13 gold");
        check_near(A.max_hp, 11.0f, 0.0f, "one chunk grants 1 Max HP");

        /* Bull never converts. */
        bt_core_init(&A, 4244u, 0, 1, BT_CHARACTER_BULL, BT_START_WEAPON_NONE);
        A.materials = 130.0f;
        bt_progress_end_wave(&A);
        check_near(A.materials, 130.0f, 0.0f, "Bull keeps all gold");
    }

    demon_episode();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
