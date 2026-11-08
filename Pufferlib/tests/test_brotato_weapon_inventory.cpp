/* tests/test_brotato_weapon_inventory.cpp
 *
 * Source-backed tests for the shared six-slot weapon loadout
 * (ocean/brotato/brotato_weapon_inventory.h).
 *
 *   run_data.gd:905-1022   add_weapon / remove_weapon / remove_weapon_by_index
 *                          / remove_all_weapons / has_weapon_slot_available
 *                          / can_combine / get_unique_weapon_ids
 *   base_shop.gd:602-748   buy_weapon, _combine_weapon, discard -> recycle value
 *   item_service.gd:701-742 get_value / get_recycling_value, tier clamp,
 *                          no_ranged_weapons pool filter
 *   dlc_1_data.gd:48-357   curse_item / _get_cursed_item_effect_modifier
 *   bull_effect_5.tres     weapon_slot 0 -> Bull stays weaponless
 *
 * These are observable-behaviour tests: they drive the public API on a real
 * BtState and check the resulting loadout / values / explicit overflow flags,
 * deriving every weapon id from the generated BT_WEAPONS table (no hardcoded
 * row indices, no source-text assertions).
 *
 * Build and run:
 *     c++ -O2 -o build/test_brotato_weapon_inventory \
 *         tests/test_brotato_weapon_inventory.cpp -lm
 *     ./build/test_brotato_weapon_inventory
 *
 * Exit code 0 = all checks passed.
 */

#include "../ocean/brotato/brotato_core.h"
#include "../ocean/brotato/brotato_weapon_inventory.h"

#include <stdio.h>

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
static void check_eq(int got, int want, const char* msg) {
    g_checks++;
    if (got != want) { g_failures++; printf("  FAIL: %s (got %d want %d)\n", msg, got, want); }
}
/* Abort a test early (recorded as a failure) when a generated-table
 * precondition the test drives from is missing. */
static int require_id(int id, const char* what) {
    if (id >= 0) return 1;
    check(0, what);
    return 0;
}

/* BtState embeds two physics worlds; keep the big state static. */
static BtState S;

/* ---- fresh state: zero every scalar this module reads, clear the loadout ---- */
static void fresh(unsigned int seed) {
    bt_rng_seed(&S.rng, (bt_u64)seed);
    bt_rng_seed(&S.utils_rng, (bt_u64)seed + 1u);
    S.scalar_effect[BT_ISTR_WEAPON_SLOT] = 0.0f;
    S.scalar_effect[BT_ISTR_MIN_WEAPON_TIER] = 0.0f;
    S.scalar_effect[BT_ISTR_MAX_WEAPON_TIER] = 99.0f;
    S.scalar_effect[BT_ISTR_MAX_MELEE_WEAPONS] = 0.0f;    /* -> source default 999 */
    S.scalar_effect[BT_ISTR_MAX_RANGED_WEAPONS] = 0.0f;   /* -> source default 999 */
    S.scalar_effect[BT_ISTR_NO_RANGED_WEAPONS] = 0.0f;
    S.scalar_effect[BT_ISTR_NO_DUPLICATE_WEAPONS] = 0.0f;
    S.scalar_effect[BT_ISTR_WEAPONS_PRICE] = 0.0f;
    S.scalar_effect[BT_ISTR_ITEMS_PRICE] = 0.0f;
    S.scalar_effect[BT_ISTR_RECYCLING_GAINS] = 0.0f;
    S.wave = 1;
    bt_weapon_reset(&S);
}

/* ---- table-driven weapon lookups (no fixed row indices) ---- */
/* First tier-0 variant that upgrades into a tier-1 variant. */
static int find_t0_upgradable(void) {
    for (int i = 0; i < BT_NWEAPONS; i++)
        if (BT_WEAPONS[i].tier == 0 && BT_WEAPONS[i].upgrades_into != BT_WEAPON_NONE)
            return i;
    return -1;
}
static int find_t0_of_kind(int kind) {
    for (int i = 0; i < BT_NWEAPONS; i++)
        if (BT_WEAPONS[i].tier == 0 && (int)BT_WEAPONS[i].kind == kind) return i;
    return -1;
}
static int find_of_kind(int kind, int skip_tier0) {
    for (int i = 0; i < BT_NWEAPONS; i++)
        if ((int)BT_WEAPONS[i].kind == kind && (!skip_tier0 || BT_WEAPONS[i].tier != 0)) return i;
    return -1;
}
/* Top of a variant's upgrade chain. */
static int chain_top(int id) {
    int guard = 0;
    while (BT_WEAPONS[id].upgrades_into != BT_WEAPON_NONE && guard++ < BT_NWEAPONS)
        id = (int)BT_WEAPONS[id].upgrades_into;
    return id;
}
/* First row whose variant owns exactly `n` top-level effects. */
static int find_fx(int n) {
    for (int i = 0; i < BT_NWEAPONS; i++) if ((int)BT_WEAPONS[i].fx_count == n) return i;
    return -1;
}
/* First row with `n` top-level effects whose upgrade also owns `n` (so a
 * combine's curse draws are predictable in count and order). */
static int find_fx_pair(int n, int* up_out) {
    for (int i = 0; i < BT_NWEAPONS; i++) {
        unsigned short up = BT_WEAPONS[i].upgrades_into;
        if ((int)BT_WEAPONS[i].fx_count != n || up == BT_WEAPON_NONE) continue;
        if ((int)BT_WEAPONS[up].fx_count != n) continue;
        *up_out = (int)up;
        return i;
    }
    return -1;
}
/* First variant that belongs to a set whose tier table is non-empty (so two
 * copies reach the source's tier-0 threshold). */
static int find_set_variant(int* set_out) {
    for (int i = 0; i < BT_NWEAPONS; i++) {
        if (BT_WEAPONS[i].set_count == 0) continue;
        for (int k = 0; k < (int)BT_WEAPONS[i].set_count; k++) {
            int s = (int)BT_WEAPON_SET_IDS[(int)BT_WEAPONS[i].set_first + k];
            if (s >= 0 && s < BT_NWEAPON_SETS && BT_WEAPON_SETS[s].nb_tiers >= 2) {
                *set_out = s;
                return i;
            }
        }
    }
    return -1;
}

/* First row with value > 1 (a non-trivial price/recycle). */
static int find_valued(void) {
    for (int i = 0; i < BT_NWEAPONS; i++) if (BT_WEAPONS[i].value > 1) return i;
    return -1;
}
/* `n` distinct ids starting at `from` (wrapping through the table). */
static int distinct_ids(int from, int n, int* out) {
    int got = 0;
    for (int k = 0; k < BT_NWEAPONS && got < n; k++) {
        int id = (from + k) % BT_NWEAPONS;
        out[got++] = id;
    }
    return got;
}
/* An id whose family differs from every id in `used`. */
static int other_family(const int* used, int nused) {
    for (int i = 0; i < BT_NWEAPONS; i++) {
        int clash = 0;
        for (int j = 0; j < nused; j++)
            if (BT_WEAPONS[i].family == BT_WEAPONS[used[j]].family) { clash = 1; break; }
        if (!clash) return i;
    }
    return -1;
}

