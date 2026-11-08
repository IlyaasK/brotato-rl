/* tests/test_brotato_scalar_events.cpp
 *
 * Source-backed regressions for the scalar-event consumers slice
 * (ocean/brotato/brotato_scalar_events.h): self-damage -> the Bull's real
 * explosion, its tick cadence / dodge / iframe / armor rules, consumable
 * healing + healing modifiers, and the gold-pickup / death / dodge event
 * consumers (ChanceStatDamageEffect buckets).
 *
 * Every expectation is transcribed from the recovered sources:
 *   player.tscn:1377           LoseHealthTimer (Timer defaults: wait_time 1.0,
 *                              repeating, autostart off)
 *   player.gd:125-126          timer armed when lose_hp_per_second is owned
 *   player.gd:833-839          _on_LoseHealthTimer_timeout: dodgeable=false,
 *                              armor_applied=false, bypass_invincibility=true
 *   player.gd:411-535          take_damage; dmg_taken > 0 -> explode_on_hit
 *   player.gd:300-313          get_damage_value (armor false -> raw value)
 *   player.gd:447-468          dmg_on_dodge / heal_on_dodge
 *   player.gd:730-756,767-786  healing + heal-over-time + heal() clamp
 *   player.gd:105-106          ItemAttractArea radius
 *   main.gd:499-503            dmg_when_death
 *   main.gd:659-693            item_box_gold
 *   main.gd:740-746            instant_gold_attracting
 *   main.gd:798-854            on_gold_picked_up
 *   main.gd:1500-1571          handle_stat_damages
 *   run_data.gd:829-835        chance_double_gold
 *   weapon_service.gd:493-495  apply_damage_bonus
 *   effects/consumables/consumable_healing_effect.gd:5-14
 *   singletons/item_attract_area.gd:6-7   radius = max(30, 150*(1+pct/100))
 *
 * Build and run:
 *     c++ -O2 -o build/test_brotato_scalar_events tests/test_brotato_scalar_events.cpp -lm
 *     ./build/test_brotato_scalar_events
 *
 * Exit code 0 = all checks passed.
 */

#include "../ocean/brotato/brotato_core.h"

#include <stdio.h>

static int g_failures = 0;
static int g_checks = 0;

static void check(int cond, const char* msg) {
    g_checks++;
    if (!cond) { g_failures++; printf("  FAIL: %s\n", msg); }
}
static void check_near(float got, float want, float tol, const char* msg) {
    g_checks++;
    if (!(bt_fabsf(got - want) <= tol)) {
        g_failures++;
        printf("  FAIL: %s (got %.6f want %.6f tol %g)\n", msg, got, want, tol);
    }
}

/* Advance the deferred player-stat-sync pipeline by its two frame boundaries
 * (run_data.gd:221-222/1400-1406/1418-1427 -> main.gd:339 on_stats_updated ->
 * stats_manager.gd:105-126/20-31 -> player.gd:349-368 update_player_stats): the
 * player's LiveStats (max_stats.health, so the heal room) only catch a stat
 * change after both. */
static void sync_player_stats(BtState* s) {
    bt_player_stat_sync_step(s);
    bt_player_stat_sync_step(s);
}

/* Persistent contact caches belong in static fixtures, not on the stack. */
static BtState A, B;
static float obs[BT_OBS_SIZE];

static int find_kind(const char* name) {
    for (int i = 0; i < BT_NKINDS; i++) {
        const char* n = BT_KIND_NAME[i];
        if (!n) continue;
        int same = 1;
        for (int j = 0; n[j] || name[j]; j++) if (n[j] != name[j]) { same = 0; break; }
        if (same) return i;
    }
    return -1;
}

