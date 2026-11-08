/* Weapon audio: the SOURCE RNG is consumed at each weapon's shoot / custom-hit
 * event even though rendering is omitted.  Source boundaries:
 *   - a NON-EMPTY shooting_sounds / custom_hit_sounds array draws exactly ONE
 *     bounded sample at the event (Array.pick_random, utils.gd:391-394), size 1
 *     included; every shipped weapon has >= 1 shooting sound (16 tiers carry a
 *     single clip), so the get_rand_element(empty) -> null branch is unreachable
 *     on this path;
 *   - that event draw happens even when the manager later rejects the playback
 *     (SoundManager.play evaluates its sound argument first);
 *   - rejected playback consumes no pitch draw, and a playlist drains one entry
 *     per frame with exactly ONE global pitch draw per started clip, the player
 *     returning after clip_length / pitch_scale seconds;
 *   - clear_queue empties pending entries but keeps players mid-playback.
 * The positional (SoundManager2D) path shares the parent's birth_audio pool, so
 * the hit test asserts through that pool's RNG behaviour only.
 */
#include "../ocean/brotato/brotato_weapon_audio.h"
#include <cstdio>

static BtState state;

/* This fixture hand-builds a BtState (it never runs bt_core_init), so it must
 * state the audio-RNG model it asserts: 1 = the source's sound draws exist,
 * which is bt_core_init's default (see BtState.audio_rng). */
static void audio_rng_default(BtState *s) { s->audio_rng = 1; }

static unsigned short find_shoot_min(void) {
    unsigned short best = BT_WEAPON_NONE;
    int best_n = 1 << 30;
    for (unsigned short i = 0; i < BT_NWEAPONS; i++) {
        int n = (int)BT_WEAPONS[i].shoot_audio_count;
        if (n > 0 && n < best_n) { best_n = n; best = i; }
    }
    return best;
}
static unsigned short find_shoot_max(void) {
    unsigned short best = BT_WEAPON_NONE;
    int best_n = 0;
    for (unsigned short i = 0; i < BT_NWEAPONS; i++)
        if ((int)BT_WEAPONS[i].shoot_audio_count > best_n) {
            best_n = (int)BT_WEAPONS[i].shoot_audio_count; best = i;
        }
    return best;
}
static unsigned short find_hit(int want_nonempty) {
    for (unsigned short i = 0; i < BT_NWEAPONS; i++)
        if (((int)BT_WEAPONS[i].hit_audio_count > 0) == (want_nonempty != 0)) return i;
    return BT_WEAPON_NONE;
}
static int busy_players(void) {
    int n = 0;
    for (int i = 0; i < BT_MAIN_AUDIO_PLAYERS; i++) n += state.main_audio_busy[i] > 0.0f;
    return n;
}
static int all_weapons_have_shoot_sounds(void) {
    for (unsigned short i = 0; i < BT_NWEAPONS; i++)
        if ((int)BT_WEAPONS[i].shoot_audio_count <= 0) return 0;
    return 1;
}

