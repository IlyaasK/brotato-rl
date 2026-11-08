"""Tests for ocean/brotato/extract_weapons.py.

Split in two halves:

* Hermetic tests over a tiny synthetic Godot project written into a tmp dir --
  they exercise weapon discovery/validation, canonical ordering + families +
  upgrade chains, the shared effect namespace, scene geometry, base stats /
  scaling, class sets, starting weapons and the attachment ring, without
  needing the recovered game.
* Real-source tests that run only when ``BROTATO_CAMPAIGN_ROOT`` /
  ``BROTATO_DLC_ROOT`` point at the recovered campaign and Abyss DLC trees.
"""

from __future__ import annotations

import importlib.util
import os
import re
import struct
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
_MODULE_PATH = ROOT / "ocean" / "brotato" / "extract_weapons.py"
_spec = importlib.util.spec_from_file_location("extract_weapons_under_test", _MODULE_PATH)
extract_weapons = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(extract_weapons)

items = extract_weapons.items


# --------------------------------------------------------------------------
# synthetic project
# --------------------------------------------------------------------------
ITEM_SERVICE_GD = """\
extends Node

var item_groups: Dictionary = {
\t"group_a": ["item_alpha"],
}

var banned_items_for_endless = ["item_alpha"]
"""

EFFECT_GD = "class_name Effect\nextends Resource\n"
WEAPON_DATA_GD = "class_name WeaponData\nextends Resource\n"
ITEM_DATA_GD = "class_name ItemData\nextends Resource\n"
WEAPON_STATS_GD = "class_name WeaponStats\nextends Resource\n"
MELEE_STATS_GD = "class_name MeleeWeaponStats\nextends WeaponStats\n"
RANGED_STATS_GD = "class_name RangedWeaponStats\nextends WeaponStats\n"
SET_DATA_GD = "class_name SetData\nextends Resource\n"
MELEE_WEAPON_GD = "class_name MeleeWeapon\nextends Node2D\n"


