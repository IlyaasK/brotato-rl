/* brotato_capacity_host.h -- HOST-ONLY diagnostic for a tripped capacity bound.
 *
 * Separate from brotato_capacity.h so the freestanding core (and the CUDA/Metal
 * translations) never pull in stdio/stdlib.  Include it from a host wrapper or a
 * test, never from brotato_core.h.
 *
 * Policy: an implementation-capacity loss fails CLOSED.  There is no
 * "continue with a truncated environment" path, so a trainer can never learn on
 * a rollout the source could not have produced.
 */
#pragma once

#include "brotato_capacity.h"

#include <stdio.h>
#include <stdlib.h>

BT_HOST_ONLY(static const char* bt_capacity_name(int bit)) {
    switch (bit) {
#define BT_CAP_CASE(name, expr) case BT_CAP_ ## name: return #name;
    BT_CAPACITY_LOSSES(BT_CAP_CASE)
#undef BT_CAP_CASE
    case BT_CAP_WPROJ_SET: return "WPROJ_SET";
    case BT_CAP_EXPL_SET:  return "EXPL_SET";
    default: return "?";
    }
}

/* Human-readable status for one env, e.g.
 * "mask=0x3 first=DROP_CONS seed=3 wave=61 tick=3612".  Returns buf. */
BT_HOST_ONLY(static const char* bt_capacity_describe(const BtState* s, char* buf, int n)) {
    unsigned long long m = bt_capacity_mask(s);
    int first = m ? __builtin_ctzll(m) : -1;
    snprintf(buf, (size_t)n, "mask=0x%llx first=%s seed=%llu wave=%d tick=%d",
             m, first < 0 ? "none" : bt_capacity_name(first),
             (unsigned long long)s->seed, s->wave, s->ticks);
    return buf;
}

/* Fail closed: when any implementation-capacity loss is live, print a
 * diagnostic naming the pool, the seed, the wave and the tick, then abort.  A
 * clean env returns immediately. */
BT_HOST_ONLY(static void bt_capacity_require_clean(const BtState* s, const char* where)) {
    unsigned long long m = bt_capacity_mask(s);
    if (m == 0ull) return;
    char buf[192];
    fprintf(stderr,
            "brotato: IMPLEMENTATION CAPACITY LOSS after %s: %s\n"
            "brotato: this environment cannot be trained on (a source-unbounded pool "
            "was exhausted); raising the bound is the fix.\n",
            where ? where : "?", bt_capacity_describe(s, buf, (int)sizeof(buf)));
    fflush(stderr);
    abort();
}
