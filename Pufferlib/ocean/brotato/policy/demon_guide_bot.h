/* demon_guide_bot.h - scripted heuristic Demon policy (Danger 5, High Endless).
 *
 * OWNERSHIP: DemonBaseline.  Acts ONLY through the env action contract
 * (brotato_state.h BT_ACT_*): combat ids 0..8, shop ids as published by
 * bt_action_mask, and level-up ids 0..4.  The policy never mutates the sim
 * store, so a CPU run and a CUDA run driven by it stay byte-identical.
 *
 * Source of the plan: ArosRising, "Brotato Guide to Endless Mode" (Demon /
 * High Endless).  The stages encoded here:
 *
 *   1. Ghost Scepter start: buy + combine ghost scepters (family tier 1..4),
 *      keep the loadout ranged, kite the swarm.
 *   2. Economy engine: Bag, Hunting Trophy, Coupon, Dangerous Bunny, Recycling
 *      Machine, Lure (and Crown while still inside the 20-wave campaign - it is
 *      banned from the endless pool).  Harvesting and Luck stats early.
 *   3. Pivot (~wave 25-40): hand the scepter plan over to Pruner / Potato
 *      Thrower / Excalibur plus Anvil and Alien Eyes.
 *   4. Spicy Sauce build: Garden, Improved Tools, Explosive Shells, Plastic
 *      Explosive, Alien Tongue, Sad Tomato, Torture, Padding, Tardigrade,
 *      Silver Bullet, Grind's Magical Leaf, Vigilante Ring, Spicy Sauce.
 *   5. Lock/save: past `stay_wave` stop paying for rerolls (free rerolls and
 *      hoarded gold only) because gold is Demon's future Max HP.
 *
 * Demon specifics the policy respects: the shop currency is Max HP
 * (character hp_shop), gold converts to Max HP at each wave end (50%, chunks of
 * 13), and recycling/rerolls always cost gold, so buys reserve a survivable
 * Max-HP floor and paid rerolls are rationed by both a per-shop cap and a gold
 * floor.
 *
 * This is a baseline/stress bot, not a trained policy: its job is to reach a
 * wave number cheaply, drive endless spawn pressure, and expose capacity
 * overflow.  It is fully deterministic so two backends running the same profile
 * produce byte-identical trajectories.
 *
 * MEASURED REACH (Ghost Scepter start, Danger 5, tracked with the sim revision
 * at landing): stock Demon, 20 seeds -> mean 3.8 waves, min 3, max 4; every run
 * ends in wave 3-4.  The binding constraint is the Demon's Max HP, not the
 * controller: the SAME policy with extra starting Max HP
 * (tests/demon_guide_bot_run.cpp's `bonus_hp` + `endless` stress arguments)
 * reaches waves 50-61 and clears the campaign, which is how this bot is used to
 * drive the deep endless schedule and the pool-capacity checks.
 *
 * SCOPE OF EVIDENCE - do not over-read the numbers above:
 *   - They are a POLICY BASELINE: they say how far this scripted bot gets, not
 *     whether the sim matches the game.  A low count is not evidence of a sim
 *     defect, and a high one is not evidence of parity.
 *   - Separately checked and MATCHED against the source and the real Demon
 *     trace: starting Max HP 10, Ghost Scepter t1 damage 10 / cooldown 47
 *     ticks, per-kind enemy hp/damage/speed/attack_cd, round((base +
 *     grow*(wave-1)) * 1.4) for waves 1-4, and the wave-1 first-enemy
 *     activation tick (122 sim vs 123 game).  That is a stat-formula and
 *     activation check only; it does NOT establish whole-sim parity.
 *   - Still UNRESOLVED and not verified by this bot: spawn timing beyond the
 *     first activation, and the ongoing Demon trajectory (action-level
 *     sim-vs-game replay).  The latter is owed, and needs the bridge's
 *     per-frame action-file source; no parity conclusion should be drawn from
 *     this bot before that comparison is run.
 */
#pragma once

#include <math.h>

#include "../brotato_core.h"

