"""Opt-in tests against the actual retail-bytecode bridge, not mock movement.

Run after refreshing the bridge manifest and observation tables:
BROTATO_RUN_LIVE_TESTS=1 python -m pytest -q tests/test_brotato_live_bridge.py
BROTATO_TEST_RUN_ROOT defaults to ~/brotato_run (retail pack and companion files).
"""
import json
import math
import os
from pathlib import Path
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[1]
pytestmark = pytest.mark.skipif(
    os.environ.get("BROTATO_RUN_LIVE_TESTS") != "1",
    reason="requires the retail pack, compatible Godot engine and GDRE",
)


def run_game(tmp_path, tag, _timeout=75, **overrides):
    source = Path(os.environ.get("BROTATO_TEST_RUN_ROOT", Path.home() / "brotato_run"))
    run = tmp_path / tag
    run.mkdir()
    (run / "Brotato.pck").symlink_to((source / "Brotato.pck").resolve())
    env = {key: value for key, value in os.environ.items() if not key.startswith("BROTATO_")}
    for key in ("BROTATO_ENGINE", "BROTATO_GDRE", "BROTATO_EXTERNAL_ROOT"):
        if key in os.environ:
            env[key] = os.environ[key]
    env.update({
        "BROTATO_RUN_DIR": str(run),
        "BROTATO_DLC_SRC": str(source / "BrotatoAbyssalTerrors.pck"),
        "BROTATO_STEAM_DATA_SRC": str(source / "steam_data.json"),
        "BROTATO_ISO_HOME": str(run / "home"),
        "BROTATO_TAG": tag,
        "BROTATO_MAX_SECONDS": "8",
        "BROTATO_WATCHDOG": "45",
        "BROTATO_RL_SEED": "1",
        "BROTATO_RL_SAMPLE_EVERY": "1",
        "BROTATO_ACTION_SOURCE": "script",
        "BROTATO_POLICY": "0",
        "BROTATO_RL_SCRIPT_ACTIONS": "1,2,3,4,5,6,7,8,0",
        "BROTATO_RL_SCRIPT_HOLD_TICKS": "1",
    })
    env.update(overrides)
    result = subprocess.run(
        ["bash", "ocean/brotato/bridge/run_game.sh"],
        cwd=ROOT, env=env, text=True, capture_output=True, timeout=_timeout,
    )
    return result, run / "rl_out" / f"rl_trace.{tag}.jsonl"


def test_completed_action_explains_the_live_position_transition(tmp_path):
    result, trace = run_game(tmp_path, "alignment")
    assert result.returncode == 0, result.stdout + result.stderr
    rows = [json.loads(line) for line in trace.read_text().splitlines()]
    assert [row["tick"] for row in rows[:20]] == list(range(20))
    diagonal = 1 / math.sqrt(2)
    directions = [(0, 0), (0, -1), (diagonal, -diagonal), (1, 0),
                  (diagonal, diagonal), (0, 1), (-diagonal, diagonal),
                  (-1, 0), (-diagonal, -diagonal)]
    # Initial centre position; no enemy, wall, shop or level-up can interrupt
    # these transitions. Changing direction every tick exposes a stale snapshot.
    assert rows[0]["action"] == 0
    for previous, current in zip(rows[:19], rows[1:20]):
        assert current["tick"] == previous["tick"] + 1
        assert current["action"] == previous["next_action"]
        dx, dy = directions[current["action"]]
        assert current["x"] - previous["x"] == pytest.approx(dx * 450 / 60, abs=2e-4)
        assert current["y"] - previous["y"] == pytest.approx(dy * 450 / 60, abs=2e-4)


def test_policy_mode_cannot_substitute_script_when_harness_disabled(tmp_path):
    result, trace = run_game(tmp_path, "policy_disabled", BROTATO_ACTION_SOURCE="policy")
    assert result.returncode != 0, result.stdout + result.stderr
    summary = json.loads(Path(str(trace) + ".summary.json").read_text())
    assert summary["failure"] == "policy_load_failed"
    assert summary["exit_code"] == 1
    assert summary["samples"] == 0


