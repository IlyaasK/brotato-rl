/* tests/test_brotato_enemy_projectiles.cpp
 *
 * Source-backed regressions for the ENEMY/NEUTRAL shot -> PLAYER hit timing
 * (the `pr*` pool's player branch, ocean/brotato/brotato_core.h bt_eproj_phase
 * + bt_player_hitq_apply):
 *   - the pair is only REPORTED by the frame's Physics2DServer::step, so the
 *     spawn frame neither damages the player nor stops the shot: every shot that
 *     reports the player is queued (BtHitQueue.np/pa) and resolved by the NEXT
 *     frame's MessageQueue::flush -- the same flush the contact hit uses;
 *   - the flush runs ONE deferred handler per reported pair, in report order, so
 *     the dodge roll / hit_prot / armor / iframe / hp order is byte-for-byte what
 *     a direct bt_player_damage sequence produces (only the FRAME moved);
 *   - the shot stops when its own hit resolves, damage dealt or not;
 *   - the queue is fail-closed at BT_HITQ_CAP (BT_CAP_HITQ latches).
 *
 * Every expected number is derived here from the recovered sources:
 *   projectiles/bullet_enemy/enemy_projectile.gd:4-14  _ready -> shoot(); stop()
 *   projectiles/projectile.gd:28-90                    motion / stop()
 *   overlap/hitbox.gd:104-110                          enable()/disable()
 *   entities/units/unit/unit.gd:536-612                deferred hurt handler
 *   entities/units/player/player.gd:300-320,430-445    dodge / iframe / armor
 *
 * Build and run:
 *     c++ -O2 -o build/test_brotato_enemy_projectiles tests/test_brotato_enemy_projectiles.cpp -lm
 *     ./build/test_brotato_enemy_projectiles
 *
 * Exit code 0 = all checks passed.
 */

#include "../ocean/brotato/brotato_core.h"
#include "../ocean/brotato/brotato_capacity_host.h"

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
static void check_eq_int(int got, int want, const char* msg) {
    g_checks++;
    if (got != want) { g_failures++; printf("  FAIL: %s (got %d want %d)\n", msg, got, want); }
}

static BtState A, B, C;

/* BtState is ~1.4 MiB; keep the states static.  Same fixture shape as
 * test_brotato_weapon_projectiles.cpp: no regen, no death mid-test by default. */
