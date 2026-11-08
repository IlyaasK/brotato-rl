/* brotato_loot.h -- source loot drop / consumable selection / deferred crate
 * ITEM selection for the shared Brotato Bull/Danger-5 core.
 *
 * Owner: LootLifecycle.  Every rule is a transcription of the recovered Godot
 * sources; each function cites its source lines.  This header contains no
 * tables of its own -- it walks the existing shared metadata
 * (BT_ITEMS / BtItem in brotato_items.h, BtUnitKind in brotato_waves.h) and
 * reuses the core's RNG primitives, bt_spawn_material, bt_item_shop_ok and
 * bt_item_has_wanted_tag.  See INTEGRATION at the bottom of this file for the
 * exact include site and the parent-owned state the callers must add.
 *
 * Sources:
 *   main.gd:598-624        spawn_loot / spawn_consumables
 *   main.gd:665-741        on_consumable_picked_up (item_box_gold) / spawn_gold
 *   main.gd:751-789        get_gold_value
 *   singletons/item_service.gd:60-203   _tiers_data / init_unlocked_pool /
 *                                       get_consumable_to_drop / get_consumable_for_tier
 *   singletons/item_service.gd:207-217  process_item_box
 *   singletons/item_service.gd:299-465  _get_rand_item_for_wave
 *   singletons/item_service.gd:441-448  get_limited_items
 *   singletons/item_service.gd:520-547  get_tier_from_wave
 *   singletons/run_data.gd:437-442      sum_all_player_effects
 *   singletons/run_data.gd:775-783      get_endless_factor
 *   singletons/utils.gd:391-403         get_rand_element / get_chance_success
 *   singletons/utils.gd:406-411         get_curse_factor
 *   items/consumables/<name>/<name>_data.tres
 *                                        fruit (tier 0), item_box (tier 1),
 *                                        legendary_item_box (tier 3)
 *   items/materials/gold.gd              INITIAL_VALUE 1
 *   dlcs/dlc_1/dlc_1_data.gd:33-43,82    update_item_effects curse roll
 *
 * RNG: the recovered game keeps two independent PCG32 streams -- the GDScript
 * global (Utils.get_chance_success/randf/rand_range/pick_random) and Utils._rng
 * (Utils.randi_range).  Both already exist in BtState (s->rng, s->utils_rng);
 * this header routes every draw to the same stream the source used, INCLUDING
 * the dropped-item spawn draws (sprite pick, push-back distance, destination,
 * rotation) and the per-gold instant-attract roll.
 *
 * DROPPED-ITEM LIFECYCLE: gold and consumables both extend items/global/item.gd
 * (materials/gold.gd, consumables/consumable.gd), so both follow the same
 * push-back -> idle -> attract state machine.  bt_loot_item_place/bt_loot_item_step
 * transcribe item.gd:26-70 exactly (no extra timers: idle_time is the source's
 * 10 s, the push-back stop radius the source's 20).  The per-item state lives in
 * the caller's BtState (see INTEGRATION; this header takes it as pointers so it
 * stays independent of the state layout).
 */
#pragma once

/* ---------------------------------------------------------------------------
 * Consumable kinds -- BtState.conkind's existing encoding, one per generated
 * ConsumableData resource (all are unlocked_by_default -> always in a tier
 * pool).  min/max_consumable_tier on every enemy kind is (0,0), so the base
 * roll only ever reaches tier 0 (COMMON = fruit); a successful item_chance
 * roll overrides to UNCOMMON (item_box) or, on a boss before the endless
 * waves, LEGENDARY (legendary_item_box).
 * ------------------------------------------------------------------------- */
#define BT_CONS_FRUIT 0           /* consumable_fruit,            tier 0, heal BT_HEAL */
#define BT_CONS_ITEM_BOX 1        /* consumable_item_box,         tier 1, heal BT_HEAL */
#define BT_CONS_LEGENDARY_BOX 2   /* consumable_legendary_item_box, tier 3, heal BT_LEGENDARY_HEAL */
#define BT_CONS_NONE (-1)

/* item_service.gd Tier enum (items/global/tier.gd) = the index into
 * _tiers_data and into BtItem.tier. */
#define BT_LOOT_TIER_COMMON 0
#define BT_LOOT_TIER_UNCOMMON 1
#define BT_LOOT_TIER_RARE 2
#define BT_LOOT_TIER_LEGENDARY 3

/* main.gd:21-22 */
#define BT_LOOT_MAX_GOLDS 50
#define BT_LOOT_MIN_GOLD_CHANCE 0.5f
/* items/materials/gold.gd:5 INITIAL_VALUE: every spawned gold is worth 1. */
#define BT_LOOT_GOLD_VALUE 1

/* item_service.gd:24-25 */
#define BT_LOOT_WANTED_TAG_CHANCE 0.05f
/* dlcs/dlc_1/dlc_data.tres (the SHIPPED instance of dlcs/dlc_1/dlc_1_data.gd;
 * the script's 0.03 / 0.03 / 0.01 defaults are all overridden): base_item_curse
 * and base_cursed_chest are 0.0 -- so a curse-free run rolls NO item curse draw
 * at all -- and max_curse_item_chance is 0.15.  Both curse sites read these
 * through the generated BT_DLC1_CURSE_* names (brotato_item_effects.h). */
#define BT_LOOT_BASE_ITEM_CURSE_CHANCE BT_DLC1_CURSE_BASE_ITEM_CURSE_CHANCE
#define BT_LOOT_MAX_CURSE_ITEM_CHANCE BT_DLC1_CURSE_MAX_CURSE_ITEM_CHANCE

/* items/global/item.gd:5-18 (materials/gold.gd and consumables/consumable.gd
 * inherit every one of these). */
#define BT_LOOT_ITEM_ATTRACT_SPEED 500.0f     /* INITIAL_ATTRACT_SPEED */
#define BT_LOOT_ITEM_ATTRACT_ACCEL 20.0f      /* ATTRACT_ACCELERATION */
#define BT_LOOT_ITEM_PUSH_BACK_SPEED 5.0f     /* _push_back_speed */
#define BT_LOOT_ITEM_IDLE_TIME 10.0f          /* idle_time_after_pushed_back (decremented by 60*delta) */
#define BT_LOOT_ITEM_PUSH_STOP2 400.0f        /* distance_squared_to() > 400 stops the push-back */
#define BT_LOOT_ITEM_SNAP_EPS 1e-5f           /* Vector2.move_toward CMP_EPSILON snap */
#define BT_LOOT_TWO_PI 6.2831853f             /* 2 * PI, as the drop rotation bound */
/* main.tscn gold_sprites export size (11 textures); gold_sprites.pick_random()
 * consumes one global draw regardless. */
#define BT_LOOT_GOLD_SPRITES 11
/* GoldBag's OWN gold_pickup_sounds array (items/materials/gold_bag.tscn:23, 5
 * sounds -- a different array from Main's).  gold_bag.gd:_on_GoldBag_area_entered
 * runs `gold.pickup(-1)` and then SoundManager.play(Utils.get_rand_element(
 * gold_pickup_sounds), 0, 0.2): the -1 makes on_gold_picked_up return before its
 * OWN sound branch, but the BAG's pick is one global draw per gold collected.
 * The array is non-empty, so the draw happens in both audio modes (it is the
 * Array.pick_random that consumes the word, before SoundManager.play runs). */
#define BT_LOOT_GOLD_BAG_SOUNDS 5

/* One dropped item's item.gd lifecycle state, stored as a small float vector:
 *   [0] push_back_destination.x   [1] push_back_destination.y
 *   [2] idle_time_after_pushed_back   [3] _current_speed
 *   [4] push_back (0/1)           [5] monitorable (0/1)
 * The position/attractor stay in the caller's existing fields (BtState.matx/
 * maty, conx/cony, mat_by, ...). */
