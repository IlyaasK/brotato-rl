// tests/test_brotato_gpu_combat.cu -- bit-exactness + speedup harness for the
// intra-environment parallel COMBAT phases in
// ocean/brotato/brotato_gpu_combat.cuh.
//
// Compile (repo root, sm_120 = RTX 5070):
//
//   nvcc -O2 -std=c++17 -arch=sm_120 --fmad=false -Xcompiler=-ffp-contract=off \
//     -Xcompiler=-Wno-narrowing -Xcompiler=-Wno-unreachable-code \
//     --diag-suppress=2361 --diag-suppress=111 --diag-suppress=128 \
//     -I. -Iocean/brotato \
//     tests/test_brotato_gpu_combat.cu -lm -o /tmp/test_brotato_gpu_combat
//   /tmp/test_brotato_gpu_combat
//
// --fmad=false + -ffp-contract=off are REQUIRED (no FMA contraction on either
// pass), exactly as the core and build.sh pin them.
//
// What this proves:
//   A. whole-step parity: a Demon (+ghost scepter) batch advanced through dense
//      waves has its BtState digest compared SERIAL bt_core_step vs the step
//      whose three combat phases run through btf_combat_phase_* -- byte for
//      byte, every tick, for BT_GC_TICKS ticks (default 10000+).
//   B. per-phase parity: each phase run standalone on real crowd states, serial
//      reference vs the parallel schedule, whole-BtState compared.
//   C. per-phase speedup: wall time of the serial phase vs the parallel phase
//      on the same crowd states.
//   D. host-side hint equivalence (pure CPU, always run): the device pre-scan
//      predicates are BT_HD, so the SCHEDULE's crux -- first_hit[] is a valid
//      lower bound and the hinted body is byte-identical to the serial body --
//      is also verified on the host over a real Demon crowd (melee + ranged
//      loadouts, synthesized projectiles) with no GPU in the loop.
//
// Environment knobs: BT_GC_ENVS (8), BT_GC_CROWD (6000), BT_GC_TICKS (10000),
//   BT_GC_SAMPLES (16), BT_GC_SKIP (1 = parity only, no timing).
#include <cuda_runtime.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../ocean/brotato/brotato_gpu_combat.cuh"

#define CUDA_OK(call) do {                                                     \
    cudaError_t _e = (call);                                                   \
    if (_e != cudaSuccess) {                                                   \
        std::fprintf(stderr, "CUDA error %s at %s:%d\n",                       \
                     cudaGetErrorString(_e), __FILE__, __LINE__);              \
        std::exit(1);                                                          \
    }                                                                          \
} while (0)

static int fails = 0;
// Serial whole-step cost per env-tick (us), for the phase-fraction report.
static double g_ser_us_per_tick = 0.0;
static void ok1(bool cond, const char *what) {
    std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) fails++;
}
static int env_int(const char *k, int dflt) {
    const char *v = getenv(k);
    return v && *v ? atoi(v) : dflt;
}

// The Demon loadout this harness exercises (adapted from the CUDA backend test:
// zone 0, one shop step so the starting-weapon Demon also rolls items).
static const int ZONE = 0;
static const int SHOP_STEPS = 1;

// Deterministic intake: seed per env exactly as brotato.cu does (i==0 -> 1).
static BT_HD bt_u64 seed_for(int i) { return (bt_u64)(i == 0 ? 1u : (unsigned)i); }

// Kiting policy (fixture generator only; never touches the simulation): moves
// away from the 6 observed nearest enemies plus a mild wall pull. Reads the
// player observation block written by bt_core_observe.
static BT_HD int gc_kite_action(BtState *s, const float *o) {
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

static BT_HD int gc_alive_enemies(const BtState *s) {
    int c = 0;
    for (int i = 0; i < BT_MAX; i++) if (s->ealive[i] && !s->eneutral[i]) c++;
    return c;
}

// ---------------------------------------------------------------------------
// device kernels
// ---------------------------------------------------------------------------
__global__ void gc_init_kernel(BtState *__restrict__ base, size_t stride, int n,
        unsigned short ch, unsigned short wp) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    BtState *s = BTF_AT(base, stride, i);
    std::memset((void *)s, 0, sizeof(BtState));
    bt_core_init(s, seed_for(i), ZONE, SHOP_STEPS, ch, wp);
}

