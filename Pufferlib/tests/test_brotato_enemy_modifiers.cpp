/* tests/test_brotato_enemy_modifiers.cpp
 *
 * Source-backed regressions for the enemy-side stat-modifier consumers slice
 * (ocean/brotato/brotato_enemy_modifiers.h):
 *
 *   G7 -- the enemy_damage / enemy_health / enemy_speed consumers
 *         (entity_service.gd get_final_enemy_damage/_health/_speed through
 *         unit.gd reset_*_stat and enemy.gd set_attack_behavior_damage), each
 *         reading the permanent SUM (scalar_effect) and this wave's
 *         stats_next_wave TEMP (temp_scalar) through
 *         bt_temporal_scalar_effective.
 *   G1 -- the boss.gd:52-58 damage_against_bosses receive multiplier, applied
 *         to elite/boss targets at every player-sourced damage path, with the
 *         source's `int(value * mult)` truncation and the `get_player_effect>0`
 *         gate, and NO effect on plain enemies / neutrals / when unowned.
 *
 * Every expectation is transcribed from the recovered sources:
 *   entity_service.gd:11-31  get_final_enemy_damage (danger*effects, round)
 *   entity_service.gd:33-53  get_final_enemy_health (same shape)
 *   entity_service.gd:55-66  get_final_enemy_speed (effects*access*endless)
 *   unit.gd:104-126          reset_health/damage/speed_stat
 *   unit.gd:113-117          get_speed_effect_mods = 1 + enemy_speed/100
 *   enemy.gd:148-151         set_attack_behavior_damage -> get_final_enemy_damage
 *   boss.gd:52-58            take_damage: int(value * (1 + stat/100)) if owned
 *   utils.gd:461-484         get_stat / sum_all_player_stats
 *   main.gd:1402-1407        stats_next_wave -> TempStats.add_stat then clear
 *   producers: black_flag_effect_2 (enemy_damage +10, permanent)
 *              starfish_effect_1   (enemy_damage +15, permanent)
 *              peacock_effect_3    (enemy_damage +50, stats_next_wave)
 *              snail_effect_1      (enemy_speed  -8,  permanent)
 *              scarf_effect_3      (enemy_speed  +25, stats_next_wave)
 *              black_flag_effect_1 (enemy_health +10, permanent)
 *              celery_tea_effect_1 (enemy_health +100, stats_next_wave)
 *              silver_bullet_effect_1 (damage_against_bosses +25, permanent)
 *
 * Build and run:
 *     c++ -O2 -o build/test_brotato_enemy_modifiers tests/test_brotato_enemy_modifiers.cpp -lm
 *     ./build/test_brotato_enemy_modifiers
 *
 * Exit code 0 = all checks passed.
 *
 * NOTE: like the other slice tests this compiles once the parent has applied
 * the brotato_temporal_events.h state additions (`temp_scalar[]`, the two
 * bonus flags) -- this slice never edits state itself.
 */

#include "../ocean/brotato/brotato_core.h"
/* The slice is included by brotato_core.h after integration; include it here so
 * the test also compiles while the core include is still being wired (pragma
 * once keeps the second include a no-op; it pulls brotato_temporal_events.h). */
#include "../ocean/brotato/brotato_enemy_modifiers.h"

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

/* Sanity guards for the table constants this test depends on. */
enum { SLOT_ENEMY = 0, SLOT_ELITE = 1, SLOT_BOSS = 2, SLOT_NEUTRAL = 3 };