def _write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def _write_bytes(path: Path, blob: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(blob)


def _wav_bytes(rate: int, channels: int, frames: int) -> bytes:
    """A minimal PCM16 RIFF/WAVE stream of exactly `frames` frames."""
    data = b"\x00\x00" * channels * frames
    return (
        b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE"
        + b"fmt " + struct.pack("<IHHIIHH", 16, 1, channels, rate,
                                rate * channels * 2, channels * 2, 16)
        + b"data" + struct.pack("<I", len(data))
    ) + data


def _item_tres(my_id: str) -> str:
    return (
        '[gd_resource type="Resource" format=2]\n\n'
        '[ext_resource path="res://items/global/item_data.gd" type="Script" id=1]\n\n'
        "[resource]\n"
        "script = ExtResource( 1 )\n"
        'my_id = "%s"\n'
        "unlocked_by_default = true\n"
        "tier = 1\nvalue = 5\neffects = [  ]\nmax_nb = -1\ntags = [  ]\n"
        % my_id
    )


def _weapon_scene(name: str, script: str, rect, sprite, hitbox, coll, muzzle,
                  hit_sounds=()) -> str:
    ext = ['[ext_resource path="%s" type="Script" id=1]' % script]
    hits = list(hit_sounds)
    for i, sound in enumerate(hits, start=2):
        ext.append('[ext_resource path="%s" type="AudioStream" id=%d]' % (sound, i))
    root = "script = ExtResource( 1 )\n"
    if hits:
        root += "custom_hit_sounds = [ %s ]\n" % ", ".join(
            "ExtResource( %d )" % i for i in range(2, 2 + len(hits)))
    return (
        '[gd_scene load_steps=%d format=2]\n\n' % (3 + len(hits))
        + "\n".join(ext) + "\n"
        '[sub_resource type="RectangleShape2D" id=1]\n'
        "extents = Vector2( %g, %g )\n"
        '[sub_resource type="CircleShape2D" id=2]\n'
        "radius = 50.0\n\n"
        '[node name="%s" type="Node2D"]\n'
        % (rect[0], rect[1], name)
        + root
        + '[node name="Sprite" type="Sprite" parent="."]\n'
        "position = Vector2( %g, %g )\n"
        '[node name="Hitbox" type="Area2D" parent="Sprite"]\n'
        "position = Vector2( %g, %g )\n"
        '[node name="Collision" type="CollisionShape2D" parent="Sprite/Hitbox"]\n'
        "position = Vector2( %g, %g )\n"
        "shape = SubResource( 1 )\n"
        '[node name="Muzzle" type="Position2D" parent="Sprite"]\n'
        "position = Vector2( %g, %g )\n"
        '[node name="Range" type="Area2D" parent="."]\n'
        '[node name="CollisionShape2D" type="CollisionShape2D" parent="Range"]\n'
        "shape = SubResource( 2 )\n"
        % (sprite[0], sprite[1], hitbox[0], hitbox[1], coll[0], coll[1],
           muzzle[0], muzzle[1])
    )


def _stats_tres(script: str, cooldown: int, damage: int, max_range: int,
                shoot_sounds=()) -> str:
    ext = ['[ext_resource path="%s" type="Script" id=1]' % script]
    sounds = list(shoot_sounds)
    for i, sound in enumerate(sounds, start=2):
        ext.append('[ext_resource path="%s" type="AudioStream" id=%d]' % (sound, i))
    refs = ", ".join("ExtResource( %d )" % i for i in range(2, 2 + len(sounds)))
    return (
        '[gd_resource type="Resource" load_steps=%d format=2]\n\n' % (1 + len(sounds))
        + "\n".join(ext) + "\n\n[resource]\n"
        "script = ExtResource( 1 )\n"
        "cooldown = %d\ndamage = %d\nmax_range = %d\n"
        'scaling_stats = [ [ "stat_melee_damage", 1.0 ] ]\n'
        "shooting_sounds = [ %s ]\n"
        % (cooldown, damage, max_range, refs)
    )


def _weapon_tres(my_id, weapon_id, tier, value, kind, scene, stats, upgrades,
                 sets, start_chars) -> str:
    ext = ['[ext_resource path="res://items/global/weapon_data.gd" type="Script" id=1]',
           '[ext_resource path="%s" type="PackedScene" id=2]' % scene,
           '[ext_resource path="%s" type="Resource" id=3]' % stats]
    nid = 4
    up_ref = ""
    if upgrades:
        ext.append('[ext_resource path="%s" type="Resource" id=%d]' % (upgrades, nid))
        up_ref = "upgrades_into = ExtResource( %d )\n" % nid
        nid += 1
    set_refs = []
    for s in sets:
        ext.append('[ext_resource path="%s" type="Resource" id=%d]' % (s, nid))
        set_refs.append("ExtResource( %d )" % nid)
        nid += 1
    start = ", ".join('"%s"' % c for c in start_chars)
    return (
        "[gd_resource type=\"Resource\" load_steps=%d format=2]\n\n" % (nid)
        + "\n".join(ext) + "\n\n[resource]\n"
        "script = ExtResource( 1 )\n"
        'my_id = "%s"\n'
        "unlocked_by_default = true\ncan_be_looted = true\n"
        'name = "%s"\ntier = %d\nvalue = %d\neffects = [  ]\n'
        'weapon_id = "%s"\ntype = %d\nsets = [ %s ]\n'
        "scene = ExtResource( 2 )\nstats = ExtResource( 3 )\n"
        "%sadd_to_chars_as_starting = [ %s ]\nis_lockable = true\ncurse_factor = 0.0\n"
        % (my_id, my_id.upper(), tier, value, weapon_id, kind,
           ", ".join(set_refs), up_ref, start)
    )


def _set_tres(my_id, name, bonus_paths) -> str:
    ext = ['[ext_resource path="res://items/sets/set_data.gd" type="Script" id=1]']
    refs = []
    for i, p in enumerate(bonus_paths, start=2):
        ext.append('[ext_resource path="%s" type="Resource" id=%d]' % (p, i))
        refs.append("ExtResource( %d )" % i)
    groups = ", ".join("[ %s ]" % r for r in refs)
    return (
        '[gd_resource type="Resource" format=2]\n\n' + "\n".join(ext) + "\n\n[resource]\n"
        "script = ExtResource( 1 )\n"
        'my_id = "%s"\nname = "%s"\nset_bonuses = [ %s ]\n' % (my_id, name, groups)
    )


def _player_tscn() -> str:
    names = ("One", "Two", "Three", "Four", "Five", "Six")
    pos = {
        1: [(0, 30)],
        2: [(40, 20), (-40, 20)],
        3: [(40, 20), (-40, 20), (0, -45)],
        4: [(40, 35), (-40, 35), (-40, -30), (40, -30)],
        5: [(35, 35), (-35, 35), (-55, -20), (55, -20), (0, -55)],
        6: [(35, 45), (-35, 45), (-60, 0), (60, 0), (-35, -45), (35, -45)],
    }
    out = ['[gd_scene format=2]\n', '[node name="Player" type="Node2D"]\n']
    out.append('[node name="Weapons" type="Node2D" parent="."]\n')
    for count in range(1, 7):
        out.append('[node name="%s" type="Node2D" parent="Weapons"]\n' % names[count - 1])
        for slot in range(count):
            out.append('[node name="Attach%d" type="Position2D" parent="Weapons/%s"]\n'
                       % (slot + 1, names[count - 1]))
            out.append("position = Vector2( %d, %d )\n" % pos[count][slot])
    return "".join(out)


def build_project(tmp_path: Path):
    campaign = tmp_path / "campaign"
    dlc = tmp_path / "dlc"
    _write(campaign / "singletons" / "item_service.gd", ITEM_SERVICE_GD)
    _write(campaign / "items" / "global" / "effect.gd", EFFECT_GD)
    _write(campaign / "items" / "global" / "item_data.gd", ITEM_DATA_GD)
    _write(campaign / "items" / "global" / "weapon_data.gd", WEAPON_DATA_GD)
    _write(campaign / "items" / "sets" / "set_data.gd", SET_DATA_GD)
    _write(campaign / "weapons" / "weapon_stats" / "weapon_stats.gd", WEAPON_STATS_GD)
    _write(campaign / "weapons" / "weapon_stats" / "melee_weapon_stats.gd", MELEE_STATS_GD)
    _write(campaign / "weapons" / "weapon_stats" / "ranged_weapon_stats.gd", RANGED_STATS_GD)
    _write(campaign / "weapons" / "melee" / "melee_weapon.gd", MELEE_WEAPON_GD)
    _write(campaign / "items" / "all" / "alpha" / "alpha_data.tres", _item_tres("item_alpha"))
    _write(campaign / "items" / "characters" / "bull" / "bull_data.tres",
           '[gd_resource type="Resource" format=2]\n\n[resource]\n'
           'wanted_tags = [ ]\nbanned_item_groups = [ ]\nbanned_items = [ ]\n')
    # axes: two melee tiers of the same family (weapon_axe), tier 0 upgrades -> 1.
    # Both tiers reference the SAME ordered shooting_sounds clips, and the axe
    # scene (shared by both tiers) carries one custom_hit_sounds export.
    axe_swings = ["res://weapons/audio/axe_swing_01.wav",
                  "res://weapons/audio/axe_swing_02.wav"]
    axe_impact = "res://weapons/audio/axe_impact.wav"
    _write_bytes(campaign / "weapons" / "audio" / "axe_swing_01.wav",
                 _wav_bytes(48000, 1, 480))      # 0.01 s
    _write_bytes(campaign / "weapons" / "audio" / "axe_swing_02.wav",
                 _wav_bytes(48000, 1, 960))      # 0.02 s
    _write_bytes(campaign / "weapons" / "audio" / "axe_impact.wav",
                 _wav_bytes(44100, 2, 1323))     # 0.03 s
    _write(campaign / "weapons" / "melee" / "axe" / "axe.tscn",
           _weapon_scene("Axe", "res://weapons/melee/melee_weapon.gd",
                         (10, 4), (5, 1), (2, 0), (-1, 0), (9, 0),
                         hit_sounds=[axe_impact]))
    _write(campaign / "weapons" / "melee" / "axe" / "1" / "axe_stats.tres",
           _stats_tres("res://weapons/weapon_stats/melee_weapon_stats.gd", 10, 4, 100,
                       shoot_sounds=axe_swings))
    _write(campaign / "weapons" / "melee" / "axe" / "2" / "axe_2_stats.tres",
           _stats_tres("res://weapons/weapon_stats/melee_weapon_stats.gd", 8, 8, 140,
                       shoot_sounds=axe_swings))
    _write(campaign / "weapons" / "melee" / "axe" / "1" / "axe_data.tres",
           _weapon_tres("weapon_axe_1", "weapon_axe", 0, 30, 0,
                        "res://weapons/melee/axe/axe.tscn",
                        "res://weapons/melee/axe/1/axe_stats.tres",
                        "res://weapons/melee/axe/2/axe_2_data.tres",
                        ["res://items/sets/primitive/primitive_set_data.tres"],
                        ["character_knight"]))
    _write(campaign / "weapons" / "melee" / "axe" / "2" / "axe_2_data.tres",
           _weapon_tres("weapon_axe_2", "weapon_axe", 1, 60, 0,
                        "res://weapons/melee/axe/axe.tscn",
                        "res://weapons/melee/axe/2/axe_2_stats.tres",
                        None, [], []))
    # a second family (weapon_club) on a different scene.
    _write(campaign / "weapons" / "melee" / "club" / "club.tscn",
           _weapon_scene("Club", "res://weapons/melee/melee_weapon.gd",
                         (20, 6), (0, 0), (3, 0), (0, 0), (30, 0)))
    _write(campaign / "weapons" / "melee" / "club" / "1" / "club_stats.tres",
           _stats_tres("res://weapons/weapon_stats/melee_weapon_stats.gd", 20, 6, 90))
    _write(campaign / "weapons" / "melee" / "club" / "1" / "club_data.tres",
           _weapon_tres("weapon_club_1", "weapon_club", 0, 25, 0,
                        "res://weapons/melee/club/club.tscn",
                        "res://weapons/melee/club/1/club_stats.tres", None, [], []))
    # set + its tier-1 bonus effect.
    _write(campaign / "items" / "sets" / "primitive" / "2" / "set_2_effect_1.tres",
           '[gd_resource type="Resource" load_steps=2 format=2]\n\n'
           '[ext_resource path="res://items/global/effect.gd" type="Script" id=1]\n\n'
           "[resource]\nscript = ExtResource( 1 )\n"
           'key = "stat_max_hp"\nvalue = 3\ncustom_args = [  ]\n')
    _write(campaign / "items" / "sets" / "primitive" / "primitive_set_data.tres",
           _set_tres("set_primitive", "WEAPON_CLASS_PRIMITIVE",
                     ["res://items/sets/primitive/2/set_2_effect_1.tres"]))
    _write(campaign / "entities" / "units" / "player" / "player.tscn", _player_tscn())
    _write(campaign / "projectiles" / "bullet" / "bullet_projectile.tscn",
           '[gd_scene load_steps=2 format=2]\n\n'
           '[sub_resource type="CircleShape2D" id=1]\nradius = 44.0\n\n'
           '[node name="BulletProjectile" type="Node2D"]\n'
           '[node name="Hitbox" type="Area2D" parent="."]\n'
           "position = Vector2( -23, 1 )\n"
           '[node name="Collision" type="CollisionShape2D" parent="Hitbox"]\n'
           "shape = SubResource( 1 )\n")
    # unregistered template (empty weapon_id) that must be reported, not registered.
    _write(campaign / "weapons" / "melee" / "template" / "template_data.tres",
           _weapon_tres("weapon_template", "", 0, 1, 0,
                        "res://weapons/melee/axe/axe.tscn",
                        "res://weapons/melee/axe/1/axe_stats.tres", None, [], []))
    _write(campaign / "singletons" / "item_service.tscn",
           '[gd_scene load_steps=6 format=2]\n\n'
           '[ext_resource path="res://singletons/item_service.gd" type="Script" id=1]\n'
           '[ext_resource path="res://items/all/alpha/alpha_data.tres" type="Resource" id=2]\n'
           '[ext_resource path="res://weapons/melee/axe/1/axe_data.tres" type="Resource" id=3]\n'
           '[ext_resource path="res://weapons/melee/axe/2/axe_2_data.tres" type="Resource" id=4]\n'
           '[ext_resource path="res://weapons/melee/club/1/club_data.tres" type="Resource" id=5]\n\n'
           '[node name="ItemService" type="Node"]\n'
           "script = ExtResource( 1 )\n"
           "items = [ ExtResource( 2 ) ]\n"
           "weapons = [ ExtResource( 3 ), ExtResource( 4 ), ExtResource( 5 ) ]\n")
    # DLC: one ranged weapon.
    _write(dlc / "dlcs" / "dlc_1" / "dlc_1_data.gd",
           "extends DLCData\n\nexport (float) var base_item_curse_chance: = 0.03\n")
    _write(dlc / "dlcs" / "dlc_1" / "items" / "gamma" / "gamma_data.tres",
           _item_tres("item_gamma"))
    _write(dlc / "dlcs" / "dlc_1" / "weapons" / "ranged" / "gun" / "gun.tscn",
           _weapon_scene("Gun", "res://weapons/ranged/ranged_weapon.gd",
                         (14, 5), (6, 0), (2, 1), (0, 0), (12, 0)))
    _write(dlc / "dlcs" / "dlc_1" / "weapons" / "ranged" / "gun" / "1" / "gun_stats.tres",
           _stats_tres("res://weapons/weapon_stats/ranged_weapon_stats.gd", 30, 7, 260))
    _write(dlc / "dlcs" / "dlc_1" / "weapons" / "ranged" / "gun" / "1" / "gun_data.tres",
           _weapon_tres("weapon_gun_1", "weapon_gun", 0, 40, 1,
                        "res://dlcs/dlc_1/weapons/ranged/gun/gun.tscn",
                        "res://dlcs/dlc_1/weapons/ranged/gun/1/gun_stats.tres", None, [], []))
    _write(dlc / "dlcs" / "dlc_1" / "dlc_data.tres",
           '[gd_resource type="Resource" format=2]\n\n'
           '[ext_resource path="res://dlcs/dlc_1/items/gamma/gamma_data.tres" '
           'type="Resource" id=1]\n'
           '[ext_resource path="res://dlcs/dlc_1/weapons/ranged/gun/1/gun_data.tres" '
           'type="Resource" id=2]\n\n[resource]\n'
           "items = [ ExtResource( 1 ) ]\n"
           "weapons = [ ExtResource( 2 ) ]\n")
    return campaign, dlc


def run_extract(tmp_path: Path, campaign: Path, dlc: Path, tag: str = "a"):
    args = extract_weapons.argparse.Namespace(
        campaign_root=str(campaign), dlc_root=str(dlc),
        items_out=str(tmp_path / tag / "brotato_items.h"),
        effects_out=str(tmp_path / tag / "brotato_item_effects.h"),
        weapons_out=str(tmp_path / tag / "brotato_weapons.h"),
        weapon_types_out=str(tmp_path / tag / "brotato_weapon_types.h"),
        weapon_contract_out=str(tmp_path / tag / "weapons_contract.md"),
        contract_out=str(tmp_path / tag / "contract.md"),
        provenance_out=str(tmp_path / tag / "prov.json"),
        weapon_provenance_out=str(tmp_path / tag / "wprov.json"))
    return items.emit_all(args)


# --------------------------------------------------------------------------
# hermetic tests
# --------------------------------------------------------------------------
def test_pool_order_family_and_upgrades(tmp_path):
    campaign, dlc = build_project(tmp_path)
    data = run_extract(tmp_path, campaign, dlc)
    wdata = data["weapons"]
    assert [w["my_id"] for w in wdata["weapons"]] == [
        "weapon_axe_1", "weapon_axe_2", "weapon_club_1", "weapon_gun_1"]
    axe1, axe2, club, gun = wdata["weapons"]
    assert axe1["family"] == axe2["family"] == 0
    assert club["family"] == 2
    assert axe1["upgrades_into"] == 1
    assert axe2["upgrades_into"] == 0xFFFF
    assert (gun["kind"], gun["tier"]) == (1, 0)
    assert wdata["dir_stats"]["campaign"]["templates"] == [
        "res://weapons/melee/template/template_data.tres"]
    assert data["builder"].strings.values[data["builder"].strings.ids[gun["weapon_id"]]] == "weapon_gun"


def test_effect_namespace_is_shared_with_no_item_sentinel(tmp_path):
    campaign, dlc = build_project(tmp_path)
    data = run_extract(tmp_path, campaign, dlc)
    builder = data["builder"]
    wdata = data["weapons"]
    item_rows = [r for r in builder.rows if r["item"] != items.BT_ITEM_NONE]
    weapon_rows = [r for r in builder.rows if r["item"] == items.BT_ITEM_NONE]
    assert len(item_rows) == 0  # the synthetic item has no effects
    # The set-bonus effect is a weapon-owned row (item == BT_ITEM_NONE).
    assert len(weapon_rows) == 1
    assert builder.rows[wdata["set_effects"][0]]["source"].endswith("set_2_effect_1.tres")
    # weapon effect rows are canonical BT_ITEM_EFF_ROWS rows (no separate table).
    assert all(0 <= v < len(builder.rows) for v in wdata["weapon_eff_index"])
    # Nothing in the emitted weapons header declares a second row array.
    weapons_h = (tmp_path / "a" / "brotato_weapons.h").read_text()
    assert "BT_WEAPON_EFF_ROWS" not in weapons_h
    assert "BT_WEAPON_BURNING" not in weapons_h


def test_geometry_and_stats(tmp_path):
    campaign, dlc = build_project(tmp_path)
    data = run_extract(tmp_path, campaign, dlc)
    wdata = data["weapons"]
    geom = wdata["geom_rows"][wdata["weapons"][0]["geom"]]
    assert (geom["shape"], geom["is_melee"]) == (1, 1)
    assert (geom["hit_rect_hx"], geom["hit_rect_hy"]) == (10.0, 4.0)
    assert (geom["sprite_x"], geom["sprite_y"]) == (5.0, 1.0)
    assert (geom["hit_x"], geom["hit_y"]) == (6.0, 1.0)     # sprite + hitbox + coll
    assert (geom["muzzle_x"], geom["muzzle_y"]) == (14.0, 1.0)
    assert geom["range_radius"] == 50.0
    stats = wdata["base_stats_rows"][wdata["weapons"][0]["stats"]]
    assert (stats["kind"], stats["cooldown"], stats["damage"], stats["max_range"]) == (1, 10, 4, 100)
    assert stats["scaling_count"] == 1
    assert wdata["base_scaling"][stats["scaling_first"]][1] == pytest.approx(1.0)


def test_weapon_audio_spans_and_durations(tmp_path):
    campaign, dlc = build_project(tmp_path)
    data = run_extract(tmp_path, campaign, dlc)
    wdata = data["weapons"]
    by_id = {w["my_id"]: w for w in wdata["weapons"]}
    axe1, axe2 = by_id["weapon_axe_1"], by_id["weapon_axe_2"]
    club, gun = by_id["weapon_club_1"], by_id["weapon_gun_1"]

    # shooting_sounds (WeaponStats inherited resource) -> one shared span across
    # both tiers, because both stats rows list the same clips in the same order.
    assert (axe1["shoot_audio_count"], axe1["shooting_sounds"]) == (2, 2)
    assert axe1["shoot_audio_first"] == axe2["shoot_audio_first"]
    # custom_hit_sounds is a scene-root export; both tiers share the axe scene.
    assert axe1["hit_audio_count"] == 1
    assert axe1["hit_audio_first"] == axe2["hit_audio_first"]
    # Empty source arrays are a ZERO span: the source guard skips the draw.
    assert (club["shoot_audio_count"], club["hit_audio_count"]) == (0, 0)
    assert (gun["shoot_audio_count"], gun["hit_audio_count"]) == (0, 0)

    durations = wdata["audio_durations"]
    assert len(durations) == 3
    shoot = [durations[axe1["shoot_audio_first"] + i] for i in (0, 1)]
    assert [d["path"] for d in shoot] == [
        "res://weapons/audio/axe_swing_01.wav", "res://weapons/audio/axe_swing_02.wav"]
    assert [d["seconds"] for d in shoot] == [pytest.approx(0.01), pytest.approx(0.02)]
    impact = durations[axe1["hit_audio_first"]]
    assert (impact["path"], impact["seconds"]) == (
        "res://weapons/audio/axe_impact.wav", pytest.approx(0.03))

    header = (tmp_path / "a" / "brotato_weapons.h").read_text()
    assert "BT_TABLE float BT_WEAPON_AUDIO_DURATION[BT_NWEAPON_AUDIO_DURATION]" in header
    assert "#define BT_NWEAPON_AUDIO_DURATION 3" in header
    # The nested on-hit table is canonical in brotato_audio_data.h; this
    # generator must not emit a second copy.
    assert "BT_WEAPON_ONHIT_AUDIO" not in header
    assert "BtWeaponAudioSpan" not in header


def test_sets_and_starting(tmp_path):
    campaign, dlc = build_project(tmp_path)
    data = run_extract(tmp_path, campaign, dlc)
    wdata = data["weapons"]
    assert len(wdata["set_rows"]) == 1
    assert wdata["set_rows"][0]["my_id"] == data["builder"].strings.ids["set_primitive"]
    assert wdata["set_rows"][0]["nb_tiers"] == 1
    assert wdata["set_tiers"] == [(0, 1)]
    assert data["builder"].strings.values[wdata["start"][0]] == "character_knight"
    assert wdata["attach"][1][0] == (0.0, 30.0)
    assert wdata["attach"][6][5] == (35.0, -45.0)


def test_proj_spec_default_row(tmp_path):
    campaign, dlc = build_project(tmp_path)
    data = run_extract(tmp_path, campaign, dlc)
    proj = data["weapons"]["proj_rows"]
    assert proj[0]["scene"] == 0        # BT_ISTR_NONE == default bullet
    assert proj[0]["hit_radius"] == 44.0
    assert (proj[0]["hit_x"], proj[0]["hit_y"]) == (-23.0, 1.0)
    header = (tmp_path / "a" / "brotato_weapons.h").read_text()
    assert "#define BT_WEAPON_PROJ_DEFAULT 0" in header
    assert "bt_weapon_proj_spec" in header


def test_headers_declare_tables_and_macros(tmp_path):
    campaign, dlc = build_project(tmp_path)
    run_extract(tmp_path, campaign, dlc)
    weapons_h = (tmp_path / "a" / "brotato_weapons.h").read_text()
    types_h = (tmp_path / "a" / "brotato_weapon_types.h").read_text()
    assert "#define BT_NWEAPONS 4" in weapons_h
    assert "BT_TABLE BtWeaponDef BT_WEAPONS[BT_NWEAPONS]" in weapons_h
    assert "BT_TABLE BtItemWeaponStats BT_WEAPON_STATS[BT_NWEAPON_STATS]" in weapons_h
    assert "BT_TABLE BtItemScaling BT_WEAPON_SCALING[BT_NWEAPON_SCALING]" in weapons_h
    assert "#define BT_WEAPON_SOUND_COUNT(W)" in weapons_h
    assert "BT_TABLE float BT_WEAPON_AUDIO_DURATION[BT_NWEAPON_AUDIO_DURATION]" in weapons_h
    assert "#define BT_NWEAPON_AUDIO_DURATION 3" in weapons_h
    assert "bt_weapon_index_from_id" in weapons_h
    assert "} BtWeaponDef;" in types_h
    assert "} BtWeaponProjSpec;" in types_h
    assert "} BtWeaponOnHit;" in types_h
    assert "#define BT_WEAPON_NONE 0xFFFFu" in types_h
    assert "#define BT_WEAPON_IDX_WEAPON_AXE_1 0" in weapons_h
    assert "BT_WEAPON_FX_CURSE_MAX" in weapons_h


def test_regeneration_is_byte_identical(tmp_path):
    campaign, dlc = build_project(tmp_path)
    run_extract(tmp_path, campaign, dlc, "one")
    names = ("brotato_items.h", "brotato_item_effects.h", "brotato_weapons.h",
             "brotato_weapon_types.h", "weapons_contract.md", "wprov.json")
    first = {name: (tmp_path / "one" / name).read_text() for name in names}
    run_extract(tmp_path, campaign, dlc, "one")
    for name in names:
        assert first[name] == (tmp_path / "one" / name).read_text(), name


def test_weapon_only_opcodes_are_appended(tmp_path):
    campaign, dlc = build_project(tmp_path)
    data = run_extract(tmp_path, campaign, dlc)
    opcodes = [r["opcode"] for r in data["builder"].rows if r["item"] == items.BT_ITEM_NONE]
    assert "Effect" in opcodes  # the set-bonus effect
    header = (tmp_path / "a" / "brotato_item_effects.h").read_text()
    assert "#define BT_ITEMEFF_UNSUPPORTED 0" in header
    # item opcode numbers are stable; a synthetic-only project has just the
    # item-derived classes of the single item-less pool.


def test_unregistered_template_is_excluded_not_registered(tmp_path):
    campaign, dlc = build_project(tmp_path)
    data = run_extract(tmp_path, campaign, dlc)
    assert all(w["my_id"] != "weapon_template" for w in data["weapons"]["weapons"])


# --------------------------------------------------------------------------
# real-source tests (opt-in)
# --------------------------------------------------------------------------
CAMPAIGN = os.environ.get("BROTATO_CAMPAIGN_ROOT")
DLC = os.environ.get("BROTATO_DLC_ROOT")
needs_source = pytest.mark.skipif(
    not (CAMPAIGN and DLC and Path(CAMPAIGN).is_dir() and Path(DLC).is_dir()),
    reason="set BROTATO_CAMPAIGN_ROOT / BROTATO_DLC_ROOT to the recovered trees")


def _real_args(tmp_path):
    return extract_weapons.argparse.Namespace(
        campaign_root=CAMPAIGN, dlc_root=DLC,
        items_out=str(tmp_path / "brotato_items.h"),
        effects_out=str(tmp_path / "brotato_item_effects.h"),
        weapons_out=str(tmp_path / "brotato_weapons.h"),
        weapon_types_out=str(tmp_path / "brotato_weapon_types.h"),
        weapon_contract_out=str(tmp_path / "weapons_contract.md"),
        contract_out=str(tmp_path / "contract.md"),
        provenance_out=str(tmp_path / "prov.json"),
        weapon_provenance_out=str(tmp_path / "wprov.json"))


@needs_source
def test_real_pool_is_complete(tmp_path):
    data = items.emit_all(_real_args(tmp_path))
    wdata = data["weapons"]
    assert wdata is not None
    assert len(wdata["campaign"]) == 201
    assert len(wdata["dlc"]) == 52
    assert len(wdata["weapons"]) == 253
    assert len({w["my_id"] for w in wdata["weapons"]}) == 253
    resolver = data["builder"].R
    for w in wdata["weapons"]:
        assert Path(resolver.find(w["source"])).is_file()


@needs_source
def test_real_upgrade_chains_and_families(tmp_path):
    data = items.emit_all(_real_args(tmp_path))
    weapons = data["weapons"]["weapons"]
    by_id = {w["my_id"]: w for w in weapons}
    assert by_id["weapon_hatchet_1"]["upgrades_into"] == by_id["weapon_hatchet_2"]["index"]
    for w in weapons:
        assert 0 <= w["family"] < len(weapons)
        assert weapons[w["family"]]["weapon_id"] == w["weapon_id"]
        if w["upgrades_into"] != 0xFFFF:
            nxt = weapons[w["upgrades_into"]]
            assert nxt["weapon_id"] == w["weapon_id"]
            assert nxt["tier"] == w["tier"] + 1


@needs_source
def test_real_effect_namespace_preserved(tmp_path):
    data = items.emit_all(_real_args(tmp_path))
    header = (tmp_path / "brotato_item_effects.h").read_text()
    assert "#define BT_ITEMEFF_BURNING 7" in header
    assert "#define BT_ITEMEFF_EXPLODING 15" in header
    # weapon-only opcodes come after the item opcodes.
    assert "#define BT_ITEMEFF_WEAPON_STACK 4" in header
    wdata = data["weapons"]
    nrows = len(data["builder"].rows)
    assert all(0 <= v < nrows for v in wdata["weapon_eff_index"])


@needs_source
def test_real_spawns_structure_and_geometry(tmp_path):
    data = items.emit_all(_real_args(tmp_path))
    weapons = data["weapons"]["weapons"]
    spawn = {w["id"] for w in weapons if w["spawns_structure"]}
    assert spawn == {"weapon_screwdriver", "weapon_wrench", "weapon_pruner"}
    for g in data["weapons"]["geom_rows"]:
        assert g["shape"] in (0, 1, 2)


@needs_source
def test_real_proj_spec(tmp_path):
    data = items.emit_all(_real_args(tmp_path))
    proj = {r["path"]: r for r in data["weapons"]["proj_rows"]}
    assert proj["res://projectiles/bullet/bullet_projectile.tscn"]["hit_radius"] == 44.0
    assert proj["res://projectiles/shuriken/shuriken_projectile.tscn"]["rot_speed"] == 25.0
    assert proj["res://projectiles/fireball_projectile/fireball_projectile.tscn"]["stop_delay"] == 20.0
    taser = proj["res://projectiles/bullet_taser/taser_projectile.tscn"]
    assert taser["slow_rad"] > 0.0 and taser["slow_amt"] == -200.0
    harpoon = proj["res://dlcs/dlc_1/projectiles/bullet_harpoon/bullet_harpoon.tscn"]
    assert harpoon["slow_amt"] == -400.0


@needs_source
def test_real_fx_curse_max_and_keys(tmp_path):
    data = items.emit_all(_real_args(tmp_path))
    weapons_h = (tmp_path / "brotato_weapons.h").read_text()
    effects_h = (tmp_path / "brotato_item_effects.h").read_text()
    assert "#define BT_WEAPON_FX_CURSE_MAX 2" in weapons_h
    assert "BT_ISTR_CAN_BURN_ENEMIES" in effects_h
    assert "BT_ISTR_CAN_ATTACK_WHILE_MOVING" in effects_h


@needs_source
def test_real_class_and_type_bonus_are_not_in_the_pool(tmp_path):
    # ClassBonusEffect / WeaponTypeBonusEffect are reachable only from character
    # effect resources (extract_characters.py), never from items/weapons/set
    # tiers -> they must not appear in this pool's effect graph.
    data = items.emit_all(_real_args(tmp_path))
    classes = {r["opcode"] for r in data["builder"].rows}
    assert "ClassBonusEffect" not in classes
    assert "WeaponTypeBonusEffect" not in classes


@needs_source
def test_real_geometry_matches_oracle(tmp_path):
    data = items.emit_all(_real_args(tmp_path))
    by_my = {w["my_id"]: w for w in data["weapons"]["weapons"]}
    geom = data["weapons"]["geom_rows"]

    def row(my_id):
        return geom[by_my[my_id]["geom"]]

    claw = row("weapon_claw_1")
    assert (claw["shape"], claw["hit_x"], claw["hit_y"],
            claw["hit_rect_hx"], claw["hit_rect_hy"]) == (1, 12.0, 0.0, 60.0, 20.0)
    chopper = row("weapon_chopper_1")
    assert (chopper["shape"], chopper["hit_x"], chopper["hit_y"],
            chopper["hit_rect_hx"], chopper["hit_rect_hy"]) == (1, 8.0, 5.0, 72.0, 16.0)
    sword = row("weapon_sword_2")
    assert (sword["shape"], sword["hit_x"], sword["hit_y"],
            sword["hit_rect_hx"], sword["hit_rect_hy"]) == (1, -8.0, 0.0, 112.0, 32.0)


@needs_source
def test_real_weapon_audio_clips_and_durations(tmp_path):
    data = items.emit_all(_real_args(tmp_path))
    wdata = data["weapons"]
    weapons = wdata["weapons"]
    durations = wdata["audio_durations"]

    # Every clip length is a real, positive source length (WAV data size / MPEG
    # frames), and every tier variant resolves a non-empty shooting_sounds row.
    assert len(durations) == 80
    assert all(d["seconds"] > 0.0 for d in durations)
    assert all(w["shoot_audio_count"] > 0 for w in weapons)
    for w in weapons:
        assert w["shoot_audio_first"] + w["shoot_audio_count"] <= len(durations)
        assert w["hit_audio_first"] + w["hit_audio_count"] <= len(durations)
    # Identical ordered clip sequences share a span: 253 variants -> 21 spans.
    assert len({(w["shoot_audio_first"], w["shoot_audio_count"])
                for w in weapons}) == 21
    # Single-clip arrays still draw once; 16 variants carry exactly one clip.
    assert sum(1 for w in weapons if w["shoot_audio_count"] == 1) == 16

    # The scene custom_hit_sounds export exists only on the lute (6 clips).
    hit = [w for w in weapons if w["hit_audio_count"]]
    assert {w["id"] for w in hit} == {"weapon_lute"}
    assert all(w["hit_audio_count"] == 6 for w in hit)
    assert sum(1 for w in weapons if w["hit_audio_count"] == 0) == 249
    lute = hit[0]
    assert [durations[lute["hit_audio_first"] + i]["path"] for i in range(6)] == [
        "res://dlcs/dlc_1/weapons/melee/lute/Lute_3.wav",
        "res://dlcs/dlc_1/weapons/melee/lute/Lute_4.wav",
        "res://dlcs/dlc_1/weapons/melee/lute/Lute_6.wav",
        "res://dlcs/dlc_1/weapons/melee/lute/Lute_7.wav",
        "res://dlcs/dlc_1/weapons/melee/lute/Lute_8.wav",
        "res://dlcs/dlc_1/weapons/melee/lute/Lute_9.wav"]

    by_path = {d["path"]: d["seconds"] for d in durations}
    # WAV: PCM16 data bytes / (rate * channels * 2), the AudioStreamSample length.
    assert by_path["res://weapons/melee_sounds/whoosh_weapon_knife_swing_01.wav"] \
        == pytest.approx(0.291916668, abs=1e-7)
    assert by_path["res://dlcs/dlc_1/resources/sounds/TOOL_Chainsaw_1_Burst_01_mono.wav"] \
        == pytest.approx(0.352494329, abs=1e-7)
    # MP3: summed MPEG frames (75 * 1152 / 48000), the AudioStreamMP3 length.
    fireball = [w for w in weapons if w["id"] == "weapon_fireball"]
    assert fireball and all(w["shoot_audio_count"] == 1 for w in fireball)
    mp3 = durations[fireball[0]["shoot_audio_first"]]
    assert mp3["path"] == "res://weapons/ranged/fireball/Fire 5 (DPS).mp3"
    assert mp3["seconds"] == pytest.approx(1.8, abs=1e-7)

    # The nested on-hit stats -> clip span table is CombatAudioData's canonical
    # BT_AUDIO_WSTATS_SHOOT_FIRST/COUNT (brotato_audio_data.h); this generator
    # must not emit a competing copy.
    for name in ("brotato_weapons.h", "brotato_weapon_types.h"):
        text = (tmp_path / name).read_text()
        assert "BT_WEAPON_ONHIT_AUDIO" not in text
        assert "BtWeaponAudioSpan" not in text
        assert "bt_weapon_onhit_audio" not in text


@needs_source
def test_real_regeneration_is_byte_identical(tmp_path):
    out = tmp_path / "one"
    out.mkdir()
    items.emit_all(_real_args(out))
    names = ("brotato_items.h", "brotato_item_effects.h", "brotato_weapons.h",
             "brotato_weapon_types.h")
    first = {name: (out / name).read_text() for name in names}
    items.emit_all(_real_args(out))
    for name in names:
        assert first[name] == (out / name).read_text(), name
