/* tests/test_brotato_structure_pool.cpp
 *
 * Behaviour tests for the source node pool of STRUCTURE/PET entities
 * (ocean/brotato/brotato_structure_pool.h + the BtState pet_pool_* fields).
 *
 * Every expectation is transcribed from the recovered sources:
 *   global/entity_spawner.gd:346-359  spawn_entity(): pop this scene's node
 *                                     (respawn() only) else scene.instance()
 *   main.gd:56-61,1649-1699           `_pool` keyed by scene.get_instance_id()
 *                                     and pop_back => LIFO
 *   entities/entity.gd:95-100         free_entity -> add_node_to_pool
 *   entities/entity.tscn:88-143       `death` method key at t=1.0 s => 60 frames
 *   entities/structures/structure.gd:9,69-76  deferred_die: `died` NOW,
 *                                     PENDING_DIE_DURATION 3.0 s then die()
 *   entities/structures/turret/turret.gd:11,39,78  `_cooldown` is a class-member
 *                                     initializer and `should_shoot` refuses
 *                                     while `_pending_die`
 *   entities/structures/landmine/landmine.gd:51-62 landmine.tscn:13  a mine dies
 *                                     at once (to_be_removed_in_priority)
 *   entities/structures/turret/wandering_bot/wandering_bot.gd:4-8  class-member
 *                                     rand_range draws, never re-drawn on reuse
 *   global/entity_spawner.gd:292-313  MAX_STRUCTURES eviction, priority array
 *   main.gd:1304 + ui/menus/shop/base_shop.gd:346 + main.gd:1727-1737
 *                                     a new Main per wave, pool queue_freed
 *
 * Build and run:
 *     c++ -O2 -o build/test_brotato_structure_pool tests/test_brotato_structure_pool.cpp -lm
 *     ./build/test_brotato_structure_pool
 *
 * Exit code 0 = all checks passed.
 */

#include "../ocean/brotato/brotato_core.h"
/* BT_RNG_JUMP_RANGE_DRAWS: one rand_range consumes this many raw draws. */
#include "../ocean/brotato/brotato_rng_jump.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;
static int g_checks = 0;

static void check(int cond, const char* msg) {
    g_checks++;
    if (!cond) { g_failures++; printf("  FAIL: %s\n", msg); }
}

/* A BtState is ~1.75 MiB; keep both probe worlds in BSS, never on the stack. */
static BtState A;
static BtState B;

static const unsigned short LANDMINE_SCENE =
    BT_ISTR_RES___ENTITIES_STRUCTURES_LANDMINE_LANDMINE_TSCN;

/* First effect row matching the filter (opcode, and optionally a scene/key). */
static int find_row(unsigned char opcode, unsigned short scene, unsigned short key) {
    for (int r = 0; r < BT_NITEM_EFF_ROWS; r++) {
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[r];
        if (E->opcode != opcode) continue;
        if (scene != 0xFFFFu && E->scene != scene) continue;
        if (key != 0xFFFFu && E->key != key) continue;
        return r;
    }
    return -1;
}

static int row_landmine(void) { return find_row(BT_ITEMEFF_STRUCTURE, LANDMINE_SCENE, 0xFFFFu); }

/* A structure-hash turret: Turret opcode, not the garden (is_spawning) and not
 * the WanderingBot (its own class-member draw path). */
static int row_turret(void) {
    for (int r = 0; r < BT_NITEM_EFF_ROWS; r++) {
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[r];
        if (E->opcode != BT_ITEMEFF_TURRET) continue;
        if (E->is_spawning) continue;
        if (E->key == BT_ISTR_WANDERING_BOT) continue;
        return r;
    }
    return -1;
}

static int row_garden(void) {
    for (int r = 0; r < BT_NITEM_EFF_ROWS; r++) {
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[r];
        if (E->opcode == BT_ITEMEFF_TURRET && E->is_spawning) return r;
    }
    return -1;
}

