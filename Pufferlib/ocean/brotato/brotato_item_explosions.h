/* brotato_item_explosions.h -- the item/pet/structure EXPLOSION lifecycle.
 *
 * This module owns the source's position-based explosion that is NOT the Bull's
 * player-centred explode_on_hit: the rocket turret's rocket, the landmines item
 * and the BotOMine's dropped landmines, and explode_on_consumable
 * (spicy_sauce).  Everything here is derived from
 *
 *   singletons/weapon_service.gd:511-531      explode(effect, args)
 *   projectiles/player_explosion.gd:45-71     start_explosion / set_area
 *   projectiles/explosion.tscn                Hitbox CircleShape2D 147.34 @ (0,3)
 *                                             + AnimationPlayer "explode" 0.05 s
 *                                             (Hitbox.disable at t = 0.05)
 *   entities/units/unit/unit.gd:552-578       a hit by a hitbox carrying an
 *                                             ExplodingEffect spawns the
 *                                             explosion at the TARGET's position
 *   entities/units/unit/unit.gd:257-386       take_damage + get_damage_value
 *   entities/structures/landmine/landmine.gd:29-46  explode() on Area2D body_exited
 *   entities/structures/landmine/landmine.tscn  trigger Area2D mask 6
 *                                             (player|enemy), CapsuleShape2D
 *                                             radius 24 height 32 @ (0,16) rot 90
 *   singletons/run_data.gd:1589-1632          handle_explode_effect(key, pos, ...)
 *   main.gd:694-695                           explode_on_consumable at the
 *                                             consumable's position
 *   items/all/turret_rocket/turret_rocket_exploding_effect.tres    scale 1.0
 *   items/all/landmines/landmine_exploding_effect.tres             scale 1.75
 *   items/all/spicy_sauce/spicy_sauce_effect_1.tres   chance 0.5 scale 1.25
 *
 * The generated graph already carries every number: BT_ITEM_EFF_ROWS holds the
 * ExplodingEffect rows (explode_chance / explode_scale / base_smoke_amount /
 * explosion_scene, and for the ItemExplodingEffect family the `stats` row),
 * BT_ITEM_WSTATS the weapon stats those rows scale, BT_ITEM_SCENES the explosion
 * scene's Hitbox circle (scene 71 = res://projectiles/explosion.tscn, radius
 * 147.34, offset (0,3)), and BT_ITEM_EFF_INDEX links a structure/turret effect
 * row to the nested ExplodingEffect it hands out (effects[] / explosion_effect).
 *
 * NOT a straight-line bullet or a Bull-centred substitute: an explosion is a
 * POINT with a radius that damages every enemy whose Hurtbox circle it overlaps
 * exactly ONCE, for a bounded number of physics frames, and the projectile that
 * carried it deals no direct damage (unit.gd:552-578 sets is_exploding and
 * skips take_damage).
 *
 * Pooling: the source's `main._explosions` is an unbounded node pool keyed by
 * the explosion scene; here it is a fixed POD array in BtState with a DECLARED
 * capacity (BT_MAX_EXPL = one slot per live projectile + live pet/structure
 * entity -- the only two producers).  There is no per-step allocation and no
 * recursion.  A spawn with no free slot FAILS EXPLICITLY: it returns -1 and
 * increments `expl.n_overflow`; a LIVE explosion is never overwritten.  An
 * ExplodingEffect whose explosion_scene has no BT_ITEM_SCENES row is never given
 * an invented radius either: it is counted in `expl.n_missing` and refused.
 *
 * CORE INTEGRATION (done in brotato_core.h; state lives in brotato_state.h):
 *   brotato_state.h: BT_EXPL_FRAMES, BT_MAX_EXPL (BT_MAX_PROJ + BT_MAX_PET),
 *     BT_EXPL_HIT_WORDS ((BT_MAX + 31) / 32), BtExplosion, BtItemExplosions,
 *     BtItemExplosions expl, unsigned char pet_lm_pressed[BT_MAX_PET],
 *     unsigned short pr_expl[BT_MAX_PROJ].
 *   bt_core_reset:   bt_item_expl_reset(s) + zeroes pet_lm_pressed/pr_expl
 *   bt_clear_wave_summons (every wave start): bt_item_expl_reset(s) + zeroes
 *                    pet_lm_pressed (a fresh main.tscn per wave has an empty
 *                    main._explosions pool)
 *   bt_spawn_summon_row: pet_lm_pressed[p] = 0
 *   bt_pet_compact:      pet_lm_pressed[w] = pet_lm_pressed[i]
 *   bt_item_expl_landmine_tick_cand: a triggered mine calls
 *                    bt_struct_pool_release(s, p, 0) instead of writing
 *                    pet_alive[p] = 0, so its node re-enters landmine.tscn's
 *                    pool 1.0 s later (brotato_structure_pool.h)
 *   bt_spawn_projectile: pr_expl[pj] = BT_ITEM_EFF_NONE
 *   bt_apply_item: BT_ITEMEFF_EXPLODING / _ITEM_EXPLODING /
 *     _ITEM_EXPLODING_WHEN_BELOW_HP ALL register through the effect-row bucket
 *     (bt_scalar_bucket_register): EXPLODE_ON_CONSUMABLE / EXPLODE_ON_DEATH feed
 *     bt_item_expl_on_key, and the player-centred EXPLODE_ON_HIT /
 *     EXPLODE_WHEN_BELOW_HP feed bt_item_expl_player_hit.  There is no separate
 *     `extra_explosions` count: every entry keeps its own row, stats and curse
 *     modifier.
 *   bt_mark_enemy_dead: bt_item_expl_on_key(s, BT_ISTR_EXPLODE_ON_DEATH, x, y)
 *     at the dying enemy's own position (main.gd:534-535), before spawn_loot.
 *   bt_explode: the shared bt_item_expl_hit_enemy primitive replaces its
 *     per-target math (one enemy-explosion formula in the core).
 *   bt_core_step:
 *     - friendly-projectile impact: bt_item_expl_projectile_hit() replaces the
 *       direct hit when the shot carries an ExplodingEffect; the plain hit goes
 *       through the same primitive.
 *     - pet-fire block: pr_expl[pj] = bt_item_expl_shot_effect(s, p); structure
 *       rows (landmines) are skipped entirely -- a Structure never shoots.
 *     - after the fire block: bt_item_expl_landmine_tick(s).
 *     - consumable pickup: bt_item_expl_on_consumable(s, conx[i], cony[i]).
 *     - after the burn tick: bt_item_expl_tick(s, &r).
 * The header also forward-declares bt_enemy_incoming_damage (G1,
 * brotato_enemy_modifiers.h, included before this one) so its include order is
 * free.
 *
 * There is no in-header guard for a missing call: a missing call is a missing
 * call, never a silent re-initialisation.
 */
#pragma once

/* brotato_enemy_modifiers.h (EnemyModifiers slice, G1) -- forward declaration so
 * this header needs no include ordering: the definition is static and located
 * later in the same translation unit, exactly like bt_scalar_explosion_damage_mult
 * is for bt_explode. */
BT_HD static float bt_enemy_incoming_damage(BT_DEVICE BtState* s, int slot, float value);

/* brotato_endless.h (endless-mode run scaling): the max(1, E*0.2) divisor the
 * Giant Belt / Greek Fire percent-of-current-health bonuses use past wave 20.
 * Forward-declared like the sibling above; brotato_core.h includes
 * brotato_endless.h before this header. */
BT_HD static float bt_endless_hp_pct_divisor(int wave);

/* brotato_combat_audio.h (CombatAudioRuntime).  Forward-declared so this header
 * keeps its free include order, exactly like bt_enemy_incoming_damage: the
 * definition is static and lives in the same translation unit.  The
 * unit.gd:298-301 hurt/crit sound picks and their SoundManager2D submission. */
BT_HD static void bt_combat_audio_enemy_hit(BT_DEVICE BtState* s, int kind, int neutral,
                                            int is_crit);
/* brotato_combat_audio.h: the player-centre explosion's own stats shooting sound
 * (player.gd:559-560), addressed by the effect's BT_ITEM_WSTATS row. */
BT_HD static void bt_combat_audio_player_explosion_row(BT_DEVICE BtState* s, int stats_row);
/* brotato_combat_audio.h: the PlayerExplosion NODE's own explosion_sounds pick
 * (player_explosion.gd:28-32), addressed by its BT_ITEM_SCENES row. */
BT_HD static void bt_combat_audio_explosion_node(BT_DEVICE BtState* s, int scene_row);

/* Candidate-hit bitset width per explosion (bt_item_expl_scan / _tick_cand).
 * SLOT-keyed: target slot j is bit (j&31) of word (j>>5).  Slot j is unique
 * within one tick and the tick's own uid/hit test still gates the hit, so a slot
 * bit is exactly as precise as a uid-keyed set -- and 48x smaller (a uid-keyed
 * set would need BT_EXPL_HIT_WORDS = 194 words per explosion). */
#define BT_EXPL_CAND_WORDS ((BT_MAX + 31) / 32)

/* brotato_weapon_effects.h (WeaponFx): the weapon hooks an explosion KILL is
 * credited to (unit.gd:569 connects the spawned explosion to its firing Weapon).
 * Forward-declared (definition later in the TU, like the audio hooks); attack_id
 * is passed -1 so the per-attack tally is skipped, exactly as the source's
 * explosion hitbox does. */
BT_HD static void bt_wfx_on_weapon_kill(BT_DEVICE BtState* s, BT_DEVICE BtWeaponFxState* fx,
                                        int weapon_slot, unsigned int uid, int attack_id,
                                        BT_CONST_PTR(unsigned short) rows, int n_rows,
                                        BT_DEVICE const float* row_curse);
BT_HD static int bt_wfx_on_weapon_hit(BT_DEVICE BtState* s, BT_DEVICE BtWeaponFxState* fx,
                                      int slot_hint, unsigned int uid, int attack_id,
                                      BT_CONST_PTR(unsigned short) rows, int n_rows,
                                      BT_DEVICE const float* row_curse);

