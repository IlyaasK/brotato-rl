#!/usr/bin/env python3
"""Emit the enemy MovementBehavior spec tables for brotato_enemy_motion.h.

Resolves, per BT_KINDS row (same order as brotato_waves.h / brotato_geometry.h):

  * the kind root's ``$MovementBehavior`` script and its exporting params
    (scene -> the node the source's Unit._ready/Enemy.init drives), and
  * every ``States/StateN/MovementBehavior`` script (the per-boss-state
    behaviour Boss._on_CheckStateTimer_timeout installs), and
  * the ``Animation/Sprite`` texture half extents, which entity.gd:34-43 folds
    into ``_min_pos``/``_max_pos`` -- the bounds every *rand_pos* movement
    behaviour samples and clamps against.

Only the fields the *movement* runtime needs are carried:

  stay_in_range        uses `_actual_target_range`, drawn at init (BT_INIT_OPS).
  follow_rand_around   uses `_distance_from_player`, drawn at init; the stored
                       offset is normalised*_actual when allow_within is false.
  patrol_edges         uses `_actual` (init draw); get_new_target's
                       max_target_distance is dead code (the clamp overwrites
                       the scaled direction -- patrol_edges_of_map:14-24).
  target_rand_around   uses `_actual` (init draw) + update_every_x_frames.
  detect_target        uses detection_range / speed_bonus_on_target_detection
                       and its two child behaviour scripts.
  follow_target        stop_close_to_target is inert for enemies: the enemy
                       scenes carry no TargetBehavior node, so the
                       _on_TargetBehavior_* handlers never fire and
                       `_target_player` stays false (follow_target:14-16,31).

Usage::

    python3 ocean/brotato/extract_movement.py \
        --campaign-root /Users/me/brotato_recovered \
        --dlc-root      /Users/me/brotato_abyss \
        [--write ocean/brotato/brotato_enemy_motion.h]

Without --write the generated block is printed to stdout.
"""
import argparse
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import extract_gameplay as G  # noqa: E402
import extract_physics as physics  # noqa: E402


# Direction enum global/direction.gd: enum {TOP, LEFT, BOTTOM, RIGHT, NONE}
# Behaviour codes mirror BtEnemyInitOp.behavior (BT_INITBEH_MOV_*).

BY_STEM = {
    "movement_behavior": G.BT_INITBEH_MOV_BASE,
    "stay_in_range_from_player_movement_behavior": G.BT_INITBEH_MOV_STAY_IN_RANGE,
    "follow_rand_pos_around_player_movement_behavior": G.BT_INITBEH_MOV_FOLLOW_RAND_AROUND,
    "patrol_around_the_map_movement_behavior": G.BT_INITBEH_MOV_PATROL_AROUND_MAP,
    "patrol_edges_of_map_movement_behavior": G.BT_INITBEH_MOV_PATROL_EDGES,
    "target_rand_pos_around_player_movement_behavior": G.BT_INITBEH_MOV_TARGET_RAND_AROUND,
    "detect_target_movement_behavior": G.BT_INITBEH_MOV_DETECT_TARGET,
    "follow_target_movement_behavior": G.BT_INITBEH_MOV_FOLLOW_TARGET,
    "go_towards_map_center_movement_behavior": G.BT_INITBEH_MOV_GO_MAP_CENTER,
    "target_rand_pos_movement_behavior": G.BT_INITBEH_MOV_TARGET_RAND_POS,
    "target_rand_gold_pos_behavior": G.BT_INITBEH_MOV_TARGET_RAND_GOLD,
    "copy_nearby_entities_movement_behavior": G.BT_INITBEH_MOV_COPY_NEARBY,
    "player_movement_behavior": G.BT_INITBEH_MOV_PLAYER,
    "stargazer_movement_behavior": G.BT_INITBEH_MOV_STARGAZER,
}

BASE = G.BT_INITBEH_MOV_BASE
UNUSED = 0xFF   # slot above BT_NKIND_STATES[kind]: never selected (estate<ns guard)

