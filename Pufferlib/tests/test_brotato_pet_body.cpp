/* tests/test_brotato_pet_body.cpp
 *
 * Deterministic regression tests for the pet BODY contact slice
 * (ocean/brotato/brotato_pet_motion.h: "pet body contacts" + the dedicated
 * BtPhysWorld `phys_pet`).  Self-contained: includes only the core header and
 * <stdio.h>.
 *
 * Build and run:
 *     c++ -O2 -o build/test_brotato_pet_body tests/test_brotato_pet_body.cpp -lm
 *     ./build/test_brotato_pet_body
 *
 * Exit code 0 = all checks passed.
 *
 * Coverage (real behaviour, not table wiring):
 *  [1] a frame with no pet body performs no contact work at all;
 *  [2] pet-vs-wall: the wall stops the body circle at the engine's
 *      contact_max_allowed_penetration, and the zone-rect clamp alone no longer
 *      decides the edge;
 *  [3] pet-vs-pet: equal-mass separation with the same penetration budget;
 *  [4] the scene filter really is what decides the pairs (0/128 pairs with
 *      walls only, 0/0 with nothing, 512/640 with other 512/640 pets);
 *  [5] a hand-built reference BtPhysWorld reproduces the module's pet
 *      positions bit-for-bit (the module feeds the engine bodies, not an
 *      independent overlap separation);
 *  [6] an unobstructed pet advances exactly speed * dt;
 *  [7] a _can_move=false pet (mad CatlingGun, dead Scapegoat) is still a body;
 *  [8] a structure entity in the pet array is not a body and never pairs;
 *  [9] end to end through bt_core_step, a pet pressed against the arena wall
 *      never sinks into it.
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

/* BtState now carries two contact worlds (~3.4 MiB); keep every big object
 * static so nothing lands on the stack. */
static BtState A, B;
static BtPhysWorld ORACLE;

/* The effect-table row that instantiates a pet class. */
static int eff_row(int opcode) {
    for (int r = 0; r < BT_NITEM_EFF_ROWS; r++)
        if ((int)BT_ITEM_EFF_ROWS[r].opcode == opcode) return r;
    return -1;
}
/* The first effect row that spawns a STRUCTURE entity (no RigidBody2D scene). */
static int struct_eff_row(void) {
    for (int r = 0; r < BT_NITEM_EFF_ROWS; r++) {
        int op = (int)BT_ITEM_EFF_ROWS[r].opcode;
        if (op == BT_ITEMEFF_STRUCTURE || op == BT_ITEMEFF_TURRET) return r;
    }
    return -1;
}
/* The pet body's circle centre x (every shipped pet offset is on y). */
static float body_cx(const BtState* s, int p) {
    int g = bt_pet_scene_geom((int)s->pet_row[p]);
    return s->pet_x[p] + (g >= 0 ? BT_ITEM_SCENES[g].body_x : 0.0f);
}
static float body_cy(const BtState* s, int p) {
    int g = bt_pet_scene_geom((int)s->pet_row[p]);
    return s->pet_y[p] + (g >= 0 ? BT_ITEM_SCENES[g].body_y : 0.0f);
}
/* Circle-centre distance between two pet slots. */
static float pet_gap(const BtState* s, int a, int b) {
    float dx = body_cx(s, b) - body_cx(s, a);
    float dy = body_cy(s, b) - body_cy(s, a);
    return bt_sqrtf(dx * dx + dy * dy);
}
/* BtPhysWorld carries no step counter; the contact-cache state identifies
 * "same step / no contact work": the live-pair count, the free-pool depth and
 * the pair_map table (see brotato_physics.h BT_PHYS_PAIR_POOL). */
