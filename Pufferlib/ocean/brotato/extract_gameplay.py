#!/usr/bin/env python3
"""Extract Brotato wave spawn groups, unit kinds, stats and attack behavior
into the portable headers ``brotato_waves.h`` and ``brotato_attacks.h``.

Ground truth is the recovered Godot 3.x project (campaign root
``brotato_recovered`` + ``brotato_abyss`` DLC root).  Every emitted row is
derived from the real ``.tres`` resources and ``.tscn`` scenes -- nothing is
hand-authored and no value is invented.  The generator is fully deterministic
(no RNG, stable sorting): rerunning it reproduces byte-identical headers.

What is resolved from source
----------------------------
* Zone definitions (``zone_1_data.tres`` / ``zone_abyss_data.tres``): per-wave
  resources, global all-wave groups, horde groups, loot groups, and the DLC
  ``groups_in_all_zones``.
* Wave resources (``wave_N.tres``): duration, ``max_enemies`` and the group
  list (including *every* ``min_difficulty`` row -- D1/D2/D4 groups are the D5
  spawn streams and must not be dropped).
* Group resources (``wave_group_data.gd``): timing, chance, repeat count and
  interval reduction, area / edge placement, horde/boss/loot/neutral flags,
  ``min_difficulty`` and the ``min_wave``/``max_wave`` window.
* Unit resources (``wave_unit_data.gd``): resolved unit scene, ``type``,
  min/max count, spawn chance, and ``additional_min_distance_from_player``
  (extra distance added to the 300px spawn ring around the player).
* Item-granted extra groups: every ``ExtraEnemyEffect``
  (``effects/items/extra_enemy_effect.gd``) references a wave-group resource
  through ``extra_group_data`` (RunData keys them by ``resource_path``).
  Those groups are outside any zone, so they are extracted as a canonical
  tail block of ``BT_GROUPS`` (one copy per zone, like the DLC
  ``groups_in_all_zones``) and exposed as ``BT_ITEM_EXTRA_GROUPS`` -- the
  ordinal ``BtItemEff.extra_group`` holds -- with every referenced unit.
* Enemy ``stats.gd`` resources: health / health growth, speed (+ absolute
  ``speed_randomization``), damage / growth, attack cooldown, material value,
  gold spread, knockback resistance, consumable drop chances and tier bounds,
  armor / growth.
* Elite and boss registries (``elites``/``bosses`` data resources, in the
  SOURCE ARRAY order -- the campaign ``ItemService`` export followed by the DLC
  ``dlc_data`` array -- filtered by ``zone_id``) as *explicit index lists* so
  bosses can never be selected as elites.
* Elite/horde schedule constants from ``run_data.gd:init_elites_spawn``.
* Attack behaviors from each unit scene (``AttackBehavior`` node plus the
  ``States/StateN`` escalation states) -- behavior kind, projectile scene and
  its *real* collision shape (circle/rect), projectile count/speed/spread,
  charge duration/speed/target/timing, cooldown and initial cooldown, and
  spawner ``enemy_to_spawn`` references.
* The ordered GLOBAL-RNG draws a fresh enemy node makes that a pooled respawn
  does not (and vice versa): the ``Enemy._idle_playback_speed`` member
  initializer, every movement/attack behavior node's ``_ready`` (in Godot's
  final child order), the ``$MovementBehavior`` ``init()`` chain and the
  ``Unit.init`` speed jitter, versus ``Unit.respawn``'s jitter and the root
  ``AttackBehavior.reset()``.  Each row carries the real ``rand_range`` bounds
  and the base the source adds after the draw as raw IEEE-754 binary64 words
  (the pinned engine draws in FP64; MSL has no ``double``), an ADD flag for
  whether the base is applied at all, the phase (fresh/reuse/both) and the
  semantic destination member.

Anything the generator cannot prove from the sources raises ``ExtractError``
instead of silently emitting a default.

Usage::

    python3 ocean/brotato/extract_gameplay.py \\
        --campaign-root /Users/me/brotato_recovered \\
        --dlc-root      /Users/me/brotato_abyss \\
        --waves-out     ocean/brotato/brotato_waves.h \\
        --attacks-out   ocean/brotato/brotato_attacks.h
"""

import argparse
import math
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import extract_physics as physics  # noqa: E402  (reuse the .tscn/.tres parser)

ExtractError = physics.ExtractError
Refs = physics._EXTREF_RE  # noqa: F841  (kept for readability)

_ANY_REF_RE = re.compile(r"ExtResource\(\s*(\d+)\s*\)")
_INNER_ARRAY_RE = re.compile(r"\[([^\[\]]*)\]")

# WaveUnitData.Type {PLAYER, ENEMY, NEUTRAL, STRUCTURE, BOSS, PET}
UNIT_TYPE_ENEMY = 1
UNIT_TYPE_NEUTRAL = 2
UNIT_TYPE_BOSS = 4

# BtSpawnGroup.is_* / schedule EliteType enum (run_data.gd)
ELITE_TYPE_ELITE = 0
ELITE_TYPE_HORDE = 1

# Attack behavior codes
BEH_NONE = 0
BEH_SHOOT = 1
BEH_CHARGE = 2
BEH_SPAWN = 3
BEH_UNKNOWN = 254

_BEH_BY_SCRIPT = {
    "attack_behavior": BEH_NONE,
    "shooting_attack_behavior": BEH_SHOOT,
    "charging_attack_behavior": BEH_CHARGE,
    "spawning_attack_behavior": BEH_SPAWN,
}

# Projectile shape codes
SHAPE_BULLET = 0
SHAPE_PILLAR = 1
SHAPE_SLASH = 2
SHAPE_OTHER = 3
# slash_projectile_2.tscn shares slash_projectile.tscn's shoot-animation
# keyframes but puts its Hitbox node at (165,-15) instead of (0,-15), so the
# sweep origin differs and the sim carries a separate shape code.
SHAPE_SLASH2 = 4


# --------------------------------------------------------------------------
# Property helpers
# --------------------------------------------------------------------------
def _refs(parsed, value):
    """All ExtResource paths referenced by a property value."""
    return [parsed.ext_path(i) for i in _ANY_REF_RE.findall(value or "")]


def _one_ref(parsed, value, where):
    hits = _refs(parsed, value)
    if len(hits) != 1:
        raise ExtractError("%s: expected exactly one resource ref, got %r"
                           % (where, value))
    return hits[0]


def get_str(props, key, default=None):
    value = props.get(key)
    if value is None:
        return default
    value = value.strip()
    if value.startswith('"') and value.endswith('"'):
        return value[1:-1]
    return value


def get_int(props, key, default):
    value = props.get(key)
    return default if value is None else int(float(value))


def get_float(props, key, default):
    value = props.get(key)
    return default if value is None else float(value)


def get_bool(props, key, default):
    value = props.get(key)
    if value is None:
        return default
    return value.strip() == "true"


# --------------------------------------------------------------------------
# stats.gd resource
# --------------------------------------------------------------------------
# Defaults mirror the `export` initializers in entities/units/unit/stats.gd.
STATS_DEFAULTS = {
    "health": 1,
    "health_increase_each_wave": 1.0,
    "speed": 300,
    "speed_randomization": 0,
    "damage": 1,
    "damage_increase_each_wave": 1.0,
    "attack_cd": 30.0,
    "value": 1,
    "knockback_resistance": 0.0,
    "gold_spread": 0,
    "can_drop_consumables": True,
    "always_drop_consumables": False,
    "base_drop_chance": 0.01,
    "item_drop_chance": 0.01,
    "min_consumable_tier": 0,
    "max_consumable_tier": 0,
    "armor": 0,
    "armor_increase_each_wave": 0.0,
    "generate_blood": True,
}


def parse_stats(resolver, ref, where):
    if not isinstance(ref, tuple):
        raise ExtractError("%s: 'stats' is not a resource reference (%r)"
                           % (where, ref))
    if ref[0] == "sub":
        _, file_path, sub_id = ref
        parsed = physics.parse_file(file_path)
        block = parsed.subs.get(sub_id)
        if block is None:
            raise ExtractError("%s: missing stats SubResource( %d )"
                               % (file_path, sub_id))
        props = block["props"]
    else:
        parsed = resolver.parse(ref[1])
        if parsed.kind != "resource":
            raise ExtractError("%s: stats %s is not a gd_resource"
                               % (where, ref[1]))
        props = parsed.root_props

    out = {}
    for key, default in STATS_DEFAULTS.items():
        if isinstance(default, bool):
            out[key] = get_bool(props, key, default)
        elif isinstance(default, int):
            out[key] = get_int(props, key, default)
        else:
            out[key] = get_float(props, key, default)
    out["_src"] = parsed.path if ref[0] != "sub" else ref[1]
    return out


# --------------------------------------------------------------------------
# Attack behavior extraction
# --------------------------------------------------------------------------
def behavior_code(script):
    if not script:
        return None
    m = re.search(r"attack_behaviors/([a-z_]+)\.gd", str(script))
    if not m:
        return BEH_UNKNOWN
    return _BEH_BY_SCRIPT.get(m.group(1), BEH_UNKNOWN)


def projectile_shape(resolver, res_path, where):
    """Return ``(code, shape_type, radius, half_x, half_y)`` for a projectile."""
    table = physics.flatten_scene(resolver, res_path, {})
    shape_ref = None
    for key in ("Hitbox/Collision", "Collision", "Hurtbox/Collision"):
        node = table.get(key)
        if node is not None and "shape" in node:
            shape_ref = node["shape"]
            break
    if shape_ref is None:
        raise ExtractError("%s: no collision shape found in %s" % (where, res_path))

    if not isinstance(shape_ref, tuple):
        raise ExtractError("%s: shape is not a resource reference (%r)"
                           % (where, shape_ref))
    if shape_ref[0] == "sub":
        _, file_path, sub_id = shape_ref
        parsed = physics.parse_file(file_path)
        block = parsed.subs.get(sub_id)
        if block is None:
            raise ExtractError("%s: missing shape SubResource( %d )" % (file_path, sub_id))
        stype = block["type"]
        props = block["props"]
    else:
        parsed = resolver.parse(shape_ref[1])
        stype = None
        props = {}
        if parsed.kind == "resource":
            props = parsed.root_props
            stype = "Resource"
        else:
            sub = physics.flatten_scene(resolver, shape_ref[1], {})
            # external shapes are plain resources in practice
            raise ExtractError("%s: external shape %s is not a gd_resource"
                               % (where, shape_ref[1]))

    radius = half_x = half_y = 0.0
    if stype == "CircleShape2D":
        radius = get_float(props, "radius", 10.0)
    elif stype == "RectangleShape2D":
        ext = props.get("extents", "Vector2( 0, 0 )")
        m = physics._VEC2_RE.match(ext.strip())
        if not m:
            raise ExtractError("%s: bad RectangleShape2D extents %r" % (where, ext))
        half_x, half_y = float(m.group(1)), float(m.group(2))
        radius = max(half_x, half_y)
    elif stype == "CapsuleShape2D":
        radius = get_float(props, "radius", 10.0)
        half_y = get_float(props, "height", 20.0) * 0.5
    else:
        # unknown / polygon shapes: keep the scene, mark as "other"
        pass

    stem = os.path.basename(res_path)
    if "pillar" in stem:
        code = SHAPE_PILLAR
    elif stem.startswith("slash_projectile_2"):
        code = SHAPE_SLASH2
    elif "slash" in stem:
        code = SHAPE_SLASH
    elif stype in ("CircleShape2D", None):
        code = SHAPE_BULLET
    else:
        code = SHAPE_OTHER
    return code, (stype or "?"), radius, half_x, half_y


# Fields that all shooting/charging/spawning behaviors expose (defaults from
# the `export` initializers in the respective `.gd`).
_SHOOT_DEFAULTS = {
    "projectile_speed": 3000, "projectile_speed_randomization": 0,
    "speed_change_after_each_projectile": 0, "cooldown": 60.0,
    "initial_cooldown": 0, "max_cd_randomization": 10,
    "long_cooldown_every_x_shoots": 0, "long_cooldown": 0,
    "damage": 1, "damage_increase_each_wave": 1.0, "min_range": 0,
    "max_range": 500, "attack_anim_speed": 1.0,
    "base_direction_randomization": 0.0, "base_direction_constant_spread": False,
    "alternate_between_base_direction_spread": False, "random_direction": False,
    "number_projectiles": 1, "projectile_spread": 0.0,
    "spawn_projectiles_on_target": False, "projectile_spawn_spread": 0,
    "projectile_spawn_only_on_borders": False, "constant_spread": False,
    "constant_spread_rand_base_pos": 0.0, "atleast_one_projectile_on_target": False,
    "shoot_towards_unit": False, "shoot_in_unit_direction": False,
    "shoot_away_from_unit": False, "shoot_from_proj_pos_towards_player": False,
    "random_rotation": 0.0, "rotate_projectile": True,
    "delete_projectile_on_death": False,
}
_CHARGE_DEFAULTS = {
    "cooldown": 60.0, "long_cooldown_every_x_shoots": 0, "long_cooldown": 0.0,
    "max_cd_randomization": 10, "min_range": 0, "max_range": 300,
    "attack_anim_speed": 1.0, "charge_duration": 1.0, "charge_speed": 500.0,
    "target_calculation_timing": 0, "target": 0, "rand_target_size": -1,
    "only_positions_in_target_direction": False,
    "scale_charge_duration_with_range": False,
}
_SPAWN_DEFAULTS = {
    "cooldown": 60.0, "max_cd_randomization": 10, "attack_anim_speed": 1.0,
    "nb_to_spawn": 1, "spawn_at_random_pos": False,
    "spawn_in_radius_around_unit": -1, "max_nb_of_spawns": -1,
}


_DEGREES_RE = re.compile(r"-?\d+(?:\.\d+)?")


def _load(node, defaults):
    out = {}
    for key, default in defaults.items():
        if isinstance(default, bool):
            out[key] = get_bool(node, key, default)
        elif isinstance(default, int):
            out[key] = get_int(node, key, default)
        else:
            out[key] = get_float(node, key, default)
    return out


_RAW_EXT_RES_RE = re.compile(
    r'\[ext_resource\s+path="([^"]+)"\s+type="(\w+)"\s+id=(\d+)\]')
_RAW_INSTANCE_RE = re.compile(r'instance\s*=\s*ExtResource\(\s*(\d+)\s*\)')
_RAW_ANIM_LINE_RE = re.compile(
    r'(?m)^\s*anims/shoot\s*=\s*(ExtResource\(\s*\d+\s*\)|'
    r'SubResource\(\s*\d+\s*\))\s*$')


def _raw_ext_paths(text):
    """``{id: (path, type)}`` for every ext_resource line of a scene/resource."""
    return {int(i): (p, t) for p, t, i in _RAW_EXT_RES_RE.findall(text)}


def _raw_anim_props(resolver, scene_res, seen=None):
    """``(props_text, description)`` of the kind scene's ``anims/shoot``.

    The shared ``.tscn`` reader drops property keys that contain ``/``
    (extract_physics ``_PROP_RE``), so ``anims/shoot`` -- and with it the whole
    shoot-animation timing -- is read here from the raw text.  The definition
    is inherited: a scene with no ``anims/shoot`` of its own uses the value of
    the base scene it instances (enemy.tscn's ``shoot.tres``)."""
    seen = set() if seen is None else seen
    if scene_res in seen:
        return None
    seen.add(scene_res)
    text = open(resolver.find(scene_res), "r", encoding="utf-8",
                errors="replace").read()
    m = _RAW_ANIM_LINE_RE.search(text)
    if m:
        value = m.group(1)
        sub = re.match(r"SubResource\(\s*(\d+)\s*\)", value)
        if sub:
            block = re.compile(
                r'(?ms)^\[sub_resource\s+type="Animation"\s+id=%d\]\s*$\n'
                r'((?:(?!\[sub_resource|\[node |\[ext_resource|\[connection|'
                r'\[editable).)*)' % int(sub.group(1)))
            sm = block.search(text)
            if sm is None:
                raise ExtractError("%s: anims/shoot SubResource(%s) is missing"
                                   % (scene_res, sub.group(1)))
            return sm.group(1), "%s SubResource(%s)" % (scene_res, sub.group(1))
        em = re.match(r"ExtResource\(\s*(\d+)\s*\)", value)
        ext = _raw_ext_paths(text).get(int(em.group(1)))
        if ext is None:
            raise ExtractError("%s: anims/shoot ExtResource(%s) is missing"
                               % (scene_res, em.group(1)))
        # an Animation .tres holds its properties after the [resource] header
        body = open(resolver.find(ext[0]), "r", encoding="utf-8",
                    errors="replace").read()
        if "[resource]" in body:
            body = body[body.index("[resource]"):]
        return body, ext[0]
    for inst in _RAW_INSTANCE_RE.finditer(text):
        base = _raw_ext_paths(text).get(int(inst.group(1)))
        if base is None or base[1] != "PackedScene":
            continue
        found = _raw_anim_props(resolver, base[0], seen)
        if found is not None:
            return found
    return None


def _raw_track_props(props_text):
    """``{track_id: {prop: value}}`` from the raw text of an Animation.

    A track's properties are one ``tracks/<id>/<prop> = value`` line each, and
    a ``keys`` value spans several lines; continuation lines are appended to
    the property they follow."""
    tracks = {}
    tid = None
    key = None
    for line in props_text.splitlines():
        m = re.match(r"tracks/(\d+)/(\S+)\s*=\s*(.*)$", line)
        if m:
            tid = int(m.group(1))
            key = m.group(2)
            tracks.setdefault(tid, {})[key] = m.group(3)
        elif tid is not None and key is not None and line.strip():
            tracks[tid][key] += " " + line.strip()
    return tracks


def _raw_animation_times(props_text, where):
    """``(shoot_time, length)`` from the raw text of one Animation resource.

    ``AnimationPlayer`` fires a ``.`` method key on the frame whose position
    reaches its time and emits ``animation_finished`` when the position reaches
    the Animation ``length`` (Godot's default 1.0 is only written when it
    differs, so an inline animation with no ``length`` line is 1.0 s)."""
    m = re.search(r'(?m)^length\s*=\s*([\d.]+)\s*$', props_text)
    length = float(m.group(1)) if m else 1.0
    found = None
    for _tid, track in sorted(_raw_track_props(props_text).items()):
        if (track.get("type") or "").strip('"') != "method":
            continue
        pm = re.search(r'NodePath\(\s*"([^"]*)"\s*\)', track.get("path", ""))
        if pm is None or pm.group(1) != ".":
            continue
        keys = track.get("keys", "")
        km = re.search(r'"times"\s*:\s*PoolRealArray\(([^)]*)\)', keys)
        names = re.findall(r'"method"\s*:\s*"([^"]+)"', keys)
        if km is None or not names:
            raise ExtractError("%s: method track without times/methods" % where)
        stamps = [float(t) for t in km.group(1).replace(",", " ").split()]
        if len(stamps) != len(names):
            raise ExtractError("%s: method track times/methods disagree" % where)
        for t, name in zip(stamps, names):
            if name == "shoot":
                if found is not None:
                    raise ExtractError("%s: two shoot method keys" % where)
                found = t
    return found, length


def _anim_frames(seconds, speed, where):
    """Godot frame index of an animation time at ``playback_speed`` ``speed``.

    The player advances by the frame delta (1/60) * speed and fires a key on
    the first frame whose position reaches it, so the index is
    ``ceil(t * 60 / speed)``; the epsilon removes the float error when that is
    an exact integer (24 frames for the 0.4 s charge prep, 48 for 0.8 s)."""
    if not speed > 0.0:
        raise ExtractError("%s: attack_anim_speed must be positive" % where)
    return int(math.ceil(seconds * 60.0 / speed - 1e-9))


def _shoot_animation_frames(resolver, kind_scene, speed, where):
    """``(shoot_at_f, anim_len_f)`` for the kind scene's ``anims/shoot``."""
    found = _raw_anim_props(resolver, kind_scene)
    if found is None:
        raise ExtractError("%s: no `anims/shoot` anywhere in the scene chain"
                           % where)
    props, src = found
    at, length = _raw_animation_times(props, "%s (%s)" % (src, where))
    if at is None:
        raise ExtractError("%s: shoot animation %s has no `shoot` method key"
                           % (where, src))
    if at > length + 1e-9:
        raise ExtractError("%s: %s fires `shoot` at %.6f after length %.6f"
                           % (where, src, at, length))
    return _anim_frames(at, speed, where), _anim_frames(length, speed, where)


_SPAWN_HANDLER_RE = re.compile(
    r"func\s+_on_AttackBehavior_wanted_to_spawn_an_enemy\s*\([^)]*\)[^:]*:")


def _spawn_self_die(resolver, table, scene):
    """1 when the scene root's spawn handler dies right after the emit.

    ``slasher_egg.gd:4-6`` (and the DLC ``iron_lung.gd:27-30``) call the base
    ``_on_AttackBehavior_wanted_to_spawn_an_enemy`` -- which emits
    ``wanted_to_spawn_an_enemy`` -- and then ``die()``, so the unit dies the
    moment its spawning attack shoots."""
    script = table.get(".", {}).get("script")
    if not isinstance(script, tuple) or script[0] != "ext":
        return 0
    text = open(resolver.find(script[1]), "r", encoding="utf-8",
                errors="replace").read()
    m = _SPAWN_HANDLER_RE.search(text)
    if not m:
        return 0
    body = text[m.start():_loop_block(text, m.start())]
    return 1 if re.search(r"\bdie\s*\(", body) else 0


