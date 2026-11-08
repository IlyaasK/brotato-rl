/* Gameplay-RNG consumers owned by the player's visual lifecycle.
 * SOURCE: player.gd:191-213 (_running_smoke.emit()/stop() only on the
 * idle<->move transition), 598-603 (play_step_sound -> SoundManager.play(
 * Utils.get_rand_element(step_sounds))), 606 (landing only from main.gd:250,
 * which runs exclusively when RunData.current_wave == 1); player.tscn landing
 * length 1.2; player_move.tres method keys 0.1/0.4, loop length 0.6;
 * running_smoke.tscn; pinned engine scene/2d/cpu_particles_2d.cpp
 * (_update_internal emission/inactive_time block, _particles_process
 * restart predicate + per-restart draw order).
 * Rendering is omitted, not its source random samples. Particle geometry and
 * seeded per-particle force updates have no observable gameplay consumer.
 */
#pragma once
#include "brotato_state.h"
#include "brotato_weapon_audio.h"

/* AnimationPlayer clock for player_move.tres / player_move_swim (length 0.6,
 * loop, method keys at 0.1 and 0.4). Pinned engine 0a9011743:
 *   animation_player.h  PlaybackData::pos is `float`;
 *   animation_player.cpp _animation_process_data():
 *     float delta = p_delta * speed_scale * cd.speed_scale;   (speed_scale = playback_speed)
 *     float next_pos = cd.pos + delta;                        (loop: fposmod, and an
 *                                                              exact `length` stays `length`)
 *   _animation_process_animation() -> Animation::method_track_get_key_indices():
 *     a key fires, once, when `p_time - p_delta <= key < p_time`.
 * The clock is therefore binary32 and the bounds are inclusive-exclusive, which
 * is what decides the frame each footstep draw lands on. `player_anim_time`
 * keeps those binary32 bits. */
BT_HD static float bt_player_anim_pos(BT_DEVICE BtState* s) {
    union { unsigned int u; float f; } v;
    v.u = (unsigned int)(s->player_anim_time & 0xFFFFFFFFULL);
    return v.f;
}

BT_HD static void bt_player_anim_store(BT_DEVICE BtState* s, float pos) {
    union { unsigned int u; float f; } v;
    v.f = pos;
    s->player_anim_time = (bt_u64)v.u;
}

BT_HD static int bt_player_anim_keys(float from, float to) {
    /* Animation::_method_track_get_key_indices_in_range; its end-of-animation
     * widening only reaches a key placed exactly at the animation length, and
     * neither move animation has one. */
    int n = 0;
    if (0.1f >= from && 0.1f < to) n++;
    if (0.4f >= from && 0.4f < to) n++;
    return n;
}

BT_HD static void bt_player_visual_reset(BT_DEVICE BtState* s) {
    s->player_fx_time = s->player_fx_inactive = 0.0f;
    s->player_fx_cycle = 0; s->player_fx_active = 0;
    /* A fresh Player scene rebuilds the CPUParticles2D node: set_amount()
     * memsets every Particle, so p.time and p.lifetime restart at 0. */
    for (int i = 0; i < 10; i++) s->player_fx_age[i] = 0.0f;
    s->player_fx_processing = 1; s->player_anim_mode = 0;
    bt_player_anim_store(s, 0.0f);   /* wave start replays the idle animation */
}

BT_HD static unsigned int bt_player_fx_hash(unsigned int x) {
    x = ((x >> 16) ^ x) * 0x45d9f3bu;
    x = ((x >> 16) ^ x) * 0x45d9f3bu;
    return (x >> 16) ^ x;
}

