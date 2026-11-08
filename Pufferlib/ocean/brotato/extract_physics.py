#!/usr/bin/env python3
"""Extract Brotato enemy/player 2D physics geometry into ``brotato_geometry.h``.

Everything in the emitted header is derived from the real Godot 3.x scene
sources (``*.tscn`` / ``*.tres``) recovered from the retail packs -- no rows are
hand-authored.  The generator resolves:

  * PackedScene inheritance (``[node ... instance=ExtResource(n)]``) across the
    campaign root and the Abyss DLC root, following every ``ExtResource`` /
    ``SubResource`` reference and every node property override;
  * ``CollisionShape2D`` -> ``CircleShape2D`` radii, including the engine
    constructor default (10.0) when ``radius`` is not stored in the scene;
  * root-local circle centres by composing the ``position`` of every node on
    the path from the scene root down to the shape node;
  * ``RigidBody2D.mass`` / ``RigidBody2D.mode`` (engine default mass 1.0);
  * ``Stats.knockback_resistance`` from the scene's ``stats`` resource;
  * the ``BT_NEUTRALS`` neutral scenes (the two zone trees), resolved verbatim
    from ``BT_NEUTRAL_SCENE`` and emitted as a separate ``BT_NEUTRAL_PHYSICS``
    table so a neutral kind can never be mistaken for an enemy ``BT_KINDS`` row.

Anything the resolver cannot prove from the sources aborts with a non-zero exit
status instead of falling back to a silent default.  Transforms it cannot fully
collapse (non-zero rotation/skew on a shape path) are reported as explicit
limitations in the generated header.

Usage::

    python3 ocean/brotato/extract_physics.py \\
        --campaign-root /Users/me/brotato_recovered \\
        --dlc-root      /Users/me/brotato_abyss \\
        --waves         ocean/brotato/brotato_waves.h \\
        --out           ocean/brotato/brotato_geometry.h
"""

import argparse
import os
import re
import sys

# --------------------------------------------------------------------------
# Engine constants (Godot 3.x).  Source: scene/resources/circle_shape_2d.cpp
# CircleShape2D::CircleShape2D() sets radius = 10; RigidBody2D::RigidBody2D()
# leaves mass at its 1.0 default.
# --------------------------------------------------------------------------
CIRCLE_DEFAULT_RADIUS = 10.0
RIGIDBODY_DEFAULT_MASS = 1.0


class ExtractError(Exception):
    """Raised for any unresolved / unsupported source condition."""


# --------------------------------------------------------------------------
# .tscn / .tres tokenizer
# --------------------------------------------------------------------------
_EXT_RE = re.compile(r'^\[ext_resource\s+(.*)\]\s*$')
_SUB_RE = re.compile(r'^\[sub_resource\s+(.*)\]\s*$')
_NODE_RE = re.compile(r'^\[node\s+(.*)\]\s*$')
_RES_RE = re.compile(r'^\[resource\]\s*$')
_ATTR_RE = re.compile(r'([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(?:"([^"]*)"|([^\s\]]+))')
_INSTANCE_RE = re.compile(r'instance\s*=\s*ExtResource\(\s*(\d+)\s*\)')
_PROP_RE = re.compile(r'^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*)$')
_EXTREF_RE = re.compile(r'^ExtResource\(\s*(\d+)\s*\)$')
_SUBREF_RE = re.compile(r'^SubResource\(\s*(\d+)\s*\)$')
_VEC2_RE = re.compile(r'^Vector2\(\s*([-\d.eE+]+)\s*,\s*([-\d.eE+]+)\s*\)$')


def _attrs(text):
    return {m.group(1): (m.group(2) if m.group(2) is not None else m.group(3))
            for m in _ATTR_RE.finditer(text)}


class ParsedFile(object):
    """One parsed ``.tscn`` or ``.tres`` file."""

    def __init__(self, path):
        self.path = path
        self.kind = None          # "scene" | "resource"
        self.ext = {}             # id -> (res_path, type)
        self.subs = {}            # id -> {"type": str, "props": {str: str}}
        self.nodes = []           # [{"name","type","parent","index","instance","props"}]
        self.root_props = {}      # for .tres: the [resource] block

    def ext_path(self, eid):
        try:
            return self.ext[int(eid)][0]
        except KeyError:
            raise ExtractError("%s: unresolved ExtResource( %s )" % (self.path, eid))


