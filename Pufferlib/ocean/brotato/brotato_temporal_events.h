/* brotato_temporal_events.h -- TempStats trigger consumers (TemporalEvents
 * slice).
 *
 * OWNERSHIP
 *   This header owns the *trigger* half of the TempStats mechanic: the four
 *   player-event keys that make `TempStats.add_stat` fire while the wave runs
 *   (on hit / on dodge / while moving / while not moving), plus the
 *   `stats_next_wave` bucket that the source moves into TempStats at the next
 *   wave start, and the effect-storage for all of them. It never re-implements
 *   the stat layer: every contribution goes through the existing helpers
 *   (bt_core_stat_add_temp / bt_core_stat_read / bt_row_variant_add /
 *   bt_row_owned / bt_row_get / bt_scalar_bucket_register / bt_recompute_links).
 *
 *   Placement: include it in brotato_core.h AFTER brotato_scalar_events.h and
 *   BEFORE bt_core_step, so the shared bucket storage helper and bt_recompute_
 *   links are already defined (they are also forward-declared at the top of
 *   brotato_core.h, so the core may call these helpers from bt_player_damage /
 *   bt_explode with a matching forward declaration).
 *
 *   NOT owned here: TempStatsPerInterval (opcode 31, brotato_core.h), the
 *   PercentDamageEffect stacks (opcode 23), the decaying_stats_on_hit /
 *   on_consumable temp stats (opcode 13 in bt_player_damage), and every stat
 *   *layer* API. Those already write `temp_stat[]`; this slice only adds the
 *   event-driven buckets that no core code read before.
 *
 * ---------------------------------------------------------------------------
 * SOURCE PROVENANCE (brotato_recovered v1.1.14.6 + brotato_abyss DLC)
 * ---------------------------------------------------------------------------
 *   temp_stats.gd:15-25            add_stat / remove_stat: `temp[hsh] += value`
 *                                  (raw accumulation; get_stat multiplies the
 *                                  gain only on read, temp_stats.gd:37-42)
 *   player.gd:215-243              check_not_moving_stats: on the 0->still
 *                                  transition, if the bucket is non-empty add
 *                                  each [key,value] entry (skipping
 *                                  percent_materials); on still->0 remove them
 *                                  and LinkedStats.reset_player
 *   player.gd:245-266              check_moving_stats: the mirror transition
 *                                  (no LinkedStats.reset)
 *   player.gd:190-191              both checks run every physics frame from
 *                                  update_animation, in that order
 *   player.gd:219,248              the ENTERING branch requires the bucket to
 *                                  be non-empty; the LEAVING branch does not
 *   player.gd:465-468              dodge branch: temp_stats_on_dodge
 *   player.gd:487-490              dmg_taken > 0 branch: temp_stats_on_hit
 *   player.gd:1030-1051            MovingTimer / NotMovingTimer (1 Hz) ->
 *                                  handle_gold_stat: percent_materials gold
 *   player.tscn:1388-1391          NotMovingTimer / MovingTimer: Timer
 *                                  defaults (wait_time 1.0, repeating)
 *   effect.gd:66-71                KEY_VALUE storage: bucket[custom_key] +=
 *                                  [key, value], merged per key
 *   boss.gd:52-58                  damage_against_bosses target multiplier
 * ---------------------------------------------------------------------------
 * Bull-reachable producers (the whole items/all + DLC pool; characters and
 * weapons are NOT purchasable by the Bull):
 *   temp_stats_on_hit          triangle_of_power_effect_3  (stat_percent_damage -2)
 *   temp_stats_while_not_moving barricade_effect_1  (stat_armor          +8)
 *                               chameleon_effect_1  (stat_dodge         +20)
 *                               statue_effect_1     (stat_attack_speed  +40)
 *                               dlc coral_effect_0   (stat_hp_regeneration+10)
 *   temp_stats_on_dodge        NO producer (cryptid_effect_3 is a CHARACTER
 *                              effect, applied by bt_core_reset, never
 *                              interned by extract_items.py)
 *   temp_stats_while_moving    NO producer (streamer_effect_1b/1c, CHARACTER)
 *
 * Consequently the pool interns only BT_ISTR_TEMP_STATS_ON_HIT (110) and
 * BT_ISTR_TEMP_STATS_WHILE_NOT_MOVING (39); the other two families have no
 * BT_ISTR_* key at all. Their dispatch still exists (the shape is symmetric and
 * a future pool would need no code change) but resolves to
 * BT_TEMPORAL_KEY_NONE, which no BT_ITEM_EFF_ROWS row can carry, so it is a
 * provable no-op tonight.
 *
 * stats_next_wave (main.gd:1402-1407, the "next wave" bucket): producers
 *   celery_tea_effect_0b (xp_gain +50) / celery_tea_effect_1 (enemy_health +100)
 *   peacock_effect_2 (xp_gain +100) / peacock_effect_3 (enemy_damage +50)
 *   dlc scarf_effect_3 (enemy_speed +25)
 * are KEY_VALUE/storage 1 rows whose keys (xp_gain, enemy_health, enemy_damage,
 * enemy_speed) are NOT core stats. At the next wave start the source moves the
 * whole bucket into TempStats and clears it (one-shot, temp for that wave).
 * Covered here: bucket storage + the wave-start application + the temp storage
 * for keys with no core stat id.
 *
 * A NOTE ON `adrenaline`: it is NOT a TempStats trigger. adrenaline_effect_1 is
 * a ChanceStatDamageEffect with custom_key heal_on_dodge, consumed by
 * bt_scalar_on_dodge (ScalarEvents). Do not wire it here.
 *
 * The source's percent_materials bookkeeping (handle_gold_stat, player.gd:
 * 1038-1061) is NOT reproduced: the only producer is a character effect
 * (streamer_effect_1), so its key is not interned and
 * bt_core_id_from_istr(percent_materials) would be its only reachable handle.
 * The consumer below (correctly) skips entries whose key maps to no core stat,
 * which is exactly the source's `if temp_stat[0] != percent_materials_hash`
 * gate -- so if such an entry ever appeared it would be skipped, never
 * mis-applied.
 *
 * NO REWARD and NO RNG: every helper here is a pure function of the bucket and
 * the raw stat layers (TempStats.add_stat draws no random number), so call
 * order relative to the explosion / decaying-stat loops does not perturb the
 * PCG32 stream.
 */
