/* tests/test_brotato_contact_solver.cpp
 *
 * Deterministic regression tests for ocean/brotato/brotato_physics.h -- the
 * transcribed GodotPhysics2D body-contact solver.
 *
 * Self-contained: includes only the header under test and <stdio.h>.  No Env,
 * no raylib, no engine, no allocation.  Build and run with:
 *
 *     cc -O2 -o build/test_contact tests/test_brotato_contact_solver.cpp -lm
 *     ./build/test_contact
 *
 * Exit code 0 = all checks passed.
 *
 * Expected values are derived analytically from the transcribed equations
 * (biases, mass_normal, warm-start impulses) and, where noted, cross-checked
 * against the real-engine fixture trace (tests/brotato_physics_fixture.gd).
 */

#include "../ocean/brotato/brotato_physics.h"

#include <stdio.h>

static int g_failures = 0;
static int g_checks = 0;

static void check(int cond, const char *msg) {
    g_checks++;
    if (!cond) {
        g_failures++;
        printf("  FAIL: %s\n", msg);
    }
}

static void check_near(float got, float want, float tol, const char *msg) {
    g_checks++;
    if (!(bt_absf(got - want) <= tol)) {
        g_failures++;
        printf("  FAIL: %s (got %.6f want %.6f tol %g)\n", msg, got, want, tol);
    }
}

static float fstep(void) { return 1.0f / 60.0f; }

static int pair_live(const BtPhysWorld* w, int a, int b) {
    return w->pair_map[bt_phys_pair_index(a, b)] != BT_PHYS_PAIR_FREE;
}

/* Enemy body: layer 4, mask 132, MODE_CHARACTER (enemy.tscn:44-47). */
static void enemy(BtPhysicsBody *b, float x, float y, float r, float mass) {
    *b = bt_phys_body_circle(bt_v2(x, y), bt_v2zero(), r, mass, 4u, 132u, BT_PHYS_MODE_CHARACTER);
}

/* Wall body: layer 128, mask 0, STATIC. */
static void wall(BtPhysicsBody *b, float cx, float cy, float hx, float hy) {
    *b = bt_phys_body_rect(bt_v2(cx, cy), hx, hy, 128u, 0u);
}

/* ------------------------------------------------------------------ */
/* 1. equal masses, initial overlap, no velocity                       */
/*    Fixture cross-check: bodies r=20 at x=300/330 settle to          */
/*    295.150024 / 334.849976; after one solver step                  */
/*    298.545013 / 331.454987.  This checks the bias channel.          */
/* ------------------------------------------------------------------ */
static void test_equal_mass_overlap(void) {
    BtPhysWorld w;
    BtPhysWorld *p = &w;
    float mx, d;
    printf("[1] equal masses, 10-unit overlap, bias push\n");
    bt_phys_world_init(p, fstep(), BT_PHYS_SOLVER_ITERATIONS);
    enemy(&p->bodies[0], 300.0f, 0.0f, 20.0f, 1.0f);
    enemy(&p->bodies[1], 330.0f, 0.0f, 20.0f, 1.0f);
    p->body_count = 2;

    bt_phys_step(p);
    /* normal removed (both already at rest), bias pushes apart by
     * 0.3 * 60 * (depth - 0.3) / 2 / 60 per body along +/-x */
    check_near(p->bodies[0].pos.x, 298.545f, 0.01f, "A moved -1.455");
    check_near(p->bodies[1].pos.x, 331.455f, 0.01f, "B moved +1.455");
    check_near(p->bodies[0].vel.x, 0.0f, 1e-4f, "A normal velocity zeroed");
    check_near(p->bodies[1].vel.x, 0.0f, 1e-4f, "B normal velocity zeroed");
    d = w.bodies[1].pos.x - w.bodies[0].pos.x;
    check(d > 30.0f, "separation grew");
    check(pair_live(p, 0, 1), "pair created");
    check(bt_phys_pair(p, 0, 1)->contact_count == 1, "one contact cached");
    check(bt_phys_pair(p, 0, 1)->collided == 1, "pair collided");

    /* settle: allowed penetration 0.3 => separation 39.7 */
    for (mx = 0; mx < 300; mx++) bt_phys_step(p);
    check_near(w.bodies[1].pos.x - w.bodies[0].pos.x, 39.7f, 0.02f, "settles at depth 0.3");
    printf("   settled separation = %.6f (fixture 39.699952)\n",
           (double)(w.bodies[1].pos.x - w.bodies[0].pos.x));
}