def parse_file(path):
    """Parse a ``.tscn``/``.tres`` file into a :class:`ParsedFile`."""
    parsed = ParsedFile(path)
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        lines = handle.read().splitlines()

    i = 0
    while i < len(lines):
        line = lines[i].strip()
        if line.startswith("[gd_scene"):
            parsed.kind = "scene"
        elif line.startswith("[gd_resource"):
            parsed.kind = "resource"
        elif _EXT_RE.match(line):
            attrs = _attrs(_EXT_RE.match(line).group(1))
            parsed.ext[int(attrs["id"])] = (attrs["path"], attrs.get("type", ""))
        elif _SUB_RE.match(line):
            attrs = _attrs(_SUB_RE.match(line).group(1))
            block = {"type": attrs.get("type", ""), "props": {}}
            i = _read_props(lines, i + 1, block["props"])
            parsed.subs[int(attrs["id"])] = block
            continue
        elif _NODE_RE.match(line):
            attrs = _attrs(_NODE_RE.match(line).group(1))
            inst = _INSTANCE_RE.search(line)
            node = {
                "name": attrs.get("name", ""),
                "type": attrs.get("type", ""),
                "parent": attrs.get("parent"),
                "index": attrs.get("index"),
                "instance": inst.group(1) if inst else None,
                "props": {},
            }
            i = _read_props(lines, i + 1, node["props"])
            parsed.nodes.append(node)
            continue
        elif _RES_RE.match(line):
            i = _read_props(lines, i + 1, parsed.root_props)
            continue
        i += 1
    return parsed


def _read_props(lines, i, out):
    """Consume ``key = value`` lines (with continuations) starting at ``i``."""
    key = None
    while i < len(lines):
        raw = lines[i]
        stripped = raw.strip()
        if stripped.startswith("[") or stripped == "":
            break
        match = _PROP_RE.match(stripped)
        if match:
            key = match.group(1)
            out[key] = match.group(2).strip()
        elif key is not None:
            # continuation of a multi-line value (arrays / dictionaries)
            out[key] += " " + stripped
        i += 1
    return i


class Resolver(object):
    """Resolves ``res://`` paths against the campaign and DLC roots."""

    def __init__(self, roots):
        self.roots = [os.path.abspath(r) for r in roots]
        self._parsed = {}

    def find(self, res_path):
        if not res_path.startswith("res://"):
            raise ExtractError("cannot resolve non-res:// path %r" % res_path)
        rel = res_path[len("res://"):]
        for root in self.roots:
            candidate = os.path.join(root, rel)
            if os.path.isfile(candidate):
                return candidate
        raise ExtractError("resource %s not found under roots %s"
                           % (res_path, self.roots))

    def parse(self, res_path):
        absolute = self.find(res_path)
        if absolute not in self._parsed:
            self._parsed[absolute] = parse_file(absolute)
        return self._parsed[absolute]


# --------------------------------------------------------------------------
# Scene flattening (inheritance + instancing + property overrides)
# --------------------------------------------------------------------------
def _norm_prop(value, parsed):
    """Normalize resource references inside a property value."""
    ext = _EXTREF_RE.match(value)
    if ext:
        return ("ext", parsed.ext_path(ext.group(1)))
    sub = _SUBREF_RE.match(value)
    if sub:
        return ("sub", parsed.path, int(sub.group(1)))
    return value


def _join(prefix, suffix):
    if prefix == ".":
        return suffix
    return prefix + "/" + suffix


