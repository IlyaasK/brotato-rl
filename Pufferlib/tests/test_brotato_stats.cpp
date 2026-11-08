/* tests/test_brotato_stats.cpp
 *
 * Source-backed tests for the shared Brotato core stat layer:
 *   run_data.gd:1305-1313 get_stat / get_stat_gain
 *   utils.gd:461-484        get_stat / get_capped_stat
 *   temp_stats.gd:21-48     TempStats.add_stat/remove_stat/get_stat
 *   linked_stats.gd:12-78   LinkedStats.reset_player (perm_stats_only)
 *   player.gd:349-368       update_player_stats (capped hp/speed/dodge/crit)
 *   weapon_service.gd:253   weapon crit = weapon base + get_capped_stat(crit)
 *
 * Build and run:
 *     c++ -O2 -o build/test_brotato_stats tests/test_brotato_stats.cpp -lm
 *     ./build/test_brotato_stats
 *
 * Exit code 0 = all checks passed.
 */

#include "../ocean/brotato/brotato_core.h"

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

/* Advance the deferred player-stat-sync pipeline by its two frame boundaries:
 * run_data.gd:1400-1406 add_stat sets _are_player_stats_dirty ->
 * run_data.gd:221-222 RunData._physics_process call_deferred("_emit_stats_updated")
 * -> run_data.gd:1418-1427 emits "stats_updated" -> main.gd:339 on_stats_updated ->
 * global/stats_manager.gd:105-126 reload_stats queues the player ->
 * stats_manager.gd:20-31 StatsManager._physics_process runs
 * player.update_player_stats() (player.gd:349-368) on the NEXT physics frame.
 * Until both boundaries pass, a stat change has not reached the player's
 * LiveStats (max_stats.health / current_stats.speed/armor/dodge / the heal). */
static void sync_player_stats(BtState* s) {
    bt_player_stat_sync_step(s);
    bt_player_stat_sync_step(s);
}

/* BtState embeds two physics worlds; keep the big states static. */
static BtState A;

/* A GAIN_STAT_FOR_EVERY_STAT row matching (target, source, perm_stats_only). */
static int find_link_row(int target_id, int src_id, int perm_only) {
    for (int r = 0; r < BT_NITEM_EFF_ROWS; r++) {
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[r];
        if (E->opcode != BT_ITEMEFF_GAIN_STAT_FOR_EVERY_STAT) continue;
        if ((int)E->perm_stats_only != perm_only) continue;
        if (bt_core_id_from_istr(E->key) != target_id) continue;
        if (bt_core_id_from_istr(E->aux_key) != src_id) continue;
        if (E->nb_stat_scaled == 0) continue;
        return r;
    }
    return -1;
}

/* An item carrying a plain (Effect, SUM) stat row for `id`. */
static int find_plain_item(int id) {
    for (int r = 0; r < BT_NITEM_EFF_ROWS; r++) {
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[r];
        if (E->opcode != BT_ITEMEFF_EFFECT) continue;
        if (E->storage != 0) continue;
        if (bt_core_id_from_istr(E->key) != id) continue;
        if (E->value <= 0) continue;
        return (int)E->item;
    }
    return -1;
}

/* The owning item of the first effect row with `opcode`. */
static int find_item_with_opcode(int opcode) {
    for (int r = 0; r < BT_NITEM_EFF_ROWS; r++)
        if ((int)BT_ITEM_EFF_ROWS[r].opcode == opcode) return (int)BT_ITEM_EFF_ROWS[r].item;
    return -1;
}

