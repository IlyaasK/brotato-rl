/* brotato_weapon_audio.h -- the gameplay-RNG half of weapon audio.
 *
 * Owner: WeaponAudioRuntime.  Rendering is omitted, but the SOURCE random
 * samples it takes are not: a weapon shot and a lute hit each draw the global
 * RNG, and losing/doubling those draws desynchronises later spawns.
 *
 * SOURCE, unless stated, is /Users/leelachebiyyam/brotato_recovered:
 *   weapons/shooting_behaviors/ranged_weapon_shooting_behavior.gd:10
 *     SoundManager.play(Utils.get_rand_element(current_stats.shooting_sounds),
 *                       current_stats.sound_db_mod, 0.2)
 *   weapons/shooting_behaviors/melee_weapon_shooting_behavior.gd:63,105
 *     the same call, AFTER the recoil tween's tween_all_completed and just
 *     before _parent.enable_hitbox() (thrust :105, sweep :63)
 *   weapons/weapon.gd:432-433
 *     if custom_hit_sounds.size() > 0:
 *         SoundManager2D.play(Utils.get_rand_element(custom_hit_sounds),
 *                             thing_hit.global_position, -2, 0.1)
 *   singletons/utils.gd:391-394        get_rand_element -> Array.pick_random
 *   singletons/sound_manager.gd:7-14   main SoundManager admission
 *   singletons/sound_manager_2d.gd:7-14 SoundManager2D admission
 *   singletons/sound_manager_parent.gd:6-8,39-48
 *     _process: ONE queued sound per frame while a player is free, and the
 *     pitch draw `1.0 + rand_range(-pitch_rand, pitch_rand)` happens AT PLAYBACK,
 *     not at submission.  `finished` frees a player after
 *     stream_length / pitch_scale seconds.
 *   project.godot:2635-2636            SoundManager autoload before SoundManager2D
 *
 * ONE draw at the source event, then playback.  Utils.get_rand_element is
 * Array.pick_random == `array[Math::rand() % size]` (bt_rng_bounded), drawn even
 * for a size-1 array and skipped entirely for an empty one.  The pitch draw is
 * one global rand_range; it is NOT duplicated here at enqueue, and no fixed
 * padding is added.
 *
 * NULL STREAM: ranged/melee shoot() calls SoundManager.play UNCONDITIONALLY, so
 * an empty shooting_sounds array would pass get_rand_element's null to play().
 * No shipped weapon row has an empty shooting sounds array (counts run 1..15;
 * exactly 16 tier variants carry a single clip) and weapon.gd:432 guards
 * custom_hit_sounds on size > 0, so this path never reaches play(null); the
 * count == 0 early return below is therefore unreachable for shipped data.  A
 * size-1 array still takes its one Array.pick_random draw (bt_rng_bounded).
 *
 * bt_weapon_audio_shoot -> main SoundManager queue (12 players / FIFO 16,
 *   source admission "a player must be free AND the queue holds < MAX_SOUNDS",
 *   pitch_rand 0.2).  The queue itself lives in brotato_state.h as main_audio_*.
 *   The parent unifies the PlayerVisual footstep consumer (player.gd:601,
 *   SoundManager.play) onto this SAME main queue, so there is exactly ONE pool.
 *   bt_main_audio_enqueue_force is the always_play path: it admits with every
 *   player busy by replacing the OLDEST QUEUED entry (sound_manager.gd:8-14).
 * bt_weapon_audio_hit -> the shared SoundManager2D queue via the parent's
 *   bt_positional_audio_enqueue (12 players / FIFO 16, pitch_rand 0.1); only the
 *   Abyss DLC lute ships custom_hit_sounds.
 * ON-HIT CHILD (not in this file): weapon_service.gd:351 also plays one
 *   positional clip (pitch_rand 0.2) per spawned special projectile; the spawn
 *   owner submits it with bt_rng_bounded(count) + bt_positional_audio_enqueue(
 *   s, BT_WEAPON_AUDIO_DURATION[first + idx], BT_AUDIO_PITCH_RANGE_02).
 *
 * INCLUDES: state + the canonical tables it re-exports (BT_WEAPONS /
 * BT_WEAPON_AUDIO_DURATION) + the positional playground (brotato_birth_audio.h),
 * never brotato_core.h or the stat helpers, so standalone fixtures compile.
 *
 * POD + BT_HD only, no allocation -> CPU/CUDA/MSL portable.
 */
