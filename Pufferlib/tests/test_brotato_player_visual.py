"""Player visual emission is a gameplay-RNG consumer.

The oracle is the pinned engine's own CPUParticles2D, driven through the source
player's emission lifecycle (wave-1 landing hold, movement toggles, idle lapse
and restart) rather than another copy of the native emission algorithm. The
running smoke (zone 0) and the swimming bubble (zone 1) differ only in amount,
lifetime and emission shape.

What the engine proves here: an `emitting` false->true transition resumes the
global stream from exactly the state the engine kept while emission was paused
(dormant particles keep ageing, and a dormant particle whose stale age already
passed the lifetime re-emits immediately); a pause longer than
`inactive_time > lifetime * 1.2` drops the system back to time/cycle 0; and a
long run wraps mid-flight. Stop/start therefore never burns a fixed number of
samples. The same fixture reruns the schedule after a visual reset to prove a
fresh Player scene starts from the fresh particle state, not from carried ages.
"""
import os
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]
ENGINE = os.environ.get("BROTATO_TEST_ENGINE")
PROJECT = ROOT / "tests/brotato_physics_project"

# running_smoke.tscn / SwimmingBubble in player.tscn (amount, lifetime, shape).
SCENES = {
    0: (6, "0.75", 2),   # RunningSmoke: 6 particles, 0.75 s, EMISSION_SHAPE_RECTANGLE
    1: (10, "0.3", 1),   # SwimmingBubble: 10 particles, 0.3 s, EMISSION_SHAPE_SPHERE
}


@pytest.fixture(scope="module")
def native_visual(tmp_path_factory):
    directory = tmp_path_factory.mktemp("brotato-player-visual")
    source = directory / "visual.cpp"
    source.write_text(r'''
#include "ocean/brotato/brotato_player_visual.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
static BtState state;
static void run(const char* schedule) {
    for (size_t i = 0; i < strlen(schedule); i++) {
        bt_player_fx_step(&state, schedule[i] == '1');
        std::printf("%u\n", bt_rng_next(&state.rng));
    }
}
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    state.zone = std::atoi(argv[1]);
    const char* schedule = argv[2];
    /* Pass 1: a fresh Player scene. Pass 2: a new wave/episode rebuilds it, so
     * the second pass must reproduce the first draw-for-draw from the reset. */
    bt_rng_seed(&state.rng, 1);
    bt_player_visual_reset(&state);
    /* This fixture hand-builds a BtState (it never runs bt_core_init), so it must
     * state the particle-RNG model it asserts: 1 = the source's CPUParticles2D
     * per-particle draws exist, which is bt_core_init's default and what the
     * engine oracle this test compares against actually did. */
    state.particles_rng = 1;
    run(schedule);
    bt_rng_seed(&state.rng, 1);
    bt_player_visual_reset(&state);
    /* This fixture hand-builds a BtState (it never runs bt_core_init), so it must
     * state the particle-RNG model it asserts: 1 = the source's CPUParticles2D
     * per-particle draws exist, which is bt_core_init's default and what the
     * engine oracle this test compares against actually did. */
    state.particles_rng = 1;
    run(schedule);
    return 0;
}
''')
    binary = directory / "visual"
    subprocess.run([os.environ.get("CXX", "clang++"), "-std=c++17", "-O2",
                    "-ffp-contract=off", "-I", str(ROOT), str(source), "-o", str(binary)],
                   check=True, capture_output=True, text=True)
    return binary


def _engine_stream(script_dir: Path, zone: int, schedule: str):
    """Drive a real CPUParticles2D through `schedule` (one char per frame) with
    the scene's particle settings and return the engine's own global stream.

    `emitting` is applied in `_physics_process` so the change lands before the
    particle's idle internal process of that same frame, exactly like
    player.gd: update_animation() runs in the physics step.
    """
    amount, lifetime, shape = SCENES[zone]
    script = script_dir / ("particle_zone%d.gd" % zone)
    script.write_text('''extends SceneTree
class Probe extends Node2D:
	var frames = 0
	var seq = ""
	var particle = null
	func _ready():
		seed(1)
	func _physics_process(_delta):
		if frames < seq.length():
			particle.emitting = (seq[frames] == "1")
	func _process(_delta):
		print("PARTICLE_SAMPLE ", randi())
		frames += 1
		if frames == seq.length():
			get_tree().quit()
func _init():
	var probe = Probe.new()
	probe.seq = "SCHEDULE"
	var particle = CPUParticles2D.new()
	particle.amount = AMOUNT
	particle.lifetime = LIFETIME
	particle.randomness = 0.35
	particle.emission_shape = SHAPE
	particle.emission_rect_extents = Vector2(10, 1)
	particle.emission_sphere_radius = 20.0
	probe.particle = particle
	probe.add_child(particle)
	get_root().add_child(probe)
'''.replace("SCHEDULE", schedule).replace("AMOUNT", str(amount))
   .replace("LIFETIME", lifetime).replace("SHAPE", str(shape)))
    engine = subprocess.run(
        [ENGINE, "--path", str(PROJECT), "--fixed-fps", "60", "--no-window",
         "--audio-driver", "Dummy", "--script", str(script)],
        capture_output=True, text=True, timeout=180)
    assert engine.returncode == 0, engine.stdout + engine.stderr
    return [int(line.removeprefix("PARTICLE_SAMPLE "))
            for line in engine.stdout.splitlines() if line.startswith("PARTICLE_SAMPLE ")]


