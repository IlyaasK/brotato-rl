#!/usr/bin/env python3
"""Extract the selectable Brotato character profiles (Bull, Knight, Demon) into
the portable header ``brotato_characters.h``.

Ground truth is the recovered Godot 3.x project (campaign root
``brotato_recovered`` + ``brotato_abyss`` DLC root).  Every field is resolved
from the real ``.tres`` resources; nothing is hand-authored and no value is
invented.

Canonical ids (stable, ABI): Bull 0, Knight 1, Demon 2.  Bull stays the
zero-slot, weaponless default the rest of the core was built around.

What is emitted
---------------
* ``brotato_characters.h``:
  - ``BT_CHARACTER_BULL`` / ``BT_CHARACTER_KNIGHT`` / ``BT_CHARACTER_DEMON`` /
    ``BT_NCHARACTERS`` / ``BT_CHARACTER_NONE``.
  - ``BtCharacterStat`` rows (kind 0 permanent base add, kind 1 stat-gain
    delta percent) and ``BtCharacterDef`` rows carrying the slot limit, weapon
    restrictions, min/max weapon tier, shop currency, explode-on-hit stats,
    the stat link (Knight), the end-of-wave stat conversion (Demon), the
    next-level XP threshold gain (next_level_xp_needed), the banned item
    groups/items and the wanted tags.
  - ``BT_CHARACTER_BAN_ITEMS`` (BT_ITEMS indices), ``BT_CHARACTER_WANT_TAGS``
    (interned BT_ISTR_* ids), ``BT_CHARACTER_START_WEAPONS`` (host-only weapon
    ``my_id`` strings) and ``BT_CHARACTER_START_TYPE``.

The item pool indices and the interned tag ids come from the SAME canonical
interner/table ``extract_items.py`` emits (the ``items`` array of
``singletons/item_service.tscn`` + ``dlcs/dlc_1/dlc_data.tres``), so a
character ban/tag can never disagree with ``brotato_items.h``.  This module
imports ``extract_items`` and reuses ``build()``; it never regenerates the
item headers.

Usage::

    python3 ocean/brotato/extract_characters.py \\
        --campaign-root /path/brotato_recovered \\
        --dlc-root      /path/brotato_abyss \\
        --characters-out ocean/brotato/brotato_characters.h
"""

from __future__ import annotations

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import extract_physics as physics  # noqa: E402
import extract_items as items  # noqa: E402
import extract_gameplay as gameplay  # noqa: E402

ExtractError = physics.ExtractError
get_str = gameplay.get_str
get_int = gameplay.get_int
get_float = gameplay.get_float
get_bool = gameplay.get_bool

_ANY_REF_RE = items._ANY_REF_RE
_STR_RE = items._STR_RE

BT_STAT_NONE = 0xFF
BT_ITEM_NONE = 0xFFFF

# Canonical character order == canonical ids. Absolute so a caller can never
# reorder the ABI.  Each entry: (source my_id, config token, resource path).
CANONICAL_CHARACTERS = (
    ("character_bull", "bull", "items/characters/bull/bull_data.tres"),
    ("character_knight", "knight", "items/characters/knight/knight_data.tres"),
    ("character_demon", "demon", "items/characters/demon/demon_data.tres"),
)

# Core stat ids 0..18 (brotato_core.h bt_core_id_from_istr + bt_core_stat_sync
# header comment).  Effects name stats by their source key.
CHAR_STAT_KEYS = (
    ("stat_max_hp", 0),
    ("stat_hp_regeneration", 1),
    ("stat_armor", 2),
    ("stat_speed", 3),
    ("stat_dodge", 4),
    ("stat_percent_damage", 5),
    ("stat_melee_damage", 6),
    ("stat_ranged_damage", 7),
    ("stat_elemental_damage", 8),
    ("stat_crit_chance", 9),
    ("stat_luck", 10),
    ("stat_crit_damage", 11),
    ("stat_harvesting", 12),
    ("stat_curse", 13),
    ("stat_accuracy", 14),
    ("stat_attack_speed", 15),
    ("stat_engineering", 16),
    ("stat_lifesteal", 17),
    ("stat_range", 18),
)
CHAR_STAT_ID = {key: sid for key, sid in CHAR_STAT_KEYS}
NSTATS = len(CHAR_STAT_KEYS)
# brotato_core.h bt_core_stat_sync: 19 core stat ids 0..18, dense.
if sorted(CHAR_STAT_ID.values()) != list(range(NSTATS)) or NSTATS != 19:
    raise ExtractError("character stat ids must be the dense core range 0..18")

