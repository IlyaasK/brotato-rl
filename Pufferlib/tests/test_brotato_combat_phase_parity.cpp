// tests/test_brotato_combat_phase_parity.cpp -- GATED (host, no CUDA toolchain)
// verification of the intra-env combat schedule's correctness crux in
// ocean/brotato/brotato_gpu_combat.cuh. Named without a "gpu_" token on
// purpose: the fast test cycle SKIPs tests/test_brotato_*gpu_* (CUDA
// toolchain), and this one is pure host code and must be gated everywhere.
//
// The parallel schedule is "snapshot predicate scan -> first_hit[] lower bound
// -> lane 0 runs the ORIGINAL core body with the hint". The claim to verify is
// exactly:
//
//   (a) first_hit[] is a LOWER BOUND: no enemy slot below it is a valid hit, so
//       skipping it cannot lose a hit;
//   (b) the hinted body is byte-identical to the serial body (same hits, same
//       order, same RNG draws, same float updates).
//
// The pre-scan predicates in the header are BT_HD, so both halves run on the
// host here and the check needs no GPU. Fixtures are real Demon crowd states
// (a melee Ghost Scepter env and a ranged SMG env) with synthesized projectiles
// so #14/#18 see dense enemy sets and both the friendly and the enemy paths.
//
// Build (the runner does this): g++ -std=c++17 -O2 -Wno-narrowing
//   -ffp-contract=off -I. -Iocean/brotato tests/test_brotato_combat_phase_parity.cpp -lm
#include "../ocean/brotato/brotato_gpu_combat.cuh"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// Fast by default (the gated cycle runs every test); raise via env for a longer
// record run: BT_CP_CROWD, BT_CP_EVERY.
static int cp_env(const char *k, int dflt) {
    const char *v = getenv(k);
    return v && *v ? atoi(v) : dflt;
}

static const int ZONE = 0, SHOP_STEPS = 1;
static bt_u64 seed_for(int i) { return (bt_u64)(i == 0 ? 1u : (unsigned)i); }

// Kiting fixture policy (never touches the simulation).
static int kite_action(BtState *s, const float *o) {
    if (s->phase == 1) return 8;
    if (s->phase == 2) return 0;
    const int EN = 11 + 16 + 8;
    float wx = 0.0f, wy = 0.0f;
    for (int sl = 0; sl < BT_SLOTS; sl++) {
        float dx = o[EN + sl * 5 + 0] * BT_ARENA_W, dy = o[EN + sl * 5 + 1] * BT_ARENA_H;
        float d2 = dx * dx + dy * dy;
        if (d2 <= 1.0f) continue;
        float inv = 1.0f / sqrtf(d2);
        wx -= dx * inv * inv; wy -= dy * inv * inv;
    }
    if (s->px < 300.0f) wx += 1.0f;
    if (s->px > BT_ARENA_W - 300.0f) wx -= 1.0f;
    if (s->py < 300.0f) wy += 1.0f;
    if (s->py > BT_ARENA_H - 300.0f) wy -= 1.0f;
    int best = 0; float bd = -1e30f;
    for (int a = 1; a <= 8; a++) {
        float d = BT_MOVE_X[a] * wx + BT_MOVE_Y[a] * wy;
        if (d > bd) { bd = d; best = a; }
    }
    return best;
}

static void seed_projectiles(BtState *s) {
    for (int q = 0; q < BT_MAX_PROJ; q += 2) {
        float x = (float)((q * 197) % (int)BT_ARENA_W);
        float y = (float)((q * 311) % (int)BT_ARENA_H);
        float rot = (float)((q * 37) % 360) * (BT_PI / 180.0f);
        int pj = bt_spawn_projectile(s, x, y, rot, 420.0f, 5.0f, BT_SHAPE_CIRCLE,
                                     17.0f, 0.0f, 0.0f, rot, (q & 2) ? 1 : 0);
        if (pj >= 0) {
            s->prcrit_chance[pj] = 0.25f; s->prcrit_dmg[pj] = 2.0f;
            s->prburn_chance[pj] = 0.3f;  s->prburn_dmg[pj] = 1.0f; s->prburn_dur[pj] = 60.0f;
        }
    }
}

