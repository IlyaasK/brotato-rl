/* tests/test_brotato_curse_weapons.cpp
 *
 * Source-backed tests for the summon-weapon / curse slice of the shared Brotato
 * core: bt_spawn_summon_row, bt_set_burn_from_item, bt_scaling_damage and the
 * pet fire block's consumption of them.
 *
 * Every expectation is transcribed from the recovered sources:
 *   weapon_service.gd:47-95,237-249   init_structure|structure_pet|pet stats,
 *                                    range adds and the init-time damage bonus
 *   weapon_service.gd:251-253, unit.gd:285-300   crit chance source + per-hit roll
 *   weapon_service.gd:281-286, projectiles/player_explosion.gd:68-71
 *                                    explosion damage/radius (scalar_events.h)
 *   weapon_service.gd:296-333        init_burning_data (scaling + bonus)
 *   weapon_service.gd:489-500        apply_scaling_stats_to_damage / bonuses
 *   burning_data.gd:20-24            BurningData.merge (chance/damage add, max dur)
 *   burn_chance_effect.gd:13         global burn bucket merge
 *   unit.tscn BurningTimer, unit.gd:67-71,618-646,661-707
 *                                    burn period + per-timeout tick + re-burn max
 *   dlc_1_data.gd:71-97,175-178,196-208  curse branches + boosts
 *   player_run_data.gd:441,515       stat_structure_percent_damage / burn bucket
 *
 * Build and run:
 *     c++ -O2 -o build/test_brotato_curse_weapons tests/test_brotato_curse_weapons.cpp -lm
 *     ./build/test_brotato_curse_weapons
 *
 * Exit code 0 = all checks passed.
 */

#include "../ocean/brotato/brotato_core.h"
/* the explosion radius/damage-multiplier consumers (usually included by the core
 * after brotato_progression.h; included here so this TU always has them) */
#include "../ocean/brotato/brotato_scalar_events.h"

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

static void step1(BtState* s, int action, float* obs, float* rew, float* term) {
    bt_core_step(s, action, obs, rew, term);
}

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

/* --- recovered-table helpers (mirror the source access patterns) --- */
static int is_struct_class(int op) {
    return op == BT_ITEMEFF_STRUCTURE || op == BT_ITEMEFF_TURRET ||
           op == BT_ITEMEFF_BUILDER_TURRET || op == BT_ITEMEFF_BOT_O_MINE;
}
static const BtItemWeaponStats* wstats_of(const BtItemEff* E) {
    return (E->stats != BT_ITEM_EFF_NONE && E->stats < BT_NITEM_WSTATS) ? &BT_ITEM_WSTATS[E->stats] : 0;
}
static const BtItemEff* sub_of(const BtItemEff* E, int i) {
    if (E->sub_count == BT_ITEM_EFF_NONE || i < 0 || i >= (int)E->sub_count) return 0;
    int idx = (int)E->sub_first + i;
    if (idx >= BT_NITEM_EFF_INDEX) return 0;
    return &BT_ITEM_EFF_ROWS[BT_ITEM_EFF_INDEX[idx]];
}
/* dlc_1_data.gd:71-97 branch order, re-derived here so the expectations below
 * stay tied to the row's own scaling/burning fields. */
static int curse_branch(const BtItemEff* E, const BtItemWeaponStats* W) {
    if (E->key == BT_ISTR_WANDERING_BOT) return 0;
    if (E->spawn_cooldown > 0) return 1;
    if (W && W->scaling_count > 0) return 2;
    const BtItemEff* C = sub_of(E, 0);
    if (C && C->opcode == BT_ITEMEFF_BURNING) return 3;
    return 4;
}
/* first structure-class effect row satisfying `branch` */
static int find_struct_row(int branch) {
    for (int r = 0; r < BT_NITEM_EFF_ROWS; r++) {
        const BtItemEff* E = &BT_ITEM_EFF_ROWS[r];
        if (!is_struct_class((int)E->opcode)) continue;
        if (E->scene == BT_ISTR_NONE) continue;
        if (curse_branch(E, wstats_of(E)) != branch) continue;
        return r;
    }
    return -1;
}
static int find_struct_pet_row(void) {
    for (int r = 0; r < BT_NITEM_EFF_ROWS; r++) {
        const BtItemEff* E = &BT_ITEM_EFF_ROWS[r];
        if (!is_struct_class((int)E->opcode)) continue;
        if (E->scene == BT_ISTR_NONE || !E->is_pet) continue;   /* StructureEffect.is_pet */
        return r;
    }
    return -1;
}
static int find_pet_row(int opcode) {
    for (int r = 0; r < BT_NITEM_EFF_ROWS; r++)
        if ((int)BT_ITEM_EFF_ROWS[r].opcode == opcode && BT_ITEM_EFF_ROWS[r].scene != BT_ISTR_NONE)
            return r;
    return -1;
}
/* the row carrying the player's global BurningData (BurnChanceEffect/BurningEffect) */
static int find_burn_row(void) {
    for (int r = 0; r < BT_NITEM_EFF_ROWS; r++) {
        const BtItemEff* E = &BT_ITEM_EFF_ROWS[r];
        if (E->opcode != BT_ITEMEFF_BURN_CHANCE && E->opcode != BT_ITEMEFF_BURNING) continue;
        if (E->burning == BT_ITEM_EFF_NONE || E->burning >= BT_NITEM_BURNING) continue;
        return r;
    }
    return -1;
}

/* weapon_service.gd:489-495 + dlc_1_data.gd:381-386: max(1, damage + sum of the
 * stat's contribution with every ratio boosted by (1+mod)). `only_stat16` keeps
 * the expectation tied to the one stat this test drives. */
