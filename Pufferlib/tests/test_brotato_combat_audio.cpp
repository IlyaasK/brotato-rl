/* tests/test_brotato_combat_audio.cpp
 *
 * Source-backed RNG / queue-boundary regressions for the gameplay half of the
 * non-weapon combat audio (ocean/brotato/brotato_combat_audio.h) and for the
 * consumers this slice wired (brotato_scalar_events.h bt_scalar_heal /
 * bt_scalar_gold_pickup, brotato_item_explosions.h, brotato_progression.h).
 *
 * Only the edges that are NOT self-evident from the data tables are asserted
 * here -- every expectation is the source statement it transcribes, never a
 * copy of the implementation's own arithmetic:
 *
 *   utils.gd:391-394   Utils.get_rand_element -> Array.pick_random:
 *                      ONE `rand() % size` draw, SKIPPED for an empty array,
 *                      taken even for size 1.
 *   utils.gd:397-401   get_chance_success(0.0) is FALSE WITH NO DRAW -- this is
 *                      why the burn tick's take_damage takes no crit draw.
 *   unit.gd:296-323    hurt pick ALWAYS, crit pick ADDITIONALLY and it REPLACES
 *                      the hurt clip; SoundManager2D.play(..., 0, 0.2) normal.
 *   unit.gd:642        apply_burning's first application: ONE burn pick.
 *   unit.gd:672+298+323 the burn tick: burn pick (custom_sound) THEN hurt pick,
 *                      and the BURN clip is what gets queued.
 *   unit.gd:750        _on_one_shotted plays effect.sound_one_shot by value:
 *                      NO pick.
 *   player.gd:440-442  hurt pick always, dodge pick additionally (replacement),
 *                      SoundManager2D.play(..., 0, 0.2, true) = always_play.
 *   player.gd:736      on_healing_effect: ONE hp_regen pick on ANY restored HP,
 *                      SoundManager.play(..., get_heal_db(), 0.1) = main queue.
 *   main.gd:802-804    gold pickup: the sound pick precedes the pickup effects.
 *   main.gd:863        level_up_sound is a literal Resource: NO pick.
 *   main.gd:798-804    ProgressData.settings.alt_gold_sounds defaults false.
 *   main.gd:958/1030/455  end-wave / run-won / run-lost picks.
 *   weapon_service.gd:350-351  special spawn: ONE pick when non-empty, and the
 *                      guard skips BOTH the pick and the play when empty.
 *   player.gd:559-560  explode()'s shooting sound is guarded the same way --
 *                      the Bull's array is empty, so it is a total no-op.
 *   sound_manager.gd:7-14 / sound_manager_2d.gd:7-14  always_play admits by
 *                      replacing the OLDEST QUEUED entry when the manager
 *                      refuses; the ordinary path drops the submission.
 */
#include "../ocean/brotato/brotato_core.h"
#include <cstdio>
#include <cstring>

static BtState st;

static void reset_state(unsigned long long seed) {
    /* This fixture hand-builds a BtState (it never runs bt_core_init), so it
     * must state the audio-RNG model it asserts: 1 = the source's sound draws
     * exist, which is bt_core_init's default (see BtState.audio_rng). */
    st.audio_rng = 1;
    for (int i = 0; i < 12; i++) { st.main_audio_busy[i] = 0.0f; st.birth_audio_busy[i] = 0.0f; }
    bt_main_audio_reset(&st);
    bt_birth_audio_reset(&st);
    bt_item_expl_reset(&st);
    /* the effect buckets are per-scenario in this test: drop everything a
     * previous section registered so each block owns exactly what it registers. */
    st.n_bk_active = 0;
    for (int r = 0; r < BT_NITEM_EFF_ROWS; r++) { st.bk_count[r] = 0; st.nvar[r] = 0; }
    st.rng.state = 0; st.rng.inc = 0;
    bt_rng_seed(&st.rng, seed);
    st.utils_rng.state = 0; st.utils_rng.inc = 0;
    bt_rng_seed(&st.utils_rng, seed + 1);
    st.done = 0; st.hp = 1.0f; st.max_hp = 10.0f;
    /* the heal room (player.gd:709/732 max_stats.health - current_stats.health)
     * reads the player LiveStats snapshot this hand-built state must also state. */
    st.p_max_hp = 10.0f;
}

static int fail(const char* what) {
    std::printf("FAIL: %s\n", what);
    return 1;
}

