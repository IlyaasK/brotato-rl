"""Pinned-Godot behavioral oracles for the recovered Brotato player weapons.

Two layers run against the *recovered* Brotato project on the pinned Godot
build:

1. ``tests/brotato_weapon_oracle_project/weapon_oracle.gd`` (default mode)
   instantiates the real recovered weapon / hitbox / projectile scenes and the
   real ``MeleeWeaponShootingBehavior`` / ``PlayerProjectile`` scripts, drives
   the engine at a fixed 60 Hz, and records what the engine actually does as
   JSONL.  The assertions below pin the consumer-visible melee hit-window /
   return / repeat-target and projectile penetration / bounce / event-ordering
   boundaries.  The oracle is the real engine driving the real game scripts, so
   these are characterisation boundaries, not a restatement of the fixture.

2. The same script in ``BROTATO_WEAPON_ORACLE_MODE=catalogue`` mode reads EVERY
   weapon data resource in the recovered tree and emits, per weapon tier, the
   base ``WeaponStats`` fields, the scene hitbox/sprite/muzzle/attach geometry,
   the effect list, the projectile-scene geometry, and the effective stats the
   game resolves under two controlled player stat blocks (weapon_service.gd's
   ``init_melee_stats`` / ``init_ranged_stats``).  ``weapon_parity_dump.cpp``
   (compiled on demand into a temp dir) emits the equivalent values straight
   from the sim's generated tables and its own resolver.  ``test_weapon_parity_
   matrix`` compares them and writes ``tests/weapon_parity_report.md`` (PASS/FAIL
   per weapon with the first mismatching quantity and the code owner to route a
   sim bug to).

When the pinned engine or the recovered tree is absent the tests skip: they
never invent a stand-in for the game's own behaviour.

Overrides: ``BROTATO_GODOT``, ``BROTATO_RECOVERED``, ``CXX``.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
ORACLE_SCRIPT = ROOT / "tests" / "brotato_weapon_oracle_project" / "weapon_oracle.gd"
DUMP_SRC = ROOT / "tests" / "brotato_weapon_oracle_project" / "weapon_parity_dump.cpp"
REPORT = ROOT / "tests" / "weapon_parity_report.md"

# Linux pin (this host).  The previous macOS paths are kept only as fallbacks so
# the same test still runs on the machine the fixture was authored on.
DEFAULT_GODOT_CANDIDATES = [
    Path.home() / "tools" / "bin" / "godot-3.7-dev1",
    Path("/Users/leelachebiyyam/tools/Godot.app/Contents/MacOS/Godot"),
]
DEFAULT_RECOVERED = ROOT / "migration" / "game" / "brotato_recovered"

FLOAT_ATOL = 1e-3
# weapon_service.gd drops attack_type / deal_dmg_on_return when it rebuilds the
# effective MeleeWeaponStats (init_base_stats does not copy them); the runtime
# reads those two off the source `stats` resource, so the sim keeps them in the
# resolved copy.  They are compared from the BASE resource, never from the
# effective block.
EFFECTIVE_SKIP = {"attack_type", "deal_dmg_on_return"}


def _godot_binary() -> Path:
    override = os.environ.get("BROTATO_GODOT")
    if override:
        return Path(override)
    for candidate in DEFAULT_GODOT_CANDIDATES:
        if candidate.exists():
            return candidate
    return DEFAULT_GODOT_CANDIDATES[0]


def _recovered_root() -> Path:
    return Path(os.environ.get("BROTATO_RECOVERED", str(DEFAULT_RECOVERED)))


def _godot_argv() -> list[str]:
    """The Godot launch prefix, wrapped in a virtual X server when headless.

    The pinned build is an X11 build; on a machine with no ``DISPLAY`` it aborts
    with "X11 Display is not available".  ``xvfb-run`` supplies one.  With a
    real display (macOS or a Linux desktop session) the binary runs directly.
    """
    godot = _godot_binary()
    import sys

    if not os.environ.get("DISPLAY") and sys.platform == "linux" and shutil.which("xvfb-run"):
        return ["xvfb-run", "-a", "-s", "-screen 0 1280x720x24", str(godot)]
    return [str(godot)]


def _require_environment() -> None:
    godot = _godot_binary()
    if not godot.exists():
        pytest.skip(f"pinned Godot binary not found: {godot}")
    recovered = _recovered_root()
    if not (recovered / "project.godot").exists():
        pytest.skip(f"recovered Brotato project not found: {recovered}")
    if not ORACLE_SCRIPT.exists():
        pytest.fail(f"oracle fixture missing: {ORACLE_SCRIPT}")


def _case(rows: list[dict], case: str, event: str) -> list[dict]:
    matched = [r for r in rows if r.get("case") == case and r.get("event") == event]
    if not matched:
        pytest.fail(f"oracle trace has no {case}/{event} row")
    return matched


def _one(rows: list[dict], case: str, event: str) -> dict:
    matched = _case(rows, case, event)
    assert len(matched) == 1, f"expected exactly one {case}/{event}, got {len(matched)}"
    return matched[0]


@pytest.fixture(scope="session")
def oracle_rows(tmp_path_factory: pytest.TempPathFactory) -> list[dict]:
    """One real simulated run of the recovered weapons on the pinned engine."""
    _require_environment()
    trace = tmp_path_factory.mktemp("weapon-oracle") / "oracle.jsonl"
    env = dict(os.environ, BROTATO_WEAPON_ORACLE_TRACE=str(trace))
    process = subprocess.run(
        _godot_argv()
        + [
            "--path", str(_recovered_root()),
            "--no-window", "--disable-audio",
            "--fixed-fps", "60",
            "--script", str(ORACLE_SCRIPT),
        ],
        env=env, capture_output=True, text=True, timeout=1800,
    )
    if process.returncode != 0:
        pytest.fail(
            "weapon oracle run failed with code "
            f"{process.returncode}\nstderr tail:\n{process.stderr[-2000:]}"
        )
    assert trace.exists(), "oracle produced no trace"
    rows = [json.loads(line) for line in trace.read_text().splitlines() if line.strip()]
    if len(rows) < 2 or "metadata" not in rows[0]:
        pytest.fail("oracle trace is missing its metadata header")
    return rows


# ---------------------------------------------------------------------------
# engine provenance
# ---------------------------------------------------------------------------


def test_oracle_runs_on_recovered_engine(oracle_rows):
    engine = oracle_rows[0]["metadata"]["engine"]
    assert (engine["major"], engine["minor"]) == (3, 7)
    assert oracle_rows[0]["metadata"]["fixed_fps"] == 60


# ---------------------------------------------------------------------------
# melee: hit window (when the hitbox is live over the attack timeline)
# ---------------------------------------------------------------------------


def test_melee_thrust_hit_window_is_bounded(oracle_rows):
    ready = _one(oracle_rows, "melee_thrust_hit_window", "ready")
    assert ready["max_range"] == 150
    assert ready["hitbox_disabled"] is True  # idle weapon cannot hit

    window = _case(oracle_rows, "melee_thrust_hit_window", "hit_window")
    assert [w["disabled"] for w in window] == [False, True]
    # Sampled on idle frames; the physics-frame counter used by the
    # return-window cases reports the same window as 8..24.
    enabled_at, disabled_at = window[0]["frame"], window[1]["frame"]
    assert enabled_at < disabled_at
    assert window[0]["frame"] == 8
    assert window[1]["frame"] == 25

    summary = _one(oracle_rows, "melee_thrust_hit_window", "summary")
    assert summary["attack_frames"] == 39
    # The hitbox is off again for the whole return to the idle pose.
    assert summary["hitbox_disabled"] is True
    # Thrust reach is the weapon's max_range past the sprite's resting offset
    # (claw Sprite.position.x == 16): 16 + 150.
    assert summary["sprite_x"] == pytest.approx([-9.0, 166.0], abs=FLOAT_ATOL)
    assert summary["sprite_y"] == pytest.approx([0.0, 0.0], abs=FLOAT_ATOL)


def test_melee_sweep_hit_window_and_arc(oracle_rows):
    ready = _one(oracle_rows, "melee_sweep_hit_window", "ready")
    assert ready["max_range"] == 135

    window = _case(oracle_rows, "melee_sweep_hit_window", "hit_window")
    assert [w["disabled"] for w in window] == [False, True]
    assert window[0]["frame"] == 8
    assert window[1]["frame"] == 26

    summary = _one(oracle_rows, "melee_sweep_hit_window", "summary")
    assert summary["attack_frames"] == 40
    assert summary["hitbox_disabled"] is True
    # Sweep reach is 0.75 * atk_distance along x; the sweep half-width is
    # atk_distance / 2 on y; the arc is +/- 0.9 * PI.
    assert summary["sprite_x"] == pytest.approx([-25.0, 101.25], abs=FLOAT_ATOL)
    assert summary["sprite_y"] == pytest.approx([-67.5, 67.5], abs=FLOAT_ATOL)
    assert summary["rot"] == pytest.approx([-0.9 * 3.141592653589793, 0.9 * 3.141592653589793], abs=FLOAT_ATOL)


def test_melee_hitbox_footprint(oracle_rows):
    # The resting CollisionShape2D footprint in the weapon-root frame, i.e.
    # Sprite.pos + Hitbox.pos + Collision.pos and the half-extents.
    claw = _one(oracle_rows, "melee_thrust_hit_window", "ready")["hit_geom"]
    assert claw["kind"] == "rect"
    assert claw["half"] == pytest.approx([60.0, 20.0])
    assert claw["center"] == pytest.approx([12.0, 0.0])

    chopper = _one(oracle_rows, "melee_sweep_hit_window", "ready")["hit_geom"]
    assert chopper["half"] == pytest.approx([72.0, 16.0])
    assert chopper["center"] == pytest.approx([8.0, 5.0])

    sword = _one(oracle_rows, "melee_alternate_attack_type", "ready")["hit_geom"]
    assert sword["half"] == pytest.approx([112.0, 32.0])
    assert sword["center"] == pytest.approx([-8.0, 0.0])


def test_melee_alternate_attack_type_toggles(oracle_rows):
    attacks = _case(oracle_rows, "melee_alternate_attack_type", "attack")
    assert len(attacks) == 2
    # THRUST (0) then SWEEP (1), flipping back to THRUST after the second shot.
    assert attacks[0]["next_attack_type_before"] == 0
    assert attacks[0]["next_attack_type_after"] == 1
    assert attacks[1]["next_attack_type_before"] == 1
    assert attacks[1]["next_attack_type_after"] == 0


def test_melee_alternate_attack_geometry_differs(oracle_rows):
    thrust, sweep = _case(oracle_rows, "melee_alternate_attack_type", "attack")
    # Thrust travels straight along +x and reaches the full max_range; sword
    # Sprite.position.x == 24, sword_2 max_range == 200.
    assert thrust["sprite_y"] == pytest.approx([0.0, 0.0], abs=FLOAT_ATOL)
    assert thrust["rot"] == pytest.approx([0.0, 0.0], abs=FLOAT_ATOL)
    assert thrust["sprite_x"][1] == pytest.approx(24.0 + 200.0, abs=FLOAT_ATOL)
    # Sweep reaches 0.75 * atk_distance and covers the +/- half-width on y.
    assert sweep["sprite_y"] == pytest.approx([-100.0, 100.0], abs=FLOAT_ATOL)
    assert sweep["sprite_x"][1] == pytest.approx(24.0 + 200.0 * 0.75, abs=FLOAT_ATOL)
    assert sweep["rot"] == pytest.approx([-0.9 * 3.141592653589793, 0.9 * 3.141592653589793], abs=FLOAT_ATOL)


def test_melee_alternate_hit_windows_are_per_attack(oracle_rows):
    windows = _case(oracle_rows, "melee_alternate_attack_type", "hit_window")
    by_attack = {}
    for row in windows:
        by_attack.setdefault(row["attack"], []).append(row)
    # Each attack: one enable and one disable, in that order.
    assert [w["disabled"] for w in by_attack[0]] == [False, True]
    assert [w["disabled"] for w in by_attack[1]] == [False, True]
    assert by_attack[0][0]["frame"] < by_attack[0][1]["frame"]
    assert by_attack[1][0]["frame"] < by_attack[1][1]["frame"]


# ---------------------------------------------------------------------------
# melee: return-phase window and per-swing target de-duplication
# ---------------------------------------------------------------------------


def test_melee_return_off_window_closes_before_return(oracle_rows):
    row = _one(oracle_rows, "melee_return_off", "target_hits")
    assert row["deal_dmg_on_return"] is False
    # The hitbox is switched off before the return tween, so the target is
    # overlapped once and damaged once.
    assert row["overlap_enters"] == 1
    assert row["overlap_exits"] == 1
    assert row["damage_events"] == 1
    assert row["hitbox_enabled_frames"] == [8, 24]


def test_melee_return_on_window_stays_open_through_return(oracle_rows):
    row = _one(oracle_rows, "melee_return_on", "target_hits")
    assert row["deal_dmg_on_return"] is True
    # The hitbox stays live through the return tween and re-covers the target
    # (two overlap entries), for a longer live window than the return-off case.
    assert row["overlap_enters"] == 2
    assert row["overlap_exits"] == 2
    assert row["hitbox_enabled_frames"] == [8, 38]
    # ...but a target already hit this swing is de-duplicated by the weapon's
    # ignored_objects, so the overlap does NOT become a second damage event.
    assert row["damage_events"] == 1


def test_melee_repeat_target_is_damaged_once_per_swing(oracle_rows):
    off = _one(oracle_rows, "melee_return_off", "target_hits")
    on = _one(oracle_rows, "melee_return_on", "target_hits")
    assert off["attack_frames"] == on["attack_frames"]
    # deal_dmg_on_return only extends the live hitbox window; it never lets one
    # swing damage the same enemy twice.
    assert on["hitbox_enabled_frames"][1] > off["hitbox_enabled_frames"][1]
    assert off["damage_events"] == 1
    assert on["damage_events"] == 1
    assert on["overlap_enters"] > off["overlap_enters"]


# ---------------------------------------------------------------------------
# projectile: penetration (pierce), damage reduction, stop
# ---------------------------------------------------------------------------


def test_projectile_pierce_spends_pierce_and_reduces_damage(oracle_rows):
    setup = _one(oracle_rows, "projectile_pierce", "setup")
    assert setup["piercing"] == 2
    assert setup["bounce"] == 0
    assert setup["damage"] == 3
    assert setup["piercing_dmg_reduction"] == pytest.approx(0.3, abs=FLOAT_ATOL)

    hits = _case(oracle_rows, "projectile_pierce", "hit")
    assert len(hits) == 3  # two pierces then a stop

    first = hits[0]
    assert first["before"]["piercing"] == 2
    assert first["before"]["damage"] == 3
    # One pierce is spent; the reduced damage is truncated to an int like the
    # source: max(1, 3 - 3 * 0.3) == 2.1 -> 2.
    assert first["after"]["piercing"] == 1
    assert first["after"]["damage"] == 2
    assert first["after"]["ignored"] == 1
    assert first["after"]["active"] is True  # still flying through

    second = hits[1]
    assert second["before"]["piercing"] == 1
    assert second["before"]["damage"] == 2
    # max(1, 2 - 2 * 0.3) == 1.4 -> 1.
    assert second["after"]["piercing"] == 0
    assert second["after"]["damage"] == 1
    assert second["after"]["ignored"] == 2
    assert second["after"]["active"] is True

    third = hits[2]
    assert third["before"]["piercing"] == 0
    # No pierce left: the projectile stops and drops its ignore list.
    assert third["after"]["active"] is False
    assert third["after"]["ignored"] == 0
    assert third["after"]["damage"] == 1  # unchanged once stopped


def test_projectile_ignores_already_hit_targets(oracle_rows):
    hits = _case(oracle_rows, "projectile_pierce", "hit")
    # Each hit adds its target to the ignore list and the next hit sees it.
    assert hits[0]["after"]["ignored"] == 1
    assert hits[1]["before"]["ignored"] == 1
    assert hits[1]["after"]["ignored"] == 2
    assert hits[2]["before"]["ignored"] == 2


# ---------------------------------------------------------------------------
# projectile: bounce (retarget, infinite range, reduction)
# ---------------------------------------------------------------------------


def test_projectile_bounce_retargets_and_extends_range(oracle_rows):
    setup = _one(oracle_rows, "projectile_bounce", "setup")
    assert setup["bounce"] == 1
    assert setup["piercing"] == 0
    assert setup["damage"] == 10
    assert setup["bounce_dmg_reduction"] == pytest.approx(0.0, abs=FLOAT_ATOL)
    assert setup["velocity"] == pytest.approx([2000.0, 0.0], abs=FLOAT_ATOL)

    hits = _case(oracle_rows, "projectile_bounce", "hit")
    first = hits[0]
    assert first["before"]["bounce"] == 1
    assert first["after"]["bounce"] == 0
    # The bounce re-aims at the spawner's target (0, -100) => straight up.
    assert first["after"]["velocity"] == pytest.approx([0.0, -2000.0], abs=0.5)
    assert first["after"]["max_range"] == 10000  # INFINITE_RANGE after a bounce
    assert first["after"]["ignored"] == 1
    assert first["after"]["active"] is True
    # bounce_dmg_reduction == 0 here, so the damage is unchanged.
    assert first["after"]["damage"] == 10

    second = hits[1]
    assert second["before"]["bounce"] == 0
    assert second["after"]["active"] is False
    assert second["after"]["ignored"] == 0


# ---------------------------------------------------------------------------
# projectile: event ordering (hit signal fires before state is consumed)
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("case", ["projectile_pierce", "projectile_bounce"])
def test_projectile_hit_event_precedes_state_consume(oracle_rows, case):
    hits = _case(oracle_rows, case, "hit")
    first = hits[0]
    seen = first["signal_seen"]
    assert len(seen) == 1, "hit_something must be observable on the first hit"
    # Inside the signal the projectile has NOT yet registered the hit: the
    # target is not on the ignore list and pierce/bounce are not yet spent.
    assert seen[0]["ignored"] == first["before"]["ignored"]
    assert seen[0]["piercing"] == first["before"]["piercing"]
    assert seen[0]["bounce"] == first["before"]["bounce"]
    assert seen[0]["active"] is True
    # The damage announced to the listener is the projectile's damage *before*
    # this hit is processed.
    assert seen[0]["damage_dealt"] == first["before"]["damage"]
    # Only after the signal returns does the projectile consume the hit.
    assert first["after"]["ignored"] == first["before"]["ignored"] + 1


def test_projectile_second_hit_announces_reduced_damage(oracle_rows):
    hits = _case(oracle_rows, "projectile_pierce", "hit")
    second = hits[1]
    assert second["signal_seen"][0]["damage_dealt"] == 2
    assert second["signal_seen"][0]["damage"] == 2

# ===========================================================================
# full weapon parity matrix: sim (tests/.../weapon_parity_dump.cpp) vs the
# real engine catalogue (weapon_oracle.gd, MODE=catalogue)
# ===========================================================================

BASE_KEYS = [
    "cooldown", "damage", "accuracy", "crit_chance", "crit_damage",
    "min_range", "max_range", "knockback", "knockback_piercing",
    "effect_scale", "lifesteal", "recoil", "recoil_duration",
    "additional_cooldown_every_x_shots", "additional_cooldown_multiplier",
    "speed_percent_modifier", "nb_projectiles", "projectile_spread",
    "piercing", "piercing_dmg_reduction", "bounce", "bounce_dmg_reduction",
    "projectile_speed", "is_healing", "can_bounce",
    "increase_projectile_speed_with_range", "can_have_positive_knockback",
    "can_have_negative_knockback", "deal_dmg_on_return", "attack_type",
    "alternate_attack_type",
]
EFF_KEYS = [
    "damage", "cooldown", "max_range", "min_range", "crit_chance",
    "crit_damage", "knockback", "lifesteal", "accuracy", "nb_projectiles",
    "projectile_spread", "piercing", "piercing_dmg_reduction", "bounce",
    "bounce_dmg_reduction", "projectile_speed", "recoil", "recoil_duration",
    "speed_percent_modifier", "alternate_attack_type",
]
GEOM_KEYS = [
    "sprite_x", "sprite_y", "sprite_off_x", "sprite_off_y",
    "muzzle_x", "muzzle_y", "attach_x", "attach_y", "range_radius",
]
# Effect fields whose value is a stat KEY string.  The sim interns the canonical
# `stat_*` key (e.g. "stat_damage") where a source resource may name the plain
# property ("damage"); both map to the same WeaponStats field, so compare them
# after stripping the canonical prefix.
STAT_NAME_KEYS = ("stat", "stat_scaled", "increased_stat_name", "stat_name")


def _norm_stat_name(key, value):
    if key in STAT_NAME_KEYS and isinstance(value, str) and value.startswith("stat_"):
        return value[5:]
    return value


EFFECT_EXTRA_KEYS = ["stat", "stat_nb", "chance", "scale", "explosion_scene",
                     "stat_scaled", "nb_stat_scaled", "increased_stat_name", "stat_name",
                     "is_pet", "is_structure", "can_be_grouped", "spawn_cooldown",
                     "perm_stats_only"]

# The Demon-endless priority weapons from ArosRising's guide.  These MUST be
# present in the comparison (they are the whole point of the matrix).
PRIORITY = {
    "weapon_ghost_scepter_1", "weapon_pruner_1",
    "weapon_potato_thrower_2", "weapon_excalibur_4",  # first shipped tier of each
    "weapon_dagger_1",                                # the guide's "Thief Dagger"
    # item-weapon / item-explosion path from the guide
    "alien_eyes", "spicy_sauce",
}

# Owner routing: which code owner must fix a sim-vs-game divergence.
OWNER_MELEE = "AuditMelee"
OWNER_RANGED = "AuditRanged"
OWNER_ITEMS = "AuditItems"


def _is_num(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def _close(a, b):
    if _is_num(a) and _is_num(b):
        return abs(float(a) - float(b)) <= FLOAT_ATOL
    return a == b


def _cmp(eng, sim, keys, section, out):
    """Append mismatches as (section, field, engine, sim); returns count."""
    n = 0
    for key in keys:
        if key not in eng or key not in sim:
            continue
        if section == "eff" and key in EFFECTIVE_SKIP:
            continue
        if not _close(eng[key], sim[key]):
            out.append((section, key, eng[key], sim[key]))
            n += 1
    return n


def _compare_geom(eng, sim, out):
    if not eng or not sim:
        return 0
    n = 0
    if eng.get("kind") != sim.get("kind"):
        out.append(("geom", "kind", eng.get("kind"), sim.get("kind")))
        n += 1
    if eng.get("kind") == "rect":
        if not _close(eng.get("half"), sim.get("half")):
            out.append(("geom", "half", eng.get("half"), sim.get("half")))
            n += 1
    elif eng.get("kind") == "circle":
        if not _close(eng.get("radius"), sim.get("radius")):
            out.append(("geom", "radius", eng.get("radius"), sim.get("radius")))
            n += 1
    if not _close(eng.get("center"), sim.get("center")):
        out.append(("geom", "center", eng.get("center"), sim.get("center")))
        n += 1
    for key in GEOM_KEYS:
        if key in eng and key in sim and not _close(eng[key], sim[key]):
            out.append(("geom", key, eng[key], sim[key]))
            n += 1
    return n


def _compare_proj(eng, sim, out):
    """Projectile-scene geometry (engine key names differ from sim names)."""
    ep = eng.get("proj") or {}
    sp = sim.get("proj") or {}
    if not ep:
        return 0
    if not sp:
        out.append(("proj", "present", ep, None))
        return 1
    n = 0
    for ek, sk in [("hit_radius", "hit_radius"), ("stop_delay", "stop_delay"),
                   ("delay", "delay"), ("rotation_speed", "rot_speed")]:
        if ek in ep and sk in sp and not _close(ep[ek], sp[sk]):
            out.append(("proj", ek, ep[ek], sp[sk]))
            n += 1
    c = ep.get("hit_center")
    if c and not (_close(c[0], sp.get("hit_x")) and _close(c[1], sp.get("hit_y"))):
        out.append(("proj", "hit_center", c, [sp.get("hit_x"), sp.get("hit_y")]))
        n += 1
    return n


NESTED_KEYS = {
    "weapon_stats": ["damage", "cooldown", "max_range", "crit_chance",
                     "piercing", "bounce", "projectile_speed"],
    "onhit_mid": ["damage", "max_range", "crit_chance", "piercing", "bounce",
                  "piercing_dmg_reduction", "bounce_dmg_reduction",
                  "projectile_speed", "nb_projectiles", "projectile_spread"],
    "stats": ["damage", "cooldown", "max_range", "crit_chance"],
    "explosion_geom": ["radius", "center_x", "center_y"],
}


def _cmp_nested(eng, sim, keys, field, out):
    n = 0
    for key in keys:
        if key in eng and key in sim and not _close(eng[key], sim[key]):
            out.append(("effects", f"{field}.{key}", eng[key], sim[key]))
            n += 1
    return n


def _compare_effects(eng, sim, out):
    ef = eng.get("effects") or []
    sf = sim.get("effects") or []
    n = 0
    if len(ef) != len(sf):
        out.append(("effects", "count", len(ef), len(sf)))
        n += 1
    for i, (e, s) in enumerate(zip(ef, sf)):
        for key in ["class", "key", "value"]:
            if not _close(e.get(key), s.get(key)):
                out.append(("effects", f"[{i}].{key}", e.get(key), s.get(key)))
                n += 1
        for key in EFFECT_EXTRA_KEYS:
            if key in e and key in s and not _close(_norm_stat_name(key, e[key]),
                                                    _norm_stat_name(key, s[key])):
                out.append(("effects", f"[{i}].{key}", e[key], s[key]))
                n += 1
        for nested, keys in NESTED_KEYS.items():
            if nested in e and nested in s:
                n += _cmp_nested(e[nested], s[nested], keys, f"[{i}].{nested}", out)
    return n


def _cmp_scaling(eng, sim, out):
    e = eng.get("base", {}).get("scaling_stats", [])
    s = sim.get("base", {}).get("scaling_stats", [])
    if len(e) != len(s):
        out.append(("scaling", "count", len(e), len(s)))
        return
    for i, (a, b) in enumerate(zip(e, s)):
        if a.get("stat") != b.get("stat"):
            out.append(("scaling", f"[{i}].stat", a.get("stat"), b.get("stat")))
        elif not _close(a.get("ratio"), b.get("ratio")):
            out.append(("scaling", f"[{i}].ratio", a.get("ratio"), b.get("ratio")))


def _compare_weapon(eng: dict, sim: dict) -> dict:
    mismatches: list[tuple] = []
    sections = {}

    def run(name, fn):
        before = len(mismatches)
        fn()
        sections[name] = len(mismatches) - before

    if eng.get("case") == "item" or sim.get("case") == "item":
        run("identity", lambda: [mismatches.append(("identity", k, eng[k], sim[k]))
                                 for k in ["tier", "value"]
                                 if k in eng and k in sim and not _close(eng[k], sim[k])])
        run("effects", lambda: _compare_effects(eng, sim, mismatches))
        return {"mismatches": mismatches, "sections": sections}

    run("identity", lambda: [mismatches.append(("identity", k, eng[k], sim[k]))
                             for k in ["tier", "kind", "value"]
                             if k in eng and k in sim and not _close(eng[k], sim[k])])
    run("base", lambda: _cmp(eng.get("base", {}), sim.get("base", {}), BASE_KEYS, "base", mismatches))
    run("scaling", lambda: _cmp_scaling(eng, sim, mismatches))
    run("geom", lambda: _compare_geom(eng.get("geom", {}), sim.get("geom", {}), mismatches))
    run("eff_zero", lambda: _cmp(eng.get("eff_zero", {}), sim.get("eff_zero", {}), EFF_KEYS, "eff", mismatches))
    run("eff_mid", lambda: _cmp(eng.get("eff_mid", {}), sim.get("eff_mid", {}), EFF_KEYS, "eff", mismatches))
    run("eff_demon", lambda: _cmp(eng.get("eff_demon", {}), sim.get("eff_demon", {}), EFF_KEYS, "eff", mismatches))
    run("proj", lambda: _compare_proj(eng, sim, mismatches))
    run("effects", lambda: _compare_effects(eng, sim, mismatches))
    return {"mismatches": mismatches, "sections": sections}


# Short cause notes for the report's failure-signature list (kept next to the
# comparison so the routing rationale travels with the generated report).
FAILURE_NOTES = {
    ("geom", "attach_x"): (
        "sim reads a root-level `.tscn` Attach node Godot never instantiates; "
        "game uses `$Sprite/Attach` LOCAL position (weapon.gd:64/166). "
        "Fix: extract_weapons.py `attach = table.get(\"Sprite/Attach\")` local, no sprite offset."
    ),
    ("geom", "attach_y"): (
        "same as attach_x (attach_y is the y component of the same Sprite/Attach read)"
    ),
    ("eff", "damage"): (
        "weapon_anchor_3: engine 85*1.3=110.5 rounds to 111 in GDScript double, sim float gives 110; "
        "weapon_captains_sword_3/4: WeaponGainStatForEveryStatEffect with stat_scaled=\"free_weapon_slots\" "
        "is unmapped in bt_core_id_from_istr (run_data.gd:1883-1893)."
    ),
}


def _owner_for(eng, sections) -> str:
    """Route a divergence to the code owner of the sim side that must change.

    ``effects`` rows live in the item/weapon effect tables (AuditItems); the
    weapon geometry table, base/effective stat tables and the
    ``bt_weapon_stats`` resolver all live in
    ``brotato_weapons.h`` / ``brotato_weapon_stats.h`` (AuditRanged); only a
    plain identity mismatch falls back to the melee/ranged weapon-data owner.
    """
    if sections.get("effects"):
        return OWNER_ITEMS
    if any(sections.get(s) for s in ("geom", "base", "scaling", "eff_zero", "eff_mid", "proj")):
        return OWNER_RANGED
    if eng.get("kind") == 0:
        return OWNER_MELEE
    return OWNER_RANGED


def _dlc_root() -> Path:
    return Path(os.environ.get("BROTATO_DLC_ROOT", str(ROOT / "migration" / "game" / "brotato_abyss")))


def _merged_project(dst: Path) -> Path:
    """Overlay the campaign tree with the DLC ``dlcs/`` directory.

    The recovered campaign sources are the base game; the Abyss DLC ships as a
    ``dlcs/dlc_1/...`` overlay.  Godot loads one ``res://`` root, so build a
    merged project with per-entry symlinks (plus the union of both ``.import``
    payload directories, which is what makes the recovered textures load).  When
    no DLC tree is present the campaign root is used unchanged.
    """
    recovered = _recovered_root()
    dlc = _dlc_root()
    if not (dlc / "dlcs").is_dir():
        return recovered
    dst.mkdir(parents=True, exist_ok=True)
    for entry in sorted(recovered.iterdir()):
        if entry.name in (".import", "dlcs"):
            continue
        (dst / entry.name).symlink_to(entry)
    imp = dst / ".import"
    imp.mkdir(exist_ok=True)
    for src in (recovered / ".import", dlc / ".import"):
        if src.is_dir():
            for f in src.iterdir():
                (imp / f.name).symlink_to(f)
    (dst / "dlcs").symlink_to(dlc / "dlcs")
    return dst


@pytest.fixture(scope="session")
def engine_catalogue(tmp_path_factory: pytest.TempPathFactory) -> dict:
    """Every recovered weapon tier, read by the real engine."""
    _require_environment()
    root = _merged_project(tmp_path_factory.mktemp("weapon-catalogue") / "project")
    trace = tmp_path_factory.mktemp("weapon-catalogue") / "catalogue.jsonl"
    env = dict(
        os.environ,
        BROTATO_WEAPON_ORACLE_TRACE=str(trace),
        BROTATO_WEAPON_ORACLE_MODE="catalogue",
    )
    process = subprocess.run(
        _godot_argv()
        + [
            "--path", str(root),
            "--no-window", "--disable-audio",
            "--fixed-fps", "60",
            "--script", str(ORACLE_SCRIPT),
        ],
        env=env, capture_output=True, text=True, timeout=3600,
    )
    assert trace.exists(), (
        "catalogue oracle produced no trace\nstderr tail:\n" + process.stderr[-2000:]
    )
    rows = [json.loads(line) for line in trace.read_text().splitlines() if line.strip()]
    catalogue = {r["my_id"]: r for r in rows
                 if r.get("case") in ("weapon", "item") and r.get("my_id")}
    assert catalogue, "catalogue oracle emitted no weapon rows"
    return catalogue


@pytest.fixture(scope="session")
def sim_catalogue(tmp_path_factory: pytest.TempPathFactory) -> dict:
    """Every sim weapon tier, dumped from the generated tables."""
    if not DUMP_SRC.exists():
        pytest.fail(f"sim parity dump source missing: {DUMP_SRC}")
    cxx = os.environ.get("CXX", "g++")
    if not shutil.which(cxx):
        pytest.skip(f"C++ compiler not found: {cxx}")
    out_bin = tmp_path_factory.mktemp("weapon-parity") / "weapon_parity_dump"
    cmd = [
        cxx, "-std=c++17", "-O2", "-Wno-narrowing", "-ffp-contract=off",
        "-I.", "-Iocean/brotato", str(DUMP_SRC), "-o", str(out_bin),
    ]
    build = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=1800)
    if build.returncode != 0:
        pytest.fail("sim parity dump build failed:\n" + build.stderr[-4000:])
    run = subprocess.run([str(out_bin)], capture_output=True, text=True, timeout=600)
    assert run.returncode == 0, f"sim parity dump failed: {run.stderr[-1000:]}"
    rows = [json.loads(line) for line in run.stdout.splitlines() if line.strip()]
    return {r["my_id"]: r for r in rows}


@pytest.fixture(scope="session")
def parity_matrix(engine_catalogue, sim_catalogue) -> dict:
    """Compare every weapon and write tests/weapon_parity_report.md."""
    results = []
    for my_id in sorted(set(engine_catalogue) | set(sim_catalogue)):
        eng = engine_catalogue.get(my_id)
        sim = sim_catalogue.get(my_id)
        if eng is None:
            results.append((my_id, sim, None, "SKIP",
                            [("coverage", "in-engine", None, "absent (DLC tree / not catalogued)")]))
            continue
        if sim is None:
            results.append((my_id, eng, None, "SKIP",
                            [("coverage", "in-sim", eng.get("weapon_id"),
                              "engine-only template resource (no weapon_id); not in the sim weapon pool")]))
            continue
        cmp = _compare_weapon(eng, sim)
        results.append((my_id, eng, sim, "PASS" if not cmp["mismatches"] else "FAIL", cmp))
    _write_report(results)
    return {"results": results}


def _write_report(results) -> None:
    lines = [
        "# Weapon parity matrix (sim vs pinned Godot 3.7)",
        "",
        "Generated by `tests/test_brotato_weapon_godot_oracles.py` "
        "(`test_weapon_parity_matrix`).",
        "",
        "Engine side: `tests/brotato_weapon_oracle_project/weapon_oracle.gd` "
        "MODE=catalogue reads every recovered weapon resource and runs the real "
        "`weapon_service.gd` resolver. Sim side: "
        "`tests/brotato_weapon_oracle_project/weapon_parity_dump.cpp` dumps the "
        "generated tables + `bt_weapon_stats_resolve`.",
        "",
        "Columns: `base` = base WeaponStats fields + scaling rows; `geom` = "
        "hitbox/sprite/muzzle/attach/range geometry; `eff0` / `effmid` / "
        "`effdemon` = effective stats under the zero / mid / Demon (300 max-hp, "
        "Danger-5 late-game) controlled player stat blocks; `proj` = "
        "projectile-scene geometry; `fx` = weapon effect rows.",
        "",
    ]
    n_pass = sum(1 for r in results if r[3] == "PASS")
    n_fail = sum(1 for r in results if r[3] == "FAIL")
    n_skip = sum(1 for r in results if r[3] == "SKIP")
    n_weapon = sum(1 for r in results if (r[1] or r[2] or {}).get("case") == "weapon")
    n_item = sum(1 for r in results if (r[1] or r[2] or {}).get("case") == "item")
    lines.append(f"Totals: {len(results)} rows ({n_weapon} weapon tiers + {n_item} item rows), "
                 f"{n_pass} PASS, {n_fail} FAIL, {n_skip} SKIP.")
    lines.append("")
    lines.append("Coverage: the engine pass runs against a merged project (campaign tree + "
                 "`dlcs/dlc_1` overlay) so every shipped tier, campaign and DLC, is read; "
                 "the sim pass dumps all `BT_WEAPONS` rows. `weapon_knuckles` is an "
                 "unregistered template resource with no `weapon_id` (see the "
                 "`brotato_weapons.h` header) and is not part of the weapon pool, hence SKIP.")
    lines.append("")
    lines.append("| weapon (my_id) | tier | kind | base | geom | eff0 | effmid | effdemon | proj | fx | status | first mismatch | owner |")
    lines.append("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for my_id, eng, sim, status, cmp in results:
        src = eng or sim or {}
        tier = src.get("tier", "")
        # engine rows expose WeaponData.Type as ``type``; sim rows use ``kind``
        kind = src.get("kind", src.get("type", ""))
        if status == "SKIP":
            s0 = cmp[0]
            lines.append(f"| `{my_id}` | {tier} | {kind} | - | - | - | - | - | - | - | SKIP | "
                         f"{s0[0]}.{s0[1]}: {s0[3]} | - |")
            continue
        sec = cmp["sections"]
        mm = cmp["mismatches"]
        first, owner = "-", "-"
        if mm:
            s0 = mm[0]
            first = f"{s0[0]}.{s0[1]}: engine={s0[2]!r} sim={s0[3]!r} ({len(mm)} mismatches)"
            owner = _owner_for(eng, sec)
        mark = lambda name: "ok" if not sec.get(name) else f"{sec[name]}!"
        lines.append(
            f"| `{my_id}` | {tier} | {kind} | {mark('base')} | {mark('geom')} | "
            f"{mark('eff_zero')} | {mark('eff_mid')} | {mark('eff_demon')} | {mark('proj')} | {mark('effects')} | "
            f"{status} | {first} | {owner} |"
        )
    lines.append("")
    sig = {}
    for _my_id, _eng, _sim, status, cmp in results:
        if status != "FAIL":
            continue
        for sec, field, _ev, _sv in cmp["mismatches"]:
            sig[(sec, field)] = sig.get((sec, field), 0) + 1
    if sig:
        lines.append("## Failure signatures")
        lines.append("")
        for (sec, field), n in sorted(sig.items(), key=lambda kv: -kv[1]):
            note = FAILURE_NOTES.get((sec, field), "")
            lines.append(f"- `{sec}.{field}`: {n} weapon rows" + (f" — {note}" if note else ""))
        lines.append("")
    failures = [(r[0], r[1], r[4]) for r in results if r[3] == "FAIL"]
    if failures:
        lines.append("## Failures by owner")
        lines.append("")
        by_owner = {}
        for my_id, eng, cmp in failures:
            by_owner.setdefault(_owner_for(eng, cmp["sections"]), []).append(my_id)
        for owner, ids in sorted(by_owner.items()):
            lines.append(f"- **{owner}**: " + ", ".join(f"`{i}`" for i in ids))
        lines.append("")
    REPORT.write_text("\n".join(lines) + "\n")


def test_weapon_parity_matrix(parity_matrix):
    results = parity_matrix["results"]
    found = {r[0] for r in results if r[3] != "SKIP"}
    missing = sorted(PRIORITY - found)
    assert not missing, f"priority weapons absent from the parity comparison: {missing}"
    assert REPORT.exists(), "parity report was not written"

    failures = [(r[0], r[4]["mismatches"][0]) for r in results if r[3] == "FAIL"]
    if failures:
        detail = "\n".join(
            f"  {name}: {sec}.{field}: engine={ev!r} sim={sv!r}"
            for name, (sec, field, ev, sv) in failures[:40]
        )
        pytest.fail(
            f"{len(failures)} weapon(s) diverge from the engine; see "
            f"{REPORT.relative_to(ROOT)} for the full matrix and owner routing:\n{detail}"
        )


# ---------------------------------------------------------------------------
# ranged recoil freeze (the cooldown clock's shooting window)
# ---------------------------------------------------------------------------

RECOIL_ORACLE_SCRIPT = ROOT / "tests" / "brotato_weapon_oracle_project" / "weapon_recoil_oracle.gd"
RECOIL_DUMP_SRC = ROOT / "tests" / "brotato_weapon_oracle_project" / "weapon_recoil_dump.cpp"


@pytest.fixture(scope="session")
def engine_recoil_freeze(tmp_path_factory: pytest.TempPathFactory) -> dict:
    """The pinned engine's own freeze length per recoil_duration."""
    _require_environment()
    if not RECOIL_ORACLE_SCRIPT.exists():
        pytest.fail(f"recoil oracle fixture missing: {RECOIL_ORACLE_SCRIPT}")
    trace = tmp_path_factory.mktemp("weapon-recoil") / "recoil.jsonl"
    env = dict(os.environ, BROTATO_WEAPON_ORACLE_TRACE=str(trace))
    process = subprocess.run(
        _godot_argv()
        + [
            "--path", str(_recovered_root()),
            "--no-window", "--disable-audio",
            "--fixed-fps", "60",
            "--script", str(RECOIL_ORACLE_SCRIPT),
        ],
        env=env, capture_output=True, text=True, timeout=1800,
    )
    if process.returncode != 0:
        pytest.fail(
            "recoil oracle run failed with code "
            f"{process.returncode}\nstderr tail:\n{process.stderr[-2000:]}"
        )
    assert trace.exists(), "recoil oracle produced no trace"
    rows = [json.loads(line) for line in trace.read_text().splitlines() if line.strip()]
    freeze = {int(r["index"]): (float(r["recoil_duration"]), int(r["freeze_frames"]))
              for r in rows if r.get("case") == "recoil_freeze"}
    assert freeze, "recoil oracle emitted no recoil_freeze rows"
    return freeze


