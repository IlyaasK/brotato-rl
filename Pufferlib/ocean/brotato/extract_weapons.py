#!/usr/bin/env python3
"""Extract the complete Brotato player-weapon pool (campaign + Abyss DLC) into
the portable headers ``brotato_weapons.h`` and ``brotato_weapon_types.h``.

Ground truth is the recovered Godot 3.x project (campaign root
``brotato_recovered`` + ``brotato_abyss`` DLC root).  Every row is resolved
from the real ``.tres`` / ``.tscn`` resources; nothing is hand-authored and no
value is invented.  Re-running reproduces byte-identical headers.

Discovery is the *source graph*: the canonical weapon pool is the ``weapons``
array of ``singletons/item_service.tscn`` (campaign) plus the ``weapons`` array
of ``dlcs/dlc_1/dlc_data.tres`` (DLC).  The directory trees
(``weapons/**/*_data.tres``, ``dlcs/dlc_1/weapons/**/*_data.tres``) are
cross-checked against it; an unregistered template resource carrying an empty
``weapon_id`` (a leftover of the removed ``weapon_classes`` schema) is reported
and excluded.

Effect namespace
----------------
There is ONE canonical effect-row namespace.  The weapon effect graph is
*appended* to the existing ``extract_items`` graph through the same
``Builder``/resolver/interner, so ``BT_ITEM_EFF_ROWS`` / ``BT_ITEM_EFF_INDEX`` /
``BT_ITEM_BURNING`` / ``BT_ITEM_SCENES`` / ``BT_ITEM_WSTATS`` / ``BT_ITEM_SCALING``
/ ``BT_ITEM_CUSTOMARGS`` carry the item rows first (byte-identical ids/rows)
and the weapon rows after.  Weapon-only rows set ``BtItemEff.item ==
BT_ITEM_NONE`` (no fake item 0).  ``BtWeaponDef.fx_first/fx_count`` index the
weapon-scoped list ``BT_WEAPON_EFF_INDEX`` (canonical ``BT_ITEM_EFF_ROWS``
indices); nested refs (``child``/``sub_*``/``stats``/``burning``) stay in the
canonical graph.  Weapon base WeaponStats rows live in ``BT_WEAPON_STATS``
(``BtItemWeaponStats``) with scaling in ``BT_WEAPON_SCALING`` (``BtItemScaling``).

What is emitted
---------------
* ``brotato_weapon_types.h`` -- ``BtWeaponDef`` / ``BtWeaponGeom`` /
  ``BtWeaponSet`` / ``BtWeaponSetTier`` types, ``BT_WEAPON_*`` / ``BT_WFLAG_*``
  macros and the ``BT_ISTR_WSTAT_*`` aliases.
* ``brotato_weapons.h`` -- ``BT_WEAPONS`` and the weapon tables above, plus the
  player weapon-container attachment ring ``BT_WEAPON_ATTACH_X/Y`` and the
  host-side id resolvers.

Audio clips
-----------
Every weapon's source sound arrays are resolved to their real clips, and each
clip's true length is read from the actual WAV/MP3/OGG bytes (never guessed):

* ``WeaponStats.shooting_sounds`` -- played by
  ``melee_weapon_shooting_behavior.gd:63,105`` and
  ``ranged_weapon_shooting_behavior.gd:10`` through
  ``SoundManager.play(Utils.get_rand_element(...))``.
* ``custom_hit_sounds`` -- the weapon scene root export, played by
  ``weapon.gd:432-433`` through ``SoundManager2D.play(Utils.get_rand_element(...))``.
* the nested ``RangedWeaponStats.shooting_sounds`` a weapon's
  ``ProjectilesOnHitEffect`` row carries -- played by
  ``weapon_service.gd:339-351`` (``manage_special_spawn_projectile``) for every
  on-hit child projectile, with the same one bounded draw.  Those rows are
  indexed by their canonical ``BT_ITEM_WSTATS`` row in the SINGLE canonical
  nested table ``BT_AUDIO_WSTATS_SHOOT_FIRST/COUNT`` of
  ``brotato_audio_data.h`` (CombatAudioData); this generator does not emit a
  second copy.

``Array.pick_random`` (``utils.gd:391-394``) consumes ONE global RNG bounded
draw, and ``get_rand_element`` returns ``null`` for an empty array without
drawing.  ``BtWeaponDef.shoot_audio_first/shoot_audio_count`` and
``hit_audio_first/hit_audio_count`` span ``BT_WEAPON_AUDIO_DURATION``; a zero
count is the source's ``size() > 0`` guard being false (no draw, no enqueue).
Identical ordered clip sequences share one span.

Usage::

    python3 ocean/brotato/extract_weapons.py \\
        --campaign-root /path/brotato_recovered \\
        --dlc-root      /path/brotato_abyss \\
        --provenance-out /path/elsewhere/brotato_items.json
"""

from __future__ import annotations

import argparse
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import extract_physics as physics  # noqa: E402
import extract_gameplay as gameplay  # noqa: E402
import extract_items as items  # noqa: E402

ExtractError = physics.ExtractError
get_str = gameplay.get_str
get_int = gameplay.get_int
get_float = gameplay.get_float
get_bool = gameplay.get_bool

_ANY_REF_RE = items._ANY_REF_RE
_STR_RE = items._STR_RE
_SCALING_RE = items._SCALING_RE
cfloat = items.cfloat
sanitize = items.sanitize
BT_ITEM_NONE = items.BT_ITEM_NONE


def f32_literal(value):
    """A C float literal that ROUND-TRIPS its float32 value.

    items.cfloat prints ``%.6g``, which is enough for the small stat values but
    truncates a computed constant (e.g. ``(float)(0.9 * PI)`` came out 10 float32
    ulps low); repr() of the float32 value always has the <= 9 significant digits
    a float32 needs, so the C compiler narrows it back to the same bits.
    """
    return repr(struct.unpack("<f", struct.pack("<f", float(value)))[0]) + "f"

BT_WEAPON_NONE = 0xFFFF
BT_WEAPON_SLOTS = 6
BT_WEAPON_ATTACH_MAX = 6

WFLAG_UNLOCKED_BY_DEFAULT = 0x01
WFLAG_LOCKABLE = 0x02
WFLAG_HAS_EFFECTS = 0x04
WFLAG_IS_MELEE = 0x08

# entity_service.gd:94 -- the only weapon_ids that spawn a structure.
STRUCTURE_WEAPON_IDS = ("weapon_screwdriver", "weapon_wrench", "weapon_pruner")

# weapon_service.gd DEFAULT_PROJECTILE_SCENE (used when a ranged stats row has
# no projectile_scene).
DEFAULT_BULLET_SCENE = "res://projectiles/bullet/bullet_projectile.tscn"


# --------------------------------------------------------------------------
# Audio clip durations
#
# Weapon sound playback picks one clip from a GDScript Array (weapon.gd:432-433,
# melee_weapon_shooting_behavior.gd:63,105, ranged_weapon_shooting_behavior.gd:10)
# and the runtime advances the playback lifetime by the clip's real length.  The
# seconds below are read from the actual source bytes -- RIFF/WAVE chunk sizes,
# MPEG frame headers, or the Ogg Vorbis granule position -- and never guessed.
# --------------------------------------------------------------------------
_MP3_BITRATES = {
    3: (0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320),
    2: (0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160),
}
_MP3_SAMPLE_RATES = {3: (44100, 48000, 32000),
                     2: (22050, 24000, 16000),
                     0: (11025, 12000, 8000)}
_AUDIO_DURATION_CACHE = {}


