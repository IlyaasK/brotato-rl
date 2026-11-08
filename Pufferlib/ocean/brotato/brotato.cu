// CUDA Brotato -- thin GPU wrapper over the shared Brotato numerical core.
//
// There is NO simulation in this file. Every environment is one BtState stepped
// by bt_core_init/reset/step/observe (ocean/brotato/brotato_core.h), the exact
// same functions the CPU host wrapper (brotato.h) and the Metal backend call.
// This file only owns: device memory for the per-env BtState array, the launch
// geometry (one CUDA thread per environment), stream binding, and the renderer.
//
// Contract with src/pufferl.cu (see src/pufferenv.h):
//   * Env.log is at offset 0 (it aliases BtState.log) so the device log_reduce
//     can sum envs[i].log for i < total_agents / envs->num_agents.
//   * Env.num_agents == 1, so puf_vec_create(total_agents, ...) allocates one
//     env per agent and the whole batch is stepped by one kernel launch.
//   * obs_t is float32, matching the shared CPU contract (brotato.h typedefs
//     obs_t = float). pufferl's pufferl_forward_step casts float -> train
//     precision (bf16 by default) because sizeof(obs_t) != sizeof(precision_t).
//   * puf_step / puf_reset always act on the full batch: Env* is the device
//     base pointer, not a single host env. No host pointer is ever stored in
//     BtState and no per-step allocation or device copy is done.
//
// COMPILE TIME (ptxas is this tree's build bottleneck: ~9 min on one brotato TU,
// and `ptxas -v` charges ~500 s of it to bt_gpu_step_kernel alone -- that entry
// is where ptxas inlines and register-allocates the whole hot path -- against
// 15-31 s for the init kernel, 16-27 s for the reset kernel and <1 s each for
// everything else).  The cost is per TU, so keep every kernel a thin call site:
//   * bt_core_step (one call per env tick) and bt_ranged_step are already out of
//     line, so bt_gpu_step_kernel stays ~250 PTX lines -- but its *compile* is
//     the expensive part either way.
//   * the reset/init path is deliberately out of line too: bt_enter_wave and
//     bt_core_reset carry `__attribute__((noinline))` for __CUDACC__ in
//     brotato_core.h.  Without them nvcc inlines a full 74k-line copy of
//     bt_enter_wave into EACH of bt_gpu_init_kernel and bt_gpu_reset_kernel
//     (bt_core_init ends in bt_core_reset): measured 554,479 -> 428,517 PTX
//     lines and those two kernels 74,203/74,094 -> 9,322/9,247 lines, with the
//     step kernel's 254 registers / 6048 B stack unchanged (the fix is free at
//     runtime).  It does not by itself cut the ~500 s step-kernel compile.
//   * do not reach for -Xptxas -O1/-O2 to compensate: -O2 and -O1 measure the
//     same ~8-9 min as the default -O3 on this module.
//   * the lever that matters is paying the step kernel's compile once per tree
//     (separate device TU + -rdc + nvlink) instead of once per test TU.
#ifndef PUFFER_BROTATO_GPU_CU
#define PUFFER_BROTATO_GPU_CU

#define PUF_BACKEND PUF_GPU

// This env implements the optional GPU action-mask hook (pufferenv.h): reset and
// step write the trainer's per-agent mask buffer from the live phase, so the
// policy sees the phase-legal action set instead of all-ones.
#define PUF_HAS_ACTION_MASK_BIND 1

#include <cuda_runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Fixed env obs dtype (independent of the trainer precision build flag).
typedef float obs_t;

#include "pufferenv.h"

// Shared core: defines BtState (with struct Log first), BT_ACTIONS,
// BT_OBS_SIZE and bt_core_* (BT_HD, so callable from host and device here).
//
// Canonical table headers use BT_TABLE storage and exclude names with
// BT_HOST_ONLY on devices; no numeric mirror is needed.
#ifndef BT_CUDA
#define BT_CUDA 1
#endif
#include "brotato_rng.h"
/* COMPILE-TIME ISOLATION of the cooperative tick (Main's production hard gate).
 *
 * Production: parse the PLAIN core. brotato_core.h then supplies the identity
 * BT_SERIAL/BT_COOP/BTF_IS_LEADER fallbacks, so the one-thread-per-env tick
 * carries no dynamic btf_group_bp test, no physics-hook dispatch, no
 * btf_phys_ctx and no warp scratch/arming at all - none of that code exists in
 * this build (register pressure and branches included). The simulation source is
 * SHARED: the same bt_core_step and the same bt_phys_* stages.
 *
 * Experimental (-DBTFW_EXPERIMENTAL_LANE_SCHED): include the warp driver instead,
 * which installs the framework + the runtime physics hook BEFORE brotato_core.h
 * is parsed and pulls the core in itself, so it must replace the direct core
 * include. BTF_NO_NAME_STUBS keeps brotato.cu's own device-pass resolver stubs
 * below (this file defines them later); BTFW_NO_COMBAT keeps this TU to the
 * physics backend only. The warp scratch setup, the kernel call and the backend
 * symbol arming exist only in this branch.
 */
#if defined(BTFW_EXPERIMENTAL_LANE_SCHED)
#define BTF_NO_NAME_STUBS 1
#define BTFW_NO_COMBAT 1
#include "brotato_gpu_warp.cuh"
#else
#include "brotato_core.h"
#endif

// nvcc parses this file's HOST code in the device pass too, where BT_HOST_ONLY
// has already erased the generated name resolvers (and the char* name fields
// they read) from the device view of brotato_characters.h / brotato_weapons.h /
// brotato_upgrades.h. The create path and the renderer call them from host
// functions, so the device pass needs a declaration that resolves. These are
// deliberately plain `static inline` (implicitly __host__): they exist only to
// satisfy the device parse of host code and a device call would fail to
// compile rather than silently return a wrong id.
#if defined(__CUDA_ARCH__)
static inline unsigned short bt_character_id_from_name(const char* name) { (void)name; return 0xFFFFu; }
static inline unsigned short bt_weapon_id_from_name(const char* name) { (void)name; return 0xFFFFu; }
static inline const char* bt_stat_display_name(int stat) { (void)stat; return "?"; }
#endif

#define NUM_ATNS 1
#define ACT_SIZES {BT_ACTIONS}
#define OBS_SIZE BT_OBS_SIZE

