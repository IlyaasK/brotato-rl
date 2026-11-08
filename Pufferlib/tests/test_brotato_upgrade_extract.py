"""Tests for ocean/brotato/extract_upgrades.py and the upgrade half of
ocean/brotato/bridge/gen_obs_tables.py.

Split in two halves:

* Hermetic tests over a tiny synthetic Godot project written into a tmp dir --
  they exercise discovery, eligibility (pool vs unpooled), tier/value mapping,
  validation failures and byte-identical regeneration without needing the
  recovered game.
* Real-source tests that run only when ``BROTATO_CAMPAIGN_ROOT`` /
  ``BROTATO_DLC_ROOT`` point at the recovered campaign and Abyss DLC trees;
  they pin the pool boundaries (64 rows / 16 stats / 13 unpooled resources) and
  the exact authored values, and check the checked-in header is not stale.
"""

from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
_MODULE_PATH = ROOT / "ocean" / "brotato" / "extract_upgrades.py"
_spec = importlib.util.spec_from_file_location("extract_upgrades_under_test", _MODULE_PATH)
extract_upgrades = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(extract_upgrades)

_OBS_PATH = ROOT / "ocean" / "brotato" / "bridge" / "gen_obs_tables.py"
_obs_spec = importlib.util.spec_from_file_location("gen_obs_tables_under_test", _OBS_PATH)
gen_obs_tables = importlib.util.module_from_spec(_obs_spec)
_obs_spec.loader.exec_module(gen_obs_tables)

UPGRADES_HEADER = ROOT / "ocean" / "brotato" / "brotato_upgrades.h"
ITEMS_HEADER = ROOT / "ocean" / "brotato" / "brotato_items.h"
BROTATO_H = ROOT / "ocean" / "brotato" / "brotato.h"
STATE_H = ROOT / "ocean" / "brotato" / "brotato_state.h"


# --------------------------------------------------------------------------
# synthetic project
# --------------------------------------------------------------------------
UPGRADE_DATA_GD = "class_name UpgradeData\nextends ItemData\n"
ITEM_DATA_GD = "class_name ItemData\nextends Resource\n"
EFFECT_GD = "class_name Effect\nextends Resource\n"
STAT_DATA_GD = "class_name StatData\nextends Resource\n"


def _write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def _effect_tres(key: str, value: float) -> str:
    return ('[gd_resource type="Resource" load_steps=2 format=2]\n\n'
            '[ext_resource path="res://items/global/effect.gd" type="Script" id=1]\n\n'
            '[resource]\nscript = ExtResource( 1 )\n'
            'key = "%s"\nvalue = %s\n' % (key, value))


def _upgrade_tres(my_id, tier: int, effect_path, unlocked: bool = True,
                  template: bool = False) -> str:
    my = '"%s"' % my_id if isinstance(my_id, str) else my_id
    effects = "[  ]" if template else "[ ExtResource( 3 ) ]"
    extra = ("tracking_text = \"\"\nis_lockable = true\nis_cursed = false\n"
             "curse_factor = 0.0\nmax_nb = -1\n") if isinstance(my_id, str) else ""
    lines = ['[gd_resource type="Resource" load_steps=4 format=2]\n',
             '[ext_resource path="res://items/upgrades/upgrade_data.gd" type="Script" id=1]\n',
             '[ext_resource path="res://items/upgrades/misc/icon.png" type="Texture" id=2]\n',
             '[ext_resource path="%s" type="Resource" id=3]\n' % effect_path,
             '\n[resource]\nscript = ExtResource( 1 )\n',
             'my_id = %s\n' % my,
             'unlocked_by_default = %s\n' % ("true" if unlocked else "false"),
             'icon = ExtResource( 2 )\n',
             'name = "UPGRADE_TEST"\n',
             'tier = %d\n' % tier,
             'value = 1\n',
             'effects = %s\n' % effects,
             extra,
             'item_appearances = [  ]\n']
    return "".join(lines)


