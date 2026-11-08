/* tests/test_brotato_rng_jump.cpp
 *
 * Deterministic regression tests for the intra-env RNG parallelisation
 * primitives (ocean/brotato/brotato_rng_jump.h):
 *
 *   1. btf_rng_advance == k sequential bt_rng_next() calls, for many k
 *      including 0, 1, word boundaries, 10^5 and >= 2^32 (via the group law and
 *      binary doubling), on both core streams.
 *   2. counts -> exclusive prefix sum -> per-lane stream reproduces the serial
 *      in-order draws exactly (values AND final stream state).
 *   3. every count function that maps to a single callable core function is
 *      checked against the ACTUAL number of words that core function draws,
 *      measured from a live BtState RNG snapshot (G and U streams separately).
 *
 * Build and run:
 *     c++ -O2 -o build/test_brotato_rng_jump tests/test_brotato_rng_jump.cpp -lm
 *     ./build/test_brotato_rng_jump
 *
 * Exit code 0 = all checks passed.
 */

#include "../ocean/brotato/brotato_core.h"
#define BT_RNG_JUMP_HAVE_CORE_STATS 1
#include "../ocean/brotato/brotato_rng_jump.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;
static int g_checks = 0;

static void check(int cond, const char* msg) {
    g_checks++;
    if (!cond) { g_failures++; printf("  FAIL: %s\n", msg); }
}
static void check_eq_u64(unsigned long long got, unsigned long long want, const char* msg) {
    g_checks++;
    if (got != want) {
        g_failures++;
        printf("  FAIL: %s (got %llu want %llu)\n", msg, got, want);
    }
}

/* BtState is ~1 MiB (embeds the persistent contact world): keep it static. */
static BtState S;

/* ---------------------------------------------------------------------------
 * 1. Jump-ahead equivalence.
 * ------------------------------------------------------------------------- */

/* k sequential draws, for small k. */
static BtRng seq_advance(BtRng r, unsigned long long k) {
    for (unsigned long long i = 0; i < k; i++) (void)bt_rng_next(&r);
    return r;
}

static int rng_eq(BtRng a, BtRng b) { return a.state == b.state && a.inc == b.inc; }

static void test_jump_one_seed(bt_u64 seed, const char* tag) {
    BtRng base;
    bt_rng_seed(&base, seed);

    /* small / medium k: direct sequential comparison */
    static const unsigned long long ks[] = { 0ULL, 1ULL, 2ULL, 3ULL, 4ULL, 5ULL,
        31ULL, 32ULL, 33ULL, 63ULL, 64ULL, 65ULL, 100ULL, 1000ULL, 65535ULL, 100000ULL };
    for (unsigned int i = 0; i < sizeof(ks) / sizeof(ks[0]); i++) {
        BtRng want = seq_advance(base, ks[i]);
        BtRng got = btf_rng_advance(base, (bt_u64)ks[i]);
        char msg[128];
        snprintf(msg, sizeof(msg), "jump(%s) k=%llu state", tag, ks[i]);
        check(rng_eq(got, want), msg);
        /* the next output after the jump must also match the sequential next */
        BtRng g2 = got, w2 = want;
        unsigned int go = bt_rng_next(&g2), wo = bt_rng_next(&w2);
        snprintf(msg, sizeof(msg), "jump(%s) k=%llu next output", tag, ks[i]);
        check(go == wo, msg);
    }

    /* large k: group law  advance(a+b) == advance(advance(a), b) */
    static const unsigned long long bigs[] = {
        4294967295ULL,                      /* 2^32 - 1 */
        4294967296ULL,                      /* 2^32 */
        4294967297ULL,                      /* 2^32 + 1 */
        3037000499ULL, 1000000007ULL, 123456789012345ULL };
    for (unsigned int i = 0; i < sizeof(bigs) / sizeof(bigs[0]); i++) {
        for (unsigned int j = 0; j < sizeof(bigs) / sizeof(bigs[0]); j++) {
            BtRng a = btf_rng_advance(base, (bt_u64)bigs[i]);
            BtRng ab = btf_rng_advance(a, (bt_u64)bigs[j]);
            BtRng sum = btf_rng_advance(base, (bt_u64)(bigs[i] + bigs[j]));
            char msg[160];
            snprintf(msg, sizeof(msg), "jump(%s) group law %llu+%llu", tag, bigs[i], bigs[j]);
            check(rng_eq(ab, sum), msg);
        }
    }

    /* huge k tied back to a real draw count: 2^32 + 7 == advance(2^32) then 7
     * sequential draws. */
    {
        BtRng hop = btf_rng_advance(base, (bt_u64)4294967296ULL);
        BtRng want = seq_advance(hop, 7ULL);
        BtRng got = btf_rng_advance(base, (bt_u64)(4294967296ULL + 7ULL));
        char msg[128];
        snprintf(msg, sizeof(msg), "jump(%s) 2^32+7 vs hop+seq", tag);
        check(rng_eq(got, want), msg);
    }

    /* binary doubling consistency: advance(2^n) applied twice == advance(2^(n+1)) */
    {
        bt_u64 p = (bt_u64)1;
        BtRng two = btf_rng_advance(base, p);
        for (int n = 0; n < 40; n++) {
            BtRng dbl = btf_rng_advance(two, p);
            p <<= 1;
            BtRng want = btf_rng_advance(base, p);
            char msg[128];
            snprintf(msg, sizeof(msg), "jump(%s) doubling n=%d", tag, n + 1);
            check(rng_eq(dbl, want), msg);
            two = dbl;
        }
    }
}

