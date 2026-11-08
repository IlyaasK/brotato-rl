/* native_forward_probe.c - run the NATIVE PufferNet forward pass on a
 * trainer checkpoint, so an exported artifact can be checked against the
 * trainer's own arithmetic instead of only against a Python re-derivation.
 *
 * Nothing here reimplements the network. The probe includes src/puffercpu.c,
 * the native CPU inference implementation ("PufferNet: default policy matching
 * the native backend Arch in algo.cu", src/puffercpu.c:533-547; used by
 * ./build.sh brotato --cpu), and calls its layers exactly the way
 * forward_puffernet does (src/puffercpu.c:584-597):
 *
 *   linear(encoder) -> mingru(layers) -> linear(decoder)
 *
 * with the weight stream consumed in the trainer's own registration order
 * (encoder.weight, decoder.weight, mingru.0.weight .. mingru.L-1.weight;
 * weights_create, src/algo.cu:982-990). Unlike forward_puffernet the probe
 * reports the raw decoder row - the first `actions` columns are the logits and
 * the last column is the value (src/pufferl.cu:702, src/algo.cu:564-573) -
 * rather than sampling an action from it, so the numbers are comparable.
 *
 * The recurrent state starts zeroed (episode start); the probe never applies
 * mingru_zero_term and never samples, so the output is deterministic.
 *
 * Build and run:
 *     cc -O2 -o /tmp/native_forward_probe \
 *         ocean/brotato/policy/native_forward_probe.c -lm
 *     /tmp/native_forward_probe CKPT.bin OBS.f32 OUT.f32 SEQ OBS HID LAYERS \
 *         ACTIONS [MASK.u8 ARGMAX.f32]
 *
 *   CKPT.bin   trainer checkpoint (puf_save_weights dump, float32)
 *   OBS.f32    SEQ * OBS float32 observations, row major
 *   OUT.f32    SEQ * (ACTIONS + 1) float32 written: logits..., value
 *   MASK.u8    optional SEQ * ACTIONS bytes, 1 = action legal (the env's
 *              legality vector); a row with no legal action is an error
 *   ARGMAX.f32 optional SEQ float32: the masked argmax this native code picks
 *              (multidiscrete hard = 1, src/puffercpu.c:199-230), i.e. the
 *              action the in-game forward must also pick
 *
 * Exit code 0 = forward ran and the checkpoint was consumed exactly in full
 * (get_weights' index ended at the file's float count, i.e. the file really is
 * the trainer's dump for this layout). Any other code is a failure.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../src/puffercpu.c"

static void* read_all(const char* path, size_t want, size_t elem) {
    FILE* file = fopen(path, "rb");
    if (!file) {
        perror(path);
        exit(3);
    }
    void* buf = malloc(want * elem);
    if (!buf) {
        fprintf(stderr, "out of memory reading %s\n", path);
        exit(3);
    }
    if (fread(buf, elem, want, file) != want || fclose(file)) {
        fprintf(stderr, "short read from %s (want %zu elements)\n", path, want);
        exit(3);
    }
    return buf;
}

int main(int argc, char** argv) {
    if (argc != 9 && argc != 11) {
        fprintf(stderr, "usage: %s CKPT OBS OUT SEQ OBS_DIM HIDDEN LAYERS "
                        "ACTIONS [MASK.u8 ARGMAX.f32]\n", argv[0]);
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
    /* Optional action-legality mask: seq * actions bytes (1 = legal), one row
     * per step, plus the masked argmax the native multidiscrete layer picks. */
    const char* mask_path = argc == 11 ? argv[9] : NULL;
    const char* argmax_path = argc == 11 ? argv[10] : NULL;
    if (seq <= 0 || obs_dim <= 0 || hidden <= 0 || layers <= 0 || actions <= 0) {
        fprintf(stderr, "all dimensions must be positive\n");
        return 2;
    }

    /* The checkpoint's real float count, read independently of the loader's
     * own bookkeeping, so this check does not depend on load_weights' internals
     * (an older variant reserved padding floats past the file's end). */
    long file_floats = 0;
    {
        FILE* handle = fopen(ckpt_path, "rb");
        if (!handle) {
            perror(ckpt_path);
            return 3;
        }
        if (fseek(handle, 0, SEEK_END)) {
            perror(ckpt_path);
            return 3;
        }
        long bytes = ftell(handle);
        fclose(handle);
        if (bytes < 0 || bytes % (long)sizeof(float) != 0) {
            fprintf(stderr, "checkpoint size %ld is not a multiple of 4\n", bytes);
            return 4;
        }
        file_floats = bytes / (long)sizeof(float);
    }

    /* Trainer weight order: encoder, decoder, mingru[0..L-1]. */
    Weights* weights = load_weights(ckpt_path);
    Linear* encoder = make_linear(weights, 1, obs_dim, hidden);
    Linear* decoder = make_linear(weights, 1, hidden, actions + 1);
    MinGRU* gru = make_mingru(weights, 1, hidden, layers);

    long consumed = (long)weights->idx;
    fprintf(stderr, "checkpoint floats: %ld consumed, %ld in file\n",
            consumed, file_floats);
    if (consumed != file_floats) {
        fprintf(stderr, "checkpoint does not match this layout: %ld of %ld "
                        "float32 consumed\n", consumed, file_floats);
        return 4;
    }

    float* obs = (float*)read_all(obs_path, (size_t)seq * obs_dim, sizeof(float));
    float* out = (float*)calloc((size_t)seq * (actions + 1), sizeof(float));
    if (!out) {
        fprintf(stderr, "out of memory for outputs\n");
        return 3;
    }

    /* Masked argmax: the native multidiscrete layer (hard = 1) is the trainer's
     * own action pick (src/puffercpu.c:199-230) and applies the same rule the
     * CUDA rollout follows for masked-off actions. */
    unsigned char* masks = NULL;
    float* argmax = NULL;
    int logit_sizes[1] = {0};
    Multidiscrete* arger = NULL;
    if (mask_path) {
        logit_sizes[0] = actions;
        masks = (unsigned char*)read_all(mask_path, (size_t)seq * actions, 1);
        argmax = (float*)calloc((size_t)seq, sizeof(float));
        if (!argmax) {
            fprintf(stderr, "out of memory for the argmax output\n");
            return 3;
        }
        arger = make_multidiscrete(1, logit_sizes, 1);
        for (int t = 0; t < seq; t++) {
            int legal = 0;
            for (int a = 0; a < actions; a++) {
                legal += masks[(size_t)t * actions + a] != 0;
            }
            if (legal == 0) {
                fprintf(stderr, "mask at step %d leaves no legal action\n", t);
                return 5;
            }
        }
    }

    for (int t = 0; t < seq; t++) {
        float* row = obs + (size_t)t * obs_dim;
        linear(encoder, row);
        mingru(gru, encoder->output);
        linear(decoder, gru->output);
        memcpy(out + (size_t)t * (actions + 1), decoder->output,
               (size_t)(actions + 1) * sizeof(float));
        if (arger) {
            multidiscrete(arger, decoder->output, argmax + t, 1,
                          masks + (size_t)t * actions);
        }
    }

    FILE* file = fopen(out_path, "wb");
    if (!file) {
        perror(out_path);
        return 3;
    }
    size_t n = (size_t)seq * (actions + 1);
    if (fwrite(out, sizeof(float), n, file) != n || fclose(file)) {
        fprintf(stderr, "failed to write %s\n", out_path);
        return 3;
    }
    if (arger) {
        FILE* afile = fopen(argmax_path, "wb");
        if (!afile) {
            perror(argmax_path);
            return 3;
        }
        if (fwrite(argmax, sizeof(float), (size_t)seq, afile) != (size_t)seq
                || fclose(afile)) {
            fprintf(stderr, "failed to write %s\n", argmax_path);
            return 3;
        }
        fprintf(stderr, "wrote %d masked argmax\n", seq);
    }
    free(obs);
    free(out);
    return 0;
}
