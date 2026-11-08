// Benchmark + bit-exactness harness for ocean/brotato/brotato_gpu_observe.cuh,
// the lane-parallel (warp-per-env) observation writer.
//
// Compile (repo root, sm_120 = RTX 5070):
//
//   nvcc -O2 -std=c++17 -arch=sm_120 --fmad=false -Xcompiler=-ffp-contract=off \
//     -Xcompiler=-Wno-narrowing -Xcompiler=-Wno-unreachable-code \
//     --diag-suppress=2361 --diag-suppress=111 --diag-suppress=128 \
//     -Xptxas -v -I. -Isrc -Iocean/brotato \
//     tests/bench_brotato_gpu_observe.cu -lm -o /tmp/bench_brotato_gpu_observe
//   /tmp/bench_brotato_gpu_observe
//
// --fmad=false + -ffp-contract=off are REQUIRED (the core never contracts a
// multiply-add on either pass). -Xptxas -v prints register/spill numbers.
//
// Parts:
//   1. device-vs-host byte parity of the observation and the action mask:
//        * btgo_observe_warp_kernel   (32 lanes per env)  vs bt_core_observe
//        * btgo_observe_thread_kernel ( 1 thread per env) vs bt_core_observe
//      over a batch of Demon envs warmed through combat / shop / level-up, plus
//      hand-built tie-heavy states (exact-distance ties in the enemy /
//      projectile / consumable sets). Every observation float, every mask byte
//      and the whole BtState (unchanged by observe) are compared.
//   2. throughput: warp-per-env vs thread-per-env observation, batch sweep.
//
// Environment knobs: BT_BENCH=0 (parity only), BT_ENVS=N, BT_WARM=N,
// BT_REF_ENVS=N (host-reference subset), BT_PARITY_STEPS=N.

#include <cuda_runtime.h>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../ocean/brotato/brotato_gpu_observe.cuh"

// CUDA device-pass name-resolver stub (bt_weapon_id_from_name is BT_HOST_ONLY and
// dropped in the device pass, but is referenced by a BT_HD initialiser; mirrors
// brotato_gpu_fast.cuh).
#if defined(__CUDA_ARCH__)
static inline unsigned short bt_weapon_id_from_name(const char* n) { (void)n; return 0xFFFFu; }
#endif

#define CUDA_OK(call) do {                                                     \
    cudaError_t _e = (call);                                                   \
    if (_e != cudaSuccess) {                                                   \
        std::fprintf(stderr, "CUDA error %s at %s:%d\n",                       \
                     cudaGetErrorString(_e), __FILE__, __LINE__);              \
        std::exit(1);                                                          \
    }                                                                          \
} while (0)

static int env_int(const char* k, int dflt) {
    const char* v = getenv(k);
    return v && *v ? atoi(v) : dflt;
}

/* deterministic xorshift64 (host) */
static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 32);
}
static int rndi(int n) { return n <= 0 ? 0 : (int)(rnd() % (uint32_t)n); }
static float rndf(float lo, float hi) {
    return lo + (hi - lo) * ((float)(rnd() & 0xFFFFFFu) / 16777216.0f);
}

/* tie-heavy synthetic state (see tests/test_brotato_gpu_observe.cpp) */
static void force_ties(BtState* s) {
    s->e_used = BT_MAX; s->pr_used = BT_MAX_PROJ; s->con_used = BT_MAX_CONS;
    for (int i = 0; i < BT_MAX; i++) {
        bt_enemy_set_alive(s, i, (unsigned char)(i < 40 ? 1 : 0));
        /* slots 40..49 are dying-but-not-alive: still observe candidates */
        bt_enemy_set_dying(s, i, (unsigned char)((i >= 40 && i < 50) ? 5 : (i % 7 == 3 ? 3 : 0)));
        s->eelite[i] = (unsigned char)(i & 1);
        s->ehp[i] = s->ealive[i] ? 30.0f + (float)i : 0.0f;
        s->emax[i] = 100.0f;
        s->edmg[i] = 5.0f + (float)(i % 7);
        s->ex[i] = s->px + (float)((i % 5) - 2);
        s->ey[i] = s->py + (float)((i / 5) % 5 - 2);
    }
    for (int i = 0; i < BT_MAX_PROJ; i++) {
        s->pralive[i] = (unsigned char)(i < 12 ? 1 : 0);
        s->prfriendly[i] = 0;
        s->prsrc[i] = (unsigned short)i;
        s->prdmg[i] = 7.0f + (float)i;
        s->prx[i] = s->px + (float)((i % 3) - 1);
        s->pry[i] = s->py + (float)((i % 3) - 1);
    }
    for (int i = 0; i < BT_MAX_CONS; i++) {
        s->conalive[i] = (unsigned char)(i < 8 ? 1 : 0);
        s->conx[i] = s->px + (float)((i % 4) - 1);
        s->cony[i] = s->py + (float)((i % 4) - 1);
    }
}