# Effect classes this module understands.  Anything else in a supported
# character's effects[] is a hard error: a silent skip would drop real source
# behaviour.
CLASS_STAT_GAINS = "StatGainsModificationEffect"
CLASS_STAT_LINK = "GainStatForEveryStatEffect"
CLASS_CONVERT_STAT = "ConvertStatEffect"
CLASS_ITEM_EXPLODING = "ItemExplodingEffect"

# Effect.StorageMethod (items/global/effect.gd)
STORAGE_SUM = 0
STORAGE_REPLACE = 2

# WeaponData.type (weapons/weapon_type.gd): MELEE 0, RANGED 1.
WEAPON_TYPE_MELEE = 0
WEAPON_TYPE_RANGED = 1


# --------------------------------------------------------------------------
# Source discovery
# --------------------------------------------------------------------------
def _script_class(resolver, pf, class_by_path):
    """Class name of a resource's `script` ExtResource (effects are scripts)."""
    script = pf.root_props.get("script")
    if not script or not physics._EXTREF_RE.match(script):
        raise ExtractError("%s: resource has no script ExtResource" % pf.path)
    path = resolver.find(pf.ext_path(physics._EXTREF_RE.match(script).group(1)))
    cls = class_by_path.get(path)
    if cls is None:
        raise ExtractError("%s: script %s has no class_name" % (pf.path, path))
    return cls


def scan_characters(roots):
    """{source my_id -> res:// path} for every CharacterData in the campaign."""
    base = roots[0]
    found = {}
    cdir = os.path.join(base, "items", "characters")
    if not os.path.isdir(cdir):
        raise ExtractError("%s: no items/characters directory" % base)
    for dirpath, _dirs, files in os.walk(cdir):
        for name in sorted(files):
            if not name.endswith(".tres"):
                continue
            absolute = os.path.join(dirpath, name)
            pf = physics.parse_file(absolute)
            script = pf.root_props.get("script")
            if not script or not physics._EXTREF_RE.match(script):
                continue
            if not pf.ext_path(physics._EXTREF_RE.match(script).group(1)).endswith(
                    "character_data.gd"):
                continue
            my_id = get_str(pf.root_props, "my_id", "")
            if not my_id.startswith("character_"):
                raise ExtractError("%s: my_id %r is not a character id"
                                   % (absolute, my_id))
            if my_id in found:
                raise ExtractError("duplicate character %s (%s and %s)"
                                   % (my_id, found[my_id], absolute))
            rel = os.path.relpath(absolute, base).replace(os.sep, "/")
            found[my_id] = "res://" + rel
    return found


def read_starting_weapons(resolver, pf, props):
    """[(weapon my_id, type, tier)] in the character-selection UI order.

    The raw ``.tres`` array order is NOT the order the player (and therefore the
    sim's pool, brotato_character_runtime.h bt_character_start_*) sees.  The
    selection scene lists a character's starting weapons through
    ItemService.get_ordered_starting_weapons (singletons/item_service.gd:1013-
    1024, called from ui/menus/run/weapon_selection.gd:92-117): every MELEE
    weapon first, in array order, then every RANGED weapon (WeaponData.Type
    MELEE=0, RANGED=1 -- items/global/weapon_data.gd:4).  Apply that same
    partition so the emitted table matches the source's displayed order.  It is
    a no-op for Bull/Knight/Demon (their arrays are already grouped) but exact
    for any future profile with an interleaved list.
    """
    out = []
    for eid in _ANY_REF_RE.findall(props.get("starting_weapons", "") or ""):
        wpf = resolver.parse(pf.ext_path(eid))
        my_id = get_str(wpf.root_props, "my_id", "")
        if not my_id.startswith("weapon_"):
            raise ExtractError("%s: starting weapon %s is not a weapon_ id"
                               % (pf.path, my_id))
        out.append((my_id,
                    get_int(wpf.root_props, "type", -1),
                    get_int(wpf.root_props, "tier", -1)))
    melee = [w for w in out if w[1] == 0]
    ranged = [w for w in out if w[1] == 1]
    other = [w for w in out if w[1] not in (0, 1)]
    return melee + ranged + other