/* Expected item_service.gd:701-719 get_value for wave 1 (no endless term). */
static int expect_price_wave1_mod(int base, float weapons_price) {
    float v = (float)base * (1.0f + weapons_price / 100.0f);   /* weapons_price factor */
    float p = v + 1.0f + v * 1.0f * 0.1f;                      /* + wave + wave*10% */
    int r = (int)p;                                            /* `as int` truncates */
    return r < 1 ? 1 : r;
}
static int expect_price_wave1(int base) { return expect_price_wave1_mod(base, 0.0f); }
/* Expected recycle value from a list price (item_service.gd:732-742 tail). */
static int expect_recycle_from(int shop_value, float recycling_gains) {
    float f = 0.25f + recycling_gains / 100.0f;
    if (f < 0.01f) f = 0.01f;
    if (f > 1.0f) f = 1.0f;
    float rv = (float)shop_value * f;
    if (rv < 1.0f) rv = 1.0f;
    int rec = (int)rv;
    return shop_value < rec ? shop_value : rec;
}
/* Expected item_service.gd:732-742 get_recycling_value(wave=1, is_weapon=true). */
static int expect_recycle_wave1(int base, float recycling_gains) {
    if (base == 1) return 1;                                   /* from_value == 1 shortcut */
    return expect_recycle_from(expect_price_wave1(base), recycling_gains);
}

/* First weapon whose own top-level effect rows carry a SUM `stat_*` row keyed
 * `key`; returns the canonical id and the row's value (+2 for
 * jousting_lance_1's stat_speed, hand_effect_1's stat_harvesting +3, ...). */
static int find_weapon_stat(int key, int* value_out) {
    for (int i = 0; i < BT_NWEAPONS; i++) {
        BT_CONST_PTR(BtWeaponDef) D = &BT_WEAPONS[i];
        for (int k = 0; k < (int)D->fx_count; k++) {
            int idx = (int)D->fx_first + k;
            if (idx >= BT_NWEAPON_EFF_INDEX) break;
            int row = (int)BT_WEAPON_EFF_INDEX[idx];
            if (row < 0 || row >= BT_NITEM_EFF_ROWS) continue;
            BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[row];
            if (E->opcode != BT_ITEMEFF_EFFECT || E->storage != 0) continue;
            if (E->key != (unsigned short)key) continue;
            if (value_out) *value_out = E->value;
            return i;
        }
    }
    return -1;
}

/* ============================ tests ============================ */

/* Bull profile: weapon_slot 0 (bull_effect_5 REPLACE 0) -> no weapon can ever
 * be bought or held. */
static void test_bull_zero_slot(void) {
    fresh(1u);
    check_eq(bt_weapon_slot_cap(&S), 0, "Bull weapon_slot effect 0 -> capacity 0");
    int w = find_t0_upgradable();
    if (!require_id(w, "table needs an upgradable tier-0 weapon")) return;
    check_eq(bt_weapon_slot_available(&S, (unsigned short)w), 0, "Bull: slot_available false");
    check_eq(bt_weapon_buy(&S, (unsigned short)w, 0, 0.0f), -1, "Bull: buy rejected");
    check_eq((int)S.wpn_overflow, 1, "Bull: rejected buy sets wpn_overflow");
    check_eq(bt_weapon_count(&S), 0, "Bull: loadout stays empty");
}

/* Six default slots: a weapon buys into the first free slot. */
static void test_capacity_and_add(void) {
    fresh(2u);
    S.scalar_effect[BT_ISTR_WEAPON_SLOT] = 6.0f;
    check_eq(bt_weapon_slot_cap(&S), 6, "default capacity 6");
    int w = find_t0_upgradable();
    if (!require_id(w, "table needs an upgradable tier-0 weapon")) return;
    check_eq(bt_weapon_slot_available(&S, (unsigned short)w), 1, "empty loadout accepts a weapon");
    int slot = bt_weapon_add(&S, (unsigned short)w, 0, 0.0f);
    check_eq(slot, 0, "first weapon lands in slot 0");
    check_eq(bt_weapon_count(&S), 1, "count 1");
    check_eq((int)bt_weapon_id(&S, 0), w, "slot 0 holds the bought variant");
    check_eq((int)S.wpn_tier[0], (int)BT_WEAPONS[w].tier, "per-slot tier mirrors BT_WEAPONS.tier");
    check(bt_weapon_uid(&S, 0) != BT_WEAPON_UID_NONE, "copy gets a live uid");
    check_eq((int)bt_weapon_uid(&S, 0), 0, "first uid is minted from 0");
    check_eq((int)S.wpn_dirty, 1, "loadout marked dirty");
    check_eq((int)S.wpn_overflow, 0, "no overflow on a legal add");
    /* invalid id is an explicit rejection */
    check_eq(bt_weapon_add(&S, (unsigned short)BT_NWEAPONS, 0, 0.0f), -1, "out-of-range id rejected");
    check_eq((int)S.wpn_overflow, 1, "out-of-range id sets wpn_overflow");
}

/* Dense compaction + stable uid + tail cleared (run_data.gd Array.remove). */
static void test_remove_compaction_and_uid(void) {
    fresh(3u);
    S.scalar_effect[BT_ISTR_WEAPON_SLOT] = 6.0f;
    int ids[3];
    int w0 = find_t0_upgradable();
    if (!require_id(w0, "table needs an upgradable tier-0 weapon")) return;
    check_eq(distinct_ids(w0, 3, ids), 3, "three distinct table ids");
    for (int i = 0; i < 3; i++) bt_weapon_add(&S, (unsigned short)ids[i], 0, 0.0f);
    unsigned int u0 = bt_weapon_uid(&S, 0);
    unsigned int last_uid = bt_weapon_uid(&S, 2);
    check_eq(bt_weapon_remove(&S, 1), ids[1], "remove returns the removed id");
    check_eq(bt_weapon_count(&S), 2, "count drops to 2");
    check_eq((int)bt_weapon_id(&S, 0), ids[0], "slot 0 unchanged");
    check_eq((int)bt_weapon_id(&S, 1), ids[2], "slot 1 shifted down");
    check_eq((int)S.wpn_id[2], (int)BT_WEAPON_NONE, "vacated tail slot id is BT_WEAPON_NONE");
    check(S.wpn_uid[2] == BT_WEAPON_UID_NONE, "vacated tail slot uid cleared");
    check_eq((int)bt_weapon_uid(&S, 1), (int)last_uid, "uid travels with the compacted copy");
    check_eq(bt_weapon_slot_from_uid(&S, last_uid), 1, "uid re-resolves to the new slot");
    check_eq(bt_weapon_slot_from_uid(&S, u0), 0, "first copy uid still resolves");
    /* out-of-range remove is a no-op returning the sentinel */
    check_eq(bt_weapon_remove(&S, 5), (int)BT_WEAPON_NONE, "out-of-range remove -> BT_WEAPON_NONE");
    check_eq(bt_weapon_count(&S), 2, "out-of-range remove changes nothing");
    /* uid never reused */
    unsigned int prev = S.wpn_uid_next;
    bt_weapon_add(&S, (unsigned short)bt_weapon_id(&S, 0), 0, 0.0f);
    check(bt_weapon_uid(&S, bt_weapon_count(&S) - 1) != last_uid, "uid not reused");
    check(S.wpn_uid_next > prev, "uid allocator advances monotonically");
}

