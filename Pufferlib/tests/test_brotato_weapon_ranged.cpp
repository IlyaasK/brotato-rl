/* tests/test_brotato_weapon_ranged.cpp
 *
 * Behaviour tests for the RANGED player-weapon slice
 * (ocean/brotato/brotato_weapon_ranged.h).
 *
 * Every expectation is transcribed from the recovered sources:
 *   weapons/weapon.gd:244-266,268-291   cooldown tick + should_shoot gate
 *   weapons/weapon.gd:293-316,318-347   shoot(), reset/get_next_cooldown,
 *                                       is_big_reload_active, update_current_spread
 *   weapons/ranged/ranged_weapon.gd:239-241,413-414  attach + muzzle transform
 *   weapons/shooting_behaviors/ranged_weapon_shooting_behavior.gd:9-44
 *                                       sound pick, attack id, per-projectile
 *                                       spread loop, two recoil tweens
 *   weapons/weapon_stats/weapon_stats.gd / ranged_weapon_stats.gd  field defaults
 *   singletons/utils.gd:266-280,391-403 get_nearest_no_max / get_rand_element
 *   entities/units/player/weapons_container.gd + player.tscn attach ring
 *   entities/units/player/weapon.tscn:39-45  Range Area2D mask 5 (neutral+enemies)
 *   entities/units/neutral/neutral.tscn:12-13  trees ARE masked bodies (layer 1)
 *   projectiles/bullet_taser/taser_projectile.gd:14  SlowHitbox -200 (scene spec)
 *
 * The tests drive the module directly (bt_ranged_step + the slot hooks) so the
 * expectations are about the weapon, not about enemy AI.  They assert OBSERVABLE
 * behaviour: how many shots left the muzzle, in which direction, when, and how
 * many global RNG draws each frame consumed (never a source-text or wiring test).
 *
 * Build and run:
 *     c++ -O2 -o build/test_brotato_weapon_ranged tests/test_brotato_weapon_ranged.cpp -lm
 *     ./build/test_brotato_weapon_ranged
 *
 * Exit code 0 = all checks passed.
 */

#include "../ocean/brotato/brotato_core.h"
#include "../ocean/brotato/brotato_weapon_ranged.h"

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

static BtState A, B;   /* ~1 MiB each: keep off the stack */

/* ------------------------------------------------------------------ */
/* fixtures                                                           */
/* ------------------------------------------------------------------ */

/* Lowest-tier BT_WEAPONS row whose WeaponData id stem is `stem` and which is
 * RANGED (-1 if the schema has no such weapon). */
static int find_weapon(const char* stem) {
    int best = -1;
    for (int w = 0; w < BT_NWEAPONS; w++) {
        const char* id = BT_WEAPONS[w].id;
        if (!id) continue;
        int same = 1;
        for (int i = 0;; i++) { if (id[i] != stem[i]) { same = 0; break; } if (!id[i]) break; }
        if (!same) continue;
        if (BT_WEAPONS[w].kind != BT_WEAPON_KIND_RANGED) continue;
        if (best < 0 || BT_WEAPONS[w].tier < BT_WEAPONS[best].tier) best = w;
    }
    return best;
}
static int find_weapon_tier(const char* stem, int tier) {
    for (int w = 0; w < BT_NWEAPONS; w++) {
        const char* id = BT_WEAPONS[w].id;
        if (!id) continue;
        int same = 1;
        for (int i = 0;; i++) { if (id[i] != stem[i]) { same = 0; break; } if (!id[i]) break; }
        if (!same) continue;
        if (BT_WEAPONS[w].kind != BT_WEAPON_KIND_RANGED) continue;
        if ((int)BT_WEAPONS[w].tier == tier) return w;
    }
    return -1;
}
/* Highest-tier row of a ranged weapon family (the max-damage variant). */
static int find_weapon_top(const char* stem) {
    int best = -1;
    for (int w = 0; w < BT_NWEAPONS; w++) {
        const char* id = BT_WEAPONS[w].id;
        if (!id) continue;
        int same = 1;
        for (int i = 0;; i++) { if (id[i] != stem[i]) { same = 0; break; } if (!id[i]) break; }
        if (!same) continue;
        if (BT_WEAPONS[w].kind != BT_WEAPON_KIND_RANGED) continue;
        if (best < 0 || BT_WEAPONS[w].tier > BT_WEAPONS[best].tier) best = w;
    }
    return best;
}
static int find_kind(const char* name) {
    for (int i = 0; i < BT_NKINDS; i++) {
        const char* n = BT_KIND_NAME[i];
        if (!n) continue;
        int same = 1;
        for (int j = 0; n[j] || name[j]; j++) if (n[j] != name[j]) { same = 0; break; }
        if (same) return i;
    }
    return 0;
}

static void wipe_enemies(BtState* s) {
    for (int i = 0; i < BT_MAX; i++) {
        if (!s->ealive[i]) continue;
        if (s->e_instance[i] != BT_POOL_NO_INST) bt_enemy_pool_release(s, s->e_instance[i]);
        s->e_instance[i] = BT_POOL_NO_INST;
        bt_enemy_set_alive(s, i, 0);
        s->phys.bodies[i].active = 0;
    }
    s->ecount = 0;
}

/* Spawn one enemy exactly at (x,y) and return its slot. */
static int spawn_at(BtState* s, int kind, float x, float y) {
    if (!bt_spawn_kind(s, kind, x, y)) return -1;
    for (int i = 0; i < BT_MAX; i++)
        if (s->ealive[i] && s->ex[i] == x && s->ey[i] == y) {
            /* These fixtures drive bt_ranged_step directly, so bt_core_step's
             * per-frame Node-transform refresh (s->ehx/s->ehy = the PRE-step
             * position, brotato_state.h:749-765) never runs.  State it: the
             * module aims at the target's Node transform, which here is the
             * spawn position.  A test that cares about the lag overrides it. */
            s->ehx[i] = s->ex[i]; s->ehy[i] = s->ey[i];
            return i;
        }
    return -1;
}

static unsigned int alive_shots(const BtState* s) { return s->wproj.n_spawned; }

