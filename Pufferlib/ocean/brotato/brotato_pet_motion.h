/* brotato_pet_motion.h -- pet motion + interaction zones for the shared core.
 *
 * Included by brotato_core.h after brotato_progression.h and before
 * bt_core_step, so every helper it calls (bt_rand_range, BT_ITEM_EFF_ROWS,
 * bt_credit_kill, bt_progress_add_xp, ...) is already defined. It only reads
 * and writes BtState; nothing allocates and nothing touches libc/stdio.
 *
 * SOURCE
 *   entities/units/pet/pet.gd + one subclass per pet, each pet's own .tscn
 *   (MovementBehavior / TargetBehavior scripts and their exports, every
 *   interaction Area2D's CircleShape2D, the Stats resource) and
 *   entities/units/movement_behaviors/*.gd. All of that is resolved from the
 *   recovered sources by extract_items.py into BT_PET_AI / BT_PET_ZONE_FX
 *   (brotato_item_effects.h); this file implements the behaviour.
 *
 *   * motion: unit.gd get_move_input = normalize(movement) * get_move_speed(),
 *     integrated with the same "snap onto the target, clamp into the zone rect"
 *     rule unit.gd _integrate_forces applies to a RigidBody2D in CHARACTER mode.
 *     follow_target / follow_target+stop_close / target_rand_pos /
 *     patrol_around_the_map / follow_rand_pos_around_player, chosen per pet from
 *     its scene's MovementBehavior script.
 *   * targeting: the pet's TargetBehavior script -- target it spawned-range
 *     (Range/TargetTriggerZone Area2D vs the target's body circle), the
 *     "first enemy when none is in range" fallback, the per-pet-type claim that
 *     stops two pets of the same type from chasing one enemy
 *     (PetService.add_target_for_pet), and the lootworm's tree-then-gold scan.
 *   * DocMoth BoostZone: while the zone (radius x boost_zone_scale) covers a
 *     player, the pet's own `effects` array is in force -- hp_regen_bonus
 *     (consumed by the core's regen tick) and double_lifesteal_bonus.
 *   * Scapegoat: its own health pool, the 0.6 s hurtbox cooldown, enemy contact
 *     and enemy-projectile hits, and the self-revive while a player stands in
 *     its HealingTriggeringZone.
 *   * Lootworm: its own ItemAttractArea / ItemPickUpArea over the gold
 *     entities (rolling its own double_chance and then running the player's
 *     common gold-pickup effects, exactly like the player's own pickup)
 *     and its TreeArea2D-triggered melee swing that only damages trees.
 *   * CatlingGun: PlayerTriggerZone stops its movement (mad mode).
 *   * body contacts: every pet of one of the eight BT_PET_AI classes is a
 *     RigidBody2D body (entity.tscn mode CHARACTER, mass 1) with its scene's
 *     body CircleShape2D and the scene root's collision_layer/mask, resolved
 *     through BT_ITEM_SCENES and BT_PET_AI. Pet-vs-pet and pet-vs-wall body
 *     contacts run through the shared GodotPhysics step (bt_phys_step) in the
 *     dedicated pet contact world (see "pet body contacts" below).
 *
 * NOT SIMULATED (named, not stubbed): the source's item push-back / idle timer
 * before a dropped gold can be attracted is not modelled (the core never
 * modelled it). boost_zone_scale multiplies the DocMoth zone radius; the
 * source scales the Area2D node. Structure bodies (layer 2048 / mask 2176) are
 * still not registered anywhere, so structure-vs-structure and structure-vs-wall
 * contacts do not run and no pet ever pairs with a structure.
 *
 * CORE WIRING: whatever owns BtState must (1) give this module a persistent
 * `BtPhysWorld phys_pet` field and (2) call bt_pet_world_init(s) once per
 * ARENA, after bt_phys_walls(s) -- the pet world's four wall bodies are copies
 * of the enemy world's.  The source rebuilds the whole scene, and therefore
 * every BodyPair2DSW, at each wave, so the call belongs at the wave-start hook
 * (bt_clear_wave_summons, reached from bt_enter_wave, which bt_core_reset
 * reaches through bt_enter_wave(1)): a recycled pet body slot must never
 * inherit a warm-started impulse from the previous arena's owner.  There is no
 * in-header guard -- a missing call is a missing call, never a silent
 * re-initialisation.
 */
#pragma once

/* 0.6 s hurtbox cooldown after a hit (scapegoat.gd INVULNERABLE_COOLDOWN). */
#define BT_PET_INVULN_CD   0.6f
/* entity.gd _min_pos/_max_pos: the zone rect inset by half the sprite size. */
#define BT_PET_RAND_EPS2   100.0f   /* MovementBehavior.EQUALITY_PRECISION^2 */
/* catling_gun.tscn:1238 PlayerTriggerZone.collision_mask = 514 (player 2 |
 * pets 512).  An Area2D body_entered only fires for a body whose
 * collision_layer intersects the zone mask, so a layer-0 pet body
 * (lootworm/scapegoat/doc_moth, BT_PET_AI.collision_layer) is invisible to it. */
#define BT_PET_PLAYER_ZONE_MASK 514u

/* --- forward declarations (defined later in the core) -------------------- */

/* brotato_scalar_events.h is included after this header, so the lootworm's own
 * ItemPickUpArea hands its gold to the player's pickup through a forward
 * declaration: the common gold-pickup effects (chance_double_gold,
 * heal_when_pickup_gold, dmg_when_pickup_gold) run exactly as they do for the
 * player's own magnet/pickup pass. */
BT_HD static int bt_scalar_gold_pickup(BT_DEVICE BtState* s, int base_value);

/* --- generated-table lookups --------------------------------------------- */

/* BT_PET_AI row index for an effect opcode (-1 when the opcode is not a pet). */
BT_HD static int bt_pet_ai_index(int opcode) {
    for (int i = 0; i < BT_NPET_AI; i++)
        if ((int)BT_PET_AI[i].opcode == opcode) return i;
    return -1;
}

/* BT_ITEM_SCENES row of the scene a pet effect row instantiates (-1 if none). */
BT_HD static int bt_pet_scene_geom(int row) {
    unsigned short g = BT_ITEMS[BT_ITEM_EFF_ROWS[row].item].scene_geom;
    return (g < BT_NITEM_SCENES) ? (int)g : -1;
}

/* summon_fx[] instance that instantiated pet slot p (-1 when unknown). Pets of
 * one effect row are interchangeable, so bt_pet_pick_fx only has to hand out
 * the multiset of owned instances, not a specific copy. */
BT_HD static int bt_pet_fx_of(BT_DEVICE BtState* s, int p) {
    int i = (int)s->pet_fx[p];
    if (i < 0 || i >= s->n_summon_fx) return -1;
    if ((int)s->summon_fx[i].row != (int)s->pet_row[p]) return -1;
    return i;
}

