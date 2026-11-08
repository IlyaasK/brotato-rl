/* tests/test_brotato_reward.cpp
 *
 * Deterministic regression tests for the potential-based reward shaping and the
 * episode-metric semantics (ocean/brotato/brotato_reward.h, the shaping hooks in
 * brotato_core.h bt_core_step, and bt_core_add_log).  Self-contained: includes
 * only the shared core header.  No Env, no raylib, no engine, no GPU.
 *
 * Build and run:
 *     g++ -std=c++17 -O2 -Wno-narrowing -ffp-contract=off -I. -Iocean/brotato \
 *         tests/test_brotato_reward.cpp -o /tmp/test_brotato_reward
 *     /tmp/test_brotato_reward
 *
 * Exit code 0 = all checks passed.  Prints "N checks, M failures" (the shape
 * tests/run_brotato_tests.sh parses).
 *
 * Coverage (behavioural only):
 *   - shaping OFF (every weight 0, the default) keeps the whole ledger exactly
 *     0 and leaves the reward stream untouched
 *   - shaping ON does not perturb the simulation: the BtState of a shaped run
 *     is bit-identical to an unshaped one except log/rew/episode_return
 *   - the reward delta over a run equals the logged shaping total (rw_total),
 *     and the per-group ledgers sum to it
 *   - with gamma = 1 the ledger telescopes exactly: rw_total == Phi(s_final) -
 *     Phi(s_0), i.e. F = gamma*Phi(s') - Phi(s) is what is actually added
 *     (Ng/Harada/Russell 1999: policy-invariant credit assignment)
 *   - each Phi group answers to its own state input (max-HP / harvesting /
 *     luck / banked materials behind the late_wave gate / item counts /
 *     owned pivot-weapon tier)
 *   - Log.perf is the wave REACHED (the game's DifficultyScore record), not a
 *     waves/20 fraction that saturates at 1.0, so it still ranks endless runs
 *
 * The run crosses a wave end (time_left is forced to 0) because that is where
 * Phi really moves: Demon converts half of its gold into Max HP there.
 */

#include "../ocean/brotato/brotato_core.h"

#include <stdio.h>
#include <string.h>

/* BtState is ~1.75 MiB (it embeds two persistent contact worlds); big states
 * MUST be static, never locals -- a 1.75 MiB stack frame overflows the default
 * stack (tests/test_brotato_core.cpp keeps its state in BSS for this reason). */
static BtState g_off, g_on, g_scratch;
static float g_obs[BT_OBS_SIZE];

static int g_checks = 0;
static int g_failures = 0;

static void check(int cond, const char* msg) {
    g_checks++;
    if (!cond) { g_failures++; printf("  FAIL: %s\n", msg); }
}
static void check_near(float got, float want, float rel, const char* msg) {
    g_checks++;
    if (!(bt_fabsf(got - want) <= rel * (1.0f + bt_fabsf(want)))) {
        g_failures++;
        printf("  FAIL: %s (got %.6f want %.6f)\n", msg, got, want);
    }
}

/* One env of the Demon/endless profile.  `shaping` selects the configured
 * weights; the loadout never changes, so the two runs stay comparable. */
static void setup(BtState* s, unsigned int seed, int shaping) {
    memset(s, 0, sizeof(*s));
    bt_core_init(s, (bt_u64)seed, 0, 1, BT_CHARACTER_DEMON,
                 bt_weapon_id_from_name("weapon_ghost_scepter"));
    /* max_hp is recomputed from the raw stat each step, so raise the STAT, not
     * the derived field: the run must survive the horizon (wave 9 has no
     * artificial cap in this test, and a random policy would die early). */
    s->stat_raw[BT_RW_STAT_MAX_HP] = 1.0e6f;
    s->stat_gain[BT_RW_STAT_MAX_HP] = 1.0f;
    s->hp = 1.0e6f;
    s->materials = 130.0f;            /* Demon: 50%/13-chunk -> +5 Max HP */
    if (!shaping) return;
    s->rew.gamma = 1.0f;              /* exact telescoping below */
    s->rew.hp = 0.5f;
    s->rew.harvest = 0.5f;
    s->rew.luck = 0.5f;
    s->rew.materials = 0.05f;
    s->rew.late_wave = 1;
    s->rew.items = 0.5f;
    s->rew.pivot = 1.0f;
    s->rew.pivot_family[0] = BT_WEAPONS[bt_weapon_index_from_id("weapon_pruner")].family;
    s->rew.pivot_family[1] = BT_WEAPONS[bt_weapon_index_from_id("weapon_potato_thrower")].family;
    s->rew.pivot_family[2] = BT_WEAPONS[bt_weapon_index_from_id("weapon_excalibur")].family;
    s->rew.on = 1;
}

