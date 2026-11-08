/* native_forward_probe.cu - run the NATIVE CUDA trainer forward pass on a
 * trainer checkpoint, so an exported artifact can be checked against the exact
 * code that trains it instead of only against a Python re-derivation.
 *
 * This is not a reimplementation: it includes src/pufferl.cu (the native
 * trainer) without PUFFERLIB_BUILD_MAIN, builds the real Arch through the
 * trainer's own build_arch/weights_create/arch_reg_rollout, loads the
 * checkpoint with the trainer's own puf_load_weights_into, and steps
 * arch_forward (src/algo.cu:929-936: encoder_forward -> puf_mm -> mingru_forward
 * -> decoder_forward). The recurrent state is carried exactly as the rollout
 * does (mingru_forward copies next_state back, src/algo.cu:831-847).
 *
 * Build (mirrors build.sh's native mode for brotato; PRECISION_FLOAT matches
 * the fp32 checkpoint the exporter reads):
 *
 *   nvcc -O2 --threads 0 -arch=native -std=c++17 \
 *     -I. -Isrc -Iocean/brotato -Ivendor -Iraylib-5.5_linux_amd64/include \
 *     -I"$CUDA_HOME/include" -I"$CUDA_HOME/include/cccl" \
 *     -Xcompiler=-DPLATFORM_DESKTOP -Xcompiler=-fopenmp \
 *     -Xcompiler=-Wno-narrowing -Xcompiler=-Wno-unreachable-code \
 *     --diag-suppress=2361 --diag-suppress=111 --diag-suppress=128 \
 *     --fmad=false -Xcompiler=-ffp-contract=off \
 *     ocean/brotato/policy/native_forward_probe.cu \
 *     raylib-5.5_linux_amd64/lib/libraylib.a \
 *     -L"$CUDA_HOME/lib64" -lcudart -lnccl -lnvidia-ml -lcublas -lcusolver \
 *     -lcurand -lm -Xlinker=-lpthread -lomp5 -lGL -o /tmp/native_forward_probe
 *
 *   /tmp/native_forward_probe CKPT.bin OBS.f32 OUT.f32 SEQ OBS HID LAYERS ACTIONS
 *
 *   CKPT.bin  trainer checkpoint (puf_save_weights dump, float32)
 *   OBS.f32   SEQ * OBS float32 observations, row major
 *   OUT.f32   SEQ * (ACTIONS + 1) float32 written: logits..., value
 *
 * Exit code 0 = the forward ran; non-zero is a failure (bad args, checkpoint
 * float count that is not exactly this layout, or device/IO trouble).
 */

#define ENV_HEADER "ocean/brotato/brotato.h"
#define ENV_NAME brotato
#define PUFFER_ENV_NAME "brotato"

#include "../../src/pufferl.cu"

static void read_exact(const char* path, void* dst, size_t bytes) {
    FILE* file = fopen(path, "rb");
    if (!file) {
        perror(path);
        exit(3);
    }
    if (fread(dst, 1, bytes, file) != bytes || fclose(file)) {
        fprintf(stderr, "short read from %s (%zu bytes)\n", path, bytes);
        exit(3);
    }
}

int main(int argc, char** argv) {
    if (argc != 9) {
        fprintf(stderr, "usage: %s CKPT OBS OUT SEQ OBS_DIM HIDDEN LAYERS "
                        "ACTIONS\n", argv[0]);
        return 2;
    }
    const char* ckpt_path = argv[1];
    const char* obs_path = argv[2];
    const char* out_path = argv[3];
    int seq = atoi(argv[4]);
    int obs_dim = atoi(argv[5]);
    int hidden = atoi(argv[6]);
    int layers = atoi(argv[7]);
    int actions = atoi(argv[8]);
    if (seq <= 0 || obs_dim <= 0 || hidden <= 0 || layers <= 0 || actions <= 0) {
        fprintf(stderr, "all dimensions must be positive\n");
        return 2;
    }

    // The trainer's parameter count for this Arch, in registration order
    // (weights_create, src/algo.cu:982-990), with the allocator's 16-byte
    // alignment (src/pufferl.cu:234-265) and puf_save_weights' length
    // (src/pufferl.cu:1722-1736).
    long expect = 0;
    long sizes[3] = {(long)hidden * obs_dim, (long)(actions + 1) * hidden,
                     3L * hidden * hidden};
    int counts[3] = {1, 1, layers};
    for (int i = 0; i < 3; i++) {
        for (int c = 0; c < counts[i]; c++) {
            expect = (expect + 15) & ~15L;   // 16-byte entries
            expect += sizes[i] * (long)sizeof(float);
        }
    }
    expect /= (long)sizeof(float);
    struct stat st;
    if (stat(ckpt_path, &st) != 0) {
        perror(ckpt_path);
        return 3;
    }
    if (st.st_size != expect * (long)sizeof(float)) {
        fprintf(stderr, "checkpoint is %ld float32, this layout dumps %ld\n",
                (long)(st.st_size / (long)sizeof(float)), expect);
        return 4;
    }

    cudaSetDevice(0);
    cublas_init_handle();

    Arch arch = build_arch(obs_dim, hidden, layers, actions, false, 1);
    Allocator params = {0};
    Allocator acts = {0};
    Weights w = weights_create(&arch, &params);
    Activations av = arch_reg_rollout(&arch, w, &acts, 1);
    Prec obs = {.shape = {1, obs_dim}};
    Prec state = {.shape = {layers, 1, hidden}};
    alloc_register(&acts, &obs);
    alloc_register(&acts, &state);
    alloc_create(&params);
    alloc_create(&acts);

    Float mw = {.data = (float*)params.mem, .shape = {params.total_elems}};
    Prec pparam = {.data = (precision_t*)params.mem,
                   .shape = {params.total_elems}};
    puf_load_weights_into(mw, pparam, 0, ckpt_path);
    cudaDeviceSynchronize();
    fprintf(stderr, "loaded %ld float32 into %ld params\n",
            (long)params.total_elems, (long)params.total_elems);

    float* host_obs = (float*)malloc((size_t)seq * obs_dim * sizeof(float));
    float* host_out = (float*)malloc((size_t)seq * (actions + 1) * sizeof(float));
    if (!host_obs || !host_out) {
        fprintf(stderr, "out of memory for IO buffers\n");
        return 3;
    }
    read_exact(obs_path, host_obs, (size_t)seq * obs_dim * sizeof(float));

    for (int t = 0; t < seq; t++) {
        cudaMemcpy(obs.data, host_obs + (size_t)t * obs_dim,
                   (size_t)obs_dim * sizeof(float), cudaMemcpyHostToDevice);
        Prec dec = arch_forward(&arch, w, av, obs, state, 0);
        cudaMemcpy(host_out + (size_t)t * (actions + 1), dec.data,
                   (size_t)(actions + 1) * sizeof(float),
                   cudaMemcpyDeviceToHost);
        cudaStreamSynchronize(0);
    }

    FILE* file = fopen(out_path, "wb");
    if (!file) {
        perror(out_path);
        return 3;
    }
    size_t n = (size_t)seq * (actions + 1);
    if (fwrite(host_out, sizeof(float), n, file) != n || fclose(file)) {
        fprintf(stderr, "failed to write %s\n", out_path);
        return 3;
    }
    fprintf(stderr, "wrote %zu float32\n", n);
    return 0;
}