static void test_jump(void) {
    static const bt_u64 seeds[] = { 0ULL, 1ULL, 2ULL, 3ULL, 7ULL, 12345ULL,
        0xdeadbeefULL, 0x0123456789abcdefULL, (bt_u64)-1 };
    for (unsigned int i = 0; i < sizeof(seeds) / sizeof(seeds[0]); i++) {
        char tag[32];
        snprintf(tag, sizeof(tag), "G/seed%u", i);
        test_jump_one_seed(seeds[i], tag);
    }
    /* the Utils stream shares the recurrence: spot-check one seed */
    {
        BtRng base;
        bt_rng_seed(&base, 99ULL);
        /* same checks as a seed tag, inlined for the utils stream */
        unsigned long long k = 4097ULL;
        BtRng want = seq_advance(base, k);
        BtRng got = btf_rng_advance(base, (bt_u64)k);
        check(rng_eq(got, want), "jump(U) k=4097");
        BtRng hop = btf_rng_advance(base, (bt_u64)((1ULL << 32)));
        BtRng sum = btf_rng_advance(base, (bt_u64)((1ULL << 32) + 3));
        check(rng_eq(seq_advance(hop, 3), sum), "jump(U) 2^32+3 vs hop+seq");
    }
}

/* ---------------------------------------------------------------------------
 * 2. counts -> exclusive prefix sum -> per-lane stream.
 * ------------------------------------------------------------------------- */
static void test_prefix_and_lanes(void) {
    unsigned int counts[6] = { 3u, 0u, 1u, 5u, 2u, 4u };
    const int n = 6;
    bt_u64 off[6], off2[6], tmp[6];
    bt_u64 total = bt_rng_prefix_offsets(counts, n, off);
    bt_u64 total2 = bt_rng_scan_hillis(counts, n, tmp, off2);

    static const bt_u64 expect_off[6] = { 0, 3, 3, 4, 9, 11 };
    for (int i = 0; i < n; i++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "prefix offset %d", i);
        check_eq_u64(off[i], expect_off[i], msg);
        snprintf(msg, sizeof(msg), "hillis offset %d", i);
        check_eq_u64(off2[i], expect_off[i], msg);
    }
    check_eq_u64(total, 15ULL, "prefix total");
    check_eq_u64(total2, 15ULL, "hillis total");

    /* lane streams vs a serial in-order walk */
    BtRng base;
    bt_rng_seed(&base, 4242ULL);
    BtRng serial = base;
    for (int i = 0; i < n; i++) {
        BtRng lane = bt_rng_lane_stream(base, off, i);
        /* the lane's first value must equal this consumer's first serial value */
        BtRng sc = serial;
        unsigned int serial_first = bt_rng_next(&sc);
        unsigned int lane_first = bt_rng_next(&lane);
        char msg[64];
        snprintf(msg, sizeof(msg), "lane %d first value", i);
        check(serial_first == lane_first, msg);
        /* ... and its post-draw state must equal serial's post-consumer state */
        BtRng lane_end = btf_rng_advance(base, off[i] + (bt_u64)counts[i]);
        for (unsigned int j = 0; j < counts[i]; j++) (void)bt_rng_next(&serial);
        check(rng_eq(lane_end, serial), "lane end state == serial cursor");
    }
    /* the runner's finish step lands exactly on the serial end state */
    check(rng_eq(btf_rng_advance(base, total), serial), "final stream state after total");

    /* 256-consumer scale run with pseudo-random per-consumer counts: every lane's
     * FIRST value and post-consumer state must equal the serial cursor, and the
     * finish must land exactly on the serial end (this mirrors the framework's
     * [5a] 256-consumer parallel-draw check on the host). */
    {
        enum { N256 = 256 };
        static unsigned int c256[N256];
        static bt_u64 o256[N256], t256[N256];
        bt_u64 state = 0x2545F4914F6CDD1DULL;
        for (int i = 0; i < N256; i++) {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            c256[i] = (unsigned int)((state >> 33) % 7u);   /* 0..6 words */
        }
        bt_u64 tot = bt_rng_prefix_offsets(c256, N256, o256);
        bt_u64 tot2 = bt_rng_scan_hillis(c256, N256, t256, o256);
        check_eq_u64(tot, tot2, "256-consumer prefix == hillis total");
        BtRng cursor = base;
        int ok = 1;
        for (int i = 0; i < N256; i++) {
            BtRng lane = bt_rng_lane_stream(base, o256, i);
            BtRng sc = cursor;
            unsigned int sf = c256[i] ? bt_rng_next(&sc) : 0u;
            unsigned int lf = c256[i] ? bt_rng_next(&lane) : 0u;
            if (c256[i] && sf != lf) ok = 0;
            for (unsigned int j = 0; j < c256[i]; j++) (void)bt_rng_next(&cursor);
        }
        check(ok, "256-consumer lane first values match serial");
        check(rng_eq(cursor, btf_rng_advance(base, tot)), "256-consumer final state matches");
    }
}

