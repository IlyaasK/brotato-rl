// Minimal Brotato CUDA baseline harness: CPU-vs-CUDA bit-exactness, env-only
// throughput (block sweep, horizon-fused, batch scaling), per-env clock64
// cycles, ptxas/occupancy resources, physics-stage attribution, and the
// host-side order-preserving wavefront (DAG levelization) parallelism study.
//
// This is the compact sibling of tests/bench_brotato_cuda.cu: it includes ONLY
// the framework and four kernels, so it compiles in a fraction of the time when
// the machine is loaded. Same measurements, same verification standard.
//
// BUILD BLOCKER + FIX (measured twice at -O2 and -O1, clang and nvcc):
//   ptxas error: File uses too much global constant data (0x4ade0 bytes, 0x10000 max)
// The five __global__ kernels below each inline the whole core, and the core's
// generated tables are `static __device__ const`, so the constant bank is emitted
// ~5x over (306,656 B vs the 65,536 B ceiling: one instantiation is ~61 KB).
// nvcc's cicc SIGSEGVs on this TU; clang's ptxas rejects it cleanly. The -O level
// does NOT matter (confirmed at -O2 and -O1), so it is not a memory/opt issue.
// FIX: keep bm_step_kernel + bm_init_kernel only and guard the timed / phys-only /
// crowd variants (definitions AND their call sites in main) behind a macro, e.g.
// -DBMF_SLIM_TU, then build with it. Triage with `cuobjdump -res-usage <cubin>` and
// read CONSTANT[n]; ptxas's `bytes gmem` says nothing about this failure mode.
//
// Compile (repo root):
//   nvcc -O2 -std=c++17 -arch=sm_120 --fmad=false -Xcompiler=-ffp-contract=off \
//     -Xcompiler=-Wno-narrowing -Xcompiler=-Wno-unreachable-code \
//     --diag-suppress=2361 --diag-suppress=111 --diag-suppress=128 \
//     -Xptxas -v -I. -Isrc -Iocean/brotato -Ivendor \
//     tests/bench_brotato_min.cu -lm -Xlinker=-lpthread -o /tmp/bench_min
//   BT_ENVS=1536 BT_TICKS=100 BT_CROWD_ENVS=8 BT_CROWD_TICKS=8000 /tmp/bench_min
//
// --fmad=false + -ffp-contract=off are REQUIRED for parity (no FMA contraction
// on either pass); build.sh pins both for brotato.

#include <cuda_runtime.h>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// ---------------------------------------------------------------------------
// Self-contained shim (deliberate): this harness does NOT include
// brotato_gpu_framework.cuh, whose in-flight churn has blocked this measurement
// three times. It needs only four things from it - the physics-stage switch, the
// strided BtState addressing helper, a world digest, and the device-pass name
// stubs - so they live here, defined against the shared core alone. The
// framework remains the home of the real dispatcher/lane contract; this file is a
// baseline measurement tool and must stay buildable independently of it.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Two build modes, same harness, same core revision — this is the A/B that
// isolates the lane-guard cost:
//   default            : framework-free, includes brotato_core.h alone, so the
//                        core's own #ifndef fallbacks make BT_SERIAL/BT_COOP the
//                        native identity (no barrier arms, no leader test).
//   -DBMF_WITH_FRAMEWORK: includes brotato_gpu_framework.cuh, which defines the
//                        runtime-guarded forms (btf_group_bp is a mutable device
//                        global, so the leader test and the barrier arms stay
//                        live) — the plain-path regression PerfTrainer measured.
// Everything else (envs, ticks, parity oracle, SPS) is identical, so the delta is
// the guards and nothing else.
// ---------------------------------------------------------------------------
#if !defined(BMF_WITH_FRAMEWORK)
#ifndef BT_CUDA
#define BT_CUDA 1
#endif
#include "brotato_rng.h"
#include "brotato_state.h"

#define BMF_MODE_FULL      0
#define BMF_MODE_NO_SCAN   1
#define BMF_MODE_SCAN_ONLY 2
#define BMF_MODE_NONE      3
__device__ int bmf_phys_mode = BMF_MODE_FULL;
// DebugService.invulnerable: the sustained-crowd instrument. The CPU path sets
// BtState.invulnerable from the optional `invulnerable` [env] key AFTER
// bt_core_init (brotato.h:508), so the device init must do the same; without it
// every GPU row is reset-dominated (a fresh 10-hp Demon dies ~250 ticks in).
// Armed from the host before the timing/crowd batches; the parity check keeps
// the default 0 so its host/device comparison is untouched.
__device__ int bmf_invuln = 0;

