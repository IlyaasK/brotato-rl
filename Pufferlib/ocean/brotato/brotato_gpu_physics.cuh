// brotato_gpu_physics.cuh -- intra-environment parallel contact physics for the
// Brotato CUDA backend. OWNER: GpuSolver.
//
// WHAT THIS IS
//   The whole Step2DSW::step pipeline (bt_phys_step in brotato_physics.h) run
//   with several lanes cooperating on ONE environment: bodies in parallel, the
//   O(n^2) broadphase scan in parallel, an order-preserving wavefront (DAG
//   levelization) for setup_pairs + the 8 Gauss-Seidel iterations, and per-body
//   integrate_velocities.  Every per-object mutation the serial step performs
//   happens in the same order, through the very same BT_HD functions
//   (bt_phys_body_pair_live / bt_phys_pair_retire / bt_phys_pair_release /
//   bt_phys_pool_claim / bt_phys_pair_setup / bt_phys_pair_solve /
//   bt_phys_body_begin_step / bt_phys_body_integrate_velocities), so the result
//   is byte-identical.
//
// WHY IT IS BIT-EXACT
//   * begin_step / integrate_velocities write only their own body.
//   * the broadphase scan is a pure, order-free classification pass over the
//     same candidate set, followed by the serial per-pair work applied by the
//     group's member 0 in ascending pair-index order: retire the previous live
//     set, then claim a slot for each live pair / release the slot of each dead
//     pair that still holds one.  The append to active_pairs and the LIFO pool
//     slot allocation therefore reproduce the serial walk exactly (an arbitrary
//     slot order would permute pairs[]/active_pairs[] bytes).
//   * setup/solve write the two bodies of the pair and the pair object only.
//     Level(op) = 1 + max(last[a], last[b]) over the pair's MUTABLE bodies
//     (inv_mass != 0), assigned by walking the exact serial order, gives a
//     schedule in which each body's ops run in ascending pair-index order: two
//     ops sharing a mutable body land on strictly increasing levels and, inside
//     one level, no two ops share a body (any order is then free).  Float
//     accumulation per body is therefore the serial accumulation order.  An
//     immutable body (inv_mass == 0: the four STATIC arena walls) never changes
//     value -- apply_impulse adds j*inv_mass == +/-0.0f and its vel/bias_vel
//     start at +0.0f -- so it contributes no edge.
//
//                         LANE GROUP
//   Every function takes a lane group (mem = BTF_MEM(bp), slot = BTF_SLOT,
//   bp = group size) exactly like brotato_gpu_framework.cuh, so it works for
//   bp == 32 (warp per env, cheap __syncwarp barriers, high env residency) and
//   bp == blockDim (block per env, more lanes per env, block-barrier per level).
//
//                         SCRATCH (caller-provided)
//   BtgpScratch is carved from caller memory (dynamic shared OR the global
//   arena), so nothing here declares static __shared__:
//       size_t nb = btgp_scratch_bytes();
//       BtgpScratch sc = btgp_scratch_carve(ptr);   /* 8-byte aligned */
//   A block running several envs (warp-per-env) gives warp w the region
//   [ptr + w*nb, ptr + (w+1)*nb): btf_phys_wavefront() does that slicing.  The
//   two BtPhysWorlds of one tick (enemy + pet) can share one region because
//   their bt_phys_step calls are sequential.
//
//                         DISPATCH
//   Including this header installs the parallel backend as the framework's
//   physics hook (define BTGP_KEEP_MEASUREMENT_HOOK to keep the stage select).
//   The driving step kernel must set btf_cur_state / btf_scratch / btf_bp before
//   each bt_core_step; on the host the dispatcher runs the serial stages, so the
//   CPU reference is never affected.

#ifndef PUFFER_BROTATO_GPU_PHYSICS_CUH
#define PUFFER_BROTATO_GPU_PHYSICS_CUH

#ifndef BTGP_KEEP_MEASUREMENT_HOOK
#ifndef BTF_PHYS_HOOK_BODY
#define BTF_PHYS_HOOK_BODY(w) btgp_phys_dispatch(w, btf_cur_state, btf_scratch, btf_bp)
#endif
#endif

#include "brotato_gpu_framework.cuh"

