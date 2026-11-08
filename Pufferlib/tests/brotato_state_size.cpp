/* tests/brotato_state_size.cpp
 *
 * Per-member size / offset report for BtState, plus a live peak-occupancy
 * probe that steps the shared core (the same code the CUDA backend runs) and
 * records the high-water mark of every bounded table, so a capacity bound can
 * be justified from a real run rather than guessed.
 *
 * Build and run:
 *     c++ -std=c++17 -O2 -ffp-contract=off -I. -Iocean/brotato \
 *         -o build/brotato_state_size tests/brotato_state_size.cpp -lm
 *     ./build/brotato_state_size            # sizes + 3 long Demon runs
 *     ./build/brotato_state_size sizes      # sizes only
 *     ./build/brotato_state_size run 10     # 10 runs
 *
 * The field table is generated (tests/gen_brotato_state_fields.inc) from the
 * struct definition by tests/gen_brotato_state_fields.py, so it can never go
 * stale relative to BtState.
 */

#include "../ocean/brotato/brotato_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cstddef>

struct BtField { const char* name; size_t off; size_t size; };

/* tests/brotato_state_fields.inc is generated from the struct definition by
 * tests/gen_brotato_state_fields.py; it declares
 *     static const BtField BT_FIELDS[] = { ... };
 *     #define BT_NFIELDS (sizeof(BT_FIELDS) / sizeof(BT_FIELDS[0]))
 * so it can never go stale relative to BtState. */
#include "brotato_state_fields.inc"

/* --------------------------------------------------------------- size mode */

static void report_sizes(void) {
    size_t sum = 0, big = 0;
    printf("=== BtState per-member size report (%d fields) ===\n", BT_NFIELDS);
    printf("sizeof(BtState) = %zu bytes (%.3f MiB)\n", sizeof(BtState),
           (double)sizeof(BtState) / (1024.0 * 1024.0));
    for (int i = 0; i < BT_NFIELDS; i++) sum += BT_FIELDS[i].size;
    printf("sum of member sizes = %zu bytes; padding = %zu bytes\n\n", sum,
           sizeof(BtState) - sum);

    /* Sort by size descending (insertion into a small index array). */
    int idx[BT_NFIELDS];
    for (int i = 0; i < BT_NFIELDS; i++) idx[i] = i;
    for (int i = 1; i < BT_NFIELDS; i++) {
        int k = idx[i], j = i - 1;
        while (j >= 0 && BT_FIELDS[idx[j]].size < BT_FIELDS[k].size) { idx[j + 1] = idx[j]; j--; }
        idx[j + 1] = k;
    }
    printf("%-28s %12s %12s %8s  %s\n", "member", "offset", "bytes", "%state", "elem/array");
    const size_t total = sizeof(BtState);
    for (int n = 0; n < BT_NFIELDS; n++) {
        const BtField* f = &BT_FIELDS[idx[n]];
        if (f->size * 200 < total) continue;  /* skip < 0.5% of state */
        printf("%-28s %12zu %12zu %7.2f%%  %s\n", f->name, f->off, f->size,
               100.0 * (double)f->size / (double)total,
               f->size >= (1u << 20) ? "MB" : "");
        big += f->size;
    }
    printf("(members below 0.5%% of state elided; listed sum %zu = %.1f%%)\n\n",
           big, 100.0 * (double)big / (double)total);

    /* Named composite element costs the compaction work keys off. */
    printf("=== composite element sizes ===\n");
    printf("BtRowVariant          %zu bytes (value,value2,count)\n", sizeof(BtRowVariant));
    printf("BtPending             %zu bytes\n", sizeof(BtPending));
    printf("BtBirth               %zu bytes\n", sizeof(BtBirth));
    printf("BtSummonFx            %zu bytes\n", sizeof(BtSummonFx));
    printf("BtShopItemCopy        %zu bytes\n", sizeof(BtShopItemCopy));
    printf("BtExplosion           %zu bytes\n", sizeof(BtExplosion));
    printf("BtWProj               %zu bytes\n", sizeof(BtWProj));
    printf("BtWProjPool           %zu bytes\n", sizeof(BtWProjPool));
    printf("BtWeaponFxState       %zu bytes\n", sizeof(BtWeaponFxState));
    printf("BtEnemyHitFx          %zu bytes\n", sizeof(BtEnemyHitFx));
    printf("BtPhysicsBody         %zu bytes\n", sizeof(BtPhysicsBody));
    printf("BtPhysContact         %zu bytes\n", sizeof(BtPhysContact));
    printf("BtPhysPair            %zu bytes\n", sizeof(BtPhysPair));
    printf("BtPhysWorld           %zu bytes\n", sizeof(BtPhysWorld));
    printf("BtRng                 %zu bytes\n", sizeof(BtRng));
    printf("BT_POOL_INST_MAX      %d\n", (int)BT_POOL_INST_MAX);
    printf("BT_OBJECT_UID_MAX     %d\n", (int)BT_OBJECT_UID_MAX);
    printf("BT_WPROJ_OBJECT_WORDS %d (=%d bytes)\n", (int)BT_WPROJ_OBJECT_WORDS,
           (int)(BT_WPROJ_OBJECT_WORDS * 4));
    printf("\n");
}