// Launch geometry: one thread per environment (each env is fully independent).
#define BT_GPU_BLOCK 128
static int bt_gpu_grid(int n) {
    return (n + BT_GPU_BLOCK - 1) / BT_GPU_BLOCK;
}

// Per-env device state. `log` aliases `state.log` (both at offset 0), which is
// what the harness log reduce requires; the Agent pointers stay NULL on device
// and are never dereferenced by the kernels.
typedef struct Env {
    union {
        BtState state;
        struct Log log;
    };
    Agent agents[1];
    int tag;
    int boundary_reached;
    int num_agents;
    unsigned int rng;
} Env;

static void bt_gpu_check(cudaError_t err, const char* what) {
    if (err != cudaSuccess) {
        fprintf(stderr, "Brotato CUDA error (%s): %s\n", what, cudaGetErrorString(err));
        exit(1);
    }
}

#include "brotato_cuda_layout.cuh"

// ---------------------------------------------------------------------------
// Kernels. State lives entirely in the device Env array; actions/obs/rewards/
// terminals are the trainer's device buffers (no staging buffers).
// ---------------------------------------------------------------------------

// Per-env action mask row: bt_action_mask writes BT_ACTIONS bytes from the
// env's phase; a wider trainer row (mask_size > BT_ACTIONS, e.g. reserved ids)
// is zero-filled so the policy can never pick them.
static __device__ void bt_gpu_write_mask(BtState* s, unsigned char* row, int mask_size) {
    if (row == 0 || mask_size <= 0) return;
    bt_action_mask(s, row);
    for (int a = BT_ACTIONS; a < mask_size; a++) row[a] = 0;
}

// ---------------------------------------------------------------------------
// Implementation-capacity fail-closed latch (StateCompact's contract).
//
// The port models the source's UNBOUNDED pools with fixed tables; when one of
// them rejects work the source would have accepted, the env is a TRUNCATED
// simulation and training on it is invalid. bt_capacity_mask() reports every
// such loss class (brotato_capacity.h) as a 64-bit word, so the latch keeps TWO
// device words: the OR of the FULL 64-bit masks and the lowest env index that
// faulted. (A single `(mask << 32) | env` word would silently DROP every loss
// class >= 32.) The host consumes the latch at the rollout boundary
// (puf_vec_create / puf_reset / puf_log), where aborting cannot train on a
// truncated env, and every read is error-checked - a failed memcpy must FAIL
// CLOSED, never look clean.
// ---------------------------------------------------------------------------
__device__ unsigned long long btf_cap_fault_mask = 0ull;   /* OR of every 64-bit mask seen */
__device__ unsigned int btf_cap_fault_env = 0xFFFFFFFFu;   /* first faulting env, RAW; UINT_MAX = none */

static __device__ void bt_gpu_cap_latch(BtState* s, int env) {
    unsigned long long m = bt_capacity_mask(s);
    if (m == 0ull) return;
    atomicOr(&btf_cap_fault_mask, m);
    atomicCAS(&btf_cap_fault_env, 0xFFFFFFFFu, (unsigned int)env);
    /* Fail-closed on the DEVICE (Main's decision): the trap aborts the kernel, so
     * the invalid buffer can never reach the learner and no per-step D2H is added.
     * The host keeps a secondary read of the two words for the diagnostic. */
    printf("brotato: capacity loss env=%d wave=%d mask=0x%016llx\n",
           env, (int)s->wave, m);
    __trap();
}

// Host side: 0 = clean, 1 = fault. NEVER fails open: a CUDA error on either read
// aborts through bt_gpu_check (names the failing copy) instead of returning
// "clean". `where` names the call site; the full 64-bit mask is printed with its
// lowest set bit decoded by index (bt_capacity_name(bit) names it host-side).
static int bt_gpu_cap_faulted(const char* where) {
    unsigned long long mask = 0ull;
    unsigned int env = 0xFFFFFFFFu;
    bt_gpu_check(cudaMemcpyFromSymbol(&mask, btf_cap_fault_mask, sizeof(mask)),
                 "cap fault mask read");
    bt_gpu_check(cudaMemcpyFromSymbol(&env, btf_cap_fault_env, sizeof(env)),
                 "cap fault env read");
    if (mask == 0ull) return 0;
    int first = __builtin_ctzll(mask);
    fprintf(stderr,
        "Brotato CUDA capacity loss (fidelity bound reached): %s\n"
        "  first faulting env %u, loss mask 0x%016llx, first class bit %d\n"
        "  a fixed table rejected work the source accepts -> the env is truncated;\n"
        "  refusing to continue rather than train on a truncated simulation.\n",
        where, env, mask, first);
    return 1;
}

static void bt_gpu_cap_reset(void) {
    unsigned long long z = 0ull;
    unsigned int e = 0xFFFFFFFFu;
    bt_gpu_check(cudaMemcpyToSymbol(btf_cap_fault_mask, &z, sizeof(z)), "cap fault mask reset");
    bt_gpu_check(cudaMemcpyToSymbol(btf_cap_fault_env, &e, sizeof(e)), "cap fault env reset");
}


// Full reset: bt_core_reset does NOT write obs (core contract), so observe
// explicitly. rewards/terminals are zeroed for the timestep that precedes the
// first puf_step of a rollout.
__global__ void bt_gpu_reset_kernel(Env* __restrict__ envs,
        obs_t* __restrict__ observations, float* __restrict__ rewards,
        float* __restrict__ terminals, unsigned char* __restrict__ masks,
        int mask_size, int num_envs) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= num_envs) return;
    BtState* s = &envs[i].state;
    bt_core_reset(s);
    bt_core_observe(s, observations + (long)i * OBS_SIZE);
    rewards[i] = 0.0f;
    terminals[i] = 0.0f;
    if (masks) bt_gpu_write_mask(s, masks + (long)i * mask_size, mask_size);
    bt_gpu_cap_latch(s, i);
}

// One env tick. bt_core_step writes obs, reward and terminal in place and, on
// episode end, accumulates envs[i].log and autoreresets the state exactly like
// the CPU puf_step.
__global__ void bt_gpu_step_kernel(Env* __restrict__ envs,
        const float* __restrict__ actions, obs_t* __restrict__ observations,
        float* __restrict__ rewards, float* __restrict__ terminals,
        unsigned char* __restrict__ masks, int mask_size, int start, int count) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= count) return;
    int idx = start + i;
    float reward = 0.0f;
    float terminal = 0.0f;
    float action = actions[(long)idx * NUM_ATNS];
    bt_core_step(&envs[idx].state, (int)action,
        observations + (long)idx * OBS_SIZE, &reward, &terminal);
    rewards[idx] = reward;
    terminals[idx] = terminal;
    if (masks) bt_gpu_write_mask(&envs[idx].state, masks + (long)idx * mask_size, mask_size);
    bt_gpu_cap_latch(&envs[idx].state, idx);
}