// ---------------------------------------------------------------------------
// scratch layout (see btgp_scratch_bytes / btgp_scratch_carve)
//
//   cls[np]     u8   candidate class 0 none / 1 live+slot / 2 live,needs slot /
//                    3 dead but still holding a slot            (np = row_start(N))
//   events[np]  u16  the candidates with cls != 0, ASCENDING pair index
//   level[P]    u16  op (active-set position) -> wavefront level
//   order[P]    u16  level-bucketed op positions
//   last[N]     int  per-body last assigned level (level build)
//   beg[cap+2]  int  per-level bucket start
//   fill[cap+2] int  per-level bucket cursor
//   meta[8]     int  [0] n, [1] prev live count, [2] unused, [3] critical path,
//                    [4] 1 = bucket cap exceeded (rescan fallback in use)
//   scan             the framework's group exclusive scan scratch
//
// The level is bounded by the live op count, which the pool caps at
// BT_PHYS_PAIR_POOL, so beg/fill are sized BTGP_LEVEL_CAP + 2 where the cap is
// 2*BT_PHYS_MAX_BODIES + 2 (the complete pair list of N bodies peaks at 2N - 3
// levels).  A build that ever exceeded it falls back to the framework's
// per-level rescan runner, which needs level[] only; that is the same schedule
// (bit-exact) and is reported through meta[4].
// ---------------------------------------------------------------------------
/* The greedy level is bounded by the live op count, and the complete pair list
 * of N bodies peaks at 2N - 3 levels, so this cap is never reached in practice:
 * a level build that did exceed it falls back to the framework's per-level
 * rescan runner, which needs level[] only (same schedule, bit-exact).  Define
 * BTGP_LEVEL_CAP lower (e.g. -DBTGP_LEVEL_CAP=4) to force that path and test it. */
#ifndef BTGP_LEVEL_CAP
#define BTGP_LEVEL_CAP (2 * BT_PHYS_MAX_BODIES + 2)
#endif

/* Candidate classes: the liveness predicate plus the pool-slot state, because
 * the serial scan's two branches act on both.  A live pair claims (or keeps) a
 * slot; a dead pair that still holds one releases it, in that order inside the
 * ascending scan, so the LIFO free stack sees the same sequence. */
#define BTGP_CLS_NONE  0  /* not live, no slot: nothing to do                    */
#define BTGP_CLS_KEEP  1  /* live, already holds a slot                          */
#define BTGP_CLS_ALLOC 2  /* live, no slot: claim one                            */
#define BTGP_CLS_FREE  3  /* not live but still holding a slot: release it       */

typedef struct BtgpScratch {
    unsigned char *cls;
    unsigned short *events;
    unsigned short *level;
    unsigned short *order;
    int *last;
    int *beg;
    int *fill;
    int *meta;
    BtfScanScratch *scan;
} BtgpScratch;

/* The broadphase candidate space is exactly the serial scan's set
 * {(i,j) : i < j < n}, enumerated in lexicographic (i,j) order.  For a fixed n
 * that order is the ascending pair-index order, because bt_phys_pair_index is
 * row-major with row i strictly above row i-1 and increasing inside a row.  Row
 * i therefore holds (n-i-1) entries starting at
 *     btgp_scan_row_start(i, n) = i*(2n-i-1)/2
 * and the whole set is the contiguous scan space [0, C(n,2)).
 *
 * NOTE this is NOT the flat pair-index range [0, row_start(i, BT_PHYS_MAX_
 * BODIES)): that range also covers (i,j) with j >= n (the row is as wide as the
 * body CAPACITY), which the serial scan never visits.  Iterating it would
 * classify and allocate cache slots for pairs the serial walk ignores. */
BT_HD static inline int btgp_scan_row_start(int i, int n) {
    return (i * (2 * n - i - 1)) / 2;
}

BT_HD static inline int btgp_scan_space(int n) {
    return (n > 1) ? (n * (n - 1)) / 2 : 0;
}

BT_HD static inline size_t btgp_align_up(size_t v, size_t a) {
    return (v + a - 1) & ~(a - 1);
}

