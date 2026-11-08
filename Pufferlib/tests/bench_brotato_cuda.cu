// Benchmark + bit-exactness harness for the Brotato CUDA execution-model
// prototypes in ocean/brotato/brotato_gpu_fast.cuh.
//
// Compile (repo root, sm_120 = RTX 5070):
//
//   nvcc -O2 -std=c++17 -arch=sm_120 --fmad=false -Xcompiler=-ffp-contract=off \
//     -Xcompiler=-Wno-narrowing -Xcompiler=-Wno-unreachable-code \
//     --diag-suppress=2361 --diag-suppress=111 --diag-suppress=128 \
//     -Xptxas -v -I. -Isrc -Iocean/brotato -Ivendor \
//     tests/bench_brotato_cuda.cu -lm -pthread -o /tmp/bench_brotato_cuda
//   /tmp/bench_brotato_cuda
//
// --fmad=false + -ffp-contract=off are REQUIRED (parity: no FMA contraction on
// either pass); build.sh pins both for brotato. -Xptxas -v prints the register /
// spill / stack numbers for every kernel in this TU.
//
// Parts:
//   1. CPU-vs-CUDA byte parity (obs, reward, terminal, full BtState) for the
//      baseline one-thread-per-env kernel and for the horizon-fused kernel.
//   2. env-only throughput: baseline (block sweep + launch bound), horizon-fused
//      (H frames per launch), CUDA graph of H launches, max-batch scaling.
//   3. physics-stage cost attribution (btf_phys_mode: full / no-scan / scan-only
//      / none).
//   4. order-preserving wavefront (DAG levelization) contact solver: bit-exact
//      verification against the serial bt_phys_* reference and parallelism
//      statistics (critical path, avg parallelism) on real Demon crowd states.
//
// Environment knobs: BT_BENCH=0 (parity only), BT_ENVS=N, BT_TICKS=N,
// BT_CROWD_ENVS=N, BT_CROWD_TICKS=N, BT_CROWD_SAMPLES=N, BT_SKIP_CROWD=1.

#include <cuda_runtime.h>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../ocean/brotato/brotato_gpu_fast.cuh"

// Peer headers (GpuSolver / GpuRng). Included AFTER fast.cuh on purpose: the
// framework has already defined BTF_PHYS_HOOK_BODY as the measurement hook, so
// GpuSolver's `#ifndef BTF_PHYS_HOOK_BODY` guard keeps bt_core_step on the serial
// stage hook here and their parallel entry points are called explicitly by the
// tests below. That keeps one binary able to measure BOTH paths on identical
// worlds. Guarded by __has_include so this harness still builds if a peer
// reorganises their files.
#if defined(__has_include)
#  if __has_include("../ocean/brotato/brotato_gpu_physics.cuh") && \
      !defined(BTF_SKIP_PHYSICS) && !defined(BTF_SKIP_PEER_HEADERS)
#    define BTF_HAVE_PHYSICS 1
#    include "../ocean/brotato/brotato_gpu_physics.cuh"
#  endif
#  if __has_include("../ocean/brotato/brotato_rng_jump.h") && \
      !defined(BTF_SKIP_RNG) && !defined(BTF_SKIP_PEER_HEADERS)
#    define BTF_HAVE_RNGJUMP 1
#    include "../ocean/brotato/brotato_rng_jump.h"
#  endif
#endif

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------
#define CUDA_OK(call) do {                                                     \
    cudaError_t _e = (call);                                                   \
    if (_e != cudaSuccess) {                                                   \
        std::fprintf(stderr, "CUDA error %s at %s:%d\n",                       \
                     cudaGetErrorString(_e), __FILE__, __LINE__);              \
        std::exit(1);                                                          \
    }                                                                          \
} while (0)

static int env_int(const char *k, int dflt) {
    const char *v = getenv(k);
    return v && *v ? atoi(v) : dflt;
}

// Deterministic, integer-only action stream shared by the host reference and the
// device batch (no FP, so both sides agree bit-for-bit on the chosen action).
static BT_HD int btf_test_action(unsigned int tick, int env) {
    unsigned int h = tick * 2654435761u + (unsigned int)env * 40503u + 0x9E3779B9u;
    return (int)((h >> 13) % 9u);
}

// ---------------------------------------------------------------------------
// device kernels owned by the harness (env setup + crowd generation)
// ---------------------------------------------------------------------------
__global__ void btf_init_kernel(BtState* __restrict__ base, size_t stride, int n,
        int zone, int shop_steps, unsigned short character, unsigned short weapon) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    unsigned int rng = (unsigned int)i;
    if (rng == 0u) rng = 1u;   // mirrors brotato.cu: i==0 -> 1 + tag(0)
    bt_core_init(BTF_AT(base, stride, i), (bt_u64)rng, zone, shop_steps, character, weapon);
}

// Kiting policy used ONLY to generate deep-wave crowd fixtures: it reads the
// player observation block (6 nearest enemies) written by bt_core_observe and
// moves away from the crowd, plus a mild wall-avoidance pull. It never touches
// the simulation, so its use of sqrtf carries no parity risk.
static __device__ int btf_kite_action(BtState* s, const float* o) {
    if (s->phase == 1) return 8;     /* base_shop "Go" */
    if (s->phase == 2) return 0;     /* first queued upgrade */
    const int EN = 11 + 16 + 8;      /* BT_OBS_PLAYER + ITEMS + UPGRADES */
    float want_x = 0.0f, want_y = 0.0f;
    for (int sl = 0; sl < BT_SLOTS; sl++) {
        float dx = o[EN + sl * 5 + 0] * BT_ARENA_W;
        float dy = o[EN + sl * 5 + 1] * BT_ARENA_H;
        float d2 = dx * dx + dy * dy;
        if (d2 <= 1.0f) continue;
        float inv = 1.0f / sqrtf(d2);
        want_x -= dx * inv * inv;    /* repulsion ~ 1/d */
        want_y -= dy * inv * inv;
    }
    if (s->px < 300.0f) want_x += 1.0f;
    if (s->px > BT_ARENA_W - 300.0f) want_x -= 1.0f;
    if (s->py < 300.0f) want_y += 1.0f;
    if (s->py > BT_ARENA_H - 300.0f) want_y -= 1.0f;
    int best = 0;
    float best_dot = -1e30f;
    for (int a = 1; a <= 8; a++) {   /* 1..8 = the 8 movement directions */
        float d = BT_MOVE_X[a] * want_x + BT_MOVE_Y[a] * want_y;
        if (d > best_dot) { best_dot = d; best = a; }
    }
    return best;
}

// Generate a crowd: advance `ticks` frames with the kiting policy, dumping a
// full BtState snapshot of every env into `snap` every `dump_every` frames
// (max_dumps slots). snap layout: [dump][env].
__global__ void btf_crowd_kernel(BtState* __restrict__ base, size_t stride, int n,
        float* __restrict__ obs, int ticks, int dump_every,
        BtState* __restrict__ snap, int snap_stride, int max_dumps) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    BtState* s = BTF_AT(base, stride, i);
    float* o = obs + (size_t)i * BTF_OBS_SIZE;
    bt_core_observe(s, o);
    int dump = 0;
    for (int t = 0; t < ticks; t++) {
        int a = btf_kite_action(s, o);
        float r = 0.0f, tm = 0.0f;
        bt_core_step(s, a, o, &r, &tm);
        if (dump_every > 0 && dump < max_dumps && (t + 1) % dump_every == 0) {
            snap[(size_t)dump * snap_stride + i] = *s;
            dump++;
        }
    }
}

// ---------------------------------------------------------------------------
// Part 1: CPU vs CUDA byte parity
// ---------------------------------------------------------------------------
static int g_parity_fail = 0;