def extract_attack(resolver, node, kind_scene, where):
    """Parse one ``AttackBehavior`` node into a plain dict."""
    beh = behavior_code(node.get("script"))
    if beh is None:
        return None

    out = {
        "beh": beh,
        "nproj": 0, "cooldown_f": 0, "pspeed": 0, "initial_cd_f": 0,
        "proj_shape": SHAPE_BULLET, "proj_radius": 0.0,
        "proj_half_x": 0.0, "proj_half_y": 0.0,
        "spread": 0.0, "constant_spread_rand_base_pos": 0.0,
        "spread_const": 0, "spawn_target": 0, "rotate_proj": 1,
        "spawn_spread": 0, "random_dir": 0,
        "damage": 0.0, "damage_grow": 0.0,
        "charge_duration": 0.0, "charge_speed": 0.0,
        "charge_target": 0, "charge_timing": 0, "charge_dir_lock": 0,
        "proj_speed_rand": 0, "speed_change_each": 0, "base_dir_rand": 0.0,
        "base_dir_const": 0, "alt_base_dir_spread": 0,
        "spawn_only_borders": 0, "atleast_one_on_target": 0,
        "shoot_towards_unit": 0, "shoot_in_unit_dir": 0,
        "shoot_away_from_unit": 0, "shoot_from_proj_pos": 0,
        "random_rotation": 0.0, "delete_on_death": 0,
        "long_cd_every": 0, "long_cooldown": 0, "max_cd_rand": 0,
        "min_range": 0, "max_range": 0, "degrees": [],
        "rand_target_size": 0, "scale_charge_with_range": 0,
        "spawn_at_random_pos": 0, "spawn_radius": 0, "max_nb_spawns": 0,
        "spawn_kind": -1, "proj_scene": "",
        "n_degrees": 0, "degree_first": 0,
        "shoot_at_f": 0, "anim_len_f": 0, "kill_self_on_spawn": 0,
    }

    if beh == BEH_SHOOT:
        p = _load(node, _SHOOT_DEFAULTS)
        out["nproj"] = p["number_projectiles"]
        out["cooldown_f"] = int(round(p["cooldown"]))
        out["pspeed"] = p["projectile_speed"]
        out["proj_speed_rand"] = p["projectile_speed_randomization"]
        out["speed_change_each"] = p["speed_change_after_each_projectile"]
        out["initial_cd_f"] = p["initial_cooldown"]
        out["spread"] = p["projectile_spread"]
        out["constant_spread_rand_base_pos"] = p["constant_spread_rand_base_pos"]
        out["spread_const"] = int(p["constant_spread"])
        out["spawn_target"] = int(p["spawn_projectiles_on_target"])
        out["rotate_proj"] = int(p["rotate_projectile"])
        out["spawn_spread"] = p["projectile_spawn_spread"]
        out["spawn_only_borders"] = int(p["projectile_spawn_only_on_borders"])
        out["random_dir"] = int(p["random_direction"])
        out["base_dir_rand"] = p["base_direction_randomization"]
        out["base_dir_const"] = int(p["base_direction_constant_spread"])
        out["alt_base_dir_spread"] = int(p["alternate_between_base_direction_spread"])
        out["atleast_one_on_target"] = int(p["atleast_one_projectile_on_target"])
        out["shoot_towards_unit"] = int(p["shoot_towards_unit"])
        out["shoot_in_unit_dir"] = int(p["shoot_in_unit_direction"])
        out["shoot_away_from_unit"] = int(p["shoot_away_from_unit"])
        out["shoot_from_proj_pos"] = int(p["shoot_from_proj_pos_towards_player"])
        out["random_rotation"] = p["random_rotation"]
        out["delete_on_death"] = int(p["delete_projectile_on_death"])
        out["long_cd_every"] = p["long_cooldown_every_x_shoots"]
        out["long_cooldown"] = int(round(p["long_cooldown"]))
        out["max_cd_rand"] = p["max_cd_randomization"]
        out["min_range"] = p["min_range"]
        out["max_range"] = p["max_range"]
        out["damage"] = p["damage"]
        out["damage_grow"] = p["damage_increase_each_wave"]
        out["degrees"] = [float(x) for x in
                          _DEGREES_RE.findall(node.get("specific_degrees_spawns", ""))]
        proj = node.get("projectile_scene")
        if proj is None:
            proj = "res://projectiles/bullet_enemy/enemy_projectile.tscn"
        elif isinstance(proj, tuple):
            proj = proj[1]
        else:
            raise ExtractError("%s: projectile_scene is not a resource ref (%r)"
                               % (where, proj))
        proj = proj[len("preload:"):] if proj.startswith("preload:") else proj
        out["proj_scene"] = proj
        code, _t, r, hx, hy = projectile_shape(resolver, proj, where)
        out["proj_shape"] = code
        out["proj_radius"] = r
        out["proj_half_x"] = hx
        out["proj_half_y"] = hy
    elif beh == BEH_CHARGE:
        p = _load(node, _CHARGE_DEFAULTS)
        out["nproj"] = 1
        out["cooldown_f"] = int(round(p["cooldown"]))
        out["pspeed"] = int(round(p["charge_speed"]))
        out["long_cd_every"] = p["long_cooldown_every_x_shoots"]
        out["long_cooldown"] = int(round(p["long_cooldown"]))
        out["max_cd_rand"] = p["max_cd_randomization"]
        out["min_range"] = p["min_range"]
        out["max_range"] = p["max_range"]
        out["charge_duration"] = p["charge_duration"]
        out["charge_speed"] = p["charge_speed"]
        out["charge_target"] = p["target"]
        out["charge_timing"] = p["target_calculation_timing"]
        out["charge_dir_lock"] = int(p["only_positions_in_target_direction"])
        out["rand_target_size"] = p["rand_target_size"]
        out["scale_charge_with_range"] = int(p["scale_charge_duration_with_range"])
    elif beh == BEH_SPAWN:
        p = _load(node, _SPAWN_DEFAULTS)
        out["nproj"] = p["nb_to_spawn"]
        out["cooldown_f"] = int(round(p["cooldown"]))
        out["max_cd_rand"] = p["max_cd_randomization"]
        out["spawn_at_random_pos"] = int(p["spawn_at_random_pos"])
        out["spawn_radius"] = p["spawn_in_radius_around_unit"]
        out["max_nb_spawns"] = p["max_nb_of_spawns"]
        enemy = node.get("enemy_to_spawn")
        if isinstance(enemy, tuple):
            out["proj_scene"] = enemy[1]
        elif enemy is not None:
            raise ExtractError("%s: enemy_to_spawn is not a resource ref (%r)"
                               % (where, enemy))

    # The shoot animation of the scene the behavior belongs to (Enemy.
    # shoot_animation_name == "shoot"): the behavior plays it from
    # physics_process, the "." method track fires `shoot` at shoot_at_f and
    # `animation_finished` arms the next cooldown at anim_len_f.  All three
    # behaviors share that structure, so the frames are per-behavior data.
    if beh in (BEH_SHOOT, BEH_CHARGE, BEH_SPAWN):
        speed = get_float(node, "attack_anim_speed", 1.0)
        frames = _shoot_animation_frames(resolver, kind_scene, speed, where)
        out["shoot_at_f"], out["anim_len_f"] = frames
        # slasher_egg.gd:4-6 / iron_lung.gd:27-30: the root script dies in its
        # wanted_to_spawn handler, one frame-visible death per spawn volley.
        out["kill_self_on_spawn"] = _spawn_self_die(
            resolver, physics.flatten_scene(resolver, kind_scene, {}), where)
    return out


def scene_states(resolver, scene_res, table):
    """Ordered ``(hp_start, timer_start, attack_dict)`` for a boss/elite."""
    parsed = resolver.parse(scene_res)
    order = []
    for node in parsed.nodes:
        if node["parent"] == "States" and node["name"].startswith("State"):
            try:
                order.append((int(node["index"]), node["name"]))
            except (TypeError, ValueError):
                order.append((len(order), node["name"]))
    order.sort()
    if not order:
        # states may be inherited: fall back to flattened keys sorted numerically
        names = [k.split("/")[-1] for k in table
                 if re.match(r"States/State\d+$", k)]
        order = [(int(n[5:]), n) for n in names]
        order.sort()
    states = []
    for _idx, name in order:
        st = table.get("States/" + name, {})
        hp_start = get_float(st, "hp_start", 0.5)
        timer_start = get_float(st, "timer_start", 45.0)
        atk = extract_attack(resolver, table.get("States/%s/AttackBehavior" % name, {}),
                             scene_res, "%s States/%s" % (scene_res, name))
        states.append({"hp_start": hp_start, "timer_start": timer_start,
                       "attack": atk})
    return states


# --------------------------------------------------------------------------
# Buffer boost (buffer.gd exports + BoostZone/BoostCollision radius) and
# spawner death-spawn (spawner.gd: 3 children of enemy_to_spawn within radius).
# --------------------------------------------------------------------------
# Defaults mirror the `export` initializers in enemies/buffer/buffer.gd.
BOOST_DEFAULTS = {
    "nb_entities_boosted_at_once": 1, "boost_cooldown": 4.0,
    "hp_boost": 150, "damage_boost": 25, "speed_boost": 50,
    "player_hp_boost": 20, "player_speed_boost": 20,
    "player_attack_speed_boost": 20, "structure_range_boost": 20,
    "structure_damage_boost": 20, "structure_attack_speed_boost": 20,
}


def parse_boost(resolver, table, scene):
    """``BtBoost`` row for a buffer-like scene, or ``None`` if it has no zone."""
    zone = table.get("BoostZone/BoostCollision")
    if zone is None:
        return None
    if "shape" not in zone:
        raise ExtractError("%s: BoostZone/BoostCollision has no shape" % scene)
    radius = physics.shape_circle(resolver, zone["shape"], scene + " BoostCollision")
    timer = table.get("BoostTimer", {})
    root = table.get(".", {})
    # map gd export names -> normalized table names
    mapping = [
        ("nb_at_once", "nb_entities_boosted_at_once", "int"),
        ("cooldown", "boost_cooldown", "float"),
        ("hp_pct", "hp_boost", "float"),
        ("dmg_pct", "damage_boost", "float"),
        ("speed_pct", "speed_boost", "float"),
        ("player_hp_pct", "player_hp_boost", "float"),
        ("player_speed_pct", "player_speed_boost", "float"),
        ("player_attack_speed_pct", "player_attack_speed_boost", "float"),
        ("structure_range_pct", "structure_range_boost", "float"),
        ("structure_damage_pct", "structure_damage_boost", "float"),
        ("structure_attack_speed_pct", "structure_attack_speed_boost", "float"),
    ]
    out = {"radius": radius}
    for name, prop, kind in mapping:
        default = BOOST_DEFAULTS[prop]
        out[name] = get_int(root, prop, default) if kind == "int" else get_float(root, prop, default)
    out["initial_cd"] = get_float(timer, "wait_time", 0.25)
    return out


# --------------------------------------------------------------------------
# Animated slash sweep (projectiles/slash/slash_projectile*.tscn anims/shoot).
# --------------------------------------------------------------------------
_VEC2_IN_RE = re.compile(r"Vector2\(\s*(-?[0-9.eE+-]+)\s*,\s*(-?[0-9.eE+-]+)\s*\)")


def _track_keys(track, where):
    """``(times, values)`` of one value track, from its raw ``keys`` text."""
    keys = track.get("keys", "")
    m = re.search(r'"times"\s*:\s*PoolRealArray\(([^)]*)\)', keys)
    if m is None:
        raise ExtractError("%s: value track without times" % where)
    times = [float(t) for t in m.group(1).replace(",", " ").split()]
    vm = re.search(r'"values"\s*:\s*\[(.*)\]', keys, re.S)
    if vm is None:
        raise ExtractError("%s: value track without values" % where)
    vecs = [(float(a), float(b)) for a, b in _VEC2_IN_RE.findall(vm.group(1))]
    if len(times) != len(vecs):
        raise ExtractError("%s: %d key times vs %d Vector2 values"
                           % (where, len(times), len(vecs)))
    return times, vecs


def _find_track(tracks, path, where):
    for _tid, track in sorted(tracks.items()):
        if (track.get("path") or "").strip() != 'NodePath("%s")' % path:
            continue
        if (track.get("type") or "").strip('"') != "value":
            continue
        return track
    raise ExtractError("%s: no value track for %s" % (where, path))


def parse_slash_sweep(resolver, scene_res, where):
    """The animated ``Hitbox/Collision`` sweep of a slash projectile scene.

    Returns ``{"keys": [(t, x, y)], "rest": (x, y), "node": (x, y),
    "extents": (x, y), "methods": [(t, name)], "length": float}``.

    The scene's AnimationPlayer autoplays ``shoot``; the ``.`` method track
    disables the hitbox at t=0, enables it at 0.48, disables it at 0.6 and calls
    ``stop`` at 0.63, while the ``Hitbox/Collision:position`` value track sweeps
    the shape linearly (interp 1 = LINEAR, update 0 = CONTINUOUS) between its
    keys.  The sibling ``Hitbox/Collision:shape:extents`` track is INERT in the
    pinned engine build -- measured by instancing this scene and sampling every
    physics frame: the RectangleShape2D extents stay at the scene value for the
    whole sweep -- so the sim must use the constant ``extents`` here."""
    found = _raw_anim_props(resolver, scene_res)
    if found is None:
        raise ExtractError("%s: %s has no anims/shoot" % (where, scene_res))
    props, src = found
    m = re.search(r"(?m)^length\s*=\s*([\d.]+)\s*$", props)
    length = float(m.group(1)) if m else 1.0
    tracks = _raw_track_props(props)
    times, pos = _track_keys(_find_track(tracks, "Hitbox/Collision:position",
                                         "%s (%s)" % (src, where)), where)
    ext_track = None
    try:
        ext_track = _find_track(tracks, "Hitbox/Collision:shape:extents",
                                "%s (%s)" % (src, where))
    except ExtractError:
        ext_track = None
    if ext_track is not None:
        # The scene DOES carry an extents track (the values animate in the text),
        # but the pinned engine never applies it to the CollisionShape2D's
        # resource property (measured).  Only the key grid is asserted here.
        e_times, _e_vals = _track_keys(ext_track, where)
        if e_times != times:
            raise ExtractError("%s: %s extents track keys are not on the position "
                               "track's grid" % (where, src))
    methods = []
    for _tid, track in sorted(tracks.items()):
        if (track.get("type") or "").strip('"') != "method":
            continue
        if (track.get("path") or "").strip() != 'NodePath(".")':
            continue
        km = re.search(r'"times"\s*:\s*PoolRealArray\(([^)]*)\)', track.get("keys", ""))
        names = re.findall(r'"method"\s*:\s*"([^"]+)"', track.get("keys", ""))
        if km is None or len(names) != len(km.group(1).replace(",", " ").split()):
            raise ExtractError("%s: %s method track times/methods disagree" % (where, src))
        for t, nm in zip((float(x) for x in km.group(1).replace(",", " ").split()), names):
            methods.append((t, nm))
    table = physics.flatten_scene(resolver, scene_res, {})
    coll = table.get("Hitbox/Collision")
    hit = table.get("Hitbox")
    if coll is None or hit is None or "shape" not in coll:
        raise ExtractError("%s: %s has no Hitbox/Collision shape" % (where, scene_res))
    rest = _VEC2_IN_RE.match((coll.get("position") or "").strip())
    node = _VEC2_IN_RE.match((hit.get("position") or "").strip())
    if rest is None or node is None:
        raise ExtractError("%s: %s Hitbox/Collision position missing" % (where, scene_res))
    _code, _stype, _radius, hx, hy = projectile_shape(resolver, scene_res, where)
    if hx <= 0.0 or hy <= 0.0:
        raise ExtractError("%s: %s collision is not a rectangle" % (where, scene_res))
    return {"keys": [(t, p[0], p[1]) for t, p in zip(times, pos)],
            "rest": (float(rest.group(1)), float(rest.group(2))),
            "node": (float(node.group(1)), float(node.group(2))),
            "extents": (hx, hy),
            "methods": methods, "length": length}


# --------------------------------------------------------------------------
# Pursuer self-boost (entities/units/enemies/pursuer/pursuer.gd:28-73,94-99).
# --------------------------------------------------------------------------
# The base Pursuer rule (boost_self + _on_hit_something reset) applies only to a
# scene whose ROOT script IS pursuer.gd: pursuer.tscn:72 and
# dlcs/dlc_1/enemies/viperfish/viperfish.tscn:20 attach it directly, while
# dlcs/dlc_1/enemies/anglerfish/anglerfish.gd:2 `extends Pursuer` overrides
# boost_self/_physics_process with its own state machine and is not this rule.
_PURSUER_SCRIPT = "res://entities/units/enemies/pursuer/pursuer.gd"

# (BtSelfBoost field, exported property, c type) -- pursuer.gd:5-12.
SELF_BOOST_EXPORTS = (
    ("cooldown", "boost_cooldown", "float"),
    ("max_boosts", "max_boosts", "int"),
    ("speed", "speed_on_boost", "float"),
    ("dmg_pct", "damage_on_boost", "int"),
    ("reset_on_hit", "reset_nb_times_boosted_on_hit", "bool"),
    ("size", "size_on_boost", "float"),
    ("change_after", "change_movement_behavior_after_x_boosts", "int"),
    ("bonus_on_change", "bonus_speed_on_change_movement_behavior", "float"),
)


def parse_self_boost(resolver, table, scene):
    """``BtSelfBoost`` row for a base-Pursuer scene, or ``None``."""
    root = table.get(".", {})
    script = root.get("script")
    if not isinstance(script, tuple) or script[0] != "ext" or script[1] != _PURSUER_SCRIPT:
        return None
    text = open(resolver.find(script[1]), "r", encoding="utf-8", errors="replace").read()
    if "func boost_self(" not in text:
        raise ExtractError("%s: %s defines no boost_self" % (scene, _PURSUER_SCRIPT))
    timer = table.get("BoostTimer")
    if timer is None:
        raise ExtractError("%s: pursuer.gd without a BoostTimer node" % scene)
    if get_bool(timer, "autostart", False) is not True:
        raise ExtractError("%s: BoostTimer is not autostart" % scene)
    # Timer autostart runs start() at NOTIFICATION_READY (the child's _ready
    # precedes the root script's, which only then sets wait_time = boost_cooldown
    # -- and set_wait_time does not touch time_left), so the FIRST timeout is the
    # scene's own wait_time (the engine's Timer default 1.0 when unset).
    out = {"initial_cd": get_float(timer, "wait_time", 1.0)}
    for field, prop, kind in SELF_BOOST_EXPORTS:
        if prop not in root:
            raise ExtractError("%s: root has no %s export" % (scene, prop))
        if kind == "float":
            out[field] = get_float(root, prop, 0.0)
        elif kind == "int":
            out[field] = get_int(root, prop, 0)
        else:
            out[field] = 1 if get_bool(root, prop, False) else 0
    return out


# --------------------------------------------------------------------------
# Hurtbox-entry reaction chance (fly.gd:3,6-11).
# --------------------------------------------------------------------------
_REACT_CHANCE_RE = re.compile(
    r"export\s*\([^)]*\)\s*var\s+proj_chance\s*(?::[^=]*)?=\s*([0-9.]+)")


def parse_react_chance(resolver, table, scene):
    """Proj chance of a kind that reacts to a hurtbox entry, or ``None``.

    ``fly.gd:3`` declares ``export (float) var proj_chance = 0.25`` and
    ``fly.tscn:24`` overrides it to 0.5; a kind whose root script has no such
    export never reacts (the row stays 0.0)."""
    root = table.get(".", {})
    script = root.get("script")
    if not isinstance(script, tuple) or script[0] != "ext":
        return None
    text = open(resolver.find(script[1]), "r", encoding="utf-8", errors="replace").read()
    m = _REACT_CHANCE_RE.search(text)
    if m is None:
        return None
    return get_float(root, "proj_chance", float(m.group(1)))


def _resolve_script_int(text, name):
    """Follow simple ``var a = b`` / ``var a = 3`` chains to an int."""
    seen = set()
    while name not in seen:
        seen.add(name)
        m = re.search(r"var\s+%s\s*=\s*([A-Za-z_][A-Za-z0-9_]*|\d+)" % re.escape(name), text)
        if not m:
            return None
        value = m.group(1)
        if value.isdigit():
            return int(value)
        name = value
    return None


def _loop_block(text, for_pos):
    """End offset of the ``for`` block starting at ``for_pos`` (indent-based)."""
    line_start = text.rfind("\n", 0, for_pos) + 1
    indent = len(text[line_start:for_pos]) - len(text[line_start:for_pos].lstrip())
    pos = text.find("\n", for_pos) + 1
    while pos < len(text):
        nxt = text.find("\n", pos)
        if nxt == -1:
            nxt = len(text)
        line = text[pos:nxt]
        if line.strip():
            cur = len(line) - len(line.lstrip())
            if cur <= indent:
                return pos
        pos = nxt + 1
    return len(text)


_DEATH_EMIT_RE = re.compile(r'emit_signal\(\s*"wanted_to_spawn_an_enemy"\s*,\s*([A-Za-z_]\w*)')
_DEATH_RADIUS_RE = re.compile(r"get_rand_pos_in_area\([^;]*?,\s*([0-9.]+)\s*\)")
_FOR_RE = re.compile(r"for\s+i\s+in\s+([A-Za-z_]\w*|\d+)")