def _write_index_files(campaign: Path, dlc: Path,
                       campaign_pool, campaign_unpooled=(), dlc_pool=()):
    """Writes the two service arrays; the resources themselves must already exist."""
    def ext_list(paths, start):
        lines = []
        refs = {}
        n = start
        for path in paths:
            lines.append('[ext_resource path="%s" type="Resource" id=%d]\n' % (path, n))
            refs[path] = n
            n += 1
        return lines, refs

    lines, refs = ext_list(list(campaign_pool) + list(campaign_unpooled), 2)
    _write(campaign / "singletons/item_service.tscn",
           '[gd_scene load_steps=%d format=2]\n\n' % (len(lines) + 2)
           + '[ext_resource path="res://singletons/item_service.gd" type="Script" id=1]\n'
           + "".join(lines)
           + '\n[node name="ItemService" type="Node"]\n'
           + 'script = ExtResource( 1 )\n'
           + 'items = [  ]\n'
           + 'upgrades = [ %s ]\n'
           % ", ".join("ExtResource( %d )" % refs[p] for p in campaign_pool))

    dlc_dir = dlc / "dlcs/dlc_1"
    dlc_dir.mkdir(parents=True, exist_ok=True)
    dlc_lines, dlc_refs = ext_list(list(dlc_pool), 1)
    body = ('upgrades = [ %s ]\n' % ", ".join("ExtResource( %d )" % dlc_refs[p] for p in dlc_pool)
            if dlc_pool else 'my_id = "dlc_1"\n')
    _write(dlc_dir / "dlc_data.tres",
           '[gd_resource type="Resource" load_steps=%d format=2]\n\n' % (len(dlc_lines) + 1)
           + "".join(dlc_lines) + '\n[resource]\n' + body)


def _add_upgrade(root: Path, directory: str, name: str, my_id, tier: int,
                 key: str, value: float, unlocked: bool = True,
                 template: bool = False) -> str:
    """Writes one upgrade resource under ``root/items/upgrades/<dir>/``."""
    base = root / ("items/upgrades/%s" % directory)
    if not template:
        _write(base / ("%s_effect.tres" % name), _effect_tres(key, value))
    _write(base / ("%s_data.tres" % name),
           _upgrade_tres(my_id, tier, "res://items/upgrades/%s/%s_effect.tres" % (directory, name),
                         unlocked, template))
    return "res://items/upgrades/%s/%s_data.tres" % (directory, name)


def _add_dlc_upgrade(root: Path, directory: str, name: str, my_id, tier: int,
                     key: str, value: float) -> str:
    """Writes one upgrade resource under ``root/dlcs/dlc_1/upgrades/<dir>/``
    (the DLC tree the generator scans, mirroring the real pack layout)."""
    base = root / ("dlcs/dlc_1/upgrades/%s" % directory)
    _write(base / ("%s_effect.tres" % name), _effect_tres(key, value))
    _write(base / ("%s_data.tres" % name),
           _upgrade_tres(my_id, tier,
                         "res://dlcs/dlc_1/upgrades/%s/%s_effect.tres" % (directory, name)))
    return "res://dlcs/dlc_1/upgrades/%s/%s_data.tres" % (directory, name)


def _skeleton(campaign: Path) -> None:
    _write(campaign / "items/global/effect.gd", EFFECT_GD)
    _write(campaign / "items/upgrades/upgrade_data.gd", UPGRADE_DATA_GD)
    _write(campaign / "items/all/item_data.gd", ITEM_DATA_GD)
    _write(campaign / "items/upgrades/stat_data.gd", STAT_DATA_GD)
    # a StatData resource in the same tree must not be mistaken for an upgrade
    _write(campaign / "items/upgrades/misc/stat_speed.tres",
           '[gd_resource type="Resource" format=2]\n\n'
           '[ext_resource path="res://items/upgrades/stat_data.gd" type="Script" id=1]\n\n'
           '[resource]\nscript = ExtResource( 1 )\nstat_name = "stat_speed"\n')


def run_extract(tmp_path: Path, campaign: Path, dlc: Path, tag: str = "a"):
    args = extract_upgrades.argparse.Namespace(
        campaign_root=str(campaign), dlc_root=str(dlc),
        header_out=str(tmp_path / tag / "brotato_upgrades.h"),
        provenance_out=str(tmp_path / tag / "brotato_upgrades.json"))
    return extract_upgrades.emit_all(args)