BT_HD static inline size_t btgp_scratch_bytes(void) {
    size_t np = (size_t)btgp_scan_space(BT_PHYS_MAX_BODIES);
    size_t P = (size_t)BT_PHYS_PAIR_POOL;
    size_t b = np * sizeof(unsigned char);
    b = btgp_align_up(b, 2) + np * sizeof(unsigned short);
    b = btgp_align_up(b, 2) + P * sizeof(unsigned short) * 2;
    b = btgp_align_up(b, 4) + (size_t)BT_PHYS_MAX_BODIES * sizeof(int);
    b = btgp_align_up(b, 4) + (size_t)(BTGP_LEVEL_CAP + 2) * sizeof(int) * 2;
    b = btgp_align_up(b, 4) + 8 * sizeof(int);
    b = btgp_align_up(b, 4) + sizeof(BtfScanScratch);
    return btgp_align_up(b, 16) + 64; /* carve does aligned steps; keep slack */
}

/* Carve the layout above out of an 8-byte-aligned region. */
BT_HD static inline BtgpScratch btgp_scratch_carve(void *base) {
    BtgpScratch s;
    size_t off = 0;
    size_t np = (size_t)btgp_scan_space(BT_PHYS_MAX_BODIES);
    size_t P = (size_t)BT_PHYS_PAIR_POOL;
    char *p = (char *)base;
    s.cls = (unsigned char *)(p + off); off = btgp_align_up(off + np, 2);
    s.events = (unsigned short *)(p + off); off = btgp_align_up(off + np * 2, 2);
    s.level = (unsigned short *)(p + off); off = btgp_align_up(off + P * 2, 2);
    s.order = (unsigned short *)(p + off); off = btgp_align_up(off + P * 2, 4);
    s.last = (int *)(p + off); off = btgp_align_up(off + (size_t)BT_PHYS_MAX_BODIES * 4, 4);
    s.beg = (int *)(p + off); off = btgp_align_up(off + (size_t)(BTGP_LEVEL_CAP + 2) * 4, 4);
    s.fill = (int *)(p + off); off = btgp_align_up(off + (size_t)(BTGP_LEVEL_CAP + 2) * 4, 4);
    s.meta = (int *)(p + off); off = btgp_align_up(off + 8 * 4, 4);
    s.scan = (BtfScanScratch *)(p + btgp_align_up(off, 4));
    return s;
}

/* Measurement-only schedule switch: 0 = level-bucketed lists (default),
 * 1 = the framework's per-level rescan.  Both are bit-exact; only the schedule
 * differs, so a harness can compare them on identical worlds. */
__device__ int btgp_wave_mode = 0;

/* Measurement-only phase selector, in the spirit of the framework's
 * BTF_PHYS_* staging: 1 = begin_step, 2 = broadphase, 4 = setup+solve,
 * 8 = integrate.  ONLY the default 0xF is a faithful simulation; the other masks
 * exist so a harness can attribute cost between the phases (a masked run is a
 * deliberately wrong simulation and is never used for parity).  Skipping a phase
 * leaves its world state as the previous step left it, so e.g. 0xD (no
 * broadphase) keeps the live set frozen. */
__device__ int btgp_phase_mask = 0xF;

// ---------------------------------------------------------------------------
// broadphase
// ---------------------------------------------------------------------------

/* Row of scan position k, advancing a per-lane hint (monotone in k). */
BT_HD static inline int btgp_pair_row(int k, int *hint, int n) {
    int i = *hint;
    while (i + 1 < n && k >= btgp_scan_row_start(i + 1, n)) i++;
    *hint = i;
    return i;
}

/* Pure candidate classifier: the serial scan's liveness predicate plus the
 * pool-slot state, evaluated in any order.  `ra` caches bt_phys_shape_aabb(a)
 * across a row walk.  `k` is the scan position; the pair's own index (the
 * pair_map key, and what active_pairs records) is bt_phys_pair_index(i, j). */
BT_HD static inline int btgp_classify(BtPhysWorld *w, int k, int *hint, int *cache_i,
                                      BtPhysRect *ra, int n) {
    int i = btgp_pair_row(k, hint, n);
    int j = i + 1 + (k - btgp_scan_row_start(i, n));
    int slot, live;
    if (i != *cache_i) {
        *cache_i = i;
        *ra = bt_phys_shape_aabb(w->bodies[i], w->bodies[i].pos);
    }
    live = bt_phys_body_pair_live(w, i, j, *ra);
    slot = (int)w->pair_map[bt_phys_pair_index(i, j)];
    if (live) return slot == (int)BT_PHYS_PAIR_FREE ? BTGP_CLS_ALLOC : BTGP_CLS_KEEP;
    return slot == (int)BT_PHYS_PAIR_FREE ? BTGP_CLS_NONE : BTGP_CLS_FREE;
}

