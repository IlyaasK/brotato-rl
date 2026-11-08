#!/usr/bin/env python3
"""Export the core's item/upgrade/weapon/character tables for the GDScript bridge.

``bt_core_observe`` encodes the four shop slots, four level-up slots and the
weapon/shop/profile observation block from the core's own generated tables
(``BT_ITEMS`` / ``BT_ITEM_EFFECTS`` in brotato_items.h, ``BT_UPGRADES`` in
brotato_upgrades.h, ``BT_WEAPONS`` in brotato_weapons.h, ``BT_CHARACTERS`` in
brotato_characters.h).  The bridge runs inside the game, so it cannot read those
C tables; this generator emits them as JSON so the GDScript observer reproduces
the encodings exactly instead of guessing, and so that both sides have one
source of truth.

Emitted per item (index order == BT_ITEMS order):

    id      table name without the game's "item_" prefix (e.g. "acid")
    tier    BtItem.tier
    price   BtItem.price
    dom     stat id of the dominant effect (largest |value|, first wins on ties)
    mag     signed value of that effect

Emitted per weapon (index order == BT_WEAPONS order): id / my_id (the two source
strings), tier, kind (0 MELEE, 1 RANGED), cooldown, damage, max_range,
crit_chance.  The bridge maps a live Weapon's id string to the canonical index
with this table and reads tier/kind here; the LIVE node's stats are used for the
observation's value fields.

Emitted per character (index order == BT_CHARACTERS order): id / name only.  The
profile slot limit is NOT a character constant (upgrades change it), so the
bridge reads the live value; only the id -> canonical index map comes from here.

Emitted per upgrade (index order == BT_UPGRADES order): stat, tier, value.
BT_UPGRADE_UNPOOLED rows (real upgrade resources the level-up picker can never
return) are emitted alongside so the bridge can name an unmapped offer instead
of guessing.  The stat contract (BT_STAT_KEYS / BT_STAT_NAMES, index == core
stat id) is emitted as ``stat_ids`` (game effect key -> id) and ``stat_names``
so the observer maps a live offer's effect key to the core's own stat id.

The output is a generated artifact: it lives under the bridge's external root and
is never committed (see patch_manifest.json).

Usage::

    gen_obs_tables.py [--items ocean/brotato/brotato_items.h]
                      [--upgrades ocean/brotato/brotato_upgrades.h]
                      [--output "$BROTATO_EXTERNAL_ROOT/brotato_obs_tables.json"]
                      [--check]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

EXIT_OK = 0
EXIT_ERROR = 2
EXIT_STALE = 3

BRIDGE_DIR = Path(__file__).resolve().parent
REPO_ROOT = BRIDGE_DIR.parent.parent.parent
DEFAULT_ITEMS = str(REPO_ROOT / "ocean" / "brotato" / "brotato_items.h")
DEFAULT_UPGRADES = str(REPO_ROOT / "ocean" / "brotato" / "brotato_upgrades.h")
DEFAULT_WEAPONS = str(REPO_ROOT / "ocean" / "brotato" / "brotato_weapons.h")
DEFAULT_CHARACTERS = str(REPO_ROOT / "ocean" / "brotato" / "brotato_characters.h")
DEFAULT_CORE_HEADER = str(REPO_ROOT / "ocean" / "brotato" / "brotato.h")
DEFAULT_CORE = str(REPO_ROOT / "ocean" / "brotato" / "brotato_core.h")
DEFAULT_STATE = str(REPO_ROOT / "ocean" / "brotato" / "brotato_state.h")
def _default_output() -> str:
    """Generated-artifact destination: $BROTATO_EXTERNAL_ROOT, else the repo's
    migration external root, else the historical $HOME/brotato_mod/external."""
    import os

    override = os.environ.get("BROTATO_EXTERNAL_ROOT", "")
    if override:
        return str(Path(override) / "brotato_obs_tables.json")
    repo = REPO_ROOT / "migration" / "game" / "external"
    if repo.is_dir():
        return str(repo / "brotato_obs_tables.json")
    return str(Path.home() / "brotato_mod" / "external" / "brotato_obs_tables.json")


DEFAULT_OUTPUT = _default_output()

ITEM_ROW = re.compile(
    r'\{\s*(?:BT_HOST_ONLY\()?\s*"(?P<name>[^"]+)"\s*,?\s*\)?\s*,?\s*'
    r'(?P<price>\d+)\s*,\s*(?P<tier>\d+)\s*,\s*'
    r'(?P<first>\d+)\s*,\s*(?P<count>\d+)\s*,\s*(?P<special>\d+)'
)
EFFECT_ROW = re.compile(r'\{\s*(?P<stat>\d+)\s*,\s*(?P<value>-?\d+(?:\.\d+)?)f\s*\}')
UPGRADE_ROW = re.compile(r'\{\s*(?P<stat>\d+)\s*,\s*(?P<tier>\d+)\s*,\s*(?P<value>-?\d+(?:\.\d+)?)f\s*\}')
UNPOOLED_ROW = re.compile(
    r'\{\s*BT_HOST_ONLY\(\s*"(?P<id>[^"]*)"\s*,?\s*\)\s*,?\s*'
    r'(?P<stat>BT_STAT_NONE|\d+)\s*,\s*(?P<tier>\d+)\s*,\s*(?P<value>-?\d+(?:\.\d+)?)f\s*,'
    r'\s*(?P<unlocked>[01])\s*\}')
STAT_KEYS = re.compile(r'BT_STAT_KEYS\[[^\]]*\]\s*=\s*\{(?P<names>[^}]*)\}')
STAT_NAMES = re.compile(r'BT_STAT_NAMES\[[^\]]*\]\s*=\s*\{(?P<names>[^}]*)\}')
CONST_DEF = re.compile(r'#define\s+(?P<name>BT_[A-Z0-9_]+)\s+(?P<value>[0-9]+(?:\.[0-9]+)?)f?')

# BT_WEAPONS rows (brotato_weapons.h, WeaponData): four BT_HOST_ONLY strings
# (id stem, per-tier my_id, name key, icon) then EXACTLY these BtWeaponDef
# scalars, in order, with no padding fields (frozen by WeaponData).  An exact
# count check means an emitter change fails loudly instead of shifting the table.
WEAPON_ROW = re.compile(
    r'BT_HOST_ONLY\(\s*"(?P<id>[^"]*)"\s*,\s*"(?P<my_id>[^"]*)"\s*,\s*'
    r'"(?P<name>[^"]*)"\s*,\s*"(?P<icon>[^"]*)"\s*,?\s*\)\s*,?\s*'
    r'(?P<rest>[^}]*)\}',
    re.S)
WEAPON_FIELDS = ("weapon_id", "stats", "geom", "upgrades_into", "family",
                 "set_first", "set_count", "tier", "kind", "can_be_looted",
                 "is_cursed", "spawns_structure", "flags", "shooting_sounds",
                 "fx_first", "fx_count", "start_first", "start_count", "value",
                 "curse_factor", "cooldown", "damage", "max_range",
                 "crit_chance", "shoot_audio_first", "shoot_audio_count",
                 "hit_audio_first", "hit_audio_count")
WEAPON_F = {name: index for index, name in enumerate(WEAPON_FIELDS)}

# BT_CHARACTERS rows (brotato_characters.h, WeaponCharacters): id then name.
CHARACTER_ROW = re.compile(
    r'BT_HOST_ONLY\(\s*"(?P<id>[^"]*)"\s*,\s*"(?P<name>[^"]*)"\s*,?\s*\)',
    re.S)


def display_path(path: Path) -> str:
    """Host-independent provenance path: repo-relative when inside the repository.

    Absolute paths would make the generated artifact differ per checkout/host, so
    the recorded provenance (and therefore the manifest pin) stays reproducible.
    """
    try:
        return str(path.resolve().relative_to(REPO_ROOT))
    except ValueError:
        return str(path)


def _defined_count(text: str, name: str) -> int | None:
    match = re.search(r'#define\s+%s\s+(\d+)\b' % re.escape(name), text)
    return int(match.group(1)) if match else None


def _tokens(blob: str) -> list[str]:
    return [t.strip() for t in blob.split(",") if t.strip()]


def parse_weapons(path: Path) -> list[dict]:
    """BT_WEAPONS rows in table order (index == canonical weapon id)."""
    text = path.read_text()
    table = _slice(text, "BT_WEAPONS[", "};")
    out = []
    for match in WEAPON_ROW.finditer(table):
        tokens = _tokens(match.group("rest"))
        if len(tokens) != len(WEAPON_FIELDS):
            raise RuntimeError(
                f"{path}: weapon row {len(out)} has {len(tokens)} scalars, "
                f"expected {len(WEAPON_FIELDS)} ({', '.join(WEAPON_FIELDS)})")

        def num(field: str) -> float:
            return float(tokens[WEAPON_F[field]].rstrip("fF"))

        out.append({
            "id": match.group("id"),
            "my_id": match.group("my_id"),
            "name": match.group("name"),
            "tier": int(num("tier")),
            "kind": int(num("kind")),
            "cooldown": int(num("cooldown")),
            "damage": int(num("damage")),
            "max_range": int(num("max_range")),
            "crit_chance": num("crit_chance"),
        })
    if not out:
        raise RuntimeError(f"{path}: no BT_WEAPONS rows parsed")
    declared = _defined_count(text, "BT_NWEAPONS")
    if declared is not None and declared != len(out):
        raise RuntimeError(
            f"{path}: parsed {len(out)} BT_WEAPONS rows but #define BT_NWEAPONS "
            f"is {declared}; the table and the bridge would disagree")
    return out


def parse_characters(path: Path) -> list[dict]:
    """BT_CHARACTERS rows in table order (index == canonical character id)."""
    text = path.read_text()
    table = _slice(text, "BT_CHARACTERS[", "};")
    out = []
    for match in CHARACTER_ROW.finditer(table):
        out.append({"id": match.group("id"), "name": match.group("name"),
                    "index": len(out)})
    if not out:
        raise RuntimeError(f"{path}: no BT_CHARACTERS rows parsed")
    declared = _defined_count(text, "BT_NCHARACTERS")
    if declared is not None and declared != len(out):
        raise RuntimeError(
            f"{path}: parsed {len(out)} BT_CHARACTERS rows but #define "
            f"BT_NCHARACTERS is {declared}")
    return out


def parse_obs_constants(*paths: Path) -> dict:
    """Reads the observation-relevant #defines so the bridge scales exactly like this core."""
    wanted = ("BT_ARENA_W", "BT_ARENA_H", "BT_MAX", "BT_WAVES", "BT_SLOTS", "BT_MAX_PROJ",
              "BT_MAX_CONS", "BT_ACTIONS", "BT_NWEAPONS", "BT_NCHARACTERS",
              # The observation's projectile squared-distance cutoff the sim uses
              # (brotato_core.h bt_core_observe); emitted so the bridge can assert
              # its own PROJ_MAX_D2 against it instead of relying on a literal.
              "BT_OBS_PROJ_MAX_D2",
              # Literal pin of the composed BT_OBS_SIZE (the expression itself is
              # not parseable here); rl_obs.gd asserts its OBS_SIZE against it.
              "BT_OBS_SIZE_PINNED")
    found = {}
    for path in paths:
        if not path.is_file():
            continue
        for match in CONST_DEF.finditer(path.read_text()):
            if match.group("name") in wanted and match.group("name") not in found:
                found[match.group("name")] = float(match.group("value"))
    return found


