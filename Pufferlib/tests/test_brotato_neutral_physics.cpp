/* tests/test_brotato_neutral_physics.cpp
 *
 * Deterministic behavioural regression tests for neutral (tree) physics
 * geometry: ocean/brotato/brotato_geometry.h (BT_NEUTRAL_PHYSICS) and the
 * neutral domain separation between BT_NEUTRALS and BT_KINDS.
 * Self-contained: includes only the core header and <stdio.h>.
 *
 * Build and run:
 *     c++ -O2 -o build/test_brotato_neutral_physics tests/test_brotato_neutral_physics.cpp -lm
 *     ./build/test_brotato_neutral_physics
 *
 * Exit code 0 = all checks passed.
 *
 * Source provenance (recovered Brotato project):
 *   entities/units/entity.tscn  root RigidBody2D mode 2, Collision (0,-24) r=40
 *   entities/units/unit/unit.tscn            Hurtbox position (0,-24)
 *   overlap/hurtbox.tscn                     Hurtbox/Collision r=32
 *   entities/units/neutral/neutral.tscn      Collision position (0,-16);
 *                                            Hurtbox/Collision (0,-14) r=48.6621
 *   entities/units/neutral/tree.tscn         Collision shape r=24, stats=tree_stats.tres
 *   dlcs/dlc_1/neutrals/tree/tree.tscn       same body r=24, its own tree_stats.tres
 *   tree_stats.tres                          knockback_resistance = 1.0
 * so every tree resolves to: body r=24 @(0,-16), hurt r=48.6621 @(0,-38),
 * no contact Hitbox, mass 1, MODE_CHARACTER.  The centres are root-local sums
 * of the node chain (Hurtbox (0,-24) + Hurtbox/Collision (0,-14) = (0,-38)).
 *
 * Sections [3]-[5] exercise the live core and require the neutral-aware
 * geometry selection (see the header comment of extract_physics.py and the
 * accessor note in the slice report): a neutral kind is a BT_NEUTRALS index,
 * so collision consumers must read BT_NEUTRAL_PHYSICS, never
 * BT_PHYSICS[bt_geom_index(neutral_kind)].  Until that change lands the spawn,
 * contact and projectile overlaps below resolve the enemy BT_PHYSICS[0] row and
 * the assertions fail, which is the point of the test.
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

/* BtState is ~1 MiB; keep it static. */
static BtState A;

static int str_has(const char* hay, const char* needle) {
    if (!hay) return 0;
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j] && hay[i + j] == needle[j]) j++;
        if (!needle[j]) return 1;
    }
    return 0;
}

/* BT_NEUTRALS index whose name contains `needle` (the two zone trees). */
static int neutral_kind(const char* needle) {
    for (int i = 0; i < BT_NNEUTRALS; i++)
        if (str_has(BT_NEUTRAL_NAME[i], needle)) return i;
    return -1;
}

/* Locate the live neutral slot holding kind `nk`. */
static int neutral_slot(BtState* s, int nk) {
    for (int i = 0; i < BT_MAX; i++)
        if (s->ealive[i] && s->eneutral[i] && (int)s->ekind[i] == nk) return i;
    return -1;
}