// One-time per-env setup, run in place on the device. State embeds persistent
// enemy/pet contact worlds and owned effects; avoid materialising a second
// batch on the host. Seeds/zone/shop_steps/profile match brotato.h puf_init.
__global__ void bt_gpu_init_kernel(Env* __restrict__ envs, int num_envs,
        int zone, int shop_steps, unsigned short character, unsigned short starting_weapon,
        int audio_rng, int particles_rng, int invulnerable, unsigned short start_wave, struct BtReward rew_cfg, obs_t* __restrict__ observations,
        float* __restrict__ rewards, float* __restrict__ terminals,
        int endless, int unlock_all) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= num_envs) return;
    Env* e = &envs[i];
    e->rng = (unsigned int)i;
    e->tag = 0;
    e->boundary_reached = 0;
    e->num_agents = 1;
    e->agents[0].observations = NULL;
    e->agents[0].actions = NULL;
    e->agents[0].rewards = NULL;
    e->agents[0].terminals = NULL;
    e->agents[0].action_mask = NULL;
    e->agents[0].policy = 0;
    if (e->rng == 0) e->rng = 1u + (unsigned int)e->tag;
    bt_core_init(&e->state, (bt_u64)e->rng, zone, shop_steps, character, starting_weapon,
                 (unsigned char)(audio_rng ? 1 : 0));
    /* Set after bt_core_init; bt_core_reset preserves it. */
    e->state.particles_rng = (unsigned char)(particles_rng ? 1 : 0);
    /* DebugService.invulnerable, mirroring brotato.h puf_init (also set after
     * bt_core_init; bt_core_reset leaves it).  bt_core_init itself writes 0. */
    e->state.invulnerable = (unsigned char)(invulnerable ? 1 : 0);
    e->state.rew = rew_cfg;   // exactly the CPU env's bt_reward_cfg_init result
    // Deep-start boot for benchmarks: the CPU host reads the same optional
    // [env] start_wave (brotato.h puf_init, dict_find, default 1), so an SPS
    // number can be taken at the enemy cap instead of cycling wave 1.
    e->state.start_wave = start_wave;
    /* Endless-mode run option, mirroring brotato.h puf_init (set after
     * bt_core_init, which defaults it to 0). */
    e->state.endless = (unsigned char)(endless ? 1 : 0);
    /* Loot pool unlock profile, mirroring brotato.h puf_init's `unlock_all`
     * (progress_data.gd:884-895): default 1 = fully unlocked, 0 = fresh save. */
    e->state.lock_default_items = (unsigned char)(unlock_all ? 0 : 1);
    // Create-time contract shared with the CPU env (brotato.h puf_init ends with
    // bt_core_observe) and with Metal (brotato_metal.mm observes on create), so
    // a caller that reads observations before the first reset sees this env's
    // state instead of cudaMalloc garbage. rewards/terminals are zeroed for the
    // same reason (puf_reset zeroes them, and puf_step overwrites them).
    if (observations) bt_core_observe(&e->state, observations + (long)i * OBS_SIZE);
    if (rewards) rewards[i] = 0.0f;
    if (terminals) terminals[i] = 0.0f;
    bt_gpu_cap_latch(&e->state, i);
}

// ---------------------------------------------------------------------------
// Device batch binding (create-path hooks; mirrors ocean/breakout/breakout.cu).
// ---------------------------------------------------------------------------

static struct {
    Env* envs;
    int n;
    obs_t* observations;
    float* actions;
    float* rewards;
    float* terminals;
    unsigned char* action_mask;   // trainer-owned device mask, see puf_bind_action_mask
    int mask_size;
    void* warp_scratch;           // n * warp_scratch_bytes, one block region per env
    size_t warp_scratch_bytes;
    int use_warp;                 // [env] warp_step (default 0 = plain kernel)
    int armed_mode;               // -1 = none; 1 = warp tick armed; 0 = plain tick
    cudaStream_t stream;
    int render_open;
} g_gpu;

// Trainer hook, mirrors puf_bind_stream: hand the env the per-agent mask buffer
// (the trainer's own device buffer, e.g. p->env.action_mask.data). Called once
// before the first rollout; the mask is then rewritten by every reset/step.
void puf_bind_action_mask(unsigned char* device_mask, int mask_size) {
    g_gpu.action_mask = device_mask;
    g_gpu.mask_size = mask_size;
}

