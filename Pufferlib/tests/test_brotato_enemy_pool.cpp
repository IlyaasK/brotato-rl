/* tests/test_brotato_enemy_pool.cpp
 *
 * Behaviour tests for the source enemy-instance pool
 * (ocean/brotato/brotato_enemy_pool.h + the BtState pool fields in
 * ocean/brotato/brotato_state.h).
 *
 * Every expectation is transcribed from the recovered sources:
 *   global/entity_spawner.gd:346-420   fresh (scene.instance+init) vs reuse
 *                                      (respawn) branch of spawn_entity
 *   main.gd:56-61,1649-1699            `_pool` keyed by scene instance id
 *                                      (== one .tscn per BT_KINDS row); pop_back
 *                                      => LIFO reuse
 *   entities/entity.gd:79-100          death_animation_finished -> pool
 *   entities/entity.tscn:88-143        `death` method key at t=1.0 s
 *                                      (Entity.die forces playback_speed 1)
 *                                      => 60 physics frames
 *   boss.gd:47,81                      Boss.respawn() asserts; death -> queue_free
 *   main.gd:1304 + ui/menus/shop/base_shop.gd:346  a new Main per wave
 *   main.gd:1727-1737                  _exit_tree queue_frees the pool
 *
 * Build and run:
 *     c++ -O2 -o build/test_brotato_enemy_pool tests/test_brotato_enemy_pool.cpp -lm
 *     ./build/test_brotato_enemy_pool
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

static void check_near(float got, float want, float tol, const char* msg) {
    g_checks++;
    float d = got - want;
    if (d < 0.0f) d = -d;
    if (!(d <= tol)) {
        g_failures++;
        printf("  FAIL: %s (got %.9g want %.9g tol %g)\n", msg, (double)got, (double)want, (double)tol);
    }
}

/* Persistent contact worlds belong in static storage, not the stack. */
static BtState A;

/* The pool's own classification: only plain enemies (BT_KIND_CLASS == 0) pool;
 * bosses/elites queue_free (boss.gd:81). Behaviour is probed through it rather
 * than by re-reading the class table here. */
static int poolable(int kind) { return bt_enemy_pool_poolable(kind); }

/* First poolable / non-poolable kind at or after `start`. */
static int find_poolable(int start) {
    for (int i = start; i < BT_NKINDS; i++) if (poolable(i)) return i;
    return -1;
}
static int find_nonpoolable(void) {
    for (int i = 0; i < BT_NKINDS; i++) if (!poolable(i)) return i;
    return -1;
}

static void steps(BtState* s, int n) { for (int i = 0; i < n; i++) bt_enemy_pool_step(s); }

static int sum_free(const BtState* s) {
    int t = 0;
    for (int k = 0; k < BT_NKINDS; k++) t += (int)s->pool_free_n[k];
    return t;
}

/* Death-animation delay is 60 frames (entity.tscn death method key @ t=1.0 s,
 * Entity.die forces playback_speed = 1). */
#define DEATH_FRAMES 60

/* utils.gd:349-366 get_rand_pos_from_direction_at_distance(dir, _zone_min_pos,
 * _zone_max_pos, Utils.EDGE_MAP_DIST) for _possible_edge_spawns ==
 * [TOP, LEFT, BOTTOM, RIGHT] indexed by Unit.randi() % 4
 * (global/entity_spawner.gd:49,474).  The fixed axis sits exactly at
 * EDGE_MAP_DIST and the varying axis spans [EDGE_MAP_DIST, zone - EDGE_MAP_DIST];
 * the group's spawn_dist_away_from_edges is NOT part of this expression
 * (entity_spawner.gd:470-477 never reads `d` in this branch). */
static void replay_edge_pos(BtState* s, float* x, float* y) {
    int dir = (int)bt_rng_bounded(&s->utils_rng, 4u);
    if (dir == 0) { *x = bt_rand_range(s, BT_EDGE_MAP_DIST, BT_ARENA_W - BT_EDGE_MAP_DIST); *y = BT_EDGE_MAP_DIST; }
    else if (dir == 1) { *x = BT_EDGE_MAP_DIST; *y = bt_rand_range(s, BT_EDGE_MAP_DIST, BT_ARENA_H - BT_EDGE_MAP_DIST); }
    else if (dir == 2) { *x = bt_rand_range(s, BT_EDGE_MAP_DIST, BT_ARENA_W - BT_EDGE_MAP_DIST); *y = BT_ARENA_H - BT_EDGE_MAP_DIST; }
    else { *x = BT_ARENA_W - BT_EDGE_MAP_DIST; *y = bt_rand_range(s, BT_EDGE_MAP_DIST, BT_ARENA_H - BT_EDGE_MAP_DIST); }
}