def flatten_scene(resolver, scene_res, cache):
    """Flatten a scene's inherited/instanced node table.

    Returns ``{node_path: {prop: str|tuple}}`` with ``"."`` for the root node.
    """
    parsed = resolver.parse(scene_res)
    if parsed.path in cache:
        return cache[parsed.path]
    if parsed.kind != "scene":
        raise ExtractError("%s is not a gd_scene" % parsed.path)

    table = {}
    root = None
    for node in parsed.nodes:
        if node["parent"] is None:
            if root is not None:
                raise ExtractError("%s: multiple root nodes" % parsed.path)
            root = node
    if root is None:
        raise ExtractError("%s: no root node" % parsed.path)

    # 1. Inherit the base scene, if this scene is a PackedScene instance.
    if root["instance"] is not None:
        base = parsed.ext_path(root["instance"])
        table.update(flatten_scene(resolver, base, cache))

    # 2. Expand every node that instances a sub-scene, under its own path.
    for node in parsed.nodes:
        if node["instance"] is None:
            continue
        prefix = "." if node["parent"] is None else _join(node["parent"], node["name"])
        sub = flatten_scene(resolver, parsed.ext_path(node["instance"]), cache)
        for key, props in sub.items():
            table[_join(prefix, key) if key != "." else prefix] = dict(props)

    # 3. Apply this file's node properties (override pass), in file order.
    for node in parsed.nodes:
        key = "." if node["parent"] is None else _join(node["parent"], node["name"])
        entry = table.setdefault(key, {})
        for prop, value in node["props"].items():
            entry[prop] = _norm_prop(value, parsed)
        if node["type"]:
            entry["__type"] = node["type"]

    cache[parsed.path] = table
    return table


# --------------------------------------------------------------------------
# Property helpers
# --------------------------------------------------------------------------
def parse_vec2(value, where, default=(0.0, 0.0)):
    if value is None:
        return default
    match = _VEC2_RE.match(value.strip())
    if not match:
        raise ExtractError("%s: unsupported Vector2 %r" % (where, value))
    return (float(match.group(1)), float(match.group(2)))


def as_float(value, where):
    try:
        return float(value)
    except (TypeError, ValueError):
        raise ExtractError("%s: expected a number, got %r" % (where, value))


def shape_circle(resolver, shape_ref, where):
    """Resolve a ``shape`` property to a CircleShape2D radius."""
    if not isinstance(shape_ref, tuple):
        raise ExtractError("%s: shape is not a resource reference (%r)" % (where, shape_ref))
    tag = shape_ref[0]
    if tag == "sub":
        _, file_path, sub_id = shape_ref
        parsed = parse_file(file_path)
        block = parsed.subs.get(sub_id)
        if block is None:
            raise ExtractError("%s: missing SubResource( %d )" % (file_path, sub_id))
        if block["type"] != "CircleShape2D":
            raise ExtractError("%s: shape SubResource( %d ) is %s, expected CircleShape2D"
                               % (file_path, sub_id, block["type"]))
        radius = block["props"].get("radius")
        return CIRCLE_DEFAULT_RADIUS if radius is None else as_float(radius, file_path)
    # tag == "ext": an external shape resource
    res_path = shape_ref[1]
    parsed = resolver.parse(res_path)
    if parsed.kind == "resource":
        radius = parsed.root_props.get("radius")
        return CIRCLE_DEFAULT_RADIUS if radius is None else as_float(radius, parsed.path)
    table = flatten_scene(resolver, res_path, {})
    radius = table.get(".", {}).get("radius")
    if radius is None:
        raise ExtractError("%s: external shape has no radius" % res_path)
    return as_float(radius, parsed.path)


def _require_upright(props, path, where):
    """Circles are only composed for rotation-free transforms."""
    if props is None:
        return
    rotation = props.get("rotation")
    if rotation is not None and as_float(rotation, where) != 0.0:
        raise ExtractError("%s: node %r has non-zero rotation (=%s); "
                           "rotated circles are not supported" % (where, path, rotation))
    rot_deg = props.get("rotation_degrees")
    if rot_deg is not None and as_float(rot_deg, where) != 0.0:
        raise ExtractError("%s: node %r has non-zero rotation_degrees (=%s); "
                           "rotated circles are not supported" % (where, path, rot_deg))


def compose_circle(resolver, table, node_paths, where):
    """Compose the root-local centre of a shape node.

    Godot transforms a chain as ``root_scale * (p_1 + p_2 + ... + p_n)``: a
    scale on an intermediate node only affects that node's own subtree, so it
    cannot be folded into a single root-local circle.  Only a uniform, upright
    root scale is therefore accepted; any other scale on the path aborts rather
    than being approximated.
    """
    root = table.get(".")
    _require_upright(root, ".", where)
    rx, ry = parse_vec2(root.get("scale") if root else None,
                        "%s .scale" % where, default=(1.0, 1.0))
    if abs(rx - ry) > 1e-6:
        raise ExtractError("%s: root has non-uniform scale (%g, %g)" % (where, rx, ry))

    cx = cy = 0.0
    for path in node_paths:
        props = table.get(path)
        if props is None:
            raise ExtractError("%s: node %r not present in flattened scene" % (where, path))
        px, py = parse_vec2(props.get("position"), "%s %s.position" % (where, path))
        cx += px
        cy += py
        _require_upright(props, path, where)
        sx, sy = parse_vec2(props.get("scale"), "%s %s.scale" % (where, path),
                            default=(1.0, 1.0))
        if abs(sx - 1.0) > 1e-9 or abs(sy - 1.0) > 1e-9:
            raise ExtractError("%s: node %r has scale (%g, %g) on the shape path; "
                               "only a uniform scene-root scale can be composed"
                               % (where, path, sx, sy))
    return (cx * rx, cy * ry, rx)


