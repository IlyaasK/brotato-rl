"""Tests for ocean/brotato/extract_enemy_hit_effects.py.

The generator publishes the canonical source-identity inventory for the
``enemy_percent_damage_taken`` effect rows: the distinct ``BtItemEff.aux_key``
(interned ``PercentDamageEffect.source_id``) values, which the runtime uses as
the active-effect / proc identity.

Hermetic tests cover the inherited exported-default resolution and the row
collector against a tiny synthetic Godot project.  Real-source tests run only
when ``BROTATO_CAMPAIGN_ROOT`` / ``BROTATO_DLC_ROOT`` point at the recovered
campaign and Abyss DLC trees.
"""

from __future__ import annotations

import importlib.util
import os
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
_MODULE_PATH = ROOT / "ocean" / "brotato" / "extract_enemy_hit_effects.py"
_spec = importlib.util.spec_from_file_location("extract_ehe_under_test", _MODULE_PATH)
extract_ehe = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(extract_ehe)

extract_items = extract_ehe.items
physics = extract_ehe.physics
ExtractError = extract_ehe.ExtractError


# --------------------------------------------------------------------------
# synthetic project
# --------------------------------------------------------------------------
GD_PERCENT = """\
extends Effect
class_name PercentDamageEffect

export(String) var source_id = ""
export(int) var duration_secs = 3
export(int) var max_stacks = 1
export(int) var max_procs = -1
export(Color) var outline_color = Color("bfebff")
"""

GD_WEAPON = """\
extends PercentDamageEffect
class_name WeaponPercentDamageEffect

static func get_id() -> String:
\treturn "weapon_percent_damage_effect"
"""

TRES_WEAPON = """\
[gd_resource type="Resource" load_steps=2 format=2]

[ext_resource path="res://effects/weapons/weapon_percent_damage_effect.gd" type="Script" id=1]

[resource]
script = ExtResource( 1 )
key = "stat_all"
value = 10
custom_key = "enemy_percent_damage_taken"
source_id = "weapon_lute"
duration_secs = 3
max_stacks = 3
max_procs = -1
"""

# Omits every exported field except key/value: the inherited defaults
# (source_id "" -> rejected, duration 3, max_stacks 1, max_procs -1) apply.
TRES_BARE = """\
[gd_resource type="Resource" load_steps=2 format=2]

[ext_resource path="res://effects/weapons/weapon_percent_damage_effect.gd" type="Script" id=1]

[resource]
script = ExtResource( 1 )
key = "stat_all"
value = 7
custom_key = "enemy_percent_damage_taken"
"""


def _write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def _resolver(tmp_path: Path):
    campaign = tmp_path / "campaign"
    dlc = tmp_path / "dlc"
    dlc.mkdir(parents=True, exist_ok=True)
    _write(campaign / "effects/items/percent_damage_effect.gd", GD_PERCENT)
    _write(campaign / "effects/weapons/weapon_percent_damage_effect.gd", GD_WEAPON)
    return physics.Resolver([str(campaign), str(dlc)])


def _fake_builder():
    """A Builder-shaped object with just .strings and .rows."""
    builder = type("FakeBuilder", (), {})()
    builder.strings = extract_items.Strings()
    builder.rows = []
    return builder


def _row(builder, source, **over):
    row = {
        "opcode": "WeaponPercentDamageEffect",
        "source": source,
        "custom_key": builder.strings.intern("enemy_percent_damage_taken"),
        "key": builder.strings.intern("stat_all"),
        "aux_key": builder.strings.intern("weapon_lute"),
        "value": 10,
        "duration_secs": 3,
        "max_stacks": 3,
        "max_procs": -1,
        "item": extract_items.BT_ITEM_NONE,
    }
    row.update(over)
    row.setdefault("source_id", builder.strings.values[row["aux_key"]])
    row.setdefault("source_id_hash",
                   extract_ehe.godot_string_hash(builder.strings.values[row["aux_key"]]))
    return row


def _collect(resolver, builder):
    by_class, class_parent = extract_ehe._class_script_map(resolver)
    return extract_ehe.collect_effects(builder, resolver, by_class, class_parent)