def _assert_streams_match(native_visual, tmp_path, zone, schedule):
    expected = _engine_stream(tmp_path, zone, schedule)
    native = subprocess.run([str(native_visual), str(zone), schedule],
                            capture_output=True, text=True, timeout=60)
    assert native.returncode == 0, native.stderr
    actual = [int(line) for line in native.stdout.splitlines()]
    assert len(expected) == len(schedule), (len(expected), len(schedule))
    assert len(actual) == 2 * len(schedule), len(actual)
    assert actual[:len(schedule)] == expected
    assert actual[len(schedule):] == expected


# Emission schedules walked frame by frame. They are timeline outlines of the
# source lifecycle, not tuned constants: the engine decides every draw.
LIFECYCLES = {
    # wave-1 landing keeps the smoke stopped; the movement that follows resumes
    # emission after the system has already lapsed and reset (both zones).
    "landing-then-run": "0" * 72 + "1" * 80,
    # a pause longer than lifetime*1.2 stops processing and resets the clock.
    "run-stop-lapse-run": "1" * 30 + "0" * 90 + "1" * 60,
    # a pause shorter than lifetime*1.2 keeps time/cycle and the dormant ages.
    "run-stop-brief-run": "1" * 30 + "0" * 15 + "1" * 60,
    # resuming emission restarts dormant particles whose stale age passed
    # lifetime (zone 1: 0.3 s lifetime over a 20-frame pause).
    "resume-dormant-stale-age": "1" * 10 + "0" * 20 + "1" * 30,
    "double-pause": "1" * 10 + "0" * 20 + "1" * 20 + "0" * 20 + "1" * 80,
    # several lifetime wraps in one continuous run.
    "wrap-many": "1" * 240,
    # movement toggling faster than the emission clock.
    "rapid-toggles": ("1" * 3 + "0" * 3) * 40,
    # pause tails: the schedule ends while emission is off, leaving dormant
    # particles whose binary32 age already rounded past the lifetime. A wave
    # rebuild must not carry those ages into the next scene (the fixture's
    # reset rerun would then restart them one frame early).
    "landing-run-stop-tail": "0" * 72 + "1" * 20 + "0" * 40,
    "pause-tail": "1" * 30 + "0" * 30,
}


@pytest.mark.skipif(not ENGINE, reason="set BROTATO_TEST_ENGINE to pinned Godot 3.7-dev1")
@pytest.mark.parametrize("zone", [0, 1], ids=["running-smoke", "swimming-bubbles"])
def test_particle_emission_preserves_engine_global_stream(native_visual, tmp_path, zone):
    _assert_streams_match(native_visual, tmp_path, zone, "1" * 120)


@pytest.mark.skipif(not ENGINE, reason="set BROTATO_TEST_ENGINE to pinned Godot 3.7-dev1")
@pytest.mark.parametrize("zone", [0, 1], ids=["running-smoke", "swimming-bubbles"])
@pytest.mark.parametrize("name", list(LIFECYCLES))
def test_particle_lifecycle_preserves_engine_global_stream(native_visual, tmp_path, zone, name):
    _assert_streams_match(native_visual, tmp_path, zone, LIFECYCLES[name])


# ---------------------------------------------------------------- footstep audio
# The move/move_swim animations carry method keys at 0.1 s and 0.4 s
# (player_move.tres) and call play_step_sound, which draws the global stream
# once per event (Utils.get_rand_element -> Array.pick_random). How many events
# a movement window produces, and on which frames, is therefore part of the
# gameplay stream; the engine's own AnimationPlayer is the oracle.

ANIM_PLAYBACKS = [1.0, 0.5, 1.5]
# A wave-1 timeline: frames 0..71 are the landing hold (no movement possible),
# then movement windows of different lengths, including the 63-frame window the
# recorded Bull run uses.
ANIM_SCHEDULES = {
    "run-80": "0" * 72 + "1" * 80,
    "run-63": "0" * 72 + "1" * 63 + "0" * 20,
    "run-200": "0" * 72 + "1" * 200,
    "stop-restart": "0" * 72 + "1" * 30 + "0" * 20 + "1" * 40,
    "toggles": "0" * 72 + ("1" * 3 + "0" * 3) * 20,
}


