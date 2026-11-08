/* tests/test_brotato_enemy_hit_effects.cpp
 *
 * Source-backed regressions for the enemy_percent_damage_taken debuff runtime
 * (ocean/brotato/brotato_enemy_hit_effects.h, owner EnemyHitEffects):
 *
 *   effect_behaviors/enemies/percent_damage_taken_enemy_effect_behavior.gd
 *     :44-58  on_hurt collects the player ITEM bucket first, then the hitbox's
 *             own rows whose custom_key_hash is enemy_percent_damage_taken
 *     :65-71  try_add_effects: apply only when find_scaling_stat(effect.key,
 *             scaling_stats) OR effect.key == stat_all
 *     :73-118 add_active_effect: the max_procs gate runs BEFORE the merge, a
 *             repeat source_id max-merges pct/duration/max_stacks, refreshes the
 *             timer and adds ONE stack up to max_stacks, restarting the proc
 *             count at 1 on (re)creation
 *     :121-137 expiry removes the record's OWN added total
 *     :23-28  _process: timer -= delta every frame, expire at <= 0; the
 *             ENGINE was measured (pinned Godot 3.7, --fixed-fps 60, delta
 *             0.016667) to expire the 3 s debuff on frame 180
 *   entities/units/unit/unit.gd:661-707 (on_burned before the burn tick)
 *   dlcs/dlc_1/dlc_1_data.gd:261 (a cursed effect value is boosted positively)
 *
 * Canonical producers under test (rows are BT_ITEM_EFF_ROWS ids):
 *   461  ice_cube item  PercentDamageEffect        key stat_elemental_damage
 *        value 10, duration_secs 3, max_stacks 1, max_procs 1, aux_key 203
 *   857  lute 1         WeaponPercentDamageEffect  key stat_all  max_stacks 3
 *   863  lute 2                                    key stat_all  max_stacks 5
 *   864  lute 3                                    key stat_all  max_stacks 7
 *   865  lute 4                                    key stat_all  max_stacks 10
 *        all four share ONE identity (aux_key 468) and max_procs -1
 *
 * Build and run:
 *     c++ -std=c++17 -O2 -ffp-contract=off -o build/test_brotato_enemy_hit_effects \
 *         tests/test_brotato_enemy_hit_effects.cpp -lm
 *     ./build/test_brotato_enemy_hit_effects
 *
 * Exit code 0 = all checks passed.
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
static void check_int(long got, long want, const char* msg) {
    g_checks++;
    if (got != want) { g_failures++; printf("  FAIL: %s (got %ld, want %ld)\n", msg, got, want); }
}

static BtState A;

/* Clean world: Bull (no weapons, so nothing else registers a bucket row), no
 * enemies; slots are addressed directly because the debuff lives per enemy slot. */