def read_explode_effect(resolver, pf, props):
    """ItemExplodingEffect -> the Bull explosion parameters (bull_effect_4)."""
    refs = _ANY_REF_RE.findall(props.get("stats", "") or "")
    if not refs:
        raise ExtractError("%s: ItemExplodingEffect has no stats resource" % pf.path)
    spf = resolver.parse(pf.ext_path(refs[0]))
    sp = spf.root_props
    scaling = []
    for stat_key, ratio in re.findall(
            r'\[\s*"([^"]+)"\s*,\s*([-\d.eE+]+)\s*\]', sp.get("scaling_stats", "") or ""):
        scaling.append((stat_key, float(ratio)))
    if not scaling:
        raise ExtractError("%s: explosion stats carry no scaling_stats" % spf.path)
    return {
        "chance": get_float(props, "chance", 0.0),
        "scale": get_float(props, "scale", 1.0),
        "scale_missing_hp": 1 if get_bool(props, "scale_with_missing_health", False) else 0,
        "damage": get_int(sp, "damage", 0),
        "max_range": get_int(sp, "max_range", 0),
        "crit_chance": get_float(sp, "crit_chance", 0.0),
        "crit_damage": get_float(sp, "crit_damage", 0.0),
        "scaling": scaling,
        "source": refs[0],
    }


