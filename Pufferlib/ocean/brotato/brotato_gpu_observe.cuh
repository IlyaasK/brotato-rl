// brotato_gpu_observe.cuh -- lane-parallel (warp-per-env) writer of the
// BT_OBS_SIZE-float Brotato observation, bit-exact against the CPU core's
// bt_core_observe (brotato_core.h). OWNER: GpuObserve.
//
// ---------------------------------------------------------------------------
// WHAT THIS IS
// ---------------------------------------------------------------------------
// bt_core_observe is the definition. This header re-SCHEDULES it across the 32
// lanes of one warp per env; it never changes a single FP operation, comparison
// order, tie-break or output index. Output bytes are therefore identical to the
// CPU core by construction, and tests/test_brotato_gpu_observe.cpp /
// tests/bench_brotato_gpu_observe.cu prove it on many random states (including
// exact-distance ties) by memcmp against bt_core_observe.
//
// The observe vector has a FIXED block layout (BT_OBS_SIZE, no padding), so
// every block's base offset is a compile-time constant:
//
//   0..10    player (BT_OBS_PLAYER)
//   11..26   item offers, 4 x 4           (BT_OBS_ITEMS)
//   27..34   level-up offers, 4 x 2       (BT_OBS_UPGRADES)
//   35..64   6 nearest enemies x 5        (BT_OBS_ENEMIES)
//   65..67   wave / time / enemy count    (BT_OBS_WAVE)
//   68..76   3 nearest enemy projectiles x 3 (BT_OBS_PROJ)
//   77..78   nearest consumable x 2       (BT_OBS_CONS)
//   79..160  weapon/shop/profile/shop-action (BT_OBS_WEAPON_SIZE = 82)
//   161..202 Demon/endless target block   (BT_OBS_TARGET = 42)
//
// Lane mapping: a "group" of `bp` lanes (bp == 32 device, bp == 1 host) owns one
// env; `mem` is the group member id. Every block is a strided loop over items
// (BTGO_FOR), so the very same function is the CPU reference at bp == 1.
// The three nearest-k blocks (enemies, enemy projectiles, consumables) run as
// warp reductions with the deterministic (distance, position) key that
// reproduces the serial selection exactly.
//
// ---------------------------------------------------------------------------
// THE SELECTION TIE-BREAK (why (dist, index) is not enough by itself)
// ---------------------------------------------------------------------------
// bt_core_observe's enemy block is a SELECTION SORT with swaps:
//
//     for sl in 0..out-1: best = sl
//         for j in sl+1..n-1: if cd[j] < cd[best] best = j   // strict '<'
//         swap cd[sl],cd[best]; swap ci[sl],ci[best]
//
// Selection-with-swaps is NOT stable for equal keys, so the reported ORDER of
// equal-distance enemies is not "ascending original slot". Reproducing it needs
// the swap permutation, which is exactly what BtgoScratch carries: the alive
// enemy slots compacted in ascending slot order into ci[] with their squared
// distances in cd[] (the identical arrays the serial code builds), then the same
// swap-based selection run step by step with a warp argmin over positions
// [sl..n). Each step's argmin is the first position attaining the minimum, i.e.
// the lexicographic minimum of the (distance, position) key -- so `cd[j] <
// cd[best]` on a left-to-right scan equals `key_less(dist_j, j, dist_best,
// best)`. The keys are (distance, position); the reduce is deterministic.
//
// The projectile and consumable blocks are a plain nearest scan (strict '<',
// first occurrence wins), so they are a single (distance, index) warp argmin per
// slot; the projectile loop carries the serial code's pralive==2 consumption
// side effect (restored to 1 after the loop), so slots 0,1,2 are sequential.
//
// ---------------------------------------------------------------------------
// WEAPON / SHOP / STAT BLOCKS
// ---------------------------------------------------------------------------
// Those blocks are per-record independent (weapon slot i, shop offer i, ...),
// so they are transcribed field-for-field from bt_weapon_observe_append
// (brotato_weapon_observe.h) and driven through the SAME owner helpers
// (bt_weapon_stats, bt_shop_price, bt_wshop_can_buy, bt_wshop_currency,
// bt_wshop_price_units, bt_reroll_cost, bt_character_hp_shop), one record per
// lane. The expressions are copied verbatim, so no helper ever re-derives a
// float. CONTRACT: when bt_core_observe / bt_weapon_observe_append change,
// this file must change in lockstep; the parity test enforces it.
//
// The action mask (bt_action_mask) is emitted by btgo_action_mask_warp with the
// identical per-id predicate.
#ifndef PUFFER_BROTATO_GPU_OBSERVE_CUH
#define PUFFER_BROTATO_GPU_OBSERVE_CUH