BT_HD static inline void bmf_phys_hook(BtPhysWorld* w) {
#ifdef __CUDA_ARCH__
    int m = bmf_phys_mode;
#else
    int m = BMF_MODE_FULL;
#endif
    if (m == BMF_MODE_NONE) return;
    bt_phys_begin_step(w);
    if (m == BMF_MODE_FULL || m == BMF_MODE_SCAN_ONLY) bt_phys_update_pairs(w);
    if (m == BMF_MODE_SCAN_ONLY) return;
    bt_phys_setup_pairs(w);
    bt_phys_solve_pairs(w);
    bt_phys_integrate_velocities(w);
}
// Substitutes the one bt_phys_step call site in the core with the stage switch.
#define bt_phys_step(w) bmf_phys_hook(w)
#include "brotato_core.h"
#undef bt_phys_step

#define BMF_OBS_SIZE BT_OBS_SIZE
#define BMF_AT(base, stride, i) \
    ((BtState*)((char*)(base) + (size_t)(i) * (size_t)(stride)))

// nvcc parses host code in the device pass, where BT_HOST_ONLY has erased the
// generated name resolvers; these satisfy that parse (host paths resolve them).
#if defined(__CUDA_ARCH__)
static inline unsigned short bt_character_id_from_name(const char* name) { (void)name; return 0xFFFFu; }
static inline unsigned short bt_weapon_id_from_name(const char* name) { (void)name; return 0xFFFFu; }
static inline const char* bt_stat_display_name(int stat) { (void)stat; return "?"; }
#endif

BT_HD static inline unsigned int bmf_fnv1a(const unsigned char* p, size_t n,
        unsigned int h) {
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned int)p[i];
        h *= 16777619u;
    }
    return h;
}
// Whole-struct digest: the world is a POD and serial/parallel copies start as
// byte-identical memcpys, so hashing every byte (padding included) is a valid
// equality test and cannot go stale when the layout changes.
BT_HD static inline unsigned int bmf_world_digest(const BtPhysWorld* w) {
    return bmf_fnv1a((const unsigned char*)w, sizeof(BtPhysWorld), 2166136261u);
}

#else  /* -DBMF_WITH_FRAMEWORK: the guarded path, same harness */
#include "brotato_gpu_framework.cuh"
#define BMF_OBS_SIZE BTF_OBS_SIZE
#define BMF_AT(base, stride, i) BTF_AT(base, stride, i)
#define bmf_phys_mode btf_phys_mode
#define bmf_world_digest(w) btf_world_digest(w)
#define BMF_MODE_FULL BTF_PHYS_FULL
#define BMF_MODE_NO_SCAN BTF_PHYS_NO_SCAN
#define BMF_MODE_SCAN_ONLY BTF_PHYS_SCAN_ONLY
#define BMF_MODE_NONE BTF_PHYS_NONE
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

// Integer-only deterministic action stream (identical on host and device).
static BT_HD int test_action(unsigned int tick, int env) {
    unsigned int h = tick * 2654435761u + (unsigned int)env * 40503u + 0x9E3779B9u;
    return (int)((h >> 13) % 9u);
}

// ---------------------------------------------------------------------------
// kernels
// ---------------------------------------------------------------------------
__global__ void bm_init_kernel(BtState* __restrict__ base, size_t stride, int n,
        int zone, int shop_steps, unsigned short character, unsigned short weapon) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    unsigned int rng = (unsigned int)i;
    if (rng == 0u) rng = 1u;   // mirrors brotato.cu: i==0 -> 1 + tag(0)
    bt_core_init(BMF_AT(base, stride, i), (bt_u64)rng, zone, shop_steps, character, weapon);
    // Matches the CPU path (brotato.h:508): set AFTER init, from the host key.
    BMF_AT(base, stride, i)->invulnerable = (unsigned char)(bmf_invuln ? 1 : 0);
}

// H ticks per launch (H == 1 is the plain baseline). actions/obs/rew/term are
// [H][n]; obs is written at idx*BMF_OBS_SIZE.
__global__ void bm_step_kernel(BtState* __restrict__ base, size_t stride,
        const float* __restrict__ actions, float* __restrict__ obs,
        float* __restrict__ rewards, float* __restrict__ terminals, int n, int H) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    BtState* s = BMF_AT(base, stride, i);
    for (int h = 0; h < H; h++) {
        size_t idx = (size_t)h * (size_t)n + (size_t)i;
        float r = 0.0f, t = 0.0f;
        bt_core_step(s, (int)actions[idx], obs + idx * BMF_OBS_SIZE, &r, &t);
        rewards[idx] = r;
        terminals[idx] = t;
    }
}

