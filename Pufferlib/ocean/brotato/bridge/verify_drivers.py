#!/usr/bin/env python3
"""Verify the bridge's v4 progression drivers from a recorded trace.

Each shop / level-up tick writes `shop_state` or `level_up_state` (mod_main.gd
_drive_shop/_drive_level_up): the requested action, the driver's `kind`, whether
the game's own handler took effect (`applied`), the verifier's reason, and the
post-action state the verifier snapshotted.  This tool reads those rows and
reports, per action id, whether the driver ever applied in the game, together
with the state delta the row shows (locked count, ban tokens, reroll count,
weapon / item count, gold) so the game-side effect can be compared with the
sim's own trace.

It also checks the two invariants the trace must satisfy for the run to be a
valid parity input:

  * every row is a complete observation (`obs_complete == 1`) -- a crate-open
    level-up row used to be marked incomplete;
  * `floating_text_nodes == 0` on every row -- the effect-only floating-text
    draw sites stayed off (rl_obs/mod_main pin the manager).

Exit status: 0 = every required driver id applied at least once (as set by
--require or the default 21..30) and both invariants hold; 1 otherwise.
"""

import argparse
import json
import sys
from collections import OrderedDict

SHOP_KINDS = {
    0: "buy_slot0", 1: "buy_slot1", 2: "buy_slot2", 3: "buy_slot3",
    4: "reroll", 8: "skip",
    9: "recycle0", 10: "recycle1", 11: "recycle2", 12: "recycle3", 13: "recycle4", 14: "recycle5",
    15: "combine0", 16: "combine1", 17: "combine2", 18: "combine3", 19: "combine4", 20: "combine5",
    21: "lock0", 22: "lock1", 23: "lock2", 24: "lock3",
    25: "ban0", 26: "ban1", 27: "ban2", 28: "ban3",
    29: "crate_take", 30: "crate_recycle",
}
DEFAULT_REQUIRED = list(range(21, 31))
STATE_KEYS = ("gold", "locked_count", "banned_count", "ban_tokens",
              "weapon_count", "item_count", "max_hp", "level", "pending", "offers")


def load_rows(path):
    rows = []
    with open(path, "r", encoding="utf-8") as handle:
        for number, line in enumerate(handle, 1):
            line = line.strip()
            if not line:
                continue
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError as exc:
                raise SystemExit("%s:%d: %s" % (path, number, exc))
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace")
    parser.add_argument("--require", default="",
                        help="comma-separated action ids that must have applied "
                             "at least once (default 21..30)")
    parser.add_argument("--no-require", action="store_true",
                        help="report only; do not fail on unexercised ids")
    parser.add_argument("--max-errors", type=int, default=25,
                        help="how many rejected attempts to print per action")
    args = parser.parse_args()

    rows = load_rows(args.trace)
    required = ([int(tok) for tok in args.require.split(",") if tok.strip()]
                if args.require else DEFAULT_REQUIRED)

    attempts = OrderedDict()   # action -> dict(count, applied, kinds, errors, deltas)
    incomplete = []
    floating = []
    for row in rows:
        tick = row.get("tick")
        if row.get("obs_complete") not in (1, True):
            incomplete.append(tick)
        if row.get("floating_text_nodes") not in (0, None):
            floating.append((tick, row.get("floating_text_nodes")))
        for field, phase in (("shop_state", "shop"), ("level_up_state", "level_up")):
            state = row.get(field)
            if not isinstance(state, dict):
                continue
            action = int(state.get("action", -1))
            entry = attempts.setdefault(action, {
                "count": 0, "applied": 0, "phase": phase, "kinds": [],
                "errors": [], "delta": None, "ticks": [],
            })
            entry["count"] += 1
            entry["ticks"].append(tick)
            kind = str(state.get("kind", ""))
            if kind and kind not in entry["kinds"]:
                entry["kinds"].append(kind)
            if int(state.get("applied", 0)) == 1:
                entry["applied"] += 1
                if entry["delta"] is None:
                    after = state.get("after") or {}
                    entry["delta"] = {k: after[k] for k in STATE_KEYS if k in after}
            else:
                reason = str(state.get("error", "")) or str(state.get("verify_error", ""))
                if len(entry["errors"]) < args.max_errors:
                    entry["errors"].append((tick, reason))

    print("trace: %s (%d rows)" % (args.trace, len(rows)))
    print("%-6s %-14s %-7s %-7s %-20s %s"
          % ("action", "name", "tries", "applied", "kinds", "state after first applied"))
    missing = []
    for action in sorted(attempts):
        entry = attempts[action]
        name = SHOP_KINDS.get(action, "level_up_%d" % action)
        print("%-6d %-14s %-7d %-7d %-20s %s" % (
            action, name, entry["count"], entry["applied"],
            ",".join(entry["kinds"]), json.dumps(entry["delta"]) if entry["delta"] else ""))

    print()
    for action in sorted(attempts):
        entry = attempts[action]
        if entry["applied"]:
            continue
        name = SHOP_KINDS.get(action, "level_up_%d" % action)
        print("REJECTED %d (%s): %d attempt(s), first reason(s):" % (action, name, entry["count"]))
        for tick, reason in entry["errors"][:6]:
            print("    tick %s: %s" % (tick, reason))

    print()
    print("rows with obs_complete != 1 : %d %s" % (len(incomplete), incomplete[:10]))
    print("rows with floating_text_nodes != 0 : %d %s" % (len(floating), floating[:10]))
    if not args.no_require:
        for action in required:
            entry = attempts.get(action)
            if entry is None or entry["applied"] == 0:
                missing.append(action)
        print("required ids never applied: %s" % (missing if missing else "none"))
    ok = not missing and not incomplete and not floating
    print("drivers ok: %s" % ("YES" if ok else "NO"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
