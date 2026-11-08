"""Behavioural tests for the fresh/reuse RNG-init extraction in
``ocean/brotato/extract_gameplay.py``.

The extractor turns a unit scene's flattened node tree into the ordered
``rand_range`` draws a *fresh* ``scene.instance()`` / ``_ready`` / ``init``
spawn performs and the (different) draws a pooled ``respawn()`` performs.  The
tests below build tiny synthetic Godot scenes that inherit behaviour nodes the
same way the real project does, run the real extraction code over them and
assert the resulting *ordering, bounds, phases and semantic destinations* --
never the generator's source text.
"""

from __future__ import annotations

import importlib.util
import math
import os
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
_MODULE_PATH = ROOT / "ocean" / "brotato" / "extract_gameplay.py"
_spec = importlib.util.spec_from_file_location("extract_gameplay_under_test", _MODULE_PATH)
extract_gameplay = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = extract_gameplay
_spec.loader.exec_module(extract_gameplay)

physics = extract_gameplay.physics
ExtractError = extract_gameplay.ExtractError

# Script paths only have to match the extractor's dispatch patterns: the
# behaviour is identified by the script stem, not by reading the .gd.
SCRIPTS = {
    "atk_base": "res://scripts/attack_behaviors/attack_behavior.gd",
    "atk_shoot": "res://scripts/attack_behaviors/shooting_attack_behavior.gd",
    "atk_charge": "res://scripts/attack_behaviors/charging_attack_behavior.gd",
    "atk_spawn": "res://scripts/attack_behaviors/spawning_attack_behavior.gd",
    "mov_follow": "res://scripts/movement_behaviors/follow_target_movement_behavior.gd",
    "mov_stay": "res://scripts/movement_behaviors/stay_in_range_from_player_movement_behavior.gd",
    "mov_follow_rand": "res://scripts/movement_behaviors/follow_rand_pos_around_player_movement_behavior.gd",
    "mov_target_rand": "res://scripts/movement_behaviors/target_rand_pos_around_player_movement_behavior.gd",
    "mov_unknown": "res://scripts/movement_behaviors/alien_new_movement_behavior.gd",
    "boss_state": "res://scripts/boss_state.gd",
}


def _write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def _base_enemy() -> str:
    """The shared enemy scene: a movement node and the root attack node."""
    return (
        '[gd_scene load_steps=3 format=2]\n'
        '\n'
        '[ext_resource path="%s" type="Script" id=1]\n'
        '[ext_resource path="%s" type="Script" id=2]\n'
        '\n'
        '[node name="Enemy" type="Node2D"]\n'
        '\n'
        '[node name="Animation" type="Node2D" parent="."]\n'
        '\n'
        '[node name="MovementBehavior" type="Node2D" parent="." index="6"]\n'
        'script = ExtResource( 2 )\n'
        '\n'
        '[node name="AttackBehavior" type="Node2D" parent="." index="7"]\n'
        'script = ExtResource( 1 )\n'
    ) % (SCRIPTS["atk_base"], SCRIPTS["mov_follow"])


def _boss_state() -> str:
    """State template: movement + attack children, like boss_state.tscn."""
    return (
        '[gd_scene load_steps=4 format=2]\n'
        '\n'
        '[ext_resource path="%s" type="Script" id=1]\n'
        '[ext_resource path="%s" type="Script" id=2]\n'
        '[ext_resource path="%s" type="Script" id=3]\n'
        '\n'
        '[node name="State1" type="Node2D"]\n'
        'script = ExtResource( 3 )\n'
        '\n'
        '[node name="MovementBehavior" type="Node2D" parent="."]\n'
        'script = ExtResource( 1 )\n'
        '\n'
        '[node name="AttackBehavior" type="Node2D" parent="."]\n'
        'script = ExtResource( 2 )\n'
    ) % (SCRIPTS["mov_follow"], SCRIPTS["atk_base"], SCRIPTS["boss_state"])


def build_project(tmp_path: Path, kind_scenes: dict) -> Path:
    campaign = tmp_path / "campaign"
    _write(campaign / "enemy_base.tscn", _base_enemy())
    _write(campaign / "boss_state.tscn", _boss_state())
    for name, text in kind_scenes.items():
        _write(campaign / name, text)
    return campaign


