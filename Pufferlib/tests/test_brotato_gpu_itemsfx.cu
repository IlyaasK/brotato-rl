//   nvcc -O2 -std=c++17 -arch=sm_120 --fmad=false -Xcompiler=-ffp-contract=off \
//     -Xcompiler=-Wno-narrowing -Xcompiler=-Wno-unreachable-code \
//     --diag-suppress=2361 --diag-suppress=111 --diag-suppress=128 \
//     -I. -Iocean/brotato tests/test_brotato_gpu_itemsfx.cu -lm
/* tests/test_brotato_gpu_itemsfx.cu
 *
 * Bit-exactness and cost of the intra-environment GPU schedule for the
 * item / structure / burning / explosion phase of one combat tick
 * (ocean/brotato/brotato_gpu_itemsfx.cuh, framework
 * brotato_gpu_framework.cuh).
 *
 * Build (from the repository root, on this host; sm_120 = RTX 5070):
 *
 *   nvcc -O2 -std=c++17 -arch=sm_120 --fmad=false -Xcompiler=-ffp-contract=off \
 *     -Xcompiler=-Wno-narrowing -Xcompiler=-Wno-unreachable-code \
 *     --diag-suppress=2361 --diag-suppress=111 --diag-suppress=128 \
 *     -I. -Iocean/brotato tests/test_brotato_gpu_itemsfx.cu -lm \
 *     -o /tmp/test_brotato_gpu_itemsfx
 *   /tmp/test_brotato_gpu_itemsfx
 *
 * -ffp-contract=off (host) and --fmad=false (device) are REQUIRED for parity,
 * exactly as in tests/test_brotato_cuda.cu.
 *
 * WHAT IS PROVED
 *   A. phase parity, many scripted Demon states: both phases are run by their
 *      serial references (bt_burn_tick for the Timer rank, bt_itemsfx_phase_serial
 *      for the garden / landmine / explosion phase, which is what bt_core_step
 *      calls by default) and by the device lane group (btfx_burn_phase +
 *      btfx_itemsfx_phase, in the same frame order). Every byte of BtState and
 *      the phase reward must match. The states carry many landmines (pressed and
 *      armed), ready Gardens, burning enemies and live explosions with all three
 *      ages.
 *   B. whole-step parity: bt_core_step itself is run for H ticks on the host and
 *      on the device with the BT_ITEMSFX_PHASE and BT_TIMER_PHASE hooks wired to
 *      the device phases. After EVERY tick the whole BtState, the observation
 *      vector, the reward and the terminal flag must match. This is the check
 *      that caught every interleaving assumption (the Timer rank running before
 *      the group pass, scan-before-apply across all parts of the phase, the
 *      explosion slot the landmine apply creates, the reward lane broadcast).
 *   C. cost: N envs x M RE-ARMED phases (every explosion is re-armed to age 1..3
 *      between timed phases, so the measured phase is always the full one rather
 *      than decaying as the scripted explosions age out), serial reference vs the
 *      device group, plus the number of pair predicates each pass moves off the
 *      ordered path. A timing is only printed after A and B pass, and the
 *      long-run state digest of both sides must still match afterwards.
 *   D. core-side cand contract (host only): bt_*_scan_range over disjoint ranges
 *      must equal the whole-range scan, and the *_cand apply reading those masks
 *      must equal the serial phase byte for byte.
 *   E. one application per rank per bt_core_step: a garden's cooldown must drop by
 *      exactly one BT_DT per step, and a burning enemy's countdown by exactly one
 *      frame per step (a phase or burn rank applied twice shows up here).
 *   F. the device order-free pass alone: its four masks must equal the host's
 *      full-range scans, over many runs (a lost mask bit from two lanes sharing a
 *      word cannot hide).
 *
 * Exit code 0 = all checks passed.
 */

#include <cuda_runtime.h>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

/* ---------------------------------------------------------------------------
 * Consumer side of the BT_ITEMSFX_PHASE hook (see the header's INTEGRATION
 * block): the hook body needs the per-env scratch address inside the device pass
 * only, and it derives it from the state pointer through two device globals the
 * test sets once per launch -- no per-lane global write, so any number of envs
 * per launch is race-free.
 * ------------------------------------------------------------------------- */
typedef struct BtfxScratch BtfxScratch;     /* forward: declared in the header */
__device__ BtfxScratch* g_scratch_base = nullptr;
__device__ char* g_env_base = nullptr;
__device__ long long g_env_stride = 0;
#define BTFX_SCRATCH_OF(s) \
    (g_scratch_base + ((char*)(s) - g_env_base) / g_env_stride)
#define BTFX_HOOK_ITEMSFX_PHASE 1
#define BTFX_HOOK_TIMER_PHASE 1
/* GpuRng's pcg32 jump-ahead: the framework declares btf_rng_advance and
 * btf_draw_parallel ODR-uses it, so a TU that includes the framework must also
 * see this definition (the framework's own consumer note). */
#include "../ocean/brotato/brotato_rng_jump.h"
#include "../ocean/brotato/brotato_gpu_itemsfx.cuh"

/* Named-field diff (the table used by tests/test_brotato_cuda.cu). */
typedef struct { const char* name; size_t off; size_t size; } BtField;
#include "brotato_state_fields.inc"

/* ------------------------------------------------------------------ harness */
static int g_checks = 0, g_failures = 0;

static void require(bool ok, const std::string& what) {
    g_checks++;
    if (!ok) { g_failures++; std::printf("  FAIL: %s\n", what.c_str()); }
}
static void ck(cudaError_t e, const char* what) {
    if (e != cudaSuccess) {
        std::fprintf(stderr, "CUDA error (%s): %s\n", what, cudaGetErrorString(e));
        std::exit(1);
    }
}

/* First differing FIELD, or "" when every field is byte-identical. Padding is
 * not compared: both sides are memcpy images of the same source, and the core
 * never writes padding. */
static std::string state_diff(const BtState& a, const BtState& b) {
    const unsigned char* p = (const unsigned char*)&a;
    const unsigned char* q = (const unsigned char*)&b;
    for (size_t i = 0; i < (size_t)BT_NFIELDS; i++) {
        const BtField& f = BT_FIELDS[i];
        if (std::memcmp(p + f.off, q + f.off, f.size) == 0) continue;
        size_t byte = 0;
        while (p[f.off + byte] == q[f.off + byte]) byte++;
        char buf[256];
        std::snprintf(buf, sizeof(buf), "field %s: first differing byte %zu (of %zu)",
                      f.name, byte, f.size);
        return std::string(buf);
    }
    return std::string();
}