/* --- persistent, per-episode policy state ------------------------------- */
typedef struct {
    int pivot_wave;     /* scepter -> Pruner/Potato Thrower/Excalibur handover */
    int stay_wave;      /* lock/save: stop paying for rerolls past this wave   */
    int last_dir;       /* previous combat movement id (anti-thrash)           */
    int last_shop_wave; /* detects a new shop so per-shop counters can reset    */
    int shop_paid;      /* paid rerolls already asked for in the open shop      */
    float hp_peak;      /* running peak Max HP (the reserve is a fraction of it) */
} DGuideBot;

static void dgb_init(DGuideBot* b, int pivot_wave, int stay_wave) {
    b->pivot_wave = pivot_wave > 0 ? pivot_wave : 30;
    b->stay_wave  = stay_wave  > 0 ? stay_wave  : 45;
    b->last_dir   = 0;
    b->last_shop_wave = -1;
    b->shop_paid  = 0;
    b->hp_peak    = 0.0f;
}

/* ==========================================================================
 * Build plan scoring.
 * ========================================================================== */

/* Guide item priority for the shop.  0 = never buy, higher = buy sooner.
 * `wave` gates the endless bans only (Crown is campaign-only). */
static int dgb_item_score(int wave, unsigned short idx) {
    if (idx >= BT_NITEMS) return 0;
    switch (idx) {
        /* --- economy engine (always first) --- */
        case BT_ITEM_IDX_BAG:               return 95;
        case BT_ITEM_IDX_HUNTING_TROPHY:    return 95;
        case BT_ITEM_IDX_COUPON:            return 92;
        case BT_ITEM_IDX_DANGEROUS_BUNNY:   return 90;
        case BT_ITEM_IDX_RECYCLING_MACHINE: return 88;
        case BT_ITEM_IDX_LURE:              return 86;
        /* item_crown is banned from the endless pool (run_data.gd:199-201). */
        case BT_ITEM_IDX_CROWN:             return wave <= BT_WAVES ? 90 : 0;
        /* --- scepter-era synergy --- */
        case BT_ITEM_IDX_ALIEN_EYES:        return 70;
        case BT_ITEM_IDX_ANVIL:             return 68;
        /* --- Spicy Sauce build --- */
        case BT_ITEM_IDX_SPICY_SAUCE:       return 78;
        case BT_ITEM_IDX_GARDEN:            return 66;
        case BT_ITEM_IDX_IMPROVED_TOOLS:    return 66;
        case BT_ITEM_IDX_EXPLOSIVE_SHELLS:  return 64;
        case BT_ITEM_IDX_PLASTIC_EXPLOSIVE: return 64;
        case BT_ITEM_IDX_ALIEN_TONGUE:      return 64;
        case BT_ITEM_IDX_PADDING:           return 62;
        case BT_ITEM_IDX_TARDIGRADE:        return 62;
        case BT_ITEM_IDX_SILVER_BULLET:     return 62;
        case BT_ITEM_IDX_SAD_TOMATO:        return 60;
        case BT_ITEM_IDX_GRINDS_MAGICAL_LEAF: return 60;
        case BT_ITEM_IDX_VIGILANTE_RING:    return 60;
        case BT_ITEM_IDX_TORTURE:           return 58;
        default: return 0;
    }
}

/* Weapon priority.  Ghost Scepter dominates until the pivot, then the pivot
 * families take over.  0 = not part of the plan. */
static int dgb_weapon_score(int wave, unsigned short id) {
    if (!bt_weapon_valid(id)) return 0;
    int fam = (int)bt_weapon_family(id);
    if (fam == BT_WEAPON_IDX_WEAPON_GHOST_SCEPTER_1)
        return wave < 25 ? 200 : 12;   /* the scepter plan, ahead of any item */
    if (fam == BT_WEAPON_IDX_WEAPON_PRUNER_1)
        return wave >= 25 ? 96 : 25;
    if (fam == BT_WEAPON_IDX_WEAPON_POTATO_THROWER_2)
        return wave >= 25 ? 94 : 25;
    if (id == BT_WEAPON_IDX_WEAPON_EXCALIBUR_4)
        return wave >= 30 ? 97 : 20;
    return 0;
}