#define BT_LOOT_ARC_N 6
#define BT_LOOT_ARC_DEST_X 0
#define BT_LOOT_ARC_DEST_Y 1
#define BT_LOOT_ARC_IDLE 2
#define BT_LOOT_ARC_SPEED 3
#define BT_LOOT_ARC_PUSH 4
#define BT_LOOT_ARC_MON 5

/* The Bull has no crate_chance producer (keys.gd's crate_chance_hash is only
 * defaulted to 0 by dlc_data.tres) and no boosted_wanted_item_tag producer, so
 * those two branches are constant folded; they are called out at their sites. */

/* ---------------------------------------------------------------------------
 * run_data.gd:775-783 get_endless_factor.  The inner quotient is integer
 * division ((endless_wave * (endless_wave + 1)) / 2 on ints), then / 100.0.
 * ------------------------------------------------------------------------- */
BT_HD static float bt_loot_endless_factor(int wave) {
    int endless_wave = wave - 20;
    if (endless_wave < 0) endless_wave = 0;
    float endless_mult = 2.0f;
    if (wave > 35) endless_mult += (float)(wave - 35) * 0.2f;
    float tri = (float)(endless_wave * (endless_wave + 1) / 2);
    return bt_maxf(0.0f, tri / 100.0f) * endless_mult;
}

/* run_data.gd:437-442 sum_all_player_effects / get_player_count: the single
 * Bull contributes its whole raw stat. */
BT_HD static float bt_loot_luck(BT_DEVICE BtState* s) { return s->luck * 0.01f; }

/* ---------------------------------------------------------------------------
 * main.gd:624-637 spawn_consumables' item_chance.
 *   item_chance = (stats.item_drop_chance * (1 + luck)) / (1 + items_spawned)
 *   item_chance += item_chance * sum(crate_chance)/100        [crate_chance == 0]
 *   if always_drop_consumables and item_drop_chance >= 1 and wave <= nb_of_waves:
 *       item_chance = 1
 * `items_spawned_this_wave` is Main._items_spawned_this_wave (main.gd:48, 640);
 * a fresh Main scene per wave means it starts at 0 every wave.
 * ------------------------------------------------------------------------- */
BT_HD static float bt_loot_item_chance(BT_DEVICE BtState* s, BT_CONST_PTR(BtUnitKind) K,
                                       int items_spawned_this_wave, int wave) {
    float luck = bt_loot_luck(s);
    float item_chance =
        (K->item_drop_chance * (1.0f + luck)) / (1.0f + (float)items_spawned_this_wave);
    /* crate_chance: no producer in the recovered sources (always 0). */
    if (K->always_drop_consumables && K->item_drop_chance >= 1.0f && wave <= BT_WAVES)
        item_chance = 1.0f;
    return item_chance;
}

/* ---------------------------------------------------------------------------
 * item_service.gd:190-203 get_consumable_for_tier: the tier pool holds exactly
 * one unlocked consumable per non-empty tier (fruit / item_box /
 * legendary_item_box); tier 2 has none, and Utils.get_rand_element returns null
 * WITHOUT drawing for an empty array.  Array.pick_random() consumes one global
 * draw even for a one-element array, so a non-empty tier consumes one.
 * The DLC update_consumable_to_get poisoned-fruit swap is not modelled: its
 * only producer is the druid character's effect (dlc_1 druid_effect_3), so it
 * is always 0 for the Bull and consumes no draw (dlc_1_data.gd:48-58).
 * ------------------------------------------------------------------------- */
BT_HD static int bt_loot_consumable_for_tier(BT_DEVICE BtState* s, int tier) {
    int kind;
    if (tier == BT_LOOT_TIER_COMMON) kind = BT_CONS_FRUIT;
    else if (tier == BT_LOOT_TIER_UNCOMMON) kind = BT_CONS_ITEM_BOX;
    else if (tier == BT_LOOT_TIER_LEGENDARY) kind = BT_CONS_LEGENDARY_BOX;
    else return BT_CONS_NONE;   /* empty pool: null, no draw */
    (void)bt_rng_bounded(&s->rng, 1u);   /* Array.pick_random over the 1-element pool */
    return kind;
}

/* ---------------------------------------------------------------------------
 * item_service.gd:157-188 get_consumable_to_drop, called from spawn_consumables
 * (main.gd:636).  Returns the BtState.conkind to create, or BT_CONS_NONE.
 *   drop_chance = min(1, stats.base_drop_chance * (1 + luck))
 *   if wave > nb_of_waves: drop_chance /= (1 + endless_factor)
 *   if chance_success(drop_chance) or always_drop_consumables:
 *       tier = Utils.randi_range(min_consumable_tier, max_consumable_tier)  [Utils stream]
 *       if chance_success(item_chance):
 *           tier = LEGENDARY if (is_boss and wave <= nb_of_waves) else UNCOMMON
 *   elif chance_success(sum(enemy_fruit_drops)/100):
 *       tier = COMMON
 * DebugService.always_drop_crates is off in the recovered run and is not modelled.
 * ------------------------------------------------------------------------- */
BT_HD static int bt_loot_roll_consumable(BT_DEVICE BtState* s, BT_CONST_PTR(BtUnitKind) K,
                                         float item_chance, int is_boss, int wave) {
    float luck = bt_loot_luck(s);
    float drop_chance = K->base_drop_chance * (1.0f + luck);
    if (drop_chance > 1.0f) drop_chance = 1.0f;
    if (wave > BT_WAVES) drop_chance /= (1.0f + bt_loot_endless_factor(wave));

    /* The left operand is evaluated first, so the draw happens even when
     * always_drop_consumables would short-circuit the branch open. */
    int rolled = bt_chance(s, drop_chance);
    if (rolled || K->always_drop_consumables) {
        int tier = bt_rng_randi_range(&s->utils_rng, (int)K->min_tier, (int)K->max_tier);
        if (bt_chance(s, item_chance)) {
            if (is_boss && wave <= BT_WAVES) tier = BT_LOOT_TIER_LEGENDARY;
            else tier = BT_LOOT_TIER_UNCOMMON;
        }
        return bt_loot_consumable_for_tier(s, tier);
    }
    /* enemy_fruit_drops has no producer for the Bull (0 -> bt_chance draws
     * nothing), but the branch is kept source-shaped. */
    float fruit = (float)(int)s->scalar_effect[BT_ISTR_ENEMY_FRUIT_DROPS] * 0.01f;
    if (bt_chance(s, fruit)) return bt_loot_consumable_for_tier(s, BT_LOOT_TIER_COMMON);
    return BT_CONS_NONE;
}

/* A pickup of an item box / legendary box increments
 * Main._items_spawned_this_wave (main.gd:639-640); a fruit does not. */
BT_HD static int bt_loot_counts_toward_wave(int kind) {
    return kind == BT_CONS_ITEM_BOX || kind == BT_CONS_LEGENDARY_BOX;
}

/* ---------------------------------------------------------------------------
 * main.gd:751-789 get_gold_value for the single-player Bull:
 *   value += value * coop_factor                          [1 player -> 0]
 *   gold_drops/enemy_gold_drops/neutral_gold_drops are summed effects / players
 *   ENEMY   -> + (gold_drops + enemy_gold_drops)
 *   NEUTRAL -> + (gold_drops + neutral_gold_drops)
 *   value = max(value, MIN_GOLD_CHANCE * base_value)
 * The unit effect_behavior gold modifier sum and scale_materials_with_distance
 * have no producer reachable by the Bull (the base
 * UnitEffectBehavior.get_gold_value_modifier is 0 and no subclass overrides it;
 * scale_materials_with_distance is not interned), so both are omitted.
 * `neutral` selects the entity-specific bucket; enemy_gold_drops is 0 for the
 * Bull but is read for fidelity, neutral_gold_drops has no producer.
 * ------------------------------------------------------------------------- */