#include <cstdint>
#include <cstring>
#include <math.h>

#if defined(__CUDACC__) || defined(__CUDA_ARCH__)
#include <cuda_runtime.h>
#endif

// brotato_core.h pulls the whole core (state, tables, bt_core_observe,
// bt_action_mask, bt_shop_price, bt_core_stat_read) and itself includes
// brotato_weapon_observe.h, so every helper used below is already declared.
#include "brotato_core.h"

#define BTGO_OBS_SIZE BT_OBS_SIZE
#define BTGO_ACTIONS  BT_ACTIONS

#if defined(__CUDA_ARCH__)
#define BTGO_LANE32 ((int)(threadIdx.x & 31u))
#define BTGO_WARPNO ((int)(threadIdx.x >> 5))
#endif

#if defined(__CUDA_ARCH__)
#define BTGO_FULL_MASK 0xffffffffu
#define btgo_sync(bp) do { if ((bp) > 1) __syncwarp(); } while (0)
#else
#define btgo_sync(bp) do { } while (0)
#endif

// Strided parallel loop header over `n` items on group member `mem` of `bp`.
#define BTGO_FOR(i, n, mem, bp) for (int i = (mem); (i) < (n); (i) += (bp))

// ---------------------------------------------------------------------------
// (distance, position) key comparison.
//
// Reproduces the serial `cd[j] < cd[best]` scan over ascending positions,
// including the NaN behaviour (< and == both false, so a NaN never displaces an
// earlier candidate and the first element is always taken as the seed):
//   key_less(d1,p1, d2,p2) == 1  <=>  p1 should win over p2.
// Ordered pairs compare by distance then position; an unordered pair falls back
// to the smaller position, which is what a left-to-right strict-< scan yields.
// ---------------------------------------------------------------------------
BT_HD static inline int btgo_key_less(float d1, int p1, float d2, int p2) {
    if (d1 < d2) return 1;
    if (d1 > d2) return 0;
    return p1 < p2;   /* d1 == d2, or unordered (NaN) */
}

// Warp-wide (bp==32) reduction of the (distance, position) key; leaves the
// group minimum in *d/*p on every lane. No-op at bp==1 (the group is one lane).
// bp must be 1 or 32: this is a single-warp reduction by construction.
BT_HD static inline void btgo_warp_argmin(float* d, int* p, int lane, int bp) {
#if defined(__CUDA_ARCH__)
    (void)lane;
    if (bp > 1) {
#pragma unroll
        for (int off = 16; off > 0; off >>= 1) {
            float od = __shfl_down_sync(BTGO_FULL_MASK, *d, off);
            int   op = __shfl_down_sync(BTGO_FULL_MASK, *p, off);
            if (btgo_key_less(od, op, *d, *p)) { *d = od; *p = op; }
        }
    }
#else
    (void)d; (void)p; (void)lane; (void)bp;
#endif
}

// Exclusive prefix of `v` over the group's members, in member order; leaves the
// group total in *total (written once, by member 0; the caller syncs before
// reading it). bp==32 uses a warp scan, bp==1 is trivial (the serial path: one
// member owns every item).
BT_HD static inline int btgo_excl_scan(int v, int mem, int bp, int* total) {
#if defined(__CUDA_ARCH__)
    if (bp > 1) {
        int incl = v;
#pragma unroll
        for (int off = 1; off < 32; off <<= 1) {
            int t = __shfl_up_sync(BTGO_FULL_MASK, incl, off);
            if (mem >= off) incl += t;
        }
        int tot = __shfl_sync(BTGO_FULL_MASK, incl, 31);
        if (mem == 0) *total = tot;
        return incl - v;
    }
#else
    (void)mem; (void)bp;
#endif
    *total = v;
    return 0;
}

