"""Tests for ocean/brotato/extract_audio.py.

Two halves, mirroring tests/test_brotato_weapon_extract.py:

* Hermetic tests over a tiny synthetic Godot project -- they pin the parts that
  are easy to get wrong: packed-scene inheritance ownership of an exported
  sound array (a base scene's ExtResource ids must resolve against the BASE
  file, not the derived one), the empty-array / direct-Resource property forms,
  the clip-span interning and the fail-loud behaviour on a missing clip.
* A real-source test that runs only when ``BROTATO_CAMPAIGN_ROOT`` /
  ``BROTATO_DLC_ROOT`` point at the recovered campaign and Abyss DLC trees.
"""

from __future__ import annotations

import importlib.util
import os
import struct
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
_MODULE_PATH = ROOT / "ocean" / "brotato" / "extract_audio.py"
_spec = importlib.util.spec_from_file_location("extract_audio_under_test", _MODULE_PATH)
extract_audio = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(extract_audio)

ExtractError = extract_audio.ExtractError


def _write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def _wav_bytes(rate: int, channels: int, frames: int) -> bytes:
    data = b"\x00\x00" * channels * frames
    return (
        b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE"
        + b"fmt " + struct.pack("<IHHIIHH", 16, 1, channels, rate,
                                rate * channels * 2, channels * 2, 16)
        + b"data" + struct.pack("<I", len(data))
    ) + data


HURT_A = "res://entities/units/unit/hurt_sounds/punch_a.wav"
HURT_B = "res://entities/units/unit/hurt_sounds/punch_b.wav"
DODGE = "res://entities/units/unit/dodge_sounds/whoosh.wav"
BURN = "res://resources/sounds/burn/burn_a.wav"
BOSS_HURT = "res://entities/units/enemies/boss/hurt_special.wav"


UNIT_TSCN = """\
[gd_scene load_steps=5 format=2]

[ext_resource path="%s" type="AudioStream" id=1]
[ext_resource path="%s" type="AudioStream" id=2]
[ext_resource path="%s" type="AudioStream" id=3]
[ext_resource path="%s" type="AudioStream" id=4]

[node name="Unit" type="RigidBody2D"]
crit_sounds = [  ]
hurt_sounds = [ ExtResource( 1 ), ExtResource( 2 ) ]
burn_sounds = [ ExtResource( 4 ) ]
dodge_sounds = [ ExtResource( 3 ) ]
""" % (HURT_A, HURT_B, DODGE, BURN)

# Inherits unit.tscn and redefines NOTHING: the array values are owned by
# unit.tscn and must be resolved against unit.tscn's ExtResource table.  The
# derived file's ExtResource( 1 ) is this very scene, so resolving against the
# derived file would silently yield "res://entities/units/unit/unit.tscn".
ENEMY_TSCN = """\
[gd_scene load_steps=2 format=2]

[ext_resource path="res://entities/units/unit/unit.tscn" type="PackedScene" id=1]

[node name="Enemy" instance=ExtResource( 1 )]
"""

BOSS_TSCN = """\
[gd_scene load_steps=3 format=2]

[ext_resource path="res://entities/units/enemies/enemy.tscn" type="PackedScene" id=1]
[ext_resource path="%s" type="AudioStream" id=2]

[node name="Boss" instance=ExtResource( 1 )]
hurt_sounds = [ ExtResource( 2 ) ]
""" % (BOSS_HURT,)

MAIN_TSCN = """\
[gd_scene load_steps=4 format=2]

[ext_resource path="res://resources/sounds/level_up.wav" type="AudioStream" id=1]
[ext_resource path="res://items/materials/drip_01.wav" type="AudioStream" id=2]
[ext_resource path="res://items/materials/drip_02.wav" type="AudioStream" id=3]

[node name="Main" type="Node"]
gold_pickup_sounds = [ ExtResource( 2 ), ExtResource( 3 ) ]
level_up_sound = ExtResource( 1 )
run_won_sounds = [  ]
"""

