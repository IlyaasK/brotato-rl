/* brotato_enemy_hit_effects.h -- the ENEMY hit-debuff runtime for
 * enemy_percent_damage_taken (percent_damage_taken_enemy_effect_behavior.gd).
 *
 * OWNERSHIP
 *   One mechanic: the per-ENEMY "takes N% more damage" debuff granted by
 *   effects whose custom_key is `enemy_percent_damage_taken`.  The POD state is
 *   `BtEnemyHitFx ehitfx;` in brotato_state.h (the same split BtExplosion /
 *   BtItemExplosions use).  Per-effect numbers (key, value, duration_secs,
 *   max_stacks, max_procs) are read from the canonical BtItemEff row; the
 *   identity is that row's interned source_id (`E->aux_key`, i.e.
 *   Keys.generate_hash(source_id) collapsed to the interned string -- the only
 *   two source_ids in the shipped data, "item_ice_cube" and "weapon_lute", do
 *   not collide, so all four lute tiers share ONE identity exactly as the
 *   source's source_id_hash does).  Shipped producers (5 rows):
 *     461  PercentDamageEffect        ice_cube (item)   key stat_elemental_damage
 *     857  WeaponPercentDamageEffect  lute 1 (weapon)   key stat_all
 *     863/864/865                     lute 2/3/4         key stat_all
 *
 *   It never re-implements damage: it only ACCUMULATES a percent that the ONE
 *   enemy-damage formula (bt_item_expl_hit_enemy) folds into its existing `pct`
 *   term, and that the burn tick folds the same way (get_damage_value's
 *   `round(dmg * (1 + total_bonus_damage/100))`, which runs BEFORE the flat
 *   armor subtraction).  It draws NO RNG.
 *
 * SOURCE (brotato_recovered v1.1.14.6 + brotato_abyss DLC)
 *   effect_behaviors/enemies/percent_damage_taken_enemy_effect_behavior.gd
 *     :9-20  ActiveEffect fields / _effects_proc_count   :23-28 _process timer
 *     :31-41 should_add_on_spawn  :44-58 on_hurt (:65-71 the scaling gate)
 *     :61-63 on_burned  :73-118 add_active_effect  :121-137 expiry
 *     :140-141 get_bonus_damage
 *   entities/units/unit/unit.gd :548-556 (on_hurt before the exploding roll and
 *     take_damage), :458-500 (get_damage_value), :661-707 (burn tick on_burned)
 *   effects/items/percent_damage_effect.gd:46-47 (bucket push)
 *   dlcs/dlc_1/dlc_1_data.gd:261/360-375 (cursed value boost, positive)
 *
 * SEMANTICS (the source's, not an approximation)
 *   * identity       E->aux_key (per source_id), never the row/uid
 *   * gate           `key == stat_all` OR the hit's scaling view holds `key`.
 *                    The view is a MASK over the 19 core stat ids, computed BY
 *                    THE CALLER from its own key view (one scalar OR per key --
 *                    no array copy, no pointer across an address space), and the
 *                    row's key is mapped with the same canonical
 *                    bt_core_id_from_istr the rest of the core uses.
 *   * merge          per source_id: pct = max, duration = max, max_stacks = max,
 *                    timer = duration (refresh), then +1 stack up to the cap;
 *                    each added stack adds the current (max) pct to the enemy's
 *                    total and to that effect's own `added` total
 *   * proc count     a NEW active effect RESTARTS the count at 1; the count is
 *                    the stack count while active and PERSISTS after expiry
 *                    (_effects_proc_count is never erased), so ice_cube
 *                    (max_procs 1) can never re-apply to the same enemy.  The
 *                    gate runs BEFORE the merge/refresh (:75-76).
 *   * expiry/clock   ActiveEffect.timer is a GDScript float that _process
 *                    subtracts the frame delta from EVERY frame, expiring when
 *                    `timer <= 0`.  The engine was MEASURED (pinned Godot 3.7 at
 *                    --fixed-fps 60): _process(delta) delivers delta = 0.016667
 *                    -- a binary32-wide value, NOT an exact binary64 1/60 -- so
 *                    the canonical 3 s effect expires on frame 180 with a
 *                    NEGATIVE residual.  The timer is therefore a frame counter
 *                    of duration_secs * 60 frames, decremented once per step and
 *                    expired at 0, which reproduces the measured oracle for every
 *                    shipped duration_secs; the regression test pins frame 180.
 *   * expiry effect  only that record's own `added` total leaves the enemy's
 *                    percent (:121-137); nothing else is disturbed
 *   * neutrals       no effect behaviors (init_effect_behaviors is Enemy-only)
 *   * respawn        unit.gd die() frees the behavior node, Enemy.respawn ->
 *                    init_effect_behaviors builds a fresh one: cleared per life
 *
 * CALLER CONTRACT (all wired)
 *   Melee / Projectile, at the source on_hurt point (top of the hit, BEFORE the
 *   exploding roll and the damage), from their OWN pointers:
 *       unsigned int mask = 0u;
 *       for (q...) mask = bt_enemy_hit_mask_add(mask, keys[q]);
 *       bt_enemy_hit_bucket_apply(s, j, mask);            // item bucket
 *       for (i < n_rows) bt_enemy_hit_apply_row(s, j, rows[i], curse[i], mask);
 *   The curse argument is the per-row SCALAR modifier (0 for uncursed), so no
 *   array is ever passed.  burn: bt_enemy_hit_burn_apply(s, j).
 *   bt_core_reset -> bt_enemy_hit_reset; bt_spawn_kind_at -> _respawn;
 *   bt_core_step -> bt_enemy_hit_tick (once per step);
 *   bt_item_expl_hit_enemy: `pct += (float)bt_enemy_hit_percent_taken(s, j);`
 *
 * NAMED DEVIATION
 *   ActiveEffect.outline_color / _parent.add_outline is visual only.
 */