# --------------------------------------------------------------------------
# exported default resolution
# --------------------------------------------------------------------------
def test_parse_export_defaults_keeps_only_exported_fields():
    defaults = extract_ehe.parse_export_defaults(GD_PERCENT)
    assert defaults == {
        "source_id": '""',
        "duration_secs": "3",
        "max_stacks": "1",
        "max_procs": "-1",
        "outline_color": 'Color("bfebff")',
    }


def test_resolve_class_defaults_walks_the_chain(tmp_path):
    resolver = _resolver(tmp_path)
    by_class, class_parent = extract_ehe._class_script_map(resolver)
    defaults = extract_ehe.resolve_class_defaults(
        resolver, by_class, class_parent, "WeaponPercentDamageEffect")
    assert extract_ehe._default_int(defaults, "duration_secs") == 3
    assert extract_ehe._default_int(defaults, "max_stacks") == 1
    assert extract_ehe._default_int(defaults, "max_procs") == -1
    assert extract_ehe._default_str(defaults, "source_id") == ""


def _defaults():
    return {"source_id": '""', "duration_secs": "3", "max_stacks": "1",
            "max_procs": "-1", "outline_color": 'Color("bfebff")'}


def test_resolve_effect_prefers_the_resource_value():
    props = {"source_id": '"weapon_lute"', "key": '"stat_all"', "value": "10",
             "duration_secs": "3", "max_stacks": "10", "max_procs": "-1"}
    effect = extract_ehe.resolve_effect("WeaponPercentDamageEffect", props, _defaults())
    assert effect["source_id"] == "weapon_lute"
    assert effect["max_stacks"] == 10


def test_resolve_effect_falls_back_to_class_defaults():
    props = {"source_id": '"x"', "key": '"stat_all"', "value": "7"}
    effect = extract_ehe.resolve_effect("PercentDamageEffect", props, _defaults())
    assert (effect["duration_secs"], effect["max_stacks"], effect["max_procs"]) == (3, 1, -1)


def test_resolve_effect_rejects_empty_source_id():
    with pytest.raises(ExtractError):
        extract_ehe.resolve_effect("PercentDamageEffect",
                                   {"key": '"stat_all"', "value": "1"}, _defaults())


def test_resolve_effect_rejects_non_ascii_source_id():
    with pytest.raises(ExtractError):
        extract_ehe.resolve_effect("PercentDamageEffect",
                                   {"source_id": '"\u00e9"', "key": '"stat_all"',
                                    "value": "1"}, _defaults())


# --------------------------------------------------------------------------
# row collector + source inventory
# --------------------------------------------------------------------------
def test_collect_effects_publishes_the_aux_key_identity(tmp_path):
    resolver = _resolver(tmp_path)
    _write(tmp_path / "campaign/weapons/lute_effect.tres", TRES_WEAPON)
    builder = _fake_builder()
    builder.rows = [_row(builder, "res://weapons/lute_effect.tres")]
    rows = _collect(resolver, builder)
    assert len(rows) == 1
    effect = rows[0]
    assert effect["row"] == 0
    assert effect["source_id"] == "weapon_lute"
    assert effect["interned_source_id"] == builder.strings.ids["weapon_lute"]
    assert effect["owned_item"] is False


def test_collect_effects_skips_other_custom_keys(tmp_path):
    resolver = _resolver(tmp_path)
    _write(tmp_path / "campaign/weapons/lute_effect.tres", TRES_WEAPON)
    builder = _fake_builder()
    other = _row(builder, "res://weapons/lute_effect.tres")
    other["custom_key"] = builder.strings.intern("burning")
    builder.rows = [other]
    with pytest.raises(ExtractError):
        _collect(resolver, builder)


def test_collect_effects_fails_on_canonical_divergence(tmp_path):
    resolver = _resolver(tmp_path)
    _write(tmp_path / "campaign/weapons/lute_effect.tres", TRES_WEAPON)
    builder = _fake_builder()
    builder.rows = [_row(builder, "res://weapons/lute_effect.tres", max_stacks=9)]
    with pytest.raises(ExtractError):
        _collect(resolver, builder)


def test_collect_effects_fails_on_missing_source_id(tmp_path):
    resolver = _resolver(tmp_path)
    _write(tmp_path / "campaign/weapons/bare.tres", TRES_BARE)
    builder = _fake_builder()
    bare = _row(builder, "res://weapons/bare.tres", value=7, duration_secs=3,
                max_stacks=1, max_procs=-1)
    bare["aux_key"] = builder.strings.intern("")
    builder.rows = [bare]
    with pytest.raises(ExtractError):
        _collect(resolver, builder)