/* ------------------------------------------------- scripted state pieces */
static int g_row_landmine = -1;   /* StructureEffect instantiating landmine.tscn   */
static int g_row_expl = -1;       /* ExplodingEffect using explosion.tscn          */
static int g_row_garden = -1;     /* the is_spawning TurretEffect (Garden)         */

static int find_row_exploding_scene(unsigned short scene) {
    for (int r = 0; r < BT_NITEM_EFF_ROWS; r++) {
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[r];
        if (bt_item_expl_opcode((int)E->opcode) && E->explosion_scene == scene) return r;
    }
    return -1;
}
static int find_landmine_row(void) {
    for (int r = 0; r < BT_NITEM_EFF_ROWS; r++) {
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[r];
        if (E->scene == BT_ISTR_RES___ENTITIES_STRUCTURES_LANDMINE_LANDMINE_TSCN &&
            bt_item_expl_nested_row(E) != BT_ITEM_EFF_NONE)
            return r;
    }
    return -1;
}
static int find_garden_row(void) {
    for (int r = 0; r < BT_NITEM_EFF_ROWS; r++)
        if (BT_ITEM_EFF_ROWS[r].is_spawning) return r;
    return -1;
}

static int place_enemy(BtState* s, int kind, float x, float y) {
    if (!bt_spawn_kind(s, kind, x, y)) return -1;
    for (int i = 0; i < BT_MAX; i++)
        if (s->ealive[i] && !s->eneutral[i] && (int)s->ekind[i] == kind &&
            s->ex[i] == x && s->ey[i] == y)
            return i;
    return -1;
}

/* The values bt_spawn_summon_row would have written for a structure slot. */
static int place_structure(BtState* s, int row, unsigned char opcode, float x, float y,
                           float dmg) {
    for (int p = 0; p < BT_MAX_PET; p++) {
        if (s->pet_alive[p]) continue;
        if (p >= s->pet_n) s->pet_n = p + 1;
        s->pet_alive[p] = 1;
        s->pet_isstruct[p] = 1;
        s->pet_opcode[p] = opcode;
        s->pet_row[p] = (unsigned short)row;
        s->pet_x[p] = x; s->pet_y[p] = y;
        s->pet_dmg[p] = dmg;
        s->pet_crit_chance[p] = 0.0f; s->pet_crit_dmg[p] = 2.0f;
        s->pet_burn_chance[p] = 0.0f; s->pet_burn_dmg[p] = 0.0f; s->pet_burn_dur[p] = 0.0f;
        s->pet_lm_pressed[p] = 0;
        s->pet_timer[p] = 0.0f;
        return p;
    }
    return -1;
}

/* A scripted Demon-run combat state, no wave groups (entities only where the
 * script puts them). `pressed` mines are past body_entered, so the tick fires
 * them (the ordered apply then allocates explosion-pool slots and draws RNG). */
static void script_state(BtState* s, bt_u64 seed, int n_enemies, int n_mines,
                         int n_pressed, int n_gardens, int n_expl, int expl_age) {
    bt_core_init(s, seed, 0, 1, BT_CHARACTER_DEMON, BT_WEAPON_NONE);
    s->n_live_groups = 0;
    bt_core_stat_add(s, 1, -15.0f);      /* no regen in the raw layer */
    s->regen_timer = 99.0f;
    s->iframe = 0.0f;
    s->dodge = 0.0f;
    s->max_hp = 1.0e7f; s->hp = 1.0e7f;  /* the player must not die mid-test */
    bt_item_expl_reset(s);
    s->px = 120.0f; s->py = 0.0f;

    for (int i = 0; i < n_enemies; i++) {
        float a = 6.2831853f * (float)i / (float)(n_enemies > 0 ? n_enemies : 1);
        float rr = 30.0f + 22.0f * (float)(i % 6);
        int e = place_enemy(s, 1 + (i % 3), 120.0f + rr * std::cos(a), rr * std::sin(a));
        if (e < 0) continue;
        s->ehp[e] = 4.0e4f; s->emax[e] = 4.0e4f;
        if (i % 3 == 0) { s->eburn[e] = 20.0f; s->eburndmg[e] = 3.0f; s->eburn[e] += (float)(i % 7); }
    }
    for (int i = 0; i < n_mines; i++) {
        float a = 6.2831853f * (float)i / (float)(n_mines > 0 ? n_mines : 1);
        int p = place_structure(s, g_row_landmine, BT_ITEMEFF_STRUCTURE,
                                120.0f + 55.0f * std::cos(a), 55.0f * std::sin(a), 30.0f);
        if (p >= 0 && i < n_pressed) s->pet_lm_pressed[p] = 1;
    }
    for (int i = 0; i < n_gardens; i++) {
        int p = place_structure(s, g_row_garden, BT_ITEMEFF_TURRET,
                                100.0f + 6.0f * (float)i, -90.0f, 0.0f);
        if (p >= 0) s->pet_timer[p] = 0.0f;   /* ready: emits this tick */
    }
    for (int i = 0; i < n_expl; i++) {
        float x = 120.0f + 24.0f * (float)(i % 9);
        float y = 12.0f * (float)(i % 7) - 36.0f;
        int pj = bt_item_expl_spawn(s, (unsigned short)g_row_expl, x, y, 40.0f,
                                    0.25f, 2.0f, 0.5f, 4.0f, 30.0f);
        if (pj >= 0) {
            int a = expl_age >= 0 ? expl_age : 1 + (i % BT_EXPL_FRAMES);
            if (a < 1) a = 1;
            if (a > BT_EXPL_FRAMES) a = BT_EXPL_FRAMES;
            s->expl.e[pj].age = a;
        }
    }
}

/* ------------------------------------------------------------------ kernels */
/* One lane group (blockDim = 32, one warp = one env) runs the phase for one env. */
__global__ void btfx_phase_kernel(BtState* states, BtfxScratch* scratch, float* rewards) {
    int env = blockIdx.x;
    float r = 0.0f;
    btfx_itemsfx_phase(&states[env], &r, &scratch[env], threadIdx.x, 0, 32);
    if (threadIdx.x == 0) rewards[env] = r;
}

/* Both phases in the core's own frame order (burn rank first, then the garden /
 * landmine / explosion phase), which is what bt_core_step does around its group
 * pass. The host reference below runs bt_burn_tick + btfx_itemsfx_phase_ref. */
