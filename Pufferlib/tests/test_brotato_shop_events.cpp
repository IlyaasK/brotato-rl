/* Unit tests for brotato_progression.h's shop / wave-boundary consumers:
 *   - item tier domain (source Tier 0..3, not the upgrade table's 1..4)
 *   - G2 duplicate_item: mirror duplicates the purchase and is consumed
 *   - G3 increase_tier_on_reroll: goldfish consumed per reroll, offer tier up
 *   - G5 gain_pct_gold_start_wave: piggy bank wave-start gold
 *   - G6 extra_elite_next_wave_chance: candy bag zone elite at Go
 *   - G8 item_hourglass: Go replays the wave, hourglass -> broken_hourglass
 *   - free-reroll acquisition delta + emptied-shop bonus reroll
 *   - extra_item_in_crate (pearl / treasure_map) + deferred crate grant
 *   - the extract_items.py replaced_by regression (generated table)
 *
 * Included through brotato_core.h exactly like the slice headers, so every
 * helper (bt_apply_item, bt_loot_pick_box_item, bt_item_remove, ...) is the one
 * the real core uses. bt_item_remove and the KEY_VALUE bucket hook are
 * parent-owned; a missing one fails these checks rather than the build's parity.
 *
 * Every check is a hand-derived source value or an invariant that must hold for
 * any RNG draw.
 */
#include "../ocean/brotato/brotato_core.h"
#include <cstdio>
#include <cstring>
#include <cmath>

static int failures;
static void check(bool ok, const char *msg) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", msg); failures++; }
}
static bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) < eps; }

static BtState st;
static BtState st2;   /* a second state: BtState is 6.7 MB, never on the stack */

static int item_by_name(const char *name) {
    for (int i = 0; i < BT_NITEMS; i++)
        if (!std::strcmp(BT_ITEMS[i].name, name)) return i;
    return -1;
}
static long total_owned(const BtState &s) {
    long n = 0;
    for (int i = 0; i < BT_NITEMS; i++) n += (long)s.item_owned[i];
    return n;
}
/* First unlimited, no-pet/structure/explode item that the Bull shop offers and
 * that carries none of the shop-progression keys under test. */
static bool has_key(const BtItem &it, unsigned short k) {
    for (int e = 0; e < (int)it.eff_count; e++) {
        int row = BT_ITEM_EFF_INDEX[it.eff_first + e];
        if (BT_ITEM_EFF_ROWS[row].key == k || BT_ITEM_EFF_ROWS[row].custom_key == k) return true;
    }
    return false;
}
static int first_plain_unlimited(void) {
    for (int i = 0; i < BT_NITEMS; i++) {
        if (BT_ITEMS[i].max_nb != -1) continue;
        if (BT_ITEMS[i].special != 0) continue;
        if (!bt_item_shop_ok(BT_CHARACTER_BULL, (unsigned short)i)) continue;
        if (has_key(BT_ITEMS[i], BT_ISTR_FREE_REROLLS)) continue;
        if (has_key(BT_ITEMS[i], BT_ISTR_ITEM_HOURGLASS)) continue;
        if (has_key(BT_ITEMS[i], BT_ISTR_GAIN_PCT_GOLD_START_WAVE)) continue;
        if (has_key(BT_ITEMS[i], BT_ISTR_DUPLICATE_ITEM)) continue;
        return i;
    }
    return -1;
}
static int find_bucket(const BtState &s, unsigned short custom_key) {
    for (int a = 0; a < s.n_bk_active; a++) {
        int row = (int)s.bk_active[a];
        if (BT_ITEM_EFF_ROWS[row].custom_key == custom_key) return row;
    }
    return -1;
}
static void give(BtState &s, int idx) {
    s.curse_apply = 0;
    bt_apply_item(&s, (unsigned short)idx);
}