/* Max-HP floor the shop must leave untouched (Demon pays in Max HP).  It is a
 * fraction of the RUN PEAK Max HP, not of the current value: pricing each buy
 * off the shrinking current Max HP would let a long shopping run drain the pool
 * geometrically (the reserve itself shrinks with every purchase). */
static float dgb_reserve(BtState* s, DGuideBot* b) {
    if (s->max_hp > b->hp_peak) b->hp_peak = s->max_hp;
    float r = 0.35f * b->hp_peak;
    if (r < 1.0f) r = 1.0f;
    return r;
}

/* Generic worth of one core stat point, from the item's legacy (stat, value)
 * rows.  Positive = the guide's priorities; curse is the only negative. */
static float dgb_stat_weight(int stat) {
    switch (stat) {
        case 0:  return 2.0f;   /* max_hp: survivability AND Demon currency   */
        case 5:  return 2.0f;   /* damage %                                   */
        case 15: return 1.8f;   /* attack speed                               */
        case 2:  return 1.4f;   /* armor                                      */
        case 6:  return 1.4f;   /* melee damage                               */
        case 7:  return 1.4f;   /* ranged damage                              */
        case 8:  return 1.2f;   /* elemental damage                           */
        case 4:  return 1.2f;   /* dodge                                      */
        case 10: return 2.5f;   /* luck                                       */
        case 12: return 3.0f;   /* harvesting: the Demon economy's engine     */
        case 17: return 1.0f;   /* lifesteal                                  */
        case 9:  return 1.0f;   /* crit chance                                */
        case 11: return 0.8f;   /* crit damage                                */
        case 3:  return 0.8f;   /* speed                                      */
        case 1:  return 0.6f;   /* hp regeneration                            */
        case 14: return 0.3f;   /* accuracy                                   */
        case 16: return 0.3f;   /* engineering                                */
        case 18: return 0.3f;   /* range                                      */
        case 13: return -1.0f;  /* curse: a cost, never a goal                */
        default: return 0.0f;
    }
}

/* Generic item value: the weighted sum of its stat rows, plus a small credit
 * for spawning pets/structures.  Used only as a fallback when the offer is not
 * one of the guide's named items, so a shop with nothing on-plan is not a
 * wasted wave. */
static float dgb_item_value(unsigned short idx) {
    if (idx >= BT_NITEMS) return 0.0f;
    const BtItem* it = &BT_ITEMS[idx];
    float sc = 0.0f;
    for (int e = 0; e < (int)it->count; e++) {
        const BtItemEffect* ef = &BT_ITEM_EFFECTS[it->first + e];
        float v = ef->value;
        if (v >  20.0f) v =  20.0f;
        if (v < -20.0f) v = -20.0f;
        sc += dgb_stat_weight((int)ef->stat) * v;
    }
    if (it->item_flags & BT_ITFLAG_PET)       sc += 8.0f;
    if (it->item_flags & BT_ITFLAG_STRUCTURE) sc += 6.0f;
    if (it->item_flags & BT_ITFLAG_TURRET)    sc += 4.0f;
    return sc;
}

/* ==========================================================================
 * Combat: kiting / approach controller.
 * ========================================================================== */
static int dgb_melee_bias(BtState* s) {
    int n = bt_weapon_count(s), melee = 0, ranged = 0;
    for (int i = 0; i < n; i++) {
        int k = bt_weapon_kind(bt_weapon_id(s, i));
        if (k == BT_WEAPON_KIND_MELEE) melee++;
        else if (k == BT_WEAPON_KIND_RANGED) ranged++;
    }
    return melee - ranged;
}

/* Combat movement: predictive clearance over the 8 movement ids.  For each
 * candidate direction the bot projects K frames of its own motion AND of every
 * enemy (each enemy walks toward the player's current position at its own
 * speed), and keeps the smallest distance to an enemy along that path (elites
 * count closer).  It picks the direction that MAXIMISES that clearance minus a
 * wall penalty band, so it walks into the emptiest future space instead of
 * reacting to where the swarm is one frame ahead.  A melee-heavy loadout at
 * healthy HP instead MINIMISES the nearest-enemy distance (closes in); gold is
 * chased only while the nearest enemy is far. */
