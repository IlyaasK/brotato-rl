"""Replay profile-initialization tests.

The CPU replay driver (tests/brotato_core_trace.cpp) takes the run's profile
(character + starting weapon) as canonical generated-table ids.  These tests
check two observable things:

  * the recorded-profile -> canonical-id mapping used by ocean/brotato/bridge/
    replay_cpu.py (Bull stays Bull, Knight+weapon resolves, unknown/mismatch is
    rejected instead of silently defaulting to Bull);
  * on the real engine driver, the initialization actually changes the emitted
    observation (Bull Danger-5 max_hp 30 vs Knight max_hp 10), and out-of-range
    ids are refused.

They need the generated observation tables (gen_obs_tables.py) and a C++
compiler; otherwise they skip.
"""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

import pytest


ROOT = Path(__file__).resolve().parents[1]


def _replay_module():
    path = ROOT / "ocean/brotato/bridge/replay_cpu.py"
    spec = importlib.util.spec_from_file_location("brotato_replay_under_test", path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


REPLAY = _replay_module()
TABLES_PATH = REPLAY.default_tables_path()
CXX = os.environ.get("CXX", "clang++")

needs_tables = pytest.mark.skipif(
    not TABLES_PATH.is_file(), reason=f"no observation tables at {TABLES_PATH}")
needs_cxx = pytest.mark.skipif(shutil.which(CXX) is None, reason=f"{CXX} not found")


def _tables():
    return json.loads(TABLES_PATH.read_text())


def _hatchet_index(tables):
    for index, row in enumerate(tables["weapons"]):
        if row["my_id"] == "weapon_hatchet_2":
            return index
    pytest.skip("weapon_hatchet_2 is not in the weapon table")


def _run_driver(seed, zone, character, weapon, inputs="0 0\n"):
    with tempfile.TemporaryDirectory(prefix="brotato-replay-profile-") as tmp:
        executable = Path(tmp) / "core_trace"
        subprocess.run(
            [CXX, "-std=c++17", "-O2", "-ffp-contract=off",
             str(ROOT / "tests/brotato_core_trace.cpp"), "-o", str(executable)],
            check=True, capture_output=True)
        result = subprocess.run(
            [str(executable), str(seed), str(zone), str(character), str(weapon)],
            input=inputs, text=True, capture_output=True, check=True)
    return [json.loads(line) for line in result.stdout.splitlines()]


@needs_tables
def test_recorded_bull_profile_stays_bull():
    character, weapon, _ = REPLAY.resolve_profile(
        _tables(), {"config": {"character_id": "character_bull", "starting_weapon": ""}}, "", "")
    assert (character, weapon) == (0, 0xFFFF)


@needs_tables
def test_recorded_knight_weapon_profile_resolves_to_canonical_ids():
    tables = _tables()
    character, weapon, _ = REPLAY.resolve_profile(
        tables,
        {"config": {"character_id": "character_knight", "starting_weapon": "weapon_hatchet_2"}},
        "", "")
    assert character == 1
    assert weapon == _hatchet_index(tables)


@needs_tables
def test_missing_metadata_defaults_to_bull():
    character, weapon, _ = REPLAY.resolve_profile(_tables(), {}, "", "")
    assert (character, weapon) == (0, 0xFFFF)


@needs_tables
def test_unknown_recorded_profile_is_rejected_not_defaulted_to_bull():
    with pytest.raises(REPLAY.TraceError):
        REPLAY.resolve_profile(
            _tables(), {"config": {"character_id": "character_ghost"}}, "", "")


@needs_tables
def test_explicit_override_mismatch_is_rejected():
    meta = {"config": {"character_id": "character_knight", "starting_weapon": ""}}
    with pytest.raises(REPLAY.TraceError):
        REPLAY.resolve_profile(_tables(), meta, "character_bull", "")


@needs_tables
@needs_cxx
def test_driver_initialization_is_observable_bull_vs_knight():
    tables = _tables()
    bull = _run_driver(1, 0, 0, 0xFFFF)
    assert bull[0]["obs"][1] * 64 == pytest.approx(30)     # Bull Danger-5 max_hp
    knight = _run_driver(1, 0, 1, _hatchet_index(tables))
    assert knight[0]["obs"][1] * 64 == pytest.approx(10)   # Knight max_hp
    assert bull[0]["obs"] != knight[0]["obs"]


@needs_cxx
def test_driver_rejects_out_of_range_profile_ids():
    with pytest.raises(subprocess.CalledProcessError):
        _run_driver(1, 0, 7, 0xFFFF)
