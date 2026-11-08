/* tests/test_brotato_start_wave.cpp
 *
 * Regression for the starting-wave boot: BtState.start_wave and the wave
 * bt_core_reset enters.
 *
 * Source behavior pinned here:
 *   run_data.gd:549 (RunData.reset)  `current_wave = DebugService.starting_wave`
 *                                    -- a plain index assignment.  There is no
 *                                    catch-up gold/items/weapons/level/XP and no
 *                                    simulation of the skipped waves, so no RNG
 *                                    draws are consumed for them; the only other
 *                                    starting_wave uses (run_data.gd:659/662)
 *                                    override elite/boss *type* and apply only
 *                                    when the debug spawn-specific knobs are set
 *                                    (the bridge never sets them).
 *   BtState.start_wave               set by bt_core_init (default 1) and by the
 *                                    optional `start_wave` [env] key in puf_init;
 *                                    bt_core_reset never clears it (like
 *                                    `endless`), so it persists across episodes.
 *   bt_core_reset                    enters start_wave instead of the literal 1.
 *
 * The default (start_wave == 1) path must stay byte-identical, so the first
 * section asserts campaign determinism; tests/test_brotato_core.cpp carries the
 * full default-path hash coverage.
 *
 * Build and run:
 *     c++ -O2 -std=c++17 -ffp-contract=off -o build/test_brotato_start_wave \
 *         tests/test_brotato_start_wave.cpp -lm
 *     ./build/test_brotato_start_wave
 *
 * Exit code 0 = all checks passed.
 */

#include "../ocean/brotato/brotato_core.h"

#include <cstdio>
#include <cstring>

static int g_checks = 0;
static int g_failures = 0;

static void check(int cond, const char *msg) {
    g_checks++;
    if (!cond) { g_failures++; std::printf("  FAIL: %s\n", msg); }
}

static void check_near(float got, float want, float tol, const char *msg) {
    g_checks++;
    if (!(got >= want - tol && got <= want + tol)) {
        g_failures++;
        std::printf("  FAIL: %s (got %.6f want %.6f tol %g)\n", msg, got, want, tol);
    }
}

/* v3 observation index of `wave / BT_WAVES` (brotato_core.h bt_core_observe):
 * player, items, upgrades, then the enemy block. */
#define WAVE_OBS_IDX (BT_OBS_PLAYER + BT_OBS_ITEMS + BT_OBS_UPGRADES + BT_OBS_ENEMIES)

static BtState A, B, C, D, W, E, F;

static void boot(BtState *s, unsigned short start_wave) {
    bt_core_init(s, 1, 0, 1, BT_CHARACTER_DEMON, BT_WEAPON_NONE);
    if (start_wave != 1) {
        s->start_wave = start_wave;
        s->rng_ready = 0;          /* one-shot seeding: replay the prologue */
        bt_core_reset(s);
    }
}

