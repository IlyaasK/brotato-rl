/* brotato_enemy_pool.h -- source enemy-instance pool (main.gd `_pool`) for the
 * shared Brotato core.
 *
 * SOURCE
 *   global/entity_spawner.gd:346-420 spawn_entity():
 *     entity = _main.get_node_from_pool(scene.get_instance_id(), entity_pool)
 *     if entity != null:  entity.respawn()            // REUSE: respawn() only
 *     else:               entity = scene.instance()   // FRESH: `_ready` +
 *                         _main.add_entity(entity)     //        init() draws
 *                         entity.init(...)
 *   main.gd:56-61,1649-1699: `_pool` is a Dictionary keyed by
 *   scene.get_instance_id() (one .tscn per BT_KINDS row, BT_KIND_SCENE), and
 *   get_node_from_pool does pop_back (LIFO).
 *   entities/entity.gd:79-100: Entity.death_animation_finished -> free_entity ->
 *   _main.add_node_to_pool(self, pool_id). That method key sits at t=1.0 s of
 *   entity.tscn's `death` animation (Entity.die forces playback_speed = 1 and
 *   plays it at t=0), i.e. the node becomes reusable 1.0 s =
 *   BT_POOL_DEATH_FRAMES = 60 physics ticks after the death (+1 when the die()
 *   came from the engine's IDLE pass; see bt_enemy_pool_release).
 *   boss.gd:81: Boss.death_animation_finished -> queue_free(), so bosses and
 *   elites (BT_KIND_CLASS != 0) never return to the pool; Boss.respawn() even
 *   asserts false (boss.gd:47).
 *   Main is recreated for every wave -- main.gd:1304 change_scene()s to the
 *   shop (ui/menus/shop/base_shop.gd:346 change_scene()s back to
 *   MenuData.game_scene) -- and Main._exit_tree queue_frees the whole pool
 *   (main.gd:1727-1737), so the pool is empty at every wave start.
 *
 * CONTRACT
 *   A poolable instance keeps its identity (and therefore every property the
 *   source never redraws on respawn: `_idle_playback_speed`, `_actual`,
 *   `_actual_target_range`, `_phase`, `_distance_from_player`) across dies and
 *   reuses. The pool is the owner of that identity: it hands out an instance id
 *   and the caller stores the per-instance init values in s->pool_val[id][o]
 *   (exact binary64 bit patterns, one bt_u64 per row o of the kind's init
 *   program: fresh row i at [i], reuse row j at [BT_KIND_INIT_FRESH_N[kind] + j];
 *   the array is dimensioned BT_MAX_KIND_INIT_OPS, the widest program). A REUSE
 *   acquire returns the same id later, so those values are still there.
 *
 *   Call with non-neutral Enemy kinds only. Neutral scenes use another domain
 *   (the caller keeps e_instance[slot] = BT_POOL_NO_INST and emits no instance
 *   state). The pool itself never branches on `fresh`; the caller does:
 *     unsigned short inst; int fresh;
 *     inst = bt_enemy_pool_acquire(s, kind, &fresh);  // BEFORE the hitbox draws
 *     s->e_instance[slot] = inst;                     // live-slot link
 *     ... fresh: draw and store pool_val[inst][o]; reuse: read pool_val[inst][o]
 *     bt_enemy_pool_release(s, s->e_instance[slot]);  // on a real death
 *     s->e_instance[slot] = BT_POOL_NO_INST;
 *   acquire(s, kind, &fresh) -> instance id (BT_POOL_NO_INST only on overflow,
 *                               with pool_overflow set). *fresh = 1 when the
 *                               source took scene.instance(), 0 on respawn().
 *   release(s, instance_id)  -> a real enemy death (bt_mark_enemy_dead, the
 *                               entity_spawner.gd:151-165 enemies_removed_for_perf
 *                               cull, and the wave-end clean_up_room deaths).
 *                               Bosses/elites: descriptor recycled, no reuse.
 *   step(s)                  -> ONCE per combat physics tick.
 *   reset(s)                 -> new Main / wave start (bt_enter_wave).
 *
 *   Call step once at the start of each combat tick, before that tick's deaths.
 *   release writes the current ring slot; it matures after 60 later step calls.
 *   Bosses/elites are always reported fresh: their release never queues reuse, it
 *   only returns the descriptor to a private recycle stack so ids cannot run out.
 *
 * Included by brotato_core.h (self-contained: pulls in brotato_state.h). No
 * allocation and no libc/stdio; POD state only, so CPU/CUDA/Metal share it.
 */