int main(void) {
    printf("[1] reset seeds the source layers, not the observable fields\n");
    {
        bt_core_init(&A, 900u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        check_near(bt_core_stat_read(&A, 0), 30.0f, 0.0f, "effective max_hp = 10 base + Bull 20");
        check_near(A.stat_raw[0], 30.0f, 0.0f, "raw max_hp storage");
        check_near(A.max_hp, 30.0f, 0.0f, "observed max_hp = capped effective");
        check_near(A.hp, 30.0f, 0.0f, "hp starts full");
        check_near(A.stat_raw[1], 15.0f, 0.0f, "raw regen = Bull's 15 (no gain)");
        check_near(A.stat_gain[1], BT_REGEN_GAIN, 0.0f, "Bull gain_stat_hp_regeneration = x1.5");
        check_near(bt_core_stat_read(&A, 1), 22.5f, 0.0f, "Utils.get_stat(regen) = raw * 1.5");
        check_near(A.regen, 22.5f, 0.0f, "observed regen = 15 * gain 1.5");
        check_near(bt_core_stat_read(&A, 2), 10.0f, 0.0f, "raw armor = Bull's 10");
        check_near(A.armor, 10.0f, 0.0f, "observed armor");
        /* the weapon base crit (bull_explosion_stats) is NOT part of the player
         * stat: the stat reads 0 while combat crit carries the 3% base. */
        check_near(bt_core_stat_read(&A, 9), 0.0f, 0.0f, "player stat_crit_chance starts 0");
        check_near(A.crit, BT_BASE_CRIT, 0.0f, "weapon crit = base 3% + player stat");
        check_near(bt_core_stat_read(&A, 11), 0.0f, 0.0f, "player stat_crit_damage starts 0");
        check_near(A.crit_dmg, BT_BASE_CRIT_DMG, 0.0f, "weapon crit_damage = bull_explosion_stats 1.5");
        int tz = 1, lz = 1;
        for (int i = 0; i < BT_NSTATS; i++) { if (A.temp_stat[i] != 0.0f) tz = 0; if (A.link_stat[i] != 0.0f) lz = 0; }
        check(tz && lz, "temp/link layers start empty");
    }

    printf("[2] hp cap: a temporary boost is reversible and never eats stored gain\n");
    {
        bt_core_init(&A, 901u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.stat_cap[0] = 40.0f;                 /* StatCapEffect hp_cap = 40 */
        bt_core_stat_sync(&A);
        check_near(A.max_hp, 30.0f, 0.0f, "cap 40 does not lower the stored 30");

        bt_core_stat_add(&A, 0, 100.0f);       /* +100 max_hp -> stored 130 */
        check_near(bt_core_stat_read(&A, 0), 130.0f, 0.0f, "storage keeps the full 130 (uncapped)");
        check_near(A.max_hp, 40.0f, 0.0f, "effective max_hp is capped at 40");
        check_near(A.hp, 30.0f, 0.0f,
                   "the raise has not reached the player yet: the LiveStats refresh is deferred");
        sync_player_stats(&A);
        check_near(A.hp, 40.0f, 0.0f, "hp follows the capped max on the deferred update_player_stats frame");

        bt_core_stat_add_temp(&A, 0, 50.0f);   /* TempStats +50 */
        check_near(A.max_hp, 40.0f, 0.0f, "temp boost cannot exceed the cap");
        check_near(bt_core_stat_read(&A, 0), 180.0f, 0.0f, "temp layer stacks on the raw storage");

        bt_core_stat_add_temp(&A, 0, -50.0f);  /* TempStats removal */
        check_near(A.max_hp, 40.0f, 0.0f, "removing the temp restores the capped value");
        check_near(bt_core_stat_read(&A, 0), 130.0f, 0.0f, "and the stored 130 is intact");

        A.stat_cap[0] = BT_STAT_CAP_NONE;      /* cap effect expires */
        bt_core_stat_sync(&A);
        check_near(A.max_hp, 130.0f, 0.0f, "raising the cap reveals the stored 130");
        check_near(A.hp, 40.0f, 0.0f, "the player still has the pre-expiry max health");
        sync_player_stats(&A);
        check_near(A.hp, 130.0f, 0.0f, "and heals by the whole increase one pipeline later");
    }

    printf("[3] dodge cap: reversible, default 60 %%\n");
    {
        bt_core_init(&A, 902u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        check_near(A.stat_cap[4], BT_DODGE_CAP_DEFAULT, 0.0f, "dodge_cap default 60");
        bt_core_stat_add(&A, 4, 100.0f);       /* +100% dodge */
        check_near(A.dodge, 0.60f, 1e-5f, "effective dodge capped at 60%");
        bt_core_stat_add_temp(&A, 4, 20.0f);
        check_near(A.dodge, 0.60f, 1e-5f, "temp dodge cannot exceed the cap");
        bt_core_stat_add_temp(&A, 4, -20.0f);
        check_near(A.dodge, 0.60f, 1e-5f, "temp removal is exact under the cap");
    }

    printf("[4] Bull regen gain applies to every permanent gain (upgrade/item path)\n");
    {
        bt_core_init(&A, 903u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        bt_core_stat_add(&A, 1, 5.0f);          /* bt_apply_item/bt_progress use this */
        check_near(A.stat_raw[1], 20.0f, 1e-6f, "raw regen storage 15 + 5");
        check_near(bt_core_stat_read(&A, 1), 30.0f, 1e-6f, "get_stat(regen) = 20 * 1.5");
        check_near(A.regen, 30.0f, 1e-6f, "the gained regen carries the 1.5 Bull gain");

        int item = find_plain_item(1);
        check(item >= 0, "an item with a plain stat_hp_regeneration effect exists");
        if (item >= 0) {
            bt_core_init(&A, 904u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            float before = A.stat_raw[1];
            bt_apply_item(&A, (unsigned short)item);
            check(A.stat_raw[1] > before, "the item raised the raw regen storage");
            check_near(A.regen, bt_core_stat_read(&A, 1), 1e-3f,
                       "regen == Utils.get_stat(hp_regeneration) after the item");
            check(A.regen > A.stat_raw[1] + A.temp_stat[1] + A.link_stat[1],
                  "the gain multiplier applies on top of the item's raw contribution");
        }
    }

    printf("[5] weapon base crit vs the stat-link source (get_stat excludes it)\n");
    {
        /* lucky_coin: target stat_luck, source stat_crit_chance, value 2, nb 1,
         * perm_stats_only = false. */
        int row = find_link_row(10, 9, 0);
        check(row >= 0, "a non-permanent-only link off stat_crit_chance exists");
        if (row >= 0) {
            BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[row];
            bt_core_init(&A, 905u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            A.lk_count[row] = 1;
            bt_recompute_links(&A);
            check_near(A.luck, 0.0f, 0.0f,
                       "link off stat_crit_chance sees the player stat 0, not the 3% weapon base");
            check_near(A.crit, BT_BASE_CRIT, 0.0f, "combat crit still carries the weapon base");

            bt_core_stat_add(&A, 9, 10.0f);     /* player crit stat +10% */
            bt_recompute_links(&A);
            check_near(A.crit, BT_BASE_CRIT + 0.10f, 1e-6f, "combat crit = base + capped stat");
            check_near(bt_core_stat_read(&A, 9), 10.0f, 1e-6f, "stat read is the player stat only");
            check_near(A.luck, bt_stat_link_gain((float)E->value, 10.0f, (float)E->nb_stat_scaled), 1e-4f,
                       "link amount comes from the player crit stat");

            bt_core_stat_add_temp(&A, 9, 5.0f);  /* perm_stats_only=false -> temp counts */
            bt_recompute_links(&A);
            check_near(A.luck, bt_stat_link_gain((float)E->value, 15.0f, (float)E->nb_stat_scaled), 1e-4f,
                       "a non-permanent-only link includes the temp layer");
        }
    }

    printf("[6] permanent-only links ignore temporary bonuses\n");
    {
        /* stone_skin: target stat_max_hp, source stat_armor, value 1, nb 1,
         * perm_stats_only = true. */
        int row = find_link_row(0, 2, 1);
        check(row >= 0, "a permanent-only link off stat_armor exists");
        if (row >= 0) {
            BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[row];
            bt_core_init(&A, 906u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);      /* armor raw 10 */
            A.lk_count[row] = 1;
            bt_recompute_links(&A);
            float base = bt_stat_link_gain((float)E->value, 10.0f, (float)E->nb_stat_scaled);
            check_near(A.max_hp, 30.0f + base, 1e-4f, "perm-only link from the permanent armor");

            bt_core_stat_add_temp(&A, 2, 10.0f);   /* TempStats armor +10 */
            bt_recompute_links(&A);
            check_near(A.armor, 20.0f, 1e-6f, "temp armor is effective for the player");
            check_near(A.max_hp, 30.0f + base, 1e-4f,
                       "perm_stats_only link ignores the temporary armor bonus");
        }
    }

    printf("[7] bt_core_stat_reset_temp drops the whole temp layer (wave boundary)\n");
    {
        bt_core_init(&A, 907u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        bt_core_stat_add_temp(&A, 0, 12.0f);
        bt_core_stat_add_temp(&A, 2, 3.0f);
        bt_core_stat_add_temp(&A, 10, 40.0f);
        check_near(A.max_hp, 42.0f, 0.0f, "temp max_hp applied");
        check_near(A.luck, 40.0f, 0.0f, "temp luck applied");
        bt_core_stat_reset_temp(&A);
        check_near(A.max_hp, 30.0f, 0.0f, "temp max_hp cleared");
        check_near(A.armor, 10.0f, 0.0f, "temp armor cleared");
        check_near(A.luck, 0.0f, 0.0f, "temp luck cleared");
        check_near(bt_core_stat_read(&A, 2), 10.0f, 0.0f, "raw armor survived the temp reset");
    }

    printf("[8] max_hp/armor truncate at the consumer; other stats stay float\n");
    {
        bt_core_init(&A, 908u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        bt_core_stat_add(&A, 0, 3.7f);         /* raw max_hp 33.7 */
        check_near(bt_core_stat_read(&A, 0), 33.7f, 1e-4f, "raw storage keeps the fraction");
        check_near(A.max_hp, 33.0f, 0.0f, "get_player_max_health truncates 33.7 -> 33");
        check_near(A.hp, 30.0f, 0.0f, "the raise is still pending on the player's LiveStats");
        sync_player_stats(&A);
        check_near(A.hp, 33.0f, 0.0f, "hp follows the truncated max on the deferred frame");

        A.stat_cap[0] = 32.9f;
        bt_core_stat_sync(&A);
        check_near(A.max_hp, 32.0f, 0.0f, "cap 32.9 truncates the capped value to 32");
        A.stat_cap[0] = 33.0f;
        bt_core_stat_sync(&A);
        check_near(A.max_hp, 33.0f, 0.0f, "cap 33 restores 33");

        bt_core_stat_add(&A, 2, 2.9f);         /* raw armor 12.9 */
        check_near(bt_core_stat_read(&A, 2), 12.9f, 1e-4f, "raw armor keeps the fraction");
        check_near(A.armor, 12.0f, 0.0f, "max_stats.armor truncates 12.9 -> 12");

        bt_core_stat_add(&A, 1, 1.5f);         /* raw regen 16.5 */
        check_near(A.regen, 16.5f * BT_REGEN_GAIN, 1e-3f, "regen is NOT truncated (24.75)");
    }

    printf("[9] repeat copies merge in the temporary registries (no copy-9 drop)\n");
    {
        int tsi_item = find_item_with_opcode(BT_ITEMEFF_TEMP_STATS_PER_INTERVAL);
        if (tsi_item >= 0) {
            bt_core_init(&A, 909u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            bt_apply_item(&A, (unsigned short)tsi_item);
            int n1 = A.n_tsi;
            float s1 = 0.0f;
            for (int i = 0; i < n1; i++) s1 += A.tsi_val[i];
            bt_apply_item(&A, (unsigned short)tsi_item);
            float s2 = 0.0f;
            for (int i = 0; i < A.n_tsi; i++) s2 += A.tsi_val[i];
            check(A.n_tsi == n1, "a second copy takes no new TempStatsPerInterval slot");
            check_near(s2, 2.0f * s1, 1e-4f, "the TSI value SUMs across copies");
            check(A.stat_reg_overflow == 0, "no false overflow");
        }

        /* Two wisps grant +2 elemental per 30 burning kills, capped per wave. */
        bt_core_init(&A, 910u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        bt_apply_item(&A, BT_ITEM_IDX_WILL_O_THE_WISP);
        bt_apply_item(&A, BT_ITEM_IDX_WILL_O_THE_WISP);
        for (int k = 0; k < 29; k++) bt_burning_kill_stats(&A);
        check_near(A.elemental, 0.0f, 1e-6f, "no early burning-kill grant");
        bt_burning_kill_stats(&A);
        check_near(A.elemental, 2.0f, 1e-6f, "30th burning kill grants both copies");
        for (int k = 0; k < 150; k++) bt_burning_kill_stats(&A);
        check_near(A.elemental, 8.0f, 1e-6f, "each copy stops after four grants");
        bt_enter_wave(&A, 2);
        for (int k = 0; k < 30; k++) bt_burning_kill_stats(&A);
        check_near(A.elemental, 10.0f, 1e-6f, "new wave resets counters, not permanent gains");

    }

    printf("[10] player damage: get_armor_coef (incl. negative armor) and iframes\n");
    {
        /* run_data.gd:1351-1358 get_armor_coef + player.gd:310-311: the PLAYER
         * scales incoming damage by 10/(10+|armor|/1.5), and a NEGATIVE armor
         * inverts that into (1 - p) + 1 (more than 100% damage taken). */
        check_near(bt_armor_mult(0.0f), 1.0f, 0.0f, "0 armor takes full damage");
        check_near(bt_armor_mult(10.0f), 10.0f / (10.0f + 10.0f / 1.5f), 1e-5f,
                   "10 armor takes 60%");
        check_near(bt_armor_mult(-10.0f), 1.0f + (1.0f - 10.0f / (10.0f + 10.0f / 1.5f)), 1e-5f,
                   "-10 armor inverts the coefficient (140%)");

        bt_core_init(&A, 911u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        /* the damage path reads the player LiveStats (p_*), which the init
         * snapshot already filled from the stat layer (raw armor +10). */
        A.p_dodge = 0.0f; A.max_hp = 60.0f; A.p_max_hp = 60.0f; A.hp = 60.0f;
        float reward = 0.0f;
        (void)bt_player_damage(&A, 10.0f, 1, -1, &reward);
        check_near(A.hp, 54.0f, 1e-4f, "+10 armor: a 10 damage hit takes round(10*0.6) = 6");

        A.hp = 60.0f; A.iframe = 0.0f;
        A.armor = -10.0f;                                     /* max_stats.armor is an int */
        A.p_armor = -10.0f;                                   /* player.gd:310 current_stats.armor */
        (void)bt_player_damage(&A, 10.0f, 1, -1, &reward);
        check_near(A.hp, 46.0f, 1e-4f, "-10 armor: the same hit takes round(10*1.4) = 14");

        /* player.gd:565-573 get_iframes: both bounds are divided by
         * max(1, get_endless_factor()); get_endless_factor is 0 through wave 20. */
        bt_core_init(&A, 912u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        check_near(bt_iframes(&A, 10.0f, 30.0f), 0.4f, 1e-6f, "wave 1: the old 0.4 s cap");
        check_near(bt_iframes(&A, 1.0f, 100.0f), 0.2f, 1e-6f, "wave 1: the old 0.2 s floor");
        A.wave = 21;
        check_near(bt_iframes(&A, 10.0f, 30.0f), 0.4f, 1e-6f,
                   "endless_factor 0.04 still divides by max(1, .)");
        A.wave = 30;                                          /* factor 0.55*2 = 1.1 */
        check_near(bt_iframes(&A, 10.0f, 30.0f), 0.4f / 1.1f, 1e-5f,
                   "wave 30 endless: the frame cap shrinks with the endless factor");
        check_near(bt_iframes(&A, 1.0f, 100.0f), 0.2f / 1.1f, 1e-5f,
                   "wave 30 endless: the frame floor shrinks too");
    }

    printf("[11] bt_rf64_div is bit-exact against hardware binary64\n");
    {
        /* The source's `/100.0` and `1.0 + pct/100.0` are binary64; on devices
         * without FP64 (Metal) the core emulates them with bt_rf64_*.  Division is
         * checked against the hardware double bit-for-bit over a spread of
         * positive/negative float pairs plus the literal 100.0 the game uses. */
        static const float nums[] = {
            0.0f, 1.0f, 3.0f, 5.0f, 7.9f, 13.0f, 16.0f, 100.0f, 1000.0f,
            -1.0f, -66.0f, -130.0f, 0.5f, 0.1f, 1e-3f, 12345.678f, 3.4e38f
        };
        int n = (int)(sizeof(nums) / sizeof(nums[0]));
        int mismatches = 0;
        for (int i = 0; i < n; i++) {
            for (int j = 0; j < n; j++) {
                if (nums[i] == 0.0f && nums[j] == 0.0f) continue;   /* 0/0 is a NaN whose
                    * sign bit is platform-specific; unreachable in-game */
                BtRngF64 emu = bt_rf64_div_ff(nums[i], nums[j]);
                float emu_f = bt_rf64_to_float(emu);
                double hw = (double)nums[i] / (double)nums[j];
                float hw_f = (float)hw;
                if (emu_f != hw_f) mismatches++;
            }
        }
        check(mismatches == 0, "emulated binary64 division matches the hardware result for every pair");

        /* 1.0 + pct/100.0 is the gain multiplier the offer/harvest paths use. */
        check_near(bt_rf64_to_float(bt_rf64_one_plus_pct(35.0f)), 1.35f, 1e-7f,
                   "1 + 35/100 = 1.35");
        check_near(bt_rf64_to_float(bt_rf64_one_plus_pct(-66.0f)), 0.34f, 1e-7f,
                   "1 + (-66)/100 = 0.34");
        check_near(bt_rf64_to_float(bt_rf64_div_ff(7.9f, 100.0f)), 0.079f, 1e-9f,
                   "7.9 / 100 = 0.079");

        /* item.gd's attraction capacity: speed 500 + 20/frame, /60 per frame. */
        check_near(bt_loot_flight_capacity(0), 0.0f, 0.0f, "0 frames travel nothing");
        check_near(bt_loot_flight_capacity(1), 500.0f / 60.0f, 1e-4f,
                   "one attracted frame travels _current_speed/60");
        check_near(bt_loot_flight_capacity(2), (500.0f + 520.0f) / 60.0f, 1e-4f,
                   "the ramp adds ATTRACT_ACCELERATION each frame");
    }

    printf("[12] gold-bag geometry reproduces the measured probe row\n");
    {
        /* run_match41 meta.environment.viewport_probe: visible 1920x1080,
         * canvas_origin (-64,-228), zoom (1,1), camera (1024,768),
         * ui_bonus_gold_pos (56,56), gold_bag_pos (120,284).  The recorded
         * identity is gold_bag_pos = ui_bonus_gold_pos - canvas_origin, and
         * canvas_origin = viewport/2 - camera, so
         *   bag = camera + (hud - viewport/2).
         * bt_core_reset already seeds the pinned defaults; assert the model
         * reproduces the probed pixels exactly. */
        bt_core_init(&A, 920u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        check_near(A.bag_view_w, 1920.0f, 0.0f, "pinned viewport width");
        check_near(A.bag_view_h, 1080.0f, 0.0f, "pinned viewport height");
        check_near(A.bag_zoom, 1.0f, 0.0f, "pinned zoom");
        check_near(A.bag_hud_x, 56.0f, 0.0f, "pinned HUD anchor x (measured)");
        check_near(A.bag_hud_y, 56.0f, 0.0f, "pinned HUD anchor y (measured)");
        A.px = 1024.0f; A.py = 768.0f;               /* the probed camera centre */
        {
            float bx, by;
            bt_loot_bag_pos(&A, &bx, &by);
            check_near(bx, 120.0f, 1e-3f, "bag world x matches the probe (120)");
            check_near(by, 284.0f, 1e-3f, "bag world y matches the probe (284)");
            /* the same identity the probe records: bag = hud - canvas_origin */
            check_near(bx, 56.0f - (-64.0f), 1e-3f, "bag = hud - canvas_origin (x)");
            check_near(by, 56.0f - (-228.0f), 1e-3f, "bag = hud - canvas_origin (y)");
        }
        /* the offset is a pure translation: it does not depend on where the
         * player is, so a kiting player drags the bag along unchanged */
        A.px = 300.0f; A.py = 1200.0f;
        {
            float bx, by;
            bt_loot_bag_pos(&A, &bx, &by);
            check_near(bx, 300.0f - 904.0f, 1e-3f, "bag offset x is translation-invariant");
            check_near(by, 1200.0f - 484.0f, 1e-3f, "bag offset y is translation-invariant");
        }
        /* a different pinned pair is injectable (host hook) */
        bt_loot_set_bag_view(&A, 1280.0f, 720.0f, 1.0f, 40.0f, 40.0f);
        {
            float bx, by;
            bt_loot_bag_pos(&A, &bx, &by);
            check_near(bx, 300.0f + (40.0f - 640.0f), 1e-3f, "injected viewport pins the bag x");
            check_near(by, 1200.0f + (40.0f - 360.0f), 1e-3f, "injected viewport pins the bag y");
        }
    }

    printf("[13] deferred player stat sync: every stat change lands two frames later\n");
    {
        /* The game's player-scope values are not written by add_stat: it only
         * sets _are_player_stats_dirty (run_data.gd:1400-1406).  RunData's
         * _physics_process queues a deferred _emit_stats_updated (run_data.gd:
         * 221-222) whose flush emits "stats_updated" (run_data.gd:1418-1427) ->
         * main.gd:339 on_stats_updated -> stats_manager.gd:105-126 reload_stats
         * queues the player -> stats_manager.gd:20-31 _physics_process calls
         * player.update_player_stats() (player.gd:349-368) one physics frame
         * later.  Measured on v4_demon_s1/probe_s1/probe_s2: demon_effect_1's
         * wave-end conversion raises max HP at row 179 and the current-health
         * heal lands at row 181. */
        bt_core_init(&A, 913u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.p_speed_pct = 0.0f;   /* BULL: no speed/armor stat beyond the base armor */

        /* max_hp +10: the stat layer moves at once, the LiveStats heal waits. */
        float hp0 = A.hp, max0 = A.max_hp, p0 = A.p_max_hp;
        bt_core_stat_add(&A, 0, 10.0f);
        check_near(A.max_hp, max0 + 10.0f, 0.0f, "max_hp stat layer moves in the add frame");
        check_near(A.hp, hp0, 0.0f, "current health is untouched in the add frame");
        bt_player_stat_sync_step(&A);
        check_near(A.hp, hp0, 0.0f, "after ONE frame boundary the refresh is only queued");
        check_near(A.p_max_hp, p0, 0.0f, "and the LiveStats max is still the old one");
        bt_player_stat_sync_step(&A);
        check_near(A.p_max_hp, max0 + 10.0f, 0.0f, "after TWO the LiveStats max catches up");
        check_near(A.hp, hp0 + 10.0f, 0.0f, "and the heal lands by the increase");

        /* A second stat feeds the same pipeline: armor lags for the damage path.
         * player.gd:310 reads current_stats.armor, so the +5 must not reduce
         * damage until the deferred pass has run. */
        float arm0 = A.p_armor;
        bt_core_stat_add(&A, 2, 5.0f);
        check_near(A.armor, arm0 + 5.0f, 0.0f, "armor stat layer moves in the add frame");
        check_near(A.p_armor, arm0, 0.0f, "the damage path still sees the old armor");
        bt_player_stat_sync_step(&A);
        check_near(A.p_armor, arm0, 0.0f, "one boundary: still queued");
        bt_player_stat_sync_step(&A);
        check_near(A.p_armor, arm0 + 5.0f, 0.0f, "two boundaries: the damage path sees +5");

        /* speed, same pipeline (unit.gd:615 movement speed). */
        float sp0 = A.p_speed_pct;
        bt_core_stat_add(&A, 3, 20.0f);
        check_near(A.p_speed_pct, sp0, 0.0f, "a speed change does not move the LiveStats yet");
        sync_player_stats(&A);
        check_near(A.p_speed_pct, sp0 + 20.0f, 0.0f, "speed lands on the same deferred pass");

        /* A fresh wave reseeds the snapshot (player.gd:349 update_player_stats
         * (true) from the rebuilt player's _ready): no pending heal. */
        bt_core_stat_add(&A, 0, 10.0f);
        bt_player_stats_reset(&A);
        check_near(A.p_max_hp, A.max_hp, 0.0f, "wave-start reset syncs the LiveStats max");
        check_near(A.hp, A.max_hp, 0.0f, "and starts the wave at full health");
        check(A.stat_dirty == 0 && A.stat_refresh_queued == 0, "the wave-start reset clears the pipeline");
    }

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
