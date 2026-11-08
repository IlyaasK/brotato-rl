/* tests/test_brotato_enemy_curse.cpp
 *
 * Source-backed regressions for the DLC cursed-enemy slice
 * (ocean/brotato/brotato_enemy_curse.h + the ecurse[] state field and the
 * bt_spawn_kind_at / bt_mark_enemy_dead / bt_buffer_boost hooks).
 *
 * Every expectation is transcribed from the recovered sources:
 *   dlcs/dlc_1/effect_behaviors/scene/curse_scene_effect_behavior.gd:50-65
 *       the roll gate (Boss/loot-alien return BEFORE the roll), the per-player
 *       chance min(1, (get_curse_factor(stat)/100/2)*(1+endless/2)), the
 *       get_chance_success(chance)==0 -> false short-circuit (no draw), and the
 *       `... and enemy.can_be_cursed` apply gate that runs AFTER the roll
 *   curse_scene_effect_behavior.tscn  hp_boost 150 / damage_boost 25 /
 *       speed_boost 15 (they override the .gd 200/50/75)
 *   curse_scene_effect_behavior.gd:8  MAX_CURSE_HP_BOOST 300
 *   curse_scene_effect_behavior.gd:22-35  hp_boost = hp_boost +
 *       min(curse, 300) * 2 (assigned to the INT BoostArgs field: truncates),
 *       then Enemy.boost -> reset_health/damage/speed_stat(percent)
 *   curse_enemy_effect_behavior.gd:4   CURSED_ENEMIES_BONUS_GOLD 0.33
 *   curse_enemy_effect_behavior.gd:20-21 get_gold_value_modifier -> 0.33
 *   curse_enemy_effect_behavior.gd:24-41 on_taken_damage -> direct add_gold
 *   singletons/utils.gd:406-411        get_curse_factor
 *   singletons/run_data.gd:775-783     get_endless_factor
 *   entity_service.gd:33-53 / unit.gd:104-111  the get_final_enemy_* path the
 *       boost recomputes through (bt_enemy_final_health/damage/speed)
 *
 * Build and run:
 *     c++ -O2 -std=c++11 -o build/test_brotato_enemy_curse \
 *         tests/test_brotato_enemy_curse.cpp -lm
 *     ./build/test_brotato_enemy_curse
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

/* BtState embeds the physics worlds; keep it in static storage. */
static BtState A;

enum { K_ANEMONE = 0, K_EVIL_MOB = 29, K_LOOTER = 38, K_SEA_PIG = 17,
       K_IRON_LUNG = 11, K_EVIL_MOB_SWIMMER = 8, K_LOOTING_PIG = 13,
       K_STARGAZER = 19, K_STONEFISH = 20, K_BUFFER = 26 };

/* The source can_be_cursed=false set (enemy.gd:11 + the six scenes). */
static const int CURSED_FALSE[] = { 8, 11, 13, 17, 29, 38 };

static void fresh(void) {
    for (int i = 0; i < BT_NITEM_STRS; i++) A.scalar_effect[i] = 0.0f;
    A.danger_hp = BT_DANGER_HP;   /* 1.40 (difficulty_5_effect_2) */
    A.danger_dmg = BT_DANGER_DMG; /* 1.40 */
    A.wave = 5;
    A.curse = 0.0f;
    A.materials = 0.0f;
    for (int j = 0; j < BT_MAX; j++) {
        bt_enemy_set_alive(&A, j, 0); A.eneutral[j] = 0; A.eelite[j] = 0; A.ekind[j] = 0;
        A.ecurse[j] = 0;
        A.ehp[j] = 0.0f; A.emax[j] = 0.0f; A.edmg[j] = 0.0f; A.espd[j] = 0.0f;
        A.espeed_rand[j] = 0.0f;
        A.eboosted[j] = 0;
        A.eboost_hp[j] = 0.0f; A.eboost_dmg[j] = 0.0f; A.eboost_spd[j] = 0.0f;
    }
    bt_rng_seed(&A.rng, 0xB407A70u);
}

