// Integrated warp-per-env tick: bit-exact verification vs the CPU core and
// env-only throughput against the plain one-thread-per-env kernel, in ONE binary
// on identical states. Demon + Ghost Scepter, D5, zone 0.
//
//   nvcc -O2 -std=c++17 -arch=sm_120 --fmad=false -Xcompiler=-ffp-contract=off \
//     -Xcompiler=-Wno-narrowing -Xcompiler=-Wno-unreachable-code \
//     --diag-suppress=2361 --diag-suppress=111 --diag-suppress=128 \
//     -I. -Isrc -Iocean/brotato -Ivendor \
//     tests/bench_brotato_warp.cu -lm -Xlinker=-lpthread -o /tmp/bench_warp
//
// BT_ENVS=<n> BT_TICKS=<t> BT_PARITY_TICKS=<t> BT_NO_POLICY=1 knobs.

#include <cuda_runtime.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../ocean/brotato/brotato_gpu_warp.cuh"

// This harness drives the cooperative tick DIRECTLY, so it must be built with
// -DBTFW_EXPERIMENTAL_LANE_SCHED: without it btfw_scratch_bytes() returns 0 (the
// production hard-disable) and the warp kernel would run with no per-env physics
// scratch. Requiring the macro here keeps that from happening by accident.
#if !defined(BTFW_EXPERIMENTAL_LANE_SCHED)
#error "build tests/bench_brotato_warp.cu with -DBTFW_EXPERIMENTAL_LANE_SCHED"
#endif

#define CUDA_OK(call) do {                                                     \
    cudaError_t _e = (call);                                                   \
    if (_e != cudaSuccess) {                                                   \
        std::fprintf(stderr, "CUDA error %s at %s:%d\n",                       \
                     cudaGetErrorString(_e), __FILE__, __LINE__);              \
        std::exit(1);                                                          \
    }                                                                          \
} while (0)

static int env_int(const char* k, int d) { const char* v = getenv(k); return v && *v ? atoi(v) : d; }

static BT_HD int test_action(unsigned int tick, int env) {
    unsigned int h = tick * 2654435761u + (unsigned int)env * 40503u + 0x9E3779B9u;
    return (int)((h >> 13) % 9u);
}

// plain one-thread-per-env kernel (the current brotato.cu geometry), H ticks
__global__ void plain_step_kernel(BtState* __restrict__ base, size_t stride,
        const float* __restrict__ actions, float* __restrict__ obs,
        float* __restrict__ rewards, float* __restrict__ terminals, int n, int H) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    BtState* s = BTF_AT(base, stride, i);
    for (int h = 0; h < H; h++) {
        float r = 0.0f, t = 0.0f;
        bt_core_step(s, (int)actions[(size_t)h * n + i], obs + (size_t)i * BTF_OBS_SIZE, &r, &t);
        rewards[(size_t)h * n + i] = r;
        terminals[(size_t)h * n + i] = t;
    }
}

static int g_fail = 0;

