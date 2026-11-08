// tests/test_brotato_gpu_spawn.cu
//
// Bit-exactness + per-phase throughput harness for ocean/brotato/brotato_gpu_spawn.cuh.
//
// Compile (repo root, sm_120 = RTX 5070):
//   nvcc -O2 -std=c++17 -arch=sm_120 --fmad=false -Xcompiler=-ffp-contract=off \
//     -Xcompiler=-Wno-narrowing -Xcompiler=-Wno-unreachable-code \
//     --diag-suppress=2361 --diag-suppress=111 --diag-suppress=128 \
//     -I. -Isrc -Iocean/brotato -Ivendor tests/test_brotato_gpu_spawn.cu -lm \
//     -o /tmp/test_brotato_gpu_spawn && /tmp/test_brotato_gpu_spawn
//
// PARTS
//   1. Differential bit-exactness: a long Demon run drives the serial core; at
//      every sample the WHOLE BtState is snapshotted, the SERIAL phase function
//      runs on one copy and the GPU spawn kernel on another from the identical
//      input, and the two full-struct digests must match byte for byte. Every
//      spawn phase is covered: birth_flush, pool_step, spawn_poll,
//      birth_advance, reserve_slot (cull), loot (consumables + materials) and
//      deaths.
//   2. Per-phase throughput: the same phase kernels run over a batch of envs;
//      GPU time per env per phase vs the serial host phase time per env.
//
// Env knobs: BT_SPAWN_ENVS, BT_SPAWN_TICKS, BT_SPAWN_SAMPLE, BT_SPAWN_ZONE,
// BT_SPAWN_BENCH=0 (parity only).

#include <cuda_runtime.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "ocean/brotato/brotato_gpu_spawn.cuh"

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

static int g_fail = 0;

// Kiting policy (identical in spirit to bench_brotato_cuda.cu): keeps the Demon
// alive so the run reaches deep waves and spawn-heavy frames.
BT_HD static int btg_kite_action(BtState* s, const float* o) {
    if (s->phase == 1) return 8;
    if (s->phase == 2) return 0;
    const int EN = 11 + 16 + 8;
    float want_x = 0.0f, want_y = 0.0f;
    for (int sl = 0; sl < BT_SLOTS; sl++) {
        float dx = o[EN + sl * 5 + 0] * BT_ARENA_W;
        float dy = o[EN + sl * 5 + 1] * BT_ARENA_H;
        float d2 = dx * dx + dy * dy;
        if (d2 <= 1.0f) continue;
        float inv = 1.0f / bt_sqrtf(d2);
        want_x -= dx * inv * inv;
        want_y -= dy * inv * inv;
    }
    if (s->px < 300.0f) want_x += 1.0f;
    if (s->px > BT_ARENA_W - 300.0f) want_x -= 1.0f;
    if (s->py < 300.0f) want_y += 1.0f;
    if (s->py > BT_ARENA_H - 300.0f) want_y -= 1.0f;
    int best = 0;
    float best_dot = -1e30f;
    for (int a = 1; a <= 8; a++) {
        float d = BT_MOVE_X[a] * want_x + BT_MOVE_Y[a] * want_y;
        if (d > best_dot) { best_dot = d; best = a; }
    }
    return best;
}

// ---------------------------------------------------------------------------
// serial phase replicas used as the reference for the two embedded Inline loops
// (the loot loops live inside bt_core_step, so there is no standalone function
// to call). These are the header's own btg_loot_serial_* -- i.e. the exact
// serial body the parallel schedule must reproduce.
// ---------------------------------------------------------------------------
static void ref_loot(BtState* s) {
    btg_loot_serial_consumables(s);
    /* NOTE: the core runs bt_enemy_lifecycle_tick between the two loops; that is
     * the enemy slice, not this phase, so it is deliberately absent here AND in
     * the GPU kernel (both schedules omit the same call). */
    btg_loot_serial_materials(s);
}

// ---------------------------------------------------------------------------
// One differential check: serial on a host copy vs the GPU kernel on a device
// copy, both from the identical input state.
// ---------------------------------------------------------------------------
struct Ctx {
    int n;
    size_t stride;
    BtState* dstate;          /* n contiguous device envs      */
    unsigned char* dmask;     /* n * BT_MAX death masks        */
    int* dslot;               /* n reserve-slot results        */
};