/* ------------------------------------------------------------------ */
/* 2. head-on response, equal masses                                   */
/* ------------------------------------------------------------------ */
static void test_head_on_equal(void) {
    BtPhysWorld w;
    BtPhysWorld *p = &w;
    printf("[2] head-on, equal masses 1 vs 1\n");
    bt_phys_world_init(p, fstep(), BT_PHYS_SOLVER_ITERATIONS);
    enemy(&p->bodies[0], 0.0f, 0.0f, 10.0f, 1.0f);
    enemy(&p->bodies[1], 19.0f, 0.0f, 10.0f, 1.0f);
    p->bodies[0].vel = bt_v2(10.0f, 0.0f);
    p->bodies[1].vel = bt_v2(-10.0f, 0.0f);
    p->body_count = 2;

    bt_phys_step(p);
    /* inelastic: both end with the common relative-normal velocity removed */
    check_near(p->bodies[0].vel.x, 0.0f, 1e-4f, "A stopped");
    check_near(p->bodies[1].vel.x, 0.0f, 1e-4f, "B stopped");
    check(p->bodies[1].pos.x - p->bodies[0].pos.x > 19.0f, "bias separated them");
    printf("   vA.x=%.6f vB.x=%.6f, sep=%.6f\n",
           (double)p->bodies[0].vel.x, (double)p->bodies[1].vel.x,
           (double)(p->bodies[1].pos.x - p->bodies[0].pos.x));
}

/* ------------------------------------------------------------------ */
/* 3. different masses, head-on                                        */
/* ------------------------------------------------------------------ */
static void test_head_on_unequal(void) {
    BtPhysWorld w;
    BtPhysWorld *p = &w;
    const float want = (10.0f * 1.0f + (-10.0f) * 100.0f) / 101.0f; /* -9.8019802 */
    printf("[3] head-on, masses 1 vs 100\n");
    bt_phys_world_init(p, fstep(), BT_PHYS_SOLVER_ITERATIONS);
    enemy(&p->bodies[0], 0.0f, 0.0f, 10.0f, 1.0f);
    enemy(&p->bodies[1], 19.0f, 0.0f, 10.0f, 100.0f);
    p->bodies[0].vel = bt_v2(10.0f, 0.0f);
    p->bodies[1].vel = bt_v2(-10.0f, 0.0f);
    p->body_count = 2;

    bt_phys_step(p);
    check_near(p->bodies[0].vel.x, want, 1e-3f, "light body -> common velocity");
    check_near(p->bodies[1].vel.x, want, 1e-3f, "heavy body -> common velocity");
    printf("   common velocity = %.6f (analytic %.6f)\n",
           (double)p->bodies[0].vel.x, (double)want);
}

