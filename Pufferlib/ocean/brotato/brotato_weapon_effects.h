/* brotato_weapon_effects.h -- player WEAPON effect consumers + the weapon-only
 * item consumers, kept out of the core (WeaponEffects slice).
 *
 * OWNERSHIP
 *   This header owns the *weapon-event* consumers of the item/weapon effect
 *   graph: the shoot / attack / hit / crit / kill callbacks Melee, Ranged,
 *   Projectiles and Inventory call, plus every weapon-only item key that becomes
 *   reachable once real player weapons exist.  It never re-implements combat:
 *   player damage, healing, enemy damage, explosions, projectiles, burning,
 *   stats and kills all go back through the core helpers (bt_core_stat_read /
 *   bt_core_stat_add / bt_scalar_heal / bt_apply_enemy_burn / bt_item_expl_spawn /
 *   bt_scaling_damage / bt_chance / bt_rng_bounded), and
 *   the enemy-hit DAMAGE formula stays the single one in bt_item_expl_hit_enemy
 *   (brotato_item_explosions.h), which reads BtEnemyHitContext.
 *
 *   The POD types and constants live in brotato_weapon_effect_types.h so
 *   brotato_state.h can embed `BtWeaponFxState` before this header is parsed.
 *   This header is included by brotato_core.h AFTER brotato_scalar_events.h and
 *   BEFORE bt_core_step, so every helper it calls is already defined.
 *
 * EFFECT VIEW CONTRACT
 *   WeaponData appends every player weapon effect graph to the EXISTING canonical
 *   tables, so a weapon's `effects[]` is a contiguous slice of CANONICAL ROW IDS:
 *   BtWeaponDef.fx_first/fx_count -> BT_WEAPON_EFF_INDEX -> BT_ITEM_EFF_ROWS.
 *   Every function here therefore takes `BT_CONST_PTR(unsigned short) rows,
 *   int n_rows` (canonical ids) or a single canonical `unsigned short row`; the
 *   same view travels inside BtEnemyHitContext.rows. Nothing copies a row,
 *   nothing is allocated, and one device pointer reaches the table on MSL/CUDA.
 *
 * SOURCE PROVENANCE (brotato_recovered v1.1.14.6 + brotato_abyss DLC)
 *   singletons/weapon_service.gd:139-168  WeaponTypeBonus / ClassBonus
 *   singletons/weapon_service.gd:170-205  WeaponStack / WeaponGainStatForEveryStat
 *   singletons/weapon_service.gd:186-190  WeaponSlowOnHit
 *   singletons/weapon_service.gd:237-281  init_base_stats: cooldown, crit, knockback
 *   singletons/weapon_service.gd:270-281  knockback clamp + negative_knockback
 *   singletons/weapon_service.gd:558-600  min/max cooldown, attack speed
 *   singletons/weapon_service.gd:608-649  weapon_scaling_stats / beast master
 *   singletons/run_data.gd:1297-1302      manage_life_steal
 *   weapons/weapon.gd:203-230             on_killed_something
 *   weapons/weapon.gd:270-303             should_shoot / shoot (reload_turrets)
 *   weapons/weapon.gd:318-347             get_next_cooldown / spread
 *   weapons/weapon.gd:440-465             on_weapon_hit_something (break_on_hit)
 *   projectiles/player_projectile.gd:130-150  crit -> bounce/pierce charges
 *   projectiles/bullet_taser/taser_projectile.gd:6-14 +
 *   projectiles/bullet_taser/delayed_taser_projectile.gd:6-14
 *                                         SlowHitbox -> add_decaying_speed(-200)
 *   entities/units/unit/unit.gd:282-300   OneShotOnHit / crit_on_hitting_burning
 *   entities/units/unit/unit.gd:348-386   crit-kill gold/heal, heal_on_kill
 *   entities/units/unit/unit.gd:458-500   get_damage_value bonus %
 *   entities/units/unit/unit.gd:548-588   weapon effects + projectiles_on_hit
 *   entities/units/unit/unit.gd:735-746   add_decaying_speed
 *   entities/units/enemies/enemy.gd:236-243  explode_on_overkill
 *   entities/units/enemies/enemy.gd:266-300  bonus_current_health_damage
 *   dlcs/dlc_1/effect_behaviors/enemy/charm_enemy_effect_behavior.gd:36-70
 *                                         charm-on-hit roll + HP threshold
 *   entities/units/player/player.gd:724-728  LifestealTimer gating (0.1 s)
 *   main.gd:798-860                       reload_when_pickup_gold
 *   effects/weapons/<class>.gd            weapon effect classes
 *
 * AUDIT (implemented here)
 *   lifesteal (LifestealTimer-gated), gold_on_crit_kill, heal_on_crit_kill,
 *   heal_on_kill, explode_on_overkill,
 *   bonus_damage_against_targets_above/below_hp,
 *   bonus_non_elemental_damage_against_burning_targets,
 *   bonus_current_health_damage, crit_on_hitting_burning_target, one_shot_on_hit,
 *   gain_stat_when_attack_killed_enemies, GainStatEveryKilledEnemiesEffect,
 *   break_on_hit, reload_when_pickup_gold, reload_turrets_on_shoot, the
 *   owner_uid+attack_id hit/kill tallies, the random cooldown window + big
 *   reload, accuracy shot spread, weapon-row exploding / burning application,
 *   charm_on_hit (CharmEnemyEffectBehavior roll), and the elemental damage-type
 *   classification.
 *
 *   Deliberately NOT kept here (single owners elsewhere, no second engine):
 *     projectiles_on_hit  -- the ProjectilesOnHitEffect opcode (the
 *       `effect_lightning_on_hit` family is the same opcode) is consumed by the
 *       weapon-projectile graph: WeaponStats resolves the nested stats
 *       (bt_weapon_stats_onhit), the firing slice carries them in
 *       BtWProjArgs.onhit_* + a.child, and Projectiles spawns them with
 *       bt_wproj_spawn_onhit on the hit (unit.gd:586-604).  A hitbox-local
 *       spawn was the wrong pool and the wrong RNG (it skipped the source's
 *       unconditional rand_range(-PI, PI)).
 *     pierce_on_crit/bounce_on_crit -- the charges ride each shot
 *       (BtWProjArgs.crit_pierce/crit_bounce) and are spent by Projectiles on
 *       the crit (player_projectile.gd:130-150).
 *     slow_in_zone -- SlowInZoneEffect is a NullEffect (effects/weapons/
 *       slow_in_zone_effect.gd; get_args returns []), so the marker row has no
 *       runtime behaviour; the real -200/-400 slow is the projectile scene's
 *       SlowHitbox, resolved from BtWeaponProjSpec.slow_rad/slow_amt and applied
 *       by bt_wproj_slow through the shared bt_add_decaying_speed
 *       (brotato_physics_math.h -- single formula).
 *     gold_on_cursed_enemy_kill -- paid by the CURSE runtime (an ENEMY effect
 *       behavior on the lethal hit), not by a weapon kill callback:
 *       bt_enemy_curse_player_kill_gold() in brotato_enemy_curse.h credits it
 *       straight to the purse (run_data.gd:810-818 add_gold: no coin, no XP).
 *   The player-scoped weapon-STAT modifier buckets and init_base_stats' ordered
 *   chain (weapon_type_bonus, weapon_class_bonus, weapon_scaling_stats,
 *   slow_on_hit, WeaponStack/WeaponGainStatForEveryStat, minimum/maximum weapon
 *   cooldowns, attack-speed conversion) are WeaponStats'
 *   (brotato_weapon_stats.h); this header does not keep a second copy.
 *   Shop-owned key families (weapons_price, specific_items_price, upgraded_baits,
 *   curse_locked_items, min/max weapon tier gating) belong to the shop slice.
 *
 * RNG-ORDER NOTE: every helper draws from the SAME PCG32 stream the source's
 * global randf()/randi() used (bt_chance -> bt_math_randf(&s->rng);
 * bt_rand_range -> s->rng), in the source call order.
 */
