/* tests/test_brotato_weapon_stats.cpp
 *
 * Source-backed tests for the effective player weapon stats layer
 * (ocean/brotato/brotato_weapon_stats.h, owner: WeaponStats):
 *   weapon_service.gd:35-63   init_melee_stats / init_ranged_stats (MIN_RANGE, stat_range)
 *   weapon_service.gd:88-132  _set_common_ranged_stats
 *   weapon_service.gd:134-290 init_base_stats (ordered modifier chain)
 *   weapon_service.gd:296-333 init_burning_data
 *   weapon_service.gd:489-495 apply_scaling_stats_to_damage
 *   weapon_service.gd:558-600 min/max weapon cooldown effects
 *   weapon_service.gd:568-571 apply_attack_speed_mod_to_cooldown (MIN_COOLDOWN = 2)
 *   dlc_1_data.gd:386-402     _boost_weapon_stats_damage
 *   effects/items/class_bonus_effect.gd, effects/weapons/weapon_stack_effect.gd
 *   weapon_service.gd:650-677 init_stats_every_x_projectiles (variants)
 *
 * Build and run (integrated; needs brotato_weapons.h present):
 *     c++ -O2 -o build/test_brotato_weapon_stats tests/test_brotato_weapon_stats.cpp -lm
 *     ./build/test_brotato_weapon_stats
 *
 * Exit code 0 = all checks passed.
 */

#include "../ocean/brotato/brotato_core.h"
#include "../ocean/brotato/brotato_weapon_stats.h"
#include "../ocean/brotato/brotato_weapon_set_runtime.h"

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
static int min_(int a, int b) { return a < b ? a : b; }

/* BtState embeds two physics worlds; keep the big state static. */
static BtState A;

static int bt_ws_is_melee_row(int i) { return BT_WEAPONS[i].kind == BT_WEAPON_KIND_MELEE; }
static int bt_ws_is_ranged_row(int i) { return BT_WEAPONS[i].kind == BT_WEAPON_KIND_RANGED; }

/* first weapon of `kind` that actually resolves (valid stats row) */
static int find_weapon(int ranged) {
    for (int i = 0; i < BT_NWEAPONS; i++) {
        if (ranged ? !bt_ws_is_ranged_row(i) : !bt_ws_is_melee_row(i)) continue;
        if ((int)BT_WEAPONS[i].stats >= BT_NWEAPON_STATS) continue;
        return i;
    }
    return -1;
}

/* first weapon whose top-level effect rows contain `opcode` */
static int find_weapon_with_fx(int opcode) {
    for (int i = 0; i < BT_NWEAPONS; i++) {
        for (int e = 0; e < (int)BT_WEAPONS[i].fx_count; e++) {
            int idx = (int)BT_WEAPONS[i].fx_first + e;
            if (idx >= BT_NWEAPON_EFF_INDEX) break;
            int row = (int)BT_WEAPON_EFF_INDEX[idx];
            if (row < 0 || row >= BT_NITEM_EFF_ROWS) continue;
            if (BT_ITEM_EFF_ROWS[row].opcode == opcode) return i;
        }
    }
    return -1;
}

static void put_slot(int slot, int wid, int cursed, float cmod, float cd) {
    A.wpn_id[slot] = (unsigned short)wid;
    A.wpn_cursed[slot] = (unsigned char)cursed;
    A.wpn_curse[slot] = cmod;
    A.wpn_cd[slot] = cd;
    if (slot + 1 > (int)A.wpn_count) A.wpn_count = (unsigned char)(slot + 1);
}

