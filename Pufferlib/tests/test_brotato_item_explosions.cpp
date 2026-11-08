/* tests/test_brotato_item_explosions.cpp
 *
 * Source-backed regressions for the item/pet/structure explosion lifecycle
 * (ocean/brotato/brotato_item_explosions.h):
 *   - the generated graph wiring (nested ExplodingEffect rows, the explosion
 *     scene's Hitbox circle, explode_scale / explode_chance);
 *   - WeaponService.explode's geometry: position, node scale, radius, the
 *     offset Hitbox circle (player_explosion.gd:65-71);
 *   - the bounded 3-frame lifetime and hit-once-per-target window
 *     (explosion.tscn "explode" 0.05 s, Hitbox.disable at t = 0.05);
 *   - unit.gd:552-578: a friendly projectile whose effects[] carries an
 *     ExplodingEffect explodes AT THE TARGET and deals no direct damage;
 *   - landmine.gd:29-46 trigger: Area2D mask 6, CapsuleShape2D radius 24
 *     height 32 @ (0,16) rot 90, explode on body_exited at the mine's position;
 *   - run_data.gd:1589-1632 handle_explode_effect for explode_on_consumable
 *     (spicy_sauce): summed chance, summed get_explosion_damage, the +-10
 *     random offset of Utils.get_random_offset_position;
 *   - the damage path (unit.gd:257-386 take_damage / get_damage_value): G1
 *     (brotato_enemy_modifiers.h bt_enemy_incoming_damage) on the PRE-crit
 *     value, crit, the above-hp bonus %, then FLAT armor `max(1, dmg - armor)`,
 *     shared with the core's bt_explode;
 *   - the pool's declared capacity with an EXPLICIT overflow failure (no live
 *     explosion is ever overwritten) and the missed-geometry refusal (never an
 *     invented radius).
 *
 * Every expected number is derived here from the generated tables + the
 * recovered sources:
 *   items/all/turret_rocket/turret_rocket_exploding_effect.tres   scale 1.0  chance 1.0
 *   items/all/landmines/landmine_exploding_effect.tres           scale 1.75 chance 1.0
 *   items/all/spicy_sauce/spicy_sauce_effect_1.tres              scale 1.25 chance 0.5
 *   items/all/spicy_sauce/spicy_sauce_stats.tres                 damage 0, scaling stat_max_hp x1.0
 *   projectiles/explosion.tscn                                   CircleShape2D radius 147.34 @ (0,3)
 *   entities/structures/landmine/landmine.tscn                   trigger mask 6, capsule r24 h32 @ (0,16)
 *   projectiles/player_explosion.gd:68-71                        scale = max(0.1, s * (1 + size/100))
 *   singletons/run_data.gd:1589-1632                             handle_explode_effect
 *   singletons/utils.gd:397-403,730-733                           get_chance_success / offset +-10
 *   singletons/weapon_service.gd:281-286,511-531                  get_explosion_damage / explode
 *   entities/units/unit/unit.gd:257-386,295-303                   crit before the % bonus, flat armor
 *   entities/units/unit/take_damage_args.gd                       armor_applied = true for a hitbox hit
 *   characters/bull/boss.gd:52-58 (via brotato_enemy_modifiers.h) pre-crit boss multiplier
 *
 * STATE REQUIREMENT (parent-owned BtState additions; the integration hook):
 *   #define BT_EXPL_FRAMES 3
 *   #define BT_MAX_EXPL (BT_MAX_PROJ + BT_MAX_PET)     -- declared capacity
 *   #define BT_EXPL_HIT_WORDS ((BT_MAX + 31) / 32)
 *   BtExplosion / BtItemExplosions typedefs (see the header's Constants block)
 *   BtItemExplosions expl;
 *   unsigned char pet_lm_pressed[BT_MAX_PET];   (zeroed at summon spawn, moved by bt_pet_compact)
 *   unsigned short pr_expl[BT_MAX_PROJ];        (BT_ITEM_EFF_NONE at bt_spawn_projectile)
 * plus brotato_enemy_modifiers.h being includable (this test includes it
 * explicitly, like the neutral test includes its own header, so it compiles while
 * the core include line is still pending).
 * Until those land this file does not compile -- the same pattern as
 * test_brotato_neutral_events.cpp's `ehit[BT_MAX]`.
 *
 * Build and run:
 *     c++ -O2 -o build/test_brotato_item_explosions tests/test_brotato_item_explosions.cpp -lm
 *     ./build/test_brotato_item_explosions
 *
 * Exit code 0 = all checks passed.
 */

#include "../ocean/brotato/brotato_core.h"
/* G1: bt_enemy_incoming_damage (the pre-crit boss multiplier) -- also included by
 * brotato_core.h in the integrated build; included explicitly here so this test
 * links while the core include line is still pending. */
#include "../ocean/brotato/brotato_enemy_modifiers.h"
/* included by brotato_core.h in the integrated build; included explicitly here so
 * this test compiles while that hook is pending. */
#include "../ocean/brotato/brotato_item_explosions.h"

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
static void check_eq_int(int got, int want, const char* msg) {
    g_checks++;
    if (got != want) { g_failures++; printf("  FAIL: %s (got %d want %d)\n", msg, got, want); }
}

/* BtState is ~1 MiB; keep the big states static. */
static BtState A, B;

/* Row indices in BT_ITEM_EFF_ROWS, resolved by resource path so the test does
 * not hardcode generator indices. */
static int g_row_landmines_item = -1;    /* StructureEffect, scene landmine.tscn */
static int g_row_landmine_expl = -1;     /* its nested ItemExplodingEffect */
static int g_row_rocket_turret = -1;     /* TurretEffect with a nested ExplodingEffect */
static int g_row_rocket_expl = -1;
static int g_row_botomine_pet = -1;      /* BotOMineEffect (pet) */
static int g_row_botomine_lm = -1;       /* its landmine StructureEffect (child) */
static int g_row_spicy = -1;             /* explode_on_consumable ItemExplodingEffect */
static int g_expl_scene = -1;            /* BT_ITEM_SCENES row of res://projectiles/explosion.tscn */

