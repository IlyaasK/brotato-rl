"""Regression tests for tests/brotato_trace_compare.py.

Loads the comparator module by path (no sys.path/import-mode assumptions) and
exercises the real comparison boundaries: alignment, tolerances, truncation,
action mismatch, non-finite values, and schema validation.
"""

from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
from pathlib import Path

import pytest

_CLI_PATH = Path(__file__).with_name("brotato_trace_compare.py")
_spec = importlib.util.spec_from_file_location("brotato_trace_compare_under_test", _CLI_PATH)
btc = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = btc
_spec.loader.exec_module(btc)


def record(tick, obs, action=0, hp=10.0, wave=1, enemies=3, x=None, y=None):
    entry = {
        "tick": tick,
        "action": action,
        "obs": list(obs),
        "hp": hp,
        "wave": wave,
        "enemies": enemies,
    }
    if x is not None:
        entry["x"] = x
    if y is not None:
        entry["y"] = y
    return entry


def write_trace(path, records):
    path.write_text("\n".join(json.dumps(r) for r in records) + "\n", encoding="utf-8")
    return path


def write_raw(path, text):
    path.write_text(text, encoding="utf-8")
    return path


def load(path, obs_size=None):
    return btc.load_trace(str(path), obs_size=obs_size)


def run(reference, candidate, atol=0.0, rtol=0.0):
    ref = load(reference)
    cand = load(candidate, obs_size=ref.obs_size)
    return ref, cand, btc.compare(ref, cand, atol, rtol)


# ---------------------------------------------------------------------------
# happy path
# ---------------------------------------------------------------------------


def test_identical_traces_match(tmp_path):
    rows = [record(t, [1.0, 2.0], hp=5.5, x=0.1, y=0.2) for t in range(4)]
    ref = write_trace(tmp_path / "ref.jsonl", rows)
    cand = write_trace(tmp_path / "cand.jsonl", rows)

    _, _, result = run(ref, cand)

    assert result.exit_code == btc.EXIT_MATCH
    assert result.status == "MATCH"
    assert result.first_divergence is None
    assert result.max_abs_error == 0.0
    assert result.action_mismatches == 0


@pytest.mark.parametrize("field,value", [
    ("wave", 2), ("enemies", 4), ("action", 1), ("action", [1]),
])
def test_discrete_state_cannot_be_hidden_by_float_tolerance(tmp_path, field, value):
    baseline = record(0, [0.0], action=[0] if isinstance(value, list) else 0)
    changed = dict(baseline)
    changed[field] = value
    ref = write_trace(tmp_path / "ref.jsonl", [baseline])
    cand = write_trace(tmp_path / "cand.jsonl", [changed])
    _, _, result = run(ref, cand, atol=100.0, rtol=100.0)
    assert result.exit_code == btc.EXIT_DIVERGED
    assert result.first_divergence.path == field


def test_enemy_node_counts_compared_when_both_traces_carry_them(tmp_path):
    """The game's RunData counter is broken at debug starts (it goes negative),
    and its scene count includes nodes still playing their death animation.
    When both sides report a node count, nodes are compared with nodes, so a
    dying enemy (living 4, nodes 5) is not a divergence.
    """
    ref_row = record(0, [0.0])
    ref_row["enemies"] = -2                  # broken game counter at a debug start
    ref_row["living_enemies_nodes"] = 5      # live + dying scene nodes
    cand_row = record(0, [0.0], enemies=4)   # sim living count excludes the dying one
    cand_row["enemy_nodes"] = 5
    ref = write_trace(tmp_path / "ref.jsonl", [ref_row])
    cand = write_trace(tmp_path / "cand.jsonl", [cand_row])
    _, _, result = run(ref, cand)
    assert result.exit_code == btc.EXIT_MATCH
    assert result.first_divergence is None

    cand_row["enemy_nodes"] = 6
    cand = write_trace(tmp_path / "cand2.jsonl", [cand_row])
    _, _, result = run(ref, cand)
    assert result.exit_code == btc.EXIT_DIVERGED
    assert result.first_divergence.path == "enemy_nodes"
    assert result.first_divergence.detail == "reference=5 candidate=6"

    # A legacy reference without the node count keeps the historical behaviour.
    legacy = record(0, [0.0], enemies=7)
    legacy_ref = write_trace(tmp_path / "legacy.jsonl", [legacy])
    _, _, result = run(legacy_ref, write_trace(tmp_path / "cand3.jsonl",
                                               [record(0, [0.0], enemies=7)]))
    assert result.exit_code == btc.EXIT_MATCH
    _, _, result = run(legacy_ref, write_trace(tmp_path / "cand4.jsonl",
                                               [record(0, [0.0], enemies=6)]))
    assert result.exit_code == btc.EXIT_DIVERGED
    assert result.first_divergence.path == "enemies"


