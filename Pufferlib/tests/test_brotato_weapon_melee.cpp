/* tests/test_brotato_weapon_melee.cpp
 *
 * Deterministic regression tests for the player MELEE weapon slice
 * (ocean/brotato/brotato_weapon_melee.h): the swing state machine + Godot Tween
 * easing, the animated oriented-rectangle hitbox, per-swing per-target hit
 * gating, multi-target swings, the THRUST/SWEEP alternation and the cooldown
 * cadence.
 *
 * Self-contained: includes only the core header (which pulls in the weapon
 * modules) and <stdio.h>.
 *
 * Build and run:
 *     c++ -O2 -o build/test_brotato_weapon_melee tests/test_brotato_weapon_melee.cpp -lm
 *     ./build/test_brotato_weapon_melee
 *
 * Exit code 0 = all checks passed.
 *
 * Expectations are transcribed from the recovered sources:
 *   weapons/weapon.gd                                       cooldown / should_shoot
 *   weapons/shooting_behaviors/melee_weapon_shooting_behavior.gd  tween segments
 *   weapons/shooting_behaviors/melee_shooting_data.gd             durations
 *   weapons/melee/melee_weapon.gd                           thrust/sweep alternation
 *   entities/units/unit/unit.gd                             hurt_area_entered / take_damage
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

/* BtState is large; keep it in static storage, never on the stack. */
static BtState A;
static BtState P;   /* scratch state for table-only probes */

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

static unsigned short wid_of(const char* my_id) { return bt_weapon_id_from_name(my_id); }