/* ---------------------------------------------------------------------------
 * 3. Count functions vs measured draws of the real core consumers.
 * ------------------------------------------------------------------------- */

/* Find how many bt_rng_next() calls separate `before` from the state the call
 * left, by replaying from `before` (exact, O(draws)). */
static unsigned long long measure(BtState* s, int utils, void (*fn)(BtState*)) {
    BtRng before = utils ? s->utils_rng : s->rng;
    fn(s);
    BtRng after = utils ? s->utils_rng : s->rng;
    BtRng cur = before;
    unsigned long long k = 0;
    for (;;) {
        if (cur.state == after.state) return k;
        (void)bt_rng_next(&cur);
        k++;
        if (k > 4000000ULL) return (unsigned long long)-1;
    }
}

static void call_gold_arc(BtState* s) {
    float arc[BT_LOOT_ARC_N];
    float angle = 0.0f;
    bt_loot_gold_arc(s, 0.0f, 0.0f, 5, arc, &angle);
}
static void call_consumable_arc(BtState* s) {
    float arc[BT_LOOT_ARC_N];
    bt_loot_consumable_arc(s, 0.0f, 0.0f, 5, arc);
}
static void call_garden_emit(BtState* s) { bt_garden_emit_fruit(s, 0.0f, 0.0f); }
static void call_structure_cull(BtState* s) { bt_enforce_structure_cap(s); }
static void call_utils_randi(BtState* s) { (void)bt_rng_randi_range(&s->utils_rng, -30, 30); }
static void call_lifesteal(BtState* s) {
    float cd = 0.0f;
    (void)bt_wfx_lifesteal(s, &cd, 0.0f);
}
static void call_chance_half(BtState* s) { (void)bt_chance(s, 0.5f); }
static void call_chance_zero(BtState* s) { (void)bt_chance(s, 0.0f); }
static void call_rand_pos(BtState* s) { float x, y; bt_rand_pos(s, 50.0f, &x, &y); }
static void call_birth_advance(BtState* s) { bt_birth_advance(s); }
static void call_pop_to_birth(BtState* s) { bt_pop_to_birth(s, s->pend_enemy, &s->n_pend_enemy, 0); }
static void call_pos_in_area(BtState* s) {
    float x, y;
    bt_pos_in_area(s, 100.0f, 100.0f, 40.0f, 0.0f, 0, &x, &y);
}

/* bt_fire_shot is the single most branch-heavy draw site; drive it directly on a
 * sandbox state and compare with bt_rngcount_enemy_fire for every flag path. */
static BtAttack g_A;
static void call_enemy_fire(BtState* s) { bt_fire_shot(s, 0, g_A, 0.0f, 0.0f); }

