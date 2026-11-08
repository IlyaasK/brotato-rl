/* brotato_structure_pool.h -- source node pool (main.gd `_pool`) for the shared
 * core's STRUCTURE and PET entities.  Companion of brotato_enemy_pool.h: the
 * enemy pool keeps a descriptor per instance id because the sim's enemy slots
 * are recycled freely; structures/pets already live in one slot table
 * (s->pet_*[BT_MAX_PET]) that the source's per-scene stack maps onto directly,
 * so the pool here is the scene-keyed LIFO plus the key that makes a slot's
 * node identity survive a die/respawn inside one wave.
 *
 * SOURCE
 *   global/entity_spawner.gd:346-359 spawn_entity():
 *     entity = _main.get_node_from_pool(scene.get_instance_id(), entities)
 *     if entity != null:  entity.respawn()          // REUSE: respawn() only
 *     else:               entity = scene.instance()  // FRESH: `_ready` + the
 *                         _main.add_entity(entity)  //        class-member
 *                         entity.init(...)          //        initializers
 *   main.gd:56-61,1649-1699: `_pool` is a Dictionary keyed by
 *   scene.get_instance_id() -- one .tscn per structure/pet, BT_ITEM_EFF_ROWS
 *   [].scene -- and get_node_from_pool does pop_back (LIFO).
 *   entities/entity.gd:95-100 free_entity -> main.add_node_to_pool(self, pool_id)
 *   from death_animation_finished, the method key of entity.tscn's `death`
 *   animation at t=1.0 s = BT_POOL_DEATH_FRAMES ticks after die().
 *   entities/structures/structure.gd:69-76 deferred_die(): `_pending_die = true`
 *   and `died` emitted AT ONCE (so the node leaves the spawner's `structures`
 *   array and the MAX_STRUCTURES count) while the node itself stays alive for
 *   PENDING_DIE_DURATION = 3.0 s before die() runs -- it reaches the pool
 *   BT_POOL_PENDING_FRAMES + BT_POOL_DEATH_FRAMES ticks after the eviction.
 *   main.gd:1304 + ui/menus/shop/base_shop.gd:346: Main is rebuilt every wave
 *   and Main._exit_tree (main.gd:1727-1737) queue_frees the whole pool, so the
 *   pool is empty at every wave start (bt_struct_pool_reset).
 *
 * WHAT A REUSE KEEPS (the sim's modelled share)
 *   respawn() (entity.gd:48-55, structure.gd:27-31, landmine.gd:14-16) and the
 *   set_data()/update_data() call that spawn_entity runs on BOTH branches
 *   (entity_spawner.gd:398-412) redraw every stat, cooldown-from-stats, burn,
 *   crit and health field -- so a pooled node is numerically identical to a
 *   fresh one EXCEPT for the class-member initializers that only scene.instance()
 *   evaluates:
 *     * WanderingBot `distance`/`rotation_speed`/`_angle` (wandering_bot.gd:4-8:
 *       three global rand_range calls = 9 raw draws, measured in the engine:
 *       a FRESH WanderingBot consumes 9 global draws, a REUSED one 0) ->
 *       carried in pet_wb_dist/rots/angle.
 *     * Turret `_cooldown: float = 0.0` (turret.gd:11) -> carried in pet_timer
 *       for BT_ITEMEFF_TURRET / BT_ITEMEFF_BUILDER_TURRET (garden included: it
 *       extends Turret).  Engine-measured: a fresh AND a reused plain Turret
 *       consume 0 global draws, and the reused one still reports the `_cooldown`
 *       the previous life left (-33.0 in the probe).  Pets reset their own
 *       cooldown in update_data (blazemander.gd:30, bonk_dog.gd:37,
 *       catling_gun.gd:42-43, ratzilla.gd:23, bot_o_mine.gd:37, lootworm.gd:38)
 *       on both branches, so only the Turret member needs the carry.
 *
 * NOT REPRODUCED (engine-measured, MAX_STRUCTURES-eviction-only)
 *   `_is_shooting` and `_nb_shots_taken` are class members that respawn() and
 *   set_data() never touch either: the engine probe kills a turret that is
 *   mid-shoot, respawns it and still reads `_is_shooting = true` /
 *   `_nb_shots_taken = 7`, which makes the SOURCE turret refuse to fire again
 *   (should_shoot's `not _is_shooting`, turret.gd:78) for the rest of its life.
 *   This core does not model the shoot animation (the same reason
 *   brotato_structures.h takes the next-cooldown draw at the shot instead of at
 *   its animation_finished), so `_is_shooting` has no window to be true in, and
 *   no structure WeaponStats row sets additional_cooldown_every_x_shots, so
 *   `_nb_shots_taken` has no reader.  Both are reachable only through a
 *   MAX_STRUCTURES eviction (>100 live structures), which no authored or
 *   endless wave reaches, and `_shooting_speed`/`player_index`/`dead` DO reset
 *   on the reuse path exactly as set_data()/respawn() do (engine-verified).
 *
 * CONTRACT
 *   A slot's node identity is (pet_pool_set, pet_pool_scene).  Call
 *     int fresh; int p = bt_struct_pool_acquire(s, E->scene, &fresh);
 *   BEFORE any per-branch initialisation: on fresh the caller runs the
 *   scene.instance()/init() side (the class-member draws), on reuse it keeps
 *   them.  Release on a real death with bt_struct_pool_release(s, p, deferred)
 *   -- deferred = 1 only for the MAX_STRUCTURES eviction of a non-Landmine
 *   structure (deferred_die).  bt_struct_pool_tick runs once per combat tick
 *   (beside bt_enemy_pool_step) and performs the deferred node's die();
 *   bt_struct_pool_reset runs at every wave start.
 *
 * Included by brotato_core.h.  No allocation and no libc/stdio; POD state only,
 * so CPU/CUDA/Metal share it.
 */
