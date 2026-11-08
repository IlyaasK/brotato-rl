// CUDA storage-only regression (the full simulation gate is test_brotato_cuda).
// nvcc -O2 -std=c++17 -arch=sm_120 --fmad=false -Xcompiler=-ffp-contract=off \
//   -Xcompiler=-Wno-narrowing --diag-suppress=2361 \
//   tests/test_brotato_cuda_layout.cu -o /tmp/test_brotato_cuda_layout
// /tmp/test_brotato_cuda_layout
#include <cuda_runtime.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#define BT_CUDA_INTERLEAVE_ENEMIES 1
#define BT_CUDA 1
#include "../ocean/brotato/brotato_state.h"
// The mapper is deliberately checked with an independent enclosing stride.
// Production's actual Env ABI is covered by test_brotato_cuda.cu.
struct Env { BtState state; unsigned char tail[24]; };
static void bt_gpu_check(cudaError_t e, const char* what) {
    if (e != cudaSuccess) {
        fprintf(stderr, "%s: %s\n", what, cudaGetErrorString(e));
        exit(1);
    }
}
#include "../ocean/brotato/brotato_cuda_layout.cuh"
__global__ static void write_fields(Env* envs, int n) {
    int e = blockIdx.x * blockDim.x + threadIdx.x;
    if (e >= n) return;
    BtState* s = &envs[e].state;
    for (int i = 0; i < BT_MAX; i++) {
        float f = (float)(e * BT_MAX + i);
        s->ex[i] = f; s->ey[i] = f + 1; s->ehp[i] = f + 2;
        s->emax[i] = f + 3; s->ehx[i] = f + 4; s->ehy[i] = f + 5;
        s->ealive[i] = (unsigned char)(e + i);
        s->ekind[i] = (unsigned char)(e + i + 1);
        s->edying[i] = (unsigned char)(e + i + 2);
    }
}
int main() {
    int cases = 0;
    for (int n : {1, 31, 32, 33, 65}) for (int mask : {0, 1, 3, 64, 256, 511}) {
        char text[16]; snprintf(text, sizeof(text), "%d", mask);
        setenv("BT_CUDA_INTERLEAVE", "1", 1);
        setenv("BT_CUDA_INTERLEAVE_MASK", text, 1);
        setenv("BT_CUDA_LAYOUT_PROFILE", "1", 1);
        Env* d = nullptr;
        bt_gpu_check(cudaMalloc(&d, (size_t)n * sizeof(Env)), "allocate");
        bt_gpu_check(cudaMemset(d, 0, (size_t)n * sizeof(Env)), "zero");
        bt_gpu_layout_create(d, n);
        write_fields<<<(n + 31) / 32, 32>>>(d, n);
        bt_gpu_check(cudaGetLastError(), "write launch");
        bt_gpu_check(cudaDeviceSynchronize(), "write sync");
        if (mask) {
            std::vector<unsigned char> raw((size_t)n * BT_MAX * 27);
            bt_gpu_check(cudaMemcpy(raw.data(), bt_cuda_layout_host.data, raw.size(),
                cudaMemcpyDeviceToHost), "interleaved image");
            for (int f = 0; f < BT_HOT_COUNT; f++)
                for (int i = 0; i < BT_MAX; i++) for (int e = 0; e < n; e++) {
                    int selected = mask & (1 << f);
                    if (f < 6) {
                        float actual;
                        size_t offset = ((size_t)f * BT_MAX * n + (size_t)i * n + e) * sizeof(float);
                        memcpy(&actual, raw.data() + offset, sizeof(actual));
                        float expected = selected ? (float)(e * BT_MAX + i + f) : 0;
                        if (actual != expected) {
                            fprintf(stderr, "word-interleave mismatch n=%d mask=%d field=%d env=%d slot=%d\n",
                                n, mask, f, e, i);
                            return 1;
                        }
                    } else {
                        size_t offset = (size_t)(24 + f - 6) * BT_MAX * n + (size_t)i * n + e;
                        unsigned char expected = selected ? (unsigned char)(e + i + f - 6) : 0;
                        if (raw[offset] != expected) {
                            fprintf(stderr, "byte-interleave mismatch n=%d mask=%d field=%d env=%d slot=%d\n",
                                n, mask, f, e, i);
                            return 1;
                        }
                    }
                }
        }
        // Exercise a nonzero-start subset before the full snapshot.
        bt_gpu_layout_pack(d, n - 1, 1);
        bt_gpu_layout_pack(d, 0, n);
        std::vector<Env> host(n);
        bt_gpu_check(cudaMemcpy(host.data(), d, (size_t)n * sizeof(Env), cudaMemcpyDeviceToHost), "snapshot");
        for (int e = 0; e < n; e++) for (int i = 0; i < BT_MAX; i++) {
            const BtState& s = host[e].state;
            float f = (float)(e * BT_MAX + i);
            if (s.ex[i] != f || s.ey[i] != f + 1 || s.ehp[i] != f + 2 ||
                s.emax[i] != f + 3 || s.ehx[i] != f + 4 || s.ehy[i] != f + 5 ||
                s.ealive[i] != (unsigned char)(e + i) ||
                s.ekind[i] != (unsigned char)(e + i + 1) ||
                s.edying[i] != (unsigned char)(e + i + 2)) {
                fprintf(stderr, "snapshot mismatch n=%d mask=%d env=%d slot=%d\n", n, mask, e, i);
                return 1;
            }
        }
        // Packing must neither race nor contribute to the real-kernel counts.
        std::vector<unsigned long long> counts((size_t)n * BT_HOT_COUNT);
        bt_gpu_check(cudaMemcpy(counts.data(), bt_cuda_layout_host.counts,
            counts.size() * sizeof(counts[0]), cudaMemcpyDeviceToHost), "counters");
        for (auto count : counts) if (count != BT_MAX) {
            fprintf(stderr, "counter mismatch n=%d mask=%d got=%llu\n", n, mask, count);
            return 1;
        }
        bt_gpu_layout_close();
        bt_gpu_check(cudaFree(d), "free");
        cases++;
    }
    puts("PASS: 30 layout cases, partial warps, field masks, canonical snapshots and access counters");
    return cases == 30 ? 0 : 1;
}