/* Hard BT_WEAPON_SLOTS bound + full-loadout eligibility. */
static void test_full_loadout(void) {
    fresh(4u);
    S.scalar_effect[BT_ISTR_WEAPON_SLOT] = 6.0f;
    int ids[6];
    int w0 = find_t0_upgradable();
    if (!require_id(w0, "table needs an upgradable tier-0 weapon")) return;
    check_eq(distinct_ids(w0, 6, ids), 6, "six distinct ids");
    for (int i = 0; i < 6; i++) check_eq(bt_weapon_add(&S, (unsigned short)ids[i], 0, 0.0f), i, "fill slot");
    check_eq(bt_weapon_count(&S), 6, "loadout full");
    int extra = other_family(ids, 6);
    check_eq(bt_weapon_slot_available(&S, (unsigned short)extra), 0, "full loadout: no free slot");
    check_eq(bt_weapon_buy(&S, (unsigned short)extra, 0, 0.0f), -1, "buy with no slot and no duplicate rejected");
    check_eq((int)S.wpn_overflow, 1, "rejected buy sets wpn_overflow");
    check_eq(bt_weapon_count(&S), 6, "rejected buy does not change the loadout");
    check_eq(bt_weapon_add(&S, (unsigned short)extra, 0, 0.0f), -1, "hard 6-slot bound rejects a 7th add");
    check_eq(bt_weapon_count(&S), 6, "hard bound keeps count at 6");
}

/* base_shop.gd:602-615 buy_weapon with a full loadout -> auto-combination. */
static void test_buy_auto_combine(void) {
    fresh(5u);
    S.scalar_effect[BT_ISTR_WEAPON_SLOT] = 6.0f;
    int w = find_t0_upgradable();
    if (!require_id(w, "table needs an upgradable tier-0 weapon")) return;
    int up = (int)BT_WEAPONS[w].upgrades_into;
    int fills[5];
    int got = 0;
    for (int i = 0; i < BT_NWEAPONS && got < 5; i++)
        if (BT_WEAPONS[i].family != BT_WEAPONS[w].family) fills[got++] = i;
    /* two copies of `w` + four others = 6 slots */
    bt_weapon_add(&S, (unsigned short)w, 0, 0.0f);
    bt_weapon_add(&S, (unsigned short)w, 0, 0.0f);
    for (int i = 0; i < 4; i++) bt_weapon_add(&S, (unsigned short)fills[i], 0, 0.0f);
    check_eq(bt_weapon_count(&S), 6, "loadout filled to 6");
    check_eq(bt_weapon_slot_available(&S, (unsigned short)w), 0, "no free slot for the same variant");
    int before_dupes = bt_weapon_copies_of(&S, (unsigned short)w);
    check_eq(before_dupes, 2, "two copies before the auto-combine");
    int slot = bt_weapon_buy(&S, (unsigned short)w, 0, 0.0f);
    check(slot >= 0, "buy auto-combines into an upgraded variant");
    check_eq(bt_weapon_count(&S), 6, "auto-combine keeps the count at 6");
    /* The purchase adds a third copy before combining two (base_shop.gd:610-611). */
    check_eq(bt_weapon_copies_of(&S, (unsigned short)w), 1, "one uncombined base copy survives the purchase");
    check_eq(bt_weapon_copies_of(&S, (unsigned short)up), 1, "exactly one upgraded variant produced");
    check_eq((int)bt_weapon_id(&S, slot), up, "the new copy sits at the returned slot");
    /* buying into a free slot just appends */
    fresh(6u);
    S.scalar_effect[BT_ISTR_WEAPON_SLOT] = 6.0f;
    bt_weapon_add(&S, (unsigned short)fills[0], 0, 0.0f);
    int s2 = bt_weapon_buy(&S, (unsigned short)w, 0, 0.0f);
    check_eq(s2, 1, "buy appends when a slot is free");
    check_eq((int)bt_weapon_id(&S, 1), w, "appended weapon is the bought variant");
}

/* can_combine (run_data.gd:1317-1326) + combine (base_shop.gd:631-703). */
static void test_combine_rules(void) {
    fresh(7u);
    S.scalar_effect[BT_ISTR_WEAPON_SLOT] = 6.0f;
    int w = find_t0_upgradable();
    if (!require_id(w, "table needs an upgradable tier-0 weapon")) return;
    int up = (int)BT_WEAPONS[w].upgrades_into;
    bt_weapon_add(&S, (unsigned short)w, 0, 0.0f);
    check_eq(bt_weapon_can_combine(&S, 0), 0, "a single copy cannot combine");
    check_eq(bt_weapon_combine(&S, 0), -1, "combine without a duplicate fails");
    check_eq(bt_weapon_count(&S), 1, "failed combine restores the source copy");
    check_eq((int)bt_weapon_id(&S, 0), w, "restored copy keeps its identity");
    check_eq((int)S.wpn_overflow, 1, "failed combine sets wpn_overflow (never a silent loss)");
    bt_weapon_add(&S, (unsigned short)w, 0, 0.0f);
    check_eq(bt_weapon_can_combine(&S, 0), 1, "two copies can combine");
    /* highest-tier rule: tier < max_weapon_tier */
    S.scalar_effect[BT_ISTR_MAX_WEAPON_TIER] = (float)BT_WEAPONS[w].tier;
    check_eq(bt_weapon_can_combine(&S, 0), 0, "tier == max_weapon_tier cannot combine");
    S.scalar_effect[BT_ISTR_MAX_WEAPON_TIER] = 99.0f;
    check_eq(bt_weapon_can_combine(&S, 0), 1, "default max_weapon_tier 99 allows the combine");
    S.wpn_tracked[0] = 7;
    S.wpn_tracked[1] = 5;
    int slot = bt_weapon_combine(&S, 0);
    check(slot >= 0, "combine succeeds");
    check_eq(bt_weapon_count(&S), 1, "combine consumes two and adds one");
    check_eq((int)bt_weapon_id(&S, slot), up, "combine produces upgrades_into");
    check_eq((int)S.wpn_tier[slot], (int)BT_WEAPONS[up].tier, "produced copy's tier is the upgraded tier");
    check_eq(S.wpn_tracked[slot], 12, "combine sums the merged copies' tracked_value");
    check_eq((int)S.wpn_cursed[slot], 0, "uncursed + uncursed -> uncursed");
    /* an upgrade that cannot advance is an explicit no-op */
    int top = chain_top(w);
    fresh(8u);
    S.scalar_effect[BT_ISTR_WEAPON_SLOT] = 6.0f;
    bt_weapon_add(&S, (unsigned short)top, 0, 0.0f);
    bt_weapon_add(&S, (unsigned short)top, 0, 0.0f);
    check_eq(bt_weapon_can_combine(&S, 0), 0, "top-tier variant cannot combine");
    check_eq(bt_weapon_combine(&S, 0), -1, "top-tier combine rejected");
    check_eq(bt_weapon_count(&S), 2, "top-tier combine leaves the loadout untouched");
}