/* Ordered compaction of cls[] != 0 over contiguous group chunks: the events
 * come out in ascending pair-index order (framework section 2 contract).
 * Returns the event count. */
BT_HD static inline int btgp_compact_events(BtgpScratch *sc, int np, int mem, int slot,
                                            int bp) {
    int chunk = (np + bp - 1) / bp;
    int lo = mem * chunk, hi = lo + chunk;
    int k, cnt = 0, base, pos;
    if (hi > np) hi = np;
    for (k = lo; k < hi; k++) cnt += sc->cls[k] != BTGP_CLS_NONE;
    base = btf_scan_excl(cnt, sc->scan, mem, slot, bp);
    pos = base;
    for (k = lo; k < hi; k++)
        if (sc->cls[k] != BTGP_CLS_NONE) sc->events[pos++] = (unsigned short)k;
    btf_group_sync(bp);
    return btf_scan_total(sc->scan, bp, slot);
}

/* Apply the classification exactly as bt_phys_update_pairs does: first retire
 * the previous live set (out-of-range slots are released now, in the previous
 * list's order, which is the order the LIFO free stack expects; in-range pairs
 * are only marked unused for this scan), then walk the classified pairs in
 * ascending pair-index order, claiming a slot for each live pair and releasing
 * the slot of each dead pair that still holds one.  `nev` entries of events[]
 * are exactly the pairs with a non-zero class, already ordered. */
BT_HD static inline void btgp_broadphase_walk(BtPhysWorld *w, BtgpScratch *sc, int n,
                                              int prev_count, int nev) {
    int hint = 0, e, k;
    for (k = 0; k < prev_count; k++)
        bt_phys_pair_retire(w, w->active_pairs[k], n);
    w->active_pair_count = 0;
    for (e = 0; e < nev; e++) {
        int k2 = (int)sc->events[e];
        int i = btgp_pair_row(k2, &hint, n);
        int j = i + 1 + (k2 - btgp_scan_row_start(i, n));
        int pi = bt_phys_pair_index(i, j);
        if (sc->cls[k2] == BTGP_CLS_FREE) bt_phys_pair_release(w, pi);
        else bt_phys_pool_claim(w, i, j, pi);
    }
}

// ---------------------------------------------------------------------------
// wavefront
// ---------------------------------------------------------------------------

/* Level-bucketed runner: level L is exactly order[beg[L] .. beg[L+1]).  One
 * group barrier per level, which is what makes level L+1 observe every body
 * write of level L.  Any order inside a level is legal (body-disjoint ops). */
template <typename ExecFn>
BT_HD static inline void btgp_wavefront_run_bucketed(const BtgpScratch *sc, int nlevels,
                                                     int mem, int bp, ExecFn exec) {
    for (int L = 1; L <= nlevels; L++) {
        int b0 = sc->beg[L], b1 = sc->beg[L + 1];
        for (int u = b0 + mem; u < b1; u += bp) exec((int)sc->order[u]);
        btf_group_sync(bp);
    }
}

/* Greedy level build over the live set, walked in the exact serial order:
 *   level(k) = 1 + max(last[a], last[b])   over mutable bodies only
 * plus the bucket lists for the level-by-level pass.  Serial by group member 0
 * (the recurrence is sequential by construction); the live set is walked once.
 * The live set is pool-bounded, so this pass is small. */