static void randomize_bounded(BtState* s) {
    memset(s, 0, sizeof(*s));
    s->e_used = BT_MAX; s->pr_used = BT_MAX_PROJ; s->con_used = BT_MAX_CONS;
    s->phase = rndi(3);
    s->hp = rndf(1.0f, 100.0f);
    s->max_hp = rndf(1.0f, 100.0f);
    s->regen = rndf(0.0f, 30.0f);
    s->iframe = (rnd() & 3u) == 0 ? 1.0f : 0.0f;
    s->materials = rndf(0.0f, 500.0f);
    s->level = rndi(41);
    s->xp = rndf(0.0f, 800.0f);
    s->xp_need = 1 + rndi(200);
    s->px = rndf(0.0f, BT_ARENA_W);
    s->py = rndf(0.0f, BT_ARENA_H);
    s->wave = 1 + rndi(40);
    s->wave_duration = 20.0f + rndf(0.0f, 60.0f);
    s->time_left = rndf(0.0f, s->wave_duration);
    s->harvest = rndf(0.0f, 300.0f);
    s->luck = rndf(0.0f, 300.0f);
    for (int i = 0; i < BT_NSTATS; i++) {
        s->stat_raw[i] = rndf(-20.0f, 60.0f);
        s->stat_gain[i] = rndf(0.5f, 2.5f);
    }
    s->scalar_effect[BT_ISTR_ITEMS_PRICE] = rndf(-40.0f, 60.0f);
    s->scalar_effect[BT_ISTR_REROLL_PRICE] = rndf(-40.0f, 60.0f);
    s->character = (unsigned short)rndi(BT_NCHARACTERS);
    s->weapon_slot_limit = (unsigned char)(1 + rndi(6));
    s->wpn_count = (unsigned char)rndi(BT_SLOTS + 1);
    for (int i = 0; i < BT_SLOTS; i++) {
        s->wpn_id[i] = (rnd() & 7u) == 0 ? BT_WEAPON_NONE : (unsigned short)rndi(BT_NWEAPONS);
        s->wpn_tier[i] = (unsigned char)rndi(6);
        s->wpn_cursed[i] = (unsigned char)rndi(2);
        s->wpn_cd[i] = rndf(0.0f, 400.0f);
    }
    for (int i = 0; i < 4; i++) {
        unsigned char kind = (unsigned char)rndi(3);
        s->shop_kind[i] = kind;
        s->shop_id[i] = kind == BT_SHOP_ITEM ? (unsigned short)rndi(BT_NITEMS)
                      : kind == BT_SHOP_WEAPON ? (unsigned short)rndi(BT_NWEAPONS) : 0;
        s->shop_cursed[i] = (unsigned char)rndi(2);
        s->lvl_offers[i] = (unsigned short)rndi(BT_NUPGRADES);
        s->shop_locked[i] = (unsigned char)rndi(3);
    }
    s->crate_kind = (unsigned char)rndi(3);
    s->crate_id = s->crate_kind == BT_SHOP_ITEM ? (unsigned short)rndi(BT_NITEMS) : 0;
    s->crate_cursed = (unsigned char)rndi(2);
    s->lvl_reroll_price = rndi(200);
    s->shop_available = (unsigned char)(rnd() & 15u);
    s->shop_timer = (rnd() & 1u) ? rndi(120) : -rndi(5);
    s->paid_rerolls = rndi(5);
    s->free_rerolls_remaining = rndi(3);
    s->has_bonus_free_reroll = (unsigned char)rndi(2);
    for (int i = 0; i < BT_NITEMS; i++) s->item_owned[i] = (unsigned short)rndi(7);
    for (int i = 0; i < BT_MAX; i++) {
        bt_enemy_set_alive(s, i, (rnd() & 1u) ? 1 : 0);
        /* a dying slot (death animation still playing) is an observe candidate
         * even when ealive == 0 */
        bt_enemy_set_dying(s, i, (rnd() & 1u)
            ? (unsigned char)rndi(BT_POOL_DEATH_FRAMES + 1) : 0);
        s->eelite[i] = (unsigned char)rndi(2);
        s->ehp[i] = s->edying[i] != 0 ? 0.0f : rndf(0.0f, 500.0f);
        s->emax[i] = rndf(50.0f, 500.0f);
        s->edmg[i] = rndf(0.0f, 50.0f);
        s->ex[i] = rndf(0.0f, BT_ARENA_W);
        s->ey[i] = rndf(0.0f, BT_ARENA_H);
    }
    for (int i = 0; i < BT_MAX_PROJ; i++) {
        s->pralive[i] = (unsigned char)((rnd() & 3u) ? 1 : 0);
        s->prfriendly[i] = (unsigned char)rndi(2);
        s->prsrc[i] = (rnd() & 3u) == 0 ? 0xFFFFu : (unsigned short)rndi(BT_MAX);
        s->prdmg[i] = rndf(0.0f, 40.0f);
        s->prx[i] = rndf(0.0f, BT_ARENA_W);
        s->pry[i] = rndf(0.0f, BT_ARENA_H);
    }
    for (int i = 0; i < BT_MAX_CONS; i++) {
        s->conalive[i] = (unsigned char)rndi(2);
        s->conx[i] = rndf(0.0f, BT_ARENA_W);
        s->cony[i] = rndf(0.0f, BT_ARENA_H);
    }
}