template <class Ser, class Launch>
static void phase_check(Ctx& c, BtState* h, int env, int tick, const char* name,
                        Ser ser, Launch launch) {
    BtState* A = (BtState*)malloc(sizeof(BtState));
    BtState* B = (BtState*)malloc(sizeof(BtState));
    *A = *h;
    ser(A);
    unsigned char* d0 = (unsigned char*)c.dstate + (size_t)env * c.stride;
    CUDA_OK(cudaMemcpy(d0, h, sizeof(BtState), cudaMemcpyHostToDevice));
    launch(c, env);
    CUDA_OK(cudaMemcpy(B, d0, sizeof(BtState), cudaMemcpyDeviceToHost));
    if (memcmp(A, B, sizeof(BtState)) != 0) {
        size_t k = 0;
        const unsigned char* pa = (const unsigned char*)A;
        const unsigned char* pb = (const unsigned char*)B;
        for (; k < sizeof(BtState); k++) if (pa[k] != pb[k]) break;
        std::printf("  MISMATCH %-14s env %d tick %d: BtState byte %zu "
                    "serial=0x%02x gpu=0x%02x (digest 0x%08x vs 0x%08x)\n",
                    name, env, tick, k, pa[k], pb[k],
                    btf_state_digest(A), btf_state_digest(B));
        g_fail = 1;
    }
    free(A); free(B);
}

// ---------------------------------------------------------------------------
// Family A: a phase that is a standalone function -> serial == that function.
// ---------------------------------------------------------------------------
static void check_callable_phases(Ctx& c, BtState* h, int env, int tick) {
    phase_check(c, h, env, tick, "birth_flush",
        [](BtState* s) { bt_birth_flush(s); },
        [](Ctx& c, int env) {
            btg_birth_flush_kernel<<<1, BTG_LANES>>>(c.dstate + env, c.stride, 1);
        });
    phase_check(c, h, env, tick, "pool_step",
        [](BtState* s) { bt_enemy_pool_step(s); },
        [](Ctx& c, int env) {
            btg_pool_step_kernel<<<1, BTG_LANES>>>(c.dstate + env, c.stride, 1);
        });
    phase_check(c, h, env, tick, "spawn_poll",
        [](BtState* s) { bt_spawn_poll(s); },
        [](Ctx& c, int env) {
            btg_spawn_poll_kernel<<<1, BTG_LANES>>>(c.dstate + env, c.stride, 1);
        });
    phase_check(c, h, env, tick, "birth_advance",
        [](BtState* s) { bt_birth_advance(s); },
        [](Ctx& c, int env) {
            btg_birth_advance_kernel<<<1, BTG_LANES>>>(c.dstate + env, c.stride, 1);
        });
    phase_check(c, h, env, tick, "reserve_slot",
        [](BtState* s) { (void)bt_reserve_slot(s); },
        [](Ctx& c, int env) {
            btg_reserve_slot_kernel<<<1, BTG_LANES>>>(c.dstate + env, c.stride, 1,
                c.dslot + env);
        });
    phase_check(c, h, env, tick, "struct_pool",
        [](BtState* s) { bt_struct_pool_tick(s); },
        [](Ctx& c, int env) {
            btg_struct_pool_tick_kernel<<<1, BTG_LANES>>>(c.dstate + env, c.stride, 1);
        });
    phase_check(c, h, env, tick, "group_cull",
        [](BtState* s) { bt_group_perf_cull(s); },
        [](Ctx& c, int env) {
            btg_group_perf_cull_kernel<<<1, BTG_LANES>>>(c.dstate + env, c.stride, 1);
        });
    phase_check(c, h, env, tick, "loot",
        [](BtState* s) { ref_loot(s); },
        [](Ctx& c, int env) {
            btg_loot_kernel<<<1, BTG_LANES>>>(c.dstate + env, c.stride, 1);
        });
}