/* Per-copy curse is preserved; combine merges to cursed-if-either and takes the
 * stronger floor (base_shop.gd:669-690, dlc_1_data.gd:353-357). */
static void test_curse_merge(void) {
    fresh(9u);
    S.scalar_effect[BT_ISTR_WEAPON_SLOT] = 6.0f;
    int w = find_t0_upgradable();
    if (!require_id(w, "table needs an upgradable tier-0 weapon")) return;
    int up = (int)BT_WEAPONS[w].upgrades_into;
    /* two differently-cursed copies stay distinct */
    bt_weapon_add(&S, (unsigned short)w, 1, 0.50f);
    bt_weapon_add(&S, (unsigned short)w, 1, 0.70f);
    check_near(S.wpn_curse[0], 0.50f, 1e-6f, "copy 0 keeps its own curse modifier");
    check_near(S.wpn_curse[1], 0.70f, 1e-6f, "copy 1 keeps its own curse modifier");
    check_near(S.wpn_wmod[0], 0.50f, 1e-6f, "copy 0 keeps its own weapon-stats modifier");
    check_near(S.wpn_wmod[1], 0.70f, 1e-6f, "copy 1 keeps its own weapon-stats modifier");
    check_eq((int)S.wpn_cursed[0], 1, "copy 0 cursed");
    check_eq((int)S.wpn_cursed[1], 1, "copy 1 cursed");
    bt_weapon_remove(&S, 0);
    check_eq(bt_weapon_count(&S), 1, "one cursed copy removed");
    check_near(S.wpn_curse[0], 0.70f, 1e-6f, "surviving copy's curse is untouched by the removal");
    check_near(S.wpn_wmod[0], 0.70f, 1e-6f, "surviving copy's stats modifier is untouched by the removal");
    check_near(bt_weapon_curse_modifier(&S, 0), 0.70f, 1e-6f, "curse modifier reads the own copy's value");
    check_near(bt_weapon_stats_curse_modifier(&S, 0), 0.70f, 1e-6f, "stats modifier accessor");
    /* cursed + uncursed -> cursed, modifier at least the cursed copy's */
    fresh(10u);
    S.scalar_effect[BT_ISTR_WEAPON_SLOT] = 6.0f;
    bt_weapon_add(&S, (unsigned short)w, 1, 0.80f);
    bt_weapon_add(&S, (unsigned short)w, 0, 0.0f);
    int slot = bt_weapon_combine(&S, 0);
    check(slot >= 0, "cursed + uncursed combines");
    check_eq((int)bt_weapon_id(&S, slot), up, "produced copy is the upgraded variant");
    check_eq((int)S.wpn_cursed[slot], 1, "cursed if either input was cursed");
    check(S.wpn_curse[slot] >= 0.80f, "combine honours the merged curse floor");
    check(S.wpn_wmod[slot] >= 0.80f, "the produced copy's weapon-stats modifier honours the floor too");
    check(S.wpn_curse[slot] >= S.wpn_wmod[slot], "aggregate >= weapon-stats modifier");
    check_near(bt_weapon_curse_modifier(&S, slot), S.wpn_curse[slot], 1e-6f,
               "produced copy's curse modifier is the stored aggregate");
    check_near(bt_weapon_stats_curse_modifier(&S, slot), S.wpn_wmod[slot], 1e-6f,
               "produced copy's stats modifier is the stored weapon draw");
    {
        float agg = S.wpn_wmod[slot];
        int fx = (int)BT_WEAPONS[up].fx_count;
        for (int e = 0; e < fx && e < BT_WEAPON_FX_CURSE_MAX; e++)
            if (S.wpn_fxcurse[slot][e] > agg) agg = S.wpn_fxcurse[slot][e];
        check_near(S.wpn_curse[slot], agg, 1e-6f,
                   "aggregate curse_factor is the max over the weapon and every effect draw");
    }
    /* uncursed + uncursed stays uncursed with modifier 0 */
    fresh(11u);
    S.scalar_effect[BT_ISTR_WEAPON_SLOT] = 6.0f;
    bt_weapon_add(&S, (unsigned short)w, 0, 0.0f);
    bt_weapon_add(&S, (unsigned short)w, 0, 0.0f);
    slot = bt_weapon_combine(&S, 0);
    check_eq((int)S.wpn_cursed[slot], 0, "uncursed copies stay uncursed");
    check_near(S.wpn_curse[slot], 0.0f, 1e-6f, "uncursed copy has no modifier");
}

/* Per-copy curse storage: curse_item draws the weapon-stats modifier FIRST and
 * then one modifier per top-level effect, and the copy's curse_factor is the max
 * over all of them (dlc_1_data.gd:57-67,344-348). */