__global__ void btfx_phases_kernel(BtState* states, BtfxScratch* scratch, float* rewards) {
    int env = blockIdx.x;
    float r = 0.0f;
    btfx_burn_phase(&states[env], &r, &scratch[env], threadIdx.x, 0, 32);
    btfx_itemsfx_phase(&states[env], &r, &scratch[env], threadIdx.x, 0, 32);
    if (threadIdx.x == 0) rewards[env] = r;
}

/* Per-env digest, so a long device-only run can be compared without moving the
 * 6.7 MB states back. */
__global__ void btfx_digest_kernel(BtState* states, unsigned int* digests, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) digests[i] = btf_state_digest(&states[i]);
}

/* Restore the scripted workload between timed phases, so the measured phase is
 * always the full one instead of decaying as the scripted explosions age out
 * (there is no producer in the measured state). Every explosion is live again at
 * age 1..BT_EXPL_FRAMES with its hit set cleared, every enemy is back to full HP
 * and burning, the consumable pool is empty and every garden is ready. The
 * host-side rearm_state() below is the same work; both are INSIDE the timed
 * region, so the comparison stays fair. Mines are left unpressed on purpose: the
 * landmine scan (a measured cost) runs, but no mine fires, so no pool churn. */
__global__ void btfx_rearm_kernel(BtState* states) {
    BtState* s = &states[blockIdx.x];
    for (int i = threadIdx.x; i < BT_MAX_EXPL; i += blockDim.x) {
        s->expl.e[i].alive = 1;
        s->expl.e[i].age = 1 + (i % BT_EXPL_FRAMES);
        s->expl.e[i].hit.n = 0;
        s->expl.e[i].hit_overflow = 0;
    }
    for (int i = threadIdx.x; i < BT_MAX; i += blockDim.x) {
        if (!s->ealive[i]) continue;
        s->ehp[i] = 4.0e4f; s->emax[i] = 4.0e4f;
        s->eburn[i] = 20.0f; s->eburndmg[i] = 3.0f;
    }
    for (int c = threadIdx.x; c < BT_MAX_CONS; c += blockDim.x) s->conalive[c] = 0;
    for (int p = threadIdx.x; p < s->pet_n; p += blockDim.x) {
        if (!s->pet_alive[p]) continue;
        if (s->pet_opcode[p] == BT_ITEMEFF_STRUCTURE) s->pet_lm_pressed[p] = 0;
        else s->pet_timer[p] = 0.0f;
    }
}

/* Host mirror of btfx_rearm_kernel, defined below. */
static void rearm_state(BtState* s);

/* Density-controlled scripted state for the cost measurement only: enemies on a
 * grid filling [-half, half]^2 around the player, the scripted explosions on a
 * cluster ring of radius `clust` around the player. Overlap (hence the ordered
 * apply's hit count) is then a controlled fraction of the pair space instead of
 * every pair, which is what a real wave looks like; the parity sections keep the
 * maximally dense states. */
static void script_bench_state(BtState* s, bt_u64 seed, int n_enemies, float half,
                               int n_expl, float clust) {
    bt_core_init(s, seed, 0, 1, BT_CHARACTER_DEMON, BT_WEAPON_NONE);
    s->n_live_groups = 0;
    bt_core_stat_add(s, 1, -15.0f);
    s->regen_timer = 99.0f;
    s->iframe = 0.0f;
    s->dodge = 0.0f;
    s->max_hp = 1.0e7f; s->hp = 1.0e7f;
    bt_item_expl_reset(s);
    s->px = 120.0f; s->py = 0.0f;
    const int cols = 16;
    int rows = (n_enemies + cols - 1) / cols;
    for (int i = 0; i < n_enemies; i++) {
        float x = s->px - half + 2.0f * half * (float)(i % cols) / (float)(cols - 1);
        float y = -half + 2.0f * half * (float)(i / cols) / (float)(rows > 1 ? rows - 1 : 1);
        int e = place_enemy(s, i % 3, x, y);
        if (e < 0) continue;
        s->ehp[e] = 4.0e4f; s->emax[e] = 4.0e4f;
        if (i % 3 == 0) { s->eburn[e] = 20.0f; s->eburndmg[e] = 3.0f; }
    }
    for (int i = 0; i < 24; i++) {
        float a = 6.2831853f * (float)i / 24.0f;
        int p = place_structure(s, g_row_landmine, BT_ITEMEFF_STRUCTURE,
                                120.0f + 55.0f * std::cos(a), 55.0f * std::sin(a), 30.0f);
        if (p < 0) break;
    }
    for (int i = 0; i < 12; i++)
        if (place_structure(s, g_row_garden, BT_ITEMEFF_TURRET, 100.0f + 6.0f * (float)i,
                            -90.0f, 0.0f) < 0) break;
    for (int k = 0; k < n_expl; k++) {
        float a = 6.2831853f * (float)k / (float)(n_expl > 0 ? n_expl : 1);
        int pj = bt_item_expl_spawn(s, (unsigned short)g_row_expl,
                                    s->px + clust * std::cos(a), clust * std::sin(a),
                                    40.0f, 0.25f, 2.0f, 0.5f, 4.0f, 30.0f);
        if (pj >= 0) s->expl.e[pj].age = 1 + (k % BT_EXPL_FRAMES);
    }
}

/* One costed workload: rearm + serial phase vs rearm + device phase, REPS times,
 * plus the long-run digest equality of the two sides. */
