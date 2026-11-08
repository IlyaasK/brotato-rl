/* Behavioral regression tests for ocean/brotato/brotato_physics_math.h.
 *
 * These are the movement / knockback / pull / clamp / overlap invariants that
 * the Bull/Danger-5 emulator must preserve, checked against a hand-evaluated
 * oracle taken from the recovered scripts:
 *   entities/units/unit/unit.gd:136-242,265-353,402-448,730-741
 *
 * Build & run (standalone, no raylib, no game headers):
 *   c++ -std=c++11 -O2 -o /tmp/test_brotato_physics_math \
 *       tests/test_brotato_physics_math.cpp && /tmp/test_brotato_physics_math
 *
 * Exit status 0 means every check passed.
 */

#include <cstdio>
#include <cstdlib>

#include "../ocean/brotato/brotato_physics_math.h"

static int g_checks = 0;
static int g_failures = 0;

static void check_impl(int ok, const char* expr, int line) {
    g_checks++;
    if (!ok) {
        g_failures++;
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expr);
    }
}

#define CHECK(expr) check_impl((expr) ? 1 : 0, #expr, __LINE__)

static int nearf(float a, float b, float eps) {
    float d = a - b;
    if (d < 0.0f) d = -d;
    return d <= eps;
}

static int vnear(BtVec2 a, BtVec2 b, float eps) {
    return nearf(a.x, b.x, eps) && nearf(a.y, b.y, eps);
}

/* ---------------------------------------------------------------- */
/* vector primitives                                                */
/* ---------------------------------------------------------------- */
static void test_vectors(void) {
    /* normalized(): a zero vector stays zero; no NaN leaks out. */
    BtVec2 z = bt_v2normalized(bt_v2zero());
    CHECK(z.x == 0.0f && z.y == 0.0f);

    CHECK(vnear(bt_v2normalized(bt_v2(3.0f, 4.0f)), bt_v2(0.6f, 0.8f), 1e-6f));

    /* direction_to of a coincident pair is zero. */
    BtVec2 d0 = bt_v2dirto(bt_v2(5.0f, 5.0f), bt_v2(5.0f, 5.0f));
    CHECK(d0.x == 0.0f && d0.y == 0.0f);

    CHECK(nearf(bt_v2len(bt_v2(3.0f, 4.0f)), 5.0f, 1e-6f));
    CHECK(nearf(bt_v2lensq(bt_v2(3.0f, 4.0f)), 25.0f, 1e-6f));
    CHECK(nearf(bt_v2dot(bt_v2(1.0f, 2.0f), bt_v2(3.0f, 4.0f)), 11.0f, 1e-6f));
    CHECK(vnear(bt_v2div(bt_v2(10.0f, 20.0f), 4.0f), bt_v2(2.5f, 5.0f), 1e-6f));

    /* project: b * (a.b / |b|^2). */
    CHECK(vnear(bt_v2project(bt_v2(50.0f, 0.0f), bt_v2(100.0f, 0.0f)), bt_v2(50.0f, 0.0f), 1e-6f));
    CHECK(vnear(bt_v2project(bt_v2(1.0f, 1.0f), bt_v2(1.0f, 0.0f)), bt_v2(1.0f, 0.0f), 1e-6f));

    /* linear_interpolate toward zero at weight 0.1 is a 0.9 scale. */
    CHECK(vnear(bt_v2lerp(bt_v2(10.0f, 20.0f), bt_v2zero(), 0.1f), bt_v2(9.0f, 18.0f), 1e-5f));

    /* Vector2 equality is exact component equality. */
    CHECK(bt_v2eq(bt_v2(1.0f, 2.0f), bt_v2(1.0f, 2.0f)) == 1);
    CHECK(bt_v2eq(bt_v2(1.0f, 2.0f), bt_v2(1.0f, 2.0002f)) == 0);
    CHECK(bt_v2eq(bt_v2(0.0f, -0.0f), bt_v2zero()) == 1);
}