static void test_curse_state(void) {
    fresh(22u);
    S.scalar_effect[BT_ISTR_WEAPON_SLOT] = 6.0f;
    int w2 = find_fx(2), w0 = find_fx(0);
    if (!require_id(w2, "table needs a weapon with 2 top-level effects")) return;
    if (!require_id(w0, "table needs a weapon with no top-level effects")) return;
    /* turn_randomization_off path: 0.38 everywhere, no draws */
    int s2 = bt_weapon_add(&S, (unsigned short)w2, 1, 0.0f);
    check(s2 >= 0, "cursed add of a 2-effect weapon");
    check_eq((int)S.wpn_cursed[s2], 1, "copy is cursed");
    check_near(S.wpn_wmod[s2], BT_WEAPON_CURSE_DET_MOD, 1e-6f, "deterministic weapon-stats modifier");
    check_near(S.wpn_fxcurse[s2][0], BT_WEAPON_CURSE_DET_MOD, 1e-6f, "deterministic effect 0 modifier");
    check_near(S.wpn_fxcurse[s2][1], BT_WEAPON_CURSE_DET_MOD, 1e-6f, "deterministic effect 1 modifier");
    check_near(S.wpn_curse[s2], BT_WEAPON_CURSE_DET_MOD, 1e-6f, "aggregate of the deterministic path");
    check_near(bt_weapon_stats_curse_modifier(&S, s2), S.wpn_wmod[s2], 1e-6f, "stats modifier accessor");
    check_near(bt_weapon_fx_curse(&S, s2, 1), S.wpn_fxcurse[s2][1], 1e-6f, "effect modifier accessor");
    check_near(bt_weapon_fx_curse(&S, s2, BT_WEAPON_FX_CURSE_MAX), 0.0f, 1e-6f, "out-of-range effect index");
    /* no effects -> the aggregate is exactly the weapon draw */
    int s0 = bt_weapon_add(&S, (unsigned short)w0, 1, 0.0f);
    check_near(S.wpn_curse[s0], S.wpn_wmod[s0], 1e-6f, "0-effect weapon: aggregate == weapon modifier");
    /* caller-baked modifier (a cursed shop offer's value) */
    int s3 = bt_weapon_add(&S, (unsigned short)w2, 1, 0.25f);
    check_near(S.wpn_wmod[s3], 0.25f, 1e-6f, "baked offer modifier stored");
    check_near(S.wpn_fxcurse[s3][1], 0.25f, 1e-6f, "baked offer modifier reaches the effects");
    check_near(S.wpn_curse[s3], 0.25f, 1e-6f, "baked offer aggregate");
    /* randomized combine: exactly (1 + fx_count) utils_rng draws, source order */
    fresh(23u);
    S.scalar_effect[BT_ISTR_WEAPON_SLOT] = 6.0f;
    int up = -1;
    int wx = find_fx_pair(2, &up);
    if (!require_id(wx, "table needs an upgradable weapon whose upgrade also has 2 effects")) return;
    bt_weapon_add(&S, (unsigned short)wx, 1, 0.30f);
    bt_weapon_add(&S, (unsigned short)wx, 1, 0.35f);
    BtRng probe = S.utils_rng;          /* the deterministic adds drew nothing */
    int cs = bt_weapon_combine(&S, 0);
    check(cs >= 0, "curse combine succeeds");
    check_eq((int)S.wpn_cursed[cs], 1, "produced copy is cursed");
    if (cs >= 0) {
        float want[3];
        for (int k = 0; k < 3; k++) {
            int r = bt_rng_randi_range(&probe, -30, 30);
            float p = (BT_DLC1_CURSE_CURSED_ITEM_BASE_PERCENT_MODIFIER +
                       BT_DLC1_CURSE_CURSED_ITEM_PERCENT_MODIFIER_INCREASE_EACH_WAVE * 0.0f +
                       (float)r) / 100.0f;               /* wave 1 -> w = 0 */
            if (p < 0.0f) p = 0.0f;
            if (p < 0.35f) p = 0.35f;                    /* merged floor */
            want[k] = p;
        }
        check_near(S.wpn_wmod[cs], want[0], 1e-6f, "weapon-stats modifier is the 1st curse draw");
        check_near(S.wpn_fxcurse[cs][0], want[1], 1e-6f, "effect 0 modifier is the 2nd curse draw");
        check_near(S.wpn_fxcurse[cs][1], want[2], 1e-6f, "effect 1 modifier is the 3rd curse draw");
        float m = want[0];
        if (want[1] > m) m = want[1];
        if (want[2] > m) m = want[2];
        check_near(S.wpn_curse[cs], m, 1e-6f, "aggregate is the max over the three draws");
        check(S.wpn_curse[cs] >= 0.35f, "merged curse floor honoured by the aggregate");
    }
    /* per-copy curse survives compaction and dies with its copy */
    fresh(24u);
    S.scalar_effect[BT_ISTR_WEAPON_SLOT] = 6.0f;
    bt_weapon_add(&S, (unsigned short)w2, 1, 0.10f);
    bt_weapon_add(&S, (unsigned short)w2, 1, 0.20f);
    bt_weapon_add(&S, (unsigned short)w2, 1, 0.30f);
    unsigned int uid_b = bt_weapon_uid(&S, 2);
    check_eq(bt_weapon_remove(&S, 0), w2, "removed the first cursed copy");
    int rb = bt_weapon_slot_from_uid(&S, uid_b);
    check_eq(rb, 1, "the third copy compacted into slot 1");
    if (rb >= 0) {
        check_near(S.wpn_wmod[rb], 0.30f, 1e-6f, "compacted copy keeps its own weapon-stats modifier");
        check_near(S.wpn_fxcurse[rb][1], 0.30f, 1e-6f, "compacted copy keeps its own effect modifiers");
        check_near(S.wpn_curse[rb], 0.30f, 1e-6f, "compacted copy keeps its own aggregate");
        check_near(S.wpn_wmod[0], 0.20f, 1e-6f, "the other survivor's curse is untouched");
    }
    unsigned int uid_0 = bt_weapon_uid(&S, 0);
    bt_weapon_remove(&S, 0);
    check_eq(bt_weapon_slot_from_uid(&S, uid_0), -1, "a removed copy's curse state goes with it");
}

/* base_shop.gd:650-668: the duplicate consumed is the first same-id copy whose
 * cursed flag differs from the source's, else the last same-id copy. */
static void test_combine_duplicate_pick(void) {
    fresh(12u);
    S.scalar_effect[BT_ISTR_WEAPON_SLOT] = 6.0f;
    int w = find_t0_upgradable();
    if (!require_id(w, "table needs an upgradable tier-0 weapon")) return;
    unsigned short up = BT_WEAPONS[w].upgrades_into;
    /* [cursed, uncursed, cursed] -> combining slot 0 must eat the uncursed one */
    bt_weapon_add(&S, (unsigned short)w, 1, 0.5f);
    bt_weapon_add(&S, (unsigned short)w, 0, 0.0f);
    bt_weapon_add(&S, (unsigned short)w, 1, 0.6f);
    unsigned int keep_uid = bt_weapon_uid(&S, 2);
    unsigned int eaten_uid = bt_weapon_uid(&S, 1);
    int slot = bt_weapon_combine(&S, 0);
    check(slot >= 0, "combine with a cursed-flag-differing duplicate succeeds");
    check_eq(bt_weapon_copies_of(&S, (unsigned short)w), 1, "exactly one base copy survives");
    check_eq(bt_weapon_slot_from_uid(&S, eaten_uid), -1, "the uncursed duplicate was the one consumed");
    check(bt_weapon_slot_from_uid(&S, keep_uid) >= 0, "the other cursed copy survived");
    check_eq((int)bt_weapon_id(&S, slot), (int)up, "an upgraded variant was produced");
    check_eq((int)S.wpn_cursed[slot], 1, "the produced copy is cursed");
}

/* run_data.gd:1262-1280 no_duplicate_weapons / max_melee|ranged_weapons. */
static void test_slot_availability_rules(void) {
    fresh(13u);
    S.scalar_effect[BT_ISTR_WEAPON_SLOT] = 6.0f;
    int melee = find_t0_of_kind(BT_WEAPON_KIND_MELEE);
    int ranged = find_of_kind(BT_WEAPON_KIND_RANGED, 0);
    int melee2 = -1;
    if (melee >= 0)
        for (int i = 0; i < BT_NWEAPONS; i++)
            if ((int)BT_WEAPONS[i].kind == BT_WEAPON_KIND_MELEE &&
                BT_WEAPONS[i].family != BT_WEAPONS[melee].family) { melee2 = i; break; }
    if (melee < 0 || melee2 < 0 || ranged < 0) {
        check(0, "table exposes melee and ranged variants");
        return;
    }
    check(1, "table exposes melee and ranged variants");
    /* no_duplicate_weapons: same family blocked, other family allowed */
    S.scalar_effect[BT_ISTR_NO_DUPLICATE_WEAPONS] = 1.0f;
    bt_weapon_add(&S, (unsigned short)melee, 0, 0.0f);
    check_eq(bt_weapon_slot_available(&S, (unsigned short)melee), 0,
             "no_duplicate_weapons blocks a second copy of the family");
    unsigned short up = BT_WEAPONS[melee].upgrades_into;
    if (up != BT_WEAPON_NONE)
        check_eq(bt_weapon_slot_available(&S, up), 0,
                 "no_duplicate_weapons blocks the family's upgraded variant too");
    check_eq(bt_weapon_slot_available(&S, (unsigned short)melee2), 1,
             "no_duplicate_weapons allows a different family");
    /* max_melee_weapons */
    fresh(14u);
    S.scalar_effect[BT_ISTR_WEAPON_SLOT] = 6.0f;
    S.scalar_effect[BT_ISTR_MAX_MELEE_WEAPONS] = 1.0f;
    bt_weapon_add(&S, (unsigned short)melee, 0, 0.0f);
    check_eq(bt_weapon_slot_available(&S, (unsigned short)melee2), 0,
             "max_melee_weapons 1 blocks a second melee weapon");
    check_eq(bt_weapon_slot_available(&S, (unsigned short)ranged), 1,
             "max_melee_weapons does not restrict ranged weapons");
    check_eq(bt_weapon_kind_copies(&S, BT_WEAPON_KIND_MELEE), 1, "kind copy counter");
    /* max_ranged_weapons REPLACE 3 (Generalist) */
    S.scalar_effect[BT_ISTR_MAX_RANGED_WEAPONS] = 3.0f;
    check_eq(bt_weapon_kind_cap(&S, BT_WEAPON_KIND_RANGED), 3, "max_ranged_weapons REPLACE 3");
    check_eq(bt_weapon_kind_cap(&S, BT_WEAPON_KIND_MELEE), 1, "max_melee_weapons REPLACE 1");
}