#pragma once

#include "brotato_state.h"
#include "brotato_item_effects.h"

/* landmine.tscn is the only structure scene with StructureEffect
 * to_be_removed_in_priority (landmine.tscn:13; structure.tscn:11 and
 * builder_turret.tscn:17 leave it false), so `st is Landmine` -- the source's
 * test for "evict with die() at once, else deferred_die()"
 * (entity_spawner.gd:310-313) -- is exactly the node's scene, and Landmine also
 * overrides die() (landmine.gd:51-62).  Same key bt_item_expl_landmine_row
 * (brotato_item_explosions.h) uses to recognise a mine. */
BT_HD static int bt_struct_pool_is_landmine(BT_DEVICE BtState* s, int p) {
    return BT_ITEM_EFF_ROWS[s->pet_row[p]].scene ==
           BT_ISTR_RES___ENTITIES_STRUCTURES_LANDMINE_LANDMINE_TSCN;
}

/* New Main / wave start: Main._exit_tree queue_frees the pool (main.gd:1727). */
BT_HD static void bt_struct_pool_reset(BT_DEVICE BtState* s) {
    for (int p = 0; p < BT_MAX_PET; p++) {
        s->pet_pool_set[p] = 0;
        s->pet_pool_scene[p] = 0;
        s->pet_pool_seq[p] = 0;
        s->pet_pool_avail[p] = 0;
        s->pet_pool_pending[p] = 0;
        s->pet_pool_die_at[p] = 0;
    }
    s->pet_pool_stamp = 0;
    s->pet_pool_overflow = 0;
}

/* entity_spawner.gd:350-359: hand back `scene`'s most recently returned node
 * (LIFO) once its death animation has finished, else instantiate a fresh one.
 * *fresh = 1 for scene.instance() (the caller must run the fresh-only
 * class-member draws), 0 for a pooled node (the caller must NOT).
 * Returns the hosting slot, or -1 when the BT_MAX_PET table is full
 * (pet_pool_overflow; the source's pool is unbounded, so this is a sim capacity
 * limit, never a source rule -- the caller also raises summon_overflow). */
