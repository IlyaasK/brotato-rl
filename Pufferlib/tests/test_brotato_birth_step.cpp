/* tests/test_brotato_birth_step.cpp
 *
 * Regression for the birth -> first-physics-step ordering of a newborn enemy.
 *
 * Source behaviour that is pinned here:
 *   global/entity_spawner.gd:319-331  on_entity_birth_timeout -> spawn_entity:
 *                                     the enemy node is added from the DEFERRED
 *                                     `call_deferred("birth")` boundary
 *                                     (entities/birth/entity_birth.gd:76), i.e.
 *                                     BETWEEN two Physics2DServer steps, so the
 *                                     newborn is integrated by the very next
 *                                     step -- with a zero commanded velocity,
 *                                     because Godot runs body integration
 *                                     before (and does not visit a just-added
 *                                     node in) that frame's _physics_process.
 *   entities/units/unit/unit.gd:164-212
 *                                     _integrate_forces assigns
 *                                     state.linear_velocity from the velocity
 *                                     cached by the previous _physics_process,
 *                                     so the first live snapshot shows the body
 *                                     after ONE step (wall overlap already
 *                                     solved) and the following snapshot still
 *                                     shows the birth position.
 *
 * Engine numbers measured on the pinned engine (3.7.dev1, 0a9011743) with a
 * SceneTree probe equivalent to tests/brotato_physics_fixture.gd: a
 * MODE_CHARACTER RigidBody2D (CircleShape2D r=22, layer 4 / mask 132, friction
 * 0, custom_integrator, zero gravity/damp) added from a deferred call at the
 * arena's right wall position below, commanded velocity zero, reports
 *   first step  x = 2040.882324   second step x = 2036.507568
 * and the recorded Bull wave-1 trace shows 2040.8832 / 2036.5066 on the matching
 * ticks of its fourth enemy (trace obs are float32: 2040.8823 encodes the same
 * value at the bridge's 6-decimal precision).
 *
 * Build and run:
 *     c++ -O2 -o build/test_brotato_birth_step tests/test_brotato_birth_step.cpp -lm
 *     ./build/test_brotato_birth_step
 *
 * Exit code 0 = all checks passed.
 */

#include "../ocean/brotato/brotato_core.h"

#include <math.h>
#include <stdio.h>

static int g_failures = 0;
static int g_checks = 0;

static void check(int cond, const char* msg) {
    g_checks++;
    if (!cond) { g_failures++; printf("  FAIL: %s\n", msg); }
}

/* Persistent world belongs in static storage, not on the stack. */
static BtState S;

/* ATOL matches tests/brotato_physics_compare.py's body-solver gate: the core is
 * a transcription of the engine solver, not a bit-identical rebuild. */
static const float ATOL = 0.002f;

static int first_alive(void) {
    for (int i = 0; i < BT_MAX; i++) if (S.ealive[i]) return i;
    return -1;
}

/* A kind whose body circle uses the large radius the wall case needs (the
 * engine fixture above used r = 22). */
static int kind_with_radius(float radius) {
    for (int k = 0; k < BT_NKINDS; k++) {
        if (BT_PHYSICS[bt_geom_index(k)].body_radius == radius) return k;
    }
    return -1;
}

/* Queue one ENEMY birth at an explicit position and run the two frames that
 * make it live: frame 1 counts the birth out (bt_birth_advance -> ready), frame
 * 2 flushes it from the deferred boundary at the top of the frame and then runs
 * bt_phys_step, so the returned slot has just had its first step. */
static int birth_and_activate(BtState* s, int kind, float x, float y) {
    s->n_births = 1;
    s->births[0].kind = (unsigned short)kind;
    s->births[0].row = 0;
    s->births[0].neutral = 0;
    s->births[0].is_boss = 0;
    s->births[0].bclass = 0;
    s->births[0].active = 1;
    s->births[0].src = -1;
    s->births[0].charmed_by = -1;
    s->births[0].x = x;
    s->births[0].y = y;
    s->births[0].timer = 0.0f;
    float obs[BT_OBS_SIZE], reward = 0.0f, terminal = 0.0f;
    bt_core_step(s, 0, obs, &reward, &terminal);
    check(s->n_births == 1 && s->ecount == 0,
          "precondition: the birth counted out but is not yet a live unit");
    bt_core_step(s, 0, obs, &reward, &terminal);
    return first_alive();
}