// Build a dense crowd: advance `ticks` frames with the kiting policy and dump
// a full BtState snapshot every `dump_every` frames into snap[dump*n+i].
__global__ void gc_crowd_kernel(BtState *__restrict__ base, size_t stride, int n,
        float *__restrict__ obs, int ticks, int dump_every,
        BtState *__restrict__ snap, int max_dumps, int *__restrict__ max_alive) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    BtState *s = BTF_AT(base, stride, i);
    float *o = obs + (size_t)i * BT_OBS_SIZE;
    bt_core_observe(s, o);
    int dump = 0, peak = 0;
    for (int t = 0; t < ticks; t++) {
        int a = gc_kite_action(s, o);
        float r = 0.0f, term = 0.0f;
        bt_core_step(s, a, o, &r, &term);
        int av = gc_alive_enemies(s);
        if (av > peak) peak = av;
        if (dump_every > 0 && (t % dump_every) == 0 && dump < max_dumps)
            snap[(size_t)dump * n + i] = *s, dump++;
    }
    max_alive[i] = peak;
}

// Whole-step parity: serial (context never armed) for T ticks, per-tick digest.
__global__ void gc_step_serial_kernel(BtState *__restrict__ base, size_t stride, int n,
        float *__restrict__ obs, int T, unsigned int *__restrict__ dig) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    BtState *s = BTF_AT(base, stride, i);
    float *o = obs + (size_t)i * BT_OBS_SIZE;
    for (int t = 0; t < T; t++) {
        bt_core_observe(s, o);
        int a = gc_kite_action(s, o);
        float r = 0.0f, term = 0.0f;
        bt_core_step(s, a, o, &r, &term);
        dig[(size_t)t * n + i] = btf_combat_state_digest(s);
    }
}

// Whole-step parity: the SAME ticks with the three combat phases armed. One
// warp per env (blockDim 32); every lane runs bt_core_step so the group-synced
// phases have their full lane group (the serial scalar work is redundant but
// writes identical bytes, and a warp executes it in lockstep).
__global__ void gc_step_par_kernel(BtState *__restrict__ base, size_t stride, int n,
        float *__restrict__ obs, int T, unsigned int *__restrict__ dig) {
    int i = blockIdx.x;
    if (i >= n) return;
    int lane = (int)(threadIdx.x & 31u);
    BtState *s = BTF_AT(base, stride, i);
    float *o = obs + (size_t)i * BT_OBS_SIZE;
    for (int t = 0; t < T; t++) {
        bt_core_observe(s, o);
        int a = gc_kite_action(s, o);
        float r = 0.0f, term = 0.0f;
        bt_core_step(s, a, o, &r, &term);
        if (lane == 0) dig[(size_t)t * n + i] = btf_combat_state_digest(s);
        __syncwarp();
    }
}

// Seed every projectile slot of a snapshot so the projectile phases exercise a
// non-trivial scan even when the weapon loadout happens to have no live shot:
// half friendly (pet-style) and half enemy, on a deterministic lattice.
__global__ void gc_seed_projectiles_kernel(BtState *__restrict__ base, size_t stride, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    BtState *s = BTF_AT(base, stride, i);
    for (int p = 0; p < BT_MAX_PROJ; p++) {
        float x = (float)((p * 197) % (int)BT_ARENA_W);
        float y = (float)((p * 311) % (int)BT_ARENA_H);
        float rot = (float)(p * 37 % 360) * (BT_PI / 180.0f);
        int friendly = (p & 1);
        int pj = bt_spawn_projectile(s, x, y, rot, 420.0f, 5.0f, BT_SHAPE_CIRCLE,
                                     17.0f, 0.0f, 0.0f, rot, friendly);
        if (pj >= 0) {
            s->prcrit_chance[pj] = 0.25f; s->prcrit_dmg[pj] = 2.0f;
            s->prburn_chance[pj] = 0.3f;  s->prburn_dmg[pj] = 1.0f; s->prburn_dur[pj] = 60.0f;
        }
    }
}