BT_HD static unsigned short bt_pet_pick_fx(BT_DEVICE BtState* s, int p) {
    int want = (int)s->pet_row[p];
    for (int i = 0; i < s->n_summon_fx; i++) {
        if (!s->summon_fx[i].is_pet) continue;
        if ((int)s->summon_fx[i].row != want) continue;
        int claimed = 0;
        for (int q = 0; q < s->pet_n; q++) {
            if (q == p || !s->pet_alive[q] || !s->pet_ai_init[q]) continue;
            if ((int)s->pet_fx[q] == i) claimed++;
        }
        if (claimed < (int)s->summon_fx[i].copies) return (unsigned short)i;
    }
    return 0xFFFFu;
}

/* Curse-transformed per-instance fields; the row's base value is the uncursed
 * fallback when the instance could not be resolved (never a silent default). */
BT_HD static float bt_pet_zone_scale(BT_DEVICE BtState* s, int p) {
    int i = bt_pet_fx_of(s, p);
    if (i >= 0) return s->summon_fx[i].boost_zone_scale;
    return BT_ITEM_EFF_ROWS[s->pet_row[p]].boost_zone_scale;
}
BT_HD static float bt_pet_health_boost(BT_DEVICE BtState* s, int p) {
    int i = bt_pet_fx_of(s, p);
    if (i >= 0) return s->summon_fx[i].health_boost;
    return BT_ITEM_EFF_ROWS[s->pet_row[p]].health_boost;
}
BT_HD static float bt_pet_revive_duration(BT_DEVICE BtState* s, int p) {
    int i = bt_pet_fx_of(s, p);
    if (i >= 0) return s->summon_fx[i].revive_duration;
    return BT_ITEM_EFF_ROWS[s->pet_row[p]].revive_duration;
}

/* pet.gd reset_speed_stat: current_stats.speed = Stats.speed *
 * max(1 + stat_speed/100, 1), truncated by the int LiveStats field. */
BT_HD static float bt_pet_speed(BT_DEVICE BtState* s, BT_CONST_PTR(BtPetAI) A) {
    float f = 1.0f + s->speed_pct * 0.01f;
    if (f < 1.0f) f = 1.0f;
    return (float)(int)(A->speed * f);
}

/* Scapegoat: the pet's own hitbox/melee WeaponStats damage through
 * weapon_service.init_melee_pet_stats (scaling + pet percent damage). */
BT_HD static float bt_pet_melee_damage(BT_DEVICE BtState* s, BT_CONST_PTR(BtPetAI) A) {
    if (A->melee_wstats >= BT_NITEM_WSTATS) return 0.0f;
    BT_CONST_PTR(BtItemWeaponStats) W = &BT_ITEM_WSTATS[A->melee_wstats];
    int scaled = bt_scaling_damage(s, (int)W->damage, W->scaling_first, W->scaling_count, 0.0f);
    float d = bt_roundf((float)scaled * (1.0f + s->dmg_pct * 0.01f));
    return d < 1.0f ? 1.0f : d;
}

/* --- per-slot initialisation --------------------------------------------- */

/* Called the first frame a live pet slot is seen (or after the slot was reused
 * by a different pet row). Mirrors Pet.init -> init_current_stats/update_data
 * for the fields this module owns. */
BT_HD static void bt_pet_slot_init(BT_DEVICE BtState* s, int p, BT_CONST_PTR(BtPetAI) A) {
    s->pet_ai_init[p] = 1;
    s->pet_ai_row[p] = s->pet_row[p];
    s->pet_dead[p] = 0;
    s->pet_zone_in[p] = 0;
    s->pet_tree_in[p] = 0;
    s->pet_melee[p] = 0;
    s->pet_mad[p] = 0;
    s->pet_tx[p] = 0.0f; s->pet_ty[p] = 0.0f;
    s->pet_hurt_cd[p] = 0.0f;
    s->pet_melee_cd[p] = 0.0f;
    s->pet_float_hp[p] = 0.0f;
    s->pet_pangle[p] = 0.0f;
    s->pet_pphase[p] = 0.0f;
    s->pet_pradius[p] = (BT_ARENA_H * 0.5f) * A->patrol_rf;
    /* Only the behaviours whose source init() draws from Utils.rand_range do so
     * here (PatrolAroundTheMap.init phase, FollowRandPosAroundPlayer.init
     * range+offset); every other movement behaviour draws nothing at spawn. */
    if (A->move == BT_PET_MOVE_PATROL) {
        s->pet_pphase[p] = bt_rand_range(s, 0.0f, 6.2831853f);
    } else if (A->move == BT_PET_MOVE_FOLLOW_RAND_POS) {
        float actual = A->stop_dist + bt_rand_range(s, -A->rand_rand, A->rand_rand);
        s->pet_tx[p] = bt_rand_range(s, -actual, actual);
        s->pet_ty[p] = bt_rand_range(s, -actual, actual);
        if (!A->allow_within) {
            float l = bt_sqrtf(s->pet_tx[p] * s->pet_tx[p] + s->pet_ty[p] * s->pet_ty[p]);
            if (l > 1e-6f) {
                s->pet_tx[p] = s->pet_tx[p] / l * actual;
                s->pet_ty[p] = s->pet_ty[p] / l * actual;
            }
        }
    }
    /* unit.gd:77 (runs in Unit.init AFTER _movement_behavior.init): every Unit
     * rolls `_speed_rand_value = rand_range(-stats.speed_randomization *
     * accessibility_speed, +...) as int`.  Every pet's speed_randomization is 0,
     * so this is a DEGENERATE rand_range(0,0) -- but the global draw is still
     * consumed (exactly as the core models the player's own, see
     * bt_enter_wave).  Keep it after the movement-behaviour init draws so the
     * global stream stays aligned per spawned pet. */
    (void)bt_rand_range(s, 0.0f, 0.0f);
    /* Scapegoat.reset_health_stat -> round(get_base_health(wave)) as int, then
     * update_data multiplies max/current health by the instance health_boost. */
    float base = bt_roundf(A->health_base + A->health_per_wave * (float)(s->wave - 1));
    s->pet_maxhp[p] = (float)(int)(base * bt_pet_health_boost(s, p));
    if (s->pet_maxhp[p] < 1.0f) s->pet_maxhp[p] = 1.0f;
    s->pet_hp[p] = s->pet_maxhp[p];
    s->pet_revive_dur[p] = bt_pet_revive_duration(s, p);
    if (A->melee_wstats < BT_NITEM_WSTATS) {
        int cd = bt_apply_atk_spd_mod_cd((int)BT_ITEM_WSTATS[A->melee_wstats].cooldown, 0.0f);
        s->pet_melee_cd[p] = (float)cd / 60.0f;
    }
    /* lootworm.gd:27-39 init_stats(): _hitbox.set_damage(_current_weapon_stats
     * .damage) -- the base damage that _on_TreeArea2D_body_entered may then
     * mutate in place (pet_hit_dmg).  Also clear the deferred-hit target. */
    s->pet_hit_dmg[p] = bt_pet_melee_damage(s, A);
    s->pet_melee_tgt[p] = -1;
}

