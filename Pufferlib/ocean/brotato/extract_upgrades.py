#!/usr/bin/env python3
"""Extract the Brotato level-up upgrade pool into ``brotato_upgrades.h``.

Ground truth is the recovered Godot 3.x project (campaign root
``brotato_recovered`` + ``brotato_abyss`` DLC root).  Every row is resolved from
the real ``UpgradeData`` / ``Effect`` resources; nothing is hand-authored and no
value is invented.  Re-running reproduces a byte-identical header (stable
ordering, no RNG).

Discovery is the *source graph*, not a directory glob:

  * the level-up picker is ``ItemService.get_upgrades`` /
    ``get_upgrade_data`` (singletons/item_service.gd:550/:581), which draws from
    ``_tiers_data[tier][TierData.UPGRADES]``;
  * that bucket is filled at item_service.gd:128-130 from ``ItemService.upgrades``;
  * ``ItemService.upgrades`` is the ``upgrades`` array of
    ``singletons/item_service.tscn`` plus the ``upgrades`` array of
    ``dlcs/dlc_1/dlc_data.tres`` (the DLC registers none).

So ``BT_UPGRADES`` is exactly the set of offers the game can produce, in the
order ``bt_rng_bounded(&s->rng, BT_NUPGRADES)`` indexes it.

Upgrade resources that exist under ``items/upgrades`` but are *not* in that
array are real but unreachable (the picker can never return them).  They are
reported instead of being silently dropped:

  * accuracy (``upgrade_accuracy_1..4``), crit damage
    (``upgrade_crit_damage_1..4``), flat damage (``upgrade_damage_1..4``) and
    the weapon-slot upgrade (``upgrade_weapon_slot``) become
    ``BT_UPGRADE_UNPOOLED`` rows;
  * ``items/upgrades/upgrade_to_process_data.tres`` is a UI template with no
    effects at all and is recorded in the provenance JSON only.

``BT_STAT_KEYS`` / ``BT_STAT_NAMES`` carry the shared stat contract (core stat
id -> game effect key / renderer display name, index == stat id) so the
renderer and the GDScript bridge read one table instead of hardcoding names.

Usage::

    python3 ocean/brotato/extract_upgrades.py \\
        --campaign-root /path/brotato_recovered \\
        --dlc-root      /path/brotato_abyss \\
        --header-out    ocean/brotato/brotato_upgrades.h \\
        --provenance-out <external>/brotato_upgrades.json
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import extract_physics as physics  # noqa: E402
import extract_gameplay as gameplay  # noqa: E402

ExtractError = physics.ExtractError
get_str = gameplay.get_str
get_int = gameplay.get_int
get_float = gameplay.get_float
get_bool = gameplay.get_bool

_ANY_REF_RE = re.compile(r"ExtResource\(\s*(\d+)\s*\)")

UPGRADE_SCRIPT = "/items/upgrades/upgrade_data.gd"

# --------------------------------------------------------------------------
# Shared stat contract: (core stat id, game effect key, renderer display name).
# Index in this tuple *is* the core stat id (core: brotato_state.h:91 BT_NSTATS,
# bt_core_stat_read / bt_core_stat_add).  Ids 0..12 and 13 (DLC curse) are the
# pre-existing contract; 14..18 are the stats the source upgrade pool also
# offers.  id 11 (crit damage) and 14 (accuracy) have no pooled upgrade row -
# see the module docstring - but they keep their ids so items/weapons and any
# future pool member map deterministically.
# --------------------------------------------------------------------------
CORE_STATS = (
    (0, "stat_max_hp", "max_hp"),
    (1, "stat_hp_regeneration", "regen"),
    (2, "stat_armor", "armor"),
    (3, "stat_speed", "speed"),
    (4, "stat_dodge", "dodge"),
    (5, "stat_percent_damage", "dmg%"),
    (6, "stat_melee_damage", "melee"),
    (7, "stat_ranged_damage", "ranged"),
    (8, "stat_elemental_damage", "elem"),
    (9, "stat_crit_chance", "crit"),
    (10, "stat_luck", "luck"),
    (11, "stat_crit_damage", "crit_dmg"),
    (12, "stat_harvesting", "harvest"),
    (13, "stat_curse", "curse"),
    (14, "stat_accuracy", "accuracy"),
    (15, "stat_attack_speed", "attack_speed"),
    (16, "stat_engineering", "engineering"),
    (17, "stat_lifesteal", "lifesteal"),
    (18, "stat_range", "range"),
)
STAT_ID = {key: sid for sid, key, _name in CORE_STATS}
STAT_KEYS = [key for _sid, key, _name in CORE_STATS]
STAT_NAMES = [name for _sid, _key, name in CORE_STATS]
BT_STAT_NONE = "BT_STAT_NONE"

# upgrade resource ids whose effect key is outside the core stat switch; their
# BT_UPGRADE_UNPOOLED row carries BT_STAT_NONE.
NO_CORE_STAT = "no core stat id for the effect key"


def cfloat(value):
    text = "%.6g" % float(value)
    if "e" in text or "E" in text:
        return text + "f"
    if "." not in text:
        text += ".0"
    return text + "f"


def stem(source):
    """Readable resource label: the ``.tres`` stem of a ``res://`` path."""
    return os.path.basename(source)[:-len(".tres")] if source.endswith(".tres") else source