def simple_project(tmp_path: Path):
    """One pooled stat (4 tiers), one pooled DLC stat, one unpooled stat and a
    UI template."""
    campaign = tmp_path / "campaign"
    dlc = tmp_path / "dlc"
    (campaign / "items/upgrades").mkdir(parents=True, exist_ok=True)
    _skeleton(campaign)
    pool = []
    for tier, value in enumerate((3.0, 6.0, 9.0, 12.0)):
        pool.append(_add_upgrade(campaign, "health",
                                 "health" if tier == 0 else "health_%d" % (tier + 1),
                                 "upgrade_max_hp_%d" % (tier + 1), tier, "stat_max_hp", value))
    dlc_pool = [_add_dlc_upgrade(dlc, "range", "range", "upgrade_range_1", 0, "stat_range", 15.0)]
    _add_dlc_upgrade(dlc, "range", "range_2", "upgrade_range_2", 1, "stat_range", 30.0)
    unpooled = []
    for tier, value in enumerate((4.0, 8.0, 12.0, 16.0)):
        unpooled.append(_add_upgrade(campaign, "accuracy",
                                     "accuracy" if tier == 0 else "accuracy_%d" % (tier + 1),
                                     "upgrade_accuracy_%d" % (tier + 1), tier,
                                     "stat_accuracy", value))
    unpooled.append(_add_upgrade(campaign, "damage", "damage", 16, 0, "stat_damage", 1.0,
                                 unlocked=False))
    template = _add_upgrade(campaign, "template", "upgrade_to_process", None, 0, "", 0.0,
                            template=True)
    _write_index_files(campaign, dlc, pool, unpooled + [template], dlc_pool)
    return campaign, dlc


# --------------------------------------------------------------------------
# hermetic tests
# --------------------------------------------------------------------------
def test_pool_rows_use_core_ids_and_one_based_tiers(tmp_path):
    campaign, dlc = simple_project(tmp_path)
    data = run_extract(tmp_path, campaign, dlc)
    assert [(r["stat"], r["core_tier"], r["stat_value"]) for r in data["rows"]] == [
        (0, 1, 3.0), (0, 2, 6.0), (0, 3, 9.0), (0, 4, 12.0), (18, 1, 15.0)]
    assert data["rows"][0]["source"].startswith("res://items/upgrades/health/")
    assert data["dir_stats"]["campaign"]["pool"] == 4
    assert data["dir_stats"]["dlc"]["pool"] == 1


def test_unpooled_and_template_are_reported_not_dropped(tmp_path):
    campaign, dlc = simple_project(tmp_path)
    data = run_extract(tmp_path, campaign, dlc)
    unpooled = {r["label"]: r for r in data["unpooled"]}
    assert len(unpooled) == 6
    assert unpooled["upgrade_accuracy_4"]["stat"] == 14
    assert unpooled["upgrade_accuracy_4"]["core_tier"] == 4
    assert unpooled["upgrade_accuracy_4"]["stat_value"] == 16.0
    # flat damage has no core stat id at all
    assert unpooled["damage_data"]["stat"] is None
    assert unpooled["damage_data"]["stat_key"] == "stat_damage"
    assert unpooled["damage_data"]["unlocked_by_default"] is False
    assert unpooled["upgrade_range_2"]["stat"] == 18 and unpooled["upgrade_range_2"]["core_tier"] == 2
    assert unpooled["upgrade_range_2"]["source"].startswith("res://dlcs/dlc_1/")
    assert [(r["stat"], r["core_tier"]) for r in data["rows"]] == [
        (0, 1), (0, 2), (0, 3), (0, 4), (18, 1)]
    assert len(data["templates"]) == 1
    assert data["templates"][0]["source"].endswith("upgrade_to_process_data.tres")
    # the StatData resource in the same tree is neither pooled nor unpooled
    assert not any("stat_speed" in r["source"] for r in data["unpooled"])


