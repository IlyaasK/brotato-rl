/* Source-ordered fresh/respawn initialization from extract_gameplay.py.
 * Values remain binary64 until their actual integer/physics consumer. Per-node
 * storage survives the source's LIFO pool; respawn rows never overwrite the
 * creation-time movement/animation rows. No allocation or host-only arithmetic.
 */
#pragma once
#include "brotato_state.h"

BT_HD static bt_u64 bt_init_bits(unsigned int low, unsigned int high) {
    return ((bt_u64)high << 32) | (bt_u64)low;
}

/* Only fresh rows describe persistent movement parameters and each boss state's
 * initial cooldown. Runtime speed/current root cooldown live in the slot. */
BT_HD static int bt_enemy_init_find(BT_DEVICE BtState* s, int slot, int dest, int state,
                                   BT_THREAD bt_u64* value) {
    unsigned short id = s->e_instance[slot];
    if (id == BT_POOL_NO_INST) return 0;
    int kind = (int)s->ekind[slot];
    int first = (int)BT_KIND_INIT_FRESH_FIRST[kind];
    for (int i = 0; i < (int)BT_KIND_INIT_FRESH_N[kind]; i++) {
        BT_CONST_PTR(BtEnemyInitOp) op = &BT_ENEMY_INIT_OPS[first + i];
        if ((int)op->dest == dest && (int)op->state == state) {
            *value = bt_pool_val(s, id)[i];
            return 1;
        }
    }
    return 0;
}

BT_HD static void bt_enemy_init_run(BT_DEVICE BtState* s, int slot, int kind,
                                    unsigned short id, int fresh) {
    int first = fresh ? (int)BT_KIND_INIT_FRESH_FIRST[kind] : (int)BT_KIND_INIT_REUSE_FIRST[kind];
    int count = fresh ? (int)BT_KIND_INIT_FRESH_N[kind] : (int)BT_KIND_INIT_REUSE_N[kind];
    int storage = fresh ? 0 : (int)BT_KIND_INIT_FRESH_N[kind];
    unsigned int shots = fresh ? 0u : (unsigned int)s->pool_shots[id];
    for (int i = 0; i < count; i++) {
        BT_CONST_PTR(BtEnemyInitOp) op = &BT_ENEMY_INIT_OPS[first + i];
        bt_u64 value;
        if ((op->flags & BT_INITFLAG_COND_SHOTS) && shots >= (unsigned int)op->cond) {
            /* get_cd returns long_cooldown without sampling, then reset clears
             * _shots_taken (shooting_attack_behavior.gd:62-65,228-235). */
            value = bt_rf64_to_bits(bt_rf64_from_float((float)BT_ATTACKS[kind].long_cooldown));
            shots = 0;
        } else {
            bt_u64 lo = op->lo_ref == BT_INIT_NO_REF ? bt_init_bits(op->lo_low, op->lo_high) :
                bt_pool_val(s, id)[storage + (int)op->lo_ref - first];
            bt_u64 hi = op->hi_ref == BT_INIT_NO_REF ? bt_init_bits(op->hi_low, op->hi_high) :
                bt_pool_val(s, id)[storage + (int)op->hi_ref - first];
            if (op->flags & BT_INITFLAG_NEG_LO) lo ^= (bt_u64)1 << 63;
            if (op->flags & BT_INITFLAG_NEG_HI) hi ^= (bt_u64)1 << 63;
            bt_enemy_init_scene_extra(s, kind, (unsigned char)op->site, (unsigned char)op->dest,
                                      (unsigned char)op->state, lo, hi);
            value = bt_rng_global_range_bits(&s->rng, lo, hi);
            s->draw_site[3] += 3u;   /* one rand_range = 3 global words */
            if (op->flags & BT_INITFLAG_ADD)
                value = bt_rng_f64_add_bits(bt_init_bits(op->add_low, op->add_high), value);
            if (op->flags & BT_INITFLAG_TRUNC) value = bt_rng_f64_trunc_bits(value);
        }
        bt_pool_val(s, id)[storage + i] = value;
        if (op->state == BT_INIT_ROOT_STATE) {
            if (op->dest == BT_INITDEST_SPEED_RAND)
                s->espeed_rand[slot] = bt_rng_f64_to_float_bits(value);
            else if (op->dest == BT_INITDEST_ATK_CD)
                s->eatk[slot] += bt_rng_f64_to_float_bits(value);   /* _current_cd, in frames */
        }
    }
    s->pool_shots[id] = 0;
}