def parse_death_spawn(resolver, table, scene):
    """List of ``{scene, count, radius}`` death-spawns for a spawner-like scene.

    Driven by each ``emit_signal("wanted_to_spawn_an_enemy", ...)`` inside the
    unit's ``die()``: the child kind comes from the matching root ``*_to_spawn``
    property, the radius from ``get_rand_pos_in_area`` and the count from the
    enclosing ``for i in N`` loop (following simple ``var`` chains)."""
    root = table.get(".", {})
    if not isinstance(root.get("enemy_to_spawn"), tuple):
        return None
    script = root.get("script")
    if not isinstance(script, tuple):
        raise ExtractError("%s: root enemy_to_spawn but no script" % scene)
    text = open(resolver.find(script[1]), "r", encoding="utf-8", errors="replace").read()
    if "func die(" not in text:
        return None
    die = text[text.index("func die("):]
    var_scene = {p: root[p][1] for p in root
                 if p.endswith("_to_spawn") and isinstance(root[p], tuple)}

    loops = [(m.start(), _loop_block(die, m.start()), m.group(1))
             for m in _FOR_RE.finditer(die)]
    entries = []
    for m in _DEATH_EMIT_RE.finditer(die):
        pos = m.start()
        target = var_scene.get(m.group(1))
        if target is None:
            raise ExtractError("%s: death-spawn target %r is not a root property"
                               % (scene, m.group(1)))
        rm = _DEATH_RADIUS_RE.search(die, pos)
        radius = float(rm.group(1)) if rm else 0.0
        count = 1
        for start, end, var in loops:
            if start <= pos < end:
                count = int(var) if var.isdigit() else (_resolve_script_int(text, var) or 1)
        entries.append({"scene": target, "count": count, "radius": radius})
    if not entries:
        raise ExtractError("%s: root enemy_to_spawn but no death-spawn emit found" % scene)
    return entries


# --------------------------------------------------------------------------
# Wave / group / unit resources
# --------------------------------------------------------------------------
def parse_unit(resolver, res_path):
    parsed = resolver.parse(res_path)
    rp = parsed.root_props
    if get_str(rp, "unit_scene_name", "") == "" and "unit_scene" not in rp:
        raise ExtractError("%s: not a wave unit resource" % res_path)
    scene = _one_ref(parsed, rp.get("unit_scene"), res_path)
    return {
        "type": get_int(rp, "type", UNIT_TYPE_ENEMY),
        "scene": scene,
        "min_n": get_int(rp, "min_number", 1),
        "max_n": get_int(rp, "max_number", 1),
        "chance": get_float(rp, "spawn_chance", 1.0),
        # WaveUnitData export (wave_unit_data.gd:13); source default 0.0.
        "additional_min_distance_from_player":
            get_float(rp, "additional_min_distance_from_player", 0.0),
        "src": res_path,
    }


def parse_group(resolver, res_path):
    parsed = resolver.parse(res_path)
    rp = parsed.root_props
    units = [parse_unit(resolver, u) for u in _refs(parsed, rp.get("wave_units_data"))]
    return {
        "timing": get_int(rp, "spawn_timing", 1),
        "chance": get_float(rp, "spawn_chance", 1.0),
        "repeat": get_int(rp, "repeating", 999),
        "repeat_interval": get_float(rp, "repeating_interval", 3),
        "reduce_interval": get_float(rp, "reduce_repeating_interval", 0),
        "min_interval": get_float(rp, "min_repeating_interval", 1),
        "area": get_int(rp, "area", -1),
        "spawn_edge": get_bool(rp, "spawn_edge_of_map", False),
        "spawn_dist": get_int(rp, "spawn_dist_away_from_edges", 0),
        "is_neutral": get_bool(rp, "is_neutral", False),
        "is_boss": get_bool(rp, "is_boss", False),
        "is_horde": get_bool(rp, "is_horde", False),
        "is_loot": get_bool(rp, "is_loot", False),
        "min_difficulty": get_int(rp, "min_difficulty", 0),
        "min_wave": get_int(rp, "min_wave", 0),
        "max_wave": get_int(rp, "max_wave", 99999),
        "units": units,
        "src": res_path,
    }


def parse_unit_template(resolver, res_path):
    """A WaveUnitData used as a TEMPLATE: unit_endless.tres has no unit_scene
    (get_additional_groups assigns it at runtime), so parse_unit's
    exactly-one-scene requirement cannot be used."""
    parsed = resolver.parse(res_path)
    rp = parsed.root_props
    return {
        "type": get_int(rp, "type", UNIT_TYPE_ENEMY),
        "min_n": get_int(rp, "min_number", 1),
        "max_n": get_int(rp, "max_number", 1),
        "chance": get_float(rp, "spawn_chance", 1.0),
        "additional_min_distance_from_player":
            get_float(rp, "additional_min_distance_from_player", 0.0),
        "src": res_path,
    }


def parse_wave(resolver, res_path):
    parsed = resolver.parse(res_path)
    rp = parsed.root_props
    groups = [parse_group(resolver, g) for g in _refs(parsed, rp.get("groups_data"))]
    conditional = []
    cond_value = rp.get("conditional_groups_data", "[  ]")
    for block in _INNER_ARRAY_RE.findall(cond_value):
        refs = _ANY_REF_RE.findall(block)
        if not refs:
            continue
        conditional.append(parse_group(resolver,
                                       _refs(parsed, block)[0]))
    return {
        "duration": get_int(rp, "wave_duration", 60),
        "max_enemies": get_int(rp, "max_enemies", 100),
        "groups": groups,
        "conditional": conditional,
        "src": res_path,
    }


def parse_zone(resolver, res_path, zone_id):
    parsed = resolver.parse(res_path)
    rp = parsed.root_props
    waves = [parse_wave(resolver, w)
             for w in _refs(parsed, rp.get("waves_data", ""))]
    def groups_of(key):
        return [parse_group(resolver, r) for r in _refs(parsed, rp.get(key, ""))]
    return {
        "zone_id": zone_id,
        "waves": waves,
        "all_waves": groups_of("groups_data_in_all_waves"),
        "hordes": groups_of("horde_groups"),
        "loot": groups_of("loot_alien_groups"),
        # zone_service.gd:74/85: the scene pool get_additional_groups draws from.
        "endless_scenes": _refs(parsed, rp.get("endless_enemy_scenes", "")),
    }


# The registry roster order is the SOURCE ARRAY order, not a directory listing:
#   ItemService.elites / .bosses   (singletons/item_service.tscn exports)
#   + dlc_data.elites / .bosses    (dlc_data.gd:63 add_resources appends them)
# then get_elites_from_zone / get_bosses_from_zone filter by zone_id
# (item_service.gd:841-848).  get_random_elite_id_hash_from_zone picks with
# Array.pick_random over that filtered array, so the array order is observable
# through the RNG stream.
_REGISTRY_ARRAYS = (
    "res://singletons/item_service.tscn",
    "res://dlcs/dlc_1/dlc_data.tres",
)


def _registry_array(resolver, res_path, prop):
    """Ordered res:// paths of a registry array exported by a .tres or .tscn."""
    parsed = resolver.parse(res_path)
    if parsed.kind == "resource":
        raw = parsed.root_props.get(prop, "")
    else:
        table = physics.flatten_scene(resolver, res_path, {})
        raw = table.get(".", {}).get(prop, "")
    if isinstance(raw, tuple):          # a single normalized ExtResource
        return [raw[1]]
    return _refs(parsed, raw)


def registry_rows(resolver, prop):
    """Ordered ``(my_id, scene, path, zone_id)`` rows of a registry array."""
    rows = []
    for res_path in _REGISTRY_ARRAYS:
        for ref in _registry_array(resolver, res_path, prop):
            entry = resolver.parse(ref)
            rp = entry.root_props
            rows.append((get_str(rp, "my_id"),
                         _one_ref(entry, rp.get("scene"), ref),
                         ref,
                         get_int(rp, "zone_id", 0)))
    return rows


# --------------------------------------------------------------------------
# Item-granted extra groups
# --------------------------------------------------------------------------
# ExtraEnemyEffect (effects/items/extra_enemy_effect.gd) grants extra enemies
# for the next wave through a wave-group resource stored in `extra_group_data`
# (apply/unapply key the player's effect list by that resource's path).  Those
# groups live outside any zone -- e.g. zones/common/bait/group_bait.tres -- so
# they are unreachable from a zone resource and must be discovered from the
# effect resources themselves.
_EXTRA_GROUP_PROP = "extra_group_data"


def discover_extra_groups(resolver):
    """Canonical sorted item-granted wave groups.

    Scans every ``.tres`` under the recovered roots for an
    ``extra_group_data`` property, resolves the referenced wave-group resource
    and returns a sorted list of ``{"path", "sources"}`` (``sources`` = the
    effect resources referencing it).  Deterministic: unique paths sorted
    lexically, so ``BT_ITEM_EXTRA_GROUPS`` ordinal 0 is stable across runs.
    """
    found = {}
    for root in resolver.roots:
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = sorted(d for d in dirnames if not d.startswith("."))
            for name in sorted(filenames):
                if not name.endswith(".tres"):
                    continue
                absolute = os.path.join(dirpath, name)
                with open(absolute, "rb") as handle:
                    if b"extra_group_data" not in handle.read():
                        continue
                res_path = ("res://"
                            + os.path.relpath(absolute, root).replace(os.sep, "/"))
                if resolver.find(res_path) != absolute:
                    continue          # shadowed by an earlier root
                parsed = resolver.parse(res_path)
                if _EXTRA_GROUP_PROP not in parsed.root_props:
                    raise ExtractError(
                        "%s: extra_group_data is not a root property; inline "
                        "ExtraEnemyEffect resources are not supported" % res_path)
                ref = _one_ref(parsed, parsed.root_props[_EXTRA_GROUP_PROP], res_path)
                found.setdefault(ref, []).append(res_path)
    return [{"path": p, "sources": sorted(found[p])} for p in sorted(found)]


def item_extra_group_ordinals(resolver):
    """``{group resource path: BT_ITEM_EXTRA_GROUPS ordinal}``.

    Shared by ``extract_items.py`` so the effect row and the wave table can
    never disagree about an ordinal.
    """
    return {spec["path"]: i for i, spec in enumerate(discover_extra_groups(resolver))}


# --------------------------------------------------------------------------
# Fresh-instance RNG initialization (scene.instance()/_ready/init vs respawn)
# --------------------------------------------------------------------------
# EntitySpawner.spawn_entity (entity_spawner.gd:346-420) either pops a pooled
# node and calls ``entity.respawn()`` or builds a *fresh* node:
#
#     entity = scene.instance()         # member initializers run here
#     _main.add_entity(entity)          # add_child -> _ready() on the subtree
#     entity.init(...)                  # Unit.init -> movement.init + jitter
#
# Only the fresh branch runs the member initializers and the ``_ready``
# callbacks; only the reuse branch re-rolls the speed jitter (unit.gd:88) and
# calls ``AttackBehavior.reset()`` (enemy.gd:71-86).  Every ``rand_range`` on
# either branch is one draw on the GLOBAL GDScript stream (brotato_rng.h), so
# the two branches consume different word counts and the parent must know
# exactly which one ran.  The rows below are those draws, in source order.
#
# ORDER = the order the source evaluates them:
#   * the ``Enemy._idle_playback_speed`` member initializer (enemy.gd:25);
#   * every movement/attack behaviour node's ``_ready`` in Godot post-order
#     (children before parents, siblings in the final child order the scene
#     instantiation produces -- see ``scene_child_order``);
#   * the ``$MovementBehavior`` chain's ``init()`` (Unit.init, then Boss.init's
#     per-state ``state.movement_behavior.init``);
#   * ``Unit.init``'s speed jitter, which the source evaluates *after*
#     ``_movement_behavior.init(self)`` (unit.gd:76-77).
# On reuse the order is speed jitter first (Unit.respawn, unit.gd:88) and then
# the root ``AttackBehavior``'s ``reset()`` (enemy.gd:73).  Bosses/elites are
# never pooled (Boss.death_animation_finished queue_frees, boss.gd:81-83, and
# Boss.respawn asserts false), so boss kinds carry no reuse rows.

# BtEnemyInitOp.op
BT_INIT_OP_RANGE = 1

# BtEnemyInitOp.phase -- which spawn branch runs the draw
BT_INIT_PHASE_FRESH = 0     # only scene.instance()/_ready/init (fresh node)
BT_INIT_PHASE_REUSE = 1     # only respawn()/reset() (pooled node)
BT_INIT_PHASE_BOTH = 2      # Unit speed jitter: both branches re-roll it

# BtEnemyInitOp.site -- where in the source the draw happens
BT_INITSITE_MEMBER = 0      # member initializer (enemy.gd:25)
BT_INITSITE_MOV_READY = 1   # MovementBehavior._ready
BT_INITSITE_MOV_INIT = 2    # MovementBehavior.init
BT_INITSITE_ATK_READY = 3   # AttackBehavior._ready -> get_cd()
BT_INITSITE_ATK_RESET = 4   # AttackBehavior.reset -> get_cd()
BT_INITSITE_SPEED = 5       # Unit speed jitter (unit.gd:77 / unit.gd:88)

# BtEnemyInitOp.dest -- the member the draw writes (semantic destination).
# INT-typed destinations store the truncated integer, exactly like the source's
# typed assignment: ``var _actual: int`` / ``... as int`` (Godot's typed
# assignment converts the double with a toward-zero truncation).
BT_INITDEST_IDLE_PLAYBACK = 0   # Enemy._idle_playback_speed  (float)
BT_INITDEST_SPEED_RAND = 1      # Unit._speed_rand_value       (int, `as int`)
BT_INITDEST_ATK_CD = 2          # AttackBehavior._current_cd   (float, frames)
BT_INITDEST_MOV_ACTUAL = 3      # movement behavior `_actual`  (var _actual: int)
BT_INITDEST_MOV_ACTUAL_RANGE = 4  # stay_in_range `_actual_target_range` (float)
BT_INITDEST_MOV_PHASE = 5       # patrol_around_the_map `_phase` (float)
BT_INITDEST_MOV_DIST_X = 6      # follow_rand_pos `_distance_from_player.x`
BT_INITDEST_MOV_DIST_Y = 7      # follow_rand_pos `_distance_from_player.y`

# BtEnemyInitOp.behavior -- the node/script the draw belongs to (provenance)
BT_INITBEH_UNIT = 0xFF
BT_INITBEH_MOV_BASE = 0
BT_INITBEH_MOV_STAY_IN_RANGE = 1
BT_INITBEH_MOV_FOLLOW_RAND_AROUND = 2
BT_INITBEH_MOV_PATROL_AROUND_MAP = 3
BT_INITBEH_MOV_PATROL_EDGES = 4
BT_INITBEH_MOV_TARGET_RAND_AROUND = 5
BT_INITBEH_MOV_DETECT_TARGET = 6
BT_INITBEH_MOV_FOLLOW_TARGET = 7
BT_INITBEH_MOV_GO_MAP_CENTER = 8
BT_INITBEH_MOV_PLAYER = 9
BT_INITBEH_MOV_COPY_NEARBY = 10
BT_INITBEH_MOV_TARGET_RAND_GOLD = 11
BT_INITBEH_MOV_TARGET_RAND_POS = 12
BT_INITBEH_MOV_STARGAZER = 13
BT_INITBEH_ATK_BASE = 16
BT_INITBEH_ATK_SHOOTING = 17
BT_INITBEH_ATK_CHARGING = 18
BT_INITBEH_ATK_SPAWNING = 19

BT_INITFLAG_NEG_LO = 1        # lo = -value[lo_ref]
BT_INITFLAG_NEG_HI = 2        # hi = -value[hi_ref]
BT_INITFLAG_TRUNC = 4         # store trunc(value) (int-typed destination)
BT_INITFLAG_COND_SHOTS = 8    # draw happens only if _shots_taken >= cond
BT_INITFLAG_ADD = 16          # value = add + draw (base applied, FP64 order)

BT_INIT_ROOT_STATE = 0xFF     # op belongs to the kind's root, not a boss state
BT_INIT_NO_REF = 0xFFFF       # lo/hi/lo_ref is a literal, not a prior op

_INIT_PHASE_NAMES = {BT_INIT_PHASE_FRESH: "fresh", BT_INIT_PHASE_REUSE: "reuse",
                     BT_INIT_PHASE_BOTH: "both"}
_INIT_SITE_NAMES = {
    BT_INITSITE_MEMBER: "member", BT_INITSITE_MOV_READY: "mov_ready",
    BT_INITSITE_MOV_INIT: "mov_init", BT_INITSITE_ATK_READY: "atk_ready",
    BT_INITSITE_ATK_RESET: "atk_reset", BT_INITSITE_SPEED: "speed"}
_INIT_DEST_NAMES = {
    BT_INITDEST_IDLE_PLAYBACK: "idle_playback", BT_INITDEST_SPEED_RAND: "speed_rand",
    BT_INITDEST_ATK_CD: "atk_cd", BT_INITDEST_MOV_ACTUAL: "mov_actual",
    BT_INITDEST_MOV_ACTUAL_RANGE: "mov_actual_range",
    BT_INITDEST_MOV_PHASE: "mov_phase", BT_INITDEST_MOV_DIST_X: "mov_dist_x",
    BT_INITDEST_MOV_DIST_Y: "mov_dist_y"}
_INIT_BEH_NAMES = {
    BT_INITBEH_UNIT: "unit", BT_INITBEH_MOV_BASE: "mov_base",
    BT_INITBEH_MOV_STAY_IN_RANGE: "mov_stay_in_range",
    BT_INITBEH_MOV_FOLLOW_RAND_AROUND: "mov_follow_rand_around",
    BT_INITBEH_MOV_PATROL_AROUND_MAP: "mov_patrol_around_map",
    BT_INITBEH_MOV_PATROL_EDGES: "mov_patrol_edges",
    BT_INITBEH_MOV_TARGET_RAND_AROUND: "mov_target_rand_around",
    BT_INITBEH_MOV_DETECT_TARGET: "mov_detect_target",
    BT_INITBEH_MOV_FOLLOW_TARGET: "mov_follow_target",
    BT_INITBEH_MOV_GO_MAP_CENTER: "mov_go_map_center",
    BT_INITBEH_MOV_PLAYER: "mov_player",
    BT_INITBEH_MOV_COPY_NEARBY: "mov_copy_nearby",
    BT_INITBEH_MOV_TARGET_RAND_GOLD: "mov_target_rand_gold",
    BT_INITBEH_MOV_TARGET_RAND_POS: "mov_target_rand_pos",
    BT_INITBEH_MOV_STARGAZER: "mov_stargazer",
    BT_INITBEH_ATK_BASE: "atk_base", BT_INITBEH_ATK_SHOOTING: "atk_shooting",
    BT_INITBEH_ATK_CHARGING: "atk_charging", BT_INITBEH_ATK_SPAWNING: "atk_spawning"}

# Fresh-path draws per MovementBehavior script.  ``defaults`` mirror the
# ``export`` initializers in the ``.gd`` (so a scene that never overrides a
# property still resolves to the source default); ``ready``/``init`` hold the
# draws in the order the script evaluates them.  ``lo``/``hi`` are the *actual*
# ``rand_range`` arguments as binary64 functions of the resolved exports and
# ``add`` is the base the source adds to the drawn value afterwards
# (``_actual = range_around_player + rand_range(...)`` draws first, then adds);
# keeping the two apart preserves the source's FP64 rounding order, and a draw
# that names ``add`` raises BT_INITFLAG_ADD (even for a zero base) so the
# replay always applies it.  A row may
# instead set ``lo_ref``/``hi_ref`` to ``-1``/``1`` when its bound is the value
# the *previous* draw of the same list produced (negated / as-is), which is how
# follow_rand_pos reuses its int-typed ``_actual``.
_MOVEMENT_SPECS = {
    "stay_in_range_from_player_movement_behavior": {
        "code": BT_INITBEH_MOV_STAY_IN_RANGE,
        "defaults": {"target_range": 300, "target_range_randomization": 100},
        "ready": [{
            "dest": BT_INITDEST_MOV_ACTUAL_RANGE,
            "src": "entities/units/movement_behaviors/stay_in_range_from_player_movement_behavior.gd:11",
            "lo": lambda p: -p["target_range_randomization"],
            "hi": lambda p: p["target_range_randomization"],
            "add": lambda p: p["target_range"],
        }],
    },
    "follow_rand_pos_around_player_movement_behavior": {
        "code": BT_INITBEH_MOV_FOLLOW_RAND_AROUND,
        "defaults": {"range_around_player": 200, "range_randomization": 0,
                     "allow_within": True},
        "init": [
            {"dest": BT_INITDEST_MOV_ACTUAL, "trunc": True,
             "src": "entities/units/movement_behaviors/follow_rand_pos_around_player_movement_behavior.gd:14",
             "lo": lambda p: -p["range_randomization"],
             "hi": lambda p: p["range_randomization"],
             "add": lambda p: p["range_around_player"]},
            {"dest": BT_INITDEST_MOV_DIST_X,
             "src": "entities/units/movement_behaviors/follow_rand_pos_around_player_movement_behavior.gd:15",
             "lo_ref": -1, "hi_ref": 1},
            {"dest": BT_INITDEST_MOV_DIST_Y,
             "src": "entities/units/movement_behaviors/follow_rand_pos_around_player_movement_behavior.gd:15",
             "lo_ref": -1, "hi_ref": 1},
        ],
    },
    "patrol_around_the_map_movement_behavior": {
        "code": BT_INITBEH_MOV_PATROL_AROUND_MAP,
        "defaults": {"radius_factor": 0.8},
        "init": [{
            "dest": BT_INITDEST_MOV_PHASE,
            "src": "entities/units/movement_behaviors/patrol_around_the_map_movement_behavior.gd:16",
            # rand_range(0.0, 2 * PI): PI is a binary64 engine constant.
            "lo": lambda p: 0.0, "hi": lambda p: 2.0 * math.pi,
        }],
    },
    "patrol_edges_of_map_movement_behavior": {
        "code": BT_INITBEH_MOV_PATROL_EDGES,
        "defaults": {"edge_distance": 300, "edge_randomization": 100,
                     "max_target_distance": 300},
        "init": [{
            "dest": BT_INITDEST_MOV_ACTUAL, "trunc": True,
            "src": "entities/units/movement_behaviors/patrol_edges_of_map_movement_behavior.gd:14",
            "lo": lambda p: -p["edge_randomization"],
            "hi": lambda p: p["edge_randomization"],
            "add": lambda p: p["edge_distance"],
        }],
    },
    "target_rand_pos_around_player_movement_behavior": {
        "code": BT_INITBEH_MOV_TARGET_RAND_AROUND,
        "defaults": {"range_around_player": 300, "range_randomization": 100,
                     "update_every_x_frames": -1},
        "init": [{
            "dest": BT_INITDEST_MOV_ACTUAL, "trunc": True,
            "src": "entities/units/movement_behaviors/target_rand_pos_around_player_movement_behavior.gd:16",
            "lo": lambda p: -p["range_randomization"],
            "hi": lambda p: p["range_randomization"],
            "add": lambda p: p["range_around_player"],
        }],
    },
    # detect_target.init forwards to these two children, in this order
    # (detect_target_movement_behavior.gd:17-21).
    "detect_target_movement_behavior": {
        "code": BT_INITBEH_MOV_DETECT_TARGET,
        "defaults": {"detection_range": 300, "speed_bonus_on_target_detection": 300},
        "init_recursive": ["MovementBehaviorBeforeTargetDetection",
                           "MovementBehaviorAfterTargetDetection"],
    },
    # MovementBehavior subclasses with no fresh-path draw:
    "movement_behavior": {"code": BT_INITBEH_MOV_BASE},
    "follow_target_movement_behavior": {
        "code": BT_INITBEH_MOV_FOLLOW_TARGET,
        "defaults": {"stop_close_to_target": False, "distance_to_target": 30.0}},
    "go_towards_map_center_movement_behavior": {"code": BT_INITBEH_MOV_GO_MAP_CENTER},
    "target_rand_pos_movement_behavior": {"code": BT_INITBEH_MOV_TARGET_RAND_POS},
    "target_rand_gold_pos_behavior": {"code": BT_INITBEH_MOV_TARGET_RAND_GOLD},
    "copy_nearby_entities_movement_behavior": {"code": BT_INITBEH_MOV_COPY_NEARBY},
    "player_movement_behavior": {"code": BT_INITBEH_MOV_PLAYER},
    # DLC stargazer subclass of target_rand_pos (dlcs/dlc_1/enemies/stargazer).
    "stargazer_movement_behavior": {"code": BT_INITBEH_MOV_STARGAZER},
}