// Per-warp scratch for the nearest-enemy selection: the serial code's own ci[]
// (slot indices) and cd[] (squared distances), plus the candidate count n
// (alive OR dying). Held in shared memory on device, on the caller's stack on
// host. Sized by BT_MAX, so it tracks the pool bound automatically.
typedef struct BtgoScratch {
    float cd[BT_MAX];
    int   ci[BT_MAX];
    int   n;
    int   pad;
} BtgoScratch;

// ---------------------------------------------------------------------------
// Nearest-k enemy block (35..64), including the serial selection-sort swaps.
// ---------------------------------------------------------------------------
BT_HD static void btgo_observe_enemies(BT_DEVICE BtState* s, BT_DEVICE float* o,
        BtgoScratch* scr, int mem, int lane, int bp) {
    // Compact nearest-enemy candidates into ci[] (ascending) with cd[] (same
    // order). A slot is a candidate while it is alive OR its death animation is
    // still playing (edying > 0): rl_obs.gd:417-425 `_collect` walks the SCENE
    // TREE, so a still-parented dying node is reported exactly like a live one
    // (ehp/emax reads 0, its stats are unchanged, and the count feature below
    // counts it). Contiguous chunks per member in member order -> ascending
    // output.
    int chunk = (BT_MAX + bp - 1) / bp;
    int lo = mem * chunk, hi = lo + chunk;
    if (hi > BT_MAX) hi = BT_MAX;
    int mine = 0;
    for (int i = lo; i < hi; i++) mine += (s->ealive[i] || s->edying[i] != 0) ? 1 : 0;
    int base = btgo_excl_scan(mine, mem, bp, &scr->n);
    int pos = base;
    for (int i = lo; i < hi; i++) {
        if (!s->ealive[i] && s->edying[i] == 0) continue;
        float dx = s->ex[i] - s->px, dy = s->ey[i] - s->py;
        scr->ci[pos] = i;
        scr->cd[pos] = dx * dx + dy * dy;
        pos++;
    }
    btgo_sync(bp);

    int n = scr->n;
    int out = n < BT_SLOTS ? n : BT_SLOTS;
    for (int sl = 0; sl < out; sl++) {
        // argmin over positions [sl, n) by (distance, position): the serial
        // inner loop's first-occurrence-of-the-minimum, in one warp reduce.
        int have = 0;
        float bd = 0.0f;
        int bpos = 0;
        for (int j = sl + mem; j < n; j += bp) {
            float d = scr->cd[j];
            if (!have) { bd = d; bpos = j; have = 1; }
            else if (btgo_key_less(d, j, bd, bpos)) { bd = d; bpos = j; }
        }
        if (!have) { bd = (float)INFINITY; bpos = 0x7fffffff; }
        btgo_warp_argmin(&bd, &bpos, lane, bp);
        if (mem == 0 && bpos != sl) {
            float td = scr->cd[sl]; scr->cd[sl] = scr->cd[bpos]; scr->cd[bpos] = td;
            int   ti = scr->ci[sl]; scr->ci[sl] = scr->ci[bpos]; scr->ci[bpos] = ti;
        }
        btgo_sync(bp);
    }
    // Emit the 5 fields of every reported slot (unreported slots stay 0 from
    // the caller's zero-fill).
    for (int k = mem; k < out * 5; k += bp) {
        int sl = k / 5, f = k % 5;
        int e = scr->ci[sl];
        float v;
        if (f == 0)      v = (s->ex[e] - s->px) / BT_ARENA_W;
        else if (f == 1) v = (s->ey[e] - s->py) / BT_ARENA_H;
        else if (f == 2) v = s->ehp[e] / s->emax[e];
        else if (f == 3) v = s->edmg[e] * 0.1f;
        else             v = (float)(s->eelite[e] == 1 ? 1 : 0);
        o[BT_OBS_PLAYER + BT_OBS_ITEMS + BT_OBS_UPGRADES + 5 * sl + f] = v;
    }
}