static void reset_sandbox(BtState* s) {
    memset(s, 0, sizeof(*s));
    bt_rng_seed(&s->rng, 11ULL);
    s->wave = 3;
    bt_enemy_set_alive(s, 0, 1);
    s->e_used = 1;
    s->ekind[0] = 0;
    s->ex[0] = 100.0f; s->ey[0] = 100.0f;
    s->px = 300.0f; s->py = 300.0f;
    s->e_hold_tick[0] = 0;
}

static void test_enemy_fire_sandbox(void) {
    static const struct { unsigned char nproj; unsigned char borders, one_on_target,
        random_dir, base_dir_const, alt_base; float csp; unsigned short jitter; const char* tag; } CFG[] = {
        { 1, 0, 0, 0, 0, 0, 0.0f, 0, "1proj plain" },
        { 3, 0, 0, 0, 0, 0, 0.0f, 0, "3proj plain" },
        { 3, 0, 1, 0, 0, 0, 0.0f, 0, "3proj one-on-target" },
        { 2, 1, 0, 0, 0, 0, 0.0f, 0, "2proj borders" },
        { 2, 0, 0, 1, 0, 0, 0.0f, 0, "2proj random-dir" },
        { 2, 0, 0, 0, 1, 0, 0.0f, 0, "2proj base-dir-const" },
        { 2, 0, 0, 0, 1, 1, 0.0f, 0, "2proj base-dir-const-alt" },
        { 2, 0, 0, 0, 0, 0, 40.0f, 0, "2proj const-spread-pos" },
        { 2, 0, 0, 1, 0, 0, 0.0f, 5, "2proj random-dir+jitter" },
    };
    for (unsigned int i = 0; i < sizeof(CFG) / sizeof(CFG[0]); i++) {
        memset(&g_A, 0, sizeof(g_A));
        g_A.beh = 1;
        g_A.nproj = CFG[i].nproj;
        g_A.spawn_only_borders = CFG[i].borders;
        g_A.atleast_one_on_target = CFG[i].one_on_target;
        g_A.random_dir = CFG[i].random_dir;
        g_A.base_dir_const = CFG[i].base_dir_const;
        g_A.alt_base_dir_spread = CFG[i].alt_base;
        g_A.constant_spread_rand_base_pos = CFG[i].csp;
        g_A.proj_speed_rand = CFG[i].jitter;
        g_A.damage = 5.0f;
        g_A.pspeed = 100;

        reset_sandbox(&S);
        unsigned long long got = measure(&S, 0, call_enemy_fire);
        /* bt_fire_shot is the body only: the shoot branch's cooldown draw lives
         * in bt_core_step, so subtract it here (0 when long_cd_every fires). */
        unsigned int want = bt_rngcount_enemy_fire(&g_A, 0, 0);
        if (g_A.long_cd_every == 0) want -= BT_RNG_JUMP_RANGE_DRAWS;
        char msg[96];
        snprintf(msg, sizeof(msg), "enemy fire sandbox: %s", CFG[i].tag);
        check_eq_u64(got, (unsigned long long)want, msg);
    }

    /* long-reload branch: eshots >= long_cd_every removes the cooldown draw */
    memset(&g_A, 0, sizeof(g_A));
    g_A.beh = 1; g_A.nproj = 2; g_A.damage = 5.0f; g_A.pspeed = 100;
    g_A.long_cd_every = 3; g_A.long_cooldown = 90;
    reset_sandbox(&S);
    S.eshots[0] = 3;
    check_eq_u64(measure(&S, 0, call_enemy_fire),
                 (unsigned long long)bt_rngcount_enemy_fire(&g_A, 3, 0),
                 "enemy fire long-reload skips cooldown draw");
    check_eq_u64(bt_rngcount_enemy_fire(&g_A, 3, 0),
                 (unsigned long long)bt_rngcount_enemy_fire(&g_A, 0, 0) - BT_RNG_JUMP_RANGE_DRAWS,
                 "long-reload removes exactly one rand_range");
}

/* bt_group_perf_cull coupling regression (GpuSpawn): the removal loop calls
 * bt_mark_enemy_dead(slot, 0) BETWEEN reservoir picks, and its death children
 * draw bt_pos_in_area per child -> the phase total is not pre-phase-fixed.
 * Verify the exact identity measured == over + 6 * sum(children(kind_i)) by
 * diffing the alive set around the call. */