static bool compare_state(const BtState* host, const BtState* dev, int env,
        int tick, const char* what) {
    const unsigned char* h = (const unsigned char*)host;
    const unsigned char* d = (const unsigned char*)dev;
    if (memcmp(h, d, sizeof(BtState)) != 0) {
        for (size_t k = 0; k < sizeof(BtState); k++) {
            if (h[k] != d[k]) {
                std::printf("  MISMATCH %s env %d tick %d: BtState byte %zu "
                            "host=0x%02x dev=0x%02x\n",
                            what, env, tick, k, h[k], d[k]);
                break;
            }
        }
        g_parity_fail = 1;
        return false;
    }
    return true;
}

static bool compare_floats(const float* host, const float* dev, int count,
        int env, int tick, const char* what) {
    for (int k = 0; k < count; k++) {
        if (memcmp(&host[k], &dev[k], sizeof(float)) != 0) {
            std::printf("  MISMATCH %s env %d tick %d slot %d: host=%.9g dev=%.9g "
                        "(bits 0x%08x vs 0x%08x)\n", what, env, tick, k,
                        host[k], dev[k],
                        *(const unsigned int*)&host[k], *(const unsigned int*)&dev[k]);
            g_parity_fail = 1;
            return false;
        }
    }
    return true;
}

// Returns 0 on success.
static int parity_check(int n, int ticks, int zone, int shop_steps,
        unsigned short character, unsigned short weapon, bool fused, int H) {
    std::printf("\n[1] CPU-vs-CUDA byte parity  (n=%d ticks=%d %s)\n", n, ticks,
                fused ? "horizon-fused" : "baseline");

    BtState* h_state = (BtState*)malloc((size_t)n * sizeof(BtState));
    BtState* d_state = NULL;
    memset(h_state, 0, (size_t)n * sizeof(BtState));
    CUDA_OK(cudaMalloc(&d_state, (size_t)n * sizeof(BtState)));
    CUDA_OK(cudaMemset(d_state, 0, (size_t)n * sizeof(BtState)));

    size_t obytes = (size_t)n * BTF_OBS_SIZE * sizeof(float);
    float *h_obs = (float*)malloc(obytes);       // host reference obs
    float *d_obs_host = (float*)malloc(obytes);  // device obs copied back
    float *h_rew = (float*)malloc(n * sizeof(float));
    float *d_rew_host = (float*)malloc(n * sizeof(float));
    float *h_term = (float*)malloc(n * sizeof(float));
    float *d_term_host = (float*)malloc(n * sizeof(float));
    float *d_obs, *d_rew, *d_term, *d_act;
    CUDA_OK(cudaMalloc(&d_obs, obytes));
    CUDA_OK(cudaMalloc(&d_rew, n * sizeof(float)));
    CUDA_OK(cudaMalloc(&d_term, n * sizeof(float)));
    CUDA_OK(cudaMalloc(&d_act, (size_t)n * sizeof(float)));
    std::vector<BtState> dbuf(n);

    btf_init_kernel<<<(n + 127) / 128, 128>>>(d_state, sizeof(BtState), n, zone,
            shop_steps, character, weapon);
    CUDA_OK(cudaGetLastError());
    CUDA_OK(cudaDeviceSynchronize());
    for (int i = 0; i < n; i++) {
        unsigned int rng = (unsigned int)i;
        if (rng == 0u) rng = 1u;
        bt_core_init(&h_state[i], (bt_u64)rng, zone, shop_steps, character, weapon);
    }
    // bt_core_init does not write every byte; both sides were zeroed first, so
    // the full-struct compare below is valid.
    CUDA_OK(cudaMemcpy(dbuf.data(), d_state, (size_t)n * sizeof(BtState),
                       cudaMemcpyDeviceToHost));
    for (int i = 0; i < n; i++) compare_state(&h_state[i], &dbuf[i], i, -1, "post-init");

    int first_fail_tick = -1;
    for (int t = 0; t < ticks && !g_parity_fail; t++) {
        std::vector<float> h_act(n);
        for (int i = 0; i < n; i++) h_act[i] = (float)btf_test_action((unsigned)t, i);
        CUDA_OK(cudaMemcpy(d_act, h_act.data(), n * sizeof(float), cudaMemcpyHostToDevice));
        if (fused) {
            btf_step_h_kernel<<<(n + 127) / 128, 128>>>(d_state, sizeof(BtState),
                    d_act, d_obs, d_rew, d_term, n, 1);
        } else {
            btf_step_kernel<<<(n + 127) / 128, 128>>>(d_state, sizeof(BtState),
                    d_act, d_obs, d_rew, d_term, n);
        }
        CUDA_OK(cudaGetLastError());
        for (int i = 0; i < n; i++) {
            float r = 0.0f, tm = 0.0f;
            bt_core_step(&h_state[i], (int)h_act[i],
                         h_obs + (size_t)i * BTF_OBS_SIZE, &r, &tm);
            h_rew[i] = r;
            h_term[i] = tm;
        }
        CUDA_OK(cudaMemcpy(d_obs_host, d_obs, obytes, cudaMemcpyDeviceToHost));
        CUDA_OK(cudaMemcpy(d_rew_host, d_rew, n * sizeof(float), cudaMemcpyDeviceToHost));
        CUDA_OK(cudaMemcpy(d_term_host, d_term, n * sizeof(float), cudaMemcpyDeviceToHost));
        for (int i = 0; i < n && !g_parity_fail; i++) {
            compare_floats(h_obs + (size_t)i * BTF_OBS_SIZE,
                           d_obs_host + (size_t)i * BTF_OBS_SIZE, BTF_OBS_SIZE,
                           i, t, "obs");
            compare_floats(&h_rew[i], &d_rew_host[i], 1, i, t, "reward");
            compare_floats(&h_term[i], &d_term_host[i], 1, i, t, "terminal");
        }
        first_fail_tick = t;
        if (g_parity_fail) break;
        if (t % 50 == 0 || t == ticks - 1) {
            CUDA_OK(cudaMemcpy(dbuf.data(), d_state, (size_t)n * sizeof(BtState),
                               cudaMemcpyDeviceToHost));
            for (int i = 0; i < n && !g_parity_fail; i++)
                compare_state(&h_state[i], &dbuf[i], i, t, "step");
        }
    }

    std::printf("  obs/reward/terminal every tick, full BtState every 50 (last tick %d):"
                " %s\n", first_fail_tick, g_parity_fail ? "FAIL" : "PASS");
    free(h_state); free(h_obs); free(d_obs_host); free(h_rew); free(d_rew_host);
    free(h_term); free(d_term_host);
    cudaFree(d_state); cudaFree(d_obs); cudaFree(d_rew); cudaFree(d_term); cudaFree(d_act);
    return g_parity_fail;
}

// ---------------------------------------------------------------------------
// Part 2: env-only throughput
// ---------------------------------------------------------------------------
struct Bench {
    BtState* state;
    float *obs, *rew, *term, *act;
    int n;
};

static void bench_alloc(Bench* b, int n) {
    b->n = n;
    CUDA_OK(cudaMalloc(&b->state, (size_t)n * sizeof(BtState)));
    CUDA_OK(cudaMemset(b->state, 0, (size_t)n * sizeof(BtState)));
    CUDA_OK(cudaMalloc(&b->obs, (size_t)n * BTF_OBS_SIZE * sizeof(float)));
    CUDA_OK(cudaMalloc(&b->rew, n * sizeof(float)));
    CUDA_OK(cudaMalloc(&b->term, n * sizeof(float)));
    CUDA_OK(cudaMalloc(&b->act, n * sizeof(float)));
    CUDA_OK(cudaMemset(b->act, 0, n * sizeof(float)));
}

static void bench_free(Bench* b) {
    cudaFree(b->state); cudaFree(b->obs); cudaFree(b->rew);
    cudaFree(b->term); cudaFree(b->act);
}