/* ---------------------------------------------------------------- */
/* scalar clamp and Rect2                                             */
/* ---------------------------------------------------------------- */
static void test_rect_clamp(void) {
    CHECK(nearf(bt_clampf(600.0f, 0.0f, 500.0f), 500.0f, 0.0f));
    CHECK(nearf(bt_clampf(-5.0f, 0.0f, 500.0f), 0.0f, 0.0f));
    CHECK(nearf(bt_clampf(250.0f, 0.0f, 500.0f), 250.0f, 0.0f));

    /* has_point: min edge inclusive, max edge exclusive. */
    CHECK(bt_rect_has_point(0.0f, 0.0f, 500.0f, 500.0f, 0.0f, 0.0f) == 1);
    CHECK(bt_rect_has_point(0.0f, 0.0f, 500.0f, 500.0f, 499.5f, 250.0f) == 1);
    CHECK(bt_rect_has_point(0.0f, 0.0f, 500.0f, 500.0f, 500.0f, 0.0f) == 0);
    CHECK(bt_rect_has_point(0.0f, 0.0f, 500.0f, 500.0f, 250.0f, 500.0f) == 0);
    CHECK(bt_rect_has_point(0.0f, 0.0f, 500.0f, 500.0f, -0.5f, 0.0f) == 0);
}

/* ---------------------------------------------------------------- */
/* collision filtering (layer/mask)                                  */
/* ---------------------------------------------------------------- */
static void test_filter(void) {
    const unsigned PLAYER_LAYER = 2u, PLAYER_MASK = 130u; /* player.tscn:1159-1160 */
    const unsigned ENEMY_LAYER = 4u, ENEMY_MASK = 132u;  /* enemy.tscn:45-46 */
    const unsigned WALL_LAYER = 128u, WALL_MASK = 0u;     /* my_tile_map_limits.gd */

    /* player and enemy bodies do not see each other, in either direction. */
    CHECK(bt_filter_pair(PLAYER_LAYER, PLAYER_MASK, ENEMY_LAYER, ENEMY_MASK) == 0);
    CHECK(bt_filter_pair(ENEMY_LAYER, ENEMY_MASK, PLAYER_LAYER, PLAYER_MASK) == 0);
    /* enemies collide with enemies; players collide with players. */
    CHECK(bt_filter_pair(ENEMY_LAYER, ENEMY_MASK, ENEMY_LAYER, ENEMY_MASK) == 1);
    CHECK(bt_filter_pair(PLAYER_LAYER, PLAYER_MASK, PLAYER_LAYER, PLAYER_MASK) == 1);
    /* both bodies collide with obstacles even though wall mask is zero. */
    CHECK(bt_filter_pair(PLAYER_LAYER, PLAYER_MASK, WALL_LAYER, WALL_MASK) == 1);
    CHECK(bt_filter_pair(ENEMY_LAYER, ENEMY_MASK, WALL_LAYER, WALL_MASK) == 1);
    /* unrelated layers never interact. */
    CHECK(bt_filter_pair(1u, 0u, 2u, 0u) == 0);
}

/* ---------------------------------------------------------------- */
/* movement normalization and speed                                  */
/* ---------------------------------------------------------------- */
static void test_movement(void) {
    CHECK(nearf(bt_move_speed(450.0f, 100.0f, -50.0f), 500.0f, 1e-5f));
    CHECK(nearf(bt_move_speed(450.0f, 0.0f, 0.0f), 450.0f, 1e-5f));
    /* get_move_speed floors at zero, so a decaying pull cannot reverse input. */
    CHECK(nearf(bt_move_speed(0.0f, 0.0f, -10.0f), 0.0f, 1e-6f));

    CHECK(vnear(bt_move_input(bt_v2(3.0f, 4.0f), 6.0f, 0.0f, 0.0f), bt_v2(3.6f, 4.8f), 1e-5f));
    CHECK(vnear(bt_move_input(bt_v2zero(), 6.0f, 0.0f, 0.0f), bt_v2zero(), 0.0f));
}

