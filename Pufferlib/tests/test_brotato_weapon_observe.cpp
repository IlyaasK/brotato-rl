/* tests/test_brotato_weapon_observe.cpp
 *
 * Source-backed tests for the appended weapon / shop / profile observation
 * block (ocean/brotato/brotato_weapon_observe.h bt_weapon_observe_append).
 *
 * The block extends bt_core_observe from 79 to 161 floats:
 *   [79 .. 126]  six weapon records x 8 fields
 *   [127.. 138]  four typed shop offers x 3 fields
 *   [139.. 140]  profile: character, weapon slot limit
 *   [141.. 160]  shop action: currency flag, 4 offers x (price/eligible/
 *                affordable/buyable), reroll (price/free/available)
 *
 * Sources:
 *   weapon.gd:28,232-248        Weapon.tier / is_cursed / _current_cooldown
 *   weapon_stats.gd:4           cooldown in ticks (60 ticks per second)
 *   item_parent_data.gd:6,11    ItemParentData.Tier 0 COMMON .. 5 DANGER_5
 *   weapon_data.gd:8-9          WeaponData.Type { MELEE, RANGED }
 *   category.gd:3               Category { ITEM, WEAPON }
 *   player_run_data.gd:434      weapon_slot default 6
 *   bull_effect_5.tres          Bull weapon_slot 0 (stays weaponless)
 *
 * These are observable-behaviour tests: they drive the real encoder on a real
 * BtState and check the emitted floats and the documented empty encodings. They
 * derive every weapon id and effective stat from the generated tables / the
 * canonical bt_weapon_stats, never from a hardcoded row index or source text.
 *
 * Build and run:
 *     c++ -O2 -o build/test_brotato_weapon_observe \
 *         tests/test_brotato_weapon_observe.cpp -lm
 *     ./build/test_brotato_weapon_observe
 *
 * Exit code 0 = all checks passed.
 */

#include "../ocean/brotato/brotato_core.h"
#include "../ocean/brotato/brotato_weapon_observe.h"

#include <stdio.h>

static int g_failures = 0;
static int g_checks = 0;

static void check(int cond, const char* msg) {
    g_checks++;
    if (!cond) { g_failures++; printf("  FAIL: %s\n", msg); }
}

static void check_near(float got, float want, float tol, const char* msg) {
    g_checks++;
    if (!(got - want <= tol && want - got <= tol)) {
        g_failures++;
        printf("  FAIL: %s (got %.7g want %.7g)\n", msg, (double)got, (double)want);
    }
}

/* BtState embeds two physics worlds; keep the big state static. */
static BtState S;

static const float SENT = -12345.0f;

static int any_nonzero(const float* o, int from, int to) {
    for (int i = from; i < to; i++) if (o[i] != 0.0f) return 1;
    return 0;
}

static int find_kind(int kind) {
    for (int i = 0; i < BT_NWEAPONS; i++) if ((int)BT_WEAPONS[i].kind == kind) return i;
    return -1;
}