BT_HD static inline void btgp_level_build(BtPhysWorld *w, BtgpScratch *sc, int n,
                                          int mem, int bp) {
    int m = w->active_pair_count;
    int i, k;
    if (mem == 0) {
        int mx = 0;
        for (i = 0; i <= BTGP_LEVEL_CAP + 1; i++) sc->beg[i] = 0;
        for (i = 0; i < n; i++) sc->last[i] = 0;
        for (k = 0; k < m; k++) {
            BtPhysPair *p = &w->pairs[w->active_pairs[k]];
            int a = p->a, b = p->b;
            int fa = w->bodies[a].inv_mass != 0.0f;
            int fb = w->bodies[b].inv_mass != 0.0f;
            int la = fa ? sc->last[a] : 0;
            int lb = fb ? sc->last[b] : 0;
            int L = (la > lb ? la : lb) + 1;
            sc->level[k] = (unsigned short)L;
            if (fa) sc->last[a] = L;
            if (fb) sc->last[b] = L;
            if (L > mx) mx = L;
            if (L <= BTGP_LEVEL_CAP) sc->beg[L]++;
        }
        sc->meta[3] = mx;
        sc->meta[4] = mx > BTGP_LEVEL_CAP ? 1 : 0;
        if (mx <= BTGP_LEVEL_CAP) {
            int acc = 0;
            for (i = 0; i <= mx + 1; i++) {
                int c = sc->beg[i];
                sc->beg[i] = acc;
                sc->fill[i] = acc;
                acc += c;
            }
        }
        sc->meta[2] = m;
    }
    btf_group_sync(bp);
    if (!sc->meta[4]) {
        BTF_FOR(k, m, mem, bp) {
            int L = sc->level[k];
#if defined(__CUDA_ARCH__)
            int pos = atomicAdd(&sc->fill[L], 1);
#else
            int pos = sc->fill[L]++;
#endif
            sc->order[pos] = (unsigned short)k;
        }
        btf_group_sync(bp);
    }
}

// ---------------------------------------------------------------------------
// one full bt_phys_step, parallel
// ---------------------------------------------------------------------------
BT_HD static inline void btf_phys_wavefront_world(BtPhysWorld *w, BtgpScratch *sc,
                                                  int mem, int slot, int bp) {
#if !defined(__CUDA_ARCH__)
    btf_phys_stage_select(w); /* host: the serial reference */
    (void)sc; (void)mem; (void)slot; (void)bp;
#else
    int n, np, i, m, nlevels, it, prev_count, nev, hint, cache_i;
    int pmask = btgp_phase_mask;
    BtPhysRect ra;

    /* ---- begin_step: per body, order-free ---- */
    n = w->body_count > BT_PHYS_MAX_BODIES ? BT_PHYS_MAX_BODIES : w->body_count;
    if (pmask & 1) {
        BTF_FOR(i, n, mem, bp) {
            BtPhysicsBody *b = &w->bodies[i];
            if (b->active) bt_phys_body_begin_step(b);
        }
    }
    btf_group_sync(bp);

    /* ---- broadphase ---- */
    if (mem == 0) {
        sc->meta[0] = bt_phys_scan_body_count(w);
        sc->meta[1] = w->active_pair_count;
    }
    btf_group_sync(bp);
    n = sc->meta[0];
    prev_count = sc->meta[1];
    np = btgp_scan_space(n);
    hint = 0; cache_i = -1; ra = bt_phys_rect(0.0f, 0.0f, 0.0f, 0.0f);
    if (pmask & 2) {
        BTF_FOR(i, np, mem, bp)
            sc->cls[i] = (unsigned char)btgp_classify(w, i, &hint, &cache_i, &ra, n);
        btf_group_sync(bp);
        nev = btgp_compact_events(sc, np, mem, slot, bp);
        if (mem == 0) btgp_broadphase_walk(w, sc, n, prev_count, nev);
    }
    btf_group_sync(bp);

    /* ---- level build + buckets (once per step: the topology is fixed for
     * setup and all 8 solve iterations) ---- */
    m = w->active_pair_count;
    nlevels = 0;
    if (m > 0) {
        btgp_level_build(w, sc, n, mem, bp);
        nlevels = sc->meta[3];
    } else if (mem == 0) {
        sc->meta[3] = 0;
        sc->meta[4] = 0;
    }
    btf_group_sync(bp);

    /* ---- setup + solve, level by level ---- */
    if (pmask & 4) {
        if (sc->meta[4] || btgp_wave_mode != 0) {
            btf_wavefront_run(sc->level, m, nlevels, mem, bp,
                [&](int k) { bt_phys_pair_setup(w, &w->pairs[w->active_pairs[k]]); });
            for (it = 0; it < w->iterations; it++)
                btf_wavefront_run(sc->level, m, nlevels, mem, bp,
                    [&](int k) { bt_phys_pair_solve(w, &w->pairs[w->active_pairs[k]]); });
        } else {
            btgp_wavefront_run_bucketed(sc, nlevels, mem, bp,
                [&](int k) { bt_phys_pair_setup(w, &w->pairs[w->active_pairs[k]]); });
            for (it = 0; it < w->iterations; it++)
                btgp_wavefront_run_bucketed(sc, nlevels, mem, bp,
                    [&](int k) { bt_phys_pair_solve(w, &w->pairs[w->active_pairs[k]]); });
        }
    }

    /* ---- integrate_velocities: per body, order-free ---- */
    if (pmask & 8) {
        BTF_FOR(i, n, mem, bp) {
            BtPhysicsBody *b = &w->bodies[i];
            if (b->active) bt_phys_body_integrate_velocities(b, w->step);
        }
    }
#endif
}