def circle_from_path(resolver, table, root_node, shape_node, where):
    """Return ``(radius, x, y)`` for a named Area2D/CollisionShape2D pair."""
    if table.get(root_node) is None:
        return None
    props = table.get(shape_node)
    if props is None:
        raise ExtractError("%s: node %r exists but shape node %r is missing"
                           % (where, root_node, shape_node))
    radius = shape_circle(resolver, props.get("shape"), "%s %s.shape" % (where, shape_node))
    parts = [root_node] if root_node == shape_node else [root_node, shape_node]
    cx, cy, scale = compose_circle(resolver, table, parts, where)
    return (radius * scale, cx, cy)


def stats_knockback(resolver, stats_ref, where):
    """Resolve ``stats`` -> ``knockback_resistance`` (Stats.gd default 0.0)."""
    if stats_ref is None:
        raise ExtractError("%s: no stats resource on the body" % where)
    if not isinstance(stats_ref, tuple):
        raise ExtractError("%s: stats is not a resource reference (%r)" % (where, stats_ref))
    if stats_ref[0] == "sub":
        _, file_path, sub_id = stats_ref
        parsed = parse_file(file_path)
        block = parsed.subs.get(sub_id)
        if block is None:
            raise ExtractError("%s: missing stats SubResource( %d )" % (file_path, sub_id))
        value = block["props"].get("knockback_resistance")
        return 0.0 if value is None else as_float(value, file_path)
    res_path = stats_ref[1]
    parsed = resolver.parse(res_path)
    if parsed.kind != "resource":
        raise ExtractError("%s: stats resource is not a gd_resource" % res_path)
    value = parsed.root_props.get("knockback_resistance")
    return 0.0 if value is None else as_float(value, parsed.path)


# --------------------------------------------------------------------------
# BT_KINDS recovery from brotato_waves.h
# --------------------------------------------------------------------------
def _host_string_table(text, marker):
    """Read a host-only ``const char*`` table body after ``marker``."""
    try:
        start = text.index(marker)
    except ValueError:
        return None
    start = text.index("{", start)
    end = text.index("};", start)
    return re.findall(r'"([^"]*)"', text[start:end])


def read_kinds(waves_path):
    """Recover BT_KINDS order together with each kind's source scene path.

    ``brotato_waves.h`` is generated by ocean/brotato/extract_gameplay.py and
    exposes ``BT_KIND_NAME``/``BT_KIND_SCENE`` aligned with ``BT_KINDS``; those
    tables are the authoritative kind -> scene mapping (elite/boss kinds are
    named after their real scene stems, so no name rewriting is needed).
    """
    with open(waves_path, "r", encoding="utf-8") as handle:
        text = handle.read()
    start = text.index("BT_KINDS[")
    start = text.index("= {", start)
    end = text.index("};", start)
    count = sum(1 for line in text[start:end].splitlines() if line.strip().startswith("{"))
    if not count:
        raise ExtractError("%s: no BT_KINDS rows found" % waves_path)
    declared = re.search(r"#define\s+BT_NKINDS\s+(\d+)", text)
    if declared and int(declared.group(1)) != count:
        raise ExtractError("%s: BT_NKINDS=%s but parsed %d rows"
                           % (waves_path, declared.group(1), count))

    names = _host_string_table(text, "BT_KIND_NAME[BT_NKINDS]")
    scenes = _host_string_table(text, "BT_KIND_SCENE[BT_NKINDS]")
    if names is None:
        raise ExtractError("%s: header must provide the BT_KIND_NAME table "
                           "(regenerate with ocean/brotato/extract_gameplay.py)"
                           % waves_path)
    if len(names) != count:
        raise ExtractError("%s: BT_KIND_NAME has %d entries, expected %d"
                           % (waves_path, len(names), count))
    if scenes is not None and len(scenes) != count:
        raise ExtractError("%s: BT_KIND_SCENE has %d entries, expected %d"
                           % (waves_path, len(scenes), count))
    # BT_KIND_SCENE is optional: when absent the scene is located from the kind
    # name, which is the scene stem (no elite/boss suffix handling needed).
    return [{"name": names[i],
             "scene": scenes[i] if scenes is not None else None}
            for i in range(count)]