_ATTACK_INIT_CODE = {
    BEH_SHOOT: BT_INITBEH_ATK_SHOOTING,
    BEH_CHARGE: BT_INITBEH_ATK_CHARGING,
    BEH_SPAWN: BT_INITBEH_ATK_SPAWNING,
}
# `_ready` sets _current_cd = get_cd() (spawning skips it when
# max_nb_of_spawns == 0); `reset` re-rolls it (reuse only).
_ATK_READY_SRC = {
    BEH_SHOOT: "entities/units/enemies/attack_behaviors/shooting_attack_behavior.gd:56",
    BEH_CHARGE: "entities/units/enemies/attack_behaviors/charging_attack_behavior.gd:36",
    BEH_SPAWN: "entities/units/enemies/attack_behaviors/spawning_attack_behavior.gd:20",
}
_ATK_RESET_SRC = {
    BEH_SHOOT: "entities/units/enemies/attack_behaviors/shooting_attack_behavior.gd:63",
    BEH_CHARGE: "entities/units/enemies/attack_behaviors/charging_attack_behavior.gd:52",
    BEH_SPAWN: "entities/units/enemies/attack_behaviors/spawning_attack_behavior.gd:24",
}


def movement_stem(script):
    """MovementBehavior script stem, or None when the path is not one."""
    if not script:
        return None
    text = str(script)
    match = re.search(r"/movement_behaviors/([a-z_]+)\.gd", text)
    if match:
        return match.group(1)
    match = re.search(r"/([a-z_]+_movement_behavior)\.gd", text)
    return match.group(1) if match else None


def attack_behavior_code(script):
    """``behavior_code`` restricted to real ``attack_behaviors/*`` scripts.

    ``behavior_code`` answers ``BEH_UNKNOWN`` for *any* path that is not an
    attack behaviour, so every caller that walks a whole node tree needs this
    filter to tell "not an attack behaviour" apart from "unknown script".
    """
    if not script or "attack_behaviors/" not in str(script):
        return None
    return behavior_code(script)


def scene_child_order(resolver, scene_res):
    """Ordered child lists of a flattened scene (``parent path -> [children]``).

    Emulates ``PackedScene.instance``: the base scene's nodes are added first,
    then the deriving scene's nodes in file order, and a node declared with an
    ``index`` is moved there (``move_child``) right after it is added.  Godot
    fires ``_ready`` bottom-up over *this* final order, which is what fixes the
    order of behaviour ``_ready`` draws.  The sibling ``index`` Godot records is
    the editor's arrangement at save time, so a node re-declared without an
    index keeps its inherited position.
    """
    children = {}
    guard = set()

    def join(prefix, rel):
        if rel == ".":
            return prefix
        return rel if prefix == "." else prefix + "/" + rel

    def child_path(parent_path, name):
        return name if parent_path == "." else parent_path + "/" + name

    def place(parent_path, path, index):
        lst = children.setdefault(parent_path, [])
        if path in lst:
            if index is None:
                return                      # inherited override: keep position
            lst.remove(path)
        if index is None or index >= len(lst):
            lst.append(path)
        else:
            lst.insert(index, path)

    def walk(res, prefix):
        parsed = resolver.parse(res)
        roots = [n for n in parsed.nodes if n["parent"] is None]
        if len(roots) != 1:
            raise ExtractError("%s: expected exactly one root node" % res)
        if roots[0]["instance"] is not None:
            walk(parsed.ext_path(roots[0]["instance"]), prefix)
        # Sub-instances first (so their subtree keys exist), then placement of
        # every node of this file in file order.
        for node in parsed.nodes:
            if node["instance"] is None or node["parent"] is None:
                continue
            path = child_path(join(prefix, node["parent"]), node["name"])
            sub = parsed.ext_path(node["instance"])
            if (sub, path) in guard:
                continue
            guard.add((sub, path))
            walk(sub, path)
        for node in parsed.nodes:
            if node["parent"] is None:
                continue                    # the root node is `prefix` itself
            parent_path = join(prefix, node["parent"])
            path = child_path(parent_path, node["name"])
            index = node["index"]
            place(parent_path, path, int(index) if index is not None else None)

    walk(scene_res, ".")
    return children


def _postorder(children, root="."):
    """Node paths below ``root``, children before parents, siblings in order."""
    out = []

    def rec(path):
        for child in children.get(path, []):
            rec(child)
        out.append(path)

    rec(root)
    return [path for path in out if path != root]   # drop the root node itself


def _init_op(phase, site, dest, behavior, state, src, lo=0.0, hi=0.0, add=0.0,
             lo_ref=None, hi_ref=None, trunc=False, cond=0, has_add=False):
    flags = 0
    if lo_ref is not None and lo_ref < 0:
        flags |= BT_INITFLAG_NEG_LO
    if hi_ref is not None and hi_ref < 0:
        flags |= BT_INITFLAG_NEG_HI
    if trunc:
        flags |= BT_INITFLAG_TRUNC
    if cond:
        flags |= BT_INITFLAG_COND_SHOTS
    if has_add:
        flags |= BT_INITFLAG_ADD
    return {"op": BT_INIT_OP_RANGE, "phase": phase, "site": site, "dest": dest,
            "behavior": behavior, "state": state, "flags": flags, "cond": cond,
            "lo": float(lo), "hi": float(hi), "add": float(add),
            "lo_ref": lo_ref, "hi_ref": hi_ref, "src": src}


def _movement_spec(node, where):
    """The MovementBehavior spec of a node, or None when the node is not one."""
    script = node.get("script")
    text = script[1] if isinstance(script, tuple) else script
    stem = movement_stem(text)
    if stem is None:
        return None
    spec = _MOVEMENT_SPECS.get(stem)
    if spec is None:
        raise ExtractError("%s: unmodeled movement behavior script %s" % (where, text))
    return spec


def _attack_cd_bounds(node, beh, fresh):
    """``(lo, hi, long_cd_every)`` of ``get_cd()``; ``lo`` None if it is skipped.

    The bounds are the real ``rand_range`` arguments of the behaviour's
    ``get_cd()`` -- ``max(1, cooldown - max_cd_randomization)`` and
    ``cooldown + max_cd_randomization`` -- evaluated in binary64 (the pinned
    build's Variant REAL is a double, so no narrowing happens on the way in),
    with the per-behaviour ``export`` defaults.  These draws have no post-add
    base: the returned value IS ``_current_cd``.  A fresh ``_ready`` always
    rolls (its ``_shots_taken`` is 0); on ``reset`` the shooting/charging
    long-cooldown branch returns ``long_cooldown`` *without* a draw once
    ``_shots_taken >= long_cooldown_every_x_shoots``.  A spawning behaviour with
    ``max_nb_of_spawns == 0`` skips the draw on ``_ready`` only.
    """
    if beh == BEH_SHOOT:
        p = _load(node, _SHOOT_DEFAULTS)
        every = p["long_cooldown_every_x_shoots"]
    elif beh == BEH_CHARGE:
        p = _load(node, _CHARGE_DEFAULTS)
        every = p["long_cooldown_every_x_shoots"]
    else:
        p = _load(node, _SPAWN_DEFAULTS)
        every = 0
        if fresh and p["max_nb_of_spawns"] == 0:
            return None, None, 0
    cd = p["cooldown"]
    rand = p["max_cd_randomization"]
    return max(1.0, cd - rand), cd + rand, every


_SITE_KEY = {BT_INITSITE_MOV_READY: "ready", BT_INITSITE_MOV_INIT: "init"}


def _movement_draws(table, path, spec, site, state, ops):
    """Append the ``_ready``/``init`` draws of the node at ``path`` to ``ops``."""
    if site == BT_INITSITE_MOV_INIT:
        for name in spec.get("init_recursive", []):
            child = path + "/" + name
            child_spec = _movement_spec(table.get(child, {}), child)
            if child_spec is None:
                raise ExtractError("%s: missing nested movement behavior" % child)
            _movement_draws(table, child, child_spec, site, state, ops)
    props = _load(table.get(path, {}), spec.get("defaults", {}))
    for draw in spec.get(_SITE_KEY[site], []):
        ops.append(_init_op(
            BT_INIT_PHASE_FRESH, site, draw["dest"], spec["code"], state, draw["src"],
            lo=draw["lo"](props) if "lo" in draw else 0.0,
            hi=draw["hi"](props) if "hi" in draw else 0.0,
            add=draw["add"](props) if "add" in draw else 0.0,
            lo_ref=draw.get("lo_ref"), hi_ref=draw.get("hi_ref"),
            trunc=draw.get("trunc", False), has_add="add" in draw))


def extract_kind_init(resolver, scene, table, is_boss, speed_rand):
    """``{"fresh": [...], "reuse": [...]}`` ordered init draws for one kind."""
    children = scene_child_order(resolver, scene)
    state_paths = list(children.get("States", []))
    state_ordinal = {p: i for i, p in enumerate(state_paths)}
    if state_paths and not is_boss:
        raise ExtractError("%s: has BossState children but is not an elite/boss"
                           % scene)
    for path in state_paths:
        script = table.get(path, {}).get("script")
        text = script[1] if isinstance(script, tuple) else script
        if not text or not text.endswith("boss_state.gd"):
            raise ExtractError("%s: States child %s is not a BossState" % (scene, path))

    def state_of(path):
        parts = path.split("/")
        if len(parts) >= 2 and parts[0] == "States":
            ordinal = state_ordinal.get(parts[0] + "/" + parts[1])
            if ordinal is not None:
                return ordinal
        return BT_INIT_ROOT_STATE

    fresh = []
    reuse = []

    # 1. Enemy._idle_playback_speed member initializer (all Enemy-derived kinds).
    fresh.append(_init_op(
        BT_INIT_PHASE_FRESH, BT_INITSITE_MEMBER, BT_INITDEST_IDLE_PLAYBACK,
        BT_INITBEH_UNIT, BT_INIT_ROOT_STATE,
        "entities/units/enemies/enemy.gd:25", lo=1.0, hi=3.0))

    # 2. Every behavior node's `_ready`, in the scene's final post-order.
    for path in _postorder(children):
        node = table.get(path)
        if node is None:
            continue
        script = node.get("script")
        text = script[1] if isinstance(script, tuple) else script
        spec = _movement_spec(node, path)
        if spec is not None:
            _movement_draws(table, path, spec, BT_INITSITE_MOV_READY,
                            state_of(path), fresh)
            continue
        beh = attack_behavior_code(text)
        if beh is None:
            continue
        if beh == BEH_UNKNOWN:
            raise ExtractError("%s: unmodeled attack behavior script %s" % (path, text))
        if beh not in _ATTACK_INIT_CODE:
            continue
        lo, hi, _every = _attack_cd_bounds(node, beh, fresh=True)
        if lo is None:
            continue
        fresh.append(_init_op(
            BT_INIT_PHASE_FRESH, BT_INITSITE_ATK_READY, BT_INITDEST_ATK_CD,
            _ATTACK_INIT_CODE[beh], state_of(path), _ATK_READY_SRC[beh],
            lo=lo, hi=hi))

    # 3. `$MovementBehavior.init` (Unit.init -> movement.init), then the speed
    #    jitter Unit.init rolls straight after it (unit.gd:76-77).  The source
    #    scales both bounds by RunData.current_run_accessibility_settings.speed
    #    (a runtime accessibility setting, 1.0 in the oracle); the row stores the
    #    unscaled +/-stats.speed_randomization, matching the `as int` destination.
    root_spec = _movement_spec(table.get("MovementBehavior", {}), "MovementBehavior")
    if root_spec is None:
        raise ExtractError("%s: no MovementBehavior node" % scene)
    _movement_draws(table, "MovementBehavior", root_spec, BT_INITSITE_MOV_INIT,
                    BT_INIT_ROOT_STATE, fresh)
    fresh.append(_init_op(
        BT_INIT_PHASE_BOTH, BT_INITSITE_SPEED, BT_INITDEST_SPEED_RAND,
        BT_INITBEH_UNIT, BT_INIT_ROOT_STATE, "entities/units/unit/unit.gd:77",
        lo=-float(speed_rand), hi=float(speed_rand), trunc=True))

    # 4. `Boss.init` loops the States calling `state.movement_behavior.init`
    #    (boss.gd:16-23) -- after Enemy.init, i.e. after the speed jitter.
    for ordinal, spath in enumerate(state_paths):
        mv = spath + "/MovementBehavior"
        spec = _movement_spec(table.get(mv, {}), mv)
        if spec is None:
            raise ExtractError("%s: %s has no MovementBehavior" % (scene, spath))
        _movement_draws(table, mv, spec, BT_INITSITE_MOV_INIT, ordinal, fresh)

    # Reuse: Unit.respawn's jitter (unit.gd:88) then the root AttackBehavior's
    # reset() (enemy.gd:73).  Bosses/elites never pool, so they have no reuse.
    if not is_boss:
        reuse.append(_init_op(
            BT_INIT_PHASE_BOTH, BT_INITSITE_SPEED, BT_INITDEST_SPEED_RAND,
            BT_INITBEH_UNIT, BT_INIT_ROOT_STATE, "entities/units/unit/unit.gd:88",
            lo=-float(speed_rand), hi=float(speed_rand), trunc=True))
        script = table.get("AttackBehavior", {}).get("script")
        text = script[1] if isinstance(script, tuple) else script
        beh = attack_behavior_code(text)
        if beh is not None and beh != BEH_UNKNOWN and beh in _ATTACK_INIT_CODE:
            lo, hi, every = _attack_cd_bounds(
                table.get("AttackBehavior", {}), beh, fresh=False)
            reuse.append(_init_op(
                BT_INIT_PHASE_REUSE, BT_INITSITE_ATK_RESET, BT_INITDEST_ATK_CD,
                _ATTACK_INIT_CODE[beh], BT_INIT_ROOT_STATE, _ATK_RESET_SRC[beh],
                lo=lo, hi=hi, cond=every))
    return {"fresh": fresh, "reuse": reuse}


# --------------------------------------------------------------------------
# Emission
# --------------------------------------------------------------------------
# Canonical field order for BtAttack -- the struct declaration and every row
# initializer are generated from these lists so they can never drift apart.
_ATTACK_INT_FIELDS = [
    ("beh", "unsigned char"), ("nproj", "unsigned char"),
    ("cooldown_f", "unsigned int"), ("pspeed", "unsigned int"),
    ("initial_cd_f", "unsigned int"), ("proj_shape", "unsigned char"),
    ("spread_const", "unsigned char"), ("spawn_target", "unsigned char"),
    ("rotate_proj", "unsigned char"), ("random_dir", "unsigned char"),
    ("charge_target", "unsigned char"), ("charge_timing", "unsigned char"),
    ("charge_dir_lock", "unsigned char"), ("spawn_spread", "unsigned int"),
    ("spawn_kind", "short"), ("proj_speed_rand", "unsigned short"),
    ("speed_change_each", "short"), ("rand_target_size", "short"),
    ("base_dir_const", "unsigned char"), ("alt_base_dir_spread", "unsigned char"),
    ("spawn_only_borders", "unsigned char"), ("atleast_one_on_target", "unsigned char"),
    ("shoot_towards_unit", "unsigned char"), ("shoot_in_unit_dir", "unsigned char"),
    ("shoot_away_from_unit", "unsigned char"), ("shoot_from_proj_pos", "unsigned char"),
    ("delete_on_death", "unsigned char"), ("long_cd_every", "unsigned char"),
    ("long_cooldown", "unsigned short"), ("max_cd_rand", "unsigned char"),
    ("min_range", "unsigned int"), ("max_range", "unsigned int"),
    ("spawn_at_random_pos", "unsigned char"), ("spawn_radius", "short"),
    ("max_nb_spawns", "short"), ("n_degrees", "unsigned char"),
    ("degree_first", "unsigned short"),
    # shoot-animation timing + spawn-handler death (see _animation_method /
    # _spawn_self_die): frames at 60 fps after attack_anim_speed.
    ("shoot_at_f", "unsigned short"), ("anim_len_f", "unsigned short"),
    ("kill_self_on_spawn", "unsigned char"),
    # Animated-sweep row (projectiles/slash/*.tscn); 0 for every other shape.
    ("proj_sweep", "unsigned char"),
]
_ATTACK_FLOAT_FIELDS = [
    "spread", "constant_spread_rand_base_pos", "base_dir_rand", "random_rotation", "proj_radius", "proj_half_x",
    "proj_half_y", "damage", "damage_grow", "charge_duration", "charge_speed",
    "scale_charge_with_range",
]


def _attack_struct_lines():
    lines = ["typedef struct {"]
    for name, ctype in _ATTACK_INT_FIELDS:
        lines.append("    %s %s;" % (ctype, name))
    for name in _ATTACK_FLOAT_FIELDS:
        lines.append("    float %s;" % name)
    lines.append("} BtAttack;")
    return lines


def fmt_float(v):
    """C float literal that always keeps a decimal point (`8` -> `8.0`)."""
    s = "%.6g" % float(v)
    if "e" in s or "E" in s:
        return s
    if "." not in s:
        s += ".0"
    return s


def _f64_words(v):
    """``(low, high)`` 32-bit words of the IEEE-754 binary64 pattern of ``v``.

    ``rand_range`` bounds and the additive base are binary64 in the pinned
    engine, so a value such as ``2 * PI`` or ``0.1`` MUST NOT be narrowed to a
    ``float`` literal.  MSL has no ``double``, so the init table carries each
    binary64 as its exact bit pattern, split into the low then the high word
    (little-endian, matching ``struct.pack("<d", ...)`` and the bit-word RNG
    API).  The pattern also preserves the sign of ``-0.0``, which a textual
    literal would not.
    """
    low, high = struct.unpack("<II", struct.pack("<d", float(v)))
    return low, high


def fmt_u32(v):
    """C/MSL `unsigned int` literal of one 32-bit word (unsigned suffix)."""
    return "%du" % (v & 0xFFFFFFFF)


def _num(value):
    if isinstance(value, bool):
        return 1 if value else 0
    return value


_MAX = {
    "u8": 0xFF, "u16": 0xFFFF, "u32": 0xFFFFFFFF,
    "s16": 0x7FFF, "s16min": -0x8000,
}


def _check(value, kind, where):
    """Fail loudly rather than silently truncating a C integer field."""
    value = _num(value)
    if kind == "s16":
        if not (_MAX["s16min"] <= value <= _MAX["s16"]):
            raise ExtractError("%s: %d does not fit signed short" % (where, value))
    elif value < 0 or value > _MAX[kind]:
        raise ExtractError("%s: %d does not fit %s" % (where, value, kind))
    return value