/* The weapon's live slot count and the (ox,oy) direction origin the module uses. */
static void origin_of(BtState* s, int slot, float* ox, float* oy) {
    unsigned short wid = s->wpn_id[slot];
    bt_ranged_origin(s, (int)s->wpn_count, slot, &BT_WEAPON_GEOM[BT_WEAPONS[wid].geom], ox, oy);
}

/* A state with exactly one ranged weapon and one enemy placed `dist` px to the
 * right of the weapon's direction origin. */
static int g_kind = -1;
static int setup_one(BtState* s, const char* stem, int tier, float dist, int enemy_kind) {
    bt_core_init(s, 20261107u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
    wipe_enemies(s);
    int w = tier < 0 ? find_weapon(stem) : find_weapon_tier(stem, tier);
    if (w < 0) return -1;
    if (bt_weapon_add(s, (unsigned short)w, 0, 0.0f) < 0) return -1;
    /* bt_weapon_add arms the slot cooldown (weapon.gd _ready ->
     * init_stats(at_wave_begin=true) -> get_next_cooldown; pinned by test 2).
     * These fixtures exercise the shot mechanics from an already-fired-out
     * weapon, so zero the tick clock (`_current_cooldown <= 0`). */
    s->wpn_cd[0] = 0.0f;
    float ox = 0.0f, oy = 0.0f;
    origin_of(s, 0, &ox, &oy);
    if (enemy_kind >= 0 && spawn_at(s, enemy_kind, ox + dist, oy) < 0) return -1;
    return w;
}

/* ------------------------------------------------------------------ */

int main(void) {
    g_kind = find_kind("chaser");
    float obs[BT_OBS_SIZE];
    float rew = 0.0f, term = 0.0f;

    int w_pistol  = find_weapon_tier("weapon_pistol", 0);
    int w_pistol4 = find_weapon_top("weapon_pistol");
    int w_rev     = find_weapon("weapon_revolver");
    int w_dbs     = find_weapon_top("weapon_double_barrel_shotgun");
    int w_chain   = find_weapon_top("weapon_chain_gun");
    int w_harp    = find_weapon("weapon_harpoon_gun");
    if (w_pistol < 0) w_pistol = find_weapon("weapon_pistol");
    if (w_pistol4 < 0) w_pistol4 = w_pistol;

    printf("weapon ranged: pistol=%d revolver=%d dbs=%d chain=%d harpoon=%d\n",
           w_pistol, w_rev, w_dbs, w_chain, w_harp);
    check(w_pistol >= 0 && w_rev >= 0 && w_dbs >= 0 && w_chain >= 0 && w_harp >= 0,
          "the canonical ranged weapon rows exist");

    /* ================================================================ */
    /* 1. Default Bull: no weapons, no shots, and the step touches nothing. */
    {
        bt_core_init(&A, 7u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        check(A.wpn_count == 0, "Bull starts with zero weapons");
        unsigned int n0 = alive_shots(&A);
        for (int t = 0; t < 120; t++) bt_ranged_step(&A, 0, 1);
        check(alive_shots(&A) == n0, "a weaponless loadout never emits a shot");
    }

    /* ================================================================ */
    /* 2. Weapon add consumes EXACTLY ONE global rand_range and arms the
     *    cooldown to get_next_cooldown(at_wave_begin=true). */
    {
        bt_core_init(&A, 11u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        wipe_enemies(&A);
        B = A;
        int slot = bt_weapon_add(&A, (unsigned short)w_pistol, 0, 0.0f);
        check(slot == 0, "the starting weapon lands in slot 0");
        BtPlayerWeaponStats st;
        check(bt_weapon_stats(&A, 0, &st), "the added weapon resolves stats");
        /* replay the single draw on the untouched snapshot */
        float expect = bt_wfx_next_cooldown(&B, st.cooldown, st.additional_cd_every,
                                            0, 0, 1, st.additional_cd_multiplier);
        check(A.rng.state == B.rng.state && A.rng.inc == B.rng.inc,
              "weapon add consumes exactly one get_next_cooldown draw");
        check_near(A.wpn_cd[0], expect, 1e-6f, "the armed cooldown is that draw");
        /* reset_cooldown clamps to the base cooldown */
        check(A.wpn_cd[0] <= (float)st.cooldown + 1e-4f,
              "the initial cooldown is clamped to the weapon cooldown");
    }

    /* ================================================================ */
    /* 3. No target in range: no shot, and no RNG consumed. */
    {
        if (setup_one(&A, "weapon_pistol", 0, 0.0f, -1) < 0) check(0, "pistol fixture");
        BtRng before = A.rng;
        for (int t = 0; t < 300; t++) bt_ranged_step(&A, 0, 1);
        check(alive_shots(&A) == 0, "no target -> no shot");
        check(A.rng.state == before.state && A.rng.inc == before.inc,
              "an idle weapon consumes no RNG draws");
        check_near(A.wpn_cd[0], 0.0f, 1e-6f, "the cooldown floors at 0 while idle");
    }

    /* ================================================================ */
    /* 4. One shot: count, muzzle position, direction, velocity, attack id,
     *    and the exact per-shot RNG draw count. */
    {
        const float dist = 200.0f;
        if (setup_one(&A, "weapon_pistol", 0, dist, g_kind) < 0) check(0, "pistol fixture");
        BtPlayerWeaponStats st;
        check(bt_weapon_stats(&A, 0, &st), "pistol stats");
        check(st.kind == BT_WEAPON_KIND_RANGED, "pistol is a ranged weapon");
        check(st.nb_projectiles == 1, "pistol fires one projectile");

        BtRng before = A.rng;
        bt_ranged_step(&A, 0, 1);
        check(alive_shots(&A) == (unsigned int)st.nb_projectiles, "one shot emits nb_projectiles");

        /* the shot consumed: 1 accuracy + 1 sound + nb_projectiles + 1 cooldown */
        B = A;   /* after-shot state; replay the draws on a fresh copy of the PRE state */
        (void)before;

        int pi = -1;
        for (int i = 0; i < BT_MAX_WPROJ; i++) if (A.wproj.p[i].alive) { pi = i; break; }
        check(pi >= 0, "the shot is live in the pool");
        if (pi >= 0) {
            BtWProj* p = &A.wproj.p[pi];
            float ox = 0.0f, oy = 0.0f, mx = 0.0f, my = 0.0f;
            origin_of(&A, 0, &ox, &oy);
            float aim = bt_atan2f(A.ey[0] - oy, A.ex[0] - ox);
            /* accuracy 0.9 on pistol tier 1: |accuracy spread| <= 0.1, and the
             * pistol's own projectile_spread is 0. */
            check_near(p->rot, aim, 0.1f + 1e-4f, "tier-1 pistol direction is aim +- (1-accuracy)");
            check_near(p->rot, aim, 0.1f + 1e-4f, "pistol spread stays inside the accuracy cone");
            bt_ranged_muzzle(&BT_WEAPON_GEOM[BT_WEAPONS[A.wpn_id[0]].geom], ox, oy,
                             p->rot == p->rot ? p->rot : 0.0f, &mx, &my);
            /* the muzzle is sampled at the weapon's rotation; a 0-spread weapon
             * would match exactly, so compare against the rotated muzzle at the
             * projectile's own rotation only for the direction check below. */
            check_near(bt_sqrtf(p->vx * p->vx + p->vy * p->vy), (float)st.projectile_speed,
                       1e-3f, "projectile speed == RangedWeaponStats.projectile_speed");
            check_near(bt_atan2f(p->vy, p->vx), p->rot, 1e-4f, "velocity direction == rotation");
            check(p->attack_id >= 1, "the shot carries the behavior attack id");
        }
        /* every projectile of a shot shares one attack id + one muzzle */
        check(A.wrng_shots[0] == 1, "nb_shots_taken advanced once");
        check(A.wrng_attack_id[0] == 2, "the attack id counter advanced once");

        /* Exact draw count: replay the source order on a fresh twin built from
         * the same seed.  bt_rand_range consumes a FIXED number of global draws
         * regardless of its bounds (GDScript rand_range), so the replay bounds
         * do not need to be the real ones. */
        bt_core_init(&B, 20261107u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        wipe_enemies(&B);
        bt_weapon_add(&B, (unsigned short)find_weapon("weapon_pistol"), 0, 0.0f);
        {
            float bx = 0.0f, by = 0.0f;
            origin_of(&B, 0, &bx, &by);
            spawn_at(&B, g_kind, bx + dist, by);
        }
        BtPlayerWeaponStats bst;
        (void)bt_weapon_stats(&B, 0, &bst);
        (void)bt_wfx_accuracy_spread(&B, bst.accuracy);          /* (1) */
        if (bst.shooting_sounds > 0)                             /* (2) */
            (void)bt_rng_bounded(&B.rng, (unsigned int)bst.shooting_sounds);
        for (int i = 0; i < bst.nb_projectiles; i++)             /* (3) */
            (void)bt_rand_range(&B, -1.0f, 1.0f);
        (void)bt_wfx_next_cooldown(&B, bst.cooldown, bst.additional_cd_every,
                                   1, 1, 0, bst.additional_cd_multiplier);   /* (4) */
        check(A.rng.state == B.rng.state && A.rng.inc == B.rng.inc,
              "one shot consumes 1 accuracy + 1 sound + nb_projectiles + 1 cooldown draws");
    }

    /* ================================================================ */
    /* 5. The fire gate.  player_run_data.gd:485 seeds
     *    can_attack_while_moving = 1, so a weapon fires WHILE MOVING by
     *    default; an effect that zeroes it (the Soldier's character effect)
     *    is the only thing that suppresses fire on the move. */
    {
        const float dist = 200.0f;
        if (setup_one(&A, "weapon_pistol", 0, dist, g_kind) < 0) check(0, "pistol fixture");
        unsigned int n0 = alive_shots(&A);
        for (int t = 0; t < 240; t++) bt_ranged_step(&A, 1, 1);   /* moving */
        check(alive_shots(&A) > n0, "a moving player still fires by default");

        /* re-arm and zero the capability: now moving must suppress fire */
        if (setup_one(&A, "weapon_pistol", 0, dist, g_kind) < 0) check(0, "pistol fixture");
        A.scalar_effect[BT_ISTR_CAN_ATTACK_WHILE_MOVING] = 0.0f;
        unsigned int m0 = alive_shots(&A);
        for (int t = 0; t < 400; t++) bt_ranged_step(&A, 1, 1);
        check(alive_shots(&A) == m0, "zero can_attack_while_moving suppresses fire on the move");
        bt_ranged_step(&A, 0, 1);
        check(alive_shots(&A) == m0 + 1, "standing still fires on the first eligible tick");
    }

    /* ================================================================ */
    /* 6. Recoil freeze: the cooldown does not tick while the two chained recoil
     *    tweens run.  The boundary is the REAL engine's, pinned per
     *    recoil_duration below (weapon_recoil_oracle.gd measures it on the
     *    pinned Godot build; test_brotato_weapon_godot_oracles.py re-checks
     *    these numbers against that engine): each tween needs
     *    floor(recoil_duration*60)+1 float32 1/60 idle steps and the second
     *    tween starts one frame after the first ends, so the freeze is 2k+1. */
    {
        struct { float d; int freeze; } engine[7] = {
            {0.01f, 3}, {0.02f, 5}, {0.05f, 7}, {0.1f, 15},
            {0.142f, 19}, {0.15f, 21}, {0.2f, 27},
        };
        for (int i = 0; i < 7; i++)
            check((int)bt_ranged_recoil_freeze_frames(engine[i].d) == engine[i].freeze,
                  "freeze matches the engine boundary for the shipped recoil_duration");
        check((int)bt_ranged_recoil_freeze_frames(0.0f) == 0,
              "a zero recoil duration freezes nothing");
    }
    {
        const float dist = 200.0f;
        if (setup_one(&A, "weapon_pistol", 0, dist, g_kind) < 0) check(0, "pistol fixture");
        BtPlayerWeaponStats st;
        (void)bt_weapon_stats(&A, 0, &st);
        bt_ranged_step(&A, 0, 1);   /* fires */
        check(A.wrng_shoot[0] == 1, "the weapon enters the shooting animation");
        float cd0 = A.wpn_cd[0];
        float freeze = bt_ranged_recoil_freeze_frames(st.recoil_duration);
        int expect = (int)freeze;
        check_near(freeze, (float)(int)freeze, 0.0f,
                   "the freeze is a whole number of frames");
        for (int t = 0; t < expect - 1; t++) {
            bt_ranged_step(&A, 1, 1);   /* move so no second shot competes */
            check_near(A.wpn_cd[0], cd0, 0.0f, "the cooldown is frozen for the freeze frames");
        }
        bt_ranged_step(&A, 1, 1);       /* the freeze frame itself */
        check(A.wrng_shoot[0] == 0, "the animation ends after `freeze` frames");
        bt_ranged_step(&A, 1, 1);       /* first counting frame */
        check_near(A.wpn_cd[0], cd0 - 1.0f, 1e-6f, "the cooldown resumes 1 tick/frame next frame");
        /* no RNG during the whole animation */
        BtRng r = A.rng;
        for (int t = 0; t < 10; t++) bt_ranged_step(&A, 1, 1);
        check(A.rng.state == r.state, "animating/cooldown frames consume no RNG");
    }

    /* ================================================================ */
    /* 6b. The SHOT's draw sequence is the same in both audio models.  The
     *     shooting-sound element pick is a gameplay draw (Array.pick_random)
     *     taken while evaluating SoundManager.play's argument
     *     (ranged_weapon_shooting_behavior.gd:10), so `audio_rng` only gates the
     *     playback pitch drawn later -- it must never remove the pick.  Same
     *     seed, same fixture, one shot each: identical stream and cooldown. */
    {
        const float dist = 200.0f;
        if (setup_one(&A, "weapon_pistol", 0, dist, g_kind) < 0) check(0, "pistol fixture");
        if (setup_one(&B, "weapon_pistol", 0, dist, g_kind) < 0) check(0, "pistol fixture");
        A.audio_rng = 1;
        B.audio_rng = 0;
        BtRng ra = A.rng, rb = B.rng;
        check(ra.state == rb.state, "both fixtures start on the same stream");
        BtPlayerWeaponStats st;
        (void)bt_weapon_stats(&A, 0, &st);
        bt_ranged_step(&A, 0, 1);
        bt_ranged_step(&B, 0, 1);
        check(ra.state != A.rng.state, "the shot consumed RNG");
        check(A.rng.state == B.rng.state,
              "one shot drains the same words with and without the audio model");
        check_near(A.wpn_cd[0], B.wpn_cd[0], 0.0f,
                   "the drawn cooldown is identical in both audio models");
        /* ...and the order is: spread(3) + sound pick(1) + one per projectile(3)
         * + cooldown(3). */
        BtRng probe = ra;
        (void)bt_rng_global_range_float(&probe, -1.0f, 1.0f);
        (void)bt_rng_bounded(&probe, (unsigned int)BT_WEAPONS[w_pistol].shoot_audio_count);
        for (int i = 0; i < (int)st.nb_projectiles; i++)
            (void)bt_rng_global_range_float(&probe, -1.0f, 1.0f);
        (void)bt_rng_global_range_float(&probe, 0.0f, 1.0f);
        check(probe.state == A.rng.state,
              "the shot draws spread, sound pick, projectiles, then cooldown");
        check((int)BT_WEAPONS[w_pistol].shoot_audio_count > 0,
              "the pistol carries a shooting sound (the pick really happens)");
    }

    /* ================================================================ */
    /* 7. Cycle length: the next shot lands `freeze + cooldown` ticks later. */
    {
        const float dist = 200.0f;
        if (setup_one(&A, "weapon_pistol", 0, dist, g_kind) < 0) check(0, "pistol fixture");
        BtPlayerWeaponStats st;
        (void)bt_weapon_stats(&A, 0, &st);
        bt_ranged_step(&A, 0, 1);
        float cd0 = A.wpn_cd[0];
        float freeze = bt_ranged_recoil_freeze_frames(st.recoil_duration);
        int ticks = 0;
        unsigned int n0 = alive_shots(&A);
        while (alive_shots(&A) == n0 && ticks < 1000) { bt_ranged_step(&A, 0, 1); ticks++; }
        check(alive_shots(&A) == n0 + 1, "the weapon fires a second time");
        /* the frame that fires is the first with cd <= 0 after the freeze */
        int expected_ticks = (int)bt_ceilf(freeze + cd0);
        check(ticks >= expected_ticks - 1 && ticks <= expected_ticks + 1,
              "the second shot lands freeze+cooldown ticks after the first");
    }

    /* ================================================================ */
    /* 8. Multishot + spread: chain_gun t4 fires 3, double_barrel t4 fires 6;
     *    every direction stays inside the spread cone and they are not all equal. */
    {
        struct { int id; int minproj; } cases[2];
        cases[0].id = w_chain; cases[0].minproj = 3;   /* chain_gun tier 4: 3 */
        cases[1].id = w_dbs;   cases[1].minproj = 4;   /* double_barrel tier 4: 4..6 */
        for (int c = 0; c < 2; c++) {
            if (cases[c].id < 0) { check(0, "multishot fixture"); continue; }
            bt_core_init(&A, 31u + (bt_u64)c, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            wipe_enemies(&A);
            bt_weapon_add(&A, (unsigned short)cases[c].id, 0, 0.0f);
            A.wpn_cd[0] = 0.0f;   /* fired out: fire on the first tick */
            float ox = 0.0f, oy = 0.0f;
            origin_of(&A, 0, &ox, &oy);
            spawn_at(&A, g_kind, ox + 150.0f, oy);
            BtPlayerWeaponStats st;
            (void)bt_weapon_stats(&A, 0, &st);
            check(st.nb_projectiles >= cases[c].minproj,
                  "the schema's top tier is a real multishot weapon");
            unsigned int n0 = alive_shots(&A);
            bt_ranged_step(&A, 0, 1);
            check(alive_shots(&A) - n0 == (unsigned int)st.nb_projectiles,
                  "the shot emits exactly nb_projectiles");
            float aim = bt_atan2f(A.ey[0] - oy, A.ex[0] - ox);
            int distinct = 0;
            float first = 1e9f;
            for (int i = 0; i < BT_MAX_WPROJ; i++) {
                BtWProj* p = &A.wproj.p[i];
                if (!p->alive) continue;
                float d = p->rot - aim;
                check(bt_fabsf(d) <= st.projectile_spread + bt_fabsf(1.0f - st.accuracy) + 1e-4f,
                      "each projectile stays inside the accuracy+spread cone");
                if (first > 1e8f) first = p->rot;
                else if (bt_fabsf(p->rot - first) > 1e-3f) distinct = 1;
            }
            check(distinct, "a multishot weapon emits projectiles in different directions");
        }
    }

    /* ================================================================ */
    /* 9. Direction and muzzle for a zero-spread, perfect-accuracy weapon. */
    {
        bt_core_init(&A, 41u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        wipe_enemies(&A);
        bt_weapon_add(&A, (unsigned short)w_pistol4, 0, 0.0f);
        A.wpn_cd[0] = 0.0f;   /* fired out: fire on the first tick */
        BtPlayerWeaponStats st;
        (void)bt_weapon_stats(&A, 0, &st);
        check_near(st.accuracy, 1.0f, 1e-6f, "pistol tier 4 has accuracy 1.0");
        check_near(st.projectile_spread, 0.0f, 1e-6f, "pistol tier 4 has no projectile spread");
        float ox = 0.0f, oy = 0.0f;
        origin_of(&A, 0, &ox, &oy);
        /* place the enemy up and to the right so the angle is not axis aligned */
        spawn_at(&A, g_kind, ox + 120.0f, oy - 90.0f);
        bt_ranged_step(&A, 0, 1);
        float aim = bt_atan2f(A.ey[0] - oy, A.ex[0] - ox);
        float mx = 0.0f, my = 0.0f;
        bt_ranged_muzzle(&BT_WEAPON_GEOM[BT_WEAPONS[A.wpn_id[0]].geom], ox, oy, aim, &mx, &my);
        int found = 0;
        for (int i = 0; i < BT_MAX_WPROJ; i++) {
            BtWProj* p = &A.wproj.p[i];
            if (!p->alive) continue;
            found = 1;
            check_near(p->rot, aim, 1e-4f, "perfect accuracy -> rotation == aim exactly");
            check_near(p->x, mx, 1e-3f, "the shot leaves the muzzle's x");
            check_near(p->y, my, 1e-3f, "the shot leaves the muzzle's y");
            check_near(bt_atan2f(p->vy, p->vx), aim, 1e-4f, "velocity == aim");
        }
        check(found, "the shot was emitted");
    }

    /* ================================================================ */
    /* 10. min_range / max_range+50 band boundaries. */
    {
        /* harpoon_gun: min_range 300, max_range 800 (canonical lowest tier 1). */
        BtPlayerWeaponStats st;
        if (setup_one(&A, "weapon_harpoon_gun", -1, 0.0f, -1) < 0) check(0, "harpoon fixture");
        (void)bt_weapon_stats(&A, 0, &st);
        check(st.min_range > 0, "harpoon_gun has a minimum range");
        float ox = 0.0f, oy = 0.0f;
        origin_of(&A, 0, &ox, &oy);
        /* inside min_range: no target -> no shot even after a long idle */
        spawn_at(&A, g_kind, ox + (float)st.min_range - 20.0f, oy);
        for (int t = 0; t < 200; t++) bt_ranged_step(&A, 0, 1);
        check(alive_shots(&A) == 0, "a target inside min_range is not shot");
        /* move it just past min_range */
        wipe_enemies(&A);
        spawn_at(&A, g_kind, ox + (float)st.min_range + 1.0f, oy);
        bt_ranged_step(&A, 0, 1);
        check(alive_shots(&A) == 1, "a target just past min_range is shot");
        /* beyond max_range + 50: no shot (pistol, max_range 400) */
        bt_core_init(&A, 43u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        wipe_enemies(&A);
        bt_weapon_add(&A, (unsigned short)w_pistol, 0, 0.0f);
        (void)bt_weapon_stats(&A, 0, &st);
        origin_of(&A, 0, &ox, &oy);
        spawn_at(&A, g_kind, ox + (float)st.max_range + 51.0f, oy);
        for (int t = 0; t < 200; t++) bt_ranged_step(&A, 0, 1);
        check(alive_shots(&A) == 0, "a target beyond max_range+50 is not shot");
        wipe_enemies(&A);
        spawn_at(&A, g_kind, ox + (float)st.max_range + 49.0f, oy);
        bt_ranged_step(&A, 0, 1);
        check(alive_shots(&A) == 1, "a target inside max_range+50 is shot");
    }

    /* ================================================================ */
    /* 11. Neutral trees are targetable: the Range Area2D mask is neutral+enemy. */
    {
        bt_core_init(&A, 47u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        wipe_enemies(&A);
        bt_weapon_add(&A, (unsigned short)w_pistol, 0, 0.0f);
        A.wpn_cd[0] = 0.0f;   /* fired out: fire on the first tick */
        float ox = 0.0f, oy = 0.0f;
        origin_of(&A, 0, &ox, &oy);
        int tree = spawn_at(&A, 0, ox + 150.0f, oy);   /* neutral kind 0 */
        check(tree >= 0, "a neutral tree was spawned");
        if (tree >= 0) { A.eneutral[tree] = 1; }
        bt_ranged_step(&A, 0, 1);
        check(alive_shots(&A) == 1, "a weapon fires at a neutral tree in range");
    }

    /* ================================================================ */
    /* 12. Big reload: the revolver's every-x-th shot takes cooldown*multiplier
     *     and consumes no cooldown draw at all. */
    {
        const float dist = 200.0f;
        if (setup_one(&A, "weapon_revolver", 0, dist, g_kind) < 0) check(0, "revolver fixture");
        BtPlayerWeaponStats st;
        (void)bt_weapon_stats(&A, 0, &st);
        check(st.additional_cd_every > 0, "revolver has additional_cooldown_every_x_shots");
        float mult = st.additional_cd_multiplier;
        check(mult > 1.0f, "revolver's additional cooldown multiplier is > 1");
        int every = st.additional_cd_every;
        for (int shot = 1; shot <= every; shot++) {
            if (shot == every) B = A;   /* snapshot the state just before the long shot */
            unsigned int n0 = alive_shots(&A);
            int guard = 0;
            while (alive_shots(&A) == n0 && guard < 4000) { bt_ranged_step(&A, 0, 1); guard++; }
            check(alive_shots(&A) == n0 + 1, "the revolver keeps firing");
            if (shot < every)
                check(bt_fabsf(A.wpn_cd[0] - (float)st.cooldown * mult) > 1e-3f,
                      "a normal shot does not take the long reload");
        }
        check_near(A.wpn_cd[0], (float)st.cooldown * mult, 1e-4f,
                   "the every-x-th shot takes cooldown*multiplier");
        /* Replay only the non-cooldown draws of that shot on the pre-shot twin:
         * get_next_cooldown takes the big-reload branch and draws NOTHING. */
        (void)bt_wfx_accuracy_spread(&B, st.accuracy);
        if (st.shooting_sounds > 0) (void)bt_rng_bounded(&B.rng, (unsigned int)st.shooting_sounds);
        for (int i = 0; i < st.nb_projectiles; i++) (void)bt_rand_range(&B, -1.0f, 1.0f);
        check(A.rng.state == B.rng.state && A.rng.inc == B.rng.inc,
              "the big-reload shot consumes no cooldown draw");
    }

    /* ================================================================ */
    /* 13. Dense compaction: removing slot 0 moves the survivor's firing state. */
    {
        bt_core_init(&A, 53u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        wipe_enemies(&A);
        bt_weapon_add(&A, (unsigned short)w_pistol, 0, 0.0f);
        bt_weapon_add(&A, (unsigned short)w_rev, 0, 0.0f);
        check(A.wpn_count == 2, "two weapons equipped");
        float ox = 0.0f, oy = 0.0f;
        origin_of(&A, 1, &ox, &oy);
        spawn_at(&A, g_kind, ox + 150.0f, oy);
        /* fire the second weapon once */
        for (int t = 0; t < 20 && A.wrng_shots[1] == 0; t++) bt_ranged_step(&A, 0, 1);
        check(A.wrng_shots[1] == 1, "the second weapon fired");
        int shots_before = A.wrng_shots[1];
        int aid_before = A.wrng_attack_id[1];
        float cd_before = A.wpn_cd[1];
        unsigned short id_before = A.wpn_id[1];
        check(bt_weapon_remove(&A, 0) >= 0, "slot 0 removed");
        check(A.wpn_count == 1, "one weapon left");
        check(A.wpn_id[0] == id_before, "the survivor shifted into slot 0");
        check_near(A.wpn_cd[0], cd_before, 1e-6f, "the survivor kept its cooldown");
        check(A.wrng_shots[0] == shots_before, "the survivor kept nb_shots_taken");
        check(A.wrng_attack_id[0] == aid_before, "the survivor kept its attack-id counter");
    }

    /* ================================================================ */
    /* 14. Determinism: identical seeds and steps give identical shots. */
    {
        bt_core_init(&A, 97u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        wipe_enemies(&A);
        bt_weapon_add(&A, (unsigned short)w_dbs, 0, 0.0f);
        float ox = 0.0f, oy = 0.0f;
        origin_of(&A, 0, &ox, &oy);
        spawn_at(&A, g_kind, ox + 160.0f, oy + 30.0f);
        B = A;
        for (int t = 0; t < 400; t++) { bt_ranged_step(&A, (t / 37) % 2, 1); bt_ranged_step(&B, (t / 37) % 2, 1); }
        check(alive_shots(&A) == alive_shots(&B), "identical runs emit the same number of shots");
        check_near(A.wpn_cd[0], B.wpn_cd[0], 0.0f, "identical runs end on the same cooldown");
        int same = 1;
        for (int i = 0; i < BT_MAX_WPROJ; i++) {
            if (A.wproj.p[i].alive != B.wproj.p[i].alive) same = 0;
            if (A.wproj.p[i].alive && bt_fabsf(A.wproj.p[i].rot - B.wproj.p[i].rot) > 0.0f) same = 0;
        }
        check(same, "identical runs emit identical live shots");
    }

    /* ================================================================ */
    /* 15. SlowHitbox (taser / harpoon gun): the only runtime effect of a
     *     SlowInZone weapon row is the projectile scene's second area
     *     (projectiles/bullet_taser/taser_projectile.gd:14 -> -200; the DLC
     *     harpoon scene -> -400).  SlowInZoneEffect itself is a NullEffect, so
     *     the ranged slice must carry the SCENE's slow_rad/slow_amt onto the
     *     shot (bt_wproj_slow then applies it via the shared bt_add_decaying_speed). */
    {
        BtPlayerWeaponStats st;
        if (setup_one(&A, "weapon_harpoon_gun", -1, 400.0f, g_kind) < 0) check(0, "harpoon fixture");
        (void)bt_weapon_stats(&A, 0, &st);
        check((int)st.min_range < 400 && 400 <= (int)st.max_range,
              "the harpoon target sits inside its [min_range, max_range] band");
        int guard = 0;
        while (alive_shots(&A) == 0 && guard < 4000) { bt_ranged_step(&A, 0, 1); guard++; }
        check(alive_shots(&A) >= 1, "the harpoon fired");
        int slow_shots = 0;
        for (int i = 0; i < BT_MAX_WPROJ; i++) {
            BtWProj* p = &A.wproj.p[i];
            if (!p->alive || !(p->flags & BT_WPROJ_F_SLOW)) continue;
            slow_shots++;
            check(p->slow_rad > 0.0f, "a slow shot carries the scene's SlowHitbox radius");
            check_near(p->slow_amt, -400.0f, 0.0f, "the harpoon SlowHitbox amount is the scene's -400");
        }
        check(slow_shots >= 1, "the harpoon's fired shot is a slow hitbox");
    }

    /* ================================================================ */
    /* 16. Wave begin (w > 1): a fresh game scene per wave re-creates every
     *     player Weapon node from the persistent loadout
     *     (base_shop.gd:345-346 -> get_tree().change_scene(MenuData.game_scene);
     *     player.gd:315-320 apply_items_effects -> add_weapon -> weapon.gd:69-88
     *     _ready -> init_stats(at_wave_begin=true) -> get_next_cooldown(true)),
     *     so every ranged copy draws ONE fresh cooldown, in slot order, with
     *     weapon_count == slot (player.get_nb_weapons() counts only the nodes
     *     already created) and its runtime state reset. */
    {
        bt_core_init(&A, 31u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        wipe_enemies(&A);
        check(bt_weapon_add(&A, (unsigned short)w_pistol, 0, 0.0f) == 0, "slot 0 pistol");
        check(bt_weapon_add(&A, (unsigned short)w_rev, 0, 0.0f) == 1, "slot 1 revolver");
        check(bt_weapon_add(&A, (unsigned short)w_chain, 0, 0.0f) == 2, "slot 2 chain gun");
        /* dirty the per-copy runtime state so the reset is observable */
        A.wrng_shots[0] = 7; A.wrng_shoot[0] = 1; A.wrng_attack_id[0] = 9;
        A.wrng_spread[0] = 0.5f; A.wrng_anim[0] = 3.0f; A.wrng_phase[0] = 1;
        B = A;
        bt_weapon_wave_begin(&A);
        for (int sl = 0; sl < 3; sl++) {
            BtPlayerWeaponStats st;
            check(bt_weapon_stats(&B, sl, &st), "the snapshot resolves slot stats");
            float want = bt_wfx_next_cooldown(&B, st.cooldown, st.additional_cd_every,
                                              0, sl, 1, st.additional_cd_multiplier);
            float mult = (st.additional_cd_every != -1 && st.additional_cd_multiplier > 0.0f)
                         ? st.additional_cd_multiplier : 1.0f;
            float cap = (float)st.cooldown * mult;
            if (want > cap) want = cap;   /* reset_cooldown() clamp, weapon.gd:311-313 */
            check_near(A.wpn_cd[sl], want, 0.0f,
                       "wave-begin cooldown = min(get_next_cooldown(true, count=slot), cd*mult)");
        }
        check(A.rng.state == B.rng.state && A.rng.inc == B.rng.inc,
              "exactly one get_next_cooldown draw per ranged slot, in slot order");
        check(A.wrng_shots[0] == 0 && A.wrng_shoot[0] == 0 && A.wrng_attack_id[0] == 1 &&
              A.wrng_spread[0] == 0.0f && A.wrng_anim[0] == 0.0f && A.wrng_phase[0] == 0,
              "the re-created copy's runtime state is cleared");
        /* a weaponless / melee-only loadout draws nothing */
        BtState* s = &B;
        bt_core_init(s, 33u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        BtRng r0 = s->rng;
        bt_weapon_wave_begin(s);
        check(s->rng.state == r0.state, "no weapons -> no wave-begin draw");
    }

    /* ================================================================ */
    /* 17. Target detection is the Range Area2D's circle-vs-BODY-circle overlap
     *     (weapon.tscn:39-45 mask 5 = neutral + enemies, radius max_range + 200
     *     set at weapon.gd:155), NOT a centre-distance test; the SELECTED target
     *     is still the nearest by centre distance among the candidates
     *     (utils.gd:266-280, strict `> min_range^2`). */
    {
        if (setup_one(&A, "weapon_pistol", 0, 0.0f, -1) < 0) check(0, "pistol fixture");
        BtPlayerWeaponStats st;
        (void)bt_weapon_stats(&A, 0, &st);
        float ox = 0.0f, oy = 0.0f;
        origin_of(&A, 0, &ox, &oy);
        float detect = (float)st.max_range + 200.0f;
        /* a body whose circle clips the Range circle while its centre lies
         * OUTSIDE it: acquired (aimed at), but the max_range + 50 fire band
         * refuses the shot. */
        int j = spawn_at(&A, g_kind, 0.0f, 0.0f);
        check(j >= 0, "enemy placed");
        BT_CONST_PTR(BtBodyPhysics) G = bt_kgeom(&A, j);
        float cx = ox + detect + G->body_radius - 2.0f;     /* wanted body-circle centre x */
        A.ex[j] = cx - G->body_x;
        A.ey[j] = oy - G->body_y;
        A.ehx[j] = A.ex[j]; A.ehy[j] = A.ey[j];   /* this test is about detection, not the lag */
        A.wrng_rot[0] = 123.0f;
        bt_ranged_step(&A, 0, 1);
        check_near(A.wrng_rot[0], bt_atan2f(A.ehy[j] - oy, A.ehx[j] - ox), 1e-6f,
                   "a body-clipping target beyond detect_range is still acquired");
        check(alive_shots(&A) == 0, "…but the max_range + 50 band refuses the shot");
        /* beyond detect_range + body_radius: no overlap -> not a candidate */
        A.ex[j] = ox + detect + G->body_radius + 5.0f - G->body_x;
        A.ey[j] = oy - G->body_y;
        A.ehx[j] = A.ex[j]; A.ehy[j] = A.ey[j];
        A.wrng_rot[0] = 123.0f;
        bt_ranged_step(&A, 0, 1);
        check_near(A.wrng_rot[0], 0.0f, 0.0f,
                   "a body fully outside the Range circle is not a candidate (idle)");
        /* the min_range gate still measures the CENTRE distance (harpoon 300) */
        BtState* s = &B;
        if (setup_one(s, "weapon_harpoon_gun", -1, 0.0f, -1) >= 0) {
            BtPlayerWeaponStats hs;
            (void)bt_weapon_stats(s, 0, &hs);
            float hx = 0.0f, hy = 0.0f;
            origin_of(s, 0, &hx, &hy);
            int hj = spawn_at(s, g_kind, hx + 250.0f, hy);   /* inside min_range 300 */
            check(hj >= 0, "an enemy inside min_range");
            check(s->ex[hj] - hx < (float)hs.min_range, "…is strictly closer than min_range");
            s->wrng_rot[0] = 123.0f;
            bt_ranged_step(s, 0, 1);
            check_near(s->wrng_rot[0], 0.0f, 0.0f,
                       "a target inside min_range is rejected by the centre-distance gate");
        }
    }

    /* ================================================================ */
    /* 18. The aim angle is measured from the target's LAGGED Node transform.
     *     weapon.gd:243-262 computes `(_current_target[0].global_position -
     *     global_position).angle()`, i.e. the target's Node transform; a
     *     MODE_CHARACTER enemy's Node transform is one physics step behind the
     *     integrated body (brotato_state.h:749-765), so the aim reads
     *     s->ehx/s->ehy, not this frame's s->ex/s->ey.  The two are put 80 px
     *     apart, far outside the accuracy cone, so the shot direction pins the
     *     SAMPLE. */
    {
        if (setup_one(&A, "weapon_pistol", 0, 200.0f, g_kind) < 0) check(0, "pistol fixture");
        BtPlayerWeaponStats st;
        (void)bt_weapon_stats(&A, 0, &st);
        float ox = 0.0f, oy = 0.0f;
        origin_of(&A, 0, &ox, &oy);
        A.ehx[0] = A.ex[0]; A.ehy[0] = A.ey[0] - 80.0f;   /* the Node transform, one step behind */
        float aim_lag = bt_atan2f(A.ehy[0] - oy, A.ehx[0] - ox);
        float aim_body = bt_atan2f(A.ey[0] - oy, A.ex[0] - ox);
        check(bt_fabsf(aim_lag - aim_body) > 2.0f * (1.0f - (float)st.accuracy) + 0.05f,
              "fixture separates the lagged and integrated aim targets");
        A.wpn_cd[0] = 0.0f;
        bt_ranged_step(&A, 0, 1);
        check(alive_shots(&A) == 1u, "the lag fixture fires");
        int li = -1;
        for (int i = 0; i < BT_MAX_WPROJ; i++) if (A.wproj.p[i].alive) { li = i; break; }
        if (li >= 0) {
            check_near(A.wproj.p[li].rot, aim_lag, 0.1f + 1e-4f,
                       "the shot is aimed at the target's LAGGED Node transform");
            check(bt_fabsf(A.wproj.p[li].rot - aim_body) > 0.1f,
                  "and NOT at this frame's integrated body position");
        }
        /* the live rotation shown while the weapon is mid-shot follows the same
         * sample (weapon.gd:257-262 get_direction) */
        A.wrng_rot[0] = 123.0f;
        bt_ranged_step(&A, 0, 1);
        check_near(A.wrng_rot[0], aim_lag + A.wrng_spread[0], 1e-6f,
                   "the recorded shot direction also follows the lagged target");
    }

    /* ================================================================ */
    /* 19. The target SELECTION sample and the fire-band narrowing.
     *
     *     (a) `get_direction_and_calculate_target` measures
     *         `target.global_position` (utils.gd:270), the target's NODE
     *         transform -- one physics step behind the integrated body -- so
     *         BOTH the picked target and its distance come from s->ehx/s->ehy.
     *         A target whose Node transform lies outside the band must NOT be
     *         shot just because the freshly integrated body is inside it.
     *     (b) `should_shoot` gates with
     *         `Utils.is_between(_current_target[1], min_range, max_range + 50)`
     *         and `is_between(number: int, ...)` (utils.gd:298) NARROWS the
     *         float distance toward zero, so a centre distance of
     *         max_range + 50.something still fires while max_range + 51 does
     *         not.  Verified against the pinned engine:
     *         is_between(350.639496, 0, 350) == true, (351.4) == false. */
    {
        BtPlayerWeaponStats st;
        float ox = 0.0f, oy = 0.0f;
        float band = 0.0f;   /* = max_range + 50, set per fixture below */

        /* (a) Node transform outside the band, integrated body inside it. */
        if (setup_one(&A, "weapon_pistol", 0, 0.0f, -1) < 0) check(0, "pistol fixture");
        (void)bt_weapon_stats(&A, 0, &st);
        band = (float)st.max_range + 50.0f;
        origin_of(&A, 0, &ox, &oy);
        int j = spawn_at(&A, g_kind, ox + band + 12.0f, oy);   /* centre just past the band */
        check(j >= 0, "lag fixture: enemy placed");
        A.ehx[j] = ox + band + 12.0f;  A.ehy[j] = oy;          /* Node transform */
        A.ex[j]  = ox + band - 12.0f;  A.ey[j]  = oy;          /* integrated body */
        unsigned int n0 = alive_shots(&A);
        bt_ranged_step(&A, 0, 1);
        check(alive_shots(&A) == n0,
              "a target whose Node transform is outside max_range+50 is not shot "
              "(the distance reads s->ehx/s->ehy)");
        /* the integrated body inside the band does not rescue it */
        check(A.ex[j] - ox < band, "fixture: the integrated body IS inside the band");

        /* (b) the narrowing: a centre distance of band + 0.6 still fires. */
        if (setup_one(&A, "weapon_pistol", 0, 0.0f, -1) < 0) check(0, "pistol fixture");
        (void)bt_weapon_stats(&A, 0, &st);
        band = (float)st.max_range + 50.0f;
        origin_of(&A, 0, &ox, &oy);
        j = spawn_at(&A, g_kind, ox + band + 0.6f, oy);
        check(j >= 0, "narrow fixture: enemy placed");
        A.ehx[j] = A.ex[j]; A.ehy[j] = A.ey[j];
        n0 = alive_shots(&A);
        bt_ranged_step(&A, 0, 1);
        check(A.ehx[j] - ox > band,
              "fixture: the centre distance is strictly greater than max_range+50");
        check(alive_shots(&A) == n0 + 1,
              "…but int(distance) == max_range+50 still fires (is_between narrows)");

        /* a full pixel past the band does NOT fire */
        if (setup_one(&A, "weapon_pistol", 0, 0.0f, -1) < 0) check(0, "pistol fixture");
        (void)bt_weapon_stats(&A, 0, &st);
        band = (float)st.max_range + 50.0f;
        origin_of(&A, 0, &ox, &oy);
        j = spawn_at(&A, g_kind, ox + band + 1.6f, oy);
        check(j >= 0, "narrow fixture: enemy placed");
        A.ehx[j] = A.ex[j]; A.ehy[j] = A.ey[j];
        n0 = alive_shots(&A);
        bt_ranged_step(&A, 0, 1);
        check(alive_shots(&A) == n0,
              "a centre distance of max_range+51 does not fire");
    }

    (void)obs; (void)rew; (void)term;
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