# --------------------------------------------------------------------------
# Discovery
# --------------------------------------------------------------------------
def _array_refs(pf, props, key):
    return [pf.ext_path(i) for i in _ANY_REF_RE.findall(props.get(key, "") or "")]


def read_pool(campaign_root, dlc_root):
    """The ``upgrades`` arrays backing ItemService.upgrades."""
    svc = physics.parse_file(os.path.join(campaign_root, "singletons", "item_service.tscn"))
    roots = [n for n in svc.nodes if n["parent"] is None]
    if len(roots) != 1:
        raise ExtractError("item_service.tscn: expected one root node")
    campaign = _array_refs(svc, roots[0]["props"], "upgrades")
    dlc_path = os.path.join(dlc_root, "dlcs", "dlc_1", "dlc_data.tres")
    dpf = physics.parse_file(dlc_path)
    dlc = _array_refs(dpf, dpf.root_props, "upgrades")
    return campaign, dlc


def _script_ref(pf, where):
    script = pf.root_props.get("script")
    if not script:
        raise ExtractError("%s: resource has no script" % where)
    match = physics._EXTREF_RE.match(script)
    if not match:
        raise ExtractError("%s: unsupported script ref %r" % (where, script))
    return pf.ext_path(match.group(1))


def is_upgrade_resource(pf):
    script = pf.root_props.get("script")
    if not script:
        return False
    match = physics._EXTREF_RE.match(script)
    if not match:
        return False
    return pf.ext_path(match.group(1)).endswith(UPGRADE_SCRIPT)


def scan_tree(resolver, root, subdir):
    """Every UpgradeData resource under ``root/subdir`` -> {res:// path: parsed}."""
    base = os.path.join(root, subdir)
    found = {}
    if not os.path.isdir(base):
        return found
    for dirpath, _dirs, files in os.walk(base):
        for name in sorted(files):
            if not name.endswith(".tres"):
                continue
            path = os.path.join(dirpath, name)
            pf = physics.parse_file(path)
            if not is_upgrade_resource(pf):
                continue
            rel = os.path.relpath(path, root).replace(os.sep, "/")
            found["res://" + rel] = pf
    return found