def test_pool_membership_is_the_eligibility_rule(tmp_path):
    """Moving a resource into the service array makes it pooled (and vice versa)."""
    campaign, dlc = simple_project(tmp_path)
    data = run_extract(tmp_path, campaign, dlc)
    assert 14 not in {r["stat"] for r in data["rows"]}

    # list accuracy tier 1 in the service array
    svc = campaign / "singletons/item_service.tscn"
    text = svc.read_text()
    extra = '[ext_resource path="res://items/upgrades/accuracy/accuracy_data.tres" ' \
            'type="Resource" id=99]\n'
    text = text.replace('\n[node name="ItemService"', extra + '\n[node name="ItemService"')
    text = text.replace("upgrades = [ ", "upgrades = [ ExtResource( 99 ), ")
    _write(svc, text)

    data = run_extract(tmp_path, campaign, dlc, "b")
    rows = {(r["stat"], r["core_tier"]): r for r in data["rows"]}
    assert (14, 1) in rows
    assert rows[(14, 1)]["stat_value"] == 4.0
    assert (14, 1) not in {(r["stat"], r["core_tier"]) for r in data["unpooled"]}


def test_pooled_row_without_unlocked_by_default_is_rejected(tmp_path):
    campaign, dlc = simple_project(tmp_path)
    # drop the "unlocked_by_default = true" line from a pooled resource
    path = campaign / "items/upgrades/health/health_data.tres"
    _write(path, path.read_text().replace("unlocked_by_default = true\n", ""))
    with pytest.raises(extract_upgrades.ExtractError):
        run_extract(tmp_path, campaign, dlc)


def test_pooled_row_with_unmappable_key_is_rejected(tmp_path):
    campaign, dlc = simple_project(tmp_path)
    _write(campaign / "items/upgrades/health/health_effect.tres",
           _effect_tres("stat_not_a_core_stat", 3.0))
    with pytest.raises(extract_upgrades.ExtractError):
        run_extract(tmp_path, campaign, dlc)


def test_pooled_row_with_out_of_range_tier_is_rejected(tmp_path):
    campaign, dlc = simple_project(tmp_path)
    path = campaign / "items/upgrades/health/health_data.tres"
    _write(path, path.read_text().replace("tier = 0\n", "tier = 7\n"))
    with pytest.raises(extract_upgrades.ExtractError):
        run_extract(tmp_path, campaign, dlc)


def test_duplicate_pooled_stat_and_tier_is_rejected(tmp_path):
    campaign, dlc = simple_project(tmp_path)
    dup = _add_upgrade(campaign, "health", "health_extra", "upgrade_max_hp_extra", 0,
                       "stat_max_hp", 99.0)
    svc = campaign / "singletons/item_service.tscn"
    text = svc.read_text()
    text = text.replace('\n[node name="ItemService"',
                        '\n[ext_resource path="%s" type="Resource" id=98]\n\n[node name="ItemService"'
                        % dup)
    text = text.replace("upgrades = [ ", "upgrades = [ ExtResource( 98 ), ")
    _write(svc, text)
    with pytest.raises(extract_upgrades.ExtractError):
        run_extract(tmp_path, campaign, dlc)


def test_dropping_a_pool_row_moves_it_to_unpooled(tmp_path):
    """Pool membership follows the service array, so removing an entry demotes
    the (still present) resource to the unpooled report."""
    campaign, dlc = simple_project(tmp_path)
    svc = campaign / "singletons/item_service.tscn"
    text = svc.read_text()
    list_start = text.index("upgrades = [ ")
    list_end = text.index(" ]", list_start)
    entries = [e.strip() for e in text[list_start + len("upgrades = [ "):list_end].split(",")]
    _write(svc, text[:list_start] + "upgrades = [ " + ", ".join(entries[:3]) + text[list_end:])
    data = run_extract(tmp_path, campaign, dlc, "b")
    assert sorted(r["core_tier"] for r in data["rows"] if r["stat"] == 0) == [1, 2, 3]
    assert (0, 4) in {(r["stat"], r["core_tier"]) for r in data["unpooled"]}


def test_regeneration_is_byte_identical(tmp_path):
    campaign, dlc = simple_project(tmp_path)
    run_extract(tmp_path, campaign, dlc, "one")
    names = ("brotato_upgrades.h", "brotato_upgrades.json")
    first = {name: (tmp_path / "one" / name).read_text() for name in names}
    run_extract(tmp_path, campaign, dlc, "one")
    for name in names:
        assert first[name] == (tmp_path / "one" / name).read_text(), name