def test_demon_emits_current_schema_and_starting_weapon(tmp_path):
    result, trace = run_game(
        tmp_path, "demon_schema5", BROTATO_CHAR="character_demon",
        BROTATO_WEAPON="weapon_ghost_scepter_1", BROTATO_ENDLESS="1",
    )
    assert result.returncode == 0, result.stdout + result.stderr
    rows = [json.loads(line) for line in trace.read_text().splitlines()]
    for row in rows:
        assert row["contract"] == "full_v5"
        assert row["obs_version"] == 5
        assert len(row["obs"]) == 203, (row["tick"], len(row["obs"]))
        assert row["weapon_count"] == 1, row["tick"]
        assert any(row["obs"][79:87]), row["tick"]
        assert row["obs"][87:127] == [0.0] * 40, row["tick"]
        # Changing the simulator's storage capacity must not rescale policy input.
        assert row["obs"][67] == pytest.approx(row["enemy_nodes"] / 100.0), row["tick"]
    # Typed shop offers are empty outside the shop phase.
    for row in rows:
        if row.get("phase", 0) == 0:
            assert row["obs"][127:139] == [0.0] * 12, row["tick"]
            assert row["obs"][141:161] == [0.0] * 20, row["tick"]
    # The v5 tail: no level-up UI exists outside phase 2, so the four presence
    # flags are 0 in combat/shop.  The ban budget is remaining tokens / 8 (0 when
    # the ban UI is off; the RL defaults force it on).
    for row in rows:
        if row.get("phase", 0) in (0, 1):
            assert row["obs"][199:203] == [0.0] * 4, row["tick"]
        assert 0.0 <= row["obs"][198] <= 1.0, row["tick"]


def test_unknown_starting_weapon_fails_closed_before_the_run(tmp_path):
    result, trace = run_game(tmp_path, "badweapon",
                             BROTATO_RL_WEAPON="weapon_this_does_not_exist")
    assert result.returncode != 0, result.stdout + result.stderr
    summary = json.loads(Path(str(trace) + ".summary.json").read_text())
    assert summary["samples"] == 0
    assert summary["run_boot"]["error"] == "starting_weapon_not_found"


def _nonzero_weapon_slots(row):
    return sum(
        1
        for slot in range(6)
        if any(row["obs"][79 + 8 * slot + field] != 0.0 for field in range(8))
    )


def test_shop_recycle_is_reflected_in_the_next_row_inventory(tmp_path):
    # Engine regression for the live owned inventory: a shop recycle/combine must
    # change the NEXT row's weapon block. Caching the arena block across the shop
    # hid exactly this. Knight with an explicit starting weapon gives a weapon to
    # recycle deterministically (Bull has no weapon slots).
    result, trace = run_game(
        tmp_path, "knight_recycle",
        _timeout=150,
        BROTATO_CHAR="character_knight",
        BROTATO_RL_WEAPON="weapon_hatchet_2",
        BROTATO_RL_SCRIPT_SHOP_ACTIONS="9,9,9,9,9",
        BROTATO_RL_SCRIPT_HOLD_TICKS="1",
        BROTATO_RL_INVULNERABLE="1",  # isolate the shop transition from combat survival
        BROTATO_MAX_SECONDS="30",
        BROTATO_WATCHDOG="120",
    )
    assert result.returncode == 0, result.stdout + result.stderr
    rows = [json.loads(line) for line in trace.read_text().splitlines()]
    assert any(row.get("phase") == 1 for row in rows), "the run never reached the shop"
    applied = [i for i, row in enumerate(rows)
               if 9 <= row["action"] <= 20 and row.get("action_applied") == 1]
    assert applied, "no owned-weapon shop action (recycle/combine) was applied"
    index = applied[0]
    assert index + 1 < len(rows), "the applied action was the last row; transition unobservable"
    before, after = rows[index], rows[index + 1]
    assert after.get("phase") == 1, "the next row left the shop; the transition is unobservable"
    # A recycle (9-14) and a combine (15-20) each remove exactly one owned weapon,
    # and the next pre-action row must show it (the cached-block bug did not).
    assert _nonzero_weapon_slots(after) == _nonzero_weapon_slots(before) - 1, (
        before["action"], before["obs"][79:127], after["obs"][79:127])
    assert after["obs"][79:127] != before["obs"][79:127]


