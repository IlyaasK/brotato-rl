/* tests/test_brotato_endless.cpp
 *
 * Regression tests for Brotato ENDLESS mode (ocean/brotato/brotato_endless.h +
 * the wave-20 continuation hook in brotato_core.h).  Self-contained: includes
 * only the core header and <stdio.h>.  No Env, no raylib, no engine.
 *
 * Build and run:
 *     c++ -O2 -o build/test_brotato_endless tests/test_brotato_endless.cpp -lm
 *     ./build/test_brotato_endless
 *
 * Exit code 0 = all checks passed.
 *
 * Coverage:
 *   [1] get_endless_factor E(wave) vs the run_data.gd:775-783 integer formula
 *   [2] enemy hp/dmg/speed endless multipliers incl. the wave-51 speed cap
 *   [3] hp-percent-damage divisor (Giant Belt / Greek Fire nerf)
 *   [4] campaign: waves <= 20 keep factor 1.0 exactly
 *   [5] enemy hp/dmg/speed actually scale past wave 20
 *   [6] wave-20 continuation: endless does NOT win, campaign does
 *   [7] endless wave-20+ shop/advance path continues
 *   [8] elite reschedule at every 10th wave (main.gd:931-933)
 *   [9] endless boss/elite eligibility (is_boss group on waves 30, 40, ...)
 *  [10] iframes shrink with the endless factor (player.gd:565-573)
 *  [11] reroll price endless term (item_service.gd:726-730)
 *  [12] log.endless_wave latches the highest wave
 */

#include "../ocean/brotato/brotato_core.h"

#include <stdio.h>

static int g_failures = 0;
static int g_checks = 0;

static void check(int cond, const char* msg) {
    g_checks++;
    if (!cond) { g_failures++; printf("  FAIL: %s\n", msg); }
}
static void check_near(float got, float want, float tol, const char* msg) {
    g_checks++;
    if (!(bt_fabsf(got - want) <= tol)) {
        g_failures++;
        printf("  FAIL: %s (got %.6f want %.6f tol %g)\n", msg, got, want, tol);
    }
}

/* BtState is ~1 MiB (it embeds the persistent contact world); keep big states
 * static so they never sit on the stack. */
static BtState S;
static BtState D;   /* dedicated stream for the Demon section */

/* Step (no-op action) until the wave-end window has elapsed and the phase is no
 * longer combat.  The wave-cleared transition (bt_core_step's s->end_wave_active
 * window, BT_END_WAVE_TICKS = 120 ticks = main.gd's 2.0 s EndWaveTimer) keeps
 * obs[7]/phase at 0 after the wave timer expires, so the shop/upgrade phase only
 * flips at the end of it. */
static void step_until_shop(BtState* s, float* obs, float* rew, float* term, int maxsteps) {
    for (int i = 0; i < maxsteps && s->phase == 0 && s->done == 0; i++)
        bt_core_step(s, 0, obs, rew, term);
}

/* run_data.gd:775-783 in binary64 reference arithmetic. */
static double ref_endless_factor(int wave) {
    int ew = wave - 20; if (ew < 0) ew = 0;
    double mult = 2.0 + (wave > 35 ? (wave - 35) * 0.2 : 0.0);
    double tri = (double)(ew * (ew + 1) / 2);
    double f = tri / 100.0;
    if (f < 0.0) f = 0.0;
    return f * mult;
}