/* Source-faithful replay of every draw ONE elite/boss group consumes
 * (wave_manager.gd:180 + entity_spawner.gd:148-205).  It is used to prove the
 * implementation takes the same draws, in the same order, on the same two
 * streams: a wrong count would desync every later spawn of the run.
 * The elite/boss group resource (zones/common/elite/group_elite.tres, and the
 * final wave's own group_1.tres) exports area = -1,
 * spawn_dist_away_from_edges = 0, spawn_edge_of_map = true, spawn_chance = 1.0,
 * and each WaveUnitData it receives has min_number == max_number == 1,
 * spawn_chance = 1.0 -- so number_total is exactly 1.0 and the residual
 * Utils.get_chance_success(0.0) returns false with NO draw (utils.gd:397-401). */
static void replay_elite_group(BtState* s, float* ox, float* oy) {
    (void)bt_chance(s, 1.0f);                     /* spawn_chance roll */
    float bx = bt_rand_range(s, 0.0f, BT_ARENA_W);/* get_group_pos: get_rand_pos(0) */
    float by = bt_rand_range(s, 0.0f, BT_ARENA_H);
    int min_dist = (int)BT_SPAWN_MIN_DIST + (-1) / 2;   /* 300 + area/2, area == -1 */
    int half_zone = (int)((BT_ARENA_W > BT_ARENA_H ? BT_ARENA_W : BT_ARENA_H) * 0.5f);
    if (min_dist > half_zone) min_dist = half_zone;
    for (;;) {
        float dx = bx - s->px, dy = by - s->py;
        if (dx * dx + dy * dy >= (float)min_dist * (float)min_dist) break;
        bx = bt_rand_range(s, 0.0f, BT_ARENA_W);
        by = bt_rand_range(s, 0.0f, BT_ARENA_H);
        min_dist = min_dist > 30 ? min_dist - 5 : 25;
        if (min_dist == 25) break;
    }
    (void)bt_rng_randi_range(&s->utils_rng, 1, 1);   /* Utils.randi_range(1,1) */
    float x, y;
    replay_edge_pos(s, &x, &y);
    int md = (int)BT_SPAWN_MIN_DIST;                  /* unit distance 0 */
    for (;;) {
        float dx = x - s->px, dy = y - s->py;
        if (dx * dx + dy * dy >= (float)md * (float)md) break;
        replay_edge_pos(s, &x, &y);
        md = md > 30 ? md - 5 : 25;
        if (md == 25) break;
    }
    *ox = x; *oy = y;
}

/* Number of global-stream draws consumed between `before` and `after`, by
 * advancing a copy of the stream one draw at a time (PCG32 state is a counter,
 * so equal state == equal stream position).  Returns -1 if the states do not
 * meet within `limit` draws. */
static int global_draws_between(BtRng before, const BtRng* after, int limit) {
    for (int n = 0; n <= limit; n++) {
        if (before.state == after->state) return n;
        (void)bt_rng_next(&before);
    }
    return -1;
}