def extract(campaign: Path, scene: str, is_boss: bool, speed_rand: int):
    resolver = physics.Resolver([str(campaign)])
    table = physics.flatten_scene(resolver, scene, {})
    return extract_gameplay.extract_kind_init(resolver, scene, table, is_boss, speed_rand)


def shape(ops):
    """``(phase, site, dest, behavior, state)`` of every op, in order."""
    return [(op["phase"], op["site"], op["dest"], op["behavior"], op["state"])
            for op in ops]


G = extract_gameplay


# --------------------------------------------------------------------------
# inheritance + `_ready` before `init` before the speed jitter
# --------------------------------------------------------------------------
INHERITOR = (
    '[gd_scene load_steps=5 format=2]\n'
    '\n'
    '[ext_resource path="res://enemy_base.tscn" type="PackedScene" id=1]\n'
    '[ext_resource path="%s" type="Script" id=2]\n'
    '[ext_resource path="%s" type="Script" id=3]\n'
    '[ext_resource path="%s" type="Script" id=4]\n'
    '\n'
    '[node name="Enemy" instance=ExtResource( 1 )]\n'
    '\n'
    '[node name="MovementBehavior" parent="." index="6"]\n'
    'script = ExtResource( 2 )\n'
    'target_range = 400\n'
    'target_range_randomization = 50\n'
    '\n'
    '[node name="AttackBehavior" parent="." index="7"]\n'
    'script = ExtResource( 3 )\n'
    'cooldown = 30.0\n'
    'max_cd_randomization = 5\n'
    '\n'
    '[node name="ExtraShootBehavior" type="Node2D" parent="." index="9"]\n'
    'script = ExtResource( 4 )\n'
    'cooldown = 100.0\n'
    'max_cd_randomization = 0\n'
) % (SCRIPTS["mov_stay"], SCRIPTS["atk_shoot"], SCRIPTS["atk_charge"])


def test_fresh_order_includes_inherited_and_added_behaviour_nodes(tmp_path):
    campaign = build_project(tmp_path, {"inheritor.tscn": INHERITOR})
    init = extract(campaign, "res://inheritor.tscn", is_boss=False, speed_rand=10)

    assert shape(init["fresh"]) == [
        # Enemy member initializer.
        (G.BT_INIT_PHASE_FRESH, G.BT_INITSITE_MEMBER,
         G.BT_INITDEST_IDLE_PLAYBACK, G.BT_INITBEH_UNIT, G.BT_INIT_ROOT_STATE),
        # $MovementBehavior._ready (index 6) before $AttackBehavior._ready (7).
        (G.BT_INIT_PHASE_FRESH, G.BT_INITSITE_MOV_READY,
         G.BT_INITDEST_MOV_ACTUAL_RANGE, G.BT_INITBEH_MOV_STAY_IN_RANGE,
         G.BT_INIT_ROOT_STATE),
        (G.BT_INIT_PHASE_FRESH, G.BT_INITSITE_ATK_READY,
         G.BT_INITDEST_ATK_CD, G.BT_INITBEH_ATK_SHOOTING, G.BT_INIT_ROOT_STATE),
        # The extra node the derived scene adds (index 9) readies after them.
        (G.BT_INIT_PHASE_FRESH, G.BT_INITSITE_ATK_READY,
         G.BT_INITDEST_ATK_CD, G.BT_INITBEH_ATK_CHARGING, G.BT_INIT_ROOT_STATE),
        # Unit.init's movement init (none here) then the speed jitter.
        (G.BT_INIT_PHASE_BOTH, G.BT_INITSITE_SPEED,
         G.BT_INITDEST_SPEED_RAND, G.BT_INITBEH_UNIT, G.BT_INIT_ROOT_STATE),
    ]