BT_HD static float bt_loot_gold_value(BT_DEVICE BtState* s, int neutral, float base_value) {
    float value = base_value;
    int gold_drops = (int)s->scalar_effect[BT_ISTR_GOLD_DROPS];
    int specific = 0;
    if (!neutral) specific = (int)s->scalar_effect[BT_ISTR_ENEMY_GOLD_DROPS];
    value += value * (float)(gold_drops + specific) / 100.0f;
    float floor_value = BT_LOOT_MIN_GOLD_CHANCE * base_value;
    if (value < floor_value) value = floor_value;
    return value;
}

/* main.gd:600-620 spawn_loot's gold gate.  Only ENEMY rolls it; a NEUTRAL drops
 * its value unconditionally.  wave_factor = wave*0.015; horde waves scale by
 * 0.65; always_drop_consumables forces 1.0 (which still consumes the draw). */
BT_HD static int bt_loot_gold_drop(BT_DEVICE BtState* s, BT_CONST_PTR(BtUnitKind) K,
                                   int neutral, int wave, int horde) {
    if (neutral) return 1;
    float chance = wave < 5 ? 1.0f : (1.0f - (float)wave * 0.015f);
    if (chance < 0.5f) chance = 0.5f;
    if (horde) chance *= 0.65f;
    if (K->always_drop_consumables) chance = 1.0f;
    return bt_chance(s, chance);
}

/* main.gd:618 gold_spread = clamp((value - 1) * 25, stats.gold_spread, 200).
 * It is the second bound of the drop distance below. */
BT_HD static int bt_loot_gold_spread(BT_CONST_PTR(BtUnitKind) K, float value) {
    return (int)bt_clampf((value - 1.0f) * 25.0f, (float)K->gold_spread, 200.0f);
}

/* main.gd:700-704 spawn_gold's per-gold count.
 *   value_floored = int(value); residual = value - value_floored
 *   spawn_count = value_floored (+1 if chance_success(residual))
 * Each created gold carries Gold.INITIAL_VALUE (1): the fractional value is
 * resolved into a NUMBER of one-material gold entities, not one big coin. */
BT_HD static int bt_loot_gold_count(BT_DEVICE BtState* s, float value) {
    int floored = (int)value;                 /* GDScript int(): truncates toward zero */
    float residual = value - (float)floored;
    return floored + (bt_chance(s, residual) ? 1 : 0);
}

/* main.gd:706-714: once MAX_GOLDS are alive the source adds INITIAL_VALUE (and
 * a scale bump) to a RANDOM live gold instead of creating another.
 * Utils.get_rand_element(_active_golds) consumes one global draw; returns the
 * chosen index in [0, n_active) so the caller can add BT_LOOT_GOLD_VALUE to that
 * gold's value. */
BT_HD static int bt_loot_gold_boost_choose(BT_DEVICE BtState* s, int n_active) {
    return (int)bt_rng_bounded(&s->rng, (unsigned int)n_active);
}

/* ZoneService.get_rand_pos_in_area(base_pos, area, 0) -- zone_service.gd:117-126
 * with _limit_dist_to_edge(0) == 0.  Two global rand_range draws.  The core's
 * single zone is (0,0)..(BT_ARENA_W,BT_ARENA_H). */
BT_HD static void bt_loot_rand_pos_in_area(BT_DEVICE BtState* s, float bx, float by, float area,
                                           BT_THREAD float* ox, BT_THREAD float* oy) {
    float extent = area * 0.5f;
    float min_x = extent, max_x = BT_ARENA_W - extent;
    float min_y = extent, max_y = BT_ARENA_H - extent;
    float px = bt_clampf(bx, min_x, max_x);
    float py = bt_clampf(by, min_y, max_y);
    *ox = bt_rand_range(s, px - extent, px + extent);
    *oy = bt_rand_range(s, py - extent, py + extent);
}

/* item.gd:26-45: reset() then drop(), in that order.  Every pooled item is
 * reset when it is picked up (item.gd:73-75) and a freshly instanced one is
 * reset from _ready (item.gd:21-22), so a dropped item is always in the reset
 * state -- the caller just writes the new destination. */
BT_HD static void bt_loot_item_place(BT_DEVICE float* arc, float dest_x, float dest_y) {
    arc[BT_LOOT_ARC_DEST_X] = dest_x;
    arc[BT_LOOT_ARC_DEST_Y] = dest_y;
    arc[BT_LOOT_ARC_IDLE] = BT_LOOT_ITEM_IDLE_TIME;
    arc[BT_LOOT_ARC_SPEED] = BT_LOOT_ITEM_ATTRACT_SPEED;
    arc[BT_LOOT_ARC_PUSH] = 1.0f;
    arc[BT_LOOT_ARC_MON] = 0.0f;
}

/* item.gd:48-70 Item._physics_process for one 1/60 s frame.
 *   push_back && dist_sq > 400 : interpolate toward push_back_destination by
 *                                delta * 5
 *   idle > 0                   : monitorable = true, push_back = false,
 *                                idle -= 60 * delta
 *   push_back                  : push_back = false
 *   attracted_by != null       : clear it when the attractor is dead; else
 *                                move_toward(attractor, delta * _current_speed)
 *                                and _current_speed += 20
 *   else                       : set_physics_process(false)
 * idle_time_after_pushed_back starts at 10.0 and is decremented by 60*delta,
 * i.e. one unit per 60 Hz frame, so the idle phase is 10 frames -- the unit is
 * the source's, transcribed verbatim, not re-timed.
 * `attracted` is attracted_by != null and is cleared in place when the attractor
 * is dead.  Returns 1 while physics processing stays enabled, 0 when the source
 * would call set_physics_process(false) (the item stays monitorable and can be
 * picked up or re-attracted). */
BT_HD static int bt_loot_item_step(BT_DEVICE float* x, BT_DEVICE float* y,
                                   BT_DEVICE float* arc,
                                   BT_DEVICE unsigned char* attracted,
                                   int attractor_dead, float tx, float ty, float delta) {
    float dx = *x - arc[BT_LOOT_ARC_DEST_X];
    float dy = *y - arc[BT_LOOT_ARC_DEST_Y];
    if (arc[BT_LOOT_ARC_PUSH] != 0.0f && dx * dx + dy * dy > BT_LOOT_ITEM_PUSH_STOP2) {
        float t = delta * BT_LOOT_ITEM_PUSH_BACK_SPEED;
        *x += (arc[BT_LOOT_ARC_DEST_X] - *x) * t;
        *y += (arc[BT_LOOT_ARC_DEST_Y] - *y) * t;
    } else if (arc[BT_LOOT_ARC_IDLE] > 0.0f) {
        arc[BT_LOOT_ARC_MON] = 1.0f;
        arc[BT_LOOT_ARC_PUSH] = 0.0f;
        arc[BT_LOOT_ARC_IDLE] -= 60.0f * delta;
    } else if (arc[BT_LOOT_ARC_PUSH] != 0.0f) {
        arc[BT_LOOT_ARC_PUSH] = 0.0f;
    } else if (*attracted != 0) {
        if (attractor_dead) {
            *attracted = 0;
            arc[BT_LOOT_ARC_SPEED] = BT_LOOT_ITEM_ATTRACT_SPEED;
        } else {
            /* Vector2.move_toward(to, delta * _current_speed): snap exactly onto
             * the target once it is within reach (or effectively coincident),
             * else step along the normalised direction. */
            float ddx = tx - *x, ddy = ty - *y;
            float d = bt_sqrtf(ddx * ddx + ddy * ddy);
            float step = delta * arc[BT_LOOT_ARC_SPEED];
            if (d <= step || d < BT_LOOT_ITEM_SNAP_EPS) { *x = tx; *y = ty; }
            else { *x += ddx / d * step; *y += ddy / d * step; }
            arc[BT_LOOT_ARC_SPEED] += BT_LOOT_ITEM_ATTRACT_ACCEL;
        }
    } else {
        return 0;
    }
    return 1;
}

