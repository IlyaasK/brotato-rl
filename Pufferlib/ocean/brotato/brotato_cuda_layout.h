/* Optional CUDA-only enemy-array layout. CPU/Metal retain ordinary C arrays.
 * The proxy has exactly the array's size/alignment and remains trivially
 * copyable. Its host view is the canonical snapshot; device subscripts resolve
 * to word-interleaved storage owned by brotato.cu. No arithmetic is changed.
 * Raw byte copies on the CUDA path must use the wrapper's snapshot helpers. */
#pragma once
#ifndef BT_CUDA_INTERLEAVE_ENEMIES
#define BT_CUDA_INTERLEAVE_ENEMIES 0
#endif
#if BT_CUDA_INTERLEAVE_ENEMIES && defined(__CUDACC__)
#define BT_CUDA_HOT_ARRAYS 1
enum { BT_HOT_EX, BT_HOT_EY, BT_HOT_EHP, BT_HOT_EMAX, BT_HOT_EHX,
       BT_HOT_EHY, BT_HOT_EALIVE, BT_HOT_EKIND, BT_HOT_EDYING, BT_HOT_COUNT };
static __device__ __forceinline__ void* bt_cuda_hot_address(
    const void* array, int field, int index, int width, bool count_access = true);
template<class T, int N, int Field> struct BtCudaHotArray {
    T raw[N];
    __host__ __device__ __forceinline__ T& operator[](int index) {
#if defined(__CUDA_ARCH__)
        return *(T*)bt_cuda_hot_address(this, Field, index, sizeof(T));
#else
        return raw[index];
#endif
    }
    __host__ __device__ __forceinline__ const T& operator[](int index) const {
#if defined(__CUDA_ARCH__)
        return *(const T*)bt_cuda_hot_address(this, Field, index, sizeof(T));
#else
        return raw[index];
#endif
    }
};
/* Preserve the shared core's byte-array clearing operation without decaying
 * an interleaved proxy into a misleading contiguous pointer. */
template<int N, int Field>
static __host__ __device__ void bt_zero_u8(BtCudaHotArray<unsigned char, N, Field>& p, int n) {
    for (int i = 0; i < n; i++) p[i] = 0;
}
#define BT_HOT_TYPE(T, alias, count, field) typedef BtCudaHotArray<T, count, field> alias
#else
#define BT_HOT_TYPE(T, alias, count, field) typedef T alias[count]
#endif