/* --- zone helpers -------------------------------------------------------- */

/* Is the player's body circle inside pet p's zone `z` (scaled by `scale`)?
 * This is the same discrete Area2D/CollisionShape2D overlap the core already
 * uses for entity births: circle vs the player's body circle. */
BT_HD static int bt_pet_player_in_zone(BT_DEVICE BtState* s, int p, BT_CONST_PTR(BtPetAI) A,
                                       int z, float scale) {
    float r = A->zone_r[z] * scale;
    if (!(r > 0.0f)) return 0;
    return bt_circle_overlap_offset(bt_v2(s->pet_x[p] + A->zone_x[z] * scale,
                                          s->pet_y[p] + A->zone_y[z] * scale),
                                    bt_v2zero(), r,
                                    bt_v2(s->px, s->py), bt_v2(0.0f, BT_PLAYER_BODY_Y),
                                    BT_PLAYER_BODY_R);
}

/* --- target selection ---------------------------------------------------- */

/* closest_enemy_target_behavior.gd: the nearest in-range enemy this pet type has
 * not already claimed, else (fallback_any) the first unclaimed enemy, else the
 * player when target_player_if_no_target. Returns the enemy slot or -1.
 * `claim[j]` holds the opcode of the pet that already took enemy j this frame
 * (the source keys PetService by pet_id, i.e. by pet class). */
BT_HD static int bt_pet_pick_enemy(BT_DEVICE BtState* s, BT_CONST_PTR(BtPetAI) A, int op,
                                   float px, float py, BT_THREAD unsigned char* claim) {
    float r = A->zone_r[BT_PET_Z_TARGET];
    int best = -1;
    float bd = 0.0f;
    for (int j = bt_enemy_next(s, 0, 1); j < (int)s->e_used; j = bt_enemy_next(s, j + 1, 1)) {
        if (!s->ealive[j] || s->eneutral[j]) continue;
        /* a charmed unit is on PETS_BIT: the source's `charmed` signal drops it
         * from ClosestEnemyTargetBehavior's target list (closest_enemy_target_
         * behavior.gd on_current_target_charmed/on_target_charmed). */
        if (bt_enemy_charmed(s, j)) continue;
        if ((int)claim[j] == op) continue;
        if (A->exclude_elite_boss && s->eelite[j]) continue;   /* Boss == elite+boss */
        BT_CONST_PTR(BtBodyPhysics) G = bt_kgeom(s, j);
        if (r > 0.0f &&
            !bt_circle_overlap_offset(bt_v2(px + A->zone_x[BT_PET_Z_TARGET],
                                            py + A->zone_y[BT_PET_Z_TARGET]),
                                      bt_v2zero(), r,
                                      bt_v2(s->ex[j], s->ey[j]), bt_v2(G->body_x, G->body_y),
                                      G->body_radius)) continue;
        float dx = s->ex[j] - px, dy = s->ey[j] - py;
        float d2 = dx * dx + dy * dy;
        if (r > 0.0f) {
            if (best < 0 || d2 < bd) { bd = d2; best = j; }
        } else if (best < 0) {
            best = j;   /* no acquisition zone: the source's enemy-list order */
            break;
        }
    }
    if (best < 0 && A->fallback_any) {
        /* closest_enemy_target_behavior.gd:39-43: with no target in range the pet
         * takes the FIRST entry of get_all_enemies(false), which is
         * `enemies + bosses` minus charmed_enemies (entity_spawner.gd:493-504).
         * The lists are append-ordered and erased on death, so the first live
         * candidate is the smallest (eelite != 0, e_act): every ordinary enemy
         * precedes every boss, and e_act is the spawn (push_back) sequence. */
        int bj = -1;
        for (int j = bt_enemy_next(s, 0, 1); j < (int)s->e_used; j = bt_enemy_next(s, j + 1, 1)) {
            if (!s->ealive[j] || s->eneutral[j]) continue;
            if (bt_enemy_charmed(s, j)) continue;   /* charmed: not a pet target */
            if ((int)claim[j] == op) continue;
            if (A->exclude_elite_boss && s->eelite[j]) continue;
            if (bj < 0) { bj = j; continue; }
            int kb = s->eelite[bj] != 0 ? 1 : 0;
            int kj = s->eelite[j] != 0 ? 1 : 0;
            if (kj < kb || (kj == kb && s->e_act[j] < s->e_act[bj])) bj = j;
        }
        best = bj;
    }
    if (best >= 0) claim[best] = (unsigned char)op;
    return best;
}

/* lootworm_target_behavior.gd: the nearest live tree, else the nearest gold. */
BT_HD static int bt_pet_pick_loot(BT_DEVICE BtState* s, float px, float py,
                                  BT_THREAD float* tx, BT_THREAD float* ty) {
    int best = -1;
    float bd = 0.0f;
    for (int j = bt_enemy_next(s, 0, 1); j < (int)s->e_used; j = bt_enemy_next(s, j + 1, 1)) {
        if (!s->ealive[j] || !s->eneutral[j]) continue;
        float dx = s->ex[j] - px, dy = s->ey[j] - py;
        float d2 = dx * dx + dy * dy;
        if (best < 0 || d2 < bd) { bd = d2; best = j; }
    }
    if (best >= 0) { *tx = s->ex[best]; *ty = s->ey[best]; return 1; }
    best = -1;
    for (int m = 0; m < (int)s->mat_used; m++) {
        if (!s->matalive[m]) continue;
        float dx = s->matx[m] - px, dy = s->maty[m] - py;
        float d2 = dx * dx + dy * dy;
        if (best < 0 || d2 < bd) { bd = d2; best = m; }
    }
    if (best >= 0) { *tx = s->matx[best]; *ty = s->maty[best]; return 1; }
    return 0;
}

/* --- Scapegoat: damage intake + self revive ------------------------------ */

/* scapegoat.gd _on_Hurtbox_area_entered + _physics_process. Enemy contact
 * Hitboxes (mask 4) and enemy projectiles (mask 16) are the only things the
 * pet's Hurtbox (mask 20) reacts to; a hit costs one point of damage and parks
 * the hurtbox for INVULNERABLE_COOLDOWN. The pet never leaves the pet array:
 * it stays dead while a player is inside HealingTriggeringZone and the revive
 * fills max_health / revive_duration health per second. */