/* ----------------------------------------------------------- occupancy mode */

typedef struct {
    long   exg_rows;          /* rows with exg_count>0 (== n_exg_active)      */
    long   bk_rows;
    long   rowvar_used;       /* sum nvar[]                                  */
    long   rowvar_maxrow;     /* peak nvar[] (deepest single row)            */
    long   var_cs_used;       /* sum nvar[] (var_cs mirrors rowvar dims)     */
    long   pool_alloc;        /* enemy-instance pool high-water               */
    long   pool_free_max;     /* peak reusable instances in a kind stack      */
    long   pool_val_used;     /* pool ids live (pool_kind != free)            */
    long   wproj_live;
    long   hitq_peak;         /* peak deferred hit-pair queue occupancy (pairs a
                               * single frame reports; sizes BT_HITQ_CAP) */
    long   phitq_peak;        /* peak player half (enemy shots reporting the
                               * player in one frame) */
    long   hitq_overflow;     /* runs that latched BT_CAP_HITQ */
    long   pr_live;           /* live enemy/pet projectiles (pr* pool) */
    long   pr_dropped;        /* dropped projectile spawns (proj_dropped) */
    long   expl_resident;
    long   n_dt, n_tsi, n_gwb, n_wtb, n_wcb, n_soh, n_wss, n_mxp, n_scdr, n_tsi2;
    long   n_tsi_peak;
    long   n_dt_peak, n_gwb_peak;
    long   summon_fx;
    long   n_pend_enemy, n_pend_tree, n_pend_boss, n_pend_summons;
    long   n_pend_struct, n_pend_pet, n_births, n_death;
    long   pet_n, matcount, ecount, n_lk;
    long   tick;
    long   level, wave, kills;
    long   phys_pairs, phys_pairs_pet;      /* peak active_pair_count per world */
    long   phys_bodies, phys_bodies_pet;    /* peak body_count per world */
} Occ;

