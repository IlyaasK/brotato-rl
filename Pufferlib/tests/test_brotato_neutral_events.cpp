/* tests/test_brotato_neutral_events.cpp
 *
 * Source-backed regressions for the neutral (tree) event slice
 * (ocean/brotato/brotato_neutral_events.h):
 *   - the per-tree hit tally and the one_shot_trees death rule
 *     (neutral.gd:4-6,14-17,20-35);
 *   - the tree_turrets structures queued on tree death (main.gd:583-593);
 *   - the entity_spawner.gd:170-182 spawn-group unit-count modifiers
 *     (number_of_enemies / trees);
 *   - the neutral domain separation (a neutral is never an enemy kind and never
 *     borrows enemy behaviour/boost).
 *
 * Every expectation is transcribed from the recovered sources:
 *   entities/units/neutral/neutral.gd:4,6,14-17,20-35,38-40
 *   entities/units/neutral/neutral.tscn:16   number_of_hits_before_dying = 8
 *   entities/units/neutral/tree.tscn         instances neutral.tscn, no override
 *   dlcs/dlc_1/neutrals/tree/tree.tscn       instances neutral.tscn, no override
 *   singletons/debug_service.gd:9,33         one_shot_enemies false, nb_enemies_mult 1.0
 *   singletons/run_data.gd:427-435           get_player_effect(_bool), sum_all
 *   main.gd:583-593                          tree_turrets turret per owned effect
 *   main.tscn:124                            turret_effect = turret_effect_1.tres
 *   items/all/turret/turret_effect_1.tres    the queued turret structure
 *   items/all/turret/turret_stats.tres       cd 32 ticks, damage 10, eng x0.8
 *   items/all/lumberjack_shirt/lumberjack_shirt_effect_1.tres  one_shot_trees 1
 *   items/all/pocket_factory/pocket_factory_effect_{0,1}.tres  engineering 2, tree_turrets 1
 *   items/all/{mouse,gentle_alien,candle,white_flag}/*_effect_*   number_of_enemies
 *   items/all/tree/tree_effect_1.tres        trees +1
 *   global/entity_spawner.gd:170-182         group unit-count modifiers
 *
 * STATE REQUIREMENT: this test needs the parent-owned BtState addition
 * `unsigned char ehit[BT_MAX];` (zeroed in bt_spawn_kind_at and bt_core_reset).
 * Until that lands it does not compile -- that is the integration hook, the same
 * way test_brotato_scalar_events.cpp requires its state fields.
 *
 * Build and run:
 *     c++ -O2 -o build/test_brotato_neutral_events tests/test_brotato_neutral_events.cpp -lm
 *     ./build/test_brotato_neutral_events
 *
 * Exit code 0 = all checks passed.
 */

#include "../ocean/brotato/brotato_core.h"
/* included by brotato_core.h in the integrated build; included explicitly here
 * so this test compiles while the core hook line is still pending. */
#include "../ocean/brotato/brotato_neutral_events.h"

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

/* BtState is ~1 MiB; keep the big states static. */
static BtState A, B;

static int str_has(const char* hay, const char* needle) {
    if (!hay) return 0;
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j] && hay[i + j] == needle[j]) j++;
        if (!needle[j]) return 1;
    }
    return 0;
}

/* BT_NEUTRALS index whose name contains `needle` (the two zone trees). */
static int neutral_kind(const char* needle) {
    for (int i = 0; i < BT_NNEUTRALS; i++)
        if (str_has(BT_NEUTRAL_NAME[i], needle)) return i;
    return -1;
}

/* enemy BT_KINDS index by exact name. */
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

static int neutral_slot(BtState* s, int nk) {
    for (int i = 0; i < BT_MAX; i++)
        if (s->ealive[i] && s->eneutral[i] && (int)s->ekind[i] == nk) return i;
    return -1;
}