#pragma once

#include "brotato_weapon_effect_types.h"

/* Max top-level effect curse modifiers a weapon copy carries (WeaponInventory
 * draws one per effect; the canonical tables max at 2, so the BT_DEVICE
 * row_curse arrays the hit/kill hooks index never exceed this). Guarded so the
 * header stays self-contained whichever include order reaches it first; the
 * owned definition lives with the storage (brotato_weapon_inventory.h). */
#ifndef BT_WEAPON_FX_CURSE_MAX
#define BT_WEAPON_FX_CURSE_MAX 2
#endif

/* Forward declarations of helpers defined later in the TU (exactly like
 * bt_scalar_events.h forward-declares bt_enemy_incoming_damage). */
BT_HD static float bt_scalar_heal(BT_DEVICE BtState* s, float value);
BT_HD static int bt_item_expl_spawn(BT_DEVICE BtState* s, unsigned short effect_row,
                                    float x, float y, float dmg,
                                    float crit_chance, float crit_dmg,
                                    float burn_chance, float burn_dmg, float burn_dur);
BT_HD static int bt_item_expl_on_key(BT_DEVICE BtState* s, unsigned short key, float x, float y);

/* ---------------------------------------------------------------------------
 * A weapon's OWN effect rows as canonical BT_ITEM_EFF_ROWS indices.
 * BT_WEAPONS[id].fx_first/fx_count index BT_WEAPON_EFF_INDEX, whose entries are
 * the canonical rows WeaponData appended (the same view the hit paths pass as
 * `rows` to every helper here).  Indices only: a table of `const BtItemEff*`
 * would need the thread/constant address-space hazard the core avoids.
 * Out-of-range spans are dropped.  Returns the count written; 0 when the id is
 * not a weapon.  Defined here (before Melee/Ranged) so BOTH slices share ONE
 * extractor -- brotato_state.h already pulled in BT_WEAPONS.
 * ------------------------------------------------------------------------- */
BT_HD static int bt_wfx_fx_rows(unsigned short id, BT_THREAD unsigned short* rows, int max) {
    if (id >= BT_NWEAPONS || rows == 0 || max <= 0) return 0;
    BT_CONST_PTR(BtWeaponDef) D = &BT_WEAPONS[id];
    int n = 0;
    for (int k = 0; k < (int)D->fx_count && n < max; k++) {
        int row = BT_WEAPON_EFF_INDEX[(int)D->fx_first + k];
        if (row < 0 || row >= BT_NITEM_EFF_ROWS) continue;
        rows[n] = (unsigned short)row;
        n++;
    }
    return n;
}

/* ---------------------------------------------------------------------------
 * Curse modifiers for a weapon copy's OWN effect rows.
 * WeaponInventory owns the draw and stores one modifier per top-level effect
 * (bt_weapon_fx_curse(s, slot, index)); this header only applies the source's
 * dlc_1_data.gd boost formulas to the row value it is about to use, so it never
 * draws and never desyncs the utils_rng stream.
 *   _boost_effect_value_positively(good) = sign(v)*ceil(|v|*(1+mod))
 *   _boost_effect_value_positively(bad)  = sign(v)*max(1, floor(|v|/(1+mod)))
 *   ExplodingEffect (WeaponData, chance<1) = min(1, chance*(1+mod/5))
 *
 *   `row_curse` provenance: a BT_DEVICE array, index-parallel to `rows`, holding
 *   each top-level effect's stored modifier. It is passed as the SOURCE pointer,
 *   never copied: Melee hands over &s->wpn_fxcurse[slot][0] of the firing copy and
 *   Projectiles the shot's own p->src_fxcurse snapshot (so a recycled slot cannot
 *   change a live shot). Both sources are the copy's own stored draws, zeroed for
 *   an uncursed copy -- no accessor indirection is needed at the call site. The
 *   shipped tables never index past BT_WEAPON_FX_CURSE_MAX (the widest weapon has
 *   fx_count == BT_WEAPON_FX_CURSE_MAX == 2), and the reads are bounded by it.
 *   Pass 0 only if a caller genuinely has no array. `curse_mod` on the single-row
 *   triggers is the same value passed by scalar.
 * ------------------------------------------------------------------------- */
BT_HD static float bt_wfx_curse_boost_good(float value, float mod) {
    if (mod == 0.0f) return value;
    float s = value < 0.0f ? -1.0f : 1.0f;
    return s * bt_ceilf(bt_fabsf(value) * (1.0f + mod));
}
BT_HD static float bt_wfx_curse_boost_bad(float value, float mod) {
    if (mod == 0.0f) return value;
    float s = value < 0.0f ? -1.0f : 1.0f;
    float v = bt_floorf(bt_fabsf(value) / (1.0f + mod));
    if (v < 1.0f) v = 1.0f;
    return s * v;
}
BT_HD static float bt_wfx_curse_explode_chance(float chance, float mod) {
    if (mod == 0.0f || chance >= 1.0f) return chance;
    float c = chance * (1.0f + mod / 5.0f);
    return c > 1.0f ? 1.0f : c;
}

/* Bounds-safe read of a scalar effect key (never an out-of-range constant index). */
BT_HD static float bt_wfx_scalar_keyed(BT_DEVICE BtState* s, int key) {
    return (key > 0 && key < BT_NITEM_STRS) ? s->scalar_effect[key] : 0.0f;
}

/* ---------------------------------------------------------------------------
 * state lifecycle. `wfx` is BtState.wfx (brotato_state.h); the BtState wrappers
 * below exist so WeaponInventory can drive compaction without knowing the field
 * name. BT_WEAPON_FX_HOOKS marks the entry points the inventory dispatch calls:
 *   slot move / slot erase  -- tail-slot compaction (a MOVED block must not be
 *                              cleared: the hooks copy the whole block down and
 *                              only clear the vacated slot);
 *   forget_uid              -- an ACTUAL removal / combine of one copy; clears
 *                              that uid's history and nothing else.
 * ------------------------------------------------------------------------- */
#define BT_WEAPON_FX_HOOKS 1

/* bt_weapon_slot_from_uid lives in brotato_weapon_inventory.h, defined later in
 * the TU; forward-declared so a uid whose copy is gone can be recycled even if
 * the caller missed a forget_uid. */
BT_HD static int bt_weapon_slot_from_uid(BT_DEVICE BtState* s, unsigned int uid);