int main(void) {
    bt_core_init(&A, 900u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
    printf("[0] weapon pool sanity\n");
    {
        check(BT_NWEAPONS > 0, "BT_NWEAPONS > 0");
        check(find_weapon(0) >= 0, "a melee weapon exists");
        check(find_weapon(1) >= 0, "a ranged weapon exists");
        check(bt_ws_field_of(BT_ISTR_WSTAT_DAMAGE) == BT_WSTAT_F_DAMAGE,
              "BT_ISTR_WSTAT_DAMAGE interned and mapped");
    }

    printf("[1] empty / out-of-range slot\n");
    {
        BtPlayerWeaponStats o;
        put_slot(0, BT_WEAPON_NONE, 0, 0.0f, 0.0f);
        A.wpn_count = 0;
        check(bt_weapon_stats(&A, 0, &o) == 0, "empty wpn_count -> 0");
        check(o.damage == 0 && o.cooldown == 0, "out zeroed on empty");
        check(bt_weapon_stats(&A, -1, &o) == 0, "slot -1 -> 0");
        check(bt_weapon_stats(&A, BT_SLOTS, &o) == 0, "slot BT_SLOTS -> 0");
    }

    printf("[2] melee basics + MIN_RANGE clamp\n");
    {
        int wid = find_weapon(0);
        BtPlayerWeaponStats o;
        put_slot(0, wid, 0, 0.0f, 7.5f);
        check(bt_weapon_stats(&A, 0, &o) == 1, "live melee slot -> 1");
        check(o.kind == BT_WEAPON_KIND_MELEE, "kind = MELEE");
        /* init_melee_stats/:35-63 keeps MIN_RANGE as a LOCAL floor for
         * max_range; new_stats.min_range itself is NOT rewritten. */
        check(o.min_range == BT_WEAPON_STATS[BT_WEAPONS[wid].stats].min_range,
              "min_range is not rewritten by the resolve");
        check(o.max_range >= BT_WEAPON_MIN_RANGE, "max_range floors at MIN_RANGE");
        check(o.cooldown >= BT_WEAPON_MIN_COOLDOWN, "cooldown >= MIN_COOLDOWN");
        check(o.damage >= 1, "damage >= 1");
        check_near(o.cooldown_left, 7.5f, 0.0f, "cooldown_left = wpn_cd[slot]");
    }

    printf("[3] ranged: class, player crit, accuracy, stat_range\n");
    {
        int wid = find_weapon(1);
        BtPlayerWeaponStats o, o2;
        put_slot(0, wid, 0, 0.0f, 0.0f);
        bt_weapon_stats(&A, 0, &o);
        check(o.kind == BT_WEAPON_KIND_RANGED, "kind = RANGED");
        check(o.nb_projectiles >= 1, "nb_projectiles >= 1");
        /* weapon_service.gd:251-253: + capped stat_crit_chance */
        bt_core_stat_add(&A, 9, 20.0f);
        bt_core_stat_add(&A, 14, 50.0f);   /* accuracy effect key */
        bt_weapon_stats(&A, 0, &o2);
        check_near(o2.crit_chance, o.crit_chance + 0.20f, 1e-4f, "crit += stat_crit_chance/100");
        check(o2.accuracy > o.accuracy, "accuracy effect raises accuracy");
        check_near(o2.max_range, o.max_range, 0.0f, "accuracy does not move range");
        /* stat_range adds to a ranged weapon's max_range */
        bt_core_stat_add(&A, 18, 40.0f);
        bt_weapon_stats(&A, 0, &o2);
        check_near(o2.max_range, o.max_range + 40.0f, 1e-4f, "ranged max_range += stat_range");
        /* melee adds stat_range/2 (init_melee_stats) */
        int mwid = find_weapon(0);
        put_slot(1, mwid, 0, 0.0f, 0.0f);
        BtPlayerWeaponStats m1, m2;
        bt_core_init(&A, 900u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        put_slot(1, mwid, 0, 0.0f, 0.0f);
        bt_weapon_stats(&A, 1, &m1);
        bt_core_stat_add(&A, 18, 40.0f);
        bt_weapon_stats(&A, 1, &m2);
        check_near(m2.max_range, m1.max_range + 20.0f, 1e-4f, "melee max_range += stat_range/2");
    }

    printf("[4] attack speed lowers cooldown; MIN_COOLDOWN floor\n");
    {
        bt_core_init(&A, 900u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        int wid = find_weapon(1);
        put_slot(0, wid, 0, 0.0f, 0.0f);
        BtPlayerWeaponStats o;
        bt_weapon_stats(&A, 0, &o);
        int raw = BT_WEAPON_STATS[BT_WEAPONS[wid].stats].cooldown;
        if (raw < BT_WEAPON_MIN_COOLDOWN) raw = BT_WEAPON_MIN_COOLDOWN;
        check_near((float)o.cooldown, (float)bt_apply_atk_spd_mod_cd(raw, 0.0f), 0.0f,
                   "cooldown = attack-speed-mod(raw) with no attack speed");
        bt_core_stat_add(&A, 15, 100.0f);   /* +100% attack speed */
        bt_weapon_stats(&A, 0, &o);
        check_near((float)o.cooldown, (float)bt_apply_atk_spd_mod_cd(raw, 1.0f), 0.0f,
                   "cooldown = raw/(1+atk) with +100% attack speed");
        check(o.cooldown >= BT_WEAPON_MIN_COOLDOWN, "cooldown never below MIN_COOLDOWN");
    }

    printf("[5] damage scaling from the weapon's scaling stats\n");
    {
        bt_core_init(&A, 900u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        int wid = find_weapon(0);
        put_slot(0, wid, 0, 0.0f, 0.0f);
        BtPlayerWeaponStats o1, o2;
        bt_weapon_stats(&A, 0, &o1);
        bt_core_stat_add(&A, 6, 100.0f);   /* +100 melee damage (weapon_service.gd:489-495) */
        bt_weapon_stats(&A, 0, &o2);
        check(o2.damage > o1.damage, "melee damage stat raises weapon damage");
    }

    printf("[6] class set bonus applies only for the weapon's set\n");
    {
        bt_core_init(&A, 900u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        int wid = find_weapon(0);
        if (BT_WEAPONS[wid].set_count == 0) {
            printf("  (skip: weapon has no sets)\n");
        } else {
            unsigned short setrow = BT_WEAPON_SET_IDS[BT_WEAPONS[wid].set_first];
            unsigned short set_id = BT_WEAPON_SETS[setrow].my_id;
            put_slot(0, wid, 0, 0.0f, 0.0f);
            BtPlayerWeaponStats o1, o2;
            bt_weapon_stats(&A, 0, &o1);
            bt_wstats_class_bonus_add(&A, set_id, BT_ISTR_WSTAT_DAMAGE, 50);
            bt_weapon_stats(&A, 0, &o2);
            check(o2.damage > o1.damage, "class bonus on the weapon's set raises damage");
            /* a bonus for an unrelated set must not apply */
            put_slot(1, find_weapon(1), 0, 0.0f, 0.0f);
            BtPlayerWeaponStats r1;
            bt_weapon_stats(&A, 1, &r1);
            bt_wstats_class_bonus_add(&A, (unsigned short)(set_id + 12345u), BT_ISTR_WSTAT_DAMAGE, 50);
            BtPlayerWeaponStats r2;
            bt_weapon_stats(&A, 1, &r2);
            check_near((float)r2.damage, (float)r1.damage, 0.0f, "unrelated set bonus ignored");
            /* reversibility: removing the entry restores the value */
            bt_wstats_class_bonus_remove(&A, set_id, BT_ISTR_WSTAT_DAMAGE, 50);
            BtPlayerWeaponStats o3;
            bt_weapon_stats(&A, 0, &o3);
            check_near((float)o3.damage, (float)o1.damage, 0.0f, "class bonus removal restores damage");
        }
    }

    printf("[7] weapon_type_bonus is class-scoped\n");
    {
        bt_core_init(&A, 900u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        int mwid = find_weapon(0), rwid = find_weapon(1);
        put_slot(0, mwid, 0, 0.0f, 0.0f);
        put_slot(1, rwid, 0, 0.0f, 0.0f);
        BtPlayerWeaponStats m1, r1;
        bt_weapon_stats(&A, 0, &m1);
        bt_weapon_stats(&A, 1, &r1);
        bt_wstats_type_bonus_add(&A, BT_WEAPON_KIND_MELEE, BT_ISTR_WSTAT_DAMAGE, 30);
        BtPlayerWeaponStats m2, r2;
        bt_weapon_stats(&A, 0, &m2);
        bt_weapon_stats(&A, 1, &r2);
        check(m2.damage > m1.damage, "melee-scoped bonus raises melee damage");
        check_near((float)r2.damage, (float)r1.damage, 0.0f, "melee-scoped bonus ignores ranged weapon");
        bt_wstats_type_bonus_remove(&A, BT_WEAPON_KIND_MELEE, BT_ISTR_WSTAT_DAMAGE, 30);
        BtPlayerWeaponStats m3;
        bt_weapon_stats(&A, 0, &m3);
        check_near((float)m3.damage, (float)m1.damage, 0.0f, "type bonus removal restores damage");
    }

    printf("[8] cursed transform (dlc_1_data.gd:386-402)\n");
    {
        BtItemWeaponStats base;
        base.kind = 2; base.damage = 10; base.crit_damage = 1.5f; base.lifesteal = 0.05f;
        base.piercing = 2; base.bounce = 3;
        BtItemWeaponStats c;
        bt_weapon_curse_stats(&base, 0.5f, &c);
        check(c.damage == (int)bt_ceilf(10.0f * 1.5f), "cursed damage = ceil(d*(1+mod))");
        check_near(c.crit_damage, bt_ws_stepify(1.5f * (1.0f + 0.5f / 5.0f), 0.1f), 1e-5f,
                   "cursed crit_damage stepify(*(1+mod/5),0.1)");
        check_near(c.lifesteal, bt_ws_stepify(0.05f * 1.5f, 0.01f), 1e-5f,
                   "cursed lifesteal stepify(*(1+mod),0.01)");
        check(c.piercing == min_((int)(base.piercing + 1),
                                 (int)bt_ceilf(2.0f * (1.0f + 0.5f / 5.0f))),
              "cursed piercing = min(p+1, ceil(p*(1+mod/5)))");
        check(c.bounce == min_((int)(base.bounce + 1),
                               (int)bt_ceilf(3.0f * (1.0f + 0.5f / 5.0f))),
              "cursed bounce = min(b+1, ceil(b*(1+mod/5)))");
        /* zero modifier is the identity */
        BtItemWeaponStats z;
        bt_weapon_curse_stats(&base, 0.0f, &z);
        check(z.damage == base.damage && z.piercing == base.piercing,
              "mod 0 transform is the identity");
        /* a cursed slot never resolves weaker than the uncursed base damage */
        bt_core_init(&A, 900u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        int wid = find_weapon(0);
        BtPlayerWeaponStats u, cu;
        put_slot(0, wid, 0, 0.0f, 0.0f);
        bt_weapon_stats(&A, 0, &u);
        put_slot(0, wid, 1, 0.5f, 0.0f);
        A.wpn_wmod[0] = 0.5f;   /* the weapon-stats draw (bt_weapon_stats_curse_modifier) */
        bt_weapon_stats(&A, 0, &cu);
        check(cu.damage >= u.damage, "cursed slot damage >= uncursed");
        check(cu.cursed == 1 && cu.curse_mod > 0.0f, "cursed/mod reported");
    }

    printf("[9] weapon_stack_effect (stick)\n");
    {
        int wid = find_weapon_with_fx(BT_ITEMEFF_WEAPON_STACK);
        if (wid < 0) {
            printf("  (skip: no shipped WeaponStackEffect row)\n");
        } else {
            bt_core_init(&A, 900u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            put_slot(0, wid, 0, 0.0f, 0.0f);
            BtPlayerWeaponStats one, two;
            bt_weapon_stats(&A, 0, &one);
            put_slot(1, wid, 0, 0.0f, 0.0f);
            bt_weapon_stats(&A, 0, &two);
            /* stacking counts the shared weapon_id family, so a second copy adds
             * value * max(0, n-1) to the stat named by the effect */
            check(two.damage > one.damage, "stack effect raises damage with a 2nd copy");
        }
    }

    printf("[10] min/max weapon cooldown buckets (weapon_service.gd:558-600)\n");
    {
        bt_core_init(&A, 900u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        int wid = find_weapon(1);
        put_slot(0, wid, 0, 0.0f, 0.0f);
        BtPlayerWeaponStats o1, o2;
        bt_weapon_stats(&A, 0, &o1);
        bt_wstats_min_cd_add(&A, o1.cooldown + 120);
        bt_weapon_stats(&A, 0, &o2);
        check(o2.cooldown > o1.cooldown, "minimum_weapon_cooldowns raises cooldown");
        bt_wstats_min_cd_remove(&A, o1.cooldown + 120);
        BtPlayerWeaponStats o3;
        bt_weapon_stats(&A, 0, &o3);
        check_near((float)o3.cooldown, (float)o1.cooldown, 0.0f, "minimum removal restores cooldown");
        if (o1.cooldown > 30) {
            bt_wstats_max_cd_add(&A, o1.cooldown - 10);
            BtPlayerWeaponStats o4;
            bt_weapon_stats(&A, 0, &o4);
            check(o4.cooldown < o1.cooldown, "maximum_weapon_cooldowns lowers cooldown");
            bt_wstats_max_cd_remove(&A, o1.cooldown - 10);
        } else {
            printf("  (skip: weapon cooldown too small for the max test)\n");
        }
    }

    printf("[11] burning data (init_burning_data)\n");
    {
        int wb = find_weapon_with_fx(BT_ITEMEFF_BURNING);
        int wn = -1;
        for (int i = 0; i < BT_NWEAPONS; i++) {
            int has = 0;
            for (int e = 0; e < (int)BT_WEAPONS[i].fx_count; e++) {
                int row = (int)BT_WEAPON_EFF_INDEX[BT_WEAPONS[i].fx_first + e];
                if (BT_ITEM_EFF_ROWS[row].opcode == BT_ITEMEFF_BURNING) has = 1;
            }
            if (!has) { wn = i; break; }
        }
        bt_core_init(&A, 900u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        if (wb < 0) {
            printf("  (skip: no shipped weapon BurningEffect)\n");
        } else {
            put_slot(0, wb, 0, 0.0f, 0.0f);
            BtPlayerWeaponStats n1, n2;
            bt_weapon_stats(&A, 0, &n1);
            check(n1.is_burning == 1, "weapon's own BurningEffect makes it burn");
            check(n1.burn_chance > 0.0f && n1.burn_dur > 0, "base burn chance/duration carried");
            /* a base-burning weapon only MERGES the global burn DAMAGE (:320) */
            if ((int)BT_ISTR_BURN_CHANCE < BT_NITEM_STRS) A.scalar_effect[BT_ISTR_BURN_CHANCE] += 5.0f;
            A.burn_chance += 0.25f;
            bt_weapon_stats(&A, 0, &n2);
            check_near(n2.burn_chance, n1.burn_chance, 0.0f, "base burn keeps its own chance");
            check(n2.burn_dmg >= n1.burn_dmg, "global burn damage merges in");
        }
        /* a weapon WITHOUT base burning adopts the global burn row wholesale */
        if (wn >= 0 && (int)BT_ISTR_BURN_CHANCE < BT_NITEM_STRS) {
            bt_core_init(&A, 900u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            A.burn_chance = 0.3f;
            A.burn_dur = 45.0f;
            A.scalar_effect[BT_ISTR_BURN_CHANCE] = 7.0f;
            put_slot(0, wn, 0, 0.0f, 0.0f);
            BtPlayerWeaponStats g;
            bt_weapon_stats(&A, 0, &g);
            check(g.is_burning == 1, "weapon without base burning adopts the global burn");
            check_near(g.burn_chance, 0.3f, 1e-5f, "global burn chance adopted");
            check(g.burn_dur == 45, "global burn duration adopted");
        }
    }

    printf("[12] per-X-shots variants (init_stats_every_x_projectiles)\n");
    {
        int wid = find_weapon_with_fx(BT_ITEMEFF_WEAPON_EFFECT_WITH_SUB_EFFECTS);
        BtPlayerWeaponStats vs[4];
        int xs[4];
        bt_core_init(&A, 900u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        int n = 0;
        if (wid >= 0) {
            put_slot(0, wid, 0, 0.0f, 0.0f);
            n = bt_weapon_stats_variants(&A, 0, xs, vs, 4);
            check(n >= 1, "a weapon ModifyEveryXProjectile row yields >=1 variant");
            if (n >= 1) check(xs[0] > 0, "variant shot interval > 0");
        } else {
            printf("  (skip: no weapon WeaponEffectWithSubEffects row)\n");
        }
        /* an item-owned ModifyEveryXProjectile (seashell: +3 projectiles) */
        int rwid = find_weapon(1);
        put_slot(1, rwid, 0, 0.0f, 0.0f);
        BtPlayerWeaponStats base;
        bt_weapon_stats(&A, 1, &base);
        bt_wstats_modify_x_add(&A, 5, 3);
        int m = bt_weapon_stats_variants(&A, 1, xs, vs, 4);
        check(m >= 1, "item ModifyEveryXProjectile yields a variant");
        if (m >= 1) {
            check(xs[0] == 5, "item variant interval = 5");
            check_near((float)vs[0].nb_projectiles, (float)(base.nb_projectiles + 3), 0.0f,
                       "item variant +3 projectile count");
            check_near(vs[0].projectile_spread, base.projectile_spread + 3.0f * 0.1f + 3.0f * 0.15f,
                       1e-4f, "item variant spread +0.1*3 (sub effect) +0.15*3 (variant term)");
        }
        bt_wstats_modify_x_remove(&A, 5, 3);
        BtPlayerWeaponStats after;
        bt_weapon_stats(&A, 1, &after);
        check_near((float)after.nb_projectiles, (float)base.nb_projectiles, 0.0f,
                   "variant apply/unapply leaves the live stats unchanged");
    }

    printf("[13] weapon class set runtime (run_data.gd:1064-1072)\n");
    {
        int wid = -1;
        for (int i = 0; i < BT_NWEAPONS; i++)
            if (BT_WEAPONS[i].set_count > 0 && (int)BT_WEAPONS[i].stats < BT_NWEAPON_STATS) { wid = i; break; }
        if (wid < 0) {
            printf("  (skip: no weapon belongs to a set)\n");
        } else {
            int setrow = (int)BT_WEAPON_SET_IDS[BT_WEAPONS[wid].set_first];
            bt_core_init(&A, 900u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            bt_weapon_set_refresh(&A);
            float before[BT_NSTATS];
            for (int i = 0; i < BT_NSTATS; i++) before[i] = A.stat_raw[i];
            put_slot(0, wid, 0, 0.0f, 0.0f);
            put_slot(1, wid, 0, 0.0f, 0.0f);   /* 2 weapons of the set -> tier 0 */
            bt_weapon_set_refresh(&A);
            int changed = 0;
            for (int i = 0; i < BT_NSTATS; i++) if (A.stat_raw[i] != before[i]) changed = 1;
            check(changed, "2 weapons of a set apply the tier-0 set bonus");
            check(bt_weapon_set_tier(&A, setrow) == 0, "tier 0 recorded");
            /* reversibility: drop to 1 weapon and refresh -> bonus removed */
            A.wpn_count = 1;
            A.wpn_id[1] = BT_WEAPON_NONE;
            bt_weapon_set_refresh(&A);
            int restored = 1;
            for (int i = 0; i < BT_NSTATS; i++)
                if (bt_fabsf(A.stat_raw[i] - before[i]) > 1e-5f) restored = 0;
            check(restored, "set bonus is fully reversible on weapon removal");
            check(bt_weapon_set_tier(&A, setrow) == 0xFF, "tier cleared");
        }
    }

    printf("[14] ProjectilesOnHitEffect child (weapon.gd:122-136)\n");
    {
        int wid = find_weapon_with_fx(BT_ITEMEFF_PROJECTILES_ON_HIT);
        BtWeaponOnHit h;
        bt_core_init(&A, 900u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        if (wid < 0) {
            printf("  (skip: no shipped ProjectilesOnHitEffect)\n");
        } else {
            put_slot(0, wid, 0, 0.0f, 0.0f);
            check(bt_weapon_stats_onhit(&A, 0, &h) == 1, "onhit effect reported");
            check(h.count > 0, "onhit count > 0");
            check(h.dmg >= 1.0f, "onhit child damage >= 1");
            check(h.max_range > 0.0f, "onhit child range resolved");
        }
        /* a weapon without the effect returns 0 and a zeroed struct */
        int plain = find_weapon(0);
        put_slot(1, plain, 0, 0.0f, 0.0f);
        check(bt_weapon_stats_onhit(&A, 1, &h) == 0, "no onhit effect -> 0");
        check(h.dmg == 0.0f && h.count == 0, "out zeroed when absent");
    }

    printf("[15] per-effect curse modifier (dlc_1_data.gd:62-67)\n");
    {
        int wid = find_weapon_with_fx(BT_ITEMEFF_BURNING);
        if (wid < 0) {
            printf("  (skip: no shipped weapon BurningEffect)\n");
        } else {
            int ord = -1;
            for (int e = 0; e < (int)BT_WEAPONS[wid].fx_count; e++) {
                int row = (int)BT_WEAPON_EFF_INDEX[BT_WEAPONS[wid].fx_first + e];
                if (BT_ITEM_EFF_ROWS[row].opcode == BT_ITEMEFF_BURNING) { ord = e; break; }
            }
            if (ord < 0 || ord >= BT_WEAPON_FX_CURSE_MAX) {
                printf("  (skip: burning effect not in the stored curse range)\n");
            } else {
                bt_core_init(&A, 900u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
                BtPlayerWeaponStats u, c;
                put_slot(0, wid, 0, 0.0f, 0.0f);
                bt_weapon_stats(&A, 0, &u);
                /* weapon-stats modifier 0 -> the base stats are untouched;
                 * the BurningEffect's own draw doubles its duration and damage. */
                put_slot(0, wid, 1, 0.0f, 0.0f);
                A.wpn_wmod[0] = 0.0f;
                A.wpn_fxcurse[0][ord] = 1.0f;
                bt_weapon_stats(&A, 0, &c);
                check_near(c.burn_dmg, u.burn_dmg * 2.0f, 1e-3f,
                           "burning damage uses the effect's own curse draw");
                check(c.burn_dur == u.burn_dur * 2, "burning duration doubled by its own draw");
                /* a weapon-stats draw must NOT change burning when the effect is uncursed */
                A.wpn_fxcurse[0][ord] = 0.0f;
                A.wpn_wmod[0] = 1.0f;
                BtPlayerWeaponStats w;
                bt_weapon_stats(&A, 0, &w);
                check_near((float)w.burn_dur, (float)u.burn_dur, 1e-5f,
                           "weapon-stats curse does not touch the burning effect");
                check(w.damage > u.damage, "weapon-stats curse boosts damage");
            }
        }
    }

    printf("[16] damage type is_elemental (unit.gd:472-479)\n");
    {
        bt_core_init(&A, 900u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        int pos = -1, neg = -1;
        for (int i = 0; i < BT_NWEAPONS; i++) {
            if ((int)BT_WEAPONS[i].stats >= BT_NWEAPON_STATS) continue;
            BT_CONST_PTR(BtItemWeaponStats) B = &BT_WEAPON_STATS[BT_WEAPONS[i].stats];
            int el = 0;
            for (int k = 0; k < (int)B->scaling_count; k++) {
                if ((int)(B->scaling_first + k) >= BT_NWEAPON_SCALING) break;
                if (bt_core_id_from_istr(BT_WEAPON_SCALING[B->scaling_first + k].stat) == 8) el = 1;
            }
            if (el && pos < 0) pos = i;
            if (!el && neg < 0) neg = i;
        }
        BtPlayerWeaponStats o;
        if (pos >= 0) {
            put_slot(0, pos, 0, 0.0f, 0.0f);
            bt_weapon_stats(&A, 0, &o);
            check(o.is_elemental == 1, "elemental scaling -> is_elemental");
        } else {
            printf("  (skip: no elemental-scaling weapon)\n");
        }
        if (neg >= 0) {
            put_slot(1, neg, 0, 0.0f, 0.0f);
            bt_weapon_stats(&A, 1, &o);
            check(o.is_elemental == 0, "non-elemental scaling -> not is_elemental");
        } else {
            printf("  (skip: no non-elemental-scaling weapon)\n");
        }
    }

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