__global__ void bm_step_timed_kernel(BtState* __restrict__ base, size_t stride,
        const float* __restrict__ actions, float* __restrict__ obs,
        float* __restrict__ rewards, float* __restrict__ terminals, int n,
        unsigned long long* __restrict__ cycles) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    BtState* s = BMF_AT(base, stride, i);
    float r = 0.0f, t = 0.0f;
    unsigned long long c0 = clock64();
    bt_core_step(s, (int)actions[i], obs + (size_t)i * BMF_OBS_SIZE, &r, &t);
    unsigned long long c1 = clock64();
    rewards[i] = r;
    terminals[i] = t;
    cycles[i] = c1 - c0;
}

__global__ void bm_phys_only_kernel(BtState* __restrict__ base, size_t stride, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    bt_phys_step(&BMF_AT(base, stride, i)->phys);
}

// Kiting policy, fixture generation only (reads the enemy observation block).
static __device__ int kite_action(BtState* s, const float* o) {
    if (s->phase == 1) return 8;     /* base_shop "Go" */
    if (s->phase == 2) return 0;     /* first queued upgrade */
    const int EN = 11 + 16 + 8;
    float wx = 0.0f, wy = 0.0f;
    for (int sl = 0; sl < BT_SLOTS; sl++) {
        float dx = o[EN + sl * 5 + 0] * BT_ARENA_W;
        float dy = o[EN + sl * 5 + 1] * BT_ARENA_H;
        float d2 = dx * dx + dy * dy;
        if (d2 <= 1.0f) continue;
        float inv = 1.0f / sqrtf(d2);
        wx -= dx * inv * inv;
        wy -= dy * inv * inv;
    }
    if (s->px < 300.0f) wx += 1.0f;
    if (s->px > BT_ARENA_W - 300.0f) wx -= 1.0f;
    if (s->py < 300.0f) wy += 1.0f;
    if (s->py > BT_ARENA_H - 300.0f) wy -= 1.0f;
    int best = 0;
    float bd = -1e30f;
    for (int a = 1; a <= 8; a++) {
        float d = BT_MOVE_X[a] * wx + BT_MOVE_Y[a] * wy;
        if (d > bd) { bd = d; best = a; }
    }
    return best;
}

__global__ void bm_crowd_kernel(BtState* __restrict__ base, size_t stride, int n,
        float* __restrict__ obs, int ticks, int dump_every, BtState* __restrict__ snap,
        int snap_stride, int max_dumps) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    BtState* s = BMF_AT(base, stride, i);
    float* o = obs + (size_t)i * BMF_OBS_SIZE;
    bt_core_observe(s, o);
    int dump = 0;
    for (int t = 0; t < ticks; t++) {
        int a = kite_action(s, o);
        float r = 0.0f, tm = 0.0f;
        bt_core_step(s, a, o, &r, &tm);
        if (dump_every > 0 && dump < max_dumps && (t + 1) % dump_every == 0) {
            snap[(size_t)dump * snap_stride + i] = *s;
            dump++;
        }
    }
}

// ---------------------------------------------------------------------------
// CPU vs CUDA byte parity
// ---------------------------------------------------------------------------
static int g_fail = 0;

static void cmp_bytes(const void* h, const void* d, size_t n, const char* what,
        int env, int tick) {
    if (memcmp(h, d, n) != 0) {
        const unsigned char* a = (const unsigned char*)h;
        const unsigned char* b = (const unsigned char*)d;
        for (size_t k = 0; k < n; k++) {
            if (a[k] != b[k]) {
                std::printf("  MISMATCH %s env %d tick %d byte %zu: host=0x%02x dev=0x%02x\n",
                            what, env, tick, k, a[k], b[k]);
                break;
            }
        }
        g_fail = 1;
    }
}

