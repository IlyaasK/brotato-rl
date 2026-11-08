#!/usr/bin/env python3
"""Compare two Brotato JSONL gameplay traces tick-by-tick.

Each line of a trace file is one JSON object with the schema::

    {"tick": <int>, "action": <int|[number, ...]>, "obs": [<number>, ...],
     "hp": <number>, "wave": <int>, "enemies": <int>}

Optional position fields ``x`` and ``y`` are compared when present.

The two traces are aligned by exact tick. A tick that appears only in one
file, or a differing record count, is a structural (invalid) failure. Within
each aligned tick, ``obs``, ``action``, and every scalar field are compared
element-wise. Numeric equality uses ``|candidate - reference| <=
atol + rtol * |reference|`` (the reference trace sets the scale).

Exit codes::

    0  traces match within tolerance
    1  the traces diverged (numeric / action mismatch)
    2  one of the traces is invalid (bad schema, duplicate ticks,
       record-count mismatch, observation shape mismatch, non-finite value)

Usage::

    python tests/brotato_trace_compare.py reference.jsonl candidate.jsonl
    python tests/brotato_trace_compare.py ref.jsonl cand.jsonl --atol 1e-4 --rtol 1e-3
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from dataclasses import dataclass, field
from pathlib import Path

EXIT_MATCH = 0
EXIT_DIVERGED = 1
EXIT_INVALID = 2

REQUIRED_FIELDS = ("tick", "action", "obs", "hp", "wave", "enemies")
POSITION_FIELDS = ("x", "y")


class TraceError(Exception):
    """Raised when a trace is structurally invalid."""


@dataclass
class Trace:
    path: str
    records: list[dict]  # ordered by line, already schema-checked
    obs_size: int
    has_position: bool


@dataclass
class Divergence:
    tick: int
    path: str
    detail: str


@dataclass
class CompareResult:
    exit_code: int
    status: str
    message: str = ""
    first_divergence: Divergence | None = None
    max_abs_error: float = 0.0
    max_rel_error: float = 0.0
    fields_compared: dict = field(default_factory=dict)
    action_mismatches: int = 0


def _path_join(*parts: str) -> str:
    return ".".join(str(p) for p in parts if p != "")


def _is_int(value) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _is_number(value) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def _require_finite(value, where: str) -> float:
    number = float(value)
    if not math.isfinite(number):
        raise TraceError(f"{where}: non-finite value {value!r}")
    return number


def _read_jsonl(path: str) -> list[tuple[int, dict]]:
    source = Path(path)
    if not source.is_file():
        raise TraceError(f"{path}: no such trace file")
    records: list[dict] = []
    with source.open("r", encoding="utf-8") as handle:
        for lineno, raw in enumerate(handle, start=1):
            line = raw.strip()
            if not line:
                continue
            try:
                value = json.loads(line)
            except json.JSONDecodeError as exc:
                raise TraceError(f"{path}:{lineno}: invalid JSON ({exc.msg})") from exc
            if not isinstance(value, dict):
                raise TraceError(f"{path}:{lineno}: record must be a JSON object")
            records.append((lineno, value))
    if not records:
        raise TraceError(f"{path}: trace is empty")
    return records


def _validate_action(value, where: str):
    if isinstance(value, bool):
        raise TraceError(f"{where}: action must be int or array of numbers")
    if _is_number(value):
        _require_finite(value, where)
        return "scalar"
    if isinstance(value, list):
        if not value:
            raise TraceError(f"{where}: action array is empty")
        for index, item in enumerate(value):
            if not _is_number(item):
                raise TraceError(f"{where}[{index}]: action element must be a number")
            _require_finite(item, f"{where}[{index}]")
        return "array"
    raise TraceError(f"{where}: action must be int or array of numbers")


def load_trace(path: str, obs_size: int | None = None) -> Trace:
    """Read and schema-check a JSONL trace.

    ``obs_size`` may be given explicitly; otherwise it is inferred from the
    first record and every record must agree with it.
    """
    raw_records = _read_jsonl(path)

    if obs_size is not None and obs_size <= 0:
        raise TraceError(f"--obs-size must be positive, got {obs_size}")

    expected_obs = obs_size
    has_position = False
    records: list[dict] = []
    seen_ticks: set[int] = set()

    for lineno, record in raw_records:
        where = f"{path}:{lineno}"
        missing = [name for name in REQUIRED_FIELDS if name not in record]
        if missing:
            raise TraceError(f"{where}: missing required field(s): {', '.join(missing)}")

        tick = record["tick"]
        if not _is_int(tick):
            raise TraceError(f"{where}: tick must be an integer, got {tick!r}")
        if tick in seen_ticks:
            raise TraceError(f"{where}: duplicate tick {tick}")
        seen_ticks.add(tick)

        obs = record["obs"]
        if not isinstance(obs, list):
            raise TraceError(f"{where}: obs must be an array of numbers")
        if not obs:
            raise TraceError(f"{where}: obs array is empty")
        for index, item in enumerate(obs):
            if not _is_number(item):
                raise TraceError(f"{where}: obs[{index}] must be a number")
            _require_finite(item, f"{where}: obs[{index}]")

        if expected_obs is None:
            expected_obs = len(obs)
        elif len(obs) != expected_obs:
            raise TraceError(
                f"{where}: obs size {len(obs)} does not match expected {expected_obs}"
            )

        _validate_action(record["action"], f"{where}: action")

        for name in ("hp",):
            if not _is_number(record[name]):
                raise TraceError(f"{where}: {name} must be a number")
            _require_finite(record[name], f"{where}: {name}")
        for name in ("wave", "enemies"):
            if not _is_int(record[name]):
                raise TraceError(f"{where}: {name} must be an integer, got {record[name]!r}")

        present = [name for name in POSITION_FIELDS if name in record]
        if present and len(present) != len(POSITION_FIELDS):
            raise TraceError(
                f"{where}: position fields must be both present or both absent "
                f"(found {', '.join(present)})"
            )
        for name in present:
            if not _is_number(record[name]):
                raise TraceError(f"{where}: {name} must be a number")
            _require_finite(record[name], f"{where}: {name}")
        has_position = has_position or bool(present)

        records.append(record)

    return Trace(path=path, records=records, obs_size=int(expected_obs), has_position=has_position)


def _numbers_close(ref_value: float, cand_value: float, atol: float, rtol: float) -> bool:
    """|cand - ref| <= atol + rtol * |ref| (the reference trace sets the scale)."""
    return abs(cand_value - ref_value) <= atol + rtol * abs(ref_value)


def _track(abs_error: float, ref_value: float, acc: dict) -> None:
    if abs_error > acc["abs"]:
        acc["abs"] = abs_error
    if ref_value != 0.0:
        rel = abs_error / abs(ref_value)
        if rel > acc["rel"]:
            acc["rel"] = rel


def compare(reference: Trace, candidate: Trace, atol: float, rtol: float) -> CompareResult:
    """Compare two validated traces aligned by exact tick."""
    if len(reference.records) != len(candidate.records):
        return CompareResult(
            exit_code=EXIT_INVALID,
            status="INVALID",
            message=(
                f"record count mismatch: reference has {len(reference.records)}, "
                f"candidate has {len(candidate.records)}"
            ),
        )

    if reference.has_position != candidate.has_position:
        only_in = reference.path if reference.has_position else candidate.path
        return CompareResult(
            exit_code=EXIT_DIVERGED,
            status="DIVERGED",
            message=f"position fields x/y present only in {only_in}",
        )

    errors = {"abs": 0.0, "rel": 0.0}
    fields_compared = {
        "records": len(reference.records),
        "obs_values": 0,
        "scalar_values": 0,
    }
    first: Divergence | None = None
    action_mismatches = 0

    def note(tick: int, path: str, detail: str) -> None:
        nonlocal first
        if first is None:
            first = Divergence(tick=tick, path=path, detail=detail)

    scalar_fields = ["hp"]
    if reference.has_position:
        scalar_fields.extend(POSITION_FIELDS)

    for ref_record, cand_record in zip(reference.records, candidate.records):
        ref_tick = ref_record["tick"]
        cand_tick = cand_record["tick"]
        if ref_tick != cand_tick:
            note(
                cand_tick,
                "tick",
                f"tick mismatch: reference={ref_tick} candidate={cand_tick}",
            )
            continue
        tick = ref_tick
        for name in ("wave", "enemies"):
            ref_value = ref_record[name]
            # A live capture's `enemies` is RunData.current_living_enemies
            # (run_data.gd:158), which is start-wave-dependent: a debug
            # `starting_wave` boot skips the spawn-side `+= 1` (main.gd:1429)
            # while its deaths still decrement (main.gd:478), so the counter goes
            # NEGATIVE at deep starts. The bridge's `living_enemies_nodes` walks
            # the scene tree, so it also counts nodes still playing their death
            # animation (equal to `enemy_nodes` on every row). Compare node count
            # with node count when both traces carry `enemy_nodes` (the sim's is
            # the observation's ealive||edying count), else keep the historical
            # direct comparison.
            cand_value = cand_record[name]
            if name == "enemies" and _is_int(ref_record.get("living_enemies_nodes")) \
                    and _is_int(cand_record.get("enemy_nodes")):
                name = "enemy_nodes"
                ref_value = ref_record["living_enemies_nodes"]
                cand_value = cand_record["enemy_nodes"]
            fields_compared["scalar_values"] += 1
            if ref_value != cand_value:
                note(tick, name,
                     f"reference={ref_value!r} candidate={cand_value!r}")

        ref_obs = ref_record["obs"]
        cand_obs = cand_record["obs"]
        if len(ref_obs) != len(cand_obs):
            note(
                tick,
                "obs",
                f"shape mismatch: reference has {len(ref_obs)} candidate has {len(cand_obs)}",
            )
        else:
            for index, (a, b) in enumerate(zip(ref_obs, cand_obs)):
                fields_compared["obs_values"] += 1
                abs_error = abs(float(a) - float(b))
                _track(abs_error, float(a), errors)
                if not _numbers_close(float(a), float(b), atol, rtol):
                    note(
                        tick,
                        f"obs[{index}]",
                        f"reference={a!r} candidate={b!r} abs_err={abs_error:.6e}",
                    )

        for name in scalar_fields:
            if name in ref_record and name in cand_record:
                a = ref_record[name]
                b = cand_record[name]
            elif name in ref_record or name in cand_record:
                note(tick, name, "field present in only one trace")
                continue
            else:
                continue
            fields_compared["scalar_values"] += 1
            abs_error = abs(float(a) - float(b))
            _track(abs_error, float(a), errors)
            if not _numbers_close(float(a), float(b), atol, rtol):
                note(tick, name, f"reference={a!r} candidate={b!r} abs_err={abs_error:.6e}")

        ref_action = ref_record["action"]
        cand_action = cand_record["action"]
        actions_match, detail = _actions_match(ref_action, cand_action)
        if not actions_match:
            action_mismatches += 1
            note(tick, "action", detail)

    if first is not None:
        return CompareResult(
            exit_code=EXIT_DIVERGED,
            status="DIVERGED",
            first_divergence=first,
            max_abs_error=errors["abs"],
            max_rel_error=errors["rel"],
            fields_compared=fields_compared,
            action_mismatches=action_mismatches,
        )

    return CompareResult(
        exit_code=EXIT_MATCH,
        status="MATCH",
        max_abs_error=errors["abs"],
        max_rel_error=errors["rel"],
        fields_compared=fields_compared,
        action_mismatches=0,
    )


def _actions_match(ref_action, cand_action):
    ref_is_array = isinstance(ref_action, list)
    cand_is_array = isinstance(cand_action, list)
    if ref_is_array != cand_is_array:
        return False, (
            f"action kind mismatch: {'array' if ref_is_array else 'scalar'} vs "
            f"{'array' if cand_is_array else 'scalar'}"
        )
    if ref_is_array:
        if len(ref_action) != len(cand_action):
            return False, (
                f"action length mismatch: {len(ref_action)} vs {len(cand_action)}"
            )
        ref_items, cand_items = ref_action, cand_action
    else:
        ref_items, cand_items = [ref_action], [cand_action]
    for index, (a, b) in enumerate(zip(ref_items, cand_items)):
        if a != b:
            return False, (
                f"action[{index}] mismatch: reference={a!r} candidate={b!r}"
                if ref_is_array
                else f"action mismatch: reference={a!r} candidate={b!r}"
            )
    return True, ""


def format_report(reference: Trace, candidate: Trace, atol: float, rtol: float,
                  result: CompareResult) -> str:
    lines = []
    lines.append(f"reference: {reference.path} ({len(reference.records)} records)")
    lines.append(f"candidate: {candidate.path} ({len(candidate.records)} records)")
    lines.append(f"obs_size: {reference.obs_size}")
    lines.append(f"atol: {atol:g}  rtol: {rtol:g}")
    lines.append(f"status: {result.status}")
    if result.message:
        lines.append(f"message: {result.message}")
    if result.first_divergence is not None:
        div = result.first_divergence
        lines.append(f"first_divergence: tick={div.tick} path={div.path} {div.detail}")
    lines.append(f"max_abs_error: {result.max_abs_error:.6e}")
    lines.append(f"max_rel_error: {result.max_rel_error:.6e}")
    compared = result.fields_compared
    if compared:
        lines.append(
            "compared: "
            f"records={compared.get('records', 0)} "
            f"obs_values={compared.get('obs_values', 0)} "
            f"scalar_values={compared.get('scalar_values', 0)}"
        )
    lines.append(f"action_mismatches: {result.action_mismatches}")
    return "\n".join(lines)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="brotato_trace_compare",
        description="Compare two Brotato JSONL gameplay traces tick-by-tick.",
    )
    parser.add_argument("reference", help="reference JSONL trace (defines rtol base)")
    parser.add_argument("candidate", help="candidate JSONL trace to check")
    parser.add_argument(
        "--atol",
        type=float,
        default=0.0,
        help="absolute tolerance for numeric fields (default: 0.0)",
    )
    parser.add_argument(
        "--rtol",
        type=float,
        default=0.0,
        help="relative tolerance for numeric fields (default: 0.0)",
    )
    parser.add_argument(
        "--obs-size",
        type=int,
        default=None,
        help="expected observation length; inferred from the first record when omitted",
    )
    return parser


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    if args.atol < 0 or args.rtol < 0:
        print("error: --atol and --rtol must be non-negative", file=sys.stderr)
        return EXIT_INVALID

    try:
        reference = load_trace(args.reference, obs_size=args.obs_size)
    except TraceError as exc:
        print(f"invalid trace: {exc}", file=sys.stderr)
        return EXIT_INVALID
    try:
        candidate = load_trace(args.candidate, obs_size=args.obs_size or reference.obs_size)
    except TraceError as exc:
        print(f"invalid trace: {exc}", file=sys.stderr)
        return EXIT_INVALID

    result = compare(reference, candidate, args.atol, args.rtol)
    print(format_report(reference, candidate, args.atol, args.rtol, result))
    if result.exit_code == EXIT_INVALID:
        print(f"invalid trace: {result.message}", file=sys.stderr)
    return result.exit_code


if __name__ == "__main__":
    raise SystemExit(main())
