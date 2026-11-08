"""Tests for ocean/brotato/extract_items.py.

Split in two halves:

* Hermetic tests over a tiny synthetic Godot project written into a tmp dir --
  they exercise discovery/validation, the effect graph, legacy-pool derivation,
  string interning, ban lists and byte-identical regeneration without needing
  the recovered game.
* Real-source tests that run only when ``BROTATO_CAMPAIGN_ROOT`` /
  ``BROTATO_DLC_ROOT`` point at the recovered campaign and Abyss DLC trees.
"""

from __future__ import annotations

import importlib.util
import os
import re
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
_MODULE_PATH = ROOT / "ocean" / "brotato" / "extract_items.py"
_spec = importlib.util.spec_from_file_location("extract_items_under_test", _MODULE_PATH)
extract_items = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(extract_items)


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
NULL_EFFECT_GD = "class_name NullEffect\nextends Effect\n"
ITEM_DATA_GD = "class_name ItemData\nextends Resource\n"


def _write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def _item_tres(name: str, my_id: str, tier: int, value: int, max_nb: int,
               tags, effect_paths) -> str:
    ext = ['[ext_resource path="res://items/global/item_data.gd" type="Script" id=1]']
    refs = []
    for i, path in enumerate(effect_paths, start=2):
        ext.append('[ext_resource path="%s" type="Resource" id=%d]' % (path, i))
        refs.append("ExtResource( %d )" % i)
    tag_text = ", ".join('"%s"' % t for t in tags)
    return (
        '[gd_resource type="Resource" format=2]\n\n'
        + "\n".join(ext) + "\n\n[resource]\n"
        "script = ExtResource( 1 )\n"
        'my_id = "%s"\n'
        "unlocked_by_default = true\n"
        "can_be_looted = true\n"
        'name = "%s"\n'
        "tier = %d\n"
        "value = %d\n"
        "effects = [ %s ]\n"
        "max_nb = %d\n"
        "tags = [ %s ]\n"
        "is_lockable = true\n"
        % (my_id, my_id.upper(), tier, value, ", ".join(refs), max_nb, tag_text))


def _effect_tres(script_path: str, fields: str) -> str:
    return (
        '[gd_resource type="Resource" load_steps=2 format=2]\n\n'
        '[ext_resource path="%s" type="Script" id=1]\n'
        "\n[resource]\n"
        "script = ExtResource( 1 )\n"
        "%s" % (script_path, fields)
    )


def build_project(tmp_path: Path) -> tuple[Path, Path]:
    campaign = tmp_path / "campaign"
    dlc = tmp_path / "dlc"

    _write(campaign / "singletons" / "item_service.gd", ITEM_SERVICE_GD)
    _write(campaign / "items" / "global" / "effect.gd", EFFECT_GD)
    _write(campaign / "items" / "global" / "item_data.gd", ITEM_DATA_GD)
    _write(campaign / "effects" / "items" / "dummy_effect.gd", NULL_EFFECT_GD)
    _write(campaign / "items" / "all" / "alpha" / "alpha_effect_1.tres",
           _effect_tres("res://items/global/effect.gd",
                        'key = "stat_max_hp"\nvalue = 5\ncustom_args = [  ]\n'))
    _write(campaign / "items" / "all" / "alpha" / "alpha_effect_2.tres",
           _effect_tres("res://items/global/effect.gd",
                        'key = "knockback"\nvalue = -2\ncustom_args = [  ]\n'))
    _write(campaign / "items" / "all" / "alpha" / "alpha_data.tres",
           _item_tres("alpha", "item_alpha", 1, 65, -1, ["stat_max_hp"],
                      ["res://items/all/alpha/alpha_effect_1.tres",
                       "res://items/all/alpha/alpha_effect_2.tres"]))
    _write(campaign / "items" / "all" / "beta" / "beta_effect_1.tres",
           _effect_tres("res://effects/items/dummy_effect.gd",
                        'key = "knockback"\nvalue = 3\ncustom_args = [  ]\n'))
    _write(campaign / "items" / "all" / "beta" / "beta_data.tres",
           _item_tres("beta", "item_beta", 2, 80, 0, ["knockback"],
                      ["res://items/all/beta/beta_effect_1.tres"]))
    _write(campaign / "items" / "characters" / "bull" / "bull_data.tres",
           '[gd_resource type="Resource" format=2]\n\n[resource]\n'
           'wanted_tags = [ "stat_armor", "explosive" ]\n'
           'banned_item_groups = [ "group_a" ]\n'
           'banned_items = [ "item_beta" ]\n')
    _write(campaign / "singletons" / "item_service.tscn",
           '[gd_scene load_steps=4 format=2]\n\n'
           '[ext_resource path="res://singletons/item_service.gd" type="Script" id=1]\n'
           '[ext_resource path="res://items/all/alpha/alpha_data.tres" type="Resource" id=2]\n'
           '[ext_resource path="res://items/all/beta/beta_data.tres" type="Resource" id=3]\n\n'
           '[node name="ItemService" type="Node"]\n'
           "script = ExtResource( 1 )\n"
           "items = [ ExtResource( 2 ), ExtResource( 3 ) ]\n")

    _write(dlc / "dlcs" / "dlc_1" / "dlc_1_data.gd",
           "extends DLCData\n\nexport (float) var base_item_curse_chance: = 0.03\n"
           "export var mirror_item_price: = - 10\n")
    _write(dlc / "dlcs" / "dlc_1" / "items" / "gamma" / "gamma_data.tres",
           _item_tres("gamma", "item_gamma", 3, 110, 1, [], []))
    _write(dlc / "dlcs" / "dlc_1" / "dlc_data.tres",
           '[gd_resource type="Resource" load_steps=2 format=2]\n\n'
           '[ext_resource path="res://dlcs/dlc_1/items/gamma/gamma_data.tres" '
           'type="Resource" id=1]\n\n[resource]\n'
           "items = [ ExtResource( 1 ) ]\n")
    return campaign, dlc


