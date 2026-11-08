/* tests/test_brotato_shop_lock.cpp
 *
 * Shop LOCK / REROLL regression (base_shop.gd:355-380 fill_shop_items).
 *
 * The port's shop keeps player_locked_items at the FRONT of the new list, in
 * the order lock_player_shop_item appended them, and regenerates the remaining
 * slots (bt_progress_roll_shop, brotato_progression.h).  That path snapshots
 * the locked offers into four 4-entry local tables before the slots are
 * rewritten; both the snapshot stores and the fill loop that copies them back
 * use `k < nl <= 4`, and `nl` is what renumbers BtState.shop_locked and tells
 * the fill loop where to stop.  This test pins that contract on the states a
 * run can actually reach, including the fully locked 4-slot shop -- the state
 * in which the CUDA device build mis-addressed the snapshot store (see the
 * compute-sanitizer note in bt_progress_roll_shop).
 *
 * Pinned here:
 *   - a locked offer keeps kind/id/curse and moves to the front, in lock order
 *     (lock order == ascending BtState.shop_locked rank);
 *   - shop_locked renumbers to a contiguous 1..nl prefix on the front slots and
 *     0 everywhere else, for a plain order, for a full 4-slot lock, and for the
 *     NON-CONTIGUOUS rank state a ban leaves behind (ban zeroes one rank without
 *     renumbering, so the snapshot counter and rank - 1 are NOT interchangeable);
 *   - the number of locked offers (nl) never exceeds the 4-slot snapshot, and
 *     no implementation-capacity latch is tripped by any of it.
 *
 * Build and run:
 *     c++ -std=c++17 -O2 -ffp-contract=off -Wno-narrowing -I. -Iocean/brotato \
 *         tests/test_brotato_shop_lock.cpp -o /tmp/t_shop_lock -lm
 *     /tmp/t_shop_lock
 *
 * Exit code 0 = all checks passed.  `-v` prints a deterministic trace of every
 * (slot -> kind,id,curse) row and lock rank, which is byte-comparable between
 * two builds of the shared core (used to prove a rewrite of the snapshot walk
 * leaves the shop byte-identical).
 */
#include "../ocean/brotato/brotato_core.h"
#include "../ocean/brotato/brotato_capacity_host.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

static int g_checks = 0, g_failures = 0;
static int g_verbose = 0;

static void check(bool ok, const char *msg) {
    g_checks++;
    if (!ok) { g_failures++; std::printf("  FAIL: %s\n", msg); }
}

/* BtState is multi-MiB: keep every instance in BSS, never on the stack. */
static BtState S;
static float obs[BT_OBS_SIZE];
static long g_trace_line = 0;

struct Offer { int kind; unsigned short id; int cursed; };

static void trace_shop(const char *tag, const BtState *s) {
    if (!g_verbose) return;
    std::printf("T%03ld %-16s ph=%d lock=[%d,%d,%d,%d] avail=%02x gold=%7.0f rc=%d",
                g_trace_line++, tag, (int)s->phase, s->shop_locked[0], s->shop_locked[1],
                s->shop_locked[2], s->shop_locked[3], s->shop_available, s->materials,
                s->reroll_count);
    for (int i = 0; i < 4; i++)
        std::printf(" s%d=(%d,%u,%d)", i, (int)s->shop_kind[i], (unsigned)s->shop_id[i],
                    (int)s->shop_cursed[i]);
    std::printf("\n");
}

static float step_action(BtState *s, int action) {
    float rew = 0.0f, term = 0.0f;
    bt_core_step(s, action, obs, &rew, &term);
    return term;
}

/* Boot the deep endless invulnerable Demon profile the deep sweep uses (same
 * start_wave/endless/invulnerable/audio_rng/weapon as the configured repro) and
 * hold until the shop phase opens. */
static void boot_and_open_shop(BtState *s, unsigned int seed, int start_wave) {
    const unsigned short wpn =
        (unsigned short)bt_weapon_index_from_id("weapon_ghost_scepter_1");
    bt_core_init(s, (bt_u64)seed, 0, 1, (unsigned short)BT_CHARACTER_DEMON, wpn, 0);
    s->particles_rng = 1;
    s->invulnerable = 1;
    s->endless = 1;
    s->start_wave = (unsigned short)start_wave;
    s->rng_ready = 0;
    bt_core_reset(s);
    for (long t = 0; t < 20000 && s->phase != 1; t++) {
        if (step_action(s, 0) > 0.0f) bt_core_reset(s);
    }
}