static int find_row_by_text_key_and_opcode(unsigned short text_key, int opcode) {
    for (int r = 0; r < BT_NITEM_EFF_ROWS; r++)
        if ((int)BT_ITEM_EFF_ROWS[r].opcode == opcode && BT_ITEM_EFF_ROWS[r].text_key == text_key)
            return r;
    return -1;
}
static int find_row_by_key_and_opcode(unsigned short key, int opcode) {
    for (int r = 0; r < BT_NITEM_EFF_ROWS; r++)
        if ((int)BT_ITEM_EFF_ROWS[r].opcode == opcode && BT_ITEM_EFF_ROWS[r].key == key)
            return r;
    return -1;
}
/* A structure/turret row that owns a nested ExplodingEffect, identified by the
 * scene of the entity it spawns. */
static int find_row_by_scene(unsigned short scene_istr) {
    for (int r = 0; r < BT_NITEM_EFF_ROWS; r++) {
        BT_CONST_PTR(BtItemEff) E = &BT_ITEM_EFF_ROWS[r];
        if (E->scene != scene_istr) continue;
        if (bt_item_expl_nested_row(E) != BT_ITEM_EFF_NONE) return r;
    }
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
    s->hp = 1.0e9f;                   /* the player must not die mid-test */
    bt_item_expl_reset(s);
}

/* Place one live enemy of `kind` at (x,y) with a large, exactly-representable
 * HP pool (float32 at 1e9 has a 64-unit step, which would swamp the damage
 * diffs these tests read). */
static int place_enemy_kind(BtState* s, int kind, float x, float y) {
    if (!bt_spawn_kind(s, kind, x, y)) return -1;
    for (int i = 0; i < BT_MAX; i++)
        if (s->ealive[i] && !s->eneutral[i] && (int)s->ekind[i] == kind &&
            s->ex[i] == x && s->ey[i] == y) {
            s->ehp[i] = 100000.0f;
            s->emax[i] = 100000.0f;
            return i;
        }
    return -1;
}
static int place_enemy(BtState* s, float x, float y) { return place_enemy_kind(s, 0, x, y); }

/* The BT_KINDS index with the LARGEST armor at wave 1 (so flat subtraction and
 * the multiplicative coefficient can never be confused: armor 3 gives 37 vs 33
 * for a 40-damage hit). */
static int armor_kind(void) {
    int best = -1;
    float best_a = 0.0f;
    for (int i = 0; i < BT_NKINDS; i++) {
        float a = BT_KINDS[i].armor;
        if (a > best_a) { best_a = a; best = i; }
    }
    return best;
}

/* Place one landmine structure slot directly (the values bt_spawn_summon_row
 * would have written for the landmine row). */
static int place_landmine(BtState* s, int row, float x, float y, float dmg) {
    int row_ok = row;
    for (int p = 0; p < BT_MAX_PET; p++) {
        if (s->pet_alive[p]) continue;
        if (p >= s->pet_n) s->pet_n = p + 1;
        s->pet_alive[p] = 1;
        s->pet_isstruct[p] = 1;
        s->pet_opcode[p] = BT_ITEMEFF_STRUCTURE;
        s->pet_row[p] = (unsigned short)row_ok;
        s->pet_x[p] = x; s->pet_y[p] = y;
        s->pet_dmg[p] = dmg;
        s->pet_crit_chance[p] = 0.0f;
        s->pet_crit_dmg[p] = 2.0f;
        s->pet_burn_chance[p] = 0.0f; s->pet_burn_dmg[p] = 0.0f; s->pet_burn_dur[p] = 0.0f;
        s->pet_lm_pressed[p] = 0;
        return p;
    }
    return -1;
}