@pytest.fixture(scope="module")
def native_anim(tmp_path_factory):
    directory = tmp_path_factory.mktemp("brotato-player-anim")
    source = directory / "anim.cpp"
    source.write_text(r'''
#include "ocean/brotato/brotato_player_visual.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
static BtState state;
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    state.zone = 0;
    const char* schedule = argv[1];
    float playback = (float)atof(argv[2]);
    bt_rng_seed(&state.rng, 1);
    bt_player_visual_reset(&state);
    /* This fixture hand-builds a BtState (it never runs bt_core_init), so it must
     * state the particle-RNG model it asserts: 1 = the source's CPUParticles2D
     * per-particle draws exist, which is bt_core_init's default and what the
     * engine oracle this test compares against actually did. */
    state.particles_rng = 1;
    /* Wave 1, tick_in_wave one-based like bt_core_step, so the landing gate
     * covers exactly the leading zero frames of every schedule. */
    state.wave = 1;
    for (size_t i = 0; i < strlen(schedule); i++) {
        state.tick_in_wave = (int)i + 1;
        int steps = bt_player_visual_step(&state, schedule[i] == '1', playback);
        bt_player_visual_emit_steps(&state, steps);
        std::printf("%d\n", steps);
    }
    return 0;
}
''')
    binary = directory / "anim"
    subprocess.run([os.environ.get("CXX", "clang++"), "-std=c++17", "-O2",
                    "-ffp-contract=off", "-I", str(ROOT), str(source), "-o", str(binary)],
                   check=True, capture_output=True, text=True)
    return binary


def _engine_step_frames(script_dir: Path, schedule: str, playback: float):
    """Frame -> method-callback count from a real AnimationPlayer running the
    move animation with player.gd's playback_speed rule."""
    script = script_dir / "anim_steps.gd"
    script.write_text('''extends SceneTree
class Probe extends Node2D:
	var proc = 0
	var seq = ""
	var ap = null
	func _ready():
		seed(1)
	func play_step_sound():
		print("STEP ", proc)
	func _physics_process(_delta):
		if proc < seq.length():
			var moving = (seq[proc] == "1")
			var current = ap.get_current_animation()
			if current == "idle":
				ap.playback_speed = 1
			elif current == "move":
				ap.playback_speed = PB
			if current == "idle" and moving:
				ap.play("move")
			elif current == "move" and not moving:
				ap.play("idle")
	func _process(_delta):
		proc += 1
		if proc == seq.length():
			get_tree().quit()
func _init():
	var probe = Probe.new()
	probe.seq = "SCHEDULE"
	var ap = AnimationPlayer.new()
	var move = Animation.new()
	move.length = 0.6
	move.loop = true
	var mid = move.add_track(Animation.TYPE_METHOD)
	move.track_set_path(mid, NodePath("."))
	move.track_insert_key(mid, 0.1, { "method": "play_step_sound", "args": [] })
	move.track_insert_key(mid, 0.4, { "method": "play_step_sound", "args": [] })
	ap.add_animation("move", move)
	var idle = Animation.new()
	idle.length = 0.6
	idle.loop = true
	ap.add_animation("idle", idle)
	probe.ap = ap
	probe.add_child(ap)
	get_root().add_child(probe)
	ap.play("idle")
'''.replace("SCHEDULE", schedule).replace("PB", repr(float(playback))))
    engine = subprocess.run(
        [ENGINE, "--path", str(PROJECT), "--fixed-fps", "60", "--no-window",
         "--audio-driver", "Dummy", "--script", str(script)],
        capture_output=True, text=True, timeout=180)
    assert engine.returncode == 0, engine.stdout + engine.stderr
    # A method callback is queued during the idle internal process and flushed at
    # the end of the same frame, so `proc` has already advanced by one.
    frames = {}
    for line in engine.stdout.splitlines():
        if line.startswith("STEP "):
            frame = int(line.split()[1]) - 1
            frames[frame] = frames.get(frame, 0) + 1
    return frames


@pytest.mark.skipif(not ENGINE, reason="set BROTATO_TEST_ENGINE to pinned Godot 3.7-dev1")
@pytest.mark.parametrize("playback", ANIM_PLAYBACKS)
@pytest.mark.parametrize("name", list(ANIM_SCHEDULES))
def test_animation_step_callbacks_match_engine(native_anim, tmp_path, name, playback):
    schedule = ANIM_SCHEDULES[name]
    expected = _engine_step_frames(tmp_path, schedule, playback)
    native = subprocess.run([str(native_anim), schedule, repr(float(playback))],
                            capture_output=True, text=True, timeout=60)
    assert native.returncode == 0, native.stderr
    actual = {i: int(v) for i, v in enumerate(native.stdout.splitlines()) if int(v)}
    assert actual == expected