int main(void) {
    setbuf(stdout, NULL);   /* keep the progress lines even if a check crashes */
    const int horizon = 900;
    printf("[1] shaping OFF is exactly 0 and ON leaves the sim untouched\n");
    setup(&g_off, 1234u, 0);
    setup(&g_on, 1234u, 1);
    float scratch[BT_RW_NTERMS];
    float phi0 = bt_reward_phi(&g_on, scratch);

    int terminated = 0;
    float reward_off = 0.0f, reward_on = 0.0f;
    for (int t = 0; t < horizon; t++) {
        if (t == 500) { g_off.time_left = 0.0f; g_on.time_left = 0.0f; }  /* end wave 1 */
        int action = 1 + (t * 7) % 8;                                     /* fixed walk */
        float r_off = 0.0f, term_off = 0.0f, r_on = 0.0f, term_on = 0.0f;
        bt_core_step(&g_off, action, g_obs, &r_off, &term_off);
        bt_core_step(&g_on, action, g_obs, &r_on, &term_on);
        reward_off += r_off;
        reward_on += r_on;
        if (term_off || term_on) { terminated = 1; break; }
    }
    check(!terminated, "no episode ended inside the horizon");
    check(g_off.log.rw_total == 0.0f, "OFF: log.rw_total is exactly 0");
    int ledger_zero = 1;
    for (int i = 0; i < BT_RW_NTERMS; i++) if (g_off.log.rw[i] != 0.0f) ledger_zero = 0;
    check(ledger_zero, "OFF: every log.rw[i] is exactly 0");

    /* Neutralise only what shaping may legitimately change: the ledger, the
     * config and the accumulated return.  All other state must be identical. */
    Log log_off = g_off.log, log_on = g_on.log;
    BtReward rew_off = g_off.rew, rew_on = g_on.rew;
    float ret_off = g_off.episode_return, ret_on = g_on.episode_return;
    memset(&g_off.log, 0, sizeof(g_off.log));
    memset(&g_on.log, 0, sizeof(g_on.log));
    memset(&g_off.rew, 0, sizeof(g_off.rew));
    memset(&g_on.rew, 0, sizeof(g_on.rew));
    g_off.episode_return = 0.0f;
    g_on.episode_return = 0.0f;
    check(memcmp(&g_off, &g_on, sizeof(BtState)) == 0, "ON vs OFF: BtState bit-identical");
    g_off.log = log_off; g_on.log = log_on;
    g_off.rew = rew_off; g_on.rew = rew_on;
    g_off.episode_return = ret_off; g_on.episode_return = ret_on;

    printf("[2] the reward delta is the logged shaping total\n");
    check(g_on.log.rw_total != 0.0f, "the ledger moved (wave-end gold->max_hp)");
    check_near(reward_on - reward_off, g_on.log.rw_total, 1e-4f,
               "reward(ON) - reward(OFF) == log.rw_total");
    float group_sum = 0.0f;
    for (int i = 0; i < BT_RW_NTERMS; i++) group_sum += g_on.log.rw[i];
    check_near(group_sum, g_on.log.rw_total, 1e-3f, "sum of group ledgers == rw_total");

    printf("[3] gamma = 1 telescopes to Phi(s_final) - Phi(s_0)\n");
    float phi1 = bt_reward_phi(&g_on, scratch);
    check_near(g_on.log.rw_total, phi1 - phi0, 1e-3f, "ledger == Phi(s_final) - Phi(s_0)");

    printf("[4] every Phi group answers to its own state input\n");
    BtState* s = &g_scratch;
    float part[BT_RW_NTERMS];
    setup(s, 7u, 1);
    s->item_owned[BT_ITEM_IDX_PADDING] += 2;
    bt_reward_phi(s, part);
    check(part[4] > 0.0f, "item counts pay (Padding)");
    s->wpn_id[1] = (unsigned short)bt_weapon_index_from_id("weapon_pruner");
    s->wpn_tier[1] = 3;
    bt_reward_phi(s, part);
    check(part[5] > 0.0f, "owned pivot weapon pays, by tier");
    s->materials = 100.0f;
    bt_reward_phi(s, part);
    check(part[3] > 0.0f, "banked materials pay from late_wave on");
    s->wave = 1;
    s->rew.late_wave = 20;
    bt_reward_phi(s, part);
    check(part[3] == 0.0f, "banked materials are gated by late_wave");
    s->rew.harvest = 1.0f;
    bt_core_stat_add(s, BT_RW_STAT_HARVESTING, 10.0f);
    bt_reward_phi(s, part);
    check(part[1] > 0.0f, "harvest group reads stat_harvesting");
    s->rew.hp = 1.0f;
    bt_reward_phi(s, part);
    check(part[0] > 0.0f, "hp group reads stat_max_hp");
    s->rew.on = 0;
    check(bt_reward_phi(s, part) == 0.0f, "shaping off => Phi is exactly 0");

    printf("[5] Log.perf is the wave reached (the game's run record)\n");
    BtState* m = &g_scratch;
    setup(m, 99u, 0);
    m->wave = 7;
    m->kills = 5;
    m->ticks = 11;
    bt_core_add_log(m);
    check(m->log.perf == 7.0f, "log.perf == wave reached (7)");
    check(m->log.score == 5.0f, "log.score == kill count (5)");
    check(m->log.n == 1.0f && m->log.episode_length == 11.0f, "log.n / episode_length");
    /* Endless: the metric must not saturate (a waves/20 fraction would cap at
     * 1.0 and rank wave 21 equal to wave 40). */
    BtState* e = &g_scratch;
    setup(e, 99u, 0);
    e->wave = 37;
    e->kills = 120;
    bt_core_add_log(e);
    check(e->log.perf == 37.0f, "log.perf == 37 in endless (no saturation)");

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures != 0;
}