def test_obs_size_inferred_from_first_row(tmp_path):
    ref = write_trace(tmp_path / "ref.jsonl", [record(0, [1.0, 2.0, 3.0])])
    cand = write_trace(tmp_path / "cand.jsonl", [record(0, [1.0, 2.0, 3.0])])

    ref_trace = load(ref)
    cand_trace = load(cand, obs_size=ref_trace.obs_size)

    assert ref_trace.obs_size == 3
    assert cand_trace.obs_size == 3


# ---------------------------------------------------------------------------
# tolerance boundaries
# ---------------------------------------------------------------------------


def test_within_tolerance_matches(tmp_path):
    ref = write_trace(tmp_path / "ref.jsonl", [record(0, [1.0], hp=100.0)])
    cand = write_trace(tmp_path / "cand.jsonl", [record(0, [1.0 + 5e-4], hp=100.0)])

    _, _, result = run(ref, cand, atol=1e-3, rtol=0.0)

    assert result.exit_code == btc.EXIT_MATCH
    assert result.first_divergence is None


def test_exactly_at_atol_boundary_matches(tmp_path):
    ref = write_trace(tmp_path / "ref.jsonl", [record(0, [0.0])])
    cand = write_trace(tmp_path / "cand.jsonl", [record(0, [1e-3])])

    _, _, result = run(ref, cand, atol=1e-3, rtol=0.0)
    assert result.exit_code == btc.EXIT_MATCH


def test_just_beyond_atol_diverges(tmp_path):
    ref = write_trace(tmp_path / "ref.jsonl", [record(0, [0.0])])
    cand = write_trace(tmp_path / "cand.jsonl", [record(0, [1.001e-3])])

    _, _, result = run(ref, cand, atol=1e-3, rtol=0.0)

    assert result.exit_code == btc.EXIT_DIVERGED
    assert result.first_divergence is not None
    assert result.first_divergence.path == "obs[0]"


def test_rtol_uses_reference_as_base(tmp_path):
    ref = write_trace(tmp_path / "ref.jsonl", [record(0, [0.0], hp=1000.0)])
    cand = write_trace(tmp_path / "cand.jsonl", [record(0, [0.0], hp=1000.9)])

    _, _, within = run(ref, cand, atol=0.0, rtol=1e-3)
    _, _, beyond = run(ref, cand, atol=0.0, rtol=1e-4)

    assert within.exit_code == btc.EXIT_MATCH
    assert beyond.exit_code == btc.EXIT_DIVERGED
    assert beyond.first_divergence.path == "hp"


def test_max_errors_reported(tmp_path):
    ref = write_trace(tmp_path / "ref.jsonl", [record(0, [1.0, 10.0]), record(1, [1.0, 10.0])])
    cand = write_trace(tmp_path / "cand.jsonl", [record(0, [1.5, 10.0]), record(1, [1.0, 8.0])])

    _, _, result = run(ref, cand)

    # max abs error is from obs[1] at tick 1 (|10 - 8| = 2).
    assert result.max_abs_error == pytest.approx(2.0)
    assert result.max_rel_error == pytest.approx(0.5)  # |1 - 1.5| / 1
    assert result.first_divergence.tick == 0
    assert result.first_divergence.path == "obs[0]"