def read_upgrade(resolver, pf, res_path, where):
    """One UpgradeData resource -> {stat_key, stat_value, tier, ...}."""
    if pf.kind != "resource":
        raise ExtractError("%s: not a resource file" % where)
    script = _script_ref(pf, where)
    if not script.endswith(UPGRADE_SCRIPT):
        raise ExtractError("%s: script %s is not UpgradeData" % (where, script))
    props = pf.root_props
    raw_tier = props.get("tier")
    if raw_tier is None:
        raise ExtractError("%s: UpgradeData without a tier" % where)
    tier = int(float(raw_tier))
    if tier < 0 or tier > 3:
        raise ExtractError("%s: tier %d outside COMMON(0)..LEGENDARY(3)" % (where, tier))
    effects = []
    for eff_path in _array_refs(pf, props, "effects"):
        epf = resolver.parse(eff_path)
        if epf.kind != "resource":
            raise ExtractError("%s: effect %s is not a resource" % (where, eff_path))
        key = get_str(epf.root_props, "key", "")
        if not key:
            raise ExtractError("%s: effect %s has no key" % (where, eff_path))
        effects.append({
            "source": eff_path,
            "key": key,
            "value": get_float(epf.root_props, "value", 0.0),
        })
    dominant = max(effects, key=lambda e: abs(e["value"])) if effects else None
    return {
        "source": res_path,
        "my_id": get_str(props, "my_id", ""),
        "my_id_is_string": str(props.get("my_id", "")).startswith('"'),
        "upgrade_id": get_str(props, "upgrade_id", ""),
        "name": get_str(props, "name", ""),
        "tier": tier,
        "value": get_int(props, "value", 1),
        "unlocked_by_default": get_bool(props, "unlocked_by_default", False),
        "effects": effects,
        "stat_key": dominant["key"] if dominant else "",
        "stat_value": dominant["value"] if dominant else 0.0,
    }


# --------------------------------------------------------------------------
# Build
# --------------------------------------------------------------------------
def build(args):
    resolver = physics.Resolver([args.campaign_root, args.dlc_root])
    campaign_pool, dlc_pool = read_pool(args.campaign_root, args.dlc_root)
    scan = scan_tree(resolver, args.campaign_root, "items/upgrades")
    scan.update(scan_tree(resolver, args.dlc_root, "dlcs/dlc_1/upgrades"))

    pool_paths = campaign_pool + dlc_pool
    seen = {}
    for path in pool_paths:
        if path in seen:
            raise ExtractError("%s: listed twice in the upgrades pool" % path)
        seen[path] = True

    rows = []
    for index, path in enumerate(pool_paths):
        pf = resolver.parse(path) if path not in scan else scan[path]
        where = "ItemService.upgrades[%d] %s" % (index, path)
        if not is_upgrade_resource(pf):
            raise ExtractError("%s: not an UpgradeData resource" % where)
        row = read_upgrade(resolver, pf, path, where)
        if not row["effects"]:
            raise ExtractError("%s: pooled upgrade has no effects" % where)
        if not row["unlocked_by_default"]:
            raise ExtractError(
                "%s: unlocked_by_default is false, so a fresh save's "
                "ProgressData.add_unlocked_by_default() never unlocks it" % where)
        if row["stat_key"] not in STAT_ID:
            raise ExtractError("%s: effect key %r has no core stat id"
                               % (where, row["stat_key"]))
        row["stat"] = STAT_ID[row["stat_key"]]
        row["core_tier"] = row["tier"] + 1
        row["service_index"] = index
        row["pooled"] = True
        rows.append(row)

    order = sorted(rows, key=lambda r: (r["stat"], r["core_tier"]))
    keyed = {}
    for row in order:
        pair = (row["stat"], row["core_tier"])
        if pair in keyed:
            raise ExtractError("two pooled upgrades share stat %d tier %d: %s and %s"
                               % (pair[0], pair[1], keyed[pair]["source"], row["source"]))
        keyed[pair] = row

    unpooled = []
    templates = []
    for path in sorted(scan):
        if path in seen:
            continue
        pf = scan[path]
        if not _array_refs(pf, pf.root_props, "effects"):
            templates.append({"source": path,
                              "reason": "UpgradeData template with no effects"})
            continue
        where = "unpooled upgrade %s" % path
        row = read_upgrade(resolver, pf, path, where)
        row["stat"] = STAT_ID.get(row["stat_key"])
        row["stat_macro"] = BT_STAT_NONE if row["stat"] is None else str(row["stat"])
        row["label"] = row["my_id"] if row["my_id_is_string"] else stem(path)
        row["core_tier"] = row["tier"] + 1
        row["pooled"] = False
        row["reason"] = ("not listed in ItemService.upgrades (singletons/"
                         "item_service.tscn), so get_upgrades() can never return it")
        unpooled.append(row)

    missing = [path for path in scan if path not in seen and path not in
               {t["source"] for t in templates} and not
               any(u["source"] == path for u in unpooled)]
    if missing:  # pragma: no cover - defensive
        raise ExtractError("unclassified upgrade resources: %s" % ", ".join(missing))

    # Canonical order: by core stat id then tier (rows without a core stat last,
    # by source), so the emitted table and the provenance are stable and readable.
    unpooled.sort(key=lambda r: (r["stat"] is None, r["stat"] or 0,
                                 r["core_tier"] if r["stat"] is not None else 0,
                                 r["source"]))

    data = {
        "rows": order,
        "unpooled": unpooled,
        "templates": templates,
        "campaign_pool": campaign_pool,
        "dlc_pool": dlc_pool,
        "scan": scan,
        "dir_stats": {
            "campaign": {"resources": len([p for p in scan if p.startswith("res://items/")]),
                         "pool": len(campaign_pool)},
            "dlc": {"resources": len([p for p in scan if not p.startswith("res://items/")]),
                    "pool": len(dlc_pool)},
        },
    }
    validate(data)
    return data