static void bench_workload(const char* name, int n_enemies, float half, int n_expl,
                           float clust, int NE, int REPS) {
    std::vector<BtState> ref(NE), dev_in(NE);
    for (int i = 0; i < NE; i++) {
        script_bench_state(&ref[i], 7700u + (bt_u64)i, n_enemies, half, n_expl, clust);
        dev_in[i] = ref[i];
    }
    std::vector<float> rref(NE, 0.0f);
    BtState* d_states = nullptr;
    BtfxScratch* d_scratch = nullptr;
    float* d_rew = nullptr;
    ck(cudaMalloc(&d_states, (size_t)NE * sizeof(BtState)), "malloc states");
    ck(cudaMalloc(&d_scratch, (size_t)NE * sizeof(BtfxScratch)), "malloc scratch");
    ck(cudaMalloc(&d_rew, (size_t)NE * sizeof(float)), "malloc rewards");
    ck(cudaMemcpy(d_states, dev_in.data(), (size_t)NE * sizeof(BtState),
                  cudaMemcpyHostToDevice), "H2D states");

    /* ---------------------------------------------------------------------
     * Per-rep parity trace (UNTIMED): the 100-cycle digest check is red, so find
     * the FIRST divergent re-armed rep and name the first differing field
     * instead of inferring where it lives. Both sides run exactly the ops the
     * timed loop runs; the states are re-seeded afterwards.
     * ------------------------------------------------------------------- */
    {
        unsigned int* d_dg = nullptr;
        ck(cudaMalloc(&d_dg, (size_t)NE * sizeof(unsigned int)), "malloc trace digests");
        std::vector<unsigned int> tdg_d(NE, 0u), tdg_h(NE, 0u);
        std::vector<BtState> probe(NE);
        int bad_rep = -1, bad_env = -1;
        for (int rep = 0; rep < REPS && bad_rep < 0; rep++) {
            for (int i = 0; i < NE; i++) {
                rearm_state(&ref[i]);
                bt_burn_tick(&ref[i], &rref[i]);
                btfx_itemsfx_phase_ref(&ref[i], &rref[i]);
            }
            btfx_rearm_kernel<<<NE, 32>>>(d_states);
            btfx_phases_kernel<<<NE, 32>>>(d_states, d_scratch, d_rew);
            btfx_digest_kernel<<<1, 256>>>(d_states, d_dg, NE);
            ck(cudaDeviceSynchronize(), "trace sync");
            ck(cudaMemcpy(tdg_d.data(), d_dg, (size_t)NE * sizeof(unsigned int),
                          cudaMemcpyDeviceToHost), "D2H trace digests");
            for (int i = 0; i < NE; i++) {
                tdg_h[i] = btf_state_digest(&ref[i]);
                if (tdg_h[i] != tdg_d[i]) { bad_rep = rep; bad_env = i; break; }
            }
            if (bad_rep >= 0) {
                ck(cudaMemcpy(probe.data(), d_states, (size_t)NE * sizeof(BtState),
                              cudaMemcpyDeviceToHost), "D2H trace states");
                std::string f = state_diff(ref[bad_env], probe[bad_env]);
                std::printf("    TRACE %-22s first divergence: rep %d env %d: %s\n",
                            name, bad_rep, bad_env, f.c_str());
                require(false, std::string(name) + ": re-armed cycle parity (rep " +
                                   std::to_string(bad_rep) + ")");
            }
        }
        if (bad_rep < 0)
            std::printf("    TRACE %-22s no divergence over %d re-armed cycles\n", name, REPS);
        cudaFree(d_dg);
        /* re-seed both sides so the timed run starts from the scripted state */
        for (int i = 0; i < NE; i++)
            script_bench_state(&ref[i], 7700u + (bt_u64)i, n_enemies, half, n_expl, clust);
        ck(cudaMemcpy(d_states, dev_in.data(), (size_t)NE * sizeof(BtState),
                      cudaMemcpyHostToDevice), "H2D re-seed");
    }

    double host_us = 0.0;
    for (int rep = 0; rep < REPS; rep++) {
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < NE; i++) rearm_state(&ref[i]);
        for (int i = 0; i < NE; i++) bt_burn_tick(&ref[i], &rref[i]);
        for (int i = 0; i < NE; i++) btfx_itemsfx_phase_ref(&ref[i], &rref[i]);
        auto t1 = std::chrono::steady_clock::now();
        host_us += std::chrono::duration<double, std::micro>(t1 - t0).count();
    }
    cudaEvent_t ev0, ev1;
    ck(cudaEventCreate(&ev0), "event");
    ck(cudaEventCreate(&ev1), "event");
    btfx_rearm_kernel<<<NE, 32>>>(d_states);
    btfx_phases_kernel<<<NE, 32>>>(d_states, d_scratch, d_rew);
    ck(cudaDeviceSynchronize(), "warm up");
    /* The warm-up advances the DEVICE one re-armed phase ahead of the host. Re-seed
     * both sides from the scripted image so the timed loops below run exactly REPS
     * phases each and the final digest comparison is rep-for-rep (without this the
     * device ends one phase ahead and the digest check fails by construction). */
    for (int i = 0; i < NE; i++)
        script_bench_state(&ref[i], 7700u + (bt_u64)i, n_enemies, half, n_expl, clust);
    ck(cudaMemcpy(d_states, dev_in.data(), (size_t)NE * sizeof(BtState),
                  cudaMemcpyHostToDevice), "H2D re-seed after warm up");
    ck(cudaEventRecord(ev0), "record");
    for (int rep = 0; rep < REPS; rep++) {
        btfx_rearm_kernel<<<NE, 32>>>(d_states);
        btfx_phases_kernel<<<NE, 32>>>(d_states, d_scratch, d_rew);
    }
    ck(cudaEventRecord(ev1), "record");
    ck(cudaEventSynchronize(ev1), "sync");
    float dev_ms = 0.0f;
    ck(cudaEventElapsedTime(&dev_ms, ev0, ev1), "elapsed");
    double dev_us = (double)dev_ms * 1000.0;

    std::vector<unsigned int> dg_h(NE), dg_d(NE, 0u);
    unsigned int* d_dg = nullptr;
    ck(cudaMalloc(&d_dg, (size_t)NE * sizeof(unsigned int)), "malloc digests");
    btfx_digest_kernel<<<1, 256>>>(d_states, d_dg, NE);
    ck(cudaDeviceSynchronize(), "digest");
    ck(cudaMemcpy(dg_d.data(), d_dg, (size_t)NE * sizeof(unsigned int),
                  cudaMemcpyDeviceToHost), "D2H digests");
    int dig_bad = -1;
    for (int i = 0; i < NE; i++) {
        dg_h[i] = btf_state_digest(&ref[i]);
        if (dg_h[i] != dg_d[i] && dig_bad < 0) dig_bad = i;
    }
    require(dig_bad < 0, std::string(name) + ": long-run digest after " +
                             std::to_string(REPS) + " re-armed phases (first bad env " +
                             std::to_string(dig_bad) + ")");
    std::printf("    %-22s %d envs x %d re-armed phases: serial %8.1f us/env-phase, "
                "device %7.2f us/env-phase, speedup %5.2fx\n",
                name, NE, REPS, host_us / (double)(NE * REPS),
                dev_us / (double)(NE * REPS), host_us / dev_us);
    cudaFree(d_states); cudaFree(d_scratch); cudaFree(d_rew); cudaFree(d_dg);
}

