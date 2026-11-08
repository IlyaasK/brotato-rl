// tests/test_brotato_gpu_physics.cu -- bit-exactness + parallelism harness for
// ocean/brotato/brotato_gpu_physics.cuh (intra-environment parallel contact
// physics).
//
// Compile (repo root, sm_120 = RTX 5070):
//
//   nvcc -O2 -std=c++17 -arch=sm_120 --fmad=false -Xcompiler=-ffp-contract=off \
//     -Xcompiler=-Wno-narrowing -Xcompiler=-Wno-unreachable-code \
//     --diag-suppress=2361 --diag-suppress=111 --diag-suppress=128 \
//     -I. -Isrc -Iocean/brotato -Ivendor \
//     tests/test_brotato_gpu_physics.cu -lm -pthread -o /tmp/test_brotato_gpu_physics
//   /tmp/test_brotato_gpu_physics
//
// --fmad=false + -ffp-contract=off are REQUIRED (parity: no FMA contraction on
// either pass), exactly as build.sh pins them for brotato.
//
// WHAT IT PROVES
//   A. For each crowd configuration (10..140 enemy bodies in dense and spread
//      discs, four STATIC arena walls, scripted deaths/revivals, scripted
//      body_count shrink/grow, and one deliberately pool-saturating config) the
//      parallel schedule (btf_phys_wavefront_world) produces a byte-identical
//      BtPhysWorld to the serial bt_phys_step over BTGP_STEPS steps: the whole
//      struct is memcmp'd (bodies, pair pool, pair_map, pool free stack,
//      active_pairs, counters, flags).  On any difference the harness re-runs
//      both sides over a bisected step count to name the FIRST diverging step
//      and the first differing byte with its region.
//   B. Parallelism statistics on the same worlds: live constraint count per
//      step, critical path (max wavefront level), average parallelism
//      (mean m/levels), and the fraction of steps that needed the rescan
//      fallback (must be 0).
//   C. Kernel time per step for the parallel schedule and, for reference, the
//      serial bt_phys_step time on the same worlds (CPU, multi-threaded across
//      environments).
//
// Environment knobs: BTGP_STEPS=N (default 10000), BTGP_ONLY=<substr> to run a
// single config, BTGP_NO_BISECT=1.

/* Capacity override: this harness needs 144-body crowds, and it also steps a
 * whole BtState whose enemy world is indexed by the state header's BT_MAX (the
 * arena walls live at BT_MAX..BT_MAX+3) and whose pet world runs to
 * BT_PET_BODY_BASE + BT_MAX_PET.  Sized generously so a growing BT_MAX (owned
 * by StateCompact) cannot write past bodies[].  The value is also capped by the
 * device's per-block shared-memory limit, because each test block keeps its
 * BtgpScratch in dynamic shared: 4*C(200,2) + ~10 KB = 89 KB, under sm_120's
 * ~99 KB opt-in limit. */
#define BT_PHYS_MAX_BODIES 200
#define BT_PHYS_PAIR_POOL  4096  /* dense synthetic blobs go far past the 190 the
                                  * game's 100-enemy cap reaches */
#define BTGP_KEEP_MEASUREMENT_HOOK

#include "../ocean/brotato/brotato_gpu_physics.cuh"

#include <chrono>
#include <cmath>

/* A lowered BTGP_LEVEL_CAP forces the rescan fallback: then the fallback counter
 * is expected to be non-zero and is reported, not failed (bit-exactness still
 * has to hold). */
static const int kCapOverridden =
    (BTGP_LEVEL_CAP < 2 * BT_PHYS_MAX_BODIES + 2) ? 1 : 0;
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

/* The state-level test below indexes the enemy world by the state header's
 * BT_MAX (walls at BT_MAX..BT_MAX+3) and the pet world up to
 * BT_PET_BODY_BASE + BT_MAX_PET, so the capacity override above must cover both
 * or bodies[] would be written past its end.  Fail loudly instead. */
#if BT_MAX + 4 > BT_PHYS_MAX_BODIES
#error "test_brotato_gpu_physics: BT_PHYS_MAX_BODIES must cover BT_MAX + 4 arena walls"
#endif
#if BT_PET_BODY_BASE + BT_MAX_PET > BT_PHYS_MAX_BODIES
#error "test_brotato_gpu_physics: BT_PHYS_MAX_BODIES must cover the pet body slots"
#endif

#define CUDA_OK(call) do {                                                    \
    cudaError_t _e = (call);                                                  \
    if (_e != cudaSuccess) {                                                  \
        std::fprintf(stderr, "CUDA error %s at %s:%d\n",                      \
                     cudaGetErrorString(_e), __FILE__, __LINE__);             \
        std::exit(1);                                                         \
    }                                                                         \
} while (0)

static int env_int(const char *k, int dflt) {
    const char *v = getenv(k);
    return v && *v ? atoi(v) : dflt;
}

#define BTGP_ARENA_W 1200.0f
#define BTGP_ARENA_H  700.0f
#define BTGP_WALL_D   128.0f

// ---------------------------------------------------------------------------
// crowd configurations
// ---------------------------------------------------------------------------
typedef struct TestCfg {
    const char *name;
    int enemies;    /* 10..140 */
    float disc;     /* spawn disc radius: small = dense blob */
    float radius;   /* body radius (shape radius, so packed spacing is ~2r) */
    int deaths;     /* scripted deactivate/reactivate */
    int shrink;     /* scripted body_count oscillation */
} TestCfg;

static const TestCfg kCfgs[] = {
    { "n10-spread",   10, 420.0f,  9.0f, 1, 0 },
    { "n20-dense",    20,  90.0f,  9.0f, 0, 0 },
    { "n30-spread",   30, 430.0f,  8.0f, 1, 0 },
    { "n40-dense",    40, 110.0f,  9.0f, 0, 1 },
    { "n50-spread",   50, 440.0f, 10.0f, 1, 0 },
    { "n60-dense",    60, 130.0f,  8.0f, 0, 0 },
    { "n70-mixed",    70, 260.0f,  9.0f, 1, 1 },
    { "n80-dense",    80, 150.0f,  9.0f, 0, 0 },
    { "n90-spread",   90, 450.0f, 11.0f, 1, 0 },
    { "n100-dense",  100, 165.0f,  9.0f, 1, 1 },
    { "n110-mixed",  110, 300.0f, 10.0f, 0, 0 },
    { "n120-dense",  120, 175.0f,  9.0f, 1, 0 },
    { "n140-dense",  140, 190.0f,  9.0f, 1, 1 },
    { "n140-tight",  140,  60.0f, 12.0f, 0, 0 }, /* saturates the pool */
};
#define BTGP_NCFG ((int)(sizeof(kCfgs) / sizeof(kCfgs[0])))

// ---------------------------------------------------------------------------
// world construction (host only; the device copy is a memcpy of this)
// ---------------------------------------------------------------------------
static unsigned int g_lcg;

static unsigned int lcg_next(void) {
    g_lcg = g_lcg * 1664525u + 1013904223u;
    return g_lcg >> 8;
}

static float lcg_unit(void) { return (float)(lcg_next() & 0xFFFFFFu) / (float)0x1000000u; }