#pragma once

/* ---------------------------------------------------------------------------
 * Bucket keys and dispatch constants
 *
 * `BT_ITEM_EFF_ROWS[].custom_key` is an interned BT_ISTR_* id in [0,
 * BT_NITEM_STRS). temp_stats_on_dodge / temp_stats_while_moving are absent from
 * that table (see the provenance note above), so their target is the sentinel
 * below, which cannot equal any real interned key. If a future item pool
 * interns them, point these at `BT_ISTR_TEMP_STATS_ON_DODGE` /
 * `BT_ISTR_TEMP_STATS_WHILE_MOVING` and delete the sentinel note.
 * ------------------------------------------------------------------------- */
#define BT_TEMPORAL_KEY_ON_HIT         BT_ISTR_TEMP_STATS_ON_HIT
#define BT_TEMPORAL_KEY_WHILE_NOT_MOVING BT_ISTR_TEMP_STATS_WHILE_NOT_MOVING
#define BT_TEMPORAL_KEY_STATS_NEXT_WAVE BT_ISTR_STATS_NEXT_WAVE
#define BT_TEMPORAL_KEY_ON_DODGE       0xFFFFu   /* no producer / not interned */
#define BT_TEMPORAL_KEY_WHILE_MOVING   0xFFFFu   /* no producer / not interned */
#define BT_TEMPORAL_KEY_NONE           0xFFFFu
#if BT_NITEM_STRS >= BT_TEMPORAL_KEY_NONE
#error "temporal sentinel key must not be a real interned id"
#endif

/* ---------------------------------------------------------------------------
 * Effect storage (parent hook)
 *
 * effect.gd:66-71 bukkets every KEY_VALUE effect by custom_key; the core keeps
 * ONE generic row registry for all of them (bk_active / bk_count / rowvar),
 * written by bt_scalar_bucket_register. temp_stats_* rows use exactly that
 * storage, so this is a named, slice-owned entry point that just forwards --
 * the parent must call ONE of the two for a given row, never both.
 * `nv` is the (possibly curse-boosted) Effect.value; `mod_pct` is the curse
 * modifier in percent (0 when not cursed).
 * ------------------------------------------------------------------------- */
BT_HD static void bt_temporal_bucket_register(BT_DEVICE BtState* s, int row, int nv, int mod_pct) {
    bt_scalar_bucket_register(s, row, nv, mod_pct);
}

