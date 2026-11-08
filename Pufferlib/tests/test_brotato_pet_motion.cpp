/* tests/test_brotato_pet_motion.cpp
 *
 * Deterministic regression tests for the pet motion + interaction-zone slice
 * (ocean/brotato/brotato_pet_motion.h, BT_PET_AI in brotato_item_effects.h).
 * Self-contained: includes only the core header and <stdio.h>.
 *
 * Build and run:
 *     c++ -O2 -o build/test_brotato_pet_motion tests/test_brotato_pet_motion.cpp -lm
 *     ./build/test_brotato_pet_motion
 *
 * Exit code 0 = all checks passed.
 *
 * Coverage (real behaviour, not table wiring): DocMoth patrol geometry; Lootworm
 * follow-target motion, its own gold attract/pickup where its own double_chance
 * is followed by the player's common gold-pickup effects (chance_double_gold /
 * heal_when_pickup_gold), the player's closer-gold rule, and its tree-only
 * melee; Scapegoat hit intake, 0.6 s hurtbox cooldown, death, and the
 * self-revive gated on a player inside
 * HealingTriggeringZone; BotOMine stopping distance_to_target from the player;
 * BonkDog's "first enemy when none is in range" fallback; CatlingGun mad mode;
 * DocMoth BoostZone entry/exit with its hp_regen_bonus on the core's regen tick
 * and the curse-scaled zone radius.
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

/* BtState is ~1 MiB; keep the big states static so they never sit on the stack. */
static BtState A, B;

static void step1(BtState* s, int action, float* obs, float* rew, float* term) {
    bt_core_step(s, action, obs, rew, term);
}

static void spawn_settled_gold(BtState* s, float x, float y, int value) {
    int slot = 0;
    while (slot < BT_MAX_MAT && s->matalive[slot]) slot++;
    bt_spawn_material(s, x, y, value);
    if (slot == BT_MAX_MAT) return;
    bt_loot_item_place(s->mat_arc[slot], x, y);
    s->mat_arc[slot][BT_LOOT_ARC_PUSH] = 0.0f;
    s->mat_arc[slot][BT_LOOT_ARC_IDLE] = 0.0f;
    s->mat_arc[slot][BT_LOOT_ARC_MON] = 1.0f;
}

static int name_has(const char* hay, const char* needle) {
    if (!hay) return 0;
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j] && hay[i + j] == needle[j]) j++;
        if (!needle[j]) return 1;
    }
    return 0;
}
static int kind_by_substr(const char* needle) {
    for (int i = 0; i < BT_NKINDS; i++)
        if (name_has(BT_KIND_NAME[i], needle)) return i;
    return -1;
}
/* Item row by its host-side id (generated BT_ITEMS[].name, no "item_" prefix). */
static int item_by_name(const char* needle) {
    for (int i = 0; i < BT_NITEMS; i++)
        if (name_has(BT_ITEMS[i].name, needle)) return i;
    return -1;
}
/* First enemy kind that owns a contact Hitbox and is not an elite/boss. */
static int plain_enemy_kind(void) {
    for (int i = 0; i < BT_NKINDS; i++) {
        if (bt_kind_class(i) != 0) continue;
        if (BT_PHYSICS[bt_geom_index(i)].has_hitbox) return i;
    }
    return -1;
}
/* Tree kind: BT_NEUTRALS is its own domain (never a BT_KINDS enemy). */
static int tree_kind(void) {
    for (int i = 0; i < BT_NNEUTRALS; i++)
        if (name_has(BT_NEUTRAL_NAME[i], "tree")) return i;
    return -1;
}
/* The effect-table row that instantiates a pet class. */
static int eff_row(int opcode) {
    for (int r = 0; r < BT_NITEM_EFF_ROWS; r++)
        if ((int)BT_ITEM_EFF_ROWS[r].opcode == opcode) return r;
    return -1;
}
/* Locate a live slot by predicate-free scan. */
static int find_enemy_slot(BtState* s, int kind, int neutral) {
    for (int i = 0; i < BT_MAX; i++)
        if (s->ealive[i] && (int)s->ekind[i] == kind &&
            (int)s->eneutral[i] == (neutral ? 1 : 0)) return i;
    return -1;
}