/* main.gd:718-758: the spawn draws (+ drop state) for ONE gold, in source
 * order.  No entity is created here -- the caller allocates the slot, puts the
 * gold at (x,y) with value BT_LOOT_GOLD_VALUE, and makes the instant-attract
 * roll AFTER these draws (main.gd:738-746).
 *   gold_sprites.pick_random()                          (1 global draw)
 *   dist = rand_range(50, 100 + spread)                 (3 draws)
 *   push_back_destination = get_rand_pos_in_area(pos, dist, 0) (6 draws)
 *   rotation = rand_range(0, 2*PI)                      (3 draws)
 * `arc` receives the item.gd drop state (destination + reset values).
 * RunData.bonus_gold > 0 (main.gd:720-726) is skipped: convert_bonus_gold is
 * produced by no recovered item (not even interned), so bonus_gold is always 0
 * for the Bull and that branch draws nothing.
 * _get_shuffled_live_players() shuffles a one-element array -- zero draws. */
BT_HD static void bt_loot_gold_arc(BT_DEVICE BtState* s, float x, float y, int spread,
                                   BT_DEVICE float* arc,
                                   BT_THREAD float* angle) {
    (void)bt_rng_bounded(&s->rng, BT_LOOT_GOLD_SPRITES);   /* gold_sprites.pick_random() */
    float dist = bt_rand_range(s, 50.0f, 100.0f + (float)spread);
    float dest_x, dest_y;
    bt_loot_rand_pos_in_area(s, x, y, dist, &dest_x, &dest_y);
    *angle = bt_rand_range(s, 0.0f, BT_LOOT_TWO_PI);
    bt_loot_item_place(arc, dest_x, dest_y);
}

/* main.gd:631-645 spawn_consumables' placement (consumable.gd sets no rotation
 * and no sprite draw -- the texture comes from consumable_data.icon):
 *   dist = rand_range(50, 100 + unit.stats.gold_spread)         (3 draws)
 *   push_back_destination = get_rand_pos_in_area(pos, dist, 0)  (6 draws)
 *   consumable.drop(pos, 0, push_back_destination)              (no rotation draw) */
BT_HD static void bt_loot_consumable_arc(BT_DEVICE BtState* s, float x, float y,
                                         int gold_spread, BT_DEVICE float* arc) {
    float dist = bt_rand_range(s, 50.0f, 100.0f + (float)gold_spread);
    float dest_x, dest_y;
    bt_loot_rand_pos_in_area(s, x, y, dist, &dest_x, &dest_y);
    bt_loot_item_place(arc, dest_x, dest_y);
}

/* main.gd:700-758 spawn_gold for the whole kill: value's floor + residual roll
 * decide how many one-material gold entities are created; each is dropped with
 * bt_loot_gold_arc and, past MAX_GOLDS, a random live gold is boosted instead
 * (main.gd:706-714).  Slots are the caller's BtState material arrays (the
 * first-free-slot convention shared with bt_spawn_material, which performs the
 * entity creation and the post-drop instant-attract roll).  A full material
 * table sets pend_overflow -- an explicit, observable failure, never a silent
 * drop.  `spread` is bt_loot_gold_spread(K, value) from spawn_loot (main.gd:618). */
BT_HD static void bt_loot_spawn_gold(BT_DEVICE BtState* s, float value, float x, float y,
                                     int spread) {
    int count = bt_loot_gold_count(s, value);
    for (int i = 0; i < count; i++) {
        if (s->matcount >= BT_LOOT_MAX_GOLDS) {
            int k = bt_loot_gold_boost_choose(s, s->matcount);
            /* main.gd:706-714 Utils.get_rand_element(_active_golds): the pick is
             * the k-th live gold in INSERTION order, not slot order. */
            int m = bt_loot_gold_ordered_slot(s, k);
            unsigned int boosted = (unsigned int)s->matval[m] + BT_LOOT_GOLD_VALUE;
            s->matval[m] = (unsigned short)(boosted > 65535u ? 65535u : boosted);
            continue;
        }
        int slot = -1;
        for (int m = 0; m < BT_MAX_MAT; m++) if (!s->matalive[m]) { slot = m; break; }
        if (slot < 0) { s->pend_overflow = 1; s->pend_drop_mat++; return; }
        float angle;
        bt_loot_gold_arc(s, x, y, spread, s->mat_arc[slot], &angle);
        (void)angle;   /* renderer rotation; consumed to keep the stream aligned */
        bt_spawn_material(s, x, y, BT_LOOT_GOLD_VALUE);   /* same first free slot */
        s->mat_attr[slot] = 0;                            /* attracted_by = null */
    }
}

/* ---------------------------------------------------------------------------
 * item_service.gd:105-135 init_unlocked_pool.  The pool test is
 * can_be_looted AND max_nb != 0.
 *
 * FULL-UNLOCK ASSUMPTION: the source additionally requires
 * ProgressData.items_unlocked.has(item.my_id_hash) (progress_data.gd:893 seeds
 * it from unlocked_by_default, or ProgressData.unlock_all_items() from
 * everything).  The current environment models a fully unlocked profile -- every
 * item in BT_ITEMS is treated as unlocked -- so that test is dropped here and
 * this helper keys off can_be_looted/max_nb alone.  If a live-game profile is
 * modelled later, add the unlocked predicate at this single point (the item
 * table already carries unlocked_by_default for it).
 * The pool's order is the item_service.tscn `items` array order, which is
 * BT_ITEMS' order, so a filtered walk over BT_ITEMS reproduces
 * Array.pick_random's index map.
 * ------------------------------------------------------------------------- */
BT_HD static int bt_loot_item_pool_ok(BT_DEVICE BtState* s, unsigned short it) {
    BT_CONST_PTR(BtItem) item = &BT_ITEMS[it];
    /* progress_data.gd:884-895 add_unlocked_by_default + unlock_all_items: the
     * _tiers_data membership test is can_be_looted && max_nb != 0 &&
     * items_unlocked.has(my_id_hash).  lock_default_items selects the fresh-save
     * profile (only unlocked_by_default items); 0 is the fully unlocked one. */
    if (s->lock_default_items && !item->unlocked_by_default) return 0;
    return item->can_be_looted && item->max_nb != 0;
}

/* item_service.gd:441-448 get_limited_items: a finite-max_nb item is dropped
 * once the player owns max_nb copies (owned_and_shop_items multiplicity). */
BT_HD static int bt_loot_item_limited(BT_DEVICE BtState* s, unsigned short it) {
    BT_CONST_PTR(BtItem) item = &BT_ITEMS[it];
    return item->max_nb > 0 && (int)s->item_owned[it] >= (int)item->max_nb;
}

/* item_service.gd:424-436: use the active profile before the endless ban swap. */
BT_HD static int bt_loot_item_banned(BT_DEVICE BtState* s, int it, int wave) {
    if (wave < BT_WAVES) return !bt_item_shop_ok(s->character, (unsigned short)it);
    for (int b = 0; b < BT_ENDLESS_BANNED_ITEMS_N; b++)
        if ((int)BT_ENDLESS_BANNED_ITEMS[b] == it) return 1;
    return 0;
}