static void place(int slot, int kind, int neutral) {
    bt_enemy_set_alive(&A, slot, 1);
    A.eneutral[slot] = (unsigned char)(neutral ? 1 : 0);
    A.ekind[slot] = (unsigned char)kind;
    A.eelite[slot] = (unsigned char)bt_kind_class(kind);
    A.ecurse[slot] = 0;
    A.espeed_rand[slot] = 0.0f;
    A.eboosted[slot] = 0;
    A.eboost_hp[slot] = 0.0f; A.eboost_dmg[slot] = 0.0f; A.eboost_spd[slot] = 0.0f;
    A.ehp[slot] = 0.0f; A.emax[slot] = 0.0f; A.edmg[slot] = 0.0f; A.espd[slot] = 0.0f;
}

/* ---------------------------------------------------------------- tables -- */

static void test_tables() {
    int zeros = 0, loot_cursed = 0;
    for (int k = 0; k < BT_NKINDS; k++) if (!BT_KIND_CAN_BE_CURSED[k]) zeros++;
    check(zeros == (int)(sizeof(CURSED_FALSE) / sizeof(CURSED_FALSE[0])),
          "BT_KIND_CAN_BE_CURSED has exactly the six source-false kinds");
    for (int i = 0; i < (int)(sizeof(CURSED_FALSE) / sizeof(CURSED_FALSE[0])); i++)
        check(BT_KIND_CAN_BE_CURSED[CURSED_FALSE[i]] == 0, "can_be_cursed false kind");
    /* every loot alien is can_be_cursed == false (their scenes export it) */
    for (int k = 0; k < BT_NKINDS; k++)
        if (BT_KIND_IS_LOOT[k]) { loot_cursed++; check(BT_KIND_CAN_BE_CURSED[k] == 0, "loot alien is not cursable"); }
    check(loot_cursed >= 4, "loot-alien kinds present");

    /* the roll gate: no class-0 kind that is a loot alien may roll, and no
     * elite/boss kind may roll. */
    for (int k = 0; k < BT_NKINDS; k++) {
        if (bt_kind_class(k) != 0)
            check(BT_KIND_CURSE_ROLLS[k] == 0, "Boss/elite never rolls the curse");
        if (BT_KIND_IS_LOOT[k])
            check(BT_KIND_CURSE_ROLLS[k] == 0, "loot alien never rolls the curse");
    }
    check(BT_KIND_CURSE_ROLLS[K_ANEMONE] == 1, "a plain enemy rolls the curse");
    check(BT_KIND_CURSE_ROLLS[K_SEA_PIG] == 1, "sea_pig rolls but refuses (can_be_cursed false)");

    check_near((float)BT_CURSE_HP_BOOST, 150.0f, 0.0f, "scene hp_boost");
    check_near((float)BT_CURSE_DAMAGE_BOOST, 25.0f, 0.0f, "scene damage_boost");
    check_near((float)BT_CURSE_SPEED_BOOST, 15.0f, 0.0f, "scene speed_boost");
    check_near((float)BT_CURSE_MAX_CURSE_HP_BOOST, 300.0f, 0.0f, "MAX_CURSE_HP_BOOST");
    check_near(BT_CURSED_ENEMIES_BONUS_GOLD, 0.33f, 0.0f, "CURSED_ENEMIES_BONUS_GOLD");
}

/* ---------------------------------------------------------------- chance -- */

static void test_chance() {
    fresh();
    check_near(bt_enemy_curse_chance(&A), 0.0f, 0.0f, "zero curse -> zero chance");

    /* wave 5 (endless 0): per = (1 - 1/(1 + curse/50)) / 2 */
    A.curse = 50.0f;
    check_near(bt_enemy_curse_chance(&A), 0.25f, 1e-6f, "curse 50 -> 0.25");
    A.curse = 150.0f;
    check_near(bt_enemy_curse_chance(&A), 0.375f, 1e-6f, "curse 150 -> 0.375 (1-1/4)/2");

    /* the endless factor scales it and the min(1.0, .) clamp is applied */
    A.wave = 40;                       /* endless_factor = (20*21/2)/100 * 3.0 */
    A.curse = 100000.0f;
    check_near(bt_enemy_curse_chance(&A), 1.0f, 0.0f, "endless clamps the chance at 1");
    A.curse = 0.0f;
}

/* ------------------------------------------------------- zero-curse RNG --- */

