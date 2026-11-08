/* tests/test_brotato_temporal_events.cpp
 *
 * Source-backed regressions for the TempStats trigger consumers slice
 * (ocean/brotato/brotato_temporal_events.h):
 *   - temp_stats_on_hit   (triangle_of_power) : add on every damaging blow
 *   - temp_stats_while_not_moving / while_moving (barricade/chameleon/statue/
 *     DLC coral) : add/remove on the movement transitions
 *   - temp_stats_on_dodge (no Bull-reachable producer) : provable no-op
 *   - the source Effect KEY_VALUE storage for those rows
 *   - the damage_against_bosses target multiplier (silver_bullet)
 *
 * Every expectation is transcribed from the recovered sources:
 *   temp_stats.gd:15-42            add_stat / remove_stat / get_stat (raw temp
 *                                  layer, gain applied on read only)
 *   player.gd:215-266              check_not_moving_stats / check_moving_stats
 *   player.gd:465-468              dodge branch: temp_stats_on_dodge
 *   player.gd:487-490              dmg_taken > 0: temp_stats_on_hit
 *   player.gd:1030-1051            MovingTimer/NotMovingTimer -> percent_materials
 *   player.tscn:1388-1391          both timers: Timer defaults, wait_time 1.0
 *   effect.gd:66-71                KEY_VALUE bucket[custom_key] += [key, value]
 *   utils.gd:461-484               get_stat / get_capped_stat
 *   run_data.gd:1305-1313          get_stat = effect * get_stat_gain
 *   boss.gd:52-58                  damage_against_bosses multiplier
 *   items/all/triangle_of_power_effect_3.tres  stat_percent_damage -2, on_hit
 *   items/all/chameleon_effect_1.tres          stat_dodge +20, while_not_moving
 *   items/all/barricade_effect_1.tres          stat_armor +8, while_not_moving
 *   items/all/statue_effect_1.tres             stat_attack_speed +40, while_not_moving
 *
 * Build and run:
 *     c++ -O2 -o build/test_brotato_temporal_events tests/test_brotato_temporal_events.cpp -lm
 *     ./build/test_brotato_temporal_events
 *
 * Exit code 0 = all checks passed.
 */

#include "../ocean/brotato/brotato_core.h"
/* The slice is included by brotato_core.h after integration; include it here so
 * the test also compiles while the core include is still being wired (pragma
 * once keeps the second include a no-op). */
#include "../ocean/brotato/brotato_temporal_events.h"

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

/* BtState embeds two physics worlds; keep the big state static. */
static BtState A;

/* Core stat ids used below (bt_core_id_from_istr). */
enum { ST_ARMOR = 2, ST_DODGE = 4, ST_PCT_DMG = 5, ST_ATK_SPD = 15 };

/* The first KEY_VALUE row whose custom_key is `bucket` and whose key maps to
 * core stat `id`. */
static int find_row(unsigned short bucket, int id, int* value_out, int* opcode_out) {
    for (int r = 0; r < BT_NITEM_EFF_ROWS; r++) {
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[r];
        if (E->custom_key != bucket) continue;
        if (E->storage != 1) continue;                 /* KEY_VALUE */
        if (bt_core_id_from_istr(E->key) != id) continue;
        if (value_out) *value_out = E->value;
        if (opcode_out) *opcode_out = (int)E->opcode;
        return r;
    }
    return -1;
}

/* The first KEY_VALUE row whose custom_key is `bucket` and whose raw key is the
 * interned `key` (used for keys with no core stat id, e.g. stats_next_wave). */
static int find_row_by_key(unsigned short bucket, unsigned short key, int* value_out) {
    for (int r = 0; r < BT_NITEM_EFF_ROWS; r++) {
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[r];
        if (E->custom_key != bucket) continue;
        if (E->storage != 1) continue;                 /* KEY_VALUE */
        if (E->key != key) continue;
        if (value_out) *value_out = E->value;
        return r;
    }
    return -1;
}

