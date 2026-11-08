/* brotato_enemy_lifecycle.h -- the enemy-specific lifecycle the movement
 * dispatch depends on: the stargazer/iron_lung pair and the evil_mob gold
 * economy.  Included by brotato_core.h after brotato_enemy_modifiers.h (it
 * needs bt_enemy_final_health/damage/speed) and after brotato_enemy_motion.h.
 * Only reads and writes BtState; nothing allocates.
 *
 * SOURCE
 *   dlcs/dlc_1/enemies/stargazer/stargazer.gd:8-79 +
 *   dlcs/dlc_1/enemies/stargazer/stargazer_movement_behavior.gd:4-17
 *   dlcs/dlc_1/enemies/iron_lung/iron_lung.gd:6-51 and both .tscn
 *   entities/units/enemies/evil_mob/evil_mob.gd:11-55 and evil_mob.tscn /
 *   evil_mob_swimmer.tscn, items/materials/gold.gd, items/global/item.gd:56-68,
 *   global/entity_spawner.gd:344-437 (spawn_entity + enemy_respawned),
 *   main.gd:700-756 (spawn_gold + get_gold_value) and main.gd:793-856
 *   (on_gold_picked_up, incl. the player_index == -1 bonus-gold branch),
 *   singletons/run_data.gd:179,697-706 (bonus_gold, clamped at 0),
 *   entities/units/player/player.gd:1013-1035 (the player's attract/pickup and
 *   the "closer than the current attractor steals it" gold rule).
 *
 * STAGES
 *   1. Spawn: bt_enemy_motion_link_spawner (called by the birth flush right
 *      after e_source is written) links a freshly spawned iron_lung to the
 *      stargazer that emitted it (stargazer.gd:46-54).
 *   2. Tick (once per frame, before the material pass):
 *      a. drain the deferred stargazer-death -> lung-death queue
 *         (iron_lung.gd:47-50);
 *      b. IronLung DetectionZone: the first stargazer body inside the zone of a
 *         non-full lung sets is_full, heals the lung to +200% and kills that
 *         stargazer; the linked stargazers get angry first (iron_lung.gd:33-42
 *         emits became_full before the deferred die);
 *      c. evil_mob gold: claim (AttractArea r100), step toward the mob and eat
 *         (PickUpArea r30 @ (0,-10)) into gold_count, then the one-step
 *         evolution (evil_mob.gd:14-40).
 *   3. Death (bt_mark_enemy_dead, before the pool release): clear source, release
 *      the gold this mob owned (item.gd:62-66), queue the linked lung's death
 *      when a stargazer dies, and enrage the stargazers linked to a lung that
 *      dies while not full (stargazer.gd:57-62).
 *
 * NOT MODELLED (named): the renderer-only parts (evolution textures, LifeBar,
 * outline/flash, SoundManager).  `boost_ended` never fires in the source for an
 * enemy either (only the player removes boost stats), so a boosted enemy keeps
 * its boosted stats.  The gold entity's own Area2D circle radius is not in the
 * physics table, so the mob's pickup uses its 30 px zone radius directly (the
 * player's pickup uses the same convention with its own constant).
 */
#pragma once
#include "brotato_enemy_motion.h"

/* iron_lung.tscn DetectionZone/CollisionShape2D: lung-local (0,-27), r 52.0096,
 * mask 4|512 (enemy + player bodies); iron_lung.gd:34 acts on stargazers only. */
#define BT_LUNG_ZONE_DY (-27.0f)
#define BT_LUNG_ZONE_R  52.0096f
/* evil_mob.tscn ItemAttractArea: CircleShape2D default r 10 at scale (10,10)
 * -> 100, root-local (0,0).  ItemPickUpArea: r 10 at scale (3,3) -> 30 at
 * root-local (0,-10). */
#define BT_MOB_ATTRACT2  10000.0f   /* 100^2 */
#define BT_MOB_PICKUP_DY (-10.0f)
#define BT_MOB_PICKUP2   900.0f     /* 30^2 */

/* Deferred death queue: a stargazer's death must kill its linked lung, but the
 * death path itself is not re-entrant (bt_credit_kill drains death events), so
 * the kill is queued and drained at the top of the tick. */
BT_HD static void bt_enemy_lifecycle_kill_lung(BT_DEVICE BtState* s, int lung_slot) {
    if (s->n_lk_q >= BT_MAX) return;
    s->lk_q[s->n_lk_q++] = (unsigned short)lung_slot;
}

/* stargazer.gd:69-79 get_angry: switch to the OnTriggered follow_target and
 * boost.  Enemy.boost (enemy.gd:311-317) runs reset_health/damage/speed_stat
 * with the boost as the PERCENT modifier, recomputed from the kind base; when
 * the unit is already boost()ed the source ADDS its values onto the existing
 * _boosted_args (stargazer.gd:72-78), so the recorded buffer percentages are
 * added here too. */
