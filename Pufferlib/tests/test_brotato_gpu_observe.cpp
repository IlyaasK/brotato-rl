/* tests/test_brotato_gpu_observe.cpp
 *
 * Byte-parity test for ocean/brotato/brotato_gpu_observe.cuh -- the lane-parallel
 * observation writer -- against the CPU reference bt_core_observe.
 *
 * Self-contained: includes the header under test and <stdio.h>. No Env, no
 * raylib, no engine, no CUDA toolkit (the host build of the header takes the
 * bp==1 group path, which is the serial reference order, so this test proves the
 * algorithm -- including the selection-sort swap tie-break -- against the core
 * on this host). The device-vs-host side (bp==32) is
 * tests/bench_brotato_gpu_observe.cu.
 *
 * Build and run:
 *     c++ -O2 -o build/test_brotato_gpu_observe tests/test_brotato_gpu_observe.cpp -lm
 *     ./build/test_brotato_gpu_observe
 *
 * Exit code 0 = all checks passed.
 *
 * Coverage:
 *   A. randomized synthetic states (bounded fields, no NaN) -- the whole
 *      observation vector, the 31-byte action mask and the whole BtState
 *      compared bit-for-bit.
 *   B. tie-heavy states: enemies / projectiles / consumables placed on small
 *      integer offsets so exact-distance ties are the common case, with
 *      >BT_SLOTS alive enemies to exercise the selection-sort swap order.
 *   C. live-stepped states (bt_core_init + random actions through combat, shop
 *      and level-up) so the weapon/shop/profile/shop-action blocks and the
 *      phase-dependent fields are compared on real data.
 */

#include "../ocean/brotato/brotato_gpu_observe.cuh"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int g_checks = 0;
static int g_failures = 0;

static void check(int cond, const char* msg) {
    g_checks++;
    if (!cond) { g_failures++; printf("  FAIL: %s\n", msg); }
}

/* deterministic xorshift64 */
static uint64_t g_rng = 0x243F6A8885A308D3ull;
static uint32_t rnd(void) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 32);
}
static int rndi(int n) { return n <= 0 ? 0 : (int)(rnd() % (uint32_t)n); }
static float rndf(float lo, float hi) {
    return lo + (hi - lo) * ((float)(rnd() & 0xFFFFFFu) / 16777216.0f);
}

/* BtState is ~1 MiB (it embeds the persistent contact world): keep it static. */
static BtState g_ref, g_gpu;
static float g_obs_ref[BT_OBS_SIZE], g_obs_gpu[BT_OBS_SIZE];
static unsigned char g_mask_ref[BT_ACTIONS], g_mask_gpu[BT_ACTIONS];

/* Compare the observation writer against bt_core_observe on identical state.
 * Returns 1 on parity, 0 (with a diagnostic) otherwise. */
static int parity(const BtState* src, const char* tag, int iter) {
    g_ref = *src;
    g_gpu = *src;
    BtState state_before = *src;

    bt_core_observe(&g_ref, g_obs_ref);
    bt_action_mask(&g_ref, g_mask_ref);

    BtgoScratch scr;
    memset(&scr, 0, sizeof(scr));
    btgo_observe_warp(&g_gpu, g_obs_gpu, &scr, 0, 0, 1);
    btgo_action_mask_warp(&g_gpu, g_mask_gpu, 0, 1);

    int ok = 1;
    if (memcmp(g_obs_ref, g_obs_gpu, sizeof(g_obs_ref)) != 0) {
        ok = 0;
        for (int i = 0; i < BT_OBS_SIZE; i++) {
            if (memcmp(&g_obs_ref[i], &g_obs_gpu[i], sizeof(float)) != 0) {
                printf("  FAIL obs[%d] (%s iter %d): ref %.9g / gpu %.9g\n",
                       i, tag, iter, (double)g_obs_ref[i], (double)g_obs_gpu[i]);
                break;
            }
        }
    }
    if (memcmp(g_mask_ref, g_mask_gpu, sizeof(g_mask_ref)) != 0) {
        ok = 0;
        for (int i = 0; i < BT_ACTIONS; i++) {
            if (g_mask_ref[i] != g_mask_gpu[i]) {
                printf("  FAIL mask[%d] (%s iter %d): ref %d / gpu %d\n",
                       i, tag, iter, g_mask_ref[i], g_mask_gpu[i]);
                break;
            }
        }
    }
    /* bt_core_observe must leave the state byte-identical (pralive is restored),
     * and so must the parallel writer. */
    if (memcmp(&state_before, &g_ref, sizeof(BtState)) != 0) {
        ok = 0; printf("  FAIL cpu observe mutated state (%s iter %d)\n", tag, iter);
    }
    if (memcmp(&state_before, &g_gpu, sizeof(BtState)) != 0) {
        ok = 0; printf("  FAIL gpu observe mutated state (%s iter %d)\n", tag, iter);
    }
    return ok;
}