int main() {
    /* ---- data sanity: every span the runtime can pick indexes a real clip --- */
    for (int ev = 0; ev < BT_NAUDIO_EVENTS; ev++) {
        if (ev == BT_AUDIO_EV_BULL_EXPLODE) continue;   /* source array is empty */
        if (BT_AUDIO_EV_COUNT[ev] == 0u)
            return fail("every non-explosion event has at least one clip");
        for (int k = 0; k < (int)BT_AUDIO_EV_COUNT[ev]; k++)
            if (BT_AUDIO_CLIP_SECONDS[BT_AUDIO_EV_FIRST[ev] + k] <= 0.0f)
                return fail("event span must carry true clip durations");
    }
    if (BT_AUDIO_EV_COUNT[BT_AUDIO_EV_BULL_EXPLODE] != 0u)
        return fail("bull_explosion_stats.shooting_sounds is empty in the source");
    /* the shipped arrays, straight from the scenes */
    if (BT_AUDIO_EV_COUNT[BT_AUDIO_EV_PLAYER_HURT] != 4u ||
        BT_AUDIO_EV_COUNT[BT_AUDIO_EV_PLAYER_DODGE] != 2u ||
        BT_AUDIO_EV_COUNT[BT_AUDIO_EV_HP_REGEN] != 2u ||
        BT_AUDIO_EV_COUNT[BT_AUDIO_EV_ALIEN_EYES] != 2u ||
        BT_AUDIO_EV_COUNT[BT_AUDIO_EV_GOLD_PICKUP] != 5u ||
        BT_AUDIO_EV_COUNT[BT_AUDIO_EV_LEVEL_UP] != 1u ||
        BT_AUDIO_EV_COUNT[BT_AUDIO_EV_END_WAVE] != 1u ||
        BT_AUDIO_EV_COUNT[BT_AUDIO_EV_RUN_WON] != 1u ||
        BT_AUDIO_EV_COUNT[BT_AUDIO_EV_RUN_LOST] != 9u ||
        BT_AUDIO_EV_COUNT[BT_AUDIO_EV_WEAPON_BREAK] != 3u ||
        BT_AUDIO_EV_COUNT[BT_AUDIO_EV_ONE_SHOT] != 1u)
        return fail("event clip counts must match the source arrays");
    /* no enemy/neutral scene overrides unit.tscn's sounds, so every kind and both
     * neutrals resolve the SAME three spans (hurt 8 / crit 4 / burn 4). */
    for (int k = 0; k < BT_NAUDIO_KIND; k++) {
        if (BT_AUDIO_KIND_HURT_COUNT[k] != BT_AUDIO_NEUTRAL_HURT_COUNT[0] ||
            BT_AUDIO_KIND_CRIT_COUNT[k] != BT_AUDIO_NEUTRAL_CRIT_COUNT[0] ||
            BT_AUDIO_KIND_BURN_COUNT[k] != BT_AUDIO_NEUTRAL_BURN_COUNT[0] ||
            BT_AUDIO_KIND_HURT_FIRST[k] != BT_AUDIO_NEUTRAL_HURT_FIRST[0] ||
            BT_AUDIO_KIND_CRIT_FIRST[k] != BT_AUDIO_NEUTRAL_CRIT_FIRST[0] ||
            BT_AUDIO_KIND_BURN_FIRST[k] != BT_AUDIO_NEUTRAL_BURN_FIRST[0])
            return fail("every enemy kind inherits unit.tscn's hurt/crit/burn arrays");
    }
    if (BT_AUDIO_NEUTRAL_HURT_COUNT[1] != 8u && BT_AUDIO_NEUTRAL_HURT_COUNT[1] != 0u)
        return fail("a neutral hurt span must be unit.tscn's or empty");

    /* ---- enemy hit: hurt pick always, crit pick replaces it ---------------- */
    {
        reset_state(101);
        BtRng probe = st.rng;
        unsigned int hi = bt_rng_bounded(&probe, BT_AUDIO_KIND_HURT_COUNT[7]);
        bt_combat_audio_enemy_hit(&st, 7, 0, 0);
        if (st.rng.state != probe.state)
            return fail("a non-crit hit draws exactly one bounded sample");
        if (st.birth_audio_count != 1)
            return fail("an enemy hit sound is submitted to the positional pool");
        if (st.birth_audio_dur[st.birth_audio_head] !=
            BT_AUDIO_CLIP_SECONDS[BT_AUDIO_KIND_HURT_FIRST[7] + hi])
            return fail("a non-crit hit must queue the hurt clip");
        if (st.birth_audio_pitch[st.birth_audio_head] != BT_AUDIO_PITCH_RANGE_02)
            return fail("unit.gd:323 plays with pitch_rand 0.2");

        reset_state(102);
        probe = st.rng;
        unsigned int h2 = bt_rng_bounded(&probe, BT_AUDIO_KIND_HURT_COUNT[7]);
        unsigned int c2 = bt_rng_bounded(&probe, BT_AUDIO_KIND_CRIT_COUNT[7]);
        (void)h2;
        bt_combat_audio_enemy_hit(&st, 7, 0, 1);
        if (st.rng.state != probe.state)
            return fail("a crit hit draws the hurt pick AND the crit pick");
        if (st.birth_audio_dur[st.birth_audio_head] !=
            BT_AUDIO_CLIP_SECONDS[BT_AUDIO_KIND_CRIT_FIRST[7] + c2])
            return fail("unit.gd:301 replaces the hurt clip with the crit clip");
    }

    /* ---- new burn (unit.gd:642) ------------------------------------------- */
    {
        reset_state(103);
        BtRng probe = st.rng;
        unsigned int bi = bt_rng_bounded(&probe, BT_AUDIO_KIND_BURN_COUNT[2]);
        bt_combat_audio_enemy_burn_apply(&st, 2, 0);
        if (st.rng.state != probe.state)
            return fail("applying a first burning draws exactly one bounded sample");
        if (st.birth_audio_count != 1 ||
            st.birth_audio_dur[st.birth_audio_head] !=
            BT_AUDIO_CLIP_SECONDS[BT_AUDIO_KIND_BURN_FIRST[2] + bi])
            return fail("the new-burn sound queues the burn clip");
    }

    /* ---- burn tick (unit.gd:672 + 298 + 323) ------------------------------ */
    for (unsigned long long seed = 1; seed <= 64; seed++) {
        reset_state(seed * 7 + 1);
        BtRng probe = st.rng;
        unsigned int burn_i = bt_rng_bounded(&probe, BT_AUDIO_KIND_BURN_COUNT[0]);
        (void)bt_rng_bounded(&probe, BT_AUDIO_KIND_HURT_COUNT[0]);
        bt_combat_audio_enemy_burn_tick(&st, 0, 0);
        if (st.rng.state != probe.state)
            return fail("the burn tick draws the burn pick then the hurt pick (2)");
        if (st.birth_audio_count != 1 ||
            st.birth_audio_dur[st.birth_audio_head] !=
            BT_AUDIO_CLIP_SECONDS[BT_AUDIO_KIND_BURN_FIRST[0] + burn_i])
            return fail("the custom_sound override queues the BURN clip, and it is the FIRST pick");
    }

    /* ---- direct events take NO pick --------------------------------------- */
    {
        reset_state(104);
        BtRng probe = st.rng;
        bt_combat_audio_level_up(&st);
        if (st.rng.state != probe.state)
            return fail("level_up_sound is played by value: no selection draw");
        if (st.main_audio_count != 1 || st.main_audio_pitch[st.main_audio_head] != BT_COMBAT_PITCH_00)
            return fail("level_up_sound is an always_play main-queue submission at pitch 0");

        reset_state(105);
        probe = st.rng;
        bt_combat_audio_one_shot(&st);
        if (st.rng.state != probe.state)
            return fail("OneShotOnHitEffect.sound_one_shot is played by value: no draw");
        if (st.birth_audio_count != 1 || st.birth_audio_pitch[st.birth_audio_head] != BT_AUDIO_PITCH_RANGE_02)
            return fail("the one-shot sound is a normal positional submission at pitch 0.2");
    }

    /* ---- explosion guard (player.gd:559-560, bull stats empty) ------------- */
    {
        reset_state(106);
        BtRng probe = st.rng;
        bt_combat_audio_player_explosion(&st);
        if (st.rng.state != probe.state || st.birth_audio_count != 0 || st.main_audio_count != 0)
            return fail("the size()>0 guard skips BOTH the draw and the submission");
    }

    /* ---- special spawn projectile (weapon_service.gd:350-351) -------------- */
    {
        int empty_row = -1, full_row = -1;
        for (int w = 0; w < BT_NAUDIO_WSTATS_SHOOT; w++) {
            if (BT_AUDIO_WSTATS_SHOOT_COUNT[w] == 0u) { if (empty_row < 0) empty_row = w; }
            else { if (full_row < 0) full_row = w;
                   for (int k = 0; k < (int)BT_AUDIO_WSTATS_SHOOT_COUNT[w]; k++)
                       if (BT_AUDIO_CLIP_SECONDS[BT_AUDIO_WSTATS_SHOOT_FIRST[w] + k] <= 0.0f)
                           return fail("a special-spawn span must carry true durations"); }
        }
        if (empty_row < 0 || full_row < 0)
            return fail("both a sound-carrying and a silent special-spawn row must exist");
        reset_state(107);
        BtRng probe = st.rng;
        bt_combat_audio_special_projectile(&st, empty_row);
        if (st.rng.state != probe.state || st.birth_audio_count != 0)
            return fail("an empty shooting_sounds array skips the pick and the play");
        reset_state(108);
        probe = st.rng;
        unsigned int idx = bt_rng_bounded(&probe, BT_AUDIO_WSTATS_SHOOT_COUNT[full_row]);
        bt_combat_audio_special_projectile(&st, full_row);
        if (st.rng.state != probe.state)
            return fail("a non-empty special-spawn array draws exactly one sample");
        if (st.birth_audio_count != 1 ||
            st.birth_audio_dur[st.birth_audio_head] !=
            BT_AUDIO_CLIP_SECONDS[BT_AUDIO_WSTATS_SHOOT_FIRST[full_row] + idx])
            return fail("the special-spawn sound queues that clip on the positional pool");
    }

    /* ---- player hurt / dodge: replacements, and always_play ---------------- */
    {
        reset_state(109);
        BtRng probe = st.rng;
        unsigned int hi = bt_rng_bounded(&probe, BT_AUDIO_EV_COUNT[BT_AUDIO_EV_PLAYER_HURT]);
        bt_combat_audio_player_hurt(&st, 0);
        if (st.rng.state != probe.state)
            return fail("a non-dodge player hit draws exactly one sample");
        if (st.birth_audio_dur[st.birth_audio_head] !=
            BT_AUDIO_CLIP_SECONDS[BT_AUDIO_EV_FIRST[BT_AUDIO_EV_PLAYER_HURT] + hi])
            return fail("a non-dodge player hit queues the hurt clip");

        reset_state(110);
        probe = st.rng;
        (void)bt_rng_bounded(&probe, BT_AUDIO_EV_COUNT[BT_AUDIO_EV_PLAYER_HURT]);
        unsigned int di = bt_rng_bounded(&probe, BT_AUDIO_EV_COUNT[BT_AUDIO_EV_PLAYER_DODGE]);
        bt_combat_audio_player_hurt(&st, 1);
        if (st.rng.state != probe.state)
            return fail("a dodged hit draws the hurt pick AND the dodge pick");
        if (st.birth_audio_dur[st.birth_audio_head] !=
            BT_AUDIO_CLIP_SECONDS[BT_AUDIO_EV_FIRST[BT_AUDIO_EV_PLAYER_DODGE] + di])
            return fail("player.gd:442 replaces the hurt clip with the dodge clip");

        /* always_play (player.gd:507): with every player busy the ORDINARY path
         * drops the submission and the forced one still lands. */
        reset_state(111);
        for (int i = 0; i < 12; i++) st.birth_audio_busy[i] = 1.0f;
        bt_positional_audio_enqueue(&st, 0.5f, BT_AUDIO_PITCH_RANGE_02);
        if (st.birth_audio_count != 0)
            return fail("an ordinary submission is dropped while every player is busy");
        bt_combat_audio_player_hurt(&st, 0);
        if (st.birth_audio_count != 1)
            return fail("player.gd:507 uses always_play, so the hit sound still lands");
    }

    /* ---- consumer: bt_scalar_heal (player.gd:730-745) ---------------------- */
    {
        reset_state(112);
        BtRng probe = st.rng;
        unsigned int hi = bt_rng_bounded(&probe, BT_AUDIO_EV_COUNT[BT_AUDIO_EV_HP_REGEN]);
        float healed = bt_scalar_heal(&st, 2.0f);
        if (healed != 2.0f)
            return fail("bt_scalar_heal must restore the clamped amount");
        if (st.rng.state != probe.state)
            return fail("a heal draws exactly one hp_regen pick");
        if (st.main_audio_count != 1 ||
            st.main_audio_dur[st.main_audio_head] !=
            BT_AUDIO_CLIP_SECONDS[BT_AUDIO_EV_FIRST[BT_AUDIO_EV_HP_REGEN] + hi] ||
            st.main_audio_pitch[st.main_audio_head] != BT_AUDIO_PITCH_RANGE_01)
            return fail("the heal sound is a main-queue submission at pitch 0.1");

        reset_state(113);
        st.hp = st.max_hp;
        probe = st.rng;
        if (bt_scalar_heal(&st, 2.0f) != 0.0f) return fail("a full-health heal restores nothing");
        if (st.rng.state != probe.state || st.main_audio_count != 0)
            return fail("a heal that restores no HP takes no pick and submits nothing");
    }

    /* ---- consumer: bt_scalar_gold_pickup (main.gd:798-804) ---------------- */
    {
        reset_state(114);
        BtRng probe = st.rng;
        unsigned int gi = bt_rng_bounded(&probe, BT_AUDIO_EV_COUNT[BT_AUDIO_EV_GOLD_PICKUP]);
        int value = bt_scalar_gold_pickup(&st, 1);
        if (value != 1) return fail("an unmodified gold pickup credits its value");
        /* an owned-effect-free state has no further producers, so the pickup's
         * own sound is the ONLY draw of the event (main.gd:801-804). */
        if (st.rng.state != probe.state)
            return fail("the gold pickup sound is the first and only draw of the event");
        if (st.main_audio_count != 1 ||
            st.main_audio_dur[st.main_audio_head] !=
            BT_AUDIO_CLIP_SECONDS[BT_AUDIO_EV_FIRST[BT_AUDIO_EV_GOLD_PICKUP] + gi])
            return fail("the gold pickup queues the default gold_pickup_sounds clip");
    }

    /* ---- row-addressed explosion sound (player.gd:559-560) ---------------- */
    {
        int hit_row = -1, below_row = -1;
        for (int r = 0; r < BT_NITEM_EFF_ROWS; r++) {
            BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[r];
            if (E->key == BT_ISTR_EXPLODE_ON_HIT && hit_row < 0) hit_row = r;
            if (E->key == BT_ISTR_EXPLODE_WHEN_BELOW_HP && below_row < 0) below_row = r;
        }
        if (hit_row < 0 || below_row < 0)
            return fail("the shipped explode_on_hit / explode_when_below_hp rows must exist");
        int silent = (int)BT_ITEM_EFF_ROWS[hit_row].stats;      /* krakens_eye: no shooting sounds */
        int ringing = (int)BT_ITEM_EFF_ROWS[below_row].stats;   /* sunken_bell: 1 clip */

        reset_state(115);
        BtRng probe = st.rng;
        bt_combat_audio_player_explosion_row(&st, silent);
        if (st.rng.state != probe.state || st.birth_audio_count != 0)
            return fail("an effect whose stats carry no shooting_sounds draws and submits nothing");

        reset_state(116);
        probe = st.rng;
        unsigned int ei = bt_rng_bounded(&probe, BT_AUDIO_WSTATS_SHOOT_COUNT[ringing]);
        bt_combat_audio_player_explosion_row(&st, ringing);
        if (st.rng.state != probe.state || st.birth_audio_count != 1)
            return fail("a row-addressed explosion sound draws once and submits once");
        if (st.birth_audio_dur[st.birth_audio_head] !=
            BT_AUDIO_CLIP_SECONDS[BT_AUDIO_WSTATS_SHOOT_FIRST[ringing] + ei] ||
            st.birth_audio_pitch[st.birth_audio_head] != BT_AUDIO_PITCH_RANGE_02)
            return fail("the item explosion queues ITS OWN stats clip at pitch 0.2 (always_play)");
    }

    /* ---- player explode_on_hit / explode_when_below_hp (player.gd:443-485) -- */
    {
        int hit_row = -1, below_row = -1;
        for (int r = 0; r < BT_NITEM_EFF_ROWS; r++) {
            BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[r];
            if (E->key == BT_ISTR_EXPLODE_ON_HIT && hit_row < 0) hit_row = r;
            if (E->key == BT_ISTR_EXPLODE_WHEN_BELOW_HP && below_row < 0) below_row = r;
        }
        /* one owned copy of each registered through the canonical bucket */
        reset_state(117);
        bt_scalar_bucket_register(&st, hit_row, 1, 0);
        bt_scalar_bucket_register(&st, below_row, 1, 0);
        if (bt_item_expl_player_owned(&st, BT_ISTR_EXPLODE_ON_HIT) != 1 ||
            bt_item_expl_player_owned(&st, BT_ISTR_EXPLODE_WHEN_BELOW_HP) != 1)
            return fail("the bucket must report one owned copy per registered instance");

        /* krakens_eye: chance 0.5 -> one global draw, then TWO Utils._rng draws
         * for the offset, then NO sound (its stats array is empty), then one
         * explosion.  nb_explosions 3 => max_offset (3-1)*20. */
        reset_state(118);
        {
            BtRng gprobe = st.rng, uprobe = st.utils_rng;
            (void)bt_rng_bounded(&gprobe, 1u);          /* the chance roll */
            (void)bt_rng_bounded(&uprobe, 2u * 40u + 1u);
            (void)bt_rng_bounded(&uprobe, 2u * 40u + 1u);
            if (BT_ITEM_EFF_ROWS[hit_row].explode_chance == 0.0f)
                return fail("the shipped explode_on_hit row must carry a chance");
            unsigned int before = st.expl.n_spawned;
            int before_snd = st.birth_audio_count;
            unsigned int u0 = st.utils_rng.state;
            (void)bt_item_expl_player_copy(&st, hit_row, &BT_ITEM_EFF_ROWS[hit_row], 1, 0.0f, 3);
            if (st.rng.state != gprobe.state)
                return fail("the player explosion takes exactly ONE chance draw");
            if (st.birth_audio_count != before_snd)
                return fail("an empty stats shooting_sounds array queues nothing");
            /* offset draws happen only when the chance roll succeeded, and then
             * exactly twice with the (nb-1)*20 scale. */
            if (st.utils_rng.state == u0) {
                if (st.expl.n_spawned != before)
                    return fail("a failed chance roll spawns nothing");
            } else if (st.utils_rng.state == uprobe.state) {
                if (st.expl.n_spawned != before + 1)
                    return fail("a successful chance roll spawns exactly one explosion");
            } else {
                return fail("the offset is TWO Utils._rng draws at the (nb-1)*20 scale");
            }
        }

        /* sunken_bell: chance 1.0 -> still ONE global draw, two offset draws, and
         * the bell clip queued. */
        reset_state(119);
        {
            BtRng gprobe = st.rng;
            (void)bt_rng_bounded(&gprobe, 1u);
            BtRng uprobe = st.utils_rng;
            (void)bt_rng_bounded(&uprobe, 2u * 40u + 1u);
            (void)bt_rng_bounded(&uprobe, 2u * 40u + 1u);
            int before_snd = st.birth_audio_count;
            unsigned int before_g = st.rng.state;
            (void)bt_item_expl_player_copy(&st, below_row, &BT_ITEM_EFF_ROWS[below_row], 1, 0.0f, 3);
            if (st.utils_rng.state != uprobe.state)
                return fail("a chance-1.0 explosion still takes the two offset draws");
            if (st.birth_audio_count != before_snd + 1)
                return fail("sunken_bell's own stats clip is queued with the explosion");
            if (st.rng.state == before_g)
                return fail("chance 1.0 still consumes its one randf sample");
        }

        /* the when-below-hp trigger, in isolation: only the sunken_bell row is
         * owned, so every spawn is that copy's. */
        reset_state(120);
        bt_scalar_bucket_register(&st, below_row, 1, 0);
        for (int i = 0; i < BT_NITEM_EFF_ROWS; i++) st.expl.expl_trig[i] = 1;  /* core's wave-start reset */
        st.hp = st.max_hp;                       /* above sunken_bell's 40% threshold */
        if (bt_item_expl_player_hit(&st, 1) != 0)
            return fail("above hp_threshold nothing explodes");
        if (st.expl.expl_trig[0] != 1)
            return fail("an out-of-threshold frame must not spend the trigger");
        st.hp = st.max_hp * 0.39f;
        unsigned int before_below = st.expl.n_spawned;
        (void)bt_item_expl_player_hit(&st, 1);
        if (st.expl.n_spawned != before_below + 1)
            return fail("below the threshold the owned copy fires once (chance 1.0)");
        if (st.expl.expl_trig[0] != 0)
            return fail("the trigger is spent by the attempt, whatever the chance roll did");
        before_below = st.expl.n_spawned;
        (void)bt_item_expl_player_hit(&st, 1);
        if (st.expl.n_spawned != before_below)
            return fail("a spent trigger must not fire again in the same wave");
    }

    /* ---- scene-keyed node pool: fresh node sounds once, reuse is silent ----- */
    {
        const int expl_scene = bt_item_expl_geom(BT_ISTR_RES___PROJECTILES_EXPLOSION_TSCN);
        if (expl_scene < 0) return fail("explosion.tscn must have a BT_ITEM_SCENES row");
        if (BT_AUDIO_EXPLNODE_COUNT[expl_scene] == 0u)
            return fail("explosion.tscn's node must carry its own explosion_sounds span");

        reset_state(121);
        st.character = BT_CHARACTER_BULL;
        if (!bt_character_explode_on_hit(st.character))
            return fail("the Bull character carries the explode_on_hit row");

        /* nb_explosions == 1 (the character row alone) => max_offset 0, and even
         * then the source takes BOTH Utils._rng offset draws. */
        BtRng uprobe = st.utils_rng;
        (void)bt_rng_bounded(&uprobe, 1u);
        (void)bt_rng_bounded(&uprobe, 1u);
        if (bt_item_expl_player_nb(&st) != 1) return fail("the character row counts as one entry");
        int slot = bt_explode(&st);
        if (slot < 0 || st.expl.n_spawned != 1) return fail("the Bull explosion reaches the shared pool");
        if (st.utils_rng.state != uprobe.state)
            return fail("the Bull explosion takes both Utils offset draws even at range 0");
        if (st.expl.cache_row[slot] != (unsigned char)expl_scene)
            return fail("the node is resident under its own scene");
        if (st.expl.snd_pending[slot] != 1)
            return fail("a freshly instantiated node owes its first-frame sound");

        /* a second explosion while the first node is live instantiates a SECOND
         * node (its scene's stack is empty), which sounds again. */
        int slot2 = bt_explode(&st);
        if (slot2 < 0 || slot2 == slot) return fail("a busy scene instantiates another node");
        if (st.expl.snd_pending[slot2] != 1) return fail("the second fresh node owes its sound too");

        /* phase A: one draw + one positional submission per fresh node, and no
         * draw at all for the node that already sounded.  The FIRST tick is the
         * spawn frame (age 0 -> 1, the node is not in the tree yet); the node's
         * first _physics_process is the tick after it. */
        float reward = 0.0f;
        BtRng gprobe = st.rng;
        unsigned int ci = bt_rng_bounded(&gprobe, BT_AUDIO_EXPLNODE_COUNT[expl_scene]);
        unsigned int ci2 = bt_rng_bounded(&gprobe, BT_AUDIO_EXPLNODE_COUNT[expl_scene]);
        int q0 = st.birth_audio_count;
        (void)bt_item_expl_tick(&st, &reward);                      /* spawn frame */
        (void)bt_item_expl_tick(&st, &reward);                      /* first physics frame */
        if (st.birth_audio_count != q0 + 2)
            return fail("every fresh node submits exactly one node sound");
        if (st.rng.state != gprobe.state)
            return fail("each fresh node takes exactly one pick (two nodes, two draws)");
        {
            unsigned short base = BT_AUDIO_EXPLNODE_FIRST[expl_scene];
            float a = st.birth_audio_dur[st.birth_audio_head];
            float b = st.birth_audio_dur[(st.birth_audio_head + 1) % 16];
            float x = BT_AUDIO_CLIP_SECONDS[base + ci];
            float y = BT_AUDIO_CLIP_SECONDS[base + ci2];
            if (!((a == x && b == y) || (a == y && b == x)))
                return fail("both node sounds queue their own scene's clip");
        }
        if (st.expl.snd_pending[slot] || st.expl.snd_pending[slot2])
            return fail("the sound is owed only once per node");

        /* let both nodes expire, then re-spawn: the resident node is REUSED and
         * must NOT sound again. */
        for (int t = 0; t < BT_EXPL_FRAMES + 2; t++) (void)bt_item_expl_tick(&st, &reward);
        int slot3 = bt_explode(&st);
        if (slot3 < 0) return fail("a returned node must be reusable");
        if (slot3 != slot && slot3 != slot2)
            return fail("the reuse takes one of the scene's resident nodes");
        if (st.expl.snd_pending[slot3] != 0)
            return fail("a reused pooled node must NOT sound again");
        BtRng g2 = st.rng;
        int q1 = st.birth_audio_count;
        (void)bt_item_expl_tick(&st, &reward);                      /* spawn frame */
        (void)bt_item_expl_tick(&st, &reward);                      /* first physics frame */
        if (st.rng.state != g2.state || st.birth_audio_count != q1)
            return fail("a reused node draws nothing and submits nothing");
    }

    /* ---- explosion hit set is keyed on the source OBJECT, not the live slot -- */
    {
        const int scene = bt_item_expl_geom(BT_ISTR_RES___PROJECTILES_EXPLOSION_TSCN);
        float reward = 0.0f;
        int kx = -1, ky = -1;
        for (int k = 0; k < BT_NKINDS; k++) {
            if (!std::strcmp(BT_KIND_NAME[k], "chaser")) kx = k;
            if (!std::strcmp(BT_KIND_NAME[k], "fly")) ky = k;
        }
        if (kx < 0 || ky < 0) return fail("explosion target kinds exist");

        /* (1) an enemy killed inside the window and replaced by a FRESH node in
         * the same slot is still hit (its uid is new). */
        bt_core_init(&st, 122, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        reset_state(122);
        bt_spawn_kind_at(&st, 0, kx, st.px, st.py, 0);
        st.emax[0] = 1000.0f; st.ehp[0] = 1000.0f;
        unsigned int uid_a = bt_object_uid(&st, 0);
        int s0 = bt_item_expl_spawn_geom(&st, scene, 1.0f, st.px, st.py, 30.0f,
                                         0.0f, 1.5f, 0.0f, 0.0f, 0.0f, -15);
        if (s0 < 0) return fail("the regression needs a live explosion");
        (void)bt_item_expl_tick(&st, &reward);                  /* spawn frame */
        (void)bt_item_expl_tick(&st, &reward);                  /* age 1: hits */
        if (st.ehp[0] >= 1000.0f) return fail("an explosion hits a target inside its radius");
        float hp_after_first = st.ehp[0];
        (void)bt_item_expl_tick(&st, &reward);                  /* age 2 */
        if (st.ehp[0] != hp_after_first)
            return fail("a target is hit at most ONCE per explosion");
        /* kill it and put a FRESH object into the same slot */
        st.ehp[0] = 0.0f;
        bt_mark_enemy_dead(&st, 0, 0);
        bt_spawn_kind_at(&st, 0, ky, st.px, st.py, 0);
        st.emax[0] = 1000.0f; st.ehp[0] = 1000.0f;
        if (bt_object_uid(&st, 0) == uid_a)
            return fail("a fresh node in the same slot must mint a new source identity");
        (void)bt_item_expl_tick(&st, &reward);                  /* age 3 */
        if (st.ehp[0] >= 1000.0f)
            return fail("a fresh object in a reused slot must still be hit");

        /* (2) a POOLED respawn (same kind) keeps the node identity, so the same
         * explosion must NOT damage it again. */
        bt_core_init(&st, 123, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        reset_state(123);
        bt_spawn_kind_at(&st, 0, kx, st.px, st.py, 0);
        st.emax[0] = 1000.0f; st.ehp[0] = 1000.0f;
        unsigned int uid_p = bt_object_uid(&st, 0);
        if (bt_item_expl_spawn_geom(&st, scene, 1.0f, st.px, st.py, 30.0f,
                                    0.0f, 1.5f, 0.0f, 0.0f, 0.0f, -15) < 0)
            return fail("the pooled regression needs a live explosion");
        (void)bt_item_expl_tick(&st, &reward);
        (void)bt_item_expl_tick(&st, &reward);
        if (st.ehp[0] >= 1000.0f) return fail("the pooled target must be hit once first");
        st.ehp[0] = 0.0f;
        bt_mark_enemy_dead(&st, 0, 0);
        for (int t = 0; t < BT_POOL_DEATH_FRAMES; t++) bt_enemy_pool_step(&st);
        bt_spawn_kind_at(&st, 0, kx, st.px, st.py, 0);            /* same kind -> pooled */
        st.emax[0] = 1000.0f; st.ehp[0] = 1000.0f;
        if (bt_object_uid(&st, 0) != uid_p)
            return fail("a pooled respawn keeps its source identity");
        (void)bt_item_expl_tick(&st, &reward);
        if (st.ehp[0] != 1000.0f)
            return fail("a pooled node is never damaged twice by the same explosion");
    }

    std::puts("test_brotato_combat_audio: all regressions passed");
    return 0;
}