static void reset_world(void) {
    bt_core_init(&A, 12345u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
    A.wave = 1;
    A.n_live_groups = 0;
    A.time_left = 1.0e9f;
    for (int i = 0; i < BT_MAX; i++) { bt_enemy_set_alive(&A, i, 0); A.eneutral[i] = 0; }
    A.ecount = 0;
    A.e_used = BT_MAX;   /* debuff fixtures address dead slots directly */
}

static const unsigned short ROW_ICE = 461;
static const unsigned short ROW_LUTE1 = 857;
static const unsigned short ROW_LUTE2 = 863;
static const unsigned short ROW_LUTE3 = 864;
static const unsigned short ROW_LUTE4 = 865;

/* the item bucket needs the item OWNED (its row registered by bt_apply_item) */
static void own_ice_cube(void) { bt_apply_item(&A, BT_ITEM_IDX_ICE_CUBE); }

static unsigned int elem_mask(void) {
    return bt_enemy_hit_mask_add(0u, (unsigned short)BT_ISTR_STAT_ELEMENTAL_DAMAGE);
}
static unsigned int weapon_mask(const char* what) {
    (void)what;
    /* a plain (non-elemental) melee weapon's view, e.g. stat_melee_damage */
    return bt_enemy_hit_mask_add(0u, (unsigned short)BT_ISTR_STAT_MELEE_DAMAGE);
}

/* --- 1. the gate: stat_all always, elemental only with the elemental bit --- */
static void test_scaling_gate(void) {
    printf("scaling gate\n");
    int j = 0;

    reset_world();
    /* lute (stat_all) applies with an EMPTY mask and with any weapon mask */
    check_int(bt_enemy_hit_apply_row(&A, j, ROW_LUTE1, 0.0f, 0u), 1,
              "lute row applies with an empty scaling view (stat_all)");
    check_int(bt_enemy_hit_percent_taken(&A, j), 10, "lute adds its 10 percent");
    check_int(bt_enemy_hit_apply_row(&A, j, ROW_LUTE2, 0.0f, weapon_mask("melee")), 1,
              "lute row applies with a melee-damage view");

    /* ice_cube (stat_elemental_damage) is gated by the hit's view */
    reset_world();
    own_ice_cube();
    bt_enemy_hit_bucket_apply(&A, j, weapon_mask("melee"));
    check_int(bt_enemy_hit_percent_taken(&A, j), 0,
              "ice_cube does NOT apply to a non-elemental hit");
    bt_enemy_hit_bucket_apply(&A, j, 0u);
    check_int(bt_enemy_hit_percent_taken(&A, j), 0,
              "ice_cube does NOT apply to a hit with no scaling view");
    bt_enemy_hit_bucket_apply(&A, j, elem_mask());
    check_int(bt_enemy_hit_percent_taken(&A, j), 10,
              "ice_cube applies to an elemental hit");
    /* the burning path is the same bucket pass with the elemental scaling */
    reset_world();
    own_ice_cube();
    bt_enemy_hit_burn_apply(&A, j);
    check_int(bt_enemy_hit_percent_taken(&A, j), 10,
              "on_burned applies ice_cube (BurningData defaults to elemental)");

    /* a neutral (tree) slot never carries the debuff (Enemy-only behaviors) */
    reset_world();
    A.eneutral[j] = 1;
    check_int(bt_enemy_hit_apply_row(&A, j, ROW_LUTE1, 0.0f, 0u), 0, "neutral: row refused");
    own_ice_cube();
    bt_enemy_hit_bucket_apply(&A, j, elem_mask());
    check_int(bt_enemy_hit_percent_taken(&A, j), 0, "neutral: no debuff");
}

/* --- 2. mixed lute tiers, ONE identity: merge, cap, source order --- */
static void test_mixed_tiers_same_identity(void) {
    printf("mixed lute tiers share one identity\n");
    int j = 1;
    reset_world();

    check_int(bt_enemy_hit_apply_row(&A, j, ROW_LUTE1, 0.0f, 0u), 1, "lute1 applies");
    check_int((long)A.ehitfx.n_rec[j], 1, "one record after lute1");
    check_int((long)A.ehitfx.stacks[j][0], 1, "stack 1");
    check_int((long)A.ehitfx.maxstacks[j][0], 3, "max_stacks 3 (tier 1)");
    check_int((long)A.ehitfx.src[j][0], 468, "identity is the shared aux_key 468");

    check_int(bt_enemy_hit_apply_row(&A, j, ROW_LUTE2, 0.0f, 0u), 1, "lute2 applies");
    check_int((long)A.ehitfx.n_rec[j], 1, "still ONE record: all tiers share the source_id");
    check_int((long)A.ehitfx.maxstacks[j][0], 5, "max_stacks max-merged to 5");
    check_int((long)A.ehitfx.stacks[j][0], 2, "second stack");
    check_int(bt_enemy_hit_percent_taken(&A, j), 20, "two stacks = 20 percent");

    check_int(bt_enemy_hit_apply_row(&A, j, ROW_LUTE3, 0.0f, 0u), 1, "lute3 applies");
    check_int((long)A.ehitfx.maxstacks[j][0], 7, "max_stacks max-merged to 7");
    check_int(bt_enemy_hit_apply_row(&A, j, ROW_LUTE4, 0.0f, 0u), 1, "lute4 applies");
    check_int((long)A.ehitfx.maxstacks[j][0], 10, "max_stacks max-merged to 10");
    check_int((long)A.ehitfx.stacks[j][0], 4, "four stacks after four tiers");
    check_int(bt_enemy_hit_percent_taken(&A, j), 40, "four stacks = 40 percent");

    /* cap at max_stacks = 10 */
    for (int k = 0; k < 20; k++) (void)bt_enemy_hit_apply_row(&A, j, ROW_LUTE4, 0.0f, 0u);
    check_int((long)A.ehitfx.stacks[j][0], 10, "stacks capped at max_stacks 10");
    check_int(bt_enemy_hit_percent_taken(&A, j), 100, "ten stacks = 100 percent");
    check_int((long)A.ehitfx.added[j][0], 100, "the record's own added total is 100");

    /* a DIFFERENT source gets its own record */
    reset_world();
    own_ice_cube();
    (void)bt_enemy_hit_apply_row(&A, j, ROW_LUTE1, 0.0f, 0u);
    bt_enemy_hit_bucket_apply(&A, j, elem_mask());
    check_int((long)A.ehitfx.n_rec[j], 2, "lute + ice_cube are two records");
    check_int(bt_enemy_hit_percent_taken(&A, j), 20, "10 (lute) + 10 (ice_cube)");

    /* the item bucket pass is the collection order the source uses */
    reset_world();
    own_ice_cube();
    bt_enemy_hit_bucket_apply(&A, j, elem_mask());
    check_int(bt_enemy_hit_apply_row(&A, j, ROW_LUTE4, 0.0f, 0u), 1, "row after the bucket");
    check_int((long)A.ehitfx.n_rec[j], 2, "bucket + row = two records regardless of order");
}

/* --- 3. the max_procs gate runs BEFORE the merge/refresh --- */
static void test_proc_gate_before_refresh(void) {
    printf("proc gate precedes refresh\n");
    int j = 2;
    reset_world();
    own_ice_cube();

    bt_enemy_hit_bucket_apply(&A, j, elem_mask());
    check_int((long)A.ehitfx.n_rec[j], 1, "ice_cube record created");
    check_int((long)A.ehitfx.proc[j][0], 1, "proc count 1");
    check_int((long)A.ehitfx.stacks[j][0], 1, "one stack (max_stacks 1)");

    /* one frame of the frame clock */
    bt_enemy_hit_tick(&A);
    unsigned long t1 = (unsigned long)A.ehitfx.timer[j][0];
    check_int((long)t1, 3 * BT_NEH_FRAMES_PER_SEC - 1, "timer counted down one frame");

    /* max_procs 1: the second application must neither stack nor REFRESH */
    bt_enemy_hit_bucket_apply(&A, j, elem_mask());
    check_int((long)A.ehitfx.timer[j][0], (long)t1, "a proc-limited repeat does NOT refresh the timer");
    check_int((long)A.ehitfx.stacks[j][0], 1, "no extra stack");
    check_int(bt_enemy_hit_percent_taken(&A, j), 10, "no extra percent");

    /* ... and the proc count persists past expiry, so it can never re-apply */
    for (int k = 0; k < 400; k++) bt_enemy_hit_tick(&A);
    check_int((long)A.ehitfx.active[j][0], 0, "expired");
    check_int(bt_enemy_hit_percent_taken(&A, j), 0, "expiry removed its own 10");
    bt_enemy_hit_bucket_apply(&A, j, elem_mask());
    check_int((long)A.ehitfx.active[j][0], 0, "ice_cube is ONCE per enemy: still inactive");
    check_int(bt_enemy_hit_percent_taken(&A, j), 0, "still no debuff");

    /* an unlimited source (lute, max_procs -1) DOES come back after expiry */
    reset_world();
    (void)bt_enemy_hit_apply_row(&A, j, ROW_LUTE4, 0.0f, 0u);
    for (int k = 0; k < 400; k++) bt_enemy_hit_tick(&A);
    check_int((long)A.ehitfx.active[j][0], 0, "lute expired");
    check_int(bt_enemy_hit_percent_taken(&A, j), 0, "lute percent removed");
    check_int(bt_enemy_hit_apply_row(&A, j, ROW_LUTE4, 0.0f, 0u), 1, "lute re-applies");
    check_int((long)A.ehitfx.stacks[j][0], 1, "recreation restarts at one stack");
    check_int((long)A.ehitfx.proc[j][0], 1, "recreation restarts the proc count at 1");
    check_int(bt_enemy_hit_percent_taken(&A, j), 10, "and adds its 10 again");
}

/* --- 4. the source clock: the MEASURED engine expiry (3 s -> 180 frames) --- */
static void test_engine_expiry(void) {
    printf("engine expiry oracle (fixed-fps 60, delta 0.016667)\n");
    int j = 3;
    reset_world();
    (void)bt_enemy_hit_apply_row(&A, j, ROW_LUTE1, 0.0f, 0u);
    check_int((long)A.ehitfx.timer[j][0], 3 * BT_NEH_FRAMES_PER_SEC,
              "timer = duration_secs * 60 frames");

    /* The pinned Godot 3.7 oracle (--fixed-fps 60) delivers _process delta
     * 0.016667 and reports the 3 s debuff expiring on frame 180 with a negative
     * remainder; frame 179 is still live. */
    for (int k = 0; k < 179; k++) bt_enemy_hit_tick(&A);
    check((long)A.ehitfx.timer[j][0] == 1, "one frame of clock left after 179 ticks");
    check_int((long)A.ehitfx.active[j][0], 1, "still active at frame 179 (engine oracle)");
    check_int(bt_enemy_hit_percent_taken(&A, j), 10, "and still debuffed");
    bt_enemy_hit_tick(&A);
    check((long)A.ehitfx.timer[j][0] == 0, "clock reaches 0 at frame 180");
    check_int((long)A.ehitfx.active[j][0], 0, "expires on frame 180 (NOT at an assumed 181)");
    check_int(bt_enemy_hit_percent_taken(&A, j), 0, "its contribution is gone");

    /* a merge restores the FULL duration */
    reset_world();
    (void)bt_enemy_hit_apply_row(&A, j, ROW_LUTE1, 0.0f, 0u);
    for (int k = 0; k < 100; k++) bt_enemy_hit_tick(&A);
    check((long)A.ehitfx.timer[j][0] < 3 * BT_NEH_FRAMES_PER_SEC, "timer partly elapsed");
    (void)bt_enemy_hit_apply_row(&A, j, ROW_LUTE1, 0.0f, 0u);   /* one more stack */
    check_int((long)A.ehitfx.timer[j][0], 3 * BT_NEH_FRAMES_PER_SEC,
              "a merge refreshes the timer to the full duration");
    check_int((long)A.ehitfx.stacks[j][0], 2, "and adds a stack");

    /* every shipped producer carries duration_secs 3 */
    check_int((long)BT_ITEM_EFF_ROWS[ROW_LUTE1].duration_secs, 3, "lute duration_secs 3");
    check_int((long)BT_ITEM_EFF_ROWS[ROW_ICE].duration_secs, 3, "ice_cube duration_secs 3");
}

/* --- 5. per-life + per-record semantics --- */
static void test_lifecycle(void) {
    printf("lifecycle: spawn/respawn/reset and per-record expiry\n");
    int j = 4;
    reset_world();

    (void)bt_enemy_hit_apply_row(&A, j, ROW_LUTE4, 0.0f, 0u);
    bt_enemy_hit_respawn(&A, j);
    check_int(bt_enemy_hit_percent_taken(&A, j), 0, "a pooled respawn clears the debuff");
    check_int((long)A.ehitfx.n_rec[j], 0, "respawn clears every record (fresh behavior node)");

    (void)bt_enemy_hit_apply_row(&A, j, ROW_LUTE1, 0.0f, 0u);
    (void)bt_enemy_hit_apply_row(&A, j, ROW_LUTE4, 0.0f, 0u);
    bt_enemy_hit_reset(&A);
    check_int(bt_enemy_hit_percent_taken(&A, j), 0, "bt_core_reset clears the block");
    check_int((long)A.ehitfx.overflow, 0, "and clears the overflow flag");

    /* only the expiring record's own contribution leaves the total */
    reset_world();
    own_ice_cube();
    bt_enemy_hit_bucket_apply(&A, j, elem_mask());     /* ice_cube: 3 s, 1 stack */
    for (int k = 0; k < 120; k++) bt_enemy_hit_tick(&A);
    (void)bt_enemy_hit_apply_row(&A, j, ROW_LUTE4, 0.0f, 0u);   /* lute: 3 s, 1 stack */
    check_int(bt_enemy_hit_percent_taken(&A, j), 20, "10 + 10");
    /* the ice_cube record is ~60 frames older: run until it expires but the
     * lute record is still alive */
    for (int k = 0; k < 61; k++) bt_enemy_hit_tick(&A);
    check_int((long)A.ehitfx.active[j][0], 0, "the older ice_cube record expired");
    check_int((long)A.ehitfx.active[j][1], 1, "the younger lute record is still active");
    check_int(bt_enemy_hit_percent_taken(&A, j), 10, "only the expired 10 was removed");

    /* the mask helper is the canonical id mapping */
    check(bt_enemy_hit_mask_add(0u, (unsigned short)BT_ISTR_STAT_MELEE_DAMAGE) ==
          (1u << bt_core_id_from_istr(BT_ISTR_STAT_MELEE_DAMAGE)),
          "mask bit == the canonical core stat id");
    check_int(bt_enemy_hit_row_ok((unsigned short)BT_ISTR_STAT_ALL, 0u), 1,
              "stat_all matches with an empty view");
    check_int(bt_enemy_hit_row_ok((unsigned short)BT_ISTR_STAT_ELEMENTAL_DAMAGE, 0u), 0,
              "elemental does not match an empty view");
}

int main(void) {
    memset(&A, 0, sizeof(A));
    test_scaling_gate();
    test_mixed_tiers_same_identity();
    test_proc_gate_before_refresh();
    test_engine_expiry();
    test_lifecycle();

    printf("test_brotato_enemy_hit_effects: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