/* --- bounded random state (zeroed first: every field observe reads is set) --- */
static void randomize(BtState* s, int allow_ties) {
    memset(s, 0, sizeof(*s));
    s->e_used = BT_MAX; s->pr_used = BT_MAX_PROJ; s->con_used = BT_MAX_CONS;
    s->phase = rndi(3);
    s->hp = rndf(1.0f, 100.0f);
    s->max_hp = rndf(1.0f, 100.0f);
    s->regen = rndf(0.0f, 30.0f);
    s->iframe = (rnd() & 3u) == 0 ? 1.0f : 0.0f;
    s->materials = rndf(0.0f, 500.0f);
    s->level = rndi(41);
    s->xp = rndf(0.0f, 800.0f);
    s->xp_need = 1 + rndi(200);
    s->px = rndf(0.0f, BT_ARENA_W);
    s->py = rndf(0.0f, BT_ARENA_H);
    s->wave = 1 + rndi(40);
    s->wave_duration = 20.0f + rndf(0.0f, 60.0f);
    s->time_left = rndf(0.0f, s->wave_duration);
    s->harvest = rndf(0.0f, 300.0f);
    s->luck = rndf(0.0f, 300.0f);
    for (int i = 0; i < BT_NSTATS; i++) {
        s->stat_raw[i] = rndf(-20.0f, 60.0f);
        s->temp_stat[i] = rndf(-10.0f, 20.0f);
        s->link_stat[i] = rndf(-10.0f, 20.0f);
        s->stat_gain[i] = rndf(0.5f, 2.5f);
    }
    s->scalar_effect[BT_ISTR_ITEMS_PRICE] = rndf(-40.0f, 60.0f);
    s->scalar_effect[BT_ISTR_REROLL_PRICE] = rndf(-40.0f, 60.0f);
    s->character = (unsigned short)rndi(BT_NCHARACTERS);
    s->weapon_slot_limit = (unsigned char)(1 + rndi(6));
    s->wpn_count = (unsigned char)rndi(BT_SLOTS + 1);
    for (int i = 0; i < BT_SLOTS; i++) {
        s->wpn_id[i] = (rnd() & 7u) == 0 ? BT_WEAPON_NONE
                                         : (unsigned short)rndi(BT_NWEAPONS);
        s->wpn_tier[i] = (unsigned char)rndi(6);
        s->wpn_cursed[i] = (unsigned char)rndi(2);
        s->wpn_cd[i] = rndf(0.0f, 400.0f);
        s->wpn_wmod[i] = rndf(0.0f, 1.0f);
    }
    for (int i = 0; i < 4; i++) {
        unsigned char kind = (unsigned char)rndi(3);
        s->shop_kind[i] = kind;
        s->shop_id[i] = kind == BT_SHOP_ITEM  ? (unsigned short)rndi(BT_NITEMS)
                      : kind == BT_SHOP_WEAPON ? (unsigned short)rndi(BT_NWEAPONS)
                      : 0;
        s->shop_cursed[i] = (unsigned char)rndi(2);
        /* row+1 encoding: 0 = absent, 1..BT_NUPGRADES = a row, plus the
         * weapon-slot sentinel occasionally. */
        s->lvl_offers[i] = (rnd() & 15u) == 0
            ? (unsigned short)BT_UPGRADE_SLOT_OFFER
            : (unsigned short)rndi(BT_NUPGRADES + 1);
        s->shop_locked[i] = (unsigned char)rndi(3);
    }
    s->ban_mode = (unsigned char)rndi(2);
    s->ban_tokens = (unsigned char)rndi(BT_BAN_MAX_TOKEN + 1);
    s->crate_kind = (unsigned char)rndi(3);
    s->crate_id = s->crate_kind == BT_SHOP_ITEM ? (unsigned short)rndi(BT_NITEMS) : 0;
    s->crate_cursed = (unsigned char)rndi(2);
    s->lvl_reroll_price = rndi(200);
    s->shop_available = (unsigned char)(rnd() & 15u);
    s->shop_timer = (rnd() & 1u) ? rndi(120) : -rndi(5);
    s->paid_rerolls = rndi(5);
    s->free_rerolls_remaining = rndi(3);
    s->has_bonus_free_reroll = (unsigned char)rndi(2);
    for (int i = 0; i < BT_NITEMS; i++) s->item_owned[i] = (unsigned short)rndi(7);

    /* enemies */
    for (int i = 0; i < BT_MAX; i++) {
        bt_enemy_set_alive(s, i, (rnd() & 1u) ? 1 : 0);
        /* a dying slot (death animation still playing) is an observe candidate
         * even when ealive == 0 */
        bt_enemy_set_dying(s, i, (rnd() & 1u)
            ? (unsigned char)rndi(BT_POOL_DEATH_FRAMES + 1) : 0);
        s->eelite[i] = (unsigned char)rndi(2);
        s->ehp[i] = s->edying[i] != 0 ? 0.0f : rndf(0.0f, 500.0f);
        s->emax[i] = rndf(50.0f, 500.0f);       /* > 0: keep ehp/emax finite */
        s->edmg[i] = rndf(0.0f, 50.0f);
        if (allow_ties) {
            static const float off[7] = { -3.0f, -2.0f, -1.0f, 0.0f, 1.0f, 2.0f, 3.0f };
            s->ex[i] = s->px + off[rndi(7)];
            s->ey[i] = s->py + off[rndi(7)];
        } else {
            s->ex[i] = rndf(0.0f, BT_ARENA_W);
            s->ey[i] = rndf(0.0f, BT_ARENA_H);
        }
    }
    /* enemy projectiles */
    for (int i = 0; i < BT_MAX_PROJ; i++) {
        s->pralive[i] = (unsigned char)((rnd() & 3u) ? 1 : 0);
        s->prfriendly[i] = (unsigned char)rndi(2);
        s->prsrc[i] = (rnd() & 3u) == 0 ? 0xFFFFu : (unsigned short)rndi(BT_MAX);
        s->prdmg[i] = rndf(0.0f, 40.0f);
        if (allow_ties) {
            static const float off[7] = { -3.0f, -2.0f, -1.0f, 0.0f, 1.0f, 2.0f, 3.0f };
            s->prx[i] = s->px + off[rndi(7)];
            s->pry[i] = s->py + off[rndi(7)];
        } else {
            s->prx[i] = rndf(0.0f, BT_ARENA_W);
            s->pry[i] = rndf(0.0f, BT_ARENA_H);
        }
    }
    /* consumables */
    for (int i = 0; i < BT_MAX_CONS; i++) {
        s->conalive[i] = (unsigned char)rndi(2);
        if (allow_ties) {
            static const float off[7] = { -3.0f, -2.0f, -1.0f, 0.0f, 1.0f, 2.0f, 3.0f };
            s->conx[i] = s->px + off[rndi(7)];
            s->cony[i] = s->py + off[rndi(7)];
        } else {
            s->conx[i] = rndf(0.0f, BT_ARENA_W);
            s->cony[i] = rndf(0.0f, BT_ARENA_H);
        }
    }
}

