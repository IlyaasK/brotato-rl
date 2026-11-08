/* Audio-RNG model regression (BtState.audio_rng).
 *
 * The RL bridge's audiovisual neutralization clears the SoundManager /
 * SoundManager2D queues before their own _process pops them (mod_main.gd
 * `_clear_sound_queues`) and pins the floating-text manager, emptying only ITS
 * four sound arrays.  A weapon's shooting_sounds / custom_hit_sounds and a
 * unit's hurt sounds are NOT emptied, and the source evaluates
 * `Utils.get_rand_element(<array>)` BEFORE `SoundManager*.play`, so in the
 * configuration the bridge records:
 *   * every gameplay sound-element pick STILL draws (Array.pick_random over a
 *     non-empty gameplay array, utils.gd:391-394);
 *   * the playback pitch rand_range (sound_manager_parent.gd:45, popped in
 *     brotato_birth_audio.h / brotato_weapon_audio.h) does NOT;
 *   * the floating-text picks/rand_range do NOT (that manager is pinned).
 * `audio_rng = 0` (the `audio_rng` [env] key, and the trace metadata
 * replay_cpu.py forwards) models exactly that: the PICK draws stay, the
 * queue/pitch side is skipped.  Getting this backwards shifts the shared global
 * stream by one word per weapon shot: measured on the Demon seed-1 endless
 * Ghost-Scepter capture, the first cooldown draw sits exactly one
 * Array.pick_random word later than a model that skips the pick
 * (ocean/brotato/bridge/replay_cpu.py vs that capture; first_divergence moved
 * from tick 158 to 170 once the pick was kept).
 *
 * Pinned here:
 *   1. audio_rng changes neither the seeded streams nor the run prologue state;
 *   2. the gameplay sound-element picks draw with BOTH models and consume the
 *      same words; an empty array draws in neither;
 *   3. with audio_rng = 0 nothing is queued (the bridge's empty-queue state) and
 *      no playback-pitch draw happens, while audio_rng = 1 does both;
 *   4. a neutralized run is deterministic (two identical runs are equal).
 * The flag only changes which RNG words are consumed: the observation/action
 * contract is untouched. */
#include "../ocean/brotato/brotato_core.h"
#include <cstdio>
#include <cstdlib>

static int g_checks = 0, g_failures = 0;

static void check(bool ok, const char *what) {
    g_checks++;
    if (!ok) {
        g_failures++;
        std::printf("FAIL: %s\n", what);
    }
}

/* The canonical Ghost Scepter tier-1 id (the profile the Demon traces record). */
#define AUDIO_RNG_TEST_WEAPON ((unsigned short)BT_WEAPON_IDX_WEAPON_GHOST_SCEPTER_1)

static BtState A;   /* audio_rng = 1 (legacy source draws) */
static BtState B;   /* audio_rng = 0 (bridge-neutralized) */

static void init_states(void) {
    bt_core_init(&A, 1u, 0, 1, (unsigned short)BT_CHARACTER_DEMON,
                 AUDIO_RNG_TEST_WEAPON, 1);
    bt_core_init(&B, 1u, 0, 1, (unsigned short)BT_CHARACTER_DEMON,
                 AUDIO_RNG_TEST_WEAPON, 0);
}

/* Post-init stream state of the neutralized run, before any audio call below
 * advances it. */
static BtRng B0;