int main() {
    audio_rng_default(&state);
    const unsigned short w_min = find_shoot_min();
    const unsigned short w_max = find_shoot_max();
    if (w_min == BT_WEAPON_NONE || w_max == BT_WEAPON_NONE) {
        std::puts("FAIL: the weapon pool must contain sound-carrying weapons");
        return 1;
    }
    /* Source-backed: no shipped weapon calls get_rand_element on an empty
     * shooting_sounds array, so a null stream never reaches this path. */
    if (!all_weapons_have_shoot_sounds()) {
        std::puts("FAIL: a weapon with an empty sound array reaches the unimplemented null-stream branch");
        return 1;
    }

    /* bt_weapon_audio_shoot takes EXACTLY ONE bounded sample, whatever the array
     * size, and queues that clip's duration. */
    for (int pass = 0; pass < 2; pass++) {
        unsigned short w = pass == 0 ? w_min : w_max;
        unsigned int n = (unsigned int)BT_WEAPONS[w].shoot_audio_count;
        bt_main_audio_reset(&state);
        bt_rng_seed(&state.rng, 11 + pass);
        BtRng probe = state.rng;
        unsigned int idx = bt_rng_bounded(&probe, n);
        bt_weapon_audio_shoot(&state, w);
        if (state.rng.state != probe.state) {
            std::puts("FAIL: a shot must draw exactly one bounded sample");
            return 1;
        }
        if (state.main_audio_count != 1) {
            std::puts("FAIL: an admitted shot must be queued");
            return 1;
        }
        unsigned int a = (unsigned int)BT_WEAPONS[w].shoot_audio_first + idx;
        if (BT_WEAPON_AUDIO_DURATION[a] <= 0.0f) {
            std::puts("FAIL: every selected clip needs a real duration");
            return 1;
        }
        if (state.main_audio_dur[state.main_audio_head] != BT_WEAPON_AUDIO_DURATION[a]) {
            std::puts("FAIL: the queued clip must be the drawn one");
            return 1;
        }
    }

    /* BT_WEAPON_NONE and an out-of-range id are inert. */
    bt_main_audio_reset(&state);
    bt_rng_seed(&state.rng, 5);
    {
        bt_u64 before = state.rng.state;
        bt_weapon_audio_shoot(&state, BT_WEAPON_NONE);
        bt_weapon_audio_shoot(&state, (unsigned short)(BT_NWEAPONS + 3));
        if (state.main_audio_count != 0 || state.rng.state != before) {
            std::puts("FAIL: BT_WEAPON_NONE / out-of-range ids must be inert");
            return 1;
        }
    }

    /* Playback starts one clip per frame with ONE global pitch draw and runs for
     * clip_length / pitch_scale seconds; drain releases the players. */
    bt_main_audio_reset(&state);
    bt_rng_seed(&state.rng, 31);
    bt_weapon_audio_shoot(&state, w_max);
    bt_weapon_audio_shoot(&state, w_max);
    if (state.main_audio_count != 2) {
        std::puts("FAIL: both submissions are admitted while players idle");
        return 1;
    }
    float first_dur = state.main_audio_dur[state.main_audio_head];
    bt_u64 pre_play = state.rng.state;
    bt_main_audio_step(&state);
    if (state.main_audio_count != 1 || busy_players() != 1 || state.rng.state == pre_play) {
        std::puts("FAIL: a frame starts exactly one clip with one pitch draw");
        return 1;
    }
    {
        float busy = 0.0f;
        for (int i = 0; i < BT_MAIN_AUDIO_PLAYERS; i++)
            if (state.main_audio_busy[i] > 0.0f) busy = state.main_audio_busy[i];
        /* pitch_scale = 1 + rand_range(-0.2, 0.2) in [0.8, 1.2]. */
        if (busy > first_dur / 0.8f + 1e-6f || busy < first_dur / 1.2f - 1e-6f) {
            std::puts("FAIL: playback lifetime must be clip_length / pitch_scale");
            return 1;
        }
    }
    pre_play = state.rng.state;
    bt_main_audio_step(&state);
    if (state.main_audio_count != 0 || busy_players() != 2 || state.rng.state == pre_play) {
        std::puts("FAIL: a free second player starts the next clip, one draw");
        return 1;
    }
    for (int i = 0; i < 600; i++) bt_main_audio_step(&state);
    if (busy_players() != 0 || state.main_audio_count != 0) {
        std::puts("FAIL: clips must release their players when their time elapses");
        return 1;
    }
    pre_play = state.rng.state;
    for (int i = 0; i < 10; i++) bt_main_audio_step(&state);
    if (state.rng.state != pre_play) {
        std::puts("FAIL: an idle manager must not keep drawing gameplay randomness");
        return 1;
    }

    /* Admission: no free player rejects the playback, but the source-event draw
     * still happened (the argument is evaluated before play()). */
    bt_main_audio_reset(&state);
    for (int i = 0; i < BT_MAIN_AUDIO_PLAYERS; i++) state.main_audio_busy[i] = 1.0f;
    bt_rng_seed(&state.rng, 41);
    {
        unsigned int n = (unsigned int)BT_WEAPONS[w_max].shoot_audio_count;
        BtRng probe = state.rng;
        (void)bt_rng_bounded(&probe, n);
        bt_weapon_audio_shoot(&state, w_max);
        if (state.main_audio_count != 0 || state.rng.state != probe.state) {
            std::puts("FAIL: rejection keeps the event draw and skips the enqueue");
            return 1;
        }
    }

    /* FIFO bound: MAX_SOUNDS slips, then rejection without a queue change, and a
     * re-submitted queue can still be drained. */
    bt_main_audio_reset(&state);
    bt_rng_seed(&state.rng, 43);
    for (int i = 0; i < 40; i++) bt_weapon_audio_shoot(&state, w_max);
    if (state.main_audio_count != BT_MAIN_AUDIO_MAX) {
        std::puts("FAIL: the main FIFO is bounded at MAX_SOUNDS");
        return 1;
    }
    bt_main_audio_clear_queue(&state);
    if (state.main_audio_count != 0 || busy_players() != 0) {
        std::puts("FAIL: clear_queue empties pending entries only");
        return 1;
    }

    /* always_play == true.  With no free player a forced submission is still
     * admitted by dropping the OLDEST QUEUED entry (never a playing channel) and
     * appending; with an empty queue it simply waits.  The event draw is the
     * caller's, so the enqueue itself never touches the RNG. */
    bt_main_audio_reset(&state);
    bt_rng_seed(&state.rng, 61);
    {
        bt_u64 before = state.rng.state;
        for (int i = 0; i < BT_MAIN_AUDIO_PLAYERS; i++) state.main_audio_busy[i] = 1.0f;
        bt_main_audio_enqueue_force(&state, 1.5f, BT_AUDIO_PITCH_RANGE_01);
        if (state.main_audio_count != 1 || state.rng.state != before) {
            std::puts("FAIL: a forced sound waits in an empty FIFO without a free player");
            return 1;
        }
        if (state.main_audio_dur[state.main_audio_head] != 1.5f) {
            std::puts("FAIL: the forced entry must be the queued one");
            return 1;
        }
        /* A second forced sound replaces the oldest queued entry. */
        bt_main_audio_enqueue_force(&state, 2.5f, BT_AUDIO_PITCH_RANGE_01);
        if (state.main_audio_count != 1 || state.main_audio_dur[state.main_audio_head] != 2.5f) {
            std::puts("FAIL: a forced sound must pop_front the oldest queued entry");
            return 1;
        }
        /* An ordinary submission is still dropped while every player is busy. */
        bt_main_audio_enqueue(&state, 3.5f, BT_AUDIO_PITCH_RANGE_01);
        if (state.main_audio_count != 1 || state.main_audio_dur[state.main_audio_head] != 2.5f) {
            std::puts("FAIL: a non-forced sound must not displace a queued one");
            return 1;
        }
        /* It still starts once a channel frees. */
        state.main_audio_busy[3] = 0.0f;
        bt_u64 pre = state.rng.state;
        bt_main_audio_step(&state);
        if (state.main_audio_count != 0 || state.rng.state == pre) {
            std::puts("FAIL: a waiting forced sound must start on the next free frame");
            return 1;
        }
    }

    /* Forced admission at the FIFO bound: full queue keeps MAX_SOUNDS entries,
     * dropping the oldest. */
    bt_main_audio_reset(&state);
    bt_rng_seed(&state.rng, 67);
    for (int i = 0; i < BT_MAIN_AUDIO_MAX; i++)
        bt_main_audio_enqueue(&state, 1.0f + (float)i, BT_AUDIO_PITCH_RANGE_02);
    if (state.main_audio_count != BT_MAIN_AUDIO_MAX) {
        std::puts("FAIL: setup must fill the main FIFO");
        return 1;
    }
    bt_main_audio_enqueue_force(&state, 99.0f, BT_AUDIO_PITCH_RANGE_02);
    if (state.main_audio_count != BT_MAIN_AUDIO_MAX) {
        std::puts("FAIL: a forced sound keeps the FIFO at its bound");
        return 1;
    }
    if (state.main_audio_dur[state.main_audio_head] != 2.0f) {
        std::puts("FAIL: the forced sound must drop the oldest queued entry");
        return 1;
    }
    if (state.main_audio_dur[(state.main_audio_head + BT_MAIN_AUDIO_MAX - 1) % BT_MAIN_AUDIO_MAX] != 99.0f) {
        std::puts("FAIL: the forced sound must land at the tail");
        return 1;
    }

    /* Custom hit sounds: one bounded draw at the source event, then the shared
     * positional pool plays it (one global pitch draw); a weapon without any
     * custom_hit_sounds draws nothing (weapon.gd:432 guards on size > 0). */
    const unsigned short w_hit = find_hit(1);
    const unsigned short w_nohit = find_hit(0);
    if (w_hit == BT_WEAPON_NONE || w_nohit == BT_WEAPON_NONE) {
        std::puts("FAIL: the pool needs a custom-hit weapon and a plain one");
        return 1;
    }
    bt_birth_audio_reset(&state);
    bt_rng_seed(&state.rng, 53);
    {
        bt_u64 before = state.rng.state;
        bt_weapon_audio_hit(&state, w_nohit);
        bt_weapon_audio_hit(&state, BT_WEAPON_NONE);
        if (state.rng.state != before) {
            std::puts("FAIL: a weapon without custom_hit_sounds must not draw");
            return 1;
        }
    }
    {
        unsigned int n = (unsigned int)BT_WEAPONS[w_hit].hit_audio_count;
        BtRng probe = state.rng;
        (void)bt_rng_bounded(&probe, n);
        bt_weapon_audio_hit(&state, w_hit);
        if (state.rng.state != probe.state) {
            std::puts("FAIL: a custom hit must draw exactly one bounded sample");
            return 1;
        }
    }
    {
        bt_u64 before = state.rng.state;
        bt_birth_audio_step(&state);
        if (state.rng.state == before) {
            std::puts("FAIL: the custom hit sound must reach positional playback");
            return 1;
        }
    }

    std::puts("PASS weapon audio: event draws, admission, FIFO and playback RNG boundaries");
}