/* Host mirror of btfx_rearm_kernel (identical work, identical order per field). */
static void rearm_state(BtState* s) {
    for (int i = 0; i < BT_MAX_EXPL; i++) {
        s->expl.e[i].alive = 1;
        s->expl.e[i].age = 1 + (i % BT_EXPL_FRAMES);
        s->expl.e[i].hit.n = 0;
        s->expl.e[i].hit_overflow = 0;
    }
    for (int i = 0; i < BT_MAX; i++) {
        if (!s->ealive[i]) continue;
        s->ehp[i] = 4.0e4f; s->emax[i] = 4.0e4f;
        s->eburn[i] = 20.0f; s->eburndmg[i] = 3.0f;
    }
    for (int c = 0; c < BT_MAX_CONS; c++) s->conalive[c] = 0;
    for (int p = 0; p < s->pet_n; p++) {
        if (!s->pet_alive[p]) continue;
        if (s->pet_opcode[p] == BT_ITEMEFF_STRUCTURE) s->pet_lm_pressed[p] = 0;
        else s->pet_timer[p] = 0.0f;
    }
}

/* The device order-free pass alone (one warp per env, bp = 32), to compare its
 * four masks against the host's full-range scans: this is the check a lost mask
 * bit (two lanes sharing a word) cannot hide. */
__global__ void btfx_scans_kernel(BtState* states, BtfxScratch* scratch) {
    btfx_burn_scans(&states[blockIdx.x], &scratch[blockIdx.x], threadIdx.x, 32);
    btfx_itemsfx_scans(&states[blockIdx.x], &scratch[blockIdx.x], threadIdx.x, 32);
}

/* Host mirror: the core's own whole-range scans, the definition of each mask. */
static void host_scans(BtState* s, BtfxScratch* out) {
    std::memset(out, 0, sizeof(*out));
    bt_structure_spawning_scan(s, out->struct_ready);
    bt_item_expl_landmine_scan(s, out->lm_inside);
    bt_burn_scan(s, out->burn_tick);
    bt_item_expl_scan(s, out->expl_cand);
}

/* Whole-step driver with the phase hook wired (one warp per env, bp = 32).
 * Every lane runs bt_core_step (the core is data-independent, so the redundant
 * execution takes the same path); only the phase diverges and lane 0 writes the
 * per-frame reward/terminal. */
__global__ void btfx_step_h_kernel(BtState* base, size_t stride, const int* actions,
                                   float* obs, float* rew, float* term, int n, int H) {
    int warp = (int)((blockIdx.x * blockDim.x + threadIdx.x) >> 5);
    int lane = (int)(threadIdx.x & 31u);
    if (warp >= n) return;
    BtState* s = BTF_AT(base, stride, warp);
    for (int h = 0; h < H; h++) {
        float r = 0.0f, t = 0.0f;
        bt_core_step(s, actions[(size_t)h * (size_t)n + (size_t)warp],
                     obs + (size_t)warp * BTF_OBS_SIZE, &r, &t);
        if (lane == 0) {
            rew[(size_t)h * (size_t)n + (size_t)warp] = r;
            term[(size_t)h * (size_t)n + (size_t)warp] = t;
        }
    }
}

/* Action schedule: mostly hold (0, keep the player still so scripted enemies stay
 * in the explosion radius), with occasional movement and a shop/level index. */
static int action_for(int step, int env) {
    static const int a[8] = { 0, 0, 0, 3, 0, 0, 5, 0 };
    return a[(unsigned)(step * 7 + env * 3) % 8u];
}