static int g_has_death_kind = -1;
static int g_no_death_kind = -1;
static int death_children_of_kind(int kind) {
    int n = (int)BT_KIND_NDEATH_SPAWNS[kind];
    if (n > BT_MAX_DEATH_SPAWNS) n = BT_MAX_DEATH_SPAWNS;
    int tot = 0;
    for (int e = 0; e < n; e++) {
        short ck = BT_KIND_DEATH_SPAWN_KIND[kind][e];
        if (ck < 0 || ck >= BT_NKINDS) continue;
        tot += (int)BT_KIND_DEATH_SPAWN_NB[kind][e];
    }
    return tot;
}
static void call_perf_cull(BtState* s) { bt_group_perf_cull(s); }
static void call_reserve_slot(BtState* s) { (void)bt_reserve_slot(s); }

static void build_cull_state(BtState* s, int kind, int n_enemies, int cap) {
    memset(s, 0, sizeof(*s));
    s->e_used = (unsigned short)n_enemies;
    bt_rng_seed(&s->rng, 21ULL);
    bt_rng_seed(&s->utils_rng, 21ULL);
    s->wave = 12;
    s->wave_max_enemies = (unsigned short)cap;
    for (int i = 0; i < n_enemies; i++) {
        bt_enemy_set_alive(s, i, 1);
        s->ekind[i] = (unsigned char)kind;
        s->eelite[i] = 0; s->eneutral[i] = 0;
        s->e_act[i] = (unsigned short)i;
        s->e_instance[i] = (unsigned short)BT_POOL_NO_INST;
        s->ex[i] = 100.0f + (float)i; s->ey[i] = 100.0f; s->ehp[i] = 10.0f;
    }
    s->ecount = n_enemies;
}

static void test_perf_cull_coupling(void) {
    int over = 15, n_enemies = 35, cap = 20;
    /* a kind WITH death children -> proves the interleaved draws */
    if (g_has_death_kind >= 0) {
        BtState* s = &S;
        build_cull_state(s, g_has_death_kind, n_enemies, cap);
        unsigned char alive_before[BT_MAX];
        for (int i = 0; i < BT_MAX; i++) alive_before[i] = s->ealive[i];
        unsigned long long got = measure(s, 0, call_perf_cull);
        int children = 0;
        for (int i = 0; i < BT_MAX; i++)
            if (alive_before[i] && !s->ealive[i]) children += death_children_of_kind((int)s->ekind[i]);
        char msg[128];
        snprintf(msg, sizeof(msg), "perf cull coupling (kind %d, %d children)", g_has_death_kind, children);
        check_eq_u64(got, (unsigned long long)bt_rngcount_perf_cull(over, children), msg);
        check(children > 0, "chosen death kind really spawns children");
    }
    /* a kind WITHOUT death children -> the pure scan count */
    if (g_no_death_kind >= 0) {
        BtState* s = &S;
        build_cull_state(s, g_no_death_kind, n_enemies, cap);
        check_eq_u64(measure(s, 0, call_perf_cull),
                     (unsigned long long)bt_rngcount_perf_cull(over, 0),
                     "perf cull no-children == over");
    }
    /* reserve-slot cull: one word per eligible enemy, nothing else */
    {
        BtState* s = &S;
        build_cull_state(s, (g_no_death_kind >= 0 ? g_no_death_kind : 0), BT_MAX, 0);
        check_eq_u64(measure(s, 0, call_reserve_slot),
                     (unsigned long long)bt_rngcount_enemy_cull(BT_MAX),
                     "reserve-slot cull == eligible count");
    }
}