static void build_world(BtPhysWorld *w, int cfg) {
    const TestCfg *c = &kCfgs[cfg];
    float W = BTGP_ARENA_W, H = BTGP_ARENA_H, D = BTGP_WALL_D;
    int i;
    std::memset(w, 0, sizeof(*w));
    bt_phys_world_init(w, 1.0f / 60.0f, BT_PHYS_SOLVER_ITERATIONS);

    /* four STATIC arena walls, same construction as bt_phys_walls */
    w->bodies[0] = bt_phys_body_rect(bt_v2(-D * 0.5f, H * 0.5f), D * 0.5f, H * 0.5f + D,
                                     BT_LAYER_WALL, BT_MASK_WALL);
    w->bodies[1] = bt_phys_body_rect(bt_v2(W + D * 0.5f, H * 0.5f), D * 0.5f, H * 0.5f + D,
                                     BT_LAYER_WALL, BT_MASK_WALL);
    w->bodies[2] = bt_phys_body_rect(bt_v2(W * 0.5f, -D * 0.5f), W * 0.5f + D, D * 0.5f,
                                     BT_LAYER_WALL, BT_MASK_WALL);
    w->bodies[3] = bt_phys_body_rect(bt_v2(W * 0.5f, H + D * 0.5f), W * 0.5f + D, D * 0.5f,
                                     BT_LAYER_WALL, BT_MASK_WALL);

    g_lcg = 0x5eed1234u + (unsigned int)cfg * 2654435761u;
    for (i = 0; i < c->enemies; i++) {
        /* sunflower spiral in a disc of radius `disc` around the arena centre,
         * then a small deterministic jitter */
        float t = (float)i + 0.5f;
        float r = c->disc * sqrtf(t / (float)c->enemies) * (0.65f + 0.7f * lcg_unit());
        float a = 6.283185307179586f * t * 0.618033988749895f;
        float x = W * 0.5f + r * cosf(a);
        float y = H * 0.5f + r * sinf(a);
        float mass = 0.6f + 1.8f * lcg_unit();
        w->bodies[4 + i] = bt_phys_body_circle(bt_v2(x, y), bt_v2(0.0f, 0.0f),
                                               c->radius, mass,
                                               BT_LAYER_ENEMY, BT_MASK_ENEMY,
                                               BT_PHYS_MODE_CHARACTER);
        w->bodies[4 + i].vel = bt_v2(0.0f, 0.0f);
    }
    w->body_count = 4 + c->enemies;
}

// ---------------------------------------------------------------------------
// scripted per-step input, IDENTICAL on the host reference and in the kernel
// (BT_HD, no transcendentals beyond the shared bt_/BT_SQRTF port, no RNG)
// ---------------------------------------------------------------------------
#define BTGP_BODY_BASE 4

/* Per-config scalars the scenario needs.  Passed as plain ints so the scenario
 * is device-callable (the host kCfgs table is host-only). */
typedef struct TestParams {
    int enemies;
    int deaths;
    int shrink;
    int cfg;
} TestParams;

/* Head (world-level) part: only lane 0 runs this. */
BT_HD static inline void btgp_test_scenario_head(BtPhysWorld *w, int step, int shrink,
                                                 int enemies) {
    if (shrink) {
        /* body_count oscillation: exercises out-of-range pair retirement and
         * the growth-back path (a re-live pair must get a FRESH empty cache) */
        int drop = ((step / 97) % 3 == 0) ? 2 : 0;
        int bc = BTGP_BODY_BASE + enemies - drop;
        if (bc < BTGP_BODY_BASE + 2) bc = BTGP_BODY_BASE + 2;
        w->body_count = bc;
    }
}

/* Target the crowd chases: the alternating leg slams it into a wall so wall
 * (STATIC) pairs are live. */
BT_HD static inline void btgp_test_targets(int cfg, int step, float *tx, float *ty) {
    int mode = (step / 300) & 1;
    *tx = mode ? ((cfg & 1) ? BTGP_ARENA_W : 0.0f) : BTGP_ARENA_W * 0.5f;
    *ty = mode ? ((cfg & 2) ? BTGP_ARENA_H : 0.0f) : BTGP_ARENA_H * 0.5f;
}

/* Per-body part: each lane owns a disjoint body, so any assignment order is
 * legal (each body writes only its own vel/active). */
BT_HD static inline void btgp_test_scenario_body(BtPhysWorld *w, int i, int step, int deaths,
                                                 int enemies, float tx, float ty) {
    BtPhysicsBody *b = &w->bodies[i];
    float dx, dy, len, spd;

    if (deaths) {
        int victim = BTGP_BODY_BASE + (int)((unsigned int)(step * 2654435761u) %
                                            (unsigned int)enemies);
        if (i == victim) {
            /* a dead unit's shape is disabled, so every pair it takes part in is
             * retired; reviving it must re-enter with an empty cache */
            if (step % 11 == 3) b->active = 0;
            else if (step % 11 == 7) b->active = 1;
        }
    }
    if (!b->active) return;
    dx = tx - b->pos.x;
    dy = ty - b->pos.y;
    len = bt_v2len(bt_v2(dx, dy));
    if (!(len > 1.0e-4f)) len = 1.0e-4f;
    spd = 40.0f + (float)((i * 29 + step * 7) % 61);
    b->vel.x = dx / len * spd;
    b->vel.y = dy / len * spd;
}

static void scenario_host(BtPhysWorld *w, int step, const TestParams *p) {
    int n, i;
    float tx, ty;
    btgp_test_scenario_head(w, step, p->shrink, p->enemies);
    n = w->body_count > BT_PHYS_MAX_BODIES ? BT_PHYS_MAX_BODIES : w->body_count;
    btgp_test_targets(p->cfg, step, &tx, &ty);
    for (i = BTGP_BODY_BASE; i < n; i++)
        btgp_test_scenario_body(w, i, step, p->deaths, p->enemies, tx, ty);
}

// ---------------------------------------------------------------------------
// CPU reference
// ---------------------------------------------------------------------------
static void run_cpu(BtPhysWorld *w, int cfg, int steps) {
    TestParams p;
    int s;
    p.enemies = kCfgs[cfg].enemies;
    p.deaths = kCfgs[cfg].deaths;
    p.shrink = kCfgs[cfg].shrink;
    p.cfg = cfg;
    for (s = 0; s < steps; s++) {
        scenario_host(w, s, &p);
        bt_phys_step(w);
    }
}

// ---------------------------------------------------------------------------
// GPU kernel: one block per environment, `steps` frames per launch
// ---------------------------------------------------------------------------
#ifndef BTGP_TEST_BLOCK
#define BTGP_TEST_BLOCK 256
#endif
/* Validation build: check the live-list invariants the level build relies on and
 * stop at the first violation instead of letting it corrupt further. */
#ifndef BTGP_DEBUG_VALIDATE
#define BTGP_DEBUG_VALIDATE 1
#endif

/* One frame of the harness's input + the parallel physics, shared by both launch
 * shapes (bp = 256 with the scratch in dynamic shared, and bp = 32 one warp per
 * block with the scratch in global memory).  Group sync is btf_group_sync(bp),
 * so the bp == 32 case is a warp barrier and the function stays host-callable. */
BT_HD static inline void btgp_test_tick(BtPhysWorld *w, BtgpScratch *sc, int s,
                                        int c_enemies, int c_deaths, int c_shrink, int c_cfg,
                                        int mem, int slot, int bp,
                                        unsigned long long *sum_ops, unsigned long long *sum_lv,
                                        float *sum_ratio, int *max_lv, int *max_ops, int *fall,
                                        int *live_steps, unsigned long long *tick_cyc) {
    float tx, ty;
    int n;
    unsigned long long tick0 = 0;
#if defined(__CUDA_ARCH__)
    tick0 = (mem == 0) ? clock64() : 0;   /* device only: this helper is BT_HD */
#endif
    btf_group_sync(bp);
    if (mem == 0) btgp_test_scenario_head(w, s, c_shrink, c_enemies);
    btf_group_sync(bp);
    n = w->body_count > BT_PHYS_MAX_BODIES ? BT_PHYS_MAX_BODIES : w->body_count;
    btgp_test_targets(c_cfg, s, &tx, &ty);
    for (int i = BTGP_BODY_BASE + mem; i < n; i += bp)
        btgp_test_scenario_body(w, i, s, c_deaths, c_enemies, tx, ty);
    btf_group_sync(bp);
    btf_phys_wavefront_world(w, sc, mem, slot, bp);
    btf_group_sync(bp);
    if (mem == 0) {
        int m = w->active_pair_count, lv = sc->meta[3];
        *sum_ops += (unsigned long long)m;
        *sum_lv += (unsigned long long)lv;
        if (m > 0) { *sum_ratio += (float)m / (float)lv; (*live_steps)++; }
        if (lv > *max_lv) *max_lv = lv;
        if (m > *max_ops) *max_ops = m;
        if (sc->meta[4]) (*fall)++;
#if defined(__CUDA_ARCH__)
        *tick_cyc += clock64() - tick0;    /* whole tick: scenario + physics + barriers */
#endif
    }
}