def read_neutrals(waves_path):
    """Recover the neutral units (BT_NEUTRALS order) with their source scenes.

    ``brotato_waves.h`` mirrors ``BT_NEUTRALS`` with ``BT_NEUTRAL_NAME`` /
    ``BT_NEUTRAL_SCENE``.  Unlike enemy kinds the two zone trees share the scene
    stem ``tree`` (they live in different packs), so the exact ``res://`` scene
    path is the only unambiguous mapping and is used verbatim.
    """
    with open(waves_path, "r", encoding="utf-8") as handle:
        text = handle.read()
    declared = re.search(r"#define\s+BT_NNEUTRALS\s+(\d+)", text)
    count = int(declared.group(1)) if declared else 0
    if count == 0:
        return []
    names = _host_string_table(text, "BT_NEUTRAL_NAME[BT_NNEUTRALS]")
    scenes = _host_string_table(text, "BT_NEUTRAL_SCENE[BT_NNEUTRALS]")
    if names is None:
        raise ExtractError("%s: BT_NNEUTRALS=%d but the BT_NEUTRAL_NAME table is "
                           "missing (regenerate with ocean/brotato/extract_gameplay.py)"
                           % (waves_path, count))
    if scenes is None:
        raise ExtractError("%s: BT_NNEUTRALS=%d but the BT_NEUTRAL_SCENE table is "
                           "missing (regenerate with ocean/brotato/extract_gameplay.py)"
                           % (waves_path, count))
    if len(names) != count:
        raise ExtractError("%s: BT_NEUTRAL_NAME has %d entries, expected %d"
                           % (waves_path, len(names), count))
    if len(scenes) != count:
        raise ExtractError("%s: BT_NEUTRAL_SCENE has %d entries, expected %d"
                           % (waves_path, len(scenes), count))
    return [{"name": names[i], "scene": scenes[i]} for i in range(count)]


def build_scene_index(resolver):
    """Map scene file stem -> res:// scene paths found under the roots."""
    index = {}
    for root in resolver.roots:
        for subdir in ("entities/units/enemies", "dlcs"):
            base = os.path.join(root, subdir)
            if not os.path.isdir(base):
                continue
            for dirpath, _dirnames, filenames in os.walk(base):
                for name in sorted(filenames):
                    if not name.endswith(".tscn"):
                        continue
                    rel = os.path.relpath(os.path.join(dirpath, name), root)
                    index.setdefault(name[:-len(".tscn")], []).append(
                        "res://" + rel.replace(os.sep, "/"))
    return index


def scene_for_kind(name):
    """Kind name -> scene stem (GameplayTables emits stems, no suffix handling)."""
    return name


def resolve_scene(index, name):
    """Locate the unique scene for a kind name, or raise."""
    hits = index.get(scene_for_kind(name))
    if not hits:
        raise ExtractError("kind %r: no scene found for stem %r"
                           % (name, scene_for_kind(name)))
    if len(hits) > 1:
        raise ExtractError("kind %r: scene stem %r is ambiguous: %s"
                           % (name, scene_for_kind(name), ", ".join(hits)))
    return hits[0]