static int parity_check(int n, int ticks, int zone, int shop_steps,
        unsigned short character, unsigned short weapon) {
    std::printf("\n[parity] CPU vs CUDA, n=%d ticks=%d\n", n, ticks);
    BtState* hs = (BtState*)malloc((size_t)n * sizeof(BtState));
    BtState* ds = NULL;
    memset(hs, 0, (size_t)n * sizeof(BtState));
    CUDA_OK(cudaMalloc(&ds, (size_t)n * sizeof(BtState)));
    CUDA_OK(cudaMemset(ds, 0, (size_t)n * sizeof(BtState)));
    size_t obytes = (size_t)n * BMF_OBS_SIZE * sizeof(float);
    float *ho = (float*)malloc(obytes), *doh = (float*)malloc(obytes);
    float hrew[64], dresh[64], hterm[64], dtesh[64];
    float *dobs, *drew, *dterm, *dact;
    CUDA_OK(cudaMalloc(&dobs, obytes));
    CUDA_OK(cudaMalloc(&drew, n * sizeof(float)));
    CUDA_OK(cudaMalloc(&dterm, n * sizeof(float)));
    CUDA_OK(cudaMalloc(&dact, n * sizeof(float)));
    std::vector<BtState> db(n);

    bm_init_kernel<<<(n + 127) / 128, 128>>>(ds, sizeof(BtState), n, zone, shop_steps,
            character, weapon);
    CUDA_OK(cudaGetLastError());
    CUDA_OK(cudaDeviceSynchronize());
    for (int i = 0; i < n; i++) {
        unsigned int rng = (unsigned int)i;
        if (rng == 0u) rng = 1u;
        bt_core_init(&hs[i], (bt_u64)rng, zone, shop_steps, character, weapon);
    }
    // both sides were zeroed before init, so a whole-struct compare is valid
    // (bt_core_init does not write every padding byte).
    CUDA_OK(cudaMemcpy(db.data(), ds, (size_t)n * sizeof(BtState), cudaMemcpyDeviceToHost));
    for (int i = 0; i < n && !g_fail; i++)
        cmp_bytes(&hs[i], &db[i], sizeof(BtState), "post-init", i, -1);

    for (int t = 0; t < ticks && !g_fail; t++) {
        std::vector<float> act(n);
        for (int i = 0; i < n; i++) act[i] = (float)test_action((unsigned)t, i);
        CUDA_OK(cudaMemcpy(dact, act.data(), n * sizeof(float), cudaMemcpyHostToDevice));
        bm_step_kernel<<<(n + 127) / 128, 128>>>(ds, sizeof(BtState), dact, dobs, drew,
                dterm, n, 1);
        CUDA_OK(cudaGetLastError());
        for (int i = 0; i < n; i++) {
            float r = 0.0f, tm = 0.0f;
            bt_core_step(&hs[i], (int)act[i], ho + (size_t)i * BMF_OBS_SIZE, &r, &tm);
            hrew[i] = r;
            hterm[i] = tm;
        }
        CUDA_OK(cudaMemcpy(doh, dobs, obytes, cudaMemcpyDeviceToHost));
        CUDA_OK(cudaMemcpy(dresh, drew, n * sizeof(float), cudaMemcpyDeviceToHost));
        CUDA_OK(cudaMemcpy(dtesh, dterm, n * sizeof(float), cudaMemcpyDeviceToHost));
        for (int i = 0; i < n && !g_fail; i++) {
            char w[64];
            snprintf(w, sizeof(w), "obs[%d]", i);
            cmp_bytes(ho + (size_t)i * BMF_OBS_SIZE, doh + (size_t)i * BMF_OBS_SIZE,
                      BMF_OBS_SIZE * sizeof(float), w, i, t);
            cmp_bytes(&hrew[i], &dresh[i], sizeof(float), "reward", i, t);
            cmp_bytes(&hterm[i], &dtesh[i], sizeof(float), "terminal", i, t);
        }
        if (t % 50 == 0 || t == ticks - 1) {
            CUDA_OK(cudaMemcpy(db.data(), ds, (size_t)n * sizeof(BtState),
                               cudaMemcpyDeviceToHost));
            for (int i = 0; i < n && !g_fail; i++)
                cmp_bytes(&hs[i], &db[i], sizeof(BtState), "BtState", i, t);
        }
    }
    std::printf("  obs+reward+terminal every tick, whole BtState every 50 ticks: %s\n",
                g_fail ? "FAIL" : "PASS");
    free(hs); free(ho); free(doh);
    cudaFree(ds); cudaFree(dobs); cudaFree(drew); cudaFree(dterm); cudaFree(dact);
    return g_fail;
}

// ---------------------------------------------------------------------------
// throughput
// ---------------------------------------------------------------------------
struct Batch {
    BtState* state;
    float *obs, *rew, *term, *act;
    int n;
};

static void batch_alloc(Batch* b, int n) {
    b->n = n;
    CUDA_OK(cudaMalloc(&b->state, (size_t)n * sizeof(BtState)));
    CUDA_OK(cudaMemset(b->state, 0, (size_t)n * sizeof(BtState)));
    CUDA_OK(cudaMalloc(&b->obs, (size_t)n * BMF_OBS_SIZE * sizeof(float)));
    CUDA_OK(cudaMalloc(&b->rew, n * sizeof(float)));
    CUDA_OK(cudaMalloc(&b->term, n * sizeof(float)));
    CUDA_OK(cudaMalloc(&b->act, n * sizeof(float)));
    CUDA_OK(cudaMemset(b->act, 0, n * sizeof(float)));
}

static void batch_init(Batch* b, int zone, int shop_steps, unsigned short ch,
        unsigned short wp) {
    bm_init_kernel<<<(b->n + 127) / 128, 128>>>(b->state, sizeof(BtState), b->n, zone,
            shop_steps, ch, wp);
    CUDA_OK(cudaGetLastError());
    CUDA_OK(cudaDeviceSynchronize());
}

