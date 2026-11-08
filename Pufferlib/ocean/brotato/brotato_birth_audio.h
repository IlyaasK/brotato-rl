/* Birth sounds affect the gameplay RNG even in a non-rendering simulation.
 * SOURCE: entity_birth.gd:47-50,99-106; healer.gd:24-26; sound_manager_2d.gd:7-14;
 * sound_manager_parent.gd:6-8,39-48. Admission requires an available player;
 * one queued sound starts per idle frame, with one global rand_range draw.
 * WAVs: entities/birth/birth_begin_sound.wav = 26948/48000 seconds;
 * birth_end_sound.wav = 17280/48000 seconds.
 * Healer's sci-fi_code_fail_08.wav = 4707/48000 seconds, on the same queue.
 * Playback lifetimes advance at fixed 60 Hz, not an audio-device clock.
 */
#pragma once
#include "brotato_state.h"

#define BT_POSITIONAL_AUDIO_HEAL 2

BT_HD static void bt_birth_audio_reset(BT_DEVICE BtState* s) {
    for (int i = 0; i < 12; i++) s->birth_audio_busy[i] = 0.0f;
    s->birth_audio_head = s->birth_audio_count = 0;
}

BT_HD static void bt_positional_audio_enqueue_impl(BT_DEVICE BtState* s, float duration,
                                                  bt_u64 pitch_range_bits, int force) {
    /* audio_rng == 0: the bridge clears this queue before its own _process can
     * pop it, so a queued entry can never start.  entity_birth's / healer's
     * sounds are literal Resources (no `Utils.get_rand_element` pick), so this
     * path carries no draw to preserve; the enqueue is pure bookkeeping. */
    if (!s->audio_rng) return;
    int available = 0;
    for (int i = 0; i < 12; i++) available |= s->birth_audio_busy[i] <= 0.0f;
    if (!available || s->birth_audio_count >= 16) {
        if (!force) return;
        /* always_play replaces the oldest queued sound, not a playing channel.
         * Array.pop_front on an empty source queue still permits the append. */
        if (s->birth_audio_count) {
            s->birth_audio_head = (s->birth_audio_head + 1) % 16;
            s->birth_audio_count--;
        }
    }
    int at = ((int)s->birth_audio_head + (int)s->birth_audio_count) % 16;
    s->birth_audio_dur[at] = duration;
    s->birth_audio_pitch[at] = pitch_range_bits;
    s->birth_audio_count++;
}

BT_HD static void bt_positional_audio_enqueue(BT_DEVICE BtState* s, float duration,
                                             bt_u64 pitch_range_bits) {
    bt_positional_audio_enqueue_impl(s, duration, pitch_range_bits, 0);
}
BT_HD static void bt_positional_audio_enqueue_force(BT_DEVICE BtState* s, float duration,
                                                   bt_u64 pitch_range_bits) {
    bt_positional_audio_enqueue_impl(s, duration, pitch_range_bits, 1);
}

BT_HD static void bt_birth_audio_enqueue(BT_DEVICE BtState* s, int sound) {
    float duration = sound == BT_POSITIONAL_AUDIO_HEAL ? 4707.0f / 48000.0f :
        (sound == 1 ? 17280.0f / 48000.0f : 26948.0f / 48000.0f);
    bt_positional_audio_enqueue(s, duration, (bt_u64)0x3FC999999999999AULL);
}

BT_HD static void bt_birth_audio_step(BT_DEVICE BtState* s) {
    int available = -1;
    for (int i = 0; i < 12; i++) {
        s->birth_audio_busy[i] = s->birth_audio_busy[i] > BT_DT ? s->birth_audio_busy[i] - BT_DT : 0.0f;
        if (available < 0 && s->birth_audio_busy[i] <= 0.0f) available = i;
    }
    if (available < 0 || s->birth_audio_count == 0) return;
    int head = (int)s->birth_audio_head;
    float duration = s->birth_audio_dur[head];
    bt_u64 magnitude = s->birth_audio_pitch[head];
    s->birth_audio_head = (s->birth_audio_head + 1) % 16;
    s->birth_audio_count--;
    /* GDScript's literal 0.2 is binary64, not a widened binary32.  With
     * audio_rng == 0 the bridge's neutralization means this playback pitch is
     * never drawn. */
    bt_u64 pitch_bits = s->audio_rng
        ? bt_rng_global_range_bits(&s->rng, magnitude ^ ((bt_u64)1 << 63), magnitude)
        : (bt_u64)0;
    float pitch = bt_rng_f64_to_float_bits(bt_rng_f64_add_bits((bt_u64)0x3FF0000000000000ULL, pitch_bits));
    s->birth_audio_busy[available] = duration / pitch;
}