// ---- per-phase apply kernels (one BtState per (warp, sample)) ----
// Serial references.
__global__ void gc_ph_wproj_serial(BtState *__restrict__ base, size_t stride, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    float r = 0.0f;
    btf_combat_wproj_serial(BTF_AT(base, stride, i), &r);
}
__global__ void gc_ph_eproj_serial(BtState *__restrict__ base, size_t stride, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    float r = 0.0f;
    btf_combat_eproj_serial(BTF_AT(base, stride, i), &r);
}
__global__ void gc_ph_contact_serial(BtState *__restrict__ base, size_t stride, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    float r = 0.0f;
    btf_combat_player_contact_serial(BTF_AT(base, stride, i), &r);
}

// Parallel schedules (one warp per sample).
__global__ void gc_ph_wproj_par(BtState *__restrict__ base, size_t stride, int n,
        BtfCombatArena *__restrict__ arena) {
    int i = blockIdx.x; if (i >= n) return;
    int lane = (int)(threadIdx.x & 31u);
    BtState *s = BTF_AT(base, stride, i);
    arena[i].reward = 0.0f;
    btf_combat_phase_wproj(s, &arena[i], 0, lane, 32);
}
__global__ void gc_ph_eproj_par(BtState *__restrict__ base, size_t stride, int n,
        BtfCombatArena *__restrict__ arena) {
    int i = blockIdx.x; if (i >= n) return;
    int lane = (int)(threadIdx.x & 31u);
    BtState *s = BTF_AT(base, stride, i);
    arena[i].reward = 0.0f;
    btf_combat_phase_eproj(s, &arena[i], 0, lane, 32);
}
__global__ void gc_ph_contact_par(BtState *__restrict__ base, size_t stride, int n,
        BtfCombatArena *__restrict__ arena) {
    int i = blockIdx.x; if (i >= n) return;
    int lane = (int)(threadIdx.x & 31u);
    BtState *s = BTF_AT(base, stride, i);
    arena[i].reward = 0.0f;
    btf_combat_phase_player_contact(s, &arena[i], 0, lane, 32);
}

__global__ void gc_count_projectiles(BtState *__restrict__ base, size_t stride, int n,
        int *__restrict__ out) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    BtState *s = BTF_AT(base, stride, i);
    int live_w = 0, live_e = 0;
    for (int p = 0; p < BT_MAX_WPROJ; p++) if (s->wproj.p[p].alive) live_w++;
    for (int p = 0; p < BT_MAX_PROJ; p++) if (s->pralive[p]) live_e++;
    out[2 * i + 0] = live_w;
    out[2 * i + 1] = live_e;
}

// ---------------------------------------------------------------------------
// host helpers
// ---------------------------------------------------------------------------
static BtState *dev_alloc_states(int n) {
    BtState *p = 0;
    CUDA_OK(cudaMalloc(&p, sizeof(BtState) * (size_t)n));
    return p;
}

// Seed a spread of live projectiles into a state (both friendly and enemy) so
// the projectile scans in a fixture are dense.
static void gc_seed_projectiles_host(BtState *s) {
    for (int q = 0; q < BT_MAX_PROJ; q += 2) {
        float x = (float)((q * 197) % (int)BT_ARENA_W);
        float y = (float)((q * 311) % (int)BT_ARENA_H);
        float rot = (float)((q * 37) % 360) * (BT_PI / 180.0f);
        int pj = bt_spawn_projectile(s, x, y, rot, 420.0f, 5.0f, BT_SHAPE_CIRCLE,
                                     17.0f, 0.0f, 0.0f, rot, (q & 2) ? 1 : 0);
        if (pj >= 0) {
            s->prcrit_chance[pj] = 0.25f; s->prcrit_dmg[pj] = 2.0f;
            s->prburn_chance[pj] = 0.3f;  s->prburn_dmg[pj] = 1.0f; s->prburn_dur[pj] = 60.0f;
        }
    }
}