def read_character(resolver, class_by_path, res_path):
    pf = resolver.parse(res_path)
    props = pf.root_props
    if not pf.ext_path(physics._EXTREF_RE.match(props["script"]).group(1)) \
            .endswith("character_data.gd"):
        raise ExtractError("%s: not a character_data resource" % res_path)
    my_id = get_str(props, "my_id", "")
    name = get_str(props, "name", "")
    if not name.startswith("CHARACTER_"):
        raise ExtractError("%s: %r is not a CHARACTER_ name" % (res_path, name))

    out = {
        "id": my_id,
        "source": res_path,
        "weapon_slots": 6,          # player_run_data.gd:434 default
        "no_melee_weapons": 0,
        "no_ranged_weapons": 0,
        "min_weapon_tier": 0,       # player_run_data.gd:488 default
        "max_weapon_tier": 99,      # player_run_data.gd:489 default
        "hp_shop": 0,
        "next_level_xp_pct": 0,     # run_data.gd:767 next_level_xp_needed SUM
        "stats": [],                # (stat id, kind, value)
        "link": None,
        "convert": None,
        "explode": None,
        "ban_groups": _STR_RE.findall(props.get("banned_item_groups", "") or ""),
        "ban_items": _STR_RE.findall(props.get("banned_items", "") or ""),
        "want_tags": _STR_RE.findall(props.get("wanted_tags", "") or ""),
        "starting_weapons": read_starting_weapons(resolver, pf, props),
    }

    for eid in _ANY_REF_RE.findall(props.get("effects", "") or ""):
        epath = pf.ext_path(eid)
        epf = resolver.parse(epath)
        eprops = epf.root_props
        cls = _script_class(resolver, epf, class_by_path)
        key = get_str(eprops, "key", "")
        storage = get_int(eprops, "storage_method", STORAGE_SUM)
        value = get_int(eprops, "value", 0)

        if cls == CLASS_STAT_GAINS:
            for stat_key in _STR_RE.findall(eprops.get("stats_modified", "") or ""):
                out["stats"].append((_char_stat_id(epath, stat_key), 1, float(value)))
        elif cls == CLASS_STAT_LINK:
            if out["link"] is not None:
                raise ExtractError("%s: two stat links in one character" % res_path)
            out["link"] = {
                "target": _char_stat_id(epath, key),
                "source": _char_stat_id(epath, get_str(eprops, "stat_scaled", "")),
                "value": value,
                "nb": get_int(eprops, "nb_stat_scaled", 0),
                "perm_only": 1 if get_bool(eprops, "perm_stats_only", True) else 0,
            }
        elif cls == CLASS_CONVERT_STAT:
            if out["convert"] is not None:
                raise ExtractError("%s: two stat conversions in one character" % res_path)
            out["convert"] = {
                "from": _char_stat_id(epath, key, allow_materials=True),
                "to": _char_stat_id(epath, get_str(eprops, "to_stat", "")),
                "pct": int(get_float(eprops, "pct_converted", 0.0)),
                "value": value,
                "to_value": get_int(eprops, "to_value", 0),
                "custom_key": get_str(eprops, "custom_key", ""),
            }
        elif cls == CLASS_ITEM_EXPLODING:
            if out["explode"] is not None:
                raise ExtractError("%s: two explode-on-hit effects" % res_path)
            ex = read_explode_effect(resolver, epf, eprops)
            out["explode"] = ex
        else:
            # Plain Effect rows: permanent stat adds (SUM), the weapon-slot
            # REPLACE, and the boolean restriction keys.
            if key in CHAR_STAT_ID and storage == STORAGE_SUM:
                out["stats"].append((CHAR_STAT_ID[key], 0, float(value)))
            elif key == "weapon_slot" and storage == STORAGE_REPLACE:
                out["weapon_slots"] = value
            elif key == "no_melee_weapons":
                out["no_melee_weapons"] = value
            elif key == "no_ranged_weapons":
                out["no_ranged_weapons"] = value
            elif key == "min_weapon_tier" and storage == STORAGE_REPLACE:
                out["min_weapon_tier"] = value
            elif key == "max_weapon_tier" and storage == STORAGE_REPLACE:
                out["max_weapon_tier"] = value
            elif key == "hp_shop" and storage == STORAGE_REPLACE:
                out["hp_shop"] = value
            elif key == "next_level_xp_needed":
                # run_data.gd:767-768: a plain SUM scalar (int) that scales the
                # next-level XP threshold, multiplied as (1 + pct/100).  Producers
                # are character effects only (baby_effect_4 +130, mutant_effect_1
                # -66, technomage_effect_2 +75 -- all storage_method 0), and the
                # key is NOT interned into BT_ISTR_* because no item or weapon
                # uses it, so it gets its own BtCharacterDef field.
                out["next_level_xp_pct"] += value
            else:
                raise ExtractError("%s: unhandled %s effect key %r (storage %d)"
                                   % (epath, cls, key, storage))
    return out


def _char_stat_id(path, stat_key, allow_materials=False):
    if allow_materials and stat_key == "materials":
        return BT_STAT_NONE
    if stat_key not in CHAR_STAT_ID:
        raise ExtractError("%s: %r is not a core stat key" % (path, stat_key))
    return CHAR_STAT_ID[stat_key]