/* curse == 0 -> get_chance_success(0) is false WITHOUT a randf draw, so the
 * run's RNG stream must be untouched by the spawn hook. */
static void test_no_draw_at_zero() {
    fresh();
    unsigned int baseline = bt_rng_next(&A.rng);
    fresh();
    place(0, K_ANEMONE, 0);
    bt_enemy_curse_roll(&A, 0, K_ANEMONE);
    check(bt_enemy_cursed(&A, 0) == 0, "zero curse never curses");
    check(bt_rng_next(&A.rng) == baseline, "zero curse draws nothing");

    /* a Boss/elite or a loot alien returns before the chance is computed, so it
     * draws nothing EVEN with a huge curse. */
    fresh();
    A.curse = 100000.0f;
    baseline = bt_rng_next(&A.rng);
    fresh();
    A.curse = 100000.0f;
    place(0, K_EVIL_MOB, 0);            /* loot alien (class 0) */
    bt_enemy_curse_roll(&A, 0, K_EVIL_MOB);
    check(bt_enemy_cursed(&A, 0) == 0, "loot alien is never cursed");
    check(bt_rng_next(&A.rng) == baseline, "loot alien draws nothing");
}

/* ----------------------------------------------------------------- boost -- */

static void test_boost_boundaries() {
    fresh();
    place(0, K_ANEMONE, 0);
    /* baseline (uncursed) hp at wave 5: round(floor(8 + 4*4) * 1.40) = 34 */
    check_near(bt_enemy_final_health(&A, bt_floorf(BT_KINDS[K_ANEMONE].hp +
                                                   BT_KINDS[K_ANEMONE].hp_grow * 4.0f), 0.0f),
               34.0f, 0.0f, "anemone wave-5 base hp");

    /* curse 0 -> hp_boost = 150 */
    A.curse = 0.0f;
    bt_enemy_curse_apply(&A, 0, K_ANEMONE);
    check(bt_enemy_cursed(&A, 0) == 1, "apply sets the cursed flag");
    check_near(A.emax[0], 84.0f, 0.0f, "curse 0 -> hp x2.5 (round(24*1.4*2.5))");
    check_near(A.ehp[0], A.emax[0], 0.0f, "hp refilled to the new max");

    /* curse 50 -> hp_boost = 150 + 100 = 250 -> x3.5 ; damage 25% ; speed 15% */
    fresh();
    place(0, K_ANEMONE, 0);
    A.curse = 50.0f;
    bt_enemy_curse_apply(&A, 0, K_ANEMONE);
    check_near(A.emax[0], 118.0f, 0.0f, "curse 50 -> hp 118 (round(33.6*3.5))");
    check_near(A.edmg[0], 8.0f, 0.0f, "damage x1.25 (round(4.4*1.4*1.25))");
    check_near(A.espd[0], 115.0f, 0.0f, "speed x1.15 (round(100*1*1.15))");

    /* curse > 300 is clamped: hp_boost = 150 + 300*2 = 750 -> x8.5, and the
     * damage/speed percentages do NOT scale with the curse stat. */
    fresh();
    place(0, K_ANEMONE, 0);
    A.curse = 100000.0f;
    bt_enemy_curse_apply(&A, 0, K_ANEMONE);
    check_near(A.emax[0], 286.0f, 0.0f, "MAX_CURSE_HP_BOOST clamp -> hp 286");
    check_near(A.edmg[0], 8.0f, 0.0f, "damage boost is the flat scene value");
    check_near(A.espd[0], 115.0f, 0.0f, "speed boost is the flat scene value");
}

/* the boost is recomputed from the kind BASE (a second apply does not compound:
 * the source's reset_*_stat(percent) recomputes, never multiplies). */
static void test_boost_is_not_compounding() {
    fresh();
    place(0, K_ANEMONE, 0);
    A.curse = 50.0f;
    bt_enemy_curse_apply(&A, 0, K_ANEMONE);
    float once = A.emax[0];
    bt_enemy_curse_apply(&A, 0, K_ANEMONE);
    check_near(A.emax[0], once, 0.0f, "curse boost recomputes from base, no compounding");
}

/* enemy.gd:311-312: Enemy.boost returns before doing anything when the scene's
 * can_be_boosted is false (BT_KIND_BOOSTABLE), so a cursed stonefish / buffer
 * keeps its spawn stats; the curse still marks it. */