/* ---------------------------------------------------------------------------
 * Explosion pool layouts and capacities live in brotato_state.h.
 *
 * Field semantics:
 *   x,y        explosion root position (args.pos)
 *   ox,oy      scaled Hitbox circle centre, root-local
 *   rad        scaled Hitbox circle radius
 *   dmg        Hitbox.damage (init-time scaled, pre-crit)
 *   crit_*     Hitbox.crit_chance (rolled per target hit) / Hitbox.crit_damage
 *   burn_*     HitboxArgs.burning_data.chance / .damage / .duration (seconds)
 *   scale      node scale (geometry + smoke amount)
 *   age        0 = spawned this frame (collision enable is deferred), ++ per step
 *   hit        hit-once-per-target bitset
 *   n_spawned / n_overflow / n_missing   exact counters, never a silent drop
 * ------------------------------------------------------------------------- */

/* explosion.tscn AnimationPlayer "explode": step 0.05 s, last key at t = 0.05
 * calls Hitbox.disable() -> the Hitbox is enabled for 3 physics frames at 60 Hz.
 * The spawn frame is NOT one of them: start_explosion() enables the collision
 * with set_deferred, which takes effect on the next physics step.
 * (BT_EXPL_FRAMES itself is a parent-owned brotato_state.h constant.) */

/* landmine.tscn trigger `Area2D` + `Area2D/CollisionShape2D`:
 *   collision_mask = 6 (BT_LAYER_PLAYER 2 | BT_LAYER_ENEMY 4)
 *   shape = CapsuleShape2D(radius = 23.9999, height = 32.0)
 *   position = (0, 16), rotation = 1.5708 (90 deg -> the capsule's segment runs
 *   along root-local X).
 * Godot 3.x CapsuleShape2D does not clamp height (capsule_shape_2d.cpp), and
 * GodotPhysics2D's CapsuleShape2DSW::get_supports offsets its two cap circles by
 * +/- height * 0.5 (shape_2d_sw.cpp:373-397), so the shape is the Minkowski sum
 * of a segment of half-length height/2 and a disc of `radius` -- the same
 * convention extract_gameplay.py uses for a capsule's (radius, height*0.5). */
#define BT_LM_TRIG_R 24.0f     /* CapsuleShape2D.radius */
#define BT_LM_TRIG_HALF 16.0f  /* CapsuleShape2D.height * 0.5 = the segment half-length */
#define BT_LM_TRIG_X 0.0f
#define BT_LM_TRIG_Y 16.0f

/* ---------------------------------------------------------------------------
 * Generated-table lookups
 * ------------------------------------------------------------------------- */

/* ExplodingEffect / ItemExplodingEffect / ItemExplodingWhenBelowHPEffect are the
 * three opcodes that spawn this explosion (brotato_core.h bt_eff_is_exploding
 * names the same set). */
BT_HD static int bt_item_expl_opcode(int op) {
    return op == BT_ITEMEFF_EXPLODING || op == BT_ITEMEFF_ITEM_EXPLODING ||
           op == BT_ITEMEFF_ITEM_EXPLODING_WHEN_BELOW_HP;
}

/* The nested ExplodingEffect row of `E`'s effects[]/sub_effects[] array, or
 * BT_ITEM_EFF_NONE.  effects[0] is the explosion a turret hands to its
 * projectiles (turret.gd:115 `_spawn_projectile_args.effects = effects`) and the
 * one a landmine fires (landmine.gd:31 `effects[0]`); the BotOMine carries its
 * landmine StructureEffect in the single `child` slot instead (bot_o_mine_effect
 * .gd:7-8), whose own nested row is resolved when that row is instantiated. */
BT_HD static unsigned short bt_item_expl_nested_row(BT_CONST_PTR(BtItemEff) E) {
    if (E->sub_count == BT_ITEM_EFF_NONE) return BT_ITEM_EFF_NONE;
    for (int i = 0; i < (int)E->sub_count; i++) {
        int idx = (int)E->sub_first + i;
        if (idx < 0 || idx >= BT_NITEM_EFF_INDEX) break;
        unsigned short row = BT_ITEM_EFF_INDEX[idx];
        if (bt_item_expl_opcode((int)BT_ITEM_EFF_ROWS[row].opcode)) return row;
    }
    return BT_ITEM_EFF_NONE;
}

/* BT_ITEM_SCENES row of an explosion scene (-1 when the generator did not
 * resolve it).  Requires a hit circle: the explosion's damage area IS the
 * Hitbox's CircleShape2D scaled by the node scale. */
BT_HD static int bt_item_expl_geom(unsigned short scene_istr) {
    if (scene_istr == BT_ISTR_NONE) return -1;
    for (int q = 0; q < BT_NITEM_SCENES; q++)
        if (BT_ITEM_SCENES[q].scene == scene_istr && BT_ITEM_SCENES[q].hit_radius > 0.0f)
            return q;
    return -1;
}

/* player_explosion.gd:65-71 set_area(p_area):
 *   explosion_scale = max(0.1, p_area + p_area * explosion_size/100)
 * and the node scale multiplies explosion.tscn's Hitbox CircleShape2D radius
 * (147.34) and its (0,3) offset. */
