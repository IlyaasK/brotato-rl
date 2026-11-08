// brotato_gpu_fast.cuh -- prototype execution models for the Brotato CUDA
// backend. OWNER: PerfGpuKernel. Builds on brotato_gpu_framework.cuh (lane
// mapping, sync, ordered compaction, wavefront runner, draw runner, digest,
// phase map); this file adds the kernels the throughput harness measures.
//
// INTEGRATION
//   brotato.cu's Env holds one BtState at offset 0 (`&envs[i].state`), and the
//   kernels here address env i as `(char*)base + i*stride`, so a caller passes
//   `&envs[0].state` with `stride = sizeof(Env)` and needs no type dependency on
//   Env. Nothing here allocates or copies a BtState per step.
//
//   brotato.cu already defines the device-pass name-resolver stubs and the same
//   core include; when including this file from there, define BT_NO_NAME_STUBS
//   first so the two sets do not clash.
//
// KERNELS
//   btf_step_kernel         baseline: one thread per env (the brotato.cu model,
//                           with a selectable block size).
//   btf_step_lb_kernel      same with __launch_bounds__(256), for the register /
//                           occupancy sweep.
//   btf_step_h_kernel       H frames per launch: one launch advances the whole
//                           batch H ticks. Bit-exact (envs are independent; each
//                           env receives its ticks in the same order as H
//                           separate launches). Cuts the trainer's rollout-loop
//                           launch overhead by 1/H.
//   btf_phys_only_kernel    bt_phys_step only (stage cost probe).
//   btf_observe_only_kernel bt_core_observe only (observe cost probe).
//   btf_step_timed_kernel   bt_core_step with a clock64 cycle counter per env.
#ifndef PUFFER_BROTATO_GPU_FAST_CUH
#define PUFFER_BROTATO_GPU_FAST_CUH

#include "brotato_gpu_framework.cuh"

// ---------------------------------------------------------------------------
// Baseline: one thread per env.
// ---------------------------------------------------------------------------
__global__ void btf_step_kernel(BtState* __restrict__ base, size_t stride,
        const float* __restrict__ actions, float* __restrict__ obs,
        float* __restrict__ rewards, float* __restrict__ terminals, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    BtState* s = BTF_AT(base, stride, i);
    float r = 0.0f, t = 0.0f;
    bt_core_step(s, (int)actions[i], obs + (size_t)i * BTF_OBS_SIZE, &r, &t);
    rewards[i] = r;
    terminals[i] = t;
}

__global__ void __launch_bounds__(256) btf_step_lb_kernel(BtState* __restrict__ base,
        size_t stride, const float* __restrict__ actions, float* __restrict__ obs,
        float* __restrict__ rewards, float* __restrict__ terminals, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    BtState* s = BTF_AT(base, stride, i);
    float r = 0.0f, t = 0.0f;
    bt_core_step(s, (int)actions[i], obs + (size_t)i * BTF_OBS_SIZE, &r, &t);
    rewards[i] = r;
    terminals[i] = t;
}

// ---------------------------------------------------------------------------
// Horizon-fused step: one launch advances every env H frames.
// actions/obs/rewards/terminals are [H][n] per-frame slices.
// ---------------------------------------------------------------------------
__global__ void btf_step_h_kernel(BtState* __restrict__ base, size_t stride,
        const float* __restrict__ actions, float* __restrict__ obs,
        float* __restrict__ rewards, float* __restrict__ terminals,
        int n, int H) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    BtState* s = BTF_AT(base, stride, i);
    for (int h = 0; h < H; h++) {
        size_t idx = (size_t)h * (size_t)n + (size_t)i;
        float r = 0.0f, t = 0.0f;
        bt_core_step(s, (int)actions[idx], obs + idx * BTF_OBS_SIZE, &r, &t);
        rewards[idx] = r;
        terminals[idx] = t;
    }
}

// ---------------------------------------------------------------------------
// Cost probes + per-env cycle timing.
// ---------------------------------------------------------------------------
__global__ void btf_phys_only_kernel(BtState* __restrict__ base, size_t stride, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    bt_phys_step(&BTF_AT(base, stride, i)->phys);
}

__global__ void btf_observe_only_kernel(BtState* __restrict__ base, size_t stride,
        float* __restrict__ obs, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    bt_core_observe(BTF_AT(base, stride, i), obs + (size_t)i * BTF_OBS_SIZE);
}

__global__ void btf_step_timed_kernel(BtState* __restrict__ base, size_t stride,
        const float* __restrict__ actions, float* __restrict__ obs,
        float* __restrict__ rewards, float* __restrict__ terminals, int n,
        unsigned long long* __restrict__ cycles) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    BtState* s = BTF_AT(base, stride, i);
    float r = 0.0f, t = 0.0f;
    unsigned long long c0 = clock64();
    bt_core_step(s, (int)actions[i], obs + (size_t)i * BTF_OBS_SIZE, &r, &t);
    unsigned long long c1 = clock64();
    rewards[i] = r;
    terminals[i] = t;
    cycles[i] = c1 - c0;
}

#endif // PUFFER_BROTATO_GPU_FAST_CUH
