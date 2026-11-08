/* tests/test_brotato_enemy_motion.cpp
 *
 * Deterministic regression tests for the enemy MovementBehavior dispatch slice
 * (ocean/brotato/brotato_enemy_motion.h).  Self-contained: includes only the
 * core header and <stdio.h>.
 *
 * Build and run:
 *     c++ -O2 -o build/test_brotato_enemy_motion tests/test_brotato_enemy_motion.cpp -lm
 *     ./build/test_brotato_enemy_motion
 *
 * Exit code 0 = all checks passed.
 *
 * Every expectation is transcribed from the recovered sources:
 *   entities/units/movement_behaviors/*.gd   get_movement/get_target_position
 *   entities/units/unit/unit.gd:198-204      get_movement() dispatch
 *   entities/entity.gd:34-43                 _min_pos/_max_pos (sprite inset)
 *   entities/units/enemies/boss/boss.gd      _current_movement_behavior switch
 *   each kind .tscn                          MovementBehavior script + exports
 *
 * Coverage: the dispatch table vs the generated BT_ENEMY_INIT_OPS behaviour
 * provenance (metadata completeness); chase (follow_target) parity with the
 * previous universal chase; stay_in_range, follow_rand_pos_around_player
 * (allow_within on/off), target_rand_pos, target_rand_pos_around_player (lazy
 * redraw + update_every_x_frames timer), patrol_edges_of_map,
 * detect_target (range trigger, bonus_speed add, persistent switch),
 * go_towards_map_center, target_rand_gold_pos; boss/elite state switching with
 * per-state target isolation; pool reuse retaining MOV/or-in-node state while
 * Unit.bonus_speed resets; and the core-step integration of the movement vector.
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

/* BtState is large; keep it in static storage, never on the stack. */
static BtState A;

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

