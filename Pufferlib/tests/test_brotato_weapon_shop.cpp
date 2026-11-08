/* Observable behaviour of the weapon shop slice (brotato_weapon_shop.h +
 * the item/weapon mixture cutover in brotato_progression.h):
 *   - Bull stays item-only, zero weapons, zero weapon offers (default profile)
 *   - Knight: 6 slots, wave 1-2 guarantee 2 weapon offers, no ranged weapons,
 *     min weapon tier 1
 *   - Demon: max-HP currency (price = ceil(gold/20)), recycle pays gold
 *   - recycle (actions 9..14) and combine (actions 15..20) on OWNED slots
 *   - full-slot auto-combination on buy
 *   - offer slots are mutually distinct; canonical kind/id stay consistent
 *
 * Included through brotato_core.h exactly like the slice headers, so every
 * helper (bt_core_init, bt_progress_open_shop, bt_weapon_*, bt_wshop_*) is the
 * one the real core uses. No RNG value is asserted: every check is a source
 * invariant (guarantee count, tier/kind domain, currency arithmetic) or an
 * exact transaction identity.
 */
#include "../ocean/brotato/brotato_core.h"
#include <cstdio>
#include <cmath>
#include <cstring>

static int failures;
static void check(bool ok, const char *msg) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", msg); failures++; }
}
static bool near(float a, float b, float eps = 1e-2f) { return std::fabs(a - b) < eps; }

static BtState st;
static BtState ref;   /* 6.7 MB: never on the stack */

/* dynamic weapon discovery: never hard-code a row index */
static int w_melee = -1, w_ranged = -1, w_up = -1, w_up_to = -1;
static void discover(void) {
    int any_up = -1, any_up_to = -1;
    for (int i = 0; i < BT_NWEAPONS; i++) {
        if (!BT_WEAPONS[i].can_be_looted) continue;
        if (w_melee < 0 && BT_WEAPONS[i].kind == BT_WEAPON_KIND_MELEE) w_melee = i;
        if (w_ranged < 0 && BT_WEAPONS[i].kind == BT_WEAPON_KIND_RANGED) w_ranged = i;
        if (BT_WEAPONS[i].upgrades_into == BT_WEAPON_NONE) continue;
        if (any_up < 0) { any_up = i; any_up_to = (int)BT_WEAPONS[i].upgrades_into; }
        /* prefer a MELEE, tier >= 1 upgradable weapon: playable by Knight */
        if (w_up < 0 && BT_WEAPONS[i].kind == BT_WEAPON_KIND_MELEE && BT_WEAPONS[i].tier >= 1) {
            w_up = i; w_up_to = (int)BT_WEAPONS[i].upgrades_into;
        }
    }
    if (w_up < 0) { w_up = any_up; w_up_to = any_up_to; }
}

static void init(unsigned short character, bt_u64 seed) {
    bt_core_init(&st, seed, 0, 0, character, BT_WEAPON_NONE);
}