def test_header_shape(tmp_path):
    campaign, dlc = simple_project(tmp_path)
    run_extract(tmp_path, campaign, dlc)
    header = (tmp_path / "a" / "brotato_upgrades.h").read_text()
    assert "#define BT_NUPGRADES 5" in header
    assert "#define BT_NUPGRADES_UNPOOLED 6" in header
    assert "BT_TABLE BtUpgrade BT_UPGRADES[BT_NUPGRADES]" in header
    assert "BT_HOST_ONLY(static const BtUpgradeUnpooled" in header
    assert "BT_HOST_ONLY(static const char* const BT_STAT_KEYS[BT_NUPGRADE_STAT_IDS]" in header
    assert "BT_HOST_ONLY(static const char* const BT_STAT_NAMES[BT_NUPGRADE_STAT_IDS]" in header
    assert "#define BT_NUPGRADE_STAT_IDS 19" in header
    assert "BT_STAT_NONE" in header
    # floats always carry a decimal point
    assert " 3, }" not in header
    provenance = json.loads((tmp_path / "a" / "brotato_upgrades.json").read_text())
    assert provenance["counts"]["pool_rows"] == 5
    assert provenance["counts"]["pooled_stats"] == 2
    assert provenance["stat_contract"][11]["key"] == "stat_crit_damage"
    assert provenance["stat_contract"][11]["pooled_tiers"] == []
    assert provenance["stat_contract"][14]["unpooled_resources"] == [
        "upgrade_accuracy_1", "upgrade_accuracy_2", "upgrade_accuracy_3", "upgrade_accuracy_4"]


# --------------------------------------------------------------------------
# emitted-contract / bridge tests (repo files only)
# --------------------------------------------------------------------------
def test_checked_in_header_contract_and_pool_boundaries():
    keys, names = gen_obs_tables.parse_stat_contract(UPGRADES_HEADER)
    assert len(keys) == len(names) == 19
    assert keys[0] == "stat_max_hp" and keys[18] == "stat_range"
    assert names[13] == "curse" and names[14] == "accuracy"
    rows = gen_obs_tables.parse_upgrades(UPGRADES_HEADER)
    assert len(rows) == 64
    assert {r["stat"] for r in rows} == set(range(11)) | {12, 15, 16, 17, 18}
    assert rows[0] == {"stat": 0, "tier": 1, "value": 3.0}
    tiers = {}
    for row in rows:
        tiers.setdefault(row["stat"], []).append(row["tier"])
    for stat, seen in tiers.items():
        assert sorted(seen) == [1, 2, 3, 4], stat
    unpooled = gen_obs_tables.parse_unpooled(UPGRADES_HEADER)
    assert len(unpooled) == 13
    assert {r["id"] for r in unpooled if r["stat"] == 14} == {
        "upgrade_accuracy_1", "upgrade_accuracy_2", "upgrade_accuracy_3", "51"}
    assert all(r["stat"] in (11, 14) or r["stat"] is None for r in unpooled)


def test_checked_in_values_are_the_authored_effects():
    rows = {(r["stat"], r["tier"]): r["value"] for r in gen_obs_tables.parse_upgrades(UPGRADES_HEADER)}
    assert [rows[(15, t)] for t in (1, 2, 3, 4)] == [5.0, 10.0, 15.0, 20.0]   # attack speed
    assert [rows[(16, t)] for t in (1, 2, 3, 4)] == [2.0, 3.0, 4.0, 5.0]      # engineering
    assert [rows[(17, t)] for t in (1, 2, 3, 4)] == [1.0, 2.0, 3.0, 4.0]      # lifesteal
    assert [rows[(18, t)] for t in (1, 2, 3, 4)] == [15.0, 30.0, 45.0, 60.0]  # range
    assert [rows[(9, t)] for t in (1, 2, 3, 4)] == [3.0, 5.0, 7.0, 9.0]       # crit chance
    assert [rows[(12, t)] for t in (1, 2, 3, 4)] == [5.0, 8.0, 10.0, 12.0]    # harvesting
    assert (11, 1) not in rows and (14, 1) not in rows