int main(void) {
    float obs[BT_OBS_SIZE], rew, term;

    printf("[1] get_endless_factor E(wave)\n");
    {
        static const int waves[] = { 20, 21, 25, 30, 35, 36, 40, 45, 50, 51, 60 };
        for (unsigned i = 0; i < sizeof(waves) / sizeof(waves[0]); i++) {
            int w = waves[i];
            float got = bt_loot_endless_factor(w);
            check_near(got, (float)ref_endless_factor(w), 1e-4f * (float)(1 + (int)ref_endless_factor(w)),
                       "E(wave) matches run_data.gd:775-783");
        }
        check_near(bt_loot_endless_factor(20), 0.0f, 0.0f, "E(20) == 0 (campaign end)");
        check_near(bt_loot_endless_factor(1), 0.0f, 0.0f, "E(1) == 0");
        check_near(bt_loot_endless_factor(21), 0.02f, 1e-6f, "E(21) == 0.02");
        check_near(bt_loot_endless_factor(30), 1.10f, 1e-5f, "E(30) == 1.10");
        check_near(bt_loot_endless_factor(40), 6.30f, 1e-5f, "E(40) == 6.30");
        check_near(bt_loot_endless_factor(51), 25.792f, 1e-4f, "E(51) == 25.792");
    }

    printf("[2] enemy endless multipliers\n");
    {
        check_near(bt_endless_enemy_health_mult(20), 1.0f, 0.0f, "hp mult 1.0 at wave 20");
        check_near(bt_endless_enemy_damage_mult(20), 1.0f, 0.0f, "dmg mult 1.0 at wave 20");
        check_near(bt_endless_enemy_speed_mult(20), 1.0f, 0.0f, "speed mult 1.0 at wave 20");
        check_near(bt_endless_enemy_health_mult(21), 1.0f + 0.02f * 2.25f, 1e-6f, "hp mult 1+E*2.25");
        check_near(bt_endless_enemy_damage_mult(40), 1.0f + 6.30f, 1e-4f, "dmg mult 1+E");
        check_near(bt_endless_enemy_health_mult(40), 1.0f + 6.30f * 2.25f, 1e-3f, "hp mult 1+E*2.25 at 40");
        /* speed: 1 + min(1.75, E/13.33); the cap binds at wave 51. */
        check_near(bt_endless_enemy_speed_mult(50), 1.0f + (float)(23.25 / 13.33), 1e-5f,
                   "speed mult uncapped at wave 50");
        check_near(bt_endless_enemy_speed_mult(51), 1.0f + 1.75f, 1e-6f, "speed cap 1.75 at wave 51");
        check_near(bt_endless_enemy_speed_mult(200), 1.0f + 1.75f, 1e-6f, "speed cap holds far past 51");
    }

    printf("[3] hp-percent-damage divisor (Giant Belt / Greek Fire)\n");
    {
        check_near(bt_endless_hp_pct_divisor(20), 1.0f, 0.0f, "divisor 1 at wave 20");
        check_near(bt_endless_hp_pct_divisor(30), 1.0f, 0.0f, "divisor 1 while E*0.2 < 1 (E=1.10)");
        check_near(bt_endless_hp_pct_divisor(40), 6.30f * 0.2f, 1e-5f, "divisor E*0.2 at wave 40");
        check_near(bt_endless_hp_pct_divisor(51), 25.792f * 0.2f, 1e-4f, "divisor E*0.2 at wave 51");
    }

    printf("[4]/[5] enemy final stats: campaign unchanged, endless scales\n");
    {
        bt_core_init(&S, 7u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        S.endless = 1;
        /* wave 20 (campaign end): factors are 1.0, so the final stats equal the
         * pre-endless core's round(base * danger * effects). */
        bt_enter_wave(&S, 20);
        float h20 = bt_enemy_final_health(&S, 100.0f, 0.0f);
        float d20 = bt_enemy_final_damage(&S, 100.0f, 0.0f);
        check_near(h20, bt_roundf(100.0f * BT_DANGER_HP), 1e-4f, "wave-20 hp == round(100*danger_hp)");
        check_near(d20, bt_roundf(100.0f * BT_DANGER_DMG), 1e-4f, "wave-20 dmg == round(100*danger_dmg)");
        /* wave 21: the endless factors engage. */
        bt_enter_wave(&S, 21);
        float h21 = bt_enemy_final_health(&S, 100.0f, 0.0f);
        float d21 = bt_enemy_final_damage(&S, 100.0f, 0.0f);
        check_near(h21, bt_roundf(100.0f * BT_DANGER_HP * bt_endless_enemy_health_mult(21)), 1e-3f,
                   "wave-21 hp scaled by endless health factor");
        check_near(d21, bt_roundf(100.0f * BT_DANGER_DMG * bt_endless_enemy_damage_mult(21)), 1e-3f,
                   "wave-21 dmg scaled by endless damage factor");
        check(h21 > h20, "endless hp > campaign hp at the same base");
        check(d21 > d20, "endless dmg > campaign dmg at the same base");
        /* speed too (base 100, danger does not touch speed). */
        float s20 = bt_enemy_final_speed(&S, 100.0f, 0.0f);   /* wave 21 now */
        bt_enter_wave(&S, 20);
        float s20c = bt_enemy_final_speed(&S, 100.0f, 0.0f);
        check_near(s20c, 100.0f, 1e-4f, "wave-20 speed unscaled");
        check_near(s20, bt_roundf(100.0f * bt_endless_enemy_speed_mult(21)), 1e-3f,
                   "wave-21 speed scaled");
    }

    printf("[6] wave-20 continuation: endless vs campaign\n");
    {
        /* campaign: the wave-20 clear ends the run as a win.  bt_core_step
         * resets the state on done, so observe the returned terminal flag. */
        bt_core_init(&S, 11u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        S.endless = 0;
        bt_enter_wave(&S, 20);
        S.hp = S.max_hp;
        S.time_left = 0.001f;
        bt_core_step(&S, 0, obs, &rew, &term);
        check(term == 1.0f, "campaign: wave 20 terminates the episode (won)");
        /* endless: the wave-20 clear opens the next shop instead. */
        bt_core_init(&S, 11u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        S.endless = 1;
        bt_enter_wave(&S, 20);
        S.hp = S.max_hp;
        S.time_left = 0.001f;
        bt_core_step(&S, 0, obs, &rew, &term);
        check(term == 0.0f && S.done == 0 && S.won == 0, "endless: wave 20 does NOT end the run");
        step_until_shop(&S, obs, &rew, &term, 400);
        check(S.phase == 1 || S.phase == 2, "endless: wave 20 opens the shop/upgrade");
        check(S.waves_cleared >= 1, "endless: wave 20 counts as cleared");
        /* the Go action advances into wave 21 (still no run end). */
        for (int i = 0; i < 16 && S.phase == 2; i++) {
            S.shop_timer = 0;
            bt_core_step(&S, 0, obs, &rew, &term);
        }
        S.shop_timer = 0;
        bt_core_step(&S, 8, obs, &rew, &term);
        check(S.wave == 21, "endless: Go advances to wave 21");
        check(term == 0.0f && S.done == 0 && S.won == 0, "endless: wave 21 is live");
    }

    printf("[7] endless wave entry (no OOB, wave counter advances)\n");
    {
        bt_core_init(&S, 13u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        S.endless = 1;
        bt_enter_wave(&S, 50);
        check(S.wave == 50, "bt_enter_wave(50) sets wave 50");
        check(S.endless_max_wave == 50, "endless_max_wave latched to 50");
        check(S.wave_duration > 0.0f, "endless wave has a positive duration");
        check(S.n_live_groups > 0, "endless wave builds a live-group list");
    }

    printf("[8] elite reschedule at every 10th wave\n");
    {
        bt_core_init(&S, 17u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        check(S.oe_n == 0, "no rescheduled elites before wave 10");
        /* wave 10 end schedules base 20 -> events in waves 21..28. */
        bt_enter_wave(&S, 10);
        S.hp = S.max_hp;
        S.time_left = 0.001f;
        bt_core_step(&S, 0, obs, &rew, &term);
        check(S.oe_n == 3, "wave 10 end appends 3 rescheduled elite events");
        check(S.oe_overflow == 0, "reschedule fits the buffer");
        int all_in = 1;
        for (int i = 0; i < S.oe_n; i++)
            if (S.oe_wave[i] < 21 || S.oe_wave[i] > 28) all_in = 0;
        check(all_in, "rescheduled waves land in [base+1, base+8] = [21,28]");
        check(S.oe_type[2] == 0 && S.oe_kind[2] != 0, "event 2 is a forced ELITE with a kind");
        /* the fired events are compacted away on the next decade's schedule. */
        S.oe_fired[0] = 1; S.oe_fired[1] = 1; S.oe_fired[2] = 1;
        bt_schedule_elites_endless(&S, 30, 0.0f);
        check(S.oe_n == 3, "fired events compact out; buffer stays at 3");
        all_in = 1;
        for (int i = 0; i < S.oe_n; i++)
            if (S.oe_wave[i] < 31 || S.oe_wave[i] > 38) all_in = 0;
        check(all_in, "second decade schedules [31,38]");

        /* Skipped-wave robustness: a schedule whose waves are never visited
         * (a jumped/skippable wave) must be reclaimed by the "wave already
         * passed" predicate, or oe_n would grow 3 per decade and trip the cap
         * at deep waves.  Regression for TrainReadiness' build/brotato_state_size
         * probe (oe=1 at deep endless). */
        bt_core_init(&S, 31u, 0, 1, BT_CHARACTER_DEMON,
                     bt_weapon_id_from_name("weapon_ghost_scepter_1"));
        S.endless = 1;
        S.wave = 10;
        bt_schedule_elites_endless(&S, 20, 0.0f);
        S.oe_overflow = 0;
        for (int W = 20; W <= 90; W += 10) {
            S.wave = W;
            bt_schedule_elites_endless(&S, W + 10, 0.0f);
        }
        check(S.oe_overflow == 0, "skipped waves never trip oe_overflow");
        check(S.oe_n <= BT_ENDLESS_ELITE_SLOTS, "oe_n stays within the two-decade bound");
        {
            int future = 1;
            for (int i = 0; i < (int)S.oe_n; i++)
                if ((int)S.oe_wave[i] <= S.wave) future = 0;
            check(future, "only future-scheduled events remain after compaction");
        }

        /* Duplicate base_wave (an hourglass wave replay re-ends the same
         * multiple-of-10 wave) must be a no-op -- run_data.gd:init_elites_spawn's
         * check_elite_generation guard.  This is the trigger that tripped
         * oe_overflow with all items owned (build/brotato_state_size deep probe). */
        bt_core_init(&S, 37u, 0, 1, BT_CHARACTER_DEMON,
                     bt_weapon_id_from_name("weapon_ghost_scepter_1"));
        S.endless = 1;
        S.wave = 50;
        bt_schedule_elites_endless(&S, 60, 0.0f);
        int n1 = (int)S.oe_n;
        bt_schedule_elites_endless(&S, 60, 0.0f);   /* the replay's second end */
        check((int)S.oe_n == n1, "duplicate base_wave schedule is a no-op");
        check(S.oe_overflow == 0, "duplicate base_wave does not overflow");
        check(n1 == 3 && S.oe_wave[0] >= 61 && S.oe_wave[0] <= 68,
              "the first schedule still produced the 3 events of base 60");
    }

    printf("[9] endless boss/elite eligibility waves\n");
    {
        check(!bt_endless_wave_has_boss_group(20), "wave 20 handled by the final-wave path, not this");
        check(!bt_endless_wave_has_boss_group(21), "wave 21 has no reused is_boss group");
        check(!bt_endless_wave_has_boss_group(29), "wave 29 has no reused is_boss group");
        check(bt_endless_wave_has_boss_group(30), "wave 30 reuses the wave-20 resource (base index 19)");
        check(!bt_endless_wave_has_boss_group(39), "wave 39 no");
        check(bt_endless_wave_has_boss_group(40), "wave 40 yes");
        check(bt_endless_wave_has_boss_group(100), "wave 100 yes");
        check(bt_endless_wave_has_boss_group(110), "wave 110 yes");
        check(bt_endless_extra_elites(20) == 0, "no extra endless elites at wave 20");
        check(bt_endless_extra_elites(21) == 1, "ceil(1/10) = 1 at wave 21");
        check(bt_endless_extra_elites(31) == 2, "ceil(11/10) = 2 at wave 31");
        check(bt_endless_extra_elites(61) == 5, "ceil(41/10) = 5 at wave 61");
    }

    printf("[10] iframes shrink with the endless factor\n");
    {
        bt_core_init(&S, 19u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        S.endless = 1;
        bt_enter_wave(&S, 20);
        float i20 = bt_iframes(&S, 5.0f, S.max_hp);   /* 5 dmg on 30 hp -> hits the hi clamp */
        bt_enter_wave(&S, 40);
        float i40 = bt_iframes(&S, 5.0f, S.max_hp);
        check(i20 > 0.0f && i40 > 0.0f, "iframes positive");
        check(i40 < i20, "endless iframes strictly smaller than campaign iframes");
        check_near(i40, i20 / bt_endless_factor(40), 1e-4f, "iframes divided by max(1,E)");
    }

    printf("[11] reroll price endless term\n");
    {
        int c20 = bt_reroll_cost(20, 0);
        int c21 = bt_reroll_cost(21, 0);
        int c40 = bt_reroll_cost(40, 0);
        check(c21 >= c20, "endless reroll cost >= campaign reroll cost");
        check(c40 > c21, "reroll cost grows into endless");
        /* delta = int(max(1, 0.4*wave*sqrt(1+E))) */
        float e = bt_loot_endless_factor(21);
        float d = 0.4f * 21.0f * bt_sqrtf(1.0f + e);
        int delta = (int)(d < 1.0f ? 1.0f : d);
        check(c21 == (int)(21.0f * 0.75f) + delta, "wave-21 reroll normal_price formula");
    }

    printf("[12] log.endless_wave latch\n");
    {
        bt_core_init(&S, 23u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        S.endless = 1;
        bt_enter_wave(&S, 42);
        S.done = 1;
        bt_core_add_log(&S);
        check_near(S.log.endless_wave, 42.0f, 0.0f, "log.endless_wave == highest wave reached");
    }

    printf("[13] Demon + Ghost Scepter endless (Demon-first)\n");
    {
        /* Own BtState: the RNG stream is seeded once per state object and then
         * runs across episodes, so a dedicated state keeps this Demon section
         * independent of the draws earlier sections consumed. */
        unsigned short gs = bt_weapon_id_from_name("weapon_ghost_scepter_1");
        bt_core_init(&D, 29u, 0, 1, BT_CHARACTER_DEMON, gs);
        D.endless = 1;
        check(D.character == BT_CHARACTER_DEMON, "Demon profile loaded");
        check(D.wpn_count == 1, "Demon holds the Ghost Scepter");
        /* the endless factors are character-independent (they read only wave) */
        bt_enter_wave(&D, 40);
        check_near(bt_enemy_final_health(&D, 100.0f, 0.0f),
                   bt_roundf(100.0f * BT_DANGER_HP * bt_endless_enemy_health_mult(40)), 1e-3f,
                   "Demon wave-40 enemy hp uses the same endless factor as Bull");
        check_near(bt_enemy_final_damage(&D, 100.0f, 0.0f),
                   bt_roundf(100.0f * BT_DANGER_DMG * bt_endless_enemy_damage_mult(40)), 1e-3f,
                   "Demon wave-40 enemy dmg uses the same endless factor");
        /* wave-20 continuation with the Demon loadout */
        bt_enter_wave(&D, 20);
        D.hp = D.max_hp;
        D.time_left = 0.001f;
        bt_core_step(&D, 0, obs, &rew, &term);
        check(term == 0.0f && D.won == 0, "Demon endless: wave 20 does not end the run");
        step_until_shop(&D, obs, &rew, &term, 400);
        for (int i = 0; i < 16 && D.phase == 2; i++) { D.shop_timer = 0; bt_core_step(&D, 0, obs, &rew, &term); }
        D.shop_timer = 0;
        bt_core_step(&D, 8, obs, &rew, &term);
        check(D.wave == 21, "Demon endless: Go advances to wave 21");
        /* live endless combat: the Ghost Scepter must actually fire past wave 20.
         * The Demon (10 HP) dies within the window, and bt_core_step resets the
         * state on the terminal step, so latch "fired" per tick. */
        unsigned int spawned_before = D.wproj.n_spawned;
        int ticks = 0, fired = 0, ovf = 0;
        for (int t = 0; t < 900; t++) {
            bt_core_step(&D, 0, obs, &rew, &term);
            ticks++;
            if (D.wproj.n_spawned > spawned_before) fired = 1;
            if (D.pend_overflow || D.pool_overflow || D.object_uid_overflow) ovf = 1;
            if (term != 0.0f) break;
        }
        check(ticks > 0, "Demon endless wave 21 steps without crashing");
        check(fired, "Ghost Scepter fired during the endless wave");
        check(!ovf, "no capacity overflow in the Demon endless smoke");
        /* endless boss/elite group is queued on a reused wave-20 resource (wave 30) */
        bt_enter_wave(&D, 30);
        check(D.n_pend_boss > 0, "Demon endless wave 30 queues the endless boss/elite group");
    }

    printf("[14] start_wave boot into endless (deep-endless training)\n");
    {
        /* puf_init sets start_wave after bt_core_init; bt_core_reset re-reads it
         * (bt_core_reset -> bt_enter_wave(s->start_wave)), so a fresh episode
         * boots at the requested wave even in endless mode. */
        bt_core_init(&S, 77u, 0, 1, BT_CHARACTER_DEMON,
                     bt_weapon_id_from_name("weapon_ghost_scepter_1"));
        S.endless = 1;
        S.start_wave = 40;
        bt_core_reset(&S);
        check(S.wave == 40, "start_wave 40 boots the episode at wave 40");
        check(S.wpn_count == 1, "the starting weapon is granted at the boot wave");
        check(S.endless == 1, "endless survives the reset");
        check_near(bt_enemy_final_health(&S, 100.0f, 0.0f),
                   bt_roundf(100.0f * BT_DANGER_HP * bt_endless_enemy_health_mult(40)), 1e-3f,
                   "endless enemy hp scaling is live at the boot wave");
        check(S.n_pend_boss == 4, "wave-40 boot queues the endless boss/elite group (2 bosses + 2 elites)");
        S.hp = S.max_hp;
        S.time_left = 0.001f;
        bt_core_step(&S, 0, obs, &rew, &term);
        step_until_shop(&S, obs, &rew, &term, 400);
        check(S.oe_last_base == 50, "wave-40 end schedules the next decade (base 50)");
        for (int i = 0; i < 16 && S.phase == 2; i++) { S.shop_timer = 0; bt_core_step(&S, 0, obs, &rew, &term); }
        S.shop_timer = 0;
        bt_core_step(&S, 8, obs, &rew, &term);
        check(S.wave == 41, "endless continues from the boot wave to 41");
    }

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