int main(void) {
    discover();
    check(w_melee >= 0, "a lootable melee weapon exists");
    check(w_ranged >= 0, "a lootable ranged weapon exists");
    check(w_up >= 0 && w_up_to >= 0 && w_up_to < BT_NWEAPONS, "an upgradable weapon + its target exist");

    /* --- 1) Bull default: one weapon-less profile, item-only shop ---------- */
    check(bt_character_weapon_slots(BT_CHARACTER_BULL) == 0, "Bull has 0 weapon slots");
    init(BT_CHARACTER_BULL, 1234);
    check(st.character == BT_CHARACTER_BULL, "profile character is Bull");
    check(bt_weapon_count(&st) == 0, "Bull starts with no weapons");
    for (bt_u64 s = 1; s <= 40; s++) {
        init(BT_CHARACTER_BULL, s);
        bt_progress_open_shop(&st);
        if (st.phase != 1) { check(false, "shop opened for Bull"); break; }
        for (int i = 0; i < 4; i++) {
            check(st.shop_kind[i] == BT_SHOP_ITEM, "Bull shop offer is always an item");
            check(st.shop_id[i] < BT_NITEMS, "Bull item id in range");
        }
        check(st.shop_available == 15, "Bull shop fills all four slots");
        for (int a = 0; a < 4; a++)
            for (int b = a + 1; b < 4; b++)
                check(st.shop_id[a] != st.shop_id[b], "offers are mutually distinct");
    }
    check(bt_wshop_hp_shop(&st) == 0, "Bull pays gold");

    /* --- 2) Knight profile guarantees + eligibility ------------------------ */
    check(bt_character_weapon_slots(BT_CHARACTER_KNIGHT) == 6, "Knight has 6 weapon slots");
    check(bt_character_no_ranged(BT_CHARACTER_KNIGHT) == 1, "Knight bans ranged weapons");
    check(bt_character_min_weapon_tier(BT_CHARACTER_KNIGHT) == 1, "Knight min weapon tier is 1");
    check(bt_character_hp_shop(BT_CHARACTER_KNIGHT) == 0, "Knight pays gold");

    int wave4_weapons = 0, wave4_items = 0;
    for (bt_u64 s = 1; s <= 60; s++) {
        init(BT_CHARACTER_KNIGHT, s);
        st.scalar_effect[BT_ISTR_WEAPON_SLOT] = 6.0f;
        st.wave = 1;
        bt_progress_open_shop(&st);
        int wcount = 0;
        for (int i = 0; i < 4; i++) {
            if (st.shop_kind[i] != BT_SHOP_WEAPON) continue;
            wcount++;
            check((int)BT_WEAPONS[st.shop_id[i]].tier >= 1, "Knight weapon offer respects min_weapon_tier");
            check(BT_WEAPONS[st.shop_id[i]].kind == BT_WEAPON_KIND_MELEE, "Knight never offered a ranged weapon");
        }
        check(wcount >= 2, "wave <= 2 guarantees at least two weapon offers");
        for (int a = 0; a < 4; a++)
            for (int b = a + 1; b < 4; b++)
                if (st.shop_kind[a] == BT_SHOP_WEAPON && st.shop_kind[b] == BT_SHOP_WEAPON)
                    check(st.shop_id[a] != st.shop_id[b], "weapon offers are mutually distinct");

        /* wave 4: no guarantee, both kinds should appear across the sample */
        init(BT_CHARACTER_KNIGHT, s);
        st.scalar_effect[BT_ISTR_WEAPON_SLOT] = 6.0f;
        st.wave = 4;
        bt_progress_open_shop(&st);
        for (int i = 0; i < 4; i++) {
            if (st.shop_kind[i] == BT_SHOP_WEAPON) {
                wave4_weapons++;
                check(BT_WEAPONS[st.shop_id[i]].kind == BT_WEAPON_KIND_MELEE, "wave 4 weapon offer is melee-only");
                check((int)BT_WEAPONS[st.shop_id[i]].tier >= 1, "wave 4 weapon offer tier >= 1");
            } else if (st.shop_kind[i] == BT_SHOP_ITEM) wave4_items++;
        }
    }
    check(wave4_weapons > 0, "weapon-capable profiles do get weapon offers");
    check(wave4_items > 0, "the mixture still offers items");

    /* Knight cannot buy a ranged weapon; the gate is observable */
    init(BT_CHARACTER_KNIGHT, 5);
    st.scalar_effect[BT_ISTR_WEAPON_SLOT] = 6.0f;
    check(bt_wshop_can_buy(&st, (unsigned short)w_ranged) == 0, "Knight cannot buy a ranged weapon");

    /* --- 3) Demon trades in max HP ---------------------------------------- */
    check(bt_character_hp_shop(BT_CHARACTER_DEMON) == 1, "Demon pays max HP");
    check(bt_character_weapon_slots(BT_CHARACTER_DEMON) == 6, "Demon has 6 weapon slots");
    init(BT_CHARACTER_DEMON, 7);
    st.scalar_effect[BT_ISTR_WEAPON_SLOT] = 6.0f;
    bt_core_stat_add(&st, 0, 200.0f);                 /* plenty of currency */
    check(near(bt_wshop_currency(&st), st.max_hp), "Demon currency is max HP");
    {
        float hp0 = st.max_hp, mat0 = st.materials;
        bt_wshop_pay(&st, 3.0f);
        check(near(st.materials, mat0), "Demon pay never touches gold");
        check(near(st.max_hp, hp0 - 3.0f), "Demon pay removes exactly the max-HP cost");
    }
    /* price identity: hp units == ceil(gold/20) */
    {
        float gold_item = bt_shop_price(&st, 0);
        check(near(bt_wshop_price_units(&st, BT_SHOP_ITEM, 0), bt_ceilf(gold_item / 20.0f)),
              "Demon item price is ceil(gold/20)");
        float gold_weap = (float)bt_weapon_price(&st, (unsigned short)w_melee);
        check(near(bt_wshop_price_units(&st, BT_SHOP_WEAPON, (unsigned short)w_melee),
                   bt_ceilf(gold_weap / 20.0f)), "Demon weapon price is ceil(gold/20)");
    }
    /* a full Demon purchase spends HP, not gold (only asserted with an
     * effect-free item, so the delta is purely the price) */
    {
        int clean = -1;   /* an item with no stat effects: only its price moves */
        for (int i = 0; i < BT_NITEMS; i++)
            if (BT_ITEMS[i].eff_count == 0 && BT_ITEMS[i].max_nb != 0) { clean = i; break; }
        if (clean < 0) {
            std::printf("weapon_shop: note: no effect-free item; full Demon purchase delta skipped\n");
        } else {
            init(BT_CHARACTER_DEMON, 21);
            st.scalar_effect[BT_ISTR_WEAPON_SLOT] = 6.0f;
            st.phase = 1; st.shop_timer = 0; st.shop_available = 1;
            st.shop_kind[0] = BT_SHOP_ITEM;
            st.shop_id[0] = (unsigned short)clean;
            st.shop_cursed[0] = 0;
            bt_core_stat_add(&st, 0, 400.0f);
            float units = bt_wshop_price_units(&st, BT_SHOP_ITEM, (unsigned short)clean);
            float hp0 = st.max_hp, mat0 = st.materials;
            bt_progress_shop_action(&st, 0);
            check(st.shop_available == 0 && st.shop_kind[0] == BT_SHOP_NONE, "Demon purchase consumes the offer");
            check(near(st.materials, mat0), "Demon purchase leaves gold untouched");
            check(near(st.max_hp, hp0 - units, 0.5f), "Demon purchase removed exactly the max-HP cost");
        }
    }

    /* --- 4) recycle (actions 9..14) --------------------------------------- */
    init(BT_CHARACTER_KNIGHT, 11);
    st.scalar_effect[BT_ISTR_WEAPON_SLOT] = 6.0f;
    check(bt_weapon_add(&st, (unsigned short)w_melee, 0, 0.0f) == 0, "weapon added to slot 0");
    check(bt_weapon_count(&st) == 1, "one weapon owned");
    {
        st.phase = 0; st.shop_timer = 0;
        bt_progress_shop_action(&st, 9);
        check(bt_weapon_count(&st) == 1, "recycle is a no-op outside the shop");
    }
    st.phase = 1; st.shop_timer = 0;
    {
        int expect = bt_weapon_recycle_value(&st, 0);
        float mat0 = st.materials;
        bt_progress_shop_action(&st, 9);
        check(bt_weapon_count(&st) == 0, "recycle removes the owned weapon");
        check(near(st.materials, mat0 + (float)expect), "recycle credits its gold value");
    }
    {
        /* Demon: recycling credits gold, never max HP */
        init(BT_CHARACTER_DEMON, 12);
        st.scalar_effect[BT_ISTR_WEAPON_SLOT] = 6.0f;
        (void)bt_weapon_add(&st, (unsigned short)w_melee, 0, 0.0f);
        st.phase = 1; st.shop_timer = 0;
        float hp0 = st.max_hp, g0 = st.materials;
        bt_progress_shop_action(&st, 9);
        check(near(st.max_hp, hp0), "Demon recycle never touches max HP");
        check(st.materials > g0, "Demon recycle credits gold");
    }

    /* --- 5) combine (actions 15..20) -------------------------------------- */
    init(BT_CHARACTER_KNIGHT, 13);
    st.scalar_effect[BT_ISTR_WEAPON_SLOT] = 6.0f;
    (void)bt_weapon_add(&st, (unsigned short)w_up, 0, 0.0f);
    (void)bt_weapon_add(&st, (unsigned short)w_up, 0, 0.0f);
    check(bt_weapon_count(&st) == 2, "two same-id weapons owned");
    st.phase = 1; st.shop_timer = 0;
    check(bt_weapon_can_combine(&st, 0) == 1, "two duplicates are combinable");
    bt_progress_shop_action(&st, 15);
    check(bt_weapon_count(&st) == 1, "combine consumes one weapon");
    check(bt_weapon_id(&st, 0) == (unsigned short)w_up_to, "combine yields upgrades_into");
    bt_progress_shop_action(&st, 15);
    check(bt_weapon_count(&st) == 1, "combine no-ops with no duplicate left");
    {
        /* outside the shop the action does nothing */
        init(BT_CHARACTER_KNIGHT, 14);
        st.scalar_effect[BT_ISTR_WEAPON_SLOT] = 6.0f;
        (void)bt_weapon_add(&st, (unsigned short)w_up, 0, 0.0f);
        (void)bt_weapon_add(&st, (unsigned short)w_up, 0, 0.0f);
        st.phase = 0; st.shop_timer = 0;
        bt_progress_shop_action(&st, 15);
        check(bt_weapon_count(&st) == 2, "combine is a no-op outside the shop");
    }

    /* --- 6) full-slot auto-combination on buy ----------------------------- */
    init(BT_CHARACTER_KNIGHT, 17);
    st.scalar_effect[BT_ISTR_WEAPON_SLOT] = 6.0f;
    check(bt_weapon_slot_cap(&st) == 6, "Knight slot cap is 6");
    for (int i = 0; i < 6; i++) bt_weapon_add(&st, (unsigned short)w_up, 0, 0.0f);
    check(bt_weapon_count(&st) == 6, "loadout filled to the cap");
    check(bt_weapon_slot_available(&st, (unsigned short)w_up) == 0, "no free slot for the same family");
    check(bt_wshop_can_buy(&st, (unsigned short)w_up) == 1, "same-id purchase may auto-combine");
    {
        st.phase = 1; st.shop_timer = 0;
        check(bt_wshop_buy(&st, (unsigned short)w_up, 0) >= 0, "auto-combining buy succeeds");
        check(bt_weapon_count(&st) == 6, "auto-combine keeps the loadout at the cap");
        int nup = 0, nold = 0;
        for (int i = 0; i < bt_weapon_count(&st); i++) {
            unsigned short id = bt_weapon_id(&st, i);
            if (id == (unsigned short)w_up_to) nup++;
            else if (id == (unsigned short)w_up) nold++;
        }
        check(nup == 1 && nold == 5, "auto-combine leaves one upgraded and five same-id weapons");
    }
    /* a max-tier weapon with no upgrades cannot be bought into a full slot */
    {
        int maxed = -1;
        for (int i = 0; i < BT_NWEAPONS; i++)
            if (BT_WEAPONS[i].can_be_looted && BT_WEAPONS[i].kind == BT_WEAPON_KIND_MELEE &&
                BT_WEAPONS[i].tier >= 1 && BT_WEAPONS[i].upgrades_into == BT_WEAPON_NONE) { maxed = i; break; }
        if (maxed >= 0) {
            init(BT_CHARACTER_KNIGHT, 18);
            st.scalar_effect[BT_ISTR_WEAPON_SLOT] = 6.0f;
            for (int i = 0; i < 6; i++) bt_weapon_add(&st, (unsigned short)maxed, 0, 0.0f);
            check(bt_wshop_can_buy(&st, (unsigned short)maxed) == 0,
                  "a full loadout cannot buy a non-upgradable weapon");
        }
    }

    /* --- 7) Demon end-of-wave gold -> max-HP conversion (main.gd:1225) ----- */
    {
        init(BT_CHARACTER_DEMON, 31);
        st.materials = 260.0f;                 /* 50% = 130; chunks = 130/13 = 10 */
        float hp0 = st.max_hp;
        bt_progress_end_wave(&st);
        check(near(st.materials, 130.0f), "Demon converts half its gold in chunks of 13");
        check(near(st.max_hp, hp0 + 10.0f), "each converted chunk adds one max HP");

        init(BT_CHARACTER_KNIGHT, 32);
        st.materials = 260.0f;
        float hp1 = st.max_hp;
        bt_progress_end_wave(&st);
        check(near(st.materials, 260.0f) && near(st.max_hp, hp1), "no conversion for a non-converting profile");
    }

    /* --- 8) combine / recycle take the sound draws ------------------------ *
     * base_shop.gd:703/744 SoundManager.play(get_rand_element(...),0,0.1,true):
     * 1 sound pick (shop.tscn:59-60 exports one sound each) + the clip's
     * playback pitch draw sound_manager_parent.gd:45 `1.0 + rand_range(-0.1,
     * 0.1)` = 3 draws, consumed at the submit action (the shop phase is
     * action-abstracted and the shared audio queue is stepped only in combat).
     * The action must add EXACTLY those 4 draws on top of the mutation. */
    {
        init(BT_CHARACTER_KNIGHT, 21);
        st.scalar_effect[BT_ISTR_WEAPON_SLOT] = 6.0f;
        (void)bt_weapon_add(&st, (unsigned short)w_up, 0, 0.0f);
        (void)bt_weapon_add(&st, (unsigned short)w_up, 0, 0.0f);
        st.phase = 1; st.shop_timer = 0;
        ref = st;
        (void)bt_weapon_combine(&ref, 0);
        BtRng r1 = ref.rng;
        for (int i = 0; i < 4; i++) (void)bt_rng_next(&r1);
        bt_progress_shop_action(&st, 15);
        check(st.rng.state == r1.state && st.rng.inc == r1.inc,
              "a combine adds exactly the sound pick + pitch draws (4)");

        init(BT_CHARACTER_KNIGHT, 22);
        st.scalar_effect[BT_ISTR_WEAPON_SLOT] = 6.0f;
        (void)bt_weapon_add(&st, (unsigned short)w_melee, 0, 0.0f);
        st.phase = 1; st.shop_timer = 0;
        ref = st;
        (void)bt_weapon_remove(&ref, 0);
        r1 = ref.rng;
        for (int i = 0; i < 4; i++) (void)bt_rng_next(&r1);
        bt_progress_shop_action(&st, 9);
        check(st.rng.state == r1.state && st.rng.inc == r1.inc,
              "a recycle adds exactly the sound pick + pitch draws (4)");
        check(bt_weapon_count(&st) == 0, "the recycled weapon is gone");
    }

    /* --- 8b) a SHOP purchase takes NO global draw -------------------------- *
     * base_shop.gd's buy handler adds the copy to the persistent RunData loadout
     * (run_data.gd:905-915 add_weapon).  The shop scene has no Player node, so no
     * Weapon node exists there and Weapon._ready's get_next_cooldown(true) draw
     * belongs to the NEXT wave's scene build (player.gd:315-320
     * apply_items_effects -> add_weapon, which this core runs as
     * bt_weapon_wave_begin).  Drawing it at the purchase charged the shop 3 words
     * early: on the faithful v5_demon_s1 rng-diag reproduction the game's tick
     * 306 (a successful weapon buy) consumed 0 global words while this core
     * consumed 3 (one rand_range), shifting every later spawn position. */
    {
        init(BT_CHARACTER_KNIGHT, 23);
        st.scalar_effect[BT_ISTR_WEAPON_SLOT] = 6.0f;
        bt_progress_open_shop(&st);
        st.phase = 1; st.shop_timer = 0;
        st.shop_kind[0] = BT_SHOP_WEAPON;
        st.shop_id[0] = (unsigned short)w_up;
        st.shop_cursed[0] = 0;
        st.shop_available = (unsigned char)(st.shop_available | 1u);
        st.materials = 1000000.0f;
        int before = bt_weapon_count(&st);
        BtRng r1 = st.rng;
        (void)bt_progress_shop_action(&st, 0);
        check(bt_weapon_count(&st) == before + 1, "the weapon offer is bought");
        check(st.shop_kind[0] == BT_SHOP_NONE, "the bought offer slot is cleared");
        check(st.rng.state == r1.state && st.rng.inc == r1.inc,
              "a shop purchase takes NO global draw (the Weapon node's cooldown draw "
              "belongs to the next wave's scene build)");
        check(st.wpn_cd[before] == 0.0f,
              "no runtime state is created for a shop-bought copy (cooldown stays 0)");
        /* the copy still gets its runtime (and its ONE cooldown draw) when the
         * next wave builds the arena scene */
        float dur = st.wave_duration;
        BtRng r2 = st.rng;
        bt_enter_wave(&st, st.wave + 1);
        check(st.wpn_id[before] == (unsigned short)w_up, "the copy survives into the wave");
        check(st.rng.state != r2.state, "the wave's scene build draws the copy's cooldown");
        (void)dur;
    }

    /* --- 8c) a shop offer's price is FROZEN at its creation wave ---------- *
     * shop_item.gd:129-140 set_shop_item(path, p_wave_value = current_wave)
     * stores `value = ItemService.get_value(wave_value, ...)` when the offer's
     * ShopItem node is created, so the shop keeps ITS OWN prices; the GO frame
     * has already advanced RunData.current_wave while the shop is still on
     * screen (v5_demon_s1/s2/s3 row 617: the reference keeps the wave-2 prices
     * [0,1,2,1] where a live recompute gives [0,2,2,2]). */
    {
        init(BT_CHARACTER_KNIGHT, 31);      /* gold currency: prices move per wave */
        st.scalar_effect[BT_ISTR_WEAPON_SLOT] = 6.0f;
        bt_progress_open_shop(&st);
        float obs[BT_OBS_SIZE];
        bt_core_observe(&st, obs);
        float frozen[4];
        int live = 0, differs = 0;
        for (int i = 0; i < 4; i++) {
            frozen[i] = obs[142 + 4 * i];
            if (st.shop_kind[i] != BT_SHOP_NONE) live++;
        }
        check(live > 0, "the shop rolled at least one offer");
        /* the offer nodes were created at this shop's wave */
        for (int i = 0; i < 4; i++)
            if (st.shop_kind[i] != BT_SHOP_NONE)
                check(st.shop_price_wave[i] == st.wave,
                      "a fresh offer is stamped with the wave it was created at");
        st.wave += 5;                       /* the GO frame's current_wave advance */
        bt_core_observe(&st, obs);
        for (int i = 0; i < 4; i++) {
            if (st.shop_kind[i] == BT_SHOP_NONE) continue;
            check(obs[142 + 4 * i] == frozen[i],
                  "advancing the wave does not re-price an offer of the still-open shop");
            check(obs[142 + 4 * i] == bt_shop_slot_price(&st, i),
                  "the observed offer price is the STORED slot price");
            check(bt_wshop_price_units(&st, (int)st.shop_kind[i], st.shop_id[i]) != frozen[i],
                  "control: the live current-wave price DID move (the check can fail)");
            differs++;
        }
        check(differs > 0, "the control offers were compared");
    }

    /* --- 9) shop entry, once per run: the anvil's upgrade_random_weapon ---- *
     * base_shop.gd:53-82 runs only while RunData.shop_effects_checked is false
     * (the run's FIRST shop entry). anvil_effect_0.tres: KEY_VALUE, key
     * stat_armor, value 2, custom_key upgrade_random_weapon -> the bucket must
     * be registered instead of an immediate +2 armor. */
    {
        int anvil = -1;
        for (int i = 0; i < BT_NITEMS; i++)
            if (BT_ITEMS[i].name && std::strcmp(BT_ITEMS[i].name, "anvil") == 0) { anvil = i; break; }
        check(anvil >= 0, "the anvil item exists");
        if (anvil >= 0) {
            init(BT_CHARACTER_KNIGHT, 31);
            st.scalar_effect[BT_ISTR_WEAPON_SLOT] = 6.0f;
            (void)bt_weapon_add(&st, (unsigned short)w_up, 0, 0.0f);
            float a0 = st.armor;
            st.curse_apply = 0;
            bt_apply_item(&st, (unsigned short)anvil);
            check(near(st.armor, a0), "an uncursed anvil adds no armor on acquisition");
            check(st.shop_effects_checked == 0, "the once-per-run gate starts open");
            bt_wshop_shop_entry(&st);
            check(st.shop_effects_checked == 1, "the gate closes after the first shop entry");
            check(bt_weapon_id(&st, 0) == (unsigned short)w_up_to,
                  "the anvil upgraded the owned weapon");
            (void)bt_weapon_add(&st, (unsigned short)w_up, 0, 0.0f);
            bt_wshop_shop_entry(&st);
            check(bt_weapon_count(&st) == 2 && bt_weapon_id(&st, 1) == (unsigned short)w_up,
                  "a second shop entry does not upgrade again");
            init(BT_CHARACTER_BULL, 32);
            st.curse_apply = 0;
            bt_apply_item(&st, (unsigned short)anvil);
            float b0 = st.armor;
            bt_wshop_shop_entry(&st);
            check(near(st.armor, b0 + 2.0f), "with no upgradable weapon the anvil grants +2 armor");
        }
    }

    std::printf("weapon_shop: %d failures\n", failures);
    return failures ? 1 : 0;
}