#pragma once

#include "brotato_rng.h"                    /* BT_HD / BT_DEVICE / BT_CONST_PTR */
#include "brotato_item_effects.h"           /* BT_ITEM_EFF_ROWS / BT_ISTR_*     */
#include "brotato_enemy_hit_effect_data.h"  /* BT_EHE_NSOURCES / BT_EHE_SOURCE_ID */

/* Per-enemy distinct source_id records == the canonical distinct-source count
 * (2 shipped: item_ice_cube, weapon_lute).  Overflow is explicit. */
#ifndef BT_NEH_MAX_ACTIVE
#define BT_NEH_MAX_ACTIVE BT_EHE_NSOURCES
#endif

/* One frame of the source clock: the measured engine delta at 60 fps is
 * 0.016667, so duration_secs expires at duration_secs * 60 frames. */
#define BT_NEH_FRAMES_PER_SEC 60

/* Defined in brotato_weapon_effects.h, later in the TU (the same forward-declare
 * pattern bt_scalar_events.h uses for bt_enemy_incoming_damage). */
BT_HD static float bt_wfx_curse_boost_good(float value, float mod);

/* ---------------------------------------------------------------------------
 * the hit's scaling view as a value: one bit per core stat id
 * ------------------------------------------------------------------------- */

/* Fold one of the caller's own scaling keys into the mask.  The caller reads
 * its keys itself (its own address space, one scalar at a time), so nothing is
 * copied and no pointer crosses a space. */
BT_HD static unsigned int bt_enemy_hit_mask_add(unsigned int mask, unsigned short key) {
    int id = bt_core_id_from_istr(key);
    if (id >= 0 && id < BT_NSTATS) mask |= (1u << id);
    return mask;
}

/* try_add_effects' gate (weapon_service.gd:481-486 find_scaling_stat plus the
 * stat_all fallback): the row applies when its key is stat_all, or when the
 * hit's mask holds the row's key.  Canonical ids throughout. */
BT_HD static int bt_enemy_hit_row_ok(unsigned short row_key, unsigned int mask) {
    if (row_key == (unsigned short)BT_ISTR_STAT_ALL) return 1;
    int id = bt_core_id_from_istr(row_key);
    return (id >= 0 && id < BT_NSTATS && ((mask >> id) & 1u) != 0u) ? 1 : 0;
}

/* An enemy_percent_damage_taken effect row: the source collects a hitbox row
 * only when `effect.custom_key_hash == Keys.enemy_percent_damage_taken_hash`,
 * and only the PercentDamageEffect family carries the debuff (the item row is
 * PercentDamageEffect, the weapon rows WeaponPercentDamageEffect). */
BT_HD static int bt_enemy_hit_is_ehe(BT_CONST_PTR(BtItemEff) E) {
    if (E->custom_key != (unsigned short)BT_ISTR_ENEMY_PERCENT_DAMAGE_TAKEN) return 0;
    return E->opcode == BT_ITEMEFF_PERCENT_DAMAGE ||
           E->opcode == BT_ITEMEFF_WEAPON_PERCENT_DAMAGE;
}

/* ---------------------------------------------------------------------------
 * lifecycle
 * ------------------------------------------------------------------------- */

/* One enemy's whole record set: the active flag and the proc count are per life
 * (cleared together on respawn). */
BT_HD static void bt_enemy_hit_clear_enemy(BT_DEVICE BtEnemyHitFx* d, int j) {
    if (j < 0 || j >= BT_MAX) return;
    d->n_rec[j] = 0;
    d->total[j] = 0;
    for (int r = 0; r < BT_NEH_MAX_ACTIVE; r++) {
        d->src[j][r] = 0;
        d->active[j][r] = 0;
        d->stacks[j][r] = 0;
        d->maxstacks[j][r] = 0;
        d->pct[j][r] = 0;
        d->added[j][r] = 0;
        d->timer[j][r] = 0;
        d->dur[j][r] = 0;
        d->proc[j][r] = 0;
    }
}

