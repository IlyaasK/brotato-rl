/* Source boundaries: SoundManager2D rejects submissions without a free player;
 * SoundManagerParent starts only one queued sound per frame. Lost/surplus pitch
 * draws change subsequent gameplay spawns, so rejection must preserve the RNG.
 */
#include "../ocean/brotato/brotato_birth_audio.h"
#include <cstdio>
static BtState state;

/* This fixture hand-builds a BtState (it never runs bt_core_init), so it must
 * state the audio-RNG model it asserts: 1 = the source's sound draws exist,
 * which is bt_core_init's default (see BtState.audio_rng). */
static void audio_rng_default(BtState *s) { s->audio_rng = 1; }

int main() {
    audio_rng_default(&state);
    bt_rng_seed(&state.rng, 1);
    bt_birth_audio_reset(&state);
    for (int i = 0; i < 12; i++) state.birth_audio_busy[i] = 1.0f;
    const bt_u64 before = state.rng.state;
    bt_birth_audio_enqueue(&state, 0);
    for (int i = 0; i < 70; i++) bt_birth_audio_step(&state);
    if (state.birth_audio_count || state.rng.state != before) {
        std::puts("FAIL: rejected birth sounds must never consume a later pitch draw");
        return 1;
    }
    for (int i = 0; i < 20; i++) bt_birth_audio_enqueue(&state, i % 2);
    if (state.birth_audio_count != 16 || state.rng.state != before) {
        std::puts("FAIL: bounded FIFO admission changes neither playback nor RNG");
        return 1;
    }
    bt_birth_audio_step(&state);
    int active = 0;
    for (int i = 0; i < 12; i++) active += state.birth_audio_busy[i] > 0.0f;
    if (active != 1 || state.birth_audio_count != 15 || state.rng.state == before) {
        std::puts("FAIL: a frame starts exactly one admitted sound");
        return 1;
    }
    for (int i = 0; i < 150; i++) bt_birth_audio_step(&state);
    for (int i = 0; i < 12; i++) if (state.birth_audio_busy[i] > 0.0f) return 1;
    const bt_u64 drained = state.rng.state;
    for (int i = 0; i < 10; i++) bt_birth_audio_step(&state);
    if (state.birth_audio_count || state.rng.state != drained) {
        std::puts("FAIL: drained playback must not keep drawing gameplay randomness");
        return 1;
    }
    /* Forced player-hit audio replaces a pending clip even while every channel
     * is busy. It must not interrupt a playing clip or draw pitch early. */
    bt_birth_audio_reset(&state);
    for (int i = 0; i < 12; i++) state.birth_audio_busy[i] = 1.0f;
    const bt_u64 forced_before = state.rng.state;
    bt_positional_audio_enqueue_force(&state, 2.0f, 0);
    bt_positional_audio_enqueue_force(&state, 0.01f, 0);
    bt_birth_audio_step(&state);
    if (state.birth_audio_count != 1 || state.rng.state != forced_before ||
        state.birth_audio_busy[0] < 0.9f) {
        std::puts("FAIL: forced audio waits for a channel without interrupting playback");
        return 1;
    }
    for (int i = 0; i < 12; i++) state.birth_audio_busy[i] = 0.0f;
    bt_birth_audio_step(&state);
    const bt_u64 forced_played = state.rng.state;
    bt_birth_audio_step(&state);
    if (state.birth_audio_count || state.birth_audio_busy[0] != 0.0f ||
        state.rng.state != forced_played || forced_played == forced_before) {
        std::puts("FAIL: forced replacement plays the latest short clip and draws pitch only at playback");
        return 1;
    }
    const bt_u64 before_heal = state.rng.state;
    bt_birth_audio_enqueue(&state, BT_POSITIONAL_AUDIO_HEAL);
    bt_birth_audio_step(&state);
    const bt_u64 heal_started = state.rng.state;
    for (int i = 0; i < 8; i++) bt_birth_audio_step(&state);
    for (int i = 0; i < 12; i++) if (state.birth_audio_busy[i] > 0.0f) {
        std::puts("FAIL: healer's 4707-frame clip must release its channel within eight ticks");
        return 1;
    }
    if (heal_started == before_heal || state.rng.state != heal_started) {
        std::puts("FAIL: a healer entry draws pitch once, not once per playback tick");
        return 1;
    }
    std::puts("PASS birth audio: rejected, queued, active and drained RNG boundaries");
}