// Trainer create: reads zone/shop_steps, seeds every env identically to the CPU
// path, and returns the device Env base (also kept in g_gpu).
Env* puf_vec_create(int n, Dict* env_kwargs,
        obs_t* observations, float* actions, float* rewards, float* terminals) {
    // Mirrors brotato.h puf_init exactly: zone/shop_steps are passed raw and
    // clamped by bt_core_init, and the seed is the CPU trainer's
    // `rng == 0 ? 1 + tag : rng` with rng = env index and tag = 0 (the GPU path
    // has no frozen policies), so env i steps the same PCG32 stream as CPU env i.
    int zone = (int)dict_get(env_kwargs, "zone");
#if defined(BTFW_EXPERIMENTAL_LANE_SCHED)
    /* btf_group_bp and btf_phys_backend are PROCESS-WIDE device globals, so a
     * second live batch may not silently re-arm them (see the mode block below). */
    const int was_live = (g_gpu.envs != NULL);
#endif
    int shop_steps = (int)dict_get(env_kwargs, "shop_steps");
    /* Profile selector: the ini carries canonical tokens ("bull", "none"); the
     * host resolves them to ids once, cheaply, instead of every env. */
    unsigned short character = bt_character_id_from_name(dict_get_str(env_kwargs, "character"));
    /* Optional deep-start boot wave, exactly like the CPU host path
     * (brotato.h puf_init): dict_find (never dict_get: a missing key must
     * not exit), default 1, and only a value >= 1.0 is honoured. */
    unsigned short start_wave = 1;
    DictItem* it_start_wave = dict_find(env_kwargs, "start_wave");
    if (it_start_wave != NULL && it_start_wave->value >= 1.0)
        start_wave = (unsigned short)it_start_wave->value;
    unsigned short starting_weapon = (unsigned short)bt_weapon_id_from_name(dict_get_str(env_kwargs, "starting_weapon"));
    /* Endless-mode run option, an OPTIONAL [env] key (dict_find, like puf_init);
     * 0 by default so a config without `endless` stays campaign. */
    int endless = 0;
    /* Audio-RNG model, mirroring brotato.h puf_init: OPTIONAL [env] key,
     * default 1 (legacy source audio draws), 0 = bridge-neutralized. */
    int audio_rng = 1;
    {
        DictItem* it_audio = dict_find(env_kwargs, "audio_rng");
        if (it_audio) audio_rng = (it_audio->value != 0.0) ? 1 : 0;
    }
    int particles_rng = 1;
    {
        DictItem* it_part = dict_find(env_kwargs, "particles_rng");
        if (it_part) particles_rng = (it_part->value != 0.0) ? 1 : 0;
    }
    /* DebugService.invulnerable, mirroring brotato.h puf_init: OPTIONAL [env]
     * key, default 0 (normal player hurtbox), 1 = hurtbox disabled. */
    int invulnerable = 0;
    {
        DictItem* it_inv = dict_find(env_kwargs, "invulnerable");
        if (it_inv) invulnerable = (it_inv->value != 0.0) ? 1 : 0;
    }
    {
        DictItem* it_endless = dict_find(env_kwargs, "endless");
        if (it_endless && it_endless->value != 0.0) endless = 1;
    }
    /* Loot pool unlock profile, mirroring brotato.h puf_init (progress_data.gd:
     * 884-895): an OPTIONAL `unlock_all` key, default 1 = fully unlocked (the
     * guide's setup); 0 models a fresh save. */
    int unlock_all = 1;
    {
        DictItem* it_unlock = dict_find(env_kwargs, "unlock_all");
        if (it_unlock != NULL) unlock_all = (it_unlock->value != 0.0) ? 1 : 0;
    }
    /* Optional warp-per-env stepping: one warp per env with the physics
     * wavefront armed (brotato_gpu_warp.cuh). OPTIONAL [env] key, dict_find like
     * `endless`, so a config without `warp_step` stays on the plain kernel. */
    g_gpu.use_warp = 0;
    g_gpu.warp_scratch = NULL;
#if defined(BTFW_EXPERIMENTAL_LANE_SCHED)
    g_gpu.warp_scratch_bytes = btfw_scratch_bytes();
    {
        DictItem* it_warp = dict_find(env_kwargs, "warp_step");
        if (it_warp && it_warp->value != 0.0) g_gpu.use_warp = 1;
    }
#else
    /* Production build: the warp tick is compiled out entirely, so the
     * `[env] warp_step` key has no effect and nothing warp-related exists. */
    g_gpu.warp_scratch_bytes = 0;
#endif

    Env* envs = NULL;
    bt_gpu_check(cudaMalloc((void**)&envs, (size_t)n * sizeof(Env)), "cudaMalloc(envs)");
    // Every byte a backend leaves untouched is zero in the CPU environment
    // (src/pufferl.cu callocs the Env array) and in Metal (brotato_metal.mm
    // memsets the state buffer). bt_core_init/bt_core_reset do not write every
    // field and never write padding, so a bare cudaMalloc would both break
    // field-by-field CPU/CUDA equality and leave any field the core reads
    // before writing with a different value on each backend.
    bt_gpu_check(cudaMemset(envs, 0, (size_t)n * sizeof(Env)), "zero envs");
    bt_gpu_layout_create(envs, n);
    // Shaping weights from the [env] ini section (brotato_reward.h). The CPU env
    // calls bt_reward_cfg_init(&env->state, kwargs) right after bt_core_init;
    // the state lives on the device here, so the same function runs once on a
    // host scratch BtState and the resulting POD is handed to the init kernel.
    // Using the core's own derivation keeps the two backends from drifting.
    struct BtReward rew_cfg;
#if defined(__CUDA_ARCH__)
    rew_cfg = (struct BtReward){};   // device pass parses this host code; never executed there
#else
    {
        static BtState bt_cfg_scratch;   // 6.4 MiB, touched once per create
        bt_reward_cfg_init(&bt_cfg_scratch, env_kwargs);
        rew_cfg = bt_cfg_scratch.rew;
    }
#endif
    // Build persistent state on the device instead of a host array + H2D copy.
    // The one create-time sync covers later puf_reset/puf_step on g_gpu.stream.
    /* Argument order must match the kernel signature exactly:
     * (..., character, starting_weapon, audio_rng, particles_rng, invulnerable, start_wave, rew_cfg, ...). */
    bt_gpu_cap_reset();   // clean latch before the init kernels write it
    bt_gpu_init_kernel<<<bt_gpu_grid(n), BT_GPU_BLOCK>>>(envs, n, zone, shop_steps, character,
        starting_weapon, audio_rng, particles_rng, invulnerable, start_wave, rew_cfg, observations, rewards,
        terminals, endless, unlock_all);
    bt_gpu_check(cudaGetLastError(), "init kernel launch");
    bt_gpu_check(cudaDeviceSynchronize(), "env init");
    if (bt_gpu_cap_faulted("puf_vec_create/bt_gpu_init_kernel")) exit(1);

    g_gpu.envs = envs;
    g_gpu.n = n;
    g_gpu.observations = observations;
    g_gpu.actions = actions;
    g_gpu.rewards = rewards;
    g_gpu.terminals = terminals;
    g_gpu.action_mask = NULL;   // puf_bind_action_mask, before the first rollout
    g_gpu.mask_size = 0;
    g_gpu.stream = 0;
    g_gpu.render_open = 0;
#if defined(BTFW_EXPERIMENTAL_LANE_SCHED)
    /* Arm the warp path last: allocate the per-env scratch and switch the
     * physics hook to the wavefront. Off unless [env] warp_step is set, and
     * forced off when this build has no physics backend (btfw_scratch_bytes()
     * == 0), so the flag can never silently select a serial-physics warp path. */
    if (g_gpu.use_warp) {
#if defined(BTFW_HAVE_PHYSICS)
        if (g_gpu.warp_scratch_bytes > 0) {
            bt_gpu_check(cudaMalloc(&g_gpu.warp_scratch,
                    (size_t)n * g_gpu.warp_scratch_bytes), "warp scratch");
            /* NOTE: the two process-wide mode globals (btf_phys_backend,
             * btf_group_bp) are written ONCE below, for both modes - see the
             * "tick mode" block. Writing them here only when use_warp==1 would
             * leave a stale bp=32 armed for a later plain batch in the same
             * process, which would then run BT_SERIAL on lane 0 only and step a
             * single env per block, silently. */
        } else {
            g_gpu.use_warp = 0;
        }
#else
        g_gpu.use_warp = 0;
#endif
    }
#endif  /* BTFW_EXPERIMENTAL_LANE_SCHED */
#if defined(BTFW_EXPERIMENTAL_LANE_SCHED)
    /* TICK MODE, written for EVERY create (1 = warp tick, 0 = plain), because
     * btf_phys_backend and btf_group_bp are process-wide device globals: arming
     * them only when use_warp==1 leaves bp=32 armed for a later plain batch in the
     * same process (eval after train, or a second batch), and that batch would
     * then execute BT_SERIAL on lane 0 only and step a single env per block -
     * silently. A second LIVE batch that asks for a different mode is refused
     * instead of letting the last create win. */
    {
        const int mode = g_gpu.use_warp ? 1 : 0;
        if (was_live && g_gpu.armed_mode != mode) {
            fprintf(stderr,
                "Brotato CUDA: refusing to create a second live env batch with a "
                "different tick mode (live=%d requested=%d): btf_group_bp and "
                "btf_phys_backend are process-wide, so one of the two batches "
                "would silently step with the wrong lane discipline\n",
                g_gpu.armed_mode, mode);
            exit(1);
        }
        if (!was_live) g_gpu.armed_mode = mode;
        int backend = mode;
        int group_bp = mode ? 32 : 1;
        bt_gpu_check(cudaMemcpyToSymbol(btf_phys_backend, &backend, sizeof(int)),
                     "tick mode: physics backend");
        bt_gpu_check(cudaMemcpyToSymbol(btf_group_bp, &group_bp, sizeof(int)),
                     "tick mode: group bp");
    }
#endif
    return envs;
}
void puf_bind_stream(cudaStream_t stream) {
    g_gpu.stream = stream;
}