BT_HD static void bt_enemy_lifecycle_angry(BT_DEVICE BtState* s, int slot) {
    bt_enemy_motion_stargazer_angry(s, slot);
    BT_CONST_PTR(BtUnitKind) K = &BT_KINDS[s->ekind[slot]];
    float w1 = (float)(s->wave - 1);
    float hp_pct = 150.0f, dmg_pct = 25.0f, spd_pct = 250.0f;
    if (s->eboosted[slot]) {
        hp_pct += s->eboost_hp[slot];
        dmg_pct += s->eboost_dmg[slot];
        spd_pct += s->eboost_spd[slot];
    }
    s->emax[slot] = bt_enemy_final_health(s, bt_floorf(K->hp + K->hp_grow * w1), hp_pct);
    s->ehp[slot] = s->emax[slot];
    s->edmg[slot] = bt_enemy_final_damage(s, K->dmg + K->dmg_grow * w1, dmg_pct);
    s->espd[slot] = bt_enemy_final_speed(s, (float)K->speed + s->espeed_rand[slot], spd_pct);
}

/* --- charm: collision layers, hurtbox and friendliness -------------------
 * Real Utils bits (singletons/utils.gd:18-30).  charm_enemy_effect_behavior.gd
 * :80-91 moves a charmed unit onto the PET layers and DISABLES its Hurtbox;
 * uncharm():163-171 restores the originals.  The player's Hurtbox mask is 20
 * (ENEMIES | ENEMY_PROJECTILES, player.tscn:1244), so a charmed unit can no
 * longer damage the player, while enemy.tscn's Hurtbox mask 1032
 * (PLAYER_PROJECTILES | PET_PROJECTILES) makes its contact Hitbox and its shots
 * (both on PET_PROJECTILES) land on OTHER ENEMIES.  The core's projectile
 * equivalent of the layer is prfriendly (1 = hits enemies). */
#define BT_LBIT_PLAYER             2u
#define BT_LBIT_ENEMIES            4u
#define BT_LBIT_PLAYER_PROJECTILES 8u
#define BT_LBIT_ENEMY_PROJECTILES 16u
#define BT_LBIT_ITEMS             32u
#define BT_LBIT_GOLD              64u
#define BT_LBIT_OBSTACLES        128u
#define BT_LBIT_BONUS_GOLD       256u
#define BT_LBIT_PETS             512u
#define BT_LBIT_PET_PROJECTILES 1024u
#define BT_LBIT_STRUCTURES      2048u

/* Body layer/mask: enemy.tscn root layer ENEMIES / mask 132
 * (ENEMIES|OBSTACLES); charmed = PETS / PETS|OBSTACLES (charm:86-87). */
BT_HD static unsigned int bt_enemy_body_layer(BT_DEVICE BtState* s, int slot) {
    return s->e_charmed[slot] ? BT_LBIT_PETS : BT_LBIT_ENEMIES;
}
BT_HD static unsigned int bt_enemy_body_mask(BT_DEVICE BtState* s, int slot) {
    return s->e_charmed[slot]
           ? (unsigned int)(BT_LBIT_PETS | BT_LBIT_OBSTACLES) : (unsigned int)BT_MASK_ENEMY;
}
/* Contact Hitbox layer: ENEMIES (enemy.tscn Hitbox) -> PET_PROJECTILES (charm:88). */
BT_HD static unsigned int bt_enemy_hitbox_layer(BT_DEVICE BtState* s, int slot) {
    return s->e_charmed[slot] ? BT_LBIT_PET_PROJECTILES : BT_LBIT_ENEMIES;
}
/* Shot layer: ENEMY_PROJECTILES -> PET_PROJECTILES (charm:82-85).  The core
 * spells the same thing as prfriendly = 1 ("a pet/turret shot hits enemies"). */
BT_HD static unsigned int bt_enemy_shot_layer(BT_DEVICE BtState* s, int slot) {
    return s->e_charmed[slot] ? BT_LBIT_PET_PROJECTILES : BT_LBIT_ENEMY_PROJECTILES;
}
/* charm:87 _parent._hurtbox.disable() -- a charmed unit takes no player damage. */
BT_HD static int bt_enemy_hurtbox_enabled(BT_DEVICE BtState* s, int slot) {
    return s->e_charmed[slot] ? 0 : 1;
}
/* Its contact hitbox and shots resolve on enemies while charmed. */
BT_HD static int bt_enemy_hits_enemies(BT_DEVICE BtState* s, int slot) {
    return (int)s->e_charmed[slot];
}