int main(void) {
    const int MIRROR = item_by_name("mirror");
    const int BROKEN_MIRROR = item_by_name("broken_mirror");
    const int GOLDFISH = item_by_name("goldfish");
    const int GOLDFISH_USED = item_by_name("goldfish_used");
    const int HOURGLASS = item_by_name("hourglass");
    const int BROKEN_HOURGLASS = item_by_name("broken_hourglass");
    const int PIGGY = item_by_name("piggy_bank");
    const int PEARL = item_by_name("pearl");
    const int TREASURE_MAP = item_by_name("treasure_map");
    check(MIRROR >= 0 && BROKEN_MIRROR >= 0 && GOLDFISH >= 0 && GOLDFISH_USED >= 0 &&
          HOURGLASS >= 0 && BROKEN_HOURGLASS >= 0 && PIGGY >= 0 && PEARL >= 0 &&
          TREASURE_MAP >= 0, "all shaped items resolve by id");
    if (failures) return 1;

    /* ---- extract_items.py replaced_by regression ------------------------- *
     * run_data.gd:880 add_item(item.replaced_by): mirror -> broken_mirror,
     * goldfish -> goldfish_used, hourglass -> broken_hourglass. */
    check(BT_ITEMS[MIRROR].replaced_by == (unsigned short)BROKEN_MIRROR,
          "mirror.replaced_by resolves to broken_mirror");
    check(BT_ITEMS[GOLDFISH].replaced_by == (unsigned short)GOLDFISH_USED,
          "goldfish.replaced_by resolves to goldfish_used");
    check(BT_ITEMS[HOURGLASS].replaced_by == (unsigned short)BROKEN_HOURGLASS,
          "hourglass.replaced_by resolves to broken_hourglass");
    check(BT_ITEMS[PIGGY].replaced_by == (unsigned short)BT_ITEM_NONE,
          "piggy_bank has no replacement");

    /* ---- exact source elite/boss roster order --------------------------- *
     * item_service.tscn:703 `elites`/`bosses` (campaign) followed by
     * dlc_data.tres:224 `elites`/`bosses` (dlc_data.gd:63 append_array), then
     * get_*_from_zone filters by zone_id (item_service.gd:841-848). */
    {
        static const char *z0[8] = {"elite_butcher", "elite_colossus", "elite_croco",
                                    "elite_insect", "elite_monk", "elite_rhino",
                                    "elite_demon", "elite_mom"};
        check(BT_ZONE_NELITES[0] == 8, "zone 0 has 8 elites");
        for (int i = 0; i < 8; i++)
            check(std::strcmp(BT_KIND_ID[BT_ZONE_ELITES[0][i]], z0[i]) == 0,
                  "zone 0 elite roster follows ItemService.elites order");
        static const char *z1[9] = {"elite_giant", "elite_giant_isopod", "elite_jellyfish",
                                    "elite_prisoner", "elite_impaled_worm",
                                    "elite_spider_crab", "elite_turtle", "elite_bat",
                                    "elite_megalodon"};
        check(BT_ZONE_NELITES[1] == 9, "zone 1 has 9 elites");
        for (int i = 0; i < 9; i++)
            check(std::strcmp(BT_KIND_ID[BT_ZONE_ELITES[1][i]], z1[i]) == 0,
                  "zone 1 elite roster follows dlc_data.elites order");
        check(std::strcmp(BT_KIND_ID[BT_ZONE_BOSSES[0][0]], "boss_crab") == 0 &&
              std::strcmp(BT_KIND_ID[BT_ZONE_BOSSES[0][1]], "boss_wizard") == 0,
              "zone 0 boss roster follows ItemService.bosses order (predator, invoker)");
        check(std::strcmp(BT_KIND_ID[BT_ZONE_BOSSES[1][0]], "boss_eel") == 0 &&
              std::strcmp(BT_KIND_ID[BT_ZONE_BOSSES[1][1]], "boss_dead_whale") == 0,
              "zone 1 boss roster follows dlc_data.bosses order");
    }

    /* ---- item tier domain + increase_tier clamp -------------------------- */
    bt_core_init(&st, 7, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
    for (int w = 1; w <= 20; w++) {
        int t = bt_progress_item_tier(&st, w);
        check(t >= 0 && t <= 3, "item tier stays in source Tier 0..3");
    }
    /* a plain roll must offer real, lootable items of the source tier domain,
     * never a nonexistent Tier 4 (the pre-fix shop compared against 1..4). */
    st.phase = 1; st.shop_timer = 0; st.shop_available = 15; st.wave = 1;
    bt_progress_roll_shop(&st, 0);
    for (int i = 0; i < 4; i++) {
        check(st.shop_kind[i] == BT_SHOP_ITEM && st.shop_id[i] < BT_NITEMS &&
              BT_ITEMS[st.shop_id[i]].tier <= 3 &&
              bt_loot_item_pool_ok(&st, st.shop_id[i]),
              "plain roll offers a lootable item in Tier 0..3");
    }
    /* get_tier_from_wave clamps tier+N to Tier.LEGENDARY (3). */
    st.shop_available = 15;
    bt_progress_roll_shop(&st, 3);
    for (int i = 0; i < 4; i++)
        check(st.shop_kind[i] == BT_SHOP_ITEM && st.shop_id[i] < BT_NITEMS &&
              BT_ITEMS[st.shop_id[i]].tier == 3,
              "increase_tier 3 forces Tier.LEGENDARY offers");

    /* ---- shop ITEM offers share the loot selection rule (axolotl reroll) --- *
     * item_service.gd:310-450 is one function for both consumers. With a Bull
     * shop at a guaranteed-type wave (wave <= 2: no CHANCE_WEAPON draw) and
     * args.increase_tier = 3, the primary pool is exactly the Tier.LEGENDARY
     * ITEMS pool, so the shop's first slot is the same single
     * _get_rand_item_for_wave draw sequence the crate path runs (rand_wanted,
     * tier, wanted-tag, pick, then the 50% axolotl reroll at :463-465). If the
     * shop omits that reroll, every seed where the crate path repicks the
     * axolotl diverges. */
    {
        int mism = 0, axolotl = 0;
        for (bt_u64 s = 1; s <= 3000; s++) {
            bt_core_init(&st, s, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            st.wave = 1;
            int oracle = bt_loot_pick_box_item(&st, 1, 3);
            bt_core_init(&st2, s, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            st2.wave = 1; st2.shop_available = 0;
            for (int i = 0; i < 4; i++) { st2.shop_kind[i] = BT_SHOP_NONE; st2.shop_id[i] = 0; }
            bt_progress_roll_shop(&st2, 3);
            if (st2.shop_kind[0] != BT_SHOP_ITEM || (int)st2.shop_id[0] != oracle) mism++;
            if (oracle == BT_ITEM_IDX_AXOLOTL) axolotl++;
        }
        check(mism == 0, "shop slot 0 == the loot selection for the same draws");
        check(axolotl > 0, "the sampled seeds expose the axolotl first pick");
    }

    /* ---- endless pool swap (item_service.gd:419-443) ---------------------- *
     * current_wave < nb_of_waves uses the character's banned_item_groups +
     * banned_items; at current_wave >= nb_of_waves (20) it uses
     * banned_items_for_endless (item_crown, item_piggy_bank) INSTEAD, so the
     * character bans no longer apply. */
    {
        const int crown = item_by_name("crown"), piggy = item_by_name("piggy_bank");
        const int butter = item_by_name("butterfly"), jelly = item_by_name("jelly");
        check(crown >= 0 && piggy >= 0 && butter >= 0 && jelly >= 0, "ban probe items resolve");
        bt_core_init(&st, 3, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        struct { int it; int w19, w20; const char *msg19, *msg20; } row[4] = {
            { crown,  1, 0, "wave 19 keeps the crown", "wave 20 endless-bans the crown" },
            { piggy,  1, 0, "wave 19 keeps the piggy bank", "wave 20 endless-bans the piggy bank" },
            { butter, 0, 1, "wave 19 applies Bull's lifesteal ban", "wave 20 drops the lifesteal ban" },
            { jelly,  0, 1, "wave 19 applies Bull's banned item", "wave 20 drops the character item ban" },
        };
        for (int r = 0; r < 4; r++) {
            st.wave = 19;
            check(bt_progress_item_candidate(&st, row[r].it, BT_ITEMS[row[r].it].tier, 0, 0,
                      0, 0, 0u, 0) == row[r].w19, row[r].msg19);
            st.wave = 20;
            check(bt_progress_item_candidate(&st, row[r].it, BT_ITEMS[row[r].it].tier, 0, 0,
                      0, 0, 0u, 0) == row[r].w20, row[r].msg20);
        }
        int banned_offers = 0;
        for (bt_u64 s = 1; s <= 400; s++) {
            bt_core_init(&st2, s, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            st2.wave = 20; st2.shop_available = 0;
            bt_progress_roll_shop(&st2, 0);
            for (int i = 0; i < 4; i++)
                if (st2.shop_kind[i] == BT_SHOP_ITEM &&
                    ((int)st2.shop_id[i] == crown || (int)st2.shop_id[i] == piggy)) banned_offers++;
        }
        check(banned_offers == 0, "no endless-banned item is offered at wave 20");
    }

    /* ---- shop price gate is exactly ItemService.get_value ----------------- *
     * shop_items_container.gd:58-62 refuses a buy while
     * get_player_currency < shop_item.value; shop_item.gd:129-132 sets
     * value = get_value(wave_value, item.value, player, true, false). */
    {
        int it = first_plain_unlimited();
        check(it >= 0, "a plain unlimited item for the price gate");
        if (it >= 0) {
            bt_core_init(&st, 101, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            st.wave = 6;
            int price = (int)bt_shop_price(&st, (unsigned short)it);
            check(price >= 1, "shop price is at least 1");
            /* the item must not grant gold itself, so the delta is the price */
            bt_core_init(&st2, 202, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            st2.wave = 6; st2.curse_apply = 0; st2.materials = 0.0f;
            bt_apply_item(&st2, (unsigned short)it);
            check(near(st2.materials, 0.0f), "the priced item grants no gold itself");
            st.phase = 1; st.shop_timer = 0; st.shop_available = 1u;
            st.shop_kind[0] = BT_SHOP_ITEM; st.shop_id[0] = (unsigned short)it; st.shop_cursed[0] = 0;
            st.materials = (float)(price - 1);
            bt_progress_shop_action(&st, 0);
            check(st.item_owned[it] == 0 && st.shop_kind[0] == BT_SHOP_ITEM,
                  "one gold short of the price leaves the offer unbought");
            st.materials = (float)price;
            bt_progress_shop_action(&st, 0);
            check(st.item_owned[it] == 1 && near(st.materials, 0.0f),
                  "exactly the price buys the offer and spends exactly the price");
        }
    }

    /* ---- reroll price: get_reroll_price(wave, paid_count) ----------------- *
     * item_service.gd:726-730: normal = int(wave*0.75) + delta + delta*count,
     * delta = int(max(1.0, 0.4*wave)); the charged value is ceil(normal *
     * max(0.1, 1 + reroll_price/100)) with reroll_price = 0 here. */
    {
        bt_core_init(&st, 303, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        st.wave = 4; st.materials = 100000.0f;
        st.phase = 1; st.shop_timer = 0; st.shop_available = 15;
        /* the button price is stored at the shop's own open (base_shop.gd:132):
         * this synthetic shop skips that path, so refresh it here. */
        bt_wshop_refresh_reroll_price(&st);
        float before = st.materials;
        bt_progress_shop_action(&st, 4);
        check(near(before - st.materials, 4.0f), "wave 4 reroll 0 costs int(3)+1 = 4");
        before = st.materials;
        bt_progress_shop_action(&st, 4);
        check(near(before - st.materials, 5.0f), "wave 4 reroll 1 costs int(3)+1+1 = 5");
        check(st.paid_rerolls == 2, "two paid rerolls counted");
        /* the reroll is charged in GOLD even under hp_shop (base_shop.gd:443
         * remove_gold), never in the shop currency. */
        bt_core_init(&st, 304, 0, 1, BT_CHARACTER_DEMON, BT_WEAPON_NONE);
        st.wave = 4; st.materials = 100000.0f;
        st.phase = 1; st.shop_timer = 0; st.shop_available = 15;
        bt_wshop_refresh_reroll_price(&st);   /* synthetic shop: see above */
        float hp0 = st.max_hp;
        bt_progress_shop_action(&st, 4);
        check(near(st.max_hp, hp0, 0.5f), "a Demon reroll never spends max HP");
        check(near(st.materials, 100000.0f - 4.0f, 0.5f), "a Demon reroll spends gold");
    }

    /* ---- shop lock (shop_item.gd:238-252, base_shop.gd:360-380) ----------- *
     * A locked offer survives a reroll and is re-placed at the FRONT of the new
     * offer list, in lock order (fill_shop_items sets _shop_items =
     * player_locked_items.duplicate(), then appends the regenerated ones). */
    {
        bt_core_init(&st, 501, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        st.wave = 6; st.phase = 1; st.shop_timer = 0; st.materials = 100000.0f;
        bt_progress_roll_shop(&st, 0);
        bt_progress_shop_action(&st, BT_ACT_LOCK0 + 3);
        check(bt_wshop_is_locked(&st, 3) == 1, "the lock action locks an offer");
        bt_progress_shop_action(&st, BT_ACT_LOCK0 + 1);
        check(bt_wshop_is_locked(&st, 1) == 1 && bt_wshop_is_locked(&st, 3) == 1, "two offers locked");
        unsigned short keep3 = st.shop_id[3], keep1 = st.shop_id[1];
        unsigned char kind3 = st.shop_kind[3], kind1 = st.shop_kind[1];
        bt_progress_shop_action(&st, BT_ACT_REROLL);
        check(st.paid_rerolls == 1, "the reroll was paid");
        check(st.shop_kind[0] == kind3 && st.shop_id[0] == keep3,
              "the first locked offer is re-placed first");
        check(st.shop_kind[1] == kind1 && st.shop_id[1] == keep1,
              "the second locked offer is re-placed second");
        check(bt_wshop_is_locked(&st, 0) == 1 && bt_wshop_is_locked(&st, 1) == 1,
              "the locks follow their offers");
        check(bt_wshop_is_locked(&st, 2) == 0 && bt_wshop_is_locked(&st, 3) == 0,
              "the regenerated offers are unlocked");
        check(st.shop_id[2] != keep3 && st.shop_id[2] != keep1 &&
              st.shop_id[3] != keep3 && st.shop_id[3] != keep1,
              "the regenerated offers differ from the locked ones");
        bt_progress_shop_action(&st, BT_ACT_LOCK0 + 0);
        check(bt_wshop_is_locked(&st, 0) == 0 && bt_wshop_is_locked(&st, 1) == 1,
              "unlocking one offer leaves the other locked");
        bt_progress_open_shop(&st);
        check(!bt_wshop_is_locked(&st, 0) && !bt_wshop_is_locked(&st, 1),
              "a new shop starts unlocked");
    }

    /* ---- ban an offer (shop_item.gd:204-210, run_data.gd:19,559-560) ------ */
    {
        bt_core_init(&st, 503, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        st.wave = 6; st.phase = 1; st.shop_timer = 0;
        bt_progress_roll_shop(&st, 0);
        check(st.ban_tokens == 8, "every run starts with 8 ban tokens");
        unsigned short id0 = st.shop_id[0];
        /* The core's default is ban_mode = 1: the RL bridge's default settings
         * force ProgressData.settings.ban_mode_toggled = true (rl_env.gd:34) and
         * rl_run.gd:467 mirrors it into uses_ban, so the ban UI is live in every
         * recorded run (the v4/v5 shop rows show ban_tokens 8 and applied bans).
         * The "ban UI off" direction must therefore be requested explicitly. */
        st.ban_mode = 0;
        bt_progress_shop_action(&st, BT_ACT_BAN0 + 0);
        check(st.ban_tokens == 8 && st.shop_kind[0] != BT_SHOP_NONE,
              "with ban mode off the ban action does nothing");
        st.ban_mode = 1;
        bt_progress_shop_action(&st, BT_ACT_BAN0 + 0);
        check(st.ban_tokens == 7, "a ban spends one token");
        check(st.shop_kind[0] == BT_SHOP_NONE && !(st.shop_available & 1u),
              "the banned offer is removed from the shop");
        check(st.ban_count == 1 && (int)st.ban_kind[0] == BT_SHOP_ITEM && st.ban_id[0] == id0,
              "the banned item is remembered by id");
        int seen = 0;
        for (bt_u64 s2 = 1; s2 <= 300; s2++) {
            bt_core_init(&st2, s2, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            st2.wave = 6; st2.phase = 1;
            st2.ban_count = st.ban_count;
            for (int b = 0; b < (int)st.ban_count; b++) {
                st2.ban_kind[b] = st.ban_kind[b]; st2.ban_id[b] = st.ban_id[b];
            }
            bt_progress_roll_shop(&st2, 0);
            for (int i = 0; i < 4; i++)
                if (st2.shop_kind[i] == BT_SHOP_ITEM && st2.shop_id[i] == id0) seen++;
        }
        check(seen == 0, "a banned item never reappears in an offer");
    }

    /* ---- level-up reroll (upgrades_ui_player_container.gd:16,57-75,175-199) */
    {
        bt_core_init(&st, 601, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        st.wave = 4; st.materials = 100000.0f;
        st.phase = 2; st.pending_upgrades = 1; st.lvl_reroll_count = 0;
        bt_progress_roll_upgrade(&st);
        check(bt_progress_lvl_reroll_price(&st) == 4,
              "wave 4 level-up reroll 0 costs int(3)+1 = 4");
        float m = st.materials;
        bt_progress_choose_upgrade(&st, BT_ACT_REROLL);
        check(near(m - st.materials, 4.0f), "the first level-up reroll is charged 4 gold");
        check(st.lvl_reroll_count == 1 && st.pending_upgrades == 1,
              "the reroll neither chooses an upgrade nor spends a pending level");
        check(bt_progress_lvl_reroll_price(&st) == 5, "reroll 1 costs 5");
        m = st.materials;
        bt_progress_choose_upgrade(&st, BT_ACT_REROLL);
        check(near(m - st.materials, 5.0f), "the second level-up reroll is charged 5 gold");
        st.materials = 0.0f;
        int c = st.lvl_reroll_count;
        bt_progress_choose_upgrade(&st, BT_ACT_REROLL);
        check(st.lvl_reroll_count == c, "an unaffordable level-up reroll is refused");
    }

    /* ---- loot pool unlock profile (progress_data.gd:884-895) ------------- *
     * lock_default_items = 1 models a fresh save: only unlocked_by_default
     * items remain in _tiers_data.  It is 0 (fully unlocked) by default. */
    {
        int def = -1, non = -1;
        for (int i = 0; i < BT_NITEMS; i++) {
            if (!BT_ITEMS[i].can_be_looted || BT_ITEMS[i].max_nb == 0) continue;
            if (BT_ITEMS[i].unlocked_by_default && def < 0) def = i;
            if (!BT_ITEMS[i].unlocked_by_default && non < 0) non = i;
        }
        check(def >= 0 && non >= 0, "both an unlocked-by-default and a locked item exist");
        bt_core_init(&st, 701, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        check(st.lock_default_items == 0, "the fully unlocked profile is the default");
        check(bt_loot_item_pool_ok(&st, (unsigned short)def) == 1 &&
              bt_loot_item_pool_ok(&st, (unsigned short)non) == 1,
              "fully unlocked: every lootable item is in the pool");
        st.lock_default_items = 1;
        check(bt_loot_item_pool_ok(&st, (unsigned short)def) == 1 &&
              bt_loot_item_pool_ok(&st, (unsigned short)non) == 0,
              "fresh save: a locked item leaves the pool, an unlocked one stays");
    }

    /* ---- KEY_VALUE / APPEND_KEY_VALUE bucket registration ---------------- */
    bt_core_init(&st, 5, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
    give(st, MIRROR);
    check(find_bucket(st, BT_ISTR_DUPLICATE_ITEM) >= 0,
          "mirror registers a duplicate_item bucket");
    give(st, GOLDFISH);
    check(find_bucket(st, BT_ISTR_INCREASE_TIER_ON_REROLL) >= 0,
          "goldfish registers an increase_tier_on_reroll bucket");
    give(st, TREASURE_MAP);
    check(find_bucket(st, BT_ISTR_EXTRA_ITEM_IN_CRATE) >= 0,
          "treasure_map registers an extra_item_in_crate bucket");

    /* ---- G2 mirror: duplicate the purchase, consume the mirror ----------- */
    bt_core_init(&st, 11, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
    give(st, MIRROR);
    check(st.item_owned[MIRROR] == 1, "one mirror owned");
    int x = first_plain_unlimited();
    check(x >= 0 && x != MIRROR, "unlimited plain purchase target found");
    if (x >= 0) {
        st.phase = 1; st.shop_timer = 0; st.shop_available = 1u;
        st.shop_kind[0] = BT_SHOP_ITEM; st.shop_id[0] = (unsigned short)x;
        st.shop_cursed[0] = 0;
        st.materials = 100000.0f;
        float r = bt_progress_shop_action(&st, 0);
        check(r > 0.0f, "mirror purchase accepted");
        /* duplicated_count = min(value 1, remaining-1) = 1 -> +1, then the
         * normal purchase -> 2 owned. */
        check(st.item_owned[x] == 2, "mirror duplicated the purchase");
        check(st.item_owned[MIRROR] == 0, "mirror was consumed");
        check(st.item_owned[BROKEN_MIRROR] == 1, "mirror became broken_mirror");
    }

    /* ---- mirror duplicates reuse the baked curse phenotype -------------- *
     * base_shop.gd:566-577 applies ONE already-curse-baked item_data N+1
     * times with no new draws; the core must replay the construction RNG so
     * every copy is identical and the stream advances exactly once. */
    bt_core_init(&st, 61, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
    give(st, MIRROR);
    {
        int q = first_plain_unlimited();
        check(q >= 0, "cursed mirror duplicate target found");
        if (q >= 0) {
            static BtState ref;                 /* one cursed apply, isolated */
            ref = st;
            ref.curse_apply = 1; ref.curse_maxmod = 0.0f;
            bt_apply_item(&ref, (unsigned short)q);

            st.phase = 1; st.shop_timer = 0; st.shop_available = 1u;
            st.shop_kind[0] = BT_SHOP_ITEM; st.shop_id[0] = (unsigned short)q;
            st.shop_cursed[0] = 1;
            st.materials = 100000.0f;
            bt_progress_shop_action(&st, 0);

            check(st.item_owned[q] == 2, "cursed item duplicated once");
            check(st.item_owned[MIRROR] == 0 && st.item_owned[BROKEN_MIRROR] == 1,
                  "mirror consumed by the cursed duplication");
            check(st.rng.state == ref.rng.state && st.rng.inc == ref.rng.inc,
                  "mirror duplication consumes the construction RNG exactly once");
            check(st.utils_rng.state == ref.utils_rng.state,
                  "mirror duplication leaves the utils stream advanced once");
            check(near(st.curse_maxmod, ref.curse_maxmod),
                  "mirror duplicates keep the baked curse modifier");
        }
    }

    /* ---- G3 goldfish: consumed per reroll, raises the offer tier --------- */
    bt_core_init(&st, 13, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
    give(st, GOLDFISH);
    int inc = bt_progress_consume_reroll_tier(&st);
    check(inc == 1, "goldfish raises the reroll tier by its effect value");
    check(st.item_owned[GOLDFISH] == 0, "goldfish consumed");
    check(st.item_owned[GOLDFISH_USED] == 1, "goldfish became goldfish_used");
    check(bt_progress_consume_reroll_tier(&st) == 0, "no goldfish -> no tier raise");

    bt_core_init(&st, 131, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
    give(st, GOLDFISH);
    st.phase = 1; st.shop_timer = 0; st.wave = 3; st.shop_available = 15;
    st.materials = 100000.0f;
    bt_progress_shop_action(&st, 4);
    check(st.item_owned[GOLDFISH] == 0 && st.item_owned[GOLDFISH_USED] == 1,
          "a reroll consumes the owned goldfish");
    check(st.shop_available == 15, "reroll refilled the shop");

    /* ---- free-reroll acquisition delta (base_shop.gd:530-536) ------------- */
    bt_core_init(&st, 17, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
    bt_progress_open_shop(&st);
    check(st.initial_free_rerolls == 0 && st.free_rerolls_remaining == 0,
          "shop opens with no free rerolls");
    {
        int y = first_plain_unlimited();
        check(y >= 0, "unlimited plain target for the delta test");
        if (y >= 0) {
            st.shop_timer = 0; st.shop_kind[0] = BT_SHOP_ITEM;
            st.shop_id[0] = (unsigned short)y; st.shop_available = 15;
            st.materials = 100000.0f;
            /* an acquisition (+free_rerolls) raises the total mid-shop ... */
            st.scalar_effect[BT_ISTR_FREE_REROLLS] += 1.0f;
            bt_progress_shop_action(&st, 0);
            check(st.free_rerolls_remaining == 1 && st.initial_free_rerolls == 1,
                  "mid-shop free_rerolls delta is added to the current shop");
        }
    }

    /* ---- emptied-shop bonus reroll (base_shop.gd:538 + :444-449) ---------- */
    bt_core_init(&st, 19, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
    {
        int z = first_plain_unlimited();
        check(z >= 0, "unlimited plain target for the bonus test");
        if (z >= 0) {
            st.phase = 1; st.shop_timer = 0; st.shop_available = 1u;
            st.shop_kind[0] = BT_SHOP_ITEM; st.shop_id[0] = (unsigned short)z;
            st.shop_cursed[0] = 0;
            st.materials = 100000.0f;
            bt_progress_shop_action(&st, 0);
            check(st.shop_available == 0, "buying the last offer empties the shop");
            check(st.has_bonus_free_reroll == 1, "emptied shop arms the bonus reroll");
            st.materials = 0.0f;               /* free even with no gold */
            st.free_rerolls_remaining = 0;
            bt_progress_shop_action(&st, 4);
            check(st.has_bonus_free_reroll == 0, "bonus reroll is consumed");
            check(st.paid_rerolls == 0, "bonus reroll is not a paid reroll");
            check(near(st.materials, 0.0f), "bonus reroll costs no gold");
        }
    }

    /* ---- G5 piggy bank wave-start gold (main.gd:1386-1400) --------------- */
    bt_core_init(&st, 23, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
    give(st, PIGGY);
    check(near(st.scalar_effect[BT_ISTR_GAIN_PCT_GOLD_START_WAVE], 20.0f),
          "piggy bank adds 20% start-of-wave gold");
    st.wave = 3; st.materials = 100.0f;
    bt_progress_piggy_bank(&st);
    check(near(st.materials, 120.0f), "wave 3 start gold +20%");
    st.wave = 25; st.materials = 100.0f;   /* past nb_of_waves (20) */
    bt_progress_piggy_bank(&st);
    check(near(st.materials, 100.0f), "positive pct does nothing past the last wave");
    st.wave = 3; st.materials = 100.0f;
    st.scalar_effect[BT_ISTR_GAIN_PCT_GOLD_START_WAVE] = -50.0f;
    bt_progress_piggy_bank(&st);
    check(near(st.materials, 50.0f), "negative pct reduces start gold mid-run");
    st.wave = 25; st.materials = 100.0f;
    bt_progress_piggy_bank(&st);
    check(near(st.materials, 0.0f), "past the last wave a negative pct is pinned to -100");

    /* ---- G8 hourglass: replay the wave, consume ------------------------- */
    bt_core_init(&st, 29, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
    give(st, HOURGLASS);
    check(st.item_owned[HOURGLASS] == 1, "hourglass owned");
    st.wave = 5; st.phase = 1; st.shop_timer = 0; st.materials = 1000.0f;
    bt_progress_shop_action(&st, 8);
    /* base_shop.gd:339-349 ends in change_scene(), which Godot applies at the
     * frame's end: the wave/arena half of the GO runs on the NEXT tick
     * (bt_progress_shop_go), so the tests drive that half explicitly. */
    bt_progress_shop_go(&st);
    check(st.wave == 5, "hourglass replays the wave instead of advancing");
    check(st.item_owned[HOURGLASS] == 0 && st.item_owned[BROKEN_HOURGLASS] == 1,
          "hourglass consumed -> broken_hourglass");
    check(near(st.hp, 1.0f), "replay starts at 1 hp (broken hourglass -100%)");

    bt_core_init(&st, 31, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
    st.wave = 5; st.phase = 1; st.shop_timer = 0; st.materials = 0.0f;
    bt_progress_shop_action(&st, 8);
    check(st.wave == 6, "without an hourglass Go advances the wave");

    /* ---- held item-box crate offer + its granted extras ------------------ *
     * upgrades_ui.gd:88-115 shows ONE option at a time: show_consumable_data
     * generates the crate item (:113 process_item_box) AFTER the box's
     * extra_item_in_crate rolls (:114), and :100-105 resolves each granted
     * extra as its OWN take/recycle offer. Take -> main.gd:1141 add_item;
     * recycle -> main.gd:1145 add_gold. */
    bt_core_init(&st, 37, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
    for (int k = 0; k < 5; k++) give(st, TREASURE_MAP);
    check(st.item_owned[TREASURE_MAP] == 5, "5 treasure maps owned");
    st.wave = 3;
    {
        unsigned short keys[8];
        int n = bt_progress_crate_extra_roll(&st, keys, 8);
        check(n == 1 && keys[0] == BT_ISTR_RANDOM,
              "5 treasure maps = one 100% random crate extra");
    }
    {
        long before = total_owned(st);
        st.pending_items = 1;
        st.pending_item_kind[0] = BT_CONS_ITEM_BOX;
        st.pending_upgrades = 0;
        st.crate_kind = BT_SHOP_NONE; st.crate_extra_count = 0;
        bt_progress_next_crate_offer(&st);
        check(st.pending_items == 0, "the box is dequeued when its crate item is generated");
        check(st.crate_kind == BT_SHOP_ITEM && st.crate_id < BT_NITEMS,
              "the box offers its crate item for a take/recycle decision");
        check(total_owned(st) == before, "the crate item is NOT auto-taken");
        check((int)st.crate_extra_count == 1 && st.crate_extra_key[0] == BT_ISTR_RANDOM,
              "the granted extra is queued behind the crate item");
        st.phase = 2;
        bt_progress_crate_take(&st);
        check(total_owned(st) == before + 1, "the crate item is taken on the TAKE action");
        check(st.crate_kind == BT_SHOP_ITEM, "the granted extra becomes the next offer");
        bt_progress_crate_take(&st);
        check(total_owned(st) == before + 2, "the granted extra is taken too");
        check(st.crate_kind == BT_SHOP_NONE && st.phase == 1, "no offers left -> the shop opens");
    }
    bt_core_init(&st, 39, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
    {
        unsigned short keys[8];
        check(bt_progress_crate_extra_roll(&st, keys, 8) == 0,
              "no extra_item_in_crate source -> nothing rolled");
    }
    {
        bt_core_init(&st, 43, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        st.wave = 3;
        st.pending_items = 1;
        st.pending_item_kind[0] = BT_CONS_ITEM_BOX;
        st.crate_kind = BT_SHOP_NONE; st.crate_extra_count = 0;
        bt_progress_next_crate_offer(&st);
        int id = (int)st.crate_id;
        int value = bt_wshop_item_recycle_value(&st, st.crate_id);
        float gold0 = st.materials;
        st.phase = 2;
        bt_progress_crate_recycle(&st);
        check(st.item_owned[id] == 0, "a recycled crate item is not taken");
        check(near(st.materials, gold0 + (float)value), "recycle credits the item's gold value");
        check(st.crate_kind == BT_SHOP_NONE && st.phase == 1, "recycling the only offer opens the shop");
    }
    bt_core_init(&st, 41, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
    {
        st.wave = 3;
        st.crate_kind = BT_SHOP_NONE;
        st.crate_extra_key[0] = BT_ISTR_ITEM_PEARL;
        st.crate_extra_count = 1;
        bt_progress_next_crate_offer(&st);
        check(st.crate_kind == BT_SHOP_ITEM && (int)st.crate_id == PEARL,
              "an explicit extra key becomes its own pearl offer");
        bt_progress_crate_take(&st);
        check(st.item_owned[PEARL] == 1, "the explicit extra item grants a pearl");
        for (int k = 0; k < 19; k++) give(st, PEARL);
        check(st.item_owned[PEARL] == 20, "pearls reach max_nb 20");
        st.crate_kind = BT_SHOP_NONE;
        st.crate_extra_key[0] = BT_ISTR_ITEM_PEARL;
        st.crate_extra_count = 1;
        bt_progress_next_crate_offer(&st);
        check(st.crate_kind == BT_SHOP_NONE, "an already-maxed extra item is dropped");
    }

    /* ---- G6 candy bag: source zone elite at Go -------------------------- */
    bt_core_init(&st, 43, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
    give(st, item_by_name("candy_bag"));
    check(near(st.scalar_effect[BT_ISTR_EXTRA_ELITE_NEXT_WAVE_CHANCE], 10.0f),
          "candy bag adds 10% extra-elite chance");
    {
        int nel = (int)BT_ZONE_NELITES[st.zone];
        check(nel > 0, "zone has an elite roster");
        st.phase = 1; st.shop_timer = 0; st.wave = 4; st.materials = 0.0f;
        /* force the chance roll to succeed (draw still consumed at 1.0) */
        st.scalar_effect[BT_ISTR_EXTRA_ELITE_NEXT_WAVE_CHANCE] = 100.0f;
        bt_progress_shop_action(&st, 8);
        bt_progress_shop_go(&st);   /* the deferred change_scene half */
        check(st.wave == 5, "Go advances the wave");
        check(st.pending_extra_elite == -1, "pending extra elite drained at wave start");
        check(st.n_pend_boss == 1, "extra elite queued through the birth lifecycle");
        int qk = st.n_pend_boss > 0 ? (int)st.pend_boss[st.n_pend_boss - 1].kind : -1;
        int ok = 0;
        for (int i = 0; i < nel; i++) if ((int)BT_ZONE_ELITES[st.zone][i] == qk) ok = 1;
        check(ok, "queued kind is one of the zone's elites");
    }
    /* no candy bag -> the chance is 0 and nothing is rolled or queued */
    bt_core_init(&st, 47, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
    st.phase = 1; st.shop_timer = 0; st.wave = 4; st.materials = 0.0f;
    bt_progress_shop_action(&st, 8);
    check(st.pending_extra_elite == -1 && st.n_pend_boss == 0,
          "no candy bag -> no extra elite");

    /* random zone elite selection is a roster member (source pick_random) */
    bt_core_init(&st, 53, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
    {
        int nel = (int)BT_ZONE_NELITES[st.zone];
        int e = bt_progress_random_elite(&st);
        int ok = 0;
        for (int i = 0; i < nel; i++) if ((int)BT_ZONE_ELITES[st.zone][i] == e) ok = 1;
        check(ok, "random elite is a member of the zone roster");
    }

    if (failures) {
        std::fprintf(stderr, "%d brotato shop-event check(s) failed\n", failures);
        return 1;
    }
    std::printf("brotato shop-event tests passed\n");
    return 0;
}
