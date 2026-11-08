#!/usr/bin/env python3
"""Evaluate one exported Brotato policy in BOTH the real game and the simulator.

Given a policy artifact from ``ocean/brotato/policy/export_policy.py`` and a list
of seeds, this tool:

  * runs one headless real-game episode per seed through
    ``ocean/brotato/bridge/run_game.sh`` (up to ``--parallel`` at a time; every
    episode gets its own ``BROTATO_RUN_DIR`` / ``BROTATO_ISO_HOME`` /
    ``BROTATO_TAG`` so they cannot collide), then parses the bridge trace, the
    summary, the engine's own ``user/log.txt`` and the game save to report:
    waves reached, win (wave 20 cleared), death wave, materials, level, kills,
    shop purchases, stalls and wall time;
  * runs the same seeds closed-loop through the shared CPU core
    (``ocean/brotato/brotato_core.h``) with the same policy weights applied by
    ``ocean/brotato/policy/policy_reference.py``'s reference forward pass, and
    reports the same metrics;
  * prints the two tables plus a sim-vs-game gap table.

The sim driver is a tiny C++ program emitted into the eval root and compiled
with the same flags the repo's own core driver uses (``tests/`` and
``bridge/replay_cpu.py``), so nothing is added to the repo.

Usage::

    # real-game + sim, 4 seeds, full run to wave 20
    python3 ocean/brotato/bridge/eval_game.py \
        --policy /path/to/policy.json --action-source policy \
        --seeds 1,2,3,4 --parallel 4

    # fast smoke: 30 simulated seconds, scripted action source
    python3 ocean/brotato/bridge/eval_game.py --action-source script \
        --seeds 1,2 --max-seconds 30 --watchdog 180 --sample-every 1

Failure modes and the metric definitions are documented in ``EVAL.md`` next to
this file.  Nothing here is committed to git by itself and no retail asset is
written into the repository.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import math
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]                      # .../Pufferlib
RUN_GAME = HERE / "run_game.sh"
POLICY_DIR = REPO / "ocean" / "brotato" / "policy"

# Game character ids (brotato_characters.h:25-28) / core sentinel.
CHARACTERS = {
    "character_bull": 0,
    "character_knight": 1,
    "character_demon": 2,
}
BT_CHARACTER_NONE = 0xFFFF
BT_WEAPON_NONE = 0xFFFF

# Wave durations, zone 0 (brotato_waves.h BT_ZONE_WAVE_DUR[0]); the 20-wave
# campaign is 1050 simulated seconds before shop/level-up ticks.
TOTAL_WAVE_SECONDS = 1050

# Scenes the bridge legitimately visits; anything else means the run walked into
# a menu the RL driver does not drive.
KNOWN_SCENES = {"Main", "Shop", "EndRun", "PauseScreen"}

# ---------------------------------------------------------------------------
# Sim driver (emitted, compiled, then driven line by line)
# ---------------------------------------------------------------------------
SIM_GLUE_SRC = r"""
/* eval_sim_policy.c - the native PufferNet forward behind an opaque C API.
 *
 * This translation unit is the only place that sees puffernet's own types; it
 * compiles as C (src/puffercpu.c is C) and is linked into the C++ sim driver.
 * The forward is exactly what policy_reference.py re-derives and what the
 * trainer's CPU path runs (native_forward_probe.c):
 *   linear(encoder) -> mingru(layers) -> linear(decoder),
 * with the decoder's first `actions` columns the logits and the last the value.
 * The action is the plain argmax over those logits: the bridge's GDScript policy
 * is fed an empty mask (rl_policy.gd calls policy.step(obs) with no mask), so a
 * masked argmax would not be the game's action.  bt_policy_sample replaces that
 * argmax with the softmax + dedicated-PRNG draw the game's rl_policy.gd runs
 * when the harness selects --action-mode sample (the default).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/puffercpu.c"

/* Largest action count the sampler's probability scratch holds (the live
 * contract's BT_ACTIONS is 31; the artifact declares its own count). */
#define BT_SAMPLE_MAX_ACTIONS 64

typedef struct {
    Weights* weights;
    Linear* encoder;
    Linear* decoder;
    MinGRU* gru;
    int actions;
} BtPolicy;

/* Load the artifact's weights (raw float32, trainer registration order).
 * Returns NULL and fills `err` when the file's float count does not match the
 * declared layout, so a mismatched artifact can never run half-loaded. */
BtPolicy* bt_policy_load(const char* path, int obs, int hidden, int layers,
                         int actions, char* err, int err_len) {
    FILE* file = fopen(path, "rb");
    if (!file) {
        snprintf(err, (size_t)err_len, "cannot open weights %s", path);
        return NULL;
    }
    if (fseek(file, 0, SEEK_END)) { fclose(file); snprintf(err, (size_t)err_len, "seek failed"); return NULL; }
    long bytes = ftell(file);
    fclose(file);
    if (bytes <= 0 || bytes % (long)sizeof(float) != 0) {
        snprintf(err, (size_t)err_len, "weights size %ld is not float32-aligned", bytes);
        return NULL;
    }
    long file_floats = bytes / (long)sizeof(float);
    long expect = (long)obs * hidden + (long)(actions + 1) * hidden
                  + (long)layers * 3 * hidden * hidden;
    if (file_floats != expect) {
        snprintf(err, (size_t)err_len,
                 "weights hold %ld float32 but layout obs=%d hidden=%d layers=%d "
                 "actions=%d needs %ld", file_floats, obs, hidden, layers, actions, expect);
        return NULL;
    }
    BtPolicy* policy = (BtPolicy*)calloc(1, sizeof(BtPolicy));
    if (!policy) { snprintf(err, (size_t)err_len, "out of memory"); return NULL; }
    policy->weights = load_weights(path);
    policy->encoder = make_linear(policy->weights, 1, obs, hidden);
    policy->decoder = make_linear(policy->weights, 1, hidden, actions + 1);
    policy->gru = make_mingru(policy->weights, 1, hidden, layers);
    policy->actions = actions;
    if ((long)policy->weights->idx != file_floats) {
        snprintf(err, (size_t)err_len,
                 "weights consumed %zu of %ld float32: layout mismatch",
                 policy->weights->idx, file_floats);
        return NULL;
    }
    return policy;
}

/* One forward pass; returns the greedy action and writes the value.  When
 * `mask` is non-NULL (one byte per action, 1 = legal) masked-off logits are
 * replaced by `masked_logit` before the argmax, exactly like the game's policy
 * (brotato_policy.gd MASKED_LOGIT = -1e4, src/algo.cu load_logit_masked) and
 * like the trainer.  A NULL mask means every action is legal. */
int bt_policy_act(BtPolicy* policy, const float* obs, const unsigned char* mask,
                  float masked_logit, float* value) {
    linear(policy->encoder, (float*)obs);
    mingru(policy->gru, policy->encoder->output);
    linear(policy->decoder, policy->gru->output);
    float* out = policy->decoder->output;
    int best = 0;
    float best_logit = 0.0f;
    for (int a = 0; a < policy->actions; a++) {
        float logit = (mask && !mask[a]) ? masked_logit : out[a];
        if (a == 0 || logit > best_logit) { best = a; best_logit = logit; }
    }
    if (value) *value = out[policy->actions];
    /* Report the effective (masked) logits so a caller can compare arithmetic. */
    for (int a = 0; a < policy->actions; a++)
        if (mask && !mask[a]) out[a] = masked_logit;
    return best;
}

/* The raw decoder row (logits..., value) after a forward, for cross-checks. */
const float* bt_policy_logits(BtPolicy* policy) {
    return policy->decoder->output;
}

/* The dedicated action-sampling PRNG: a 32-bit xorshift, the SAME algorithm
 * rl_policy.gd and policy_reference.py run.  It is this driver's own stream,
 * seeded from the run seed plus a constant, and never touches the core's RNG,
 * so sampling an action cannot shift a gameplay draw.  One 32-bit draw per
 * decision, so the two sides stay on the same sequence when they make the same
 * number of decisions. */