STATS_TRES = """\
[gd_resource type="Resource" format=2]

[ext_resource path="res://weapons/ranged/smg/shot_01.wav" type="AudioStream" id=2]

[resource]
cooldown = 60
shooting_sounds = [ ExtResource( 2 ), ExtResource( 2 ) ]
"""

EMPTY_STATS_TRES = """\
[gd_resource type="Resource" format=2]

[resource]
cooldown = 60
shooting_sounds = [  ]
"""

EXPLOSION_TSCN = """\
[gd_scene load_steps=3 format=2]

[ext_resource path="res://projectiles/rocket/boom_01.wav" type="AudioStream" id=1]
[ext_resource path="res://projectiles/rocket/boom_02.wav" type="AudioStream" id=2]

[node name="PlayerExplosion" type="Node2D"]
explosion_sounds = [ ExtResource( 2 ), ExtResource( 1 ) ]
"""

WEAPON_SERVICE_GD = """\
extends Node

var breaking_sounds: Array = [
\tpreload("res://weapons/melee_sounds/break_01.wav"), 
\tpreload("res://weapons/melee_sounds/break_02.wav"), 
]
"""


def _minor_project(tmp_path: Path):
    campaign = tmp_path / "campaign"
    dlc = tmp_path / "dlc"
    _write(campaign / "entities/units/unit/unit.tscn", UNIT_TSCN)
    _write(campaign / "entities/units/enemies/enemy.tscn", ENEMY_TSCN)
    _write(campaign / "entities/units/enemies/boss/boss.tscn", BOSS_TSCN)
    _write(campaign / "main.tscn", MAIN_TSCN)
    _write(campaign / "weapons/weapon_stats/small_stats.tres", STATS_TRES)
    _write(campaign / "weapons/weapon_stats/empty_stats.tres", EMPTY_STATS_TRES)
    _write(campaign / "singletons/weapon_service.gd", WEAPON_SERVICE_GD)
    _write(campaign / "projectiles/explosion.tscn", EXPLOSION_TSCN)
    for res_path, rate, channels, frames in (
            (HURT_A, 44100, 1, 44100),
            (HURT_B, 48000, 2, 4800),
            (DODGE, 44100, 1, 8820),
            (BURN, 22050, 1, 11025),
            (BOSS_HURT, 44100, 1, 22050),
            ("res://resources/sounds/level_up.wav", 44100, 1, 44100),
            ("res://items/materials/drip_01.wav", 44100, 1, 4410),
            ("res://items/materials/drip_02.wav", 44100, 1, 2205),
            ("res://weapons/ranged/smg/shot_01.wav", 44100, 1, 4410),
            ("res://weapons/melee_sounds/break_01.wav", 44100, 1, 4410),
            ("res://weapons/melee_sounds/break_02.wav", 44100, 1, 8820),
            ("res://projectiles/rocket/boom_01.wav", 44100, 1, 4410),
            ("res://projectiles/rocket/boom_02.wav", 44100, 1, 8820)):
        _write_bytes(campaign / res_path[len("res://"):],
                     _wav_bytes(rate, channels, frames))
    resolver = extract_audio.physics.Resolver([str(campaign), str(dlc)])
    return resolver