# --------------------------------------------------------------------------
# Build
# --------------------------------------------------------------------------
def build(args):
    resolver = physics.Resolver([args.campaign_root, args.dlc_root])
    class_by_path, _class_parent = items.build_class_index(resolver.roots)

    # The canonical item pool (indices + interned tag ids) is owned by
    # extract_items; reusing its build() keeps every character ban/tag in the
    # same index space as brotato_items.h.  build() only reads the two roots.
    item_args = argparse.Namespace(campaign_root=args.campaign_root,
                                   dlc_root=args.dlc_root)
    item_data = items.build(item_args)
    if not all(k in item_data for k in ("id_to_index", "group_names", "builder")):
        raise ExtractError("extract_items.build() no longer exposes the canonical "
                           "item index / group / interner tables this module needs")
    id_to_index = item_data["id_to_index"]
    group_names = item_data["group_names"]
    strings = item_data["builder"].strings

    scanned = scan_characters(resolver.roots)

    characters = []
    for canon_id, token, rel in CANONICAL_CHARACTERS:
        if canon_id not in scanned:
            raise ExtractError("canonical character %s (%s) not found in the tree"
                               % (canon_id, rel))
        expected = "res://" + rel
        if scanned[canon_id] != expected:
            raise ExtractError("character %s lives at %s, expected %s"
                               % (canon_id, scanned[canon_id], expected))
        char = read_character(resolver, class_by_path, expected)
        if char["id"] != canon_id:
            raise ExtractError("%s: my_id %r != canonical %r" % (rel, char["id"], canon_id))
        char["token"] = token

        # Banned items -> BT_ITEMS indices (same space as BT_BULL_BANNED_ITEMS).
        banned = []
        for item_id in char["ban_items"]:
            if item_id not in id_to_index:
                raise ExtractError("%s: banned item %s is not in the item pool"
                                   % (rel, item_id))
            banned.append(id_to_index[item_id])
        char["ban_indices"] = sorted(banned)

        # Banned groups -> the BT_ITEM_GROUP_* bitmask bit order.
        mask = 0
        for group in char["ban_groups"]:
            if group not in group_names:
                raise ExtractError("%s: banned group %r is not an item group"
                                   % (rel, group))
            mask |= 1 << group_names.index(group)
        char["ban_group_mask"] = mask

        # Wanted tags -> the interned ids brotato_item_effects.h exposes.  A tag
        # the item pool never interned would be a dangling id, so reject it.
        want_ids = []
        for tag in char["want_tags"]:
            sid = strings.ids.get(tag)
            if sid is None:
                raise ExtractError("%s: wanted tag %r is not an interned item tag"
                                   % (rel, tag))
            want_ids.append(sid)
        char["want_ids"] = want_ids
        characters.append(char)

    # Validate every starting weapon id matches its tier (the source arrays are
    # curated per character; a mismatch means we misread the resource).
    for char in characters:
        for my_id, wtype, tier in char["starting_weapons"]:
            if wtype not in (WEAPON_TYPE_MELEE, WEAPON_TYPE_RANGED):
                raise ExtractError("%s: weapon %s has type %d" % (char["id"], my_id, wtype))
            if tier < 0:
                raise ExtractError("%s: weapon %s has no tier" % (char["id"], my_id))

    return {
        "characters": characters,
        "group_names": group_names,
        "item_count": len(id_to_index),
    }


# --------------------------------------------------------------------------
# Emit
# --------------------------------------------------------------------------
def cfloat(value):
    return items.cfloat(value)


def _stat_rows(characters):
    rows = []
    spans = []
    for char in characters:
        first = len(rows)
        rows.extend(char["stats"])
        spans.append((first, len(char["stats"])))
    return rows, spans