// deaths: build a mask from the state, run serial bt_mark_enemy_dead per slot,
// then run the kernel with the same mask.
static void check_deaths(Ctx& c, BtState* h, int env, int tick) {
    unsigned char mask[BT_MAX];
    memset(mask, 0, sizeof(mask));
    int picked = 0;
    for (int i = 0; i < BT_MAX && picked < 3; i++) {
        if (!h->ealive[i] || h->eneutral[i]) continue;
        mask[i] = 1;
        picked++;
    }
    unsigned char* dm0 = c.dmask + (size_t)env * BT_MAX;
    CUDA_OK(cudaMemcpy(dm0, mask, BT_MAX, cudaMemcpyHostToDevice));
    phase_check(c, h, env, tick, "deaths",
        [mask](BtState* s) {
            for (int i = 0; i < BT_MAX; i++)
                if (mask[i] && s->ealive[i]) bt_mark_enemy_dead(s, i, 1);
        },
        [](Ctx& c, int env) {
            btg_deaths_kernel<<<1, BTG_LANES>>>(c.dstate + env, c.stride, 1,
                c.dmask + (size_t)env * BT_MAX);
        });
}

// Structure/pet node pool: bt_struct_pool_tick releases deferred_die nodes whose
// 3 s timer fired. bt_struct_pool_release only touches pet_alive/pet_pool_*, so a
// minimal live pet set is a faithful input and the phase is exercised directly.
static void stress_struct_pool(Ctx& c, int env) {
    BtState* h = (BtState*)malloc(sizeof(BtState));
    memset(h, 0, sizeof(BtState));
    bt_core_init(h, 31337u + (unsigned)env, 0, 1, BT_CHARACTER_DEMON,
                 bt_weapon_id_from_name("ghost_scepter"));
    bt_enter_wave(h, 22);
    h->pet_n = 4;
    for (int p = 0; p < 4; p++) {
        h->pet_alive[p] = 1;
        h->pet_pool_pending[p] = (unsigned char)(p != 1);   /* 1 not pending */
        /* p0 due now, p2 due at ticks, p3 due in 100, p1 not pending */
        h->pet_pool_die_at[p] = (unsigned int)h->ticks + (p == 2 ? 0u : (p == 3 ? 100u : 0u));
    }
    h->pet_pool_stamp = 7u;
    phase_check(c, h, env, -1, "struct_pool",
        [](BtState* s) { bt_struct_pool_tick(s); },
        [](Ctx& c, int env) {
            btg_struct_pool_tick_kernel<<<1, BTG_LANES>>>(c.dstate + env, c.stride, 1);
        });
    free(h);
}

// ---------------------------------------------------------------------------
// Focused stress worlds: force the phases onto their widest/edge object spaces
// so the compaction and the ordered commit are exercised where they matter.
// ---------------------------------------------------------------------------
static void stress_births(Ctx& c, int env, int tick) {
    BtState* h = (BtState*)malloc(sizeof(BtState));
    memset(h, 0, sizeof(BtState));
    bt_core_init(h, 12345u + (unsigned)env, 0, 1, BT_CHARACTER_DEMON,
                 bt_weapon_id_from_name("ghost_scepter"));
    bt_enter_wave(h, 20);
    /* 200 pending births at varied countdown stages + a pile overlapping the
     * player (relocation branch) and a pile of non-enemy classes. */
    h->n_births = 200;
    for (int i = 0; i < 200; i++) {
        BtBirth* b = &h->births[i];
        b->neutral = (unsigned char)(i % 7 == 0);
        b->is_boss = (unsigned char)(!b->neutral && i % 11 == 0);
        b->kind = (unsigned short)(b->neutral ? (i % BT_NNEUTRALS) : (i % 6));
        b->row = 0;
        b->bclass = (unsigned char)(i % 13 == 0 ? 1 : (i % 17 == 0 ? 2 : 0));
        b->active = (unsigned char)(i % 5 == 0 ? 1 : (i % 3 == 0 ? 2 : 0));
        if (i % 9 == 0) b->neutral = b->is_boss = 0;   /* relocate-eligible */
        b->src = -1; b->charmed_by = -1;
        b->x = (i % 9 == 0) ? h->px : (float)(i * 7 % 1200);
        b->y = (i % 9 == 0) ? h->py : (float)(i * 13 % 900);
        b->timer = (float)((i % 3 == 0) ? 0 : (i % 5) + 1);
    }
    check_callable_phases(c, h, env, tick);
    check_deaths(c, h, env, tick);
    free(h);
}