static int dgb_combat_action(BtState* s, DGuideBot* b) {
    const float px = s->px, py = s->py;
    const int K = 8;                       /* projection frames */
    float spd = BT_PLAYER_SPEED * (1.0f + s->speed_pct * 0.01f);
    float hp_frac = s->max_hp > 0.0f ? s->hp / s->max_hp : 0.0f;
    int melee_heavy = dgb_melee_bias(s) > 0;
    int approach = (melee_heavy && hp_frac > 0.45f) ? 1 : 0;

    float near_d2 = 1e30f;
    int n_enemy = 0;
    for (int i = 0; i < BT_MAX; i++) {
        if (!s->ealive[i]) continue;
        float dx = s->ex[i] - px, dy = s->ey[i] - py;
        float d2 = dx * dx + dy * dy;
        if (d2 < near_d2) near_d2 = d2;
        n_enemy++;
    }
    float mat_dx = 0.0f, mat_dy = 0.0f, mat_d2 = 1e30f;
    for (int i = 0; i < BT_MAX_MAT; i++) {
        if (!s->matalive[i]) continue;
        float dx = s->matx[i] - px, dy = s->maty[i] - py;
        float d2 = dx * dx + dy * dy;
        if (d2 < mat_d2) { mat_d2 = d2; mat_dx = dx; mat_dy = dy; }
    }
    /* gold is picked up by walking over it, but only chase it while calm */
    int chase_gold = (n_enemy == 0 || near_d2 > 260.0f * 260.0f) && mat_d2 > 25.0f;
    float mgl = chase_gold ? sqrtf(mat_d2) : 1.0f;

    int best = 1;
    float best_score = -1e30f;
    for (int d = 1; d <= 8; d++) {
        float nx = px, ny = py, minclear = 1e30f;
        for (int k = 1; k <= K; k++) {
            nx += BT_MOVE_X[d] * spd * BT_DT;
            ny += BT_MOVE_Y[d] * spd * BT_DT;
            for (int i = 0; i < BT_MAX; i++) {
                if (!s->ealive[i]) continue;
                float ex = s->ex[i], ey = s->ey[i];
                float ux = px - ex, uy = py - ey;
                float ul = sqrtf(ux * ux + uy * uy) + 1e-3f;
                ex += ux / ul * s->espd[i] * BT_DT * (float)k;
                ey += uy / ul * s->espd[i] * BT_DT * (float)k;
                float dx = nx - ex, dy = ny - ey;
                float dd = sqrtf(dx * dx + dy * dy);
                if (s->eelite[i]) dd *= 0.6f;
                if (dd < minclear) minclear = dd;
            }
        }
        if (minclear > 1e29f) minclear = 1000.0f;
        float sc = approach ? -minclear : minclear;
        /* wall penalty band, px of penetration past 60 px from an edge */
        if (nx < 60.0f)              sc -= (60.0f - nx) * 3.0f;
        if (nx > BT_ARENA_W - 60.0f) sc -= (nx - (BT_ARENA_W - 60.0f)) * 3.0f;
        if (ny < 60.0f)              sc -= (60.0f - ny) * 3.0f;
        if (ny > BT_ARENA_H - 60.0f) sc -= (ny - (BT_ARENA_H - 60.0f)) * 3.0f;
        if (n_enemy == 0) {
            float cx = BT_ARENA_W * 0.5f - px, cy = BT_ARENA_H * 0.5f - py;
            float cl = sqrtf(cx * cx + cy * cy) + 1e-3f;
            sc += (BT_MOVE_X[d] * cx + BT_MOVE_Y[d] * cy) / cl * 5.0f;
        }
        if (chase_gold)
            sc += (BT_MOVE_X[d] * mat_dx + BT_MOVE_Y[d] * mat_dy) / mgl * 15.0f;
        if (d == b->last_dir) sc += 0.5f;   /* hysteresis: avoid thrash */
        if (sc > best_score) { best_score = sc; best = d; }
    }
    b->last_dir = best;
    return best;
}

/* ==========================================================================
 * Shop (phase 1).
 * ========================================================================== */