/* ---------------------------------------------------------------- */
/* knockback direction, replacement, decay, resistance               */
/* ---------------------------------------------------------------- */
static void test_knockback(void) {
    /* scale = 100 - resistance * 100. */
    CHECK(nearf(bt_knockback_scale(0.0f), 100.0f, 1e-5f));
    CHECK(nearf(bt_knockback_scale(0.8f), 20.0f, 1e-4f));
    CHECK(bt_knockback_scale(1.0f) == 0.0f);

    /* plain direction is the hitbox direction verbatim. */
    CHECK(vnear(bt_knockback_direction(bt_v2(0.0f, 1.0f)), bt_v2(0.0f, 1.0f), 0.0f));

    /* weighted direction: 0.2 * hitbox + 0.8 * attacker.direction_to(victim). */
    BtVec2 w = bt_knockback_direction_weighted(bt_v2(0.0f, 1.0f), bt_v2(0.0f, 0.0f), bt_v2(10.0f, 0.0f));
    CHECK(vnear(w, bt_v2(0.8f, 0.2f), 1e-6f));

    /* coincident attacker/victim leaves only the hitbox term. */
    BtVec2 wc = bt_knockback_direction_weighted(bt_v2(1.0f, 0.0f), bt_v2(5.0f, 5.0f), bt_v2(5.0f, 5.0f));
    CHECK(vnear(wc, bt_v2(0.2f, 0.0f), 1e-6f));

    /* the weighted sum is NOT renormalised: its length is below 1. */
    BtVec2 wn = bt_knockback_direction_weighted(bt_v2(1.0f, 0.0f), bt_v2(0.0f, 0.0f), bt_v2(10.0f, 10.0f));
    CHECK(bt_v2len(wn) < 0.999f);
    CHECK(bt_v2len(wn) > 0.5f);
    CHECK(vnear(wn, bt_v2(0.2f + 0.8f * 0.70710678f, 0.8f * 0.70710678f), 1e-5f));

    /* a hit REPLACES the stored vector with direction * amount. */
    CHECK(vnear(bt_stored_knockback(bt_v2(0.8f, 0.2f), 5.0f), bt_v2(4.0f, 1.0f), 1e-6f));

    /* decay runs before the resistance scaling. */
    BtKnockbackTick t = bt_knockback_advance(bt_v2(10.0f, 20.0f), 0.0f);
    CHECK(vnear(t.stored, bt_v2(9.0f, 18.0f), 1e-5f));
    CHECK(vnear(t.velocity, bt_v2(900.0f, 1800.0f), 1e-2f));
    /* velocity is exactly stored_after * scale, not stored_before * scale * k. */
    CHECK(nearf(t.velocity.x, t.stored.x * 100.0f, 1e-3f));

    /* resistance 1 removes the velocity entirely but still decays the store. */
    BtKnockbackTick t1 = bt_knockback_advance(bt_v2(10.0f, 20.0f), 1.0f);
    CHECK(vnear(t1.stored, bt_v2(9.0f, 18.0f), 1e-5f));
    CHECK(t1.velocity.x == 0.0f && t1.velocity.y == 0.0f);

    /* an empty store yields zero and is left untouched. */
    BtKnockbackTick tz = bt_knockback_advance(bt_v2zero(), 0.5f);
    CHECK(tz.stored.x == 0.0f && tz.stored.y == 0.0f);
    CHECK(tz.velocity.x == 0.0f && tz.velocity.y == 0.0f);

    /* next velocity = move input + knockback contribution. */
    BtVec2 stored = bt_v2(10.0f, 20.0f);
    BtVec2 nv = bt_next_velocity(bt_v2(3.0f, 4.0f), 6.0f, 0.0f, 0.0f, 0.0f, stored, &stored);
    CHECK(vnear(stored, bt_v2(9.0f, 18.0f), 1e-5f));
    CHECK(vnear(nv, bt_v2(3.6f + 900.0f, 4.8f + 1800.0f), 1e-2f));

    /* death knockback floors at 15.0 and uses the raw hitbox amount. */
    CHECK(vnear(bt_death_knockback(bt_v2(1.0f, 0.0f), 5.0f), bt_v2(15.0f, 0.0f), 1e-6f));
    CHECK(vnear(bt_death_knockback(bt_v2(1.0f, 0.0f), 40.0f), bt_v2(40.0f, 0.0f), 1e-6f));
}

/* ---------------------------------------------------------------- */
/* piercing correction                                               */
/* ---------------------------------------------------------------- */
static void test_piercing(void) {
    /* min(resistance, piercing): resistance 0 never adds anything. */
    CHECK(nearf(bt_piercing_knockback_amount(10.0f, 0.5f, 0.0f), 10.0f, 1e-6f));
    /* piercing >= resistance: amount + amount * resistance / (1 - resistance). */
    CHECK(nearf(bt_piercing_knockback_amount(10.0f, 1.0f, 0.5f), 20.0f, 1e-5f));
    CHECK(nearf(bt_piercing_knockback_amount(10.0f, 5.0f, 0.5f), 20.0f, 1e-5f));
    /* piercing below resistance scales the added share. */
    CHECK(nearf(bt_piercing_knockback_amount(10.0f, 0.2f, 0.5f), 14.0f, 1e-5f));
    /* resistance 1 is the untouched special case (no divide by zero). */
    CHECK(nearf(bt_piercing_knockback_amount(10.0f, 3.0f, 1.0f), 10.0f, 0.0f));
}