/* Fresh environment: nothing owned, D5 danger, four typed slots. */
static void fresh(void) {
    for (int i = 0; i < BT_NITEM_STRS; i++) { A.scalar_effect[i] = 0.0f; A.temp_scalar[i] = 0.0f; }
    A.danger_hp = BT_DANGER_HP;
    A.danger_dmg = BT_DANGER_DMG;
    for (int j = 0; j < BT_MAX; j++) {
        bt_enemy_set_alive(&A, j, 0); A.eneutral[j] = 0; A.eelite[j] = 0; A.ekind[j] = 0;
    }
    A.wave = 1;
    bt_enemy_set_alive(&A, SLOT_ENEMY, 1);   A.eelite[SLOT_ENEMY] = 0;   A.eneutral[SLOT_ENEMY] = 0;
    bt_enemy_set_alive(&A, SLOT_ELITE, 1);   A.eelite[SLOT_ELITE] = 1;   A.eneutral[SLOT_ELITE] = 0;
    bt_enemy_set_alive(&A, SLOT_BOSS, 1);    A.eelite[SLOT_BOSS] = 2;    A.eneutral[SLOT_BOSS] = 0;
    bt_enemy_set_alive(&A, SLOT_NEUTRAL, 1); A.eelite[SLOT_NEUTRAL] = 0; A.eneutral[SLOT_NEUTRAL] = 1;
}

/* ---------------------------------------------------------------------------
 * G7 -- factors with nothing owned are exactly 1.0 (no-dead behaviour).
 * ------------------------------------------------------------------------- */
static void test_factors_default(void) {
    fresh();
    check_near(bt_enemy_damage_effects_factor(&A), 1.0f, 1e-6f, "no enemy_damage -> damage factor 1.0");
    check_near(bt_enemy_health_effects_factor(&A), 1.0f, 1e-6f, "no enemy_health -> health factor 1.0");
    check_near(bt_enemy_speed_effects_factor(&A),  1.0f, 1e-6f, "no enemy_speed  -> speed factor 1.0");

    /* danger factor is on its own: base * danger_hp / danger_dmg, round. */
    check_near(bt_enemy_final_damage(&A, 10.0f, 0.0f), 14.0f, 1e-4f, "no item: damage = round(10*1.4)");
    check_near(bt_enemy_final_health(&A, 10.0f, 0.0f), 14.0f, 1e-4f, "no item: health = round(10*1.4)");
    check_near(bt_enemy_final_speed(&A, 300.0f, 0.0f), 300.0f, 1e-4f, "no item: speed = 300");
}

/* ---------------------------------------------------------------------------
 * G7 -- permanent SUM producers (black_flag/starfish/snail/black_flag health).
 * ------------------------------------------------------------------------- */
static void test_factors_permanent(void) {
    fresh();
    /* enemy_damage +10 -> 1 + 10/100 = 1.10; round(10 * 1.4 * 1.1). */
    A.scalar_effect[BT_ISTR_ENEMY_DAMAGE] = 10.0f;
    check_near(bt_enemy_damage_effects_factor(&A), 1.1f, 1e-6f, "enemy_damage +10 -> factor 1.1");
    check_near(bt_enemy_final_damage(&A, 10.0f, 0.0f), 15.0f, 1e-4f, "damage = round(10*1.4*1.1)");

    /* enemy_health +10 -> 1.10. */
    A.scalar_effect[BT_ISTR_ENEMY_HEALTH] = 10.0f;
    check_near(bt_enemy_final_health(&A, 10.0f, 0.0f), 15.0f, 1e-4f, "health = round(10*1.4*1.1)");

    /* enemy_speed -8 (snail) -> 0.92; round(300 * 0.92). */
    A.scalar_effect[BT_ISTR_ENEMY_SPEED] = -8.0f;
    check_near(bt_enemy_speed_effects_factor(&A), 0.92f, 1e-6f, "enemy_speed -8 -> factor 0.92");
    check_near(bt_enemy_final_speed(&A, 300.0f, 0.0f), 276.0f, 1e-4f, "speed = round(300*0.92)");

    /* Stacked copies sum (SUM storage): starfish x2 = +30. */
    A.scalar_effect[BT_ISTR_ENEMY_DAMAGE] = 30.0f;
    check_near(bt_enemy_damage_effects_factor(&A), 1.3f, 1e-6f, "enemy_damage +30 -> factor 1.3");
}

/* ---------------------------------------------------------------------------
 * G7 -- stats_next_wave TEMP producers (peacock / scarf / celery tea) combine
 * with the permanent SUM through bt_temporal_scalar_effective.
 * ------------------------------------------------------------------------- */