BT_HD static int bt_struct_pool_acquire(BT_DEVICE BtState* s, unsigned short scene,
                                        BT_THREAD int* fresh) {
    /* 1. the scene's own stack: pop_back, i.e. the highest return stamp that
     *    already matured.  A resident node whose animation is still running is
     *    NOT in the stack, so it is skipped entirely (the source would have
     *    missed the pool and instantiated). */
    int best = -1;
    unsigned int best_seq = 0u;
    for (int p = 0; p < s->pet_n; p++) {
        if (s->pet_alive[p]) continue;
        if (s->pet_pool_pending[p]) continue;      /* deferred_die: still in the world */
        if (!s->pet_pool_set[p] || s->pet_pool_scene[p] != scene) continue;
        if (s->pet_pool_seq[p] == 0u) continue;
        if (s->pet_pool_avail[p] > (unsigned int)s->ticks) continue;
        if (best < 0 || s->pet_pool_seq[p] > best_seq) { best = p; best_seq = s->pet_pool_seq[p]; }
    }
    if (best >= 0) {
        s->pet_pool_seq[best] = 0u;                /* live again */
        *fresh = 0;
        return best;
    }
    /* 2. no resident node: scene.instance().  The slot choice is the sim's own
     *    (lowest free index, else grow -- the source allocates a brand new
     *    node), and the node identity of whatever occupied it is dropped: that
     *    node belonged to another scene's stack, or is a same-scene node whose
     *    animation has not finished. */
    int p = -1;
    for (int i = 0; i < s->pet_n; i++) if (!s->pet_alive[i]) { p = i; break; }
    if (p < 0) {
        if (s->pet_n >= BT_MAX_PET) { s->pet_pool_overflow = 1; return -1; }
        p = s->pet_n++;
    }
    s->pet_pool_set[p] = 1;
    s->pet_pool_scene[p] = scene;
    s->pet_pool_seq[p] = 0u;
    s->pet_pool_pending[p] = 0;
    *fresh = 1;
    return p;
}

/* A structure/pet node left the world: Entity.death_animation_finished ->
 * free_entity -> main.add_node_to_pool (entity.gd:95-100), so it re-enters its
 * scene's LIFO stack BT_POOL_DEATH_FRAMES ticks later.  `deferred` = the node
 * was evicted by MAX_STRUCTURES and is not a Landmine: Structure.deferred_die()
 * leaves the structure list at once but stays ALIVE (and in the world) for
 * PENDING_DIE_DURATION before die(); pet_alive stays 1 until bt_struct_pool_tick
 * reaches pet_pool_die_at. */
BT_HD static void bt_struct_pool_release(BT_DEVICE BtState* s, int p, int deferred) {
    if (p < 0 || p >= BT_MAX_PET) return;
    if (deferred) {
        s->pet_pool_pending[p] = 1;
        s->pet_pool_die_at[p] = (unsigned int)s->ticks + BT_POOL_PENDING_FRAMES;
        return;                                    /* die() runs later */
    }
    s->pet_alive[p] = 0;
    s->pet_pool_pending[p] = 0;
    s->pet_pool_seq[p] = ++s->pet_pool_stamp;
    s->pet_pool_avail[p] = (unsigned int)s->ticks + BT_POOL_DEATH_FRAMES;
}

/* One combat tick: a deferred_die node's 3.0 s timer fires, i.e. it plays the
 * usual 1.0 s death animation from now on and is pooled BT_POOL_DEATH_FRAMES
 * ticks later.  Runs beside bt_enemy_pool_step so CPU/GPU schedules agree. */
BT_HD static void bt_struct_pool_tick(BT_DEVICE BtState* s) {
    for (int p = 0; p < s->pet_n; p++) {
        if (!s->pet_pool_pending[p]) continue;
        if ((unsigned int)s->ticks < s->pet_pool_die_at[p]) continue;
        bt_struct_pool_release(s, p, 0);
    }
}