_FLAG_ALLOW_WITHIN = 1

PNG_SIG = b"\x89PNG\r\n\x1a\n"


def png_size(path):
    with open(path, "rb") as fh:
        head = fh.read(24)
    if head[:8] != PNG_SIG or head[12:16] != b"IHDR":
        raise G.ExtractError("%s: not a PNG IHDR" % path)
    w, h = struct.unpack(">II", head[16:24])
    return w, h


def script_stem(node):
    script = node.get("script")
    text = script[1] if isinstance(script, tuple) else script
    if not text:
        return None
    m = re.search(r"/movement_behaviors/([a-z_]+)\.gd$", text)
    if m:
        return m.group(1)
    m = re.search(r"/([a-z_]+_movement_behavior)\.gd$", text)
    if m:
        return m.group(1)
    return None


def spec_of(node, where):
    stem = script_stem(node)
    if stem is None or stem not in BY_STEM:
        raise G.ExtractError("%s: unmodelled MovementBehavior script %r" % (where, stem))
    beh = BY_STEM[stem]
    flags = 0
    p0 = 0.0
    p1 = 0.0
    pre = 0
    sub = 0
    if stem == "follow_rand_pos_around_player_movement_behavior":
        if G.get_bool(node, "allow_within", True):
            flags |= _FLAG_ALLOW_WITHIN
    elif stem == "target_rand_pos_around_player_movement_behavior":
        p0 = float(G.get_int(node, "update_every_x_frames", -1))
    elif stem == "detect_target_movement_behavior":
        p0 = G.get_float(node, "detection_range", 300)
        p1 = G.get_float(node, "speed_bonus_on_target_detection", 300)
    return beh, pre, sub, flags, p0, p1


def resolve_spec(table, path, where):
    node = table.get(path)
    if node is None:
        raise G.ExtractError("%s: missing %s" % (where, path))
    beh, pre, sub, flags, p0, p1 = spec_of(node, where + " " + path)
    if beh == G.BT_INITBEH_MOV_DETECT_TARGET:
        before = table.get(path + "/MovementBehaviorBeforeTargetDetection")
        after = table.get(path + "/MovementBehaviorAfterTargetDetection")
        if before is None or after is None:
            raise G.ExtractError("%s: detect_target without both children" % where)
        pre = BY_STEM[script_stem(before)]
        sub = BY_STEM[script_stem(after)]
    return (beh, pre, sub, flags, p0, p1)


def fmt_spec(s):
    beh, pre, sub, flags, p0, p1 = s
    return "{ %du, %du, %du, %du, %sf, %sf }" % (
        beh, pre, sub, flags, G.fmt_float(p0), G.fmt_float(p1))


# Behaviour codes the runtime dispatches.  Every code a scene resolves to MUST
# be here; the zeros-only codes MUST NOT be reached by any enemy kind/state.
DISPATCHED = {
    G.BT_INITBEH_MOV_FOLLOW_TARGET,
    G.BT_INITBEH_MOV_STAY_IN_RANGE,
    G.BT_INITBEH_MOV_FOLLOW_RAND_AROUND,
    G.BT_INITBEH_MOV_TARGET_RAND_POS,
    G.BT_INITBEH_MOV_TARGET_RAND_AROUND,
    G.BT_INITBEH_MOV_PATROL_EDGES,
    G.BT_INITBEH_MOV_DETECT_TARGET,
    G.BT_INITBEH_MOV_GO_MAP_CENTER,
    G.BT_INITBEH_MOV_TARGET_RAND_GOLD,
    G.BT_INITBEH_MOV_STARGAZER,
}
ZERO_ONLY = {
    G.BT_INITBEH_MOV_BASE: "movement_behavior (base)",
    G.BT_INITBEH_MOV_PATROL_AROUND_MAP: "patrol_around_the_map",
    G.BT_INITBEH_MOV_COPY_NEARBY: "copy_nearby_entities",
    G.BT_INITBEH_MOV_PLAYER: "player_movement_behavior",
}
PARAM_FREE_CHILD = {G.BT_INITBEH_MOV_TARGET_RAND_POS, G.BT_INITBEH_MOV_FOLLOW_TARGET}