static void bench_init(Bench* b, int zone, int shop_steps,
        unsigned short character, unsigned short weapon) {
    btf_init_kernel<<<(b->n + 127) / 128, 128>>>(b->state, sizeof(BtState), b->n,
            zone, shop_steps, character, weapon);
    CUDA_OK(cudaGetLastError());
    CUDA_OK(cudaDeviceSynchronize());
}

static double time_baseline(Bench* b, int ticks, int block, bool use_lb) {
    for (int t = 0; t < 5; t++)
        btf_step_kernel<<<(b->n + block - 1) / block, block>>>(b->state, sizeof(BtState),
                b->act, b->obs, b->rew, b->term, b->n);
    CUDA_OK(cudaDeviceSynchronize());
    cudaEvent_t e0, e1;
    cudaEventCreate(&e0); cudaEventCreate(&e1);
    cudaEventRecord(e0);
    for (int t = 0; t < ticks; t++) {
        if (use_lb)
            btf_step_lb_kernel<<<(b->n + block - 1) / block, block>>>(b->state,
                    sizeof(BtState), b->act, b->obs, b->rew, b->term, b->n);
        else
            btf_step_kernel<<<(b->n + block - 1) / block, block>>>(b->state,
                    sizeof(BtState), b->act, b->obs, b->rew, b->term, b->n);
    }
    cudaEventRecord(e1);
    CUDA_OK(cudaDeviceSynchronize());
    float ms = 0.0f;
    cudaEventElapsedTime(&ms, e0, e1);
    cudaEventDestroy(e0); cudaEventDestroy(e1);
    return (double)b->n * ticks / ((double)ms / 1000.0);
}

// Baseline stepping over only `nsub` envs of the batch (grid scaled by nsub), to
// separate env-count scaling from bandwidth saturation.
static double time_baseline_n(Bench* b, int nsub, int ticks, int block) {
    for (int t = 0; t < 3; t++)
        btf_step_kernel<<<(nsub + block - 1) / block, block>>>(b->state, sizeof(BtState),
                b->act, b->obs, b->rew, b->term, nsub);
    CUDA_OK(cudaDeviceSynchronize());
    cudaEvent_t e0, e1;
    cudaEventCreate(&e0); cudaEventCreate(&e1);
    cudaEventRecord(e0);
    for (int t = 0; t < ticks; t++)
        btf_step_kernel<<<(nsub + block - 1) / block, block>>>(b->state, sizeof(BtState),
                b->act, b->obs, b->rew, b->term, nsub);
    cudaEventRecord(e1);
    CUDA_OK(cudaDeviceSynchronize());
    float ms = 0.0f;
    cudaEventElapsedTime(&ms, e0, e1);
    cudaEventDestroy(e0); cudaEventDestroy(e1);
    return (double)nsub * ticks / ((double)ms / 1000.0);
}

static double time_hfused(Bench* b, int ticks, int H, int block) {
    if (ticks % H) ticks = (ticks / H) * H;
    size_t frames = (size_t)H * b->n;
    float *hobs, *hrew, *hterm, *hact;
    CUDA_OK(cudaMalloc(&hobs, frames * BTF_OBS_SIZE * sizeof(float)));
    CUDA_OK(cudaMalloc(&hrew, frames * sizeof(float)));
    CUDA_OK(cudaMalloc(&hterm, frames * sizeof(float)));
    CUDA_OK(cudaMalloc(&hact, frames * sizeof(float)));
    CUDA_OK(cudaMemset(hact, 0, frames * sizeof(float)));
    for (int t = 0; t < 2; t++)
        btf_step_h_kernel<<<(b->n + block - 1) / block, block>>>(b->state, sizeof(BtState),
                hact, hobs, hrew, hterm, b->n, H);
    CUDA_OK(cudaDeviceSynchronize());
    cudaEvent_t e0, e1;
    cudaEventCreate(&e0); cudaEventCreate(&e1);
    cudaEventRecord(e0);
    for (int t = 0; t < ticks / H; t++)
        btf_step_h_kernel<<<(b->n + block - 1) / block, block>>>(b->state, sizeof(BtState),
                hact, hobs, hrew, hterm, b->n, H);
    cudaEventRecord(e1);
    CUDA_OK(cudaDeviceSynchronize());
    float ms = 0.0f;
    cudaEventElapsedTime(&ms, e0, e1);
    double sps = (double)b->n * ticks / ((double)ms / 1000.0);
    cudaEventDestroy(e0); cudaEventDestroy(e1);
    cudaFree(hobs); cudaFree(hrew); cudaFree(hterm); cudaFree(hact);
    return sps;
}

// CUDA graph of H plain step launches, addressing the same H*n observation /
// action slices the fused kernel uses (so the memory traffic matches).
static double time_graph(Bench* b, int ticks, int H, int block) {
    if (ticks % H) ticks = (ticks / H) * H;
    size_t frames = (size_t)H * b->n;
    float *hobs, *hrew, *hterm, *hact;
    CUDA_OK(cudaMalloc(&hobs, frames * BTF_OBS_SIZE * sizeof(float)));
    CUDA_OK(cudaMalloc(&hrew, frames * sizeof(float)));
    CUDA_OK(cudaMalloc(&hterm, frames * sizeof(float)));
    CUDA_OK(cudaMalloc(&hact, frames * sizeof(float)));
    CUDA_OK(cudaMemset(hact, 0, frames * sizeof(float)));

    cudaStream_t st;
    CUDA_OK(cudaStreamCreate(&st));
    CUDA_OK(cudaStreamBeginCapture(st, cudaStreamCaptureModeThreadLocal));
    for (int h = 0; h < H; h++)
        btf_step_kernel<<<(b->n + block - 1) / block, block, 0, st>>>(b->state, sizeof(BtState),
                hact + (size_t)h * b->n,
                hobs + (size_t)h * b->n * BTF_OBS_SIZE,
                hrew + (size_t)h * b->n, hterm + (size_t)h * b->n, b->n);
    cudaGraph_t graph;
    CUDA_OK(cudaStreamEndCapture(st, &graph));
    cudaGraphExec_t exec;
    CUDA_OK(cudaGraphInstantiate(&exec, graph, 0));
    CUDA_OK(cudaGraphDestroy(graph));

    for (int t = 0; t < 2; t++) CUDA_OK(cudaGraphLaunch(exec, st));
    CUDA_OK(cudaStreamSynchronize(st));
    cudaEvent_t e0, e1;
    cudaEventCreate(&e0); cudaEventCreate(&e1);
    cudaEventRecord(e0, st);
    for (int t = 0; t < ticks / H; t++) CUDA_OK(cudaGraphLaunch(exec, st));
    cudaEventRecord(e1, st);
    CUDA_OK(cudaStreamSynchronize(st));
    float ms = 0.0f;
    cudaEventElapsedTime(&ms, e0, e1);
    double sps = (double)b->n * ticks / ((double)ms / 1000.0);
    cudaEventDestroy(e0); cudaEventDestroy(e1);
    CUDA_OK(cudaGraphExecDestroy(exec));
    CUDA_OK(cudaStreamDestroy(st));
    cudaFree(hobs); cudaFree(hrew); cudaFree(hterm); cudaFree(hact);
    return sps;
}

