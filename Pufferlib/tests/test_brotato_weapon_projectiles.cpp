/* tests/test_brotato_weapon_projectiles.cpp
 *
 * Source-backed regressions for the PLAYER RANGED WEAPON projectile graph
 * (ocean/brotato/brotato_weapon_projectiles.h):
 *   - spawn contract: fields, geometry, the range lifetime, and ZERO RNG draws;
 *   - the deferred first-frame collision (Hitbox.enable is set_deferred);
 *   - motion + destroy_on_leaving_screen (projectile.gd:34,43-44) and the
 *     range-lifetime stop (player_projectile.gd:32-34,74-79);
 *   - hit-once-per-enemy + pierce damage decay (player_projectile.gd:103-117);
 *   - bounce: retarget a random OTHER enemy, INFINITE_RANGE, knockback reset and
 *     bounce damage decay (player_projectile.gd:119-129);
 *   - the carried ExplodingEffect REPLACES the direct hit (unit.gd:552-578);
 *   - projectiles_on_hit children at the target, ignoring the target
 *     (unit.gd:586-604);
 *   - the separate slow hitbox (bullet_taser/bullet_harpoon) applied once;
 *   - knockback (unit.gd:275-333), speed_percent_modifier (unit.gd:605-606) and
 *     the crit -> pierce_on_crit/bounce_on_crit charge order (player_projectile.gd:132-154);
 *   - stop_delay drift (fireball stop_delay 20);
 *   - the DECLARED capacity with an EXPLICIT overflow failure (a live shot is
 *     never overwritten);
 *   - the shareable player_lifesteal_cd timer (run_data.gd:1297-1302).
 *
 * Every expected number is derived here from the recovered sources + the
 * generated tables:
 *   projectiles/player_projectile.gd:4-7,28-34,37-64,74-79,103-154
 *   projectiles/projectile.gd:28-90            motion / stop_delay / stop()
 *   projectiles/delayed_player_projectile.gd:5,8-17
 *   entities/units/unit/unit.gd:275-333,534-608
 *   overlap/hitbox.gd:73-110                   ignored_objects / knockback
 *   singletons/weapon_service.gd:34-135,366-435
 *   singletons/entity_spawner.gd:480-490       get_rand_enemy
 *   singletons/run_data.gd:1297-1302           manage_life_steal
 *   projectiles/projectile.tscn                Hitbox CircleShape2D radius 44 @ (23,0)
 *
 * STATE REQUIREMENT:
 *   brotato_weapon_projectile_types.h (BT_MAX_WPROJ, BT_WPROJ_F_*, BtWProj,
 *   BtWProjPool, BtWProjArgs, BtWProjChild) is included by brotato_state.h
 *   (after BT_MAX), which also adds:
 *     BtWProjPool wproj;
 *     float       player_lifesteal_cd;
 *   plus the profile constants BT_CHARACTER_BULL / BT_WEAPON_NONE used by
 *   bt_core_init (see test_brotato_item_explosions.cpp).  Until those land this
 *   file does not compile -- the same pattern as the other slice tests.
 *
 * Build and run:
 *     c++ -O2 -o build/test_brotato_weapon_projectiles tests/test_brotato_weapon_projectiles.cpp -lm
 *     ./build/test_brotato_weapon_projectiles
 *
 * Exit code 0 = all checks passed.
 */

#include "../ocean/brotato/brotato_core.h"
/* included by brotato_core.h in the integrated build; included explicitly here so
 * this TU compiles while that hook is pending. */
#include "../ocean/brotato/brotato_weapon_projectiles.h"
/* host-only capacity diagnostic (flag -> name), for the hit-queue bound. */
#include "../ocean/brotato/brotato_capacity_host.h"

#include <stdio.h>
#include <string.h>

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
static void check_eq_int(int got, int want, const char* msg) {
    g_checks++;
    if (got != want) { g_failures++; printf("  FAIL: %s (got %d want %d)\n", msg, got, want); }
}

/* BtState is ~1 MiB; keep the big states static. */
static BtState A, B;