// GPU setup happens once in puf_vec_create; pufferl never calls puf_init on the
// GPU path, but the symbol must exist with the CPU signature.
void puf_init(Env* env, Dict*) {
    env->num_agents = 1;
}

void puf_reset(Env* env) {
    (void)env;
    if (g_gpu.envs == NULL || g_gpu.n <= 0) return;
    bt_gpu_reset_kernel<<<bt_gpu_grid(g_gpu.n), BT_GPU_BLOCK, 0, g_gpu.stream>>>(
        g_gpu.envs, g_gpu.observations, g_gpu.rewards, g_gpu.terminals,
        g_gpu.action_mask, g_gpu.mask_size, g_gpu.n);
    bt_gpu_check(cudaGetLastError(), "reset kernel launch");
    /* Rollout boundary: a capacity loss anywhere in the batch fails the run here,
     * before the learner consumes any of it. */
    if (g_gpu.stream) bt_gpu_check(cudaStreamSynchronize(g_gpu.stream), "reset sync");
    if (bt_gpu_cap_faulted("puf_reset/bt_gpu_reset_kernel")) exit(1);
}

void puf_step(Env* env) {
    (void)env;
    if (g_gpu.envs == NULL || g_gpu.n <= 0) return;
#if defined(BTFW_EXPERIMENTAL_LANE_SCHED)
    if (g_gpu.use_warp) {
        /* One warp per env (blockDim 32, env == blockIdx.x); stride = sizeof(Env)
         * addresses each env's BtState, whose union member sits at offset 0.
         * H=1, no policy, optional cycles; the action mask is written by the
         * kernel exactly like bt_gpu_step_kernel. */
        btfw_step_kernel<<<g_gpu.n, 32, 0, g_gpu.stream>>>(&g_gpu.envs[0].state,
                sizeof(Env), g_gpu.actions, g_gpu.observations, g_gpu.rewards,
                g_gpu.terminals, g_gpu.n, 1, 0, g_gpu.warp_scratch, NULL,
                g_gpu.action_mask, g_gpu.mask_size);
    } else
#endif  /* BTFW_EXPERIMENTAL_LANE_SCHED */
    {
        bt_gpu_step_kernel<<<bt_gpu_grid(g_gpu.n), BT_GPU_BLOCK, 0, g_gpu.stream>>>(
            g_gpu.envs, g_gpu.actions, g_gpu.observations,
            g_gpu.rewards, g_gpu.terminals, g_gpu.action_mask, g_gpu.mask_size, 0, g_gpu.n);
    }
    bt_gpu_check(cudaGetLastError(), "step kernel launch");
}

void puf_close(Env* env) {
    (void)env;
    bt_gpu_layout_close();
    if (g_gpu.warp_scratch != NULL) {
        cudaFree(g_gpu.warp_scratch);
        g_gpu.warp_scratch = NULL;
    }
    if (g_gpu.envs != NULL) {
        cudaFree(g_gpu.envs);
        g_gpu.envs = NULL;
    }
    if (g_gpu.render_open) {
        CloseWindow();
        g_gpu.render_open = 0;
    }
    g_gpu.n = 0;
}

void puf_log(Log* log, Dict* out) {
    /* Rollout boundary: fail closed before the learner consumes the batch. This
     * is the second host-side read of the device capacity latch (the first is
     * puf_reset); puf_step itself stays fully async and never synchronizes. */
    if (g_gpu.envs != NULL) {
        if (g_gpu.stream) bt_gpu_check(cudaStreamSynchronize(g_gpu.stream), "puf_log sync");
        if (bt_gpu_cap_faulted("puf_log/bt_gpu_step_kernel")) exit(1);
    }
    dict_set(out, "perf", log->perf);
    dict_set(out, "score", log->score);
    dict_set(out, "episode_return", log->episode_return);
    dict_set(out, "episode_length", log->episode_length);
    dict_set(out, "n", log->n);
    /* Endless mode: the highest wave the run reached (0 for a campaign run). */
    dict_set(out, "endless_wave", log->endless_wave);
#ifndef __CUDA_ARCH__
    /* Potential-based shaping ledger (brotato_reward.h), exactly the CPU
     * wrapper's puf_log: one key per Phi group plus their sum. */
    dict_set(out, "rw_total", log->rw_total);
    for (int i = 0; i < BT_RW_NTERMS; i++) dict_set(out, BT_RW_TERM_NAME[i], log->rw[i]);
#endif
}

