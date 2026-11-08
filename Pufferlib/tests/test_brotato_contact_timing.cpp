/* tests/test_brotato_contact_timing.cpp
 *
 * Behavioral fixtures for the player/enemy contact TIMING in
 * ocean/brotato/brotato_core.h + brotato_state.h.  Self-contained: includes
 * only the core header and <stdio.h>.  No Env, no raylib, no engine.
 *
 * Build and run:
 *     c++ -O2 -o build/test_brotato_contact_timing tests/test_brotato_contact_timing.cpp -lm
 *     ./build/test_brotato_contact_timing
 *
 * Exit code 0 = all checks passed.
 *
 * What is pinned (evidence: entity.tscn mode=2 / player.tscn mode=3, and
 * main.cpp's per-frame order Physics2DServer::sync -> flush_queries ->
 * Node._physics_process -> end_sync -> Physics2DServer::step):
 *
 *  [1] DEFERRED HIT.  The enemy contact Hitbox is an Area2D child of the enemy
 *      Unit; the pair is formed by Physics2DServer::step and only delivered as
 *      `area_entered` at the NEXT frame's flush_queries, where unit.gd:610-611
 *      call_deferred()s hurt_area_entered_deferred.  A pair that already exists
 *      at the start of a step therefore lands its take_damage at the END of the
 *      FOLLOWING step: an overlapping static enemy must not damage on the first
 *      step, only on the second.
 *
 *  [2] LAGGED HITBOX AREA.  A MODE_CHARACTER body's Node transform (and thus
 *      every child Area2D transform) is one step behind the integrated body, so
 *      step(t) tests the enemy Hitbox at the position the enemy had at the END
 *      of step(t-1) (BtState.ehx/ehy), while the MODE_KINEMATIC player's
 *      Hurtbox is already at step(t)'s position.  A walking enemy therefore
 *      damages only when the position at a step BOUNDARY (not the position
 *      after the step) is inside the contact circle: with the current-position
 *      overlap first appearing on step (t), damage first lands on step (t+2).
 *
 * Both fixtures read the contact circle from the core's own geometry
 * (bt_kgeom) -- no radius or offset is hard-coded to fit an observation.
 */

#include "../ocean/brotato/brotato_core.h"

#include <stdio.h>

static int g_failures = 0;
static int g_checks = 0;

static void check(int cond, const char* msg) {
    g_checks++;
    if (!cond) { g_failures++; printf("  FAIL: %s\n", msg); }
}

/* BtState is ~1 MiB; keep it static so it never sits on the stack. */
static BtState A;

static float obs[BT_OBS_SIZE], rew, term;

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

/* Signed distance test between the player Hurtbox circle and slot k's contact
 * Hitbox circle, using the AREA sample at (sx, sy). */
static float contact_dist(BtState* s, int k, float sx, float sy) {
    BT_CONST_PTR(BtBodyPhysics) G = bt_kgeom(s, k);
    float hx = sx + G->hit_x, hy = sy + G->hit_y;
    float dx = hx - s->px, dy = hy - (s->py + BT_PLAYER_HURT_Y);
    return bt_sqrtf(dx * dx + dy * dy) - (G->hit_radius + BT_PLAYER_HURT_R);
}