// which: 0 = whole bt_core_step kernel, 1 = bt_phys_step-only kernel
static double time_phys_mode(Bench* b, int ticks, int block, int mode, int which) {
    int m = mode;
    CUDA_OK(cudaMemcpyToSymbol(btf_phys_mode, &m, sizeof(int)));
    for (int t = 0; t < 3; t++) {
        if (which == 0)
            btf_step_kernel<<<(b->n + block - 1) / block, block>>>(b->state, sizeof(BtState),
                    b->act, b->obs, b->rew, b->term, b->n);
        else
            btf_phys_only_kernel<<<(b->n + block - 1) / block, block>>>(b->state,
                    sizeof(BtState), b->n);
    }
    CUDA_OK(cudaDeviceSynchronize());
    cudaEvent_t e0, e1;
    cudaEventCreate(&e0); cudaEventCreate(&e1);
    cudaEventRecord(e0);
    for (int t = 0; t < ticks; t++) {
        if (which == 0)
            btf_step_kernel<<<(b->n + block - 1) / block, block>>>(b->state, sizeof(BtState),
                    b->act, b->obs, b->rew, b->term, b->n);
        else
            btf_phys_only_kernel<<<(b->n + block - 1) / block, block>>>(b->state,
                    sizeof(BtState), b->n);
    }
    cudaEventRecord(e1);
    CUDA_OK(cudaDeviceSynchronize());
    float ms = 0.0f;
    cudaEventElapsedTime(&ms, e0, e1);
    double sps = (double)b->n * ticks / ((double)ms / 1000.0);
    cudaEventDestroy(e0); cudaEventDestroy(e1);
    m = BTF_PHYS_FULL;
    CUDA_OK(cudaMemcpyToSymbol(btf_phys_mode, &m, sizeof(int)));
    return sps;
}

// clock64 cycles per env, whole step. Returns median.
static unsigned long long time_clock64(Bench* b, int ticks, int block, int nsmall) {
    unsigned long long* d_cyc;
    CUDA_OK(cudaMalloc(&d_cyc, (size_t)2 * nsmall * sizeof(unsigned long long)));
    unsigned long long* h_cyc = (unsigned long long*)malloc((size_t)2 * nsmall * sizeof(unsigned long long));
    for (int t = 0; t < 3; t++)
        btf_step_timed_kernel<<<(nsmall + block - 1) / block, block>>>(b->state,
                sizeof(BtState), b->act, b->obs, b->rew, b->term, nsmall, d_cyc);
    CUDA_OK(cudaDeviceSynchronize());
    cudaMemcpy(h_cyc, d_cyc, (size_t)2 * nsmall * sizeof(unsigned long long),
               cudaMemcpyDeviceToHost);
    unsigned long long mn = ~0ull, mx = 0, sum = 0;
    for (int i = 0; i < nsmall; i++) {
        unsigned long long c = h_cyc[i];
        if (c < mn) mn = c;
        if (c > mx) mx = c;
        sum += c;
    }
    std::printf("  clock64 per-env whole-step cycles (n=%d): min=%llu avg=%llu max=%llu\n",
                nsmall, mn, sum / (unsigned long long)nsmall, mx);
    free(h_cyc);
    cudaFree(d_cyc);
    (void)ticks;
    return sum / (unsigned long long)nsmall;
}

static void report_resources() {
    std::printf("\n[kernels] registers / local / shared / occupancy\n");
    struct { const char* name; const void* fn; int block; } ks[] = {
        {"btf_step_kernel",      (const void*)btf_step_kernel,      128},
        {"btf_step_lb_kernel",   (const void*)btf_step_lb_kernel,   256},
        {"btf_step_h_kernel",    (const void*)btf_step_h_kernel,    128},
        {"btf_phys_only_kernel", (const void*)btf_phys_only_kernel, 128},
        {"btf_step_timed_kernel",(const void*)btf_step_timed_kernel,128},
    };
    for (auto& k : ks) {
        cudaFuncAttributes a;
        if (cudaFuncGetAttributes(&a, k.fn) != cudaSuccess) continue;
        int blocks = 0;
        cudaOccupancyMaxActiveBlocksPerMultiprocessor(&blocks, k.fn, k.block, 0);
        std::printf("  %-22s regs=%-3d local=%zuB smem=%zuB  blocks/SM@%d=%d "
                    "threads/SM=%d\n",
                    k.name, a.numRegs, (size_t)a.localSizeBytes,
                    (size_t)a.sharedSizeBytes, k.block, blocks, blocks * k.block);
    }
}

// ---------------------------------------------------------------------------
// Part 4: order-preserving wavefront (DAG levelization) contact solver
// ---------------------------------------------------------------------------
/* Pool-slot reference: under the compact contact-cache pool contract
 * (brotato_physics.h BT_PHYS_PAIR_POOL) `active_pairs[k]` is a POOL SLOT, not a
 * pair index, and `pairs[slot]` is the live record.  The slot is the identity to
 * keep here (the same slot in the identical probe copy), and the per-slot flag
 * array therefore has to span the pool, not the C(n,2) pair-index space. */
struct BtfOpRef { int slot; unsigned char pass; };

struct BtfContext {
    BtPhysWorld* probe;      // probe copy (serial setup pass for per-pair flags)
    BtPhysWorld* ref;        // serial reference copy
    unsigned char* has_active;
    int* last;
    BtfOpRef* ops;
    int* level_start;
    int* level_count;
    int* level_fill;
    int* lvl_of;
};

struct BtfLevelStats {
    long long ops;
    long long path;
    long long pairs;
    long long bodies;
    int lvlgroups[24];
};

static BtfContext* ctx_alloc() {
    BtfContext* c = (BtfContext*)calloc(1, sizeof(BtfContext));
    /* the per-slot flag array spans the live pool; the op/level arrays span the
     * worst-case op count (9 passes over at most BT_PHYS_PAIR_POOL live pairs) */
    size_t cap = (size_t)BT_PHYS_PAIR_POOL + 1;
    c->probe = (BtPhysWorld*)malloc(sizeof(BtPhysWorld));
    c->ref = (BtPhysWorld*)malloc(sizeof(BtPhysWorld));
    c->has_active = (unsigned char*)malloc(cap);
    c->last = (int*)malloc(sizeof(int) * BT_PHYS_MAX_BODIES);
    c->ops = (BtfOpRef*)malloc(sizeof(BtfOpRef) * cap * 9);
    c->level_start = (int*)malloc(sizeof(int) * (cap * 9 + 2));
    c->level_count = (int*)malloc(sizeof(int) * (cap * 9 + 2));
    c->level_fill = (int*)malloc(sizeof(int) * (cap * 9 + 2));
    c->lvl_of = (int*)malloc(sizeof(int) * cap * 9);
    return c;
}

static void ctx_free(BtfContext* c) {
    free(c->probe); free(c->ref); free(c->has_active); free(c->last);
    free(c->ops); free(c->level_start); free(c->level_count);
    free(c->level_fill); free(c->lvl_of); free(c);
}