/* 1 when at least one owned row targets `key` (the source's `arr.size() > 0`
 * gate on the ENTERING transition). */
BT_HD static int bt_temporal_has(BT_DEVICE BtState* s, unsigned short key) {
    if (key == BT_TEMPORAL_KEY_NONE) return 0;
    for (int a = 0; a < s->n_bk_active; a++) {
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[s->bk_active[a]];
        if (E->custom_key == key) return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Apply (sign = +1) or reverse (sign = -1) one TempStats bucket.
 *
 * Source order: `for entry in bucket: TempStats.add_stat(entry.key, entry.
 * value)` (player.gd:224-227, 253-256, 468-469, 488-489). The source bucket is
 * merged per key (effect.gd:66-71), but add/remove are additive and symmetric,
 * so iterating the owning ROWS and summing each row's OWNED copies
 * (bt_row_owned / bt_row_get) reproduces the same total -- and keeps mixed
 * cursed/uncursed copies of one effect at their own values.
 *
 * Boundaries: the contribution lands in the temporary layer only
 * (bt_core_stat_add_temp). It is NEVER written into stat_raw, and it is never
 * clamped here: bt_core_stat_sync derives (raw + temp + link) * gain and applies
 * the hp/speed/dodge/crit caps, so a temp buff is always reversible and can
 * never exceed a cap. Entries whose key maps to no core stat (id 255) are
 * skipped -- that is the source's percent_materials gate.
 *
 * Returns the signed total pushed into the temp layer (0 for a no-op).
 * ------------------------------------------------------------------------- */
BT_HD static float bt_temporal_apply_bucket(BT_DEVICE BtState* s, unsigned short key, float sign) {
    float applied = 0.0f;
    if (key == BT_TEMPORAL_KEY_NONE) return 0.0f;
    for (int a = 0; a < s->n_bk_active; a++) {
        int r = (int)s->bk_active[a];
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[r];
        if (E->custom_key != key) continue;
        int id = bt_core_id_from_istr(E->key);
        if (id == 255) continue;   /* not a core stat (percent_materials, ...) */
        for (int c = 0, n = bt_row_owned(s, r); c < n; c++) {
            int v = 0, v2 = 0;
            if (!bt_row_get(s, r, E, c, &v, &v2)) break;
            float d = sign * (float)v;
            bt_core_stat_add_temp(s, id, d);
            applied += d;
        }
    }
    return applied;
}

/* ---------------------------------------------------------------------------
 * On hit (player.gd:487-490): dmg_taken > 0 -> for each temp_stats_on_hit entry
 * TempStats.add_stat(key, value). There is NO lifetime: the buff stacks on every
 * hit and is only cleared by TempStats.reset() at the wave boundary
 * (bt_core_stat_reset_temp). Call once per player blow that actually took HP,
 * AFTER the health reduction (same place as the TSI reset_on_hit /
 * decaying_stats_on_hit loops in bt_player_damage).
 * ------------------------------------------------------------------------- */
BT_HD static float bt_temporal_on_hit(BT_DEVICE BtState* s) {
    return bt_temporal_apply_bucket(s, BT_TEMPORAL_KEY_ON_HIT, 1.0f);
}

/* On dodge (player.gd:465-468): fired only on a successful dodge roll, AFTER
 * bt_scalar_on_dodge (dmg_on_dodge -> heal_on_dodge -> temp_stats_on_dodge).
 * No producer exists in the Bull pool, so this is a documented no-op tonight. */
BT_HD static float bt_temporal_on_dodge(BT_DEVICE BtState* s) {
    return bt_temporal_apply_bucket(s, BT_TEMPORAL_KEY_ON_DODGE, 1.0f);
}

/* ---------------------------------------------------------------------------
 * Movement transitions (player.gd:215-266).
 *
 * One call per combat frame with the raw INPUT moving flag (the source tests
 * `movement.x != 0 or movement.y != 0`, i.e. the pressed direction, not the
 * resolved displacement; the core's `BT_MOVE_X[action] != 0 ||
 * BT_MOVE_Y[action] != 0` is that predicate). Ordering inside the call is the
 * source's: check_not_moving_stats first, then check_moving_stats.
 *
 *   still -> still : no-op
 *   move  -> still : if the while_not_moving bucket exists and the flag is not
 *                    yet set, set it and ADD every entry, then refresh links
 *                    (the source's LinkedStats.reset_player is only called from
 *                    THIS branch, player.gd:240-241 -- a quirk kept as-is)
 *   still -> move  : if the flag is set, clear it and REMOVE every entry
 *   move  -> move  : the while_moving mirror (no producer in this pool)
 *
 * `moving_bonuses_applied` / `not_moving_bonuses_applied` are the two BtState
 * flags the parent adds (see the PARENT REQUIREMENT block at the end).
 * ------------------------------------------------------------------------- */
BT_HD static void bt_temporal_set_moving(BT_DEVICE BtState* s, int moving) {
    /* check_not_moving_stats (player.gd:215-243) */
    if (!s->not_moving_bonuses_applied && moving == 0 &&
        bt_temporal_has(s, BT_TEMPORAL_KEY_WHILE_NOT_MOVING)) {
        s->not_moving_bonuses_applied = 1;
        (void)bt_temporal_apply_bucket(s, BT_TEMPORAL_KEY_WHILE_NOT_MOVING, 1.0f);
        bt_recompute_links(s);
    } else if (s->not_moving_bonuses_applied && moving != 0) {
        s->not_moving_bonuses_applied = 0;
        (void)bt_temporal_apply_bucket(s, BT_TEMPORAL_KEY_WHILE_NOT_MOVING, -1.0f);
        bt_recompute_links(s);
    }
    /* check_moving_stats (player.gd:245-266) */
    if (!s->moving_bonuses_applied && moving != 0 &&
        bt_temporal_has(s, BT_TEMPORAL_KEY_WHILE_MOVING)) {
        s->moving_bonuses_applied = 1;
        (void)bt_temporal_apply_bucket(s, BT_TEMPORAL_KEY_WHILE_MOVING, 1.0f);
    } else if (s->moving_bonuses_applied && moving == 0) {
        s->moving_bonuses_applied = 0;
        (void)bt_temporal_apply_bucket(s, BT_TEMPORAL_KEY_WHILE_MOVING, -1.0f);
    }
}

/* ---------------------------------------------------------------------------
 * Wave-boundary reset (main.gd:1234 TempStats.reset).
 *
 * MUST run together with bt_core_stat_reset_temp: that helper zeroes
 * temp_stat[], and this clears the two transition flags. They are a pair -- if
 * the flags survived a temp wipe, the next still->move transition would SUBTRACT
 * entries that were never added. Call both from bt_progress_end_wave, right
 * where bt_core_stat_reset_temp already is (brotato_progression.h). A full
 * episode reset (bt_core_reset) must also zero the two flags.
 * ------------------------------------------------------------------------- */
BT_HD static void bt_temporal_reset(BT_DEVICE BtState* s) {
    s->moving_bonuses_applied = 0;
    s->not_moving_bonuses_applied = 0;
    /* TempStats.reset() also drops the stats_next_wave temp scalars. */
    for (int i = 0; i < BT_NITEM_STRS; i++) s->temp_scalar[i] = 0.0f;
}

/* ---------------------------------------------------------------------------
 * stats_next_wave (main.gd:1402-1407) -- "the stats you gain next wave".
 *
 * The source keeps a per-player dictionary bucket keyed by the interned stat
 * hash (effect.gd:66-71 KEY_VALUE storage), and at the START of the next wave
 * moves the whole bucket into TempStats then .clear()s it:
 *
 *     for stat_next_wave in effects[stats_next_wave_hash]:
 *         TempStats.add_stat(stat_next_wave[0], stat_next_wave[1], player_index)
 *     effects[stats_next_wave_hash].clear()
 *
 * So it is a ONE-SHOT, TEMPORARY gain for exactly the wave after acquisition:
 * the applied value lives in the temp layer for that wave (cleared at the wave
 * boundary by bt_temporal_reset / bt_core_stat_reset_temp) and the bucket never
 * fires again unless the item is acquired anew (re-register).
 *
 * Pool producers (all KEY_VALUE/storage 1): peacock_effect_2 (xp_gain +100),
 * celery_tea_effect_0b (xp_gain +50), celery_tea_effect_1 (enemy_health +100),
 * peacock_effect_3 (enemy_damage +50), dlc scarf_effect_3 (enemy_speed +25).
 * Every one of those keys has NO core stat id (bt_core_id_from_istr == 255), so
 * they cannot use temp_stat[]; they land in temp_scalar[] (indexed by the same
 * interned key the rest of the core uses for scalar_effect[]). A key that DOES
 * map to a core stat (none in the shipped pool, but the source handles them
 * uniformly) goes to the normal temporary layer so gain/caps apply.
 * ------------------------------------------------------------------------- */
BT_HD static float bt_temporal_temp_scalar(BT_DEVICE BtState* s, unsigned short key) {
    return (key != BT_ISTR_NONE && key < BT_NITEM_STRS) ? s->temp_scalar[key] : 0.0f;
}

BT_HD static void bt_temporal_apply_stats_next_wave(BT_DEVICE BtState* s) {
    /* 1. move every bucket entry into the temp layers. */
    for (int a = 0; a < s->n_bk_active; a++) {
        int r = (int)s->bk_active[a];
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[r];
        if (E->custom_key != BT_TEMPORAL_KEY_STATS_NEXT_WAVE) continue;
        if (E->key == BT_ISTR_NONE || E->key >= BT_NITEM_STRS) continue;
        int id = bt_core_id_from_istr(E->key);
        for (int c = 0, n = bt_row_owned(s, r); c < n; c++) {
            int v = 0, v2 = 0;
            if (!bt_row_get(s, r, E, c, &v, &v2)) break;
            if (id != 255) bt_core_stat_add_temp(s, id, (float)v);
            else s->temp_scalar[E->key] += (float)v;
        }
    }
    /* 2. clear the bucket (effects[stats_next_wave_hash].clear()), leaving the
     *    other buckets in bk_active untouched. */
    int w = 0;
    for (int a = 0; a < s->n_bk_active; a++) {
        int r = (int)s->bk_active[a];
        if (BT_ITEM_EFF_ROWS[r].custom_key == BT_TEMPORAL_KEY_STATS_NEXT_WAVE) {
            /* The source clears the whole bucket: the row must be re-registered
             * from scratch when the item is acquired again (otherwise the stale
             * per-copy variants would double-count on the next wave start). */
            s->bk_count[r] = 0;
            s->nvar[r] = 0;
            continue;
        }
        s->bk_active[w++] = s->bk_active[a];
    }
    s->n_bk_active = w;
}

/* Reader for the core's existing scalar consumers: the EFFECTIVE scalar value
 * (permanent SumData effect + this wave's temp contribution), which is
 * Utils.get_stat(key) for a key with no core stat id. Consumers that today read
 * `s->scalar_effect[key]` must add bt_temporal_temp_scalar(s, key); see hook 10.
 * For a key that maps to a core stat this is 0 and bt_core_stat_read is used
 * instead (the temp already sits in temp_stat[]). */
BT_HD static float bt_temporal_scalar_effective(BT_DEVICE BtState* s, unsigned short key) {
    float raw = s->scalar_effect[key];
    int id = bt_core_id_from_istr(key);
    if (id != 255) return bt_core_stat_read(s, id);
    return raw + s->temp_scalar[key];
}

/* ---------------------------------------------------------------------------
 * Boss/elite damage multiplier (boss.gd:52-58).
 *
 *   damage_against_bosses = RunData.get_player_effect(key)
 *   if damage_against_bosses > 0:
 *       dmg = int(value * (1 + Utils.get_stat(key)/100))
 *
 * Utils.get_stat = RunData.get_stat * get_stat_gain = effect * (1 + gain/100).
 * damage_against_bosses is not a core stat (bt_core_id_from_istr == 255), so
 * its SUM storage lives in scalar_effect[] (bt_apply_item's id==255 branch) and
 * no "gain_damage_against_bosses" effect exists in the pool, so gain == 1.
 * Generic over `key` so other producers (jack, a character) share it; the Bull
 * reach is all/silver_bullet (+25%).
 *
 * Bull-reachable because the Bull's explosion carries from_player_index. The
 * CALLER decides boss-ness: apply it only when the target's kind class is
 * elite/boss (bt_kind_class(kind) != 0).
 * ------------------------------------------------------------------------- */
BT_HD static float bt_temporal_target_damage_mult(BT_DEVICE BtState* s, unsigned short key) {
    float raw = s->scalar_effect[key];
    if (raw <= 0.0f) return 1.0f;                 /* gate: get_player_effect > 0 */
    int id = bt_core_id_from_istr(key);
    float eff = (id != 255) ? bt_core_stat_read(s, id) : raw;
    return 1.0f + eff * 0.01f;
}