def _wav_duration(abs_path, where):
    """RIFF/WAVE length in seconds from the fmt/data chunk sizes."""
    with open(abs_path, "rb") as handle:
        blob = handle.read()
    if blob[:4] != b"RIFF" or blob[8:12] != b"WAVE":
        raise ExtractError("%s: not a RIFF/WAVE stream" % where)
    fmt = None
    data_size = None
    off = 12
    while off + 8 <= len(blob):
        chunk_id = blob[off:off + 4]
        size = struct.unpack_from("<I", blob, off + 4)[0]
        if chunk_id == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", blob, off + 8)
        elif chunk_id == b"data":
            data_size = size
        off += 8 + size + (size & 1)
    if fmt is None or data_size is None:
        raise ExtractError("%s: WAV without a fmt or data chunk" % where)
    format_tag, channels, rate, _byte_rate, _align, bits = fmt
    if format_tag != 1 or bits % 8 or channels == 0 or rate == 0:
        raise ExtractError("%s: unsupported WAV fmt %d/%d-bit/%dch"
                           % (where, format_tag, bits, channels))
    return data_size / float(rate * channels * (bits // 8))


def _mp3_duration(abs_path, where):
    """MPEG Layer III length in seconds by summing every audio frame."""
    with open(abs_path, "rb") as handle:
        blob = handle.read()
    off = 0
    if blob[:3] == b"ID3":
        off = 10 + ((blob[6] << 21) | (blob[7] << 14) | (blob[8] << 7) | blob[9])
    seconds = 0.0
    frames = 0
    while off + 4 <= len(blob):
        header = struct.unpack_from(">I", blob, off)[0]
        if (header & 0xFFE00000) != 0xFFE00000:
            off += 1
            continue
        version = (header >> 19) & 3
        layer = (header >> 17) & 3
        bitrate_index = (header >> 12) & 0xF
        rate_index = (header >> 10) & 3
        padding = (header >> 9) & 1
        if version == 1 or layer != 1 or bitrate_index in (0, 15) or rate_index == 3:
            off += 1
            continue
        samples = 1152 if version == 3 else 576
        rate = _MP3_SAMPLE_RATES[version][rate_index]
        frame_len = (samples // 8) * _MP3_BITRATES[version][bitrate_index] * 1000 // rate
        frame_len += padding
        if frame_len <= 4:
            off += 1
            continue
        seconds += samples / float(rate)
        frames += 1
        off += frame_len
    if frames == 0:
        raise ExtractError("%s: no MPEG audio frames found" % where)
    return seconds


def _ogg_duration(abs_path, where):
    """Ogg Vorbis length in seconds from the last page's granule position."""
    with open(abs_path, "rb") as handle:
        blob = handle.read()
    if blob[:4] != b"OggS":
        raise ExtractError("%s: not an Ogg stream" % where)
    segments = blob[26]
    body = 27 + segments
    packet = blob[body:body + blob[27]]
    if packet[:7] != b"\x01vorbis":
        raise ExtractError("%s: not an Ogg Vorbis identification header" % where)
    rate = struct.unpack_from("<I", packet, 12)[0]
    if rate == 0:
        raise ExtractError("%s: Ogg Vorbis sample rate is 0" % where)
    last = blob.rfind(b"OggS")
    granule = struct.unpack_from("<q", blob, last + 6)[0]
    return granule / float(rate)


def audio_clip_duration(resolver, res_path):
    """True length (seconds) of the sound resource at ``res_path``."""
    abs_path = resolver.find(res_path)
    cached = _AUDIO_DURATION_CACHE.get(abs_path)
    if cached is not None:
        return cached
    ext = os.path.splitext(abs_path)[1].lower()
    if ext == ".wav":
        seconds = _wav_duration(abs_path, res_path)
    elif ext == ".mp3":
        seconds = _mp3_duration(abs_path, res_path)
    elif ext == ".ogg":
        seconds = _ogg_duration(abs_path, res_path)
    else:
        raise ExtractError("%s: unsupported audio clip type %r" % (res_path, ext))
    _AUDIO_DURATION_CACHE[abs_path] = seconds
    return seconds


def _audio_float(value):
    """C float literal that keeps the value's binary32 encoding intact.

    Clip lengths are GDScript ``AudioStream.get_length()`` values (binary32);
    the shared 6-significant-digit ``cfloat`` would perturb that.  Round to
    binary32 first, then emit 9 significant digits, which is exactly enough to
    round-trip any float32."""
    value = struct.unpack("<f", struct.pack("<f", float(value)))[0]
    text = "%.9g" % value
    if "e" in text or "E" in text:
        return text + "f"
    if "." not in text:
        text += ".0"
    return text + "f"


# --------------------------------------------------------------------------
# Source keys interned for weapon/profile/shop consumers (append-only; the
# standard BT_ISTR_<UPPER> macro is emitted for each).  Ids already interned by
# the item/weapon pass keep their number.
# --------------------------------------------------------------------------
REQUESTED_KEYS = (
    "weapon_slot", "weapon_slot_upgrades", "starting_weapon", "cursed_starting_weapon",
    "no_melee_weapons", "no_ranged_weapons", "no_duplicate_weapons",
    "min_weapon_tier", "max_weapon_tier", "max_melee_weapons", "max_ranged_weapons",
    "free_weapon_slots", "weapons_price", "specific_items_price",
    "minimum_weapons_in_shop", "remove_shop_items", "destroy_weapons", "hp_shop",
    "unique_weapons_count", "all_weapons_count_for_sets", "convert_stats_end_of_wave",
    "weapon_type_bonus", "weapon_class_bonus", "stat_beast_master_effect",
    "maximum_weapon_cooldowns", "can_attack_while_moving", "can_burn_enemies",
    "bounce_damage", "bounce_on_crit", "pierce_on_crit", "break_on_hit", "charm_on_hit",
    "lightning_on_hit", "slow_on_hit", "one_shot_on_hit", "negative_knockback",
    "reload_when_pickup_gold", "reload_turrets_on_shoot",
    "bonus_damage_against_targets_below_hp",
    "bonus_non_elemental_damage_against_burning_targets", "bonus_current_health_damage",
    "crit_on_hitting_burning_target", "explode_on_overkill",
    "stat_double_lifesteal_bonus", "gain_stat_when_attack_killed_enemies",
    "slow_in_zone", "additional_weapon_effects", "gain_stat_for_every_step_after_equip",
    "stat_all",
    "damage", "max_range", "min_range", "cooldown", "accuracy", "crit_chance",
    "crit_damage", "lifesteal", "knockback", "knockback_piercing", "effect_scale",
    "recoil", "recoil_duration", "additional_cooldown_every_x_shots",
    "additional_cooldown_multiplier", "speed_percent_modifier", "attack_speed_mod",
    "nb_projectiles", "projectile_spread", "piercing", "piercing_dmg_reduction",
    "bounce", "bounce_dmg_reduction", "projectile_speed", "is_healing", "can_bounce",
    "deal_dmg_on_return", "alternate_attack_type", "attack_type",
    "increase_projectile_speed_with_range",
)

WSTAT_ALIASES = (
    "damage", "max_range", "min_range", "cooldown", "accuracy", "crit_chance",
    "crit_damage", "lifesteal", "knockback", "knockback_piercing", "effect_scale",
    "recoil", "recoil_duration", "additional_cooldown_every_x_shots",
    "additional_cooldown_multiplier", "speed_percent_modifier", "attack_speed_mod",
    "nb_projectiles", "projectile_spread", "piercing", "piercing_dmg_reduction",
    "bounce", "bounce_dmg_reduction", "projectile_speed", "is_healing", "can_bounce",
    "deal_dmg_on_return", "alternate_attack_type", "attack_type",
    "increase_projectile_speed_with_range",
)


# --------------------------------------------------------------------------
# Schema (single source of truth for emission)
# --------------------------------------------------------------------------
WEAPON_DEF_FIELDS = (
    ("id", "const char*", True, "WeaponData.weapon_id stem, e.g. \"weapon_hatchet\" (host-only)"),
    ("my_id", "const char*", True, "canonical per-tier id, e.g. \"weapon_hatchet_1\" (host-only)"),
    ("name", "const char*", True, "ItemParentData.name localization key (host-only)"),
    ("icon", "const char*", True, "icon resource path (host-only)"),
    ("weapon_id", "unsigned short", False, "BT_ISTR_* of `id` (weapon_id group)"),
    ("stats", "unsigned short", False, "BT_WEAPON_STATS row (uncursed base WeaponStats)"),
    ("geom", "unsigned short", False, "BT_WEAPON_GEOM row"),
    ("upgrades_into", "unsigned short", False, "BT_WEAPONS row of the next tier, BT_WEAPON_NONE if top"),
    ("family", "unsigned short", False, "BT_WEAPONS row: first (lowest-tier) variant of `id`"),
    ("set_first", "unsigned short", False, "first index into BT_WEAPON_SET_IDS"),
    ("set_count", "unsigned char", False, "number of sets on this weapon"),
    ("tier", "unsigned char", False, "ItemParentData.tier 0..5"),
    ("kind", "unsigned char", False, "WeaponData.Type: 0 MELEE, 1 RANGED"),
    ("can_be_looted", "unsigned char", False, "ItemParentData.can_be_looted"),
    ("is_cursed", "unsigned char", False, "ItemParentData.is_cursed template flag"),
    ("spawns_structure", "unsigned char", False, "EntityService.is_weapon_spawning_structure"),
    ("flags", "unsigned char", False, "BT_WFLAG_*"),
    ("shooting_sounds", "unsigned char", False, "WeaponStats.shooting_sounds count (RNG bounded-draw size)"),
    ("fx_first", "unsigned short", False, "first index into BT_WEAPON_EFF_INDEX"),
    ("fx_count", "unsigned short", False, "number of top-level weapon effect rows"),
    ("start_first", "unsigned short", False, "first index into BT_WEAPON_START"),
    ("start_count", "unsigned char", False, "add_to_chars_as_starting entries"),
    ("value", "unsigned short", False, "ItemParentData.value base shop price"),
    ("curse_factor", "float", False, "max(ItemParentData.curse_factor, effect curse_factor)"),
    ("cooldown", "int", False, "== BT_WEAPON_STATS[stats].cooldown (ticks)"),
    ("damage", "int", False, "== BT_WEAPON_STATS[stats].damage"),
    ("max_range", "int", False, "== BT_WEAPON_STATS[stats].max_range"),
    ("crit_chance", "float", False, "== BT_WEAPON_STATS[stats].crit_chance"),
    ("shoot_audio_first", "unsigned short", False,
     "first BT_WEAPON_AUDIO_DURATION row of the WeaponStats.shooting_sounds clips"),
    ("shoot_audio_count", "unsigned short", False,
     "shooting_sounds.size(); 0 => the source guard skips the pick_random draw"),
    ("hit_audio_first", "unsigned short", False,
     "first BT_WEAPON_AUDIO_DURATION row of the scene custom_hit_sounds clips"),
    ("hit_audio_count", "unsigned short", False,
     "custom_hit_sounds.size(); 0 => no SoundManager2D.play and no draw"),
)

WEAPON_GEOM_FIELDS = (
    ("scene", "unsigned short", "BT_ISTR_* weapon PackedScene path"),
    ("shape", "unsigned char", "BT_WEAPON_SHAPE_* of Sprite/Hitbox/Collision"),
    ("is_melee", "unsigned char", "scene root script is MeleeWeapon"),
    ("hit_circle_r", "float", "CircleShape2D.radius (0 when rect/none)"),
    ("hit_rect_hx", "float", "RectangleShape2D.extents.x (0 when circle/none)"),
    ("hit_rect_hy", "float", "RectangleShape2D.extents.y (0 when circle/none)"),
    ("hit_x", "float", "Hitbox centre (weapon-root frame)"),
    ("hit_y", "float", "Hitbox centre (weapon-root frame)"),
    ("sprite_x", "float", "Sprite.position (weapon-root frame)"),
    ("sprite_y", "float", "Sprite.position (weapon-root frame)"),
    ("sprite_off_x", "float", "Sprite.offset.x"),
    ("sprite_off_y", "float", "Sprite.offset.y"),
    ("muzzle_x", "float", "Muzzle centre (weapon-root frame)"),
    ("muzzle_y", "float", "Muzzle centre (weapon-root frame)"),
    ("attach_x", "float", "Attach centre (weapon-root frame)"),
    ("attach_y", "float", "Attach centre (weapon-root frame)"),
    ("range_radius", "float", "Range/CollisionShape2D.radius (scene authored value)"),
)

WEAPON_SET_FIELDS = (
    ("my_id", "unsigned short", "BT_ISTR_* SetData.my_id"),
    ("name", "unsigned short", "BT_ISTR_* SetData.name localization key"),
    ("tier_first", "unsigned short", "first BtWeaponSetTier row"),
    ("nb_tiers", "unsigned char", "set_bonuses.size()"),
)

WEAPON_PROJ_FIELDS = (
    ("scene", "unsigned short", "BT_ISTR_* projectile PackedScene path (BT_ISTR_NONE = default bullet)"),
    ("hit_radius", "float", "Hitbox Area2D CircleShape2D radius"),
    ("hit_x", "float", "Hitbox circle centre (scene-root frame)"),
    ("hit_y", "float", "Hitbox circle centre (scene-root frame)"),
    ("rot_speed", "float", "PlayerProjectile.rotation_speed (export)"),
    ("stop_delay", "float", "Projectile.stop_delay (export)"),
    ("delay", "float", "DelayedPlayerProjectile.delay scene override (0 = runtime rand_range)"),
    ("slow_rad", "float", "optional SlowHitbox Area2D circle radius (0 if none)"),
    ("slow_amt", "float", "slow hitbox add_decaying_speed amount (0 if none)"),
)

# Resolved view of a weapon's ProjectilesOnHitEffect child (WeaponStats fills it
# at runtime from the effect row E and the nested stats row st).
WEAPON_ONHIT_FIELDS = (
    ("count", "unsigned char", "ProjectilesOnHitEffect.value (projectiles per hit)"),
    ("auto_target", "unsigned char", "ProjectilesOnHitEffect.auto_target_enemy"),
    ("piercing", "int", "nested RangedWeaponStats.piercing"),
    ("pierce_reduce", "float", "nested RangedWeaponStats.piercing_dmg_reduction"),
    ("bounce", "int", "nested RangedWeaponStats.bounce"),
    ("bounce_reduce", "float", "nested RangedWeaponStats.bounce_dmg_reduction"),
    ("dmg", "float", "nested RangedWeaponStats.damage"),
    ("crit_chance", "float", "nested RangedWeaponStats.crit_chance"),
    ("crit_dmg", "float", "nested RangedWeaponStats.crit_damage"),
    ("burn_chance", "float", "nested burning_data.chance"),
    ("burn_dmg", "float", "nested burning_data.damage"),
    ("burn_dur", "float", "nested burning_data.duration (ticks)"),
    ("max_range", "float", "nested RangedWeaponStats.max_range"),
    ("speed", "float", "nested RangedWeaponStats.projectile_speed"),
    ("projectile_scene", "unsigned short", "nested RangedWeaponStats.projectile_scene (BT_ISTR_* / NONE)"),
)


# --------------------------------------------------------------------------
# Discovery / validation
# --------------------------------------------------------------------------
def _array_refs(pf, props, key):
    return [pf.ext_path(i) for i in _ANY_REF_RE.findall(props.get(key, "") or "")]


def read_weapon_pool(campaign_root, dlc_root):
    svc = physics.parse_file(os.path.join(campaign_root, "singletons/item_service.tscn"))
    roots = [n for n in svc.nodes if n["parent"] is None]
    if len(roots) != 1:
        raise ExtractError("item_service.tscn: expected one root node")
    campaign = _array_refs(svc, roots[0]["props"], "weapons")
    dlc_file = os.path.join(dlc_root, "dlcs", "dlc_1", "dlc_data.tres")
    dpf = physics.parse_file(dlc_file)
    dlc = _array_refs(dpf, dpf.root_props, "weapons")
    return campaign, dlc


def _scan_weapon_dirs(root, subdir):
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
            if not match or not pf.ext_path(match.group(1)).endswith("weapon_data.gd"):
                continue
            rel = os.path.relpath(path, root).replace(os.sep, "/")
            found["res://" + rel] = path
    return found


def validate_weapon_dirs(root, subdir, pool, label):
    scan = _scan_weapon_dirs(root, subdir)
    templates, registered = [], {}
    for res_path, abs_path in scan.items():
        props = physics.parse_file(abs_path).root_props
        if get_str(props, "weapon_id", "") == "":
            templates.append(res_path)
        else:
            registered[res_path] = abs_path
    pool_set = set(pool)
    missing = [p for p in pool if p not in registered]
    extra = [p for p in registered if p not in pool_set]
    if missing:
        raise ExtractError("%s: %d weapon resources absent from the tree (e.g. %s)"
                           % (label, len(missing), ", ".join(sorted(missing)[:3])))
    if extra:
        raise ExtractError("%s: %d weapon resources in the tree but not in the "
                           "service array (e.g. %s)"
                           % (label, len(extra), ", ".join(sorted(extra)[:3])))
    return {"resources": len(pool), "templates": sorted(templates)}


def read_attach_table(campaign_root):
    """Player weapon container attachment positions (player.tscn Weapons/*)."""
    path = os.path.join(campaign_root, "entities/units/player/player.tscn")
    pf = physics.parse_file(path)
    names = ("One", "Two", "Three", "Four", "Five", "Six")
    table = [[(0.0, 0.0)] * BT_WEAPON_SLOTS for _ in range(BT_WEAPON_SLOTS + 1)]
    seen = {n: 0 for n in names}
    for node in pf.nodes:
        parent = node["parent"] or ""
        if not parent.startswith("Weapons/"):
            continue
        parts = parent.split("/")
        if len(parts) != 2 or parts[1] not in seen or not node["name"].startswith("Attach"):
            continue
        slot = int(node["name"][len("Attach"):]) - 1
        count = names.index(parts[1]) + 1
        pos = physics.parse_vec2(node["props"].get("position"),
                                 "%s Weapons/%s/%s.position" % (path, parts[1], node["name"]))
        table[count][slot] = pos
        seen[parts[1]] += 1
    for count in range(1, BT_WEAPON_SLOTS + 1):
        if seen[names[count - 1]] != count:
            raise ExtractError("player.tscn: Weapons/%s has %d Attach nodes, expected %d"
                               % (names[count - 1], seen[names[count - 1]], count))
    return table


# --------------------------------------------------------------------------
# Scene geometry
# --------------------------------------------------------------------------
def read_shape(resolver, ref, where):
    if not isinstance(ref, tuple):
        raise ExtractError("%s: shape is not a resource reference (%r)" % (where, ref))
    if ref[0] == "sub":
        _, file_path, sub_id = ref
        parsed = physics.parse_file(file_path)
        block = parsed.subs.get(sub_id)
        if block is None:
            raise ExtractError("%s: missing SubResource( %d )" % (file_path, sub_id))
        props = block["props"]
        if block["type"] == "RectangleShape2D":
            hx, hy = physics.parse_vec2(props.get("extents"), "%s extents" % where,
                                        default=(0.0, 0.0))
            return ("rect", hx, hy, 0.0)
        if block["type"] == "CircleShape2D":
            r = props.get("radius")
            return ("circle", 0.0, 0.0,
                    physics.CIRCLE_DEFAULT_RADIUS if r is None else physics.as_float(r, where))
        raise ExtractError("%s: unsupported shape type %s" % (where, block["type"]))
    parsed = resolver.parse(ref[1])
    if parsed.kind != "resource":
        raise ExtractError("%s: external shape %s is not a shape resource" % (where, ref[1]))
    if "extents" in parsed.root_props:
        hx, hy = physics.parse_vec2(parsed.root_props.get("extents"), "%s extents" % where)
        return ("rect", hx, hy, 0.0)
    r = parsed.root_props.get("radius")
    return ("circle", 0.0, 0.0,
            physics.CIRCLE_DEFAULT_RADIUS if r is None else physics.as_float(r, where))


class WeaponBuilder(object):
    """Weapon metadata builder sharing the canonical item Builder + interner."""

    def __init__(self, builder, campaign_root, dlc_root):
        self.builder = builder
        self.strings = builder.strings
        self.campaign_root = campaign_root
        self.dlc_root = dlc_root
        self.weapons = []
        self.geom = {}
        self.geom_rows = []
        self.sets = {}
        self.set_rows = []
        self.set_tiers = []
        self.set_effects = []
        self.set_ids = []
        self.start = []
        self.start_index = {}
        self.base_index = {}
        self.base_stats_rows = []
        self.base_scaling = []
        self.weapon_eff_index = []
        self.proj_index = {}
        self.proj_rows = []
        self.attach = None
        self.dir_stats = None
        self.audio_durations = []
        self.audio_spans = {}
        self.hit_clip_cache = {}

    # -- audio clips -------------------------------------------------------
    def audio_seq(self, clip_paths):
        """Span into BT_WEAPON_AUDIO_DURATION for one ordered clip sequence.

        Identical ordered sequences (including the same array inherited by many
        tier variants) share one span.  An empty sequence is (0, 0): the source
        guard (``size() > 0``) is false, so no clip exists to index."""
        key = tuple(clip_paths)
        if not key:
            return (0, 0)
        span = self.audio_spans.get(key)
        if span is not None:
            return span
        first = len(self.audio_durations)
        for clip in key:
            self.audio_durations.append({
                "path": clip,
                "seconds": audio_clip_duration(self.builder.R, clip),
            })
        span = (first, len(key))
        self.audio_spans[key] = span
        return span

    def hit_sounds(self, scene_res):
        """The weapon scene's ``custom_hit_sounds`` export (scene-root array).

        Resolved through the flattened scene so an inherited/instanced weapon
        scene contributes its base export and every node override applies."""
        if scene_res in self.hit_clip_cache:
            return self.hit_clip_cache[scene_res]
        clips = []
        if scene_res:
            parsed = self.builder.R.parse(scene_res)
            table = physics.flatten_scene(self.builder.R, scene_res, {})
            raw = (table.get(".") or {}).get("custom_hit_sounds")
            if isinstance(raw, tuple) and raw[0] == "ext":
                clips = [raw[1]]
            elif isinstance(raw, str) and raw:
                clips = [parsed.ext_path(eid) for eid in _ANY_REF_RE.findall(raw)]
        self.hit_clip_cache[scene_res] = clips
        return clips

    # -- geometry ----------------------------------------------------------
    def geometry(self, scene_res):
        if scene_res in self.geom:
            return self.geom[scene_res]
        table = physics.flatten_scene(self.builder.R, scene_res, {})
        root = table.get(".") or {}
        script = root.get("script")
        is_melee = 0
        if isinstance(script, tuple) and script[0] == "ext" \
                and os.path.basename(script[1]) == "melee_weapon.gd":
            is_melee = 1
        shape_kind, hx, hy, radius = "none", 0.0, 0.0, 0.0
        hit_x = hit_y = 0.0
        sprite_x = sprite_y = sprite_off_x = sprite_off_y = 0.0
        muzzle_x = muzzle_y = attach_x = attach_y = range_radius = 0.0
        sprite = table.get("Sprite") or {}
        sprite_x, sprite_y = physics.parse_vec2(sprite.get("position"),
                                                "%s Sprite.position" % scene_res, default=(0.0, 0.0))
        sprite_off_x, sprite_off_y = physics.parse_vec2(sprite.get("offset"),
                                                       "%s Sprite.offset" % scene_res, default=(0.0, 0.0))
        hitbox = table.get("Sprite/Hitbox") or {}
        hbx, hby = physics.parse_vec2(hitbox.get("position"),
                                      "%s Sprite/Hitbox.position" % scene_res, default=(0.0, 0.0))
        coll = table.get("Sprite/Hitbox/Collision")
        if coll is not None:
            cx, cy = physics.parse_vec2(coll.get("position"),
                                        "%s Sprite/Hitbox/Collision.position" % scene_res,
                                        default=(0.0, 0.0))
            shape_kind, hx, hy, radius = read_shape(
                self.builder.R, coll.get("shape"), "%s Sprite/Hitbox/Collision.shape" % scene_res)
            hit_x, hit_y = sprite_x + hbx + cx, sprite_y + hby + cy
        muzzle = table.get("Sprite/Muzzle") or {}
        mx, my = physics.parse_vec2(muzzle.get("position"),
                                    "%s Sprite/Muzzle.position" % scene_res, default=(0.0, 0.0))
        muzzle_x, muzzle_y = sprite_x + mx, sprite_y + my
        # weapon.gd:64 `_attach = $Sprite/Attach`, weapon.gd:166
        # `position = attach_to - _attach.position`: the subtracted offset is the
        # Sprite/Attach node's LOCAL position, so the ROOT-frame sprite offset must
        # NOT be folded in.  The runtime tree only ever contains the base
        # weapon.tscn's `Sprite/Attach` (a derived scene's root-level `Attach` is
        # reported by Godot as "modified from inside an instance, but it has
        # vanished" and is never instantiated), and its position is (0,0) unless
        # the derived scene overrides `Sprite/Attach`.
        attach = table.get("Sprite/Attach")
        if attach is not None:
            attach_x, attach_y = physics.parse_vec2(attach.get("position"),
                                                    "%s Sprite/Attach.position" % scene_res, default=(0.0, 0.0))
        range_node = table.get("Range/CollisionShape2D")
        if range_node is not None:
            _k, _hx, _hy, range_radius = read_shape(
                self.builder.R, range_node.get("shape"),
                "%s Range/CollisionShape2D.shape" % scene_res)
        index = len(self.geom_rows)
        self.geom_rows.append({
            "scene": self.strings.intern(scene_res),
            "shape": {"none": 0, "rect": 1, "circle": 2}[shape_kind],
            "is_melee": is_melee,
            "hit_circle_r": radius, "hit_rect_hx": hx, "hit_rect_hy": hy,
            "hit_x": hit_x, "hit_y": hit_y,
            "sprite_x": sprite_x, "sprite_y": sprite_y,
            "sprite_off_x": sprite_off_x, "sprite_off_y": sprite_off_y,
            "muzzle_x": muzzle_x, "muzzle_y": muzzle_y,
            "attach_x": attach_x, "attach_y": attach_y,
            "range_radius": range_radius,
        })
        self.geom[scene_res] = index
        return index

    # -- base weapon stats (weapon-owned table) ----------------------------
    def base_stats(self, res_path):
        if res_path in self.base_index:
            return self.base_index[res_path]
        pf = self.builder.R.parse(res_path)
        props = pf.root_props
        _, cls = self.builder.script_class(pf, props)
        kind = {"RangedWeaponStats": 2, "MeleeWeaponStats": 1}.get(cls, 0)
        first = len(self.base_scaling)
        for stat_key, ratio in _SCALING_RE.findall(props.get("scaling_stats", "") or ""):
            self.base_scaling.append((self.strings.intern(stat_key), float(ratio)))
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
            "scaling_count": len(self.base_scaling) - first,
            "projectile_scene": 0,
            "projectile_scene_path": "",
        }
        refs = _ANY_REF_RE.findall(props.get("projectile_scene", "") or "")
        if refs:
            target = pf.ext_path(refs[0])
            row["projectile_scene"] = self.strings.intern(target)
            row["projectile_scene_path"] = target
            self.builder.scene(target)
        index = len(self.base_stats_rows)
        self.base_stats_rows.append(row)
        self.base_index[res_path] = index
        return index

    # -- projectile scene spec ---------------------------------------------
    def proj_spec(self, scene_res):
        table = physics.flatten_scene(self.builder.R, scene_res, {})
        root = table.get(".") or {}
        rot = get_float(root, "rotation_speed", 0.0)
        stop = get_float(root, "stop_delay", 0.0)
        delay = get_float(root, "delay", 0.0)
        sidx = self.builder.scene(scene_res)
        hit_r, hit_x, hit_y = self.builder.scene_rows[sidx]["hit"]
        slow_rad = slow_amt = 0.0
        for path in table:
            if path.split("/")[-1] != "SlowHitbox":
                continue
            child = path + "/Collision"
            if child in table:
                _k, _hx, _hy, slow_rad = read_shape(
                    self.builder.R, table[child].get("shape"),
                    "%s %s.shape" % (scene_res, child))
            break
        script = root.get("script")
        if isinstance(script, tuple) and script[0] == "ext" and script[1].endswith(".gd"):
            try:
                text = open(self.builder.R.find(script[1]),
                            encoding="utf-8", errors="replace").read()
            except OSError:
                text = ""
            match = re.search(r"add_decaying_speed\(\s*(-?\s*\d+(?:\.\d+)?)", text)
            if match:
                slow_amt = float(re.sub(r"\s+", "", match.group(1)))
        return {
            "scene": 0 if scene_res == DEFAULT_BULLET_SCENE else self.strings.intern(scene_res),
            "hit_radius": hit_r, "hit_x": hit_x, "hit_y": hit_y,
            "rot_speed": rot, "stop_delay": stop, "delay": delay,
            "slow_rad": slow_rad, "slow_amt": slow_amt, "path": scene_res,
        }

    # -- sets --------------------------------------------------------------
    def set(self, set_res):
        if set_res in self.sets:
            return self.sets[set_res]
        pf = self.builder.R.parse(set_res)
        props = pf.root_props
        tier_first = len(self.set_tiers)
        groups = re.findall(r"\[([^\[\]]*)\]", props.get("set_bonuses", "") or "")
        for group in groups:
            first = len(self.set_effects)
            for eid in _ANY_REF_RE.findall(group):
                self.set_effects.append(self.builder.effect(pf.ext_path(eid), BT_ITEM_NONE))
            self.set_tiers.append((first, len(self.set_effects) - first))
        index = len(self.set_rows)
        self.set_rows.append({
            "my_id": self.strings.intern(get_str(props, "my_id", "")),
            "name": self.strings.intern(get_str(props, "name", "")),
            "tier_first": tier_first,
            "nb_tiers": len(groups),
            "source": set_res,
        })
        self.sets[set_res] = index
        return index

    # -- one weapon --------------------------------------------------------
    def weapon(self, res_path):
        pf = self.builder.R.parse(res_path)
        props = pf.root_props
        script = props.get("script")
        if not script or not physics._EXTREF_RE.match(script) \
                or not pf.ext_path(physics._EXTREF_RE.match(script).group(1)).endswith("weapon_data.gd"):
            raise ExtractError("%s: not a weapon_data resource" % res_path)
        my_id = get_str(props, "my_id", "")
        weapon_id = get_str(props, "weapon_id", "")
        if not my_id.startswith("weapon_") or not weapon_id.startswith("weapon_"):
            raise ExtractError("%s: my_id %r / weapon_id %r are not weapon ids"
                               % (res_path, my_id, weapon_id))
        fx_first = len(self.weapon_eff_index)
        children = [self.builder.effect(pf.ext_path(eid), BT_ITEM_NONE)
                    for eid in _ANY_REF_RE.findall(props.get("effects", "") or "")]
        self.weapon_eff_index.extend(children)

        stats_ref = _ANY_REF_RE.findall(props.get("stats", "") or "")
        if not stats_ref:
            raise ExtractError("%s: weapon has no stats resource" % res_path)
        stats_path = pf.ext_path(stats_ref[0])
        stats = self.base_stats(stats_path)
        stats_parsed = self.builder.R.parse(stats_path)
        stats_props = stats_parsed.root_props
        shoot_refs = _ANY_REF_RE.findall(stats_props.get("shooting_sounds", "") or "")
        shooting_sounds = len(shoot_refs)
        shoot_span = self.audio_seq([stats_parsed.ext_path(eid) for eid in shoot_refs])

        scene_ref = _ANY_REF_RE.findall(props.get("scene", "") or "")
        scene_path = pf.ext_path(scene_ref[0]) if scene_ref else ""
        geom = self.geometry(scene_path) if scene_path else BT_ITEM_NONE
        hit_span = self.audio_seq(self.hit_sounds(scene_path))

        set_paths = [pf.ext_path(sid) for sid in _ANY_REF_RE.findall(props.get("sets", "") or "")]
        set_indices = [self.set(sp) for sp in set_paths]
        curse_factor = get_float(props, "curse_factor", 0.0)
        for slot in children:
            curse_factor = max(
                curse_factor,
                get_float(self.builder.R.parse(self.builder.rows[slot]["source"]).root_props,
                          "curse_factor", 0.0))
        up_refs = _ANY_REF_RE.findall(props.get("upgrades_into", "") or "")
        upgrades_path = pf.ext_path(up_refs[0]) if up_refs else ""
        start_chars = _STR_RE.findall(props.get("add_to_chars_as_starting", "") or "")
        icon_ref = _ANY_REF_RE.findall(props.get("icon", "") or "")
        flags = 0
        if get_bool(props, "unlocked_by_default", False):
            flags |= WFLAG_UNLOCKED_BY_DEFAULT
        if get_bool(props, "is_lockable", True):
            flags |= WFLAG_LOCKABLE
        if children:
            flags |= WFLAG_HAS_EFFECTS
        kind = get_int(props, "type", 0)
        if kind == 0:
            flags |= WFLAG_IS_MELEE
        srow = self.base_stats_rows[stats]
        row = {
            "index": len(self.weapons),
            "id": weapon_id, "my_id": my_id,
            "name": get_str(props, "name", ""),
            "icon": pf.ext_path(icon_ref[0]) if icon_ref else "",
            "weapon_id": weapon_id,
            "stats": stats, "geom": geom,
            "upgrades_path": upgrades_path,
            "upgrades_into": BT_WEAPON_NONE,
            "family": BT_WEAPON_NONE,
            "set_paths": set_paths, "set_indices": set_indices,
            "set_first": 0, "set_count": len(set_indices),
            "tier": get_int(props, "tier", 0), "kind": kind,
            "can_be_looted": 1 if get_bool(props, "can_be_looted", True) else 0,
            "is_cursed": 1 if get_bool(props, "is_cursed", False) else 0,
            "spawns_structure": 1 if weapon_id in STRUCTURE_WEAPON_IDS else 0,
            "flags": flags, "shooting_sounds": shooting_sounds,
            "fx_first": fx_first, "fx_count": len(children),
            "start_chars": start_chars, "start_first": 0, "start_count": len(start_chars),
            "value": get_int(props, "value", 1),
            "curse_factor": curse_factor,
            "base_cooldown": srow["cooldown"], "base_damage": srow["damage"],
            "base_max_range": srow["max_range"], "base_crit_chance": srow["crit_chance"],
            "shoot_audio_first": shoot_span[0], "shoot_audio_count": shoot_span[1],
            "hit_audio_first": hit_span[0], "hit_audio_count": hit_span[1],
            "source": res_path,
        }
        self.weapons.append(row)
        return row["index"]

    # -- post-processing ---------------------------------------------------
    def finish(self, campaign_pool, dlc_pool, dir_stats):
        path_to_index = {row["source"]: row["index"] for row in self.weapons}
        for row in self.weapons:
            if row["upgrades_path"]:
                if row["upgrades_path"] not in path_to_index:
                    raise ExtractError("%s: upgrades_into %s is not a weapon in the pool"
                                       % (row["source"], row["upgrades_path"]))
                row["upgrades_into"] = path_to_index[row["upgrades_path"]]
        family = {}
        for row in self.weapons:
            self.strings.intern(row["weapon_id"])
            prev = family.get(row["weapon_id"])
            if prev is None or (row["tier"], row["index"]) < (prev["tier"], prev["index"]):
                family[row["weapon_id"]] = row
        for row in self.weapons:
            row["family"] = family[row["weapon_id"]]["index"]
        for row in self.weapons:
            row["set_first"] = len(self.set_ids)
            self.set_ids.extend(row["set_indices"])
        for row in self.weapons:
            row["start_first"] = len(self.start)
            for cid in row["start_chars"]:
                if cid not in self.start_index:
                    self.start_index[cid] = self.strings.intern(cid)
                    self.start.append(self.start_index[cid])
        self.attach = read_attach_table(self.campaign_root)
        # Per-projectile-scene runtime spec: the default bullet plus every
        # projectile scene a weapon base stats row or a weapon ProjectilesOnHit
        # stats row points at.
        paths = []
        seen = set()

        def _add(p):
            if p and p not in seen:
                seen.add(p)
                paths.append(p)

        _add(DEFAULT_BULLET_SCENE)
        for row in self.base_stats_rows:
            _add(row["projectile_scene_path"])
        for row in self.builder.rows:
            if row["item"] == BT_ITEM_NONE and row["stats"] != BT_ITEM_NONE:
                sr = self.builder.wstats_rows[row["stats"]]
                if sr["projectile_scene"]:
                    _add(self.strings.values[sr["projectile_scene"]])
        ordered = [DEFAULT_BULLET_SCENE] + sorted(p for p in paths if p != DEFAULT_BULLET_SCENE)
        for i, p in enumerate(ordered):
            self.proj_index[p] = i
            self.proj_rows.append(self.proj_spec(p))
        self.dir_stats = dir_stats


def build_weapons(data, args):
    """Append the weapon graph to the shared item builder; returns wdata (or
    None when the source has no weapon pool, e.g. the synthetic item test)."""
    builder = data["builder"]
    campaign, dlc = read_weapon_pool(args.campaign_root, args.dlc_root)
    if not campaign and not dlc:
        return None
    dir_stats = {
        "campaign": validate_weapon_dirs(args.campaign_root, "weapons", campaign, "campaign weapons"),
        "dlc": validate_weapon_dirs(args.dlc_root, "dlcs/dlc_1/weapons", dlc, "dlc weapons"),
    }
    wb = WeaponBuilder(builder, args.campaign_root, args.dlc_root)
    for path in campaign + dlc:
        wb.weapon(path)
    wb.finish(campaign, dlc, dir_stats)
    seen = set()
    for text in REQUESTED_KEYS:
        if text not in seen:
            seen.add(text)
            wb.strings.intern(text)
    report = {
        "weapons": len(wb.weapons),
        "audio_durations": len(wb.audio_durations),
        "shoot_audio_weapons": sum(1 for r in wb.weapons if r["shoot_audio_count"]),
        "hit_audio_weapons": sum(1 for r in wb.weapons if r["hit_audio_count"]),
        "empty_shoot_audio": sum(1 for r in wb.weapons if not r["shoot_audio_count"]),
        "empty_hit_audio": sum(1 for r in wb.weapons if not r["hit_audio_count"]),
    }
    return {
        "builder": builder, "wb": wb, "weapons": wb.weapons,
        "geom_rows": wb.geom_rows, "set_rows": wb.set_rows,
        "set_tiers": wb.set_tiers, "set_effects": wb.set_effects,
        "set_ids": wb.set_ids, "start": wb.start, "attach": wb.attach,
        "base_stats_rows": wb.base_stats_rows, "base_scaling": wb.base_scaling,
        "weapon_eff_index": wb.weapon_eff_index, "proj_rows": wb.proj_rows,
        "audio_durations": wb.audio_durations, "audio_report": report,
        "dir_stats": dir_stats, "campaign": campaign, "dlc": dlc,
    }


def emit_weapon_types(wdata, out_path):
    lines = []
    lines.append("/* Generated by ocean/brotato/extract_weapons.py -- DO NOT EDIT.")
    lines.append(" *")
    lines.append(" * Weapon metadata types + macros.  Tables live in brotato_weapons.h.")
    lines.append(" * Every value is resolved from the recovered Godot 3.x sources.")
    lines.append(" */")
    lines.append("#pragma once")
    lines.append('#include "brotato_rng.h"   /* BT_TABLE / BT_HOST_ONLY / BT_HD */')
    lines.append("")
    lines.append("#define BT_WEAPON_NONE 0xFFFFu")
    lines.append("#define BT_WEAPON_SLOTS 6")
    lines.append("#define BT_WEAPON_ATTACH_MAX 6")
    lines.append("")
    lines.append("/* WeaponData.Type (items/global/weapon_data.gd). */")
    lines.append("#define BT_WEAPON_KIND_MELEE  0")
    lines.append("#define BT_WEAPON_KIND_RANGED 1")
    lines.append("/* MeleeWeaponStats.AttackType / MeleeAttackType (weapons/melee). */")
    lines.append("#define BT_WEAPON_ATTACK_THRUST 0")
    lines.append("#define BT_WEAPON_ATTACK_SWEEP  1")
    lines.append("/* BtWeaponGeom.shape. */")
    lines.append("#define BT_WEAPON_SHAPE_NONE   0")
    lines.append("#define BT_WEAPON_SHAPE_RECT   1")
    lines.append("#define BT_WEAPON_SHAPE_CIRCLE 2")
    lines.append("")
    lines.append("/* BtWeaponDef.flags bits. */")
    lines.append("#define BT_WFLAG_UNLOCKED_BY_DEFAULT 0x01")
    lines.append("#define BT_WFLAG_LOCKABLE            0x02")
    lines.append("#define BT_WFLAG_HAS_EFFECTS         0x04")
    lines.append("#define BT_WFLAG_IS_MELEE            0x08")
    lines.append("")
    lines.append("/* Global weapon source constants. */")
    lines.append("#define BT_WEAPON_DETECTION_RANGE 200       /* weapon.gd DETECTION_RANGE */")
    lines.append("#define BT_WEAPON_MIN_RANGE       25        /* weapon_service.gd MIN_RANGE */")
    lines.append("#define BT_WEAPON_MIN_COOLDOWN    2         /* weapon_service.gd MIN_COOLDOWN */")
    # (float)(0.9*PI): the engine computes `0.9 * PI` in GDScript binary64 and the
    # Tween narrows it once to the real_t property, so the macro must be the
    # float32 of the double product, written with the 9 significant digits that
    # round-trip a float32 (items.cfloat's %.6g truncated this value by 10 ulp).
    lines.append("#define BT_WEAPON_MELEE_SWEEP_ANGLE %s /* (float)(0.9*PI) "
                 "melee_weapon_shooting_behavior.gd: `0.9 * PI` in GDScript binary64, narrowed "
                 "once to the real_t property/tween value; 9 significant digits so the literal "
                 "round-trips the float32 (the old %%.6g form was 10 float32 ulps low) */"
                 % f32_literal(0.9 * 3.141592653589793))
    lines.append("#define BT_WEAPON_MELEE_MIN_SWEEP_DISTANCE 250")
    lines.append("#define BT_WEAPON_MELEE_BASE_ATK_DURATION %s /* MeleeShootingData.BASE_ATK_DURATION */"
                 % cfloat(0.2))
    lines.append("#define BT_WEAPON_MELEE_SWEEP_HALF_DIVISOR 4")
    lines.append("")
    lines.append("/* Character ids (brotato_characters.h); guarded so both can be included. */")
    lines.append("#ifndef BT_CHARACTER_BULL")
    lines.append("#define BT_CHARACTER_BULL   0")
    lines.append("#define BT_CHARACTER_KNIGHT 1")
    lines.append("#define BT_CHARACTER_DEMON  2")
    lines.append("#define BT_NCHARACTERS      3")
    lines.append("#define BT_CHARACTER_NONE   0xFFFFu")
    lines.append("#endif")
    lines.append("")
    lines.append("typedef struct {")
    for name, ctype, doc in WEAPON_GEOM_FIELDS:
        lines.append("    %-14s %s;%s/* %s */" % (ctype, name, " " * max(1, 16 - len(name)), doc))
    lines.append("} BtWeaponGeom;")
    lines.append("")
    lines.append("typedef struct {")
    for name, ctype, doc in WEAPON_SET_FIELDS:
        lines.append("    %-14s %s;%s/* %s */" % (ctype, name, " " * max(1, 16 - len(name)), doc))
    lines.append("} BtWeaponSet;")
    lines.append("")
    lines.append("typedef struct { unsigned short first, count; } BtWeaponSetTier;")
    lines.append("")
    lines.append("/* Per-projectile-scene runtime metadata (weapon_service.gd spawn path). */")
    lines.append("typedef struct {")
    for name, ctype, doc in WEAPON_PROJ_FIELDS:
        lines.append("    %-14s %s;%s/* %s */" % (ctype, name, " " * max(1, 16 - len(name)), doc))
    lines.append("} BtWeaponProjSpec;")
    lines.append("")
    lines.append("/* Resolved ProjectilesOnHitEffect child (WeaponStats fills at runtime). */")
    lines.append("typedef struct {")
    for name, ctype, doc in WEAPON_ONHIT_FIELDS:
        lines.append("    %-14s %s;%s/* %s */" % (ctype, name, " " * max(1, 16 - len(name)), doc))
    lines.append("} BtWeaponOnHit;")
    lines.append("")
    lines.append("typedef struct {")
    for name, ctype, host_only, doc in WEAPON_DEF_FIELDS:
        if host_only:
            lines.append("    BT_HOST_ONLY(%s %s;)   /* %s */" % (ctype, name, doc))
        else:
            lines.append("    %-14s %s;%s/* %s */" % (ctype, name, " " * max(1, 16 - len(name)), doc))
    lines.append("} BtWeaponDef;")
    lines.append("")
    lines.append("/* WeaponStats property-name aliases (same ids as BT_ISTR_*). */")
    for key in WSTAT_ALIASES:
        lines.append("#define BT_ISTR_WSTAT_%s BT_ISTR_%s" % (sanitize(key), sanitize(key)))
    lines.append("")
    return items._write("\n".join(lines), out_path)


def _emit_wstats_row(lines, row):
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


def emit_weapons(wdata, out_path):
    strings = wdata["builder"].strings
    weapons = wdata["weapons"]
    base = wdata["base_stats_rows"]
    scaling = wdata["base_scaling"]
    proj = wdata["proj_rows"]
    lines = []
    lines.append("/* Generated by ocean/brotato/extract_weapons.py -- DO NOT EDIT.")
    lines.append(" *")
    lines.append(" * Complete Brotato weapon pool (campaign + Abyss DLC), one BtWeaponDef")
    lines.append(" * per tier variant, plus the weapon-scoped effect index and the melee /")
    lines.append(" * ranged scene geometry.  Every value is resolved from the recovered")
    lines.append(" * Godot 3.x sources; nothing is invented.")
    lines.append(" *")
    lines.append(" * Effect rows live in the ONE canonical effect namespace of")
    lines.append(" * brotato_item_effects.h: item rows first (ids unchanged), then the weapon")
    lines.append(" * rows (BtItemEff.item == BT_ITEM_NONE).  BtWeaponDef.fx_first/fx_count")
    lines.append(" * index BT_WEAPON_EFF_INDEX; nested child/sub/stats/burning refs are")
    lines.append(" * canonical BT_ITEM_* indices.")
    lines.append(" *")
    lines.append(" * Pool: %d campaign + %d DLC weapons = %d tier variants."
                 % (len(wdata["campaign"]), len(wdata["dlc"]), len(weapons)))
    tmpl = wdata["dir_stats"]["campaign"]["templates"] + wdata["dir_stats"]["dlc"]["templates"]
    lines.append(" * Unregistered template resources without a weapon_id: %s."
                 % (", ".join(tmpl) if tmpl else "none"))
    lines.append(" */")
    lines.append("#pragma once")
    lines.append('#include "brotato_weapon_types.h"')
    lines.append('#include "brotato_item_effects.h"   /* BtItemEff / BtItemWeaponStats / BT_ISTR_* */')
    lines.append("")
    lines.append("#define BT_WEAPON_NONE 0xFFFFu")
    lines.append("#define BT_WEAPON_SLOTS 6")
    lines.append("")
    lines.append("/* WeaponStats.shooting_sounds length: the RNG bounded-draw size for the")
    lines.append(" * per-shot random shooting sound (0 => no draw).  The same clips'")
    lines.append(" * lengths are BT_WEAPON_AUDIO_DURATION[shoot_audio_first..+count). */")
    lines.append("#define BT_WEAPON_SOUND_COUNT(W) ((W)->shooting_sounds)")
    lines.append("")
    lines.append("#define BT_NWEAPON_SCALING %d" % max(1, len(scaling)))
    lines.append("BT_TABLE BtItemScaling BT_WEAPON_SCALING[BT_NWEAPON_SCALING] = {")
    for stat, ratio in scaling:
        lines.append("    { %d, %s },   /* %s */" % (stat, cfloat(ratio), strings.values[stat]))
    lines.append("};")
    lines.append("")
    lines.append("#define BT_NWEAPON_STATS %d" % max(1, len(base)))
    lines.append("/* Weapon base WeaponStats rows (UNCURSED).  Denormalized copies of")
    lines.append(" * cooldown/damage/max_range/crit_chance also live in BT_WEAPONS. */")
    lines.append("BT_TABLE BtItemWeaponStats BT_WEAPON_STATS[BT_NWEAPON_STATS] = {")
    for row in base:
        _emit_wstats_row(lines, row)
    if not base:
        lines.append("    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,")
        lines.append("      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,")
        lines.append("      0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },")
    lines.append("};")
    lines.append("")
    audio = wdata["audio_durations"]
    lines.append("#define BT_NWEAPON_AUDIO_DURATION %d" % max(1, len(audio)))
    lines.append("/* True clip lengths in seconds, read from the real source audio bytes")
    lines.append(" * (RIFF/WAVE data size, MPEG frame scan / Ogg granule).  A weapon's")
    lines.append(" * BtWeaponDef shoot_audio_first/count and hit_audio_first/count span")
    lines.append(" * this table; identical ordered clip sequences share one span and the")
    lines.append(" * count is 0 when the source array is empty (that pick_random is")
    lines.append(" * skipped entirely: utils.gd:391-394 returns null before any draw).")
    lines.append(" * The runtime advances each playing clip's lifetime by")
    lines.append(" * duration/pitch_scale, matching SoundManagerParent._process. */")
    lines.append("BT_TABLE float BT_WEAPON_AUDIO_DURATION[BT_NWEAPON_AUDIO_DURATION] = {")
    for row in audio:
        lines.append("    %s,   /* %s */" % (_audio_float(row["seconds"]), row["path"]))
    if not audio:
        lines.append("    0.0f,")
    lines.append("};")
    lines.append("")
    lines.append("#define BT_NWEAPON_EFF_INDEX %d" % max(1, len(wdata["weapon_eff_index"])))
    lines.append("/* Canonical BT_ITEM_EFF_ROWS indices, one contiguous range per weapon")
    lines.append(" * (BtWeaponDef.fx_first/fx_count). */")
    lines.append("BT_TABLE unsigned short BT_WEAPON_EFF_INDEX[%d] = {"
                 % max(1, len(wdata["weapon_eff_index"])))
    items._emit_ushort_table(lines, wdata["weapon_eff_index"])
    lines.append("};")
    lines.append("")
    lines.append("#define BT_NWEAPON_PROJ_SPEC %d" % max(1, len(proj)))
    lines.append("#define BT_WEAPON_PROJ_DEFAULT 0")
    lines.append("/* Per-projectile-scene runtime metadata.  Row BT_WEAPON_PROJ_DEFAULT")
    lines.append(" * (scene == BT_ISTR_NONE) is res://projectiles/bullet/bullet_projectile.tscn,")
    lines.append(" * the WeaponService.DEFAULT_PROJECTILE_SCENE fallback.  `delay` is 0 unless")
    lines.append(" * the scene overrides it (DelayedPlayerProjectile.delay is otherwise a")
    lines.append(" * runtime rand_range(0, 3) the RNG owner must draw). */")
    lines.append("BT_TABLE BtWeaponProjSpec BT_WEAPON_PROJ_SPEC[BT_NWEAPON_PROJ_SPEC] = {")
    for row in proj:
        lines.append("    { %d, %s, %s, %s, %s, %s, %s, %s, %s },   /* %s */"
                     % (row["scene"], cfloat(row["hit_radius"]), cfloat(row["hit_x"]),
                        cfloat(row["hit_y"]), cfloat(row["rot_speed"]),
                        cfloat(row["stop_delay"]), cfloat(row["delay"]),
                        cfloat(row["slow_rad"]), cfloat(row["slow_amt"]), row["path"]))
    lines.append("};")
    lines.append("")
    lines.append("#define BT_NWEAPON_GEOM %d" % max(1, len(wdata["geom_rows"])))
    lines.append("BT_TABLE BtWeaponGeom BT_WEAPON_GEOM[%d] = {" % max(1, len(wdata["geom_rows"])))
    for row in wdata["geom_rows"]:
        lines.append("    { %d, %d, %d, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s },"
                     % (row["scene"], row["shape"], row["is_melee"],
                        cfloat(row["hit_circle_r"]), cfloat(row["hit_rect_hx"]),
                        cfloat(row["hit_rect_hy"]), cfloat(row["hit_x"]), cfloat(row["hit_y"]),
                        cfloat(row["sprite_x"]), cfloat(row["sprite_y"]),
                        cfloat(row["sprite_off_x"]), cfloat(row["sprite_off_y"]),
                        cfloat(row["muzzle_x"]), cfloat(row["muzzle_y"]),
                        cfloat(row["attach_x"]), cfloat(row["attach_y"]),
                        cfloat(row["range_radius"])))
    if not wdata["geom_rows"]:
        lines.append("    { 0, 0, 0, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,"
                     " 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },")
    lines.append("};")
    lines.append("")
    lines.append("#define BT_NWEAPON_SETS %d" % max(1, len(wdata["set_rows"])))
    lines.append("BT_TABLE BtWeaponSet BT_WEAPON_SETS[%d] = {" % max(1, len(wdata["set_rows"])))
    for row in wdata["set_rows"]:
        lines.append("    { %d, %d, %d, %d },   /* %s */"
                     % (row["my_id"], row["name"], row["tier_first"], row["nb_tiers"], row["source"]))
    if not wdata["set_rows"]:
        lines.append("    { 0, 0, 0, 0 },")
    lines.append("};")
    lines.append("")
    lines.append("#define BT_NWEAPON_SET_TIERS %d" % max(1, len(wdata["set_tiers"])))
    lines.append("BT_TABLE BtWeaponSetTier BT_WEAPON_SET_TIERS[%d] = {" % max(1, len(wdata["set_tiers"])))
    for first, count in wdata["set_tiers"]:
        lines.append("    { %d, %d }," % (first, count))
    if not wdata["set_tiers"]:
        lines.append("    { 0, 0 },")
    lines.append("};")
    lines.append("")
    lines.append("#define BT_NWEAPON_SET_EFFECTS %d" % max(1, len(wdata["set_effects"])))
    lines.append("BT_TABLE unsigned short BT_WEAPON_SET_EFFECTS[%d] = {"
                 % max(1, len(wdata["set_effects"])))
    items._emit_ushort_table(lines, wdata["set_effects"])
    lines.append("};")
    lines.append("")
    lines.append("#define BT_NWEAPON_SET_IDS %d" % max(1, len(wdata["set_ids"])))
    lines.append("BT_TABLE unsigned short BT_WEAPON_SET_IDS[%d] = {" % max(1, len(wdata["set_ids"])))
    items._emit_ushort_table(lines, wdata["set_ids"])
    lines.append("};")
    lines.append("")
    lines.append("#define BT_NWEAPON_START %d" % max(1, len(wdata["start"])))
    lines.append("BT_TABLE unsigned short BT_WEAPON_START[%d] = {" % max(1, len(wdata["start"])))
    items._emit_ushort_table(lines, wdata["start"])
    lines.append("};")
    lines.append("")
    lines.append("/* Player weapon-container attachment positions (player.tscn")
    lines.append(" * Weapons/<One..Six>/AttachN); BT_WEAPON_ATTACH_X[count][slot]. */")
    lines.append("BT_TABLE float BT_WEAPON_ATTACH_X[7][6] = {")
    for count in range(0, BT_WEAPON_SLOTS + 1):
        xs = [wdata["attach"][count][s][0] for s in range(BT_WEAPON_SLOTS)]
        lines.append("    { %s }," % ", ".join(cfloat(v) for v in xs))
    lines.append("};")
    lines.append("BT_TABLE float BT_WEAPON_ATTACH_Y[7][6] = {")
    for count in range(0, BT_WEAPON_SLOTS + 1):
        ys = [wdata["attach"][count][s][1] for s in range(BT_WEAPON_SLOTS)]
        lines.append("    { %s }," % ", ".join(cfloat(v) for v in ys))
    lines.append("};")
    lines.append("")
    symbols = set()
    for row in weapons:
        symbol = "BT_WEAPON_IDX_" + re.sub(r"[^A-Za-z0-9_]", "_", row["my_id"]).upper()
        if symbol in symbols:
            raise ExtractError("weapon identity macro collision: %s" % symbol)
        symbols.add(symbol)
        lines.append("#define %s %d" % (symbol, row["index"]))
    lines.append("")
    lines.append("#define BT_NWEAPONS %d" % len(weapons))
    lines.append("/* Canonical source bound: the maximum number of top-level effect rows any")
    lines.append(" * weapon carries (WeaponInventory draws one curse modifier per top-level")
    lines.append(" * effect, dlc_1_data.gd:82). */")
    lines.append("#define BT_WEAPON_FX_CURSE_MAX %d" % max(0, max(
        (row["fx_count"] for row in weapons), default=0)))
    lines.append("BT_TABLE BtWeaponDef BT_WEAPONS[BT_NWEAPONS] = {")
    for row in weapons:
        lines.append("    /* [%d] %s <- %s */" % (row["index"], row["my_id"], row["source"]))
        lines.append('    { BT_HOST_ONLY("%s", "%s", "%s", "%s",)'
                     % (row["id"], row["my_id"], _cstr(row["name"]), _cstr(row["icon"])))
        lines.append("      %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d,"
                     % (strings.ids[row["weapon_id"]], row["stats"], row["geom"],
                        row["upgrades_into"], row["family"], row["set_first"],
                        row["set_count"], row["tier"], row["kind"], row["can_be_looted"],
                        row["is_cursed"], row["spawns_structure"], row["flags"],
                        row["shooting_sounds"]))
        lines.append("      %d, %d, %d, %d, %d, %s, %d, %d, %d, %s,"
                     % (row["fx_first"], row["fx_count"], row["start_first"],
                        row["start_count"], row["value"], cfloat(row["curse_factor"]),
                        row["base_cooldown"], row["base_damage"],
                        row["base_max_range"], cfloat(row["base_crit_chance"])))
        lines.append("      %d, %d, %d, %d },"
                     % (row["shoot_audio_first"], row["shoot_audio_count"],
                        row["hit_audio_first"], row["hit_audio_count"]))
    lines.append("};")
    lines.append("")
    lines.append("/* Accessors (device + host). */")
    lines.append("BT_HD static inline int bt_weapon_effect_row(unsigned short weapon, unsigned short ordinal) {")
    lines.append("    if (weapon >= BT_NWEAPONS || ordinal >= BT_WEAPONS[weapon].fx_count)")
    lines.append("        return BT_WEAPON_NONE;")
    lines.append("    return BT_WEAPON_EFF_INDEX[BT_WEAPONS[weapon].fx_first + ordinal];")
    lines.append("}")
    lines.append("BT_HD static inline BT_CONST_PTR(BtWeaponProjSpec) bt_weapon_proj_spec(unsigned short scene) {")
    lines.append("    if (scene == BT_ISTR_NONE) return &BT_WEAPON_PROJ_SPEC[BT_WEAPON_PROJ_DEFAULT];")
    lines.append("    for (int i = 0; i < BT_NWEAPON_PROJ_SPEC; i++)")
    lines.append("        if (BT_WEAPON_PROJ_SPEC[i].scene == scene) return &BT_WEAPON_PROJ_SPEC[i];")
    lines.append("    return &BT_WEAPON_PROJ_SPEC[BT_WEAPON_PROJ_DEFAULT];")
    lines.append("}")
    lines.append("")
    lines.append("/* Host-side canonical id resolvers. */")
    lines.append("BT_HOST_ONLY(static inline int bt_weapon_str_eq(const char* a, const char* b) {")
    lines.append("    if (!a || !b) return 0;")
    lines.append("    while (*a && *a == *b) { a++; b++; }")
    lines.append("    return *a == *b;")
    lines.append("})")
    lines.append("BT_HOST_ONLY(BT_HD static inline int bt_weapon_index_from_id(const char* name) {")
    lines.append("    if (!name || name[0] == '\\0') return BT_WEAPON_NONE;")
    lines.append("    if (bt_weapon_str_eq(name, \"none\")) return BT_WEAPON_NONE;")
    lines.append("    for (int i = 0; i < BT_NWEAPONS; i++)")
    lines.append("        if (bt_weapon_str_eq(name, BT_WEAPONS[i].my_id)) return i;")
    lines.append("    for (int i = 0; i < BT_NWEAPONS; i++)")
    lines.append("        if (bt_weapon_str_eq(name, BT_WEAPONS[i].id)) return i;")
    lines.append("    return BT_WEAPON_NONE;")
    lines.append("})")
    lines.append("BT_HOST_ONLY(BT_HD static inline unsigned short bt_weapon_id_from_name(const char* name) {")
    lines.append("    return (unsigned short)bt_weapon_index_from_id(name);")
    lines.append("})")
    lines.append("")
    lines.append("#ifndef __METAL_VERSION__")
    lines.append("#ifdef __cplusplus")
    lines.append("#define BT_WEAPON_ASSERT(c, m) static_assert(c, m)")
    lines.append("#else")
    lines.append("#define BT_WEAPON_ASSERT(c, m) _Static_assert(c, m)")
    lines.append("#endif")
    lines.append('BT_WEAPON_ASSERT(BT_NWEAPONS <= 65535, "weapon index must fit unsigned short");')
    lines.append('BT_WEAPON_ASSERT(BT_NWEAPON_STATS <= 65535, "weapon stats must fit unsigned short");')
    lines.append("#undef BT_WEAPON_ASSERT")
    lines.append("#endif")
    lines.append("")
    return items._write("\n".join(lines), out_path)


def _cstr(text):
    return text.replace("\\", "\\\\").replace('"', '\\"')


def emit_weapons_contract(wdata, out_path):
    lines = []
    lines.append("# Brotato weapon data contract")
    lines.append("")
    lines.append("Generated by `ocean/brotato/extract_weapons.py`.  Companion headers:")
    lines.append("`brotato_weapon_types.h` and `brotato_weapons.h`.")
    lines.append("")
    lines.append("| table | rows |")
    lines.append("|---|---|")
    lines.append("| weapons `BT_WEAPONS` | %d |" % len(wdata["weapons"]))
    lines.append("| weapon stats `BT_WEAPON_STATS` | %d |" % len(wdata["base_stats_rows"]))
    lines.append("| weapon scaling `BT_WEAPON_SCALING` | %d |" % len(wdata["base_scaling"]))
    lines.append("| weapon effect index `BT_WEAPON_EFF_INDEX` | %d |" % len(wdata["weapon_eff_index"]))
    lines.append("| weapon geometry `BT_WEAPON_GEOM` | %d |" % len(wdata["geom_rows"]))
    lines.append("| weapon sets `BT_WEAPON_SETS` | %d |" % len(wdata["set_rows"]))
    lines.append("| weapon audio durations `BT_WEAPON_AUDIO_DURATION` | %d |"
                 % len(wdata["audio_durations"]))
    lines.append("")
    lines.append("| `BtWeaponDef` field | type | semantics |")
    lines.append("|---|---|---|")
    for name, ctype, _host, doc in WEAPON_DEF_FIELDS:
        lines.append("| `%s` | `%s` | %s |" % (name, ctype, doc))
    lines.append("")
    lines.append("| `BtWeaponGeom` field | type | semantics |")
    lines.append("|---|---|---|")
    for name, ctype, doc in WEAPON_GEOM_FIELDS:
        lines.append("| `%s` | `%s` | %s |" % (name, ctype, doc))
    lines.append("")
    return items._write("\n".join(lines), out_path)


def emit_weapon_provenance(wdata, out_path, args):
    strings = wdata["builder"].strings
    weapons = []
    for row in wdata["weapons"]:
        effects = []
        for slot in wdata["weapon_eff_index"][row["fx_first"]:row["fx_first"] + row["fx_count"]]:
            eff = wdata["builder"].rows[slot]
            effects.append({"row": slot, "class": eff["opcode"], "source": eff["source"]})
        weapons.append({
            "index": row["index"], "id": row["id"], "my_id": row["my_id"],
            "source": row["source"], "file": wdata["builder"].R.find(row["source"]),
            "weapon_id": row["weapon_id"], "tier": row["tier"], "kind": row["kind"],
            "value": row["value"], "stats": row["stats"], "geom": row["geom"],
            "upgrades_into": row["upgrades_into"], "family": row["family"],
            "sets": [wdata["wb"].set_rows[i]["source"] for i in row["set_indices"]],
            "start_chars": row["start_chars"],
            "fx_first": row["fx_first"], "fx_count": row["fx_count"],
            "effects": effects,
            "shoot_audio_first": row["shoot_audio_first"],
            "shoot_audio_count": row["shoot_audio_count"],
            "hit_audio_first": row["hit_audio_first"],
            "hit_audio_count": row["hit_audio_count"],
        })
    payload = {
        "generator": "ocean/brotato/extract_weapons.py",
        "campaign_root": args.campaign_root,
        "dlc_root": args.dlc_root,
        "counts": {
            "weapons": len(weapons),
            "campaign": len(wdata["campaign"]), "dlc": len(wdata["dlc"]),
            "base_stats": len(wdata["base_stats_rows"]),
            "scaling": len(wdata["base_scaling"]),
            "geom": len(wdata["geom_rows"]),
            "sets": len(wdata["set_rows"]),
            "effect_index": len(wdata["weapon_eff_index"]),
            "audio_durations": len(wdata["audio_durations"]),
            "shoot_audio_weapons": wdata["audio_report"]["shoot_audio_weapons"],
            "hit_audio_weapons": wdata["audio_report"]["hit_audio_weapons"],
            "interned_strings": len(strings.values),
        },
        "templates": wdata["dir_stats"]["campaign"]["templates"]
        + wdata["dir_stats"]["dlc"]["templates"],
        "audio_durations": [
            {"row": i, "path": row["path"], "seconds": row["seconds"]}
            for i, row in enumerate(wdata["audio_durations"])
        ],
        "weapons": weapons,
    }
    return items._write(json.dumps(payload, indent=1, sort_keys=False) + "\n", out_path)


# --------------------------------------------------------------------------
# Standalone entry point (writes every header via extract_items.emit_all)
# --------------------------------------------------------------------------
def main(argv):
    parser = argparse.ArgumentParser(description="Extract the Brotato weapon pool.")
    parser.add_argument("--campaign-root", required=True)
    parser.add_argument("--dlc-root", required=True)
    parser.add_argument("--items-out", default="ocean/brotato/brotato_items.h")
    parser.add_argument("--effects-out", default="ocean/brotato/brotato_item_effects.h")
    parser.add_argument("--weapons-out", default="ocean/brotato/brotato_weapons.h")
    parser.add_argument("--weapon-types-out", default="ocean/brotato/brotato_weapon_types.h")
    parser.add_argument("--weapon-contract-out", default="ocean/brotato/brotato_weapons_contract.md")
    parser.add_argument("--contract-out", default="ocean/brotato/brotato_item_effects_contract.md")
    parser.add_argument("--provenance-out", required=True)
    parser.add_argument("--weapon-provenance-out", default="")
    args = parser.parse_args(argv)
    try:
        data = items.emit_all(args)
    except ExtractError as exc:
        sys.stderr.write("extract_weapons: error: %s\n" % exc)
        return 1
    wdata = data.get("weapons")
    if wdata:
        sys.stderr.write("extract_weapons: %d weapon rows, %d base stats, %d effect idx, "
                         "%d geometry, %d sets -> %s, %s\n"
                         % (len(wdata["weapons"]), len(wdata["base_stats_rows"]),
                            len(wdata["weapon_eff_index"]), len(wdata["geom_rows"]),
                            len(wdata["set_rows"]), args.weapons_out, args.weapon_types_out))
        report = wdata["audio_report"]
        sys.stderr.write(
            "extract_weapons: audio %d clips; shooting_sounds non-empty on %d/%d weapons "
            "(shoot_audio span + one pick_random draw; empty is unreachable so "
            "SoundManager.play(null) cannot happen), custom_hit_sounds non-empty on %d/%d "
            "(empty => the size() > 0 guard skips SoundManager2D.play entirely, no draw)\n"
            % (report["audio_durations"], report["shoot_audio_weapons"], report["weapons"],
               report["hit_audio_weapons"], report["weapons"]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