#pragma once

#include "brotato_state.h"
#include "brotato_birth_audio.h"

/* binary64 bits of the POSITIVE pitch-range magnitude a call site passes to
 * play().  The negative bound is this with the sign bit set, exactly as
 * sound_manager_parent.gd calls rand_range(-pitch_rand, pitch_rand). */
#define BT_AUDIO_PITCH_RANGE_02 ((bt_u64)0x3FC999999999999AULL)  /* 0.2 */
#define BT_AUDIO_PITCH_RANGE_01 ((bt_u64)0x3FB999999999999AULL)  /* 0.1 */

#define BT_MAIN_AUDIO_PLAYERS 12
#define BT_MAIN_AUDIO_MAX 16

/* SoundManagerParent.clear_queue() (main.gd:1078): empty the pending FIFO but
 * leave players mid-playback untouched. */
BT_HD static void bt_main_audio_clear_queue(BT_DEVICE BtState* s) {
    s->main_audio_head = s->main_audio_count = 0;
}

/* SoundManager._ready: 12 idle players, empty FIFO. */
BT_HD static void bt_main_audio_reset(BT_DEVICE BtState* s) {
    for (int i = 0; i < BT_MAIN_AUDIO_PLAYERS; i++) s->main_audio_busy[i] = 0.0f;
    bt_main_audio_clear_queue(s);
}

/* SoundManager.play(sound, volume_mod, pitch_rand, always_play): submit only
 * while a player is free and the FIFO holds fewer than MAX_SOUNDS.  With
 * always_play (source sound_manager.gd:8-14) the fallback pops the OLDEST QUEUED
 * entry -- a queued sound, never a playing channel -- and appends, so a forced
 * sound is admitted even with every player busy (it then waits in the FIFO for a
 * channel); an empty FIFO simply appends.  `duration` is the clip's length in
 * seconds and `pitch_range_bits` its positive pitch-range magnitude. */
BT_HD static int bt_main_audio_any_free(BT_DEVICE BtState* s) {
    int available = 0;
    for (int i = 0; i < BT_MAIN_AUDIO_PLAYERS; i++)
        available |= s->main_audio_busy[i] <= 0.0f;
    return available;
}

BT_HD static void bt_main_audio_queue(BT_DEVICE BtState* s, float duration,
                                      bt_u64 pitch_range_bits) {
    int at = ((int)s->main_audio_head + (int)s->main_audio_count) % BT_MAIN_AUDIO_MAX;
    s->main_audio_dur[at] = duration;
    s->main_audio_pitch[at] = pitch_range_bits;
    s->main_audio_count++;
}

BT_HD static void bt_main_audio_enqueue_impl(BT_DEVICE BtState* s, float duration,
                                             bt_u64 pitch_range_bits, int force) {
    /* audio_rng == 0: the bridge empties this queue before its own _process
     * can pop it, so a queued clip can never start and its playback-pitch draw
     * never happens.  The enqueue itself takes no draw, so skipping it is
     * RNG-equivalent to the bridge's per-frame clear. */
    if (!s->audio_rng) return;
    if (bt_main_audio_any_free(s) && s->main_audio_count < BT_MAIN_AUDIO_MAX) {
        bt_main_audio_queue(s, duration, pitch_range_bits);
        return;
    }
    if (!force) return;                      /* ordinary submission is dropped */
    if (s->main_audio_count > 0) {           /* Array.pop_front() of the oldest queued */
        s->main_audio_head = (s->main_audio_head + 1) % BT_MAIN_AUDIO_MAX;
        s->main_audio_count--;
    }
    bt_main_audio_queue(s, duration, pitch_range_bits);
}

/* always_play == false (weapon shots, footsteps): RNG-free drop when rejected. */
BT_HD static void bt_main_audio_enqueue(BT_DEVICE BtState* s, float duration,
                                        bt_u64 pitch_range_bits) {
    bt_main_audio_enqueue_impl(s, duration, pitch_range_bits, 0);
}

/* always_play == true: admit even with every player busy, replacing the oldest
 * queued entry (the event's own draw happened in the caller either way). */
BT_HD static void bt_main_audio_enqueue_force(BT_DEVICE BtState* s, float duration,
                                              bt_u64 pitch_range_bits) {
    bt_main_audio_enqueue_impl(s, duration, pitch_range_bits, 1);
}