static int same_contact_state(const BtPhysWorld* a, const BtPhysWorld* b) {
    if (a->active_pair_count != b->active_pair_count) return 0;
    if (a->pool_free_n != b->pool_free_n) return 0;
    for (int i = 0; i < BT_PHYS_PAIR_CAPACITY; i++)
        if (a->pair_map[i] != b->pair_map[i]) return 0;
    return 1;
}
int main(void) {
    float obs[BT_OBS_SIZE], rew, term;

    printf("[1] a frame with no pet body runs no contact work\n");
    {
        bt_core_init(&A, 7u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        unsigned int apc = A.phys_pet.active_pair_count;
        unsigned short pfn = A.phys_pet.pool_free_n;
        A.px = 1000.0f; A.py = 1200.0f;
        for (int t = 0; t < 10; t++) bt_step_pet_motion(&A);
        check(A.phys_pet.body_count == BT_PET_BODY_BASE, "still walls-only after 10 frames");
        check(A.phys_pet.active_pair_count == apc && A.phys_pet.pool_free_n == pfn,
              "the contact pipeline is never entered without pet bodies");
        check(A.phys.body_count == BT_MAX + 4, "the enemy world is still untouched");
    }

    printf("[2] pet vs wall: the wall body stops the pet, not the zone clamp\n");
    {
        bt_core_init(&A, 11u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.px = 1000.0f; A.py = 1200.0f;             /* player far from the pet */
        int row = eff_row(BT_ITEMEFF_BONK_DOG);
        int p = bt_spawn_summon_row(&A, (unsigned short)row, 30.0f, 700.0f, 0, 0.0f, 0.0f, 0.0f);
        check(p == 0, "bonk_dog spawned");
        check(!A.phys_pet.bodies[BT_PET_BODY_BASE + 0].active, "no body before the first pet step");
        for (int t = 0; t < 120; t++) bt_step_pet_motion(&A);
        check(A.phys_pet.bodies[BT_PET_BODY_BASE + 0].active, "the live pet has a body");
        check(A.phys_pet.body_count == BT_PET_BODY_BASE + 1, "body_count covers the live pet");
        float pen = -body_cx(&A, 0) + 40.0f;        /* wall face at x = 0 */
        check(pen <= BT_PHYS_CONTACT_MAX_ALLOWED_PENETRATION + 0.01f, "the wall body keeps penetration <= 0.3");
        check(A.pet_x[0] >= 39.6f && A.pet_x[0] <= 40.01f,
              "the pet parks at the wall face (the zone-rect clamp alone would allow x down to 0)");
        check(A.pet_y[0] == 700.0f, "the contact normal is radial: no tangential drift");
        /* the same body built by hand resolves identically */
        bt_phys_world_init(&ORACLE, BT_DT, BT_PHYS_SOLVER_ITERATIONS);
        for (int i = 0; i < BT_PET_WALLS; i++) ORACLE.bodies[i] = A.phys.bodies[BT_MAX + i];
        ORACLE.bodies[BT_PET_BODY_BASE] =
            bt_phys_body_circle(bt_v2(30.0f, 700.0f), bt_v2(0.0f, -24.0f), 40.0f, 1.0f,
                                512u, 640u, BT_PHYS_MODE_CHARACTER);
        ORACLE.body_count = BT_PET_BODY_BASE + 1;
        for (int t = 0; t < 120; t++) {
            ORACLE.bodies[BT_PET_BODY_BASE].vel = bt_v2zero();
            bt_phys_step(&ORACLE);
        }
        check_near(ORACLE.bodies[BT_PET_BODY_BASE].pos.x, A.pet_x[0], 0.0f,
                   "an independent world reproduces the wall parking exactly");
    }

    printf("[3] pet vs pet: equal-mass separation, same penetration budget\n");
    {
        bt_core_init(&A, 13u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.px = 1000.0f; A.py = 1200.0f;
        int row = eff_row(BT_ITEMEFF_BONK_DOG);
        int p0 = bt_spawn_summon_row(&A, (unsigned short)row, 1000.0f, 700.0f, 0, 0.0f, 0.0f, 0.0f);
        int p1 = bt_spawn_summon_row(&A, (unsigned short)row, 1040.0f, 700.0f, 0, 0.0f, 0.0f, 0.0f);
        check(p0 == 0 && p1 == 1, "two base pets spawned");
        check_near(pet_gap(&A, 0, 1), 40.0f, 1e-3f, "both circles start 40 apart (overlap 40)");
        for (int t = 0; t < 120; t++) bt_step_pet_motion(&A);
        float gap = pet_gap(&A, 0, 1);
        check(gap >= 80.0f - BT_PHYS_CONTACT_MAX_ALLOWED_PENETRATION - 0.01f, "the overlap shrinks to the allowed penetration");
        check(gap <= 80.0f - BT_PHYS_CONTACT_MAX_ALLOWED_PENETRATION + 0.01f, "the solver never over-separates the pair");
        check_near(A.pet_x[0], 1020.0f - gap * 0.5f, 0.01f, "equal mass: pet 0 moves out symmetrically");
        check_near(A.pet_x[1], 1020.0f + gap * 0.5f, 0.01f, "equal mass: pet 1 moves out symmetrically");
        check(A.pet_y[0] == 700.0f && A.pet_y[1] == 700.0f,
              "identical circle offsets make the contact normal pure x");

        /* three in a line: the middle one is squeezed from both sides */
        bt_core_init(&A, 14u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.px = 1000.0f; A.py = 1200.0f;
        bt_spawn_summon_row(&A, (unsigned short)row, 1000.0f, 700.0f, 0, 0.0f, 0.0f, 0.0f);
        bt_spawn_summon_row(&A, (unsigned short)row, 1035.0f, 700.0f, 0, 0.0f, 0.0f, 0.0f);
        bt_spawn_summon_row(&A, (unsigned short)row, 1070.0f, 700.0f, 0, 0.0f, 0.0f, 0.0f);
        for (int t = 0; t < 180; t++) bt_step_pet_motion(&A);
        check(A.pet_x[0] < A.pet_x[1] && A.pet_x[1] < A.pet_x[2], "the line order is preserved");
        check(pet_gap(&A, 0, 1) >= 80.0f - BT_PHYS_CONTACT_MAX_ALLOWED_PENETRATION - 0.01f, "left pair separated");
        check(pet_gap(&A, 1, 2) >= 80.0f - BT_PHYS_CONTACT_MAX_ALLOWED_PENETRATION - 0.01f, "right pair separated");
        check(A.pet_x[1] > 1000.0f && A.pet_x[1] < 1070.0f, "the middle pet stayed between its neighbours");
    }

    printf("[4] the scene filter decides which pairs exist\n");
    {
        /* lootworm (0/128) + base pet (512/640) overlapping: no pair at all */
        bt_core_init(&A, 15u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.px = 1000.0f; A.py = 1200.0f;
        int lrow = eff_row(BT_ITEMEFF_LOOTWORM);
        int brow = eff_row(BT_ITEMEFF_BONK_DOG);
        bt_spawn_summon_row(&A, (unsigned short)lrow, 1000.0f, 700.0f, 0, 0.0f, 0.0f, 0.0f);
        bt_spawn_summon_row(&A, (unsigned short)brow, 1020.0f, 700.0f, 0, 0.0f, 0.0f, 0.0f);
        check_near(pet_gap(&A, 0, 1), 20.0f, 1e-3f, "lootworm and base pet overlap");
        for (int t = 0; t < 60; t++) bt_step_pet_motion(&A);
        check_near(pet_gap(&A, 0, 1), 20.0f, 1e-3f,
                   "a 0/128 pet and a 512/640 pet never pair (masks exclude each other)");

        /* two lootworms overlapping: walls only, so no pet-vs-pet pair */
        bt_core_init(&A, 16u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.px = 1000.0f; A.py = 1200.0f;
        bt_spawn_summon_row(&A, (unsigned short)lrow, 1000.0f, 700.0f, 0, 0.0f, 0.0f, 0.0f);
        bt_spawn_summon_row(&A, (unsigned short)lrow, 1010.0f, 700.0f, 0, 0.0f, 0.0f, 0.0f);
        for (int t = 0; t < 60; t++) bt_step_pet_motion(&A);
        check_near(pet_gap(&A, 0, 1), 10.0f, 1e-3f, "layer-0 pets do not collide with each other");

        /* reference world: the literal 0/0 filter cannot pair with the wall */
        bt_phys_world_init(&ORACLE, BT_DT, BT_PHYS_SOLVER_ITERATIONS);
        for (int i = 0; i < BT_PET_WALLS; i++) ORACLE.bodies[i] = A.phys.bodies[BT_MAX + i];
        ORACLE.bodies[BT_PET_BODY_BASE + 0] =
            bt_phys_body_circle(bt_v2(10.0f, 700.0f), bt_v2zero(), 40.0f, 1.0f,
                                0u, 0u, BT_PHYS_MODE_CHARACTER);              /* doc_moth */
        ORACLE.bodies[BT_PET_BODY_BASE + 1] =
            bt_phys_body_circle(bt_v2(10.0f, 900.0f), bt_v2(0.0f, -24.0f), 40.0f, 1.0f,
                                512u, 640u, BT_PHYS_MODE_CHARACTER);          /* base pet */
        ORACLE.body_count = BT_PET_BODY_BASE + 2;
        for (int t = 0; t < 90; t++) {
            ORACLE.bodies[BT_PET_BODY_BASE + 0].vel = bt_v2zero();
            ORACLE.bodies[BT_PET_BODY_BASE + 1].vel = bt_v2zero();
            bt_phys_step(&ORACLE);
        }
        check(ORACLE.bodies[BT_PET_BODY_BASE + 0].pos.x == 10.0f,
              "a 0/0 pet body is never touched by the wall");
        check(ORACLE.bodies[BT_PET_BODY_BASE + 1].pos.x >= 39.6f,
              "a 512/640 pet body at the same depth is pushed out");

        /* a 0/128 pet overlapping the wall IS pushed out */
        bt_core_init(&A, 17u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.px = 1000.0f; A.py = 1200.0f;
        bt_spawn_summon_row(&A, (unsigned short)lrow, 20.0f, 700.0f, 0, 0.0f, 0.0f, 0.0f);
        for (int t = 0; t < 90; t++) bt_step_pet_motion(&A);
        check(A.pet_x[0] >= 39.6f && A.pet_x[0] <= 40.01f,
              "mask 128 alone still puts the pets against the wall body");
    }

    printf("[5] the module feeds the engine bodies, not its own separation pass\n");
    {
        bt_core_init(&A, 19u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.px = 1000.0f; A.py = 1200.0f;
        int row = eff_row(BT_ITEMEFF_BONK_DOG);
        bt_spawn_summon_row(&A, (unsigned short)row, 1000.0f, 700.0f, 0, 0.0f, 0.0f, 0.0f);
        bt_spawn_summon_row(&A, (unsigned short)row, 1040.0f, 700.0f, 0, 0.0f, 0.0f, 0.0f);
        bt_phys_world_init(&ORACLE, BT_DT, BT_PHYS_SOLVER_ITERATIONS);
        for (int i = 0; i < BT_PET_WALLS; i++) ORACLE.bodies[i] = A.phys.bodies[BT_MAX + i];
        ORACLE.bodies[BT_PET_BODY_BASE + 0] =
            bt_phys_body_circle(bt_v2(1000.0f, 700.0f), bt_v2(0.0f, -24.0f), 40.0f, 1.0f,
                                512u, 640u, BT_PHYS_MODE_CHARACTER);
        ORACLE.bodies[BT_PET_BODY_BASE + 1] =
            bt_phys_body_circle(bt_v2(1040.0f, 700.0f), bt_v2(0.0f, -24.0f), 40.0f, 1.0f,
                                512u, 640u, BT_PHYS_MODE_CHARACTER);
        ORACLE.body_count = BT_PET_BODY_BASE + 2;
        check(same_contact_state(&A.phys_pet, &ORACLE),
              "both worlds start with the same contact-cache state");
        for (int t = 0; t < 200; t++) {
            bt_step_pet_motion(&A);
            ORACLE.bodies[BT_PET_BODY_BASE + 0].vel = bt_v2zero();
            ORACLE.bodies[BT_PET_BODY_BASE + 1].vel = bt_v2zero();
            bt_phys_step(&ORACLE);
            check_near(A.pet_x[0], ORACLE.bodies[BT_PET_BODY_BASE + 0].pos.x, 0.0f,
                       "pet 0 transform matches the reference world exactly");
            check_near(A.pet_x[1], ORACLE.bodies[BT_PET_BODY_BASE + 1].pos.x, 0.0f,
                       "pet 1 transform matches the reference world exactly");
            check(same_contact_state(&A.phys_pet, &ORACLE),
                  "the module keeps the reference world's contact-cache state");
        }
    }

    printf("[6] unobstructed pet motion is exactly the script velocity * dt\n");
    {
        bt_core_init(&A, 21u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.px = 200.0f; A.py = 200.0f;
        int row = eff_row(BT_ITEMEFF_LOOTWORM);
        int p = bt_spawn_summon_row(&A, (unsigned short)row, 1000.0f, 700.0f, 0, 0.0f, 0.0f, 0.0f);
        check(p == 0, "lootworm spawned");
        bt_spawn_material(&A, 1400.0f, 700.0f, 5);   /* well outside every pet zone */
        float x0 = A.pet_x[0], y0 = A.pet_y[0];
        bt_step_pet_motion(&A);
        check_near(A.pet_speed[0], 250.0f, 1e-3f, "pet speed = Stats.speed");
        check_near(A.pet_x[0] - x0, 250.0f * BT_DT, 1e-4f,
                   "one step advances exactly speed * dt toward the target");
        check(A.pet_y[0] == y0, "the target is due +x: no lateral motion");
        for (int t = 0; t < 3; t++) bt_step_pet_motion(&A);
        check_near(A.pet_x[0] - x0, 4.0f * 250.0f * BT_DT, 1e-3f,
                   "four steps advance four times speed * dt");
        check(A.matalive[0] && A.mat_by[0] == 0, "the gold is still untouched ahead of the pet");
    }

    printf("[7] a _can_move=false pet is still a body\n");
    {
        /* CatlingGun: a player inside its PlayerTriggerZone freezes it (mad mode) */
        bt_core_init(&A, 23u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.px = 30.0f; A.py = 700.0f;
        int row = eff_row(BT_ITEMEFF_CATLING_GUN);
        bt_spawn_summon_row(&A, (unsigned short)row, 30.0f, 700.0f, 0, 0.0f, 0.0f, 0.0f);
        for (int t = 0; t < 120; t++) bt_step_pet_motion(&A);
        check(A.pet_mad[0] == 1, "the player inside the zone puts the CatlingGun in mad mode");
        check(A.pet_x[0] >= 39.6f && A.pet_x[0] <= 40.01f,
              "the frozen (velocity 0) pet body is still pushed out of the wall");

        /* Scapegoat: die() clears _can_move but the scene node keeps its body */
        bt_core_init(&A, 24u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.px = 1000.0f; A.py = 1200.0f;
        int srow = eff_row(BT_ITEMEFF_SCAPEGOAT);
        bt_spawn_summon_row(&A, (unsigned short)srow, 30.0f, 700.0f, 0, 0.0f, 0.0f, 0.0f);
        bt_step_pet_motion(&A);                     /* initialises the slot */
        A.pet_dead[0] = 1;
        A.pet_hp[0] = 0.0f;
        for (int t = 0; t < 120; t++) bt_step_pet_motion(&A);
        check(A.pet_dead[0] == 1, "no player in the healing zone: the Scapegoat stays dead");
        check(A.pet_x[0] >= 39.6f && A.pet_x[0] <= 40.01f,
              "a dead pet body is still resolved against the wall");
    }

    printf("[8] a structure in the pet array is not a body\n");
    {
        bt_core_init(&A, 25u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.px = 1000.0f; A.py = 1200.0f;
        int srow = struct_eff_row();
        check(srow >= 0, "a structure effect row exists");
        int ps = bt_spawn_summon_row(&A, (unsigned short)srow, 1000.0f, 700.0f, 1, 0.0f, 0.0f, 0.0f);
        int brow = eff_row(BT_ITEMEFF_BONK_DOG);
        int pb = bt_spawn_summon_row(&A, (unsigned short)brow, 1020.0f, 700.0f, 0, 0.0f, 0.0f, 0.0f);
        check(ps == 0 && pb == 1, "structure and pet occupy distinct slots");
        check(A.pet_isstruct[0] == 1 && A.pet_isstruct[1] == 0, "slots keep their entity class");
        for (int t = 0; t < 120; t++) bt_step_pet_motion(&A);
        check(!A.phys_pet.bodies[BT_PET_BODY_BASE + 0].active,
              "the structure slot never gets a body");
        check(A.phys_pet.bodies[BT_PET_BODY_BASE + 1].active, "the pet slot does");
        check_near(A.pet_x[1], 1020.0f, 1e-4f,
                   "a 512/640 pet overlapping a structure is not pushed by it (layer 2048)");
        check_near(A.pet_x[0], 1000.0f, 1e-4f, "the structure itself never moves");
    }

    printf("[9] end to end: a pet pressing on the wall never sinks into it\n");
    {
        /* A target the pet can never consume keeps it driving into the wall for
         * the whole run: the BotOMine follows the player and stops
         * distance_to_target (100) away, so a player standing 200 px beyond the
         * left wall holds it pressed there.  (A gold inside the wall would be
         * attracted out through the 100 px ItemAttractArea and picked up, and a
         * tree would be killed by its own melee, either way ending the press.) */
        bt_core_init(&A, 27u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        A.px = -200.0f; A.py = 700.0f;
        int row = eff_row(BT_ITEMEFF_BOT_O_MINE);
        int p = bt_spawn_summon_row(&A, (unsigned short)row, 600.0f, 700.0f, 0, 0.0f, 0.0f, 0.0f);
        check(p == 0, "bot_o_mine spawned");
        float minx = 1e9f, worst_pen = -1e9f;
        int finite = 1;
        for (int t = 0; t < 300; t++) {
            bt_core_step(&A, 0, obs, &rew, &term);
            if (!(A.pet_x[0] == A.pet_x[0]) || !(A.pet_y[0] == A.pet_y[0])) finite = 0;
            if (A.pet_x[0] < minx) minx = A.pet_x[0];
            if (t >= 240) {
                float pen = 40.0f - A.pet_x[0];
                if (pen > worst_pen) worst_pen = pen;
            }
        }
        check(finite, "pet x stays finite through 300 core steps");
        check(minx < 200.0f, "the pet actually reached the wall region");
        check(worst_pen <= BT_PHYS_CONTACT_MAX_ALLOWED_PENETRATION + 0.01f,
              "settled penetration never exceeds the engine's allowed 0.3");
        check(A.pet_x[0] >= 39.6f && A.pet_x[0] <= 40.01f,
              "the pet is held at the wall face by the body contact");
        check(A.phys_pet.body_count == BT_PET_BODY_BASE + 1, "the pet world tracks the live pet");

        /* determinism: the same run from two fresh states is identical */
        bt_core_init(&B, 27u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        B.n_live_groups = 0;
        B.px = -200.0f; B.py = 700.0f;
        bt_spawn_summon_row(&B, (unsigned short)row, 600.0f, 700.0f, 0, 0.0f, 0.0f, 0.0f);
        for (int t = 0; t < 120; t++) bt_core_step(&B, 0, obs, &rew, &term);
        bt_core_init(&A, 27u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        A.px = -200.0f; A.py = 700.0f;
        bt_spawn_summon_row(&A, (unsigned short)row, 600.0f, 700.0f, 0, 0.0f, 0.0f, 0.0f);
        for (int t = 0; t < 120; t++) bt_core_step(&A, 0, obs, &rew, &term);
        check(A.pet_x[0] == B.pet_x[0] && A.pet_y[0] == B.pet_y[0],
              "pet body contacts are deterministic across identical runs");
        check(same_contact_state(&A.phys_pet, &B.phys_pet),
              "both worlds have the same contact-cache state");
    }

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