def run_extract(tmp_path: Path, campaign: Path, dlc: Path, tag: str = "a"):
    args = extract_items.argparse.Namespace(
        campaign_root=str(campaign), dlc_root=str(dlc),
        items_out=str(tmp_path / tag / "brotato_items.h"),
        effects_out=str(tmp_path / tag / "brotato_item_effects.h"),
        contract_out=str(tmp_path / tag / "contract.md"),
        provenance_out=str(tmp_path / tag / "prov.json"))
    return extract_items.emit_all(args)


# --------------------------------------------------------------------------
# hermetic tests
# --------------------------------------------------------------------------
def test_pool_counts_and_validation(tmp_path):
    campaign, dlc = build_project(tmp_path)
    data = run_extract(tmp_path, campaign, dlc)
    assert [row["id"] for row in data["items"]] == ["item_alpha", "item_beta", "item_gamma"]
    assert data["dir_stats"]["campaign"] == {"dirs": 2, "items": 2}
    assert data["dir_stats"]["dlc"] == {"dirs": 1, "items": 1}
    assert data["items"][0]["name"] == "alpha"
    assert data["items"][0]["price"] == 65
    assert data["items"][1]["max_nb"] == 0
    assert data["items"][1]["can_be_looted"] == 1


def test_legacy_pool_is_contiguous_and_derived(tmp_path):
    campaign, dlc = build_project(tmp_path)
    data = run_extract(tmp_path, campaign, dlc)
    alpha, beta, gamma = data["items"]
    assert (alpha["first"], alpha["count"]) == (0, 1)
    assert (beta["first"], beta["count"]) == (1, 0)
    assert data["legacy"] == [(0, 5.0)]


def test_effect_graph_carries_class_and_keys(tmp_path):
    campaign, dlc = build_project(tmp_path)
    data = run_extract(tmp_path, campaign, dlc)
    strings = data["builder"].strings
    beta = data["items"][1]
    slot = data["builder"].eff_index[beta["eff_first"]]
    row = data["builder"].rows[slot]
    assert row["opcode"] == "NullEffect"
    assert strings.values[row["key"]] == "knockback"
    assert beta["count"] == 0  # knockback is not a core stat id


def test_effect_index_pool_is_consistent(tmp_path):
    campaign, dlc = build_project(tmp_path)
    data = run_extract(tmp_path, campaign, dlc)
    builder = data["builder"]
    for row in builder.rows:
        if row["sub_count"]:
            assert row["sub_first"] + row["sub_count"] <= len(builder.eff_index)
    for item in data["items"]:
        assert item["eff_first"] + item["eff_count"] <= len(builder.eff_index)
        assert item["first"] + item["count"] <= len(data["legacy"])


def test_ban_lists(tmp_path):
    campaign, dlc = build_project(tmp_path)
    data = run_extract(tmp_path, campaign, dlc)
    index = data["id_to_index"]
    assert index["item_beta"] in [index[i] for i in data["endless"]]
    assert data["bull"]["banned_items"] == ["item_beta"]
    assert data["bull"]["banned_groups"] == ["group_a"]
    assert data["bull"]["wanted"] == ["stat_armor", "explosive"]