BT_HD static void bt_pet_scapegoat(BT_DEVICE BtState* s, int p, BT_CONST_PTR(BtPetAI) A) {
    if (s->pet_hurt_cd[p] > 0.0f && !s->pet_dead[p]) {
        s->pet_hurt_cd[p] -= BT_DT;
        if (s->pet_hurt_cd[p] < 0.0f) s->pet_hurt_cd[p] = 0.0f;
    }
    int g = bt_pet_scene_geom((int)s->pet_row[p]);
    float hx = 0.0f, hy = 0.0f, hr = 0.0f;
    if (g >= 0) {
        hx = BT_ITEM_SCENES[g].hurt_x; hy = BT_ITEM_SCENES[g].hurt_y;
        hr = BT_ITEM_SCENES[g].hurt_radius;
    }
    if (hr > 0.0f && !s->pet_dead[p] && s->pet_hurt_cd[p] <= 0.0f) {
        int hit = 0;
        /* enemy contact Hitbox (same overlap the player contact test uses) */
        for (int i = bt_enemy_next(s, 0, 1); i < (int)s->e_used && !hit; i = bt_enemy_next(s, i + 1, 1)) {
            if (!s->ealive[i] || s->esup[i] || s->eneutral[i]) continue;
            BT_CONST_PTR(BtBodyPhysics) G = bt_kgeom(s, i);
            if (!G->has_hitbox) continue;
            if (!bt_circle_overlap_offset(bt_v2(s->ex[i], s->ey[i]), bt_v2(G->hit_x, G->hit_y),
                                          G->hit_radius,
                                          bt_v2(s->pet_x[p], s->pet_y[p]), bt_v2(hx, hy), hr))
                continue;
            hit = 1;
        }
        /* enemy projectile (layer 16); the projectile stops on the hit */
        if (!hit) {
            for (int i = 0; i < (int)s->pr_used && !hit; i++) {
                if (!s->pralive[i] || s->prfriendly[i]) continue;
                float dx = s->prx[i] - (s->pet_x[p] + hx);
                float dy = s->pry[i] - (s->pet_y[p] + hy);
                float rr = s->prrad[i] + hr;
                if (dx * dx + dy * dy > rr * rr) continue;
                s->pralive[i] = 0;   /* enemy_projectile.gd _on_Hitbox_hit_something */
                hit = 1;
            }
        }
        if (hit) {
            /* take_damage(1, args): the recovered Scapegoat passes the literal 1 */
            s->pet_hp[p] -= 1.0f;
            if (s->pet_hp[p] < 0.0f) s->pet_hp[p] = 0.0f;
            if (s->pet_hp[p] <= 0.0f) {
                s->pet_hp[p] = 0.0f;
                s->pet_dead[p] = 1;
                s->pet_float_hp[p] = 0.0f;
            }
            s->pet_hurt_cd[p] = BT_PET_INVULN_CD;
        }
    }
    if (s->pet_dead[p]) {
        int player_in = bt_pet_player_in_zone(s, p, A, BT_PET_Z_HEAL, 1.0f);
        if (!player_in) return;
        if (s->pet_hp[p] >= s->pet_maxhp[p]) {
            s->pet_dead[p] = 0;
            s->pet_float_hp[p] = 0.0f;
            s->pet_hp[p] = s->pet_maxhp[p];
            return;
        }
        float rate = 0.0f;
        if (s->pet_revive_dur[p] > 0.0f) rate = s->pet_maxhp[p] / s->pet_revive_dur[p];
        s->pet_float_hp[p] += rate * BT_DT;
        s->pet_hp[p] = bt_floorf(s->pet_float_hp[p] < s->pet_maxhp[p]
                                 ? s->pet_float_hp[p] : s->pet_maxhp[p]);
        /* scapegoat.gd:43-66: the revive only fires on the following frame --
         * _physics_process checks `current_stats.health >= max_stats.health`
         * BEFORE the else branch that fills the health, so the frame that
         * reaches max health keeps `dead = true` (with health at max) and
         * _rising() runs at the top of the NEXT frame. */
    }
}

/* --- Lootworm: the pet's own gold attract / pickup + tree melee ----------- */

/* lootworm.gd: the pet attracts gold into its own ItemAttractArea (setting
 * attracted_by, the same field the player's magnet uses), picks gold up in its
 * ItemPickUpArea (rolling its own double_chance, then the player's common
 * pickup effects via gold.pickup(player_index)) and melees only trees.
 * The player can still take a gold back when it is closer to the player than
 * the pet (player.gd _on_ItemAttractArea_area_entered). */