# --------------------------------------------------------------------------
# Geometry extraction
# --------------------------------------------------------------------------
def extract_body(resolver, cache, scene_res, label, require_hitbox=True, require_hurtbox=True):
    table = flatten_scene(resolver, scene_res, cache)
    root = table.get(".")
    if root is None:
        raise ExtractError("%s: flattened scene has no root node" % scene_res)
    if root.get("__type") != "RigidBody2D":
        raise ExtractError("%s: root node type is %r, expected RigidBody2D"
                           % (scene_res, root.get("__type")))
    mode = root.get("mode")
    if mode is None:
        raise ExtractError("%s: root RigidBody2D does not declare 'mode'" % scene_res)
    mass = RIGIDBODY_DEFAULT_MASS
    if "mass" in root:
        mass = as_float(root["mass"], "%s mass" % scene_res)

    body = circle_from_path(resolver, table, "Collision", "Collision", label)
    if body is None:
        raise ExtractError("%s: body 'Collision' shape node is missing" % scene_res)
    hurt = circle_from_path(resolver, table, "Hurtbox", "Hurtbox/Collision", label)
    hit = circle_from_path(resolver, table, "Hitbox", "Hitbox/Collision", label)
    if require_hurtbox and hurt is None:
        raise ExtractError("%s: %s has no 'Hurtbox' Area2D (Hurtbox/Collision)"
                           % (scene_res, label))
    if require_hitbox and hit is None:
        raise ExtractError("%s: %s has no contact 'Hitbox' Area2D (Hitbox/Collision)"
                           % (scene_res, label))

    knockback = 0.0
    if hurt is not None:
        knockback = stats_knockback(resolver, root.get("stats"), label)

    limitations = []
    size_on_boost = root.get("size_on_boost")
    if size_on_boost is not None and as_float(size_on_boost, label) != 0.0:
        limitations.append(
            "runtime: boost adds size_on_boost=%s per boost to body/Hurtbox/Hitbox "
            "scales (script: *attack_behaviors/charging_attack_behavior.gd)" % size_on_boost)

    return {
        "table": table,
        "mode": int(float(mode)),
        "mass": mass,
        "knockback": knockback,
        "body": body,
        "hurt": hurt,
        "hit": hit,
        "limitations": limitations,
    }


# --------------------------------------------------------------------------
# Header emission
# --------------------------------------------------------------------------
def fmt(value):
    """Deterministic float literal with float32-safe precision."""
    value = float(value)
    if value == 0.0:
        value = 0.0  # normalise -0.0
    text = "%.6f" % value
    if "." in text:
        text = text.rstrip("0")
        if text.endswith("."):
            text += "0"
    return text + "f"


def row_comment(row):
    """The provenance comment block emitted above one BtBodyPhysics row."""
    out = ["    /* %s" % row["kind"],
           "     * scene: %s" % row["scene"]]
    for limitation in row["limitations"]:
        out.append("     * LIMITATION %s" % limitation)
    out.append("     */")
    return out


def body_init(row):
    """One ``BtBodyPhysics`` initializer from an extracted body dict."""
    def circle(ref):
        if ref is None:
            return (fmt(0), fmt(0), fmt(0))
        return (fmt(ref[0]), fmt(ref[1]), fmt(ref[2]))

    b = circle(row["body"])
    hu = circle(row["hurt"])
    hi = circle(row["hit"])
    return ("{ %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %d, %d, %d }"
            % (b[0], b[1], b[2], hu[0], hu[1], hu[2], hi[0], hi[1], hi[2],
               fmt(row["mass"]), fmt(row["knockback"]),
               row["mode"], 1 if row["hit"] else 0, 1 if row["hurt"] else 0))