static void fresh(BtState* s, bt_u64 seed) {
    bt_core_init(s, seed, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
    s->n_live_groups = 0;
    bt_core_stat_add(s, 1, -15.0f);   /* disable regen in its raw layer */
    s->regen_timer = 99.0f;
    /* the movement flags + temp-scalar layer are parent-owned BtState
     * additions and start clear */
    s->moving_bonuses_applied = 0;
    s->not_moving_bonuses_applied = 0;
    for (int i = 0; i < BT_NITEM_STRS; i++) s->temp_scalar[i] = 0.0f;
}

int main(void) {
    printf("[1] on-hit: temp layer only, stacks, stat_raw untouched\n");
    {
        int tri_val = 0, tri_op = -1;
        int row = find_row(BT_TEMPORAL_KEY_ON_HIT, ST_PCT_DMG, &tri_val, &tri_op);
        check(row >= 0, "triangle_of_power temp_stats_on_hit row exists");
        if (row >= 0) {
            check(tri_val == -2, "triangle_of_power on-hit value is -2");
            check(tri_op == BT_ITEMEFF_EFFECT, "it is a plain Effect row");

            fresh(&A, 1201u);
            float raw0 = A.stat_raw[ST_PCT_DMG];
            float read0 = bt_core_stat_read(&A, ST_PCT_DMG);

            bt_temporal_bucket_register(&A, row, tri_val, 0);
            check(bt_temporal_has(&A, BT_TEMPORAL_KEY_ON_HIT),
                  "registration makes the on-hit bucket visible");
            check_near(A.temp_stat[ST_PCT_DMG], 0.0f, 0.0f,
                       "acquiring the item applies nothing (trigger-only, player.gd:487)");

            float applied = bt_temporal_on_hit(&A);
            check_near(applied, -2.0f, 1e-6f, "one hit pushes -2 into the temp layer");
            check_near(A.temp_stat[ST_PCT_DMG], -2.0f, 1e-6f, "temp_stat holds the raw -2");
            check_near(bt_core_stat_read(&A, ST_PCT_DMG), read0 - 2.0f, 1e-6f,
                       "effective percent_damage drops by 2");
            check_near(A.stat_raw[ST_PCT_DMG], raw0, 0.0f,
                       "stat_raw is never written (temp boundary)");

            bt_temporal_on_hit(&A);
            check_near(A.temp_stat[ST_PCT_DMG], -4.0f, 1e-6f,
                       "on-hit stacks every blow (no per-hit lifetime)");
            check(A.n_bk_active == 1, "one bucket row, not one per trigger");
        }
    }

    printf("[2] multiplicity: every owned copy contributes its own value (rowvar)\n");
    {
        int tri_val = 0;
        int row = find_row(BT_TEMPORAL_KEY_ON_HIT, ST_PCT_DMG, &tri_val, NULL);
        if (row >= 0) {
            fresh(&A, 1202u);
            bt_temporal_bucket_register(&A, row, tri_val, 0);
            bt_temporal_bucket_register(&A, row, tri_val, 0);
            check(bt_row_owned(&A, row) == 2, "two owned copies recorded");
            check_near(bt_temporal_on_hit(&A), -4.0f, 1e-6f, "two copies -> -4 per hit");

            bt_temporal_bucket_register(&A, row, -6, 40);   /* a cursed copy */
            check(bt_row_owned(&A, row) == 3, "three owned copies recorded");
            check_near(bt_temporal_on_hit(&A), -10.0f, 1e-6f,
                       "per-copy values aggregate (-2 -2 -6)");
            check_near(A.temp_stat[ST_PCT_DMG], -14.0f, 1e-6f, "temp layer sum");
        }
    }

    printf("[3] raw/effective/cap boundaries (chameleon dodge while still)\n");
    {
        int cham = 0;
        int row = find_row(BT_TEMPORAL_KEY_WHILE_NOT_MOVING, ST_DODGE, &cham, NULL);
        check(row >= 0, "chameleon temp_stats_while_not_moving dodge row exists");
        if (row >= 0) {
            check(cham == 20, "chameleon while-not-moving dodge is +20");

            fresh(&A, 1203u);
            A.stat_cap[ST_DODGE] = 10.0f;      /* dodge_cap 10 (default is 60) */
            bt_core_stat_sync(&A);
            bt_temporal_bucket_register(&A, row, cham, 0);

            check_near(A.dodge, 0.0f, 0.0f, "not moving yet: no temp");
            bt_temporal_set_moving(&A, 0);
            check(A.not_moving_bonuses_applied == 1, "still flag set on the transition");
            check_near(A.temp_stat[ST_DODGE], 20.0f, 1e-6f,
                       "temp layer stores the UNCAPPED +20");
            check_near(bt_core_stat_read(&A, ST_DODGE), 20.0f, 1e-6f,
                       "uncapped effective (utils.gd:461 get_stat)");
            check_near(A.dodge, 0.10f, 1e-6f,
                       "observable dodge is capped (utils.gd:469 get_capped_stat)");
            check_near(A.stat_raw[ST_DODGE], 0.0f, 0.0f, "raw dodge untouched");

            bt_temporal_set_moving(&A, 0);
            check_near(A.temp_stat[ST_DODGE], 20.0f, 1e-6f, "still->still does not re-add");

            bt_temporal_set_moving(&A, 1);
            check(A.not_moving_bonuses_applied == 0, "still->move clears the flag");
            check_near(A.temp_stat[ST_DODGE], 0.0f, 1e-6f, "temp removed on the transition");
            check_near(A.dodge, 0.0f, 1e-6f, "dodge reversible (storage never clamped)");

            bt_temporal_set_moving(&A, 1);
            check_near(A.temp_stat[ST_DODGE], 0.0f, 1e-6f, "move->move is a no-op");
        }
    }

    printf("[4] movement aggregation + wave-boundary reset lifetime\n");
    {
        int statue_v = 0, barricade_v = 0, cham_v = 0;
        int r_as = find_row(BT_TEMPORAL_KEY_WHILE_NOT_MOVING, ST_ATK_SPD, &statue_v, NULL);
        int r_ar = find_row(BT_TEMPORAL_KEY_WHILE_NOT_MOVING, ST_ARMOR, &barricade_v, NULL);
        int r_dg = find_row(BT_TEMPORAL_KEY_WHILE_NOT_MOVING, ST_DODGE, &cham_v, NULL);
        check(r_as >= 0 && r_ar >= 0 && r_dg >= 0, "statue/barricade/chameleon rows exist");
        if (r_as >= 0 && r_ar >= 0 && r_dg >= 0) {
            fresh(&A, 1204u);
            bt_temporal_bucket_register(&A, r_as, statue_v, 0);
            bt_temporal_bucket_register(&A, r_ar, barricade_v, 0);
            bt_temporal_bucket_register(&A, r_dg, cham_v, 0);

            bt_temporal_set_moving(&A, 0);
            check_near(A.temp_stat[ST_ATK_SPD], (float)statue_v, 1e-6f, "statue attack_speed");
            check_near(A.temp_stat[ST_ARMOR], (float)barricade_v, 1e-6f, "barricade armor");
            check_near(A.temp_stat[ST_DODGE], (float)cham_v, 1e-6f, "chameleon dodge");

            bt_temporal_set_moving(&A, 1);
            check_near(A.temp_stat[ST_ATK_SPD], 0.0f, 1e-6f, "attack_speed removed on move");
            check_near(A.temp_stat[ST_ARMOR], 0.0f, 1e-6f, "armor removed on move");

            bt_temporal_set_moving(&A, 0);
            check_near(A.temp_stat[ST_ATK_SPD], (float)statue_v, 1e-6f, "re-added on stop");

            /* main.gd:1234 TempStats.reset -- the two helpers MUST be called
             * together, otherwise the flags survive the temp wipe. */
            bt_core_stat_reset_temp(&A);
            bt_temporal_reset(&A);
            check(A.moving_bonuses_applied == 0 && A.not_moving_bonuses_applied == 0,
                  "wave reset clears both transition flags");
            check_near(A.temp_stat[ST_ATK_SPD], 0.0f, 1e-6f, "temp layer wiped");

            bt_temporal_set_moving(&A, 1);
            check_near(A.temp_stat[ST_ATK_SPD], 0.0f, 1e-6f,
                       "no double-subtract after a reset (flags cleared with the temp)");
            bt_temporal_set_moving(&A, 0);
            check_near(A.temp_stat[ST_ATK_SPD], (float)statue_v, 1e-6f,
                       "a new wave re-applies the still bonus");
        }
    }

    printf("[5] on-dodge / while-moving: no producer in the Bull pool -> no-op\n");
    {
        /* No BT_ITEM_EFF_ROWS row may carry either sentinel (it is not a real
         * interned id), and neither dispatch may move the temp layer. */
        int stray = 0;
        for (int r = 0; r < BT_NITEM_EFF_ROWS; r++) {
            if (BT_ITEM_EFF_ROWS[r].custom_key == BT_TEMPORAL_KEY_ON_DODGE) stray++;
            if (BT_ITEM_EFF_ROWS[r].custom_key == BT_TEMPORAL_KEY_WHILE_MOVING) stray++;
        }
        check(stray == 0, "no row carries the on-dodge / while-moving sentinel key");

        int cham_v = 0;
        int r_dg = find_row(BT_TEMPORAL_KEY_WHILE_NOT_MOVING, ST_DODGE, &cham_v, NULL);
        fresh(&A, 1205u);
        check(bt_temporal_has(&A, BT_TEMPORAL_KEY_ON_DODGE) == 0, "on-dodge bucket empty");
        check_near(bt_temporal_on_dodge(&A), 0.0f, 0.0f, "on-dodge dispatch is a no-op");
        check_near(A.temp_stat[ST_DODGE], 0.0f, 0.0f, "on-dodge changed nothing");

        if (r_dg >= 0) {
            bt_temporal_bucket_register(&A, r_dg, cham_v, 0);
            /* entering "moving" must not touch the not-moving bucket */
            bt_temporal_set_moving(&A, 1);
            check(A.moving_bonuses_applied == 0, "while-moving flag never sets (no producer)");
            check_near(A.temp_stat[ST_DODGE], 0.0f, 0.0f,
                       "move-in does not apply the still bucket");
            bt_temporal_set_moving(&A, 0);
            check_near(A.temp_stat[ST_DODGE], (float)cham_v, 1e-6f, "stop applies it");
            bt_temporal_set_moving(&A, 1);
            check_near(A.temp_stat[ST_DODGE], 0.0f, 1e-6f, "move removes it");
        }
    }

    printf("[6] end-to-end: bt_apply_item routes temp rows to the bucket only\n");
    {
        /* Requires the parent's bt_apply_item hook (see the PARENT REQUIREMENT
         * block in the header). Without it the temp value would leak into
         * stat_raw through bt_core_stat_add. */
        fresh(&A, 1206u);
        bt_apply_item(&A, BT_ITEM_IDX_TRIANGLE_OF_POWER);
        check_near(A.stat_raw[ST_PCT_DMG], 20.0f, 0.0f,
                   "triangle_of_power permanent +20 only (the -2 stays out of raw)");
        check(bt_temporal_has(&A, BT_TEMPORAL_KEY_ON_HIT),
              "triangle_of_power registered the on-hit bucket");
        check_near(A.temp_stat[ST_PCT_DMG], 0.0f, 0.0f, "nothing applied at acquisition");
        check_near(bt_temporal_on_hit(&A), -2.0f, 1e-6f, "hit applies -2 through the item");
        check_near(bt_core_stat_read(&A, ST_PCT_DMG), 18.0f, 1e-6f,
                   "effective percent_damage = 20 - 2");

        bt_apply_item(&A, BT_ITEM_IDX_TRIANGLE_OF_POWER);
        check_near(bt_temporal_on_hit(&A), -4.0f, 1e-6f,
                   "a second copy doubles the per-hit value (rowvar multiplicity)");
        check_near(A.stat_raw[ST_PCT_DMG], 40.0f, 0.0f, "permanent part stacks too");

        fresh(&A, 1207u);
        bt_apply_item(&A, BT_ITEM_IDX_CHAMELEON);
        check_near(A.stat_raw[ST_DODGE], 3.0f, 0.0f,
                   "chameleon permanent +3 dodge (effect_0)");
        check(bt_temporal_has(&A, BT_TEMPORAL_KEY_WHILE_NOT_MOVING),
              "chameleon registered the while-not-moving bucket");
        check_near(A.temp_stat[ST_DODGE], 0.0f, 0.0f, "moving yet: no temp dodge");
        bt_temporal_set_moving(&A, 0);
        check_near(A.temp_stat[ST_DODGE], 20.0f, 1e-6f, "still: +20 temp dodge");
        check_near(bt_core_stat_read(&A, ST_DODGE), 23.0f, 1e-6f, "effective 3 + 20");
        bt_temporal_set_moving(&A, 1);
        check_near(bt_core_stat_read(&A, ST_DODGE), 3.0f, 1e-6f, "moving: back to 3");
    }

    printf("[7] damage_against_bosses target multiplier (silver_bullet)\n");
    {
        fresh(&A, 1208u);
        check_near(bt_temporal_target_damage_mult(&A, BT_ISTR_DAMAGE_AGAINST_BOSSES), 1.0f, 0.0f,
                   "no effect owned -> x1");
        A.scalar_effect[BT_ISTR_DAMAGE_AGAINST_BOSSES] = 25.0f;
        check_near(bt_temporal_target_damage_mult(&A, BT_ISTR_DAMAGE_AGAINST_BOSSES), 1.25f, 1e-6f,
                   "+25% against bosses");
        A.scalar_effect[BT_ISTR_DAMAGE_AGAINST_BOSSES] = -10.0f;
        check_near(bt_temporal_target_damage_mult(&A, BT_ISTR_DAMAGE_AGAINST_BOSSES), 1.0f, 1e-6f,
                   "gate is get_player_effect > 0 (boss.gd:56)");

        fresh(&A, 1209u);
        bt_apply_item(&A, BT_ITEM_IDX_SILVER_BULLET);
        check_near(A.scalar_effect[BT_ISTR_DAMAGE_AGAINST_BOSSES], 25.0f, 0.0f,
                   "silver_bullet +25 lands in scalar_effect (unmapped stat)");
        check_near(bt_temporal_target_damage_mult(&A, BT_ISTR_DAMAGE_AGAINST_BOSSES), 1.25f, 1e-6f,
                   "end-to-end +25% boss multiplier");
    }

    printf("[8] stats_next_wave: temp at wave start, one-shot, no permanent leak\n");
    {
        int v = 0;
        int row = find_row_by_key(BT_TEMPORAL_KEY_STATS_NEXT_WAVE, BT_ISTR_XP_GAIN, &v);
        check(row >= 0, "a stats_next_wave xp_gain row exists (peacock/celery_tea)");
        if (row >= 0) {
            check(v == 100 || v == 50, "xp_gain next-wave value is +50 or +100");
            fresh(&A, 1210u);
            bt_temporal_bucket_register(&A, row, v, 0);
            check(bt_temporal_has(&A, BT_TEMPORAL_KEY_STATS_NEXT_WAVE), "bucket registered");
            check_near(A.temp_scalar[BT_ISTR_XP_GAIN], 0.0f, 0.0f,
                       "nothing applied at acquisition (main.gd:1402 is wave-start)");

            bt_temporal_apply_stats_next_wave(&A);
            check_near(A.temp_scalar[BT_ISTR_XP_GAIN], (float)v, 1e-6f,
                       "moved into the temp layer at wave start");
            check_near(A.scalar_effect[BT_ISTR_XP_GAIN], 0.0f, 0.0f,
                       "no permanent scalar leak");
            check(!bt_temporal_has(&A, BT_TEMPORAL_KEY_STATS_NEXT_WAVE),
                  "bucket cleared after applying (effects[...].clear())");

            bt_temporal_apply_stats_next_wave(&A);
            check_near(A.temp_scalar[BT_ISTR_XP_GAIN], (float)v, 1e-6f,
                       "one-shot: a second wave start does not re-apply");
            check_near(bt_temporal_scalar_effective(&A, BT_ISTR_XP_GAIN), (float)v, 1e-6f,
                       "effective = permanent + temp");

            bt_temporal_reset(&A);
            check_near(A.temp_scalar[BT_ISTR_XP_GAIN], 0.0f, 0.0f,
                       "wave reset drops the temp scalar");

            /* re-acquire after a clear: only the new copy may count */
            bt_temporal_bucket_register(&A, row, v, 0);
            bt_temporal_apply_stats_next_wave(&A);
            check_near(A.temp_scalar[BT_ISTR_XP_GAIN], (float)v, 1e-6f,
                       "a re-acquired copy applies its own value (stale variants cleared)");
        }

        /* clearing only the stats_next_wave rows must not disturb other buckets */
        int cham_v = 0;
        int r_dg = find_row(BT_TEMPORAL_KEY_WHILE_NOT_MOVING, ST_DODGE, &cham_v, NULL);
        if (row >= 0 && r_dg >= 0) {
            fresh(&A, 1211u);
            bt_temporal_bucket_register(&A, r_dg, cham_v, 0);
            bt_temporal_bucket_register(&A, row, v, 0);
            int n_before = A.n_bk_active;
            check(n_before == 2, "two buckets registered");
            bt_temporal_apply_stats_next_wave(&A);
            check(A.n_bk_active == 1, "only the stats_next_wave row was removed");
            check(bt_temporal_has(&A, BT_TEMPORAL_KEY_WHILE_NOT_MOVING) == 1,
                  "the while-not-moving bucket survives");
        }
    }

    printf("[9] end-to-end: peacock's next-wave stats route through the bucket\n");
    {
        /* peacock_effect_2 (xp_gain +100) + effect_3 (enemy_damage +50) are
         * stats_next_wave rows; peacock_effect_1 is a permanent xp_gain +25. */
        fresh(&A, 1212u);
        bt_apply_item(&A, BT_ITEM_IDX_PEACOCK);
        check_near(A.scalar_effect[BT_ISTR_XP_GAIN], 25.0f, 0.0f,
                   "only the permanent +25 xp_gain lands in scalar_effect");
        check(bt_temporal_has(&A, BT_TEMPORAL_KEY_STATS_NEXT_WAVE),
              "peacock registered the stats_next_wave bucket");
        check_near(A.temp_scalar[BT_ISTR_XP_GAIN], 0.0f, 0.0f, "nothing at acquisition");
        check_near(A.temp_scalar[BT_ISTR_ENEMY_DAMAGE], 0.0f, 0.0f, "nothing at acquisition");

        bt_temporal_apply_stats_next_wave(&A);
        check_near(A.temp_scalar[BT_ISTR_XP_GAIN], 100.0f, 1e-6f, "peacock +100% XP next wave");
        check_near(A.temp_scalar[BT_ISTR_ENEMY_DAMAGE], 50.0f, 1e-6f, "peacock +50 enemy damage");
        check_near(bt_temporal_scalar_effective(&A, BT_ISTR_XP_GAIN), 125.0f, 1e-6f,
                   "effective xp_gain = 25 permanent + 100 next-wave");
        check(!bt_temporal_has(&A, BT_TEMPORAL_KEY_STATS_NEXT_WAVE), "bucket cleared");
        bt_temporal_reset(&A);
        check_near(A.temp_scalar[BT_ISTR_XP_GAIN], 0.0f, 0.0f, "next wave is temporary");
        check_near(A.scalar_effect[BT_ISTR_XP_GAIN], 25.0f, 0.0f,
                   "the permanent +25 survives the wave reset");
    }

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