static void reset_all(BtState* s, bt_u64 seed) {
    bt_core_init(s, seed, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
    s->wave = 1;
    s->n_live_groups = 0;          /* spawn only what each case places */
    for (int i = 0; i < BT_MAX; i++) bt_enemy_set_alive(s, i, 0);
    s->ecount = 0;
    for (int i = 0; i < BT_MAX_MAT; i++) s->matalive[i] = 0;
    s->matcount = 0;
    s->px = 1024.0f; s->py = 768.0f;
}

/* Fixtures place already-settled units: the enemy's MovementBehavior reads
 * _parent.global_position during _physics_process, i.e. the pre-step transform
 * the rigid body held while the upcoming step was integrated (see the
 * bt_enemy_move comment in brotato_enemy_motion.h and unit.gd:214-215).  A unit
 * that has been sitting at (x,y) therefore has node transform == body position,
 * so ex/ey and ehx/ehy agree; every fixture that moves a unit keeps that
 * invariant through set_pos. */
static void set_pos(BtState* s, int slot, float x, float y) {
    s->ex[slot] = x; s->ey[slot] = y;
    s->ehx[slot] = x; s->ehy[slot] = y;
    s->phys.bodies[slot].pos = bt_v2(x, y);
}

static void put(BtState* s, int slot, int kind, float x, float y) {
    bt_enemy_set_alive(s, slot, 0); s->ecount = 0;
    bt_spawn_kind_at(s, slot, kind, x, y, 0);
    s->ehx[slot] = x; s->ehy[slot] = y;
}

/* Advance the deferred player-stat-sync pipeline by its two frame boundaries
 * (run_data.gd:221-222/1400-1406/1418-1427 -> main.gd:339 on_stats_updated ->
 * stats_manager.gd:105-126 reload_stats -> stats_manager.gd:20-31
 * player.update_player_stats, player.gd:349-368): a stat change only reaches
 * the player's LiveStats (max_stats.health, current_stats.speed/armor/dodge and
 * the heal) after both. */
static void sync_player_stats(BtState* s) {
    bt_player_stat_sync_step(s);
    bt_player_stat_sync_step(s);
}

/* bt_spawn_kind_at seeds ex/ey and the body; a fixture that spawns directly
 * keeps the settled node-transform invariant (ehx/ehy == ex/ey) through here. */
static void spawn_at(BtState* s, int slot, int kind, float x, float y) {
    bt_spawn_kind_at(s, slot, kind, x, y, 0);
    s->ehx[slot] = x; s->ehy[slot] = y;
}

/* Drop a *settled, monitorable* gold at (x,y) -- the push-back/idle phases are
 * already over, so the attract/pickup rules act on the next tick. */
static int place_gold(BtState* s, float x, float y, int value) {
    bt_spawn_material(s, x, y, value);
    for (int m = 0; m < BT_MAX_MAT; m++) {
        if (!s->matalive[m]) continue;
        if (s->matx[m] != x || s->maty[m] != y) continue;
        s->mat_arc[m][BT_LOOT_ARC_PUSH] = 0.0f;
        s->mat_arc[m][BT_LOOT_ARC_IDLE] = 0.0f;
        s->mat_arc[m][BT_LOOT_ARC_MON] = 1.0f;
        s->mat_arc[m][BT_LOOT_ARC_SPEED] = BT_LOOT_ITEM_ATTRACT_SPEED;
        return m;
    }
    return -1;
}

/* A real enemy death: return the instance to the pool, then clear the slot. */
static void kill_slot(BtState* s, int slot) {
    if (s->e_instance[slot] != BT_POOL_NO_INST)
        bt_enemy_pool_release(s, s->e_instance[slot]);
    s->e_instance[slot] = (unsigned short)BT_POOL_NO_INST;
    bt_enemy_set_alive(s, slot, 0);
    if (s->ecount > 0) s->ecount--;
    s->phys.bodies[slot].active = 0;
}

static int is_dispatch_beh(int b) {
    switch (b) {
    case BT_INITBEH_MOV_FOLLOW_TARGET:
    case BT_INITBEH_MOV_STAY_IN_RANGE:
    case BT_INITBEH_MOV_FOLLOW_RAND_AROUND:
    case BT_INITBEH_MOV_TARGET_RAND_POS:
    case BT_INITBEH_MOV_TARGET_RAND_AROUND:
    case BT_INITBEH_MOV_PATROL_EDGES:
    case BT_INITBEH_MOV_DETECT_TARGET:
    case BT_INITBEH_MOV_GO_MAP_CENTER:
    case BT_INITBEH_MOV_TARGET_RAND_GOLD:
    case BT_INITBEH_MOV_STARGAZER: return 1;
    default: return 0;
    }
}

int main(void) {
    const int chaser   = find_kind("chaser");
    const int clam     = find_kind("clam");
    const int walrus   = find_kind("walrus");
    const int spawner  = find_kind("spawner");
    const int anemone  = find_kind("anemone");
    const int fly      = find_kind("fly");
    const int squid    = find_kind("colossal_squid");
    const int puffer   = find_kind("pufferfish");
    const int angler   = find_kind("anglerfish");
    const int jelly    = find_kind("jellyfish");
    const int gargoyle = find_kind("gargoyle");
    const int evil     = find_kind("evil_mob");
    const int starg    = find_kind("stargazer");
    const int lung     = find_kind("iron_lung");
    const int evmob    = find_kind("evil_mob");
    const int buffer = find_kind("buffer"), bat = find_kind("bat");
    const int giant = find_kind("giant"), invoker = find_kind("invoker");
    if (chaser < 0 || clam < 0 || walrus < 0 || spawner < 0 || anemone < 0 ||
        fly < 0 || squid < 0 || puffer < 0 || angler < 0 || jelly < 0 ||
        gargoyle < 0 || evil < 0 || starg < 0 || lung < 0 || evmob < 0 ||
        buffer < 0 || bat < 0 || giant < 0 || invoker < 0) {
        printf("test_brotato_enemy_motion: missing kinds\n");
        return 1;
    }
    check((BT_KIND_MOVK[starg] & BT_MOVK_STARGAZER) != 0, "stargazer kind flagged");
    check((BT_KIND_MOVK[lung] & BT_MOVK_IRON_LUNG) != 0, "iron_lung kind flagged");
    check((BT_KIND_MOVK[evil] & BT_MOVK_EVIL_MOB) != 0, "evil_mob kind flagged");

    printf("[1] dispatch table vs BT_ENEMY_INIT_OPS movement provenance\n");
    {
        int rows = 0;
        for (int k = 0; k < BT_NKINDS; k++) {
            check(is_dispatch_beh((int)BT_MOV_ROOT[k].beh),
                  "root movement behaviour is dispatched");
            for (int st = 0; st < (int)BT_NKIND_STATES[k]; st++)
                check(is_dispatch_beh((int)BT_MOV_STATE[k][st].beh),
                      "state movement behaviour is dispatched");
            int first = (int)BT_KIND_INIT_FRESH_FIRST[k];
            int n = (int)BT_KIND_INIT_FRESH_N[k];
            for (int i = 0; i < n; i++) {
                BT_CONST_PTR(BtEnemyInitOp) op = &BT_ENEMY_INIT_OPS[first + i];
                if (op->site != BT_INITSITE_MOV_READY &&
                    op->site != BT_INITSITE_MOV_INIT) continue;
                rows++;
                int beh = (int)(op->state == BT_INIT_ROOT_STATE
                                ? BT_MOV_ROOT[k].beh : BT_MOV_STATE[k][op->state].beh);
                check(beh == (int)op->behavior,
                      "movement-init row behaviour matches the dispatch table");
            }
        }
        check(rows > 20, "the movement-init rows were located");
    }

    printf("[2] follow_target == the previous universal chase\n");
    {
        reset_all(&A, 11u);
        put(&A, 0, chaser, 500.0f, 500.0f);
        A.px = 800.0f; A.py = 600.0f;
        BtVec2 m = bt_enemy_move(&A, 0);
        check_near(m.x, 300.0f, 0.0f, "chase dx");
        check_near(m.y, 100.0f, 0.0f, "chase dy");
        float spd = bt_move_speed(A.espd[0], A.ebonus[0], 0.0f);
        BtVec2 mv = bt_move_input(m, A.espd[0], A.ebonus[0], 0.0f);
        check_near(bt_v2len(mv), spd, 1e-3f, "get_move_input length == move speed");
    }

    printf("[3] stay_in_range_from_player: target = player + _actual_target_range*dir\n");
    {
        reset_all(&A, 22u);
        put(&A, 0, clam, 1000.0f, 700.0f);
        A.px = 1000.0f; A.py = 1000.0f;
        float r = bt_mov_val(&A, 0, BT_MAX_STATES, BT_INITDEST_MOV_ACTUAL_RANGE);
        check(r >= 350.0f && r <= 450.0f, "clam target_range = 400 +- 50");
        BtVec2 m = bt_enemy_move(&A, 0);
        check_near(m.x, 0.0f, 0.0f, "stay_in_range pushes along (self-player) only");
        check_near(m.y, (1000.0f - r) - 700.0f, 1e-3f, "stay_in_range target y");
        /* already at the target -> ZERO (vectors_approx_equal <= 10) */
        set_pos(&A, 0, 1000.0f, 1000.0f - r);
        BtVec2 z = bt_enemy_move(&A, 0);
        check_near(z.x, 0.0f, 0.0f, "stay_in_range zero at target x");
        check_near(z.y, 0.0f, 0.0f, "stay_in_range zero at target y");
    }

    printf("[4] follow_rand_pos_around_player: allow_within normalisation\n");
    {
        reset_all(&A, 33u);
        put(&A, 0, spawner, 600.0f, 600.0f);
        A.px = 600.0f; A.py = 600.0f;   /* self == player -> movement == _distance */
        float a = bt_mov_val(&A, 0, BT_MAX_STATES, BT_INITDEST_MOV_ACTUAL);
        check(a >= 400.0f && a <= 500.0f, "spawner range 450 +- 50");
        BtVec2 m = bt_enemy_move(&A, 0);
        check_near(bt_v2len(m), a, 1e-3f, "allow_within=false -> |offset| == _actual");

        reset_all(&A, 44u);
        put(&A, 0, walrus, 600.0f, 600.0f);
        A.px = 600.0f; A.py = 600.0f;
        float dx = bt_mov_val(&A, 0, BT_MAX_STATES, BT_INITDEST_MOV_DIST_X);
        float dy = bt_mov_val(&A, 0, BT_MAX_STATES, BT_INITDEST_MOV_DIST_Y);
        BtVec2 w = bt_enemy_move(&A, 0);
        check_near(w.x, dx, 1e-3f, "allow_within=true keeps the raw offset x");
        check_near(w.y, dy, 1e-3f, "allow_within=true keeps the raw offset y");
    }

    printf("[5] target_rand_pos: lazy uniform point, held until reached\n");
    {
        reset_all(&A, 55u);
        put(&A, 0, anemone, 10.0f, 10.0f);
        A.px = 1000.0f; A.py = 1000.0f;
        unsigned short inst = A.e_instance[0];
        BtVec2 m1 = bt_enemy_move(&A, 0);
        float tx = *bt_pool_mvtx(&A, inst, BT_MAX_STATES);
        float ty = *bt_pool_mvty(&A, inst, BT_MAX_STATES);
        float minx = BT_KIND_HALF_W[anemone], miny = BT_KIND_HALF_H[anemone];
        check(tx >= minx && tx <= BT_ARENA_W - minx, "target x inside _min/_max");
        check(ty >= miny && ty <= BT_ARENA_H - miny, "target y inside _min/_max");
        check_near(m1.x, tx - 10.0f, 1e-3f, "movement = target - self (x)");
        check_near(m1.y, ty - 10.0f, 1e-3f, "movement = target - self (y)");
        BtVec2 m2 = bt_enemy_move(&A, 0);
        check_near(m2.x, m1.x, 0.0f, "target held while unreached x");
        check_near(m2.y, m1.y, 0.0f, "target held while unreached y");
        set_pos(&A, 0, tx, ty);          /* reach it -> redraw */
        (void)bt_enemy_move(&A, 0);
        float nx = *bt_pool_mvtx(&A, inst, BT_MAX_STATES);
        float ny = *bt_pool_mvty(&A, inst, BT_MAX_STATES);
        check(!(nx == tx && ny == ty), "target redrawn once reached");
        check(nx >= minx && nx <= BT_ARENA_W - minx, "redrawn target inside bounds");
    }

    printf("[6] target_rand_pos_around_player: clamp + update timer\n");
    {
        reset_all(&A, 66u);
        put(&A, 0, fly, 10.0f, 10.0f);
        A.px = 1000.0f; A.py = 1000.0f;
        float a = bt_mov_val(&A, 0, BT_MAX_STATES, BT_INITDEST_MOV_ACTUAL);
        check(a >= 100.0f && a <= 200.0f, "fly range 150 +- 50");
        unsigned short ninst = A.e_instance[0];
        (void)bt_enemy_move(&A, 0);
        float tx = *bt_pool_mvtx(&A, ninst, BT_MAX_STATES);
        float ty = *bt_pool_mvty(&A, ninst, BT_MAX_STATES);
        float minx = BT_KIND_HALF_W[fly], miny = BT_KIND_HALF_H[fly];
        check(tx >= minx && tx <= BT_ARENA_W - minx, "around-player target clamped x");
        check(ty >= miny && ty <= BT_ARENA_H - miny, "around-player target clamped y");
        check(bt_fabsf(tx - 1000.0f) <= a + 1e-3f, "target within +-_actual of the player");
        check(bt_fabsf(ty - 1000.0f) <= a + 1e-3f, "target within +-_actual of the player (y)");
        for (int i = 0; i < 5; i++) bt_enemy_move(&A, 0);
        check(*bt_pool_mvtime(&A, A.e_instance[0], BT_MAX_STATES) == 0.0f,
              "update_every_x_frames == -1 never advances the timer");

        reset_all(&A, 77u);
        put(&A, 0, squid, 10.0f, 10.0f);
        A.px = 1000.0f; A.py = 1000.0f;
        int node = BT_MAX_STATES;
        unsigned short inst = A.e_instance[0];
        (void)bt_enemy_move(&A, 0);
        check(*bt_pool_mvtime(&A, inst, node) == 0.0f, "squid redraw on the first call");
        for (int i = 0; i < 119; i++) (void)bt_enemy_move(&A, 0);
        check_near(*bt_pool_mvtime(&A, inst, node), 119.0f, 1e-3f, "timer = 119 after 120 calls");
        (void)bt_enemy_move(&A, 0);
        check(*bt_pool_mvtime(&A, inst, node) == 0.0f, "timer resets at update_every_x_frames");
    }

    printf("[7] patrol_edges_of_map: target clamped into the sprite-inset rect\n");
    {
        reset_all(&A, 88u);
        put(&A, 0, puffer, 10.0f, 10.0f);
        A.px = 1024.0f; A.py = 768.0f;
        unsigned short inst = A.e_instance[0];
        BtVec2 m = bt_enemy_move(&A, 0);
        float tx = *bt_pool_mvtx(&A, inst, BT_MAX_STATES);
        float ty = *bt_pool_mvty(&A, inst, BT_MAX_STATES);
        float minx = BT_KIND_HALF_W[puffer], miny = BT_KIND_HALF_H[puffer];
        check(tx >= minx && tx <= BT_ARENA_W - minx, "patrol target x clamped");
        check(ty >= miny && ty <= BT_ARENA_H - miny, "patrol target y clamped");
        check_near(m.x, tx - 10.0f, 1e-3f, "movement = target - self (x)");
        BtVec2 m2 = bt_enemy_move(&A, 0);
        check_near(m2.x, m.x, 0.0f, "patrol target held while unreached");
    }

    printf("[8] detect_target: range trigger, bonus_speed, persistent switch\n");
    {
        reset_all(&A, 99u);
        put(&A, 0, angler, 100.0f, 100.0f);
        A.px = 1200.0f; A.py = 1200.0f;   /* far */
        BtVec2 m = bt_enemy_move(&A, 0);
        float tx = A.ex[0] + m.x, ty = A.ey[0] + m.y;
        float minx = BT_KIND_HALF_W[angler];
        check(tx >= minx && tx <= BT_ARENA_W - minx, "undetected anglerfish wanders");
        check(A.ebonus[0] == 0.0f, "no bonus before detection");
        check(*bt_pool_mvflag(&A, A.e_instance[0], BT_MAX_STATES) == 0, "flag clear before detection");
        A.px = 200.0f; A.py = 200.0f;     /* within 340 */
        BtVec2 d = bt_enemy_move(&A, 0);
        check_near(A.ebonus[0], 350.0f, 0.0f, "detection adds speed_bonus 350");
        check(*bt_pool_mvflag(&A, A.e_instance[0], BT_MAX_STATES) == 1, "detected flag set");
        check_near(d.x, 100.0f, 0.0f, "detected -> follow_target chase x");
        check_near(d.y, 100.0f, 0.0f, "detected -> follow_target chase y");
        A.px = 1200.0f; A.py = 1200.0f;   /* out of range again */
        BtVec2 d2 = bt_enemy_move(&A, 0);
        check_near(d2.x, 1100.0f, 0.0f, "switch persists after the target leaves range");
        check_near(A.ebonus[0], 350.0f, 0.0f, "no repeat bonus after the first detection");
    }

    printf("[9] go_towards_map_center: state movement node\n");
    {
        reset_all(&A, 111u);
        put(&A, 0, jelly, 100.0f, 100.0f);
        A.estate[0] = 0;   /* ordinals are 0-based; the scene names it State1 */
        BtVec2 m = bt_enemy_move(&A, 0);
        check_near(m.x, BT_ARENA_W * 0.5f - 100.0f, 0.0f, "toward center x");
        check_near(m.y, BT_ARENA_H * 0.5f - 100.0f, 0.0f, "toward center y");
    }

    printf("[10] boss state switch keeps per-state movement nodes separate\n");
    {
        reset_all(&A, 122u);
        put(&A, 0, gargoyle, 500.0f, 500.0f);
        A.px = 900.0f; A.py = 500.0f;
        unsigned short inst = A.e_instance[0];
        (void)bt_enemy_move(&A, 0);               /* root: target_rand_pos */
        float rtx = *bt_pool_mvtx(&A, inst, BT_MAX_STATES);
        float rty = *bt_pool_mvty(&A, inst, BT_MAX_STATES);
        check(rtx != 0.0f || rty != 0.0f, "root node target drawn");
        A.estate[0] = 0;                          /* state0: follow_target */
        BtVec2 s0 = bt_enemy_move(&A, 0);
        check_near(s0.x, 400.0f, 0.0f, "state0 chase");
        A.estate[0] = 1;                          /* state1: target_rand_pos_around_player */
        check(*bt_pool_mvtx(&A, inst, 1) == 0.0f, "state1 node target starts empty");
        BtVec2 s1 = bt_enemy_move(&A, 0);
        check(*bt_pool_mvtx(&A, inst, 1) != 0.0f || *bt_pool_mvty(&A, inst, 1) != 0.0f,
              "state1 node target drawn");
        check(*bt_pool_mvtx(&A, inst, BT_MAX_STATES) == rtx &&
              *bt_pool_mvty(&A, inst, BT_MAX_STATES) == rty,
              "root node target untouched by the state node");
        check_near(s1.x, *bt_pool_mvtx(&A, inst, 1) - A.ex[0], 1e-2f,
                   "state1 movement is its own target - self (x)");
        check_near(s1.y, *bt_pool_mvty(&A, inst, 1) - A.ey[0], 1e-2f,
                   "state1 movement is its own target - self (y)");
    }

    printf("[11] target_rand_gold_pos: random live gold, else random point\n");
    {
        reset_all(&A, 133u);
        put(&A, 0, evil, 100.0f, 100.0f);
        A.px = 1000.0f; A.py = 1000.0f;
        BtVec2 m = bt_enemy_move(&A, 0);          /* no gold -> uniform point */
        unsigned short ginst = A.e_instance[0];
        float tx = *bt_pool_mvtx(&A, ginst, BT_MAX_STATES);
        float ty = *bt_pool_mvty(&A, ginst, BT_MAX_STATES);
        float minx = BT_KIND_HALF_W[evil], miny = BT_KIND_HALF_H[evil];
        check(tx >= minx && tx <= BT_ARENA_W - minx, "no-gold target in bounds");
        check(ty >= miny && ty <= BT_ARENA_H - miny, "no-gold target in bounds (y)");
        check_near(tx, A.ex[0] + m.x, 1e-2f, "movement = target - self");
        /* one live gold -> exactly that gold's position */
        bt_spawn_material(&A, 333.0f, 222.0f, 1);
        check(A.matalive[0] == 1, "gold spawned");
        /* reach the stored target so the next call redraws from the gold list */
        set_pos(&A, 0, *bt_pool_mvtx(&A, A.e_instance[0], BT_MAX_STATES),
               *bt_pool_mvty(&A, A.e_instance[0], BT_MAX_STATES));
        BtVec2 g = bt_enemy_move(&A, 0);
        check_near(A.ex[0] + g.x, 333.0f, 1e-2f, "target is the live gold x");
        check_near(A.ey[0] + g.y, 222.0f, 1e-2f, "target is the live gold y");
    }

    printf("[12] pool reuse: MOV values + node state persist, bonus_speed resets\n");
    {
        reset_all(&A, 144u);
        put(&A, 0, clam, 1000.0f, 700.0f);
        unsigned short id0 = A.e_instance[0];
        float r1 = bt_mov_val(&A, 0, BT_MAX_STATES, BT_INITDEST_MOV_ACTUAL_RANGE);
        kill_slot(&A, 0);
        for (int i = 0; i < 60; i++) bt_enemy_pool_step(&A);
        put(&A, 0, clam, 1000.0f, 700.0f);
        check(A.e_instance[0] == id0, "the same pool instance came back");
        float r2 = bt_mov_val(&A, 0, BT_MAX_STATES, BT_INITDEST_MOV_ACTUAL_RANGE);
        check_near(r2, r1, 0.0f, "reuse keeps the init-time _actual_target_range");

        /* detect_target flag survives, Unit.bonus_speed resets (Unit.respawn) */
        reset_all(&A, 155u);
        put(&A, 1, angler, 100.0f, 100.0f);
        unsigned short id1 = A.e_instance[1];
        A.px = 200.0f; A.py = 200.0f;
        (void)bt_enemy_move(&A, 1);
        check_near(A.ebonus[1], 350.0f, 0.0f, "detected on the fresh instance");
        kill_slot(&A, 1);
        for (int i = 0; i < 60; i++) bt_enemy_pool_step(&A);
        put(&A, 1, angler, 100.0f, 100.0f);
        check(A.e_instance[1] == id1, "anglerfish instance reused");
        check(A.ebonus[1] == 0.0f, "bonus_speed reset on respawn");
        check(*bt_pool_mvflag(&A, A.e_instance[1], BT_MAX_STATES) == 1,
              "detected flag survives the respawn");
        BtVec2 d = bt_enemy_move(&A, 1);
        check_near(d.x, 100.0f, 0.0f, "respawned anglerfish still chases (flag kept)");
        check(A.ebonus[1] == 0.0f, "no re-detection bonus after respawn");
    }

    printf("[13] core-step integration: movement vector drives the body\n");
    {
        reset_all(&A, 166u);
        put(&A, 0, jelly, 100.0f, 100.0f);
        A.estate[0] = 0;                 /* go_towards_map_center */
        A.px = 10.0f; A.py = 10.0f;      /* player in the corner */
        float obs[BT_OBS_SIZE]; float rew, term;
        bt_core_step(&A, 0, obs, &rew, &term);
        bt_core_step(&A, 0, obs, &rew, &term); /* first Unit update after birth hold */
        BtVec2 v = A.phys.bodies[0].vel;
        float cx = BT_ARENA_W * 0.5f - A.ex[0], cy = BT_ARENA_H * 0.5f - A.ey[0];
        float cl = bt_sqrtf(cx * cx + cy * cy), vl = bt_v2len(v);
        check(vl > 1.0f, "enemy body moving");
        check((v.x * cx + v.y * cy) / (cl * vl) > 0.9f,
              "velocity points at the map center, not the player");
    }

    printf("[14] gold ordering: _active_golds insertion order, not slot order\n");
    {
        reset_all(&A, 200u);
        for (int i = 0; i < BT_MAX_MAT; i++) A.matalive[i] = 0;
        A.matcount = 0; A.mat_seq_next = 0; A.bonus_gold = 0;
        bt_spawn_material(&A, 10.0f, 0.0f, 1);      /* slot 0, seq 0 */
        bt_spawn_material(&A, 20.0f, 0.0f, 1);      /* slot 1, seq 1 */
        A.matalive[0] = 0; A.matcount--;            /* the first gold is picked up */
        bt_spawn_material(&A, 30.0f, 0.0f, 1);      /* slot 0 again, seq 2 */
        check(A.matalive[0] && A.matalive[1], "two live golds");
        check(A.mat_seq[1] < A.mat_seq[0], "slot order differs from insertion order");
        int k0 = bt_loot_gold_ordered_slot(&A, 0);
        int k1 = bt_loot_gold_ordered_slot(&A, 1);
        check(k0 == 1, "k=0 is the older gold (slot 1)");
        check(k1 == 0, "k=1 is the newer gold (slot 0)");
        check_near(A.matx[k0], 20.0f, 0.0f, "older gold position");
        check_near(A.matx[k1], 30.0f, 0.0f, "newer gold position");
    }

    printf("[15] stargazer: the iron_lung it spawned is its movement target\n");
    {
        reset_all(&A, 201u);
        int st = 0, lg = 1;
        spawn_at(&A, st, starg, 100.0f, 100.0f);
        spawn_at(&A, lg, lung, 700.0f, 100.0f);
        check(A.ealive[st] && A.ealive[lg], "stargazer + lung placed");
        check(A.mv_lung[st] == (unsigned short)BT_POOL_NO_INST, "no link before the spawn hook");
        A.e_source[lg] = (short)st;
        bt_enemy_motion_link_spawner(&A, lg);
        check(A.mv_lung[st] == A.e_instance[lg], "linked by pool instance");
        check(A.mv_lung[lg] == (unsigned short)BT_POOL_NO_INST, "the lung holds no link");
        A.px = 100.0f; A.py = 900.0f;   /* the player is not the target */
        BtVec2 m = bt_enemy_move(&A, st);
        check_near(m.x, 600.0f, 1e-3f, "target is the lung (x)");
        check_near(m.y, 0.0f, 1e-3f, "target is the lung (y)");
    }

    printf("[16] iron_lung DetectionZone: full, +200%% hp, the stargazer is eaten\n");
    {
        reset_all(&A, 202u);
        int st = 0, lg = 1;
        spawn_at(&A, st, starg, 500.0f, 500.0f);
        spawn_at(&A, lg, lung, 900.0f, 500.0f);
        A.e_source[lg] = (short)st;
        bt_enemy_motion_link_spawner(&A, lg);
        unsigned short linst = A.e_instance[lg];
        float base_hp = A.emax[lg];
        A.px = 1200.0f; A.py = 1200.0f;
        bt_enemy_lifecycle_tick(&A);
        check(A.pool_lungfull[linst] == 0, "not full while the stargazer is away");
        /* move the stargazer's BODY circle onto the zone's circle */
        BT_CONST_PTR(BtBodyPhysics) G = bt_kgeom(&A, st);
        set_pos(&A, st, 900.0f - G->body_x, (500.0f + BT_LUNG_ZONE_DY) - G->body_y);
        bt_enemy_lifecycle_tick(&A);
        check(A.pool_lungfull[linst] == 1, "the stargazer filled the lung");
        check(A.emax[lg] > base_hp, "lung healed to +200%");
        check_near(A.ehp[lg], A.emax[lg], 1e-3f, "healed to full");
        check(!A.ealive[st], "the stargazer was eaten");
        check(A.ealive[lg], "a full lung survives");
    }

    printf("[17] stargazer death kills its linked lung (not full)\n");
    {
        reset_all(&A, 203u);
        int st = 0, lg = 1;
        spawn_at(&A, st, starg, 500.0f, 500.0f);
        spawn_at(&A, lg, lung, 900.0f, 500.0f);
        A.e_source[lg] = (short)st;
        bt_enemy_motion_link_spawner(&A, lg);
        check(A.pool_lungfull[A.e_instance[lg]] == 0, "the lung is not full");
        bt_credit_kill(&A, st, 1);
        check(!A.ealive[st], "stargazer dead");
        check(A.ealive[lg], "the lung's death is deferred to the frame end");
        bt_enemy_lifecycle_tick(&A);
        check(!A.ealive[lg], "the queued lung death ran");
    }

    printf("[18] a lung that dies not full enrages its stargazer\n");
    {
        reset_all(&A, 204u);
        int st = 0, lg = 1;
        spawn_at(&A, st, starg, 500.0f, 500.0f);
        spawn_at(&A, lg, lung, 900.0f, 500.0f);
        A.e_source[lg] = (short)st;
        bt_enemy_motion_link_spawner(&A, lg);
        float base_spd = A.espd[st];
        bt_credit_kill(&A, lg, 1);
        check(A.mv_angry[st] == 1, "angry switch set");
        check(A.espd[st] > base_spd, "speed boosted");
        check_near(A.espd[st],
                   bt_enemy_final_speed(&A, (float)BT_KINDS[starg].speed + A.espeed_rand[st], 250.0f),
                   1e-3f, "speed = get_final_enemy_speed(base, 250)");
        A.px = 100.0f; A.py = 100.0f;
        BtVec2 m = bt_enemy_move(&A, st);
        check_near(m.x, 100.0f - A.ex[st], 1e-3f, "angry -> OnTriggered follow_target (x)");
    }

    printf("[19] evil_mob: claim, eat, gold_count, evolve, banked drop\n");
    {
        reset_all(&A, 205u);
        int mob = 0;
        spawn_at(&A, mob, evmob, 1000.0f, 1000.0f);
        A.px = 100.0f; A.py = 100.0f;      /* player far away */
        A.bonus_gold = 0;
        int g = place_gold(&A, 1050.0f, 1000.0f, 1);   /* in the 100 attract, out of the 30 pickup */
        check(g >= 0, "gold placed");
        bt_enemy_lifecycle_tick(&A);
        check(A.mat_mob[g] == mob, "claimed by the evil_mob");
        check(A.matx[g] < 1050.0f, "stepped toward the mob");
        A.matx[g] = 1000.0f; A.maty[g] = 990.0f;       /* inside the pickup circle */
        bt_enemy_lifecycle_tick(&A);
        check(A.matalive[g] == 0, "gold eaten");
        check(A.emob_count[mob] == 1, "gold_count += the coin value");
        check(A.bonus_gold == 1, "eaten gold is banked (player_index -1)");
        check_near(A.materials, 30.0f, 0.0f, "no materials credited to the player");
        /* main.gd:720-731: the next dropped coin absorbs the banked gold */
        int g2 = place_gold(&A, 300.0f, 300.0f, 1);
        check(g2 >= 0 && A.matval[g2] == 2, "banked gold doubled the next coin");
        check(A.bonus_gold == 0, "banked gold removed (clamped at 0)");
        A.matalive[g2] = 0; A.matcount--;

        /* evolution: quantity_to_evolve [0,10,20], bonus_speed = -(125*evolution) */
        A.emob_count[mob] = 9; A.emob_evo[mob] = 0; A.bonus_gold = 0;
        float hp1 = A.emax[mob];
        place_gold(&A, 1000.0f, 990.0f, 1);
        bt_enemy_lifecycle_tick(&A);
        check(A.emob_count[mob] == 10 && A.emob_evo[mob] == 1, "evolves at 10");
        check_near(A.ebonus[mob], -125.0f, 0.0f, "bonus_speed = -(125*1)");
        check_near(A.emax[mob], hp1, 1e-3f, "health *= int(1 + 0.5*1) = 1");
        A.emob_count[mob] = 19; A.bonus_gold = 0;
        place_gold(&A, 1000.0f, 990.0f, 1);
        bt_enemy_lifecycle_tick(&A);
        check(A.emob_evo[mob] == 2, "evolves at 20");
        check_near(A.ebonus[mob], -250.0f, 0.0f, "bonus_speed = -(125*2)");
        check_near(A.emax[mob], hp1 * 2.0f, 1e-3f, "health *= int(1 + 0.5*2) = 2");

        /* death drop = get_stats_value = int(gold_count*(evolution+1)) */
        for (int i = 0; i < BT_MAX_MAT; i++) A.matalive[i] = 0;
        A.matcount = 0; A.bonus_gold = 0;
        A.emob_count[mob] = 20; A.emob_evo[mob] = 2;
        bt_credit_kill(&A, mob, 1);
        int total = 0;
        for (int i = 0; i < BT_MAX_MAT; i++) if (A.matalive[i]) total += (int)A.matval[i];
        check(total == 60, "drops int(20*(2+1)) = 60 gold of entities");
    }

    printf("[20] the spawn-attack source reaches the stargazer link end to end\n");
    {
        reset_all(&A, 206u);
        int st = 0;
        spawn_at(&A, st, starg, 900.0f, 900.0f);
        A.px = 200.0f; A.py = 200.0f;
        A.eatk[st] = 0.0f;                 /* the spawning attack fires */
        float obs[BT_OBS_SIZE]; float rew, term;
        int lg = -1;
        for (int t = 0; t < 150 && lg < 0; t++) {
            bt_core_step(&A, 0, obs, &rew, &term);
            for (int i = 0; i < BT_MAX; i++)
                if (A.ealive[i] && (int)A.ekind[i] == lung) { lg = i; break; }
        }
        check(lg >= 0, "the stargazer spawned its iron_lung");
        if (lg >= 0) {
            check(A.e_source[lg] == (short)st, "the lung remembers its spawner");
            check(A.mv_lung[st] == A.e_instance[lg], "and the stargazer linked it");
        }
    }

    printf("[21] can_be_boosted guard metadata + helper\n");
    {
        int buffer = find_kind("buffer");
        int bat = find_kind("bat");
        int giant = find_kind("giant");
        check(buffer >= 0 && bat >= 0 && giant >= 0, "boost-flag fixture kinds present");
        check(BT_KIND_BOOSTABLE[chaser] == 1, "chaser can_be_boosted (enemy.tscn default)");
        check(BT_KIND_BOOSTABLE[buffer] == 0, "buffer can_be_boosted = false (scene)");
        check(BT_KIND_BOOSTABLE[bat] == 0, "bat can_be_boosted = false (DLC scene)");
        check(BT_KIND_BOOSTABLE[giant] == 0, "giant can_be_boosted = false (DLC scene)");
        reset_all(&A, 300u);
        put(&A, 0, chaser, 500.0f, 500.0f);
        check(bt_enemy_can_boost(&A, 0) == 1, "a chaser can be boosted");
        reset_all(&A, 301u);
        put(&A, 0, buffer, 500.0f, 500.0f);
        check(bt_enemy_can_boost(&A, 0) == 0, "a buffer cannot");
        reset_all(&A, 302u);
        put(&A, 0, starg, 500.0f, 500.0f);
        check(bt_enemy_can_boost(&A, 0) == 1, "a stargazer can until it is angry");
        A.mv_angry[0] = 1;
        check(bt_enemy_can_boost(&A, 0) == 0, "get_angry clears can_be_boosted");
        A.mv_angry[0] = 0;
        check(bt_enemy_can_boost(&A, 0) == 1, "respawn restores it (flag cleared)");
    }

    printf("[22] charm: gates, state, movement swap, target, expiry\n");
    {
        int sea_pig = find_kind("sea_pig");
        check(sea_pig >= 0, "sea_pig present");
        check(BT_KIND_CHARMABLE[chaser] == 1, "a plain enemy is charmable");
        check(BT_KIND_CHARMABLE[sea_pig] == 0, "can_be_charmed = false (scene)");
        check(BT_KIND_CHARMABLE[evil] == 0, "is_loot is never charmable");
        reset_all(&A, 303u);
        put(&A, 0, chaser, 500.0f, 500.0f);
        check(bt_enemy_can_charm(&A, 0) == 1, "a chaser is charmable");
        reset_all(&A, 304u);
        put(&A, 0, sea_pig, 500.0f, 500.0f);
        check(bt_enemy_can_charm(&A, 0) == 0, "sea_pig is not");
        reset_all(&A, 305u);
        put(&A, 0, evil, 500.0f, 500.0f);
        check(bt_enemy_can_charm(&A, 0) == 0, "evil_mob is not (is_loot)");
        reset_all(&A, 306u);
        put(&A, 0, invoker, 500.0f, 500.0f);
        check(bt_enemy_can_charm(&A, 0) == 0, "a boss is not");

        /* charm() state, stats and the speed bonus */
        reset_all(&A, 307u);
        put(&A, 0, chaser, 500.0f, 500.0f);
        float dmg0 = A.edmg[0];
        A.eburn[0] = 2.0f; A.eburndmg[0] = 3.0f;
        bt_enemy_lifecycle_charm(&A, 0, 0);
        check(A.e_charmed[0] == 1 && A.e_charm_by[0] == 0, "charm state set");
        check_near(A.e_charm_timer[0], BT_CHARM_DURATION, 0.0f, "CharmTimer = 8 s");
        check(A.e_charm_shield[0] == 1, "_absorb_next_damage armed");
        check(A.charm_count[0] == 1, "RunData.current_charmed_enemies[0]++");
        check_near(A.edmg[0],
                   bt_enemy_final_damage(&A, BT_KINDS[chaser].dmg + BT_KINDS[chaser].dmg_grow * 0.0f,
                                         200.0f), 1e-3f, "reset_damage_stat(200)");
        check(A.edmg[0] > dmg0, "charm raised the damage");
        check_near(A.ebonus[0], 100.0f, 0.0f, "bonus_speed += 100");
        check(A.eburn[0] == 0.0f && A.eburndmg[0] == 0.0f, "stop_burning()");
        /* the shield absorbs exactly one damage instance */
        check(bt_enemy_charm_absorb(&A, 0) == 1, "first hit absorbed");
        check(bt_enemy_charm_absorb(&A, 0) == 0, "later hits are normal");
        /* the charmed unit chases the NEAREST OTHER enemy, not the player */
        put(&A, 1, chaser, 800.0f, 500.0f);
        A.px = 100.0f; A.py = 100.0f;
        BtVec2 m = bt_enemy_move(&A, 0);
        check_near(m.x, 300.0f, 1e-3f, "charmed chase -> the other enemy (x)");
        check_near(m.y, 0.0f, 1e-3f, "charmed chase -> the other enemy (y)");

        /* the movement swap: a charmed stay_in_range kind wanders instead */
        reset_all(&A, 308u);
        put(&A, 0, clam, 10.0f, 10.0f);
        A.px = 1000.0f; A.py = 1000.0f;
        unsigned short cinst = A.e_instance[0];
        (void)bt_enemy_move(&A, 0);
        check(*bt_pool_mvtx(&A, cinst, BT_MAX_STATES) == 0.0f,
              "stay_in_range stores no target node");
        bt_enemy_lifecycle_charm(&A, 0, 0);
        (void)bt_enemy_move(&A, 0);
        float ctv = *bt_pool_mvtx(&A, cinst, BT_MAX_STATES);
        check(ctv != 0.0f, "the charm swap stores a TargetRandPos target");
        check(ctv >= BT_KIND_HALF_W[clam] && ctv <= BT_ARENA_W - BT_KIND_HALF_W[clam],
              "and it is a uniform point in [_min,_max]");

        /* expiry: the CharmTimer kills the unit, whose death uncharm()s */
        reset_all(&A, 309u);
        put(&A, 0, chaser, 500.0f, 500.0f);
        bt_enemy_lifecycle_charm(&A, 0, 0);
        check(A.charm_count[0] == 1, "charmed");
        A.e_charm_timer[0] = BT_DT * 0.5f;
        bt_enemy_lifecycle_tick(&A);
        check(!A.ealive[0], "the CharmTimer expiry dies");
        check(A.e_charmed[0] == 0 && A.charm_count[0] == 0, "on_death -> uncharm");
        check_near(A.ebonus[0], 0.0f, 0.0f, "the +100 bonus speed is released");

        /* spawn propagation: a child of a charmed enemy is charmed too */
        reset_all(&A, 310u);
        put(&A, 0, chaser, 500.0f, 500.0f);
        bt_enemy_lifecycle_charm_spawn(&A, 0, -1);
        check(A.e_charmed[0] == 0, "charmed_by = -1 leaves it uncharmed");
        bt_enemy_lifecycle_charm_spawn(&A, 0, 0);
        check(A.e_charmed[0] == 1 && A.e_charm_by[0] == 0, "charmed_by propagates");
        bt_enemy_lifecycle_charm_spawn(&A, 0, 1);
        check(A.charm_count[1] == 0 && A.e_charm_by[0] == 0, "an already-charmed unit is a no-op");
    }

    printf("[23] charm layers/friendliness, boost retarget, wave-end bonus\n");
    {
        reset_all(&A, 311u);
        put(&A, 0, chaser, 500.0f, 500.0f);
        check(bt_enemy_body_layer(&A, 0) == BT_LBIT_ENEMIES, "uncharmed body layer = ENEMIES");
        check(bt_enemy_body_mask(&A, 0) == BT_MASK_ENEMY, "uncharmed body mask = 132");
        check(bt_enemy_hitbox_layer(&A, 0) == BT_LBIT_ENEMIES, "uncharmed hitbox layer");
        check(bt_enemy_shot_layer(&A, 0) == BT_LBIT_ENEMY_PROJECTILES, "uncharmed shot layer");
        check(bt_enemy_hurtbox_enabled(&A, 0) == 1, "uncharmed: hurtable");
        check(bt_enemy_hits_enemies(&A, 0) == 0, "uncharmed: hits the player");
        bt_enemy_lifecycle_charm(&A, 0, 0);
        check(bt_enemy_body_layer(&A, 0) == BT_LBIT_PETS, "charmed body layer = PETS");
        check(bt_enemy_body_mask(&A, 0) == (BT_LBIT_PETS | BT_LBIT_OBSTACLES),
              "charmed body mask = PETS|OBSTACLES");
        check(bt_enemy_hitbox_layer(&A, 0) == BT_LBIT_PET_PROJECTILES, "charmed hitbox layer");
        check(bt_enemy_shot_layer(&A, 0) == BT_LBIT_PET_PROJECTILES, "charmed shot layer");
        check(bt_enemy_hurtbox_enabled(&A, 0) == 0, "charmed: hurtbox disabled");
        check(bt_enemy_hits_enemies(&A, 0) == 1, "charmed: hits enemies");
        /* player.tscn Hurtbox mask 20 cannot see a PET_PROJECTILES hitbox; the
         * enemy Hurtbox mask 1032 can */
        check((20u & BT_LBIT_PET_PROJECTILES) == 0, "the player's hurtbox ignores it");
        check((1032u & BT_LBIT_PET_PROJECTILES) != 0, "an enemy hurtbox sees it");

        reset_all(&A, 312u);
        int buf = find_kind("buffer");
        int hlr = find_kind("healer");
        put(&A, 0, buf, 500.0f, 500.0f);
        check(bt_enemy_boost_retargets(&A, 0) == 0, "an uncharmed buffer boosts enemies");
        check(bt_enemy_boost_zone_mask(&A, 0) == BT_LBIT_ENEMIES, "BoostZone mask = ENEMIES");
        bt_enemy_lifecycle_charm(&A, 0, 0);
        check(bt_enemy_boost_retargets(&A, 0) == 1, "a charmed buffer retargets");
        check(bt_enemy_boost_zone_mask(&A, 0) == (BT_LBIT_PLAYER | BT_LBIT_STRUCTURES),
              "BoostZone mask = PLAYER|STRUCTURES");
        check(hlr >= 0 && (BT_KIND_MOVK[hlr] & BT_MOVK_HEALER) != 0, "healer root script flagged");
        reset_all(&A, 313u);
        put(&A, 0, hlr, 500.0f, 500.0f);
        bt_enemy_lifecycle_charm(&A, 0, 0);
        check(bt_enemy_boost_retargets(&A, 0) == 1, "a charmed healer retargets too");
        reset_all(&A, 314u);
        put(&A, 0, chaser, 500.0f, 500.0f);
        check(bt_enemy_boost_retargets(&A, 0) == 0, "a charmed chaser never retargets");

        /* wave-end charmed_enemy_bonus, per player, from the BASE stats value */
        reset_all(&A, 315u);
        put(&A, 0, chaser, 500.0f, 500.0f);
        put(&A, 1, buf, 600.0f, 500.0f);
        bt_enemy_lifecycle_charm(&A, 0, 0);
        bt_enemy_lifecycle_charm(&A, 1, 1);
        check_near(bt_enemy_lifecycle_charmed_bonus(&A, 0),
                   bt_loot_gold_value(&A, 0, (float)BT_KINDS[chaser].value), 1e-4f,
                   "player 0 sums only its own charmed enemies");
        check_near(bt_enemy_lifecycle_charmed_bonus(&A, 1),
                   bt_loot_gold_value(&A, 0, (float)BT_KINDS[buf].value), 1e-4f,
                   "player 1 sums its own");
        check(bt_enemy_lifecycle_charmed_bonus(&A, 2) == 0.0f, "an uninvolved player gets nothing");
        bt_enemy_lifecycle_uncharm(&A, 0);
        check(bt_enemy_lifecycle_charmed_bonus(&A, 0) == 0.0f, "uncharm removes it from the bonus");
    }

    printf("[24] source-object uid: mint, pooled reuse, neutral retain, overflow\n");
    {
        reset_all(&A, 320u);
        put(&A, 0, chaser, 500.0f, 500.0f);
        unsigned int u1 = bt_object_uid(&A, 0);
        check(u1 != 0u, "a fresh node mints a uid");
        check(A.object_uid_next == u1, "minted monotonically from the per-wave counter");
        /* an ordinary pooled respawn keeps its node identity */
        kill_slot(&A, 0);
        for (int i = 0; i < 60; i++) bt_enemy_pool_step(&A);
        put(&A, 0, chaser, 500.0f, 500.0f);
        check(bt_object_uid(&A, 0) == u1, "a pooled respawn retains its uid");
        /* a different node gets a different uid */
        put(&A, 1, buffer, 600.0f, 500.0f);
        check(bt_object_uid(&A, 1) != u1 && bt_object_uid(&A, 1) != 0u, "a new node mints a new uid");
        /* A neutral tree is pooled by scene in the source, but its node returns to
         * the pool only when the 1.0 s death animation ends (entity.gd:68-100), so
         * a respawn before those 60 frames instantiates a FRESH node. */
        reset_all(&A, 321u);
        check(bt_spawn_neutral(&A, 0, 700.0f, 500.0f) == 1, "neutral tree placed");
        int nt = -1;
        for (int i = 0; i < BT_MAX; i++) if (A.ealive[i] && A.eneutral[i]) { nt = i; break; }
        check(nt >= 0, "neutral slot found");
        unsigned int un = bt_object_uid(&A, nt);
        check(un != 0u, "a neutral mints a uid");
        bt_credit_kill(&A, nt, 1);
        check(A.neutral_uid_n[0] == 0, "the retired neutral is NOT reusable yet");
        check(bt_spawn_neutral(&A, 0, 800.0f, 500.0f) == 1, "respawned before 60 frames");
        int nt2 = -1;
        for (int i = 0; i < BT_MAX; i++) if (A.ealive[i] && A.eneutral[i]) { nt2 = i; break; }
        check(nt2 >= 0, "early-respawn slot found");
        check(nt2 >= 0 && bt_object_uid(&A, nt2) != un && bt_object_uid(&A, nt2) != 0u,
              "an early respawn is a NEW node (fresh uid, no early reuse)");
        /* after the 60-frame death animation the SAME node returns to the pool */
        reset_all(&A, 322u);
        check(bt_spawn_neutral(&A, 0, 700.0f, 500.0f) == 1, "neutral tree placed again");
        nt = -1;
        for (int i = 0; i < BT_MAX; i++) if (A.ealive[i] && A.eneutral[i]) { nt = i; break; }
        check(nt >= 0, "neutral slot found");
        un = bt_object_uid(&A, nt);
        bt_credit_kill(&A, nt, 1);
        for (int i = 0; i < 60; i++) { bt_enemy_pool_step(&A); bt_neutral_uid_step(&A); }
        check(A.neutral_uid_n[0] == 1, "after 60 frames the uid is back in its kind's LIFO");
        check(bt_spawn_neutral(&A, 0, 800.0f, 500.0f) == 1, "respawned after 60 frames");
        nt2 = -1;
        for (int i = 0; i < BT_MAX; i++) if (A.ealive[i] && A.eneutral[i]) { nt2 = i; break; }
        check(nt2 >= 0 && bt_object_uid(&A, nt2) == un, "the reused neutral node keeps its old uid");
        check(A.neutral_uid_n[0] == 0, "and the LIFO entry was consumed");
        /* explicit exhaustion, never an aliased or slot-fallback id */
        reset_all(&A, 323u);
        A.object_uid_next = BT_OBJECT_UID_MAX;
        A.object_uid_overflow = 0;
        put(&A, 0, chaser, 500.0f, 500.0f);
        check(bt_object_uid(&A, 0) == 0u, "exhausted space yields the 0 sentinel");
        check(A.object_uid_overflow == 1, "and raises object_uid_overflow");
    }

    printf("[25] Healer BoostZone heal + charmed player boost/expiry\n");
    {
        int hlr = find_kind("healer");
        int buf = find_kind("buffer");
        check(hlr >= 0 && buf >= 0, "healer/buffer kinds present");
        /* uncharmed: the zone heals a damaged ENEMY on entry, once per entry */
        reset_all(&A, 330u);
        put(&A, 0, hlr, 500.0f, 500.0f);
        put(&A, 1, chaser, 600.0f, 500.0f);
        A.emax[1] = 1000.0f; A.ehp[1] = 1.0f;
        bt_enemy_lifecycle_tick(&A);
        check_near(A.ehp[1], 101.0f, 1e-3f, "entry heals heal(100)+0");
        bt_enemy_lifecycle_tick(&A);
        check_near(A.ehp[1], 101.0f, 1e-3f, "staying inside does not heal again");
        set_pos(&A, 1, 2000.0f, 1400.0f);       /* leave the zone */
        bt_enemy_lifecycle_tick(&A);
        set_pos(&A, 1, 600.0f, 500.0f);         /* and re-enter */
        bt_enemy_lifecycle_tick(&A);
        check_near(A.ehp[1], 201.0f, 1e-3f, "a re-entry heals again");
        /* charmed: the player is healed instead */
        reset_all(&A, 331u);
        put(&A, 0, hlr, 500.0f, 500.0f);
        bt_enemy_lifecycle_charm(&A, 0, 0);
        A.max_hp = 100.0f; A.hp = 10.0f;
        A.px = 600.0f; A.py = 500.0f - BT_PLAYER_BODY_Y;
        bt_enemy_lifecycle_tick(&A);
        check_near(A.hp, 11.0f, 1e-3f, "a charmed Healer heals the player by player_heal(1)");
        /* charmed Buffer: the player boost + its 5 s expiry */
        reset_all(&A, 332u);
        put(&A, 0, buf, 500.0f, 500.0f);
        bt_enemy_lifecycle_charm(&A, 0, 0);
        bt_core_stat_add(&A, 0, 70.0f);          /* stat layer max_hp 30 -> 100 */
        sync_player_stats(&A);                   /* the player's max_stats.health reaches 100 */
        A.hp = 50.0f;                            /* damaged at the moment of the boost */
        A.px = 600.0f; A.py = 500.0f - BT_PLAYER_BODY_Y;
        float max0 = A.max_hp;
        bt_enemy_lifecycle_charmed_boost(&A, 0);
        check(A.pboost_active == 1, "the player boost is live");
        check_near(A.max_hp, max0 * 1.20f, 1e-3f, "player_hp_boost 20% raised max HP");
        check_near(A.hp, 50.0f + 20.0f, 1e-3f,
                   "player.gd:1110-1112 boost() adds the increase to current health at once");
        sync_player_stats(&A);
        check_near(A.hp, 50.0f + 40.0f, 1e-3f,
                   "and the deferred update_player_stats adds it again (max_stats.health grew)");
        check(A.temp_stat[3] >= 20.0f, "player_speed_boost 20 in TempStats");
        check(A.temp_stat[15] >= 20.0f, "player_attack_speed_boost 20 in TempStats");
        A.pboost_timer = BT_DT * 0.5f;
        bt_enemy_lifecycle_tick(&A);
        check(A.pboost_active == 0, "the BoostTimer expiry removes it");
        check_near(A.max_hp, max0, 1e-3f, "max HP is back");
        check(A.temp_stat[3] < 20.0f && A.temp_stat[15] < 20.0f, "and the temp stats are gone");
    }

    printf("[26] attack animation cadence: charge cooldown, egg death\n");
    {
        /* charging_attack_behavior.gd:59-121: the cooldown expiring starts the
         * charge animation, its `shoot` method (shoot_at_f frames) locks the
         * dash and arms `get_cd() + charge_duration * 60`; the dash then runs
         * for charge_duration.  A charger must NOT re-charge the frame its dash
         * ends.  Knight: without Bull's explode_on_hit, which would kill the
         * charging enemy on its own contact hit. */
        const int chg = find_kind("charger");
        check(chg >= 0, "charger present");
        bt_core_init(&A, 400u, 0, 1, BT_CHARACTER_KNIGHT, BT_WEAPON_NONE);
        A.wave = 1; A.n_live_groups = 0;
        for (int i = 0; i < BT_MAX; i++) bt_enemy_set_alive(&A, i, 0);
        for (int i = 0; i < BT_MAX_MAT; i++) A.matalive[i] = 0;
        A.ecount = 0; A.matcount = 0;
        A.px = 1000.0f; A.py = 500.0f;
        put(&A, 0, chg, 800.0f, 500.0f);        /* inside max_range 300 */
        unsigned int at_f = BT_ATTACKS[chg].shoot_at_f;
        check(at_f > 0 && BT_ATTACKS[chg].anim_len_f >= at_f,
              "the charge animation timing was extracted");
        float obs[BT_OBS_SIZE]; float rew, term;
        float cd0 = A.eatk[0];                  /* the spawn `_ready` get_cd() */
        check(cd0 > 0.0f, "the charger spawns with a cooldown");
        int dash_at = -1;
        for (int f = 1; f <= 400; f++) {
            bt_core_step(&A, 0, obs, &rew, &term);
            if (A.echarge[0] > 0.0f) {
                if (dash_at < 0) dash_at = f;
                else if (A.echarge[0] <= 1.0f) break;   /* echarge counts frames */
            }
        }
        check(dash_at > 0, "the charger dashed");
        check(A.eatk[0] > 0.0f,
              "the charge cooldown was re-armed (no immediate re-dash)");
        int expect = (int)cd0 + (int)at_f;   /* cd0 is already in FRAMES */
        check(dash_at >= expect - 2 && dash_at <= expect + 3,
              "the dash starts after get_cd() + the charge windup");

        /* slasher_egg.gd:4-6: the egg dies the moment its spawning attack emits
         * (one child per egg), not once per cooldown forever. */
        const int egg = find_kind("slasher_egg");
        const int slasher = find_kind("slasher");
        check(egg >= 0 && slasher >= 0, "slasher_egg + slasher present");
        check(BT_ATTACKS[egg].kill_self_on_spawn == 1, "the egg's self-die is extracted");
        bt_core_init(&A, 401u, 0, 1, BT_CHARACTER_KNIGHT, BT_WEAPON_NONE);
        A.wave = 1; A.n_live_groups = 0;
        for (int i = 0; i < BT_MAX; i++) bt_enemy_set_alive(&A, i, 0);
        for (int i = 0; i < BT_MAX_MAT; i++) A.matalive[i] = 0;
        A.ecount = 0; A.matcount = 0;
        put(&A, 0, egg, 120.0f, 120.0f);
        A.px = 1500.0f; A.py = 1200.0f;
        bt_core_step(&A, 0, obs, &rew, &term);  /* newborn hold */
        A.eatk[0] = 0.0f;
        for (int f = 0; f < 400; f++) bt_core_step(&A, 0, obs, &rew, &term);
        int eggs = 0, kids = 0;
        for (int i = 0; i < BT_MAX; i++) {
            if (!A.ealive[i] || A.eneutral[i]) continue;
            if ((int)A.ekind[i] == egg) eggs++;
            if ((int)A.ekind[i] == slasher) kids++;
        }
        check(kids == 1, "the egg spawned exactly one slasher");
        check(eggs == 0, "and died doing it");
    }

    printf("[27] the charging clock is a FRAME clock: lock frame, `shoot` key, unlock Timer\n");
    {
        /* charging_attack_behavior.gd:41-46 (_ready: `_current_cd = get_cd()`
         * counts FRAMES, `_unlock_move_timer.wait_time = charge_duration`) and
         * :63-67 (`_current_cd = max(_current_cd - 60 * delta, 0)`, exactly one
         * frame subtracted per physics frame).  An attack clock of N frames
         * therefore expires on the Nth frame; the animation's t=0 method runs
         * start_shoot() at THAT frame's deferred flush (`_can_move = false`,
         * mass x5), so the next frame's movement input is zero and the enemy's
         * last step is the frame after the trigger (the core caches the NEXT
         * step's velocity); the `shoot` key at shoot_at_f frames unlocks the
         * dash, and `_unlock_move_timer` fires charge_duration frames later.
         * A float32 SECONDS accumulator (eatk = frames/60, decremented by
         * BT_DT) is one frame LATE whenever the exact remainder lands on 0: the
         * seed-1 deep-start capture's wave-20 predator (120-frame cooldown, no
         * randomization) took 121 frames, so its hold slipped one frame and the
         * capture's first divergence was tick 185 obs[35] with one extra 5 px
         * step to x 1526.4060 where the game holds at x 1529.5636. */
        const int pred = find_kind("predator");
        check(pred >= 0, "the predator kind exists");
        const unsigned int pred_cd = BT_ATTACKS[pred].cooldown_f;
        const unsigned int pred_at = BT_ATTACKS[pred].shoot_at_f;
        const float pred_dash = bt_timer_frames(BT_ATTACKS[pred].charge_duration);
        check(pred_cd == 120u && pred_at == 48u && pred_dash == 30.0f,
              "the predator's extracted clock: 120-frame cooldown, 48-frame key, 30-frame unlock");
        bt_core_init(&A, 9u, 0, 1, BT_CHARACTER_KNIGHT, BT_WEAPON_NONE);
        A.wave = 1; A.n_live_groups = 0;
        for (int i = 0; i < BT_MAX; i++) bt_enemy_set_alive(&A, i, 0);
        for (int i = 0; i < BT_MAX_MAT; i++) A.matalive[i] = 0;
        A.ecount = 0; A.matcount = 0;
        A.px = 1000.0f; A.py = 500.0f;
        put(&A, 0, pred, 1300.0f, 500.0f);      /* 300 px: inside max_range 500 */
        float obs[BT_OBS_SIZE], rew, term;
        bt_core_step(&A, 0, obs, &rew, &term);  /* newborn hold: no Unit update */
        A.eatk[0] = (float)pred_cd;             /* the spawn `_ready` get_cd() */
        int lock_at = 0, shoot_at = 0, unlock_at = 0;
        float x_lock = 0.0f, x_step = 0.0f, x_hold = 0.0f;
        float v_lock = -1.0f, v_next = -1.0f;
        for (int f = 1; f <= 400 && !unlock_at; f++) {
            bt_core_step(&A, 0, obs, &rew, &term);
            if (!lock_at && A.ewind[0] == 1) {
                lock_at = f; x_lock = A.ex[0]; v_lock = bt_v2lensq(A.phys.bodies[0].vel);
            } else if (lock_at && !shoot_at && A.ewind[0] == 0 && A.echarge[0] > 0.0f) {
                shoot_at = f;
                check_near(A.echarge[0], pred_dash, 0.25f,
                           "the dash arms the unlock Timer's own FRAME count, not seconds");
            } else if (shoot_at && A.echarge[0] <= 0.0f) {
                unlock_at = f;
                check_near(A.phys.bodies[0].mass, A.emass[0], 1e-3f,
                           "the unlock restores the original mass");
            }
            if (lock_at && f == lock_at + 1) { x_step = A.ex[0]; v_next = bt_v2lensq(A.phys.bodies[0].vel); }
            if (lock_at && f == lock_at + 2) x_hold = A.ex[0];
        }
        check(lock_at == (int)pred_cd,
              "the telegraph (start_shoot's lock) starts on the cooldown's own frame");
        check(v_lock > 0.0f && v_next == 0.0f,
              "the trigger frame keeps its cached step, the next frame's command is zero");
        check(bt_fabsf(x_step - x_lock) > 0.1f, "so the enemy still takes that one step");
        check(bt_fabsf(x_hold - x_step) < 1e-3f, "and holds from the frame after");
        check(shoot_at == (int)pred_cd + (int)pred_at,
              "the `shoot` key fires shoot_at_f frames after the trigger");
        check(unlock_at == (int)pred_cd + (int)pred_at + (int)pred_dash,
              "the unlock Timer fires charge_duration frames after the shoot key");

        /* The unlock Timer's own countdown: its wait_time is subtracted by the
         * float32 physics delta and it fires when time_left goes NEGATIVE, so the
         * charger's 0.75 s is 45 frames, not 45.0 -- and not the 46 a float32
         * seconds accumulator held (the same +1 that slipped the predator). */
        const int chg = find_kind("charger");
        check(chg >= 0, "the charger kind exists");
        const float chg_dash = bt_timer_frames(BT_ATTACKS[chg].charge_duration);
        check(chg_dash == 45.0f, "the charger's 0.75 s unlock Timer is 45 frames");
        bt_core_init(&A, 10u, 0, 1, BT_CHARACTER_KNIGHT, BT_WEAPON_NONE);
        A.wave = 1; A.n_live_groups = 0;
        for (int i = 0; i < BT_MAX; i++) bt_enemy_set_alive(&A, i, 0);
        for (int i = 0; i < BT_MAX_MAT; i++) A.matalive[i] = 0;
        A.ecount = 0; A.matcount = 0;
        A.px = 1000.0f; A.py = 500.0f;
        put(&A, 0, chg, 1200.0f, 500.0f);
        bt_core_step(&A, 0, obs, &rew, &term);
        A.eatk[0] = 1.0f;                       /* telegraph on the next frame */
        int armed_at = 0, dash_end = 0;
        for (int f = 1; f <= 200 && !dash_end; f++) {
            bt_core_step(&A, 0, obs, &rew, &term);
            if (A.echarge[0] > 0.0f) { if (!armed_at) armed_at = f; }
            else if (armed_at) dash_end = f;
        }
        check(armed_at > 0 && dash_end == armed_at + (int)chg_dash,
              "the charger's dash holds exactly the unlock Timer's frames");
    }

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