@pytest.fixture(scope="session")
def sim_recoil_freeze(tmp_path_factory: pytest.TempPathFactory) -> dict:
    """The shared core's freeze length for the same durations."""
    if not RECOIL_DUMP_SRC.exists():
        pytest.fail(f"recoil dump source missing: {RECOIL_DUMP_SRC}")
    cxx = os.environ.get("CXX", "g++")
    if not shutil.which(cxx):
        pytest.skip(f"C++ compiler not found: {cxx}")
    out_bin = tmp_path_factory.mktemp("weapon-recoil") / "weapon_recoil_dump"
    cmd = [
        cxx, "-std=c++17", "-O2", "-Wno-narrowing", "-ffp-contract=off",
        "-I.", "-Iocean/brotato", str(RECOIL_DUMP_SRC), "-o", str(out_bin),
    ]
    build = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=1800)
    if build.returncode != 0:
        pytest.fail("recoil dump build failed:\n" + build.stderr[-4000:])
    run = subprocess.run([str(out_bin)], capture_output=True, text=True, timeout=600)
    assert run.returncode == 0, f"recoil dump failed: {run.stderr[-1000:]}"
    rows = [json.loads(line) for line in run.stdout.splitlines() if line.strip()]
    return {int(r["index"]): (float(r["recoil_duration"]), int(r["freeze_frames"]))
            for r in rows}