BT_HD static void bt_player_fx_step(BT_DEVICE BtState* s, int emitting) {
    int count = s->zone == 0 ? 6 : 10;
    float lifetime = s->zone == 0 ? 0.75f : 0.3f;
    if (emitting) { s->player_fx_processing = 1; s->player_fx_inactive = 0.0f; }
    if (!s->player_fx_processing) return;
    if (!emitting) {
        s->player_fx_inactive += BT_DT;
        if (s->player_fx_inactive > lifetime * 1.2f) {
            s->player_fx_processing = 0;
            s->player_fx_time = s->player_fx_inactive = 0.0f;
            s->player_fx_cycle = 0;
            return;
        }
    }
    float prev = s->player_fx_time;
    float time = prev + BT_DT;
    if (time > lifetime) { time -= lifetime; s->player_fx_cycle++; }
    s->player_fx_time = time;
    float system_phase = time / lifetime;
    for (int i = 0; i < count; i++) {
        unsigned short bit = (unsigned short)(1u << i);
        if (!emitting && !(s->player_fx_active & bit)) continue;
        float phase = (float)i / (float)count;
        unsigned int seed = s->player_fx_cycle;
        if (phase >= system_phase) seed--;
        seed = seed * (unsigned int)count + (unsigned int)i;
        float random = (float)(bt_player_fx_hash(seed) % 65536u) / 65536.0f;
        phase += 0.35f * random / (float)count;
        float restart_time = phase * lifetime;
        int restart = 0;
        if (time > prev) {
            if (restart_time >= prev && restart_time < time) {
                restart = 1;
            }
        } else if (restart_time >= prev) {
            restart = 1;
        } else if (restart_time < time) {
            restart = 1;
        }
        /* cpu_particles_2d.cpp: `if (p.time * (1.0 - explosiveness_ratio) >
         * p.lifetime) restart = true;` is evaluated for every particle that was
         * not skipped, including dormant ones. With explosiveness_ratio 0 and
         * lifetime_randomness 0 (both scene defaults) p.lifetime == lifetime,
         * so a dormant particle whose stale age already passed the lifetime
         * re-emits as soon as emission resumes. */
        if (s->player_fx_age[i] > lifetime) restart = 1;
        if (restart) {
            /* particles_rng == 0 mirrors the bridge's suppression: every
             * CPUParticles2D has emitting cleared, so the engine takes this
             * same branch (cpu_particles_2d.cpp:838-841 `if (!emitting)
             * { p.active = false; continue; }`) and draws nothing. */
            if (!emitting || !s->particles_rng) { s->player_fx_active &= (unsigned short)~bit; continue; }
            s->player_fx_active |= bit;
            (void)bt_rng_next(&s->rng);       /* Particle.seed */
            (void)bt_math_randf(&s->rng);     /* angle_rand */
            (void)bt_math_randf(&s->rng);     /* scale_rand */
            (void)bt_math_randf(&s->rng);     /* hue_rot_rand */
            (void)bt_math_randf(&s->rng);     /* anim_offset_rand */
            (void)bt_math_randf(&s->rng);     /* spread */
            (void)bt_math_randf(&s->rng);     /* initial velocity */
            (void)bt_math_randf(&s->rng);     /* lifetime randomness (zero still draws) */
            (void)bt_math_randf(&s->rng);     /* rectangle x / sphere s */
            (void)bt_math_randf(&s->rng);     /* rectangle y / sphere t */
            s->player_fx_age[i] = 0.0f;
        } else if (s->player_fx_active & bit) {
            /* Engine restart initializes age but does not call _particle_process
             * until the following frame. Its fractional motion delta is visual.
             * (The engine's `else if (p.time > p.lifetime)` arm is unreachable
             * here: it repeats the stale-age test already folded into restart.) */
            s->player_fx_age[i] += BT_DT;
        }
    }
}


/* Call after the physics/deferred birth boundary. Returned method-track events
 * are emitted AFTER SoundManager2D processes the physics frame's birth sounds:
 * AnimationPlayer method callbacks are deferred until the idle boundary. */
BT_HD static int bt_player_visual_step(BT_DEVICE BtState* s, int moving, float playback) {
    /* main.gd:246-251 calls player.land() only when RunData.current_wave == 1,
     * so only wave 1 plays the 1.2 s "landing" animation (player.gd:604-615).
     * While that animation is current, update_animation()'s idle/move arms never
     * run, so the smoke stays stopped and the anim clock starts at the landing's
     * end (frames 0..71). Later waves are rebuilt from the shop scene and start
     * on "idle" with the smoke ready to emit on the first moving frame. */
    if (s->wave == 1 && s->tick_in_wave <= 72) {
        if (s->tick_in_wave == 72) { s->player_anim_mode = 1; bt_player_anim_store(s, 0.0f); }
        bt_player_fx_step(s, 0);
        return 0;
    }
    /* update_animation() sets playback_speed from the animation that is *current*
     * before it switches (player.gd:202-212), so the idle->move frame advances
     * under speed 1 and only later frames use the move ratio. */
    unsigned char mode = moving ? 2 : 1;
    float speed = playback;
    if (s->player_anim_mode != mode) {
        s->player_anim_mode = mode; bt_player_anim_store(s, 0.0f); speed = 1.0f;
    }
    bt_player_fx_step(s, moving);
    if (!moving) return 0;
    float delta = BT_DT * speed;
    float next = bt_player_anim_pos(s) + delta;
    /* Math::fposmod for the loop, plus the engine keeping an exact `length`
     * instead of collapsing it to 0. */
    while (next > 0.6f) next -= 0.6f;
    float from = next - delta;
    while (from < 0.0f) from += 0.6f;           /* the engine fposmods a wrapped
                                                 * from_time, splitting the range */
    int steps = from > next
            ? bt_player_anim_keys(from, 0.6f) + bt_player_anim_keys(0.0f, next)
            : bt_player_anim_keys(from, next);
    bt_player_anim_store(s, next);
    return steps;
}

BT_HD static void bt_player_visual_emit_steps(BT_DEVICE BtState* s, int steps) {
    for (int i = 0; i < steps; i++) {
        /* Utils.get_rand_element delegates to Array.pick_random: global randi,
         * not the Utils RandomNumberGenerator stream. */
        unsigned int sound = bt_rng_next(&s->rng) % (s->zone == 0 ? 6u : 4u);
        /* Exact binary32 AudioStreamMP3.get_length values from the pinned
         * engine, in player.tscn's step_sounds / swim_sounds order. */
        unsigned int bits = 0x3F272F05u;
        if (s->zone != 0) {
            if (sound == 0) bits = 0x3E22BFA8u;
            else if (sound == 1) bits = 0x3DD247A8u;
            else if (sound == 2) bits = 0x3E13C594u;
            else bits = 0x3E8524AAu;
        }
        union { unsigned int u; float f; } duration;
        duration.u = bits;
        bt_main_audio_enqueue(s, duration.f, (bt_u64)0x3FB999999999999AULL);
    }
}