/* ------------------------------------------------------------------ */
/* 4. zero-distance centres (degenerate SAT axis)                      */
/* ------------------------------------------------------------------ */
static void test_coincident(void) {
    BtPhysWorld w;
    BtPhysWorld *p = &w;
    printf("[4] coincident centres\n");
    bt_phys_world_init(p, fstep(), BT_PHYS_SOLVER_ITERATIONS);
    enemy(&p->bodies[0], 0.0f, 0.0f, 10.0f, 1.0f);
    enemy(&p->bodies[1], 0.0f, 0.0f, 10.0f, 1.0f);
    p->body_count = 2;

    bt_phys_step(p);
    /* the zero axis falls back to (0,1); the pair separates purely on +/-y */
    check(bt_absf(p->bodies[0].pos.x) < 1e-5f, "A stays on x axis");
    check(bt_absf(p->bodies[1].pos.x) < 1e-5f, "B stays on x axis");
    check(p->bodies[0].pos.y * p->bodies[1].pos.y < 0.0f, "opposite y directions");
    check(p->bodies[1].pos.y - p->bodies[0].pos.y > 5.0f, "bias separates by depth 20");
    printf("   dy = %.6f (fixture: one body +y, the other -y)\n",
           (double)(p->bodies[1].pos.y - p->bodies[0].pos.y));
}

/* ------------------------------------------------------------------ */
/* 5. static axis-aligned wall                                         */
/* ------------------------------------------------------------------ */
static void test_static_wall(void) {
    BtPhysWorld w;
    BtPhysWorld *p = &w;
    printf("[5] static wall, circle falling into it\n");
    bt_phys_world_init(p, fstep(), BT_PHYS_SOLVER_ITERATIONS);
    wall(&p->bodies[0], 0.0f, 0.0f, 100.0f, 10.0f);
    enemy(&p->bodies[1], 0.0f, 15.0f, 10.0f, 1.0f);
    p->bodies[1].vel = bt_v2(0.0f, -10.0f);
    p->body_count = 2;

    bt_phys_step(p);
    check_near(p->bodies[0].pos.y, 0.0f, 1e-6f, "wall did not move");
    check_near(p->bodies[1].vel.y, 0.0f, 1e-4f, "circle normal velocity removed");
    check(p->bodies[1].pos.y > 15.0f, "circle pushed out of the wall");
    printf("   circle y=%.6f vy=%.6f\n",
           (double)p->bodies[1].pos.y, (double)p->bodies[1].vel.y);
}

/* ------------------------------------------------------------------ */
/* 6. player/enemy bodies never collide                                */
/* ------------------------------------------------------------------ */
static void test_no_player_enemy(void) {
    BtPhysWorld w;
    BtPhysWorld *p = &w;
    BtPhysPair *pr;
    printf("[6] player (kinematic, layer 2 mask 130) vs enemy (layer 4 mask 132)\n");
    bt_phys_world_init(p, fstep(), BT_PHYS_SOLVER_ITERATIONS);
    /* player body registered in the world for the pair test */
    p->bodies[0] = bt_phys_body_circle(bt_v2(0.0f, 0.0f), bt_v2(0.0f, -12.0f),
                                       24.0f, 1.0f, 2u, 130u, BT_PHYS_MODE_KINEMATIC);
    enemy(&p->bodies[1], 5.0f, 0.0f, 20.0f, 1.0f);
    p->body_count = 2;

    bt_phys_update_pairs(p);
    pr = bt_phys_pair(p, 0, 1);
    check(bt_phys_pair_solves(&p->bodies[0], &p->bodies[1]) == 0, "mask rejects the pair");
    check(!pair_live(p, 0, 1), "no pair object created");
    check(bt_phys_pair_setup(p, pr) == 0, "setup refuses");
    check(p->active_pair_count == 0, "broadphase reports no live pairs");
}

