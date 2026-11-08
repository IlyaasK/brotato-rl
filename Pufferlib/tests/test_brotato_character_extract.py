"""Tests for ocean/brotato/extract_characters.py.

Hermetic tests build a small synthetic campaign + DLC (item pool, item groups
and the three character directories) and assert the extraction's structured
result; an opt-in test runs against the recovered sources when
BROTATO_CAMPAIGN_ROOT / BROTATO_DLC_ROOT are set.
"""

from __future__ import annotations

import importlib.util
import os
import re
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
_MODULE_PATH = ROOT / "ocean" / "brotato" / "extract_characters.py"
_spec = importlib.util.spec_from_file_location("extract_characters_under_test", _MODULE_PATH)
extract_characters = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(extract_characters)


# --------------------------------------------------------------------------
# synthetic project
# --------------------------------------------------------------------------
ITEM_SERVICE_GD = """\
extends Node

var item_groups: Dictionary = {
\t"group_a": ["item_alpha"],
}

var banned_items_for_endless = ["item_beta"]
"""

EFFECT_GD = "class_name Effect\nextends Resource\n"
ITEM_DATA_GD = "class_name ItemData\nextends Resource\n"
CHARACTER_DATA_GD = "class_name CharacterData\nextends ItemData\n"
WEAPON_DATA_GD = "class_name WeaponData\nextends ItemData\n"
STAT_GAINS_GD = ("class_name StatGainsModificationEffect\nextends Effect\n"
                 "export(Array, String) var stats_modified\n")
STAT_LINK_GD = ("class_name GainStatForEveryStatEffect\nextends Effect\n"
                "export(int) var nb_stat_scaled = 0\n"
                'export(String) var stat_scaled = ""\n'
                "export(bool) var perm_stats_only = true\n")
CONVERT_GD = ("class_name ConvertStatEffect\nextends Effect\n"
              "export(float) var pct_converted = 1.0\n"
              'export(String) var to_stat = "stat_max_hp"\n'
              "export(int) var to_value = 1\n")
EXPLODING_GD = "class_name ItemExplodingEffect\nextends Effect\nexport (Resource) var stats\n"


def _write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def _effect(path: Path, script: str, fields: str) -> None:
    _write(path,
           '[gd_resource type="Resource" load_steps=2 format=2]\n\n'
           '[ext_resource path="%s" type="Script" id=1]\n\n[resource]\n'
           "script = ExtResource( 1 )\n%s" % (script, fields))


def _item(path: Path, my_id: str, tier: int, value: int, tags, effect_paths) -> None:
    ext = []
    refs = []
    for i, ref in enumerate(effect_paths, start=2):
        ext.append('[ext_resource path="%s" type="Resource" id=%d]' % (ref, i))
        refs.append("ExtResource( %d )" % i)
    _write(path,
           '[gd_resource type="Resource" load_steps=%d format=2]\n\n'
           '[ext_resource path="res://items/global/item_data.gd" type="Script" id=1]\n'
           % (2 + len(ext))
           + "\n".join(ext) + "\n\n[resource]\n"
           "script = ExtResource( 1 )\n"
           'my_id = "%s"\nname = "%s"\ntier = %d\nvalue = %d\n'
           "effects = [ %s ]\nmax_nb = -1\nunlocked_by_default = true\n"
           "can_be_looted = true\ntags = [ %s ]\n"
           % (my_id, my_id.upper(), tier, value, ", ".join(refs),
              ", ".join('"%s"' % t for t in tags)))


def _weapon(path: Path, my_id: str, wtype: int, tier: int) -> None:
    _write(path,
           '[gd_resource type="Resource" load_steps=2 format=2]\n\n'
           '[ext_resource path="res://items/global/weapon_data.gd" type="Script" id=1]\n\n'
           "[resource]\nscript = ExtResource( 1 )\n"
           'my_id = "%s"\ntype = %d\ntier = %d\n' % (my_id, wtype, tier))


def _character(path: Path, my_id: str, effects, extra_refs=(), extra="") -> None:
    """CharacterData resource.  `extra_refs` are non-effect resources (weapon
    data) referenced from `extra` through {r0}, {r1}, ... placeholders."""
    ids = {}
    ext = ['[ext_resource path="res://items/characters/character_data.gd" type="Script" id=1]']
    effect_ids = []
    for i, ref in enumerate(effects, start=2):
        ext.append('[ext_resource path="%s" type="Resource" id=%d]' % (ref, i))
        effect_ids.append(i)
    for j, ref in enumerate(extra_refs):
        eid = 2 + len(effects) + j
        ids["r%d" % j] = eid
        ext.append('[ext_resource path="%s" type="Resource" id=%d]' % (ref, eid))
    _write(path,
           '[gd_resource type="Resource" load_steps=%d format=2]\n\n' % (1 + len(ext))
           + "\n".join(ext) + "\n\n[resource]\n"
           "script = ExtResource( 1 )\n"
           'my_id = "%s"\nname = "%s"\ntier = 0\nvalue = 1\n' % (my_id, my_id.upper())
           + "effects = [ %s ]\n" % ", ".join("ExtResource( %d )" % i for i in effect_ids)
           + extra.format(**{k: "ExtResource( %d )" % v for k, v in ids.items()}))