def validate(data):
    rows = data["rows"]
    if not rows:
        raise ExtractError("no pooled upgrades found")
    if not any(r["stat"] == 0 and r["core_tier"] == 1 for r in rows):
        raise ExtractError("the pool has no stat 0 tier 1 row, so BT_UPGRADES[0] "
                           "would not be the core's empty-slot encoding")
    pooled = {(r["stat"], r["core_tier"]) for r in rows}
    for row in rows:
        if row["stat"] not in range(19):
            raise ExtractError("%s: stat id %d outside the core contract"
                               % (row["source"], row["stat"]))
        if row["core_tier"] not in (1, 2, 3, 4):
            raise ExtractError("%s: tier %d outside 1..4" % (row["source"], row["core_tier"]))
    # A stat may legitimately ship fewer than four tiers (or gain a second row
    # per tier in a later patch); the generator emits whatever the array holds.
    # What it must never do is emit two rows for the same (stat, tier): the
    # bridge addresses an offer by (stat, tier), so the value would be ambiguous.
    tiers_per_stat = {}
    for row in rows:
        tiers_per_stat.setdefault(row["stat"], []).append(row["core_tier"])
    for stat, tiers in tiers_per_stat.items():
        if len(tiers) != len(set(tiers)):
            raise ExtractError("stat %d has duplicate tiers %s" % (stat, sorted(tiers)))
    for row in data["unpooled"]:
        if row["stat"] is None:
            if row["stat_key"] in STAT_ID:
                raise ExtractError("%s: key %s maps to a core stat id but the row "
                                   "carries none" % (row["source"], row["stat_key"]))
            continue
        if row["stat"] not in range(19):
            raise ExtractError("%s: stat id %d outside the core contract"
                               % (row["source"], row["stat"]))
        if row["stat_key"] not in STAT_ID:
            raise ExtractError("%s: stat id %d has no contract key %r"
                               % (row["source"], row["stat"], row["stat_key"]))
        if (row["stat"], row["core_tier"]) in pooled:
            raise ExtractError("%s: stat %d tier %d is both pooled and unpooled"
                               % (row["source"], row["stat"], row["core_tier"]))