/* charm:93-96 / uncharm:173-176: a charmed Healer or Buffer retargets its
 * BoostZone from the scene's ENEMIES mask (buffer.tscn:56) to
 * PLAYER_BIT|STRUCTURES_BIT.  The class test is the source's own (`_parent is
 * Healer or _parent is Buffer`), taken from the scene root script. */
BT_HD static int bt_enemy_boost_retargets(BT_DEVICE BtState* s, int slot) {
    if (!s->e_charmed[slot]) return 0;
    return (BT_KIND_MOVK[s->ekind[slot]] & (BT_MOVK_HEALER | BT_MOVK_BUFFER)) != 0;
}
BT_HD static unsigned int bt_enemy_boost_zone_mask(BT_DEVICE BtState* s, int slot) {
    return bt_enemy_boost_retargets(s, slot)
           ? (unsigned int)(BT_LBIT_PLAYER | BT_LBIT_STRUCTURES) : BT_LBIT_ENEMIES;
}

/* charm_enemy_effect_behavior.gd:72-131 charm(): the state plus the effects the
 * movement slice owns.  The collision-layer/hurtbox/projectile-layer/BoostZone
 * swaps and the "first damage is absorbed" rule are combat: the caller gates
 * them on bt_enemy_charmed() and consumes bt_enemy_charm_absorb().  The fresh
 * TargetRandPosMovementBehavior the source add_child()s starts with a ZERO
 * _current_target, so the node's stored target is cleared here. */
BT_HD static void bt_enemy_lifecycle_charm(BT_DEVICE BtState* s, int slot, int player_index) {
    s->e_charmed[slot] = 1;
    s->e_charm_by[slot] = (short)player_index;
    s->e_charm_timer[slot] = BT_CHARM_DURATION;
    s->e_charm_shield[slot] = 1;                 /* _absorb_next_damage */
    if (player_index >= 0 && player_index < 4) s->charm_count[player_index]++;
    s->eburn[slot] = 0.0f; s->eburndmg[slot] = 0.0f;   /* stop_burning(): the
                                 * core models burning as the frame countdown +
                                 * tick damage only, so clearing both is the
                                 * whole conversion; set_burning(false)'s
                                 * reset_speed_stat has nothing to remove (the
                                 * core has no burning_enemy_speed consumer) and
                                 * RunData.current_burning_enemies is unmodeled
                                 * (a pre-existing burning-model gap). */
    BT_CONST_PTR(BtUnitKind) K = &BT_KINDS[s->ekind[slot]];
    float w1 = (float)(s->wave - 1);
    /* reset_damage_stat(BT_CHARM_DAMAGE_INCREASE) -- recomputed from the kind
     * base, exactly like Enemy.boost (enemy.gd:311-317) */
    s->edmg[slot] = bt_enemy_final_damage(s, K->dmg + K->dmg_grow * w1,
                                          (float)BT_CHARM_DAMAGE_INCREASE);
    if (s->espd[slot] > 0.0f) s->ebonus[slot] += 100.0f;   /* charm:127-128 */
    unsigned short inst = s->e_instance[slot];
    if (inst != (unsigned short)BT_POOL_NO_INST) {
        int node = bt_mov_node(s, slot);
        *bt_pool_mvtx(s, inst, node) = 0.0f;
        *bt_pool_mvty(s, inst, node) = 0.0f;
    }
    /* charm:86-91: the body moves onto the PET layers (its Hitbox and shots are
     * on PET_PROJECTILES, its Hurtbox is disabled -- the shared enemy-hit
     * primitive already refuses a charmed target). */
    s->phys.bodies[slot].layer = bt_enemy_body_layer(s, slot);
    s->phys.bodies[slot].mask = bt_enemy_body_mask(s, slot);
}

/* charm_enemy_effect_behavior.gd:158-188 uncharm(): reached from on_death
 * (:135-138) and from the CharmTimer expiry's die().  The movement revert is
 * implicit (bt_mov_beh only applies the swap while e_charmed). */
BT_HD static void bt_enemy_lifecycle_uncharm(BT_DEVICE BtState* s, int slot) {
    if (!s->e_charmed[slot]) return;
    int by = (int)s->e_charm_by[slot];
    if (by >= 0 && by < 4 && s->charm_count[by] > 0) s->charm_count[by]--;
    s->e_charmed[slot] = 0;
    s->e_charm_by[slot] = -1;
    s->e_charm_timer[slot] = 0.0f;
    s->e_charm_shield[slot] = 0;
    if (s->espd[slot] > 0.0f) s->ebonus[slot] -= 100.0f;
    /* uncharm():163-166 restores the original layers */
    s->phys.bodies[slot].layer = bt_enemy_body_layer(s, slot);
    s->phys.bodies[slot].mask = bt_enemy_body_mask(s, slot);
}