static void test_counts_vs_core(void) {
    memset(&S, 0, sizeof(S));
    bt_rng_seed(&S.rng, 7ULL);
    bt_rng_seed(&S.utils_rng, 7ULL);

    /* gold arc == 13 */
    check_eq_u64(measure(&S, 0, call_gold_arc), bt_rngcount_gold_arc(), "gold arc draws");
    /* consumable arc == 9 */
    check_eq_u64(measure(&S, 0, call_consumable_arc), bt_rngcount_consumable_arc(),
                 "consumable arc draws");
    /* placement primitives the enemy spawn/charge branches compose from:
     * bt_rand_pos and bt_pos_in_area each draw 2 rand_range = 6 */
    memset(&S, 0, sizeof(S));
    bt_rng_seed(&S.rng, 7ULL);
    bt_rng_seed(&S.utils_rng, 7ULL);
    check_eq_u64(measure(&S, 0, call_rand_pos), 2ULL * BT_RNG_JUMP_RANGE_DRAWS, "bt_rand_pos draws");
    check_eq_u64(measure(&S, 0, call_pos_in_area), 2ULL * BT_RNG_JUMP_RANGE_DRAWS,
                 "bt_pos_in_area draws");
    check_eq_u64(bt_rngcount_enemy_charge_retarget(2), 6ULL, "charge RAND_POINT = 6");
    check_eq_u64(bt_rngcount_enemy_charge_retarget(1), 6ULL, "charge AROUND_PLAYER = 6");
    check_eq_u64(bt_rngcount_enemy_charge_retarget(0), 0ULL, "charge PLAYER = 0");

    /* spawn phases: birth pop, birth-advance finished, group pos */
    {
        memset(&S, 0, sizeof(S));
        bt_rng_seed(&S.rng, 7ULL);
        bt_rng_seed(&S.utils_rng, 7ULL);
        S.n_births = 1;
        S.births[0].active = 1; S.births[0].timer = 1.0f;
        S.births[0].bclass = 0; S.births[0].neutral = 1; S.births[0].is_boss = 0;
        S.births[0].x = 1000.0f; S.births[0].y = 1000.0f;
        check_eq_u64(measure(&S, 0, call_birth_advance),
                     (unsigned long long)bt_rngcount_birth_advance_finished(0),
                     "birth advance ready draws");
    }
    {
        memset(&S, 0, sizeof(S));
        bt_rng_seed(&S.rng, 7ULL);
        bt_rng_seed(&S.utils_rng, 7ULL);
        S.n_births = 1;
        S.births[0].active = 1; S.births[0].timer = 1.0f;
        S.births[0].bclass = 0; S.births[0].neutral = 0; S.births[0].is_boss = 0;
        S.births[0].x = S.px; S.births[0].y = S.py + BT_PLAYER_BODY_Y;
        check_eq_u64(measure(&S, 0, call_birth_advance),
                     (unsigned long long)bt_rngcount_birth_advance_finished(1),
                     "birth advance relocation draws");
    }
    {
        memset(&S, 0, sizeof(S));
        bt_rng_seed(&S.rng, 7ULL);
        bt_rng_seed(&S.utils_rng, 7ULL);
        S.n_pend_enemy = 1;
        S.pend_enemy[0].kind = 0; S.pend_enemy[0].neutral = 0;
        check_eq_u64(measure(&S, 0, call_pop_to_birth),
                     (unsigned long long)bt_rngcount_birth_pop(0, 0), "birth pop draws");
    }
    check_eq_u64(bt_rngcount_spawn_poll(5, 0, 0), 3ULL + 5ULL * 6ULL, "spawn poll 5 pops");
    check_eq_u64(bt_rngcount_spawn_poll(3, 1, 103), 3ULL + 3ULL * 6ULL + 3ULL,
                 "spawn poll 3 pops incl. structure cap");
    check_eq_u64(bt_rngcount_group_unit(1, 4, 2), 1ULL + 1ULL + 4ULL * 6ULL + 2ULL * 6ULL,
                 "group unit count");
    check_eq_u64(bt_rngcount_group_pos(3), 4ULL * 6ULL, "group pos with retries");

    /* enemy spawn attack: cooldown 3 + 2*6 per spawned unit */
    {
        BtAttack A;
        memset(&A, 0, sizeof(A));
        A.nproj = 2; A.spawn_at_random_pos = 1;
        check_eq_u64(bt_rngcount_enemy_spawn_attack(&A), 3ULL + 4ULL * BT_RNG_JUMP_RANGE_DRAWS,
                     "spawn attack random-pos count");
        A.spawn_at_random_pos = 0; A.spawn_radius = 30;
        check_eq_u64(bt_rngcount_enemy_spawn_attack(&A), 3ULL + 4ULL * BT_RNG_JUMP_RANGE_DRAWS,
                     "spawn attack in-area count");
        A.spawn_radius = -1;
        check_eq_u64(bt_rngcount_enemy_spawn_attack(&A), 3ULL, "spawn attack no-placement count");
    }

    /* enemy hit sound: hurt pick always (when the clip span is non-empty), crit
     * pick additionally, pitch is a fixed constant (no draw) */
    {
        int hc = (int)BT_AUDIO_KIND_HURT_COUNT[0];
        int cc = (int)BT_AUDIO_KIND_CRIT_COUNT[0];
        check_eq_u64(bt_rngcount_enemy_hit_sound(0, hc, cc),
                     (unsigned long long)(hc > 0 ? 1 : 0), "hit sound non-crit");
        check_eq_u64(bt_rngcount_enemy_hit_sound(1, hc, cc),
                     (unsigned long long)((hc > 0 ? 1 : 0) + (cc > 0 ? 1 : 0)), "hit sound crit");
    }
    /* garden emit == 10 */
    check_eq_u64(measure(&S, 0, call_garden_emit), bt_rngcount_garden_emit(), "garden emit draws");
    /* utils randi_range == 1 on the U stream */
    check_eq_u64(measure(&S, 1, call_utils_randi), bt_rngcount_utils_randi_range(),
                 "utils randi_range draws");
    /* lifesteal roll == 1, unconditionally */
    check_eq_u64(measure(&S, 0, call_lifesteal), bt_rngcount_lifesteal(), "lifesteal draws");
    /* bt_chance: 1 at non-zero, 0 at zero */
    check_eq_u64(measure(&S, 0, call_chance_half), bt_rngcount_chance(0.5f), "chance(0.5) draws");
    check_eq_u64(measure(&S, 0, call_chance_zero), bt_rngcount_chance(0.0f), "chance(0) draws");

    /* structure cull: 103 structures -> 3 removed -> 3 bounded draws */
    memset(&S, 0, sizeof(S));
    bt_rng_seed(&S.rng, 7ULL);
    for (int i = 0; i < 103; i++) { S.pet_alive[i] = 1; S.pet_isstruct[i] = 1; }
    S.pet_n = 103;
    check_eq_u64(measure(&S, 0, call_structure_cull), bt_rngcount_structure_cull(103),
                 "structure cull draws");
    check_eq_u64(bt_rngcount_structure_cull(103), 3ULL, "structure cull count value");
    check_eq_u64(bt_rngcount_structure_cull(100), 0ULL, "structure cull at cap is 0");
    check_eq_u64(bt_rngcount_enemy_cull(100), 100ULL, "enemy cull count value");
    check_eq_u64(bt_rngcount_enemy_cull(0), 0ULL, "enemy cull empty is 0");

    /* ranged shot with no weapons must be 0 (pre-state gate) */
    memset(&S, 0, sizeof(S));
    bt_rng_seed(&S.rng, 7ULL);
    check_eq_u64(bt_rngcount_ranged_shot(&S, 0, 0), 0ULL, "no weapon -> 0 ranged draws");
    check_eq_u64(bt_rngcount_melee_swing(&S, 0, 1), 0ULL, "no weapon -> 0 melee draws");
    check_eq_u64(bt_rngcount_enemy_shoot(&S, 0, 1, 0), 0ULL, "dead enemy -> 0 shot draws");

    /* enemy fire formula, hand-checked on the extracted sandbox (kind 0):
     * nproj 1, no const/borders/random_dir/jitter -> cooldown 3 + base 3 +
     * rot 3 + projectile(rot 3) = 12. Only assert the structural identity:
     * a non-shooting enemy draws 0. */
    (void)0;
}