static void fresh(BtState* s, bt_u64 seed) {
    bt_core_init(s, seed, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
    s->n_live_groups = 0;
    bt_core_stat_add(s, 1, -15.0f);   /* disable regen in its raw source layer */
    s->regen_timer = 99.0f;
    s->iframe = 0.0f;
    s->dodge = 0.0f;
    s->hp = 1.0e9f;                   /* the player must not die mid-test */
    bt_item_expl_reset(s);
    bt_wproj_reset(s);
}

static int place_enemy_kind(BtState* s, int kind, float x, float y) {
    if (!bt_spawn_kind(s, kind, x, y)) return -1;
    for (int i = 0; i < BT_MAX; i++)
        if (s->ealive[i] && !s->eneutral[i] && (int)s->ekind[i] == kind &&
            s->ex[i] == x && s->ey[i] == y) {
            s->ehp[i] = 100000.0f;
            s->emax[i] = 100000.0f;
            return i;
        }
    return -1;
}
static int place_enemy(BtState* s, float x, float y) { return place_enemy_kind(s, 0, x, y); }
static int place_neutral(BtState* s, float x, float y) {
    if (!bt_spawn_neutral(s, 0, x, y)) return -1;   /* bt_spawn_kind_at(..., neutral=1) */
    for (int i = 0; i < BT_MAX; i++)
        if (s->ealive[i] && s->eneutral[i]) { s->ehp[i] = 1.0e9f; return i; }
    return -1;
}

static BtWProjArgs base_args(float x, float y, float rot, float speed, float dmg) {
    BtWProjArgs a;
    memset(&a, 0, sizeof a);
    a.x = x; a.y = y; a.rot = rot; a.speed = speed; a.dmg = dmg;
    a.crit_dmg = 1.5f;
    a.max_range = 10000.0f;
    a.shape = BT_SHAPE_CIRCLE;
    a.flags = BT_WPROJ_F_LEAVE;
    a.owner_uid = 0;
    a.child.shape = BT_SHAPE_CIRCLE;
    return a;
}

/* A weapon whose canonical fx slice (BT_WEAPON_EFF_INDEX) carries an
 * ExplodingEffect row: want_zero=0 requires chance 1.0 + a resolvable explosion
 * scene (deterministic spawn), want_zero=1 requires chance 0 (the roll never
 * succeeds).  Returns the weapon index, or -1. */
static int find_weapon_with_exploding_row(int want_zero) {
    for (int w = 0; w < BT_NWEAPONS; w++) {
        int f = (int)BT_WEAPONS[w].fx_first, c = (int)BT_WEAPONS[w].fx_count;
        for (int k = 0; k < c; k++) {
            int r = (int)BT_WEAPON_EFF_INDEX[f + k];
            if (r < 0 || r >= BT_NITEM_EFF_ROWS) continue;
            BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[r];
            if (!bt_item_expl_opcode((int)E->opcode)) continue;
            if (want_zero) { if (E->explode_chance <= 0.0f) return w; }
            else if (E->explode_chance >= 1.0f && bt_item_expl_geom(E->explosion_scene) >= 0) return w;
        }
    }
    return -1;
}
/* The single live explosion (these tests spawn one at a time). */
static int live_explosion(BtState* s) {
    for (int i = 0; i < BT_MAX_EXPL; i++) if (s->expl.e[i].alive) return i;
    return -1;
}
static float expected_hit(BtState* s, int j, float base) {
    float armor = bt_kunit(s, j)->armor;
    float v = bt_roundf(base);
    float t = v - armor;
    return t < 1.0f ? 1.0f : t;
}

int main(void) {
    /* ---------------------------------------------------------------- spawn */
    printf("[1] spawn contract + zero RNG draws + capacity\n");
    {
        BtState* s = &A; fresh(s, 7);
        BtRng before = s->rng;
        BtWProjArgs a = base_args(300, 400, 0.0f, 600.0f, 40.0f);
        a.rad = 44.0f; a.offx = 23.0f; a.offy = 0.0f; a.max_range = 500.0f;
        a.piercing = 2; a.pierce_reduce = 0.5f;
        a.bounce = 1; a.bounce_reduce = 0.25f;
        a.crit_chance = 0.2f; a.crit_dmg = 2.0f;
        a.knockback = 100.0f; a.knockback_piercing = 0.3f;
        a.speed_pct = -50.0f; a.slow_rad = 133.5f; a.slow_amt = -200.0f;
        a.rot_speed = 25.0f; a.stop_delay = 20.0f; a.delay = 7.0f;
        a.flags = BT_WPROJ_F_LEAVE;
        a.owner_uid = 3; a.attack_id = 42;
        a.src_weapon_id = 7; a.src_curse = 0.5f; a.is_elemental = 1;
        a.src_fxcurse[0] = 0.25f; a.src_fxcurse[1] = -0.5f;
        int i = bt_wproj_spawn(s, a);
        check_eq_int(i, 0, "spawn fills slot 0");
        check(s->wproj.p[0].alive == 1, "slot is live");
        check_near(s->wproj.p[0].vx, 600.0f, 1e-4f, "vx = cos(0)*speed");
        check_near(s->wproj.p[0].vy, 0.0f, 1e-4f, "vy = sin(0)*speed");
        check_near(s->wproj.p[0].range_timer, (500.0f + BT_WPROJ_EXTRA_DIST) / 600.0f, 1e-5f,
                   "range lifetime (max_range+100)/speed");
        /* Hitbox.enable() is set_deferred (hitbox.gd:104-106), but Godot's
         * MessageQueue flush (SceneTree::iteration, scene_tree.cpp:609) drains
         * that call in the SAME frame, before that frame's
         * Physics2DServer::step -- so the shot is in the tree from its spawn
         * frame and the spawn frame's step already tests its hitbox at the
         * SPAWN position (hitbox.gd `active` is set immediately by shoot()). */
        check(s->wproj.p[0].armed == 1, "in the tree from the spawn frame (enable was flushed)");
        check_eq_int((int)s->hitq.n, 0, "the spawn frames no enemy -> no pair queued");
        check_near(s->wproj.p[0].rad, 44.0f, 1e-6f, "rad from caller");
        check_near(s->wproj.p[0].offx, 23.0f, 1e-6f, "offx from caller");
        check_eq_int((int)s->wproj.p[0].shape, BT_SHAPE_CIRCLE, "shape circle");
        check_eq_int((int)s->wproj.p[0].piercing, 2, "piercing");
        check_eq_int((int)s->wproj.p[0].bounce, 1, "bounce");
        check_eq_int((int)s->wproj.p[0].owner_uid, 3, "owner_uid");
        check_eq_int((int)s->wproj.p[0].src_weapon_id, 7, "immutable src_weapon_id snapshot");
        check_near(s->wproj.p[0].src_curse, 0.5f, 1e-6f, "immutable src_curse snapshot");
        check_near(s->wproj.p[0].src_fxcurse[0], 0.25f, 1e-6f, "per-effect curse snapshot [0]");
        check_near(s->wproj.p[0].src_fxcurse[1], -0.5f, 1e-6f, "per-effect curse snapshot [1]");
        check_eq_int((int)s->wproj.p[0].is_elemental, 1, "is_elemental snapshot");
        check_eq_int(s->wproj.p[0].attack_id, 42, "attack_id");
        check((s->wproj.p[0].flags & BT_WPROJ_F_LEAVE) != 0, "flag kept");
        check((s->wproj.p[0].flags & BT_WPROJ_F_SLOW) != 0, "slow flag derived from slow_rad");
        check_eq_int((int)s->wproj.n_spawned, 1, "n_spawned");
        check(memcmp(&before, &s->rng, sizeof(BtRng)) == 0, "spawn consumes ZERO RNG for a non-delayed shot");

        /* fill the pool; the very next spawn FAILS EXPLICITLY */
        float dmg0 = s->wproj.p[0].dmg;
        int ok = 0;
        for (int k = 0; k < BT_MAX_WPROJ + 4; k++) {
            BtWProjArgs b = base_args(10.0f, 10.0f, 0.0f, 600.0f, 5.0f);
            if (bt_wproj_spawn(s, b) >= 0) ok++; else break;
        }
        check_eq_int(ok, BT_MAX_WPROJ - 1, "pool holds BT_MAX_WPROJ-1 more");
        check_eq_int((int)s->wproj.n_spawned, BT_MAX_WPROJ, "all slots live");
        check_eq_int((int)s->wproj.n_overflow, 1, "overflow is explicit");
        check_near(s->wproj.p[0].dmg, dmg0, 1e-6f, "a live shot is never overwritten");
    }

    /* ------------------------------------------------- deferred collision */
    printf("[2] spawn-frame pairing + deferred hit + motion\n");
    {
        BtState* s = &A; fresh(s, 11);
        int j = place_enemy(s, 100.0f, 400.0f);
        check(j >= 0, "enemy placed");
        BtWProjArgs a = base_args(100.0f, 400.0f, 0.0f, 600.0f, 40.0f);
        a.rad = 44.0f; a.max_range = 10000.0f;
        int i = bt_wproj_spawn(s, a);
        float hp0 = s->ehp[j];
        float r = 0.0f;
        /* The spawn frame: the hitbox was enabled by this frame's flush, so its
         * step already frames the enemy at the SPAWN position -- but the report
         * is only QUEUED (unit.gd:610-611 defers the hurt to the next frame's
         * flush) and the shot has not moved yet. */
        check_eq_int((int)s->hitq.n, 1, "the spawn frame pairs the shot with the target it frames");
        check_eq_int((int)s->hitq.a[0], i, "the queued pair names the shot slot");
        check_eq_int((int)s->hitq.b[0], j, "the queued pair names the enemy slot");
        check_near(s->wproj.p[i].x, 100.0f, 1e-3f, "the spawn frame does not move the shot");
        check_near(s->ehp[j], hp0, 0.01f, "no damage on the spawn frame: the hurt is deferred");
        /* The next frame's flush runs the deferred hurt (its roll, its damage
         * and its stop()); only after that does the shot's own
         * _physics_process move it. */
        bt_wproj_step(s, &r);
        check_near(s->ehp[j], hp0 - expected_hit(s, j, 40.0f), 0.05f, "direct hit applies damage");
        check(s->wproj.p[i].alive == 0, "no pierce/bounce -> stop() on the hit");
        check_eq_int((int)s->wproj.p[i].n_hits, 1, "one hit_something event");
        check_near(s->wproj.p[i].hit_dmg, expected_hit(s, j, 40.0f), 0.05f, "hit_something post-armor damage");
        check_eq_int((int)s->wproj.p[i].hit_crit, 0, "not a crit");
        check_near(s->wproj.p[i].x, 100.0f, 1e-3f, "a shot stopped by its hit never moves");
        check_eq_int((int)s->hitq.n, 0, "the queue is drained by that flush");
        check_near(r, 0.0f, 1e-6f, "no kill -> no reward");
    }

    /* A shot that frames nothing at spawn moves on its first frame and pairs
     * only when its circle reaches a target; that pair resolves one frame later,
     * at the position where the overlap was reported. */
    {
        BtState* s = &A; fresh(s, 12);
        int j = place_enemy(s, 400.0f, 400.0f);
        check(j >= 0, "enemy placed");
        BtWProjArgs a = base_args(100.0f, 400.0f, 0.0f, 600.0f, 40.0f);
        a.rad = 5.0f; a.max_range = 10000.0f;
        int i = bt_wproj_spawn(s, a);
        float r = 0.0f;
        float hp0 = s->ehp[j];
        check_eq_int((int)s->hitq.n, 0, "a shot that frames nothing at spawn queues no pair");
        int steps = 0;
        while (s->hitq.n == 0 && steps < 60) { bt_wproj_step(s, &r); steps++; }
        check_eq_int((int)s->hitq.n, 1, "the moving shot pairs when its circle reaches the target");
        check(steps > 1, "the target is reached after the spawn frame");
        check_near(s->wproj.p[i].x, 100.0f + 10.0f * (float)steps, 1e-2f,
                   "the pair is taken at the shot's post-move position");
        check_near(s->ehp[j], hp0, 0.01f, "and its damage still waits for the next flush");
        bt_wproj_step(s, &r);
        check_near(s->ehp[j], hp0 - expected_hit(s, j, 40.0f), 0.05f,
                   "the moving hit lands one frame after the pair");
        check(s->wproj.p[i].alive == 0, "hit-once -> stop");
        check_near(r, 0.0f, 1e-6f, "no kill -> no reward");
    }

    /* ------------------------------------------------------- offscreen */
    printf("[3] destroy_on_leaving_screen\n");
    {
        BtState* s = &A; fresh(s, 13);
        BtWProjArgs a = base_args(BT_CAM_RECT_X1 - 5.0f, 400.0f, 0.0f, 600.0f, 40.0f);
        a.rad = 44.0f;
        int i = bt_wproj_spawn(s, a);
        float r = 0.0f;
        bt_wproj_step(s, &r);   /* arm */
        bt_wproj_step(s, &r);   /* move offscreen -> stop */
        check(s->wproj.p[i].alive == 0, "crossing the camera edge returns the shot to the pool");

        BtWProjArgs b = base_args(BT_CAM_RECT_X1 - 5.0f, 400.0f, 0.0f, 600.0f, 40.0f);
        b.rad = 44.0f; b.flags = 0;   /* destroy_on_leaving_screen = false */
        int k = bt_wproj_spawn(s, b);
        bt_wproj_step(s, &r);
        bt_wproj_step(s, &r);
        check(s->wproj.p[k].alive == 1, "without the flag it survives");
    }

    /* --------------------------------------------------- pierce + decay */
    printf("[4] pierce: every pair of one frame is queued, then drained in order\n");
    {
        BtState* s = &A; fresh(s, 17);
        int j1 = place_enemy(s, 150.0f, 400.0f);
        int j2 = place_enemy(s, 150.0f, 405.0f);
        check(j1 >= 0 && j2 >= 0 && j1 != j2, "two enemies placed");
        check(j1 < j2, "slots are ascending (placement order)");
        float hp1 = s->ehp[j1], hp2 = s->ehp[j2];
        BtWProjArgs a = base_args(120.0f, 400.0f, 0.0f, 600.0f, 40.0f);
        a.rad = 44.0f; a.piercing = 1; a.pierce_reduce = 0.5f;
        int i = bt_wproj_spawn(s, a);
        float r = 0.0f;
        /* ONE shot framing TWO hurtboxes in one frame queues BOTH handlers (the
         * engine reports every pair of the step); the source's stop()/pierce
         * branch cannot drop the second one, because every reported handler of
         * the step runs its own tail. */
        check_eq_int((int)s->hitq.n, 2, "both pairs of the frame are queued");
        check_eq_int((int)s->hitq.a[0], i, "pair 0 is the shot");
        check_eq_int((int)s->hitq.a[1], i, "pair 1 is the shot");
        check_eq_int((int)s->hitq.b[0], j1, "pair 0 is the lower enemy slot (scan order)");
        check_eq_int((int)s->hitq.b[1], j2, "pair 1 is the higher enemy slot");
        bt_wproj_step(s, &r);   /* the flush drains both, in report order */
        /* first enemy takes 40, damage decays to max(1,40-20)=20, second takes 20 */
        int lo = j1 < j2 ? j1 : j2, hi = j1 < j2 ? j2 : j1;
        check_near(s->ehp[lo], (lo == j1 ? hp1 : hp2) - expected_hit(s, lo, 40.0f), 0.05f,
                   "first target takes full damage");
        check_near(s->ehp[hi], (hi == j1 ? hp1 : hp2) - expected_hit(s, hi, 20.0f), 0.05f,
                   "second target takes the decayed damage");
        check(s->wproj.p[i].alive == 0, "pierce exhausted -> stop");
    }

    /* Hitbox.damage is an int: the pierce reduction is truncated each hit
     * (double_barrel_shotgun: 3 -> 2 -> 1 with reduce 0.3). */
    {
        BtState* s = &A; fresh(s, 18);
        int j1 = place_enemy(s, 150.0f, 400.0f);
        int j2 = place_enemy(s, 150.0f, 405.0f);
        check(j1 >= 0 && j2 >= 0, "two enemies placed");
        BtWProjArgs a = base_args(120.0f, 400.0f, 0.0f, 600.0f, 3.0f);
        a.rad = 44.0f; a.piercing = 2; a.pierce_reduce = 0.3f;
        int i = bt_wproj_spawn(s, a);
        float r = 0.0f;
        bt_wproj_step(s, &r);
        bt_wproj_step(s, &r);
        check_near(s->wproj.p[i].dmg, 1.0f, 1e-6f, "int truncation 3->2->1 (not 1.47)");
        check_eq_int(s->wproj.p[i].piercing, 0, "two pierces spent on two hits");
    }

    /* Simultaneous victims resolve in CREATION order, which is what the
     * bridge's BROTATO_RL_ORDER_HURTS comparator uses: ascending victim
     * INSTANCE id == ascending creation order == ascending `e_object_uid` (the
     * pooled instance's uid, minted once per fresh node and retained across a
     * pooled reuse -- brotato_core.h:722-723, exactly ObjectDB's semantics).
     * NOT the actor-slot order the scan walks.  The fixture states the two
     * identities explicitly so the case is deterministic: the target in the
     * LOWER slot is the one created LATER. */
    {
        BtState* s = &A; fresh(s, 19);
        int s1 = place_enemy_kind(s, 0, 100.0f, 400.0f);
        int s2 = place_enemy_kind(s, 0, 100.0f, 405.0f);
        check(s1 >= 0 && s2 >= 0 && s1 < s2, "two targets: the first occupies the lower slot");
        /* creation order inverted relative to slot order */
        s->e_object_uid[s1] = 500u;   /* created LATER ... */
        s->e_object_uid[s2] = 100u;   /* ... than the target in the higher slot */
        float hp1 = s->ehp[s1], hp2 = s->ehp[s2];
        BtWProjArgs a2 = base_args(120.0f, 400.0f, 0.0f, 600.0f, 40.0f);
        a2.rad = 44.0f; a2.piercing = 1; a2.pierce_reduce = 0.5f;
        int i = bt_wproj_spawn(s, a2);
        check(i >= 0, "the shot spawned");
        check_eq_int((int)s->hitq.n, 2, "both victims are queued in the one frame");
        check_eq_int((int)s->hitq.b[0], s2, "the CREATED-EARLIER victim is queued first");
        check_eq_int((int)s->hitq.b[1], s1, "the created-later victim second, despite its lower slot");
        check_eq_int((int)s->hitq.b_uid[0], 100u, "and the queued identity tokens follow the same order");
        float r = 0.0f;
        bt_wproj_step(s, &r);
        check_near(s->ehp[s2], hp2 - expected_hit(s, s2, 40.0f), 0.05f,
                   "creation-first victim takes the full hit");
        check_near(s->ehp[s1], hp1 - expected_hit(s, s1, 20.0f), 0.05f,
                   "and the later one the decayed hit (slot order would swap these)");
    }

    /* Two shots hitting ONE victim in the same step resolve in HITBOX creation
     * order (the bridge's `_hurt_before` second key), which the wproj pool's
     * slot order does NOT follow: a slot freed by a returned shot is handed to
     * the NEXT spawn, so the later-created shot sits in the lower slot. */
    {
        BtState* s = &A; fresh(s, 23);
        BtWProjArgs far = base_args(600.0f, 600.0f, 0.0f, 0.0f, 40.0f);
        far.rad = 44.0f; far.scene_istr = 5u;
        int p1 = bt_wproj_spawn(s, far);          /* slot 0, created first */
        int p2 = bt_wproj_spawn(s, far);          /* slot 1, created second */
        check(p1 == 0 && p2 == 1, "two shots in slots 0 and 1");
        bt_wproj_return(s, p1);                   /* slot 0 free, node resident */
        int p3 = bt_wproj_spawn(s, far);          /* reclaims slot 0, created THIRD */
        check(p3 == 0, "the next spawn reclaims the freed slot (slot order != creation order)");
        check(s->wproj.cre[p3] > s->wproj.cre[p2],
              "the lower slot holds the LATER-created hitbox");
        int j = place_enemy_kind(s, 0, 150.0f, 400.0f);
        check(j >= 0, "victim placed");
        s->ehp[j] = 5.0f; s->emax[j] = 5.0f;       /* one hit kills it */
        s->wproj.p[p3].x = 150.0f; s->wproj.p[p3].y = 400.0f;   /* both onto the victim */
        s->wproj.p[p2].x = 150.0f; s->wproj.p[p2].y = 400.0f;
        float r = 0.0f;
        bt_wproj_step(s, &r);                     /* the step: SLOT order enqueues */
        check_eq_int((int)s->hitq.n, 2, "both shots reported the victim in one step");
        check_eq_int((int)s->hitq.a[0], p3, "the enqueue order is the slot order");
        check((int)s->hitq.a_cre[0] > (int)s->hitq.a_cre[1], "which is NOT the creation order");
        bt_wproj_step(s, &r);                     /* the flush: creation order */
        check_near(s->wproj.p[p2].hit_dmg, 40.0f, 1e-4f,
                   "the CREATION-FIRST hitbox lands its hit on the victim");
        check_near(s->wproj.p[p3].hit_dmg, 0.0f, 1e-4f,
                   "and the later hitbox finds the victim already gone (slot order would swap these)");
        check(s->ehp[j] <= 0.0f, "the victim died from the first-applied hit");
    }

    /* ------------------------------------------------ hit-queue capacity */
    printf("[4b] hit queue capacity: fail-closed, no reorder\n");
    {
        /* One frame can report more pairs than BT_HITQ_CAP: several shots, each
         * framing a crowd that none of them has touched yet.  The queue is
         * FAIL-CLOSED -- the pairs that do not fit are dropped and BT_CAP_HITQ
         * latches, and the prefix that fits keeps the engine's report order
         * (bt_wproj_enqueue_hit) -- never a silent reorder or a wrapped count.
         * BT_HITQ_CAP is sized from the DEEP peak-occupancy probe with headroom
         * (tests/brotato_state_size.cpp `hitq_peak`). */
        enum { NENEMY = 40 };
        BtState* s = &A; fresh(s, 20);
        int e[NENEMY];
        int placed = 0;
        for (int k = 0; k < NENEMY; k++) {
            int slot = place_enemy_kind(s, 0, 100.0f + 30.0f * (float)(k % 8),
                                        100.0f + 30.0f * (float)(k / 8));
            if (slot < 0) break;
            e[placed++] = slot;
        }
        check_eq_int(placed, NENEMY, "the crowd is placed");
        int want_shots = BT_HITQ_CAP / placed + 1;      /* > cap when all overlap */
        check(want_shots <= BT_MAX_WPROJ, "the crowding shots fit the projectile pool");
        int shot[64];
        check(want_shots <= (int)(sizeof shot / sizeof shot[0]), "test-local shot table");
        int spawned = 0;
        for (int k = 0; k < want_shots; k++) {
            BtWProjArgs a = base_args(100.0f, 100.0f, 0.0f, 0.0f, 40.0f);
            a.rad = 2000.0f;             /* every shot frames the whole crowd */
            a.piercing = BT_HITQ_CAP;    /* no shot stops inside this test */
            int si = bt_wproj_spawn(s, a);
            if (si < 0) break;
            shot[spawned++] = si;
        }
        check_eq_int(spawned, want_shots, "all crowding shots spawned");
        check_eq_int((int)s->hitq.n, BT_HITQ_CAP, "exactly the capacity is queued");
        check_eq_int((int)s->hitq.overflow, 1, "the pairs that do not fit are dropped");
        check((bt_capacity_mask(s) & (1ull << BT_CAP_HITQ)) != 0ull, "BT_CAP_HITQ latches");
        check(bt_capacity_name(BT_CAP_HITQ) != NULL, "the class has a diagnostic name");
        /* the queued prefix is the report order: shot by shot, enemy slot
         * ascending -- the drop happens at the TAIL, nothing is reordered */
        check_eq_int((int)s->hitq.a[0], shot[0], "pair 0 belongs to the first shot");
        check_eq_int((int)s->hitq.b[0], e[0], "pair 0 targets the first enemy slot");
        int li = BT_HITQ_CAP - 1;
        check_eq_int((int)s->hitq.a[li], shot[li / placed], "the last queued pair keeps report order");
        check_eq_int((int)s->hitq.b[li], e[li % placed], "the last queued pair keeps report order");
        /* and the queue that fits still resolves */
        float r = 0.0f;
        float hp_e = s->ehp[e[0]];
        bt_wproj_step(s, &r);
        check(s->ehp[e[0]] < hp_e, "the queued prefix still resolves");
        check_eq_int((int)s->hitq.n, 0, "and the queue drains");
    }

    /* ---------------------------------------------------------- bounce */
    printf("[5] bounce retargets a different enemy + INFINITE_RANGE + decay\n");
    {
        BtState* s = &A; fresh(s, 19);
        int ja = place_enemy(s, 150.0f, 400.0f);
        int jb = place_enemy(s, 400.0f, 400.0f);
        check(ja >= 0 && jb >= 0, "two enemies placed");
        BtWProjArgs a = base_args(120.0f, 400.0f, 0.0f, 600.0f, 40.0f);
        a.rad = 44.0f; a.bounce = 1; a.bounce_reduce = 0.5f;
        a.knockback = 100.0f; a.knockback_piercing = 0.0f;
        int i = bt_wproj_spawn(s, a);
        float r = 0.0f;
        bt_wproj_step(s, &r);   /* arm */
        bt_wproj_step(s, &r);   /* hits ja -> bounce toward jb */
        check(s->wproj.p[i].alive == 1, "bounce keeps the shot alive");
        check_eq_int(s->wproj.p[i].bounce, 0, "bounce spent");
        check_near(s->wproj.p[i].dmg, 20.0f, 1e-4f, "bounce damage decay");
        check(s->wproj.p[i].range_timer > 10.0f, "range reset to INFINITE (10000)");
        check_near(s->wproj.p[i].kb_amt, 0.0f, 1e-6f, "knockback reset on bounce");
        check(s->wproj.p[i].vx > 0.0f, "retargets jb (to the right)");
        check_near(s->wproj.p[i].vy, 0.0f, 1e-3f, "straight along +x");
        /* a live neutral must never be a bounce target (get_rand_enemy) */
        place_neutral(s, 700.0f, 700.0f);
        int pick = bt_wproj_rand_enemy(s, -1);
        check(pick >= 0 && !s->eneutral[pick], "bounce ignores neutral trees");
    }

    /* ------------------------------------------- explosion replaces hit */
    printf("[6] carried ExplodingEffect replaces the direct hit (unit.gd:552-578)\n");
    {
        int wf = find_weapon_with_exploding_row(0);
        check(wf >= 0, "a weapon fx slice with an ExplodingEffect (chance 1) exists");
        BtState* s = &A; fresh(s, 23);
        int j = place_enemy(s, 100.0f, 400.0f);
        float hp0 = s->ehp[j];
        BtWProjArgs a = base_args(100.0f, 400.0f, 0.0f, 600.0f, 40.0f);
        a.rad = 44.0f;
        if (wf >= 0) {
            a.fx_first = BT_WEAPONS[wf].fx_first;
            a.fx_count = (unsigned char)BT_WEAPONS[wf].fx_count;
        }
        int i = bt_wproj_spawn(s, a);
        float r = 0.0f;
        bt_wproj_step(s, &r);
        bt_wproj_step(s, &r);
        check_eq_int((int)s->expl.n_spawned, 1, "the explosion was spawned");
        check_near(s->ehp[j], hp0, 0.01f, "no direct damage when exploding");
        check_eq_int((int)s->wproj.p[i].n_hits, 1, "one hit_something event");
        check_near(s->wproj.p[i].hit_dmg, 0.0f, 1e-6f, "emitted damage is 0 when exploding");
        int e = live_explosion(s);
        check(e >= 0, "an explosion is live");
        if (e >= 0) {
            check_near(s->expl.e[e].x, s->ex[j], 1e-4f, "spawned at the target");
            check_near(s->expl.e[e].y, s->ey[j], 1e-4f, "spawned at the target");
        }
        check(s->wproj.p[i].alive == 0, "no pierce/bounce -> stop");
    }

    /* the ExplodingEffect chance is rolled: a failed roll deals direct damage */
    {
        int wf0 = find_weapon_with_exploding_row(1);
        if (wf0 >= 0) {
            BtState* s = &A; fresh(s, 24);
            int j = place_enemy(s, 100.0f, 400.0f);
            float hp0 = s->ehp[j];
            BtWProjArgs a = base_args(100.0f, 400.0f, 0.0f, 600.0f, 40.0f);
            a.rad = 44.0f;
            a.fx_first = BT_WEAPONS[wf0].fx_first;
            a.fx_count = (unsigned char)BT_WEAPONS[wf0].fx_count;
            int i = bt_wproj_spawn(s, a);
            float r = 0.0f;
            bt_wproj_step(s, &r);
            bt_wproj_step(s, &r);
            check_eq_int((int)s->expl.n_spawned, 0, "chance 0 -> no explosion");
            check(s->ehp[j] < hp0, "a failed explosion roll deals the direct hit");
            check_near(s->wproj.p[i].hit_dmg, hp0 - s->ehp[j], 0.05f,
                       "emitted damage is the direct hit");
        }
    }

    /* ------------------------------------------------ projectiles_on_hit */
    printf("[7] projectiles_on_hit children at the target, target ignored\n");
    {
        BtState* s = &A; fresh(s, 29);
        int j = place_enemy(s, 100.0f, 400.0f);
        BtWProjArgs a = base_args(100.0f, 400.0f, 0.0f, 600.0f, 40.0f);
        a.rad = 44.0f;
        a.onhit_n = 3; a.onhit_auto = 0;
        a.child.dmg = 5.0f; a.child.speed = 600.0f; a.child.max_range = 10000.0f;
        a.child.rad = 44.0f; a.child.crit_dmg = 1.5f;
        a.child.shape = BT_SHAPE_CIRCLE;
        int i = bt_wproj_spawn(s, a);
        float r = 0.0f;
        float hp0 = s->ehp[j];
        float parent_dmg = expected_hit(s, j, 40.0f);
        /* The parent's pair comes from its spawn frame; THIS frame's flush runs
         * the hurt, which spawns the children into the same pass. */
        bt_wproj_step(s, &r);
        check(s->wproj.p[i].alive == 0, "parent stopped after the hit");
        check_near(s->ehp[j], hp0 - parent_dmg, 0.05f, "the parent's own hit is the only damage");
        int kids = 0;
        for (int q = 0; q < BT_MAX_WPROJ; q++) {
            if (!s->wproj.p[q].alive) continue;
            kids++;
            /* a child added at this frame's flush is in the tree for this
             * frame's step (so the spawn site paired it) but has not run its
             * first _physics_process yet: it sits exactly on the target. */
            check_near(s->wproj.p[q].x, s->ex[j], 1e-4f, "child at the target x");
            check_near(s->wproj.p[q].y, s->ey[j], 1e-4f, "child at the target y");
            check(bt_wproj_ignored(&s->wproj.p[q], bt_wproj_eid(s, j)) != 0, "child ignores the target");
            check_near(s->wproj.p[q].dmg, 5.0f, 1e-6f, "child damage");
        }
        check_eq_int(kids, 3, "exactly onhit_n children");
        check_eq_int((int)s->hitq.n, 0, "the ignored spawner is never paired");
        /* next frame: the children move, and a target they ignore never pairs */
        bt_wproj_step(s, &r);
        int kids2 = 0;
        for (int q = 0; q < BT_MAX_WPROJ; q++) if (s->wproj.p[q].alive) kids2++;
        check_eq_int(kids2, 3, "children move on their first _physics_process frame");
        check_eq_int((int)s->hitq.n, 0, "still no pair against the ignored target");
        check_near(s->ehp[j], hp0 - parent_dmg, 0.05f, "the children never hurt their spawner");
    }

    /* ------------------------------------------------------- slow hitbox */
    printf("[8] separate slow hitbox applied once per projectile\n");
    {
        BtState* s = &A; fresh(s, 31);
        int j = place_enemy(s, 200.0f, 400.0f);
        BtWProjArgs a = base_args(100.0f, 400.0f, 0.0f, 600.0f, 40.0f);
        a.rad = 5.0f; a.slow_rad = 250.0f; a.slow_amt = -200.0f;
        int i = bt_wproj_spawn(s, a);
        float r = 0.0f;
        bt_wproj_step(s, &r);   /* arm */
        bt_wproj_step(s, &r);   /* slow tick */
        float want = bt_add_decaying_speed(-200, s->espd[j], 0.0f);
        check_near(s->edecay[j], want, 1e-4f, "slow applies add_decaying_speed(-200)");
        float got = s->edecay[j];
        bt_wproj_step(s, &r);
        check_near(s->edecay[j], got, 1e-6f, "slow applies only once per projectile");
        check(s->wproj.p[i].slow_rad == 250.0f, "slow radius retained");
    }

    /* ------------------------------------------- knockback + speed_pct */
    printf("[9] knockback (unit.gd:281-282,333) + speed_percent_modifier (unit.gd:606)\n");
    {
        BtState* s = &A; fresh(s, 37);
        int j = place_enemy(s, 100.0f, 400.0f);
        /* the fixture drives bt_wproj_step directly: bt_core_step's per-frame
         * Node-transform refresh (s->ehx/s->ehy, brotato_state.h:749-765) never
         * runs, so state the lagged transform the knockback reads. */
        s->ehx[j] = s->ex[j]; s->ehy[j] = s->ey[j];
        BtWProjArgs a = base_args(100.0f, 400.0f, 0.0f, 600.0f, 40.0f);
        a.rad = 44.0f; a.knockback = 100.0f; a.speed_pct = -50.0f;
        bt_wproj_spawn(s, a);
        float r = 0.0f;
        bt_wproj_step(s, &r);
        bt_wproj_step(s, &r);
        /* unit.gd:333 stores `knockback_direction * final_knockback_amount`, and
         * unit.gd:281-282 makes knockback_direction the 0.2/0.8 weighted form for
         * ANY amount sign (not just a pull): replays the source with the target
         * 924 px left of and 368 px above the player. */
        BtVec2 hb = bt_v2(bt_cosf(a.rot), bt_sinf(a.rot));
        BtVec2 want_kb = bt_stored_knockback(
            bt_knockback_direction_weighted(hb, bt_v2(s->px, s->py),
                                            bt_v2(s->ehx[j], s->ehy[j])),
            a.knockback);
        check_near(s->ekbx[j], want_kb.x, 1e-3f, "stored knockback = weighted direction*amount (x)");
        check_near(s->ekby[j], want_kb.y, 1e-3f, "stored knockback = weighted direction*amount (y)");
        check(bt_fabsf(s->ekbx[j] - hb.x * a.knockback) > 1e-2f,
              "and NOT the raw hitbox direction (unit.gd:333)");
        float want = bt_add_decaying_speed_amt(s->espd[j] * -50.0f * 0.01f, s->espd[j], 0.0f);
        check_near(s->edecay[j], want, 1e-4f, "speed_percent_modifier decay");
    }

    /* --------------------------------------------- crit -> pierce charge */
    printf("[10] crit charge order: pierce_on_crit lets the shot pierce\n");
    {
        BtState* s = &A; fresh(s, 41);
        int j = place_enemy(s, 100.0f, 400.0f);
        check(j >= 0, "enemy placed");
        BtWProjArgs a = base_args(100.0f, 400.0f, 0.0f, 600.0f, 40.0f);
        a.rad = 44.0f; a.crit_chance = 1.0f; a.crit_pierce = 1; a.crit_bounce = 0;
        a.pierce_reduce = 0.5f;
        int i = bt_wproj_spawn(s, a);
        float r = 0.0f;
        bt_wproj_step(s, &r);
        bt_wproj_step(s, &r);
        check(s->wproj.p[i].alive == 1, "crit pierce keeps the shot alive");
        check_eq_int(s->wproj.p[i].piercing, 0, "crit added a pierce that was consumed");
        check_near(s->wproj.p[i].dmg, 20.0f, 1e-4f, "pierce damage decay");
    }

    /* ---------------------------------------- delayed + stop_delay */
    printf("[11] delayed arming + stop_delay drift\n");
    {
        BtState* s = &A; fresh(s, 43);
        int j = place_enemy(s, 100.0f, 400.0f);
        float hp0 = s->ehp[j];
        BtWProjArgs a = base_args(100.0f, 400.0f, 0.0f, 600.0f, 40.0f);
        a.rad = 44.0f; a.flags = BT_WPROJ_F_LEAVE | BT_WPROJ_F_DELAYED;
        /* pre-seed the node's retained delay: same scene => pool reuse, no draw */
        s->wproj.cache_res[0] = 1; s->wproj.cache_scene[0] = 0x1234u;
        s->wproj.cache_delay[0] = 3.0f; s->wproj.cache_seq[0] = 1u;
        s->wproj.used = 1;
        a.scene_istr = 0x1234u;
        int i = bt_wproj_spawn(s, a);
        check_near(s->wproj.p[i].delay, 3.0f, 1e-6f, "same-scene node reuses its retained delay");
        /* a different scene is a FRESH node -> exactly one initializer draw */
        BtWProjArgs f = base_args(300.0f, 300.0f, 0.0f, 600.0f, 5.0f);
        f.rad = 44.0f; f.flags = BT_WPROJ_F_DELAYED; f.scene_istr = 0x9999u;
        int fi = bt_wproj_spawn(s, f);
        check(fi >= 0 && s->wproj.cache_delay[fi] >= 0.0f && s->wproj.cache_delay[fi] <= 3.0f,
              "fresh node delay in [0,3]");
        check_near(s->wproj.p[fi].delay, s->wproj.cache_delay[fi], 1e-6f,
                   "fresh node countdown == its initializer");
        bt_wproj_return(s, fi);   /* keep the pool as the delayed test expects */
        float r = 0.0f;
        for (int k = 0; k < 3; k++) bt_wproj_step(s, &r);   /* 3 -> 2 -> 1 -> armed */
        check_eq_int((int)s->hitq.n, 1, "the frame the delay expires pairs at the moved position");
        check_near(s->ehp[j], hp0, 0.01f, "a disabled hitbox pairs nothing while delayed");
        bt_wproj_step(s, &r);   /* the next frame's flush runs the hurt */
        check_near(s->ehp[j], hp0 - expected_hit(s, j, 40.0f), 0.05f, "hits once armed");
        check(s->wproj.p[i].alive == 0, "then stops");

        BtState* t = &B; fresh(t, 47);
        BtWProjArgs b = base_args(500.0f, 500.0f, 0.0f, 600.0f, 40.0f);
        b.rad = 44.0f; b.stop_delay = 20.0f; b.flags = 0;
        int k2 = bt_wproj_spawn(t, b);
        bt_wproj_stop(t, k2);
        check(t->wproj.p[k2].alive == 1, "stop with stop_delay keeps the node");
        /* stop() disabled the hitbox, but the drift itself is the in-tree
         * node's own _physics_process: it counts from the first frame after the
         * stop and returns to the pool on the frame `_elapsed_delay` reaches
         * stop_delay (projectile.gd:36-41). */
        bt_wproj_step(t, &r);           /* drift frame 1 */
        for (int k = 0; k < 18; k++) bt_wproj_step(t, &r);
        check(t->wproj.p[k2].alive == 1, "drifts less than stop_delay frames");
        bt_wproj_step(t, &r);
        check(t->wproj.p[k2].alive == 0, "returns after stop_delay frames");
    }

    /* -------- scene-keyed LIFO node pool: interleaved two-scene reverse return */
    {
        BtState* s = &A; fresh(s, 61);
        BtWProjArgs x = base_args(100.0f, 100.0f, 0.0f, 600.0f, 5.0f);
        x.rad = 44.0f; x.scene_istr = 10u;
        BtWProjArgs y = base_args(200.0f, 200.0f, 0.0f, 600.0f, 5.0f);
        y.rad = 44.0f; y.scene_istr = 20u;
        int a1 = bt_wproj_spawn(s, x);
        int b1 = bt_wproj_spawn(s, y);
        check(a1 >= 0 && b1 >= 0 && a1 != b1, "two scene nodes instantiated");
        s->wproj.cache_delay[a1] = 1.0f; s->wproj.cache_delay[b1] = 2.0f;
        bt_wproj_return(s, a1);   /* scene-10 node returned FIRST  */
        bt_wproj_return(s, b1);   /* scene-20 node returned SECOND (later seq) */
        int a2 = bt_wproj_spawn(s, x);
        check(a2 == a1, "scene-10 shot reuses ITS node, not the later-returned other scene");
        int b2 = bt_wproj_spawn(s, y);
        check(b2 == b1, "scene-20 shot reuses ITS node");
        check_near(s->wproj.cache_delay[a1], 1.0f, 1e-6f, "retained descriptor per node (a)");
        check_near(s->wproj.cache_delay[b1], 2.0f, 1e-6f, "retained descriptor per node (b)");
        /* another scene's resident node is never taken: a 3rd scene needs a fresh slot */
        BtWProjArgs z = base_args(300.0f, 300.0f, 0.0f, 600.0f, 5.0f);
        z.rad = 44.0f; z.scene_istr = 30u;
        int c1 = bt_wproj_spawn(s, z);
        check(c1 >= 0 && c1 != a1 && c1 != b1, "a new scene takes a never-instantiated slot");
    }

    /* ------------------------------------------------------ lifesteal */
    printf("[12] player_lifesteal_cd gates the lifesteal heal\n");
    {
        BtState* s = &A; fresh(s, 53);
        int j = place_enemy(s, 100.0f, 400.0f);
        check(j >= 0, "enemy placed");
        s->hp = 10.0f; s->max_hp = 30.0f; s->zone_double_lifesteal = 0;
        BtWProjArgs a = base_args(100.0f, 400.0f, 0.0f, 600.0f, 40.0f);
        a.rad = 44.0f; a.lifesteal = 1.0f;   /* randf() < 1.0 always succeeds */
        bt_wproj_spawn(s, a);
        float r = 0.0f;
        bt_wproj_step(s, &r);
        bt_wproj_step(s, &r);
        check_near(s->hp, 11.0f, 1e-4f, "lifesteal heals 1");
        check(s->player_lifesteal_cd > 0.0f, "the shared timer was armed");
    }

    /* --------------------------- weapon on-hit callbacks ------------------- */
    printf("[13] on_weapon_hit tally + break_on_hit (gold + owning-uid removal)\n");
    {
        int wb = -1, wb_chance = 0;
        for (int w = 0; w < BT_NWEAPONS && wb < 0; w++) {
            int f = (int)BT_WEAPONS[w].fx_first, c = (int)BT_WEAPONS[w].fx_count;
            for (int k = 0; k < c; k++) {
                int r = (int)BT_WEAPON_EFF_INDEX[f + k];
                if (r < 0 || r >= BT_NITEM_EFF_ROWS) continue;
                if (bt_eff_is(&BT_ITEM_EFF_ROWS[r], BT_ISTR_BREAK_ON_HIT)) {
                    wb = w; wb_chance = BT_ITEM_EFF_ROWS[r].value; break;
                }
            }
        }
        check(wb >= 0, "a weapon with a break_on_hit row exists");
        if (wb >= 0) {
            BtState* s = &A; fresh(s, 59);
            int j = place_enemy(s, 100.0f, 400.0f);
            check(j >= 0, "enemy placed");
            s->wpn_count = 1; s->wpn_id[0] = (unsigned short)wb; s->wpn_uid[0] = 77u;
            BtWProjArgs a = base_args(100.0f, 400.0f, 0.0f, 600.0f, 40.0f);
            a.rad = 44.0f; a.owner_uid = 77u; a.attack_id = 5;
            a.fx_first = BT_WEAPONS[wb].fx_first;
            a.fx_count = (unsigned char)BT_WEAPONS[wb].fx_count;
            bt_wproj_spawn(s, a);
            float r = 0.0f;
            bt_wproj_step(s, &r);
            bt_wproj_step(s, &r);
            check_eq_int(bt_wfx_attack_hits(&s->wfx, 77u, 5), 1, "per-attack hit tally");
            if (wb_chance >= 100) {
                check_eq_int((int)s->wpn_count, 0, "break removes the owning uid slot");
                int live = 0;
                for (int q = 0; q < BT_MAX_MAT; q++) live += s->matalive[q];
                check(live > 0, "break gold spawned");
            } else {
                check_eq_int((int)s->wpn_count, 1, "no break at chance < 100 kept the slot");
            }
        }
    }

    printf("[14] hit and slow caches follow source objects across actor-slot reuse\n");
    {
        BtState* s = &A; fresh(s, 61);
        BtWProjArgs a = base_args(100.0f, 400.0f, 0.0f, 0.0f, 40.0f);
        a.piercing = 4;
        a.flags |= BT_WPROJ_F_SLOW;
        a.slow_rad = 250.0f; a.slow_amt = -200.0f;
        int shot = bt_wproj_spawn(s, a);
        int first = place_enemy(s, 100.0f, 400.0f);
        unsigned int original = bt_wproj_eid(s, first);
        float reward = 0.0f, hp = s->ehp[first];
        bt_wproj_step(s, &reward);  /* the step: the overlap is paired */
        bt_wproj_step(s, &reward);  /* the next flush: the hurt runs */
        check(s->ehp[first] < hp, "first object takes projectile damage");
        check(s->edecay[first] < 0.0f, "first object takes the separate slow");
        bt_mark_enemy_dead(s, first, 0);
        int replacement = place_enemy(s, 100.0f, 400.0f);
        /* A dying node cannot supply a new identity before it enters the pool. */
        check(bt_wproj_eid(s, replacement) != original, "fresh node has a different identity");
        hp = s->ehp[replacement];
        bt_wproj_step(s, &reward);  /* pairs with the NEW object in the same slot */
        bt_wproj_step(s, &reward);  /* and hurts it on the next flush */
        check(s->ehp[replacement] < hp, "new object in its slot is damaged");
        check(s->edecay[replacement] < 0.0f, "new object in its slot is slowed");
        for (int t = 0; t < BT_POOL_DEATH_FRAMES; t++) {
            bt_enemy_pool_step(s);
            bt_enemy_dying_step(s);
        }
        bt_mark_enemy_dead(s, replacement, 0);
        int respawned = place_enemy(s, 100.0f, 400.0f);
        check(bt_wproj_eid(s, respawned) == original, "mature pooled node retains its identity");
        hp = s->ehp[respawned];
        /* two frames: a pair WOULD have been queued and resolved by now */
        bt_wproj_step(s, &reward);
        bt_wproj_step(s, &reward);
        check_near(s->ehp[respawned], hp, 1e-6f, "same pooled object is not damaged twice");
        check_near(s->edecay[respawned], 0.0f, 1e-6f, "same pooled object is not slowed twice");
        bt_wproj_return(s, shot);
        shot = bt_wproj_spawn(s, a);
        bt_wproj_step(s, &reward);
        bt_wproj_step(s, &reward);
        check(s->ehp[respawned] < hp, "a returned projectile clears its object history");
        check(s->edecay[respawned] < 0.0f, "a returned projectile clears its slow history");
    }

    printf("[15] one piercing shot remembers more historical targets than live actor slots\n");
    {
        BtState* s = &A; fresh(s, 67);
        BtWProjArgs a = base_args(100.0f, 400.0f, 0.0f, 0.0f, 40.0f);
        a.piercing = BT_MAX + 20;
        int shot = bt_wproj_spawn(s, a);
        float reward = 0.0f;
        bt_wproj_step(s, &reward);
        int second_kind = -1;
        for (int k = 1; k < BT_NKINDS; k++) {
            if (BT_KIND_CLASS[k] == 0 && BT_KIND_SCENE[k] != BT_KIND_SCENE[0]) {
                second_kind = k;
                break;
            }
        }
        check(second_kind >= 0, "a different poolable scene is available");
        int damaged = 0;
        for (int n = 0; second_kind >= 0 && n < BT_MAX + 10; n++) {
            if (n == BT_MAX) {
                /* Mature the first scene's dying nodes before reusing storage.
                 * Another scene cannot reuse their pooled identities. */
                for (int t = 0; t < BT_POOL_DEATH_FRAMES; t++) {
                    bt_enemy_pool_step(s);
                    bt_enemy_dying_step(s);
                }
            }
            int kind = n < BT_MAX ? 0 : second_kind;
            int enemy = place_enemy_kind(s, kind, 100.0f, 400.0f);
            if (enemy < 0) {
                check(0, "a target fits after its predecessor's lifecycle retires");
                break;
            }
            float hp = s->ehp[enemy];
            bt_wproj_step(s, &reward);   /* this frame's step pairs the overlap */
            bt_wproj_step(s, &reward);   /* the next frame's flush runs the hurt */
            damaged += s->ehp[enemy] < hp;
            bt_mark_enemy_dead(s, enemy, 0);
        }
        check_eq_int(damaged, BT_MAX + 10, "each distinct source object receives its hit");
        check(s->wproj.p[shot].alive && !s->wproj.p[shot].ign_overflow,
              "historical targets do not overflow at the simultaneous-live actor bound");
    }

    /* ---------------------------------------------- crit roll semantics */
    printf("[16] the projectile crit roll is Utils.get_chance_success(crit_chance)\n");
    {
        /* unit.gd:280 + utils.gd:397-403: `randf() <= crit_chance` (and the
         * `chance == 0` short circuit draws nothing).  The flush that runs the
         * deferred hurt consumes a fixed global-draw sequence whose prefix is the
         * same for every stream state, so the crit verdict must equal
         * `randf() <= 0.5f` evaluated at ONE fixed offset k for every seed --
         * while the 1000-quantized surrogate this replaced (raw % 1000 * 0.001 <
         * chance) agrees only ~half the time.  The test therefore derives k
         * instead of assuming it. */
        enum { NSEED = 24, NOFF = 8 };
        int matches[NOFF];
        for (int k = 0; k < NOFF; k++) matches[k] = 0;
        int cases = 0;
        for (int t = 0; t < NSEED; t++) {
            BtState* s = &A; fresh(s, (bt_u64)(41 + t * 7));
            int j = place_enemy(s, 100.0f, 400.0f);
            if (j < 0) { check(0, "enemy placed"); break; }
            BtWProjArgs a = base_args(100.0f, 400.0f, 0.0f, 600.0f, 40.0f);
            a.rad = 44.0f;
            a.crit_chance = 0.5f;
            a.crit_dmg = 2.0f;
            int i = bt_wproj_spawn(s, a);
            float r = 0.0f;
            /* the spawn frame draws nothing (see [1]) and only PAIRS the shot;
             * the hurt's rolls all live in the NEXT frame's flush. */
            check_eq_int((int)s->hitq.n, 1, "the spawn frame pairs the shot");
            BtRng snap = s->rng;
            int want[NOFF];
            for (int k = 0; k < NOFF; k++) {
                BtRng q = snap;
                for (int d = 0; d < k; d++) (void)bt_rng_next(&q);
                want[k] = (bt_math_randf(&q) <= 0.5f) ? 1 : 0;
            }
            bt_wproj_step(s, &r);                       /* the flush: hurt runs */
            int got = (int)s->wproj.p[i].hit_crit;
            check_eq_int((int)s->wproj.p[i].n_hits, 1, "the shot hit once");
            cases++;
            for (int k = 0; k < NOFF; k++) if (got == want[k]) matches[k]++;
        }
        int best = 0;
        for (int k = 0; k < NOFF; k++) if (matches[k] > best) best = matches[k];
        check_eq_int(best, cases,
                     "the crit verdict is randf() <= crit_chance at ONE fixed draw offset");
    }

    /* an EXPLODED hit still runs the weapon hit callbacks */
    printf("[17] an exploded hit still runs on_weapon_hit_something (unit.gd:608)\n");
    {
        int wf = find_weapon_with_exploding_row(0);
        check(wf >= 0, "a weapon with a guaranteed ExplodingEffect exists");
        if (wf >= 0) {
            BtState* s = &A; fresh(s, 43);
            bt_weapon_add(s, (unsigned short)wf, 0, 0.0f);   /* owning copy (uid 0) */
            s->wpn_cd[0] = 0.0f;
            int j = place_enemy(s, 100.0f, 400.0f);
            float hp0 = s->ehp[j];
            BtWProjArgs a = base_args(100.0f, 400.0f, 0.0f, 600.0f, 40.0f);
            a.rad = 44.0f;
            a.fx_first = BT_WEAPONS[wf].fx_first;
            a.fx_count = (unsigned char)BT_WEAPONS[wf].fx_count;
            a.attack_id = 5;
            a.owner_uid = bt_weapon_uid(s, 0);
            int i = bt_wproj_spawn(s, a);
            float r = 0.0f;
            bt_wproj_step(s, &r);
            bt_wproj_step(s, &r);
            check_eq_int((int)s->expl.n_spawned, 1, "the explosion replaced the direct hit");
            check_near(s->ehp[j], hp0, 0.01f, "the target took no direct damage");
            check_near(s->wproj.p[i].hit_dmg, 0.0f, 1e-6f, "emitted damage is 0");
            check_eq_int(bt_wfx_attack_hits(&s->wfx, a.owner_uid, 5), 1,
                         "hitbox.hit_something runs unconditionally, so the HIT tally does too");
        }
    }

    /* unit.gd:581 burn BEFORE unit.gd:285 take_damage */
    printf("[18] the burn roll applies before the damage/crit step (unit.gd:581)\n");
    {
        int wb = -1;
        for (int w = 0; w < BT_NWEAPONS && wb < 0; w++)
            for (int k = 0; k < (int)BT_WEAPONS[w].fx_count; k++) {
                int r = (int)BT_WEAPON_EFF_INDEX[(int)BT_WEAPONS[w].fx_first + k];
                if (r < 0 || r >= BT_NITEM_EFF_ROWS) continue;
                if (bt_eff_is(&BT_ITEM_EFF_ROWS[r], BT_ISTR_CRIT_ON_HITTING_BURNING_TARGET)) { wb = w; break; }
            }
        check(wb >= 0, "a crit_on_hitting_burning_target row exists (DLC spoon)");
        if (wb >= 0) {
            BtState* s = &A; fresh(s, 47);
            int j = place_enemy(s, 100.0f, 400.0f);
            BtWProjArgs a = base_args(100.0f, 400.0f, 0.0f, 600.0f, 40.0f);
            a.rad = 44.0f;
            a.fx_first = BT_WEAPONS[wb].fx_first;
            a.fx_count = (unsigned char)BT_WEAPONS[wb].fx_count;
            a.burn_chance = 1.0f; a.burn_dmg = 1.0f; a.burn_dur = 60.0f;
            a.crit_chance = 0.0f;               /* only the burning target may crit */
            int i = bt_wproj_spawn(s, a);
            float r = 0.0f;
            bt_wproj_step(s, &r);
            bt_wproj_step(s, &r);
            check(s->eburn[j] > 0.0f, "the burn was applied on the hit");
            check_eq_int((int)s->wproj.p[i].hit_crit, 1,
                         "the target is already burning when the damage is rolled -> forced crit");
        }
    }

    /* ----------------------------------- weapon-sourced explosion credit */
    printf("[19] a weapon-sourced explosion kill credits the firing weapon (unit.gd:569)\n");
    {
        int wf = find_weapon_with_exploding_row(0);
        check(wf >= 0, "an exploding weapon exists");
        if (wf >= 0) {
            BtState* s = &A; fresh(s, 53);
            bt_weapon_add(s, (unsigned short)wf, 0, 0.0f);
            s->wpn_cd[0] = 0.0f;
            unsigned int uid = bt_weapon_uid(s, 0);
            int j = place_enemy(s, 100.0f, 400.0f);
            s->ehp[j] = 5.0f; s->emax[j] = 5.0f;   /* the explosion must be lethal */
            BtWProjArgs a = base_args(100.0f, 400.0f, 0.0f, 600.0f, 40.0f);
            a.rad = 44.0f;
            a.fx_first = BT_WEAPONS[wf].fx_first;
            a.fx_count = (unsigned char)BT_WEAPONS[wf].fx_count;
            a.attack_id = 7;
            a.owner_uid = uid;
            (void)bt_wproj_spawn(s, a);
            float r = 0.0f;
            bt_wproj_step(s, &r);
            bt_wproj_step(s, &r);
            check_eq_int((int)s->expl.n_spawned, 1, "the carried explosion spawned");
            check_eq_int((int)s->wfx.wave_kills[0], 0, "no kill before the explosion ticks");
            for (int t = 0; t < 8; t++) (void)bt_item_expl_tick(s, &r);
            check(s->ehp[j] <= 0.0f || s->ealive[j] == 0, "the explosion killed the target");
            check_eq_int((int)s->wfx.wave_kills[0], 1,
                         "the kill lands on the FIRING weapon's wave counter");
            /* an unattributed explosion (dead uid) credits nobody */
            BtState* t2 = &B;
            fresh(t2, 59);
            int j2 = place_enemy(t2, 100.0f, 400.0f);
            t2->ehp[j2] = 5.0f; t2->emax[j2] = 5.0f;
            BtWProjArgs b = base_args(100.0f, 400.0f, 0.0f, 600.0f, 40.0f);
            b.rad = 44.0f;
            b.fx_first = BT_WEAPONS[wf].fx_first;
            b.fx_count = (unsigned char)BT_WEAPONS[wf].fx_count;
            b.owner_uid = 0xFFFFFFFFu;               /* no live copy owns this */
            (void)bt_wproj_spawn(t2, b);
            bt_wproj_step(t2, &r);
            bt_wproj_step(t2, &r);
            for (int t = 0; t < 8; t++) (void)bt_item_expl_tick(t2, &r);
            check(t2->ehp[j2] <= 0.0f || t2->ealive[j2] == 0, "the unattributed explosion killed the target");
            check_eq_int((int)t2->wfx.wave_kills[0], 0, "no weapon owned -> no credit");
        }
    }

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}