static void cmp(const void* h, const void* d, size_t n, const char* what, int env, int tick) {
    if (memcmp(h, d, n) != 0) {
        const unsigned char* a = (const unsigned char*)h;
        const unsigned char* b = (const unsigned char*)d;
        size_t k = 0;
        for (; k < n; k++) if (a[k] != b[k]) break;
        std::printf("  MISMATCH %s env %d tick %d byte %zu: host=0x%02x gpu=0x%02x\n",
                    what, env, tick, k, a[k], b[k]);
        g_fail = 1;
    }
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    int zone = 0, shop_steps = 1;
    unsigned short demon = bt_character_id_from_name("demon");
    unsigned short ghost = bt_weapon_id_from_name("ghost_scepter");
    cudaDeviceProp prop;
    CUDA_OK(cudaGetDeviceProperties(&prop, 0));
    size_t freeb = 0, totalb = 0;
    CUDA_OK(cudaMemGetInfo(&freeb, &totalb));
    size_t sb = btfw_scratch_bytes();
    std::printf("device %s sm_%d%d SMs=%d free=%.2f GB  sizeof(BtState)=%zu (%.2f MiB)\n",
                prop.name, prop.major, prop.minor, prop.multiProcessorCount,
                freeb / 1073741824.0, sizeof(BtState), sizeof(BtState) / 1048576.0);
    std::printf("integrated: warp-per-env bp=32, physics backend = %s, scratch %.1f KB/env\n",
#if BTF_PHYS_HAVE_BACKEND && defined(BTFW_HAVE_PHYSICS)
                "wavefront (brotato_gpu_physics.cuh)",
#else
                "SERIAL (physics header absent -> nothing parallel armed)",
#endif
                sb / 1024.0);

    // ---- parity: warp kernel vs the host core, Demon + Ghost
    //
    // A single run cannot tell "correct" from "a race that has not fired yet"
    // (the failure modes are nondeterministic), so this drives BT_PARITY_REPEAT
    // independent trajectories (per-repeat action salt) at a residency that
    // actually overlaps blocks (default 1024 envs = 1024 concurrent warps), and
    // fails fast on the first differing byte, naming the field + env + tick.
    int pn = env_int("BT_PARITY_ENVS", 1024), pt = env_int("BT_PARITY_TICKS", 400);
    int reps = env_int("BT_PARITY_REPEAT", 3);   /* repeated seeds: default >1 */
    int maxpt = env_int("BT_PARITY_MAX_TICKS", pt * 4);  /* extend until a terminal is covered */
    {
        BtState* hs = (BtState*)malloc((size_t)pn * sizeof(BtState));
        memset(hs, 0, (size_t)pn * sizeof(BtState));
        BtState* ds = NULL;
        CUDA_OK(cudaMalloc(&ds, (size_t)pn * sizeof(BtState)));
        size_t ob = (size_t)pn * BTF_OBS_SIZE * sizeof(float);
        float *ho = (float*)malloc(ob), *doh = (float*)malloc(ob);
        float *dobs, *drew, *dterm, *dact;
        CUDA_OK(cudaMalloc(&dobs, ob));
        CUDA_OK(cudaMalloc(&drew, pn * sizeof(float)));
        CUDA_OK(cudaMalloc(&dterm, pn * sizeof(float)));
        CUDA_OK(cudaMalloc(&dact, pn * sizeof(float)));
        unsigned char* dmask = NULL;
        std::vector<unsigned char> hmask((size_t)pn * BT_ACTIONS), gmask((size_t)pn * BT_ACTIONS);
        CUDA_OK(cudaMalloc(&dmask, (size_t)pn * BT_ACTIONS));
        void* dscratch;
        CUDA_OK(cudaMalloc(&dscratch, (size_t)pn * sb));
        std::vector<BtState> db(pn);
        std::vector<float> act(pn), hrew(pn), hterm(pn), grew(pn), gterm(pn);

        int be = 1, gbp = 32;
#if defined(BTFW_HAVE_PHYSICS)
        CUDA_OK(cudaMemcpyToSymbol(btf_phys_backend, &be, sizeof(int)));
        CUDA_OK(cudaMemcpyToSymbol(btf_group_bp, &gbp, sizeof(int)));
#else
        (void)be; (void)gbp;   /* no physics header: the warp kernel has no scratch */
#endif
        std::printf("\n[parity] warp kernel (physics=%s) vs CPU core, n=%d ticks=%d reps=%d\n",
#if BTF_PHYS_HAVE_BACKEND && defined(BTFW_HAVE_PHYSICS)
                    "wavefront",
#else
                    "serial",
#endif
                    pn, pt, reps);
        int term_seen = 0;
        for (int rep = 0; rep < reps && !g_fail; rep++) {
            int term_rep = 0, first_term_tick = -1;
            /* Both sides must start from all-zero bytes on EVERY rep: bt_core_init
             * does not write every padding/derived byte, so re-initialising a host
             * state left over from the previous rep keeps stale bytes and reports a
             * false post-init mismatch against the freshly zeroed device copy
             * (observed: rep1 env0 byte 219460 host 0x92 vs gpu 0x00). */
            memset(hs, 0, (size_t)pn * sizeof(BtState));
            CUDA_OK(cudaMemset(ds, 0, (size_t)pn * sizeof(BtState)));
            btfw_init_kernel<<<(pn + 127) / 128, 128>>>(ds, sizeof(BtState), pn, zone,
                    shop_steps, demon, ghost, 1, 1);   /* audio_rng = 1: legacy source draws */
            CUDA_OK(cudaGetLastError());
            CUDA_OK(cudaDeviceSynchronize());
            for (int i = 0; i < pn; i++) {
                unsigned int rng = (unsigned int)i;
                if (rng == 0u) rng = 1u;
                bt_core_init(&hs[i], (bt_u64)rng, zone, shop_steps, demon, ghost);
            }
            CUDA_OK(cudaMemcpy(db.data(), ds, (size_t)pn * sizeof(BtState), cudaMemcpyDeviceToHost));
            for (int i = 0; i < pn && !g_fail; i++)
                cmp(&hs[i], &db[i], sizeof(BtState), "post-init", i, -1);
            unsigned int salt = 2654435761u * (unsigned)rep + 0x9E3779B9u;
            // Terminal coverage is REQUIRED, not assumed: run the nominal window
            // and, if no episode has ended in this rep, keep going (up to maxpt)
            // so the terminal/shop-reset paths are actually exercised.
            for (int t = 0; (t < pt || (!term_rep && t < maxpt)) && !g_fail; t++) {
                for (int i = 0; i < pn; i++) act[i] = (float)test_action((unsigned)t + salt, i);
                CUDA_OK(cudaMemcpy(dact, act.data(), pn * sizeof(float), cudaMemcpyHostToDevice));
                btfw_step_kernel<<<pn, 32>>>(ds, sizeof(BtState), dact, dobs, drew, dterm,
                                             pn, 1, 0, dscratch, NULL, dmask, BT_ACTIONS);
                CUDA_OK(cudaGetLastError());
                for (int i = 0; i < pn; i++) {
                    float r = 0.0f, tm = 0.0f;
                    bt_core_step(&hs[i], (int)act[i], ho + (size_t)i * BTF_OBS_SIZE, &r, &tm);
                    hrew[i] = r;
                    hterm[i] = tm;
                    if (tm != 0.0f) {
                        term_seen = 1;
                        if (!term_rep) { term_rep = 1; first_term_tick = t; }
                    }
                }
                CUDA_OK(cudaMemcpy(doh, dobs, ob, cudaMemcpyDeviceToHost));
                CUDA_OK(cudaMemcpy(grew.data(), drew, pn * sizeof(float), cudaMemcpyDeviceToHost));
                CUDA_OK(cudaMemcpy(gterm.data(), dterm, pn * sizeof(float), cudaMemcpyDeviceToHost));
                CUDA_OK(cudaMemcpy(gmask.data(), dmask, (size_t)pn * BT_ACTIONS, cudaMemcpyDeviceToHost));
                for (int i = 0; i < pn && !g_fail; i++) {
                    char w[64];
                    snprintf(w, sizeof(w), "obs[%d]", i);
                    cmp(ho + (size_t)i * BTF_OBS_SIZE, doh + (size_t)i * BTF_OBS_SIZE,
                        BTF_OBS_SIZE * sizeof(float), w, i, t);
                    cmp(&hrew[i], &grew[i], sizeof(float), "reward", i, t);
                    cmp(&hterm[i], &gterm[i], sizeof(float), "terminal", i, t);
                    bt_action_mask(&hs[i], hmask.data() + (size_t)i * BT_ACTIONS);
                    cmp(hmask.data() + (size_t)i * BT_ACTIONS,
                        gmask.data() + (size_t)i * BT_ACTIONS, BT_ACTIONS, "mask", i, t);
                }
                if (t % 50 == 0 || t == pt - 1) {
                    CUDA_OK(cudaMemcpy(db.data(), ds, (size_t)pn * sizeof(BtState),
                                       cudaMemcpyDeviceToHost));
                    for (int i = 0; i < pn && !g_fail; i++)
                        cmp(&hs[i], &db[i], sizeof(BtState), "BtState", i, t);
                }
            }
            if (!term_rep) {
                std::printf("  rep %d: NO TERMINAL within %d ticks -> terminal/shop-reset path NOT "
                            "covered; treating as FAILURE\n", rep, maxpt);
                g_fail = 1;
            }
            std::printf("  rep %d: obs/reward/terminal/mask every tick, whole BtState every 50 "
                        "| first terminal tick %d%s\n", rep, first_term_tick,
                        g_fail ? " FAIL" : " PASS");
        }
        if (!term_seen) g_fail = 1;   /* required coverage: never PASS without it */
        std::printf("  %s  (terminal coverage REQUIRED; seen: %s)\n", g_fail ? "FAIL" : "PASS",
                    term_seen ? "yes" : "NO");
        free(hs); free(ho); free(doh);
        cudaFree(ds); cudaFree(dobs); cudaFree(drew); cudaFree(dterm); cudaFree(dact);
        cudaFree(dscratch);
    }
    if (g_fail) { std::printf("PARITY FAILED\n"); return 1; }

    if (env_int("BT_BENCH", 1) == 0) return 0;

    // ---- throughput
    int want = env_int("BT_ENVS", 0);
    const size_t reserve = (size_t)512 * 1024 * 1024;
    int maxn = (int)((freeb > reserve ? freeb - reserve : freeb) / sizeof(BtState));
    int maxn_scr = (int)((freeb > reserve ? freeb - reserve : freeb) / (sizeof(BtState) + sb));
    if (maxn_scr < maxn) maxn = maxn_scr;
    if (want > 0 && want < maxn) maxn = want;
    int ticks = env_int("BT_TICKS", 60);
    std::printf("\n[throughput] Demon+Ghost, n=%d envs\n", maxn);

    BtState* dstate = NULL;
    CUDA_OK(cudaMalloc(&dstate, (size_t)maxn * sizeof(BtState)));
    CUDA_OK(cudaMemset(dstate, 0, (size_t)maxn * sizeof(BtState)));
    float *dobs, *drew, *dterm, *dact;
    CUDA_OK(cudaMalloc(&dobs, (size_t)maxn * BTF_OBS_SIZE * sizeof(float)));
    CUDA_OK(cudaMalloc(&drew, maxn * sizeof(float)));
    CUDA_OK(cudaMalloc(&dterm, maxn * sizeof(float)));
    CUDA_OK(cudaMalloc(&dact, (size_t)maxn * sizeof(float)));
    CUDA_OK(cudaMemset(dact, 0, (size_t)maxn * sizeof(float)));
    void* dscratch = NULL;
    CUDA_OK(cudaMalloc(&dscratch, (size_t)maxn * sb));
    btfw_init_kernel<<<(maxn + 127) / 128, 128>>>(dstate, sizeof(BtState), maxn, zone,
            shop_steps, demon, ghost, 1, 1);   /* audio_rng = 1: legacy source draws */
    CUDA_OK(cudaGetLastError());
    CUDA_OK(cudaDeviceSynchronize());

    int use_policy = env_int("BT_NO_POLICY", 0) ? 0 : 1;

    // time one (mode, nsub, H) configuration
    auto run = [&](int mode, int nsub, int H, int be) {
        int gbp = mode ? 32 : 1;   /* warp-per-env vs one thread per env */
#if defined(BTFW_HAVE_PHYSICS)
        CUDA_OK(cudaMemcpyToSymbol(btf_phys_backend, &be, sizeof(int)));
        CUDA_OK(cudaMemcpyToSymbol(btf_group_bp, &gbp, sizeof(int)));
#else
        (void)be; (void)gbp;
#endif
        size_t frames = (size_t)H * nsub;
        float *aobs = dobs, *arew = drew, *aterm = dterm, *aact = dact;
        bool big = H > 1;
        if (big) {
            CUDA_OK(cudaMalloc(&aobs, frames * BTF_OBS_SIZE * sizeof(float)));
            CUDA_OK(cudaMalloc(&arew, frames * sizeof(float)));
            CUDA_OK(cudaMalloc(&aterm, frames * sizeof(float)));
            CUDA_OK(cudaMalloc(&aact, frames * sizeof(float)));
            CUDA_OK(cudaMemset(aact, 0, frames * sizeof(float)));
        }
        for (int t = 0; t < 2; t++) {
            if (mode) btfw_step_kernel<<<nsub, 32>>>(dstate, sizeof(BtState), aact, aobs,
                    arew, aterm, nsub, H, use_policy, dscratch, NULL, NULL, 0);
            else plain_step_kernel<<<(nsub + 127) / 128, 128>>>(dstate, sizeof(BtState),
                    aact, aobs, arew, aterm, nsub, H);
        }
        CUDA_OK(cudaDeviceSynchronize());
        cudaEvent_t e0, e1;
        cudaEventCreate(&e0); cudaEventCreate(&e1);
        cudaEventRecord(e0);
        for (int t = 0; t < ticks / H; t++) {
            if (mode) btfw_step_kernel<<<nsub, 32>>>(dstate, sizeof(BtState), aact, aobs,
                    arew, aterm, nsub, H, use_policy, dscratch, NULL, NULL, 0);
            else plain_step_kernel<<<(nsub + 127) / 128, 128>>>(dstate, sizeof(BtState),
                    aact, aobs, arew, aterm, nsub, H);
        }
        cudaEventRecord(e1);
        CUDA_OK(cudaDeviceSynchronize());
        float ms = 0.0f;
        cudaEventElapsedTime(&ms, e0, e1);
        cudaEventDestroy(e0); cudaEventDestroy(e1);
        if (big) { cudaFree(aobs); cudaFree(arew); cudaFree(aterm); cudaFree(aact); }
        double sps = (double)nsub * (double)(ticks / H) * H / ((double)ms / 1000.0);
        return sps;
    };

    // headline: plain vs warp at the env counts Main asked for
    int targets[] = {2048, 4096, 8192};
    std::printf("  %-8s %12s %12s %8s %10s\n", "envs", "plain SPS", "warp SPS",
                "ratio", "warp ns/env");
    for (int ns : targets) {
        if (ns > maxn) {
            std::printf("  %-8d  skipped (does not fit in %.2f GB free; max %d)\n", ns,
                        freeb / 1073741824.0, maxn);
            continue;
        }
        double p = run(0, ns, 1, 0);
        double w = run(1, ns, 1, 1);
        std::printf("  %-8d %12.0f %12.0f %7.2fx %10.1f\n", ns, p, w, w / p, 1e9 / w);
    }
    std::printf("  horizon-fused at n=%d (warp):\n", maxn);
    for (int H : {16, 64}) {
        double w = run(1, maxn, H, 1);
        std::printf("    H=%-3d %10.0f env-steps/s  %8.1f ns/env-tick\n", H, w, 1e9 / w);
    }

    // physics share of the tick (serial backend, stage switch), then the warp gain
    {
        int mode_none = BTF_PHYS_NONE, mode_full = BTF_PHYS_FULL;
        CUDA_OK(cudaMemcpyToSymbol(btf_phys_mode, &mode_full, sizeof(int)));
        double pfull = run(0, maxn, 1, 0);
        CUDA_OK(cudaMemcpyToSymbol(btf_phys_mode, &mode_none, sizeof(int)));
        double pnone = run(0, maxn, 1, 0);
        CUDA_OK(cudaMemcpyToSymbol(btf_phys_mode, &mode_full, sizeof(int)));
        double wfull = run(1, maxn, 1, 1);
        std::printf("  physics split at n=%d: plain tick %.0f SPS, physics-free tick %.0f SPS "
                    "=> physics = %.1f%% of the plain tick\n", maxn, pfull, pnone,
                    100.0 * (1.0 - pfull / pnone));
        std::printf("  wavefront vs serial physics (plain kernel A/B, same n): "
                    "%.2fx  (warp tick %.0f SPS vs plain %.0f SPS = %.2fx)\n",
                    pfull / pnone, wfull, pfull, wfull / pfull);
    }

    // per-env clock64 cycles for the warp kernel
    {
        int ns = maxn < 2048 ? maxn : 2048;
        unsigned long long* dc = NULL;
        CUDA_OK(cudaMalloc(&dc, (size_t)ns * sizeof(unsigned long long)));
        for (int t = 0; t < 2; t++)
            btfw_step_kernel<<<ns, 32>>>(dstate, sizeof(BtState), dact, dobs, drew, dterm,
                                         ns, 1, use_policy, dscratch, dc, NULL, 0);
        CUDA_OK(cudaDeviceSynchronize());
        std::vector<unsigned long long> hc(ns);
        CUDA_OK(cudaMemcpy(hc.data(), dc, (size_t)ns * sizeof(unsigned long long),
                           cudaMemcpyDeviceToHost));
        unsigned long long mn = ~0ull, mx = 0, sum = 0;
        for (int i = 0; i < ns; i++) { mn = hc[i] < mn ? hc[i] : mn; mx = hc[i] > mx ? hc[i] : mx; sum += hc[i]; }
        std::printf("  warp clock64 per-env tick (n=%d): min=%llu avg=%llu max=%llu cycles\n",
                    ns, mn, sum / ns, mx);
        cudaFree(dc);
    }
    cudaFree(dstate); cudaFree(dobs); cudaFree(drew); cudaFree(dterm); cudaFree(dact);
    cudaFree(dscratch);
    std::printf("\ndone\n");
    return 0;
}