/* Knight restrictions (item_service.gd:309-311, 341-354) + unique families. */
static void test_class_restrictions(void) {
    fresh(15u);
    S.scalar_effect[BT_ISTR_WEAPON_SLOT] = 6.0f;
    S.scalar_effect[BT_ISTR_MIN_WEAPON_TIER] = 1.0f;    /* Knight knight_effect_4 */
    S.scalar_effect[BT_ISTR_MAX_WEAPON_TIER] = 3.0f;
    check_eq(bt_weapon_min_tier(&S), 1, "min_weapon_tier 1");
    check_eq(bt_weapon_max_tier(&S), 3, "max_weapon_tier 3");
    check_eq(bt_weapon_tier_clamp(&S, 0), 1, "tier clamp raises common to min 1");
    check_eq(bt_weapon_tier_clamp(&S, 5), 3, "tier clamp caps at max 3");
    check_eq(bt_weapon_tier_clamp(&S, 2), 2, "tier clamp passes through inside the band");
    S.scalar_effect[BT_ISTR_NO_RANGED_WEAPONS] = 1.0f;  /* Knight knight_effect_3 */
    int ranged = find_of_kind(BT_WEAPON_KIND_RANGED, 0);
    int melee = find_of_kind(BT_WEAPON_KIND_MELEE, 0);
    if (ranged < 0 || melee < 0) { check(0, "table exposes melee and ranged weapons"); return; }
    check_eq(bt_weapon_no_ranged(&S), 1, "no_ranged_weapons set");
    check_eq(bt_weapon_shop_kind_ok(&S, (unsigned short)ranged), 0, "ranged weapon excluded from the pool");
    check_eq(bt_weapon_shop_kind_ok(&S, (unsigned short)melee), 1, "melee weapon stays in the pool");
    /* unique families / family copies */
    fresh(16u);
    S.scalar_effect[BT_ISTR_WEAPON_SLOT] = 6.0f;
    int w = find_t0_upgradable();
    if (!require_id(w, "table needs an upgradable tier-0 weapon")) return;
    unsigned short up = BT_WEAPONS[w].upgrades_into;
    bt_weapon_add(&S, (unsigned short)w, 0, 0.0f);
    bt_weapon_add(&S, (unsigned short)w, 0, 0.0f);
    check_eq(bt_weapon_family_copies(&S, BT_WEAPONS[w].family), 2, "two copies of the family");
    check_eq(bt_weapon_unique_families(&S), 1, "one unique family for two same-variant copies");
    if (up != BT_WEAPON_NONE) {
        bt_weapon_add(&S, up, 0, 0.0f);
        check_eq(bt_weapon_unique_families(&S), 1, "the upgraded variant shares the family");
        check_eq(bt_weapon_family_copies(&S, BT_WEAPONS[w].family), 3, "family counts every tier variant");
    }
}

/* item_service.gd:701-742 value + recycle value. */
static void test_value_and_recycle(void) {
    fresh(17u);
    S.scalar_effect[BT_ISTR_WEAPON_SLOT] = 6.0f;
    int w = find_valued();
    if (!require_id(w, "table needs a weapon with value > 1")) return;
    check(w >= 0, "table has a weapon with value > 1");
    int base = (int)BT_WEAPONS[w].value;
    check_eq(bt_weapon_price(&S, (unsigned short)w), expect_price_wave1(base), "wave-1 list price");
    bt_weapon_add(&S, (unsigned short)w, 0, 0.0f);
    check_eq(bt_weapon_recycle_value(&S, 0), expect_recycle_wave1(base, 0.0f), "wave-1 recycle value");
    /* the source's from_value parameter: the specific_items_price fold point */
    check_eq(bt_weapon_recycle_value_from(&S, (unsigned short)w, base),
             expect_recycle_wave1(base, 0.0f), "recycle_value_from with the plain base matches");
    check_eq(bt_weapon_recycle_value_from(&S, (unsigned short)w, 1), 1, "from_value == 1 shortcut");
    check(bt_weapon_recycle_value_from(&S, (unsigned short)w, base * 3) >
          expect_recycle_wave1(base, 0.0f), "a folded (larger) base raises the recycle value");
    /* recycling_gains 100 -> factor clamps to 1.0 -> recycle == list price */
    S.scalar_effect[BT_ISTR_RECYCLING_GAINS] = 100.0f;
    check_eq(bt_weapon_recycle_value(&S, 0), expect_price_wave1(base),
             "recycling_gains 100 gives the full list price");
    /* weapons_price -95 halves the list price band */
    S.scalar_effect[BT_ISTR_RECYCLING_GAINS] = 0.0f;
    S.scalar_effect[BT_ISTR_WEAPONS_PRICE] = -95.0f;
    int discounted = bt_weapon_price(&S, (unsigned short)w);
    check(discounted < expect_price_wave1(base), "weapons_price -95 lowers the list price");
    check(discounted >= 1, "price never drops below 1");
    check_eq(bt_weapon_recycle_value(&S, 0), expect_recycle_from(expect_price_wave1_mod(base, -95.0f), 0.0f),
             "recycle value follows the discounted list price");
    /* past the campaign's last wave the items_price factor is not applied */
    S.scalar_effect[BT_ISTR_WEAPONS_PRICE] = 0.0f;
    S.wave = BT_WAVES + 1;
    int after = bt_weapon_recycle_value(&S, 0);
    check(after >= 1, "recycle value stays >= 1 past the campaign");
    /* out-of-range slot */
    check_eq(bt_weapon_recycle_value(&S, 3), 0, "out-of-range recycle slot -> 0");
}