int main(void) {
    float o[BT_OBS_SIZE];

    printf("[1] width and wiring\n");
    /* obs 165..173 (v4 shop block): lock flags, crate offer, level-up reroll
     * price, read back from the state the shop actions set. */
    {
        S.shop_locked[1] = 1;
        S.shop_locked[3] = 2;
        S.crate_kind = BT_SHOP_ITEM;
        S.crate_id = 5;
        S.crate_cursed = 1;
        S.lvl_reroll_price = 7;
        for (int i = 0; i < BT_OBS_SIZE; i++) o[i] = SENT;
        bt_core_observe(&S, o);
        check(o[165] == 0.0f && o[166] == 1.0f && o[167] == 0.0f && o[168] == 1.0f,
              "obs 165..168 = per-offer lock flag");
        check(o[169] == (float)BT_SHOP_ITEM, "obs 169 = crate offer kind");
        check(o[170] > 0.0f && o[170] < 1.0f, "obs 170 = crate id normalised");
        check(o[171] == 1.0f, "obs 171 = crate cursed");
        check(o[172] == 1.0f, "obs 172 = crate ready");
        check(o[173] == 7.0f, "obs 173 = level-up reroll price");
        S.crate_kind = BT_SHOP_NONE;
        S.crate_id = 0; S.crate_cursed = 0; S.lvl_reroll_price = 0;
        for (int i = 0; i < 4; i++) S.shop_locked[i] = 0;
        for (int i = 0; i < BT_OBS_SIZE; i++) o[i] = SENT;
        bt_core_observe(&S, o);
        check(o[169] == 0.0f && o[170] == 0.0f && o[171] == 0.0f && o[172] == 0.0f,
              "no crate offer -> all four crate slots are zero");
    }

    check(BT_OBS_SIZE == 203,
          "observation width is 203 (161 v3 + 4 stats + 4 locks + 4 crate + 1 level-up reroll + 24 guide items + 1 ban budget + 4 level-up presence)");
    check(BT_OBS_TARGET == 42 && BT_OBS_T_STATS == 4 && BT_OBS_T_LOCKS == 4 &&
          BT_OBS_T_CRATE == 4 && BT_OBS_T_LVLRR == 1 && BT_OBS_T_ITEMS == 24 &&
          BT_OBS_T_BAN == 1 && BT_OBS_T_LVLOFF == 4,
          "the appended target block is 4 + 4 + 4 + 1 + 24 + 1 + 4");
    check(BT_ACTIONS == 31, "action space is 31 (21 v3 + lock 21..24, ban 25..28, crate 29/30)");
    check(BT_OBS_SIZE_PINNED == 203 && BT_OBS_SIZE == BT_OBS_SIZE_PINNED,
          "the literal pin gen_obs_tables.py emits equals the composed BT_OBS_SIZE");
    check(BT_OBS_WEAPONS == 48 && BT_OBS_SHOP_EXT == 12 && BT_OBS_PROFILE == 2 &&
          BT_OBS_SHOP_ACTION == 20,
          "block component sizes are 48 / 12 / 2 / 20");
    check(BT_OBS_WEAPON_SIZE == 82, "block size is 82");
    check(BT_WEAPON_SLOTS == 6, "six weapon slots");

    bt_core_init(&S, 1234u, 0, 1, (unsigned short)BT_CHARACTER_BULL, (unsigned short)BT_WEAPON_NONE);
    check((int)S.character == BT_CHARACTER_BULL, "Bull profile seeds character_id 0");
    check(S.weapon_slot_limit == 0, "Bull's weapon_slot effect value 0 -> zero slots");
    int weaponless = 1;
    for (int i = 0; i < BT_WEAPON_SLOTS; i++)
        if (S.wpn_id[i] != BT_WEAPON_NONE) weaponless = 0;
    check(weaponless, "Bull starts with no weapon in any slot");

    for (int i = 0; i < BT_OBS_SIZE; i++) o[i] = SENT;
    bt_core_observe(&S, o);
    int tail_written = 1;
    for (int i = 79; i < BT_OBS_SIZE; i++) if (o[i] == SENT) tail_written = 0;
    check(tail_written, "bt_core_observe writes the appended 82-float block");

    printf("[2] Bull: empty slots and profile\n");
    check(!any_nonzero(o, 79, 127), "six empty weapon slots emit eight 0.0 each");
    check(!any_nonzero(o, 127, 139), "empty shop offers emit 0.0 / 0.0 / 0.0");
    check(!any_nonzero(o, 141, 161), "an empty shop-action block emits all 0.0");
    check_near(o[139], 1.0f / (float)(BT_NCHARACTERS + 1), 1e-6f,
               "profile character: Bull (id 0) -> 1/(BT_NCHARACTERS+1)");
    check_near(o[140], 0.0f, 1e-6f, "profile slot limit: Bull 0 / 6 = 0.0");

    printf("[3] occupied slot, typed offers, Knight profile\n");
    bt_core_init(&S, 4321u, 0, 1, (unsigned short)BT_CHARACTER_BULL, (unsigned short)BT_WEAPON_NONE);
    int wid = find_kind(BT_WEAPON_KIND_MELEE);
    check(wid >= 0, "BT_WEAPONS holds a melee weapon");
    if (wid < 0) wid = 0;

    S.wpn_id[0] = (unsigned short)wid;
    S.wpn_tier[0] = 2;
    S.wpn_cursed[0] = 1;
    S.wpn_count = 1;
    S.shop_kind[0] = BT_SHOP_ITEM;
    S.shop_id[0] = 5;
    S.shop_cursed[0] = 1;
    S.shop_kind[1] = BT_SHOP_WEAPON;
    S.shop_id[1] = 3;
    S.shop_cursed[1] = 0;
    S.character = (unsigned short)BT_CHARACTER_KNIGHT;
    S.weapon_slot_limit = 6;
    float half_period = 0.0f;
    {
        BtPlayerWeaponStats st;
        bt_weapon_stats(&S, 0, &st);
        half_period = (float)st.cooldown * 0.5f;
        S.wpn_cd[0] = half_period;      /* halfway through the attack cooldown */
    }

    for (int i = 0; i < BT_OBS_SIZE; i++) o[i] = SENT;
    check(bt_weapon_observe_append(&S, o, 79) == 161,
          "bt_weapon_observe_append returns 161");

    check_near(o[79], (float)(wid + 1) / (float)(BT_NWEAPONS + 1), 1e-6f,
               "weapon id -> (index+1)/(BT_NWEAPONS+1)");
    check_near(o[80], 0.5f, 1e-6f, "tier 2 -> 2/4");
    check_near(o[81], (float)BT_WEAPONS[wid].kind, 1e-6f, "kind comes from the table");
    {
        BtPlayerWeaponStats st;
        bt_weapon_stats(&S, 0, &st);
        float cd = st.cooldown > 0 ? (float)st.cooldown_left / (float)st.cooldown : 0.0f;
        if (cd < 0.0f) cd = 0.0f; else if (cd > 1.0f) cd = 1.0f;
        check_near(cd, 0.5f, 1e-5f, "half-period countdown is observable in combat");
        check_near(o[82], cd, 1e-6f, "cooldown = clamp(cooldown_left/cooldown)");
        check_near(o[83], (float)st.damage * 0.1f, 1e-6f, "damage * 0.1");
        check_near(o[84], (float)st.max_range / BT_ARENA_W, 1e-5f, "range / arena_w");
        check_near(o[85], st.crit_chance, 1e-6f, "crit_chance raw");
    }
    check_near(o[86], 1.0f, 1e-6f, "cursed flag 1.0");
    check(!any_nonzero(o, 87, 127), "the other five weapon slots emit 0.0");

    check_near(o[127], (float)BT_SHOP_ITEM, 1e-6f, "item offer type 1.0");
    check_near(o[128], 6.0f / (float)(BT_NITEMS + 1), 1e-6f, "item id -> (index+1)/(BT_NITEMS+1)");
    check_near(o[129], 1.0f, 1e-6f, "item offer cursed");
    check_near(o[130], (float)BT_SHOP_WEAPON, 1e-6f, "weapon offer type 2.0");
    check_near(o[131], 4.0f / (float)(BT_NWEAPONS + 1), 1e-6f,
               "weapon id -> (index+1)/(BT_NWEAPONS+1)");
    check_near(o[132], 0.0f, 1e-6f, "weapon offer not cursed");
    check(!any_nonzero(o, 133, 139), "empty offers 3 and 4 emit 0.0 / 0.0 / 0.0");

    check_near(o[139], (float)(BT_CHARACTER_KNIGHT + 1) / (float)(BT_NCHARACTERS + 1), 1e-6f,
               "profile character: Knight");
    check_near(o[140], 1.0f, 1e-6f, "profile slot limit: 6 / 6 = 1.0");

    printf("[4] empty sentinel never collides with a real weapon\n");
    check(o[79] != 0.0f, "weapon index 0 emits a nonzero id (0.0 is the empty sentinel)");

    printf("[5] shop phase: no live weapon attack clock\n");
    /* The shop scene has no Weapon node and therefore no attack clock, so the
     * cooldown feature reads 0.0 (ready) while the stored countdown survives
     * into the next arena step - a shop row never invents a countdown. */
    float cd_saved = S.wpn_cd[0];
    BtPlayerWeaponStats st5;
    bt_weapon_stats(&S, 0, &st5);
    S.phase = 1;
    for (int i = 0; i < BT_OBS_SIZE; i++) o[i] = SENT;
    check(bt_weapon_observe_append(&S, o, 79) == 161, "shop row still writes 82 floats");
    check_near(o[82], 0.0f, 1e-6f, "phase 1 emits cooldown_frac 0.0");
    check(S.wpn_cd[0] == cd_saved, "the stored cooldown counter is not reset by the observer");
    check_near(o[80], 0.5f, 1e-6f, "tier is unchanged in the shop");
    check_near(o[83], (float)st5.damage * 0.1f, 1e-6f, "damage is unchanged in the shop");
    check_near(o[86], 1.0f, 1e-6f, "cursed is unchanged in the shop");
    check_near(o[139], (float)(BT_CHARACTER_KNIGHT + 1) / (float)(BT_NCHARACTERS + 1), 1e-6f,
               "profile is unchanged in the shop");

    S.phase = 0;
    for (int i = 0; i < BT_OBS_SIZE; i++) o[i] = SENT;
    bt_weapon_observe_append(&S, o, 79);
    check_near(o[82], 0.5f, 1e-5f, "phase 0 restores the live cooldown fraction");

    printf("[6] actionable shop block: exact price / eligibility / reroll\n");
    /* Fresh Knight shop row with two live offers (one item, one weapon). */
    bt_core_init(&S, 777u, 0, 1, (unsigned short)BT_CHARACTER_KNIGHT, (unsigned short)BT_WEAPON_NONE);
    S.character = (unsigned short)BT_CHARACTER_KNIGHT;
    S.wave = 5;
    S.materials = 1000.0f;
    S.phase = 1;
    S.paid_rerolls = 0;
    S.free_rerolls_remaining = 0;
    S.has_bonus_free_reroll = 0;
    for (int i = 0; i < 4; i++) { S.shop_kind[i] = BT_SHOP_NONE; S.shop_id[i] = 0; }
    S.shop_kind[0] = BT_SHOP_ITEM; S.shop_id[0] = 5;
    int woffer = find_kind(BT_WEAPON_KIND_RANGED);
    if (woffer < 0) { printf("FAIL: ranged weapon fixture missing\n"); return 1; }
    S.shop_kind[1] = BT_SHOP_WEAPON; S.shop_id[1] = (unsigned short)woffer;

    for (int i = 0; i < BT_OBS_SIZE; i++) o[i] = SENT;
    check(bt_weapon_observe_append(&S, o, 79) == 161, "shop-action row returns 161");

    check_near(o[141], 0.0f, 1e-6f, "Knight currency is gold (currency_is_hp 0.0)");
    check(o[142] > 0.0f && o[145] == 1.0f, "item offer is priced and buyable");
    check(o[146] > 0.0f && o[147] == 0.0f && o[149] == 0.0f,
          "Knight cannot buy a ranged weapon even when it is affordable");
    check(!any_nonzero(o, 150, 158), "absent offers have no price or buy action");
    float item_price = o[142];
    S.shop_available = 3;
    bt_progress_shop_action(&S, 1);
    check(S.shop_kind[1] == BT_SHOP_WEAPON && S.materials == 1000.0f,
          "ineligible ranged offer remains unsold and costs no gold");
    S.materials = item_price - 1.0f;
    bt_weapon_observe_append(&S, o, 79);
    check(o[144] == 0.0f && o[145] == 0.0f, "item becomes unaffordable below its price");
    bt_progress_shop_action(&S, 0);
    check(S.shop_kind[0] == BT_SHOP_ITEM, "unaffordable offer cannot be purchased");
    S.materials = item_price;
    S.shop_timer = 1;
    bt_weapon_observe_append(&S, o, 79);
    check(o[144] == 1.0f && o[145] == 0.0f, "purchase delay blocks an affordable offer");
    bt_progress_shop_action(&S, 0);
    check(S.shop_kind[0] == BT_SHOP_ITEM, "purchase delay leaves the offer intact");
    bt_weapon_observe_append(&S, o, 79);
    check(o[144] == 1.0f && o[145] == 1.0f, "item becomes buyable at its price");
    bt_progress_shop_action(&S, 0);
    check(S.shop_kind[0] == BT_SHOP_NONE && S.materials == 0.0f,
          "buyable item costs the observed price and clears the offer");
    S.materials = 1000.0f;

    /* Free reroll: source zeroes the price (base_shop.gd:479-483). */
    S.free_rerolls_remaining = 1;
    for (int i = 0; i < BT_OBS_SIZE; i++) o[i] = SENT;
    bt_weapon_observe_append(&S, o, 79);
    check_near(o[158], 0.0f, 1e-6f, "free reroll price is 0.0");
    check_near(o[159], 1.0f, 1e-6f, "free reroll flag is 1.0");
    check_near(o[160], 1.0f, 1e-6f, "free reroll is available");
    S.free_rerolls_remaining = 0;

    /* Unaffordable paid reroll -> unavailable. */
    S.materials = 0.5f;
    for (int i = 0; i < BT_OBS_SIZE; i++) o[i] = SENT;
    bt_weapon_observe_append(&S, o, 79);
    check_near(o[160], 0.0f, 1e-6f, "unaffordable paid reroll is unavailable");
    S.materials = 1000.0f;

    /* Shop actions are absent outside the shop, even with stale offer state. */
    S.phase = 0;
    for (int i = 0; i < BT_OBS_SIZE; i++) o[i] = SENT;
    bt_weapon_observe_append(&S, o, 79);
    check(!any_nonzero(o, 141, 161), "shop actions are absent outside the shop phase");

    /* Demon: hp_shop prices in max-HP units; reroll stays gold. */
    S.phase = 1;
    S.character = (unsigned short)BT_CHARACTER_DEMON;
    S.paid_rerolls = 0;
    S.free_rerolls_remaining = 0;
    S.has_bonus_free_reroll = 0;
    S.shop_kind[0] = BT_SHOP_ITEM; S.shop_id[0] = 5; S.shop_available = 3;
    S.materials = 0.0f;
    for (int i = 0; i < BT_OBS_SIZE; i++) o[i] = SENT;
    bt_weapon_observe_append(&S, o, 79);
    check_near(o[141], 1.0f, 1e-6f, "Demon currency_is_hp is 1.0");
    check(o[142] > 0.0f && o[144] == 1.0f,
          "Demon can afford an HP-priced item with no gold");
    check(o[158] > 0.0f && o[160] == 0.0f,
          "Demon cannot pay a gold reroll with HP");
    float reroll_price = o[158];
    S.materials = reroll_price;
    bt_weapon_observe_append(&S, o, 79);
    check(o[160] == 1.0f, "Demon can reroll when gold reaches the observed price");

    printf("[7] armor slot carries the ARMOR STAT, not the player's truncated armor\n");
    /* bt_core_observe o[3] = Utils.get_stat(stat_armor_hash, 0) / 20, the value
     * the bridge mod reads (rl_obs.gd:974).  The player's own armour used by the
     * damage path (player.gd:353 max_stats.armor = get_stat as int, stored in
     * s->armor) is truncated and must NOT feed the observation. */
    bt_core_init(&S, 606u, 0, 1, (unsigned short)BT_CHARACTER_BULL, (unsigned short)BT_WEAPON_NONE);
    S.stat_gain[2] = 1.0f;
    S.stat_raw[2] = 3.5f;       /* fractional armor stat */
    S.temp_stat[2] = 0.0f;
    S.link_stat[2] = 0.0f;
    S.armor = 3.0f;             /* what bt_core_stat_sync would store (int cast) */
    for (int i = 0; i < BT_OBS_SIZE; i++) o[i] = SENT;
    bt_core_observe(&S, o);
    check_near(o[3], 3.5f * 0.05f, 1e-6f, "o[3] = armor stat / 20 (3.5 -> 0.175)");
    check(o[3] != S.armor * 0.05f, "o[3] is not the truncated s->armor / 20 (0.15)");

    printf("[8] projectile slots: three nearest within BT_OBS_PROJ_MAX_D2, distinct\n");
    /* The bridge mod reports a projectile only while its squared distance is
     * below PROJ_MAX_D2 = 70000 (rl_obs.gd:70, state_contract.json obs[68..76]).
     * Under the old 512^2 = 262144 cutoff a projectile at 300 px (90000) would
     * have been reported; it must not be. */
    bt_core_init(&S, 909u, 0, 1, (unsigned short)BT_CHARACTER_BULL, (unsigned short)BT_WEAPON_NONE);
    S.pr_used = 5;   /* the raw projectile fixtures below populate slots 0..4 */
    check_near(BT_OBS_PROJ_MAX_D2, 70000.0f, 1e-3f, "cutoff constant is 70000 (the bridge value)");
    S.px = 0.0f; S.py = 0.0f;
    S.pralive[0] = 1; S.prx[0] = 100.0f; S.pry[0] = 0.0f; S.prdmg[0] = 7.0f;  /* d2 = 10000 */
    S.pralive[1] = 1; S.prx[1] = 300.0f; S.pry[1] = 0.0f; S.prdmg[1] = 9.0f;  /* d2 = 90000  */
    S.pralive[2] = 1; S.prx[2] = 50.0f;  S.pry[2] = 0.0f; S.prdmg[2] = 5.0f;  /* d2 = 2500   */
    /* An enemy-fired shot names its firing enemy slot (0xFFFF = not one). */
    S.prsrc[0] = 0; S.prsrc[1] = 0; S.prsrc[2] = 0;
    S.prfriendly[0] = 0; S.prfriendly[1] = 0; S.prfriendly[2] = 0;
    for (int i = 0; i < BT_OBS_SIZE; i++) o[i] = SENT;
    bt_core_observe(&S, o);
    check_near(o[68], 50.0f / BT_ARENA_W, 1e-6f, "slot 0 nearest projectile dx / arena_w");
    check_near(o[69], 0.0f, 1e-6f, "slot 0 dy");
    check_near(o[70], 0.5f, 1e-6f, "slot 0 damage * 0.1");
    check_near(o[71], 100.0f / BT_ARENA_W, 1e-6f, "slot 1 second-nearest projectile");
    check_near(o[73], 0.7f, 1e-6f, "slot 1 damage * 0.1");
    check(!any_nonzero(o, 74, 77), "slot 2 empty: the 90000 projectile is beyond the 70000 cutoff");
    check(S.pralive[0] == 1 && S.pralive[1] == 1 && S.pralive[2] == 1,
          "the observer restores the consumed markers");

    /* Decisive single-projectile check: inside 262144 but outside 70000. */
    S.pralive[0] = S.pralive[1] = S.pralive[2] = 0;
    S.pralive[4] = 1; S.prx[4] = 400.0f; S.pry[4] = 0.0f; S.prdmg[4] = 3.0f;  /* d2 = 160000 */
    S.prsrc[4] = 0; S.prfriendly[4] = 0;
    for (int i = 0; i < BT_OBS_SIZE; i++) o[i] = SENT;
    bt_core_observe(&S, o);
    check(!any_nonzero(o, 68, 77),
          "a lone projectile at d2 = 160000 (>70000, <262144) is not reported");

    /* Nearer-first ordering across distinct projectiles. */
    S.pralive[4] = 0;
    S.pralive[0] = 1; S.prx[0] = 100.0f; S.pry[0] = 0.0f; S.prdmg[0] = 1.0f; S.prsrc[0] = 0;
    S.pralive[1] = 1; S.prx[1] = -100.0f; S.pry[1] = 100.0f; S.prdmg[1] = 2.0f; S.prsrc[1] = 0;
    for (int i = 0; i < BT_OBS_SIZE; i++) o[i] = SENT;
    bt_core_observe(&S, o);
    check_near(o[68], 100.0f / BT_ARENA_W, 1e-6f, "the nearer projectile fills slot 0");
    check_near(o[71], -100.0f / BT_ARENA_W, 1e-6f, "the farther projectile fills slot 1");

    /* A charmed enemy's shot is friendly (hits enemies) but is still parented
     * under EnemyProjectiles, so it stays in the observation. */
    S.pralive[0] = 1; S.prx[0] = 100.0f; S.pry[0] = 0.0f; S.prdmg[0] = 4.0f;
    S.prsrc[0] = 7; S.prfriendly[0] = 1;
    for (int i = 0; i < BT_OBS_SIZE; i++) o[i] = SENT;
    bt_core_observe(&S, o);
    check_near(o[68], 100.0f / BT_ARENA_W, 1e-6f, "a charmed enemy shot is still reported");

    /* A pet/turret or item special projectile lives in the player container and
     * must NOT fill an enemy-projectile slot (friendly, no source enemy). */
    S.pralive[1] = 0;
    S.pralive[0] = 1; S.prx[0] = 10.0f; S.pry[0] = 0.0f; S.prdmg[0] = 4.0f;
    S.prsrc[0] = 0xFFFFu; S.prfriendly[0] = 1;
    for (int i = 0; i < BT_OBS_SIZE; i++) o[i] = SENT;
    bt_core_observe(&S, o);
    check(!any_nonzero(o, 68, 77), "a friendly pet/turret shot is not an enemy projectile");

    printf("[9] v4 target block: Demon/endless stats, item counts, action mask\n");
    bt_core_init(&S, 4242u, 0, 1, (unsigned short)BT_CHARACTER_DEMON, (unsigned short)BT_WEAPON_NONE);
    S.harvest = 7.5f;
    S.luck = 12.0f;
    for (int i = 0; i < BT_NITEMS; i++) S.item_owned[i] = 0;
    S.item_owned[BT_OBS_GUIDE_ITEMS[0]] = 2;    /* bag x2  */
    S.item_owned[BT_OBS_GUIDE_ITEMS[5]] = 9;    /* recycling machine, clamped to 4 */
    for (int i = 0; i < BT_OBS_SIZE; i++) o[i] = SENT;
    bt_core_observe(&S, o);
    check_near(o[161], 0.075f, 1e-6f, "harvesting / 100");
    check_near(o[162], 0.12f, 1e-6f, "luck / 100");
    check_near(o[163], 0.0f, 1e-6f, "endless factor is 0 through wave 20");
    check_near(o[164], (float)S.wave / 50.0f, 1e-6f, "wave / 50");
    check(!any_nonzero(o, 165, 174), "locks / crate offer / level-up reroll are absent (all 0.0) in a fresh state");
    check_near(o[174], 2.0f / 4.0f, 1e-6f, "guide item 0 owned x2 -> 0.5");
    check_near(o[179], 1.0f, 1e-6f, "guide item 5 owned x9 clamps to 4/4");
    check_near(o[197], 0.0f, 1e-6f, "an unowned guide item -> 0.0");

    /* RunData.get_endless_factor() is 0 at wave 20 and grows past it; the slot
     * is the squashed factor, so a wave-30 run reports a nonzero value. */
    S.wave = 30;
    bt_core_observe(&S, o);
    check(o[163] > 0.0f && o[163] < 1.0f, "endless factor is squashed into (0,1) past wave 20");
    check_near(o[164], 0.6f, 1e-6f, "wave 30 / 50");
    S.wave = 1;

    /* The per-phase action mask. */
    {
        unsigned char mask[BT_ACTIONS];
        S.phase = 0;
        bt_action_mask(&S, mask);
        int legal = 0, bad = 0;
        for (int i = 0; i < BT_ACTIONS; i++) { legal += mask[i] != 0; if (i > 8 && mask[i]) bad++; }
        check(legal == 9 && bad == 0, "combat mask exposes only 0..8");
        S.phase = 1;
        for (int i = 0; i < 4; i++) { S.shop_kind[i] = (unsigned char)BT_SHOP_NONE; S.shop_id[i] = 0; }
        S.materials = 1000.0f; S.shop_timer = 0; S.free_rerolls_remaining = 1;
        S.wpn_count = 0;
        for (int i = 0; i < BT_WEAPON_SLOTS; i++) S.wpn_id[i] = BT_WEAPON_NONE;
        bt_action_mask(&S, mask);
        check(mask[BT_ACT_REROLL] && mask[BT_ACT_SKIP], "an empty shop still rerolls (free) and skips");
        check(!mask[0] && !mask[BT_ACT_LOCK0] && !mask[BT_ACT_BAN0] &&
              !mask[BT_ACT_RECYCLE0] && !mask[BT_ACT_COMBINE0],
              "nothing to buy/lock/ban/recycle/combine without a live offer or an owned weapon");

        S.character = (unsigned short)BT_CHARACTER_DEMON;
        S.weapon_slot_limit = 6;
        S.shop_kind[0] = (unsigned char)BT_SHOP_ITEM; S.shop_id[0] = 5;
        S.shop_kind[1] = (unsigned char)BT_SHOP_WEAPON; S.shop_id[1] = 118;   /* ghost scepter tier 1 */
        bt_action_mask(&S, mask);
        check(mask[0] && mask[BT_ACT_LOCK0] && mask[BT_ACT_BAN0],
              "an affordable ITEM offer is buyable, lockable and bannable");
        check(mask[BT_ACT_LOCK0 + 1] && !mask[BT_ACT_BAN0 + 1],
              "a WEAPON offer is lockable but never bannable (shop_item.gd:106-109)");
        S.shop_timer = 3;
        bt_action_mask(&S, mask);
        check(!mask[0] && mask[BT_ACT_REROLL] && mask[BT_ACT_SKIP],
              "the purchase-delay timer masks BUYS only: it lives in the container's "
              "buy/steal handlers (shop_items_container.gd:57-73,80-94), so the reroll "
              "button and skip stay live");
        S.shop_timer = 0;
        /* Demon prices in max HP (hp_shop), so make slot 0 unaffordable with Bull's gold. */
        S.character = (unsigned short)BT_CHARACTER_BULL;
        S.materials = 0.0f;
        bt_action_mask(&S, mask);
        check(!mask[0] && mask[BT_ACT_LOCK0], "an unaffordable ITEM offer is not buyable but is lockable");
        S.character = (unsigned short)BT_CHARACTER_DEMON;

        S.wpn_id[0] = 118; S.wpn_count = 1;
        bt_action_mask(&S, mask);
        check(mask[BT_ACT_RECYCLE0] && !mask[BT_ACT_COMBINE0],
              "a lone weapon recycles but cannot combine (can_combine needs a second copy)");
        S.wpn_id[1] = 118; S.wpn_count = 2;
        bt_action_mask(&S, mask);
        check(mask[BT_ACT_COMBINE0] && mask[BT_ACT_COMBINE0 + 1],
              "two copies of the same variant can be combined");
        S.wpn_id[0] = S.wpn_id[1] = BT_WEAPON_NONE; S.wpn_count = 0;
        S.shop_kind[0] = S.shop_kind[1] = (unsigned char)BT_SHOP_NONE;
        S.shop_id[0] = S.shop_id[1] = 0; S.materials = 1000.0f;

        /* level-up: only present offers, reroll needs more than one offer and gold */
        S.phase = 2;
        S.crate_kind = (unsigned char)BT_SHOP_NONE;
        for (int i = 0; i < 4; i++) S.lvl_offers[i] = 0;
        /* row+1 encoding: these are BT_UPGRADES rows 3 and 7 */
        S.lvl_offers[0] = 4; S.lvl_offers[1] = 8; S.lvl_reroll_price = 5;
        bt_action_mask(&S, mask);
        check(mask[0] && mask[1] && !mask[2] && !mask[3] && mask[BT_ACT_REROLL],
              "level-up exposes only the present offers, reroll with an affordable price");
        S.materials = 1.0f;
        bt_action_mask(&S, mask);
        check(!mask[BT_ACT_REROLL], "an unaffordable level-up reroll is masked");
        S.materials = 1000.0f; S.lvl_offers[1] = 0;
        bt_action_mask(&S, mask);
        check(!mask[1] && !mask[BT_ACT_REROLL], "a single offer hides the reroll and masks the absent slot");
        /* The weapon-slot-upgrade level-up state: slot 0 holds the sentinel
         * offer, slots 1..3 hold 0 (absent).  An absent slot must NOT read as
         * present (bt_progress_choose_upgrade would index a bogus row). */
        S.lvl_offers[0] = (unsigned short)BT_UPGRADE_SLOT_OFFER;
        for (int i = 1; i < 4; i++) S.lvl_offers[i] = 0;
        bt_action_mask(&S, mask);
        check(mask[0] && !mask[1] && !mask[2] && !mask[3],
              "the weapon-slot sentinel offer is present; absent (0) slots are not");
        /* An out-of-range value (beyond BT_NUPGRADES) is also absent. */
        S.lvl_offers[1] = (unsigned short)(BT_NUPGRADES + 1);
        bt_action_mask(&S, mask);
        check(!mask[1], "an out-of-range level-up slot value reads as absent");
        for (int i = 0; i < 4; i++) S.lvl_offers[i] = 0;

        S.phase = 0;
    }

    printf("[10] shop-owned slots 165..173 + the crate-phase mask\n");
    bt_core_init(&S, 777u, 0, 1, (unsigned short)BT_CHARACTER_DEMON, (unsigned short)BT_WEAPON_NONE);
    S.shop_locked[0] = 1; S.shop_locked[1] = 0; S.shop_locked[2] = 2; S.shop_locked[3] = 0;
    S.crate_kind = (unsigned char)BT_SHOP_ITEM;
    S.crate_id = 42;
    S.crate_cursed = 1;
    S.lvl_reroll_price = 37;
    for (int i = 0; i < BT_OBS_SIZE; i++) o[i] = SENT;
    bt_core_observe(&S, o);
    check_near(o[165], 1.0f, 1e-6f, "locked slot 0 -> 1.0");
    check_near(o[166], 0.0f, 1e-6f, "unlocked slot 1 -> 0.0");
    check_near(o[167], 1.0f, 1e-6f, "locked slot 2 (rank 2) -> 1.0");
    check_near(o[168], 0.0f, 1e-6f, "unlocked slot 3 -> 0.0");
    check_near(o[169], (float)BT_SHOP_ITEM, 1e-6f, "crate kind");
    check_near(o[170], 43.0f / (float)(BT_NITEMS + 1), 1e-6f, "crate id (idx+1)/(N+1)");
    check_near(o[171], 1.0f, 1e-6f, "crate cursed");
    check_near(o[172], 1.0f, 1e-6f, "crate ready");
    check_near(o[173], 37.0f, 1e-6f, "level-up reroll price (raw gold units)");

    {
        unsigned char mask[BT_ACTIONS];
        S.phase = 2;
        bt_action_mask(&S, mask);
        int legal = 0;
        for (int i = 0; i < BT_ACTIONS; i++) legal += mask[i] != 0;
        check(legal == 2 && mask[BT_ACT_CRATE_TAKE] && mask[BT_ACT_CRATE_RECYCLE],
              "an open crate offer exposes only the take / recycle pair");
        S.crate_kind = (unsigned char)BT_SHOP_NONE;
        for (int i = 0; i < 4; i++) S.lvl_offers[i] = 0;
        S.lvl_offers[0] = 4; S.lvl_offers[1] = 8; S.lvl_reroll_price = 5; S.materials = 1000.0f;
        bt_action_mask(&S, mask);
        legal = 0;
        for (int i = 0; i < BT_ACTIONS; i++) legal += mask[i] != 0;
        check(legal == 3 && mask[0] && mask[1] && mask[BT_ACT_REROLL] && !mask[BT_ACT_CRATE_TAKE],
              "no crate offer -> the present level-up choices plus an affordable reroll");
        S.phase = 0;
    }

    printf("[11] v5 tail: ban budget + level-up offer presence\n");
    /* bt_core_init leaves ban_mode on with BT_BAN_MAX_TOKEN tokens, so a fresh
     * run observes 1.0; ban_mode off forces 0.0 (the artifact's ban condition
     * reads this field as a nonzero gate). */
    bt_core_init(&S, 4242u, 0, 1, (unsigned short)BT_CHARACTER_DEMON, (unsigned short)BT_WEAPON_NONE);
    check(S.ban_mode == 1 && S.ban_tokens == BT_BAN_MAX_TOKEN,
          "a fresh run starts with ban_mode on and BT_BAN_MAX_TOKEN tokens");
    for (int i = 0; i < BT_OBS_SIZE; i++) o[i] = SENT;
    bt_core_observe(&S, o);
    check_near(o[198], 1.0f, 1e-6f, "obs[198] = ban_tokens / BT_BAN_MAX_TOKEN (8/8)");
    check(!any_nonzero(o, 199, 203), "no level-up offer -> all four presence flags are 0.0");
    S.ban_tokens = 3;
    bt_core_observe(&S, o);
    check_near(o[198], 3.0f / (float)BT_BAN_MAX_TOKEN, 1e-6f, "ban budget is normalised by BT_BAN_MAX_TOKEN");
    S.ban_mode = 0;
    bt_core_observe(&S, o);
    check_near(o[198], 0.0f, 1e-6f, "ban mode off -> the ban budget field is 0.0");
    S.ban_mode = 1;
    /* presence mirrors bt_lvl_offer_present in every phase, and slot 0's row 0
     * (stored as 1) is a real offer, not the absent marker. */
    S.lvl_offers[0] = 1;
    S.lvl_offers[1] = (unsigned short)BT_UPGRADE_SLOT_OFFER;
    S.lvl_offers[2] = 0;
    S.lvl_offers[3] = (unsigned short)(BT_NUPGRADES + 5);
    bt_core_observe(&S, o);
    check(o[199] == 1.0f && o[200] == 1.0f && o[201] == 0.0f && o[202] == 0.0f,
          "presence is 1.0 for a row-0 offer and the sentinel, 0.0 for absent/out-of-range");
    check_near(o[27], 0.0f, 1e-6f, "slot 0 row 0 (max_hp) emits stat 0");
    check_near(o[28], 0.3f, 1e-6f, "slot 0 row 0 emits the raw value 3.0 -> 0.3");

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