def _validate_attack(a, where):
    if a is None:
        return
    _check(a["beh"], "u8", where + ".beh")
    _check(a["nproj"], "u8", where + ".nproj")
    _check(a["cooldown_f"], "u32", where + ".cooldown_f")
    _check(a["pspeed"], "u32", where + ".pspeed")
    _check(a["initial_cd_f"], "u32", where + ".initial_cd_f")
    _check(a["proj_shape"], "u8", where + ".proj_shape")
    _check(a["spawn_spread"], "u32", where + ".spawn_spread")
    _check(a["spawn_kind"], "s16", where + ".spawn_kind")
    _check(a["charge_target"], "u8", where + ".charge_target")
    _check(a["charge_timing"], "u8", where + ".charge_timing")
    _check(a["proj_speed_rand"], "u16", where + ".proj_speed_rand")
    _check(a["speed_change_each"], "s16", where + ".speed_change_each")
    _check(a["rand_target_size"], "s16", where + ".rand_target_size")
    _check(a["long_cooldown"], "u16", where + ".long_cooldown")
    _check(a["max_cd_rand"], "u8", where + ".max_cd_rand")
    _check(a["long_cd_every"], "u8", where + ".long_cd_every")
    _check(a["min_range"], "u32", where + ".min_range")
    _check(a["max_range"], "u32", where + ".max_range")
    _check(a["spawn_radius"], "s16", where + ".spawn_radius")
    _check(a["max_nb_spawns"], "s16", where + ".max_nb_spawns")
    _check(a["shoot_at_f"], "u16", where + ".shoot_at_f")
    _check(a["anim_len_f"], "u16", where + ".anim_len_f")
    _check(a["kill_self_on_spawn"], "u8", where + ".kill_self_on_spawn")
    if a["beh"] == BEH_SHOOT and a["anim_len_f"] <= 0:
        raise ExtractError("%s: a shooting behaviour must have its shoot "
                           "animation frames" % where)
    if a["beh"] == BEH_SPAWN and a["anim_len_f"] <= 0:
        raise ExtractError("%s: a spawning behaviour must have its shoot "
                           "animation frames" % where)
    if a["beh"] == BEH_CHARGE and a["shoot_at_f"] <= 0:
        raise ExtractError("%s: a charging behaviour must wind up for at least "
                           "one frame" % where)
    if a["kill_self_on_spawn"] and a["beh"] != BEH_SPAWN:
        raise ExtractError("%s: kill_self_on_spawn on a non-spawning behaviour"
                           % where)
    for key in ("base_dir_const", "alt_base_dir_spread", "spawn_only_borders",
                "atleast_one_on_target", "shoot_towards_unit", "shoot_in_unit_dir",
                "shoot_away_from_unit", "shoot_from_proj_pos", "delete_on_death",
                "scale_charge_with_range", "spawn_at_random_pos", "spread_const",
                "spawn_target", "rotate_proj", "random_dir", "charge_dir_lock"):
        _check(a[key], "u8", "%s.%s" % (where, key))


def validate(data):
    for scene in data["enemy_kinds"] + data["neutral_kinds"]:
        s = data["kind_stats"][scene]
        for key in ("speed_randomization", "value", "gold_spread"):
            _check(s[key], "u16", "%s.%s" % (scene, key))
        for key in ("min_consumable_tier", "max_consumable_tier"):
            _check(s[key], "u8", "%s.%s" % (scene, key))
    for scene in data["enemy_kinds"]:
        _validate_attack(data["kind_attack"][scene], scene + " root")
        for i, st in enumerate(data["kind_states"][scene]):
            _validate_attack(st["attack"], "%s state%d" % (scene, i))
    # Fresh/reuse init rows: every field fits its C width, every referenced
    # bound points at an EARLIER row of the same sequence, and the per-kind
    # spans stay inside the flat op table.
    for scene in data["enemy_kinds"]:
        init = data["kind_init"][scene]
        for seq in ("fresh", "reuse"):
            ops = init[seq]
            if len(ops) > 0xFF:
                raise ExtractError("%s.%s: more than 255 init ops" % (scene, seq))
            for i, op in enumerate(ops):
                where = "%s.%s[%d]" % (scene, seq, i)
                for key in ("op", "phase", "site", "dest", "behavior", "state", "flags"):
                    _check(op[key], "u8", where + "." + key)
                _check(op["cond"], "u16", where + ".cond")
                if not (math.isfinite(op["lo"]) and math.isfinite(op["hi"])
                        and math.isfinite(op["add"])):
                    raise ExtractError("%s: non-finite rand_range bound/base" % where)
                for key in ("lo_ref", "hi_ref"):
                    ref = op[key]
                    if ref is None:
                        continue
                    if ref not in (-1, 1) or i == 0 or seq == "reuse":
                        raise ExtractError(
                            "%s: %s must name an earlier row of the same sequence"
                            % (where, key))
    groups = []
    for zone in data["zones"]:
        for wave in zone["waves"]:
            groups += wave["groups"] + wave["conditional"]
        groups += zone["all_waves"] + zone["hordes"] + zone["loot"]
    groups += data["all_zone_groups"]
    # Item-granted extra groups must be disjoint from the zone group space so
    # their BT_ITEM_EXTRA_GROUPS ordinals index unique rows.
    zone_paths = set(g["src"] for g in groups)
    for g in data["extra_groups"]:
        if g["src"] in zone_paths:
            raise ExtractError(
                "item extra group %s is already a zone group; it cannot also "
                "carry a stable BT_ITEM_EXTRA_GROUPS ordinal" % g["src"])
    groups = groups + data["extra_groups"]
    for g in groups:
        where = g["src"]
        _check(g["timing"], "u16", where + ".timing")
        _check(g["repeat"], "s16", where + ".repeat")
        _check(g["area"], "s16", where + ".area")
        _check(g["spawn_dist"], "u32", where + ".spawn_dist")
        _check(g["min_difficulty"], "u8", where + ".min_difficulty")
        _check(g["min_wave"], "u32", where + ".min_wave")
        _check(g["max_wave"], "u32", where + ".max_wave")
        for u in g["units"]:
            _check(u["type"], "u8", u["src"] + ".type")
            _check(u["min_n"], "u8", u["src"] + ".min")
            _check(u["max_n"], "u8", u["src"] + ".max")


def build(args):
    resolver = physics.Resolver([args.campaign_root, args.dlc_root])
    camp = resolver.roots[0]
    dlc = resolver.roots[1]

    zone1 = parse_zone(resolver, "res://zones/zone_1/zone_1_data.tres", 0)
    zone2 = parse_zone(resolver,
                       "res://dlcs/dlc_1/zones/abyss/zone_abyss_data.tres", 1)

    # Endless wave templates (zone_service.tscn: endless_groups /
    # endless_unit_groups are single-element arrays).
    endless_group = parse_group(resolver, "res://zones/common/endless/group_endless.tres")
    endless_unit = parse_unit_template(resolver, "res://zones/common/endless/unit_endless.tres")

    # DLC-wide groups that appear in every zone (wave_manager.gd:151-154).
    dlc_parsed = resolver.parse("res://dlcs/dlc_1/dlc_data.tres")
    all_zone_groups = [parse_group(resolver, r)
                       for r in _refs(dlc_parsed,
                                      dlc_parsed.root_props.get("groups_in_all_zones", ""))]

    # Item-granted extra groups (ExtraEnemyEffect.extra_group_data).  They are
    # emitted as a zone-paired tail block of BT_GROUPS so BtItemEff.extra_group
    # can hold a small ordinal instead of a host path string.
    extra_group_specs = discover_extra_groups(resolver)
    extra_groups = [parse_group(resolver, spec["path"])
                    for spec in extra_group_specs]

    # Elite / boss registries in the SOURCE ARRAY order (the campaign
    # ItemService.* exports followed by the DLC dlc_data.* arrays appended by
    # dlc_data.gd:63 add_resources), filtered by zone_id
    # (item_service.gd:841-848).  get_random_elite_id_hash_from_zone uses
    # Array.pick_random over this order, so it must be preserved exactly.
    elite_rows = registry_rows(resolver, "elites")
    boss_rows = registry_rows(resolver, "bosses")
    elites = {z: [(i, s, p) for i, s, p, zd in elite_rows if zd == z] for z in (0, 1)}
    bosses = {z: [(i, s, p) for i, s, p, zd in boss_rows if zd == z] for z in (0, 1)}
    for z in (0, 1):
        if not elites[z]:
            raise ExtractError("zone %d: no elites found" % z)
        if not bosses[z]:
            raise ExtractError("zone %d: no bosses found" % z)

    # ---- collect the kind (scene) space ------------------------------------
    # Enemies: every scene spawned by any wave/global/horde/loot group, plus
    # every spawner's enemy_to_spawn target.  Elites/bosses: registry scenes.
    scene_units = {}   # res:// path -> unit type (1 enemy / 2 neutral)
    for zone in (zone1, zone2):
        for wave in zone["waves"]:
            for grp in wave["groups"] + wave["conditional"]:
                for u in grp["units"]:
                    scene_units.setdefault(u["scene"], u["type"])
        for grp in zone["all_waves"] + zone["hordes"] + zone["loot"]:
            for u in grp["units"]:
                scene_units.setdefault(u["scene"], u["type"])
    for grp in all_zone_groups:
        for u in grp["units"]:
            scene_units.setdefault(u["scene"], u["type"])
    for grp in extra_groups:
        for u in grp["units"]:
            scene_units.setdefault(u["scene"], u["type"])

    elite_scenes = {}   # scene -> (zone, my_id)
    boss_scenes = {}
    for z in (0, 1):
        for my_id, scene, _p in elites[z]:
            elite_scenes.setdefault(scene, (z, my_id))
        for my_id, scene, _p in bosses[z]:
            boss_scenes.setdefault(scene, (z, my_id))

    # Attack behaviors are parsed before the kind space is frozen because
    # spawners reference further kinds (e.g. stargazer -> iron_lung).  Iterate to
    # a fixpoint so every spawner target becomes a real kind.
    scene_cache = {}
    def scene_info(res_path):
        if res_path not in scene_cache:
            scene_cache[res_path] = {
                "table": physics.flatten_scene(resolver, res_path, {})}
        return scene_cache[res_path]

    for scene in list(elite_scenes) + list(boss_scenes):
        scene_units.setdefault(scene, UNIT_TYPE_ENEMY)

    while True:
        added = False
        for scene in list(scene_units):
            if scene_units[scene] == UNIT_TYPE_NEUTRAL:
                continue
            table = scene_info(scene)["table"]
            nodes = [table.get("AttackBehavior", {})]
            nodes += [table[k] for k in table
                      if re.match(r"States/State\d+/AttackBehavior$", k)]
            for node in nodes:
                if behavior_code(node.get("script")) == BEH_SPAWN:
                    target = node.get("enemy_to_spawn")
                    if isinstance(target, tuple):
                        target = target[1]
                        target = target[len("preload:"):] if target.startswith("preload:") else target
                        if target not in scene_units:
                            scene_units[target] = UNIT_TYPE_ENEMY
                            added = True
        if not added:
            break

    # ---- enemy / neutral kinds (deterministic order) -----------------------
    # Class 0 = plain enemies (sorted), then class 1 elites, then class 2
    # bosses, so BT_KIND_CLASS is grouped and bosses are the trailing block.
    neutral_kinds = []
    plain = []
    for scene in sorted(scene_units):
        if scene_units[scene] == UNIT_TYPE_NEUTRAL:
            neutral_kinds.append(scene)
        elif scene not in elite_scenes and scene not in boss_scenes:
            plain.append(scene)
    enemy_kinds = plain + sorted(elite_scenes) + sorted(boss_scenes)
    seen = set()
    deduped = []
    for scene in enemy_kinds:
        if scene not in seen:
            seen.add(scene)
            deduped.append(scene)
    enemy_kinds = deduped

    def scene_stem(res_path):
        return os.path.basename(res_path)[:-len(".tscn")]

    # BT_KINDS must expose unique stems so extract_physics.py can resolve each
    # name back to exactly one scene file.  Neutrals are not physics kinds, so
    # two neutral stems may coincide (the two zone trees); they stay distinct by
    # BT_NEUTRAL_SCENE path.
    stems = {}
    for scene in enemy_kinds:
        stems.setdefault(scene_stem(scene), []).append(scene)
    duplicates = {k: v for k, v in stems.items() if len(v) > 1}
    if duplicates:
        raise ExtractError("duplicate enemy scene stems require a source stem map: %s"
                           % duplicates)

    kind_index = {scene: i for i, scene in enumerate(enemy_kinds)}
    kind_class = {}
    kind_zone = {}
    kind_id = {}
    for scene in enemy_kinds:
        cls, zone, kid = 0, 255, None
        if scene in elite_scenes:
            cls, zone, kid = 1, elite_scenes[scene][0], elite_scenes[scene][1]
        if scene in boss_scenes:
            cls, zone, kid = 2, boss_scenes[scene][0], boss_scenes[scene][1]
        kind_class[scene] = cls
        kind_zone[scene] = zone
        kind_id[scene] = kid
        if kid is None:
            table = scene_info(scene)["table"]
            kid = get_str(table.get(".", {}), "enemy_id", scene_stem(scene))
        kind_id[scene] = kid

    # entity_spawner.gd:151-165 culls over-cap PLAIN enemies by picking from
    # `enemies_to_remove_in_priority` when it is non-empty, else from
    # `enemies`.  That list holds every enemy whose scene sets the exported
    # Enemy.to_be_removed_in_priority (entities/units/enemies/enemy.gd:13,
    # overridden per .tscn; entity_spawner.gd:405 pushes to it on spawn).  The
    # scan is source-derived: only scenes that declare the override are 1.
    kind_prio = {}
    for scene in enemy_kinds:
        table = scene_info(scene)["table"]
        kind_prio[scene] = 1 if get_bool(table.get(".", {}),
                                        "to_be_removed_in_priority", False) else 0

    # ---- stats + attacks per enemy kind -----------------------------------
    kind_stats = {}
    kind_attack = {}
    kind_states = {}
    attack_scene = {}
    for scene in enemy_kinds + neutral_kinds:
        table = scene_info(scene)["table"]
        root = table.get(".", {})
        kind_stats[scene] = parse_stats(resolver, root.get("stats"), scene)

    for scene in enemy_kinds:
        table = scene_info(scene)["table"]
        atk = extract_attack(resolver, table.get("AttackBehavior", {}), scene, scene)
        kind_attack[scene] = atk
        kind_states[scene] = scene_states(resolver, scene, table)
        if atk and atk.get("proj_scene"):
            attack_scene[scene] = atk["proj_scene"]
        for st in kind_states[scene]:
            if st["attack"] and st["attack"].get("proj_scene"):
                attack_scene[scene] = st["attack"]["proj_scene"]

    # Spawner references must resolve to a known enemy kind.
    for scene in enemy_kinds:
        entries = [kind_attack[scene]] + [s["attack"] for s in kind_states[scene]]
        for a in entries:
            if a and a["beh"] == BEH_SPAWN and a.get("proj_scene"):
                if a["proj_scene"] not in kind_index:
                    raise ExtractError("%s: spawner enemy_to_spawn %s is not a known kind"
                                       % (scene, a["proj_scene"]))
                a["spawn_kind"] = kind_index[a["proj_scene"]]

    # ---- buffer boost + spawner death-spawn --------------------------------
    kind_boost = {}
    kind_death = {}
    for scene in enemy_kinds + neutral_kinds:
        table = scene_info(scene)["table"]
        kind_boost[scene] = parse_boost(resolver, table, scene)
        kind_death[scene] = parse_death_spawn(resolver, table, scene)
    for scene in enemy_kinds:
        for d in (kind_death.get(scene) or []):
            if d["scene"] not in kind_index:
                raise ExtractError("%s: death-spawn target %s is not a known kind"
                                   % (scene, d["scene"]))
            d["kind"] = kind_index[d["scene"]]

    # ---- self-boost, hurtbox reaction, slash sweep --------------------------
    kind_selfboost = {}
    kind_react = {}
    for scene in enemy_kinds + neutral_kinds:
        table = scene_info(scene)["table"]
        kind_selfboost[scene] = parse_self_boost(resolver, table, scene)
        kind_react[scene] = parse_react_chance(resolver, table, scene)

    # One sweep row per DISTINCT slash projectile scene any attack uses.  The
    # keyframe arrays are emitted once, so every row must share them (verified).
    slash_rows = []                 # list of (res_path, stem, spec)
    slash_index = {}                # res_path -> row index
    slash_keys = None
    for scene in enemy_kinds:
        entries = [kind_attack[scene]] + [s["attack"] for s in kind_states[scene]]
        for a in entries:
            if not a or a.get("proj_shape") not in (SHAPE_SLASH, SHAPE_SLASH2):
                continue
            res = a.get("proj_scene")
            if res is None:
                raise ExtractError("%s: slash attack without a projectile scene" % scene)
            if res in slash_index:
                continue
            spec = parse_slash_sweep(resolver, res, scene)
            if slash_keys is None:
                slash_keys = spec["keys"]
            elif spec["keys"] != slash_keys:
                raise ExtractError("%s: %s slash keyframes differ from %s; the flat "
                                   "keyframe table cannot carry both"
                                   % (scene, res, slash_rows[0][0]))
            slash_index[res] = len(slash_rows)
            slash_rows.append((res, os.path.basename(res)[:-len(".tscn")], spec))

    def sweep_of(a):
        if not a or a.get("proj_shape") not in (SHAPE_SLASH, SHAPE_SLASH2):
            return 0
        return slash_index[a["proj_scene"]]

    # Row order IS the shape-code order the sim indexes with (BT_SHAPE_SLASH = 2
    # -> row 0, BT_SHAPE_SLASH2 = 4 -> row 1), so a third slash projectile scene
    # must extend the sim's mapping explicitly instead of silently aliasing.
    slash_rows.sort(key=lambda r: 0 if "slash_projectile_2" not in r[1] else 1)
    if len(slash_rows) > 2:
        raise ExtractError("more than two slash projectile scenes: %s"
                           % [r[1] for r in slash_rows])
    slash_index = {res: i for i, (res, _lbl, _spec) in enumerate(slash_rows)}

    # Every attack row carries the sweep row its projectiles must animate with
    # (0 for everything that is not a slash).
    for scene in enemy_kinds:
        if kind_attack[scene]:
            kind_attack[scene]["proj_sweep"] = sweep_of(kind_attack[scene])
        for st in kind_states[scene]:
            if st["attack"]:
                st["attack"]["proj_sweep"] = sweep_of(st["attack"])

    # ---- fresh/reuse RNG initialization ------------------------------------
    # One ordered op list per enemy kind (neutrals extend Unit, not Enemy, so
    # they have no idle member draw / `_ready` attack draw; their only init
    # draw is the shared speed jitter already covered by BT_KINDS).
    kind_init = {}
    for scene in enemy_kinds:
        table = scene_info(scene)["table"]
        kind_init[scene] = extract_kind_init(
            resolver, scene, table, kind_class[scene] != 0,
            kind_stats[scene]["speed_randomization"])

    return {
        "resolver": resolver,
        "zones": [zone1, zone2],
        "all_zone_groups": all_zone_groups,
        "extra_groups": extra_groups,
        "extra_group_specs": extra_group_specs,
        "enemy_kinds": enemy_kinds,
        "neutral_kinds": neutral_kinds,
        "kind_index": kind_index,
        "neutral_index": {s: i for i, s in enumerate(neutral_kinds)},
        "endless_group": endless_group,
        "endless_unit": endless_unit,
        "kind_class": kind_class,
        "kind_zone": kind_zone,
        "kind_prio": kind_prio,
        "kind_id": kind_id,
        "kind_stats": kind_stats,
        "kind_attack": kind_attack,
        "kind_states": kind_states,
        "kind_boost": kind_boost,
        "kind_death": kind_death,
        "kind_selfboost": kind_selfboost,
        "kind_react": kind_react,
        "slash_rows": slash_rows,
        "slash_keys": slash_keys or [],
        "sweep_of": sweep_of,
        "kind_init": kind_init,
        "scene_stem": scene_stem,
        "elites": elites,
        "bosses": bosses,
        "elite_scenes": elite_scenes,
        "boss_scenes": boss_scenes,
    }


def _unit_rows(data, group):
    """Render a group's units and return (rows, count)."""
    rows = []
    for u in group["units"]:
        scene = u["scene"]
        if scene in data["kind_index"]:
            domain, index = 0, data["kind_index"][scene]
        elif scene in data["neutral_index"]:
            domain, index = 1, data["neutral_index"][scene]
        else:
            raise ExtractError("unit scene %s is not a known kind" % scene)
        rows.append("    { %d, %d, %d, %d, %d, %sf, %sf },  /* %s */"
                    % (domain, u["type"], index, u["min_n"], u["max_n"],
                       fmt_float(u["chance"]),
                       fmt_float(u["additional_min_distance_from_player"]),
                       data["scene_stem"](scene)))
    return rows


