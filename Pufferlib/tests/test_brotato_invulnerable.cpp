/* DebugService.invulnerable regression (BtState.invulnerable).
 *
 * run_data.gd:556 sets `invulnerable = DebugService.invulnerable`, and
 * player.gd:114-115 disables the player's Hurtbox under it
 * (`if RunData.invulnerable: disable_hurtbox()`).  With the Hurtbox Area2D
 * disabled, NO enemy contact Hitbox and NO enemy projectile ever reports the
 * player, so the sim must register no player hit at the two overlap tests --
 * bt_player_contact_probe (the contact probe) and bt_proj_hits_player -- and
 * therefore run none of the hit path (no iframe/dodge draw, no hurt sound, no
 * damage, no dmg_taken, no enemy decaying-slow on contact).  Everything else --
 * enemy spawns, AI, wave RNG, projectile motion -- is untouched, so two runs
 * that differ only in the flag must be bit-identical up to the tick the
 * vulnerable one first REGISTERS a hit.
 *
 * Build and run:
 *     g++ -std=c++17 -O2 -ffp-contract=off -Wno-narrowing -I. -Iocean/brotato \
 *         tests/test_brotato_invulnerable.cpp -o /tmp/t_invulnerable -lm
 */
#include "../ocean/brotato/brotato_core.h"
#include "../ocean/brotato/brotato_capacity_host.h"
#include <cstdio>
#include <cstring>
#include <initializer_list>

static int g_checks = 0, g_failures = 0;
static void check(bool ok, const char *what) {
    g_checks++;
    if (!ok) { g_failures++; std::printf("FAIL: %s\n", what); }
}

/* BtState is multi-MiB: keep every instance in BSS, never on the stack. */
static BtState A;   /* invulnerable = 0 (the normal player hurtbox) */
static BtState B;   /* invulnerable = 1 (DebugService.invulnerable)     */
static BtState D;   /* the deep wave-40 endless invulnerable run        */
static float obsA[BT_OBS_SIZE], obsB[BT_OBS_SIZE], obsD[BT_OBS_SIZE];

/* Everything a registered player hit can later touch, plus the whole enemy /
 * projectile population: identical here means the two runs are the same world.
 * The hit registration itself (epend, hitq) is deliberately excluded -- that is
 * the one thing the flag is allowed to change. */
static bool world_equal(const BtState &a, const BtState &b) {
    if (a.rng.state != b.rng.state || a.utils_rng.state != b.utils_rng.state) return false;
    if (a.ticks != b.ticks || a.wave != b.wave || a.tick_in_wave != b.tick_in_wave ||
        a.phase != b.phase || a.kills != b.kills || a.ecount != b.ecount) return false;
    if (a.px != b.px || a.py != b.py) return false;
    /* hit effects (all written only inside bt_player_damage) */
    if (a.hp != b.hp || a.max_hp != b.max_hp || a.iframe != b.iframe ||
        a.dmg_taken != b.dmg_taken || a.materials != b.materials) return false;
    if (std::memcmp(a.ex, b.ex, sizeof(a.ex)) ||
        std::memcmp(a.ey, b.ey, sizeof(a.ey)) ||
        std::memcmp(a.ehp, b.ehp, sizeof(a.ehp)) ||
        std::memcmp(a.ealive, b.ealive, sizeof(a.ealive)) ||
        std::memcmp(a.esup, b.esup, sizeof(a.esup)) ||
        std::memcmp(a.edecay, b.edecay, sizeof(a.edecay)) ||
        std::memcmp(a.enboost, b.enboost, sizeof(a.enboost))) return false;
    if (std::memcmp(a.pralive, b.pralive, sizeof(a.pralive)) ||
        std::memcmp(a.prx, b.prx, sizeof(a.prx)) ||
        std::memcmp(a.pry, b.pry, sizeof(a.pry))) return false;
    return true;
}

/* No hit of either kind is registered on the player: no pending contact and an
 * empty enemy-projectile -> player queue. */
static bool no_pending_hit(const BtState &s) { return s.epend < 0 && s.hitq.np == 0; }

/* A deterministic movement pattern (never action 0 so the player always moves,
 * which is what walks it into the wave's enemies).  In a shop phase the same
 * ids are legal choices, so both twins stay in lock-step. */
static int move_action(int t) { return 1 + (t / 90) % 8; }