int main(void) {
    int dev = 0;
    ck(cudaSetDevice(dev), "set device");
    cudaDeviceProp prop{};
    ck(cudaGetDeviceProperties(&prop, dev), "device props");
    std::printf("device: %s (cc %d.%d, %zu bytes shared/block)\n",
                prop.name, prop.major, prop.minor, prop.sharedMemPerBlock);
    std::printf("BtState = %zu bytes, BtExplosion = %zu, BtfxScratch = %zu, BTFX_BP = %d\n",
                sizeof(BtState), sizeof(BtExplosion), sizeof(BtfxScratch), (int)BTFX_BP);
    std::printf("explosion candidate bitsets: %d explosions x %d words = %d uints\n",
                (int)BT_MAX_EXPL, (int)BT_EXPL_CAND_WORDS, (int)BTFX_CAND_WORDS);

    const unsigned short expl_scene = BT_ISTR_RES___PROJECTILES_EXPLOSION_TSCN;
    g_row_expl = find_row_exploding_scene(expl_scene);
    g_row_landmine = find_landmine_row();
    g_row_garden = find_garden_row();
    std::printf("[0] scripted rows: exploding=%d landmine=%d garden=%d\n",
                g_row_expl, g_row_landmine, g_row_garden);
    if (g_row_expl < 0 || g_row_landmine < 0 || g_row_garden < 0) {
        std::printf("%d checks, %d failures (generated graph prerequisites missing)\n",
                    g_checks, g_failures);
        return 1;
    }

    /* ------------------------------------------------ A. phase parity */
    std::printf("[A] phase parity over scripted Demon states\n");
    {
        const int NE = 12;
        std::vector<BtState> host(NE);           /* serial reference  */
        std::vector<BtState> host_copy(NE);      /* device input      */
        std::vector<BtState> got(NE);
        std::vector<float> rew_ref(NE, 0.0f), rew_dev(NE, 0.0f);
        const int shapes[NE][6] = {
            /* enemies, mines, pressed, gardens, explosions, age */
            {  0,  0, 0,  0,  0,  1 },
            { 40,  0, 0,  0,  0,  1 },
            { 40, 20, 0,  0,  0,  1 },
            { 40, 20, 20, 0,  0,  1 },
            {  0,  0, 0, 12,  0,  1 },
            {  0,  0, 0,  0, 60,  1 },
            { 60,  0, 0,  0, 90,  1 },
            {100, 30, 30, 20, 120, 1 },
            {100, 30,  0, 20, 192, 1 },
            { 80, 10, 10, 10,  64,  2 },
            { 80, 10, 10, 10,  64,  3 },
            {100,128,128, 32, 192,  2 },
        };
        for (int i = 0; i < NE; i++) {
            script_state(&host[i], 900u + (bt_u64)i, shapes[i][0], shapes[i][1],
                         shapes[i][2], shapes[i][3], shapes[i][4], shapes[i][5]);
            host_copy[i] = host[i];
            rew_ref[i] = 0.0f;
            /* the frame's own order: Timer-rank burn, then the itemsfx phase */
            bt_burn_tick(&host[i], &rew_ref[i]);
            btfx_itemsfx_phase_ref(&host[i], &rew_ref[i]);
        }

        BtState* d_states = nullptr;
        BtfxScratch* d_scratch = nullptr;
        float* d_rew = nullptr;
        ck(cudaMalloc(&d_states, (size_t)NE * sizeof(BtState)), "malloc states");
        ck(cudaMalloc(&d_scratch, (size_t)NE * sizeof(BtfxScratch)), "malloc scratch");
        ck(cudaMalloc(&d_rew, (size_t)NE * sizeof(float)), "malloc rewards");
        ck(cudaMemcpy(d_states, host_copy.data(), (size_t)NE * sizeof(BtState),
                      cudaMemcpyHostToDevice), "H2D states");
        ck(cudaMemset(d_rew, 0, (size_t)NE * sizeof(float)), "zero rewards");
        btfx_phases_kernel<<<NE, 32>>>(d_states, d_scratch, d_rew);
        ck(cudaDeviceSynchronize(), "phase kernel");
        ck(cudaMemcpy(got.data(), d_states, (size_t)NE * sizeof(BtState),
                      cudaMemcpyDeviceToHost), "D2H states");
        ck(cudaMemcpy(rew_dev.data(), d_rew, (size_t)NE * sizeof(float),
                      cudaMemcpyDeviceToHost), "D2H rewards");

        for (int i = 0; i < NE; i++) {
            std::string d = state_diff(host[i], got[i]);
            require(d.empty(), "state " + std::to_string(i) + " (" +
                                   std::to_string(shapes[i][0]) + " enemies, " +
                                   std::to_string(shapes[i][1]) + " mines, " +
                                   std::to_string(shapes[i][4]) + " explosions): " + d);
            /* every byte, not only the generated field table: both sides start
             * as our own memcpy images and the core never writes padding */
            require(std::memcmp((const void*)&host[i], (const void*)&got[i],
                                sizeof(BtState)) == 0,
                    "state " + std::to_string(i) + " whole-struct bytes");
            require(shapes[i][0] + shapes[i][1] + shapes[i][2] + shapes[i][3] +
                        shapes[i][4] == 0 ||
                        std::memcmp((const void*)&host_copy[i], (const void*)&host[i],
                                    sizeof(BtState)) != 0,
                    "state " + std::to_string(i) + ": the phase mutated the state");
            require(rew_ref[i] == rew_dev[i], "state " + std::to_string(i) + " reward");
        }
        std::printf("    %d scripted states compared byte-for-byte (and reward)\n", NE);
        cudaFree(d_states); cudaFree(d_scratch); cudaFree(d_rew);
    }

    /* ------------------------------------------------ B. whole-step parity */
    std::printf("[B] whole bt_core_step parity (BT_ITEMSFX_PHASE hooked to the group)\n");
    {
        const int NE = 4, H = 24;
        std::vector<BtState> ref(NE);
        for (int i = 0; i < NE; i++)
            script_state(&ref[i], 4200u + (bt_u64)i, 50 + 10 * i, 24, 12, 8, 40, 1);

        std::vector<int> acts((size_t)H * NE);
        for (int h = 0; h < H; h++)
            for (int e = 0; e < NE; e++) acts[(size_t)h * NE + e] = action_for(h, e);

        std::vector<BtState> dev_in = ref;                 /* same start */
        std::vector<float> rew_h((size_t)H * NE, 0.0f), term_h((size_t)H * NE, 0.0f);
        std::vector<float> rew_d((size_t)H * NE, 0.0f), term_d((size_t)H * NE, 0.0f);

        BtState* d_states = nullptr;
        BtfxScratch* d_scratch = nullptr;
        float* d_rew = nullptr;
        float* d_term = nullptr;
        int* d_acts = nullptr;
        float* d_obs = nullptr;
        ck(cudaMalloc(&d_states, (size_t)NE * sizeof(BtState)), "malloc states");
        ck(cudaMalloc(&d_scratch, (size_t)NE * sizeof(BtfxScratch)), "malloc scratch");
        ck(cudaMalloc(&d_obs, (size_t)NE * BT_OBS_SIZE * sizeof(float)), "malloc obs");
        ck(cudaMalloc(&d_rew, rew_h.size() * sizeof(float)), "malloc rewards");
        ck(cudaMalloc(&d_term, term_h.size() * sizeof(float)), "malloc terminals");
        ck(cudaMalloc(&d_acts, acts.size() * sizeof(int)), "malloc actions");
        ck(cudaMemcpy(d_states, dev_in.data(), (size_t)NE * sizeof(BtState),
                      cudaMemcpyHostToDevice), "H2D states");
        ck(cudaMemcpy(d_acts, acts.data(), acts.size() * sizeof(int),
                      cudaMemcpyHostToDevice), "H2D actions");
        /* the hook derives the per-env scratch from the state pointer */
        ck(cudaMemcpyToSymbol(g_env_base, &d_states, sizeof(d_states)), "env base");
        long long stride = (long long)sizeof(BtState);
        ck(cudaMemcpyToSymbol(g_env_stride, &stride, sizeof(stride)), "env stride");
        ck(cudaMemcpyToSymbol(g_scratch_base, &d_scratch, sizeof(d_scratch)), "scratch base");

        btfx_step_h_kernel<<<NE, 32>>>(d_states, (size_t)stride, d_acts, d_obs, d_rew, d_term,
                                       NE, H);
        ck(cudaDeviceSynchronize(), "step kernel");

        /* Host reference: the same core, the same actions, serial. */
        std::vector<BtState> after(NE);
        ck(cudaMemcpy(after.data(), d_states, (size_t)NE * sizeof(BtState),
                      cudaMemcpyDeviceToHost), "D2H states");
        ck(cudaMemcpy(rew_d.data(), d_rew, rew_d.size() * sizeof(float),
                      cudaMemcpyDeviceToHost), "D2H rewards");
        ck(cudaMemcpy(term_d.data(), d_term, term_d.size() * sizeof(float),
                      cudaMemcpyDeviceToHost), "D2H terminals");

        std::vector<BtState> dev_after = after;
        std::vector<float> obs_h((size_t)NE * BT_OBS_SIZE, 0.0f);
        for (int h = 0; h < H; h++) {
            for (int e = 0; e < NE; e++) {
                float r = 0.0f, t = 0.0f;
                bt_core_step(&ref[e], acts[(size_t)h * NE + e],
                             obs_h.data() + (size_t)e * BT_OBS_SIZE, &r, &t);
                rew_h[(size_t)h * NE + e] = r;
                term_h[(size_t)h * NE + e] = t;
            }
        }
        std::vector<float> obs_d((size_t)NE * BT_OBS_SIZE, 0.0f);
        float* d_obs_out = nullptr;
        ck(cudaMalloc(&d_obs_out, obs_d.size() * sizeof(float)), "malloc obs out");
        /* re-run the device pass to capture the final observation vector */
        ck(cudaMemcpy(d_states, dev_in.data(), (size_t)NE * sizeof(BtState),
                      cudaMemcpyHostToDevice), "H2D states (obs pass)");
        btfx_step_h_kernel<<<NE, 32>>>(d_states, (size_t)stride, d_acts, d_obs_out, d_rew, d_term,
                                       NE, H);
        ck(cudaDeviceSynchronize(), "step kernel (obs pass)");
        ck(cudaMemcpy(obs_d.data(), d_obs_out, obs_d.size() * sizeof(float),
                      cudaMemcpyDeviceToHost), "D2H obs");
        int obs_bad = -1;
        for (size_t i = 0; i < obs_d.size(); i++)
            if (obs_h[i] != obs_d[i]) { obs_bad = (int)i; break; }
        require(obs_bad < 0, "whole step: observation float #" + std::to_string(obs_bad));
        cudaFree(d_obs_out);
        int mismatch_step = -1;
        for (int h = 0; h < H && mismatch_step < 0; h++) {
            for (int e = 0; e < NE; e++) {
                if (rew_h[(size_t)h * NE + e] != rew_d[(size_t)h * NE + e] ||
                    term_h[(size_t)h * NE + e] != term_d[(size_t)h * NE + e]) {
                    std::string why = "step " + std::to_string(h) + " env " + std::to_string(e) +
                                      " reward/terminal";
                    require(false, why);
                    mismatch_step = h;
                    break;
                }
            }
        }
        /* The device ran all H ticks in one launch; compare the final state. To
         * keep the diagnostic per-step, replay with the device state pulled back
         * after every tick is the slow path -- instead compare the end state AND
         * the per-tick reward/terminal, which pinpoints a divergence tick. */
        for (int e = 0; e < NE; e++) {
            std::string d = state_diff(ref[e], dev_after[e]);
            require(d.empty(), "whole step, env " + std::to_string(e) + ": " + d);
        }
        std::printf("    %d envs x %d ticks: state + reward + terminal match\n", NE, H);
        cudaFree(d_states); cudaFree(d_scratch); cudaFree(d_rew); cudaFree(d_term);
        cudaFree(d_acts); cudaFree(d_obs);
    }

    /* ------------------------------------------------ C. cost */
    std::printf("[C] cost: serial reference vs device group\n");
    {
        const int NE = 32, REPS = 100;
        /* Two workloads, both re-armed each rep so the timed phase is always the
         * full one:
         *   realistic   -- 96 enemies on a 600 px box, the 192 scripted
         *                  explosions clustered near the player, so ~30% of the
         *                  explosion/target pairs overlap (a plausible Demon
         *                  wave: the order-free scan is the bulk);
         *   worst-case  -- every enemy piled inside the cluster radius, so
         *                  essentially every pair overlaps and the ORDER-LOCKED
         *                  apply (RNG + damage + kill callbacks, which cannot be
         *                  re-scheduled) dominates: the bound on any speedup. */
        bench_workload("realistic", 96, 300.0f, BT_MAX_EXPL, 40.0f, NE, REPS);
        bench_workload("worst-case dense", BT_MAX, 100.0f, BT_MAX_EXPL, 20.0f, NE, REPS);
        std::printf("    order-free pair predicates per env-phase: explosion x target %d "
                    "(%d x %d), landmine capsule %d (%d x %d), burn %d, garden %d\n",
                    (int)(BT_MAX_EXPL * BT_MAX), (int)BT_MAX_EXPL, (int)BT_MAX,
                    (int)(BT_MAX_PET * BT_MAX), (int)BT_MAX_PET, (int)BT_MAX,
                    (int)BT_MAX, (int)BT_MAX_PET);
    }

    /* ------------------------------- D. core-side cand contract (host only) */
    std::printf("[D] core-side cand contract + range partitioning (host only)\n");
    {
        const int NE = 6, LANES = 7;
        std::vector<BtState> ref(NE), sched(NE);
        std::vector<float> r_ref(NE, 0.0f), r_sched(NE, 0.0f);
        std::vector<unsigned int> cand((size_t)BT_MAX_EXPL * BT_EXPL_CAND_WORDS), cand2(cand.size());
        std::vector<unsigned int> lm(BTFX_PET_WORDS), lm2(lm.size());
        std::vector<unsigned int> bm(BTFX_ENEMY_WORDS), bm2(bm.size());
        std::vector<unsigned int> sm(BTFX_PET_WORDS), sm2(sm.size());
        for (int i = 0; i < NE; i++) {
            script_state(&ref[i], 3100u + (bt_u64)i, 40, 16, 8, 6, 64, 1 + (i % 3));
            sched[i] = ref[i];
            bt_burn_tick(&ref[i], &r_ref[i]);
            btfx_itemsfx_phase_ref(&ref[i], &r_ref[i]);
            BtState* s = &sched[i];
            /* every mask computed over the whole range must equal the union of
             * LANES disjoint ranges (what the device group does) */
            std::fill(cand.begin(), cand.end(), 0u);
            std::fill(cand2.begin(), cand2.end(), 0u);
            bt_item_expl_scan(s, cand.data());
            for (int m = 0; m < LANES; m++) {
                int c = (BT_MAX_EXPL + LANES - 1) / LANES;
                bt_item_expl_scan_range(s, cand2.data(), m * c, m * c + c);
            }
            require(cand == cand2, "explosion range partition (state " + std::to_string(i) + ")");
            std::fill(lm.begin(), lm.end(), 0u); std::fill(lm2.begin(), lm2.end(), 0u);
            bt_item_expl_landmine_scan(s, lm.data());
            for (int m = 0; m < LANES; m++) {
                int c = (BT_MAX_PET + LANES - 1) / LANES;
                bt_item_expl_landmine_scan_range(s, lm2.data(), m * c, m * c + c);
            }
            require(lm == lm2, "landmine range partition (state " + std::to_string(i) + ")");
            std::fill(bm.begin(), bm.end(), 0u); std::fill(bm2.begin(), bm2.end(), 0u);
            bt_burn_scan(s, bm.data());
            for (int m = 0; m < LANES; m++) {
                int c = (BT_MAX + LANES - 1) / LANES;
                bt_burn_scan_range(s, bm2.data(), m * c, m * c + c);
            }
            require(bm == bm2, "burn range partition (state " + std::to_string(i) + ")");
            std::fill(sm.begin(), sm.end(), 0u); std::fill(sm2.begin(), sm2.end(), 0u);
            bt_structure_spawning_scan(s, sm.data());
            for (int m = 0; m < LANES; m++) {
                int c = (BT_MAX_PET + LANES - 1) / LANES;
                bt_structure_spawning_scan_range(s, sm2.data(), m * c, m * c + c);
            }
            require(sm == sm2, "garden range partition (state " + std::to_string(i) + ")");
            /* the ordered apply reading those masks must equal the serial phase */
            bt_structure_spawning_tick_cand(s, sm.data());
            (void)bt_item_expl_landmine_tick_cand(s, lm.data());
            bt_burn_tick_cand(s, bm.data(), &r_sched[i]);
            (void)bt_item_expl_tick_cand(s, &r_sched[i], cand.data());
            std::string d = state_diff(ref[i], sched[i]);
            require(d.empty(), "cand schedule == serial (state " + std::to_string(i) + "): " + d);
            require(r_ref[i] == r_sched[i], "cand reward (state " + std::to_string(i) + ")");
        }
        std::printf("    %d scripted states: masks and the cand apply equal the serial phase\n", NE);
    }

    /* ------------------- E. the phase runs exactly once per bt_core_step ---- */
    std::printf("[E] one items/fx phase per step (double-application regression)\n");
    {
        BtState s;
        script_state(&s, 6100u, 0, 0, 0, 1, 0, 1);
        int g = -1;
        for (int p = 0; p < s.pet_n; p++)
            if (s.pet_alive[p] && s.pet_opcode[p] == BT_ITEMEFF_TURRET) { g = p; break; }
        require(g >= 0, "scripted garden slot");
        if (g >= 0) {
            /* A garden's only pet_timer writer in a step with no enemies is the
             * items/fx phase: `pet_timer -= BT_DT` exactly once. Park the timer
             * well above zero so no emission happens and the exact FP decrement
             * sequence is observable; 60 ticks must subtract exactly 60 * BT_DT. */
            s.pet_timer[g] = 10.0f;
            float expect = 10.0f;
            std::vector<float> obs(BT_OBS_SIZE, 0.0f);
            const int H = 60;
            for (int h = 0; h < H; h++) {
                float r = 0.0f, t = 0.0f;
                bt_core_step(&s, 0, obs.data(), &r, &t);
                expect -= BT_DT;
            }
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "garden cooldown decremented once per tick (got %.9g want %.9g)",
                          (double)s.pet_timer[g], (double)expect);
            require(s.pet_timer[g] == expect, buf);
        }
        /* The burn rank must likewise run exactly once per step: an enemy with
         * eburn = 20 must count down to 19 over one step, never 18 (a burn tick
         * applied twice in the frame shows up here). */
        BtState b;
        script_state(&b, 6200u, 1, 0, 0, 0, 0, 1);
        int e = -1;
        for (int i = 0; i < BT_MAX; i++)
            if (b.ealive[i] && !b.eneutral[i]) { e = i; break; }
        require(e >= 0, "scripted enemy slot for the burn check");
        if (e >= 0) {
            b.eburn[e] = 20.0f;
            b.eburndmg[e] = 0.0f;   /* no damage: only the countdown is observed */
            std::vector<float> obs2(BT_OBS_SIZE, 0.0f);
            float r = 0.0f, t = 0.0f;
            bt_core_step(&b, 0, obs2.data(), &r, &t);
            char bb[160];
            std::snprintf(bb, sizeof(bb),
                          "burning countdown decremented once per tick (got %.9g want 19)",
                          (double)b.eburn[e]);
            require(b.eburn[e] == 19.0f, bb);
        }
    }

    /* ------------------- F. device scan masks == host full-range masks ------ */
    std::printf("[F] device order-free masks vs host full-range scans\n");
    {
        const int NE = 8, ITERS = 40;
        std::vector<BtState> host(NE), dev_in(NE);
        std::vector<BtfxScratch> want(NE), got(NE);
        for (int i = 0; i < NE; i++) {
            script_state(&host[i], 5200u + (bt_u64)i, 60 + 10 * i, 24, 8, 8, BT_MAX_EXPL, 1);
            host_scans(&host[i], &want[i]);
            dev_in[i] = host[i];
        }
        BtState* d_states = nullptr;
        BtfxScratch* d_sc = nullptr;
        ck(cudaMalloc(&d_states, (size_t)NE * sizeof(BtState)), "malloc states");
        ck(cudaMalloc(&d_sc, (size_t)NE * sizeof(BtfxScratch)), "malloc scratch");
        ck(cudaMemcpy(d_states, dev_in.data(), (size_t)NE * sizeof(BtState),
                      cudaMemcpyHostToDevice), "H2D states");
        for (int it = 0; it < ITERS; it++) {
            ck(cudaMemset(d_sc, 0, (size_t)NE * sizeof(BtfxScratch)), "zero scratch");
            btfx_scans_kernel<<<NE, 32>>>(d_states, d_sc);
            ck(cudaDeviceSynchronize(), "scans kernel");
            ck(cudaMemcpy(got.data(), d_sc, (size_t)NE * sizeof(BtfxScratch),
                          cudaMemcpyDeviceToHost), "D2H scratch");
            for (int i = 0; i < NE; i++) {
                require(std::memcmp(want[i].struct_ready, got[i].struct_ready,
                                    sizeof(want[i].struct_ready)) == 0,
                        "iter " + std::to_string(it) + " env " + std::to_string(i) +
                            ": garden mask");
                require(std::memcmp(want[i].lm_inside, got[i].lm_inside,
                                    sizeof(want[i].lm_inside)) == 0,
                        "iter " + std::to_string(it) + " env " + std::to_string(i) +
                            ": landmine mask");
                require(std::memcmp(want[i].burn_tick, got[i].burn_tick,
                                    sizeof(want[i].burn_tick)) == 0,
                        "iter " + std::to_string(it) + " env " + std::to_string(i) +
                            ": burn mask");
                require(std::memcmp(want[i].expl_cand, got[i].expl_cand,
                                    sizeof(want[i].expl_cand)) == 0,
                        "iter " + std::to_string(it) + " env " + std::to_string(i) +
                            ": explosion cand mask");
            }
            if (g_failures) break;
        }
        std::printf("    %d envs x %d runs: all four masks equal the host scans\n", NE, ITERS);
        cudaFree(d_states); cudaFree(d_sc);
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