# --------------------------------------------------------------------------
# Emission
# --------------------------------------------------------------------------
def emit_header(data, out_path, campaign_root, dlc_root):
    rows = data["rows"]
    unpooled = data["unpooled"]
    stats = sorted({r["stat"] for r in rows})
    tiers = sorted({r["core_tier"] for r in rows})
    lines = []
    lines.append("/* Generated by ocean/brotato/extract_upgrades.py -- DO NOT EDIT.")
    lines.append(" *")
    lines.append(" * The Brotato level-up upgrade table, resolved from the recovered Godot")
    lines.append(" * 3.x sources (campaign + Abyssal Terrors DLC).  Regenerate with:")
    lines.append(" *")
    lines.append(" *   python3 ocean/brotato/extract_upgrades.py \\")
    lines.append(" *       --campaign-root %s \\" % campaign_root)
    lines.append(" *       --dlc-root %s \\" % dlc_root)
    lines.append(" *       --header-out ocean/brotato/brotato_upgrades.h \\")
    lines.append(" *       --provenance-out <external>/brotato_upgrades.json")
    lines.append(" *")
    lines.append(" * The pool is the `upgrades` array of singletons/item_service.tscn")
    lines.append(" * (%d resources) plus the `upgrades` array of dlcs/dlc_1/dlc_data.tres"
                 % len(data["campaign_pool"]))
    lines.append(" * (%d resources), which is what ItemService._tiers_data[tier][UPGRADES]"
                 % len(data["dlc_pool"]))
    lines.append(" * is built from (item_service.gd:128-130) and therefore the exact set of")
    lines.append(" * offers ItemService.get_upgrades() can return: %d stats x tiers %s."
                 % (len(stats), "/".join(str(t) for t in tiers)))
    lines.append(" *")
    lines.append(" * Real upgrade resources outside that array are unreachable by the picker")
    lines.append(" * and are kept in BT_UPGRADE_UNPOOLED instead of being dropped:")
    for row in unpooled:
        lines.append(" *   %-24s stat %-4s tier %d  value %-6s unlocked_by_default %d"
                     % (row["label"],
                        "none" if row["stat"] is None else str(row["stat"]),
                        row["core_tier"], cfloat(row["stat_value"]),
                        1 if row["unlocked_by_default"] else 0))
    for template in data["templates"]:
        lines.append(" *   %s is a UI template with no effects (provenance only)."
                     % template["source"])
    lines.append(" *")
    lines.append(" * Row layout is the one the core already reads: { stat, tier, value }")
    lines.append(" * with stat = the shared core stat id, tier = 1..4 (resource tier + 1)")
    lines.append(" * and value = the dominant Effect.value.  bt_core_observe emits")
    lines.append(" * stat / 12.0f and clamp(value * 0.1f, -1, 1) from these rows, unchanged.")
    lines.append(" */")
    lines.append("#pragma once")
    lines.append('#include "brotato_rng.h"   /* BT_TABLE / BT_HOST_ONLY */')
    lines.append('#include "brotato_items.h"  /* BT_STAT_NONE */')
    lines.append("")
    lines.append("/* Shared stat contract.  Index == core stat id; BT_STAT_KEYS[i] is the")
    lines.append(" * game effect key and BT_STAT_NAMES[i] the renderer's display name (one")
    lines.append(" * table for the renderer and the GDScript bridge, so neither hardcodes a")
    lines.append(" * stat name).  The count must equal BT_NSTATS (brotato_state.h). */")
    lines.append("#define BT_NUPGRADE_STAT_IDS %d" % len(CORE_STATS))
    lines.append("BT_HOST_ONLY(static const char* const BT_STAT_KEYS[BT_NUPGRADE_STAT_IDS] = {")
    lines.append("    " + ", ".join('"%s"' % key for key in STAT_KEYS) + ",\n};)")
    lines.append("")
    lines.append("BT_HOST_ONLY(static const char* const BT_STAT_NAMES[BT_NUPGRADE_STAT_IDS] = {")
    lines.append("    " + ", ".join('"%s"' % name for name in STAT_NAMES) + ",\n};)")
    lines.append("")
    lines.append("BT_HOST_ONLY(static inline const char* bt_stat_display_name(int stat) {")
    lines.append('    return stat >= 0 && stat < BT_NUPGRADE_STAT_IDS ? BT_STAT_NAMES[stat] : "?";')
    lines.append("})")
    lines.append("")
    lines.append("typedef struct { unsigned char stat, tier; float value; } BtUpgrade;")
    lines.append("")
    lines.append("/* BT_UPGRADES index order is the core's own draw order.  Row 0 stays a tier-1")
    lines.append(" * row because bt_core_observe emits it as the empty-slot encoding (an")
    lines.append(" * absent level-up slot).  BtState.lvl_offers[] / lvl_prev[] store a row as")
    lines.append(" * `row + 1`, so 0 is the unambiguous \"no offer\" marker. */")
    lines.append("#define BT_NUPGRADES %d" % len(rows))
    lines.append("BT_TABLE BtUpgrade BT_UPGRADES[BT_NUPGRADES] = {")
    for row in rows:
        lines.append("    { %d, %d, %s },%s/* %s */"
                     % (row["stat"], row["core_tier"], cfloat(row["stat_value"]),
                        " " * max(1, 16 - len(cfloat(row["stat_value"]))),
                        row["my_id"] or row["source"]))
    lines.append("};")
    lines.append("")
    lines.append("/* Upgrade resources the level-up picker can never return (absent from")
    lines.append(" * ItemService.upgrades) and the UI template, extracted so the pool boundary")
    lines.append(" * is explicit.  stat is BT_STAT_NONE when the effect key has no core stat")
    lines.append(" * id (flat `stat_damage`, `weapon_slot`). */")
    lines.append("typedef struct {")
    lines.append("    BT_HOST_ONLY(const char* id;)")
    lines.append("    unsigned char stat;              /* core stat id, or BT_STAT_NONE */")
    lines.append("    unsigned char tier;              /* 1..4 (resource tier + 1) */")
    lines.append("    float         value;             /* dominant Effect.value */")
    lines.append("    unsigned char unlocked_by_default;")
    lines.append("} BtUpgradeUnpooled;")
    lines.append("#define BT_NUPGRADES_UNPOOLED %d" % len(unpooled))
    lines.append("BT_HOST_ONLY(static const BtUpgradeUnpooled "
                 "BT_UPGRADE_UNPOOLED[BT_NUPGRADES_UNPOOLED] = {")
    for row in unpooled:
        lines.append("    { BT_HOST_ONLY(\"%s\",) %s, %d, %s, %d },%s/* %s [%s] */"
                     % (row["my_id"] or stem(row["source"]),
                        row["stat_macro"], row["core_tier"], cfloat(row["stat_value"]),
                        1 if row["unlocked_by_default"] else 0,
                        " " * max(1, 14 - len(cfloat(row["stat_value"]))),
                        stem(row["source"]), row["stat_key"]))
    lines.append("};)")
    lines.append("")
    slot = next((r for r in unpooled if (r["my_id"] or "") == "upgrade_weapon_slot"), None)
    if slot is not None:
        lines.append("/* The ONE offer the level-up picker can return OUTSIDE BT_UPGRADES:")
        lines.append(" * item_service.gd:550-556 get_upgrades returns [weapon_slot_upgrade_data] when")
        lines.append(" * the run owns the weapon_slot_upgrades effect and the current weapon_slot is")
        lines.append(" * below it.  The resource is items/upgrades/weapon_slot/weapon_slot_data.tres")
        lines.append(" * (my_id \"upgrade_weapon_slot\", tier 0, value 1, single effect key")
        lines.append(" * \"weapon_slot\" value 1, upgrade_id \"upgrade_weapon_slot\"), i.e. the")
        lines.append(" * BT_UPGRADE_UNPOOLED row above -- promoted here to DEVICE constants so the")
        lines.append(" * level-up path does not need the host-only table.  `weapon_slot` is a run-level")
        lines.append(" * scalar with no core stat id (BT_STAT_NONE), so BtState.lvl_offers[] uses")
        lines.append(" * BT_UPGRADE_SLOT_OFFER as its index-space sentinel (0xFFFE: distinct from")
        lines.append(" * 0 = absent and from every BT_UPGRADES row).  extract_upgrades.py")
        lines.append(" * emits these four constants from the resource. */")
        lines.append("#define BT_UPGRADE_SLOT_OFFER 0xFFFEu")
        lines.append("#define BT_UPGRADE_SLOT_STAT  %s" % slot["stat_macro"])
        lines.append("#define BT_UPGRADE_SLOT_TIER  %d" % slot["core_tier"])
        lines.append("#define BT_UPGRADE_SLOT_VALUE %s" % cfloat(slot["stat_value"]))
    return _write("\n".join(lines), out_path)