BT_HD static float bt_item_expl_node_scale(BT_DEVICE BtState* s, float effect_scale) {
    float sc = effect_scale * (1.0f + s->scalar_effect[BT_ISTR_EXPLOSION_SIZE] * 0.01f);
    return sc < 0.1f ? 0.1f : sc;
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

BT_HD static void bt_item_expl_reset(BT_DEVICE BtState* s) {
    for (int i = 0; i < (int)s->expl.used; i++) {
        BT_DEVICE BtExplosion* e = &s->expl.e[i];
        e->x = 0.0f; e->y = 0.0f; e->ox = 0.0f; e->oy = 0.0f; e->rad = 0.0f;
        e->dmg = 0.0f; e->crit_chance = 0.0f; e->crit_dmg = 0.0f;
        e->burn_chance = 0.0f; e->burn_dmg = 0.0f; e->burn_dur = 0.0f;
        e->scale = 0.0f; e->age = 0;
        e->hit.n = 0;
        e->hit_overflow = 0;
        e->alive = 0;
        e->has_src = 0; e->src_uid = 0u;
        e->src_fx_first = 0; e->src_fx_count = 0; e->src_slot = 0;
        /* a fresh main.tscn starts with an EMPTY scene-keyed node cache: every
         * node is instantiated on demand again, so each one sounds again. */
        s->expl.cache_res[i] = 0;
        s->expl.cache_row[i] = 255;
        s->expl.cache_seq[i] = 0u;
        s->expl.snd_pending[i] = 0;
    }
    s->expl.used = 0;
    s->expl.seq_next = 0u;
    s->expl.n_spawned = 0;
    s->expl.n_overflow = 0;
    s->expl.n_missing = 0;
    /* player.gd:123/787-800 init_exploding_stats(true): every owned copy of an
     * explode_when_below_hp row starts the wave with ONE trigger.  The array is
     * indexed by the copy's ordinal in bucket order (bt_item_expl_player_hit). */
    for (int i = 0; i < BT_NITEM_EFF_ROWS; i++) s->expl.expl_trig[i] = 1;
    s->expl.expl_trig_overflow = 0;
}

/* cache_row is one byte, so the scene-row space must fit (BT_NITEM_SCENES is
 * generated); widen the field to unsigned short if a future source grows past it. */
#if BT_NITEM_SCENES > 255
#error "brotato_item_explosions.h: expl.cache_row is one byte but BT_NITEM_SCENES > 255"
#endif

/* Projectiles/player_explosion.gd sound_db_mod of the CHARACTER explosion row
 * (items/characters/bull/bull_effect_4.tres:20 = -15).  Only its `> -50` gate
 * matters (player_explosion.gd:30). */
#define BT_ITEM_EXPL_CHAR_SOUND_DB (-15)

/* main.get_node_from_pool (main.gd:1649-1666) + add_node_to_pool (:1678-1699):
 * the source pool is a Dictionary keyed by the SCENE instance id holding, per
 * scene, a resident stack of returned nodes -- pop_back on reuse (LIFO) and a
 * fresh instance() only when that scene's stack is empty.  So:
 *   1. the free slot holding the most recently RETURNED node of THIS scene;
 *   2. else a slot that has never been instantiated (a fresh node);
 *   3. else the pool is full: explicit failure, and another scene's resident
 *      node is NEVER taken. */
BT_HD static int bt_item_expl_slot(BT_DEVICE BtState* s, int scene_row) {
    int best = -1;
    unsigned int best_seq = 0u;
    for (int i = 0; i < (int)s->expl.used; i++) {
        if (s->expl.e[i].alive) continue;
        if (!s->expl.cache_res[i] || (int)s->expl.cache_row[i] != scene_row) continue;
        if (best < 0 || s->expl.cache_seq[i] > best_seq) { best = i; best_seq = s->expl.cache_seq[i]; }
    }
    if (best >= 0) return best;
    for (int i = 0; i < BT_MAX_EXPL; i++)
        if (!s->expl.cache_res[i]) return i;
    return -1;
}

/* WeaponService.explode (singletons/weapon_service.gd:511-531) with the scene
 * geometry already resolved:
 *   instance = pool[scene] or instance()               (bt_item_expl_slot)
 *   instance.global_position = args.pos
 *   instance.sound_db_mod = effect.sound_db_mod        (the node's own SFX gate)
 *   instance.set_damage(args) / set_area(effect.scale) / start_explosion()
 * `scene_row` is the BT_ITEM_SCENES row of the explosion scene and `scale` the
 * effect's own scale (bt_item_expl_node_scale applies the explosion_size stat).
 * `dmg` is the already init-time-scaled Hitbox.damage the CALLER computed.
 * Explicit failures, both counted and both returning -1:
 *   - scene_row has no geometry        -> n_missing++ (never an invented radius);
 *   - the pool cannot host the node    -> n_overflow++ (a live node is never
 *                                          overwritten and another scene's
 *                                          resident node is never taken). */
BT_HD static int bt_item_expl_spawn_geom(BT_DEVICE BtState* s, int scene_row, float scale,
                                         float x, float y, float dmg,
                                         float crit_chance, float crit_dmg,
                                         float burn_chance, float burn_dmg, float burn_dur,
                                         int sound_db_mod) {
    if (scene_row < 0 || scene_row >= BT_NITEM_SCENES ||
        BT_ITEM_SCENES[scene_row].hit_radius <= 0.0f) {
        s->expl.n_missing++;
        return -1;
    }
    float node_scale = bt_item_expl_node_scale(s, scale);
    int pj = bt_item_expl_slot(s, scene_row);
    if (pj < 0) { s->expl.n_overflow++; return -1; }
    int fresh = !s->expl.cache_res[pj];
    if (fresh) {
        /* A node instantiated by this spawn is FRESH: it sounds on its first
         * physics frame (and never again, even when the pool returns it). */
        s->expl.cache_row[pj] = (unsigned char)scene_row;
    }
    s->expl.cache_res[pj] = 1;
    if (pj >= (int)s->expl.used) s->expl.used = (unsigned short)(pj + 1);
    s->expl.snd_pending[pj] = (fresh && sound_db_mod > -50) ? 1 : 0;
    BT_DEVICE BtExplosion* e = &s->expl.e[pj];
    e->x = x; e->y = y;
    e->rad = BT_ITEM_SCENES[scene_row].hit_radius * node_scale;
    e->ox = BT_ITEM_SCENES[scene_row].hit_x * node_scale;
    e->oy = BT_ITEM_SCENES[scene_row].hit_y * node_scale;
    e->dmg = dmg;
    e->crit_chance = crit_chance;
    e->crit_dmg = crit_dmg;
    e->burn_chance = burn_chance;
    e->burn_dmg = burn_dmg;
    e->burn_dur = burn_dur;
    e->scale = node_scale;
    e->age = 0;
    /* weapon.gd Hitbox.ignored_objects keys the source's aim on the NODE, so the
     * hit set is keyed on bt_object_uid(s, j) exactly like BtWProj.ignored: a
     * pooled node keeps its identity, a FRESH object in the same actor slot does
     * not, and an enemy killed and respawned into another slot between two frames
     * of this 3-frame window is therefore hit.  Only the words this explosion
     * actually touched are cleared before reuse (bt_wproj_clear_targets). */
    e->hit.n = 0;
    e->has_src = 0; e->src_uid = 0u;
    e->src_fx_first = 0; e->src_fx_count = 0; e->src_slot = 0;
    e->alive = 1;
    s->expl.n_spawned++;
    return pj;
}

/* unit.gd:569: the weapon that fired a shot carrying this explosion connects the
 * spawned node's hit_something/killed_something to itself, so a kill by the
 * explosion is credited to that weapon's wave tally / GainStatEveryKilledEnemies
 * rows (weapon.gd:203-230).  The weapon-projectile spawn calls this right after
 * bt_item_expl_spawn; every other source leaves has_src 0 (no weapon credit).
 * `fx_first`/`fx_count` index BT_WEAPON_EFF_INDEX exactly like bt_wproj's
 * rows/n_rows, and `slot` is the firing weapon's slot in s->wpn_fxcurse. */
BT_HD static void bt_item_expl_set_source(BT_DEVICE BtState* s, int pj, unsigned int uid,
                                          int fx_first, int fx_count, int slot) {
    if (pj < 0 || pj >= BT_MAX_EXPL) return;
    BT_DEVICE BtExplosion* e = &s->expl.e[pj];
    e->src_uid = uid;
    e->src_fx_first = (unsigned short)fx_first;
    e->src_fx_count = (unsigned short)fx_count;
    e->src_slot = (unsigned short)slot;
    e->has_src = 1;
}

/* `effect_row` is the ExplodingEffect row: its explosion_scene and explode_scale
 * are read here, and its sound_db_mod gates the node's own SFX. */
BT_HD static int bt_item_expl_spawn(BT_DEVICE BtState* s, unsigned short effect_row,
                                    float x, float y, float dmg,
                                    float crit_chance, float crit_dmg,
                                    float burn_chance, float burn_dmg, float burn_dur) {
    if (effect_row >= BT_NITEM_EFF_ROWS) { s->expl.n_missing++; return -1; }
    BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[effect_row];
    int g = bt_item_expl_geom(E->explosion_scene);
    if (g < 0) { s->expl.n_missing++; return -1; }
    return bt_item_expl_spawn_geom(s, g, E->explode_scale, x, y, dmg, crit_chance, crit_dmg,
                                   burn_chance, burn_dmg, burn_dur, (int)E->sound_db_mod);
}

/* One enemy hit by an explosion hitbox: unit.gd:257-386 take_damage ->
 * get_damage_value, in the source's own order.  This is THE single enemy
 * explosion damage formula in the core -- bt_explode (the Bull explosion) calls
 * it too, so there is no second copy to disagree with.
 *
 *   1. base    = Hitbox.damage as the caller computed it (pre-crit)
 *   2. G1      bt_enemy_incoming_damage(s, j, base)  (brotato_enemy_modifiers.h)
 *              boss.gd:52-58 wraps Unit.take_damage: for an elite/boss while
 *              damage_against_bosses is owned the multiplier lands on the
 *              PRE-crit value, with the source's int() truncation.
 *   3. crit    is_crit (the caller's per-target roll) -> round(v * crit_dmg)
 *              (unit.gd:295-297).
 *   4. bonus   round(dmg * (1 + bonus_damage_against_targets_above_hp/100))
 *              (get_damage_value's total_bonus_damage term).
 *   5. ARMOR   max(1, dmg - armor)   (get_damage_value: `result.value =
 *              max(1, dmg_value - current_stats.armor)`; armor_applied is TRUE
 *              for a hitbox hit -- take_damage_args.gd default -- and FALSE only
 *              on the burn tick).  This is FLAT subtraction, NOT the
 *              player-facing bt_armor_mult coefficient: enemy armor is an
 *              absolute reduction.
 *   6. adds    giant_crit (crit only) and burning_enemy_hp_percent_damage (only
 *              while the target burns): flat, post-armor, ref_hp * value/factor.
 *   7. on hurt remove_speed (enemy.gd _on_hurt), the hitbox's own burning_data
 *              applied with its chance, and ChanceStatDamageEffect's proc (also
 *              routed through G1).
 *   8. kill    bt_neutral_resolve_hit (brotato_neutral_events.h) runs for a
 *              neutral target -- its 8-hit tally / one_shot_trees rule, never
 *              bypassed by the ehp test -- and bt_credit_kill(s, j, 1) queues the
 *              death callbacks iteratively for enemies and trees alike.
 *
 * `from_player_index` / `has_from` are the source's TakeDamageArgs values
 * (neutral.gd reads them): every explosion in this core is player-sourced, so
 * the callers pass (0, 1); the burn tick's (armor_applied=false) path passes
 * (-1, 0).
 * `killed` increments once when the target dies.  Returns the damage dealt. */

/* EffectsManager._on_weapon_did_crit: a crit (or one-shot) hit whose Hitbox
 * carries a `from` makes the VICTIM emit `crit_effect(hitbox.from)`
 * (unit.gd:312-315), and every Unit is connected to the manager (main.gd:905),
 * which plays `particles/critical_particles.tscn` at the weapon
 * (effects_manager.gd:42-58 play()).  That play is deliberately NOT gated by
 * ProgressData.settings.visual_effects (unlike the blood hit particles at
 * :52-53 and the gold pickup at :71-72, which the bridge's visual-effect
 * settings do suppress), and its CPUParticles2D is a POOLED instance created at
 * runtime, so the bridge's particle suppression -- which clears `emitting` on
 * the scene's particle nodes -- leaves this one live.
 *
 * The scene is amount 1 + one_shot + explosiveness 1.0 on a POINT emission
 * shape with a color_initial_ramp and no curve parameters, so exactly one
 * particle restarts and the pinned engine's per-restart draw order
 * (scene/2d/cpu_particles_2d.cpp:842-891) consumes exactly nine global words:
 *   :864 Particle.seed = Math::rand();
 *   :866 angle_rand / :867 scale_rand / :868 hue_rot_rand / :869 anim_offset_rand
 *        = Math::randf();
 *   :872 start_color_rand = color_initial_ramp->get_color_at_offset(Math::randf());
 *   :877 the spread draw: (Math::randf() * 2 - 1) * PI * spread / 180
 *   :879 the initial-velocity lerp draw (evaluated even at zero randomness)
 *   :890 p.lifetime = lifetime * (1 - Math::randf() * lifetime_randomness)
 * The rest of the restart (curve lookups, scale/rotation arithmetic, the POINT
 * emission shape) draws nothing.  Measured against the rng-diag engine on the
 * stock-wave capture: these nine words are the whole difference between the
 * game's and the core's global draws on the frame of the first lethal crit. */
BT_HD static void bt_fx_crit_particles(BT_DEVICE BtState* s) {
    (void)bt_rng_next(&s->rng);     /* Particle.seed (:864)                   */
    (void)bt_math_randf(&s->rng);   /* angle_rand (:866)                      */
    (void)bt_math_randf(&s->rng);   /* scale_rand (:867)                      */
    (void)bt_math_randf(&s->rng);   /* hue_rot_rand (:868)                    */
    (void)bt_math_randf(&s->rng);   /* anim_offset_rand (:869)                */
    (void)bt_math_randf(&s->rng);   /* color_initial_ramp offset (:872)       */
    (void)bt_math_randf(&s->rng);   /* spread (:877)                          */
    (void)bt_math_randf(&s->rng);   /* initial-velocity lerp (:879)           */
    (void)bt_math_randf(&s->rng);   /* lifetime randomness (:890)             */
}

BT_HD static float bt_item_expl_hit_enemy(BT_DEVICE BtState* s, int j, float base, int is_crit,
                                          float crit_dmg,
                                          float burn_chance, float burn_dmg, float burn_dur,
                                          float proc_chance, float proc_dmg,
                                          int from_player_index, int has_from,
                                          BT_THREAD int* killed, BT_THREAD BtEnemyHitContext* ctx) {
    if (ctx) {
        if (bt_wfx_force_crit_when_burning(ctx->rows, ctx->count, s->eburn[j] > 0.0f))
            is_crit = 1;
        ctx->was_crit = is_crit;
    }
    if (bt_enemy_charmed(s, j) || bt_enemy_charm_absorb(s, j)) return 0.0f;
    /* unit.gd:266-267 `if _pending_die: return [0, 0, false]`: once a lethal hit
     * has queued this unit's deferred die(), every later hit of the same flush
     * deals nothing (and cannot queue a second death). */
    if (s->epend_die[j]) return 0.0f;
    /* unit.gd:298-301: the hurt pick ALWAYS happens and the crit pick REPLACES
     * it when the hit crits (`is_crit` is already resolved -- force-crit-when-
     * burning folded above); unit.gd:323 then submits to SoundManager2D's shared
     * positional pool with pitch_rand 0.2.  This is the source's position: after
     * the crit / OneShotOnHit rolls, before get_damage_value. */
    bt_combat_audio_enemy_hit(s, (int)s->ekind[j], (int)s->eneutral[j], is_crit);
    BT_CONST_PTR(BtUnitKind) K = bt_kunit(s, j);
    float armor = K->armor + K->armor_grow * (float)(s->wave - 1);
    float pre_hp = s->ehp[j];
    /* G1 (brotato_enemy_modifiers.h): pre-crit, pre-armor multiplier */
    float v = bt_enemy_incoming_damage(s, j, base);
    if (is_crit) v = bt_roundf(v * crit_dmg);
    float pct = 0.0f;   /* small_fish: pre-armor %, vs high-HP targets */
    float gcrit = 0.0f; /* giant_crit: post-armor, factor 100 (1000 boss) */
    for (int a = 0; a < s->n_bk_active; a++) {
        int r = (int)s->bk_active[a];
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[r];
        if (!ctx && bt_eff_is(E, BT_ISTR_BONUS_DAMAGE_AGAINST_TARGETS_ABOVE_HP)) {
            for (int c = 0, nown = bt_row_owned(s, r); c < nown; c++) {
                int bv, bv2;
                if (!bt_row_get(s, r, E, c, &bv, &bv2)) break;
                if (pre_hp >= s->emax[j] * ((float)bv2 * 0.01f)) pct += (float)bv;
            }
        } else if (bt_eff_is(E, BT_ISTR_GIANT_CRIT_DAMAGE))
            gcrit += bt_row_sum_v(s, r, E);
    }
    if (ctx)
        pct = bt_wfx_hit_bonus_pct(s, j, ctx->rows, ctx->count,
                                  ctx->is_elemental, s->eburn[j] > 0.0f);
    /* unit.gd:485-486: `for effect_behavior in effect_behaviors.get_children():
     * total_bonus_damage += effect_behavior.get_bonus_damage(hitbox,
     * from_player_index)` runs for EVERY incoming damage source and the total is
     * applied PRE-armor (`dmg_value = round(dmg_value * (1 + total/100))` at
     * unit.gd:498), so the enemy's own percent-damage-taken debuff lands on an
     * explosion hit too -- including the ctx == 0 explosion path.  The helper owns
     * the neutral-slot and effect-expiry rules (brotato_enemy_hit_effects.h). */
    pct += (float)bt_enemy_hit_percent_taken(s, j);
    float dmg = bt_roundf(v * (1.0f + pct * 0.01f));   /* dmg_value as int */
    /* get_damage_value: max(1, dmg_value - current_stats.armor) -- FLAT subtraction */
    float taken = dmg - armor;
    if (taken < 1.0f) taken = 1.0f;
    if (is_crit) {
        float fac = (bt_kind_class(s->ekind[j]) == 2) ? 1000.0f : 100.0f;
        /* enemy.gd:290 giant_crit bonus is divided by max(1, E*0.2) in endless
         * (the Giant Belt nerf); 1.0 exactly for waves <= 20. */
        taken += bt_roundf(pre_hp * (gcrit / fac) / bt_endless_hp_pct_divisor(s->wave));
    }
    if (s->eburn[j] > 0.0f) {
        float bsum = 0.0f;
        for (int a = 0; a < s->n_bk_active; a++) {
            int r = (int)s->bk_active[a];
            BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[r];
            if (!bt_eff_is(E, BT_ISTR_BURNING_ENEMY_HP_PERCENT_DAMAGE)) continue;
            bsum += bt_row_sum_v(s, r, E);
        }
        float fac = (bt_kind_class(s->ekind[j]) == 2) ? 1000.0f : 100.0f;
        /* enemy.gd:302 Greek Fire / burning_enemy_hp_percent_damage: same
         * max(1, E*0.2) divisor. */
        taken += bt_roundf(pre_hp * (bsum / fac) / bt_endless_hp_pct_divisor(s->wave));
    }
    {
        float vsum = 0.0f, vmax = 0.0f;
        for (int a = 0; a < s->n_bk_active; a++) {
            int r = (int)s->bk_active[a];
            BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[r];
            if (!bt_eff_is(E, BT_ISTR_REMOVE_SPEED)) continue;
            vsum += bt_row_sum_v(s, r, E);
            if (bt_row_max_v2(s, r, E) * 0.01f > vmax) vmax = bt_row_max_v2(s, r, E) * 0.01f;
        }
        if (vsum > 0.0f && s->espd[j] > s->espd[j] * (1.0f - vmax))
            s->espd[j] -= s->espd[j] * (vsum * 0.01f);
    }
    if (ctx) {
        taken += ctx->current_health_damage;
        if (ctx->one_shot && taken < pre_hp) taken = pre_hp;
    }
    s->ehp[j] -= taken;
    /* unit.gd:305-306 `current_stats.health = max(0.0, health - full_dmg_value)`:
     * the game's health never goes below zero, and a dying node's health is read
     * by the observation (rl_obs.gd enemy_features ratio ehp/emax). */
    if (s->ehp[j] < 0.0f) s->ehp[j] = 0.0f;
    /* unit.gd:581: Utils.get_chance_success(hitbox.burning_data.chance) -- an
     * EXACT `randf() <= chance` (bt_chance, no draw at chance 0), not a
     * quantized integer surrogate.  (can_burn_enemies is seeded true by
     * player_run_data.gd:501, so that gate never blocks this core.) */
    if (bt_chance(s, burn_chance))
        bt_apply_enemy_burn(s, j, burn_dmg, burn_dur);
    /* ChanceStatDamageEffect procs are event-driven in the source
     * (handle_stat_damages), so no shipped caller passes a nonzero
     * proc_chance/proc_dmg here; kept exact for a future one:
     * main.gd:1512 `randf() < chance/100` proceeds. */
    if (proc_dmg > 0.0f && bt_math_randf(&s->rng) < proc_chance * 0.01f)
        s->ehp[j] -= bt_enemy_incoming_damage(s, j, proc_dmg);
    /* unit.gd:312-315: `if (is_crit or is_one_shot) and hitbox: ... if
     * hitbox.from != null: emit_signal("crit_effect", hitbox.from)` -- the
     * weapon's crit particle play (see bt_fx_crit_particles).  It sits after
     * this hit's own rolls and before the frame's deferred die()/loot draws.
     * (OneShotOnHitEffect is not modelled by this primitive, so only the
     * is_crit arm is reachable today.) */
    if (is_crit && has_from) bt_fx_crit_particles(s);
    /* neutral.gd:20-35 (a tree keeps its own hit tally / one_shot_trees rule;
     * the helper never kills, so the ehp test below still credits a lethal blow) */
    int neutral_death = bt_neutral_resolve_hit(s, j, from_player_index, has_from);
    if (s->ehp[j] <= 0.0f || neutral_death) {
        /* Curse behavior credits the purse directly, before death callbacks. */
        if (from_player_index >= 0 && !s->eneutral[j])
            (void)bt_enemy_curse_player_kill_gold(s, j);
        /* unit.gd:344-356: die() is DEFERRED, so the death callbacks (loot /
         * XP / spawn_loot RNG) run after every hit of this flush, in hit order.
         * bt_queue_kill records the order; bt_drain_kill_q runs them. */
        bt_queue_kill(s, j); (*killed)++;
    }
    return taken;
}

/* One step of every live explosion (call once per step, after the actor updates
 * and BEFORE the wave's end).  Age 0 is the spawn frame (the Hitbox collision is
 * enabled deferred, so nothing is tested then); ages 1..BT_EXPL_FRAMES each run
 * one overlap test against every live target's Hurtbox circle, and every target
 * NODE is hit at most ONCE per explosion (the Area2D area_entered semantics: an
 * explosion that expands over a new target later in its window still hits it).
 * The hit set is keyed on bt_object_uid(s, j) -- the source-object identity
 * weapon.gd Hitbox.ignored_objects holds -- never on the reusable live slot, so
 * an enemy killed and replaced by a FRESH node inside the window is still hit
 * while a pooled respawn (same node) is not.  Only the words this explosion
 * touched are cleared on reuse (e->hit.n = 0, O(1)); the cold
 * reset clears every word.
 * Targets are the enemy/neutral slots (enemy.tscn Hurtbox collision_mask 1032
 * includes the explosion layer 8; the player's mask 20 and the pets' do not, so
 * an explosion never hits the player or a pet).
 * Returns the number of kills; *reward gains R_KILL for each.
 *
 * `cand` is the order-free half of this tick: when non-NULL it is the candidate
 * hit set BT_MAX_EXPL * BT_EXPL_CAND_WORDS unsigned ints (indexed by explosion
 * slot; within an explosion, target SLOT j is bit (j&31) of word (j>>5)) that
 * bt_item_expl_scan produces, and the circle-overlap test below becomes a bit
 * test.  A parallel schedule therefore runs every overlap test off the critical
 * path and keeps this order-locked pass for the RNG/damage/audio side.  cand ==
 * NULL is the serial reference: the overlap test is evaluated inline, byte for
 * byte as before. */
BT_HD static int bt_item_expl_tick_cand(BT_DEVICE BtState* s, BT_THREAD float* reward,
                                        BT_CONST_PTR(unsigned int) cand) {
    int killed = 0;
    /* (A) every live PlayerExplosion node's first _physics_process: the node's own
     * explosion_sounds pick (player_explosion.gd:28-32).  Godot runs node
     * processing BEFORE the physics-area signals, so on the frame a node first
     * processes, every fresh node's pick precedes that frame's hit resolutions --
     * hence a phase of its own.  `age == 0` is the spawn frame (node not in the
     * tree yet), and snd_pending is set only by a FRESH instantiation. */
    for (int i = 0; i < (int)s->expl.used; i++) {
        if (!s->expl.e[i].alive || s->expl.e[i].age == 0) continue;
        if (!s->expl.snd_pending[i]) continue;
        s->expl.snd_pending[i] = 0;
        bt_combat_audio_explosion_node(s, (int)s->expl.cache_row[i]);
    }
    for (int i = 0; i < (int)s->expl.used; i++) {
        BT_DEVICE BtExplosion* e = &s->expl.e[i];
        if (!e->alive) continue;
        if (e->age == 0) { e->age = 1; continue; }   /* collision enable deferred */
        if (e->age > BT_EXPL_FRAMES) {
            /* end_explosion: the node returns to its scene's resident cache with
             * this order, so the next spawn of that scene reuses it LIFO. */
            e->alive = 0;
            s->expl.snd_pending[i] = 0;
            s->expl.cache_seq[i] = s->expl.seq_next++;
            continue;
        }
        e->age++;
        /* one node's kills for the herding reward below (per node, not per
         * tick: the i-loop visits every live explosion node). */
        int node_kills = 0;
        for (int j = bt_enemy_next(s, 0, 1); j < (int)s->e_used; j = bt_enemy_next(s, j + 1, 1)) {
            if (!s->ealive[j]) continue;
            /* source identity, not the reusable live slot: weapon.gd
             * Hitbox.ignored_objects holds the NODE, so an enemy killed and
             * respawned into another slot inside this window is NOT skipped. */
            unsigned int uid = bt_object_uid(s, j);
            if (uid == 0u || uid > BT_OBJECT_UID_MAX) { s->object_uid_overflow = 1; continue; }
            unsigned int word = uid >> 5, bit = 1u << (uid & 31u);
            if (bt_uidset_has(&e->hit, word, bit)) continue;
            if (cand) {
                /* the parallel schedule already ran this pair's overlap test */
                if (!(cand[(size_t)i * BT_EXPL_CAND_WORDS + (j >> 5)] & (1u << (j & 31u))))
                    continue;
            } else {
                BT_CONST_PTR(BtBodyPhysics) G = bt_kgeom(s, j);
                if (!bt_circle_overlap_offset(bt_v2(s->ex[j], s->ey[j]), bt_v2(G->hurt_x, G->hurt_y),
                                              G->hurt_radius,
                                              bt_v2(e->x, e->y), bt_v2(e->ox, e->oy), e->rad))
                    continue;
            }
            /* unit.gd:536-600: the source hurt handler rolls the hitbox's
             * burn chance (unit.gd:581, applying the burn and its sound) BEFORE
             * take_damage rolls the crit (unit.gd:285).  Reproduce that draw
             * order: burn roll + burn sound, then the per-target crit roll, then
             * the hit sound inside the primitive.  burn is passed 0 below so the
             * primitive does not roll it a second time. */
            if (bt_chance(s, e->burn_chance))
                bt_apply_enemy_burn(s, j, e->burn_dmg, e->burn_dur);
            /* unit.gd:285: is_crit = Utils.get_chance_success(hitbox.crit_chance)
             * -- an EXACT `randf() <= crit_chance` (bt_chance), rolled once per
             * target. */
            int is_crit = bt_chance(s, e->crit_chance);
            if (bt_uidset_set(&e->hit, word, bit) < 0) { e->hit_overflow = 1;          /* loud */
                BT_CAP_LATCH(s, BT_CAP_EXPL_SET); }
            int k = 0;
            /* explosion.tscn/player_explosion.tscn leave their Hitbox
             * knockback_direction/knockback_amount at the (0,0)/0 defaults, so
             * the live hit stores a zero knockback vector -- but unit.gd:348's
             * die() seed floors the RAW amount at MIN_DEATH_KNOCKBACK_AMOUNT, so
             * a killed target still drifts along the 0.2/0.8 weighted direction
             * (0.8 * player -> victim here).  The victim sample is the lagged
             * Node transform (s->ehx/s->ehy), as in bt_melee_knockback.
             * Recorded before the hit because the
             * queued die() consumes it at the frame's flush. */
            {
                BtVec2 dir = bt_knockback_direction_weighted(bt_v2zero(),
                                                             bt_v2(s->px, s->py),
                                                             bt_v2(s->ehx[j], s->ehy[j]));
                BtVec2 dkb = bt_death_knockback(dir, 0.0f);
                s->edkbx[j] = dkb.x; s->edkby[j] = dkb.y;
            }
            /* the caller owns the crit draw (unit.gd rolls it per target hit);
             * the primitive applies G1, the crit multiply, the bonus %, FLAT
             * armor and the neutral resolve hook.  (0, 1) = the source's
             * TakeDamageArgs for a player-sourced explosion hitbox.  burn is 0:
             * it was already rolled and applied above, in the source's order. */
            (void)bt_item_expl_hit_enemy(s, j, e->dmg, is_crit, e->crit_dmg,
                                         0.0f, 0.0f, 0.0f,
                                         0.0f, 0.0f, 0, 1, &k, 0);
            if (e->has_src) {
                /* unit.gd:569: the firing weapon's hit_something (weapon.gd:440-465)
                 * runs for EVERY explosion hit and its on_killed_something
                 * (weapon.gd:203-230) for every explosion kill; attack_id -1
                 * skips the per-attack tally, exactly like the source's explosion
                 * hitbox.  has_src is set only by bt_item_expl_set_source from the
                 * weapon-projectile spawn, so non-weapon explosions are untouched.
                 * hit before kill (the source defers die()). */
                BT_CONST_PTR(unsigned short) wrows =
                    &BT_WEAPON_EFF_INDEX[(int)e->src_fx_first];
                int wnr = (int)e->src_fx_count;
                BT_DEVICE const float* wrc = &s->wpn_fxcurse[(int)e->src_slot][0];
                (void)bt_wfx_on_weapon_hit(s, &s->wfx, (int)e->src_slot, e->src_uid, -1,
                                           wrows, wnr, wrc);
                if (k)
                    bt_wfx_on_weapon_kill(s, &s->wfx, (int)e->src_slot, e->src_uid, -1,
                                          wrows, wnr, wrc);
            }
            if (k) { killed++; node_kills++; }
        }
        /* Herding (brotato.h reward design): a single explosion that catches N
         * enemies pays N * (1 + R_HERD*(N-1)); N==1 is byte-identical to the old
         * flat R_KILL, and simultaneous nodes keep their own N (per node). */
        if (node_kills > 0)
            *reward += (float)node_kills * R_KILL
                     + R_HERD * (float)(node_kills * (node_kills - 1));
    }
    /* The MessageQueue::flush boundary of this frame's hit batch.  This tick is
     * the LAST hit phase of bt_core_step (the melee / ranged / player-projectile
     * phases and the deferred contact all ran before it), so every lethal hit
     * queued so far -- across all of this frame's areas -- is applied now, in the
     * order the hits landed, before anything reads the frame's result: exactly
     * unit.gd:344-356's call_deferred("die") drained by one flush after the last
     * hit.  A direct caller of this tick (tests, tools) gets the same kills the
     * frame would. */
    bt_drain_kill_q(s);
    return killed;
}

/* The order-free half of bt_item_expl_tick: for every explosion that will run
 * its overlap tests this frame (alive, age 1..BT_EXPL_FRAMES) and every live
 * target whose uid bit is not already spent, ONE Hurtbox-circle overlap test.
 * The result is the candidate hit set -- BT_MAX_EXPL * BT_EXPL_CAND_WORDS
 * unsigned ints the CALLER zeroes -- where target slot j of explosion i is bit
 * (j&31) of word (j>>5).  Pure: apart from the object_uid_overflow flag (which
 * the serial loop also sets, and only ever to 1) it mutates nothing but `cand`.
 * Independent pairs, so any schedule may compute it; a serial caller can
 * equally skip it and let the tick test inline (cand == NULL).
 *
 * `_range` is the same work restricted to explosions [i_lo, i_hi): disjoint
 * ranges are INDEPENDENT (each explosion owns its own cand words), so a lane
 * group can split the pool with no atomics and no order dependence. */
BT_HD static void bt_item_expl_scan_range(BT_DEVICE BtState* s, BT_DEVICE unsigned int* cand,
                                          int i_lo, int i_hi) {
    if (i_lo < 0) i_lo = 0;
    if (i_hi > (int)s->expl.used) i_hi = s->expl.used;
    for (int i = i_lo; i < i_hi; i++) {
        BT_DEVICE BtExplosion* e = &s->expl.e[i];
        if (!e->alive || e->age == 0 || e->age > BT_EXPL_FRAMES) continue;
        for (int j = bt_enemy_next(s, 0, 1); j < (int)s->e_used; j = bt_enemy_next(s, j + 1, 1)) {
            if (!s->ealive[j]) continue;
            unsigned int uid = bt_object_uid(s, j);
            if (uid == 0u || uid > BT_OBJECT_UID_MAX) { s->object_uid_overflow = 1; continue; }
            unsigned int word = uid >> 5, bit = 1u << (uid & 31u);
            if (bt_uidset_has(&e->hit, word, bit)) continue;
            BT_CONST_PTR(BtBodyPhysics) G = bt_kgeom(s, j);
            if (!bt_circle_overlap_offset(bt_v2(s->ex[j], s->ey[j]), bt_v2(G->hurt_x, G->hurt_y),
                                          G->hurt_radius,
                                          bt_v2(e->x, e->y), bt_v2(e->ox, e->oy), e->rad))
                continue;
            cand[(size_t)i * BT_EXPL_CAND_WORDS + (j >> 5)] |= 1u << (j & 31u);
        }
    }
}

/* The whole pool (the serial reference partitioning). */
BT_HD static void bt_item_expl_scan(BT_DEVICE BtState* s, BT_DEVICE unsigned int* cand) {
    bt_item_expl_scan_range(s, cand, 0, BT_MAX_EXPL);
}

/* Serial reference: the overlap test is evaluated inline (cand == NULL). */
BT_HD static int bt_item_expl_tick(BT_DEVICE BtState* s, BT_THREAD float* reward) {
    return bt_item_expl_tick_cand(s, reward, 0);
}

/* ---------------------------------------------------------------------------
 * Trigger: the entity's shots carry an explosion (unit.gd:552-578)
 * ------------------------------------------------------------------------- */

/* The ExplodingEffect a shooting entity's projectiles carry, or BT_ITEM_EFF_NONE.
 * projectiles/player_projectile.gd:76-84 set_effects() copies the FIRING node's
 * own effects[] onto the hitbox, and turret.gd:112-118 is the only pet/structure
 * that hands its effects to WeaponService.spawn_projectile -- the Blazemander /
 * BotOMine / CatlingGun / Ratzilla shots pass no effects (their own hitboxes and
 * burning_data are separate), and the landmine is a Structure that never fires in
 * the source (its nested explosion belongs to its own trigger). */
BT_HD static unsigned short bt_item_expl_shot_effect(BT_DEVICE BtState* s, int p) {
    int op = (int)s->pet_opcode[p];
    if (op != BT_ITEMEFF_TURRET && op != BT_ITEMEFF_BUILDER_TURRET) return BT_ITEM_EFF_NONE;
    return bt_item_expl_nested_row(&BT_ITEM_EFF_ROWS[s->pet_row[p]]);
}

/* unit.gd:555-578: a hit by a friendly projectile whose effects[] holds an
 * ExplodingEffect spawns the explosion AT THE TARGET's position with the
 * projectile's own damage / crit / burning, and the target takes NO direct
 * damage (is_exploding short-circuits take_damage).
 * Call from the friendly-projectile impact block instead of applying prdmg.
 * Returns 1 when the explosion replaced the direct hit. */
BT_HD static int bt_item_expl_projectile_hit(BT_DEVICE BtState* s, int pj, int j) {
    unsigned short row = s->pr_expl[pj];
    if (row == BT_ITEM_EFF_NONE) return 0;
    (void)bt_item_expl_spawn(s, row, s->ex[j], s->ey[j], s->prdmg[pj],
                             s->prcrit_chance[pj], s->prcrit_dmg[pj],
                             s->prburn_chance[pj], s->prburn_dmg[pj], s->prburn_dur[pj]);
    return 1;
}

/* ---------------------------------------------------------------------------
 * Trigger: landmine structures (landmines item + the BotOMine's bombs)
 * ------------------------------------------------------------------------- */

/* The nested ExplodingEffect row of the landmine pet slot `p`, or -1 when the
 * slot is not a landmine.  Both producers instantiate
 * res://entities/structures/landmine/landmine.tscn (BT_ISTR 69): the landmines
 * item's StructureEffect row and the BotOMine's landmine_effect_stat row. */
BT_HD static int bt_item_expl_landmine_row(BT_DEVICE BtState* s, int p) {
    if (s->pet_opcode[p] != BT_ITEMEFF_STRUCTURE) return -1;
    BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[s->pet_row[p]];
    if (E->scene != BT_ISTR_RES___ENTITIES_STRUCTURES_LANDMINE_LANDMINE_TSCN) return -1;
    unsigned short row = bt_item_expl_nested_row(E);
    return (row == BT_ITEM_EFF_NONE) ? -1 : (int)row;
}

/* The landmine trigger's capsule (see BT_LM_TRIG_* above) vs a body circle at
 * (bx, by) with radius `r`: the segment runs along root-local X. */
BT_HD static int bt_item_expl_lm_hits(float cx, float cy, float bx, float by, float r) {
    float lx = bx - (cx + BT_LM_TRIG_X);
    float ly = by - (cy + BT_LM_TRIG_Y);
    float qx = lx < -BT_LM_TRIG_HALF ? -BT_LM_TRIG_HALF
                                    : (lx > BT_LM_TRIG_HALF ? BT_LM_TRIG_HALF : lx);
    float dx = lx - qx, dy = ly;
    float rr = BT_LM_TRIG_R + r;
    return (dx * dx + dy * dy) <= rr * rr;
}

/* The trigger's `inside` predicate: body_entered against the player and every
 * live, non-neutral enemy (bt_item_expl_lm_hits).  Pure -- the serial tick and
 * bt_item_expl_landmine_scan evaluate exactly this. */
BT_HD static int bt_item_expl_landmine_inside(BT_DEVICE BtState* s, int p) {
    float cx = s->pet_x[p], cy = s->pet_y[p];
    int inside = bt_item_expl_lm_hits(cx, cy, s->px, s->py + BT_PLAYER_BODY_Y,
                                      BT_PLAYER_BODY_R);
    for (int j = bt_enemy_next(s, 0, 1); j < (int)s->e_used && !inside; j = bt_enemy_next(s, j + 1, 1)) {
        if (!s->ealive[j] || s->eneutral[j]) continue;
        BT_CONST_PTR(BtBodyPhysics) G = bt_kgeom(s, j);
        inside = bt_item_expl_lm_hits(cx, cy, s->ex[j] + G->body_x,
                                      s->ey[j] + G->body_y, G->body_radius);
    }
    return inside;
}

/* The order-free half of the landmine tick: one bt_item_expl_landmine_inside
 * per live landmine slot, written as bit (p>>5, p&31) of inside_words --
 * CEIL(BT_MAX_PET/32) unsigned ints the CALLER zeroes.  Pure, so any schedule
 * may compute it; the tick then reads the bit instead of the O(pets*enemies)
 * trigger scan.
 * `_range` restricts the walk to pet slots [p_lo, p_hi): each slot owns its own
 * bit, so disjoint ranges are independent (no atomics). */
BT_HD static void bt_item_expl_landmine_scan_range(BT_DEVICE BtState* s,
                                                   BT_DEVICE unsigned int* inside_words,
                                                   int p_lo, int p_hi) {
    if (p_lo < 0) p_lo = 0;
    if (p_hi > s->pet_n) p_hi = s->pet_n;
    for (int p = p_lo; p < p_hi; p++) {
        if (!s->pet_alive[p]) continue;
        if (bt_item_expl_landmine_row(s, p) < 0) continue;
        if (bt_item_expl_landmine_inside(s, p)) inside_words[p >> 5] |= 1u << (p & 31u);
    }
}

/* The whole pet range (the serial reference partitioning). */
BT_HD static void bt_item_expl_landmine_scan(BT_DEVICE BtState* s,
                                             BT_DEVICE unsigned int* inside_words) {
    bt_item_expl_landmine_scan_range(s, inside_words, 0, s->pet_n);
}

/* landmine.gd:29-46: the trigger Area2D (mask 6 = player|enemy, so never a tree,
 * a pet or another structure) arms the mine on body_entered and calls
 * explode() on body_exited -- which fires effects[0] through WeaponService.explode
 * at the landmine's own position with the structure's stats (damage, crit, the
 * nested exploding bonus, engineering scaling) and then kills the mine.
 * pet_lm_pressed[p] is that "a body has entered" flag.  Call once per step after
 * the enemy/pet updates; slot bookkeeping (pet_alive) is updated here.
 * `inside_words` non-NULL = the precomputed bt_item_expl_landmine_scan mask
 * (the trigger test is then a bit read); NULL = the serial reference, which
 * evaluates each slot's predicate inline, byte for byte as before. */
BT_HD static int bt_item_expl_landmine_tick_cand(BT_DEVICE BtState* s,
                                                 BT_CONST_PTR(unsigned int) inside_words) {
    int fired = 0;
    for (int p = 0; p < s->pet_n; p++) {
        if (!s->pet_alive[p]) continue;
        int row = bt_item_expl_landmine_row(s, p);
        if (row < 0) continue;
        float cx = s->pet_x[p], cy = s->pet_y[p];
        int inside = inside_words ? (int)((inside_words[p >> 5] >> (p & 31u)) & 1u)
                                  : bt_item_expl_landmine_inside(s, p);
        if (inside) { s->pet_lm_pressed[p] = 1; continue; }
        if (!s->pet_lm_pressed[p]) continue;
        (void)bt_item_expl_spawn(s, (unsigned short)row, cx, cy, s->pet_dmg[p],
                                 s->pet_crit_chance[p], s->pet_crit_dmg[p],
                                 s->pet_burn_chance[p], s->pet_burn_dmg[p],
                                 s->pet_burn_dur[p]);
        /* Landmine.die(): the node plays its 1.0 s death animation and re-enters
         * landmine.tscn's pool (main.gd:1678-1699) exactly BT_POOL_DEATH_FRAMES
         * ticks from now -- the BotOMine / landmines item reuses THAT node for
         * its next mine (brotato_structure_pool.h). */
        bt_struct_pool_release(s, p, 0);
        fired++;
    }
    return fired;
}

/* Serial reference: each slot's trigger predicate is evaluated inline
 * (inside_words == NULL). */
BT_HD static int bt_item_expl_landmine_tick(BT_DEVICE BtState* s) {
    return bt_item_expl_landmine_tick_cand(s, 0);
}

/* ---------------------------------------------------------------------------
 * Trigger: the position-based ItemExplodingEffect keys (run_data.gd:1589-1632)
 * ------------------------------------------------------------------------- */

/* weapon_service.gd:281-286 get_explosion_damage(stats, player_index), the
 * damage an ItemExplodingEffect key deals:
 *   max(1, round(max(1, damage + sum_scaling_stat_values(stats.scaling_stats))
 *                * (1 + stat_percent_damage/100 + explosion_damage/100)))
 * (no structure percent-damage term: these keys are player effects).
 * `mod` is the copy's curse modifier (dlc_1_data.gd:146-150 boosts a
 * scale_with_missing_health effect's `value` and otherwise
 * _boost_weapon_stats_damage(stats, mod): damage *(1+mod) and every scaling
 * ratio *(1+mod), which bt_scaling_damage applies through `mod`). */
BT_HD static int bt_item_expl_wstats_damage(BT_DEVICE BtState* s, unsigned short wrow, float mod) {
    if (wrow == BT_ITEM_EFF_NONE || wrow >= BT_NITEM_WSTATS) return 1;
    BT_CONST_PTR(BtItemWeaponStats) W = &BT_ITEM_WSTATS[wrow];
    /* dlc_1_data.gd:379-383 _boost_weapon_stats_damage: the BASE damage is
     * `int(ceil(stats.damage * (1+mod)))` under curse, separate from the per
     * scaling-row ratio boost bt_scaling_damage already applies (bt_curse_stat is
     * the same ceil). */
    int base_dmg = bt_curse_stat((float)(int)W->damage, mod);
    float d = (float)bt_scaling_damage(s, base_dmg, W->scaling_first, W->scaling_count, mod);
    float mult = bt_summon_percent_bonus(s, 0, 0) + bt_explosion_damage(s) * 0.01f;
    float v = bt_roundf(d * mult);
    return v < 1.0f ? 1 : (int)v;
}

/* Utils.get_chance_success (utils.gd:397-403):
 *   if chance == 0.0: return false        (no draw)
 *   return randf() <= chance
 * The core models the global randf() stream with s->rng. */
BT_HD static int bt_item_expl_chance(BT_DEVICE BtState* s, float chance) {
    if (chance <= 0.0f) return 0;
    return bt_math_randf(&s->rng) <= chance;
}

/* run_data.gd:1589-1632 handle_explode_effect(key, position, player_index).  The
 * bucket for `key` is the registered effect-row bucket (bk_active / bk_count /
 * rowvar, i.e. one entry per owned copy with its own curse modifier):
 *   - the chance of every owned copy is SUMMED and rolled ONCE,
 *   - the damage is the SUM of get_explosion_damage over the copies,
 *   - position = Utils.get_random_offset_position(position, 10),
 *   - the args come from effect[0] (accuracy / crit_chance + the capped crit
 *     stat / crit_damage / burning_data / scaling_stats).
 * `key` is an interned key (BT_ISTR_EXPLODE_ON_CONSUMABLE ...); rows of the
 * ExplodingEffect family with that key are the only consumers.  Every shipped
 * producer of these keys is an ItemExplodingEffect, whose curse branch
 * (dlc_1_data.gd:146-150) never changes `chance` -- only the WeaponData
 * ExplodingEffect branch (:151) raises chance, and no such row carries one of
 * these keys.
 * Returns the pool slot, or -1 when the roll failed or nothing is owned. */
BT_HD static int bt_item_expl_on_key(BT_DEVICE BtState* s, unsigned short key, float x, float y) {
    float chance = 0.0f;
    int dmg = 0;
    int first = -1;
    float first_mod = 0.0f;
    for (int a = 0; a < s->n_bk_active; a++) {
        int r = (int)s->bk_active[a];
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[r];
        if (E->key != key && E->custom_key != key) continue;
        if (!bt_item_expl_opcode((int)E->opcode)) continue;
        /* One entry per APPLIED instance (effect.gd apply -> push_back), each with
         * its own curse-transformed (value, value2=mod*100) variant; a row with no
         * recorded variant has bk_count copies at the row's own values. */
        if (s->nvar[r] > 0) {
            for (int v = 0; v < (int)s->nvar[r]; v++) {
                int copies = (int)bt_rvar(s,r)[v].count;
                float mod = (float)bt_rvar(s,r)[v].value2 * 0.01f;
                if (copies <= 0) continue;
                if (first < 0) { first = r; first_mod = mod; }
                chance += E->explode_chance * (float)copies;
                dmg += copies * bt_item_expl_wstats_damage(s, E->stats, mod);
            }
        } else if (s->bk_count[r] > 0) {
            float mod = 0.0f;
            if (first < 0) { first = r; first_mod = mod; }
            chance += E->explode_chance * (float)s->bk_count[r];
            dmg += (int)s->bk_count[r] * bt_item_expl_wstats_damage(s, E->stats, mod);
        }
    }
    if (first < 0) return -1;
    if (!bt_item_expl_chance(s, chance)) return -1;
    BT_CONST_PTR(BtItemEff) F = &BT_ITEM_EFF_ROWS[first];
    /* Utils.get_random_offset_position(pos, 10): pos + Vector2(randi()%21-10,
     * randi()%21-10) (utils.gd:730-733, Utils._rng). */
    float ox = -10.0f + (float)bt_rng_bounded(&s->utils_rng, 21u);
    float oy = -10.0f + (float)bt_rng_bounded(&s->utils_rng, 21u);
    float crit_chance = 0.0f, crit_dmg = 0.0f;
    if (F->stats != BT_ITEM_EFF_NONE && F->stats < BT_NITEM_WSTATS) {
        crit_chance = BT_ITEM_WSTATS[F->stats].crit_chance;
        /* _boost_weapon_stats_damage: crit_damage * (1 + mod/5), stepify 0.1 */
        float cd = BT_ITEM_WSTATS[F->stats].crit_damage * (1.0f + first_mod / 5.0f);
        crit_dmg = bt_roundf(cd * 10.0f) / 10.0f;
    }
    crit_chance += bt_core_stat_capped(s, 9) * 0.01f;
    return bt_item_expl_spawn(s, (unsigned short)first, x + ox, y + oy, (float)dmg,
                              crit_chance, crit_dmg, 0.0f, 0.0f, 0.0f);
}

/* main.gd:693-695 on_consumable_picked_up: the consumable's position is the
 * explosion position (the core's bt_explode is player-centred and cannot be
 * used).  Call from the consumable pickup branch. */
BT_HD static int bt_item_expl_on_consumable(BT_DEVICE BtState* s, float x, float y) {
    return bt_item_expl_on_key(s, BT_ISTR_EXPLODE_ON_CONSUMABLE, x, y);
}

/* ---------------------------------------------------------------------------
 * PLAYER-CENTRED explode_on_hit / explode_when_below_hp (player.gd:443-485).
 *
 * These two keys are NOT handle_explode_effect's summed families: take_damage
 * iterates the per-player effect LIST (one entry per owned copy, acquisition
 * order) and calls player.explode() once per entry:
 *
 *   var explode_on_hit_effects = RunData.get_player_effect(explode_on_hit_hash)
 *   var explode_when_below_hp_effects = ...(explode_when_below_hp_hash)
 *   var nb_explosions = explode_on_hit_effects.size() + explode_when_below_hp_effects.size()
 *   for effect in explode_on_hit_effects:
 *       explode(_explode_on_hit_stats[effect], effect, nb_explosions)
 *   for effect in explode_when_below_hp_effects:
 *       if current_stats.health <= max_stats.health * (effect.hp_threshold / 100.0)
 *          and _explode_when_below_hp_triggers[effect] > 0:
 *           explode(_explode_when_below_hp_stats[effect], effect, nb_explosions)
 *           _explode_when_below_hp_triggers[effect] -= 1
 *
 * so each copy rolls its OWN chance and spawns its OWN explosion (the shared
 * primitive bt_item_expl_spawn), and nb_explosions -- the count of ALL entries of
 * both keys, the CHARACTER's own row included -- decides the position offset
 * scale.  Only two shipped rows carry these keys (krakens_eye explode_on_hit,
 * sunken_bell explode_when_below_hp, both max_nb 1), so walking the canonical
 * registration bucket in order reproduces the source list order exactly.
 *
 * The per-copy cursed stats ride the bucket's BtRowVariant (value, value2 =
 * curse mod * 100): dlc_1_data.gd:146-150 boosts an ItemExplodingEffect's stats
 * damage (or, only for scale_with_missing_health, its value) -- never its
 * chance -- so bt_item_expl_wstats_damage(s, E->stats, mod) is the same
 * resolver bt_item_expl_on_key already uses.
 * ------------------------------------------------------------------------- */

/* Owned copies of the rows carrying `key` -- the source's
 * explode_on_hit_effects.size() / explode_when_below_hp_effects.size(). */
BT_HD static int bt_item_expl_player_owned(BT_DEVICE BtState* s, unsigned short key) {
    int n = 0;
    for (int a = 0; a < s->n_bk_active; a++) {
        int r = (int)s->bk_active[a];
        if (BT_ITEM_EFF_ROWS[r].key != key) continue;
        n += bt_row_owned(s, r);
    }
    return n;
}

/* player.gd:470-471 nb_explosions = explode_on_hit_effects.size() +
 * explode_when_below_hp_effects.size(): the pass's total entry count, which
 * scales the position offset of EVERY explosion of the pass (Utils
 * .get_random_offset_position(pos, (nb_explosions-1)*20)).  The character's own
 * ItemExplodingEffect row is one of those entries even though it is not an item
 * row here (the core keeps it in bt_character_explode_on_hit), so it is counted
 * first, exactly where the source's list holds it. */
BT_HD static int bt_item_expl_player_nb(BT_DEVICE BtState* s) {
    return (bt_character_explode_on_hit(s->character) ? 1 : 0)
         + bt_item_expl_player_owned(s, BT_ISTR_EXPLODE_ON_HIT)
         + bt_item_expl_player_owned(s, BT_ISTR_EXPLODE_WHEN_BELOW_HP);
}

/* The CHARACTER's own explode_on_hit row (bull_effect_4.tres, an
 * ItemExplodingEffect: chance 1.0, scale 1.0, storm scene explosion.tscn, stats
 * bull_explosion_stats, sound_db_mod -15).  It runs through the SAME deferred
 * explosion pool and lifecycle as every item row, so its crit is rolled PER
 * TARGET by bt_item_expl_tick, its node's own explosion_sounds plays once when
 * the node is freshly instantiated, and its two Utils._rng offset draws are
 * taken exactly as the source's explode() takes them.
 *
 * player.gd:443-485 + 544-565: on any damage taken,
 *   for effect in explode_on_hit_effects: explode(_explode_on_hit_stats[effect],
 *                                                effect, nb_explosions)
 * with init_base_stats' exploding branch giving
 *   damage = max(1, round(scaled * (set_bonus + (1 + dmg_pct/100) + explosion_damage/100)))
 * and crit_chance = stats.crit_chance + capped(player crit) (weapon_service.gd
 * :248,253).  bt_scalar_explosion_damage_mult IS that multiplier, and the base
 * damage (30), the per-stat scale (3), the crit chance (0.03) and crit_damage
 * (1.5) come straight off BT_CHARACTERS' generated explode_* fields, i.e. the
 * same numbers bull_explosion_stats.tres carries -- no local constant table.
 * explode_scale_missing_hp is 0 for every shipped character, so
 * get_additional_scaling_damage's `value * missing_health - stats.damage` term is
 * 0 here.
 *
 * Returns the pool slot, or -1 when the character has no such row, the chance
 * roll failed, or the pool refused. */
BT_HD static int bt_item_expl_char_explode(BT_DEVICE BtState* s) {
    if (!bt_character_explode_on_hit(s->character) ||
        !bt_chance(s, bt_character_explode_chance(s->character))) return -1;
    /* player.gd:470-471 nb_explosions + :549 the offset scale, shared by the
     * whole pass (see bt_item_expl_player_nb). */
    int nb = bt_item_expl_player_nb(s);
    int max_off = (nb - 1) * 20;
    if (max_off < 0) max_off = 0;
    unsigned int span = (unsigned int)(2 * max_off + 1);
    float ox = (float)(-max_off + (int)bt_rng_bounded(&s->utils_rng, span));
    float oy = (float)(-max_off + (int)bt_rng_bounded(&s->utils_rng, span));
    /* player.gd:559-560 for this row: bull_explosion_stats.shooting_sounds is
     * EMPTY, so no draw and no submission here -- the node's own sound is the
     * fresh-node pass in bt_item_expl_tick. */
    bt_combat_audio_player_explosion(s);
    int g = bt_item_expl_geom(BT_ISTR_RES___PROJECTILES_EXPLOSION_TSCN);
    if (g < 0) { s->expl.n_missing++; return -1; }
    /* The Bull's OWN stats, straight off the generated character definition
     * (explode_damage 30, explode_dmg_scale 3 per stat_*_damage, crit 0.03 + the
     * capped player crit stat, crit_damage 1.5, scale 1.0) -- the same numbers
     * bull_explosion_stats.tres carries, with no local constant table.
     * init_base_stats' exploding branch: max(1, round(scaled * (set_bonus +
     * (1 + dmg_pct/100) + explosion_damage/100))). */
    BT_CONST_PTR(BtCharacterDef) C = &BT_CHARACTERS[bt_character_index(s->character)];
    /* apply_scaling_stats_to_damage (weapon_service.gd:489-490) is
     * `max(1.0, damage + sum) as int`: the scaled base is TRUNCATED TO INT
     * before the init-time percent multiplier, exactly as init_base_stats does
     * for the Bull's ItemExplodingEffect stats (bull_explosion_stats: damage 30,
     * scaling_stats stat_melee|ranged|elemental_damage x3.0 each, which fold
     * into the single generated explode_dmg_scale over s->dmg_stat). */
    float scaled = (float)C->explode_damage + C->explode_dmg_scale * s->dmg_stat;
    if (scaled < 1.0f) scaled = 1.0f;
    float dmg = bt_roundf((float)(int)scaled * bt_scalar_explosion_damage_mult(s));
    if (dmg < 1.0f) dmg = 1.0f;
    float crit_chance = C->explode_crit_chance + bt_core_stat_capped(s, 9) * 0.01f;
    return bt_item_expl_spawn_geom(s, g, C->explode_scale, s->px + ox, s->py + oy, dmg,
                                   crit_chance, C->explode_crit_damage,
                                   0.0f, 0.0f, 0.0f,   /* bull_explosion_stats: no burning_data */
                                   BT_ITEM_EXPL_CHAR_SOUND_DB);
}

/* One owned copy's player-centre explosion (player.gd:544-565 +
 * weapon_service.gd:511-531), in the source's own draw order:
 *   1. Utils.get_chance_success(effect.chance) -- chance == 0.0 takes NO draw;
 *   2. Utils.get_random_offset_position(pos, (nb_explosions-1)*20): TWO
 *      Utils._rng draws (utils.gd:730-733 `randi() % (2*max+1)`), even at
 *      nb_explosions == 1 where both offsets are 0;
 *   3. the effect stats' own shooting_sounds pick (player.gd:559-560);
 *   4. WeaponService.explode -> the shared pool with the node scale from
 *      effect.scale (bt_item_expl_spawn).
 * `value` is the copy's own Effect.value (only scale_with_missing_health uses
 * it) and `mod` its curse modifier.  Returns the pool slot or -1. */
BT_HD static int bt_item_expl_player_copy(BT_DEVICE BtState* s, int row, BT_CONST_PTR(BtItemEff) E,
                                          int value, float mod, int nb_explosions) {
    if (!bt_item_expl_chance(s, E->explode_chance)) return -1;
    int max_off = (nb_explosions - 1) * 20;
    if (max_off < 0) max_off = 0;
    unsigned int span = (unsigned int)(2 * max_off + 1);
    float ox = (float)(-max_off + (int)bt_rng_bounded(&s->utils_rng, span));
    float oy = (float)(-max_off + (int)bt_rng_bounded(&s->utils_rng, span));
    bt_combat_audio_player_explosion_row(s, (int)E->stats);
    float crit_chance = 0.0f, crit_dmg = 0.0f;
    if (E->stats != BT_ITEM_EFF_NONE && E->stats < BT_NITEM_WSTATS) {
        crit_chance = BT_ITEM_WSTATS[E->stats].crit_chance;
        /* _boost_weapon_stats_damage: crit_damage * (1 + mod/5), stepify 0.1 */
        float cd = BT_ITEM_WSTATS[E->stats].crit_damage * (1.0f + mod / 5.0f);
        crit_dmg = bt_roundf(cd * 10.0f) / 10.0f;
    }
    crit_chance += bt_core_stat_capped(s, 9) * 0.01f;
    float dmg = (float)bt_item_expl_wstats_damage(s, E->stats, mod);
    /* ItemExplodingEffect.get_additional_scaling_damage: while
     * scale_with_missing_health the resource damage is REPLACED by
     * value * missing_health (run_data.gd:305-306 = max_hp - hp).  No shipped
     * explosion row sets the flag, so this term is 0 for every shipped run. */
    if (E->scale_missing_hp && E->stats != BT_ITEM_EFF_NONE && E->stats < BT_NITEM_WSTATS) {
        /* int maths in the source: value * (max_health - current_health) */
        dmg += (float)(value * ((int)s->max_hp - (int)s->hp))
             - (float)BT_ITEM_WSTATS[E->stats].damage;
    }
    return bt_item_expl_spawn(s, (unsigned short)row, s->px + ox, s->py + oy, dmg,
                              crit_chance, crit_dmg, 0.0f, 0.0f, 0.0f);
}

/* The whole player-centred pass (player.gd:443-485).  `nb_explosions` is the
 * SOURCE's total entry count -- the caller computes it as
 *   (bt_character_explode_on_hit(s->character) ? 1 : 0)
 * + bt_item_expl_player_owned(s, BT_ISTR_EXPLODE_ON_HIT)
 * + bt_item_expl_player_owned(s, BT_ISTR_EXPLODE_WHEN_BELOW_HP)
 * because the character's own ItemExplodingEffect row is not an item row (the
 * core models it as bt_character_explode_on_hit / bt_explode) yet still counts
 * towards the offset scale.
 *
 * The when-below-hp copies each spend ONE trigger per wave: the source sets
 * `_explode_when_below_hp_triggers[effect] = 1` for every owned copy in
 * init_exploding_stats(true) from player._ready (player.gd:123,787-800) -- the
 * player node is rebuilt every wave -- and decrements it on use, EVEN when the
 * chance roll then fails.  s->expl_trig holds those per-copy counters, slot by
 * slot in the same bucket order; the wave-start reset belongs to the core
 * (bt_core_reset / bt_enter_wave).
 *
 * Returns the explosions actually spawned. */
BT_HD static int bt_item_expl_player_hit(BT_DEVICE BtState* s, int nb_explosions) {
    int spawned = 0;
    for (int a = 0; a < s->n_bk_active; a++) {
        int row = (int)s->bk_active[a];
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[row];
        if (E->key != BT_ISTR_EXPLODE_ON_HIT) continue;
        for (int c = 0, n = bt_row_owned(s, row); c < n; c++) {
            int v = 0, v2 = 0;
            if (!bt_row_get(s, row, E, c, &v, &v2)) break;
            if (bt_item_expl_player_copy(s, row, E, v, (float)v2 * 0.01f, nb_explosions) >= 0)
                spawned++;
        }
    }
    int trig = 0;
    for (int a = 0; a < s->n_bk_active; a++) {
        int row = (int)s->bk_active[a];
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[row];
        if (E->key != BT_ISTR_EXPLODE_WHEN_BELOW_HP) continue;
        for (int c = 0, n = bt_row_owned(s, row); c < n; c++, trig++) {
            int v = 0, v2 = 0;
            if (!bt_row_get(s, row, E, c, &v, &v2)) break;
            if (trig >= BT_NITEM_EFF_ROWS) { s->expl.expl_trig_overflow = 1; continue; }
            /* player.gd:483 gates on `current_stats.health <= max_stats.health *
             * (hp_threshold/100)` -- the deferred LiveStats max. */
            if (s->hp > s->p_max_hp * ((float)E->hp_threshold / 100.0f)) continue;
            if (s->expl.expl_trig[trig] == 0) continue;
            if (bt_item_expl_player_copy(s, row, E, v, (float)v2 * 0.01f, nb_explosions) >= 0)
                spawned++;
            s->expl.expl_trig[trig] = 0;   /* spent even when the chance roll failed */
        }
    }
    return spawned;
}