/* Fresh single-player env with no wave groups: entities exist only when placed. */
static void fresh(BtState* s, bt_u64 seed) {
    bt_core_init(s, seed, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
    s->n_live_groups = 0;
    bt_core_stat_add(s, 1, -15.0f);   /* disable regen in its raw source layer */
    s->regen_timer = 99.0f;
    s->iframe = 0.0f;
    s->dodge = 0.0f;
}

/* Place one live neutral directly (bypassing the birth countdown). */
static void place_neutral(BtState* s, int slot, int kind, float x, float y, float hp) {
    bt_enemy_set_alive(s, slot, 1);
    s->eneutral[slot] = 1;
    s->ekind[slot] = (unsigned char)kind;
    s->ex[slot] = x; s->ey[slot] = y;
    s->ehp[slot] = hp; s->emax[slot] = hp;
    s->edmg[slot] = 1.0f; s->espd[slot] = 0.0f; s->eatk[slot] = 99.0f;
    s->ebeh[slot] = 0; s->eelite[slot] = 0;
    s->eburn[slot] = 0.0f; s->eburndmg[slot] = 0.0f;
    s->ehit[slot] = 0;
    s->ecount = 1;
}

/* One damage instance against a neutral: subtract `taken`, then run the rule.
 * Returns the helper's decision (1 = the caller must credit the kill). */
static int hit_neutral(BtState* s, int slot, float taken, int from_player_index, int has_from) {
    s->ehp[slot] -= taken;
    return bt_neutral_on_damage(s, slot, from_player_index, has_from);
}

int main(void) {
    const int nk_tree = neutral_kind("tree");
    check(nk_tree >= 0, "a tree neutral kind is present");

    printf("[1] hit tally (neutral.gd:20-35): the 8th player hit kills\n");
    {
        fresh(&A, 201u);
        place_neutral(&A, 0, nk_tree, 500.0f, 500.0f, 1000.0f);
        int died_at = -1;
        for (int i = 1; i <= 8; i++) {
            int die = hit_neutral(&A, 0, 1.0f, 0, 1);
            if (die && died_at < 0) died_at = i;
        }
        check(died_at == 8, "neutral dies on the 8th player hit, not earlier");
        check(A.ehit[0] == BT_NEUTRAL_HITS_BEFORE_DYING,
              "counter reaches number_of_hits_before_dying (8)");
        check(A.ealive[0], "the helper never kills: the slot is still alive for the caller");

        /* 7 hits do not kill */
        fresh(&A, 202u);
        place_neutral(&A, 0, nk_tree, 500.0f, 500.0f, 1000.0f);
        for (int i = 0; i < 7; i++) check(hit_neutral(&A, 0, 1.0f, 0, 1) == 0, "hit 1..7 does not kill");
        check(A.ehit[0] == 7, "counter is 7 after seven hits");
    }

    printf("[2] non-player / sourceless hits (neutral.gd:25-28,30)\n");
    {
        /* hitbox.from exists but is not player-indexed -> early return: the hit is
         * tallied, but the count can NEVER kill. */
        fresh(&A, 203u);
        place_neutral(&A, 0, nk_tree, 500.0f, 500.0f, 1000.0f);
        for (int i = 0; i < 20; i++)
            check(hit_neutral(&A, 0, 1.0f, -1, 1) == 0, "a non-player `from` never triggers the count death");
        check(A.ehit[0] == 20, "the suppressed hits are still tallied");

        /* no hitbox at all (burning tick, take_damage_args has no hitbox): the
         * count death DOES apply, but one_shot_trees cannot (neutral.gd:31). */
        fresh(&A, 204u);
        A.scalar_effect[BT_ISTR_ONE_SHOT_TREES] = 1.0f;   /* would one-shot if a hitbox existed */
        place_neutral(&A, 0, nk_tree, 500.0f, 500.0f, 1000.0f);
        for (int i = 0; i < 7; i++)
            check(hit_neutral(&A, 0, 1.0f, -1, 0) == 0, "sourceless hits do not one-shot");
        check(hit_neutral(&A, 0, 1.0f, -1, 0) == 1, "sourceless hits kill by count");
    }

    printf("[3] one_shot_trees (neutral.gd:31)\n");
    {
        fresh(&A, 205u);
        check(A.scalar_effect[BT_ISTR_ONE_SHOT_TREES] == 0.0f, "one_shot_trees starts at 0");
        check(bt_neutral_one_shot_trees(&A, 0) == 0, "not owned -> false");
        place_neutral(&A, 0, nk_tree, 500.0f, 500.0f, 1000.0f);
        check(hit_neutral(&A, 0, 1.0f, 0, 1) == 0, "without the item the first hit does not kill");

        /* real item wiring: items/all/lumberjack_shirt/*_effect_1.tres (SUM) */
        fresh(&A, 206u);
        bt_apply_item(&A, BT_ITEM_IDX_LUMBERJACK_SHIRT);
        check(A.scalar_effect[BT_ISTR_ONE_SHOT_TREES] > 0.0f,
              "lumberjack_shirt stores one_shot_trees in scalar_effect");
        check(bt_neutral_one_shot_trees(&A, 0) == 1, "owned -> get_player_effect_bool true");
        place_neutral(&A, 0, nk_tree, 500.0f, 500.0f, 1000.0f);
        check(hit_neutral(&A, 0, 1.0f, 0, 1) == 1, "the FIRST player hit one-shots the tree");
        check(A.ehit[0] == 1, "one-shot death happens on hit 1, not 8");

        /* a second owned copy (SUM storage) stays owned, never turns off */
        bt_apply_item(&A, BT_ITEM_IDX_LUMBERJACK_SHIRT);
        check(bt_neutral_one_shot_trees(&A, 0) == 1, "repeat copies keep the bool true");
    }

    printf("[4] HP death short-circuit (neutral.gd:25-26)\n");
    {
        fresh(&A, 207u);
        place_neutral(&A, 0, nk_tree, 500.0f, 500.0f, 3.0f);
        int die = hit_neutral(&A, 0, 5.0f, 0, 1);   /* parent .take_damage killed it */
        check(A.ehp[0] <= 0.0f, "damage dropped HP to <= 0");
        check(die == 0, "helper defers to the caller's ehp<=0 credit path");
        check(A.ehit[0] == 1, "the hit was still tallied (neutral.gd:23 before the dead check)");
    }

    printf("[5] per-slot tally reset on spawn (neutral.gd:17 respawn)\n");
    {
        fresh(&A, 208u);
        A.ehit[0] = 7;
        check(bt_spawn_neutral(&A, nk_tree, 400.0f, 400.0f) == 1, "spawn a neutral");
        check(A.ehit[0] == 0, "bt_spawn_kind_at zeroes the recycled slot's hit tally");
        check(A.ealive[0] && A.eneutral[0], "spawned slot is a live neutral");

        /* and a fresh episode resets every previously allocated slot */
        bt_spawn_kind_at(&A, 3, nk_tree, 500.0f, 500.0f, 1);
        A.ehit[3] = 5;
        bt_core_reset(&A);
        check(A.ehit[3] == 0, "bt_core_reset zeroes the neutral hit tally");
    }

    printf("[6] tree_turrets on neutral death (main.gd:583-593)\n");
    {
        fresh(&A, 209u);
        place_neutral(&A, 0, nk_tree, 500.0f, 500.0f, 1.0f);
        check(bt_neutral_death_turrets(&A, 0) == 0, "no pocket_factory -> no turret");
        check(A.n_pend_struct == 0, "nothing queued");

        /* real item wiring: items/all/pocket_factory/pocket_factory_effect_1.tres */
        fresh(&A, 210u);
        int fx_before = A.n_summon_fx;
        bt_apply_item(&A, BT_ITEM_IDX_POCKET_FACTORY);
        check(A.scalar_effect[BT_ISTR_TREE_TURRETS] == 1.0f, "pocket_factory stores tree_turrets = 1");
        check_near(bt_core_stat_read(&A, 16), 2.0f, 1e-6f,
                   "pocket_factory_effect_0 grants +2 engineering (its tree-turret scaling stat)");
        check(A.n_summon_fx == fx_before,
              "pocket_factory registers no structures_hash instance (its turret is spawned at death)");

        place_neutral(&A, 0, nk_tree, 500.0f, 500.0f, 1.0f);
        check(bt_neutral_death_turrets(&A, 0) == 1, "one turret queued per owned tree_turrets effect");
        check(A.n_pend_struct == 1, "queued into queues_to_spawn_structures");
        check(A.pend_struct[0].kind == 0xFFFFu, "runtime spawn (no summon_fx instance)");
        check(A.pend_struct[0].row == BT_ITEM_EFF_ROW_TREE_TURRET,
              "the queued row is turret_effect_1.tres (BT_ITEM_EFF_ROW_TREE_TURRET)");
        check(A.pend_struct[0].neutral == 0, "a turret is a structure, not a neutral");
        /* the turret spawns at a random point within 200 of the tree
         * (entity_spawner.gd:468-476). */
        {
            float dx = A.pend_struct[0].x - 500.0f, dy = A.pend_struct[0].y - 500.0f;
            check(bt_fabsf(dx) <= 100.0f + 1e-3f && bt_fabsf(dy) <= 100.0f + 1e-3f,
                  "turret position is inside the 200-wide spawn area");
        }

        /* the emulated EntityBirth lifecycle activates it with its real WeaponStats */
        bt_spawn_poll(&A);
        check(A.n_pend_struct == 0 && A.n_births == 1, "queue entry became one birth");
        check(A.births[0].bclass == 1, "the birth is a STRUCTURE");
        check(A.births[0].row == BT_ITEM_EFF_ROW_TREE_TURRET, "birth carries the turret row");
        A.births[0].active = 2;              /* birth timer elapsed */
        bt_birth_flush(&A);
        check(A.n_births == 0, "birth consumed at the deferred boundary");
        check(A.pet_n == 1, "one live structure/pet entity");
        check(A.pet_alive[0] && A.pet_isstruct[0],
              "the tree turret is a structures_hash entity (MAX_STRUCTURES accounted)");
        check(A.pet_row[0] == BT_ITEM_EFF_ROW_TREE_TURRET && A.pet_opcode[0] == BT_ITEMEFF_TURRET,
              "instantiated from the turret effect row");
        /* real WeaponStats, not a fake DPS: turret_stats.tres cd 32 ticks,
         * damage 10 + 0.8*engineering(2) = 11 (pocket_factory's +2 engineering). */
        check_near(A.pet_dmg[0], 11.0f, 1e-6f, "turret damage comes from its real WeaponStats");
        check_near(A.pet_cd[0], 32.0f / 60.0f, 1e-6f, "turret cooldown is its real 32-tick WeaponStats cd");
        check_near(A.pet_timer[0], 0.0f, 1e-6f, "a turret starts ready (no fake first-shot grant)");
        check(A.pet_nproj[0] >= 1, "the turret has its real projectile count");

        /* the single-entry wrapper resolves the death and queues the turret
         * without ever crediting the kill itself */
        fresh(&A, 217u);
        bt_apply_item(&A, BT_ITEM_IDX_POCKET_FACTORY);
        place_neutral(&A, 0, nk_tree, 500.0f, 500.0f, 1.0f);
        A.ehp[0] -= 1.0f;                     /* lethal damage already applied */
        check(bt_neutral_resolve_hit(&A, 0, 0, 1) == 1, "resolve_hit flags the kill");
        check(A.ealive[0], "resolve_hit does not kill the slot");
        check(A.n_pend_struct == 1, "resolve_hit queued the tree turret before crediting");
        bt_credit_kill(&A, 0, 1);
        check(!A.ealive[0], "the caller's bt_credit_kill handles the death");
    }

    printf("[7] spawn-group modifiers (entity_spawner.gd:170-182)\n");
    {
        fresh(&A, 211u);
        check_near(bt_neutral_enemy_count_modifier(&A), 0.0f, 1e-6f, "no items -> 0 enemy modifier");
        check_near(bt_neutral_tree_count_modifier(&A), 0.0f, 1e-6f, "no items -> 0 tree modifier");
        check_near(bt_neutral_group_number(&A, 0, 0, 0.0f, 3.0f), 3.0f, 1e-6f, "enemy count unchanged");
        check_near(bt_neutral_group_number(&A, 1, 0, 0.0f, 3.0f), 3.0f, 1e-6f, "tree count unchanged");

        /* real item wiring: mouse +10 / number_of_enemies (SUM) */
        bt_apply_item(&A, BT_ITEM_IDX_MOUSE);
        check_near(A.scalar_effect[BT_ISTR_NUMBER_OF_ENEMIES], 10.0f, 1e-6f,
                   "mouse stores number_of_enemies +10");
        check_near(bt_neutral_enemy_count_modifier(&A), 0.1f, 1e-6f, "modifier is sum/100");
        check_near(bt_neutral_group_number(&A, 0, 0, 0.0f, 3.0f), 3.3f, 1e-5f, "number *= 1.1");

        /* candle -10 cancels mouse: another real producer */
        bt_apply_item(&A, BT_ITEM_IDX_CANDLE);
        check_near(A.scalar_effect[BT_ISTR_NUMBER_OF_ENEMIES], 0.0f, 1e-6f, "mouse +10, candle -10 -> 0");
        check_near(bt_neutral_group_number(&A, 0, 0, 0.0f, 3.0f), 3.0f, 1e-5f, "back to 3");

        /* white_flag -5 (fewer enemies) */
        bt_apply_item(&A, BT_ITEM_IDX_WHITE_FLAG);
        check_near(bt_neutral_group_number(&A, 0, 0, 0.0f, 3.0f), 3.0f * 0.95f, 1e-5f, "number *= 0.95");

        /* loot groups skip the enemy modifier entirely (entity_spawner.gd:178) */
        check_near(bt_neutral_group_number(&A, 0, 1, 0.0f, 3.0f), 3.0f, 1e-6f,
                   "a loot group never takes the enemy count modifier");

        /* coop factor adds before the modifier (entity_spawner.gd:179) */
        fresh(&B, 212u);
        check_near(bt_neutral_group_number(&B, 0, 0, 0.5f, 2.0f), 3.0f, 1e-6f,
                   "coop_factor 0.5 adds 50% to the rolled count");

        /* clamp: number = max(1.0, ...) (entity_spawner.gd:180) */
        fresh(&B, 213u);
        bt_apply_item(&B, BT_ITEM_IDX_WHITE_FLAG);   /* -5% */
        check_near(bt_neutral_group_number(&B, 0, 0, 0.0f, 3.0f), 2.85f, 1e-5f,
                   "3 trees -> 2.85 with a -5% modifier");
        check_near(bt_neutral_group_number(&B, 0, 0, 0.0f, 1.0f), 1.0f, 1e-5f,
                   "max(1.0, 0.95) clamps a fractional count up to 1");

        /* trees: tree_effect_1 +1, applied only to the NEUTRAL domain */
        fresh(&B, 214u);
        bt_apply_item(&B, BT_ITEM_IDX_TREE);
        check_near(bt_neutral_tree_count_modifier(&B), 1.0f, 1e-6f, "tree item -> trees +1");
        check_near(bt_neutral_group_number(&B, 1, 0, 0.0f, 3.0f), 4.0f, 1e-6f, "tree count +1");
        check_near(bt_neutral_group_number(&B, 0, 0, 0.0f, 3.0f), 3.0f, 1e-6f,
                   "the trees modifier never touches an enemy count");
    }

    printf("[8] neutral domain separation (no enemy behaviour / boost)\n");
    {
        fresh(&A, 215u);
        int e = find_kind("anemone");
        check(e >= 0, "anemone kind present");
        check(bt_spawn_neutral(&A, nk_tree, 400.0f, 400.0f) == 1, "spawn a tree");
        int ns = neutral_slot(&A, nk_tree);
        check(ns == 0, "tree occupies slot 0");

        check(A.eneutral[ns] == 1, "slot tagged neutral");
        check(A.ebeh[ns] == 0, "no enemy attack behaviour");
        check(A.eelite[ns] == 0, "never an elite/boss");
        check_near(A.eatk[ns], 0.0f, 1e-6f, "no attack cooldown");
        check_near(A.ebtimer[ns], 0.0f, 1e-6f, "no buffer boost timer");
        check(A.eboosted[ns] == 0, "is_boosted false");
        check(bt_kunit(&A, ns) == &BT_NEUTRALS[nk_tree], "bt_kunit resolves the BT_NEUTRALS domain");
        check(bt_kgeom(&A, ns) == &BT_NEUTRAL_PHYSICS[nk_tree], "bt_kgeom resolves neutral physics");

        /* an enemy slot is never tallied by the neutral helper */
        if (e >= 0) {
            check(bt_spawn_kind(&A, e, 900.0f, 900.0f) == 1, "spawn an enemy");
            int es = -1;
            for (int i = 0; i < BT_MAX; i++)
                if (A.ealive[i] && !A.eneutral[i]) { es = i; break; }
            check(es >= 0, "enemy slot found");
            A.ehp[es] -= 1.0f;
            check(bt_neutral_on_damage(&A, es, 0, 1) == 0, "the helper ignores an enemy slot");
            check(A.ehit[es] == 0, "no tally is written to an enemy slot");
        }

        /* one live step: the neutral neither moves nor shoots (no enemy state) */
        fresh(&A, 216u);
        bt_spawn_neutral(&A, nk_tree, 700.0f, 500.0f);
        ns = neutral_slot(&A, nk_tree);
        check(ns >= 0, "neutral placed");
        float sx = A.ex[ns], sy = A.ey[ns];
        float obs[BT_OBS_SIZE];
        float rew = 0.0f, term = 0.0f;
        bt_core_step(&A, 0, obs, &rew, &term);
        check_near(A.ex[ns], sx, 0.0f, "a neutral does not move");
        check_near(A.ey[ns], sy, 0.0f, "a neutral does not move");
        int live_proj = 0;
        for (int i = 0; i < BT_MAX_PROJ; i++) live_proj += A.pralive[i] ? 1 : 0;
        check(live_proj == 0, "a neutral fires nothing");
    }

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