static int row_wandering_bot(void) {
    return find_row(BT_ITEMEFF_TURRET, 0xFFFFu, BT_ISTR_WANDERING_BOT);
}

/* The source's `structures` array as the sim mirrors it: live, structure-tagged
 * and not already removed by deferred_die (structure.gd:72-76 emits `died` at
 * once, so the node leaves the array while it is still alive). */
static int count_structures(const BtState* s) {
    int n = 0;
    for (int i = 0; i < s->pet_n; i++)
        if (s->pet_alive[i] && s->pet_isstruct[i] && !s->pet_pool_pending[i]) n++;
    return n;
}

/* Raw pcg32 draws between two stream states (-1 = not reachable within 4096). */
static int draws_between(const BtRng* a, const BtRng* b) {
    for (int n = 0; n < 4096; n++) {
        BtRng c = *a;
        bt_rng_advance(&c, (bt_u64)n);
        if (memcmp(&c, b, sizeof(BtRng)) == 0) return n;
    }
    return -1;
}

static BtState* new_world(BtState* s, unsigned int seed) {
    bt_core_init(s, seed, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
    s->n_live_groups = 0;   /* no wave groups: only the probes below spawn */
    return s;
}

int main(void) {
    setbuf(stdout, NULL);
    static float obs[BT_OBS_SIZE];
    float rew, term;

    printf("[1] fresh vs pooled: one node per scene, LIFO pop_back\n");
    {
        int rt = row_turret();
        check(rt >= 0, "a TurretEffect row exists");
        if (rt >= 0) {
            BtState* s = new_world(&A, 900u);
            unsigned short sc = BT_ITEM_EFF_ROWS[rt].scene;
            int t1 = bt_spawn_summon_row(s, (unsigned short)rt, 10.0f, 10.0f, 1, 0.0f, 0.0f, 0.0f);
            int t2 = bt_spawn_summon_row(s, (unsigned short)rt, 20.0f, 20.0f, 1, 0.0f, 0.0f, 0.0f);
            check(t1 == 0 && t2 == 1, "two turret spawns take two slots");
            check(s->pet_pool_set[t1] && s->pet_pool_scene[t1] == sc,
                  "a slot carries the scene of the node it hosts");
            check(s->pet_pool_seq[t1] == 0u && s->pet_pool_seq[t2] == 0u,
                  "a live node is not in the pool");

            /* the source's death -> free_entity -> add_node_to_pool */
            s->ticks = 100;
            bt_struct_pool_release(s, t1, 0);
            check(s->pet_pool_seq[t1] != 0u && s->pet_alive[t1] == 0,
                  "a released node leaves the world and enters its scene's stack");
            bt_struct_pool_release(s, t2, 0);
            check(s->pet_pool_seq[t2] > s->pet_pool_seq[t1],
                  "LIFO: the last returned node carries the higher stamp");

            /* entity.tscn's death animation runs 1.0 s = 60 frames */
            int fresh = 9;
            int q = bt_struct_pool_acquire(s, sc, &fresh);
            check(fresh == 1, "a node still inside its death animation is not reused");

            s->ticks = 160;
            int fresh2 = 9;
            q = bt_struct_pool_acquire(s, sc, &fresh2);
            check(q == t2 && fresh2 == 0,
                  "pop_back: the last returned node is the one reused");

            /* another scene's resident node is never borrowed */
            int fresh3 = 9;
            int q3 = bt_struct_pool_acquire(s, (unsigned short)(sc + 1), &fresh3);
            check(q3 >= 0 && fresh3 == 1, "another scene's resident node is never taken");

            /* the whole table full: explicit, never a silent drop */
            BtState* f = new_world(&B, 901u);
            for (int i = 0; i < BT_MAX_PET; i++)
                (void)bt_spawn_summon_row(f, (unsigned short)rt, 0.0f, 0.0f, 1, 0.0f, 0.0f, 0.0f);
            check(f->pet_n == BT_MAX_PET, "the probe filled the pet table");
            int overflow = bt_spawn_summon_row(f, (unsigned short)rt, 0.0f, 0.0f, 1, 0.0f, 0.0f, 0.0f);
            check(overflow == -1 && f->pet_pool_overflow == 1 && f->summon_overflow == 1,
                  "a full table fails explicitly (the source's pool is unbounded)");
        }
    }

    printf("[2] a fresh instance runs its class-member initializers; a reuse does not\n");
    {
        int rw = row_wandering_bot();
        check(rw >= 0, "a WanderingBot row exists");
        if (rw >= 0) {
            BtState* s = new_world(&A, 902u);
            check(bt_pet_ai_index(BT_ITEMEFF_TURRET) < 0,
                  "a Turret has no pet-AI slot (its motion is the core's own block)");
            int p = bt_spawn_summon_row(s, (unsigned short)rw, 0.0f, 0.0f, 1, 0.0f, 0.0f, 0.0f);
            check(p == 0, "the WanderingBot takes the first slot");
            float d0 = s->pet_wb_dist[p], r0 = s->pet_wb_rots[p], a0 = s->pet_wb_angle[p];
            check(d0 >= 100.0f && d0 <= 200.0f && r0 >= 2.0f && r0 <= 2.5f &&
                  a0 >= 0.0f && a0 <= 6.2831853f,
                  "its three class-member draws (wandering_bot.gd:4-8) landed");

            s->ticks = 60;
            bt_struct_pool_release(s, p, 0);
            s->ticks = s->pet_pool_avail[p];
            BtRng before = s->rng;
            int p2 = bt_spawn_summon_row(s, (unsigned short)rw, 0.0f, 0.0f, 1, 0.0f, 0.0f, 0.0f);
            BtRng after_reuse = s->rng;
            check(p2 == p, "the next WanderingBot reuses the pooled node");
            check(s->pet_wb_dist[p] == d0 && s->pet_wb_rots[p] == r0 && s->pet_wb_angle[p] == a0,
                  "a reused node keeps distance/rotation_speed/_angle");

            /* the same spawn forced onto a not-yet-pooled node is the fresh
             * branch: it must consume exactly the three class-member rand_range
             * calls more (wandering_bot.gd:4-8, 3 raw draws each). */
            bt_struct_pool_release(s, p2, 0);
            s->ticks = s->pet_pool_avail[p2] - 1u;    /* still dying -> fresh */
            s->rng = before;
            int p3 = bt_spawn_summon_row(s, (unsigned short)rw, 0.0f, 0.0f, 1, 0.0f, 0.0f, 0.0f);
            BtRng after_fresh = s->rng;
            check(p3 == p, "the not-yet-pooled node forces a fresh instance in that slot");
            int dr = draws_between(&before, &after_reuse);
            int df = draws_between(&before, &after_fresh);
            check(dr >= 0 && df >= 0, "both streams are reachable by jump-ahead");
            check(df - dr == 3 * (int)BT_RNG_JUMP_RANGE_DRAWS,
                  "fresh emits exactly the 3 WanderingBot rand_range draws a reuse skips");
        }
    }

    printf("[3] Turret._cooldown carries across a reuse; a non-turret resets it\n");
    {
        int rt = row_turret();
        int rl = row_landmine();
        check(rt >= 0 && rl >= 0, "turret + landmine rows exist");
        if (rt >= 0 && rl >= 0) {
            BtState* s = new_world(&A, 903u);
            int p = bt_spawn_summon_row(s, (unsigned short)rt, 0.0f, 0.0f, 1, 0.0f, 0.0f, 0.0f);
            check(s->pet_timer[p] == 0.0f, "a fresh turret starts ready (`_cooldown = 0.0`)");
            s->pet_timer[p] = -7.5f;      /* what a lived-through cooldown looks like */
            s->ticks = 60;
            bt_struct_pool_release(s, p, 0);
            s->ticks += BT_POOL_DEATH_FRAMES;
            int p2 = bt_spawn_summon_row(s, (unsigned short)rt, 0.0f, 0.0f, 1, 0.0f, 0.0f, 0.0f);
            check(p2 == p && s->pet_timer[p] == -7.5f,
                  "a reused turret keeps its `_cooldown` (turret.gd:11 class member)");

            /* a landmine is a plain Structure: no `_cooldown` member, so the
             * sim's pet_timer must come back from its fresh WeaponStats path. */
            s->pet_timer[p] = -7.5f;
            bt_struct_pool_release(s, p, 0);
            s->ticks += BT_POOL_DEATH_FRAMES;
            int lp = bt_spawn_summon_row(s, (unsigned short)rl, 0.0f, 0.0f, 1, 0.0f, 0.0f, 0.0f);
            check(lp == p && s->pet_timer[p] == s->pet_cd[p] &&
                  s->pet_timer[p] != -7.5f,
                  "a reused non-turret takes the cooldown its stats re-initialised");
        }
    }

    printf("[4] a triggered landmine dies and re-enters landmine.tscn's pool\n");
    {
        int rl = row_landmine();
        check(rl >= 0, "a landmine row exists");
        if (rl >= 0) {
            BtState* s = new_world(&A, 904u);
            unsigned short sc = BT_ITEM_EFF_ROWS[rl].scene;
            int p = bt_spawn_summon_row(s, (unsigned short)rl, 5000.0f, 5000.0f, 1,
                                        0.0f, 0.0f, 0.0f);
            check(p == 0 && s->pet_alive[0] == 1, "the mine is live");
            s->ticks = 42;
            s->pet_lm_pressed[0] = 1;                 /* armed by body_entered */
            (void)bt_item_expl_landmine_tick(s);      /* body_exited -> explode() */
            check(s->pet_alive[0] == 0, "the triggered mine dies (Landmine.die)");
            check(s->pet_pool_seq[0] != 0u &&
                  s->pet_pool_avail[0] == 42u + BT_POOL_DEATH_FRAMES,
                  "its node is pooled exactly 1.0 s after the death animation");

            /* the probe runs on a scratch copy: a premature acquire would take
             * the slot (scene.instance(), the source's own branch) */
            B = *s;
            B.ticks = 42 + BT_POOL_DEATH_FRAMES - 1;
            int fresh = 9;
            int q = bt_struct_pool_acquire(&B, sc, &fresh);
            check(q >= 0 && fresh == 1, "no reuse while the node is still dying");
            B = *s;
            B.ticks = 42 + BT_POOL_DEATH_FRAMES;
            fresh = 9;
            q = bt_struct_pool_acquire(&B, sc, &fresh);
            check(q == 0 && fresh == 0, "once pooled, that scene's node is the one handed out");

            s->ticks = 42 + BT_POOL_DEATH_FRAMES;
            int next = bt_spawn_summon_row(s, (unsigned short)rl, 10.0f, 10.0f, 1,
                                           0.0f, 0.0f, 0.0f);
            check(next == 0 && s->pet_alive[0] == 1, "and the next mine runs from it");
        }
    }

    printf("[5] MAX_STRUCTURES: a live mine is evicted first, others deferred_die\n");
    {
        int rl = row_landmine();
        int rt = row_turret();
        if (rl >= 0 && rt >= 0) {
            /* landmine.tscn is the only to_be_removed_in_priority scene, so the
             * source's structures_to_remove_in_priority is never empty while a
             * mine is alive: a turret must survive the eviction. */
            BtState* s = new_world(&A, 905u);
            for (int i = 0; i < BT_MAX_STRUCTS - 1; i++)
                (void)bt_spawn_summon_row(s, (unsigned short)rl, 0.0f, 0.0f, 1, 0.0f, 0.0f, 0.0f);
            int tslot = bt_spawn_summon_row(s, (unsigned short)rt, 0.0f, 0.0f, 1, 0.0f, 0.0f, 0.0f);
            (void)bt_spawn_summon_row(s, (unsigned short)rl, 0.0f, 0.0f, 1, 0.0f, 0.0f, 0.0f);
            check(count_structures(s) == BT_MAX_STRUCTS + 1, "the probe is one over the cap");
            bt_enforce_structure_cap(s);
            check(count_structures(s) == BT_MAX_STRUCTS, "one structure evicted");
            check(s->pet_alive[tslot] == 1 && s->pet_pool_pending[tslot] == 0,
                  "priority array: the turret is untouched while a mine is alive");
            int mines = 0;
            for (int i = 0; i < s->pet_n; i++)
                if (s->pet_alive[i] && bt_struct_pool_is_landmine(s, i)) mines++;
            check(mines == BT_MAX_STRUCTS - 1, "a mine was the victim (Landmine.die)");

            /* without a mine the victim deferred_dies: `died` at once (it leaves
             * the structure count) but the node lives 3.0 s before die(), so its
             * death animation puts it in the pool 4.0 s after the eviction. */
            BtState* d = new_world(&B, 906u);
            for (int i = 0; i < BT_MAX_STRUCTS + 1; i++) {
                (void)bt_spawn_summon_row(d, (unsigned short)rt, 0.0f, 0.0f, 1, 0.0f, 0.0f, 0.0f);
                d->pet_timer[i] = -1.0f - (float)i;    /* per-life sentinel */
            }
            d->ticks = 200;
            bt_enforce_structure_cap(d);
            int ev = -1;
            for (int i = 0; i < d->pet_n; i++) if (d->pet_pool_pending[i]) ev = i;
            check(ev >= 0, "a non-mine eviction is a deferred_die");
            check(d->pet_alive[ev] == 1, "the deferred node is still alive");
            check(count_structures(d) == BT_MAX_STRUCTS,
                  "but it already left the structure list (structure.gd:72-76)");
            check(d->pet_pool_seq[ev] == 0u, "and it is not in the pool yet");

            d->ticks = 200 + BT_POOL_PENDING_FRAMES - 1;
            bt_struct_pool_tick(d);
            check(d->pet_alive[ev] == 1, "alive for the whole 3.0 s pending window");
            d->ticks = 200 + BT_POOL_PENDING_FRAMES;
            bt_struct_pool_tick(d);
            check(d->pet_alive[ev] == 0 && d->pet_pool_seq[ev] != 0u,
                  "die() runs when the 3.0 s timer fires");
            check(d->pet_pool_avail[ev] == d->ticks + BT_POOL_DEATH_FRAMES,
                  "and the node is pooled 1.0 s after that");

            d->ticks = d->pet_pool_avail[ev];
            int np = bt_spawn_summon_row(d, (unsigned short)rt, 0.0f, 0.0f, 1, 0.0f, 0.0f, 0.0f);
            check(np == ev, "the next spawn of that scene reuses the deferred node");
            check(d->pet_timer[ev] == -1.0f - (float)ev,
                  "and it kept the `_cooldown` its previous life left");
        }
    }

    printf("[6] pending structures stop shooting; a garden does not (turret.gd:78)\n");
    {
        int rt = row_turret();
        int rg = row_garden();
        check(rt >= 0 && rg >= 0, "turret + garden rows exist");
        if (rt >= 0 && rg >= 0) {
            BtState* s = new_world(&A, 907u);
            int p = bt_spawn_summon_row(s, (unsigned short)rt, 0.0f, 0.0f, 1, 0.0f, 0.0f, 0.0f);
            /* one live enemy in range, far enough that this step's shot is still
             * in flight when it returns */
            bt_spawn_kind(s, 0, BT_ARENA_W * 0.5f + 200.0f, BT_ARENA_H * 0.5f);
            s->pet_x[p] = BT_ARENA_W * 0.5f; s->pet_y[p] = BT_ARENA_H * 0.5f;
            s->pet_range[p] = 400.0f;
            s->pet_timer[p] = 0.0f;
            s->pet_pool_pending[p] = 0;
            bt_struct_pool_release(s, p, 1);   /* deferred_die: pending, still alive */
            check(s->pet_alive[p] == 1 && s->pet_pool_pending[p] == 1,
                  "deferred_die keeps the node alive");
            for (int i = 0; i < BT_MAX_PROJ; i++) s->pralive[i] = 0;
            bt_core_step(s, 0, obs, &rew, &term);
            int pending_shots = 0;
            for (int i = 0; i < BT_MAX_PROJ; i++) if (s->pralive[i]) pending_shots++;
            check(pending_shots == 0, "`not _pending_die` keeps a pending turret from firing");

            s->pet_pool_pending[p] = 0;
            s->pet_pool_die_at[p] = 0;
            s->pet_timer[p] = 0.0f;
            s->pet_range[p] = 400.0f;   /* a step may refresh it from the stats */
            for (int i = 0; i < BT_MAX_PROJ; i++) s->pralive[i] = 0;
            bt_core_step(s, 0, obs, &rew, &term);
            int live_shots = 0;
            for (int i = 0; i < BT_MAX_PROJ; i++) if (s->pralive[i]) live_shots++;
            check(live_shots > 0, "the same turret fires once it is not pending");

            /* Garden overrides should_shoot without the _pending_die test, so a
             * pending garden keeps emitting fruit (garden.gd:9-10). */
            BtState* g = new_world(&B, 908u);
            int gp = bt_spawn_summon_row(g, (unsigned short)rg, 0.0f, 0.0f, 1, 0.0f, 0.0f, 0.0f);
            check(g->pet_timer[gp] == 0.0f, "a fresh garden starts ready");
            bt_struct_pool_release(g, gp, 1);   /* pending, but a Garden still emits */
            for (int i = 0; i < BT_MAX_CONS; i++) g->conalive[i] = 0;
            bt_structure_spawning_tick(g);
            int fruits = 0;
            for (int i = 0; i < BT_MAX_CONS; i++) if (g->conalive[i]) fruits++;
            check(fruits == 1, "a pending garden still emits its fruit");
        }
    }

    printf("[7] the pool is per wave and moves with bt_pet_compact\n");
    {
        int rt = row_turret();
        if (rt >= 0) {
            BtState* s = new_world(&A, 908u);
            int p = bt_spawn_summon_row(s, (unsigned short)rt, 0.0f, 0.0f, 1, 0.0f, 0.0f, 0.0f);
            (void)p;
            /* keep a live node in slot 0 and a returned one in slot 1 so the
             * compaction has something to move */
            bt_spawn_summon_row(s, (unsigned short)rt, 1.0f, 1.0f, 1, 0.0f, 0.0f, 0.0f);
            s->ticks = 10;
            bt_struct_pool_release(s, 0, 0);
            bt_spawn_summon_row(s, (unsigned short)rt, 2.0f, 2.0f, 1, 0.0f, 0.0f, 0.0f);
            unsigned int seq1 = s->pet_pool_seq[1];
            unsigned int avail1 = s->pet_pool_avail[1];
            bt_pet_compact(s);
            check(s->pet_n == 2, "compaction keeps both slots (slot 0 was respawned)");
            check(s->pet_pool_set[0] && s->pet_pool_seq[1] == seq1 &&
                  s->pet_pool_avail[1] == avail1,
                  "pool identity moves with the slot and is never scrambled");

            unsigned int stamp = s->pet_pool_stamp;
            check(stamp != 0u, "the LIFO stamp advanced");
            bt_enter_wave(s, 2);
            check(s->pet_pool_stamp == 0u && s->pet_pool_overflow == 0,
                  "a new wave (new Main) frees the whole node pool");
            for (int i = 0; i < BT_MAX_PET; i++)
                if (s->pet_pool_set[i] || s->pet_pool_seq[i] != 0u)
                    check(0, "every slot's node identity is gone at wave start");
            check(s->pet_n == 0, "and no structure/pet entity survives the wave");
        }
    }

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures != 0;
}