/* Force the tie-heavy selection geometry: >BT_SLOTS alive enemies on a small
 * integer grid (exact-distance ties dominate), all projectiles equidistant
 * except a few, several consumables equidistant. */
static void force_ties(BtState* s) {
    for (int i = 0; i < BT_MAX; i++) {
        bt_enemy_set_alive(s, i, (unsigned char)(i < 40 ? 1 : 0));
        /* slots 40..49 are dying-but-not-alive: still candidates */
        bt_enemy_set_dying(s, i, (unsigned char)((i >= 40 && i < 50) ? 5 : (i % 7 == 3 ? 3 : 0)));
        s->ehp[i] = s->ealive[i] ? 50.0f : 0.0f;
        s->ex[i] = s->px + (float)((i % 5) - 2);
        s->ey[i] = s->py + (float)((i / 5) % 5 - 2);
    }
    for (int i = 0; i < BT_MAX_PROJ; i++) {
        s->pralive[i] = (unsigned char)(i < 12 ? 1 : 0);
        s->prfriendly[i] = 0;
        s->prsrc[i] = (unsigned short)i;
        s->prx[i] = s->px + (float)((i % 3) - 1);
        s->pry[i] = s->py + (float)((i % 3) - 1);
    }
    for (int i = 0; i < BT_MAX_CONS; i++) {
        s->conalive[i] = (unsigned char)(i < 8 ? 1 : 0);
        s->conx[i] = s->px + (float)((i % 4) - 1);
        s->cony[i] = s->py + (float)((i % 4) - 1);
    }
}