// ---------------------------------------------------------------------------
// 3 nearest enemy projectiles (68..76) + nearest consumable (77..78).
// ---------------------------------------------------------------------------
BT_HD static void btgo_observe_projectiles(BT_DEVICE BtState* s, BT_DEVICE float* o,
        int mem, int lane, int bp) {
    for (int slot = 0; slot < 3; slot++) {
        float bd = 1e18f;
        int bpos = 0x7fffffff;
        for (int i = mem; i < BT_MAX_PROJ; i += bp) {
            if (s->pralive[i] != 1) continue;
            if (s->prfriendly[i] && s->prsrc[i] == 0xFFFFu) continue;
            float dx = s->prx[i] - s->px, dy = s->pry[i] - s->py;
            float d = dx * dx + dy * dy;
            if (btgo_key_less(d, i, bd, bpos)) { bd = d; bpos = i; }
        }
        btgo_warp_argmin(&bd, &bpos, lane, bp);
        if (mem == 0) {
            int base = BT_OBS_PLAYER + BT_OBS_ITEMS + BT_OBS_UPGRADES +
                       BT_OBS_ENEMIES + BT_OBS_WAVE + 3 * slot;
            if (bpos != 0x7fffffff && bd < BT_OBS_PROJ_MAX_D2) {
                o[base + 0] = (s->prx[bpos] - s->px) / BT_ARENA_W;
                o[base + 1] = (s->pry[bpos] - s->py) / BT_ARENA_H;
                o[base + 2] = s->prdmg[bpos] * 0.1f;
                s->pralive[bpos] = 2;
            } else {
                o[base + 0] = 0.0f; o[base + 1] = 0.0f; o[base + 2] = 0.0f;
            }
        }
        btgo_sync(bp);
    }
    for (int i = mem; i < BT_MAX_PROJ; i += bp)
        if (s->pralive[i] == 2) s->pralive[i] = 1;
    btgo_sync(bp);

    // nearest consumable
    float bd = 1e18f;
    int bpos = 0x7fffffff;
    for (int i = mem; i < BT_MAX_CONS; i += bp) {
        if (!s->conalive[i]) continue;
        float dx = s->conx[i] - s->px, dy = s->cony[i] - s->py;
        float d = dx * dx + dy * dy;
        if (btgo_key_less(d, i, bd, bpos)) { bd = d; bpos = i; }
    }
    btgo_warp_argmin(&bd, &bpos, lane, bp);
    if (mem == 0) {
        int base = BT_OBS_PLAYER + BT_OBS_ITEMS + BT_OBS_UPGRADES +
                   BT_OBS_ENEMIES + BT_OBS_WAVE + BT_OBS_PROJ;
        if (bpos != 0x7fffffff) {
            o[base + 0] = (s->conx[bpos] - s->px) / BT_ARENA_W;
            o[base + 1] = (s->cony[bpos] - s->py) / BT_ARENA_H;
        } else {
            o[base + 0] = 0.0f; o[base + 1] = 0.0f;
        }
    }
    btgo_sync(bp);
}

// ---------------------------------------------------------------------------
// Weapon / shop / profile / shop-action block (79..160), one record per lane;
// transcribed field-for-field from bt_weapon_observe_append.
// ---------------------------------------------------------------------------
#define BTGO_WEAPON_BASE (BT_OBS_PLAYER + BT_OBS_ITEMS + BT_OBS_UPGRADES + \
        BT_OBS_ENEMIES + BT_OBS_WAVE + BT_OBS_PROJ + BT_OBS_CONS)   /* = 79 */