/* run_data.gd:1540-1578 starting weapons. */
static void test_starting_weapons(void) {
    fresh(18u);
    S.scalar_effect[BT_ISTR_WEAPON_SLOT] = 6.0f;
    int ids[3];
    int w0 = find_t0_upgradable();
    if (!require_id(w0, "table needs an upgradable tier-0 weapon")) return;
    distinct_ids(w0, 3, ids);
    bt_weapon_starting_enqueue(&S, (unsigned short)ids[0], 0);
    bt_weapon_starting_enqueue(&S, (unsigned short)ids[1], 0);
    check_eq((int)S.wpn_start_count, 2, "two queued starting weapons");
    check_eq(bt_weapon_starting_apply(&S), 2, "both starting weapons applied");
    check_eq(bt_weapon_count(&S), 2, "applied starting weapons are in the loadout");
    check_eq((int)bt_weapon_id(&S, 0), ids[0], "starting order preserved (slot 0)");
    check_eq((int)bt_weapon_id(&S, 1), ids[1], "starting order preserved (slot 1)");
    check_eq((int)S.wpn_start_count, 0, "queue drained");
    check_eq((int)S.wpn_start_overflow, 0, "no queue overflow");
    /* over-long queue is explicit, never silently dropped */
    fresh(19u);
    S.scalar_effect[BT_ISTR_WEAPON_SLOT] = 6.0f;
    for (int i = 0; i < BT_WEAPON_START_MAX + 1; i++)
        bt_weapon_starting_enqueue(&S, (unsigned short)ids[i % 3], 0);
    check_eq((int)S.wpn_start_overflow, 1, "queue overflow is flagged");
    check_eq((int)S.wpn_start_count, BT_WEAPON_START_MAX, "queue is filled to its bound");
    int added = bt_weapon_starting_apply(&S);
    check_eq(added, BT_WEAPON_SLOTS, "every queued starting weapon is applied up to the 6-slot bound");
    check_eq(bt_weapon_count(&S), BT_WEAPON_SLOTS, "loadout holds as many as the record can");
    check_eq((int)S.wpn_overflow, 1, "starting weapons past the 6-slot bound are an explicit overflow");
    /* invalid starting id is rejected explicitly */
    fresh(20u);
    S.scalar_effect[BT_ISTR_WEAPON_SLOT] = 6.0f;
    bt_weapon_starting_enqueue(&S, (unsigned short)BT_NWEAPONS, 0);
    check_eq((int)S.wpn_start_overflow, 1, "invalid starting id sets the overflow flag");
    check_eq((int)S.wpn_start_count, 0, "invalid starting id is not queued");
}

/* Class-set bonuses are re-derived by bt_weapon_set_refresh() at the same points
 * the source calls update_sets() (run_data.gd:905-915 add_weapon,
 * run_data.gd:1005-1009 after_weapon_removed, run_data.gd:1054-1072 re-derive). */
static void test_set_bonus_wiring(void) {
    fresh(25u);
    S.scalar_effect[BT_ISTR_WEAPON_SLOT] = 6.0f;
    int set = -1;
    int w = find_set_variant(&set);
    if (!require_id(w, "table needs a weapon that belongs to a set")) return;
    check_eq((int)S.wpn_set_tier[set], (int)BT_WEAPON_SET_TIER_NONE,
             "no set tier applied on an empty loadout");
    int up = (int)BT_WEAPONS[w].upgrades_into;
    bt_weapon_add(&S, (unsigned short)w, 0, 0.0f);
    check_eq((int)S.wpn_set_tier[set], (int)BT_WEAPON_SET_TIER_NONE,
             "one weapon is below the 2-weapon threshold");
    bt_weapon_add(&S, (unsigned short)w, 0, 0.0f);
    check_eq((int)S.wpn_set_tier[set], 0, "two weapons of the set apply tier 0");
    bt_weapon_add(&S, (unsigned short)w, 0, 0.0f);
    check_eq((int)S.wpn_set_tier[set], 1, "three weapons of the set apply tier 1");
    /* removal re-derives (after_weapon_removed -> update_sets) */
    bt_weapon_remove(&S, 0);
    check_eq((int)S.wpn_set_tier[set], 0, "removal re-derives down to tier 0");
    bt_weapon_remove(&S, 0);
    check_eq((int)S.wpn_set_tier[set], (int)BT_WEAPON_SET_TIER_NONE,
             "dropping below the threshold unapplies the tier");
    /* combine re-derives from an empty loadout (erase + erase + append) */
    bt_weapon_remove_all(&S);
    bt_weapon_add(&S, (unsigned short)w, 0, 0.0f);
    bt_weapon_add(&S, (unsigned short)w, 0, 0.0f);
    check_eq((int)S.wpn_set_tier[set], 0, "tier applied before the combine");
    if (up != (int)BT_WEAPON_NONE) {
        int slot = bt_weapon_combine(&S, 0);
        check(slot >= 0, "combine succeeds");
        check_eq(bt_weapon_count(&S), 1, "combine leaves exactly one copy");
        check_eq((int)S.wpn_set_tier[set], (int)BT_WEAPON_SET_TIER_NONE,
                 "combine re-derives the set state (one copy is below the threshold)");
    }
    /* remove_all and reset both leave every tier unapplied */
    bt_weapon_remove_all(&S);
    bt_weapon_add(&S, (unsigned short)w, 0, 0.0f);
    bt_weapon_add(&S, (unsigned short)w, 0, 0.0f);
    check_eq((int)S.wpn_set_tier[set], 0, "tier applied before remove_all");
    bt_weapon_remove_all(&S);
    check_eq((int)S.wpn_set_tier[set], (int)BT_WEAPON_SET_TIER_NONE,
             "remove_all unapplies every tier");
    bt_weapon_add(&S, (unsigned short)w, 0, 0.0f);
    bt_weapon_add(&S, (unsigned short)w, 0, 0.0f);
    check_eq((int)S.wpn_set_tier[set], 0, "tier applied again before reset");
    bt_weapon_reset(&S);
    check_eq((int)S.wpn_set_tier[set], (int)BT_WEAPON_SET_TIER_NONE,
             "reset clears the set bookkeeping without unapplying");
}

/* remove_all + reset (run_data.gd:1010-1022, bt_core_reset hook). */
static void test_remove_all_and_reset(void) {
    fresh(21u);
    S.scalar_effect[BT_ISTR_WEAPON_SLOT] = 6.0f;
    int ids[3];
    int w0 = find_t0_upgradable();
    if (!require_id(w0, "table needs an upgradable tier-0 weapon")) return;
    distinct_ids(w0, 3, ids);
    for (int i = 0; i < 3; i++) bt_weapon_add(&S, (unsigned short)ids[i], 1, 0.3f);
    bt_weapon_remove_all(&S);
    check_eq(bt_weapon_count(&S), 0, "remove_all empties the loadout");
    for (int i = 0; i < BT_WEAPON_SLOTS; i++) {
        check_eq((int)S.wpn_id[i], (int)BT_WEAPON_NONE, "reset id slot is BT_WEAPON_NONE");
        check(S.wpn_uid[i] == BT_WEAPON_UID_NONE, "reset uid slot is none");
        check_near(S.wpn_curse[i], 0.0f, 1e-6f, "reset curse is 0");
        check_near(S.wpn_wmod[i], 0.0f, 1e-6f, "reset weapon-stats curse modifier is 0");
        for (int c = 0; c < BT_WEAPON_FX_CURSE_MAX; c++)
            check_near(S.wpn_fxcurse[i][c], 0.0f, 1e-6f, "reset effect curse modifier is 0");
        check_near(S.wpn_cd[i], 0.0f, 1e-6f, "reset cooldown is 0 ticks");
    }
    bt_weapon_starting_enqueue(&S, (unsigned short)ids[0], 0);
    S.wpn_overflow = 1;
    bt_weapon_reset(&S);
    check_eq((int)S.wpn_count, 0, "reset clears the count");
    check_eq((int)S.wpn_overflow, 0, "reset clears wpn_overflow");
    check_eq((int)S.wpn_dirty, 0, "reset clears wpn_dirty");
    check_eq((int)S.wpn_start_count, 0, "reset clears the starting queue");
    for (int i = 0; i < BT_NWEAPON_SETS; i++)
        check_eq((int)S.wpn_set_tier[i], (int)BT_WEAPON_SET_TIER_NONE,
                 "reset clears the applied-set bookkeeping");
    check_eq((int)S.wpn_uid_next, 0, "reset clears the uid allocator");
    check_eq(bt_weapon_starting_apply(&S), 0, "nothing is applied after reset");
}