static void test_factors_next_wave_temp(void) {
    fresh();
    /* peacock_effect_3: enemy_damage +50 next wave -> 1 + 50/100 = 1.5. */
    A.temp_scalar[BT_ISTR_ENEMY_DAMAGE] = 50.0f;
    check_near(bt_enemy_damage_effects_factor(&A), 1.5f, 1e-6f, "next-wave enemy_damage +50 -> factor 1.5");
    check_near(bt_enemy_final_damage(&A, 10.0f, 0.0f), 21.0f, 1e-4f, "damage = round(10*1.4*1.5)");

    /* celery_tea_effect_1: enemy_health +100 next wave -> factor 2.0. */
    A.temp_scalar[BT_ISTR_ENEMY_HEALTH] = 100.0f;
    check_near(bt_enemy_health_effects_factor(&A), 2.0f, 1e-6f, "next-wave enemy_health +100 -> factor 2.0");
    check_near(bt_enemy_final_health(&A, 10.0f, 0.0f), 28.0f, 1e-4f, "health = round(10*1.4*2)");

    /* scarf_effect_3 (+25 temp) + snail (-8 permanent) -> net +17 -> 1.17. */
    A.scalar_effect[BT_ISTR_ENEMY_SPEED] = -8.0f;
    A.temp_scalar[BT_ISTR_ENEMY_SPEED] = 25.0f;
    check_near(bt_temporal_scalar_effective(&A, BT_ISTR_ENEMY_SPEED), 17.0f, 1e-6f, "speed raw+temp = 17");
    check_near(bt_enemy_speed_effects_factor(&A), 1.17f, 1e-6f, "speed factor = 1.17");
    check_near(bt_enemy_final_speed(&A, 300.0f, 0.0f), 351.0f, 1e-4f, "speed = round(300*1.17)");

    /* permanent + temp on enemy_damage sum too. */
    A.scalar_effect[BT_ISTR_ENEMY_DAMAGE] = 10.0f;
    A.temp_scalar[BT_ISTR_ENEMY_DAMAGE] = 50.0f;
    check_near(bt_enemy_damage_effects_factor(&A), 1.6f, 1e-6f, "damage perm 10 + temp 50 -> 1.6");
}

/* ---------------------------------------------------------------------------
 * G7 -- percent_modifier (buffer boost) and its clamps.
 * ------------------------------------------------------------------------- */
static void test_percent_modifier(void) {
    fresh();
    /* damage/health clamp boost at 0.01; speed does NOT clamp (get_final_enemy_speed). */
    check_near(bt_enemy_final_damage(&A, 100.0f, 50.0f), 210.0f, 1e-4f, "damage boost +50% -> round(100*1.4*1.5)");
    check_near(bt_enemy_final_health(&A, 100.0f, 50.0f), 210.0f, 1e-4f, "health boost +50% -> round(100*1.4*1.5)");
    check_near(bt_enemy_final_speed(&A, 100.0f, 50.0f), 150.0f, 1e-4f, "speed boost +50% -> round(100*1.5)");

    /* clamp: percent -200 -> boost max(0.01, -1) = 0.01 -> round(10*1.4*0.01) = 0. */
    check_near(bt_enemy_final_damage(&A, 10.0f, -200.0f), 0.0f, 1e-4f, "damage boost clamped to 0.01");
    check_near(bt_enemy_final_health(&A, 10.0f, -200.0f), 0.0f, 1e-4f, "health boost clamped to 0.01");
    /* speed has NO clamp: boost = 1 + (-200)/100 = -1 -> round(300*-1) = -300. */
    check_near(bt_enemy_final_speed(&A, 300.0f, -200.0f), -300.0f, 1e-4f, "speed boost not clamped");
}

/* ---------------------------------------------------------------------------
 * G7 -- the factor clamps: damage/health effects_factor max(0.01, ...).
 * ------------------------------------------------------------------------- */