def emit_waves(data, out_path):
    """Emit brotato_waves.h: device-portable (BT_TABLE) numeric tables plus
    BT_HOST_ONLY name tables."""
    stem = data["scene_stem"]
    kinds = data["enemy_kinds"]
    neutrals = data["neutral_kinds"]
    nwaves = max(len(z["waves"]) for z in data["zones"])

    groups = []                      # flat BtSpawnGroup rows (dicts)
    zone_units = {0: [], 1: []}      # per-zone flat BtSpawnUnit C lines
    zone_unit_count = {0: 0, 1: 0}
    zone_wave_rows = {0: [], 1: []}
    all_wave_idx = {0: [], 1: []}
    horde_idx = {0: [], 1: []}
    loot_idx = {0: [], 1: []}
    allzone_idx = {0: [], 1: []}

    def add_group(zone, g):
        gid = len(groups)
        rows = _unit_rows(data, g)
        first = zone_unit_count[zone]
        zone_units[zone].extend(rows)
        zone_unit_count[zone] += len(rows)
        groups.append({"row": g, "zone": zone, "unit_first": first,
                       "unit_count": len(rows), "id": gid})
        return gid

    for zone in data["zones"]:
        zi = zone["zone_id"]
        for wave in zone["waves"]:
            gfirst = len(groups)
            for g in wave["groups"]:
                add_group(zi, g)
            if wave["conditional"]:
                cfirst = len(groups)
                for g in wave["conditional"]:
                    add_group(zi, g)
            else:
                cfirst = 0
            zone_wave_rows[zi].append((wave["duration"], wave["max_enemies"],
                                       gfirst, len(wave["groups"]), cfirst,
                                       len(wave["conditional"])))
        all_wave_idx[zi] = [add_group(zi, g) for g in zone["all_waves"]]
        horde_idx[zi] = [add_group(zi, g) for g in zone["hordes"]]
        loot_idx[zi] = [add_group(zi, g) for g in zone["loot"]]
        # DLC groups_in_all_zones spawn in EVERY zone: one copy per zone so the
        # per-zone unit spans stay valid.
        allzone_idx[zi] = [add_group(zi, g) for g in data["all_zone_groups"]]

    # Item-granted extra groups (ExtraEnemyEffect).  Emitted AFTER every zone
    # group so all existing BT_GROUPS indices stay frozen; one copy per zone
    # (like the DLC all-zone groups) keeps each copy's unit span valid inside
    # that zone's BtSpawnUnit slice.
    item_extra_idx = {}
    for ordinal, g in enumerate(data["extra_groups"]):
        item_extra_idx[ordinal] = {z: add_group(z, g) for z in (0, 1)}

    unit_lines = zone_units[0] + zone_units[1]
    unit_off = [0, len(zone_units[0])]

    L = []
    L.append("/* Generated by ocean/brotato/extract_gameplay.py from the decompiled")
    L.append(" * Brotato project (v1.1.14.6).  Campaign zone 0 = zones/zone_1,")
    L.append(" * DLC zone 1 = dlcs/dlc_1/zones/abyss.  Every row is source-derived;")
    L.append(" * rerunning the generator reproduces this file byte-for-byte.")
    L.append(" *")
    L.append(" * DEVICE PORTABILITY: numeric tables use BT_TABLE (static const on the")
    L.append(" * host, constant/device storage on MSL/nvcc).  Every string/pointer")
    L.append(" * lives in a BT_HOST_ONLY(...) table, which the device prelude expands")
    L.append(" * away -- so CUDA/MSL translation units contain no names and no")
    L.append(" * pointers.  Per-zone unit rosters are flattened into BT_ZONE_UNITS_ALL")
    L.append(" * + BT_ZONE_UNITS_OFF (index via BT_ZONE_UNIT(zone, i) in")
    L.append(" * brotato_state.h); the host also gets the legacy pointer array")
    L.append(" * BT_ZONE_UNITS[BT_ZONES] under BT_HOST_ONLY.")
    L.append(" */")
    L.append("#ifndef BROTATO_WAVES_H")
    L.append("#define BROTATO_WAVES_H")
    L.append("#include \"brotato_rng.h\"   /* BT_TABLE / BT_HOST_ONLY */")
    L.append("")
    L.append("#define BT_ZONES 2")
    L.append("#define BT_WAVES_MAX %d" % nwaves)
    L.append("")

    # ---- unit kind stats ---------------------------------------------------
    L.append("/* Enemy/elite/boss kind stats (entities/units/unit/stats.gd).")
    L.append(" * speed_rand is the ABSOLUTE speed jitter (unit.gd:81 adds")
    L.append(" * rand_range(-r, +r) to stats.speed -- it is not a percentage). */")
    L.append("typedef struct {")
    L.append("    float hp, hp_grow;             /* health, health_increase_each_wave */")
    L.append("    float speed;                   /* stats.speed (absolute units/s) */")
    L.append("    unsigned short speed_rand;     /* stats.speed_randomization (absolute) */")
    L.append("    float dmg, dmg_grow;           /* damage, damage_increase_each_wave */")
    L.append("    float attack_cd;               /* stats.attack_cd (frames) */")
    L.append("    unsigned short value;          /* material value per kill */")
    L.append("    unsigned short gold_spread;    /* extra material spread */")
    L.append("    float knockback_res;           /* knockback_resistance (0..1 factor) */")
    L.append("    float base_drop_chance, item_drop_chance;")
    L.append("    float armor, armor_grow;       /* armor, armor_increase_each_wave */")
    L.append("    unsigned char can_drop_consumables, always_drop_consumables;")
    L.append("    unsigned char min_tier, max_tier; /* consumable tier bounds */")
    L.append("} BtUnitKind;")
    L.append("")
    L.append("#define BT_NKINDS %d" % len(kinds))
    L.append("BT_TABLE BtUnitKind BT_KINDS[BT_NKINDS] = {")
    for scene in kinds:
        s = data["kind_stats"][scene]
        L.append("    { %sf, %sf, %sf, %d, %sf, %sf, %sf, %d, %d, %sf, %sf, %sf, %sf, %sf, %d, %d, %d, %d },  /* %s */"
                 % (fmt_float(s["health"]), fmt_float(s["health_increase_each_wave"]),
                    fmt_float(s["speed"]), s["speed_randomization"],
                    fmt_float(s["damage"]), fmt_float(s["damage_increase_each_wave"]),
                    fmt_float(s["attack_cd"]), s["value"], s["gold_spread"],
                    fmt_float(s["knockback_resistance"]),
                    fmt_float(s["base_drop_chance"]), fmt_float(s["item_drop_chance"]),
                    fmt_float(s["armor"]), fmt_float(s["armor_increase_each_wave"]),
                    1 if s["can_drop_consumables"] else 0,
                    1 if s["always_drop_consumables"] else 0,
                    s["min_consumable_tier"], s["max_consumable_tier"], stem(scene)))
    L.append("};")
    L.append("")
    L.append("/* class: 0 = enemy, 1 = elite, 2 = boss;  zone: 0/1 or 255 = any */")
    L.append("#define BT_KIND_CLASS_N BT_NKINDS")
    L.append("BT_TABLE unsigned char BT_KIND_CLASS[BT_KIND_CLASS_N] = {")
    L.append("    " + ", ".join(str(data["kind_class"][s]) for s in kinds))
    L.append("};")
    L.append("BT_TABLE unsigned char BT_KIND_ZONE[BT_NKINDS] = {")
    L.append("    " + ", ".join(str(data["kind_zone"][s]) for s in kinds))
    L.append("};")
    L.append("/* Enemy.to_be_removed_in_priority (entities/units/enemies/enemy.gd:13,")
    L.append(" * overridden per .tscn: baby_alien/chaser/fin_alien/helmet_alien + the DLC")
    L.append(" * lobster/plankton/shrimp).  1 = the kind joins")
    L.append(" * entity_spawner.enemies_to_remove_in_priority on spawn (:405), the list")
    L.append(" * the over-cap cull draws from first (:151-165). */")
    L.append("#define BT_KIND_PRIORITY_REMOVE_N BT_NKINDS")
    L.append("BT_TABLE unsigned char BT_KIND_PRIORITY_REMOVE[BT_KIND_PRIORITY_REMOVE_N] = {")
    L.append("    " + ", ".join(str(data["kind_prio"][s]) for s in kinds))
    L.append("};")
    L.append("/* Standalone absolute speed jitter (== BT_KINDS[].speed_rand); not a pct. */")
    L.append("#define BT_SPEED_RAND_N BT_NKINDS")
    L.append("BT_TABLE unsigned short BT_SPEED_RAND[BT_SPEED_RAND_N] = {")
    L.append("    " + ", ".join(str(data["kind_stats"][s]["speed_randomization"])
                              for s in kinds))
    L.append("};")
    L.append("")
    L.append("/* Host-only name tables (dropped on device builds), aligned with BT_KINDS.")
    L.append(" * BT_KIND_NAME is the scene stem (also the extract_physics.py lookup key);")
    L.append(" * BT_KIND_ID is the source enemy_id / elite / boss id. */")
    L.append("BT_HOST_ONLY(static const char* const BT_KIND_NAME[BT_NKINDS] = {")
    L.append("    " + ", ".join('"%s"' % stem(s) for s in kinds))
    L.append("};)")
    L.append("BT_HOST_ONLY(static const char* const BT_KIND_ID[BT_NKINDS] = {")
    L.append("    " + ", ".join('"%s"' % data["kind_id"][s] for s in kinds))
    L.append("};)")
    L.append("BT_HOST_ONLY(static const char* const BT_KIND_SCENE[BT_NKINDS] = {")
    L.append("    " + ", ".join('"%s"' % s for s in kinds))
    L.append("};)")
    L.append("")

    # ---- buffer boost (buffer.gd / buffer.tscn) ---------------------------
    L.append("/* Buffer-style boost (entities/units/enemies/buffer/buffer.gd).")
    L.append(" * Zero rows (radius 0) mean the kind has no BoostZone and never boosts.")
    L.append(" * Radius is the BoostZone/BoostCollision CircleShape2D; *_pct are the")
    L.append(" * effect percentage values applied on boost. */")
    L.append("typedef struct {")
    L.append("    float radius;            /* BoostZone collision radius (0 = none) */")
    L.append("    float initial_cd;        /* BoostTimer wait_time (s) */")
    L.append("    float cooldown;          /* boost_cooldown (s) */")
    L.append("    unsigned char nb_at_once;/* nb_entities_boosted_at_once */")
    L.append("    float hp_pct, dmg_pct, speed_pct;               /* enemy boosts */")
    L.append("    float player_hp_pct, player_speed_pct, player_attack_speed_pct;")
    L.append("    float structure_range_pct, structure_damage_pct,")
    L.append("          structure_attack_speed_pct;")
    L.append("} BtBoost;")
    L.append("#define BT_NBOOST BT_NKINDS")
    L.append("BT_TABLE BtBoost BT_KIND_BOOST[BT_NBOOST] = {")
    for scene in kinds:
        b = data["kind_boost"].get(scene)
        if b is None:
            b = {"radius": 0.0, "initial_cd": 0.0, "cooldown": 0.0, "nb_at_once": 0,
                 "hp_pct": 0.0, "dmg_pct": 0.0, "speed_pct": 0.0,
                 "player_hp_pct": 0.0, "player_speed_pct": 0.0,
                 "player_attack_speed_pct": 0.0, "structure_range_pct": 0.0,
                 "structure_damage_pct": 0.0, "structure_attack_speed_pct": 0.0}
        L.append("    { %sf, %sf, %sf, %d, %sf, %sf, %sf, %sf, %sf, %sf, %sf, %sf, %sf },  /* %s */"
                 % (fmt_float(b["radius"]), fmt_float(b["initial_cd"]),
                    fmt_float(b["cooldown"]), _check(b["nb_at_once"], "u8", scene + ".nb_at_once"),
                    fmt_float(b["hp_pct"]), fmt_float(b["dmg_pct"]), fmt_float(b["speed_pct"]),
                    fmt_float(b["player_hp_pct"]), fmt_float(b["player_speed_pct"]),
                    fmt_float(b["player_attack_speed_pct"]), fmt_float(b["structure_range_pct"]),
                    fmt_float(b["structure_damage_pct"]), fmt_float(b["structure_attack_speed_pct"]),
                    stem(scene)))
    L.append("};")
    L.append("")
    L.append("/* Spawner death-spawn (unit die(): spawn children of KIND within")
    L.append(" * RADIUS arena units).  ND entries = number of death-spawn emits; a")
    L.append(" * KIND of -1 marks an unused slot.  Counts are the scripts' loop bounds;")
    L.append(" * blobfish's first count is the script default (4) -- it is modified at")
    L.append(" * runtime by weapon type / number_of_enemies, which the table cannot hold. */")
    max_death = max((len(data["kind_death"].get(s) or []) for s in kinds), default=0)
    L.append("#define BT_MAX_DEATH_SPAWNS %d" % max(1, max_death))
    def _death_rows(field, ctype, default, where):
        rows = []
        for s in kinds:
            ds = data["kind_death"].get(s) or []
            vals = [ds[i][field] if i < len(ds) else default
                    for i in range(BT_MAX_DEATH_LOCAL)]
            rows.append("    { %s },  /* %s */" % (
                ", ".join(str(_check(v, ctype, where % (s,))) for v in vals), stem(s)))
        return rows
    BT_MAX_DEATH_LOCAL = max(1, max_death)
    L.append("BT_TABLE short BT_KIND_DEATH_SPAWN_KIND[BT_NKINDS][BT_MAX_DEATH_SPAWNS] = {")
    L.extend(_death_rows("kind", "s16", -1, "%s.death_kind"))
    L.append("};")
    L.append("BT_TABLE unsigned char BT_KIND_NDEATH_SPAWNS[BT_NKINDS] = {")
    L.append("    " + ", ".join(str(len(data["kind_death"].get(s) or [])) for s in kinds))
    L.append("};")
    L.append("BT_TABLE unsigned char BT_KIND_DEATH_SPAWN_NB[BT_NKINDS][BT_MAX_DEATH_SPAWNS] = {")
    L.extend(_death_rows("count", "u8", 0, "%s.death_nb"))
    L.append("};")
    L.append("BT_TABLE float BT_KIND_DEATH_SPAWN_RADIUS[BT_NKINDS][BT_MAX_DEATH_SPAWNS] = {")
    for s in kinds:
        ds = data["kind_death"].get(s) or []
        vals = [fmt_float(ds[i]["radius"]) + "f" if i < len(ds) else "0.0f"
                for i in range(BT_MAX_DEATH_LOCAL)]
        L.append("    { %s },  /* %s */" % (", ".join(vals), stem(s)))
    L.append("};")
    L.append("")

    # ---- Pursuer self-boost (pursuer.gd:28-73,94-99) ----------------------
    L.append("/* Pursuer self-boost (entities/units/enemies/pursuer/pursuer.gd).")
    L.append(" * Rows cover exactly the scenes whose ROOT script is pursuer.gd")
    L.append(" * (pursuer.tscn, dlc_1/viperfish.tscn); a kind with max_boosts 0 has")
    L.append(" * no BoostTimer.  cooldown/initial_cd are seconds, speed is the flat")
    L.append(" * bonus_speed added per boost, dmg_pct the percent modifier")
    L.append(" * reset_damage_stat receives as damage_on_boost * nb_times_boosted,")
    L.append(" * reset_on_hit is reset_nb_times_boosted_on_hit.  initial_cd is the")
    L.append(" * BoostTimer's scene wait_time (autostart calls start() at the child's")
    L.append(" * _ready, before the root script sets wait_time = boost_cooldown, and")
    L.append(" * set_wait_time never touches time_left), cooldown the value every")
    L.append(" * later reload uses.  pursuer.gd's size_on_boost /")
    L.append(" * change_movement_behavior_after_x_boosts branches are NOT modelled:")
    L.append(" * the campaign pursuer sets size 0 and change -1 (inert); only the DLC")
    L.append(" * viperfish sets them, and the shared core has no per-slot geometry")
    L.append(" * scale or alternate MovementBehavior node. */")
    L.append("typedef struct {")
    L.append("    float cooldown;            /* boost_cooldown (s) */")
    L.append("    float initial_cd;          /* BoostTimer wait_time at autostart (s) */")
    L.append("    float speed;               /* speed_on_boost (flat bonus_speed) */")
    L.append("    unsigned char max_boosts;  /* max_boosts (0 = no BoostTimer) */")
    L.append("    unsigned char dmg_pct;     /* damage_on_boost (percent per boost) */")
    L.append("    unsigned char reset_on_hit;/* reset_nb_times_boosted_on_hit */")
    L.append("    float size;                /* size_on_boost: +0.25*size per boost to")
    L.append("                                * every Collision/Hurtbox/Hitbox shape scale */")
    L.append("    short change_after;        /* change_movement_behavior_after_x_boosts (-1 none) */")
    L.append("    float bonus_on_change;     /* bonus_speed_on_change_movement_behavior (flat) */")
    L.append("} BtSelfBoost;")
    L.append("#define BT_NSELFBOOST BT_NKINDS")
    L.append("BT_TABLE BtSelfBoost BT_KIND_SELFBOOST[BT_NSELFBOOST] = {")
    for scene in kinds:
        b = data["kind_selfboost"].get(scene)
        if b is None:
            b = {"cooldown": 0.0, "initial_cd": 0.0, "speed": 0.0,
                 "max_boosts": 0, "dmg_pct": 0, "reset_on_hit": 0,
                 "size": 0.0, "change_after": -1, "bonus_on_change": 0.0}
        L.append("    { %sf, %sf, %sf, %d, %d, %d, %sf, %d, %sf },  /* %s */"
                 % (fmt_float(b["cooldown"]), fmt_float(b["initial_cd"]),
                    fmt_float(b["speed"]),
                    _check(b["max_boosts"], "u8", scene + ".max_boosts"),
                    _check(b["dmg_pct"], "u8", scene + ".damage_on_boost"),
                    _check(b["reset_on_hit"], "u8", scene + ".reset_on_hit"),
                    fmt_float(b["size"]),
                    _check(b["change_after"], "s16", scene + ".change_after"),
                    fmt_float(b["bonus_on_change"]),
                    stem(scene)))
    L.append("};")
    L.append("")
    L.append("/* Hurtbox-entry reaction chance (fly.gd:3,6-11 + fly.tscn:24): on a")
    L.append(" * RangedWeapon / shooting-Pet hitbox entering the Hurtbox, Utils.")
    L.append(" * get_chance_success(proj_chance) draws once on the global stream")
    L.append(" * (brotato_core.h bt_chance: chance 0 draws nothing) and a success")
    L.append(" * call_deferred()s the attack behavior's shoot().  0 = the kind has")
    L.append(" * no such reaction. */")
    L.append("BT_TABLE float BT_KIND_PROJ_CHANCE[BT_NKINDS] = {")
    rows = []
    for scene in kinds:
        c = data["kind_react"].get(scene)
        rows.append("%sf" % fmt_float(0.0 if c is None else c))
    L.append("    " + ", ".join(rows))
    L.append("};")
    L.append("")

    # ---- fresh/reuse RNG initialization -----------------------------------
    # Every row is one source `rand_range(lo, hi)` the fresh or the reuse spawn
    # branch runs (see extract_kind_init above).  `lo_ref`/`hi_ref` name an
    # earlier row of the SAME sequence whose stored value is the bound (used by
    # follow_rand_pos: `_distance_from_player = Vector2(rand_range(-_actual,
    # _actual), ...)`); 0xFFFF means the `lo`/`hi` literal is the bound.
    init_rows = []                 # (sequence, kind scene, op)
    init_fresh = {s: data["kind_init"][s]["fresh"] for s in kinds}
    init_reuse = {s: data["kind_init"][s]["reuse"] for s in kinds}
    for s in kinds:
        init_rows.extend(("fresh", s, op) for op in init_fresh[s])
    for s in kinds:
        init_rows.extend(("reuse", s, op) for op in init_reuse[s])

    def _init_row(idx, seq, s, op):
        def ref(value):
            return BT_INIT_NO_REF if value is None else idx + value
        lo_low, lo_high = _f64_words(op["lo"])
        hi_low, hi_high = _f64_words(op["hi"])
        add_low, add_high = _f64_words(op["add"])
        return ("    { %d, %d, %d, %d, %d, %d, %d, %d, %d, %d,"
                " %s, %s, %s, %s, %s, %s },"
                "  /* %s %s %s */" % (
                    op["op"], op["phase"], op["site"], op["dest"], op["behavior"],
                    op["state"], op["flags"], op["cond"], ref(op["lo_ref"]),
                    ref(op["hi_ref"]),
                    fmt_u32(lo_low), fmt_u32(lo_high), fmt_u32(hi_low),
                    fmt_u32(hi_high), fmt_u32(add_low), fmt_u32(add_high),
                    stem(s), _INIT_SITE_NAMES[op["site"]],
                    _INIT_DEST_NAMES[op["dest"]]))

    L.append("/* Fresh-vs-reuse RNG initialization (entity_spawner.gd:346-420).")
    L.append(" * A fresh node runs scene.instance() member initializers, the")
    L.append(" * `_ready` callbacks and init(); a pooled node only runs respawn().")
    L.append(" * Both branches draw on the GLOBAL GDScript `rand_range` stream, so")
    L.append(" * a spawn MUST replay exactly the sequence its branch took.")
    L.append(" *")
    L.append(" * PRECISION: `rand_range(a, b)` is Math::random((double)a,(double)b)")
    L.append(" * and the pinned build stores Variant REAL as a double, so every")
    L.append(" * bound here is binary64 and no result is narrowed (brotato_rng.h).")
    L.append(" * MSL has no native double, so lo/hi/add are carried as their exact")
    L.append(" * IEEE-754 binary64 bit patterns, low 32-bit word first: lo_low/")
    L.append(" * lo_high, hi_low/hi_high, add_low/add_high.  A float literal would")
    L.append(" * round a bound such as 2*PI or 0.1 and change the draw.")
    L.append(" * A row evaluates as:  v = lo + randd() * (hi - lo);  v += add;")
    L.append(" * all in FP64, with `lo`/`hi` the real source arguments and `add` the")
    L.append(" * base the source adds afterwards (`_actual = range_around_player +")
    L.append(" * rand_range(-rr, rr)` draws, then adds), which keeps the source's")
    L.append(" * FP64 rounding order.  BT_INITFLAG_ADD marks a row that adds a base")
    L.append(" * AFTER the draw -- set even when that base is 0, so the replay never")
    L.append(" * reorders the arithmetic or loses a -0.0; a row without the flag IS")
    L.append(" * the drawn value.  A row whose lo_ref/hi_ref name an earlier row uses")
    L.append(" * that row's retained per-instance value bits, not the lo/hi literal.")
    L.append(" * NOTE for the replay API: bt_rf64_range() takes binary32 bounds, so")
    L.append(" * the binary64 bit-word global rand_range (bt_rng_global_range_bits),")
    L.append(" * bt_rng_f64_add_bits and bt_rng_f64_trunc_bits are required here.")
    L.append(" *")
    L.append(" * BT_KIND_INIT_FRESH_* spans a kind's fresh sequence, BT_KIND_INIT_REUSE_*")
    L.append(" * its reuse sequence; both index BT_ENEMY_INIT_OPS in order.  Phase says")
    L.append(" * which branch owns the row (BT_INIT_PHASE_BOTH is the speed jitter,")
    L.append(" * re-rolled on both).  dest names the member the drawn value initializes")
    L.append(" * (see BT_INITDEST_*): MOV_ACTUAL/SPEED_RAND are int-typed in the source")
    L.append(" * (`var _actual: int`, `... as int`), so store the truncated integer and")
    L.append(" * reuse it for a later row's referenced bound (BT_INITFLAG_TRUNC).")
    L.append(" * A row with BT_INITFLAG_COND_SHOTS draws only when the instance's")
    L.append(" * `_shots_taken` already reached `cond` (shooting/charging get_cd's")
    L.append(" * long-cooldown branch returns `long_cooldown` without a draw).")
    L.append(" *")
    L.append(" * Kinds whose BT_KIND_CLASS is elite/boss are never pooled")
    L.append(" * (boss.gd:81-83 queue_free; Boss.respawn asserts), so their reuse")
    L.append(" * span is empty.  Neutrals extend Unit, not Enemy: no idle member draw")
    L.append(" * and no `_ready` attack draw -- their only init draw is the shared")
    L.append(" * speed jitter already replayed for BT_KINDS. */")
    L.append("#define BT_INIT_OP_RANGE 1")
    L.append("#define BT_INIT_PHASE_FRESH 0  /* scene.instance()/_ready/init only */")
    L.append("#define BT_INIT_PHASE_REUSE 1  /* respawn()/reset() only (pooled) */")
    L.append("#define BT_INIT_PHASE_BOTH  2  /* Unit speed jitter: both branches */")
    L.append("#define BT_INITSITE_MEMBER    0  /* enemy.gd:25 member initializer */")
    L.append("#define BT_INITSITE_MOV_READY 1  /* MovementBehavior._ready */")
    L.append("#define BT_INITSITE_MOV_INIT  2  /* MovementBehavior.init */")
    L.append("#define BT_INITSITE_ATK_READY 3  /* AttackBehavior._ready get_cd() */")
    L.append("#define BT_INITSITE_ATK_RESET 4  /* AttackBehavior.reset get_cd() */")
    L.append("#define BT_INITSITE_SPEED     5  /* Unit.init/respawn speed jitter */")
    L.append("#define BT_INITDEST_IDLE_PLAYBACK   0  /* _idle_playback_speed (float) */")
    L.append("#define BT_INITDEST_SPEED_RAND      1  /* _speed_rand_value (int) */")
    L.append("#define BT_INITDEST_ATK_CD          2  /* _current_cd, frames (float) */")
    L.append("#define BT_INITDEST_MOV_ACTUAL      3  /* `_actual` (int) */")
    L.append("#define BT_INITDEST_MOV_ACTUAL_RANGE 4 /* `_actual_target_range` (float) */")
    L.append("#define BT_INITDEST_MOV_PHASE       5  /* patrol `_phase` (float) */")
    L.append("#define BT_INITDEST_MOV_DIST_X      6  /* follow_rand `_distance.x` */")
    L.append("#define BT_INITDEST_MOV_DIST_Y      7  /* follow_rand `_distance.y` */")
    L.append("#define BT_INITBEH_UNIT                255")
    L.append("#define BT_INITBEH_MOV_BASE              0")
    L.append("#define BT_INITBEH_MOV_STAY_IN_RANGE     1")
    L.append("#define BT_INITBEH_MOV_FOLLOW_RAND_AROUND 2")
    L.append("#define BT_INITBEH_MOV_PATROL_AROUND_MAP 3")
    L.append("#define BT_INITBEH_MOV_PATROL_EDGES      4")
    L.append("#define BT_INITBEH_MOV_TARGET_RAND_AROUND 5")
    L.append("#define BT_INITBEH_MOV_DETECT_TARGET     6")
    L.append("#define BT_INITBEH_MOV_FOLLOW_TARGET     7")
    L.append("#define BT_INITBEH_MOV_GO_MAP_CENTER     8")
    L.append("#define BT_INITBEH_MOV_PLAYER            9")
    L.append("#define BT_INITBEH_MOV_COPY_NEARBY      10")
    L.append("#define BT_INITBEH_MOV_TARGET_RAND_GOLD 11")
    L.append("#define BT_INITBEH_MOV_TARGET_RAND_POS  12")
    L.append("#define BT_INITBEH_MOV_STARGAZER        13")
    L.append("#define BT_INITBEH_ATK_BASE       16")
    L.append("#define BT_INITBEH_ATK_SHOOTING   17")
    L.append("#define BT_INITBEH_ATK_CHARGING   18")
    L.append("#define BT_INITBEH_ATK_SPAWNING   19")
    L.append("#define BT_INITFLAG_NEG_LO     1  /* lo = -value[lo_ref] */")
    L.append("#define BT_INITFLAG_NEG_HI     2  /* hi = -value[hi_ref] */")
    L.append("#define BT_INITFLAG_TRUNC      4  /* store trunc(value) (int dest) */")
    L.append("#define BT_INITFLAG_COND_SHOTS 8  /* draw only if _shots_taken >= cond */")
    L.append("#define BT_INITFLAG_ADD       16  /* value = add + draw (even add == 0) */")
    L.append("#define BT_INIT_ROOT_STATE 0xFF  /* row belongs to the kind root */")
    L.append("#define BT_INIT_NO_REF   0xFFFF  /* bound is the lo/hi literal */")
    L.append("typedef struct {")
    L.append("    unsigned char op;        /* BT_INIT_OP_* */")
    L.append("    unsigned char phase;     /* BT_INIT_PHASE_* */")
    L.append("    unsigned char site;      /* BT_INITSITE_* */")
    L.append("    unsigned char dest;      /* BT_INITDEST_* */")
    L.append("    unsigned char behavior;  /* BT_INITBEH_* (provenance) */")
    L.append("    unsigned char state;     /* boss-state ordinal, 0xFF = root */")
    L.append("    unsigned char flags;     /* BT_INITFLAG_* */")
    L.append("    unsigned short cond;     /* COND_SHOTS threshold, else 0 */")
    L.append("    unsigned short lo_ref;   /* absolute op index, or BT_INIT_NO_REF */")
    L.append("    unsigned short hi_ref;")
    L.append("    unsigned int lo_low, lo_high;    /* lo bound, binary64 low/high word */")
    L.append("    unsigned int hi_low, hi_high;    /* hi bound, binary64 low/high word */")
    L.append("    unsigned int add_low, add_high;  /* base added after the draw (FP64) */")
    L.append("} BtEnemyInitOp;")
    n_fresh = sum(len(init_fresh[s]) for s in kinds)
    n_reuse = sum(len(init_reuse[s]) for s in kinds)
    L.append("#define BT_NINIT_FRESH %d" % n_fresh)
    L.append("#define BT_NINIT_REUSE %d" % n_reuse)
    L.append("#define BT_NINIT_OPS %d" % max(1, len(init_rows)))
    L.append("BT_TABLE BtEnemyInitOp BT_ENEMY_INIT_OPS[BT_NINIT_OPS] = {")
    for idx, (seq, s, op) in enumerate(init_rows):
        L.append(_init_row(idx, seq, s, op))
    if not init_rows:
        L.append("    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0u, 0u, 0u, 0u, 0u, 0u },")
    L.append("};")
    L.append("/* Per-kind spans into BT_ENEMY_INIT_OPS.  Fresh runs idle -> `_ready` ->")
    L.append(" * movement init -> speed jitter; reuse runs speed jitter -> root attack")
    L.append(" * reset.  Boss/elite kinds have an empty reuse span (never pooled):")
    L.append(" * when N == 0 the FIRST is BT_NINIT_OPS (one past the end) and must not")
    L.append(" * be dereferenced. */")
    L.append("BT_TABLE unsigned short BT_KIND_INIT_FRESH_FIRST[BT_NKINDS] = {")
    offset = 0
    row = []
    for s in kinds:
        row.append(offset)
        offset += len(init_fresh[s])
    L.append("    " + ", ".join(str(v) for v in row))
    L.append("};")
    L.append("BT_TABLE unsigned char BT_KIND_INIT_FRESH_N[BT_NKINDS] = {")
    L.append("    " + ", ".join(str(len(init_fresh[s])) for s in kinds))
    L.append("};")
    L.append("BT_TABLE unsigned short BT_KIND_INIT_REUSE_FIRST[BT_NKINDS] = {")
    row = []
    for s in kinds:
        row.append(offset)
        offset += len(init_reuse[s])
    L.append("    " + ", ".join(str(v) for v in row))
    L.append("};")
    L.append("BT_TABLE unsigned char BT_KIND_INIT_REUSE_N[BT_NKINDS] = {")
    L.append("    " + ", ".join(str(len(init_reuse[s])) for s in kinds))
    L.append("};")
    L.append("/* Largest per-kind init program (fresh rows then reuse rows).  A per-")
    L.append(" * instance retained-value array is BT_MAX_KIND_INIT_OPS words of binary64")
    L.append(" * and indexes a kind's rows in program order: fresh row i at [i], reuse")
    L.append(" * row j at [BT_KIND_INIT_FRESH_N[kind] + j].  The reuse half never")
    L.append(" * overwrites the fresh half, so a referenced fresh value (_actual) stays")
    L.append(" * live across a pooled respawn. */")
    max_init_ops = max((len(init_fresh[s]) + len(init_reuse[s]) for s in kinds),
                       default=1)
    L.append("#define BT_MAX_KIND_INIT_OPS %d" % max(1, max_init_ops))
    L.append("BT_HOST_ONLY(static const char* const BT_ENEMY_INIT_SRC[BT_NINIT_OPS] = {")
    for _idx, (_seq, _s, op) in enumerate(init_rows):
        L.append('    "%s",' % op["src"])
    if not init_rows:
        L.append('    "",')
    L.append("};)")

    # ---- neutrals ----------------------------------------------------------
    L.append("/* Neutral units (trees) -- separate domain, never a physics enemy kind. */")
    L.append("#define BT_NNEUTRALS %d" % len(neutrals))
    if neutrals:
        L.append("BT_TABLE BtUnitKind BT_NEUTRALS[BT_NNEUTRALS] = {")
        for scene in neutrals:
            s = data["kind_stats"][scene]
            L.append("    { %sf, %sf, %sf, %d, %sf, %sf, %sf, %d, %d, %sf, %sf, %sf, %sf, %sf, %d, %d, %d, %d },  /* %s */"
                     % (fmt_float(s["health"]), fmt_float(s["health_increase_each_wave"]),
                        fmt_float(s["speed"]), s["speed_randomization"],
                        fmt_float(s["damage"]), fmt_float(s["damage_increase_each_wave"]),
                        fmt_float(s["attack_cd"]), s["value"], s["gold_spread"],
                        fmt_float(s["knockback_resistance"]),
                        fmt_float(s["base_drop_chance"]), fmt_float(s["item_drop_chance"]),
                        fmt_float(s["armor"]), fmt_float(s["armor_increase_each_wave"]),
                        1 if s["can_drop_consumables"] else 0,
                        1 if s["always_drop_consumables"] else 0,
                        s["min_consumable_tier"], s["max_consumable_tier"], stem(scene)))
        L.append("};")
        L.append("BT_HOST_ONLY(static const char* const BT_NEUTRAL_NAME[BT_NNEUTRALS] = {")
        L.append("    " + ", ".join('"%s"' % stem(s) for s in neutrals) + "};)")
        L.append("BT_HOST_ONLY(static const char* const BT_NEUTRAL_SCENE[BT_NNEUTRALS] = {")
        L.append("    " + ", ".join('"%s"' % s for s in neutrals) + "};)")
    else:
        L.append("BT_TABLE BtUnitKind BT_NEUTRALS[1] = { {0} };")
    L.append("")

    # ---- elites / bosses ---------------------------------------------------
    def idx_list(reg, zone):
        return [data["kind_index"][scene] for _id, scene, _p in reg[zone]]

    max_elites = max(len(idx_list(data["elites"], z)) for z in (0, 1))
    max_bosses = max(len(idx_list(data["bosses"], z)) for z in (0, 1))
    L.append("/* Explicit per-zone elite/boss rosters (indices into BT_KINDS).")
    L.append(" * Bosses live in a SEPARATE list so they can never be chosen as elites. */")
    L.append("#define BT_MAX_ELITES %d" % max_elites)
    L.append("#define BT_MAX_BOSSES %d" % max_bosses)
    L.append("BT_TABLE unsigned short BT_ZONE_ELITES[BT_ZONES][BT_MAX_ELITES] = {")
    for z in (0, 1):
        row = idx_list(data["elites"], z)
        row += [0] * (max_elites - len(row))
        L.append("    { %s },  /* %s */" % (", ".join(str(i) for i in row),
                                            ", ".join(stem(s) for _i, s, _p in data["elites"][z])))
    L.append("};")
    L.append("BT_TABLE unsigned char BT_ZONE_NELITES[BT_ZONES] = { %s };"
             % ", ".join(str(len(idx_list(data["elites"], z))) for z in (0, 1)))
    L.append("BT_TABLE unsigned short BT_ZONE_BOSSES[BT_ZONES][BT_MAX_BOSSES] = {")
    for z in (0, 1):
        row = idx_list(data["bosses"], z)
        row += [0] * (max_bosses - len(row))
        L.append("    { %s },  /* %s */" % (", ".join(str(i) for i in row),
                                            ", ".join(stem(s) for _i, s, _p in data["bosses"][z])))
    L.append("};")
    L.append("BT_TABLE unsigned char BT_ZONE_NBOSSES[BT_ZONES] = { %s };"
             % ", ".join(str(len(idx_list(data["bosses"], z))) for z in (0, 1)))
    L.append("")
    L.append("/* Elite/horde schedule (run_data.gd:init_elites_spawn, base_wave=10).")
    L.append(" * Elite i spawns at wave base_wave + [lo..hi]; events 0/1 are HORDES")
    L.append(" * with probability BT_ELITE_HORDE_CHANCE, event 2 is always an elite.")
    L.append(" * Count per difficulty: <2 -> 0, 2-3 -> 1, >=4 -> 3. */")
    L.append("#define BT_ELITE_BASE_WAVE 10")
    L.append("#define BT_ELITE_HORDE_CHANCE 0.4f")
    L.append("#define BT_ELITE_EVENTS 3")
    L.append("BT_TABLE unsigned char BT_ELITE_NB_BY_DIFFICULTY[6] = { 0, 0, 1, 1, 3, 3 };")
    L.append("BT_TABLE unsigned char BT_ELITE_WAVE_LO[BT_ELITE_EVENTS] = { 1, 4, 7 };")
    L.append("BT_TABLE unsigned char BT_ELITE_WAVE_HI[BT_ELITE_EVENTS] = { 2, 5, 8 };")
    L.append("BT_TABLE unsigned char BT_ELITE_FORCED_ELITE[BT_ELITE_EVENTS] = { 0, 0, 1 };")
    L.append("")

    # ---- spawn groups / units / waves -------------------------------------
    L.append("/* Wave unit (entities/units/enemies/wave_unit_data.gd).")
    L.append(" * domain 0 -> index in BT_KINDS, domain 1 -> index in BT_NEUTRALS. */")
    L.append("typedef struct {")
    L.append("    unsigned char domain;         /* 0 = BT_KINDS, 1 = BT_NEUTRALS */")
    L.append("    unsigned char type;           /* WaveUnitData.Type */")
    L.append("    unsigned short index;")
    L.append("    unsigned char min_n, max_n;")
    L.append("    float chance;                 /* spawn_chance */")
    L.append("    float additional_min_distance_from_player;  /* + BT_SPAWN_MIN_DIST */")
    L.append("} BtSpawnUnit;")
    L.append("")
    L.append("/* Wave spawn group (zones/wave_group_data.gd), all fields preserved. */")
    L.append("typedef struct {")
    L.append("    unsigned char zone;           /* owning zone (unit spans are zone-local) */")
    L.append("    unsigned short timing;        /* spawn_timing (s) */")
    L.append("    float chance;                 /* spawn_chance */")
    L.append("    short repeat;                 /* repeating (999 or -1) */")
    L.append("    float repeat_interval;")
    L.append("    float reduce_interval;        /* reduce_repeating_interval */")
    L.append("    float min_interval;           /* min_repeating_interval */")
    L.append("    short area;                   /* -1 = anywhere, else radius */")
    L.append("    unsigned char spawn_edge_of_map;")
    L.append("    unsigned int spawn_dist_away_from_edges;")
    L.append("    unsigned char is_neutral, is_boss, is_horde, is_loot;")
    L.append("    unsigned char min_difficulty; /* D5 (5) runs every group with min<=5 */")
    L.append("    unsigned int min_wave, max_wave;")
    L.append("    unsigned short unit_first, unit_count; /* zone-local into units */")
    L.append("    unsigned short group_id;      /* stable index into BT_GROUPS */")
    L.append("} BtSpawnGroup;")
    L.append("")
    L.append("/* Per-wave header.  group_first/n_groups index BT_GROUPS; conditional")
    L.append(" * groups are the cond_first/n_cond span.  A group's units live at")
    L.append(" * BT_ZONE_UNIT(group.zone, group.unit_first + i). */")
    L.append("typedef struct {")
    L.append("    unsigned short duration;")
    L.append("    unsigned short max_enemies;   /* 100 in every real wave */")
    L.append("    unsigned short group_first;  unsigned char n_groups;")
    L.append("    unsigned short cond_first;   unsigned char n_cond;")
    L.append("} BtWave;")
    L.append("")
    L.append("#define BT_NGROUPS %d" % len(groups))
    L.append("#define BT_NUNITS %d" % len(unit_lines))
    L.append("#define BT_ZONE_UNITS_TOTAL BT_NUNITS")
    L.append("#define BT_MAX_GROUPS %d" % max(
        max((r[3] for r in rows), default=0)
        for rows in zone_wave_rows.values()))
    L.append("#define BT_NCOND %d" % sum(r[5] for r in
                                        zone_wave_rows[0] + zone_wave_rows[1]))
    L.append("BT_TABLE BtSpawnGroup BT_GROUPS[BT_NGROUPS] = {")
    for e in groups:
        g = e["row"]
        L.append("    { %d, %d, %sf, %d, %sf, %sf, %sf, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d },  /* %s */"
                 % (e["zone"], g["timing"], fmt_float(g["chance"]), g["repeat"],
                    fmt_float(g["repeat_interval"]), fmt_float(g["reduce_interval"]),
                    fmt_float(g["min_interval"]), g["area"],
                    1 if g["spawn_edge"] else 0, g["spawn_dist"],
                    1 if g["is_neutral"] else 0, 1 if g["is_boss"] else 0,
                    1 if g["is_horde"] else 0, 1 if g["is_loot"] else 0,
                    g["min_difficulty"], g["min_wave"], g["max_wave"],
                    e["unit_first"], e["unit_count"], e["id"],
                    g["src"].split("/")[-1]))
    L.append("};")
    L.append("/* Flattened per-zone unit roster: zone z owns")
    L.append(" * BT_ZONE_UNITS_ALL[BT_ZONE_UNITS_OFF[z] .. +BT_ZONE_UNITS_OFF[z+1]). */")
    L.append("BT_TABLE BtSpawnUnit BT_ZONE_UNITS_ALL[BT_NUNITS] = {")
    L.extend(unit_lines if unit_lines else ["    { 0, 0, 0, 0, 0, 0.0f, 0.0f }"])
    L.append("};")
    L.append("BT_TABLE unsigned int BT_ZONE_UNITS_OFF[BT_ZONES + 1] = { %d, %d, %d };"
             % (unit_off[0], unit_off[1], len(unit_lines)))
    L.append("/* Legacy host pointer view (dropped on device; use BT_ZONE_UNIT there). */")
    L.append("BT_HOST_ONLY(static const BtSpawnUnit* const BT_ZONE_UNITS[BT_ZONES] = {")
    L.append("    BT_ZONE_UNITS_ALL, BT_ZONE_UNITS_ALL + %d };)" % unit_off[1])
    L.append("")
    L.append("BT_TABLE BtWave BT_ZONE_WAVES[BT_ZONES][BT_WAVES_MAX] = {")
    for zi, zone in enumerate(data["zones"]):
        L.append("  {  /* zone %d */" % zi)
        for wi in range(nwaves):
            if wi < len(zone["waves"]):
                r = zone_wave_rows[zi][wi]
                L.append("    { %d, %d, %d, %d, %d, %d },  /* wave %d */"
                         % (r[0], r[1], r[2], r[3], r[4], r[5], wi + 1))
            else:
                L.append("    { 0, 0, 0, 0, 0, 0 },")
        L.append("  },")
    L.append("};")
    L.append("")
    cond_list = []
    for z in (0, 1):
        for r in zone_wave_rows[z]:
            if r[5]:
                cond_list.extend(range(r[4], r[4] + r[5]))
    L.append("BT_TABLE unsigned short BT_COND_GROUPS[%d] = { %s };"
             % (max(1, len(cond_list)),
                ", ".join(str(i) for i in cond_list) if cond_list else "0"))
    L.append("")

    def emit_idx(name, mapping):
        width = max(len(v) for v in mapping.values())
        L.append("BT_TABLE unsigned short %s[BT_ZONES][%d] = {" % (name, max(1, width)))
        for z in (0, 1):
            row = mapping[z] + [0] * (width - len(mapping[z]))
            L.append("    { %s }," % ", ".join(str(i) for i in row))
        L.append("};")
        L.append("BT_TABLE unsigned char %s_N[BT_ZONES] = { %s };"
                 % (name, ", ".join(str(len(mapping[z])) for z in (0, 1))))
    L.append("/* Global groups applied to every wave (zone_data.groups_data_in_all_waves). */")
    emit_idx("BT_ZONE_ALLWAVE_GROUPS", all_wave_idx)
    L.append("/* Horde group sets (used when an elite event rolls a HORDE). */")
    emit_idx("BT_ZONE_HORDE_GROUPS", horde_idx)
    L.append("/* Loot-alien groups (extra_loot_aliens effects). */")
    emit_idx("BT_ZONE_LOOT_GROUPS", loot_idx)
    L.append("/* DLC groups that spawn in EVERY zone (wave_manager.gd:151-154);")
    L.append(" * one copy per zone, added to each zone's group stream. */")
    emit_idx("BT_ZONE_ALLZONE_GROUPS", allzone_idx)
    L.append("")
    L.append("/* Item-granted extra groups (ExtraEnemyEffect.extra_group_data, e.g. the")
    L.append(" * bait item).  BtItemEff.extra_group holds the ORDINAL into this table;")
    L.append(" * the core resolves BT_ITEM_EXTRA_GROUPS[ordinal][zone] to a BT_GROUPS")
    L.append(" * row.  One row per zone (like the DLC all-zone groups) so each copy's")
    L.append(" * unit span stays inside its own zone slice; wave_manager.gd:114-121")
    L.append(" * appends the resolved group `BtItemEff.value` times to the next wave's")
    L.append(" * group list.  Ordinals are stable: paths sorted lexically. */")
    L.append("#define BT_NITEM_EXTRA_GROUPS %d" % max(1, len(data["extra_groups"])))
    L.append("BT_TABLE unsigned short BT_ITEM_EXTRA_GROUPS[BT_NITEM_EXTRA_GROUPS][BT_ZONES] = {")
    for ordinal in range(len(data["extra_groups"])):
        L.append("    { %d, %d },  /* [%d] %s  <- %s */"
                 % (item_extra_idx[ordinal][0], item_extra_idx[ordinal][1],
                    ordinal, data["extra_group_specs"][ordinal]["path"],
                    ", ".join(data["extra_group_specs"][ordinal]["sources"])))
    if not data["extra_groups"]:
        L.append("    { 0, 0 },  /* no item extra groups */")
    L.append("};")
    L.append("BT_HOST_ONLY(static const char* const BT_ITEM_EXTRA_GROUP_PATH[BT_NITEM_EXTRA_GROUPS] = {")
    for ordinal in range(len(data["extra_groups"])):
        L.append('    "%s",' % data["extra_group_specs"][ordinal]["path"])
    if not data["extra_groups"]:
        L.append('    "",')
    L.append("};)")
    L.append("")
    # Worst-case number of groups live at once in a single wave (for per-wave
    # live-group tracking state).  Derived from the per-list maxima so it
    # re-derives when the data changes.
    L.append("/* Per-list maxima + worst-case concurrently-live group count.")
    L.append(" * HORDE and LOOT are NOT mutually exclusive: a horde event (waves")
    L.append(" * 11-12/14-15) can coincide with extra_loot_aliens effects, so both")
    L.append(" * sets are summed.  This counts table-sourced groups only; the runtime")
    L.append(" * additionally appends the elite/boss group and item-granted")
    L.append(" * extra-group copies (BT_ITEM_EXTRA_GROUPS via BtItemEff.extra_group),")
    L.append(" * which callers must add on top if they track those too. */")
    L.append("#define BT_MAX_GROUPS_COND %d" % max(
        (r[5] for rows in zone_wave_rows.values() for r in rows), default=0))
    L.append("#define BT_ZONE_ALLWAVE_GROUPS_MAX %d"
             % max((len(v) for v in all_wave_idx.values()), default=0))
    L.append("#define BT_ZONE_ALLZONE_GROUPS_MAX %d"
             % max((len(v) for v in allzone_idx.values()), default=0))
    L.append("#define BT_ZONE_HORDE_GROUPS_MAX %d"
             % max((len(v) for v in horde_idx.values()), default=0))
    L.append("#define BT_ZONE_LOOT_GROUPS_MAX %d"
             % max((len(v) for v in loot_idx.values()), default=0))
    L.append("/* ENDLESS generated groups (zone_service.gd:47 nb = int((wave / 10.0) * 2)).")
    L.append(" * The source list is unbounded; the sim sizes for waves through")
    L.append(" * BT_ENDLESS_WAVE_BOUND and raises BtState.group_overflow loudly beyond it.")
    L.append(" * SUPPORTED RANGE: nb = int(w * 0.2) is exact while the WHOLE live list")
    L.append(" * stays under BT_MAX_LIVE_GROUPS (BT_MAX_DYNGROUPS reserves the")
    L.append(" * generation BT_ENDLESS_WAVE_BOUND-era waves need); every generated group")
    L.append(" * past that cap is DROPPED with BtState.group_overflow set -- loud, never")
    L.append(" * a silent clamp; exact spill for deeper endless runs belongs to")
    L.append(" * brotato_capacity.h. */")
    L.append("#define BT_ENDLESS_WAVE_BOUND 200")
    L.append("#define BT_MAX_DYNGROUPS ((BT_ENDLESS_WAVE_BOUND / 10) * 2)")
    L.append("#define BT_MAX_LIVE_GROUPS (BT_MAX_GROUPS + BT_MAX_GROUPS_COND \\")
    L.append("    + BT_ZONE_ALLWAVE_GROUPS_MAX + BT_ZONE_ALLZONE_GROUPS_MAX \\")
    L.append("    + BT_ZONE_HORDE_GROUPS_MAX + BT_ZONE_LOOT_GROUPS_MAX \\")
    L.append("    + BT_MAX_DYNGROUPS)")
    L.append("")
    L.append("/* Wave durations (seconds), flat convenience view. */")
    L.append("BT_TABLE unsigned char BT_ZONE_WAVE_DUR[BT_ZONES][BT_WAVES_MAX] = {")
    for zi, zone in enumerate(data["zones"]):
        row = [w["duration"] for w in zone["waves"]]
        row += [0] * (nwaves - len(row))
        L.append("    { %s }," % ", ".join(str(d) for d in row))
    L.append("};")
    L.append("")

    # ---- endless wave resources -------------------------------------------
    # ZoneService.get_wave_data (zone_service.gd:40-68) generates every wave
    # past the zone's 20 authored waves from a BASE authored wave, and
    # get_additional_groups (zone_service.gd:70-92) appends extra groups built
    # from the two singleton templates + the zone's endless_enemy_scenes.  The
    # runtime overwrites the copy's spawn_timing/spawn_edge_of_map/area/
    # repeating_interval and the unit copy's unit_scene/min_number/max_number,
    # so only the templates' constant fields and the per-zone scene roster are
    # data.  main.gd:484-491 uses the same generator with wave_duration 90 for
    # the wave-20 boss-kill transition groups.
    eg = data["endless_group"]
    eu = data["endless_unit"]
    L.append("/* Endless wave resources (zone_service.tscn: endless_groups =")
    L.append(" * [zones/common/endless/group_endless.tres], endless_unit_groups =")
    L.append(" * [zones/common/endless/unit_endless.tres]; each zone data resource's")
    L.append(" * endless_enemy_scenes).  ZoneService.get_additional_groups builds every")
    L.append(" * extra group from these two templates; the per-copy fields are overwritten")
    L.append(" * at runtime, so only these constants and the roster are data. */")
    L.append("BT_TABLE BtSpawnGroup BT_ENDLESS_GROUP_TMPL[1] = {")
    L.append("    { 0, %d, %sf, %d, %sf, %sf, %sf, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d },  /* %s */"
             % (eg["timing"], fmt_float(eg["chance"]), eg["repeat"],
                fmt_float(eg["repeat_interval"]), fmt_float(eg["reduce_interval"]),
                fmt_float(eg["min_interval"]), eg["area"],
                1 if eg["spawn_edge"] else 0, eg["spawn_dist"],
                1 if eg["is_neutral"] else 0, 1 if eg["is_boss"] else 0,
                1 if eg["is_horde"] else 0, 1 if eg["is_loot"] else 0,
                eg["min_difficulty"], eg["min_wave"], eg["max_wave"],
                0, 0, 0, eg["src"].split("/")[-1]))
    L.append("};")
    L.append("/* unit_endless.tres: the runtime assigns unit_scene (-> BT_ZONE_ENDLESS_KINDS)")
    L.append(" * and min_number/max_number per copy (zone_service.gd:84-89). */")
    L.append("BT_TABLE BtSpawnUnit BT_ENDLESS_UNIT_TMPL[1] = {")
    L.append("    { 0, %d, 0, %d, %d, %sf, %sf },  /* %s */"
             % (eu["type"], eu["min_n"], eu["max_n"], fmt_float(eu["chance"]),
                fmt_float(eu["additional_min_distance_from_player"]),
                eu["src"].split("/")[-1]))
    L.append("};")
    n_endless = max(len(z["endless_scenes"]) for z in data["zones"])
    L.append("#define BT_MAX_ENDLESS_KINDS %d" % n_endless)
    L.append("/* zone_*_data.tres endless_enemy_scenes, in source order, as BT_KINDS indices. */")
    L.append("BT_TABLE unsigned short BT_ZONE_ENDLESS_KINDS[BT_ZONES][BT_MAX_ENDLESS_KINDS] = {")
    for z in data["zones"]:
        row = [data["kind_index"][s] for s in z["endless_scenes"]]
        row += [0] * (n_endless - len(row))
        L.append("    { %s },  /* %s */" % (", ".join(str(i) for i in row),
                                            ", ".join(stem(s) for s in z["endless_scenes"])))
    L.append("};")
    L.append("BT_TABLE unsigned char BT_ZONE_NENDLESS_KINDS[BT_ZONES] = { %s };"
             % ", ".join(str(len(z["endless_scenes"])) for z in data["zones"]))
    L.append("/* zone_service.gd:48-49: every generated wave lasts 60 s and scales the")
    L.append(" * base wave's max_enemies by (1.25 + wave_index * 0.01); main.gd:487 uses")
    L.append(" * wave_duration 90 for the wave-20 boss-kill transition groups. */")
    L.append("#define BT_ENDLESS_DURATION 60")
    L.append("#define BT_ENDLESS_TRANSITION_DURATION 90")
    L.append("#define BT_ENDLESS_TRANSITION_GROUPS_PER_TEN 3")
    L.append("#define BT_ENDLESS_MAXENEMY_BASE 1.25f")
    L.append("#define BT_ENDLESS_MAXENEMY_PER_INDEX 0.01f")
    L.append("")
    L.append("#endif /* BROTATO_WAVES_H */")
    L.append("")
    with open(out_path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(L))