// ---- 5. host-side hint equivalence ------------------------------------------
// The device pre-scan functions are BT_HD and run on the host too, so the crux
// of the schedule -- "first_hit[] is a valid LOWER BOUND and the hinted body is
// byte-identical to the serial body" -- is verified on the CPU as well, over a
// real Demon crowd (melee + ranged loadouts) with synthesized projectiles, in
// both the friendly and the enemy paths.
static int host_hint_equivalence(void) {
    unsigned short ch = (unsigned short)bt_character_id_from_name("demon");
    unsigned short wp_melee = (unsigned short)bt_weapon_id_from_name("weapon_ghost_scepter_1");
    unsigned short wp_ranged = (unsigned short)bt_weapon_id_from_name("weapon_smg_1");
    const int NENV = 2, CROWD = 15000, EVERY = 250;
    std::vector<BtState> st((size_t)NENV);
    std::vector<float> obs((size_t)NENV * BT_OBS_SIZE, 0.0f);
    for (int i = 0; i < NENV; i++) {
        std::memset(&st[i], 0, sizeof(BtState));
        bt_core_init(&st[i], seed_for(i), ZONE, SHOP_STEPS, ch, i == 0 ? wp_melee : wp_ranged);
        bt_core_observe(&st[i], &obs[(size_t)i * BT_OBS_SIZE]);
    }
    long states = 0, lw = 0, le = 0, bad = 0;
    int peak = 0;
    std::vector<int> hint(BTF_COMBAT_MAX_FIRST_HIT);
    for (int t = 0; t < CROWD; t++) {
        for (int i = 0; i < NENV; i++) {
            int a = gc_kite_action(&st[i], &obs[(size_t)i * BT_OBS_SIZE]);
            float r = 0.0f, term = 0.0f;
            bt_core_step(&st[i], a, &obs[(size_t)i * BT_OBS_SIZE], &r, &term);
            int av = gc_alive_enemies(&st[i]);
            if (av > peak) peak = av;
        }
        if ((t % EVERY) != 0) continue;
        for (int i = 0; i < NENV; i++) {
            states++;
            // #14 wproj: serial body vs hinted body
            BtState A = st[i], B;
            gc_seed_projectiles_host(&A);
            B = A;
            for (int q = 0; q < BT_MAX_WPROJ; q++) if (B.wproj.p[q].alive) lw++;
            for (int q = 0; q < BT_MAX_WPROJ; q++) hint[q] = btf_combat_wproj_first_hit(&B, q);
            float ra = 0.0f, rb = 0.0f;
            (void)bt_wproj_step(&A, &ra);
            (void)bt_wproj_step_hint(&B, &rb, hint.data());
            if (ra != rb || std::memcmp(&A, &B, sizeof(BtState)) != 0) bad++;
            // #18 enemy projectiles: serial vs hinted
            A = st[i];
            gc_seed_projectiles_host(&A);
            B = A;
            for (int q = 0; q < BT_MAX_PROJ; q++) if (B.pralive[q]) le++;
            for (int q = 0; q < BT_MAX_PROJ; q++) hint[q] = btf_combat_eproj_first_hit(&B, q);
            ra = 0.0f; rb = 0.0f;
            bt_eproj_phase(&A, &ra, 0);
            bt_eproj_phase(&B, &rb, hint.data());
            if (ra != rb || std::memcmp(&A, &B, sizeof(BtState)) != 0) bad++;
            // #20 contact: serial probe vs parallel-probe reduction
            A = st[i]; B = st[i];
            ra = 0.0f; rb = 0.0f;
            bt_player_contact_phase(&A, &ra);
            bt_player_contact_apply(&B, &rb);
            int h = -1;
            for (int k = 0; k < BT_MAX; k++) if (bt_player_contact_probe(&B, k)) { h = k; break; }
            B.epend = (h < 0) ? (short)-1 : (short)h;
            if (ra != rb || std::memcmp(&A, &B, sizeof(BtState)) != 0) bad++;
        }
    }
    std::printf("  host hint-equivalence: %ld states, peak %d enemies, live wproj %ld,"
                " live eproj %ld, mismatches %ld\n", states, peak, lw, le, bad);
    char msg[192];
    std::snprintf(msg, sizeof(msg),
                  "host hint-equivalence (%ld dense states, #14/#18/#20): %s", states,
                  bad == 0 ? "byte-identical" : "MISMATCH");
    ok1(bad == 0, msg);
    return bad != 0;
}

