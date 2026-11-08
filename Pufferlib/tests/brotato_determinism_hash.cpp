/* tests/brotato_determinism_hash.cpp
 *
 * Bit-exactness gate for BtState layout / capacity work.
 *
 * Replays a FIXED (seed, action schedule) trajectory for a set of profiles
 * (Bull / Knight / Demon, campaign + endless, with and without every item
 * granted) and hashes the three streams the RL contract is defined on:
 *
 *     observation   every bt_core_step's obs[] vector (BT_OBS_SIZE floats)
 *     reward        the step's reward float
 *     terminal      the step's terminal float
 *
 * plus a curated fingerprint of the game-visible scalar state after each step
 * (hp/wave/kills/positions/rng/log ...), so a layout change that silently moves
 * a field is caught even if the obs encoding happened not to read it.
 *
 * Any compaction / re-layout of BtState MUST leave every digest printed here
 * byte-identical.  Record the "before" digests first:
 *
 *   c++ -std=c++17 -O2 -Wno-narrowing -ffp-contract=off -I. -Iocean/brotato \
 *       -o build/brotato_determinism_hash tests/brotato_determinism_hash.cpp -lm
 *   ./build/brotato_determinism_hash > /tmp/before.txt
 *   ... edit ... rebuild ...
 *   ./build/brotato_determinism_hash > /tmp/after.txt && diff /tmp/before.txt /tmp/after.txt
 *
 * Every profile runs twice: "natural" (the shipped wave durations) and
 * "ff" (wave_ovr = 1.0 s waves, the trace-replay hook) so the deep-wave code
 * paths -- elite/horde/boss schedule, endless group rebuild, the wave-10k
 * reschedule -- are exercised inside a short run.  All runs are played "god
 * mode" (a zero-cost iframe/hp poke that consumes no sim RNG) so a profile
 * survives to the deep schedule instead of dying in wave 1; the poke itself is
 * deterministic, so before/after comparison is unaffected.
 *
 * Usage: ./brotato_determinism_hash [steps] [seeds]
 *        defaults: 40000 natural steps, 4x that for the fast-forward runs,
 *                  3 seeds
 */

#include "../ocean/brotato/brotato_core.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/* --------------------------------------------------------------- FNV-1a --- */

static uint64_t g_hash;

static void mix(const void* p, size_t n) {
    const unsigned char* b = (const unsigned char*)p;
    for (size_t i = 0; i < n; i++) { g_hash ^= b[i]; g_hash *= 1099511628211ull; }
}
static void mix_f(float v)    { mix(&v, 4); }
static void mix_i(int v)      { mix(&v, 4); }
static void mix_u(uint32_t v) { mix(&v, 4); }

/* Curated, layout-independent fingerprint of the game-visible state.  Only
 * named fields -- never raw bytes, which would pick up padding. */
static void mix_state(const BtState& s) {
    mix_f(s.hp); mix_f(s.max_hp); mix_f(s.px); mix_f(s.py);
    mix_f(s.materials); mix_f(s.xp); mix_f(s.dmg_taken);
    mix_f(s.episode_return); mix_f(s.time_left);
    mix_f(s.speed_pct); mix_f(s.dodge); mix_f(s.dmg_pct); mix_f(s.crit);
    mix_f(s.luck); mix_f(s.harvest); mix_f(s.curse);
    mix_f(s.wpn_cd[0]); mix_f(s.wpn_melee_rot[0]);
    mix_i(s.level); mix_i(s.wave); mix_i(s.ticks); mix_i(s.kills);
    mix_i(s.ecount); mix_i(s.phase); mix_i(s.done); mix_i(s.won);
    mix_i(s.waves_cleared); mix_i(s.matcount); mix_i(s.pet_n);
    mix_i((int)s.lvl_offers[0]); mix_i((int)s.lvl_prev[0]);
    mix_i(s.wpn_count); mix_i(s.n_live_groups); mix_i(s.n_births);
    mix_i(s.n_death_events); mix_i((int)s.pool_alloc);
    mix_i((int)s.wproj.n_live); mix_i((int)s.expl.n_spawned);
    mix_i(s.endless_max_wave);
    mix(&s.rng.state, sizeof(s.rng.state));
    mix(&s.utils_rng.state, sizeof(s.utils_rng.state));
    mix(&s.log.n, sizeof(s.log.n));
    mix(&s.log.episode_return, sizeof(s.log.episode_return));
    mix(&s.log.rw_total, sizeof(s.log.rw_total));
    for (int i = 0; i < BT_RW_NTERMS; i++) mix_f(s.log.rw[i]);
}

/* ------------------------------------------------------- action schedule --- */

/* Stateless, deterministic, independent of the sim's own RNG. */
static int action_for(int step, int prof) {
    uint32_t h = (uint32_t)step * 2654435761u ^ (uint32_t)(prof + 1) * 40503u;
    h ^= h >> 13; h *= 2246822519u; h ^= h >> 16;
    return (int)(h % (uint32_t)BT_ACTIONS);
}

/* --------------------------------------------------------------- profiles -- */

typedef struct {
    uint64_t       seed;
    int            steps;
    int            zone;          /* 0 = zone_1 campaign */
    int            shop_steps;
    unsigned short character;
    unsigned short weapon;
    int            endless;
    int            all_items;     /* grant every BT_NITEMS row after init */
    int            ff;            /* fast-forward: wave_ovr = 1.0 s waves */
    const char*    name;
} Profile;

