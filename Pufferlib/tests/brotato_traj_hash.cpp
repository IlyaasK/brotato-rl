// Cross-build trajectory hash for the shared Brotato core.
//
// The CUDA device kernel and the host reference inside
// tests/test_brotato_cuda.cu agree bit-for-bit by construction (that test
// asserts it per step). This program answers the *other* question: does the
// standalone CPU build - which build.sh compiles with clang -O2 -mavx2 -mfma and
// no -ffp-contract flag, unlike the CUDA host pass - produce the same numbers?
//
// It replays exactly the same trajectory as layer G of tests/test_brotato_cuda.cu
// (same seeds, same action schedule, same fields hashed) and prints one FNV-1a
// digest, so the three builds can be compared directly:
//
//   clang++ -O2 -mavx2 -mfma -I ocean/brotato -o /tmp/hash_cpu tests/brotato_traj_hash.cpp
//   clang++ -O2 -mavx2 -mfma -ffp-contract=off -I ocean/brotato -o /tmp/hash_cpu_nofma tests/brotato_traj_hash.cpp
//   g++    -O2 -ffp-contract=off -I ocean/brotato -o /tmp/hash_gcc tests/brotato_traj_hash.cpp
//
// Usage: ./hash [steps] [character_token] [weapon_token]
//        defaults: 1500 bull none
#include "brotato_core.h"
#include "brotato_rng.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static uint64_t g_hash = 1469598103934665603ull;

static void mix(const void *p, size_t n) {
    const unsigned char *b = (const unsigned char *)p;
    for (size_t i = 0; i < n; i++) {
        g_hash ^= b[i];
        g_hash *= 1099511628211ull;
    }
}
static void mix_f(float v) { mix(&v, 4); }
static void mix_i(int v) { mix(&v, 4); }

static int action_for(int step, int env) {
    unsigned int h = (unsigned int)step * 2654435761u ^ (unsigned int)(env + 1) * 40503u;
    h ^= h >> 13;
    h *= 2246822519u;
    h ^= h >> 16;
    return (int)(h % (unsigned int)BT_ACTIONS);
}

static void mix_state(const BtState &s, int env, int step) {
    mix_f(s.hp);
    mix_f(s.max_hp);
    mix_f(s.px);
    mix_f(s.py);
    mix_f(s.materials);
    mix_f(s.xp);
    mix_f(s.dmg_taken);
    mix_f(s.episode_return);
    mix_f(s.time_left);
    mix_f(s.wpn_cd[0]);
    mix_f(s.wpn_melee_rot[0]);
    mix_i(s.level);
    mix_i(s.wave);
    mix_i(s.ticks);
    mix_i(s.kills);
    mix_i(s.ecount);
    mix_i(s.phase);
    mix_i((int)s.lvl_offers[0]);
    mix_i(s.wpn_count);
    mix_i(env);
    mix_i(step);
    mix(&s.rng.state, sizeof(s.rng.state));
    mix(&s.log.n, sizeof(s.log.n));
}

int main(int argc, char **argv) {
    const int N = 8;
    const int steps = argc > 1 ? std::atoi(argv[1]) : 1500;
    const char *character = argc > 2 ? argv[2] : "bull";
    const char *weapon = argc > 3 ? argv[3] : "none";
    const int ZONE = 0, SHOP_STEPS = 1;
    const unsigned short ch = bt_character_id_from_name(character);
    const unsigned short wp = (unsigned short)bt_weapon_id_from_name(weapon);

    static BtState s[8];
    static float obs[8 * BT_OBS_SIZE];
    float rew[8], term[8];

    for (int i = 0; i < N; i++)
        bt_core_init(&s[i], (i == 0) ? 1ull : (unsigned long long)i, ZONE, SHOP_STEPS, ch, wp);

    for (int t = 0; t < steps; t++) {
        for (int i = 0; i < N; i++) {
            int a = action_for(t, i);
            bt_core_step(&s[i], a, &obs[i * BT_OBS_SIZE], &rew[i], &term[i]);
            mix(&obs[i * BT_OBS_SIZE], BT_OBS_SIZE * sizeof(float));
            mix_f(rew[i]);
            mix_f(term[i]);
            mix_state(s[i], i, t);
        }
    }
    std::printf("%s steps=%d character=%s weapon=%s hash=%016llx\n", argv[0] ? "traj" : "traj", steps,
                character, weapon, (unsigned long long)g_hash);
    std::printf("env0: hp=%.9g px=%.9g py=%.9g mats=%.9g xp=%.9g level=%d wave=%d ticks=%d kills=%d "
                "ecount=%d phase=%d rng=%llu\n", s[0].hp, s[0].px, s[0].py, s[0].materials, s[0].xp,
                s[0].level, s[0].wave, s[0].ticks, s[0].kills, s[0].ecount, s[0].phase,
                (unsigned long long)s[0].rng.state);
    return 0;
}