def emit(rows, player, neutrals, kinds, out_path, campaign_root, dlc_root, waves_path):
    lines = []
    lines.append("/* Generated by ocean/brotato/extract_physics.py -- DO NOT EDIT.")
    lines.append(" *")
    lines.append(" * Reproducible extraction of Brotato Bull/Danger-5 2D physics geometry")
    lines.append(" * from the recovered Godot 3.x scene sources.  Regenerate with:")
    lines.append(" *")
    lines.append(" *   python3 ocean/brotato/extract_physics.py \\")
    lines.append(" *       --campaign-root %s \\" % campaign_root)
    lines.append(" *       --dlc-root %s \\" % dlc_root)
    lines.append(" *       --waves %s \\" % waves_path)
    lines.append(" *       --out %s" % out_path)
    lines.append(" *")
    lines.append(" * Every value is resolved from the real PackedScene/SubResource graph;")
    lines.append(" * no row is hand-authored.  Circle radii honour the engine default")
    lines.append(" * CircleShape2D radius (%g) when the scene omits 'radius'." % CIRCLE_DEFAULT_RADIUS)
    lines.append(" * Centres are root-local and compose the 'position' of each node on the")
    lines.append(" * path root -> shape node (e.g. Hurtbox position + Hurtbox/Collision position).")
    lines.append(" *")
    lines.append(" * Body mode uses Godot RigidBody2D enum values: 2 = MODE_CHARACTER,")
    lines.append(" * 3 = MODE_KINEMATIC.")
    lines.append(" *")
    lines.append(" * BT_PHYSICS rows are ordered exactly like BT_KINDS in")
    lines.append(" * brotato_waves.h and carry the matching BT_KIND_NAME / BT_KIND_SCENE")
    lines.append(" * entry (%d kinds, enemy + elite + boss).  BT_NEUTRAL_PHYSICS rows are"
                 % len(kinds))
    lines.append(" * ordered exactly like BT_NEUTRALS and carry the matching")
    lines.append(" * BT_NEUTRAL_NAME / BT_NEUTRAL_SCENE entry; neutrals are their own kind")
    lines.append(" * domain, so the enemy BT_PHYSICS table must never be indexed with a")
    lines.append(" * neutral kind.")
    lines.append(" *")
    lines.append(" * Physics is initial-scene geometry only: it is exact for an unscaled,")
    lines.append(" * unrotated scene root; runtime scaling is listed per row where relevant.")
    lines.append(" *")
    lines.append(" * Device portability: tables use BT_TABLE (brotato_rng.h) so MSL gets")
    lines.append(" * `static constant` and CUDA device passes get device-resident const;")
    lines.append(" * rows hold only floats and unsigned chars -- no pointers, no names.")
    lines.append(" */")
    lines.append("#pragma once")
    lines.append('#include "brotato_rng.h"   /* BT_TABLE (host static const, MSL static constant, CUDA device const) */')
    lines.append("")
    lines.append("")
    lines.append("typedef struct {")
    lines.append("    float body_radius, body_x, body_y; /* RigidBody2D body CollisionShape2D (CircleShape2D), root-local */")
    lines.append("    float hurt_radius, hurt_x, hurt_y; /* damage Hurtbox Area2D circle, root-local */")
    lines.append("    float hit_radius, hit_x, hit_y;    /* contact Hitbox Area2D circle, root-local (0 if !has_hitbox) */")
    lines.append("    float mass;                        /* RigidBody2D.mass (engine default %g) */" % RIGIDBODY_DEFAULT_MASS)
    lines.append("    float knockback_resistance;        /* Stats.knockback_resistance (Stats.gd default 0.0) */")
    lines.append("    unsigned char body_mode;           /* RigidBody2D.mode: 2 character, 3 kinematic */")
    lines.append("    unsigned char has_hitbox;")
    lines.append("    unsigned char has_hurtbox;")
    lines.append("} BtBodyPhysics;")
    lines.append("")
    lines.append("#define BT_PHYSICS_NKINDS %d" % len(kinds))
    lines.append("BT_TABLE BtBodyPhysics BT_PHYSICS[BT_PHYSICS_NKINDS] = {")
    for row in rows:
        lines.extend(row_comment(row))
        lines.append("    %s," % body_init(row))
    lines.append("};")
    lines.append("")
    lines.append("/* Player body (%s), RigidBody2D kinematic mode 3. */" % player["scene"])
    lines.append("BT_TABLE BtBodyPhysics BT_PLAYER_PHYSICS = %s;" % body_init(player))
    lines.append("")
    lines.append("/* Neutral unit bodies (zone trees).  Ordered exactly like BT_NEUTRALS in")
    lines.append(" * brotato_waves.h and carrying the matching BT_NEUTRAL_NAME /")
    lines.append(" * BT_NEUTRAL_SCENE entry.  A neutral kind is a BT_NEUTRALS index, not a")
    lines.append(" * BT_KINDS index, so it must be resolved through this table.  Trees carry")
    lines.append(" * no contact Hitbox (has_hitbox = 0).")
    lines.append(" */")
    lines.append("#define BT_NEUTRAL_PHYSICS_NKINDS %d" % len(neutrals))
    if neutrals:
        lines.append("BT_TABLE BtBodyPhysics BT_NEUTRAL_PHYSICS[BT_NEUTRAL_PHYSICS_NKINDS] = {")
        for row in neutrals:
            lines.extend(row_comment(row))
            lines.append("    %s," % body_init(row))
        lines.append("};")
    else:
        # mirror the BT_NEUTRALS[1] fallback: never indexed because no slot is
        # neutral when the neutral roster is empty.
        lines.append("BT_TABLE BtBodyPhysics BT_NEUTRAL_PHYSICS[1] = { { 0 } };")
    lines.append("")
    limitations = []
    for row in list(rows) + list(neutrals):
        for limitation in row["limitations"]:
            limitations.append("%s: %s" % (row["kind"], limitation))
    lines.append("/* Known limitations of this extraction:")
    if limitations:
        for limitation in limitations:
            lines.append(" *  - %s" % limitation)
    else:
        lines.append(" *  - none detected")
    lines.append(" *")
    lines.append(" * Runtime effects applied by scripts (not represented above): charging attack")
    lines.append(" * behaviour multiplies mass by 5 while charging; charmed enemies retarget")
    lines.append(" * layers; death disables body/hurtbox shapes.")
    lines.append(" */")
    lines.append("")

    rendered = "\n".join(lines)
    with open(out_path, "w", encoding="utf-8") as handle:
        handle.write(rendered)
    return rendered