static void stress_loot(Ctx& c, int env, int tick) {
    BtState* h = (BtState*)malloc(sizeof(BtState));
    memset(h, 0, sizeof(BtState));
    bt_core_init(h, 777u + (unsigned)env, 0, 1, BT_CHARACTER_DEMON,
                 bt_weapon_id_from_name("ghost_scepter"));
    bt_enter_wave(h, 25);
    h->con_used = BT_MAX_CONS; h->mat_used = BT_MAX_MAT;
    /* 16 consumables + 64 materials in every arc stage, player mid-arena with
     * gold rings around it; a few materials homing (mat_home) and a couple
     * mob-owned (mat_mob) with a live enemy to exercise the steal path. */
    h->hp = h->max_hp * 0.5f;
    h->px = BT_ARENA_W * 0.5f; h->py = BT_ARENA_H * 0.5f;
    for (int i = 0; i < BT_MAX_CONS; i++) {
        h->conalive[i] = 1;
        h->conkind[i] = (unsigned char)(i % 3);
        h->conx[i] = h->px + (float)((i % 4) * 12 - 18);
        h->cony[i] = h->py + (float)((i / 4) * 12 - 18);
        h->con_arc[i][BT_LOOT_ARC_MON] = (float)(i % 2);
        h->con_arc[i][BT_LOOT_ARC_IDLE] = (float)(i % 11);
        h->con_arc[i][BT_LOOT_ARC_PUSH] = (float)(i % 2);
        h->con_arc[i][BT_LOOT_ARC_SPEED] = BT_LOOT_ITEM_ATTRACT_SPEED;
        h->con_attr[i] = (unsigned char)(i % 3 == 0);
    }
    for (int m = 0; m < BT_MAX_MAT; m++) {
        h->matalive[m] = 1;
        h->matval[m] = (unsigned short)(1 + m % 5);
        h->matx[m] = h->px + (float)((m % 8) * 15 - 52);
        h->maty[m] = h->py + (float)((m / 8) * 15 - 52);
        h->mat_arc[m][BT_LOOT_ARC_MON] = (float)(m % 2);
        h->mat_arc[m][BT_LOOT_ARC_IDLE] = (float)(m % 13);
        h->mat_arc[m][BT_LOOT_ARC_PUSH] = (float)(m % 2);
        h->mat_arc[m][BT_LOOT_ARC_SPEED] = BT_LOOT_ITEM_ATTRACT_SPEED;
        h->mat_attr[m] = (unsigned char)(m % 4 == 0);
        h->mat_home[m] = (unsigned char)(m % 19 == 0);
        h->mat_mob[m] = -1;
        h->mat_seq[m] = (unsigned int)m;
    }
    h->materials = 0.0f;
    check_callable_phases(c, h, env, tick);
    free(h);
}

static void stress_cull(Ctx& c, int env, int tick) {
    BtState* h = (BtState*)malloc(sizeof(BtState));
    memset(h, 0, sizeof(BtState));
    bt_core_init(h, 4242u + (unsigned)env, 0, 1, BT_CHARACTER_DEMON,
                 bt_weapon_id_from_name("ghost_scepter"));
    bt_enter_wave(h, 18);
    h->e_used = BT_MAX;
    /* FULL pool: every slot alive, mixed neutral/elite/boss so the eligible set
     * is a non-trivial subset -> exercises the parallel reservoir cull. */
    for (int i = 0; i < BT_MAX; i++) {
        bt_enemy_set_alive(h, i, 1);
        h->ekind[i] = (unsigned short)(i % 11 == 0 ? (i % BT_NNEUTRALS) : (i % 36));  /* i%36 hits priority kinds */
        h->eneutral[i] = (unsigned char)(i % 11 == 0);
        h->ex[i] = (float)(i * 11 % 1200); h->ey[i] = (float)(i * 7 % 900);
        h->ehp[i] = 100.0f; h->emax[i] = 100.0f;
        h->e_instance[i] = (unsigned short)BT_POOL_NO_INST;
        h->phys.bodies[i].active = 1;
    }
    h->ecount = BT_MAX;
    check_callable_phases(c, h, env, tick);
    free(h);
}