/* ---------------------------------------------------------------------------
 * item_service.gd:520-547 get_tier_from_wave: one rand_range(0,1) draw (3 draws
 * in binary64), then _tiers_data is scanned from LEGENDARY down to COMMON.
 * Returns Tier.COMMON..LEGENDARY (0..3) -- the same value space as BtItem.tier.
 * ------------------------------------------------------------------------- */
BT_HD static int bt_loot_item_tier(BT_DEVICE BtState* s, int wave) {
    float draw = bt_rand_range(s, 0.0f, 1.0f);
    float luck = bt_loot_luck(s);
    int tier = BT_LOOT_TIER_COMMON;
    for (int i = 3; i >= 0; i--) {
        int min_wave; float base, bonus, limit;
        if (i == 3)      { min_wave = 6; base = 0.0f; bonus = 0.0023f; limit = 0.08f; }
        else if (i == 2) { min_wave = 2; base = 0.0f; bonus = 0.02f;   limit = 0.25f; }
        else if (i == 1) { min_wave = 0; base = 0.0f; bonus = 0.06f;   limit = 0.6f;  }
        else             { min_wave = 0; base = 1.0f; bonus = 0.0f;    limit = 1.0f;  }
        float wave_base = (float)(wave - 1 - min_wave) * bonus;
        if (wave_base < 0.0f) wave_base = 0.0f;
        float wave_chance = luck >= 0.0f ? wave_base * (1.0f + luck)
                                         : wave_base / (1.0f + bt_fabsf(luck));
        float chance = base + wave_chance;
        if (draw <= bt_minf(chance, limit)) { tier = i; break; }
    }
    return tier;
}

/* _get_rand_item_for_wave's pool levels (item_service.gd:310-450):
 *   level 0 = full tier pool  = _tiers_data[tier][ITEMS]          (last resort)
 *   level 1 = backup_pool     = full minus limited
 *   level 2 = pool            = backup minus banned minus wanted-tag failures
 * args.excluded_items and args.forced_shop_tag are empty for a crate, the
 * run-level players_data.banned_items list is empty for a fresh Bull run, and
 * remove_shop_items is produced by no item in the recovered pool (it is not
 * even interned), so none of them filter the pool. */