/* bt_core_reset (a fresh run): every record and the overflow flag. */
BT_HD static void bt_enemy_hit_reset(BT_DEVICE BtState* s) {
    s->ehitfx.overflow = 0;
    for (int j = 0; j < BT_MAX; j++) bt_enemy_hit_clear_enemy(&s->ehitfx, j);
}

/* Every spawn (bt_spawn_kind_at): Enemy.respawn -> init_effect_behaviors builds
 * a FRESH behavior node, so nothing is inherited from the previous occupant. */
BT_HD static void bt_enemy_hit_respawn(BT_DEVICE BtState* s, int j) {
    bt_enemy_hit_clear_enemy(&s->ehitfx, j);
}

/* get_bonus_damage: the enemy's accumulated percent.  Neutrals carry no effect
 * behavior in the source, so a tree is never debuffed. */
BT_HD static int bt_enemy_hit_percent_taken(BT_DEVICE BtState* s, int j) {
    if (j < 0 || j >= BT_MAX) return 0;
    if (s->eneutral[j]) return 0;
    return (int)s->ehitfx.total[j];
}

/* ---------------------------------------------------------------------------
 * apply (on_hurt / on_burned)
 * ------------------------------------------------------------------------- */

/* add_active_effect for ONE candidate effect (already gate-checked):
 *   [source_id, key, value, duration_secs, max_stacks, max_procs, color]
 * `frames` is the source's duration_secs * BT_NEH_FRAMES_PER_SEC (the frame
 * clock above); the timer is assigned that value on creation and on refresh. */
BT_HD static void bt_enemy_hit_add_one(BT_DEVICE BtEnemyHitFx* d, int j, unsigned short src,
                                       int pct, int frames, int max_stacks, int max_procs) {
    if (max_stacks < 1) max_stacks = 1;
    if (frames < 1) frames = 1;
    int rec = -1;
    for (int r = 0; r < (int)d->n_rec[j]; r++)
        if (d->src[j][r] == src) { rec = r; break; }
    /* :75-76 -- the proc gate runs BEFORE the merge/refresh, over the PERSISTENT
     * per-source count (a source that already proc'd its limit may neither
     * refresh nor stack again). */
    if (max_procs != -1 && rec >= 0 && (int)d->proc[j][rec] >= max_procs) return;
    if (rec < 0) {
        if ((int)d->n_rec[j] >= BT_NEH_MAX_ACTIVE) { d->overflow = 1; return; }
        rec = (int)d->n_rec[j];
        d->n_rec[j] = (unsigned char)(rec + 1);
        d->src[j][rec] = src;
        d->active[j][rec] = 0;
        d->stacks[j][rec] = 0;
        d->maxstacks[j][rec] = 0;
        d->pct[j][rec] = 0;
        d->added[j][rec] = 0;
        d->timer[j][rec] = 0;
        d->dur[j][rec] = 0;
        d->proc[j][rec] = 0;
    }
    if (!d->active[j][rec]) {
        /* :108-118 new ActiveEffect: proc count RESTARTS at 1, first stack
         * applied now, timer = duration. */
        d->active[j][rec] = 1;
        d->proc[j][rec] = 1;
        d->stacks[j][rec] = 1;
        d->maxstacks[j][rec] = (unsigned char)(max_stacks > 255 ? 255 : max_stacks);
        d->pct[j][rec] = (short)(pct > 32767 ? 32767 : (pct < -32768 ? -32768 : pct));
        d->added[j][rec] = d->pct[j][rec];
        d->dur[j][rec] = (unsigned short)(frames > 65535 ? 65535 : frames);
        d->timer[j][rec] = d->dur[j][rec];
        d->total[j] = (short)(d->total[j] + d->pct[j][rec]);
        return;
    }
    /* :94-106 already_exists: max-merge the per-effect values, refresh the timer
     * to the (possibly extended) duration, then stack if there is room. */
    if (max_stacks > (int)d->maxstacks[j][rec])
        d->maxstacks[j][rec] = (unsigned char)(max_stacks > 255 ? 255 : max_stacks);
    if (frames > (int)d->dur[j][rec])
        d->dur[j][rec] = (unsigned short)(frames > 65535 ? 65535 : frames);
    if (pct > (int)d->pct[j][rec])
        d->pct[j][rec] = (short)(pct > 32767 ? 32767 : (pct < -32768 ? -32768 : pct));
    d->timer[j][rec] = d->dur[j][rec];
    if ((int)d->stacks[j][rec] >= (int)d->maxstacks[j][rec]) return;
    d->stacks[j][rec] = (unsigned char)((int)d->stacks[j][rec] + 1);
    d->total[j] = (short)(d->total[j] + d->pct[j][rec]);
    d->added[j][rec] = (short)(d->added[j][rec] + d->pct[j][rec]);
    d->proc[j][rec] = (unsigned char)((int)d->proc[j][rec] + 1);
}