/* ------------------------------------------------------------------ */
/* 7. contact persistence and reset on broadphase unpair               */
/* ------------------------------------------------------------------ */
static void test_persistence_reset(void) {
    BtPhysWorld w;
    BtPhysWorld *p = &w;
    BtPhysPair *pr;
    printf("[7] contact cache persistence + reset\n");
    bt_phys_world_init(p, fstep(), BT_PHYS_SOLVER_ITERATIONS);
    enemy(&p->bodies[0], 300.0f, 0.0f, 20.0f, 1.0f);
    enemy(&p->bodies[1], 330.0f, 0.0f, 20.0f, 1.0f);
    p->body_count = 2;

    bt_phys_step(p);
    pr = bt_phys_pair(p, 0, 1);
    check(pair_live(p, 0, 1), "pair live while overlapping");
    check(pr->contact_count == 1, "contact cached");
    check(pr->contacts[0].acc_normal_impulse >= 0.0f, "normal impulse non-negative");
    check(pr->contacts[0].acc_bias_impulse > 0.0f, "bias impulse accumulated");

    /* bodies separate -> broadphase unpair -> cache dropped */
    p->bodies[0].pos = bt_v2(100.0f, 0.0f);
    p->bodies[1].pos = bt_v2(900.0f, 0.0f);
    bt_phys_update_pairs(p);
    check(!pair_live(p, 0, 1), "pair retired when AABBs part");
    check(pr->contact_count == 0, "cache dropped with the pair");

    /* re-overlap -> a fresh pair starts from zero accumulated impulses */
    p->bodies[0].pos = bt_v2(300.0f, 0.0f);
    p->bodies[1].pos = bt_v2(330.0f, 0.0f);
    bt_phys_update_pairs(p);
    check(pair_live(p, 0, 1), "pair recreated on re-overlap");
    bt_phys_setup_pairs(p);
    check(pr->contact_count == 1, "contact re-detected");
    check(pr->contacts[0].acc_normal_impulse == 0.0f, "normal impulse reset");
    check(pr->contacts[0].acc_bias_impulse == 0.0f, "bias impulse reset");
}

/* ------------------------------------------------------------------ */
/* 8. warm start survives across steps while the pair stays live       */
/* ------------------------------------------------------------------ */
static void test_warm_start(void) {
    BtPhysWorld w;
    BtPhysWorld *p = &w;
    float first;
    printf("[8] warm start across frames\n");
    bt_phys_world_init(p, fstep(), BT_PHYS_SOLVER_ITERATIONS);
    enemy(&p->bodies[0], 300.0f, 0.0f, 20.0f, 1.0f);
    enemy(&p->bodies[1], 330.0f, 0.0f, 20.0f, 1.0f);
    p->body_count = 2;

    bt_phys_step(p);
    first = bt_phys_pair(p, 0, 1)->contacts[0].acc_bias_impulse;
    check(first > 0.0f, "first step accumulates bias impulse");
    bt_phys_step(p);
    check(pair_live(p, 0, 1), "pair still live");
    check(bt_phys_pair(p, 0, 1)->contacts[0].reused == 1, "contact recycled");
    printf("   first bias impulse = %.6f\n", (double)first);
}

/* ------------------------------------------------------------------ */
/* 9. kinematic test_motion against a wall                             */
/*    Player circle r=24, centre offset (0,-12), mask 130, KINEMATIC.  */
/*    Placed 24 units inside the left wall face (x=0); step 1 recovery */
/*    must push it out along +x with recovery factor 0.4, and the rest */
/*    query must report the remaining penetration and a +x normal.     */
/* ------------------------------------------------------------------ */
static void test_kinematic_wall(void) {
    BtPhysWorld w;
    BtPhysicsBody player;
    BtPhysMotionResult res;
    int collided;
    printf("[9] kinematic test_motion vs static wall\n");
    bt_phys_world_init(&w, fstep(), BT_PHYS_SOLVER_ITERATIONS);
    /* left wall: inner face at x = 0, body spans x in [-256, 0] */
    wall(&w.bodies[0], -128.0f, 768.0f, 128.0f, 768.0f);
    w.body_count = 1;

    player = bt_phys_body_circle(bt_v2(0.0f, 400.0f), bt_v2(0.0f, -12.0f),
                                 24.0f, 1.0f, 2u, 130u, BT_PHYS_MODE_KINEMATIC);

    collided = bt_phys_test_body_motion(&w, &player, bt_v2zero(), 0.08f, &res);
    check(collided == 1, "wall collision reported");
    check(res.motion.x > 0.0f, "recovery pushes the player out (+x)");
    check(res.motion.x < 24.0f, "recovery does not overshoot the wall face");
    check(res.collision_normal.x > 0.5f, "collision normal points away from the wall");
    check(res.collider == 0, "collider is the wall body");
    check(res.collision_depth > 0.0f, "remaining penetration reported");
    check(res.collision_safe_fraction >= 0.0f && res.collision_safe_fraction <= 1.0f,
          "safe fraction in range");
    printf("   motion=(%.6f, %.6f) normal=(%.6f, %.6f) depth=%.6f collider=%d\n",
           (double)res.motion.x, (double)res.motion.y,
           (double)res.collision_normal.x, (double)res.collision_normal.y,
           (double)res.collision_depth, res.collider);

    /* no wall nearby -> no collision and the full motion is returned */
    {
        BtPhysWorld w2;
        BtPhysMotionResult r2;
        int c2;
        bt_phys_world_init(&w2, fstep(), BT_PHYS_SOLVER_ITERATIONS);
        c2 = bt_phys_test_body_motion(&w2, &player, bt_v2(-2.0f, 1.0f), 0.08f, &r2);
        check(c2 == 0, "empty world -> no collision");
        check_near(r2.motion.x, -2.0f, 1e-6f, "full motion returned");
        check_near(r2.motion.y, 1.0f, 1e-6f, "full motion returned (y)");
    }
}