static void run_profile(const Profile& p, uint64_t* total) {
    static BtState S;
    static float obs[BT_OBS_SIZE];
    bt_core_init(&S, p.seed, p.zone, p.shop_steps, p.character, p.weapon);
    if (p.ff) { S.wave_ovr = 1.0f; bt_core_reset(&S); }
    S.endless = (unsigned char)(p.endless ? 1 : 0);
    /* Stress HP so the profile reaches the deep wave schedule instead of dying
     * in wave 1.  Deterministic, sim-RNG-free (the same route
     * tests/demon_guide_bot_run.cpp's bonus_hp uses). */
    bt_core_stat_add(&S, 0, 1.0e6f);
    bt_core_stat_sync(&S);
    S.hp = S.max_hp;
    if (p.all_items) {
        for (int it = 0; it < BT_NITEMS; it++)
            bt_apply_item(&S, (unsigned short)it);
    }

    g_hash = 1469598103934665603ull;
    int steps_run = 0;
    for (int i = 0; i < p.steps; i++) {
        int a = action_for(i, (int)(p.character * 7 + p.endless * 3 + p.all_items + p.ff * 5));
        if (S.phase != 0) {
            /* shop / level-up: fold the deterministic index into a LEGAL action
             * (mask-driven) so the episode always progresses to the next wave. */
            unsigned char mask[BT_ACTIONS];
            bt_action_mask(&S, mask);
            int n = 0;
            for (int k = 0; k < BT_ACTIONS; k++) if (mask[k]) n++;
            if (n > 0) {
                int pick = (a % n);
                for (int k = 0; k < BT_ACTIONS; k++)
                    if (mask[k] && pick-- == 0) { a = k; break; }
            }
        }
        /* Deterministic, sim-RNG-free immortality (see the header comment). */
        if (S.iframe <= 0.0f) S.iframe = 1000.0f;
        if (S.hp < S.max_hp) S.hp = S.max_hp;
        float rew = 0.0f, term = 0.0f;
        bt_core_step(&S, a, obs, &rew, &term);
        mix(obs, sizeof(float) * BT_OBS_SIZE);
        mix_f(rew);
        mix_f(term);
        mix_state(S);
        steps_run = i + 1;
        if (term != 0.0f) break;    /* episode over: digest the truncated run */
    }
    printf("%-40s seed=%-20llu steps=%-7d tick=%-7d wave=%-4d maxw=%-5d kills=%-6d done=%d won=%d  %016llx\n",
           p.name, (unsigned long long)p.seed, steps_run, S.ticks, S.wave,
           S.endless_max_wave, S.kills, (int)S.done, (int)S.won,
           (unsigned long long)g_hash);
    *total ^= g_hash;
    *total *= 1099511628211ull;
}

int main(int argc, char** argv) {
    int steps = argc > 1 ? atoi(argv[1]) : 40000;
    int seeds = argc > 2 ? atoi(argv[2]) : 3;

    unsigned short sword = (unsigned short)bt_weapon_index_from_id("weapon_sword");
    if ((int)sword == (int)BT_WEAPON_NONE) sword = BT_WEAPON_NONE;
    unsigned short scepter = 118;   /* Ghost Scepter tier 1 (guide start) */
    {
        int idx = bt_weapon_index_from_id("weapon_ghost_scepter");
        if (idx != (int)BT_WEAPON_NONE) scepter = (unsigned short)idx;
    }

    printf("=== BtState determinism hash (sizeof=%zu) ===\n", sizeof(BtState));
    uint64_t total = 1469598103934665603ull;
    const uint64_t base_seed = 0x5eed0001ull;

    for (int s = 0; s < seeds; s++) {
        uint64_t seed = base_seed + 0x9e3779b97f4a7c15ull * (uint64_t)s;
        for (int endless = 0; endless <= 1; endless++) {
            for (int items = 0; items <= 1; items++) {
                for (int ff = 0; ff <= 1; ff++) {
                    unsigned short chars[3] = { BT_CHARACTER_BULL, BT_CHARACTER_KNIGHT,
                                                BT_CHARACTER_DEMON };
                    unsigned short wpns[3]  = { BT_WEAPON_NONE, sword, scepter };
                    const char*    cnames[3] = { "bull", "knight", "demon" };
                    for (int k = 0; k < 3; k++) {
                        Profile p;
                        p.seed = seed; p.steps = ff ? steps * 4 : steps;
                        p.zone = 0; p.shop_steps = 4;
                        p.character = chars[k]; p.weapon = wpns[k];
                        p.endless = endless; p.all_items = items; p.ff = ff;
                        static char namebuf[8][3][160];
                        snprintf(namebuf[ff][k], sizeof(namebuf[ff][k]),
                                 "%s/%s%s%s", cnames[k],
                                 endless ? "endless" : "campaign",
                                 items ? "/allitems" : "",
                                 ff ? "/ff" : "");
                        p.name = namebuf[ff][k];
                        run_profile(p, &total);
                    }
                }
            }
        }
    }
    printf("=== TOTAL %016llx ===\n", (unsigned long long)total);
    return 0;
}