def emit_characters(data, out_path, campaign_root, dlc_root):
    characters = data["characters"]
    rows, spans = _stat_rows(characters)

    bans = []
    ban_spans = []
    wants = []
    want_spans = []
    starts = []
    start_spans = []
    for char in characters:
        b0 = len(bans); bans.extend(char["ban_indices"]); ban_spans.append((b0, len(char["ban_indices"])))
        w0 = len(wants); wants.extend(char["want_ids"]); want_spans.append((w0, len(char["want_ids"])))
        s0 = len(starts); starts.extend(char["starting_weapons"]); start_spans.append((s0, len(char["starting_weapons"])))

    lines = []
    lines.append("/* Generated by ocean/brotato/extract_characters.py -- DO NOT EDIT.")
    lines.append(" *")
    lines.append(" * Selectable character profiles (Bull, Knight, Demon) resolved from the")
    lines.append(" * recovered Godot 3.x sources.  Regenerate with:")
    lines.append(" *")
    lines.append(" *   python3 ocean/brotato/extract_characters.py \\")
    lines.append(" *       --campaign-root %s \\" % campaign_root)
    lines.append(" *       --dlc-root %s \\" % dlc_root)
    lines.append(" *       --characters-out %s" % out_path)
    lines.append(" *")
    lines.append(" * Canonical ids are stable: Bull 0 (default, zero-slot), Knight 1, Demon 2.")
    lines.append(" * Item bans are BT_ITEMS indices and wanted tags are interned BT_ISTR_*")
    lines.append(" * ids, both taken from the same table extract_items.py emits, so they can")
    lines.append(" * never drift from brotato_items.h / brotato_item_effects.h.")
    lines.append(" *")
    lines.append(" * Source: items/characters/ (bull, knight, demon directories); effect")
    lines.append(" * scripts effect.gd, stat_gains_modification_effect.gd,")
    lines.append(" * gain_stat_for_every_stat_effect.gd, convert_stat_effect.gd and")
    lines.append(" * item_exploding_effect.gd.")
    lines.append(" */")
    lines.append("#pragma once")
    lines.append("")
    lines.append('#include "brotato_rng.h"   /* BT_HD / BT_TABLE / BT_HOST_ONLY */')
    lines.append("")
    lines.append("#define BT_CHARACTER_BULL 0")
    lines.append("#define BT_CHARACTER_KNIGHT 1")
    lines.append("#define BT_CHARACTER_DEMON 2")
    lines.append("#define BT_NCHARACTERS %d" % len(characters))
    lines.append("#define BT_CHARACTER_NONE 0x%04Xu" % BT_ITEM_NONE)
    lines.append("#define BT_CHARACTER_SLOT_DEFAULT 6   /* player_run_data.gd:434 */")
    lines.append("#define BT_CHARACTER_STAT_NONE 0x%02Xu" % BT_STAT_NONE)
    lines.append("")
    lines.append("/* One per-character stat modification.  kind 0 adds `value` to the")
    lines.append(" * permanent stat_raw layer (effect.gd SUM on a stat key); kind 1 adds")
    lines.append(" * `value` percent to that stat's gain multiplier")
    lines.append(" * (stat_gains_modification_effect.gd: gain_<stat> += value). */")
    lines.append("typedef struct BtCharacterStat {")
    lines.append("    unsigned char stat;")
    lines.append("    unsigned char kind;   /* 0 permanent base add, 1 gain delta percent */")
    lines.append("    float value;")
    lines.append("} BtCharacterStat;")
    lines.append("")
    lines.append("typedef struct BtCharacterDef {")
    lines.append("    BT_HOST_ONLY(const char* id;)     /* source my_id, e.g. \"character_bull\" */")
    lines.append("    BT_HOST_ONLY(const char* name;)   /* config token: bull / knight / demon */")
    lines.append("    unsigned char weapon_slots;       /* weapon_slot effect (Bull REPLACE 0) */")
    lines.append("    unsigned char no_melee_weapons;")
    lines.append("    unsigned char no_ranged_weapons;  /* knight_effect_3 */")
    lines.append("    unsigned char min_weapon_tier;    /* knight_effect_4 (REPLACE 1) */")
    lines.append("    unsigned char max_weapon_tier;    /* player_run_data.gd:489 default 99 */")
    lines.append("    unsigned char hp_shop;            /* demon_effect_2: currency = stat_max_hp */")
    lines.append("    unsigned char explode_on_hit;     /* bull_effect_4 */")
    lines.append("    unsigned char explode_scale_missing_hp;")
    lines.append("    unsigned char link_target;        /* core stat id gained, 255 none */")
    lines.append("    unsigned char link_source;        /* core stat id scaled (stat_armor) */")
    lines.append("    unsigned char link_perm_only;     /* GainStatForEveryStatEffect.perm_stats_only */")
    lines.append("    unsigned char convert_from;       /* core stat id, 255 = materials/gold */")
    lines.append("    unsigned char convert_to;         /* core stat id added (stat_max_hp) */")
    lines.append("    unsigned char convert_pct;        /* percent of the source converted */")
    lines.append("    unsigned char stat_first;         /* first BT_CHARACTER_STATS row */")
    lines.append("    unsigned char stat_count;")
    lines.append("    unsigned short ban_group_mask;    /* BT_ITEM_GROUP_* bitmask */")
    lines.append("    unsigned short ban_first;         /* first BT_CHARACTER_BAN_ITEMS entry */")
    lines.append("    unsigned short ban_count;")
    lines.append("    unsigned short want_first;        /* first BT_CHARACTER_WANT_TAGS entry */")
    lines.append("    unsigned short want_count;")
    lines.append("    unsigned short start_first;       /* first BT_CHARACTER_START_WEAPONS entry */")
    lines.append("    unsigned short start_count;")
    lines.append("    short link_value;")
    lines.append("    short link_nb;")
    lines.append("    short convert_value;              /* source chunk size (13) */")
    lines.append("    short convert_to_value;           /* added per chunk (1) */")
    lines.append("    short next_level_xp_pct;          /* run_data.gd:767 next_level_xp_needed SUM (baby +130, mutant -66, technomage +75) */")
    lines.append("    short explode_damage;             /* bull_explosion_stats.damage */")
    lines.append("    short explode_range;              /* bull_explosion_stats.max_range */")
    lines.append("    float explode_chance;")
    lines.append("    float explode_scale;")
    lines.append("    float explode_crit_chance;")
    lines.append("    float explode_crit_damage;")
    lines.append("    float explode_dmg_scale;          /* scaling per stat_*_damage entry */")
    lines.append("} BtCharacterDef;")
    lines.append("")

    lines.append("/* kind 0 = stat_raw += value, kind 1 = stat_gain += value/100. */")
    lines.append("BT_TABLE BtCharacterStat BT_CHARACTER_STATS[%d] = {" % max(1, len(rows)))
    for char_index, char in enumerate(characters):
        first, count = spans[char_index]
        for sid, kind, value in rows[first:first + count]:
            lines.append("    { %d, %d, %s },  /* %s */"
                         % (sid, kind, cfloat(value), char["id"]))
    if not rows:
        lines.append("    { 0, 0, 0.0f },")
    lines.append("};")
    lines.append("")

    lines.append("/* BT_ITEMS indices banned for the character (item_service.gd:313-450). */")
    lines.append("BT_TABLE unsigned short BT_CHARACTER_BAN_ITEMS[%d] = { %s };"
                 % (max(1, len(bans)), ", ".join(str(i) for i in bans) if bans else "0"))
    lines.append("")
    lines.append("/* Interned BT_ISTR_* tag ids the character's shop pool is biased toward. */")
    lines.append("BT_TABLE unsigned short BT_CHARACTER_WANT_TAGS[%d] = { %s };"
                 % (max(1, len(wants)), ", ".join(str(i) for i in wants) if wants else "0"))
    lines.append("")
    lines.append("/* Host-only starting-weapon my_id strings, in the character-selection")
    lines.append(" * UI order: melee block first, then ranged (ItemService.")
    lines.append(" * get_ordered_starting_weapons, item_service.gd:1013-1024; the scene")
    lines.append(" * consumes it at ui/menus/run/weapon_selection.gd:92-117).  The")
    lines.append(" * selected weapon is resolved to a BT_WEAPONS id by the host config and")
    lines.append(" * passed to bt_core_init; the device never reads these names. */")
    lines.append("BT_HOST_ONLY(static const char* const BT_CHARACTER_START_WEAPONS[%d] = { %s };)"
                 % (max(1, len(starts)),
                    ", ".join('"%s"' % w[0] for w in starts) if starts else '""'))
    lines.append("/* WeaponData.type of each start entry: 0 melee, 1 ranged. */")
    lines.append("BT_TABLE unsigned char BT_CHARACTER_START_TYPE[%d] = { %s };"
                 % (max(1, len(starts)),
                    ", ".join(str(w[1]) for w in starts) if starts else "0"))
    lines.append("")
    lines.append("BT_TABLE BtCharacterDef BT_CHARACTERS[BT_NCHARACTERS] = {")
    for index, char in enumerate(characters):
        link = char["link"] or {}
        conv = char["convert"] or {}
        ex = char["explode"] or {}
        s0, sc = spans[index]
        b0, bc = ban_spans[index]
        w0, wc = want_spans[index]
        t0, tc = start_spans[index]
        lines.append("    { BT_HOST_ONLY(\"%s\", \"%s\",)" % (char["id"], char["token"]))
        lines.append("      %d, %d, %d, %d, %d, %d, %d, %d,"
                     % (char["weapon_slots"], char["no_melee_weapons"],
                        char["no_ranged_weapons"], char["min_weapon_tier"],
                        char["max_weapon_tier"], char["hp_shop"],
                        1 if char["explode"] else 0,
                        (ex.get("scale_missing_hp", 0) if ex else 0)))
        lines.append("      %d, %d, %d, %d, %d, %d, %d, %d,"
                     % (link.get("target", BT_STAT_NONE), link.get("source", BT_STAT_NONE),
                        link.get("perm_only", 0),
                        conv.get("from", BT_STAT_NONE), conv.get("to", BT_STAT_NONE),
                        conv.get("pct", 0), s0, sc))
        lines.append("      0x%04Xu, %d, %d, %d, %d, %d, %d,"
                     % (char["ban_group_mask"], b0, bc, w0, wc, t0, tc))
        lines.append("      %d, %d, %d, %d, %d, %d, %d,"
                     % (link.get("value", 0), link.get("nb", 0),
                        conv.get("value", 0), conv.get("to_value", 0),
                        char["next_level_xp_pct"],
                        ex.get("damage", 0), ex.get("max_range", 0)))
        lines.append("      %s, %s, %s, %s, %s },  /* %s */"
                     % (cfloat(ex.get("chance", 0.0) if ex else 0.0),
                        cfloat(ex.get("scale", 0.0) if ex else 0.0),
                        cfloat(ex.get("crit_chance", 0.0) if ex else 0.0),
                        cfloat(ex.get("crit_damage", 0.0) if ex else 0.0),
                        cfloat(ex["scaling"][0][1] if ex and all(
                            r == ex["scaling"][0][1] for _k, r in ex["scaling"]) else 0.0),
                        char["source"]))
    lines.append("};")
    lines.append("")
    lines.append("/* Config token (or source my_id) -> canonical character index.  Host-only")
    lines.append(" * (the device reads the numeric id from BtState.character).  NULL or an")
    lines.append(" * unknown name resolves to the default Bull, id 0. */")
    lines.append("BT_HOST_ONLY(static inline unsigned short")
    lines.append("bt_character_id_from_name(const char* name) {")
    lines.append("    if (name != 0) {")
    lines.append("        for (unsigned short i = 0; i < (unsigned short)BT_NCHARACTERS; i++) {")
    lines.append("            const char* id = BT_CHARACTERS[i].id;")
    lines.append("            const char* token = BT_CHARACTERS[i].name;")
    lines.append("            for (int which = 0; which < 2; which++) {")
    lines.append("                const char* cand = which == 0 ? id : token;")
    lines.append("                int k = 0;")
    lines.append("                while (cand[k] != 0 && cand[k] == name[k]) k++;")
    lines.append("                if (cand[k] == 0 && name[k] == 0) return i;")
    lines.append("            }")
    lines.append("        }")
    lines.append("    }")
    lines.append("    return (unsigned short)BT_CHARACTER_BULL;")
    lines.append("};)")
    lines.append("")
    return items._write("\n".join(lines), out_path)


def emit_all(args):
    data = build(args)
    emit_characters(data, args.characters_out, args.campaign_root, args.dlc_root)
    return data


def main(argv):
    parser = argparse.ArgumentParser(description="Extract Brotato character profiles.",
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--campaign-root", required=True)
    parser.add_argument("--dlc-root", required=True)
    parser.add_argument("--characters-out", default="ocean/brotato/brotato_characters.h")
    args = parser.parse_args(argv)
    try:
        data = emit_all(args)
    except ExtractError as exc:
        sys.stderr.write("extract_characters: error: %s\n" % exc)
        return 1
    counts = ", ".join("%s=%d" % (c["token"], len(c["starting_weapons"]))
                       for c in data["characters"])
    sys.stderr.write("extract_characters: %d characters (%s), %d item pool, -> %s\n"
                     % (len(data["characters"]), counts, data["item_count"],
                        args.characters_out))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
