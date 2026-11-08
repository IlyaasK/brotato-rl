/* Included by brotato.cu after Env is complete. Experimental, compile with
 * -DBT_CUDA_INTERLEAVE_ENEMIES=1; BT_CUDA_INTERLEAVE=0 selects the AoS control
 * in the same binary. BT_CUDA_INTERLEAVE_MASK optionally selects field bits in
 * BT_HOT_* order, allowing a same-build hottest-field sweep after profiling.
 * BT_CUDA_LAYOUT_PROFILE=1 counts subscripts per field/env on the real kernel
 * (never enable profiling for throughput runs). */
#pragma once
#if defined(BT_CUDA_HOT_ARRAYS)
#include <stddef.h>
#include <stdint.h>
#include <type_traits>
static_assert(std::is_trivially_copyable<BtHotEx>::value, "snapshot must remain POD");
static_assert(sizeof(BtHotEx) == BT_MAX * sizeof(float), "proxy changes state ABI");
struct BtCudaLayout {
    char* envs;
    unsigned char* data;
    unsigned long long* counts;
    int n, enabled;
};
static __constant__ BtCudaLayout bt_cuda_layout;
static BtCudaLayout bt_cuda_layout_host;
static __device__ __forceinline__ void* bt_cuda_hot_address(
        const void* array, int field, int index, int width, bool count_access) {
    const BtCudaLayout& l = bt_cuda_layout;
    uintptr_t offset = (uintptr_t)array - (uintptr_t)l.envs;
    if (offset >= (size_t)l.n * sizeof(Env))
        return (char*)array + (size_t)index * width;
    size_t env = offset / sizeof(Env);
    if (l.counts && count_access) l.counts[(size_t)field * l.n + env]++;
    if (!(l.enabled & (1u << field))) return (char*)array + (size_t)index * width;
    /* Float fields first, byte fields last. A warp's same-slot accesses are
     * consecutive 4-byte words (or bytes), including partial batches. */
    size_t field_byte = field < 6 ? (size_t)field * BT_MAX * sizeof(float)
                                : (size_t)(24 + field - 6) * BT_MAX;
    return l.data + field_byte * l.n + ((size_t)index * l.n + env) * width;
}
static void bt_gpu_layout_create(Env* envs, int n) {
    BtCudaLayout l = {};
    l.envs = (char*)envs;
    l.n = n;
    const char* enabled = getenv("BT_CUDA_INTERLEAVE");
    l.enabled = !enabled || atoi(enabled) != 0 ? (1 << BT_HOT_COUNT) - 1 : 0;
    const char* fields = getenv("BT_CUDA_INTERLEAVE_MASK");
    if (fields) l.enabled &= (unsigned int)strtoul(fields, NULL, 0);
    if (l.enabled) {
        bt_gpu_check(cudaMalloc((void**)&l.data, (size_t)n * BT_MAX * 27), "layout allocation");
        bt_gpu_check(cudaMemset(l.data, 0, (size_t)n * BT_MAX * 27), "layout zero");
    }
    const char* profile = getenv("BT_CUDA_LAYOUT_PROFILE");
    if (profile && atoi(profile)) {
        bt_gpu_check(cudaMalloc((void**)&l.counts, (size_t)n * BT_HOT_COUNT * sizeof(*l.counts)), "layout counters");
        bt_gpu_check(cudaMemset(l.counts, 0, (size_t)n * BT_HOT_COUNT * sizeof(*l.counts)), "layout counters zero");
    }
    bt_gpu_check(cudaMemcpyToSymbol(bt_cuda_layout, &l, sizeof(l)), "layout bind");
    bt_cuda_layout_host = l;
}
static void bt_gpu_layout_profile_reset() {
    const BtCudaLayout& l = bt_cuda_layout_host;
    if (l.counts) bt_gpu_check(cudaMemset(l.counts, 0,
        (size_t)l.n * BT_HOT_COUNT * sizeof(*l.counts)), "layout counter reset");
}
static void bt_gpu_layout_profile_report() {
    const BtCudaLayout& l = bt_cuda_layout_host;
    if (!l.counts) return;
    size_t bytes = (size_t)l.n * BT_HOT_COUNT * sizeof(*l.counts);
    unsigned long long* h = (unsigned long long*)malloc(bytes);
    if (!h) { fprintf(stderr, "layout counter host allocation failed\n"); exit(1); }
    bt_gpu_check(cudaMemcpy(h, l.counts, bytes, cudaMemcpyDeviceToHost), "layout counter read");
    const char* names[] = {"ex", "ey", "ehp", "emax", "ehx", "ehy", "ealive", "ekind", "edying"};
    for (int f = 0; f < BT_HOT_COUNT; f++) {
        unsigned long long total = 0;
        for (int e = 0; e < l.n; e++) total += h[(size_t)f * l.n + e];
        printf("layout_accesses %s %llu\n", names[f], total);
    }
    free(h);
}
/* Materialize only the proxy-backed fields in their reserved canonical slots.
 * This is exclusively a snapshot boundary, not part of any simulation tick.
 * .raw bypasses the proxy, so snapshots contain true logical state. */
template<class T, int N, int Field>
static __device__ void bt_gpu_layout_pack_array(BtCudaHotArray<T, N, Field>& a, int i) {
    a.raw[i] = *(const T*)bt_cuda_hot_address(&a, Field, i, sizeof(T), false);
}
__global__ void bt_gpu_layout_pack_kernel(Env* envs, int start, int count) {
    int env = blockIdx.x;
    if (env >= count) return;
    BtState* s = &envs[start + env].state;
    for (int i = threadIdx.x; i < BT_MAX; i += blockDim.x) {
#define BT_PACK(name) bt_gpu_layout_pack_array(s->name, i)
        BT_PACK(ex); BT_PACK(ey); BT_PACK(ehp); BT_PACK(emax);
        BT_PACK(ehx); BT_PACK(ehy); BT_PACK(ealive); BT_PACK(ekind); BT_PACK(edying);
#undef BT_PACK
    }
}
static void bt_gpu_layout_pack(Env* envs, int start, int count, cudaStream_t stream = 0) {
    if (!bt_cuda_layout_host.enabled || count <= 0) return;
    bt_gpu_layout_pack_kernel<<<count, 128, 0, stream>>>(envs, start, count);
    bt_gpu_check(cudaGetLastError(), "layout snapshot pack");
    bt_gpu_check(cudaStreamSynchronize(stream), "layout snapshot sync");
}
static void bt_gpu_layout_close() {
    cudaFree(bt_cuda_layout_host.data);
    cudaFree(bt_cuda_layout_host.counts);
    bt_cuda_layout_host = {};
    bt_gpu_check(cudaMemcpyToSymbol(bt_cuda_layout, &bt_cuda_layout_host,
        sizeof(bt_cuda_layout_host)), "layout unbind");
}
#else
static void bt_gpu_layout_create(Env*, int) {}
static void bt_gpu_layout_profile_reset() {}
static void bt_gpu_layout_profile_report() {}
static void bt_gpu_layout_pack(Env*, int, int, cudaStream_t = 0) {}
static void bt_gpu_layout_close() {}
#endif