BT_HD static void btgo_observe_weapons(BT_DEVICE BtState* s, BT_DEVICE float* o,
        int mem, int bp) {
    // 17 independent records: 6 weapon slots, 4 typed shop offers, profile,
    // currency flag, 4 offer economies, reroll.
    for (int rec = mem; rec < 17; rec += bp) {
        if (rec < 6) {                                   /* weapon slot */
            int slot = rec;
            int base = BTGO_WEAPON_BASE + 8 * slot;
            unsigned short id = s->wpn_id[slot];
            if (id == BT_WEAPON_NONE) continue;          /* zeros (zero-fill) */
            BT_CONST_PTR(BtWeaponDef) def = &BT_WEAPONS[id];
            BtPlayerWeaponStats st;
            (void)bt_weapon_stats(s, slot, &st);

            float cd = st.cooldown > 0 ? st.cooldown_left / (float)st.cooldown : 0.0f;
            if (cd < 0.0f) cd = 0.0f;
            else if (cd > 1.0f) cd = 1.0f;
            if (s->phase == 1) cd = 0.0f;

            o[base + 0] = (float)(id + 1) / (float)(BT_NWEAPONS + 1);
            o[base + 1] = (float)s->wpn_tier[slot] * 0.25f;
            o[base + 2] = (float)def->kind;
            o[base + 3] = cd;
            o[base + 4] = (float)st.damage * 0.1f;
            o[base + 5] = (float)st.max_range / BT_ARENA_W;
            o[base + 6] = st.crit_chance;
            o[base + 7] = s->wpn_cursed[slot] ? 1.0f : 0.0f;
        } else if (rec < 10) {                           /* typed shop offer */
            int i = rec - 6;
            int base = BTGO_WEAPON_BASE + BT_OBS_WEAPONS + 3 * i;
            unsigned char kind = s->shop_kind[i];
            o[base + 0] = (float)kind;
            if (kind == BT_SHOP_ITEM) {
                o[base + 1] = (float)(s->shop_id[i] + 1) / (float)(BT_NITEMS + 1);
            } else if (kind == BT_SHOP_WEAPON) {
                o[base + 1] = (float)(s->shop_id[i] + 1) / (float)(BT_NWEAPONS + 1);
            } else {
                o[base + 1] = 0.0f;
            }
            o[base + 2] = s->shop_cursed[i] ? 1.0f : 0.0f;
        } else if (rec == 10) {                          /* profile */
            int base = BTGO_WEAPON_BASE + BT_OBS_WEAPONS + BT_OBS_SHOP_EXT;
            o[base + 0] = (float)(s->character + 1) / (float)(BT_NCHARACTERS + 1);
            o[base + 1] = (float)s->weapon_slot_limit / (float)BT_CHARACTER_SLOT_DEFAULT;
        } else if (rec == 11) {                          /* currency flag */
            int base = BTGO_WEAPON_BASE + BT_OBS_WEAPONS + BT_OBS_SHOP_EXT + BT_OBS_PROFILE;
            o[base] = s->phase == 1 && bt_character_hp_shop(s->character) ? 1.0f : 0.0f;
        } else if (rec < 16) {                           /* offer economy */
            int i = rec - 12;
            int base = BTGO_WEAPON_BASE + BT_OBS_WEAPONS + BT_OBS_SHOP_EXT +
                       BT_OBS_PROFILE + 1 + 4 * i;
            float currency = s->phase == 1 ? bt_wshop_currency(s) : 0.0f;
            unsigned char kind = s->phase == 1 ? s->shop_kind[i] : BT_SHOP_NONE;
            unsigned short id = s->shop_id[i];
            int present = 0, eligible = 0;
            float price = 0.0f;
            if (kind == BT_SHOP_ITEM && (int)id < BT_NITEMS) {
                present = 1; eligible = 1;
                price = bt_shop_slot_price(s, i);
            } else if (kind == BT_SHOP_WEAPON && bt_weapon_valid(id)) {
                present = 1;
                eligible = bt_wshop_can_buy(s, id);
                price = bt_shop_slot_price(s, i);
            }
            int affordable = present && currency >= price;
            int buyable = present && eligible && affordable && s->shop_timer <= 0;
            o[base + 0] = present ? price : 0.0f;
            o[base + 1] = eligible ? 1.0f : 0.0f;
            o[base + 2] = affordable ? 1.0f : 0.0f;
            o[base + 3] = buyable ? 1.0f : 0.0f;
        } else {                                         /* reroll */
            int base = BTGO_WEAPON_BASE + BT_OBS_WEAPONS + BT_OBS_SHOP_EXT +
                       BT_OBS_PROFILE + BT_OBS_SHOP_ACTION - 3;
            float rr_price = 0.0f;
            int rr_free = 0, rr_avail = 0;
            if (s->phase == 1) {
                rr_free = (s->free_rerolls_remaining > 0 || s->has_bonus_free_reroll) ? 1 : 0;
                /* the button's STORED price (base_shop.gd:479-481/132) and no
                 * buy-delay term: both mirror the CPU writer exactly. */
                rr_price = rr_free ? 0.0f : (float)s->shop_reroll_price;
                rr_avail = (rr_free || s->materials >= rr_price) ? 1 : 0;
            }
            o[base + 0] = rr_price;
            o[base + 1] = rr_free ? 1.0f : 0.0f;
            o[base + 2] = rr_avail ? 1.0f : 0.0f;
        }
    }
}