def test_same_source_id_across_tiers_is_one_identity():
    """The four lute tiers are distinct rows that inherit the SAME source_id."""
    builder = _fake_builder()
    rows = [
        _row(builder, "res://lute_1.tres", max_stacks=3),
        _row(builder, "res://lute_2.tres", max_stacks=5),
        _row(builder, "res://lute_3.tres", max_stacks=7),
        _row(builder, "res://lute_4.tres", max_stacks=10),
    ]
    for index, row in enumerate(rows):
        row["row"] = index
        row["interned_source_id"] = row["aux_key"]
    sources = extract_ehe.build_sources(rows)
    assert len(sources) == 1
    assert sources[0]["interned"] == builder.strings.ids["weapon_lute"]
    assert sources[0]["rows"] == [0, 1, 2, 3]
    assert sources[0]["max_stacks"] == 10


def test_distinct_source_ids_are_distinct_identities():
    builder = _fake_builder()
    item = _row(builder, "res://ice_cube.tres", aux_key=builder.strings.intern("item_ice_cube"))
    weapon = _row(builder, "res://lute.tres")
    for index, row in enumerate((item, weapon)):
        row["row"] = index
        row["interned_source_id"] = row["aux_key"]
    sources = extract_ehe.build_sources([item, weapon])
    assert [s["interned"] for s in sources] == [
        builder.strings.ids["item_ice_cube"], builder.strings.ids["weapon_lute"]]


def test_build_sources_rejects_hash_collisions(monkeypatch):
    monkeypatch.setattr(extract_ehe, "godot_string_hash", lambda text: 7)
    builder = _fake_builder()
    first = _row(builder, "res://a.tres", aux_key=builder.strings.intern("item_ice_cube"))
    second = _row(builder, "res://b.tres")
    for index, row in enumerate((first, second)):
        row["row"] = index
        row["interned_source_id"] = row["aux_key"]
    with pytest.raises(ExtractError):
        extract_ehe.build_sources([first, second])


# --------------------------------------------------------------------------
# real-source test (opt-in)
# --------------------------------------------------------------------------
CAMPAIGN = os.environ.get("BROTATO_CAMPAIGN_ROOT")
DLC = os.environ.get("BROTATO_DLC_ROOT")
needs_source = pytest.mark.skipif(
    not (CAMPAIGN and DLC and Path(CAMPAIGN).is_dir() and Path(DLC).is_dir()),
    reason="set BROTATO_CAMPAIGN_ROOT / BROTATO_DLC_ROOT to the recovered trees")


class _Args(object):
    def __init__(self, campaign_root, dlc_root):
        self.campaign_root = campaign_root
        self.dlc_root = dlc_root


@needs_source
def test_real_enemy_percent_damage_source_inventory(tmp_path):
    data = extract_ehe.extract(_Args(CAMPAIGN, DLC))
    builder = data["builder"]

    rows = {effect["row"]: effect for effect in data["rows"]}
    # canonical item + weapon enemy_percent_damage_taken effect rows
    assert sorted(rows) == [461, 857, 863, 864, 865]

    lute = builder.strings.ids["weapon_lute"]
    ice = builder.strings.ids["item_ice_cube"]
    # the four lute tiers are distinct rows carrying one shared identity
    assert {rows[r]["interned_source_id"] for r in (857, 863, 864, 865)} == {lute}
    assert sorted(rows[r]["max_stacks"] for r in (857, 863, 864, 865)) == [3, 5, 7, 10]
    assert rows[461]["interned_source_id"] == ice
    assert rows[461]["owned_item"] is True
    assert all(rows[r]["owned_item"] is False for r in (857, 863, 864, 865))

    # two distinct identities, and each row's aux_key IS the resolved source_id
    assert [source["interned"] for source in data["sources"]] == [ice, lute]
    assert [source["source_id"] for source in data["sources"]] == [
        "item_ice_cube", "weapon_lute"]
    for effect in data["rows"]:
        assert effect["interned_source_id"] == builder.strings.ids[effect["source_id"]]