int main(void) {
    int chaser = find_kind("chaser");
    check(chaser >= 0, "chaser kind found (a contact-Hitbox enemy without projectiles)");
    if (chaser < 0) { printf("test_brotato_contact_timing: FAILED (%d)\n", g_failures); return 1; }

    /* --------------------------------------------------------------------- */
    printf("[1] an overlapping static enemy damages one step late (call_deferred)\n");
    {
        bt_core_init(&A, 91u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;                     /* no wave spawns */
        A.wave = 1;
        A.iframe = 0.0f;
        A.regen = 0.0f; A.regen_timer = bt_regen_period(0.0f);
        int k = bt_reserve_slot(&A);
        bt_spawn_kind_at(&A, k, chaser, A.px + 20.0f, A.py, 0);
        A.espd[k] = 0.0f;                        /* parked: no commanded motion */
        check(A.ealive[k], "enemy live");
        check(contact_dist(&A, k, A.ex[k], A.ey[k]) < 0.0f,
              "the parked enemy's Hitbox already overlaps the player Hurtbox");

        bt_core_step(&A, 0, obs, &rew, &term);   /* step 1: pair forms, deferred */
        check(A.hp == 30.0f, "step 1 of an existing overlap deals no damage (deferred)");
        check(A.iframe == 0.0f, "step 1 sets no i-frames");
        float ex1 = A.ex[k], ey1 = A.ey[k];

        bt_core_step(&A, 0, obs, &rew, &term);   /* step 2: deferred hit lands */
        check(A.hp == 29.0f, "step 2 lands the deferred contact damage (1 hp)");
        check(A.iframe > 0.0f, "the landed hit starts the invincibility timer");
        check(bt_fabsf(A.ex[k] - ex1) < 1e-3f && bt_fabsf(A.ey[k] - ey1) < 1e-3f,
              "the zero-speed enemy stayed parked (the overlap was continuous)");
    }

    /* --------------------------------------------------------------------- */
    printf("[2] a walking enemy damages two steps after its CURRENT overlap\n");
    {
        bt_core_init(&A, 92u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        A.wave = 1;
        A.iframe = 0.0f;
        /* no hp_regeneration tick inside the window (Bull's regen would heal the
         * single contact hit back before the loop ends) */
        A.regen = 0.0f;
        A.regen_timer = bt_regen_period(0.0f);
        A.px = 1000.0f; A.py = 768.0f;           /* player parked (action 0) */
        int k = bt_reserve_slot(&A);
        bt_spawn_kind_at(&A, k, chaser, A.px + 300.0f, A.py, 0);
        A.espd[k] = 1200.0f;                     /* 20 px/step: closes the gap */

        int first_boundary_overlap = -1;   /* step whose START sample overlaps  */
        int first_current_overlap = -1;    /* step whose END position overlaps  */
        int first_damage = -1;
        int damages = 0;
        for (int t = 0; t < 22; t++) {
            float sx = A.ex[k], sy = A.ey[k];    /* the AREA sample this step uses */
            int boundary = contact_dist(&A, k, sx, sy) <= 0.0f;
            float hp0 = A.hp;
            bt_core_step(&A, 0, obs, &rew, &term);
            int current = contact_dist(&A, k, A.ex[k], A.ey[k]) <= 0.0f;
            if (boundary && first_boundary_overlap < 0) first_boundary_overlap = t;
            if (current && first_current_overlap < 0) first_current_overlap = t;
            if (A.hp < hp0) { damages++; if (first_damage < 0) first_damage = t; }
        }

        printf("      first_boundary_overlap=%d first_current_overlap=%d first_damage=%d damages=%d\n",
               first_boundary_overlap, first_current_overlap, first_damage, damages);
        check(first_boundary_overlap >= 1, "the enemy walks in and reaches the contact circle");
        /* post-step position at step t == the AREA sample at step t+1 */
        check(first_current_overlap == first_boundary_overlap - 1,
              "the current-position overlap is one step ahead of the boundary sample");
        check(first_damage == first_boundary_overlap + 1,
              "damage lands the step after the boundary sample overlaps (lagged AREA)");
        check(first_damage == first_current_overlap + 2,
              "the CURRENT-position overlap never damages on its own step (2-step total)");
        check(damages == 1, "exactly one contact hit inside one invincibility window");
        check(A.hp == 29.0f, "one contact hit costs 1 hp (take_damage(1))");
    }

    /* --------------------------------------------------------------------- */
    printf("[3] the contact Hitbox AREA is sampled at the pre-step position\n");
    {
        bt_core_init(&A, 93u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        A.wave = 1;
        A.iframe = 0.0f;
        A.regen = 0.0f; A.regen_timer = bt_regen_period(0.0f);
        A.px = 1000.0f; A.py = 768.0f;
        int k = bt_reserve_slot(&A);
        bt_spawn_kind_at(&A, k, chaser, A.px + 300.0f, A.py, 0);
        A.espd[k] = 1200.0f;
        /* the newborn hold step and the AI's one-step velocity cache must both
         * burn before the body actually moves */
        for (int t = 0; t < 3; t++) bt_core_step(&A, 0, obs, &rew, &term);
        float x0 = A.ex[k], y0 = A.ey[k];
        bt_core_step(&A, 0, obs, &rew, &term);
        check(A.ehx[k] == x0 && A.ehy[k] == y0,
              "ehx/ehy hold the step-start (Node-visible) position");
        check(A.ex[k] != x0 || A.ey[k] != y0,
              "the body did move during the step (the sample is not the integrated position)");
    }

    if (g_failures) {
        printf("test_brotato_contact_timing: %d/%d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("test_brotato_contact_timing: all %d checks passed\n", g_checks);
    return 0;
}