// ---------------------------------------------------------------------------
// Demon/endless target block (161..BT_OBS_SIZE-1).
// ---------------------------------------------------------------------------
#define BTGO_TARGET_BASE (BT_OBS_SIZE - BT_OBS_TARGET)   /* = 161 */

BT_HD static void btgo_observe_target(BT_DEVICE BtState* s, BT_DEVICE float* o,
        int mem, int bp) {
    // stats: harvesting/100, luck/100, squashed endless factor, wave/50
    for (int k = mem; k < BT_OBS_T_STATS; k += bp) {
        float v;
        if (k == 0)      v = s->harvest * 0.01f;
        else if (k == 1) v = s->luck * 0.01f;
        else if (k == 2) {
            float ef = bt_loot_endless_factor(s->wave);
            if (ef < 0.0f) ef = 0.0f;
            v = ef / (1.0f + ef);
        } else           v = (float)s->wave * 0.02f;
        o[BTGO_TARGET_BASE + k] = v;
    }
    // shop-owned state (AuditShop): offer lock flags, the held crate offer and
    // the level-up reroll price -- one item per lane, per the owner's encoding.
    for (int sc = mem; sc < BT_OBS_T_LOCKS + BT_OBS_T_CRATE + BT_OBS_T_LVLRR; sc += bp) {
        float v;
        if (sc < BT_OBS_T_LOCKS) {
            v = s->shop_locked[sc] ? 1.0f : 0.0f;
        } else if (sc < BT_OBS_T_LOCKS + BT_OBS_T_CRATE) {
            int c = sc - BT_OBS_T_LOCKS;
            if (c == 0)      v = (float)s->crate_kind;
            else if (c == 1) v = s->crate_kind == BT_SHOP_ITEM
                                 ? (float)(s->crate_id + 1) / (float)(BT_NITEMS + 1) : 0.0f;
            else if (c == 2) v = s->crate_kind != BT_SHOP_NONE ? (float)s->crate_cursed : 0.0f;
            else             v = s->crate_kind != BT_SHOP_NONE ? 1.0f : 0.0f;
        } else {
            v = (float)s->lvl_reroll_price;
        }
        o[BTGO_TARGET_BASE + BT_OBS_T_STATS + sc] = v;
    }
    // curated guide-item owned counts, min(owned,4)/4
    for (int gi = mem; gi < BT_OBS_T_ITEMS; gi += bp) {
        unsigned short owned = s->item_owned[BT_OBS_GUIDE_ITEMS[gi]];
        if (owned > 4) owned = 4;
        o[BTGO_TARGET_BASE + BT_OBS_T_STATS + BT_OBS_T_LOCKS + BT_OBS_T_CRATE +
          BT_OBS_T_LVLRR + gi] = (float)owned * 0.25f;
    }
    // v5 tail: the ban token budget (0 when ban_mode is off) and the four
    // level-up offer presence flags -- identical to bt_core_observe, so the two
    // writers stay bit-identical.
    for (int kb = mem; kb < BT_OBS_T_BAN; kb += bp) {
        o[BTGO_TARGET_BASE + BT_OBS_T_STATS + BT_OBS_T_LOCKS + BT_OBS_T_CRATE +
          BT_OBS_T_LVLRR + BT_OBS_T_ITEMS + kb] =
            s->ban_mode ? (float)s->ban_tokens / (float)BT_BAN_MAX_TOKEN : 0.0f;
    }
    for (int kp = mem; kp < BT_OBS_T_LVLOFF; kp += bp) {
        o[BTGO_TARGET_BASE + BT_OBS_T_STATS + BT_OBS_T_LOCKS + BT_OBS_T_CRATE +
          BT_OBS_T_LVLRR + BT_OBS_T_ITEMS + BT_OBS_T_BAN + kp] =
            bt_lvl_offer_present(s, kp) ? 1.0f : 0.0f;
    }
}

// ---------------------------------------------------------------------------
// Per-phase action mask (bt_action_mask), per-id predicate, strided per lane.
// ---------------------------------------------------------------------------
BT_HD void btgo_action_mask_warp(BT_DEVICE BtState* s, BT_DEVICE unsigned char* mask,
        int mem, int bp) {
    if (mask == 0) return;
    /* Delegate to the core's per-id predicate: bt_action_legal is the single
     * source of truth the CPU writer also uses, so this lane-parallel writer
     * cannot drift from it (it previously duplicated the phase rules). */
    for (int i = mem; i < BT_ACTIONS; i += bp) {
        mask[i] = (unsigned char)(bt_action_legal(s, i) ? 1 : 0);
    }
}