int main(void) {
    unsigned short ch = (unsigned short)bt_character_id_from_name("demon");
    unsigned short wp_melee = (unsigned short)bt_weapon_id_from_name("weapon_ghost_scepter_1");
    unsigned short wp_ranged = (unsigned short)bt_weapon_id_from_name("weapon_smg_1");
    const int NENV = 2;
    const int CROWD = cp_env("BT_CP_CROWD", 1500);
    const int EVERY = cp_env("BT_CP_EVERY", 100);
    std::vector<BtState> st((size_t)NENV);
    std::vector<float> obs((size_t)NENV * BT_OBS_SIZE, 0.0f);
    for (int i = 0; i < NENV; i++) {
        std::memset(&st[i], 0, sizeof(BtState));
        bt_core_init(&st[i], seed_for(i), ZONE, SHOP_STEPS, ch, i == 0 ? wp_melee : wp_ranged);
        bt_core_observe(&st[i], &obs[(size_t)i * BT_OBS_SIZE]);
    }

    int checks = 0, failures = 0;
    long states = 0, lw = 0, le = 0;
    int peak = 0;
    std::vector<int> hint(BTF_COMBAT_MAX_FIRST_HIT);

    for (int t = 0; t < CROWD; t++) {
        for (int i = 0; i < NENV; i++) {
            int a = kite_action(&st[i], &obs[(size_t)i * BT_OBS_SIZE]);
            float r = 0.0f, term = 0.0f;
            bt_core_step(&st[i], a, &obs[(size_t)i * BT_OBS_SIZE], &r, &term);
            int av = 0;
            for (int k = 0; k < BT_MAX; k++) if (st[i].ealive[k] && !st[i].eneutral[k]) av++;
            if (av > peak) peak = av;
        }
        if ((t % EVERY) != 0) continue;
        for (int i = 0; i < NENV; i++) {
            states++;
            // #14 bt_wproj_step: serial body vs hinted body
            BtState A = st[i], B;
            seed_projectiles(&A);
            B = A;
            for (int q = 0; q < BT_MAX_WPROJ; q++) if (B.wproj.p[q].alive) lw++;
            for (int q = 0; q < BT_MAX_WPROJ; q++) hint[q] = btf_combat_wproj_first_hit(&B, q);
            float ra = 0.0f, rb = 0.0f;
            (void)bt_wproj_step(&A, &ra);
            (void)bt_wproj_step_hint(&B, &rb, hint.data());
            checks++;
            if (ra != rb || std::memcmp(&A, &B, sizeof(BtState)) != 0) {
                failures++;
                std::printf("FAIL #14 wproj hint: t=%d env=%d (reward %g vs %g)\n", t, i, ra, rb);
            }
            // #18 enemy projectile loop: serial vs hinted
            A = st[i];
            seed_projectiles(&A);
            B = A;
            for (int q = 0; q < BT_MAX_PROJ; q++) if (B.pralive[q]) le++;
            for (int q = 0; q < BT_MAX_PROJ; q++) hint[q] = btf_combat_eproj_first_hit(&B, q);
            ra = 0.0f; rb = 0.0f;
            bt_eproj_phase(&A, &ra, 0);
            bt_eproj_phase(&B, &rb, hint.data());
            checks++;
            if (ra != rb || std::memcmp(&A, &B, sizeof(BtState)) != 0) {
                failures++;
                std::printf("FAIL #18 eproj hint: t=%d env=%d\n", t, i);
            }
            // #20 contact: serial probe vs the parallel min-index reduction
            A = st[i]; B = st[i];
            ra = 0.0f; rb = 0.0f;
            bt_player_contact_phase(&A, &ra);
            bt_player_contact_apply(&B, &rb);
            int h = -1;
            for (int k = 0; k < BT_MAX; k++) if (bt_player_contact_probe(&B, k)) { h = k; break; }
            B.epend = (h < 0) ? (short)-1 : (short)h;
            checks++;
            if (ra != rb || std::memcmp(&A, &B, sizeof(BtState)) != 0) {
                failures++;
                std::printf("FAIL #20 contact probe: t=%d env=%d (epend %d vs %d)\n",
                            t, i, (int)A.epend, (int)B.epend);
            }
        }
    }
    std::printf("  Demon crowd: %ld sampled states, peak %d enemies, %ld live player"
                " projectiles, %ld live enemy projectiles\n", states, peak, lw, le);
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