def test_bounds_come_from_the_scene_exports(tmp_path):
    campaign = build_project(tmp_path, {"inheritor.tscn": INHERITOR})
    init = extract(campaign, "res://inheritor.tscn", is_boss=False, speed_rand=10)
    fresh = init["fresh"]

    # stay_in_range: `target_range + rand_range(-randomization, +randomization)`
    # -- the draw's own bounds, then the base the source adds afterwards.
    assert (fresh[1]["lo"], fresh[1]["hi"], fresh[1]["add"]) == (-50.0, 50.0, 400.0)
    # shooting get_cd: rand_range(max(1, cooldown - rand), cooldown + rand)
    assert (fresh[2]["lo"], fresh[2]["hi"], fresh[2]["add"]) == (25.0, 35.0, 0.0)
    # charging get_cd reuses its own exports (max_cd_randomization = 0)
    assert (fresh[3]["lo"], fresh[3]["hi"]) == (100.0, 100.0)
    # speed jitter is symmetric and int-typed (`... as int`)
    assert (fresh[4]["lo"], fresh[4]["hi"], fresh[4]["add"]) == (-10.0, 10.0, 0.0)
    assert fresh[4]["flags"] & G.BT_INITFLAG_TRUNC
    assert not fresh[1]["flags"] & G.BT_INITFLAG_TRUNC


def test_reuse_order_is_jitter_then_root_attack_reset(tmp_path):
    campaign = build_project(tmp_path, {"inheritor.tscn": INHERITOR})
    init = extract(campaign, "res://inheritor.tscn", is_boss=False, speed_rand=10)

    assert shape(init["reuse"]) == [
        (G.BT_INIT_PHASE_BOTH, G.BT_INITSITE_SPEED,
         G.BT_INITDEST_SPEED_RAND, G.BT_INITBEH_UNIT, G.BT_INIT_ROOT_STATE),
        (G.BT_INIT_PHASE_REUSE, G.BT_INITSITE_ATK_RESET,
         G.BT_INITDEST_ATK_CD, G.BT_INITBEH_ATK_SHOOTING, G.BT_INIT_ROOT_STATE),
    ]
    assert (init["reuse"][1]["lo"], init["reuse"][1]["hi"]) == (25.0, 35.0)


def test_inherited_kind_with_no_overrides_only_pays_idle_and_jitter(tmp_path):
    campaign = build_project(tmp_path, {
        "plain.tscn": (
            '[gd_scene load_steps=2 format=2]\n'
            '\n'
            '[ext_resource path="res://enemy_base.tscn" type="PackedScene" id=1]\n'
            '\n'
            '[node name="Enemy" instance=ExtResource( 1 )]\n'
        )})
    init = extract(campaign, "res://plain.tscn", is_boss=False, speed_rand=50)

    assert shape(init["fresh"]) == [
        (G.BT_INIT_PHASE_FRESH, G.BT_INITSITE_MEMBER,
         G.BT_INITDEST_IDLE_PLAYBACK, G.BT_INITBEH_UNIT, G.BT_INIT_ROOT_STATE),
        (G.BT_INIT_PHASE_BOTH, G.BT_INITSITE_SPEED,
         G.BT_INITDEST_SPEED_RAND, G.BT_INITBEH_UNIT, G.BT_INIT_ROOT_STATE),
    ]
    assert shape(init["reuse"]) == [
        (G.BT_INIT_PHASE_BOTH, G.BT_INITSITE_SPEED,
         G.BT_INITDEST_SPEED_RAND, G.BT_INITBEH_UNIT, G.BT_INIT_ROOT_STATE),
    ]


def test_add_flag_marks_a_base_that_is_added_after_the_draw(tmp_path):
    campaign = build_project(tmp_path, {"inheritor.tscn": INHERITOR})
    fresh = extract(campaign, "res://inheritor.tscn", is_boss=False,
                    speed_rand=10)["fresh"]

    # stay_in_range `_actual_target_range` = target_range + rand_range(-rr, rr):
    # the base is applied AFTER the draw, so the row must carry
    # BT_INITFLAG_ADD (which is what keeps a zero base from being folded away).
    assert fresh[1]["flags"] & G.BT_INITFLAG_ADD
    assert fresh[1]["add"] == 400.0
    # A draw whose value IS the result (idle member, attack get_cd, speed
    # jitter) must not claim an addition, even though add is 0.0 on the row.
    for op in (fresh[0], fresh[2], fresh[3], fresh[4]):
        assert not op["flags"] & G.BT_INITFLAG_ADD
        assert op["add"] == 0.0