#pragma once

#include "brotato_state.h"

/* Bosses/elites (BT_KIND_CLASS != 0: 1 elite, 2 boss) queue_free and never pool
 * (boss.gd:47,81). A non-neutral Enemy kind in [0, BT_NKINDS) is required. */
BT_HD static int bt_enemy_pool_poolable(int kind) {
    if (kind < 0 || kind >= BT_NKINDS) return 0;
    return BT_KIND_CLASS[kind] == 0;
}

/* New Main / wave start: Main._exit_tree queue_frees the pool (main.gd:1727).
 * Also neutralises every live-slot link. */
BT_HD static void bt_enemy_pool_reset(BT_DEVICE BtState* s) {
    for (int k = 0; k < BT_NKINDS; k++) {
        s->pool_free_head[k] = (unsigned short)BT_POOL_NO_INST;
        s->pool_free_n[k] = 0;
    }
    for (int r = 0; r < BT_POOL_DEATH_FRAMES; r++) {
        s->pool_dly_head[r] = (unsigned short)BT_POOL_NO_INST;
        s->pool_dly_tail[r] = (unsigned short)BT_POOL_NO_INST;
    }
    for (int i = 0; i < BT_MAX; i++) s->e_instance[i] = (unsigned short)BT_POOL_NO_INST;
    s->pool_alloc = 0;
    s->pool_mv_state_n = 0;
    s->pool_np_free = (unsigned short)BT_POOL_NO_INST;
    s->pool_pos = 0;
    s->pool_overflow = 0;
    s->pool_val_overflow = 0;
}

/* Dense placement is id*BT_MAX_KIND_INIT_OPS: descriptors are allocated in id
 * order and a recycled descriptor retains its run. Do not change that stride:
 * a reuse op can read bits left by a previous wave. A deliberately undersized
 * value pool still raises the same loud flag and uses the shared scratch run. */
BT_HD static void bt_pool_val_ensure(BT_DEVICE BtState* s, unsigned short id) {
    if (id >= (unsigned short)BT_POOL_ID_CAP) {
        s->pool_overflow = 1;
        return;
    }
    if (((unsigned int)id + 1u) * BT_MAX_KIND_INIT_OPS > (unsigned int)BT_POOL_VAL_WORDS)
        s->pool_val_overflow = 1;
}

/* Take a never-used instance id for `kind`; NO_INST + pool_overflow once the
 * shared BT_POOL_ID_CAP storage space is exhausted (explicit, never a drop; the
 * source-side BT_POOL_INST_MAX id space is larger, see its comment). */
BT_HD static unsigned short bt_pool_alloc_inst(BT_DEVICE BtState* s, int kind) {
    if ((unsigned int)s->pool_alloc >= (unsigned int)BT_POOL_ID_CAP) {
        s->pool_overflow = 1;
        return (unsigned short)BT_POOL_NO_INST;
    }
    /* Only boss/elite descriptors own state nodes. A released descriptor is
     * always reused before allocating another, so BT_MAX live slots is an exact
     * bound for these banks. Keep the defensive failure loud. */
    if (!bt_enemy_pool_poolable(kind) && s->pool_mv_state_n >= BT_MAX) {
        s->pool_overflow = 1;
        return (unsigned short)BT_POOL_NO_INST;
    }
    {
        unsigned short id = s->pool_alloc++;
        s->pool_kind[id] = (unsigned char)kind;
        s->pool_mv_state[id] = bt_enemy_pool_poolable(kind)
            ? (unsigned short)BT_POOL_NO_INST : s->pool_mv_state_n++;
        bt_pool_val_ensure(s, id);
        return id;
    }
}

/* 1 = FRESH instance: the source took the `scene.instance()` branch, so the
 *     caller emits the fresh-only draws (`_ready`, `_idle_playback_speed`,
 *     movement `init`).
 * 0 = REUSE: the source took the `entity.respawn()` branch, so the caller emits
 *     only the reuse draws (respawn/reset) and none of the fresh-only ones.
 * Returns the instance id backing the spawn (BT_POOL_NO_INST only on overflow). */