def parse_stat_contract(path: Path) -> tuple[list[str], list[str]]:
    """The emitted core stat contract (index == core stat id)."""
    text = path.read_text()
    keys = []
    names = []
    for pattern, out in ((STAT_KEYS, keys), (STAT_NAMES, names)):
        match = pattern.search(text)
        if match is None:
            raise RuntimeError(f"{path}: no BT_STAT_KEYS/BT_STAT_NAMES table parsed")
        out.extend(re.findall(r'"([^"]+)"', match.group("names")))
    if len(keys) != len(names):
        raise RuntimeError(f"{path}: {len(keys)} stat keys vs {len(names)} stat names")
    if not keys:
        raise RuntimeError(f"{path}: empty stat contract")
    return keys, names


def _slice(text: str, start_marker: str, end_marker: str) -> str:
    start = text.index(start_marker)
    end = text.index(end_marker, start)
    return text[start:end]


def parse_items(path: Path) -> tuple[list[dict], list[dict]]:
    text = path.read_text()
    table = _slice(text, "BT_ITEMS[", "BT_ITEM_EFFECTS[")
    effects = _slice(text, "BT_ITEM_EFFECTS[", "};")
    rows = [m.groupdict() for m in ITEM_ROW.finditer(table)]
    effect_rows = [m.groupdict() for m in EFFECT_ROW.finditer(effects)]
    if not rows or not effect_rows:
        raise RuntimeError(f"{path}: no item rows parsed")

    items = []
    for row in rows:
        first = int(row["first"])
        count = int(row["count"])
        dom = 0
        mag = 0.0
        for offset in range(count):
            effect = effect_rows[first + offset]
            value = float(effect["value"])
            if abs(value) > abs(mag):
                mag = value
                dom = int(effect["stat"])
        items.append({
            "id": row["name"],
            "tier": int(row["tier"]),
            "price": int(row["price"]),
            "dom": dom,
            "mag": mag,
        })
    return items, effect_rows