static int dgb_shop_action(BtState* s, DGuideBot* b, const unsigned char* mask) {
    int wave = s->wave;
    if (wave != b->last_shop_wave) { b->last_shop_wave = wave; b->shop_paid = 0; }

    float currency = bt_wshop_currency(s);
    float reserve = dgb_reserve(s, b);
    int have_free = (s->free_rerolls_remaining > 0 || s->has_bonus_free_reroll);

    /* 1) best affordable offer: a plan weapon, then a plan item, then any
     *    positive-value generic item; early on, any affordable ranged weapon
     *    (breadth beats tier while the Demon is squishy). */
    int best_act = -1;
    float best_score = 0.0f;
    for (int i = 0; i < 4; i++) {
        if (!mask[i] || !(s->shop_available & (1u << i))) continue;
        int kind = s->shop_kind[i];
        if (kind == BT_SHOP_NONE) continue;
        unsigned short offer = s->shop_id[i];
        float price = bt_wshop_price_units(s, kind, offer);
        if (currency - price < reserve) continue;
        float score = 0.0f;
        if (kind == BT_SHOP_WEAPON) {
            if (!bt_wshop_can_buy(s, offer)) continue;   /* the buy would be a no-op */
            int ws = dgb_weapon_score(wave, offer);
            if (ws > 0) score = 1000.0f + (float)ws;
            else if (bt_weapon_count(s) < 4 &&
                     bt_weapon_kind(offer) == BT_WEAPON_KIND_RANGED)
                score = 300.0f;
        } else {
            if (offer >= BT_NITEMS) continue;
            int maxn = (int)BT_ITEMS[offer].max_nb;
            if (maxn >= 0 && (int)s->item_owned[offer] >= maxn) continue;
            int named = dgb_item_score(wave, offer);
            if (named > 0) score = 1000.0f + (float)named;
            else {
                float g = dgb_item_value(offer);
                if (g >= 4.0f) score = g;   /* generic filler, below any named item */
            }
        }
        if (score > best_score) { best_score = score; best_act = i; }
    }
    if (best_act >= 0) return best_act;

    /* 2) combine once the rack is full: breadth first, and a full rack is the
     *    only moment combining buys a slot back. */
    if (bt_weapon_count(s) >= bt_weapon_slot_cap(s)) {
        int cand = -1, cand_rank = -1;
        int nc = bt_weapon_count(s);
        for (int i = 0; i < nc && i < BT_SLOTS; i++) {
            if (!bt_weapon_can_combine(s, i)) continue;
            int rank = dgb_weapon_score(wave, bt_weapon_id(s, i));
            if (rank > cand_rank) { cand_rank = rank; cand = i; }
        }
        if (cand >= 0 && mask[BT_ACT_COMBINE0 + cand]) return BT_ACT_COMBINE0 + cand;
    }

    /* 3) recycle an off-plan weapon when the rack is full */
    if (bt_weapon_count(s) >= bt_weapon_slot_cap(s)) {
        int nc = bt_weapon_count(s);
        for (int i = 0; i < nc && i < BT_SLOTS; i++) {
            if (dgb_weapon_score(wave, bt_weapon_id(s, i)) > 12) continue;
            if (mask[BT_ACT_RECYCLE0 + i]) return BT_ACT_RECYCLE0 + i;
            break;
        }
    }

    /* 4) reroll: free rerolls always, paid ones rationed per shop and by a
     *    gold floor (gold is the Demon's future Max HP). */
    if (have_free && mask[BT_ACT_REROLL]) return BT_ACT_REROLL;
    if (mask[BT_ACT_REROLL] && wave <= b->stay_wave &&
        b->shop_paid < 8 && s->paid_rerolls < 8) {
        int cost = bt_reroll_cost(wave, s->paid_rerolls);
        if (s->materials >= (float)(cost + 12)) { b->shop_paid++; return BT_ACT_REROLL; }
    }

    /* 5) nothing worth doing: advance to the next wave */
    if (mask[BT_ACT_SKIP]) return BT_ACT_SKIP;
    return BT_ACT_HOLD;
}