def test_owner_stat_count_matches_the_core():
    keys, names = gen_obs_tables.parse_stat_contract(UPGRADES_HEADER)
    match = __import__("re").search(r"#define BT_NSTATS (\d+)", STATE_H.read_text())
    assert match is not None
    assert int(match.group(1)) == len(names)
    renderer = BROTATO_H.read_text()
    assert "BT_STAT_NAMES" in renderer          # renderer reads the emitted contract
    assert "BT_NUPGRADE_STAT_IDS != BT_NSTATS" in renderer   # compile-time drift guard
    assert "stat_names[" not in renderer        # the hand-copied lists are gone


def test_gen_obs_tables_payload_round_trip(tmp_path):
    payload = gen_obs_tables.build(ITEMS_HEADER, UPGRADES_HEADER,
                                   BROTATO_H, ROOT / "ocean/brotato/brotato_core.h", STATE_H)
    assert payload["counts"]["upgrades"] == 64
    assert payload["counts"]["unpooled_upgrades"] == 13
    assert payload["counts"]["stats"] == 19
    assert payload["stat_ids"]["stat_attack_speed"] == 15
    assert payload["stat_ids"]["stat_range"] == 18
    assert payload["stat_names"][16] == "engineering"
    # every pool row can be addressed by (stat, tier) - what rl_obs indexes
    keyed = {(r["stat"], r["tier"]): r["value"] for r in payload["upgrades"]}
    assert keyed[(18, 4)] == 60.0
    blob = json.dumps(payload, indent="\t") + "\n"
    assert json.loads(blob)["counts"] == payload["counts"]


# --------------------------------------------------------------------------
# real-source tests (opt-in)
# --------------------------------------------------------------------------
CAMPAIGN = os.environ.get("BROTATO_CAMPAIGN_ROOT")
DLC = os.environ.get("BROTATO_DLC_ROOT")
needs_source = pytest.mark.skipif(
    not (CAMPAIGN and DLC and Path(CAMPAIGN).is_dir() and Path(DLC).is_dir()),
    reason="set BROTATO_CAMPAIGN_ROOT / BROTATO_DLC_ROOT to the recovered trees")


@needs_source
def test_real_pool_matches_the_service_array(tmp_path):
    args = extract_upgrades.argparse.Namespace(
        campaign_root=CAMPAIGN, dlc_root=DLC,
        header_out=str(tmp_path / "brotato_upgrades.h"),
        provenance_out=str(tmp_path / "prov.json"))
    data = extract_upgrades.emit_all(args)
    assert len(data["campaign_pool"]) == 64
    assert data["dlc_pool"] == []
    assert len(data["rows"]) == 64
    assert len({r["stat"] for r in data["rows"]}) == 16
    assert len(data["unpooled"]) == 13
    assert len(data["templates"]) == 1
    assert len(data["scan"]) == 78
    # every pooled stat carries all four tiers in this source revision
    tiers = {}
    for row in data["rows"]:
        tiers.setdefault(row["stat"], []).append(row["core_tier"])
    assert all(sorted(v) == [1, 2, 3, 4] for v in tiers.values())
    # both DLC and campaign trees were searched
    assert data["dir_stats"]["dlc"]["resources"] == 0


@needs_source
def test_real_header_matches_checked_in(tmp_path):
    args = extract_upgrades.argparse.Namespace(
        campaign_root=CAMPAIGN, dlc_root=DLC,
        header_out=str(tmp_path / "brotato_upgrades.h"), provenance_out=None)
    extract_upgrades.emit_all(args)
    fresh = (tmp_path / "brotato_upgrades.h").read_text()
    checked_in = UPGRADES_HEADER.read_text()
    # The leading comment records the source roots it was generated from; the
    # generated code/table body must be byte-identical.
    marker = "#pragma once"
    assert fresh[fresh.index(marker):] == checked_in[checked_in.index(marker):], \
        "ocean/brotato/brotato_upgrades.h is stale: regenerate with extract_upgrades.py"