/* entity_spawner.gd:411-412 spawn_entity: `if charmed_by != -1:
 * entity.set_charmed(charmed_by)` -- the summon's charmed_by travels with the
 * source (enemy.gd:334, spawner.gd:12).  charm()'s only guard is `if charmed`,
 * so the spawn path applies it directly (the on_hurt gates are for hit-driven
 * charming). */
BT_HD static void bt_enemy_lifecycle_charm_spawn(BT_DEVICE BtState* s, int slot, int charmed_by) {
    if (charmed_by < 0) return;
    if (s->e_charmed[slot]) return;
    bt_enemy_lifecycle_charm(s, slot, charmed_by);
}

/* --- charmed Healer/Buffer BoostZone (charm:93-96, uncharm:173-176) --------
 * The zone mask becomes PLAYER|STRUCTURES, so the boost/heal lands on the player
 * (and structures) instead of enemies.  player.gd:1100-1112 boost(): the hp
 * boost is a percent of max HP added to max AND current health, the speed and
 * attack-speed boosts are flat TempStats, all removed by boost_ended
 * (player.gd:1114-1128) when the player.tscn BoostTimer (5.0 s) fires.
 * turret.gd:162-176 / landmine.gd:62-71: base damage and range scaled and the
 * cooldown shrunk, restored by boost_ended. */
BT_HD static void bt_enemy_lifecycle_charmed_boost(BT_DEVICE BtState* s, int buffer_slot) {
    BT_CONST_PTR(BtBoost) B = &BT_KIND_BOOST[s->ekind[buffer_slot]];
    float bx = s->ex[buffer_slot], by = s->ey[buffer_slot];
    unsigned int mask = bt_enemy_boost_zone_mask(s, buffer_slot);
    /* the player (body circle, as in the pet-targeting zone checks) */
    if ((mask & BT_LBIT_PLAYER) && !s->pboost_active) {
        if (bt_circle_overlap_offset(bt_v2(bx, by), bt_v2zero(), B->radius,
                                     bt_v2(s->px, s->py), bt_v2(0.0f, BT_PLAYER_BODY_Y),
                                     BT_PLAYER_BODY_R)) {
            /* player.gd:1110-1112 boost(): the increase is int(max_stats.health *
             * hp_boost/100) and it heals current_stats.health directly (NOT via
             * the deferred pass), so the LiveStats max is the deferred p_max_hp
             * while the hp add is immediate here. */
            int increase = (int)(s->p_max_hp * (B->player_hp_pct / 100.0f));
            bt_core_stat_add_temp(s, 0, (float)increase);        /* stat_max_hp */
            s->hp += (float)increase;
            bt_core_stat_add_temp(s, 3, B->player_speed_pct);    /* stat_speed */
            bt_core_stat_add_temp(s, 15, B->player_attack_speed_pct);
            s->pboost_active = 1;
            s->pboost_timer = 5.0f;                              /* player.tscn BoostTimer */
            s->pboost_hp = (float)increase;
            s->pboost_speed = B->player_speed_pct;
            s->pboost_aspeed = B->player_attack_speed_pct;
        }
    }
    /* structures (turret/builder/landmine instances) */
    if (mask & BT_LBIT_STRUCTURES) {
        for (int q = 0; q < s->pet_n; q++) {
            if (!s->pet_alive[q] || !s->pet_isstruct[q] || s->pstboost_active[q]) continue;
            /* the structure's scene body circle (same lookup pet_motion uses,
             * inlined so this header needs no pet_motion include) */
            unsigned short gr = BT_ITEMS[BT_ITEM_EFF_ROWS[s->pet_row[q]].item].scene_geom;
            int g = (gr < BT_NITEM_SCENES) ? (int)gr : -1;
            float rr = B->radius + (g >= 0 ? BT_ITEM_SCENES[g].body_radius : 0.0f);
            if (!bt_circle_overlap_offset(bt_v2(bx, by), bt_v2zero(), rr,
                                          bt_v2(s->pet_x[q], s->pet_y[q]),
                                          (g >= 0 ? bt_v2(BT_ITEM_SCENES[g].body_x, BT_ITEM_SCENES[g].body_y)
                                                  : bt_v2zero()),
                                          0.0f)) continue;
            float sp = B->structure_attack_speed_pct / 100.0f;
            s->pstboost_active[q] = 1;
            s->pstboost_timer[q] = 5.0f;                         /* turret.tscn BoostTimer */
            s->pstboost_dmg[q] = s->pet_dmg[q];
            s->pstboost_rng[q] = s->pet_range[q];
            s->pstboost_cd[q] = s->pet_cd[q];
            s->pet_dmg[q] *= 1.0f + B->structure_damage_pct / 100.0f;
            s->pet_range[q] *= 1.0f + B->structure_range_pct / 100.0f;
            s->pet_cd[q] -= s->pet_cd[q] * sp;
            s->pet_timer[q] -= s->pet_cd[q] * sp;
            if (s->pet_timer[q] < 0.0f) s->pet_timer[q] = 0.0f;
        }
    }
}

