#!/usr/bin/env python3
"""Read, validate and summarise a Brotato RL bridge trace.

Two artifacts share this reader:

  full (default)  one JSON object per completed physics step with the 203-float
                  core observation.  This is the contract consumed by
                  tests/brotato_trace_compare.py: every line carries
                  tick/action/obs/hp/wave/enemies plus x/y, obs is a fixed-length
                  array of finite numbers, and obs_complete is 1.  The width and
                  the action bound are read from ``state_contract.json`` (never a
                  literal here), so the bridge payload and this validator cannot
                  drift from the schema.
  partial         diagnostics only (--partial): a 6-value observation preceded by
                  a bridge_header line, which makes the file invalid for a
                  full-observation validator by construction.

Field-by-field semantics, action semantics and the known divergences are in
``state_contract.json`` next to this file.

Usage::

    parse_trace.py rl_trace.jsonl                      # validate + summary
    parse_trace.py rl_trace.partial.jsonl --partial     # diagnostics artifact
    parse_trace.py rl_trace.jsonl --check               # smoke acceptance gate
    parse_trace.py rl_trace.jsonl --json state.json     # flat arrays for a trainer

``--check`` requires: at least one sample, profile-matching max_hp,
enemies observed, the observation schema and width to match the contract, and - when an
action source is active - at least one step whose injected movement was actually
registered by the game (action_applied == 1).

Exit codes: 0 ok, 1 check failed, 2 usage/parse error.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

EXIT_OK = 0
EXIT_CHECK_FAILED = 1
EXIT_USAGE = 2

DEFAULT_CONTRACT = Path(__file__).resolve().parent / "state_contract.json"

REQUIRED = ("tick", "action", "obs", "hp", "wave", "enemies")
POSITION = ("x", "y")
HEADER_RECORD = "bridge_header"
PARTIAL_CONTRACT = "partial_v0"
FULL_CONTRACT = "full_v5"


class TraceError(Exception):
    """Structural problem in the trace or its contract."""


def _is_int(value) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _is_number(value) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def load_contract(path: Path) -> dict:
    if not path.is_file():
        raise TraceError(f"no state contract at {path}")
    return json.loads(path.read_text())


def contract_obs_length(contract: dict) -> int:
    """Authoritative observation width: explicit size first, then range expansion."""
    explicit = contract.get("observation", {}).get("size")
    if isinstance(explicit, int) and explicit > 0:
        return explicit
    declared = contract.get("record", {}).get("observer_fields", {}).get("obs")
    if isinstance(declared, dict) and declared:
        width = 0
        for key in declared:
            parts = str(key).split("-")
            try:
                if len(parts) == 1:
                    width += 1
                else:
                    low, high = int(parts[0]), int(parts[1])
                    width += max(0, high - low + 1)
            except ValueError:
                continue
        if width:
            return width
    return 0


def contract_action_count(contract: dict) -> int:
    """Authoritative action bound from the contract (0 = unchecked)."""
    action = contract.get("action", {})
    count = action.get("count") if isinstance(action, dict) else None
    if isinstance(count, int) and not isinstance(count, bool) and count > 0:
        return count
    return 0


# character_bull at Danger 5: PlayerRunData.DEFAULT_MAX_HP 10 + bull_effect_1
# stat_max_hp +20.  Used ONLY for the character_bull profile (the recorded boot
# fact is authoritative for any other profile), never as a blanket expectation.
DEFAULT_BULL_MAX_HP = 30


def profile_expected_max_hp(summary_file, meta, override: int = 0):
    """(expected_max_hp, source_label, character) for the acceptance gate.

    The expectation comes from the run's own recorded boot facts
    (run_boot.facts.player_max_hp), so a selectable profile passes on its own
    numbers; character_bull keeps its documented 30 invariant.  An explicit
    --expect-max-hp override wins.
    """
    facts = {}
    if isinstance(summary_file, dict):
        run_boot = summary_file.get("run_boot")
        if isinstance(run_boot, dict):
            facts = run_boot.get("facts") or {}
    if not facts and isinstance(meta, dict):
        facts = meta.get("facts") or {}
    character = str(facts.get("character_found", ""))
    raw = facts.get("player_max_hp")
    recorded = int(raw) if isinstance(raw, int) and not isinstance(raw, bool) else None
    if override:
        return int(override), "CLI --expect-max-hp", character
    if character == "character_bull":
        return DEFAULT_BULL_MAX_HP, "character_bull invariant (10 base + bull_effect_1 +20)", character
    if recorded is not None:
        return recorded, "run_boot.facts.player_max_hp (recorded profile)", character
    return DEFAULT_BULL_MAX_HP, "default (no recorded boot fact; assuming default Bull)", character


def load_trace(path: Path) -> tuple[list[dict], dict | None]:
    """Returns (data records, header or None)."""
    if not path.is_file():
        raise TraceError(f"no such trace: {path}")
    records: list[dict] = []
    header: dict | None = None
    for lineno, raw in enumerate(path.read_text().splitlines(), start=1):
        line = raw.strip()
        if not line:
            continue
        try:
            value = json.loads(line)
        except json.JSONDecodeError as exc:
            raise TraceError(f"{path}:{lineno}: invalid JSON ({exc})") from exc
        if not isinstance(value, dict):
            raise TraceError(f"{path}:{lineno}: record must be a JSON object")
        if value.get("record") == HEADER_RECORD:
            if header is not None or records:
                raise TraceError(f"{path}:{lineno}: header record must be the first line and unique")
            header = value
            continue
        records.append(value)
    if not records:
        raise TraceError(f"{path}: trace has no data records")
    return records, header


def validate_header(header: dict, path: Path) -> None:
    if header.get("contract") != PARTIAL_CONTRACT:
        raise TraceError(
            f"{path}: header contract is {header.get('contract')!r}, expected {PARTIAL_CONTRACT!r}"
        )
    if header.get("obs_complete") != 0:
        raise TraceError(f"{path}: header advertises obs_complete={header.get('obs_complete')!r}, expected 0")
    if not _is_int(header.get("obs_length")) or header["obs_length"] <= 0:
        raise TraceError(f"{path}: header obs_length must be a positive int")


def validate(records: list[dict], path: Path, expected_obs: int, partial: bool,
             action_count: int = 0) -> tuple[int, list]:
    obs_len = 0
    last_tick = None
    incomplete: list = []
    for index, record in enumerate(records, start=1):
        where = f"{path}:{index}"
        expected_contract = PARTIAL_CONTRACT if partial else FULL_CONTRACT
        expected_version = 0 if partial else 5
        if record.get("contract") != expected_contract:
            raise TraceError(
                f"{where}: contract {record.get('contract')!r} != {expected_contract!r}"
            )
        if not _is_int(record.get("obs_version")) or record["obs_version"] != expected_version:
            raise TraceError(
                f"{where}: obs_version {record.get('obs_version')!r} != {expected_version}"
            )
        for name in REQUIRED:
            if name not in record:
                raise TraceError(f"{where}: missing required field {name!r}")
        tick = record["tick"]
        if not _is_int(tick):
            raise TraceError(f"{where}: tick must be an int, got {tick!r}")
        if last_tick is not None and tick <= last_tick:
            raise TraceError(f"{where}: tick {tick} is not greater than the previous tick {last_tick}")
        last_tick = tick

        if not _is_number(record["hp"]):
            raise TraceError(f"{where}: hp must be a number, got {record['hp']!r}")
        action = record["action"]
        if not _is_int(action):
            raise TraceError(f"{where}: action must be an int, got {action!r}")
        # -1 is the documented "no action was ever injected" value; any other
        # value must be inside the contract's action space.
        if action_count and action != -1 and not 0 <= action < action_count:
            raise TraceError(
                f"{where}: action {action} is outside the contract's 0..{action_count - 1} action space"
            )
        for name in ("wave", "enemies"):
            if not _is_int(record[name]):
                raise TraceError(f"{where}: {name} must be an int, got {record[name]!r}")

        obs = record["obs"]
        if not isinstance(obs, list) or not obs:
            raise TraceError(f"{where}: obs must be a non-empty array")
        for position, item in enumerate(obs):
            if not _is_number(item) or not math.isfinite(float(item)):
                raise TraceError(f"{where}: obs[{position}] must be a finite number, got {item!r}")
        if obs_len == 0:
            obs_len = len(obs)
        elif len(obs) != obs_len:
            raise TraceError(f"{where}: obs length {len(obs)} != {obs_len}")

        present = [name for name in POSITION if name in record]
        if present and len(present) != len(POSITION):
            raise TraceError(f"{where}: x and y must both be present or both absent (found {present})")
        for name in present:
            if not _is_number(record[name]) or not math.isfinite(float(record[name])):
                raise TraceError(f"{where}: {name} must be a finite number, got {record[name]!r}")

        complete = record.get("obs_complete")
        if partial:
            if complete not in (0, None):
                raise TraceError(f"{where}: partial trace must carry obs_complete=0, got {complete!r}")
        elif complete not in (0, 1):
            raise TraceError(
                f"{where}: full trace must carry obs_complete=1 (or 0 when live offers could not be "
                f"mapped), got {complete!r}"
            )
        if complete == 0 and not partial:
            incomplete.append((tick, record.get("obs_unmapped", [])))

    if expected_obs and obs_len != expected_obs:
        raise TraceError(f"{path}: observation length {obs_len} does not match the contract's {expected_obs}")
    return obs_len, incomplete


def summarise(records: list[dict], obs_len: int, partial: bool) -> dict:
    def values(name):
        return [record[name] for record in records if name in record]

    hp_values = values("hp")
    enemies_values = values("enemies")
    enemy_nodes_values = values("enemy_nodes")
    applied_values = values("action_applied")
    first_enemy = next(
        (
            record["tick"]
            for record in records
            if record["enemies"] > 0 or record.get("enemy_nodes", 0) > 0
        ),
        None,
    )
    first_applied = next((record["tick"] for record in records if record.get("action_applied") == 1), None)
    return {
        "contract": PARTIAL_CONTRACT if partial else FULL_CONTRACT,
        "partial_contract": partial,
        "rows": len(records),
        "obs_len": obs_len,
        "tick_first": records[0]["tick"],
        "tick_last": records[-1]["tick"],
        "hp_first": hp_values[0],
        "hp_last": hp_values[-1],
        "hp_min": min(hp_values),
        "hp_max": max(hp_values),
        "max_hp_values": sorted({record.get("max_hp", -1) for record in records}),
        "waves_seen": sorted({record["wave"] for record in records}),
        "difficulty_seen": sorted({record.get("difficulty", -1) for record in records}),
        "phases_seen": sorted({record.get("phase", -1) for record in records}),
        "actions_seen": sorted({record["action"] for record in records}),
        "actions_applied": sum(1 for value in applied_values if value == 1),
        "first_action_applied_tick": first_applied,
        "action_sources": sorted({record.get("action_source", "") for record in records}),
        "obs_complete_values": sorted({record.get("obs_complete", 0) for record in records}),
        "partial_rows_by_completeness": sum(1 for record in records if record.get("obs_complete") == 0),
        "policy_ok_values": sorted({record.get("policy_ok", 0) for record in records}),
        "enemies_max": max(enemies_values),
        "enemy_nodes_max": max(enemy_nodes_values) if enemy_nodes_values else 0,
        "first_enemy_tick": first_enemy,
        "scenes_seen": sorted({record.get("scene", "") for record in records}),
    }


def flat_arrays(records: list[dict]) -> dict:
    def column(name):
        return [record.get(name) for record in records]

    return {
        "tick_values": column("tick"),
        "action_values": column("action"),
        "next_action_values": column("next_action"),
        "action_applied_values": column("action_applied"),
        "obs": [list(record["obs"]) for record in records],
        "x_values": column("x"),
        "y_values": column("y"),
        "hp_values": column("hp"),
        "wave_values": column("wave"),
        "enemies_values": column("enemies"),
        "enemy_nodes_values": column("enemy_nodes"),
        "gold_values": column("gold"),
        "level_values": column("level"),
        "weapon_count_values": column("weapon_count"),
        "phase_values": column("phase"),
        "policy_ok_values": column("policy_ok"),
        "obs_version": records[0].get("obs_version", 0),
        "obs_complete": records[0].get("obs_complete", 0),
    }


def load_side_file(path: Path) -> dict:
    if not path.is_file():
        return {}
    try:
        return json.loads(path.read_text())
    except json.JSONDecodeError:
        return {}


def check(summary: dict, expect_max_hp: int, summary_file: dict, expect_obs: int,
          expect_source: str = "", character: str = "") -> list[str]:
    problems = []
    if summary["rows"] < 1:
        problems.append("no samples were written (no live Player node was ever found)")
    if expect_obs and summary["obs_len"] != expect_obs:
        problems.append(f"observation length is {summary['obs_len']}, expected {expect_obs}")
    if expect_max_hp not in summary["max_hp_values"]:
        problems.append(
            f"max_hp values seen {summary['max_hp_values']} never reached {expect_max_hp} "
            f"({expect_source or 'recorded profile'})"
        )
    # Preserve the Bull check: a character_bull run must reach the documented 30,
    # and its own recorded boot fact must agree with that invariant.
    if character == "character_bull":
        recorded = None
        if isinstance(summary_file, dict):
            run_boot = summary_file.get("run_boot")
            if isinstance(run_boot, dict):
                facts = run_boot.get("facts") or {}
                raw = facts.get("player_max_hp")
                if isinstance(raw, int) and not isinstance(raw, bool):
                    recorded = raw
        if recorded is not None and recorded != DEFAULT_BULL_MAX_HP:
            problems.append(
                f"character_bull recorded boot max_hp {recorded} != {DEFAULT_BULL_MAX_HP} "
                "(10 base + bull_effect_1 +20)"
            )
    if summary["first_enemy_tick"] is None:
        problems.append(
            f"no enemy was ever observed (enemies_max={summary['enemies_max']}, "
            f"enemy_nodes_max={summary['enemy_nodes_max']})"
        )
    if summary.get("incomplete_rows"):
        examples = "; ".join(f"tick {tick} unmapped={unmapped}"
                             for tick, unmapped in summary.get("incomplete_examples", []))
        problems.append(
            f"{summary['incomplete_rows']} row(s) are marked obs_complete=0 because live offers could not be "
            f"mapped onto the core tables (their slots carry the core's empty-slot encoding): {examples}"
        )
    active = [source for source in summary["action_sources"] if source not in ("", "none")]
    if active and summary["actions_applied"] < 1:
        problems.append(
            f"no injected movement was registered by the game (action sources {active}); "
            "action_applied never became 1"
        )
    failure = summary_file.get("failure") or ""
    if failure:
        problems.append(f"bridge reported failure {failure!r}: {summary_file.get('failure_message', '')}")
    return problems


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("trace")
    parser.add_argument("--contract", default=str(DEFAULT_CONTRACT))
    parser.add_argument("--json", default="", help="also write the flat arrays to this file")
    parser.add_argument("--check", action="store_true", help="apply the smoke acceptance invariants")
    parser.add_argument("--partial", action="store_true",
                        help="validate the partial diagnostics artifact (bridge_header, obs_complete=0)")
    parser.add_argument("--expect-max-hp", type=int, default=0,
                        help="expected max_hp (0 = derive from the run's recorded boot facts; "
                             "character_bull keeps its 30 invariant)")
    parser.add_argument("--expect-obs", type=int, default=0,
                        help="expected observation length (default: the contract's declared width)")
    return parser


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    trace_path = Path(args.trace)
    try:
        contract = load_contract(Path(args.contract))
        declared_obs = contract_obs_length(contract)
        expect_obs = args.expect_obs or (0 if args.partial else declared_obs)
        action_count = contract_action_count(contract)
        records, header = load_trace(trace_path)
        if args.partial:
            if header is None:
                raise TraceError(
                    f"{trace_path}: expected a {PARTIAL_CONTRACT} artifact (bridge_header first line); "
                    "drop --partial to validate a full-observation trace"
                )
            validate_header(header, trace_path)
        elif header is not None:
            raise TraceError(
                f"{trace_path}: this is a {PARTIAL_CONTRACT} diagnostics artifact; run with --partial"
            )
        obs_len, incomplete_rows = validate(records, trace_path, expect_obs, args.partial, action_count)
        summary = summarise(records, obs_len, args.partial)
        summary["action_count"] = action_count
        summary["incomplete_rows"] = len(incomplete_rows)
        summary["incomplete_examples"] = [list(item) for item in incomplete_rows[:5]]
    except TraceError as exc:
        print(f"trace error: {exc}", file=sys.stderr)
        return EXIT_USAGE

    meta = load_side_file(Path(str(trace_path) + ".meta.json"))
    summary_file = load_side_file(Path(str(trace_path) + ".summary.json"))

    # Profile-aware acceptance: the expected max_hp comes from the run's own
    # recorded boot facts, so a selectable profile (Knight/Demon) passes on its
    # own numbers while character_bull keeps its documented 30 invariant.
    expect_max_hp, expect_source, character = profile_expected_max_hp(
        summary_file, meta, args.expect_max_hp)
    summary["expected_max_hp"] = expect_max_hp
    summary["expected_max_hp_source"] = expect_source
    summary["profile_character"] = character

    print(f"trace  : {trace_path}")
    if header is not None:
        print(f"  header              contract={header.get('contract')} obs_complete={header.get('obs_complete')}")
    for key in (
        "contract", "rows", "obs_len", "action_count", "tick_first", "tick_last", "hp_first", "hp_last", "hp_min",
        "max_hp_values", "expected_max_hp", "profile_character", "waves_seen", "difficulty_seen", "phases_seen", "actions_seen",
        "actions_applied", "first_action_applied_tick", "action_sources", "policy_ok_values",
        "enemies_max", "enemy_nodes_max", "first_enemy_tick", "scenes_seen", "obs_complete_values", "incomplete_rows"
    ):
        print(f"  {key:20s} {summary[key]}")
    if meta:
        print(f"  platform            {meta.get('facts', {}).get('platform_type', '?')}")
        print(f"  active dlc          {meta.get('facts', {}).get('active_dlc_ids', [])}")
        print(f"  run boot ok         {meta.get('run_boot', {}).get('ok')}")
        print(f"  settings repairs    {meta.get('settings', {}).get('repair_count')}")
        scale = (meta.get("obs") or {}).get("scale") or {}
        if scale:
            print(f"  obs scale           arena {scale.get('arena_w')}x{scale.get('arena_h')} "
                  f"max_enemies={scale.get('max_enemies')} waves={scale.get('waves')} "
                  f"legacy={1 if scale.get('legacy_train_scale') else 0}")
        revision = (meta.get("obs") or {}).get("core_revision") or {}
        if revision:
            hashes = {k: str(v)[:12] for k, v in revision.items() if k.endswith("_sha256")}
            print(f"  core revision       {hashes}")
        action_report = (summary_file or {}).get("action") or meta.get("action", {}).get("injection", {})
        print(f"  native input        applications={action_report.get('input_applications')} "
              f"verified={action_report.get('applied_total')} mismatches={action_report.get('verify_mismatches')}")
        policy_report = (summary_file or {}).get("policy") or meta.get("policy")
        if policy_report:
            print(f"  policy              loaded={policy_report.get('loaded')} "
                  f"obs_size={policy_report.get('obs_size')} steps_ok={policy_report.get('step_ok')}")
    if summary_file:
        print(f"  bridge ok           {summary_file.get('ok')} ({summary_file.get('reason')})")

    if args.json:
        payload = {"summary": summary, "arrays": flat_arrays(records)}
        if header is not None:
            payload["header"] = header
        if meta:
            payload["meta"] = meta
        if summary_file:
            payload["bridge_summary"] = summary_file
        Path(args.json).write_text(json.dumps(payload))
        print(f"  arrays written to   {args.json}")

    if args.check:
        problems = check(summary, expect_max_hp, summary_file, expect_obs, expect_source, character)
        if problems:
            print("CHECK FAILED")
            for problem in problems:
                print(f"  - {problem}")
            return EXIT_CHECK_FAILED
        print(
            "CHECK OK: %s trace, obs=%d, max_hp==%d (%s, character %s), first enemy at tick %s, "
            "movement registered on %d step(s)"
            % (summary["contract"], summary["obs_len"], expect_max_hp,
               expect_source, character or "<unknown>",
               summary["first_enemy_tick"], summary["actions_applied"])
        )
    return EXIT_OK


if __name__ == "__main__":
    raise SystemExit(main())
