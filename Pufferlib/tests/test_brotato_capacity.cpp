/* tests/test_brotato_capacity.cpp
 *
 * Regression test for the implementation-capacity fail-closed contract
 * (ocean/brotato/brotato_capacity.h + brotato_capacity_host.h):
 *
 *   [1] a clean init/reset reports mask 0
 *   [2] a REAL forced overflow (the ordinary-enemy spawn queue) is detected by
 *       bt_capacity_mask, with the specific class bits set
 *   [3] bt_capacity_require_clean FAILS CLOSED: in a forked child it aborts
 *       (SIGABRT) rather than returning, so a trainer can never carry on with a
 *       truncated environment
 *   [4] the host diagnostic names the pool and the seed/wave/tick
 *   [5] GAME-DEFINED eviction (the plain-enemy perf cull, pool_cull_fallback) is
 *       NOT reported as a capacity loss
 *
 * Build and run:
 *   g++ -std=c++17 -O2 -Wno-narrowing -ffp-contract=off -I. -Iocean/brotato \
 *       tests/test_brotato_capacity.cpp -o /tmp/t_capacity -lm && /tmp/t_capacity
 */

#include "../ocean/brotato/brotato_core.h"
#include "../ocean/brotato/brotato_capacity_host.h"

#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_checks = 0, g_fail = 0;

static void check(int cond, const char* what) {
    g_checks++;
    if (!cond) { g_fail++; printf("  FAIL: %s\n", what); }
}

static int first_kind(void) {
    for (int k = 0; k < BT_NKINDS; k++) {
        if (BT_KIND_CLASS[k] == 0) return k;
    }
    return -1;
}