/* ---------------------------------------------------------------- */
/* negative pull amount + decaying speed                             */
/* ---------------------------------------------------------------- */
static void test_pull(void) {
    /* distance 200, raw radii 32/32, no movement, no piercing:
     *   buffer = 32 + 32 + 0 + 0 + min(200/3, 150) = 130.6667
     *   max_travel = 69.3333
     *   expected = | -60 * 100 / 6 | = 1000 > max_travel
     *   adjusted = -(69.3333 * 6 / 100) = -4.16 */
    float adj = bt_pull_knockback_amount(-60.0f, bt_v2(0.0f, 0.0f), 32.0f, 0.0f, bt_v2zero(),
                                         bt_v2(200.0f, 0.0f), 32.0f, 0.0f, bt_v2zero(), 0.0f, 0.0f);
    CHECK(nearf(adj, -4.16f, 1e-3f));

    /* when expected travel fits inside max_travel the base amount is kept. */
    float adj2 = bt_pull_knockback_amount(-1.0f, bt_v2(0.0f, 0.0f), 10.0f, 0.0f, bt_v2zero(),
                                          bt_v2(200.0f, 0.0f), 10.0f, 0.0f, bt_v2zero(), 0.0f, 0.0f);
    CHECK(nearf(adj2, -1.0f, 1e-6f));

    /* attacker moving away (attacker_dot > 0) shrinks attacker_speed_factor x0.1:
     *   attacker_speed_factor = -(1 * 60 / 6) = -10 -> -1
     *   buffer = 64 - 1 + 66.6667 = 129.6667 ; max_travel = 70.3333
     *   adjusted = -(70.3333 * 6 / 100) = -4.22 */
    float adj3 = bt_pull_knockback_amount(-60.0f, bt_v2(0.0f, 0.0f), 32.0f, 0.0f, bt_v2zero(),
                                          bt_v2(200.0f, 0.0f), 32.0f, 60.0f, bt_v2(1.0f, 0.0f), 0.0f, 0.0f);
    CHECK(nearf(adj3, -4.22f, 2e-3f));
    /* the same attacker moving toward the victim (dot < 0) keeps the full factor:
     *   attacker_speed_factor = -(-1 * 60 / 6) = +10
     *   buffer = 64 + 10 + 66.6667 = 140.6667 ; max_travel = 59.3333
     *   adjusted = -3.56 : a larger buffer, so a shorter (less negative) clamp. */
    float adj4 = bt_pull_knockback_amount(-60.0f, bt_v2(0.0f, 0.0f), 32.0f, 0.0f, bt_v2zero(),
                                          bt_v2(200.0f, 0.0f), 32.0f, 60.0f, bt_v2(-1.0f, 0.0f), 0.0f, 0.0f);
    CHECK(nearf(adj4, -3.56f, 2e-3f));
    CHECK(adj4 > adj3);

    /* piercing is applied to the already-clamped pull amount:
     *   -4.16 + (-4.16 * min(0.5, 1.0) / 0.5) = -8.32 */
    float adj5 = bt_pull_knockback_amount(-60.0f, bt_v2(0.0f, 0.0f), 32.0f, 0.0f, bt_v2zero(),
                                          bt_v2(200.0f, 0.0f), 32.0f, 0.0f, bt_v2zero(), 1.0f, 0.5f);
    CHECK(nearf(adj5, -8.32f, 2e-3f));

    /* add_decaying_speed: normal add. */
    CHECK(nearf(bt_add_decaying_speed(5, 100.0f, 0.0f), 5.0f, 1e-6f));
    /* clamp at 0.9 * speed. */
    CHECK(nearf(bt_add_decaying_speed(-300, 100.0f, 0.0f), -90.0f, 1e-6f));
    /* /20 attenuation once already past 0.45 * speed (integer truncation). */
    CHECK(nearf(bt_add_decaying_speed(-5, 100.0f, -50.0f), -50.0f, 1e-6f));
    /* hard stop once past 0.7 * speed. */
    CHECK(nearf(bt_add_decaying_speed(-5, 100.0f, -80.0f), -80.0f, 1e-6f));
    /* float argument is truncated toward zero at the boundary. */
    CHECK(nearf(bt_add_decaying_speed_amt(-300.9f, 100.0f, 0.0f), -90.0f, 1e-6f));
}

