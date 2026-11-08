/* Particle-RNG model regression (BtState.particles_rng).
 *
 * The sim transcribes the source's player running-smoke CPUParticles2D
 * (brotato_player_visual.h: each restarted particle consumes Particle.seed
 * (bt_rng_next) + 9 bt_math_randf = 10 global draws, cpu_particles_2d.cpp:863-912).
 * The RL bridge clears every CPUParticles2D's emitting flag, so in the
 * configuration it records the game takes the engine's
 * `if (!emitting) { p.active = false; continue; }` branch and consumes nothing.
 * particles_rng = 0 must mirror exactly that: no per-particle draw and no
 * active particle, with everything else (the visual clock) unchanged. */
#include "../ocean/brotato/brotato_core.h"
#include <cstdio>

static int g_checks = 0, g_failures = 0;
static void check(bool ok, const char *what) {
    g_checks++;
    if (!ok) { g_failures++; std::printf("FAIL: %s\n", what); }
}

static BtState A;   /* particles_rng = 1 (legacy source draws) */
static BtState B;   /* particles_rng = 0 (bridge-suppressed)   */

int main(void) {
    bt_core_init(&A, 1u, 0, 1, (unsigned short)BT_CHARACTER_DEMON,
                 (unsigned short)BT_WEAPON_IDX_WEAPON_GHOST_SCEPTER_1);
    bt_core_init(&B, 1u, 0, 1, (unsigned short)BT_CHARACTER_DEMON,
                 (unsigned short)BT_WEAPON_IDX_WEAPON_GHOST_SCEPTER_1);
    check(A.particles_rng == 1, "bt_core_init defaults particles_rng to 1 (legacy)");
    B.particles_rng = 0;
    check(A.rng.state == B.rng.state, "the flag does not change the seeded stream");

    /* The run prologue (landing, tick_in_wave <= 72) draws no particle, so the
     * fresh streams must stay equal until the smoke starts emitting. */
    const bt_u64 a0 = A.rng.state;
    for (int t = 1; t <= 72; t++) {
        A.tick_in_wave = (unsigned short)t; B.tick_in_wave = (unsigned short)t;
        bt_player_visual_step(&A, 1, 1.0f);
        bt_player_visual_step(&B, 1, 1.0f);
    }
    check(A.rng.state == a0 && B.rng.state == a0,
          "the landing window draws no particle randomness in either model");

    /* After the landing the smoke emits while the player moves: with the legacy
     * model each restarted particle costs 10 draws; suppressed, none. */
    for (int t = 73; t <= 260; t++) {
        A.tick_in_wave = (unsigned short)t; B.tick_in_wave = (unsigned short)t;
        bt_player_visual_step(&A, 1, 1.0f);
        bt_player_visual_step(&B, 1, 1.0f);
    }
    check(A.rng.state != a0, "particles_rng = 1 consumes the running-smoke draws");
    check(B.rng.state == a0, "particles_rng = 0 consumes no particle draw");
    check(A.player_fx_active != 0, "particles_rng = 1 leaves particles active");
    check(B.player_fx_active == 0, "particles_rng = 0 leaves none active (the bridge state)");
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