/* ONE of the hitbox's own canonical rows, applied at its source-order position
 * (the caller loops its own rows, so no row array is passed).  `curse_mod` is
 * that row's per-copy curse modifier as a SCALAR (0 when uncursed); a cursed
 * row's value is boosted positively (dlc_1_data.gd:261).  Returns 1 when the
 * row was an ehe row AND passed the scaling gate. */
BT_HD static int bt_enemy_hit_apply_row(BT_DEVICE BtState* s, int j, int row,
                                        float curse_mod, unsigned int mask) {
    if (j < 0 || j >= BT_MAX || s->eneutral[j]) return 0;
    if (row < 0 || row >= BT_NITEM_EFF_ROWS) return 0;
    BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[row];
    if (!bt_enemy_hit_is_ehe(E)) return 0;
    if (!bt_enemy_hit_row_ok(E->key, mask)) return 0;
    int pct = E->value;
    if (curse_mod != 0.0f) pct = (int)bt_wfx_curse_boost_good((float)E->value, curse_mod);
    bt_enemy_hit_add_one(&s->ehitfx, j, E->aux_key, pct,
                         (E->duration_secs > 0 ? E->duration_secs : 3) * BT_NEH_FRAMES_PER_SEC,
                         E->max_stacks > 0 ? E->max_stacks : 1, E->max_procs);
    return 1;
}

/* The player item bucket pass (RunData.get_player_effect(key), collected FIRST
 * by on_hurt: `effects.append_array(item_effects)`), in registration
 * (acquisition) order.  One call per hit, after the caller's mask is built. */
BT_HD static void bt_enemy_hit_bucket_apply(BT_DEVICE BtState* s, int j, unsigned int mask) {
    if (j < 0 || j >= BT_MAX || s->eneutral[j]) return;
    for (int a = 0; a < s->n_bk_active; a++) {
        int row = (int)s->bk_active[a];
        if (row < 0 || row >= BT_NITEM_EFF_ROWS) continue;
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[row];
        if (!bt_enemy_hit_is_ehe(E)) continue;
        if (!bt_enemy_hit_row_ok(E->key, mask)) continue;
        int frames = (E->duration_secs > 0 ? E->duration_secs : 3) * BT_NEH_FRAMES_PER_SEC;
        int maxs = E->max_stacks > 0 ? E->max_stacks : 1;
        /* each OWNED copy of the row is one push into the source's effect Array */
        for (int c = 0, n = bt_row_owned(s, row); c < n; c++) {
            int bv = 0, bv2 = 0;
            if (!bt_row_get(s, row, E, c, &bv, &bv2)) break;
            bt_enemy_hit_add_one(&s->ehitfx, j, E->aux_key, bv, frames, maxs, E->max_procs);
        }
    }
}

/* on_burned (unit.gd:661-668, before the burn tick's damage): the same bucket
 * pass with the BurningData's scaling stats -- the resource default
 * [["stat_elemental_damage", 1.0]], and no item/weapon burning row overrides it
 * (BT_ITEM_BURNING carries damage/duration/spread only).  A burn tick carries no
 * hitbox rows and its source is the burn itself, so no curse applies. */
BT_HD static void bt_enemy_hit_burn_apply(BT_DEVICE BtState* s, int j) {
    unsigned int mask = bt_enemy_hit_mask_add(0u, (unsigned short)BT_ISTR_STAT_ELEMENTAL_DAMAGE);
    bt_enemy_hit_bucket_apply(s, j, mask);
}

/* ---------------------------------------------------------------------------
 * per-step decay (_process: timer -= delta, expire at <= 0)
 * ONCE per step from bt_core_step.  See the clock note above: duration_secs
 * expires at duration_secs * 60 frames, the measured engine oracle.  On expiry
 * only that record's own `added` total leaves the enemy's percent. */
BT_HD static void bt_enemy_hit_tick(BT_DEVICE BtState* s) {
    BT_DEVICE BtEnemyHitFx* d = &s->ehitfx;
    for (int j = 0; j < (int)s->e_used; j++) {
        if (d->n_rec[j] == 0) continue;
        for (int r = 0; r < (int)d->n_rec[j]; r++) {
            if (!d->active[j][r]) continue;
            if (d->timer[j][r] > 0) d->timer[j][r] = (unsigned short)(d->timer[j][r] - 1);
            if (d->timer[j][r] == 0) {
                d->total[j] = (short)(d->total[j] - d->added[j][r]);
                d->active[j][r] = 0;
            }
        }
    }
}