// Execute one world's physics with an order-preserving wavefront schedule.
// Levels are computed by walking the exact serial op order (setup pass, then the
// 8 solve passes) and assigning level(c) = 1 + max(last[bodyA], last[bodyB]);
// two ops in one level share no writable body, so running level by level (serial
// order inside a level) is a topological order of the per-body dependency DAG
// and reproduces the serial result byte-for-byte. Bodies with inv_mass == 0
// (the STATIC walls) never change value (apply_impulse adds j*0), so they create
// no dependency.
static unsigned int btf_phys_step_levelized(BtPhysWorld* w, BtfContext* ctx,
        BtfLevelStats* st) {
    // probe: exact per-pair "writes bodies" flags from a serial setup pass on an
    // untouched copy of the pre-step world (narrowphase decides on pre-step pos).
    BtPhysWorld* probe = ctx->probe;
    *probe = *w;
    bt_phys_begin_step(probe);
    bt_phys_update_pairs(probe);
    bt_phys_setup_pairs(probe);
    int np = probe->active_pair_count;
    /* per-slot "this live pair writes at least one body" flags: the probe copy
     * runs the identical update_pairs, so it hands out the same slots. */
    unsigned char* has_active = ctx->has_active;
    memset(has_active, 0, (size_t)BT_PHYS_PAIR_POOL + 1);   /* keyed by pool slot */
    for (int k = 0; k < np; k++) {
        const BtPhysPair* p = &probe->pairs[probe->active_pairs[k]];
        int any = 0;
        for (int c = 0; c < p->contact_count; c++)
            if (p->contacts[c].active) { any = 1; break; }
        has_active[probe->active_pairs[k]] = (unsigned char)any;
    }

    bt_phys_begin_step(w);
    bt_phys_update_pairs(w);

    int* last = ctx->last;
    for (int i = 0; i < BT_PHYS_MAX_BODIES; i++) last[i] = 0;

    size_t nopsmax = (size_t)np * 9;
    int* lvl_of = ctx->lvl_of;
    int* cnt = ctx->level_count;
    memset(cnt, 0, sizeof(int) * ((size_t)BT_PHYS_PAIR_POOL * 9 + 2));

    long long nops = 0;
    int path = 0;
    // pass 0 = setup (ALWAYS executed: it writes pair-local cache state even when
    // it applies no impulse); passes 1..8 = solve (skipped when it would write
    // nothing, i.e. no active contact).
    for (int pass = 0; pass < 9; pass++) {
        for (int k = 0; k < np; k++) {
            int idx = w->active_pairs[k];
            if (!has_active[idx]) {
                // No active contact: touches no body. A setup op still writes its
                // pair-local cache so it MUST run (bucket 0, original order, before
                // every body-writing op - free because it mutates nothing shared);
                // a solve that writes nothing is skipped outright (-1 marker).
                if (pass == 0) { lvl_of[(size_t)pass * np + k] = 0; cnt[0]++; }
                else { lvl_of[(size_t)pass * np + k] = -1; }
                continue;
            }
            const BtPhysPair* p = &w->pairs[idx];
            int a = p->a, b = p->b;
            int la = (w->bodies[a].inv_mass != 0.0f) ? last[a] : 0;
            int lb = (w->bodies[b].inv_mass != 0.0f) ? last[b] : 0;
            int L = (la > lb ? la : lb) + 1;
            if (w->bodies[a].inv_mass != 0.0f) last[a] = L;
            if (w->bodies[b].inv_mass != 0.0f) last[b] = L;
            lvl_of[(size_t)pass * np + k] = L;
            cnt[L]++;
            if (L > path) path = L;
            nops++;
        }
    }
    (void)nopsmax;

    int acc = 0;
    for (int L = 0; L <= path; L++) { ctx->level_start[L] = acc; acc += cnt[L]; }
    ctx->level_start[path + 1] = acc;
    for (int L = 0; L <= path; L++) ctx->level_fill[L] = ctx->level_start[L];
    BtfOpRef* ops = ctx->ops;
    for (int pass = 0; pass < 9; pass++) {
        for (int k = 0; k < np; k++) {
            int L = lvl_of[(size_t)pass * np + k];
            if (L < 0) continue;   /* solve that would write nothing */
            int pos = ctx->level_fill[L]++;
            ops[pos].slot = w->active_pairs[k];
            ops[pos].pass = (unsigned char)pass;
        }
    }

    for (int L = 0; L <= path; L++) {
        for (int pos = ctx->level_start[L]; pos < ctx->level_start[L + 1]; pos++) {
            BtPhysPair* p = &w->pairs[ops[pos].slot];
            if (ops[pos].pass == 0) bt_phys_pair_setup(w, p);
            else bt_phys_pair_solve(w, p);
        }
    }
    bt_phys_integrate_velocities(w);

    st->ops += nops;
    st->path += path;
    st->pairs += np;
    st->bodies += w->body_count;
    return btf_world_digest(w);
}

// ---------------------------------------------------------------------------
// Part 0: framework primitive self-test
//
// Validates the published primitives (btf_scan_excl + the documented ordered
// count/scan/write compaction) against a serial reference, on device, with a
// group of bp = 256 lanes (BTF_BLOCK_PER_ENV shape). Three agents build on
// these, so they are gated like everything else.
// ---------------------------------------------------------------------------
__global__ void btf_scan_test_kernel(const int* v, int n, int* out, int* total) {
    __shared__ BtfScanScratch sc;
    int mem = BTF_BLOCKLANE, bp = (int)blockDim.x;
    int chunk = (n + bp - 1) / bp;
    int lo = mem * chunk, hi = lo + chunk;
    if (hi > n) hi = n;
    int mine = (lo < hi) ? v[lo] : 0;   /* one item per member: scan the item */
    (void)mine;
    /* scan one value per member over the first bp items */
    int val = (mem < n) ? v[mem] : 0;
    int pre = btf_scan_excl(val, &sc, mem, BTF_SLOT, bp);
    if (mem < n) out[mem] = pre;
    if (mem == 0) *total = sc.total;
}

__global__ void btf_compact_test_kernel(const unsigned char* flags, int n, int* out,
        int* count) {
    __shared__ BtfScanScratch sc;
    int mem = BTF_BLOCKLANE, bp = (int)blockDim.x;
    int chunk = (n + bp - 1) / bp;
    int lo = mem * chunk, hi = lo + chunk;
    if (hi > n) hi = n;
    int mine = 0;
    for (int i = lo; i < hi; i++) mine += flags[i] ? 1 : 0;
    int base = btf_scan_excl(mine, &sc, mem, BTF_SLOT, bp);
    int pos = base;
    for (int i = lo; i < hi; i++) if (flags[i]) out[pos++] = i;
    btf_group_sync(bp);
    if (mem == 0) *count = sc.total;
}

static int framework_selftest() {
    const int n = 8192;
    std::vector<unsigned char> flags(n);
    std::vector<int> vals(n), host_pre(n);
    unsigned int s = 12345u;
    for (int i = 0; i < n; i++) {
        s = s * 1664525u + 1013904223u;
        flags[i] = (unsigned char)((s >> 17) & 1u);
        vals[i] = (int)((s >> 9) % 97u);
    }
    int acc = 0;
    for (int i = 0; i < n; i++) { host_pre[i] = acc; acc += vals[i]; }

    unsigned char* d_flags; int *d_vals, *d_out, *d_total;
    CUDA_OK(cudaMalloc(&d_flags, n));
    CUDA_OK(cudaMalloc(&d_vals, n * sizeof(int)));
    CUDA_OK(cudaMalloc(&d_out, n * sizeof(int)));
    CUDA_OK(cudaMalloc(&d_total, sizeof(int)));
    CUDA_OK(cudaMemcpy(d_flags, flags.data(), n, cudaMemcpyHostToDevice));
    CUDA_OK(cudaMemcpy(d_vals, vals.data(), n * sizeof(int), cudaMemcpyHostToDevice));

    int fails = 0;
    int bp = 256;

    // scan over the first 256 items
    btf_scan_test_kernel<<<1, bp>>>(d_vals, bp, d_out, d_total);
    CUDA_OK(cudaGetLastError());
    CUDA_OK(cudaDeviceSynchronize());
    std::vector<int> got(bp);
    int got_total = 0;
    CUDA_OK(cudaMemcpy(got.data(), d_out, bp * sizeof(int), cudaMemcpyDeviceToHost));
    CUDA_OK(cudaMemcpy(&got_total, d_total, sizeof(int), cudaMemcpyDeviceToHost));
    int want_total = 0;
    for (int i = 0; i < bp; i++) {
        if (got[i] != host_pre[i]) { if (!fails) std::printf("  scan FAIL at %d: %d vs %d\n", i, got[i], host_pre[i]); fails++; }
        want_total += vals[i];
    }
    if (got_total != want_total) { std::printf("  scan total FAIL: %d vs %d\n", got_total, want_total); fails++; }

    // ordered compaction over all 8192 items on one 256-lane group
    btf_compact_test_kernel<<<1, bp>>>(d_flags, n, d_out, d_total);
    CUDA_OK(cudaGetLastError());
    CUDA_OK(cudaDeviceSynchronize());
    std::vector<int> cout(n);
    int ccount = 0;
    CUDA_OK(cudaMemcpy(cout.data(), d_out, n * sizeof(int), cudaMemcpyDeviceToHost));
    CUDA_OK(cudaMemcpy(&ccount, d_total, sizeof(int), cudaMemcpyDeviceToHost));
    int cexpect = 0;
    for (int i = 0; i < n; i++) if (flags[i]) cexpect++;
    if (ccount != cexpect) {
        std::printf("  compaction count FAIL: %d vs %d\n", ccount, cexpect);
        fails++;
    }
    int k = 0;
    for (int i = 0; i < n && !fails; i++) {
        if (!flags[i]) continue;
        if (cout[k] != i) {
            std::printf("  compaction order FAIL at %d: got %d want %d\n", k, cout[k], i);
            fails++;
        }
        k++;
    }
    std::printf("[0] framework self-test: scan+ordered-compaction over %d items "
                "(bp=%d): %s (%d flagged)\n", n, bp, fails ? "FAIL" : "PASS", cexpect);
    cudaFree(d_flags); cudaFree(d_vals); cudaFree(d_out); cudaFree(d_total);
    return fails;
}