// ---------------------------------------------------------------------------
// Full observation: exactly BT_OBS_SIZE floats into `o`, bit-identical to
// bt_core_observe. `mem` is the group member id [0,bp), `lane` its lane inside
// a warp (== mem for bp<=32), `bp` the group size (1 or 32). `scr` is per-group
// scratch (shared on device, caller stack on host).
// ---------------------------------------------------------------------------
BT_HD void btgo_observe_warp(BT_DEVICE BtState* s, BT_DEVICE float* o,
        BtgoScratch* scr, int mem, int lane, int bp) {
    if (o == 0) return;
    /* Single-warp groups only: the reductions are __shfl-scoped, so bp must be
     * 32 (a warp) or 1 (the host serial reference). Anything else would silently
     * combine lanes from a neighbouring group, so refuse it. */
    if (bp != 1 && bp != 32) return;

    for (int i = mem; i < BT_OBS_SIZE; i += bp) o[i] = 0.0f;
    btgo_sync(bp);

    // --- player block (0..10) -------------------------------------------------
    for (int k = mem; k < BT_OBS_PLAYER; k += bp) {
        float v;
        switch (k) {
        /* obs[0] = RunData.get_player_current_health / get_player_max_health:
         * the LiveStats current health while a wave runs, the stat-layer max in
         * the shop (run_data.gd:295-298), exactly like bt_core_observe. */
        case 0:  v = (s->phase == 1 ? s->max_hp : s->hp) / s->max_hp; break;
        case 1:  v = s->max_hp * 0.015625f; break;
        case 2:  v = s->regen * 0.0333333f; break;
        case 3:  v = bt_core_stat_read(s, 2) * 0.05f; break;
        case 4:  v = s->px / BT_ARENA_W; break;
        case 5:  v = s->py / BT_ARENA_H; break;
        case 6:  v = s->iframe > 0.0f ? 1.0f : 0.0f; break;
        case 7:  v = (float)s->phase; break;
        case 8:  v = s->materials * 0.01f; break;
        case 9:  v = (float)s->level * 0.05f; break;
        default: v = s->xp / (float)(s->xp_need > 0 ? s->xp_need : 1); break;
        }
        o[k] = v;
    }

    // --- item offers (11..26), 4 x 4 -----------------------------------------
    for (int i = mem; i < 4; i += bp) {
        unsigned short index = s->shop_kind[i] == BT_SHOP_ITEM &&
            !(s->phase == 1 && !(s->shop_available & (1u << i))) ? s->shop_id[i] : 0;
        BT_CONST_PTR(BtItem) it = &BT_ITEMS[index];
        float price = bt_observe_shop_price(s, i);
        int b = BT_OBS_PLAYER + 4 * i;
        o[b + 0] = (float)it->tier / 4.0f;
        o[b + 1] = s->materials >= price ? 1.0f : 0.0f;
        int dom = 0; float mag = 0.0f;
        for (int e = 0; e < it->count; e++) {
            BT_CONST_PTR(BtItemEffect) ef = &BT_ITEM_EFFECTS[it->first + e];
            if (bt_fabsf(ef->value) > bt_fabsf(mag)) { mag = ef->value; dom = ef->stat; }
        }
        o[b + 2] = (float)dom / 10.0f;
        o[b + 3] = bt_clampf(mag * 0.1f, -1.0f, 1.0f);
    }

    // --- level-up offers (27..34), 4 x 2 -------------------------------------
    for (int i = mem; i < 4; i += bp) {
        /* Same decode as bt_core_observe: lvl_offers[i] is (BT_UPGRADES row + 1),
         * 0 = absent (empty-slot encoding = BT_UPGRADES[0]), and the weapon-slot
         * sentinel carries BT_UPGRADE_SLOT_*.  A real offer shown in phase 2
         * carries the level_upgrades_modifications-scaled value. */
        unsigned short off = s->lvl_offers[i];
        int row = bt_lvl_offer_row(off);
        unsigned char st; float raw; int real_offer;
        if (off == (unsigned short)BT_UPGRADE_SLOT_OFFER) {
            st = (unsigned char)BT_UPGRADE_SLOT_STAT; raw = BT_UPGRADE_SLOT_VALUE; real_offer = 1;
        } else if (row < 0) {
            st = BT_UPGRADES[0].stat; raw = BT_UPGRADES[0].value; real_offer = 0;
        } else {
            st = BT_UPGRADES[row].stat; raw = BT_UPGRADES[row].value; real_offer = 1;
        }
        float v = (s->phase == 2 && real_offer) ? bt_upgrade_offer_value(s, raw) : raw;
        int b = BT_OBS_PLAYER + BT_OBS_ITEMS + 2 * i;
        o[b + 0] = (float)st / 12.0f;
        o[b + 1] = bt_clampf(v * 0.1f, -1.0f, 1.0f);
    }

    // --- enemies (35..64) ----------------------------------------------------
    btgo_observe_enemies(s, o, scr, mem, lane, bp);
    btgo_sync(bp);

    // --- wave (65..67) -------------------------------------------------------
    for (int k = mem; k < BT_OBS_WAVE; k += bp) {
        int b = BT_OBS_PLAYER + BT_OBS_ITEMS + BT_OBS_UPGRADES + BT_OBS_ENEMIES + k;
        if (k == 0)      o[b] = (float)s->wave / (float)BT_WAVES;
        else if (k == 1) {
            float observed_time = s->time_left;   /* mirrors the CPU writer */
            if (observed_time < 0.0f) observed_time = 0.0f;
            o[b] = s->wave_duration > 0.0f ? observed_time / s->wave_duration : 0.0f;
        } else           o[b] = (float)scr->n * BT_ENEMY_COUNT_SCALE;
    }

    // --- projectiles (68..76) + consumable (77..78) -------------------------
    btgo_observe_projectiles(s, o, mem, lane, bp);

    // --- weapons / shop / profile / shop-action (79..160) -------------------
    btgo_observe_weapons(s, o, mem, bp);

    // --- Demon/endless target block (161..197) ------------------------------
    btgo_observe_target(s, o, mem, bp);
}