def test_ranged_recoil_freeze_matches_core(engine_recoil_freeze, sim_recoil_freeze):
    """The core's cooldown freeze must equal the engine's per recoil_duration.

    The shipped ranged weapons use recoil_duration 0.01/0.02/0.05/0.1/0.142/0.15/
    0.2, so a wrong Tween frame model (e.g. binary64 instead of the engine's
    float32 1/60 accumulator, or one frame short per chained tween) moves the
    whole weapon clock -- measured on the Demon seed-1 Ghost-Scepter capture, a
    4-frame-short freeze was the first divergence at tick 158.
    """
    missing = sorted(set(engine_recoil_freeze) - set(sim_recoil_freeze))
    assert not missing, f"the sim dump omits oracle sweeps: {missing}"
    bad = []
    for index in sorted(engine_recoil_freeze):
        duration, engine = engine_recoil_freeze[index]
        assert abs(duration - sim_recoil_freeze[index][0]) < 1e-6, (
            f"sweep {index} is a different recoil_duration: "
            f"engine={duration} core={sim_recoil_freeze[index][0]}"
        )
        sim = sim_recoil_freeze[index][1]
        if engine != sim:
            bad.append(f"recoil_duration={duration:g}: engine={engine} core={sim}")
    assert not bad, "recoil freeze diverges from the pinned engine:\n  " + "\n  ".join(bad)