def build_project(tmp_path: Path):
    campaign = tmp_path / "campaign"
    dlc = tmp_path / "dlc"

    _write(campaign / "singletons" / "item_service.gd", ITEM_SERVICE_GD)
    _write(campaign / "items" / "global" / "effect.gd", EFFECT_GD)
    _write(campaign / "items" / "global" / "item_data.gd", ITEM_DATA_GD)
    _write(campaign / "items" / "global" / "weapon_data.gd", WEAPON_DATA_GD)
    _write(campaign / "items" / "characters" / "character_data.gd", CHARACTER_DATA_GD)
    _write(campaign / "effects" / "items" / "stat_gains_modification_effect.gd", STAT_GAINS_GD)
    _write(campaign / "effects" / "items" / "gain_stat_for_every_stat_effect.gd", STAT_LINK_GD)
    _write(campaign / "effects" / "items" / "convert_stat_effect.gd", CONVERT_GD)
    _write(campaign / "effects" / "items" / "item_exploding_effect.gd", EXPLODING_GD)

    # -- item pool: alpha carries the stat_armor tag, beta is banned by Bull
    _effect(campaign / "items" / "all" / "alpha" / "alpha_effect_1.tres",
            "res://items/global/effect.gd",
            'key = "stat_max_hp"\nvalue = 5\ncustom_args = [  ]\n')
    _item(campaign / "items" / "all" / "alpha" / "alpha_data.tres",
          "item_alpha", 1, 10, ["stat_armor"],
          ["res://items/all/alpha/alpha_effect_1.tres"])
    _item(campaign / "items" / "all" / "beta" / "beta_data.tres",
          "item_beta", 1, 10, [], [])
    _write(campaign / "singletons" / "item_service.tscn",
           '[gd_scene load_steps=4 format=2]\n\n'
           '[ext_resource path="res://singletons/item_service.gd" type="Script" id=1]\n'
           '[ext_resource path="res://items/all/alpha/alpha_data.tres" type="Resource" id=2]\n'
           '[ext_resource path="res://items/all/beta/beta_data.tres" type="Resource" id=3]\n\n'
           '[node name="ItemService" type="Node"]\n'
           "script = ExtResource( 1 )\n"
           "items = [ ExtResource( 2 ), ExtResource( 3 ) ]\n")

    _write(dlc / "dlcs" / "dlc_1" / "dlc_1_data.gd", "extends DLCData\n")
    _write(dlc / "dlcs" / "dlc_1" / "dlc_data.tres",
           '[gd_resource type="Resource" format=2]\n\n[resource]\nitems = [  ]\n')
    (dlc / "dlcs" / "dlc_1" / "items").mkdir(parents=True, exist_ok=True)

    # -- starting weapons
    _weapon(campaign / "weapons" / "melee" / "sword" / "2" / "sword_2_data.tres",
            "weapon_sword_2", 0, 1)
    _weapon(campaign / "weapons" / "ranged" / "wand" / "1" / "wand_data.tres",
            "weapon_wand_1", 1, 0)

    # -- Bull
    bull = campaign / "items" / "characters" / "bull"
    _effect(bull / "bull_effect_1.tres", "res://items/global/effect.gd",
            'key = "stat_max_hp"\nvalue = 20\nstorage_method = 0\n')
    _effect(bull / "bull_effect_2.tres", "res://items/global/effect.gd",
            'key = "stat_hp_regeneration"\nvalue = 15\nstorage_method = 0\n')
    _effect(bull / "bull_effect_3.tres", "res://items/global/effect.gd",
            'key = "stat_armor"\nvalue = 10\nstorage_method = 0\n')
    _effect(bull / "bull_effect_3b.tres",
            "res://effects/items/stat_gains_modification_effect.gd",
            'key = "effect_increase_stat_gains"\nvalue = 50\n'
            'stats_modified = [ "stat_hp_regeneration" ]\n')
    _write(bull / "bull_explosion_stats.tres",
           '[gd_resource type="Resource" format=2]\n\n[resource]\n'
           "cooldown = 60\ndamage = 30\ncrit_chance = 0.03\ncrit_damage = 1.5\n"
           "max_range = 150\n"
           'scaling_stats = [ [ "stat_melee_damage", 3.0 ], '
           '[ "stat_ranged_damage", 3.0 ], [ "stat_elemental_damage", 3.0 ] ]\n')
    _write(bull / "bull_effect_4.tres",
           '[gd_resource type="Resource" load_steps=3 format=2]\n\n'
           '[ext_resource path="res://items/characters/bull/bull_explosion_stats.tres" '
           'type="Resource" id=1]\n'
           '[ext_resource path="res://effects/items/item_exploding_effect.gd" '
           'type="Script" id=2]\n\n[resource]\n'
           "script = ExtResource( 2 )\n"
           'key = "explode_on_hit"\nvalue = 1\nchance = 1.0\nscale = 1.0\n'
           "stats = ExtResource( 1 )\n")
    _effect(bull / "bull_effect_5.tres", "res://items/global/effect.gd",
            'key = "weapon_slot"\nvalue = 0\nstorage_method = 2\n')
    _character(bull / "bull_data.tres", "character_bull",
               ["res://items/characters/bull/bull_effect_%s.tres" % n
                for n in ("1", "2", "3", "3b", "4", "5")],
               extra='wanted_tags = [ "stat_armor" ]\n'
                     'banned_item_groups = [ "group_a" ]\n'
                     'banned_items = [ "item_beta" ]\n'
                     "starting_weapons = [  ]\n")

    # -- Knight
    knight = campaign / "items" / "characters" / "knight"
    _effect(knight / "knight_effect_1.tres",
            "res://effects/items/gain_stat_for_every_stat_effect.gd",
            'key = "stat_melee_damage"\nvalue = 2\nstat_scaled = "stat_armor"\n'
            "nb_stat_scaled = 1\nperm_stats_only = false\n")
    _effect(knight / "knight_effect_2.tres", "res://items/global/effect.gd",
            'key = "stat_armor"\nvalue = 3\nstorage_method = 0\n')
    _effect(knight / "knight_effect_3.tres", "res://items/global/effect.gd",
            'key = "no_ranged_weapons"\nvalue = 1\nstorage_method = 0\n')
    _effect(knight / "knight_effect_4.tres", "res://items/global/effect.gd",
            'key = "min_weapon_tier"\nvalue = 1\nstorage_method = 2\n')
    _effect(knight / "knight_effect_5.tres",
            "res://effects/items/stat_gains_modification_effect.gd",
            'key = "effect_reduce_stat_gains"\nvalue = -50\n'
            'stats_modified = [ "stat_attack_speed" ]\n')
    _character(knight / "knight_data.tres", "character_knight",
               ["res://items/characters/knight/knight_effect_%d.tres" % n for n in (1, 2, 3, 4, 5)],
               extra_refs=("res://weapons/melee/sword/2/sword_2_data.tres",
                           "res://weapons/ranged/wand/1/wand_data.tres"),
               extra='banned_item_groups = [ "group_a" ]\n'
                     'wanted_tags = [ "stat_armor" ]\n'
                     "starting_weapons = [ {r0}, {r1} ]\n")

    # -- Demon
    demon = campaign / "items" / "characters" / "demon"
    _effect(demon / "demon_effect_1.tres", "res://effects/items/convert_stat_effect.gd",
            'key = "materials"\ncustom_key = "convert_stats_end_of_wave"\nvalue = 13\n'
            'storage_method = 1\npct_converted = 50.0\nto_stat = "stat_max_hp"\nto_value = 1\n')
    _effect(demon / "demon_effect_2.tres", "res://items/global/effect.gd",
            'key = "hp_shop"\nvalue = 1\nstorage_method = 2\n')
    _character(demon / "demon_data.tres", "character_demon",
               ["res://items/characters/demon/demon_effect_%d.tres" % n for n in (1, 2)],
               extra_refs=("res://weapons/ranged/wand/1/wand_data.tres",),
               extra="starting_weapons = [ {r0} ]\n")
    return campaign, dlc