__global__ void btgp_test_kernel(char *__restrict__ base, size_t stride, int n_envs,
                                 int steps, const int *__restrict__ params,
                                 unsigned int *__restrict__ stats,
                                 unsigned long long *__restrict__ cycles) {
    extern __shared__ char smem[];
    int env = blockIdx.x;
    BtPhysWorld *w;
    BtgpScratch sc;
    int mem, slot, bp, s;
    unsigned long long sum_ops = 0, sum_lv = 0;
    float sum_ratio = 0.0f;
    int max_lv = 0, max_ops = 0, fall = 0, live_steps = 0;
    if (env >= n_envs) return;
    w = (BtPhysWorld *)(base + (size_t)env * stride);
    sc = btgp_scratch_carve(smem);
    unsigned long long tick_cyc = 0;
    mem = threadIdx.x;
    slot = threadIdx.x >> 5;
    bp = blockDim.x;
    for (s = 0; s < steps; s++) {
        btgp_test_tick(w, &sc, s, params[env * 4 + 0], params[env * 4 + 1],
                       params[env * 4 + 2], params[env * 4 + 3], mem, slot, bp,
                       &sum_ops, &sum_lv, &sum_ratio, &max_lv, &max_ops, &fall, &live_steps,
                       &tick_cyc);
#if BTGP_DEBUG_VALIDATE
        /* invariant check on the live list the level build trusts (first steps) */
        if (mem == 0 && s < 64) {
            int k, bad = -1, bslot = -1, ba = -1, bb = -1;   /* -1: no violation yet */
            if (w->active_pair_count < 0 || w->active_pair_count > BT_PHYS_PAIR_POOL) bad = -2;
            for (k = 0; bad == -1 && k < w->active_pair_count; k++) {
                int sl = w->active_pairs[k];
                if (sl < 0 || sl > BT_PHYS_PAIR_POOL) { bad = k; bslot = sl; break; }
                if (w->pairs[sl].a < 0 || w->pairs[sl].a >= BT_PHYS_MAX_BODIES ||
                    w->pairs[sl].b < 0 || w->pairs[sl].b >= BT_PHYS_MAX_BODIES) {
                    bad = k; bslot = sl; ba = w->pairs[sl].a; bb = w->pairs[sl].b; break;
                }
            }
            if (bad != -1 && cycles) {
                cycles[env * 8 + 0] = (unsigned long long)s;
                cycles[env * 8 + 1] = (unsigned long long)bad;
                cycles[env * 8 + 2] = (unsigned long long)bslot;
                cycles[env * 8 + 3] = (unsigned long long)ba;
                cycles[env * 8 + 4] = (unsigned long long)bb;
                cycles[env * 8 + 5] = (unsigned long long)w->active_pair_count;
                cycles[env * 8 + 6] = (unsigned long long)w->pool_free_n;
                cycles[env * 8 + 7] = 0x5A5A5A5AULL;   /* marker: validation failed */
                break;
            }
        }
#endif
    }
    if (mem == 0) {
        stats[env * 8 + 0] = (unsigned int)sum_ops;
        stats[env * 8 + 1] = (unsigned int)sum_lv;
        stats[env * 8 + 2] = (unsigned int)max_lv;
        stats[env * 8 + 3] = (unsigned int)max_ops;
        stats[env * 8 + 4] = (unsigned int)fall;
        stats[env * 8 + 5] = (unsigned int)live_steps;
        stats[env * 8 + 6] = (unsigned int)(sum_ratio * 1024.0f);
        stats[env * 8 + 7] = (unsigned int)steps;
        if (cycles && cycles[env * 8 + 7] != 0x5A5A5A5AULL)
            cycles[env * 8 + 6] = tick_cyc;
    }
}

/* Same tick, one warp (32 lanes) per env, blockDim = 32, scratch in GLOBAL at
 * btgp_scratch_bytes() per env, so nothing but registers/threads limits
 * residency.  This is the barrier-economics fork: __syncwarp instead of
 * __syncthreads at every level. */
__global__ void btgp_test_kernel_warp(char *__restrict__ base, size_t stride, int n_envs,
                                      int steps, const int *__restrict__ params,
                                      unsigned int *__restrict__ stats,
                                      unsigned long long *__restrict__ cycles,
                                      char *__restrict__ gscratch) {
    int env = blockIdx.x;
    BtPhysWorld *w;
    BtgpScratch sc;
    int mem, slot, bp, s;
    unsigned long long sum_ops = 0, sum_lv = 0;
    float sum_ratio = 0.0f;
    int max_lv = 0, max_ops = 0, fall = 0, live_steps = 0;
    if (env >= n_envs) return;
    w = (BtPhysWorld *)(base + (size_t)env * stride);
    sc = btgp_scratch_carve(gscratch + (size_t)env * btgp_scratch_bytes());
    mem = threadIdx.x;
    unsigned long long tick_cyc = 0;
    slot = threadIdx.x >> 5;
    bp = blockDim.x; /* 32 */
    for (s = 0; s < steps; s++) {
        btgp_test_tick(w, &sc, s, params[env * 4 + 0], params[env * 4 + 1],
                       params[env * 4 + 2], params[env * 4 + 3], mem, slot, bp,
                       &sum_ops, &sum_lv, &sum_ratio, &max_lv, &max_ops, &fall, &live_steps,
                       &tick_cyc);
#if BTGP_DEBUG_VALIDATE
        /* invariant check on the live list the level build trusts (first steps) */
        if (mem == 0 && s < 64) {
            int k, bad = -1, bslot = -1, ba = -1, bb = -1;   /* -1: no violation yet */
            if (w->active_pair_count < 0 || w->active_pair_count > BT_PHYS_PAIR_POOL) bad = -2;
            for (k = 0; bad == -1 && k < w->active_pair_count; k++) {
                int sl = w->active_pairs[k];
                if (sl < 0 || sl > BT_PHYS_PAIR_POOL) { bad = k; bslot = sl; break; }
                if (w->pairs[sl].a < 0 || w->pairs[sl].a >= BT_PHYS_MAX_BODIES ||
                    w->pairs[sl].b < 0 || w->pairs[sl].b >= BT_PHYS_MAX_BODIES) {
                    bad = k; bslot = sl; ba = w->pairs[sl].a; bb = w->pairs[sl].b; break;
                }
            }
            if (bad != -1 && cycles) {
                cycles[env * 8 + 0] = (unsigned long long)s;
                cycles[env * 8 + 1] = (unsigned long long)bad;
                cycles[env * 8 + 2] = (unsigned long long)bslot;
                cycles[env * 8 + 3] = (unsigned long long)ba;
                cycles[env * 8 + 4] = (unsigned long long)bb;
                cycles[env * 8 + 5] = (unsigned long long)w->active_pair_count;
                cycles[env * 8 + 6] = (unsigned long long)w->pool_free_n;
                cycles[env * 8 + 7] = 0x5A5A5A5AULL;   /* marker: validation failed */
                break;
            }
        }
#endif
    }
    if (mem == 0) {
        stats[env * 8 + 0] = (unsigned int)sum_ops;
        stats[env * 8 + 1] = (unsigned int)sum_lv;
        stats[env * 8 + 2] = (unsigned int)max_lv;
        stats[env * 8 + 3] = (unsigned int)max_ops;
        stats[env * 8 + 4] = (unsigned int)fall;
        stats[env * 8 + 5] = (unsigned int)live_steps;
        stats[env * 8 + 6] = (unsigned int)(sum_ratio * 1024.0f);
        stats[env * 8 + 7] = (unsigned int)steps;
        if (cycles && cycles[env * 8 + 7] != 0x5A5A5A5AULL)
            cycles[env * 8 + 6] = tick_cyc;
    }
}