# ---------------------------------------------------------------------------
# truncation / alignment
# ---------------------------------------------------------------------------


def test_truncated_candidate_is_invalid(tmp_path):
    rows = [record(t, [1.0]) for t in range(3)]
    ref = write_trace(tmp_path / "ref.jsonl", rows)
    cand = write_trace(tmp_path / "cand.jsonl", rows[:2])

    _, _, result = run(ref, cand)

    assert result.exit_code == btc.EXIT_INVALID
    assert result.status == "INVALID"
    assert "record count mismatch" in result.message


def test_extra_record_in_candidate_is_invalid(tmp_path):
    rows = [record(t, [1.0]) for t in range(3)]
    ref = write_trace(tmp_path / "ref.jsonl", rows[:2])
    cand = write_trace(tmp_path / "cand.jsonl", rows)

    _, _, result = run(ref, cand)
    assert result.exit_code == btc.EXIT_INVALID


def test_same_length_shifted_ticks_diverge(tmp_path):
    ref = write_trace(tmp_path / "ref.jsonl", [record(0, [1.0]), record(1, [1.0])])
    cand = write_trace(tmp_path / "cand.jsonl", [record(0, [1.0]), record(2, [1.0])])

    _, _, result = run(ref, cand)

    assert result.exit_code == btc.EXIT_DIVERGED
    assert result.first_divergence.path == "tick"
    assert result.first_divergence.tick == 2


def test_first_divergence_is_earliest_tick(tmp_path):
    ref = write_trace(
        tmp_path / "ref.jsonl",
        [record(0, [1.0]), record(1, [1.0]), record(2, [1.0])],
    )
    cand = write_trace(
        tmp_path / "cand.jsonl",
        [record(0, [1.0]), record(1, [2.0]), record(2, [3.0])],
    )

    _, _, result = run(ref, cand)
    assert result.first_divergence.tick == 1


# ---------------------------------------------------------------------------
# action comparison
# ---------------------------------------------------------------------------


def test_scalar_action_mismatch_diverges(tmp_path):
    ref = write_trace(tmp_path / "ref.jsonl", [record(0, [1.0], action=1)])
    cand = write_trace(tmp_path / "cand.jsonl", [record(0, [1.0], action=2)])

    _, _, result = run(ref, cand)

    assert result.exit_code == btc.EXIT_DIVERGED
    assert result.first_divergence.path == "action"
    assert result.action_mismatches == 1


def test_array_action_mismatch_diverges(tmp_path):
    ref = write_trace(tmp_path / "ref.jsonl", [record(0, [1.0], action=[0, 1, 0])])
    cand = write_trace(tmp_path / "cand.jsonl", [record(0, [1.0], action=[0, 1, 1])])

    _, _, result = run(ref, cand)

    assert result.exit_code == btc.EXIT_DIVERGED
    assert result.first_divergence.path == "action"
    assert "action[2]" in result.first_divergence.detail


def test_action_kind_mismatch_diverges(tmp_path):
    ref = write_trace(tmp_path / "ref.jsonl", [record(0, [1.0], action=1)])
    cand = write_trace(tmp_path / "cand.jsonl", [record(0, [1.0], action=[1])])

    _, _, result = run(ref, cand)

    assert result.exit_code == btc.EXIT_DIVERGED
    assert "kind mismatch" in result.first_divergence.detail


def test_action_length_mismatch_diverges(tmp_path):
    ref = write_trace(tmp_path / "ref.jsonl", [record(0, [1.0], action=[0, 1])])
    cand = write_trace(tmp_path / "cand.jsonl", [record(0, [1.0], action=[0, 1, 0])])

    _, _, result = run(ref, cand)

    assert result.exit_code == btc.EXIT_DIVERGED
    assert "length mismatch" in result.first_divergence.detail


def test_matching_array_action_passes(tmp_path):
    rows = [record(t, [1.0], action=[0, 1, 2]) for t in range(2)]
    ref = write_trace(tmp_path / "ref.jsonl", rows)
    cand = write_trace(tmp_path / "cand.jsonl", rows)

    _, _, result = run(ref, cand)
    assert result.exit_code == btc.EXIT_MATCH


