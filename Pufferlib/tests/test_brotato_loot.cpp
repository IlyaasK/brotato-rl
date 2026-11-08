/* Unit tests for brotato_loot.h: source loot drop / consumable selection /
 * deferred crate ITEM selection.
 *
 * The header is included after brotato_core.h, exactly like the slice headers
 * inside the core, so it can reuse bt_spawn_material / bt_item_shop_ok /
 * bt_item_has_wanted_tag / bt_kind_class.
 *
 * Every check is either a hand-computed source value or a property that must
 * hold for any RNG draw (tier/pool membership, entity counts, draw routing).
 */
#include "../ocean/brotato/brotato_core.h"
#include "../ocean/brotato/brotato_loot.h"
#include <cstdio>
#include <cstring>
#include <cmath>

static int failures;
static void check(bool ok, const char *msg) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", msg); failures++; }
}
static bool near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) < eps; }

static BtState st;
static void fresh(bt_u64 seed) {
    std::memset(&st, 0, sizeof(st));
    st.mat_used = BT_MAX_MAT; st.con_used = BT_MAX_CONS;   /* raw slot fixtures */
    bt_rng_seed(&st.rng, seed);
    bt_rng_seed(&st.utils_rng, seed ^ 0x9e3779b97f4a7c15ull);
}

/* Number of PCG32 outputs consumed from st.rng by one call (bounded replay). */
static int draws(void (*fn)(void)) {
    BtRng before = st.rng;
    fn();
    if (st.rng.state == before.state) return 0;
    BtRng probe = before;
    for (int n = 1; n <= 64; n++) {
        bt_rng_next(&probe);
        if (probe.state == st.rng.state) return n;
    }
    return -1;
}

static int g_kind;
static void call_tier_common(void) { g_kind = bt_loot_consumable_for_tier(&st, BT_LOOT_TIER_COMMON); }
static void call_tier_item_box(void) { g_kind = bt_loot_consumable_for_tier(&st, BT_LOOT_TIER_UNCOMMON); }
static void call_tier_rare(void) { g_kind = bt_loot_consumable_for_tier(&st, BT_LOOT_TIER_RARE); }

static BtUnitKind g_uk;
static int g_it;
static void call_pick(void) { g_it = bt_loot_pick_box_item(&st, 1, -1); }
static void call_pick_legendary(void) { g_it = bt_loot_pick_box_item(&st, 1, BT_LOOT_TIER_LEGENDARY); }

static float g_arc[BT_LOOT_ARC_N];
static float g_angle;
static void call_gold_count_int(void) { (void)bt_loot_gold_count(&st, 2.0f); }
static void call_gold_count_frac(void) { (void)bt_loot_gold_count(&st, 2.5f); }
static void call_gold_arc(void) { bt_loot_gold_arc(&st, 0.0f, 0.0f, 50, g_arc, &g_angle); }
static void call_cons_arc(void) { bt_loot_consumable_arc(&st, 0.0f, 0.0f, 50, g_arc); }
static void call_boost(void) { (void)bt_loot_gold_boost_choose(&st, BT_LOOT_MAX_GOLDS); }

static int item_by_name(const char *name) {
    for (int i = 0; i < BT_NITEMS; i++)
        if (!std::strcmp(BT_ITEMS[i].name, name)) return i;
    return -1;
}