def emit_attacks(data, out_path):
    stem = data["scene_stem"]
    kinds = data["enemy_kinds"]
    states = data["kind_states"]

    L = []
    L.append("/* Per-kind attack configuration, extracted from each unit's scene")
    L.append(" * (entities/units/enemies/attack_behaviors/<kind>.gd + AttackBehavior nodes).")
    L.append(" * Generated by ocean/brotato/extract_gameplay.py.  Rows align with")
    L.append(" * BT_KINDS in brotato_waves.h.")
    L.append(" *")
    L.append(" * beh: 0 none, 1 shooting, 2 charging, 3 spawning.")
    L.append(" * proj_shape: 0 circle bullet, 1 pillar, 2 slash, 3 other.  The sloped")
    L.append(" *   value comes from the projectile scene's own collision shape, not a")
    L.append(" *   circular-bullet assumption.")
    L.append(" * charge_target (TargetType): 0 PLAYER, 1 RAND_POINT_AROUND_PLAYER, 2 RAND_POINT.")
    L.append(" * charge_timing (Timing): 0 START_SHOOT, 1 SHOOT -- when the charge")
    L.append(" *   direction is locked (see charge_dir_lock).")
    L.append(" */")
    L.append("#ifndef BROTATO_ATTACKS_H")
    L.append("#define BROTATO_ATTACKS_H")
    L.append("#include \"brotato_waves.h\"")
    L.append("")
    L.extend(_attack_struct_lines())
    L.append("")
    L.append("typedef struct { BtAttack atk; float hp_start, timer_start; } BtAtkState;")
    L.append("")
    max_states = max((len(states[s]) for s in kinds), default=0)
    max_states = max(1, max_states)
    L.append("#define BT_MAX_STATES %d" % max_states)
    L.append("#define BT_ATK_STATES BT_MAX_STATES  /* compat alias */")
    L.append("/* Escalating states (boss_state.gd): state i activates when")
    L.append(" * health <= max_health*hp_start OR elapsed_time >= timer_start. */")
    L.append("#define BT_NATTACKS %d" % len(kinds))

    # specific_degrees_spawns -> flat degree table (spans recorded per attack)
    degrees = []
    def degree_span(a):
        if a is None or not a.get("degrees"):
            return 0, 0
        first = len(degrees)
        degrees.extend(a["degrees"])
        return first, len(a["degrees"])

    for scene in kinds:
        for a in [data["kind_attack"].get(scene)] + \
                 [s["attack"] for s in states[scene]]:
            if a is None:
                continue
            first, count = degree_span(a)
            a["degree_first"] = first
            a["n_degrees"] = count

    L.append("/* specific_degrees_spawns values (degrees), referenced by BT_ATTACKS. */")
    L.append("#define BT_NDEGREES %d" % len(degrees))
    L.append("BT_TABLE float BT_ATTACK_DEGREES[%d] = { %s };"
             % (max(1, len(degrees)),
                ", ".join("%sf" % fmt_float(d) for d in degrees) if degrees else "0.0f"))
    L.append("")
    # ---- animated slash sweep (slash_projectile*.tscn anims/shoot) ---------
    keys = data["slash_keys"]
    rows = data["slash_rows"]
    L.append("/* Animated slash sweep: the Hitbox/Collision shape the `shoot`")
    L.append(" * animation drives.  AnimationPlayer autoplays `shoot` (IDLE process")
    L.append(" * mode: its position advances once per idle step, so the collision test")
    L.append(" * of a frame sees the position written by the previous idle step).  The")
    L.append(" * value track Hitbox/Collision:position is interp=1 (LINEAR) and")
    L.append(" * update=0 (CONTINUOUS): the keyframes below interpolate linearly, and")
    L.append(" * before the first key the shape keeps the scene's rest position.  The")
    L.append(" * scene's Hitbox/Collision:shape:extents track is INERT in the pinned")
    L.append(" * build (measured: instancing the scene and sampling the CollisionShape2D")
    L.append(" * every physics frame keeps the RectangleShape2D extents constant), so")
    L.append(" * ext_x/ext_y are the CONSTANT half extents from the same shape the")
    L.append(" * attack table's proj_half_x/proj_half_y carry.  node_x/node_y is the")
    L.append(" * Hitbox node position the Collision offset lives under. */")
    L.append("#define BT_NSWEEPS %d" % max(1, len(rows)))
    L.append("#define BT_SWEEP_KEYS %d" % max(1, len(keys)))
    def _keys_row(field):
        vals = ", ".join("%sf" % fmt_float(k[field]) for k in keys)
        return vals if vals else "0.0f"
    L.append("BT_TABLE float BT_SWEEP_KEY_TIME[BT_SWEEP_KEYS] = { %s };" % _keys_row(0))
    L.append("BT_TABLE float BT_SWEEP_KEY_X[BT_SWEEP_KEYS] = { %s };" % _keys_row(1))
    L.append("BT_TABLE float BT_SWEEP_KEY_Y[BT_SWEEP_KEYS] = { %s };" % _keys_row(2))
    L.append("typedef struct {")
    L.append("    float node_x, node_y;   /* Hitbox node position (root local) */")
    L.append("    float rest_x, rest_y;   /* Collision position before the first key */")
    L.append("    float ext_x, ext_y;     /* CONNECTED half extents (inert extents track) */")
    L.append("} BtSlashSweep;")
    L.append("BT_TABLE BtSlashSweep BT_SLASH_SWEEPS[BT_NSWEEPS] = {")
    if rows:
        for _res, label, spec in rows:
            L.append("    { %sf, %sf, %sf, %sf, %sf, %sf },  /* %s */"
                     % (fmt_float(spec["node"][0]), fmt_float(spec["node"][1]),
                        fmt_float(spec["rest"][0]), fmt_float(spec["rest"][1]),
                        fmt_float(spec["extents"][0]), fmt_float(spec["extents"][1]),
                        label))
    else:
        L.append("    { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },")
    L.append("};")
    L.append("")
    L.append("BT_TABLE BtAttack BT_ATTACKS[BT_NATTACKS] = {")
    for scene in kinds:
        L.append(_attack_row(data["kind_attack"].get(scene), stem(scene)))
    L.append("};")
    L.append("")
    L.append("BT_TABLE unsigned char BT_NKIND_STATES[BT_NATTACKS] = {")
    L.append("    " + ", ".join(str(len(states[s])) for s in kinds))
    L.append("};")
    L.append("BT_TABLE BtAtkState BT_ATTACK_STATES[BT_NATTACKS][BT_MAX_STATES] = {")
    for scene in kinds:
        sts = states[scene]
        L.append("    {  /* %s */" % stem(scene))
        for st in sts:
            L.append("      { %s, %sf, %sf },"
                     % (_attack_row(st["attack"], "", inline=True),
                        fmt_float(st["hp_start"]), fmt_float(st["timer_start"])))
        for _ in range(max_states - len(sts)):
            L.append("      { %s, 0.0f, 0.0f },"
                     % _attack_row(None, "", inline=True))
        L.append("    },")
    L.append("};")
    L.append("")
    L.append("#endif /* BROTATO_ATTACKS_H */")
    L.append("")
    with open(out_path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(L))