static void test_non_boostable_kind_keeps_its_stats() {
    fresh();
    place(0, K_STONEFISH, 0);
    check(BT_KIND_BOOSTABLE[K_STONEFISH] == 0, "stonefish scene refuses boosts");
    check(BT_KIND_CAN_BE_CURSED[K_STONEFISH] == 1, "stonefish can still be cursed");
    A.curse = 50.0f;
    A.emax[0] = 111.0f; A.ehp[0] = 111.0f; A.edmg[0] = 22.0f; A.espd[0] = 333.0f;
    bt_enemy_curse_apply(&A, 0, K_STONEFISH);
    check(bt_enemy_cursed(&A, 0) == 1, "non-boostable kind still gets the curse flag");
    check_near(A.emax[0], 111.0f, 0.0f, "no hp boost for a non-boostable kind");
    check_near(A.ehp[0], 111.0f, 0.0f, "no hp refill");
    check_near(A.edmg[0], 22.0f, 0.0f, "no damage boost");
    check_near(A.espd[0], 333.0f, 0.0f, "no speed boost");
    check(A.eboosted[0] == 0, "no boost recorded");
}

/* stargazer.gd:69-79 get_angry: `if is_boosted and _boosted_args` ADDS its own
 * 150/25/250 onto the args of the boost already in force.  A cursed stargazer is
 * is_boosted (the curse boost) with _boosted_args = the curse args, so the angry
 * boost combines: hp 150+curse, damage 25+25, speed 250+15. */
static void test_cursed_stargazer_angry_combines() {
    fresh();
    place(0, K_STARGAZER, 0);
    check(BT_KIND_BOOSTABLE[K_STARGAZER] == 1, "stargazer can be boosted");
    A.curse = 50.0f;                     /* curse hp_pct = 150 + 2*50 = 250 */
    bt_enemy_curse_apply(&A, 0, K_STARGAZER);
    check(A.eboosted[0] == 1, "the curse records the boost in force");
    check_near(A.eboost_hp[0], 250.0f, 0.0f, "curse hp_pct recorded");
    check_near(A.eboost_dmg[0], 25.0f, 0.0f, "curse dmg_pct recorded");
    check_near(A.eboost_spd[0], 15.0f, 0.0f, "curse spd_pct recorded");

    bt_enemy_lifecycle_angry(&A, 0);
    float w1 = (float)(A.wave - 1);
    float base = bt_floorf(BT_KINDS[K_STARGAZER].hp + BT_KINDS[K_STARGAZER].hp_grow * w1);
    check_near(A.emax[0], bt_enemy_final_health(&A, base, 400.0f), 0.0f, "angry hp = curse+own");
    check_near(A.edmg[0], bt_enemy_final_damage(&A,
               BT_KINDS[K_STARGAZER].dmg + BT_KINDS[K_STARGAZER].dmg_grow * w1, 50.0f), 0.0f,
               "angry damage = curse+own");
    check_near(A.espd[0], bt_enemy_final_speed(&A,
               (float)BT_KINDS[K_STARGAZER].speed + A.espeed_rand[0], 265.0f), 0.0f,
               "angry speed = curse+own");
}

/* ------------------------------------------------------------- roll gate -- */

static void test_roll_end_to_end() {
    fresh();
    A.curse = 50.0f;                    /* p = 0.25 */
    int cursed = 0, trials = 400;
    for (int t = 0; t < trials; t++) {
        place(0, K_ANEMONE, 0);
        bt_enemy_curse_roll(&A, 0, K_ANEMONE);
        if (bt_enemy_cursed(&A, 0)) {
            cursed++;
            check_near(A.emax[0], 118.0f, 0.0f, "rolled curse applies the boost");
        }
    }
    check(cursed > 0 && cursed < trials, "the roll is stochastic (some cursed, some not)");

    /* a kind that rolls but refuses the curse (sea_pig) never ends up cursed */
    fresh();
    A.curse = 100000.0f;
    for (int t = 0; t < trials; t++) {
        place(0, K_SEA_PIG, 0);
        bt_enemy_curse_roll(&A, 0, K_SEA_PIG);
        if (bt_enemy_cursed(&A, 0)) { check(0, "sea_pig must never be cursed"); break; }
    }
    /* a Boss/elite and a loot alien never roll */
    fresh();
    A.curse = 100000.0f;
    place(0, 45, 0);                    /* first elite block row */
    check(bt_kind_class(45) != 0, "row 45 is elite/boss");
    for (int t = 0; t < 100; t++) { bt_enemy_curse_roll(&A, 0, 45); }
    check(bt_enemy_cursed(&A, 0) == 0, "elite/boss never cursed");
}