def _write_bytes(path: Path, blob: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(blob)


# --------------------------------------------------------------------------
# hermetic tests
# --------------------------------------------------------------------------
def test_scene_inheritance_resolves_against_the_owning_file(tmp_path):
    resolver = _minor_project(tmp_path)
    enemy = "res://entities/units/enemies/enemy.tscn"
    assert extract_audio.scene_sound_array(resolver, enemy, "hurt_sounds") == [HURT_A, HURT_B]
    assert extract_audio.scene_sound_array(resolver, enemy, "burn_sounds") == [BURN]
    # dodge_sounds only exists on unit.tscn, two inheritance hops up.
    assert extract_audio.scene_sound_array(resolver, enemy, "dodge_sounds") == [DODGE]
    # an empty array literal is a real value (count 0), not "undefined".
    assert extract_audio.scene_sound_array(resolver, enemy, "crit_sounds") == []


def test_derived_scene_override_wins(tmp_path):
    resolver = _minor_project(tmp_path)
    boss = "res://entities/units/enemies/boss/boss.tscn"
    assert extract_audio.scene_sound_array(resolver, boss, "hurt_sounds") == [BOSS_HURT]
    # everything it does not override still comes from the base.
    assert extract_audio.scene_sound_array(resolver, boss, "burn_sounds") == [BURN]
    assert extract_audio.scene_sound_array(resolver, boss, "dodge_sounds") == [DODGE]


def test_undefined_property_is_empty_and_direct_resource_is_one_clip(tmp_path):
    resolver = _minor_project(tmp_path)
    main = "res://main.tscn"
    assert extract_audio.scene_sound_array(resolver, main, "gold_pickup_sounds") == [
        "res://items/materials/drip_01.wav", "res://items/materials/drip_02.wav"]
    # a bare `ExtResource( n )` is the DIRECT single-Resource form (no pick).
    assert extract_audio.scene_sound_array(resolver, main, "level_up_sound") == [
        "res://resources/sounds/level_up.wav"]
    assert extract_audio.scene_sound_array(resolver, main, "run_won_sounds") == []
    assert extract_audio.scene_sound_array(resolver, main, "run_lost_sounds") == []


def test_resource_and_script_arrays(tmp_path):
    resolver = _minor_project(tmp_path)
    assert extract_audio.resource_sound_array(
        resolver, "res://weapons/weapon_stats/small_stats.tres", "shooting_sounds") == [
        "res://weapons/ranged/smg/shot_01.wav", "res://weapons/ranged/smg/shot_01.wav"]
    assert extract_audio.resource_sound_array(
        resolver, "res://weapons/weapon_stats/empty_stats.tres", "shooting_sounds") == []
    assert extract_audio.script_preload_array(
        resolver, "res://singletons/weapon_service.gd", "breaking_sounds") == [
        "res://weapons/melee_sounds/break_01.wav", "res://weapons/melee_sounds/break_02.wav"]


def test_missing_clip_file_fails_loudly(tmp_path):
    resolver = _minor_project(tmp_path)
    _write(tmp_path / "campaign/entities/units/unit/unit.tscn",
           UNIT_TSCN.replace('path="%s" type="AudioStream" id=3' % DODGE,
                             'path="res://entities/units/unit/dodge_sounds/gone.wav"'
                             ' type="AudioStream" id=3'))
    clips = extract_audio.scene_sound_array(
        resolver, "res://entities/units/unit/unit.tscn", "dodge_sounds")
    assert clips == ["res://entities/units/unit/dodge_sounds/gone.wav"]
    table = extract_audio.ClipTable(resolver)
    with pytest.raises(ExtractError):
        table.span(clips)


def test_clip_span_interning_and_durations(tmp_path):
    resolver = _minor_project(tmp_path)
    table = extract_audio.ClipTable(resolver)
    assert table.span([HURT_A, HURT_B]) == (0, 2)
    # the same ordered sequence is interned: the unit arrays cost 2 rows, not
    # 2 * (number of scenes that inherit them).
    assert table.span([HURT_A, HURT_B]) == (0, 2)
    assert table.paths == [HURT_A, HURT_B]
    # a different sequence appends a contiguous run of its own.
    third = "res://resources/sounds/burn/burn_a.wav"
    assert table.span([HURT_B, third]) == (2, 2)
    assert table.paths[2:4] == [HURT_B, third]
    # an empty sequence never allocates a row and takes no RNG draw.
    assert table.span([]) == (0, 0)
    assert len(table.paths) == 4
    # true lengths come from the real bytes.
    assert table.duration(HURT_A) == pytest.approx(1.0)
    # 4800 frames * 2 channels * 2 bytes @ 48000 Hz * 2 ch = 0.1 s
    assert table.duration(HURT_B) == pytest.approx(0.1)


def test_missing_clip_rejected_when_interning(tmp_path):
    resolver = _minor_project(tmp_path)
    table = extract_audio.ClipTable(resolver)
    with pytest.raises(ExtractError):
        table.span(["res://resources/sounds/nope.wav"])


def test_float32_literal_round_trips():
    for value in (0.0, 1.0, 0.1, 0.625729144, 5.20800018, 1.0 / 3.0):
        text = extract_audio._f32_seconds(value)
        assert text.endswith("f")
        back = struct.unpack("<f", struct.pack("<f", float(text[:-1])))[0]
        assert back == struct.unpack("<f", struct.pack("<f", float(value)))[0]


def test_explosion_node_sounds_and_scene_discovery(tmp_path):
    resolver = _minor_project(tmp_path)
    assert extract_audio.scene_sound_array(
        resolver, "res://projectiles/explosion.tscn", "explosion_sounds") == [
        "res://projectiles/rocket/boom_02.wav", "res://projectiles/rocket/boom_01.wav"]
    # discovery reads the .tscn root exports straight off disk.
    assert extract_audio.scenes_with_prop(resolver.roots, "explosion_sounds") == [
        "res://projectiles/explosion.tscn"]
    assert extract_audio.scenes_with_prop(resolver.roots, "dodge_sounds") == [
        "res://entities/units/unit/unit.tscn"]


def test_event_enum_is_dense_and_named():
    names = [name for name, _ in extract_audio.EVENTS]
    assert len(set(names)) == len(names)
    assert all(name.startswith("BT_AUDIO_EV_") for name in names)
    assert extract_audio.EVENT_INDEX["BT_AUDIO_EV_PLAYER_HURT"] == 0
    assert max(extract_audio.EVENT_INDEX.values()) == len(names) - 1


# --------------------------------------------------------------------------
# real-source test (opt-in)
# --------------------------------------------------------------------------
CAMPAIGN = os.environ.get("BROTATO_CAMPAIGN_ROOT")
DLC = os.environ.get("BROTATO_DLC_ROOT")
needs_source = pytest.mark.skipif(
    not (CAMPAIGN and DLC and Path(CAMPAIGN).is_dir() and Path(DLC).is_dir()),
    reason="set BROTATO_CAMPAIGN_ROOT / BROTATO_DLC_ROOT to the recovered trees")


@needs_source
def test_real_audio_events_and_spans(tmp_path):
    out = tmp_path / "brotato_audio_data.h"
    prov = tmp_path / "audio.json"
    args = extract_audio.argparse.Namespace(
        campaign_root=CAMPAIGN, dlc_root=DLC, out=str(out), provenance_out=str(prov))
    data = extract_audio.extract(args)
    extract_audio.emit_audio_data(data, str(out))
    extract_audio.emit_provenance(data, str(prov), args)

    resolver = data["resolver"]
    events = {e["name"]: e for e in data["report"]["events"]}
    # source array sizes (campaign + DLC), straight off the .tscn / .gd sources.
    assert events["BT_AUDIO_EV_PLAYER_HURT"]["count"] == 4
    assert events["BT_AUDIO_EV_PLAYER_DODGE"]["count"] == 2
    assert events["BT_AUDIO_EV_HP_REGEN"]["count"] == 2
    assert events["BT_AUDIO_EV_ALIEN_EYES"]["count"] == 2
    assert events["BT_AUDIO_EV_GOLD_PICKUP"]["count"] == 5
    assert events["BT_AUDIO_EV_GOLD_ALT_PICKUP"]["count"] == 5
    assert events["BT_AUDIO_EV_LEVEL_UP"]["count"] == 1
    assert events["BT_AUDIO_EV_END_WAVE"]["count"] == 1
    assert events["BT_AUDIO_EV_RUN_WON"]["count"] == 1
    assert events["BT_AUDIO_EV_RUN_LOST"]["count"] == 9
    assert events["BT_AUDIO_EV_WEAPON_BREAK"]["count"] == 3
    assert events["BT_AUDIO_EV_ONE_SHOT"]["count"] == 1
    assert events["BT_AUDIO_EV_BULL_EXPLODE"]["count"] == 0
    # PlayerExplosion NODES own their own scene-root sound array.
    assert events["BT_AUDIO_EV_EXPLOSION_NODE"]["count"] == 4
    assert events["BT_AUDIO_EV_EXPLOSION_NODE"]["clips"] == [
        "res://projectiles/rocket/explosion_small_no_tail_01.wav",
        "res://projectiles/rocket/explosion_small_no_tail_02.wav",
        "res://projectiles/rocket/explosion_small_no_tail_03.wav",
        "res://projectiles/rocket/explosion_small_short_01.wav"]
    assert events["BT_AUDIO_EV_FLESH_EXPLOSION_NODE"]["count"] == 2
    assert events["BT_AUDIO_EV_FLESH_EXPLOSION_NODE"]["clips"] == [
        "res://projectiles/flesh_explosion/bullet_impact_body_flesh_01.wav",
        "res://projectiles/flesh_explosion/bullet_impact_body_flesh_02.wav"]
    assert extract_audio.scenes_with_prop(resolver.roots, "explosion_sounds") == [
        scene for _, scene in extract_audio.EXPLOSION_SCENES]

    # scene-keyed view for the device runtime: one row per BT_ITEM_SCENES row,
    # only the two PlayerExplosion scenes non-zero, matching the named events.
    paths = data["scene_row_paths"]
    spans = data["explnode_spans"]
    assert len(spans) == 50 == len(paths)
    nonzero = {paths[i]: spans[i] for i in range(len(spans)) if spans[i][1]}
    assert nonzero == {
        "res://projectiles/explosion.tscn": data["events"]["BT_AUDIO_EV_EXPLOSION_NODE"],
        "res://projectiles/flesh_explosion/flesh_explosion.tscn":
            data["events"]["BT_AUDIO_EV_FLESH_EXPLOSION_NODE"],
    }

    # the one reachable one-shot clip is the vorpal sword mp3, shared by every
    # OneShotOnHitEffect row.
    assert events["BT_AUDIO_EV_ONE_SHOT"]["clips"] == [
        "res://weapons/melee/vorpal_sword/vorpal_one_shot.mp3"]
    for row in data["report"]["one_shot_rows"]:
        assert row["clips"] == events["BT_AUDIO_EV_ONE_SHOT"]["clips"]

    # LEVEL_UP is a direct Resource play; ONE_SHOT a direct AudioStream play.
    assert events["BT_AUDIO_EV_LEVEL_UP"]["direct"] and events["BT_AUDIO_EV_ONE_SHOT"]["direct"]
    assert not events["BT_AUDIO_EV_RUN_WON"]["direct"]

    # every enemy kind + neutral resolves the unit.tscn arrays through scene
    # inheritance; no shipped scene overrides them.
    assert len(data["kind_spans"]) == 66
    assert len({(k["hurt"][0], k["crit"][0], k["burn"][0]) for k in data["report"]["kinds"]}) == 1
    for kind in data["report"]["kinds"]:
        assert len(kind["hurt"]) == 8 and len(kind["crit"]) == 4 and len(kind["burn"]) == 4
    for neutral in data["report"]["neutrals"]:
        assert len(neutral["hurt"]) == 8 and len(neutral["crit"]) == 4 and len(neutral["burn"]) == 4

    # BT_ITEM_WSTATS index space: the sunken bell explosion ships one clip.
    wstats = {w["index"]: w for w in data["report"]["wstats"]}
    assert len(data["wstats_spans"]) == 39
    assert wstats[21]["resource"].endswith("sunken_bell_explosion_stats.tres")
    assert wstats[21]["clips"] == ["res://dlcs/dlc_1/items/sunken_bell/bell_large_ringing_01.wav"]
    assert wstats[38]["clips"] == ["res://weapons/ranged/taser/taser.wav"]

    # every published clip exists and has a positive true length.
    for row in data["report"]["events"]:
        for clip in row["clips"]:
            assert Path(resolver.find(clip)).is_file()
    table = data["table"]
    for index in range(len(table.paths)):
        assert table.paths[index].startswith("res://")
    assert all(seconds > 0.0 for seconds in (table.duration(p) for p in table.paths))

    assert "#define BT_NAUDIO_CLIPS %d" % len(table.paths) in out.read_text()