# Scene-root scripts the movement/lifecycle slice special-cases.
MOVK_BITS = {"stargazer.gd": 1, "evil_mob.gd": 2, "iron_lung.gd": 4,
             "healer.gd": 8, "buffer.gd": 16}

# healer.gd exports (healer.gd:8-11 defaults; the healer/hermit scenes set them
# explicitly).  A non-healer scene has none of these properties.
HEALER_EXPORTS = ("heal", "heal_increase_each_wave", "player_heal",
                  "player_heal_increase_each_wave")
HEALER_DEFAULTS = (100.0, 10.0, 1.0, 0.5)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--campaign-root", required=True)
    ap.add_argument("--dlc-root", required=True)
    ap.add_argument("--write", default=None)
    ap.add_argument("--header", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "brotato_enemy_motion.h"))
    args = ap.parse_args()

    class A:
        pass
    a = A()
    a.campaign_root = args.campaign_root
    a.dlc_root = args.dlc_root
    data = G.build(a)
    resolver = data["resolver"]
    roots = resolver.roots

    enemy_kinds = data["enemy_kinds"]
    if len(enemy_kinds) != 66:
        raise G.ExtractError("expected 66 enemy kinds, got %d" % len(enemy_kinds))

    order = sorted(enemy_kinds, key=lambda s: data["kind_index"][s])
    per_kind_root = []
    per_kind_states = []
    halfw = []
    halfh = []
    movk = []
    dec = []
    qty = []
    heal = []
    boostable = []
    charmable = []
    isloot = []
    used = set()
    for scene in order:
        table = physics.flatten_scene(resolver, scene, {})
        stem = data["scene_stem"](scene)
        root_spec = resolve_spec(table, "MovementBehavior", stem)
        rnode = table.get(".") or {}
        rscript = rnode.get("script")
        rtext = rscript[1] if isinstance(rscript, tuple) else rscript
        rbase = os.path.basename(rtext) if rtext else ""
        movk.append(MOVK_BITS.get(rbase, 0))
        # evil_mob.gd exports (evil_mob.tscn / evil_mob_swimmer.tscn).
        dec.append(G.get_int(rnode, "decrease_speed_by_evolve", 0))
        prop = rnode.get("quantity_to_evolve", "") or ""
        q = [int(x) for x in re.findall(r"-?\d+", prop)][:3]
        while len(q) < 3:
            q.append(0)
        qty.append(q)
        # scene-root flags (entity.gd:7 can_be_boosted, enemy.gd:11-12
        # can_be_charmed / is_loot; enemy.tscn:48,53 supplies the defaults)
        if rbase == "healer.gd":
            heal.append([G.get_float(rnode, k, d) for k, d in zip(HEALER_EXPORTS, HEALER_DEFAULTS)])
        else:
            heal.append([0.0, 0.0, 0.0, 0.0])
        boostable.append(G.get_bool(rnode, "can_be_boosted", True))
        charmable.append(G.get_bool(rnode, "can_be_charmed", True))
        isloot.append(G.get_bool(rnode, "is_loot", False))
        # state ordinals follow
        # State ordinals must match the generated BT_ATTACK_STATES order: the
        # extractor numbers states by scene_child_order's States children, and
        # scene_states (extract_gameplay.py:458) re-derives the same order from
        # the raw node indices.  Assert the two agree, then index by child order.
        children = G.scene_child_order(resolver, scene)
        states = list(children.get("States", []))
        names_child = [p.split("/")[-1] for p in states]
        parsed = resolver.parse(scene)
        raw = []
        for node in parsed.nodes:
            if node["parent"] == "States" and node["name"].startswith("State"):
                try:
                    raw.append((int(node["index"]), node["name"]))
                except (TypeError, ValueError):
                    raw.append((len(raw), node["name"]))
        raw.sort()
        if names_child != [n for _i, n in raw]:
            raise G.ExtractError("%s: state order %s != BT_ATTACK_STATES order %s"
                                 % (stem, names_child, [n for _i, n in raw]))
        state_specs = []
        for spath in states:
            mv = spath + "/MovementBehavior"
            if mv not in table:
                raise G.ExtractError("%s: %s missing MovementBehavior" % (stem, spath))
            state_specs.append(resolve_spec(table, mv, stem))
        # The state count must equal the generated attack-state count so
        # BT_MOV_STATE[kind][st] and BT_ATTACK_STATES[kind][st] agree.
        want = len(data["kind_states"][scene])
        if len(state_specs) != want:
            raise G.ExtractError("%s: %d movement states, %d attack states"
                                 % (stem, len(state_specs), want))
        if want > 3:
            raise G.ExtractError("%s: %d states exceeds BT_MAX_STATES" % (stem, want))
        for spec in [root_spec] + state_specs:
            used.add(spec[0])
            if spec[0] == G.BT_INITBEH_MOV_DETECT_TARGET:
                if spec[1] not in PARAM_FREE_CHILD or spec[2] not in PARAM_FREE_CHILD:
                    raise G.ExtractError(
                        "%s: detect_target child %r/%r carries params the runtime "
                        "does not model" % (stem, spec[1], spec[2]))
        if root_spec[0] == G.BT_INITBEH_MOV_DETECT_TARGET:
            if root_spec[1] != G.BT_INITBEH_MOV_TARGET_RAND_POS or \
               root_spec[2] != G.BT_INITBEH_MOV_FOLLOW_TARGET:
                raise G.ExtractError("%s: unexpected detect_target children" % stem)
        spr = table.get("Animation/Sprite")
        if spr is None or not isinstance(spr.get("texture"), tuple):
            raise G.ExtractError("%s: no Animation/Sprite texture" % stem)
        tex = spr["texture"][1]
        rel = tex[len("res://"):]
        path = None
        for r in roots:
            cand = os.path.join(r, rel)
            if os.path.exists(cand):
                path = cand
                break
        if path is None:
            raise G.ExtractError("%s: texture %s not found under roots" % (stem, tex))
        w, h = png_size(path)
        per_kind_root.append((stem, root_spec))
        per_kind_states.append((stem, state_specs))
        halfw.append(w / 2.0)
        halfh.append(h / 2.0)

    # Every behaviour a kind reaches must be dispatched; the zero-only codes
    # must stay unreachable (they are the runtime's `default:` arm).
    missing = used - DISPATCHED
    if missing:
        raise G.ExtractError("movement behaviours not dispatched: %s"
                             % sorted(missing))
    reached_zero = used & set(ZERO_ONLY)
    if reached_zero:
        raise G.ExtractError("zero-only behaviours are reachable: %s"
                             % [ZERO_ONLY[b] for b in sorted(reached_zero)])

    # Charm constants: the DLC charm behavior scene + Utils.CHARM_DURATION.
    charm = {}
    utils_gd = os.path.join(args.campaign_root, "singletons", "utils.gd")
    with open(utils_gd) as fh:
        m = re.search(r"const CHARM_DURATION\s*:?\s*=\s*([0-9.]+)", fh.read())
    if not m:
        raise G.ExtractError("%s: CHARM_DURATION not found" % utils_gd)
    charm["duration"] = float(m.group(1))
    charm_tscn = os.path.join(args.dlc_root, "dlcs", "dlc_1", "effect_behaviors",
                              "enemy", "charm_enemy_effect_behavior.tscn")
    with open(charm_tscn) as fh:
        ct = fh.read()
    for key, pat in (("max_count", r"max_charm_count\s*=\s*(\d+)"),
                     ("damage_increase", r"damage_increase\s*=\s*(\d+)")):
        m = re.search(pat, ct)
        if not m:
            raise G.ExtractError("%s: %s not found" % (charm_tscn, key))
        charm[key] = int(m.group(1))

    L = []
    L.append("/* ===================== BEGIN GENERATED =====================")
    L.append(" * extract_movement.py -- DO NOT EDIT below; regenerate with")
    L.append(" *   python3 ocean/brotato/extract_movement.py \\")
    L.append(" *       --campaign-root <recovered> --dlc-root <abyss> --write <this file>")
    L.append(" *")
    L.append(" * BT_MOV_ROOT / BT_MOV_STATE carry only the movement-runtime fields of")
    L.append(" * each scene's MovementBehavior chain; the init draws those behaviours")
    L.append(" * sample live in BT_ENEMY_INIT_OPS (brotato_waves.h), keyed by the same")
    L.append(" * state ordinal.  BT_KIND_HALF_W/H are the Animation/Sprite texture")
    L.append(" * half extents entity.gd:34-43 folds into _min_pos/_max_pos.")
    L.append(" * =========================================================== */")
    L.append("#define BT_MOVFK_ALLOW_WITHIN 1u")
    L.append("")
    L.append("/* BT_KINDS rows whose scene root carries a script the movement/lifecycle")
    L.append(" * slice must special-case (stargazer.gd, evil_mob.gd, iron_lung.gd). */")
    L.append("#define BT_MOVK_STARGAZER 1u")
    L.append("#define BT_MOVK_EVIL_MOB  2u")
    L.append("#define BT_MOVK_IRON_LUNG 4u")
    L.append("#define BT_MOVK_HEALER    8u   /* scene root is healer.gd */")
    L.append("#define BT_MOVK_BUFFER   16u   /* scene root is buffer.gd */")
    L.append("BT_TABLE unsigned char BT_KIND_MOVK[BT_NKINDS] = {")
    for i, (stem, _s) in enumerate(per_kind_root):
        L.append("    %du,  /* %s */" % (movk[i], stem))
    L.append("};")
    L.append("")
    L.append("/* evil_mob.gd exports: decrease_speed_by_evolve (bonus_speed = -(dec*evo))")
    L.append(" * and quantity_to_evolve (the gold_count thresholds). */")
    L.append("BT_TABLE unsigned char BT_KIND_MOB_DECREASE[BT_NKINDS] = {")
    for i, (stem, _s) in enumerate(per_kind_root):
        L.append("    %du,  /* %s */" % (dec[i] & 0xFF, stem))
    L.append("};")
    L.append("BT_TABLE unsigned short BT_KIND_MOB_QTY[BT_NKINDS][3] = {")
    for i, (stem, _s) in enumerate(per_kind_root):
        L.append("    { %du, %du, %du },  /* %s */" % (qty[i][0], qty[i][1], qty[i][2], stem))
    L.append("};")
    L.append("")
    L.append("/* Scene-root flags for the boost/charm guards (entity.gd:7/107-110")
    L.append(" * can_be_boosted, enemy.gd:11-12 can_be_charmed / is_loot). */")
    L.append("BT_TABLE unsigned char BT_KIND_BOOSTABLE[BT_NKINDS] = {")
    for i, (stem, _s) in enumerate(per_kind_root):
        L.append("    %du,  /* %s */" % (1 if boostable[i] else 0, stem))
    L.append("};")
    L.append("/* healer.gd BoostZone exports: heal/heal_grow (enemies) and\n"
             " * player_heal/player_heal_grow (the player, charmed only). */\n"
             "BT_TABLE float BT_KIND_HEAL[BT_NKINDS][4] = {")
    for i, (stem, _s) in enumerate(per_kind_root):
        L.append("    { %sf, %sf, %sf, %sf },  /* %s */" % (
            G.fmt_float(heal[i][0]), G.fmt_float(heal[i][1]),
            G.fmt_float(heal[i][2]), G.fmt_float(heal[i][3]), stem))
    L.append("};")
    L.append("BT_TABLE unsigned char BT_KIND_CHARMABLE[BT_NKINDS] = {")
    for i, (stem, _s) in enumerate(per_kind_root):
        L.append("    %du,  /* %s */" % (1 if (charmable[i] and not isloot[i]) else 0, stem))
    L.append("};")
    L.append("BT_TABLE unsigned char BT_KIND_IS_LOOT[BT_NKINDS] = {")
    for i, (stem, _s) in enumerate(per_kind_root):
        L.append("    %du,  /* %s */" % (1 if isloot[i] else 0, stem))
    L.append("};")
    L.append("")
    L.append("/* dlc_1 charm_enemy_effect_behavior.tscn (max_charm_count/damage_increase;")
    L.append(" * the .gd defaults 5/50 are overridden in the scene) and the CharmTimer")
    L.append(" * wait_time = Utils.CHARM_DURATION (singletons/utils.gd:35). */")
    L.append("#define BT_CHARM_DURATION %.1ff" % charm["duration"])
    L.append("#define BT_CHARM_MAX_COUNT %d" % charm["max_count"])
    L.append("#define BT_CHARM_DAMAGE_INCREASE %d" % charm["damage_increase"])
    L.append("")
    L.append("typedef struct {")
    L.append("    unsigned char beh;   /* BT_INITBEH_MOV_* of the active node */")
    L.append("    unsigned char pre;   /* detect_target: before-detection child */")
    L.append("    unsigned char sub;   /* detect_target: after-detection child */")
    L.append("    unsigned char flags; /* BT_MOVFK_* */")
    L.append("    float p0;            /* rand_around: update_every_x_frames; detect: detection_range */")
    L.append("    float p1;            /* detect: speed_bonus_on_target_detection */")
    L.append("} BtMovSpec;")
    L.append("")
    L.append("/* Animation/Sprite texture half width (entity.gd:36-38 _min_pos inset). */")
    L.append("BT_TABLE float BT_KIND_HALF_W[BT_NKINDS] = {")
    for i, (stem, _s) in enumerate(per_kind_root):
        L.append("    %sf,  /* %s */" % (G.fmt_float(halfw[i]), stem))
    L.append("};")
    L.append("")
    L.append("BT_TABLE float BT_KIND_HALF_H[BT_NKINDS] = {")
    for i, (stem, _s) in enumerate(per_kind_root):
        L.append("    %sf,  /* %s */" % (G.fmt_float(halfh[i]), stem))
    L.append("};")
    L.append("")
    L.append("/* Kind root $MovementBehavior (Unit._ready / Enemy.init, state 0xFF). */")
    L.append("BT_TABLE BtMovSpec BT_MOV_ROOT[BT_NKINDS] = {")
    for stem, s in per_kind_root:
        L.append("    %s,  /* %s */" % (fmt_spec(s), stem))
    L.append("};")
    L.append("")
    L.append("/* States/StateN/MovementBehavior (Boss._on_CheckStateTimer_timeout).")
    L.append(" * Slots >= BT_NKIND_STATES[kind] are unreachable (estate < ns guard). */")
    L.append("BT_TABLE BtMovSpec BT_MOV_STATE[BT_NKINDS][BT_MAX_STATES] = {")
    for stem, specs in per_kind_states:
        cells = [fmt_spec(s) for s in specs]
        while len(cells) < 3:
            cells.append("{ %du, 0u, 0u, 0u, 0.0f, 0.0f }" % UNUSED)
        L.append("    { %s },  /* %s (%d states) */" % (", ".join(cells), stem, len(specs)))
    L.append("};")
    L.append("/* ====================== END GENERATED ====================== */")
    block = "\n".join(L) + "\n"

    if args.write:
        src = open(args.write).read()
        begin = src.index("/* ===================== BEGIN GENERATED =====================")
        end = src.index("/* ====================== END GENERATED ====================== */")
        end = src.index("\n", end) + 1
        open(args.write, "w").write(src[:begin] + block + src[end:])
    else:
        sys.stdout.write(block)


if __name__ == "__main__":
    main()