/* Boost expiry (player.gd:1114-1128 boost_ended + turret.gd:178-181) and the
 * Healer BoostZone step, both once per frame from bt_enemy_lifecycle_tick. */
BT_HD static void bt_enemy_lifecycle_boost_expire(BT_DEVICE BtState* s) {
    if (s->pboost_active) {
        s->pboost_timer -= BT_DT;
        if (s->pboost_timer <= 0.0f) {
            bt_core_stat_add_temp(s, 0, -s->pboost_hp);
            bt_core_stat_add_temp(s, 3, -s->pboost_speed);
            bt_core_stat_add_temp(s, 15, -s->pboost_aspeed);
            if (s->hp > s->max_hp) s->hp = s->max_hp;
            s->pboost_active = 0;
            s->pboost_timer = 0.0f;
        }
    }
    for (int q = 0; q < s->pet_n; q++) {
        if (!s->pstboost_active[q]) continue;
        s->pstboost_timer[q] -= BT_DT;
        if (s->pstboost_timer[q] > 0.0f) continue;
        s->pet_dmg[q] = s->pstboost_dmg[q];
        s->pet_range[q] = s->pstboost_rng[q];
        s->pet_cd[q] = s->pstboost_cd[q];
        s->pstboost_active[q] = 0;
        s->pstboost_timer[q] = 0.0f;
    }
}

/* healer.gd:13-31 _on_BoostZone_body_entered: on ENTRY into the zone of a
 * damaged non-structure body, a Healer heals it -- `player_heal` for a Player,
 * `heal` for an Enemy (both + (wave-1)*the matching increase).  The membership
 * bitset reproduces the Area2D overlap set so an entry (0 -> 1 bit) is what
 * triggers a heal. */
BT_HD static int bt_heal_bit_get(BT_DEVICE BtState* s, int healer, int target) {
    return (s->heal_in[healer][target >> 3] >> (target & 7)) & 1;
}
BT_HD static void bt_heal_bit_set(BT_DEVICE BtState* s, int healer, int target, int v) {
    BT_DEVICE unsigned char* b = &s->heal_in[healer][target >> 3];
    if (v) *b = (unsigned char)(*b | (unsigned char)(1u << (target & 7)));
    else *b = (unsigned char)(*b & (unsigned char)~(1u << (target & 7)));
}
BT_HD static void bt_enemy_lifecycle_heal_step(BT_DEVICE BtState* s) {
    for (int i = bt_enemy_next(s, 0, 1); i < (int)s->e_used; i = bt_enemy_next(s, i + 1, 1)) {
        if (!s->ealive[i] || s->eneutral[i]) continue;
        if (!(BT_KIND_MOVK[s->ekind[i]] & BT_MOVK_HEALER)) continue;
        float r = BT_KIND_BOOST[s->ekind[i]].radius;
        if (r <= 0.0f) continue;
        float w1 = (float)(s->wave - 1);
        BT_CONST_PTR(float) H = BT_KIND_HEAL[s->ekind[i]];
        int charmed = (int)s->e_charmed[i];
        /* membership bit 0 = the player, bit (j+1) = enemy slot j */
        int pin = charmed && bt_circle_overlap_offset(bt_v2(s->ex[i], s->ey[i]), bt_v2zero(), r,
                        bt_v2(s->px, s->py), bt_v2(0.0f, BT_PLAYER_BODY_Y), BT_PLAYER_BODY_R);
        int was_p = bt_heal_bit_get(s, i, 0);
        bt_heal_bit_set(s, i, 0, pin);
        if (pin && !was_p && s->hp < s->p_max_hp) {
            /* healer.gd:24-26: a damaged Player gets int(player_heal + (wave-1)*grow) */
            bt_birth_audio_enqueue(s, BT_POSITIONAL_AUDIO_HEAL);
            (void)bt_scalar_heal(s, (float)(int)(H[2] + w1 * H[3]));
        }
        for (int j = 0; j < (int)s->e_used; j++) {
            int want = 0;
            if (!charmed && j != i && s->ealive[j] && !s->eneutral[j]) {
                BT_CONST_PTR(BtBodyPhysics) G = bt_kgeom(s, j);
                want = bt_circle_overlap_offset(bt_v2(s->ex[i], s->ey[i]), bt_v2zero(), r,
                                                bt_v2(s->ex[j], s->ey[j]),
                                                bt_v2(G->body_x, G->body_y), G->body_radius);
            }
            int was = bt_heal_bit_get(s, i, j + 1);
            bt_heal_bit_set(s, i, j + 1, want);
            if (!want || was) continue;      /* only an ENTRY heals */
            /* healer.gd:28-29: an Enemy is healed by heal + (wave-1)*grow, capped */
            if (s->ehp[j] < s->emax[j]) {
                bt_birth_audio_enqueue(s, BT_POSITIONAL_AUDIO_HEAL);
                s->ehp[j] += H[0] + w1 * H[1];
                if (s->ehp[j] > s->emax[j]) s->ehp[j] = s->emax[j];
            }
        }
    }
}