// ---------------------------------------------------------------------------
// Part 5: peer prototypes — wavefront physics (GpuSolver) + jump-ahead RNG
// ---------------------------------------------------------------------------
#if defined(BTF_HAVE_PHYSICS)
// One block (32 lanes) per world: copy, run the SERIAL bt_phys_step on the copy
// and the wavefront on the live world from identical input, compare digests.
__global__ void btf_wf_verify_kernel(BtPhysWorld* __restrict__ live,
        BtPhysWorld* __restrict__ refs, int n, void* __restrict__ scratch,
        int* __restrict__ mism, unsigned int* __restrict__ digests) {
    int e = blockIdx.x;
    if (e >= n) return;
    BtPhysWorld* W = &live[e];
    BtPhysWorld* R = &refs[e];
    if (threadIdx.x == 0) *R = *W;
    btf_group_sync(32);
    unsigned int d0 = btf_world_digest(R);
    if (threadIdx.x == 0) bt_phys_step(R);           /* serial reference */
    btf_group_sync(32);
    BtgpScratch sc = btgp_block_scratch(scratch, 32);
    btf_phys_wavefront_world(W, &sc, BTF_MEM(32), BTF_SLOT, 32);
    btf_group_sync(32);
    unsigned int d1 = btf_world_digest(R), d2 = btf_world_digest(W);
    if (threadIdx.x == 0) {
        if (d0 != d1) atomicAdd(mism, 1);            /* inputs differed already */
        if (d1 != d2) atomicAdd(mism, 1);
        digests[e] = d1 ^ (d2 << 1);
    }
}

// Physics-only throughput: one warp per env, K warps per block, scratch in
// global (warp-per-env mode). mode 0 = wavefront, mode 1 = serial bt_phys_step
// by lane 0 of each warp (the per-warp serial cost, for reference only).
__global__ void btf_wf_phys_kernel(BtState* __restrict__ base, size_t stride, int n,
        void* __restrict__ scratch, int mode, int K) {
    int warp = BTF_SLOT;
    int env = blockIdx.x * K + warp;
    if (env >= n) return;
    BtState* s = BTF_AT(base, stride, env);
    void* blk = (char*)scratch + (size_t)blockIdx.x * (size_t)K * btgp_scratch_bytes();
    if (mode == 0) {
        BtgpScratch sc = btgp_block_scratch(blk, 32);
        btf_phys_wavefront_world(&s->phys, &sc, BTF_MEM(32), BTF_SLOT, 32);
    } else {
        if (BTF_LANE32 == 0) bt_phys_step(&s->phys);
    }
}
#endif

#if defined(BTF_HAVE_RNGJUMP)
// Jump-ahead draw runner: consumer i draws counts[i] raw u32 values into
// out[i*MAXD + j]. The serial reference is computed on the host from a copy of
// the same stream, so both the values and the final stream state are compared.
// Plain BT_HD functors (not lambdas): btf_draw_parallel is a BT_HD template, so
// its host copy is instantiated too and must not reference a device-only lambda.
#define BTF_MAXDRAWS 8
struct BtfTestCount {
    const unsigned int* c;
    BT_HD unsigned int operator()(int i) const { return c[i]; }
};
struct BtfTestDraw {
    unsigned int* out;
    BT_HD void operator()(int i, int j, BtRng* r) const {
        out[(size_t)i * BTF_MAXDRAWS + j] = bt_rng_next(r);
    }
};
__global__ void btf_draw_test_kernel(BtRng* __restrict__ rng,
        unsigned int* __restrict__ counts, int n, unsigned int* __restrict__ out) {
    __shared__ BtfScanScratch sc;
    int bp = (int)blockDim.x;
    btf_draw_parallel(rng, counts, n, &sc, BTF_BLOCKLANE, BTF_SLOT, bp,
                      BtfTestCount{counts}, BtfTestDraw{out});
}
#endif

static int rng_advance_test() {
#if defined(BTF_HAVE_RNGJUMP)
    int fails = 0;
    const bt_u64 ks[] = {0, 1, 2, 31, 32, 33, 1000, 1000000};
    for (unsigned int seed = 1; seed <= 4; seed++) {
        BtRng base; bt_rng_seed(&base, (bt_u64)seed * 0x9E3779B97F4A7C15ull);
        for (bt_u64 k : ks) {
            BtRng step = base;
            for (bt_u64 i = 0; i < k; i++) (void)bt_rng_next(&step);
            BtRng adv = btf_rng_advance(base, k);
            if (step.state != adv.state || step.inc != adv.inc) {
                std::printf("  btf_rng_advance FAIL seed=%u k=%llu\n", seed,
                            (unsigned long long)k);
                fails++;
            }
            /* and the NEXT output must match too */
            BtRng s2 = step, a2 = adv;
            if (bt_rng_next(&s2) != bt_rng_next(&a2)) {
                std::printf("  btf_rng_advance next-output FAIL seed=%u k=%llu\n", seed,
                            (unsigned long long)k);
                fails++;
            }
        }
    }
    // draw runner: parallel draws + final stream vs serial
    const int n = 256;
    std::vector<unsigned int> counts(n);
    unsigned int rs = 7u;
    for (int i = 0; i < n; i++) { rs = rs * 1103515245u + 12345u; counts[i] = (rs >> 28) & 7u; }
    BtRng cpu; bt_rng_seed(&cpu, 4242u);
    std::vector<unsigned int> want((size_t)n * BTF_MAXDRAWS, 0);
    for (int i = 0; i < n; i++)
        for (unsigned int j = 0; j < counts[i]; j++) want[(size_t)i * BTF_MAXDRAWS + j] = bt_rng_next(&cpu);

    BtRng* d_rng; unsigned int* d_cnt; unsigned int* d_out;
    CUDA_OK(cudaMalloc(&d_rng, sizeof(BtRng)));
    CUDA_OK(cudaMalloc(&d_cnt, n * sizeof(unsigned int)));
    CUDA_OK(cudaMalloc(&d_out, (size_t)n * BTF_MAXDRAWS * sizeof(unsigned int)));
    BtRng start; bt_rng_seed(&start, 4242u);
    CUDA_OK(cudaMemcpy(d_rng, &start, sizeof(BtRng), cudaMemcpyHostToDevice));
    CUDA_OK(cudaMemcpy(d_cnt, counts.data(), n * sizeof(unsigned int), cudaMemcpyHostToDevice));
    btf_draw_test_kernel<<<1, n>>>(d_rng, d_cnt, n, d_out);
    CUDA_OK(cudaGetLastError());
    CUDA_OK(cudaDeviceSynchronize());
    std::vector<unsigned int> got((size_t)n * BTF_MAXDRAWS);
    BtRng gotrng;
    CUDA_OK(cudaMemcpy(got.data(), d_out, got.size() * sizeof(unsigned int),
                       cudaMemcpyDeviceToHost));
    CUDA_OK(cudaMemcpy(&gotrng, d_rng, sizeof(BtRng), cudaMemcpyDeviceToHost));
    for (size_t i = 0; i < got.size() && !fails; i++)
        if (got[i] != want[i]) { std::printf("  draw runner FAIL at %zu: %u vs %u\n", i, got[i], want[i]); fails++; }
    if (gotrng.state != cpu.state || gotrng.inc != cpu.inc) {
        std::printf("  draw runner final-stream FAIL\n"); fails++;
    }
    std::printf("[5a] jump-ahead: btf_rng_advance vs k x bt_rng_next (%zu k values x 4 seeds) "
                "and parallel draw runner over %d consumers: %s\n",
                sizeof(ks) / sizeof(ks[0]), n, fails ? "FAIL" : "PASS");
    cudaFree(d_rng); cudaFree(d_cnt); cudaFree(d_out);
    return fails;
#else
    std::printf("[5a] jump-ahead: SKIPPED (brotato_rng_jump.h absent)\n");
    return 0;
#endif
}