static double time_steps(Batch* b, int nsub, int ticks, int block, int H) {
    if (ticks % H) ticks = (ticks / H) * H;
    size_t frames = (size_t)H * nsub;
    float *hobs = b->obs, *hrew = b->rew, *hterm = b->term, *hact = b->act;
    float *aobs = NULL, *arew = NULL, *aterm = NULL, *aact = NULL;
    if (H > 1) {
        CUDA_OK(cudaMalloc(&aobs, frames * BMF_OBS_SIZE * sizeof(float)));
        CUDA_OK(cudaMalloc(&arew, frames * sizeof(float)));
        CUDA_OK(cudaMalloc(&aterm, frames * sizeof(float)));
        CUDA_OK(cudaMalloc(&aact, frames * sizeof(float)));
        CUDA_OK(cudaMemset(aact, 0, frames * sizeof(float)));
        hobs = aobs; hrew = arew; hterm = aterm; hact = aact;
    }
    for (int t = 0; t < 2; t++)
        bm_step_kernel<<<(nsub + block - 1) / block, block>>>(b->state, sizeof(BtState),
                hact, hobs, hrew, hterm, nsub, H);
    CUDA_OK(cudaDeviceSynchronize());
    cudaEvent_t e0, e1;
    cudaEventCreate(&e0); cudaEventCreate(&e1);
    cudaEventRecord(e0);
    for (int t = 0; t < ticks / H; t++)
        bm_step_kernel<<<(nsub + block - 1) / block, block>>>(b->state, sizeof(BtState),
                hact, hobs, hrew, hterm, nsub, H);
    cudaEventRecord(e1);
    CUDA_OK(cudaDeviceSynchronize());
    float ms = 0.0f;
    cudaEventElapsedTime(&ms, e0, e1);
    cudaEventDestroy(e0); cudaEventDestroy(e1);
    if (H > 1) { cudaFree(aobs); cudaFree(arew); cudaFree(aterm); cudaFree(aact); }
    return (double)nsub * ticks / ((double)ms / 1000.0);
}

static void report_resources() {
    std::printf("\n[resources] ptxas / occupancy\n");
    struct { const char* n; const void* f; int blk; } ks[] = {
        {"bm_step_kernel",        (const void*)bm_step_kernel,        128},
        {"bm_step_kernel(256)",   (const void*)bm_step_kernel,        256},
        {"bm_step_timed_kernel",  (const void*)bm_step_timed_kernel,  128},
        {"bm_phys_only_kernel",   (const void*)bm_phys_only_kernel,   128},
    };
    int sms = 0, dev = 0;
    cudaGetDevice(&dev);
    cudaDeviceGetAttribute(&sms, cudaDevAttrMultiProcessorCount, dev);
    for (auto& k : ks) {
        cudaFuncAttributes a;
        if (cudaFuncGetAttributes(&a, k.f) != cudaSuccess) continue;
        int blocks = 0;
        cudaOccupancyMaxActiveBlocksPerMultiprocessor(&blocks, k.f, k.blk, 0);
        std::printf("  %-22s regs=%-3d local=%zuB smem=%zuB blocks/SM@%d=%d threads/SM=%d "
                    "(SM count %d)\n", k.n, a.numRegs, (size_t)a.localSizeBytes,
                    (size_t)a.sharedSizeBytes, k.blk, blocks, blocks * k.blk, sms);
    }
}

// ---------------------------------------------------------------------------
// host-side order-preserving wavefront (DAG levelization) study
//
// level(op) = 1 + max(last[a], last[b]) over the MUTABLE bodies the op touches
// (inv_mass == 0 never changes value, so it contributes no edge). Ops at the same
// level share no writable body, so level-by-level execution is a topological
// order of the per-object dependency DAG: the same result as the serial order.
// ---------------------------------------------------------------------------
struct Op { int slot; unsigned char pass; };