int main(void) {
    init_states();
    B0 = B.rng;

    /* 1. the flag itself and the seeded streams / prologue state */
    check(A.audio_rng == 1, "audio_rng = 1 is recorded on the state");
    check(B.audio_rng == 0, "audio_rng = 0 is recorded on the state");
    check(A.rng.state == B.rng.state,
          "audio_rng does not change the seeded global stream");
    check(A.utils_rng.state == B.utils_rng.state,
          "audio_rng does not change the seeded Utils stream");
    check(A.hp == B.hp && A.max_hp == B.max_hp && A.materials == B.materials &&
              A.px == B.px && A.py == B.py && A.wave == B.wave,
          "the run prologue state is identical with and without the audio model");

    /* 2a. a weapon shot: the sound PICK is a gameplay draw and happens in BOTH
     * models (ranged_weapon_shooting_behavior.gd:10 evaluates
     * Utils.get_rand_element BEFORE SoundManager.play); only audio_rng = 1
     * queues the clip and draws its playback pitch on the next step. */
    {
        const unsigned int n =
            (unsigned int)BT_WEAPONS[AUDIO_RNG_TEST_WEAPON].shoot_audio_count;
        check(n > 0u, "the probe weapon carries a shooting sound (a pick exists)");
        BtRng a_rng = A.rng, b_rng = B.rng;
        BtRng probe = b_rng;
        (void)bt_rng_bounded(&probe, n);   /* the one bounded pick */
        bt_weapon_audio_shoot(&A, AUDIO_RNG_TEST_WEAPON);
        bt_main_audio_step(&A);
        bt_weapon_audio_shoot(&B, AUDIO_RNG_TEST_WEAPON);
        check(B.rng.state == probe.state,
              "audio_rng = 0 keeps the weapon sound pick (exactly one draw)");
        check(B.main_audio_count == 0,
              "audio_rng = 0 queues no clip (the bridge's empty queues)");
        BtRng after_shot = B.rng;
        bt_main_audio_step(&B);
        check(B.rng.state == after_shot.state,
              "audio_rng = 0 draws no weapon playback pitch");
        check(A.rng.state != a_rng.state,
              "audio_rng = 1 consumes the weapon sound pick + pitch");
        check(A.main_audio_count == 0 && B.main_audio_count == 0,
              "the weapon clip is drained (or never queued) in both models");
    }

    /* 2b. a positional (birth) sound: enqueue + the drain's pitch. */
    {
        BtRng b_rng = B.rng;
        BtRng a_rng = A.rng;
        bt_birth_audio_enqueue(&A, 1);
        bt_birth_audio_step(&A);
        bt_birth_audio_enqueue(&B, 1);
        bt_birth_audio_step(&B);
        check(A.rng.state != a_rng.state,
              "audio_rng = 1 consumes the positional playback pitch");
        check(B.rng.state == b_rng.state,
              "audio_rng = 0 consumes no positional-audio draw");
        check(B.birth_audio_count == 0,
              "audio_rng = 0 queues nothing (the bridge's empty sound queues)");
    }

    /* 2c. the combat-audio element picks (enemy hit / player hurt spans) are
     * gameplay draws too: both models drain the SAME word, so the picked clip
     * matches; an empty span draws in neither. */
    {
        unsigned short first = BT_AUDIO_EV_FIRST[BT_AUDIO_EV_PLAYER_HURT];
        unsigned short count = BT_AUDIO_EV_COUNT[BT_AUDIO_EV_PLAYER_HURT];
        check(count > 0u, "the player-hurt span has clips (a pick is possible)");
        BtRng a_rng = A.rng, b_rng = B.rng;
        float da = bt_combat_audio_clip(&A, first, count);
        float db = bt_combat_audio_clip(&B, first, count);
        BtRng b_probe = b_rng;
        unsigned int b_idx = bt_rng_bounded(&b_probe, (unsigned int)count);
        check(A.rng.state != a_rng.state, "audio_rng = 1 picks a hurt clip (draw)");
        check(B.rng.state == b_probe.state,
              "audio_rng = 0 keeps the hurt-clip pick (exactly one draw)");
        check(db > 0.0f && db == BT_AUDIO_CLIP_SECONDS[(unsigned int)first + b_idx],
              "the neutralized model reports the picked clip");
        check(da > 0.0f, "audio_rng = 1 reports a clip duration");
    }
    {
        const unsigned short efirst = BT_AUDIO_EV_FIRST[BT_AUDIO_EV_BULL_EXPLODE];
        const unsigned short ecount = BT_AUDIO_EV_COUNT[BT_AUDIO_EV_BULL_EXPLODE];
        BtRng a_rng = A.rng, b_rng = B.rng;
        (void)bt_combat_audio_player_explosion(&A);
        (void)bt_combat_audio_player_explosion(&B);
        if (ecount == 0u) {
            check(A.rng.state == a_rng.state && B.rng.state == b_rng.state,
                  "an empty sound array draws in neither model");
        } else {
            check(A.rng.state != a_rng.state && B.rng.state != b_rng.state &&
                      A.rng.state == B.rng.state,
                  "a non-empty span draws the same word in both models");
        }
        (void)efirst;
    }

    /* 2d. the explosion-node pick. */
    if (BT_NAUDIO_EXPLNODE_SCENES > 0 && BT_AUDIO_EXPLNODE_COUNT[0] != 0u) {
        BtRng a_rng = A.rng, b_rng = B.rng;
        BtRng b_probe = b_rng;
        (void)bt_rng_bounded(&b_probe, (unsigned int)BT_AUDIO_EXPLNODE_COUNT[0]);
        bt_combat_audio_explosion_node(&A, 0);
        bt_combat_audio_explosion_node(&B, 0);
        check(A.rng.state != a_rng.state, "audio_rng = 1 picks an explosion-node clip");
        check(B.rng.state == b_probe.state,
              "audio_rng = 0 keeps the explosion-node pick (exactly one draw)");
    }

    /* 2e. consecutive picks stay aligned: two fresh same-seeded states consume
     * the same pick words under both models (the pick is never gated). */
    {
        static BtState C, D;
        bt_core_init(&C, 7u, 0, 1, (unsigned short)BT_CHARACTER_DEMON,
                     AUDIO_RNG_TEST_WEAPON, 1);
        bt_core_init(&D, 7u, 0, 1, (unsigned short)BT_CHARACTER_DEMON,
                     AUDIO_RNG_TEST_WEAPON, 0);
        BtRng d_rng = D.rng;
        bt_weapon_audio_shoot(&C, AUDIO_RNG_TEST_WEAPON);
        bt_weapon_audio_shoot(&D, AUDIO_RNG_TEST_WEAPON);
        check(C.rng.state == D.rng.state,
              "the first pick word is identical in both models");
        bt_weapon_audio_shoot(&C, AUDIO_RNG_TEST_WEAPON);
        bt_weapon_audio_shoot(&D, AUDIO_RNG_TEST_WEAPON);
        check(C.rng.state == D.rng.state,
              "consecutive picks stay aligned (the pick is not gated)");
        check(D.rng.state != d_rng.state,
              "the picks advance the neutralized stream");
    }

    /* 3/4. determinism: identical neutralized runs stay identical, and the two
     * models diverge once a sound event has happened (the flag is effective). */
    {
        static BtState C;
        bt_core_init(&C, 1u, 0, 1, (unsigned short)BT_CHARACTER_DEMON,
                     AUDIO_RNG_TEST_WEAPON, 0);
        bool same_as_b = C.rng.state == B0.state && C.hp == B.hp;
        bool both_neutral = C.audio_rng == 0 && B.audio_rng == 0;
        check(same_as_b && both_neutral, "two audio_rng = 0 runs start identical");
        /* A and B differ only because A consumed the playback-pitch words (and
         * the same pick words) above. */
        check(A.rng.state != B.rng.state,
              "the audio model changes the stream once a sound event happened");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