// ---------------------------------------------------------------------------
// BtState-level entry points + the framework hook body
// ---------------------------------------------------------------------------
BT_HD static inline BtgpScratch btgp_block_scratch(void *block_scratch, int bp) {
    char *base = (char *)block_scratch;
#if defined(__CUDA_ARCH__)
    if (bp <= 32) base += (size_t)BTF_SLOT * btgp_scratch_bytes();
#else
    (void)bp;
#endif
    return btgp_scratch_carve(base);
}

/* Step &s->phys. */
BT_HD static inline void btf_phys_wavefront(BtState *s, void *block_scratch, int bp) {
#if defined(__CUDA_ARCH__)
    BtgpScratch sc = btgp_block_scratch(block_scratch, bp);
    btf_phys_wavefront_world(&s->phys, &sc, BTF_MEM(bp), BTF_SLOT, bp);
#else
    (void)s; (void)block_scratch; (void)bp;
#endif
}

/* Step BT_PET_WORLD(s) (its own BtPhysWorld; same schedule, sequential call
 * site, so it reuses the same scratch region). */
BT_HD static inline void btf_phys_pet_wavefront(BtState *s, void *block_scratch, int bp) {
#if defined(__CUDA_ARCH__)
    BtgpScratch sc = btgp_block_scratch(block_scratch, bp);
    btf_phys_wavefront_world(BT_PET_WORLD(s), &sc, BTF_MEM(bp), BTF_SLOT, bp);
#else
    (void)s; (void)block_scratch; (void)bp;
#endif
}

/* The framework's physics hook body.  bt_core_step calls it once per world, and
 * the two call sites are at different points of the tick, so they stay
 * separate.  On the host (and with a null state) it runs the serial stages. */
BT_HD static inline void btgp_phys_dispatch(BtPhysWorld *w, BtState *s, void *scratch,
                                            int bp) {
#if defined(__CUDA_ARCH__)
    if (s != 0 && w == &s->phys) btf_phys_wavefront(s, scratch, bp);
    else if (s != 0) btf_phys_pet_wavefront(s, scratch, bp);
    else btf_phys_stage_select(w);
#else
    (void)s; (void)scratch; (void)bp;
    btf_phys_stage_select(w);
#endif
}

/* The framework's RUNTIME backend dispatch (compile the consumer with
 * -DBTF_PHYS_HAVE_BACKEND=1): the hook body routes the two world call sites here
 * when btf_phys_backend != 0, so one binary can A/B the serial stages against
 * this schedule on identical worlds.  The framework declares these two.
 * A driver that already provides them (ocean/brotato/brotato_gpu_warp.cuh
 * defines BTF_PHYS_BACKEND_WRAPPERS for exactly that) keeps its own. */
#if BTF_PHYS_HAVE_BACKEND && !defined(BTF_PHYS_BACKEND_WRAPPERS)
BT_HD static inline void btf_phys_backend_main(BtState *s, void *scratch, int bp) {
    btf_phys_wavefront(s, scratch, bp);
}
BT_HD static inline void btf_phys_backend_pet(BtState *s, void *scratch, int bp) {
    btf_phys_pet_wavefront(s, scratch, bp);
}
#endif

#endif // PUFFER_BROTATO_GPU_PHYSICS_CUH
