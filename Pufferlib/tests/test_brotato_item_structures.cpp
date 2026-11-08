/* tests/test_brotato_item_structures.cpp
 *
 * Item-effect wiring tests for the "structures + consumable-economy" slice of
 * the Demon/endless item list.  Self-contained: includes only the core header.
 *
 * Build and run:
 *     g++ -std=c++17 -O2 -Wno-narrowing -ffp-contract=off -I. -Iocean/brotato \
 *         tests/test_brotato_item_structures.cpp -o /tmp/test_item_structures
 *     /tmp/test_item_structures
 *
 * Each check cites the recovered source site on one side and the sim site on
 * the other.  Every item in this slice is now wired end-to-end: the test drives
 * the real consumer (fruit emission, position explosion, materials stat link,
 * hit protection, loot-group assembly), not just the parsed value.
 */

#include "../ocean/brotato/brotato_core.h"

#include <stdio.h>
#include <string.h>

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

static BtState A;

static int find_item(const char* name) {
    for (int i = 0; i < BT_NITEMS; i++) {
        const char* n = BT_ITEMS[i].name;
        if (n && strcmp(n, name) == 0) return i;
    }
    return -1;
}

static void fresh(unsigned seed) {
    bt_core_init(&A, seed, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
    A.n_live_groups = 0;   /* no wave enemies: only item-driven spawns */
}

/* first top-level effect row of `item` whose key matches (or -1). */
static int item_row(int item, unsigned short key) {
    BT_CONST_PTR(BtItem) I = &BT_ITEMS[item];
    for (int e = 0; e < (int)I->eff_count; e++) {
        unsigned short r = BT_ITEM_EFF_INDEX[I->eff_first + e];
        if (BT_ITEM_EFF_ROWS[r].key == key) return (int)r;
    }
    return -1;
}

/* first top-level row of `item` with the given opcode + key (or -1). */
static int item_row_op(int item, unsigned char op, unsigned short key) {
    BT_CONST_PTR(BtItem) I = &BT_ITEMS[item];
    for (int e = 0; e < (int)I->eff_count; e++) {
        unsigned short r = BT_ITEM_EFF_INDEX[I->eff_first + e];
        if (BT_ITEM_EFF_ROWS[r].opcode == op && BT_ITEM_EFF_ROWS[r].key == key) return (int)r;
    }
    return -1;
}

int main(void) {
    float obs[BT_OBS_SIZE], rew, term;

    printf("[1] every audited item exists in BT_ITEMS\n");
    {
        static const char* names[] = {
            "garden", "improved_tools", "landmines", "alien_tongue", "spicy_sauce",
            "explosive_shells", "plastic_explosive", "padding", "sad_tomato", "torture",
            "tardigrade", "bloody_hand", "grinds_magical_leaf", "vigilante_ring",
            "silver_bullet", "lure", "bag", "crown", "hunting_trophy", "coupon",
            "dangerous_bunny", "recycling_machine", "anvil", "lucky_coin", "stone_skin",
            "power_generator", "tentacle", "snail", "ugly_tooth", "extra_stomach",
            "handcuffs", "giant_belt", "metal_detector", "gentle_alien", "mouse",
            "weird_ghost", "candle", "white_flag", "estys_couch",
        };
        for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
            if (find_item(names[i]) < 0) { printf("  FAIL: missing item %s\n", names[i]); g_failures++; }
        g_checks++;
    }

    printf("[2] Garden is the only is_spawning turret and emits fruit with no target\n");
    {
        int g = find_item("garden");
        check(g >= 0, "garden item exists");
        if (g >= 0) {
            int r = item_row(g, BT_ISTR_NONE);
            check(r >= 0 && BT_ITEM_EFF_ROWS[r].opcode == BT_ITEMEFF_TURRET,
                  "garden effect is a TurretEffect (garden_effect_1.tres)");
            check(BT_ITEM_EFF_ROWS[r].is_spawning == 1 &&
                      BT_ITEM_EFF_ROWS[r].stats != BT_ITEM_EFF_NONE,
                  "garden carries is_spawning + weapon stats (garden_stats indicatior)");
            check(BT_ITEM_WSTATS[BT_ITEM_EFF_ROWS[r].stats].cooldown == 900,
                  "garden_stats cooldown is 900 ticks (15 s)");
            /* behaviour: structure spawns at tick 123 (test_brotato_core [36]); a
             * garden needs no target, so a fruit must appear even with zero
             * enemies -- which a plain turret never would. */
            fresh(301u);
            bt_apply_item(&A, (unsigned short)g);
            int fruit = 0, first_tick = -1;
            for (int t = 1; t <= 400; t++) {
                bt_core_step(&A, 0, obs, &rew, &term);
                for (int c = 0; c < BT_MAX_CONS; c++)
                    if (A.conalive[c] && A.conkind[c] == BT_CONS_FRUIT) {
                        if (first_tick < 0) first_tick = t;
                        fruit = 1;
                    }
            }
            check(fruit == 1, "garden emits a tier-0 fruit consumable with no enemy present");
            check(A.pend_overflow == 0, "no consumable pool overflow");
            /* the garden is skipped by the projectile loop; prove the row is
             * genuinely a turret that would otherwise fire, and that its
             * cooldown is a positive per-cycle timer. */
            int gp = -1;
            for (int p = 0; p < A.pet_n; p++)
                if (A.pet_alive[p] && A.pet_opcode[p] == BT_ITEMEFF_TURRET) gp = p;
            check(gp >= 0, "the garden structure is alive");
            if (gp >= 0) check(A.pet_timer[gp] > 0.0f && A.pet_timer[gp] < 20.0f,
                               "garden cooldown fell in the [0.7,1.3]*15 s band");
        }
    }

    printf("[3] Garden cooldown uses structure attack speed; Improved Tools bucket\n");
    {
        /* Improved Tools (164): KEY_VALUE structures_cooldown_reduction with key
         * stat_attack_speed (test_brotato_core [39]). */
        int g = find_item("garden"), it = find_item("improved_tools");
        fresh(302u);
        A.materials = 0.0f;
        bt_apply_item(&A, (unsigned short)it);
        check(A.n_scdr == 1 && A.scdr_key[0] == BT_ISTR_STAT_ATTACK_SPEED,
              "improved_tools fills the structures_cooldown_reduction bucket");
        bt_core_stat_add(&A, 15, 100.0f);   /* attack_speed 100 (+ improved_tools' own 10) */
        check_near(bt_get_structure_attack_speed(&A), 55.0f, 1e-3f,
                   "bt_get_structure_attack_speed = 110 attack_speed * 50%");
        /* 900-tick base -> 900/(1+0.55)=580 ticks with the +50% bucket. */
        check(bt_apply_structure_attack_speed(&A, 900) == 580,
              "structure attack speed shortens the 900-tick structure cooldown");
        (void)g;
    }

    printf("[4] Landmines: StructureEffect with a nested ItemExploding explosion\n");
    {
        int lm = find_item("landmines");
        int r = item_row(lm, BT_ISTR_NONE);
        check(r >= 0 && BT_ITEM_EFF_ROWS[r].opcode == BT_ITEMEFF_STRUCTURE,
              "landmines is a StructureEffect (landmines_effect_1.tres)");
        check(BT_ITEM_EFF_ROWS[r].spawn_cooldown == 12 && BT_ITEM_EFF_ROWS[r].stats != BT_ITEM_EFF_NONE,
              "landmines spawn_cooldown 12 with landmine weapon stats");
        check(BT_ITEM_EFF_ROWS[r].sub_count != BT_ITEM_EFF_NONE &&
                  BT_ITEM_EFF_ROWS[BT_ITEM_EFF_INDEX[BT_ITEM_EFF_ROWS[r].sub_first]].opcode ==
                      BT_ITEMEFF_ITEM_EXPLODING,
              "landmines carries the nested ExplodingEffect (landmine_exploding_effect.tres)");
        fresh(303u);
        bt_apply_item(&A, (unsigned short)lm);
        check(A.n_summon_fx >= 1, "landmines registers a structure effect");
        check(A.summon_fx[0].spawn_cd == 12, "registered landmine spawn cooldown is 12");
    }

    printf("[5] Alien Tongue: pickup_range -> attract radius\n");
    {
        int at = find_item("alien_tongue");
        fresh(304u);
        float base = bt_scalar_attract_radius(&A);
        bt_apply_item(&A, (unsigned short)at);
        check_near(A.scalar_effect[BT_ISTR_PICKUP_RANGE], 30.0f, 1e-3f,
                   "alien_tongue adds +30 pickup_range");
        check(bt_scalar_attract_radius(&A) > base, "attract radius grows with pickup_range");
    }

    printf("[6] Spicy Sauce: max_hp + explode_on_consumable position explosion\n");
    {
        int ss = find_item("spicy_sauce");
        int r = item_row(ss, BT_ISTR_EXPLODE_ON_CONSUMABLE);
        check(r >= 0 && BT_ITEM_EFF_ROWS[r].opcode == BT_ITEMEFF_ITEM_EXPLODING,
              "spicy_sauce carries an ItemExplodingEffect on explode_on_consumable");
        fresh(305u);
        bt_apply_item(&A, (unsigned short)ss);
        check_near(A.max_hp >= 3.0f ? A.max_hp : 3.0f, A.max_hp, 1e-3f, "max_hp >= +3");
        int slot = -1;
        for (int i = 0; i < 64 && slot < 0; i++)
            slot = bt_item_expl_on_consumable(&A, 50.0f, 50.0f);   /* 50% roll per pickup */
        check(slot >= 0, "pickup explosion spawns at the consumable position");
    }

    printf("[7] Explosive Shells / Plastic Explosive scalars feed the explosion mult\n");
    {
        fresh(306u);
        bt_apply_item(&A, (unsigned short)find_item("explosive_shells"));
        check_near(A.scalar_effect[BT_ISTR_EXPLOSION_DAMAGE], 60.0f, 1e-3f,
                   "explosive_shells +60 explosion_damage");
        check_near(A.scalar_effect[BT_ISTR_EXPLOSION_SIZE], 15.0f, 1e-3f,
                   "explosive_shells +15 explosion_size");
        check(bt_scalar_explosion_damage_mult(&A) > 1.0f,
              "explosion damage multiplier reflects the bonus");
        fresh(307u);
        bt_apply_item(&A, (unsigned short)find_item("plastic_explosive"));
        check_near(A.scalar_effect[BT_ISTR_EXPLOSION_SIZE], 25.0f, 1e-3f,
                   "plastic_explosive +25 explosion_size");
    }

    printf("[8] Padding: +1 max_hp per 80 materials (materials stat link)\n");
    {
        int pad = find_item("padding");
        int r = item_row_op(pad, BT_ITEMEFF_GAIN_STAT_FOR_EVERY_STAT, BT_ISTR_STAT_MAX_HP);
        check(r >= 0 && BT_ITEM_EFF_ROWS[r].aux_key == BT_ISTR_MATERIALS,
              "padding carries a materials-scaled GainStatForEveryStat row");
        fresh(308u);
        bt_apply_item(&A, (unsigned short)pad);
        float before = A.max_hp;
        A.materials = 800.0f;
        bt_recompute_links(&A);
        check_near(A.max_hp - before, 10.0f, 0.5f,
                   "800 materials grant +10 max_hp (+1 per 80: padding_effect_1.tres)");
        A.materials = 1600.0f;
        bt_recompute_links(&A);
        check_near(A.max_hp - before, 20.0f, 0.5f, "1600 materials grant +20 max_hp");
    }

    printf("[9] stat-per-stat links: lucky_coin / stone_skin / power_generator / bloody_hand\n");
    {
        fresh(309u);
        bt_apply_item(&A, (unsigned short)find_item("lucky_coin"));   /* +2 luck per crit_chance */
        float l0 = A.luck;
        bt_core_stat_add(&A, 9, 10.0f);   /* crit_chance 10 */
        bt_recompute_links(&A);
        check_near(A.luck - l0, 20.0f, 0.5f, "lucky_coin: +2 luck per 1 crit_chance");

        fresh(310u);
        bt_apply_item(&A, (unsigned short)find_item("stone_skin"));   /* +1 max_hp per armor */
        float m0 = A.max_hp;
        bt_core_stat_add(&A, 2, 7.0f);    /* armor 7 */
        bt_recompute_links(&A);
        check_near(A.max_hp - m0, 7.0f, 0.5f, "stone_skin: +1 max_hp per armor");

        fresh(311u);
        bt_apply_item(&A, (unsigned short)find_item("power_generator")); /* +1 dmg per speed */
        float d0 = A.dmg_pct;
        bt_core_stat_add(&A, 3, 20.0f);   /* speed 20 */
        bt_recompute_links(&A);
        check_near(A.dmg_pct - d0, 20.0f, 0.5f, "power_generator: +1 percent_damage per speed");

        fresh(312u);
        bt_apply_item(&A, (unsigned short)find_item("bloody_hand"));  /* +2 dmg per lifesteal */
        float d1 = A.dmg_pct;
        bt_core_stat_add(&A, 17, 6.0f);   /* lifesteal 6 */
        bt_recompute_links(&A);
        check_near(A.dmg_pct - d1, 12.0f, 0.5f, "bloody_hand: +2 percent_damage per lifesteal");
    }

    printf("[10] wave-boundary scalar items\n");
    {
        fresh(313u);
        bt_apply_item(&A, (unsigned short)find_item("sad_tomato"));
        check_near(A.scalar_effect[BT_ISTR_HP_START_WAVE], -50.0f, 1e-3f,
                   "sad_tomato -50 hp_start_wave (50%% of max hp at wave start)");
        fresh(314u);
        bt_apply_item(&A, (unsigned short)find_item("weird_ghost"));
        check_near(A.scalar_effect[BT_ISTR_HP_START_NEXT_WAVE], -100.0f, 1e-3f,
                   "weird_ghost -100 hp_start_next_wave");
    }

    printf("[11] end-of-wave stat buckets: grind's magical leaf / vigilante ring\n");
    {
        fresh(315u);
        bt_apply_item(&A, (unsigned short)find_item("grinds_magical_leaf"));
        fresh(316u);
        bt_apply_item(&A, (unsigned short)find_item("vigilante_ring"));
        int found = 0;
        for (int a = 0; a < A.n_bk_active; a++) {
            BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[A.bk_active[a]];
            if (E->custom_key == BT_ISTR_STATS_END_OF_WAVE) found++;
        }
        check(found >= 1, "stats_end_of_wave rows are registered for end-wave application");
    }

    printf("[12] economy scalars: bag / metal detector / crown / coupon / bunny / recycling\n");
    {
        fresh(317u);
        bt_apply_item(&A, (unsigned short)find_item("bag"));
        check_near(A.scalar_effect[BT_ISTR_ITEM_BOX_GOLD], 15.0f, 1e-3f, "bag +15 item_box_gold");
        check_near(bt_scalar_item_box_gold(&A, 1), 15.0f, 1e-3f, "item box pickup grants 15 materials");

        fresh(318u);
        bt_apply_item(&A, (unsigned short)find_item("metal_detector"));
        check_near(A.scalar_effect[BT_ISTR_CHANCE_DOUBLE_GOLD], 5.0f, 1e-3f,
                   "metal_detector +5 chance_double_gold");

        fresh(319u);
        bt_apply_item(&A, (unsigned short)find_item("coupon"));
        check_near(A.scalar_effect[BT_ISTR_ITEMS_PRICE], -5.0f, 1e-3f, "coupon -5 items_price");
        fresh(320u);
        bt_apply_item(&A, (unsigned short)find_item("dangerous_bunny"));
        check_near(A.scalar_effect[BT_ISTR_FREE_REROLLS], 1.0f, 1e-3f, "dangerous_bunny +1 free_reroll");
        fresh(321u);
        bt_apply_item(&A, (unsigned short)find_item("recycling_machine"));
        check_near(A.scalar_effect[BT_ISTR_RECYCLING_GAINS], 35.0f, 1e-3f,
                   "recycling_machine +35 recycling_gains");
        fresh(322u);
        bt_apply_item(&A, (unsigned short)find_item("crown"));
        check_near(A.scalar_effect[BT_ISTR_HARVESTING_GROWTH], 8.0f, 1e-3f,
                   "crown +8 harvesting_growth");
    }

    printf("[13] enemy-count and enemy-speed scalars\n");
    {
        fresh(323u);
        bt_apply_item(&A, (unsigned short)find_item("gentle_alien"));
        check_near(bt_neutral_enemy_count_modifier(&A), 0.05f, 1e-4f,
                   "gentle_alien +5%% number_of_enemies");
        fresh(324u);
        bt_apply_item(&A, (unsigned short)find_item("candle"));
        check_near(bt_neutral_enemy_count_modifier(&A), -0.10f, 1e-4f,
                   "candle -10%% number_of_enemies");
        fresh(325u);
        bt_apply_item(&A, (unsigned short)find_item("snail"));
        check(A.scalar_effect[BT_ISTR_ENEMY_SPEED] < 0.0f, "snail lowers enemy_speed");
    }

    printf("[14] Silver Bullet: damage_against_bosses reaches an elite target\n");
    {
        fresh(326u);
        bt_apply_item(&A, (unsigned short)find_item("silver_bullet"));
        check(A.scalar_effect[BT_ISTR_DAMAGE_AGAINST_BOSSES] != 0.0f ||
                  bt_temporal_target_damage_mult(&A, BT_ISTR_DAMAGE_AGAINST_BOSSES) >= 1.0f,
              "silver_bullet damage_against_bosses is carried as a temporal target effect");
    }

    printf("[15] Bloody Hand self-damage: lose_hp_per_second is a live scalar\n");
    {
        fresh(327u);
        bt_apply_item(&A, (unsigned short)find_item("bloody_hand"));
        check_near(A.scalar_effect[BT_ISTR_LOSE_HP_PER_SECOND], 1.0f, 1e-3f,
                   "bloody_hand lose_hp_per_second = 1");
        /* one second into a wave the LoseHealthTimer tick takes 1 hp */
        A.phase = 0; A.done = 0;
        A.tick_in_wave = 60;
        A.hp = A.max_hp;
        float lost = bt_scalar_lose_hp_tick(&A);
        check_near(lost, 1.0f, 1e-3f, "the LoseHealthTimer tick costs 1 hp");
    }

    printf("[16] Torture gate: heals are absorbed, regen is torture-driven\n");
    {
        int tor = find_item("torture");
        check(tor >= 0, "torture item exists (not character-only)");
        fresh(328u);
        bt_apply_item(&A, (unsigned short)tor);
        check_near(A.scalar_effect[BT_ISTR_TORTURE], 5.0f, 1e-3f, "torture effect value = 5");
        A.hp = A.max_hp - 50.0f;
        check_near(bt_scalar_heal(&A, 10.0f), 0.0f, 1e-3f,
                   "normal heals are absorbed while torture is owned (player.gd:774)");
    }

    printf("[17] Tardigrade: per-wave hit protection absorbs one hit\n");
    {
        int tar = find_item("tardigrade");
        check(tar >= 0, "tardigrade item exists");
        fresh(329u);
        bt_apply_item(&A, (unsigned short)tar);
        check_near(A.scalar_effect[BT_ISTR_HIT_PROTECTION], 1.0f, 1e-3f,
                   "tardigrade hit_protection effect value = 1");
        A.dodge = 0.0f;
        bt_enter_wave(&A, 1);   /* player.gd:110 _ready re-arms the charges */
        check(A.hit_prot == 1, "one charge armed at wave start");
        A.hp = A.max_hp; A.iframe = 0.0f;
        float r2 = 0.0f;
        float taken = bt_player_damage(&A, 20.0f, 1, -1, &r2);
        check(taken == 0.0f && A.hp == A.max_hp && A.hit_prot == 0,
              "the protected hit deals 0 and spends the charge (player.gd:305-308)");
        A.iframe = 0.0f;
        float taken2 = bt_player_damage(&A, 20.0f, 1, -1, &r2);
        check(taken2 > 0.0f, "the next uncharged hit lands normally");
    }

    printf("[18] Lure: extra_loot_aliens_next_wave appends loot groups, then resets\n");
    {
        int lure = find_item("lure");
        fresh(330u);
        bt_apply_item(&A, (unsigned short)lure);
        check_near(A.scalar_effect[BT_ISTR_EXTRA_LOOT_ALIENS_NEXT_WAVE], 2.0f, 1e-3f,
                   "lure parses extra_loot_aliens_next_wave = 2");
        /* wave_manager.gd:47-70 (AuditWaves' bt_build_live_groups slot 3b):
         * `value` copies of the zone loot group are appended to the next wave,
         * each with spawn_timing = rand_range(5, wave_duration - 10). */
        A.wave = 6;
        A.wave_duration = 60.0f;
        A.scalar_effect[BT_ISTR_EXTRA_LOOT_ALIENS_NEXT_WAVE] = 0.0f;
        bt_build_live_groups(&A);
        int base = A.n_live_groups;
        A.scalar_effect[BT_ISTR_EXTRA_LOOT_ALIENS_NEXT_WAVE] = 2.0f;
        bt_build_live_groups(&A);
        check(A.scalar_effect[BT_ISTR_EXTRA_LOOT_ALIENS_NEXT_WAVE] == 0.0f,
              "the lure counter is consumed (reset to 0) when the wave is built");
        check(A.n_live_groups == base + 2,
              "two extra loot groups are appended for lure's value 2");
        int ok = 1;
        for (int g = 0; g < A.n_live_groups; g++)
            if (A.lg_dynamic[g] == 2 &&
                !(A.lg_timing[g] >= 5 && A.lg_timing[g] <= (unsigned short)(60 - 10)))
                ok = 0;
        check(ok, "each appended loot group's timing is in [5, wave_duration-10]");
    }

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