unsigned int bt_sampler_next(unsigned int* state) {
    unsigned int x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

void bt_sampler_seed(unsigned int* state, unsigned long long seed) {
    *state = (unsigned int)((seed + 0x9E3779B9ull) & 0xFFFFFFFFull);
    if (*state == 0u) *state = 1u;
    bt_sampler_next(state);   /* two warm-up draws, like the GDScript side */
    bt_sampler_next(state);
}

/* Sampled action: the softmax over the effective (masked) logits, ONE uniform
 * from the dedicated stream, cumulative-probability selection.  Masked-off ids
 * carry `masked_logit` (-1e4), whose exp() underflows to exactly 0.0, so an
 * illegal action can never be drawn.  The forward is identical to
 * bt_policy_act's, so a sampled run remains comparable with a greedy one. */
int bt_policy_sample(BtPolicy* policy, const float* obs, const unsigned char* mask,
                     float masked_logit, unsigned int* rng_state, float* value) {
    double probs[BT_SAMPLE_MAX_ACTIONS];
    linear(policy->encoder, (float*)obs);
    mingru(policy->gru, policy->encoder->output);
    linear(policy->decoder, policy->gru->output);
    float* out = policy->decoder->output;
    int n = policy->actions;
    if (n <= 0 || n > BT_SAMPLE_MAX_ACTIONS) return 0;
    double best = (mask && !mask[0]) ? (double)masked_logit : (double)out[0];
    double total = 0.0;
    for (int a = 0; a < n; a++) {
        double logit = (mask && !mask[a]) ? (double)masked_logit : (double)out[a];
        if (logit > best) best = logit;
    }
    for (int a = 0; a < n; a++) {
        double logit = (mask && !mask[a]) ? (double)masked_logit : (double)out[a];
        double p = exp(logit - best);
        probs[a] = p;
        total += p;
    }
    double u = ((double)bt_sampler_next(rng_state) / 4294967296.0) * total;
    double acc = 0.0;
    int chosen = n - 1;
    for (int a = 0; a < n; a++) {
        acc += probs[a];
        if (u < acc) { chosen = a; break; }
    }
    if (value) *value = out[n];
    /* Report the effective (masked) logits so a caller can compare arithmetic. */
    for (int a = 0; a < n; a++)
        if (mask && !mask[a]) out[a] = masked_logit;
    return chosen;
}

/* Reset the recurrent state (fresh episode). */
void bt_policy_reset(BtPolicy* policy) {
    float zero = 0.0f;
    mingru_zero_term(policy->gru, &zero);
}

void bt_policy_free(BtPolicy* policy) {
    if (policy) free(policy);
}
"""

SIM_DRIVER_SRC = r"""
// eval_sim_step.cpp - closed-loop driver for the shared Brotato CPU core.
//
// Modes:
//   script  (no --weights): prints a row, reads one action per line on stdin.
//   policy  (--weights):    computes each action in-process with the native
//                           PufferNet forward (eval_sim_policy.c), so the
//                           per-step pipe traffic disappears.  --action-mode
//                           picks the action from the masked logits: "sample"
//                           (default, the softmax + dedicated-PRNG draw the
//                           game's rl_policy.gd runs) or "argmax" (greedy).
//
// Rows: one JSON object per printed step, then one final row when the core
// reports terminal:
//   {"t":tick,"a":action,"ph":phase,"w":wave,"hp":..,"mat":..,"lvl":..,
//    "kills":..,"wc":..,"steps":..,"r":reward,"obs":[...]?}
//   {"end":1,"we":endless-aware wave reached,"ew":max wave entered,
//    "won":..,"kills":..,"ticks":..,"ret":..,"w":..,"hp":..,"mat":..,"lvl":..,
//    "buys":..,"rerolls":..,"skips":..,"recycles":..,"combines":..,"level_ups":..,
//    "steps":..,"max_shop_run":..,"max_lvl_run":..,"r":..}
//   {"end":1,"policy_abort":1,"tick":..,"phase":..,"action":..,"steps":..}
//
// Achieved metrics come from the core's numeric episode log
// (brotato_core.h bt_core_add_log, which bt_core_reset never touches):
// log.perf is the wave REACHED, log.endless_wave the highest wave entered,
// log.score the kill count, log.episode_length the tick count.  `w/hp/mat/lvl`
// on the final row are the pre-step snapshot (exact for a wave-20 clear, which
// does not end-of-wave process).
#include "ocean/brotato/brotato_core.h"
#include "ocean/brotato/policy/demon_guide_bot.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

extern "C" {
typedef struct BtPolicy BtPolicy;
BtPolicy* bt_policy_load(const char*, int, int, int, int, char*, int);
int bt_policy_act(BtPolicy*, const float*, const unsigned char*, float, float*);
int bt_policy_sample(BtPolicy*, const float*, const unsigned char*, float,
                     unsigned int*, float*);
unsigned int bt_sampler_next(unsigned int*);
void bt_sampler_seed(unsigned int*, unsigned long long);
const float* bt_policy_logits(BtPolicy*);
void bt_policy_reset(BtPolicy*);
void bt_policy_free(BtPolicy*);
}

static BtState g_state;
static int g_print_every = 1;
static int g_obs_every = 0;
/* Action selection from the policy's masked logits: 1 = sample (default) with
 * bt_policy_sample and the dedicated PRNG below, 0 = the historical argmax.
 * --action-mode overrides it; the --forward-obs cross-check path is always
 * argmax (it compares against policy_reference's own argmax). */
static int g_action_mode = 1;
static unsigned int g_rng_state = 1;
static unsigned long long g_phase_mask[3] = {0, 0, 0};   /* per-phase allowed mask */
/* Artifact mask metadata, consumed exactly like rl_policy.gd: the clause is
 * chosen per OBSERVATION (phase from the artifact's phase_field_index, then the
 * first selector whose phase matches and whose when_nonzero field is non-zero
 * replaces the clause), and an absent/empty clause means "no mask". */
static char g_mask_clauses[1024] = {0};
static char g_mask_selectors[512] = {0};
static int g_mask_phase_field = -1;
static int g_have_mask = 0;
static float g_masked_logit = -1e4f;

/* Per-id conditionals (artifact action_mask.conditionals), the same shape and
 * semantics rl_policy.gd applies: an entry is scoped to a clause key and its ids
 * are legal iff EVERY test passes.  Serialized by artifact_mask_spec as
 *   clause|id_base|count|test+test...
 * with each test "op:a:b:c:d":
 *   n  nonzero            obs[a + b*k] != 0
 *   e  equals             obs[a + b*k] == c
 *   d  block_duplicate    another slot of the block carries obs[a + b*k] (!= 0)
 *   k  block_count_gte    at least d slots of the block are nonzero
 *   g  ge                 obs[a] >= obs[b] * c
 * (a = field_base, b = field_stride; unused numbers are 0.) */
#define MASK_MAX_COND 16
#define MASK_MAX_TEST 4
typedef struct {
    char clause[32];
    int id_base, count, ntest;
    char op[MASK_MAX_TEST];
    double a[MASK_MAX_TEST], b[MASK_MAX_TEST], c[MASK_MAX_TEST], d[MASK_MAX_TEST];
} MaskCond;
static MaskCond g_mask_conds[MASK_MAX_COND];
static int g_mask_nconds = 0;

static int parse_mask_conditionals(const char* spec) {
    g_mask_nconds = 0;
    if (!spec || !*spec) return 1;
    const char* cur = spec;
    while (*cur) {
        if (g_mask_nconds >= MASK_MAX_COND) return 0;
        MaskCond c;
        std::memset(&c, 0, sizeof(c));
        const char* bar = std::strchr(cur, '|');
        if (!bar) return 0;
        int n = (int)(bar - cur);
        if (n >= (int)sizeof(c.clause)) return 0;
        std::memcpy(c.clause, cur, (size_t)n);
        c.clause[n] = 0;
        const char* p = bar + 1;
        c.id_base = std::atoi(p);
        p = std::strchr(p, '|'); if (!p) return 0; p++;
        c.count = std::atoi(p);
        p = std::strchr(p, '|'); if (!p) return 0; p++;
        /* The entry's tests end at its ';' (or the string end); every search
         * below must stay inside that span.  '+' joins the tests of THIS entry
         * only: an unbounded strchr('+') jumped into the next entry, appended
         * that entry's numbers as bogus tests and dropped every later entry, so
         * the whole spec collapsed to its first conditional (the sim then ran
         * with a loose mask while the core enforced bt_action_legal). */
        const char* stop = std::strchr(p, ';');
        while (*p && *p != ';') {
            if (c.ntest >= MASK_MAX_TEST) return 0;
            double x = 0, y = 0, z = 0, w = 0;
            char op = *p;
            if (std::sscanf(p + 1, ":%lf:%lf:%lf:%lf", &x, &y, &z, &w) != 4) return 0;
            c.op[c.ntest] = op;
            c.a[c.ntest] = x; c.b[c.ntest] = y; c.c[c.ntest] = z; c.d[c.ntest] = w;
            c.ntest++;
            const char* plus = std::strchr(p, '+');
            if (plus && stop && plus > stop) plus = 0;
            p = plus ? plus + 1 : (stop ? stop : p + std::strlen(p));
        }
        if (*p == ';') p++;
        g_mask_conds[g_mask_nconds++] = c;
        cur = p;
    }
    return 1;
}

/* One conditional test for the k-th id of its block (0/1). */
static int mask_cond_test(const MaskCond* c, int t, const float* obs, int k) {
    const double fb = c->a[t], stride = c->b[t];
    const double arg_c = c->c[t], arg_d = c->d[t];
    char op = c->op[t];
    if (op == 'g') {
        int i = (int)fb, j = (int)stride;
        if (i < 0 || i >= BT_OBS_SIZE || j < 0 || j >= BT_OBS_SIZE) return 0;
        return obs[i] >= obs[j] * (float)arg_c;
    }
    if (op == 'k') {
        int cnt = (int)arg_c, need = (int)arg_d, have = 0;
        for (int j = 0; j < cnt; j++) {
            int idx = (int)fb + j * (int)stride;
            if (idx >= 0 && idx < BT_OBS_SIZE && obs[idx] != 0.0f) have++;
        }
        return have >= need;
    }
    int idx = (int)fb + k * (int)stride;
    if (idx < 0 || idx >= BT_OBS_SIZE) return 0;
    if (op == 'e') return obs[idx] == (float)arg_c;
    if (op == 'd') {
        float mine = obs[idx];
        if (mine == 0.0f) return 0;
        int cnt = (int)arg_c;
        for (int j = 0; j < cnt; j++) {
            if (j == k) continue;
            int other = (int)fb + j * (int)stride;
            if (other >= 0 && other < BT_OBS_SIZE && obs[other] == mine) return 1;
        }
        return 0;
    }
    return obs[idx] != 0.0f;        /* 'n' */
}

static const char* phase_name(int phase) {
    switch (phase) {
        case 0: return "combat";
        case 1: return "shop";
        case 2: return "level_up";
        default: return "phase";
    }
}

/* Fill `out` (one byte per action) with the mask the artifact declares for this
 * observation.  Returns 1 when a clause was applied, 0 when every action is legal
 * (no metadata, or a clause naming no ids). */
static int artifact_mask_for(const float* obs, unsigned char* out, int actions) {
    for (int a = 0; a < actions; a++) out[a] = 1;
    if (!g_have_mask || g_mask_phase_field < 0 || g_mask_phase_field >= BT_OBS_SIZE)
        return 0;
    char key[64];
    const int phase = (int)bt_roundf(obs[g_mask_phase_field]);
    std::snprintf(key, sizeof(key), "%d_%s", phase, phase_name(phase));
    char sel[512];
    std::snprintf(sel, sizeof(sel), "%s", g_mask_selectors);
    for (char* entry = std::strtok(sel, ";"); entry; entry = std::strtok(0, ";")) {
        int sel_phase = -1, sel_index = -1;
        char sel_use[64] = {0};
        if (std::sscanf(entry, "%d:%d:%63s", &sel_phase, &sel_index, sel_use) != 3)
            continue;
        if (sel_phase != phase || sel_index < 0 || sel_index >= BT_OBS_SIZE)
            continue;
        if (obs[sel_index] != 0.0f) { std::snprintf(key, sizeof(key), "%s", sel_use); break; }
    }
    char clauses[1024];
    std::snprintf(clauses, sizeof(clauses), "%s", g_mask_clauses);
    int applied = 0;
    for (char* entry = std::strtok(clauses, ";"); entry; entry = std::strtok(0, ";")) {
        char* colon = std::strchr(entry, ':');
        if (!colon) continue;
        *colon = 0;
        if (std::strcmp(entry, key) != 0) continue;
        for (int a = 0; a < actions; a++) out[a] = 0;
        char* sub = colon + 1;
        for (char* span = std::strtok(sub, ","); span; span = std::strtok(0, ",")) {
            int lo = 0, hi = 0;
            if (std::sscanf(span, "%d-%d", &lo, &hi) == 2) { }
            else if (std::sscanf(span, "%d", &lo) == 1) hi = lo;
            else continue;
            if (lo < 0) lo = 0;
            if (hi > actions - 1) hi = actions - 1;
            for (int a = lo; a <= hi; a++) out[a] = 1;
        }
        applied = 1;
        break;
    }
    if (!applied) return 0;
    /* clause-scoped, AND-combined conditionals (bt_action_legal's state gates) */
    for (int ci = 0; ci < g_mask_nconds; ci++) {
        const MaskCond* c = &g_mask_conds[ci];
        if (c->clause[0] != 0 && std::strcmp(c->clause, key) != 0) continue;
        for (int k = 0; k < c->count; k++) {
            int target = c->id_base + k;
            if (target < 0 || target >= actions) continue;
            int ok = 1;
            for (int t = 0; t < c->ntest && ok; t++)
                if (!mask_cond_test(c, t, obs, k)) ok = 0;
            out[target] = (unsigned char)(ok ? 1 : 0);
        }
    }
    return 1;
}

static unsigned long long g_applied_mask = 0;
static unsigned long long g_core_mask = 0;

/* Core mask + applied (artifact) mask for the current phase, as bitmasks.  Called
 * before the first row too, so no row carries a placeholder zero mask. */
static unsigned char g_core_bytes[BT_ACTIONS];
static unsigned char g_applied_bytes[BT_ACTIONS];

static int g_mask_phase_now = 0;   /* phase the masks below belong to */

static void update_masks(const float* obs) {
    g_mask_phase_now = g_state.phase;
    bt_action_mask(&g_state, g_core_bytes);
    g_core_mask = 0;
    for (int a = 0; a < BT_ACTIONS && a < 64; a++)
        if (g_core_bytes[a]) g_core_mask |= 1ull << a;
    artifact_mask_for(obs, g_applied_bytes, BT_ACTIONS);
    g_applied_mask = 0;
    for (int a = 0; a < BT_ACTIONS && a < 64; a++)
        if (g_applied_bytes[a]) g_applied_mask |= 1ull << a;
}

static void emit_row(const float* obs, int tick, int action, float reward) {
    std::printf("{\"t\":%d,\"a\":%d,\"ph\":%d,\"w\":%d,\"hp\":%.9g,"
                "\"mhp\":%.9g,\"mat\":%.9g,\"lvl\":%d,\"kills\":%d,"
                "\"wc\":%d,\"x\":%.9g,\"y\":%.9g,\"r\":%.9g,"
                "\"m\":%llu,\"am\":%llu,\"pm\":%d",
                tick, action, g_state.phase, g_state.wave, g_state.hp, g_state.max_hp,
                g_state.materials, g_state.level, g_state.kills, g_state.waves_cleared,
                g_state.px, g_state.py, reward, g_core_mask, g_applied_mask,
                g_mask_phase_now);
    if (obs && g_obs_every > 0 && (tick % g_obs_every) == 0) {
        std::printf(",\"obs\":[");
        for (int i = 0; i < BT_OBS_SIZE; i++) std::printf("%s%.9g", i ? "," : "", obs[i]);
        std::printf("]");
    }
    std::puts("}");
    std::fflush(stdout);
}

/* "1:0-4,8-28;2:0-4,29,30" -> per-phase bitmasks. */
static int parse_phase_spec(const char* spec, unsigned long long* out) {
    const char* cursor = spec;
    while (*cursor) {
        int phase = atoi(cursor);
        const char* colon = strchr(cursor, ':');
        if (!colon || phase < 0 || phase > 2) return 0;
        const char* end = strchr(colon, ';');
        const char* sub = colon + 1;
        while (*sub && (!end || sub < end)) {
            int lo = atoi(sub);
            const char* dash = strchr(sub, '-');
            int hi = lo;
            if (dash && dash < (end ? end : sub + strlen(sub))) {
                const char* after = dash + 1;
                if (*after >= '0' && *after <= '9') hi = atoi(after);
            }
            if (lo < 0 || hi < lo || hi >= 64) return 0;
            for (int a = lo; a <= hi; a++) out[phase] |= 1ull << a;
            const char* comma = strchr(sub, ',');
            if (!comma || (end && comma > end)) break;
            sub = comma + 1;
        }
        cursor = end ? end + 1 : cursor + strlen(cursor);
    }
    return 1;
}

int main(int argc, char** argv) {
    unsigned long long seed = 1;
    int zone = 0, character = 0, weapon = 0xFFFF, shop_steps = 1, endless = 0;
    long max_steps = 0, bench = 0;
    int audio_rng = 0;      /* 0 = the bridge's audiovisual neutralization */
    int particles_rng = 0;  /* 0 = the bridge suppresses particle RNG draws */
    int start_wave = 0;     /* 0 = the core's campaign default (wave 1) */
    int invulnerable = 0;   /* 0 = the normal player hurtbox; 1 = DebugService.invulnerable */
    float bonus_hp = 0.0f;
    const char* weights = 0;
    const char* forward_obs = 0;
    const char* forward_mask_path = 0;
    int hidden = 0, layers = 0, actions = 0;
    int policy_ok = 0, use_bot = 0;
    int bot_pivot = 0, bot_stay = 0;
    char err[256] = {0};
    for (int i = 1; i < argc; i++) {
        const char* k = argv[i];
        const char* v = (i + 1 < argc) ? argv[i + 1] : "";
        if (!std::strcmp(k, "--seed")) { seed = std::strtoull(v, 0, 10); i++; }
        else if (!std::strcmp(k, "--zone")) { zone = std::atoi(v); i++; }
        else if (!std::strcmp(k, "--char")) { character = std::atoi(v); i++; }
        else if (!std::strcmp(k, "--weapon")) { weapon = std::atoi(v); i++; }
        else if (!std::strcmp(k, "--shop-steps")) { shop_steps = std::atoi(v); i++; }
        else if (!std::strcmp(k, "--endless")) { endless = std::atoi(v); i++; }
        else if (!std::strcmp(k, "--print-every")) { g_print_every = std::atoi(v); i++; }
        else if (!std::strcmp(k, "--obs-every")) { g_obs_every = std::atoi(v); i++; }
        else if (!std::strcmp(k, "--weights")) { weights = v; i++; }
        else if (!std::strcmp(k, "--hidden")) { hidden = std::atoi(v); i++; }
        else if (!std::strcmp(k, "--layers")) { layers = std::atoi(v); i++; }
        else if (!std::strcmp(k, "--actions")) { actions = std::atoi(v); i++; }
        else if (!std::strcmp(k, "--gate")) { if (!parse_phase_spec(v, g_phase_mask)) { std::fprintf(stderr, "bad --gate %s\n", v); return 2; } i++; }
        else if (!std::strcmp(k, "--mask-clauses")) { std::snprintf(g_mask_clauses, sizeof(g_mask_clauses), "%s", v); g_have_mask = 1; i++; }
        else if (!std::strcmp(k, "--mask-selectors")) { std::snprintf(g_mask_selectors, sizeof(g_mask_selectors), "%s", v); i++; }
        else if (!std::strcmp(k, "--mask-phase-field")) { g_mask_phase_field = std::atoi(v); i++; }
        else if (!std::strcmp(k, "--mask-conditionals")) { if (!parse_mask_conditionals(v)) { std::fprintf(stderr, "bad --mask-conditionals\n"); return 2; } i++; }
        else if (!std::strcmp(k, "--masked-logit")) { g_masked_logit = (float)std::atof(v); i++; }
        else if (!std::strcmp(k, "--forward-mask")) { forward_mask_path = v; i++; }
        else if (!std::strcmp(k, "--bot")) { use_bot = std::atoi(v); i++; }
        else if (!std::strcmp(k, "--bot-pivot")) { bot_pivot = std::atoi(v); i++; }
        else if (!std::strcmp(k, "--bot-stay")) { bot_stay = std::atoi(v); i++; }
        else if (!std::strcmp(k, "--max-steps")) { max_steps = std::atol(v); i++; }
        else if (!std::strcmp(k, "--bonus-hp")) { bonus_hp = (float)std::atof(v); i++; }
        else if (!std::strcmp(k, "--audio-rng")) { audio_rng = std::atoi(v); i++; }
        else if (!std::strcmp(k, "--start-wave")) { start_wave = std::atoi(v); i++; }
        else if (!std::strcmp(k, "--particles-rng")) { particles_rng = std::atoi(v); i++; }
        else if (!std::strcmp(k, "--invulnerable")) { invulnerable = std::atoi(v); i++; }
        else if (!std::strcmp(k, "--action-mode")) {
            if (!std::strcmp(v, "sample")) g_action_mode = 1;
            else if (!std::strcmp(v, "argmax")) g_action_mode = 0;
            else { std::fprintf(stderr, "bad --action-mode %s (argmax|sample)\n", v); return 2; }
            i++;
        }
        else if (!std::strcmp(k, "--bench")) { bench = std::atol(v); i++; }
        else if (!std::strcmp(k, "--forward-obs")) { forward_obs = v; i++; }
        else if (!std::strcmp(k, "--forward-mask")) { forward_mask_path = v; i++; }
        else { std::fprintf(stderr, "unknown argument %s\n", k); return 2; }
    }
    if (g_print_every < 1) g_print_every = 1;
    /* The sampler's own stream: seeded from the run seed + a constant, exactly
     * like the game's rl_policy.gd and policy_reference.py.  Independent of the
     * core's RNG, so sampling never shifts a gameplay draw. */
    bt_sampler_seed(&g_rng_state, (unsigned long long)seed);

    bt_core_init(&g_state, (bt_u64)seed, zone, shop_steps,
                 (unsigned short)character, (unsigned short)weapon,
                 (unsigned char)audio_rng);
    /* Run options set AFTER init (brotato.h puf_init): bt_core_init -> reset
     * clears them, and reset only runs when an episode ends. */
    if (start_wave > 0) g_state.start_wave = (unsigned short)start_wave;
    /* Particle-RNG model, set AFTER init exactly like puf_init (brotato.h):
     * 0 = the bridge's suppressed particles (the CPU bridge always suppresses),
     * 1 = the legacy draws.  Matched game-vs-sim work uses 0. */
    g_state.particles_rng = (unsigned char)(particles_rng ? 1 : 0);
    /* DebugService.invulnerable, set AFTER init exactly like puf_init: the
     * sim's player hurtbox is disabled, so no enemy contact/projectile hit is
     * registered (player.gd:114-115 disable_hurtbox).  Matched game-vs-sim
     * work uses the same value on both sides. */
    g_state.invulnerable = (unsigned char)(invulnerable ? 1 : 0);
    /* Endless is a run option set AFTER init (brotato.h puf_init); the state
     * keeps it until the episode terminates (bt_core_reset clears it, and that
     * only runs when done -> this one-episode process ends there). */
    if (endless) g_state.endless = 1;
    /* Deep-start boot: bt_core_init's own reset already ran at the campaign
     * wave 1, so a `start_wave` set above would otherwise only take effect at
     * the NEXT reset -- and on an invulnerable run there is no terminal, hence
     * no next reset, so the run would sit at wave 1 forever while the game
     * starts at N.  Re-run the deterministic reset on FRESH streams (clearing
     * rng_ready makes it replay the run prologue instead of appending a second
     * one), exactly like tests/brotato_core_trace.cpp, so the FIRST episode
     * enters start_wave directly.  A campaign run (start_wave 0 or 1) keeps the
     * historical call shape and a byte-identical state. */
    if (start_wave > 1) {
        g_state.rng_ready = 0;
        bt_core_reset(&g_state);
    }
    if (bonus_hp > 0.0f) {
        /* dgb stress mode (tests/demon_guide_bot_run.cpp): extra starting Max HP so
         * the same policy reaches the deep wave schedule.  Demon's Max HP is its
         * currency, so the shop then also trades freely. */
        bt_core_stat_add(&g_state, 0, bonus_hp);
        bt_core_stat_sync(&g_state);
        g_state.hp = g_state.max_hp;
    }

    BtPolicy* policy = 0;
    DGuideBot bot;
    if (use_bot) dgb_init(&bot, bot_pivot, bot_stay);
    if (weights) {
        if (!hidden || !layers || !actions) {
            std::fprintf(stderr, "--weights needs --hidden/--layers/--actions\n");
            return 2;
        }
        policy = bt_policy_load(weights, (int)BT_OBS_SIZE, hidden, layers, actions,
                                err, (int)sizeof(err));
        if (!policy) {
            std::printf("{\"hello\":0,\"error\":\"%s\"}\n", err);
            std::fflush(stdout);
            return 3;
        }
        bt_policy_reset(policy);
        policy_ok = 1;
    }

    /* Core's own per-phase action mask (bt_action_mask, authoritative), read by
     * poking the phase on the freshly initialised state and restoring it: the
     * harness compares it with the bridge's policy gate. */
    unsigned long long mask_phase[3] = {0, 0, 0};
    {
        unsigned char mask[BT_ACTIONS];
        const int saved = g_state.phase;
        for (int ph = 0; ph <= 2; ph++) {
            g_state.phase = ph;
            bt_action_mask(&g_state, mask);
            for (int a = 0; a < BT_ACTIONS && a < 64; a++)
                if (mask[a]) mask_phase[ph] |= 1ull << a;
        }
        g_state.phase = saved;
    }
    std::printf("{\"hello\":1,\"obs\":%d,\"actions\":%d,\"waves\":%d,"
                "\"endless\":%d,\"forward\":\"%s\",\"audio_rng\":%d,"
                "\"particles_rng\":%d,\"start_wave\":%d,\"invulnerable\":%d,"
                "\"action_mode\":\"%s\","
                "\"mask0\":%llu,\"mask1\":%llu,\"mask2\":%llu}\n",
                (int)BT_OBS_SIZE, (int)BT_ACTIONS, (int)BT_WAVES, endless,
                policy_ok ? "native" : (use_bot ? "bot" : "extern"),
                audio_rng, particles_rng, (int)g_state.start_wave, invulnerable,
                g_action_mode ? "sample" : "argmax",
                mask_phase[0], mask_phase[1], mask_phase[2]);
    std::fflush(stdout);

    float obs[BT_OBS_SIZE], reward = 0.0f, terminal = 0.0f, value = 0.0f;
    if (forward_obs) {
        /* Feed a *recorded* observation sequence through the same recurrent
         * forward, one row at a time, and print the raw decoder row: this lets
         * the harness compare the native forward against policy_reference.py on
         * identical inputs (a closed-loop comparison desyncs after the first
         * action flip and says nothing).  Always the greedy action: the
         * comparison is arithmetic, not the run's selected action. */
        if (!policy_ok) { std::fprintf(stderr, "--forward-obs needs --weights\n"); return 2; }
        FILE* file = std::fopen(forward_obs, "rb");
        if (!file) { std::fprintf(stderr, "cannot open %s\n", forward_obs); return 3; }
        bt_policy_reset(policy);
        FILE* mask_file = forward_mask_path ? std::fopen(forward_mask_path, "rb") : 0;
        unsigned char fmask[BT_ACTIONS];
        int index = 0;
        while (std::fread(obs, sizeof(float), BT_OBS_SIZE, file) == BT_OBS_SIZE) {
            unsigned char* mask_ptr = 0;
            if (mask_file && std::fread(fmask, 1, (size_t)actions, mask_file) == (size_t)actions)
                mask_ptr = fmask;
            else if (!mask_file && g_have_mask) {
                artifact_mask_for(obs, fmask, actions);
                mask_ptr = fmask;
            }
            int a = bt_policy_act(policy, obs, mask_ptr, g_masked_logit, &value);
            const float* out = bt_policy_logits(policy);
            std::printf("{\"i\":%d,\"a\":%d,\"v\":%.9g,\"logits\":[", index, a, value);
            for (int k = 0; k < actions; k++)
                std::printf("%s%.9g", k ? "," : "", out[k]);
            std::puts("]}");
            index++;
        }
        std::fclose(file);
        if (mask_file) std::fclose(mask_file);
        std::fflush(stdout);
        return 0;
    }
    if (bench > 0) {
        /* Inner-loop benchmark: forward + step, resetting on terminal exactly as a
         * rollout does, so the number is steps/s of the eval loop itself. */
        clock_t t0 = clock();
        double w0 = (double)time(0);
        for (long i = 0; i < bench; i++) {
            bt_core_observe(&g_state, obs);
            int a = 0;
            if (use_bot) {
                unsigned char amask[BT_ACTIONS];
                bt_action_mask(&g_state, amask);
                a = dgb_action(&g_state, &bot, amask);
                if (a < 0 || a >= BT_ACTIONS) a = 0;
            } else if (policy_ok) {
                unsigned char bmask[BT_ACTIONS];
                artifact_mask_for(obs, bmask, BT_ACTIONS);
                a = g_action_mode
                    ? bt_policy_sample(policy, obs, bmask, g_masked_logit, &g_rng_state, &value)
                    : bt_policy_act(policy, obs, bmask, g_masked_logit, &value);
            }
            bt_core_step(&g_state, a, obs, &reward, &terminal);
            if (terminal != 0.0f) {
                bt_core_reset(&g_state);
                if (bonus_hp > 0.0f) {
                    bt_core_stat_add(&g_state, 0, bonus_hp);
                    bt_core_stat_sync(&g_state);
                    g_state.hp = g_state.max_hp;
                }
            }
        }
        double secs = (double)(clock() - t0) / (double)CLOCKS_PER_SEC;
        if (secs <= 0.0) secs = 1e-6;
        std::printf("{\"bench\":%ld,\"cpu_seconds\":%.4f,\"steps_per_second\":%.0f,"
                    "\"forward\":\"%s\"}\n", bench, secs, (double)bench / secs,
                    policy_ok ? "native" : (use_bot ? "bot" : "extern"));
        std::fflush(stdout);
        return 0;
    }
    bt_core_observe(&g_state, obs);
    update_masks(obs);
    emit_row(obs, 0, 0, 0.0f);

    long steps = 0, shops = 0, rerolls = 0, skips = 0, recycles = 0, combines = 0;
    long level_ups = 0, buys = 0, shop_run = 0, max_shop_run = 0, lvl_run = 0, max_lvl_run = 0;
    int action = 0, stream_end = 0;
    for (;;) {
        if ((max_steps > 0 && steps >= max_steps) || stream_end) {
            /* Budget exhausted (the game's BROTATO_RL_MAX_TICKS counts every
             * physics step, phase steps included) or an extern action stream
             * ended.  The episode log is empty because the episode never
             * finished, so report the live state. */
            std::printf("{\"end\":1,\"trunc\":1,\"we\":%d,\"ew\":%d,\"won\":0,"
                        "\"kills\":%d,\"ticks\":%d,\"w\":%d,\"hp\":%.9g,"
                        "\"mat\":%.9g,\"lvl\":%d,\"buys\":%ld,\"rerolls\":%ld,"
                        "\"skips\":%ld,\"recycles\":%ld,\"combines\":%ld,"
                        "\"level_ups\":%ld,\"steps\":%ld,\"max_shop_run\":%ld,"
                        "\"max_lvl_run\":%ld}\n",
                        g_state.wave, g_state.endless_max_wave, g_state.kills,
                        g_state.ticks, g_state.wave, g_state.hp, g_state.materials,
                        g_state.level, buys, rerolls, skips, recycles, combines,
                        level_ups, steps, max_shop_run, max_lvl_run);
            std::fflush(stdout);
            return 0;
        }
        /* The core's own per-phase legality mask (one byte per action) and the
         * mask actually applied.  The game masks with the *artifact's* rules
         * (rl_policy.gd legal_by_phase -> brotato_policy.gd MASKED_LOGIT), so the
         * native forward consumes exactly those when the harness passed --mask;
         * the scripted bot consumes the core's own mask like its own driver.
         * Both are recorded per row, so a difference is visible, not assumed. */
        update_masks(obs);
        if (use_bot) {
            action = dgb_action(&g_state, &bot, g_core_bytes);
            if (action < 0 || action >= BT_ACTIONS) action = 0;
        } else if (policy_ok) {
            action = g_action_mode
                ? bt_policy_sample(policy, obs, g_applied_bytes, g_masked_logit,
                                   &g_rng_state, &value)
                : bt_policy_act(policy, obs, g_applied_bytes, g_masked_logit,
                                &value);
        } else if (std::scanf("%d", &action) != 1) {
            if (steps == 0) break;      /* nothing ran: no row to report */
            stream_end = 1;             /* the stream is the budget */
            continue;
        }

        if ((policy_ok || use_bot) && (g_state.phase == 1 || g_state.phase == 2)) {
            /* Mirror the bridge's fail-closed gate: an action the bridge would
             * reject ends the episode in the game, so it must end it here too. */
            if (action > 63 || !((g_phase_mask[g_state.phase] >> action) & 1ull)) {
                std::printf("{\"end\":1,\"policy_abort\":1,\"tick\":%d,"
                            "\"phase\":%d,\"action\":%d,\"steps\":%ld}\n",
                            g_state.ticks, g_state.phase, action, steps);
                std::fflush(stdout);
                return 0;
            }
        }
        const int snap_w = g_state.wave, snap_lvl = g_state.level, snap_phase = g_state.phase;
        const float snap_hp = g_state.hp, snap_mat = g_state.materials;
        const int prev_kills = g_state.kills, prev_level = g_state.level;
        bt_core_step(&g_state, action, obs, &reward, &terminal);
        steps++;
        if (snap_phase == 1) {
            shops++;
            if (action == 4) rerolls++;
            else if (action == 8) skips++;
            else if (action >= 0 && action <= 3) { if (reward > 0.0f) buys++; }
            else if (action >= 9 && action <= 14) recycles++;
            else if (action >= 15 && action <= 20) combines++;
            shop_run++; if (shop_run > max_shop_run) max_shop_run = shop_run;
            lvl_run = 0;
        } else if (snap_phase == 2) {
            if (reward > 0.0f) level_ups++;
            lvl_run++; if (lvl_run > max_lvl_run) max_lvl_run = lvl_run;
            shop_run = 0;
        } else {
            shop_run = 0; lvl_run = 0;
        }
        (void)prev_kills; (void)prev_level;
        if (terminal != 0.0f) {
            /* log.perf is the wave reached; log.endless_wave the highest wave
             * entered.  Campaign win = the terminal step happened at wave 20
             * with the player alive; in endless only death ends a run, so the
             * wave-20 clear shows up as having reached wave 21+. */
            const int wave_end = (int)(g_state.log.perf + 0.5f);
            const int wave_max = (int)(g_state.log.endless_wave + 0.5f);
            /* log.perf is the wave reached (brotato_core.h bt_core_add_log), so
             * in endless "cleared wave 20" is exactly "reached wave 21+"; a
             * campaign run ends at wave 20 with the player alive. */
            const int won = endless ? (wave_end > BT_WAVES)
                                    : (wave_end >= BT_WAVES && snap_hp > 0.0f);
            std::printf("{\"end\":1,\"term\":1,\"a\":%d,\"we\":%d,\"ew\":%d,"
                        "\"won\":%d,\"kills\":%d,\"ticks\":%d,\"ret\":%.9g,\"w\":%d,"
                        "\"hp\":%.9g,\"mat\":%.9g,\"lvl\":%d,\"buys\":%ld,"
                        "\"rerolls\":%ld,\"skips\":%ld,\"recycles\":%ld,"
                        "\"combines\":%ld,\"level_ups\":%ld,\"steps\":%ld,"
                        "\"max_shop_run\":%ld,\"max_lvl_run\":%ld,\"r\":%.9g}\n",
                        action, wave_end, wave_max, won, (int)(g_state.log.score + 0.5f),
                        (int)(g_state.log.episode_length + 0.5f),
                        g_state.log.episode_return, snap_w, snap_hp, snap_mat,
                        snap_lvl, buys, rerolls, skips, recycles, combines, level_ups,
                        steps, max_shop_run, max_lvl_run, reward);
            std::fflush(stdout);
            return 0;
        }
        if ((steps % g_print_every) == 0) {
            emit_row(obs, g_state.ticks, action, reward);
        }
    }
    if (policy) bt_policy_free(policy);
    return 0;
}
"""


def build_sim_driver(root: Path, quiet: bool, include: Path | None = None) -> Path:
    """Emit + compile the sim driver (native policy glue + driver), cached under
    ``root/build``.  The glue compiles as C because it includes ``src/puffercpu.c``
    (puffernet's C inference path); the driver compiles as C++ for the core
    header and links the glue object."""
    build_dir = root / "build"
    build_dir.mkdir(parents=True, exist_ok=True)
    include = include or REPO
    sources = {build_dir / "eval_sim_policy.c": SIM_GLUE_SRC,
               build_dir / "eval_sim_step.cpp": SIM_DRIVER_SRC}
    binary = build_dir / "eval_sim_step"
    object_file = build_dir / "eval_sim_policy.o"
    stamp_file = build_dir / "sim_driver.stamp"
    compiler_c = os.environ.get("CC", "clang")
    compiler_cxx = os.environ.get("CXX", "clang++")
    flags = ["-O2", "-ffp-contract=off"]
    commands = [
        [compiler_c, "-std=c99", *flags, "-I", str(include),
         str(build_dir / "eval_sim_policy.c"), "-c", "-o", str(object_file)],
        [compiler_cxx, "-std=c++17", *flags, "-I", str(include),
         str(build_dir / "eval_sim_step.cpp"), str(object_file),
         "-lm", "-o", str(binary)],
    ]
    # Content stamp over everything that decides the binary (both sources, the
    # commands, the include root): a binary whose *sources* are current but whose
    # *object* is from an older revision is otherwise indistinguishable from a
    # good build, and that cost a real debugging detour (a stale driver hung at
    # the first phase transition).  Never trust mtimes here.
    stamp = hashlib.sha256()
    for cmd in commands:
        stamp.update("\0".join(cmd).encode())
    for path, body in sorted(sources.items()):
        stamp.update(path.name.encode())
        stamp.update(body.encode())
    # The included core headers decide the binary too, and they are the files
    # peers are editing continuously: hash every header the driver can pull in.
    header_root = include / "ocean" / "brotato"
    header_files = sorted(header_root.glob("*.h")) if header_root.is_dir() else []
    for path in header_files:
        stamp.update(path.name.encode())
        stamp.update(path.read_bytes())
    puffer = include / "src" / "puffercpu.c"
    if puffer.is_file():
        stamp.update(puffer.read_bytes())
    stamp_hex = stamp.hexdigest()
    if (binary.is_file() and object_file.is_file() and stamp_file.is_file()
            and stamp_file.read_text().strip() == stamp_hex):
        return binary
    for path, body in sources.items():
        if not path.is_file() or path.read_text() != body:
            path.write_text(body)
    for cmd in commands:
        if not quiet:
            print("eval: compiling sim driver: %s" % " ".join(cmd))
        try:
            subprocess.run(cmd, check=True, capture_output=True, text=True)
        except FileNotFoundError as exc:
            raise EvalError("no compiler (%s): %s" % (cmd[0], exc))
        except subprocess.CalledProcessError as exc:
            raise EvalError("sim driver failed to compile:\n%s"
                            % (exc.stderr or exc.stdout))
    stamp_file.write_text(stamp_hex + "\n")
    return binary


class EvalError(Exception):
    """Unrecoverable setup problem in this tool."""


# ---------------------------------------------------------------------------
# policy_reference import (same forward pass the GDScript policy implements)
# ---------------------------------------------------------------------------
def load_reference():
    import importlib.util

    sys.path.insert(0, str(POLICY_DIR))
    spec = importlib.util.spec_from_file_location(
        "brotato_policy_reference", POLICY_DIR / "policy_reference.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class PolicyAbort(Exception):
    """The policy produced an action its phase does not allow.

    The real bridge fails closed here (mod_main.gd ``_progression_action`` ->
    ``_fail_policy``), so the sim must stop at the same place instead of running
    a longer episode the game would never have produced.
    """

    def __init__(self, phase: int, action: int):
        super().__init__("%s phase: policy action %d not valid"
                         % (PHASE_NAMES.get(phase, "phase%d" % phase), action))
        self.phase = phase
        self.action = action


PHASE_NAMES = {0: "combat", 1: "shop", 2: "level_up"}


def artifact_mask_conditionals(artifact: dict | None) -> str:
    """Serialize the artifact's `action_mask.conditionals` for the C++ driver.

    Entry = ``clause|id_base|count|test+test...``; test = ``op:a:b:c:d`` with the
    same codes mask_cond_test() decodes (n nonzero, e equals, d block_duplicate,
    k block_count_gte, g ge).  Clauses carry no '|', '+', ';' or ':', so the flat
    grammar is unambiguous."""
    rules = (artifact or {}).get("action_mask")
    conds = rules.get("conditionals") if isinstance(rules, dict) else None
    entries = []
    for c in conds or []:
        if not isinstance(c, dict):
            continue
        tests = []
        for t in c.get("tests") or []:
            if not isinstance(t, dict):
                continue
            op = str(t.get("op", ""))
            if op == "nonzero":
                tests.append("n:%d:%d:0:0" % (int(t["field_base"]), int(t["field_stride"])))
            elif op == "equals":
                tests.append("e:%d:%d:%g:0" % (int(t["field_base"]),
                                               int(t["field_stride"]),
                                               float(t["value"])))
            elif op == "block_duplicate":
                tests.append("d:%d:%d:%d:0" % (int(t["field_base"]),
                                               int(t["field_stride"]),
                                               int(t["count"])))
            elif op == "block_count_gte":
                tests.append("k:%d:%d:%d:%d" % (int(t["field_base"]),
                                                int(t["field_stride"]),
                                                int(t["count"]),
                                                int(t["value"])))
            elif op == "ge":
                tests.append("g:%d:%d:%g:0" % (int(t["field"]), int(t["other"]),
                                               float(t["scale"])))
        if tests:
            entries.append("%s|%d|%d|%s" % (str(c.get("clause", "")),
                                            int(c.get("id_base", -1)),
                                            int(c.get("count", 0)),
                                            "+".join(tests)))
    return ";".join(entries)

# Fallback when the contract cannot be parsed: the 21-action era table.
_FALLBACK_PHASE_ACTIONS = {1: set(range(0, 5)) | set(range(8, 21)),
                           2: set(range(0, 4))}

_PHASE_HEADER = re.compile(r"\((?:phase|Phases)\s*(\d)[^)]*\)")
_PHASE_TAIL = "Movement actions"


def _actions_in(text: str) -> set:
    actions = set()
    for low, high in re.findall(r"(\d+)(?:\s*[-.]{1,2}\s*(\d+))?", text):
        lo = int(low)
        hi = int(high) if high else lo
        if 0 <= lo <= hi < 512:
            actions.update(range(lo, hi + 1))
    return actions


def phase_valid_actions() -> dict:
    """Per-phase allowed actions, read from ``state_contract.json``'s action
    table (the phase-factored prose the bridge implements).

    Segments are keyed by the phase number in their header, so wording changes
    ("Level-up", "Crate", ...) cannot desynchronise this from the contract; a
    phase-2 segment therefore covers level-up offers and crate actions the same
    way the contract nests them.
    """
    contract = read_json(HERE / "state_contract.json") or {}
    table = str(((contract.get("action") or {}).get("table") or ""))
    headers = list(_PHASE_HEADER.finditer(table))
    if not headers:
        return dict(_FALLBACK_PHASE_ACTIONS)
    by_phase = {}
    for i, header in enumerate(headers):
        end = headers[i + 1].start() if i + 1 < len(headers) else len(table)
        segment = table[header.end():end]
        tail = segment.find(_PHASE_TAIL)
        if tail >= 0:
            segment = segment[:tail]
        by_phase.setdefault(int(header.group(1)), set()).update(
            _actions_in(segment))
    if 1 not in by_phase or 2 not in by_phase:
        return dict(_FALLBACK_PHASE_ACTIONS)
    return {0: by_phase.get(0, set()), 1: by_phase[1], 2: by_phase[2]}


BRIDGE_MOD_MAIN = (HERE / "mod" / "res" / "mods-unpacked" /
                    "rlbridge-headlessrl" / "mod_main.gd")

_RANGE = re.compile(r"proposed\s*>=\s*(\d+)\s+and\s+proposed\s*<=\s*(\d+)")
_UPTO = re.compile(r"proposed\s*<=\s*(\d+)")
_FROM = re.compile(r"proposed\s*>=\s*(\d+)")
_EQUALS = re.compile(r"proposed\s*==\s*(\d+)")
_CLAUSE = re.compile(
    r'(?:if|elif)\s+phase_kind\s*==\s*"(\w+)"\s*:(.*?)'
    r'(?=(?:elif|else)\s*[^\n]*:|\Z)', re.S)


def _range_set(expr: str) -> set:
    """Allowed actions from one GDScript boolean expression over ``proposed``."""
    actions = set()
    if not expr:
        return actions
    for low, high in _RANGE.findall(expr):
        actions.update(range(int(low), int(high) + 1))
    rest = _RANGE.sub(" ", expr)
    for value in _EQUALS.findall(rest):
        actions.add(int(value))
    if actions:
        return actions
    rest = _EQUALS.sub(" ", rest)
    for high in _UPTO.findall(rest):
        actions.update(range(0, int(high) + 1))
    for low in _FROM.findall(rest):
        actions.update(range(int(low), 512))
    return actions


def bridge_valid_actions() -> dict:
    """The bridge's own policy-action gate, parsed from ``mod_main.gd``.

    This is the rule that decides whether the *game* applies the policy's action
    or fails the episode closed (``_progression_action``), so the sim mirrors it
    for the abort decision.  Both spellings the bridge has used are supported (a
    ``var valid: bool = ... if phase_kind ... else ...`` ternary and the
    readable ``var valid: bool`` / ``if phase_kind == "x": valid = ...`` chain);
    anything else returns ``{}`` and the caller falls back to the contract.
    """
    try:
        text = BRIDGE_MOD_MAIN.read_text()
    except OSError:
        return {}
    marker = "var valid: bool"
    if marker not in text:
        return {}
    tail = text[text.index(marker):]

    ternary = re.match(r'var valid: bool = (.*?)\n', tail)
    if ternary:
        arms = re.match(r'\s*(.*?)\s+if\s+phase_kind\s*==\s*"(\w+)"\s+else\s+(.*)$',
                        ternary.group(1), re.S)
        if arms:
            return {"shop": _range_set(arms.group(3)),
                    arms.group(2): _range_set(arms.group(1))}

    region = tail.split("\n\tif valid:", 1)[0]
    out = {}
    for kind, body in _CLAUSE.findall(region):
        assignment = re.search(r"valid\s*=\s*([^\n]*)", body)
        if assignment:
            out[kind] = _range_set(assignment.group(1))
    if out.get("shop") and out.get("level_up"):
        return out
    return {}


_VALID_ACTIONS = None
_GATE_NOTES = []


def action_gate_notes() -> list:
    """Human-readable disagreements between the contract and the bridge gate."""
    return list(_GATE_NOTES)


def action_gate_label() -> str:
    """Where the effective per-phase action gate came from."""
    if _VALID_ACTIONS is None:
        return "unresolved"
    if _VALID_ACTIONS == bridge_valid_actions():
        return "bridge mod_main.gd"
    return "state_contract.json (bridge gate unparsable)"


def init_action_gate() -> None:
    """Resolve the per-phase action gate and record any contract/bridge drift.

    The abort decision mirrors the bridge (the game's ground truth); the
    contract's ``action.table`` is what the *core* implements.  When they differ,
    a policy that uses the contract-only actions is rejected in-game but would
    have been applied by the core, so the difference is reported.
    """
    global _VALID_ACTIONS
    contract = phase_valid_actions()
    bridge = bridge_valid_actions()
    if not bridge:
        _VALID_ACTIONS = contract
        _GATE_NOTES.append("bridge action gate unparsable; using the contract's "
                           "per-phase action sets")
        return
    for kind, phase in (("shop", 1), ("level_up", 2)):
        contract_set = contract.get(phase, set())
        contract_only = sorted(contract_set - bridge.get(kind, set()))
        bridge_only = sorted(bridge.get(kind, set()) - contract_set)
        if contract_only:
            _GATE_NOTES.append(
                "%s: contract allows %s but the bridge gate (mod_main.gd) rejects "
                "them (game episode would fail closed)"
                % (kind, contract_only[:12]))
        if bridge_only:
            _GATE_NOTES.append(
                "%s: the bridge gate allows %s but state_contract.json does not "
                "declare them" % (kind, bridge_only[:12]))
    _VALID_ACTIONS = bridge


def phase_action_valid(phase: int, action: int) -> bool:
    """Mirror the bridge's fail-closed rule (mod_main.gd ``_progression_action``):
    combat is never gated, the shop and level-up accept only their sets."""
    global _VALID_ACTIONS
    if action < 0:
        return False
    if phase == 0:
        return True
    if _VALID_ACTIONS is None:
        init_action_gate()
    key = "shop" if phase == 1 else "level_up"
    return action in _VALID_ACTIONS.get(key, _FALLBACK_PHASE_ACTIONS.get(phase, set()))


class SampledRng:
    """The dedicated action-sampling PRNG: a 32-bit xorshift.

    The SAME algorithm the game's ``rl_policy.gd`` and the native driver
    (this file's ``SIM_GLUE_SRC``, ``bt_sampler_*``) run, seeded from the run
    seed plus a constant.  It is the eval's own stream and never touches the
    core's RNG, so sampling an action cannot shift a gameplay draw.  One 32-bit
    draw per decision keeps the three implementations on the same sequence while
    they make the same number of decisions.
    """

    CONST = 0x9E3779B9

    def __init__(self, seed: int = 0):
        self.state = 1
        self.seed(seed)

    def seed(self, seed: int) -> None:
        self.state = ((int(seed) + self.CONST) & 0xFFFFFFFF) or 1
        self.next()                       # two warm-up draws, like both others
        self.next()

    def next(self) -> int:
        x = self.state
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        self.state = x
        return x

    def uniform(self) -> float:
        return self.next() / 4294967296.0


def sample_masked_logits(masked_logits, rng: SampledRng, allowed=None) -> int:
    """One sampled action: softmax over the masked logits, ONE uniform from the
    dedicated stream, cumulative-probability selection.

    Masked-off ids carry MASKED_LOGIT (-1e4), whose exp() underflows to exactly
    0.0, so they can never be drawn.  ``allowed`` optionally restricts the draw
    to a phase's legal ids (the reference forward applies no artifact mask, so
    its own contract sets stand in for one)."""
    best = None
    for a, logit in enumerate(masked_logits):
        if allowed is not None and a not in allowed:
            continue
        if best is None or logit > best:
            best = logit
    if best is None:
        return -1
    probs = []
    total = 0.0
    for a, logit in enumerate(masked_logits):
        p = math.exp(logit - best) if allowed is None or a in allowed else 0.0
        probs.append(p)
        total += p
    u = rng.uniform() * total
    acc = 0.0
    for a, p in enumerate(probs):
        acc += p
        if u < acc:
            return a
    return len(probs) - 1


class PhasePolicy:
    """Applies one exported artifact to sim observations, phase-aware.

    Combat accepts 0..20 (9..20 are movement holds), the shop accepts 0..4 and
    8..20, level-up accepts 0..3.  An action outside the phase's set aborts the
    episode exactly like the game's fail-closed policy mode.

    ``action_mode`` mirrors the game's ``rl_policy.gd``: "sample" (default)
    draws from the softmax over the reference's masked logits with one uniform
    per decision from the dedicated PRNG, "argmax" is the greedy pick.
    """

    def __init__(self, artifact: dict, reference, script, action_mode: str = "argmax"):
        self.ref = reference.Reference(artifact)
        self.script = script
        self.action_mode = "sample" if action_mode == "sample" else "argmax"
        self.rng = SampledRng(0)
        self.sampled = 0
        self.fallbacks = 0
        self.step_calls = 0

    def reset(self, seed=None) -> None:
        self.ref.reset()
        if seed is not None:
            self.rng.seed(int(seed))

    def act(self, obs, phase: int) -> int:
        self.step_calls += 1
        result = self.ref.step(obs)
        if self.action_mode == "sample":
            allowed = {a for a in range(len(result["masked_logits"]))
                       if phase_action_valid(phase, a)}
            proposed = sample_masked_logits(result["masked_logits"], self.rng, allowed)
            self.sampled += 1
        else:
            proposed = int(result["action"])
        if phase_action_valid(phase, proposed):
            return proposed
        self.fallbacks += 1
        raise PolicyAbort(phase, proposed)


class ScriptPolicy:
    """The bridge's deterministic script source (rl_cfg.gd / rl_progress.gd):

    * combat: cycle 8,2,4,6,0,3,1,5,7,0 and hold each for 30 ticks;
    * shop: reroll once, then skip (action 8);
    * level-up: choose offer 0.
    """

    COMBAT_CYCLE = [8, 2, 4, 6, 0, 3, 1, 5, 7, 0]
    HOLD_TICKS = 30

    def __init__(self):
        self.combat_ticks = 0
        self.rerolls = 0

    def reset(self) -> None:
        self.combat_ticks = 0
        self.rerolls = 0

    def act(self, obs, phase: int) -> int:
        if phase == 1:
            if self.rerolls < 1:
                self.rerolls += 1
                return 4
            return 8
        if phase == 2:
            return 0
        index = (self.combat_ticks // self.HOLD_TICKS) % len(self.COMBAT_CYCLE)
        self.combat_ticks += 1
        return self.COMBAT_CYCLE[index]


# ---------------------------------------------------------------------------
# Sim episode
# ---------------------------------------------------------------------------
def write_policy_weights(artifact: dict, root: Path) -> tuple[Path, tuple]:
    """Materialise the artifact's weights as the float32 stream puffernet's
    ``load_weights`` consumes (registration order, no padding: the arch is
    packable, which export_policy.check_trainer_packable proves at export time).

    Returns (path, (obs, hidden, layers, actions))."""
    import base64
    import hashlib
    import struct

    layout = artifact["layout"]
    dims = (int(layout["obs_size"]), int(layout["hidden"]), int(layout["layers"]),
            int(layout["actions"]))
    obs, hidden, layers, actions = dims
    order = artifact.get("weight_order")
    tensors = {t["name"]: t for t in artifact["tensors"]}
    if not order:
        order = ["encoder.weight", "decoder.weight"] + \
                ["mingru.%d.weight" % i for i in range(layers)]
    blob = bytearray()
    for name in order:
        entry = tensors.get(name)
        if entry is None:
            raise EvalError("artifact is missing tensor %r" % name)
        blob += base64.b64decode(entry["data"])
    expect = obs * hidden + (actions + 1) * hidden + layers * 3 * hidden * hidden
    if len(blob) != expect * 4:
        raise EvalError("artifact weights are %d float32, layout obs=%d hidden=%d "
                        "layers=%d actions=%d needs %d"
                        % (len(blob) // 4, obs, hidden, layers, actions, expect))
    digest = hashlib.sha256(bytes(blob)).hexdigest()[:16]
    build = root / "build"
    build.mkdir(parents=True, exist_ok=True)
    path = build / ("policy_%s.f32" % digest)
    if not path.is_file():
        path.write_bytes(bytes(blob))
    return path, dims


def native_forward_sequence(binary: Path, weights: Path, dims: tuple,
                            obs_rows: list, mask_rows: list | None = None,
                            masked_logit: float = -1e4,
                            tag: str = "0") -> list:
    """Run the native forward over a recorded obs sequence (no stepping)."""
    import struct

    build = binary.parent
    obs_file = build / ("crosscheck_obs_%s.f32" % tag)
    flat = [float(x) for row in obs_rows for x in row]
    obs_file.write_bytes(struct.pack("<%df" % len(flat), *flat))
    obs_dim, hidden, layers, actions = dims
    mask_file = None
    if mask_rows:
        mask_file = build / ("crosscheck_mask_%s.u8" % tag)
        mask_file.write_bytes(bytes(
            1 if (bits >> a) & 1 else 0
            for bits in mask_rows[:len(obs_rows)] for a in range(actions)))
    argv = [str(binary), "--seed", "1", "--zone", "0", "--char", "2",
            "--weapon", "118", "--shop-steps", "1", "--endless", "0",
            "--print-every", "1", "--obs-every", "0", "--max-steps", "1",
            "--weights", str(weights), "--hidden", str(hidden),
            "--layers", str(layers), "--actions", str(actions),
            "--forward-obs", str(obs_file), "--masked-logit", repr(float(masked_logit))]
    if mask_file is not None:
        argv += ["--forward-mask", str(mask_file)]
    proc = subprocess.run(argv, capture_output=True, text=True, timeout=1800)
    rows = [json.loads(line) for line in proc.stdout.splitlines()
            if line.startswith("{") and '"logits"' in line]
    if not rows:
        raise EvalError("native forward-obs produced nothing: %s"
                        % (proc.stderr or proc.stdout)[-200:])
    return rows


def run_sim_bench(binary: Path, args, weights, dims, bot, character: int,
                  sim_weapon: int) -> dict:
    """Measure the sim inner loop (observe -> action -> step) in one process."""
    argv = [str(binary), "--seed", "1", "--zone", "0", "--char", str(character),
            "--weapon", str(sim_weapon), "--shop-steps", "1",
            "--endless", "1" if args.endless else "0",
            "--print-every", "1000000", "--obs-every", "0",
            "--max-steps", "1000000000", "--bench", str(args.sim_bench),
            "--action-mode", args.action_mode,
            "--gate", gate_spec()]
    if bot is not None:
        argv += ["--bot", "1", "--bot-pivot", str(bot[0]), "--bot-stay", str(bot[1])]
    if weights is not None:
        obs_dim, hidden, layers, actions = dims
        argv += ["--weights", str(weights), "--hidden", str(hidden),
                 "--layers", str(layers), "--actions", str(actions)]
    if args.bonus_hp:
        argv += ["--bonus-hp", repr(float(args.bonus_hp))]
    proc = subprocess.run(argv, capture_output=True, text=True, timeout=3600)
    for line in proc.stdout.splitlines():
        if '"bench"' in line:
            return json.loads(line)
    raise EvalError("sim bench failed: %s" % (proc.stderr or proc.stdout)[-300:])


def run_sim_episode(binary: Path, seed: int, character: int, weapon: int,
                    zone: int, shop_steps: int, policy, script, max_steps: int,
                    wall_timeout: int, endless: bool = False,
                    weights: Path | None = None, dims: tuple | None = None,
                    gate_spec: str = "", print_every: int = 1,
                    obs_every: int = 0, bot: tuple | None = None,
                    bonus_hp: float = 0.0, mask_spec: str | None = None,
                    mask_conditionals: str = "",
                    masked_logit: float = -1e4, mask_rules: dict | None = None,
                    mask_selectors: str = "", mask_phase_field: int = -1,
                    audio_rng: int = 0, start_wave: int = 0,
                    particles_rng: int = 0, invulnerable: int = 0,
                    action_stream: list | None = None,
                    action_mode: str = "sample") -> dict:
    """One simulator episode.

    Two forward paths for a policy: ``weights`` set -> the driver computes each
    action in-process with the native puffernet forward (fast; obs printed only
    every ``obs_every`` steps for cross-checks); ``policy`` set and ``weights``
    None -> the harness drives step by step and applies ``policy_reference.py``
    (the reference arithmetic, one action per line on the driver's stdin).

    ``max_steps`` bounds *every* step, not just combat ticks: the core does not
    advance ``state.ticks`` while a phase is 1 (shop) or 2 (level-up), so a policy
    that keeps proposing shop actions the core accepts as no-ops would otherwise
    spin until the wall timeout.  The game's own ``BROTATO_RL_MAX_TICKS`` counts
    every physics step including shop frames, so this is the same budget."""
    argv = [str(binary),
            "--seed", str(seed), "--zone", str(zone), "--char", str(character),
            "--weapon", str(weapon), "--shop-steps", str(shop_steps),
            "--endless", "1" if endless else "0",
            "--print-every", str(max(1, print_every)), "--obs-every", str(obs_every),
            "--max-steps", str(max_steps), "--audio-rng", str(int(audio_rng)),
            "--particles-rng", str(int(particles_rng)),
            "--action-mode", action_mode]
    if invulnerable:
        argv += ["--invulnerable", str(int(invulnerable))]
    if start_wave:
        argv += ["--start-wave", str(int(start_wave))]
    if bonus_hp:
        argv += ["--bonus-hp", repr(float(bonus_hp))]
    if mask_spec:
        argv += ["--mask-clauses", mask_spec, "--masked-logit",
                 repr(float(masked_logit)), "--mask-phase-field",
                 str(int(mask_phase_field))]
    if mask_selectors:
        argv += ["--mask-selectors", mask_selectors]
    if mask_conditionals:
        argv += ["--mask-conditionals", mask_conditionals]
    if bot is not None:
        argv += ["--bot", "1", "--bot-pivot", str(bot[0]), "--bot-stay", str(bot[1]),
                 "--gate", gate_spec]
    if weights is not None:
        obs_dim, hidden, layers, actions = dims
        argv += ["--weights", str(weights), "--hidden", str(hidden),
                 "--layers", str(layers), "--actions", str(actions),
                 "--gate", gate_spec]
    driver_drives = weights is not None or bot is not None
    proc = subprocess.Popen(argv, stdin=None if driver_drives else subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, bufsize=1)
    started = time.time()
    if policy is not None:
        policy.reset(seed)
    script.reset()

    metrics = new_metrics(seed, "sim")
    metrics["endless"] = endless
    metrics["action_mode"] = action_mode
    if endless:
        metrics["flags"].append("endless")
    metrics["audio_rng"] = int(audio_rng)
    metrics["particles_rng"] = int(particles_rng)
    if int(particles_rng) != 0:
        metrics["notes"].append("particles_rng=%d (legacy draws; the CPU bridge "
                                "suppresses particles, so matched work uses 0)"
                                % int(particles_rng))
    if int(audio_rng) != 0:
        metrics["notes"].append("audio_rng=%d (legacy source audio draws; the "
                                "bridge-neutralized game is audio_rng=0)"
                                % int(audio_rng))
    if start_wave:
        metrics["flags"].append("start_wave=%d" % int(start_wave))
        metrics["notes"].append("sim started at wave %d (state.start_wave)" % start_wave)
    if bonus_hp:
        metrics["flags"].append("stress_bonus_hp")
    if bot is not None:
        metrics["flags"].append("dgb_bot")
    metrics["invulnerable"] = int(invulnerable)
    if int(invulnerable):
        metrics["flags"].append("invulnerable")
        metrics["notes"].append("invulnerable (BtState.invulnerable: the sim's player "
                                "Hurtbox is disabled, mirroring the game's "
                                "DebugService.invulnerable)")
    if bonus_hp:
        metrics["notes"].append("stress bonus_hp=%g (only needed when the sim is NOT "
                                "invulnerable)" % bonus_hp)
    shop = {"actions": 0, "buys": 0, "rerolls": 0, "skips": 0, "level_ups": 0}
    phase_ticks = {0: 0, 1: 0, 2: 0}
    stall_steps = {1: 0, 2: 0}
    max_shop_run = 0
    max_level_up_run = 0
    end_row = None
    abort_row = None
    last_phase = 0
    pending = None
    truncated = False
    steps = 0
    actions_seen = []
    obs_seen = []
    mask_seen = []
    mask_diff = 0
    # bits outside the combine block: the one legality condition the artifact
    # cannot express (bt_weapon_can_combine's upgrade path, see
    # export_policy.unobservable_conditions), so a divergence there is expected
    # while any OTHER divergence is a real mask-parity break.
    _COMBINE_BITS = sum(1 << a for a in range(15, 21))
    mask_diff_other = 0
    mask_meta_diff = 0
    # Driver-applied (artifact) vs core bt_action_legal: the direction that
    # matters (artifact admits what the core forbids) is a hard failure.
    mask_admits_illegal = 0
    mask_admits_illegal_other = 0
    mask_narrower = 0
    # The masks in a row were chosen from the PREVIOUS step's observation (the
    # driver observes, masks, acts, then prints the post-step row), so the
    # harness re-derives them from that same observation.
    mask_obs_prev = None
    mask_phase_steps = {}
    actions_n = 0
    if isinstance(mask_rules, dict):
        for allowed in (mask_rules.get("clauses") or {}).values():
            for a in allowed:
                actions_n = max(actions_n, int(a) + 1)
    collect_actions = obs_every > 0

    try:
        while True:
            line = proc.stdout.readline()
            if not line:
                break
            line = line.strip()
            if not line:
                continue
            row = json.loads(line)
            if row.get("hello") is not None:
                metrics["sim_obs_size"] = int(row.get("obs", 0))
                metrics["sim_actions"] = int(row.get("actions", 0))
                metrics["sim_forward"] = str(row.get("forward", ""))
                if str(row.get("action_mode", action_mode)) != action_mode:
                    metrics["notes"].append("driver action_mode=%s, harness asked "
                                            "for %s" % (row.get("action_mode"),
                                                        action_mode))
                if "mask1" in row:
                    metrics["core_masks"] = {
                        0: int(row.get("mask0", 0)),
                        1: int(row.get("mask1", 0)),
                        2: int(row.get("mask2", 0)),
                    }
                if int(row["hello"]) == 0:
                    metrics["status"] = "sim_failure"
                    metrics["failure"] = ("native policy load failed: %s"
                                          % row.get("error", ""))
                    break
                if policy is not None and weights is None \
                        and int(row["obs"]) != policy.ref.obs_size:
                    metrics["status"] = "sim_failure"
                    metrics["failure"] = (
                        "schema mismatch: the sim core observes %d floats but the "
                        "artifact was exported for %d; re-export the policy at the "
                        "live core schema" % (int(row["obs"]), policy.ref.obs_size))
                    break
                continue
            if row.get("policy_abort"):
                abort_row = row
                break
            if row.get("end"):
                end_row = row
                break
            if pending is not None:
                phase_of, action_of = pending
                pending = None
                # A shop buy is verified by the core's own step reward (0.5 on a
                # completed purchase), mirroring the bridge's shop_buys counter.
                if phase_of == 1 and 0 <= action_of <= 3 and float(row["r"]) > 0.0:
                    shop["buys"] += 1

            # --- one sampled data row ---------------------------------------
            if mask_obs_prev is None and row.get("obs") is not None:
                # The first data row's own observation is the one the previous
                # (initial) row's masks were chosen from.
                mask_obs_prev = row["obs"]
            ticks = int(row["t"])
            phase = int(row["ph"])
            metrics["waves_reached"] = max(metrics["waves_reached"], int(row["w"]))
            metrics["materials"] = row["mat"]
            metrics["level"] = int(row["lvl"])
            metrics["kills"] = int(row["kills"])
            metrics["ticks"] = ticks
            phase_ticks[phase] = phase_ticks.get(phase, 0) + 1
            if phase in (1, 2):
                stall_steps[phase] += 1
                max_shop_run = max(max_shop_run, stall_steps[1])
                max_level_up_run = max(max_level_up_run, stall_steps[2])
            else:
                stall_steps[1] = 0
                stall_steps[2] = 0
            if metrics["death_wave"] is None and row["hp"] <= 0.0:
                metrics["death_wave"] = int(row["w"])
            if row.get("m") is not None:
                core_mask = int(row["m"])
                applied_mask = int(row.get("am", core_mask))
                # The mask the DRIVER actually applied (the artifact's, as the
                # game applies it) against the core's own bt_action_legal: an
                # action the artifact admits but the core forbids is a hard
                # parity break (the policy would be sampled an action the game's
                # core never trained on), the other direction is safe but
                # reported.  This is the check that catches a mask-spec parsing
                # bug the harness's own structured evaluation cannot see.
                if applied_mask & ~core_mask:
                    mask_admits_illegal += 1
                    if (applied_mask & ~core_mask) & ~_COMBINE_BITS:
                        mask_admits_illegal_other += 1
                elif core_mask & ~applied_mask:
                    mask_narrower += 1
                if mask_rules and mask_obs_prev is not None:
                    # The harness's own evaluation of the artifact metadata for the
                    # observation the driver masked, checked against `am`.
                    harness_mask = mask_from_rules(mask_rules, mask_obs_prev, actions_n)
                    if harness_mask != applied_mask:
                        mask_meta_diff += 1
                # The masks in a row belong to the phase that was current when the
                # action was chosen ("pm"), which is the pre-step phase: a row that
                # entered a new phase carries the previous phase's mask.
                mask_phase = int(row.get("pm", phase))
                declared = None
                if mask_rules and mask_obs_prev is not None:
                    declared = mask_from_rules(mask_rules, mask_obs_prev, actions_n)
                crate = mask_phase == 2 and applied_mask == ((1 << 29) | (1 << 30))
                key = "crate" if crate else mask_phase
                mask_phase_steps[key] = mask_phase_steps.get(key, 0) + 1
                if declared is not None and declared != core_mask:
                    mask_diff += 1
                    if (declared ^ core_mask) & ~_COMBINE_BITS:
                        mask_diff_other += 1
                    mask_phase_steps["core!=applied_phase%d" % mask_phase] = \
                        mask_phase_steps.get("core!=applied_phase%d" % mask_phase, 0) + 1
                if crate and declared is not None and declared != core_mask:
                    mask_phase_steps["crate_core_differs"] = \
                        mask_phase_steps.get("crate_core_differs", 0) + 1
                if collect_actions and len(mask_seen) < CROSSCHECK_STEPS:
                    mask_seen.append(applied_mask)
            if collect_actions:
                if row.get("obs") is not None:
                    if len(obs_seen) < CROSSCHECK_STEPS:
                        obs_seen.append(row["obs"])
                    mask_obs_prev = row["obs"]
                if row.get("a") is not None:
                    actions_seen.append((ticks, int(row["a"])))
            if ticks >= max_steps or steps >= max_steps:
                truncated = True
                break
            if driver_drives:
                # The driver owns the action (native forward / scripted Demon bot).
                steps += 1
                last_phase = phase
                continue
            if policy is not None:
                try:
                    action = policy.act(row["obs"], phase)
                except ValueError as exc:
                    metrics["status"] = "sim_failure"
                    metrics["failure"] = "policy_reference rejected the observation: %s" % exc
                    truncated = True
                    break
                except PolicyAbort as abort:
                    metrics["status"] = "policy_failure"
                    metrics["failure"] = (
                        "tick %d: %s: policy action %d not valid in the %s phase"
                        % (ticks, PHASE_NAMES.get(abort.phase, "phase"),
                           abort.action, PHASE_NAMES.get(abort.phase, abort.phase)))
                    metrics["reason"] = "policy_inference_failed"
                    metrics["policy_abort_tick"] = ticks
                    truncated = True
                    break
            elif action_stream is not None:
                if steps >= len(action_stream):
                    truncated = True
                    break
                action = int(action_stream[steps])
            else:
                action = script.act(row.get("obs"), phase)
            if collect_actions:
                actions_seen.append((ticks, int(action)))
            if phase == 1:
                shop["actions"] += 1
                if action == 4:
                    shop["rerolls"] += 1
                elif action == 8:
                    shop["skips"] += 1
            elif phase == 2:
                shop["level_ups"] += 1
            pending = (phase, action)
            steps += 1
            last_phase = phase
            try:
                proc.stdin.write("%d\n" % action)
                proc.stdin.flush()
            except BrokenPipeError:
                break
    finally:
        try:
            if proc.stdin is not None:
                proc.stdin.close()
        except (BrokenPipeError, ValueError):
            pass
        try:
            proc.wait(timeout=wall_timeout)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
            metrics["failure"] = "sim wall timeout after %ds" % wall_timeout
            metrics["status"] = "sim_failure"
        stderr = proc.stderr.read() if proc.stderr else ""
        if proc.returncode not in (0, None) and metrics["status"] == "ok":
            metrics["status"] = "sim_failure"
            metrics["failure"] = "sim driver exit %s: %s" % (
                proc.returncode, (stderr or "").strip()[-300:])

    metrics["wall_s"] = round(time.time() - started, 2)
    metrics["steps"] = steps
    metrics["shop_buys"] = shop["buys"]
    metrics["shop_rerolls"] = shop["rerolls"]
    metrics["shop_skips"] = shop["skips"]
    metrics["shop_actions"] = shop["actions"]
    metrics["level_ups"] = shop["level_ups"]
    metrics["phase_ticks"] = {"combat": phase_ticks.get(0, 0),
                              "shop": phase_ticks.get(1, 0),
                              "level_up": phase_ticks.get(2, 0)}
    if policy is not None and weights is None:
        metrics["notes"].append("policy_fallbacks=%d/%d" % (policy.fallbacks,
                                                            policy.step_calls))
    if metrics.get("sim_forward"):
        metrics["notes"].append("forward=%s" % metrics["sim_forward"])
    if weights is not None:
        metrics["notes"].append("action_mode=%s (native forward)" % action_mode)
    elif policy is not None:
        metrics["notes"].append("action_mode=%s (policy_reference forward, "
                                "harness-driven)" % action_mode)
    else:
        metrics["notes"].append("action_mode=%s not applied: the harness supplies "
                                "the actions" % action_mode)
    if policy is not None and policy.sampled:
        metrics["notes"].append("sampled=%d" % policy.sampled)

    if abort_row is not None:
        phase = int(abort_row["phase"])
        metrics["status"] = "policy_failure"
        metrics["failure"] = ("tick %d: %s: policy action %d not valid in the %s phase"
                              % (int(abort_row["tick"]), PHASE_NAMES.get(phase, "phase"),
                                 int(abort_row["action"]),
                                 PHASE_NAMES.get(phase, phase)))
        metrics["reason"] = "policy_inference_failed"
        metrics["policy_abort_tick"] = int(abort_row["tick"])
    elif end_row is not None:
        metrics["won"] = bool(end_row.get("won"))
        metrics["waves_reached"] = max(metrics["waves_reached"], int(end_row.get("we", 0)))
        metrics["waves_cleared"] = (20 if (metrics["won"] and not endless)
                                    else max(0, int(end_row.get("we", 0)) - 1))
        metrics["kills"] = int(end_row.get("kills", metrics["kills"]))
        metrics["materials"] = end_row.get("mat")
        metrics["level"] = int(end_row.get("lvl", metrics["level"]))
        metrics["ticks"] = int(end_row.get("ticks", metrics["ticks"]))
        metrics["steps"] = int(end_row.get("steps", metrics["steps"]))
        metrics["shop_buys"] = int(end_row.get("buys", metrics["shop_buys"]))
        metrics["shop_rerolls"] = int(end_row.get("rerolls", metrics["shop_rerolls"]))
        metrics["shop_skips"] = int(end_row.get("skips", metrics["shop_skips"]))
        metrics["shop_recycles"] = int(end_row.get("recycles", 0))
        metrics["shop_combines"] = int(end_row.get("combines", 0))
        metrics["level_ups"] = int(end_row.get("level_ups", metrics["level_ups"]))
        max_shop_run = max(max_shop_run, int(end_row.get("max_shop_run", 0)))
        max_level_up_run = max(max_level_up_run,
                               int(end_row.get("max_lvl_run", 0)))
        if int(end_row.get("ew", 0)) > metrics["waves_reached"]:
            metrics["waves_reached"] = int(end_row["ew"])
        if not metrics["won"] and metrics["death_wave"] is None \
                and not end_row.get("trunc"):
            # A truncated row is a budget cut, not a death: the wave it stopped
            # in was not lost.
            metrics["death_wave"] = int(end_row["w"])
        metrics["reason"] = "won" if metrics["won"] else "died"
        if end_row.get("trunc"):
            metrics["reason"] = "tick_budget"
            metrics["no_progress"] = True
            if not metrics["stall"]:
                metrics["stall"] = "no_progress"
    elif metrics["status"] == "ok":
        metrics["reason"] = "tick_budget"
        metrics["no_progress"] = True

    if metrics["status"] == "ok":
        if max_shop_run >= STALL_TICKS or (truncated and last_phase == 1):
            metrics["stall"] = "shop"
            metrics["no_progress"] = True
        elif max_level_up_run >= STALL_TICKS or (truncated and last_phase == 2):
            metrics["stall"] = "level_up"
            metrics["no_progress"] = True
        elif truncated and not metrics["won"] and metrics["death_wave"] is None:
            metrics["stall"] = "no_progress"
    if metrics["status"] == "policy_failure":
        metrics["notes"].append("policy_abort_tick=%s"
                                % metrics.pop("policy_abort_tick", "?"))
    if metrics.get("sim_obs_size"):
        metrics["notes"].append("sim_obs=%d" % metrics["sim_obs_size"])
    masks = metrics.pop("core_masks", None)
    if masks:
        # The core's own per-phase mask (bt_action_mask) vs the bridge's gate: a
        # real, measured contract check rather than a documented assumption.
        gate = _VALID_ACTIONS or init_action_gate() or _VALID_ACTIONS
        for phase, key in ((1, "shop"), (2, "level_up")):
            core = {a for a in range(64) if (masks[phase] >> a) & 1}
            if phase == 2 and masks[2] and masks[2] == (1 << 29) | (1 << 30):
                continue        # crate-open mask replaces the choices; noted live
            bridge = set(gate.get(key, set()))
            if core != bridge:
                metrics["notes"].append(
                    "core_mask[%s]=%s vs bridge gate %s" % (key, sorted(core),
                                                            sorted(bridge)))
        metrics["core_masks"] = {str(k): v for k, v in masks.items()}
    if actions_seen:
        metrics["actions_head"] = [a for _t, a in actions_seen[:CROSSCHECK_STEPS]]
    if obs_seen:
        metrics["obs_head"] = obs_seen[:CROSSCHECK_STEPS]
    if mask_seen:
        metrics["mask_head"] = mask_seen[:CROSSCHECK_STEPS]
    if mask_phase_steps:
        # Applied mask (the artifact's rules, i.e. the game's) vs the core's own
        # bt_action_mask: a difference is a compatibility fact (the crate selector
        # is the known open one) and is reported per step count.
        metrics["mask_report"] = {
            "phase_steps": {str(k): v for k, v in sorted(mask_phase_steps.items(),
                                                         key=str)},
            "core_mask_differs_on": mask_diff,
            # The parity check that matters: the mask the DRIVER applied (the
            # artifact's, exactly as the game applies it) must never admit an
            # action the core's bt_action_legal forbids.  The harness's own
            # structured re-evaluation is reported next to it, because a
            # spec-parsing bug inside the driver is invisible to that one.
            "applied_equals_core": mask_admits_illegal == 0,
            "driver_mask_admits_core_illegal": mask_admits_illegal,
            "driver_mask_admits_core_illegal_outside_combine":
                mask_admits_illegal_other,
            "driver_mask_narrower_than_core": mask_narrower,
            "harness_equals_core": mask_diff == 0,
            "harness_mask_differs_outside_combine": mask_diff_other,
        }
        if mask_admits_illegal_other or mask_diff_other:
            if metrics["status"] == "ok":
                metrics["status"] = "mask_failure"
            metrics["failure"] = "mask_parity"
            metrics["notes"].append(
                "MASK PARITY FAILURE: the applied (artifact) mask admits "
                "core-illegal action(s) on %d step(s) and the harness's own "
                "evaluation differs from bt_action_legal on %d step(s) outside "
                "the combine-upgrade-path residual"
                % (mask_admits_illegal_other, mask_diff_other))
        metrics["notes"].append(
            "mask steps: " + ", ".join("%s=%d" % (k, v) for k, v in
                                       sorted(mask_phase_steps.items(), key=str))
            + ("; applied mask admits core-illegal id(s) on %d step(s)"
               % mask_admits_illegal if mask_admits_illegal
               else "; applied mask == core bt_action_legal on every step")
            + ("; applied mask narrower than the core on %d step(s)"
               % mask_narrower if mask_narrower else "")
            + ("" if mask_rules else "; artifact declares no mask rules")
            + ("; harness evaluation != applied mask on %d step(s)" % mask_meta_diff
               if mask_meta_diff else ""))
    return metrics


# ---------------------------------------------------------------------------
# Game episode
# ---------------------------------------------------------------------------
STALL_TICKS = 600    # 10 simulated seconds without leaving shop/level-up
CROSSCHECK_STEPS = 900   # native-vs-reference action agreement window

# Endless defaults (ArosRising's High Endless plan): Demon starts weapon_ghost_scepter_1.
ENDLESS_CHARACTER = "character_demon"
ENDLESS_WEAPON = "weapon_ghost_scepter_1"


def new_metrics(seed: int, source: str) -> dict:
    return {
        "seed": seed,
        "source": source,
        "status": "ok",
        "failure": "",
        "reason": "",
        "wall_s": 0.0,
        "ticks": 0,
        "waves_reached": 0,
        "waves_cleared": 0,
        "won": False,
        "death_wave": None,
        "materials": None,
        "level": 0,
        "kills": None,
        "shop_buys": 0,
        "shop_rerolls": 0,
        "shop_skips": 0,
        "shop_recycles": 0,
        "shop_combines": 0,
        "shop_actions": 0,
        "level_ups": 0,
        "stall": "",
        "no_progress": False,
        "phase_ticks": {},
        "scenes": [],
        "check_ok": None,
        "flags": [],
        "notes": [],
    }


def snapshot_launcher(run_root: Path) -> tuple[Path, str]:
    """Freeze run_game.sh for this invocation.

    bash reads a script lazily by byte offset, so an editor saving into
    run_game.sh while episodes are running makes those episodes execute a mix of
    old and new bytes (observed: a bogus "Permission denied" at a shifted line
    and a nonzero rc, with a perfectly valid trace).  Every episode of one
    invocation therefore runs one immutable copy, whose hash is reported.
    """
    import hashlib

    launcher_dir = run_root / "launcher"
    launcher_dir.mkdir(parents=True, exist_ok=True)
    frozen = launcher_dir / "run_game.snapshot.sh"
    shutil.copy2(RUN_GAME, frozen)
    frozen.chmod(0o755)
    digest = hashlib.sha256(frozen.read_bytes()).hexdigest()
    return frozen, digest


def eval_external_root(run_root: Path) -> Path:
    """Private generated-source root, so a concurrent run_game.sh (another
    agent's or another eval) cannot rewrite the tables under our engines."""
    external = run_root / "external"
    external.mkdir(parents=True, exist_ok=True)
    return external


def private_tmpdir(run_root: Path) -> str:
    """A private TMPDIR for the launcher (xvfb-run's mktemp + the engine).

    With /tmp at its quota (Errno 122) xvfb-run cannot create its Xauthority and
    the engine then reports the display as unavailable, which looks like a broken
    X server rather than a full /tmp.
    """
    path = run_root / "tmp"
    path.mkdir(parents=True, exist_ok=True)
    return str(path)


def run_launcher(launcher: Path, env: dict, display_num: int = 199,
                 **kwargs) -> subprocess.CompletedProcess:
    """Run the frozen launcher, providing a display when the caller has none.

    run_game.sh wraps only its *launch* in xvfb-run, but its staging/build step
    runs the engine's GDScript parse check, which needs an X display: without one
    the build fails ("X11 Display is not available", "the bridge was never
    instantiated").  Wrapping the whole script (when DISPLAY is unset and
    xvfb-run exists) gives the build a display too, and run_game.sh then sees
    DISPLAY set and does not wrap a second time.
    """
    cmd = ["bash", str(launcher)]
    wraps = (not env.get("DISPLAY") and env.get("BROTATO_XVFB", "auto") != "0"
             and shutil.which("xvfb-run"))
    if wraps:
        # An EXPLICIT display number, never -a: this host carries >100 stale
        # /tmp/.X11-unix/X* sockets, and xvfb-run -a picks one of those numbers,
        # Xvfb fails to bind, and the engine then reports the display as
        # unavailable (which reads like a broken X server).  One number per
        # episode keeps parallel episodes apart.
        cmd = ["xvfb-run", "-n", str(display_num), "-s", "-screen 0 %s"
               % env.get("BROTATO_XVFB_SCREEN", "1280x720x24")] + cmd
    # A display that failed to bind looks exactly like a broken pack ("X11 Display
    # is not available" -> "the bridge was never instantiated" -> parse check
    # FAILED), which would be a false pre-land failure under display contention.
    # Retry on a fresh display number instead of trusting that verdict; every
    # check is re-run, never skipped.
    attempts = kwargs.pop("display_attempts", 3)
    result = None
    for attempt in range(max(1, attempts)):
        result = subprocess.run(cmd, env=env, cwd=str(REPO), **kwargs)
        text = (result.stdout or "") + (result.stderr or "")
        if not wraps or "X11 Display is not available" not in text:
            return result
        if attempt + 1 < attempts:
            display_num += 700
            cmd = ["xvfb-run", "-n", str(display_num), "-s", "-screen 0 %s"
                   % env.get("BROTATO_XVFB_SCREEN", "1280x720x24"),
                   "bash", str(launcher)]
    return result


BUILD_INPUT_SUFFIXES = (".gd", ".py", ".json", ".tscn", ".tres", ".cfg", ".sh",
                        ".gdignore", ".txt", ".md", ".gdshader")
# Documentation and this tool are not build inputs.
BUILD_INPUT_EXCLUDE = {"EVAL.md", "eval_game.py", "obs_action_audit.md"}


def build_inputs_digest() -> str:
    """Content digest of everything the patched pack is built from.

    The pack embeds the mod's GDScript, so a peer landing a mod change makes an
    existing Brotato.rl.pck stale.  Reusing it would silently evaluate the old
    bridge (observed: the staged pack served the 161-float observation after the
    mod had moved to 198, and the policy was rejected as a schema mismatch).
    """
    import hashlib

    digest = hashlib.sha256()
    roots = [HERE / "mod"]
    for root in roots:
        for path in sorted(root.rglob("*")):
            if not path.is_file() or "__pycache__" in path.parts:
                continue
            if path.suffix not in BUILD_INPUT_SUFFIXES:
                continue
            digest.update(str(path.relative_to(HERE)).encode())
            digest.update(b"\0")
            digest.update(hashlib.sha256(path.read_bytes()).digest())
    for path in sorted(HERE.iterdir()):
        if path.name in BUILD_INPUT_EXCLUDE:
            continue
        if path.is_file() and path.suffix in BUILD_INPUT_SUFFIXES:
            digest.update(path.name.encode())
            digest.update(b"\0")
            digest.update(hashlib.sha256(path.read_bytes()).digest())
    # The pack also embeds shared repo-sourced files that neither root above
    # covers (build_mod_pck's `repo` sources): the GDScript policy the mod loads
    # as res://mods-unpacked/rlbridge-headlessrl/brotato_policy.gd.  Without it a
    # policy-script edit left the staged pack stale (observed: the artifact's
    # action_mask never reached rl_policy.gd, so the bridge ran unmasked).
    for path in sorted(POLICY_DIR.glob("*.gd")):
        digest.update(str(path.relative_to(REPO)).encode())
        digest.update(b"\0")
        digest.update(hashlib.sha256(path.read_bytes()).digest())
    return digest.hexdigest()


def gate_spec() -> str:
    """The per-phase action gate as the sim driver's ``--gate`` argument.

    Built from the *effective* gate (the bridge's own rule), so the driver aborts
    exactly where the game would.  Combat is never gated (the driver checks
    phases 1 and 2 only)."""
    if _VALID_ACTIONS is None:
        init_action_gate()
    parts = []
    for phase, key in ((1, "shop"), (2, "level_up")):
        actions = sorted(_VALID_ACTIONS.get(key, set()))
        if not actions:
            continue
        ranges = []
        start = prev = actions[0]
        for value in actions[1:]:
            if value == prev + 1:
                prev = value
                continue
            ranges.append(str(start) if start == prev else "%d-%d" % (start, prev))
            start = prev = value
        ranges.append(str(start) if start == prev else "%d-%d" % (start, prev))
        parts.append("%d:%s" % (phase, ",".join(ranges)))
    return ";".join(parts)


def artifact_mask_spec(artifact: dict | None):
    """(clauses, selectors, phase_field, masked_logit) from the artifact.

    The artifact carries the whole mask contract (export_policy.py emits it):
    ``legal_by_phase`` keyed by clause name ("2_crate"), ``phase_field_index``,
    and ``selectors`` whose ``when_nonzero`` field picks a replacement clause for
    an observation -- exactly what rl_policy.gd consumes.  Nothing is flattened
    and no clause is dropped: the driver evaluates the same rules per observation.
    ``None`` means the artifact predates mask support, so the game applies no mask
    and the sim matches that (the core's own mask is still recorded).
    """
    if not isinstance(artifact, dict):
        return None, None, -1, -1e4
    rules = artifact.get("action_mask")
    if not isinstance(rules, dict):
        return None, None, -1, -1e4
    masked_logit = float(rules.get("masked_logit", -1e4))
    phase_field = int(rules.get("phase_field_index", -1))
    by_phase = rules.get("legal_by_phase")
    if not isinstance(by_phase, dict) or not by_phase:
        return None, None, phase_field, masked_logit
    clauses = []
    for key, ranges in by_phase.items():
        if not isinstance(ranges, list):
            continue
        spans = []
        for pair in ranges:
            if isinstance(pair, list) and len(pair) >= 2:
                low, high = int(pair[0]), int(pair[1])
                spans.append(str(low) if low == high else "%d-%d" % (low, high))
        if spans:
            clauses.append("%s:%s" % (key, ",".join(spans)))
    selectors = []
    for sel in rules.get("selectors") or []:
        if not isinstance(sel, dict):
            continue
        when = sel.get("when_nonzero") or {}
        phase = int(sel.get("phase", -1))
        index = int(when.get("index", -1)) if isinstance(when, dict) else -1
        use = str(sel.get("use", ""))
        if phase >= 0 and index >= 0 and use:
            selectors.append("%d:%d:%s" % (phase, index, use))
    return (";".join(clauses) or None, ";".join(selectors), phase_field,
            masked_logit)


PHASE_NAMES = {0: "combat", 1: "shop", 2: "level_up"}


def artifact_mask_rules(artifact: dict | None) -> dict:
    """{"clauses": {name: frozenset(ids)}, "selectors": [(phase, index, use)],
    "phase_field": int} from the artifact's own action_mask -- the same rules the
    driver evaluates, so the harness can check the driver's per-step mask."""
    rules = (artifact or {}).get("action_mask")
    out = {"clauses": {}, "selectors": [], "conditionals": [], "phase_field": -1}
    if not isinstance(rules, dict):
        return out
    out["phase_field"] = int(rules.get("phase_field_index", -1))
    for key, ranges in (rules.get("legal_by_phase") or {}).items():
        if not isinstance(ranges, list):
            continue
        allowed = set()
        for pair in ranges:
            if isinstance(pair, list) and len(pair) >= 2:
                allowed.update(range(int(pair[0]), int(pair[1]) + 1))
        out["clauses"][str(key)] = frozenset(allowed)
    for sel in rules.get("selectors") or []:
        if not isinstance(sel, dict):
            continue
        when = sel.get("when_nonzero") or {}
        out["selectors"].append((int(sel.get("phase", -1)),
                                 int(when.get("index", -1)) if isinstance(when, dict) else -1,
                                 str(sel.get("use", ""))))
    conds = rules.get("conditionals")
    if isinstance(conds, list):
        out["conditionals"] = conds
    return out


def _cond_test(t: dict, obs, k: int, actions: int) -> bool:
    """One artifact conditional test for the k-th id of its block (mirrors
    rl_policy.gd._test_passes and the C++ mask_cond_test)."""
    op = str(t.get("op", "nonzero"))
    if op == "ge":
        a, b = int(t.get("field", -1)), int(t.get("other", -1))
        if not (0 <= a < len(obs) and 0 <= b < len(obs)):
            return False
        return obs[a] >= obs[b] * float(t.get("scale", 1.0))
    base, stride = int(t.get("field_base", -1)), int(t.get("field_stride", 0))
    if base < 0:
        return False
    if op == "block_count_gte":
        n = sum(1 for j in range(int(t.get("count", 0)))
                if 0 <= base + j * stride < len(obs) and obs[base + j * stride] != 0.0)
        return n >= int(t.get("value", 0))
    field = base + k * stride
    if not (0 <= field < len(obs)):
        return False
    if op == "equals":
        return obs[field] == float(t.get("value", 0.0))
    if op == "block_duplicate":
        mine = obs[field]
        if mine == 0.0:
            return False
        return any(j != k and 0 <= base + j * stride < len(obs)
                   and obs[base + j * stride] == mine
                   for j in range(int(t.get("count", 0))))
    return obs[field] != 0.0


def mask_from_rules(rules: dict, obs, actions: int) -> int:
    """The artifact's mask for one observation, as a bitmask (the harness-side
    re-derivation the driver's `am` column is checked against)."""
    if not rules or not rules.get("clauses"):
        return (1 << actions) - 1
    field = rules.get("phase_field", -1)
    phase = int(round(obs[field])) if 0 <= field < len(obs) else -1
    key = "%d_%s" % (phase, PHASE_NAMES.get(phase, "phase"))
    for sel_phase, index, use in rules.get("selectors", []):
        if sel_phase == phase and 0 <= index < len(obs) and obs[index] != 0.0:
            key = use
            break
    allowed = rules["clauses"].get(key)
    if not allowed:
        return (1 << actions) - 1
    legal = set(allowed)
    for cond in rules.get("conditionals") or []:
        if not isinstance(cond, dict):
            continue
        clause = str(cond.get("clause", ""))
        if clause and clause != key:
            continue
        for k in range(int(cond.get("count", 0))):
            target = int(cond.get("id_base", -1)) + k
            if target < 0 or target >= actions:
                continue
            if all(_cond_test(t, obs, k, actions) for t in cond.get("tests") or []):
                legal.add(target)
            else:
                legal.discard(target)
    bits = 0
    for a in legal:
        bits |= 1 << a
    return bits


def resolve_sim_weapon(run_root: Path, name: str) -> tuple[int, str]:
    """(BT_WEAPONS id, canonical name) for a starting-weapon id string, using the
    same canonical tables the bridge/replay use."""
    import importlib.util

    spec = importlib.util.spec_from_file_location("brotato_replay_cpu", HERE / "replay_cpu.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    candidates = [eval_external_root(run_root) / "brotato_obs_tables.json",
                  REPO / "migration" / "game" / "external" / "brotato_obs_tables.json",
                  Path.home() / "brotato_mod" / "external" / "brotato_obs_tables.json"]
    tables = None
    for path in candidates:
        if path.is_file():
            tables = module.load_tables(path)
            break
    if tables is None:
        raise EvalError("no brotato_obs_tables.json to resolve the starting weapon "
                        "(run one game episode first, or pass --sim-weapon)")
    index, canonical = module.resolve_weapon(tables, name)
    if index is None:
        raise EvalError("starting weapon %r is unknown/ambiguous in the canonical "
                        "weapon table" % name)
    return int(index), canonical


def display_for(tag: str) -> int:
    """A stable, per-invocation Xvfb display number in a high range."""
    import zlib
    return 200 + (zlib.crc32(tag.encode()) % 600)


def stage_shared(run_root: Path, force: bool, quiet: bool,
                 reuse: bool = False) -> Path:
    """Stage the retail inputs and build the patched pack once, shared by every
    episode (per-episode run dirs hard-link these files).  Rebuilt whenever the
    build inputs change."""
    stage = run_root / "stage"
    marker = stage / "Brotato.rl.pck"
    digest_path = stage / "build_inputs.sha256"
    digest = build_inputs_digest()
    if marker.is_file() and reuse:
        if not quiet:
            print("eval: reusing the existing patched pack (--reuse-pack)")
        return stage
    if marker.is_file() and not force and digest_path.is_file() \
            and digest_path.read_text().strip() == digest:
        return stage
    stage.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["BROTATO_REPO"] = str(REPO)      # the snapshot cannot derive it
    env["BROTATO_RUN_DIR"] = str(stage)
    env["BROTATO_NO_LAUNCH"] = "1"
    env["BROTATO_EXTERNAL_ROOT"] = str(eval_external_root(run_root))
    env["TMPDIR"] = private_tmpdir(run_root)
    env.pop("BROTATO_TAG", None)
    if not quiet:
        print("eval: staging retail inputs + building the patched pack (once)")
    proc = run_launcher(snapshot_launcher(run_root)[0], env,
                        display_num=199, capture_output=True, text=True)
    if proc.returncode != 0 or not marker.is_file():
        raise EvalError("staging/build failed (rc=%d):\n%s\n%s"
                        % (proc.returncode, proc.stdout[-2000:], proc.stderr[-2000:]))
    digest_path.write_text(digest + "\n")
    return stage


def prepare_run_dir(run_root: Path, tag: str, stage: Path) -> tuple[Path, Path]:
    run_dir = run_root / ("run_" + tag)
    iso_home = run_dir / "isolated_home"
    (run_dir / "rl_out").mkdir(parents=True, exist_ok=True)
    iso_home.mkdir(parents=True, exist_ok=True)
    for name in ("Brotato.pck", "BrotatoAbyssalTerrors.pck", "steam_data.json",
                 "Brotato.rl.pck"):
        src = stage / name
        if not src.is_file():
            raise EvalError("staging is missing %s" % src)
        dst = run_dir / name
        if dst.exists():
            # A rebuilt pack is a new file; a stale hard link would keep the old
            # bridge (and its old observation schema) in this episode.
            src_stat, dst_stat = src.stat(), dst.stat()
            if (src_stat.st_size, src_stat.st_mtime_ns, src_stat.st_ino) == \
                    (dst_stat.st_size, dst_stat.st_mtime_ns, dst_stat.st_ino):
                continue
            dst.unlink()
        try:
            os.link(src, dst)              # hard link: no 200 MB copies per seed
        except OSError:
            shutil.copy2(src, dst)
    return run_dir, iso_home


def run_game_episode(args, tag: str, seed: int, stage: Path,
                     launcher: Path) -> dict:
    metrics = new_metrics(seed, "game")
    metrics["endless"] = bool(args.endless)
    metrics["invulnerable"] = bool(args.invulnerable)
    metrics["start_wave"] = int(args.start_wave)
    metrics["action_mode"] = args.action_mode
    if args.action_source == "policy":
        metrics["notes"].append("action_mode=%s" % args.action_mode)
    run_dir, iso_home = prepare_run_dir(args.run_root, tag, stage)
    out_dir = run_dir / "rl_out"
    trace = out_dir / ("rl_trace.%s.jsonl" % tag)
    summary_path = Path(str(trace) + ".summary.json")
    meta_path = Path(str(trace) + ".meta.json")

    env = os.environ.copy()
    env.update({
        "BROTATO_REPO": str(REPO),
        "BROTATO_RUN_DIR": str(run_dir),
        "BROTATO_EXTERNAL_ROOT": str(eval_external_root(args.run_root)),
        "TMPDIR": private_tmpdir(args.run_root),
        "BROTATO_ISO_HOME": str(iso_home),
        "BROTATO_TAG": tag,
        "BROTATO_SKIP_STAGE": "1",
        "BROTATO_SKIP_BUILD": "1",
        "BROTATO_MAX_SECONDS": str(args.max_seconds),
        "BROTATO_WATCHDOG": str(args.watchdog),
        "BROTATO_RL_MAX_TICKS": str(args.max_seconds * 60),
        "BROTATO_RL_SAMPLE_EVERY": str(args.sample_every),
        "BROTATO_CHAR": args.game_char,
        "BROTATO_RL_CHAR": args.game_char,
        "BROTATO_RL_DANGER": str(args.danger),
        "BROTATO_RL_SEED": str(seed),
        "BROTATO_RL_NO_SAVE": "0",     # let the game save its own run stats
        "BROTATO_ACTION_SOURCE": args.action_source,
        "BROTATO_RL_ACTION_MODE": args.action_mode,
    })
    if args.start_wave:
        env["BROTATO_RL_WAVE"] = str(args.start_wave)
    if args.game_weapon:
        env["BROTATO_WEAPON"] = args.game_weapon
        env["BROTATO_RL_WEAPON"] = args.game_weapon
    if args.endless:
        env["BROTATO_ENDLESS"] = "1"
    if args.invulnerable:
        env["BROTATO_RL_INVULNERABLE"] = "1"
    if args.action_source == "policy":
        env["BROTATO_POLICY"] = "1"
        env["BROTATO_RL_POLICY_ARTIFACT"] = str(args.policy)
    if args.action_source == "file":
        # The mod consumes one id per physics frame (last integer of a line) and
        # fails boot on a bad stream, so both sides read the same file.
        env["BROTATO_RL_ACTION_FILE"] = str(args.action_file.resolve())

    started = time.time()
    try:
        proc = run_launcher(launcher, env, display_num=display_for(tag),
                            capture_output=True, text=True,
                            timeout=args.watchdog + 120)
        rc, stdout, stderr = proc.returncode, proc.stdout, proc.stderr
    except subprocess.TimeoutExpired as exc:
        rc, stdout, stderr = 124, exc.stdout or "", exc.stderr or ""
        metrics["status"] = "bridge_failure"
        metrics["failure"] = "harness timeout after %ds" % (args.watchdog + 120)
    metrics["wall_s"] = round(time.time() - started, 2)
    metrics["rc"] = rc
    (out_dir / ("harness.%s.log" % tag)).write_text(
        (stdout or "") + "\n--- stderr ---\n" + (stderr or ""))

    summary = read_json(summary_path)
    trace_stats = parse_game_trace(trace, metrics)
    game_user_dir = find_game_user_dir(iso_home)
    log_marks = parse_game_log(game_user_dir / "log.txt") if game_user_dir else {}
    save_stats = read_save_stats(game_user_dir) if game_user_dir else {}
    check = parse_check_output((stdout or "") + "\n" + (stderr or ""))

    classify_game(metrics, rc, summary, trace_stats, log_marks, save_stats,
                  check, stdout or "", stderr or "")
    if rc == 124:
        # The harness itself was killed on the wall clock: no partial success.
        metrics["status"] = "bridge_failure"
        metrics["failure"] = "harness timeout after %ds" % (args.watchdog + 120)
    if getattr(args, "reuse_pack", False):
        metrics["notes"].append("pack reused without build-input check")
    if getattr(args, "invulnerable", False):
        metrics["flags"].append("invulnerable")
        metrics["notes"].append("invulnerable (DebugService; smoke only)")
    if getattr(args, "endless", False):
        metrics["flags"].append("endless")
        metrics["notes"].append("endless (RunData.is_endless_run)")
    if getattr(args, "start_wave", 0):
        metrics["flags"].append("start_wave=%d" % args.start_wave)
        metrics["notes"].append("started at wave %d (DebugService.starting_wave)"
                                % args.start_wave)
    if getattr(args, "reuse_pack", False):
        metrics["flags"].append("reuse_pack")
    metrics["trace"] = str(trace)
    metrics["summary"] = str(summary_path)
    if args.prune_traces and trace.is_file():
        for suffix in ("", ".meta.json", ".summary.json"):
            p = Path(str(trace) + suffix)
            if p.is_file() and p.stat().st_size > args.keep_trace_bytes:
                p.unlink()
    return metrics


def parse_check_output(stdout: str) -> dict:
    """Facts from the launcher's own output: the trace gate result, the
    native-input verification counters, and any ``trace error:`` line (the
    validator's own explanation for its exit code)."""
    out = {"check_ok": None, "check_reasons": [], "applications": None,
           "verified": None, "mismatches": None, "game_inhibited": None,
           "trace_error": "", "engine_status": None, "watchdog": False}
    err = re.search(r"trace error: (.*)", stdout)
    if err:
        out["trace_error"] = err.group(1).strip()
    if "CHECK OK" in stdout:
        out["check_ok"] = True
    elif "CHECK FAILED" in stdout:
        out["check_ok"] = False
        capturing = False
        for line in stdout.splitlines():
            if line.strip().startswith("CHECK FAILED"):
                capturing = True
                continue
            if capturing:
                if line.startswith("  - "):
                    out["check_reasons"].append(line.strip()[4:])
                elif line.strip():
                    break
    engine = re.search(r"engine exit status (-?\d+)", stdout)
    if engine:
        out["engine_status"] = int(engine.group(1))
    out["watchdog"] = "watchdog expired after" in stdout
    inhibited = re.search(r"game_inhibited=(\d+)", stdout)
    if inhibited:
        out["game_inhibited"] = int(inhibited.group(1))
    m = re.search(r"applications=(\d+) verified=(\d+) mismatches=(\d+)", stdout)
    if m:
        out["applications"] = int(m.group(1))
        out["verified"] = int(m.group(2))
        out["mismatches"] = int(m.group(3))
    return out


def read_json(path: Path):
    try:
        return json.loads(path.read_text())
    except (OSError, json.JSONDecodeError):
        return None


def parse_game_trace(trace: Path, metrics: dict) -> dict:
    """Stream the bridge trace; keep only aggregates (a full run is ~100 MB)."""
    stats = {"rows": 0, "max_wave": 0, "final_gold": None, "max_level": 0,
             "first_death_wave": None, "last_scene": "", "last_phase": -1,
             "last_tick": -1, "scenes": [], "phase_ticks": {}, "incomplete": 0,
             "policy_ok": 0, "policy_bad": 0, "action_sources": {},
             "max_wave_in_progress": 0, "last_row": None,
             "movement_rows": 0, "movement_applied": 0, "holds": 0,
             "movement_rows_active": 0, "movement_applied_active": 0,
             "movement_rows_cleanup": 0, "movement_applied_cleanup": 0,
             "movement_inhibited": 0, "movement_unguarded_miss": 0,
             "unguarded_example": None, "verify_errors": 0,
             "shop_run": 0, "max_shop_run": 0, "levelup_run": 0,
             "max_levelup_run": 0}
    if not trace.is_file():
        return stats
    with trace.open() as handle:
        for line in handle:
            line = line.strip()
            if not line or line.startswith('{"record"'):
                continue
            try:
                row = json.loads(line)
            except json.JSONDecodeError:
                continue
            stats["rows"] += 1
            wave = int(row.get("wave", 0))
            stats["max_wave"] = max(stats["max_wave"], wave)
            if row.get("wave_in_progress"):
                stats["max_wave_in_progress"] = max(stats["max_wave_in_progress"], wave)
            if row.get("gold") is not None:
                stats["final_gold"] = row["gold"]
            stats["max_level"] = max(stats["max_level"], int(row.get("level", 0)))
            if stats["first_death_wave"] is None and float(row.get("hp", 1)) <= 0.0:
                stats["first_death_wave"] = wave
            scene = str(row.get("scene", ""))
            if scene and scene not in stats["scenes"]:
                stats["scenes"].append(scene)
            stats["last_scene"] = scene
            phase = int(row.get("phase", -1))
            stats["last_phase"] = phase
            stats["phase_ticks"][phase] = stats["phase_ticks"].get(phase, 0) + 1
            stats["last_tick"] = int(row.get("tick", stats["last_tick"]))
            if row.get("action_file_index") is not None:
                stats["action_file_max"] = max(stats.get("action_file_max", -1),
                                               int(row["action_file_index"]))
            stats["last_row"] = row
            if row.get("obs_complete", 1) != 1:
                stats["incomplete"] += 1
            if row.get("policy_ok"):
                stats["policy_ok"] += 1
            elif str(row.get("action_source", "")) == "policy":
                stats["policy_bad"] += 1
            src = str(row.get("action_source", ""))
            stats["action_sources"][src] = stats["action_sources"].get(src, 0) + 1
            act = row.get("action")
            if isinstance(act, int) and not isinstance(act, bool):
                if act == 0 or act > 8:
                    stats["holds"] += 1
                elif row.get("phase") not in (0, None) or row.get("scene") == "Shop":
                    # A shop/level-up row is not a movement comparison, whatever
                    # action id it carries: only the arena injects movement.
                    stats["holds"] += 1
                else:
                    stats["movement_rows"] += 1
                    applied = 1 if row.get("action_applied") else 0
                    stats["movement_applied"] += applied
                    if row.get("action_verify_error"):
                        stats["verify_errors"] += 1
                    # The bridge's own guard is authoritative when present: a
                    # failed comparison is game-inhibited only when the Player
                    # itself reports cleaning_up or !_can_move (main.gd:808-810,
                    # unit.gd:137), and everything else stays a real mismatch.
                    err = str(row.get("action_verify_error") or "")
                    guarded = (err.startswith("game_inhibited_input")
                               or row.get("player_can_move") == 0
                               or row.get("player_cleaning_up") == 1)
                    # A frame the bridge deliberately did not compare (a phase
                    # transition resync) is neither an inhibited input nor a miss.
                    uncompared = err.startswith("phase_transition_resync")
                    has_guard_fields = any(k in row for k in
                                           ("player_can_move", "player_cleaning_up",
                                            "action_verify_error"))
                    if not applied and uncompared:
                        stats["movement_uncompared"] = \
                            stats.get("movement_uncompared", 0) + 1
                    elif not applied:
                        if guarded:
                            stats["movement_inhibited"] += 1
                        elif has_guard_fields:
                            stats["movement_unguarded_miss"] += 1
                            if stats["unguarded_example"] is None:
                                stats["unguarded_example"] = {
                                    k: row.get(k) for k in
                                    ("tick", "action", "action_applied",
                                     "action_verify_error", "player_can_move",
                                     "player_cleaning_up", "player_move_locked",
                                     "wave", "wave_in_progress", "wave_time_left",
                                     "phase", "scene")}
                    # player.gd:808-810: while the wave is over the arena is still
                    # the active scene (the cleanup window, ~120 ticks) with
                    # _can_move false and _current_movement ZERO, so a movement
                    # request legitimately does not register there.
                    if row.get("wave_in_progress"):
                        stats["movement_rows_active"] += 1
                        stats["movement_applied_active"] += applied
                    else:
                        stats["movement_rows_cleanup"] += 1
                        stats["movement_applied_cleanup"] += applied
            if scene == "Shop" or phase == 1:
                stats["shop_run"] += 1
                stats["max_shop_run"] = max(stats["max_shop_run"], stats["shop_run"])
                stats["levelup_run"] = 0
            elif phase == 2:
                stats["levelup_run"] += 1
                stats["max_levelup_run"] = max(stats["max_levelup_run"], stats["levelup_run"])
                stats["shop_run"] = 0
            else:
                stats["shop_run"] = 0
                stats["levelup_run"] = 0
    return stats


def find_game_user_dir(iso_home: Path) -> Path | None:
    """The engine's user:// dir under the isolated HOME (XDG_DATA_HOME/<name>)."""
    share = iso_home / ".local" / "share"
    if share.is_dir():
        for child in sorted(share.iterdir()):
            if (child / "user").is_dir():
                return child / "user"
    return None


def parse_game_log(path: Path) -> dict:
    """The game's own user/log.txt (written unconditionally by DebugService)."""
    marks = {"won": False, "lost": False, "waves_ended": 0, "endless": False,
             "run_data_blocks": [], "exists": False}
    if not path.is_file():
        return marks
    marks["exists"] = True
    text = path.read_text(errors="replace")
    marks["won"] = "is_run_won" in text
    marks["lost"] = "is_run_lost" in text
    marks["endless"] = "is_endless_run" in text
    marks["waves_ended"] = text.count("wave_cleared_label started...")
    for block in re.findall(r"--Run Data--(.*?)--Run Data end--", text, re.S):
        entry = {}
        for key, pattern in (("gold", r"Gold:\s*(-?\d+)"),
                             ("level_ups", r"Level ups:\s*(\d+)"),
                             ("consumables", r"Consumables:\s*(\d+)"),
                             ("weapons", r"Weapons:\s*(.*)"),
                             ("wave", r"Wave:\s*(\d+)")):
            m = re.search(pattern, block)
            if m:
                entry[key] = m.group(1).strip()
        marks["run_data_blocks"].append(entry)
    return marks


def read_save_stats(user_dir: Path) -> dict:
    """The game's own run stats (ProgressData.data), from the isolated save.

    With BROTATO_RL_NO_SAVE=0 the game writes save_v3_0.json at every wave end;
    in a fresh isolated home ``data`` starts at zero, so ``enemies_killed`` and
    ``materials_collected`` are this episode's totals.
    """
    data = read_json(user_dir / "save_v3_0.json")
    if not isinstance(data, dict):
        return {}
    stats = data.get("data")
    return stats if isinstance(stats, dict) else {}


def classify_game(metrics: dict, rc: int, summary, trace_stats: dict,
                  log_marks: dict, save_stats: dict, check: dict,
                  stdout: str, stderr: str) -> None:
    metrics["scenes"] = trace_stats["scenes"]
    metrics["phase_ticks"] = {
        "combat": trace_stats["phase_ticks"].get(0, 0),
        "shop": trace_stats["phase_ticks"].get(1, 0),
        "level_up": trace_stats["phase_ticks"].get(2, 0),
    }
    metrics["ticks"] = trace_stats["last_tick"] + 1
    metrics["waves_reached"] = trace_stats["max_wave"]
    if trace_stats["final_gold"] is not None:
        metrics["materials"] = trace_stats["final_gold"]
    metrics["level"] = trace_stats["max_level"]
    metrics["death_wave"] = trace_stats["first_death_wave"]
    metrics["check_ok"] = check.get("check_ok")
    if trace_stats["movement_rows"]:
        metrics["notes"].append(
            "move_landed=%d/%d in-wave (cleanup window: %d/%d game-inhibited, "
            "player.gd:808-810) hold_rows=%d"
            % (trace_stats["movement_applied_active"],
               trace_stats["movement_rows_active"],
               trace_stats["movement_applied_cleanup"],
               trace_stats["movement_rows_cleanup"], trace_stats["holds"]))
        if trace_stats["movement_unguarded_miss"]:
            metrics["notes"].append(
                "UNGUARDED movement misses=%d (real delivery failures; first: %s)"
                % (trace_stats["movement_unguarded_miss"],
                   json.dumps(trace_stats["unguarded_example"])))
        elif trace_stats["movement_inhibited"]:
            metrics["notes"].append(
                "all %d failed movement comparison(s) are game-inhibited "
                "(player_can_move 0 / cleaning_up 1 on the same row)"
                % trace_stats["movement_inhibited"])
        if trace_stats["verify_errors"]:
            metrics["notes"].append("rows carrying action_verify_error=%d"
                                    % trace_stats["verify_errors"])
        if trace_stats.get("movement_uncompared"):
            metrics["notes"].append(
                "%d movement row(s) were not compared by design "
                "(phase_transition_resync)" % trace_stats["movement_uncompared"])
    if check.get("mismatches"):
        metrics["notes"].append("input_mismatches=%d" % check["mismatches"])
    if trace_stats["incomplete"]:
        metrics["notes"].append("incomplete_obs_rows=%d" % trace_stats["incomplete"])
    if trace_stats.get("action_file_max") is not None:
        # rl_inject.gd increments action_file_index after consuming, and the row
        # records that index, so the consumed count IS the max index (1-based),
        # not max+1: a 1200-id stream over 1200 frames ends at index 1200.
        metrics["action_file_consumed"] = trace_stats["action_file_max"]
        metrics["notes"].append("action-file ids consumed=%d"
                                % metrics["action_file_consumed"])
    if trace_stats["action_sources"]:
        metrics["notes"].append("actions=" + ",".join(
            "%s:%d" % (k or "?", v) for k, v in
            sorted(trace_stats["action_sources"].items())))
    if save_stats:
        if isinstance(save_stats.get("enemies_killed"), int):
            metrics["kills"] = save_stats["enemies_killed"]
        if isinstance(save_stats.get("materials_collected"), int):
            metrics["notes"].append("materials_collected=%d"
                                    % save_stats["materials_collected"])

    # --- failure classification (bridge vs policy) ------------------------
    failure = ""
    summary_ok = None
    if isinstance(summary, dict):
        obs_report = summary.get("obs") or {}
        if isinstance(obs_report.get("obs_size"), int):
            metrics["obs_size"] = obs_report["obs_size"]
        failure = str(summary.get("failure") or "")
        summary_ok = bool(summary.get("ok"))
        metrics["reason"] = str(summary.get("reason") or "")
        prog = summary.get("progression") or {}
        metrics["shop_buys"] = int(prog.get("shop_buys", 0))
        metrics["shop_rerolls"] = int(prog.get("shop_rerolls", 0))
        metrics["shop_skips"] = int(prog.get("shop_skips", 0))
        metrics["shop_recycles"] = int(prog.get("shop_recycles", 0))
        metrics["shop_combines"] = int(prog.get("shop_combines", 0))
        metrics["shop_actions"] = int(prog.get("shop_actions", 0))
        metrics["level_ups"] = int(prog.get("level_ups_chosen", 0))

    if failure == "policy_inference_failed":
        metrics["status"] = "policy_failure"
        metrics["failure"] = failure
    elif failure:
        metrics["status"] = "bridge_failure"
        metrics["failure"] = "%s: %s" % (failure, summary.get("failure_message", ""))
    elif summary is None and check.get("watchdog"):
        # The engine was reaped by run_game.sh's wall-clock watchdog, so no
        # summary was written; the partial trace is still usable and this is a
        # wall-clock budget decision, not a bridge defect.
        metrics["status"] = "bridge_warn"
        metrics["failure"] = ("watchdog expired (raise --watchdog or lower "
                              "--max-seconds); no bridge summary")
    elif summary is None:
        metrics["status"] = "bridge_failure"
        metrics["failure"] = "no bridge summary written"
    elif not summary_ok:
        metrics["status"] = "bridge_failure"
        metrics["failure"] = "bridge summary ok=false (reason=%s)" % metrics["reason"]
    elif trace_stats["rows"] == 0:
        metrics["status"] = "bridge_failure"
        metrics["failure"] = "bridge trace has no rows"
    elif rc != 0 or trace_stats["incomplete"] or check.get("check_ok") is False:
        # The episode itself ran and its metrics are usable, but something on the
        # bridge side is degraded: a nonzero launcher rc (Godot 3.7-dev1 teardown
        # assertions are a known cause), incomplete observation rows, or the
        # trace gate failing.  Reported distinctly, not hidden, and still
        # aggregated -- see EVAL.md.
        reasons = []
        if rc != 0:
            reasons.append("run_game.sh rc=%d%s"
                           % (rc, (" (%s)" % check["trace_error"])
                              if check.get("trace_error") else ""))
        if trace_stats["incomplete"]:
            reasons.append("%d incomplete observation row(s)" % trace_stats["incomplete"])
        if check.get("check_ok") is False:
            reasons.append("trace gate failed: "
                           + "; ".join(check.get("check_reasons") or [])[:200])
        metrics["status"] = "bridge_warn"
        metrics["failure"] = " | ".join(reasons)

    # --- outcome ----------------------------------------------------------
    won_marker = bool(log_marks.get("won")) or save_stats.get("run_won") == 1
    lost_marker = bool(log_marks.get("lost"))
    if (metrics.get("endless") and trace_stats["max_wave"] > 20
            and metrics.get("start_wave", 0) <= 20):
        # In endless the run ends only by death, so "cleared wave 20" is exactly
        # "reached wave 21+"; the game logs no win marker for it.  A run that was
        # *started* past 20 (--start-wave) proves nothing about wave 20.
        metrics["won"] = True
    elif won_marker:
        metrics["won"] = True
    elif lost_marker:
        metrics["won"] = False
    elif (trace_stats["scenes"] and trace_stats["scenes"][-1] == "EndRun"
          and trace_stats["max_wave"] >= 20
          and metrics["status"] in ("ok", "bridge_warn")):
        metrics["won"] = True
    metrics["waves_cleared"] = (20 if metrics["won"] else
                               max(0, (metrics["death_wave"] or trace_stats["max_wave"]) - 1))
    if not metrics["won"] and metrics["death_wave"] is None:
        metrics["death_wave"] = trace_stats["max_wave"] if lost_marker else None

    # --- stalls -----------------------------------------------------------
    unexpected = [s for s in trace_stats["scenes"] if s not in KNOWN_SCENES]
    if unexpected:
        metrics["stall"] = "menu:" + ",".join(unexpected)
        metrics["no_progress"] = True
    elif trace_stats["max_shop_run"] >= STALL_TICKS:
        metrics["stall"] = "shop"
        metrics["no_progress"] = True
    elif trace_stats["max_levelup_run"] >= STALL_TICKS:
        metrics["stall"] = "level_up"
        metrics["no_progress"] = True
    elif (metrics["status"] in ("ok", "bridge_warn")
          and metrics["reason"] == "tick_budget"
          and not metrics["won"] and metrics["death_wave"] is None):
        metrics["stall"] = "no_progress"
        metrics["no_progress"] = True
    if log_marks.get("exists") is False:
        metrics["notes"].append("no game log.txt (engine never booted the run?)")
    if not save_stats:
        metrics["notes"].append("no game save stats (kills unavailable)")


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------
def fmt(value, width=6):
    if value is None:
        return "n/a".rjust(width)
    if isinstance(value, bool):
        return ("Y" if value else "n").rjust(width)
    if isinstance(value, float):
        return ("%.1f" % value).rjust(width)
    return str(value).rjust(width)


HEADER = (" seed | waves |  won | death |  mat  | lvl | kills |  buy |rr|skip|rc|lu "
          "|  ticks | stall        |  wall | status")


def print_table(title: str, rows: list) -> None:
    print()
    print("== %s ==" % title)
    print(HEADER)
    print("-" * len(HEADER))
    for r in rows:
        print(" %4s | %5s | %4s | %5s | %5s | %3s | %5s | %4s |%2s|%4s|%2s|%2s | %6s | %-12s | %5s | %s%s%s"
              % (fmt(r["seed"], 4), fmt(r["waves_reached"], 5), fmt(r["won"], 4),
                 fmt(r["death_wave"], 5), fmt(r["materials"], 5), fmt(r["level"], 3),
                 fmt(r["kills"], 5), fmt(r["shop_buys"], 4), fmt(r["shop_rerolls"], 2),
                 fmt(r["shop_skips"], 4), fmt(r["shop_recycles"], 2),
                 fmt(r["level_ups"], 2), fmt(r["ticks"], 6), (r["stall"] or "-")[:12],
                 fmt(r["wall_s"], 5),
                 r["status"] if r["status"] == "ok" else r["status"] + ":" + (r["failure"] or "")[:60],
                 (" <" + ",".join(r["flags"]) + ">" if r["flags"] else ""),
                 (" [" + ";".join(r["notes"]) + "]") if r["notes"] else ""))


def print_gap(game_rows: list, sim_rows: list) -> None:
    print()
    game_obs = {r.get("obs_size") for r in game_rows if r.get("obs_size")}
    sim_obs = {r.get("sim_obs_size") for r in sim_rows if r.get("sim_obs_size")}
    print("== sim vs game (same seeds, same policy weights) ==")
    if game_obs and sim_obs and game_obs != sim_obs:
        print("WARNING: the game bridge observed %s floats and the sim core %s - "
              "the two sides are NOT the same observation space, so the columns "
              "below compare policies that saw different state vectors "
              "(rebuild the pack from the current mod sources)"
              % (sorted(game_obs), sorted(sim_obs)))
    print(" seed | game_waves sim_waves | game_won sim_won | game_mat sim_mat |"
          " game_kills sim_kills | game_wall sim_wall | interventions")
    print(" (rows marked !smoke-only used a smoke intervention -- game "
          "invulnerability and sim stress HP are DIFFERENT interventions, so "
          "those pairs are not parity evidence)")
    print("-" * 120)
    sim_by_seed = {r["seed"]: r for r in sim_rows}
    if not any(r["status"] in ("ok", "bridge_warn") for r in sim_rows):
        print(" (no usable simulator rows - see the sim table's status column)")
        return
    for g in game_rows:
        s = sim_by_seed.get(g["seed"])
        if s is None:
            continue
        flags = sorted(set(g.get("flags") or []) | set(s.get("flags") or []))
        print(" %4s | %10s %9s | %8s %7s | %8s %7s | %10s %9s | %9s %8s | %s"
              % (fmt(g["seed"], 4), fmt(g["waves_reached"], 10), fmt(s["waves_reached"], 9),
                 fmt(g["won"], 8), fmt(s["won"], 7), fmt(g["materials"], 8),
                 fmt(s["materials"], 7), fmt(g["kills"], 10), fmt(s["kills"], 9),
                 fmt(g["wall_s"], 9), fmt(s["wall_s"], 8),
                 ("!smoke-only:" + ",".join(flags)) if flags else ""))


def aggregate(rows: list) -> dict:
    """Means over usable rows; interventions (invulnerability, stress HP,
    start-wave) are counted separately and excluded from the parity means."""
    usable = [r for r in rows if r["status"] in ("ok", "bridge_warn")]
    ok = [r for r in usable if not r.get("flags")]
    def mean(key, pool):
        vals = [r[key] for r in pool if isinstance(r[key], (int, float))]
        return round(sum(vals) / len(vals), 2) if vals else None
    return {
        "episodes": len(rows),
        "ok": len(usable),
        "unflagged": len(ok),
        "flagged": len(usable) - len(ok),
        "warn": sum(1 for r in rows if r["status"] == "bridge_warn"),
        "policy_failures": sum(1 for r in rows if r["status"] == "policy_failure"),
        "bridge_failures": sum(1 for r in rows if r["status"] == "bridge_failure"),
        "sim_failures": sum(1 for r in rows if r["status"] == "sim_failure"),
        "wins": sum(1 for r in ok if r["won"]),
        "stalls": sum(1 for r in ok if r["stall"]),
        "mean_waves_reached": mean("waves_reached", ok),
        "mean_materials": mean("materials", ok),
        "mean_level": mean("level", ok),
        "mean_kills": mean("kills", ok),
        "mean_shop_buys": mean("shop_buys", ok),
        "mean_wall_s": mean("wall_s", ok),
    }


def print_aggregate(title: str, agg: dict) -> None:
    print("%s aggregate: episodes=%s usable=%s (warn=%s, flagged=%s) wins=%s stalls=%s "
          "policy_fail=%s bridge_fail=%s sim_fail=%s | mean over unflagged rows: "
          "waves=%s materials=%s level=%s kills=%s buys=%s wall_s=%s"
          % (title, agg["episodes"], agg["ok"], agg["warn"], agg["flagged"],
             agg["wins"], agg["stalls"],
             agg["policy_failures"], agg["bridge_failures"], agg["sim_failures"],
             agg["mean_waves_reached"], agg["mean_materials"], agg["mean_level"],
             agg["mean_kills"], agg["mean_shop_buys"], agg["mean_wall_s"]))


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def contract_obs_size() -> int:
    """The observation width the bridge/game contract currently declares."""
    contract = read_json(HERE / "state_contract.json") or {}
    size = (contract.get("observation") or {}).get("size")
    return int(size) if isinstance(size, int) and size > 0 else 0


def parse_seeds(text: str) -> list:
    seeds = []
    for token in text.replace(" ", ",").split(","):
        if not token:
            continue
        if not token.lstrip("-").isdigit():
            raise EvalError("bad seed %r" % token)
        seeds.append(int(token))
    if not seeds:
        raise EvalError("--seeds is empty")
    return seeds


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Evaluate a Brotato policy in the real game and the simulator.")
    p.add_argument("--policy", type=Path, default=None,
                   help="exported artifact JSON (export_policy.py)")
    p.add_argument("--action-source", choices=("policy", "script", "file"),
                   default=None,
                   help="default: policy when --policy is given, else script. "
                        "'file' consumes --action-file one id per step on BOTH "
                        "sides (game BROTATO_RL_ACTION_FILE, sim stdin)")
    p.add_argument("--action-file", type=Path, default=None,
                   help="with --action-source file: one action id per line (the "
                        "mod reads the last integer of a line, so a '<tick> "
                        "<action>' stream works too)")
    p.add_argument("--action-mode", choices=("argmax", "sample"), default="sample",
                   help="how the policy turns its masked logits into an action on "
                        "BOTH sides: 'sample' (default) draws from the softmax "
                        "over the masked logits, one uniform per decision from a "
                        "dedicated PRNG seeded from the run seed (game "
                        "BROTATO_RL_ACTION_MODE, sim driver --action-mode), so a "
                        "greedy action that never leaves the shop can be escaped; "
                        "'argmax' is the historical greedy pick")
    p.add_argument("--seeds", default="",
                   help="comma list, e.g. 1,2,3,4 (not needed by --sim-bench)")
    p.add_argument("--parallel", type=int, default=4,
                   help="game episodes to run at once (default 4)")
    p.add_argument("--max-seconds", type=int, default=1200,
                   help="simulated seconds per episode (default 1200, > the 1050 s "
                        "20-wave campaign)")
    p.add_argument("--watchdog", type=int, default=None,
                   help="wall-clock seconds per game episode (default 4*max-seconds)")
    p.add_argument("--sample-every", type=int, default=6,
                   help="bridge trace sampling (1 = every tick; default 6 keeps a "
                        "full run's trace readable)")
    p.add_argument("--character", "--game-char", dest="game_char",
                   default="character_bull",
                   help="character id for both sides (default character_bull; "
                        "character_demon with --endless)")
    p.add_argument("--weapon", "--game-weapon", dest="game_weapon", default="",
                   help="starting weapon id for both sides (default: none; "
                        "weapon_ghost_scepter_1 with --endless)")
    p.add_argument("--danger", type=int, default=5)
    p.add_argument("--endless", action="store_true",
                   help="endless run on both sides (game BROTATO_ENDLESS=1, core "
                        "[env] endless): defaults --character/--weapon to Demon + "
                        "weapon_ghost_scepter_1, and the wave-20 stop is skipped")
    p.add_argument("--invulnerable", action="store_true",
                   help="both sides (game DebugService.invulnerable, core "
                        "BtState.invulnerable): the player Hurtbox is disabled, so no "
                        "enemy contact/projectile hit is registered.  Lets a smoke reach "
                        "deep waves; every row is flagged in its notes")
    p.add_argument("--start-wave", type=int, default=0,
                   help="start wave on BOTH sides (game DebugService.starting_wave, "
                        "core state.start_wave; 0 = the retail wave 1)")
    p.add_argument("--particles-rng", type=int, choices=(0, 1), default=0,
                   help="sim only: 0 (default) = the bridge's suppressed particle "
                        "RNG (the CPU bridge always suppresses), 1 = legacy draws")
    p.add_argument("--audio-rng", type=int, choices=(0, 1), default=0,
                   help="sim only: 0 (default) = the bridge's audiovisual "
                        "neutralization, 1 = the legacy source audio draws. A trace "
                        "recorded with the neutralized bridge needs 0.")
    p.add_argument("--sim-weapon", type=int, default=BT_WEAPON_NONE,
                   help="sim starting weapon id (BT_WEAPONS index; default none)")
    p.add_argument("--sim-forward", choices=("native", "python", "bot"), default="native",
                   help="policy forward for the sim: in-process native puffernet "
                        "(default) or policy_reference.py driven step by step")
    p.add_argument("--sim-print-every", type=int, default=60,
                   help="with --sim-forward native: print a data row every N steps "
                        "(default 60; metrics come from the driver's own counters)")
    p.add_argument("--sim-obs-every", type=int, default=0,
                   help="with --sim-forward native: include the observation every N "
                        "steps (default 0; needed only for cross-checks)")
    p.add_argument("--sim-crosscheck", dest="sim_crosscheck", action="store_true",
                   default=None,
                   help="run the first seed through both forwards at stride 1 and "
                        "compare their action sequences (default: on for a policy)")
    p.add_argument("--no-sim-crosscheck", dest="sim_crosscheck",
                   action="store_false")
    p.add_argument("--sim-bench", type=int, default=0,
                   help="run the sim inner loop N steps in one process and print "
                        "steps/s for the selected --sim-forward, then exit")
    p.add_argument("--bonus-hp", type=float, default=0.0,
                   help="sim only: starting Max HP grant (dgb stress mode). The "
                        "game has no equivalent, so a --bonus-hp row is flagged and "
                        "never treated as a parity scenario")
    p.add_argument("--bot-pivot", type=int, default=0,
                   help="--sim-forward bot: dgb weapon handover wave (0 = 30)")
    p.add_argument("--bot-stay", type=int, default=0,
                   help="--sim-forward bot: dgb lock/save wave (0 = 45)")
    p.add_argument("--sim-include", type=Path, default=None,
                   help="include root the sim driver compiles the core headers "
                        "from (default the live repo; point it at a pinned "
                        "snapshot to evaluate a frozen core)")
    p.add_argument("--run-root", type=Path, default=None,
                   help="eval root (default $HOME/brotato_eval)")
    p.add_argument("--tag-prefix", default="eval")
    p.add_argument("--sim-only", action="store_true")
    p.add_argument("--game-only", action="store_true")
    p.add_argument("--resim", action="store_true",
                   help="force a re-stage of the shared patched pack")
    p.add_argument("--reuse-pack", action="store_true",
                   help="reuse the existing patched pack without checking the "
                        "build inputs (escape hatch while the tree is mid-edit; "
                        "the pack may then predate the current bridge sources)")
    p.add_argument("--prune-traces", action="store_true",
                   help="delete trace files larger than --keep-trace-bytes after parsing")
    p.add_argument("--keep-trace-bytes", type=int, default=50 * 1024 * 1024)
    p.add_argument("--json", type=Path, default=None, help="write the full result JSON")
    p.add_argument("--quiet", action="store_true")
    return p


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    seeds = parse_seeds(args.seeds) if args.seeds else []
    if not args.sim_bench and not seeds:
        raise EvalError("--seeds is required (or use --sim-bench)")
    if args.action_source is None:
        args.action_source = "policy" if args.policy else "script"
    if args.action_source == "policy" and not args.policy:
        raise EvalError("--action-source policy needs --policy")
    if args.action_source == "file" and not args.action_file:
        raise EvalError("--action-source file needs --action-file")
    if args.action_file is not None:
        if not args.action_file.is_file():
            raise EvalError("action file not found: %s" % args.action_file)
        args.action_source = "file"
    if args.policy and not args.policy.is_file():
        raise EvalError("policy artifact not found: %s" % args.policy)
    args.run_root = args.run_root or (Path.home() / "brotato_eval")
    args.run_root.mkdir(parents=True, exist_ok=True)
    if args.endless or args.sim_forward == "bot":
        # High-Endless Demon baseline (and the scripted Demon bot's own profile):
        # Demon starts weapon_ghost_scepter_1.  An explicit --character/--weapon
        # still wins; --sim-forward bot on Bull would be a different policy than
        # dgb was written for.
        if args.game_char == "character_bull":
            args.game_char = ENDLESS_CHARACTER
        if not args.game_weapon:
            args.game_weapon = ENDLESS_WEAPON
    if args.game_char not in CHARACTERS:
        raise EvalError("unknown character %r (known: %s)"
                        % (args.game_char, ", ".join(CHARACTERS)))
    sim_weapon = args.sim_weapon
    if args.game_weapon and sim_weapon == BT_WEAPON_NONE:
        # Both sides must start from the same loadout, so resolve the same id
        # string to the core's BT_WEAPONS index through the canonical tables.
        sim_weapon, canonical = resolve_sim_weapon(args.run_root, args.game_weapon)
        if canonical != args.game_weapon:
            args.game_weapon = canonical
    if args.watchdog is None:
        args.watchdog = max(300, args.max_seconds * 4)
    init_action_gate()
    gate_notes = action_gate_notes()
    for note in gate_notes:
        print("eval: WARNING action gate: %s" % note, file=sys.stderr)
    if args.policy:
        artifact_obs = int((json.loads(args.policy.read_text()).get("layout") or {})
                           .get("obs_size", 0))
        contract = contract_obs_size()
        contract_actions = int((read_json(HERE / "state_contract.json") or {})
                               .get("action", {}).get("count", 0))
        artifact_actions = int((json.loads(args.policy.read_text()).get("layout") or {})
                               .get("actions", 0))
        if contract_actions and artifact_actions != contract_actions:
            print("eval: WARNING artifact actions=%d but the bridge contract declares "
                  "%d" % (artifact_actions, contract_actions), file=sys.stderr)
        if contract and artifact_obs != contract:
            print("eval: WARNING artifact obs_size=%d but the bridge contract (%s) "
                  "declares %d; the game side will reject the policy and the sim "
                  "will refuse to run it (re-export at the live schema)"
                  % (artifact_obs, "state_contract.json", contract), file=sys.stderr)
    if args.max_seconds * 60 > TOTAL_WAVE_SECONDS * 60:
        full = True
    else:
        full = False
    if not args.quiet:
        print("eval: repo      %s" % REPO)
        print("eval: action    %s%s | mode %s" % (args.action_source,
              (" (%s)" % args.policy) if args.policy else "", args.action_mode))
        print("eval: seeds     %s | parallel %d" % (seeds, args.parallel))
        print("eval: gate      %s" % action_gate_label())
        print("eval: game      char=%s danger=%d weapon=%s start_wave=%s "
              "max_seconds=%d watchdog=%d sample_every=%d full_run=%s endless=%s%s"
              % (args.game_char, args.danger, args.game_weapon or "<none>",
                 args.start_wave or "retail", args.max_seconds, args.watchdog,
                 args.sample_every, full, args.endless,
                 " invulnerable" if args.invulnerable else ""))
        if args.endless:
            print("eval: endless   sim weapon id=%s (both sides: %s)"
                  % (sim_weapon, args.game_weapon))

    result = {"seeds": seeds, "action_source": args.action_source,
              "action_mode": args.action_mode,
              "endless": bool(args.endless),
              "invulnerable": bool(args.invulnerable),
              "character": args.game_char, "weapon": args.game_weapon,
              "sim_weapon": sim_weapon,
              "action_file": (str(args.action_file.resolve())
                              if args.action_file else None),
              "action_file_ids": (sum(1 for line in
                                      args.action_file.read_text().splitlines()
                                      if line.strip() and
                                      not line.lstrip().startswith("#"))
                                  if args.action_file else 0),
              "particles_rng": args.particles_rng,
              "action_gate_effective": {str(k): sorted(v)
                                        for k, v in (_VALID_ACTIONS or {}).items()},
              "action_gate_contract": {str(k): sorted(v)
                                       for k, v in phase_valid_actions().items()},
              "action_gate_source": action_gate_label(),
              "action_gate_notes": gate_notes,
              "policy": str(args.policy) if args.policy else None,
              "max_seconds": args.max_seconds, "sample_every": args.sample_every,
              "character": args.game_char, "danger": args.danger}

    if args.sim_bench:
        # Schema/forward benchmark only: no game side, no episodes.
        binary = None
        try:
            binary = build_sim_driver(args.run_root, args.quiet, args.sim_include)
        except EvalError as exc:
            raise EvalError(str(exc))
        artifact = json.loads(args.policy.read_text()) if args.policy else None
        weights = dims = None
        if artifact is not None and args.sim_forward == "native":
            weights, dims = write_policy_weights(artifact, args.run_root)
        bot = ((args.bot_pivot, args.bot_stay)
               if args.sim_forward == "bot" else None)
        bench = run_sim_bench(binary, args, weights, dims, bot,
                              CHARACTERS[args.game_char], sim_weapon)
        print("eval: sim bench %s: %s steps in %.3fs cpu -> %.0f steps/s"
              % (bench.get("forward"), bench.get("bench"), bench.get("cpu_seconds", 0.0),
                 bench.get("steps_per_second", 0.0)))
        if args.json:
            args.json.write_text(json.dumps({"bench": bench}, indent=1))
        return 0

    game_rows, sim_rows = [], []
    stage = None

    stage_error = ""
    if not args.sim_only:
        try:
            stage = stage_shared(args.run_root, args.resim, args.quiet,
                                 args.reuse_pack)
        except EvalError as exc:
            stage_error = str(exc)
            stage = None
            if not args.quiet:
                print("eval: GAME SIDE UNAVAILABLE: %s" % stage_error)
        tags = ["%s%d" % (args.tag_prefix, s) for s in seeds]
        if len(set(tags)) != len(tags):
            raise EvalError("tag collision: %s" % tags)

        launcher, launcher_hash = snapshot_launcher(args.run_root)
        result["launcher"] = {"path": str(launcher), "sha256": launcher_hash}
        if not args.quiet:
            print("eval: launcher  %s (%s)" % (launcher, launcher_hash[:12]))

        def job(seed_tag):
            seed, tag = seed_tag
            if stage is None:
                row = new_metrics(seed, "game")
                row["status"] = "bridge_failure"
                row["failure"] = "staging/build failed: %s" % stage_error[:300]
                return row
            try:
                return run_game_episode(args, tag, seed, stage, launcher)
            except Exception as exc:      # noqa: BLE001 - reported per episode
                row = new_metrics(seed, "game")
                row["status"] = "bridge_failure"
                row["failure"] = "%s: %s" % (type(exc).__name__, exc)
                return row

        with concurrent.futures.ThreadPoolExecutor(max_workers=args.parallel) as pool:
            for row in pool.map(job, list(zip(seeds, tags))):
                game_rows.append(row)
                if not args.quiet:
                    print("eval: seed %s done (%.1fs) %s"
                          % (row["seed"], row["wall_s"], row["status"]))
        game_rows.sort(key=lambda r: r["seed"])

    if not args.game_only:
        artifact = json.loads(args.policy.read_text()) if args.policy else None
        sim_error = ""
        binary = None
        try:
            binary = build_sim_driver(args.run_root, args.quiet, args.sim_include)
        except EvalError as exc:
            # The shared core can be transiently broken by another agent's
            # in-flight edit; that must not throw away the game results.
            sim_error = str(exc)
            if not args.quiet:
                print("eval: SIM UNAVAILABLE: %s" % sim_error)
        reference = load_reference() if (artifact and binary) else None
        action_stream = None
        if args.action_source == "file" and args.action_file is not None:
            action_stream = [int(line.split()[-1]) for line in
                             args.action_file.read_text().splitlines()
                             if line.strip() and not line.lstrip().startswith("#")]
            if not args.quiet:
                print("eval: action file %s (%d ids; consumed one per step on both "
                      "sides)" % (args.action_file, len(action_stream)))
        weights = dims = None
        mask_spec, mask_selectors, mask_phase_field, masked_logit = \
            artifact_mask_spec(artifact)
        mask_conditionals = artifact_mask_conditionals(artifact)
        mask_rules = artifact_mask_rules(artifact)
        if artifact is not None and mask_spec is None and not args.quiet:
            print("eval: WARNING artifact declares no action_mask rules; the game "
                  "applies no mask and the sim matches that (the core's own "
                  "bt_action_mask is still recorded per step)")
        elif mask_spec and not args.quiet:
            print("eval: sim policy mask clauses=%s selectors=%s phase_field=%s "
                  "masked_logit=%g (artifact metadata, evaluated per observation "
                  "exactly like rl_policy.gd)" % (mask_spec, mask_selectors or "-",
                                                  mask_phase_field, masked_logit))
        if args.sim_forward == "bot" and not args.quiet:
            print("eval: sim forward bot (dgb pivot=%d stay=%d)"
                  % (args.bot_pivot or 30, args.bot_stay or 45))
        if artifact is not None and args.sim_forward == "native" and binary is not None:
            weights, dims = write_policy_weights(artifact, args.run_root)
            if not args.quiet:
                print("eval: sim weights %s (obs=%d hidden=%d layers=%d actions=%d) "
                      "gate=%s" % (weights.name, *dims, gate_spec()))
        for index, seed in enumerate(seeds):
            row = new_metrics(seed, "sim")
            row["endless"] = bool(args.endless)
            if binary is None:
                row["status"] = "sim_failure"
                row["failure"] = "sim driver unavailable: %s" % sim_error[:200]
                sim_rows.append(row)
                continue
            script = ScriptPolicy()
            policy = PhasePolicy(artifact, reference, script, args.action_mode) if artifact else None
            if policy is not None and args.action_source == "script":
                policy = None
            cross = (args.sim_crosscheck is not False and index == 0
                     and policy is not None and weights is not None
                     and args.sim_forward == "native")
            bot = ((args.bot_pivot, args.bot_stay)
                   if args.sim_forward == "bot" else None)
            budget = args.max_seconds * 60
            # When the harness drives the driver (script source, or the
            # reference forward), the driver answers one row per action: the row
            # stride must be 1 or the two sides deadlock.  Wnen the *driver* owns
            # the action (native forward) it can stride freely.
            stride = args.sim_print_every
            every = args.sim_obs_every
            if weights is None and bot is None:
                stride = 1
                if policy is not None:
                    every = 1
            if cross:
                # The cross-check needs the primary run's own observations and the
                # action applied at every step.
                stride, every = 1, 1
            row = run_sim_episode(
                binary, seed, CHARACTERS[args.game_char], sim_weapon, 0, 1,
                policy, script, budget, args.watchdog, endless=args.endless,
                weights=weights, dims=dims, gate_spec=gate_spec(),
                print_every=stride, obs_every=every, bot=bot,
                bonus_hp=args.bonus_hp, mask_spec=mask_spec,
                mask_conditionals=mask_conditionals,
                masked_logit=masked_logit, mask_rules=mask_rules,
                mask_selectors=mask_selectors, mask_phase_field=mask_phase_field,
                audio_rng=args.audio_rng, start_wave=args.start_wave,
                particles_rng=args.particles_rng, invulnerable=int(args.invulnerable),
                action_stream=action_stream, action_mode=args.action_mode)

            if args.sim_include:
                row["notes"].append("core include %s" % args.sim_include)
            if cross:
                # Cross-check the native forward against policy_reference.py on
                # *identical* inputs: the primary run recorded every observation
                # (stride 1), so the reference forward is replayed over that same
                # sequence and the native forward is re-run on it in one pass.
                # A closed-loop comparison of two runs says nothing after the
                # first action flip; this one is exact and feed-forward.
                reference_module = load_reference()
                ref = reference_module.Reference(artifact)
                ref.reset()
                obs_rows = row.get("obs_head") or []
                mask_rows = row.get("mask_head") or []
                native = native_forward_sequence(binary, weights, dims, obs_rows,
                                                 mask_rows, masked_logit,
                                                 tag=str(seed))
                span = min(len(obs_rows), len(native))
                worst = 0.0
                first_bad = None
                for i in range(span):
                    bits = mask_rows[i] if i < len(mask_rows) else 0
                    mask_bytes = [1 if (bits >> a) & 1 else 0
                                  for a in range(artifact["layout"]["actions"])]
                    expected = ref.step(obs_rows[i], mask_bytes)
                    # Compare like with like: the reference reports the raw
                    # logits in "logits" and the masked ones in "masked_logits",
                    # and the native row carries the masked values it selected on.
                    worst = max(worst, max(abs(a - b) for a, b in
                                           zip(native[i]["logits"],
                                               expected["masked_logits"])))
                    if native[i]["a"] != expected["action"] and first_bad is None:
                        gap = sorted(expected["logits"])[-1] - sorted(expected["logits"])[-2]
                        first_bad = {"step": i, "native": native[i]["a"],
                                     "reference": expected["action"],
                                     "logit_gap": round(gap, 9)}
                # Phase-specific forward checks on the same obs rows: the crate
                # mask (only take/recycle legal) and the level-up/shop masks are
                # exercised even when the recorded window happens to be all
                # combat, so every case in the contract is covered explicitly.
                actions_n = int(artifact["layout"]["actions"])
                phase_masks = {
                    "combat_0-8": [1 if a <= 8 else 0 for a in range(actions_n)],
                    "shop_0-4_8-28": [1 if (a <= 4 or 8 <= a <= 28) else 0
                                      for a in range(actions_n)],
                    "level_up_0-4": [1 if a <= 4 else 0 for a in range(actions_n)],
                    "crate_29-30": [1 if a >= 29 else 0 for a in range(actions_n)],
                }
                probe_rows = obs_rows[:8]
                mask_checks = {}
                for label, mask in phase_masks.items():
                    native_rows = native_forward_sequence(
                        binary, weights, dims, probe_rows,
                        [sum(1 << a for a, bit in enumerate(mask) if bit)] * len(probe_rows),
                        masked_logit, tag="%s_%s" % (seed, label))
                    ref2 = reference_module.Reference(artifact)
                    ref2.reset()
                    same = all(native_rows[i]["a"] ==
                               ref2.step(probe_rows[i], mask)["action"]
                               for i in range(min(len(probe_rows), len(native_rows))))
                    picks = sorted({r["a"] for r in native_rows})
                    mask_checks[label] = {"actions_agree": same,
                                          "native_picks": picks}
                row["crosscheck"] = {"steps": span, "max_logit_delta": worst,
                                     "first_mismatch": first_bad,
                                     "phase_masks": mask_checks}
                row["notes"].append(
                    "phase-mask forward checks: " + ", ".join(
                        "%s(%s%s)" % (k, "ok" if v["actions_agree"] else "MISMATCH",
                                      ",".join(map(str, v["native_picks"])))
                        for k, v in mask_checks.items()))
                row["notes"].append(
                    "crosscheck native vs policy_reference: %d/%d actions identical, "
                    "max |logit delta| %.3g" % (span - (1 if first_bad else 0), span,
                                                worst))
                if first_bad:
                    print("eval: WARNING native forward and policy_reference.py pick "
                          "different actions at step %d (native %s, reference %s, "
                          "top-2 logit gap %.3g)"
                          % (first_bad["step"], first_bad["native"],
                             first_bad["reference"], first_bad["logit_gap"]),
                          file=sys.stderr)
                row.pop("obs_head", None)
                row.pop("actions_head", None)
                row.pop("mask_head", None)
            sim_rows.append(row)

    if game_rows:
        print_table("real game (bridge)", game_rows)
        print_aggregate("game", aggregate(game_rows))
    if sim_rows:
        print_table("simulator (shared CPU core)", sim_rows)
        print_aggregate("sim", aggregate(sim_rows))
    if game_rows and sim_rows:
        print_gap(game_rows, sim_rows)

    result["game"] = game_rows
    result["sim"] = sim_rows
    if game_rows:
        result["game_aggregate"] = aggregate(game_rows)
    if sim_rows:
        result["sim_aggregate"] = aggregate(sim_rows)
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(result, indent=1))

    bad = [r for r in (game_rows + sim_rows)
           if r["status"] not in ("ok", "bridge_warn")]
    return 1 if bad else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except EvalError as exc:
        print("eval_game: %s" % exc, file=sys.stderr)
        raise SystemExit(2)
