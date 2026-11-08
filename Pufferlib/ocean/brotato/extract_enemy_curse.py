#!/usr/bin/env python3
"""Emit the DLC cursed-enemy tables + tuning constants for brotato_enemy_curse.h.

SOURCE (brotato_recovered v1.1.14.6 + brotato_abyss dlc_1)

  * dlcs/dlc_1/effect_behaviors/scene/curse_scene_effect_behavior.gd:50-65
        _on_EntitySpawner_enemy_respawned(enemy) is the ONLY curse producer.
        It returns early (and therefore draws nothing) for `enemy is Boss`
        (the core's BT_KIND_CLASS != 0) and for `enemy.stats in
        _loot_alien_stats` (built from current_zone_data.loot_alien_groups,
        i.e. the scenes whose root exports is_loot = true -- the same flag
        brotato_enemy_motion.h BT_KIND_IS_LOOT carries).  For every other
        respawned enemy it rolls get_chance_success(...) and then applies the
        curse only `... and enemy.can_be_cursed`; the roll is the LEFT operand
        of that `and`, so it still draws when can_be_cursed is false.
  * entities/units/enemies/enemy.gd:11 + enemy.tscn:52  can_be_cursed (default
    true); the DLC scenes evil_mob / evil_mob_swimmer / looter / looting_pig /
    iron_lung / sea_pig export false.
  * dlcs/dlc_1/effect_behaviors/scene/curse_scene_effect_behavior.tscn
        hp_boost = 150 / damage_boost = 25 / speed_boost = 15 (they override
        the .gd defaults 200 / 50 / 75).
  * dlcs/dlc_1/effect_behaviors/scene/curse_scene_effect_behavior.gd:8
        MAX_CURSE_HP_BOOST = 300
  * dlcs/dlc_1/effect_behaviors/enemy/curse_enemy_effect_behavior.gd:4
        CURSED_ENEMIES_BONUS_GOLD = 0.33

Rows follow the exact BT_KINDS order (brotato_waves.h / brotato_geometry.h /
brotato_enemy_motion.h), re-derived from extract_gameplay.build().

Usage::

    python3 ocean/brotato/extract_enemy_curse.py \
        --campaign-root /Users/me/brotato_recovered \
        --dlc-root      /Users/me/brotato_abyss \
        [--write ocean/brotato/brotato_enemy_curse.h]

Without --write the generated block is printed to stdout.
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import extract_gameplay as G  # noqa: E402
import extract_physics as physics  # noqa: E402


def _read(path):
    with open(path) as fh:
        return fh.read()


def _scalar(text, key, where, cast=float):
    m = re.search(r"\b%s\s*(?::\s*[A-Za-z_]*)?\s*=\s*(-?[0-9]+(?:\.[0-9]+)?)" % re.escape(key), text)
    if not m:
        raise G.ExtractError("%s: %s not found" % (where, key))
    return cast(m.group(1))


def extract(args):
    """Return the per-kind flags (in BT_KINDS order) + the tuning constants."""
    data = G.build(args)
    resolver = data["resolver"]
    enemy_kinds = data["enemy_kinds"]
    if len(enemy_kinds) != 66:
        raise G.ExtractError("expected 66 enemy kinds, got %d" % len(enemy_kinds))
    order = sorted(enemy_kinds, key=lambda s: data["kind_index"][s])

    can_be_cursed = []
    rolls = []
    for scene in order:
        table = physics.flatten_scene(resolver, scene, {})
        rnode = table.get(".") or {}
        stem = data["scene_stem"](scene)
        cursed = 1 if G.get_bool(rnode, "can_be_cursed", True) else 0
        is_loot = 1 if G.get_bool(rnode, "is_loot", False) else 0
        cls = data["kind_class"][scene]
        can_be_cursed.append((cursed, stem))
        # roll gate: NOT a Boss/elite (class 0) and NOT a loot alien
        rolls.append((1 if (cls == 0 and not is_loot) else 0, stem))

    eb = os.path.join(args.dlc_root, "dlcs", "dlc_1", "effect_behaviors")
    scene_gd = os.path.join(eb, "scene", "curse_scene_effect_behavior.gd")
    scene_tscn = os.path.join(eb, "scene", "curse_scene_effect_behavior.tscn")
    enemy_gd = os.path.join(eb, "enemy", "curse_enemy_effect_behavior.gd")
    gd_text = _read(scene_gd)
    ts_text = _read(scene_tscn)
    eg_text = _read(enemy_gd)

    return {
        "can_be_cursed": can_be_cursed,
        "rolls": rolls,
        "hp_boost": _scalar(ts_text, "hp_boost", scene_tscn, int),
        "damage_boost": _scalar(ts_text, "damage_boost", scene_tscn, int),
        "speed_boost": _scalar(ts_text, "speed_boost", scene_tscn, int),
        "max_hp_boost": _scalar(gd_text, "MAX_CURSE_HP_BOOST", scene_gd, int),
        "gold_bonus": _scalar(eg_text, "CURSED_ENEMIES_BONUS_GOLD", enemy_gd, float),
    }


def render(data):
    L = []
    L.append("/* ===================== BEGIN GENERATED =====================")
    L.append(" * extract_enemy_curse.py -- DO NOT EDIT below; regenerate with")
    L.append(" *   python3 ocean/brotato/extract_enemy_curse.py \\")
    L.append(" *       --campaign-root <recovered> --dlc-root <abyss> --write <this file>")
    L.append(" * =========================================================== */")
    L.append("")
    L.append("/* curse_scene_effect_behavior.tscn (the .gd defaults 200/50/75 are")
    L.append(" * overridden by the scene) + .gd MAX_CURSE_HP_BOOST +")
    L.append(" * curse_enemy_effect_behavior.gd CURSED_ENEMIES_BONUS_GOLD. */")
    L.append("#define BT_CURSE_HP_BOOST %d" % data["hp_boost"])
    L.append("#define BT_CURSE_DAMAGE_BOOST %d" % data["damage_boost"])
    L.append("#define BT_CURSE_SPEED_BOOST %d" % data["speed_boost"])
    L.append("#define BT_CURSE_MAX_CURSE_HP_BOOST %d" % data["max_hp_boost"])
    L.append("#define BT_CURSED_ENEMIES_BONUS_GOLD %sf" % G.fmt_float(data["gold_bonus"]))
    L.append("")
    L.append("/* entities/units/enemies/enemy.gd:11 can_be_cursed (scene root;")
    L.append(" * enemy.tscn:52 default true). */")
    L.append("BT_TABLE unsigned char BT_KIND_CAN_BE_CURSED[BT_NKINDS] = {")
    for v, stem in data["can_be_cursed"]:
        L.append("    %du,  /* %s */" % (v, stem))
    L.append("};")
    L.append("")
    L.append("/* curse_scene_effect_behavior.gd:51 the roll/RNG gate: `if enemy is")
    L.append(" * Boss or enemy.stats in _loot_alien_stats: return` -> class 0 and not")
    L.append(" * a loot alien.  1 = _curse_enemy's chance roll runs for this kind. */")
    L.append("BT_TABLE unsigned char BT_KIND_CURSE_ROLLS[BT_NKINDS] = {")
    for v, stem in data["rolls"]:
        L.append("    %du,  /* %s */" % (v, stem))
    L.append("};")
    L.append("/* ====================== END GENERATED ====================== */")
    return "\n".join(L) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--campaign-root", required=True)
    ap.add_argument("--dlc-root", required=True)
    ap.add_argument("--write", default=None)
    ap.add_argument("--header", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "brotato_enemy_curse.h"))
    args = ap.parse_args()

    class A:
        pass
    a = A()
    a.campaign_root = args.campaign_root
    a.dlc_root = args.dlc_root
    block = render(extract(a))

    if args.write:
        src = _read(args.write)
        begin = src.index("/* ===================== BEGIN GENERATED =====================")
        end = src.index("/* ====================== END GENERATED ====================== */")
        end = src.index("\n", end) + 1
        open(args.write, "w").write(src[:begin] + block + src[end:])
    else:
        sys.stdout.write(block)


if __name__ == "__main__":
    main()