def test_curse_constants(tmp_path):
    campaign, dlc = build_project(tmp_path)
    data = run_extract(tmp_path, campaign, dlc)
    assert data["curse"]["base_item_curse_chance"] == pytest.approx(0.03)
    assert data["curse"]["mirror_item_price"] == pytest.approx(-10.0)


def test_regeneration_is_byte_identical(tmp_path):
    campaign, dlc = build_project(tmp_path)
    run_extract(tmp_path, campaign, dlc, "one")
    names = ("brotato_items.h", "brotato_item_effects.h", "contract.md", "prov.json")
    first = {name: (tmp_path / "one" / name).read_text() for name in names}
    run_extract(tmp_path, campaign, dlc, "one")
    for name in names:
        assert first[name] == (tmp_path / "one" / name).read_text(), name


def test_string_interning_is_stable_and_collision_free(tmp_path):
    campaign, dlc = build_project(tmp_path)
    data = run_extract(tmp_path, campaign, dlc)
    strings = data["builder"].strings
    assert strings.values[0] == ""
    assert strings.macros[""] == "BT_ISTR_NONE"
    assert len(set(strings.macros.values())) == len(strings.macros)
    for text, sid in strings.ids.items():
        assert strings.values[sid] == text


def test_unregistered_directory_item_is_rejected(tmp_path):
    campaign, dlc = build_project(tmp_path)
    stray = campaign / "items" / "all" / "delta" / "delta_data.tres"
    _write(stray, _item_tres("delta", "item_delta", 0, 1, -1, [], []))
    with pytest.raises(extract_items.ExtractError):
        run_extract(tmp_path, campaign, dlc)


def test_missing_directory_item_is_rejected(tmp_path):
    campaign, dlc = build_project(tmp_path)
    (campaign / "items" / "all" / "empty").mkdir(parents=True)
    with pytest.raises(extract_items.ExtractError):
        run_extract(tmp_path, campaign, dlc)


def test_headers_declare_both_pools(tmp_path):
    campaign, dlc = build_project(tmp_path)
    run_extract(tmp_path, campaign, dlc)
    items = (tmp_path / "a" / "brotato_items.h").read_text()
    effects = (tmp_path / "a" / "brotato_item_effects.h").read_text()
    assert "#define BT_NITEMS 3" in items
    assert "#define BT_NITEM_EFFECTS 1" in items
    assert "BT_TABLE BtItem BT_ITEMS[BT_NITEMS]" in items
    assert "BT_TABLE BtItemEffect BT_ITEM_EFFECTS[1]" in items
    assert "#define BT_NITEM_EFF_ROWS" in effects
    assert "BT_HOST_ONLY(static const char* const BT_ITEM_STR_NAMES" in effects
    # no bare integer literal is used for a float field (`.0f` discipline)
    assert " 5, }" not in items


# --------------------------------------------------------------------------
# real-source tests (opt-in)
# --------------------------------------------------------------------------
CAMPAIGN = os.environ.get("BROTATO_CAMPAIGN_ROOT")
DLC = os.environ.get("BROTATO_DLC_ROOT")
needs_source = pytest.mark.skipif(
    not (CAMPAIGN and DLC and Path(CAMPAIGN).is_dir() and Path(DLC).is_dir()),
    reason="set BROTATO_CAMPAIGN_ROOT / BROTATO_DLC_ROOT to the recovered trees")


@needs_source
def test_real_pool_is_complete(tmp_path):
    args = extract_items.argparse.Namespace(
        campaign_root=CAMPAIGN, dlc_root=DLC,
        items_out=str(tmp_path / "brotato_items.h"),
        effects_out=str(tmp_path / "brotato_item_effects.h"),
        contract_out=str(tmp_path / "contract.md"),
        provenance_out=str(tmp_path / "prov.json"))
    data = extract_items.emit_all(args)
    assert len(data["items"]) == 247
    assert data["dir_stats"]["campaign"]["dirs"] == 208
    assert data["dir_stats"]["campaign"]["items"] == 209
    assert data["dir_stats"]["dlc"]["dirs"] == 35
    assert data["dir_stats"]["dlc"]["items"] == 38
    assert len(data["legacy"]) > 255          # the old 8-bit pool overflowed
    assert data["builder"].unsupported == []
    for row in data["items"]:
        assert row["source"].startswith("res://")
        assert Path(extract_items.physics.Resolver([CAMPAIGN, DLC]).find(row["source"])).is_file()