int main(void) {
    const unsigned short wpn =
        (unsigned short)bt_weapon_index_from_id("weapon_ghost_scepter_1");

    /* ---------------------------------------------------------------- [1] */
    /* Twin runs from one init: Demon + Ghost Scepter booted straight into a
     * crowded endless wave 25 (a fresh Demon there takes its first hit within a
     * few hundred ticks), so the first-hit tick is well inside the window. */
    std::printf("[1] twin runs differ ONLY in BtState.invulnerable\n");
    for (BtState *s : {&A, &B}) {
        bt_core_init(s, 1u, 0, 1, (unsigned short)BT_CHARACTER_DEMON, wpn);
        s->endless = 1;
        s->start_wave = 25;
        s->rng_ready = 0;
        bt_core_reset(s);
    }
    check(A.invulnerable == 0, "bt_core_init defaults invulnerable to 0");
    check(world_equal(A, B), "the two inits are identical before the flag is set");
    B.invulnerable = 1;

    const float b_hp0 = B.hp, b_max0 = B.max_hp;
    int first_div = -1, div_epend = 0, div_hitq = 0;
    bool div_world_clean = false, div_is_registration = false;
    bool b_never_hit = true, b_never_damaged = true, a_took_hit = false;
    int b_wave_max = B.wave;
    const int ticks = 3000;
    for (int t = 0; t < ticks; t++) {
        /* Hold position: the wave-25 crowd walks straight onto the player. */
        int a = 0;
        float rew = 0.0f, termA = 0.0f, termB = 0.0f;
        bt_core_step(&A, a, obsA, &rew, &termA);
        bt_core_step(&B, a, obsB, &rew, &termB);
        /* A is the vulnerable twin: its episode ending means a hit landed
         * lethally (bt_core_step resets on done, so dmg_taken is wiped there). */
        if (termA > 0.0f) a_took_hit = true;
        if (termB > 0.0f) b_never_hit = false;   /* B must never end either */
        /* B must never register a hit, never be damaged, never hold iframes. */
        if (!no_pending_hit(B) || B.hp != b_max0 || B.iframe != 0.0f || B.dmg_taken != 0.0f)
            b_never_hit = false;
        /* No damage: the player's current health tracks its LiveStats max
         * (p_max_hp).  The stat LAYER s->max_hp may lead it by the deferred
         * stat-sync pipeline (run_data.gd:221-222/1400-1406/1418-1427 ->
         * stats_manager.gd:20-31 -> player.gd:349-368), so `hp == max_hp` is not
         * the damage invariant; `hp == p_max_hp` is. */
        if (B.hp != B.p_max_hp) b_never_damaged = false;
        if (B.wave > b_wave_max) b_wave_max = B.wave;
        bool weq = world_equal(A, B);
        bool reg_eq = A.epend == B.epend && A.hitq.np == B.hitq.np;
        if (first_div < 0 && (!weq || !reg_eq)) {
            first_div = t;
            div_world_clean = weq;
            div_is_registration = !no_pending_hit(A) && no_pending_hit(B);
            div_epend = (int)A.epend;
            div_hitq = (int)A.hitq.np;
        }
    }
    check(b_never_hit, "invulnerable run registers no hit, holds no pending hit, no iframes");
    check(b_never_damaged, "invulnerable run takes no damage (hp == max_hp for every tick)");
    check(B.dmg_taken == 0.0f, "invulnerable run accumulates dmg_taken == 0");
    check(B.hp == b_hp0, "invulnerable run keeps its starting hp");
    check(first_div >= 0, "the vulnerable twin DOES register a first hit within the window");
    check(first_div >= 0 && div_world_clean,
          "at the first-divergence tick the two worlds are still identical "
          "(enemies/RNG/hp/projectiles): the flag changes nothing before the hit");
    check(div_is_registration,
          "the first divergence is exactly the hit registration (A pending, B clean)");
    check(a_took_hit, "the vulnerable twin's registered hit landed (its episode ended)");
    std::printf("    first divergence at tick %d (A epend=%d hitq=%d, B clean); A died=%d; "
                "B wave reached %d\n", first_div, div_epend, div_hitq, (int)a_took_hit, b_wave_max);

    /* ---------------------------------------------------------------- [2] */
    /* The deep run: start_wave 40 + endless + invulnerable must survive 20k
     * ticks in a live crowd with no implementation-capacity loss. */
    std::printf("[2] deep run: start_wave 40, endless, invulnerable, 20k ticks\n");
    bt_core_init(&D, 7u, 0, 1, (unsigned short)BT_CHARACTER_DEMON, wpn);
    D.particles_rng = 0;
    D.invulnerable = 1;
    D.endless = 1;
    D.start_wave = 40;
    /* Re-run the deterministic reset on FRESH streams (the documented deep-start
     * boot): bt_core_reset enters BtState.start_wave directly. */
    D.rng_ready = 0;
    bt_core_reset(&D);
    check(D.wave == 40, "the deep run enters wave 40 straight away");

    long max_ecount = 0, max_pr_live = 0, max_hitq = 0;
    int resets = 0;
    bool deep_clean = true;
    const int deep_ticks = 20000;
    for (int t = 0; t < deep_ticks; t++) {
        float rew = 0.0f, term = 0.0f;
        bt_core_step(&D, move_action(t), obsD, &rew, &term);
        if (D.ecount > max_ecount) max_ecount = D.ecount;
        {
            long live = 0;
            for (int i = 0; i < BT_MAX_PROJ; i++) if (D.pralive[i]) live++;
            if (live > max_pr_live) max_pr_live = live;
            if ((long)D.hitq.np > max_hitq) max_hitq = (long)D.hitq.np;
        }
        if (D.hp != D.p_max_hp || D.dmg_taken != 0.0f || D.iframe != 0.0f) deep_clean = false;
        if (term > 0.0f) { resets++; bt_core_reset(&D); }
    }
    check(deep_clean, "20k deep ticks with no damage and no iframes (hurtbox stays disabled)");
    check(D.dmg_taken == 0.0f, "the deep run accumulates dmg_taken == 0");
    check(D.done == 0, "the deep run never ended (invulnerable + endless survive)");
    check(max_ecount >= 32, "a real crowd formed (peak live enemies >= 32)");
    check(bt_capacity_mask(&D) == 0ull,
          "no implementation-capacity loss after 20k deep ticks (capacity latch clean)");
    std::printf("    wave=%d peak_enemies=%ld peak_projectiles=%ld peak_hitq=%ld resets=%d\n",
                D.wave, max_ecount, max_pr_live, max_hitq, resets);
    if (bt_capacity_mask(&D) != 0ull) {
        char buf[256];
        bt_capacity_describe(&D, buf, (int)sizeof(buf));
        std::printf("    capacity: %s\n", buf);
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