def emit_provenance(data, out_path, args):
    pooled_tiers = {}
    for row in data["rows"]:
        pooled_tiers.setdefault(row["stat"], []).append(row["core_tier"])
    unpooled_by_stat = {}
    for row in data["unpooled"]:
        unpooled_by_stat.setdefault(row["stat"], []).append(row["label"])
    stat_contract = []
    for sid, key, name in CORE_STATS:
        stat_contract.append({
            "id": sid,
            "key": key,
            "name": name,
            "pooled_tiers": sorted(pooled_tiers.get(sid, [])),
            "unpooled_resources": unpooled_by_stat.get(sid, []),
        })
    payload = {
        "generator": "ocean/brotato/extract_upgrades.py",
        "campaign_root": os.path.abspath(args.campaign_root),
        "dlc_root": os.path.abspath(args.dlc_root),
        "rule": "BT_UPGRADES = singletons/item_service.tscn `upgrades` array "
                "+ dlcs/dlc_1/dlc_data.tres `upgrades` array, ordered by "
                "(core stat id, tier); everything else under items/upgrades is "
                "recorded as unpooled or as a template.",
        "stat_contract": stat_contract,
        "counts": {
            "campaign_pool": len(data["campaign_pool"]),
            "dlc_pool": len(data["dlc_pool"]),
            "pool_rows": len(data["rows"]),
            "pooled_stats": len({r["stat"] for r in data["rows"]}),
            "unpooled": len(data["unpooled"]),
            "templates": len(data["templates"]),
            "scanned_resources": len(data["scan"]),
        },
        "pool": [{
            "index": i,
            "stat": row["stat"],
            "tier": row["core_tier"],
            "value": row["stat_value"],
            "stat_key": row["stat_key"],
            "my_id": row["my_id"],
            "upgrade_id": row["upgrade_id"],
            "resource_tier": row["tier"],
            "service_index": row["service_index"],
            "source": row["source"],
            "effect": row["effects"][0]["source"],
        } for i, row in enumerate(data["rows"])],
        "unpooled": [{
            "my_id": row["my_id"],
            "label": row["label"],
            "stat": row["stat"],
            "tier": row["core_tier"],
            "value": row["stat_value"],
            "stat_key": row["stat_key"],
            "unlocked_by_default": row["unlocked_by_default"],
            "reason": row["reason"],
            "source": row["source"],
        } for row in data["unpooled"]],
        "templates": data["templates"],
    }
    _write(json.dumps(payload, indent="\t", sort_keys=False), out_path)
    return payload