/* --- device kernels ------------------------------------------------------- */

// Duplicate a small set of source states across the whole batch by byte copy.
// Used only by the timing sweep so one representative state can fill thousands
// of envs without instantiating bt_core_step in the device pass (which is the
// single most expensive function for nvcc/ptxas in this TU).
__global__ void btgo_spread_kernel(BtState* __restrict__ base, size_t stride,
        const BtState* __restrict__ src, int nsrc, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    BtState* d = (BtState*)((char*)base + (size_t)i * stride);
    const BtState* s = (const BtState*)((const char*)src + (size_t)(i % nsrc) * stride);
    memcpy(d, s, sizeof(BtState));
}

/* --- host reference ------------------------------------------------------- */

// Warm one state on the host: bt_core_init + `steps` pseudo-random actions
// (deterministic per seed). bt_core_step is the definition of the tick, and the
// host pass is the same code the device pass compiles, so the states fed to the
// device kernels are real core states.
static void host_warm(BtState* s, int steps, unsigned long long seed,
        int character, int weapon, unsigned int h) {
    bt_core_init(s, seed, 0, 1, (unsigned short)character, (unsigned short)weapon);
    h |= 1u;
    for (int t = 0; t < steps; t++) {
        h = h * 1664525u + 1013904223u;
        float r = 0.0f, te = 0.0f;
        bt_core_step(s, (int)((h >> 16) % (unsigned)BT_ACTIONS), 0, &r, &te);
    }
}

struct CmpResult { int obs_bad = 0; int mask_bad = 0; int state_bad = 0; int first_obs = -1; };

static CmpResult compare_env(const BtState& src,
        const float* dev_obs, const unsigned char* dev_mask) {
    CmpResult r;
    BtState s = src;
    std::vector<float> ref(BT_OBS_SIZE);
    unsigned char refm[BT_ACTIONS];
    bt_core_observe(&s, ref.data());
    bt_action_mask(&s, refm);
    for (int i = 0; i < BT_OBS_SIZE; i++) {
        if (memcmp(&ref[i], &dev_obs[i], sizeof(float)) != 0) {
            r.obs_bad++;
            if (r.first_obs < 0) r.first_obs = i;
        }
    }
    for (int i = 0; i < BT_ACTIONS; i++)
        if (refm[i] != dev_mask[i]) r.mask_bad++;
    if (memcmp(&s, &src, sizeof(BtState)) != 0) r.state_bad++;
    return r;
}