static void occ_observe(Occ* o, const BtState* s) {
    long rv = 0; int mx = 0;
    for (int i = 0; i < BT_NITEM_EFF_ROWS; i++) {
        rv += s->nvar[i];
        if (s->nvar[i] > mx) mx = s->nvar[i];
    }
    o->rowvar_used = rv; o->var_cs_used = rv;
    if (mx > o->rowvar_maxrow) o->rowvar_maxrow = mx;
    int exg = 0, bk = 0, lk = 0;
    for (int i = 0; i < BT_NITEM_EFF_ROWS; i++) {
        if (s->exg_count[i]) exg++;
        if (s->bk_count[i]) bk++;
        if (s->lk_count[i]) lk++;
    }
    o->exg_rows = exg; o->bk_rows = bk; o->n_lk = lk;
    if ((int)s->pool_alloc > o->pool_alloc) o->pool_alloc = s->pool_alloc;
    int pfmax = 0, pfree = 0;
    for (int i = 0; i < BT_NKINDS; i++) {
        if (s->pool_free_n[i] > pfmax) pfmax = s->pool_free_n[i];
        pfree += s->pool_free_n[i];
    }
    o->pool_free_max = pfmax; o->pool_val_used = pfree;
    if ((int)s->wproj.n_live > o->wproj_live) o->wproj_live = s->wproj.n_live;
    /* the hit queue holds this frame's reported pairs until the next flush, so
     * the post-step value IS the occupancy a frame must be able to hold. */
    if ((long)s->hitq.n > o->hitq_peak) o->hitq_peak = (long)s->hitq.n;
    if ((long)s->hitq.np > o->phitq_peak) o->phitq_peak = (long)s->hitq.np;
    if (s->hitq.overflow) o->hitq_overflow = 1;
    {
        int pl = 0;
        for (int i = 0; i < BT_MAX_PROJ; i++) if (s->pralive[i]) pl++;
        if (pl > o->pr_live) o->pr_live = pl;
    }
    if ((long)s->pr_dropped > o->pr_dropped) o->pr_dropped = (long)s->pr_dropped;
    int res = 0;
    for (int i = 0; i < BT_MAX_EXPL; i++) if (s->expl.cache_res[i]) res++;
    if (res > o->expl_resident) o->expl_resident = res;
#define MAXF(f) do { if ((int)s->f > o->f##_peak) o->f##_peak = (int)s->f; } while (0)
    MAXF(n_dt); MAXF(n_tsi); MAXF(n_gwb);
#undef MAXF
    if (s->n_wtb > o->n_wtb) o->n_wtb = s->n_wtb;
    if (s->n_wcb > o->n_wcb) o->n_wcb = s->n_wcb;
    if (s->n_soh > o->n_soh) o->n_soh = s->n_soh;
    if (s->n_wss > o->n_wss) o->n_wss = s->n_wss;
    if (s->n_mxp > o->n_mxp) o->n_mxp = s->n_mxp;
    if (s->n_scdr > o->n_scdr) o->n_scdr = s->n_scdr;
    if (s->n_summon_fx > o->summon_fx) o->summon_fx = s->n_summon_fx;
    if (s->n_pend_enemy > o->n_pend_enemy) o->n_pend_enemy = s->n_pend_enemy;
    if (s->n_pend_tree > o->n_pend_tree) o->n_pend_tree = s->n_pend_tree;
    if (s->n_pend_boss > o->n_pend_boss) o->n_pend_boss = s->n_pend_boss;
    if (s->n_pend_summons > o->n_pend_summons) o->n_pend_summons = s->n_pend_summons;
    if (s->n_pend_struct > o->n_pend_struct) o->n_pend_struct = s->n_pend_struct;
    if (s->n_pend_pet > o->n_pend_pet) o->n_pend_pet = s->n_pend_pet;
    if (s->n_births > o->n_births) o->n_births = s->n_births;
    if (s->n_death_events > o->n_death) o->n_death = s->n_death_events;
    if (s->pet_n > o->pet_n) o->pet_n = s->pet_n;
    if (s->matcount > o->matcount) o->matcount = s->matcount;
    if (s->ecount > o->ecount) o->ecount = s->ecount;
    o->tick = s->ticks; o->level = s->level; o->wave = s->wave;
    if (s->kills > o->kills) o->kills = s->kills;
    if (s->phys.active_pair_count > o->phys_pairs) o->phys_pairs = s->phys.active_pair_count;
    if (s->phys_pet.active_pair_count > o->phys_pairs_pet) o->phys_pairs_pet = s->phys_pet.active_pair_count;
    if (s->phys.body_count > o->phys_bodies) o->phys_bodies = s->phys.body_count;
    if (s->phys_pet.body_count > o->phys_bodies_pet) o->phys_bodies_pet = s->phys_pet.body_count;
}

/* Deterministic xorshift action stream: no RNG draws from the sim itself. */
static unsigned int g_act = 0x1234567u;
static int next_random_action(void) {
    g_act ^= g_act << 13; g_act ^= g_act >> 17; g_act ^= g_act << 5;
    return (int)(g_act % (unsigned)BT_ACTIONS);
}

/* Heuristic pilot: kite away from the nearest enemy (a ranged weapon does the
 * damage), buy every shop slot it can afford, take the first level-up offer.
 * This is only a workload generator for capacity measurement -- it never touches
 * the sim's own RNG. */
static int policy_action(const BtState* s, int stall) {
    if (s->phase == 1) {                     /* shop between waves */
        if (s->shop_timer > 0) return 8;     /* wait out the shop timer */
        if (stall > 8) return 8;             /* nothing affordable left: leave */
        for (int i = 0; i < 4; i++)
            if (s->shop_available & (1u << i)) return i;  /* buy next slot */
        return 8;                            /* nothing left: leave */
    }
    if (s->phase == 2) return 0;             /* level-up: first offer */

    /* combat: move directly away from the nearest live enemy */
    int best = -1; float bestd2 = 1e30f;
    for (int i = 0; i < BT_MAX; i++) {
        if (!s->ealive[i]) continue;
        float dx = s->ex[i] - s->px, dy = s->ey[i] - s->py;
        float d2 = dx * dx + dy * dy;
        if (d2 < bestd2) { bestd2 = d2; best = i; }
    }
    if (best < 0) return 0;
    float ax = s->px - s->ex[best], ay = s->py - s->ey[best];
    float l = bt_sqrtf(ax * ax + ay * ay);
    if (l < 1e-6f) return 0;
    ax /= l; ay /= l;
    int bi = 0; float bd = -2.0f;
    for (int a = 1; a <= 8; a++) {           /* 1..8 are the 8 directions */
        float d = BT_MOVE_X[a] * ax + BT_MOVE_Y[a] * ay;
        if (d > bd) { bd = d; bi = a; }
    }
    return bi;
}