BT_HD static void bt_pet_lootworm(BT_DEVICE BtState* s, int p, BT_CONST_PTR(BtPetAI) A) {
    float px = s->pet_x[p], py = s->pet_y[p];
    /* Deferred hit delivery (unit.gd:610-611 `_on_Hurtbox_area_entered` ->
     * call_deferred("hurt_area_entered_deferred", hitbox), :536-608): the tree's
     * Hurtbox area_entered for a hitbox enabled at frame N is delivered at frame
     * N+1, so the take_damage lands one frame after the swing.  Apply a hit
     * armed last frame BEFORE anything else this frame. */
    if (s->pet_melee_tgt[p] >= 0) {
        int j = (int)s->pet_melee_tgt[p];
        s->pet_melee_tgt[p] = -1;
        if (j < BT_MAX && s->ealive[j] && s->eneutral[j]) {
            float dmg = s->pet_hit_dmg[p];
            BT_CONST_PTR(BtUnitKind) K = bt_kunit(s, j);
            float armor = K->armor + K->armor_grow * (float)(s->wave - 1);
            float taken = bt_roundf(dmg) - armor;
            if (taken < 1.0f) taken = 1.0f;
            s->ehp[j] -= taken;
            if (s->ehp[j] < 0.0f) s->ehp[j] = 0.0f;   /* unit.gd:305-306 */
            int neutral_death = bt_neutral_resolve_hit(s, j, 0, 1);
            if (s->ehp[j] <= 0.0f || neutral_death) bt_credit_kill(s, j, 1);
        }
    }
    /* TreeArea2D membership + the swing cadence (lootworm.gd:45-57,91-106). */
    float tr = A->zone_r[BT_PET_Z_TREE];
    int trees = 0;
    if (tr > 0.0f) {
        for (int j = bt_enemy_next(s, 0, 1); j < (int)s->e_used; j = bt_enemy_next(s, j + 1, 1)) {
            if (!s->ealive[j] || !s->eneutral[j]) continue;
            BT_CONST_PTR(BtBodyPhysics) G = bt_kgeom(s, j);
            if (!bt_circle_overlap_offset(bt_v2(px + A->zone_x[BT_PET_Z_TREE],
                                                py + A->zone_y[BT_PET_Z_TREE]),
                                          bt_v2zero(), tr,
                                          bt_v2(s->ex[j], s->ey[j]), bt_v2(G->body_x, G->body_y),
                                          G->body_radius)) continue;
            trees++;
            /* lootworm.gd:91-99 _on_TreeArea2D_body_entered: `_hitbox.damage`
             * is MUTATED IN PLACE (and persists for the rest of the wave) to
             * tree max_health / tree_damage_ratio once the tree is tougher than
             * damage * ratio.  It never decreases, so a running max models the
             * single persistent field. */
            if (A->tree_ratio > 0.0f && s->emax[j] > s->pet_hit_dmg[p] * A->tree_ratio)
                s->pet_hit_dmg[p] = s->emax[j] / A->tree_ratio;
        }
    }
    s->pet_tree_in[p] = (unsigned char)(trees > 0 ? 1 : 0);
    if (!s->pet_melee[p]) {
        s->pet_melee_cd[p] -= BT_DT;
        if (s->pet_melee_cd[p] <= 0.0f && trees > 0) {
            int hg = bt_pet_scene_geom((int)s->pet_row[p]);
            float bx = 0.0f, by = 0.0f, br = 0.0f;
            if (hg >= 0) {
                bx = BT_ITEM_SCENES[hg].hit_x; by = BT_ITEM_SCENES[hg].hit_y;
                br = BT_ITEM_SCENES[hg].hit_radius;
            }
            if (br > 0.0f) {
                s->pet_melee[p] = 1;    /* _hitbox.enable(): live for one frame */
                for (int j = bt_enemy_next(s, 0, 1); j < (int)s->e_used; j = bt_enemy_next(s, j + 1, 1)) {
                    if (!s->ealive[j] || !s->eneutral[j]) continue;
                    BT_CONST_PTR(BtBodyPhysics) G = bt_kgeom(s, j);
                    if (!bt_circle_overlap_offset(bt_v2(px + bx, py + by), bt_v2zero(), br,
                                                  bt_v2(s->ex[j], s->ey[j]),
                                                  bt_v2(G->hurt_x, G->hurt_y), G->hurt_radius))
                        continue;
                    /* arm the deferred hit (delivered next frame); the source
                     * clears ignored_objects and disables the hitbox next frame,
                     * so one Hurtbox pair per swing is hit. */
                    s->pet_melee_tgt[p] = (short)j;
                    break;
                }
            } else {
                s->pet_melee_cd[p] = 0.0f;   /* no Hitbox geometry: stay ready */
            }
        }
    } else {
        s->pet_melee[p] = 0;
        if (A->melee_wstats < BT_NITEM_WSTATS) {
            int cd = bt_apply_atk_spd_mod_cd((int)BT_ITEM_WSTATS[A->melee_wstats].cooldown, 0.0f);
            s->pet_melee_cd[p] = (float)cd / 60.0f;
        }
    }
}

/* Gold attract/pickup for every live lootworm. Runs once per frame after the
 * player's own magnet/pickup pass, which skips every gold a pet has claimed. */
BT_HD static void bt_pet_gold(BT_DEVICE BtState* s) {
    int ai = bt_pet_ai_index(BT_ITEMEFF_LOOTWORM);
    float pax = s->px, pay = s->py + BT_ATTRACT_Y;
    for (int m = 0; m < (int)s->mat_used; m++) {
        if (!s->matalive[m]) { s->mat_by[m] = 0; s->mat_attr[m] = 0; continue; }
        int pet_owned = s->mat_attr[m] == 2;
        if (pet_owned) {
            int q = (int)s->mat_by[m] - 1;
            if (q < 0 || q >= s->pet_n || !s->pet_alive[q] ||
                    s->pet_opcode[q] != BT_ITEMEFF_LOOTWORM) {
                s->mat_by[m] = 0; s->mat_attr[m] = 0;
                s->mat_arc[m][BT_LOOT_ARC_SPEED] = BT_LOOT_ITEM_ATTRACT_SPEED;
            } else {
                /* The core skips pet-owned items: advance their Item physics
                 * exactly once here, including the initial push-back/idle arc. */
                (void)bt_loot_item_step(&s->matx[m], &s->maty[m], s->mat_arc[m],
                    &s->mat_attr[m], 0, s->pet_x[q], s->pet_y[q], BT_DT);
                s->matspd[m] = s->mat_arc[m][BT_LOOT_ARC_SPEED];
            }
        }
        if (ai < 0 || s->mat_arc[m][BT_LOOT_ARC_MON] == 0.0f) continue;
        BT_CONST_PTR(BtPetAI) A = &BT_PET_AI[ai];
        float pdx = pax - s->matx[m], pdy = pay - s->maty[m];
        float pd2 = pdx * pdx + pdy * pdy;
        float player_radius = bt_scalar_attract_radius(s);
        for (int p = 0; p < s->pet_n; p++) {
            if (!s->pet_alive[p] || s->pet_opcode[p] != BT_ITEMEFF_LOOTWORM) continue;
            float ax = s->pet_x[p] + A->zone_x[BT_PET_Z_ATTRACT];
            float ay = s->pet_y[p] + A->zone_y[BT_PET_Z_ATTRACT];
            float dx = ax - s->matx[m], dy = ay - s->maty[m];
            float d2 = dx * dx + dy * dy;
            float ar = A->zone_r[BT_PET_Z_ATTRACT];
            if (s->mat_attr[m] == 2 && s->mat_by[m] == p + 1 &&
                    pd2 < d2 && pd2 <= player_radius * player_radius) {
                s->mat_by[m] = 0; s->mat_attr[m] = 1; s->mat_home[m] = 1;
                break;
            }
            /* lootworm.gd:59-65 _on_ItemAttractArea_area_entered: the pet only
             * claims a gold whose `attracted_by == null` -- it NEVER takes one
             * the player (mat_attr 1) or an evil_mob (mat_mob >= 0) already
             * attracts (only the player steals, player.gd:1026-1030, modelled
             * above).  The gold must also be monitorable (Item.monitorable, an
             * Area2D only sees monitorable peers) and inside the pet's own
             * ItemAttractArea. */
            if (s->mat_attr[m] == 0 && s->mat_mob[m] < 0 && ar > 0.0f && d2 <= ar * ar) {
                s->mat_by[m] = (unsigned char)(p + 1);
                s->mat_attr[m] = 2;
            }
            if (s->mat_attr[m] != 2 || s->mat_by[m] != p + 1) continue;
            float qx = s->pet_x[p] + A->zone_x[BT_PET_Z_PICKUP] - s->matx[m];
            float qy = s->pet_y[p] + A->zone_y[BT_PET_Z_PICKUP] - s->maty[m];
            float qr = A->zone_r[BT_PET_Z_PICKUP];
            if (!(qr > 0.0f) || qx * qx + qy * qy > qr * qr) continue;
            int value = (int)s->matval[m];
            if (bt_chance(s, s->pet_double[p])) value *= 2;
            value = bt_scalar_gold_pickup(s, value);
            bt_drain_death_events(s);
            s->materials += (float)value;
            bt_progress_add_xp(s, (float)value);
            s->matalive[m] = 0; s->matcount--;
            s->mat_by[m] = 0; s->mat_attr[m] = 0;
            break;
        }
    }
}