def parse_upgrades(path: Path) -> list[dict]:
    text = path.read_text()
    table = _slice(text, "BT_UPGRADES[", "};")
    rows = [m.groupdict() for m in UPGRADE_ROW.finditer(table)]
    if not rows:
        raise RuntimeError(f"{path}: no upgrade rows parsed")
    return [{"stat": int(r["stat"]), "tier": int(r["tier"]), "value": float(r["value"])}
            for r in rows]


def parse_unpooled(path: Path) -> list[dict]:
    """BT_UPGRADE_UNPOOLED rows: real resources the picker can never return."""
    text = path.read_text()
    table = _slice(text, "BT_UPGRADE_UNPOOLED[", "};")
    rows = [m.groupdict() for m in UNPOOLED_ROW.finditer(table)]
    if not rows:
        raise RuntimeError(f"{path}: no BT_UPGRADE_UNPOOLED rows parsed")
    out = []
    for row in rows:
        stat = row["stat"]
        out.append({
            "id": row["id"],
            "stat": None if stat == "BT_STAT_NONE" else int(stat),
            "tier": int(row["tier"]),
            "value": float(row["value"]),
            "unlocked_by_default": int(row["unlocked"]),
        })
    return out


def build(items_path: Path, upgrades_path: Path, core_header: Path | None = None,
          core_path: Path | None = None, state_path: Path | None = None,
          weapons_path: Path | None = None,
          characters_path: Path | None = None) -> dict:
    items, effects = parse_items(items_path)
    upgrades = parse_upgrades(upgrades_path)
    unpooled = parse_unpooled(upgrades_path)
    stat_keys, stat_names = parse_stat_contract(upgrades_path)
    payload = {
        "source": {
            "items": display_path(items_path),
            "items_sha256": hashlib.sha256(items_path.read_bytes()).hexdigest(),
            "upgrades": display_path(upgrades_path),
            "upgrades_sha256": hashlib.sha256(upgrades_path.read_bytes()).hexdigest(),
            "generator": "ocean/brotato/bridge/gen_obs_tables.py",
            "note": "index order matches BT_ITEMS / BT_UPGRADES / BT_WEAPONS / "
                    "BT_CHARACTERS; dom/mag use the core's largest-|value| rule "
                    "with first-wins ties; stat_ids maps a live upgrade effect key "
                    "to the core's own stat id",
        },
        "item_prefix": "item_",
        "items": items,
        "upgrades": upgrades,
        "unpooled_upgrades": unpooled,
        "stat_ids": {key: index for index, key in enumerate(stat_keys)},
        "stat_names": stat_names,
        "counts": {"items": len(items), "item_effects": len(effects),
                   "upgrades": len(upgrades), "unpooled_upgrades": len(unpooled),
                   "stats": len(stat_names)},
    }

    # Weapon / character tables for the weapon+shop+profile observation block
    # (bt_weapon_observe_append). Emitted only when the generated headers exist;
    # a missing table is reported by the caller, never silently skipped here.
    if weapons_path is not None and weapons_path.is_file():
        weapons = parse_weapons(weapons_path)
        payload["source"]["weapons"] = display_path(weapons_path)
        payload["source"]["weapons_sha256"] = hashlib.sha256(
            weapons_path.read_bytes()).hexdigest()
        payload["weapon_prefix"] = "weapon_"
        payload["weapons"] = weapons
        payload["counts"]["weapons"] = len(weapons)
    if characters_path is not None and characters_path.is_file():
        characters = parse_characters(characters_path)
        payload["source"]["characters"] = display_path(characters_path)
        payload["source"]["characters_sha256"] = hashlib.sha256(
            characters_path.read_bytes()).hexdigest()
        payload["characters"] = characters
        payload["counts"]["characters"] = len(characters)

    if core_header is not None and core_header.is_file():
        payload["source"]["core_header"] = display_path(core_header)
        payload["source"]["core_header_sha256"] = hashlib.sha256(core_header.read_bytes()).hexdigest()

    # Core revision provenance: the observation is only meaningful together with
    # the constants and headers it was ported from, because the policy artifact
    # was trained against whichever revision was live at training time.
    revision = {"obs_constants": parse_obs_constants(
        *[p for p in (state_path, core_path, weapons_path, characters_path) if p])}
    for label, path in (("core_header", core_header), ("core", core_path), ("state", state_path)):
        if path is not None and path.is_file():
            revision[label + "_path"] = display_path(path)
            revision[label + "_sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
    payload["core_revision"] = revision
    return payload


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--items", default=DEFAULT_ITEMS)
    parser.add_argument("--upgrades", default=DEFAULT_UPGRADES,
                        help="brotato_upgrades.h: BT_UPGRADES / BT_UPGRADE_UNPOOLED "
                             "rows and the BT_STAT_KEYS / BT_STAT_NAMES contract")
    parser.add_argument("--weapons", default=DEFAULT_WEAPONS,
                        help="brotato_weapons.h: BT_WEAPONS rows (canonical weapon ids)")
    parser.add_argument("--characters", default=DEFAULT_CHARACTERS,
                        help="brotato_characters.h: BT_CHARACTERS rows (canonical "
                             "character ids for the profile observation)")
    parser.add_argument("--core-header", default=DEFAULT_CORE_HEADER,
                        help="brotato.h (revision provenance only)")
    parser.add_argument("--core", default=DEFAULT_CORE, help="brotato_core.h (observer source)")
    parser.add_argument("--state", default=DEFAULT_STATE, help="brotato_state.h (schema constants)")
    parser.add_argument("--output", default=DEFAULT_OUTPUT)
    parser.add_argument("--check", action="store_true",
                        help="fail if the output file is missing or stale")
    args = parser.parse_args(argv)

    try:
        # The observation needs all four tables; a missing generated
        # header is a hard error so the bridge can never run on a stale schema.
        for label, path in (("weapons", args.weapons), ("characters", args.characters)):
            if not Path(path).is_file():
                raise RuntimeError(f"{path} does not exist; the weapon/shop/profile "
                                   f"observation block needs the {label} table")
        payload = build(Path(args.items), Path(args.upgrades), Path(args.core_header),
                        Path(args.core), Path(args.state),
                        Path(args.weapons), Path(args.characters))
    except (OSError, ValueError, RuntimeError) as exc:
        print(f"generate failed: {exc}", file=sys.stderr)
        return EXIT_ERROR

    blob = json.dumps(payload, indent="\t") + "\n"
    digest = hashlib.sha256(blob.encode()).hexdigest()
    output = Path(args.output)

    if args.check:
        if not output.is_file():
            print(f"stale: {output} does not exist", file=sys.stderr)
            return EXIT_STALE
        existing = hashlib.sha256(output.read_bytes()).hexdigest()
        if existing != digest:
            print(f"stale: {output} sha256 {existing} != {digest}", file=sys.stderr)
            return EXIT_STALE
        print(f"up to date: {output} sha256={existing}")
        return EXIT_OK

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(blob)
    print(f"wrote {output} sha256={digest}")
    print(f"  items={payload['counts']['items']} effects={payload['counts']['item_effects']} "
          f"upgrades={payload['counts']['upgrades']} "
          f"unpooled_upgrades={payload['counts']['unpooled_upgrades']} "
          f"stats={payload['counts']['stats']} "
          f"weapons={payload['counts']['weapons']} "
          f"characters={payload['counts']['characters']}")
    return EXIT_OK


if __name__ == "__main__":
    raise SystemExit(main())