/* ------------------------------------------------------------------ */
/* 11. inactive bodies retire their pairs (no ghost solving)           */
/* ------------------------------------------------------------------ */
static void test_inactive_retire(void) {
    BtPhysWorld w;
    BtPhysWorld *p = &w;
    BtPhysPair *pr;
    float x0, x1;
    printf("[11] inactive bodies retire their pairs\n");
    bt_phys_world_init(p, fstep(), BT_PHYS_SOLVER_ITERATIONS);
    enemy(&p->bodies[0], 300.0f, 0.0f, 20.0f, 1.0f);
    enemy(&p->bodies[1], 330.0f, 0.0f, 20.0f, 1.0f);
    p->body_count = 2;

    bt_phys_step(p);
    pr = bt_phys_pair(p, 0, 1);
    check(pair_live(p, 0, 1), "live while both active");
    check(p->active_pair_count == 1, "one live pair");

    /* both bodies dead: death disables the shape, so the pair must retire */
    p->bodies[0].active = 0;
    p->bodies[1].active = 0;
    bt_phys_update_pairs(p);
    check(!pair_live(p, 0, 1), "pair retired when bodies go inactive");
    check(pr->contact_count == 0, "cache dropped with the pair");
    check(p->active_pair_count == 0, "no live pairs");

    /* a whole step must neither re-list, re-impulse nor move them */
    x0 = p->bodies[0].pos.x;
    x1 = p->bodies[1].pos.x;
    bt_phys_step(p);
    check(!pair_live(p, 0, 1), "stays retired through a step");
    check(p->active_pair_count == 0, "no live pairs after the step");
    check(p->bodies[0].pos.x == x0 && p->bodies[1].pos.x == x1, "no ghost motion");

    /* one dead body is enough to retire the pair */
    p->bodies[0].active = 1;
    bt_phys_update_pairs(p);
    check(p->active_pair_count == 0, "still retired with B inactive");
}

