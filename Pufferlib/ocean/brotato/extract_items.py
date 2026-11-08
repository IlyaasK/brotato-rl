#!/usr/bin/env python3
"""Extract the complete Brotato item pool + structured item-effect graph into
the portable headers ``brotato_items.h`` and ``brotato_item_effects.h``.

Ground truth is the recovered Godot 3.x project (campaign root
``brotato_recovered`` + ``brotato_abyss`` DLC root).  Every row is resolved
from the real ``.tres`` resources through the shared ``extract_physics``
resolver (ExtResource / SubResource / scene inheritance); nothing is
hand-authored and no value is invented.  Re-running reproduces byte-identical
headers (no RNG, stable ordering).

Discovery is the *source graph*, not a directory glob: the canonical item pool
is the ``items`` array of ``singletons/item_service.tscn`` (campaign) plus the
``items`` array of ``dlcs/dlc_1/dlc_data.tres`` (DLC).  The directory trees
(``items/all/<dir>``, ``dlcs/dlc_1/items/<dir>``) are cross-checked against it
and every mismatch is reported.

What is emitted
---------------
* ``brotato_items.h`` -- one ``BtItem`` row per item (legacy core fields kept
  verbatim, extended with the shop/limit/curse/tag/group fields), the legacy
  contiguous ``BT_ITEM_EFFECTS`` stat pool the current core consumes, and the
  Bull / endless shop-ban lists.
* ``brotato_item_effects.h`` -- the structured effect graph: one row per
  effect resource occurrence (class opcode, target/source stat keys, flat
  args, nested BurningData, WeaponStats for pets/turrets/projectiles, packed
  scene geometry, spawn timing, crit/lifesteal/penetration), the host-only
  interned string table (device uses integer ids), item tags, item groups and
  the DLC curse constants.  ``ExtraEnemyEffect.extra_group`` is a numeric
  ordinal into ``BT_ITEM_EXTRA_GROUPS`` (brotato_waves.h), resolved from the
  effect's ``extra_group_data`` wave-group resource by the same discovery
  routine the wave generator uses.
* a JSON provenance document (``--provenance-out``) naming the source path of
  every item and every effect.

Usage::

    python3 ocean/brotato/extract_items.py \\
        --campaign-root /path/brotato_recovered \\
        --dlc-root      /path/brotato_abyss \\
        --items-out     ocean/brotato/brotato_items.h \\
        --effects-out   ocean/brotato/brotato_item_effects.h \\
        --provenance-out /path/elsewhere/brotato_items.json
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
_SUB_ID_RE = re.compile(r"SubResource\(\s*(\d+)\s*\)")
_STR_RE = re.compile(r'"([^"]*)"')
_SCALING_RE = re.compile(r'\[\s*"([^"]+)"\s*,\s*([-\d.eE+]+)\s*\]')
_CLASS_NAME_RE = re.compile(r'^\s*class_name\s+([A-Za-z_][A-Za-z0-9_]*)', re.M)
_EXTENDS_RE = re.compile(r'^\s*extends\s+([A-Za-z_][A-Za-z0-9_]*)', re.M)

# Canonical core stat ids (brotato_core.h bt_apply_item / brotato.h stat_names).
STAT_KEYS = (
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
)
STAT_ID = {key: sid for key, sid in STAT_KEYS}
# StatCapEffect.key -> capped core stat (utils.gd get_capped_stat match table).
CAP_KEY_TO_STAT = {"hp_cap": 0, "speed_cap": 3, "dodge_cap": 4, "crit_chance_cap": 9}
BT_STAT_NONE = 0xFF
BT_ITEM_NONE = 0xFFFF

# Item / effect flag bits (also the layout of BtItemEff.flags).
IEFLAG_EXPLODE = 0x00000001
IEFLAG_EXPLODE_BELOW_HP = 0x00000002
IEFLAG_PET = 0x00000004
IEFLAG_STRUCTURE = 0x00000008
IEFLAG_TURRET = 0x00000010
IEFLAG_BURN = 0x00000020
IEFLAG_TEMP_STATS = 0x00000040
IEFLAG_STAT_CAP = 0x00000080
IEFLAG_STAT_LINK = 0x00000100
IEFLAG_PERCENT_DAMAGE = 0x00000200
IEFLAG_EXTRA_ENEMY = 0x00000400
IEFLAG_PROJECTILE = 0x00000800
IEFLAG_DOUBLE_VALUE = 0x00001000
IEFLAG_DOUBLE_KEY_VALUE = 0x00002000
IEFLAG_WEAPON_COOLDOWN = 0x00004000
IEFLAG_SUB_EFFECTS = 0x00008000
IEFLAG_CHANCE_STAT_DAMAGE = 0x00010000
IEFLAG_SWAP_STATS = 0x00020000
IEFLAG_GAIN_WHILE_BURNING = 0x00040000
IEFLAG_BUILDER_TURRET = 0x00080000
IEFLAG_UNSUPPORTED = 0x80000000

FLAG_DOCS = (
    (IEFLAG_EXPLODE, "BT_ITFLAG_EXPLODE", "explode-on-hit / explosion scene or nested ItemExplodingEffect"),
    (IEFLAG_EXPLODE_BELOW_HP, "BT_ITFLAG_EXPLODE_BELOW_HP", "ItemExplodingWhenBelowHPEffect (hp_threshold present)"),
    (IEFLAG_PET, "BT_ITFLAG_PET", "spawns a pet (PetEffect/Blazemander/BonkDog/CatlingGun/BotOMine/Ratzilla/Lootworm/DocMoth/Scapegoat)"),
    (IEFLAG_STRUCTURE, "BT_ITFLAG_STRUCTURE", "StructureEffect"),
    (IEFLAG_TURRET, "BT_ITFLAG_TURRET", "TurretEffect / BuilderTurretEffect"),
    (IEFLAG_BURN, "BT_ITFLAG_BURN", "carries BurningData (BurnChanceEffect / BurningEffect)"),
    (IEFLAG_TEMP_STATS, "BT_ITFLAG_TEMP_STATS", "TempStatsPerIntervalEffect"),
    (IEFLAG_STAT_CAP, "BT_ITFLAG_STAT_CAP", "StatCapEffect"),
    (IEFLAG_STAT_LINK, "BT_ITFLAG_STAT_LINK", "GainStatForEveryStatEffect dynamic stat link"),
    (IEFLAG_PERCENT_DAMAGE, "BT_ITFLAG_PERCENT_DAMAGE", "PercentDamageEffect"),
    (IEFLAG_EXTRA_ENEMY, "BT_ITFLAG_EXTRA_ENEMY", "ExtraEnemyEffect"),
    (IEFLAG_PROJECTILE, "BT_ITFLAG_PROJECTILE", "ProjectileEffect"),
    (IEFLAG_DOUBLE_VALUE, "BT_ITFLAG_DOUBLE_VALUE", "DoubleValueEffect"),
    (IEFLAG_DOUBLE_KEY_VALUE, "BT_ITFLAG_DOUBLE_KEY_VALUE", "DoubleKeyValueEffect"),
    (IEFLAG_WEAPON_COOLDOWN, "BT_ITFLAG_WEAPON_COOLDOWN", "WeaponCooldownEffect"),
    (IEFLAG_SUB_EFFECTS, "BT_ITFLAG_SUB_EFFECTS", "EffectWithSubEffects / structure `effects` array"),
    (IEFLAG_CHANCE_STAT_DAMAGE, "BT_ITFLAG_CHANCE_STAT_DAMAGE", "ChanceStatDamageEffect"),
    (IEFLAG_SWAP_STATS, "BT_ITFLAG_SWAP_STATS", "SwapMaxMinStatEffect"),
    (IEFLAG_GAIN_WHILE_BURNING, "BT_ITFLAG_GAIN_WHILE_BURNING", "GainStatForKilledEnemiesWhileBurning"),
    (IEFLAG_BUILDER_TURRET, "BT_ITFLAG_BUILDER_TURRET", "BuilderTurretEffect / upgrade"),
    (IEFLAG_UNSUPPORTED, "BT_ITFLAG_UNSUPPORTED", "effect script outside effects/ (see provenance JSON)"),
)


# --------------------------------------------------------------------------
# Pet movement / interaction metadata (BT_PET_AI, brotato_pet_motion.h)
#
# Every pet class reachable through a PetEffect subclass instantiates ONE scene;
# the scene -- not the effect -- decides how the pet moves (its MovementBehavior
# node's script and exported fields), how fast (its Stats resource) and which
# interaction zones it exposes (each Area2D's CircleShape2D, with the uniform
# scale Godot applies on the CollisionShape2D leaf composed into the radius).
# PET_AI_CLASSES maps the effect opcode to the documented zone node names; the
# emitted BT_PET_Z_* ordinals index BT_PET_ZONE_[RXY].
PET_AI_CLASSES = {
    "PetEffect": {},
    "LootwormEffect": {"attract": "ItemAttractArea", "pickup": "ItemPickUpArea",
                       "tree": "TreeArea2D"},
    "DocMothEffect": {"boost": "BoostZone"},
    "ScapegoatEffect": {"heal": "HealingTriggeringZone"},
    "BonkDogEffect": {"target": "TargetBehavior/Range"},
    "CatlingGunEffect": {"target": "TargetTriggerZone", "player": "PlayerTriggerZone"},
    "BotOMineEffect": {"target": "TargetTriggerZone"},
    "BlazemanderEffect": {"target": "TargetBehavior/Range", "detect": "Enemy_detection"},
    "RatzillaEffect": {"target": "TargetBehavior/Range"},
}
# Zone ordinal -> emitting struct field, in the emitted BT_PET_Z_* order.
PET_AI_ZONES = ("boost", "attract", "pickup", "tree", "heal", "target", "player", "detect")
# TargetBehavior script stem -> BT_PET_TARGET_* ("" -> none: the base TargetBehavior
# leaves current_target null, so a follow-target pet has no target to chase).
PET_TARGET_KINDS = {
    "target_behavior": "BT_PET_TARGET_NONE",
    "closest_enemy_target_behavior": "BT_PET_TARGET_ENEMY",
    "closest_non_burning_enemy_target_behavior": "BT_PET_TARGET_ENEMY",
    "closest_player_target_behavior": "BT_PET_TARGET_PLAYER",
    "lootworm_target_behavior": "BT_PET_TARGET_LOOT",
}
# MovementBehavior script stem -> BT_PET_MOVE_* (follow_target splits on its own
# stop_close_to_target export).
PET_MOVE_KINDS = {
    "follow_target_movement_behavior": "BT_PET_MOVE_FOLLOW_TARGET",
    "target_rand_pos_movement_behavior": "BT_PET_MOVE_RAND_POS",
    "patrol_around_the_map_movement_behavior": "BT_PET_MOVE_PATROL",
    "follow_rand_pos_around_player_movement_behavior": "BT_PET_MOVE_FOLLOW_RAND_POS",
}
PET_SCRIPT_STEM = re.compile(r"([^/]+)\.gd$")
# Keys of the `effects` array a pet zone applies to a player inside it (only the
# DocMoth carries one).  Unknown keys abort rather than being dropped.
PET_ZONE_FX_KEYS = {
    "hp_regen_bonus": 0,          # BT_PET_FX_HP_REGEN_BONUS
    "double_lifesteal_bonus": 1,  # BT_PET_FX_DOUBLE_LIFESTEAL
}


def cfloat(value):
    """Deterministic C float literal (always a decimal point + `f`)."""
    value = float(value)
    if value == 0.0:
        value = 0.0
    text = "%.6g" % value
    if "e" in text or "E" in text:
        return text + "f"
    if "." not in text:
        text += ".0"
    return text + "f"


def sanitize(token):
    return re.sub(r"[^A-Za-z0-9_]", "_", token).upper()


def opcode_macro(cls):
    """CamelCase effect class -> UPPER_SNAKE opcode stem (trailing Effect dropped)."""
    text = re.sub(r"(?<=[a-z0-9])(?=[A-Z])", "_", cls)
    text = re.sub(r"(?<=[A-Z])(?=[A-Z][a-z])", "_", text).upper()
    if text.endswith("_EFFECT") and text != "EFFECT":
        text = text[:-len("_EFFECT")]
    return "BT_ITEMEFF_" + re.sub(r"[^A-Z0-9_]", "_", text)


# --------------------------------------------------------------------------
# Class index (class_name / extends) for effect taxonomy
# --------------------------------------------------------------------------
def build_class_index(roots):
    by_path = {}
    parent = {}
    for root in roots:
        for dirpath, _dirs, files in os.walk(root):
            for name in sorted(files):
                if not name.endswith(".gd"):
                    continue
                path = os.path.join(dirpath, name)
                try:
                    with open(path, "r", encoding="utf-8", errors="replace") as handle:
                        text = handle.read(4096)
                except OSError:
                    continue
                match = _CLASS_NAME_RE.search(text)
                if not match:
                    continue
                cls = match.group(1)
                parent[cls] = (_EXTENDS_RE.search(text).group(1)
                               if _EXTENDS_RE.search(text) else None)
                by_path[os.path.abspath(path)] = cls
    return by_path, parent


def ancestry(cls, parent):
    chain = []
    seen = set()
    while cls and cls not in seen:
        seen.add(cls)
        chain.append(cls)
        cls = parent.get(cls)
    return chain


# --------------------------------------------------------------------------
# String interning
# --------------------------------------------------------------------------
class Strings(object):
    def __init__(self):
        self.ids = {"": 0}
        self.values = [""]
        self.macros = {"": "BT_ISTR_NONE"}
        self.macro_set = {"BT_ISTR_NONE"}

    def intern(self, text):
        if text is None:
            text = ""
        if text not in self.ids:
            macro = "BT_ISTR_" + sanitize(text)
            # Case-only / punctuation-only collisions (e.g. effect_knockback vs
            # EFFECT_KNOCKBACK) get a deterministic numeric suffix.
            while macro in self.macro_set:
                macro += "_%d" % len(self.values)
            self.macro_set.add(macro)
            self.ids[text] = len(self.values)
            self.values.append(text)
            self.macros[text] = macro
        return self.ids[text]


# --------------------------------------------------------------------------
# Item discovery
# --------------------------------------------------------------------------
def _array_refs(pf, props, key):
    return [pf.ext_path(i) for i in _ANY_REF_RE.findall(props.get(key, "") or "")]


def read_pool(resolver, campaign_root, dlc_root):
    svc = physics.parse_file(os.path.join(campaign_root, "singletons/item_service.tscn"))
    roots = [n for n in svc.nodes if n["parent"] is None]
    if len(roots) != 1:
        raise ExtractError("item_service.tscn: expected one root node")
    campaign = _array_refs(svc, roots[0]["props"], "items")
    dlc_file = os.path.join(dlc_root, "dlcs", "dlc_1", "dlc_data.tres")
    dpf = physics.parse_file(dlc_file)
    dlc = _array_refs(dpf, dpf.root_props, "items")
    return campaign, dlc


def _scan_item_dirs(root, subdir):
    """{res:// path -> top directory name} for every item_data.tres under subdir."""
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
            script = pf.root_props.get("script")
            if not script:
                continue
            match = physics._EXTREF_RE.match(script)
            if not match or not pf.ext_path(match.group(1)).endswith("item_data.gd"):
                continue
            rel = os.path.relpath(path, root).replace(os.sep, "/")
            top = os.path.relpath(dirpath, base).split(os.sep)[0]
            found["res://" + rel] = top
    return found


def validate_dirs(root, subdir, pool, label):
    scan = _scan_item_dirs(root, subdir)
    dirs = sorted({d for d in os.listdir(os.path.join(root, subdir))
                   if os.path.isdir(os.path.join(root, subdir, d))})
    missing = [p for p in pool if p not in scan]
    extra = [p for p in scan if p not in pool]
    covered = {scan[p] for p in pool if p in scan}
    uncovered = [d for d in dirs if d not in covered]
    if missing:
        raise ExtractError("%s: %d item resources absent from the tree (e.g. %s)"
                           % (label, len(missing), ", ".join(sorted(missing)[:3])))
    if extra:
        raise ExtractError("%s: %d item resources in the tree but not in the service "
                           "array (e.g. %s)" % (label, len(extra), ", ".join(sorted(extra)[:3])))
    if uncovered:
        raise ExtractError("%s: %d directories hold no registered item: %s"
                           % (label, len(uncovered), ", ".join(uncovered)))
    return {"dirs": len(dirs), "items": len(pool)}


# --------------------------------------------------------------------------
# Effect graph
# --------------------------------------------------------------------------
class Builder(object):
    def __init__(self, resolver, class_by_path, class_parent, extra_groups):
        self.R = resolver
        self.class_by_path = class_by_path
        self.class_parent = class_parent
        # ExtraEnemyEffect.extra_group_data path -> BT_ITEM_EXTRA_GROUPS ordinal
        # (brotato_waves.h).  Shared with extract_gameplay so both agree.
        self.extra_groups = extra_groups
        self.strings = Strings()
        self.rows = []            # BtItemEff dicts
        self.eff_index = []       # row indices (item effects + sub effects)
        self.wstats = {}          # res path -> index
        self.wstats_rows = []
        self.scaling = []         # (stat_str_id, ratio)
        self.burning = {}
        self.burning_rows = []
        self.scenes = {}
        self.scene_rows = []
        self.pets = []            # BtPetAI dicts (pet motion / interaction zones)
        self.pet_fx = []          # (key_kind, value, value2) rows of zone effects
        self.pet_scene = ""       # scene currently resolved by pet_ai (error text)
        self.customargs = []      # (arg_index, arg_sign, arg_format, arg_value, arg_key)
        self.items = []
        self.unsupported = []

    # -- class helpers -----------------------------------------------------
    def script_class(self, pf, props):
        script = props.get("script")
        if not script:
            raise ExtractError("%s: resource has no script" % pf.path)
        match = physics._EXTREF_RE.match(script)
        if not match:
            raise ExtractError("%s: unsupported script ref %r" % (pf.path, script))
        res_path = pf.ext_path(match.group(1))
        abs_path = self.R.find(res_path)
        cls = self.class_by_path.get(os.path.abspath(abs_path))
        if cls is None:
            raise ExtractError("%s: script %s has no class_name" % (pf.path, res_path))
        return res_path, cls

    # -- sub-resource string values ---------------------------------------
    def intern_value(self, props, key):
        value = props.get(key)
        if value is None:
            return 0
        return self.strings.intern(get_str(props, key, ""))

    # -- weapon stats ------------------------------------------------------
    def weapon_stats(self, res_path):
        if res_path in self.wstats:
            return self.wstats[res_path]
        pf = self.R.parse(res_path)
        props = pf.root_props
        _, cls = self.script_class(pf, props)
        kind = {"RangedWeaponStats": 2, "MeleeWeaponStats": 1}.get(cls, 0)
        first = len(self.scaling)
        for stat_key, ratio in _SCALING_RE.findall(props.get("scaling_stats", "") or ""):
            self.scaling.append((self.strings.intern(stat_key), float(ratio)))
        row = {
            "kind": kind,
            "cooldown": get_int(props, "cooldown", 60),
            "damage": get_int(props, "damage", 1),
            "min_range": get_int(props, "min_range", 0),
            "max_range": get_int(props, "max_range", 150),
            "knockback": get_int(props, "knockback", 0),
            "recoil": get_int(props, "recoil", 25),
            "sound_db_mod": get_int(props, "sound_db_mod", -5),
            "additional_cd_every": get_int(props, "additional_cooldown_every_x_shots", -1),
            "speed_percent_modifier": get_int(props, "speed_percent_modifier", 0),
            "nb_projectiles": get_int(props, "nb_projectiles", 1),
            "piercing": get_int(props, "piercing", 0),
            "bounce": get_int(props, "bounce", 0),
            "projectile_speed": get_int(props, "projectile_speed", 3000),
            "accuracy": get_float(props, "accuracy", 1.0),
            "crit_chance": get_float(props, "crit_chance", 0.03),
            "crit_damage": get_float(props, "crit_damage", 1.5),
            "knockback_piercing": get_float(props, "knockback_piercing", 0.0),
            "effect_scale": get_float(props, "effect_scale", 1.0),
            "lifesteal": get_float(props, "lifesteal", 0.0),
            "recoil_duration": get_float(props, "recoil_duration", 0.1),
            "additional_cd_multiplier": get_float(props, "additional_cooldown_multiplier", -1.0),
            "projectile_spread": get_float(props, "projectile_spread", 0.0),
            "piercing_dmg_reduction": get_float(props, "piercing_dmg_reduction", 0.5),
            "bounce_dmg_reduction": get_float(props, "bounce_dmg_reduction", 0.5),
            "can_have_positive_knockback": get_bool(props, "can_have_positive_knockback", True),
            "can_have_negative_knockback": get_bool(props, "can_have_negative_knockback", False),
            "is_healing": get_bool(props, "is_healing", False),
            "can_bounce": get_bool(props, "can_bounce", True),
            "increase_proj_speed": get_bool(props, "increase_projectile_speed_with_range", False),
            "deal_dmg_on_return": get_bool(props, "deal_dmg_on_return", False),
            "alternate_attack_type": get_bool(props, "alternate_attack_type", False),
            "attack_type": get_int(props, "attack_type", 0),
            "scaling_first": first,
            "scaling_count": len(self.scaling) - first,
            "projectile_scene": 0,
        }
        refs = _ANY_REF_RE.findall(props.get("projectile_scene", "") or "")
        if refs:
            row["projectile_scene"] = self.strings.intern(pf.ext_path(refs[0]))
            self.scene(refs and pf.ext_path(refs[0]))
        index = len(self.wstats_rows)
        self.wstats_rows.append(row)
        self.wstats[res_path] = index
        return index

    # -- burning data ------------------------------------------------------
    def burning_data(self, res_path):
        if res_path in self.burning:
            return self.burning[res_path]
        pf = self.R.parse(res_path)
        props = pf.root_props
        first = len(self.scaling)
        for stat_key, ratio in _SCALING_RE.findall(props.get("scaling_stats", "") or ""):
            self.scaling.append((self.strings.intern(stat_key), float(ratio)))
        row = {
            "chance": get_float(props, "chance", 0.0),
            "damage": get_int(props, "damage", 0),
            "duration": get_int(props, "duration", 0),
            "spread": get_int(props, "spread", 0),
            "is_global_burn": get_bool(props, "is_global_burn", False),
            "scaling_first": first,
            "scaling_count": len(self.scaling) - first,
        }
        index = len(self.burning_rows)
        self.burning_rows.append(row)
        self.burning[res_path] = index
        return index

    # -- scene geometry ----------------------------------------------------
    def scene(self, res_path):
        if res_path in self.scenes:
            return self.scenes[res_path]
        row = {
            "scene": self.strings.intern(res_path),
            "kind": 0, "reason": 0, "mode": 0,
            "has_hitbox": 0, "has_hurtbox": 0,
            "body": (0.0, 0.0, 0.0), "hurt": (0.0, 0.0, 0.0), "hit": (0.0, 0.0, 0.0),
        }
        try:
            table = physics.flatten_scene(self.R, res_path, {})
        except ExtractError as exc:
            row["reason"] = self.strings.intern(str(exc))
            index = len(self.scene_rows)
            self.scene_rows.append(row)
            self.scenes[res_path] = index
            return index
        root = table.get(".") or {}
        pairs = (("body", "Collision", "Collision"),
                 ("hurt", "Hurtbox", "Hurtbox/Collision"),
                 ("hit", "Hitbox", "Hitbox/Collision"))
        if root.get("__type") == "RigidBody2D":
            row["kind"] = 1
            row["mode"] = int(float(root.get("mode", 0) or 0))
        else:
            row["kind"] = 2
        for key, area, shape in pairs:
            try:
                circle = physics.circle_from_path(self.R, table, area, shape, res_path)
            except ExtractError:
                circle = None
            if circle:
                row[key] = circle
                if key == "hit":
                    row["has_hitbox"] = 1
                if key == "hurt":
                    row["has_hurtbox"] = 1
        index = len(self.scene_rows)
        self.scene_rows.append(row)
        self.scenes[res_path] = index
        return index

    # -- pet movement / interaction metadata -------------------------------
    def zone_shape(self, table, zone_path):
        """Shape child path of an Area2D node (name varies: CollisionShape2D,
        Collision, ...): the direct child that carries a `shape` property."""
        prefix = zone_path + "/"
        found = [key for key in table
                 if key.startswith(prefix) and "shape" in table[key]
                 and "/" not in key[len(prefix):]]
        if len(found) != 1:
            raise ExtractError("%s: expected one shape child under %r, found %d"
                               % (self.pet_scene, zone_path, len(found)))
        return found[0]

    def zone_circle(self, table, zone_path):
        """(radius, x, y) of an Area2D's circle, composing the uniform scales
        along the node path.

        ``extract_physics.compose_circle`` only folds a scene-root scale and
        rejects a scale on any other node; the recovered pet scenes (the
        Lootworm's ItemAttractArea / ItemPickUpArea) instead put a uniform
        scale on the CollisionShape2D leaf itself, which Godot folds into the
        circle radius.  Uniform, upright scales compose multiplicatively here.
        """
        where = self.pet_scene
        child = self.zone_shape(table, zone_path)
        radius = physics.shape_circle(self.R, table[child].get("shape"),
                                      "%s %s.shape" % (where, child))
        root = table.get(".") or {}
        scale = 1.0
        rx, ry = physics.parse_vec2(root.get("scale"), "%s .scale" % where, default=(1.0, 1.0))
        if abs(rx - ry) > 1e-6:
            raise ExtractError("%s: root has non-uniform scale (%g, %g)" % (where, rx, ry))
        scale = rx
        cx = cy = 0.0
        for path in (zone_path, child):
            props = table.get(path) or {}
            px, py = physics.parse_vec2(props.get("position"),
                                        "%s %s.position" % (where, path))
            sx, sy = physics.parse_vec2(props.get("scale"),
                                        "%s %s.scale" % (where, path), default=(1.0, 1.0))
            if abs(sx - sy) > 1e-6:
                raise ExtractError("%s: node %r has non-uniform scale (%g, %g)"
                                   % (where, path, sx, sy))
            cx += scale * px
            cy += scale * py
            scale *= sx
        return radius * scale, cx, cy

    def pet_ai(self, res_path, opcode_name):
        """One BT_PET_AI row, resolved from the pet's scene + Stats resource."""
        self.pet_scene = res_path
        table = physics.flatten_scene(self.R, res_path, {})
        root = table.get(".") or {}
        script = root.get("script")
        if not isinstance(script, tuple) or script[0] != "ext":
            raise ExtractError("%s: pet scene root has no script" % res_path)
        cls = self.class_by_path.get(os.path.abspath(self.R.find(script[1])))
        if cls is None:
            raise ExtractError("%s: script %s has no class_name" % (res_path, script[1]))
        stem = opcode_name[:-len("Effect")] if opcode_name.endswith("Effect") else opcode_name
        if cls != stem:
            raise ExtractError("%s: scene script %s is not the %s pet class"
                               % (res_path, script[1], stem))
        row = {
            "opcode": opcode_name,
            "opcode_macro": opcode_macro(opcode_name),
            "scene": res_path,
            "move": "BT_PET_MOVE_FOLLOW_TARGET",
            "target": "BT_PET_TARGET_NONE",
            "stop_close": 0,
            "is_melee": 0,
            "allow_within": 1,
            "fallback_any": 0,
            "exclude_elite_boss": 0,
            "target_player_if_none": 0,
            "melee_wstats": BT_ITEM_NONE,
            "speed": 0.0,
            "speed_rand": 0.0,
            "stop_dist": 30.0,          # FollowTargetMovementBehavior.distance_to_target
            "rand_rand": 0.0,
            "patrol_rf": 0.8,           # PatrolAroundTheMapMovementBehavior.radius_factor
            "edge_x": 0.0,              # entity.gd init: -sprite.texture.size / 2
            "edge_y": 0.0,
            "health_base": 0.0,
            "health_per_wave": 0.0,
            "collision_layer": 0,       # scene root RigidBody2D.collision_layer
            "collision_mask": 0,        # scene root RigidBody2D.collision_mask
            "zones": {},
        }
        # The pet scene root's collision filter (entity.tscn's 0/0 overridden by
        # pet.tscn 512/640 and again by lootworm/scapegoat 0/128, doc_moth 0/0).
        row["collision_layer"] = get_int(root, "collision_layer", 0)
        row["collision_mask"] = get_int(root, "collision_mask", 0)
        stats_ref = root.get("stats")
        if isinstance(stats_ref, tuple) and stats_ref[0] == "ext":
            props = self.R.parse(stats_ref[1]).root_props
            row["speed"] = get_float(props, "speed", 0.0)
            row["speed_rand"] = get_float(props, "speed_randomization", 0.0)
            row["health_base"] = get_float(props, "health", 0.0)
            row["health_per_wave"] = get_float(props, "health_increase_each_wave", 0.0)
        wstats_ref = root.get("weapon_stats")
        if isinstance(wstats_ref, tuple) and wstats_ref[0] == "ext":
            row["melee_wstats"] = self.weapon_stats(wstats_ref[1])
            row["is_melee"] = 1
        # Lootworm.tree_damage_ratio (scene export): a tree tougher than
        # hitbox.damage * ratio is hit for max_health / ratio instead.
        row["tree_ratio"] = get_float(root, "tree_damage_ratio", 0.0)
        viewport = table.get("Animation/Viewport")
        if viewport is not None:
            vx, vy = physics.parse_vec2(viewport.get("size"),
                                        "%s Animation/Viewport.size" % res_path, default=(0.0, 0.0))
            row["edge_x"] = vx * 0.5
            row["edge_y"] = vy * 0.5
        mb = table.get("MovementBehavior")
        if not isinstance(mb, dict) or not isinstance(mb.get("script"), tuple):
            raise ExtractError("%s: no MovementBehavior script" % res_path)
        move_stem = PET_SCRIPT_STEM.search(mb["script"][1]).group(1)
        if move_stem not in PET_MOVE_KINDS:
            raise ExtractError("%s: unhandled movement behaviour %s" % (res_path, move_stem))
        row["move"] = PET_MOVE_KINDS[move_stem]
        if move_stem == "follow_target_movement_behavior":
            row["stop_close"] = 1 if get_bool(mb, "stop_close_to_target", False) else 0
            row["stop_dist"] = get_float(mb, "distance_to_target", 30.0)
            if row["stop_close"]:
                row["move"] = "BT_PET_MOVE_FOLLOW_TARGET_STOP"
        elif move_stem == "follow_rand_pos_around_player_movement_behavior":
            row["stop_dist"] = get_float(mb, "range_around_player", 200.0)
            row["rand_rand"] = get_float(mb, "range_randomization", 0.0)
            row["allow_within"] = 1 if get_bool(mb, "allow_within", True) else 0
        elif move_stem == "patrol_around_the_map_movement_behavior":
            row["patrol_rf"] = get_float(mb, "radius_factor", 0.8)
        tb = table.get("TargetBehavior")
        if not isinstance(tb, dict) or not isinstance(tb.get("script"), tuple):
            raise ExtractError("%s: no TargetBehavior script" % res_path)
        target_stem = PET_SCRIPT_STEM.search(tb["script"][1]).group(1)
        if target_stem not in PET_TARGET_KINDS:
            raise ExtractError("%s: unhandled target behaviour %s" % (res_path, target_stem))
        row["target"] = PET_TARGET_KINDS[target_stem]
        if target_stem == "closest_enemy_target_behavior":
            # no in-range target -> the FIRST enemy in get_all_enemies(false)
            row["fallback_any"] = 1
            row["exclude_elite_boss"] = 1 if get_bool(tb, "exclude_elite_and_boss_target", False) else 0
        row["target_player_if_none"] = 1 if get_bool(tb, "target_player_if_no_target", False) else 0
        for field, node in PET_AI_CLASSES[opcode_name].items():
            radius, x, y = self.zone_circle(table, node)
            row["zones"][field] = (radius, x, y)
        # The pet's own `effects` array (DocMoth: applied to every player inside
        # its zone on body_entered and unapplied on body_exited).
        row["fx_first"] = len(self.pet_fx)
        refs = _ANY_REF_RE.findall(root.get("effects", "") or "")
        scene_file = self.R.parse(res_path)
        for eid in refs:
            path = scene_file.ext_path(eid)
            props = self.R.parse(path).root_props
            key = get_str(props, "key", "")
            if key not in PET_ZONE_FX_KEYS:
                raise ExtractError("%s: zone effect %s has unhandled key %r"
                                   % (res_path, path, key))
            self.pet_fx.append((PET_ZONE_FX_KEYS[key], get_float(props, "value", 0.0),
                                get_float(props, "value2", 0.0)))
        row["fx_count"] = len(self.pet_fx) - row["fx_first"]
        return row

    def build_pet_ai(self):
        """BT_PET_AI rows for every pet class an item can instantiate.

        Rows are ordered by first appearance in the effect table, so the same
        sources always produce the same header.  Each pet scene is resolved
        once; duplicate scenes (two items granting the same pet) share a row.
        """
        seen = set()
        for eff in self.rows:
            opcode = eff["opcode"]
            if opcode not in PET_AI_CLASSES:
                continue
            scene = self.strings.values[eff["scene"]] if eff["scene"] else ""
            if not scene or scene in seen:
                continue
            seen.add(scene)
            self.pets.append(self.pet_ai(scene, opcode))

    # -- custom args -------------------------------------------------------
    def custom_args(self, pf, props):
        first = len(self.customargs)
        for sub_id in _SUB_ID_RE.findall(props.get("custom_args", "") or ""):
            block = pf.subs.get(int(sub_id))
            if block is None:
                raise ExtractError("%s: missing custom_args SubResource(%s)" % (pf.path, sub_id))
            sub = block["props"]
            self.customargs.append((
                get_int(sub, "arg_index", -1),
                get_int(sub, "arg_sign", 2),
                get_int(sub, "arg_format", 0),
                get_int(sub, "arg_value", 0),
                self.strings.intern(get_str(sub, "arg_key", "")),
            ))
        return first, len(self.customargs) - first

    # -- one effect row ----------------------------------------------------
    def effect(self, res_path, item_index):
        pf = self.R.parse(res_path)
        props = pf.root_props
        script_path, cls = self.script_class(pf, props)
        chain = ancestry(cls, self.class_parent)
        slot = len(self.rows)
        self.rows.append(None)
        row = {
            "opcode": cls,
            "opcode_supported": ("/effects/" in script_path
                                 or os.path.basename(script_path) == "effect.gd"),
            "source": res_path,
            "item": item_index,
            "sign": get_int(props, "effect_sign", 3),
            "storage": get_int(props, "storage_method", 0),
            "strategy": get_int(props, "sum_strategy", 0),
            "flags": 0,
            "chance_pct": 0,
            "reset_on_hit": 0,
            "perm_stats_only": 0,
            "scale_missing_hp": 0,
            "can_be_grouped": 0,
            "is_pet": 0,
            "is_structure": 0,
            "is_burning": 0,
            "is_spawning": 0,
            "auto_target": 0,
            "key": self.intern_value(props, "key"),
            "custom_key": self.intern_value(props, "custom_key"),
            "text_key": self.intern_value(props, "text_key"),
            "aux_key": 0,
            "aux2_key": 0,
            "tracking_key": 0,
            "color": 0,
            "scene": 0,
            "explosion_scene": 0,
            "extra_group": BT_ITEM_NONE,
            "extra_group_path": "",
            "sub_first": 0,
            "sub_count": 0,
            "stats": BT_ITEM_NONE,
            "stats2": BT_ITEM_NONE,
            "burning": BT_ITEM_NONE,
            "child": BT_ITEM_NONE,
            "ca_first": 0,
            "ca_count": 0,
            "value": get_int(props, "value", 0),
            "value2": get_int(props, "value2", 0),
            "value3": get_int(props, "value3", 0),
            "nb_stat_scaled": get_int(props, "nb_stat_scaled", 0),
            "spawn_cooldown": get_int(props, "spawn_cooldown", -1),
            "spawn_in_center": get_int(props, "spawn_in_center", -1),
            "spawn_around_player": get_int(props, "spawn_around_player", -1),
            "projectile_cooldown": get_int(props, "cooldown", -1),
            "duration_secs": get_int(props, "duration_secs", 0),
            "max_stacks": get_int(props, "max_stacks", 0),
            "max_procs": get_int(props, "max_procs", -1),
            "hp_threshold": get_int(props, "hp_threshold", 0),
            "sound_db_mod": get_int(props, "sound_db_mod", 0),
            "interval": get_float(props, "interval", 0.0),
            "boost_zone_scale": get_float(props, "boost_zone_scale", 0.0),
            "double_chance": get_float(props, "double_chance", 0.0),
            "explode_scale": get_float(props, "scale", 0.0),
            "base_smoke_amount": get_float(props, "base_smoke_amount", 0.0),
            "shooting_animation_speed": get_float(props, "shooting_animation_speed", 0.0),
            "health_boost": get_float(props, "health_boost", 0.0),
            "revive_duration": get_float(props, "revive_duration", 0.0),
        }
        flags = 0
        if "PetEffect" in chain:
            flags |= IEFLAG_PET
        if "StructureEffect" in chain:
            flags |= IEFLAG_STRUCTURE
        if "TurretEffect" in chain:
            flags |= IEFLAG_TURRET
        if "BuilderTurretEffect" in chain:
            flags |= IEFLAG_TURRET | IEFLAG_BUILDER_TURRET
        if "ExplodingEffect" in chain:
            flags |= IEFLAG_EXPLODE
        if "DoubleValueEffect" in chain:
            flags |= IEFLAG_DOUBLE_VALUE
        if "ChanceStatDamageEffect" in chain:
            flags |= IEFLAG_CHANCE_STAT_DAMAGE
        if "GainStatForKilledEnemiesWhileBurning" in chain:
            flags |= IEFLAG_GAIN_WHILE_BURNING
        if cls == "StatCapEffect":
            flags |= IEFLAG_STAT_CAP
        if cls == "TempStatsPerIntervalEffect":
            flags |= IEFLAG_TEMP_STATS
        if cls == "GainStatForEveryStatEffect":
            flags |= IEFLAG_STAT_LINK
        if cls == "PercentDamageEffect":
            flags |= IEFLAG_PERCENT_DAMAGE
        if cls == "ExtraEnemyEffect":
            flags |= IEFLAG_EXTRA_ENEMY
        if cls == "ProjectileEffect":
            flags |= IEFLAG_PROJECTILE
        if cls == "DoubleKeyValueEffect":
            flags |= IEFLAG_DOUBLE_KEY_VALUE
        if cls == "WeaponCooldownEffect":
            flags |= IEFLAG_WEAPON_COOLDOWN
        if cls == "EffectWithSubEffects":
            flags |= IEFLAG_SUB_EFFECTS
        if cls == "SwapMaxMinStatEffect":
            flags |= IEFLAG_SWAP_STATS
        if cls == "BurnChanceEffect" or "burning_data" in props:
            flags |= IEFLAG_BURN
        if "explosion_scene" in props:
            flags |= IEFLAG_EXPLODE
        if "hp_threshold" in props:
            flags |= IEFLAG_EXPLODE_BELOW_HP
        if not row["opcode_supported"]:
            flags |= IEFLAG_UNSUPPORTED
            self.unsupported.append({"class": cls, "source": res_path})
        row["flags"] = flags

        row["key"] = row["key"] or self.intern_value(props, "key")
        for src, dst in (("key2", "aux_key"), ("stat_scaled", "aux_key"),
                         ("set_cap_to_current_stat", "aux_key"), ("source_id", "aux_key"),
                         ("set_id", "aux_key")):
            if src in props:
                row[dst] = self.strings.intern(get_str(props, src, ""))
        for src, dst in (("stat_name", "aux2_key"), ("stat_displayed_name", "aux2_key")):
            if src in props:
                row[dst] = self.strings.intern(get_str(props, src, ""))
        # Weapon-only effect classes name their stat links with different
        # properties; capture them into the same generic slots.  Gated by class
        # so the item rows are byte-identical to the item-only generator.
        if cls == "WeaponStackEffect" and "weapon_stacked_id" in props:
            row["aux_key"] = self.strings.intern(get_str(props, "weapon_stacked_id", ""))
        elif cls == "WeaponSlowOnHitEffect" and "scaling_stat" in props:
            row["aux_key"] = self.strings.intern(get_str(props, "scaling_stat", ""))
        elif cls == "GainStatEveryKilledEnemiesEffect":
            if "stat" in props:
                row["aux_key"] = self.strings.intern(get_str(props, "stat", ""))
            if "stat_nb" in props:
                row["nb_stat_scaled"] = get_int(props, "stat_nb", 0)
        if cls == "WeaponGainStatForEveryStatEffect" and "increased_stat_name" in props:
            row["aux2_key"] = self.strings.intern(get_str(props, "increased_stat_name", ""))
        for src, dst in (("tracking_key", "tracking_key"), ("tracking_text", "tracking_key")):
            if src in props:
                row[dst] = self.strings.intern(get_str(props, src, ""))
        if "outline_color" in props:
            match = re.search(r'Color\(\s*"([0-9A-Fa-f]+)"\s*\)', props["outline_color"])
            row["color"] = self.strings.intern(match.group(1) if match else props["outline_color"])
        if "reset_on_hit" in props:
            row["reset_on_hit"] = 1 if get_bool(props, "reset_on_hit", False) else 0
        if "perm_stats_only" in props:
            row["perm_stats_only"] = 1 if get_bool(props, "perm_stats_only", True) else 0
        if "scale_with_missing_health" in props:
            row["scale_missing_hp"] = 1 if get_bool(props, "scale_with_missing_health", False) else 0
        if "can_be_grouped" in props:
            row["can_be_grouped"] = 1 if get_bool(props, "can_be_grouped", True) else 0
        if "is_pet" in props:
            row["is_pet"] = 1 if get_bool(props, "is_pet", False) else 0
        if "is_structure" in props:
            row["is_structure"] = 1 if get_bool(props, "is_structure", False) else 0
        if "is_burning" in props:
            row["is_burning"] = 1 if get_bool(props, "is_burning", False) else 0
        if "is_spawning" in props:
            row["is_spawning"] = 1 if get_bool(props, "is_spawning", False) else 0
        if "auto_target_enemy" in props:
            row["auto_target"] = 1 if get_bool(props, "auto_target_enemy", False) else 0
        if "chance" in props:
            raw = props["chance"].strip()
            if "." in raw or "e" in raw or "E" in raw:
                row["explode_scale"] = row["explode_scale"]
                row["explode_chance"] = get_float(props, "chance", 0.0)
            else:
                row["chance_pct"] = get_int(props, "chance", 0)
        row["explode_chance"] = get_float(props, "chance", 0.0) if "." in props.get("chance", "") else 0.0

        row["ca_first"], row["ca_count"] = self.custom_args(pf, props)

        for key in ("scene", "explosion_scene"):
            refs = _ANY_REF_RE.findall(props.get(key, "") or "")
            if refs:
                target = pf.ext_path(refs[0])
                if key == "scene":
                    row["scene"] = self.strings.intern(target)
                    self.scene(target)
                else:
                    row["explosion_scene"] = self.strings.intern(target)
                    self.scene(target)
        refs = _ANY_REF_RE.findall(props.get("extra_group_data", "") or "")
        if refs:
            target = pf.ext_path(refs[0])
            if target not in self.extra_groups:
                raise ExtractError(
                    "%s: extra_group_data %s has no BT_ITEM_EXTRA_GROUPS row"
                    % (res_path, target))
            row["extra_group"] = self.extra_groups[target]
            row["extra_group_path"] = target

        for key in ("stats", "weapon_stats"):
            refs = _ANY_REF_RE.findall(props.get(key, "") or "")
            if refs:
                row["stats"] = self.weapon_stats(pf.ext_path(refs[0]))
        refs = _ANY_REF_RE.findall(props.get("ranged_weapon_stats", "") or "")
        if refs:
            row["stats2"] = self.weapon_stats(pf.ext_path(refs[0]))
        refs = _ANY_REF_RE.findall(props.get("burning_data", "") or "")
        if refs:
            row["burning"] = self.burning_data(pf.ext_path(refs[0]))
        for key in ("explosion_effect", "landmine_effect_stat"):
            refs = _ANY_REF_RE.findall(props.get(key, "") or "")
            if refs:
                row["child"] = self.effect(pf.ext_path(refs[0]), item_index)

        children = []
        for key in ("effects", "sub_effects"):
            for eid in _ANY_REF_RE.findall(props.get(key, "") or ""):
                children.append(self.effect(pf.ext_path(eid), item_index))
        if children:
            row["flags"] |= IEFLAG_SUB_EFFECTS
            row["sub_first"] = len(self.eff_index)
            row["sub_count"] = len(children)
            self.eff_index.extend(children)

        self.rows[slot] = row
        return slot

    # -- one item ----------------------------------------------------------
    def item(self, res_path):
        pf = self.R.parse(res_path)
        props = pf.root_props
        script = props.get("script")
        if not script or not physics._EXTREF_RE.match(script) \
                or not pf.ext_path(physics._EXTREF_RE.match(script).group(1)).endswith("item_data.gd"):
            raise ExtractError("%s: not an item_data resource" % res_path)
        my_id = get_str(props, "my_id", "")
        if not my_id.startswith("item_"):
            raise ExtractError("%s: my_id %r is not an item id" % (res_path, my_id))
        index = len(self.items)
        children = [self.effect(pf.ext_path(eid), index)
                    for eid in _ANY_REF_RE.findall(props.get("effects", "") or "")]
        first = len(self.eff_index)
        self.eff_index.extend(children)
        tags = [self.strings.intern(t) for t in _STR_RE.findall(props.get("tags", "") or "")]
        # replaced_by names a res:// ItemData resource; store the target's my_id
        # so finish_items can resolve it through id_to_index, which is keyed by
        # my_id (not by path). Storing the raw path resolved every row to
        # BT_ITEM_NONE.
        replaced = BT_ITEM_NONE
        for eid in _ANY_REF_RE.findall(props.get("replaced_by", "") or ""):
            target = self.R.parse(pf.ext_path(eid)).root_props
            replaced = get_str(target, "my_id", BT_ITEM_NONE)
        row = {
            "index": index,
            "id": my_id,
            "name": my_id[len("item_"):],
            "source": res_path,
            "price": get_int(props, "value", 1),
            "tier": get_int(props, "tier", 0),
            "eff_first": first,
            "eff_count": len(children),
            "flags": 0,
            "proc_c": 0,
            "proc_d": 0.0,
            "cap_stat": BT_STAT_NONE,
            "cap_val": 0.0,
            "tag_first": 0,
            "tag_count": 0,
            "group_mask": 0,
            "replaced_by": replaced,
            "scene_geom": BT_ITEM_NONE,
            "max_nb": get_int(props, "max_nb", -1),
            "unlocked_by_default": 1 if get_bool(props, "unlocked_by_default", False) else 0,
            "can_be_looted": 1 if get_bool(props, "can_be_looted", True) else 0,
            "is_lockable": 1 if get_bool(props, "is_lockable", True) else 0,
            "is_cursed": 1 if get_bool(props, "is_cursed", False) else 0,
            "unlock_codex": get_int(props, "unlock_codex_descr_after_get_it", 1),
            "curse_factor": get_float(props, "curse_factor", 0.0),
            "tags": tags,
        }
        self.items.append(row)
        return index

    def finish_items(self, group_membership, id_to_index):
        for row in self.items:
            first = row["eff_first"]
            children = self.eff_index[first:first + row["eff_count"]]
            flags = 0
            chance = 0
            dmg = 0.0
            cap_stat = BT_STAT_NONE
            cap_val = 0.0
            for slot in _recursive_rows(children, self.rows, self.eff_index):
                eff = self.rows[slot]
                flags |= eff["flags"]
                if eff["opcode"] == "ChanceStatDamageEffect":
                    chance = max(chance, eff["chance_pct"])
                    dmg += eff["value"]
                if eff["opcode"] == "StatCapEffect":
                    if cap_stat == BT_STAT_NONE:
                        dynamic = self.strings.values[eff["aux_key"]]
                        key = self.strings.values[eff["key"]]
                        if dynamic in STAT_ID:
                            cap_stat = STAT_ID[dynamic]
                        elif key in CAP_KEY_TO_STAT:
                            cap_stat = CAP_KEY_TO_STAT[key]
                        cap_val = float(eff["value"])
                if eff["scene"] and row["scene_geom"] == BT_ITEM_NONE:
                    row["scene_geom"] = self.scenes.get(self.strings.values[eff["scene"]], BT_ITEM_NONE)
            row["flags"] = flags
            row["proc_c"] = chance
            row["proc_d"] = dmg
            row["cap_stat"] = cap_stat
            row["cap_val"] = cap_val
            if flags & IEFLAG_EXPLODE:
                row["special"] = 1
            elif flags & (IEFLAG_PET | IEFLAG_STRUCTURE | IEFLAG_TURRET | IEFLAG_PROJECTILE):
                row["special"] = 2
            else:
                row["special"] = 0
            row["group_mask"] = group_membership.get(row["id"], 0)
            if row["replaced_by"] != BT_ITEM_NONE:
                row["replaced_index"] = id_to_index.get(row["replaced_by"], BT_ITEM_NONE)
            else:
                row["replaced_index"] = BT_ITEM_NONE


def _recursive_rows(children, rows, eff_index):
    for slot in children:
        yield slot
        eff = rows[slot]
        for sub in eff_index[eff["sub_first"]:eff["sub_first"] + eff["sub_count"]]:
            yield sub
        if eff["child"] != BT_ITEM_NONE:
            yield eff["child"]


# --------------------------------------------------------------------------
# Item groups / bans
# --------------------------------------------------------------------------
def read_item_groups(campaign_root):
    text = open(os.path.join(campaign_root, "singletons/item_service.gd"),
                "r", encoding="utf-8").read()
    start = text.index("var item_groups: Dictionary = {")
    end = text.index("\n}", start)
    body = text[start:end]
    groups = {}
    for match in re.finditer(r'"([a-z_]+)":\s*\[([^\]]*)\]', body):
        groups[match.group(1)] = [item for item in _STR_RE.findall(match.group(2))]
    start = text.index("var banned_items_for_endless")
    end = text.index("\n", start)
    endless = _STR_RE.findall(text[start:end])
    return groups, endless


def read_bull_bans(campaign_root):
    path = os.path.join(campaign_root, "items/characters/bull/bull_data.tres")
    pf = physics.parse_file(path)
    props = pf.root_props
    return (_STR_RE.findall(props.get("wanted_tags", "") or ""),
            _STR_RE.findall(props.get("banned_item_groups", "") or ""),
            _STR_RE.findall(props.get("banned_items", "") or ""))


def read_curse(dlc_root):
    """DLC 1 curse tuning: the script's export defaults overridden by the SHIPPED
    ``dlcs/dlc_1/dlc_data.tres`` instance (a stored property wins over the script
    default it shadows).  The three chance exports ARE overridden in the shipped
    resource -- base_cursed_chest_chance and base_item_curse_chance are 0.0 (the
    .gd defaults 0.03 would make dlc_1_data.update_item_effects roll a curse draw
    on every generated offer even with 0 curse, shifting the whole shop roll) and
    max_curse_item_chance is 0.15.  Names absent from the instance keep the
    script default."""
    path = os.path.join(dlc_root, "dlcs/dlc_1/dlc_1_data.gd")
    text = open(path, "r", encoding="utf-8").read()
    out = {}
    for match in re.finditer(r"^export[^\n]*var[ \t]+([a-z_0-9]+)[ \t]*:?[^\n=]*=[ \t]*(-?[ \t]*\d+(?:\.\d+)?)", text, re.M):
        out[match.group(1)] = float(match.group(2).replace(' ',''))
    instance = os.path.join(dlc_root, "dlcs/dlc_1/dlc_data.tres")
    if os.path.isfile(instance):
        props = physics.parse_file(instance).root_props
        for name in out:
            raw = props.get(name)
            if raw is None:
                continue
            value = re.match(r"[ \t]*(-?[ \t]*\d+(?:\.\d+)?)", raw)
            if value:
                out[name] = float(value.group(1).replace(' ', ''))
    return out


# --------------------------------------------------------------------------
# Field schema (single source of truth for struct emission + contract doc)
# --------------------------------------------------------------------------
ITEEM_FIELDS = [
    ("opcode", "unsigned char", "BT_ITEMEFF_* opcode of the source effect class"),
    ("sign", "unsigned char", "Effect.Sign (0 POSITIVE 1 NEGATIVE 2 NEUTRAL 3 FROM_VALUE 4 FROM_ARG 5 OVERRIDE)"),
    ("storage", "unsigned char", "Effect.StorageMethod (0 SUM 1 KEY_VALUE 2 REPLACE 3 APPEND_KEY 4 APPEND_KEY_VALUE)"),
    ("strategy", "unsigned char", "DoubleValueEffect.SumStrategy (0 SUM_BOTH 1 SUM_VALUE_1 2 SUM_VALUE_2)"),
    ("flags", "unsigned int", "BT_ITFLAG_* bitmask (see flag table)"),
    ("chance_pct", "unsigned char", "ChanceStatDamageEffect.chance, percent"),
    ("reset_on_hit", "unsigned char", "TempStatsPerIntervalEffect.reset_on_hit"),
    ("perm_stats_only", "unsigned char", "GainStatForEveryStatEffect.perm_stats_only"),
    ("scale_missing_hp", "unsigned char", "ItemExplodingEffect.scale_with_missing_health"),
    ("can_be_grouped", "unsigned char", "StructureEffect.can_be_grouped"),
    ("is_pet", "unsigned char", "StructureEffect.is_pet"),
    ("is_structure", "unsigned char", "PetEffect.is_structure"),
    ("is_burning", "unsigned char", "TurretEffect.is_burning"),
    ("is_spawning", "unsigned char", "TurretEffect.is_spawning"),
    ("auto_target", "unsigned char", "ProjectileEffect.auto_target_enemy"),
    ("key", "unsigned short", "BT_ISTR_* interned target/source stat key (Effect.key)"),
    ("custom_key", "unsigned short", "BT_ISTR_* interned Effect.custom_key (effect-storage bucket)"),
    ("text_key", "unsigned short", "BT_ISTR_* interned Effect.text_key"),
    ("aux_key", "unsigned short", "BT_ISTR_* key2/stat_scaled/set_cap_to_current_stat/source_id/set_id"),
    ("aux2_key", "unsigned short", "BT_ISTR_* stat_name/stat_displayed_name (ClassBonusEffect)"),
    ("tracking_key", "unsigned short", "BT_ISTR_* tracking_key or tracking_text"),
    ("color", "unsigned short", "BT_ISTR_* outline_color hex (PercentDamageEffect)"),
    ("scene", "unsigned short", "BT_ISTR_* PackedScene path (pet/structure scene)"),
    ("explosion_scene", "unsigned short", "BT_ISTR_* PackedScene path (ExplodingEffect)"),
    ("extra_group", "unsigned short", "BT_ITEM_EXTRA_GROUPS ordinal, per-zone BT_GROUPS row (ExtraEnemyEffect; BT_ITEM_EFF_NONE if none)"),
    ("sub_first", "unsigned short", "first index into BT_ITEM_EFF_INDEX for effects[]/sub_effects[]"),
    ("sub_count", "unsigned short", "number of child effects (BT_ITEM_EFF_NONE if none)"),
    ("stats", "unsigned short", "BT_ITEM_WSTATS row index (or BT_ITEM_EFF_NONE)"),
    ("stats2", "unsigned short", "secondary BT_ITEM_WSTATS row (Blazemander ranged / landmine)"),
    ("burning", "unsigned short", "BT_ITEM_BURNING row index (or BT_ITEM_EFF_NONE)"),
    ("child", "unsigned short", "single nested effect row (explosion_effect / landmine_effect_stat)"),
    ("item", "unsigned short", "BT_ITEMS index that owns this effect row"),
    ("ca_first", "unsigned short", "first index into BT_ITEM_CUSTOMARGS"),
    ("ca_count", "unsigned short", "number of custom args"),
    ("value", "int", "Effect.value"),
    ("value2", "int", "DoubleValueEffect/DoubleKeyValueEffect.value2"),
    ("value3", "int", "GainStatForKilledEnemiesWhileBurning.value3"),
    ("nb_stat_scaled", "int", "GainStatForEveryStatEffect.nb_stat_scaled"),
    ("spawn_cooldown", "int", "StructureEffect.spawn_cooldown (ticks; -1 = stats.cooldown)"),
    ("spawn_in_center", "int", "StructureEffect.spawn_in_center (-1 = scene default)"),
    ("spawn_around_player", "int", "StructureEffect.spawn_around_player (-1 = scene default)"),
    ("projectile_cooldown", "int", "ProjectileEffect.cooldown (ticks; -1 = stats.cooldown)"),
    ("duration_secs", "int", "PercentDamageEffect.duration_secs"),
    ("max_stacks", "int", "PercentDamageEffect.max_stacks"),
    ("max_procs", "int", "PercentDamageEffect.max_procs"),
    ("hp_threshold", "int", "ItemExplodingWhenBelowHPEffect.hp_threshold"),
    ("sound_db_mod", "int", "ExplodingEffect.sound_db_mod"),
    ("interval", "float", "TempStatsPerIntervalEffect.interval (seconds)"),
    ("boost_zone_scale", "float", "DocMothEffect.boost_zone_scale"),
    ("double_chance", "float", "LootwormEffect.double_chance"),
    ("explode_chance", "float", "ExplodingEffect.chance (0..1)"),
    ("explode_scale", "float", "ExplodingEffect.scale"),
    ("base_smoke_amount", "float", "ExplodingEffect.base_smoke_amount"),
    ("shooting_animation_speed", "float", "TurretEffect.shooting_animation_speed"),
    ("health_boost", "float", "ScapegoatEffect.health_boost"),
    ("revive_duration", "float", "ScapegoatEffect.revive_duration"),
]

WSTATS_FIELDS = [
    ("kind", "unsigned char", "0 WeaponStats, 1 MeleeWeaponStats, 2 RangedWeaponStats"),
    ("is_healing", "unsigned char", "WeaponStats.is_healing"),
    ("can_bounce", "unsigned char", "RangedWeaponStats.can_bounce"),
    ("deal_dmg_on_return", "unsigned char", "MeleeWeaponStats.deal_dmg_on_return"),
    ("alternate_attack_type", "unsigned char", "MeleeWeaponStats.alternate_attack_type"),
    ("increase_proj_speed", "unsigned char", "RangedWeaponStats.increase_projectile_speed_with_range"),
    ("can_have_positive_knockback", "unsigned char", "WeaponStats.can_have_positive_knockback"),
    ("can_have_negative_knockback", "unsigned char", "WeaponStats.can_have_negative_knockback"),
    ("attack_type", "unsigned char", "MeleeWeaponStats.AttackType (0 THRUST 1 SWEEP)"),
    ("projectile_scene", "unsigned short", "BT_ISTR_* projectile PackedScene path or BT_ITEM_EFF_NONE"),
    ("scaling_first", "unsigned short", "first index into BT_ITEM_SCALING"),
    ("scaling_count", "unsigned short", "number of (stat, ratio) scaling rows"),
    ("cooldown", "int", "WeaponStats.cooldown (ticks)"),
    ("damage", "int", "WeaponStats.damage"),
    ("min_range", "int", "WeaponStats.min_range"),
    ("max_range", "int", "WeaponStats.max_range"),
    ("knockback", "int", "WeaponStats.knockback"),
    ("recoil", "int", "WeaponStats.recoil"),
    ("sound_db_mod", "int", "WeaponStats.sound_db_mod"),
    ("additional_cd_every", "int", "WeaponStats.additional_cooldown_every_x_shots"),
    ("speed_percent_modifier", "int", "WeaponStats.speed_percent_modifier"),
    ("nb_projectiles", "int", "RangedWeaponStats.nb_projectiles"),
    ("piercing", "int", "RangedWeaponStats.piercing"),
    ("bounce", "int", "RangedWeaponStats.bounce"),
    ("projectile_speed", "int", "RangedWeaponStats.projectile_speed"),
    ("accuracy", "float", "WeaponStats.accuracy"),
    ("crit_chance", "float", "WeaponStats.crit_chance"),
    ("crit_damage", "float", "WeaponStats.crit_damage"),
    ("knockback_piercing", "float", "WeaponStats.knockback_piercing"),
    ("effect_scale", "float", "WeaponStats.effect_scale"),
    ("lifesteal", "float", "WeaponStats.lifesteal"),
    ("recoil_duration", "float", "WeaponStats.recoil_duration"),
    ("additional_cd_multiplier", "float", "WeaponStats.additional_cooldown_multiplier"),
    ("projectile_spread", "float", "RangedWeaponStats.projectile_spread"),
    ("piercing_dmg_reduction", "float", "RangedWeaponStats.piercing_dmg_reduction"),
    ("bounce_dmg_reduction", "float", "RangedWeaponStats.bounce_dmg_reduction"),
]

ITEM_FIELDS = [
    ("name", "const char*", "host-only item id without the item_ prefix (icon lookup)"),
    ("price", "unsigned short", "ItemParentData.value"),
    ("tier", "unsigned char", "ItemParentData.tier (0 COMMON .. 5 DANGER_5)"),
    ("first", "unsigned short", "first index into BT_ITEM_EFFECTS"),
    ("count", "unsigned short", "number of legacy stat effects"),
    ("special", "unsigned char", "legacy: 1 explode-on-hit, 2 pet/structure/turret/projectile"),
    ("proc_c", "unsigned char", "max ChanceStatDamageEffect.chance (percent)"),
    ("proc_d", "float", "sum of ChanceStatDamageEffect.value"),
    ("cap_stat", "unsigned char", "StatCapEffect key mapped to a core stat id (255 = none)"),
    ("cap_val", "float", "StatCapEffect.value"),
    ("eff_first", "unsigned short", "first index into BT_ITEM_EFF_INDEX"),
    ("eff_count", "unsigned short", "number of top-level effect rows"),
    ("tag_first", "unsigned short", "first index into BT_ITEM_TAGS"),
    ("tag_count", "unsigned short", "number of tags"),
    ("group_mask", "unsigned short", "BT_ITEM_GROUP_* bitmask"),
    ("item_flags", "unsigned int", "BT_ITFLAG_* aggregate over the whole effect graph"),
    ("replaced_by", "unsigned short", "BT_ITEMS index this item is replaced by (or BT_ITEM_NONE)"),
    ("scene_geom", "unsigned short", "BT_ITEM_SCENES row for the spawned scene (or BT_ITEM_NONE)"),
    ("max_nb", "short", "ItemData.max_nb (-1 unlimited, 0 never in shop)"),
    ("unlocked_by_default", "unsigned char", "ItemParentData.unlocked_by_default"),
    ("can_be_looted", "unsigned char", "ItemParentData.can_be_looted"),
    ("is_lockable", "unsigned char", "ItemParentData.is_lockable"),
    ("is_cursed", "unsigned char", "ItemParentData.is_cursed"),
    ("unlock_codex", "unsigned char", "ItemParentData.unlock_codex_descr_after_get_it"),
    ("curse_factor", "float", "ItemParentData.curse_factor"),
]


# --------------------------------------------------------------------------
# Build
# --------------------------------------------------------------------------
def build(args):
    resolver = physics.Resolver([args.campaign_root, args.dlc_root])
    class_by_path, class_parent = build_class_index(resolver.roots)
    campaign, dlc = read_pool(resolver, args.campaign_root, args.dlc_root)
    dir_stats = {
        "campaign": validate_dirs(args.campaign_root, "items/all", campaign, "campaign items"),
        "dlc": validate_dirs(args.dlc_root, "dlcs/dlc_1/items", dlc, "dlc items"),
    }
    builder = Builder(resolver, class_by_path, class_parent,
                      gameplay.item_extra_group_ordinals(resolver))
    for path in campaign + dlc:
        builder.item(path)
    id_to_index = {row["id"]: row["index"] for row in builder.items}
    groups, endless = read_item_groups(args.campaign_root)
    group_names = list(groups.keys())
    group_membership = {}
    for bit, name in enumerate(group_names):
        for item_id in groups[name]:
            group_membership[item_id] = group_membership.get(item_id, 0) | (1 << bit)
    builder.finish_items(group_membership, id_to_index)
    builder.build_pet_ai()
    # Keys.structure_percent_damage_hash / Keys.structure_range_hash are player
    # stat keys with no effect resource of their own (their only producers are
    # the Engineer's builder_turret stat_added calls), so they are interned
    # explicitly: the core reads them from scalar_effect[BT_ISTR_*].  Interned
    # last, so no effect-derived string id moves.
    builder.strings.intern("structure_percent_damage")
    builder.strings.intern("structure_range")
    # Unit.init reads both burn timer modifiers even when the item pool has no
    # producer for the positive-increase key (unit.gd:67-71).
    builder.strings.intern("burning_cooldown_increase")
    wanted, banned_groups, banned_items = read_bull_bans(args.campaign_root)
    wanted_ids = [builder.strings.intern(t) for t in wanted]
    tag_first = {}
    tag_list = []
    for row in builder.items:
        tag_first[row["index"]] = len(tag_list)
        tag_list.extend(row["tags"])
    legacy = []
    first = 0
    for row in builder.items:
        slots = builder.eff_index[row["eff_first"]:row["eff_first"] + row["eff_count"]]
        count = 0
        for slot in slots:
            eff = builder.rows[slot]
            key = builder.strings.values[eff["key"]]
            if key in STAT_ID:
                legacy.append((STAT_ID[key], float(eff["value"])))
                count += 1
        row["first"] = first
        row["count"] = count
        first += count
    return {
        "builder": builder,
        "items": builder.items,
        "legacy": legacy,
        "groups": groups,
        "group_names": group_names,
        "endless": endless,
        "bull": {"wanted": wanted, "wanted_ids": wanted_ids,
                 "banned_groups": banned_groups, "banned_items": banned_items},
        "tag_first": tag_first,
        "tags": tag_list,
        "curse": read_curse(args.dlc_root),
        "id_to_index": id_to_index,
        "dir_stats": dir_stats,
        "campaign": campaign,
        "dlc": dlc,
    }


# --------------------------------------------------------------------------
# Emit brotato_items.h
# --------------------------------------------------------------------------
def emit_items(data, out_path, campaign_root, dlc_root):
    builder = data["builder"]
    items = data["items"]
    lines = []
    lines.append("/* Generated by ocean/brotato/extract_items.py -- DO NOT EDIT.")
    lines.append(" *")
    lines.append(" * Complete Brotato item pool (campaign + Abyss DLC), resolved from the")
    lines.append(" * recovered Godot 3.x sources.  Regenerate with:")
    lines.append(" *")
    lines.append(" *   python3 ocean/brotato/extract_items.py \\")
    lines.append(" *       --campaign-root %s \\" % campaign_root)
    lines.append(" *       --dlc-root %s \\" % dlc_root)
    lines.append(" *       --items-out %s \\" % out_path)
    lines.append(" *       --effects-out ocean/brotato/brotato_item_effects.h \\")
    lines.append(" *       --provenance-out <external>/brotato_items.json")
    lines.append(" *")
    lines.append(" * The pool is the `items` array of singletons/item_service.tscn plus the")
    lines.append(" * `items` array of dlcs/dlc_1/dlc_data.tres; the directory trees are")
    lines.append(" * cross-checked against it (%d campaign items in %d dirs, %d DLC items in %d"
                 % (data["dir_stats"]["campaign"]["items"], data["dir_stats"]["campaign"]["dirs"],
                    data["dir_stats"]["dlc"]["items"], data["dir_stats"]["dlc"]["dirs"]))
    lines.append(" * dirs).  Nothing is dropped and no value is invented.")
    lines.append(" *")
    lines.append(" * BtItem keeps the legacy field set the current core reads")
    lines.append(" * (name/price/tier/first/count/special/proc_c/proc_d/cap_stat/cap_val),")
    lines.append(" * widened so the pool no longer overflows an 8-bit first/count.  The")
    lines.append(" * structured effect graph that supersedes the derived fields lives in")
    lines.append(" * brotato_item_effects.h (included by consumers that need it, not here,")
    lines.append(" * so the existing core build is unchanged).")
    lines.append(" *")
    lines.append(" * Legacy-pool stat ids are the canonical core ids 0..12 (see the contract")
    lines.append(" * document); id 12 is harvesting, which the core folds through its")
    lines.append(" * `default:` switch branch.  The previous 147-item table only ever used")
    lines.append(" * 0..10, so the core's observation now sees `dom` up to 12.")
    lines.append(" */")
    lines.append("#pragma once")
    lines.append('#include "brotato_rng.h"   /* BT_TABLE / BT_HOST_ONLY */')
    lines.append("")
    lines.append("#define BT_ITEM_NONE 0xFFFFu")
    lines.append("#define BT_STAT_NONE 0xFFu")
    lines.append("")
    lines.append("/* Effect/item flag bits (also the layout of BtItemEff.flags in")
    lines.append(" * brotato_item_effects.h). */")
    for _bit, macro, doc in FLAG_DOCS:
        lines.append("#define %s 0x%08xu   /* %s */" % (macro, _bit, doc))
    lines.append("")
    lines.append("typedef struct { unsigned char stat; float value; } BtItemEffect;")
    lines.append("")
    lines.append("typedef struct {")
    for name, ctype, doc in ITEM_FIELDS:
        if ctype == "const char*":
            lines.append("    BT_HOST_ONLY(const char* name;)   /* %s */" % doc)
        else:
            lines.append("    %-16s %s;%s/* %s */" % (ctype, name, " " * max(1, 18 - len(name)), doc))
    lines.append("} BtItem;")
    lines.append("")
    lines.append("#define BT_NITEMS %d" % len(items))
    lines.append("#define BT_NITEM_EFFECTS %d" % len(data["legacy"]))
    symbols = set()
    for index, row in enumerate(items):
        symbol = "BT_ITEM_IDX_" + re.sub(r"[^A-Za-z0-9_]", "_", row["name"]).upper()
        if symbol in symbols:
            raise ExtractError("item identity macro collision: %s" % symbol)
        symbols.add(symbol)
        lines.append("#define %s %d" % (symbol, index))
    lines.append("")
    lines.append("BT_TABLE BtItem BT_ITEMS[BT_NITEMS] = {")
    for row in items:
        lines.append("    { BT_HOST_ONLY(\"%s\",) %d, %d, %d, %d, %d, %d, %s, %d, %s,"
                     % (row["name"], row["price"], row["tier"], row["first"], row["count"],
                        row["special"], row["proc_c"], cfloat(row["proc_d"]),
                        row["cap_stat"], cfloat(row["cap_val"])))
        lines.append("      %d, %d, %d, %d, 0x%04xu, 0x%08xu, %d, %d, %d, %d, %d, %d, %d, %d, %s },  /* %s */"
                     % (row["eff_first"], row["eff_count"],
                        data["tag_first"].get(row["index"], 0),
                        len(row["tags"]),
                        row["group_mask"], row["flags"] & 0xFFFFFFFF,
                        row["replaced_index"], row["scene_geom"],
                        row["max_nb"], row["unlocked_by_default"], row["can_be_looted"],
                        row["is_lockable"], row["is_cursed"], row["unlock_codex"],
                        cfloat(row["curse_factor"]), row["source"]))
    lines.append("};")
    lines.append("")
    lines.append("/* Contiguous legacy stat pool: BT_ITEM_EFFECTS[item.first .. +item.count).")
    lines.append(" * Entries are the item's direct effects whose key maps to a core stat id.")
    lines.append(" * The remaining direct effects carry keys outside the core's switch and")
    lines.append(" * are preserved in full by brotato_item_effects.h. */")
    lines.append("BT_TABLE BtItemEffect BT_ITEM_EFFECTS[%d] = {" % max(1, len(data["legacy"])))
    for sid, value in data["legacy"]:
        lines.append("    { %d, %s }," % (sid, cfloat(value)))
    lines.append("};")
    lines.append("")
    _append_bans(lines, data)
    lines.append("")
    return _write("\n".join(lines), out_path)


def _append_bans(lines, data):
    index = data["id_to_index"]
    bull = data["bull"]
    lines.append("/* Shop bans.  ItemService.banned_items_for_endless (shop bans once the")
    lines.append(" * run passes wave nb_of_waves) and the Bull character's own banned items /")
    lines.append(" * banned item groups / wanted tags (items/characters/bull/bull_data.tres). */")
    for name, ids in (("BT_ENDLESS_BANNED_ITEMS", data["endless"]),
                      ("BT_BULL_BANNED_ITEMS", bull["banned_items"])):
        resolved = [index[i] for i in sorted(ids) if i in index]
        lines.append("#define %s_N %d" % (name, len(resolved)))
        lines.append("BT_TABLE unsigned short %s[%d] = { %s };"
                     % (name, max(1, len(resolved)),
                        ", ".join(str(i) for i in resolved) if resolved else "0"))
    group_bits = [data["group_names"].index(g) for g in bull["banned_groups"]
                  if g in data["group_names"]]
    mask = 0
    for bit in group_bits:
        mask |= 1 << bit
    lines.append("#define BT_BULL_BANNED_GROUP_MASK 0x%04xu" % mask)
    lines.append("#define BT_BULL_WANTED_TAGS_N %d" % len(bull["wanted"]))
    lines.append("/* Interned string ids (names in brotato_item_effects.h). */")
    lines.append("BT_TABLE unsigned short BT_BULL_WANTED_TAGS[%d] = { %s };"
                 % (max(1, len(bull["wanted"])),
                    ", ".join("%d /* %s */" % (data["builder"].strings.ids[t],
                                               data["builder"].strings.macros[t])
                              for t in bull["wanted"])
                    if bull["wanted"] else "0"))
    lines.append("#define BT_NITEM_GROUPS %d" % len(data["group_names"]))
    lines.append("BT_HOST_ONLY(static const char* const BT_ITEM_GROUP_NAME[BT_NITEM_GROUPS] = {")
    lines.append("    " + ", ".join('"%s"' % g for g in data["group_names"]) + " };)")
    for bit, name in enumerate(data["group_names"]):
        lines.append("#define BT_ITEM_GROUP_%s 0x%04xu"
                     % (sanitize(name), 1 << bit))


# --------------------------------------------------------------------------
# Emit brotato_item_effects.h
# --------------------------------------------------------------------------
def emit_effects(data, out_path, campaign_root, dlc_root, extra_opcodes=()):
    builder = data["builder"]
    strings = builder.strings
    lines = []
    lines.append("/* Generated by ocean/brotato/extract_items.py -- DO NOT EDIT.")
    lines.append(" *")
    lines.append(" * Structured Brotato item-effect graph: one row per effect resource")
    lines.append(" * occurrence, carrying the original class opcode, target/source stat")
    lines.append(" * keys, flat args and nested BurningData / WeaponStats / spawned-scene")
    lines.append(" * geometry.  Every value is resolved from the recovered sources.")
    lines.append(" *")
    lines.append(" * Regenerate with the sibling brotato_items.h:")
    lines.append(" *   python3 ocean/brotato/extract_items.py --campaign-root %s \\" % campaign_root)
    lines.append(" *       --dlc-root %s --items-out ocean/brotato/brotato_items.h \\" % dlc_root)
    lines.append(" *       --effects-out %s --provenance-out <external>/brotato_items.json" % out_path)
    lines.append(" *")
    lines.append(" * Device portability: every table is BT_TABLE (host static const, MSL")
    lines.append(" * `static constant`, CUDA device const).  Strings never reach the device;")
    lines.append(" * they are interned to integer ids and printed only from the BT_HOST_ONLY")
    lines.append(" * name table.")
    lines.append(" */")
    lines.append("#pragma once")
    lines.append('#include "brotato_items.h"   /* BtItem / BT_NITEMS / BT_ITFLAG_* */')
    lines.append("")
    lines.append("#define BT_ITEM_EFF_NONE 0xFFFFu")
    lines.append("")
    lines.append("/* ExtraEnemyEffect rows store the ORDINAL of their wave group in")
    lines.append(" * `BtItemEff.extra_group` (from the effect's `extra_group_data` resource);")
    lines.append(" * the ordinal indexes BT_ITEM_EXTRA_GROUPS in brotato_waves.h, which maps")
    lines.append(" * it to a per-zone BT_GROUPS row.  BT_ITEM_EFF_NONE grants no extra group.")
    lines.append(" * `BtItemEff.value` is the number of copies appended to the next wave")
    lines.append(" * (wave_manager.gd:114-121). */")
    lines.append("")
    lines.append("/* Interned string ids.  BT_ISTR_NONE (0) == \"\". */")
    lines.append("#define BT_ISTR_NONE 0")
    for text in strings.values[1:]:
        lines.append("#define %s %d" % (strings.macros[text], strings.ids[text]))
    lines.append("#define BT_NITEM_STRS %d" % len(strings.values))
    # Jerky schedules independent SceneTreeTimers within its source duration.
    # A frame-indexed ring stores every overlapping pickup without allocations.
    hot_key = strings.ids.get("consumable_heal_over_time", BT_ITEM_NONE)
    hot_frames = max([1] + [int(row["value"]) * 60 for row in builder.rows
                            if row["key"] == hot_key])
    lines.append("#define BT_HOT_MAX_FRAMES %d" % hot_frames)
    lines.append("BT_HOST_ONLY(static const char* const BT_ITEM_STR_NAMES[BT_NITEM_STRS] = {")
    for text in strings.values:
        lines.append('    "%s",' % text.replace("\\", "\\\\").replace('"', '\\"'))
    lines.append("};)")
    lines.append("")
    # Item opcodes stay in their original sorted order; weapon-only opcodes
    # (rows whose BtItemEff.item == BT_ITEM_NONE) are appended so every existing
    # BT_ITEMEFF_* number is preserved.  See extract_weapons.py.
    opcodes = sorted({r["opcode"] for r in builder.rows if r["item"] != BT_ITEM_NONE})
    for cls in sorted({r["opcode"] for r in builder.rows if r["item"] == BT_ITEM_NONE}
                      - set(opcodes)):
        opcodes.append(cls)
    for cls in sorted(set(extra_opcodes) - set(opcodes)):
        opcodes.append(cls)
    lines.append("/* Effect class opcodes (CamelCase source class -> BT_ITEMEFF_*).")
    lines.append(" * BT_ITEMEFF_UNSUPPORTED marks an effect script outside effects/ (its")
    lines.append(" * class name is still interned in the provenance document). */")
    lines.append("#define BT_ITEMEFF_UNSUPPORTED 0")
    for i, cls in enumerate(opcodes, start=1):
        lines.append("#define %s %d   /* %s */" % (opcode_macro(cls), i, cls))
    lines.append("#define BT_NITEMEFF_CLASSES %d" % (len(opcodes) + 1))
    lines.append("BT_HOST_ONLY(static const char* const BT_ITEMEFF_CLASS_NAME[BT_NITEMEFF_CLASSES] = {")
    lines.append('    "unsupported",')
    for cls in opcodes:
        lines.append('    "%s",' % cls)
    lines.append("};)")
    lines.append("")
    lines.append("typedef struct {")
    for name, ctype, doc in ITEEM_FIELDS:
        lines.append("    %-16s %s;%s/* %s */" % (ctype, name, " " * max(1, 18 - len(name)), doc))
    lines.append("} BtItemEff;")
    lines.append("")
    lines.append("typedef struct { unsigned short stat; float ratio; } BtItemScaling;")
    lines.append("typedef struct {")
    for name, ctype, doc in WSTATS_FIELDS:
        lines.append("    %-16s %s;%s/* %s */" % (ctype, name, " " * max(1, 18 - len(name)), doc))
    lines.append("} BtItemWeaponStats;")
    lines.append("")
    lines.append("typedef struct {")
    lines.append("    unsigned short scaling_first, scaling_count;  /* into BT_ITEM_SCALING */")
    lines.append("    unsigned char  is_global_burn, _pad0, _pad1, _pad2;")
    lines.append("    float chance;   /* BurningData.chance (0..1) */")
    lines.append("    int   damage;   /* BurningData.damage */")
    lines.append("    int   duration; /* BurningData.duration (ticks) */")
    lines.append("    int   spread;   /* BurningData.spread */")
    lines.append("} BtItemBurning;")
    lines.append("")
    lines.append("typedef struct {")
    lines.append("    unsigned short scene;      /* BT_ISTR_* PackedScene path */")
    lines.append("    unsigned short reason;     /* BT_ISTR_* failure reason, BT_ISTR_NONE if resolved */")
    lines.append("    unsigned char  kind;       /* 0 unresolved, 1 RigidBody2D, 2 other Node2D */")
    lines.append("    unsigned char  mode;       /* RigidBody2D.mode when kind==1 */")
    lines.append("    unsigned char  has_hitbox, has_hurtbox, _pad0, _pad1;")
    lines.append("    float body_radius, body_x, body_y;  /* root-local body CollisionShape2D circle */")
    lines.append("    float hurt_radius, hurt_x, hurt_y;  /* Hurtbox Area2D circle */")
    lines.append("    float hit_radius, hit_x, hit_y;     /* Hitbox Area2D circle */")
    lines.append("} BtItemSceneGeom;")
    lines.append("")
    lines.append("typedef struct { unsigned char arg_index, arg_sign, arg_format, _pad0;"
                 " int arg_value; unsigned short arg_key; } BtItemCustomArg;")
    lines.append("")
    lines.append("#define BT_NITEM_EFF_ROWS %d" % len(builder.rows))
    lines.append("#define BT_NITEM_EFF_INDEX %d" % len(builder.eff_index))
    lines.append("BT_TABLE unsigned short BT_ITEM_EFF_INDEX[%d] = {"
                 % max(1, len(builder.eff_index)))
    _emit_ushort_table(lines, builder.eff_index)
    lines.append("};")
    lines.append("")
    lines.append("BT_TABLE BtItemEff BT_ITEM_EFF_ROWS[%d] = {" % len(builder.rows))
    for i, row in enumerate(builder.rows):
        lines.append("    /* [%d] %s  <- %s */" % (i, row["opcode"], row["source"]))
        lines.append("    { %d, %d, %d, %d, 0x%08xu, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d,"
                     % (0 if not row["opcode_supported"] else opcodes.index(row["opcode"]) + 1,
                        row["sign"], row["storage"], row["strategy"], row["flags"] & 0xFFFFFFFF,
                        row["chance_pct"], row["reset_on_hit"], row["perm_stats_only"],
                        row["scale_missing_hp"], row["can_be_grouped"], row["is_pet"],
                        row["is_structure"], row["is_burning"], row["is_spawning"],
                        row["auto_target"]))
        lines.append("      %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d,"
                     % (row["key"], row["custom_key"], row["text_key"], row["aux_key"],
                        row["aux2_key"], row["tracking_key"], row["color"], row["scene"],
                        row["explosion_scene"], row["extra_group"], row["sub_first"],
                        row["sub_count"] if row["sub_count"] else BT_ITEM_NONE,
                        row["stats"], row["stats2"], row["burning"]))
        lines.append("      %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d,"
                     % (row["child"], row["item"], row["ca_first"], row["ca_count"],
                        row["value"], row["value2"], row["value3"], row["nb_stat_scaled"],
                        row["spawn_cooldown"], row["spawn_in_center"], row["spawn_around_player"],
                        row["projectile_cooldown"], row["duration_secs"], row["max_stacks"],
                        row["max_procs"], row["hp_threshold"]))
        lines.append("      %d, %s, %s, %s, %s, %s, %s, %s, %s, %s },"
                     % (row["sound_db_mod"], cfloat(row["interval"]),
                        cfloat(row["boost_zone_scale"]), cfloat(row["double_chance"]),
                        cfloat(row["explode_chance"]), cfloat(row["explode_scale"]),
                        cfloat(row["base_smoke_amount"]), cfloat(row["shooting_animation_speed"]),
                        cfloat(row["health_boost"]), cfloat(row["revive_duration"])))
    lines.append("};")
    lines.append("")
    lines.append("#define BT_NITEM_SCALING %d" % max(1, len(builder.scaling)))
    lines.append("BT_TABLE BtItemScaling BT_ITEM_SCALING[%d] = {" % max(1, len(builder.scaling)))
    for stat, ratio in builder.scaling:
        lines.append("    { %d, %s },   /* %s */" % (stat, cfloat(ratio), strings.values[stat]))
    lines.append("};")
    lines.append("")
    lines.append("#define BT_NITEM_WSTATS %d" % max(1, len(builder.wstats_rows)))
    lines.append("BT_TABLE BtItemWeaponStats BT_ITEM_WSTATS[%d] = {" % max(1, len(builder.wstats_rows)))
    for i, row in enumerate(builder.wstats_rows):
        lines.append("    { %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d,"
                     % (row["kind"], row["is_healing"], row["can_bounce"],
                        row["deal_dmg_on_return"], row["alternate_attack_type"],
                        row["increase_proj_speed"], row["can_have_positive_knockback"],
                        row["can_have_negative_knockback"], row["attack_type"],
                        row["projectile_scene"], row["scaling_first"], row["scaling_count"]))
        lines.append("      %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d,"
                     % (row["cooldown"], row["damage"], row["min_range"], row["max_range"],
                        row["knockback"], row["recoil"], row["sound_db_mod"],
                        row["additional_cd_every"], row["speed_percent_modifier"],
                        row["nb_projectiles"], row["piercing"], row["bounce"],
                        row["projectile_speed"]))
        lines.append("      %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s },"
                     % (cfloat(row["accuracy"]), cfloat(row["crit_chance"]),
                        cfloat(row["crit_damage"]), cfloat(row["knockback_piercing"]),
                        cfloat(row["effect_scale"]), cfloat(row["lifesteal"]),
                        cfloat(row["recoil_duration"]), cfloat(row["additional_cd_multiplier"]),
                        cfloat(row["projectile_spread"]), cfloat(row["piercing_dmg_reduction"]),
                        cfloat(row["bounce_dmg_reduction"])))
    lines.append("};")
    lines.append("")
    lines.append("#define BT_NITEM_BURNING %d" % max(1, len(builder.burning_rows)))
    lines.append("BT_TABLE BtItemBurning BT_ITEM_BURNING[%d] = {" % max(1, len(builder.burning_rows)))
    for row in builder.burning_rows:
        lines.append("    { %d, %d, %d, 0, 0, 0, %s, %d, %d, %d },"
                     % (row["scaling_first"], row["scaling_count"],
                        1 if row["is_global_burn"] else 0, cfloat(row["chance"]),
                        row["damage"], row["duration"], row["spread"]))
    lines.append("};")
    lines.append("")
    lines.append("#define BT_NITEM_SCENES %d" % max(1, len(builder.scene_rows)))
    lines.append("/* Spawned-scene geometry (pet/structure/projectile/explosion scenes):")
    lines.append(" * kind 0 = unresolved (reason names the extract_physics failure), 1 =")
    lines.append(" * RigidBody2D body, 2 = other Node2D (projectile/explosion).  Circles are")
    lines.append(" * initial-scene, root-local, unrotated geometry.") 
    lines.append(" */")
    lines.append("BT_TABLE BtItemSceneGeom BT_ITEM_SCENES[%d] = {" % max(1, len(builder.scene_rows)))
    for row in builder.scene_rows:
        lines.append("    { %d, %d, %d, %d, %d, %d, 0, 0," % (row["scene"], row["reason"],
                     row["kind"], row["mode"], row["has_hitbox"], row["has_hurtbox"]))
        lines.append("      %s, %s, %s, %s, %s, %s, %s, %s, %s },"
                     % tuple(cfloat(v) for v in (row["body"] + row["hurt"] + row["hit"])))
    lines.append("};")
    lines.append("")
    lines.append("/* Pet behaviour table (brotato_pet_motion.h).  One row per pet class that")
    lines.append(" * an item can instantiate, resolved from that pet's scene + *_stats.tres:")
    lines.append(" * the MovementBehavior node's script and exported fields, the")
    lines.append(" * TargetBehavior script, the Stats speed/health and every interaction")
    lines.append(" * Area2D's CircleShape2D (radius + centre, uniform scales composed).")
    lines.append(" * zone_*[] is indexed by the BT_PET_Z_* ordinals below; an unused zone is")
    lines.append(" * radius 0. */")
    lines.append("#define BT_PET_MOVE_FOLLOW_TARGET      0")
    lines.append("#define BT_PET_MOVE_FOLLOW_TARGET_STOP 1")
    lines.append("#define BT_PET_MOVE_RAND_POS           2")
    lines.append("#define BT_PET_MOVE_PATROL             3")
    lines.append("#define BT_PET_MOVE_FOLLOW_RAND_POS    4")
    lines.append("#define BT_PET_TARGET_NONE   0")
    lines.append("#define BT_PET_TARGET_ENEMY  1")
    lines.append("#define BT_PET_TARGET_PLAYER 2")
    lines.append("#define BT_PET_TARGET_LOOT   3")
    lines.append("#define BT_PET_Z_BOOST  0")
    lines.append("#define BT_PET_Z_ATTRACT 1")
    lines.append("#define BT_PET_Z_PICKUP  2")
    lines.append("#define BT_PET_Z_TREE    3")
    lines.append("#define BT_PET_Z_HEAL    4")
    lines.append("#define BT_PET_Z_TARGET  5")
    lines.append("#define BT_PET_Z_PLAYER  6")
    lines.append("#define BT_PET_Z_DETECT  7")
    lines.append("#define BT_PET_NZONES    8")
    lines.append("typedef struct {")
    lines.append("    unsigned char  opcode;          /* BT_ITEMEFF_* of the instantiated pet */")
    lines.append("    unsigned char  move;            /* BT_PET_MOVE_*                        */")
    lines.append("    unsigned char  target;          /* BT_PET_TARGET_*                      */")
    lines.append("    unsigned char  stop_close;      /* FollowTarget.stop_close_to_target    */")
    lines.append("    unsigned char  is_melee;        /* damages through its own Hitbox       */")
    lines.append("    unsigned char  allow_within;    /* FollowRandPos.allow_within           */")
    lines.append("    unsigned char  fallback_any;    /* closest_enemy: first enemy when none in range */")
    lines.append("    unsigned char  exclude_elite_boss; /* closest_enemy: skip elite/boss    */")
    lines.append("    unsigned char  target_player_if_none; /* target disappears -> the player */")
    lines.append("    unsigned char  _pad0, _pad1, _pad2;")
    lines.append("    unsigned short melee_wstats;    /* WeaponStats row of its own Hitbox (BT_ITEM_EFF_NONE) */")
    lines.append("    unsigned short fx_first;        /* first BT_PET_ZONE_FX row             */")
    lines.append("    unsigned char  fx_count;        /* BT_PET_ZONE_FX rows                  */")
    lines.append("    unsigned char  _pad3;")
    lines.append("    float          speed;           /* Stats.speed                          */")
    lines.append("    float          speed_rand;      /* Stats.speed_randomization            */")
    lines.append("    float          stop_dist;       /* stop distance / range_around_player  */")
    lines.append("    float          rand_rand;       /* FollowRandPos.range_randomization    */")
    lines.append("    float          patrol_rf;       /* PatrolAroundTheMap.radius_factor     */")
    lines.append("    float          edge_x, edge_y;  /* half the pet sprite size (zone inset)*/")
    lines.append("    float          health_base;     /* Stats.health                         */")
    lines.append("    float          health_per_wave; /* Stats.health_increase_each_wave      */")
    lines.append("    float          tree_ratio;      /* Lootworm.tree_damage_ratio (0 = no tree melee) */")
    lines.append("    float          zone_r[BT_PET_NZONES];  /* BT_PET_Z_* -> radius           */")
    lines.append("    float          zone_x[BT_PET_NZONES];  /*            -> centre offset x  */")
    lines.append("    float          zone_y[BT_PET_NZONES];  /*            -> centre offset y  */")
    lines.append("    unsigned int   collision_layer; /* pet scene root RigidBody2D.collision_layer */")
    lines.append("    unsigned int   collision_mask;  /* pet scene root RigidBody2D.collision_mask  */")
    lines.append("} BtPetAI;")
    lines.append("/* The pet's own `effects` array, applied to a player inside its zone and")
    lines.append(" * unapplied when the player leaves (DocMoth BoostZone; effects/items/")
    lines.append(" * double_value_effect.gd + effect.gd). */")
    lines.append("#define BT_PET_FX_HP_REGEN_BONUS    0")
    lines.append("#define BT_PET_FX_DOUBLE_LIFESTEAL  1")
    lines.append("typedef struct {")
    lines.append("    unsigned char kind;             /* BT_PET_FX_*                          */")
    lines.append("    unsigned char _pad0, _pad1, _pad2;")
    lines.append("    float         value;            /* Effect.value                         */")
    lines.append("    float         value2;           /* DoubleValueEffect.value2 (hp % threshold) */")
    lines.append("} BtPetZoneFx;")
    lines.append("#define BT_NPET_ZONE_FX %d" % max(1, len(builder.pet_fx)))
    lines.append("BT_TABLE BtPetZoneFx BT_PET_ZONE_FX[%d] = {" % max(1, len(builder.pet_fx)))
    for kind, value, value2 in builder.pet_fx:
        lines.append("    { %d, 0, 0, 0, %s, %s }," % (kind, cfloat(value), cfloat(value2)))
    if not builder.pet_fx:
        lines.append("    { 0, 0, 0, 0, 0.0f, 0.0f },")
    lines.append("};")
    pets = builder.pets
    lines.append("#define BT_NPET_AI %d" % max(1, len(pets)))
    lines.append("BT_TABLE BtPetAI BT_PET_AI[%d] = {" % max(1, len(pets)))
    for row in pets:
        lines.append("    /* %s -> %s */" % (row["opcode"], row["scene"]))
        lines.append("    { %s, %s, %s, %d, %d, %d, %d, %d, %d, 0, 0, 0,"
                     % (row["opcode_macro"], row["move"], row["target"], row["stop_close"],
                        row["is_melee"], row["allow_within"], row["fallback_any"],
                        row["exclude_elite_boss"], row["target_player_if_none"]))
        lines.append("      %s, %d, %d, 0,"
                     % ("BT_ITEM_EFF_NONE" if row["melee_wstats"] == BT_ITEM_NONE
                        else str(row["melee_wstats"]), row["fx_first"], row["fx_count"]))
        lines.append("      %s, %s, %s, %s, %s, %s, %s, %s, %s, %s,"
                     % (cfloat(row["speed"]), cfloat(row["speed_rand"]),
                        cfloat(row["stop_dist"]), cfloat(row["rand_rand"]),
                        cfloat(row["patrol_rf"]), cfloat(row["edge_x"]), cfloat(row["edge_y"]),
                        cfloat(row["health_base"]), cfloat(row["health_per_wave"]),
                        cfloat(row["tree_ratio"])))
        for field in ("zone_r", "zone_x", "zone_y"):
            idx = 0 if field == "zone_r" else (1 if field == "zone_x" else 2)
            values = []
            for zone in PET_AI_ZONES:
                triple = row["zones"].get(zone)
                values.append(triple[idx] if triple else 0.0)
            lines.append("      { %s }," % ", ".join(cfloat(v) for v in values))
        lines.append("      %d, %d }," % (row["collision_layer"], row["collision_mask"]))
    if not pets:
        lines.append("    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, BT_ITEM_EFF_NONE, 0, 0, 0,")
        lines.append("     0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,")
        lines.append("      { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },")
        lines.append("      { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },")
        lines.append("      { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },")
        lines.append("      0, 0 },")
    lines.append("};")
    lines.append("")
    lines.append("#define BT_NITEM_TAGS %d" % max(1, len(data["tags"])))
    lines.append("BT_TABLE unsigned short BT_ITEM_TAGS[%d] = { %s };"
                 % (max(1, len(data["tags"])),
                    ", ".join(str(t) for t in data["tags"]) if data["tags"] else "BT_ISTR_NONE"))
    lines.append("")
    lines.append("#define BT_NITEM_CUSTOMARGS %d" % max(1, len(builder.customargs)))
    lines.append("BT_TABLE BtItemCustomArg BT_ITEM_CUSTOMARGS[%d] = {"
                 % max(1, len(builder.customargs)))
    for arg_index, arg_sign, arg_format, arg_value, arg_key in builder.customargs:
        lines.append("    { %d, %d, %d, 0, %d, %d }," % (arg_index, arg_sign, arg_format,
                                                          arg_value, arg_key))
    lines.append("};")
    lines.append("")
    lines.append("/* Dynamic stat links (GainStatForEveryStatEffect; run_data.gd")
    lines.append(" * get_scaling_bonus / gain_stat_for_every_stat_effect): when the item is")
    lines.append(" * acquired, gain `value` per `nb` units of the `source` stat into the")
    lines.append(" * `target` stat.  Replaces the hand-maintained BtStatLink indices in")
    lines.append(" * brotato_state.h, which no longer match this pool's item order. */")
    lines.append("typedef struct {")
    lines.append("    unsigned short item;        /* BT_ITEMS index owning the link */")
    lines.append("    unsigned char  target;      /* core stat id gained (BT_STAT_NONE if outside 0..12) */")
    lines.append("    unsigned char  source;      /* core stat id scaled (BT_STAT_NONE if outside 0..12) */")
    lines.append("    float          value;       /* gain per `nb` units of source */")
    lines.append("    int            nb;          /* nb_stat_scaled (may be -1: treat as 1) */")
    lines.append("    unsigned char  perm_stats_only;")
    lines.append("    unsigned short target_key;  /* BT_ISTR_* target stat key */")
    lines.append("    unsigned short source_key;  /* BT_ISTR_* source stat key */")
    lines.append("} BtItemStatLink;")
    links = []
    for slot, row in enumerate(builder.rows):
        if row["opcode"] != "GainStatForEveryStatEffect":
            continue
        target = strings.values[row["key"]]
        source = strings.values[row["aux_key"]]
        links.append((row["item"], STAT_ID.get(target, BT_STAT_NONE),
                      STAT_ID.get(source, BT_STAT_NONE), float(row["value"]),
                      row["nb_stat_scaled"], row["perm_stats_only"], row["key"], row["aux_key"]))
    lines.append("#define BT_NITEM_STAT_LINKS %d" % max(1, len(links)))
    lines.append("BT_TABLE BtItemStatLink BT_ITEM_STAT_LINKS[%d] = {" % max(1, len(links)))
    for item, target, source, value, nb, perm, tk, sk in links:
        owner = "BT_ITEM_NONE" if item == BT_ITEM_NONE else data["items"][item]["id"]
        lines.append("    { %d, %d, %d, %s, %d, %d, %d, %d },   /* %s */"
                     % (item, target, source, cfloat(value), nb, perm, tk, sk, owner))
    if not links:
        lines.append("    { 0, 0, 0, 0.0f, 0, 0, 0, 0 },")
    lines.append("};")
    lines.append("")
    lines.append("/* DLC 1 curse tuning (dlcs/dlc_1/dlc_1_data.gd exports, overridden by"
                 " the shipped dlcs/dlc_1/dlc_data.tres instance). */")
    for name, value in sorted(data["curse"].items()):
        lines.append("#define BT_DLC1_CURSE_%s %s" % (sanitize(name), cfloat(value)))
    lines.append("")
    lines.append("#ifndef __METAL_VERSION__")
    lines.append("#ifdef __cplusplus")
    lines.append("#define BT_ITEM_ASSERT(c, m) static_assert(c, m)")
    lines.append("#else")
    lines.append("#define BT_ITEM_ASSERT(c, m) _Static_assert(c, m)")
    lines.append("#endif")
    lines.append('BT_ITEM_ASSERT(BT_NITEMS <= 65535, "item index must fit unsigned short");')
    lines.append('BT_ITEM_ASSERT(BT_NITEM_EFFECTS <= 65535, "legacy effect pool must fit unsigned short");')
    lines.append('BT_ITEM_ASSERT(BT_NITEM_EFF_ROWS <= 65535, "effect rows must fit unsigned short");')
    lines.append('BT_ITEM_ASSERT(BT_NITEM_EFF_INDEX <= 65535, "effect index pool must fit unsigned short");')
    lines.append('BT_ITEM_ASSERT(BT_NITEM_STRS <= 65535, "interned strings must fit unsigned short");')
    lines.append("#undef BT_ITEM_ASSERT")
    lines.append("#endif")
    lines.append("")
    return _write("\n".join(lines), out_path)


def _emit_ushort_table(lines, values, per_line=12):
    for i in range(0, max(1, len(values)), per_line):
        chunk = values[i:i + per_line]
        lines.append("    " + ", ".join(str(v) for v in chunk) + ",")


# --------------------------------------------------------------------------
# Contract doc + provenance
# --------------------------------------------------------------------------
def emit_contract(data, out_path, extra_opcodes=()):
    builder = data["builder"]
    lines = []
    lines.append("# Brotato item-effect data contract")
    lines.append("")
    lines.append("Generated by `ocean/brotato/extract_items.py`.  Companion headers:")
    lines.append("`ocean/brotato/brotato_items.h` (`BtItem`, `BT_ITEMS`, `BT_ITEM_EFFECTS`)")
    lines.append("and `ocean/brotato/brotato_item_effects.h` (structured graph).  Every")
    lines.append("value is resolved from the recovered campaign + Abyss DLC resources.")
    lines.append("")
    lines.append("## Counts")
    lines.append("")
    lines.append("| table | rows |")
    lines.append("|---|---|")
    lines.append("| items | %d |" % len(data["items"]))
    lines.append("| legacy stat pool `BT_ITEM_EFFECTS` | %d |" % len(data["legacy"]))
    lines.append("| effect rows `BT_ITEM_EFF_ROWS` | %d |" % len(builder.rows))
    lines.append("| effect index `BT_ITEM_EFF_INDEX` | %d |" % len(builder.eff_index))
    lines.append("| weapon stats `BT_ITEM_WSTATS` | %d |" % len(builder.wstats_rows))
    lines.append("| scaling rows `BT_ITEM_SCALING` | %d |" % len(builder.scaling))
    lines.append("| burning rows `BT_ITEM_BURNING` | %d |" % len(builder.burning_rows))
    lines.append("| scene geometry `BT_ITEM_SCENES` | %d |" % len(builder.scene_rows))
    lines.append("| pet behaviour `BT_PET_AI` | %d |" % len(builder.pets))
    lines.append("| interned strings `BT_ITEM_STR_NAMES` | %d |" % len(builder.strings.values))
    lines.append("| dynamic stat links `BT_ITEM_STAT_LINKS` | %d |"
                 % sum(1 for r in builder.rows if r["opcode"] == "GainStatForEveryStatEffect"))
    lines.append("")
    lines.append("The legacy stat ids are the canonical core ids 0..12: 0 max_hp, 1")
    lines.append("hp_regeneration, 2 armor, 3 speed, 4 dodge, 5 percent_damage, 6")
    lines.append("melee_damage, 7 ranged_damage, 8 elemental_damage, 9 crit_chance, 10")
    lines.append("luck, 11 crit_damage, 12 harvesting.  Id 12 reaches the core through its")
    lines.append("`default:` branch, so the legacy pool now emits ids up to 12 where the")
    lines.append("former 147-item table only used 0..10 -> the core observation's dominant")
    lines.append("stat feature (`dom / 10`) can now exceed 1.  Every id also appears in the")
    lines.append("structured graph with its interned key for exact handling.")
    lines.append("")
    lines.append("## `BtItem` fields (brotato_items.h)")
    lines.append("")
    lines.append("| field | type | semantics |")
    lines.append("|---|---|---|")
    for name, ctype, doc in ITEM_FIELDS:
        lines.append("| `%s` | `%s` | %s |" % (name, ctype, doc))
    lines.append("")
    lines.append("## `BtItemEff` fields (brotato_item_effects.h)")
    lines.append("")
    lines.append("| field | type | semantics |")
    lines.append("|---|---|---|")
    for name, ctype, doc in ITEEM_FIELDS:
        lines.append("| `%s` | `%s` | %s |" % (name, ctype, doc))
    lines.append("")
    lines.append("## `BtItemWeaponStats` fields")
    lines.append("")
    lines.append("| field | type | semantics |")
    lines.append("|---|---|---|")
    for name, ctype, doc in WSTATS_FIELDS:
        lines.append("| `%s` | `%s` | %s |" % (name, ctype, doc))
    lines.append("")
    lines.append("## Flag bits (`BT_ITFLAG_*`)")
    lines.append("")
    lines.append("| macro | bit | semantics |")
    lines.append("|---|---|---|")
    for bit, macro, doc in FLAG_DOCS:
        lines.append("| `%s` | `0x%08x` | %s |" % (macro, bit, doc))
    lines.append("")
    lines.append("## Effect opcodes")
    lines.append("")
    lines.append("`BT_ITEMEFF_UNSUPPORTED` (0) marks a source effect class outside")
    lines.append("`effects/`; its class name is recorded in the provenance JSON.  Every")
    lines.append("other opcode is a `class_name` from the recovered effect scripts.")
    lines.append("")
    lines.append("| opcode | class |")
    lines.append("|---|---|")
    opcodes = sorted({r["opcode"] for r in builder.rows if r["item"] != BT_ITEM_NONE})
    for cls in sorted({r["opcode"] for r in builder.rows if r["item"] == BT_ITEM_NONE}
                      - set(opcodes)):
        opcodes.append(cls)
    for cls in sorted(set(extra_opcodes) - set(opcodes)):
        opcodes.append(cls)
    lines.append("| `BT_ITEMEFF_UNSUPPORTED` | (unsupported) |")
    for cls in opcodes:
        lines.append("| `%s` | %s |" % (opcode_macro(cls), cls))
    lines.append("")
    lines.append("## Dynamic stat links")
    lines.append("")
    lines.append("`BT_ITEM_STAT_LINKS` / `BT_NITEM_STAT_LINKS` replace the hand-maintained")
    lines.append("`BtStatLink` indices in `brotato_state.h` (75/96/117), which no longer")
    lines.append("match this pool's item order (the pool is the complete 247-item campaign +")
    lines.append("DLC set, not the former 147-item subset).  Fields:")
    lines.append("")
    lines.append("| field | semantics |")
    lines.append("|---|---|")
    lines.append("| `item` | `BT_ITEMS` index owning the link |")
    lines.append("| `target` | core stat id gained (`BT_STAT_NONE` if outside 0..12) |")
    lines.append("| `source` | core stat id scaled (`BT_STAT_NONE` if outside 0..12) |")
    lines.append("| `value` | gain per `nb` units of the source stat |")
    lines.append("| `nb` | `nb_stat_scaled` (`int`: may be -1, the game's per-unit sentinel) |")
    lines.append("| `perm_stats_only` | `GainStatForEveryStatEffect.perm_stats_only` |")
    lines.append("| `target_key` | `BT_ISTR_*` interned target stat key |")
    lines.append("| `source_key` | `BT_ISTR_*` interned source stat key |")
    lines.append("")
    lines.append("## Item groups")
    lines.append("")
    lines.append("| bit | macro | members |")
    lines.append("|---|---|---|")
    for bit, name in enumerate(data["group_names"]):
        lines.append("| %d | `BT_ITEM_GROUP_%s` | %s |"
                     % (bit, sanitize(name), ", ".join(data["groups"][name])))
    lines.append("")
    lines.append("## Shop bans")
    lines.append("")
    lines.append("- `BT_ENDLESS_BANNED_ITEMS` / `_N`: `ItemService.banned_items_for_endless`.")
    lines.append("- `BT_BULL_BANNED_ITEMS` / `_N`: Bull `banned_items`")
    lines.append("  (%s)." % ", ".join(data["bull"]["banned_items"]))
    lines.append("- `BT_BULL_BANNED_GROUP_MASK`: Bull `banned_item_groups` (%s)."
                 % ", ".join(data["bull"]["banned_groups"]))
    lines.append("- `BT_BULL_WANTED_TAGS` / `_N`: Bull `wanted_tags` (interned ids).")
    lines.append("")
    return _write("\n".join(lines), out_path)


def emit_provenance(data, out_path, args):
    builder = data["builder"]
    strings = builder.strings
    items = []
    for row in data["items"]:
        effects = []
        slots = builder.eff_index[row["eff_first"]:row["eff_first"] + row["eff_count"]]
        for slot in _recursive_rows(slots, builder.rows, builder.eff_index):
            eff = builder.rows[slot]
            entry = {
                "row": slot,
                "class": eff["opcode"],
                "opcode": ("BT_ITEMEFF_UNSUPPORTED" if not eff["opcode_supported"]
                           else opcode_macro(eff["opcode"])),
                "key": strings.values[eff["key"]],
                "flags": eff["flags"],
                "source": eff["source"],
                "file": builder.R.find(eff["source"]),
            }
            if eff["extra_group_path"]:
                entry["extra_group"] = {"path": eff["extra_group_path"],
                                        "ordinal": eff["extra_group"]}
            effects.append(entry)
        items.append({
            "index": row["index"],
            "id": row["id"],
            "name": row["name"],
            "source": row["source"],
            "file": builder.R.find(row["source"]),
            "price": row["price"],
            "tier": row["tier"],
            "max_nb": row["max_nb"],
            "tags": [strings.values[t] for t in row["tags"]],
            "group_mask": row["group_mask"],
            "flags": row["flags"],
            "replaced_by": (strings.values[0] if row["replaced_index"] == BT_ITEM_NONE
                            else data["items"][row["replaced_index"]]["id"]),
            "legacy": {"first": row["first"], "count": row["count"],
                       "special": row["special"], "proc_c": row["proc_c"],
                       "proc_d": row["proc_d"], "cap_stat": row["cap_stat"],
                       "cap_val": row["cap_val"]},
            "effects": effects,
        })
    payload = {
        "generator": "ocean/brotato/extract_items.py",
        "campaign_root": args.campaign_root,
        "dlc_root": args.dlc_root,
        "counts": {
            "items": len(data["items"]),
            "campaign_items": len(data["campaign"]),
            "campaign_dirs": data["dir_stats"]["campaign"]["dirs"],
            "dlc_items": len(data["dlc"]),
            "dlc_dirs": data["dir_stats"]["dlc"]["dirs"],
            "legacy_effects": len(data["legacy"]),
            "effect_rows": len(builder.rows),
            "weapon_stats": len(builder.wstats_rows),
            "stat_links": sum(1 for r in builder.rows
                              if r["opcode"] == "GainStatForEveryStatEffect"),
            "custom_args": len(builder.customargs),
            "burning": len(builder.burning_rows),
            "scenes": len(builder.scene_rows),
            "pets": len(builder.pets),
            "interned_strings": len(strings.values),
        },
        "string_ids": {text: strings.ids[text] for text in strings.values},
        "items": items,
        "unsupported": builder.unsupported,
    }
    _write(json.dumps(payload, indent=1, sort_keys=False) + "\n", out_path)


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
    wdata = None
    if getattr(args, "weapons_out", None):
        import extract_weapons as weapons  # lazy import avoids an import cycle
        wdata = weapons.build_weapons(data, args)
    emit_effects(data, args.effects_out, args.campaign_root, args.dlc_root)
    emit_items(data, args.items_out, args.campaign_root, args.dlc_root)
    emit_contract(data, args.contract_out)
    if wdata is not None:
        import extract_weapons as weapons
        weapons.emit_weapon_types(wdata, args.weapon_types_out)
        weapons.emit_weapons(wdata, args.weapons_out)
        weapons.emit_weapons_contract(
            wdata, getattr(args, "weapon_contract_out",
                           "ocean/brotato/brotato_weapons_contract.md"))
        weapon_prov = getattr(args, "weapon_provenance_out", "")
        if weapon_prov:
            weapons.emit_weapon_provenance(wdata, weapon_prov, args)
    emit_provenance(data, args.provenance_out, args)
    data["weapons"] = wdata
    return data


def main(argv):
    parser = argparse.ArgumentParser(description="Extract the Brotato item pool.",
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--campaign-root", required=True)
    parser.add_argument("--dlc-root", required=True)
    parser.add_argument("--items-out", default="ocean/brotato/brotato_items.h")
    parser.add_argument("--effects-out", default="ocean/brotato/brotato_item_effects.h")
    parser.add_argument("--weapons-out", default="ocean/brotato/brotato_weapons.h")
    parser.add_argument("--weapon-types-out", default="ocean/brotato/brotato_weapon_types.h")
    parser.add_argument("--weapon-contract-out",
                        default="ocean/brotato/brotato_weapons_contract.md")
    parser.add_argument("--contract-out", default="ocean/brotato/brotato_item_effects_contract.md")
    parser.add_argument("--provenance-out", required=True,
                        help="external JSON provenance path (outside the game repo)")
    parser.add_argument("--weapon-provenance-out", default="",
                        help="external JSON weapon provenance path (optional)")
    args = parser.parse_args(argv)
    try:
        data = emit_all(args)
    except ExtractError as exc:
        sys.stderr.write("extract_items: error: %s\n" % exc)
        return 1
    sys.stderr.write(
        "extract_items: %d items (%d campaign in %d dirs + %d DLC in %d dirs), "
        "%d legacy effects, %d effect rows, %d weapon-stat rows, %d stat links, "
        "%d burning, %d scenes, %d interned strings -> %s, %s\n"
        % (len(data["items"]), data["dir_stats"]["campaign"]["items"],
           data["dir_stats"]["campaign"]["dirs"], len(data["dlc"]),
           data["dir_stats"]["dlc"]["dirs"], len(data["legacy"]),
           len(data["builder"].rows), len(data["builder"].wstats_rows),
           data["counts"]["stat_links"] if "counts" in data else
           sum(1 for r in data["builder"].rows if r["opcode"] == "GainStatForEveryStatEffect"),
           len(data["builder"].burning_rows), len(data["builder"].scene_rows),
           len(data["builder"].strings.values), args.items_out, args.effects_out))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