int main(void) {
    std::printf("[1] campaign boot: start_wave defaults to 1 and enters wave 1\n");
    {
        boot(&A, 1);
        check(A.start_wave == 1, "bt_core_init defaults start_wave to 1");
        check(A.wave == 1, "campaign boot enters wave 1");
        check(A.tick_in_wave == 0, "wave 1 starts at tick 0");
        float obs[BT_OBS_SIZE];
        bt_core_observe(&A, obs);
        check_near(obs[WAVE_OBS_IDX], 1.0f / (float)BT_WAVES, 1e-6f, "observation reports wave 1");
    }

    std::printf("[2] the default path is deterministic and survives a reset\n");
    {
        boot(&B, 1);
        check(std::memcmp(&A, &B, sizeof(BtState)) == 0, "two campaign boots are byte-identical");
        /* a reset inside a run must not REWIND the streams (one-shot seeding):
           it keeps drawing from where the run left off instead of re-seeding. */
        bt_u64 rng_before = B.rng.state, utils_before = B.utils_rng.state;
        BtRng fresh, fresh_utils;
        bt_rng_seed(&fresh, B.seed);
        bt_rng_seed(&fresh_utils, B.seed);
        bt_core_reset(&B);
        check(B.rng.state != rng_before && B.utils_rng.state != utils_before,
              "a second reset advances the streams (the prologue draws again)");
        check(B.rng.state != fresh.state && B.utils_rng.state != fresh_utils.state,
              "a second reset does not rewind to the seeded state (one-shot seeding)");
        check(B.wave == 1 && B.start_wave == 1, "the second reset re-enters wave 1");
    }

    std::printf("[3] start_wave = 20 enters wave 20 directly\n");
    {
        boot(&C, 20);
        check(C.start_wave == 20, "start_wave stored");
        check(C.wave == 20, "the reset entered wave 20");
        check(C.tick_in_wave == 0, "the entered wave starts at tick 0");
        check(C.endless_max_wave == 20, "endless_max_wave latched to the entered wave");
        check_near(C.wave_duration, bt_wave_duration(C.zone, 20), 1e-3f,
                   "wave 20 uses the table duration");
        /* the run prologue consumes the entered wave's first frame (the recorded
         * row 0 is written after the fresh Main scene's first physics frame). */
        check_near(C.time_left, C.wave_duration - BT_DT, 1e-3f,
                   "the entered wave's timer is one prologue frame in");
        float obs[BT_OBS_SIZE];
        bt_core_observe(&C, obs);
        check_near(obs[WAVE_OBS_IDX], 20.0f / (float)BT_WAVES, 1e-6f, "observation reports wave 20");
    }

    std::printf("[4] the run's first wave grants the starting weapon\n");
    {
        /* The game grants add_starting_items_and_weapons() at boot, so a run
         * that starts at wave 20 must still be armed at tick 0. */
        static BtState G, H;
        bt_core_init(&G, 1, 0, 1, BT_CHARACTER_DEMON, (unsigned short)118);
        float o1[BT_OBS_SIZE]; bt_core_observe(&G, o1);
        bt_core_init(&H, 1, 0, 1, BT_CHARACTER_DEMON, (unsigned short)118);
        H.start_wave = 20; H.rng_ready = 0; bt_core_reset(&H);
        float o20[BT_OBS_SIZE]; bt_core_observe(&H, o20);
        check(o1[79] > 0.0f, "the wave-1 boot is armed (weapon id field non-zero)");
        check_near(o20[79], o1[79], 1e-6f, "the wave-20 boot is armed with the same weapon");
        check(H.wave == 20, "the armed boot still starts at wave 20");
    }

    std::printf("[5] the wave-20 boot is deterministic (re-seeded prologue)\n");
    {
        boot(&D, 20);
        check(std::memcmp(&C, &D, sizeof(BtState)) == 0,
              "two wave-20 boots from the same seed are byte-identical");
        check(D.start_wave == 20, "start_wave survives the entry");
    }

    std::printf("[6] no catch-up: skipped waves consume no draws\n");
    {
        /* Walking waves 1..20 burns a wave prologue per wave; the boot must not. */
        bt_core_init(&W, 1, 0, 1, BT_CHARACTER_DEMON, BT_WEAPON_NONE);  /* wave 1 */
        for (int w = 2; w <= 20; w++) bt_enter_wave(&W, w);
        check(W.wave == 20, "the walked state reaches wave 20");
        check(!(C.rng.state == W.rng.state && C.utils_rng.state == W.utils_rng.state),
              "the wave-20 boot does not simulate the skipped waves (streams differ)");
        check(C.rng.state != W.rng.state || C.utils_rng.state != W.utils_rng.state,
              "at least one stream differs from the walked run");
    }

    std::printf("[7] the campaign path still consumes the wave-1 prologue exactly once\n");
    {
        boot(&E, 1);
        bt_core_init(&F, 1, 0, 1, BT_CHARACTER_DEMON, BT_WEAPON_NONE);
        check(std::memcmp(&E, &F, sizeof(BtState)) == 0,
              "a boot with start_wave == 1 equals a plain bt_core_init");
        check(E.n_live_groups == F.n_live_groups, "wave-1 group schedule unchanged");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
