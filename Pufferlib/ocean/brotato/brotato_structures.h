/* brotato_structures.h -- structure-item runtime for the "structures +
 * consumable-economy" item slice.
 *
 * Garden (items/all/garden/garden_effect_1.tres: a TurretEffect subclass with
 * `is_spawning = true`) does NOT shoot projectiles.  garden.gd overrides
 * Turret.should_shoot() to ignore targets and shoot() to emit
 * `wanted_to_spawn_fruit`, which main.gd:1446-1464 turns into a tier-COMMON
 * (fruit) consumable dropped at a random point around the structure.
 *
 * The core's pet firing loop treats every non-StructureEffect summon as a
 * projectile turret and would otherwise fire a 1-damage shot at the nearest
 * enemy every garden_stats.tres cooldown (damage 0 -> clamped to 1,
 * nb_projectiles 0 -> forced to 1).  bt_structure_spawning_tick() below owns the
 * real fruit emission and the firing loop must skip these rows.
 *
 * SOURCE PROVENANCE (brotato_recovered v1.1.14.6 + brotato_abyss)
 *   entities/structures/turret/garden/garden.gd:5-11   shoot() / should_shoot()
 *   entities/structures/turret/turret.gd:29-31,121-128,155-157
 *                                                      _physics_process /
 *                                                      _get_next_cooldown /
 *                                                      animation-finished
 *   main.gd:1437-1439,1446-1464                        structure signal wiring /
 *                                                      on_structure_wanted_to_spawn_fruit
 *   items/consumables/fruit/fruit_data.tres            tier 0, heal BT_HEAL
 *   singletons/item_service.tscn:710                   ItemService.consumables
 *                                                      (tier-0 pool is {fruit})
 *   items/all/garden/garden_stats.tres                 cooldown 900 ticks, 3 shooting_sounds
 *
 * RNG (stream = the global randf/randi, i.e. BtState.rng; one fruit cycle):
 *   Utils.get_rand_element(garden_stats.shooting_sounds)      1 draw (3 sounds)
 *   ItemService.get_consumable_for_tier(Tier.COMMON)          1 draw (1-element pool)
 *   dist = rand_range(100, 150)                               3 draws
 *   dest = Vector2(rand_range(px-dist, px+dist),
 *                  rand_range(py-dist, py+dist))              6 draws
 *   _get_next_cooldown(): rand_range(max(1, mc*0.7), mc*1.3)  3 draws
 * 14 draws in that order.  The sound + fruit land inside shoot(); the cooldown
 * draw lands on the animation-finished signal 0.1 s later -- this core does not
 * model the shoot animation for any turret (see the pet block in brotato_core.h),
 * so the draw is taken immediately after, matching that existing convention.
 */
#pragma once

/* garden_stats.tres `shooting_sounds` has exactly 3 entries. */
#define BT_GARDEN_SOUNDS 3

/* main.gd:1446-1464: spawn one tier-0 (fruit) consumable at (x, y) with the
 * garden's own drop draws.  Only the pool slot is created here; the shared
 * consumable pickup path (bt_loot_consumable_take) handles the rest. */
BT_HD static void bt_garden_emit_fruit(BT_DEVICE BtState* s, float x, float y) {
    /* ItemService.get_consumable_for_tier(Tier.COMMON): pick_random over the
     * tier-0 pool, which is {fruit} for the base game and the Abyss DLC alike
     * (item_service.tscn:710; the DLC consumables poisoned_fruit / cursed_chest
     * are reached through update_consumable_to_get, never the tier pool, and
     * only roll when a producer of `poisoned_fruit` is owned, which draws
     * nothing at 0).  One draw regardless of pool size. */
    (void)bt_rng_bounded(&s->rng, 1u);
    float dist = bt_rand_range(s, 100.0f, 150.0f);
    float dx = bt_rand_range(s, x - dist, x + dist);
    float dy = bt_rand_range(s, y - dist, y + dist);
    for (int c = 0; c < BT_MAX_CONS; c++) {
        if (s->conalive[c]) continue;
        s->conx[c] = x;
        s->cony[c] = y;
        s->conkind[c] = (unsigned char)BT_CONS_FRUIT;
        s->conalive[c] = 1;
        if (c >= (int)s->con_used) s->con_used = (unsigned short)(c + 1);
        s->con_seq[c] = s->con_seq_next++;
        s->conspd[c] = BT_ATTRACT_SPEED;
        s->con_attr[c] = 0;
        bt_loot_item_place(s->con_arc[c], dx, dy);
        return;
    }
    /* Consumable pool full: explicit, observable overflow (never a silent drop),
     * the same failure the enemy-drop path uses. */
    s->pend_overflow = 1;
    s->pend_drop_cons++;
}