int main(void) {
    const int N = env_int("BT_GC_ENVS", 8);
    const int CROWD = env_int("BT_GC_CROWD", 6000);
    const int TICKS = env_int("BT_GC_TICKS", 10000);
    const int SAMPLES = env_int("BT_GC_SAMPLES", 16);
    const int SKIP = env_int("BT_GC_SKIP", 0);
    const size_t stride = sizeof(BtState);

    unsigned short ch = (unsigned short)bt_character_id_from_name("demon");
    unsigned short wp = (unsigned short)bt_weapon_id_from_name("weapon_ghost_scepter_1");
    std::printf("== test_brotato_gpu_combat ==  Demon + ghost_scepter_1, zone %d, shop %d,"
                " envs %d, crowd %d, ticks %d, samples %d\n", ZONE, SHOP_STEPS, N, CROWD, TICKS, SAMPLES);

    BtState *A = dev_alloc_states(N);
    BtState *B = dev_alloc_states(N);
    float *obsA = 0, *obsB = 0;
    CUDA_OK(cudaMalloc(&obsA, sizeof(float) * BT_OBS_SIZE * (size_t)N));
    CUDA_OK(cudaMalloc(&obsB, sizeof(float) * BT_OBS_SIZE * (size_t)N));
    unsigned int *digA = 0, *digB = 0;
    CUDA_OK(cudaMalloc(&digA, sizeof(unsigned int) * (size_t)TICKS * N));
    CUDA_OK(cudaMalloc(&digB, sizeof(unsigned int) * (size_t)TICKS * N));
    int *maxalive = 0;
    CUDA_OK(cudaMalloc(&maxalive, sizeof(int) * (size_t)N));

    // ---- 1. init both batches identically ----
    gc_init_kernel<<<N, 1>>>(A, stride, N, ch, wp);
    gc_init_kernel<<<N, 1>>>(B, stride, N, ch, wp);
    CUDA_OK(cudaDeviceSynchronize());

    // ---- 2. grow a dense crowd (identical seeds/actions on both batches) ----
    BtState *snapbuf = dev_alloc_states((size_t)SAMPLES * N);
    int dump_every = CROWD / SAMPLES > 0 ? CROWD / SAMPLES : 1;
    gc_crowd_kernel<<<N, 1>>>(A, stride, N, obsA, CROWD, dump_every, snapbuf, SAMPLES, maxalive);
    CUDA_OK(cudaDeviceSynchronize());
    {
        std::vector<int> pk(N);
        CUDA_OK(cudaMemcpy(pk.data(), maxalive, sizeof(int) * N, cudaMemcpyDeviceToHost));
        int sum = 0; for (int i = 0; i < N; i++) sum += pk[i];
        std::printf("  crowd: peak live enemies per env = %d avg (dense fixture built)\n", sum / N);
    }

    // ---- 3. whole-step parity: serial vs armed, per-tick digest ----
    {
        BtState *A0 = dev_alloc_states(N), *B0 = dev_alloc_states(N);
        CUDA_OK(cudaMemcpy(A0, A, sizeof(BtState) * N, cudaMemcpyDeviceToDevice));
        CUDA_OK(cudaMemcpy(B0, A, sizeof(BtState) * N, cudaMemcpyDeviceToDevice));
        BtfCombatArena *arena = 0;
        CUDA_OK(cudaMalloc(&arena, sizeof(BtfCombatArena) * (size_t)N));
        CUDA_OK(cudaMemset(arena, 0, sizeof(BtfCombatArena) * (size_t)N));

        cudaEvent_t e0, e1; CUDA_OK(cudaEventCreate(&e0)); CUDA_OK(cudaEventCreate(&e1));
        btf_combat_clear_arena();            /* the serial reference is never armed */
        CUDA_OK(cudaEventRecord(e0));
        gc_step_serial_kernel<<<N, 1>>>(A0, stride, N, obsA, TICKS, digA);
        CUDA_OK(cudaEventRecord(e1)); CUDA_OK(cudaEventSynchronize(e1));
        float ms_ser = 0.0f; CUDA_OK(cudaEventElapsedTime(&ms_ser, e0, e1));
        g_ser_us_per_tick = 1e3 * (double)ms_ser / ((double)TICKS * (double)N);

        // Arm the arena ARRAY base once (host->device symbol copy); inside the
        // kernel each warp derives its own env from blockIdx/threadIdx, so no
        // kernel writes any shared context. Also arm the framework's group mode
        // (32 = one warp per env, the contract brotato.cu uses): the core's
        // BT_SERIAL spans then run on lane 0 ONLY and its BT_COOP spans meet at
        // __syncwarp, so no span is executed redundantly across the warp.
        { int bp32 = 32; CUDA_OK(cudaMemcpyToSymbol(btf_group_bp, &bp32, sizeof(int))); }
        btf_combat_set_arena(arena);
        CUDA_OK(cudaEventRecord(e0));
        gc_step_par_kernel<<<N, 32>>>(B0, stride, N, obsB, TICKS, digB);
        CUDA_OK(cudaEventRecord(e1)); CUDA_OK(cudaEventSynchronize(e1));
        float ms_par = 0.0f; CUDA_OK(cudaEventElapsedTime(&ms_par, e0, e1));
        btf_combat_clear_arena();
        { int bp1 = 1; CUDA_OK(cudaMemcpyToSymbol(btf_group_bp, &bp1, sizeof(int))); }

        std::vector<unsigned int> ha((size_t)TICKS * N), hb((size_t)TICKS * N);
        CUDA_OK(cudaMemcpy(ha.data(), digA, sizeof(unsigned int) * ha.size(), cudaMemcpyDeviceToHost));
        CUDA_OK(cudaMemcpy(hb.data(), digB, sizeof(unsigned int) * hb.size(), cudaMemcpyDeviceToHost));
        long first = -1;
        for (size_t k = 0; k < ha.size(); k++) if (ha[k] != hb[k]) { first = (long)k; break; }

        std::printf("  whole-step: %d ticks x %d envs, per-tick BtState digest\n", TICKS, N);
        ok1(first < 0, first < 0 ? "serial vs armed step: every tick byte-identical"
                                 : "serial vs armed step: FIRST DIVERGENCE");
        if (first >= 0) {
            std::printf("      tick %ld env %ld: serial 0x%08x armed 0x%08x\n",
                        first / N, first % N, ha[(size_t)first], hb[(size_t)first]);
        }
        // final whole-state byte compare
        std::vector<BtState> sa(N), sb(N);
        CUDA_OK(cudaMemcpy(sa.data(), A0, sizeof(BtState) * N, cudaMemcpyDeviceToHost));
        CUDA_OK(cudaMemcpy(sb.data(), B0, sizeof(BtState) * N, cudaMemcpyDeviceToHost));
        int badenv = -1; size_t badoff = 0;
        for (int i = 0; i < N && badenv < 0; i++) {
            const unsigned char *pa = (const unsigned char *)&sa[i], *pb = (const unsigned char *)&sb[i];
            for (size_t b = 0; b < sizeof(BtState); b++)
                if (pa[b] != pb[b]) { badenv = i; badoff = b; break; }
        }
        ok1(badenv < 0, badenv < 0 ? "final BtState byte-for-byte identical"
                                   : "final BtState byte mismatch");
        if (badenv >= 0)
            std::printf("      env %d byte %zu: %02x vs %02x\n", badenv, badoff,
                        ((const unsigned char *)&sa[badenv])[badoff],
                        ((const unsigned char *)&sb[badenv])[badoff]);
        if (!SKIP)
            std::printf("  whole-step timing: serial %.3f ms, armed(warp-phases) %.3f ms"
                        " (redundant 32-lane scalar work included)\n", ms_ser, ms_par);
        cudaFree(A0); cudaFree(B0); cudaFree(arena);
        cudaEventDestroy(e0); cudaEventDestroy(e1);
    }

    // ---- 4. per-phase parity + timing on real crowd states ----
    {
        // Rebuild fresh crowd snapshots (the batch state above has been stepped).
        gc_crowd_kernel<<<N, 1>>>(A, stride, N, obsA, CROWD, dump_every, snapbuf, SAMPLES, maxalive);
        CUDA_OK(cudaDeviceSynchronize());
        const int S = SAMPLES * N;
        BtState *S1 = dev_alloc_states(S), *S2 = dev_alloc_states(S), *S3 = dev_alloc_states(S);
        CUDA_OK(cudaMemcpy(S1, snapbuf, sizeof(BtState) * S, cudaMemcpyDeviceToDevice));
        CUDA_OK(cudaMemcpy(S2, snapbuf, sizeof(BtState) * S, cudaMemcpyDeviceToDevice));
        CUDA_OK(cudaMemcpy(S3, snapbuf, sizeof(BtState) * S, cudaMemcpyDeviceToDevice));
        gc_seed_projectiles_kernel<<<S, 1>>>(S1, stride, S);
        gc_seed_projectiles_kernel<<<S, 1>>>(S2, stride, S);
        gc_seed_projectiles_kernel<<<S, 1>>>(S3, stride, S);
        CUDA_OK(cudaDeviceSynchronize());

        BtfCombatArena *arena = 0;
        CUDA_OK(cudaMalloc(&arena, sizeof(BtfCombatArena) * (size_t)S));
        int *live = 0;
        CUDA_OK(cudaMalloc(&live, sizeof(int) * 2 * (size_t)S));
        gc_count_projectiles<<<S, 1>>>(S1, stride, S, live);
        CUDA_OK(cudaDeviceSynchronize());
        std::vector<int> hv(2 * (size_t)S);
        CUDA_OK(cudaMemcpy(hv.data(), live, sizeof(int) * hv.size(), cudaMemcpyDeviceToHost));
        long lw = 0, le = 0;
        for (int i = 0; i < S; i++) { lw += hv[2 * i]; le += hv[2 * i + 1]; }
        std::printf("  phase fixtures: %d states, %ld live wproj total, %ld live eproj total\n",
                    S, lw, le);

        BtState *ref = (BtState *)std::malloc(sizeof(BtState) * (size_t)S);
        BtState *got = (BtState *)std::malloc(sizeof(BtState) * (size_t)S);

        struct Phase { const char *name;
            void (*ser)(BtState *, size_t, int);
            void (*par)(BtState *, size_t, int, BtfCombatArena *);
            BtState *src; };
        Phase phases[3] = {
            { "#14 bt_wproj_step", gc_ph_wproj_serial, gc_ph_wproj_par, S1 },
            { "#18 enemy projectiles", gc_ph_eproj_serial, gc_ph_eproj_par, S2 },
            { "#20 player contact", gc_ph_contact_serial, gc_ph_contact_par, S3 },
        };

        cudaEvent_t e0, e1; CUDA_OK(cudaEventCreate(&e0)); CUDA_OK(cudaEventCreate(&e1));
        for (int p = 0; p < 3; p++) {
            // serial into S1..: use the source buffer directly (phases are
            // deterministic functions of the snapshot state).
            BtState *scopy = dev_alloc_states(S), *pcopy = dev_alloc_states(S);
            CUDA_OK(cudaMemcpy(scopy, phases[p].src, sizeof(BtState) * S, cudaMemcpyDeviceToDevice));
            CUDA_OK(cudaMemcpy(pcopy, phases[p].src, sizeof(BtState) * S, cudaMemcpyDeviceToDevice));

            for (int rep = 0; rep < 2; rep++) {
                CUDA_OK(cudaMemcpy(scopy, phases[p].src, sizeof(BtState) * S, cudaMemcpyDeviceToDevice));
                phases[p].ser<<<S, 1>>>(scopy, stride, S);
            }
            CUDA_OK(cudaEventRecord(e0));
            for (int rep = 0; rep < 20; rep++) {
                CUDA_OK(cudaMemcpy(scopy, phases[p].src, sizeof(BtState) * S, cudaMemcpyDeviceToDevice));
                phases[p].ser<<<S, 1>>>(scopy, stride, S);
            }
            CUDA_OK(cudaEventRecord(e1)); CUDA_OK(cudaEventSynchronize(e1));
            float ms_ser = 0.0f; CUDA_OK(cudaEventElapsedTime(&ms_ser, e0, e1));

            for (int rep = 0; rep < 2; rep++) {
                CUDA_OK(cudaMemcpy(pcopy, phases[p].src, sizeof(BtState) * S, cudaMemcpyDeviceToDevice));
                phases[p].par<<<S, 32>>>(pcopy, stride, S, arena);
            }
            CUDA_OK(cudaEventRecord(e0));
            for (int rep = 0; rep < 20; rep++) {
                CUDA_OK(cudaMemcpy(pcopy, phases[p].src, sizeof(BtState) * S, cudaMemcpyDeviceToDevice));
                phases[p].par<<<S, 32>>>(pcopy, stride, S, arena);
            }
            CUDA_OK(cudaEventRecord(e1)); CUDA_OK(cudaEventSynchronize(e1));
            float ms_par = 0.0f; CUDA_OK(cudaEventElapsedTime(&ms_par, e0, e1));

            // verify: compare the two copies on the host after one more run
            CUDA_OK(cudaMemcpy(scopy, phases[p].src, sizeof(BtState) * S, cudaMemcpyDeviceToDevice));
            CUDA_OK(cudaMemcpy(pcopy, phases[p].src, sizeof(BtState) * S, cudaMemcpyDeviceToDevice));
            phases[p].ser<<<S, 1>>>(scopy, stride, S);
            phases[p].par<<<S, 32>>>(pcopy, stride, S, arena);
            CUDA_OK(cudaMemcpy(ref, scopy, sizeof(BtState) * S, cudaMemcpyDeviceToHost));
            CUDA_OK(cudaMemcpy(got, pcopy, sizeof(BtState) * S, cudaMemcpyDeviceToHost));
            long bad = -1; size_t boff = 0;
            for (int i = 0; i < S && bad < 0; i++) {
                const unsigned char *a = (const unsigned char *)&ref[i];
                const unsigned char *b = (const unsigned char *)&got[i];
                for (size_t k = 0; k < sizeof(BtState); k++)
                    if (a[k] != b[k]) { bad = i; boff = k; break; }
            }
            char msg[160];
            std::snprintf(msg, sizeof(msg), "%-24s serial vs parallel: %s", phases[p].name,
                          bad < 0 ? "byte-identical" : "MISMATCH");
            ok1(bad < 0, msg);
            if (bad >= 0)
                std::printf("      state %ld byte %zu: %02x vs %02x\n", bad, boff,
                            ((const unsigned char *)&ref[bad])[boff],
                            ((const unsigned char *)&got[bad])[boff]);
            if (!SKIP) {
                double us_ser = 1e3 * ms_ser / 20.0, us_par = 1e3 * ms_par / 20.0;
                std::printf("      %-24s serial %8.2f us   parallel %8.2f us   speedup %5.2fx"
                            "   (serial phase = %5.2f%% of the serial tick%s)\n",
                            phases[p].name, us_ser, us_par, us_ser / (us_par > 0 ? us_par : 1e-9),
                            g_ser_us_per_tick > 0.0 ? 100.0 * us_ser / g_ser_us_per_tick : 0.0,
                            (p == 0 || p == 1) ? ", seeded fixture" : "");
            }
            cudaFree(scopy); cudaFree(pcopy);
        }
        cudaEventDestroy(e0); cudaEventDestroy(e1);
        std::free(ref); std::free(got);
        cudaFree(snapbuf); cudaFree(S1); cudaFree(S2); cudaFree(S3);
        cudaFree(arena); cudaFree(live);
    }

    // ---- 5. host-side hint equivalence (pure CPU, always run) ----
    (void)host_hint_equivalence();

    std::printf("== %s (%d failure%s) ==\n", fails == 0 ? "OK" : "FAILED", fails,
                fails == 1 ? "" : "s");
    return fails == 0 ? 0 : 1;
}