int main(void) {
    static BtState S;

    printf("[1] a clean init/reset reports no implementation-capacity loss\n");
    {
        static float obs[BT_OBS_SIZE];
        float rew = 0.0f, term = 0.0f;
        bt_core_init(&S, 1u, 0, 1, BT_CHARACTER_DEMON, BT_WEAPON_NONE);
        check(bt_capacity_mask(&S) == 0ull, "clean after init");
        bt_core_step(&S, 0, obs, &rew, &term);
        check(bt_capacity_mask(&S) == 0ull, "clean after one step");
    }

    printf("[2] a forced spawn-queue overflow is detected, with its class bits\n");
    {
        int ch = first_kind();
        bt_core_init(&S, 2u, 0, 1, BT_CHARACTER_DEMON, BT_WEAPON_NONE);
        S.n_pend_enemy = 0; S.pend_overflow = 0; S.pend_drop_enemy = 0;
        if (ch >= 0) {
            for (int i = 0; i < BT_MAX_PEND_ENEMY + 3; i++) bt_queue_enemy(&S, ch, 1.0f, 1.0f);
            unsigned long long m = bt_capacity_mask(&S);
            check(S.n_pend_enemy == BT_MAX_PEND_ENEMY, "queue stops at BT_MAX_PEND_ENEMY");
            check(S.pend_drop_enemy == 3, "every rejected push is counted");
            check((m & (1ull << BT_CAP_PEND)) != 0ull, "PEND class bit set");
            check((m & (1ull << BT_CAP_DROP_ENEMY)) != 0ull, "DROP_ENEMY class bit set");
            check(m != 0ull, "mask is nonzero");

            /* STICKY LATCH: a loss must survive the flags being cleared by the
             * episode reset that bt_core_step performs on `done` (and by a
             * same-tick death that zeroes a per-entity flag). */
            /* step WHILE the loss is live: bt_core_step latches it before its
             * end-of-episode reset can zero the per-episode flags. */
            { float o[BT_OBS_SIZE]; float rw = 0.0f, tm = 0.0f; bt_core_step(&S, 0, o, &rw, &tm); }
            S.n_pend_enemy = 0; S.pend_overflow = 0; S.pend_drop_enemy = 0;  /* reset-like */
            check(bt_capacity_mask(&S) != 0ull,
                  "the loss survives an episode reset (sticky latch)");
            check((bt_capacity_mask(&S) & (1ull << BT_CAP_DROP_ENEMY)) != 0ull,
                  "the latched class is still named");
            bt_capacity_clear(&S);
            check(bt_capacity_mask(&S) == 0ull, "bt_capacity_clear() acknowledges it");
        }
    }

    printf("[2b] a loss is reported even when it outlives nothing (same-tick death)\n");
    {
        static BtState D;
        bt_core_init(&D, 21u, 0, 1, BT_CHARACTER_DEMON, BT_WEAPON_NONE);
        /* simulate a shot whose identity set overflowed and which then died in
         * the same tick: the live scan cannot see it, the latch must. */
        D.wproj.p[0].alive = 1; D.wproj.p[0].ign_overflow = 1;
        D.wproj.used = 1;
        check((bt_capacity_mask(&D) & (1ull << BT_CAP_WPROJ_SET)) != 0ull,
              "live shot's set overflow is reported");
        D.wproj.p[0].alive = 0;   /* died in the same tick */
        check(bt_capacity_mask(&D) == 0ull, "the live scan alone loses it (expected)");
        bt_capacity_scan(&D);     /* what bt_core_step does before its reset */
        BT_CAP_LATCH(&D, BT_CAP_WPROJ_SET);
        D.wproj.p[0].ign_overflow = 0;
        check((bt_capacity_mask(&D) & (1ull << BT_CAP_WPROJ_SET)) != 0ull,
              "the latch reports it after the entity is gone");
    }

    printf("[3] bt_capacity_require_clean fails closed (SIGABRT), never returns\n");
    {
        int ch = first_kind();
        bt_core_init(&S, 3u, 0, 1, BT_CHARACTER_DEMON, BT_WEAPON_NONE);
        S.n_pend_enemy = 0; S.pend_overflow = 0;
        if (ch >= 0) for (int i = 0; i < BT_MAX_PEND_ENEMY + 3; i++) bt_queue_enemy(&S, ch, 1.0f, 1.0f);
        /* Prove the clean path DOES return, then that the dirty path cannot. */
        {
            static BtState C;
            bt_core_init(&C, 4u, 0, 1, BT_CHARACTER_DEMON, BT_WEAPON_NONE);
            bt_capacity_require_clean(&C, "clean-control");
            check(1, "clean state returns from require_clean");
        }
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) {
            /* child: must abort here */
            freopen("/dev/null", "w", stderr);
            bt_capacity_require_clean(&S, "forced-overflow");
            _exit(0);   /* reached only if the check failed to fail closed */
        }
        int st = 0;
        waitpid(pid, &st, 0);
        check(WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT,
              "a dirty state aborts (SIGABRT) instead of returning");
    }

    printf("[4] the host diagnostic names the pool and the seed/wave/tick\n");
    {
        char buf[192];
        bt_capacity_describe(&S, buf, (int)sizeof(buf));
        check(strstr(buf, "DROP_ENEMY") != NULL || strstr(buf, "PEND") != NULL,
              "diagnostic names a spawn-queue class");
        check(strstr(buf, "seed=") != NULL && strstr(buf, "wave=") != NULL &&
              strstr(buf, "tick=") != NULL, "diagnostic carries seed/wave/tick");
    }

    printf("[5] the live-enemy-slot ceiling fails closed until a cold arena exists\n");
    {
        static BtState G;
        bt_core_init(&G, 5u, 0, 1, BT_CHARACTER_DEMON, BT_WEAPON_NONE);
        check(bt_capacity_mask(&G) == 0ull, "clean before the ceiling is touched");
        G.pool_cull_fallback = 1;   /* the source's own perf cull: game-defined */
        check((bt_capacity_mask(&G) & (1ull << BT_CAP_ELITE_SLOTS)) == 0ull,
              "the game-defined plain cull is NOT a capacity loss");
        G.elite_slot_pressure = 1;  /* a boss/elite had to evict a plain enemy */
        check((bt_capacity_mask(&G) & (1ull << BT_CAP_ELITE_SLOTS)) != 0ull,
              "the BT_MAX live-slot ceiling is reported as a capacity loss");
    }

    printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