int main(void) {
    /* These fixtures hand-build a BtState and never run bt_core_init, which is
     * the only initializer for the two RNG-model gates; without this they would
     * assert the neutralized (audio 0 / particles 0) model instead of the
     * source default they were written for. */
    st.audio_rng = 1;
    st.particles_rng = 1;
    /* ---- consumable tier -> kind ---------------------------------------- */
    fresh(1);
    check(bt_loot_consumable_for_tier(&st, BT_LOOT_TIER_COMMON) == BT_CONS_FRUIT,
          "tier 0 is the fruit");
    check(bt_loot_consumable_for_tier(&st, BT_LOOT_TIER_UNCOMMON) == BT_CONS_ITEM_BOX,
          "tier 1 is the item box");
    check(bt_loot_consumable_for_tier(&st, BT_LOOT_TIER_LEGENDARY) == BT_CONS_LEGENDARY_BOX,
          "tier 3 is the legendary item box");
    check(bt_loot_consumable_for_tier(&st, BT_LOOT_TIER_RARE) == BT_CONS_NONE,
          "tier 2 has no unlocked consumable");
    /* pick_random over a one-element array is still one global draw; an empty
     * tier pool draws nothing. */
    fresh(7); check(draws(call_tier_common) == 1, "non-empty tier pool consumes one global draw");
    fresh(7); check(draws(call_tier_item_box) == 1, "item-box tier consumes one global draw");
    fresh(7); check(draws(call_tier_rare) == 0, "empty tier pool consumes no draw");

    /* ---- item_chance: luck + per-wave crate counter --------------------- */
    fresh(2);
    std::memset(&g_uk, 0, sizeof(g_uk));
    g_uk.item_drop_chance = 0.05f;
    check(near(bt_loot_item_chance(&st, &g_uk, 0, 1), 0.05f), "item_chance base");
    check(near(bt_loot_item_chance(&st, &g_uk, 4, 1), 0.05f / 5.0f), "items spawned this wave divides item_chance");
    st.luck = 100.0f;
    check(near(bt_loot_item_chance(&st, &g_uk, 0, 1), 0.10f), "luck scales item_chance");
    st.luck = 0.0f;
    g_uk.always_drop_consumables = 1; g_uk.item_drop_chance = 1.0f;
    check(near(bt_loot_item_chance(&st, &g_uk, 4, 20), 1.0f),
          "always-drop with item_drop_chance>=1 forces item_chance within the run");
    check(near(bt_loot_item_chance(&st, &g_uk, 4, 21), 0.2f),
          "the always-drop override does not apply past nb_of_waves");
    g_uk.always_drop_consumables = 0; g_uk.item_drop_chance = 0.05f;

    /* ---- get_consumable_to_drop ---------------------------------------- */
    fresh(3);
    std::memset(&g_uk, 0, sizeof(g_uk));
    g_uk.base_drop_chance = 1.0f; g_uk.min_tier = 0; g_uk.max_tier = 0;
    check(bt_loot_roll_consumable(&st, &g_uk, 0.0f, 0, 1) == BT_CONS_FRUIT,
          "guaranteed drop with no item chance yields a fruit");
    check(bt_loot_roll_consumable(&st, &g_uk, 1.0f, 0, 1) == BT_CONS_ITEM_BOX,
          "item chance upgrades a non-boss drop to an item box");
    check(bt_loot_roll_consumable(&st, &g_uk, 1.0f, 1, 20) == BT_CONS_LEGENDARY_BOX,
          "a boss on wave <= nb_of_waves yields the legendary box");
    g_uk.base_drop_chance = 0.0f;
    check(bt_loot_roll_consumable(&st, &g_uk, 1.0f, 0, 1) == BT_CONS_NONE,
          "zero base_drop_chance and no always-drop yields nothing");
    g_uk.always_drop_consumables = 1;
    check(bt_loot_roll_consumable(&st, &g_uk, 0.0f, 0, 1) == BT_CONS_FRUIT,
          "always_drop_consumables bypasses the base-drop roll");
    check(bt_loot_counts_toward_wave(BT_CONS_ITEM_BOX) &&
          bt_loot_counts_toward_wave(BT_CONS_LEGENDARY_BOX) &&
          !bt_loot_counts_toward_wave(BT_CONS_FRUIT),
          "only boxes count toward the wave's item-spawn counter");
    /* routing: base-drop + item-chance draws are global, the tier roll is Utils. */
    {
        BtRng before_g = st.rng, before_u = st.utils_rng;
        (void)bt_loot_roll_consumable(&st, &g_uk, 1.0f, 0, 1);
        check(st.rng.state != before_g.state && st.utils_rng.state != before_u.state,
              "consumable roll draws from both streams");
    }

    /* ---- get_gold_value ------------------------------------------------- */
    fresh(4);
    check(near(bt_loot_gold_value(&st, 0, 10.0f), 10.0f), "gold value without modifiers");
    st.scalar_effect[BT_ISTR_GOLD_DROPS] = 50.0f;
    check(near(bt_loot_gold_value(&st, 0, 10.0f), 15.0f), "gold_drops scales enemy value");
    check(near(bt_loot_gold_value(&st, 1, 10.0f), 15.0f), "gold_drops scales neutral value");
    st.scalar_effect[BT_ISTR_GOLD_DROPS] = -100.0f;
    check(near(bt_loot_gold_value(&st, 0, 10.0f), BT_LOOT_MIN_GOLD_CHANCE * 10.0f),
          "MIN_GOLD_CHANCE floors the value");
    st.scalar_effect[BT_ISTR_GOLD_DROPS] = 0.0f;

    /* ---- gold drop gate ------------------------------------------------- */
    fresh(5);
    std::memset(&g_uk, 0, sizeof(g_uk));
    check(bt_loot_gold_drop(&st, &g_uk, 1, 20, 0) == 1, "a neutral always drops its value");
    check(bt_loot_gold_drop(&st, &g_uk, 0, 1, 0) == 1, "waves 1-4 always drop for enemies");
    {
        int normal = 0, horde = 0;
        for (int sd = 0; sd < 400; sd++) {
            fresh((bt_u64)(100 + sd));
            normal += bt_loot_gold_drop(&st, &g_uk, 0, 20, 0);
            fresh((bt_u64)(100 + sd));
            horde += bt_loot_gold_drop(&st, &g_uk, 0, 20, 1);
        }
        check(normal > 0 && horde > 0 && horde < normal, "horde waves scale the enemy drop chance down");
    }

    /* ---- spawn draws: fractional count + item.gd arc --------------------- */
    fresh(6);
    check(draws(call_gold_count_int) == 0, "an integer gold value consumes no residual draw");
    fresh(6);
    check(draws(call_gold_count_frac) == 1, "a fractional gold value consumes one residual draw");
    {
        int mn = 99, mx = 0;
        for (int sd = 0; sd < 200; sd++) {
            fresh((bt_u64)(700 + sd));
            int n = bt_loot_gold_count(&st, 2.5f);
            if (n < mn) mn = n;
            if (n > mx) mx = n;
        }
        check(mn == 2 && mx == 3, "value 2.5 spawns two or three golds on the residual roll");
    }
    fresh(6);
    check(bt_loot_gold_count(&st, 3.0f) == 3, "value 3.0 spawns three golds");

    /* gold spawn draws: sprite pick (1) + dist (3) + area pos (6) + rotation
     * (3); the instant-attract roll belongs to the caller's entity creation. */
    fresh(20);
    check(draws(call_gold_arc) == 13, "one gold spawn consumes the source draws");
    /* consumables: dist (3) + area pos (6); no sprite pick, rotation is 0. */
    fresh(20);
    check(draws(call_cons_arc) == 9, "one consumable spawn consumes the source draws");

    /* the destination is drawn inside the zone, scatter up to the drop
     * distance; the arc state starts in the reset state. */
    {
        fresh(21);
        float angle;
        bt_loot_gold_arc(&st, 1024.0f, 768.0f, 50, g_arc, &angle);
        float dest_x = g_arc[BT_LOOT_ARC_DEST_X], dest_y = g_arc[BT_LOOT_ARC_DEST_Y];
        check(dest_x >= 0.0f && dest_x <= BT_ARENA_W, "drop destination x stays in the zone");
        check(dest_y >= 0.0f && dest_y <= BT_ARENA_H, "drop destination y stays in the zone");
        check(angle >= 0.0f && angle <= BT_LOOT_TWO_PI, "drop rotation is inside [0, 2*PI)");
        check(g_arc[BT_LOOT_ARC_IDLE] == BT_LOOT_ITEM_IDLE_TIME, "idle time starts at the source 10");
        check(g_arc[BT_LOOT_ARC_SPEED] == BT_LOOT_ITEM_ATTRACT_SPEED, "attract speed starts at 500");
        check(g_arc[BT_LOOT_ARC_PUSH] == 1.0f && g_arc[BT_LOOT_ARC_MON] == 0.0f,
              "a fresh drop pushes back and is not monitorable");
    }
    /* rand_pos_in_area matches ZoneService: clamp then expand by area/2. */
    fresh(22);
    for (int sd = 0; sd < 100; sd++) {
        fresh((bt_u64)(800 + sd));
        float ox, oy;
        bt_loot_rand_pos_in_area(&st, -500.0f, 99999.0f, 200.0f, &ox, &oy);
        check(ox >= -100.0f && ox <= 300.0f, "clamped x expands by area/2 around the clamp");
        check(oy >= BT_ARENA_H - 200.0f && oy <= BT_ARENA_H, "clamped y stays at the zone bound");
    }

    /* ---- item.gd lifecycle: push-back, idle, attract, freeze ------------ */
    {
        float x = 1000.0f, y = 1000.0f;
        unsigned char attr = 0;
        bt_loot_item_place(g_arc, 1200.0f, 1000.0f);
        check(g_arc[BT_LOOT_ARC_PUSH] == 1.0f && g_arc[BT_LOOT_ARC_MON] == 0.0f &&
              g_arc[BT_LOOT_ARC_IDLE] == BT_LOOT_ITEM_IDLE_TIME &&
              g_arc[BT_LOOT_ARC_SPEED] == BT_LOOT_ITEM_ATTRACT_SPEED,
              "place() writes the source reset()+drop() state");
        int push_frames = 0;
        while (g_arc[BT_LOOT_ARC_PUSH] != 0.0f) {
            check(bt_loot_item_step(&x, &y, g_arc, &attr, 0, 0.0f, 0.0f, BT_DT) == 1,
                  "push-back keeps physics running");
            push_frames++;
            check(push_frames < 1000, "push-back terminates");
        }
        check(x > 1180.0f, "push-back converges to within the source 20-unit stop radius");
        check(g_arc[BT_LOOT_ARC_MON] == 1.0f, "pickup is enabled once the push-back ends");
        check(g_arc[BT_LOOT_ARC_IDLE] == BT_LOOT_ITEM_IDLE_TIME - 60.0f * BT_DT,
              "the push-back-ending frame is the first idle frame (60*delta)");
        /* idle: stationary for idle_time / (60*delta) frames -- the source
         * decrements idle_time by 60*delta, i.e. one unit per 60 Hz frame. */
        float idle_x = x, idle_y = y;
        int idle_frames = 1;   /* the frame that ended the push-back */
        while (bt_loot_item_step(&x, &y, g_arc, &attr, 0, 0.0f, 0.0f, BT_DT) != 0) {
            idle_frames++;
            check(x == idle_x && y == idle_y, "the item is stationary while it idles");
        }
        check(idle_frames >= (int)BT_LOOT_ITEM_IDLE_TIME && idle_frames <= (int)BT_LOOT_ITEM_IDLE_TIME + 1,
              "the idle phase lasts the source idle_time frames, no arbitrary timer");
        check(g_arc[BT_LOOT_ARC_IDLE] <= 0.0f, "idle_time_after_pushed_back reaches zero");
        /* with no attractor, physics stops (set_physics_process(false)). */
        check(bt_loot_item_step(&x, &y, g_arc, &attr, 0, 0.0f, 0.0f, BT_DT) == 0,
              "an un-attracted settled item disables physics");
        /* attracted: move_toward with the accelerating speed. */
        attr = 1;
        float px = x + 1000.0f, py = y;
        float before = x;
        check(bt_loot_item_step(&x, &y, g_arc, &attr, 0, px, py, BT_DT) == 1, "an attracted item keeps moving");
        check(x > before, "the item moves toward the attractor");
        check(g_arc[BT_LOOT_ARC_SPEED] == BT_LOOT_ITEM_ATTRACT_SPEED + BT_LOOT_ITEM_ATTRACT_ACCEL,
              "attraction speed accelerates by the source 20/frame");
        check(near(x - before, BT_LOOT_ITEM_ATTRACT_SPEED * BT_DT, 1e-3f),
              "the first attract step is delta * INITIAL_ATTRACT_SPEED");
        /* a dead attractor is cleared and the speed resets (no move). */
        float held = x;
        bt_loot_item_step(&x, &y, g_arc, &attr, 1, px, py, BT_DT);
        check(attr == 0 && x == held, "a dead attractor is cleared without moving the item");
        check(g_arc[BT_LOOT_ARC_SPEED] == BT_LOOT_ITEM_ATTRACT_SPEED,
              "the attract speed resets when the attractor dies");
        /* an item already at the attractor does not overshoot. */
        attr = 1;
        float hx = x, hy = y;
        bt_loot_item_step(&x, &y, g_arc, &attr, 0, hx, hy, BT_DT);
        check(x == hx && y == hy, "move_toward never overshoots");
    }

    /* ---- bt_loot_spawn_gold: full per-kill loop over the material table --- */
    {
        fresh(23);
        st.matcount = 0; std::memset(st.matalive, 0, sizeof(st.matalive));
        std::memset(st.mat_attr, 0, sizeof(st.mat_attr));
        bt_loot_spawn_gold(&st, 2.0f, 100.0f, 100.0f, 50);
        check(st.matcount == 2, "value 2.0 creates two gold entities");
        {
            int n = 0;
            for (int m = 0; m < BT_MAX_MAT; m++) {
                if (!st.matalive[m]) continue;
                n++;
                check(st.matval[m] == BT_LOOT_GOLD_VALUE, "each spawned gold carries INITIAL_VALUE");
                check(st.mat_arc[m][BT_LOOT_ARC_PUSH] == 1.0f && st.mat_arc[m][BT_LOOT_ARC_MON] == 0.0f,
                      "each spawned gold starts its push-back arc");
                check(st.mat_attr[m] == 0, "a spawned gold has no attractor");
                check(st.matx[m] == 100.0f && st.maty[m] == 100.0f, "the gold starts at the kill position");
            }
            check(n == 2, "two live golds");
        }
        /* MAX_GOLDS: no new entity, an existing gold is boosted. */
        fresh(23);
        st.matcount = 0; std::memset(st.matalive, 0, sizeof(st.matalive));
        for (int m = 0; m < BT_LOOT_MAX_GOLDS; m++) { st.matalive[m] = 1; st.matval[m] = 1; st.matcount++; }
        bt_loot_spawn_gold(&st, 4.0f, 0.0f, 0.0f, 50);
        check(st.matcount == BT_LOOT_MAX_GOLDS, "at MAX_GOLDS no further entities are created");
        {
            long total = 0;
            for (int m = 0; m < BT_MAX_MAT; m++) if (st.matalive[m]) total += st.matval[m];
            check(total == BT_LOOT_MAX_GOLDS + 4, "overflow gold is added to live golds, never dropped");
        }
    }

    /* ---- MAX_GOLDS boost pick: one draw, index in range ---------------- */
    {
        fresh(8);
        check(draws(call_boost) == 1, "the overflow boost pick consumes one global draw");
        int mn = 99, mx = -1;
        for (int sd = 0; sd < 200; sd++) {
            fresh((bt_u64)(900 + sd));
            int k = bt_loot_gold_boost_choose(&st, BT_LOOT_MAX_GOLDS);
            if (k < mn) mn = k;
            if (k > mx) mx = k;
        }
        check(mn == 0 && mx == BT_LOOT_MAX_GOLDS - 1, "the boost pick indexes [0, n_active)");
    }

    /* ---- item tier roll ------------------------------------------------- */
    fresh(9);
    for (int sd = 0; sd < 200; sd++) {
        fresh((bt_u64)(1000 + sd));
        int t = bt_loot_item_tier(&st, 1);
        check(t == BT_LOOT_TIER_COMMON, "wave 1 always rolls COMMON (tiers>=1 have zero chance)");
    }
    fresh(10);
    for (int sd = 0; sd < 500; sd++) {
        fresh((bt_u64)(2000 + sd));
        int t = bt_loot_item_tier(&st, 40);
        check(t >= 0 && t <= 3, "tier stays in Tier.COMMON..LEGENDARY");
    }

    /* ---- pool predicate: can_be_looted && max_nb != 0 (full-unlock model) */
    {
        /* The environment models a fully unlocked profile, so an
         * unlocked_by_default == 0 item is still in the pool. */
        int gnome = item_by_name("gnome");
        check(gnome >= 0 && BT_ITEMS[gnome].unlocked_by_default == 0 && BT_ITEMS[gnome].can_be_looted == 1 &&
              bt_loot_item_pool_ok(&st, (unsigned short)gnome) == 1,
              "a locked item is still lootable under the full-unlock model");
        int no_loot = -1, max0 = -1;
        for (int i = 0; i < BT_NITEMS && (no_loot < 0 || max0 < 0); i++) {
            if (no_loot < 0 && BT_ITEMS[i].can_be_looted == 0) no_loot = i;
            if (max0 < 0 && BT_ITEMS[i].max_nb == 0) max0 = i;
        }
        check(no_loot >= 0 && bt_loot_item_pool_ok(&st, (unsigned short)no_loot) == 0, "can_be_looted=0 is excluded");
        check(max0 >= 0 && bt_loot_item_pool_ok(&st, (unsigned short)max0) == 0, "max_nb=0 is excluded");
        int acid = item_by_name("acid");
        check(acid >= 0 && bt_loot_item_pool_ok(&st, (unsigned short)acid) == 1, "a plain lootable item is in the pool");
    }

    /* ---- wanted-tag level and banned filter ----------------------------- */
    {
        int tier0 = BT_LOOT_TIER_COMMON;
        int plain = -1, wanted = -1;
        for (int i = 0; i < BT_NITEMS && (plain < 0 || wanted < 0); i++) {
            if ((int)BT_ITEMS[i].tier != tier0 || !bt_loot_item_pool_ok(&st, (unsigned short)i)) continue;
            if (bt_character_wants_item(BT_CHARACTER_BULL, (unsigned short)i)) { if (wanted < 0) wanted = i; }
            else if (plain < 0) plain = i;
        }
        fresh(11);
        if (plain >= 0)
            check(bt_loot_pool_member(&st, plain, tier0, 1, 1, 2) == 0 &&
                  bt_loot_pool_member(&st, plain, tier0, 1, 0, 2) == 1,
                  "wanted-tag roll removes items without a wanted tag from the primary pool");
        if (wanted >= 0)
            check(bt_loot_pool_member(&st, wanted, tier0, 1, 1, 2) == 1,
                  "wanted-tag items survive the wanted filter");
        int banned_group = -1;
        for (int i = 0; i < BT_NITEMS; i++)
            if ((int)BT_ITEMS[i].tier == tier0 && bt_loot_item_pool_ok(&st, (unsigned short)i) &&
                (BT_ITEMS[i].group_mask & BT_BULL_BANNED_GROUP_MASK)) { banned_group = i; break; }
        if (banned_group >= 0)
            check(bt_loot_pool_member(&st, banned_group, tier0, 1, 0, 2) == 0 &&
                  bt_loot_pool_member(&st, banned_group, tier0, 1, 0, 1) == 1,
                  "banned-group items are only removed from the primary pool (backup keeps them)");
        int banned_item = (int)BT_BULL_BANNED_ITEMS[0];
        if ((int)BT_ITEMS[banned_item].tier == tier0 && bt_loot_item_pool_ok(&st, (unsigned short)banned_item))
            check(bt_loot_pool_member(&st, banned_item, tier0, 1, 0, 2) == 0,
                  "the character's banned items are removed from the primary pool");
    }

    /* ---- deferred crate pick ------------------------------------------- */
    {
        /* wave 1 -> COMMON tier, only items that pass the pool predicate. */
        for (int sd = 0; sd < 300; sd++) {
            fresh((bt_u64)(3000 + sd));
            int it = bt_loot_pick_box_item(&st, 1, -1);
            check(it >= 0 && it < BT_NITEMS, "crate pick is a valid item index");
            check((int)BT_ITEMS[it].tier == BT_LOOT_TIER_COMMON, "crate pick matches the rolled tier");
            check(bt_loot_item_pool_ok(&st, (unsigned short)it) != 0, "crate pick is inside the lootable pool");
            check(bt_item_shop_ok(BT_CHARACTER_BULL, (unsigned short)it) != 0, "crate pick is not a banned item/group");
        }

        /* legendary box forces tier LEGENDARY. */
        for (int sd = 0; sd < 200; sd++) {
            fresh((bt_u64)(4000 + sd));
            int it = bt_loot_pick_box_item(&st, 1, BT_LOOT_TIER_LEGENDARY);
            check(it >= 0 && (int)BT_ITEMS[it].tier == BT_LOOT_TIER_LEGENDARY,
                  "legendary box always yields a LEGENDARY item");
        }

        /* a maxed finite item is never offered, even at wave 1. */
        int limited = -1, max_nb = 0;
        for (int i = 0; i < BT_NITEMS; i++)
            if ((int)BT_ITEMS[i].tier == BT_LOOT_TIER_COMMON && BT_ITEMS[i].max_nb > 0 && bt_loot_item_pool_ok(&st, (unsigned short)i)) {
                limited = i; max_nb = BT_ITEMS[i].max_nb; break;
            }
        if (limited >= 0) {
            int seen_other = 0;
            for (int sd = 0; sd < 300; sd++) {
                fresh((bt_u64)(5000 + sd));
                st.item_owned[limited] = (unsigned short)max_nb;
                int it = bt_loot_pick_box_item(&st, 1, -1);
                check(it != limited, "an item at its max_nb is dropped from the crate pool");
                if (it != limited) seen_other = 1;
            }
            check(seen_other, "other items are still offered when one is maxed");
        }

        /* always-dropped item: crank all but one eligible tier-0 item to their
         * limit and confirm only the survivor is ever picked. */
        int survivor = -1;
        for (int i = 0; i < BT_NITEMS; i++)
            if ((int)BT_ITEMS[i].tier == BT_LOOT_TIER_COMMON && bt_loot_item_pool_ok(&st, (unsigned short)i) &&
                bt_item_shop_ok(BT_CHARACTER_BULL, (unsigned short)i)) { survivor = i; break; }
        if (survivor >= 0) {
            fresh(12);
            for (int i = 0; i < BT_NITEMS; i++) {
                if (!bt_loot_item_pool_ok(&st, (unsigned short)i)) continue;
                if (i == survivor) continue;
                if (BT_ITEMS[i].max_nb > 0) st.item_owned[i] = (unsigned short)BT_ITEMS[i].max_nb;
            }
            int bad = 0;
            for (int sd = 0; sd < 200; sd++) {
                fresh((bt_u64)(6000 + sd));
                st.item_owned[survivor] = 0;
                for (int i = 0; i < BT_NITEMS; i++) {
                    if (!bt_loot_item_pool_ok(&st, (unsigned short)i) || i == survivor) continue;
                    if (BT_ITEMS[i].max_nb > 0) st.item_owned[i] = (unsigned short)BT_ITEMS[i].max_nb;
                }
                int it = bt_loot_pick_box_item(&st, 1, -1);
                if ((int)BT_ITEMS[it].tier != BT_LOOT_TIER_COMMON) bad++;
                if (!bt_loot_item_pool_ok(&st, (unsigned short)it)) bad++;
            }
            check(bad == 0, "the crate pool always stays inside the eligible tier pool");
        }

        /* draw routing: the crate pick uses only the global stream, and a
         * tier-0 pick (axolotl is tier 3, so never rerolled here) consumes
         * rand_wanted + tier(3) + wanted + pick = 6 global draws. */
        {
            fresh(13);
            BtRng before_u = st.utils_rng;
            check(draws(call_pick) == 6, "a crate pick consumes the source selection draws");
            check(st.utils_rng.state == before_u.state, "the crate pick does not touch the Utils stream");
        }
        fresh(17);
        check(draws(call_pick_legendary) == 6,
              "the legendary box still evaluates (and consumes) the tier roll");
        check(g_it >= 0 && (int)BT_ITEMS[g_it].tier == BT_LOOT_TIER_LEGENDARY,
              "fixed_tier overrides the rolled tier");
    }

    /* ---- DLC curse roll ------------------------------------------------- */
    fresh(14);
    st.curse = 0.0f;
    /* dlcs/dlc_1/dlc_data.tres (the SHIPPED instance of dlc_1_data.gd) sets
     * base_item_curse_chance = 0.0, overriding the script default 0.03, so
     * max(base*100, stat_curse) is exactly 0.0 at stat_curse 0.  Utils.
     * get_chance_success (:397-403) returns false for a 0.0 chance WITHOUT
     * consuming a draw -- which is why a curse-free run rolls no item curse. */
    check(near(bt_loot_curse_chance(&st), 0.0f),
          "shipped base_item_curse_chance 0.0 gives chance 0.0 at stat_curse 0");
    check(!bt_loot_item_cursed(&st), "a 0-chance curse roll fails");
    {
        BtRng before0 = st.rng;
        (void)bt_loot_item_cursed(&st);
        check(st.rng.state == before0.state, "a 0-chance curse roll consumes no draw");
    }
    st.curse = 100.0f;
    check(near(bt_loot_curse_chance(&st), BT_LOOT_MAX_CURSE_ITEM_CHANCE * (1.0f - 1.0f / (1.0f + 100.0f / 50.0f))),
          "curse chance grows with the curse stat");
    {
        BtRng before = st.rng;
        (void)bt_loot_item_cursed(&st);
        check(st.rng.state != before.state, "the curse roll consumes one global draw");
    }

    /* ---- endless factor ------------------------------------------------- */
    check(near(bt_loot_endless_factor(20), 0.0f), "no endless factor before the endless waves");
    check(near(bt_loot_endless_factor(21), 0.02f), "wave 21 endless factor");
    check(near(bt_loot_endless_factor(36), 1.36f * 2.2f), "wave 36 endless factor uses integer triangular division");

    /* ---- main.gd:963-1010 clean_up_room: gold bag + consumable take -------
     * The window now SIMULATES item.gd's attraction frame by frame: a gold flies
     * to the GoldBag (main.gd:1002-1004) and banks on arrival (main.gd:854-859,
     * no draws/XP); a consumable flies to the player and is taken on arrival.
     * Anything still in flight at the EndWaveTimer timeout is dropped with the
     * room. */
    {
        /* A coin placed beside the player reaches the bag and is banked; the bag
         * sits up-left of the player, so the flight is a few frames. */
        fresh(29);
        st.materials = 40.0f; st.xp = 0.0f; st.bonus_gold = 0;
        st.matcount = 0; std::memset(st.matalive, 0, sizeof(st.matalive));
        st.matalive[0] = 1; st.matval[0] = 3; st.mat_seq[0] = 0; st.mat_seq_next = 1;
        st.matx[0] = st.px; st.maty[0] = st.py;
        bt_loot_item_place(st.mat_arc[0], st.px, st.py);   /* drop state: push-back done */
        st.mat_arc[0][BT_LOOT_ARC_PUSH] = 0.0f;
        st.mat_arc[0][BT_LOOT_ARC_MON] = 1.0f;
        BtRng bank_before = st.rng;
        float m0 = st.materials;
        bt_loot_wave_end_begin(&st);
        check(st.matalive[0] == 1 && st.mat_attr[0] == BT_LOOT_ATTR_BAG,
              "the window aims the live gold at the bag");
        for (int t = 0; t < 120 && st.matalive[0]; t++) bt_loot_wave_end_step(&st);
        check(st.matalive[0] == 0, "the coin reaches the bag inside the window");
        check(st.bonus_gold == 3, "the bag banks the coin's value as bonus_gold");
        check(near(st.materials, m0) && near(st.xp, 0.0f),
              "a bag pickup grants no materials and no XP");
        BtRng one = bank_before;
        (void)bt_rng_next(&one);
        check(st.rng.state == one.state,
              "banking consumes exactly one global draw (GoldBag's own pickup-sound pick)");
        /* the banked value is merged into the next wave's spawned gold */
        bt_spawn_material(&st, 10.0f, 10.0f, BT_LOOT_GOLD_VALUE);
        check(st.bonus_gold == 2 && st.matval[0] == 2,
              "the next spawned gold absorbs min(value, bonus_gold)");

        /* A coin the flight cannot reach is LOST when the room is torn down. */
        fresh(29);
        st.bonus_gold = 0;
        st.matcount = 0; std::memset(st.matalive, 0, sizeof(st.matalive));
        st.matalive[0] = 1; st.matval[0] = 7; st.mat_seq[0] = 0; st.mat_seq_next = 1;
        st.matx[0] = BT_ARENA_W - 4.0f; st.maty[0] = BT_ARENA_H - 4.0f;
        bt_loot_item_place(st.mat_arc[0], st.matx[0], st.maty[0]);
        st.mat_arc[0][BT_LOOT_ARC_PUSH] = 0.0f;
        st.mat_arc[0][BT_LOOT_ARC_MON] = 1.0f;
        /* push the bag to the far corner while the player stays put: the coin can
         * travel at most bt_loot_flight_capacity(120) in the window */
        bt_loot_set_bag_view(&st, 1920.0f, 1080.0f, 1.0f, -3000.0f, -2500.0f);
        bt_loot_wave_end_begin(&st);
        for (int t = 0; t < 120; t++) bt_loot_wave_end_step(&st);
        {
            /* proof of the enforced outcome: the coin's distance to the bag
             * exceeds everything the attraction ramp can cover in the window */
            float bx, by;
            bt_loot_bag_pos(&st, &bx, &by);
            float dx = st.matx[0] - bx, dy = st.maty[0] - by;
            float dist = bt_sqrtf(dx * dx + dy * dy);
            check(dist > bt_loot_flight_capacity(BT_END_WAVE_TICKS),
                  "the unreachable case exceeds the 120-tick flight capacity");
        }
        bt_loot_wave_end_drop_remaining(&st);
        check(st.matalive[0] == 0 && st.bonus_gold == 0,
              "a coin that never reaches the bag is lost, not banked");
        bt_loot_set_bag_view(&st, 1920.0f, 1080.0f, 1.0f, BT_LOOT_BAG_HUD_X, BT_LOOT_BAG_HUD_Y);

        /* A live consumable is attracted to the player and picked up on arrival:
         * one sound draw, the heal, and (item boxes) a deferred crate. */
        fresh(30);
        st.materials = 0.0f; st.max_hp = 30.0f; st.p_max_hp = 30.0f; st.hp = 10.0f;
        std::memset(st.conalive, 0, sizeof(st.conalive));
        st.conalive[2] = 1; st.conkind[2] = BT_CONS_FRUIT;
        st.conx[2] = st.px; st.cony[2] = st.py + BT_PICKUP_Y;
        bt_loot_item_place(st.con_arc[2], st.conx[2], st.cony[2]);
        st.con_arc[2][BT_LOOT_ARC_PUSH] = 0.0f;
        st.con_arc[2][BT_LOOT_ARC_MON] = 1.0f;
        BtRng take_before = st.rng;
        bt_loot_wave_end_begin(&st);
        check(st.con_attr[2] == 1, "the window attracts the consumable to the player");
        bt_loot_wave_end_step(&st);
        check(st.conalive[2] == 0, "the fruit is picked up on its arrival frame");
        check(near(st.hp, 13.0f), "the fruit heals its 3 HP");
        check(st.rng.state != take_before.state, "the pickup takes its sound draw");
        check(st.pending_items == 0, "a fruit queues no crate");

        fresh(31);
        st.materials = 0.0f; st.max_hp = 30.0f; st.p_max_hp = 30.0f; st.hp = 30.0f;
        std::memset(st.conalive, 0, sizeof(st.conalive));
        st.conalive[1] = 1; st.conkind[1] = BT_CONS_ITEM_BOX;
        st.conx[1] = st.px; st.cony[1] = st.py + BT_PICKUP_Y;
        bt_loot_item_place(st.con_arc[1], st.conx[1], st.cony[1]);
        st.con_arc[1][BT_LOOT_ARC_PUSH] = 0.0f;
        st.con_arc[1][BT_LOOT_ARC_MON] = 1.0f;
        bt_loot_wave_end_begin(&st);
        bt_loot_wave_end_step(&st);
        check(st.conalive[1] == 0 && st.pending_items == 1 &&
              st.pending_item_kind[0] == BT_CONS_ITEM_BOX,
              "an item box queues its crate for the upgrade UI");
        check(near(st.materials, 0.0f), "Bull owns no item_box_gold");

        /* main.gd:637 push_back / :797 erase: both items arrive on the same frame,
         * so the tree order (insertion order, con_seq) decides the pickup order,
         * not slot order.  Slot 1 is the OLDEST (seq 0), slot 0 the newest. */
        fresh(32);
        st.materials = 0.0f; st.max_hp = 60.0f; st.p_max_hp = 60.0f; st.hp = 60.0f;
        std::memset(st.conalive, 0, sizeof(st.conalive));
        std::memset(st.con_seq, 0, sizeof(st.con_seq));
        st.conalive[1] = 1; st.conkind[1] = BT_CONS_LEGENDARY_BOX; st.con_seq[1] = 0;
        st.conalive[0] = 1; st.conkind[0] = BT_CONS_ITEM_BOX;      st.con_seq[0] = 1;
        st.con_seq_next = 2;
        for (int s2 = 0; s2 < 2; s2++) {
            int i = (s2 == 0) ? 1 : 0;
            st.conx[i] = st.px; st.cony[i] = st.py + BT_PICKUP_Y;
            bt_loot_item_place(st.con_arc[i], st.conx[i], st.cony[i]);
            st.con_arc[i][BT_LOOT_ARC_PUSH] = 0.0f;
            st.con_arc[i][BT_LOOT_ARC_MON] = 1.0f;
        }
        bt_loot_wave_end_begin(&st);
        bt_loot_wave_end_step(&st);
        check(st.pending_items == 2 &&
              st.pending_item_kind[0] == BT_CONS_LEGENDARY_BOX &&
              st.pending_item_kind[1] == BT_CONS_ITEM_BOX,
              "same-frame arrivals resolve in insertion order, not slot order");
    }

    /* ---- determinism ---------------------------------------------------- */
    {
        fresh(15);
        int a = bt_loot_pick_box_item(&st, 5, -1);
        fresh(15);
        int b = bt_loot_pick_box_item(&st, 5, -1);
        check(a == b, "identical state and seed reproduce the same crate pick");
        fresh(16);
        float da = bt_loot_roll_consumable(&st, &g_uk, 0.5f, 0, 3) +
                   bt_loot_item_chance(&st, &g_uk, 0, 3);
        fresh(16);
        float db = bt_loot_roll_consumable(&st, &g_uk, 0.5f, 0, 3) +
                   bt_loot_item_chance(&st, &g_uk, 0, 3);
        check(near(da, db), "identical state and seed reproduce the same consumable roll");
    }

    if (failures == 0) std::printf("brotato_loot: OK\n");
    else std::printf("brotato_loot: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