// ---------------------------------------------------------------------------
// Renderer. D2H-snapshots env 0's BtState and draws it with raylib. Fully
// implemented (never a silent no-op): arena, enemies, projectiles, consumables,
// player, HUD and the shop / level-up overlay. No textures are loaded, and shop
// offers are labelled by canonical kind/id + tier/price rather than by name: the
// generated tables keep their `name`/`id`/`icon` fields behind BT_HOST_ONLY,
// which the device pass expands away, and nvcc parses host function bodies in
// that pass too.
// ---------------------------------------------------------------------------
#ifndef BT_WINDOW
#define BT_WINDOW 960
#endif
#ifndef BT_HEIGHT
#define BT_HEIGHT 640
#endif

// BtState is ~1.04 MiB, far too large for a stack frame; the renderer is
// single-threaded so one static snapshot buffer is enough.
static BtState g_render_state;

// Optional real-game sprites. Point BROTATO_ASSETS at a local directory holding
// `kind_paths.txt` / `weapon_paths.txt` (one PNG path per BT kind / weapon id,
// empty line = none) and a `game/` copy of the recovered project for the fixed
// sprites. No game art ships with the repo: without BROTATO_ASSETS the viewer
// draws the primitive shapes below.
typedef struct {
    int on;
    Texture2D kind[BT_NKINDS];
    Texture2D weapon[BT_NWEAPONS];
    Texture2D potato, legs, eyes, mouth, ear_b, ear_f, tail;
    Texture2D material, fruit, box, bullet, bullet_enemy;
} BtSprites;
static BtSprites g_spr;

static Texture2D bt_spr_load(const char* path) {
    Texture2D t = {0};
    if (path && path[0] && FileExists(path)) {
        t = LoadTexture(path);
        SetTextureFilter(t, TEXTURE_FILTER_BILINEAR);
    }
    return t;
}

static void bt_spr_load_list(const char* dir, const char* file, Texture2D* out, int n) {
    FILE* f = fopen(TextFormat("%s/%s", dir, file), "r");
    if (!f) return;
    char line[1024];
    for (int i = 0; i < n && fgets(line, sizeof(line), f); i++) {
        line[strcspn(line, "\r\n")] = 0;
        out[i] = bt_spr_load(line);
    }
    fclose(f);
}

static void bt_spr_init(void) {
    const char* dir = getenv("BROTATO_ASSETS");
    if (!dir || !dir[0]) return;
    bt_spr_load_list(dir, "kind_paths.txt", g_spr.kind, BT_NKINDS);
    bt_spr_load_list(dir, "weapon_paths.txt", g_spr.weapon, BT_NWEAPONS);
#define BT_SPR(field, rel) g_spr.field = bt_spr_load(TextFormat("%s/game/%s", dir, rel))
    BT_SPR(potato, "entities/units/player/potato.png");
    BT_SPR(legs, "entities/units/player/legs.png");
    BT_SPR(eyes, "items/characters/demon/demon_eyes.png");
    BT_SPR(mouth, "items/characters/demon/demon_mouth.png");
    BT_SPR(ear_b, "items/characters/demon/demon_ear_back.png");
    BT_SPR(ear_f, "items/characters/demon/demon_ear_front.png");
    BT_SPR(tail, "items/characters/demon/demon_tail.png");
    BT_SPR(material, "items/materials/material_0000.png");
    BT_SPR(fruit, "items/consumables/fruit/fruit.png");
    BT_SPR(box, "items/consumables/item_box/item_box.png");
    BT_SPR(bullet, "projectiles/bullet_ghost_scepter/bullet_scepter_0000.png");
    BT_SPR(bullet_enemy, "projectiles/bullet_enemy/bullet_enemy.png");
#undef BT_SPR
    g_spr.on = g_spr.potato.id != 0;
}

// Draw a texture centred at screen (x, y), scaled by sc, rotated (degrees),
// optionally mirrored horizontally.
static void bt_spr_draw(Texture2D t, float x, float y, float sc, float deg, int flip, Color tint) {
    if (t.id == 0) return;
    float w = (float)t.width, h = (float)t.height;
    Rectangle src = {0, 0, flip ? -w : w, h};
    Rectangle dst = {x, y, w * sc, h * sc};
    DrawTexturePro(t, src, dst, (Vector2){w * sc * 0.5f, h * sc * 0.5f}, deg, tint);
}