# --------------------------------------------------------------------------
# chained bounds and boss-State ordering
# --------------------------------------------------------------------------
BOSS = (
    '[gd_scene load_steps=7 format=2]\n'
    '\n'
    '[ext_resource path="res://enemy_base.tscn" type="PackedScene" id=1]\n'
    '[ext_resource path="res://boss_state.tscn" type="PackedScene" id=2]\n'
    '[ext_resource path="%s" type="Script" id=3]\n'
    '[ext_resource path="%s" type="Script" id=4]\n'
    '[ext_resource path="%s" type="Script" id=5]\n'
    '[ext_resource path="%s" type="Script" id=6]\n'
    '\n'
    '[node name="Boss" instance=ExtResource( 1 )]\n'
    '\n'
    '[node name="States" type="Node2D" parent="." index="10"]\n'
    '\n'
    '[node name="MovementBehavior" parent="." index="6"]\n'
    'script = ExtResource( 3 )\n'
    'range_around_player = 300\n'
    'range_randomization = 0\n'
    '\n'
    '[node name="State1" parent="States" index="0" instance=ExtResource( 2 )]\n'
    '\n'
    '[node name="MovementBehavior" parent="States/State1" index="0"]\n'
    'script = ExtResource( 4 )\n'
    'range_around_player = 200\n'
    'range_randomization = 40\n'
    '\n'
    '[node name="AttackBehavior" parent="States/State1" index="1"]\n'
    'script = ExtResource( 5 )\n'
    'cooldown = 60.0\n'
    'max_cd_randomization = 10\n'
    '\n'
    '[node name="State2" parent="States" index="1" instance=ExtResource( 2 )]\n'
    '\n'
    '[node name="MovementBehavior" parent="States/State2" index="0"]\n'
    'script = ExtResource( 6 )\n'
    'target_range = 200\n'
    'target_range_randomization = 100\n'
) % (SCRIPTS["mov_follow_rand"], SCRIPTS["mov_target_rand"],
     SCRIPTS["atk_shoot"], SCRIPTS["mov_stay"])


def test_state_ready_draws_precede_root_init_draws(tmp_path):
    campaign = build_project(tmp_path, {"boss.tscn": BOSS})
    fresh = extract(campaign, "res://boss.tscn", is_boss=True, speed_rand=25)["fresh"]

    # idle; ready pass over the whole tree (root movement/attack have none, so
    # State1's attack and State2's movement); root movement init; jitter; then
    # Boss.init's per-state movement inits.
    assert shape(fresh) == [
        (G.BT_INIT_PHASE_FRESH, G.BT_INITSITE_MEMBER,
         G.BT_INITDEST_IDLE_PLAYBACK, G.BT_INITBEH_UNIT, G.BT_INIT_ROOT_STATE),
        (G.BT_INIT_PHASE_FRESH, G.BT_INITSITE_ATK_READY,
         G.BT_INITDEST_ATK_CD, G.BT_INITBEH_ATK_SHOOTING, 0),
        (G.BT_INIT_PHASE_FRESH, G.BT_INITSITE_MOV_READY,
         G.BT_INITDEST_MOV_ACTUAL_RANGE, G.BT_INITBEH_MOV_STAY_IN_RANGE, 1),
        (G.BT_INIT_PHASE_FRESH, G.BT_INITSITE_MOV_INIT,
         G.BT_INITDEST_MOV_ACTUAL, G.BT_INITBEH_MOV_FOLLOW_RAND_AROUND,
         G.BT_INIT_ROOT_STATE),
        (G.BT_INIT_PHASE_FRESH, G.BT_INITSITE_MOV_INIT,
         G.BT_INITDEST_MOV_DIST_X, G.BT_INITBEH_MOV_FOLLOW_RAND_AROUND,
         G.BT_INIT_ROOT_STATE),
        (G.BT_INIT_PHASE_FRESH, G.BT_INITSITE_MOV_INIT,
         G.BT_INITDEST_MOV_DIST_Y, G.BT_INITBEH_MOV_FOLLOW_RAND_AROUND,
         G.BT_INIT_ROOT_STATE),
        (G.BT_INIT_PHASE_BOTH, G.BT_INITSITE_SPEED,
         G.BT_INITDEST_SPEED_RAND, G.BT_INITBEH_UNIT, G.BT_INIT_ROOT_STATE),
        (G.BT_INIT_PHASE_FRESH, G.BT_INITSITE_MOV_INIT,
         G.BT_INITDEST_MOV_ACTUAL, G.BT_INITBEH_MOV_TARGET_RAND_AROUND, 0),
    ]


