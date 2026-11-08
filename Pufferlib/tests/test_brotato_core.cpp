/* tests/test_brotato_core.cpp
 *
 * Deterministic regression tests for the shared Brotato core
 * (ocean/brotato/brotato_core.h + brotato_state.h).  Self-contained: includes
 * only the core header and <stdio.h>.  No Env, no raylib, no engine.
 *
 * Build and run:
 *     c++ -O2 -o build/test_brotato_core tests/test_brotato_core.cpp -lm
 *     ./build/test_brotato_core
 *
 * Exit code 0 = all checks passed.
 *
 * Coverage (consumer-visible bugs fixed in this integration, NOT source
 * wiring): reset restores stale XP/HP/stats, no immediate level-up, projectile
 * damage triggers the on-hit explosion (no 1 s cooldown), enemy stat scaling on
 * (wave-1) with round(), absolute speed jitter, regen tick, wave-scaled item
 * price + reroll cost, XP curve, kill-excess capping, byte-identical
 * stepping for identical seeds, and reset-after-step restoring the fresh wave
 * clock (time_left / tick counters and the derived wave-time observation).
 */

#include "../ocean/brotato/brotato_core.h"

#include <stdio.h>
#include <string.h>

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

/* BtState is ~1 MiB (it embeds the persistent contact world); keep the big
 * states static so they never sit on the stack. */
static BtState A, B;

static int find_kind(const char* name) {
    for (int i = 0; i < BT_NKINDS; i++) {
        const char* n = BT_KIND_NAME[i];
        if (!n) continue;
        int same = 1;
        for (int j = 0; n[j] || name[j]; j++) if (n[j] != name[j]) { same = 0; break; }
        if (same) return i;
    }
    return -1;
}

static void step1(BtState* s, int action, float* obs, float* rew, float* term) {
    /* The legacy cases below inject raw projectile slots, bypassing spawn. */
    s->pr_used = BT_MAX_PROJ;
    bt_core_step(s, action, obs, rew, term);
}

static void spawn_settled_gold(BtState* s, float x, float y, int value) {
    int slot = 0;
    while (slot < BT_MAX_MAT && s->matalive[slot]) slot++;
    bt_spawn_material(s, x, y, value);
    if (slot == BT_MAX_MAT) return;
    bt_loot_item_place(s->mat_arc[slot], x, y);
    s->mat_arc[slot][BT_LOOT_ARC_PUSH] = 0.0f;
    s->mat_arc[slot][BT_LOOT_ARC_IDLE] = 0.0f;
    s->mat_arc[slot][BT_LOOT_ARC_MON] = 1.0f;
}