// ---------------------------------------------------------------------------
// Kernels. One warp per env: blockDim.x must be a multiple of 32; warp w of
// block b owns env (b*(blockDim.x/32) + w).
// ---------------------------------------------------------------------------
#ifndef BTGO_WARPS_PER_BLOCK
#define BTGO_WARPS_PER_BLOCK 4
#endif

#if defined(__CUDACC__)
#define BTGO_AT(base, stride, i) \
    ((BtState*)((char*)(base) + (size_t)(i) * (size_t)(stride)))

// Warp-per-env observe: each warp writes one env's BT_OBS_SIZE floats (and, if
// `masks` is non-null, that env's action mask).
__global__ void btgo_observe_warp_kernel(BtState* __restrict__ base, size_t stride,
        float* __restrict__ obs, unsigned char* __restrict__ masks, int n) {
    int warps = blockDim.x >> 5;
    int w = (int)(threadIdx.x >> 5);
    int lane = (int)(threadIdx.x & 31u);
    int env = blockIdx.x * warps + w;
    if (env >= n) return;
    __shared__ BtgoScratch scr[BTGO_WARPS_PER_BLOCK];
    BtState* s = BTGO_AT(base, stride, env);
    btgo_observe_warp(s, obs + (size_t)env * BT_OBS_SIZE, &scr[w], lane, lane, 32);
    if (masks) btgo_action_mask_warp(s, masks + (size_t)env * BT_ACTIONS, lane, 32);
}

// One-thread-per-env baseline (identical output, for measurement/comparison).
__global__ void btgo_observe_thread_kernel(BtState* __restrict__ base, size_t stride,
        float* __restrict__ obs, unsigned char* __restrict__ masks, int n) {
    int env = blockIdx.x * blockDim.x + threadIdx.x;
    if (env >= n) return;
    BtState* s = BTGO_AT(base, stride, env);
    bt_core_observe(s, obs + (size_t)env * BT_OBS_SIZE);
    if (masks) bt_action_mask(s, masks + (size_t)env * BT_ACTIONS);
}
#endif

#endif // PUFFER_BROTATO_GPU_OBSERVE_CUH
