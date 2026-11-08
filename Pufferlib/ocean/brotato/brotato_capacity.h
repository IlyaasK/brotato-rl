/* brotato_capacity.h -- implementation-capacity (port bound) loss detection.
 *
 * The port models the source's UNBOUNDED pools with fixed-size tables.  A table
 * that rejects work the source would have accepted is a FIDELITY LOSS, not a
 * game mechanic, and training must fail closed on it rather than silently
 * continue on a truncated environment.  This header is the single place that
 * names every such flag/counter, so the CPU wrapper (brotato.h), the CUDA/Metal
 * wrappers and the tests all ask the same question.
 *
 * FREESTANDING: no libc, no host-only calls -- it is included by brotato_core.h
 * and therefore by the CUDA and Metal translations too.  The host diagnostic
 * (print + abort) lives in brotato_capacity_host.h.
 *
 * GAME-DEFINED eviction is deliberately NOT listed here; those are real
 * mechanics the source itself performs:
 *   - MAX_STRUCTURES structure eviction (entity_spawner.gd:292-303)
 *   - a boss/elite descriptor recycle (boss.gd:81 queue_free)
 *   - a wave's authored enemy cap (wave_max_enemies)
 *
 * pool_cull_fallback IS game-defined: it now fires only for a plain/neutral
 * spawn that evicted a plain enemy, which is the source's own
 * enemies_removed_for_perf cull.
 *
 * The ONE exception is ELITE_SLOTS (elite_slot_pressure): BT_MAX=192 covers 144
 * plain endless enemies + transient group overshoot + 2 bosses + 6 trees + 28
 * concurrently live elites, which is exact only while infinite-endless elite
 * demand stays below that (one event/wave is exact to ~wave 290, and several
 * events on one wave lower that proportionally).  Past it the sim evicts a PLAIN
 * enemy to seat a boss/elite, where the source has no such cap -- so the split
 * flag is an implementation-capacity loss and FAILS CLOSED until an exact
 * cold-elite arena exists (Main's directive).  WavesGaps' bt_reserve_slot_kind
 * guarantees the two causes are mutually exclusive.
 */
#pragma once

#include "brotato_state.h"

/* One entry per loss class: X(NAME, expression).  Add a new bounded table here
 * and every consumer (mask, name table, wrappers, tests) picks it up. */
#define BT_CAPACITY_LOSSES(X)                                                       \
    /* --- identity / registry tables ------------------------------------- */      \
    X(POOL_ID,          s->pool_overflow)          /* pooled instance-id table  */  \
    X(POOL_VAL,         s->pool_val_overflow)      /* ragged init-value run     */  \
    X(OBJECT_UID,       s->object_uid_overflow)    /* source-object uid space   */  \
    X(CURSE_VARIANT,    s->curse_overflow)         /* item-variant row / pool   */  \
    X(STAT_REG,         s->stat_reg_overflow)      /* temp-effect registry      */  \
    X(SUMMON,           s->summon_overflow)        /* summon_fx registration    */  \
    X(PET_POOL,         s->pet_pool_overflow)      /* structure/pet node pool   */  \
    X(WEAPON,           s->wpn_overflow)           /* weapon slot table         */  \
    X(WEAPON_START,     s->wpn_start_overflow)     /* starting-weapon table     */  \
    /* --- spawn queues (push rejected, or the birth stage full) ---------- */      \
    X(PEND,             s->pend_overflow)          /* any queue push rejected   */  \
    X(DROP_ENEMY,       s->pend_drop_enemy)                                                      \
    X(DROP_TREE,        s->pend_drop_tree)                                                       \
    X(DROP_BOSS,        s->pend_drop_boss)                                                       \
    X(DROP_SUMMONS,     s->pend_drop_summons)                                                    \
    X(DROP_STRUCT,      s->pend_drop_struct)                                                     \
    X(DROP_PET,         s->pend_drop_pet)                                                        \
    X(DROP_BIRTH,       s->pend_drop_birth)        /* EntityBirth stage full     */  \
    X(DROP_CONS,        s->pend_drop_cons)         /* consumable pool full      */  \
    X(DROP_MAT,         s->pend_drop_mat)          /* material/gold pool full   */  \
    X(DROP_BOX,         s->pend_drop_box)                                                        \
    X(DROP_CRATE,       s->pend_drop_crate)                                                      \
    /* --- entity pools ---------------------------------------------------- */      \
    X(PROJ,             s->pr_overflow)            /* enemy projectile pool     */  \
    X(WPROJ_POOL,       s->wproj.n_overflow)       /* player projectile pool    */  \
    X(HITQ,             s->hitq.overflow)          /* deferred hit-pair queue   */  \
    X(EXPL_POOL,        s->expl.n_overflow)        /* explosion node pool       */  \
    X(EXPL_MISSING,     s->expl.n_missing)         /* explosion node missing    */  \
    X(EXPL_TRIGGER,     s->expl.expl_trig_overflow)/* below-HP trigger table    */  \
    X(EHITFX,           s->ehitfx.overflow)        /* percent-damage records    */  \
    /* --- wave scheduling -------------------------------------------------- */     \
    X(GROUP,            s->group_overflow)         /* live wave-group table     */  \
    X(ELITE_SLOTS,      s->elite_slot_pressure)    /* BT_MAX live-slot ceiling:
                                                    * a boss/elite reserve had to
                                                    * evict a plain enemy (the
                                                    * source seats it instead) */  \
    X(OE,               s->oe_overflow)            /* endless elite schedule    */  \
    /* --- physics worlds --------------------------------------------------- */     \
    X(PHYS_BODIES,      s->phys.overflow)          /* enemy world body table    */  \
    X(PHYS_PAIRS,       s->phys.pair_overflow)     /* enemy world pair cache    */  \
    X(PHYS_SHAPES,      s->phys.unsupported)       /* enemy world shape combo   */  \
    X(PHYS_PET_BODIES,  s->phys_pet.overflow)      /* pet world body table      */  \
    X(PHYS_PET_PAIRS,   s->phys_pet.pair_overflow) /* pet world pair cache      */  \
    X(PHYS_PET_SHAPES,  s->phys_pet.unsupported)   /* pet world shape combo     */