/* ==========================================================================
 * Level-up (phase 2): Harvesting/Luck early (the economy), then damage and
 * survivability.  Never reroll a level-up (it costs gold).
 * ========================================================================== */
static float dgb_upgrade_score(int wave, int stat) {
    switch (stat) {
        case 12: return wave <= BT_WAVES ? 6.0f : 1.5f;  /* harvesting */
        case 10: return wave <= BT_WAVES ? 4.5f : 1.5f;  /* luck */
        case 0:  return 2.0f;                            /* max hp (also currency) */
        case 5:  return 2.2f;                            /* damage % */
        case 6:  return 1.8f;                            /* melee damage */
        case 7:  return 1.8f;                            /* ranged damage */
        case 15: return 1.8f;                            /* attack speed */
        case 8:  return 1.6f;                            /* elemental damage */
        case 9:  return 1.2f;                            /* crit chance */
        case 11: return 1.0f;                            /* crit damage */
        case 2:  return wave > 15 ? 1.6f : 1.0f;         /* armor */
        case 4:  return wave > 15 ? 1.4f : 0.9f;         /* dodge */
        case 3:  return 1.0f;                            /* speed */
        case 17: return wave > 15 ? 1.2f : 0.8f;         /* lifesteal */
        case 1:  return 0.5f;                            /* regen (weak for Demon) */
        case 16: return 0.6f;                            /* engineering */
        case 14: return 0.6f;                            /* accuracy */
        case 18: return 0.5f;                            /* range */
        case 13: return 0.0f;                            /* curse: never */
        default: return 0.7f;
    }
}

static int dgb_level_action(BtState* s, DGuideBot* b, const unsigned char* mask) {
    (void)b;
    /* An open item box holds phase 2 until it is taken or recycled
     * (upgrades_ui.gd:88-115 shows ONE consumable at a time).  Take a free
     * item the plan values; recycle one that is net-negative (a -Max HP box is
     * a real cost for the Demon) into gold, which becomes Max HP at wave end. */
    if (s->crate_kind != BT_SHOP_NONE) {
        int take = 1;
        if (s->crate_kind == BT_SHOP_ITEM && s->crate_id < BT_NITEMS &&
            dgb_item_value(s->crate_id) < 0.0f) take = 0;
        if (take && mask[BT_ACT_CRATE_TAKE]) return BT_ACT_CRATE_TAKE;
        if (!take && mask[BT_ACT_CRATE_RECYCLE]) return BT_ACT_CRATE_RECYCLE;
        if (mask[BT_ACT_CRATE_TAKE]) return BT_ACT_CRATE_TAKE;
        if (mask[BT_ACT_CRATE_RECYCLE]) return BT_ACT_CRATE_RECYCLE;
        return BT_ACT_HOLD;
    }
    int best = -1;
    float best_score = -1e30f;
    for (int i = 0; i < 4; i++) {
        if (!mask[i]) continue;
        const BtUpgrade* up = &BT_UPGRADES[s->lvl_offers[i]];
        float sc = dgb_upgrade_score(s->wave, (int)up->stat) * up->value;
        if (sc > best_score) { best_score = sc; best = i; }
    }
    return best >= 0 ? best : BT_ACT_HOLD;
}

/* ==========================================================================
 * Dispatcher.  The chosen id is validated against the published mask so a
 * phase the policy has not modelled (or a future mask change) can never wedge
 * the run on a no-op action: the lowest allowed id is taken instead. */
static int dgb_first_allowed(const unsigned char* mask) {
    for (int i = 0; i < BT_ACTIONS; i++) if (mask[i]) return i;
    return BT_ACT_HOLD;
}

static int dgb_action(BtState* s, DGuideBot* b, const unsigned char* mask) {
    int a = BT_ACT_HOLD;
    if (s->max_hp > b->hp_peak) b->hp_peak = s->max_hp;
    if (s->phase == 0) a = dgb_combat_action(s, b);
    else if (s->phase == 1) a = dgb_shop_action(s, b, mask);
    else a = dgb_level_action(s, b, mask);
    if (mask && (a < 0 || a >= BT_ACTIONS || !mask[a])) a = dgb_first_allowed(mask);
    return a;
}