/* Clean deterministic world: Bull (no weapons), no wave groups, centred player. */
static void reset_world(void) {
    bt_core_init(&A, 12345u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
    A.wave = 1;
    A.n_live_groups = 0;
    A.time_left = 1.0e9f;
    for (int i = 0; i < BT_MAX; i++) bt_enemy_set_alive(&A, i, 0);
    A.ecount = 0;
    A.px = 1024.0f; A.py = 768.0f;
}

/* Spawn `kind` so its Hurtbox circle centre lands exactly on (hx,hy). */
static void place_hurt(int slot, int kind, float hx, float hy) {
    bt_spawn_kind_at(&A, slot, kind, hx, hy, 0);
    BT_CONST_PTR(BtBodyPhysics) G = bt_kgeom(&A, slot);
    A.ex[slot] = hx - G->hurt_x;
    A.ey[slot] = hy - G->hurt_y;
}

/* Weapon origin for a single-slot loadout (attach layout "1 weapon"). */
#define MELEE_WX(s) ((s)->px)
#define MELEE_WY(s) ((s)->py + 6.0f)

/* =========================================================================
 * 1. Swing-animation state machine (bt_melee_anim) -- source timing.
 * ========================================================================= */
static void set_swing(int slot, float t, float rec, float atk, float back,
                      float recoil, float maxrange, int ddr, float rot) {
    A.wpn_melee_t[slot] = t;
    A.wpn_melee_recoil_dur[slot] = rec;
    A.wpn_melee_atk_dur[slot] = atk;
    A.wpn_melee_back_dur[slot] = back;
    A.wpn_melee_recoil[slot] = recoil;
    A.wpn_melee_maxrange[slot] = maxrange;
    A.wpn_melee_ddr[slot] = (unsigned char)ddr;
    A.wpn_melee_rot[slot] = rot;
}

static void test_thrust_animation(void) {
    unsigned short wid = wid_of("weapon_spear_1");
    BT_CONST_PTR(BtWeaponGeom) G = &BT_WEAPON_GEOM[BT_WEAPONS[wid].geom];
    float sx = G->sprite_x, sy = G->sprite_y;
    float rec = 0.1f, atk = 0.3f, back = 0.2f, recoil = 25.0f, mrange = 350.0f;
    /* frame boundaries (source tween stepping + one yield-resume frame) */
    int rec_f = bt_melee_frames(rec);           /* 6  */
    int out_f = bt_melee_frames(atk * 0.5f);    /* 9  */
    int hit_start = rec_f + 1;                  /* 7  */
    int hit_end = hit_start + out_f + 1;        /* 17 */

    float px, py, rot; int hit, phase;

    set_swing(0, 0.0f, rec, atk, back, recoil, mrange, 0, 0.0f);
    bt_melee_anim(&A, 0, BT_MELEE_ANIM_THRUST, G, &px, &py, &rot, &hit, &phase);
    check_near(px, sx, 1e-4f, "thrust t=0 sprite x = initial");
    check_near(py, sy, 1e-4f, "thrust t=0 sprite y = initial");
    check(hit == 0 && phase == 1, "thrust t=0 is recoil, hitbox off");

    set_swing(0, (float)rec_f, rec, atk, back, recoil, mrange, 0, 0.0f);
    bt_melee_anim(&A, 0, BT_MELEE_ANIM_THRUST, G, &px, &py, &rot, &hit, &phase);
    check_near(px, sx - recoil, 1e-3f, "thrust recoil done holds at sx - recoil");
    check(hit == 0 && phase == 1, "thrust recoil done: hitbox still off (await yield)");

    set_swing(0, (float)hit_start, rec, atk, back, recoil, mrange, 0, 0.0f);
    bt_melee_anim(&A, 0, BT_MELEE_ANIM_THRUST, G, &px, &py, &rot, &hit, &phase);
    check_near(px, sx - recoil, 1e-3f, "thrust out start = recoil end");
    check(hit == 1 && phase == 2, "thrust out start: hitbox on");

    set_swing(0, (float)(hit_start + out_f), rec, atk, back, recoil, mrange, 0, 0.0f);
    bt_melee_anim(&A, 0, BT_MELEE_ANIM_THRUST, G, &px, &py, &rot, &hit, &phase);
    check_near(px, sx + mrange, 1e-2f, "thrust out end reaches max_range");
    check(hit == 1 && phase == 2, "thrust out end still in hit window");

    set_swing(0, (float)(hit_end + 3), rec, atk, back, recoil, mrange, 0, 0.0f);
    bt_melee_anim(&A, 0, BT_MELEE_ANIM_THRUST, G, &px, &py, &rot, &hit, &phase);
    check(hit == 0 && phase == 3, "thrust return (no ddr): hitbox off");

    set_swing(0, (float)(hit_end + 3), rec, atk, back, recoil, mrange, 1, 0.0f);
    bt_melee_anim(&A, 0, BT_MELEE_ANIM_THRUST, G, &px, &py, &rot, &hit, &phase);
    check(hit == 1 && phase == 3, "thrust return (ddr): hitbox on");
}

static void test_sweep_animation(void) {
    unsigned short wid = wid_of("weapon_hatchet_1");
    BT_CONST_PTR(BtWeaponGeom) G = &BT_WEAPON_GEOM[BT_WEAPONS[wid].geom];
    float sx = G->sprite_x, sy = G->sprite_y;
    float rec = 0.1f, atk = 0.4f, back = 0.2f, recoil = 25.0f;
    float sweep_angle = BT_WEAPON_MELEE_SWEEP_ANGLE;
    float atk_distance = 125.0f;
    float side_range = atk_distance * 0.5f;
    int rec_f = bt_melee_frames(rec);                 /* 6 */
    int shd_f = bt_melee_frames(atk / (float)BT_WEAPON_MELEE_SWEEP_HALF_DIVISOR); /* 6 */
    int hit_start = rec_f + 1;                        /* 7 */
    int out2_start = hit_start + shd_f + 1;           /* 14 */
    int hit_end = out2_start + shd_f + 1;             /* 21 */
    A.wpn_melee_atkdist[0] = atk_distance;
    float x_out = sx + (float)(int)atk_distance * 0.75f;
    /* facing right (rot 0): side_a = -side_range, angle_a = -0.9PI */
    float side_a = -side_range, angle_a = -sweep_angle;
    float side_b = side_range, angle_b = sweep_angle;

    float px, py, rot; int hit, phase;

    set_swing(0, (float)hit_start, rec, atk, back, recoil, 125.0f, 0, 0.0f);
    bt_melee_anim(&A, 0, BT_MELEE_ANIM_SWEEP, G, &px, &py, &rot, &hit, &phase);
    check_near(px, sx - recoil, 1e-3f, "sweep out1 start x = recoil end");
    check_near(py, sy + side_a, 1e-3f, "sweep out1 start y = sy + side_a");
    check_near(rot, angle_a, 1e-3f, "sweep out1 start rotation = -0.9PI");
    check(hit == 1 && phase == 2, "sweep out1 start: hitbox on");

    set_swing(0, (float)(out2_start - 1), rec, atk, back, recoil, 125.0f, 0, 0.0f);
    bt_melee_anim(&A, 0, BT_MELEE_ANIM_SWEEP, G, &px, &py, &rot, &hit, &phase);
    check_near(px, x_out, 1e-2f, "sweep out1 end x = sx + 0.75*range");
    check_near(py, sy, 1e-2f, "sweep out1 end y = sy");
    check_near(rot, 0.0f, 1e-2f, "sweep out1 end rotation = 0");

    set_swing(0, (float)out2_start, rec, atk, back, recoil, 125.0f, 0, 0.0f);
    bt_melee_anim(&A, 0, BT_MELEE_ANIM_SWEEP, G, &px, &py, &rot, &hit, &phase);
    check_near(rot, 0.0f, 1e-3f, "sweep out2 start rotation = 0");
    check(hit == 1 && phase == 3, "sweep out2 start: hitbox on");

    set_swing(0, (float)hit_end, rec, atk, back, recoil, 125.0f, 0, 0.0f);
    bt_melee_anim(&A, 0, BT_MELEE_ANIM_SWEEP, G, &px, &py, &rot, &hit, &phase);
    check_near(py, sy + side_b, 1e-3f, "sweep out2 end y = sy + side_b");
    check_near(rot, angle_b, 1e-3f, "sweep out2 end rotation = +0.9PI");
    check(hit == 0 && phase == 4, "sweep return (no ddr): hitbox off");

    set_swing(0, (float)hit_end, rec, atk, back, recoil, 125.0f, 1, 0.0f);
    bt_melee_anim(&A, 0, BT_MELEE_ANIM_SWEEP, G, &px, &py, &rot, &hit, &phase);
    check(hit == 1 && phase == 4, "sweep return (ddr): hitbox on");
}

/* =========================================================================
 * 2. Weapon init / dense-compaction runtime hooks.
 * ========================================================================= */
static void test_slot_hooks(void) {
    reset_world();
    unsigned short hatchet = wid_of("weapon_hatchet_1");   /* SWEEP */
    unsigned short spear = wid_of("weapon_spear_1");       /* THRUST */
    check(bt_weapon_kind(hatchet) == BT_WEAPON_KIND_MELEE, "hatchet is melee");
    check(bt_weapon_kind(spear) == BT_WEAPON_KIND_MELEE, "spear is melee");

    int s0 = bt_weapon_add(&A, hatchet, 0, 0.0f);
    int s1 = bt_weapon_add(&A, spear, 0, 0.0f);
    check(s0 == 0 && s1 == 1 && bt_weapon_count(&A) == 2, "two melee weapons added densely");
    check(A.wpn_melee_atk[0] == BT_MELEE_SWEEP, "hatchet next_attack_type = SWEEP");
    check(A.wpn_melee_atk[1] == BT_MELEE_THRUST, "spear next_attack_type = THRUST");
    check(A.wpn_cd[0] > 0.0f && A.wpn_cd[1] > 0.0f, "initial cooldown drawn by slot_reset");
    check(A.wpn_melee_anim[0] == BT_MELEE_ANIM_IDLE && A.wpn_melee_shooting[0] == 0,
          "runtime starts idle");

    /* Erase slot 0: the spear's runtime must follow it down (dense compaction),
     * and the vacated tail slot must be cleared, never smeared. */
    bt_weapon_erase(&A, 0);
    check(bt_weapon_count(&A) == 1, "one weapon after erase");
    check(A.wpn_melee_atk[0] == BT_MELEE_THRUST, "spear runtime moved to slot 0 on compaction");
    check(A.wpn_melee_shooting[1] == 0 && A.wpn_melee_atk[1] == BT_MELEE_THRUST,
          "vacated slot runtime cleared");
}

/* =========================================================================
 * 1b. Engine frame accounting + the EXP overshoot factor, pinned against the
 *     pinned-Godot oracle run (tests/test_brotato_weapon_godot_oracles.py:
 *     claw thrust hitbox live on engine frames 8..24, attack_frames 39).
 * ========================================================================= */
static void test_engine_frame_accounting(void) {
    /* Tween::_tween_process completes on `elapsed > delay + duration` with
     * elapsed accumulated in float32 (p_delta += BT_DT), a STRICTLY greater
     * test: an exact multiple of the frame delta needs one step more
     * (6*BT_DT == 0.1f -> 7), a non-multiple is the quotient rounded up. */
    check(bt_melee_frames(0.1f) == 7, "0.1 s segment needs 7 tween steps (6*BT_DT == 0.1f)");
    check(bt_melee_frames(0.2f) == 13, "0.2 s segment needs 13 tween steps (12*BT_DT == 0.2f)");
    check(bt_melee_frames(0.05f) == 3, "0.05 s recoil_duration needs 3 tween steps");
    check(bt_melee_frames(0.26071429f) == 16, "0.2607143 s half out-tween needs 16 steps");
    check(bt_melee_frames(0.0f) == 0, "zero duration takes no step");

    /* expo::out is `c * 1.001 * (-pow(2, -10*t/d) + 1) + b`
     * (scene/animation/easing_equations.h:172-177): the 1.001 overshoot is part
     * of every recoil/return half of every melee swing.
     *
     * The ARITHMETIC is the engine's: `pow(2, <float>)` selects the binary64
     * pow(double,double) overload and `1.001` is a double, so the tail is
     * binary64 with ONE narrowing on return.  The values below are the pinned
     * engine's own: a REAL-property Tween(0.0 -> 1.0, dur 0.2, TRANS_EXPO,
     * EASE_OUT) at delta = 1/60 sampled from ~/tools/bin/godot-3.7-dev1, i.e.
     * bt_melee_elapsed(k) -> frame k+1's value.  The float32 exp2f form (the
     * previous sim implementation) is checked to MISMATCH as the negative
     * control. */
    {
        static const float kEngineExpo[11] = {
            0.439207762480f, 0.685704767704f, 0.824046552181f, 0.901688218117f,
            0.945263147354f, 0.969718754292f, 0.983443975449f, 0.991147041321f,
            0.995470225811f, 0.997896492481f, 0.999258220196f,
        };
        int bad = 0, float_would_match = 0;
        for (int k = 1; k <= 11; k++) {
            float got = bt_melee_ease_expo_out(0.0f, 1.0f, bt_melee_elapsed(k), 0.2f);
            if (got != kEngineExpo[k - 1]) bad++;
            float fl = 1.001f * (1.0f - exp2f(-10.0f * bt_melee_elapsed(k) / 0.2f));
            if (fl == kEngineExpo[k - 1]) float_would_match++;
        }
        check(bad == 0, "expo::out matches the pinned engine on all 11 interpolated frames");
        check(float_would_match < 11,
              "negative control: the float32 exp2f form does NOT reproduce the engine");
        check(bt_melee_ease_expo_out(0.0f, 1.0f, 0.2f, 0.2f) == 1.0f,
              "expo::out lands exactly on b + c on the completing frame (t == d)");
        check(bt_melee_ease_expo_out(0.0f, 1.0f, 0.0f, 0.2f) == 0.0f,
              "expo::out starts exactly at b");
    }

    /* The elapsed time is the engine's own float32 ACCUMULATION of the frame
     * delta (tween.cpp:752-753 `data.elapsed += p_delta`), not the product
     * k * BT_DT: they diverge from k = 6 on, and the eased value moves with them
     * (the frame table above is only reproduced by the accumulation). */
    {
        check(bt_melee_elapsed(0) == 0.0f, "elapsed(0) == 0");
        check(bt_melee_elapsed(1) == BT_DT, "elapsed(1) == BT_DT");
        check(bt_melee_elapsed(6) != 6.0f * BT_DT,
              "elapsed(6) is the float32 sum, not 6 * BT_DT (the engine's own divergence)");
        check(bt_melee_elapsed(6) == 0.100000001490f, "elapsed(6) pinned to the engine's accumulation");
        check(bt_melee_elapsed(12) == 0.200000002980f, "elapsed(12) pinned to the engine's accumulation");
    }

    /* The sweep angle is `0.9 * PI` in GDScript binary64, narrowed once to the
     * real_t property (melee_weapon_shooting_behavior.gd:57). */
    check(BT_WEAPON_MELEE_SWEEP_ANGLE == (float)(0.9 * 3.14159265358979323846),
          "sweep angle is the engine's (float)(0.9*PI) to the last bit");
    check(BT_WEAPON_MELEE_SWEEP_ANGLE > 2.82743f,
          "sweep angle is not the truncated %.6g literal (10 float32 ulps low)");

    /* The claw's own resolved durations through the real swing state machine:
     * recoil_duration 0.1, max_range 150 (thrust) -> out_f 16, back 0.2 -> 13.
     * The recoil emit is at frame 7, the set_deferred enable effective on the
     * physics step of frame 8, the disable (out complete at 23 -> emit 24) on
     * frame 25, set_shooting(false) on frame 38. */
    reset_world();
    unsigned short claw = wid_of("weapon_claw_1");
    check(claw != BT_WEAPON_NONE, "claw weapon present");
    if (claw != BT_WEAPON_NONE) {
        int s0 = bt_weapon_add(&A, claw, 0, 0.0f);
        BtPlayerWeaponStats cst;
        check(s0 >= 0 && bt_weapon_stats(&A, s0, &cst), "claw stats resolve");
        check(cst.attack_type == BT_MELEE_THRUST, "claw is a THRUST weapon");
        float rfac = bt_melee_range_factor((float)cst.max_range, cst.atk_spd);
        float atk = bt_melee_atk_duration(cst.atk_spd, rfac);
        set_swing(s0, 0.0f, cst.recoil_duration, atk, bt_melee_back_duration(cst.atk_spd),
                  (float)cst.recoil, (float)cst.max_range, 0, 0.0f);
        int rec_f = bt_melee_frames(cst.recoil_duration);
        int out_f = bt_melee_frames(atk * 0.5f);
        int back_f = bt_melee_frames(bt_melee_back_duration(cst.atk_spd));
        float px, py, rot; int hit, phase;
        set_swing(s0, (float)rec_f, cst.recoil_duration, atk, bt_melee_back_duration(cst.atk_spd),
                  (float)cst.recoil, (float)cst.max_range, 0, 0.0f);
        bt_melee_anim(&A, s0, BT_MELEE_ANIM_THRUST, &BT_WEAPON_GEOM[BT_WEAPONS[claw].geom],
                      &px, &py, &rot, &hit, &phase);
        check(hit == 0 && rec_f == 7, "claw: recoil occupies frames 0..6, hitbox still off");
        set_swing(s0, (float)(rec_f + 1), cst.recoil_duration, atk, bt_melee_back_duration(cst.atk_spd),
                  (float)cst.recoil, (float)cst.max_range, 0, 0.0f);
        bt_melee_anim(&A, s0, BT_MELEE_ANIM_THRUST, &BT_WEAPON_GEOM[BT_WEAPONS[claw].geom],
                      &px, &py, &rot, &hit, &phase);
        check(hit == 1 && rec_f + 1 == 8, "claw: hitbox live from engine frame 8 (oracle)");
        int hit_end = rec_f + 1 + out_f + 1;
        int swing_end = hit_end + back_f;
        check(hit_end - 1 == 24, "claw: last live frame is 24 (oracle)");
        check(swing_end == 38, "claw: set_shooting(false) on engine frame 38 (oracle attack_frames 39)");
        set_swing(s0, (float)(hit_end - 1), cst.recoil_duration, atk, bt_melee_back_duration(cst.atk_spd),
                  (float)cst.recoil, (float)cst.max_range, 0, 0.0f);
        bt_melee_anim(&A, s0, BT_MELEE_ANIM_THRUST, &BT_WEAPON_GEOM[BT_WEAPONS[claw].geom],
                      &px, &py, &rot, &hit, &phase);
        check(hit == 1, "claw: last live frame is still hitting");
        set_swing(s0, (float)hit_end, cst.recoil_duration, atk, bt_melee_back_duration(cst.atk_spd),
                  (float)cst.recoil, (float)cst.max_range, 0, 0.0f);
        bt_melee_anim(&A, s0, BT_MELEE_ANIM_THRUST, &BT_WEAPON_GEOM[BT_WEAPONS[claw].geom],
                      &px, &py, &rot, &hit, &phase);
        check(hit == 0 && phase == 3, "claw: hitbox off on frame 25 (oracle)");
        set_swing(s0, (float)swing_end, cst.recoil_duration, atk, bt_melee_back_duration(cst.atk_spd),
                  (float)cst.recoil, (float)cst.max_range, 0, 0.0f);
        bt_melee_anim(&A, s0, BT_MELEE_ANIM_THRUST, &BT_WEAPON_GEOM[BT_WEAPONS[claw].geom],
                      &px, &py, &rot, &hit, &phase);
        check(phase == 0 && hit == 0, "claw: swing ends (phase 0) on engine frame 38");
    }
}

/* =========================================================================
 * 1c. unit.gd:281 measures the knockback from the ATTACKER NODE (the player),
 *     not from the weapon's attach origin.
 * ========================================================================= */
static void test_knockback_attacker_is_player(void) {
    reset_world();
    int kind = find_kind("chaser");
    unsigned short hatchet = wid_of("weapon_hatchet_1");   /* knockback 2 */
    bt_weapon_add(&A, hatchet, 0, 0.0f);
    A.wpn_cd[0] = 0.0f;
    /* enemy on the PLAYER's own y row: 6 px above the single-weapon attach
     * origin, so the two candidate attacker positions differ. */
    place_hurt(0, kind, MELEE_WX(&A) + 100.0f, MELEE_WY(&A) - 6.0f);
    A.ehp[0] = 1.0e6f; A.emax[0] = 1.0e6f;
    /* unit.gd:281 feeds the VICTIM's own `global_position` into the weighted
     * direction: its Node transform, which for a MODE_CHARACTER enemy lags the
     * integrated body by one physics step (brotato_state.h:749-765).  This
     * fixture drives bt_melee_step directly -- no bt_core_step, so no per-frame
     * ehx/ehy refresh -- so it states the two positions explicitly and keeps
     * them far apart: the knockback must follow the LAGGED one. */
    A.ehx[0] = A.ex[0]; A.ehy[0] = A.ey[0] - 20.0f;
    BtPlayerWeaponStats st;
    bt_weapon_stats(&A, 0, &st);
    int frames = 0;
    float hp0 = A.ehp[0];
    while (frames < 40 && A.ehp[0] >= hp0 - 1e-4f) { bt_melee_step(&A, 0, 0.0f); frames++; }
    check(A.ehp[0] < hp0 - 1e-4f, "hatchet hit lands on the fixture enemy");
    float rot = A.wpn_melee_rot[0];
    BtVec2 hbdir = bt_v2(bt_cosf(rot), bt_sinf(rot));
    BtVec2 victim_lag = bt_v2(A.ehx[0], A.ehy[0]);
    BtVec2 victim_body = bt_v2(A.ex[0], A.ey[0]);
    BtVec2 from_player = bt_knockback_direction_weighted(hbdir, bt_v2(A.px, A.py), victim_lag);
    BtVec2 from_weapon = bt_knockback_direction_weighted(hbdir, bt_v2(MELEE_WX(&A), MELEE_WY(&A)), victim_lag);
    BtVec2 from_body = bt_knockback_direction_weighted(hbdir, bt_v2(A.px, A.py), victim_body);
    check(bt_fabsf(from_player.y - from_weapon.y) > 1e-3f,
          "fixture separates the player-centre and weapon-origin conventions");
    check(bt_fabsf(from_player.y - from_body.y) > 1e-3f,
          "fixture separates the lagged Node transform from the integrated body");
    check_near(A.ekby[0], from_player.y * (float)st.knockback, 1e-4f,
               "melee knockback direction is measured from the player (unit.gd:281)");
    check(bt_fabsf(A.ekby[0] - from_weapon.y * (float)st.knockback) > 1e-3f,
          "and NOT from the weapon attach origin");
    check_near(A.ekbx[0], from_player.x * (float)st.knockback, 1e-4f,
               "knockback vector = direction * amount (unit.gd:333)");
    check(bt_fabsf(A.ekby[0] - from_body.y * (float)st.knockback) > 1e-3f,
          "and NOT from the frame's integrated body position");
}

static void test_thrust_hit_once(void) {
    reset_world();
    int kind = find_kind("chaser");
    check(kind >= 0, "chaser kind present");
    unsigned short spear = wid_of("weapon_spear_1");   /* dmg 15, range 0-350, kb 0 */
    bt_weapon_add(&A, spear, 0, 0.0f);
    A.wpn_cd[0] = 0.0f;   /* start swinging immediately for exact frame timing */

    BtPlayerWeaponStats st;
    bt_weapon_stats(&A, 0, &st);
    check(st.kind == BT_WEAPON_KIND_MELEE, "bt_weapon_stats reports melee");

    place_hurt(0, kind, MELEE_WX(&A) + 150.0f, MELEE_WY(&A));
    /* The check below measures the DEALT damage, so the target needs a pool
     * larger than any crit it can take: the wave-1 spawn's own health is small
     * (and the enemy-init health factor is another owner's live area). */
    A.ehp[0] = 1000.0f; A.emax[0] = 1000.0f;
    BT_CONST_PTR(BtUnitKind) K = bt_kunit(&A, 0);
    float armor = K->armor + K->armor_grow * (float)(A.wave - 1);
    float expect_min = (float)st.damage - armor;
    if (expect_min < 1.0f) expect_min = 1.0f;
    float crit_full = bt_roundf((float)st.damage * st.crit_damage) - armor;
    if (crit_full < 1.0f) crit_full = 1.0f;
    float expect_max = crit_full + 0.01f;

    float hp0 = A.ehp[0];
    int frames = 0, hits = 0;
    float first_drop = 0.0f;
    while (frames < 60 && hits == 0) {
        bt_melee_step(&A, 0, 0.0f);
        if (A.ehp[0] < hp0 - 1e-4f) { hits++; first_drop = hp0 - A.ehp[0]; }
        frames++;
    }
    check(hits == 1, "thrust lands exactly one hit while a target is in range");
    check(first_drop >= expect_min - 1e-3f && first_drop <= expect_max,
          "thrust damage = max(1, weapon damage - armor) (crit bounded)");
    check(frames > 6, "hit lands after the recoil window, not on frame 0");

    float hp1 = A.ehp[0];
    int extra = 0;
    for (int i = 0; i < 20; i++) { bt_melee_step(&A, 0, 0.0f); if (A.ehp[0] < hp1 - 1e-4f) extra++; }
    check(extra <= 1, "per-swing hit gating: at most a new swing re-hits");
}

/* =========================================================================
 * 4. Multi-target sweep: every enemy on the arc is hit once per swing.
 * ========================================================================= */
static void test_sweep_multi_target(void) {
    reset_world();
    int kind = find_kind("chaser");
    unsigned short hatchet = wid_of("weapon_hatchet_1");   /* SWEEP */
    bt_weapon_add(&A, hatchet, 0, 0.0f);
    A.wpn_cd[0] = 0.0f;

    place_hurt(0, kind, MELEE_WX(&A) + 60.0f, MELEE_WY(&A));
    place_hurt(1, kind, MELEE_WX(&A) + 100.0f, MELEE_WY(&A));
    place_hurt(2, kind, MELEE_WX(&A) - 400.0f, MELEE_WY(&A));   /* outside detection */

    float h0 = A.ehp[0], h1 = A.ehp[1], h2 = A.ehp[2];
    for (int i = 0; i < 40; i++) bt_melee_step(&A, 0, 0.0f);

    check(A.ehp[0] < h0 - 1e-4f, "sweep hits the near enemy on the arc");
    check(A.ehp[1] < h1 - 1e-4f, "sweep hits the far enemy on the arc");
    check(!(A.ehp[2] < h2 - 1e-4f), "sweep never reaches an enemy behind the player");
}

/* =========================================================================
 * 5. Repeat boundaries: no re-entry before the swing ends; cooldown honoured.
 * ========================================================================= */
static void test_repeat_cadence(void) {
    reset_world();
    int kind = find_kind("chaser");
    unsigned short spear = wid_of("weapon_spear_1");
    bt_weapon_add(&A, spear, 0, 0.0f);
    A.wpn_cd[0] = 0.0f;
    BtPlayerWeaponStats st;
    bt_weapon_stats(&A, 0, &st);

    place_hurt(0, kind, MELEE_WX(&A) + 150.0f, MELEE_WY(&A));

    float rec = st.recoil_duration;
    float rfac = bt_melee_range_factor((float)st.max_range, st.atk_spd);
    float atk = bt_melee_atk_duration(st.atk_spd, rfac);
    float back = bt_melee_back_duration(st.atk_spd);
    float cycle_f = (rec + atk + back) * 60.0f + (float)st.cooldown;   /* frames */
    int expect = (int)(120.0f / cycle_f);
    if (expect < 1) expect = 1;

    float prev = A.ehp[0];
    int hits = 0;
    for (int i = 0; i < 120; i++) {
        bt_melee_step(&A, 0, 0.0f);
        if (A.ehp[0] < prev - 1e-4f) hits++;
        prev = A.ehp[0];
        if (!A.ealive[0]) break;
    }
    check(hits >= expect - 1 && hits <= expect + 1,
          "repeat cadence matches the source cycle over 120 frames");

    /* can_attack_while_moving defaults to 1 (player_run_data.gd:485), so a moving
     * player fires; zeroing it (Soldier) disables firing while moving. */
    reset_world();
    bt_weapon_add(&A, spear, 0, 0.0f);
    A.wpn_cd[0] = 0.0f;
    place_hurt(0, kind, MELEE_WX(&A) + 150.0f, MELEE_WY(&A));
    float h = A.ehp[0];
    for (int i = 0; i < 60; i++) bt_melee_step(&A, 1, 1.0f);
    check(A.ehp[0] < h - 1e-4f, "fires while moving (can_attack_while_moving default 1)");

    reset_world();
    bt_weapon_add(&A, spear, 0, 0.0f);
    A.wpn_cd[0] = 0.0f;
    A.scalar_effect[BT_ISTR_CAN_ATTACK_WHILE_MOVING] = 0.0f;
    place_hurt(0, kind, MELEE_WX(&A) + 150.0f, MELEE_WY(&A));
    float h2 = A.ehp[0];
    for (int i = 0; i < 60; i++) bt_melee_step(&A, 1, 1.0f);
    check(!(A.ehp[0] < h2 - 1e-4f), "no fire while moving when capability is zeroed");
}

/* =========================================================================
 * 6. THRUST/SWEEP alternation (MeleeWeapon.shoot flip).
 * ========================================================================= */
static void test_alternation(void) {
    reset_world();
    unsigned short sword = wid_of("weapon_sword_2");   /* THRUST, alternate=true */
    if (!bt_weapon_valid(sword)) return;
    P.wpn_id[0] = sword; P.wpn_cursed[0] = 0; P.wpn_curse[0] = 0.0f;
    P.wpn_cd[0] = 0.0f; P.wpn_count = 1;
    BtPlayerWeaponStats st;
    if (!bt_weapon_stats(&P, 0, &st)) return;
    if (st.alternate_attack_type != 1) return;   /* only this family flips */

    int kind = find_kind("chaser");
    bt_weapon_add(&A, sword, 0, 0.0f);
    A.wpn_cd[0] = 0.0f;
    check(A.wpn_melee_atk[0] == st.attack_type, "first attack type = source attack_type");

    place_hurt(0, kind, MELEE_WX(&A) + 100.0f, MELEE_WY(&A));
    int seen_anim = 0;
    for (int i = 0; i < 120; i++) {
        bt_melee_step(&A, 0, 0.0f);
        if (A.wpn_melee_anim[0] != BT_MELEE_ANIM_IDLE) seen_anim = A.wpn_melee_anim[0];
        if (A.wpn_melee_atk[0] != st.attack_type) break;
    }
    check(A.wpn_melee_atk[0] != st.attack_type, "next_attack_type flips after a swing");
    check(seen_anim != 0, "a swing actually ran");
}

/* =========================================================================
 * 7. End to end through bt_core_step: a held player with a melee weapon
 *    damages (or kills) an in-range enemy during normal play.
 * ========================================================================= */
static void test_core_step_end_to_end(void) {
    reset_world();
    int kind = find_kind("chaser");
    unsigned short spear = wid_of("weapon_spear_1");
    bt_weapon_add(&A, spear, 0, 0.0f);
    place_hurt(0, kind, A.px + 120.0f, A.py + 6.0f);

    float h0 = A.ehp[0];
    float obs[BT_OBS_SIZE];
    float rew = 0.0f, term = 0.0f;
    int killed = 0, damaged = 0;
    for (int i = 0; i < 240; i++) {
        bt_core_step(&A, 0, obs, &rew, &term);
        if (!A.ealive[0]) { killed = 1; break; }
        if (A.ehp[0] < h0 - 1e-4f) damaged = 1;
        if (term > 0.5f) break;
    }
    check(damaged || killed, "melee weapon damages an enemy through bt_core_step");
}

/* =========================================================================
 * 8. Melee on-hit children (unit.gd:586-604 + weapon_service.gd:339-380).
 *    A real strike of a ProjectilesOnHitEffect melee weapon spawns its nested
 *    child shots at the target, ignoring the struck source node, carrying the
 *    nested stats row's own scene/geometry/delay/slow/bounce/elemental type --
 *    and they are real projectiles that fly on and hit.
 * ========================================================================= */
static int live_children(void) {
    int n = 0;
    for (int q = 0; q < BT_MAX_WPROJ; q++) if (A.wproj.p[q].alive) n++;
    return n;
}

static int strike_once(unsigned short wid, int kind, float dist) {
    reset_world();
    bt_weapon_add(&A, wid, 0, 0.0f);
    A.wpn_cd[0] = 0.0f;
    place_hurt(0, kind, MELEE_WX(&A) + dist, MELEE_WY(&A));
    A.ehp[0] = 1.0e6f; A.emax[0] = 1.0e6f;
    float h0 = A.ehp[0];
    for (int i = 0; i < 90; i++) {
        bt_melee_step(&A, 0, 0.0f);
        if (A.ehp[0] < h0 - 1e-4f) return 1;
    }
    return 0;
}

static void test_onhit_children(void) {
    int kind = find_kind("chaser");
    check(kind >= 0, "chaser kind present");

    /* lightning_shiv: 1 elemental child, auto_target_enemy = true */
    unsigned short shiv = wid_of("weapon_lightning_shiv_4");
    check(bt_weapon_valid(shiv) && bt_weapon_kind(shiv) == BT_WEAPON_KIND_MELEE,
          "lightning_shiv_4 is a melee weapon");
    if (bt_weapon_valid(shiv)) {
        unsigned int near_uid = 0;
        reset_world();
        bt_weapon_add(&A, shiv, 0, 0.0f);
        A.wpn_cd[0] = 0.0f;
        place_hurt(0, kind, MELEE_WX(&A) + 100.0f, MELEE_WY(&A));
        A.ehp[0] = 1.0e6f; A.emax[0] = 1.0e6f;
        near_uid = bt_object_uid(&A, 0);
        check(near_uid != 0u, "struck target carries a source-object identity");
        float h0 = A.ehp[0];
        int hit = 0;
        for (int i = 0; i < 60 && !hit; i++) {
            bt_melee_step(&A, 0, 0.0f);
            if (A.ehp[0] < h0 - 1e-4f) hit = 1;
        }
        check(hit, "lightning_shiv strike damages the target");
        check(live_children() == 1, "exactly one child projectile per lightning_shiv hit");
        for (int q = 0; q < BT_MAX_WPROJ; q++) {
            if (!A.wproj.p[q].alive) continue;
            check_near(A.wproj.p[q].x, A.ex[0], 1e-3f, "child spawns at the struck node x");
            check_near(A.wproj.p[q].y, A.ey[0], 1e-3f, "child spawns at the struck node y");
            check(bt_wproj_ignored(&A.wproj.p[q], near_uid) != 0,
                  "child ignores the struck node's identity");
        }
    }

    /* thunder_sword_4 ProjectilesOnHitEffect.value = 4
     * (thunder_sword_4_effect.tres): unit.gd:587 spawns `value` children of the
     * delayed taser scene (slow hitbox) per hit. */
    unsigned short ts = wid_of("weapon_thunder_sword_4");
    if (bt_weapon_valid(ts) && bt_weapon_kind(ts) == BT_WEAPON_KIND_MELEE) {
        check(strike_once(ts, kind, 150.0f), "thunder_sword strike damages the target");
        BtWeaponOnHit toh;
        check(bt_weapon_stats_onhit(&A, 0, &toh) && toh.count == 4,
              "thunder_sword_4 ProjectilesOnHitEffect.value is 4");
        check(live_children() == (int)toh.count,
              "thunder_sword spawns one child per effect value");
        for (int q = 0; q < BT_MAX_WPROJ; q++) {
            if (!A.wproj.p[q].alive) continue;
            check(A.wproj.p[q].slow_rad > 0.0f, "thunder_sword child carries the nested slow hitbox");
            check((A.wproj.p[q].flags & BT_WPROJ_F_DELAYED) != 0,
                  "thunder_sword child is the delayed projectile scene");
            check(A.wproj.p[q].delay >= 0.0f && A.wproj.p[q].delay <= 3.0f,
                  "delayed child retains its node delay in [0,3]");
        }
    }

    /* dextroyer effect 2: a bouncing child */
    unsigned short dx = wid_of("weapon_dextroyer_4");
    if (bt_weapon_valid(dx) && bt_weapon_kind(dx) == BT_WEAPON_KIND_MELEE) {
        check(strike_once(dx, kind, 150.0f), "dextroyer strike damages the target");
        int n = 0, bounced = 0;
        for (int q = 0; q < BT_MAX_WPROJ; q++) {
            if (!A.wproj.p[q].alive) continue;
            n++;
            if (A.wproj.p[q].bounce > 0) bounced = 1;
        }
        check(n >= 1, "dextroyer hit spawns its child");
        check(bounced, "dextroyer child carries the nested bounce count");
    }

    /* a spawned child is a REAL projectile: it flies on and hits the auto-targeted
     * other enemy, and keeps its captured view after its owning weapon is gone */
    if (bt_weapon_valid(shiv)) {
        reset_world();
        bt_weapon_add(&A, shiv, 0, 0.0f);
        A.wpn_cd[0] = 0.0f;
        place_hurt(0, kind, MELEE_WX(&A) + 100.0f, MELEE_WY(&A));
        place_hurt(1, kind, MELEE_WX(&A) + 600.0f, MELEE_WY(&A));
        A.ehp[0] = 1.0e6f; A.emax[0] = 1.0e6f;
        A.ehp[1] = 1.0e6f; A.emax[1] = 1.0e6f;
        float h0 = A.ehp[0], h1 = A.ehp[1];
        int hit0 = 0, hit1 = 0, removed = 0;
        for (int i = 0; i < 120 && !hit1; i++) {
            bt_melee_step(&A, 0, 0.0f);
            float r = 0.0f;
            bt_wproj_step(&A, &r);
            if (!hit0 && A.ehp[0] < h0 - 1e-4f) hit0 = 1;
            if (hit0 && !removed) { bt_weapon_remove(&A, 0); removed = 1; }
            if (A.ehp[1] < h1 - 1e-4f) hit1 = 1;
        }
        check(hit0, "melee strike damages the struck target");
        check(hit1, "the spawned child flies on and hits the auto-targeted enemy");
        check(removed && bt_weapon_count(&A) == 0,
              "the owning weapon was removed before the child landed (immutable view)");
    }
}

/* =========================================================================
 * 9. Per-swing hit gating is keyed on the source-OBJECT identity, not the live
 *    actor slot: the same node is never hit twice in one swing, but a NEW node
 *    stamped into the SAME slot during that swing is a new target.
 * ========================================================================= */
static void test_hit_identity_gate(void) {
    int kind = find_kind("chaser");
    unsigned short hatchet = wid_of("weapon_hatchet_1");   /* SWEEP: long hit window */
    if (!bt_weapon_valid(hatchet)) return;

    reset_world();
    bt_weapon_add(&A, hatchet, 0, 0.0f);
    A.wpn_cd[0] = 0.0f;
    place_hurt(0, kind, MELEE_WX(&A) + 60.0f, MELEE_WY(&A));
    A.ehp[0] = 1.0e6f; A.emax[0] = 1.0e6f;

    float prev = A.ehp[0];
    int first = 0;
    for (int i = 0; i < 40 && !first; i++) {
        bt_melee_step(&A, 0, 0.0f);
        if (A.ehp[0] < prev - 1e-4f) first = 1;
    }
    check(first, "sweep hits the enemy inside its arc");

    unsigned int uid_first = bt_object_uid(&A, 0);
    check(uid_first != 0u, "the enemy carries a source-object identity");

    /* keep the current swing's hit window open deterministically for the rest of
     * the check (the harness only lengthens the swing; it does not touch gating) */
    A.wpn_melee_atk_dur[0] = 2.0f;

    /* (a) the SAME node is never hit twice within one swing */
    float hp1 = A.ehp[0];
    int same_rehit = 0;
    for (int i = 0; i < 4 && A.wpn_melee_hit_on[0]; i++) {
        bt_melee_step(&A, 0, 0.0f);
        if (A.ehp[0] < hp1 - 1e-4f) same_rehit = 1;
    }
    check(!same_rehit, "same source node is not hit twice in one swing");
    check(bt_object_uid(&A, 0) == uid_first, "the node identity is unchanged");

    /* (b) a NEW node in the SAME live actor slot is a new target: a fresh node
     *     stamped into slot 0 must be hittable within the SAME swing. */
    A.e_object_uid[0] = bt_object_uid_mint(&A);
    float hp2 = A.ehp[0];
    int new_hit = 0;
    for (int i = 0; i < 10 && !new_hit && A.wpn_melee_hit_on[0]; i++) {
        bt_melee_step(&A, 0, 0.0f);
        if (A.ehp[0] < hp2 - 1e-4f) new_hit = 1;
    }
    check(new_hit, "a new node in the same actor slot is hittable within the same swing");
}

/* =========================================================================
 * 10. Crit-kill gold (gold_on_crit_kill) is a DIRECT purse credit
 *     (unit.gd:367-382 RunData.add_gold -> run_data.gd:810-818), NOT a coin:
 *     the purse gains it and nothing is spawned (so no coin, no XP, none of the
 *     gold-pickup path).  Controlled by a same-seed pair of runs that differ
 *     only in whether the gold_on_crit_kill row is owned, so the dying enemy's
 *     own drop cancels out; the crit-kill roll happens AFTER that drop, so both
 *     runs' RNG streams are identical up to it.
 * ========================================================================= */
static int find_eff_row(unsigned short key) {
    for (int i = 0; i < BT_NITEM_EFF_ROWS; i++)
        if (bt_eff_is(&BT_ITEM_EFF_ROWS[i], key)) return i;
    return -1;
}
static void own_effect_row(int row, int v, int v2) {
    if (A.bk_count[row] == 0) A.bk_active[A.n_bk_active++] = (unsigned short)row;
    A.bk_count[row]++;
    bt_row_variant_add(&A, row, v, v2);
}

static void crit_kill_run(int with_gold_row, int row,
                          float* purse_delta, int* coin_delta, float* xp_delta) {
    int kind = find_kind("chaser");
    unsigned short spear = wid_of("weapon_spear_1");
    reset_world();
    A.stat_raw[9] = 1000.0f;       /* stat_crit_chance: every hit crits */
    A.stat_cap[9] = 1000000.0f;
    if (with_gold_row && row >= 0) own_effect_row(row, 100, 0);
    bt_weapon_add(&A, spear, 0, 0.0f);
    A.wpn_cd[0] = 0.0f;
    place_hurt(0, kind, MELEE_WX(&A) + 120.0f, MELEE_WY(&A));
    A.ehp[0] = 1.0f;               /* dies on the first hit */
    float m0 = A.materials;
    int c0 = A.matcount;
    float x0 = A.xp;
    for (int i = 0; i < 40; i++) bt_melee_step(&A, 0, 0.0f);
    *purse_delta = A.materials - m0;
    *coin_delta = A.matcount - c0;
    *xp_delta = A.xp - x0;
}

static void test_crit_kill_gold_direct(void) {
    int row = find_eff_row(BT_ISTR_GOLD_ON_CRIT_KILL);
    check(row >= 0, "a gold_on_crit_kill row exists");
    if (row < 0) return;
    float purse_a = 0.0f, purse_b = 0.0f, xp_a = 0.0f, xp_b = 0.0f;
    int coins_a = 0, coins_b = 0;
    crit_kill_run(1, row, &purse_a, &coins_a, &xp_a);   /* row owned */
    crit_kill_run(0, row, &purse_b, &coins_b, &xp_b);   /* control, same seed */
    check(purse_a == purse_b + 1.0f,
          "crit-kill gold is credited DIRECTLY to the purse (exactly +1 with the row)");
    check(coins_a == coins_b,
          "crit-kill gold spawns NO coin (coin count matches the control run)");
    check(xp_a == xp_b, "crit-kill gold grants NO XP");
}

/* =========================================================================
 * 11. Weapon-feeding stat change (stats_manager.gd:36-45 -> init_stats(false)
 *     -> reset_cooldown, weapon.gd:311-313/324-326/349-350): the ALREADY
 *     running cooldown is re-clamped by the freshly resolved stats, with the
 *     every-x-shots big-reload branch, and with ZERO RNG (never
 *     get_next_cooldown).  The chainsaw carries `additional_cd_every` with a
 *     multiplier, so both sides of `_nb_shots_taken % every == 0` are testable.
 * ========================================================================= */
static void test_stats_changed_clamp(void) {
    reset_world();
    unsigned short saw = wid_of("weapon_chainsaw_4");
    if (!bt_weapon_valid(saw) || bt_weapon_kind(saw) != BT_WEAPON_KIND_MELEE) return;
    bt_weapon_add(&A, saw, 0, 0.0f);
    BtPlayerWeaponStats st;
    if (!bt_weapon_stats(&A, 0, &st)) return;
    check(st.additional_cd_every > 0 && st.additional_cd_multiplier > 0.0f,
          "chainsaw carries an every-x-shots big reload");
    if (!(st.additional_cd_every > 0 && st.additional_cd_multiplier > 0.0f)) return;

    /* off the boundary: clamp to the plain cooldown, no RNG draw */
    A.wpn_melee_shots[0] = (unsigned short)(st.additional_cd_every - 1);
    A.wpn_cd[0] = 9999.0f;
    BtRng before = A.rng;
    bt_weapon_stats_changed(&A);
    check_near(A.wpn_cd[0], (float)st.cooldown, 0.0f,
               "stat-change clamp uses the plain cooldown off the every-X boundary");
    check(memcmp(&before, &A.rng, sizeof(BtRng)) == 0,
          "stat-change clamp consumes no RNG (never get_next_cooldown)");

    /* on the boundary: is_big_reload_active -> the multiplier */
    A.wpn_melee_shots[0] = (unsigned short)st.additional_cd_every;
    A.wpn_cd[0] = 9999.0f;
    bt_weapon_stats_changed(&A);
    check_near(A.wpn_cd[0], (float)st.cooldown * st.additional_cd_multiplier, 0.0f,
               "stat-change clamp applies the multiplier on the every-X boundary");
}

/* Pick a PCG state whose next `Math::randf()` (bt_math_randf: ONE draw) lands in
 * [lo, hi].  bt_rng_next's xorshifted output is 0 for every state below 2^27, so
 * the search strides well-separated 64-bit states instead of counting up. */
static int rng_state_with_randf(float lo, float hi, BtRng* out) {
    BtRng r = A.rng;
    for (bt_u64 i = 1; i < 4000000u; i++) {
        r.state = i * (bt_u64)0x9E3779B97F4A7C15u + (bt_u64)0x2545F4914F6CDD1D;
        BtRng probe = r;
        float v = bt_math_randf(&probe);
        if (v >= lo && v <= hi) { *out = r; return 1; }
    }
    return 0;
}

/* =========================================================================
 * 13. Wave-begin node re-creation (gap 1): ONE get_next_cooldown(true) draw per
 *     melee copy, in SLOT order, with the draw's weapon_count bound equal to the
 *     copy's own SLOT INDEX -- player.gd:288-289 counts only the nodes already
 *     created (player.gd:387-389 pushes onto current_weapons AFTER add_child has
 *     run _ready), so the copy created as slot k draws with weapon_count == k.
 * ========================================================================= */
static void test_wave_begin_draw(void) {
    reset_world();
    unsigned short plank = wid_of("weapon_plank_1");
    unsigned short spear = wid_of("weapon_spear_1");
    check(plank != BT_WEAPON_NONE && spear != BT_WEAPON_NONE, "wave-begin weapons present");
    int s0 = bt_weapon_add(&A, plank, 0, 0.0f);
    int s1 = bt_weapon_add(&A, spear, 0, 0.0f);
    check(s0 == 0 && s1 == 1, "two melee copies in slot order");

    BtPlayerWeaponStats st0, st1;
    check(bt_weapon_stats(&A, 0, &st0) && bt_weapon_stats(&A, 1, &st1), "wave-begin stats resolve");
    check(st0.cooldown < 180, "first copy's cooldown is below the wave-begin clamp");

    BtRng before = A.rng;
    /* Reference replay of both draws from the same stream, weapon_count == slot,
     * with the same assign-then-reset_cooldown order the source uses. */
    P = A;
    float d0 = bt_wfx_next_cooldown(&P, st0.cooldown, st0.additional_cd_every, 0, 0, 1,
                                    st0.additional_cd_multiplier);
    P.wpn_cd[0] = d0;
    bt_weapon_reset_cooldown(&P, 0);
    float ref0 = P.wpn_cd[0];
    float d1 = bt_wfx_next_cooldown(&P, st1.cooldown, st1.additional_cd_every, 0, 1, 1,
                                    st1.additional_cd_multiplier);
    P.wpn_cd[1] = d1;
    bt_weapon_reset_cooldown(&P, 1);
    float ref1 = P.wpn_cd[1];
    BtRng after_ref = P.rng;

    bt_weapon_wave_begin(&A);
    check(A.wpn_cd[0] == ref0 && A.wpn_cd[1] == ref1,
          "wave-begin replays the per-slot draws in slot order");
    check(memcmp(&before, &A.rng, sizeof(BtRng)) != 0, "wave-begin consumed RNG");
    check(A.rng.state == after_ref.state && A.rng.inc == after_ref.inc,
          "wave-begin consumes exactly the two slot-ordered get_next_cooldown draws");
    /* weapon_count == 0 -> get_max_rand_cooldown == min(0,0) == 0 ->
     * rand_range(cb, cb): slot 0's drawn cooldown is EXACTLY the cooldown.  The
     * pre-fix form passed the dense count (1) and drew in [cb-5, cb+5]. */
    check(d0 == (float)st0.cooldown, "slot 0 draws exactly `cooldown` (weapon_count == 0)");
    check(A.wpn_cd[0] == (float)st0.cooldown ||
          A.wpn_cd[0] == (float)st0.cooldown * st0.additional_cd_multiplier,
          "slot 0 keeps the exact cooldown after reset_cooldown");
    /* The dense-count bound (the pre-fix off-by-one) draws a different value for
     * slot 0: max_rand becomes min(cb/5, 5) instead of 0. */
    {
        P = A;
        float dense0 = bt_wfx_next_cooldown(&P, st0.cooldown, st0.additional_cd_every, 0, 1, 1,
                                            st0.additional_cd_multiplier);
        check(dense0 != (float)st0.cooldown,
              "negative control: the dense-count bound does NOT draw the exact cooldown");
    }
    /* The runtime is re-created, not merely clamped: a mid-swing copy is idle
     * again with its attack id reset. */
    A.wpn_melee_phase[0] = 3;
    A.wpn_melee_shooting[0] = 1;
    A.wpn_melee_attack_id[0] = 7u;
    A.wpn_melee_hit[0][0] = 0xFFFFFFFFu;
    bt_weapon_wave_begin(&A);
    check(A.wpn_melee_phase[0] == 0 && A.wpn_melee_shooting[0] == 0 &&
          A.wpn_melee_attack_id[0] == 0u && A.wpn_melee_hit[0][0] == 0u,
          "wave-begin clears the swing runtime (fresh node, not a clamp)");
    check(A.wpn_melee_atk[0] == st0.attack_type, "wave-begin restores next_attack_type");
    check(bt_weapon_count(&A) == 2, "loadout unchanged by the wave-begin pass");
}

/* =========================================================================
 * 14. Range-member ship (gap 2): the weapon's Range Area2D overlaps the unit's
 *     BODY CollisionShape2D circle (weapon.tscn mask 5 = layer_1 neutrals +
 *     layer_3 enemies), not a centre-to-centre distance, and neutral trees are
 *     candidates too (neutral.tscn collision_layer = 1).
 * ========================================================================= */
static void test_target_membership(void) {
    reset_world();
    int kind = find_kind("chaser");
    check(kind >= 0, "chaser kind present");
    unsigned short spear = wid_of("weapon_spear_1");   /* max_range 350, min_range 0 */
    bt_weapon_add(&A, spear, 0, 0.0f);
    BtPlayerWeaponStats st;
    bt_weapon_stats(&A, 0, &st);
    float detect = (float)st.max_range + (float)BT_WEAPON_DETECTION_RANGE;   /* 550 */

    /* Pin the enemy first so its body circle is readable. */
    place_hurt(0, kind, MELEE_WX(&A) + 100.0f, MELEE_WY(&A));
    BT_CONST_PTR(BtBodyPhysics) G = bt_kgeom(&A, 0);
    check(G->body_radius > 6.0f, "enemy body circle is wide enough for the test");

    /* Body circle reachable, centre OUTSIDE the Range circle. */
    float d = detect + 5.0f;
    place_hurt(0, kind, MELEE_WX(&A) + d, MELEE_WY(&A));
    {
        float dx = A.ex[0] - MELEE_WX(&A), dy = A.ey[0] - MELEE_WY(&A);
        float centre = bt_sqrtf(dx * dx + dy * dy);
        float bx = dx + G->body_x, by = dy + G->body_y;
        check(centre > detect, "centre distance is OUTSIDE the Range circle");
        check(bt_sqrtf(bx * bx + by * by) <= detect + G->body_radius,
              "body circle overlaps the Range circle");
    }
    {
        unsigned short tgt; float tdist;
        int has = bt_melee_pick_target(&A, MELEE_WX(&A), MELEE_WY(&A),
                                       (float)st.min_range, detect, &tgt, &tdist);
        check(has && tgt == 0, "body-overlap membership selects the enemy the centre test would miss");
    }

    /* Beyond the body reach: not a member. */
    place_hurt(0, kind, MELEE_WX(&A) + detect + G->body_radius + 50.0f, MELEE_WY(&A));
    {
        unsigned short tgt; float tdist;
        int has = bt_melee_pick_target(&A, MELEE_WX(&A), MELEE_WY(&A),
                                       (float)st.min_range, detect, &tgt, &tdist);
        check(!has, "an enemy beyond the body reach is not a member");
    }

    /* min_range stays CENTRE-to-centre. */
    place_hurt(0, kind, MELEE_WX(&A) + 5.0f, MELEE_WY(&A));
    {
        unsigned short tgt; float tdist;
        int has = bt_melee_pick_target(&A, MELEE_WX(&A), MELEE_WY(&A),
                                       25.0f, detect, &tgt, &tdist);
        check(!has, "min_range still filters on the centre distance");
    }

    /* A neutral tree is a candidate (neutral.tscn collision_layer 1). */
    for (int i = 0; i < BT_MAX; i++) bt_enemy_set_alive(&A, i, 0);
    check(bt_spawn_neutral(&A, 0, MELEE_WX(&A) + 300.0f, MELEE_WY(&A)) == 1, "neutral tree spawned");
    {
        unsigned short tgt; float tdist;
        int has = bt_melee_pick_target(&A, MELEE_WX(&A), MELEE_WY(&A),
                                       (float)st.min_range, detect, &tgt, &tdist);
        check(has && tgt == 0, "a neutral tree inside the Range circle is a target");
    }
}

/* =========================================================================
 * 14b. The swing also DAMAGES the tree it targets: every tree's Hurtbox
 *      monitors mask 1032 (neutral.tscn) so the melee Hitbox hits it, and the
 *      neutral-specific resolution (hit tally / one_shot_trees / HP death /
 *      tree_turrets) runs inside the shared hit primitive.
 * ========================================================================= */
static void test_melee_hits_neutral_tree(void) {
    reset_world();
    unsigned short spear = wid_of("weapon_spear_1");
    bt_weapon_add(&A, spear, 0, 0.0f);
    A.wpn_cd[0] = 0.0f;
    for (int i = 0; i < BT_MAX; i++) bt_enemy_set_alive(&A, i, 0);
    A.ecount = 0;
    check(bt_spawn_neutral(&A, 0, MELEE_WX(&A) + 60.0f, MELEE_WY(&A)) == 1,
          "tree spawned inside the thrust envelope");
    BtPlayerWeaponStats st;
    bt_weapon_stats(&A, 0, &st);
    check((float)st.damage > A.ehp[0], "the weapon's damage is lethal to a wave-1 tree");
    int frames = 0;
    /* unit.gd:349-352 defers the death (`call_deferred("die", ...)` +
     * `_pending_die = true`), so the unit stays `ealive` until the engine
     * flushes its MessageQueue at the END of the frame -- the sim's equivalent
     * is bt_drain_kill_q (bt_core_step drains once per tick, brotato_core.h),
     * which a bare bt_melee_step fixture must model itself. */
    while (frames < 200 && A.ealive[0]) { bt_melee_step(&A, 0, 0.0f); bt_drain_kill_q(&A); frames++; }
    check(!A.ealive[0], "the melee swing damages and kills a neutral tree");
    check(BT_NEUTRAL_HITS(&A, 0) >= 1, "the tree's own hit tally advanced (neutral.gd:23)");
    check(frames > 6, "the tree dies from a real swing, not on frame 0");
}

/* =========================================================================
 * 15. Melee unit-side ExplodingEffect credits the firing weapon (unit.gd:569).
 * ========================================================================= */
static void test_explosion_source_credit(void) {
    reset_world();
    int kind = find_kind("chaser");
    unsigned short plank = wid_of("weapon_plank_4");   /* ExplodingEffect chance 0.40 */
    check(plank != BT_WEAPON_NONE, "plank_4 present");
    int slot = bt_weapon_add(&A, plank, 0, 0.0f);
    check(slot == 0, "plank in slot 0");
    BtPlayerWeaponStats st;
    bt_weapon_stats(&A, 0, &st);
    int n_rows = (int)BT_WEAPONS[plank].fx_count;
    BT_CONST_PTR(unsigned short) rows = &BT_WEAPON_EFF_INDEX[(int)BT_WEAPONS[plank].fx_first];
    check(n_rows > 0 && bt_item_expl_opcode((int)BT_ITEM_EFF_ROWS[(int)rows[0]].opcode),
          "plank_4's first weapon row is the ExplodingEffect");

    place_hurt(0, kind, MELEE_WX(&A) + 60.0f, MELEE_WY(&A));
    A.ehp[0] = 1.0f;   /* the explosion must kill */

    /* The hit's FIRST draw is the ExplodingEffect chance roll (no charm source is
     * owned, so the on_hurt charm roll draws nothing): pick a PCG state whose
     * next output is inside the row's chance. */
    float chance = BT_ITEM_EFF_ROWS[(int)rows[0]].explode_chance;
    check(chance > 0.0f && chance < 1.0f, "plank_4's explode chance is a real probability");
    check(rng_state_with_randf(0.0f, chance, &A.rng) == 1,
          "found a PCG state whose first draw is inside the explode chance");

    (void)bt_melee_hit_enemy(&A, slot, plank, &st, 0, MELEE_WX(&A), MELEE_WY(&A), 0.0f,
                             rows, A.wpn_fxcurse[0], n_rows);
    int pj = -1;
    for (int i = 0; i < BT_MAX_EXPL; i++)
        if (A.expl.e[i].alive && (pj < 0 || i < pj)) pj = i;
    check(pj >= 0, "the unit-side ExplodingEffect spawned an explosion");
    if (pj >= 0) {
        check(A.expl.e[pj].has_src == 1, "the melee explosion carries the firing weapon as its source");
        check(A.expl.e[pj].src_uid == A.wpn_uid[slot], "source uid = the melee copy's uid");
        check((int)A.expl.e[pj].src_slot == slot, "source slot = the melee copy's slot");
        check((int)A.expl.e[pj].src_fx_first == (int)BT_WEAPONS[plank].fx_first &&
              (int)A.expl.e[pj].src_fx_count == (int)BT_WEAPONS[plank].fx_count,
              "source rows = the weapon's own canonical fx span");
        /* unit.gd:552-570: an exploding hit deals NO direct damage. */
        check(A.ehp[0] == 1.0f, "the exploding hit itself deals no direct damage");

        /* Let the explosion resolve: the kill must be credited to the weapon. */
        int kills0 = (int)A.wfx.wave_kills[slot];
        float reward = 0.0f;
        int killed = 0;
        for (int i = 0; i < 4 && !killed; i++) killed = bt_item_expl_tick(&A, &reward);
        check(killed >= 1, "the explosion killed the target");
        check((int)A.wfx.wave_kills[slot] == kills0 + 1,
              "the explosion's kill is credited to the firing melee weapon (weapon.gd:203-230)");
    }
}

/* =========================================================================
 * 16. get_chance_success with an integer-percent threshold (core: the
 *     gain_random_primary_stats_on_go_to_next_wave site was a 100-quantized
 *     bounded draw; main.gd:254-260 divides in GDScript binary64).
 * ========================================================================= */
static void test_chance_pct(void) {
    reset_world();
    /* chance == 0 -> NO draw (utils.gd:400-401) */
    {
        BtRng before = A.rng;
        check(bt_chance_pct(&A, 0) == 0, "0 % never succeeds");
        check(memcmp(&before, &A.rng, sizeof(BtRng)) == 0, "0 % draws nothing");
    }
    /* 100 % succeeds but still consumes one draw */
    {
        BtRng before = A.rng;
        check(bt_chance_pct(&A, 100) == 1, "100 % always succeeds");
        check(memcmp(&before, &A.rng, sizeof(BtRng)) != 0, "100 % still consumes one draw");
        BtRng r = before;
        (void)bt_rng_next(&r);
        check(r.state == A.rng.state, "100 % consumes exactly ONE draw");
    }
    /* The decision is `randf() <= pct/100` on the SAME draw the engine makes:
     * a state whose randf() is just under the threshold succeeds, just over
     * fails, and the boundary itself succeeds (inclusive `<=`). */
    {
        int thr = 30;
        BtRng below, above;
        check(rng_state_with_randf(0.0f, (float)thr / 100.0f * 0.999f, &below),
              "found a draw below the 30 % threshold");
        check(rng_state_with_randf((float)thr / 100.0f * 1.001f, 0.999f, &above),
              "found a draw above the 30 % threshold");
        A.rng = below;
        check(bt_chance_pct(&A, thr) == 1, "a draw below the threshold succeeds");
        A.rng = above;
        check(bt_chance_pct(&A, thr) == 0, "a draw above the threshold fails");
    }
    /* The Wave begin applies it: with a candy bag the +stats are PERMANENT
     * (RunData.add_stat), and the count is drawn per row. */
    check(bt_chance_pct(&A, 100) == 1, "candy-bag row (chance 100) always fires");
}

int main(void) {
    printf("test_brotato_weapon_melee\n");
    test_thrust_animation();
    test_sweep_animation();
    test_engine_frame_accounting();
    test_knockback_attacker_is_player();
    test_slot_hooks();
    test_thrust_hit_once();
    test_sweep_multi_target();
    test_repeat_cadence();
    test_alternation();
    test_core_step_end_to_end();
    test_onhit_children();
    test_hit_identity_gate();
    test_crit_kill_gold_direct();
    test_stats_changed_clamp();
    test_wave_begin_draw();
    test_target_membership();
    test_melee_hits_neutral_tree();
    test_explosion_source_credit();
    test_chance_pct();
    printf("  %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