static BtState g_live;
static float g_live_obs[BT_OBS_SIZE];

int main(void) {
    check(BT_OBS_SIZE == 203, "BT_OBS_SIZE is 203");

    /* A. randomized synthetic states */
    int okA = 1;
    for (int it = 0; it < 4000; it++) {
        randomize(&g_live, it & 1);
        okA &= parity(&g_live, "random", it);
        if (!okA) break;
    }
    check(okA, "4000 randomized synthetic states bit-exact");
    printf("A: 4000 randomized states\n");

    /* B. tie-heavy states */
    int okB = 1;
    for (int it = 0; it < 2000; it++) {
        randomize(&g_live, 1);
        if (it & 1) force_ties(&g_live);
        okB &= parity(&g_live, "ties", it);
        if (!okB) break;
    }
    check(okB, "2000 tie-heavy states bit-exact");
    printf("B: 2000 tie-heavy states\n");

    /* B2. Deterministic selection-order case: 4 alive enemies with squared
     * distances 9,9,4,1 at slots 0..3. The serial selection-with-swaps reports
     * slots 3,2,1,0; a stable (dist,index) sort would report 3,2,0,1. This pins
     * the exact swap semantics the GPU argmin reproduces. */
    {
        BtState s;
        memset(&s, 0, sizeof(s));
        s.e_used = 4;
        s.px = 1024.0f; s.py = 768.0f;
        s.hp = 10.0f; s.max_hp = 10.0f; s.xp_need = 1; s.wave = 1;
        s.wave_duration = 90.0f; s.time_left = 90.0f;
        for (int i = 0; i < 4; i++) {
            bt_enemy_set_alive(&s, i, 1); s.eelite[i] = 0;
            s.ehp[i] = 50.0f; s.emax[i] = 100.0f; s.edmg[i] = 10.0f;
        }
        s.ex[0] = s.px + 3.0f; s.ey[0] = s.py;   /* d2 = 9 */
        s.ex[1] = s.px;        s.ey[1] = s.py + 3.0f;  /* d2 = 9 */
        s.ex[2] = s.px + 2.0f; s.ey[2] = s.py;   /* d2 = 4 */
        s.ex[3] = s.px + 1.0f; s.ey[3] = s.py;   /* d2 = 1 */
        float ref[BT_OBS_SIZE];
        bt_core_observe(&s, ref);
        int order[4];
        for (int j = 0; j < 4; j++)
            order[j] = (int)(ref[BT_OBS_PLAYER + BT_OBS_ITEMS + BT_OBS_UPGRADES + 5 * j]
                             * BT_ARENA_W);   /* dx of the enemy in slot j */
        check(order[0] == 1 && order[1] == 2 && order[2] == 0 && order[3] == 3,
              "enemy selection-with-swaps order is 3,2,1,0 (not stable-by-index)");
        okB &= parity(&s, "selection-order", 0);
        check(okB, "selection-order state bit-exact");
        printf("B2: selection order {dx} = %d,%d,%d,%d (expect 1,2,0,3)\n",
               order[0], order[1], order[2], order[3]);

        /* the unreported enemy slots stay exactly 0 (core zero-fills the block) */
        int pad_zero = 1;
        for (int k = BT_OBS_PLAYER + BT_OBS_ITEMS + BT_OBS_UPGRADES + 5 * 4;
             k < BT_OBS_PLAYER + BT_OBS_ITEMS + BT_OBS_UPGRADES + BT_OBS_ENEMIES; k++)
            if (ref[k] != 0.0f) pad_zero = 0;
        check(pad_zero, "enemy pad slots (slots >= alive count) are 0.0");
    }

    /* B2b. A dying unit (ealive == 0, edying > 0) is a nearest-enemy candidate
     * AND is counted by the enemy-count feature: one alive + three dying must
     * report n = 4 with the dying slots' 0.0 hp ratio. */
    {
        BtState s;
        memset(&s, 0, sizeof(s));
        s.e_used = 4;
        s.px = 1024.0f; s.py = 768.0f;
        s.hp = 10.0f; s.max_hp = 10.0f; s.xp_need = 1; s.wave = 1;
        s.wave_duration = 90.0f; s.time_left = 90.0f;
        for (int i = 0; i < 3; i++) {                 /* dying, nearest */
            bt_enemy_set_alive(&s, i, 0); bt_enemy_set_dying(&s, i, 10); s.ehp[i] = 0.0f; s.emax[i] = 100.0f;
            s.ex[i] = s.px + (float)(i + 1); s.ey[i] = s.py;
        }
        bt_enemy_set_alive(&s, 3, 1); bt_enemy_set_dying(&s, 3, 0);             /* alive, farthest */
        s.ehp[3] = 50.0f; s.emax[3] = 100.0f;
        s.ex[3] = s.px + 8.0f; s.ey[3] = s.py;
        float ref2[BT_OBS_SIZE];
        bt_core_observe(&s, ref2);
        int nfeat = (int)(ref2[BT_OBS_PLAYER + BT_OBS_ITEMS + BT_OBS_UPGRADES +
                              BT_OBS_ENEMIES + 2] / BT_ENEMY_COUNT_SCALE + 0.5f);
        int okd = (nfeat == 4);
        for (int j = 0; j < 3; j++)
            if (!(ref2[BT_OBS_PLAYER + BT_OBS_ITEMS + BT_OBS_UPGRADES + 5 * j + 2] == 0.0f))
                okd = 0;
        check(okd, "dying units are observe candidates (n = 4, hp ratio 0.0)");
        okB &= parity(&s, "dying-candidates", 0);
        check(okB, "dying-candidate state bit-exact");
        printf("B2b: dying candidates counted n=%d (expect 4)\n", nfeat);
    }

    /* B3. Action-mask contract, asserted against bt_action_mask on crafted
     * phase/crate combinations (the GPU mask is byte-compared in every parity
     * call above; this pins the intended semantics). */
    {
        BtState s;
        memset(&s, 0, sizeof(s));
        int ok = 1;
        unsigned char ref[BT_ACTIONS];
        /* phase 0 combat: ids 0..8 only */
        s.phase = 0; s.crate_kind = BT_SHOP_NONE;
        parity(&s, "mask-phase0", 0);
        bt_action_mask(&s, ref);
        for (int i = 0; i < BT_ACTIONS; i++) {
            if (!!g_mask_ref[i] != (i < BT_ACT_MOVE_COUNT ? 1 : 0)) ok = 0;
            if (g_mask_ref[i] != ref[i]) ok = 0;
        }
        /* phase 1 shop, no offers: nothing but skip (reroll needs gold/free) */
        s.phase = 1; s.materials = 0.0f; s.shop_timer = 0;
        s.free_rerolls_remaining = 0; s.has_bonus_free_reroll = 0;
        s.ban_mode = 1; s.ban_tokens = 8;
        parity(&s, "mask-phase1-empty", 0);
        bt_action_mask(&s, ref);
        for (int i = 0; i < BT_ACTIONS; i++) if (g_mask_ref[i] != ref[i]) ok = 0;
        if (g_mask_ref[BT_ACT_SKIP] != 1 || g_mask_ref[BT_ACT_LOCK0] != 0 ||
            g_mask_ref[BT_ACT_BAN0] != 0 || g_mask_ref[0] != 0) ok = 0;
        /* phase 1 shop with an ITEM and a WEAPON offer: lock both, ban only the item */
        s.materials = 1000.0f;
        s.shop_kind[0] = BT_SHOP_ITEM; s.shop_id[0] = 5;
        s.shop_kind[1] = BT_SHOP_WEAPON; s.shop_id[1] = 118;
        parity(&s, "mask-phase1-offers", 0);
        bt_action_mask(&s, ref);
        for (int i = 0; i < BT_ACTIONS; i++) if (g_mask_ref[i] != ref[i]) ok = 0;
        if (g_mask_ref[BT_ACT_LOCK0] != 1 || g_mask_ref[BT_ACT_LOCK0 + 1] != 1 ||
            g_mask_ref[BT_ACT_BAN0] != 1 || g_mask_ref[BT_ACT_BAN0 + 1] != 0) ok = 0;
        s.shop_kind[0] = BT_SHOP_NONE; s.shop_id[0] = 0;
        s.shop_kind[1] = BT_SHOP_NONE; s.shop_id[1] = 0;
        /* phase 2 without a crate: no offers -> nothing legal */
        s.phase = 2; s.crate_kind = BT_SHOP_NONE;
        for (int i = 0; i < 4; i++) s.lvl_offers[i] = 0;
        parity(&s, "mask-phase2", 0);
        bt_action_mask(&s, ref);
        for (int i = 0; i < BT_ACTIONS; i++) {
            if (!!g_mask_ref[i] != 0) ok = 0;
            if (g_mask_ref[i] != ref[i]) ok = 0;
        }
        /* phase 2 with an open crate: only take / recycle */
        s.crate_kind = BT_SHOP_ITEM;
        parity(&s, "mask-crate", 0);
        bt_action_mask(&s, ref);
        for (int i = 0; i < BT_ACTIONS; i++) {
            int want = (i == BT_ACT_CRATE_TAKE || i == BT_ACT_CRATE_RECYCLE) ? 1 : 0;
            if (!!g_mask_ref[i] != want) ok = 0;
            if (g_mask_ref[i] != ref[i]) ok = 0;
        }
        check(ok, "action mask follows the phase/crate contract");
        printf("B3: action mask contract checked (4 phase/crate layouts)\n");
    }

    /* C. live-stepped states: Demon + ghost scepter, random actions, long enough
     * to cross the wave boundary into the shop / level-up phases. */
    unsigned short demon = (unsigned short)bt_character_id_from_name("Demon");
    unsigned short scepter = bt_weapon_id_from_name("weapon_ghost_scepter_1");
    if (demon == 0xFFFFu) demon = BT_CHARACTER_DEMON;
    int okC = 1, tested = 0, transitions = 0;
    int phases_seen[4] = {0, 0, 0, 0};
    for (int env = 0; env < 12 && okC; env++) {
        bt_core_init(&g_live, 0xB0A70u + (bt_u64)env * 7919u, 0, 1, demon, scepter);
        int prev_phase = g_live.phase;
        for (int step = 0; step < 7000; step++) {
            int action = rndi(BT_ACTIONS);
            float rew = 0.0f, term = 0.0f;
            bt_core_step(&g_live, action, g_live_obs, &rew, &term);
            int ph = g_live.phase;
            if (ph >= 0 && ph < 4) phases_seen[ph] = 1;
            if ((step & 15) == 0 || ph != prev_phase) {
                okC &= parity(&g_live, "live", env * 100000 + step);
                tested++;
                if (ph != prev_phase) transitions++;
                if (!okC) break;
            }
            prev_phase = ph;
        }
    }
    check(okC, "live-stepped states bit-exact");
    check(tested > 500, "live states exercised");
    check(phases_seen[0] && phases_seen[1], "live states reached combat + shop");
    printf("C: %d live states, %d phase transitions, phases seen: "
           "combat=%d shop=%d levelup=%d\n",
           tested, transitions, phases_seen[0], phases_seen[1], phases_seen[2]);

    if (g_failures) {
        printf("%d checks, %d failures\n", g_checks, g_failures);
        return 1;
    }
    printf("all %d checks passed\n", g_checks);
    return 0;
}