int main(void) {
    /* ---------------------------------------------------------------- graph */
    printf("[1] generated graph wiring\n");
    {
        int row_landmine_scene = -1;
        for (int r = 0; r < BT_NITEM_EFF_ROWS; r++)
            if (BT_ITEM_EFF_ROWS[r].scene == BT_ISTR_RES___ENTITIES_STRUCTURES_LANDMINE_LANDMINE_TSCN)
                { row_landmine_scene = r; break; }
        check(row_landmine_scene >= 0, "a row instantiates landmine.tscn");

        g_row_landmines_item = row_landmine_scene;
        g_row_botomine_lm = -1;
        for (int r = row_landmine_scene + 1; r < BT_NITEM_EFF_ROWS; r++)
            if (BT_ITEM_EFF_ROWS[r].scene == BT_ISTR_RES___ENTITIES_STRUCTURES_LANDMINE_LANDMINE_TSCN)
                { g_row_botomine_lm = r; break; }
        check(g_row_botomine_lm >= 0, "the BotOMine's landmine_effect_stat row also uses landmine.tscn");

        g_row_landmine_expl = (int)bt_item_expl_nested_row(&BT_ITEM_EFF_ROWS[g_row_landmines_item]);
        check(g_row_landmine_expl >= 0, "the landmines StructureEffect carries a nested ExplodingEffect");
        if (g_row_landmine_expl >= 0) {
            check(BT_ITEM_EFF_ROWS[g_row_landmine_expl].explosion_scene == BT_ISTR_RES___PROJECTILES_EXPLOSION_TSCN,
                  "it uses res://projectiles/explosion.tscn");
            check_near(BT_ITEM_EFF_ROWS[g_row_landmine_expl].explode_scale, 1.75f, 1e-6f,
                       "landmine_exploding_effect.tres scale 1.75");
            check_near(BT_ITEM_EFF_ROWS[g_row_landmine_expl].explode_chance, 1.0f, 1e-6f,
                       "landmine_exploding_effect.tres chance 1.0");
        }
        if (g_row_botomine_lm >= 0)
            check((int)bt_item_expl_nested_row(&BT_ITEM_EFF_ROWS[g_row_botomine_lm]) >= 0,
                  "the BotOMine landmine row carries its own nested ExplodingEffect");

        /* rocket turret: TurretEffect spawning rocket_turret.tscn */
        g_row_rocket_turret = find_row_by_scene(BT_ISTR_RES___ENTITIES_STRUCTURES_TURRET_ROCKET_ROCKET_TURRET_TSCN);
        check(g_row_rocket_turret >= 0, "the rocket turret TurretEffect row exists");
        if (g_row_rocket_turret >= 0) {
            g_row_rocket_expl = (int)bt_item_expl_nested_row(&BT_ITEM_EFF_ROWS[g_row_rocket_turret]);
            check_eq_int((g_row_rocket_expl >= 0) ? (int)BT_ITEM_EFF_ROWS[g_row_rocket_expl].opcode : -1,
                         BT_ITEMEFF_EXPLODING, "its nested row is an ExplodingEffect");
            if (g_row_rocket_expl >= 0) {
                check_near(BT_ITEM_EFF_ROWS[g_row_rocket_expl].explode_scale, 1.0f, 1e-6f,
                           "turret_rocket_exploding_effect.tres scale 1.0");
                check_near(BT_ITEM_EFF_ROWS[g_row_rocket_expl].explode_chance, 1.0f, 1e-6f,
                           "turret_rocket_exploding_effect.tres chance 1.0");
            }
        }

        /* BotOMine pet row: its landmine lives in the single `child` slot, so the
         * pet's own shots must NOT carry an explosion. */
        g_row_botomine_pet = find_row_by_text_key_and_opcode(BT_ISTR_EFFECT_PET_BOT_O_MINE, BT_ITEMEFF_BOT_O_MINE);
        check(g_row_botomine_pet >= 0, "the BotOMineEffect row exists");
        if (g_row_botomine_pet >= 0) {
            check_eq_int((int)bt_item_expl_nested_row(&BT_ITEM_EFF_ROWS[g_row_botomine_pet]),
                         (int)BT_ITEM_EFF_NONE, "the BotOMine pet carries no effects[] ExplodingEffect");
            check_eq_int((int)BT_ITEM_EFF_ROWS[g_row_botomine_pet].child, g_row_botomine_lm,
                         "its landmine StructureEffect is the `child` slot");
        }

        /* spicy_sauce: explode_on_consumable */
        g_row_spicy = find_row_by_key_and_opcode(BT_ISTR_EXPLODE_ON_CONSUMABLE, BT_ITEMEFF_ITEM_EXPLODING);
        check(g_row_spicy >= 0, "the explode_on_consumable ItemExplodingEffect row exists");
        if (g_row_spicy >= 0) {
            check_near(BT_ITEM_EFF_ROWS[g_row_spicy].explode_scale, 1.25f, 1e-6f,
                       "spicy_sauce_effect_1.tres scale 1.25");
            check_near(BT_ITEM_EFF_ROWS[g_row_spicy].explode_chance, 0.5f, 1e-6f,
                       "spicy_sauce_effect_1.tres chance 0.5");
            check(BT_ITEM_EFF_ROWS[g_row_spicy].stats != BT_ITEM_EFF_NONE,
                  "it names the WeaponStats it scales from");
        }

        /* explosion scene geometry: radius 147.34 @ (0,3) */
        g_expl_scene = bt_item_expl_geom(BT_ISTR_RES___PROJECTILES_EXPLOSION_TSCN);
        check(g_expl_scene >= 0, "the explosion scene resolves to a BT_ITEM_SCENES row");
        if (g_expl_scene >= 0) {
            check_near(BT_ITEM_SCENES[g_expl_scene].hit_radius, 147.34f, 1e-4f,
                       "explosion.tscn Hitbox CircleShape2D radius 147.34");
            check_near(BT_ITEM_SCENES[g_expl_scene].hit_x, 0.0f, 1e-6f, "Hitbox x offset 0");
            check_near(BT_ITEM_SCENES[g_expl_scene].hit_y, 3.0f, 1e-6f, "Hitbox y offset 3");
        }
        check_eq_int(bt_item_expl_geom(BT_ISTR_NONE), -1, "no scene -> unresolved");
    }

    /* Every later section drives these rows; without them the generated graph no
     * longer matches the recovered sources. */
    if (g_row_landmine_expl < 0 || g_row_rocket_expl < 0 || g_row_spicy < 0 ||
        g_row_botomine_lm < 0 || g_expl_scene < 0) {
        printf("\n%d checks, %d failures (graph prerequisites missing)\n", g_checks, g_failures);
        return g_failures ? 1 : 0;
    }

    /* ------------------------------------------------------- spawn geometry */
    printf("[2] WeaponService.explode geometry + lifetime\n");
    {
        fresh(&A, 301u);
        int pj = bt_item_expl_spawn(&A, (unsigned short)g_row_rocket_expl, 100.0f, 100.0f, 25.0f,
                                    0.0f, 2.0f, 0.0f, 0.0f, 0.0f);
        check(pj >= 0, "spawn");
        check_near(A.expl.e[pj].rad, 147.34f, 1e-3f, "rocket explosion radius 147.34 (scale 1.0)");
        check_near(A.expl.e[pj].ox, 0.0f, 1e-6f, "scaled x offset");
        check_near(A.expl.e[pj].oy, 3.0f, 1e-6f, "scaled y offset 3");
        check_eq_int(A.expl.e[pj].age, 0, "the Hitbox collision enable is deferred");
        check_eq_int(A.expl.n_spawned, 1, "one explosion created");

        /* +15 explosion_size (explosive_shells/honey) scales the node, so the
         * Hitbox radius and its offset both scale (player_explosion.gd:68-71) */
        A.scalar_effect[BT_ISTR_EXPLOSION_SIZE] = 15.0f;
        int pj2 = bt_item_expl_spawn(&A, (unsigned short)g_row_landmine_expl, -50.0f, -50.0f, 10.0f,
                                     0.0f, 2.0f, 0.0f, 0.0f, 0.0f);
        check(pj2 >= 0, "landmine explosion spawn");
        check_near(A.expl.e[pj2].rad, 147.34f * 1.75f * 1.15f, 1e-2f,
                   "scale = effect.scale * (1 + explosion_size/100)");
        check_near(A.expl.e[pj2].oy, 3.0f * 1.75f * 1.15f, 1e-3f, "the offset scales with the node");

        /* a large negative explosion_size is FLOORED at 0.1x, never a zero or
         * negative radius (player_explosion.gd:70 max(0.1, ...)) */
        A.scalar_effect[BT_ISTR_EXPLOSION_SIZE] = -95.0f;      /* scale 1.0 -> 0.05 -> 0.1 */
        int pj3 = bt_item_expl_spawn(&A, (unsigned short)g_row_rocket_expl, -900.0f, -900.0f, 10.0f,
                                     0.0f, 2.0f, 0.0f, 0.0f, 0.0f);
        check(pj3 >= 0, "floored-scale spawn");
        check_near(A.expl.e[pj3].rad, 147.34f * 0.1f, 1e-3f,
                   "explosion_size is floored at 0.1x on the pooled node");
        A.scalar_effect[BT_ISTR_EXPLOSION_SIZE] = 0.0f;

        /* frame model: no test on the spawn frame, then exactly 3 frames */
        int es = place_enemy(&A, 100.0f, 100.0f);
        check(es >= 0, "enemy at the explosion centre");
        float hp0 = A.ehp[es];
        float rew = 0.0f;
        bt_item_expl_tick(&A, &rew);
        check_near(A.ehp[es], hp0, 0.0f, "spawn frame: the explosion has not been enabled yet");
        bt_item_expl_tick(&A, &rew);            /* hit frame 1 */
        check(A.ehp[es] < hp0, "frame 1: the explosion hits");
        float hp1 = A.ehp[es];
        /* a target that only reaches the radius inside the window is still hit */
        int late = place_enemy(&A, 100.0f, 100.0f + 50.0f);
        check(late >= 0, "late target placed");
        float late_hp0 = A.ehp[late];
        bt_item_expl_tick(&A, &rew);            /* hit frame 2 */
        check_near(A.ehp[es], hp1, 0.0f, "hit-once-per-target: frame 2 does not hit again");
        check(A.ehp[late] < late_hp0, "a target entering inside the window is hit");
        bt_item_expl_tick(&A, &rew);            /* hit frame 3 */
        check_near(A.ehp[es], hp1, 0.0f, "frame 3 does not hit again");
        check(A.expl.e[pj].alive == 1, "still inside its 3-frame window");
        check(A.expl.e[pj2].alive == 1, "an independently spawned explosion keeps its own clock");
        bt_item_expl_tick(&A, &rew);            /* frame 4: the window is over */
        check(A.expl.e[pj].alive == 0, "the explosion expires after BT_EXPL_FRAMES");
        check(A.expl.e[pj2].alive == 0, "and so does its sibling");
        int gone = place_enemy(&A, 100.0f, 100.0f - 50.0f);
        float gone_hp0 = A.ehp[gone];
        bt_item_expl_tick(&A, &rew);
        check_near(A.ehp[gone], gone_hp0, 0.0f, "an expired explosion damages nothing");
    }

    /* -------------------------------------------------------- window/damage */
    printf("[3] window, radius, crit, armor, kill\n");
    {
        fresh(&B, 302u);
        B.scalar_effect[BT_ISTR_EXPLOSION_SIZE] = 0.0f;
        /* the explosion is a POINT: an enemy farther than radius + hurtbox is safe */
        int far_id = place_enemy(&B, 100.0f + 147.34f + 200.0f, 100.0f);
        int near_id = place_enemy(&B, 100.0f + 100.0f, 100.0f);
        check(far_id >= 0 && near_id >= 0, "two enemies placed");
        BT_CONST_PTR(BtBodyPhysics) Gn = bt_kgeom(&B, near_id);
        BT_CONST_PTR(BtBodyPhysics) Gf = bt_kgeom(&B, far_id);

        int pj = bt_item_expl_spawn(&B, (unsigned short)g_row_rocket_expl, 100.0f, 100.0f, 40.0f,
                                    0.0f, 2.0f, 0.0f, 0.0f, 0.0f);
        check(pj >= 0, "spawn");
        float rew = 0.0f;
        bt_item_expl_tick(&B, &rew);
        bt_item_expl_tick(&B, &rew);   /* one hit frame */
        BT_CONST_PTR(BtUnitKind) Kn = bt_kunit(&B, near_id);
        float armor = Kn->armor + Kn->armor_grow * (float)(B.wave - 1);
        float want = bt_roundf(40.0f) - armor;           /* get_damage_value: max(1, dmg - armor) */
        if (want < 1.0f) want = 1.0f;
        check_near(100000.0f - B.ehp[near_id], want, 1e-2f, "damage = max(1, dmg - armor) (flat)");
        check_near(B.ehp[far_id], 100000.0f, 0.0f, "an enemy outside the radius is untouched");
        (void)Gn; (void)Gf;

        /* ARMOR IS FLAT (unit.gd get_damage_value), not the player-facing
         * bt_armor_mult coefficient: a kind with armor 3 takes dmg - 3. */
        int ak = armor_kind();
        check(ak >= 0, "a BT_KINDS row with armor > 0 exists");
        if (ak >= 0) {
            int aid = place_enemy_kind(&B, ak, 1200.0f, 1200.0f);
            check(aid >= 0, "armored enemy placed");
            BT_CONST_PTR(BtUnitKind) Ka = bt_kunit(&B, aid);
            float aarmor = Ka->armor + Ka->armor_grow * (float)(B.wave - 1);
            check(aarmor > 0.0f, "its armor is non-zero at this wave");
            (void)bt_item_expl_spawn(&B, (unsigned short)g_row_rocket_expl, 1200.0f, 1200.0f, 40.0f,
                                     0.0f, 2.0f, 0.0f, 0.0f, 0.0f);
            bt_item_expl_tick(&B, &rew);
            bt_item_expl_tick(&B, &rew);
            float flat = bt_roundf(40.0f) - aarmor;
            float mult = bt_roundf(40.0f * bt_armor_mult(aarmor));   /* the WRONG convention */
            check_near(100000.0f - B.ehp[aid], flat, 1e-2f, "flat armor: 40 - armor");
            check(bt_fabsf((100000.0f - B.ehp[aid]) - flat) <
                  bt_fabsf((100000.0f - B.ehp[aid]) - mult),
                  "and not the bt_armor_mult coefficient");

            /* G1 (brotato_enemy_modifiers.h) lands on the PRE-crit value with the
             * source's int() truncation (boss.gd:52-58 wraps Unit.take_damage):
             * base 3, crit x3, +50% against bosses -> int(4.5) = 4 -> crit 12,
             * NOT int(round(3*3)*1.5) = 13. */
            int boss_id = place_enemy_kind(&B, ak, 1500.0f, 1500.0f);
            check(boss_id >= 0, "boss-class slot placed");
            B.eelite[boss_id] = 2;                        /* the class G1 reads */
            B.scalar_effect[BT_ISTR_DAMAGE_AGAINST_BOSSES] = 50.0f;
            (void)bt_item_expl_spawn(&B, (unsigned short)g_row_rocket_expl, 1500.0f, 1500.0f, 3.0f,
                                     1.0f, 3.0f, 0.0f, 0.0f, 0.0f);
            bt_item_expl_tick(&B, &rew);
            bt_item_expl_tick(&B, &rew);
            float pre = bt_enemy_incoming_damage(&B, boss_id, 3.0f);
            check_near(pre, 4.0f, 1e-6f, "G1 truncates pre-crit: int(3 * 1.5) = 4");
            float want_pre = bt_roundf(pre * 3.0f) - aarmor;
            float want_post = bt_roundf(3.0f * 3.0f) * 1.5f - aarmor;
            check_near(100000.0f - B.ehp[boss_id], want_pre, 1e-2f,
                       "the boss multiplier lands on the pre-crit value");
            check(bt_fabsf((100000.0f - B.ehp[boss_id]) - want_pre) <
                  bt_fabsf((100000.0f - B.ehp[boss_id]) - want_post),
                  "and not on the post-crit value");
            B.eelite[boss_id] = 0;
            B.scalar_effect[BT_ISTR_DAMAGE_AGAINST_BOSSES] = 0.0f;
        }

        /* crit is rolled per target; the crit multiply precedes the % bonus and
         * armor (unit.gd:295-297) */
        int crit_id = place_enemy(&B, 400.0f, 400.0f);
        int pjc = bt_item_expl_spawn(&B, (unsigned short)g_row_rocket_expl, 400.0f, 400.0f, 40.0f,
                                     1.0f, 2.5f, 0.0f, 0.0f, 0.0f);
        check(pjc >= 0, "crit explosion spawn");
        bt_item_expl_tick(&B, &rew);
        bt_item_expl_tick(&B, &rew);
        BT_CONST_PTR(BtUnitKind) Kc = bt_kunit(&B, crit_id);
        float carmor = Kc->armor + Kc->armor_grow * (float)(B.wave - 1);
        float cwant = bt_roundf(bt_roundf(40.0f * 2.5f)) - carmor;
        if (cwant < 1.0f) cwant = 1.0f;
        check_near(100000.0f - B.ehp[crit_id], cwant, 1e-2f, "crit = round(dmg * crit_damage) before armor");

        /* burn carried by the explosion hitbox (unit.gd:583-585) */
        int burn_id = place_enemy(&B, 700.0f, 700.0f);
        (void)bt_item_expl_spawn(&B, (unsigned short)g_row_rocket_expl, 700.0f, 700.0f, 5.0f,
                                 0.0f, 2.0f, 1.0f, 7.0f, 3.0f);
        bt_item_expl_tick(&B, &rew);
        bt_item_expl_tick(&B, &rew);
        check(B.eburn[burn_id] > 0.0f, "the explosion's burning_data burned the target");
        check_near(B.eburndmg[burn_id], 7.0f, 1e-6f, "burn damage carried through");

        /* a kill credits R_KILL through bt_credit_kill */
        int dead_id = place_enemy(&B, 900.0f, 900.0f);
        B.ehp[dead_id] = 3.0f;
        B.emax[dead_id] = 3.0f;
        float r0 = rew;
        (void)bt_item_expl_spawn(&B, (unsigned short)g_row_rocket_expl, 900.0f, 900.0f, 50.0f,
                                 0.0f, 2.0f, 0.0f, 0.0f, 0.0f);
        bt_item_expl_tick(&B, &rew);
        bt_item_expl_tick(&B, &rew);
        check(B.ealive[dead_id] == 0, "a lethal explosion hit removes the target");
        check_near(rew - r0, R_KILL, 1e-6f, "the kill is rewarded once");
    }

    /* ----------------------------------------------------- pool robustness */
    printf("[4] declared capacity: explicit overflow refusal\n");
    {
        fresh(&A, 303u);
        int placed = 0, refused = 0;
        for (int i = 0; i < BT_MAX_EXPL + 5; i++) {
            int pj = bt_item_expl_spawn(&A, (unsigned short)g_row_rocket_expl, (float)i, 0.0f, 10.0f,
                                        0.0f, 2.0f, 0.0f, 0.0f, 0.0f);
            if (pj >= 0) placed++; else refused++;
        }
        /* BT_MAX_EXPL is DECOUPLED from BT_MAX_PROJ + BT_MAX_PET (Main's
         * directive): the source pools one explosion node per spawning scene,
         * not one per live projectile.  Evidence for the 256 bound: an
         * every-item deep endless probe (tests/brotato_state_size.cpp DEEP)
         * peaks at 122 live explosions at BT_MAX=100 and 132 at BT_MAX=192
         * against a 2048/4096-slot probe pool, i.e. ~2x headroom.  No assertion
         * on the constant itself: the behaviour below (exact fill, loud refusal
         * past the bound, no live overwrite, identity-preserving reuse) is the
         * contract, so this test never has to be repinned when the bound moves. */
        check_eq_int(placed, BT_MAX_EXPL, "every slot inside the capacity is placed");
        check_eq_int(refused, 5, "every spawn past the capacity FAILS (returns -1)");
        check_eq_int((int)A.expl.n_overflow, 5, "and is counted (never a silent drop)");
        int alive = 0;
        for (int i = 0; i < BT_MAX_EXPL; i++) alive += A.expl.e[i].alive ? 1 : 0;
        check_eq_int(alive, BT_MAX_EXPL, "no LIVE explosion was ever overwritten");
        check_eq_int((int)A.expl.n_spawned, BT_MAX_EXPL, "spawn counter is exact");
        /* the first slot still carries the first spawn's payload: nothing
         * overwrote it while the pool was full */
        check_near(A.expl.e[0].x, 0.0f, 1e-6f, "slot 0 kept its own explosion");

        /* a row without a resolvable explosion scene is refused, not invented */
        int miss = bt_item_expl_spawn(&A, (unsigned short)g_row_botomine_pet, 0.0f, 0.0f, 10.0f,
                                      0.0f, 2.0f, 0.0f, 0.0f, 0.0f);
        check_eq_int(miss, -1, "an unresolved explosion scene is refused");
        check_eq_int((int)A.expl.n_missing, 1, "and counted");
        check_eq_int(bt_item_expl_spawn(&A, (unsigned short)BT_NITEM_EFF_ROWS, 0.0f, 0.0f, 1.0f,
                                        0.0f, 1.0f, 0.0f, 0.0f, 0.0f), -1,
                     "an out-of-range row is refused");
        check_eq_int((int)A.expl.n_missing, 2, "both refusals are counted, no spawn happened");
        check_eq_int((int)A.expl.n_spawned, BT_MAX_EXPL, "and the spawn counter did not move");
    }

    /* the pool is scene-keyed and resident: a returned node is reused LIFO for
     * its OWN scene, and only a FRESH node owes the first-frame node sound
     * (player_explosion.gd:28-32 + main.gd:1649-1699) */
    printf("[4b] resident scene node: reuse is LIFO and silent\n");
    {
        fresh(&A, 305u);
        int q0 = A.birth_audio_count;
        float rew2 = 0.0f;
        int s1 = bt_item_expl_spawn(&A, (unsigned short)g_row_rocket_expl, 10.0f, 0.0f, 10.0f,
                                    0.0f, 1.0f, 0.0f, 0.0f, 0.0f);
        check(s1 >= 0, "first spawn");
        check(A.expl.cache_res[s1] == 1 && A.expl.cache_row[s1] == (unsigned char)g_expl_scene,
              "the node is resident under its own scene row");
        check(A.expl.snd_pending[s1] == 1, "a fresh node owes its first-frame sound");
        bt_item_expl_tick(&A, &rew2);            /* spawn frame (age 0 -> 1) */
        bt_item_expl_tick(&A, &rew2);            /* first physics frame */
        check(A.expl.snd_pending[s1] == 0, "the node sound is owed only once");
        check(A.birth_audio_count == q0 + 1, "the fresh node sounded exactly once");

        /* expire it, then spawn the same scene again: the SAME resident node */
        for (int t = 0; t < BT_EXPL_FRAMES + 1; t++) bt_item_expl_tick(&A, &rew2);
        int q1 = A.birth_audio_count;
        int s2 = bt_item_expl_spawn(&A, (unsigned short)g_row_rocket_expl, 20.0f, 0.0f, 10.0f,
                                    0.0f, 1.0f, 0.0f, 0.0f, 0.0f);
        check_eq_int(s2, s1, "the returned node is reused (LIFO, same scene)");
        check(A.expl.snd_pending[s2] == 0, "a reused node does NOT owe a sound");
        bt_item_expl_tick(&A, &rew2);            /* spawn frame */
        bt_item_expl_tick(&A, &rew2);            /* first physics frame */
        check(A.birth_audio_count == q1, "a reused node submits nothing");
    }

    /* --------------------------------------------------------- landmines */
    printf("[5] landmine trigger (landmine.gd:29-46)\n");
    {
        fresh(&A, 304u);
        A.px = 100.0f; A.py = 100.0f;                  /* the player, far from the mine */
        int p = place_landmine(&A, g_row_landmines_item, 800.0f, 800.0f, 12.0f);
        check(p >= 0, "landmine placed");
        check_eq_int(bt_item_expl_landmine_row(&A, p), g_row_landmine_expl,
                     "the landmine row resolves its nested ExplodingEffect");
        check_eq_int(bt_item_expl_landmine_tick(&A), 0, "no body inside: the mine waits");
        check(A.pet_alive[p] == 1, "an untriggered mine persists");
        check_eq_int(A.expl.n_spawned, 0, "and spawned nothing");

        /* an enemy walking over the capsule arms the mine ... */
        BT_CONST_PTR(BtBodyPhysics) G = bt_kgeom(&A, 0);
        float cx = 800.0f, cy = 800.0f;
        /* put the enemy's BODY circle centre on the capsule's segment end */
        float bcx = cx + BT_LM_TRIG_HALF, bcy = cy + BT_LM_TRIG_Y;
        int e = place_enemy(&A, bcx - G->body_x, bcy - G->body_y);
        check(e >= 0, "enemy on the trigger");
        check_eq_int(bt_item_expl_landmine_tick(&A), 0, "body inside: the mine only arms");
        check(A.pet_lm_pressed[p] == 1, "the body_entered flag is recorded");
        check(A.pet_alive[p] == 1, "still armed, not yet triggered");

        /* ... and explodes when that body leaves */
        A.ex[e] = (cx + BT_LM_TRIG_HALF + BT_LM_TRIG_R + G->body_radius + 10.0f) - G->body_x;
        check_eq_int(bt_item_expl_landmine_tick(&A), 1, "body_exited fires the explosion");
        check(A.pet_alive[p] == 0, "the mine dies (landmine.gd:45 die())");
        check_eq_int((int)A.expl.n_spawned, 1, "one explosion, at the mine's position");
        int slot = -1;
        for (int i = 0; i < BT_MAX_EXPL; i++) if (A.expl.e[i].alive) slot = i;
        check(slot >= 0, "explosion live");
        check_near(A.expl.e[slot].x, cx, 1e-6f, "position = the landmine's own position");
        check_near(A.expl.e[slot].y, cy, 1e-6f, "position = the landmine's own position");
        check_near(A.expl.e[slot].rad, 147.34f * 1.75f, 1e-2f, "radius from landmine scale 1.75");
        check_near(A.expl.e[slot].dmg, 12.0f, 1e-6f, "damage = the structure's init-time damage");

        /* capsule geometry: 24 radius + 16 half-length along local X */
        check(bt_item_expl_lm_hits(cx, cy, cx + BT_LM_TRIG_HALF + BT_LM_TRIG_R, cy + BT_LM_TRIG_Y, 0.0f),
              "on the segment end (24 + 16 along x)");
        check(!bt_item_expl_lm_hits(cx, cy, cx + BT_LM_TRIG_HALF + BT_LM_TRIG_R + 1.0f, cy + BT_LM_TRIG_Y, 0.0f),
              "one unit past it is outside");
        check(bt_item_expl_lm_hits(cx, cy, cx, cy + BT_LM_TRIG_Y + BT_LM_TRIG_R, 0.0f),
              "24 to the side is inside");
        check(!bt_item_expl_lm_hits(cx, cy, cx, cy + BT_LM_TRIG_Y + BT_LM_TRIG_R + 1.0f, 0.0f),
              "25 to the side is outside");

        /* the player body (layer 2, mask 6) also arms the mine */
        fresh(&A, 305u);
        int p2 = place_landmine(&A, g_row_botomine_lm, 1000.0f, 1000.0f, 9.0f);
        check(p2 >= 0, "a BotOMine landmine is the same structure");
        A.px = 1000.0f; A.py = 1000.0f + BT_LM_TRIG_Y - BT_PLAYER_BODY_Y;
        check_eq_int(bt_item_expl_landmine_tick(&A), 0, "the player's body arms it");
        check(A.pet_lm_pressed[p2] == 1, "player armed the mine");
        A.px = 3000.0f; A.py = 3000.0f;
        check_eq_int(bt_item_expl_landmine_tick(&A), 1, "the player leaving triggers it");
        check(A.pet_alive[p2] == 0, "and the mine dies");

        /* a neutral tree is layer 1, so the mask-6 Area2D ignores it */
        fresh(&A, 306u);
        A.px = 3000.0f; A.py = 3000.0f;
        int p3 = place_landmine(&A, g_row_landmines_item, 1500.0f, 1500.0f, 5.0f);
        int nk = -1;
        for (int i = 0; i < BT_NNEUTRALS; i++) nk = i;   /* any neutral kind */
        check(nk >= 0, "a neutral kind exists");
        (void)bt_spawn_neutral(&A, nk, 1500.0f, 1500.0f);
        check_eq_int(bt_item_expl_landmine_tick(&A), 0, "a tree never arms a landmine");
        check(A.pet_lm_pressed[p3] == 0, "no body_entered from the neutral layer");
    }

    /* --------------------------------------- projectile-carried explosion */
    printf("[6] unit.gd:552-578 friendly-projectile explosion\n");
    {
        fresh(&A, 307u);
        /* the turret's shots carry its nested ExplodingEffect; other pets do not */
        int p = place_landmine(&A, g_row_rocket_turret, 0.0f, 0.0f, 0.0f);
        check(p >= 0, "rocket turret slot");
        A.pet_opcode[p] = BT_ITEMEFF_TURRET;
        check_eq_int((int)bt_item_expl_shot_effect(&A, p), g_row_rocket_expl,
                     "a TurretEffect hands its effects[] to its projectiles (turret.gd:115)");
        A.pet_opcode[p] = BT_ITEMEFF_BOT_O_MINE;
        A.pet_row[p] = (unsigned short)g_row_botomine_pet;
        check_eq_int((int)bt_item_expl_shot_effect(&A, p), (int)BT_ITEM_EFF_NONE,
                     "the BotOMine's own shots carry no explosion");
        A.pet_opcode[p] = BT_ITEMEFF_STRUCTURE;
        A.pet_row[p] = (unsigned short)g_row_landmines_item;
        check_eq_int((int)bt_item_expl_shot_effect(&A, p), (int)BT_ITEM_EFF_NONE,
                     "a landmine structure is not a shooter");

        int e = place_enemy(&A, 600.0f, 600.0f);
        check(e >= 0, "enemy placed");
        int pj = bt_spawn_projectile(&A, 0.0f, 0.0f, 0.0f, 2000.0f, 33.0f, BT_SHAPE_CIRCLE,
                                     17.0f, 0.0f, 0.0f, 0.0f, 1);
        check(pj >= 0, "friendly projectile spawned");
        check_eq_int((int)A.pr_expl[pj], (int)BT_ITEM_EFF_NONE, "pr_expl initialised to NONE");
        check_eq_int(bt_item_expl_projectile_hit(&A, pj, e), 0,
                     "no carried explosion: the caller keeps the direct hit");
        A.pr_expl[pj] = (unsigned short)g_row_rocket_expl;
        A.prcrit_dmg[pj] = 2.0f;
        check_eq_int(bt_item_expl_projectile_hit(&A, pj, e), 1,
                     "a carried ExplodingEffect replaces the direct hit (is_exploding)");
        int slot = -1;
        for (int i = 0; i < BT_MAX_EXPL; i++) if (A.expl.e[i].alive) slot = i;
        check(slot >= 0, "explosion spawned");
        check_near(A.expl.e[slot].x, A.ex[e], 1e-6f, "at the TARGET's position, not the shot's");
        check_near(A.expl.e[slot].y, A.ey[e], 1e-6f, "at the TARGET's position, not the shot's");
        check_near(A.expl.e[slot].dmg, 33.0f, 1e-6f, "the projectile's own damage rides the explosion");
        float hp0 = A.ehp[e];
        float rew = 0.0f;
        bt_item_expl_tick(&A, &rew);
        check_near(A.ehp[e], hp0, 0.0f, "the target took no direct damage on the impact frame");
        bt_item_expl_tick(&A, &rew);
        check(A.ehp[e] < hp0, "the explosion damages the target on the next frame");
    }

    /* -------------------------------------------- explode_on_consumable */
    printf("[7] handle_explode_effect: explode_on_consumable (spicy_sauce)\n");
    {
        fresh(&B, 308u);
        B.stat_raw[0] = 100.0f;           /* stat_max_hp = the scaling stat */
        bt_core_stat_sync(&B);
        B.stat_raw[5] = 0.0f;             /* stat_percent_damage */
        B.scalar_effect[BT_ISTR_EXPLOSION_DAMAGE] = 0.0f;
        B.scalar_effect[BT_ISTR_EXPLOSION_SIZE] = 0.0f;
        bt_core_stat_sync(&B);
        B.n_bk_active = 0;
        B.bk_count[g_row_spicy] = 0;
        B.nvar[g_row_spicy] = 0;

        /* nothing owned -> no explosion */
        check_eq_int(bt_item_expl_on_consumable(&B, 500.0f, 500.0f), -1, "no owned copy, no explosion");

        /* one owned copy: chance 0.5, damage = round(max(1, 0 + max_hp) * 1.0) */
        bt_scalar_bucket_register(&B, g_row_spicy, 1, 0);
        check_eq_int((int)B.bk_count[g_row_spicy], 1, "the copy registered in the effect bucket");
        check_eq_int((int)B.nvar[g_row_spicy], 1, "with its own variant");
        /* force the 0.5 roll: two copies sum to chance 1.0 (spicy_sauce max_nb = 2) */
        bt_scalar_bucket_register(&B, g_row_spicy, 1, 0);
        int pj = bt_item_expl_on_consumable(&B, 500.0f, 500.0f);
        check(pj >= 0, "two copies: chance 0.5 + 0.5 rolls once and succeeds");
        check_near(B.expl.e[pj].rad, 147.34f * 1.25f, 1e-2f, "spicy_sauce scale 1.25");
        check_near(B.expl.e[pj].dmg, 200.0f, 1e-3f, "damage = 2 copies x round(max_hp * 1.0)");
        check(bt_fabsf(B.expl.e[pj].x - 500.0f) <= 10.0f, "position within +-10 of the consumable");
        check(bt_fabsf(B.expl.e[pj].y - 500.0f) <= 10.0f, "position within +-10 of the consumable");
        check_near(B.expl.e[pj].crit_chance, bt_core_stat_capped(&B, 9) * 0.01f, 1e-6f,
                   "crit_chance = stats.crit_chance + the capped player crit stat");
        check_near(B.expl.e[pj].crit_dmg, 1.5f, 1e-6f, "spicy_sauce_stats crit_damage 1.5");

        /* percent damage + explosion damage fold into the multiplier
         * (weapon_service.gd:281-286) */
        B.stat_raw[5] = 50.0f;
        bt_core_stat_sync(&B);
        B.scalar_effect[BT_ISTR_EXPLOSION_DAMAGE] = 20.0f;
        int pj2 = bt_item_expl_on_consumable(&B, 100.0f, 100.0f);
        check(pj2 >= 0, "second consumable explosion");
        check_near(B.expl.e[pj2].dmg, bt_roundf(200.0f * (1.0f + 0.5f + 0.2f)), 1e-3f,
                   "round(damage * (1 + percent_damage/100 + explosion_damage/100))");

        /* a cursed copy scales its scaling stats (_boost_weapon_stats_damage) */
        B.stat_raw[5] = 0.0f;
        B.scalar_effect[BT_ISTR_EXPLOSION_DAMAGE] = 0.0f;
        bt_core_stat_sync(&B);
        B.n_bk_active = 0;
        B.bk_count[g_row_spicy] = 0;
        B.nvar[g_row_spicy] = 0;
        bt_scalar_bucket_register(&B, g_row_spicy, 1, 0);
        bt_scalar_bucket_register(&B, g_row_spicy, 1, 50);   /* +50% curse modifier */
        int pj3 = bt_item_expl_on_consumable(&B, 0.0f, 0.0f);
        check(pj3 >= 0, "cursed copy pair fires");
        check_near(B.expl.e[pj3].dmg, 100.0f + 150.0f, 1e-3f,
                   "per copy: the cursed copy's ratio * (1 + mod) is 50% higher");
        /* the args come from effect[0] -- the first applied copy -- so the cursed
         * copy does not change crit_damage (run_data.gd:1606-1618) */
        check_near(B.expl.e[pj3].crit_dmg, 1.5f, 1e-6f, "critical args come from effect[0]");
    }

    /* ---------------------------------------------------------- chance roll */
    printf("[8] Utils.get_chance_success (utils.gd:397-403)\n");
    {
        fresh(&A, 309u);
        check_eq_int(bt_item_expl_chance(&A, 0.0f), 0, "a 0 chance never succeeds");
        check_eq_int(bt_item_expl_chance(&A, 1.0f), 1, "a 1.0 chance always succeeds");
        int hits = 0;
        for (int i = 0; i < 400; i++) hits += bt_item_expl_chance(&A, 0.5f);
        check(hits > 120 && hits < 280, "a 0.5 chance succeeds about half the time");
        /* one owned spicy_sauce copy can also miss: the bucket path returns -1 */
        fresh(&A, 310u);
        A.stat_raw[0] = 100.0f;
        bt_core_stat_sync(&A);
        bt_scalar_bucket_register(&A, g_row_spicy, 1, 0);
        int fired = 0, missed = 0;
        for (int i = 0; i < 400; i++) {
            int pj = bt_item_expl_on_consumable(&A, 10.0f, 10.0f);
            if (pj >= 0) { fired++; A.expl.e[pj].alive = 0; } else missed++;
        }
        check(fired > 120 && fired < 280, "one 0.5 copy fires about half the pickups");
        check(missed > 120 && missed < 280, "and misses about half");
    }

    printf("[9] curse base-damage boost in get_explosion_damage (dlc_1_data.gd:379-383)\n");
    {
        /* a real explosion stats row whose scaling stats are all 0 on a fresh
         * Bull, so the difference isolates the BASE-damage boost. */
        fresh(&A, 311u);
        int wrow = -1;
        for (int w = 0; w < BT_NITEM_WSTATS && wrow < 0; w++) {
            BT_CONST_PTR(BtItemWeaponStats) W = &BT_ITEM_WSTATS[w];
            if (W->damage <= 0) continue;
            int ok = 1;
            for (int i = 0; i < (int)W->scaling_count; i++) {
                int id = bt_core_id_from_istr(BT_ITEM_SCALING[W->scaling_first + i].stat);
                if (id != 255 && bt_core_stat_read(&A, id) != 0.0f) { ok = 0; break; }
            }
            if (ok) wrow = w;
        }
        check(wrow >= 0, "an explosion stats row with base damage and 0-valued scaling exists");
        if (wrow >= 0) {
            int d = BT_ITEM_WSTATS[wrow].damage;
            int plain = bt_item_expl_wstats_damage(&A, (unsigned short)wrow, 0.0f);
            int c50 = bt_item_expl_wstats_damage(&A, (unsigned short)wrow, 0.5f);
            check_eq_int(plain, d, "no curse: get_explosion_damage == the stats base damage");
            check_eq_int(c50, (int)bt_ceilf((float)d * 1.5f),
                         "curse +50%: base damage ceil(d * (1 + mod)) is boosted too");
        }
    }

    printf("[10] herding reward: one node pays N*R_KILL + R_HERD*N*(N-1)\n");
    {
        /* a single explosion that catches N enemies (brotato.h reward design) */
        fresh(&B, 312u);
        int e0 = place_enemy(&B, 500.0f, 500.0f);
        int e1 = place_enemy(&B, 510.0f, 500.0f);
        int e2 = place_enemy(&B, 520.0f, 500.0f);
        check(e0 >= 0 && e1 >= 0 && e2 >= 0, "three enemies placed");
        (void)e0; (void)e1; (void)e2;
        (void)bt_item_expl_spawn(&B, (unsigned short)g_row_rocket_expl, 500.0f, 500.0f, 1.0e9f,
                                 0.0f, 2.0f, 0.0f, 0.0f, 0.0f);
        float rew = 0.0f;
        (void)bt_item_expl_tick(&B, &rew);          /* spawn frame (deferred) */
        int kills = bt_item_expl_tick(&B, &rew);    /* hit frame */
        check_eq_int(kills, 3, "one explosion node killed all three");
        check_near(rew, 3.0f * R_KILL + R_HERD * 6.0f, 1e-4f,
                   "reward = 3*R_KILL + R_HERD*3*2 (clustering pays)");

        /* N == 1 must be byte-identical to the old flat R_KILL */
        fresh(&B, 313u);
        int e3 = place_enemy(&B, 600.0f, 600.0f);
        check(e3 >= 0, "one enemy placed");
        (void)e3;
        (void)bt_item_expl_spawn(&B, (unsigned short)g_row_rocket_expl, 600.0f, 600.0f, 1.0e9f,
                                 0.0f, 2.0f, 0.0f, 0.0f, 0.0f);
        float rew1 = 0.0f;
        (void)bt_item_expl_tick(&B, &rew1);
        int k1 = bt_item_expl_tick(&B, &rew1);
        check_eq_int(k1, 1, "one kill");
        check_near(rew1, R_KILL, 1e-6f, "N=1 pays exactly R_KILL");
    }

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