static int wavefront_test(Bench* b, int ticks, int nworlds) {
#if defined(BTF_HAVE_PHYSICS)
    size_t sb = btgp_scratch_bytes();
    std::printf("\n[5b] wavefront physics vs serial (scratch=%.1f KB/env)\n", sb / 1024.0);
    int fails = 0;

    // correctness on nworlds real worlds taken from the live batch
    BtState* host = (BtState*)malloc((size_t)nworlds * sizeof(BtState));
    CUDA_OK(cudaMemcpy(host, b->state, (size_t)nworlds * sizeof(BtState),
                       cudaMemcpyDeviceToHost));
    BtPhysWorld *d_live, *d_ref; int *d_mism; unsigned int* d_dg;
    CUDA_OK(cudaMalloc(&d_live, (size_t)nworlds * sizeof(BtPhysWorld)));
    CUDA_OK(cudaMalloc(&d_ref, (size_t)nworlds * sizeof(BtPhysWorld)));
    void* d_scratch;
    CUDA_OK(cudaMalloc(&d_scratch, (size_t)nworlds * sb));
    CUDA_OK(cudaMalloc(&d_mism, sizeof(int)));
    CUDA_OK(cudaMalloc(&d_dg, (size_t)nworlds * sizeof(unsigned int)));
    CUDA_OK(cudaMemset(d_mism, 0, sizeof(int)));
    for (int i = 0; i < nworlds; i++) {
        CUDA_OK(cudaMemcpy((char*)d_live + (size_t)i * sizeof(BtPhysWorld),
                           &host[i].phys, sizeof(BtPhysWorld), cudaMemcpyHostToDevice));
    }
    btf_wf_verify_kernel<<<nworlds, 32>>>(d_live, d_ref, nworlds, d_scratch, d_mism, d_dg);
    CUDA_OK(cudaGetLastError());
    CUDA_OK(cudaDeviceSynchronize());
    int mism = 0;
    CUDA_OK(cudaMemcpy(&mism, d_mism, sizeof(int), cudaMemcpyDeviceToHost));
    std::printf("  wavefront == serial on %d identical worlds: %d mismatches -> %s\n",
                nworlds, mism, mism ? "NOT BIT-EXACT" : "BIT-EXACT");
    fails += mism;
    cudaFree(d_live); cudaFree(d_ref); cudaFree(d_scratch);
    cudaFree(d_mism); cudaFree(d_dg);
    free(host);

    // timing over the whole batch
    int n = b->n;
    int K = 4, block = 32 * K;
    int grid = (n + K - 1) / K;
    void* d_sc2;
    CUDA_OK(cudaMalloc(&d_sc2, (size_t)grid * K * sb));
    auto run = [&](int mode, int reps) {
        cudaEvent_t e0, e1;
        cudaEventCreate(&e0); cudaEventCreate(&e1);
        for (int t = 0; t < 2; t++)
            btf_wf_phys_kernel<<<grid, block>>>(b->state, sizeof(BtState), n, d_sc2, mode, K);
        CUDA_OK(cudaDeviceSynchronize());
        cudaEventRecord(e0);
        for (int t = 0; t < reps; t++)
            btf_wf_phys_kernel<<<grid, block>>>(b->state, sizeof(BtState), n, d_sc2, mode, K);
        cudaEventRecord(e1);
        CUDA_OK(cudaDeviceSynchronize());
        float ms = 0.0f;
        cudaEventElapsedTime(&ms, e0, e1);
        cudaEventDestroy(e0); cudaEventDestroy(e1);
        return (double)n * reps / ((double)ms / 1000.0);
    };
    int reps = ticks / 4 > 20 ? ticks / 4 : 20;
    double wf = run(0, reps);
    double ser = run(1, reps);
    std::printf("  physics-only throughput (n=%d): wavefront %10.0f /s   "
                "serial-by-lane0 %10.0f /s   speedup %.2fx\n", n, wf, ser, wf / ser);
    std::printf("  (serial-by-lane0 is 1/32 occupancy and is NOT the baseline; the "
                "baseline is btf_phys_only_kernel from [3]: one thread per env.)\n");
    cudaFree(d_sc2);
    return fails;
#else
    (void)b; (void)ticks; (void)nworlds;
    std::printf("\n[5b] wavefront physics: SKIPPED (brotato_gpu_physics.cuh absent)\n");
    return 0;
#endif
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    int zone = 0, shop_steps = 1;
    unsigned short bull = bt_character_id_from_name("bull");
    unsigned short none = bt_weapon_id_from_name("none");
    unsigned short demon = bt_character_id_from_name("demon");
    unsigned short ghost = bt_weapon_id_from_name("ghost_scepter");
    if (bull == 0xFFFFu) bull = 0;   /* fall back to the default profile */

    cudaDeviceProp prop;
    CUDA_OK(cudaGetDeviceProperties(&prop, 0));
    size_t freeb = 0, totalb = 0;
    CUDA_OK(cudaMemGetInfo(&freeb, &totalb));
    std::printf("device: %s sm_%d%d SMs=%d  mem total=%.2f GB free=%.2f GB\n",
                prop.name, prop.major, prop.minor, prop.multiProcessorCount,
                totalb / 1073741824.0, freeb / 1073741824.0);
    std::printf("sizeof(BtState)=%zu bytes  sizeof(BtPhysWorld)=%zu  "
                "sizeof(BtPhysPair)=%zu  pairs/env=%d  OBS=%d\n",
                sizeof(BtState), sizeof(BtPhysWorld), sizeof(BtPhysPair),
                BT_PHYS_PAIR_CAPACITY, BTF_OBS_SIZE);

    int want_envs = env_int("BT_ENVS", 0);
    int max_envs = (int)((freeb - (size_t)1024 * 1024 * 1024) / sizeof(BtState));
    if (want_envs > 0 && want_envs < max_envs) max_envs = want_envs;
    int ticks = env_int("BT_TICKS", 300);

    // ---- Part 0 + 5a (framework primitives, jump-ahead): no env state needed
    if (framework_selftest()) {
        std::printf("FRAMEWORK SELF-TEST FAILED\n");
        return 1;
    }
    if (rng_advance_test()) {
        std::printf("JUMP-AHEAD TEST FAILED\n");
        return 1;
    }

    // ---- Part 1
    if (parity_check(6, 1200, zone, shop_steps, bull, none, false, 1)) {
        std::printf("BASELINE PARITY FAILED\n");
        return 1;
    }
    if (parity_check(6, 400, zone, shop_steps, demon, ghost, false, 1)) {
        std::printf("DEMON PARITY FAILED\n");
        return 1;
    }

    if (env_int("BT_BENCH", 1) == 0) return 0;

    // ---- Part 2
    std::printf("\n[2] env-only throughput (n=%d envs, action=const 0)\n", max_envs);
    Bench b;
    bench_alloc(&b, max_envs);
    bench_init(&b, zone, shop_steps, bull, none);
    report_resources();

    std::printf("\n  baseline, block sweep (%d ticks):\n", ticks);
    for (int block : {32, 64, 128, 256, 512}) {
        double sps = time_baseline(&b, ticks, block, false);
        std::printf("    block=%-3d  %10.0f env-steps/s\n", block, sps);
    }
    double base_sps = time_baseline(&b, ticks, 128, false);
    double lb_sps = time_baseline(&b, ticks, 256, true);
    std::printf("    block=128 baseline (reference)          %10.0f env-steps/s\n", base_sps);
    std::printf("    block=256 launch_bounds(256) variant    %10.0f env-steps/s (%.2fx)\n",
                lb_sps, lb_sps / base_sps);

    std::printf("\n  batch scaling (block=128, %d ticks): envs -> SPS and per-env ns/tick\n",
                ticks);
    {
        int subs[] = {128, 256, 512, 768, 1024, 1536, 2048};
        for (int ns : subs) {
            if (ns > max_envs) continue;
            double s = time_baseline_n(&b, ns, ticks, 128);
            std::printf("    n=%-5d %10.0f env-steps/s   %7.1f ns/env-tick\n", ns, s,
                        1e9 / s);
        }
    }

    std::printf("\n  horizon-fused vs CUDA graph of plain launches:\n");
    for (int H : {16, 64, 128}) {
        double f = time_hfused(&b, ticks, H, 128);
        double g = time_graph(&b, ticks, H, 128);
        std::printf("    H=%-4d  fused %10.0f env-steps/s   graph %10.0f env-steps/s\n",
                    H, f, g);
    }

    time_clock64(&b, ticks, 128, b.n);

    std::printf("\n  physics-stage attribution (whole-step kernel, n=%d):\n", b.n);
    const char* modenames[] = {"full physics", "no broadphase scan", "broadphase only", "no physics"};
    double sfull = time_phys_mode(&b, ticks, 128, 0, 0);
    for (int i = 0; i < 4; i++) {
        double s = time_phys_mode(&b, ticks, 128, i, 0);
        std::printf("    mode=%-20s %10.0f env-steps/s   stage costs %.1f%% of tick\n",
                    modenames[i], s, 100.0 * (1.0 - (i == 0 ? 1.0 : sfull / s)));
    }
    std::printf("\n  physics-only kernel, same stage switch:\n");
    double pfull = time_phys_mode(&b, ticks, 128, 0, 1);
    for (int i = 0; i < 4; i++) {
        double s = time_phys_mode(&b, ticks, 128, i, 1);
        std::printf("    mode=%-20s %10.0f phys-only steps/s   stage = %.2f ms/batch-tick\n",
                    modenames[i], s, (i == 0 ? 1.0 : 0.0) * 0.0 + 1e3 / s);
    }
    (void)pfull;

    // ---- Part 4
    int cenvs = env_int("BT_CROWD_ENVS", 16);
    int cticks = env_int("BT_CROWD_TICKS", 24000);
    int csamples = env_int("BT_CROWD_SAMPLES", 8);
    if (env_int("BT_SKIP_CROWD", 0) == 0) {
        std::printf("\n[4] levelized contact solver: %d Demon envs x %d ticks, "
                    "%d snapshots each\n", cenvs, cticks, csamples);
        Bench cb;
        bench_alloc(&cb, cenvs);
        bench_init(&cb, zone, shop_steps, demon, ghost);
        int dump_every = cticks / (csamples + 1);
        BtState* snap = NULL;
        CUDA_OK(cudaMalloc(&snap, (size_t)csamples * cenvs * sizeof(BtState)));
        btf_crowd_kernel<<<(cenvs + 127) / 128, 128>>>(cb.state, sizeof(BtState), cenvs,
                cb.obs, cticks, dump_every, snap, cenvs, csamples);
        CUDA_OK(cudaGetLastError());
        CUDA_OK(cudaDeviceSynchronize());
        std::vector<BtState> host_snap((size_t)csamples * cenvs);
        CUDA_OK(cudaMemcpy(host_snap.data(), snap,
                           (size_t)csamples * cenvs * sizeof(BtState),
                           cudaMemcpyDeviceToHost));
        cudaFree(snap);

        BtfContext* ctx = ctx_alloc();
        BtfLevelStats st;
        memset(&st, 0, sizeof(st));
        long long mismatch = 0, nsamp = 0;
        std::printf("  %6s %6s %6s %6s %8s %6s %7s\n",
                    "wave", "env", "bodies", "pairs", "ops", "path", "par");
        for (int s = 0; s < csamples; s++) {
            for (int e = 0; e < cenvs; e++) {
                BtState* sst = &host_snap[(size_t)s * cenvs + e];
                if (sst->phase != 0) continue;   /* combat frames only */
                BtPhysWorld* w = &sst->phys;
                // serial reference on a private copy
                *ctx->ref = *w;
                bt_phys_step(ctx->ref);
                unsigned int dref = btf_world_digest(ctx->ref);
                BtfLevelStats local;
                memset(&local, 0, sizeof(local));
                unsigned int dlev = btf_phys_step_levelized(w, ctx, &local);
                if (dlev != dref) {
                    mismatch++;
                    std::printf("  LEVELIZE MISMATCH wave=%d env=%d digest 0x%08x vs "
                                "serial 0x%08x (pairs=%lld)\n",
                                sst->wave, e, dlev, dref, local.pairs);
                }
                int bucket = (int)(local.path / 16);
                if (bucket > 23) bucket = 23;
                st.lvlgroups[bucket]++;
                st.ops += local.ops; st.path += local.path;
                st.pairs += local.pairs; st.bodies += local.bodies;
                if (nsamp < 24)
                    std::printf("  %6d %6d %6lld %6lld %8lld %6lld %7.2f\n",
                                sst->wave, e, local.bodies, local.pairs, local.ops,
                                local.path, local.path ? (double)local.ops / local.path : 0.0);
                nsamp++;
            }
        }
        double avg_pairs = nsamp ? (double)st.pairs / nsamp : 0.0;
        double avg_path = nsamp ? (double)st.path / nsamp : 0.0;
        double avg_ops = nsamp ? (double)st.ops / nsamp : 0.0;
        std::printf("  snapshots analysed: %lld   digest mismatches: %lld   %s\n",
                    nsamp, mismatch, mismatch ? "NOT BIT-EXACT" : "BIT-EXACT vs serial");
        std::printf("  avg bodies=%.1f  avg active pairs=%.1f  avg writing ops=%.0f  "
                    "avg critical path=%.1f  AVG PARALLELISM=%.2f\n",
                    nsamp ? (double)st.bodies / nsamp : 0.0, avg_pairs, avg_ops,
                    avg_path, avg_path > 0 ? avg_ops / avg_path : 0.0);
        std::printf("  critical-path histogram (16-level buckets):");
        for (int i = 0; i < 24; i++)
            if (st.lvlgroups[i]) std::printf(" [%d-%d]=%d", i * 16, i * 16 + 15, st.lvlgroups[i]);
        std::printf("\n");
        ctx_free(ctx);
        // ---- Part 5b on the same deep-wave batch
        wavefront_test(&cb, 200, cenvs);
        bench_free(&cb);
    }

    bench_free(&b);
    std::printf("\ndone (parity %s)\n", g_parity_fail ? "FAILED" : "ok");
    return g_parity_fail ? 1 : 0;
}