/* Per-frame tick for the is_spawning turrets (Garden).  Call once per step from
 * the core's pet block, after the projectile-firing loop and before the landmine
 * tick.  A garden starts ready (turret.gd: `_cooldown = 0.0`, which the core
 * mirrors by initialising pet_timer to 0 for turrets) and needs no target, so it
 * emits its first fruit on the frame it exists.
 * `ready_words` non-NULL = the precomputed bt_structure_spawning_scan mask
 * (a garden's "cooldown reached 0" decision is then a bit read; the timer is
 * still decremented here, exactly once); NULL = the serial reference, which
 * makes the decision inline, byte for byte as before. */
BT_HD static void bt_structure_spawning_tick_cand(BT_DEVICE BtState* s,
                                                  BT_CONST_PTR(unsigned int) ready_words) {
    for (int p = 0; p < s->pet_n; p++) {
        if (!s->pet_alive[p] || s->pet_opcode[p] != BT_ITEMEFF_TURRET) continue;
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[s->pet_row[p]];
        if (!E->is_spawning) continue;
        s->pet_timer[p] -= BT_DT;
        int ready = ready_words ? (int)((ready_words[p >> 5] >> (p & 31u)) & 1u)
                                : !(s->pet_timer[p] > 0.0f);
        if (!ready) continue;
        /* garden.gd:5-7 shoot(): the sound pick, then emit_signal. */
        (void)bt_rng_bounded(&s->rng, BT_GARDEN_SOUNDS);
        bt_garden_emit_fruit(s, s->pet_x[p], s->pet_y[p]);
        /* turret.gd:121-128 _get_next_cooldown:
         *   rand_range(max(1, mc * 0.7), mc * 1.3),
         * mc = apply_structure_attack_speed_effects(stats.cooldown). */
        int base = (E->stats != BT_ITEM_EFF_NONE && E->stats < BT_NITEM_WSTATS)
                       ? (int)BT_ITEM_WSTATS[E->stats].cooldown
                       : 0;
        float mc = (float)bt_apply_structure_attack_speed(s, base);
        float lo = mc * 0.7f;
        if (lo < 1.0f) lo = 1.0f;
        float hi = mc * 1.3f;
        s->pet_timer[p] = bt_rand_range(s, lo, hi) / 60.0f;
    }
}

/* The order-free half of the garden tick: for every live is_spawning turret
 * whose cooldown reaches zero this frame, set bit (p>>5, p&31) of ready_words
 * -- CEIL(BT_MAX_PET/32) unsigned ints the CALLER zeroes.  It does NOT
 * decrement pet_timer (the tick does that exactly once), so any schedule may run
 * it.  `!(timer - BT_DT > 0)` is the exact negation of the tick's own
 * `timer > 0` test after the decrement.  `_range` restricts the walk to pet slots
 * [p_lo, p_hi): each slot owns its own bit, so disjoint ranges are independent. */
BT_HD static void bt_structure_spawning_scan_range(BT_DEVICE BtState* s,
                                                   BT_DEVICE unsigned int* ready_words,
                                                   int p_lo, int p_hi) {
    if (p_lo < 0) p_lo = 0;
    if (p_hi > s->pet_n) p_hi = s->pet_n;
    for (int p = p_lo; p < p_hi; p++) {
        if (!s->pet_alive[p] || s->pet_opcode[p] != BT_ITEMEFF_TURRET) continue;
        if (!BT_ITEM_EFF_ROWS[s->pet_row[p]].is_spawning) continue;
        if (!(s->pet_timer[p] - BT_DT > 0.0f)) ready_words[p >> 5] |= 1u << (p & 31u);
    }
}

/* The whole pet range (the serial reference partitioning). */
BT_HD static void bt_structure_spawning_scan(BT_DEVICE BtState* s,
                                             BT_DEVICE unsigned int* ready_words) {
    bt_structure_spawning_scan_range(s, ready_words, 0, s->pet_n);
}

/* Serial reference entry point used by bt_core_step (ready_words == NULL: the
 * readiness decision is made inline). */
BT_HD static void bt_structure_spawning_tick(BT_DEVICE BtState* s) {
    bt_structure_spawning_tick_cand(s, (BT_CONST_PTR(unsigned int))0);
}