BT_HD static void bt_wfx_log_clear(BT_DEVICE BtWeaponAttackLog* g) {
    g->uid = BT_WFX_UID_FREE;
    for (int i = 0; i < BT_WFX_ATTACK_HISTORY; i++) {
        g->hit.attack[i] = 0;  g->hit.n[i] = 0;  g->hit.used[i] = 0;
        g->kill.attack[i] = 0; g->kill.n[i] = 0; g->kill.used[i] = 0;
    }
    g->hit.oldest = 0;
    g->kill.oldest = 0;
}

BT_HD static void bt_wfx_reset(BT_DEVICE BtWeaponFxState* fx) {
    for (int i = 0; i < BT_WFX_SLOTS; i++) {
        bt_wfx_log_clear(&fx->log[i]);
        fx->wave_kills[i] = 0;
    }
    fx->overflow = 0;
}
/* A fresh main.tscn per wave rebuilds every Weapon node, so each weapon's
 * 20-attack window is dropped with it and the wave kill counter restarts. */
BT_HD static void bt_wfx_reset_wave(BT_DEVICE BtWeaponFxState* fx) {
    for (int i = 0; i < BT_WFX_SLOTS; i++) {
        for (int k = 0; k < BT_WFX_ATTACK_HISTORY; k++) {
            fx->log[i].hit.used[k] = 0; fx->log[i].kill.used[k] = 0;
        }
        fx->log[i].hit.oldest = 0; fx->log[i].kill.oldest = 0;
        fx->wave_kills[i] = 0;
    }
}
/* Tail-slot compaction: the block (uid + both windows) MOVES with its copy. */
BT_HD static void bt_wfx_slot_move(BT_DEVICE BtWeaponFxState* fx, int from, int to) {
    if (from < 0 || from >= BT_WFX_SLOTS || to < 0 || to >= BT_WFX_SLOTS || from == to) return;
    fx->log[to] = fx->log[from];
    fx->wave_kills[to] = fx->wave_kills[from];
}
/* Erase the VACATED slot (last slot after a smear, or every slot at run init). */
BT_HD static void bt_wfx_slot_erase(BT_DEVICE BtWeaponFxState* fx, int slot) {
    if (slot < 0 || slot >= BT_WFX_SLOTS) return;
    bt_wfx_log_clear(&fx->log[slot]);
    fx->wave_kills[slot] = 0;
}
/* A copy is actually GONE (removed / combined / replaced): drop its history by
 * uid. Never call this for a tail-slot move -- that would clear a uid that was
 * just relocated. */
BT_HD static void bt_wfx_forget_uid(BT_DEVICE BtWeaponFxState* fx, unsigned int uid) {
    if (uid == BT_WFX_UID_FREE) return;
    for (int i = 0; i < BT_WFX_SLOTS; i++)
        if (fx->log[i].uid == uid) { bt_wfx_log_clear(&fx->log[i]); return; }
}

/* Resolve (or claim) `uid`'s history. First recycles any block whose copy no
 * longer exists, then matches by uid, then claims a free block (the caller's
 * slot hint first). Returns 0 -- and sets fx->overflow, never silently evicting
 * another owner -- when no block is available. */
BT_HD static BT_DEVICE BtWeaponAttackLog* bt_wfx_log_claim(BT_DEVICE BtState* s,
                                                           BT_DEVICE BtWeaponFxState* fx,
                                                           unsigned int uid, int slot_hint) {
    if (uid != BT_WFX_UID_FREE && s != 0) {
        for (int i = 0; i < BT_WFX_SLOTS; i++) {
            unsigned int u = fx->log[i].uid;
            if (u == BT_WFX_UID_FREE || u == uid) continue;
            if (bt_weapon_slot_from_uid(s, u) < 0) bt_wfx_log_clear(&fx->log[i]);
        }
    }
    for (int i = 0; i < BT_WFX_SLOTS; i++) if (fx->log[i].uid == uid) return &fx->log[i];
    if (slot_hint >= 0 && slot_hint < BT_WFX_SLOTS && fx->log[slot_hint].uid == BT_WFX_UID_FREE) {
        fx->log[slot_hint].uid = uid;
        return &fx->log[slot_hint];
    }
    for (int i = 0; i < BT_WFX_SLOTS; i++)
        if (fx->log[i].uid == BT_WFX_UID_FREE) { fx->log[i].uid = uid; return &fx->log[i]; }
    fx->overflow = 1;
    return 0;
}
BT_HD static BT_DEVICE BtWeaponAttackLog* bt_wfx_log_find(BT_DEVICE BtWeaponFxState* fx,
                                                         unsigned int uid) {
    for (int i = 0; i < BT_WFX_SLOTS; i++) if (fx->log[i].uid == uid) return &fx->log[i];
    return 0;
}

/* BtState-based forwards (WeaponInventory dispatch; bt_weapon_reset() calls the
 * erase for every slot so wave_kills[] is zeroed at run init). */
BT_HD static void bt_weapon_fx_slot_move(BT_DEVICE BtState* s, int from, int to) {
    bt_wfx_slot_move(&s->wfx, from, to);
}
BT_HD static void bt_weapon_fx_slot_erase(BT_DEVICE BtState* s, int slot) {
    bt_wfx_slot_erase(&s->wfx, slot);
}
BT_HD static void bt_weapon_fx_forget_uid(BT_DEVICE BtState* s, unsigned int uid) {
    bt_wfx_forget_uid(&s->wfx, uid);
}

/* ---------------------------------------------------------------------------
 * Damage type (weapon_service.gd:109-112, unit.gd:477-480).
 *
 * ITEM-SCALING ONLY. `first/count` index BT_ITEM_SCALING -- the table that the
 * ITEM effect graph uses (a nested ProjectilesOnHitEffect's `weapon_stats`, an
 * AlienEyes-style item projectile, a burning row's scaling_stats).
 *
 * A PLAYER WEAPON's base stats row indexes BT_WEAPON_SCALING, a DIFFERENT table,
 * so its indices must never be passed here. WeaponStats resolves the elemental
 * flag from the weapon's EFFECTIVE scaling rows at the source's own assignment
 * point (weapon_service.gd:109-112) and exposes it as
 * `BtPlayerWeaponStats.is_elemental`; Melee/Ranged pass that resolved flag into
 * bt_wfx_hit_bonus_pct / BtEnemyHitContext.is_elemental and the projectiles.
 * ------------------------------------------------------------------------- */