def run_extract(tmp_path: Path, campaign: Path, dlc: Path, tag: str = "a"):
    args = extract_characters.argparse.Namespace(
        campaign_root=str(campaign), dlc_root=str(dlc),
        characters_out=str(tmp_path / tag / "brotato_characters.h"))
    return extract_characters.emit_all(args)


# --------------------------------------------------------------------------
# hermetic tests
# --------------------------------------------------------------------------
def test_profiles_are_canonical_order(tmp_path):
    campaign, dlc = build_project(tmp_path)
    data = run_extract(tmp_path, campaign, dlc)
    assert [c["id"] for c in data["characters"]] == [
        "character_bull", "character_knight", "character_demon"]
    assert [c["token"] for c in data["characters"]] == ["bull", "knight", "demon"]


def test_bull_profile_fields(tmp_path):
    campaign, dlc = build_project(tmp_path)
    bull = run_extract(tmp_path, campaign, dlc)["characters"][0]
    assert bull["weapon_slots"] == 0
    assert bull["no_ranged_weapons"] == 0
    assert bull["hp_shop"] == 0
    assert bull["explode"]["damage"] == 30
    assert bull["explode"]["max_range"] == 150
    assert bull["explode"]["crit_chance"] == pytest.approx(0.03)
    assert bull["explode"]["crit_damage"] == pytest.approx(1.5)
    assert bull["explode"]["scaling"] == [
        ("stat_melee_damage", 3.0), ("stat_ranged_damage", 3.0),
        ("stat_elemental_damage", 3.0)]
    assert (0, 0, 20.0) in bull["stats"]
    assert (1, 0, 15.0) in bull["stats"]
    assert (2, 0, 10.0) in bull["stats"]
    assert (1, 1, 50.0) in bull["stats"]         # gain_stat_hp_regeneration +50
    assert bull["ban_group_mask"] == 1 << 0      # group_a is bit 0
    assert bull["ban_indices"] == [1]            # item_beta, pool position 1
    assert bull["want_ids"]                      # stat_armor interned
    assert bull["starting_weapons"] == []