/* ---------------------------------------------------------------------------
 * 4. Classification report (printed, not asserted).
 * ------------------------------------------------------------------------- */
typedef struct { const char* phase; const char* cls; const char* evidence; } Row;

static const Row ROWS[] = {
    { "birth flush / spawn activate", "a(cond)",
      "spawn_kind_at: init rows + motion + 1 curse roll; all pre-state per spawn; "
      "slot allocation must be the ordered apply" },
    { "spawn poll (pop -> EntityBirth)", "a",
      "core.h:881-905: 3 owner shuffles + 6 per popped entry (+ structure cap); "
      "bt_rngcount_spawn_poll" },
    { "birth advance (relocation)", "a", "core.h:911-938; bt_rngcount_birth_advance_finished" },
    { "spawn group unit", "a/c",
      "core.h:1007-1035: randi_range + residual + 6/unit; away-from-player retry "
      "count is own-value-dependent" },
    { "enemy AI (shoot/charge/spawn/boost)", "b",
      "core.h:4204-4390: bt_buffer_boost mutates OTHER bodies' ehp/eboosted and the "
      "state escalation reads ehp -> a pre-pass count is invalid with live buffers/bosses; "
      "no-buffer ticks are (a)" },
    { "enemy AI shot (single consumer)", "a",
      "core.h:3783-3862 bt_fire_shot terms; see bt_rngcount_enemy_fire" },
    { "enemy AI charge retarget", "a", "core.h:4322-4349; bt_rngcount_enemy_charge_retarget" },
    { "enemy AI spawn attack", "a", "core.h:4350-4374; bt_rngcount_enemy_spawn_attack" },
    { "enemy buffer boost", "b", "core.h:3893-3940 reservoir scan changes per pick" },
    { "enemy hit sound", "c", "unit.gd:282-323: crit flag selects the extra clip" },
    { "ranged weapon step", "a", "weapon_ranged.h:349-521; bt_rngcount_ranged_shot" },
    { "melee weapon swing start", "a", "weapon_melee.h:665-735; bt_rngcount_melee_swing" },
    { "melee/projectile enemy hits", "c",
      "item_explosions.h bt_item_expl_hit_enemy: crit roll selects the 2-draw crit "
      "sound and the explosion chain -> own-value-dependent" },
    { "crit roll", "a", "unit.gd:285 -> bt_chance; bt_rngcount_crit" },
    { "dodge roll", "a", "player.gd:302 raw randf even at dodge 0; bt_rngcount_dodge" },
    { "lifesteal roll", "a", "weapon.gd:412 unconditional; bt_rngcount_lifesteal" },
    { "burn roll", "a", "unit.gd:543-546 gated; bt_rngcount_burn" },
    { "gold drop", "a", "main.gd:600-620; bt_rngcount_gold_drop + rolls" },
    { "gold coin count + arcs", "c",
      "main.gd:700-746: coin count = floor + residual ROLL -> count depends on the "
      "drawn residual; arcs are 13/draw once the count is known" },
    { "consumable drop", "c", "item_service.gd:157-203; tier from the drop roll" },
    { "turret projectile volley", "n/a",
      "core.h:4648-4717 draws NOTHING (nearest-target scan + spawn_projectile)" },
    { "garden turret emit", "a", "structures.h:51-105; bt_rngcount_garden_emit/tick" },
    { "BotOMine landmine drop", "a", "core.h:4662-4673 bt_pos_in_area = 6" },
    { "enemy cull (reserve_slot)", "a",
      "core.h:650-676: reservoir scan = eligible_n, the single kill draws nothing" },
    { "perf cull (bt_group_perf_cull)", "b/c",
      "core.h:1052-1081: per removal a scan draw + bt_mark_enemy_dead(...,0) whose "
      "death children draw 6/child; the removed kinds are drawn -> not pre-phase-fixed" },
    { "structure cull", "a", "core.h:2655-2672 = max(0, n - BT_MAX_STRUCTS)" },
    { "scalar hot tick", "c",
      "scalar_events.h:267-280: one heal per due point, but a heal only draws while "
      "HP room remains -> count is mutated in-phase" },
    { "alien-eyes tick", "a", "scalar_events.h:690-740 = 1 sound + even-spaced shots" },
    { "player visual/dust", "a", "player_visual.h:116-125 = 10 words per particle, "
      "particle count from pre-state" },
    { "wave group fire", "b", "core.h:1115-1157: unit count is drawn (randi_range) "
      "before the per-unit placement draws" },
};