BT_HD static int bt_loot_pool_member(BT_DEVICE BtState* s, int it, int tier, int wave,
                                     int wanted, int level) {
    if ((int)BT_ITEMS[it].tier != tier) return 0;
    if (!bt_loot_item_pool_ok(s, (unsigned short)it)) return 0;
    if (level >= 1 && bt_loot_item_limited(s, (unsigned short)it)) return 0;
    if (level >= 2) {
        if (bt_loot_item_banned(s, it, wave)) return 0;
        if (wanted && BT_CHARACTERS[s->character].want_count > 0 &&
            !bt_character_wants_item(s->character, (unsigned short)it)) return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------------------
 * item_service.gd:207-217 process_item_box -> :299-450 _get_rand_item_for_wave
 * (TierData.ITEMS).  `fixed_tier` is Tier.LEGENDARY for the legendary item box
 * and -1 otherwise; the tier roll is still evaluated under fixed_tier (it only
 * overrides the result).  Returned index is a BT_ITEMS index or BT_ITEM_NONE.
 *
 * Draw order (global stream): rand_wanted (1), get_tier_from_wave (3), the
 * wanted-tag roll (1), the pick (1); then, only when the pick is the axolotl,
 * one randf() and a second pick.  The DLC curse roll (bt_loot_item_cursed) is
 * the next draw, made by apply_item_effect_modifications before the item is
 * applied (dlc_1_data.gd:33-43).
 * ------------------------------------------------------------------------- */
BT_HD static int bt_loot_pick_box_item(BT_DEVICE BtState* s, int wave, int fixed_tier) {
    (void)bt_math_randf(&s->rng);                 /* rand_wanted := randf() (:303) */
    int tier = bt_loot_item_tier(s, wave);        /* (:303) */
    if (fixed_tier >= 0) tier = fixed_tier;       /* (:306) */
    /* wanted_item_tag_chance: boosted_wanted_item_tag has no Bull producer. */
    int wanted = bt_chance(s, BT_LOOT_WANTED_TAG_CHANCE);   /* (:392-395) */

    int n[3];
    for (int lv = 0; lv < 3; lv++) {
        int c = 0;
        for (int i = 0; i < BT_NITEMS; i++)
            c += bt_loot_pool_member(s, i, tier, wave, wanted, lv);
        n[lv] = c;
    }
    int level = n[2] ? 2 : (n[1] ? 1 : 0);
    int count = n[level];
    if (count <= 0) return (int)BT_ITEM_NONE;

    int pick = (int)bt_rng_bounded(&s->rng, (unsigned int)count);   /* (:437) */
    int chosen = (int)BT_ITEM_NONE, seen = 0;
    for (int i = 0; i < BT_NITEMS; i++) {
        if (!bt_loot_pool_member(s, i, tier, wave, wanted, level)) continue;
        if (seen++ == pick) { chosen = i; break; }
    }
    /* item_service.gd:431-432: only when the primary pool was used, the
     * axolotl has a 50% chance to be rerolled from that same pool.  The
     * backup/full fallback path performs no reroll (and draws nothing). */
    if (level == 2 && chosen == BT_ITEM_IDX_AXOLOTL && bt_math_randf(&s->rng) < 0.5f) {
        int repick = (int)bt_rng_bounded(&s->rng, (unsigned int)count);
        int rseen = 0;
        for (int i = 0; i < BT_NITEMS; i++) {
            if (!bt_loot_pool_member(s, i, tier, wave, wanted, level)) continue;
            if (rseen++ == repick) { chosen = i; break; }
        }
    }
    return chosen;
}

/* ---------------------------------------------------------------------------
 * dlcs/dlc_1/dlc_1_data.gd:33-43,82 update_item_effects: apply_item_effect_
 * modifications rolls a curse on every generated item (shop offers use the same
 * numbers, brotato_weapon_shop.h bt_wshop_offer_cursed).
 *   curse  = max(base_item_curse_chance*100 (=0), stat_curse)
 *   chance = max_curse_item_chance * (1 - 1/(1 + curse/50))      [== .15 * ...]
 * With the shipped base chance 0 the clamp is inert and a 0-curse run's chance
 * is exactly 0.0, which Utils.get_chance_success (:397-403) short-circuits
 * WITHOUT drawing. */
BT_HD static float bt_loot_curse_chance(BT_DEVICE BtState* s) {
    float curse = bt_maxf(s->curse, BT_LOOT_BASE_ITEM_CURSE_CHANCE * 100.0f);
    return BT_LOOT_MAX_CURSE_ITEM_CHANCE * (1.0f - 1.0f / (1.0f + curse / 50.0f));
}

/* One global draw.  The caller must set s->curse_apply to the returned flag
 * (and reset s->curse_maxmod) before bt_apply_item so the item's effects are
 * boosted exactly like a cursed shop purchase. */
BT_HD static int bt_loot_item_cursed(BT_DEVICE BtState* s) {
    return bt_chance(s, bt_loot_curse_chance(s));
}

/* Core helpers this header calls but does not define.  brotato_core.h includes
 * this header BEFORE brotato_scalar_events.h / brotato_item_explosions.h, so the
 * two pickup-effect helpers are forward-declared here exactly like
 * brotato_progression.h forward-declares the loot selectors it consumes. */
BT_HD static float bt_scalar_item_box_gold(BT_DEVICE BtState* s, int conkind);
BT_HD static float bt_scalar_consumable_heal(BT_DEVICE BtState* s, float base_heal);
BT_HD static int bt_item_expl_on_consumable(BT_DEVICE BtState* s, float x, float y);

/* ---------------------------------------------------------------------------
 * Consumable pickup effects for dropped consumable `i` (the caller has already
 * passed the monitorable / attractor / pickup-area gate).  Shared by the
 * per-frame player pickup (brotato_core.h) and the wave-end clean-up below, so
 * both paths take exactly the source's draws and effects.
 *
 * Order (main.gd:667-698 on_consumable_picked_up + consumable.gd:9-11 pickup):
 *   1. consumable.gd:9-11 SoundManager.play(Utils.get_rand_element(pickup_sounds))
 *      -- ONE global draw over the consumable's own sound list (fruit 3 entries,
 *      item boxes 1).  Item.pickup itself (emit + reset) draws nothing.
 *   2. main.gd:671-672 item_box_gold: materials only (no XP).
 *   3. item/legendary box -> _consumables_to_process (processed at the upgrade UI).
 *   4. consumable_stats_while_max (player.gd:907-923) while at full HP.
 *   5. decaying_stats_on_consumable (player.gd:925-930).
 *   6. the heal itself: consumable_healing_effect.gd:5-14.
 *   7. main.gd:693-696 explode_on_consumable at the consumable's position.
 * Effects that the source gates on `not _cleaning_up` (the temp
 * consumable_stats_while_max stat and the two explode_on_consumable signals) are
 * NOT modelled here for the wave-end path: `_cleaning_up` is true there, so the
 * source skips them too.  The `player_index >= 0` gold/consumable path in
 * Player.on_consumable_picked_up is the only caller, and it runs for the wave-end
 * attraction exactly like a walk-over pickup. */
BT_HD static void bt_loot_consumable_take(BT_DEVICE BtState* s, int i) {
    (void)bt_rng_bounded(&s->rng, s->conkind[i] == BT_CONS_FRUIT ? 3u : 1u);
    /* fruit heals 3; item boxes heal 3 and queue an item for wave end.  The
     * full-HP gate is player.gd:909/933 on_consumable_picked_up
     * (`current_stats.health >= max_stats.health`), i.e. the deferred LiveStats. */
    int was_full = s->hp >= s->p_max_hp;
    float heal = s->conkind[i] == 2 ? BT_LEGENDARY_HEAL : BT_HEAL;
    s->materials += bt_scalar_item_box_gold(s, (int)s->conkind[i]);
    if (s->conkind[i] == 1 || s->conkind[i] == 2) {
        if (s->pending_items >= BT_MAX_PENDING) { s->pend_overflow = 1; s->pend_drop_box++; }
        else s->pending_item_kind[s->pending_items++] = s->conkind[i];
    }
    s->conalive[i] = 0;
    /* #2 consumable_stats_while_max (only while at full HP) */
    if (was_full) {
        for (int a = 0; a < s->n_bk_active; a++) {
            int row = (int)s->bk_active[a];
            BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[row];
            if (!bt_eff_is(E, BT_ISTR_CONSUMABLE_STATS_WHILE_MAX)) continue;
            int id = bt_core_id_from_istr(E->key);
            if (id == 255) continue;
            if (s->nvar[row] == 0) {
                if ((float)s->cs_count[row] < (float)E->value2) {
                    bt_core_stat_add(s, id, (float)E->value);
                    s->cs_count[row] += (unsigned short)E->value;
                }
            } else {
                /* one counter per owned copy: each copy has its own cap */
                for (int c = 0; c < (int)s->nvar[row]; c++) {
                    if ((float)bt_varcs(s,row)[c] < (float)bt_rvar(s,row)[c].value2) {
                        bt_core_stat_add(s, id, (float)bt_rvar(s,row)[c].value);
                        bt_varcs(s,row)[c]++;
                    }
                }
            }
        }
    }
    /* #9 decaying_stats_on_consumable: temp stat for `value2` seconds */
    for (int a = 0; a < s->n_bk_active; a++) {
        int row = (int)s->bk_active[a];
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[row];
        if (!bt_eff_is(E, BT_ISTR_DECAYING_STATS_ON_CONSUMABLE)) continue;
        int id = bt_core_id_from_istr(E->key);
        if (id == 255) continue;
        int d2 = 0;
        float dv = (float)bt_row_dominant(s, row, E, &d2);
        int found = -1;
        for (int d = 0; d < s->n_dt; d++)
            if (s->dt_id[d] == id && !s->dt_onhit[d] && s->dt_dur[d] == (float)d2) { found = d; break; }
        if (found < 0) {
            if (s->n_dt >= 8) { s->stat_reg_overflow = 1; continue; }
            found = s->n_dt++;
            s->dt_id[found] = (unsigned char)id; s->dt_onhit[found] = 0;
            s->dt_val[found] = 0.0f; s->dt_dur[found] = (float)d2;
        }
        if (s->dt_val[found] != dv) {
            bt_core_stat_add_temp(s, id, dv - s->dt_val[found]);
            s->dt_val[found] = dv;
        }
        s->dt_remain[found] = (float)d2;
    }
    (void)bt_scalar_consumable_heal(s, heal);
    /* main.gd:693-696 on_consumable_picked_up: explode_on_consumable at
     * the consumable's own position (spicy_sauce). */
    (void)bt_item_expl_on_consumable(s, s->conx[i], s->cony[i]);
}

/* ---------------------------------------------------------------------------
 * The gold bag's world position (main.gd:1064-1072 get_gold_bag_pos):
 *   world = get_viewport().get_canvas_transform().affine_inverse()
 *                   .xform(_ui_bonus_gold_pos.global_position)
 * With the camera snapped to the lone player (MyCamera._is_snapped is true for a
 * single player) the canvas transform is a uniform scale `zoom` about the camera
 * centre, so
 *   bag = cam_centre + (hud_screen - viewport/2) / zoom.
 * MyCamera._calculate_final_position clamps the centre so the visible rect stays
 * inside the map (a map larger than the screen scrolls, a smaller one centres),
 * and the pinned viewport/zoom are the bridge's canonical eval configuration
 * (BtState.bag_view_* / bag_zoom / bag_hud_*, seeded here, overridable by the
 * host).  `hud_screen` is UI/HUD/LifeContainerP1/UIBonusGold/Position2D, i.e. a
 * fixed point in the top-left HUD column: the VBox stacks the 95x64 life bar,
 * the XP bar and the gold row before the bonus-gold row, and the Position2D sits
 * at (32,32) inside it (ui/hud/ui_bonus_gold.tscn:39-40).  All BT_LOOT_BAG_*
 * defaults live in brotato_state.h next to the fields. */

/* Host hook: pin the canonical eval configuration (the same pair the bridge
 * records in the meta).  Called once before stepping. */
BT_HD static void bt_loot_set_bag_view(BT_DEVICE BtState* s, float view_w, float view_h,
                                       float zoom, float hud_x, float hud_y) {
    s->bag_view_w = view_w > 0.0f ? view_w : BT_LOOT_BAG_VIEW_W;
    s->bag_view_h = view_h > 0.0f ? view_h : BT_LOOT_BAG_VIEW_H;
    s->bag_zoom   = zoom   > 0.0f ? zoom   : BT_LOOT_BAG_ZOOM;
    s->bag_hud_x = hud_x; s->bag_hud_y = hud_y;
}

/* The bag's world position for the current frame (see above).  BridgeGaps'
 * viewport probe confirms main.tscn:187 is a plain Camera2D with zoom (1,1) and
 * no limit_* set (its defaults do not clamp), so the camera centre is the lone
 * player's position and the canvas transform is the pure translation
 * `viewport/2 - player_pos`; the recorded ui_bonus_gold_pos pins bag_hud_x/y. */
BT_HD static void bt_loot_bag_pos(BT_DEVICE BtState* s, BT_THREAD float* bx, BT_THREAD float* by) {
    float z = s->bag_zoom > 0.0f ? s->bag_zoom : 1.0f;
    *bx = s->px + (s->bag_hud_x - s->bag_view_w * 0.5f) / z;
    *by = s->py + (s->bag_hud_y - s->bag_view_h * 0.5f) / z;
}

/* The total path a dropped item covers in `ticks` frames of item.gd's
 * attraction branch: `_current_speed` starts at INITIAL_ATTRACT_SPEED (500) and
 * gains ATTRACT_ACCELERATION (20) per frame, and each frame moves
 * _current_speed / 60 world units (delta = 1/60). */
BT_HD static float bt_loot_flight_capacity(int ticks) {
    if (ticks <= 0) return 0.0f;
    float sum = (float)ticks * BT_LOOT_ITEM_ATTRACT_SPEED
              + 0.5f * (float)ticks * (float)(ticks - 1) * BT_LOOT_ITEM_ATTRACT_ACCEL;
    return sum / 60.0f;
}

/* ---------------------------------------------------------------------------
 * main.gd:963-1010 clean_up_room's attraction set-up (the wave-end frame, after
 * manage_harvesting and before TempStats.reset):
 *   * GOLD (main.gd:1002-1004): every live gold's collision layer moves to
 *     BONUS_GOLD_BIT and its `attracted_by` becomes the GoldBag; physics stays on
 *     from that frame.  A gold that REACHES the bag runs Gold.pickup(-1) ->
 *     on_gold_picked_up with player_index == -1 (main.gd:854-859): RunData.
 *     add_bonus_gold, and the handler RETURNS before the pickup sound, the
 *     double-gold roll, the heal roll and the materials/XP loop -- NO XP.  The
 *     BAG then plays its OWN pickup sound (gold_bag.gd), so each collected gold
 *     still consumes exactly ONE global draw (see BT_LOOT_GOLD_BAG_SOUNDS).  A
 *     gold that does NOT reach the bag within the 2 s window is freed with
 *     the room (the scene changes at the EndWaveTimer timeout), i.e. LOST.
 *   * CONSUMABLE (main.gd:1005-1010): every consumable WITHOUT a damage effect is
 *     given `attracted_by = a live player` and physics on; it is then picked up
 *     through the ordinary pickup circle -> bt_loot_consumable_take (one sound
 *     draw).  One that never arrives is freed with the room.
 * Both kinds continue the item.gd push-back/idle phase they were in when physics
 * was re-enabled, so the pickup FRAME (and therefore the exact interleaving of
 * the consumable sound draws with the WaveClearedLabel draws) is simulated
 * rather than folded. */
#define BT_LOOT_ATTR_BAG 3

BT_HD static void bt_loot_wave_end_begin(BT_DEVICE BtState* s) {
    float bx, by;
    (void)bx; (void)by;
    for (int m = 0; m < (int)s->mat_used; m++) {
        if (!s->matalive[m]) continue;
        s->mat_attr[m] = BT_LOOT_ATTR_BAG;   /* attracted_by = the GoldBag */
        s->mat_home[m] = 0;                  /* no instant-attract on the bag run */
        s->mat_mob[m] = -1;
        s->mat_by[m] = 0;                    /* player gold pickup is disabled */
    }
    for (int i = 0; i < (int)s->con_used; i++) {
        if (!s->conalive[i]) continue;
        /* main.gd:1005-1010 sets attracted_by directly, bypassing the player
         * health gate that _attract_nearby_consumables has. */
        s->con_attr[i] = 1;
    }
}

/* One frame of the wave-end item attraction (item.gd:26-70).  Gold flies to the
 * bag and banks on arrival; consumables fly to the player and are taken on
 * arrival.  Items that have not arrived by the end of the window are dropped by
 * bt_loot_wave_end_drop_remaining. */
BT_HD static int bt_loot_con_ordered_slot(BT_DEVICE BtState* s, int k);

BT_HD static void bt_loot_wave_end_step(BT_DEVICE BtState* s) {
    float bx, by;
    bt_loot_bag_pos(s, &bx, &by);
    /* Both loops walk the source's INSERTION order (the game's Consumables /
     * _active_golds containers are append-ordered, and their nodes'
     * _physics_process runs in tree order), so a frame in which several items
     * arrive resolves them oldest-first. */
    {
        int live_g = 0;
        for (int m = 0; m < (int)s->mat_used; m++) if (s->matalive[m]) live_g++;
        for (int k = 0; ; ) {
            /* bt_loot_gold_ordered_slot's contract is k < #live gold (it returns a
             * stale slot once k runs past the table), so the walk is bounded by the
             * live count; a removed (banked) coin keeps k so the next k-th is the
             * next live coin, an unarrived one advances k. */
            if (k >= live_g) break;
            int m = bt_loot_gold_ordered_slot(s, k);
            if (m < 0 || !s->matalive[m]) break;
            (void)bt_loot_item_step(&s->matx[m], &s->maty[m], s->mat_arc[m], &s->mat_attr[m],
                                    0, bx, by, BT_DT);
            if (s->mat_arc[m][BT_LOOT_ARC_MON] == 0.0f) { k++; continue; }   /* push-back/idle */
            float dx = s->matx[m] - bx, dy = s->maty[m] - by;
            if (!(dx * dx + dy * dy <= BT_LOOT_ITEM_SNAP_EPS)) { k++; continue; }
            s->bonus_gold += (int)s->matval[m];                     /* add_bonus_gold */
            /* gold_bag.gd:_on_GoldBag_area_entered plays the BAG's own pickup
             * sound after `gold.pickup(-1)`: one global draw per gold collected
             * (Utils.get_rand_element over its 5-sound array). */
            (void)bt_rng_bounded(&s->rng, BT_LOOT_GOLD_BAG_SOUNDS);
            if (s->bonus_gold < 0) s->bonus_gold = 0;
            s->matalive[m] = 0;
            s->matcount--;
            s->mat_attr[m] = 0;
            s->mat_by[m] = 0;
            s->mat_home[m] = 0;
            s->mat_mob[m] = -1;
            live_g--;
        }
    }
    {
        int live = 0;
        for (int i = 0; i < (int)s->con_used; i++) if (s->conalive[i]) live++;
        for (int k = 0; ; ) {
            if (k >= live) break;
            int i = bt_loot_con_ordered_slot(s, k);
            if (i < 0 || !s->conalive[i]) break;
            float ax = s->px, ay = s->py + BT_ATTRACT_Y;
            (void)bt_loot_item_step(&s->conx[i], &s->cony[i], s->con_arc[i], &s->con_attr[i],
                                    0, ax, ay, BT_DT);
            s->conspd[i] = s->con_arc[i][BT_LOOT_ARC_SPEED];
            if (s->con_arc[i][BT_LOOT_ARC_MON] == 0.0f) { k++; continue; }
            float pdx = s->px - s->conx[i], pdy = (s->py + BT_PICKUP_Y) - s->cony[i];
            if (!(pdx * pdx + pdy * pdy < BT_PICKUP_R * BT_PICKUP_R)) { k++; continue; }
            bt_loot_consumable_take(s, i);   /* removes slot i: live drops, k stays */
            live--;
        }
    }
}

/* The room is torn down at the EndWaveTimer timeout (base_shop.gd:346 change_scene):
 * every item that did not reach its attractor in the 2 s window is freed and
 * therefore LOST -- no bank, no pickup, no draw. */
BT_HD static void bt_loot_wave_end_drop_remaining(BT_DEVICE BtState* s) {
    for (int m = 0; m < (int)s->mat_used; m++) {
        if (!s->matalive[m]) continue;
        s->matalive[m] = 0;
        s->matcount--;
        s->mat_attr[m] = 0; s->mat_by[m] = 0; s->mat_home[m] = 0; s->mat_mob[m] = -1;
    }
    for (int i = 0; i < (int)s->con_used; i++) {
        if (!s->conalive[i]) continue;
        s->conalive[i] = 0;
        s->con_attr[i] = 0;
        s->conspd[i] = BT_LOOT_ITEM_ATTRACT_SPEED;
    }
}

/* The (k+1)-th live consumable in main.gd's `_consumables` insertion order
 * (main.gd:637 push_back / :797 erase keep that list ordered).  bt_spawn_* and
 * the structure fruit emit record each consumable's spawn sequence in con_seq,
 * so the k-th smallest con_seq is the source list's k-th entry; the (seq, slot)
 * key is total, so a fixture that never wrote a sequence still gets a
 * deterministic order.  Mirrors bt_loot_gold_ordered_slot.  -1 if < k+1 live. */
BT_HD static int bt_loot_con_ordered_slot(BT_DEVICE BtState* s, int k) {
    unsigned int last_seq = 0u;
    int last_slot = -1;
    int found = -1;
    for (int n = 0; n <= k; n++) {
        unsigned int best_seq = 0xFFFFFFFFu;
        int best_slot = -1;
        for (int m = 0; m < (int)s->con_used; m++) {
            if (!s->conalive[m]) continue;
            unsigned int seq = s->con_seq[m];
            if (n > 0 && (seq < last_seq || (seq == last_seq && m <= last_slot))) continue;
            if (seq < best_seq || (seq == best_seq && (best_slot < 0 || m < best_slot))) {
                best_seq = seq; best_slot = m;
            }
        }
        if (best_slot < 0) break;
        last_seq = best_seq; last_slot = best_slot;
        found = best_slot;
    }
    return found;
}

/* ---------------------------------------------------------------------------
 * INTEGRATION (parent-owned; no core/state edits are made by this header)
 *
 * 1. Include this header at the end of brotato_core.h, after the item/spawn
 *    helpers it reuses and alongside the other slices, i.e. next to
 *    brotato_progression.h / brotato_pet_motion.h / brotato_scalar_events.h.
 *    Functions used earlier in the TU (e.g. bt_mark_enemy_dead) need a forward
 *    declaration of the BT_HD prototypes they call.
 *
 * 2. State (parent adds to BtState, brotato_state.h):
 *    a) a per-wave counter
 *        int items_spawned_this_wave;
 *             -- Main._items_spawned_this_wave (main.gd:48,640), which starts at
 *                0 every wave because a fresh Main scene is created per wave.
 *    b) the kind of each deferred box, so the crate can pick its tier:
 *        unsigned char pending_item_kind[<parent-chosen bound>];
 *        int n_pending_items;   + an explicit overflow flag
 *    (BT_MAX_CONS is not enough -- boxes accumulate across a wave as pickups
 *     free consumable slots; wrap with an explicit overflow flag, never a
 *     silent drop.)
 *    c) the item.gd lifecycle vector for each dropped gold and each dropped
 *       consumable (BT_LOOT_ARC_N floats each; layout at the top of the file):
 *        float mat_arc[BT_MAX_MAT][BT_LOOT_ARC_N];
 *        float con_arc[BT_MAX_CONS][BT_LOOT_ARC_N];
 *       plus an attraction speed for consumables (gold already has matspd):
 *        float conspd[BT_MAX_CONS];        -- _current_speed, starts 500
 *       and an attractor kind per dropped item (0 none, 1 player, 2 pet) so
 *       item.gd's `attracted_by != null` / attractor-dead branch can run:
 *        unsigned char mat_attr[BT_MAX_MAT], con_attr[BT_MAX_CONS];
 *       (gold's mat_by is the pet slot and mat_home the instant-attract flag;
 *        mat_attr supersedes the conflated "0 = none or player".)
 *
 * 3. Kill emission -- bt_mark_enemy_dead already calls the selection helpers;
 *    add the drop state.  `kind` is s->ekind[j]; a neutral is never a Boss:
 *        if (K->can_drop_consumables) {
 *            ...existing item_chance / roll / counter code...
 *            if (consumable >= 0) { slot = first free con slot;
 *                conx/conkind/conalive = ...; conspd[slot] = BT_LOOT_ITEM_ATTRACT_SPEED;
 *                con_attr[slot] = 0;
 *                bt_loot_consumable_arc(s, x, y, K->gold_spread, s->con_arc[slot]); }
 *        }
 *        if (bt_loot_gold_drop(s, K, s->eneutral[j], s->wave, s->is_horde_wave)) {
 *            float v = bt_loot_gold_value(s, s->eneutral[j], (float)K->value);
 *            bt_loot_spawn_gold(s, v, x, y, bt_loot_gold_spread(K, v));   <- NOTE +spread arg
 *        }
 *    bt_loot_spawn_gold performs the whole count/boost/spawn loop, the arc
 *    draws and the entity creation; it needs the forward-declared prototype
 *    updated to (s, value, x, y, spread).
 *    Then, every frame, for each live dropped item whose physics is enabled:
 *        int keep = bt_loot_item_step(&x, &y, arc, &attr,
 *                        attr == 2 && pet_is_dead, pet_x, pet_y, BT_DT);
 *        if (!keep) mark that item's physics disabled.
 *    Pickup is gated on arc[BT_LOOT_ARC_MON] (monitorable) and on
 *    (attr == 0 || attr == player).
 *
 * 4. Deferred crate ITEM selection (bt_progress_end_wave's pending_items loop;
 *    replaces the uniform bt_rng_bounded(&s->rng, BT_NITEMS)):
 *        for (i over n_pending_items, FIFO) {
 *            int fixed = (pending_item_kind[i] == BT_CONS_LEGENDARY_BOX)
 *                        ? BT_LOOT_TIER_LEGENDARY : -1;
 *            int it = bt_loot_pick_box_item(s, s->wave, fixed);
 *            s->curse_apply = (unsigned char)bt_loot_item_cursed(s);
 *            s->curse_maxmod = 0.0f;
 *            if (it != (int)BT_ITEM_NONE) bt_apply_item(s, (unsigned short)it);
 *            s->curse_apply = 0;
 *        }
 *    A consumable pickup also plays a random pickup sound
 *    (consumables/consumable.gd:9-11 -> Utils.get_rand_element(pickup_sounds)),
 *    one global draw AFTER reset(); the pickup path must take it to keep the
 *    stream aligned.
 *
 * 5. Shop (brotato_progression.h) -- coordinate with the shop owner:
 *    a) bt_progress_tier returns 1..4 while BtItem.tier is the source Tier
 *       0..3 (the upgrade table BT_UPGRADES deliberately uses resource tier+1,
 *       but the item table does not -- see brotato_items.h's `tier` note and
 *       extract_items.py's get_int(props,"tier",0)).  bt_progress_item_candidate
 *       compares candidate->tier == tier directly, so the shop currently serves
 *       the tier one step too high and has no tier-4 pool.  bt_loot_item_tier
 *       is the source-exact 0..3 roll; the shop can call it (or subtract 1).
 *    b) bt_progress_item_candidate must use this header's bt_loot_item_pool_ok
 *       (can_be_looted && max_nb != 0, under the full-unlock assumption) so the
 *       shop and the crate share one pool predicate.
 * ------------------------------------------------------------------------- */