static int expect_scaling_damage(const BtItemWeaponStats* W, int damage, float stat_val,
                                 float mod, int only_stat16) {
    float sum = 0.0f;
    for (int i = 0; i < (int)W->scaling_count; i++) {
        const BtItemScaling sc = BT_ITEM_SCALING[W->scaling_first + i];
        int id = bt_core_id_from_istr(sc.stat);
        if (only_stat16 && id != 16) continue;
        sum += (id == 16 ? stat_val : 0.0f) * (sc.ratio * (1.0f + mod));
    }
    int v = (int)((float)damage + sum);
    return v < 1 ? 1 : v;
}

/* Spawn the entity registered for `item` with a known per-effect curse modifier:
 * bt_register_summon + the emulated 1 s StructureTimer / 3-frame poll / 60-frame
 * EntityBirth chain, which is tick 123 for a fresh wave (see test_brotato_core). */
static void spawn_registered(BtState* s, int item, float mod, float* obs, float* rew, float* term) {
    bt_register_summon(s, (unsigned short)item, mod);
    for (int t = 1; t <= 123; t++) step1(s, 0, obs, rew, term);
}

int main(void) {
    float obs[BT_OBS_SIZE], rew, term;

    int row2 = find_struct_row(2), row3 = find_struct_row(3), row4 = find_struct_row(4);
    int row_sp = find_struct_pet_row();
    int row_dog = find_pet_row(BT_ITEMEFF_BONK_DOG);
    int row_burn = find_burn_row();
    check(row2 >= 0, "a structure row with scaling rows exists (weapon-stats curse branch)");
    check(row3 >= 0, "a structure row whose effects[0] is a BurningEffect exists (turret_flame)");
    check(row4 >= 0, "a non-scaling structure row without burning exists (garden)");
    check(row_sp >= 0, "a StructureEffect with is_pet exists (init_structure_pet_stats)");
    check(row_dog >= 0, "a melee pet row exists (init_melee_pet_stats)");
    check(row_burn >= 0, "an item-level BurningData row exists");

    printf("[1] structure damage uses structure_percent_damage, not stat_percent_damage\n");
    {
        /* weapon_service.gd:239-243: a structure (is_structure, not is_pet) takes
         * 1 + structure_percent_damage/100 INSTEAD of the player's
         * stat_percent_damage; a structure-pet (init_structure_pet_stats) adds it
         * on top; an ordinary pet uses stat_percent_damage alone.
         * player_run_data.gd:441 keys the stat as "structure_percent_damage" and
         * nothing in the Bull/Danger-5 sources writes it (no item effect keys it,
         * it is absent from ItemService.upgrades, and builder_turret.gd:263 hangs
         * off the unsimulated bonus-gold conversion), so the term is exactly 0 --
         * read from scalar_effect under the emitted key, never hardcoded. */
        const BtItemEff* E2 = &BT_ITEM_EFF_ROWS[row2];
        const BtItemWeaponStats* W2 = wstats_of(E2);
        int eng = 100;
        int struct_dmg = expect_scaling_damage(W2, (int)W2->damage, (float)eng, 0.0f, 1);

        bt_core_init(&A, 501u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        bt_core_stat_add(&A, 5, 100.0f);                 /* +100% player percent damage */
        bt_core_stat_add(&A, 16, (float)eng);
        check_near(bt_structure_percent_damage(&A), 0.0f, 0.0f,
                   "structure_percent_damage has no producer -> 0");
        spawn_registered(&A, (int)E2->item, 0.0f, obs, &rew, &term);
        check(A.pet_n == 1 && A.pet_alive[0], "structure entity spawned through the lifecycle");
        check(A.pet_isstruct[0] == 1, "structure lives in the structures_hash slot");
        check(A.pet_dmg[0] == (float)struct_dmg,
              "structure damage = round(scaling_damage * (1 + structure_percent/100))");
        check(A.pet_dmg[0] != (float)(struct_dmg * 2),
              "structure damage ignores the player's stat_percent_damage (weapon_service.gd:241)");

        bt_core_init(&A, 501u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        bt_core_stat_add(&A, 5, 100.0f);
        bt_core_stat_add(&A, 16, (float)eng);
        bt_register_summon(&A, (unsigned short)BT_ITEM_EFF_ROWS[row_dog].item, 0.0f);
        for (int t = 1; t <= 123; t++) step1(&A, 0, obs, &rew, &term);
        check(A.pet_n == 1 && A.pet_dmg[0] >= 1.0f, "melee pet spawned");
        check(A.summon_fx[0].is_pet == 1, "bonk_dog is a stat_pets_hash entry");
        {
            const BtItemWeaponStats* WD = wstats_of(&BT_ITEM_EFF_ROWS[row_dog]);
            int sd = expect_scaling_damage(WD, (int)WD->damage, (float)eng, 0.0f, 1);
            check(A.pet_dmg[0] == (float)(sd * 2),
                  "ordinary pet damage = round(scaling_damage * (1 + stat_percent_damage/100))");
        }

        bt_core_init(&A, 501u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        bt_core_stat_add(&A, 5, 100.0f);
        bt_core_stat_add(&A, 16, (float)eng);
        bt_register_summon(&A, (unsigned short)BT_ITEM_EFF_ROWS[row_sp].item, 0.0f);
        for (int t = 1; t <= 123; t++) step1(&A, 0, obs, &rew, &term);
        {
            const BtItemWeaponStats* WS = wstats_of(&BT_ITEM_EFF_ROWS[row_sp]);
            int ss = expect_scaling_damage(WS, (int)WS->damage, (float)eng, 0.0f, 1);
            check(A.pet_dmg[0] == (float)(ss * 2),
                  "structure-pet damage = round(scaling_damage * (1 + (stat_percent + structure_percent)/100))");
        }
    }

    printf("[2] cursed structure branch 2: _boost_weapon_stats_damage (scaling_stats non-empty)\n");
    {
        const BtItemEff* E = &BT_ITEM_EFF_ROWS[row2];
        const BtItemWeaponStats* W = wstats_of(E);
        int item = (int)E->item;
        int eng = 100;
        float mod = 0.5f;
        int exp_u = expect_scaling_damage(W, (int)W->damage, (float)eng, 0.0f, 1);
        int exp_c = expect_scaling_damage(W, (int)bt_ceilf((float)W->damage * (1.0f + mod)),
                                          (float)eng, mod, 1);

        bt_core_init(&A, 601u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        bt_core_stat_add(&A, 16, (float)eng);
        spawn_registered(&A, item, 0.0f, obs, &rew, &term);
        float cdu = A.pet_cd[0], dmu = A.pet_dmg[0];
        check(dmu == (float)exp_u, "uncursed structure damage = damage + sum(stat*ratio)");

        bt_core_init(&B, 601u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        B.n_live_groups = 0;
        bt_core_stat_add(&B, 16, (float)eng);
        spawn_registered(&B, item, mod, obs, &rew, &term);
        check(B.pet_dmg[0] == (float)exp_c,
              "cursed structure damage = ceil(dmg*(1+mod)) + sum(stat*ratio*(1+mod))");
        check(B.pet_dmg[0] > dmu, "branch 2 boosts damage (not burning / not cooldown)");
        check_near(B.pet_cd[0], cdu, 1e-6f, "branch 2 leaves the structure cooldown alone");
    }

    printf("[3] cursed structure branch 3: nested BurningEffect scaling (turret_flame)\n");
    {
        const BtItemEff* E = &BT_ITEM_EFF_ROWS[row3];
        const BtItemWeaponStats* W = wstats_of(E);
        const BtItemEff* C = sub_of(E, 0);
        check(C && C->opcode == BT_ITEMEFF_BURNING, "the row's effects[0] is a BurningEffect");
        check(C && C->burning != BT_ITEM_EFF_NONE && C->burning < BT_NITEM_BURNING,
              "the nested BurningEffect carries a BT_ITEM_BURNING row");
        const BtItemBurning* Bn = &BT_ITEM_BURNING[C->burning];
        check(W->scaling_count == 0, "the structure's own stats carry no scaling rows (source branch 3)");
        int item = (int)E->item;
        int eng = 100;
        float mod = 0.5f;
        /* init_burning_data: global bucket empty -> raw = B.damage; scaling uses the
         * burning row; then apply_structure_damage_bonus (0 here). */
        int bu = (int)Bn->damage, bc = (int)Bn->damage;
        {
            float su = 0.0f, sc_ = 0.0f;
            for (int i = 0; i < (int)Bn->scaling_count; i++) {
                const BtItemScaling sc = BT_ITEM_SCALING[Bn->scaling_first + i];
                if (bt_core_id_from_istr(sc.stat) != 16) continue;
                su += (float)eng * sc.ratio;
                sc_ += (float)eng * sc.ratio * (1.0f + mod);
            }
            bu = (int)((float)Bn->damage + su);
            bc = (int)((float)Bn->damage + sc_);
            if (bu < 1) bu = 1;
            if (bc < 1) bc = 1;
        }

        bt_core_init(&A, 602u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        bt_core_stat_add(&A, 16, (float)eng);
        spawn_registered(&A, item, 0.0f, obs, &rew, &term);
        float cdu = A.pet_cd[0];
        check_near(A.pet_burn_chance[0], Bn->chance, 1e-6f, "uncursed burning chance = BurningData.chance");
        check_near(A.pet_burn_dur[0], (float)Bn->duration, 1e-6f, "uncursed burning duration = BurningData.duration");
        check(A.pet_burn_dmg[0] == (float)bu, "uncursed burning damage = max(1, damage + sum)");

        bt_core_init(&B, 602u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        B.n_live_groups = 0;
        bt_core_stat_add(&B, 16, (float)eng);
        spawn_registered(&B, item, mod, obs, &rew, &term);
        check(B.pet_burn_dmg[0] == (float)bc,
              "cursed burning damage grows with the boosted scaling ratios (dlc_1_data.gd:90-93)");
        check(B.pet_burn_dmg[0] > A.pet_burn_dmg[0], "branch 3 boosts the burning, not the weapon stats");
        check_near(B.pet_dmg[0], A.pet_dmg[0], 1e-6f,
                   "branch 3 leaves the weapon damage untouched (FlameTurret forces damage 1)");
        check_near(B.pet_cd[0], cdu, 1e-6f, "branch 3 leaves the cooldown untouched");
    }

    printf("[4] cursed structure branch 4: the structure's own cooldown (garden)\n");
    {
        const BtItemEff* E = &BT_ITEM_EFF_ROWS[row4];
        const BtItemWeaponStats* W = wstats_of(E);
        check(W && W->scaling_count == 0, "branch 4 row has no scaling rows");
        check(!sub_of(E, 0) || sub_of(E, 0)->opcode != BT_ITEMEFF_BURNING, "branch 4 row has no burning child");
        int item = (int)E->item;
        float mod = 0.5f;
        /* dlc_1_data.gd:94-96: apply_attack_speed_mod_to_cooldown(cd / TICKS_PER_SECOND,
         * mod) * TICKS_PER_SECOND (MIN_COOLDOWN 2). */
        int cd_u = (int)W->cooldown;
        int secs = cd_u / 60;
        int v = (int)((float)secs / (1.0f + mod));
        if (v < 2) v = 2;
        int cd_c = v * 60;
        if (cd_c < 2) cd_c = 2;

        bt_core_init(&A, 603u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        spawn_registered(&A, item, 0.0f, obs, &rew, &term);
        check_near(A.pet_cd[0], (float)cd_u / 60.0f, 1e-6f, "uncursed structure cooldown = W.cooldown/60");
        float dmu = A.pet_dmg[0];

        bt_core_init(&B, 603u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        B.n_live_groups = 0;
        spawn_registered(&B, item, mod, obs, &rew, &term);
        check_near(B.pet_cd[0], (float)cd_c / 60.0f, 1e-6f,
                   "cursed structure cooldown = apply_attack_speed_mod_to_cooldown(cd/60, mod)*60");
        check(B.pet_cd[0] < A.pet_cd[0], "branch 4 shortens the structure cooldown");
        check_near(B.pet_dmg[0], dmu, 1e-6f, "branch 4 leaves the weapon damage untouched");
    }

    printf("[5] the global burn bucket: BurningData.merge + init_burning_data\n");
    {
        /* burn_chance_effect.gd:13 merges into effects[Keys.burn_chance_hash];
         * BurningData.merge adds chance and damage and takes the max duration;
         * the bucket keeps BurningData.new()'s elemental 1:1 scaling
         * (player_run_data.gd:515) and apply_damage_bonus then uses
         * stat_percent_damage. dlc_1_data.gd:196-208 ceil-boosts the cursed row. */
        const BtItemEff* E = &BT_ITEM_EFF_ROWS[row_burn];
        const BtItemBurning* Bn = &BT_ITEM_BURNING[E->burning];
        bt_core_init(&A, 604u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        bt_set_burn_from_item(&A, (unsigned short)row_burn, 0.0f);
        check_near(A.burn_chance, Bn->chance, 1e-6f, "chance = BurningData.chance");
        check_near(A.burn_dur, (float)Bn->duration, 1e-6f, "duration = BurningData.duration");
        check_near(A.burn_dmg, (float)Bn->damage, 1e-6f, "damage = max(1, raw + elemental*1)");

        bt_core_init(&A, 604u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        bt_core_stat_add(&A, 8, 20.0f);
        bt_set_burn_from_item(&A, (unsigned short)row_burn, 0.0f);
        check_near(A.burn_dmg, (float)Bn->damage + 20.0f, 1e-6f,
                   "the bucket scales with stat_elemental_damage 1:1 (BurningData.new default)");
        /* apply_damage_bonus: a SECOND owned copy of the same row would merge
         * (adding raw damage again), so the percent-damage re-evaluation uses a
         * fresh bucket built once with both stats already present. */
        bt_core_init(&A, 604u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        bt_core_stat_add(&A, 8, 20.0f);
        bt_core_stat_add(&A, 5, 100.0f);
        bt_set_burn_from_item(&A, (unsigned short)row_burn, 0.0f);
        check_near(A.burn_dmg, 2.0f * ((float)Bn->damage + 20.0f), 1e-6f,
                   "apply_damage_bonus multiplies by (1 + stat_percent_damage/100)");

        bt_core_init(&B, 604u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        B.n_live_groups = 0;
        bt_set_burn_from_item(&B, (unsigned short)row_burn, 0.5f);
        check_near(B.burn_chance, Bn->chance, 1e-6f, "the curse does not touch chance");
        check_near(B.burn_dur, (float)(int)bt_ceilf((float)Bn->duration * 1.5f), 1e-6f,
                   "cursed duration = ceil(dur*(1+mod))");
        float cursed_raw = (float)(int)bt_ceilf((float)Bn->damage * 1.5f);
        check_near(B.burn_dmg, cursed_raw, 1e-6f, "cursed damage = ceil(dmg*(1+mod))");
        bt_set_burn_from_item(&B, (unsigned short)row_burn, 0.5f);
        check_near(B.burn_chance, 2.0f * Bn->chance, 1e-6f, "merge: chance adds per owned copy");
        check_near(B.burn_dmg, 2.0f * cursed_raw, 1e-6f, "merge: damage adds per owned copy");
    }

    printf("[6] consumer-visible burning: a cursed flame turret burns what it hits\n");
    {
        int chaser = find_kind("chaser");
        check(chaser >= 0, "chaser kind found");
        if (chaser >= 0 && row3 >= 0) {
            const BtItemEff* E = &BT_ITEM_EFF_ROWS[row3];
            const BtItemEff* C = sub_of(E, 0);
            const BtItemBurning* Bn = &BT_ITEM_BURNING[C->burning];
            int eng = 100;
            float mod = 0.5f;
            float sumc = 0.0f;
            for (int i = 0; i < (int)Bn->scaling_count; i++) {
                const BtItemScaling sc = BT_ITEM_SCALING[Bn->scaling_first + i];
                if (bt_core_id_from_istr(sc.stat) != 16) continue;
                sumc += (float)eng * sc.ratio * (1.0f + mod);
            }
            int bc = (int)((float)Bn->damage + sumc);
            if (bc < 1) bc = 1;

            bt_core_init(&A, 605u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            A.n_live_groups = 0;
            bt_core_stat_add(&A, 16, (float)eng);
            spawn_registered(&A, (int)E->item, mod, obs, &rew, &term);
            check(A.pet_n == 1 && A.pet_alive[0], "cursed flame turret spawned");
            check(A.pet_burn_dmg[0] == (float)bc, "the turret carries its curse-boosted burning");
            check_near(A.pet_burn_dur[0], (float)Bn->duration, 1e-6f, "burning duration rides the pet");

            int k = bt_reserve_slot(&A);
            bt_spawn_kind_at(&A, k, chaser, A.pet_x[0] + 2.0f, A.pet_y[0], 0);
            A.ehp[k] = 1000.0f; A.emax[k] = 1000.0f;
            int fired = 0;
            for (int t = 0; t < 120 && A.eburn[k] <= 0.0f; t++) {
                step1(&A, 0, obs, &rew, &term);
                for (int pj = 0; pj < BT_MAX_PROJ && !fired; pj++)
                    if (A.pralive[pj] && A.prfriendly[pj]) fired = 1;
            }
            check(fired, "the structure fired a friendly projectile at the enemy in range");
            check(A.eburn[k] > 0.0f, "the hit enemy is burning (consumer-visible)");
            check_near(A.eburndmg[k], (float)bc, 1e-3f, "per-tick burning damage = the cursed turret burning");
            check_near(A.eburn[k], bt_burn_period_frames(&A) * (float)Bn->duration, 1e-3f,
                       "a flush-applied burn retains its full duration until the next internal timer pass");
        }
    }

    printf("[7] the fire block fires spawn-time damage; pet shots ride their burning\n");
    {
        /* weapon_service.gd:237-249 applies the percent bonus once, at init; the
         * fire block must not re-apply stat_percent_damage per shot. */
        const BtItemEff* E2 = &BT_ITEM_EFF_ROWS[row2];
        int chaser = find_kind("chaser");
        if (chaser >= 0 && row2 >= 0) {
            int eng = 100;
            bt_core_init(&A, 601u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            A.n_live_groups = 0;
            bt_core_stat_add(&A, 16, (float)eng);
            bt_core_stat_add(&A, 5, 100.0f);
            spawn_registered(&A, (int)E2->item, 0.0f, obs, &rew, &term);
            float want = A.pet_dmg[0];
            check(want > 1.0f, "structure damage is non-trivial for the fire-block check");
            int k = bt_reserve_slot(&A);
            bt_spawn_kind_at(&A, k, chaser, A.pet_x[0] + 250.0f, A.pet_y[0], 0);
            A.ehp[k] = 1000.0f; A.emax[k] = 1000.0f;
            int pj = -1;
            for (int t = 0; t < 120 && pj < 0; t++) {
                step1(&A, 0, obs, &rew, &term);
                for (int q = 0; q < BT_MAX_PROJ; q++)
                    if (A.pralive[q] && A.prfriendly[q]) { pj = q; break; }
            }
            check(pj >= 0, "the structure fired a projectile");
            if (pj >= 0) {
                check_near(A.prdmg[pj], want, 1e-3f,
                           "the shot carries the spawn-time damage (no per-shot percent multiply)");
                check(A.prdmg[pj] != 2.0f * want,
                      "the player's stat_percent_damage is not applied a second time on the shot");
                check_near(A.prburn_chance[pj], A.pet_burn_chance[0], 1e-6f,
                           "a non-burning structure's shot carries no burning");
            }
        }
    }

    printf("[8] burning lands on the unit.tscn BurningTimer period (burn cooldown)\n");
    {
        /* unit.tscn's BurningTimer starts at wait_time 0.5 s and unit.gd:67-71
         * scales it by the summed burning_cooldown_increase/reduction player
         * effects; unit.gd:661-707 deals the burning damage once per timeout and
         * decrements the duration. The pet-shot path is exercised in [6]; this
         * block drives the burn directly so nothing else can touch the enemy. */
        int chaser = find_kind("chaser");
        check(chaser >= 0, "chaser kind found");
        if (chaser >= 0 && row3 >= 0) {
            const BtItemEff* C = sub_of(&BT_ITEM_EFF_ROWS[row3], 0);
            const BtItemBurning* Bn = &BT_ITEM_BURNING[C->burning];
            bt_core_init(&A, 609u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            A.n_live_groups = 0;
            float per0 = bt_burn_period_frames(&A);
            check_near(per0, 30.0f, 0.0f, "base BurningTimer period = 0.5 s = 30 frames");
            int k = bt_reserve_slot(&A);
            bt_spawn_kind_at(&A, k, chaser, A.px + 250.0f, A.py, 0);
            A.ehp[k] = 1000.0f; A.emax[k] = 1000.0f;
            bt_apply_enemy_burn(&A, k, 12.0f, (float)Bn->duration);
            check_near(A.eburn[k], (float)Bn->duration * per0, 1e-3f,
                       "a burn stores duration ticks * the period in frames");
            check_near(A.eburndmg[k], 12.0f, 1e-6f, "the burn keeps its per-tick damage");
            float hp0 = A.ehp[k];
            for (int t = 0; t < (int)per0 - 2; t++) step1(&A, 0, obs, &rew, &term);
            check_near(A.ehp[k], hp0, 1e-3f, "no burn damage before the timeout");
            int dropped = 0;
            for (int t = 0; t < 4 && !dropped; t++) {
                step1(&A, 0, obs, &rew, &term);
                if (A.ehp[k] < hp0) dropped = 1;
            }
            check(dropped, "the burn tick lands on the BurningTimer timeout");
            check_near(hp0 - A.ehp[k], 12.0f, 1e-3f, "one timeout deals exactly one burning damage tick");
            /* a second burn while one is active must not restart the timer */
            float rem = A.eburn[k];
            bt_apply_enemy_burn(&A, k, 5.0f, 2.0f);
            check_near(A.eburn[k], rem, 1e-6f, "a weaker/shorter re-burn does not restart the timer");
            check_near(A.eburndmg[k], 12.0f, 1e-6f, "the damage keeps the max of both burnings");

            /* the emitted buckets: eyes_surgery (burning_cooldown_reduction) and
             * frozen_heart (a negative reduction) scale the period. */
            int red_row = -1, inc_row = -1;
            for (int r = 0; r < BT_NITEM_EFF_ROWS; r++) {
                const BtItemEff* E = &BT_ITEM_EFF_ROWS[r];
                if (E->key != BT_ISTR_BURNING_COOLDOWN_REDUCTION) continue;
                if (E->value > 0 && red_row < 0) red_row = r;
                if (E->value < 0 && inc_row < 0) inc_row = r;
            }
            check(red_row >= 0, "an item keys the burning_cooldown_reduction bucket");
            if (red_row >= 0) {
                bt_core_init(&A, 610u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
                A.n_live_groups = 0;
                bt_apply_item(&A, (unsigned short)BT_ITEM_EFF_ROWS[red_row].item);
                float wt = 0.5f * (1.0f - (float)BT_ITEM_EFF_ROWS[red_row].value / 100.0f);
                check_near(bt_burn_period_frames(&A), bt_roundf(wt * 60.0f), 1e-6f,
                           "reduction% shortens the period (wait_time = 0.5 * (1 + inc - red))");
                check(bt_burn_period_frames(&A) < per0, "the reduced period is shorter than 0.5 s");
            }
            if (inc_row >= 0) {
                bt_core_init(&A, 611u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
                A.n_live_groups = 0;
                bt_apply_item(&A, (unsigned short)BT_ITEM_EFF_ROWS[inc_row].item);
                check(bt_burn_period_frames(&A) > per0, "a negative reduction lengthens the period");
            }
        }
    }

    printf("[9] projectile slots are cleaned on spawn (no stale burning)\n");
    {
        bt_core_init(&A, 606u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        int pj = bt_spawn_projectile(&A, A.px, A.py, 0.0f, 100.0f, 1.0f, BT_SHAPE_CIRCLE,
                                     BT_PROJ_R_DEFAULT, 0.0f, 0.0f, 0.0f, 0);
        check(pj >= 0, "bt_spawn_projectile returns the slot index (-1 when full)");
        check_near(A.prburn_chance[pj], 0.0f, 0.0f, "a fresh slot has no burning chance");
        /* pretend a burning pet shot occupied it, then reuse the slot */
        A.prburn_chance[pj] = 1.0f; A.prburn_dmg[pj] = 7.0f; A.prburn_dur[pj] = 8.0f;
        A.prfriendly[pj] = 1;
        A.pralive[pj] = 0;
        int pj2 = bt_spawn_projectile(&A, A.px, A.py, 0.0f, 100.0f, 1.0f, BT_SHAPE_CIRCLE,
                                      BT_PROJ_R_DEFAULT, 0.0f, 0.0f, 0.0f, 0);
        check(pj2 == pj, "the freed slot is the next one allocated");
        check_near(A.prburn_chance[pj2], 0.0f, 0.0f, "a recycled slot does not keep a stale burning chance");
        check_near(A.prburn_dmg[pj2], 0.0f, 0.0f, "a recycled slot does not keep a stale burning damage");
        check_near(A.prburn_dur[pj2], 0.0f, 0.0f, "a recycled slot does not keep a stale burning duration");
    }

    printf("[10] per-entity burning survives entity compaction\n");
    {
        if (row3 >= 0 && row_sp >= 0) {
            bt_core_init(&A, 608u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            A.n_live_groups = 0;
            bt_core_stat_add(&A, 16, 100.0f);
            bt_register_summon(&A, (unsigned short)BT_ITEM_EFF_ROWS[row_sp].item, 0.0f);
            /* a second entity after the first one so compaction has to move it */
            bt_register_summon(&A, (unsigned short)BT_ITEM_EFF_ROWS[row3].item, 0.5f);
            /* two structures drain one per SPAWN_DELAY poll, so the second birth
             * (3 frames later) activates at tick 126, not 123. */
            for (int t = 1; t <= 126; t++) step1(&A, 0, obs, &rew, &term);
            check(A.pet_n == 2, "two entities spawned");
            int burning_slot = -1, plain_slot = -1;
            for (int i = 0; i < A.pet_n; i++) {
                if (A.pet_burn_chance[i] > 0.0f) burning_slot = i;
                else plain_slot = i;
            }
            check(burning_slot >= 0, "the flame turret slot carries burning");
            check(plain_slot >= 0, "the plain entity slot has no burning");
            if (burning_slot >= 0 && plain_slot >= 0) {
                float keep_dmg = A.pet_burn_dmg[burning_slot];
                float keep_dur = A.pet_burn_dur[burning_slot];
                float keep_ch = A.pet_burn_chance[burning_slot];
                /* kill the PLAIN entity: the burning one must be moved down by the
                 * compaction with its per-entity burning intact */
                A.pet_alive[plain_slot] = 0;
                bt_pet_compact(&A);
                check(A.pet_n == 1, "compaction dropped the dead slot");
                check_near(A.pet_burn_chance[0], keep_ch, 1e-6f, "burning chance follows the entity");
                check_near(A.pet_burn_dmg[0], keep_dmg, 1e-6f, "burning damage follows the entity");
                check_near(A.pet_burn_dur[0], keep_dur, 1e-6f, "burning duration follows the entity");
            }
        }
    }

    printf("[11] structure_range, structures_can_crit and per-hit crit rolls\n");
    {
        /* weapon_service.gd:39,51,85: a melee pet adds stat_range/2, a ranged pet
         * (and the ProjectileEffect) adds stat_range, a structure/structure-pet
         * adds structure_range, all clamped to MIN_RANGE 25. weapon_service.gd:
         * 251-253 adds the player's stat_crit_chance unless the entity is a
         * structure and structures_can_crit is not owned; unit.gd:285-300 rolls the
         * crit per hit and multiplies the value BEFORE armor. */
        const BtItemEff* E2 = &BT_ITEM_EFF_ROWS[row2];
        const BtItemWeaponStats* W2 = wstats_of(E2);
        int crit_row = -1;
        for (int r = 0; r < BT_NITEM_EFF_ROWS && crit_row < 0; r++)
            if (BT_ITEM_EFF_ROWS[r].key == BT_ISTR_STRUCTURES_CAN_CRIT) crit_row = r;
        check(crit_row >= 0, "an item keys the structures_can_crit bucket");
        int row_cat = find_pet_row(BT_ITEMEFF_CATLING_GUN);
        check(row_cat >= 0, "a ranged pet row exists");

        bt_core_init(&A, 701u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        bt_core_stat_add(&A, 18, 100.0f);                       /* player stat_range */
        bt_core_stat_add(&A, 9, 30.0f);                         /* player stat_crit_chance */
        spawn_registered(&A, (int)E2->item, 0.0f, obs, &rew, &term);
        check_near(A.pet_range[0], bt_maxf(25.0f, (float)W2->max_range + bt_structure_range(&A)), 1e-3f,
                   "structure range = max(MIN_RANGE, max_range + structure_range)");
        check(A.pet_range[0] < (float)W2->max_range + 100.0f,
              "player stat_range does not reach a structure");
        check_near(A.pet_crit_chance[0], W2->crit_chance, 1e-6f,
                   "a structure without structures_can_crit gets no player crit");

        /* the structure_range bucket itself (no shipped item keys it; a keyed
         * effect would land here, exactly like the builder's runtime grant) */
        bt_core_init(&A, 702u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        A.scalar_effect[BT_ISTR_STRUCTURE_RANGE] = 40.0f;
        bt_core_stat_add(&A, 18, 100.0f);
        spawn_registered(&A, (int)E2->item, 0.0f, obs, &rew, &term);
        check_near(A.pet_range[0], (float)W2->max_range + 40.0f, 1e-3f,
                   "structure_range feeds the structure's range");

        {
            const BtItemWeaponStats* WC = wstats_of(&BT_ITEM_EFF_ROWS[row_cat]);
            bt_core_init(&A, 703u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            A.n_live_groups = 0;
            A.scalar_effect[BT_ISTR_STRUCTURE_RANGE] = 40.0f;
            bt_core_stat_add(&A, 18, 100.0f);
            bt_core_stat_add(&A, 9, 30.0f);
            spawn_registered(&A, (int)BT_ITEM_EFF_ROWS[row_cat].item, 0.0f, obs, &rew, &term);
            check_near(A.pet_range[0], (float)WC->max_range + 100.0f, 1e-3f,
                       "ranged pet range = max_range + stat_range (structure_range ignored)");
            check_near(A.pet_crit_chance[0], WC->crit_chance + 0.30f, 1e-6f,
                       "a pet always takes the player's stat_crit_chance");

            const BtItemWeaponStats* WD = wstats_of(&BT_ITEM_EFF_ROWS[row_dog]);
            bt_core_init(&A, 704u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            A.n_live_groups = 0;
            bt_core_stat_add(&A, 18, 100.0f);
            spawn_registered(&A, (int)BT_ITEM_EFF_ROWS[row_dog].item, 0.0f, obs, &rew, &term);
            check_near(A.pet_range[0], (float)WD->max_range + 50.0f, 1e-3f,
                       "melee pet range = max_range + stat_range/2");
        }

        if (crit_row >= 0) {
            int chaser = find_kind("chaser");
            check(chaser >= 0, "chaser kind found");
            bt_core_init(&A, 705u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            A.n_live_groups = 0;
            bt_core_stat_add(&A, 9, 100.0f);                    /* guaranteed crit */
            bt_core_stat_add(&A, 16, 100.0f);
            bt_apply_item(&A, (unsigned short)BT_ITEM_EFF_ROWS[crit_row].item);
            spawn_registered(&A, (int)E2->item, 0.0f, obs, &rew, &term);
            check_near(A.pet_crit_chance[0], W2->crit_chance + bt_core_stat_capped(&A, 9) * 0.01f, 1e-6f,
                       "structures_can_crit adds the player's crit to a structure");
            if (chaser >= 0) {
                int k = bt_reserve_slot(&A);
                bt_spawn_kind_at(&A, k, chaser, A.pet_x[0] + 250.0f, A.pet_y[0], 0);
                A.ehp[k] = 1000.0f; A.emax[k] = 1000.0f;
                float armor = BT_KINDS[chaser].armor + BT_KINDS[chaser].armor_grow * (float)(A.wave - 1);
                float want_crit = bt_roundf(bt_roundf(A.pet_dmg[0] * W2->crit_damage) * bt_armor_mult(armor));
                float want_plain = bt_roundf(A.pet_dmg[0] * bt_armor_mult(armor));
                float hp0 = A.ehp[k];
                for (int t = 0; t < 120 && A.ehp[k] >= hp0; t++) step1(&A, 0, obs, &rew, &term);
                check_near(hp0 - A.ehp[k], want_crit, 1e-3f,
                           "a crit multiplies the shot damage BEFORE armor (unit.gd:285-300)");
                check(want_crit != want_plain, "the crit actually changed the damage");
            }
        }
    }

    printf("[12] explosion_damage: Bull explosion + explosive summons\n");
    {
        /* weapon_service.gd:245-249 (is_exploding adds :247) and :281-286: the
         * multiplier is stat_percent_damage/100 + explosion_damage/100 on top of
         * the base. Dynamite/explosive_shells/honey key the bucket. */
        int expl_row = -1;
        for (int r = 0; r < BT_NITEM_EFF_ROWS && expl_row < 0; r++)
            if (BT_ITEM_EFF_ROWS[r].key == BT_ISTR_EXPLOSION_DAMAGE) expl_row = r;
        check(expl_row >= 0, "an item keys the explosion_damage bucket");
        int expl_dmg = expl_row >= 0 ? (int)BT_ITEM_EFF_ROWS[expl_row].value : 0;
        int land_row = -1;
        for (int r = 0; r < BT_NITEM_EFF_ROWS && land_row < 0; r++)
            if ((int)BT_ITEM_EFF_ROWS[r].opcode == BT_ITEMEFF_STRUCTURE &&
                bt_eff_is_exploding(&BT_ITEM_EFF_ROWS[r])) land_row = r;
        check(land_row >= 0, "an explosive structure row exists (landmines)");

        if (land_row >= 0) {
            const BtItemEff* EL = &BT_ITEM_EFF_ROWS[land_row];
            const BtItemWeaponStats* WL = wstats_of(EL);
            bt_core_init(&A, 801u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            A.n_live_groups = 0;
            /* in THIS state every stat is 0, so a scaling row (landmines' own
             * stat 19) contributes nothing and plain == WL->damage */
            check(WL && WL->scaling_count == 0 || bt_core_stat_read(&A, 16) == 0.0f,
                  "the explosive structure's scaling stat is 0 in this state");
            spawn_registered(&A, (int)EL->item, 0.0f, obs, &rew, &term);
            float plain = A.pet_dmg[0];
            check(plain == (float)(int)WL->damage,
                  "without explosion_damage an explosive structure keeps its base damage");
            if (expl_row >= 0) {
                bt_core_init(&A, 802u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
                A.n_live_groups = 0;
                bt_apply_item(&A, (unsigned short)BT_ITEM_EFF_ROWS[expl_row].item);
                check_near(bt_explosion_damage(&A), (float)expl_dmg, 1e-6f,
                           "the item feeds the explosion_damage bucket");
                spawn_registered(&A, (int)EL->item, 0.0f, obs, &rew, &term);
                check_near(A.pet_dmg[0], bt_roundf(plain * (1.0f + (float)expl_dmg * 0.01f)), 1e-3f,
                           "explosive structure damage *= (1 + explosion_damage/100)");
            }
        }

        /* the Bull explosion carries the same term (multiplier = 1 + (pct + expl)/100) */
        int chaser = find_kind("chaser");
        if (chaser >= 0 && expl_row >= 0) {
            bt_core_init(&A, 803u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            A.n_live_groups = 0;
            bt_core_stat_add(&A, 9, -3.0f);                     /* no crit: deterministic */
            bt_core_stat_add(&A, 5, 100.0f);
            int k = bt_reserve_slot(&A);
            bt_spawn_kind_at(&A, k, chaser, A.px + 20.0f, A.py, 0);
            A.ehp[k] = 10000.0f; A.emax[k] = 10000.0f;
            float armor = BT_KINDS[chaser].armor + BT_KINDS[chaser].armor_grow * (float)(A.wave - 1);
            float base = BT_EXPLODE_DMG + BT_EXPLODE_SCALE_STAT * A.dmg_stat;
            float hp0 = A.ehp[k];
            check(bt_explode(&A) >= 0, "the Bull row spawns a pooled explosion node");
            for (int t = 0; t < BT_EXPL_FRAMES + 1; t++) (void)bt_item_expl_tick(&A, &rew);
            check_near(hp0 - A.ehp[k], bt_roundf(bt_roundf(base * 2.0f) * bt_armor_mult(armor)), 1e-3f,
                       "Bull explosion = round(damage * (1 + pct/100))");

            bt_core_init(&B, 804u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            B.n_live_groups = 0;
            bt_core_stat_add(&B, 9, -3.0f);
            bt_core_stat_add(&B, 5, 100.0f);
            bt_apply_item(&B, (unsigned short)BT_ITEM_EFF_ROWS[expl_row].item);
            int k2 = bt_reserve_slot(&B);
            bt_spawn_kind_at(&B, k2, chaser, B.px + 20.0f, B.py, 0);
            B.ehp[k2] = 10000.0f; B.emax[k2] = 10000.0f;
            float hp2 = B.ehp[k2];
            check(bt_explode(&B) >= 0, "the Bull row spawns a pooled explosion node");
            for (int t = 0; t < BT_EXPL_FRAMES + 1; t++) (void)bt_item_expl_tick(&B, &rew);
            /* weapon_service.gd:239-247 (doubles in GDScript): multiplier =
             * (1 + stat_percent_damage/100) + explosion_damage/100. Compute it in
             * double, or the float32 grouping drops below the source value
             * (30 * 2.15 = 64.5 -> round 65). */
            float want_mult = (float)((1.0 + 100.0 / 100.0) + (double)expl_dmg / 100.0);
            check_near(hp2 - B.ehp[k2],
                       bt_roundf(bt_roundf(base * want_mult) * bt_armor_mult(armor)), 1e-3f,
                       "Bull explosion *= (1 + (stat_percent_damage + explosion_damage)/100)");
        }

        /* explosion_size scales the radius (projectiles/player_explosion.gd:68-71) */
        int size_row = -1;
        for (int r = 0; r < BT_NITEM_EFF_ROWS && size_row < 0; r++)
            if (BT_ITEM_EFF_ROWS[r].key == BT_ISTR_EXPLOSION_SIZE && BT_ITEM_EFF_ROWS[r].value > 0)
                size_row = r;
        check(size_row >= 0, "an item keys the explosion_size bucket");
        if (chaser >= 0 && size_row >= 0) {
            const BtBodyPhysics* G = &BT_PHYSICS[bt_geom_index(chaser)];
            float dist = BT_EXPLODE_R + G->hurt_radius + 1.0f;   /* outside the base radius */
            bt_core_init(&A, 805u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            A.n_live_groups = 0;
            bt_core_stat_add(&A, 9, -3.0f);
            int ka = bt_reserve_slot(&A);
            bt_spawn_kind_at(&A, ka, chaser, A.px + BT_EXPLODE_OFF_X + dist - G->hurt_x,
                             A.py + BT_EXPLODE_OFF_Y - G->hurt_y, 0);
            A.ehp[ka] = 10000.0f; A.emax[ka] = 10000.0f;
            float hpa = A.ehp[ka];
            check(bt_explode(&A) >= 0, "the Bull row spawns a pooled explosion node");
            for (int t = 0; t < BT_EXPL_FRAMES + 1; t++) (void)bt_item_expl_tick(&A, &rew);
            check_near(A.ehp[ka], hpa, 1e-3f, "outside the base explosion radius: no damage");

            bt_core_init(&B, 806u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            B.n_live_groups = 0;
            bt_core_stat_add(&B, 9, -3.0f);
            bt_apply_item(&B, (unsigned short)BT_ITEM_EFF_ROWS[size_row].item);
            int kb = bt_reserve_slot(&B);
            bt_spawn_kind_at(&B, kb, chaser, B.px + BT_EXPLODE_OFF_X + dist - G->hurt_x,
                             B.py + BT_EXPLODE_OFF_Y - G->hurt_y, 0);
            B.ehp[kb] = 10000.0f; B.emax[kb] = 10000.0f;
            float hpb = B.ehp[kb];
            check(bt_explode(&B) >= 0, "the Bull row spawns a pooled explosion node");
            for (int t = 0; t < BT_EXPL_FRAMES + 1; t++) (void)bt_item_expl_tick(&B, &rew);
            check(B.ehp[kb] < hpb, "the scaled radius reaches an enemy the base radius misses");
        }
    }

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