/* --- pet body contacts (GodotPhysics) ------------------------------------ */

/* Every pet of one of the eight BT_PET_AI classes is a real physics body in the
 * source: the scene root is entity.tscn's RigidBody2D in mode 2
 * (MODE_CHARACTER) with mass 1 (engine default) and custom_integrator, the body
 * CollisionShape2D is that scene's circle (BT_ITEM_SCENES -> the pet row's
 * body_radius/body_x/body_y) and the collision filter is the scene root's
 * RigidBody2D.collision_layer/collision_mask (BT_PET_AI -> collision_layer/
 * collision_mask), which the pet scenes override per scene:
 *
 *   pet.tscn (base)          layer 512, mask 640   walls 128 | pets 512
 *   lootworm, scapegoat      layer   0, mask 128   walls only
 *   doc_moth                 layer   0, mask   0   no body contacts at all
 *   bonk_dog, bot_o_mine,
 *   catling_gun, blazemander,
 *   ratzilla                 layer 512, mask 640   (no override)
 *
 * Structures (layer 2048, mask 2176) and enemies (layer 4, mask 132) never
 * satisfy bt_filter_pair against a pet, so the pet contact island is exactly
 * pets x pets plus pets x the four arena walls.  That island shares no body
 * with the enemy island, and the walls are STATIC (inv_mass 0, so
 * Body2DSW::apply_impulse / apply_bias_impulse are no-ops on them); running it
 * through its own bt_phys_step is therefore numerically identical to the
 * engine's single Step2DSW pass over everything, and keeps a pet crowd from
 * bloating the enemy world's pair table.
 *
 * The pet world holds its own copy of the four arena walls first, then the
 * BT_MAX_PET pet bodies, so pet slot p is bodies[BT_PET_BODY_BASE + p] and the
 * per-step body_count covers the walls plus the live pet prefix.  A frame with
 * no live pet body skips the contact step entirely (the walls are STATIC and
 * could not form a solvable pair on their own). */

#define BT_PET_WALLS 4
#define BT_PET_WALL_BASE 0             /* the four arena walls                */
#define BT_PET_BODY_BASE BT_PET_WALLS  /* body index of pet slot 0            */
/* The pet world's last body index is BT_PET_BODY_BASE + BT_MAX_PET - 1 and the
 * enemy world's is BT_MAX + 3; both must fit BtPhysWorld.bodies. */
#if (BT_PET_BODY_BASE + BT_MAX_PET) > BT_PHYS_MAX_BODIES
#error "BtPhysWorld too small for the pet world: raise BT_PHYS_MAX_BODIES to BT_PET_BODY_BASE + BT_MAX_PET"
#endif
#if (BT_MAX + 4) > BT_PHYS_MAX_BODIES
#error "BtPhysWorld too small for the enemy world: raise BT_PHYS_MAX_BODIES to BT_MAX + 4"
#endif

/* The persistent contact world this module owns inside BtState.  Written by the
 * state owner; see bt_pet_world_init below for the required arena-start call. */
#define BT_PET_WORLD(s) (&(s)->phys_pet)

/* Fill `b` with pet p's source body, starting at the pet's current entity
 * position with velocity ZERO (unit.gd owns linear_velocity through
 * _integrate_forces; the movement block below sets it).  A pet whose scene has
 * no body circle keeps radius 0 / filter 0, and a pet whose BT_PET_AI row is
 * missing (a structure -- it has no RigidBody2D scene at all) is never passed
 * here: bt_step_pet_motion only fills body slots for BT_PET_AI classes. */
BT_HD static void bt_pet_body_fill(BT_DEVICE BtState* s, BT_AS_WORLD BtPhysicsBody* b, int p) {
    int g = bt_pet_scene_geom((int)s->pet_row[p]);
    int ai = bt_pet_ai_index((int)s->pet_opcode[p]);
    float r = 0.0f, ox = 0.0f, oy = 0.0f;
    unsigned int layer = 0u, mask = 0u;
    if (g >= 0) {
        r = BT_ITEM_SCENES[g].body_radius;
        ox = BT_ITEM_SCENES[g].body_x;
        oy = BT_ITEM_SCENES[g].body_y;
    }
    if (ai >= 0) {
        layer = BT_PET_AI[ai].collision_layer;
        mask = BT_PET_AI[ai].collision_mask;
    }
    *b = bt_phys_body_circle(bt_v2(s->pet_x[p], s->pet_y[p]), bt_v2(ox, oy), r, 1.0f,
                             layer, mask, BT_PHYS_MODE_CHARACTER);
}

/* Build the pet contact world.  MUST run once per arena, after bt_phys_walls
 * (bt_clear_wave_summons): the four arena wall bodies are the enemy world's own
 * (same StaticBody2D rects, layer 128 / mask 0), so the two worlds cannot drift
 * apart, and a recycled pet body slot must not inherit any previous arena's
 * warm-started impulse.  The pet slots start inactive with fully initialised
 * bodies, and the engine's contact parameters come from the shared
 * bt_phys_world_init defaults. */
BT_HD static void bt_pet_world_init(BT_DEVICE BtState* s) {
    BT_AS_WORLD BtPhysWorld* w = BT_PET_WORLD(s);
    bt_phys_world_init(w, BT_DT, BT_PHYS_SOLVER_ITERATIONS);
    for (int i = 0; i < BT_PET_WALLS; i++) w->bodies[BT_PET_WALL_BASE + i] = s->phys.bodies[BT_MAX + i];
    for (int p = 0; p < BT_MAX_PET; p++) {
        BtPhysicsBody b = bt_phys_body_circle(bt_v2zero(), bt_v2zero(), 0.0f, 1.0f,
                                              0u, 0u, BT_PHYS_MODE_CHARACTER);
        b.active = 0;
        w->bodies[BT_PET_BODY_BASE + p] = b;
    }
    /* lootworm.gd:27-39 init_stats(at_wave_begin = true): the scene is rebuilt
     * each wave, so every pet's _hitbox.damage (pet_hit_dmg, mutated by
     * _on_TreeArea2D_body_entered during the wave) is reset to the weapon
     * stats damage here, exactly as reset_speed_stat/init_current_stats are. */
    for (int p = 0; p < BT_MAX_PET; p++) {
        int ai = bt_pet_ai_index((int)s->pet_opcode[p]);
        if (ai >= 0) s->pet_hit_dmg[p] = bt_pet_melee_damage(s, &BT_PET_AI[ai]);
        s->pet_melee_tgt[p] = -1;
    }
    /* walls only until bt_step_pet_motion finds a live pet body */
    w->body_count = BT_PET_BODY_BASE;
}