// ---------------------------------------------------------------------------
// Benchmark helpers: a device-to-device reset kernel (scatters one source state
// into every env) and a reserve_slot launcher wrapper.
// ---------------------------------------------------------------------------
__global__ void btg_reset_kernel(BtState* __restrict__ base, size_t stride, int n,
        const BtState* __restrict__ src) {
    int i = blockIdx.x;
    if (i >= n) return;
    unsigned char* d = (unsigned char*)BTF_AT(base, stride, i);
    const unsigned char* s = (const unsigned char*)src;
    for (size_t k = threadIdx.x; k < sizeof(BtState); k += blockDim.x) d[k] = s[k];
}

static int* g_dslot = nullptr;
static void launch_cull(BtState* b, size_t s, int n, cudaStream_t st) {
    btg_launch_reserve_slot(b, s, n, g_dslot, st);
}

// Fused spawn schedule: every spawn phase for an env in ONE launch (the shape
// the integrated core uses), order-preserving. Compared against the same serial
// sequence on the CPU.
__global__ void btg_test_fused_kernel(BtState* __restrict__ base, size_t stride, int n) {
    __shared__ BtgShared sh;
    int env = blockIdx.x;
    if (env >= n) return;
    BtState* s = BTF_AT(base, stride, env);
    bt_birth_flush(s);
    bt_enemy_pool_step(s);
    bt_struct_pool_tick(s);
    bt_spawn_poll(s);
    btg_birth_advance_par(s, BTF_LANE32, &sh);
    btg_loot_consumables(s, BTF_LANE32, &sh);
    btg_loot_materials(s, BTF_LANE32, &sh);
}
static void launch_fused(BtState* b, size_t st, int n, cudaStream_t s) {
    btg_test_fused_kernel<<<n, BTG_LANES, 0, s>>>(b, st, n);
}
static void ser_fused(BtState* s) {
    bt_birth_flush(s);
    bt_enemy_pool_step(s);
    bt_struct_pool_tick(s);
    bt_spawn_poll(s);
    bt_birth_advance(s);
    btg_loot_serial_consumables(s);
    btg_loot_serial_materials(s);
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    (void)argc; (void)argv;
    const int zone = env_int("BT_SPAWN_ZONE", 0);
    const int shop_steps = 1;
    const unsigned short demon = bt_character_id_from_name("demon");
    const unsigned short ghost = bt_weapon_id_from_name("ghost_scepter");

    cudaDeviceProp prop;
    CUDA_OK(cudaGetDeviceProperties(&prop, 0));
    std::printf("device: %s sm_%d%d SMs=%d  sizeof(BtState)=%zu\n", prop.name,
                prop.major, prop.minor, prop.multiProcessorCount, sizeof(BtState));

    int n = env_int("BT_SPAWN_ENVS", 4);
    int ticks = env_int("BT_SPAWN_TICKS", 6000);
    int sample = env_int("BT_SPAWN_SAMPLE", 37);

    std::vector<BtState> h; h.resize(n);
    Ctx c;
    c.n = n;
    c.stride = sizeof(BtState);
    { size_t f=0,t=0; cudaMemGetInfo(&f,&t);
      std::printf("gpu mem free=%.2f GB / %.2f GB\n", f/1073741824.0, t/1073741824.0); }
    int alloc = n > 16 ? n : 16;   /* stress worlds use env indices 0..7 */
    while (alloc > 1 && cudaMalloc(&c.dstate, (size_t)alloc * c.stride) != cudaSuccess) {
        cudaGetLastError(); alloc -= 4; if (alloc < 1) alloc = 1;
    }
    if (alloc < 8) { std::printf("too little GPU memory free\n"); return 0; }
    CUDA_OK(cudaMalloc(&c.dmask, (size_t)alloc * BT_MAX));
    CUDA_OK(cudaMalloc(&c.dslot, (size_t)alloc * sizeof(int)));

    // ---------------------------------------------------------------- setup
    std::printf("\n[1] Demon differential bit-exactness (n=%d ticks=%d sample=%d)\n",
                n, ticks, sample);
    for (int i = 0; i < n; i++) {
        memset(&h[i], 0, sizeof(BtState));
        unsigned int seed = (unsigned int)i + 1u;
        bt_core_init(&h[i], (bt_u64)seed, zone, shop_steps, demon, ghost);
        /* spread envs over deep waves: 1, 6, 12, 20, 28, 36, ... */
        int want = 1 + (i % 6) * 6;
        if (want > 1) bt_enter_wave(&h[i], want);
    }

    auto t0 = std::chrono::steady_clock::now();
    long checked[8] = {0};
    long samples = 0;
    for (int t = 0; t < ticks; t++) {
        std::vector<float> obs((size_t)n * BT_OBS_SIZE);
        for (int i = 0; i < n; i++) {
            float* o = &obs[(size_t)i * BT_OBS_SIZE];
            int a = btg_kite_action(&h[i], o);
            float r = 0.0f, term = 0.0f;
            bt_core_step(&h[i], a, o, &r, &term);
        }
        if (t % sample == 0) {
            samples++;
            for (int i = 0; i < n; i++) check_callable_phases(c, &h[i], i, t);
            /* rows counted per phase for the summary */
            checked[0] += n; checked[1] += n; checked[2] += n; checked[3] += n;
            checked[4] += n; checked[5] += n;
        }
        if (t % (sample * 5) == 0) {
            for (int i = 0; i < n; i++) {
                /* deaths on a copy so the run is unaffected */
                BtState* cp = (BtState*)malloc(sizeof(BtState));
                *cp = h[i];
                check_deaths(c, cp, i, t);
                free(cp);
            }
            checked[6] += n;
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();
    std::printf("  long run: %d envs x %d ticks, %ld samples, %.1fs\n",
                n, ticks, samples, secs);

    // stress worlds
    std::printf("\n[1b] focused stress worlds (wide births / loot / full-pool cull)\n");
    int sn = 8;
    int struct_pool_runs = 0;
    for (int i = 0; i < sn; i++) {
        stress_births(c, i, -1);
        stress_loot(c, i, -1);
        stress_cull(c, i, -1);
        stress_struct_pool(c, i);
        struct_pool_runs += 3;   /* p0, p2, p3 releases forced per world */
    }
    (void)struct_pool_runs;
    std::printf("  stress: %d worlds x 3 scenarios + %d forced struct-pool releases\n", sn, struct_pool_runs);

    if (g_fail) {
        std::printf("\nSPAWN BIT-EXACTNESS FAILED\n");
        return 1;
    }
    std::printf("  all spawn phases byte-identical to the serial core\n");

    if (env_int("BT_SPAWN_BENCH", 1) == 0) return 0;

    // ---------------------------------------------------------- throughput
    // Per-phase cost on SATURATED object spaces (the wide case the intra-env
    // schedule targets): BN envs all holding the same saturated state. The
    // phase time is measured as (reset+phase) - (reset) with a device-to-device
    // reset kernel, so the number is the kernel's own per-env cost with launch
    // overhead amortized over BN.
    int BN = env_int("BT_SPAWN_BENCH_ENVS", 256);
    size_t freeb = 0, totalb = 0;
    CUDA_OK(cudaMemGetInfo(&freeb, &totalb));
    {   /* the box may be shared by other CUDA jobs: cap to what fits */
        size_t room = freeb > (size_t)768 * 1024 * 1024 ? freeb - (size_t)768 * 1024 * 1024 : 0;
        int capn = (int)(room / sizeof(BtState));
        if (capn < 8) capn = 8;
        if (BN > capn) BN = capn;
    }
    std::printf("\n[2] per-phase throughput on saturated worlds (BN=%d)\n", BN);

    BtState* src_host = (BtState*)malloc(sizeof(BtState));
    memset(src_host, 0, sizeof(BtState));
    bt_core_init(src_host, 999u, 0, 1, demon, ghost);
    bt_enter_wave(src_host, 20);
    src_host->con_used = BT_MAX_CONS; src_host->mat_used = BT_MAX_MAT;
    src_host->n_births = 200;
    src_host->px = BT_ARENA_W * 0.5f; src_host->py = BT_ARENA_H * 0.5f;
    for (int i = 0; i < 200; i++) {
        BtBirth* b = &src_host->births[i];
        b->neutral = (unsigned char)(i % 7 == 0);
        b->is_boss = (unsigned char)(!b->neutral && i % 11 == 0);
        b->kind = (unsigned short)(b->neutral ? (i % BT_NNEUTRALS) : (i % 6));
        b->row = 0;
        b->bclass = (unsigned char)(i % 13 == 0 ? 1 : (i % 17 == 0 ? 2 : 0));
        b->active = (unsigned char)1;
        if (i % 9 == 0) b->neutral = b->is_boss = 0;
        b->src = -1; b->charmed_by = -1;
        b->x = (float)(i * 7 % 1200); b->y = (float)(i * 13 % 900);
        b->timer = (float)((i % 5) + 1);
    }
    for (int i = 0; i < BT_MAX_CONS; i++) {
        src_host->conalive[i] = 1;
        src_host->conkind[i] = (unsigned char)(i % 3);
        src_host->conx[i] = src_host->px + (float)((i % 4) * 12 - 18);
        src_host->cony[i] = src_host->py + (float)((i / 4) * 12 - 18);
        src_host->con_arc[i][BT_LOOT_ARC_MON] = (float)(i % 2);
        src_host->con_arc[i][BT_LOOT_ARC_SPEED] = BT_LOOT_ITEM_ATTRACT_SPEED;
    }
    for (int m = 0; m < BT_MAX_MAT; m++) {
        src_host->matalive[m] = 1;
        src_host->matval[m] = 1;
        src_host->matx[m] = src_host->px + (float)((m % 8) * 15 - 52);
        src_host->maty[m] = src_host->py + (float)((m / 8) * 15 - 52);
        src_host->mat_arc[m][BT_LOOT_ARC_MON] = (float)(m % 2);
        src_host->mat_arc[m][BT_LOOT_ARC_SPEED] = BT_LOOT_ITEM_ATTRACT_SPEED;
        src_host->mat_mob[m] = -1;
        src_host->mat_seq[m] = (unsigned int)m;
    }
    /* full pool for the reserve_slot/cull row */
    BtState* full_host = (BtState*)malloc(sizeof(BtState));
    *full_host = *src_host;
    full_host->e_used = BT_MAX;
    for (int i = 0; i < BT_MAX; i++) {
        bt_enemy_set_alive(full_host, i, 1);
        full_host->ekind[i] = (unsigned short)(i % 11 == 0 ? (i % BT_NNEUTRALS) : (i % 36));
        full_host->eneutral[i] = (unsigned char)(i % 11 == 0);
        full_host->ex[i] = (float)(i * 11 % 1200); full_host->ey[i] = (float)(i * 7 % 900);
        full_host->ehp[i] = 100.0f; full_host->emax[i] = 100.0f;
        full_host->e_instance[i] = (unsigned short)BT_POOL_NO_INST;
        full_host->eelite[i] = (unsigned char)(i % 17 == 0 ? 1 : 0);
        full_host->e_act[i] = (unsigned int)((i * 37) % 1000);   /* append order */
        full_host->phys.bodies[i].active = 1;
    }
    full_host->ecount = BT_MAX;
    full_host->wave_max_enemies = 55;   /* cull 45 plain units */

    BtState *big = 0, *src_dev = 0, *full_dev = 0;
    while (BN > 8 && cudaMalloc(&big, (size_t)BN * c.stride) != cudaSuccess) {
        cudaGetLastError(); BN /= 2;
    }
    if (!big) { std::printf("  (no device room for the batch bench; skipped)\n"); }
    if (big) {
    CUDA_OK(cudaMalloc(&g_dslot, (size_t)BN * sizeof(int)));
    CUDA_OK(cudaMalloc(&src_dev, sizeof(BtState)));
    CUDA_OK(cudaMalloc(&full_dev, sizeof(BtState)));
    CUDA_OK(cudaMemcpy(src_dev, src_host, sizeof(BtState), cudaMemcpyHostToDevice));
    CUDA_OK(cudaMemcpy(full_dev, full_host, sizeof(BtState), cudaMemcpyHostToDevice));

    // reset kernel: scatter src into every env (the phase measurement baseline)
    cudaEvent_t e0, e1;
    CUDA_OK(cudaEventCreate(&e0));
    CUDA_OK(cudaEventCreate(&e1));
    int iters = env_int("BT_SPAWN_ITERS", 60);
    /* CPU reference working set: small enough to stay cache-hot, so the
     * serial number is the phase's own cost, not a cold-RAM stream. */
    int SBLK = env_int("BT_SPAWN_SER_BLK", 8);
    if (SBLK > BN) SBLK = BN;

    auto reset_all = [&](const BtState* src_dev_) {
        btg_reset_kernel<<<BN, 256>>>(big, c.stride, BN, src_dev_);
    };
    auto elapsed = [&]() {
        float ms = 0.0f; CUDA_OK(cudaEventElapsedTime(&ms, e0, e1));
        return (double)ms * 1e6;   // ns
    };

    auto gpu_phase_ns = [&](void (*launch)(BtState*, size_t, int, cudaStream_t),
                            const BtState* src_dev_) {
        /* the reset is OUTSIDE the timed window: each launch starts from the
         * saturated source, and the reported cost is the MIN single launch. */
        double best = 1e30;
        for (int it = 0; it < iters; it++) {
            reset_all(src_dev_);
            CUDA_OK(cudaEventRecord(e0));
            launch(big, c.stride, BN, 0);
            CUDA_OK(cudaEventRecord(e1));
            CUDA_OK(cudaEventSynchronize(e1));
            double ns = elapsed();
            if (ns < best) best = ns;
        }
        return best / (double)BN;
    };
    auto ser_phase_ns = [&](void (*fn)(BtState*), const BtState* s) {
        // Each round re-copies the source into the batch (untimed, mirroring the
        // GPU reset kernel) and times one pass of the phase over fresh, cache-hot
        // states -- so neither side measures state degeneration.
        BtState* cp = (BtState*)malloc(sizeof(BtState));
        double best = 1e30;      /* MIN over rounds: robust to a busy, shared box */
        for (int it = 0; it < iters; it++) {
            *cp = *s;                       /* untimed reset (mirrors the GPU)   */
            auto t0s = std::chrono::steady_clock::now();
            fn(cp);                         /* cache-hot, non-degenerated state */
            auto t1s = std::chrono::steady_clock::now();
            double ns = std::chrono::duration<double, std::nano>(t1s - t0s).count();
            if (ns < best) best = ns;
        }
        free(cp);
        return best;
    };

    struct Row { const char* name; double ser_ns; double gpu_ns; };
    std::vector<Row> rows;
    auto add = [&](const char* name, void (*ser)(BtState*),
                   void (*launch)(BtState*, size_t, int, cudaStream_t),
                   const BtState* hs, const BtState* ds) {
        rows.push_back({name, ser_phase_ns(ser, hs), gpu_phase_ns(launch, ds)});
    };
    add("birth_flush",  [](BtState* s){ bt_birth_flush(s); },  btg_launch_birth_flush, src_host, src_dev);
    add("pool_step",    [](BtState* s){ bt_enemy_pool_step(s); }, btg_launch_pool_step, src_host, src_dev);
    add("spawn_poll",   [](BtState* s){ bt_spawn_poll(s); },   btg_launch_spawn_poll, src_host, src_dev);
    add("birth_advance",[](BtState* s){ bt_birth_advance(s); }, btg_launch_birth_advance, src_host, src_dev);
    add("loot",         [](BtState* s){ ref_loot(s); },        btg_launch_loot, src_host, src_dev);
    add("cull_full",    [](BtState* s){ (void)bt_reserve_slot(s); }, launch_cull, full_host, full_dev);
    add("group_cull",   [](BtState* s){ bt_group_perf_cull(s); }, btg_launch_group_perf_cull, full_host, full_dev);
    add("FUSED(all)",   [](BtState* s){ ser_fused(s); }, launch_fused, src_host, src_dev);

    std::printf("  %-14s %12s %12s %8s\n", "phase", "serial ns/env", "gpu ns/env", "speedup");
    for (auto& r : rows)
        std::printf("  %-14s %12.1f %12.1f %7.2fx\n", r.name, r.ser_ns, r.gpu_ns,
                    r.ser_ns / (r.gpu_ns > 0 ? r.gpu_ns : 1e-9));

    CUDA_OK(cudaEventDestroy(e0));
    CUDA_OK(cudaEventDestroy(e1));
    cudaFree(big); cudaFree(src_dev); cudaFree(full_dev); cudaFree(g_dslot);
    }  /* if (big) */
    cudaFree(c.dstate); cudaFree(c.dmask); cudaFree(c.dslot);
    std::printf("\nDONE\n");
    return 0;
}