BT_HD static int bt_wfx_item_scaling_is_elemental(unsigned short scaling_first,
                                                  unsigned short scaling_count) {
    for (int i = 0; i < (int)scaling_count; i++) {
        int idx = (int)scaling_first + i;
        if (idx >= BT_NITEM_SCALING) break;
        if (bt_core_id_from_istr(BT_ITEM_SCALING[idx].stat) == 8) return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * unit.gd:458-500 get_damage_value bonus percent -- the whole source total
 * (weapon rows + both item buckets + non-elemental-vs-burning), exactly the
 * value bt_item_expl_hit_enemy reads through BtEnemyHitContext.rows.
 * ------------------------------------------------------------------------- */
BT_HD static float bt_wfx_hit_bonus_pct(BT_DEVICE BtState* s, int enemy_slot,
                                        BT_CONST_PTR(unsigned short) rows, int n_rows,
                                        int is_elemental, int is_burning) {
    float hp = s->ehp[enemy_slot], maxhp = s->emax[enemy_slot];
    float total = 0.0f;

    for (int i = 0; i < n_rows; i++) {
        int ri = (int)rows[i];
        if (ri < 0 || ri >= BT_NITEM_EFF_ROWS) continue;
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[ri];
        if (bt_eff_is(E, BT_ISTR_BONUS_DAMAGE_AGAINST_TARGETS_ABOVE_HP)) {
            if (hp >= maxhp * ((float)E->value2 * 0.01f)) total += (float)E->value;
        } else if (bt_eff_is(E, BT_ISTR_BONUS_DAMAGE_AGAINST_TARGETS_BELOW_HP)) {
            if (hp <= maxhp * ((float)E->value2 * 0.01f)) total += (float)E->value;
        }
    }

    if (is_burning) {
        float bn = bt_wfx_scalar_keyed(s, BT_ISTR_BONUS_NON_ELEMENTAL_DAMAGE_AGAINST_BURNING_TARGETS);
        if (bn > 0.0f && !is_elemental) total += bn;
    }

    for (int a = 0; a < s->n_bk_active; a++) {
        int r = (int)s->bk_active[a];
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[r];
        int above = bt_eff_is(E, BT_ISTR_BONUS_DAMAGE_AGAINST_TARGETS_ABOVE_HP);
        int below = bt_eff_is(E, BT_ISTR_BONUS_DAMAGE_AGAINST_TARGETS_BELOW_HP);
        if (!above && !below) continue;
        for (int c = 0, n = bt_row_owned(s, r); c < n; c++) {
            int bv = 0, bv2 = 0;
            if (!bt_row_get(s, r, E, c, &bv, &bv2)) break;
            float thr = maxhp * ((float)bv2 * 0.01f);
            if (above && hp >= thr) total += (float)bv;
            else if (below && hp <= thr) total += (float)bv;
        }
    }
    return total;
}

/* enemy.gd:266-272 bonus_current_health_damage: post-armor flat damage
 * int(current_health * value/factor / max(1, endless_factor)); factor is 100 for
 * an ordinary unit and 1000 for a boss. */
BT_HD static float bt_wfx_current_health_damage(BT_DEVICE BtState* s, int enemy_slot,
                                                BT_CONST_PTR(unsigned short) rows, int n_rows,
                                                float factor, float endless_factor) {
    if (factor <= 0.0f) return 0.0f;
    float bonus = 0.0f;
    for (int i = 0; i < n_rows; i++) {
        int ri = (int)rows[i];
        if (ri < 0 || ri >= BT_NITEM_EFF_ROWS) continue;
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[ri];
        if (!bt_eff_is(E, BT_ISTR_BONUS_CURRENT_HEALTH_DAMAGE)) continue;
        bonus += (s->ehp[enemy_slot] * ((float)E->value / factor)) / bt_maxf(1.0f, endless_factor);
    }
    return (float)(int)bonus;
}

/* ---------------------------------------------------------------------------
 * run_data.gd:1297-1302 + player.gd:724-728 on_lifesteal_effect.
 *   roll randf() < lifesteal -> value = 1 (2 with stat_double_lifesteal_bonus)
 *   then, ONLY when the LifestealTimer is stopped, start it and heal `value`.
 *   A lifesteal inside the 0.1 s window is DROPPED (no heal, no queue).
 * `cd_seconds` is BtState.player_lifesteal_cd: Projectiles sets it to
 * BT_WFX_LIFESTEAL_SECONDS and advances it by BT_DT once per step (in SECONDS).
 * Returns the HP restored.
 * ------------------------------------------------------------------------- */
BT_HD static float bt_wfx_lifesteal(BT_DEVICE BtState* s, BT_DEVICE float* cd_seconds, float lifesteal) {
    /* run_data.gd:1297-1302 manage_life_steal: `if randf() < weapon_stats.lifesteal`
     * evaluates randf() on EVERY call -- the lifesteal value only decides the
     * comparison, and the 0.1 s LifestealTimer gate lives in the LISTENER
     * (player.gd:724-728 on_lifesteal_effect: `if _lifesteal_timer.is_stopped()`),
     * i.e. AFTER the draw.  A lifesteal roll inside the window therefore still
     * consumes its global draw and heals nothing.  Both weapon hit sites call
     * this unconditionally per hit (weapon.gd:412 melee, player_projectile.gd:105
     * ranged), so the draw MUST NOT be short-circuited by lifesteal == 0 or by
     * an active timer. */
    float roll = bt_math_randf(&s->rng);
    if (roll >= lifesteal) return 0.0f;
    if (cd_seconds != 0 && *cd_seconds > 0.0f) return 0.0f;
    float value = 1.0f;
    if (bt_wfx_scalar_keyed(s, BT_ISTR_STAT_DOUBLE_LIFESTEAL_BONUS) > 0.0f) value = 2.0f;
    if (cd_seconds != 0) *cd_seconds = BT_WFX_LIFESTEAL_SECONDS;
    return bt_scalar_heal(s, value);
}

/* ---------------------------------------------------------------------------
 * unit.gd:282-300 crit helpers.
 * ------------------------------------------------------------------------- */
BT_HD static int bt_wfx_force_crit_when_burning(BT_CONST_PTR(unsigned short) rows, int n_rows,
                                                int is_burning) {
    if (!is_burning) return 0;
    for (int i = 0; i < n_rows; i++) {
        int ri = (int)rows[i];
        if (ri < 0 || ri >= BT_NITEM_EFF_ROWS) continue;
        if (bt_eff_is(&BT_ITEM_EFF_ROWS[ri], BT_ISTR_CRIT_ON_HITTING_BURNING_TARGET)) return 1;
    }
    return 0;
}

/* OneShotOnHitEffect: chance = value/100; success means the hit deals at least
 * the target's current health (unit.gd:292-296). Each row is rolled and
 * OVERWRITES the flag, so the last such row on the hitbox decides. Returns 1
 * when the hit is a one-shot. */
BT_HD static int bt_wfx_one_shot(BT_DEVICE BtState* s, BT_CONST_PTR(unsigned short) rows, int n_rows) {
    int one = 0;
    for (int i = 0; i < n_rows; i++) {
        int ri = (int)rows[i];
        if (ri < 0 || ri >= BT_NITEM_EFF_ROWS) continue;
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[ri];
        if (E->opcode != BT_ITEMEFF_ONE_SHOT_ON_HIT) continue;
        one = bt_chance(s, (float)E->value * 0.01f) ? 1 : 0;
    }
    return one;
}

/* ---------------------------------------------------------------------------
 * unit.gd:348-378 kill-side consumers (also enemy.gd:236-243).
 * ------------------------------------------------------------------------- */

/* crit kill: gold_on_crit_kill (item bucket [key, chance%] and weapon rows with
 * value = chance%) + heal_on_crit_kill. Returns the gold added. */
BT_HD static int bt_wfx_crit_kill_gold(BT_DEVICE BtState* s, BT_CONST_PTR(unsigned short) rows,
                                       int n_rows) {
    int gold = 0, item_chance = 0;
    /* Effect.KEY_VALUE merges copies by key before Unit rolls once. */
    for (int a = 0; a < s->n_bk_active; a++) {
        int r = (int)s->bk_active[a];
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[r];
        if (bt_eff_is(E, BT_ISTR_GOLD_ON_CRIT_KILL))
            item_chance += (int)bt_row_sum_v(s, r, E);
    }
    if (bt_chance(s, (float)item_chance * 0.01f)) gold++;
    for (int i = 0; i < n_rows; i++) {
        int ri = (int)rows[i];
        if (ri < 0 || ri >= BT_NITEM_EFF_ROWS) continue;
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[ri];
        if (!bt_eff_is(E, BT_ISTR_GOLD_ON_CRIT_KILL)) continue;
        if (bt_math_randf(&s->rng) <= (float)E->value * 0.01f) gold += 1;
    }
    if (bt_chance(s, bt_wfx_scalar_keyed(s, BT_ISTR_HEAL_ON_CRIT_KILL) * 0.01f))
        (void)bt_scalar_heal(s, 1.0f);
    return gold;
}

/* NOTE: gold_on_cursed_enemy_kill is NOT consumed here.  Its source home is
 * curse_enemy_effect_behavior.gd:24-41 (an ENEMY effect behavior triggered on
 * the lethal hit), i.e. the curse runtime -- bt_enemy_curse_player_kill_gold()
 * in brotato_enemy_curse.h credits it straight to the purse.  Keeping a second
 * copy here would double-pay the moment the caller passes a real cursed flag.
 */

/* unit.gd:363 heal_on_kill + enemy.gd:236-243 explode_on_overkill.
 * Called once per credited player kill; adds the gold to *gold_added. */
BT_HD static void bt_wfx_on_player_kill(BT_DEVICE BtState* s, int enemy_slot, int was_crit,
                                        float killing_blow,
                                        BT_CONST_PTR(unsigned short) rows, int n_rows,
                                        BT_THREAD int* gold_added) {
    if (bt_chance(s, bt_wfx_scalar_keyed(s, BT_ISTR_HEAL_ON_KILL) * 0.01f))
        (void)bt_scalar_heal(s, 1.0f);

    if (was_crit) *gold_added += bt_wfx_crit_kill_gold(s, rows, n_rows);

    /* enemy.gd:238-242: the FIRST explode_on_overkill row whose value% of the
     * dying enemy's max hp the killing blow met fires one explosion. */
    for (int a = 0; a < s->n_bk_active; a++) {
        int r = (int)s->bk_active[a];
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[r];
        if (!bt_eff_is(E, BT_ISTR_EXPLODE_ON_OVERKILL)) continue;
        for (int c = 0, n = bt_row_owned(s, r); c < n; c++) {
            int bv = 0, bv2 = 0;
            if (!bt_row_get(s, r, E, c, &bv, &bv2)) break;
            if (killing_blow >= s->emax[enemy_slot] * ((float)bv * 0.01f)) {
                (void)bt_item_expl_on_key(s, BT_ISTR_EXPLODE_ON_OVERKILL,
                                          s->ex[enemy_slot], s->ey[enemy_slot]);
                return;
            }
        }
    }
}

/* ---------------------------------------------------------------------------
 * Burning-enemy consumers (linked_stats.gd:41, unit.gd:717-727).
 *
 * `RunData.current_burning_enemies` is the number of units whose `_is_burning`
 * is true, which in this core is exactly `ealive[j] && eburn[j] > 0` (eburn is
 * set by bt_apply_enemy_burn and cleared by stop_burning). It is the source term
 * of the ONE reachable burning-enemy link, item_fried_rice
 * (gain_stat_for_every_stat_effect: key = stat_hp_regeneration, stat_scaled =
 * "burning_enemy", value 1, nb_stat_scaled 1 -> +1 regen per burning enemy,
 * BT_ITEM_STAT_LINKS source_key == BT_ISTR_BURNING_ENEMY with a BT_STAT_NONE
 * source). Computed on demand: a scan of BT_MAX slots, no state field.
 * ------------------------------------------------------------------------- */
BT_HD static int bt_wfx_burning_enemies(BT_DEVICE BtState* s) {
    int n = 0;
    for (int j = bt_enemy_next(s, 0, 1); j < (int)s->e_used; j = bt_enemy_next(s, j + 1, 1)) if (s->ealive[j] && s->eburn[j] > 0.0f) n++;
    return n;
}

/* The link contribution of one owned GainStatForEveryStat row whose scaled stat
 * is the burning-enemy count (E->aux_key == BT_ISTR_BURNING_ENEMY), or 0 when the
 * row is not that link. bt_recompute_links calls this for the rows whose source
 * would otherwise be skipped as BT_STAT_NONE:
 *     float srcv = bt_core_stat_read(s, src);
 *     if (src == 255 && E->aux_key == BT_ISTR_BURNING_ENEMY) {
 *         s->link_stat[t] += bt_wfx_burning_enemy_link_bonus(s, r) * (float)s->lk_count[r];
 *         continue;
 *     }
 * (linked_stats.gd:37-42 uses current_burning_enemies as actual_nb_scaled in
 * place of the source stat's value.) */
BT_HD static float bt_wfx_burning_enemy_link_bonus(BT_DEVICE BtState* s, unsigned short row) {
    if (row >= BT_NITEM_EFF_ROWS) return 0.0f;
    BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[row];
    if (E->aux_key != BT_ISTR_BURNING_ENEMY || E->nb_stat_scaled == 0) return 0.0f;
    return bt_stat_link_gain((float)E->value, (float)bt_wfx_burning_enemies(s),
                             (float)E->nb_stat_scaled);
}

/* ---------------------------------------------------------------------------
 * charm_on_hit (dlcs/dlc_1 charm_enemy_effect_behavior.gd:36-70).
 *
 * The charm behavior is attached to an enemy while ANY player weapon carries a
 * charm row; on being hurt it accumulates a chance and rolls once:
 *   item bucket rows  (custom_key == charm_on_hit; the row's key is the SCALING
 *     stat): when hp/max < value2/100 -> += max(1, (value/100) * stat(key)) / 100
 *   weapon rows (rows): when hp/max < value2/100 -> += value / 100
 *   chance /= max(1, endless_factor / 2)
 *   if current_charmed_enemies[player] >= max_charm_count -> refuse
 * Returns 1 when the enemy must be charmed; the CALLER then drives the enemy-side
 * charm (EnemyMotionIntegration: charm timer, damage +200%, target switch, the
 * +1 current_charmed_enemies count).  The shipped
 * charm_enemy_effect_behavior.tscn sets max_charm_count = 999.
 *
 * REACHABILITY (shipped + selectable): the only charm producer is the RANGED
 * flute's weapon rows (NullCharmEffect rows 885-888, custom_key charm_on_hit),
 * read through the `rows` view.  No ITEM carries charm_on_hit (lucky_charm is
 * stat_luck), so the item-bucket loop is a verified no-op for the Bull/Knight/
 * Demon selector; the sole CharmEffect (romantic character, key stat_max_hp) is
 * a DLC character outside that selector.  The roll is a strict no-op with zero
 * RNG when nothing carries the row (bt_chance(0) draws nothing), so any hitbox
 * may call it unconditionally.
 * ------------------------------------------------------------------------- */
BT_HD static int bt_wfx_charm_roll(BT_DEVICE BtState* s, int enemy_slot, int charms_active,
                                   int max_charm, BT_CONST_PTR(unsigned short) rows, int n_rows,
                                   float endless_factor) {
    if (charms_active >= max_charm) return 0;
    float hp = s->ehp[enemy_slot], maxhp = s->emax[enemy_slot];
    if (maxhp <= 0.0f) return 0;
    float frac = hp / maxhp;
    float chance = 0.0f;

    for (int a = 0; a < s->n_bk_active; a++) {
        int r = (int)s->bk_active[a];
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[r];
        if (!bt_eff_is(E, BT_ISTR_CHARM_ON_HIT)) continue;
        int stat_id = bt_core_id_from_istr(E->key);
        for (int c = 0, n = bt_row_owned(s, r); c < n; c++) {
            int bv = 0, bv2 = 0;
            if (!bt_row_get(s, r, E, c, &bv, &bv2)) break;
            if (frac >= (float)bv2 * 0.01f) continue;
            float stat = stat_id != 255 ? bt_core_stat_read(s, stat_id) : 0.0f;
            float per = (float)bv * 0.01f * stat;
            if (per < 1.0f) per = 1.0f;
            chance += per * 0.01f;
        }
    }
    for (int i = 0; i < n_rows; i++) {
        int ri = (int)rows[i];
        if (ri < 0 || ri >= BT_NITEM_EFF_ROWS) continue;
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[ri];
        if (!bt_eff_is(E, BT_ISTR_CHARM_ON_HIT)) continue;
        if (frac >= (float)E->value2 * 0.01f) continue;
        chance += (float)E->value * 0.01f;
    }
    float divisor = endless_factor * 0.5f;
    if (divisor < 1.0f) divisor = 1.0f;
    chance /= divisor;
    return bt_chance(s, chance) ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * weapon.gd:203-230 / 440-465: the per-WEAPON attack tallies. Each copy owns its
 * own 20-entry window (MAX_ATTACK_COUNT_HISTORY is per Weapon node), so two
 * weapons never evict each other's ids; the uid selects the owning window.
 * ------------------------------------------------------------------------- */
BT_HD static int bt_wfx_tally(BT_DEVICE BtWeaponAttackTally* t, int attack_id) {
    /* weapon.gd:453-457 erases every id < `attack_id - MAX_ATTACK_COUNT_HISTORY
     * + 1` BEFORE the new id is ever read back. The source keeps a Dictionary
     * (21 entries momentarily, then prunes to <= 20); this 20-slot ring must
     * therefore prune FIRST so an id scrolling out of a full window frees its
     * place for the incoming id. Claiming a slot first would return early on a
     * full window and leave `oldest` frozen -- the oldest id would never be
     * evicted. */
    int remove_until = attack_id - BT_WFX_ATTACK_HISTORY + 1;
    if (remove_until > t->oldest) {
        for (int i = 0; i < BT_WFX_ATTACK_HISTORY; i++)
            if (t->used[i] && t->attack[i] < remove_until) t->used[i] = 0;
        t->oldest = remove_until;
    }
    int slot = -1;
    for (int i = 0; i < BT_WFX_ATTACK_HISTORY; i++)
        if (t->used[i] && t->attack[i] == attack_id) { slot = i; break; }
    if (slot < 0) {
        for (int i = 0; i < BT_WFX_ATTACK_HISTORY; i++) if (!t->used[i]) { slot = i; break; }
        if (slot < 0) return 0;   /* unreachable after the prune above; guard only */
        t->used[slot] = 1; t->attack[slot] = attack_id; t->n[slot] = 0;
    }
    if (t->n[slot] < 65535) t->n[slot]++;
    return (int)t->n[slot];
}
BT_HD static int bt_wfx_tally_lookup(BT_DEVICE BtWeaponAttackTally* t, int attack_id) {
    for (int i = 0; i < BT_WFX_ATTACK_HISTORY; i++)
        if (t->used[i] && t->attack[i] == attack_id) return (int)t->n[i];
    return 0;
}
BT_HD static int bt_wfx_attack_hits(BT_DEVICE BtWeaponFxState* fx, unsigned int uid, int attack_id) {
    BT_DEVICE BtWeaponAttackLog* g = bt_wfx_log_find(fx, uid);
    return g != 0 ? bt_wfx_tally_lookup(&g->hit, attack_id) : 0;
}
BT_HD static int bt_wfx_attack_kills(BT_DEVICE BtWeaponFxState* fx, unsigned int uid, int attack_id) {
    BT_DEVICE BtWeaponAttackLog* g = bt_wfx_log_find(fx, uid);
    return g != 0 ? bt_wfx_tally_lookup(&g->kill, attack_id) : 0;
}

/* weapon.gd:440-465: per-attack HIT tally + break_on_hit. Returns the gold to
 * drop when the weapon BREAKS (0 = survives); the caller (Inventory/Ranged)
 * removes the weapon, since slot ownership is Inventory's. `slot_hint` is the
 * firing copy's current slot, used only to claim its history block. */
BT_HD static int bt_wfx_on_weapon_hit(BT_DEVICE BtState* s, BT_DEVICE BtWeaponFxState* fx,
                                      int slot_hint, unsigned int uid, int attack_id,
                                      BT_CONST_PTR(unsigned short) rows, int n_rows,
                                      BT_DEVICE const float* row_curse) {
    if (attack_id >= 0) {
        BT_DEVICE BtWeaponAttackLog* g = bt_wfx_log_claim(s, fx, uid, slot_hint);
        if (g != 0) (void)bt_wfx_tally(&g->hit, attack_id);
    }
    int gold_out = 0;
    for (int i = 0; i < n_rows; i++) {
        int ri = (int)rows[i];
        if (ri < 0 || ri >= BT_NITEM_EFF_ROWS) continue;
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[ri];
        if (!bt_eff_is(E, BT_ISTR_BREAK_ON_HIT)) continue;
        float gold = (float)E->value2;
        if (row_curse != 0 && i < BT_WEAPON_FX_CURSE_MAX)
            gold = bt_wfx_curse_boost_good(gold, row_curse[i]);
        /* weapon.gd:440-465 has NO `break`: EVERY break_on_hit row rolls its own
         * chance (each is a global draw) and emits its own wanted_to_break.  The
         * gold below is dropped by player.gd:393-408 on the first success only
         * because the weapon is already gone by then, so the returned gold is the
         * FIRST successful row's and the later rows still draw. */
        if (bt_chance(s, (float)E->value * 0.01f) && gold_out == 0) gold_out = (int)gold;
    }
    return gold_out;
}

/* weapon.gd:203-230 on_killed_something:
 *   gain_stat_when_attack_killed_enemies bucket rows [stat_key, add, count]: when
 *     THIS attack's kill count reaches `count`, add `add` to the stat;
 *   GainStatEveryKilledEnemiesEffect (a weapon row): every `value` kills this wave
 *     with this weapon, add `nb_stat_scaled` to the stat named by `aux_key`.
 *
 *   BUCKET PRODUCER: the player-scoped bucket above is filled by bt_apply_item
 *   from KEY_VALUE rows whose custom_key is gain_stat_when_attack_killed_enemies.
 *   Its ONLY producer in the recovered sources is the DLC dwarf character effect
 *   (brotato_abyss dlcs/dlc_1/characters/dwarf/effects/dwarf_effect_0.tres: key
 *   stat_engineering, value 1, value2 6 -> +1 engineering per 6th attack kill).
 *   The base game only DECLARES the bucket (singletons/player_run_data.gd:586) --
 *   no shipped item or weapon fills it -- and the Bull/Knight/Demon selector has
 *   no dwarf, so the canonical generated graph carries no row with that
 *   custom_key and the branch is a strict no-op (zero RNG, zero stat) there. */
BT_HD static void bt_wfx_on_weapon_kill(BT_DEVICE BtState* s, BT_DEVICE BtWeaponFxState* fx,
                                        int weapon_slot, unsigned int uid, int attack_id,
                                        BT_CONST_PTR(unsigned short) rows, int n_rows,
                                        BT_DEVICE const float* row_curse) {
    int count = 0;
    if (weapon_slot >= 0 && weapon_slot < BT_WFX_SLOTS) {
        if (fx->wave_kills[weapon_slot] < 65535) fx->wave_kills[weapon_slot]++;
        count = (int)fx->wave_kills[weapon_slot];
    }
    if (attack_id >= 0) {
        BT_DEVICE BtWeaponAttackLog* g = bt_wfx_log_claim(s, fx, uid, weapon_slot);
        int kills = g != 0 ? bt_wfx_tally(&g->kill, attack_id) : 0;
        for (int a = 0; a < s->n_bk_active; a++) {
            int r = (int)s->bk_active[a];
            BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[r];
            if (!bt_eff_is(E, BT_ISTR_GAIN_STAT_WHEN_ATTACK_KILLED_ENEMIES)) continue;
            int stat_id = bt_core_id_from_istr(E->key);
            if (stat_id == 255) continue;
            for (int c = 0, n = bt_row_owned(s, r); c < n; c++) {
                int bv = 0, bv2 = 0;
                if (!bt_row_get(s, r, E, c, &bv, &bv2)) break;
                if (bv2 > 0 && kills == bv2) bt_core_stat_add(s, stat_id, (float)bv);
            }
        }
    }
    for (int i = 0; i < n_rows; i++) {
        int ri = (int)rows[i];
        if (ri < 0 || ri >= BT_NITEM_EFF_ROWS) continue;
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[ri];
        if (E->opcode != BT_ITEMEFF_GAIN_STAT_EVERY_KILLED_ENEMIES) continue;
        int stat_id = bt_core_id_from_istr(E->aux_key);
        if (stat_id == 255 || E->value <= 0) continue;
        int every = E->value;
        if (row_curse != 0 && i < BT_WEAPON_FX_CURSE_MAX)
            every = (int)bt_wfx_curse_boost_bad((float)E->value, row_curse[i]);
        if (every <= 0) continue;
        if (count % every == 0) bt_core_stat_add(s, stat_id, (float)E->nb_stat_scaled);
    }
}

/* ---------------------------------------------------------------------------
 * Weapon-stat modifiers consumed by WeaponStats' bt_weapon_stats().
 * ------------------------------------------------------------------------- */

/* NOTE: weapon_service.gd:270-281 init_base_stats' knockback clamp (item
 * `knockback`, `negative_knockback`, the can_have_positive/negative_knockback
 * bounds) is owned by WeaponStats' bt_ws_resolve_core chain, which applies it
 * from the same item scalars; this header keeps no second copy. */

/* weapon.gd:318-347 get_next_cooldown / get_max_rand_cooldown. ONE randf draw. */
BT_HD static float bt_wfx_next_cooldown(BT_DEVICE BtState* s, int base_cd, int add_every,
                                        int nb_shots, int weapon_count, int at_wave_begin,
                                        float add_mult) {
    int big = (!at_wave_begin && add_every != -1 && add_every != 0 && (nb_shots % add_every) == 0);
    if (big) return (float)base_cd * add_mult;
    float basis = (float)base_cd;
    if (at_wave_begin && basis >= 180.0f) basis = 180.0f;
    int wc = weapon_count < 6 ? weapon_count : 6;
    float max_rand = bt_minf((float)wc * basis / 5.0f, (float)wc * 5.0f);
    float lo = basis - max_rand; if (lo < 1.0f) lo = 1.0f;
    return bt_rand_range(s, lo, basis + max_rand);
}

/* weapon.gd:345-347 update_current_spread: rand_range(-1+accuracy, 1-accuracy). */
BT_HD static float bt_wfx_accuracy_spread(BT_DEVICE BtState* s, float accuracy) {
    return bt_rand_range(s, -1.0f + accuracy, 1.0f - accuracy);
}

/* ---------------------------------------------------------------------------
 * Weapon-tier effect triggers (a weapon's own canonical rows).
 * ------------------------------------------------------------------------- */

/* unit.gd:552-570 + effects/weapons/exploding_effect.gd: a hit whose effect list
 * holds an ExplodingEffect rolls `chance` and on success spawns the explosion at
 * the TARGET; the target then takes NO direct damage. Delegates straight to the
 * shared position-based spawn (bt_item_expl_spawn). Returns 1 when one spawned. */
BT_HD static int bt_wfx_weapon_explode(BT_DEVICE BtState* s, unsigned short row,
                                       float x, float y, float dmg,
                                       float crit_chance, float crit_dmg,
                                       float burn_chance, float burn_dmg, float burn_dur,
                                       float curse_mod) {
    if (row >= BT_NITEM_EFF_ROWS) return 0;
    BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[row];
    if (!bt_item_expl_opcode((int)E->opcode)) return 0;
    if (!bt_chance(s, bt_wfx_curse_explode_chance(E->explode_chance, curse_mod))) return 0;
    return bt_item_expl_spawn(s, row, x, y, dmg, crit_chance, crit_dmg,
                              burn_chance, burn_dmg, burn_dur) >= 0;
}

/* Same roll and span as bt_wfx_weapon_explode, but returns the spawned explosion's
 * SLOT (>= 0) instead of a boolean so the caller can attribute it to the firing
 * weapon (unit.gd:569 connects the explosion's hit_something/killed_something to
 * `from`).  -1 = nothing spawned.  Kept separate so bt_wfx_weapon_explode's
 * boolean contract (used by the melee paths) is untouched. */
BT_HD static int bt_wfx_weapon_explode_src(BT_DEVICE BtState* s, unsigned short row,
                                           float x, float y, float dmg,
                                           float crit_chance, float crit_dmg,
                                           float burn_chance, float burn_dmg, float burn_dur,
                                           float curse_mod) {
    if (row >= BT_NITEM_EFF_ROWS) return -1;
    BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[row];
    if (!bt_item_expl_opcode((int)E->opcode)) return -1;
    if (!bt_chance(s, bt_wfx_curse_explode_chance(E->explode_chance, curse_mod))) return -1;
    return bt_item_expl_spawn(s, row, x, y, dmg, crit_chance, crit_dmg,
                              burn_chance, burn_dmg, burn_dur);
}

/* unit.gd:581-585 (inside `if not is_exploding`): the burn applies when
 * `Utils.get_chance_success(hitbox.burning_data.chance)` succeeds AND
 * `not hitbox.is_healing` AND the player owns can_burn_enemies.  GDScript's
 * `and` short-circuits LEFT TO RIGHT, so the chance is drawn BEFORE the
 * is_healing / can_burn_enemies gates -- i.e. the draw happens even when the
 * burning cannot be applied (utils.gd's get_chance_success still skips its own
 * draw at chance 0).  Callers pass can_burn = 0 for an is_healing hitbox. */
BT_HD static int bt_wfx_weapon_burn_apply(BT_DEVICE BtState* s, int enemy_slot, float can_burn,
                                          float burn_chance, float burn_dmg, float burn_dur) {
    if (!bt_chance(s, burn_chance)) return 0;
    if (can_burn <= 0.0f) return 0;
    bt_apply_enemy_burn(s, enemy_slot, burn_dmg, burn_dur);
    return 1;
}

/* ---------------------------------------------------------------------------
 * main.gd:798-860 gold-pickup weapon reload (pure queries).
 *
 * CONSUMER (core gold pickup, bt_core_step's materials block, NOT this file):
 *   for each weapon W with bt_wfx_weapon_reloads_on_gold(W's rows) and
 *   !W._is_shooting, keep the one with the LARGEST wpn_cd[slot]; then zero only
 *   that weapon's wpn_cd; and zero every BuilderTurret whose rows carry the key.
 *   When bt_wfx_reload_all_on_gold(s) (the player-level buccaneer effect) is
 *   true, zero EVERY weapon's wpn_cd.  Only the ranged blunderbuss (a DLC
 *   weapon row) and the buccaneer character ship the key today, so without this
 *   callsite the whole family is dead.
 * ------------------------------------------------------------------------- */
BT_HD static int bt_wfx_weapon_reloads_on_gold(BT_CONST_PTR(unsigned short) rows, int n_rows) {
    for (int i = 0; i < n_rows; i++) {
        int ri = (int)rows[i];
        if (ri < 0 || ri >= BT_NITEM_EFF_ROWS) continue;
        if (bt_eff_is(&BT_ITEM_EFF_ROWS[ri], BT_ISTR_RELOAD_WHEN_PICKUP_GOLD)) return 1;
    }
    return 0;
}
BT_HD static int bt_wfx_reload_all_on_gold(BT_DEVICE BtState* s) {
    return bt_wfx_scalar_keyed(s, BT_ISTR_RELOAD_WHEN_PICKUP_GOLD) > 0.0f ? 1 : 0;
}
/* weapons/weapon.gd:300-302: a reload_turrets_on_shoot row makes the shot emit
 * wanted_to_reset_turrets_cooldown; entity_spawner.gd:570 then zeroes every
 * BuilderTurret cooldown. Returns the number of turrets reset. ZERO RNG. */
BT_HD static int bt_wfx_reload_turrets(BT_DEVICE BtState* s, BT_THREAD const unsigned short* rows,
                                       int n_rows) {
    int has = 0;
    for (int i = 0; i < n_rows; i++) {
        int ri = (int)rows[i];
        if (ri < 0 || ri >= BT_NITEM_EFF_ROWS) continue;
        if (bt_eff_is(&BT_ITEM_EFF_ROWS[ri], BT_ISTR_RELOAD_TURRETS_ON_SHOOT)) { has = 1; break; }
    }
    if (!has) return 0;
    int reset = 0;
    for (int p = 0; p < s->pet_n; p++) {
        if (!s->pet_alive[p]) continue;
        int op = (int)s->pet_opcode[p];
        if (op != BT_ITEMEFF_BUILDER_TURRET && op != BT_ITEMEFF_BUILDER_TURRET_UPGRADE) continue;
        s->pet_timer[p] = 0.0f;
        reset++;
    }
    return reset;
}

/* ---------------------------------------------------------------------------
 * INTEGRATION HOOKS (parent wires these; all in files this header does not own)
 *
 * brotato_state.h:   `BtWeaponFxState wfx;`  and  `float player_lifesteal_cd;`
 * bt_core_reset:     bt_wfx_reset(&s->wfx); s->player_lifesteal_cd = 0.0f;
 * bt_enter_wave:     bt_wfx_reset_wave(&s->wfx);
 * bt_core_step:      Projectiles advances the shared lifesteal countdown ONCE
 *                    per step, in SECONDS:
 *                        if (s->player_lifesteal_cd > 0.0f)
 *                            s->player_lifesteal_cd -= BT_DT;
 * WeaponInventory:   bt_weapon_fx_slot_move(s, from, to) / _erase(s, slot) at the
 *                    same points it moves/erases its own per-slot arrays, plus
 *                    bt_weapon_fx_forget_uid(s, uid) for a copy that CEASES TO
 *                    EXIST (remove/combine/upgrade). Forget is never called on a
 *                    tail-slot move (that would clear a relocated uid);
 *                    bt_weapon_reset() calls _erase for every slot.
 * Melee/Ranged/Projectiles:
 *                    bt_wfx_lifesteal(s, &s->player_lifesteal_cd, lifesteal) per
 *                    hit, then
 *                    bt_wfx_on_weapon_hit(s, fx, slot_hint, uid, attack_id, rows,
 *                                         n_rows, row_curse)  -> break gold
 *                    bt_wfx_on_weapon_kill(s, fx, slot, uid, attack_id, rows,
 *                                          n_rows, row_curse)
 *                    bt_wfx_one_shot / bt_wfx_current_health_damage (feed
 *                    BtEnemyHitContext) / bt_wfx_force_crit_when_burning, and the
 *                    weapon-row triggers bt_wfx_weapon_explode / _burn_apply /
 *                    _charm_roll. `rows` is BT_CONST_PTR (the constant
 *                    BT_WEAPON_EFF_INDEX slice); `row_curse` is the BT_DEVICE
 *                    source array (s->wpn_fxcurse for Melee, p->src_fxcurse for
 *                    Projectiles).  `projectiles_on_hit`
 *                    children are spawned by Projectiles' bt_wproj_spawn_onhit,
 *                    not here (see the header AUDIT note).
 * bt_apply_item:     register WEAPON_COOLDOWN-opcode rows through this header's
 *                    queries, and register the KEY_VALUE rows whose bucket this
 *                    header reads:
 *                        if (E->storage == 1 &&
 *                            (E->custom_key == BT_ISTR_GOLD_ON_CRIT_KILL ||
 *                             E->custom_key == BT_ISTR_GAIN_STAT_WHEN_ATTACK_KILLED_ENEMIES))
 *                            { bt_scalar_bucket_register(s, row, nv, 0); break; }
 * bt_item_expl_hit_enemy: takes the final `BT_THREAD BtEnemyHitContext* ctx`
 *                    (rows/count/is_elemental/one_shot/current_health_damage/
 *                    owner_uid/attack_id/was_crit) and reads
 *                    bt_wfx_hit_bonus_pct(s, j, ctx->rows, ctx->count,
 *                    ctx->is_elemental, is_burning) plus the force-crit /
 *                    one-shot / current-health terms; it must not keep its own
 *                    above-hp loop. Forward-declare bt_wfx_hit_bonus_pct before
 *                    it if this header is included later in the TU.
 * bt_recompute_links: for a GainStatForEveryStat row whose source maps to
 *                    BT_STAT_NONE but whose aux_key is BT_ISTR_BURNING_ENEMY,
 *                    add bt_wfx_burning_enemy_link_bonus(s, row) * lk_count[row]
 *                    to link_stat[target] (item_fried_rice; the exactly stated
 *                    hook is in the comment above that helper).
 * ------------------------------------------------------------------------- */