int main(void) {
    const int ke = find_poolable(0);
    const int k2 = (ke >= 0) ? find_poolable(ke + 1) : -1;
    const int kx = find_nonpoolable();
    if (ke < 0 || k2 < 0 || kx < 0) {
        printf("test_brotato_enemy_pool: missing kinds (ke=%d k2=%d kx=%d)\n", ke, k2, kx);
        printf("\n%d checks, %d failures\n", g_checks, g_failures);
        return 1;
    }

    printf("[1] fresh until the death animation ends, reuse after (entity.gd:79, entity.tscn t=1.0)\n");
    {
        int fresh = -1;
        unsigned short id0, id1, id2;
        bt_enemy_pool_reset(&A);
        id0 = bt_enemy_pool_acquire(&A, ke, &fresh);
        check(fresh == 1, "first spawn is fresh (no free instance)");
        check(id0 != (unsigned short)BT_POOL_NO_INST, "fresh spawn returns a real instance id");
        check(A.pool_free_n[ke] == 0, "a fresh spawn creates no free instance");

        bt_enemy_pool_release(&A, id0);
        check(A.pool_free_n[ke] == 0, "a dying instance is not reusable immediately");

        steps(&A, DEATH_FRAMES - 1);
        check(A.pool_free_n[ke] == 0, "still animating one frame before the method key");
        id1 = bt_enemy_pool_acquire(&A, ke, &fresh);
        check(fresh == 1, "before 60 frames the spawn is still fresh");
        check(id1 != id0, "the fresh instance is not the still-animating one");

        bt_enemy_pool_step(&A);
        check(A.pool_free_n[ke] == 1, "the instance returns to the pool on frame 60");
        id2 = bt_enemy_pool_acquire(&A, ke, &fresh);
        check(fresh == 0, "the returned instance is reused");
        check(id2 == id0, "reuse returns the SAME instance identity");
        check(A.pool_free_n[ke] == 0, "reuse consumes the free instance");
        bt_enemy_pool_acquire(&A, ke, &fresh);
        check(fresh == 1, "with the pool empty the next spawn is fresh again");
        check(A.pool_overflow == 0, "no overflow on the normal path");
        bt_enemy_pool_release(&A, id2);          /* the reused instance dies again */
        steps(&A, DEATH_FRAMES - 1);
        check(A.pool_free_n[ke] == 0, "a reused instance re-pools on its own 60-frame cycle");
        bt_enemy_pool_step(&A);
        check(A.pool_free_n[ke] == 1, "it is reusable again exactly 60 frames later");
    }

    printf("[2] reuse is LIFO: the last released instance is handed back first\n");
    {
        int fresh = -1;
        unsigned short ia, ib, ic, g1, g2, g3;
        bt_enemy_pool_reset(&A);
        ia = bt_enemy_pool_acquire(&A, ke, &fresh);
        ib = bt_enemy_pool_acquire(&A, ke, &fresh);
        ic = bt_enemy_pool_acquire(&A, ke, &fresh);
        bt_enemy_pool_release(&A, ia);
        bt_enemy_pool_release(&A, ib);
        bt_enemy_pool_release(&A, ic);
        steps(&A, DEATH_FRAMES);
        check(A.pool_free_n[ke] == 3, "all three deaths matured");
        g1 = bt_enemy_pool_acquire(&A, ke, &fresh);
        g2 = bt_enemy_pool_acquire(&A, ke, &fresh);
        g3 = bt_enemy_pool_acquire(&A, ke, &fresh);
        check(fresh == 0 && g1 == ic, "first pop is the last released (ic)");
        check(fresh == 0 && g2 == ib, "second pop is ib");
        check(fresh == 0 && g3 == ia, "third pop is ia (pop_back order)");
    }

    printf("[3] per-scene separation: pools are keyed by scene == kind (entity_spawner.gd:350)\n");
    {
        int fresh = -1;
        unsigned short ia, ib, g;
        bt_enemy_pool_reset(&A);
        ia = bt_enemy_pool_acquire(&A, ke, &fresh);
        ib = bt_enemy_pool_acquire(&A, k2, &fresh);
        bt_enemy_pool_release(&A, ia);
        bt_enemy_pool_release(&A, ib);
        steps(&A, DEATH_FRAMES);
        check(A.pool_free_n[ke] == 1 && A.pool_free_n[k2] == 1, "one free instance per kind");
        g = bt_enemy_pool_acquire(&A, ke, &fresh);
        check(fresh == 0 && g == ia, "kind ke reuses its own instance");
        check(A.pool_free_n[ke] == 0 && A.pool_free_n[k2] == 1, "k2's instance was not stolen");
        bt_enemy_pool_acquire(&A, ke, &fresh);
        check(fresh == 1, "an empty ke pool spawns fresh even with k2 free");
        check(A.pool_free_n[k2] == 1, "k2's free instance is untouched");
        g = bt_enemy_pool_acquire(&A, k2, &fresh);
        check(fresh == 0 && g == ib, "kind k2 reuses its own instance");
    }

    printf("[4] staggered releases mature on their own exact 60-frame offsets\n");
    {
        int fresh = -1;
        unsigned short ia, ib, g;
        bt_enemy_pool_reset(&A);
        ia = bt_enemy_pool_acquire(&A, ke, &fresh);
        bt_enemy_pool_release(&A, ia);                 /* death at frame 0 */
        steps(&A, 2);                                  /* two frames pass   */
        ib = bt_enemy_pool_acquire(&A, ke, &fresh);
        bt_enemy_pool_release(&A, ib);                 /* death at frame 2  */
        steps(&A, DEATH_FRAMES - 1 - 2);               /* 59 steps after frame 0 */
        check(A.pool_free_n[ke] == 0, "neither death is due yet");
        bt_enemy_pool_step(&A);                        /* frame 60 after the first death */
        check(A.pool_free_n[ke] == 1, "the frame-0 death matured at 60 frames");
        g = bt_enemy_pool_acquire(&A, ke, &fresh);
        check(fresh == 0 && g == ia, "frame-0 instance reused");
        bt_enemy_pool_step(&A);
        check(A.pool_free_n[ke] == 0, "the frame-2 death is not due yet (59 frames)");
        bt_enemy_pool_step(&A);
        check(A.pool_free_n[ke] == 1, "the frame-2 death matures exactly two frames later");
        g = bt_enemy_pool_acquire(&A, ke, &fresh);
        check(fresh == 0 && g == ib, "frame-2 instance reused");
    }

    printf("[5] bosses and elites never pool; their descriptor recycles fresh (boss.gd:47,81)\n");
    {
        int fresh = -1;
        unsigned short id1, id2;
        bt_enemy_pool_reset(&A);
        check(!poolable(kx), "the chosen kind is not poolable");
        id1 = bt_enemy_pool_acquire(&A, kx, &fresh);
        check(fresh == 1, "a boss/elite spawn is always fresh");
        check(id1 != (unsigned short)BT_POOL_NO_INST, "a boss/elite gets a descriptor");
        bt_enemy_pool_release(&A, id1);
        steps(&A, DEATH_FRAMES * 2);
        check(sum_free(&A) == 0, "a boss/elite death never enters any free stack");
        check(A.pool_free_n[kx] == 0, "no free instance for the non-poolable kind");
        id2 = bt_enemy_pool_acquire(&A, kx, &fresh);
        check(fresh == 1, "a boss/elite respawn is still fresh (never a reuse)");
        check(id2 == id1, "the queue_free descriptor was recycled");
    }

    printf("[6] a new Main resets the pool (main.gd:1304,1727 -> bt_enter_wave)\n");
    {
        int fresh = -1;
        unsigned short id;
        bt_enemy_pool_reset(&A);
        id = bt_enemy_pool_acquire(&A, ke, &fresh);
        bt_enemy_pool_release(&A, id);                 /* in flight  */
        steps(&A, DEATH_FRAMES);
        check(A.pool_free_n[ke] == 1, "precondition: one free instance");
        id = bt_enemy_pool_acquire(&A, ke, &fresh);
        bt_enemy_pool_release(&A, id);                 /* another in flight */
        steps(&A, 5);
        A.pool_overflow = 1;                           /* pretend a saturation happened */
        A.e_instance[0] = id;
        bt_enemy_pool_reset(&A);                       /* new Main / wave start */
        check(sum_free(&A) == 0, "every kind's free stack cleared");
        check(A.pool_pos == 0, "ring cursor reset");
        check(A.pool_alloc == 0, "instance allocator reset");
        check(A.pool_np_free == (unsigned short)BT_POOL_NO_INST, "boss/elite recycle cleared");
        check(A.pool_overflow == 0, "overflow flag cleared");
        check(A.e_instance[0] == (unsigned short)BT_POOL_NO_INST, "live-slot links neutralised");
        steps(&A, DEATH_FRAMES * 2);
        check(sum_free(&A) == 0, "in-flight deaths were dropped, nothing resurrects");
        id = bt_enemy_pool_acquire(&A, ke, &fresh);
        check(fresh == 1, "the first spawn after a new Main is fresh");
        check(id == 0, "the id space restarted");
    }

    printf("[6b] state-node banks are boss-only and retain the full live-slot bound\n");
    {
        for (int k = 0; k < BT_NKINDS; k++)
            check(!poolable(k) || BT_NKIND_STATES[k] == 0,
                  "pooled source scenes have only the root MovementBehavior");
        bt_enemy_pool_reset(&A);
        int fresh = 0, kind = find_nonpoolable();
        unsigned short ids[BT_MAX];
        for (int i = 0; i < BT_MAX; i++) {
            ids[i] = bt_enemy_pool_acquire(&A, kind, &fresh);
            check(ids[i] != BT_POOL_NO_INST && fresh, "all live boss slots fit");
            check(A.pool_mv_state[ids[i]] == i, "each boss gets an independent bank");
            for (int n = 0; n < BT_MAX_STATES; n++)
                *bt_pool_mvtx(&A, ids[i], n) = (float)(i * BT_MAX_STATES + n);
        }
        for (int i = 0; i < BT_MAX; i++)
            for (int n = 0; n < BT_MAX_STATES; n++)
                check(*bt_pool_mvtx(&A, ids[i], n) == (float)(i * BT_MAX_STATES + n),
                      "every boss/state combination remains distinct");
        bt_enemy_pool_release(&A, ids[BT_MAX - 1]);
        unsigned short recycled = bt_enemy_pool_acquire(&A, kind, &fresh);
        check(recycled == ids[BT_MAX - 1] && fresh, "fresh boss reuses its descriptor/bank");
        check(A.pool_mv_state_n == BT_MAX && !A.pool_overflow,
              "descriptor reuse needs no extra bank and cannot overflow");
        bt_enemy_pool_reset(&A);
        check(A.pool_mv_state_n == 0, "new Main resets the bank allocator");
        unsigned short normal = bt_enemy_pool_acquire(&A, find_poolable(0), &fresh);
        check(A.pool_mv_state[normal] == BT_POOL_NO_INST && A.pool_mv_state_n == 0,
              "ordinary pooled enemies do not allocate nonexistent state nodes");
    }

    printf("[7] edge-of-map placement: the fixed axis is EDGE_MAP_DIST, the varying axis spans the\n"
           "    full zone and ignores spawn_dist_away_from_edges (entity_spawner.gd:470-477, utils.gd:349-366)\n");
    {
        /* zones/zone_1/019/d1_group_1.tres and d2_group_1.tres are the only
         * shipped groups with spawn_edge_of_map == true AND
         * spawn_dist_away_from_edges != 0 (both 100, min_difficulty 1/2, so both
         * fire at Danger 5 on wave 19). */
        bt_core_init(&A, 71u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        bt_rng_seed(&A.rng, 71u);
        BtSpawnGroup g;
        g.zone = 0; g.timing = 0; g.chance = 0.0f; g.repeat = 0;
        g.repeat_interval = 0.0f; g.reduce_interval = 0.0f; g.min_interval = 0.0f;
        g.area = -1; g.spawn_edge_of_map = 1; g.spawn_dist_away_from_edges = 100;
        g.is_neutral = 0; g.is_boss = 0; g.is_horde = 0; g.is_loot = 0;
        g.min_difficulty = 0; g.min_wave = 0; g.max_wave = 0;
        g.unit_first = 0; g.unit_count = 0; g.group_id = 0;
        int dirs[4] = { 0, 0, 0, 0 };
        int on_edge = 0, in_band = 0, off_edge = 0, x_low = 0, x_high = 0, y_out = 0;
        float xmin = 1e9f, xmax = -1e9f, ymin = 1e9f, ymax = -1e9f;
        for (int t = 0; t < 400; t++) {
            float x = 0.0f, y = 0.0f;
            bt_pos_in_area(&A, 0.0f, 0.0f, (float)g.area,
                           (float)g.spawn_dist_away_from_edges,
                           (int)g.spawn_edge_of_map, &x, &y);
            if (x == BT_EDGE_MAP_DIST) dirs[1]++;
            else if (x == BT_ARENA_W - BT_EDGE_MAP_DIST) dirs[3]++;
            else if (y == BT_EDGE_MAP_DIST) dirs[0]++;
            else if (y == BT_ARENA_H - BT_EDGE_MAP_DIST) dirs[2]++;
            else off_edge++;
            on_edge++;
            /* The x-varying directions (TOP/BOTTOM) must reach the full
             * [EDGE, W-EDGE] span; a `d`-narrowed range would clamp them to
             * [164, 1884].  The y-varying directions (LEFT/RIGHT) must leave
             * [164, 1372] -- which the d-narrowed range could never do. */
            if (y == BT_EDGE_MAP_DIST || y == BT_ARENA_H - BT_EDGE_MAP_DIST) {
                if (x < xmin) xmin = x;
                if (x > xmax) xmax = x;
                if (x < 164.0f) x_low++;
                if (x > 1884.0f) x_high++;
            } else {
                if (y < ymin) ymin = y;
                if (y > ymax) ymax = y;
                if (y < 164.0f || y > 1372.0f) y_out++;
            }
            if (x >= (float)BT_EDGE_MAP_DIST &&
                y >= (float)BT_EDGE_MAP_DIST &&
                x <= BT_ARENA_W - BT_EDGE_MAP_DIST &&
                y <= BT_ARENA_H - BT_EDGE_MAP_DIST) in_band++;
        }
        printf("      x-varying [%.3f, %.3f]  y-varying [%.3f, %.3f]\n",
               (double)xmin, (double)xmax, (double)ymin, (double)ymax);
        check(off_edge == 0, "every draw lands exactly on one of the four map edges");
        check(on_edge == 400, "all draws are edge draws");
        check(in_band == 400, "the varying axis stays inside [EDGE_MAP_DIST, zone - EDGE_MAP_DIST]");
        check(x_low > 0 && x_high > 0,
              "the x-varying axis leaves the d-narrowed [164,1884] window");
        check(y_out > 0, "the y-varying axis leaves the d-narrowed [164,1372] window (d is unused)");
        check(dirs[0] > 0 && dirs[1] > 0 && dirs[2] > 0 && dirs[3] > 0,
              "all four edge directions are reachable (Utils.randi() % 4)");
        /* golden value: the source range is rand_range(64, W-64) /
         * rand_range(64, H-64), NOT the d-narrowed
         * rand_range(64+100, W-64-100) / rand_range(64+100, H-64-100) (which
         * would turn this y into 355.0 at the same draw). */
        bt_rng_seed(&A.rng, 71u);
        bt_rng_seed(&A.utils_rng, 71u);
        float gx = 0.0f, gy = 0.0f;
        bt_pos_in_area(&A, 0.0f, 0.0f, -1.0f, 100.0f, 1, &gx, &gy);
        check_near(gx, 1984.0f, 1e-4f, "golden edge x (RIGHT edge) for the wave-19 d1 geometry");
        check_near(gy, 286.622650f, 1e-3f, "golden varying y from the FULL [64, H-64] span");
    }

    printf("[8] a scheduled elite spawns through the elite group: map edge, >= 300 from the player,\n"
           "    and exactly the source's draws in the source's order (entity_spawner.gd:148-205)\n");
    {
        int on_edge = 0, far = 0, replayed = 0, n = 0;
        float dmin = 1e9f;
        for (unsigned int seed = 1; seed <= 300u; seed++) {
            bt_core_init(&A, (bt_u64)seed, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            A.px = BT_ARENA_W * 0.5f;
            A.py = BT_ARENA_H * 0.5f;
            bt_u64 g0 = A.rng.state, u0 = A.utils_rng.state;
            A.n_pend_boss = 0;
            bt_spawn_boss_group(&A, (int)BT_ZONE_ELITES[A.zone][0], -1, 1);
            if (A.n_pend_boss != 1) continue;
            A.n_pend_boss--;
            float rx = A.pend_boss[0].x, ry = A.pend_boss[0].y;
            bt_u64 g1 = A.rng.state, u1 = A.utils_rng.state;
            n++;
            if (rx == BT_EDGE_MAP_DIST || rx == BT_ARENA_W - BT_EDGE_MAP_DIST ||
                ry == BT_EDGE_MAP_DIST || ry == BT_ARENA_H - BT_EDGE_MAP_DIST) on_edge++;
            float dx = rx - A.px, dy = ry - A.py;
            float d = sqrtf(dx * dx + dy * dy);
            if (d < dmin) dmin = d;
            if (d >= BT_SPAWN_MIN_DIST) far++;
            /* replay on copies: the same two streams must end in the same state */
            A.rng.state = g0; A.utils_rng.state = u0;
            float px = 0.0f, py = 0.0f;
            replay_elite_group(&A, &px, &py);
            if (A.rng.state == g1 && A.utils_rng.state == u1 && px == rx && py == ry) replayed++;
        }
        check(n == 300, "the elite group queues exactly one boss per scheduled elite");
        check(on_edge == n, "every elite spawns exactly on a map edge");
        check(far == n, "every elite is at least INITIAL_MIN_DIST_FROM_PLAYER (300) away");
        check(replayed == n, "the replay consumes the same draws in the same order on both streams");
        check(dmin >= BT_SPAWN_MIN_DIST, "the closest elite respected the 300 radius");
    }

    printf("[9] live-group order matches wave_manager.gd:init (wave, all-waves, item extras,\n"
           "    horde block, DLC all-zone last)\n");
    {
        bt_core_init(&A, 3u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.wave = 15;                                   /* a horde-capable wave */
        A.elite_wave[0] = 15; A.elite_type[0] = 1;     /* HORDE event */
        A.elite_wave[1] = 0; A.elite_type[1] = 0;
        A.elite_wave[2] = 0; A.elite_type[2] = 0;
        A.n_exg_active = 0;
        bt_build_live_groups(&A);
        int i_wave_last = -1, i_allwave_lo = -1, i_horde_lo = -1, i_allzone = -1, i_horde_hi = -1;
        for (int i = 0; i < A.n_live_groups; i++) {
            unsigned short gi = A.live_group[i];
            if (gi >= 120 && gi <= 124) { if (i_horde_lo < 0) i_horde_lo = i; i_horde_hi = i; }
            if (gi == BT_ZONE_ALLZONE_GROUPS[0][0]) i_allzone = i;
            if (gi == BT_ZONE_ALLWAVE_GROUPS[0][0]) i_allwave_lo = i;
            if (gi < 120) i_wave_last = i;
        }
        check(i_horde_lo > i_allwave_lo, "the horde block follows groups_data_in_all_waves");
        check(i_horde_lo > i_wave_last, "the horde block follows the wave's own groups");
        check(i_allzone > i_horde_hi, "DLC all-zone groups are appended after the horde block");
        check(A.n_live_groups == 8 + 3 + 5 + 1, "wave 15 = 8 own + 3 all-wave + 5 horde + 1 all-zone");
    }

    printf("[10] a repeating group rolls spawn_chance ONCE: repeats emit without one\n"
           "     (wave_manager.gd:171-181 first reach vs :197-215 add/emit repeat)\n");
    {
        /* Wave 1's own group (zones/zone_1/001/group_1.tres) is timing 1,
         * repeating 999, interval 3, spawn_chance 1.0, area -1, dist 0.  Both
         * fires below run on an identically seeded stream, so they consume the
         * same geometry/count draws; only the first-reach spawn_chance roll may
         * differ -- it must be exactly one extra draw. */
        bt_core_init(&A, 11u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_exg_active = 0;
        bt_build_live_groups(&A);
        unsigned short gi = A.live_group[0];
        A.n_live_groups = 1;
        A.live_group[0] = gi;
        BtRng g0;
        int first_draws, repeat_draws;
        for (int i = 0; i < BT_MAX_LIVE_GROUPS; i++) {
            A.group_fired[i] = 0; A.group_repeating[i] = 0;
            A.group_interval[i] = 0.0f; A.group_next_elapsed[i] = 0.0f; A.live_mult[i] = 1;
        }
        bt_rng_seed(&A.rng, 99u);
        bt_rng_seed(&A.utils_rng, 99u);
        g0 = A.rng;
        A.n_pend_enemy = 0;
        bt_fire_groups(&A, 1.0f);
        first_draws = global_draws_between(g0, &A.rng, 4096);
        check(A.group_fired[0] == 1 && A.group_repeating[0] > 0,
              "the first reach marks the group fired and arms its repeats");
        int queued_first = A.n_pend_enemy;
        check(queued_first >= 4, "the first reach queues the 4..5 baby aliens");

        for (int i = 0; i < BT_MAX_LIVE_GROUPS; i++) {
            A.group_fired[i] = 0; A.group_repeating[i] = 0;
            A.group_interval[i] = 0.0f; A.group_next_elapsed[i] = 0.0f; A.live_mult[i] = 1;
        }
        A.group_fired[0] = 1;              /* already fired, repeat due now */
        A.group_repeating[0] = 5;
        A.group_interval[0] = 3.0f;
        A.group_next_elapsed[0] = 0.0f;
        bt_rng_seed(&A.rng, 99u);
        bt_rng_seed(&A.utils_rng, 99u);
        g0 = A.rng;
        A.n_pend_enemy = 0;
        bt_fire_groups(&A, 1.0f);
        repeat_draws = global_draws_between(g0, &A.rng, 4096);
        check(A.n_pend_enemy == queued_first,
              "a repeat queues the same batch (same seeded geometry)");
        check(first_draws > 0 && repeat_draws > 0, "both fires draw from the global stream");
        check(first_draws == repeat_draws + 1,
              "the first reach costs exactly one extra global draw (the spawn_chance roll)");

        /* And a group that already repeated to exhaustion stops firing. */
        A.group_repeating[0] = 1;
        A.group_next_elapsed[0] = 0.0f;
        A.n_pend_enemy = 0;
        bt_fire_groups(&A, 1.0f);
        check(A.group_repeating[0] == 0 && A.n_pend_enemy > 0, "the last repeat still spawns");
        A.n_pend_enemy = 0;
        bt_fire_groups(&A, 100.0f);
        check(A.n_pend_enemy == 0, "an exhausted repeat counter stops firing");
    }

    printf("[11] endless waves: base wave, max_enemies/duration scaling, generated groups\n"
           "     (zone_service.gd:40-92)\n");
    {
        bt_core_init(&A, 21u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        /* authored waves are untouched */
        check(bt_wave_duration(0, 20) == 90.0f, "wave 20 keeps its 90 s duration");
        check(bt_wave_duration(0, 19) == 60.0f, "wave 19 keeps its 60 s duration");
        check(bt_wave_duration(0, 21) == (float)BT_ENDLESS_DURATION, "wave 21 is 60 s");
        check(bt_wave_duration(0, 200) == (float)BT_ENDLESS_DURATION, "wave 200 is 60 s");

        /* wave 21: wi = (21-1) % 20 = 0 -> +10 = 10 -> authored wave 11 */
        A.wave = 21;
        A.n_exg_active = 0;
        A.elite_wave[0] = A.elite_wave[1] = A.elite_wave[2] = 0;
        A.elite_type[0] = A.elite_type[1] = A.elite_type[2] = 0;
        bt_u64 g0 = A.rng.state, u0 = A.utils_rng.state;
        bt_build_live_groups(&A);
        bt_u64 g1 = A.rng.state, u1 = A.utils_rng.state;
        BT_CONST_PTR(BtWave) base = &BT_ZONE_WAVES[0][10];
        check(A.group_overflow == 0, "wave 21 fits the generated-group bound");
        check(A.wave_max_enemies == (unsigned short)(int)(100.0f * (1.25f + 10.0f * 0.01f)),
              "wave 21 max_enemies = int(100 * (1.25 + 10*0.01)) = 135");
        int nb = (int)(21.0f * 0.2f);
        check(nb == 4, "wave 21 asks for int(21/10*2) = 4 generated groups");
        int dyn = 0;
        for (int i = 0; i < A.n_live_groups; i++) if (A.lg_dynamic[i]) dyn++;
        check(dyn == nb, "exactly 4 live slots are generated groups");
        check(A.n_live_groups == (int)base->n_groups + nb +
                                   (int)BT_ZONE_ALLWAVE_GROUPS_N[0] + 1,
              "live list = base wave 11 groups + 4 generated + all-waves + all-zone (no horde event)");
        /* the same wave with a HORDE event also carries the zone horde groups */
        A.elite_wave[0] = 21;
        A.elite_type[0] = 1;
        bt_build_live_groups(&A);
        check(A.n_live_groups == (int)base->n_groups + nb +
                                   (int)BT_ZONE_ALLWAVE_GROUPS_N[0] +
                                   (int)BT_ZONE_HORDE_GROUPS_N[0] + 1,
              "an endless horde wave keeps every horde group");
        A.elite_wave[0] = 0; A.elite_type[0] = 0;
        A.wave = 21;
        bt_rng_seed(&A.rng, 21u); bt_rng_seed(&A.utils_rng, 21u);
        g0 = A.rng.state; u0 = A.utils_rng.state;
        bt_build_live_groups(&A);
        g1 = A.rng.state; u1 = A.utils_rng.state;
        /* first generated slot's source fields */
        int d0 = -1;
        for (int i = 0; i < A.n_live_groups; i++) if (A.lg_dynamic[i]) { d0 = i; break; }
        check(d0 == (int)base->n_groups, "generated groups follow the base wave's own groups");
        if (d0 >= 0) {
            float span = (float)(BT_ENDLESS_DURATION - 10) / (float)nb;
            for (int k = 0; k < nb; k++) {
                int g = d0 + k;
                int want_timing = (int)((float)k * span + 1.0f);
                check((int)A.lg_timing[g] == want_timing,
                      "generated spawn_timing = int(k*((60-10)/nb) + 1)");
                check(A.lg_area[g] == -1.0f || (A.lg_area[g] >= 200.0f && A.lg_area[g] < 300.0f),
                      "generated area is -1 or int(rand_range(200,300))");
                check(A.lg_interval[g] >= 6.0f && A.lg_interval[g] <= 8.0f,
                      "generated repeating_interval = int(round(rand_range(6,8)))");
                check(A.lg_edge[g] == 0 || A.lg_edge[g] == 1, "generated edge flag is a bool");
                int in_roster = 0;
                for (int r = 0; r < (int)BT_ZONE_NENDLESS_KINDS[0]; r++)
                    if ((int)BT_ZONE_ENDLESS_KINDS[0][r] == (int)A.lg_kind[g]) in_roster = 1;
                check(in_roster, "generated unit kind comes from the zone endless roster");
                check((int)A.lg_minn[g] == (int)(1.0f + (float)nb / 5.0f),
                      "generated min_number = int(1 + bonus_enemies_per_group)");
                check((int)A.lg_maxn[g] >= (int)A.lg_minn[g], "generated max_number >= min_number");
            }
        }
        /* draw-order replay on copies of both streams */
        {
            BtState B;
            B = A;                       /* copies the post-build state */
            B.rng.state = g0; B.utils_rng.state = u0;
            int nk = (int)BT_ZONE_NENDLESS_KINDS[0];
            float bonus = (float)nb / 5.0f;
            for (int i = 0; i < nb; i++) {
                (void)bt_rng_bounded(&B.rng, 1u);              /* endless_groups pick */
                float edge_rand = bt_math_randf(&B.rng);
                float area_rand = bt_math_randf(&B.rng);
                if (!(area_rand > 0.5f)) (void)bt_rand_range(&B, 200.0f, 300.0f);
                (void)bt_roundf(bt_rand_range(&B, 6.0f, 8.0f));
                (void)bt_rng_bounded(&B.rng, 1u);              /* endless_unit_groups pick */
                (void)bt_rng_bounded(&B.rng, (unsigned int)nk);
                int min_n = (int)(1.0f + bonus);
                (void)bt_roundf(bt_rand_range(&B, (float)min_n, (float)min_n + 1.0f));
                (void)edge_rand;
            }
            check(B.rng.state == g1 && B.utils_rng.state == u1,
                  "the generated groups consume exactly the source's draws in order");
        }
        /* deeper waves: wi = 19 -> 144 at wave 40; a 200-wave bound holds */
        A.wave = 40;
        A.group_overflow = 0;
        bt_build_live_groups(&A);
        check(A.wave_max_enemies == (unsigned short)(int)(100.0f * (1.25f + 19.0f * 0.01f)),
              "wave 40 max_enemies = 144");
        A.wave = 200;
        A.group_overflow = 0;
        bt_build_live_groups(&A);
        check(A.group_overflow == 0 && A.wave_max_enemies == 144,
              "wave 200 still fits and keeps the 144 cap");
        int dyn200 = 0;
        for (int i = 0; i < A.n_live_groups; i++) if (A.lg_dynamic[i]) dyn200++;
        check(dyn200 == 40, "wave 200 generates int(200/10*2) = 40 groups");
    }

    printf("[12] the per-group-event cull trims the PLAIN list to wave_max_enemies, one global draw each\n"
           "     (entity_spawner.gd:150-165)\n");
    {
        bt_core_init(&A, 31u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        int plain = find_poolable(0);
        int boss = find_nonpoolable();
        A.n_live_groups = 0;
        for (int i = 0; i < 5; i++) (void)bt_spawn_kind(&A, plain, 100.0f + (float)i, 100.0f);
        (void)bt_spawn_kind(&A, boss, 200.0f, 200.0f);            /* elite/boss: never culled */
        (void)bt_spawn_neutral(&A, 0, 300.0f, 300.0f);            /* neutral tree: never culled */
        int n_plain = 0, n_all = 0;
        for (int i = 0; i < BT_MAX; i++) {
            if (!A.ealive[i]) continue;
            n_all++;
            if (!A.eneutral[i] && A.eelite[i] == 0) n_plain++;
        }
        check(n_plain == 5, "precondition: 5 plain enemies alive");
        check(n_all == 7, "precondition: 5 plain + 1 boss + 1 tree");
        A.wave_max_enemies = 3;
        BtRng g0 = A.rng;
        int kills0 = A.kills;
        bt_group_perf_cull(&A);
        int draws = global_draws_between(g0, &A.rng, 4096);
        n_plain = n_all = 0;
        int n_boss = 0, n_tree = 0;
        for (int i = 0; i < BT_MAX; i++) {
            if (!A.ealive[i]) continue;
            n_all++;
            if (A.eneutral[i]) n_tree++;
            else if (A.eelite[i] != 0) n_boss++;
            else n_plain++;
        }
        check(n_plain == 3, "plain enemies trimmed to max_enemies exactly");
        check(A.kills - kills0 == 2, "exactly enemies.size() - max_enemies deaths");
        check(draws == 2, "one global draw per removal (Utils.get_rand_element)");
        check(n_boss == 1 && n_tree == 1, "bosses/elites and neutrals are never culled");
    }

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) printf("test_brotato_enemy_pool: OK\n");
    return g_failures ? 1 : 0;
}