def test_draw_bounds_chain_to_the_value_just_drawn(tmp_path):
    campaign = build_project(tmp_path, {"boss.tscn": BOSS})
    fresh = extract(campaign, "res://boss.tscn", is_boss=True, speed_rand=25)["fresh"]

    # State2's `_actual_target_range` from its own exports.
    assert (fresh[2]["lo"], fresh[2]["hi"], fresh[2]["add"]) == (-100.0, 100.0, 200.0)
    # follow_rand_pos `_actual` = range_around_player + rand_range(-rr, +rr):
    # the draw's real bounds are +/-range_randomization, the base is added after.
    assert (fresh[3]["lo"], fresh[3]["hi"], fresh[3]["add"]) == (-0.0, 0.0, 300.0)
    assert fresh[3]["flags"] & G.BT_INITFLAG_TRUNC
    # `_distance_from_player` binds to the int `_actual` the previous row stored.
    for op in (fresh[4], fresh[5]):
        assert op["dest"] in (G.BT_INITDEST_MOV_DIST_X, G.BT_INITDEST_MOV_DIST_Y)
        assert op["lo_ref"] == -1 and op["hi_ref"] == 1
        assert op["add"] == 0.0
        assert op["flags"] & G.BT_INITFLAG_NEG_LO
        assert not op["flags"] & G.BT_INITFLAG_NEG_HI
    # State1's target_rand_pos_around `_actual` from ITS overridden exports.
    assert (fresh[7]["lo"], fresh[7]["hi"], fresh[7]["add"]) == (-40.0, 40.0, 200.0)


def test_pi_bound_stays_binary64(tmp_path):
    """`_phase = rand_range(0.0, 2 * PI)` must not be narrowed to float32."""
    import struct

    scene = (
        '[gd_scene load_steps=3 format=2]\n'
        '\n'
        '[ext_resource path="res://enemy_base.tscn" type="PackedScene" id=1]\n'
        '[ext_resource path="%s" type="Script" id=2]\n'
        '\n'
        '[node name="Enemy" instance=ExtResource( 1 )]\n'
        '\n'
        '[node name="MovementBehavior" parent="." index="6"]\n'
        'script = ExtResource( 2 )\n'
    ) % "res://scripts/movement_behaviors/patrol_around_the_map_movement_behavior.gd"
    campaign = build_project(tmp_path, {"patrol.tscn": scene})
    fresh = extract(campaign, "res://patrol.tscn", is_boss=False, speed_rand=0)["fresh"]

    phase = [op for op in fresh if op["dest"] == G.BT_INITDEST_MOV_PHASE]
    assert len(phase) == 1
    assert phase[0]["lo"] == 0.0
    assert phase[0]["hi"] == 2.0 * math.pi
    # a float32 bound would be a different double
    assert struct.unpack("<f", struct.pack("<f", phase[0]["hi"]))[0] != phase[0]["hi"]
    assert phase[0]["add"] == 0.0


def test_boss_kinds_never_reuse_a_pooled_instance(tmp_path):
    campaign = build_project(tmp_path, {"boss.tscn": BOSS})
    init = extract(campaign, "res://boss.tscn", is_boss=True, speed_rand=25)
    assert init["reuse"] == []


