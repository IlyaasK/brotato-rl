"""Tests for ocean/brotato/extract_enemy_curse.py.

The generator emits the DLC cursed-enemy per-kind flags + tuning constants that
brotato_enemy_curse.h consumes.  These tests pin the source-derived expectations
(curse_scene_effect_behavior.gd:50-65 roll gate, enemy.gd:11 can_be_cursed,
the scene/gd tuning numbers) without re-deriving them from the C header.
"""

from __future__ import annotations

import importlib.util
import os
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
_MODULE_PATH = ROOT / "ocean" / "brotato" / "extract_enemy_curse.py"
_spec = importlib.util.spec_from_file_location("extract_ece_under_test", _MODULE_PATH)
extract_ece = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(extract_ece)


# --------------------------------------------------------------------------
# the `key = value` / `key: type = value` / `key: = value` resolver
# --------------------------------------------------------------------------
def test_scalar_reads_plain_assignment():
    assert extract_ece._scalar("hp_boost = 150\n", "hp_boost", "x", int) == 150


def test_scalar_reads_typed_and_untyped_gdscript_assignment():
    assert extract_ece._scalar("const MAX_CURSE_HP_BOOST: int = 300\n",
                               "MAX_CURSE_HP_BOOST", "x", int) == 300
    assert extract_ece._scalar("const CURSED_ENEMIES_BONUS_GOLD: = 0.33\n",
                               "CURSED_ENEMIES_BONUS_GOLD", "x", float) == pytest.approx(0.33)


def test_scalar_raises_when_absent():
    with pytest.raises(extract_ece.physics.ExtractError):
        extract_ece._scalar("nothing here\n", "hp_boost", "x", int)


def test_render_is_stable_and_contains_the_constants():
    data = {
        "can_be_cursed": [(0, "evil_mob"), (1, "anemone")],
        "rolls": [(0, "evil_mob"), (1, "anemone")],
        "hp_boost": 150, "damage_boost": 25, "speed_boost": 15,
        "max_hp_boost": 300, "gold_bonus": 0.33,
    }
    out = extract_ece.render(data)
    assert out == extract_ece.render(data)
    assert "#define BT_CURSE_HP_BOOST 150" in out
    assert "#define BT_CURSE_DAMAGE_BOOST 25" in out
    assert "#define BT_CURSE_SPEED_BOOST 15" in out
    assert "#define BT_CURSE_MAX_CURSE_HP_BOOST 300" in out
    assert "#define BT_CURSED_ENEMIES_BONUS_GOLD 0.33f" in out
    assert out.index("BT_KIND_CAN_BE_CURSED") < out.index("BT_KIND_CURSE_ROLLS")


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


def _args():
    return _Args(CAMPAIGN, DLC)


@needs_source
def test_real_curse_tables_match_the_scenes():
    data = extract_ece.extract(_args())
    cursed = dict((stem, v) for v, stem in data["can_be_cursed"])
    rolls = dict((stem, v) for v, stem in data["rolls"])
    assert len(cursed) == 66 and len(rolls) == 66

    # enemy.gd:11 + the six overriding scenes
    for stem in ("evil_mob", "evil_mob_swimmer", "looter", "looting_pig",
                 "iron_lung", "sea_pig"):
        assert cursed[stem] == 0, stem
    assert cursed["anemone"] == 1 and cursed["sea_pig"] == 0

    # sea_pig rolls but refuses the curse (the two gates are independent)
    assert rolls["sea_pig"] == 1
    # every loot alien is excluded from the roll
    for stem in ("evil_mob", "evil_mob_swimmer", "looter", "looting_pig"):
        assert rolls[stem] == 0, stem
    # plain enemies roll
    assert rolls["anemone"] == 1


@needs_source
def test_real_curse_constants_match_the_scenes():
    data = extract_ece.extract(_args())
    assert data["hp_boost"] == 150          # curse_scene_effect_behavior.tscn
    assert data["damage_boost"] == 25
    assert data["speed_boost"] == 15
    assert data["max_hp_boost"] == 300      # curse_scene_effect_behavior.gd:8
    assert data["gold_bonus"] == pytest.approx(0.33)  # curse_enemy_effect_behavior.gd:4


@needs_source
def test_real_header_generated_block_is_current():
    data = extract_ece.extract(_args())
    header = (ROOT / "ocean" / "brotato" / "brotato_enemy_curse.h").read_text()
    begin = header.index("/* ===================== BEGIN GENERATED =====================")
    end = header.index("/* ====================== END GENERATED ====================== */")
    end = header.index("\n", end) + 1
    assert header[begin:end] == extract_ece.render(data)