BT_HD static unsigned short bt_enemy_pool_acquire(BT_DEVICE BtState* s, int kind, BT_THREAD int* fresh) {
    if (!bt_enemy_pool_poolable(kind)) {
        /* Boss/elite: scene.instance() every time (boss.gd:81 queue_free). Reuse a
         * recycled descriptor if one is free, but the caller still runs the fresh
         * init: pool_val is not retained across boss/elite spawns. */
        *fresh = 1;
        if (s->pool_np_free != (unsigned short)BT_POOL_NO_INST) {
            unsigned short id = s->pool_np_free;
            s->pool_np_free = s->pool_next[id];
            s->pool_kind[id] = (unsigned char)kind;
            bt_pool_val_ensure(s, id);   /* a recycled descriptor already has one */
            return id;
        }
        return bt_pool_alloc_inst(s, kind);
    }
    if (s->pool_free_head[kind] != (unsigned short)BT_POOL_NO_INST) {
        unsigned short id = s->pool_free_head[kind];
        s->pool_free_head[kind] = s->pool_next[id];
        s->pool_free_n[kind]--;
        *fresh = 0;
        return id;
    }
    *fresh = 1;
    return bt_pool_alloc_inst(s, kind);
}

/* A real enemy death: the node enters the 1.0 s death animation and returns to
 * the pool BT_POOL_DEATH_FRAMES ticks later (one more for a death the engine
 * registered from its IDLE pass, BT_POOL_DEATH_IDLE_EXTRA -- see
 * bt_enemy_clean_up_room; the ring below keeps BT_POOL_DEATH_FRAMES because that
 * +1 can never be observed: a clean_up_room death empties the room and the next
 * wave rebuilds Main and its pool >= 2 s later). Bosses/elites: queue_free, so
 * only the descriptor is recycled (never a reuse).  NO_INST (neutral/overflow):
 * no-op. */
BT_HD static void bt_enemy_pool_release(BT_DEVICE BtState* s, unsigned short instance_id) {
    if (instance_id == (unsigned short)BT_POOL_NO_INST) return;
    if (instance_id >= (unsigned short)BT_POOL_ID_CAP) { s->pool_overflow = 1; return; }
    {
        int kind = (int)s->pool_kind[instance_id];
        if (!bt_enemy_pool_poolable(kind)) {
            s->pool_next[instance_id] = s->pool_np_free;
            s->pool_np_free = instance_id;
            return;
        }
        /* append to the current ring slot (FIFO within the slot) */
        {
            int r = s->pool_pos;
            unsigned short tail = s->pool_dly_tail[r];
            s->pool_next[instance_id] = (unsigned short)BT_POOL_NO_INST;
            if (tail == (unsigned short)BT_POOL_NO_INST) s->pool_dly_head[r] = instance_id;
            else s->pool_next[tail] = instance_id;
            s->pool_dly_tail[r] = instance_id;
        }
    }
}

/* One combat physics tick: advance the ring cursor, then mature the slot whose
 * releases have now been in flight BT_POOL_DEATH_FRAMES ticks. The FIFO is
 * pushed oldest-first onto the kind's LIFO free stack, so the last released
 * instance is the first one reused. */
BT_HD static void bt_enemy_pool_step(BT_DEVICE BtState* s) {
    s->pool_pos = (s->pool_pos + 1) % BT_POOL_DEATH_FRAMES;
    {
        int r = s->pool_pos;
        unsigned short cur = s->pool_dly_head[r];
        while (cur != (unsigned short)BT_POOL_NO_INST) {
            unsigned short nxt = s->pool_next[cur];
            int kind = (int)s->pool_kind[cur];
            s->pool_next[cur] = s->pool_free_head[kind];
            s->pool_free_head[kind] = cur;
            s->pool_free_n[kind]++;
            cur = nxt;
        }
        s->pool_dly_head[r] = (unsigned short)BT_POOL_NO_INST;
        s->pool_dly_tail[r] = (unsigned short)BT_POOL_NO_INST;
    }
}