def test_live_shop_price_predicts_purchase_and_offer_removal(tmp_path):
    result, trace = run_game(
        tmp_path, "shop_prices", _timeout=150,
        BROTATO_RL_SCRIPT_SHOP_ACTIONS="0,1,2,3,8",
        BROTATO_RL_SCRIPT_HOLD_TICKS="1",
        BROTATO_RL_INVULNERABLE="1",
        BROTATO_MAX_SECONDS="25",
        BROTATO_WATCHDOG="120",
    )
    assert result.returncode == 0, result.stdout + result.stderr
    rows = [json.loads(line) for line in trace.read_text().splitlines()]
    purchases = [i for i, row in enumerate(rows[:-1])
                 if row.get("phase") == 1 and 0 <= row["action"] <= 3
                 and row.get("action_applied") == 1 and rows[i + 1].get("phase") == 1]
    assert purchases, "no observable live shop purchase"
    before, after = rows[purchases[0]], rows[purchases[0] + 1]
    slot = before["action"]
    base = 142 + 4 * slot
    price, eligible, affordable, buyable = before["obs"][base:base + 4]
    assert eligible == affordable == buyable == 1.0
    assert price > 0
    assert before["obs"][8] * 100 - after["obs"][8] * 100 == pytest.approx(price, abs=1e-3)
    assert after["obs"][base:base + 4] == [0.0] * 4


def test_durable_lock_flag_survives_the_go_transition(tmp_path):
    """obs[165..168] are DURABLE RunData lock flags in every phase.

    `rl_obs.gd` emits them from `RunData.get_player_locked_shop_items(0)`, the
    list `lock_player_shop_item` grows (run_data.gd:456-468) and only the NEXT
    shop entry clears (base_shop.gd:110-120).  A slot with a live offer is locked
    when that offer matches an entry by my_id (base_shop.gd:277-289); a slot with
    NO live offer keeps the entry's own flag, because the GO frame has already
    swapped the scene (and advanced current_wave) while RunData still holds the
    lock -- the core's durable `shop_locked[]` read 1 there and the observation
    read 0 (v5_demon_s3 row 1854).  Captures recorded before that fallback carry
    the old 0 on those rows, so only a recapture can confirm it: this test IS
    that recapture.
    """
    result, trace = run_game(
        tmp_path, "lock_go", _timeout=200,
        BROTATO_RL_SCRIPT_SHOP_ACTIONS="21,8",   # lock offer 0, then GO
        BROTATO_RL_SCRIPT_HOLD_TICKS="1",
        BROTATO_RL_INVULNERABLE="1",
        BROTATO_MAX_SECONDS="40",
        BROTATO_WATCHDOG="180",
    )
    assert result.returncode == 0, result.stdout + result.stderr
    rows = [json.loads(line) for line in trace.read_text().splitlines()]
    shops = [i for i, row in enumerate(rows) if row.get("phase") == 1]
    assert shops, "the run never reached the shop"
    locked_rows = [i for i, row in enumerate(rows) if row["obs"][165] == 1.0]
    assert locked_rows, "the lock action never registered a locked offer"
    # every row that reports the lock is inside the shop or in the arena right
    # after it (the durable list is cleared only when the next shop opens)
    assert max(locked_rows) < len(rows) - 1
    # the first arena row after the shop still reports the durable lock, and it
    # is exactly the row the GO frame's scene swap produced
    first_after = max(shops) + 1
    assert rows[first_after].get("phase") == 0
    assert rows[first_after]["obs"][165] == 1.0, (
        "the arena row after the GO dropped the durable lock flag "
        "(the pre-fix observer only matched live offers and reported 0)")