/* buffer.gd:46 refuses a target with can_be_boosted == false OR is_boosted ==
 * true; a cursed unit is is_boosted (the curse boost), so a buffer in range must
 * not touch it.  The uncursed control proves the harness. */
static void test_buffer_refuses_a_cursed_target() {
    /* control: an uncursed, unboosted anemone in range IS boosted (hp_pct 150) */
    fresh();
    place(0, K_ANEMONE, 0);
    place(1, K_BUFFER, 0);
    A.ex[0] = 100.0f; A.ey[0] = 100.0f;
    A.ex[1] = 100.0f; A.ey[1] = 100.0f;
    /* the BoostZone's body_entered sweep: buffer.gd:26-28 pushes every body that
     * overlaps, and the timeout then walks THAT list (entity_spawner/Area2D
     * order), so the fixture must run the entry step exactly like a frame does. */
    bt_zone_update(&A, 1);
    float base = bt_floorf(BT_KINDS[K_ANEMONE].hp + BT_KINDS[K_ANEMONE].hp_grow * 4.0f);
    bt_buffer_boost(&A, 1, K_BUFFER);
    check(A.eboosted[0] == 1, "control: the buffer boosts an uncursed enemy in range");
    check_near(A.emax[0], bt_enemy_final_health(&A, base, 150.0f), 0.0f,
               "control: buffer hp boost applied");

    /* cursed: the buffer must skip it and keep the curse's own stats */
    fresh();
    place(0, K_ANEMONE, 0);
    place(1, K_BUFFER, 0);
    A.ex[0] = 100.0f; A.ey[0] = 100.0f;
    A.ex[1] = 100.0f; A.ey[1] = 100.0f;
    A.curse = 50.0f;
    bt_enemy_curse_apply(&A, 0, K_ANEMONE);
    float cursed_hp = A.emax[0];
    check_near(cursed_hp, 118.0f, 0.0f, "cursed anemone hp before the buffer");
    /* the cursed target is still an overlapping body with can_be_boosted true
     * (the curse changes is_boosted, not the scene flag), so it stays in the
     * list and only the timeout's eligibility test may skip it. */
    bt_zone_update(&A, 1);
    check((int)A.zone_n[1] == 1, "the cursed target is still in the BoostZone list");
    bt_buffer_boost(&A, 1, K_BUFFER);
    check_near(A.emax[0], cursed_hp, 0.0f, "a cursed enemy is not buffer-boosted");
}

/* ------------------------------------------------------------ gold paths -- */

static void test_gold_value_modifier() {
    fresh();
    place(0, K_ANEMONE, 0);
    check_near(bt_enemy_curse_gold_value_mult(&A, 0), 1.0f, 0.0f, "uncursed gold mult");
    A.ecurse[0] = 1;
    check_near(bt_enemy_curse_gold_value_mult(&A, 0), 1.33f, 1e-6f, "cursed gold mult +33%");
    place(3, K_ANEMONE, 1);             /* a neutral (tree) */
    A.ecurse[3] = 1;                    /* even if the byte were stale */
    check_near(bt_enemy_curse_gold_value_mult(&A, 3), 1.33f, 1e-6f, "mult reads ecurse");
    check(bt_enemy_cursed(&A, -1) == 0 && bt_enemy_cursed(&A, BT_MAX) == 0, "accessor bounds");
}