# --------------------------------------------------------------------------
# branch conditionalities
# --------------------------------------------------------------------------
def _spawner_scene(max_nb_of_spawns: int, cooldown: float, rand: int) -> str:
    return (
        '[gd_scene load_steps=3 format=2]\n'
        '\n'
        '[ext_resource path="res://enemy_base.tscn" type="PackedScene" id=1]\n'
        '[ext_resource path="%s" type="Script" id=2]\n'
        '\n'
        '[node name="Enemy" instance=ExtResource( 1 )]\n'
        '\n'
        '[node name="AttackBehavior" parent="." index="7"]\n'
        'script = ExtResource( 2 )\n'
        'cooldown = %s\n'
        'max_cd_randomization = %d\n'
        'max_nb_of_spawns = %d\n'
    ) % (SCRIPTS["atk_spawn"], cooldown, rand, max_nb_of_spawns)


def test_spawning_skips_the_fresh_roll_when_it_starts_exhausted(tmp_path):
    campaign = build_project(tmp_path, {
        "capped.tscn": _spawner_scene(0, "90.0", 0)})
    init = extract(campaign, "res://capped.tscn", is_boss=False, speed_rand=0)

    # _ready sets LARGE_NUMBER instead of calling get_cd(), so no fresh roll...
    assert [op["site"] for op in init["fresh"]] == [
        G.BT_INITSITE_MEMBER, G.BT_INITSITE_SPEED]
    # ...but reset() always rolls get_cd() on the reuse branch.
    assert [op["site"] for op in init["reuse"]] == [
        G.BT_INITSITE_SPEED, G.BT_INITSITE_ATK_RESET]
    assert (init["reuse"][1]["lo"], init["reuse"][1]["hi"]) == (90.0, 90.0)


def test_long_cooldown_marks_the_reset_roll_conditional(tmp_path):
    scene = (
        '[gd_scene load_steps=3 format=2]\n'
        '\n'
        '[ext_resource path="res://enemy_base.tscn" type="PackedScene" id=1]\n'
        '[ext_resource path="%s" type="Script" id=2]\n'
        '\n'
        '[node name="Enemy" instance=ExtResource( 1 )]\n'
        '\n'
        '[node name="AttackBehavior" parent="." index="7"]\n'
        'script = ExtResource( 2 )\n'
        'cooldown = 60.0\n'
        'max_cd_randomization = 10\n'
        'long_cooldown_every_x_shoots = 4\n'
    ) % SCRIPTS["atk_shoot"]
    campaign = build_project(tmp_path, {"longcd.tscn": scene})
    init = extract(campaign, "res://longcd.tscn", is_boss=False, speed_rand=0)

    fresh_ready = [op for op in init["fresh"] if op["site"] == G.BT_INITSITE_ATK_READY]
    assert fresh_ready and not fresh_ready[0]["flags"] & G.BT_INITFLAG_COND_SHOTS
    reset = [op for op in init["reuse"] if op["site"] == G.BT_INITSITE_ATK_RESET]
    assert reset[0]["flags"] & G.BT_INITFLAG_COND_SHOTS
    assert reset[0]["cond"] == 4


# --------------------------------------------------------------------------
# unsupported source branches fail loudly instead of silently emitting nothing
# --------------------------------------------------------------------------
def test_unmodeled_movement_behavior_is_rejected(tmp_path):
    scene = (
        '[gd_scene load_steps=3 format=2]\n'
        '\n'
        '[ext_resource path="res://enemy_base.tscn" type="PackedScene" id=1]\n'
        '[ext_resource path="%s" type="Script" id=2]\n'
        '\n'
        '[node name="Enemy" instance=ExtResource( 1 )]\n'
        '\n'
        '[node name="MovementBehavior" parent="." index="6"]\n'
        'script = ExtResource( 2 )\n'
    ) % SCRIPTS["mov_unknown"]
    campaign = build_project(tmp_path, {"weird.tscn": scene})
    with pytest.raises(ExtractError):
        extract(campaign, "res://weird.tscn", is_boss=False, speed_rand=0)