int main(void) {
    float obs[BT_OBS_SIZE], rew, term;

    printf("[1] constants + reset ownership\n");
    bt_core_init(&A, 1234u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
    check_near(BT_ARENA_W, 2048.0f, 0.0f, "arena width 2048");
    check_near(BT_ARENA_H, 1536.0f, 0.0f, "arena height 1536");
    check(BT_MAX == 192, "BT_MAX 192 (144 plain + 12 transient + 2 bosses + 19 elites + 6 trees = 183)");
    check(BT_PHYS_MAX_BODIES >= BT_MAX + 4, "the contact world holds BT_MAX enemies + 4 arena walls");
    check(A.wave_max_enemies == 100, "authored wave cap 100 (entity_spawner.gd:152 max_enemies)");
    check_near(A.max_hp, 30.0f, 0.0f, "player max hp 10 + Bull 20 = 30");
    check_near(A.hp, 30.0f, 0.0f, "hp starts full");
    check(A.xp == 0.0f, "xp reset to 0");
    check(A.level == 0, "level reset to 0");
    check(A.xp_need == 16, "xp_need initialised to (3+0+1)^2 = 16 (not 0)");
    check_near(A.crit, BT_BASE_CRIT, 1e-6f, "crit starts 3% (bull_explosion_stats)");
    check_near(A.crit_dmg, BT_BASE_CRIT_DMG, 1e-6f, "crit_dmg starts 1.5");
    check(A.materials == 30.0f, "materials reset");
    check(A.phase == 0, "starts in combat");

    printf("[2] no immediate level-up\n");
    step1(&A, 0, obs, &rew, &term);
    check(A.phase == 0, "fresh episode stays in combat (xp 0 < 16)");
    check(A.level == 0, "no auto level-up");
    check(obs[10] >= 0.0f && obs[10] < 1.0f, "obs xp fraction in [0,1)");

    printf("[3] reset clears stale progression/HP\n");
    A.xp = 500.0f; A.level = 7; A.materials = 999.0f; A.hp = 3.0f;
    A.stat_raw[0] = 77.0f; A.stat_raw[9] = 90.0f;   /* stale raw stat layer */
    bt_core_reset(&A);
    check_near(A.max_hp, 30.0f, 0.0f, "reset max_hp back to 30");
    check_near(A.hp, 30.0f, 0.0f, "reset hp back to full");
    check(A.xp == 0.0f && A.level == 0, "reset xp/level");
    check(A.materials == 30.0f, "reset materials");
    check_near(A.crit, BT_BASE_CRIT, 1e-6f, "reset crit (weapon base; player stat 0)");
    check(A.xp_need == 16, "reset xp_need to 16");

    printf("[5] enemy scaling on (wave-1), rounded\n");
    int chaser = find_kind("chaser");
    check(chaser >= 0, "chaser kind found");
    if (chaser >= 0) {
        bt_core_init(&A, 7u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.wave = 1;
        bt_enemy_set_alive(&A, 0, 0); A.ecount = 0;
        bt_spawn_kind(&A, chaser, 900.0f, 900.0f);
        check_near(A.emax[0], 1.0f, 0.0f, "wave 1 chaser hp = round((1+1*0)*1.4) = 1");
        A.wave = 3;
        bt_enemy_set_alive(&A, 0, 0); A.ecount = 0;
        bt_spawn_kind(&A, chaser, 900.0f, 900.0f);
        check_near(A.emax[0], 4.0f, 0.0f, "wave 3 chaser hp = round((1+1*2)*1.4) = 4");
    }

    printf("[6] absolute speed jitter\n");
    int jitter_kind = -1;
    for (int i = 0; i < BT_NKINDS && jitter_kind < 0; i++)
        if (BT_KINDS[i].speed_rand > 0) jitter_kind = i;
    check(jitter_kind >= 0, "a kind with speed_randomization exists");
    if (jitter_kind >= 0) {
        int ok = 1;
        for (int t = 0; t < 64; t++) {
            bt_core_init(&A, 100u + (bt_u64)t, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            A.wave = 1; bt_enemy_set_alive(&A, 0, 0); A.ecount = 0;
            bt_spawn_kind(&A, jitter_kind, 500.0f, 500.0f);
            float base = BT_KINDS[jitter_kind].speed;
            float r = (float)BT_KINDS[jitter_kind].speed_rand;
            float sp = A.espd[0];
            if (!(sp >= base - r - 1e-3f && sp <= base + r + 1e-3f)) ok = 0;
        }
        check(ok, "speed stays within stats.speed +/- speed_randomization (absolute)");
        check(BT_KINDS[jitter_kind].speed_rand < 1000, "jitter is absolute units, not a percent");
    }

    printf("[7] regen ticks 1 HP per period\n");
    bt_core_init(&A, 99u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
    A.n_live_groups = 0;                    /* no spawns: isolate regen */
    A.max_hp = 30.0f; A.hp = 5.0f; A.regen = 22.5f;
    A.regen_timer = bt_regen_period(A.regen);
    float hp0 = A.hp;
    for (int t = 0; t < 60; t++) step1(&A, 0, obs, &rew, &term);
    check(A.hp > hp0, "hp regenerates over a second");
    bt_core_init(&A, 99u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
    A.n_live_groups = 0;
    for (int t = 0; t < 100; t++) step1(&A, 0, obs, &rew, &term);
    A.hp = A.max_hp - 1.0f;
    step1(&A, 0, obs, &rew, &term);
    check(A.hp == A.max_hp - 1.0f, "time at full health does not advance regeneration");
    for (int t = 1; t < 29; t++) step1(&A, 0, obs, &rew, &term);
    check(A.hp == A.max_hp - 1.0f, "fractional regen is truncated before calculating its period");
    step1(&A, 0, obs, &rew, &term);
    check(A.hp == A.max_hp, "22.5 regen heals on the thirtieth tick");

    printf("[8] projectile damage triggers the on-hit explosion\n");
    if (chaser >= 0) {
        bt_core_init(&A, 5u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.wave = 1; A.iframe = 0.0f;
        int k = bt_reserve_slot(&A);
        bt_spawn_kind_at(&A, k, chaser, A.px + 100.0f, A.py, 0);
        A.ehp[k] = 5.0f; A.emax[k] = 5.0f;
        /* enemy projectile overlapping the player hurtbox; no contact (100 px) */
        A.pralive[0] = 1; A.prx[0] = A.px; A.pry[0] = A.py;
        A.prvx[0] = 0.0f; A.prvy[0] = 0.0f; A.prdmg[0] = 1.0f;
        A.prshape[0] = BT_SHAPE_CIRCLE; A.prrad[0] = BT_PROJ_R_DEFAULT;
        A.prhx[0] = 0.0f; A.prhy[0] = 0.0f; A.prrot[0] = 0.0f; A.prage[0] = 0.0f;
        float rew2 = 0.0f, term2 = 0.0f;
        bt_core_step(&A, 0, obs, &rew2, &term2);
        /* the explosion is DEFERRED into the shared pool: the step above spawned
         * it (and its node), the following window frames resolve the hits */
        for (int t = 0; t < BT_EXPL_FRAMES + 1; t++) step1(&A, 0, obs, &rew2, &term2);
        check(!A.ealive[k], "explosion (from projectile damage) killed the enemy");
        check(A.kills >= 1, "kill counted");
        check(A.expl.n_spawned >= 1, "the Bull explosion went through the shared node pool");
    }

    printf("[9] explosion kill drops a material entity (no auto-credit)\n");
    if (chaser >= 0) {
        bt_core_init(&A, 11u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        A.wave = 1;
        int k = bt_reserve_slot(&A);
        bt_spawn_kind_at(&A, k, chaser, A.px + 50.0f, A.py, 0);
        A.ehp[k] = 1.0f; A.emax[k] = 1.0f;
        /* the spawn is deferred: bt_explode only places the node in the pool */
        check(bt_explode(&A) >= 0, "the Bull explosion spawns into the shared node pool");
        check(A.expl.n_spawned >= 1, "one pooled explosion node");
        (void)bt_item_expl_tick(&A, &rew);              /* spawn frame: collision deferred */
        check(A.ealive[k], "no hit on the spawn frame");
        for (int t = 0; t < BT_EXPL_FRAMES; t++) (void)bt_item_expl_tick(&A, &rew);
        check(!A.ealive[k], "the deferred explosion killed a point-blank enemy");
        check(A.materials == 30.0f, "kill does not auto-credit materials");
        check(A.matcount >= 1, "kill dropped a material entity");
        float m0 = A.materials;
        for (int t = 0; t < 30; t++) step1(&A, 0, obs, &rew, &term);
        check(A.materials > m0 && A.xp >= 1.0f, "magnet collects the drop -> gold + 1:1 XP");
    }

    printf("[10] kill-excess caps at BT_MAX\n");
    bt_core_init(&A, 3u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
    A.wave = 1;
    for (int i = 0; i < BT_MAX + 20; i++) bt_spawn_kind(&A, (chaser >= 0 ? chaser : 0), 100.0f, 100.0f);
    check(A.ecount == BT_MAX, "enemy count capped (no silent drop / no overflow)");

    printf("[11] wave-scaled item price + reroll cost\n");
    check_near(bt_item_price(1, 20), 23.0f, 0.0f, "price wave1 base20 = 20+1+2 = 23");
    check_near(bt_item_price(10, 20), 50.0f, 0.0f, "price wave10 base20 = 50 (audit example)");
    check(bt_reroll_cost(10, 0) == (int)(10.0f * 0.75f) + 4, "reroll wave10 first = int(7.5)+4");
    check(bt_reroll_cost(10, 1) > bt_reroll_cost(10, 0), "reroll grows with count");

    printf("[12] XP curve (level+4)^2\n");
    check(bt_xp_needed(0) == 16 && bt_xp_needed(1) == 25 && bt_xp_needed(2) == 36, "xp curve 16/25/36");

    printf("[13] deterministic identical seeds\n");
    bt_core_init(&A, 4242u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
    bt_core_init(&B, 4242u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
    float oa[BT_OBS_SIZE], ob[BT_OBS_SIZE];
    int identical = 1;
    for (int t = 0; t < 300; t++) {
        int act = t % 9;
        float ra, ta, rb, tb;
        bt_core_step(&A, act, oa, &ra, &ta);
        bt_core_step(&B, act, ob, &rb, &tb);
        if (ra != rb || ta != tb || A.px != B.px || A.py != B.py || A.hp != B.hp) { identical = 0; break; }
        for (int i = 0; i < BT_OBS_SIZE; i++) if (oa[i] != ob[i]) { identical = 0; break; }
        if (!identical) break;
    }
    check(identical, "two states with the same seed step byte-identically");

    printf("[14] player motion respects the physical arena walls\n");
    bt_core_init(&A, 8u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
    A.n_live_groups = 0;                    /* no spawns: isolate wall clamp */
    for (int t = 0; t < 400; t++) step1(&A, 7, obs, &rew, &term);   /* move left */
    check(A.px >= -1.0f && A.px <= BT_ARENA_W, "player stays inside the arena");
    check(A.px < BT_ARENA_W * 0.5f, "player actually moved left");

    printf("[15] projectile shape windows + geometry (pillar/slash)\n");
    check(bt_proj_shape_active(BT_SHAPE_PILLAR, 0.50f) == 0, "pillar inert before 0.54 s");
    check(bt_proj_shape_active(BT_SHAPE_PILLAR, 0.60f) == 1, "pillar live in [0.54,0.68]");
    check(bt_proj_shape_active(BT_SHAPE_PILLAR, 0.90f) == 0, "pillar inert after 0.68 s");
    check(bt_proj_shape_active(BT_SHAPE_SLASH, 0.50f) == 1, "slash live in [0.48,0.60)");
    check(bt_proj_shape_active(BT_SHAPE_SLASH, 0.20f) == 0, "slash inert before 0.48 s");
    check(bt_proj_shape_active(BT_SHAPE_CIRCLE, 0.0f) == 1, "circle always live");
    check(bt_proj_shape_stop(BT_SHAPE_SLASH) == BT_SLASH_STOP, "slash stops at 0.63 s");
    check(bt_proj_shape_stop(BT_SHAPE_CIRCLE) < 0.0f, "circle has no animation stop");
    check(bt_proj_shape_leaves(BT_SHAPE_SLASH) == 0, "slash survives leaving the screen");
    {
        /* slash rect (constant extents) overlapping the player only inside its
         * window; the rect centre is the ANIMATED Hitbox/Collision sweep, so the
         * projectile root sits 88 px behind the swept centre at pos 0.5
         * (brotato_attacks.h BT_SLASH_SWEEPS + [59]). */
        bt_core_init(&A, 21u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        A.pralive[0] = 1; A.prx[0] = A.px + 88.0f; A.pry[0] = A.py - 3.0f;
        A.prvx[0] = 0.0f; A.prvy[0] = 0.0f; A.prdmg[0] = 1.0f;
        A.prshape[0] = BT_SHAPE_SLASH; A.prrot[0] = 0.0f;
        A.prhx[0] = 16.0f; A.prhy[0] = 9.0f; A.prrad[0] = 0.0f;
        A.prage[0] = 0.50f;
        float hp0 = A.hp;
        step1(&A, 0, obs, &rew, &term);
        check(A.hp < hp0, "slash inside its window damages the player");
        A.pralive[0] = 1; A.prx[0] = A.px + 88.0f; A.pry[0] = A.py - 3.0f;
        A.prvx[0] = 0.0f; A.prvy[0] = 0.0f; A.prage[0] = 0.20f;
        A.iframe = 0.0f;
        float hp1 = A.hp;
        step1(&A, 0, obs, &rew, &term);
        check(A.hp == hp1, "slash before its window is inert");
    }

    printf("[16] shooting spawns exactly nproj projectiles (counter)\n");
    {
        int spitter = find_kind("spitter");
        check(spitter >= 0, "spitter kind found");
        if (spitter >= 0) {
            bt_core_init(&A, 31u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            A.n_live_groups = 0;
            int k = bt_reserve_slot(&A);
            bt_spawn_kind_at(&A, k, spitter, A.px + 400.0f, A.py, 0);
            step1(&A, 0, obs, &rew, &term); /* finish the newborn's held Unit update */
            A.eatk[k] = 0.0f;
            /* Cooldown expiry starts the `shoot` animation; the volley leaves at the
             * animation's method key (shooting_attack_behavior.gd:70-81,185-188;
             * shoot.tres key 0.8 s = BT_ATTACKS[].shoot_at_f frames). */
            int fire_at = (int)BT_ATTACKS[spitter].shoot_at_f + 1;
            int early = 0;
            for (int f = 1; f < fire_at; f++) {
                step1(&A, 0, obs, &rew, &term);
                for (int i = 0; i < BT_MAX_PROJ; i++) if (A.pralive[i]) early++;
            }
            check(early == 0 && A.eshots[k] == 0, "no projectile before the shoot animation's method key");
            step1(&A, 0, obs, &rew, &term);
            int alive = 0;
            for (int i = 0; i < BT_MAX_PROJ; i++) if (A.pralive[i]) alive++;
            check(alive == (int)BT_ATTACKS[spitter].nproj, "one shot emitted nproj projectiles");
            check(A.eshots[k] == 1, "shot counter incremented");
        }
    }

    printf("[17] buffer boosts one enemy per cooldown (transitions)\n");
    {
        int chaser2 = find_kind("chaser");
        int bkind = find_kind("buffer");
        check(bkind >= 0, "a boost kind (radius > 0) exists in BT_KIND_BOOST");
        bt_core_init(&A, 41u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        if (chaser2 >= 0 && bkind >= 0) {
            int b = bt_reserve_slot(&A);
            bt_spawn_kind_at(&A, b, bkind, A.px + 500.0f, A.py, 0);
            int e1 = bt_reserve_slot(&A);
            bt_spawn_kind_at(&A, e1, chaser2, A.px + 560.0f, A.py, 0);
            int e2 = bt_reserve_slot(&A);
            bt_spawn_kind_at(&A, e2, chaser2, A.px + 620.0f, A.py, 0);
            step1(&A, 0, obs, &rew, &term); /* finish the newborns' held Unit updates */
            A.ebtimer[b] = 0.0f;
            step1(&A, 0, obs, &rew, &term);
            int boosted = (int)A.eboosted[e1] + (int)A.eboosted[e2];
            check(boosted == 1, "exactly nb_entities_boosted_at_once boosted per timeout");
            int bx = A.eboosted[e1] ? e1 : e2;
            float rh = 1.0f + BT_KIND_BOOST[bkind].hp_pct * 0.01f;
            float rd = 1.0f + BT_KIND_BOOST[bkind].dmg_pct * 0.01f;
            float rs = 1.0f + BT_KIND_BOOST[bkind].speed_pct * 0.01f;
            float w1 = (float)(A.wave - 1);
            float exp_max = bt_roundf((BT_KINDS[chaser2].hp + BT_KINDS[chaser2].hp_grow * w1) * A.danger_hp * rh);
            float exp_dmg = bt_roundf((BT_KINDS[chaser2].dmg + BT_KINDS[chaser2].dmg_grow * w1) * A.danger_dmg * rd);
            check_near(A.emax[bx], exp_max, 1e-3f, "boosted max hp recomputed from base x(1+hp_pct/100)");
            check_near(A.ehp[bx], A.emax[bx], 1e-3f, "boosted hp refilled to the new max");
            check_near(A.edmg[bx], exp_dmg, 1e-3f, "boosted damage x(1+dmg_pct/100)");
            check(A.ehp[e1] > 0.0f && rs > 0.0f, "boost ratios sane");
            check_near(A.ebtimer[b], bt_timer_frames((float)BT_KIND_BOOST[bkind].cooldown), 1e-3f,
                       "boost timer reset to cooldown (frame count)");
        }
    }

    printf("[18] spawner emits its children on death\n");
    {
        int spk = find_kind("spawner");
        bt_core_init(&A, 51u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        check(spk >= 0, "spawner kind found");
        if (spk >= 0) {
            int expect = 0;
            for (int e = 0; e < (int)BT_KIND_NDEATH_SPAWNS[spk] && e < BT_MAX_DEATH_SPAWNS; e++)
                expect += (int)BT_KIND_DEATH_SPAWN_NB[spk][e];
            check(expect == 3, "spawner death-spawn data = 3 children");
            int sp = bt_reserve_slot(&A);
            bt_spawn_kind_at(&A, sp, spk, A.px + 400.0f, A.py, 0);
            int before = A.ecount;
            A.n_pend_summons = 0;
            bt_credit_kill(&A, sp, 1);
            check(A.ecount == before - 1, "spawner death removes only the spawner body");
            check(A.n_pend_summons == expect,
                  "spawner death children are QUEUED as summons (entity_spawner queue)");
            /* 3 children drain 1/poll at frames 3/6/9, each birthing for 60 frames */
            for (int t = 0; t < 3 + 3 + 60 + 3; t++) step1(&A, 0, obs, &rew, &term);
            check(A.ecount >= expect, "queued summons activate after the birth countdown");
        }
    }

    printf("[19] materials are entities: dropped, magnetised, picked up\n");
    {
        int chaser3 = find_kind("chaser");
        bt_core_init(&A, 61u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        check(A.matcount == 0 && A.materials == 30.0f, "no materials at reset");
        if (chaser3 >= 0) {
            /* kill at point blank: gold drops as an entity, not auto-credited */
            int k = bt_reserve_slot(&A);
            bt_spawn_kind_at(&A, k, chaser3, A.px + 10.0f, A.py, 0);
            A.ehp[k] = 1.0f; A.emax[k] = 1.0f;
            check(bt_explode(&A) >= 0, "the Bull explosion spawns into the shared node pool");
            for (int t = 0; t < BT_EXPL_FRAMES + 1; t++) (void)bt_item_expl_tick(&A, &rew);
            check(A.materials == 30.0f, "kill does not auto-credit materials");
            check(A.matcount >= 1, "kill dropped a material entity");
            /* Source drops finish their push-back arc before becoming pickable. */
            for (int t = 0; t < 240 && A.matcount > 0; t++)
                step1(&A, 0, obs, &rew, &term);
            check_near(A.materials, 31.0f, 1e-6f, "material picked up -> materials");
            check_near(A.xp, 1.0f, 1e-6f, "material pickup grants 1:1 XP");

            /* a distant drop is NOT collected until the player walks over */
            bt_core_init(&A, 62u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            A.n_live_groups = 0;
            spawn_settled_gold(&A, A.px + 400.0f, A.py, 5);
            check(A.matcount == 1 && A.materials == 30.0f, "distant material waits");
            step1(&A, 0, obs, &rew, &term);
            check(A.materials == 30.0f, "distant material not auto-collected");
            check(A.matx[0] == A.px + 400.0f, "material outside attract radius does not move");
        }
    }

    printf("[20] Bull shop filter excludes banned items/groups\n");
    {
        bt_core_init(&A, 71u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        int bad = 0;
        for (int r = 0; r < 400; r++) {
            for (int i = 0; i < 4; i++) {
                if (A.shop_id[i] >= BT_NITEMS) { bad++; continue; }
                if (!bt_item_shop_ok(A.character, A.shop_id[i])) bad++;
            }
            /* force a fresh shop roll */
            A.phase = 1; A.shop_bought = 0; step1(&A, 8, obs, &rew, &term);
        }
        check(bad == 0, "no banned/lifesteal/max_nb=0 item is ever offered");
        check(BT_BULL_BANNED_ITEMS_N == 8, "Bull banned-item table present");
    }

    printf("[21] ordered effect graph (BT_ITEM_EFF_INDEX order)\n");
    {
        /* item 101 power_generator rows in graph order:
         *   1) op17 GAIN_STAT_FOR_EVERY_STAT key=percent_damage aux=speed v1 nb1
         *   2) op13 EFFECT key=percent_damage value=-5
         * speed untouched (10) -> link int(1*(10/1)) = +10, then -5 => +5. */
        bt_core_init(&A, 81u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        bt_core_stat_add(&A, 3, 10.0f);
        bt_apply_item(&A, 101);
        check_near(A.dmg_pct, 5.0f, 1e-4f, "power_generator link(+10) then EFFECT(-5) = +5");
        /* item 150 estys_couch rows: op13 max_hp +5; op17 link regen per speed
         * v2 nb=-1; op13 speed -20. LinkedStats recomputes from FINAL stats, so
         * speed=-10 -> int(2*(-10/-1)) = +20 regen (order-independent). */
        bt_core_init(&A, 82u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        bt_core_stat_add(&A, 3, 10.0f);
        bt_apply_item(&A, 150);
        check_near(A.speed_pct, -10.0f, 1e-4f, "estys_couch speed -20");
        /* The link amount (int(2*(-10/-1)) = 20) is a LinkedStats contribution;
         * get_stat_gain(hp_regeneration) is the Bull's 1.5, so it reads 30. */
        check_near(A.regen, 22.5f + 30.0f, 1e-3f,
                   "estys_couch link from final speed=-10,nb=-1 -> +20 raw, x1.5 gain");
    }

    printf("[22] burning setup comes from BT_ITEM_BURNING\n");
    {
        int burn_item = -1;
        for (int i = 0; i < BT_NITEMS && burn_item < 0; i++)
            if (BT_ITEMS[i].item_flags & BT_ITFLAG_BURN) burn_item = i;
        check(burn_item >= 0, "an item with BT_ITFLAG_BURN exists");
        if (burn_item >= 0) {
            bt_core_init(&A, 91u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            bt_apply_item(&A, (unsigned short)burn_item);
            check(A.burn_chance > 0.0f && A.burn_dmg > 0.0f && A.burn_dur > 0.0f,
                  "burn chance/damage/duration set from BurningData");
        }
    }

    printf("[23] entity_spawner queue -> entity_birth lifecycle\n");
    {
        int ch = find_kind("chaser");
        check(ch >= 0, "chaser kind found for the lifecycle test");
        bt_core_init(&A, 123u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        A.n_pend_enemy = 0; A.n_births = 0; A.cur_spawn_delay = 0; A.pend_overflow = 0;
        if (ch >= 0) {
            bt_queue_enemy(&A, ch, 200.0f, 200.0f);
            bt_queue_enemy(&A, ch, 300.0f, 300.0f);
            bt_queue_enemy(&A, ch, 400.0f, 400.0f);
            check(A.n_pend_enemy == 3, "three spawns queued");
            check(A.ecount == 0, "queued spawns are not live enemies");
            for (int t = 0; t < 3; t++) step1(&A, 0, obs, &rew, &term);
            check(A.n_pend_enemy == 2, "one entry drained on the SPAWN_DELAY=3 poll");
            check(A.n_births == 1, "the drained entry became an EntityBirth");
            check(A.ecount == 0, "a birth is not a live enemy during its countdown");
            check_near(A.births[0].x, 400.0f, 1e-3f, "pop_back drains the LAST-pushed entry (LIFO)");
            for (int t = 0; t < 59; t++) step1(&A, 0, obs, &rew, &term);
            check(A.ecount == 0, "still unborn one frame before the countdown ends");
            step1(&A, 0, obs, &rew, &term);
            check(A.ecount >= 1, "the birth activates 60 frames after it was polled");
        }
    }

    printf("[24] QUEUE_LIMIT flood rule (int(clamp((size-100)/10,1,2)))\n");
    {
        int ch = find_kind("chaser");
        bt_core_init(&A, 124u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        A.n_pend_enemy = 0; A.n_births = 0; A.cur_spawn_delay = 0;
        if (ch >= 0) {
            for (int i = 0; i < BT_QUEUE_LIMIT; i++) bt_queue_enemy(&A, ch, 100.0f, 100.0f);
            for (int t = 0; t < 3; t++) step1(&A, 0, obs, &rew, &term);
            check(A.n_pend_enemy == BT_QUEUE_LIMIT - 1,
                  "size == QUEUE_LIMIT still drains 1 (clamp((0)/10,1,2) == 1)");
            while (A.n_pend_enemy < BT_QUEUE_LIMIT + 25) bt_queue_enemy(&A, ch, 100.0f, 100.0f);
            int before = A.n_pend_enemy;
            for (int t = 0; t < 3; t++) step1(&A, 0, obs, &rew, &term);
            check(A.n_pend_enemy == before - 2,
                  "size >= QUEUE_LIMIT+20 drains 2 per poll (clamp(...) == 2)");
        }
    }

    printf("[25] queue overflow is an explicit failure, never a silent drop\n");
    {
        int ch = find_kind("chaser");
        bt_core_init(&A, 125u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_pend_enemy = 0; A.pend_overflow = 0;
        if (ch >= 0) {
            for (int i = 0; i < BT_MAX_PEND_ENEMY + 7; i++) bt_queue_enemy(&A, ch, 1.0f, 1.0f);
            check(A.n_pend_enemy == BT_MAX_PEND_ENEMY, "queue stops at the POD bound");
            check(A.pend_overflow == 1, "an over-cap enqueue flags pend_overflow");
        }
    }

    printf("[26] reset clears lifecycle state and stat ids 14..18\n");
    {
        bt_core_init(&A, 126u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_pend_enemy = 5; A.n_pend_tree = 2; A.n_births = 3; A.pend_overflow = 1;
        bt_core_stat_add(&A, 14, 4.0f); bt_core_stat_add(&A, 15, 4.0f);
        bt_core_stat_add(&A, 16, 4.0f); bt_core_stat_add(&A, 17, 4.0f);
        bt_core_stat_add(&A, 18, 4.0f);
        bt_row_variant_add(&A, 10, 3, 10);
        bt_row_variant_add(&A, 10, 3, 10);  /* count 2 */
        A.curse_overflow = 1;
        bt_core_reset(&A);
        check(A.n_pend_enemy == 0 && A.n_pend_tree == 0 && A.n_pend_boss == 0 && A.n_pend_summons == 0,
              "reset empties every spawn queue");
        check(A.n_births == 0, "reset clears pending births");
        check(A.pend_overflow == 0 && A.cur_spawn_delay == 0, "reset clears overflow + spawn delay");
        check(A.accuracy == 0.0f && A.attack_speed == 0.0f && A.engineering == 0.0f &&
              A.lifesteal == 0.0f && A.range_stat == 0.0f, "reset zeroes stat ids 14..18");
        check(A.nvar[10] == 0 && A.curse_overflow == 0, "reset clears per-row curse variants + overflow");
    }

    printf("[27] stat ids 14..18: read/add/cap + interned-key mapping\n");
    {
        bt_core_init(&A, 127u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        bt_core_stat_add(&A, 14, 7.0f);
        bt_core_stat_add(&A, 15, 8.0f);
        bt_core_stat_add(&A, 16, 9.0f);
        bt_core_stat_add(&A, 17, 10.0f);
        bt_core_stat_add(&A, 18, 11.0f);
        check_near(bt_core_stat_read(&A, 14), 7.0f, 1e-6f, "accuracy id 14");
        check_near(bt_core_stat_read(&A, 15), 8.0f, 1e-6f, "attack speed id 15");
        check_near(bt_core_stat_read(&A, 16), 9.0f, 1e-6f, "engineering id 16");
        check_near(bt_core_stat_read(&A, 17), 10.0f, 1e-6f, "lifesteal id 17");
        check_near(bt_core_stat_read(&A, 18), 11.0f, 1e-6f, "range id 18");
        check(bt_stat_cap(14, 0.0f) == BT_STAT_CAP_NONE && bt_stat_cap(15, 0.0f) == BT_STAT_CAP_NONE &&
              bt_stat_cap(16, 0.0f) == BT_STAT_CAP_NONE && bt_stat_cap(17, 0.0f) == BT_STAT_CAP_NONE &&
              bt_stat_cap(18, 0.0f) == BT_STAT_CAP_NONE, "ids 14..18 have no finite source cap");
        check(bt_core_id_from_istr(BT_ISTR_ACCURACY) == 14, "\"accuracy\" -> id 14");
        check(bt_core_id_from_istr(BT_ISTR_STAT_ATTACK_SPEED) == 15, "stat_attack_speed -> id 15");
        check(bt_core_id_from_istr(BT_ISTR_STAT_ENGINEERING) == 16, "stat_engineering -> id 16");
        check(bt_core_id_from_istr(BT_ISTR_STAT_LIFESTEAL) == 17, "stat_lifesteal -> id 17");
        check(bt_core_id_from_istr(BT_ISTR_STAT_RANGE) == 18, "stat_range -> id 18");
        {
            int ok = 1;
            for (int i = 0; i < 16; i++) if (BT_PRIMARY_STATS[i] >= 19) ok = 0;
            check(ok, "BT_PRIMARY_STATS holds only valid core stat ids (run_data.gd:30-48)");
        }
    }

    printf("[28] engineering stat link\n");
    {
        /* item 153 strange_book: op17 link stat_engineering per stat_elemental 1:1 */
        bt_core_init(&A, 128u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        bt_core_stat_add(&A, 8, 5.0f);
        bt_apply_item(&A, 153);
        check_near(A.engineering, 5.0f, 1e-4f, "strange_book link: engineering += int(1 * elemental/1)");
        check_near(A.melee, -1.0f, 1e-4f, "strange_book melee -1");
        check_near(A.ranged, -1.0f, 1e-4f, "strange_book ranged -1");
    }

    printf("[29] pet stats: attack speed ignored, range added, curse boosts damage\n");
    {
        /* item 202 bonk_dog: WSTATS 13 melee damage 10 / cooldown 28 / max_range 150.
         * Acquisition only REGISTERS the effect (entity_spawner.gd StructureTimer
         * lifecycle): the pet entity appears after the wave's first 1 s tick. */
        bt_core_init(&A, 129u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        A.curse_apply = 0;
        bt_core_stat_add(&A, 15, 500.0f);
        bt_core_stat_add(&A, 18, 100.0f);
        bt_apply_item(&A, 202);
        check(A.n_summon_fx == 1, "bonk_dog registers one pet effect");
        check(A.summon_fx[0].is_pet == 1, "bonk_dog is a stat_pets_hash entry");
        check(A.pet_n == 0, "acquisition does not create the pet entity");
        for (int t = 0; t < 123; t++) step1(&A, 0, obs, &rew, &term);
        check(A.pet_n == 1 && A.pet_alive[0], "pet entity exists after the StructureTimer tick");
        check_near(A.pet_cd[0], 28.0f / 60.0f, 1e-6f,
                   "pet cooldown ignores stat_attack_speed (weapon_service.gd:221-226)");
        check_near(A.pet_range[0], 150.0f + 100.0f * 0.5f, 1e-3f,
                   "melee pet range = max_range + stat_range/2");
        float d0 = A.pet_dmg[0];
        check(d0 >= 10.0f, "pet damage = weapon damage + scaling");
        check(A.summon_fx[0].mod == 0.0f, "uncursed acquisition carries no modifier");

        bt_core_init(&B, 129u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        B.n_live_groups = 0;
        bt_core_stat_add(&B, 15, 500.0f); bt_core_stat_add(&B, 18, 100.0f);
        B.curse_apply = 1; B.curse_maxmod = 0.0f;
        bt_apply_item(&B, 202);
        B.curse_apply = 0;
        check(B.summon_fx[0].mod > 0.0f, "cursed pet instance carries its modifier");
        for (int t = 0; t < 123; t++) step1(&B, 0, obs, &rew, &term);
        check(B.pet_n == 1, "cursed pet spawns through the same lifecycle");
        check(B.pet_dmg[0] > d0, "cursed pet weapon damage is boosted (ceil(dmg*(1+mod)))");
        check(B.curse > 0.0f, "cursed item appends the curse stat");
        check(B.curse_overflow == 0, "curse bookkeeping did not overflow for the pet item");
    }

    printf("[30] burning: curse boosts duration/damage, curse gain formula\n");
    {
        int burn_item = -1;
        for (int i = 0; i < BT_NITEMS && burn_item < 0; i++)
            if (BT_ITEMS[i].item_flags & BT_ITFLAG_BURN) burn_item = i;
        check(burn_item >= 0, "a BT_ITFLAG_BURN item exists");
        if (burn_item >= 0) {
            bt_core_init(&A, 130u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            A.curse_apply = 0;
            bt_apply_item(&A, (unsigned short)burn_item);
            float dur0 = A.burn_dur;
            check(dur0 > 0.0f, "burning duration set from BurningData");
            check(A.curse == 0.0f, "non-cursed application adds no curse");
            bt_core_init(&B, 130u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            B.curse_apply = 1; B.curse_maxmod = 0.0f;
            bt_apply_item(&B, (unsigned short)burn_item);
            float expect_curse = BT_DLC1_CURSE_CURSE_PER_ITEM_VALUE *
                                 (float)BT_ITEMS[burn_item].price * (1.0f + B.curse_maxmod);
            if (expect_curse < 1.0f) expect_curse = 1.0f;
            B.curse_apply = 0;
            check(B.burn_dur > dur0, "cursed burning duration = ceil(dur*(1+mod)) > base");
            check_near(B.curse, bt_roundf(expect_curse), 1e-3f,
                       "curse gain = round(max(1, 0.04*value*(1+maxmod))) (no stat_curse row exists)");
        }
    }

    printf("[31] curse modifier sign rules (dlc_1_data.gd:365-379)\n");
    {
        check(bt_curse_value(10, BT_SIGN_POSITIVE, 0.5f, 0, 0) == 15, "good: ceil(10*1.5)");
        check(bt_curse_value(-10, BT_SIGN_NEGATIVE, 0.5f, 0, 0) == -6, "bad: -max(1,floor(10/1.5)) = -6");
        check(bt_curse_value(10, BT_SIGN_NEUTRAL, 0.5f, 0, 0) == 10, "neutral unchanged");
        check(bt_curse_value(-20, BT_SIGN_NEGATIVE, 0.1f, 1, BT_SIGN_POSITIVE) == -22,
              "forced-POSITIVE override on a -20 row: -ceil(20*1.1) = -22");
        check(bt_curse_value(0, BT_SIGN_POSITIVE, 0.5f, 0, 0) == 0, "zero value stays zero");
    }

    printf("[32] per-copy variant storage: sums, max, merge, iteration\n");
    {
        bt_core_init(&A, 132u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        int row = -1;
        for (int r = 0; r < BT_NITEM_EFF_ROWS && row < 0; r++)
            if (BT_ITEM_EFF_ROWS[r].opcode == BT_ITEMEFF_DOUBLE_VALUE) row = r;
        check(row >= 0, "a DOUBLE_VALUE row exists");
        if (row >= 0) {
            BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[row];
            bt_row_variant_add(&A, row, 5, 10);
            bt_row_variant_add(&A, row, 7, 20);
            bt_row_variant_add(&A, row, 5, 10);   /* exact duplicate merges */
            check(A.nvar[row] == 2, "distinct tuples kept separately, duplicates merged");
            check(bt_row_owned(&A, row) == 3, "owned count = sum of variant counts");
            check_near(bt_row_sum_v(&A, row, E), 17.0f, 1e-4f, "sum aggregates every copy's value");
            {
                float vsum2 = 0.0f;
                for (int i = 0; i < A.nvar[row]; i++)
                    vsum2 += (float)bt_rvar(&A,row)[i].value2 * (float)bt_rvar(&A,row)[i].count;
                check_near(vsum2, 40.0f, 1e-4f, "value2 tuples preserved per copy (10+20+10)");
            }
            check_near(bt_row_max_v2(&A, row, E), 20.0f, 1e-4f, "max value2 across copies");
            {
                int v = 0, v2 = 0;
                check(bt_row_get(&A, row, E, 0, &v, &v2) && v == 5 && v2 == 10, "copy 0 tuple");
                check(bt_row_get(&A, row, E, 1, &v, &v2) && v == 5 && v2 == 10, "copy 1 tuple (merged)");
                check(bt_row_get(&A, row, E, 2, &v, &v2) && v == 7 && v2 == 20, "copy 2 tuple (second variant)");
                check(!bt_row_get(&A, row, E, 3, &v, &v2), "iteration stops past the last copy");
            }
            A.nvar[row] = 0;
            for (int i = 0; i <= BT_MAX_VARIANTS; i++)
                bt_row_variant_add(&A, row, i, i);
            check(A.curse_overflow == 1, "variant overflow is flagged, never silently dropped");
            check(A.nvar[row] == BT_MAX_VARIANTS &&
                  bt_rvar(&A,row)[BT_MAX_VARIANTS - 1].value == BT_MAX_VARIANTS - 1,
                  "full variant storage preserves every accepted tuple");
        }
    }

    printf("[33] wave-1 group fires at 1 s; first live enemy at tick 123\n");
    {
        bt_core_init(&A, 777u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        int c122 = 0, c123 = 0, c126 = 0, c135 = 0;
        for (int t = 1; t <= 135; t++) {
            step1(&A, 0, obs, &rew, &term);
            if (t == 122) c122 = A.ecount;
            if (t == 123) c123 = A.ecount;
            if (t == 126) c126 = A.ecount;
            if (t == 135) c135 = A.ecount;
        }
        check(c122 == 0, "no live enemy before the first birth completes (tick 122)");
        check(c123 == 1, "first enemy activates at tick 123 (fire 60 + poll 63 + 60 frames)");
        check(c126 == 2, "second birth activates 3 ticks later (one entry per poll)");
        check(c135 >= 4, "wave-1 group (min_number 4) yielded >= 4 live enemies by tick 135");
    }

    printf("[34] birth activation is deferred: newborn keeps its position a frame\n");
    {
        int ch = find_kind("chaser");
        bt_core_init(&A, 133u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        A.n_pend_enemy = 0; A.n_births = 0; A.cur_spawn_delay = 0;
        if (ch >= 0) {
            float bx = 1200.0f, by = 900.0f;
            bt_queue_enemy(&A, ch, bx, by);
            for (int t = 0; t < 62; t++) step1(&A, 0, obs, &rew, &term);
            check(A.ecount == 0, "no live enemy one frame before activation");
            step1(&A, 0, obs, &rew, &term);           /* activation frame (poll 3 + 60) */
            check(A.ecount == 1, "birth activated on its countdown frame");
            int slot = -1;
            for (int i = 0; i < BT_MAX; i++) if (A.ealive[i]) { slot = i; break; }
            check(slot >= 0, "the activated enemy occupies a slot");
            if (slot >= 0) {
                check_near(A.ex[slot], bx, 1e-3f, "newborn x unchanged (deferred spawn, no same-frame AI)");
                check_near(A.ey[slot], by, 1e-3f, "newborn y unchanged (deferred spawn, no same-frame AI)");
                step1(&A, 0, obs, &rew, &term);
                check_near(A.ex[slot], bx, 1e-3f, "fresh rigid body first advances with zero cached x velocity");
                check_near(A.ey[slot], by, 1e-3f, "fresh rigid body first advances with zero cached y velocity");
                step1(&A, 0, obs, &rew, &term);
                check(A.ex[slot] != bx || A.ey[slot] != by, "the next physics step uses the cached movement velocity");
            }
        }
    }

    printf("[35] mixed uncursed/cursed copies keep every copy's own tuple\n");
    {
        int row = -1;
        for (int r = 0; r < BT_NITEM_EFF_ROWS && row < 0; r++)
            if (BT_ITEM_EFF_ROWS[r].opcode == BT_ITEMEFF_DOUBLE_VALUE ||
                BT_ITEM_EFF_ROWS[r].opcode == BT_ITEMEFF_DOUBLE_KEY_VALUE) row = r;
        check(row >= 0, "a bucket row exists");
        if (row >= 0) {
            unsigned short item = BT_ITEM_EFF_ROWS[row].item;
            BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[row];
            /* uncursed, cursed, uncursed on one state */
            bt_core_init(&A, 134u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            A.curse_apply = 0; bt_apply_item(&A, item);
            A.curse_apply = 1; A.curse_maxmod = 0.0f; bt_apply_item(&A, item);
            A.curse_apply = 0; bt_apply_item(&A, item);
            check(bt_row_owned(&A, row) == 3, "all three copies kept (no overwrite)");
            {
                int base_copies = 0;
                for (int i = 0; i < A.nvar[row]; i++)
                    if (bt_rvar(&A,row)[i].value == E->value && bt_rvar(&A,row)[i].value2 == E->value2)
                        base_copies += bt_rvar(&A,row)[i].count;
                check(base_copies >= 2, "the two uncursed copies keep the generated tuple");
            }
            /* reverse: cursed first, then two uncursed */
            bt_core_init(&B, 134u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            B.curse_apply = 1; B.curse_maxmod = 0.0f; bt_apply_item(&B, item);
            B.curse_apply = 0; bt_apply_item(&B, item); bt_apply_item(&B, item);
            check(bt_row_owned(&B, row) == 3, "order-independent: all three copies kept");
            {
                int base_copies = 0;
                for (int i = 0; i < B.nvar[row]; i++)
                    if (bt_rvar(&B,row)[i].value == E->value && bt_rvar(&B,row)[i].value2 == E->value2)
                        base_copies += bt_rvar(&B,row)[i].count;
                check(base_copies >= 2, "reverse order also keeps the two uncursed copies");
            }
            check(A.curse_overflow == 0 && B.curse_overflow == 0,
                  "no variant overflow for these tuple counts");
        }
    }

    printf("[36] structures/pets are created by the StructureTimer, not acquisition\n");
    {
        /* entity_spawner.gd:526-567: the wave's FIRST 1 s StructureTimer tick
         * queues every owned structure, the spawner poll turns it into an
         * EntityBirth 3 frames later, and the birth activates 60 frames after
         * that (tick 60 -> 63 -> 123). */
        int turret_item = -1;
        for (int r = 0; r < BT_NITEM_EFF_ROWS && turret_item < 0; r++)
            if (BT_ITEM_EFF_ROWS[r].opcode == BT_ITEMEFF_TURRET) turret_item = BT_ITEM_EFF_ROWS[r].item;
        check(turret_item >= 0, "a TurretEffect item exists");
        if (turret_item >= 0) {
            bt_core_init(&A, 201u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            A.n_live_groups = 0;
            bt_apply_item(&A, (unsigned short)turret_item);
            check(A.n_summon_fx >= 1, "acquisition registers the structure effect");
            check(A.pet_n == 0, "no structure entity exists right after acquisition");
            int c60 = 0, c62 = 0, c122 = 0, c123 = 0;
            for (int t = 1; t <= 123; t++) {
                step1(&A, 0, obs, &rew, &term);
                if (t == 60) c60 = A.n_pend_struct;
                if (t == 62) c62 = A.n_births;
                if (t == 122) c122 = A.pet_alive[0];
                if (t == 123) c123 = A.pet_alive[0];
            }
            check(c60 == 0, "no structure queued before the 1 s StructureTimer tick");
            check(A.n_pend_struct == 0, "the queue is drained by the SPAWN_DELAY poll");
            check(c62 == 0 && A.n_births == 0, "the birth completes and is consumed");
            check(c122 == 0, "not alive one frame before the birth countdown ends");
            check(c123 == 1, "structure activates 60 frames after its birth");
            check(A.pet_isstruct[0] == 1, "the entity is a structures_hash structure");
            check_near(A.pet_body_r[0], BT_ITEM_SCENES[BT_ITEMS[turret_item].scene_geom].body_radius,
                       1e-3f, "body geometry comes from BT_ITEM_SCENES");
        }
    }

    printf("[37] landmine cadence: base tick + cur_time %% spawn_cooldown\n");
    {
        int landmine_item = -1;
        for (int r = 0; r < BT_NITEM_EFF_ROWS && landmine_item < 0; r++)
            if (BT_ITEM_EFF_ROWS[r].opcode == BT_ITEMEFF_STRUCTURE &&
                BT_ITEM_EFF_ROWS[r].spawn_cooldown > 0) landmine_item = BT_ITEM_EFF_ROWS[r].item;
        check(landmine_item >= 0, "a StructureEffect with a positive spawn_cooldown exists");
        if (landmine_item >= 0) {
            int cd = 0;
            bt_core_init(&A, 202u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            A.n_live_groups = 0;
            bt_apply_item(&A, (unsigned short)landmine_item);
            for (int i = 0; i < A.n_summon_fx; i++)
                if (!A.summon_fx[i].is_pet) cd = A.summon_fx[i].spawn_cd;
            check(cd > 0, "the registered structure keeps its spawn cooldown");
            check(A.wave_duration > (float)cd && A.wave_duration <= 60.0f,
                  "wave 1 is short enough that only one repeat fits");
            int live122 = 0, live123 = 0, live782 = 0, live783 = 0;
            for (int t = 1; t <= 800; t++) {
                step1(&A, 0, obs, &rew, &term);
                int n = 0;
                for (int i = 0; i < A.pet_n; i++) if (A.pet_alive[i] && A.pet_isstruct[i]) n++;
                if (t == 122) live122 = n;
                if (t == 123) live123 = n;
                if (t == 782) live782 = n;
                if (t == 783) live783 = n;
            }
            check(live122 == 0 && live123 == 1,
                  "the base structure appears just after the wave's 1 s tick (tick 123)");
            check(live782 == 1 && live783 == 2,
                  "a repeat spawn lands at cur_time = spawn_cooldown (783 = 720+3+60)");
            check(A.summon_overflow == 0 && A.pend_overflow == 0, "no silent queue overflow");
        }
    }

    printf("[38] curse-transformed summon fields (dlc_1_data.gd)\n");
    {
        /* bt_register_summon takes the per-effect modifier directly, so the
         * transform is checked deterministically. */
        bt_core_init(&A, 203u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        bt_register_summon(&A, 71, 0.5f);        /* landmines, spawn_cooldown 12 */
        check(A.summon_fx[0].spawn_cd == 8, "cursed landmine spawn_cooldown 12 -> 12/1.5 -> 8");
        check(A.summon_fx[0].mod == 0.0f, "the curse diverted to spawn_cooldown, not the weapon");
        bt_core_init(&A, 203u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        bt_register_summon(&A, 205, 0.5f);       /* bot_o_mine */
        check(A.summon_fx[0].mod == 0.5f, "BotOMine weapon stats carry the modifier");
        check(A.summon_fx[0].spawn_cd == 200, "BotOMine landmine cooldown 300 -> 300/1.5");
        bt_core_init(&A, 203u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        bt_register_summon(&A, 200, 0.5f);
        check_near(A.summon_fx[0].double_chance, 0.15f, 1e-5f, "Lootworm double_chance * (1+mod)");
        bt_core_init(&A, 203u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        bt_register_summon(&A, 201, 0.5f);
        check_near(A.summon_fx[0].boost_zone_scale, 1.5f, 1e-5f, "DocMoth boost_zone_scale * (1+mod)");
        bt_core_init(&A, 203u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        bt_register_summon(&A, 203, 0.5f);
        check_near(A.summon_fx[0].revive_duration, 2.0f / 1.5f, 1e-5f, "Scapegoat revive_duration / (1+mod)");
        check_near(A.summon_fx[0].health_boost, 1.5f, 1e-5f, "Scapegoat health_boost += mod");
        bt_core_init(&A, 203u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        bt_register_summon(&A, 129, 0.5f);
        check(A.summon_fx[0].mod == 0.5f && A.summon_fx[0].is_turret == 1,
              "cursed turret keeps its weapon modifier");
        bt_core_init(&A, 203u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        bt_register_summon(&A, 147, 0.5f);
        check(A.summon_fx[0].mod == 0.0f, "wandering_bot only takes the extra stat_speed");
        bt_core_init(&A, 203u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        bt_register_summon(&A, 206, 0.5f);
        check(A.n_summon_fx == 0, "a scene-less PetEffect (jellyshield) registers nothing");
        bt_core_init(&A, 203u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        for (int i = 0; i < BT_MAX_SUMMON_FX + 3; i++) bt_register_summon(&A, 129, 0.0f);
        check(A.n_summon_fx == BT_MAX_SUMMON_FX && A.summon_overflow == 1,
              "registration overflow is explicit, never a silent drop");
    }

    printf("[39] get_structure_attack_speed feeds the spawn cadence\n");
    {
        /* clockwork_wasp: structure_attack_speed +10 (SUM). improved_tools: a
         * KEY_VALUE structures_cooldown_reduction bucket that must NOT be added
         * to stat_attack_speed. */
        bt_core_init(&A, 204u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        bt_apply_item(&A, 190);
        check_near(A.structure_attack_speed, 10.0f, 1e-4f, "clockwork_wasp +10 structure_attack_speed");
        bt_core_init(&A, 204u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        bt_apply_item(&A, 164);
        check(A.n_scdr == 1 && A.scdr_key[0] == BT_ISTR_STAT_ATTACK_SPEED,
              "improved_tools fills the structures_cooldown_reduction bucket");
        check_near(A.attack_speed, 10.0f, 1e-4f,
                   "the KEY_VALUE value (50) is not added to stat_attack_speed");
        bt_core_stat_add(&A, 15, 90.0f);   /* attack_speed 10 -> 100 */
        check_near(bt_get_structure_attack_speed(&A), 50.0f, 1e-4f,
                   "sum_scaling_stat_values: 100 * (50/100)");
        A.structure_attack_speed = 10.0f;
        check_near(bt_get_structure_attack_speed(&A), 60.0f, 1e-4f, "plus the flat stat");
        check(bt_apply_structure_attack_speed(&A, 12) == 7, "12 / (1 + 0.6) truncates to 7");
        check(bt_apply_structure_attack_speed(&A, -1) == -1, "no cooldown stays -1");
    }

    printf("[40] Lootworm doubles collected gold with its double_chance\n");
    {
        int loot_row = -1;
        for (int r = 0; r < BT_NITEM_EFF_ROWS && loot_row < 0; r++)
            if (BT_ITEM_EFF_ROWS[r].opcode == BT_ITEMEFF_LOOTWORM) loot_row = r;
        check(loot_row >= 0, "a LootwormEffect row exists");
        if (loot_row >= 0) {
            bt_core_init(&A, 205u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            A.n_live_groups = 0;
            spawn_settled_gold(&A, A.px, A.py, 5);
            step1(&A, 0, obs, &rew, &term);
            check_near(A.materials, 35.0f, 1e-4f, "without a lootworm the gold is not doubled");
            bt_core_init(&A, 205u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            A.n_live_groups = 0;
            bt_spawn_summon_row(&A, (unsigned short)loot_row, 100.0f, 100.0f, 0, 0.0f, 1.0f, 0.0f);
            check(A.pet_opcode[0] == BT_ITEMEFF_LOOTWORM, "the lootworm pet is instantiated");
            /* only gold the lootworm's OWN ItemPickUpArea collects is doubled
             * (lootworm.gd:71-83); gold dropped on the player's body is taken by
             * the player's pickup pass first (it skips pet-claimed gold and runs
             * before bt_pet_gold), so the sample gold must sit in the pet's reach
             * (ItemAttractArea r 100 / ItemPickUpArea r 30 around the pet). */
            spawn_settled_gold(&A, A.pet_x[0], A.pet_y[0], 5);
            step1(&A, 0, obs, &rew, &term);
            check_near(A.materials, 40.0f, 1e-4f, "double_chance 1.0 doubles the collected gold");
        }
    }

    printf("[41] MAX_STRUCTURES eviction + per-wave summon lifecycle\n");
    {
        /* entity_spawner.gd:292-303 removes random structures while the live
         * count exceeds MAX_STRUCTURES (100). */
        bt_core_init(&A, 206u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        for (int i = 0; i < BT_MAX_STRUCTS + 1; i++) { A.pet_alive[i] = 1; A.pet_isstruct[i] = 1; }
        A.pet_n = BT_MAX_STRUCTS + 1;
        bt_enforce_structure_cap(&A);
        int n = 0;
        for (int i = 0; i < A.pet_n; i++) if (A.pet_alive[i] && A.pet_isstruct[i]) n++;
        check(n == BT_MAX_STRUCTS, "one structure evicted to satisfy MAX_STRUCTURES");
        bt_enforce_structure_cap(&A);
        n = 0;
        for (int i = 0; i < A.pet_n; i++) if (A.pet_alive[i] && A.pet_isstruct[i]) n++;
        check(n == BT_MAX_STRUCTS, "eviction is a no-op at exactly MAX_STRUCTURES");

        /* a new wave kills every structure/pet entity and restarts the
         * StructureTimer base-spawn flags (fresh main.tscn per wave). */
        bt_core_init(&A, 207u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        bt_apply_item(&A, 129);
        for (int t = 0; t < 125; t++) step1(&A, 0, obs, &rew, &term);
        check(A.pet_n == 1 && A.pet_alive[0] == 1, "turret live during the wave");
        bt_enter_wave(&A, 2);
        check(A.n_summon_fx == 1, "the owned effect survives the wave");
        check(A.pet_n == 0, "the structure entity does not survive the wave");
        check(A.base_struct_spawned == 0 && A.base_pet_spawned == 0,
              "the StructureTimer base-spawn flags restart every wave");
        check(A.n_pend_struct == 0 && A.n_pend_pet == 0, "wave start clears both queues");
        for (int t = 0; t < 125; t++) step1(&A, 0, obs, &rew, &term);
        check(A.pet_n == 1 && A.pet_alive[0] == 1, "the next wave re-spawns the turret");
    }

    printf("[42] StructureTimer base-pet spawn + turret ordering\n");
    {
        /* structures_hash order = non-turrets then turrets by strength
         * (EntityService.sort_turrets_by_strength). */
        int turret_item = -1, landmine_item = -1;
        for (int r = 0; r < BT_NITEM_EFF_ROWS; r++) {
            if (BT_ITEM_EFF_ROWS[r].opcode == BT_ITEMEFF_TURRET && turret_item < 0)
                turret_item = BT_ITEM_EFF_ROWS[r].item;
            if (BT_ITEM_EFF_ROWS[r].opcode == BT_ITEMEFF_STRUCTURE &&
                BT_ITEM_EFF_ROWS[r].spawn_cooldown > 0 && landmine_item < 0)
                landmine_item = BT_ITEM_EFF_ROWS[r].item;
        }
        if (turret_item >= 0 && landmine_item >= 0) {
            bt_core_init(&A, 208u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            bt_apply_item(&A, (unsigned short)turret_item);
            bt_apply_item(&A, (unsigned short)landmine_item);
            int k0 = bt_struct_slot(&A, 0), k1 = bt_struct_slot(&A, 1);
            check(k0 >= 0 && k1 >= 0 && k0 != k1, "both structures are iterated");
            check(A.summon_fx[k0].is_turret == 0,
                  "the non-turret (landmine) is iterated before the turret");
            check(bt_struct_slot(&A, 2) < 0, "iteration stops after the last structure");
        }
    }

    printf("[43] unmapped effect keys are preserved + item ownership tracked\n");
    {
        /* Keys with no core stat id (xp_gain 44, items_price 49, ...) keep their
         * SUM/REPLACE value in scalar_effect[]; every acquisition bumps the
         * item's owned counter. */
        int xp_item = -1;
        for (int r = 0; r < BT_NITEM_EFF_ROWS && xp_item < 0; r++)
            if (BT_ITEM_EFF_ROWS[r].opcode == BT_ITEMEFF_EFFECT &&
                BT_ITEM_EFF_ROWS[r].key == BT_ISTR_XP_GAIN &&
                bt_core_id_from_istr(BT_ITEM_EFF_ROWS[r].key) == 255)
                xp_item = BT_ITEM_EFF_ROWS[r].item;
        check(xp_item >= 0, "an xp_gain item exists (unmapped by bt_core_id_from_istr)");
        if (xp_item >= 0) {
            bt_core_init(&A, 209u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            bt_apply_item(&A, (unsigned short)xp_item);
            float v = A.scalar_effect[BT_ISTR_XP_GAIN];
            check(v > 0.0f, "the xp_gain SUM value is preserved as a scalar");
            check(A.item_owned[xp_item] == 1, "the item's owned counter is bumped");
            bt_apply_item(&A, (unsigned short)xp_item);
            check(A.item_owned[xp_item] == 2, "a second acquisition increments again");
            check_near(A.scalar_effect[BT_ISTR_XP_GAIN], v * 2.0f, 1e-3f,
                       "SUM storage accumulates across copies");
            bt_core_reset(&A);
            check(A.scalar_effect[BT_ISTR_XP_GAIN] == 0.0f && A.item_owned[xp_item] == 0,
                  "reset clears scalar effects and item ownership");
        }
    }

    printf("[44] source spawn rejection radius and minimum-radius termination\n");
    {
        /* Godot 3.7.dev1 0a9011743, seed(71), entity_spawner.gd:200-209:
         * a zero-area group at the player forces 55/115 redraws before the
         * 25px floor. These are the NEXT placement coordinates from Godot,
         * so skipped redraws or an extra floor iteration change the result. */
        const float additional[2] = { 0.0f, 300.0f };
        const float next_x[2] = { 602.822814941f, 956.936279297f };
        const float next_y[2] = { 766.242126465f, 1075.481567383f };
        BtSpawnGroup group = {};
        group.area = 0;
        for (int i = 0; i < 2; i++) {
            bt_core_init(&A, 71u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            bt_rng_seed(&A.rng, 71u);
            float x = A.px, y = A.py;
            bt_spawn_pos_away_from_player(&A, &group, additional[i], A.px, A.py, &x, &y);
            bt_rand_pos(&A, 0.0f, &x, &y);
            check_near(x, next_x[i], 1e-4f, "spawn redraws preserve the source next x");
            check_near(y, next_y[i], 1e-4f, "spawn redraws preserve the source next y");
        }
        bt_rng_seed(&A.rng, 71u);
        float x = A.px + BT_SPAWN_MIN_DIST, y = A.py;
        bt_spawn_pos_away_from_player(&A, &group, 0.0f, A.px, A.py, &x, &y);
        check_near(x, A.px + BT_SPAWN_MIN_DIST, 0.0f, "exact-radius position is accepted");
        bt_rand_pos(&A, 0.0f, &x, &y);
        check_near(x, 323.814758301f, 1e-4f, "accepted position consumes no redraw x");
        check_near(y, 1515.100830078f, 1e-4f, "accepted position consumes no redraw y");
    }

    printf("[45] original-game seed-1 elite and boss selection order\n");
    {
        /* Observed in the untouched retail bytecode via spawn_rng_probe:
         * elite hashes identify monk/butcher/mom; bosses are wizard/crab. */
        bt_core_init(&A, 1u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        const int waves[3] = { 12, 14, 18 };
        const char* elites[3] = { "monk", "butcher", "mom" };
        for (int i = 0; i < 3; i++) {
            check(A.elite_wave[i] == waves[i], "elite wave matches original-game RNG");
            check(A.elite_type[i] == 0, "elite/horde choice matches original game");
            check((int)A.elite_kind[i] == find_kind(elites[i]), "elite roster removal preserves source picks");
        }
        check((int)A.boss_kind[0] == find_kind("invoker"), "first boss matches source wizard pick");
        check((int)A.boss_kind[1] == find_kind("predator"), "second boss matches source crab pick");
    }

    printf("[46] a full projectile pool is an explicit failure, never a silent drop\n");
    {
        bt_core_init(&A, 127u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        for (int i = 0; i < BT_MAX_PROJ; i++) A.pralive[i] = 1;
        int pj = bt_spawn_projectile(&A, 10.0f, 10.0f, 0.0f, 100.0f, 1.0f, 0, 1.0f,
                                     0.0f, 0.0f, 0.0f, 0);
        check(pj == -1, "a full pool rejects the spawn");
        check(A.pr_overflow == 1, "and flags pr_overflow (loud, never silent)");
        check(A.pr_dropped == 1, "and counts the dropped spawn (pr_dropped)");
        A.pr_overflow = 1;
        bt_core_reset(&A);
        check(A.pr_overflow == 0, "reset clears pr_overflow");
        check(A.pr_dropped == 0, "reset clears pr_dropped");
    }

    printf("[47] deferred contact hit precedes the frame's explosion tick\n");
    {
        /* The engine applies the contact hit the previous step's Area2D pair
         * queued BEFORE that frame's pair resolution:
         *   Physics2DServer::flush_queries -> Area2D::_area_inout ->
         *     unit.gd:610-611 call_deferred("hurt_area_entered_deferred")
         *   SceneTree::iteration           -> MessageQueue::flush()
         *     (scene/main/scene_tree.cpp:609) runs it
         *   Physics2DServer::step          -> AreaPair2DSW::setup (the frame's
         *     own pair test; main/main.cpp:2383-2410)
         * so the Bull's explode_on_hit node (player.gd:475-480 -> bt_explode),
         * which the contact hit spawns, is live for THIS frame's
         * bt_item_expl_tick.  Applying the queued hit after the explosion tick
         * costs the fresh explosion its deferred-collision age-0 frame and
         * pushes its first (of three) overlap tests one frame later.
         *
         * The trace oracle (migration/game/rl_trace.contact161_normal.jsonl)
         * pins the relation: the contact damage is first visible on tick 278
         * (hp 30 -> 29) and both Bull-explosion kills land on tick 279
         * (5 -> 3 enemies) -- exactly ONE frame apart, never two.  A chaser
         * walked into a weaponless Bull reproduces it here (observed: contact
         * lands on frame 8, the explosion kills it on frame 9). */
        bt_core_init(&A, 1u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        int chaser = find_kind("chaser");
        check(chaser >= 0, "chaser kind found");
        for (int i = 0; i < BT_MAX; i++) bt_enemy_set_alive(&A, i, 0);
        A.ecount = 0;
        A.n_births = 0;
        int slot = 0;
        bt_spawn_kind_at(&A, slot, chaser, A.px, A.py + 60.0f, 0);
        check(A.ealive[slot] != 0, "one chaser is placed next to the player");
        A.e_hold_tick[slot] = 0xFFFFu;   /* no newborn hold */
        float prev_hp = A.hp;
        int prev_count = A.ecount;
        int contact_frame = -1, kill_frame = -1;
        for (int f = 0; f < 12; f++) {
            step1(&A, 0, obs, &rew, &term);
            if (contact_frame < 0 && A.hp < prev_hp) contact_frame = f;
            if (kill_frame < 0 && A.ecount < prev_count) kill_frame = f;
            prev_hp = A.hp;
            prev_count = A.ecount;
        }
        check(contact_frame >= 0, "the contact hit lands (player hp drops)");
        check(kill_frame >= 0, "the Bull explosion kills the adjacent enemy");
        check(kill_frame == contact_frame + 1,
              "the explosion's first overlap test lands ONE frame after the contact hit");
    }

    printf("[48] Demon: same deferred-contact -> explosion phase (Demon-first)\n");
    {
        /* The Demon + Ghost Scepter profile reaches this machine through the
         * Demon's own hurt path once it owns a player-centred ExplodingEffect
         * (krakens_eye, BT_ITEM_IDX_KRAKENS_EYE -> explode_on_hit); the Bull
         * character row is NOT needed.  With the Demon's scepter firing in the
         * same frames, the whole chain must stay: the step-F pair queues the
         * deferred hit, the hit lands at F+1, and the explosion it spawns runs
         * its FIRST overlap test at F+2 -- i.e. one frame after the damage.
         * Moving the apply back behind the frame's explosion tick pushes the
         * kill to F+3 (regression: measured 8/9 fixed vs 8/10 reverted). */
        bt_core_init(&A, 1u, 0, 1, BT_CHARACTER_DEMON, BT_WEAPON_IDX_WEAPON_GHOST_SCEPTER_1);
        bt_apply_item(&A, BT_ITEM_IDX_KRAKENS_EYE);
        int chaser = find_kind("chaser");
        check(chaser >= 0, "chaser kind found");
        for (int i = 0; i < BT_MAX; i++) bt_enemy_set_alive(&A, i, 0);
        A.ecount = 0;
        A.n_births = 0;
        int slot = 0;
        bt_spawn_kind_at(&A, slot, chaser, A.px, A.py + 60.0f, 0);
        check(A.ealive[slot] != 0, "one chaser is placed next to the Demon");
        A.e_hold_tick[slot] = 0xFFFFu;   /* no newborn hold */
        float prev_hp = A.hp;
        int prev_count = A.ecount;
        int pair_frame = -1, contact_frame = -1, kill_frame = -1;
        for (int f = 0; f < 40; f++) {
            step1(&A, 0, obs, &rew, &term);
            /* the step's own pair probe: the exact test the frame tail ran */
            if (pair_frame < 0 && A.ealive[slot] && bt_player_contact_probe(&A, slot))
                pair_frame = f;
            if (contact_frame < 0 && A.hp < prev_hp) contact_frame = f;
            if (kill_frame < 0 && A.ecount < prev_count) kill_frame = f;
            prev_hp = A.hp;
            prev_count = A.ecount;
        }
        check(pair_frame >= 0, "the Demon/enemy contact pair forms");
        check(contact_frame == pair_frame + 1,
              "the deferred hit lands one frame after the pair forms");
        check(kill_frame == contact_frame + 1,
              "the krakens_eye explosion kills the adjacent enemy one frame later");
    }

    printf("[49] over-cap plain-enemy cull (entity_spawner.gd:151-165)\n");
    {
        int prio_kind = -1, plain_kind = -1;
        for (int i = 0; i < BT_NKINDS; i++) {
            if (BT_KIND_CLASS[i] != 0) continue;
            if (BT_KIND_PRIORITY_REMOVE[i]) { if (prio_kind < 0) prio_kind = i; }
            else if (plain_kind < 0) plain_kind = i;
        }
        check(prio_kind >= 0 && plain_kind >= 0, "the roster has a priority and a plain kind");
        /* the table is the .tscn scan (Enemy.to_be_removed_in_priority) */
        check(BT_KIND_PRIORITY_REMOVE[find_kind("chaser")] == 1,
              "chaser.tscn sets to_be_removed_in_priority");
        check(BT_KIND_PRIORITY_REMOVE[find_kind("baby_alien")] == 1,
              "baby_alien.tscn sets to_be_removed_in_priority");
        check(BT_KIND_PRIORITY_REMOVE[find_kind("lobster")] == 1,
              "dlc lobster.tscn sets to_be_removed_in_priority");
        check(BT_KIND_PRIORITY_REMOVE[find_kind("crab")] == 0,
              "crab.tscn leaves to_be_removed_in_priority false");

        /* 2 plain + 1 priority, cap 2 -> exactly one cull, and because the
         * priority list is non-empty the draw MUST take that list's only entry
         * (one global draw, no Utils draw). */
        for (bt_u64 seed = 1; seed <= 4; seed++) {
            int s0 = -1, s1 = -1, sp = -1, ec0;
            bt_u64 r0, u0;
            BtRng expect;
            bt_core_init(&A, seed, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            A.wave = 3;
            A.wave_max_enemies = 2;
            A.n_live_groups = 0;
            (void)bt_spawn_kind(&A, plain_kind, 100.0f, 100.0f);
            (void)bt_spawn_kind(&A, plain_kind, 200.0f, 200.0f);
            (void)bt_spawn_kind(&A, prio_kind, 300.0f, 300.0f);
            for (int i = 0; i < BT_MAX; i++) {
                if (!A.ealive[i] || A.eneutral[i] || A.eelite[i]) continue;
                if (A.ekind[i] == (unsigned char)prio_kind) sp = i;
                else if (s0 < 0) s0 = i; else s1 = i;
            }
            check(sp >= 0 && s0 >= 0 && s1 >= 0, "the three plain enemies are live");
            ec0 = A.ecount;
            r0 = A.rng.state; u0 = A.utils_rng.state;
            expect = A.rng;
            (void)bt_rng_next(&expect);
            bt_group_perf_cull(&A);
            check(A.ecount == ec0 - 1, "exactly one plain enemy culled");
            check(!A.ealive[sp], "the priority-list enemy is the one culled");
            check(A.ealive[s0] && A.ealive[s1], "both ordinary enemies survive");
            check(A.rng.state == expect.state && A.rng.state != r0,
                  "the cull consumes exactly one GLOBAL draw");
            check(A.utils_rng.state == u0, "the cull consumes no Utils draw");
        }

        /* no priority entry: the draw indexes the whole plain list */
        bt_core_init(&A, 31u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.wave = 3;
        A.wave_max_enemies = 1;
        A.n_live_groups = 0;
        for (int i = 0; i < 4; i++) (void)bt_spawn_kind(&A, plain_kind, 100.0f + 10.0f * i, 100.0f);
        bt_group_perf_cull(&A);
        check(A.ecount == 1, "the cull leaves exactly wave_max_enemies plain enemies");

        /* bosses and neutrals live in separate lists: never culled */
        bt_core_init(&A, 41u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.wave = 12;
        A.wave_max_enemies = 1;
        A.n_live_groups = 0;
        (void)bt_spawn_kind(&A, (int)A.boss_kind[0], 10.0f, 10.0f);
        (void)bt_spawn_neutral(&A, 0, 20.0f, 20.0f);
        for (int i = 0; i < 3; i++) (void)bt_spawn_kind(&A, plain_kind, 100.0f + 10.0f * i, 100.0f);
        check(A.ecount == 5, "five live units before the cull");
        bt_group_perf_cull(&A);
        {
            int alive_boss = 0, alive_tree = 0;
            for (int i = 0; i < BT_MAX; i++) {
                if (!A.ealive[i]) continue;
                if (A.eneutral[i]) alive_tree++;
                else if (A.eelite[i]) alive_boss++;
            }
            check(A.ecount == 3, "only the 2 excess PLAIN enemies are culled");
            check(alive_boss == 1 && alive_tree == 1, "the boss and the tree survive");
        }
    }

    printf("[50] wave-20 player boss kill appends the endless transition groups (main.gd:484-491)\n");
    {
        /* exactly one boss alive + endless -> 6 groups with spawn_timing = elapsed + i */
        bt_core_init(&A, 5u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        check(A.difficulty == 5, "difficulty defaults to Danger 5");
        A.wave = BT_WAVES;
        A.wave_duration = 90.0f;
        A.time_left = 41.0f;
        A.endless = 1;
        A.all_last_wave_bosses_killed = 0;
        A.n_live_groups = 2;                       /* the transition groups go LAST */
        A.live_group[0] = 0; A.live_group[1] = 0;
        check(bt_kind_class((int)A.boss_kind[0]) == 2, "the final-wave boss is a Boss kind");
        check(bt_spawn_kind(&A, (int)A.boss_kind[0], 100.0f, 100.0f), "the final boss spawns");
        {
            int slot = -1;
            for (int i = 0; i < BT_MAX; i++) if (A.ealive[i] && A.eelite[i]) { slot = i; break; }
            check(slot >= 0, "the boss slot is live");
            A.ehp[slot] = 1.0f;
            bt_credit_kill(&A, slot, 1);           /* a player kill */
        }
        check(A.all_last_wave_bosses_killed == 1, "all_last_wave_bosses_killed is set");
        check(A.n_live_groups == 8, "int((20/10.0)*3) = 6 groups are appended");
        for (int g = 2; g < 8; g++) {
            int want = (int)(90.0f - 41.0f + BT_DT) + (g - 2);
            check(A.lg_dynamic[g] == 1, "the transition slot is a generated group");
            check((int)A.lg_timing[g] == want, "spawn_timing = int(elapsed + i)");
            check((int)A.lg_minn[g] == 2, "bonus_enemies_per_group default 1 -> min_number 2");
            check((int)A.lg_maxn[g] >= 2, "max_number >= min_number");
            check(A.group_fired[g] == 0 && A.live_mult[g] == 1, "the appended slot is not yet fired");
        }

        /* more than one boss/elite alive -> no transition */
        bt_core_init(&A, 7u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.wave = BT_WAVES; A.wave_duration = 90.0f; A.time_left = 50.0f;
        A.endless = 1; A.all_last_wave_bosses_killed = 0; A.n_live_groups = 0;
        for (int i = 0; i < 3; i++) (void)bt_spawn_kind(&A, (int)A.boss_kind[0], 10.0f * i, 10.0f);
        {
            int slot = -1;
            for (int i = 0; i < BT_MAX; i++) if (A.ealive[i] && A.eelite[i]) { slot = i; break; }
            bt_credit_kill(&A, slot, 1);
        }
        check(A.all_last_wave_bosses_killed == 0 && A.n_live_groups == 0,
              "two bosses/elites still alive -> no transition");

        /* campaign run: the wave timer restarts at 0.1 s instead */
        bt_core_init(&A, 6u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.wave = BT_WAVES; A.wave_duration = 90.0f; A.time_left = 50.0f;
        A.endless = 0; A.all_last_wave_bosses_killed = 0; A.n_live_groups = 0;
        (void)bt_spawn_kind(&A, (int)A.boss_kind[0], 10.0f, 10.0f);
        {
            int slot = -1;
            for (int i = 0; i < BT_MAX; i++) if (A.ealive[i] && A.eelite[i]) { slot = i; break; }
            bt_credit_kill(&A, slot, 1);
        }
        check(A.time_left == 0.1f, "a campaign run restarts the wave timer at 0.1 s");
        check(A.n_live_groups == 0 && A.all_last_wave_bosses_killed == 0,
              "no endless groups on a campaign run");
    }

    printf("[51] boss.gd:38-45 hp chain (difficulty 5 double_boss vs the difficulty<=0 clause)\n");
    {
        /* the factor the source picks, applied to the engine's own base hp */
        int bk5 = -1, bk0 = -1;
        for (int i = 0; i < BT_NKINDS; i++) {
            if (BT_KIND_CLASS[i] != 2) continue;
            if (BT_KIND_ZONE[i] == 1 && bk5 < 0) bk5 = i;
            if (BT_KIND_ZONE[i] == 0 && bk0 < 0) bk0 = i;
        }
        check(bk5 >= 0 && bk0 >= 0, "both zones expose a boss kind");
        for (int c = 0; c < 4; c++) {
            const int zone = c >= 2 ? 0 : 1;
            const int wave = c == 3 ? 5 : BT_WAVES;
            const float factor = c == 0 ? 0.75f : (c == 1 ? 0.9f : (c == 2 ? 1.0f : 0.5f));
            const char* what = c == 0 ? "D5 final wave -> *0.75 (double_boss)"
                             : c == 1 ? "difficulty 0 DLC final wave -> *0.9"
                             : c == 2 ? "difficulty 0 campaign final wave -> no factor"
                                      : "D5 early wave -> *0.5";
            int kind = zone == 1 ? bk5 : bk0;
            bt_core_init(&A, 13u, zone, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            if (c == 1 || c == 2) A.difficulty = 0;
            A.wave = wave;
            (void)bt_spawn_kind(&A, kind, 10.0f, 10.0f);
            {
                int slot = -1;
                for (int i = 0; i < BT_MAX; i++) if (A.ealive[i] && A.eelite[i]) { slot = i; break; }
                check(slot >= 0, "the boss spawned");
                if (slot >= 0) {
                    BT_CONST_PTR(BtUnitKind) K = &BT_KINDS[kind];
                    float base = bt_enemy_final_health(&A,
                        bt_floorf(K->hp + K->hp_grow * (float)(wave - 1)), 0.0f);
                    float want = factor == 1.0f ? base : bt_roundf(base * factor);
                    check_near(A.emax[slot], want, 0.0f, what);
                }
            }
        }
    }

    printf("[52] extra_loot_aliens_next_wave (Lure) loot-alien groups (wave_manager.gd:57-70)\n");
    {
        bt_core_init(&A, 17u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.wave = 5;
        A.n_exg_active = 0;
        A.elite_wave[0] = A.elite_wave[1] = A.elite_wave[2] = 0;
        A.elite_type[0] = A.elite_type[1] = A.elite_type[2] = 0;
        A.n_live_groups = 0;
        A.scalar_effect[BT_ISTR_EXTRA_LOOT_ALIENS_NEXT_WAVE] = 2.0f;
        {
            bt_u64 g0 = A.rng.state, u0 = A.utils_rng.state;
            int loot = 0, first = -1;
            bt_build_live_groups(&A);
            for (int i = 0; i < A.n_live_groups; i++) {
                if ((int)A.live_group[i] != (int)BT_ZONE_LOOT_GROUPS[0][0]) continue;
                loot++;
                if (first < 0) first = i;
                check(A.lg_dynamic[i] == 2, "the loot copy is a table row with an overridden timing");
                check((float)A.lg_timing[i] >= 5.0f &&
                      (float)A.lg_timing[i] <= A.wave_duration - 10.0f,
                      "spawn_timing = rand_range(5, wave_time_left - 10)");
            }
            check(loot == 2, "two copies of the zone loot group are appended (value 2)");
            check(A.scalar_effect[BT_ISTR_EXTRA_LOOT_ALIENS_NEXT_WAVE] == 0.0f,
                  "the next-wave effect is consumed");
            check(A.group_overflow == 0, "the live-group list holds them");
            /* exactly two rand_range draws (3 global words each), no Utils draw */
            B = A;
            B.rng.state = g0;
            B.utils_rng.state = u0;
            {
                float t0 = bt_rand_range(&B, 5.0f, (float)A.wave_duration - 10.0f);
                float t1 = bt_rand_range(&B, 5.0f, (float)A.wave_duration - 10.0f);
                check(B.rng.state == A.rng.state && B.utils_rng.state == A.utils_rng.state,
                      "the copies consume exactly the source's draws in order");
                if (first >= 0) {
                    check_near((float)(int)t0, (float)(int)A.lg_timing[first], 0.0f,
                               "the first drawn timing is the first loot copy's");
                    check_near((float)(int)t1, (float)(int)A.lg_timing[first + 1], 0.0f,
                               "the second drawn timing is the second loot copy's");
                }
            }
        }
        /* the duplicate fires through the timing override and spawns its alien */
        {
            unsigned short row = BT_ZONE_LOOT_GROUPS[0][0];
            for (int i = 0; i < BT_MAX_LIVE_GROUPS; i++) {
                A.group_fired[i] = 0; A.group_repeating[i] = 0;
                A.group_interval[i] = 0.0f; A.group_next_elapsed[i] = 0.0f;
                A.live_mult[i] = 1; A.lg_dynamic[i] = 0;
            }
            A.n_live_groups = 1;
            A.live_group[0] = row;
            A.lg_dynamic[0] = 2;
            A.lg_timing[0] = 12;
            A.n_pend_enemy = 0;
            bt_fire_groups(&A, 11.9f);
            check(A.n_pend_enemy == 0, "the loot group does not fire before its timing");
            bt_fire_groups(&A, 12.0f);
            check(A.n_pend_enemy == 1, "the loot group fires at its drawn timing (one looter)");
        }
    }

    printf("[55] pursuer self-boost: autostart BoostTimer, speed, hit reset\n");
    {
        /* pursuer.gd:28-29,42-52,55-58,94-99 + pursuer.tscn:76-83.
         * Scene: boost_cooldown 1.0, max_boosts 10, speed_on_boost 45,
         * damage_on_boost 0, reset_nb_times_boosted_on_hit true. */
        int pk = find_kind("pursuer");
        check(pk >= 0, "pursuer kind found");
        check(BT_KIND_SELFBOOST[pk].max_boosts == 10, "max_boosts from pursuer.tscn");
        check_near(BT_KIND_SELFBOOST[pk].speed, 45.0f, 1e-6f, "speed_on_boost 45");
        check_near(BT_KIND_SELFBOOST[pk].cooldown, 1.0f, 1e-6f, "boost_cooldown 1.0");
        check(BT_KIND_SELFBOOST[pk].reset_on_hit == 1, "reset_nb_times_boosted_on_hit true");
        bt_core_init(&A, 91u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        int p = bt_reserve_slot(&A);
        bt_spawn_kind_at(&A, p, pk, A.px + 900.0f, A.py, 0);
        A.espd[p] = 0.0f;   /* measurement-only: keep it away from the player */
        check_near(A.ebtimer[p], bt_timer_frames(BT_KIND_SELFBOOST[pk].initial_cd), 1e-6f,
                   "fresh spawn: BoostTimer autostart uses the scene wait_time's frame count");
        check((int)A.enboost[p] == 0, "nb_times_boosted starts at 0");
        float b0 = A.ebonus[p];
        step1(&A, 0, obs, &rew, &term);      /* the newborn's held Unit update */
        int first = -1;
        for (int f = 1; f <= 70; f++) {
            step1(&A, 0, obs, &rew, &term);
            if (A.enboost[p] != 0 && first < 0) {
                first = f;
                check_near(A.ebtimer[p], bt_timer_frames(BT_KIND_SELFBOOST[pk].cooldown), 1e-4f,
                           "the timer reloads to boost_cooldown's frame count");
            }
            if (f == 59) check((int)A.enboost[p] == 0, "no boost before 60 timer ticks");
        }
        check(first == 60, "the boost lands on the 60th BoostTimer tick (1.0 s)");
        check((int)A.enboost[p] == 1, "one boost");
        check_near(A.ebonus[p], b0 + BT_KIND_SELFBOOST[pk].speed, 1e-4f,
                   "bonus_speed += speed_on_boost (flat)");
        /* max_boosts cap: run another 10 s.  The boosted speed would walk the
         * pursuer into the player (contact resets the counter and the Bull's
         * explosion kills it), so pin it away every frame -- only the timer is
         * measured here. */
        for (int f = 0; f < 600; f++) {
            step1(&A, 0, obs, &rew, &term);
            A.ex[p] = A.px + 900.0f; A.ey[p] = A.py;
            A.phys.bodies[p].pos = bt_v2(A.ex[p], A.ey[p]);
        }
        check((int)A.enboost[p] == 10, "nb_times_boosted saturates at max_boosts");
        check_near(A.ebonus[p], b0 + 10.0f * BT_KIND_SELFBOOST[pk].speed, 1e-3f,
                   "exactly max_boosts speed increments");
        /* pursuer.gd:94-99 _on_hit_something (the deferred contact apply) */
        check((int)A.ealive[p] == 1, "the pinned pursuer is still alive");
        A.enboost[p] = 7; A.ebonus[p] = 315.0f;
        A.ebtimer[p] = bt_timer_frames(BT_KIND_SELFBOOST[pk].cooldown);   /* no fire this step */
        A.epend = (short)p;
        step1(&A, 0, obs, &rew, &term);
        check((int)A.enboost[p] == 0, "a contact hit zeroes nb_times_boosted");
        check(A.ebonus[p] == 0.0f, "and zeroes bonus_speed");
        /* the ZERO rows: a chaser has no BoostTimer at all */
        int ck = find_kind("chaser");
        check(BT_KIND_SELFBOOST[ck].max_boosts == 0, "a non-Pursuer kind has no BoostTimer row");
    }

    printf("[56] fly reaction shot: hurtbox-entry roll + deferred shoot()\n");
    {
        /* fly.gd:3,6-11 + fly.tscn:24 (proj_chance 0.5): a RangedWeapon or
         * shooting-Pet hitbox entering the Hurtbox rolls the chance on the
         * GLOBAL stream and a success call_deferred()s the behavior's shoot(),
         * which fires independently of the 1e9-frame cooldown. */
        int fk = find_kind("fly");
        check(fk >= 0, "fly kind found");
        check_near(BT_KIND_PROJ_CHANCE[fk], 0.5f, 1e-6f, "proj_chance 0.5 from fly.tscn");
        bt_core_init(&A, 101u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        int f = bt_reserve_slot(&A);
        bt_spawn_kind_at(&A, f, fk, A.px + 400.0f, A.py, 0);
        step1(&A, 0, obs, &rew, &term);
        check(A.eflypend[f] == 0, "no reaction queued yet");
        /* (a) a qualifying source rolls exactly ONE global draw */
        B = A;
        /* Roll the reference outcome ONCE from the pre-react snapshot: the old
         * form called bt_chance twice (each call consuming another draw) and
         * only passed while the stream happened to line up. */
        int rolled = bt_chance(&B, 0.5f);
        bt_enemy_fly_react(&A, f, 1);
        check(B.rng.state == A.rng.state && B.rng.inc == A.rng.inc,
              "the roll consumes exactly one draw");
        check(A.eflypend[f] == (unsigned char)rolled,
              "the queued flag is the rolled outcome (chance 0.5)");
        /* (b) a non-qualifying source draws NOTHING (chance 0 short-circuits) */
        B = A; A.eflypend[f] = 0;
        bt_enemy_fly_react(&A, f, 0);
        check(B.rng.state == A.rng.state && A.eflypend[f] == 0,
              "a melee/turret source neither draws nor queues");
        /* (c) a kind without the export never reacts */
        int ck = find_kind("chaser");
        int c = bt_reserve_slot(&A);
        bt_spawn_kind_at(&A, c, ck, A.px + 460.0f, A.py, 0);
        step1(&A, 0, obs, &rew, &term);
        B = A;
        bt_enemy_fly_react(&A, c, 1);
        check(B.rng.state == A.rng.state, "a kind with proj_chance 0 never draws");
        /* (d) the deferred boundary fires the volley regardless of the cooldown */
        for (int i = 0; i < BT_MAX_PROJ; i++) A.pralive[i] = 0;
        A.eatk[f] = 1.0e9f;             /* the fly's own cd: 1e9 FRAMES (eatk is a frame clock) */
        A.eflypend[f] = 1;
        int shots0 = (int)A.eshots[f];
        bt_enemy_fly_flush(&A);
        int alive = 0;
        for (int i = 0; i < BT_MAX_PROJ; i++) if (A.pralive[i]) alive++;
        check(alive == (int)BT_ATTACKS[fk].nproj, "the queued reaction fires nproj shots");
        check(A.eflypend[f] == 0, "the queue is consumed");
        check((int)A.eshots[f] == shots0 + 1, "the volley counts as a shot");
        check(A.prfriendly[0] == 0, "an enemy reaction shot is not a friendly shot");
    }

    printf("[57] buffer zone membership list + Array.shuffle draws\n");
    {
        /* buffer.gd:17,26-28,39,44-45,82-83: entities_in_zone is a
         * body_entered INSERTION-ordered list (ineligible members included) that
         * `shuffle()` permutes with n-1 global draws before the first eligible
         * member of the shuffled list is boosted.  The shuffle is pinned
         * Godot's Array::shuffle: `for (i = size-1; i > 0; i--) j = rand() %
         * (i+1); swap(i, j)`. */
        int bk = find_kind("buffer");
        int ck = find_kind("chaser");
        bt_core_init(&A, 111u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        int b = bt_reserve_slot(&A);
        bt_spawn_kind_at(&A, b, bk, A.px + 500.0f, A.py, 0);
        A.espd[b] = 0.0f;
        step1(&A, 0, obs, &rew, &term);
        int m[3];
        for (int i = 0; i < 3; i++) {          /* one entry per frame: insertion order */
            m[i] = bt_reserve_slot(&A);
            bt_spawn_kind_at(&A, m[i], ck, A.px + 540.0f + 40.0f * i, A.py, 0);
            A.espd[m[i]] = 0.0f;               /* frozen: the zone set is stable */
            step1(&A, 0, obs, &rew, &term);
        }
        check((int)A.zone_n[b] == 3, "three bodies are in the BoostZone list");
        check((int)A.zone_mem[b][0] == m[0] && (int)A.zone_mem[b][1] == m[1] &&
              (int)A.zone_mem[b][2] == m[2], "members are in body_entered order");
        /* the timeout shuffles the WHOLE list (n-1 draws) then boosts the first
         * eligible member; member 0 is already boosted (ineligible but still in
         * the list, buffer.gd:46). */
        A.eboosted[m[0]] = 1;
        unsigned char shuffled[3];
        for (int k = 0; k < 3; k++) shuffled[k] = A.zone_mem[b][k];
        B = A;
        for (int k = 2; k > 0; k--) {
            unsigned int j = bt_rng_next(&B.rng) % (unsigned int)(k + 1);
            unsigned char t = shuffled[k]; shuffled[k] = shuffled[(int)j]; shuffled[(int)j] = t;
        }
        int want_pick = -1;
        for (int k = 0; k < 3; k++) {
            int j = (int)shuffled[k];
            if (j >= 0 && B.ealive[j] && !B.eneutral[j] && !B.eboosted[j]) { want_pick = j; break; }
        }
        check(want_pick >= 0, "the shuffled list has an eligible member");
        A.ebtimer[b] = 0.0f;
        bt_buffer_boost(&A, b, bk);            /* the BoostTimer timeout body */
        check(A.rng.state == B.rng.state && A.rng.inc == B.rng.inc,
              "the timeout consumes exactly n-1 draws (Array.shuffle)");
        check(want_pick >= 0 && A.eboosted[want_pick] == 1,
              "the boosted member is the first eligible of the shuffled list");
        check(A.eboosted[m[0]] == 1 && (int)A.zone_n[b] == 3,
              "an ineligible member stays in the list and is never re-boosted");
        /* body_exited erases the member (buffer.gd:82-83) IN PLACE, so the
         * relative order of the survivors is untouched (the boost above already
         * shuffled the live list, exactly like Array::shuffle permutes
         * entities_in_zone in the engine).  The body -- not just ex/ey -- must
         * move: bt_core_step refreshes ex/ey from the physics world. */
        int n_before = (int)A.zone_n[b];
        unsigned char order_before[BT_MAX];
        for (int k = 0; k < n_before; k++) order_before[k] = A.zone_mem[b][k];
        A.ex[m[1]] = A.ex[b] + 5000.0f;
        A.ey[m[1]] = A.ey[b];
        A.phys.bodies[m[1]].pos = bt_v2(A.ex[m[1]], A.ey[m[1]]);
        A.ebtimer[b] = bt_timer_frames((float)BT_KIND_BOOST[bk].cooldown);   /* no shuffle here */
        step1(&A, 0, obs, &rew, &term);
        check((int)A.zone_n[b] == n_before - 1, "body_exited erases the member");
        {
            int k2 = 0, ok = 1;
            for (int k = 0; k < n_before; k++) {
                if ((int)order_before[k] == m[1]) continue;
                if ((int)A.zone_mem[b][k2] != (int)order_before[k]) ok = 0;
                k2++;
            }
            check(ok && k2 == (int)A.zone_n[b],
                  "the survivors keep their (shuffled) relative order");
        }
        /* a dead member leaves the list too (the engine removes the body) */
        A.ealive[m[0]] = 0;
        A.ebtimer[b] = bt_timer_frames((float)BT_KIND_BOOST[bk].cooldown);
        step1(&A, 0, obs, &rew, &term);
        check((int)A.zone_n[b] == 1 && (int)A.zone_mem[b][0] == m[2],
              "a dead member is erased");
    }

    printf("[58] projectile despawn box is the camera rect, not the arena box\n");
    {
        /* projectile.gd:43-44: destroy_on_leaving_screen tests
         * ZoneService.current_zone_max_camera_rect.has_point(position), and
         * main.gd:198-201 + my_camera.gd:229-241 make that Rect2(-96,-192,2240,1824)
         * for zone_1 (32x24 tiles).  Rect2.has_point is half-open. */
        check(bt_cam_rect_has_point(-96.0f, -192.0f) == 1, "the top-left corner is inside");
        check(bt_cam_rect_has_point(-96.5f, 0.0f) == 0, "x below x0 is outside");
        check(bt_cam_rect_has_point(2143.0f, 0.0f) == 1, "x just below x0+2240 is inside");
        check(bt_cam_rect_has_point(2144.0f, 0.0f) == 0, "x == x0+2240 is outside (half-open)");
        check(bt_cam_rect_has_point(0.0f, 1631.0f) == 1, "y below y0+1824 is inside");
        check(bt_cam_rect_has_point(0.0f, 1633.0f) == 0, "y past y0+1824 is outside");
        check(bt_cam_rect_has_point(0.0f, -100.0f) == 1, "y in [-192,0) is inside (the old 0..H box refused it)");
        check(bt_cam_rect_has_point(-50.0f, 0.0f) == 1, "x in [-96,0) is inside (the old 0..W box refused it)");
        bt_core_init(&A, 121u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        int pk = find_kind("anglerfish");
        int e = bt_reserve_slot(&A);
        bt_spawn_kind_at(&A, e, pk, 400.0f, 400.0f, 0);
        step1(&A, 0, obs, &rew, &term);
        for (int i = 0; i < BT_MAX_PROJ; i++) A.pralive[i] = 0;
        A.pralive[0] = 1; A.prshape[0] = BT_SHAPE_CIRCLE; A.prrad[0] = 5.0f;
        A.prvx[0] = 0.0f; A.prvy[0] = 0.0f; A.prdmg[0] = 1.0f;
        A.prx[0] = -50.0f; A.pry[0] = 800.0f;
        step1(&A, 0, obs, &rew, &term);
        check(A.pralive[0] == 1, "a shot in [-96,0)x lies inside the camera rect and survives");
        A.prx[0] = 2150.0f;
        step1(&A, 0, obs, &rew, &term);
        check(A.pralive[0] == 0, "a shot past x0+2240 despawns");
        A.pralive[0] = 1; A.prx[0] = 800.0f; A.pry[0] = -100.0f;
        step1(&A, 0, obs, &rew, &term);
        check(A.pralive[0] == 1, "a shot in y [-192,0) survives");
        A.pry[0] = 1700.0f;
        step1(&A, 0, obs, &rew, &term);
        check(A.pralive[0] == 0, "a shot past y0+1824 despawns");
        /* a slash keeps destroy_on_leaving_screen false (slash_projectile.tscn:146) */
        A.pralive[0] = 1; A.prshape[0] = BT_SHAPE_SLASH; A.prx[0] = 9000.0f; A.pry[0] = 9000.0f;
        A.prage[0] = 0.5f;
        step1(&A, 0, obs, &rew, &term);
        check(A.pralive[0] == 1, "a slash never despawns by leaving the screen");
    }

    printf("[59] animated slash sweep: keyframes, interpolation, window\n");
    {
        /* Measured against the pinned engine by instancing
         * projectiles/slash/slash_projectile.tscn and sampling every physics
         * frame (see the generator comment in brotato_attacks.h): the
         * Hitbox/Collision position track interpolates LINEARLY between its
         * keys, the CollisionShape2D extents stay at the scene value (the
         * shape:extents track is inert), the hitbox is enabled for
         * animation position in [0.48, 0.60) and `stop` fires at 0.63.  The
         * engine's own values at the sampled positions are the reference. */
        float cx, cy, hx, hy;
        bt_slash_shape(BT_SHAPE_SLASH, 0.2f, &cx, &cy, &hx, &hy);
        check_near(cx, -112.0f, 1e-3f, "before the first key the rest position (-112,15) holds");
        check_near(cy, -15.0f + 15.0f, 1e-3f, "rest y: node -15 + collision 15");
        check_near(hx, 16.0f, 1e-6f, "constant half extents x (extents track inert)");
        check_near(hy, 9.0f, 1e-6f, "constant half extents y");
        bt_slash_shape(BT_SHAPE_SLASH, 0.483334f, &cx, &cy, &hx, &hy);
        check_near(cx, -116.332985f, 2e-3f, "engine frame value at pos 0.483334");
        bt_slash_shape(BT_SHAPE_SLASH, 0.5f, &cx, &cy, &hx, &hy);
        check_near(cx, -87.999680f, 2e-3f, "engine frame value at pos 0.5");
        check_near(cy, -15.0f + 18.0f, 1e-3f, "collision y 18 -> local 3");
        bt_slash_shape(BT_SHAPE_SLASH, 0.55f, &cx, &cy, &hx, &hy);
        check_near(cx, 17.500286f, 2e-3f, "engine frame value at pos 0.55");
        check_near(cy, -15.0f + 18.333338f, 2e-3f, "y interpolates between keys 0.54/0.57");
        bt_slash_shape(BT_SHAPE_SLASH, 0.616667f, &cx, &cy, &hx, &hy);
        check_near(cx, 126.111191f, 2e-3f, "engine frame value at pos 0.616667");
        bt_slash_shape(BT_SHAPE_SLASH, 0.633333f, &cx, &cy, &hx, &hy);
        check_near(cx, 143.0f, 1e-3f, "past the last key the last value holds");
        bt_slash_shape(BT_SHAPE_SLASH2, 0.5f, &cx, &cy, &hx, &hy);
        check_near(cx, -87.999680f + 165.0f, 2e-3f,
                   "slash_projectile_2 puts its Hitbox at x 165");
        check(bt_proj_shape_active(BT_SHAPE_SLASH, 0.47f) == 0, "inert before 0.48");
        check(bt_proj_shape_active(BT_SHAPE_SLASH, 0.48f) == 1, "enable key at 0.48 is live");
        check(bt_proj_shape_active(BT_SHAPE_SLASH, 0.5999f) == 1, "still live just before 0.60");
        check(bt_proj_shape_active(BT_SHAPE_SLASH, 0.6f) == 0, "the 0.60 disable key wins at 0.60");
        check(bt_proj_shape_active(BT_SHAPE_SLASH2, 0.5f) == 1, "slash_2 shares the window");
        check_near(bt_proj_shape_stop(BT_SHAPE_SLASH), BT_SLASH_STOP, 1e-6f, "stop at 0.63");
        /* the swept rect reaches where the projectile root is NOT: a slash whose
         * box (centre local (-88,3) at pos 0.5) covers the player damages it,
         * while one centered on the root does not. */
        bt_core_init(&A, 131u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        for (int i = 0; i < BT_MAX_PROJ; i++) A.pralive[i] = 0;
        A.pralive[0] = 1; A.prshape[0] = BT_SHAPE_SLASH; A.prrot[0] = 0.0f;
        A.prhx[0] = 16.0f; A.prhy[0] = 9.0f; A.prrad[0] = 0.0f;
        A.prvx[0] = 0.0f; A.prvy[0] = 0.0f; A.prdmg[0] = 1.0f; A.prage[0] = 0.5f;
        A.prx[0] = A.px + 88.0f; A.pry[0] = A.py - 3.0f;   /* sweep centre on the player */
        float hp0 = A.hp;
        step1(&A, 0, obs, &rew, &term);
        check(A.hp < hp0, "the swept rect damages the player at its swept centre");
        A.iframe = 0.0f;
        A.pralive[0] = 1; A.prx[0] = A.px; A.pry[0] = A.py; A.prage[0] = 0.5f;
        float hp1 = A.hp;
        step1(&A, 0, obs, &rew, &term);
        check(A.hp == hp1, "the rect is NOT centred on the projectile root any more");
        A.pralive[0] = 1; A.prx[0] = A.px + 88.0f; A.pry[0] = A.py - 3.0f; A.prage[0] = 0.2f;
        float hp2 = A.hp;
        step1(&A, 0, obs, &rew, &term);
        check(A.hp == hp2, "and it is inert outside its window");
    }

    printf("[53] capacity: the supported endless ranges are explicit and fail LOUD\n");
    {
        /* Slot side: BT_MAX holds 144 plain + 12 transient + 2 bosses + 6 trees,
         * leaving 28 concurrently live elites.  A single endless elite event
         * carries 1 + ceil((wave-20)/10) units (run_data.gd:785-788). */
        const int elite_headroom = BT_MAX - 144 - 12 - 2 - 6;
        check(elite_headroom == 28, "BT_MAX leaves 28 slots for live elites");
        check(bt_endless_extra_elites(20) == 0, "wave 20 has no extra endless elites");
        check(bt_endless_extra_elites(1000) == 98,
              "the endless extra-elite term grows without bound (wave 1000 -> 98)");
        check(1 + bt_endless_extra_elites(290) <= elite_headroom,
              "one elite event per wave is exact through wave 290");
        check(1 + bt_endless_extra_elites(300) > elite_headroom,
              "and overflows the inline slots from wave 300 (capacity layer's job)");

        /* Group side: nb = int(w*0.2) fits BT_MAX_DYNGROUPS through
         * BT_ENDLESS_WAVE_BOUND and is DROPPED LOUDLY beyond it. */
        bt_core_init(&A, 3u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_exg_active = 0;
        A.elite_wave[0] = A.elite_wave[1] = A.elite_wave[2] = 0;
        A.elite_type[0] = A.elite_type[1] = A.elite_type[2] = 0;
        A.endless = 1;
        A.wave = BT_ENDLESS_WAVE_BOUND;
        bt_build_live_groups(&A);
        {
            int dyn = 0;
            for (int i = 0; i < A.n_live_groups; i++) if (A.lg_dynamic[i]) dyn++;
            check(dyn == (int)BT_MAX_DYNGROUPS && A.group_overflow == 0,
                  "wave 200 generates every group (int(200*0.2) == BT_MAX_DYNGROUPS)");
        }
        A.group_overflow = 0;
        A.wave = 205;                              /* int(205*0.2) = 41 > BT_MAX_DYNGROUPS */
        bt_build_live_groups(&A);
        {
            int dyn = 0;
            for (int i = 0; i < A.n_live_groups; i++) if (A.lg_dynamic[i]) dyn++;
            /* BT_MAX_DYNGROUPS only reserves the generation the bound-era list
             * needs; the hard limit is the whole live list, so 41 still fits
             * wherever the wave's own/all-wave/DLC groups leave room. */
            check(dyn == 41 || A.group_overflow == 1,
                  "wave 205 takes every generated group, or flags the drop loudly");
        }
        /* Walk into the region where the generated list no longer fits the live
         * list: the flag MUST appear, and the wave is not silently truncated. */
        {
            int first_ovf = -1, ovf_dyn = 0, ovf_other = 0;
            for (int w = 201; w <= 400; w++) {
                A.wave = w;
                A.group_overflow = 0;
                bt_build_live_groups(&A);
                int dyn = 0, other = 0;
                for (int i = 0; i < A.n_live_groups; i++) {
                    if (A.lg_dynamic[i]) dyn++; else other++;
                }
                if (A.group_overflow) { first_ovf = w; ovf_dyn = dyn; ovf_other = other; break; }
            }
            check(first_ovf > 0 && first_ovf <= 300,
                  "the generated-group list is exact well past wave 200 and then flags");
            check(first_ovf > 0 && ovf_dyn + ovf_other == (int)BT_MAX_LIVE_GROUPS,
                  "the flagged wave fills the live list exactly (no silent clamp)");
            check(A.group_overflow == 1, "the flag stays set at the boundary wave");
        }
        A.group_overflow = 0;
        A.wave = 1000;                             /* int(1000*0.2) = 200 */
        bt_build_live_groups(&A);
        check(A.group_overflow == 1, "wave 1000 is flagged, never silently truncated");

        /* Slot exhaustion must evict a PLAIN enemy and flag it, never clamp a
         * boss/elite: pool_cull_fallback is the loud net. */
        bt_core_init(&A, 43u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.wave = 25;
        A.wave_max_enemies = 0;                    /* force the plain list to 0 */
        A.pool_cull_fallback = 0;
        check(A.pool_cull_fallback == 0, "the net starts clear");

        /* The two fallback causes must be DISTINCT: a plain spawn evicting is
         * the source's cull shape; a BOSS/ELITE spawn evicting is pure
         * implementation-capacity loss (elite_slot_pressure). */
        {
            int plain = -1, bk = -1;
            for (int i = 0; i < BT_NKINDS && plain < 0; i++)
                if (BT_KIND_CLASS[i] == 0 && !BT_KIND_PRIORITY_REMOVE[i]) plain = i;
            for (int i = 0; i < BT_NKINDS && bk < 0; i++)
                if (BT_KIND_CLASS[i] == 2) bk = i;
            check(plain >= 0 && bk >= 0, "a plain and a boss kind exist for the probe");
            /* plain spawn evicting -> pool_cull_fallback only */
            bt_core_init(&A, 47u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            A.wave = 3; A.n_live_groups = 0;
            for (int i = 0; i < BT_MAX; i++) (void)bt_spawn_kind(&A, plain, 30.0f, 30.0f);
            check(A.ecount == BT_MAX, "every slot is taken with plain enemies");
            check(A.pool_cull_fallback == 0 && A.elite_slot_pressure == 0, "no fallback yet");
            (void)bt_spawn_kind(&A, plain, 10.0f, 10.0f);
            check(A.pool_cull_fallback == 1 && A.elite_slot_pressure == 0,
                  "a plain spawn's eviction is the game-defined pool_cull_fallback");
            /* boss/elite spawn evicting -> elite_slot_pressure only */
            bt_core_init(&A, 53u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            A.wave = 3; A.n_live_groups = 0;
            for (int i = 0; i < BT_MAX; i++) (void)bt_spawn_kind(&A, plain, 30.0f, 30.0f);
            (void)bt_spawn_kind(&A, bk, 10.0f, 10.0f);
            check(A.elite_slot_pressure == 1 && A.pool_cull_fallback == 0,
                  "a boss/elite spawn's eviction is distinct (capacity loss, no clamp)");
        }
    }

    printf("[55] pursuer self-boost: autostart BoostTimer, speed, hit reset\n");
    {
        /* pursuer.gd:28-29,42-52,55-58,94-99 + pursuer.tscn:76-83.
         * Scene: boost_cooldown 1.0, max_boosts 10, speed_on_boost 45,
         * damage_on_boost 0, reset_nb_times_boosted_on_hit true. */
        int pk = find_kind("pursuer");
        check(pk >= 0, "pursuer kind found");
        check(BT_KIND_SELFBOOST[pk].max_boosts == 10, "max_boosts from pursuer.tscn");
        check_near(BT_KIND_SELFBOOST[pk].speed, 45.0f, 1e-6f, "speed_on_boost 45");
        check_near(BT_KIND_SELFBOOST[pk].cooldown, 1.0f, 1e-6f, "boost_cooldown 1.0");
        check(BT_KIND_SELFBOOST[pk].reset_on_hit == 1, "reset_nb_times_boosted_on_hit true");
        bt_core_init(&A, 91u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        int p = bt_reserve_slot(&A);
        bt_spawn_kind_at(&A, p, pk, A.px + 900.0f, A.py, 0);
        A.espd[p] = 0.0f;   /* measurement-only: keep it away from the player */
        check_near(A.ebtimer[p], bt_timer_frames(BT_KIND_SELFBOOST[pk].initial_cd), 1e-6f,
                   "fresh spawn: BoostTimer autostart uses the scene wait_time's frame count");
        check((int)A.enboost[p] == 0, "nb_times_boosted starts at 0");
        float b0 = A.ebonus[p];
        step1(&A, 0, obs, &rew, &term);      /* the newborn's held Unit update */
        int first = -1;
        for (int f = 1; f <= 70; f++) {
            step1(&A, 0, obs, &rew, &term);
            if (A.enboost[p] != 0 && first < 0) {
                first = f;
                check_near(A.ebtimer[p], bt_timer_frames(BT_KIND_SELFBOOST[pk].cooldown), 1e-4f,
                           "the timer reloads to boost_cooldown's frame count");
            }
            if (f == 59) check((int)A.enboost[p] == 0, "no boost before 60 timer ticks");
        }
        check(first == 60, "the boost lands on the 60th BoostTimer tick (1.0 s)");
        check((int)A.enboost[p] == 1, "one boost");
        check_near(A.ebonus[p], b0 + BT_KIND_SELFBOOST[pk].speed, 1e-4f,
                   "bonus_speed += speed_on_boost (flat)");
        /* max_boosts cap: run another 10 s.  The boosted speed would walk the
         * pursuer into the player (contact resets the counter and the Bull's
         * explosion kills it), so pin it away every frame -- only the timer is
         * measured here. */
        for (int f = 0; f < 600; f++) {
            step1(&A, 0, obs, &rew, &term);
            A.ex[p] = A.px + 900.0f; A.ey[p] = A.py;
            A.phys.bodies[p].pos = bt_v2(A.ex[p], A.ey[p]);
        }
        check((int)A.enboost[p] == 10, "nb_times_boosted saturates at max_boosts");
        check_near(A.ebonus[p], b0 + 10.0f * BT_KIND_SELFBOOST[pk].speed, 1e-3f,
                   "exactly max_boosts speed increments");
        /* pursuer.gd:94-99 _on_hit_something (the deferred contact apply) */
        check((int)A.ealive[p] == 1, "the pinned pursuer is still alive");
        A.enboost[p] = 7; A.ebonus[p] = 315.0f;
        A.ebtimer[p] = bt_timer_frames(BT_KIND_SELFBOOST[pk].cooldown);   /* no fire this step */
        A.epend = (short)p;
        step1(&A, 0, obs, &rew, &term);
        check((int)A.enboost[p] == 0, "a contact hit zeroes nb_times_boosted");
        check(A.ebonus[p] == 0.0f, "and zeroes bonus_speed");
        /* the ZERO rows: a chaser has no BoostTimer at all */
        int ck = find_kind("chaser");
        check(BT_KIND_SELFBOOST[ck].max_boosts == 0, "a non-Pursuer kind has no BoostTimer row");
    }

    printf("[56] fly reaction shot: hurtbox-entry roll + deferred shoot()\n");
    {
        /* fly.gd:3,6-11 + fly.tscn:24 (proj_chance 0.5): a RangedWeapon or
         * shooting-Pet hitbox entering the Hurtbox rolls the chance on the
         * GLOBAL stream and a success call_deferred()s the behavior's shoot(),
         * which fires independently of the 1e9-frame cooldown. */
        int fk = find_kind("fly");
        check(fk >= 0, "fly kind found");
        check_near(BT_KIND_PROJ_CHANCE[fk], 0.5f, 1e-6f, "proj_chance 0.5 from fly.tscn");
        bt_core_init(&A, 101u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        int f = bt_reserve_slot(&A);
        bt_spawn_kind_at(&A, f, fk, A.px + 400.0f, A.py, 0);
        step1(&A, 0, obs, &rew, &term);
        check(A.eflypend[f] == 0, "no reaction queued yet");
        /* (a) a qualifying source rolls exactly ONE global draw */
        B = A;
        /* Roll the reference outcome ONCE from the pre-react snapshot: the old
         * form called bt_chance twice (each call consuming another draw) and
         * only passed while the stream happened to line up. */
        int rolled = bt_chance(&B, 0.5f);
        bt_enemy_fly_react(&A, f, 1);
        check(B.rng.state == A.rng.state && B.rng.inc == A.rng.inc,
              "the roll consumes exactly one draw");
        check(A.eflypend[f] == (unsigned char)rolled,
              "the queued flag is the rolled outcome (chance 0.5)");
        /* (b) a non-qualifying source draws NOTHING (chance 0 short-circuits) */
        B = A; A.eflypend[f] = 0;
        bt_enemy_fly_react(&A, f, 0);
        check(B.rng.state == A.rng.state && A.eflypend[f] == 0,
              "a melee/turret source neither draws nor queues");
        /* (c) a kind without the export never reacts */
        int ck = find_kind("chaser");
        int c = bt_reserve_slot(&A);
        bt_spawn_kind_at(&A, c, ck, A.px + 460.0f, A.py, 0);
        step1(&A, 0, obs, &rew, &term);
        B = A;
        bt_enemy_fly_react(&A, c, 1);
        check(B.rng.state == A.rng.state, "a kind with proj_chance 0 never draws");
        /* (d) the deferred boundary fires the volley regardless of the cooldown */
        for (int i = 0; i < BT_MAX_PROJ; i++) A.pralive[i] = 0;
        A.eatk[f] = 1.0e9f;             /* the fly's own cd: 1e9 FRAMES (eatk is a frame clock) */
        A.eflypend[f] = 1;
        int shots0 = (int)A.eshots[f];
        bt_enemy_fly_flush(&A);
        int alive = 0;
        for (int i = 0; i < BT_MAX_PROJ; i++) if (A.pralive[i]) alive++;
        check(alive == (int)BT_ATTACKS[fk].nproj, "the queued reaction fires nproj shots");
        check(A.eflypend[f] == 0, "the queue is consumed");
        check((int)A.eshots[f] == shots0 + 1, "the volley counts as a shot");
        check(A.prfriendly[0] == 0, "an enemy reaction shot is not a friendly shot");
    }

    printf("[57] buffer zone membership list + Array.shuffle draws\n");
    {
        /* buffer.gd:17,26-28,39,44-45,82-83: entities_in_zone is a
         * body_entered INSERTION-ordered list (ineligible members included) that
         * `shuffle()` permutes with n-1 global draws before the first eligible
         * member of the shuffled list is boosted.  The shuffle is pinned
         * Godot's Array::shuffle: `for (i = size-1; i > 0; i--) j = rand() %
         * (i+1); swap(i, j)`. */
        int bk = find_kind("buffer");
        int ck = find_kind("chaser");
        bt_core_init(&A, 111u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        int b = bt_reserve_slot(&A);
        bt_spawn_kind_at(&A, b, bk, A.px + 500.0f, A.py, 0);
        A.espd[b] = 0.0f;
        step1(&A, 0, obs, &rew, &term);
        int m[3];
        for (int i = 0; i < 3; i++) {          /* one entry per frame: insertion order */
            m[i] = bt_reserve_slot(&A);
            bt_spawn_kind_at(&A, m[i], ck, A.px + 540.0f + 40.0f * i, A.py, 0);
            A.espd[m[i]] = 0.0f;               /* frozen: the zone set is stable */
            step1(&A, 0, obs, &rew, &term);
        }
        check((int)A.zone_n[b] == 3, "three bodies are in the BoostZone list");
        check((int)A.zone_mem[b][0] == m[0] && (int)A.zone_mem[b][1] == m[1] &&
              (int)A.zone_mem[b][2] == m[2], "members are in body_entered order");
        /* the timeout shuffles the WHOLE list (n-1 draws) then boosts the first
         * eligible member; member 0 is already boosted (ineligible but still in
         * the list, buffer.gd:46). */
        A.eboosted[m[0]] = 1;
        unsigned char shuffled[3];
        for (int k = 0; k < 3; k++) shuffled[k] = A.zone_mem[b][k];
        B = A;
        for (int k = 2; k > 0; k--) {
            unsigned int j = bt_rng_next(&B.rng) % (unsigned int)(k + 1);
            unsigned char t = shuffled[k]; shuffled[k] = shuffled[(int)j]; shuffled[(int)j] = t;
        }
        int want_pick = -1;
        for (int k = 0; k < 3; k++) {
            int j = (int)shuffled[k];
            if (j >= 0 && B.ealive[j] && !B.eneutral[j] && !B.eboosted[j]) { want_pick = j; break; }
        }
        check(want_pick >= 0, "the shuffled list has an eligible member");
        A.ebtimer[b] = 0.0f;
        bt_buffer_boost(&A, b, bk);            /* the BoostTimer timeout body */
        check(A.rng.state == B.rng.state && A.rng.inc == B.rng.inc,
              "the timeout consumes exactly n-1 draws (Array.shuffle)");
        check(want_pick >= 0 && A.eboosted[want_pick] == 1,
              "the boosted member is the first eligible of the shuffled list");
        check(A.eboosted[m[0]] == 1 && (int)A.zone_n[b] == 3,
              "an ineligible member stays in the list and is never re-boosted");
        /* body_exited erases the member (buffer.gd:82-83) IN PLACE, so the
         * relative order of the survivors is untouched (the boost above already
         * shuffled the live list, exactly like Array::shuffle permutes
         * entities_in_zone in the engine).  The body -- not just ex/ey -- must
         * move: bt_core_step refreshes ex/ey from the physics world. */
        int n_before = (int)A.zone_n[b];
        unsigned char order_before[BT_MAX];
        for (int k = 0; k < n_before; k++) order_before[k] = A.zone_mem[b][k];
        A.ex[m[1]] = A.ex[b] + 5000.0f;
        A.ey[m[1]] = A.ey[b];
        A.phys.bodies[m[1]].pos = bt_v2(A.ex[m[1]], A.ey[m[1]]);
        A.ebtimer[b] = bt_timer_frames((float)BT_KIND_BOOST[bk].cooldown);   /* no shuffle here */
        step1(&A, 0, obs, &rew, &term);
        check((int)A.zone_n[b] == n_before - 1, "body_exited erases the member");
        {
            int k2 = 0, ok = 1;
            for (int k = 0; k < n_before; k++) {
                if ((int)order_before[k] == m[1]) continue;
                if ((int)A.zone_mem[b][k2] != (int)order_before[k]) ok = 0;
                k2++;
            }
            check(ok && k2 == (int)A.zone_n[b],
                  "the survivors keep their (shuffled) relative order");
        }
        /* a dead member leaves the list too (the engine removes the body) */
        A.ealive[m[0]] = 0;
        A.ebtimer[b] = bt_timer_frames((float)BT_KIND_BOOST[bk].cooldown);
        step1(&A, 0, obs, &rew, &term);
        check((int)A.zone_n[b] == 1 && (int)A.zone_mem[b][0] == m[2],
              "a dead member is erased");
    }

    printf("[58] projectile despawn box is the camera rect, not the arena box\n");
    {
        /* projectile.gd:43-44: destroy_on_leaving_screen tests
         * ZoneService.current_zone_max_camera_rect.has_point(position), and
         * main.gd:198-201 + my_camera.gd:229-241 make that Rect2(-96,-192,2240,1824)
         * for zone_1 (32x24 tiles).  Rect2.has_point is half-open. */
        check(bt_cam_rect_has_point(-96.0f, -192.0f) == 1, "the top-left corner is inside");
        check(bt_cam_rect_has_point(-96.5f, 0.0f) == 0, "x below x0 is outside");
        check(bt_cam_rect_has_point(2143.0f, 0.0f) == 1, "x just below x0+2240 is inside");
        check(bt_cam_rect_has_point(2144.0f, 0.0f) == 0, "x == x0+2240 is outside (half-open)");
        check(bt_cam_rect_has_point(0.0f, 1631.0f) == 1, "y below y0+1824 is inside");
        check(bt_cam_rect_has_point(0.0f, 1633.0f) == 0, "y past y0+1824 is outside");
        check(bt_cam_rect_has_point(0.0f, -100.0f) == 1, "y in [-192,0) is inside (the old 0..H box refused it)");
        check(bt_cam_rect_has_point(-50.0f, 0.0f) == 1, "x in [-96,0) is inside (the old 0..W box refused it)");
        bt_core_init(&A, 121u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        int pk = find_kind("anglerfish");
        int e = bt_reserve_slot(&A);
        bt_spawn_kind_at(&A, e, pk, 400.0f, 400.0f, 0);
        step1(&A, 0, obs, &rew, &term);
        for (int i = 0; i < BT_MAX_PROJ; i++) A.pralive[i] = 0;
        A.pralive[0] = 1; A.prshape[0] = BT_SHAPE_CIRCLE; A.prrad[0] = 5.0f;
        A.prvx[0] = 0.0f; A.prvy[0] = 0.0f; A.prdmg[0] = 1.0f;
        A.prx[0] = -50.0f; A.pry[0] = 800.0f;
        step1(&A, 0, obs, &rew, &term);
        check(A.pralive[0] == 1, "a shot in [-96,0)x lies inside the camera rect and survives");
        A.prx[0] = 2150.0f;
        step1(&A, 0, obs, &rew, &term);
        check(A.pralive[0] == 0, "a shot past x0+2240 despawns");
        A.pralive[0] = 1; A.prx[0] = 800.0f; A.pry[0] = -100.0f;
        step1(&A, 0, obs, &rew, &term);
        check(A.pralive[0] == 1, "a shot in y [-192,0) survives");
        A.pry[0] = 1700.0f;
        step1(&A, 0, obs, &rew, &term);
        check(A.pralive[0] == 0, "a shot past y0+1824 despawns");
        /* a slash keeps destroy_on_leaving_screen false (slash_projectile.tscn:146) */
        A.pralive[0] = 1; A.prshape[0] = BT_SHAPE_SLASH; A.prx[0] = 9000.0f; A.pry[0] = 9000.0f;
        A.prage[0] = 0.5f;
        step1(&A, 0, obs, &rew, &term);
        check(A.pralive[0] == 1, "a slash never despawns by leaving the screen");
    }

    printf("[59] animated slash sweep: keyframes, interpolation, window\n");
    {
        /* Measured against the pinned engine by instancing
         * projectiles/slash/slash_projectile.tscn and sampling every physics
         * frame (see the generator comment in brotato_attacks.h): the
         * Hitbox/Collision position track interpolates LINEARLY between its
         * keys, the CollisionShape2D extents stay at the scene value (the
         * shape:extents track is inert), the hitbox is enabled for
         * animation position in [0.48, 0.60) and `stop` fires at 0.63.  The
         * engine's own values at the sampled positions are the reference. */
        float cx, cy, hx, hy;
        bt_slash_shape(BT_SHAPE_SLASH, 0.2f, &cx, &cy, &hx, &hy);
        check_near(cx, -112.0f, 1e-3f, "before the first key the rest position (-112,15) holds");
        check_near(cy, -15.0f + 15.0f, 1e-3f, "rest y: node -15 + collision 15");
        check_near(hx, 16.0f, 1e-6f, "constant half extents x (extents track inert)");
        check_near(hy, 9.0f, 1e-6f, "constant half extents y");
        bt_slash_shape(BT_SHAPE_SLASH, 0.483334f, &cx, &cy, &hx, &hy);
        check_near(cx, -116.332985f, 2e-3f, "engine frame value at pos 0.483334");
        bt_slash_shape(BT_SHAPE_SLASH, 0.5f, &cx, &cy, &hx, &hy);
        check_near(cx, -87.999680f, 2e-3f, "engine frame value at pos 0.5");
        check_near(cy, -15.0f + 18.0f, 1e-3f, "collision y 18 -> local 3");
        bt_slash_shape(BT_SHAPE_SLASH, 0.55f, &cx, &cy, &hx, &hy);
        check_near(cx, 17.500286f, 2e-3f, "engine frame value at pos 0.55");
        check_near(cy, -15.0f + 18.333338f, 2e-3f, "y interpolates between keys 0.54/0.57");
        bt_slash_shape(BT_SHAPE_SLASH, 0.616667f, &cx, &cy, &hx, &hy);
        check_near(cx, 126.111191f, 2e-3f, "engine frame value at pos 0.616667");
        bt_slash_shape(BT_SHAPE_SLASH, 0.633333f, &cx, &cy, &hx, &hy);
        check_near(cx, 143.0f, 1e-3f, "past the last key the last value holds");
        bt_slash_shape(BT_SHAPE_SLASH2, 0.5f, &cx, &cy, &hx, &hy);
        check_near(cx, -87.999680f + 165.0f, 2e-3f,
                   "slash_projectile_2 puts its Hitbox at x 165");
        check(bt_proj_shape_active(BT_SHAPE_SLASH, 0.47f) == 0, "inert before 0.48");
        check(bt_proj_shape_active(BT_SHAPE_SLASH, 0.48f) == 1, "enable key at 0.48 is live");
        check(bt_proj_shape_active(BT_SHAPE_SLASH, 0.5999f) == 1, "still live just before 0.60");
        check(bt_proj_shape_active(BT_SHAPE_SLASH, 0.6f) == 0, "the 0.60 disable key wins at 0.60");
        check(bt_proj_shape_active(BT_SHAPE_SLASH2, 0.5f) == 1, "slash_2 shares the window");
        check_near(bt_proj_shape_stop(BT_SHAPE_SLASH), BT_SLASH_STOP, 1e-6f, "stop at 0.63");
        /* the swept rect reaches where the projectile root is NOT: a slash whose
         * box (centre local (-88,3) at pos 0.5) covers the player damages it,
         * while one centered on the root does not. */
        bt_core_init(&A, 131u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        for (int i = 0; i < BT_MAX_PROJ; i++) A.pralive[i] = 0;
        A.pralive[0] = 1; A.prshape[0] = BT_SHAPE_SLASH; A.prrot[0] = 0.0f;
        A.prhx[0] = 16.0f; A.prhy[0] = 9.0f; A.prrad[0] = 0.0f;
        A.prvx[0] = 0.0f; A.prvy[0] = 0.0f; A.prdmg[0] = 1.0f; A.prage[0] = 0.5f;
        A.prx[0] = A.px + 88.0f; A.pry[0] = A.py - 3.0f;   /* sweep centre on the player */
        float hp0 = A.hp;
        step1(&A, 0, obs, &rew, &term);
        check(A.hp < hp0, "the swept rect damages the player at its swept centre");
        A.iframe = 0.0f;
        A.pralive[0] = 1; A.prx[0] = A.px; A.pry[0] = A.py; A.prage[0] = 0.5f;
        float hp1 = A.hp;
        step1(&A, 0, obs, &rew, &term);
        check(A.hp == hp1, "the rect is NOT centred on the projectile root any more");
        A.pralive[0] = 1; A.prx[0] = A.px + 88.0f; A.pry[0] = A.py - 3.0f; A.prage[0] = 0.2f;
        float hp2 = A.hp;
        step1(&A, 0, obs, &rew, &term);
        check(A.hp == hp2, "and it is inert outside its window");
    }

    printf("[60] pursuer size_on_boost scales the live per-slot geometry\n");
    {
        /* pursuer.gd:59,75-83: every boost adds 0.25*size_on_boost to the
         * Collision/Hurtbox/Hitbox shape scales and NOTHING ever shrinks them
         * again (reset_size runs only from the DLC anglerfish's
         * reset_attack_data), while the hit reset zeroes nb_times_boosted.  The
         * campaign pursuer sets size_on_boost 0 (pursuer.tscn:80); the DLC
         * viperfish sets 0.08 (viperfish.tscn:30) and IS reachable: zone 1
         * (abyss) wave groups spawn kind 22 (BT_GROUPS). */
        int vk = find_kind("viperfish");
        int pk = find_kind("pursuer");
        check(vk >= 0 && pk >= 0, "viperfish and pursuer kinds found");
        check_near(BT_KIND_SELFBOOST[vk].size, 0.08f, 1e-6f, "viperfish size_on_boost 0.08");
        check(BT_KIND_SELFBOOST[vk].change_after == 3, "viperfish change_after_x 3");
        check_near(BT_KIND_SELFBOOST[vk].bonus_on_change, 200.0f, 1e-6f, "viperfish bonus 200");
        check(BT_KIND_SELFBOOST[pk].size == 0.0f, "campaign pursuer size_on_boost 0");
        check(BT_KIND_SELFBOOST[pk].change_after == -1, "campaign pursuer change_after -1");
        BT_CONST_PTR(BtBodyPhysics) K = &BT_PHYSICS[bt_geom_index(vk)];
        bt_core_init(&A, 141u, 1, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        int v = bt_reserve_slot(&A);
        bt_spawn_kind_at(&A, v, vk, A.px + 900.0f, A.py, 0);
        A.espd[v] = 0.0f;
        step1(&A, 0, obs, &rew, &term);
        check_near(A.egeom[v].body_radius, K->body_radius, 1e-4f, "no boost: the kind row is exact");
        check_near(A.egeom[v].hurt_radius, K->hurt_radius, 1e-4f, "no boost: hurtbox radius");
        /* three boosts (max_boosts 3): scale 1 + 0.25*0.08*n */
        A.enboost[v] = 0; A.ebonus[v] = 0.0f;
        for (int n = 1; n <= 3; n++) {
            bt_enemy_self_boost(&A, v, vk);
            float sc = 1.0f + 0.25f * 0.08f * (float)n;
            check_near(A.egeom[v].body_radius, K->body_radius * sc, 1e-3f, "body radius x(1+0.02n)");
            check_near(A.egeom[v].hurt_radius, K->hurt_radius * sc, 1e-3f, "hurtbox radius x(1+0.02n)");
            check_near(A.egeom[v].hit_radius, K->hit_radius * sc, 1e-3f, "contact hitbox radius x(1+0.02n)");
            check_near(A.egeom[v].hurt_x, K->hurt_x, 1e-6f, "the shape OFFSET is not scaled");
        }
        check((int)A.eboost_n[v] == 3, "boosts-ever-applied counted");
        /* viperfish sets reset_nb_times_boosted_on_hit false, so the hit path
         * does not touch it; force a contact reset on a pursuer instead and
         * check the SIZE survives it (set_size is never undone) */
        A.enboost[v] = 0;
        float kept = A.egeom[v].hurt_radius;
        A.ebtimer[v] = bt_timer_frames(BT_KIND_SELFBOOST[vk].cooldown);   /* no fire this step */
        step1(&A, 0, obs, &rew, &term);
        check_near(A.egeom[v].hurt_radius, kept, 1e-6f, "the scaled geometry persists");
        /* the campaign pursuer: size 0 -> geometry never changes */
        int p2 = bt_reserve_slot(&A);
        bt_spawn_kind_at(&A, p2, pk, A.px + 1000.0f, A.py, 0);
        A.espd[p2] = 0.0f;
        step1(&A, 0, obs, &rew, &term);
        BT_CONST_PTR(BtBodyPhysics) KP = &BT_PHYSICS[bt_geom_index(pk)];
        float r0 = A.egeom[p2].hit_radius;
        A.enboost[p2] = 0;
        bt_enemy_self_boost(&A, p2, pk);
        check_near(A.egeom[p2].hit_radius, r0, 1e-6f, "size 0: no geometry change");
        check_near(A.egeom[p2].body_radius, KP->body_radius, 1e-6f, "and the kind row still holds");
    }

    printf("[53] a killed enemy's node outlives its `enemies` entry (death animation)\n");
    {
        /* entity.gd:88-100: die() plays entity.tscn's `death` animation and only
         * free_entity()/add_node_to_pool (its method key at t = 1.0 s =
         * BT_POOL_DEATH_FRAMES ticks) removes the node from the tree.  Until then
         * rl_obs.gd:417-425 `_collect` still reports it in the nearest-enemy
         * slots (health 0, drifting on the death knockback). */
        int chaser = find_kind("chaser");
        check(chaser >= 0, "chaser kind found");
        bt_core_init(&A, 99u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0; A.n_pend_enemy = 0; A.n_births = 0;
        for (int i = 0; i < BT_MAX; i++) { bt_enemy_set_alive(&A, i, 0); bt_enemy_set_dying(&A, i, 0); A.epend_die[i] = 0; }
        A.ecount = 0;
        int slot = 0;
        bt_spawn_kind_at(&A, slot, chaser, A.px + 200.0f, A.py, 0);
        check(A.ealive[slot] != 0, "the target is live");
        step1(&A, 0, obs, &rew, &term);
        check_near(obs[37], 1.0f, 1e-4f, "a live enemy's observed health ratio is 1");

        /* A lethal hit through the shared primitive: unit.gd:344-356 defers die()
         * and _pending_die refuses every later hit of the same flush. */
        A.phys.bodies[slot].vel = bt_v2(60.0f, 0.0f);   /* the next step's cached velocity: 1 px/frame */
        A.edkbx[slot] = 0.6f; A.edkby[slot] = 0.0f;     /* death knockback: 0.6 * 100 = 60 px/s */
        int k = 0;
        (void)bt_item_expl_hit_enemy(&A, slot, A.ehp[slot] + 10.0f, 0, 1.0f,
                                     0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0, 1, &k, 0);
        check(k == 1, "the lethal hit registers");
        check(A.ealive[slot] && A.edying[slot] == 0 && A.epend_die[slot] == 1,
              "the hit alone does not kill: die() is still deferred");
        (void)bt_item_expl_hit_enemy(&A, slot, 100.0f, 0, 1.0f,
                                     0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0, 1, &k, 0);
        check(A.ealive[slot] && A.ehp[slot] == 0.0f,
              "a second hit in the same flush deals nothing (unit.gd:266)");
        float x0 = A.ex[slot];
        bt_drain_kill_q(&A);
        check(!A.ealive[slot] && A.edying[slot] == BT_POOL_DEATH_FRAMES,
              "the queued die() starts the death animation");

        float want = x0 + 60.0f * BT_DT;   /* the death frame's cached velocity still applies */
        step1(&A, 0, obs, &rew, &term);
        check(A.edying[slot] == BT_POOL_DEATH_FRAMES - 1, "one animation frame elapsed");
        check_near(A.ex[slot], want, 2e-3f, "the node keeps the death frame's cached velocity");
        check_near(obs[37], 0.0f, 1e-6f, "the dying node's observed health ratio is 0");
        check_near(obs[35], (A.ex[slot] - A.px) / BT_ARENA_W, 1e-6f,
                   "the dying node fills the nearest-enemy slot");
        check_near(obs[67], 1.0f * BT_ENEMY_COUNT_SCALE, 1e-6f,
                   "the enemy-count feature still counts the dying node");
        int drift_ok = 1, seen_ok = 1;
        float disp = 0.9f;   /* frame 2's knockback step: 0.9^1 * kb * scale * dt */
        for (int f = 2; f <= BT_POOL_DEATH_FRAMES; f++) {
            want += 0.6f * 100.0f * BT_DT * disp;
            disp *= 0.9f;
            step1(&A, 0, obs, &rew, &term);
            if (bt_absf(A.ex[slot] - want) > 3e-2f) drift_ok = 0;
            if (A.edying[slot] > 0) {
                if (bt_absf(obs[37]) > 1e-6f ||
                    bt_absf(obs[35] - (A.ex[slot] - A.px) / BT_ARENA_W) > 1e-6f) seen_ok = 0;
            }
        }
        check(drift_ok, "the death knockback decays x0.9 per frame while the node lives");
        check(seen_ok, "the observation follows the drifting node for its whole life");
        check(A.edying[slot] == 0, "the node is pooled after 60 frames");
        check(A.phase == 0, "the wave is still running");
        check_near(obs[35], 0.0f, 0.0f, "the enemy slot is zeroed once the node is gone");
        check_near(obs[67], 0.0f, 0.0f, "the count feature drops the pooled node");

        /* The WAVE-EXPIRY kill is registered from the engine's IDLE pass (the
         * WaveTimer's `timeout` -> main.gd:1232 clean_up_room), so its die() misses
         * the kill frame's own AnimationPlayer advance and its node outlives the
         * physics-kill window by exactly one row: 61, not 60.  The game shows both:
         * v4_demon_s1 keeps `enemy_nodes`/obs[67] for 61 rows on all 11 of its
         * expired waves (kill row 179 -> last node row 239, gone 240), a weapon
         * kill for 60 (norm_ref row 163 -> 223).  Drive the real per-frame path
         * (bt_core_step) here so a second bt_enemy_dying_step inside the wave-end
         * window -- which halved the window and doubled the death drift -- cannot
         * come back. */
        {
            const int WINDOW = BT_POOL_DEATH_FRAMES + BT_POOL_DEATH_IDLE_EXTRA;
            bt_core_init(&A, 777u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            A.n_live_groups = 0; A.n_pend_enemy = 0; A.n_births = 0;
            for (int i = 0; i < BT_MAX; i++) { bt_enemy_set_alive(&A, i, 0); bt_enemy_set_dying(&A, i, 0); A.epend_die[i] = 0; }
            A.ecount = 0;
            bt_spawn_kind_at(&A, 0, chaser, A.px + 200.0f, A.py, 0);
            A.e_hold_tick[0] = 0xFFFFu;
            check(A.ecount == 1, "one live enemy before the expiry frame");
            /* the core's own expiry test: time_left one BT_DT above 0 (see the
             * clock comment in bt_core_step) fires the wave end on the next step */
            A.time_left = BT_DT + 1e-5f;
            step1(&A, 0, obs, &rew, &term);
            check(A.end_wave_active && A.ecount == 0,
                  "the expiry frame kills it and arms the 2 s wave-end window");
            check(A.edying[0] == WINDOW,
                  "an idle-pass kill enters the one-row-longer death animation");
            check_near(obs[67], 1.0f * BT_ENEMY_COUNT_SCALE, 1e-6f,
                       "the kill row itself still reports the node");
            int rows_with_node = 1, counter_ok = 1;
            for (int t = 1; t <= WINDOW; t++) {
                step1(&A, 0, obs, &rew, &term);
                if (obs[67] > 0.0f) rows_with_node++;
                if (A.edying[0] != (unsigned char)(WINDOW - t)) counter_ok = 0;
            }
            check(counter_ok, "the window advances exactly one death-animation frame per row");
            check(rows_with_node == WINDOW,
                  "the expiry node stays observable for 61 rows (a weapon kill's 60 + 1)");
            check_near(obs[67], 0.0f, 0.0f, "and is gone on the row after the window");
            check(A.ecount == 0, "the living count stays 0 through the whole window");
        }

        /* The death clip can be CUT SHORT.  A killed unit's behavior Timers are
         * plain CHILD nodes, so Godot keeps firing them while the `death` clip
         * plays, and charging_attack_behavior.gd:124-127
         * (`on_unlock_move_timer_timeout`) re-writes the clip's playback_speed
         * to _idle_playback_speed (enemy.gd:25, one rand_range(1,3) per NODE).
         * The clip then advances faster and the node leaves the tree early: the
         * deep-start captures show 38/39/58/37-row wave-end windows where the
         * norm is 61 (seed-1 wave-20 predator: the unlock Timer's remaining 22
         * frames expire at row 261 with the clip at position 0.366667 and
         * playback_speed 2.3986, and the node is gone at row 277 = kill + 38).
         * Drive the real per-frame path and pin the rescaled window against the
         * same float32 accumulation the AnimationPlayer uses. */
        {
            int bruiser = find_kind("horned_bruiser");
            check(bruiser >= 0, "horned_bruiser kind found");
            check(bruiser >= 0 && (int)BT_ATTACKS[bruiser].beh == BT_BEH_CHARGE,
                  "horned_bruiser runs the charging attack behavior");

            /* (a) mid-dash: shoot() (:110-121) armed the unlock Timer, so
             * `echarge` -- the Timer's own countdown, in frames -- is running. */
            bt_core_init(&A, 778u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            A.n_live_groups = 0; A.n_pend_enemy = 0; A.n_births = 0;
            for (int i = 0; i < BT_MAX; i++) { bt_enemy_set_alive(&A, i, 0); bt_enemy_set_dying(&A, i, 0); A.epend_die[i] = 0; }
            A.ecount = 0;
            bt_spawn_kind_at(&A, 0, bruiser, A.px + 200.0f, A.py, 0);
            A.e_hold_tick[0] = 0xFFFFu;
            A.eatk[0] = 1000.0f;          /* no attack clock activity in the fixture */
            A.ewind[0] = 0;
            A.echarge[0] = 10.0f;         /* the dash's unlock Timer, frames left */
            A.time_left = BT_DT + 1e-5f;
            step1(&A, 0, obs, &rew, &term);
            check(A.end_wave_active && A.ecount == 0, "the expiry frame kills the dasher");
            check(A.edying[0] == BT_POOL_DEATH_FRAMES + BT_POOL_DEATH_IDLE_EXTRA,
                  "the idle-pass kill enters the one-row-longer death animation");
            check(A.edspd[0] == 1.0f, "die() rewinds the clip to playback_speed 1");
            /* the kill samples the node's own rand_range(1,3) from the pool
             * ledger row (enemy.gd:25 runs in the member initializer) */
            check(A.edidl[0] > 1.0f && A.edidl[0] < 3.0f,
                  "the node's own _idle_playback_speed draw is in [1,3)");
            check(A.echarge[0] == 9.0f,
                  "the kill frame's own dash tick already ran (the Timer is a frame clock)");
            int E = (int)A.echarge[0];    /* frames left on the unlock Timer */
            float pos = 0.0f;             /* clip position after E speeds-1 advances */
            for (int i = 1; i <= E; i++) pos = pos + 1.0f * BT_DT;
            int m = bt_death_frames_left(pos, A.edidl[0]);
            int expect = E + m;           /* observable rows including the kill row */
            check(expect < BT_POOL_DEATH_FRAMES + BT_POOL_DEATH_IDLE_EXTRA,
                  "a running unlock Timer shortens the death window");
            int rows_with_node = 1, switch_ok = 1, rescale_ok = 1;
            for (int t = 1; t <= expect + 2; t++) {
                step1(&A, 0, obs, &rew, &term);
                if (obs[67] > 0.0f) rows_with_node++;
                if (t == E) {
                    if (A.edspd[0] != A.edidl[0]) switch_ok = 0;
                    if (A.edying[0] != (unsigned char)m) rescale_ok = 0;
                }
            }
            check(switch_ok, "the unlock Timer re-writes the clip's playback_speed on timeout");
            check(rescale_ok, "and the frames left are rescaled from the position reached");
            check(rows_with_node == expect,
                  "the dasher's node stays observable E + rescaled frames, not 61");
            check(A.edying[0] == 0, "and it is gone after the rescaled window");

            /* (b) the same kill without a running Timer keeps the base window. */
            bt_core_init(&A, 778u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            A.n_live_groups = 0; A.n_pend_enemy = 0; A.n_births = 0;
            for (int i = 0; i < BT_MAX; i++) { bt_enemy_set_alive(&A, i, 0); bt_enemy_set_dying(&A, i, 0); A.epend_die[i] = 0; }
            A.ecount = 0;
            bt_spawn_kind_at(&A, 0, bruiser, A.px + 200.0f, A.py, 0);
            A.e_hold_tick[0] = 0xFFFFu;
            A.eatk[0] = 1000.0f;
            A.ewind[0] = 0;
            A.echarge[0] = 0.0f;          /* no dash: the Timer is not running */
            A.time_left = BT_DT + 1e-5f;
            step1(&A, 0, obs, &rew, &term);
            check(A.edying[0] == BT_POOL_DEATH_FRAMES + BT_POOL_DEATH_IDLE_EXTRA,
                  "without a running Timer the expiry window is still 61");
            int quiet_rows = 1;
            for (int t = 1; t <= BT_POOL_DEATH_FRAMES + BT_POOL_DEATH_IDLE_EXTRA + 2; t++) {
                step1(&A, 0, obs, &rew, &term);
                if (obs[67] > 0.0f) quiet_rows++;
            }
            check(quiet_rows == BT_POOL_DEATH_FRAMES + BT_POOL_DEATH_IDLE_EXTRA,
                  "and the node is observable for exactly 61 rows");
        }
    }

    printf("[54] a multi-kill explosion applies every hit before any die (unit.gd:352)\n");
    {
        /* unit.gd:344-356 queues die() with call_deferred, and MessageQueue::flush
         * drains the whole queue AFTER every hurt_area_entered_deferred of the
         * frame, so one explosion's hits all land before the first die body --
         * i.e. before any of the frame's loot/drop RNG runs. */
        int chaser = find_kind("chaser");
        /* seed 3: the two orders land on different crit values, so the pre-fix
         * inline order is provably distinguishable (kill set 3 -> 2) -- see the
         * sensitivity check at the end. */
        bt_core_init(&A, 3u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0; A.n_pend_enemy = 0; A.n_births = 0;
        for (int i = 0; i < BT_MAX; i++) { bt_enemy_set_alive(&A, i, 0); bt_enemy_set_dying(&A, i, 0); A.epend_die[i] = 0; }
        A.ecount = 0; A.n_kill_q = 0;
        for (int t = 0; t < 3; t++) {
            bt_spawn_kind_at(&A, t, chaser, A.px + 20.0f, A.py - 30.0f + 30.0f * (float)t, 0);
            A.e_hold_tick[t] = 0xFFFFu;
            /* crit-lethal only: a crit (1 * 3) kills, a plain hit (1) does not,
             * so the order its crit roll draws from IS observable in the kills. */
            A.ehp[t] = 2.0f; A.emax[t] = 2.0f;
        }
        check(A.ecount == 3, "three targets are live inside the circle");
        int pj = bt_explode(&A);
        check(pj >= 0, "the explosion node is pooled");
        A.expl.e[pj].dmg = 1.0f;
        A.expl.e[pj].crit_chance = 0.5f;    /* one crit roll per hit */
        A.expl.e[pj].crit_dmg = 3.0f;
        A.expl.e[pj].burn_chance = 0.0f;
        A.expl.snd_pending[pj] = 0;         /* isolate the RNG from the node's sound pick */
        (void)bt_item_expl_tick(&A, &rew);  /* spawn frame: the hitbox collision is deferred */
        check(A.ealive[0] && A.ealive[1] && A.ealive[2], "no hit on the spawn frame");

        static BtState C2;                  /* the pre-tick state for the sensitivity replay */
        C2 = A;
        B = A;                              /* expected: all hits, then all dies */
        bt_u64 pre_rng = A.rng.state;
        (void)bt_item_expl_tick(&A, &rew);  /* the real tick (its own drain included) */
        check(A.ecount < 3, "the explosion kills at least one target");
        for (int j = 0; j < 3; j++) {
            if (!B.ealive[j]) continue;
            int is_crit = bt_chance(&B, B.expl.e[pj].crit_chance);
            int kk = 0;
            (void)bt_item_expl_hit_enemy(&B, j, B.expl.e[pj].dmg, is_crit, B.expl.e[pj].crit_dmg,
                                         0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0, 1, &kk, 0);
        }
        bt_drain_kill_q(&B);
        check(B.rng.state == A.rng.state && B.rng.state != pre_rng,
              "the tick consumes the RNG in the all-hits-then-all-dies order");
        check(B.utils_rng.state == A.utils_rng.state, "the Utils stream matches too");
        check(A.ealive[0] == B.ealive[0] && A.ealive[1] == B.ealive[1] &&
              A.ealive[2] == B.ealive[2], "the same targets die");
        check(A.matcount == B.matcount, "the same number of drops");
        int same_drops = 1;
        for (int m = 0; m < BT_MAX_MAT; m++) {
            if (A.matalive[m] != B.matalive[m] || A.matval[m] != B.matval[m] ||
                A.matx[m] != B.matx[m] || A.maty[m] != B.maty[m]) same_drops = 0;
        }
        check(same_drops, "the drops land exactly where the two-phase order puts them");

        /* Sensitivity: the pre-fix inline order (hit,die per target) hands the
         * same draws out differently -- the second target's crit roll comes after
         * the first die() body's draws -- so it must NOT reproduce that result. */
        A = C2;                             /* the pre-tick state, restored */
        for (int j = 0; j < 3; j++) {
            if (!A.ealive[j]) continue;
            int is_crit = bt_chance(&A, A.expl.e[pj].crit_chance);
            int kk = 0;
            (void)bt_item_expl_hit_enemy(&A, j, A.expl.e[pj].dmg, is_crit, A.expl.e[pj].crit_dmg,
                                         0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0, 1, &kk, 0);
            bt_drain_kill_q(&A);
        }
        int inline_differs = (A.ealive[0] != B.ealive[0] || A.ealive[1] != B.ealive[1] ||
                              A.ealive[2] != B.ealive[2]);
        for (int m = 0; m < BT_MAX_MAT; m++)
            if (A.matalive[m] != B.matalive[m] || A.matx[m] != B.matx[m] ||
                A.maty[m] != B.maty[m]) inline_differs = 1;
        check(inline_differs, "the inline hit+die order is distinguishable (the deferral matters)");
    }

    printf("[61] pursuer change_movement_behavior_after_x_boosts (viperfish)\n");
    {
        /* pursuer.gd:16,69-72 + viperfish.tscn:31-32 (change_after 3, bonus 200):
         * once nb_times_boosted reaches the threshold the active MovementBehavior
         * is swapped once to $MovementBehaviorOnBoost (viperfish.tscn:52-53 =
         * follow_target with default params) and a flat +200 bonus_speed is
         * added.  movement_behavior_changed is sticky: the hit reset zeroes
         * nb_times_boosted but never switches back. */
        int vk = find_kind("viperfish");
        int pk = find_kind("pursuer");
        check(BT_KIND_SELFBOOST[vk].change_after == 3, "viperfish threshold 3");
        check(BT_KIND_SELFBOOST[pk].change_after == -1, "campaign pursuer has no switch");
        bt_core_init(&A, 151u, 1, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        int v = bt_reserve_slot(&A);
        bt_spawn_kind_at(&A, v, vk, A.px + 700.0f, A.py, 0);
        A.espd[v] = 0.0f;
        step1(&A, 0, obs, &rew, &term);
        check(A.mv_boost[v] == 0, "no switch before the threshold");
        A.enboost[v] = 0; A.ebonus[v] = 0.0f;
        bt_enemy_self_boost(&A, v, vk);          /* boost 1 */
        bt_enemy_self_boost(&A, v, vk);          /* boost 2 */
        check(A.mv_boost[v] == 0, "still no switch below the threshold");
        check_near(A.ebonus[v], 2.0f * BT_KIND_SELFBOOST[vk].speed, 1e-3f, "two speed boosts only");
        bt_enemy_self_boost(&A, v, vk);          /* boost 3 -> switch */
        check(A.mv_boost[v] == 1, "the third boost switches the MovementBehavior");
        check_near(A.ebonus[v],
                   3.0f * BT_KIND_SELFBOOST[vk].speed + BT_KIND_SELFBOOST[vk].bonus_on_change,
                   1e-3f, "and adds the flat bonus_speed_on_change once");
        float after = A.ebonus[v];
        bt_enemy_self_boost(&A, v, vk);          /* a 4th: the switch must not re-add */
        check_near(A.ebonus[v], after + BT_KIND_SELFBOOST[vk].speed, 1e-3f,
                   "the switch bonus is paid ONCE");
        /* the swapped behavior's vector: follow_target = current_target - self,
         * where the enemy's only target is the player root. */
        A.ehx[v] = A.ex[v]; A.ehy[v] = A.ey[v];
        BtVec2 mv = bt_enemy_move(&A, v);
        check_near(mv.x, A.px - A.ehx[v], 1e-3f, "switched movement heads straight at the player");
        check_near(mv.y, A.py - A.ehy[v], 1e-3f, "switched movement heads straight at the player (y)");
        /* the sticky flag survives the hit reset that zeroes the counter */
        A.enboost[v] = 5; A.ebonus[v] = 100.0f;
        A.ebtimer[v] = bt_timer_frames(BT_KIND_SELFBOOST[vk].cooldown);
        A.epend = (short)v;
        step1(&A, 0, obs, &rew, &term);
        check(A.mv_boost[v] == 1, "the switch is sticky (movement_behavior_changed)");
        /* a campaign pursuer never switches */
        int p2 = bt_reserve_slot(&A);
        bt_spawn_kind_at(&A, p2, pk, A.px + 800.0f, A.py, 0);
        A.espd[p2] = 0.0f;
        step1(&A, 0, obs, &rew, &term);
        A.enboost[p2] = 0;
        for (int n = 0; n < 4; n++) bt_enemy_self_boost(&A, p2, pk);
        check(A.mv_boost[p2] == 0, "change_after -1 never switches");
    }

    printf("[62] the burning timer's hit+die rank the internal pass (before the group pass)\n");
    {
        /* unit.tscn's BurningTimer is a physics Timer: its `timeout` runs in
         * SceneTree::iteration's "physics_process_internal" pass, BEFORE the
         * "physics_process" group pass and before that iteration's
         * MessageQueue::flush (scene_tree.cpp:603-609).  Consequences the core
         * must reproduce:
         *   1. a lethal burn tick lands BEFORE this frame's area hits, so
         *      unit.gd:266-267 `_pending_die` makes the frame's weapon hit deal
         *      NOTHING; and
         *   2. its die() is queued ahead of the dies the flush itself queues from
         *      the area hits, i.e. burn dies drain BEFORE area-hit dies. */
        int chaser = find_kind("chaser");
        check(chaser >= 0, "chaser kind found");
        bt_core_init(&A, 4242u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0; A.n_pend_enemy = 0; A.n_births = 0;
        for (int i = 0; i < BT_MAX; i++) { bt_enemy_set_alive(&A, i, 0); bt_enemy_set_dying(&A, i, 0); A.epend_die[i] = 0; }
        A.ecount = 0; A.n_kill_q = 0; A.n_kill_tq = 0;
        int b = bt_reserve_slot(&A);
        bt_spawn_kind_at(&A, b, chaser, A.px + 300.0f, A.py, 0);
        A.e_hold_tick[b] = 0xFFFFu;
        int w = bt_reserve_slot(&A);
        bt_spawn_kind_at(&A, w, chaser, A.px + 320.0f, A.py, 0);
        A.e_hold_tick[w] = 0xFFFFu;
        /* b dies to a burn tick this frame; w survives it and is killed by the
         * frame's area hit, so the two dies must come out in that rank. */
        A.ehp[b] = 1.0f; A.ehp[w] = 50.0f;
        A.eburn[b] = 1.0f; A.eburndmg[b] = 5.0f;   /* bt_burn_period 1: ticks now */
        A.ekbx[w] = 0.0f; A.ekby[w] = 0.0f;
        A.edkbx[w] = 0.0f; A.edkby[w] = 0.0f;
        (void)bt_burn_tick(&A, &rew);          /* the internal-pass timer */
        check(A.epend_die[b] == 1, "a lethal burn tick queues b's die");
        check(A.ealive[b] == 1, "and the die is still deferred");
        /* the group pass would now run; the flush's area hit on b must be refused */
        int k = 0;
        float before = A.ehp[b];
        (void)bt_item_expl_hit_enemy(&A, b, 10.0f, 0, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0, 1, &k, 0);
        check(k == 0 && A.ehp[b] == before,
              "an area hit on the burning-to-death slot deals nothing (unit.gd:266)");
        (void)bt_item_expl_hit_enemy(&A, w, 100.0f, 0, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0, 1, &k, 0);
        check(k == 1, "the area hit kills the other target");
        check(A.n_kill_tq == 1 && A.n_kill_q == 1, "one Timer-rank and one area-rank die queued");
        /* the flush boundary: the Timer rank drains FIRST */
        bt_u64 r0 = A.rng.state;
        bt_drain_kill_q(&A);
        check(A.n_kill_tq == 0 && A.n_kill_q == 0, "both ranks drained");
        check(A.epend_die[b] == 0 && A.epend_die[w] == 0, "the pending-die flags cleared");
        check(!A.ealive[b] && A.edying[b] == BT_POOL_DEATH_FRAMES,
              "b's die() ran and started its death animation");
        check(A.rng.state != r0, "the deaths consumed the loot RNG in the flush's rank order");
        check(!A.ealive[w] && A.edying[w] == BT_POOL_DEATH_FRAMES, "w's die() ran too");
    }

    printf("[63] reset after a stepped episode restores the fresh wave clock\n");
    {
        /* The reset contract the strict CPU/CUDA harness (tests/test_brotato_cuda.cu
         * layer A) leans on: bt_core_reset must leave a *stepped* state with a
         * brand-new wave clock, not the previous episode's.  The wave-time
         * feature is (time_left - BT_DT) / wave_duration for a combat snapshot,
         * so a reset that inherited the old time_left would report one BT_DT of
         * extra wave clock -- which is exactly the "observation[66]: got
         * 0.998333335 want 0.999166667" divergence the harness reported at
         * reset (the value was the *post-step* buffer; obs[66] is this feature).
         * bt_enter_wave owns the whole clock: time_left, tick_in_wave,
         * tick_sound_left and the tick counter. */
        const int WAVE_TIME = BT_OBS_PLAYER + BT_OBS_ITEMS + BT_OBS_UPGRADES + BT_OBS_ENEMIES + 1;

        bt_core_init(&A, 777u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        bt_core_observe(&A, obs);
        float fresh_time = obs[WAVE_TIME];
        /* The run prologue consumes the wave's FIRST frame: the recorded row 0
         * is written at the top of the game's first bridge frame, i.e. after the
         * freshly created Main scene's first physics frame, so its WaveTimer
         * reads duration - 1/60 (measured on v5 s1 row 0; the clock then tracks
         * the game frame for frame and no observation compensates). */
        check(A.time_left == A.wave_duration - BT_DT,
              "a fresh episode's wave clock is one frame into the wave");
        check(fresh_time > 0.0f && fresh_time < 1.0f, "the fresh wave-time feature is in (0,1)");

        int stepped = 0;
        while (stepped < 600 && !A.done) { bt_core_step(&A, 0, obs, &rew, &term); stepped++; }
        check(stepped > 0, "the episode ran ticks before the reset");
        check(A.time_left < A.wave_duration, "the stepped state's time_left has run down");
        check(A.ticks == stepped && A.tick_in_wave == stepped, "the stepped state's tick counters advanced");
        check(obs[WAVE_TIME] != fresh_time, "the wave-time feature tracks the wave clock (control)");

        bt_core_reset(&A);
        bt_core_observe(&A, obs);
        check(A.ticks == 0, "reset zeroes the tick counter");
        check(A.phase == 0, "reset returns to the combat phase");
        check(A.time_left == A.wave_duration - BT_DT,
              "reset restores time_left one frame into the fresh wave (same prologue)");
        check(A.tick_in_wave == 0, "reset clears the in-wave tick counter");
        check(A.tick_sound_left == 0, "reset disarms the wave tick-sound timer");
        check(obs[WAVE_TIME] == fresh_time,
              "reset-after-step reproduces the fresh episode's wave-time feature bit-exactly");

        bt_core_reset(&A);
        bt_core_observe(&A, obs);
        check(obs[WAVE_TIME] == fresh_time, "a second reset reproduces the same wave clock");

        /* The replay-override lane: `start_wave` (the boot wave every reset
         * re-enters) and `wave_ovr` (the instant_waves duration hook) are
         * init/wrapper-scoped and survive bt_core_reset by design, so the clock
         * they build must come out identical no matter how much history the state
         * carries. */
        bt_core_init(&B, 777u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        B.start_wave = 20;
        B.wave_ovr = 12.0f;
        bt_core_reset(&B);
        bt_core_observe(&B, obs);
        float fresh_ovr = obs[WAVE_TIME];
        check(B.wave == 20 && B.wave_duration == 12.0f && B.time_left == 12.0f - BT_DT,
              "a fresh reset honours start_wave + wave_ovr (wave 20 at the 12 s override, "
              "one prologue frame into its clock)");

        for (int i = 0; i < 300 && !B.done; i++) bt_core_step(&B, 6, obs, &rew, &term);
        bt_core_reset(&B);
        bt_core_observe(&B, obs);
        check(B.wave == 20, "reset re-enters the configured start_wave after any history");
        check(B.wave_duration == 12.0f && B.time_left == 12.0f - BT_DT,
              "the wave_ovr duration survives the reset (the clock is one prologue "
              "frame in, like every entry)");
        check(B.ticks == 0 && B.tick_in_wave == 0 && B.tick_sound_left == 0,
              "the override lane's clock is fresh too (tick counter, in-wave tick, tick-sound timer)");
        check(obs[WAVE_TIME] == fresh_ovr,
              "reset-after-step matches a fresh reset in the override lane bit-exactly");
    }

    printf("[64] a death knockback direction uses the LAGGED enemy Node position (unit.gd:281)\n");
    {
        /* unit.gd:281 weights a hit's hitbox direction with
         * `players_ref[fpi].global_position.direction_to(global_position)` -- the
         * VICTIM's own Node transform.  A MODE_CHARACTER enemy's Node transform
         * is one physics step behind the integrated body (brotato_state.h:749-765),
         * so at the frame of the hit it still reads the PRE-step position
         * (s->ehx/s->ehy), exactly like the enemy contact Hitbox area test.  This
         * pins the SAMPLE, not just the formula: the two positions disagree and
         * the queued die() seed (unit.gd:348) must follow the lagged one. */
        int chaser = find_kind("chaser");
        check(chaser >= 0, "chaser kind found");
        bt_core_init(&A, 77u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0; A.n_pend_enemy = 0; A.n_births = 0;
        for (int i = 0; i < BT_MAX; i++) { bt_enemy_set_alive(&A, i, 0); bt_enemy_set_dying(&A, i, 0); A.epend_die[i] = 0; }
        A.ecount = 0; A.n_kill_q = 0; A.n_kill_tq = 0;
        bt_wproj_reset(&A);
        A.px = 0.0f; A.py = 0.0f;
        int j = bt_reserve_slot(&A);
        bt_spawn_kind_at(&A, j, chaser, 40.0f, 20.0f, 0);
        check(j >= 0 && A.ealive[j] != 0, "the target is live");
        A.ehp[j] = 1.0f; A.emax[j] = 1.0f;      /* any hit is lethal */
        A.ehx[j] = 8.0f;  A.ehy[j] = 36.0f;     /* the Node transform, one step behind */
        A.ex[j]  = 40.0f; A.ey[j]  = 20.0f;     /* this frame's integrated body */
        BtWProjArgs a = BtWProjArgs();
        a.x = 40.0f; a.y = 20.0f;               /* the shot starts on the target */
        a.rot = 0.0f;                           /* hitbox knockback direction (1, 0) */
        a.speed = 600.0f;
        a.dmg = 40.0f; a.crit_dmg = 1.0f;
        a.max_range = 10000.0f;
        a.shape = BT_SHAPE_CIRCLE;
        a.flags = BT_WPROJ_F_LEAVE;
        a.rad = 44.0f;
        a.knockback = 12.0f;                    /* raw hitbox amount */
        int i = bt_wproj_spawn(&A, a);
        check(i >= 0, "the shot spawns");
        float r = 0.0f;
        bt_wproj_step(&A, &r);                  /* arm (Hitbox.enable is deferred) */
        bt_wproj_step(&A, &r);                  /* hit */
        check(A.ehp[j] <= 0.0f, "the hit is lethal");
        BtVec2 hb = bt_v2(bt_cosf(a.rot), bt_sinf(a.rot));
        BtVec2 lag = bt_knockback_direction_weighted(hb, bt_v2(A.px, A.py), bt_v2(8.0f, 36.0f));
        BtVec2 body = bt_knockback_direction_weighted(hb, bt_v2(A.px, A.py), bt_v2(40.0f, 20.0f));
        BtVec2 want = bt_death_knockback(lag, a.knockback);
        BtVec2 other = bt_death_knockback(body, a.knockback);
        check(bt_fabsf(want.x - other.x) + bt_fabsf(want.y - other.y) > 1e-3f,
              "the fixture separates the lagged and integrated positions");
        check_near(A.edkbx[j], want.x, 1e-5f, "the death seed uses the LAGGED Node position (x)");
        check_near(A.edkby[j], want.y, 1e-5f, "the death seed uses the LAGGED Node position (y)");
        check(bt_fabsf(A.edkbx[j] - other.x) + bt_fabsf(A.edkby[j] - other.y) > 1e-3f,
              "and NOT this frame's integrated body position");
        bt_drain_kill_q(&A);
        check(!A.ealive[j] && A.edying[j] == BT_POOL_DEATH_FRAMES, "the queued die() runs");
        check_near(A.edkx[j], want.x, 1e-5f, "die() seeds the drifting node's stored vector (x)");
        check_near(A.edky[j], want.y, 1e-5f, "die() seeds the drifting node's stored vector (y)");
    }

    printf("[65] a crit hit consumes the critical-particle restart draws (effects_manager.gd:42-58)\n");
    {
        /* unit.gd:312-315 emits `crit_effect(hitbox.from)` for a crit (or
         * one-shot) hit on a unit, and every Unit is wired to the manager
         * (main.gd:905), which plays particles/critical_particles.tscn: amount 1
         * + one_shot + explosiveness 1.0 on a POINT shape, so exactly one
         * particle restarts and the engine's per-restart draw order
         * (scene/2d/cpu_particles_2d.cpp:864-890) consumes nine global words.
         * The play is NOT gated by settings.visual_effects, so it must consume
         * them even with the bridge's particle suppression on. Pin the count and
         * both gates (is_crit, has_from). */
        int chaser = find_kind("chaser");
        bt_core_init(&A, 20261108u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0; A.n_pend_enemy = 0; A.n_births = 0;
        for (int i = 0; i < BT_MAX; i++) { bt_enemy_set_alive(&A, i, 0); bt_enemy_set_dying(&A, i, 0); A.epend_die[i] = 0; }
        A.ecount = 0; A.n_kill_q = 0; A.n_kill_tq = 0;
        int j = bt_reserve_slot(&A);
        bt_spawn_kind_at(&A, j, chaser, 300.0f, 400.0f, 0);
        A.ehp[j] = 1.0e9f; A.emax[j] = 1.0e9f;   /* survives every probe hit */
        int k = 0;
        /* Arms that differ ONLY in has_from: every other draw of the hit (the
         * sound picks) is identical, so the arm delta is exactly the
         * crit-particle words. */
        BtRng r_crit = A.rng;
        (void)bt_item_expl_hit_enemy(&A, j, 1.0f, 1, 2.0f, 0.0f, 0.0f, 0.0f,
                                     0.0f, 0.0f, 0, 1, &k, 0);
        BtRng r_crit_from = A.rng;
        (void)bt_item_expl_hit_enemy(&A, j, 1.0f, 1, 2.0f, 0.0f, 0.0f, 0.0f,
                                     0.0f, 0.0f, -1, 0, &k, 0);
        BtRng r_crit_nofrom = A.rng;
        int n_from = 0, n_nofrom = 0;
        { BtRng t = r_crit; while (!(t.state == r_crit_from.state && t.inc == r_crit_from.inc) && n_from < 64) { (void)bt_rng_next(&t); n_from++; } }
        { BtRng t = r_crit_from; while (!(t.state == r_crit_nofrom.state && t.inc == r_crit_nofrom.inc) && n_nofrom < 64) { (void)bt_rng_next(&t); n_nofrom++; } }
        check(n_from - n_nofrom == 9, "a crit hit with a hitbox `from` draws exactly 9 more words (the particle restart)");
        check(n_nofrom > 0, "the crit hit's own sound picks still draw");
        BtRng r_pre = A.rng;
        (void)bt_item_expl_hit_enemy(&A, j, 1.0f, 0, 2.0f, 0.0f, 0.0f, 0.0f,
                                     0.0f, 0.0f, 0, 1, &k, 0);
        check(A.rng.state != r_pre.state, "a plain hit draws its own sound picks");
    }

    printf("[66] the enemy observation's elite field is the source's is_elite boolean\n");
    {
        /* rl_obs.gd:488-491 writes 1.0 iff Enemy.is_elite (ItemEnemy.gd:5); every
         * boss scene overrides the boss.gd:4 default off (predator.tscn:26), so a
         * boss class (2) must NOT leak its class into obs[39+5k]. */
        bt_core_init(&A, 11u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        (void)0; int plain = find_kind("chaser");
        int elite = find_kind("butcher");
        int boss = find_kind("predator");
        check(plain >= 0 && elite >= 0 && boss >= 0, "the three probe kinds exist");
        check(bt_kind_class(plain) == 0 && bt_kind_class(elite) == 1 && bt_kind_class(boss) == 2,
              "the probe kinds carry the classes 0/1/2");
        for (int i = 0; i < BT_MAX; i++) { bt_enemy_set_alive(&A, i, 0); bt_enemy_set_dying(&A, i, 0); A.epend_die[i] = 0; }
        A.ecount = 0; A.n_kill_q = 0; A.n_kill_tq = 0;
        (void)bt_spawn_kind(&A, boss, A.px + 30.0f, A.py);
        (void)bt_spawn_kind(&A, elite, A.px + 60.0f, A.py);
        (void)bt_spawn_kind(&A, plain, A.px + 90.0f, A.py);
        float obs[BT_OBS_SIZE];
        bt_core_observe(&A, obs);
        check(obs[39] == 0.0f, "a boss (class 2) writes elite 0, not its class");
        check(obs[44] == 1.0f, "an elite (class 1) writes elite 1");
        check(obs[49] == 0.0f, "a plain enemy (class 0) writes elite 0");
    }

    printf("[67] deep-start wave-20: boss scene-script `_init` repeats + predator child `_ready` draws\n");
    {
        /* Godot runs an Enemy-derived class chain's member initializers once per
         * SCRIPT LEVEL (bare kind scene 1, scripted kind 2, kind -> boss.tscn ->
         * enemy.tscn 3), and a boss scene's plain children draw while the tree
         * enters it (predator.tscn's `Pivot` rolls one get_rand_element and its
         * nine EnemyProjectileRotating children one rand_range each).  The
         * generated op list has a single member row and no child row, so
         * bt_enemy_init_scene_extra replays them.  The rngdiag site log of the
         * seed-1 deep-start capture pins the fresh-spawn word counts below. */
        int k_baby = find_kind("baby_alien");
        int k_boss = find_kind("predator");
        check(k_baby >= 0 && k_boss >= 0, "the wave-20 probe kinds exist");
        check(BT_KIND_INIT_REPEAT[k_baby] == 0 && BT_KIND_INIT_REPEAT[k_boss] == 2,
              "a bare kind's scene repeats the `_init` member row 1x, a boss scene 3x");
        if (find_kind("pursuer") >= 0)
            check(BT_KIND_INIT_REPEAT[find_kind("pursuer")] == 1,
                  "a script-overriding kind's scene repeats the `_init` member row 2x");
        check(BT_KIND_SCENE_READY_N[k_boss] == 10,
              "the predator's Pivot block is nine projectile + one direction draw");
        {
            /* Every spawn path draw uses the GLOBAL stream, so the word count of
             * one fresh spawn is the whole difference. */
            struct { const char* name; int want; } probe[] = {
                { "baby_alien", 6 },    /* 1 member + speed                       */
                { "invoker",   21 },    /* 3 member + 3 atk_cd + speed            */
                { "predator",  49 },    /* 3 member + 2 atk_cd + 10 child + speed
                                         * + movement init                        */
            };
            for (unsigned pi = 0; pi < sizeof(probe)/sizeof(probe[0]); pi++) {
                int k = find_kind(probe[pi].name);
                if (k < 0) continue;
                bt_core_init(&A, 1234u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
                BtRng from = A.rng;
                (void)bt_spawn_kind(&A, k, A.px + 200.0f, A.py);
                BtRng to = A.rng;
                int w = 0;
                { BtRng t = from;
                  while (!(t.state == to.state && t.inc == to.inc) && w < 512) { (void)bt_rng_next(&t); w++; } }
                check_near((float)w, (float)probe[pi].want, 0.5f,
                           "a fresh spawn consumes the source's global words");
            }
        }
        /* Wave-20 group spawn (seed 1, Demon, ghost scepter, endless,
         * invulnerable, 4 s waves, the capture's scripted actions): the first
         * group baby_alien is born on tick 123 and must carry the source's
         * unit.gd:81 jitter (-47 -> get_final_enemy_speed 203), not the word a
         * 40-words-early stream read (-10 -> 240). */
        bt_core_init(&A, 1u, 0, 1, BT_CHARACTER_DEMON, (unsigned short)118, 0);
        A.particles_rng = 0; A.invulnerable = 1; A.endless = 1;
        A.wave_ovr = 4.0f; A.start_wave = 20; A.rng_ready = 0;
        bt_core_reset(&A);
        {
            static const int script_actions[10] = { 8, 2, 4, 6, 0, 3, 1, 5, 7, 0 };
            float obs[BT_OBS_SIZE], rew, term;
            int slot = -1;
            for (int t = 1; t <= 123; t++) {
                int act = script_actions[((t - 1) / 30) % 10];
                bt_core_step(&A, act, obs, &rew, &term);
            }
            for (int i = 0; i < BT_MAX; i++)
                if (A.ealive[i] && A.ekind[i] == k_baby && A.e_hold_tick[i] >= 120u)
                    slot = i;
            check(slot >= 0, "the wave-20 group's baby_alien is born on tick 123");
            if (slot >= 0) {
                check_near(A.ex[slot], 1850.27625f, 0.01f, "at the group's rolled edge position");
                check_near(A.ey[slot], 1472.0f, 0.01f, "at the group's rolled edge position");
                check_near(A.espeed_rand[slot], -47.0f, 0.5f,
                           "the baby_alien carries the source's speed jitter (-47)");
                check_near(A.espd[slot], 203.0f, 0.5f,
                           "and get_final_enemy_speed yields 203");
            }
        }
    }

    printf("[68] the charging clock is a FRAME clock (deep-start wave-20 predator)\n");
    {
        /* The merge-gate mirror of tests/test_brotato_enemy_motion.cpp [27]
         * (brotato_land.sh only compiles THIS file): charging_attack_behavior.gd
         * :41-46 keeps `_current_cd` in FRAMES and :63-67 subtracts exactly one
         * frame per physics frame, so a 120-frame cooldown expires on frame 120
         * and start_shoot's `_can_move = false` (the animation's t=0 method,
         * deferred to that frame's flush) zeroes the NEXT frame's movement input
         * while the trigger frame still carries the cached step.  A float32
         * SECONDS accumulator (eatk = frames/60 decremented by BT_DT) stalls at
         * the boundary, which put the seed-1 deep-start capture's wave-20
         * predator hold one frame late: the game holds at tick 185 x 1529.5636,
         * the seconds clock stepped once more to x 1526.4060 (the capture's
         * first divergence, tick 185 obs[35]). */
        int k = find_kind("predator");
        check(k >= 0, "the predator kind exists");
        if (k >= 0) {
            int cd_f = (int)BT_ATTACKS[k].cooldown_f;
            int at_f = (int)BT_ATTACKS[k].shoot_at_f;
            int dash_f = (int)bt_timer_frames(BT_ATTACKS[k].charge_duration);
            float obs[BT_OBS_SIZE], rew, term;
            bt_core_init(&A, 9u, 0, 1, BT_CHARACTER_KNIGHT, BT_WEAPON_NONE);
            A.wave = 1; A.n_live_groups = 0;
            for (int i = 0; i < BT_MAX; i++) bt_enemy_set_alive(&A, i, 0);
            for (int i = 0; i < BT_MAX_MAT; i++) A.matalive[i] = 0;
            A.ecount = 0; A.matcount = 0;
            A.px = 1000.0f; A.py = 500.0f;
            (void)bt_spawn_kind_at(&A, 0, k, 1300.0f, 500.0f, 0);
            A.ehx[0] = 1300.0f; A.ehy[0] = 500.0f;
            bt_core_step(&A, 0, obs, &rew, &term);      /* newborn hold */
            A.eatk[0] = (float)cd_f;                    /* the `_ready` get_cd() */
            int lock_at = 0, shoot_at = 0, unlock_at = 0;
            float x_lock = 0.0f, x_step = 0.0f, x_hold = 0.0f;
            for (int f = 1; f <= 400 && !unlock_at; f++) {
                bt_core_step(&A, 0, obs, &rew, &term);
                if (!lock_at && A.ewind[0] == 1) { lock_at = f; x_lock = A.ex[0]; }
                else if (lock_at && !shoot_at && A.ewind[0] == 0 && A.echarge[0] > 0.0f) shoot_at = f;
                else if (shoot_at && A.echarge[0] <= 0.0f) unlock_at = f;
                if (lock_at && f == lock_at + 1) x_step = A.ex[0];
                if (lock_at && f == lock_at + 2) x_hold = A.ex[0];
            }
            check(lock_at == cd_f, "start_shoot's lock starts on the cooldown's own FRAME");
            check(bt_fabsf(x_step - x_lock) > 0.1f && bt_fabsf(x_hold - x_step) < 1e-3f,
                  "the trigger frame still steps, the next one holds (frame-clock lock)");
            check(shoot_at == cd_f + at_f, "the `shoot` key fires shoot_at_f frames later");
            check(unlock_at == cd_f + at_f + dash_f,
                  "and the unlock Timer fires charge_duration frames after it");
        }
    }

    printf("[69] sparse slot bounds retain holes, dying nodes and processing order\n");
    {
        float oa[BT_OBS_SIZE], ob[BT_OBS_SIZE], ra, rb, ta, tb;
        bt_core_init(&A, 7123u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        check(A.e_used == 0 && A.pr_used == 0 && A.con_used == 0 && A.mat_used == 0 &&
              A.wproj.used == 0 && A.expl.used == 0, "fresh pools have empty scan bounds");
        int k = find_kind("chaser");
        if (k >= 0) {
            bt_spawn_kind_at(&A, 0, k, A.px + 100.0f, A.py, 0);
            bt_spawn_kind_at(&A, 3, k, A.px - 100.0f, A.py, 0);
            bt_spawn_kind_at(&A, 9, k, A.px + 200.0f, A.py, 0);
            bt_mark_enemy_dead(&A, 9, 0);
            check(A.e_used == 10 && A.edying[9] != 0,
                  "the highest dying node stays inside the enemy bound");
        }
        int pj = bt_spawn_projectile(&A, A.px + 150.0f, A.py, 0.0f, 0.0f, 1.0f,
                                    BT_SHAPE_CIRCLE, BT_PROJ_R_DEFAULT, 0.0f, 0.0f, 0.0f, 0);
        check(pj == 0 && A.pr_used == 1, "projectile spawn extends only its used prefix");
        bt_spawn_material(&A, A.px + 200.0f, A.py, 1);
        check(A.mat_used == 1, "material spawn extends its used prefix");
        A.con_used = 8; A.conalive[7] = 1; A.conkind[7] = BT_CONS_FRUIT;
        A.conx[7] = A.px + 100.0f; A.cony[7] = A.py;
        bt_loot_item_place(A.con_arc[7], A.conx[7], A.cony[7]);
        B = A;
        B.e_used = BT_MAX; B.pr_used = BT_MAX_PROJ;
        B.con_used = BT_MAX_CONS; B.mat_used = BT_MAX_MAT;
        B.wproj.used = BT_MAX_WPROJ; B.expl.used = BT_MAX_EXPL;
        int same = 1;
        for (int t = 0; t < 256; t++) {
            bt_core_step(&A, t % 9, oa, &ra, &ta);
            bt_core_step(&B, t % 9, ob, &rb, &tb);
            if (memcmp(oa, ob, sizeof(oa)) || memcmp(&ra, &rb, sizeof(ra)) ||
                memcmp(&ta, &tb, sizeof(ta))) same = 0;
        }
        check(same, "used-prefix and full-capacity scans produce identical output bits");
        bt_core_reset(&A);
        check(A.e_used == 0 && A.pr_used == 0 && A.con_used == 0 && A.mat_used == 0,
              "reset empties the used bounds");
        check(!A.ealive[9] && !A.edying[9] && !A.conalive[7] && !A.pralive[0],
              "reset clears the highest used and dying slots");
        for (int i = 0; i < BT_MAX; i++) {
            bt_enemy_set_alive(&A, i, (unsigned char)(i % 7 == 3));
            bt_enemy_set_dying(&A, i, (unsigned char)(i % 11 == 9 ? 2 : 0));
        }
        int ordered = 1;
        for (int mode = 1; mode <= 3; mode++) {
            int next = bt_enemy_next(&A, 0, mode);
            for (int i = 0; i < BT_MAX; i++) {
                if (!(((mode & 1) && A.ealive[i]) || ((mode & 2) && A.edying[i]))) continue;
                if (next != i) ordered = 0;
                next = bt_enemy_next(&A, i + 1, mode);
            }
            if (next != BT_MAX) ordered = 0;
        }
        check(ordered, "live/dying/union bit walks match ascending byte-flag scans across words");
        bt_enemy_set_alive(&A, BT_MAX - 1, 1);
        check(bt_enemy_next(&A, BT_MAX - 1, 1) == BT_MAX - 1,
              "the last declared enemy slot is not lost");
        bt_enemy_set_alive(&A, 31, 1); bt_enemy_set_alive(&A, 32, 1);
        check(bt_enemy_next(&A, 31, 1) == 31 && bt_enemy_next(&A, 32, 1) == 32,
              "adjacent slots across a word boundary preserve order");
    }

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
