#!/usr/bin/env python3
"""Extract the canonical source-identity inventory for the
``enemy_percent_damage_taken`` effect rows into the portable header
``brotato_enemy_hit_effect_data.h``.

Ground truth is the recovered Godot 3.x project (campaign root
``brotato_recovered`` + ``brotato_abyss`` DLC root).  This generator publishes
DATA ONLY: the runtime consumer (``brotato_enemy_hit_effects.h``,
WeaponInteractionRuntime) owns the state machine.

The identity
------------
``effect_behaviors/enemies/percent_damage_taken_enemy_effect_behavior.gd`` keys
its per-enemy active effect and proc counter on the tuple's first element,
``PercentDamageEffect.source_id_hash`` (``percent_damage_effect.gd``::

    export(String) var source_id = ""
    func to_array() -> Array:
        return [source_id_hash, key_hash, value, duration_secs,
                max_stacks, max_procs, outline_color.to_html()]

).  That identity is the ``source_id`` STRING, and extract_items.py already
interns it into ``BtItemEff.aux_key`` for every such row -- so the runtime keys
active effects on the EXISTING canonical ``aux_key``; there is no second
identity namespace and no hash is published.

``aux_key`` equality is the same equivalence relation as ``source_id_hash``
equality for this data: the project only ever defines two ``source_id`` values
(``"item_ice_cube"`` and ``"weapon_lute"``), and their Godot 3.x
``String.hash()`` values do not collide.  The generator computes the hashes
internally and hard-fails on a collision, so the published ``aux_key`` inventory
is provably equivalent.

Everything else the behaviour reads (``key`` / ``value`` / ``duration_secs`` /
``max_stacks`` / ``max_procs`` / ``color``) is already canonical in
``BtItemEff`` and is NOT duplicated here.

Usage::

    python3 ocean/brotato/extract_enemy_hit_effects.py \\
        --campaign-root /path/brotato_recovered \\
        --dlc-root      /path/brotato_abyss \\
        --out           ocean/brotato/brotato_enemy_hit_effect_data.h \\
        --provenance-out /path/elsewhere/brotato_enemy_hit_effects.json
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import extract_physics as physics  # noqa: E402
import extract_items as items  # noqa: E402
import extract_weapons as weapons  # noqa: E402

ExtractError = physics.ExtractError
get_str = items.get_str
get_int = items.get_int

# The behaviour filters every candidate on this custom_key
# (percent_damage_taken_enemy_effect_behavior.gd:55 / percent_damage_effect.gd).
CUSTOM_KEY = "enemy_percent_damage_taken"
# The base class that owns the exported fields + defaults.
PERCENT_DAMAGE_CLASS = "PercentDamageEffect"
# PercentDamageEffect's exported fields we resolve (from the real script chain).
DEFAULT_FIELDS = ("source_id", "duration_secs", "max_stacks", "max_procs",
                  "outline_color")

_EXPORT_RE = re.compile(
    r'^\s*export\s*\([^)]*\)\s*var\s+([A-Za-z_]\w*)\s*(?::[^=\n]+)?=\s*(.+?)\s*$')


# --------------------------------------------------------------------------
# Godot 3.x String.hash()  (the source of Keys.generate_hash, keys.gd:434)
# --------------------------------------------------------------------------
def godot_string_hash(text):
    """Godot 3.x ``String.hash()``: djb2 on Unicode code points, uint32.

    Used ONLY to prove ``aux_key`` equality does not differ from the source's
    ``source_id_hash`` equality (collision check below).  Iterating Python code
    points matches the engine's 32-bit ``CharType`` for every ASCII ``source_id``
    the project defines (non-ASCII is rejected)."""
    hsh = 5381
    for ch in text:
        hsh = (hsh * 33 + ord(ch)) & 0xFFFFFFFF
    return hsh


# --------------------------------------------------------------------------
# Inherited exported defaults (resolved from the actual .gd chain)
# --------------------------------------------------------------------------
def parse_export_defaults(text):
    """``{field: raw_literal}`` for the exported fields we care about."""
    out = {}
    for line in text.splitlines():
        match = _EXPORT_RE.match(line)
        if not match:
            continue
        name, raw = match.group(1), match.group(2)
        if name in DEFAULT_FIELDS:
            out[name] = raw
    return out


def _class_script_map(resolver):
    """``(class -> abs script path, class -> parent class)`` for the source."""
    by_path, parent = items.build_class_index(resolver.roots)
    by_class = {}
    for abs_path, cls in by_path.items():
        by_class.setdefault(cls, abs_path)
    return by_class, parent


def _read_text(path):
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        return handle.read()


def resolve_class_defaults(resolver, by_class, class_parent, cls):
    """Merge ``export`` defaults up the class chain (base first, leaf wins)."""
    defaults = {}
    for ancestor in reversed(items.ancestry(cls, class_parent)):
        script = by_class.get(ancestor)
        if script is None:
            continue
        for name, raw in parse_export_defaults(_read_text(script)).items():
            defaults[name] = raw
    missing = [f for f in DEFAULT_FIELDS if f not in defaults]
    if missing:
        raise ExtractError(
            "%s: the source chain defines no exported default for %s "
            "(expected on %s)" % (cls, ", ".join(missing), PERCENT_DAMAGE_CLASS))
    return defaults


def _default_str(defaults, name):
    value = defaults[name].strip()
    if value.startswith('"') and value.endswith('"'):
        return value[1:-1]
    return value


def _default_int(defaults, name):
    return int(float(defaults[name].strip()))


# --------------------------------------------------------------------------
# Per-effect resolution (resource value, else inherited default)
# --------------------------------------------------------------------------
def resolve_effect(opcode, props, defaults):
    """The resolved PercentDamageEffect tuple for one effect resource.

    ``source_id`` must be real: an absent, empty or non-ASCII ``source_id`` is a
    hard error (no placeholder, no guess)."""
    source_id = get_str(props, "source_id", None)
    if source_id is None:
        source_id = _default_str(defaults, "source_id")
    source_id = source_id.strip()
    if not source_id:
        raise ExtractError(
            "%s: source_id is empty; it is the active-effect identity and must "
            "be a real string" % opcode)
    for ch in source_id:
        if ord(ch) > 127:
            raise ExtractError(
                "%s: source_id %r is not ASCII; String.hash() depends on the "
                "(16/32-bit) CharType width" % (opcode, source_id))
    return {
        "opcode": opcode,
        "source_id": source_id,
        "source_id_hash": godot_string_hash(source_id),
        "key": get_str(props, "key", "").strip(),
        "value": get_int(props, "value", 0),
        "duration_secs": get_int(props, "duration_secs", _default_int(defaults, "duration_secs")),
        "max_stacks": get_int(props, "max_stacks", _default_int(defaults, "max_stacks")),
        "max_procs": get_int(props, "max_procs", _default_int(defaults, "max_procs")),
    }


# --------------------------------------------------------------------------
# Extraction
# --------------------------------------------------------------------------
def collect_effects(builder, resolver, by_class, class_parent):
    """Every canonical row that is an ``enemy_percent_damage_taken`` effect.

    Each row's ``aux_key`` must equal the resource's resolved ``source_id`` (the
    canonical row IS the identity the runtime will key on), and the canonical
    numeric fields must equal the resolved tuple -- any divergence is a hard
    error rather than a silently wrong consumer."""
    custom_id = builder.strings.ids.get(CUSTOM_KEY)
    if custom_id is None:
        raise ExtractError("interned string %r is absent from the item table" % CUSTOM_KEY)
    rows = []
    for index, row in enumerate(builder.rows):
        if row is None or row["custom_key"] != custom_id:
            continue
        chain = items.ancestry(row["opcode"], class_parent)
        if PERCENT_DAMAGE_CLASS not in chain:
            raise ExtractError(
                "BT_ITEM_EFF_ROWS[%d] has custom_key %s but opcode %s is not a %s"
                % (index, CUSTOM_KEY, row["opcode"], PERCENT_DAMAGE_CLASS))
        props = resolver.parse(row["source"]).root_props
        defaults = resolve_class_defaults(resolver, by_class, class_parent, row["opcode"])
        effect = resolve_effect(row["opcode"], props, defaults)

        where = "BT_ITEM_EFF_ROWS[%d]" % index
        for field in ("value", "duration_secs", "max_stacks", "max_procs"):
            if row[field] != effect[field]:
                raise ExtractError(
                    "%s: canonical %s=%r but the source resolves %r"
                    % (where, field, row[field], effect[field]))
        if builder.strings.values[row["key"]] != effect["key"]:
            raise ExtractError("%s: canonical key=%r but source resolves %r"
                               % (where, builder.strings.values[row["key"]], effect["key"]))
        if builder.strings.values[row["aux_key"]] != effect["source_id"]:
            raise ExtractError(
                "%s: canonical aux_key (source_id)=%r but source resolves %r"
                % (where, builder.strings.values[row["aux_key"]], effect["source_id"]))

        effect["row"] = index
        effect["source_res"] = row["source"]
        effect["interned_source_id"] = row["aux_key"]
        effect["owned_item"] = row["item"] != items.BT_ITEM_NONE
        rows.append(effect)

    if not rows:
        raise ExtractError("no canonical effect row carries custom_key %r" % CUSTOM_KEY)
    return rows


def build_sources(rows):
    """Distinct source identities (interned ``aux_key``), first appearance.

    Verifies the hash-collision premise that makes ``aux_key`` a faithful stand
    in for ``source_id_hash``: two DIFFERENT source_ids must not hash-collide."""
    sources = []
    index_of = {}
    hash_of_id = {}
    for effect in rows:
        interned = effect["interned_source_id"]
        if interned in hash_of_id and hash_of_id[interned] != effect["source_id_hash"]:
            raise ExtractError("interned source_id %d maps to two strings" % interned)
        hash_of_id[interned] = effect["source_id_hash"]
        if interned not in index_of:
            index_of[interned] = len(sources)
            sources.append({
                "interned": interned,
                "source_id": effect["source_id"],
                "hash": effect["source_id_hash"],
                "max_stacks": effect["max_stacks"],
                "rows": [],
            })
        entry = sources[index_of[interned]]
        entry["rows"].append(effect["row"])
        entry["max_stacks"] = max(entry["max_stacks"], effect["max_stacks"])

    seen = {}
    for source in sources:
        other = seen.get(source["hash"])
        if other is not None:
            raise ExtractError(
                "source_id_hash collision between %r and %r (both %u): aux_key "
                "equality would diverge from the source's identity"
                % (other, source["source_id"], source["hash"]))
        seen[source["hash"]] = source["source_id"]
    return sources


def extract(args):
    resolver = physics.Resolver([args.campaign_root, args.dlc_root])
    data = items.build(args)
    wdata = weapons.build_weapons(data, args)
    builder = data["builder"]
    by_class, class_parent = _class_script_map(resolver)
    rows = collect_effects(builder, resolver, by_class, class_parent)
    sources = build_sources(rows)
    return {
        "resolver": resolver,
        "builder": builder,
        "wdata": wdata,
        "rows": rows,
        "sources": sources,
    }


# --------------------------------------------------------------------------
# Emission
# --------------------------------------------------------------------------
def emit_header(data, out_path, campaign_root, dlc_root):
    builder = data["builder"]
    sources = data["sources"]
    n_rows = len(builder.rows)
    lines = []
    lines.append("/* Generated by ocean/brotato/extract_enemy_hit_effects.py -- DO NOT EDIT.")
    lines.append(" *")
    lines.append(" * Closed inventory of the source identities carried by the canonical")
    lines.append(" * enemy_percent_damage_taken effect rows.  The runtime keys its per-enemy")
    lines.append(" * PercentDamageTaken active / proc state on the EXISTING")
    lines.append(" * BtItemEff.aux_key (interned PercentDamageEffect.source_id) of the effect row")
    lines.append(" * it is applying -- there is no second identity namespace and no hash here.")
    lines.append(" * aux_key equality is the same relation as the source's source_id_hash")
    lines.append(" * equality: this project defines only these source_id values and their Godot")
    lines.append(" * 3.x String.hash() values do not collide (checked at generation time).")
    lines.append(" *")
    lines.append(" * key / value / duration_secs / max_stacks / max_procs / color are NOT")
    lines.append(" * duplicated -- read them from BT_ITEM_EFF_ROWS[row].  Resolve a source name")
    lines.append(" * through the existing BT_ISTR name table.")
    lines.append(" *")
    lines.append(" * Regenerate with:")
    lines.append(" *")
    lines.append(" *   python3 ocean/brotato/extract_enemy_hit_effects.py \\")
    lines.append(" *       --campaign-root %s \\" % campaign_root)
    lines.append(" *       --dlc-root %s \\" % dlc_root)
    lines.append(" *       --out %s \\" % out_path)
    lines.append(" *       --provenance-out <external>/brotato_enemy_hit_effects.json")
    lines.append(" */")
    lines.append("#pragma once")
    lines.append('#include "brotato_rng.h"          /* BT_TABLE */')
    lines.append('#include "brotato_item_effects.h" /* BT_ISTR_* / BT_ITEM_EFF_ROWS */')
    lines.append("")
    lines.append("#if defined(BT_NITEM_EFF_ROWS) && BT_NITEM_EFF_ROWS != %d" % n_rows)
    lines.append('#error "brotato_enemy_hit_effect_data.h: BT_NITEM_EFF_ROWS changed; regenerate with extract_enemy_hit_effects.py"')
    lines.append("#endif")
    lines.append("")
    lines.append("/* Distinct source identities (first appearance).  Each entry is a")
    lines.append(" * BT_ISTR_* interned id -- exactly BtItemEff.aux_key on the rows below.")
    for source in sources:
        lines.append(" *   %s  rows %s"
                     % (builder.strings.macros[source["source_id"]],
                        ",".join(str(r) for r in source["rows"])))
    lines.append(" */")
    lines.append("#define BT_EHE_NSOURCES %d" % max(1, len(sources)))
    lines.append("BT_TABLE unsigned short BT_EHE_SOURCE_ID[BT_EHE_NSOURCES] = {")
    for source in sources:
        lines.append("    %d,   /* %s */"
                     % (source["interned"], builder.strings.macros[source["source_id"]]))
    if not sources:
        lines.append("    0,")
    lines.append("};")
    lines.append("")
    return items._write("\n".join(lines), out_path)


def emit_provenance(data, out_path, args):
    payload = {
        "generator": "ocean/brotato/extract_enemy_hit_effects.py",
        "campaign_root": args.campaign_root,
        "dlc_root": args.dlc_root,
        "custom_key": CUSTOM_KEY,
        "identity": "BtItemEff.aux_key (interned PercentDamageEffect.source_id)",
        "counts": {
            "rows": len(data["rows"]),
            "sources": len(data["sources"]),
            "table_rows": len(data["builder"].rows),
        },
        "rows": [
            {
                "canonical_row": effect["row"],
                "opcode": effect["opcode"],
                "source_res": effect["source_res"],
                "source_id": effect["source_id"],
                "interned_source_id": effect["interned_source_id"],
                "interned_source_id_macro": data["builder"].strings.macros[effect["source_id"]],
                "source_id_hash": effect["source_id_hash"],
                "key": effect["key"],
                "value": effect["value"],
                "duration_secs": effect["duration_secs"],
                "max_stacks": effect["max_stacks"],
                "max_procs": effect["max_procs"],
                "owned_by": "item" if effect["owned_item"] else "weapon",
            }
            for effect in data["rows"]
        ],
        "sources": [
            {
                "source_id": source["source_id"],
                "interned_source_id": source["interned"],
                "interned_source_id_macro": data["builder"].strings.macros[source["source_id"]],
                "source_id_hash": source["hash"],
                "max_stacks": source["max_stacks"],
                "rows": source["rows"],
            }
            for source in data["sources"]
        ],
    }
    return items._write(json.dumps(payload, indent=1, sort_keys=False) + "\n", out_path)


# --------------------------------------------------------------------------
def main(argv):
    parser = argparse.ArgumentParser(
        description="Extract Brotato enemy_percent_damage_taken source-identity metadata.")
    parser.add_argument("--campaign-root", required=True)
    parser.add_argument("--dlc-root", required=True)
    parser.add_argument("--out", default="ocean/brotato/brotato_enemy_hit_effect_data.h")
    parser.add_argument("--provenance-out", default="")
    args = parser.parse_args(argv)
    try:
        data = extract(args)
        emit_header(data, args.out, args.campaign_root, args.dlc_root)
    except ExtractError as exc:
        sys.stderr.write("extract_enemy_hit_effects: error: %s\n" % exc)
        return 1
    if args.provenance_out:
        emit_provenance(data, args.provenance_out, args)
    sys.stderr.write(
        "extract_enemy_hit_effects: %d rows (%d item, %d weapon), %d distinct sources -> %s\n"
        % (len(data["rows"]),
           sum(1 for e in data["rows"] if e["owned_item"]),
           sum(1 for e in data["rows"] if not e["owned_item"]),
           len(data["sources"]), args.out))
    for source in data["sources"]:
        sys.stderr.write(
            "extract_enemy_hit_effects:   source %-14s aux_key=%d rows=%s max_stacks=%d\n"
            % (source["source_id"], source["interned"],
               ",".join(str(r) for r in source["rows"]), source["max_stacks"]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