def test_knight_profile_fields(tmp_path):
    campaign, dlc = build_project(tmp_path)
    knight = run_extract(tmp_path, campaign, dlc)["characters"][1]
    assert knight["min_weapon_tier"] == 1
    assert knight["no_ranged_weapons"] == 1
    assert knight["no_melee_weapons"] == 0
    assert knight["weapon_slots"] == 6
    assert knight["link"] == {"target": 6, "source": 2, "value": 2, "nb": 1,
                              "perm_only": 0}
    assert (2, 0, 3.0) in knight["stats"]
    assert (15, 1, -50.0) in knight["stats"]
    assert [w[0] for w in knight["starting_weapons"]] == [
        "weapon_sword_2", "weapon_wand_1"]
    assert [w[1] for w in knight["starting_weapons"]] == [0, 1]


def test_demon_profile_fields(tmp_path):
    campaign, dlc = build_project(tmp_path)
    demon = run_extract(tmp_path, campaign, dlc)["characters"][2]
    assert demon["hp_shop"] == 1
    assert demon["convert"] == {"from": 0xFF, "to": 0, "pct": 50, "value": 13,
                                "to_value": 1, "custom_key": "convert_stats_end_of_wave"}
    assert demon["stats"] == []
    assert demon["ban_group_mask"] == 0
    assert [w[0] for w in demon["starting_weapons"]] == ["weapon_wand_1"]


def test_starting_weapons_use_selection_ui_order(tmp_path):
    """The pool is emitted MELEE block first, RANGED block second, each in .tres
    order (weapon_selection.gd:92-117 -> item_service.gd:1013-1024), not the raw
    array order.  A reversed .tres array must still come out melee-first."""
    campaign, dlc = build_project(tmp_path)
    knight = campaign / "items" / "characters" / "knight" / "knight_data.tres"
    text = knight.read_text()
    match = re.search(r"starting_weapons = \[ (ExtResource\( \d+ \)), (ExtResource\( \d+ \)) \]", text)
    assert match, "the knight fixture must list two starting weapons"
    knight.write_text(text.replace(
        match.group(0),
        "starting_weapons = [ %s, %s ]" % (match.group(2), match.group(1))))
    knight_out = run_extract(tmp_path, campaign, dlc, tag="order")["characters"][1]
    assert [w[0] for w in knight_out["starting_weapons"]] == [
        "weapon_sword_2", "weapon_wand_1"]
    assert [w[1] for w in knight_out["starting_weapons"]] == [0, 1]