def test_state_without_the_boss_state_script_is_rejected(tmp_path):
    scene = (
        '[gd_scene load_steps=3 format=2]\n'
        '\n'
        '[ext_resource path="res://enemy_base.tscn" type="PackedScene" id=1]\n'
        '[ext_resource path="%s" type="Script" id=2]\n'
        '\n'
        '[node name="Boss" instance=ExtResource( 1 )]\n'
        '\n'
        '[node name="States" type="Node2D" parent="." index="10"]\n'
        '\n'
        '[node name="State1" type="Node2D" parent="States" index="0"]\n'
        'script = ExtResource( 2 )\n'
        '\n'
        '[node name="MovementBehavior" type="Node2D" parent="States/State1" index="0"]\n'
        'script = ExtResource( 2 )\n'
    ) % SCRIPTS["mov_follow"]
    campaign = build_project(tmp_path, {"badstate.tscn": scene})
    with pytest.raises(ExtractError):
        extract(campaign, "res://badstate.tscn", is_boss=True, speed_rand=0)


# --------------------------------------------------------------------------
# real-source test (opt-in, like test_brotato_item_extract.py)
# --------------------------------------------------------------------------
CAMPAIGN = os.environ.get("BROTATO_CAMPAIGN_ROOT")
DLC = os.environ.get("BROTATO_DLC_ROOT")
needs_source = pytest.mark.skipif(
    not (CAMPAIGN and DLC and Path(CAMPAIGN).is_dir() and Path(DLC).is_dir()),
    reason="set BROTATO_CAMPAIGN_ROOT / BROTATO_DLC_ROOT to the recovered trees")


@needs_source
def test_real_kind_init_spans_are_consistent():
    args = extract_gameplay.argparse.Namespace(campaign_root=CAMPAIGN, dlc_root=DLC)
    data = extract_gameplay.build(args)
    for scene in data["enemy_kinds"]:
        init = data["kind_init"][scene]
        fresh, reuse = init["fresh"], init["reuse"]
        # Every fresh sequence starts with the Enemy idle member draw and
        # contains exactly one shared speed-jitter row.
        assert fresh[0]["site"] == extract_gameplay.BT_INITSITE_MEMBER, scene
        assert (fresh[0]["lo"], fresh[0]["hi"]) == (1.0, 3.0), scene
        assert sum(op["site"] == extract_gameplay.BT_INITSITE_SPEED
                   for op in fresh) == 1, scene
        # Bosses/elites are never pooled: an empty reuse span, otherwise a
        # single speed-jitter row (unit.gd:88) first.
        if data["kind_class"][scene] != 0:
            assert reuse == [], scene
        else:
            assert reuse[0]["site"] == extract_gameplay.BT_INITSITE_SPEED, scene
            assert sum(op["site"] == extract_gameplay.BT_INITSITE_SPEED
                       for op in reuse) == 1, scene
        # Bound references only ever point at the immediately preceding row.
        for i, op in enumerate(fresh):
            for key in ("lo_ref", "hi_ref"):
                if op[key] is not None:
                    assert i > 0, (scene, i, key)
        # Binary64 bounds are always finite, and BT_INITFLAG_ADD is the
        # discriminator for "value = base + draw": it stays set for a legal
        # zero base, so the base VALUE must not be used as the test.
        for op in fresh + reuse:
            assert math.isfinite(op["lo"]) and math.isfinite(op["hi"]), scene
            assert math.isfinite(op["add"]), scene
            if op["site"] in (extract_gameplay.BT_INITSITE_MEMBER,
                              extract_gameplay.BT_INITSITE_ATK_READY,
                              extract_gameplay.BT_INITSITE_ATK_RESET,
                              extract_gameplay.BT_INITSITE_SPEED):
                assert not op["flags"] & extract_gameplay.BT_INITFLAG_ADD, \
                    (scene, op["src"])
                assert op["add"] == 0.0, (scene, op["src"])
            elif op["lo_ref"] is None and op["dest"] in (
                    extract_gameplay.BT_INITDEST_MOV_ACTUAL,
                    extract_gameplay.BT_INITDEST_MOV_ACTUAL_RANGE):
                assert op["flags"] & extract_gameplay.BT_INITFLAG_ADD, \
                    (scene, op["src"])
