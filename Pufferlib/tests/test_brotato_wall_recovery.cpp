/* tests/test_brotato_wall_recovery.cpp
 *
 * Pins the engine's STEP-1 free-body recovery (Space2DSW::test_body_motion,
 * space_2d_sw.cpp:786-885) the way unit.gd:148-154 consumes it: the player has
 * settled against an arena wall inside the 0.08 test-motion margin, and on the
 * frame it moves OFF that wall the recovery shift is still part of
 * `result.motion`, so it is added as EXTRA motion even though the rest query
 * reports no collision for that call (`r->motion += body_transform - p_from`,
 * space_2d_sw.cpp:1207-1211).  Ignoring that term drops the shift.
 *
 * Evidence: the trained-policy capture
 *   ~/brotato_eval/demon1b_full/run_eval1/rl_out/rl_trace.eval1.jsonl
 * pushes the player UP into the top arena wall for ticks 190-198 (action 1);
 * game and sim both rest at y=36.066345.  Tick 199 moves diagonally away
 * (action 4) and the game moves y +5.311707 while the core that added the raw
 * motion moved +5.303303 -- the missing 0.008404 px then rode the episode and
 * at tick 660 flipped an edge-of-hitbox kill.  With the recovery kept, the
 * replay's first divergence moves from tick 199 to tick 223 and the positions
 * agree through tick 798.
 *
 * Build and run:
 *     c++ -O2 -o build/test_brotato_wall_recovery tests/test_brotato_wall_recovery.cpp -lm
 *     ./build/test_brotato_wall_recovery
 *
 * Exit code 0 = all checks passed.
 */

#include "../ocean/brotato/brotato_core.h"

#include <stdio.h>

static int g_failures = 0;
static int g_checks = 0;

static int nearf(float a, float b, float tol) { float d = a - b; return (d < 0 ? -d : d) <= tol; }

static void check(int cond, const char* msg) {
    g_checks++;
    if (!cond) { g_failures++; printf("  FAIL: %s\n", msg); }
}

/* BtState is ~1 MiB; keep it in BSS so it never sits on the stack. */
static BtState A;
static float obs[BT_OBS_SIZE], rew, term;

/* The top arena wall's inner face is y = 0 (my_tile_map_limits.gd:27-35,
 * 4 * Utils.TILE_SIZE deep, grown past both corners).  The player's collision
 * circle is centred at py + BT_PLAYER_BODY_Y, so its topmost point -- the only
 * point that can reach the wall -- sits at py + BT_PLAYER_BODY_Y - BT_PLAYER_BODY_R. */
static float player_wall_gap(void) {
    return (A.py + BT_PLAYER_BODY_Y - BT_PLAYER_BODY_R) - 0.0f;
}

int main(void) {
    int up = -1, diag = -1, a;
    for (a = 0; a < BT_ACTIONS; a++) {
        if (BT_MOVE_X[a] == 0.0f && BT_MOVE_Y[a] < 0.0f && up < 0) up = a;
        if (BT_MOVE_X[a] > 0.0f && BT_MOVE_Y[a] > 0.0f && diag < 0) diag = a;
    }
    check(up >= 0 && diag >= 0, "the action table has a pure-up and a down-right action");

    /* Player speed is BT_PLAYER_SPEED * (1 + p_speed_pct/100) (player.gd:352,
     * unit.gd:615); with no items bought p_speed_pct is 0. */
    const float raw_x = BT_PLAYER_SPEED * BT_DT * BT_MOVE_X[diag];
    const float raw_y = BT_PLAYER_SPEED * BT_DT * BT_MOVE_Y[diag];

    bt_core_init(&A, 8u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
    A.n_live_groups = 0;                 /* no spawns: isolate the wall contact */

    /* ~98 frames from the arena centre to the wall at 7.5 px/frame; the last
     * frames settle the margin-limited rest. */
    int t;
    float prev_y = A.py;
    for (t = 0; t < 200; t++) {
        prev_y = A.py;
        bt_core_step(&A, up, obs, &rew, &term);
    }
    check(A.py < BT_ARENA_H * 0.05f, "the player reaches the top wall moving up");
    check(A.py - prev_y > -1e-5f, "the settled player holds its resting y (recovery and motion cancel)");

    /* Resting contact: the margin-limited rest sits INSIDE the 0.08 test-motion
     * margin, which is what makes the next frame's STEP 1 recover. */
    float rest_y = A.py, rest_x = A.px;
    check(player_wall_gap() > 0.0f && player_wall_gap() < 0.08f,
          "the settled player rests inside the 0.08 test-motion margin");

    /* The engine-side rule: with the body at that rest pose, a ZERO-motion
     * test_motion still reports the recovery as (part of) result.motion. */
    {
        BtPhysicsBody p = bt_phys_body_circle(bt_v2(A.px, A.py), bt_v2(0.0f, BT_PLAYER_BODY_Y),
                                              BT_PLAYER_BODY_R, 1.0f, BT_LAYER_PLAYER, BT_MASK_PLAYER,
                                              BT_PHYS_MODE_KINEMATIC);
        BtPhysMotionResult r;
        int c = bt_phys_test_body_motion(&A.phys, &p, bt_v2zero(), 0.08f, &r);
        check(c == 1, "the resting pose is still a reported contact for test_motion");
        check(r.collision_normal.y > 0.99f && r.collision_normal.x == 0.0f,
              "the resting contact normal points away from the wall (+y)");
        check(r.motion.x == 0.0f && r.motion.y > 0.0f,
              "a zero-motion call still returns the STEP-1 recovery as result.motion");
    }

    /* Leaving the wall: the recovery rides along as extra motion on that frame
     * only.  Game (tick 199): y +5.311707 = 5.303303 raw + 0.008404 recovery. */
    bt_core_step(&A, diag, obs, &rew, &term);
    float dx = A.px - rest_x, dy = A.py - rest_y;
    float surplus = dy - raw_y;
    printf("leave frame: dx=%.7f dy=%.7f (raw %.7f) surplus=%.7f\n", dx, dy, raw_y, surplus);
    check(nearf(dx, raw_x, 1e-3f), "leaving-frame x is the plain diagonal motion (no recovery in x)");
    check(dy > raw_y, "leaving-frame y keeps the wall recovery as extra motion");
    check(surplus > 0.0080f && surplus < 0.0088f,
          "the extra y is the engine's STEP-1 recovery for this rest (0.008404 px)");

    /* The NEXT frame has no contact left, so it is the plain motion again. */
    float y1 = A.py;
    bt_core_step(&A, diag, obs, &rew, &term);
    check(nearf(A.py - y1, raw_y, 1e-3f), "the frame after the leave is the plain motion again");

    if (g_failures) { printf("test_brotato_wall_recovery: FAILED (%d/%d)\n", g_failures, g_checks); return 1; }
    printf("test_brotato_wall_recovery: OK (%d checks)\n", g_checks);
    return 0;
}