int main(void) {
    float obs[BT_OBS_SIZE], rew, term;

    printf("[1] BT_NEUTRAL_PHYSICS: source-resolved tree geometry in BT_NEUTRALS order\n");
    {
        check(BT_NEUTRAL_PHYSICS_NKINDS == BT_NNEUTRALS,
              "BT_NEUTRAL_PHYSICS_NKINDS mirrors BT_NNEUTRALS");
        check(BT_NEUTRAL_PHYSICS_NKINDS == 2, "two neutral scenes (the two zone trees)");

        for (int i = 0; i < BT_NEUTRAL_PHYSICS_NKINDS; i++) {
            BT_CONST_PTR(BtBodyPhysics) G = &BT_NEUTRAL_PHYSICS[i];
            /* body CollisionShape2D: tree.tscn SubResource(2) radius 24,
             * root-local centre from neutral.tscn Collision position (0,-16). */
            check_near(G->body_radius, 24.0f, 1e-3f, "tree body radius = 24 (tree.tscn)");
            check_near(G->body_x, 0.0f, 1e-3f, "tree body offset x = 0");
            check_near(G->body_y, -16.0f, 1e-3f, "tree body offset y = -16 (neutral.tscn)");
            /* Hurtbox (0,-24) + Hurtbox/Collision (0,-14) = (0,-38), r 48.6621. */
            check_near(G->hurt_radius, 48.6621f, 1e-3f,
                       "tree hurt radius = 48.6621 (neutral.tscn Hurtbox/Collision)");
            check_near(G->hurt_x, 0.0f, 1e-3f, "tree hurt offset x = 0");
            check_near(G->hurt_y, -38.0f, 1e-3f, "tree hurt offset y = -38 (Hurtbox + Collision)");
            check(G->has_hurtbox == 1, "tree owns a Hurtbox");
            check(G->has_hitbox == 0, "tree has no contact Hitbox");
            check_near(G->hit_radius, 0.0f, 1e-3f, "no Hitbox -> hit radius 0");
            check(G->body_mode == 2, "tree RigidBody2D mode 2 (MODE_CHARACTER)");
            check_near(G->mass, 1.0f, 1e-3f, "tree mass = RigidBody2D default 1");
            check_near(G->knockback_resistance, 1.0f, 1e-3f,
                       "tree knockback_resistance = 1.0 (tree_stats.tres)");
        }

        /* Canonical order follows extract_gameplay.py's sorted neutral scenes:
         * the DLC pack path sorts before the campaign path. */
        check(str_has(BT_NEUTRAL_SCENE[0], "dlc_1/neutrals/tree/tree.tscn"),
              "neutral 0 is the DLC zone tree (canonical sort order)");
        check(str_has(BT_NEUTRAL_SCENE[1], "entities/units/neutral/tree.tscn"),
              "neutral 1 is the campaign zone tree");
        check(str_has(BT_NEUTRAL_NAME[0], "tree") && str_has(BT_NEUTRAL_NAME[1], "tree"),
              "both neutral names are the source scene stem 'tree'");
    }

    printf("[2] neutral domain separation: a neutral kind must not alias an enemy row\n");
    {
        /* Both trees share the stem 'tree', which is not a BT_KINDS stem, and
         * neutral kind 0/1 would wrongly resolve BT_PHYSICS[0]/[1] -- comparing
         * them proves the enemy table is a different geometry. */
        for (int i = 0; i < BT_NEUTRAL_PHYSICS_NKINDS; i++) {
            BT_CONST_PTR(BtBodyPhysics) E = &BT_PHYSICS[bt_geom_index(i)];
            BT_CONST_PTR(BtBodyPhysics) N = &BT_NEUTRAL_PHYSICS[i];
            int differs = E->body_radius != N->body_radius ||
                          E->body_y != N->body_y ||
                          E->hurt_radius != N->hurt_radius ||
                          E->hurt_y != N->hurt_y ||
                          E->has_hitbox != N->has_hitbox;
            check(differs, "BT_PHYSICS[neutral kind] is not the tree geometry");
        }
        check(BT_PHYSICS[bt_geom_index(0)].has_hitbox == 1,
              "enemy BT_PHYSICS[0] carries a contact Hitbox the tree must not use");
    }

    printf("[3] a spawned tree body uses BT_NEUTRAL_PHYSICS geometry\n");
    {
        bt_core_init(&A, 71u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        int nk = neutral_kind("tree");
        check(nk >= 0, "a tree neutral kind exists in BT_NEUTRALS");
        if (nk >= 0) {
            check(bt_spawn_neutral(&A, nk, 1000.0f, 700.0f), "neutral tree placed in a slot");
            int t = neutral_slot(&A, nk);
            check(t >= 0, "tree slot is live and tagged neutral");
            if (t >= 0) {
                BT_CONST_PTR(BtBodyPhysics) G = &BT_NEUTRAL_PHYSICS[nk];
                check_near(A.phys.bodies[t].radius, G->body_radius, 1e-3f,
                           "physical body radius from BT_NEUTRAL_PHYSICS");
                check_near(A.phys.bodies[t].offset.x, G->body_x, 1e-3f,
                           "physical body offset x from BT_NEUTRAL_PHYSICS");
                check_near(A.phys.bodies[t].offset.y, G->body_y, 1e-3f,
                           "physical body offset y from BT_NEUTRAL_PHYSICS");
                check_near(A.emass[t], G->mass, 1e-3f, "tree mass from BT_NEUTRAL_PHYSICS");
            }
        }
    }

    printf("[4] a tree deals no contact damage (no Hitbox in BT_NEUTRAL_PHYSICS)\n");
    {
        bt_core_init(&A, 72u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        int nk = neutral_kind("tree");
        int t = -1;
        if (nk >= 0 && bt_spawn_neutral(&A, nk, 1000.0f, 700.0f)) t = neutral_slot(&A, nk);
        check(t >= 0, "tree live for the contact test");
        if (t >= 0) {
            A.px = 1000.0f; A.py = 700.0f;    /* player standing on the tree */
            float hp0 = A.hp;
            bt_core_step(&A, 0, obs, &rew, &term);
            check(A.hp >= hp0 - 1e-4f,
                  "player takes no tree contact damage (has_hitbox = 0)");
            check(A.iframe == 0.0f, "no contact i-frames from a tree");
            check(A.ealive[t] && A.eneutral[t], "the tree is untouched by the player");
        }
    }

    printf("[5] friendly projectiles use the tree's neutral Hurtbox circle\n");
    {
        bt_core_init(&A, 73u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        int nk = neutral_kind("tree");
        int t = -1;
        if (nk >= 0 && bt_spawn_neutral(&A, nk, 1000.0f, 700.0f)) t = neutral_slot(&A, nk);
        check(t >= 0, "tree live for the projectile test");
        if (t >= 0) {
            BT_CONST_PTR(BtBodyPhysics) N = &BT_NEUTRAL_PHYSICS[nk];
            float ncx = A.ex[t] + N->hurt_x, ncy = A.ey[t] + N->hurt_y;
            /* 40 px above the neutral centre: inside 48.6621 + 0.5, but 60 px
             * from the BT_PHYSICS[0] centre (which sits 20 px lower). */
            A.prx[0] = ncx;
            A.pry[0] = ncy - 40.0f;
            A.prrad[0] = 0.5f;
            A.prshape[0] = BT_SHAPE_CIRCLE;
            A.prrot[0] = 0.0f;
            A.prhx[0] = 0.0f; A.prhy[0] = 0.0f;

            check(bt_circle_overlap_offset(bt_v2(A.ex[t], A.ey[t]), bt_v2(N->hurt_x, N->hurt_y),
                                           N->hurt_radius,
                                           bt_v2(A.prx[0], A.pry[0]), bt_v2zero(), A.prrad[0]),
                  "emitted neutral hurt circle contains the shot");
            check(bt_proj_hits_enemy(&A, 0, t) == 1,
                  "bt_proj_hits_enemy uses the neutral Hurtbox circle");

            /* Regression discriminator: the enemy-table row would miss this shot. */
            BT_CONST_PTR(BtBodyPhysics) E = &BT_PHYSICS[bt_geom_index(nk)];
            float ecx = A.ex[t] + E->hurt_x, ecy = A.ey[t] + E->hurt_y;
            float dx = A.prx[0] - ecx, dy = A.pry[0] - ecy;
            float rr = E->hurt_radius + A.prrad[0];
            check(dx * dx + dy * dy > rr * rr,
                  "the enemy BT_PHYSICS row would miss the same shot");
        }
    }

    if (g_failures == 0) printf("test_brotato_neutral_physics: OK (%d checks)\n", g_checks);
    else printf("test_brotato_neutral_physics: %d/%d checks FAILED\n", g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