/* ------------------------------------------------------------------ */
/* 12. body_count shrink retires out-of-range pairs                    */
/* ------------------------------------------------------------------ */
static void test_body_count_shrink(void) {
    BtPhysWorld w;
    BtPhysWorld *p = &w;
    printf("[12] body_count shrink retires pairs\n");
    bt_phys_world_init(p, fstep(), BT_PHYS_SOLVER_ITERATIONS);
    enemy(&p->bodies[0], 0.0f, 0.0f, 20.0f, 1.0f);
    enemy(&p->bodies[1], 10.0f, 0.0f, 20.0f, 1.0f);
    enemy(&p->bodies[2], 20.0f, 0.0f, 20.0f, 1.0f);
    p->body_count = 3;

    bt_phys_step(p);
    check(p->active_pair_count == 3, "three overlapping bodies -> three pairs");
    p->body_count = 2;
    bt_phys_update_pairs(p);
    check(p->active_pair_count == 1, "only the (0,1) pair survives the shrink");
    check(!pair_live(p, 0, 2), "pair with the dropped index retired");
    check(!pair_live(p, 1, 2), "pair with the dropped index retired (1,2)");
    p->body_count = 0;
    bt_phys_update_pairs(p);
    check(p->active_pair_count == 0, "empty world has no live pairs");
}

/* ------------------------------------------------------------------ */
/* 13. recycled indices start with an empty contact cache              */
/* ------------------------------------------------------------------ */
static void test_recycled_index(void) {
    BtPhysWorld w;
    BtPhysWorld *p = &w;
    BtPhysPair *pr;
    printf("[13] recycled body index has no ghost impulse\n");
    bt_phys_world_init(p, fstep(), BT_PHYS_SOLVER_ITERATIONS);
    enemy(&p->bodies[0], 300.0f, 0.0f, 20.0f, 1.0f);
    enemy(&p->bodies[1], 330.0f, 0.0f, 20.0f, 1.0f);
    p->body_count = 2;

    bt_phys_step(p);
    pr = bt_phys_pair(p, 0, 1);
    check(pr->contacts[0].acc_bias_impulse > 0.0f, "live pair has accumulated impulse");

    /* index 0 dies, its pair retires, then the slot is handed to a new body */
    p->bodies[0].active = 0;
    bt_phys_update_pairs(p);
    check(!pair_live(p, 0, 1), "pair retired while 0 is dead");
    p->bodies[0] = bt_phys_body_circle(bt_v2(300.0f, 0.0f), bt_v2zero(),
                                       20.0f, 1.0f, 4u, 132u, BT_PHYS_MODE_CHARACTER);
    bt_phys_update_pairs(p);
    check(pair_live(p, 0, 1), "pair recreated for the new body");
    bt_phys_setup_pairs(p);
    check(pr->contact_count == 1, "contact re-detected");
    check(pr->contacts[0].acc_bias_impulse == 0.0f, "bias impulse reset (no ghost)");
    check(pr->contacts[0].acc_normal_impulse == 0.0f, "normal impulse reset (no ghost)");
}

/* ------------------------------------------------------------------ */
/* 10. memory accounting                                               */
/* ------------------------------------------------------------------ */
static void test_memory(void) {
    long long bytes;
    printf("[10] memory\n");
    printf("   sizeof(BtPhysicsBody) = %d\n", bt_phys_sizeof_body());
    printf("   sizeof(BtPhysPair)    = %d\n", bt_phys_sizeof_pair());
    printf("   pair capacity         = %d (C(%d,2))\n",
           bt_phys_pair_capacity(), BT_PHYS_MAX_BODIES);
    bytes = (long long)sizeof(BtPhysWorld);
    printf("   total world storage   = %lld bytes (%.3f MiB)\n",
           bytes, (double)bytes / (1024.0 * 1024.0));
    check(bt_phys_sizeof_pair() > 0 && bt_phys_sizeof_pair() < 512, "pair size sane");
    check(bt_phys_pair_capacity() == (BT_PHYS_MAX_BODIES * (BT_PHYS_MAX_BODIES - 1)) / 2,
          "all-pairs capacity");
}