/* SoundManagerParent._process + the players' `finished` signal, one 60 Hz frame:
 * finished players return first (busy timer expiring), then ONE queued sound
 * starts with ONE global rand_range pitch draw and plays for
 * clip_length / pitch_scale seconds. */
BT_HD static void bt_main_audio_step(BT_DEVICE BtState* s) {
    int available = -1;
    for (int i = 0; i < BT_MAIN_AUDIO_PLAYERS; i++) {
        s->main_audio_busy[i] = s->main_audio_busy[i] > BT_DT
                                    ? s->main_audio_busy[i] - BT_DT : 0.0f;
        if (available < 0 && s->main_audio_busy[i] <= 0.0f) available = i;
    }
    if (available < 0 || s->main_audio_count == 0) return;
    float duration = s->main_audio_dur[s->main_audio_head];
    bt_u64 hi = s->main_audio_pitch[s->main_audio_head];
    bt_u64 lo = hi ^ ((bt_u64)1 << 63);      /* negate the magnitude */
    s->main_audio_head = (s->main_audio_head + 1) % BT_MAIN_AUDIO_MAX;
    s->main_audio_count--;
    /* GDScript's literal pitch_rand is binary64, so the bounds are too.
     * audio_rng == 0: the bridge's neutralization means no playback pitch. */
    bt_u64 pitch_bits = s->audio_rng ? bt_rng_global_range_bits(&s->rng, lo, hi) : (bt_u64)0;
    float pitch = bt_rng_f64_to_float_bits(
        bt_rng_f64_add_bits((bt_u64)0x3FF0000000000000ULL, pitch_bits));
    s->main_audio_busy[available] = duration / pitch;
}

/* One weapon shot: Array.pick_random over the weapon's shooting_sounds (ONE
 * bounded global draw, skipped for an empty array) then SoundManager.play with
 * pitch_rand 0.2.  `wid` is the canonical BT_WEAPONS index; the clip length is
 * its generated BT_WEAPON_AUDIO_DURATION entry.
 *
 * The PICK is taken in BOTH audio models: it is a gameplay draw
 * (`Utils.get_rand_element(current_stats.shooting_sounds)`,
 * ranged_weapon_shooting_behavior.gd:10) that happens before `SoundManager.play`
 * is even called, and the bridge's neutralization never empties a weapon's sound
 * array -- it only clears the playback queues (mod_main.gd `_clear_sound_queues`).
 * `audio_rng` therefore gates the queue/pitch side only. */
BT_HD static void bt_weapon_audio_shoot(BT_DEVICE BtState* s, unsigned short wid) {
    if (wid >= BT_NWEAPONS) return;
    BT_CONST_PTR(BtWeaponDef) D = &BT_WEAPONS[wid];
    unsigned int n = (unsigned int)D->shoot_audio_count;
    if (n == 0u) return;                     /* get_rand_element(empty) -> null */
    unsigned int idx = bt_rng_bounded(&s->rng, n);
    float duration = BT_WEAPON_AUDIO_DURATION[(unsigned int)D->shoot_audio_first + idx];
    bt_main_audio_enqueue(s, duration, BT_AUDIO_PITCH_RANGE_02);
}

/* One weapon Hitbox hit: Array.pick_random over custom_hit_sounds (ONE bounded
 * global draw, skipped for an empty array) then SoundManager2D.play with
 * pitch_rand 0.1, queued through the shared positional pool.  Like the shoot
 * pick above, the pick is taken in both audio models; only the queue/pitch side
 * is gated by `audio_rng`. */
BT_HD static void bt_weapon_audio_hit(BT_DEVICE BtState* s, unsigned short wid) {
    if (wid >= BT_NWEAPONS) return;
    BT_CONST_PTR(BtWeaponDef) D = &BT_WEAPONS[wid];
    unsigned int n = (unsigned int)D->hit_audio_count;
    if (n == 0u) return;                     /* get_rand_element(empty) -> null */
    unsigned int idx = bt_rng_bounded(&s->rng, n);
    float duration = BT_WEAPON_AUDIO_DURATION[(unsigned int)D->hit_audio_first + idx];
    bt_positional_audio_enqueue(s, duration, BT_AUDIO_PITCH_RANGE_01);
}