enum {
#define BT_CAP_ENUM(name, expr) BT_CAP_ ## name,
    BT_CAPACITY_LOSSES(BT_CAP_ENUM)
#undef BT_CAP_ENUM
    /* per-LIVE-entity flags: a live shot's ignored/slowed set or a live
     * explosion's hit set hit its touched-word capacity. */
    BT_CAP_WPROJ_SET,
    BT_CAP_EXPL_SET,
    BT_CAP_NCLASSES
};

/* Bit i of the returned mask == loss class i is tripped.  0 == clean.  Device
 * callable, so the CUDA/Metal wrappers can trap on it. */
BT_HD static BT_DEVICE unsigned long long bt_capacity_scan(BT_DEVICE const BtState* s) {
    unsigned long long m = 0;
    int bit = 0;
#define BT_CAP_BIT(name, expr) do { if ((expr) != 0) m |= 1ull << bit; bit++; } while (0);
    BT_CAPACITY_LOSSES(BT_CAP_BIT)
#undef BT_CAP_BIT
    for (int i = 0; i < (int)s->wproj.used; i++)
        if (s->wproj.p[i].alive && s->wproj.p[i].ign_overflow)
            m |= 1ull << BT_CAP_WPROJ_SET;
    for (int i = 0; i < (int)s->expl.used; i++)
        if (s->expl.e[i].alive && s->expl.e[i].hit_overflow)
            m |= 1ull << BT_CAP_EXPL_SET;
    return m;
}

/* Latch a class at the moment it happens.  bt_core_step calls
 * bt_capacity_latch_scan() before its own end-of-episode reset, and the callers
 * of a per-ENTITY refusal (a shot's identity set, an explosion's hit set) latch
 * their class at the refusal, so a loss is still reported after the entity dies
 * in the same tick.  The latch is sticky: bt_core_reset never clears it. */
#define BT_CAP_LATCH(s, bit) do { (s)->capacity_latch |= 1ull << (bit); } while (0)

BT_HD static BT_DEVICE void bt_capacity_latch_scan(BT_DEVICE BtState* s) {
    s->capacity_latch |= bt_capacity_scan(s);
}

/* Explicit acknowledgement (host/tests only): a wrapper that has reported the
 * loss may clear the latch and continue with a re-created env. */
BT_HD static BT_DEVICE void bt_capacity_clear(BT_DEVICE BtState* s) {
    s->capacity_latch = 0ull;
}

/* Sticky OR of everything latched plus everything currently tripped. */
BT_HD static BT_DEVICE unsigned long long bt_capacity_mask(BT_DEVICE const BtState* s) {
    return s->capacity_latch | bt_capacity_scan(s);
}