/* ---------------------------------------------------------------- */
/* _integrate_forces: clamp then snap                                */
/* ---------------------------------------------------------------- */
static void test_integrate(void) {
    /* outside on +x: rebuild velocity from the clamped predicted origin. */
    BtVec2 v = bt_zone_clamp_velocity(bt_v2(0.0f, 0.0f), bt_v2(1000.0f, 0.0f), 1.0f,
                                      0.0f, 0.0f, 500.0f, 500.0f);
    CHECK(vnear(v, bt_v2(500.0f, 0.0f), 1e-4f));

    /* predicted origin exactly on the exclusive max edge still clamps, and the
     * rebuilt velocity is numerically unchanged. */
    BtVec2 vb = bt_zone_clamp_velocity(bt_v2(400.0f, 0.0f), bt_v2(100.0f, 0.0f), 1.0f,
                                       0.0f, 0.0f, 500.0f, 500.0f);
    CHECK(vnear(vb, bt_v2(100.0f, 0.0f), 1e-4f));

    /* fully inside: untouched. */
    BtVec2 vi = bt_zone_clamp_velocity(bt_v2(0.0f, 0.0f), bt_v2(100.0f, 0.0f), 1.0f,
                                       0.0f, 0.0f, 500.0f, 500.0f);
    CHECK(vnear(vi, bt_v2(100.0f, 0.0f), 0.0f));

    /* negative overshoot clamps to the origin corner. */
    BtVec2 vn = bt_zone_clamp_velocity(bt_v2(10.0f, 10.0f), bt_v2(-100.0f, 0.0f), 1.0f,
                                       0.0f, 0.0f, 500.0f, 500.0f);
    CHECK(vnear(vn, bt_v2(-10.0f, 0.0f), 1e-4f));

    /* y overshoot. */
    BtVec2 vy = bt_zone_clamp_velocity(bt_v2(0.0f, 100.0f), bt_v2(0.0f, 1000.0f), 1.0f,
                                       0.0f, 0.0f, 500.0f, 500.0f);
    CHECK(vnear(vy, bt_v2(0.0f, 400.0f), 1e-4f));

    /* snap when the target sits at/just behind the step's end. */
    BtIntegrateResult r = bt_integrate_forces(bt_v2(0.0f, 0.0f), bt_v2(100.0f, 0.0f), 1.0f,
                                              0.0f, 0.0f, 500.0f, 500.0f, 1, bt_v2(50.0f, 0.0f), 1, 0);
    CHECK(r.snapped == 1);
    CHECK(r.velocity.x == 0.0f && r.velocity.y == 0.0f);
    CHECK(vnear(r.origin, bt_v2(50.0f, 0.0f), 0.0f));

    /* far target: keep velocity, do not teleport. */
    BtIntegrateResult r2 = bt_integrate_forces(bt_v2(0.0f, 0.0f), bt_v2(100.0f, 0.0f), 1.0f,
                                               0.0f, 0.0f, 500.0f, 500.0f, 1, bt_v2(200.0f, 0.0f), 1, 0);
    CHECK(r2.snapped == 0);
    CHECK(vnear(r2.velocity, bt_v2(100.0f, 0.0f), 0.0f));
    CHECK(vnear(r2.origin, bt_v2(0.0f, 0.0f), 0.0f));

    /* dot > 0 branch: target behind the origin but within step length. */
    BtIntegrateResult r3 = bt_integrate_forces(bt_v2(0.0f, 0.0f), bt_v2(60.0f, 80.0f), 1.0f,
                                               0.0f, 0.0f, 500.0f, 500.0f, 1, bt_v2(-60.0f, -80.0f), 1, 0);
    CHECK(r3.snapped == 0);
    CHECK(vnear(r3.velocity, bt_v2(60.0f, 80.0f), 0.0f));

    /* empty velocity is an exact-zero early out. */
    BtIntegrateResult r4 = bt_integrate_forces(bt_v2(0.0f, 0.0f), bt_v2zero(), 1.0f,
                                               0.0f, 0.0f, 500.0f, 500.0f, 1, bt_v2(50.0f, 0.0f), 1, 0);
    CHECK(r4.snapped == 0);
    CHECK(r4.velocity.x == 0.0f && r4.velocity.y == 0.0f);

    /* can_move / move_locked / missing target all suppress the snap but keep
     * the velocity. */
    BtIntegrateResult r5 = bt_integrate_forces(bt_v2(0.0f, 0.0f), bt_v2(100.0f, 0.0f), 1.0f,
                                               0.0f, 0.0f, 500.0f, 500.0f, 1, bt_v2(50.0f, 0.0f), 0, 0);
    CHECK(r5.snapped == 0 && vnear(r5.velocity, bt_v2(100.0f, 0.0f), 0.0f));
    BtIntegrateResult r6 = bt_integrate_forces(bt_v2(0.0f, 0.0f), bt_v2(100.0f, 0.0f), 1.0f,
                                               0.0f, 0.0f, 500.0f, 500.0f, 1, bt_v2(50.0f, 0.0f), 1, 1);
    CHECK(r6.snapped == 0 && vnear(r6.velocity, bt_v2(100.0f, 0.0f), 0.0f));
    BtIntegrateResult r7 = bt_integrate_forces(bt_v2(0.0f, 0.0f), bt_v2(100.0f, 0.0f), 1.0f,
                                               0.0f, 0.0f, 500.0f, 500.0f, 0, bt_v2(50.0f, 0.0f), 1, 0);
    CHECK(r7.snapped == 0 && vnear(r7.velocity, bt_v2(100.0f, 0.0f), 0.0f));

    /* clamp happens first, and the clamped velocity can then snap. */
    BtIntegrateResult r8 = bt_integrate_forces(bt_v2(0.0f, 0.0f), bt_v2(1000.0f, 0.0f), 1.0f,
                                               0.0f, 0.0f, 500.0f, 500.0f, 1, bt_v2(500.0f, 0.0f), 1, 0);
    CHECK(r8.snapped == 1);
    CHECK(r8.velocity.x == 0.0f && r8.velocity.y == 0.0f);
    CHECK(vnear(r8.origin, bt_v2(500.0f, 0.0f), 0.0f));
}