static void test_direct_kill_gold() {
    fresh();
    place(0, K_ANEMONE, 0);
    A.scalar_effect[BT_ISTR_GOLD_ON_CURSED_ENEMY_KILL] = 3.0f;

    /* not cursed: nothing, no producer read */
    check(bt_enemy_curse_player_kill_gold(&A, 0) == 0, "uncursed kill pays nothing");
    check_near(A.materials, 0.0f, 0.0f, "uncursed kill does not touch materials");

    /* cursed + black_flag_effect_0 (value 3): DIRECT purse credit, no coin, no XP */
    A.ecurse[0] = 1;
    int paid = bt_enemy_curse_player_kill_gold(&A, 0);
    check(paid == 3, "cursed kill pays gold_on_cursed_enemy_kill");
    check_near(A.materials, 3.0f, 0.0f, "cursed kill credits materials directly");
    /* idempotence guard: the caller calls it once per lethal hit */
    A.ecurse[0] = 0;
    check(bt_enemy_curse_player_kill_gold(&A, 0) == 0, "cleared flag pays nothing");

    /* no producer owned -> no payout even when cursed */
    fresh();
    place(0, K_ANEMONE, 0);
    A.ecurse[0] = 1;
    check(bt_enemy_curse_player_kill_gold(&A, 0) == 0, "no producer -> no payout");
    check_near(A.materials, 0.0f, 0.0f, "no producer leaves materials alone");
}

/* ------------------------------------------------------- burn kill gold -- */

static int find_kind(const char* name) {
    for (int i = 0; i < BT_NKINDS; i++) {
        const char* n = BT_KIND_NAME[i];
        if (!n) continue;
        int same = 1;
        for (int j = 0; n[j] || name[j]; j++) if (n[j] != name[j]) { same = 0; break; }
        if (same) return i;
    }
    return -1;
}

/* A burn tick that kills (unit.gd:661-707 take_damage with
 * Unit._burning_player_index) must pay the curse gold; in this core every burn
 * is player-0 sourced, so the tick's kill credits it directly. */
static void test_burn_kill_gold() {
    float obs[BT_OBS_SIZE], rew, term;
    int kind = find_kind("anemone");
    check(kind == K_ANEMONE, "anemone resolves to kind 0");

    /* cursed burn kill -> direct gold, no coin/XP */
    bt_core_init(&A, 4242u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
    A.curse = 0.0f;                     /* keep the spawn roll out of the way */
    bt_enemy_set_alive(&A, 0, 0); A.ecount = 0;
    bt_spawn_kind(&A, kind, 900.0f, 900.0f);
    check(A.ealive[0] == 1, "burn target spawned in slot 0");
    A.ecurse[0] = 1;
    A.scalar_effect[BT_ISTR_GOLD_ON_CURSED_ENEMY_KILL] = 2.0f;
    A.eburn[0] = 1.0f; A.eburndmg[0] = 1.0e9f;
    float before = A.materials;
    bt_core_step(&A, 0, obs, &rew, &term);
    check(A.ealive[0] == 0, "the burn tick killed the cursed target");
    check_near(A.materials, before + 2.0f, 0.0f, "a cursed burn kill pays directly");

    /* uncursed burn kill -> nothing */
    bt_core_init(&A, 4243u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
    A.curse = 0.0f;
    bt_enemy_set_alive(&A, 0, 0); A.ecount = 0;
    bt_spawn_kind(&A, kind, 900.0f, 900.0f);
    check(A.ealive[0] == 1, "burn target spawned in slot 0");
    A.ecurse[0] = 0;
    A.scalar_effect[BT_ISTR_GOLD_ON_CURSED_ENEMY_KILL] = 2.0f;
    A.eburn[0] = 1.0f; A.eburndmg[0] = 1.0e9f;
    before = A.materials;
    bt_core_step(&A, 0, obs, &rew, &term);
    check(A.ealive[0] == 0, "the burn tick killed the uncursed target");
    check_near(A.materials, before, 0.0f, "an uncursed burn kill pays nothing");
}

int main() {
    test_tables();
    test_chance();
    test_no_draw_at_zero();
    test_boost_boundaries();
    test_boost_is_not_compounding();
    test_non_boostable_kind_keeps_its_stats();
    test_cursed_stargazer_angry_combines();
    test_roll_end_to_end();
    test_buffer_refuses_a_cursed_target();
    test_gold_value_modifier();
    test_direct_kill_gold();
    test_burn_kill_gold();

    printf("%s: %d checks, %d failures\n", g_failures ? "FAIL" : "OK", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