/* Accessors for the combat paths that the charm behaviour rewires. */
BT_HD static int bt_enemy_charmed(BT_DEVICE BtState* s, int slot) {
    return (int)s->e_charmed[slot];
}
/* enemy.gd:337-341 get_charmed_by_player_index(): the charmer, or -1.  The
 * spawn path passes this along with the source (enemy.gd:334, spawner.gd:12). */
BT_HD static int bt_enemy_lifecycle_charmed_by(BT_DEVICE BtState* s, int slot) {
    return s->e_charmed[slot] ? (int)s->e_charm_by[slot] : -1;
}
/* charm_enemy_effect_behavior.gd:133-137 get_bonus_damage: the FIRST damage
 * instance after charming returns `0 - LARGE_NUMBER` (a full absorb) and clears
 * the flag; every later hit is normal. */
BT_HD static int bt_enemy_charm_absorb(BT_DEVICE BtState* s, int slot) {
    if (!s->e_charm_shield[slot]) return 0;
    s->e_charm_shield[slot] = 0;
    return 1;
}

/* main.gd:1246-1263 manage_harvesting (wave end, after end_of_the_wave, before
 * clean_up_room): each per-player total adds
 *   charmed_enemy_bonus = sum over live enemies charmed by that player of
 *                         get_gold_value(ENEMY, default_die_args, stats.value)
 * `stats.value` is the kind's BASE value (BT_KINDS[].value -- never the evil_mob
 * get_stats_value override); default_die_args has killed_by_player_index = -1
 * (no scale_materials_with_distance) and no hitbox (no effect-behavior gold
 * modifier), and get_gold_value consumes NO draws.  The source sums in
 * _entity_spawner.enemies insertion order; this walks slots ascending, which
 * differs only in float summation order. */
BT_HD static float bt_enemy_lifecycle_charmed_bonus(BT_DEVICE BtState* s, int player_index) {
    float total = 0.0f;
    for (int i = bt_enemy_next(s, 0, 1); i < (int)s->e_used; i = bt_enemy_next(s, i + 1, 1)) {
        if (!s->ealive[i] || s->eneutral[i]) continue;
        if (!s->e_charmed[i] || (int)s->e_charm_by[i] != player_index) continue;
        total += bt_loot_gold_value(s, 0, (float)BT_KINDS[s->ekind[i]].value);
    }
    return total;
}

/* evil_mob.gd:32-55: the PickUpArea ate one gold.  `gold.pickup(-1)` removes it
 * from _active_golds (main.gd:795-797) and -- player_index < 0 -- banks its
 * value as bonus_gold instead of crediting materials/XP (main.gd:854,
 * run_data.gd:697-706 clamps to >= 0).  One evolution step per pickup. */
BT_HD static void bt_enemy_lifecycle_mob_eat(BT_DEVICE BtState* s, int i, int m) {
    int val = (int)s->matval[m];
    unsigned int cnt = (unsigned int)s->emob_count[i] + (unsigned int)val;
    s->emob_count[i] = (unsigned short)(cnt > 65535u ? 65535u : cnt);
    s->matalive[m] = 0;
    s->matcount--;
    s->mat_attr[m] = 0; s->mat_by[m] = 0; s->mat_home[m] = 0; s->mat_mob[m] = -1;
    s->bonus_gold += val;
    if (s->bonus_gold < 0) s->bonus_gold = 0;
    int evo = (int)s->emob_evo[i];
    if (evo + 1 < 3 &&
        (int)s->emob_count[i] >= (int)BT_KIND_MOB_QTY[s->ekind[i]][evo + 1]) {
        evo += 1;
        s->emob_evo[i] = (unsigned char)evo;
        /* evil_mob.gd:50,51-52: bonus_speed = -(dec*evolution); health *= int(1+0.5*evo) */
        s->ebonus[i] = -((float)BT_KIND_MOB_DECREASE[s->ekind[i]] * (float)evo);
        float mult = (float)(int)(1.0f + 0.5f * (float)evo);
        s->emax[i] *= mult;
        s->ehp[i] *= mult;
    }
}