// ---------------------------------------------------------------------------
// comparison helpers
// ---------------------------------------------------------------------------
static unsigned long long world_hash(const void *p, size_t n) {
    const unsigned char *b = (const unsigned char *)p;
    unsigned long long h = 1469598103934665603ULL;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= (unsigned long long)b[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static void describe_offset(size_t off) {
    size_t o = 0;
    struct { const char *name; size_t lo, hi; } r[8];
    int i;
    r[0].name = "bodies";       r[0].lo = o; o += sizeof(BtPhysicsBody) * BT_PHYS_MAX_BODIES; r[0].hi = o;
    r[1].name = "body_count";   r[1].lo = o; o += sizeof(int); r[1].hi = o;
    r[2].name = "pairs(pool)";  r[2].lo = o; o += sizeof(BtPhysPair) * (BT_PHYS_PAIR_POOL + 1); r[2].hi = o;
    r[3].name = "pair_map";     r[3].lo = o; o += sizeof(unsigned short) * BT_PHYS_PAIR_CAPACITY; r[3].hi = o;
    r[4].name = "pool_free";    r[4].lo = o; o += sizeof(unsigned short) * BT_PHYS_PAIR_POOL; r[4].hi = o;
    r[5].name = "pool_free_n";  r[5].lo = o; o += sizeof(unsigned short); r[5].hi = o;
    r[6].name = "active_pairs"; r[6].lo = o; o += sizeof(((BtPhysWorld *)0)->active_pairs); r[6].hi = o;
    r[7].name = "wc/counters";  r[7].lo = o; r[7].hi = sizeof(BtPhysWorld);
    for (i = 0; i < 8; i++) {
        if (off >= r[i].lo && off < r[i].hi) {
            std::fprintf(stderr, "  first differing byte %zu: %s + %zu\n",
                         off, r[i].name, off - r[i].lo);
            return;
        }
    }
    std::fprintf(stderr, "  first differing byte %zu: (beyond the modelled regions)\n", off);
}

// ---------------------------------------------------------------------------
// device-side world buffer
// ---------------------------------------------------------------------------
static char *g_dev = 0;
static size_t g_dev_stride = 0;
static int g_dev_n = 0;

static void dev_alloc(int n_envs) {
    g_dev_stride = sizeof(BtPhysWorld);
    g_dev_n = n_envs;
    CUDA_OK(cudaMalloc(&g_dev, g_dev_stride * (size_t)n_envs));
}

static void dev_upload(const std::vector<BtPhysWorld> &w) {
    CUDA_OK(cudaMemcpy(g_dev, w.data(), g_dev_stride * w.size(), cudaMemcpyHostToDevice));
}

static void dev_download(std::vector<BtPhysWorld> &w) {
    CUDA_OK(cudaMemcpy(w.data(), g_dev, g_dev_stride * w.size(), cudaMemcpyDeviceToHost));
}

/* block == 0 -> full block per env (bp = BTGP_TEST_BLOCK, scratch in dynamic
 * shared); block == 32 -> one warp per env, scratch in GLOBAL (the fork). */
static double run_gpu(const std::vector<int> &params, int n_envs, int steps,
                      std::vector<BtPhysWorld> &out, unsigned int *stats,
                      std::vector<unsigned long long> *cyc = 0, int block = 0) {
    size_t smem = btgp_scratch_bytes();
    int *dparams = 0;
    unsigned int *dstats = 0;
    unsigned long long *dcyc = 0;
    char *gscratch = 0;
    cudaEvent_t ev0, ev1;
    float ms = 0.0f;
    int blk = block ? block : BTGP_TEST_BLOCK;
    CUDA_OK(cudaFuncSetAttribute(btgp_test_kernel, cudaFuncAttributeMaxDynamicSharedMemorySize,
                                 (int)smem));
    CUDA_OK(cudaMalloc(&dparams, sizeof(int) * params.size()));
    CUDA_OK(cudaMemcpy(dparams, params.data(), sizeof(int) * params.size(),
                       cudaMemcpyHostToDevice));
    CUDA_OK(cudaMalloc(&dstats, sizeof(unsigned int) * 8 * (size_t)n_envs));
    if (cyc) {
        cyc->assign((size_t)n_envs * 8, 0ULL);
        CUDA_OK(cudaMalloc(&dcyc, sizeof(unsigned long long) * 8 * (size_t)n_envs));
        CUDA_OK(cudaMemset(dcyc, 0, sizeof(unsigned long long) * 8 * (size_t)n_envs));
    }
    if (block) CUDA_OK(cudaMalloc(&gscratch, btgp_scratch_bytes() * (size_t)n_envs));
    CUDA_OK(cudaEventCreate(&ev0));
    CUDA_OK(cudaEventCreate(&ev1));
    CUDA_OK(cudaEventRecord(ev0));
    if (block)
        btgp_test_kernel_warp<<<n_envs, blk, 0>>>(g_dev, g_dev_stride, n_envs, steps, dparams,
                                                  dstats, dcyc, gscratch);
    else
        btgp_test_kernel<<<n_envs, blk, smem>>>(g_dev, g_dev_stride, n_envs, steps, dparams,
                                                dstats, dcyc);
    CUDA_OK(cudaEventRecord(ev1));
    CUDA_OK(cudaEventSynchronize(ev1));
    CUDA_OK(cudaGetLastError());
    CUDA_OK(cudaEventElapsedTime(&ms, ev0, ev1));
    CUDA_OK(cudaMemcpy(stats, dstats, sizeof(unsigned int) * 8 * (size_t)n_envs,
                       cudaMemcpyDeviceToHost));
    if (cyc) CUDA_OK(cudaMemcpy(cyc->data(), dcyc,
                                sizeof(unsigned long long) * 8 * (size_t)n_envs,
                                cudaMemcpyDeviceToHost));
    dev_download(out);
    if (gscratch) CUDA_OK(cudaFree(gscratch));
    if (dcyc) CUDA_OK(cudaFree(dcyc));
    CUDA_OK(cudaFree(dstats));
    CUDA_OK(cudaFree(dparams));
    CUDA_OK(cudaEventDestroy(ev0));
    CUDA_OK(cudaEventDestroy(ev1));
    return (double)ms / 1000.0;
}

// ---------------------------------------------------------------------------
// config execution: CPU vs GPU, bit-exact
// ---------------------------------------------------------------------------
static int g_failures = 0;

static void report_stats(const char *name, const unsigned int *st, int cfg, int steps,
                         double gpu_s, double cpu_s, size_t bodies) {
    unsigned long long sum_ops = st[0], sum_lv = st[1];
    int max_lv = (int)st[2], max_ops = (int)st[3], fall = (int)st[4], live = (int)st[5];
    double ratio = live ? (double)st[6] / 1024.0 / (double)live : 0.0;
    double us = steps > 0 ? gpu_s * 1e6 / (double)steps : 0.0;
    std::printf("  %-11s bodies=%-4zu m/step=%-8.1f lvl/step=%-7.2f maxlvl=%-4d "
                "maxm=%-5d avgpar=%-6.2f fallback=%-2d gpu=%.3fs (%.1f us/env-step) cpu=%.3fs (%.1f us/step)\n",
                name, bodies,
                (double)sum_ops / (double)steps, (double)sum_lv / (double)steps, max_lv,
                max_ops, ratio, fall, gpu_s, us,
                cpu_s, steps > 0 ? cpu_s * 1e6 / (double)steps : 0.0);
    (void)cfg;
    if (fall && !kCapOverridden) {
        std::fprintf(stderr, "FAIL: %s needed the rescan fallback on %d steps (max level %d)\n",
                     name, fall, max_lv);
        g_failures++;
    } else if (fall) {
        std::printf("    %s: rescan fallback exercised on %d steps (BTGP_LEVEL_CAP=%d)\n",
                    name, fall, BTGP_LEVEL_CAP);
    }
}

static void dump_diff(const BtPhysWorld *a, const BtPhysWorld *b, int steps) {
    const unsigned char *pa = (const unsigned char *)a, *pb = (const unsigned char *)b;
    size_t off, po;
    int d, k;
    for (off = 0; off < sizeof(BtPhysWorld); off++) if (pa[off] != pb[off]) break;
    std::fprintf(stderr, "  steps=%d first byte %zu\n", steps, off);
    std::fprintf(stderr, "  A: body_count=%d live=%d free_n=%d ovf=%d povf=%d | "
                         "B: body_count=%d live=%d free_n=%d ovf=%d povf=%d\n",
                 a->body_count, a->active_pair_count, (int)a->pool_free_n, a->overflow,
                 a->pair_overflow, b->body_count, b->active_pair_count, (int)b->pool_free_n,
                 b->overflow, b->pair_overflow);
    d = 0;
    for (k = 0; k < BT_PHYS_PAIR_POOL && d < 6; k++) {
        if (std::memcmp(&a->pairs[k], &b->pairs[k], sizeof(BtPhysPair)) != 0) {
            std::fprintf(stderr, "  slot %d: A(a=%d b=%d cc=%d) B(a=%d b=%d cc=%d)\n",
                         k, a->pairs[k].a, a->pairs[k].b, a->pairs[k].contact_count,
                         b->pairs[k].a, b->pairs[k].b, b->pairs[k].contact_count);
            d++;
        }
    }
    for (k = 0; k < a->active_pair_count && k < b->active_pair_count; k++) {
        if (a->active_pairs[k] != b->active_pairs[k]) {
            std::fprintf(stderr, "  active_pairs[%d]: A=%d B=%d\n", k, a->active_pairs[k],
                         b->active_pairs[k]);
            break;
        }
    }
    d = 0;
    for (k = 0; k < BT_PHYS_PAIR_CAPACITY && d < 6; k++) {
        if (a->pair_map[k] != b->pair_map[k]) {
            std::fprintf(stderr, "  pair_map[%d]: A=%d B=%d\n", k, (int)a->pair_map[k],
                         (int)b->pair_map[k]);
            d++;
        }
    }
    po = off > 13444 ? off - 13444 : 0;
    std::fprintf(stderr, "  (offset-in-pool=%zu slot=%zu field=%zu)\n", po,
                 po / sizeof(BtPhysPair), po % sizeof(BtPhysPair));
}

static std::vector<int> params_for(const std::vector<int> &cfgs) {
    std::vector<int> p;
    for (size_t i = 0; i < cfgs.size(); i++) {
        p.push_back(kCfgs[cfgs[i]].enemies);
        p.push_back(kCfgs[cfgs[i]].deaths);
        p.push_back(kCfgs[cfgs[i]].shrink);
        p.push_back(cfgs[i]);
    }
    return p;
}

static void run_cfg(int cfg, int steps, int bisect) {
    const TestCfg *c = &kCfgs[cfg];
    std::vector<BtPhysWorld> cpu(1), gpu(1);
    std::vector<unsigned int> stats(8);
    std::vector<int> params = params_for(std::vector<int>(1, cfg));
    double cpu_s, gpu_s;

    build_world(&cpu[0], cfg);
    build_world(&gpu[0], cfg);
    {
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        run_cpu(&cpu[0], cfg, steps);
        cpu_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }
    dev_alloc(1);
    dev_upload(gpu);
    gpu_s = run_gpu(params, 1, steps, gpu, stats.data());
    {
        unsigned long long hc = world_hash(&cpu[0], sizeof(BtPhysWorld));
        unsigned long long hg = world_hash(&gpu[0], sizeof(BtPhysWorld));
        int rc = std::memcmp(&cpu[0], &gpu[0], sizeof(BtPhysWorld));
        if (rc != 0 || hc != hg) {
            std::fprintf(stderr, "FAIL: %s: CPU vs GPU differ after %d steps (hash %llx vs %llx)\n",
                         c->name, steps, hc, hg);
            g_failures++;
            if (bisect) {
                int lo = 0, hi = steps, mid;
                while (lo + 1 < hi) {
                    std::vector<BtPhysWorld> cw(1), gw(1);
                    mid = (lo + hi) / 2;
                    build_world(&cw[0], cfg);
                    build_world(&gw[0], cfg);
                    run_cpu(&cw[0], cfg, mid);
                    dev_upload(gw);
                    run_gpu(params, 1, mid, gw, stats.data());
                    if (std::memcmp(&cw[0], &gw[0], sizeof(BtPhysWorld)) == 0) lo = mid;
                    else hi = mid;
                }
                std::fprintf(stderr, "  first diverging step = %d\n", hi);
                {
                    std::vector<BtPhysWorld> cw(1), gw(1);
                    size_t off;
                    const unsigned char *a, *b;
                    build_world(&cw[0], cfg);
                    build_world(&gw[0], cfg);
                    run_cpu(&cw[0], cfg, hi);
                    dev_upload(gw);
                    run_gpu(params, 1, hi, gw, stats.data());
                    a = (const unsigned char *)&cw[0];
                    b = (const unsigned char *)&gw[0];
                    for (off = 0; off < sizeof(BtPhysWorld); off++)
                        if (a[off] != b[off]) break;
                    describe_offset(off);
                }
            }
            stats.assign(8, 0);
        } else {
            std::printf("  %-11s bodies=%-4d BIT-EXACT over %d steps\n", c->name,
                        4 + c->enemies, steps);
        }
    }
    report_stats(c->name, stats.data(), cfg, steps, gpu_s, cpu_s, (size_t)(4 + c->enemies));
    CUDA_OK(cudaFree(g_dev));
    g_dev = 0;
}

// ---------------------------------------------------------------------------
// one launch with one block per configuration: the real deployment shape
// (intra-env parallelism only pays if env residency stays high)
// ---------------------------------------------------------------------------
static void run_batch(int steps) {
    std::vector<int> cfgs;
    std::vector<BtPhysWorld> w, out;
    std::vector<unsigned int> stats(8 * BTGP_NCFG);
    std::vector<int> params;
    double gpu_s;
    int i;

    for (i = 0; i < BTGP_NCFG; i++) cfgs.push_back(i);
    params = params_for(cfgs);
    for (i = 0; i < BTGP_NCFG; i++) {
        BtPhysWorld x;
        build_world(&x, i);
        w.push_back(x);
        out.push_back(x);
    }
    dev_alloc(BTGP_NCFG);
    dev_upload(w);
    gpu_s = run_gpu(params, BTGP_NCFG, steps, out, stats.data());
    {
        double envsteps = (double)BTGP_NCFG * (double)steps;
        std::printf("\nbatch: %d envs x %d steps = %.0f env-steps in %.3f s -> %.2f M env-steps/s "
                    "(%.2f us per env-step)\n",
                    BTGP_NCFG, steps, envsteps, gpu_s, envsteps / gpu_s / 1e6,
                    gpu_s * 1e6 / envsteps);
        for (i = 0; i < BTGP_NCFG; i++) {
            unsigned long long so = stats[i * 8 + 0];
            std::printf("    %-11s bodies=%-4d m/step=%.1f maxlvl=%-3d maxm=%-5d\n",
                        kCfgs[i].name, 4 + kCfgs[i].enemies, (double)so / (double)steps,
                        (int)stats[i * 8 + 2], (int)stats[i * 8 + 3]);
        }
    }
    CUDA_OK(cudaFree(g_dev));
    g_dev = 0;
}

// ---------------------------------------------------------------------------
// Integration: the framework dispatcher (btgp_phys_dispatch) stepping BOTH
// worlds of one BtState in the core's tick order — the enemy world (crowd in
// slots 0..99 with its four arena walls at BT_MAX..BT_MAX+3) and the pet world
// (bt_pet_world_init's walls 0..3 plus pet bodies 4..).  The whole BtState is
// memcmp'd, so a stray write outside the two worlds is a failure too.
// ---------------------------------------------------------------------------
BT_HD static inline void btgp_core_scenario_body(BtPhysWorld *w, int i, int step, int deaths,
                                                 int nen, float tx, float ty) {
    BtPhysicsBody *b = &w->bodies[i];
    float dx, dy, len, spd;
    if (i >= nen) return; /* arena walls and unused enemy slots */
    if (deaths) {
        int victim = (int)((unsigned int)(step * 2654435761u) % (unsigned int)nen);
        if (i == victim) {
            if (step % 11 == 3) b->active = 0;
            else if (step % 11 == 7) b->active = 1;
        }
    }
    if (!b->active) return;
    dx = tx - b->pos.x;
    dy = ty - b->pos.y;
    len = bt_v2len(bt_v2(dx, dy));
    if (!(len > 1.0e-4f)) len = 1.0e-4f;
    spd = 40.0f + (float)((i * 29 + step * 7) % 61);
    b->vel.x = dx / len * spd;
    b->vel.y = dy / len * spd;
}

static void build_state(BtState *st, int nen, int npets) {
    int i;
    float W = BT_ARENA_W, H = BT_ARENA_H;
    std::memset(st, 0, sizeof(*st));
    bt_phys_world_init(&st->phys, BT_DT, BT_PHYS_SOLVER_ITERATIONS);
    bt_phys_walls(st);                       /* body_count = BT_MAX + 4, walls at BT_MAX.. */
    g_lcg = 0x1234abcdU + (unsigned int)nen * 2654435761u;
    for (i = 0; i < BT_MAX; i++) {
        float on = (i < nen) ? 1.0f : 0.0f;
        float t = (float)i + 0.5f;
        float r = 130.0f * sqrtf(t / (float)BT_MAX) * (0.6f + 0.8f * lcg_unit());
        float a = 6.283185307179586f * t * 0.618033988749895f;
        BtPhysicsBody b = bt_phys_body_circle(bt_v2(W * 0.5f + r * cosf(a), H * 0.5f + r * sinf(a)),
                                              bt_v2(0.0f, 0.0f), 6.0f + 6.0f * lcg_unit(),
                                              0.5f + 2.0f * lcg_unit(),
                                              BT_LAYER_ENEMY, BT_MASK_ENEMY,
                                              BT_PHYS_MODE_CHARACTER);
        b.active = (unsigned char)(on != 0.0f);
        st->phys.bodies[i] = b;
    }
    bt_pet_world_init(st);                   /* pet world: walls + BT_MAX_PET inactive pets */
    g_lcg = 0x77aa55eeU + (unsigned int)npets * 2654435761u;
    for (i = 0; i < BT_MAX_PET && i < npets; i++) {
        float t = (float)i + 0.5f;
        float r = 180.0f * sqrtf(t / (float)(npets > 0 ? npets : 1)) * (0.6f + 0.8f * lcg_unit());
        float a = 6.283185307179586f * t * 0.618033988749895f;
        BtPhysicsBody b = bt_phys_body_circle(bt_v2(W * 0.5f + r * cosf(a), H * 0.5f + r * sinf(a)),
                                              bt_v2(0.0f, 0.0f), 5.0f + 4.0f * lcg_unit(),
                                              0.5f + 2.0f * lcg_unit(),
                                              512u, 640u, /* pet collision filter */
                                              BT_PHYS_MODE_CHARACTER);
        st->phys_pet.bodies[BT_PET_BODY_BASE + i] = b;
    }
    st->phys_pet.body_count = BT_PET_BODY_BASE + (npets < BT_MAX_PET ? npets : BT_MAX_PET);
}

__global__ void btgp_state_kernel(char *__restrict__ base, size_t stride, int n_envs, int steps,
                                  const int *__restrict__ params,
                                  unsigned int *__restrict__ stats) {
    extern __shared__ char smem[];
    int env = blockIdx.x;
    BtState *st;
    int mem, slot, bp, s, n, nen, deaths, npets;
    unsigned long long sum_ops = 0, sum_lv = 0;
    int max_lv = 0, fall = 0;
    if (env >= n_envs) return;
    st = (BtState *)(base + (size_t)env * stride);
    mem = threadIdx.x;
    slot = threadIdx.x >> 5;
    bp = blockDim.x;
    nen = params[env * 4 + 0];
    deaths = params[env * 4 + 1];
    npets = params[env * 4 + 2];

    for (s = 0; s < steps; s++) {
        float tx, ty, px, py;
        BtgpScratch sc;
        /* enemy world input, then the enemy physics step (core.h call site) */
        __syncthreads();
        btgp_test_targets(0, s, &tx, &ty);
        for (int i = mem; i < BT_MAX; i += bp)
            btgp_core_scenario_body(&st->phys, i, s, deaths, nen, tx, ty);
        __syncthreads();
        btgp_phys_dispatch(&st->phys, st, smem, bp);
        __syncthreads();
        if (mem == 0) {
            sc = btgp_scratch_carve(smem);
            sum_ops += (unsigned long long)st->phys.active_pair_count;
            sum_lv += (unsigned long long)sc.meta[3];
            if (sc.meta[3] > max_lv) max_lv = sc.meta[3];
            if (sc.meta[4]) fall++;
        }
        /* pet world input, then the pet physics step (pet_motion.h call site) */
        btgp_test_targets(1, s, &px, &py);
        n = st->phys_pet.body_count > BT_PHYS_MAX_BODIES ? BT_PHYS_MAX_BODIES
                                                        : st->phys_pet.body_count;
        for (int i = BTGP_BODY_BASE + mem; i < n; i += bp)
            btgp_test_scenario_body(&st->phys_pet, i, s, deaths, npets, px, py);
        __syncthreads();
        btgp_phys_dispatch(BT_PET_WORLD(st), st, smem, bp);
        __syncthreads();
        if (mem == 0) {
            sc = btgp_scratch_carve(smem);
            sum_ops += (unsigned long long)st->phys_pet.active_pair_count;
            sum_lv += (unsigned long long)sc.meta[3];
            if (sc.meta[3] > max_lv) max_lv = sc.meta[3];
            if (sc.meta[4]) fall++;
        }
    }
    if (mem == 0) {
        stats[env * 8 + 0] = (unsigned int)sum_ops;
        stats[env * 8 + 1] = (unsigned int)sum_lv;
        stats[env * 8 + 2] = (unsigned int)max_lv;
        stats[env * 8 + 3] = (unsigned int)fall;
    }
}

static void run_state_cfg(int nen, int npets, int deaths, int steps) {
    std::vector<BtState> cpu(1), gpu(1);
    std::vector<unsigned int> stats(8, 0);
    std::vector<int> params(4);
    double gpu_s;
    char *dev = 0;
    size_t stride = sizeof(BtState);
    int s, i;

    build_state(&cpu[0], nen, npets);
    build_state(&gpu[0], nen, npets);
    params[0] = nen; params[1] = deaths; params[2] = npets; params[3] = 0;
    {
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        for (s = 0; s < steps; s++) {
            float tx, ty, px, py, n;
            btgp_test_targets(0, s, &tx, &ty);
            for (i = 0; i < BT_MAX; i++)
                btgp_core_scenario_body(&cpu[0].phys, i, s, deaths, nen, tx, ty);
            bt_phys_step(&cpu[0].phys);
            btgp_test_targets(1, s, &px, &py);
            n = (float)cpu[0].phys_pet.body_count;
            for (i = BTGP_BODY_BASE; i < (int)n && i < BT_PHYS_MAX_BODIES; i++)
                btgp_test_scenario_body(&cpu[0].phys_pet, i, s, deaths, npets, px, py);
            bt_phys_step(BT_PET_WORLD(&cpu[0]));
        }
        gpu_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }
    CUDA_OK(cudaMalloc(&dev, stride));
    {
        int *dparams = 0;
        unsigned int *dstats = 0;
        size_t smem = btgp_scratch_bytes();
        CUDA_OK(cudaFuncSetAttribute(btgp_state_kernel, cudaFuncAttributeMaxDynamicSharedMemorySize,
                                     (int)smem));
        CUDA_OK(cudaMalloc(&dparams, sizeof(int) * 4));
        CUDA_OK(cudaMalloc(&dstats, sizeof(unsigned int) * 8));
        CUDA_OK(cudaMemcpy(dev, &gpu[0], stride, cudaMemcpyHostToDevice));
        CUDA_OK(cudaMemcpy(dparams, params.data(), sizeof(int) * 4, cudaMemcpyHostToDevice));
        btgp_state_kernel<<<1, BTGP_TEST_BLOCK, smem>>>(dev, stride, 1, steps, dparams, dstats);
        CUDA_OK(cudaGetLastError());
        CUDA_OK(cudaMemcpy(&gpu[0], dev, stride, cudaMemcpyDeviceToHost));
        CUDA_OK(cudaMemcpy(stats.data(), dstats, sizeof(unsigned int) * 8,
                           cudaMemcpyDeviceToHost));
        CUDA_OK(cudaFree(dparams));
        CUDA_OK(cudaFree(dstats));
    }
    CUDA_OK(cudaFree(dev));
    {
        const unsigned char *a = (const unsigned char *)&cpu[0];
        const unsigned char *b = (const unsigned char *)&gpu[0];
        size_t off = 0;
        while (off < sizeof(BtState) && a[off] == b[off]) off++;
        if (off != sizeof(BtState)) {
            std::fprintf(stderr, "FAIL: state(n_enemies=%d, n_pets=%d): first differing BtState "
                                 "byte %zu of %zu after %d steps\n",
                         nen, npets, off, sizeof(BtState), steps);
            g_failures++;
        } else {
            std::printf("  state nen=%-4d npets=%-4d deaths=%d  whole BtState BIT-EXACT over %d "
                        "steps (m/step=%.1f maxlvl=%d fallback=%u cpu=%.3fs)\n",
                        nen, npets, deaths, steps, (double)stats[0] / (double)steps,
                        (int)stats[2], stats[3], gpu_s);
        }
    }
}

// ---------------------------------------------------------------------------
// phase attribution (btgp_phase_mask; measurement only -- a masked run is a
// deliberately wrong simulation, so these numbers are never used for parity)
// ---------------------------------------------------------------------------
static void run_phases(int cfg, int steps) {
    static const int masks[5] = { 0xF, 0xB, 0xD, 0x7, 0x0 };
    static const char *mname[5] = { "full", "no solve", "no broadphase", "no integrate",
                                    "no phases" };
    const TestCfg *c = &kCfgs[cfg];
    std::vector<BtPhysWorld> w(1);
    std::vector<unsigned int> stats(8);
    std::vector<int> params = params_for(std::vector<int>(1, cfg));
    double t[5];
    unsigned int full_stats[8] = {0};
    unsigned int mask_stats[5][8];
    int i, keep = 0xF;
    std::printf("  phase attribution %s (bodies=%d, %d steps):\n", c->name, 4 + c->enemies, steps);
    dev_alloc(1);
    for (i = 0; i < 5; i++) {
        CUDA_OK(cudaMemcpyToSymbol(btgp_phase_mask, &masks[i], sizeof(int)));
        build_world(&w[0], cfg);
        dev_upload(w);
        t[i] = run_gpu(params, 1, steps, w, stats.data());
        for (int j = 0; j < 8; j++) mask_stats[i][j] = stats[j];
        if (i == 0) for (int j = 0; j < 8; j++) full_stats[j] = stats[j];
    }
    CUDA_OK(cudaMemcpyToSymbol(btgp_phase_mask, &keep, sizeof(int)));
    CUDA_OK(cudaFree(g_dev));
    g_dev = 0;
    /* Absolute times only.  Masking a phase changes the world the other phases
     * then see -- with no solve the crowd is never separated and the live set
     * explodes, with no integrate it never moves -- so subtracting mask times is
     * NOT a phase split.  Only two readings are clean:
     *   no-broadphase: no pair is ever claimed, i.e. begin+integrate+barriers;
     *   no-phases:     the loop/barrier floor.
     * full - no-broadphase is therefore the whole contact cost (broadphase +
     * level build + setup + 8 solves) as one lump. */
    for (i = 0; i < 5; i++)
        std::printf("    %-14s %8.1f us/env-step  (its own m/step=%.1f lvl/step=%.2f)\n",
                    mname[i], t[i] * 1e6 / (double)steps, (double)mask_stats[i][0] / (double)steps,
                    (double)mask_stats[i][1] / (double)steps);
    std::printf("    clean readings: contact cost (full - no-broadphase)=%.1f  "
                "begin+integrate+barriers=%.1f  loop floor=%.1f  (full run: m/step=%.1f "
                "lvl/step=%.2f, 9 passes x lvl+1 barriers)\n",
                (t[0] - t[2]) * 1e6 / (double)steps, t[2] * 1e6 / (double)steps,
                t[4] * 1e6 / (double)steps, (double)full_stats[0] / (double)steps,
                (double)full_stats[1] / (double)steps);
}

// ---------------------------------------------------------------------------
// barrier economics: bp = 32 (one warp per env, scratch in GLOBAL) vs bp = 256
//
// WHAT THIS PROVES, and why it used to fault: the warp leg runs the very same
// tick as the block leg with the same lane group, so its ONLY difference is the
// scratch's address space (global vs dynamic shared) and the barrier
// (`__syncwarp()` vs `__syncthreads()`).  With the framework's scratch scan
// (a multi-round read-modify-write chain through the scratch) the warp leg was
// NOT bit-exact and could fault: a lane's load of another lane's previous-round
// store is not ordered by a bare warp barrier on this hardware, so the scan
// returned non-monotone prefixes and a wrong total (measured: total 36 -> 9),
// btgp_compact_events emitted a non-ascending events[] list, and the walk's
// monotone-hint decode turned it into a negative pair index (-3) that
// bt_phys_pool_claim stored as b, so btgp_level_build read
// w->bodies[-3].inv_mass (memcheck: 4-byte read 196 B before the world
// allocation).  btgp_compact_events now scans a full warp through
// __shfl_*_sync (register-only, see btgp_warp_scan_excl), so this leg is exact
// regardless of where the scratch lives.  Each fork run is therefore a real
// assertion: a non-bit-exact bp=32 result is counted as a harness failure.
// ---------------------------------------------------------------------------
static void report_cycles(const char *tag, const std::vector<unsigned long long> &cyc,
                          int n_envs, int steps, double wall_s) {
    unsigned long long tot[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    size_t i;
    double khz = 0.0;
    for (i = 0; i + 7 < cyc.size(); i += 8)
        for (int j = 0; j < 8; j++) tot[j] += cyc[i + j];
    /* one env per launch: its whole-tick cycles over the measured wall time is
     * the effective SM clock during this very workload (no clockRate API, no
     * separate probe kernel).  With many envs the wall time covers all of them,
     * so only cycles are reported. */
    if (n_envs == 1 && wall_s > 0.0 && tot[6] > 0)
        khz = (double)tot[6] / wall_s / 1000.0;
    else
        khz = 0.0;
    {
        double per = (double)steps * (double)n_envs;
        double to_us = (khz > 0.0) ? (1e6 / (khz * 1000.0)) : 0.0;
        std::printf("    %s whole tick=%.0f cycles/env-step (SM clk %.2f GHz) = %.2f "
                    "us/env-step\n", tag, (double)tot[6] / per, khz / 1e6,
                    (double)tot[6] / per * to_us);
    }
}

static void run_fork(int cfg, int envs, int steps) {
    const TestCfg *c = &kCfgs[cfg];
    std::vector<BtPhysWorld> w(envs);   /* uploaded first, then reused as the result buffer */
    std::vector<unsigned int> stats(8 * (size_t)envs);
    std::vector<unsigned long long> cyc;
    std::vector<int> cfgs, params;
    cudaDeviceProp prop;
    double t256, t32;
    int i, blocks32 = 0;
    CUDA_OK(cudaGetDeviceProperties(&prop, 0));
    for (i = 0; i < envs; i++) cfgs.push_back(cfg);
    params = params_for(cfgs);
    for (i = 0; i < envs; i++) build_world(&w[i], cfg);
    dev_alloc(envs);
    CUDA_OK(cudaOccupancyMaxActiveBlocksPerMultiprocessor(&blocks32, btgp_test_kernel_warp, 32, 0));
    dev_upload(w);
    t32 = run_gpu(params, envs, steps, w, stats.data(), &cyc, 32);
    {
        BtPhysWorld ref;
        int ok = 0;
        build_world(&ref, cfg);
        run_cpu(&ref, cfg, steps);   /* one env is enough: every block runs the same world */
        ok = (std::memcmp(&ref, &w[0], sizeof(BtPhysWorld)) == 0);
        if (!ok) {
            std::fprintf(stderr, "FAIL: fork %s envs=%d: the bp=32 warp schedule is NOT "
                                 "bit-exact against the serial reference\n", c->name, envs);
            g_failures++;
        }
        std::printf("  fork %s envs=%d steps=%d: bp=32 %.2f us/env-step (%.2f M env-steps/s) "
                    "bit-exact(env0 vs serial)=%d | %d blocks/SM at 32 lanes -> %d envs/SM, "
                    "%d warps/SM (SM count %d)\n",
                    c->name, envs, steps, t32 * 1e6 / ((double)steps * envs),
                    (double)envs * steps / t32 / 1e6, ok, blocks32, blocks32,
                    blocks32, prop.multiProcessorCount);
    }
    for (i = 0; i < envs; i++) {
        if (cyc[(size_t)i * 8 + 7] == 0x5A5A5A5AULL) {
            std::fprintf(stderr, "FAIL: warp live-list invariant broken in env %d: step=%llu k=%llu "
                                 "slot=%lld a=%lld b=%lld count=%llu pool_free_n=%llu\n",
                         i, cyc[(size_t)i * 8 + 0], cyc[(size_t)i * 8 + 1],
                         (long long)cyc[(size_t)i * 8 + 2], (long long)cyc[(size_t)i * 8 + 3],
                         (long long)cyc[(size_t)i * 8 + 4], cyc[(size_t)i * 8 + 5],
                         cyc[(size_t)i * 8 + 6]);
            g_failures++;
        }
    }
    report_cycles("bp=32", cyc, envs, steps, t32);
    CUDA_OK(cudaFree(g_dev));
    g_dev = 0;
    if (envs == 1) {
        std::vector<unsigned long long> cyc256;
        std::vector<unsigned int> st2(8);
        dev_alloc(1);
        dev_upload(w);
        t256 = run_gpu(params_for(std::vector<int>(1, cfg)), 1, steps, w, st2.data(), &cyc256, 0);
        std::printf("  fork %s envs=1: bp=256 %.2f us/env-step  bp=32 %.2f us/env-step  "
                    "ratio(32/256)=%.2f\n", c->name, t256 * 1e6 / (double)steps,
                    t32 * 1e6 / (double)steps, t32 / t256);
        report_cycles("bp=256", cyc256, 1, steps, t256);
        CUDA_OK(cudaFree(g_dev));
        g_dev = 0;
    }
}

// ---------------------------------------------------------------------------
int main(void) {
    int steps = env_int("BTGP_STEPS", 10000);
    int bisect = env_int("BTGP_NO_BISECT", 0) ? 0 : 1;
    const char *only = getenv("BTGP_ONLY");
    int i, nfail = 0;

    std::printf("brotato GPU parallel physics: scratch=%zu B/world, pool=%d, body cap=%d, "
                "level cap=%d%s, steps=%d\n", btgp_scratch_bytes(), BT_PHYS_PAIR_POOL,
                BT_PHYS_MAX_BODIES, BTGP_LEVEL_CAP,
                kCapOverridden ? " (overridden: rescan fallback expected)" : "", steps);
    {
        int spb = 0;
        size_t smem = btgp_scratch_bytes();
        CUDA_OK(cudaFuncSetAttribute(btgp_test_kernel, cudaFuncAttributeMaxDynamicSharedMemorySize,
                                     (int)smem));
        CUDA_OK(cudaOccupancyMaxActiveBlocksPerMultiprocessor(&spb, btgp_test_kernel,
                                                              BTGP_TEST_BLOCK, smem));
        cudaDeviceProp prop;
        CUDA_OK(cudaGetDeviceProperties(&prop, 0));
        std::printf("kernel: block=%d dynamic shared=%zu B -> %d blocks/SM "
                    "(device max opt-in shared/block=%zu)\n",
                    BTGP_TEST_BLOCK, smem, spb, (size_t)prop.sharedMemPerBlockOptin);
    }

    for (i = 0; i < BTGP_NCFG; i++) {
        if (only && !std::strstr(kCfgs[i].name, only)) continue;
        run_cfg(i, steps, bisect);
    }
    if (!only) {
        int ssteps = env_int("BTGP_STATE_STEPS", steps);
        std::printf("\nBtState integration (framework dispatcher, enemy world + pet world):\n");
        run_state_cfg(10, 8, 1, ssteps);
        run_state_cfg(50, 32, 1, ssteps);
        run_state_cfg(100, 128, 1, ssteps);
        run_state_cfg(100, 0, 0, ssteps);
        run_batch(steps);
        if (env_int("BTGP_FORK", 1)) {
            std::printf("\nbarrier economics fork (same crowds, warp-per-env vs block-per-env):\n");
            run_fork(5, 1, steps < 3000 ? steps : 3000);    /* n60-dense, single env A/B + phases */
            run_fork(13, 1, steps < 3000 ? steps : 3000);   /* n140-tight, single env A/B + phases */
            run_fork(5, 512, steps < 1000 ? steps : 1000);  /* residency/throughput at bp=32 */
            run_fork(13, 512, steps < 1000 ? steps : 1000);
        }
        if (env_int("BTGP_PHASES", 1)) {
            int psteps = steps < 2000 ? steps : 2000;
            std::printf("\nphase attribution (btgp_phase_mask; measurement only):\n");
            run_phases(5, psteps);   /* n60-dense */
            run_phases(13, psteps);  /* n140-tight */
        }
    }

    nfail = g_failures;
    if (nfail) {
        std::printf("FAILURES: %d\n", nfail);
        return 1;
    }
    std::printf("OK: all configurations bit-exact\n");
    return 0;
}