# ---------------------------------------------------------------------------
# schema validation
# ---------------------------------------------------------------------------


def test_duplicate_tick_is_invalid(tmp_path):
    ref = write_trace(tmp_path / "ref.jsonl", [record(0, [1.0]), record(0, [1.0])])
    cand = write_trace(tmp_path / "cand.jsonl", [record(0, [1.0]), record(0, [1.0])])

    with pytest.raises(btc.TraceError, match="duplicate tick 0"):
        load(ref)
    with pytest.raises(btc.TraceError, match="duplicate tick 0"):
        load(cand)


def test_missing_required_field_is_invalid(tmp_path):
    bad = {"tick": 0, "action": 0, "obs": [1.0], "hp": 1.0, "wave": 1}
    path = write_trace(tmp_path / "ref.jsonl", [bad])

    with pytest.raises(btc.TraceError, match="missing required field"):
        load(path)


def test_obs_shape_mismatch_is_invalid(tmp_path):
    rows = [record(0, [1.0, 2.0]), record(1, [1.0])]
    path = write_trace(tmp_path / "ref.jsonl", rows)

    with pytest.raises(btc.TraceError, match="obs size 1 does not match expected 2"):
        load(path)


def test_explicit_obs_size_mismatch_is_invalid(tmp_path):
    path = write_trace(tmp_path / "ref.jsonl", [record(0, [1.0, 2.0])])

    with pytest.raises(btc.TraceError, match="obs size 2 does not match expected 3"):
        load(path, obs_size=3)


@pytest.mark.parametrize("bad_value", [float("nan"), float("inf"), float("-inf")])
def test_non_finite_obs_is_invalid(tmp_path, bad_value):
    path = write_raw(
        tmp_path / "ref.jsonl",
        json.dumps(record(0, [1.0, bad_value])) + "\n",
    )

    with pytest.raises(btc.TraceError, match="non-finite"):
        load(path)


def test_non_finite_hp_is_invalid(tmp_path):
    path = write_raw(
        tmp_path / "ref.jsonl",
        json.dumps(record(0, [1.0], hp=float("nan"))) + "\n",
    )

    with pytest.raises(btc.TraceError, match="non-finite"):
        load(path)


def test_non_finite_action_is_invalid(tmp_path):
    path = write_raw(
        tmp_path / "ref.jsonl",
        json.dumps(record(0, [1.0], action=float("inf"))) + "\n",
    )

    with pytest.raises(btc.TraceError, match="non-finite"):
        load(path)


def test_obs_not_array_is_invalid(tmp_path):
    path = write_trace(tmp_path / "ref.jsonl", [record(0, [1.0])])
    raw = json.loads(path.read_text())
    raw["obs"] = 1.0
    write_trace(path, [raw])

    with pytest.raises(btc.TraceError, match="obs must be an array"):
        load(path)


def test_non_integer_tick_is_invalid(tmp_path):
    path = write_trace(tmp_path / "ref.jsonl", [record(0.5, [1.0])])

    with pytest.raises(btc.TraceError, match="tick must be an integer"):
        load(path)


def test_non_integer_wave_is_invalid(tmp_path):
    row = record(0, [1.0])
    row["wave"] = 1.5
    path = write_trace(tmp_path / "ref.jsonl", [row])

    with pytest.raises(btc.TraceError, match="wave must be an integer"):
        load(path)


def test_empty_trace_is_invalid(tmp_path):
    path = write_raw(tmp_path / "ref.jsonl", "\n\n")

    with pytest.raises(btc.TraceError, match="trace is empty"):
        load(path)


def test_invalid_json_line_is_invalid(tmp_path):
    path = write_raw(tmp_path / "ref.jsonl", "{not json}\n")

    with pytest.raises(btc.TraceError, match="invalid JSON"):
        load(path)


def test_missing_file_is_invalid(tmp_path):
    with pytest.raises(btc.TraceError, match="no such trace file"):
        load(tmp_path / "nope.jsonl")


