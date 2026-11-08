/* tests/test_brotato_player_weapon_effects.cpp
 *
 * Source-backed regressions for the player weapon-effect consumers
 * (ocean/brotato/brotato_weapon_effects.h): the shoot / hit / crit / kill event
 * callbacks, the lifesteal timer, damage-type bonuses, the weapon cooldown
 * window, knockback, the charm-on-hit roll, the exploding trigger, and the
 * previously weapon-only item consumers.
 *
 * Every expectation is transcribed from the recovered sources:
 *   run_data.gd:1297-1302        manage_life_steal (randf < lifesteal -> 1, x2)
 *   player.gd:724-728            on_lifesteal_effect (LifestealTimer gating)
 *   player.tscn:1381             LifestealTimer wait_time 0.1 one_shot
 *   unit.gd:458-500              get_damage_value bonus percent
 *   unit.gd:348-378              crit-kill gold/heal, heal_on_kill
 *   unit.gd:282-300              one_shot_on_hit / crit_on_hitting_burning
 *   unit.gd:552-570              unit/weapon explode rolls (exploding trigger)
 *   enemy.gd:236-243             explode_on_overkill
 *   enemy.gd:266-272             bonus_current_health_damage
 *   weapon.gd:203-230,440-465    attack tallies, break_on_hit
 *   weapon.gd:318-347            get_next_cooldown / MAX_ATTACK_COUNT_HISTORY
 *   weapon.gd:345-347            update_current_spread
 *   weapon_service.gd:270-281    knockback clamp
 *   dlcs/dlc_1/effect_behaviors/enemy/charm_enemy_effect_behavior.gd:36-70
 *                                charm-on-hit roll (HP threshold + stat scaling)
 *   main.gd:798-860              reload_when_pickup_gold
 *
 * The taser/harpoon slow-in-zone is NOT a weapon-row consumer: SlowInZoneEffect
 * is a NullEffect, and the real slow is the projectile scene's SlowHitbox
 * (BtWeaponProjSpec.slow_rad/slow_amt -> bt_wproj_slow), regressed in
 * test_brotato_weapon_ranged.cpp.  projectiles_on_hit children are likewise the
 * weapon-projectile graph's (bt_wproj_spawn_onhit), not this header's.
 *
 * Build and run (after the weapon cutover has landed in the core):
 *     c++ -O2 -o build/test_brotato_player_weapon_effects \
 *         tests/test_brotato_player_weapon_effects.cpp -lm
 *     ./build/test_brotato_player_weapon_effects
 *
 * Exit code 0 = all checks passed.
 */

#include "../ocean/brotato/brotato_core.h"
/* the weapon-event consumers (usually included by the core after
 * brotato_scalar_events.h; included here so this TU always has them) */
#include "../ocean/brotato/brotato_weapon_effects.h"

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
static void check_eq_int(long long got, long long want, const char* msg) {
    g_checks++;
    if (got != want) { g_failures++; printf("  FAIL: %s (got %lld want %lld)\n", msg, got, want); }
}

static BtState A;
static BtState B;

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
    s->n_live_groups = 0;         /* enemies only when placed */
    bt_core_stat_add(s, 1, -15.0f);
    s->regen_timer = 99.0f;
    s->iframe = 0.0f;
    s->dodge = 0.0f;
    bt_wfx_reset(&s->wfx);
    s->player_lifesteal_cd = 0.0f;
}
static void place_enemy(BtState* s, int slot, int kind, float x, float y, float hp) {
    bt_spawn_kind_at(s, slot, kind, x, y, 0);
    /* These fixtures drive bt_ranged_step directly, so bt_core_step's per-frame
     * Node-transform refresh (s->ehx/s->ehy = the PRE-step position,
     * brotato_state.h:749-765) never runs.  State it: the target selection and
     * the shot band read the target's Node transform (`target.global_position`,
     * utils.gd:270 / weapon.gd:248), which here is the spawn position. */
    s->ehx[slot] = s->ex[slot]; s->ehy[slot] = s->ey[slot];
    s->ehp[slot] = hp; s->emax[slot] = hp;
    s->edmg[slot] = 1.0f; s->espd[slot] = 0.0f; s->eatk[slot] = 99.0f;
    s->edecay[slot] = 0.0f;
}
/* First canonical effect row carrying `key` (or -1). */
static int find_row(unsigned short key) {
    for (int r = 0; r < BT_NITEM_EFF_ROWS; r++)
        if (BT_ITEM_EFF_ROWS[r].key == key || BT_ITEM_EFF_ROWS[r].custom_key == key) return r;
    return -1;
}
/* First canonical effect row with `opcode` (or -1). */
static int find_opcode(int opcode) {
    for (int r = 0; r < BT_NITEM_EFF_ROWS; r++)
        if ((int)BT_ITEM_EFF_ROWS[r].opcode == opcode) return r;
    return -1;
}
/* Register one owned copy of a canonical row into the bucket, exactly as
 * bt_scalar_bucket_register does from bt_apply_item. */
static void own_row(BtState* s, int row, int v, int v2) {
    if (s->bk_count[row] == 0) s->bk_active[s->n_bk_active++] = (unsigned short)row;
    s->bk_count[row]++;
    bt_row_variant_add(s, row, v, v2);
}
/* A weapon whose own fx rows register an item-related weapon-bonus bucket. */
static int wpn_has_bucket_row(int q) {
    BT_CONST_PTR(BtWeaponDef) D = &BT_WEAPONS[q];
    for (int k = 0; k < (int)D->fx_count; k++) {
        int idx = (int)D->fx_first + k;
        if (idx >= BT_NWEAPON_EFF_INDEX) break;
        int row = (int)BT_WEAPON_EFF_INDEX[idx];
        if (row < 0 || row >= BT_NITEM_EFF_ROWS) continue;
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[row];
        if (E->storage == 1 && bt_iwb_bucket_of_custom_key(E->custom_key) >= 0) return 1;
    }
    return 0;
}
/* Whether two weapons share a class set (so a 2-weapon loadout could trigger a
 * set bonus that would confound a stat comparison). */
static int wpn_sets_overlap(int a, int b) {
    BT_CONST_PTR(BtWeaponDef) A = &BT_WEAPONS[a];
    BT_CONST_PTR(BtWeaponDef) B = &BT_WEAPONS[b];
    for (int i = 0; i < (int)A->set_count; i++) {
        int sa = (int)BT_WEAPON_SET_IDS[A->set_first + i];
        for (int j = 0; j < (int)B->set_count; j++)
            if ((int)BT_WEAPON_SET_IDS[B->set_first + j] == sa) return 1;
    }
    return 0;
}
/* A weapon this test can add without any side effect beyond the weapon itself:
 * valid base stats and no item-related weapon-bonus row. */
static int wpn_test_usable(int q) {
    return (int)BT_WEAPONS[q].stats < BT_NWEAPON_STATS && !wpn_has_bucket_row(q);
}

