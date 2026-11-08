/* tests/test_brotato_enemy_init.cpp
 *
 * Focused regression for the ORDER of a flushed enemy's creation/init draws
 * inside one frame (enemy creation / init RNG consumer ordering).
 *
 * Source behaviour that is pinned here:
 *   global/entity_spawner.gd:52-75   the drain pass (`queue_to_spawn` pop ->
 *                                    spawn_entity_birth -> EntityBirth.start,
 *                                    whose `rotation_degrees = rand_range(0,360)`
 *                                    is a global draw) runs INSIDE the spawner's
 *                                    own _physics_process walk, and it shuffles
 *                                    the four owner queues with three global words
 *                                    even when every queue is empty.
 *   entities/birth/entity_birth.gd:108-112
 *                                    the activation is `call_deferred("birth")`,
 *                                    i.e. a deferred boundary the engine flushes
 *                                    AFTER the frame's whole _physics_process
 *                                    walk -- so within one frame the drain words
 *                                    are consumed BEFORE the activated enemy's
 *                                    scene.instance() member initializers
 *                                    (enemy.gd:25 `_idle_playback_speed`),
 *                                    _ready() AttackBehavior get_cd and Unit.init
 *                                    `_speed_rand_value = rand_range(-r, r)`
 *                                    (unit.gd:77-81).
 *
 * Observable consequence the core must reproduce: the captured live Demon +
 * Ghost Scepter Danger-5 zone-0 endless run's first wave-1 enemy (a chaser-like
 * kind, stats.speed 250, speed_randomization 50) reports speed 262 at the tick
 * it appears -- 250 + a jitter of 12 -- and its birth position.  If the boundary
 * flush consumes its init words BEFORE the frame's drain words (the historical
 * order here) the same enemy reports 272 (jitter 22): the per-frame draw COUNT
 * is identical, only the words the enemy reads move by the drain's three.
 *
 * The second check is structural rather than a snapshot: it re-derives the
 * jitter from a saved RNG state at BOTH candidate offsets and requires the core
 * to have used the post-drain one.
 *
 * Build and run:
 *     c++ -O2 -std=c++17 -Wno-narrowing -ffp-contract=off -I. -Iocean/brotato \
 *         tests/test_brotato_enemy_init.cpp -o build/test_brotato_enemy_init -lm
 *     ./build/test_brotato_enemy_init
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
    if (!(fabsf(got - want) <= tol)) {
        g_failures++;
        printf("  FAIL: %s (got %.6f want %.6f)\n", msg, (double)got, (double)want);
    }
}

/* Persistent world belongs in static storage, not on the stack. */
static BtState A;
static float obs[BT_OBS_SIZE], rew, term;

static void step1(int action) {
    bt_core_step(&A, action, obs, &rew, &term);
}

/* The live capture's scripted action stream: 10 actions, 30 ticks each. */
static const int SCRIPT[10] = { 8, 2, 4, 6, 0, 3, 1, 5, 7, 0 };

/* Number of global words the kind's FRESH enemy-init sequence consumes before
 * (and excluding) the Unit speed jitter.  Read from the generated op table, so
 * the test never hardcodes a kind's init shape. */
static int jitter_word_offset(int kind, int* found) {
    int first = (int)BT_KIND_INIT_FRESH_FIRST[kind];
    int n = (int)BT_KIND_INIT_FRESH_N[kind];
    int words = 0;
    *found = 0;
    for (int i = 0; i < n; i++) {
        BT_CONST_PTR(BtEnemyInitOp) op = &BT_ENEMY_INIT_OPS[first + i];
        if ((int)op->dest == BT_INITDEST_SPEED_RAND && op->state == BT_INIT_ROOT_STATE) {
            *found = 1;
            return words;
        }
        words += 3;                 /* one rand_range = three global words */
    }
    return words;
}

/* The jitter the core must NOT have used: the same draw taken `words` earlier,
 * i.e. straight after the frame's drain instead of after the drain plus the
 * enemy's preceding init draws. */
static int jitter_at(BtRng base, int skip_words, int rbound) {
    BtRng r = base;
    for (int i = 0; i < skip_words; i++) (void)bt_rng_next(&r);
    return bt_rng_global_range_int(&r, -(float)rbound, (float)rbound);
}