# ---------------------------------------------------------------------------
# optional position fields
# ---------------------------------------------------------------------------


def test_position_fields_compared(tmp_path):
    ref = write_trace(tmp_path / "ref.jsonl", [record(0, [1.0], x=0.0, y=0.0)])
    cand = write_trace(tmp_path / "cand.jsonl", [record(0, [1.0], x=0.0, y=1.5)])

    _, _, result = run(ref, cand)

    assert result.exit_code == btc.EXIT_DIVERGED
    assert result.first_divergence.path == "y"


def test_position_present_only_in_reference_diverges(tmp_path):
    ref = write_trace(tmp_path / "ref.jsonl", [record(0, [1.0], x=0.0, y=0.0)])
    cand = write_trace(tmp_path / "cand.jsonl", [record(0, [1.0])])

    _, _, result = run(ref, cand)

    assert result.exit_code == btc.EXIT_DIVERGED
    assert "position fields" in result.message


def test_partial_position_fields_invalid(tmp_path):
    row = record(0, [1.0], x=0.0)
    path = write_trace(tmp_path / "ref.jsonl", [row])

    with pytest.raises(btc.TraceError, match="both present or both absent"):
        load(path)


# ---------------------------------------------------------------------------
# CLI entry point
# ---------------------------------------------------------------------------


def test_main_returns_match_code(tmp_path, capsys):
    rows = [record(0, [1.0], hp=3.0)]
    ref = write_trace(tmp_path / "ref.jsonl", rows)
    cand = write_trace(tmp_path / "cand.jsonl", rows)

    code = btc.main([str(ref), str(cand)])
    out = capsys.readouterr().out

    assert code == btc.EXIT_MATCH
    assert "status: MATCH" in out


def test_main_reports_first_divergence_and_exit_code(tmp_path, capsys):
    ref = write_trace(tmp_path / "ref.jsonl", [record(0, [1.0])])
    cand = write_trace(tmp_path / "cand.jsonl", [record(0, [2.0])])

    code = btc.main([str(ref), str(cand)])
    out = capsys.readouterr().out

    assert code == btc.EXIT_DIVERGED
    assert "first_divergence: tick=0 path=obs[0]" in out


def test_main_invalid_trace_returns_invalid_code(tmp_path, capsys):
    ref = write_trace(tmp_path / "ref.jsonl", [record(0, [1.0])])
    cand = write_trace(tmp_path / "cand.jsonl", [record(0, [1.0]), record(1, [1.0])])

    code = btc.main([str(ref), str(cand)])
    err = capsys.readouterr().err

    assert code == btc.EXIT_INVALID
    assert "invalid trace" in err


def test_main_rejects_negative_tolerance(tmp_path, capsys):
    ref = write_trace(tmp_path / "ref.jsonl", [record(0, [1.0])])
    cand = write_trace(tmp_path / "cand.jsonl", [record(0, [1.0])])

    code = btc.main([str(ref), str(cand), "--atol", "-1"])
    assert code == btc.EXIT_INVALID


def test_main_explicit_obs_size(tmp_path, capsys):
    ref = write_trace(tmp_path / "ref.jsonl", [record(0, [1.0, 2.0])])
    cand = write_trace(tmp_path / "cand.jsonl", [record(0, [1.0, 2.0])])

    code = btc.main([str(ref), str(cand), "--obs-size", "2"])
    assert code == btc.EXIT_MATCH


def test_cli_is_runnable_as_subprocess(tmp_path):
    rows = [record(0, [1.0], hp=3.0)]
    ref = write_trace(tmp_path / "ref.jsonl", rows)
    cand = write_trace(tmp_path / "cand.jsonl", rows)

    result = subprocess.run(
        [sys.executable, str(_CLI_PATH), str(ref), str(cand)],
        capture_output=True,
        text=True,
        check=False,
    )

    assert result.returncode == btc.EXIT_MATCH, result.stderr
    assert "status: MATCH" in result.stdout