/* bt_mark_enemy_dead hook, run BEFORE the pool release so the dying unit's
 * identity is still resolvable. */
BT_HD static void bt_enemy_lifecycle_on_death(BT_DEVICE BtState* s, int j) {
    s->e_source[j] = -1;                      /* enemy.gd:227 */
    /* charm_enemy_effect_behavior.gd:135-138 on_death -> uncharm(): the charm
     * count and the bonus speed are released even for a non-enemy-domain slot. */
    bt_enemy_lifecycle_uncharm(s, j);
    /* A neutral's uid retirement is NOT here: it happens when the death animation
     * ends (bt_neutral_uid_retire/step in brotato_core.h, entity.gd:68-100). */
    if (s->eneutral[j]) return;               /* BT_KIND_MOVK is the enemy domain */
    unsigned char mk = BT_KIND_MOVK[s->ekind[j]];
    /* item.gd:62-66: a dead attractor releases the gold it owned. */
    for (int m = 0; m < (int)s->mat_used; m++) {
        if (s->mat_mob[m] != (short)j) continue;
        s->mat_mob[m] = -1;
        s->mat_arc[m][BT_LOOT_ARC_SPEED] = BT_LOOT_ITEM_ATTRACT_SPEED;
        s->mat_attr[m] = 0;
    }
    if (mk & BT_MOVK_STARGAZER) {
        /* stargazer.gd:37 connects its "died" to the lung's on_stargazer_died
         * (iron_lung.gd:47-50): the lung dies too when it is not full. */
        unsigned short linst = s->mv_lung[j];
        int l = bt_mov_slot_of_instance(s, linst);
        if (l >= 0 && !s->pool_lungfull[linst]) bt_enemy_lifecycle_kill_lung(s, l);
    }
    if (mk & BT_MOVK_IRON_LUNG) {
        /* stargazer.gd:57-62 on_target_lung_died: a lung that dies while not
         * full enrages every stargazer linked to it. */
        unsigned short inst = s->e_instance[j];
        if (inst != (unsigned short)BT_POOL_NO_INST && !s->pool_lungfull[inst]) {
            for (int st = bt_enemy_next(s, 0, 1); st < (int)s->e_used; st = bt_enemy_next(s, st + 1, 1)) {
                if (!s->ealive[st] || s->eneutral[st]) continue;
                if (st == j) continue;
                if (s->mv_lung[st] != inst) continue;
                if (!(BT_KIND_MOVK[s->ekind[st]] & BT_MOVK_STARGAZER)) continue;
                bt_enemy_lifecycle_angry(s, st);
            }
        }
    }
}

/* Drain the deferred queue (iron_lung.gd:47-50).  Called at the top AND the end
 * of the tick so a lung killed by this frame's zone check dies in the same
 * frame, exactly like the source's synchronous signal. */
BT_HD static void bt_enemy_lifecycle_drain_kills(BT_DEVICE BtState* s) {
    for (int q = 0; q < (int)s->n_lk_q; q++) {
        int l = (int)s->lk_q[q];
        if (l < 0 || l >= BT_MAX || !s->ealive[l] || s->eneutral[l]) continue;
        if (!(BT_KIND_MOVK[s->ekind[l]] & BT_MOVK_IRON_LUNG)) continue;
        (void)bt_credit_kill(s, l, 1);
    }
    s->n_lk_q = 0;
}

/* Once per frame.  Placement in bt_core_step: before the material pass, so an
 * evil_mob claims a gold before the player's this-frame attract test and the
 * player can still steal it back when strictly closer (player.gd:1028-1029). */