int main(void) {
    printf("[1] Demon Ghost-Scepter wave-1 capture: flushed enemy reads the\n"
           "    post-drain words (speed 262, not 272)\n");
    {
        bt_core_init(&A, 1u, 0, 1, (unsigned short)BT_CHARACTER_DEMON,
                     (unsigned short)BT_WEAPON_IDX_WEAPON_GHOST_SCEPTER_1, 0u);
        A.particles_rng = 0;
        A.wave_ovr = 20.0f;                 /* the capture's recorded duration */
        A.rng_ready = 0;
        bt_core_reset(&A);
        check(A.ecount == 0, "no live enemy before the first birth completes");

        /* step to the frame in which the first wave-1 enemy activates, saving the
         * RNG state that frame starts from */
        BtRng frame_start = A.rng;
        for (int t = 1; t <= 123; t++) {
            int action = SCRIPT[((t - 1) / 30) % 10];
            if (t < 123) frame_start = A.rng;   /* state entering tick 123 */
            step1(action);
        }
        check(A.ecount == 1, "the first wave-1 enemy is live on tick 123");

        int slot = -1;
        for (int i = 0; i < BT_MAX; i++) if (A.ealive[i]) { slot = i; break; }
        check(slot >= 0, "the activated enemy occupies a slot");
        if (slot < 0) goto done;

        /* the captured live trace's own values (float32 obs: 0.1303288/0.3703337
         * encode this position, hp ratio 1, elite 0) */
        check_near(A.ex[slot], 1290.912842f, 1e-3f, "newborn x is the captured spawn x");
        check_near(A.ey[slot], 1336.832520f, 1e-3f, "newborn y is the captured spawn y");
        check_near(A.espeed_rand[slot], 12.0f, 1e-6f,
                   "speed jitter is the post-drain draw (12, not the 3-words-early 22)");
        check_near(A.espd[slot], 262.0f, 1e-3f,
                   "enemy speed reproduces the live capture's 262 (250 + 12)");

        /* structural: the jitter must be the draw that sits after the frame's
         * three drain words plus the enemy's preceding init draws */
        int found = 0;
        int off = jitter_word_offset((int)A.ekind[slot], &found);
        check(found, "the kind's fresh init sequence has a Unit speed jitter");
        int rbound = (int)BT_KINDS[A.ekind[slot]].speed_rand;
        check(rbound > 0, "the kind has a non-zero speed_randomization");
        int expect_words = 3 + off;         /* drain shuffle + preceding init draws */
        int want = jitter_at(frame_start, expect_words, rbound);
        int early = jitter_at(frame_start, off, rbound);
        check(want != early, "the two candidate words are distinguishable here");
        check((int)A.espeed_rand[slot] == want,
              "the jitter came from the word AFTER the frame's spawn drain");
        check((int)A.espeed_rand[slot] != early,
              "the jitter did not come from the pre-drain word");
    }

    printf("[2] synthetic chaser birth: same order on an activation frame\n");
    {
        int ch = -1;
        for (int i = 0; i < BT_NKINDS; i++) {
            if (BT_KIND_INIT_FRESH_N[i] == 0) continue;
            int f = 0;
            (void)jitter_word_offset(i, &f);
            if (f && BT_KIND_CLASS[i] == 0 && BT_KINDS[i].speed_rand > 0) { ch = i; break; }
        }
        check(ch >= 0, "a plain kind with a speed jitter exists");
        if (ch >= 0) {
            int rbound = (int)BT_KINDS[ch].speed_rand;
            bt_core_init(&A, 4242u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE, 0u);
            A.particles_rng = 0;
            A.n_live_groups = 0;
            A.n_pend_enemy = 0; A.n_births = 0; A.cur_spawn_delay = 0;
            bt_queue_enemy(&A, ch, 1500.0f, 1200.0f);
            for (int t = 0; t < 3; t++) step1(0);          /* the drain makes the birth */
            check(A.n_births == 1 && A.ecount == 0, "the drain made one birth");
            /* walk to the frame the countdown ends in, watching for the activation */
            int activated_at = -1;
            BtRng before = A.rng;
            for (int t = 0; t < 70 && activated_at < 0; t++) {
                before = A.rng;
                step1(0);
                if (A.ecount >= 1) activated_at = t;
            }
            check(activated_at >= 0, "the birth activates");
            int slot = -1;
            for (int i = 0; i < BT_MAX; i++) if (A.ealive[i]) { slot = i; break; }
            check(slot >= 0, "the synthetic enemy occupies a slot");
            if (slot >= 0) {
                int found = 0;
                int off = jitter_word_offset(ch, &found);
                int want = jitter_at(before, 3 + off, rbound);
                int early = jitter_at(before, off, rbound);
                check(found && want != early, "the synthetic frame is order-sensitive");
                check((int)A.espeed_rand[slot] == want,
                      "the synthetic newborn also reads its jitter after the drain");
                check((int)A.espeed_rand[slot] != early,
                      "the synthetic newborn does not read the pre-drain word");
            }
        }
    }

done:
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) printf("test_brotato_enemy_init: OK\n");
    return g_failures ? 1 : 0;
}