/* A weapon's own SUM `stat_*` effect rows move the player's stat LAYER at
 * add_weapon and move it back at after_weapon_removed (run_data.gd:905-915 ->
 * apply_item_effects -> effect.gd:59-72 apply(): effects[key] += value; and
 * :1000-1009 -> unapply_item_effects -> unapply(): effects[key] -= value).
 * Shipped producers: jousting_lance_1 (stat_speed +2), hand_effect_1
 * (stat_harvesting +3), rock_3_effect_1 (stat_max_hp +2).
 *
 * The observable layer is stat_raw (the SUM target effect.gd writes); the
 * deferred LiveStats snapshot (p_speed_pct, ...) is re-based at every wave build
 * by bt_player_stats_reset, which the core's own tests cover. */
static void test_weapon_stat_effects(void) {
    fresh(7u);
    S.scalar_effect[BT_ISTR_WEAPON_SLOT] = 6.0f;
    int sid = bt_core_id_from_istr(BT_ISTR_STAT_SPEED);
    if (!require_id(sid == 255 ? -1 : sid, "table needs a core stat_speed id")) return;
    int want_speed = 0;
    int lance = find_weapon_stat(BT_ISTR_STAT_SPEED, &want_speed);
    if (!require_id(lance, "table needs a weapon with a SUM stat_speed row")) return;
    check(want_speed > 0, "the stat_speed row carries a positive value");
    /* These fixtures hand-build a BtState: stat_raw carries whatever the tests
     * above left (they drive the same rail now), and stat_gain is 0, so pin the
     * layer this test observes instead of inheriting it. */
    S.stat_raw[sid] = 0.0f;
    S.stat_gain[sid] = 1.0f;
    float before = S.stat_raw[sid];
    int slot = bt_weapon_add(&S, (unsigned short)lance, 0, 0.0f);
    check_eq(slot, 0, "stat-effect weapon lands in slot 0");
    check_near(S.stat_raw[sid], before + (float)want_speed, 1e-6f,
               "add_weapon applies the copy's stat_speed row");
    check_eq(bt_weapon_id(&S, 0), (unsigned short)lance, "slot 0 holds it");
    check_near(S.speed_pct, before + (float)want_speed, 1e-6f,
               "the derived speed stat follows the layer");
    check_eq(bt_weapon_erase(&S, 0), (int)lance, "erase removes the copy");
    check_near(S.stat_raw[sid], before, 1e-6f,
               "after_weapon_removed unapplies the stat_speed row");
    check_eq(bt_weapon_count(&S), 0, "loadout empty again");

    /* A second stat family, and the slot-count-independent path: the value comes
     * from the row, not from a constant. */
    int want_hp = 0;
    int rock = find_weapon_stat(BT_ISTR_STAT_MAX_HP, &want_hp);
    if (require_id(rock, "table needs a weapon with a SUM stat_max_hp row")) {
        int hp_id = bt_core_id_from_istr(BT_ISTR_STAT_MAX_HP);
        S.stat_raw[hp_id] = 0.0f;
        S.stat_gain[hp_id] = 1.0f;
        float hp_before = S.stat_raw[hp_id];
        check(bt_weapon_add(&S, (unsigned short)rock, 0, 0.0f) >= 0, "add the stat_max_hp weapon");
        check_near(S.stat_raw[hp_id], hp_before + (float)want_hp, 1e-6f,
                   "a different stat family lands in its own layer");
        bt_weapon_remove_all(&S);
        check_near(S.stat_raw[hp_id], hp_before, 1e-6f, "remove_all unapplies every copy");
    }

    /* A weapon whose SUM rows key NO core stat must leave the player's stat
     * layer untouched: those keys (xp_gain, knockback, burning_spread, ...) have
     * no core stat id and belong to the scalar rails the item path routes. */
    int scalar_only_weapons = 0;
    for (int i = 0; i < BT_NWEAPONS; i++) {
        BT_CONST_PTR(BtWeaponDef) D = &BT_WEAPONS[i];
        int has_stat_row = 0, has_scalar_row = 0;
        for (int k = 0; k < (int)D->fx_count; k++) {
            int idx = (int)D->fx_first + k;
            if (idx >= BT_NWEAPON_EFF_INDEX) break;
            int row = (int)BT_WEAPON_EFF_INDEX[idx];
            if (row < 0 || row >= BT_NITEM_EFF_ROWS) continue;
            BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[row];
            if (E->opcode != BT_ITEMEFF_EFFECT || E->storage != 0) continue;
            if (bt_core_id_from_istr(E->key) == 255) has_scalar_row = 1;
            else has_stat_row = 1;
        }
        if (!has_scalar_row || has_stat_row) continue;
        float keep = S.stat_raw[3];
        float keep2 = S.stat_raw[0];
        check(bt_weapon_add(&S, (unsigned short)i, 0, 0.0f) >= 0, "add a scalar-only weapon");
        check_near(S.stat_raw[3], keep, 1e-6f, "a non-stat SUM row leaves stat_speed alone");
        check_near(S.stat_raw[0], keep2, 1e-6f, "a non-stat SUM row leaves stat_max_hp alone");
        bt_weapon_remove_all(&S);
        scalar_only_weapons++;
    }
    check(scalar_only_weapons > 0, "the pool ships a weapon whose SUM rows are all scalars");
}

int main(void) {
    /* These fixtures hand-build a BtState and never run bt_core_init, which is
     * the only initializer for the two RNG-model gates; without this they would
     * assert the neutralized (audio 0 / particles 0) model instead of the
     * source default they were written for. */
    S.audio_rng = 1;
    S.particles_rng = 1;
    printf("brotato_weapon_inventory\n");
    test_bull_zero_slot();
    test_capacity_and_add();
    test_remove_compaction_and_uid();
    test_full_loadout();
    test_buy_auto_combine();
    test_combine_rules();
    test_curse_merge();
    test_curse_state();
    test_combine_duplicate_pick();
    test_slot_availability_rules();
    test_class_restrictions();
    test_value_and_recycle();
    test_starting_weapons();
    test_set_bonus_wiring();
    test_remove_all_and_reset();
    test_weapon_stat_effects();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