int main(void) {
    const int kind_anemone = find_kind("anemone");   /* armor 0.0 */
    check(kind_anemone >= 0, "anemone kind is present (armor 0 fixture)");

    printf("[1] lifesteal: LifestealTimer gate (0.1 s) + unconditional draw\n");
    {
        fresh(&A, 11);
        A.hp = 5.0f; A.max_hp = 30.0f;
        BtRng before = A.rng;
        float healed = bt_wfx_lifesteal(&A, &A.player_lifesteal_cd, 0.0f);
        check(healed == 0.0f, "0%% lifesteal heals nothing");
        /* run_data.gd:1297-1302 manage_life_steal's FIRST statement is
         * `if randf() < weapon_stats.lifesteal:` -- the draw is evaluated for
         * every hit (the value only decides the comparison) and the 0.1 s
         * LifestealTimer gate lives in the listener player.gd:724-728, i.e.
         * AFTER the draw.  Both weapon hit sites (weapon.gd:412 melee,
         * player_projectile.gd:105 ranged) call it unconditionally. */
        check(memcmp(&before, &A.rng, sizeof(BtRng)) != 0,
              "0%% lifesteal still consumes its global randf() (run_data.gd:1298)");
        check(A.player_lifesteal_cd == 0.0f, "0%% lifesteal does not start the timer");

        healed = bt_wfx_lifesteal(&A, &A.player_lifesteal_cd, 1.0f);   /* always rolls */
        check_near(healed, 1.0f, 0.0f, "100%% lifesteal heals 1");
        check_near(A.hp, 6.0f, 0.0f, "lifesteal added 1 HP");
        check_near(A.player_lifesteal_cd, BT_WFX_LIFESTEAL_SECONDS, 0.0f,
                   "LifestealTimer armed for 0.1 s (SECONDS)");
        healed = bt_wfx_lifesteal(&A, &A.player_lifesteal_cd, 1.0f);
        check_near(healed, 0.0f, 0.0f,
                   "a lifesteal inside the 0.1 s window is dropped (player.gd:725)");
        check_near(A.hp, 6.0f, 0.0f, "no HP gained inside the window");

        A.player_lifesteal_cd = 0.0f;
        A.scalar_effect[BT_ISTR_STAT_DOUBLE_LIFESTEAL_BONUS] = 1.0f;
        healed = bt_wfx_lifesteal(&A, &A.player_lifesteal_cd, 1.0f);
        check_near(healed, 2.0f, 0.0f, "double_lifesteal_bonus heals 2");
        A.scalar_effect[BT_ISTR_STAT_DOUBLE_LIFESTEAL_BONUS] = 0.0f;
    }

    printf("[2] damage-type: elemental detection from the canonical scaling table\n");
    {
        int elem_first = -1;
        for (int i = 0; i < BT_NITEM_SCALING; i++)
            if (bt_core_id_from_istr(BT_ITEM_SCALING[i].stat) == 8) { elem_first = i; break; }
        check(elem_first >= 0, "an elemental scaling row exists");
        if (elem_first >= 0)
            check(bt_wfx_item_scaling_is_elemental((unsigned short)elem_first, 1) == 1,
                  "elemental ITEM scaling slice is classified elemental");
        check(bt_wfx_item_scaling_is_elemental(0, 0) == 0, "empty scaling slice is non-elemental");
    }

    printf("[3] hit bonus %%: above/below HP thresholds on canonical rows\n");
    {
        fresh(&A, 12);
        place_enemy(&A, 0, kind_anemone, 100.0f, 100.0f, 100.0f);
        int above = find_row(BT_ISTR_BONUS_DAMAGE_AGAINST_TARGETS_ABOVE_HP);
        int below = find_row(BT_ISTR_BONUS_DAMAGE_AGAINST_TARGETS_BELOW_HP);
        check(above >= 0, "an above_hp row exists in the canonical graph");
        check(below >= 0, "a below_hp row exists in the canonical graph");

        if (above >= 0) {
            own_row(&A, above, 30, 50);          /* value = pct, value2 = hp threshold % */
            check_near(bt_wfx_hit_bonus_pct(&A, 0, 0, 0, 0, 0), 30.0f, 0.0f,
                       "above-hp bonus applies at full HP (>= 50%%)");
            A.ehp[0] = 40.0f;
            check_near(bt_wfx_hit_bonus_pct(&A, 0, 0, 0, 0, 0), 0.0f, 0.0f,
                       "above-hp bonus drops below the threshold");
        }
        if (below >= 0) {
            fresh(&A, 13);
            place_enemy(&A, 0, kind_anemone, 100.0f, 100.0f, 100.0f);
            own_row(&A, below, 20, 30);
            A.ehp[0] = 25.0f;
            check_near(bt_wfx_hit_bonus_pct(&A, 0, 0, 0, 0, 0), 20.0f, 0.0f,
                       "below-hp bonus applies at <= 30%% HP");
            A.ehp[0] = 90.0f;
            check_near(bt_wfx_hit_bonus_pct(&A, 0, 0, 0, 0, 0), 0.0f, 0.0f,
                       "below-hp bonus drops above the threshold");
            /* weapon-row view reaches the same helper */
            fresh(&A, 14);
            place_enemy(&A, 0, kind_anemone, 100.0f, 100.0f, 100.0f);
            A.ehp[0] = 25.0f;
            unsigned short rows[1]; rows[0] = (unsigned short)below;
            check_near(bt_wfx_hit_bonus_pct(&A, 0, rows, 1, 0, 0),
                       (float)BT_ITEM_EFF_ROWS[below].value, 0.0f,
                       "weapon-row view yields the row's own value below threshold");
        }
    }

    printf("[4] crit kill: gold_on_crit_kill + heal_on_crit_kill\n");
    {
        fresh(&A, 15);
        A.hp = 1.0f; A.max_hp = 30.0f;
        int g = find_row(BT_ISTR_GOLD_ON_CRIT_KILL);
        check(g >= 0, "a gold_on_crit_kill row exists");
        if (g >= 0) {
            own_row(&A, g, 100, 0);              /* 100%% chance copy */
            check(bt_wfx_crit_kill_gold(&A, 0, 0) == 1,
                  "a 100%% gold_on_crit_kill crit kill pays 1 gold");
            fresh(&A, 16);
            own_row(&A, g, 0, 0);
            check(bt_wfx_crit_kill_gold(&A, 0, 0) == 0, "0%% crit-kill row pays no gold");
        }
        fresh(&A, 161);
        for (int n = 0; n < 4; n++) bt_apply_item(&A, BT_ITEM_IDX_HUNTING_TROPHY);
        check(bt_wfx_crit_kill_gold(&A, 0, 0) == 1,
              "four hunting trophies merge into one guaranteed gold roll");
        fresh(&A, 17);
        A.hp = 1.0f; A.max_hp = 30.0f;
        A.scalar_effect[BT_ISTR_HEAL_ON_CRIT_KILL] = 100.0f;
        (void)bt_wfx_crit_kill_gold(&A, 0, 0);
        check_near(A.hp, 2.0f, 0.0f, "heal_on_crit_kill heals 1 on a crit kill");
    }

    printf("[5] on-kill: heal_on_kill + attack-kill stat, uid-scoped tallies\n");
    {
        fresh(&A, 18);
        A.hp = 1.0f; A.max_hp = 30.0f;
        A.scalar_effect[BT_ISTR_HEAL_ON_KILL] = 100.0f;
        int gold = 0;
        bt_wfx_on_player_kill(&A, 0, 0, 0.0f, 0, 0, &gold);
        check_near(A.hp, 2.0f, 0.0f, "heal_on_kill heals 1 (unit.gd:363)");

        /* weapon.gd:203-230 on_killed_something has TWO attack-kill producers.
         * The player-scoped `gain_stat_when_attack_killed_enemies` KEY_VALUE
         * bucket (weapon.gd:210, read here via bt_eff_is over s->bk_active) has
         * exactly ONE producer in the recovered sources: the DLC dwarf character
         * effect (brotato_abyss
         * dlcs/dlc_1/characters/dwarf/effects/dwarf_effect_0.tres = key
         * stat_engineering, custom_key gain_stat_when_attack_killed_enemies,
         * +1 at every 6th attack kill).  The base game only DECLARES the bucket
         * (singletons/player_run_data.gd:586); no shipped item or weapon fills
         * it, and the Bull/Knight/Demon selector has no dwarf, so the canonical
         * generated graph carries no row with that custom_key.  Exercise the
         * branch with the bucket it actually has -- a strict no-op -- and assert
         * the full +value-at-count path only when a future graph carries the
         * row. */
        int ak = find_row(BT_ISTR_GAIN_STAT_WHEN_ATTACK_KILLED_ENEMIES);
        {
            fresh(&A, 19);
            float ak_all[BT_NSTATS];
            for (int q = 0; q < BT_NSTATS; q++) ak_all[q] = bt_core_stat_read(&A, q);
            BtRng ak_rng = A.rng;
            bt_wfx_on_weapon_kill(&A, &A.wfx, 0, 40u, 77, 0, 0, 0);
            bt_wfx_on_weapon_kill(&A, &A.wfx, 0, 40u, 77, 0, 0, 0);
            int ak_quiet = 1;
            for (int q = 0; q < BT_NSTATS; q++)
                if (bt_core_stat_read(&A, q) != ak_all[q]) ak_quiet = 0;
            check(ak_quiet, "an empty attack-kill bucket grants no stat");
            check(memcmp(&ak_rng, &A.rng, sizeof(BtRng)) == 0,
                  "an empty attack-kill bucket draws no RNG");
            check(bt_wfx_attack_kills(&A.wfx, 40u, 77) == 2,
                  "the attack-kill tally still tracks 2 kills for one attack id");
        }
        if (ak >= 0) {
            fresh(&A, 19);
            int stat_id = bt_core_id_from_istr(BT_ITEM_EFF_ROWS[ak].key);
            check(stat_id != 255, "the attack-kill row keys a core stat");
            float before = bt_core_stat_read(&A, stat_id);
            own_row(&A, ak, 3, 2);               /* +3 after 2 kills by ONE attack */
            bt_wfx_on_weapon_kill(&A, &A.wfx, 0, 40u, 77, 0, 0, 0);
            check_near(bt_core_stat_read(&A, stat_id), before, 0.0f,
                       "1st kill of the attack grants nothing");
            bt_wfx_on_weapon_kill(&A, &A.wfx, 0, 40u, 77, 0, 0, 0);
            check_near(bt_core_stat_read(&A, stat_id), before + 3.0f, 0.0f,
                       "2nd kill of the same attack grants +3");
            /* a DIFFERENT weapon copy with the same attack id is a different tally */
            check(bt_wfx_attack_kills(&A.wfx, 41u, 77) == 0,
                  "the same attack id on another weapon copy is not conflated");
        }

        /* GainStatEveryKilledEnemiesEffect on the weapon's own row: the second,
         * always-reachable producer (weapon.gd:216-222). */
        {
            fresh(&A, 19);
            int gk = find_opcode(BT_ITEMEFF_GAIN_STAT_EVERY_KILLED_ENEMIES);
            check(gk >= 0, "a GainStatEveryKilledEnemiesEffect row exists");
            if (gk >= 0) {
                unsigned short rows[1]; rows[0] = (unsigned short)gk;
                int sid = bt_core_id_from_istr(BT_ITEM_EFF_ROWS[gk].aux_key);
                int every = BT_ITEM_EFF_ROWS[gk].value;
                check(sid != 255 && every > 0, "the per-kill row names a stat and cadence");
                if (sid != 255 && every > 0) {
                    float b0 = bt_core_stat_read(&A, sid);
                    for (int i = 0; i < every; i++)
                        bt_wfx_on_weapon_kill(&A, &A.wfx, 0, 50u, 90 + i, rows, 1, 0);
                    check_near(bt_core_stat_read(&A, sid),
                               b0 + (float)BT_ITEM_EFF_ROWS[gk].nb_stat_scaled, 0.0f,
                               "GainStatEveryKilledEnemiesEffect grants after `value` kills");
                }
            }
        }

        /* break_on_hit -> the gold to drop, per the row's value2 */
        fresh(&A, 20);
        int br = find_row(BT_ISTR_BREAK_ON_HIT);
        check(br >= 0, "a break_on_hit row exists");
        if (br >= 0) {
            unsigned short rows[1]; rows[0] = (unsigned short)br;
            int gold2 = bt_wfx_on_weapon_hit(&A, &A.wfx, 0, 60u, 5, rows, 1, 0);
            check(gold2 == BT_ITEM_EFF_ROWS[br].value2 || gold2 == 0,
                  "break_on_hit returns the row's drop value or 0");
            check(bt_wfx_attack_hits(&A.wfx, 60u, 5) == 1, "hit tally counted the hit");
        }
    }

    printf("[6] cooldown window + accuracy spread (weapon.gd:318-347)\n");
    {
        fresh(&A, 21);
        check_near(bt_wfx_next_cooldown(&A, 60, 5, 5, 3, 0, 4.0f), 240.0f, 0.0f,
                   "big reload returns cooldown * additional_cooldown_multiplier");
        for (int i = 0; i < 256; i++) {
            float cd = bt_wfx_next_cooldown(&A, 60, -1, 0, 3, 0, 1.0f);
            if (cd < 24.0f || cd > 96.0f) { check(0, "random cooldown outside the source window"); break; }
        }
        check(1, "random cooldown stays in [basis - max_rand, basis + max_rand]");
        for (int i = 0; i < 64; i++) {
            float sp = bt_wfx_accuracy_spread(&A, 0.25f);
            if (sp < -0.75f || sp > 0.75f) { check(0, "accuracy spread out of range"); break; }
        }
        check(1, "accuracy spread bounded by accuracy");
    }

    printf("[7] charm on hit (charm_enemy_effect_behavior.gd:36-70)\n");
    {
        fresh(&A, 23);
        place_enemy(&A, 0, kind_anemone, 100.0f, 100.0f, 100.0f);
        int ch = find_row(BT_ISTR_CHARM_ON_HIT);
        check(ch >= 0, "a charm_on_hit row exists");
        if (ch >= 0) {
            unsigned short rows[1]; rows[0] = (unsigned short)ch;
            float thr = (float)BT_ITEM_EFF_ROWS[ch].value2 * 0.01f;
            check(thr > 0.0f, "the charm row carries an HP threshold");
            /* above the threshold the row contributes nothing -> never charms */
            int above_n = 0;
            for (int i = 0; i < 16; i++) {
                if (bt_wfx_charm_roll(&A, 0, 0, 999, rows, 1, 1.0f)) above_n++;
            }
            check(above_n == 0, "no charm above the row's HP threshold");
            /* below the threshold the chance is value/100 (flute: 0.1) */
            A.ehp[0] = A.emax[0] * (thr * 0.5f);
            int hits = 0;
            for (int i = 0; i < 400; i++)
                if (bt_wfx_charm_roll(&A, 0, 0, 999, rows, 1, 1.0f)) hits++;
            check(hits > 0, "charm can succeed below the HP threshold");
            check(hits < 400, "charm is not guaranteed by a sub-100%% row");
            /* the count cap refuses once max_charm is reached */
            check(bt_wfx_charm_roll(&A, 0, 999, 999, rows, 1, 1.0f) == 0,
                  "charm refused at max_charm_count");
        }
    }

    printf("[8] exploding trigger (unit.gd:552-570): the weapon-row ExplodingEffect consumer\n");
    {
        fresh(&A, 25);
        int ex = find_opcode(BT_ITEMEFF_EXPLODING);
        check(ex >= 0, "an ExplodingEffect weapon row exists (rocket_launcher)");
        if (ex >= 0)
            check(bt_wfx_weapon_explode(&A, (unsigned short)ex, 0.0f, 0.0f, 1.0f,
                                        0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f) <= 1,
                  "the exploding trigger is reachable and bounded");
    }

    printf("[9] reload on gold queries (main.gd:798-860)\n");
    {
        fresh(&A, 26);
        int rg = find_row(BT_ISTR_RELOAD_WHEN_PICKUP_GOLD);
        check(rg >= 0, "a reload_when_pickup_gold row exists");
        if (rg >= 0) {
            unsigned short rows[1]; rows[0] = (unsigned short)rg;
            check(bt_wfx_weapon_reloads_on_gold(rows, 1) == 1,
                  "a weapon row with reload_when_pickup_gold reloads that weapon");
            check(bt_wfx_weapon_reloads_on_gold(0, 0) == 0, "an empty effect view never reloads");
        }
        A.scalar_effect[BT_ISTR_RELOAD_WHEN_PICKUP_GOLD] = 1.0f;
        check(bt_wfx_reload_all_on_gold(&A) == 1,
              "the player-level reload_when_pickup_gold reloads every weapon");
        fresh(&A, 27);
        check(bt_wfx_reload_turrets(&A, 0, 0) == 0, "no reload_turrets row resets nothing");
        int rt = find_row(BT_ISTR_RELOAD_TURRETS_ON_SHOOT);
        if (rt >= 0) {
            unsigned short rows[1]; rows[0] = (unsigned short)rt;
            check(bt_wfx_reload_turrets(&A, rows, 1) == 0,
                  "with no live BuilderTurret the reload resets nothing");
        }
    }

    printf("[10] state lifecycle: per-copy histories, slot compaction, forget\n");
    {
        fresh(&A, 28);
        A.wpn_count = 3;
        A.wpn_uid[0] = 100u; A.wpn_uid[1] = 101u; A.wpn_uid[2] = 102u;
        check(A.wfx.overflow == 0, "a fresh state has no tally overflow");
        /* one hit and one kill for each of three live copies, interleaved */
        for (int rep = 0; rep < 3; rep++) {
            for (int i = 0; i < 3; i++) {
                unsigned int u = A.wpn_uid[i];
                (void)bt_wfx_on_weapon_hit(&A, &A.wfx, i, u, (int)u + rep, 0, 0, 0);
                bt_wfx_on_weapon_kill(&A, &A.wfx, i, u, (int)u + rep, 0, 0, 0);
            }
        }
        for (int i = 0; i < 3; i++) {
            unsigned int u = A.wpn_uid[i];
            check(bt_wfx_attack_hits(&A.wfx, u, (int)u + 2) == 1,
                  "each weapon copy keeps its own interleaved hit history");
            check(bt_wfx_attack_kills(&A.wfx, u, (int)u + 2) == 1,
                  "each weapon copy keeps its own interleaved kill history");
        }
        /* MAX_ATTACK_COUNT_HISTORY is PER WEAPON: filling copy 100's window must
         * not evict copy 101's or 102's ids */
        for (int id = 1; id <= BT_WFX_ATTACK_HISTORY + 5; id++)
            (void)bt_wfx_on_weapon_hit(&A, &A.wfx, 0, 100u, 1000 + id, 0, 0, 0);
        /* the 25 ids 1001..1025 scroll the window to 1006..1025: the oldest
         * (1001) is evicted, the newest (1025) is retained */
        check(bt_wfx_attack_hits(&A.wfx, 100u, 1001) == 0,
              "the oldest id of THAT weapon is pruned");
        check(bt_wfx_attack_hits(&A.wfx, 100u, 1000 + BT_WFX_ATTACK_HISTORY + 5) == 1,
              "the newest id of THAT weapon is retained");
        check(bt_wfx_attack_hits(&A.wfx, 101u, 101 + 2) == 1,
              "another weapon's history is untouched by the window fill");
        check(A.wfx.overflow == 0, "no overflow: six per-copy blocks, three owners");

        /* slot compaction: the block MOVES with its copy (uid stays on the block) */
        bt_wfx_slot_move(&A.wfx, 1, 0);
        check(bt_wfx_attack_hits(&A.wfx, 101u, 101 + 2) == 1,
              "a moved copy keeps its history after tail compaction");
        A.wfx.wave_kills[1] = 9;
        bt_wfx_slot_move(&A.wfx, 1, 0);
        check(A.wfx.wave_kills[0] == 9, "the wave kill counter moves with the copy");

        /* actual removal: forget by uid clears only that copy's block */
        bt_wfx_forget_uid(&A.wfx, 100u);
        check(bt_wfx_attack_hits(&A.wfx, 100u, 1000 + BT_WFX_ATTACK_HISTORY + 5) == 0,
              "forget_uid drops the removed copy's history");
        check(bt_wfx_attack_hits(&A.wfx, 102u, 102 + 2) == 1,
              "forget_uid leaves other copies alone");

        bt_wfx_reset_wave(&A.wfx);
        check(bt_wfx_attack_hits(&A.wfx, 102u, 102 + 2) == 0, "wave reset drops every window");
        for (int i = 0; i < BT_WFX_SLOTS; i++) check(A.wfx.wave_kills[i] == 0, "wave reset clears the counters");
        bt_wfx_reset(&A.wfx);
        for (int i = 0; i < BT_WFX_SLOTS; i++)
            check(A.wfx.log[i].uid == BT_WFX_UID_FREE, "reset frees every history block");

        /* BtState wrappers used by WeaponInventory */
        A.wfx.wave_kills[0] = 5;
        bt_weapon_fx_slot_move(&A, 0, 2);
        check(A.wfx.wave_kills[2] == 5, "bt_weapon_fx_slot_move forwards to the state field");
        bt_weapon_fx_slot_erase(&A, 2);
        check(A.wfx.wave_kills[2] == 0, "bt_weapon_fx_slot_erase forwards to the state field");
        A.wfx.log[2].uid = 7u;
        bt_weapon_fx_forget_uid(&A, 7u);
        check(A.wfx.log[2].uid == BT_WFX_UID_FREE, "bt_weapon_fx_forget_uid forwards to the state field");
    }

    printf("[11] six weapons, one window each: no cross-owner eviction\n");
    {
        fresh(&A, 30);
        A.wpn_count = BT_WFX_SLOTS;
        for (int i = 0; i < BT_WFX_SLOTS; i++) A.wpn_uid[i] = 500u + (unsigned int)i;
        /* give every copy a full distinct window, interleaved one id per copy */
        for (int id = 0; id < BT_WFX_ATTACK_HISTORY; id++)
            for (int i = 0; i < BT_WFX_SLOTS; i++)
                (void)bt_wfx_on_weapon_hit(&A, &A.wfx, i, A.wpn_uid[i], id, 0, 0, 0);
        for (int i = 0; i < BT_WFX_SLOTS; i++)
            check(bt_wfx_attack_hits(&A.wfx, A.wpn_uid[i], 0) == 1,
                  "every one of the six copies kept its oldest id");
        check(A.wfx.overflow == 0, "six live copies fit six history blocks");
        /* one more id for copy 0 pushes only ITS window forward */
        (void)bt_wfx_on_weapon_hit(&A, &A.wfx, 0, 500u, BT_WFX_ATTACK_HISTORY, 0, 0, 0);
        check(bt_wfx_attack_hits(&A.wfx, 500u, 0) == 0, "copy 0 pruned its own oldest id");
        for (int i = 1; i < BT_WFX_SLOTS; i++)
            check(bt_wfx_attack_hits(&A.wfx, A.wpn_uid[i], 0) == 1,
                  "the other five copies are untouched");
    }

    printf("[12] burning-enemy link (linked_stats.gd:41, item_fried_rice)\n");
    {
        fresh(&A, 29);
        place_enemy(&A, 0, kind_anemone, 10.0f, 10.0f, 50.0f);
        place_enemy(&A, 1, kind_anemone, 20.0f, 20.0f, 50.0f);
        place_enemy(&A, 2, kind_anemone, 30.0f, 30.0f, 50.0f);
        check(bt_wfx_burning_enemies(&A) == 0, "no burning enemy -> count 0");
        A.eburn[0] = 10.0f; A.eburndmg[0] = 1.0f;
        A.eburn[2] = 5.0f;  A.eburndmg[2] = 1.0f;
        check(bt_wfx_burning_enemies(&A) == 2, "two burning enemies counted");
        /* a burning enemy that dies stops counting */
        bt_enemy_set_alive(&A, 0, 0);
        check(bt_wfx_burning_enemies(&A) == 1, "a dead enemy is not a burning enemy");

        int fr = -1;
        for (int r = 0; r < BT_NITEM_EFF_ROWS; r++)
            if (BT_ITEM_EFF_ROWS[r].aux_key == BT_ISTR_BURNING_ENEMY) { fr = r; break; }
        check(fr >= 0, "a burning_enemy-scaled link row exists (fried_rice)");
        if (fr >= 0) {
            check(BT_ITEM_EFF_ROWS[fr].nb_stat_scaled != 0, "the link row carries nb_stat_scaled");
            check_near(bt_wfx_burning_enemy_link_bonus(&A, (unsigned short)fr), 1.0f, 0.0f,
                       "+1 stat per burning enemy with one burning and nb 1");
            bt_enemy_set_alive(&A, 0, 1); A.eburn[0] = 10.0f;   /* two burning -> +2 */
            check_near(bt_wfx_burning_enemy_link_bonus(&A, (unsigned short)fr), 2.0f, 0.0f,
                       "the link scales with the burning-enemy count");
        }
        int other = find_opcode(BT_ITEMEFF_ONE_SHOT_ON_HIT);
        if (other >= 0)
            check_near(bt_wfx_burning_enemy_link_bonus(&A, (unsigned short)other), 0.0f, 0.0f,
                       "a non-burning-enemy row contributes nothing");
    }

    /* ================================================================ */
    /* 13. break_on_hit rolls EVERY such row, not just the first
     *     (weapon.gd:440-465 has no `break`: a second successful row emits a
     *     second wanted_to_break, whose listener returns early only because the
     *     weapon is already gone -- player.gd:393-408 -- so the DRAW of every
     *     later row still happens). */
    {
        int br = find_row(BT_ISTR_BREAK_ON_HIT);
        check(br >= 0, "a break_on_hit row exists (DLC brick)");
        if (br >= 0) {
            BtState* s = &A; fresh(s, 71);
            unsigned short rows[2] = { (unsigned short)br, (unsigned short)br };
            float chance = (float)BT_ITEM_EFF_ROWS[br].value * 0.01f;
            int row_gold = (int)BT_ITEM_EFF_ROWS[br].value2;
            BtRng r0 = s->rng;
            BtRng q = r0;
            int want = 0;
            if (chance != 0.0f) {
                float d1 = bt_math_randf(&q);   /* the FIRST row's chance roll */
                (void)bt_math_randf(&q);        /* the second row's roll */
                want = (d1 <= chance) ? row_gold : 0;
            }
            int got = bt_wfx_on_weapon_hit(s, &s->wfx, 0, 5u, -1, rows, 2, 0);
            int draws = 0;
            { BtRng q2 = r0; while (draws < 64 && q2.state != s->rng.state) { (void)bt_rng_next(&q2); draws++; } }
            check_eq_int(draws, chance != 0.0f ? 2 : 0,
                         "each of the two break_on_hit rows rolled its own chance");
            check_eq_int(got, want, "the first successful row's gold is reported");
        }
    }

    /* 14. unit.gd:575-585 condition ORDER: the burn chance is rolled BEFORE the
     *     is_healing / can_burn_enemies gates, so a rejected roll still consumed
     *     its global draw (and the gates cannot short-circuit ahead of it). */
    {
        BtState* s = &A; fresh(s, 73);
        place_enemy(s, 0, kind_anemone, 100.0f, 400.0f, 1000.0f);
        int j = 0;
        BtRng r1 = s->rng;
        (void)bt_rng_next(&r1);
        s->eburn[j] = 0.0f;
        check_eq_int(bt_wfx_weapon_burn_apply(s, j, 0.0f, 1.0f, 5.0f, 60.0f), 0,
                     "can_burn_enemies 0 -> the burn is not applied");
        check(s->rng.state == r1.state && s->rng.inc == r1.inc,
              "…but the chance roll still consumed its draw");
        check(s->eburn[j] <= 0.0f, "nothing was ignited");
        /* the applying path: exactly one draw and the target burns */
        BtRng r3 = s->rng;
        (void)bt_rng_next(&r3);
        check_eq_int(bt_wfx_weapon_burn_apply(s, j, 1.0f, 1.0f, 5.0f, 60.0f), 1,
                     "can_burn_enemies > 0 -> applied");
        /* the chance roll is consumed; apply_burning then adds its own burn-sound
         * pick (unit.gd:642), so only ">= 1 roll" is asserted here */
        check(s->rng.state != r3.state && s->rng.inc == r3.inc, "the chance roll was consumed");
        check(s->eburn[j] > 0.0f, "the target is burning");
        /* chance 0 consumes nothing at all (utils.gd:397-403 early-out) */
        BtRng r4 = s->rng;
        check_eq_int(bt_wfx_weapon_burn_apply(s, j, 1.0f, 0.0f, 5.0f, 60.0f), 0,
                     "chance 0 -> no burn");
        check(s->rng.state == r4.state, "chance 0 -> no draw");
    }

    /* ================================================================ */
    /* 15. Item -> weapon-bucket producers (run_data.gd:857-875 acquisition;
     *     effects/items/weapon_cooldown_effect.gd apply()).  Acquiring the
     *     shipped minimum_weapon_cooldowns item raises a live weapon's effective
     *     period so `shooting + cooldown` reaches the item's tick minimum
     *     (weapon_service.gd:558-571); the maximum bucket lowers it with the
     *     SMALLEST value winning (weapon_service.gd:584-600); and the firing
     *     cadence follows the raised period.  No unapply is exercised: the game
     *     only removes Mirror/Goldfish/Hourglass. */
    {
        int cd_row = -1, item = -1;
        for (int r = 0; r < BT_NITEM_EFF_ROWS && cd_row < 0; r++) {
            BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[r];
            if ((int)E->opcode != BT_ITEMEFF_WEAPON_COOLDOWN) continue;
            if (E->key != (unsigned short)BT_ISTR_MINIMUM_WEAPON_COOLDOWNS) continue;
            cd_row = r; item = (int)E->item;
        }
        check(cd_row >= 0 && item >= 0, "the shipped minimum_weapon_cooldowns row exists");
        /* the lowest-cooldown ranged tier (the minimum only bites below it) */
        int w = -1;
        for (int q = 0; q < BT_NWEAPONS; q++) {
            if (BT_WEAPONS[q].kind != BT_WEAPON_KIND_RANGED) continue;
            if ((int)BT_WEAPONS[q].stats >= BT_NWEAPON_STATS) continue;
            if (BT_WEAPON_STATS[BT_WEAPONS[q].stats].cooldown > 8) continue;
            if (w < 0 || BT_WEAPONS[q].stats < BT_WEAPONS[w].stats) w = q;
        }
        check(w >= 0, "a low-cooldown ranged weapon exists");
        if (cd_row >= 0 && w >= 0) {
            int min_ticks = BT_ITEM_EFF_ROWS[cd_row].value;
            /* --- without the item --- */
            BtState* s = &A; fresh(s, 79);
            int slot0 = bt_weapon_add(s, (unsigned short)w, 0, 0.0f);
            BtPlayerWeaponStats st0;
            (void)bt_weapon_stats(s, slot0, &st0);
            float base_total0 = bt_ws_shooting_frames(&st0, st0.atk_spd) + (float)st0.cooldown;
            check(base_total0 < (float)min_ticks, "the base total is below the item minimum");
            s->wpn_cd[slot0] = 0.0f;
            {
                float ox = 0.0f, oy = 0.0f;
                bt_ranged_origin(s, (int)s->wpn_count, slot0, &BT_WEAPON_GEOM[BT_WEAPONS[w].geom], &ox, &oy);
                place_enemy(s, 0, kind_anemone, ox + 120.0f, oy, 1000.0f);
            }
            for (int t = 0; t < 400; t++) bt_ranged_step(s, 0, 1);
            unsigned int shots0 = s->wproj.n_spawned;
            check(shots0 > 0, "the weapon fires without the item");
            /* --- with the item acquired --- */
            BtState* s2 = &B; fresh(s2, 79);
            int slot = bt_weapon_add(s2, (unsigned short)w, 0, 0.0f);
            bt_apply_item(s2, (unsigned short)item);
            BtPlayerWeaponStats st;
            (void)bt_weapon_stats(s2, slot, &st);
            float total = bt_ws_shooting_frames(&st, st.atk_spd) + (float)st.cooldown;
            check(st.cooldown > st0.cooldown, "acquiring the item raises the effective cooldown");
            check(total >= (float)min_ticks && total <= (float)min_ticks + 1.0f,
                  "…so shooting + cooldown reaches the item's tick minimum (ceil, <= +1)");
            s2->wpn_cd[slot] = 0.0f;
            {
                float ox = 0.0f, oy = 0.0f;
                bt_ranged_origin(s2, (int)s2->wpn_count, slot, &BT_WEAPON_GEOM[BT_WEAPONS[w].geom], &ox, &oy);
                place_enemy(s2, 0, kind_anemone, ox + 120.0f, oy, 1000.0f);
            }
            for (int t = 0; t < 400; t++) bt_ranged_step(s2, 0, 1);
            unsigned int shots1 = s2->wproj.n_spawned;
            check(shots1 > 0 && shots1 < shots0,
                  "the raised period slows the firing cadence (fewer shots over 400 ticks)");
            /* --- maximum bucket precedence: the SMALLEST value wins --- */
            bt_wstats_max_cd_add(s2, 40);
            bt_wstats_max_cd_add(s2, 8);
            (void)bt_weapon_stats(s2, slot, &st);
            float total2 = bt_ws_shooting_frames(&st, st.atk_spd) + (float)st.cooldown;
            check(total2 <= 40.0f && total2 < total,
                  "the maximum bucket lowers the total toward the smallest owned value");
        }
    }

    /* ================================================================ */
    /* 16. Item -> weapon_scaling_stats producers (effect.gd apply() KEY_VALUE
     *     push into effects[weapon_scaling_stats]; weapon_service.gd:618-632
     *     _apply_weapon_scaling_stat_effects; consumed at the
     *     weapon_scaling_stats rail of bt_weapon_stats).  The shipped producers
     *     are frozen_heart (stat_elemental_damage 10%) and nail
     *     (stat_engineering 20%): acquiring the item adds [stat, pct] to the
     *     player bucket, every live weapon gains an effective [stat, pct/100]
     *     scaling row, and its resolved DAMAGE rises by pct% of the player stat.
     *     A control state replicating the item's OTHER stat effects but not the
     *     scaling row isolates the row's contribution.  No unapply is exercised:
     *     the game only removes Mirror/Goldfish/Hourglass. */
    {
        int w = -1;
        for (int q = 0; q < BT_NWEAPONS; q++) {
            if (BT_WEAPONS[q].kind != BT_WEAPON_KIND_RANGED) continue;
            if ((int)BT_WEAPONS[q].stats >= BT_NWEAPON_STATS) continue;
            w = q; break;
        }
        check(w >= 0, "a ranged weapon with stats exists");

        if (w >= 0) {
            /* --- frozen_heart: elemental damage (core 8) x 10% --- */
            BtState* s = &A; fresh(s, 91);
            int slot = bt_weapon_add(s, (unsigned short)w, 0, 0.0f);
            bt_core_stat_add(s, 8, 208.0f);      /* item's +8 elemental folded in */
            bt_core_stat_add(s, 9, 5.0f);        /* item's +5 crit chance */
            BtPlayerWeaponStats ctl;
            (void)bt_weapon_stats(s, slot, &ctl);

            BtState* s2 = &B; fresh(s2, 91);
            int slot2 = bt_weapon_add(s2, (unsigned short)w, 0, 0.0f);
            bt_core_stat_add(s2, 8, 200.0f);
            bt_apply_item(s2, BT_ITEM_IDX_FROZEN_HEART);
            BtPlayerWeaponStats got;
            (void)bt_weapon_stats(s2, slot2, &got);

            check_near(bt_core_stat_read(s2, 8), 208.0f, 0.0f,
                       "frozen_heart adds its +8 elemental damage");
            check_near(bt_core_stat_read(s2, 9), 5.0f, 0.0f,
                       "frozen_heart adds its +5 crit chance");
            float r0 = 0.0f, r1 = 0.0f;
            for (int i = 0; i < ctl.n_scaling; i++) r0 += ctl.sc_ratio[i];
            for (int i = 0; i < got.n_scaling; i++) r1 += got.sc_ratio[i];
            check_near(r1 - r0, 0.10f, 1e-4f,
                       "the weapon's effective scaling gains the item's 10% row");
            int delta = got.damage - ctl.damage;
            check(delta >= 20 && delta <= 21,
                  "weapon damage rises by ~10% of elemental damage (20.8 trunc)");

            /* stacking: every owned copy pushes its own row (source push_back per
             * copy), so a second copy doubles the ratio and the damage bonus.  The
             * second copy also repeats the +8 elemental / +5 crit effects, so the
             * control carries those too. */
            bt_apply_item(s2, BT_ITEM_IDX_FROZEN_HEART);
            BtPlayerWeaponStats got2;
            (void)bt_weapon_stats(s2, slot2, &got2);
            fresh(s, 91);
            int slot_c = bt_weapon_add(s, (unsigned short)w, 0, 0.0f);
            bt_core_stat_add(s, 8, 216.0f);
            bt_core_stat_add(s, 9, 10.0f);
            BtPlayerWeaponStats ctl2;
            (void)bt_weapon_stats(s, slot_c, &ctl2);
            float r0b = 0.0f, r2 = 0.0f;
            for (int i = 0; i < ctl2.n_scaling; i++) r0b += ctl2.sc_ratio[i];
            for (int i = 0; i < got2.n_scaling; i++) r2 += got2.sc_ratio[i];
            check_near(r2 - r0b, 0.20f, 1e-4f,
                       "a second copy stacks the 10% row to 20%");
            check(got2.damage - ctl2.damage >= 43 && got2.damage - ctl2.damage <= 44,
                  "two copies ~double the damage bonus (0.2 * 216 = 43.2)");

            /* --- nail: engineering (core 16) x 20% --- */
            BtState* s3 = &A; fresh(s3, 92);
            int slot3 = bt_weapon_add(s3, (unsigned short)w, 0, 0.0f);
            bt_core_stat_add(s3, 16, 205.0f);    /* item's +5 engineering folded in */
            bt_core_stat_add(s3, 7, -2.0f);      /* item's -2 ranged damage */
            BtPlayerWeaponStats ctl3;
            (void)bt_weapon_stats(s3, slot3, &ctl3);

            BtState* s4 = &B; fresh(s4, 92);
            int slot4 = bt_weapon_add(s4, (unsigned short)w, 0, 0.0f);
            bt_core_stat_add(s4, 16, 200.0f);
            bt_apply_item(s4, BT_ITEM_IDX_NAIL);
            BtPlayerWeaponStats got4;
            (void)bt_weapon_stats(s4, slot4, &got4);

            check_near(bt_core_stat_read(s4, 16), 205.0f, 0.0f,
                       "nail adds its +5 engineering");
            check_near(bt_core_stat_read(s4, 7), -2.0f, 0.0f,
                       "nail adds its -2 ranged damage");
            float n0 = 0.0f, n1 = 0.0f;
            for (int i = 0; i < ctl3.n_scaling; i++) n0 += ctl3.sc_ratio[i];
            for (int i = 0; i < got4.n_scaling; i++) n1 += got4.sc_ratio[i];
            check_near(n1 - n0, 0.20f, 1e-4f,
                       "the weapon's effective scaling gains the item's 20% row");
            int delta4 = got4.damage - ctl3.damage;
            check(delta4 >= 40 && delta4 <= 41,
                  "weapon damage rises by ~20% of engineering (41.0)");
        }
    }

    /* ================================================================ */
    /* 17. Item-related weapon bonuses (run_data.gd:1085-1166
     *     update_item_related_effects).  focus/spider/jelly register
     *     unique_weapon_effects [stat, value] pairs and excalibur registers an
     *     additional_weapon_effects pair; each is multiplied by the
     *     distinct-family / owned-weapon count and applied to the PLAYER stat
     *     layer, fully re-derived on every acquisition and inventory mutation.
     *     The observable is a player stat compared against a control with an
     *     IDENTICAL loadout, so any weapon-set bonus cancels out. */
    {
        int w_add = -1;
        for (int q = 0; q < BT_NWEAPONS && w_add < 0; q++) {
            BT_CONST_PTR(BtWeaponDef) D = &BT_WEAPONS[q];
            for (int k = 0; k < (int)D->fx_count; k++) {
                int idx = (int)D->fx_first + k;
                if (idx >= BT_NWEAPON_EFF_INDEX) break;
                int row = (int)BT_WEAPON_EFF_INDEX[idx];
                if (row < 0 || row >= BT_NITEM_EFF_ROWS) continue;
                BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[row];
                if (E->storage == 1 &&
                    E->custom_key == (unsigned short)BT_ISTR_ADDITIONAL_WEAPON_EFFECTS) { w_add = q; break; }
            }
        }
        check(w_add >= 0, "a weapon carries the additional_weapon_effects row (excalibur)");

        int wc = -1, wl = -1, wo = -1;
        for (int q = 0; q < BT_NWEAPONS; q++) {
            if (!wpn_test_usable(q)) continue;
            if (wc < 0 && (int)BT_WEAPONS[q].tier == 0) wc = q;
            if (wl < 0 && (int)BT_WEAPONS[q].tier >= 3) wl = q;
        }
        if (wc >= 0 && wl >= 0 && wpn_sets_overlap(wc, wl)) wl = -1;
        if (wc >= 0 && wl < 0)
            for (int q = 0; q < BT_NWEAPONS; q++)
                if (wpn_test_usable(q) && (int)BT_WEAPONS[q].tier >= 3 && !wpn_sets_overlap(q, wc)) { wl = q; break; }
        if (w_add >= 0)
            for (int q = 0; q < BT_NWEAPONS; q++)
                if (wpn_test_usable(q) && q != w_add && !wpn_sets_overlap(q, w_add)) { wo = q; break; }
        check(wc >= 0 && wl >= 0, "a bucket-free common + legendary weapon pair exists");
        check(wo >= 0, "a bucket-free weapon disjoint from excalibur exists");

        /* --- focus: -3 attack_speed (core 15) per DISTINCT family --- */
        if (wc >= 0 && wl >= 0) {
            BtState* sw = &A; BtState* so = &B; float base;
            fresh(sw, 101); fresh(so, 101);
            bt_weapon_add(sw, (unsigned short)wc, 0, 0.0f);
            bt_weapon_add(so, (unsigned short)wc, 0, 0.0f);
            base = bt_core_stat_read(so, 15);
            bt_apply_item(sw, BT_ITEM_IDX_FOCUS);
            check_near(bt_core_stat_read(sw, 15), base - 3.0f, 0.0f,
                       "focus: -3 attack_speed at 1 unique family");

            fresh(sw, 101); fresh(so, 101);
            bt_weapon_add(sw, (unsigned short)wc, 0, 0.0f); bt_weapon_add(sw, (unsigned short)wc, 0, 0.0f);
            bt_weapon_add(so, (unsigned short)wc, 0, 0.0f); bt_weapon_add(so, (unsigned short)wc, 0, 0.0f);
            base = bt_core_stat_read(so, 15);
            bt_apply_item(sw, BT_ITEM_IDX_FOCUS);
            check_near(bt_core_stat_read(sw, 15), base - 3.0f, 0.0f,
                       "focus: a duplicate family does not raise the count");

            fresh(sw, 101); fresh(so, 101);
            bt_weapon_add(sw, (unsigned short)wc, 0, 0.0f); bt_weapon_add(sw, (unsigned short)wl, 0, 0.0f);
            bt_weapon_add(so, (unsigned short)wc, 0, 0.0f); bt_weapon_add(so, (unsigned short)wl, 0, 0.0f);
            base = bt_core_stat_read(so, 15);
            bt_apply_item(sw, BT_ITEM_IDX_FOCUS);
            check_near(bt_core_stat_read(sw, 15), base - 6.0f, 0.0f,
                       "focus: -6 attack_speed at 2 unique families");
            check_eq_int(bt_iwb_count(sw, BT_IWB_UNIQUE), 2, "unique count is 2");
            /* transition on removal: the dropped family re-derives the bonus */
            bt_weapon_remove(sw, 1);
            check_near(bt_core_stat_read(sw, 15), base - 3.0f, 0.0f,
                       "focus: removing a family re-derives to -3");
            check_eq_int(bt_iwb_count(sw, BT_IWB_UNIQUE), 1, "unique count back to 1");
        }

        /* --- spider: +6 attack_speed per distinct family --- */
        if (wc >= 0) {
            BtState* sw = &B; BtState* so = &A;
            fresh(sw, 102); fresh(so, 102);
            bt_weapon_add(sw, (unsigned short)wc, 0, 0.0f);
            bt_weapon_add(so, (unsigned short)wc, 0, 0.0f);
            float base = bt_core_stat_read(so, 15);
            bt_apply_item(sw, BT_ITEM_IDX_SPIDER);
            check_near(bt_core_stat_read(sw, 15), base + 6.0f, 0.0f,
                       "spider: +6 attack_speed at 1 unique family");
        }

        /* --- jelly: +1 max_hp (core 0) per family per copy, stacking --- */
        if (wc >= 0 && wl >= 0) {
            BtState* sw = &A; BtState* so = &B;
            fresh(sw, 103); fresh(so, 103);
            bt_weapon_add(sw, (unsigned short)wc, 0, 0.0f);
            bt_weapon_add(so, (unsigned short)wc, 0, 0.0f);
            float base = bt_core_stat_read(so, 0);
            bt_apply_item(sw, BT_ITEM_IDX_JELLY);
            check_near(bt_core_stat_read(sw, 0), base + 1.0f, 0.0f,
                       "jelly: +1 max_hp at 1 family x 1 copy");
            bt_apply_item(sw, BT_ITEM_IDX_JELLY);
            check_near(bt_core_stat_read(sw, 0), base + 2.0f, 0.0f,
                       "jelly: a second copy stacks to +2");
            bt_weapon_add(sw, (unsigned short)wl, 0, 0.0f);
            bt_weapon_add(so, (unsigned short)wl, 0, 0.0f);
            base = bt_core_stat_read(so, 0);
            check_near(bt_core_stat_read(sw, 0), base + 4.0f, 0.0f,
                       "jelly: 2 copies x 2 families -> +4 max_hp");
        }

        /* --- excalibur: additional_weapon_effects, -3 armor (core 2) per owned
         *     weapon, registered at add and dropped at remove --- */
        if (w_add >= 0 && wo >= 0) {
            BtState* s = &A;
            fresh(s, 104);
            float ar0 = bt_core_stat_read(s, 2);
            bt_weapon_add(s, (unsigned short)w_add, 0, 0.0f);
            check_near(bt_core_stat_read(s, 2), ar0 - 3.0f, 0.0f,
                       "excalibur alone: additional -3 armor for 1 owned weapon");
            check_eq_int(bt_iwb_count(s, BT_IWB_ADDITIONAL), 1, "additional count is 1");
            bt_weapon_add(s, (unsigned short)wo, 0, 0.0f);
            check_near(bt_core_stat_read(s, 2), ar0 - 6.0f, 0.0f,
                       "a 2nd weapon raises the additional count to 2 (-6 armor)");
            check_eq_int(bt_iwb_count(s, BT_IWB_ADDITIONAL), 2, "additional count is 2");
            bt_weapon_remove(s, 1);
            check_near(bt_core_stat_read(s, 2), ar0 - 3.0f, 0.0f,
                       "removing the 2nd weapon re-derives to -3 armor");
            bt_weapon_remove(s, 0);
            check_near(bt_core_stat_read(s, 2), ar0, 0.0f,
                       "removing excalibur unregisters its row -> 0 armor");
        }

        /* --- tier_i / tier_iv: no shipped producer (King character only); the
         *     source runner is exercised directly, like the cooldown helpers --- */
        if (wc >= 0 && wl >= 0) {
            BtState* s = &B;
            fresh(s, 105);
            float ar0 = bt_core_stat_read(s, 2);
            bt_iwb_add(s, BT_IWB_TIER_I, (unsigned short)BT_ISTR_STAT_ARMOR, 5);
            bt_iwb_add(s, BT_IWB_TIER_IV, (unsigned short)BT_ISTR_STAT_ARMOR, -7);
            bt_item_wbonus_refresh(s);
            check_near(bt_core_stat_read(s, 2), ar0, 0.0f,
                       "tier buckets grant nothing with no weapon");
            bt_weapon_add(s, (unsigned short)wc, 0, 0.0f);   /* tier 0 = COMMON */
            check_near(bt_core_stat_read(s, 2), ar0 + 5.0f, 0.0f,
                       "tier_i x1 common weapon -> +5 armor");
            check_eq_int(bt_iwb_count(s, BT_IWB_TIER_I), 1, "tier_i count 1");
            bt_weapon_add(s, (unsigned short)wl, 0, 0.0f);   /* tier 3 = LEGENDARY */
            check_near(bt_core_stat_read(s, 2), ar0 + 5.0f - 7.0f, 0.0f,
                       "tier_iv x1 legendary weapon -> its -7 armor also applies");
            check_eq_int(bt_iwb_count(s, BT_IWB_TIER_IV), 1, "tier_iv count 1");
        }

        /* --- boundary: the declared-pair table is bounded; the (MAX+1)th pair
         *     is an explicit overflow (stat_reg_overflow), never a silent drop,
         *     and the applied bonus stays consistent with the kept pairs --- */
        if (wc >= 0) {
            BtState* s = &B;
            fresh(s, 106);
            bt_weapon_add(s, (unsigned short)wc, 0, 0.0f);
            for (int i = 0; i < BT_IWB_MAX; i++) bt_apply_item(s, BT_ITEM_IDX_FOCUS);
            check_eq_int((int)s->n_iwb, BT_IWB_MAX, "the declared-pair table fills to its bound");
            check(s->stat_reg_overflow == 0, "no overflow at the bound");
            bt_apply_item(s, BT_ITEM_IDX_FOCUS);   /* the (BT_IWB_MAX + 1)th */
            check(s->stat_reg_overflow == 1, "the next pair is an explicit overflow");
            check_eq_int((int)s->n_iwb, BT_IWB_MAX, "the table stays at its bound (fail-closed)");
        }
    }

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