/* --- the per-frame step --------------------------------------------------- */

BT_HD static void bt_step_pet_motion(BT_DEVICE BtState* s) {
    /* 1. slot sweep: drop the state of dead slots, build newly-live ones. */
    for (int p = 0; p < s->pet_n; p++) {
        if (!s->pet_alive[p]) {
            s->pet_ai_init[p] = 0;
            s->pet_fx[p] = 0xFFFFu;
            s->pet_zone_in[p] = 0;
            s->pet_tree_in[p] = 0;
            s->pet_melee[p] = 0;
            s->pet_mad[p] = 0;
            continue;
        }
        int ai = bt_pet_ai_index((int)s->pet_opcode[p]);
        if (ai < 0) continue;                       /* structures/turrets do not move */
        if (!s->pet_ai_init[p] || (int)s->pet_ai_row[p] != (int)s->pet_row[p]) {
            s->pet_fx[p] = bt_pet_pick_fx(s, p);
            bt_pet_slot_init(s, p, &BT_PET_AI[ai]);
        }
    }

    /* 1b. pet body slots: only a live pet of a BT_PET_AI class is a
     * RigidBody2D body.  Every slot starts inactive and the per-pet loop below
     * fills the live ones; the island is stepped once at the end. */
    for (int p = 0; p < s->pet_n; p++)
        BT_PET_WORLD(s)->bodies[BT_PET_BODY_BASE + p].active = 0;

    /* 2. gold (lootworm attract/pickup), after the player's own magnet pass. */
    bt_pet_gold(s);

    /* 3. per-pet zones + motion. */
    s->zone_hp_regen_bonus = 0.0f;
    s->zone_double_lifesteal = 0;
    s->zone_hp_regen_threshold = 100;
    unsigned char claim[BT_MAX];
    for (int j = 0; j < (int)s->e_used; j++) claim[j] = 0xFFu;
    int n_body = 0;   /* highest live pet body slot + 1 (0 = no pet bodies) */

    for (int p = 0; p < s->pet_n; p++) {
        if (!s->pet_alive[p]) continue;
        int ai = bt_pet_ai_index((int)s->pet_opcode[p]);
        if (ai < 0) {
            /* Structures have no BT_PET_AI row and never move here: the
             * WanderingBot's global_position is advanced in the core's firing
             * block, AFTER the turret's targeting, so the turret targets from
             * the bot's PRE-move position (Turret._physics_process runs before
             * WanderingBot._physics_process in the node's call_multilevel). */
            continue;
        }
        BT_CONST_PTR(BtPetAI) A = &BT_PET_AI[ai];
        int op = (int)s->pet_opcode[p];
        float spd = bt_pet_speed(s, A);
        s->pet_speed[p] = spd;
        float px = s->pet_x[p], py = s->pet_y[p];
        /* the pet's contact body, at this frame's script position, velocity 0 */
        BT_AS_WORLD BtPhysicsBody* B = &BT_PET_WORLD(s)->bodies[BT_PET_BODY_BASE + p];
        bt_pet_body_fill(s, B, p);
        if (p + 1 > n_body) n_body = p + 1;

        /* --- DocMoth BoostZone: membership + the pet's own effect array ---- */
        if (A->zone_r[BT_PET_Z_BOOST] > 0.0f) {
            float scale = bt_pet_zone_scale(s, p);
            int inside = s->pet_dead[p] ? 0
                       : bt_pet_player_in_zone(s, p, A, BT_PET_Z_BOOST, scale);
            s->pet_zone_in[p] = (unsigned char)inside;
            if (inside) {
                for (int k = 0; k < (int)A->fx_count; k++) {
                    BT_CONST_PTR(BtPetZoneFx) F = &BT_PET_ZONE_FX[A->fx_first + k];
                    if (F->kind == BT_PET_FX_HP_REGEN_BONUS) {
                        s->zone_hp_regen_bonus += F->value;
                        s->zone_hp_regen_threshold = (int)F->value2;
                    } else if (F->kind == BT_PET_FX_DOUBLE_LIFESTEAL) {
                        s->zone_double_lifesteal = 1;
                    }
                }
            }
        }

        /* --- CatlingGun: PlayerTriggerZone (mask 2|512) stops the pet -------
         * catling_gun.gd:176-185,73-82: a Player or any pet that is not itself
         * a catling gun inside the zone freezes the pet and boosts it. */
        if (A->zone_r[BT_PET_Z_PLAYER] > 0.0f) {
            float pr = A->zone_r[BT_PET_Z_PLAYER];
            float cx = px + A->zone_x[BT_PET_Z_PLAYER];
            float cy = py + A->zone_y[BT_PET_Z_PLAYER];
            int close = bt_circle_overlap_offset(bt_v2(cx, cy), bt_v2zero(), pr,
                                                 bt_v2(s->px, s->py),
                                                 bt_v2(0.0f, BT_PLAYER_BODY_Y), BT_PLAYER_BODY_R);
            for (int q = 0; q < s->pet_n && !close; q++) {
                if (q == p || !s->pet_alive[q]) continue;
                if (s->pet_isstruct[q]) continue;   /* `body is Pet`, not a structure */
                if (s->pet_opcode[q] == BT_ITEMEFF_CATLING_GUN) continue;  /* is_catling_gun() */
                /* an Area2D body_entered needs the body's collision_layer to
                 * intersect the zone mask; the lootworm/scapegoat/doc_moth pet
                 * bodies are layer 0, so the PlayerTriggerZone never sees them. */
                int qai = bt_pet_ai_index((int)s->pet_opcode[q]);
                if (qai < 0 || (BT_PET_AI[qai].collision_layer & BT_PET_PLAYER_ZONE_MASK) == 0)
                    continue;
                int g = bt_pet_scene_geom((int)s->pet_row[q]);
                if (g < 0) continue;
                close = bt_circle_overlap_offset(bt_v2(cx, cy), bt_v2zero(), pr,
                                                 bt_v2(s->pet_x[q], s->pet_y[q]),
                                                 bt_v2(BT_ITEM_SCENES[g].body_x,
                                                       BT_ITEM_SCENES[g].body_y),
                                                 BT_ITEM_SCENES[g].body_radius);
            }
            s->pet_mad[p] = (unsigned char)close;
        }

        if (op == BT_ITEMEFF_SCAPEGOAT) bt_pet_scapegoat(s, p, A);
        if (op == BT_ITEMEFF_LOOTWORM) bt_pet_lootworm(s, p, A);
        /* _can_move == false: CatlingGun mad mode (mode STATIC) and a dead
         * Scapegoat (die() clears _can_move, _rising() restores it). */
        if (s->pet_mad[p] || s->pet_dead[p]) continue;

        /* --- movement behaviour (unit.gd get_movement/get_move_input) ------ */
        float tx = px, ty = py;
        int have = 0, target_is_player = 0;
        if (A->move == BT_PET_MOVE_PATROL) {
            /* patrol_around_the_map_movement_behavior.gd:19-24: get_movement()
             * returns `_current_target - _parent.global_position`, and
             * `_current_target` is advanced ONLY by this behavior node's own
             * _physics_process (line 22-24).  Godot 3 calls _physics_process in
             * scene-tree order, PARENT BEFORE CHILD (Main::iteration ->
             * main_loop->iteration() -> SceneTree::_notify_group_pause sorted by
             * Node::is_greater_than, main/main.cpp:2268), and the MovementBehavior
             * is a child of the pet (pet.tscn:211), so the unit's own
             * _physics_process (which reads get_movement()) runs BEFORE this
             * behavior's update: frame N moves using the target set at the END of
             * frame N-1, and the very first frame uses the un-set Vector2.ZERO.
             * Use the stored (previous-frame) target, then advance the circle for
             * the NEXT frame. */
            float rad = s->pet_pradius[p];
            tx = s->pet_tx[p];
            ty = s->pet_ty[p];
            have = 1;
            if (rad > 1.0f) {
                s->pet_pangle[p] += (spd / rad) * BT_DT;
                float ang = s->pet_pangle[p] + s->pet_pphase[p];
                s->pet_tx[p] = BT_ARENA_W * 0.5f + bt_cosf(ang) * rad;
                s->pet_ty[p] = BT_ARENA_H * 0.5f + bt_sinf(ang) * rad;
            }
        } else if (A->move == BT_PET_MOVE_RAND_POS) {
            /* target_rand_pos: a uniform point in [_min_pos, _max_pos], redrawn
             * when it is reached (within EQUALITY_PRECISION). */
            float dx = s->pet_tx[p] - px, dy = s->pet_ty[p] - py;
            if ((s->pet_tx[p] == 0.0f && s->pet_ty[p] == 0.0f) ||
                dx * dx + dy * dy <= BT_PET_RAND_EPS2) {
                s->pet_tx[p] = bt_rand_range(s, A->edge_x,
                                             BT_ARENA_W - A->edge_x > A->edge_x
                                             ? BT_ARENA_W - A->edge_x : A->edge_x);
                s->pet_ty[p] = bt_rand_range(s, A->edge_y,
                                             BT_ARENA_H - A->edge_y > A->edge_y
                                             ? BT_ARENA_H - A->edge_y : A->edge_y);
            }
            tx = s->pet_tx[p]; ty = s->pet_ty[p];
            have = 1;
        } else if (A->move == BT_PET_MOVE_FOLLOW_RAND_POS) {
            /* follow_rand_pos_around_player: the per-pet offset drawn at init. */
            tx = s->px + s->pet_tx[p]; ty = s->py + s->pet_ty[p];
            target_is_player = 1;
            have = 1;
        } else {
            /* follow_target (+ stop_close_to_target) */
            if (A->target == BT_PET_TARGET_PLAYER) {
                tx = s->px; ty = s->py;
                target_is_player = 1;
                have = 1;
            } else if (A->target == BT_PET_TARGET_ENEMY) {
                int tgt = bt_pet_pick_enemy(s, A, op, px, py, claim);
                if (tgt >= 0) { tx = s->ex[tgt]; ty = s->ey[tgt]; have = 1; }
                else if (A->target_player_if_none) {
                    tx = s->px; ty = s->py;
                    target_is_player = 1;
                    have = 1;
                }
            } else if (A->target == BT_PET_TARGET_LOOT) {
                have = bt_pet_pick_loot(s, px, py, &tx, &ty);
            }
        }
        if (!have) continue;

        /* FollowTargetMovementBehavior.get_movement: ZERO when the target is
         * the player and distance_to_target is met (bot_o_mine only). */
        float dx = tx - px, dy = ty - py;
        if (A->move == BT_PET_MOVE_FOLLOW_TARGET_STOP && target_is_player &&
            dx * dx + dy * dy < A->stop_dist * A->stop_dist) continue;
        if (A->move == BT_PET_MOVE_FOLLOW_RAND_POS && dx * dx + dy * dy <= BT_PET_RAND_EPS2)
            continue;

        float len = bt_sqrtf(dx * dx + dy * dy);
        if (len <= 1e-6f) continue;
        float step = spd * BT_DT;
        /* unit.gd _integrate_forces: when the target is within one step the
         * body snaps onto it with linear_velocity ZERO
         * (origin_to_target.length_squared() <= origin_to_next_origin.length_squared());
         * otherwise linear_velocity is this frame's movement input, shortened
         * to the zone-rect clamp when the step would leave the rect.  The
         * physics step below integrates that velocity and resolves the
         * pet-vs-pet / pet-vs-wall contacts on top of it. */
        if (step >= len) {
            B->pos = bt_v2(tx, ty);
            B->vel = bt_v2zero();
            continue;
        }
        float vx = dx / len * spd;
        float vy = dy / len * spd;
        float nx = px + vx * BT_DT;
        float ny = py + vy * BT_DT;
        if (nx < 0.0f) { nx = 0.0f; vx = (nx - px) / BT_DT; }
        if (nx > BT_ARENA_W) { nx = BT_ARENA_W; vx = (nx - px) / BT_DT; }
        if (ny < 0.0f) { ny = 0.0f; vy = (ny - py) / BT_DT; }
        if (ny > BT_ARENA_H) { ny = BT_ARENA_H; vy = (ny - py) / BT_DT; }
        B->vel = bt_v2(vx, vy);
    }

    /* 4. step the pet contact island (pet-vs-pet and pet-vs-wall bodies) with
     * the shared GodotPhysics step, then take the solved transforms back.  The
     * script velocities above were all set before the step, exactly as the
     * engine runs every pet's _integrate_forces before Step2DSW.  A Bull with
     * no pets leaves the world walls-only (no pair can be live: the walls are
     * STATIC), so the step is skipped and no pet/solver work runs at all. */
    if (n_body > 0) {
        BT_PET_WORLD(s)->body_count = BT_PET_BODY_BASE + n_body;
        bt_phys_step(BT_PET_WORLD(s));
        for (int p = 0; p < n_body; p++) {
            BT_AS_WORLD BtPhysicsBody* B = &BT_PET_WORLD(s)->bodies[BT_PET_BODY_BASE + p];
            if (!B->active) continue;
            s->pet_x[p] = B->pos.x;
            s->pet_y[p] = B->pos.y;
        }
    } else {
        BT_PET_WORLD(s)->body_count = BT_PET_BODY_BASE;
    }
}