def test_next_level_xp_pct_is_extracted(tmp_path):
    """run_data.gd:767-768 next_level_xp_needed is a plain SUM character effect
    (baby_effect_4 +130, mutant_effect_1 -66, technomage_effect_2 +75); it rides
    a BtCharacterDef field, never an interned item-stat key."""
    campaign, dlc = build_project(tmp_path)
    _effect(campaign / "items" / "characters" / "knight" / "knight_xp.tres",
            "res://items/global/effect.gd",
            'key = "next_level_xp_needed"\nvalue = 75\nstorage_method = 0\n')
    _character(campaign / "items" / "characters" / "knight" / "knight_data.tres",
               "character_knight",
               ["res://items/characters/knight/knight_effect_%d.tres" % n
                for n in (1, 2, 3, 4, 5)]
               + ["res://items/characters/knight/knight_xp.tres"])
    data = run_extract(tmp_path, campaign, dlc, tag="xp")["characters"]
    assert data[1]["next_level_xp_pct"] == 75
    assert data[0]["next_level_xp_pct"] == 0      # Bull has no such effect
    assert data[2]["next_level_xp_pct"] == 0      # Demon has no such effect


def test_regeneration_is_byte_identical(tmp_path):
    campaign, dlc = build_project(tmp_path)
    out = tmp_path / "brotato_characters.h"
    args = extract_characters.argparse.Namespace(
        campaign_root=str(campaign), dlc_root=str(dlc), characters_out=str(out))
    extract_characters.emit_all(args)
    first = out.read_text()
    extract_characters.emit_all(args)
    assert first == out.read_text()
    assert "BT_NCHARACTERS 3" in first


def test_unsupported_effect_is_rejected(tmp_path):
    campaign, dlc = build_project(tmp_path)
    _write(campaign / "effects" / "items" / "weird_effect.gd",
           "class_name WeirdEffect\nextends Effect\n")
    _effect(campaign / "items" / "characters" / "bull" / "bull_effect_6.tres",
            "res://effects/items/weird_effect.gd", 'key = "mystery"\nvalue = 1\n')
    _character(campaign / "items" / "characters" / "bull" / "bull_data.tres",
               "character_bull",
               ["res://items/characters/bull/bull_effect_%s.tres" % n
                for n in ("1", "2", "3", "3b", "4", "5", "6")])
    with pytest.raises(extract_characters.ExtractError):
        run_extract(tmp_path, campaign, dlc)


def test_missing_canonical_character_is_rejected(tmp_path):
    campaign, dlc = build_project(tmp_path)
    os.remove(campaign / "items" / "characters" / "demon" / "demon_data.tres")
    with pytest.raises(extract_characters.ExtractError):
        run_extract(tmp_path, campaign, dlc)


def test_uninterned_wanted_tag_is_rejected(tmp_path):
    # extract_items() interns the Bull file's own wanted tags, so a tag the
    # item pool never carries must be rejected through a non-Bull profile.
    campaign, dlc = build_project(tmp_path)
    _character(campaign / "items" / "characters" / "knight" / "knight_data.tres",
               "character_knight",
               ["res://items/characters/knight/knight_effect_%d.tres" % n for n in (1, 2, 3, 4, 5)],
               extra='wanted_tags = [ "stat_armor", "not_a_real_tag" ]\n')
    with pytest.raises(extract_characters.ExtractError):
        run_extract(tmp_path, campaign, dlc)


# --------------------------------------------------------------------------
# real-source test (opt-in)
# --------------------------------------------------------------------------
CAMPAIGN = os.environ.get("BROTATO_CAMPAIGN_ROOT")
DLC = os.environ.get("BROTATO_DLC_ROOT")
needs_source = pytest.mark.skipif(
    not (CAMPAIGN and DLC and Path(CAMPAIGN).is_dir() and Path(DLC).is_dir()),
    reason="set BROTATO_CAMPAIGN_ROOT / BROTATO_DLC_ROOT to the recovered trees")


@needs_source
def test_real_profiles_match_source(tmp_path):
    args = extract_characters.argparse.Namespace(
        campaign_root=CAMPAIGN, dlc_root=DLC,
        characters_out=str(tmp_path / "brotato_characters.h"))
    data = extract_characters.emit_all(args)
    bull, knight, demon = data["characters"]
    assert (bull["weapon_slots"], bull["no_ranged_weapons"], bull["hp_shop"]) == (0, 0, 0)
    assert bull["explode"]["damage"] == 30
    assert len(bull["ban_indices"]) == 8
    assert (knight["weapon_slots"], knight["no_ranged_weapons"],
            knight["min_weapon_tier"], knight["link"]["value"]) == (6, 1, 1, 2)
    assert len(knight["starting_weapons"]) == 13
    assert all(w[1] == 0 for w in knight["starting_weapons"])
    assert len({w[0] for w in knight["starting_weapons"]}) == 13
    assert (demon["hp_shop"], demon["convert"]["pct"], demon["convert"]["value"]) == (1, 50, 13)
    assert len(demon["starting_weapons"]) == 12