static void fresh(BtState* s, bt_u64 seed) {
    bt_core_init(s, seed, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
    s->n_live_groups = 0;        /* no wave groups: enemies only when placed */
    bt_core_stat_add(s, 1, -15.0f); /* disable regen in its raw source layer */
    s->regen_timer = 99.0f;
    s->iframe = 0.0f;
    s->dodge = 0.0f;
}

/* place one live enemy at (x,y) */
static void place_enemy(BtState* s, int slot, int kind, float x, float y, float hp) {
    bt_spawn_kind_at(s, slot, kind, x, y, 0);
    s->ehp[slot] = hp; s->emax[slot] = hp;
    s->edmg[slot] = 1.0f; s->espd[slot] = 0.0f; s->eatk[slot] = 99.0f;
}

/* Resolve an effect row by its (key, opcode) so the test never hardcodes a
 * generator index. */
static int find_row_by_key_and_opcode(unsigned short key, int opcode) {
    for (int r = 0; r < BT_NITEM_EFF_ROWS; r++)
        if ((int)BT_ITEM_EFF_ROWS[r].opcode == opcode && BT_ITEM_EFF_ROWS[r].key == key)
            return r;
    return -1;
}

/* Resolve an item index by its host name (without the item_ prefix). */
static int find_item_by_name(const char* name) {
    for (int i = 0; i < BT_NITEMS; i++) {
        const char* n = BT_ITEMS[i].name;
        if (!n) continue;
        int same = 1;
        for (int j = 0; n[j] || name[j]; j++) if (n[j] != name[j]) { same = 0; break; }
        if (same) return i;
    }
    return -1;
}

int main(void) {
    float rew = 0.0f, term = 0.0f;
    const int kind_anemone = find_kind("anemone");   /* armor 0.0 -> explosion hits raw */
    check(kind_anemone >= 0, "anemone kind is present (armor 0 fixture)");

    printf("[1] lose_hp_per_second: 1 Hz cadence, raw damage, no dodge/armor/iframe\n");
    {
        fresh(&A, 101u);
        A.scalar_effect[BT_ISTR_LOSE_HP_PER_SECOND] = 10.0f;
        bt_core_stat_add(&A, 4, 50.0f);   /* dodge 50%: must NOT be rolled    */
        bt_core_stat_add(&A, 5, 50.0f);   /* +50% damage: must NOT scale it   */
        A.iframe = 0.3f;                  /* must NOT block it, and NOT be consumed */
        float hp0 = A.hp;

        A.tick_in_wave = 59;
        check_near(bt_scalar_lose_hp_tick(&A), 0.0f, 0.0f, "no tick at 59 frames");
        check_near(A.hp, hp0, 0.0f, "hp untouched off-cadence");

        A.tick_in_wave = 60;
        float taken = bt_scalar_lose_hp_tick(&A);
        check_near(taken, 10.0f, 1e-6f, "1 s tick deals the raw lose_hp_per_second value");
        check_near(A.hp, hp0 - 10.0f, 1e-6f, "armor/dodge/dmg_pct do not apply to self damage");
        check_near(A.iframe, 0.3f, 1e-6f, "self damage neither respects nor resets iframes");
        check_near(A.dmg_taken, 10.0f, 1e-6f, "dmg_taken is credited");

        A.tick_in_wave = 61;
        check_near(bt_scalar_lose_hp_tick(&A), 0.0f, 0.0f, "only one tick per 60 frames");

        A.tick_in_wave = 120;
        check_near(bt_scalar_lose_hp_tick(&A), 10.0f, 1e-6f, "second tick one second later");

        A.scalar_effect[BT_ISTR_LOSE_HP_PER_SECOND] = 0.0f;
        A.tick_in_wave = 180;
        check_near(bt_scalar_lose_hp_tick(&A), 0.0f, 0.0f, "no effect -> no tick");

        /* the blow can kill: clamp(value, 0, health) then hp = 0 */
        A.scalar_effect[BT_ISTR_LOSE_HP_PER_SECOND] = 1000.0f;
        A.hp = 7.0f;
        A.tick_in_wave = 240;
        check_near(bt_scalar_lose_hp_tick(&A), 7.0f, 1e-6f, "self damage clamps to current hp");
        check_near(A.hp, 0.0f, 1e-6f, "self damage can kill");
    }

    printf("[2] lose_hp_per_second fires the Bull's REAL explosion (not a DPS grant)\n");
    {
        fresh(&A, 102u);
        bt_core_stat_add(&A, 9, -3.0f);   /* crit -> 0: deterministic, no crit roll can land */
        A.scalar_effect[BT_ISTR_LOSE_HP_PER_SECOND] = 1.0f;
        if (kind_anemone >= 0) {
            place_enemy(&A, 0, kind_anemone, A.px, A.py, 1000.0f);
            float eh0 = A.ehp[0];
            A.tick_in_wave = 30;
            check_near(bt_scalar_lose_hp_tick(&A), 0.0f, 0.0f, "off-cadence: no explosion");
            check_near(A.ehp[0], eh0, 0.0f, "enemy untouched off-cadence");

            A.tick_in_wave = 60;
            float taken = bt_scalar_lose_hp_tick(&A);
            check_near(taken, 1.0f, 1e-6f, "self damage is 1");
            check(A.expl.n_spawned >= 1, "the self-damage spawned a pooled explosion node");
            /* the explosion is DEFERRED: ages 1..BT_EXPL_FRAMES resolve the hits */
            for (int t = 0; t < BT_EXPL_FRAMES + 1; t++) (void)bt_item_expl_tick(&A, &rew);
            /* explosion.tscn: 30 + 3*dmg_stat, dmg_stat 0 -> 30, round(30 * armor_mult(0)) */
            check_near(eh0 - A.ehp[0], BT_EXPLODE_DMG, 1e-6f,
                       "the Bull explosion damage is dealt (explode_on_hit path)");

            /* scaling: explosion damage = 30 + 3*stat_*_damage */
            A.ehp[0] = 1000.0f;
            bt_core_stat_add(&A, 6, 10.0f);      /* melee +10 -> dmg_stat 10 */
            float eh1 = A.ehp[0];
            A.tick_in_wave = 120;
            (void)bt_scalar_lose_hp_tick(&A);
            for (int t = 0; t < BT_EXPL_FRAMES + 1; t++) (void)bt_item_expl_tick(&A, &rew);
            check_near(eh1 - A.ehp[0], BT_EXPLODE_DMG + BT_EXPLODE_SCALE_STAT * 10.0f, 1e-6f,
                       "explosion scales 3x the damage stats");
        }
    }

    printf("[3] self-damage cadence over real bt_core_step frames\n");
    {
        fresh(&A, 103u);
        A.scalar_effect[BT_ISTR_LOSE_HP_PER_SECOND] = 1.0f;
        float hp0 = A.hp;
        /* the core hook runs at the top of bt_core_step, before tick_in_wave++ */
        for (int t = 0; t < 61; t++) bt_core_step(&A, 0, obs, &rew, &term);
        check_near(A.hp, hp0 - 1.0f, 1e-4f, "first tick lands 1.0 s into the wave");
        for (int t = 0; t < 60; t++) bt_core_step(&A, 0, obs, &rew, &term);
        check_near(A.hp, hp0 - 2.0f, 1e-4f, "second tick one second later");
        check(A.tick_in_wave == 121, "tick_in_wave advanced");
    }

    printf("[4] consume heal: consumable_heal modifier, max(0,..), heal() clamp\n");
    {
        fresh(&A, 104u);
        check_near(bt_scalar_heal(&A, 5.0f), 0.0f, 1e-6f, "hp at max -> heal() restores nothing");

        A.hp = A.max_hp - 10.0f;
        check_near(bt_scalar_heal(&A, 4.0f), 4.0f, 1e-6f, "heal adds up to the clamp");
        check_near(A.hp, A.max_hp - 6.0f, 1e-6f, "health after heal");

        A.hp = A.max_hp - 20.0f;
        check_near(bt_scalar_consumable_heal(&A, BT_HEAL), BT_HEAL, 1e-6f, "fruit heals 3");

        A.hp = A.max_hp - 20.0f;
        A.scalar_effect[BT_ISTR_CONSUMABLE_HEAL] = 2.0f;      /* weird_food */
        check_near(bt_scalar_consumable_heal(&A, BT_HEAL), 5.0f, 1e-6f, "consumable_heal adds");

        A.hp = A.max_hp - 20.0f;
        A.scalar_effect[BT_ISTR_CONSUMABLE_HEAL] = -5.0f;     /* alien_worm */
        check_near(bt_scalar_consumable_heal(&A, BT_HEAL), 0.0f, 1e-6f, "total clamps at 0");

        bt_core_stat_add(&A, 0, 270.0f);
        sync_player_stats(&A);      /* the +270 max_hp reaches the player's heal room */
        A.hp = 30.0f;
        A.scalar_effect[BT_ISTR_CONSUMABLE_HEAL] = 0.0f;
        check_near(bt_scalar_consumable_heal(&A, BT_LEGENDARY_HEAL), BT_LEGENDARY_HEAL, 1e-6f,
                   "legendary box heals 100");
    }

    printf("[5] consumable_heal_over_time: 1 HP per duration/total\n");
    {
        fresh(&A, 105u);
        A.scalar_effect[BT_ISTR_CONSUMABLE_HEAL] = 0.0f;
        A.scalar_effect[BT_ISTR_CONSUMABLE_HEAL_OVER_TIME] = 4.0f;   /* dlc jerky, REPLACE */
        A.hp = A.max_hp - 10.0f;
        float hp0 = A.hp;
        check_near(bt_scalar_consumable_heal(&A, BT_HEAL), 0.0f, 1e-6f, "no instant heal while HoT runs");
        check(A.hot_pending == 3, "3 one-HP ticks pending (total_healing 3)");

        for (int t = 0; t < 80; t++) (void)bt_scalar_hot_tick(&A);
        check_near(A.hp, hp0 + 1.0f, 1e-4f, "first HoT tick after 1.333 s");
        check(A.hot_pending == 2, "one tick consumed");
        for (int t = 0; t < 160; t++) (void)bt_scalar_hot_tick(&A);
        check_near(A.hp, hp0 + 3.0f, 1e-4f, "whole chain heals total_healing over duration");
        check(A.hot_pending == 0, "chain finished");
        check_near(bt_scalar_hot_tick(&A), 0.0f, 0.0f, "idle once finished");
    }

    printf("[6] gold pickup: chance_double_gold, heal_when_pickup_gold, dmg_when_pickup_gold\n");
    {
        fresh(&A, 106u);
        check(A.matalive[0] == 0, "fresh state has no gold");

        /* deterministic modifiers */
        A.scalar_effect[BT_ISTR_CHANCE_DOUBLE_GOLD] = 100.0f;
        A.scalar_effect[BT_ISTR_HEAL_WHEN_PICKUP_GOLD] = 100.0f;
        A.hp = A.max_hp - 10.0f;
        float hp0 = A.hp;
        int v = bt_scalar_gold_pickup(&A, 3);
        check(v == 6, "chance_double_gold x2");
        check_near(A.hp, hp0 + 1.0f, 1e-6f, "heal_when_pickup_gold heals 1");

        A.scalar_effect[BT_ISTR_CHANCE_DOUBLE_GOLD] = 0.0f;
        A.scalar_effect[BT_ISTR_HEAL_WHEN_PICKUP_GOLD] = 0.0f;
        check(bt_scalar_gold_pickup(&A, 3) == 3, "no modifiers -> plain value");

        /* dmg_when_pickup_gold through the registered bucket */
        bt_scalar_bucket_register(&A, 20, 25, 0);
        check(A.bk_count[20] == 1, "row 20 registered in the bucket");
        check(bt_row_owned(&A, 20) == 1, "row 20 owned once");
        int rv = 0, rv2 = 0;
        check(bt_row_get(&A, 20, &BT_ITEM_EFF_ROWS[20], 0, &rv, &rv2) == 1, "row 20 readback ok");
        check(rv == 25, "row 20 variant value");
        bt_core_stat_add(&A, 10, 100.0f);
        if (kind_anemone >= 0) {
            int hits = 0, bad = 0;
            float expect = bt_floorf(bt_maxf(1.0f, 25.0f * 0.01f * 100.0f));
            for (int t = 0; t < 400; t++) {
                place_enemy(&A, 0, kind_anemone, A.px + 300.0f, A.py, 100000.0f);
                float d = bt_scalar_chance_damages(&A, BT_ISTR_DMG_WHEN_PICKUP_GOLD);
                if (d > 0.0f) { hits++; if (bt_fabsf(d - expect) > 1e-6f) bad++; }
            }
            check(hits > 0, "dmg_when_pickup_gold fires on some pickups");
            check(hits < 400, "dmg_when_pickup_gold respects the 25% chance");
            check(bad == 0, "each hit deals floor(max(1, 25% * luck)) damage");
        }
        /* no live enemy -> the consumer is a no-op */
        bt_enemy_set_alive(&A, 0, 0); A.ecount = 0;
        check_near(bt_scalar_chance_damages(&A, BT_ISTR_DMG_WHEN_PICKUP_GOLD), 0.0f, 0.0f,
                   "no enemy -> no damage");
    }

    printf("[7] dmg_when_death on a credited player kill\n");
    {
        fresh(&A, 107u);
        bt_scalar_bucket_register(&A, 109, 25, 0);
        bt_core_stat_add(&A, 10, 100.0f);
        if (kind_anemone >= 0) {
            int hits = 0, bad = 0;
            for (int t = 0; t < 400; t++) {
                place_enemy(&A, 1, kind_anemone, A.px + 300.0f, A.py, 1000.0f);
                float d = bt_scalar_on_kill(&A);
                if (d > 0.0f) { hits++; if (bt_fabsf(d - 25.0f) > 1e-6f) bad++; }
            }
            check(hits > 0 && hits < 400, "dmg_when_death fires at its 25% chance");
            check(bad == 0, "dmg_when_death deals floor(max(1, 25% * luck))");
        }
    }

    printf("[8] dodge events: dmg_on_dodge hits the attacker, heal_on_dodge heals\n");
    {
        fresh(&A, 108u);
        bt_scalar_bucket_register(&A, 410, 300, 0);
        bt_scalar_bucket_register(&A, 395, 5, 0);
        bt_core_stat_add(&A, 6, 10.0f);

        if (kind_anemone >= 0) {
            bt_core_stat_add(&A, 0, 2970.0f);   /* max_hp 3000: plenty of heal room */
            sync_player_stats(&A);              /* the +2970 reaches the player's heal room */
            float hp_floor = 1000.0f;
            int heals = 0, dmg_bad = 0;
            for (int t = 0; t < 200; t++) {
                place_enemy(&A, 0, kind_anemone, A.px + 300.0f, A.py, 1000.0f);
                A.hp = hp_floor;
                A.iframe = 0.0f;
                float before_hp = A.hp;
                bt_scalar_on_dodge(&A, 0);
                if (bt_fabsf((1000.0f - A.ehp[0]) - 30.0f) > 1e-6f) dmg_bad++;
                if (A.hp > before_hp) {
                    heals++;
                    if (bt_fabsf((A.hp - before_hp) - 5.0f) > 1e-6f) check(0, "heal_on_dodge heals 5");
                }
            }
            check(dmg_bad == 0, "dmg_on_dodge deals max(1, 300% * melee) to the attacker");
            check(heals > 0 && heals < 200, "heal_on_dodge fires at its 50% chance");
            /* no attacker -> dmg_on_dodge is skipped, heal still applies */
            place_enemy(&A, 0, kind_anemone, A.px + 300.0f, A.py, 1000.0f);
            A.hp = hp_floor;
            bt_scalar_on_dodge(&A, -1);
            check_near(A.ehp[0], 1000.0f, 0.0f, "no attacker -> no dmg_on_dodge");
        }
    }

    printf("[9] ItemAttractArea radius from pickup_range\n");
    {
        fresh(&A, 109u);
        check_near(bt_scalar_attract_radius(&A), BT_ATTRACT_R, 1e-6f, "base radius 150");
        A.scalar_effect[BT_ISTR_PICKUP_RANGE] = 50.0f;
        check_near(bt_scalar_attract_radius(&A), 225.0f, 1e-6f, "+50% -> 225");
        A.scalar_effect[BT_ISTR_PICKUP_RANGE] = -50.0f;
        check_near(bt_scalar_attract_radius(&A), 75.0f, 1e-6f, "-50% -> 75");
        A.scalar_effect[BT_ISTR_PICKUP_RANGE] = -100.0f;
        check_near(bt_scalar_attract_radius(&A), 30.0f, 1e-6f, "floored at 30");
    }

    printf("[10] item_box_gold on box pickup\n");
    {
        fresh(&A, 110u);
        check_near(bt_scalar_item_box_gold(&A, 1), 0.0f, 0.0f, "no item_box_gold owned");
        A.scalar_effect[BT_ISTR_ITEM_BOX_GOLD] = 5.0f;
        check_near(bt_scalar_item_box_gold(&A, 1), 5.0f, 1e-6f, "item box grants 5");
        check_near(bt_scalar_item_box_gold(&A, 2), 5.0f, 1e-6f, "legendary box grants 5");
        check_near(bt_scalar_item_box_gold(&A, 0), 0.0f, 0.0f, "fruit grants none");
    }

    printf("[11] instant_gold_attracting spawn roll\n");
    {
        fresh(&A, 111u);
        check(bt_scalar_gold_spawn_attract(&A) == 0, "unowned -> no attract, no draw");
        A.scalar_effect[BT_ISTR_INSTANT_GOLD_ATTRACTING] = 100.0f;
        check(bt_scalar_gold_spawn_attract(&A) == 1, "certainty -> attract");
        A.scalar_effect[BT_ISTR_INSTANT_GOLD_ATTRACTING] = 50.0f;
        int hits = 0;
        for (int t = 0; t < 400; t++) hits += bt_scalar_gold_spawn_attract(&A);
        check(hits > 0 && hits < 400, "50% attracts roughly half");
    }

    printf("[12] explosion_damage feeds the pooled explosion damage\n");
    {
        fresh(&A, 112u);
        check_near(bt_scalar_explosion_damage_mult(&A), 1.0f, 1e-6f, "no modifiers -> x1");

        A.scalar_effect[BT_ISTR_EXPLOSION_DAMAGE] = 60.0f;    /* explosive_shells */
        check_near(bt_scalar_explosion_damage_mult(&A), 1.6f, 1e-6f,
                   "explosion_damage ADDS into the multiplier");

        A.scalar_effect[BT_ISTR_EXPLOSION_DAMAGE] = 0.0f;
        bt_core_stat_add(&A, 5, 100.0f);                      /* +100% damage */
        check_near(bt_scalar_explosion_damage_mult(&A), 2.0f, 1e-6f,
                   "stat_percent_damage shares the same multiplier");
        /* explosion_size's radius effect is covered through the pooled explosion
         * node in test_brotato_item_explosions.cpp (bt_item_expl_node_scale). */
    }

    printf("[13] alien_eyes: 3 s radial emission through the projectile store\n");
    {
        fresh(&A, 113u);
        bt_scalar_bucket_register(&A, 5, 6, 0);

        A.tick_in_wave = 179;
        check_near(bt_scalar_alien_eyes_tick(&A), 0.0f, 0.0f, "no emission before 3 s");
        A.tick_in_wave = 180;
        check_near(bt_scalar_alien_eyes_tick(&A), 6.0f, 0.0f, "6 radial shots at 3 s");
        int live = 0;
        for (int p = 0; p < BT_MAX_PROJ; p++) if (A.pralive[p]) live++;
        check(live == 6, "6 live projectiles in the core store");
        check_near(A.prdmg[0], 1.0f + 0.5f * A.max_hp, 1e-6f,
                   "damage = round((1 + 0.5*stat_max_hp) * (1 + dmg_pct))");
        check_near(A.prvx[0], 500.0f, 1e-3f, "shot 0 points along +x at projectile_speed");
        check_near(A.prvy[0], 0.0f, 1e-3f, "shot 0 has no y velocity");
        check(A.prshape[0] == BT_SHAPE_CIRCLE, "circle projectile");
        check(A.prfriendly[0] == 1, "player-owned: damages enemies");
        check_near(A.prrad[0], 20.0f, 1e-6f, "radius from the alien_eye_projectile scene");
    }

    printf("[14] projectiles_on_death: spawns at the dying enemy\n");
    {
        fresh(&A, 114u);
        bt_scalar_bucket_register(&A, 23, 1, 0);
        bt_core_stat_add(&A, 7, 9.0f);          /* ranged damage 9 */
        check_near(bt_scalar_proj_on_death(&A, 100.0f, 200.0f), 1.0f, 0.0f, "one shot per kill");
        check(A.pralive[0] == 1, "projectile alive");
        check_near(A.prx[0], 100.0f, 1e-6f, "spawn x = the enemy position");
        check_near(A.pry[0], 200.0f, 1e-6f, "spawn y = the enemy position");
        check_near(A.prdmg[0], 1.0f + 9.0f, 1e-6f,
                   "damage = round((1 + 1.0*stat_ranged_damage) * (1 + dmg_pct))");
        float sp = bt_sqrtf(A.prvx[0] * A.prvx[0] + A.prvy[0] * A.prvy[0]);
        check_near(sp, 3000.0f, 1e-2f, "speed from base_ranged_weapon_stats");
        /* no live emitter row -> no shots */
        A.n_bk_active = 0;
        check_near(bt_scalar_proj_on_death(&A, 0.0f, 0.0f), 0.0f, 0.0f, "unowned -> no shot");
    }

    printf("[15] overlapping timed heals retain independent deadlines\n");
    {
        fresh(&A, 115u);
        A.scalar_effect[BT_ISTR_CONSUMABLE_HEAL_OVER_TIME] = 4.0f;
        A.hp = 10.0f;
        bt_scalar_consumable_heal(&A, 3.0f);
        for (int i = 0; i < 40; i++) bt_scalar_hot_tick(&A);
        bt_scalar_consumable_heal(&A, 3.0f);
        for (int i = 0; i < 40; i++) bt_scalar_hot_tick(&A);
        check_near(A.hp, 11.0f, 0.0f, "first pickup heals at frame 80");
        for (int i = 0; i < 40; i++) bt_scalar_hot_tick(&A);
        check_near(A.hp, 12.0f, 0.0f, "second pickup heals at its own frame 120");
        for (int i = 0; i < 160; i++) bt_scalar_hot_tick(&A);
        check_near(A.hp, 16.0f, 0.0f, "both independent three-HP chains finish");
        check(A.hot_pending == 0, "all scheduled healing consumed");
    }

    printf("[16] chained on-kill callbacks preserve depth-first order\n");
    {
        fresh(&A, 116u);
        int death_row = -1, projectile_row = -1;
        for (int row = 0; row < BT_NITEM_EFF_ROWS; row++) {
            const BtItemEff* effect = &BT_ITEM_EFF_ROWS[row];
            if (effect->opcode == BT_ITEMEFF_CHANCE_STAT_DAMAGE &&
                    effect->custom_key == BT_ISTR_DMG_WHEN_DEATH) death_row = row;
            if (effect->opcode == BT_ITEMEFF_PROJECTILE &&
                    effect->key == BT_ISTR_PROJECTILES_ON_DEATH) projectile_row = row;
        }
        check(death_row >= 0 && projectile_row >= 0, "required on-kill items exist");
        if (death_row >= 0 && projectile_row >= 0 && kind_anemone >= 0) {
            for (int copy = 0; copy < 64; copy++)
                bt_scalar_bucket_register(&A, death_row, 25, 0);
            bt_scalar_bucket_register(&A, projectile_row, 1, 0);
            bt_core_stat_add(&A, 10, 100.0f);
            for (int i = 0; i < 3; i++)
                place_enemy(&A, i, kind_anemone, 100.0f * (i + 1), 100.0f, 1.0f);
            A.ecount = 3;
            bt_credit_kill(&A, 0, 1);
            check(A.ecount == 0 && A.kills == 3, "death proc completes its entire kill chain");
            check(A.n_death_events == 0, "all death callbacks finish synchronously");
            int shots = 0, last = -1;
            for (int i = 0; i < BT_MAX_PROJ; i++) if (A.pralive[i]) { shots++; last = i; }
            check(shots == 3, "each killed enemy emits one projectile");
            if (last >= 0) check_near(A.prx[last], 100.0f, 0.0f,
                "root projectile follows every descendant callback");
        }
    }

    printf("[17] ordinary healing cannot resurrect a dead player\n");
    {
        fresh(&A, 117u);
        A.hp = 0.0f;
        check_near(bt_scalar_consumable_heal(&A, 3.0f), 0.0f, 0.0f, "dead player receives no heal");
        check_near(A.hp, 0.0f, 0.0f, "dead health remains zero");
    }

    printf("[18] Bull explosion truncates the scaled base before the dmg multiplier\n");
    {
        /* weapon_service.gd:237-249: apply_scaling_stats_to_damage(...) is
         * `max(1.0, damage + sum) as int` -- the scaled base is TRUNCATED before
         * the init-time percent multiplier, so a fractional scaling stat rounds
         * DOWN (30 + 3*0.5 = 31.5 -> 31), it is not round()ed (32). */
        fresh(&A, 118u);
        bt_core_stat_add(&A, 9, -3.0f);   /* crit -> 0: no crit roll can land */
        A.scalar_effect[BT_ISTR_LOSE_HP_PER_SECOND] = 1.0f;
        if (kind_anemone >= 0) {
            place_enemy(&A, 0, kind_anemone, A.px, A.py, 100000.0f);
            bt_core_stat_add(&A, 6, 0.5f);   /* melee +0.5 -> dmg_stat 0.5 */
            check_near(A.dmg_stat, 0.5f, 1e-6f, "fractional melee lands in dmg_stat");
            float eh0 = A.ehp[0];
            A.tick_in_wave = 60;
            (void)bt_scalar_lose_hp_tick(&A);
            for (int t = 0; t < BT_EXPL_FRAMES + 1; t++) (void)bt_item_expl_tick(&A, &rew);
            check_near(eh0 - A.ehp[0], 31.0f, 1e-6f,
                       "scaled base truncates (31.5 -> 31) before the percent multiplier");
        }
    }

    printf("[19] above-hp item bonus folds into a chance-damage hit (unit.gd:493-503)\n");
    {
        int row_bonus = find_row_by_key_and_opcode(
            BT_ISTR_BONUS_DAMAGE_AGAINST_TARGETS_ABOVE_HP, BT_ITEMEFF_DOUBLE_VALUE);
        check(row_bonus >= 0, "an above-hp DOUBLE_VALUE row is present (small_fish)");
        fresh(&A, 119u);
        bt_scalar_bucket_register(&A, 109, 25, 0);   /* cyberball dmg_when_death 25% */
        bt_core_stat_add(&A, 10, 100.0f);            /* luck 100 -> base 25 */
        if (row_bonus >= 0 && kind_anemone >= 0) {
            A.bk_active[A.n_bk_active] = (unsigned short)row_bonus;
            A.bk_count[row_bonus] = 1;
            A.nvar[row_bonus] = 0;
            A.n_bk_active++;
            int hits = 0, bad = 0;
            for (int t = 0; t < 400; t++) {
                place_enemy(&A, 1, kind_anemone, A.px + 300.0f, A.py, 1000.0f);
                float d = bt_scalar_on_kill(&A);
                if (d > 0.0f) { hits++; if (bt_fabsf(d - 28.0f) > 1e-6f) bad++; }
            }
            check(hits > 0, "the boosted proc still fires");
            check(bad == 0, "above-hp +10%: round(25 * 1.1) = 28, not 25");
            /* the same target above its threshold boost is 0 below it */
            check_near(bt_scalar_target_bonus_pct(&A, 1), 10.0f, 1e-6f,
                       "above the 75% threshold -> +10%");
            A.ehp[1] = 700.0f;   /* 70% of max: below the 75% threshold */
            check_near(bt_scalar_target_bonus_pct(&A, 1), 0.0f, 1e-6f,
                       "below the threshold -> +0%");
        }
    }

    printf("[20] ghost_outfit raises the dodge cap (plain-effect *_cap key)\n");
    {
        fresh(&A, 120u);
        int it = find_item_by_name("ghost_outfit");
        check(it >= 0, "ghost_outfit is in the item pool");
        if (it >= 0) {
            check_near(A.stat_cap[4], BT_DODGE_CAP_DEFAULT, 1e-6f,
                       "dodge cap starts at the init default 60");
            bt_apply_item(&A, (unsigned short)it);
            /* effect.gd REPLACE on key dodge_cap writes the same entry
             * get_capped_stat reads; the core mirrors it into stat_cap[]. */
            check_near(A.stat_cap[4], 70.0f, 1e-6f, "ghost_outfit sets dodge_cap 70");
            bt_core_stat_add(&A, 4, 200.0f);
            check_near(A.dodge, 0.70f, 1e-6f, "dodge clamps at the raised 70% cap");
        }
    }

    printf("[21] torture item: own regen heal, block every other heal\n");
    {
        fresh(&A, 121u);
        bt_core_stat_add(&A, 0, 1000.0f);
        sync_player_stats(&A);      /* the +1000 max_hp reaches the player's LiveStats */
        A.hp = A.max_hp - 100.0f;
        float hp0 = A.hp;
        A.scalar_effect[BT_ISTR_CONSUMABLE_HEAL] = 3.0f;
        check_near(bt_scalar_consumable_heal(&A, 0.0f), 3.0f, 1e-6f,
                   "baseline consumable heal = 3");
        A.hp = hp0;
        /* items/all/torture: key torture, value 5 (EFFECT_TORTURE). */
        A.scalar_effect[BT_ISTR_TORTURE] = 5.0f;
        check_near(bt_scalar_consumable_heal(&A, 0.0f), 0.0f, 0.0f,
                   "torture absorbs consumable healing");
        check_near(A.hp, hp0, 0.0f, "hp unchanged by the blocked heal");
        check_near(bt_scalar_heal(&A, 10.0f), 0.0f, 0.0f, "a plain heal is blocked under torture");
        check_near(bt_scalar_heal_raw(&A, 5.0f), 5.0f, 1e-6f,
                   "the torture-sourced regen heal bypasses the gate");
        check_near(A.hp, hp0 + 5.0f, 1e-6f, "and restores the torture value");

        /* the regen tick itself: 1 s interval, heals the torture value */
        A.regen_timer = 0.0f;
        float hp1 = A.hp;
        bt_core_step(&A, 0, obs, &rew, &term);
        check_near(A.hp, hp1 + 5.0f, 1e-3f, "the regen tick heals the torture value");
        check_near(A.regen_timer, 1.0f, 1e-3f, "at the torture 1 s interval");
    }

    (void)B;
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