static void test_effects_clamp(void) {
    fresh();
    A.scalar_effect[BT_ISTR_ENEMY_DAMAGE] = -200.0f;   /* 1 - 2 = -1 -> clamp 0.01 */
    check_near(bt_enemy_damage_effects_factor(&A), 0.01f, 1e-6f, "enemy_damage clamp 0.01");
    check_near(bt_enemy_final_damage(&A, 10.0f, 0.0f), 0.0f, 1e-4f, "clamped damage ~= 0");
}

/* ---------------------------------------------------------------------------
 * G1 -- boss.gd damage_against_bosses multiplier.
 * ------------------------------------------------------------------------- */
static void test_boss_receive(void) {
    fresh();
    /* not owned -> value passes through untouched, even fractional. */
    check_near(bt_enemy_incoming_damage(&A, SLOT_BOSS, 100.7f), 100.7f, 1e-6f, "unowned boss: unchanged");
    check_near(bt_enemy_incoming_damage(&A, SLOT_ELITE, 100.7f), 100.7f, 1e-6f, "unowned elite: unchanged");
    /* plain enemy: never scaled even when owned. */
    A.scalar_effect[BT_ISTR_DAMAGE_AGAINST_BOSSES] = 25.0f;
    check_near(bt_enemy_incoming_damage(&A, SLOT_ENEMY, 100.7f), 100.7f, 1e-6f, "enemy: never scaled");
    /* neutral (tree): eelite 0 -> unchanged. */
    check_near(bt_enemy_incoming_damage(&A, SLOT_NEUTRAL, 100.7f), 100.7f, 1e-6f, "neutral: unchanged");

    /* owned: elite and boss both scale by int(value * (1+stat/100)). */
    check_near(bt_enemy_incoming_damage(&A, SLOT_ELITE, 100.0f), 125.0f, 1e-6f, "elite 100 -> 125");
    check_near(bt_enemy_incoming_damage(&A, SLOT_BOSS, 100.0f), 125.0f, 1e-6f, "boss 100 -> 125");
    /* int() truncation (GDScript int), not round: 7 * 1.25 = 8.75 -> 8. */
    check_near(bt_enemy_incoming_damage(&A, SLOT_BOSS, 7.0f), 8.0f, 1e-6f, "boss 7 -> int(8.75) = 8");
    check_near(bt_enemy_incoming_damage(&A, SLOT_BOSS, 3.0f), 3.0f, 1e-6f, "boss 3 -> int(3.75) = 3");

    /* stacked copies: 2 silver bullets = +50 -> mult 1.5. */
    A.scalar_effect[BT_ISTR_DAMAGE_AGAINST_BOSSES] = 50.0f;
    check_near(bt_enemy_incoming_damage(&A, SLOT_BOSS, 100.0f), 150.0f, 1e-6f, "2x silver bullet -> 150");
    /* +10 (absurd but exercises the generic path): int(100*1.1) = 110. */
    A.scalar_effect[BT_ISTR_DAMAGE_AGAINST_BOSSES] = 10.0f;
    check_near(bt_enemy_incoming_damage(&A, SLOT_ELITE, 100.0f), 110.0f, 1e-6f, "mult 1.1 -> int(110.0) = 110");

    /* out-of-range slot is a safe passthrough (bounds guard). */
    check_near(bt_enemy_incoming_damage(&A, -1, 42.0f), 42.0f, 1e-6f, "negative slot: unchanged");
    check_near(bt_enemy_incoming_damage(&A, BT_MAX, 42.0f), 42.0f, 1e-6f, "past-end slot: unchanged");
}

int main(void) {
    /* These fixtures hand-build a BtState and never run bt_core_init, which is
     * the only initializer for the two RNG-model gates; without this they would
     * assert the neutralized (audio 0 / particles 0) model instead of the
     * source default they were written for. */
    A.audio_rng = 1;
    A.particles_rng = 1;
    test_factors_default();
    test_factors_permanent();
    test_factors_next_wave_temp();
    test_percent_modifier();
    test_effects_clamp();
    test_boss_receive();

    printf("test_brotato_enemy_modifiers: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