int main(void) {
    const int kind = kind_with_radius(22.0f);
    check(kind >= 0, "a 22-radius enemy kind exists in the generated tables");
    if (kind < 0) return 1;
    const float r = BT_PHYSICS[bt_geom_index(kind)].body_radius;

    /* ---- 1. newborn overlapping the arena's right wall ---------------- */
    {
        bt_core_init(&S, 1ull, 0, 1, (unsigned short)BT_CHARACTER_BULL,
                     (unsigned short)BT_WEAPON_NONE);
        /* Inside the wall body (inner face x = BT_ARENA_W), the same position
         * the recorded run's fourth wave-1 enemy was born at. */
        const float bx = 2047.131836f, by = 1043.144653f;
        check(BT_ARENA_W - bx < r, "birth position overlaps the arena wall");
        const int slot = birth_and_activate(&S, kind, bx, by);
        check(slot >= 0, "the newborn is live on its appearance frame");
        if (slot < 0) return 1;

        /* The first live snapshot is the post-step position: the overlap is
         * solved by the frame's own bt_phys_step, not one frame later. */
        check(S.ex[slot] < bx - 1.0f,
              "wall overlap is solved on the newborn's own first snapshot");
        check(fabsf(S.ex[slot] - 2040.882324f) < ATOL &&
              fabsf(S.ey[slot] - by) < ATOL,
              "first snapshot matches the engine's first-step position");
        /* The step used a zero commanded velocity (no Unit update yet): the
         * newborn keeps the depenetrated position and no movement input. */
        check(bt_v2lensq(S.phys.bodies[slot].vel) == 0.0f,
              "newborn commanded velocity is still zero after its first step");
        /* bt_core_step bumps `ticks` at the end of the frame, so the frame the
         * newborn appeared in is ticks-1. */
        check(S.e_hold_tick[slot] == (unsigned short)(S.ticks - 1),
              "the appearance frame's Unit update is held out");

        float obs[BT_OBS_SIZE], reward = 0.0f, terminal = 0.0f;
        bt_core_step(&S, 0, obs, &reward, &terminal);
        check(fabsf(S.ex[slot] - 2036.507568f) < ATOL,
              "second snapshot matches the engine's second-step position");
    }

    /* ---- 2. free space: unchanged appearance and next-tick motion ------ */
    {
        bt_core_init(&S, 1ull, 0, 1, (unsigned short)BT_CHARACTER_BULL,
                     (unsigned short)BT_WEAPON_NONE);
        /* Far from every wall and from the player's arena-centre start, so
         * nothing but the unit's own movement can change the position. */
        const float bx = 1324.0f, by = 968.0f;
        const int slot = birth_and_activate(&S, kind, bx, by);
        check(slot >= 0, "free-space newborn is live on its appearance frame");
        if (slot < 0) return 1;

        check(S.ex[slot] == bx && S.ey[slot] == by,
              "free-space newborn keeps its birth position on its appearance frame");
        check(bt_v2lensq(S.phys.bodies[slot].vel) == 0.0f,
              "free-space newborn has no commanded velocity on that frame");

        float obs[BT_OBS_SIZE], reward = 0.0f, terminal = 0.0f;
        bt_core_step(&S, 0, obs, &reward, &terminal);
        check(S.ex[slot] == bx && S.ey[slot] == by,
              "free-space newborn still has not moved when its first Unit update runs");
        check(bt_v2lensq(S.phys.bodies[slot].vel) > 0.0f,
              "the first Unit update commands a velocity used by the NEXT step");

        bt_core_step(&S, 0, obs, &reward, &terminal);
        check(S.ex[slot] != bx || S.ey[slot] != by,
              "free-space newborn's first movement lands exactly one step later");
    }

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) printf("test_brotato_birth_step: OK\n");
    return g_failures ? 1 : 0;
}