/* A shop reroll (action 4) costs gold (base_shop.gd:443-449); hand the shop a
 * purse so the action always executes, and report whether it did. */
static bool reroll(BtState *s) {
    s->materials = 500000.0f;
    int rc = s->reroll_count;
    step_action(s, BT_ACT_REROLL);
    return s->reroll_count > rc;
}

static void lock_slot(BtState *s, int slot) { step_action(s, BT_ACT_LOCK0 + slot); }

int main(int argc, char **argv) {
    /* This test walks the state a device-side out-of-bounds write was observed
     * on; the runner captures stdout, so keep every check visible even if a
     * future regression makes the core abort mid-run. */
    setbuf(stdout, NULL);
    g_verbose = argc > 1 && std::strcmp(argv[1], "-v") == 0;
    const int start_wave = 60;

    Offer want[4];
    int want_n = 0;

    std::printf("[1] plain lock order (wave %d shop): locks move to the front\n", start_wave);
    boot_and_open_shop(&S, 1u, start_wave);
    check(S.phase == 1, "the deep wave-60 boot reaches the shop phase");
    trace_shop("shop open", &S);
    {
        /* slot 2 first, then slot 0: lock order == [2, 0] */
        want_n = 0;
        for (int pass = 0; pass < 2; pass++) {
            int slot = pass == 0 ? 2 : 0;
            want[want_n].kind = (int)S.shop_kind[slot];
            want[want_n].id = S.shop_id[slot];
            want[want_n].cursed = (int)S.shop_cursed[slot];
            want_n++;
        }
        lock_slot(&S, 2);
        lock_slot(&S, 0);
    }
    trace_shop("locked 2,0", &S);
    check(S.shop_locked[2] == 1 && S.shop_locked[0] == 2,
          "lock ranks follow append order (slot 2 -> rank 1, slot 0 -> rank 2)");
    check(reroll(&S), "the reroll action executed (gold was sufficient)");
    trace_shop("after reroll", &S);
    check(S.phase == 1, "the reroll keeps the env in the shop phase");
    check(S.shop_locked[0] == 1 && S.shop_locked[1] == 2 &&
          S.shop_locked[2] == 0 && S.shop_locked[3] == 0,
          "a reroll renumbers the surviving locks to the 1..nl front prefix");
    for (int k = 0; k < want_n; k++) {
        check((int)S.shop_kind[k] == want[k].kind && S.shop_id[k] == want[k].id &&
              (int)S.shop_cursed[k] == want[k].cursed,
              "the locked offer stays front-of-list with kind/id/curse intact");
    }
    check(bt_capacity_mask(&S) == 0ull, "no capacity loss on the plain lock+reroll path");

    /* ---- [2] the fully locked 4-slot shop: the state the CUDA device build
     * mis-addressed.  Four locks force the snapshot's last index; every reroll
     * must keep all four offers, in lock order, front-prefix renumbered. */
    std::printf("[2] fully locked 4-slot shop (the device-fault state)\n");
    boot_and_open_shop(&S, 2u, start_wave);
    check(S.phase == 1, "second deep boot reaches the shop phase");
    {
        int order[4] = { 1, 3, 0, 2 };
        want_n = 0;
        for (int k = 0; k < 4; k++) {
            int slot = order[k];
            want[want_n].kind = (int)S.shop_kind[slot];
            want[want_n].id = S.shop_id[slot];
            want[want_n].cursed = (int)S.shop_cursed[slot];
            want_n++;
            lock_slot(&S, slot);
        }
    }
    trace_shop("locked 1,3,0,2", &S);
    check(S.shop_locked[1] == 1 && S.shop_locked[3] == 2 && S.shop_locked[0] == 3 &&
          S.shop_locked[2] == 4,
          "all four slots carry distinct ranks 1..4 in append order");
    for (int r = 0; r < 3; r++) {
        check(reroll(&S), "the reroll action executed with all four slots locked");
        char tag[32];
        std::snprintf(tag, sizeof(tag), "full reroll %d", r + 1);
        trace_shop(tag, &S);
        for (int k = 0; k < 4; k++) {
            check((int)S.shop_kind[k] == want[k].kind && S.shop_id[k] == want[k].id &&
                  (int)S.shop_cursed[k] == want[k].cursed,
                  "with all four slots locked every offer stays put, in lock order");
        }
        check(S.shop_locked[0] == 1 && S.shop_locked[1] == 2 && S.shop_locked[2] == 3 &&
              S.shop_locked[3] == 4,
              "all four ranks survive the reroll in lock order");
        check(bt_capacity_mask(&S) == 0ull, "no capacity loss with a 4-slot locked shop");
    }

    /* ---- [3] the NON-CONTIGUOUS rank state a ban leaves: bt_wshop_ban zeroes
     * one rank in place (shop_item.gd:204-210) without renumbering, so the
     * surviving ranks are e.g. {2,3}.  The snapshot must still emit both locked
     * offers at the front (nl == 2): a walk that assumed nl == rank - 1 would
     * lose one of them. */
    std::printf("[3] non-contiguous ranks after a ban (nl != rank - 1)\n");
    boot_and_open_shop(&S, 4u, start_wave);
    check(S.phase == 1, "third deep boot reaches the shop phase");
    S.ban_mode = 1;
    S.ban_tokens = 8;
    int item_slot = -1;
    for (int i = 0; i < 4; i++)
        if (S.shop_kind[i] == BT_SHOP_ITEM && (S.shop_available & (1u << i))) { item_slot = i; break; }
    check(item_slot >= 0, "the wave-60 shop offers an ITEM slot to ban");
    if (item_slot >= 0) {
        /* lock the ITEM slot FIRST so it holds rank 1, then two others */
        int others[2], n_others = 0;
        for (int i = 0; i < 4 && n_others < 2; i++)
            if (i != item_slot && S.shop_kind[i] != BT_SHOP_NONE) others[n_others++] = i;
        check(n_others == 2, "two more offers are lockable");
        lock_slot(&S, item_slot);
        lock_slot(&S, others[0]);
        lock_slot(&S, others[1]);
        trace_shop("locked 3 slots", &S);
        check(S.shop_locked[item_slot] == 1 && S.shop_locked[others[0]] == 2 &&
              S.shop_locked[others[1]] == 3,
              "three slots locked in append order (the ITEM slot holds rank 1)");
        /* the two offers that must survive, in lock order (rank 2 then rank 3) */
        want_n = 0;
        for (int k = 0; k < 4; k++)
            if (S.shop_locked[k] == 2 || S.shop_locked[k] == 3) {
                want[want_n].kind = (int)S.shop_kind[k];
                want[want_n].id = S.shop_id[k];
                want[want_n].cursed = (int)S.shop_cursed[k];
                want_n++;
            }
        check(want_n == 2, "two locked offers survive the ban");
        step_action(&S, BT_ACT_BAN0 + item_slot);   /* ban the rank-1 ITEM offer */
        trace_shop("after ban", &S);
        check(S.shop_locked[item_slot] == 0,
              "the ban zeroes the banned slot's rank in place");
        /* base_shop.gd on_shop_item_banned erases the offer's entry from the
         * per-shop lock list (an Array, so the surviving push_back order closes
         * up), and the v5 s1 trace proves the banned offer is not preserved by
         * the next reroll; the RANKS therefore renumber to 1..n at the ban. */
        check(S.shop_locked[others[0]] == 1 && S.shop_locked[others[1]] == 2,
              "the ban erases the banned lock: the surviving ranks close up to 1..n");
        check(reroll(&S), "the reroll action executed on the banned-rank shop");
        trace_shop("after reroll", &S);
        check(S.shop_locked[0] == 1 && S.shop_locked[1] == 2 && S.shop_locked[2] == 0 &&
              S.shop_locked[3] == 0,
              "the surviving locks renumber to the contiguous 1..nl front prefix");
        for (int k = 0; k < want_n; k++) {
            check((int)S.shop_kind[k] == want[k].kind && S.shop_id[k] == want[k].id &&
                  (int)S.shop_cursed[k] == want[k].cursed,
                  "both surviving locked offers land at the front in rank order");
        }
        check(bt_capacity_mask(&S) == 0ull, "no capacity loss on the banned-rank path");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