BT_HD static void bt_enemy_lifecycle_tick(BT_DEVICE BtState* s) {
    /* 0. charm expiry: charm_enemy_effect_behavior.gd:151-156 the CharmTimer
     * (wait_time = Utils.CHARM_DURATION) times out into _parent.die(), whose
     * on_death uncharm()s. */
    for (int i = bt_enemy_next(s, 0, 1); i < (int)s->e_used; i = bt_enemy_next(s, i + 1, 1)) {
        if (!s->ealive[i] || !s->e_charmed[i]) continue;
        s->e_charm_timer[i] -= BT_DT;
        if (s->e_charm_timer[i] <= 0.0f) {
            s->e_charm_timer[i] = 0.0f;
            (void)bt_credit_kill(s, i, 1);
        }
    }
    /* 1. deferred stargazer-death -> linked-lung deaths (iron_lung.gd:47-50) */
    bt_enemy_lifecycle_drain_kills(s);

    /* 2. IronLung DetectionZone (iron_lung.gd:33-42) */
    for (int i = bt_enemy_next(s, 0, 1); i < (int)s->e_used; i = bt_enemy_next(s, i + 1, 1)) {
        if (!s->ealive[i] || s->eneutral[i]) continue;
        if (!(BT_KIND_MOVK[s->ekind[i]] & BT_MOVK_IRON_LUNG)) continue;
        unsigned short inst = s->e_instance[i];
        if (inst == (unsigned short)BT_POOL_NO_INST || s->pool_lungfull[inst]) continue;
        float zx = s->ex[i], zy = s->ey[i] + BT_LUNG_ZONE_DY;
        for (int j = bt_enemy_next(s, 0, 1); j < (int)s->e_used; j = bt_enemy_next(s, j + 1, 1)) {
            if (!s->ealive[j] || s->eneutral[j] || j == i) continue;
            if (!(BT_KIND_MOVK[s->ekind[j]] & BT_MOVK_STARGAZER)) continue;
            BT_CONST_PTR(BtBodyPhysics) G = bt_kgeom(s, j);
            float bx = s->ex[j] + G->body_x, by = s->ey[j] + G->body_y;
            float rr = BT_LUNG_ZONE_R + G->body_radius;
            float dx = bx - zx, dy = by - zy;
            if (dx * dx + dy * dy > rr * rr) continue;
            s->pool_lungfull[inst] = 1;
            BT_CONST_PTR(BtUnitKind) K = &BT_KINDS[s->ekind[i]];
            float w1 = (float)(s->wave - 1);
            /* iron_lung.gd:40 reset_health_stat(200) */
            s->emax[i] = bt_enemy_final_health(s, bt_floorf(K->hp + K->hp_grow * w1), 200.0f);
            s->ehp[i] = s->emax[i];
            bt_enemy_lifecycle_angry(s, j);   /* became_full -> get_angry (pre-death) */
            (void)bt_credit_kill(s, j, 1);    /* iron_lung.gd:41 body.die() */
            break;
        }
    }

    /* 3. evil_mob gold (evil_mob.gd:14-40 + item.gd:56-68) */
    for (int i = bt_enemy_next(s, 0, 1); i < (int)s->e_used; i = bt_enemy_next(s, i + 1, 1)) {
        if (!s->ealive[i] || s->eneutral[i]) continue;
        if (!(BT_KIND_MOVK[s->ekind[i]] & BT_MOVK_EVIL_MOB)) continue;
        for (int m = 0; m < (int)s->mat_used; m++) {
            if (!s->matalive[m]) continue;
            if (s->mat_arc[m][BT_LOOT_ARC_MON] == 0.0f) continue;   /* not monitorable yet */
            /* PickUpArea (evil_mob.gd:30-40): eats ANY gold in range, no owner
             * test -- the source's area_entered has none either. */
            float px = s->ex[i], py = s->ey[i] + BT_MOB_PICKUP_DY;
            float pdx = s->matx[m] - px, pdy = s->maty[m] - py;
            if (pdx * pdx + pdy * pdy <= BT_MOB_PICKUP2) {
                bt_enemy_lifecycle_mob_eat(s, i, m);
                continue;
            }
            if (s->mat_by[m] != 0) continue;                         /* a pet owns it */
            if (s->mat_attr[m] != 0 && s->mat_attr[m] != 1) continue;
            int owner = (int)s->mat_mob[m];
            if (owner >= 0 && (owner != i || !s->ealive[owner] || s->eneutral[owner]))
                owner = -1;
            if (owner < 0) {
                if (s->mat_attr[m] == 1) continue;   /* already the player's */
                /* AttractArea (evil_mob.gd:22-26): claim an unowned gold. */
                float adx = s->matx[m] - s->ex[i], ady = s->maty[m] - s->ey[i];
                if (adx * adx + ady * ady > BT_MOB_ATTRACT2) continue;
                s->mat_mob[m] = (short)i;
                owner = i;
            }
            if (owner != i) continue;
            /* the source homes the gold toward attracted_by (item.gd:56-68) with
             * its own _physics_process, which the material pass no longer runs
             * for a mob-owned gold. */
            s->mat_attr[m] = 1;
            (void)bt_loot_item_step(&s->matx[m], &s->maty[m], s->mat_arc[m], &s->mat_attr[m],
                                    0, s->ex[i], s->ey[i], BT_DT);
        }
    }

    /* 4. charm-driven BoostZone runtime: the charmed Healer/Buffer retarget
     * (applied from the enemy loop's timer) and its expiry, plus the Healer's
     * entry heal, both once per frame. */
    bt_enemy_lifecycle_boost_expire(s);
    bt_enemy_lifecycle_heal_step(s);

    /* 5. a stargazer killed by this frame's zone check dies with its lung in the
     * same frame (iron_lung.gd:47-50). */
    bt_enemy_lifecycle_drain_kills(s);
}