static void test_endpoint_capacity(void) {
    static BtPhysWorld w;
    bt_phys_world_init(&w, fstep(), BT_PHYS_SOLVER_ITERATIONS);
    const int a = BT_PHYS_MAX_BODIES - 2, b = BT_PHYS_MAX_BODIES - 1;
    for (int i = 0; i < BT_PHYS_MAX_BODIES; i++) w.bodies[i].active = 0;
    enemy(&w.bodies[a], 300.0f, 0.0f, 20.0f, 1.0f);
    enemy(&w.bodies[b], 330.0f, 0.0f, 20.0f, 1.0f);
    w.body_count = BT_PHYS_MAX_BODIES;
    bt_phys_step(&w);
    BtPhysPair* p = bt_phys_pair(&w, a, b);
    check(pair_live(&w, a, b), "highest source body indices retain a live pair");
    check(p->a == a && p->b == b, "pair endpoint narrowing preserves full capacity");
    check(p->contact_count == 1 && !w.pair_overflow, "highest indices solve without loss");
}

/* ------------------------------------------------------------------ */
/* 16. the contact cache is kept in the pair's A-LOCAL frame            */
/*     BodyPair2DSW::setup computes `offset_B = B.origin - A.origin` and */
/*     then the SAT, the cached contact points and `depth` all in that   */
/*     frame, so the ball centres are ~tens of px apart, not hundreds.   */
/*     The contact normal is a near-cancelling difference of two points  */
/*     that sit at the combined radius, so doing the same algebra in     */
/*     world coordinates rounds it differently in the last bits, and a   */
/*     pair that stays in contact for tens of frames turns that into     */
/*     visible drift (match41: first obs divergence at tick 455).        */
/*     Fixture: match41 ticks 380-383, enemies @Enemy@732 / @Enemy@733   */
/*     (baby_alien, r=22, body offset (0,-3)) whose centres reach the    */
/*     44 px combined radius on step 3.  The expected positions are the  */
/*     real game's own enemy_diag values for those ticks.                */
static void test_pair_frame_local(void) {
    static BtPhysWorld w;
    static const float vx[3] = { -128.533966f, -127.261948f, -126.001907f };
    static const float vy[3] = { 191.937546f, 192.783279f, 193.609192f };
    static const float wx[3] = { -162.073730f, -160.597595f, -159.131180f };
    static const float wy[3] = { 219.684097f, 220.765488f, 221.824860f };
    int k;
    printf("[16] contact frame is the pair's A-local frame (match41 380-383)\n");
    bt_phys_world_init(&w, fstep(), BT_PHYS_SOLVER_ITERATIONS);
    w.body_count = 2;
    w.bodies[0] = bt_phys_body_circle(bt_v2(433.072327f, 700.321838f), bt_v2(0.0f, -3.0f),
                                      22.0f, 1.0f, 4u, 132u, BT_PHYS_MODE_CHARACTER);
    w.bodies[1] = bt_phys_body_circle(bt_v2(476.725708f, 689.361938f), bt_v2(0.0f, -3.0f),
                                      22.0f, 1.0f, 4u, 132u, BT_PHYS_MODE_CHARACTER);
    for (k = 0; k < 3; k++) {
        w.bodies[0].vel = bt_v2(vx[k], vy[k]);
        w.bodies[1].vel = bt_v2(wx[k], wy[k]);
        bt_phys_step(&w);
    }
    check(bt_phys_pair(&w, 0, 1)->contact_count == 1, "the pair is in contact on step 3");
    check(w.bodies[0].pos.x == 426.394958f && w.bodies[0].pos.y == 710.034790f,
          "A integrates to the game's tick-383 position");
    check(w.bodies[1].pos.x == 469.009705f && w.bodies[1].pos.y == 700.325806f,
          "B integrates to the game's tick-383 position (world-frame arithmetic misses it)");
}

int main(void) {
    printf("brotato contact solver regression\n");
    test_equal_mass_overlap();
    test_head_on_equal();
    test_head_on_unequal();
    test_coincident();
    test_static_wall();
    test_no_player_enemy();
    test_persistence_reset();
    test_warm_start();
    test_kinematic_wall();
    test_memory();
    test_inactive_retire();
    test_body_count_shrink();
    test_recycled_index();
    test_endpoint_capacity();
    test_pair_frame_local();
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