_ZERO_ATTACK = {
    "beh": BEH_NONE, "nproj": 0, "cooldown_f": 0, "pspeed": 0, "initial_cd_f": 0,
    "proj_shape": SHAPE_BULLET, "spread": 0.0, "constant_spread_rand_base_pos": 0.0,
    "spread_const": 0, "spawn_target": 0, "proj_sweep": 0,
    "rotate_proj": 1, "spawn_spread": 0, "random_dir": 0, "damage": 0.0,
    "damage_grow": 0.0, "charge_duration": 0.0, "charge_speed": 0.0,
    "charge_target": 0, "charge_timing": 0, "charge_dir_lock": 0, "spawn_kind": -1,
    "proj_speed_rand": 0, "speed_change_each": 0, "base_dir_rand": 0.0,
    "base_dir_const": 0, "alt_base_dir_spread": 0, "spawn_only_borders": 0,
    "atleast_one_on_target": 0, "shoot_towards_unit": 0, "shoot_in_unit_dir": 0,
    "shoot_away_from_unit": 0, "shoot_from_proj_pos": 0, "random_rotation": 0.0,
    "delete_on_death": 0, "long_cd_every": 0, "long_cooldown": 0, "max_cd_rand": 0,
    "min_range": 0, "max_range": 0, "rand_target_size": 0,
    "scale_charge_with_range": 0, "spawn_at_random_pos": 0, "spawn_radius": 0,
    "max_nb_spawns": 0, "proj_radius": 0.0, "proj_half_x": 0.0, "proj_half_y": 0.0,
    "n_degrees": 0, "degree_first": 0,
    "shoot_at_f": 0, "anim_len_f": 0, "kill_self_on_spawn": 0,
}


def _attack_row(a, label, inline=False):
    if a is None:
        a = _ZERO_ATTACK
    parts = [_num(a[name]) for name, _t in _ATTACK_INT_FIELDS]
    parts += [fmt_float(_num(a[name])) for name in _ATTACK_FLOAT_FIELDS]
    row = "{ " + ", ".join(str(v) for v in parts) + " }"
    if inline:
        return row
    return "    %s,  /* %s */" % (row, label)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--campaign-root", required=True)
    ap.add_argument("--dlc-root", required=True)
    ap.add_argument("--waves-out", default="ocean/brotato/brotato_waves.h")
    ap.add_argument("--attacks-out", default="ocean/brotato/brotato_attacks.h")
    ap.add_argument("--report", action="store_true")
    args = ap.parse_args(argv)

    data = build(args)
    validate(data)
    emit_waves(data, args.waves_out)
    emit_attacks(data, args.attacks_out)
    sys.stderr.write("extract_gameplay: %d kinds (%d elite/boss), %d neutrals, "
                     "%d zones, %d item extra groups -> %s, %s\n"
                     % (len(data["enemy_kinds"]),
                        sum(1 for s in data["enemy_kinds"] if data["kind_class"][s]),
                        len(data["neutral_kinds"]), len(data["zones"]),
                        len(data["extra_groups"]),
                        args.waves_out, args.attacks_out))
    if args.report:
        report(data)
    return 0


def report(data):
    stem = data["scene_stem"]
    for zone in data["zones"]:
        print("zone %d: %d waves" % (zone["zone_id"], len(zone["waves"])))
        for wi, wave in enumerate(zone["waves"]):
            ng = len(wave["groups"])
            print("  wave %2d dur=%3d max=%3d groups=%2d cond=%d"
                  % (wi + 1, wave["duration"], wave["max_enemies"], ng,
                     len(wave["conditional"])))
    print("kinds:")
    for scene in data["enemy_kinds"]:
        print("  %-18s class=%d zone=%s" % (stem(scene), data["kind_class"][scene],
                                            data["kind_zone"][scene]))
    print("enemy init (phase: site/dest[behavior@state]):")
    for scene in data["enemy_kinds"]:
        init = data["kind_init"][scene]

        def order(seq):
            return " ".join(
                "%s:%s/%s[%s%s]" % (_INIT_PHASE_NAMES[op["phase"]],
                                    _INIT_SITE_NAMES[op["site"]],
                                    _INIT_DEST_NAMES[op["dest"]],
                                    _INIT_BEH_NAMES[op["behavior"]],
                                    ("@s%d" % op["state"])
                                    if op["state"] != BT_INIT_ROOT_STATE else "")
                for op in seq)

        print("  %-18s fresh[%d]: %s" % (stem(scene), len(init["fresh"]),
                                         order(init["fresh"])))
        if init["reuse"]:
            print("  %-18s reuse[%d]: %s" % ("", len(init["reuse"]),
                                             order(init["reuse"])))
    for z in (0, 1):
        print("zone %d elites: %s" % (z, [i for i, _s, _p in data["elites"][z]]))
        print("zone %d bosses: %s" % (z, [i for i, _s, _p in data["bosses"][z]]))
    for ordinal, spec in enumerate(data["extra_group_specs"]):
        print("item extra group [%d] %s <- %s"
              % (ordinal, spec["path"], ", ".join(spec["sources"])))


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