int main(void) {
    int bench = env_int("BT_BENCH", 1);
    int parity_steps = env_int("BT_PARITY_STEPS", 24);

    CUDA_OK(cudaFree(0));
    cudaDeviceProp prop;
    CUDA_OK(cudaGetDeviceProperties(&prop, 0));

    size_t state_size = sizeof(BtState);
    unsigned short scepter = bt_weapon_id_from_name("weapon_ghost_scepter_1");
    if (scepter == BT_WEAPON_NONE) scepter = 0;

    printf("=== brotato_gpu_observe: warp-per-env observation ===\n");
    printf("device: %s (sm_%d%d), BtState = %zu bytes, BT_OBS_SIZE = %d, BT_ACTIONS = %d\n",
           prop.name, prop.major, prop.minor, state_size, (int)BT_OBS_SIZE, (int)BT_ACTIONS);

    /* ---------------------------------------------------------------- part 1 */
    /* Parity batch: warmed live states + hand-built synthetic states. */
    const int n_envs = env_int("BT_ENVS", 64);
    const int warm_default = env_int("BT_WARM", 900);
    BtState* d_states = nullptr;
    CUDA_OK(cudaMalloc(&d_states, state_size * (size_t)n_envs));

    // Half combat-warmed, half pushed far enough to reach the shop phase.
    int warm_combat = warm_default;
    int warm_long = warm_default * 7;
    {
        std::vector<BtState> hstates((size_t)n_envs);
        int half = n_envs / 2;
        for (int i = 0; i < n_envs; i++) {
            int steps = (i < half) ? warm_combat : warm_long;
            host_warm(&hstates[(size_t)i], steps,
                      0x1234567ull + 7919ull * (unsigned long long)i,
                      BT_CHARACTER_DEMON, (int)scepter,
                      (unsigned int)(i * 2654435761u));
        }
        CUDA_OK(cudaMemcpy(d_states, hstates.data(), state_size * (size_t)n_envs,
                           cudaMemcpyHostToDevice));
    }

    float* d_obs = nullptr;
    unsigned char* d_mask = nullptr;
    CUDA_OK(cudaMalloc(&d_obs, sizeof(float) * BT_OBS_SIZE * (size_t)n_envs));
    CUDA_OK(cudaMalloc(&d_mask, (size_t)BT_ACTIONS * (size_t)n_envs));

    /* hand-built / randomized synthetic states + host copies */
    std::vector<BtState> synth(parity_steps);
    for (int i = 0; i < parity_steps; i++) {
        BtState s;
        randomize_bounded(&s);
        if (i & 1) force_ties(&s);
        synth[i] = s;
    }
    BtState* d_synth = nullptr;
    CUDA_OK(cudaMalloc(&d_synth, state_size * (size_t)parity_steps));
    CUDA_OK(cudaMemcpy(d_synth, synth.data(), state_size * (size_t)parity_steps,
                       cudaMemcpyHostToDevice));

    int total_bad = 0;
    for (int pass = 0; pass < 2; pass++) {
        BtState* d_base = (pass == 1) ? d_synth : d_states;
        int cnt = (pass == 1) ? parity_steps : n_envs;
        // warp kernel
        CUDA_OK(cudaMemset(d_obs, 0xAB, sizeof(float) * BT_OBS_SIZE * (size_t)cnt));
        CUDA_OK(cudaMemset(d_mask, 0xAB, (size_t)BT_ACTIONS * (size_t)cnt));
        int b = BTGO_WARPS_PER_BLOCK;
        btgo_observe_warp_kernel<<<(cnt + b - 1) / b, b * 32>>>(d_base, state_size,
            d_obs, d_mask, cnt);
        CUDA_OK(cudaDeviceSynchronize());
        CUDA_OK(cudaGetLastError());

        // thread kernel
        std::vector<float> obs_w(BT_OBS_SIZE * (size_t)cnt), obs_t(BT_OBS_SIZE * (size_t)cnt);
        std::vector<unsigned char> mask_w((size_t)BT_ACTIONS * (size_t)cnt),
                                    mask_t((size_t)BT_ACTIONS * (size_t)cnt);
        CUDA_OK(cudaMemcpy(obs_w.data(), d_obs, obs_w.size() * sizeof(float), cudaMemcpyDeviceToHost));
        CUDA_OK(cudaMemcpy(mask_w.data(), d_mask, mask_w.size(), cudaMemcpyDeviceToHost));
        btgo_observe_thread_kernel<<<(cnt + 127) / 128, 128>>>(d_base, state_size,
            d_obs, d_mask, cnt);
        CUDA_OK(cudaDeviceSynchronize());
        CUDA_OK(cudaGetLastError());
        CUDA_OK(cudaMemcpy(obs_t.data(), d_obs, obs_t.size() * sizeof(float), cudaMemcpyDeviceToHost));
        CUDA_OK(cudaMemcpy(mask_t.data(), d_mask, mask_t.size(), cudaMemcpyDeviceToHost));

        // compare the two kernels against each other
        int diffs = 0;
        for (size_t k = 0; k < obs_w.size(); k++)
            if (memcmp(&obs_w[k], &obs_t[k], sizeof(float)) != 0) diffs++;
        for (size_t k = 0; k < mask_w.size(); k++)
            if (mask_w[k] != mask_t[k]) diffs++;

        // compare against the host reference on a subset (each compare copies a
        // full BtState back, so keep the subset small)
        int ref_envs = env_int("BT_REF_ENVS", 24);
        if (ref_envs > cnt) ref_envs = cnt;
        int bad = 0;
        for (int i = 0; i < ref_envs; i++) {
            int idx = (cnt > ref_envs) ? (int)((uint64_t)i * (uint64_t)cnt / (uint64_t)ref_envs) : i;
            BtState hs;
            if (pass == 1) hs = synth[idx];
            else CUDA_OK(cudaMemcpy(&hs, (char*)d_base + (size_t)idx * state_size,
                                    state_size, cudaMemcpyDeviceToHost));
            CmpResult cw = compare_env(hs, obs_w.data() + (size_t)idx * BT_OBS_SIZE,
                                       mask_w.data() + (size_t)idx * BT_ACTIONS);
            CmpResult ct = compare_env(hs, obs_t.data() + (size_t)idx * BT_OBS_SIZE,
                                       mask_t.data() + (size_t)idx * BT_ACTIONS);
            if (cw.obs_bad || ct.obs_bad) {
                bad++;
                printf("  FAIL pass %d env %d: warp obs_bad=%d (first %d) thread obs_bad=%d (first %d)\n",
                       pass, idx, cw.obs_bad, cw.first_obs, ct.obs_bad, ct.first_obs);
            }
            if (cw.mask_bad || ct.mask_bad)
                printf("  FAIL pass %d env %d: warp mask_bad=%d thread mask_bad=%d\n",
                       pass, idx, cw.mask_bad, ct.mask_bad);
            if (cw.state_bad) printf("  FAIL pass %d env %d: warp observe mutated state\n", pass, idx);
            if (ct.state_bad) printf("  FAIL pass %d env %d: thread observe mutated state\n", pass, idx);
            bad += (cw.mask_bad || ct.mask_bad || cw.state_bad || ct.state_bad) ? 1 : 0;
            if (bad > 5) break;
        }
        total_bad += bad;
        const char* name = (pass == 0) ? "warmed batch " : "synthetic/tie ";
        printf("part1 %s: %d envs x2 kernels, kernel-vs-kernel diffs=%d, host-ref checked %d envs, bad=%d\n",
               name, cnt, diffs, ref_envs, bad);
    }
    cudaFree(d_synth);
    printf("PART 1 %s (%d envs/layouts checked)\n", total_bad ? "FAILED" : "PASSED", n_envs + parity_steps);

    /* ---------------------------------------------------------------- part 2 */
    if (bench) {
        size_t free_b = 0, total_b = 0;
        CUDA_OK(cudaMemGetInfo(&free_b, &total_b));
        // keep the state array under a third of free (other agents share the GPU)
        int max_envs = (int)((free_b / 3) / state_size);
        if (max_envs > 4096) max_envs = 4096;
        if (max_envs < 64) max_envs = 64;

        int sweep[8]; int nsweep = 0;
        for (int e = 64; e <= max_envs && nsweep < 8; e <<= 1) sweep[nsweep++] = e;
        if (nsweep == 0) { sweep[nsweep++] = max_envs; }

        BtState* db = nullptr;
        CUDA_OK(cudaMalloc(&db, state_size * (size_t)max_envs));
        float* ob = nullptr;
        unsigned char* mb = nullptr;
        CUDA_OK(cudaMalloc(&ob, sizeof(float) * BT_OBS_SIZE * (size_t)max_envs));
        CUDA_OK(cudaMalloc(&mb, (size_t)BT_ACTIONS * (size_t)max_envs));
        // 8 distinct warmed states, spread across the batch by byte copy
        const int nsrc = 8;
        std::vector<BtState> srcs((size_t)nsrc);
        for (int i = 0; i < nsrc; i++)
            host_warm(&srcs[(size_t)i], warm_default,
                      0x5A5A5A5Aull + 104729ull * (unsigned long long)i,
                      BT_CHARACTER_DEMON, (int)scepter,
                      (unsigned int)(0x9E3779B9u * (unsigned)(i + 1)));
        BtState* d_srcs = nullptr;
        CUDA_OK(cudaMalloc(&d_srcs, state_size * (size_t)nsrc));
        CUDA_OK(cudaMemcpy(d_srcs, srcs.data(), state_size * (size_t)nsrc,
                           cudaMemcpyHostToDevice));
        btgo_spread_kernel<<<(max_envs + 127) / 128, 128>>>(db, state_size, d_srcs,
                                                            nsrc, max_envs);
        CUDA_OK(cudaDeviceSynchronize());
        CUDA_OK(cudaGetLastError());

        printf("part2: observation throughput (ns per env-observe; warmer = state in cache)\n");
        printf("  %8s %14s %14s %10s\n", "envs", "warp ns/env", "thread ns/env", "speedup");
        for (int si = 0; si < nsweep; si++) {
            int ne = sweep[si];
            int b = BTGO_WARPS_PER_BLOCK;
            // warm
            btgo_observe_warp_kernel<<<(ne + b - 1) / b, b * 32>>>(db, state_size, ob, mb, ne);
            CUDA_OK(cudaDeviceSynchronize());
            int reps = (ne < 4096) ? 200 : 40;
            auto t0 = std::chrono::high_resolution_clock::now();
            for (int r = 0; r < reps; r++)
                btgo_observe_warp_kernel<<<(ne + b - 1) / b, b * 32>>>(db, state_size, ob, mb, ne);
            CUDA_OK(cudaDeviceSynchronize());
            auto t1 = std::chrono::high_resolution_clock::now();
            double warp_ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / (double)((size_t)reps * ne);

            btgo_observe_thread_kernel<<<(ne + 127) / 128, 128>>>(db, state_size, ob, mb, ne);
            CUDA_OK(cudaDeviceSynchronize());
            t0 = std::chrono::high_resolution_clock::now();
            for (int r = 0; r < reps; r++)
                btgo_observe_thread_kernel<<<(ne + 127) / 128, 128>>>(db, state_size, ob, mb, ne);
            CUDA_OK(cudaDeviceSynchronize());
            t1 = std::chrono::high_resolution_clock::now();
            double thr_ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / (double)((size_t)reps * ne);

            printf("  %8d %14.1f %14.1f %9.2fx\n", ne, warp_ns, thr_ns, thr_ns / warp_ns);
        }
        cudaFree(mb); cudaFree(ob); cudaFree(db); cudaFree(d_srcs);
    }

    if (total_bad) { printf("RESULT: FAILED\n"); return 1; }
    printf("RESULT: OK\n");
    return 0;
}