int main(void) {
    float obs[BT_OBS_SIZE], rew, term;

    printf("[1] DocMoth patrol: circles the map centre at radius_factor * half-height\n");
    {
        bt_core_init(&A, 21u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        int row = eff_row(BT_ITEMEFF_DOC_MOTH);
        check(row >= 0, "doc_moth effect row exists");
        check_near(BT_ARENA_H * 0.5f * 0.8f, 614.4f, 0.1f, "patrol radius = 768 * 0.8");
        int p = bt_spawn_summon_row(&A, (unsigned short)row,
                                    BT_ARENA_W * 0.5f + 614.4f, BT_ARENA_H * 0.5f, 0, 0, 0, 0);
        check(p == 0 && A.pet_alive[0], "doc_moth entity spawned");
        check_near(A.pet_pradius[0], 614.4f, 1e-2f, "patrol radius initialised from the table");
        check(A.pet_tx[0] == 0.0f && A.pet_ty[0] == 0.0f,
              "patrol target starts at the source's un-set Vector2.ZERO");
        float x0 = A.pet_x[0], y0 = A.pet_y[0];
        /* patrol_around_the_map's own _physics_process runs AFTER the pet's
         * unit._physics_process (Godot 3: parent node before its
         * MovementBehavior child), so frame 1 moves toward the un-set ZERO
         * target (-x, -y from a spawn at centre+(radius,0)), not the circle. */
        step1(&A, 0, obs, &rew, &term);
        check(A.pet_x[0] < x0 && A.pet_y[0] < y0,
              "frame 1 moves toward the un-set ZERO target (target lags one frame)");
        check_near(bt_v2dist(bt_v2(A.pet_tx[0], A.pet_ty[0]),
                             bt_v2(BT_ARENA_W * 0.5f, BT_ARENA_H * 0.5f)),
                   614.4f, 1e-2f, "the advanced target lies on the patrol circle");
        for (int t = 0; t < 59; t++) step1(&A, 0, obs, &rew, &term);
        float dx = A.pet_x[0] - x0, dy = A.pet_y[0] - y0;
        check(bt_sqrtf(dx * dx + dy * dy) > 100.0f, "DocMoth actually moves (patrol target advances)");
        /* _angle += (speed / radius) * delta -> 2.5 px of arc per frame at 150 speed */
        check(bt_fabsf(bt_atan2f(A.pet_y[0] - BT_ARENA_H * 0.5f, A.pet_x[0] - BT_ARENA_W * 0.5f)
                       - bt_atan2f(y0 - BT_ARENA_H * 0.5f, x0 - BT_ARENA_W * 0.5f)) > 0.1f,
              "patrol angle advanced");
    }

    printf("[2] Lootworm: follow-target motion, own gold attract/pickup, tree-only melee\n");
    {
        /* 2a: no tree and no gold -> no target -> the pet does not move. */
        bt_core_init(&A, 31u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        A.px = 200.0f; A.py = 200.0f;                 /* player away from the pet */
        int row = eff_row(BT_ITEMEFF_LOOTWORM);
        int p = bt_spawn_summon_row(&A, (unsigned short)row, 1000.0f, 700.0f, 0, 0.0f, 0.0f, 0.0f);
        check(p == 0, "lootworm spawned");
        float sx = A.pet_x[0], sy = A.pet_y[0];
        for (int t = 0; t < 30; t++) step1(&A, 0, obs, &rew, &term);
        check_near(A.pet_x[0], sx, 1e-4f, "lootworm with no target stands still");
        check_near(A.pet_y[0], sy, 1e-4f, "lootworm with no target stands still (y)");
        check_near(A.pet_speed[0], 250.0f, 1e-3f, "pet current speed = Stats.speed * speed factor");

        /* 2b: a gold outside the attract area is chased (follow_target motion). */
        spawn_settled_gold(&A, 1250.0f, 700.0f, 5);
        for (int t = 0; t < 30; t++) step1(&A, 0, obs, &rew, &term);
        check(A.pet_x[0] > sx + 60.0f, "lootworm chases the nearest gold");

        /* 2c: inside the ItemAttractArea the pet claims it (mat_by = slot+1). */
        A.matx[0] = A.pet_x[0] + 80.0f;
        A.maty[0] = A.pet_y[0];
        A.matspd[0] = BT_ATTRACT_SPEED;
        step1(&A, 0, obs, &rew, &term);
        check(A.matalive[0] && A.mat_by[0] == 1,
              "lootworm claims a gold inside its 100 px ItemAttractArea");

        /* 2d: pickup doubles with double_chance and credits materials + XP. */
        bt_core_init(&B, 31u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        B.n_live_groups = 0;
        B.px = 200.0f; B.py = 200.0f;
        float mat0 = B.materials, xp0 = B.xp;
        int pb = bt_spawn_summon_row(&B, (unsigned short)row, 1000.0f, 700.0f, 0, 0.0f, 1.0f, 0.0f);
        check(pb == 0 && B.pet_double[0] == 1.0f, "lootworm double_chance in force from its instance");
        spawn_settled_gold(&B, 1040.0f, 700.0f, 5);
        for (int t = 0; t < 60 && B.matcount > 0; t++) step1(&B, 0, obs, &rew, &term);
        check(B.matcount == 0, "lootworm picks the gold up in its own ItemPickUpArea");
        check_near(B.materials - mat0, 10.0f, 1e-3f, "double_chance 1.0 -> gold value x2");
        check(B.xp - xp0 >= 10.0f, "pet pickup grants the player the same 1:1 XP");

        /* 2e: the player takes a gold back when it is closer than the pet. */
        bt_core_init(&A, 32u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        A.px = 200.0f; A.py = 200.0f;                 /* player far: the pet claims first */
        bt_spawn_summon_row(&A, (unsigned short)row, 1000.0f, 700.0f, 0, 0.0f, 0.0f, 0.0f);
        spawn_settled_gold(&A, 1090.0f, 700.0f, 5);   /* 90 px: attract only, no pickup */
        step1(&A, 0, obs, &rew, &term);
        check(A.mat_by[0] == 1, "pet claims the gold while no player is near");
        A.px = 1040.0f; A.py = 700.0f;                /* 50 px from the gold, 90 px from the pet */
        step1(&A, 0, obs, &rew, &term);
        check(A.mat_by[0] == 0, "closer player takes the gold back from the pet attractor");
        for (int t = 0; t < 3; t++) step1(&A, 0, obs, &rew, &term);
        check(A.mat_by[0] == 0, "the pet does not re-claim a gold the player is closer to");

        /* 2e2: the pet never takes a gold the player ALREADY attracts, even when
         * the pet is closer (lootworm.gd:59-65: item.attracted_by != null). */
        bt_core_init(&A, 35u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        A.px = 200.0f; A.py = 200.0f;
        bt_spawn_summon_row(&A, (unsigned short)row, 1000.0f, 700.0f, 0, 0.0f, 0.0f, 0.0f);
        spawn_settled_gold(&A, 1090.0f, 700.0f, 5);   /* 90 px pet, far from the player */
        A.mat_attr[0] = 1;                            /* the player already attracts it */
        step1(&A, 0, obs, &rew, &term);
        check(A.mat_by[0] == 0 && A.mat_attr[0] == 1,
              "pet does not take a gold the player already attracts");

        /* 2e3: the player's ItemAttractArea is centred on the player ROOT, not
         * the (0,-24) pickup offset (player.tscn ItemAttractArea/CollisionShape2D
         * stores no position).  A gold exactly 150 px BELOW the root is inside
         * the 150 px magnet circle and is attracted (attracted_by = player); the
         * ItemPickupArea circle @ (0,-24) is 174 px away, so it is not picked up. */
        bt_core_init(&A, 36u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        A.px = 1000.0f; A.py = 768.0f;
        spawn_settled_gold(&A, 1000.0f, 768.0f + 150.0f, 5);
        step1(&A, 0, obs, &rew, &term);
        check(A.mat_attr[0] == 1 && A.matalive[0],
              "gold 150 px below the player root is magnetised (area centred on the root)");

        /* 2f: melee hits only trees; an enemy at the same spot is untouched. */
        bt_core_init(&A, 33u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        A.px = 200.0f; A.py = 200.0f;
        p = bt_spawn_summon_row(&A, (unsigned short)row, 1000.0f, 700.0f, 0, 0.0f, 0.0f, 0.0f);
        int tk = tree_kind();
        check(tk >= 0, "a tree (neutral) kind exists in BT_NEUTRALS");
        int ek = kind_by_substr("chaser");
        if (ek < 0) ek = plain_enemy_kind();
        check(ek >= 0, "an enemy kind with a contact Hitbox exists");
        if (tk >= 0) {
            bt_spawn_neutral(&A, tk, 1000.0f, 700.0f);
            int ts = find_enemy_slot(&A, tk, 1);
            check(ts >= 0, "tree entity spawned");
            bt_spawn_kind(&A, ek, 1000.0f, 700.0f);
            int es = find_enemy_slot(&A, ek, 0);
            float ehp0 = (es >= 0) ? A.ehp[es] : 0.0f;
            step1(&A, 0, obs, &rew, &term);
            check(A.pet_tree_in[0] == 1, "tree inside the lootworm's TreeArea2D");
            int killed = 0;
            for (int t = 0; t < 240 && !killed; t++) {
                step1(&A, 0, obs, &rew, &term);
                if (ts >= 0 && !A.ealive[ts]) killed = 1;
            }
            check(killed, "lootworm's own melee swing kills the tree");
            if (es >= 0 && A.ealive[es])
                check_near(A.ehp[es], ehp0, 1e-3f, "lootworm melee never damages a non-tree");
        }

        /* 2g: lootworm.gd:68-75 doubles the gold with its own double_chance and
         * then hands it to gold.pickup(player_index), so the player's common
         * pickup effects (main.gd:798-853) run on the pet's pickup: the
         * metal_detector chance_double_gold doubles the already doubled value
         * and the cute_monkey heal_when_pickup_gold heals the player by 1. */
        bt_core_init(&A, 34u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        A.px = 200.0f; A.py = 200.0f;                 /* player away from the gold */
        int md = item_by_name("metal_detector");
        int cm = item_by_name("cute_monkey");
        check(md >= 0 && cm >= 0, "metal_detector and cute_monkey items exist");
        if (md >= 0) bt_apply_item(&A, (unsigned short)md);
        if (cm >= 0) bt_apply_item(&A, (unsigned short)cm);
        /* Both items roll their own chance per gold: pin the two keys they emit
         * (metal_detector chance_double_gold, cute_monkey
         * heal_when_pickup_gold) for a deterministic pickup. The HP delta below
         * is isolated from the periodic regen tick and from the level-up
         * max_hp grant that the pickup's own XP would trigger. */
        A.scalar_effect[BT_ISTR_CHANCE_DOUBLE_GOLD] = 100.0f;
        A.scalar_effect[BT_ISTR_HEAL_WHEN_PICKUP_GOLD] = 100.0f;
        A.hp = A.max_hp * 0.5f;                 /* below max, well clear of 0 */
        A.regen_timer = 1.0e9f;
        A.xp_need = 1000000000;
        float mat1 = A.materials, xp1 = A.xp, hp1 = A.hp;
        bt_spawn_summon_row(&A, (unsigned short)row, 1000.0f, 700.0f, 0, 0.0f, 1.0f, 0.0f);
        spawn_settled_gold(&A, 1040.0f, 700.0f, 5);
        /* The pickup happens within a few frames (the pet drives the 40 px to
         * the gold); the bound only stops the loop from spinning if it does. */
        for (int t = 0; t < 20 && A.matcount > 0; t++) step1(&A, 0, obs, &rew, &term);
        check(A.matcount == 0, "lootworm picks the gold up with the player's pickup effects on");
        check_near(A.materials - mat1, 20.0f, 1e-3f,
                   "pet double_chance x2 then chance_double_gold x2 -> 5 * 4");
        check(A.xp - xp1 >= 20.0f, "the boosted value is the player's 1:1 gold/XP credit");
        check_near(A.hp - hp1, 1.0f, 1e-3f, "heal_when_pickup_gold heals the player by 1");
    }

    printf("[3] Scapegoat: hit intake, hurtbox cooldown, death, revive\n");
    {
        bt_core_init(&A, 41u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        A.px = 200.0f; A.py = 200.0f;                 /* player away: no revive yet */
        int row = eff_row(BT_ITEMEFF_SCAPEGOAT);
        int p = bt_spawn_summon_row(&A, (unsigned short)row, 1000.0f, 700.0f, 0, 0.0f, 0.0f, 0.0f);
        check(p == 0 && A.pet_alive[0], "scapegoat spawned");
        check_near(A.pet_maxhp[0], 40.0f, 1e-3f, "scapegoat max hp = 40 * health_boost(1.0)");
        check_near(A.pet_hp[0], 40.0f, 1e-3f, "scapegoat starts at full health");
        check_near(A.pet_revive_dur[0], 2.0f, 1e-3f, "revive_duration 2.0 from ScapegoatEffect");
        check(A.pet_dead[0] == 0, "scapegoat starts alive");

        int ek = kind_by_substr("chaser");
        if (ek < 0) ek = plain_enemy_kind();
        bt_spawn_kind(&A, ek, 1000.0f, 700.0f);       /* overlapping its Hurtbox */
        {
            int es = find_enemy_slot(&A, ek, 0);
            if (es >= 0) A.espd[es] = 0.0f;           /* hold the attacker in place */
        }
        /* The Scapegoat wanders (target_rand_pos), so hold it onto the attacker
         * that is itself held above: one frame's script step moves it < 3 px,
         * well inside the 32 px Hurtbox, so the contact persists across the
         * 0.6 s cooldown. */
        A.pet_x[0] = 1000.0f; A.pet_y[0] = 700.0f;
        step1(&A, 0, obs, &rew, &term);
        check_near(A.pet_hp[0], 39.0f, 1e-3f, "enemy contact Hitbox costs 1 hp (take_damage(1))");
        check_near(A.pet_hurt_cd[0], BT_PET_INVULN_CD, 1e-3f, "hurtbox parked for 0.6 s");
        for (int t = 0; t < 10; t++) {
            A.pet_x[0] = 1000.0f; A.pet_y[0] = 700.0f;
            step1(&A, 0, obs, &rew, &term);
        }
        check_near(A.pet_hp[0], 39.0f, 1e-3f, "no second hit during the invulnerable cooldown");
        for (int t = 0; t < 32; t++) {
            A.pet_x[0] = 1000.0f; A.pet_y[0] = 700.0f;
            step1(&A, 0, obs, &rew, &term);
        }
        check(A.pet_hp[0] < 39.0f, "hurtbox re-enables after 0.6 s -> next hit lands");

        /* Kill it, then hold the player outside the heal zone: no revive. */
        A.pet_x[0] = 1000.0f; A.pet_y[0] = 700.0f;
        A.pet_hp[0] = 1.0f;
        A.pet_hurt_cd[0] = 0.0f;
        step1(&A, 0, obs, &rew, &term);
        check(A.pet_dead[0] == 1 && A.pet_hp[0] == 0.0f, "scapegoat dies at 0 hp");
        for (int t = 0; t < 60; t++) step1(&A, 0, obs, &rew, &term);
        check(A.pet_dead[0] == 1 && A.pet_hp[0] == 0.0f,
              "dead scapegoat does not revive while no player is in HealingTriggeringZone");

        /* Player steps inside the zone: max_health / revive_duration per second.
         * The dead pet holds position (die() clears _can_move), so the revive
         * distance is deterministic. */
        A.pet_x[0] = 1000.0f;
        A.pet_y[0] = 700.0f;
        A.px = 1080.0f; A.py = 720.0f;                /* inside HealingTriggeringZone */
        step1(&A, 0, obs, &rew, &term);
        for (int t = 0; t < 30; t++) step1(&A, 0, obs, &rew, &term);
        check(A.pet_hp[0] > 0.0f && A.pet_hp[0] < A.pet_maxhp[0],
              "revive fills health at max_health / revive_duration per second (~20 hp/s)");
        check(A.pet_dead[0] == 1, "still dead while the revive is running");
        for (int t = 0; t < 120 && A.pet_dead[0]; t++) step1(&A, 0, obs, &rew, &term);
        check(A.pet_dead[0] == 0 && A.pet_hp[0] == A.pet_maxhp[0], "scapegoat rises at full health");
    }

    printf("[4] BotOMine: follows the player and stops distance_to_target away\n");
    {
        bt_core_init(&A, 51u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        A.px = 1000.0f; A.py = 700.0f;
        int row = eff_row(BT_ITEMEFF_BOT_O_MINE);
        int p = bt_spawn_summon_row(&A, (unsigned short)row, 1500.0f, 700.0f, 0, 0.0f, 0.0f, 0.0f);
        check(p == 0, "bot_o_mine spawned");
        for (int t = 0; t < 150; t++) step1(&A, 0, obs, &rew, &term);
        float dx = A.pet_x[0] - A.px, dy = A.pet_y[0] - A.py;
        float d = bt_sqrtf(dx * dx + dy * dy);
        check_near(d, 100.0f, 5.0f, "bot_o_mine stops distance_to_target = 100 from the player");
        float x1 = A.pet_x[0];
        for (int t = 0; t < 60; t++) step1(&A, 0, obs, &rew, &term);
        check_near(A.pet_x[0], x1, 3.0f, "bot_o_mine holds station once stopped");
    }

    printf("[5] BonkDog: first enemy when none is in range, nearest when one is\n");
    {
        bt_core_init(&A, 61u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        A.px = 100.0f; A.py = 100.0f;
        int row = eff_row(BT_ITEMEFF_BONK_DOG);
        int p = bt_spawn_summon_row(&A, (unsigned short)row, 1000.0f, 700.0f, 0, 0.0f, 0.0f, 0.0f);
        check(p == 0, "bonk_dog spawned");
        int ek = kind_by_substr("chaser");
        if (ek < 0) ek = plain_enemy_kind();
        bt_spawn_kind(&A, ek, 2000.0f, 1400.0f);      /* ~1300 px away: outside the 600 zone */
        float x0 = A.pet_x[0], y0 = A.pet_y[0];
        for (int t = 0; t < 30; t++) step1(&A, 0, obs, &rew, &term);
        float dx = A.pet_x[0] - x0, dy = A.pet_y[0] - y0;
        check(dx > 50.0f && dy > 20.0f,
              "bonk_dog chases the fallback enemy even beyond its acquisition zone");
        float d0 = bt_sqrtf((A.pet_x[0] - 2000.0f) * (A.pet_x[0] - 2000.0f) +
                            (A.pet_y[0] - 1400.0f) * (A.pet_y[0] - 1400.0f));
        for (int t = 0; t < 30; t++) step1(&A, 0, obs, &rew, &term);
        float d1 = bt_sqrtf((A.pet_x[0] - 2000.0f) * (A.pet_x[0] - 2000.0f) +
                            (A.pet_y[0] - 1400.0f) * (A.pet_y[0] - 1400.0f));
        check(d1 < d0 - 100.0f, "bonk_dog keeps closing on the target (no stop for enemies)");
    }

    printf("[6] CatlingGun: PlayerTriggerZone freezes the pet\n");
    {
        bt_core_init(&A, 71u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        A.px = 1000.0f; A.py = 700.0f;
        int row = eff_row(BT_ITEMEFF_CATLING_GUN);
        int p = bt_spawn_summon_row(&A, (unsigned short)row, 1020.0f, 700.0f, 0, 0.0f, 0.0f, 0.0f);
        check(p == 0, "catling_gun spawned");
        step1(&A, 0, obs, &rew, &term);
        check(A.pet_mad[0] == 1, "player inside PlayerTriggerZone -> mad mode");
        float x0 = A.pet_x[0], y0 = A.pet_y[0];
        for (int t = 0; t < 60; t++) step1(&A, 0, obs, &rew, &term);
        check_near(A.pet_x[0], x0, 1e-4f, "mad catling_gun does not move");
        check_near(A.pet_y[0], y0, 1e-4f, "mad catling_gun does not move (y)");
        A.px = 100.0f; A.py = 100.0f;
        for (int t = 0; t < 30; t++) step1(&A, 0, obs, &rew, &term);
        check(A.pet_mad[0] == 0, "zone empty -> mad mode ends");
        check(bt_fabsf(A.pet_x[0] - x0) + bt_fabsf(A.pet_y[0] - y0) > 10.0f,
              "catling_gun resumes its random-position movement");
    }

    printf("[7] DocMoth BoostZone: entry/exit + hp_regen_bonus + curse scaling\n");
    {
        bt_core_init(&A, 81u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        /* bt_core_init already derives the Bull's regen (stat_raw[1] =
         * BT_BULL_REGEN, stat_gain[1] = BT_REGEN_GAIN); the regen tick below is
         * forced with regen_timer, so no fixture-side stat write is needed. */
        int row = eff_row(BT_ITEMEFF_DOC_MOTH);
        int p = bt_spawn_summon_row(&A, (unsigned short)row, A.px, A.py, 0, 0.0f, 0.0f, 0.0f);
        check(p == 0, "doc_moth spawned on the player");
        step1(&A, 0, obs, &rew, &term);
        check(A.pet_zone_in[0] == 1, "player inside the 200 px BoostZone");
        check_near(A.zone_hp_regen_bonus, 1.0f, 1e-3f, "hp_regen_bonus on entry (value 1)");
        check(A.zone_hp_regen_threshold == 100, "hp_regen_bonus threshold value2 = 100");
        check(A.zone_double_lifesteal == 1, "double_lifesteal_bonus on entry");

        /* One regen tick with the zone on the player: 1 hp * (1 + 1). */
        A.hp = 10.0f;
        A.regen_timer = 0.0f;
        float before = A.hp;
        step1(&A, 0, obs, &rew, &term);
        check_near(A.hp - before, 2.0f, 1e-3f, "BoostZone doubles the regen tick");

        /* Push the pet outside (the zone follows the pet): the bonus clears. */
        A.pet_x[0] = A.px + 260.0f;
        A.pet_y[0] = A.py;
        step1(&A, 0, obs, &rew, &term);
        check(A.pet_zone_in[0] == 0, "player outside the BoostZone after the pet moves");
        check_near(A.zone_hp_regen_bonus, 0.0f, 1e-3f, "hp_regen_bonus unapplied on exit");
        check(A.zone_double_lifesteal == 0, "double_lifesteal_bonus unapplied on exit");
        A.hp = 10.0f;
        A.regen_timer = 0.0f;
        before = A.hp;
        step1(&A, 0, obs, &rew, &term);
        check_near(A.hp - before, 1.0f, 1e-3f, "regen back to 1 hp per tick outside the zone");

        /* Cursed DocMoth: boost_zone_scale > 1 widens the membership radius. */
        bt_core_init(&B, 82u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        B.n_live_groups = 0;
        B.curse_apply = 1;
        B.curse_maxmod = 0.0f;
        bt_apply_item(&B, 201);                        /* item_doc_moth */
        B.curse_apply = 0;
        check(B.n_summon_fx == 1, "cursed doc_moth registered one effect instance");
        check(B.summon_fx[0].boost_zone_scale > 1.0f, "curse scaled the instance boost_zone_scale");
        for (int t = 0; t < 123; t++) step1(&B, 0, obs, &rew, &term);
        check(B.pet_n == 1 && B.pet_alive[0], "cursed doc_moth entity spawned");
        {
            float scale = (float)B.summon_fx[0].boost_zone_scale;
            float edge = 200.0f * scale + BT_PLAYER_BODY_R;
            B.pet_x[0] = B.px + edge - 5.0f;
            B.pet_y[0] = B.py;
            step1(&B, 0, obs, &rew, &term);
            check(B.pet_zone_in[0] == 1, "just inside the scaled zone");
            B.pet_x[0] = B.px + edge + 20.0f;
            B.pet_y[0] = B.py;
            step1(&B, 0, obs, &rew, &term);
            check(B.pet_zone_in[0] == 0, "just outside the scaled zone");
        }
    }

    printf("[8] source-parity fixes: spawn draw, deferred hitbox, magnet routing, peer layers\n");
    {
        /* (1) unit.gd:77: every Unit.init consumes a degenerate rand_range(0,0)
         * (all pets have speed_randomization 0) AFTER the movement behaviour
         * init.  A FOLLOW_TARGET lootworm draws nothing else, so its spawn must
         * advance the global stream by exactly that one call. */
        bt_core_init(&A, 81u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        bt_core_init(&B, 81u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        B.n_live_groups = 0;
        int row = eff_row(BT_ITEMEFF_LOOTWORM);
        (void)bt_spawn_summon_row(&A, (unsigned short)row, 1000.0f, 700.0f, 0, 0.0f, 0.0f, 0.0f);
        (void)bt_rand_range(&B, 0.0f, 0.0f);
        check(A.rng.state == B.rng.state,
              "pet spawn consumes exactly the degenerate rand_range(0,0) (unit.gd:77)");
    }
    {
        /* (3) lootworm.gd:91-99 mutates _hitbox.damage IN PLACE (persists) and
         * the hit is delivered one frame later (unit.gd:610-611 deferred
         * Hurtbox handler). */
        bt_core_init(&A, 82u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        A.px = 200.0f; A.py = 200.0f;
        int row = eff_row(BT_ITEMEFF_LOOTWORM);
        bt_spawn_summon_row(&A, (unsigned short)row, 1000.0f, 700.0f, 0, 0.0f, 0.0f, 0.0f);
        int tk = tree_kind();
        bt_spawn_neutral(&A, tk, 1000.0f, 700.0f);
        int ts = find_enemy_slot(&A, tk, 1);
        check(ts >= 0, "tough tree spawned for the hitbox-mutation test");
        A.emax[ts] = 5000.0f; A.ehp[ts] = 5000.0f;      /* emulate a tough tree */
        step1(&A, 0, obs, &rew, &term);
        check_near(A.pet_hit_dmg[0], 1000.0f, 1e-3f,
                   "hitbox.damage mutated in place to tree_hp / tree_damage_ratio");
        int armed = 0;
        for (int t = 0; t < 300 && !armed; t++) {
            step1(&A, 0, obs, &rew, &term);
            if (A.pet_melee_tgt[0] >= 0) armed = 1;
        }
        check(armed, "the lootworm armed a swing against the tree");
        check_near(A.ehp[ts], 5000.0f, 1e-3f, "the armed swing has not damaged the tree yet");
        step1(&A, 0, obs, &rew, &term);
        check(A.ehp[ts] < 5000.0f, "the Hurtbox hit lands one frame after the swing");
        check_near(A.pet_hit_dmg[0], 1000.0f, 1e-3f,
                   "the mutated hitbox damage persists (never decreases)");
    }
    {
        /* (5) PlayerTriggerZone (mask 514) cannot see a layer-0 pet body
         * (lootworm/scapegoat/doc_moth), but does see another catling_gun. */
        bt_core_init(&A, 83u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        A.px = 200.0f; A.py = 200.0f;                    /* player far away */
        int crow = eff_row(BT_ITEMEFF_CATLING_GUN);
        int c = bt_spawn_summon_row(&A, (unsigned short)crow, 1000.0f, 700.0f, 0, 0.0f, 0.0f, 0.0f);
        int lrow = eff_row(BT_ITEMEFF_LOOTWORM);
        (void)bt_spawn_summon_row(&A, (unsigned short)lrow, 1010.0f, 700.0f, 0, 0.0f, 0.0f, 0.0f);
        check(c == 0 && A.pet_n >= 2, "catling_gun + lootworm spawned");
        step1(&A, 0, obs, &rew, &term);
        check(A.pet_mad[c] == 0,
              "a layer-0 pet does not trigger the catling_gun PlayerTriggerZone");
        (void)bt_spawn_summon_row(&A, (unsigned short)crow, 990.0f, 700.0f, 0, 0.0f, 0.0f, 0.0f);
        step1(&A, 0, obs, &rew, &term);
        check(A.pet_mad[c] == 0,
              "another catling_gun is excluded by is_catling_gun() (catling_gun.gd:177)");
        /* a visible, non-catling pet body (bonk_dog, layer 512) DOES freeze it */
        int brow = eff_row(BT_ITEMEFF_BONK_DOG);
        (void)bt_spawn_summon_row(&A, (unsigned short)brow, 980.0f, 700.0f, 0, 0.0f, 0.0f, 0.0f);
        step1(&A, 0, obs, &rew, &term);
        check(A.pet_mad[c] == 1, "a layer-512 non-catling pet freezes it");
    }
    {
        /* (6) main.gd:740-746: instant_gold_attracting hands the gold to an
         * existing LOOTWORM (attracted_by = the pet), which then homes it in. */
        bt_core_init(&A, 84u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        A.px = 200.0f; A.py = 200.0f;
        int lrow = eff_row(BT_ITEMEFF_LOOTWORM);
        bt_spawn_summon_row(&A, (unsigned short)lrow, 1000.0f, 700.0f, 0, 0.0f, 0.0f, 0.0f);
        A.scalar_effect[BT_ISTR_INSTANT_GOLD_ATTRACTING] = 100.0f;  /* roll succeeds */
        spawn_settled_gold(&A, 1000.0f, 1200.0f, 5);     /* 500 px: only a routed home reaches */
        check(A.mat_attr[0] == 2 && A.mat_by[0] == 1,
              "instant_gold_attracting routes the gold to the existing lootworm");
        float d0 = bt_v2dist(bt_v2(A.matx[0], A.maty[0]), bt_v2(A.pet_x[0], A.pet_y[0]));
        for (int t = 0; t < 10; t++) step1(&A, 0, obs, &rew, &term);
        float d1 = bt_v2dist(bt_v2(A.matx[0], A.maty[0]), bt_v2(A.pet_x[0], A.pet_y[0]));
        check(A.matalive[0] && d1 < d0, "the lootworm-owned gold homes in on the pet");
    }

    printf("[9] wandering_bot orbit + enemies+bosses fallback order\n");
    {
        /* (2) wandering_bot.gd class-init draws (3 global rand_range) + the
         * orbit around the player (wandering_bot.gd:18-21). */
        bt_core_init(&A, 85u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        A.px = 1000.0f; A.py = 700.0f;
        int wrow = -1;
        for (int r = 0; r < BT_NITEM_EFF_ROWS; r++)
            if (BT_ITEM_EFF_ROWS[r].key == BT_ISTR_WANDERING_BOT) { wrow = r; break; }
        check(wrow >= 0, "a wandering_bot effect row exists");
        int w = bt_spawn_summon_row(&A, (unsigned short)wrow, 0.0f, 0.0f, 1, 0.0f, 0.0f, 0.0f);
        check(w == 0 && A.pet_alive[0], "wandering_bot structure spawned");
        check(A.pet_wb_dist[0] >= 100.0f && A.pet_wb_dist[0] <= 200.0f,
              "distance drawn from rand_range(100,200) at spawn");
        check(A.pet_wb_rots[0] >= 2.0f && A.pet_wb_rots[0] <= 2.5f,
              "rotation_speed drawn from rand_range(2,2.5) at spawn");
        check(A.pet_wb_angle[0] >= 0.0f && A.pet_wb_angle[0] <= 6.2831853f,
              "_angle drawn from rand_range(0,2*PI) at spawn");
        float a0 = A.pet_wb_angle[0], d0 = A.pet_wb_dist[0], r0 = A.pet_wb_rots[0];
        /* ordering: the bot's position is NOT advanced by the pet-motion step
         * (which runs before the turret firing block), only by the core step. */
        float wx = A.pet_x[0];
        bt_step_pet_motion(&A);
        check(A.pet_x[0] == wx && A.pet_wb_angle[0] == a0,
              "the wandering_bot does not move in bt_step_pet_motion (targeting first)");
        step1(&A, 0, obs, &rew, &term);
        check(A.pet_wb_angle[0] != a0,
              "the wandering_bot moves in the core's post-targeting firing block");
        check_near(A.pet_wb_angle[0], a0 + BT_DT * r0, 1e-6f,
                   "orbit angle advances by delta * rotation_speed");
        check_near(A.pet_x[0], A.px + bt_cosf(A.pet_wb_angle[0]) * d0, 1e-3f,
                   "orbit x = player.x + cos(angle) * distance");
        check_near(A.pet_y[0], A.py + bt_sinf(A.pet_wb_angle[0]) * d0, 1e-3f,
                   "orbit y = player.y + sin(angle) * distance");
        A.px += 300.0f;                       /* the orbit follows the player */
        step1(&A, 0, obs, &rew, &term);
        check_near(A.pet_x[0], A.px + bt_cosf(A.pet_wb_angle[0]) * d0, 1e-3f,
                   "the wandering_bot orbit re-centres on the player each frame");
    }
    {
        /* (4) closest_enemy_target_behavior.gd:39-43: the "first enemy" fallback
         * is the first entry of `enemies + bosses`: append order with every
         * ordinary enemy before every boss.
         * Two far enemies, first-spawned to the LEFT: the pet must pick it. */
        bt_core_init(&A, 86u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
        A.n_live_groups = 0;
        A.px = 1000.0f; A.py = 700.0f;
        int brow = eff_row(BT_ITEMEFF_BONK_DOG);
        bt_spawn_summon_row(&A, (unsigned short)brow, 1000.0f, 700.0f, 0, 0.0f, 0.0f, 0.0f);
        int ek = kind_by_substr("chaser");
        if (ek < 0) ek = plain_enemy_kind();
        bt_spawn_kind(&A, ek, 200.0f, 700.0f);      /* first (left), 800 px away  */
        bt_spawn_kind(&A, ek, 1900.0f, 700.0f);     /* second (right), 900 px away */
        float x0 = A.pet_x[0];
        step1(&A, 0, obs, &rew, &term);
        check(A.pet_x[0] < x0, "fallback picks the first-appended enemy (left)");
        int es1 = find_enemy_slot(&A, ek, 0);
        check(es1 >= 0, "the left enemy is live");
        if (es1 >= 0) bt_enemy_set_alive(&A, es1, 0);            /* kill the first enemy */
        x0 = A.pet_x[0];
        step1(&A, 0, obs, &rew, &term);
        check(A.pet_x[0] > x0, "with the first dead, the fallback takes the next");
        /* a boss is always LAST in `enemies + bosses`, so an ordinary enemy with
         * a HIGHER append sequence still wins. */
        int bk = -1;
        for (int i = 0; i < BT_NKINDS; i++) if (bt_kind_class(i) == 2) { bk = i; break; }
        check(bk >= 0, "a boss kind exists");
        if (bk >= 0) {
            bt_core_init(&B, 86u, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            B.n_live_groups = 0;
            B.px = 1000.0f; B.py = 700.0f;
            bt_spawn_summon_row(&B, (unsigned short)brow, 1000.0f, 700.0f, 0, 0.0f, 0.0f, 0.0f);
            bt_spawn_kind(&B, bk, 200.0f, 700.0f);   /* boss appended FIRST */
            bt_spawn_kind(&B, ek, 1900.0f, 700.0f);  /* ordinary enemy appended later */
            float bx0 = B.pet_x[0];
            step1(&B, 0, obs, &rew, &term);
            check(B.pet_x[0] > bx0,
                  "an ordinary enemy precedes a boss regardless of append order");
        }
    }

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