# --------------------------------------------------------------------------
# Entry point
# --------------------------------------------------------------------------
def main(argv):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--campaign-root", required=True,
                        help="root of the recovered campaign project (contains entities/)")
    parser.add_argument("--dlc-root", required=True,
                        help="root of the recovered Abyss DLC project (contains dlcs/dlc_1/)")
    parser.add_argument("--waves", required=True,
                        help="brotato_waves.h providing BT_KINDS order")
    parser.add_argument("--out", required=True, help="output brotato_geometry.h path")
    parser.add_argument("--player-scene", default="res://entities/units/player/player.tscn",
                        help="player scene resource path (default: %(default)s)")
    args = parser.parse_args(argv)

    try:
        return run(args)
    except ExtractError as exc:
        sys.stderr.write("extract_physics: error: %s\n" % exc)
        return 1


def _row(label, scene_res, extracted):
    return {
        "kind": label,
        "scene": scene_res,
        "mode": extracted["mode"],
        "mass": extracted["mass"],
        "knockback": extracted["knockback"],
        "body": extracted["body"],
        "hurt": extracted["hurt"],
        "hit": extracted["hit"],
        "limitations": extracted["limitations"],
    }


def run(args):
    kinds = read_kinds(args.waves)
    neutrals = read_neutrals(args.waves)
    resolver = Resolver([args.campaign_root, args.dlc_root])
    index = build_scene_index(resolver)
    cache = {}

    rows = []
    for kind in kinds:
        name = kind["name"]
        by_name = resolve_scene(index, name)
        if kind["scene"] is not None and kind["scene"] != by_name:
            raise ExtractError("kind %r: BT_KIND_SCENE says %s but stem lookup found %s"
                               % (name, kind["scene"], by_name))
        scene_res = kind["scene"] or by_name
        extracted = extract_body(resolver, cache, scene_res, name)
        if extracted["mode"] != 2:
            raise ExtractError("kind %r (%s): expected enemy RigidBody2D mode 2, got %d"
                               % (name, scene_res, extracted["mode"]))
        rows.append(_row(name, scene_res, extracted))

    neutral_rows = []
    for neutral in neutrals:
        scene_res = neutral["scene"]
        extracted = extract_body(resolver, cache, scene_res, neutral["name"],
                                 require_hitbox=False)
        if extracted["mode"] != 2:
            raise ExtractError("neutral %r (%s): expected RigidBody2D mode 2, got %d"
                               % (neutral["name"], scene_res, extracted["mode"]))
        neutral_rows.append(_row(neutral["name"], scene_res, extracted))

    player = extract_body(resolver, cache, args.player_scene, "player",
                          require_hitbox=False)
    player["scene"] = args.player_scene
    if player["mode"] != 3:
        raise ExtractError("player: expected RigidBody2D mode 3, got %d" % player["mode"])

    emit(rows, player, neutral_rows, kinds, args.out,
         args.campaign_root, args.dlc_root, args.waves)
    sys.stderr.write("extract_physics: wrote %d kinds + %d neutrals + player to %s\n"
                     % (len(rows), len(neutral_rows), args.out))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