/* ---------------------------------------------------------------- */
/* discrete circle overlap with composed offsets                     */
/* ---------------------------------------------------------------- */
static void test_overlap(void) {
    /* touching counts as overlapping (SAT separation is dist > rA + rB). */
    CHECK(bt_circle_overlap(bt_v2(0.0f, 0.0f), 10.0f, bt_v2(20.0f, 0.0f), 10.0f) == 1);
    /* just beyond contact does not. */
    CHECK(bt_circle_overlap(bt_v2(0.0f, 0.0f), 10.0f, bt_v2(20.001f, 0.0f), 10.0f) == 0);
    /* clear overlap. */
    CHECK(bt_circle_overlap(bt_v2(0.0f, 0.0f), 10.0f, bt_v2(10.0f, 0.0f), 10.0f) == 1);
    /* concentric zero-radius circles still "overlap" in the engine's test. */
    CHECK(bt_circle_overlap(bt_v2(0.0f, 0.0f), 0.0f, bt_v2(0.0f, 0.0f), 0.0f) == 1);

    /* player body (r24, offset (0,-12)) vs enemy contact hitbox (r16.0312, (0,0)):
     * composed centres are 12 apart, threshold 40.0312. */
    CHECK(bt_circle_overlap_offset(bt_v2(0.0f, 0.0f), bt_v2(0.0f, -12.0f), 24.0f,
                                   bt_v2(0.0f, 0.0f), bt_v2(0.0f, 0.0f), 16.0312f) == 1);
    CHECK(bt_circle_overlap_offset(bt_v2(0.0f, 0.0f), bt_v2(0.0f, -12.0f), 24.0f,
                                   bt_v2(0.0f, 100.0f), bt_v2(0.0f, 0.0f), 16.0312f) == 0);

    /* player hurtbox (r21, offset (0,-12)) vs that hitbox: 12 <= 37.0312. */
    CHECK(bt_circle_overlap_offset(bt_v2(0.0f, 0.0f), bt_v2(0.0f, -12.0f), 21.0f,
                                   bt_v2(0.0f, 0.0f), bt_v2(0.0f, 0.0f), 16.0312f) == 1);

    /* offsets shift the test: same roots, only-compensating offsets touch. */
    CHECK(bt_circle_overlap_offset(bt_v2(0.0f, 0.0f), bt_v2(0.0f, -12.0f), 6.0f,
                                   bt_v2(0.0f, 0.0f), bt_v2(0.0f, 0.0f), 6.0f) == 1);
    CHECK(bt_circle_overlap_offset(bt_v2(0.0f, 0.0f), bt_v2(0.0f, -12.001f), 6.0f,
                                   bt_v2(0.0f, 0.0f), bt_v2(0.0f, 0.0f), 6.0f) == 0);
}

int main(void) {
    test_vectors();
    test_rect_clamp();
    test_filter();
    test_movement();
    test_knockback();
    test_piercing();
    test_pull();
    test_integrate();
    test_overlap();

    std::printf("brotato_physics_math: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