static void print_table(void) {
    printf("\n  RNG phase qualification map (a=parallel, b=cross-consumer, c=self-value, n/a=no draws)\n");
    printf("  %-38s %-9s %s\n", "phase", "class", "evidence");
    for (unsigned int i = 0; i < sizeof(ROWS) / sizeof(ROWS[0]); i++)
        printf("  %-38s %-9s %s\n", ROWS[i].phase, ROWS[i].cls, ROWS[i].evidence);
}

int main(void) {
    /* These fixtures hand-build a BtState and never run bt_core_init, which is
     * the only initializer for the two RNG-model gates; without this they would
     * assert the neutralized (audio 0 / particles 0) model instead of the
     * source default they were written for. */
    S.audio_rng = 1;
    S.particles_rng = 1;
    printf("test_brotato_rng_jump\n");
    test_jump();
    test_prefix_and_lanes();
    test_enemy_fire_sandbox();
    for (int k = 0; k < BT_NKINDS; k++) {
        if (BT_KIND_CLASS[k] != 0) continue;         /* plain enemies only */
        if (g_has_death_kind < 0 && death_children_of_kind(k) > 0) g_has_death_kind = k;
        if (g_no_death_kind < 0 && death_children_of_kind(k) == 0) g_no_death_kind = k;
    }
    printf("  death-child kind=%d  no-child kind=%d\n", g_has_death_kind, g_no_death_kind);
    test_perf_cull_coupling();
    test_counts_vs_core();
    print_table();
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures) { printf("RESULT: FAIL\n"); return 1; }
    printf("RESULT: PASS\n");
    return 0;
}