void puf_render(Env* env) {
    (void)env;
    if (g_gpu.envs == NULL || g_gpu.n < 1) return;
    if (g_gpu.stream) {
        bt_gpu_check(cudaStreamSynchronize(g_gpu.stream), "render stream sync");
    }

    bt_gpu_layout_pack(g_gpu.envs, 0, 1, g_gpu.stream);
    BtState& s = g_render_state;
    cudaError_t err = cudaMemcpy(&s, &g_gpu.envs->state, sizeof(BtState),
        cudaMemcpyDeviceToHost);
    if (err != cudaSuccess) {
        fprintf(stderr, "Brotato render: BtState D2H failed: %s\n",
            cudaGetErrorString(err));
        return;
    }

    if (!g_gpu.render_open) {
        InitWindow(BT_WINDOW, BT_HEIGHT, "Brotato RL (CUDA)");
        SetTargetFPS(60);
        bt_spr_init();
        g_gpu.render_open = 1;
    }
    if (IsKeyDown(KEY_ESCAPE)) exit(0);
    if (IsKeyPressed(KEY_TAB)) ToggleFullscreen();

    const float sc = (float)BT_WINDOW / BT_ARENA_W;
    BeginDrawing();
    if (g_spr.on) {
        ClearBackground((Color){113, 98, 84, 255});   /* tiles_1.png mean colour */
    } else {
        ClearBackground((Color){26, 30, 24, 255});
        for (int gx = 0; gx < (int)BT_ARENA_W; gx += 80) {
            DrawLine((int)(gx * sc), 0, (int)(gx * sc), BT_HEIGHT, (Color){34, 40, 32, 255});
        }
        for (int gy = 0; gy < (int)BT_ARENA_H; gy += 80) {
            DrawLine(0, (int)(gy * sc), BT_WINDOW, (int)(gy * sc), (Color){34, 40, 32, 255});
        }
    }

    if (g_spr.on) {
        for (int i = 0; i < BT_MAX_MAT; i++) {
            if (!s.matalive[i]) continue;
            bt_spr_draw(g_spr.material, s.matx[i] * sc, s.maty[i] * sc, sc, 0.0f, 0, WHITE);
        }
    }

    for (int i = 0; i < BT_MAX; i++) {
        if (!s.ealive[i] && !s.edying[i]) continue;
        int k = (int)s.ekind[i];
        if (g_spr.on && k >= 0 && k < BT_NKINDS && g_spr.kind[k].id != 0) {
            Color tint = s.edying[i] ? (Color){255, 255, 255, 110} : WHITE;
            bt_spr_draw(g_spr.kind[k], s.ex[i] * sc, s.ey[i] * sc, sc, 0.0f,
                        s.px < s.ex[i], tint);
            continue;
        }
        if (!s.ealive[i]) continue;
        Color c = s.eelite[i] ? (Color){240, 120, 60, 255}
                : s.ekind[i] == 2 ? (Color){200, 70, 90, 255}
                : s.ekind[i] == 1 ? (Color){170, 90, 110, 255}
                                  : (Color){140, 110, 120, 255};
        DrawCircle((int)(s.ex[i] * sc), (int)(s.ey[i] * sc),
                   s.eelite[i] ? 16 : 10, c);
    }
    for (int i = 0; i < BT_MAX_PROJ; i++) {
        if (!s.pralive[i]) continue;
        int cx = (int)(s.prx[i] * sc), cy = (int)(s.pry[i] * sc);
        /* Friendly shots (player/pet/turret) are the bright gold, enemy shots
         * red. The drawn surface mirrors bt_projectile_hits (brotato_core.h):
         * an oriented rect for BT_SHAPE_SLASH and BT_SHAPE_PILLAR, a circle
         * otherwise, so the viewer shows the real collision footprint. */
        Color c = s.prfriendly[i] ? (Color){250, 220, 120, 255}
                                  : (Color){230, 110, 130, 255};
        float prot = s.prrot[i];
        float deg = prot * 57.2957795f;
        if (s.prshape[i] == BT_SHAPE_SLASH) {
            float ox = cosf(prot) * BT_SLASH_OFF_X - sinf(prot) * BT_SLASH_OFF_Y;
            float oy = sinf(prot) * BT_SLASH_OFF_X + cosf(prot) * BT_SLASH_OFF_Y;
            float hx = s.prhx[i], hy = s.prhy[i];
            DrawRectanglePro((Rectangle){(float)cx + ox, (float)cy + oy, 2.0f * hx, 2.0f * hy},
                             (Vector2){hx, hy}, deg, c);
        } else if (s.prshape[i] == BT_SHAPE_PILLAR) {
            float ox = cosf(prot) * BT_PILLAR_OFF_X - sinf(prot) * BT_PILLAR_OFF_Y;
            float oy = sinf(prot) * BT_PILLAR_OFF_X + cosf(prot) * BT_PILLAR_OFF_Y;
            float r = s.prrad[i];
            DrawRectanglePro((Rectangle){(float)cx + ox, (float)cy + oy, 2.0f * r, 2.0f * s.prhy[i]},
                             (Vector2){r, s.prhy[i]}, deg, c);
        } else if (g_spr.on && (s.prfriendly[i] ? g_spr.bullet.id : g_spr.bullet_enemy.id) != 0) {
            bt_spr_draw(s.prfriendly[i] ? g_spr.bullet : g_spr.bullet_enemy,
                        (float)cx, (float)cy, sc, deg, 0, WHITE);
        } else {
            DrawCircle(cx, cy, s.prrad[i] > 0.0f ? s.prrad[i] * sc : 4.0f, c);
        }
    }
    for (int i = 0; i < BT_MAX_CONS; i++) {
        if (!s.conalive[i]) continue;
        if (g_spr.on) {
            bt_spr_draw(s.conkind[i] == 0 ? g_spr.fruit : g_spr.box,
                        s.conx[i] * sc, s.cony[i] * sc, sc, 0.0f, 0, WHITE);
            continue;
        }
        DrawRectangle((int)(s.conx[i] * sc) - 5, (int)(s.cony[i] * sc) - 5, 10, 10,
                      (Color){120, 230, 150, 255});
    }

    if (g_spr.on) {
        /* Demon: potato body + legs + the character's appearance layers, the
         * same stacking order as the player scene (back ear/tail behind). */
        float x = s.px * sc, y = s.py * sc;
        int flip = 0;
        bt_spr_draw(g_spr.tail, x, y, sc, 0.0f, flip, WHITE);
        bt_spr_draw(g_spr.ear_b, x, y, sc, 0.0f, flip, WHITE);
        bt_spr_draw(g_spr.legs, x, y + 18.0f * sc, sc, 0.0f, flip, WHITE);
        bt_spr_draw(g_spr.potato, x, y, sc, 0.0f, flip, WHITE);
        bt_spr_draw(g_spr.eyes, x, y, sc, 0.0f, flip, WHITE);
        bt_spr_draw(g_spr.mouth, x, y, sc, 0.0f, flip, WHITE);
        bt_spr_draw(g_spr.ear_f, x, y, sc, 0.0f, flip, WHITE);
        /* Owned weapons on the game's ring around the player. */
        int nw = (int)s.wpn_count;
        for (int w = 0; w < nw && w < BT_SLOTS; w++) {
            unsigned short wid = s.wpn_id[w];
            if (wid >= BT_NWEAPONS || g_spr.weapon[wid].id == 0) continue;
            float a = -1.5707963f + 6.2831853f * (float)w / (float)(nw > 0 ? nw : 1);
            bt_spr_draw(g_spr.weapon[wid], x + cosf(a) * 55.0f * sc, y + sinf(a) * 55.0f * sc,
                        sc, 0.0f, 0, WHITE);
        }
    } else {
        DrawCircle((int)(s.px * sc), (int)(s.py * sc), 14, (Color){235, 210, 120, 255});
        DrawCircleLines((int)(s.px * sc), (int)(s.py * sc), (int)(96.0f * sc),
                        (Color){235, 210, 120, 60});
    }

    /* Equipped weapons: the melee swing surface is the exact source tween
     * geometry from bt_melee_visual (origin/rotation for the current swing
     * frame), bright while the hit window is open, and each owned slot gets a
     * HUD marker. The CUDA pass compiles the numeric-only tables, so markers are
     * labelled by canonical id, exactly like the item offers below. */
    for (int i = 0; i < (int)s.wpn_count && i < BT_SLOTS; i++) {
        unsigned short wid = s.wpn_id[i];
        if (s.wpn_melee_shooting[i]) {
            float ox = 0.0f, oy = 0.0f, rot = 0.0f;
            bt_melee_visual(&s, i, &ox, &oy, &rot);
            float sx = (float)(s.px + ox) * sc;
            float sy = (float)(s.py + oy) * sc;
            Color wc = s.wpn_melee_hit_on[i] ? (Color){255, 240, 160, 220} : (Color){200, 200, 210, 120};
            DrawRectanglePro((Rectangle){sx, sy, 34.0f, 6.0f}, (Vector2){0, 3},
                             rot * 57.2957795f, wc);
        }
        int bx = BT_WINDOW - 34 - i * 28, by = BT_HEIGHT - 34;
        DrawRectangleLines(bx - 11, by - 11, 22, 22,
                           s.wpn_melee_hit_on[i] ? (Color){255, 240, 160, 255} : (Color){140, 150, 170, 255});
        const char* kind = (wid < BT_NWEAPONS && BT_WEAPONS[wid].kind == BT_WEAPON_KIND_MELEE) ? "M" : "R";
        DrawText(TextFormat("%s%d", kind, (int)wid), bx - 9, by - 6, 12, RAYWHITE);
    }

    DrawText(TextFormat("wave %d/%d", s.wave, BT_WAVES), 12, 10, 24, RAYWHITE);
    DrawText(TextFormat("%.0fs  char %d", s.time_left, (int)s.character), 12, 38, 20, GRAY);
    DrawText(TextFormat("kills %d", s.kills), 200, 10, 24, (Color){240, 200, 120, 255});
    DrawText(TextFormat("hp %.0f/%.0f", s.hp, s.max_hp), 200, 38, 20,
             s.hp < s.max_hp * 0.35f ? (Color){240, 90, 90, 255} : RAYWHITE);
    DrawText(TextFormat("regen %.0f  armor %.0f", s.regen, s.armor), 420, 10, 20,
             (Color){150, 200, 160, 255});
    DrawText(TextFormat("enemies %d", s.ecount), 420, 34, 20, (Color){180, 150, 150, 255});
    DrawText(TextFormat("materials %.0f", s.materials), 640, 10, 20,
             (Color){120, 220, 160, 255});
    DrawText(TextFormat("level %d   dmg%% %.0f   armor %.0f", s.level, s.dmg_pct, s.armor),
             640, 34, 18, (Color){160, 200, 220, 255});
    DrawText(TextFormat("dodge %.0f%%  crit %.0f%%  speed%% %.0f", s.dodge * 100.0f,
                        s.crit * 100.0f, s.speed_pct),
             640, 56, 16, (Color){150, 170, 200, 255});

    if (s.phase == 1 || s.phase == 2) {
        int px0 = 120, py0 = 150, pw = BT_WINDOW - 240, ph = 380;
        DrawRectangle(px0, py0, pw, ph, (Color){18, 20, 26, 240});
        DrawRectangleLines(px0, py0, pw, ph, (Color){120, 160, 200, 255});
        const char* title = s.phase == 1 ? "SHOP  -  choose 0..3, 4 reroll, 8 skip"
                                         : "LEVEL UP  -  choose 0..3";
        DrawText(title, px0 + 20, py0 + 14, 22, (Color){235, 220, 140, 255});
        DrawText(TextFormat("wave %d   materials %.0f   level %d", s.wave, s.materials, s.level),
                 px0 + 20, py0 + 44, 18, RAYWHITE);
        for (int i = 0; i < 4; i++) {
            int y = py0 + 84 + i * 64;
            if (s.phase == 1) {
                // Canonical offer identity (shop_kind/shop_id). The CUDA pass
                // compiles the numeric-only device tables (no `const char* name`
                // / `icon`), so offers are labelled by canonical kind + id /
                // tier / price rather than by name. A NONE slot (empty or
                // already bought) is left blank, never rendered as item[0];
                // price/affordability use the shop's own currency helpers.
                unsigned char kind = s.shop_kind[i];
                unsigned short id = s.shop_id[i];
                const char* unit = bt_wshop_hp_shop(&s) ? "hp" : "mat";
                if (kind == BT_SHOP_ITEM) {
                    const BtItem* it = &BT_ITEMS[id];
                    float price = bt_wshop_price_units(&s, BT_SHOP_ITEM, id);
                    int afford = bt_wshop_currency(&s) >= price;
                    DrawText(TextFormat("[%d] item #%d  T%d  %.0f %s%s", i, (int)id, it->tier,
                                        price, unit, s.shop_cursed[i] ? "  cursed" : ""),
                             px0 + 76, y, 20, afford ? RAYWHITE : (Color){110, 110, 120, 255});
                    for (int e = 0; e < it->count; e++) {
                        const BtItemEffect* ef = &BT_ITEM_EFFECTS[it->first + e];
                        DrawText(TextFormat("%s %+.0f", bt_stat_display_name(ef->stat), ef->value),
                                 px0 + 430 + e * 110, y, 18, (Color){150, 210, 170, 255});
                    }
                } else if (kind == BT_SHOP_WEAPON) {
                    const BtWeaponDef* wp = &BT_WEAPONS[id];
                    float price = bt_wshop_price_units(&s, BT_SHOP_WEAPON, id);
                    int afford = bt_wshop_can_buy(&s, id) && bt_wshop_currency(&s) >= price;
                    DrawText(TextFormat("[%d] weapon #%d  T%d %s  %.0f %s%s", i, (int)id, wp->tier,
                                        wp->kind == BT_WEAPON_KIND_MELEE ? "M" : "R", price, unit,
                                        s.shop_cursed[i] ? "  cursed" : ""),
                             px0 + 76, y, 20, afford ? RAYWHITE : (Color){110, 110, 120, 255});
                    DrawText(TextFormat("dmg %d  cd %d", wp->damage, wp->cooldown),
                             px0 + 430, y, 18, (Color){150, 210, 170, 255});
                } else {
                    DrawText(TextFormat("[%d] --", i), px0 + 76, y, 20,
                             (Color){90, 95, 105, 255});
                }
            } else {
                const BtUpgrade* up = &BT_UPGRADES[s.lvl_offers[i]];
                DrawText(TextFormat("[%d] %s  +%.0f  (tier %d)", i, bt_stat_display_name(up->stat),
                                    up->value, up->tier), px0 + 30, y, 20, RAYWHITE);
            }
        }
    }

    EndDrawing();
    puf_web_vsync();
}

#endif