def _write(text, path):
    directory = os.path.dirname(os.path.abspath(path))
    if directory and not os.path.isdir(directory):
        os.makedirs(directory)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text if text.endswith("\n") else text + "\n")
    return text


# --------------------------------------------------------------------------
# Entry point
# --------------------------------------------------------------------------
def emit_all(args):
    data = build(args)
    emit_header(data, args.header_out, args.campaign_root, args.dlc_root)
    if args.provenance_out:
        emit_provenance(data, args.provenance_out, args)
    return data


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Extract the Brotato level-up upgrade pool.",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--campaign-root", required=True)
    parser.add_argument("--dlc-root", required=True)
    parser.add_argument("--header-out", default="ocean/brotato/brotato_upgrades.h")
    parser.add_argument("--provenance-out", default=None,
                        help="external JSON provenance path (optional)")
    args = parser.parse_args(argv)
    try:
        data = emit_all(args)
    except ExtractError as exc:
        sys.stderr.write("extract_upgrades: error: %s\n" % exc)
        return 1
    sys.stderr.write(
        "extract_upgrades: %d pool rows (%d stats x %d tiers, campaign %d + DLC %d), "
        "%d unpooled resources, %d templates -> %s\n"
        % (len(data["rows"]), len({r["stat"] for r in data["rows"]}),
           len({r["core_tier"] for r in data["rows"]}), len(data["campaign_pool"]),
           len(data["dlc_pool"]), len(data["unpooled"]), len(data["templates"]),
           args.header_out))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
