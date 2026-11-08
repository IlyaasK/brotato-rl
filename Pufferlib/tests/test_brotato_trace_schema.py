"""Reject incompatible trace schemas before they reach policy/replay consumers."""
import importlib.util
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "brotato_trace_schema", ROOT / "ocean/brotato/bridge/parse_trace.py"
)
TRACE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(TRACE)


def row(tick=0, **changes):
    result = dict(tick=tick, action=30, obs=[0.0] * 203, hp=10,
                  wave=1, enemies=0, obs_complete=1,
                  contract="full_v5", obs_version=5)
    result.update(changes)
    return result


def test_current_schema_accepts_crate_action_and_preserves_identity():
    records = [row()]
    assert TRACE.validate(records, Path("trace"), 203, False, 31) == (203, [])
    assert TRACE.summarise(records, 203, False)["contract"] == "full_v5"


@pytest.mark.parametrize("changes", [
    # a v4 row (198-wide, full_v4 / obs_version 4) is an older contract: rejected
    {"contract": "full_v4"},
    {"obs_version": 4},
    {"obs": [0.0] * 198},
    # and the v3 contract before it
    {"contract": "full_v3"},
    {"obs_version": 3},
    {"obs_version": True},
    {"obs": [0.0] * 161},
    {"action": 31},
])
def test_incompatible_row_is_rejected_even_after_valid_prefix(changes):
    with pytest.raises(TRACE.TraceError):
        TRACE.validate([row(), row(1, **changes)], Path("trace"), 203, False, 31)


def test_partial_diagnostics_cannot_enter_full_observation_pipeline():
    records = [row(contract="partial_v0", obs_version=0,
                   obs=[0.0] * 6, obs_complete=0)]
    assert TRACE.validate(records, Path("trace"), 6, True, 31) == (6, [])
    with pytest.raises(TRACE.TraceError):
        TRACE.validate(records, Path("trace"), 203, False, 31)