static int analyse_world(BtPhysWorld* w, BtPhysWorld* scratch, unsigned char* has_active,
        int* last, Op* ops, int* lvl_of, int* cnt, int* lstart, int* lfill,
        long long* st_ops, long long* st_path, int* st_pairs, int* st_bodies) {
    size_t cap = (size_t)BT_PHYS_PAIR_POOL + 1;
    // probe: per-slot "this pair writes bodies" flags from a serial setup pass
    *scratch = *w;
    bt_phys_begin_step(scratch);
    bt_phys_update_pairs(scratch);
    bt_phys_setup_pairs(scratch);
    int np = scratch->active_pair_count;
    memset(has_active, 0, cap);
    for (int k = 0; k < np; k++) {
        const BtPhysPair* p = &scratch->pairs[scratch->active_pairs[k]];
        int any = 0;
        for (int c = 0; c < p->contact_count; c++)
            if (p->contacts[c].active) { any = 1; break; }
        has_active[scratch->active_pairs[k]] = (unsigned char)any;
    }

    bt_phys_begin_step(w);
    bt_phys_update_pairs(w);

    for (int i = 0; i < BT_PHYS_MAX_BODIES; i++) last[i] = 0;
    memset(cnt, 0, sizeof(int) * (cap * 9 + 2));
    long long nops = 0;
    int path = 0;
    for (int pass = 0; pass < 9; pass++) {
        for (int k = 0; k < np; k++) {
            int slot = w->active_pairs[k];
            unsigned char wr = has_active[slot];
            if (!wr) {
                // No active contact: this op touches no body. A setup op still
                // writes its own pair-local cache, so it MUST run - bucket 0, in
                // original order, before every body-writing op (order is free
                // because it mutates nothing shared). A solve op that writes
                // nothing is skipped outright (-1 marker).
                if (pass == 0) { lvl_of[(size_t)pass * np + k] = 0; cnt[0]++; }
                else { lvl_of[(size_t)pass * np + k] = -1; }
                continue;
            }
            const BtPhysPair* p = &w->pairs[slot];
            int a = p->a, b = p->b;
            int fa = w->bodies[a].inv_mass != 0.0f;
            int fb = w->bodies[b].inv_mass != 0.0f;
            int la = fa ? last[a] : 0, lb = fb ? last[b] : 0;
            int L = (la > lb ? la : lb) + 1;
            if (fa) last[a] = L;
            if (fb) last[b] = L;
            lvl_of[(size_t)pass * np + k] = L;
            cnt[L]++;
            if (L > path) path = L;
            nops++;
        }
    }
    int acc = 0;
    for (int L = 0; L <= path; L++) { lstart[L] = acc; acc += cnt[L]; }
    lstart[path + 1] = acc;
    for (int L = 0; L <= path; L++) lfill[L] = lstart[L];
    for (int pass = 0; pass < 9; pass++) {
        for (int k = 0; k < np; k++) {
            int L = lvl_of[(size_t)pass * np + k];
            if (L < 0) continue;   /* solve that would write nothing */
            int pos = lfill[L]++;
            ops[pos].slot = w->active_pairs[k];
            ops[pos].pass = (unsigned char)pass;
        }
    }
    for (int L = 0; L <= path; L++) {
        for (int pos = lstart[L]; pos < lstart[L + 1]; pos++) {
            BtPhysPair* p = &w->pairs[ops[pos].slot];
            if (ops[pos].pass == 0) bt_phys_pair_setup(w, p);
            else bt_phys_pair_solve(w, p);
        }
    }
    bt_phys_integrate_velocities(w);
    *st_ops += nops;
    *st_path += path;
    *st_pairs += np;
    *st_bodies += w->body_count;
    return path;
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    int zone = 0, shop_steps = 1;
    unsigned short bull = bt_character_id_from_name("bull");
    unsigned short none = bt_weapon_id_from_name("none");
    unsigned short demon = bt_character_id_from_name("demon");
    unsigned short ghost = bt_weapon_id_from_name("ghost_scepter");

    cudaDeviceProp prop;
    CUDA_OK(cudaGetDeviceProperties(&prop, 0));
    size_t freeb = 0, totalb = 0;
    CUDA_OK(cudaMemGetInfo(&freeb, &totalb));
    std::printf("device %s sm_%d%d SMs=%d  free=%.2f GB\n", prop.name, prop.major,
                prop.minor, prop.multiProcessorCount, freeb / 1073741824.0);
    std::printf("sizeof(BtState)=%zu (%.2f MiB)  sizeof(BtPhysWorld)=%zu (%.1f KiB)  "
                "sizeof(BtPhysPair)=%zu  PAIR_POOL=%d  PAIR_CAPACITY=%d  OBS=%d\n",
                sizeof(BtState), sizeof(BtState) / 1048576.0, sizeof(BtPhysWorld),
                sizeof(BtPhysWorld) / 1024.0, sizeof(BtPhysPair), BT_PHYS_PAIR_POOL,
                BT_PHYS_PAIR_CAPACITY, BMF_OBS_SIZE);

    int ticks = env_int("BT_TICKS", 100);
    int want = env_int("BT_ENVS", 0);
    int maxn = (int)((freeb - (size_t)900 * 1024 * 1024) / sizeof(BtState));
    if (want > 0 && want < maxn) maxn = want;

    // Demon-first per the user directive (Demon + Ghost Scepter, zone 0, D5).
    if (parity_check(6, env_int("BT_PARITY_TICKS", 1200), zone, shop_steps, demon, ghost))
        return 1;
    if (env_int("BT_BENCH", 1) == 0) return g_fail ? 1 : 0;

    Batch b;
    batch_alloc(&b, maxn);
    int iv = env_int("BT_INVULN", 0);
    CUDA_OK(cudaMemcpyToSymbol(bmf_invuln, &iv, sizeof(int)));
    std::printf("[cfg] invulnerable=%d (sustained-crowd instrument; CPU parity used 0)\n", iv);
    batch_init(&b, zone, shop_steps, demon, ghost);   // Demon + Ghost Scepter
    report_resources();

    std::printf("\n[throughput] env-only, action=const 0, %d ticks, max batch n=%d\n", ticks, maxn);
    std::printf("  block sweep (H=1):\n");
    double ref = 0.0;
    for (int blk : {64, 128, 256, 512}) {
        double s = time_steps(&b, maxn, ticks, blk, 1);
        if (blk == 128) ref = s;
        std::printf("    block=%-3d  %10.0f env-steps/s  %6.1f ns/env-tick\n", blk, s, 1e9 / s);
    }
    std::printf("  horizon-fused (block=128):\n");
    for (int H : {16, 64, 128}) {
        double s = time_steps(&b, maxn, ticks, 128, H);
        std::printf("    H=%-4d  %10.0f env-steps/s  %.2fx baseline\n", H, s, s / ref);
    }
    std::printf("  batch scaling (H=1, block=128):\n");
    for (int ns : {128, 256, 512, 768, 1024, 1536, 2048, 3072}) {
        if (ns > maxn) continue;
        double s = time_steps(&b, ns, ticks, 128, 1);
        std::printf("    n=%-5d %10.0f env-steps/s  %6.1f ns/env-tick\n", ns, s, 1e9 / s);
    }

    {
        int ns = maxn < 512 ? maxn : 512;
        unsigned long long* dc;
        CUDA_OK(cudaMalloc(&dc, (size_t)ns * sizeof(unsigned long long)));
        std::vector<unsigned long long> hc(ns);
        for (int t = 0; t < 3; t++)
            bm_step_timed_kernel<<<(ns + 127) / 128, 128>>>(b.state, sizeof(BtState),
                    b.act, b.obs, b.rew, b.term, ns, dc);
        CUDA_OK(cudaDeviceSynchronize());
        CUDA_OK(cudaMemcpy(hc.data(), dc, (size_t)ns * sizeof(unsigned long long),
                           cudaMemcpyDeviceToHost));
        unsigned long long mn = ~0ull, mx = 0, sum = 0;
        for (int i = 0; i < ns; i++) { mn = hc[i] < mn ? hc[i] : mn; mx = hc[i] > mx ? hc[i] : mx; sum += hc[i]; }
        std::printf("  clock64 per-env whole-step cycles (n=%d): min=%llu avg=%llu max=%llu"
                    "  -> implied SM clock %.2f GHz (avg cycles / ns-per-env-tick)\n",
                    ns, mn, sum / ns, mx, (double)(sum / ns) / (1e9 / ref));
        cudaFree(dc);
    }

    std::printf("\n[stage] physics-stage attribution (whole-step kernel, bmf_phys_mode)\n");
    const char* mn_[] = {"full physics", "no broadphase scan", "broadphase only", "no physics"};
    double sf = 0.0;
    for (int m = 0; m < 4; m++) {
        int mode = m;
        CUDA_OK(cudaMemcpyToSymbol(bmf_phys_mode, &mode, sizeof(int)));
        double s = time_steps(&b, maxn, ticks, 128, 1);
        if (m == 0) sf = s;
        std::printf("    mode=%-20s %10.0f env-steps/s   stage costs %5.1f%% of tick\n",
                    mn_[m], s, 100.0 * (1.0 - (m == 0 ? 1.0 : sf / s)));
    }
    {
        int mode = BMF_MODE_FULL;
        CUDA_OK(cudaMemcpyToSymbol(bmf_phys_mode, &mode, sizeof(int)));
        cudaEvent_t e0, e1;
        cudaEventCreate(&e0); cudaEventCreate(&e1);
        cudaEventRecord(e0);
        for (int t = 0; t < ticks; t++)
            bm_phys_only_kernel<<<(maxn + 127) / 128, 128>>>(b.state, sizeof(BtState), maxn);
        cudaEventRecord(e1);
        CUDA_OK(cudaDeviceSynchronize());
        float ms = 0.0f;
        cudaEventElapsedTime(&ms, e0, e1);
        std::printf("    physics-only kernel (serial, 1 thread/env): %10.0f phys-steps/s "
                    "(%6.1f ns/env-tick)\n", (double)maxn * ticks / (ms / 1000.0),
                    1e9 * (ms / 1000.0) / ((double)maxn * ticks));
        cudaEventDestroy(e0); cudaEventDestroy(e1);
    }

    // ---- levelization study on real deep-wave Demon worlds
    int ce = env_int("BT_CROWD_ENVS", 8);
    int ct = env_int("BT_CROWD_TICKS", 8000);
    int cs = env_int("BT_CROWD_SAMPLES", 4);
    if (env_int("BT_SKIP_CROWD", 0) == 0) {
        std::printf("\n[levelize] %d Demon+ghost envs x %d ticks, %d snapshots each\n", ce, ct, cs);
        Batch cb;
        batch_alloc(&cb, ce);
        batch_init(&cb, zone, shop_steps, demon, ghost);
        int de = ct / (cs + 1);
        BtState* snap = NULL;
        CUDA_OK(cudaMalloc(&snap, (size_t)cs * ce * sizeof(BtState)));
        bm_crowd_kernel<<<(ce + 127) / 128, 128>>>(cb.state, sizeof(BtState), ce, cb.obs,
                ct, de, snap, ce, cs);
        CUDA_OK(cudaGetLastError());
        CUDA_OK(cudaDeviceSynchronize());
        std::vector<BtState> hsnap((size_t)cs * ce);
        CUDA_OK(cudaMemcpy(hsnap.data(), snap, (size_t)cs * ce * sizeof(BtState),
                           cudaMemcpyDeviceToHost));
        cudaFree(snap);

        size_t cap = (size_t)BT_PHYS_PAIR_POOL + 1;
        BtPhysWorld* scratch = (BtPhysWorld*)malloc(sizeof(BtPhysWorld));
        BtPhysWorld* refw = (BtPhysWorld*)malloc(sizeof(BtPhysWorld));
        unsigned char* ha = (unsigned char*)malloc(cap);
        int* last = (int*)malloc(sizeof(int) * BT_PHYS_MAX_BODIES);
        Op* ops = (Op*)malloc(sizeof(Op) * cap * 9);
        int* lvl = (int*)malloc(sizeof(int) * cap * 9);
        int* cnt = (int*)malloc(sizeof(int) * (cap * 9 + 2));
        int* ls = (int*)malloc(sizeof(int) * (cap * 9 + 2));
        int* lf = (int*)malloc(sizeof(int) * (cap * 9 + 2));
        long long so = 0, sp = 0, mism = 0, nsamp = 0;
        int spair = 0, sbody = 0, mxpath = 0;
        std::printf("  %5s %5s %7s %6s %7s %6s %6s\n", "wave", "env", "bodies", "pairs",
                    "ops", "path", "par");
        for (int s = 0; s < cs; s++) {
            for (int e = 0; e < ce; e++) {
                BtState* st = &hsnap[(size_t)s * ce + e];
                if (st->phase != 0) continue;
                *refw = st->phys;
                bt_phys_step(refw);
                unsigned int dr = bmf_world_digest(refw);
                long long o0 = so;
                int b0 = sbody;
                int path = analyse_world(&st->phys, scratch, ha, last, ops, lvl, cnt,
                                         ls, lf, &so, &sp, &spair, &sbody);
                unsigned int dl = bmf_world_digest(&st->phys);
                if (dr != dl) {
                    std::printf("  LEVELIZE MISMATCH wave=%d env=%d 0x%08x vs 0x%08x\n",
                                st->wave, e, dl, dr);
                    mism++;
                }
                if (path > mxpath) mxpath = path;
                if (nsamp < 20)
                    std::printf("  %5d %5d %7d %6d %7lld %6d %6.2f\n", st->wave, e,
                                sbody - b0, spair, so - o0, path,
                                path ? (double)(so - o0) / path : 0.0);
                nsamp++;
            }
        }
        double ap = nsamp ? (double)sp / nsamp : 0.0;
        double ao = nsamp ? (double)so / nsamp : 0.0;
        std::printf("  snapshots=%lld mismatches=%lld  avg pairs=%.1f avg ops=%.0f "
                    "avg path=%.1f (max %d)  AVG PARALLELISM=%.2f  %s\n",
                    nsamp, mism, ap, ao, nsamp ? (double)sp / nsamp : 0.0, mxpath,
                    ap > 0 ? ao / ap : 0.0, mism ? "NOT BIT-EXACT" : "BIT-EXACT vs serial");
        free(scratch); free(refw); free(ha); free(last); free(ops);
        free(lvl); free(cnt); free(ls); free(lf);
        cudaFree(cb.state); cudaFree(cb.obs); cudaFree(cb.rew);
        cudaFree(cb.term); cudaFree(cb.act);
    }

    cudaFree(b.state); cudaFree(b.obs); cudaFree(b.rew); cudaFree(b.term); cudaFree(b.act);
    std::printf("\ndone (parity %s)\n", g_fail ? "FAILED" : "ok");
    return g_fail ? 1 : 0;
}