/* Merge a per-run record into a cross-run peak record. */
static void occ_merge(Occ* a, const Occ* b) {
    long* pa = (long*)a; const long* pb = (const long*)b;
    for (size_t k = 0; k < sizeof(Occ) / sizeof(long); k++)
        if (pb[k] > pa[k]) pa[k] = pb[k];
}

static void run_one(unsigned int seed, int steps, int verbose, Occ* acc,
                    unsigned short character, unsigned short weapon, int rnd_policy,
                    int god, int swarm, int deep_wave) {
    static BtState S;
    static float obs[BT_OBS_SIZE];
    float rew = 0.f, term = 0.f;
    bt_core_init(&S, seed, 0, 1, character, weapon);
    /* Capacity measurement at an endless depth: jump straight into the deep
     * wave so the spawn stream (max_enemies scaled by the endless factor) is the
     * one being measured; re-enter it after every episode reset. */
    if (deep_wave > 0) bt_enter_wave(&S, deep_wave);
    Occ o; memset(&o, 0, sizeof(o));
    int i = 0;
    float last_mat = S.materials;
    int stall = 0;
    for (; i < steps; i++) {
        int a = swarm ? (S.phase == 1 ? policy_action(&S, stall) : (S.phase == 2 ? 0 : 0))
                      : (rnd_policy ? next_random_action() : policy_action(&S, stall));
        /* Measurement-only immortality: the run reaches deep waves so the
         * capacity peaks are the ones a long Demon/endless run sees.  This
         * only pokes iframe (no sim RNG is consumed), so the workload is
         * representative even though the trajectory itself is not a real one. */
        if (god) { S.max_hp = 1.0e9f; S.hp = S.max_hp;
                   if (S.iframe <= 0.0f) S.iframe = 1000.0f; }
        bt_core_step(&S, a, obs, &rew, &term);
        /* Re-apply the measurement-only immortality after the step too: a
         * per-frame stat sync can clamp hp back to the wave-40 max. */
        if (god) { S.max_hp = 1.0e9f; S.hp = S.max_hp; }
        if (deep_wave > 0 && S.phase == 0 && S.wave != deep_wave) bt_enter_wave(&S, deep_wave);
        occ_observe(&o, &S);
        if (S.materials == last_mat) stall++; else stall = 0;
        last_mat = S.materials;
    }
    printf("seed %-10u char %-2u wpn %-3u steps %-6d tick %-7d wave %-3d level %-3d kills %-6d done %d phase %d hp %.1f mat %.0f deep %d\n",
           seed, character, weapon, steps, o.tick, o.wave, o.level, o.kills, (int)S.done, (int)S.phase, S.hp, S.materials, deep_wave);
    if (verbose) {
        printf("  rowvar: used=%ld maxrow=%d | exg_rows=%ld bk_rows=%ld lk_rows=%ld\n",
               o.rowvar_used, o.rowvar_maxrow, o.exg_rows, o.bk_rows, o.n_lk);
        printf("  pool:   alloc=%ld maxfree=%ld freetot=%ld | wproj_live=%ld hitq=%ld phitq=%ld pr_live=%ld pr_dropped=%ld expl_res=%ld\n",
               o.pool_alloc, o.pool_free_max, o.pool_val_used, o.wproj_live, o.hitq_peak,
               o.phitq_peak, o.pr_live,
               o.pr_dropped, o.expl_resident);
        printf("  regs:   n_dt=%ld n_tsi=%ld n_gwb=%ld wtb=%ld wcb=%ld soh=%ld wss=%ld mxp=%ld scdr=%ld\n",
               o.n_dt_peak, o.n_tsi_peak, o.n_gwb_peak, o.n_wtb, o.n_wcb, o.n_soh,
               o.n_wss, o.n_mxp, o.n_scdr);
        printf("  queues: sumfx=%ld pend_enemy=%ld pend_struct=%ld pend_pet=%ld births=%ld death=%ld\n",
               o.summon_fx, o.n_pend_enemy, o.n_pend_struct, o.n_pend_pet, o.n_births, o.n_death);
        printf("  live:   pet_n=%ld matcount=%ld ecount=%ld\n", o.pet_n, o.matcount, o.ecount);
    }
    if (acc) occ_merge(acc, &o);
}

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "all";
    int nruns = argc > 2 ? atoi(argv[2]) : 3;
    int steps = argc > 3 ? atoi(argv[3]) : 120000;

    if (!strcmp(mode, "sizes")) { report_sizes(); return 0; }
    report_sizes();
    unsigned short wpn = (unsigned short)bt_weapon_index_from_id("weapon_wand_1");
    if ((int)wpn == (int)BT_WEAPON_NONE) wpn = BT_WEAPON_NONE;
    printf("=== live peak-occupancy over %d runs x %d steps (Demon+wpn%u kite; then random) ===\n",
           nruns, steps, (unsigned)wpn);
    Occ acc; memset(&acc, 0, sizeof(acc));
    for (int r = 0; r < nruns; r++)
        run_one(1000u + 7919u * (unsigned)r, steps, 1, &acc, BT_CHARACTER_DEMON, wpn, 0, 1, 0, 0);
    /* Endless-depth capacity runs: waves 40/50/60 (the pr* peak that sizes
     * BT_MAX_PROJ). */
    if (!strcmp(mode, "all") || !strcmp(mode, "deep")) {
        int deeps[3] = { 40, 50, 60 };
        for (int d = 0; d < 3; d++)
            for (int r = 0; r < 2; r++)
                run_one(20000u + 101u * (unsigned)d + 7u * (unsigned)r, steps / 4, 1,
                        &acc, BT_CHARACTER_DEMON, wpn, 0, 1, 0, deeps[d]);
    }
    /* A few random-action runs for contrast (they die early, low occupancy). */
    for (int r = 0; r < 2; r++)
        run_one(555u + 7u * (unsigned)r, steps / 4, 0, &acc, BT_CHARACTER_BULL, BT_WEAPON_NONE, 1, 0, 0, 0);
    for (int r = 0; r < 2; r++)
        run_one(9001u + 13u * (unsigned)r, steps, 1, &acc, BT_CHARACTER_DEMON, wpn, 0, 1, 1, 0);
    printf("\n--- merged peak across all runs ---\n");
    printf("  rowvar: used=%ld maxrow=%d (of %d) | exg_rows=%ld bk_rows=%ld lk_rows=%ld\n",
           acc.rowvar_used, (int)acc.rowvar_maxrow, BT_MAX_VARIANTS,
           acc.exg_rows, acc.bk_rows, acc.n_lk);
    printf("  pool:   alloc=%ld maxfree=%ld live=%ld (of %d) | wproj_live=%ld (of %d) hitq_peak=%ld (of %d, overflow %ld) phitq_peak=%ld expl_res=%ld\n",
           acc.pool_alloc, acc.pool_free_max, acc.pool_val_used, BT_POOL_INST_MAX,
           acc.wproj_live, BT_MAX_WPROJ, acc.hitq_peak, BT_HITQ_CAP, acc.hitq_overflow,
           acc.phitq_peak, acc.expl_resident);
    printf("  proj:   pr_live=%ld (of %d) pr_dropped=%ld | expl_live=%ld (of %d)\n",
           acc.pr_live, BT_MAX_PROJ, acc.pr_dropped, acc.expl_resident, BT_MAX_EXPL);
    printf("  regs:   n_dt=%ld n_tsi=%ld n_gwb=%ld wtb=%ld wcb=%ld soh=%ld wss=%ld mxp=%ld scdr=%ld\n",
           acc.n_dt_peak, acc.n_tsi_peak, acc.n_gwb_peak, acc.n_wtb, acc.n_wcb,
           acc.n_soh, acc.n_wss, acc.n_mxp, acc.n_scdr);
    printf("  queues: sumfx=%ld pend_enemy=%ld pend_struct=%ld pend_pet=%ld births=%ld death=%ld\n",
           acc.summon_fx, acc.n_pend_enemy, acc.n_pend_struct, acc.n_pend_pet,
           acc.n_births, acc.n_death);
    printf("  live:   pet_n=%ld matcount=%ld ecount=%ld kills=%ld level=%ld wave=%ld\n",
           acc.pet_n, acc.matcount, acc.ecount, acc.kills, acc.level, acc.wave);
    printf("  phys:   enemy_world pairs=%ld bodies=%ld | pet_world pairs=%ld bodies=%ld (cap %d)\n",
           acc.phys_pairs, acc.phys_bodies, acc.phys_pairs_pet, acc.phys_bodies_pet,
           BT_PHYS_PAIR_CAPACITY);
    return 0;
}