static void fresh(BtState* s, bt_u64 seed) {
    bt_core_init(s, seed, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
    s->n_live_groups = 0;
    bt_core_stat_add(s, 1, -15.0f);
    s->regen_timer = 99.0f;
    s->hp = 1.0e9f;
    s->max_hp = 1.0e9f;
    s->iframe = 0.0f;
    s->dodge = 0.0f;
    s->hit_prot = 0;
    bt_item_expl_reset(s);
    bt_wproj_reset(s);
}

/* One enemy shot (`friendly = 0`) aimed at the player from `dist` px away on
 * the +x axis, moving toward the player at `speed`. */
static int spawn_enemy_shot(BtState* s, float dist, float speed, float dmg) {
    float x = s->px + dist;
    float y = s->py + BT_PLAYER_HURT_Y;
    int i = bt_spawn_projectile(s, x, y, BT_PI, speed, dmg, BT_SHAPE_CIRCLE,
                                8.0f, 0.0f, 0.0f, 0.0f, 0);
    return i;
}

int main(void) {
    /* ---------------------------------------- spawn-frame report, deferred hit */
    printf("[1] the shot's frame REPORTS the pair; the next frame's flush hurts\n");
    {
        BtState* s = &A; fresh(s, 7);
        s->hp = 100.0f;
        int i = spawn_enemy_shot(s, 10.0f, 0.0f, 10.0f);   /* already overlapping */
        check(i >= 0, "the shot spawned");
        float hp0 = s->hp;
        float r = 0.0f;
        bt_eproj_phase(s, &r, 0);
        check_eq_int((int)s->hitq.np, 1, "the frame queues the reported pair");
        check_eq_int((int)s->hitq.pa[0], i, "the queued pair names the shot slot");
        check_near(s->hp, hp0, 1e-6f, "no damage on the pair's frame");
        check(s->pralive[i] == 1, "and the shot is not stopped yet");
        check_near(r, 0.0f, 1e-6f, "no reward either");
        /* the hitbox is `active` (unit.gd:537 guard), so the pair survives */
        bt_player_hurt_flush(s, &r);
        check_eq_int((int)s->hitq.np, 0, "the flush drains the queue");
        check(s->hp < hp0, "the deferred hurt lands at the flush");
        check(s->pralive[i] == 0, "enemy_projectile.gd:12-13 stop on hit");
        check_near(r, R_DMG * (hp0 - s->hp), 1e-4f, "damage reward follows the hit");
        check(s->iframe > 0.0f, "the hit started the iframe timer");
    }

    /* --------------------------------------------------------- moving hit */
    printf("[2] a shot that reaches the player pairs where it reaches it\n");
    {
        BtState* s = &A; fresh(s, 11);
        s->hp = 100.0f;
        int i = spawn_enemy_shot(s, 300.0f, 600.0f, 10.0f);
        check(i >= 0, "the shot spawned");
        float hp0 = s->hp, r = 0.0f;
        check_eq_int((int)s->hitq.np, 0, "nothing is reported at spawn range");
        int steps = 0;
        while (s->hitq.np == 0 && steps < 60) { bt_eproj_phase(s, &r, 0); steps++; }
        check(steps > 1, "the shot travelled before it reported");
        check_eq_int((int)s->hitq.np, 1, "the overlap frame reports the pair");
        check_near(s->hp, hp0, 1e-6f, "no damage on the pair's frame");
        bt_player_hurt_flush(s, &r);
        check(s->hp < hp0, "the moving hit lands one frame later");
        check(s->pralive[i] == 0, "one shot, one hit");
    }

    /* ------------------------------------------- several shots, one frame */
    printf("[3] several shots reporting the player in one frame\n");
    {
        BtState* s = &A; fresh(s, 13);
        s->hp = 100.0f;
        int i0 = spawn_enemy_shot(s, 10.0f, 0.0f, 10.0f);
        int i1 = spawn_enemy_shot(s, 6.0f, 0.0f, 20.0f);
        int i2 = spawn_enemy_shot(s, 14.0f, 0.0f, 30.0f);
        check(i0 >= 0 && i1 >= 0 && i2 >= 0, "three shots spawned");
        float r = 0.0f;
        bt_eproj_phase(s, &r, 0);
        check_eq_int((int)s->hitq.np, 3, "every reported pair is queued");
        check_eq_int((int)s->hitq.pa[0], i0, "report order is the slot scan");
        check_eq_int((int)s->hitq.pa[1], i1, "report order is the slot scan");
        check_eq_int((int)s->hitq.pa[2], i2, "report order is the slot scan");
        /* the twin: the same three handlers run directly, in the same order */
        BtState* t = &B; fresh(t, 13);
        t->hp = 100.0f;
        float rt = 0.0f;
        for (int k = 0; k < 3; k++) {
            float dmg = (k == 0 ? 10.0f : (k == 1 ? 20.0f : 30.0f));
            float taken = bt_player_damage(t, dmg, 1, 0, &rt);
            if (taken > 0.0f) rt += R_DMG * taken;
        }
        float hp_twin = t->hp;
        bt_player_hurt_flush(s, &r);
        check_near(s->hp, hp_twin, 1e-4f, "the flush equals the same handler sequence");
        check_near(r, rt, 1e-4f, "reward identical too");
        check_near(s->iframe, t->iframe, 1e-4f, "iframe identical");
        check(s->pralive[i0] == 0 && s->pralive[i1] == 0 && s->pralive[i2] == 0,
              "each shot stops on its own hit, damage dealt or not");
        /* the iframe the first pair started is what turns the rest away: the
         * same run with ONE handler reaches the same hp */
        BtState* u = &C; fresh(u, 13); u->hp = 100.0f;
        float ru = 0.0f;
        (void)bt_player_damage(u, 10.0f, 1, 0, &ru);
        check_near(s->hp, u->hp, 1e-4f, "only the first pair got through the iframe gate");
    }

    /* ------------------------- dodge / armor / death ordering is unchanged */
    printf("[4] dodge roll, armor and the lethal clamp keep their order\n");
    {
        for (int variant = 0; variant < 3; variant++) {
            BtState* s = &A; fresh(s, (bt_u64)(17 + variant));
            BtState* t = &B; fresh(t, (bt_u64)(17 + variant));
            float dmg = 10.0f;
            s->hp = 100.0f; t->hp = 100.0f;
            if (variant == 0) {                 /* dodge draw, then armor */
                s->dodge = 0.5f; t->dodge = 0.5f;
                s->armor = 5.0f; t->armor = 5.0f;
            } else if (variant == 1) {           /* armor only */
                s->armor = 20.0f; t->armor = 20.0f;
            } else {                             /* lethal clamp */
                s->hp = 3.0f; t->hp = 3.0f;
            }
            int i = spawn_enemy_shot(s, 10.0f, 0.0f, dmg);
            (void)i;
            float r = 0.0f, rt = 0.0f;
            bt_eproj_phase(s, &r, 0);
            bt_player_hurt_flush(s, &r);
            (void)bt_player_damage(t, dmg, 1, 0, &rt);
            check_near(s->hp, t->hp, 1e-5f, "hp matches the direct handler");
            check_near(s->iframe, t->iframe, 1e-5f, "iframe matches the direct handler");
            check(memcmp(&s->rng, &t->rng, sizeof(BtRng)) == 0,
                  "identical RNG state: the dodge draw happens at the same point");
        }
        BtState* s = &A; fresh(s, 23); s->hp = 3.0f;
        int i = spawn_enemy_shot(s, 10.0f, 0.0f, 10.0f);
        float r = 0.0f;
        bt_eproj_phase(s, &r, 0);
        bt_player_hurt_flush(s, &r);
        check_near(s->hp, 0.0f, 1e-6f, "the lethal hit clamps to the remaining hp");
        check_near(r, R_DMG * 3.0f, 1e-4f, "and the reward follows the clamped damage");
        check(s->pralive[i] == 0, "the shot still stops");
    }

    /* --------------------------------------------------- fail-closed bound */
    printf("[5] the player half of the queue is fail-closed too\n");
    {
        BtState* s = &A; fresh(s, 29);
        s->hp = 1000.0f;
        s->max_hp = 1000.0f;
        int want = BT_HITQ_CAP + 1;
        check(want <= BT_MAX_PROJ, "the crowding shots fit the projectile pool");
        int spawned = 0;
        for (int k = 0; k < want; k++) {
            int i = spawn_enemy_shot(s, 4.0f, 0.0f, 10.0f);
            if (i < 0) break;
            spawned++;
        }
        check_eq_int(spawned, want, "the crowding shots spawned");
        float r = 0.0f;
        bt_eproj_phase(s, &r, 0);
        check_eq_int((int)s->hitq.np, BT_HITQ_CAP, "exactly the capacity is queued");
        check_eq_int((int)s->hitq.overflow, 1, "the pairs that do not fit are dropped");
        check((bt_capacity_mask(s) & (1ull << BT_CAP_HITQ)) != 0ull, "BT_CAP_HITQ latches");
        float hp0 = s->hp;
        bt_player_hurt_flush(s, &r);
        check(s->hp < hp0, "the queued prefix still resolves");
        check_eq_int((int)s->hitq.np, 0, "and the queue drains");
        /* the dropped shot is still live: nothing was silently reordered onto it */
        int live = 0, dead = 0;
        for (int k = 0; k < BT_MAX_PROJ; k++) { if (s->pralive[k]) live++; else dead++; }
        check_eq_int(live, 1, "the one dropped shot is still in flight");
        check(dead > 0, "the queued prefix stopped its shots");
    }

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
