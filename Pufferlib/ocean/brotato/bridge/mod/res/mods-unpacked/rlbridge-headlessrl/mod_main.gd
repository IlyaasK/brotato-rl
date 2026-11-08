extends Node
# rlbridge-headlessrl - RL bridge entry point, loaded by ModLoader from
# res://mods-unpacked/rlbridge-headlessrl/mod_main.gd.
#
# What it does, in order:
#   1. During ModLoader initialisation (_init) it prepares action injection for
#      player 0 (rl_inject.gd): movement is applied by pressing/releasing the four
#      analog actions the original movement behavior reads.
#   2. Defers everything else to the first idle frame, because ModLoader is
#      autoload #6 while ItemService is #11 and RunData is #14: it polls for the
#      autoloads it needs.
#   3. Repairs and applies the loaded settings (rl_env.gd), so a save file that
#      carries JSON nulls cannot break ProgressData.apply_settings() or
#      RunData.reset().
#   4. Loads the core observation tables (rl_obs.gd) and, optionally, the
#      exported policy (rl_policy.gd).
#   5. Boots the run through the game's own RunData API (rl_run.gd) and switches
#      to the arena scene.
#   6. Runs the episode across phases through the game's own handlers:
#        phase 0 (arena combat)  - inject a movement direction each tick
#        phase 2 (arena level-up) - choose an offer via the UpgradesUI container
#        phase 1 (shop scene)     - buy a slot / reroll / skip / recycle / combine
#      Records describe one completed physics step: the state is post-step,
#      `action` is the action that was requested for that step and
#      `action_applied` reports whether the game registered it.
#   7. Ends on the tick budget (or when the run leaves arena and shop) with a
#      summary plus diagnostics, and quits so a harness gets the process back.
#
# Honesty rules: no observation component is padded with invented numbers; the
# only "absent" encodings are the core's own (zeroed enemy/projectile/
# consumable/weapon slots, and BT_ITEMS[0]/BT_UPGRADES[0] features for empty shop
# and level-up slots, which is what bt_core_observe emits with shop[i]=0).  The
# full observation (v5) adds six owned-weapon records, four typed shop offers,
# two profile fields, actionable shop prices and eligibility, the Demon/endless
# block and the two v5 mask fields (ban budget + level-up presence) (rl_obs.gd).
# `action_applied` is 1 only when the engine registered the requested action:
# for movement, the native analog actions were pressed and the player's own
# `_current_movement` matched; for shop/level-up, the game's own handler ran and
# the phase-specific post-condition changed.

const RLCfg := preload("res://mods-unpacked/rlbridge-headlessrl/rl_cfg.gd")
const RLRun := preload("res://mods-unpacked/rlbridge-headlessrl/rl_run.gd")
const RLTrace := preload("res://mods-unpacked/rlbridge-headlessrl/rl_trace.gd")
const RLEnv := preload("res://mods-unpacked/rlbridge-headlessrl/rl_env.gd")
const RLPolicy := preload("res://mods-unpacked/rlbridge-headlessrl/rl_policy.gd")
const RLObs := preload("res://mods-unpacked/rlbridge-headlessrl/rl_obs.gd")
const RLInject := preload("res://mods-unpacked/rlbridge-headlessrl/rl_inject.gd")
const RLProgress := preload("res://mods-unpacked/rlbridge-headlessrl/rl_progress.gd")

const BRIDGE_VERSION := "1.6.0"

const REQUIRED_AUTOLOADS := [
	"Keys", "Platform", "ItemService", "ZoneService", "RunData",
	"ProgressData", "MenuData", "DebugService"
]

const ARENA_SCENE_HINT := "Main"
const SHOP_SCENE_HINT := "Shop"

const PHASE_COMBAT := 0
const PHASE_SHOP := 1
const PHASE_LEVEL_UP := 2

var config := {}

var _trace = null
var _obs = null
var _inject = null
var _policy = null
var _progress = null

var _booted := false
var _finished := false
var _boot_frames := 0
var _ticks := 0
var _arena_ticks := 0
var _samples := 0
var _boot_tick := -1
var _run_result := {}
var _settings_report := {}
var _obs_report := {}
var _inject_report := {}
var _failure := ""
var _failure_message := ""
var _scenes_seen := []
var _player_seen := false
var _first_enemy_tick := -1
var _max_enemy_nodes := 0
var _max_living_enemies := 0
var _hp_first := -1
var _hp_last := -1
var _hp_min := -1
var _max_hp_seen := -1
var _arena_loaded_tick := -1
var _arena_snapshots := []
var _stall_warned := false
var _last_position := Vector2.ZERO
var _wave_seen := -1
var _out_of_scene_ticks := 0
var _shop_ticks := 0
var _level_up_ticks := 0
var _reroll_budget := 1
var _shop_script_index := 0
var _shop_script_tick := 0
var _pending_action_valid := false
var _prev_phase := -1
var _incomplete_rows := 0
var _exit_code := 0
var _dlc_preload := {}
var _movement_probe := {}
var _scene_preload_done := false
var _phase_counts := {"combat": 0, "shop": 0, "level_up": 0}

# --- full unlock (#1) ------------------------------------------------------
var _full_unlock_report := {}
# --- RNG diagnostics (#2/#3) ----------------------------------------------
var _rng_diag_supported := false
var _rng_math_last := 0
var _rng_utils_last := 0
var _rng_counters_used := false
var _rng_math_final := -1
var _rng_utils_final := -1
# --- effect-RNG site forcing (#3) -----------------------------------------
var _effect_rng_off := {}
var _sound_queue_clears := 0
var _floating_text_pins := 0
var _level_up_script_index := 0
var _level_up_script_tick := 0
var _grants_report := {}
var _shop_pool_report := {}
# Determinism probe (BROTATO_RL_ENEMY_DIAG): lazily opened per-run dump file.
var _enemy_diag_file = null
var _enemy_diag_rows := 0
# ... and the Hurtbox `area_entered` order log (the input the 6-nearest hit
# resolution of a one-hit projectile actually depends on).
var _enemy_diag_events_file = null
var _enemy_diag_events := 0
# Deterministic damage resolution (see `_on_hurt_ordered`).
var _hurt_order := false
var _hurt_queue: Array = []
var _hurt_flush_scheduled := false
var _hurt_resolved := 0
var _hurt_units_taken_over := 0
# CPUParticles2D nodes seen so far; emission is re-suppressed every idle frame
# (scene/2d/cpu_particles_2d.cpp:863-911 draws Math::rand()/Math::randf() per
# emitted particle, i.e. from the GLOBAL stream the core does not model).
var _particle_nodes := []
var _particle_emissions_stopped := 0
var _particle_frames := 0
# One-shot record of the render configuration the game actually runs with: the
# arena's gold-bag position is main.gd:1067-1072
# `get_viewport().get_canvas_transform().affine_inverse().xform(_ui_bonus_gold_pos.global_position)`,
# so the sim needs these values pinned rather than re-derived per run.
var _viewport_probe := {}
# Wave-end probe window state (bag-anchor validation): the expiry tick and the
# offsets already recorded for the current wave.
var _wave_end_tick := -1
var _wave_end_probe_offsets := []
var _wave_end_probes := []
# Per-row evidence for the progression drivers (#5): the driver's kind plus the
# post-action state the verifier snapshotted, attached to the next row written.
var _last_shop_row := {}
var _last_level_up_row := {}


func _init() -> void:
	# Keep sampling (and eventually quitting) even if something pauses the tree.
	pause_mode = Node.PAUSE_MODE_PROCESS
	config = RLCfg.from_env()
	prints("RLBRIDGE: rlbridge-headlessrl instantiated; BROTATO_RL =", str(config["enabled"]))
	if not config["enabled"]:
		return
	prints("RLBRIDGE: config", JSON.print(config))

	# Action injection for player 0 is prepared here, while ModLoader is still
	# initialising, i.e. before the arena scene or any Player node exists.
	# Under this engine's editor feature both Utils.get_game_dir() and
	# _ModLoaderPath.get_local_folder_dir() resolve to "res://", so the game's own
	# ProgressData.load_dlc_pcks() looks for the DLC pack INSIDE the main pack
	# (res://BrotatoAbyssalTerrors.pck), which cannot be loaded - and without the
	# DLC character_bull does not exist at all.  We therefore load the retail DLC
	# pack ourselves from its real path before ProgressData (#9) runs: this _init
	# executes during ModLoader (#6) initialisation, i.e. earlier.  The path comes
	# from BROTATO_RL_DLC_PCK (staged by the launcher); when the game can already
	# see res://dlcs/ (non-editor run) this is a no-op.
	_preload_dlc_pack()

	_inject = RLInject.new()
	_inject.configure(config)


func _preload_dlc_pack() -> void:
	var directory := Directory.new()
	var already := directory.dir_exists("res://dlcs/")
	var path := str(config.get("dlc_pck", ""))
	_dlc_preload = {
		"requested_path": path,
		"editor_feature": OS.has_feature("editor"),
		# Utils is autoload #7 and we are running inside #6, so its
		# get_game_dir() cannot be called yet; this mirrors it exactly.
		"game_dir": _game_dir_now(),
		"dlcs_dir_before": already
	}
	if already:
		_dlc_preload["action"] = "skipped: res://dlcs/ already available"
		prints("RLBRIDGE: DLC pack preload skipped;", JSON.print(_dlc_preload))
		return
	var file := File.new()
	if path.empty() or not file.file_exists(path):
		_dlc_preload["action"] = "not attempted: BROTATO_RL_DLC_PCK missing or unreadable"
		printerr("RLBRIDGE: DLC pack preload cannot run:", JSON.print(_dlc_preload))
		return
	var ok: bool = ProjectSettings.load_resource_pack(path)
	_dlc_preload["loaded"] = ok
	_dlc_preload["dlcs_dir_after"] = directory.dir_exists("res://dlcs/")
	_dlc_preload["action"] = "ProjectSettings.load_resource_pack"
	_dlc_preload["path_size"] = file.get_len() if file.open(path, File.READ) == OK else -1
	prints("RLBRIDGE: DLC pack preload ->", JSON.print(_dlc_preload))


func _game_dir_now() -> String:
	var game_dir: String = OS.get_executable_path().get_base_dir()
	if OS.get_name() == "OSX":
		game_dir = game_dir.get_base_dir().get_base_dir()
		if game_dir.ends_with(".app"):
			game_dir = game_dir.get_base_dir()
	if OS.has_feature("editor"):
		game_dir = "res://"
	return game_dir


func _ready() -> void:
	if not config["enabled"]:
		return
	# Autoload ORDER is what makes the full-unlock forcing possible here:
	# Godot's main.cpp instantiates EVERY autoload and binds it as a global
	# constant BEFORE any of them is added to the tree ("defer so references are
	# all valid on _ready()"), and it adds them in project.godot order.  This
	# node is a child of ModLoader (autoload #7), so its _ready() runs while
	# ModLoader enters the tree -- after every autoload object exists (including
	# DebugService, #25) but before ProgressData (#10) runs its own _ready().
	_force_pre_boot_state()
	# Every CPUParticles2D, wherever and whenever it is created (weapon scenes per
	# wave, pools, death/explosion effects), is registered here so its emission can
	# be suppressed before its own internal process runs (this node is inside
	# ModLoader, autoload #7, i.e. ahead of every arena node in tree order).
	var tree := get_tree()
	if tree != null and not tree.is_connected("node_added", self, "_on_node_added"):
		tree.connect("node_added", self, "_on_node_added")
	if tree != null and not tree.is_connected("idle_frame", self, "_on_idle_frame"):
		# SceneTree::idle() emits idle_frame BEFORE it walks the idle_process_internal
		# group (scene_tree.cpp:658 vs :666) and before the draw pass, so clearing
		# `emitting` here lands before CPUParticles2D::_particles_process runs that
		# frame whatever a gameplay script re-enabled during physics.
		tree.connect("idle_frame", self, "_on_idle_frame")
	_register_particles_tree(tree.get_root(), 0)
	call_deferred("_bootstrap_step")


func _on_idle_frame() -> void:
	_suppress_particle_emission()


# Records the render configuration once, on the first arena tick: viewport and
# window size, the 2d-stretch mode, the canvas transform the Camera2D installs
# (scale == zoom) and the HUD anchor main.gd:1067-1072 uses.  `gold_bag_pos` is
# read through the game's own accessor, so the recorded number is the game's.
func _probe_viewport(scene) -> void:
	if not _viewport_probe.empty() or scene == null:
		return
	_viewport_probe = _viewport_snapshot(scene)
	prints("RLBRIDGE: viewport probe ->", JSON.print(_viewport_probe))


# The bag anchor can only be validated in the frames the bonus-gold bag actually
# flies in, so the same snapshot is taken at the wave expiry tick and at +60/+119
# ticks after it (the bag window ProgressionGaps needs), together with the live
# bag node position the bag itself uses.
func _probe_wave_end(scene, wave_timer, player = null) -> void:
	if scene == null:
		return
	var time_left := observed_wave_time_left(wave_timer)
	if time_left <= 0.0 and _wave_end_tick < 0:
		_wave_end_tick = _ticks
	if _wave_end_tick < 0:
		return
	var offset := _ticks - _wave_end_tick
	if offset != 0 and offset != 60 and offset != 119:
		return
	if _wave_end_probe_offsets.has(offset):
		return
	_wave_end_probe_offsets.push_back(offset)
	var snapshot := _viewport_snapshot(scene)
	snapshot["tick"] = _ticks
	snapshot["offset_from_wave_end"] = offset
	snapshot["wave"] = RunData.current_wave
	snapshot["wave_time_left"] = time_left
	snapshot["wave_in_progress"] = 1 if RunData.wave_in_progress else 0
	var bag = scene.get("_gold_bag")
	snapshot["gold_bag_node_pos"] = str(bag.global_position) if (bag != null and is_instance_valid(bag)) else "<none>"
	# The player position lets the sim validate its own assumption that the plain
	# Camera2D follows the player (camera == player, zoom (1,1)) from the same row
	# instead of matching ticks against the trace.
	if player != null and is_instance_valid(player):
		snapshot["player_global_position"] = str(player.global_position)
	else:
		snapshot["player_global_position"] = "<none>"
	_wave_end_probes.push_back(snapshot)
	prints("RLBRIDGE: wave-end viewport probe ->", JSON.print(snapshot))


func _viewport_snapshot(scene) -> Dictionary:
	if scene == null:
		return {}
	var viewport = get_viewport()
	var report := {}
	if viewport != null:
		var transform: Transform2D = viewport.get_canvas_transform()
		report["visible_rect_size"] = str(viewport.get_visible_rect().size)
		report["canvas_transform"] = str(transform)
		report["canvas_scale"] = str(transform.get_scale())
		report["canvas_origin"] = str(transform.get_origin())
		report["canvas_inverse_origin"] = str(transform.affine_inverse().get_origin())
		report["size_override"] = str(viewport.get_size_override())
	report["window_size"] = str(OS.get_window_size())
	report["screen_size"] = str(OS.get_screen_size())
	report["stretch_mode"] = ProjectSettings.get_setting("display/window/stretch/mode")
	report["stretch_aspect"] = ProjectSettings.get_setting("display/window/stretch/aspect")
	report["project_window_size"] = "%sx%s" % [
		str(ProjectSettings.get_setting("display/window/size/width")),
		str(ProjectSettings.get_setting("display/window/size/height"))]
	report["video_driver"] = OS.get_video_driver_name(OS.get_current_video_driver())
	var camera = scene.get_node_or_null("Camera")
	if camera != null and camera is Camera2D:
		report["camera_path"] = str(camera.get_path())
		report["camera_zoom"] = str(camera.zoom)
		report["camera_offset"] = str(camera.offset)
		report["camera_global_position"] = str(camera.global_position)
		report["camera_anchor_mode"] = int(camera.anchor_mode)
	var hud_anchor = scene.get_node_or_null("UI/HUD/LifeContainerP1/UIBonusGold/Position2D")
	if hud_anchor != null and hud_anchor is Node2D:
		report["ui_bonus_gold_pos"] = str(hud_anchor.global_position)
	if scene.has_method("get_gold_bag_pos"):
		report["gold_bag_pos"] = str(scene.get_gold_bag_pos())
	return report


func _on_node_added(node: Node) -> void:
	_register_particle_node(node)
	_register_hurt_diag(node)
	_install_hurt_order(node)


# --- deterministic damage resolution --------------------------------------
# unit.gd:610-611 answers every Hurtbox `area_entered` with
# `call_deferred("hurt_area_entered_deferred", hitbox)`, so the ORDER in which a
# step's simultaneous hurtbox/hitbox overlaps are resolved is the FIFO order of
# the physics server's own `area_entered` emissions.  That order is a function of
# engine-internal containers that are ordered by raw pointers:
#   * BroadPhase2DHashGrid::_check_motion (servers/physics_2d/
#     broad_phase_2d_hash_grid.cpp:84-102) iterates `Map<Element *, PairData *>`,
#     ordered by the `Element *` heap address, to fire the pair callbacks that
#     queue each Area2DSW monitor update; and
#   * Area2DSW::call_queries (area_2d_sw.cpp:242-284) emits the monitor callback
#     per `Map<BodyKey, BodyState>` entry, whose BodyKey orders by
#     `RID::operator<`, i.e. `_data < p_rid._data` (core/rid.h:66-68) -- again a
#     raw heap address.
# Two runs of the same binary therefore emit the same set of overlaps in a
# different order (measured: three simultaneously overlapped enemies' enter order
# flipped between runs, and with it the enemy a one-hit, `piercing=0` projectile
# damages -- player_projectile.gd:107-113 stops the projectile on the first hit,
# so only the first-resolved victim is damaged and the run diverges).  No engine
# setting removes this: fixed-fps already pins the timers, and `physics/2d/
# use_bvh=true` (which replaces the pointer-ordered broadphase Map with an
# integer-indexed BVH) still flips, because the second Map above is untouched.
# The bridge therefore takes the scene's own connection over and re-queues each
# step's events in ascending (victim instance id, hitbox instance id) order.
# Order only changes when one step has more than one hurt event; a single event
# is resolved exactly where the vanilla deferral resolved it.
func _install_hurt_order(node: Node) -> void:
	if not _hurt_order:
		return
	if not node.has_method("hurt_area_entered_deferred"):
		return
	var hurtbox = node.get_node_or_null("Hurtbox")
	if hurtbox == null or not (hurtbox is Area2D):
		return
	if hurtbox.is_connected("area_entered", node, "_on_Hurtbox_area_entered"):
		hurtbox.disconnect("area_entered", node, "_on_Hurtbox_area_entered")
	if not hurtbox.is_connected("area_entered", self, "_on_hurt_ordered"):
		var err = hurtbox.connect("area_entered", self, "_on_hurt_ordered", [node])
		if err == OK:
			_hurt_units_taken_over += 1
		else:
			printerr("RLBRIDGE: cannot take over Hurtbox.area_entered for ", node, ": ", err)


func _on_hurt_ordered(area: Area2D, unit) -> void:
	_hurt_queue.push_back([unit, area])
	if not _hurt_flush_scheduled:
		# Scheduled from the first event of the step, so the flush lands exactly
		# where the first vanilla deferral landed (the message queue flush after
		# the physics iteration, main.cpp:2400-2416).
		_hurt_flush_scheduled = true
		call_deferred("_flush_hurt_queue")


func _flush_hurt_queue() -> void:
	_hurt_flush_scheduled = false
	var pending: Array = _hurt_queue
	_hurt_queue = []
	if pending.size() > 1:
		pending.sort_custom(self, "_hurt_before")
	for item in pending:
		var unit = item[0]
		var area = item[1]
		if unit == null or not is_instance_valid(unit):
			continue
		if area == null or not is_instance_valid(area):
			continue
		unit.hurt_area_entered_deferred(area)
		_hurt_resolved += 1


func _hurt_before(a, b) -> bool:
	var ka := int(a[0].get_instance_id()) if is_instance_valid(a[0]) else 0x7FFFFFFF
	var kb := int(b[0].get_instance_id()) if is_instance_valid(b[0]) else 0x7FFFFFFF
	if ka != kb:
		return ka < kb
	var ha := int(a[1].get_instance_id()) if is_instance_valid(a[1]) else 0x7FFFFFFF
	var hb := int(b[1].get_instance_id()) if is_instance_valid(b[1]) else 0x7FFFFFFF
	return ha < hb


func _hurt_order_report() -> Dictionary:
	return {
		"enabled": _hurt_order,
		"units_taken_over": _hurt_units_taken_over,
		"hurts_resolved": _hurt_resolved,
		"order": "ascending (victim instance id, hitbox instance id)",
		"why": "unit.gd:611 defers each hurt in the physics server's area_entered order, which is pointer-ordered (BroadPhase2DHashGrid::_check_motion Map<Element*,PairData*>; Area2DSW::call_queries Map<BodyKey,BodyState> via RID::operator<) and therefore not reproducible run to run; the scene connection is replaced so the resolution order is a pure function of the game state",
		"vanilla_semantics": "identical call set and call site (hurt_area_entered_deferred), identical deferral point; only the order of simultaneous events changes"
	}


# Determinism probe: every Enemy's Hurtbox emits `area_entered` for each attacking
# Hitbox Area2D the physics server reports as newly overlapping, and unit.gd:611
# defers the damage for each of them.  The ORDER of those emissions is what decides
# which of two simultaneously overlapped enemies a one-hit projectile damages (the
# first deferred call stops the projectile, player_projectile.gd:107-113), so it is
# recorded here (appended handler: the scene's own handler still runs first).
func _register_hurt_diag(node: Node) -> void:
	if str(config.get("enemy_diag", "")).empty():
		return
	if not (node is Enemy):
		return
	var hurtbox = node.get_node_or_null("Hurtbox")
	if hurtbox == null or not (hurtbox is Area2D):
		return
	if not hurtbox.is_connected("area_entered", self, "_on_hurt_area_entered_diag"):
		hurtbox.connect("area_entered", self, "_on_hurt_area_entered_diag", [node])


func _on_hurt_area_entered_diag(area: Area2D, enemy) -> void:
	if _enemy_diag_events_file == null:
		var path := str(config.get("enemy_diag", "")) + ".events"
		var f := File.new()
		var err := f.open(path, File.WRITE)
		if err != OK:
			printerr("RLBRIDGE: enemy event diag cannot open %s: %d" % [path, err])
			config["enemy_diag"] = ""
			return
		_enemy_diag_events_file = f
	var hb_from = area.get("from")
	var line := {
		"tick": _ticks,
		"pf": int(Engine.get_physics_frames()),
		"enemy": enemy.get_instance_id() if is_instance_valid(enemy) else -1,
		"enemy_dead": 1 if (is_instance_valid(enemy) and bool(enemy.get("dead"))) else 0,
		"area": area.get_instance_id(),
		"area_active": 1 if bool(area.get("active")) else 0,
		"area_from": hb_from.get_instance_id() if (hb_from != null and is_instance_valid(hb_from)) else -1,
		"ignored": 1 if (area.get("ignored_objects") != null and (area.get("ignored_objects") as Array).has(enemy)) else 0,
		"px": enemy.global_position.x if is_instance_valid(enemy) else -1.0,
		"py": enemy.global_position.y if is_instance_valid(enemy) else -1.0,
	}
	_enemy_diag_events_file.store_line(JSON.print(line))
	_enemy_diag_events += 1


func _register_particles_tree(node: Node, depth: int) -> void:
	if node == null or depth > 32:
		return
	_register_particle_node(node)
	for child in node.get_children():
		_register_particles_tree(child, depth + 1)


func _register_particle_node(node: Node) -> void:
	if node is CPUParticles2D or node is Particles2D:
		if not _particle_nodes.has(node):
			_particle_nodes.push_back(node)
			if bool(node.get("emitting")):
				node.set("emitting", false)
				_particle_emissions_stopped += 1


# Runs before the particles' own internal process in the same frame (the
# idle_frame signal is emitted ahead of the idle_process_internal group walk and
# ahead of the draw pass), so an emission a gameplay script just enabled never
# reaches the emitter, and with `emitting` false _particles_process skips every
# draw (`if (!emitting) { p.active = false; continue; }` precedes Math::rand()).
func _suppress_particle_emission() -> void:
	_particle_frames += 1
	var alive := []
	for node in _particle_nodes:
		if node == null or not is_instance_valid(node):
			continue
		alive.push_back(node)
		if bool(node.get("emitting")):
			node.set("emitting", false)
			_particle_emissions_stopped += 1
	_particle_nodes = alive


# Everything that MUST be in place before the retail autoloads' _ready() runs,
# or before the run's first tick.
func _force_pre_boot_state() -> void:
	_rng_diag_supported = Engine.has_method("get_math_rand_draw_count")
	_hurt_order = bool(config.get("order_hurts", true))
	# Provenance for engine-level determinism knobs: a `--main-pack` run also
	# loads `<pack dir>/override.cfg` (project_settings.cpp:387-395), so a run
	# dir can pin these without rebuilding the pack; record what the engine
	# actually resolved (and, for jitter/fps, what it uses at runtime).
	prints("RLBRIDGE: physics settings", JSON.print({
		"physics/2d/use_bvh": ProjectSettings.get_setting("physics/2d/use_bvh"),
		"physics/2d/thread_model": ProjectSettings.get_setting("physics/2d/thread_model"),
		"physics/2d/cell_size": ProjectSettings.get_setting("physics/2d/cell_size"),
		"physics/2d/bp_hash_table_size": ProjectSettings.get_setting("physics/2d/bp_hash_table_size"),
		"physics/common/physics_fps": int(Engine.iterations_per_second),
		"physics/common/physics_jitter_fix": float(Engine.physics_jitter_fix),
		"rendering/threads/thread_model": ProjectSettings.get_setting("rendering/threads/thread_model"),
		"debug/settings/fps/force_fps": int(Engine.target_fps)}))
	prints("RLBRIDGE: deterministic damage resolution (order_hurts) =", _hurt_order,
		"| engine threads: physics/2d/thread_model=", ProjectSettings.get_setting("physics/2d/thread_model"),
		"rendering/threads/thread_model=", ProjectSettings.get_setting("rendering/threads/thread_model"))
	_full_unlock_report = {"requested": bool(config["full_unlock"]), "flag_set": false,
		"flag_set_at": "mod_main._ready() (autoload #7, before ProgressData #10)",
		"engine_rng_diag": _rng_diag_supported}
	# --- 1. Full unlock ----------------------------------------------------
	# The game's own switch for a fully-unlocked profile: ProgressData._ready()
	# (progress_data.gd:134-136) then runs unlock_all() + save().  Setting it here
	# makes the shop pool (ItemService.init_unlocked_pool()) the FULL retail pool.
	if bool(config["full_unlock"]):
		if DebugService == null:
			_full_unlock_report["error"] = "DebugService singleton not bound yet"
			printerr("RLBRIDGE: full unlock NOT applied - DebugService is not available")
		else:
			DebugService.generate_full_unlocked_save_file = true
			_full_unlock_report["flag_set"] = true
			_full_unlock_report["generate_full_unlocked_save_file"] = true
			prints("RLBRIDGE: DebugService.generate_full_unlocked_save_file = true (before ProgressData._ready)")
	if bool(config["always_crates"]):
		DebugService.always_drop_crates = true
		_full_unlock_report["always_drop_crates"] = true
	# --- 3. Effect-only RNG sites -----------------------------------------
	# SoundManagerParent._process (singletons/sound_manager_parent.gd:45) draws
	# one global rand_range PER SOUND PLAYED for its pitch.  The audio itself is
	# a dummy driver, the draw is not, so the queues are emptied before the
	# manager's own _process can pop an entry (this node is processed first: it
	# is inside ModLoader, autoload #7, ahead of SoundManager #16/#17).
	_effect_rng_off = {
		"sound_pitch": "singletons/sound_manager_parent.gd:45 (one global rand_range per sound popped by _process) - suppressed only under audio_rng=0 by emptying SoundManager/SoundManager2D queues before their own _process runs; under audio_rng=1 the draw is left intact and the core's audio model accounts for it",
		"floating_text": "visual_effects/floating_text/floating_text.gd:17 (one global rand_range per text shown)",
		"floating_text_sound_pick": "visual_effects/floating_text/floating_text_manager_base.gd:33,44 -> Utils.get_rand_element -> Array.pick_random() (one global draw per displayed stat/heal)",
		"particles": "scene/2d/cpu_particles_2d.cpp:826-912 CPUParticles2D::_particles_process draws Math::rand()/Math::randf() per emitted particle (a Ghost Scepter particle scene costs 11-13 global words per particle); every CPUParticles2D/Particles2D is registered via SceneTree.node_added and its `emitting` flag is cleared from a SceneTree.idle_frame handler, which runs before the idle_process_internal group walk and before the draw pass, so no emission reaches the emitter however often a gameplay script re-enables it (the draw itself is guarded by `if (restart) { if (!emitting) { p.active = false; continue; }`). PROCESS_MODE_DISABLED was rejected: it would also stop particles/burning/burning_particles.gd _physics_process, which spreads burning between units (gameplay)",
		"forced_off": [
			"singletons/sound_manager_parent.gd:45 sound pitch -> SoundManager/SoundManager2D queues emptied every idle frame before their own _process can pop (0 draws)",
			"visual_effects/floating_text/floating_text.gd:17 -> manager pinned (current_nb_of_texts > MAX_TEXTS) every arena/shop tick, and its four sound arrays emptied",
			"visual_effects/floating_text/floating_text_manager.gd:65 damage numbers -> ProgressData.settings.damage_display = false",
			"global/effects_manager.gd:79-81 -> ProgressData.settings.visual_effects = false",
			"global/screenshaker.gd:26,36-42 -> ProgressData.settings.screenshake = false",
			"scene/2d/cpu_particles_2d.cpp:863-911 CPUParticles2D emission -> emitting cleared every idle frame (every particle scene: ghost_scepter_particles.tscn on the Ghost Scepter, muzzle/explosion/burn/death effects)",
			"gameplay interaction checked: singletons/pets (scapegoat.gd:54-63,116-130) and unit.gd:711 / fireball.gd:10 / burning_particles.gd:18 only use `emitting` to drive animations and audio; no stat, damage, heal amount or pet-state decision reads it, so clearing it changes no gameplay value"
		],
		"recorded_not_forced": [
			"entities/birth/entity_birth.gd:47 rotation_degrees = rand_range(0, 360) -- one GLOBAL draw per entity birth/spawn",
			"ui/menus/ingame/character_panel_ui.gd:37 _ready -> advance(rand_range(...)) -- one GLOBAL draw per arena HUD instance",
			"singletons/ui_service.gd:88 play_error_animation -- one GLOBAL draw per rejected UI action"
		]
	}


# Runs before SoundManager's own _process (tree order), so no queued sound is
# ever popped and its pitch draw never happens.
func _process(_delta: float) -> void:
	if not config["enabled"]:
		return
	# audio_rng=0 (training/eval) neutralizes the game's sound draws; audio_rng=1
	# (legacy) leaves them for the core's own audio model.
	if int(config["audio_rng"]) == 0:
		_clear_sound_queues()
	_suppress_particle_emission()


func _clear_sound_queues() -> void:
	var cleared := 0
	if SoundManager != null and SoundManager.get("sounds_to_play") != null:
		cleared += SoundManager.sounds_to_play.size()
		SoundManager.sounds_to_play.clear()
	if SoundManager2D != null and SoundManager2D.get("sounds_to_play") != null:
		cleared += SoundManager2D.sounds_to_play.size()
		SoundManager2D.sounds_to_play.clear()
	if cleared > 0:
		_sound_queue_clears += cleared


# Pins the arena's floating-text manager so its display() guard trips: display()
# returns before creating a text when current_nb_of_texts > MAX_TEXTS (100) and
# always_display is false, and every non-damage caller passes false.  The
# damage-number caller is gated by ProgressData.settings.damage_display (false).
# The four sound arrays are emptied only under audio_rng=0: that removes
# display_icon()'s Utils.get_rand_element() draw and its SoundManager queue
# entries, which is what a core run without the audio model expects.
func _pin_floating_text(scene) -> void:
	if scene == null:
		return
	var manager = scene.get("_floating_text_manager")
	if manager == null:
		manager = scene.find_node("FloatingTextManager*", true, false)
	if manager == null:
		return
	if manager.get("current_nb_of_texts") != null:
		manager.current_nb_of_texts = 101
		_floating_text_pins += 1
	if int(config["audio_rng"]) != 0:
		return
	for key in ["stat_pos_sounds", "stat_neg_sounds", "harvest_pos_sounds", "harvest_neg_sounds"]:
		if manager.get(key) != null and not (manager.get(key) as Array).empty():
			manager.set(key, [])
	_effect_rng_off["floating_text_manager_path"] = str(manager.get_path())


# --- 2. RNG draw counters (diagnostic engine only) ------------------------
# Engine.get_math_rand_draw_count() counts every RandomPCG::rand() the global
# Math stream makes (Math::rand/randf/randd/randi/rand_range/randomize all go
# through it), and RandomNumberGenerator.get_draw_count() counts the same
# primitive on Utils' own generator (Utils._rng).  Both are cumulative, so the
# per-step delta between consecutive rows is the stream consumption of that
# physics step.
func _rng_counts() -> Dictionary:
	if not _rng_diag_supported:
		return {"math": -1, "utils": -1, "step_math": -1, "step_utils": -1}
	var math_count := int(Engine.get_math_rand_draw_count())
	var utils_count := int(Utils._rng.get_draw_count())
	var step_math := math_count - _rng_math_last if _rng_counters_used else math_count
	var step_utils := utils_count - _rng_utils_last if _rng_counters_used else utils_count
	_rng_math_last = math_count
	_rng_utils_last = utils_count
	_rng_counters_used = true
	_rng_math_final = math_count
	_rng_utils_final = utils_count
	return {"math": math_count, "utils": utils_count, "step_math": step_math, "step_utils": step_utils}


func _rng_diag_report() -> Dictionary:
	return {
		"supported": _rng_diag_supported,
		"engine_method": "Engine.get_math_rand_draw_count()" if _rng_diag_supported else "",
		"utils_method": "RandomNumberGenerator.get_draw_count() on Utils._rng" if _rng_diag_supported else "",
		"math_total": _rng_math_final,
		"utils_total": _rng_utils_final,
		"rows_carrying_counters": _rng_counters_used,
		"note": "cumulative draws by the end of the run; per-row rng_*_step fields are the per-step deltas"
	}


# Debug item/weapon grants, applied through the game's own RunData API.
func _apply_grants() -> void:
	var report := {"items": [], "weapons": [], "errors": []}
	if _run_result.empty() or not bool(_run_result.get("ok", false)):
		_grants_report = {"skipped": "run configuration did not succeed"}
		return
	for item_id in config.get("grant_items_list", []):
		var item = ItemService.get_element_safe(ItemService.items, str(item_id))
		if item == null or not (item is ItemData):
			report["errors"].push_back("item not found: %s" % str(item_id))
			continue
		RunData.add_item(item, 0)
		report["items"].push_back(str(item_id))
	for weapon_id in config.get("grant_weapons_list", []):
		var weapon = ItemService.get_element_safe(ItemService.weapons, str(weapon_id))
		if weapon == null or not (weapon is WeaponData):
			report["errors"].push_back("weapon not found: %s" % str(weapon_id))
			continue
		RunData.add_weapon(weapon, 0)
		report["weapons"].push_back(str(weapon_id))
	_grants_report = report
	prints("RLBRIDGE: debug grants ->", JSON.print(report))


# The decision-relevant resolved configuration, canonicalised and hashed, so a
# capture can be tied to the exact config that produced it (and a wrong-config
# read is detectable without parsing the trace).
const FINGERPRINT_KEYS := [
	"character_id", "starting_weapon", "danger", "zone", "wave", "endless",
	"invulnerable", "wave_seconds", "instant_waves", "always_crates",
	"grant_items", "grant_weapons", "audio_rng", "full_unlock", "action_source",	"script_actions", "script_shop_actions", "script_level_up_actions",
	"action_file", "seed", "max_ticks", "rng_diag", "capture_kind"
]


func _config_fingerprint() -> Dictionary:
	var parts := []
	for key in FINGERPRINT_KEYS:
		parts.push_back("%s=%s" % [key, str(config.get(key, ""))])
	# particles_rng is a fact about what the run did, not a configured knob, so it
	# is appended explicitly (config.particles_rng does not exist by design).
	parts.push_back("particles_rng=%d" % _particles_rng_effective())
	var canonical := PoolStringArray(parts).join("|")
	return {
		"sha256": canonical.sha256_text(),
		"keys": FINGERPRINT_KEYS,
		"canonical": canonical
	}


# The effective particle-RNG mode of this run.  The suppression is installed
# unconditionally while the bridge is enabled (every CPUParticles2D/Particles2D is
# registered and its `emitting` cleared from a SceneTree.idle_frame handler), so
# the recorded value is the fact: 0.  It must NOT be derived from the frame counter:
# _write_meta() runs at BOOT, before the first idle frame, and would report 1 for a
# run whose suppression is active (observed).  The counters that prove the loop ran
# travel beside it as particles_rng_evidence in meta/summary.
func _particles_rng_effective() -> int:
	return 0 if config.get("enabled", false) else 1


# A baseline capture is the replay-comparable canonical config: any grant, forced
# crate or instant wave changes what the core must reproduce, so a baseline that
# carries one is a configuration error and must not be recorded as a baseline.
func _baseline_violations() -> Array:
	var violations := []
	if not str(config.get("grant_items", "")).empty():
		violations.push_back("grant_items=%s" % config["grant_items"])
	if not str(config.get("grant_weapons", "")).empty():
		violations.push_back("grant_weapons=%s" % config["grant_weapons"])
	if bool(config.get("always_crates", false)):
		violations.push_back("always_crates=1")
	if bool(config.get("instant_waves", false)):
		violations.push_back("instant_waves=1")
	return violations


func _missing_autoloads() -> Array:
	var missing := []
	var root := get_tree().get_root()
	for name in REQUIRED_AUTOLOADS:
		if root.get_node_or_null(name) == null:
			missing.push_back(name)
	return missing


func _bootstrap_step() -> void:
	if _booted or _finished:
		return

	var missing := _missing_autoloads()
	if not missing.empty():
		_boot_frames += 1
		if _boot_frames == 1 or _boot_frames % 60 == 0:
			prints("RLBRIDGE: waiting for autoloads", missing, "frame", _boot_frames)
		if _boot_frames > 600:
			_fail_boot("autoload_timeout", "autoloads never became available: %s" % str(missing))
			return
		call_deferred("_bootstrap_step")
		return

	_booted = true
	prints("RLBRIDGE: autoloads ready after %d frame(s)" % _boot_frames)

	# --- settings repair + application ------------------------------------
	var env_runner := RLEnv.new()
	if config["repair_settings"]:
		_settings_report = env_runner.sanitize_settings(config["force_english"])
		prints("RLBRIDGE: settings repaired:", _settings_report["repair_count"], "key(s)",
			_settings_report["repairs"])
	else:
		_settings_report = {"repairs": [], "repair_count": 0, "disabled": true}
		if config["force_english"]:
			TranslationServer.set_locale("en")
			ProgressData.settings["language"] = "en"

	if config["apply_settings"]:
		_settings_report.merge(env_runner.apply_settings_safely(config["suppress_fullscreen"]))
		prints("RLBRIDGE: ProgressData.apply_settings() -> locale =", _settings_report["locale"],
			"| fullscreen setting =", _settings_report["fullscreen"])
	else:
		prints("RLBRIDGE: ProgressData.apply_settings() skipped by configuration")

	prints("RLBRIDGE: platform =", Platform.get_type_as_string(),
		"| steam singleton =", Engine.has_singleton("Steam"),
		"| dlc dir =", Directory.new().dir_exists("res://dlcs/"),
		"| user dir =", ProjectSettings.globalize_path("user://"))

	# --- observation tables + progression helpers --------------------------
	_progress = RLProgress.new()
	_obs = RLObs.new()
	if not _obs.load_tables(config["obs_tables"]):
		_fail_boot("obs_tables_missing", "%s (generate with ocean/brotato/bridge/gen_obs_tables.py)"
			% _obs.table_error)
		return
	_obs_report = _obs.report()
	prints("RLBRIDGE: observation tables loaded; obs_size =", _obs_report["obs_size"],
		"| scale =", JSON.print(_obs_report["scale"]))

	# --- run configuration -------------------------------------------------
	# action_source=file fails closed before anything is configured: a missing or
	# malformed stream must never be silently replaced by the scripted sequence.
	if str(config["action_source"]) == "file" and not _inject.is_ready():
		_fail_boot("action_file_open_failed",
			"action_source=file but the action stream is unusable: %s" % _inject.action_file_error)
		return
	# A baseline capture must be the replay-comparable canonical config.
	if str(config["capture_kind"]) == "baseline":
		var violations := _baseline_violations()
		if not violations.empty():
			_fail_boot("baseline_config_violation",
				"capture_kind=baseline with non-baseline settings: %s" % PoolStringArray(violations).join(", "))
			return
	var runner := RLRun.new()
	runner.apply_debug_knobs(config)
	_run_result = runner.configure(config["character_id"], config["danger"], config["zone"],
		config["starting_weapon"], config["endless"])
	_shop_pool_report = _run_result.get("facts", {}).get("shop_pool", {})
	if not _shop_pool_report.empty():
		prints("RLBRIDGE: shop pool ->", JSON.print({
			"full_pool": _shop_pool_report.get("full_pool", null),
			"totals": _shop_pool_report.get("totals", {}),
			"expected": _shop_pool_report.get("expected", {}),
			"missing_total": _shop_pool_report.get("missing_total", null)
		}))

	# --- trace -------------------------------------------------------------
	_trace = RLTrace.new()
	_trace.configure(config["partial"])
	if not _trace.open_trace(config["trace_path"]):
		_fail_boot("trace_open_failed", "File.open('%s') error %d" % [config["trace_path"], _trace.open_error])
		return
	var header_error: int = _trace.write_header({
		"bridge_version": BRIDGE_VERSION,
		"obs_definition": "see the meta file and ocean/brotato/bridge/state_contract.json"
	})
	if header_error != OK:
		_fail_boot("trace_header_failed", "header write error %d" % header_error)
		return

	# --- optional policy ---------------------------------------------------
	if config["policy"]:
		_policy = RLPolicy.new()
		var policy_load: Dictionary = _policy.load_artifact(config["policy_artifact"])
		prints("RLBRIDGE: policy load ->", JSON.print(policy_load))
		if not policy_load["ok"]:
			if _policy_mode():
				_fail_boot("policy_load_failed",
					"action_source=policy but the artifact could not be loaded: %s" % str(policy_load.get("error", "")))
				return
			prints("RLBRIDGE: policy unavailable; action source '%s' continues to drive the run"
				% config["action_source"])
	elif _policy_mode():
		_fail_boot("policy_load_failed", "action_source=policy but the policy harness is disabled (BROTATO_RL_POLICY=0)")
		return
	else:
		prints("RLBRIDGE: policy harness disabled (BROTATO_RL_POLICY=0)")
	_inject_report = _inject.report()

	_write_meta()

	if not _run_result["ok"]:
		_failure = _run_result["error"]
		_failure_message = _run_result["error_message"]
		_exit_code = 1
		prints("RLBRIDGE: RUN CONFIGURATION FAILED:", _failure, "-", _failure_message)
		_finish("run_configuration_failed")
		return

	# Debug item/weapon grants (pet/structure-heavy traces): applied through the
	# game's own RunData.add_item()/add_weapon() so all effects, caches and
	# stat/challenge bookkeeping run exactly as a purchase does.
	_apply_grants()

	# The very first completed step has no injected action yet; the engine's own
	# input for it is "no input", i.e. HOLD.  Seed the injector with a zero
	# direction so the first row reports action 0 (a real, replayable action)
	# instead of an uninitialised -1, and so action/next_action stay aligned.
	_inject.pending_action = 0
	_inject.pending_direction = Vector2.ZERO
	_inject.injected_action = 0
	_inject.injected_direction = Vector2.ZERO
	# The seeded HOLD is a real completed step too: mark the injection as
	# verifiable so the first row reports action 0 (a replayable hold) rather than
	# an uninitialised -1.
	_pending_action_valid = true

	var facts = _run_result["facts"]
	_boot_tick = _ticks
	prints("RLBRIDGE: BOOT OK | char =", facts["character_found"],
		"| hp =", facts["player_current_hp"], "/", facts["player_max_hp"],
		"| danger =", facts["difficulty"], "| wave =", facts["wave"], "| zone =", facts["zone"],
		"| dlc =", str(facts["active_dlc_ids"]))
	prints("RLBRIDGE: changing scene to", MenuData.game_scene)
	get_tree().change_scene(MenuData.game_scene)


# ---------------------------------------------------------------------------
# Phase detection
# ---------------------------------------------------------------------------

func _phase(scene, scene_name: String) -> int:
	if scene_name == SHOP_SCENE_HINT:
		return PHASE_SHOP
	if scene_name == ARENA_SCENE_HINT:
		var upgrades = scene.get("_upgrades_ui")
		if upgrades != null and is_instance_valid(upgrades) and upgrades is CanvasItem and upgrades.visible:
			return PHASE_LEVEL_UP
	return PHASE_COMBAT


func _log_arena(scene) -> void:
	var wave_timer = scene.get_node_or_null("WaveTimer")
	var snapshot := {
		"tick": _ticks,
		"scene": str(scene.name),
		"phase": PHASE_COMBAT,
		"wave": RunData.current_wave,
		"difficulty": RunData.current_difficulty,
		"wave_in_progress": 1 if RunData.wave_in_progress else 0,
		"wave_time_left": wave_timer.time_left if wave_timer != null else -1.0,
		"wave_duration": wave_timer.wait_time if wave_timer != null else -1.0,
		"living_enemies_run_data": RunData.current_living_enemies,
		"enemy_nodes": RLTrace.count_entity_nodes(get_tree(), "enemy"),
		"player_nodes": RLTrace.count_entity_nodes(get_tree(), "player"),
		"projectile_nodes": RLTrace.container_child_count(get_tree(), "EnemyProjectiles"),
		"consumable_nodes": RLTrace.container_child_count(get_tree(), "Consumables"),
		"entity_spawner": 1 if scene.get_node_or_null("EntitySpawner") != null else 0,
		"wave_manager": 1 if scene.get_node_or_null("WaveManager") != null else 0,
		"hp": RunData.get_player_current_health(0),
		"max_hp": RunData.get_player_max_health(0),
		"player_count": RunData.get_player_count(),
		"current_zone": RunData.current_zone
	}
	_arena_snapshots.push_back(snapshot)
	if _arena_snapshots.size() > 120:
		_arena_snapshots.pop_front()
	prints("RLBRIDGE: ARENA", JSON.print(snapshot))
	if snapshot["enemy_nodes"] == 0 and snapshot["wave_time_left"] > 0.0 and \
			_arena_ticks > 300 and not _stall_warned:
		_stall_warned = true
		printerr("RLBRIDGE: no enemy nodes after %d arena ticks in wave %d; the spawn path is stalled - see this snapshot" % [_arena_ticks, RunData.current_wave])


# ---------------------------------------------------------------------------
# Main loop
# ---------------------------------------------------------------------------

func _physics_process(_delta: float) -> void:
	if not _booted or _finished:
		return
	_ticks += 1

	var tree := get_tree()
	var scene = RLTrace.current_scene(tree)
	var scene_name := ""
	if scene != null:
		scene_name = scene.name
	if scene_name != "" and not _scenes_seen.has(scene_name):
		_scenes_seen.push_back(scene_name)
		prints("RLBRIDGE: scene ->", scene_name, "at tick", _ticks)

	var in_arena := scene_name == ARENA_SCENE_HINT
	var is_shop := scene_name == SHOP_SCENE_HINT
	# Floating text / its sound picks are effect-only global-RNG sites the core
	# does not model; pin the live manager every tick (a wave teardown can clear
	# it) so display() returns before drawing.
	if in_arena or is_shop:
		_pin_floating_text(scene)
	var player = RLTrace.find_player(tree) if in_arena else null
	if player != null:
		_last_position = player.global_position
		_player_seen = true

	# Per-wave arena bookkeeping so the stall diagnostics stay meaningful across
	# waves (the run continues through the shop into wave 2 and beyond).
	if RunData.current_wave != _wave_seen:
		_wave_seen = RunData.current_wave
		_arena_ticks = 0
		_stall_warned = false
		# A new wave re-arms the wave-end probe window (expiry tick unknown again).
		_wave_end_tick = -1
		_wave_end_probe_offsets.clear()

	var phase := _phase(scene, scene_name)
	# First arena tick after a shop / level-up phase: the player was not simulated
	# while the arena was inactive, so its movement state cannot be verified for
	# that tick (rl_inject.verify() skips the comparison once).
	if phase == PHASE_COMBAT and _prev_phase >= 0 and _prev_phase != PHASE_COMBAT:
		_inject.resync_pending = true
	_prev_phase = phase

	if not _run_result.empty() and not _run_result["ok"]:
		if _ticks >= int(config["max_ticks"]):
			_finish("tick_budget")
		return

	# --- progression phases -------------------------------------------------
	if is_shop:
		_shop_ticks += 1
		_out_of_scene_ticks = 0
		_phase_counts["shop"] += 1
		_drive_shop(scene, _ticks - 1)
		return
	if phase == PHASE_LEVEL_UP and player != null:
		_level_up_ticks += 1
		_out_of_scene_ticks = 0
		_phase_counts["level_up"] += 1
		_drive_level_up(scene, player, _ticks - 1)
		return

	if in_arena and player != null:
		if _arena_loaded_tick < 0:
			_arena_loaded_tick = _ticks
			prints("RLBRIDGE: arena scene active at tick", _ticks)
			_probe_viewport(scene)
		_probe_wave_end(scene, scene.get_node_or_null("WaveTimer"), player)
		_arena_ticks += 1
		_phase_counts["combat"] += 1
		if _arena_ticks == 1 or _arena_ticks % 60 == 0:
			_log_arena(scene)
	elif _arena_loaded_tick >= 0 and not in_arena:
		# Neither arena nor shop: the run ended (death/victory) or an unexpected
		# scene loaded.  Give it a window, then stop with a reason.
		_out_of_scene_ticks += 1
		if _out_of_scene_ticks > 300:
			_finish("unexpected_scene")
			return
	elif _arena_loaded_tick < 0 and _ticks > 900:
		_fail_boot("arena_never_loaded", "scene '%s' never became '%s' within 900 physics ticks"
			% [scene_name, ARENA_SCENE_HINT])
		return

	if player == null:
		if _ticks >= int(config["max_ticks"]):
			_finish("tick_budget")
		return

	# --- combat: one observation, used for the policy input and the record ---
	# At the start of this tick the state is post-step (step_index), so this is
	# obs[step_index].
	var step_index := _ticks - 1
	var wave_timer = scene.get_node_or_null("WaveTimer")
	var observed: Dictionary = _obs.observe(tree, player, PHASE_COMBAT, wave_timer, _last_position)
	var obs: PoolRealArray = observed["obs"]
	if not str(config.get("enemy_diag", "")).empty():
		_enemy_diag_row(step_index, tree, player)

	# One verification path for every mechanism: the player's own _current_movement
	# (what Unit.get_movement() returns from its movement behavior) must equal the
	# direction requested for the step that just completed.
	var verify: Dictionary = {}
	if _pending_action_valid:
		verify = _inject.verify(player)

	# policy, then choose and inject the action for the next step
	var policy_result: Dictionary = {}
	if _policy != null:
		policy_result = _policy.step(obs)
	var choice: Dictionary = _inject.choose_action(step_index, _policy, policy_result)
	if _policy_mode() and str(choice["source"]) != "policy":
		_fail_policy(step_index, str(choice.get("reason", "policy did not produce an action")))
		return
	var action := -1
	var applied := 0
	if not verify.empty():
		action = int(verify["action"])
		applied = int(verify["applied"])
	_inject.set_action(int(choice["action"]))
	_pending_action_valid = true
	_inject.apply_input(int(choice["action"]))

	if step_index % int(config["sample_every"]) == 0:
		_write_row(step_index, observed, action, applied, choice, policy_result, scene_name, wave_timer, false)

	if _ticks >= int(config["max_ticks"]):
		_finish("tick_budget")


# ---------------------------------------------------------------------------
# Progression drivers (shop / level-up)
# ---------------------------------------------------------------------------

func _drive_shop(scene, step_index: int) -> void:
	var tree := get_tree()
	var wave_timer = scene.get_node_or_null("WaveTimer")
	# Pre-action observation: this is what the ACTION CHOICE sees (a real player
	# sees the shop frame, presses, and the effect shows up in the NEXT frame's
	# observation), and a skip must not make the choice describe the arena it
	# just switched to.  The shop scene has no Player node, so the last real
	# arena position is carried (position_carried=1).
	var observed: Dictionary = _obs.observe(tree, RLTrace.find_player(tree), PHASE_SHOP, wave_timer, _last_position)
	var choice := _progression_action(step_index, "shop", observed["obs"])
	if _policy_mode() and str(choice["source"]) != "policy":
		_fail_policy(step_index, "shop: " + str(choice.get("reason", "policy did not produce an action")))
		return
	var snapshot: Dictionary = _progress.shop_snapshot(scene, SHOP_SCENE_HINT)
	var result: Dictionary = _progress.shop_action(scene, int(choice["action"]))
	var verify := {"applied": 0, "error": str(result.get("error", ""))}
	if bool(result.get("ok", false)):
		verify = _progress.verify_shop(scene, str(result["kind"]), snapshot, str(scene.name))
	else:
		prints("RLBRIDGE: shop action", choice["action"], "failed:", result.get("error", ""))
	# Movement is held while the arena is not active.  The analog actions are
	# released too: without that the last combat direction stays pressed, the
	# engine still derives it, and the next arena tick's movement verification
	# compares the stale vector against the fresh injection (a false mismatch).
	_inject.set_action(0)
	_inject.apply_input(0)
	_pending_action_valid = true
	if step_index % int(config["sample_every"]) != 0:
		if _ticks >= int(config["max_ticks"]):
			_finish("tick_budget")
		return
	# The RECORDED row describes the state AFTER this tick's action, like every
	# combat row (input applied, frame stepped, then observed).  The game applies
	# a shop action synchronously inside shop_action (it calls the scene's own
	# handler and then FAILS unless the purchase/recycle/reroll is already
	# applied), so the effect belongs to this tick; only the pre-action `observed`
	# above is deliberately older, and that one feeds the action choice.
	var row_obs: Dictionary = _obs.observe(tree, RLTrace.find_player(tree), PHASE_SHOP, wave_timer, _last_position)
	_last_shop_row = {
		"action": int(choice["action"]),
		"kind": str(result.get("kind", "")),
		"ok": bool(result.get("ok", false)),
		"error": str(result.get("error", "")),
		"applied": int(verify["applied"]),
		"verify_error": str(verify["error"]),
		"after": verify.get("after", {})
	}
	_write_row(step_index, row_obs, int(choice["action"]), int(verify["applied"]), choice, {}, str(scene.name), wave_timer, true)
	if _ticks >= int(config["max_ticks"]):
		_finish("tick_budget")


func _drive_level_up(scene, player, step_index: int) -> void:
	var ui = scene.get("_upgrades_ui")
	var wave_timer = scene.get_node_or_null("WaveTimer")
	# Pre-action observation with the live player, so player stats come from the
	# entity/RunData and not from a post-action re-query.
	var observed: Dictionary = _obs.observe(get_tree(), player, PHASE_LEVEL_UP, wave_timer, player.global_position)
	var choice := _progression_action(step_index, "level_up", observed["obs"])
	if _policy_mode() and str(choice["source"]) != "policy":
		_fail_policy(step_index, "level_up: " + str(choice.get("reason", "policy did not produce an action")))
		return
	var snapshot: Dictionary = _progress.level_up_snapshot(ui)
	var result: Dictionary = _progress.level_up_action(ui, int(choice["action"]))
	var verify := {"applied": 0, "error": str(result.get("error", ""))}
	if bool(result.get("ok", false)):
		verify = _progress.verify_level_up(ui, snapshot, str(result.get("kind", "")))
	else:
		prints("RLBRIDGE: level-up action", choice["action"], "failed:", result.get("error", ""))
	_inject.set_action(0)
	_inject.apply_input(0)
	_pending_action_valid = true
	if step_index % int(config["sample_every"]) != 0:
		if _ticks >= int(config["max_ticks"]):
			_finish("tick_budget")
		return
	_last_level_up_row = {
		"action": int(choice["action"]),
		"kind": str(result.get("kind", "")),
		"ok": bool(result.get("ok", false)),
		"error": str(result.get("error", "")),
		"applied": int(verify["applied"]),
		"verify_error": str(verify["error"]),
		"after": verify.get("after", {})
	}
	_write_row(step_index, _obs.observe(get_tree(), player, PHASE_LEVEL_UP, wave_timer, player.global_position),
	           int(choice["action"]), int(verify["applied"]), choice, {}, str(scene.name), wave_timer, false)
	if _ticks >= int(config["max_ticks"]):
		_finish("tick_budget")


# Policy's choice when it is valid for the phase, otherwise the deterministic
# scripted one; the source and any rejection reason are recorded per row.
func _progression_action(step_index: int, phase_kind: String, obs: PoolRealArray) -> Dictionary:
	var scripted := 0
	# The id's provenance is what the row must report: an id taken from the action
	# stream is source "file" exactly like a combat frame from the same stream.
	var source_label := "script"
	if _inject.source == "file":
		# One id per STATE: the recorded stream advances once per shop / level-up
		# tick, the same cadence the policy and scripted sources use.
		scripted = _inject.next_file_action()
		source_label = "file"
	elif phase_kind == "shop":
		var configured: Array = config.get("script_shop_actions_list", [])
		if configured.empty():
			scripted = _progress.scripted_shop_action(step_index, _reroll_budget, _progress.shop_rerolls)
		else:
			# One action per attempt, advancing through the configured sequence
			# every BROTATO_RL_SCRIPT_HOLD_TICKS ticks so each attempt gets time to
			# settle.  A rejected attempt keeps its own reason through the module's
			# verification, so a purchase only ever reports applied when the game's
			# own handler took the currency.
			var hold: int = int(max(1, int(config["script_hold_ticks"])))
			scripted = int(configured[_shop_script_index % configured.size()])
			if _shop_script_tick % hold == 0:
				_shop_script_index += 1
			_shop_script_tick += 1
	else:
		var configured_lu: Array = config.get("script_level_up_actions_list", [])
		if configured_lu.empty():
			scripted = _progress.scripted_level_up_action(step_index)
		else:
			# Same one-action-per-attempt cadence as the shop sequence, so a crate
			# offer (29 take / 30 recycle) gets the settle ticks its container needs.
			var hold_lu: int = int(max(1, int(config["script_hold_ticks"])))
			scripted = int(configured_lu[_level_up_script_index % configured_lu.size()])
			if _level_up_script_tick % hold_lu == 0:
				_level_up_script_index += 1
			_level_up_script_tick += 1
	if _policy == null:
		return {"action": scripted, "source": source_label, "reason": ""}

	# The caller passes the observation it already built (pre-action, with the live
	# player when the arena is active) so the policy consumes exactly the vector the
	# row records - never a post-action re-query that could land in another phase.
	var policy_result: Dictionary = _policy.step(obs)
	if bool(policy_result.get("ok", false)):
		var proposed := int(policy_result["action"])
		# Declared contract per phase: level-up 0-3 choose that offer; shop 0-3 buy
		# that slot, 4 reroll, 8 skip, 9-14 recycle weapon slot, 15-20 combine
		# weapon slot.  Anything else is invalid and falls back.
		# v4 phase-factored gate (brotato_state.h BT_ACTIONS = 31) -- keep it equal
		# to the contract's sets (state_contract.json action.table) so a policy
		# action is never admitted without a driver and never rejected with one:
		#   combat    0..8            movement
		#   shop      0..4, 8..28     buy/reroll/go/recycle/combine/lock/ban
		#   level-up  0..4, 29..30    choose/reroll, crate take/recycle
		var valid: bool
		if phase_kind == "level_up":
			valid = (proposed >= 0 and proposed <= 4) or proposed == 29 or proposed == 30
		elif phase_kind == "shop":
			valid = (proposed >= 0 and proposed <= 4) or (proposed >= 8 and proposed <= 28)
		else:
			valid = proposed >= 0 and proposed <= 8
		if valid:
			return {"action": proposed, "source": "policy", "reason": ""}
		return {"action": scripted, "source": "script_fallback",
			"reason": "policy action %d not valid in the %s phase" % [proposed, phase_kind]}
	return {"action": scripted, "source": "script_fallback", "reason": str(policy_result.get("error", ""))}


# ---------------------------------------------------------------------------
# Row building
# ---------------------------------------------------------------------------

# One-shot evidence about the native movement path: the behavior instance the unit
# is using, whether the unit is allowed to move, what it last produced, and the
# input vector the engine currently derives from our pressed actions.
func _movement_diagnostics(player) -> Dictionary:
	var behavior = player.get("_current_movement_behavior")
	var behavior_script = null
	if behavior != null and is_instance_valid(behavior):
		behavior_script = behavior.get_script()
	var current = player.get("_current_movement")
	return {
		"player_node": str(player.name),
		"behavior_node": str(behavior.name) if behavior != null else "<null>",
		"behavior_script_path": str(behavior_script.resource_path) if behavior_script != null else "<none>",
		"player_index": player.get("player_index"),
		"behavior_device": behavior.get("device") if behavior != null else -999,
		"can_move": player.get("_can_move"),
		"move_locked": player.get("_move_locked"),
		"current_movement": str(current),
		"input_vector_now": str(_inject.current_input_vector()),
		"pending_action": _inject.pending_action
	}


# Determinism probe (BROTATO_RL_ENEMY_DIAG): dump the observer's own enemy view
# for this step.  The walk is the observer's (`_obs._collect`, recursive child
# order) with the same depth/size guard, so the dumped list IS the input the
# 6-nearest selection ran on.  Read-only: no RNG draw, no observer state touched
# (deliberately not `_obs._stat`, which would record a note).
#
# Entry: [index, instance_id, name, child_index, parent_name, queued_for_deletion,
#   dead, x, y, hp, max_hp, enemy_id, anim_name, anim_pos, anim_speed,
#   anim_playing, cleaning_up, _idle_playback_speed, _can_move, _move_locked,
#   _pending_die, behaviors] where behaviors is `_enemy_diag_behavior_view`.
# The trailing block is the death-speed ground truth: `anim_speed` is the
# AnimationPlayer's playback_speed (the only thing that can shorten a death
# window) and `behaviors` names the writer that can still fire on a dead node
# (a charging behavior's `_unlock_move_timer` timeout).
func _enemy_diag_row(step_index: int, tree, player) -> void:
	if _enemy_diag_file == null:
		var path := str(config.get("enemy_diag", ""))
		var f := File.new()
		var err := f.open(path, File.WRITE)
		if err != OK:
			printerr("RLBRIDGE: enemy diag cannot open %s: %d" % [path, err])
			config["enemy_diag"] = ""
			return
		_enemy_diag_file = f
	var enemies: Array = []
	_obs._collect(tree.get_root(), "enemy", enemies, 0)
	var entries := []
	for i in range(enemies.size()):
		var e = enemies[i]
		if not is_instance_valid(e):
			entries.push_back([i, -1, "<freed>"])
			continue
		var pos: Vector2 = e.global_position
		var hp := -1.0
		var max_hp := -1.0
		if e.current_stats != null and is_instance_valid(e.current_stats):
			var v = e.current_stats.get("health")
			if typeof(v) == TYPE_REAL or typeof(v) == TYPE_INT:
				hp = float(v)
		if e.max_stats != null and is_instance_valid(e.max_stats):
			var w = e.max_stats.get("health")
			if typeof(w) == TYPE_REAL or typeof(w) == TYPE_INT:
				max_hp = float(w)
		var anim = e.get("_animation_player")
		var anim_name := ""
		var anim_pos := -1.0
		var anim_speed := -1.0
		var anim_playing := -1
		if anim != null and is_instance_valid(anim):
			anim_name = str(anim.current_animation)
			anim_pos = float(anim.current_animation_position)
			anim_speed = float(anim.playback_speed)
			anim_playing = 1 if anim.is_playing() else 0
		var parent = e.get_parent()
		var idle_speed := -1.0
		var idle_raw = e.get("_idle_playback_speed")
		if typeof(idle_raw) == TYPE_REAL or typeof(idle_raw) == TYPE_INT:
			idle_speed = float(idle_raw)
		entries.push_back([
			i, e.get_instance_id(), str(e.name), e.get_index(),
			str(parent.name) if parent != null else "<none>",
			1 if e.is_queued_for_deletion() else 0,
			1 if bool(e.get("dead")) else 0,
			pos.x, pos.y, hp, max_hp,
			str(e.enemy_id),
			anim_name, anim_pos, anim_speed, anim_playing,
			1 if bool(e.get("cleaning_up")) else 0,
			idle_speed,
			1 if bool(e.get("_can_move")) else 0,
			1 if bool(e.get("_move_locked")) else 0,
			1 if bool(e.get("_pending_die")) else 0,
			_enemy_diag_behavior_view(e)])
	_enemy_diag_file.store_line(JSON.print({
		"tick": step_index,
		"physics_frames": int(Engine.get_physics_frames()),
		"idle_frames": int(Engine.get_idle_frames()),
		"px": player.global_position.x, "py": player.global_position.y,
		"n": entries.size(), "enemies": entries}))
	_enemy_diag_rows += 1


# Per-attack-behavior view for the death-speed probe: which behavior the enemy
# currently drives, and (for the charging family) the state of the child Timer
# whose `timeout` writes `playback_speed = _idle_playback_speed`
# (charging_attack_behavior.gd:126).  Read-only: no RNG draw, no state touched.
# Entry: [class, is_current, unlock_time_left, unlock_is_stopped, unlock_wait_time]
func _enemy_diag_behavior_view(e) -> Array:
	var views := []
	var all = e.get("_all_attack_behaviors")
	if all == null or typeof(all) != TYPE_ARRAY:
		return views
	var current = e.get("_current_attack_behavior")
	for b in all:
		if b == null or not is_instance_valid(b):
			views.push_back(["<freed>", 0, -1.0, -1, -1.0])
			continue
		var cls := str(b.get_class())
		var script = b.get_script()
		if script != null and str(script.resource_path) != "":
			cls = "%s@%s" % [cls, str(script.resource_path).get_file()]
		var timer = b.get("_unlock_move_timer")
		var t_left := -1.0
		var t_stopped := -1
		var t_wait := -1.0
		if timer != null and is_instance_valid(timer):
			t_left = float(timer.time_left)
			t_stopped = 1 if timer.is_stopped() else 0
			t_wait = float(timer.wait_time)
		views.push_back([cls, 1 if b == current else 0, t_left, t_stopped, t_wait])
	return views


func _write_row(step_index: int, observed: Dictionary, action: int, applied: int,
		choice: Dictionary, policy_result: Dictionary, scene_name: String, wave_timer,
		position_carried: bool) -> void:
	if _movement_probe.empty():
		var probe_player = RLTrace.find_player(get_tree())
		if probe_player != null:
			_movement_probe = _movement_diagnostics(probe_player)
			prints("RLBRIDGE: movement probe ->", JSON.print(_movement_probe))
	var record := _build_record(step_index, observed, action, applied, choice, policy_result,
		scene_name, wave_timer, position_carried)
	_trace.write_record(record)
	_samples += 1
	_update_stats(record, observed)


func _build_record(step_index: int, observed: Dictionary, action: int, applied: int,
		choice: Dictionary, policy_result: Dictionary, scene_name: String, wave_timer,
		position_carried: bool) -> Dictionary:
	var player = RLTrace.find_player(get_tree())
	var position := _last_position
	if player != null:
		position = player.global_position
	var hp: int = int(RunData.get_player_current_health(0))
	var max_hp: int = int(RunData.get_player_max_health(0))
	var living: int = int(RunData.current_living_enemies)
	var enemy_nodes := RLTrace.count_entity_nodes(get_tree(), "enemy")
	var counts: Dictionary = observed["counts"]
	var obs: PoolRealArray = observed["obs"]

	var record := {
		"tick": step_index,
		"action": action,
		"next_action": int(choice["action"]),
		"action_applied": applied,
		"obs": Array(obs),
		"hp": hp,
		"max_hp": max_hp,
		"wave": RunData.current_wave,
		"enemies": living,
		"x": position.x,
		"y": position.y,
		"position_carried": 1 if position_carried else 0,
		"obs_version": 0 if _trace.partial_mode else 5,
		"obs_complete": 0 if _trace.partial_mode else 1,
		"contract": _trace.contract_name(),
		"difficulty": RunData.current_difficulty,
		"zone": RunData.current_zone,
		"gold": RunData.get_player_gold(0),
		"level": RunData.get_player_level(0),
		"xp": RunData.get_player_xp(0),
		"weapon_count": RunData.get_player_weapons(0).size(),
		"living_enemies": living,
		# `living_enemies` above is the GAME's own counter
		# (RunData.current_living_enemies: main.gd:1429 += 1 on spawn, main.gd:478
		# -= 1 on death).  At a debug starting_wave the initial spawn batch can
		# bypass the increment while its deaths still decrement, so that counter can
		# go negative (observed: 0 -> -2 at tick 119 of a wave-20 start).  The two
		# fields below are authoritative: `living_enemies_nodes` is counted from live
		# Enemy nodes (start-wave independent, never negative) and
		# `living_enemies_run_data` keeps the raw game counter for provenance.  The
		# observation's enemy block is already node-derived (rl_obs._collect), so it
		# is not affected by the game counter.
		"living_enemies_nodes": enemy_nodes,
		"living_enemies_run_data": living,
		"living_trees": RunData.current_living_trees,
		"enemy_nodes": enemy_nodes,
		"player_nodes": RLTrace.count_entity_nodes(get_tree(), "player"),
		"pet_nodes": RLTrace.count_entity_nodes(get_tree(), "pet"),
		"projectile_nodes": counts["projectiles"],
		"consumable_nodes": counts["consumables"],
		"wave_in_progress": 1 if RunData.wave_in_progress else 0,
		"wave_time_left": observed_wave_time_left(wave_timer),
		"wave_duration": observed_wave_duration(wave_timer),
		"phase": int(observed["phase"]),
		"action_source": str(choice["source"]),
		"policy_ok": 1 if bool(policy_result.get("ok", false)) else 0,
		"policy_error": str(policy_result.get("error", "")),
		"policy_value": float(policy_result.get("value", 0.0)),
		"policy_error_count": _inject.policy_fallbacks,
		"scene": scene_name
	}
	# The verification verdict must reach the row: without it a reader cannot tell
	# "no mismatch" from "the error was never recorded".  `player_*` is the game's
	# own state at record time, so the game-inhibited case is auditable from the
	# trace alone.
	# The verification verdict must reach the row -- and only for the tick whose
	# comparison produced it: consume_verify_error() clears it, so a shop/level-up
	# row (no comparison) can never repeat the previous wave-end string.
	record["action_verify_error"] = _inject.consume_verify_error()
	if player != null and is_instance_valid(player):
		record["player_can_move"] = 1 if bool(player.get("_can_move")) else 0
		record["player_cleaning_up"] = 1 if bool(player.get("cleaning_up")) else 0
		record["player_move_locked"] = 1 if bool(player.get("_move_locked")) else 0
	# Per-row applied mask: the mask the bridge itself applies at this phase (artifact
	# clauses + conditionals via rl_policy.legal_mask_for), so an eval/replay can
	# assert per-row equality against the sim's own mask ("mask":[31] in
	# tests/brotato_core_trace.cpp output).  Absent when no artifact with mask
	# metadata is loaded -- the bridge then has no mask of its own to report.
	if _policy != null and _policy.loaded and not obs.empty():
		var own_mask: PoolByteArray = _policy.legal_mask_for(obs)
		if not own_mask.empty():
			record["mask"] = Array(own_mask)
	if bool(config["rng_diag"]) and _rng_diag_supported:
		var rng := _rng_counts()
		record["rng_math_draws"] = int(rng["math"])
		record["rng_utils_draws"] = int(rng["utils"])
		record["rng_math_draws_step"] = int(rng["step_math"])
		record["rng_utils_draws_step"] = int(rng["step_utils"])
	# Effect-draw forcing evidence: a non-zero count means a floating text was
	# created despite the pin (it must stay 0), and the driver state is the
	# per-row proof the progression action reached the game.
	record["floating_text_nodes"] = RLTrace.container_child_count(get_tree(), "FloatingTexts")
	if str(config["action_source"]) == "file":
		# Ties this row to the consumed line of the recorded stream.
		record["action_file_index"] = int(_inject.action_file_index)
	if not _last_shop_row.empty():
		record["shop_state"] = _last_shop_row
		_last_shop_row = {}
	if not _last_level_up_row.empty():
		record["level_up_state"] = _last_level_up_row
		_last_level_up_row = {}
	# The observer reports any feature it had to default (missing/non-finite
	# numeric, missing required live-offer UI, ...) in `errors`.  A row with any
	# such error is NOT a complete observation, exactly like an unmapped live
	# offer: the full array is still emitted, but obs_complete drops to 0 and
	# the reasons travel with the row.
	var obs_errors = observed.get("errors")
	if typeof(obs_errors) == TYPE_ARRAY and not obs_errors.empty():
		record["obs_complete"] = 0
		record["obs_errors"] = obs_errors
		_incomplete_rows += 1
		if _incomplete_rows == 1:
			printerr("RLBRIDGE: observation marked incomplete on tick %d: %s" % [step_index, str(obs_errors)])
	else:
		record["obs_errors"] = []

	# A live offer that could not be mapped leaves its slots with the core's
	# empty-slot encoding, so the row is NOT a complete observation.
	var unmapped_live = observed.get("unmapped_live")
	if typeof(unmapped_live) == TYPE_ARRAY and not unmapped_live.empty():
		record["obs_complete"] = 0
		record["obs_unmapped"] = unmapped_live
		_incomplete_rows += 1
		if _incomplete_rows == 1:
			printerr("RLBRIDGE: observation marked incomplete on tick %d: live offers unmapped %s" % [step_index, str(unmapped_live)])
	if _trace.partial_mode:
		record["obs"] = [position.x, position.y, 0.0, 0.0, hp, max_hp]
		record["obs_partial_fields"] = ["x", "y", "unused", "unused", "hp", "max_hp"]
	return record


func observed_wave_time_left(wave_timer) -> float:
	if wave_timer == null or not is_instance_valid(wave_timer):
		return -1.0
	var value = wave_timer.time_left
	return float(value) if typeof(value) == TYPE_REAL or typeof(value) == TYPE_INT else -1.0


func observed_wave_duration(wave_timer) -> float:
	if wave_timer == null or not is_instance_valid(wave_timer):
		return -1.0
	var value = wave_timer.wait_time
	return float(value) if typeof(value) == TYPE_REAL or typeof(value) == TYPE_INT else -1.0


func _update_stats(record: Dictionary, observed: Dictionary) -> void:
	var hp := int(record["hp"])
	var max_hp: int = int(RunData.get_player_max_health(0))
	if _hp_first < 0:
		_hp_first = hp
	_hp_last = hp
	if _hp_min < 0 or hp < _hp_min:
		_hp_min = hp
	_max_hp_seen = max(_max_hp_seen, max_hp)
	var enemy_nodes := int(record["enemy_nodes"])
	_max_enemy_nodes = max(_max_enemy_nodes, enemy_nodes)
	_max_living_enemies = max(_max_living_enemies, int(record["enemies"]))
	_player_seen = true
	if _first_enemy_tick < 0 and (enemy_nodes > 0 or int(record["enemies"]) > 0):
		_first_enemy_tick = int(record["tick"])


# The three settings the RL bridge forces off (rl_run.apply_debug_knobs) plus the
# sites that still draw from a game RNG when they are off.  The game's visual
# sites use the *global* stream (randf/rand_range), which the shared core does not
# model, so a trace records exactly which of them were enabled for this run.
#   off by these settings : effects_manager.gd:53 (hit particles/effect, incl.
#                           randf() at :80), screenshaker.gd:26 (shake, incl. the
#                           per-frame randf() at :39), floating_text_manager.gd:65
#                           (damage numbers -> floating_text.gd:17 rand_range)
#   still drawing when off: floating_text_manager.gd:15/19/25/35/52/57/106
#                           (mutation/level-up/harvest/stat/heal/turret/gold-boost
#                           texts) -> one global rand_range per text through
#                           floating_text.gd:17; display_icon also draws
#                           Utils.get_rand_element() (the seeded Utils stream) for
#                           its sound pick.  Neither is modelled by the core.
func _settings_with_effect_report() -> Dictionary:
	var report: Dictionary = _settings_report.duplicate()
	if typeof(ProgressData.settings) == TYPE_DICTIONARY:
		report["screenshake"] = ProgressData.settings.get("screenshake", null)
		report["visual_effects"] = ProgressData.settings.get("visual_effects", null)
		report["damage_display"] = ProgressData.settings.get("damage_display", null)
	report["effect_rng_sites"] = _effect_rng_off_with_counts()
	return report


func _effect_rng_off_with_counts() -> Dictionary:
	var report := _effect_rng_off.duplicate()
	report["audio_rng"] = int(config["audio_rng"])
	report["audio_rng_meaning"] = "0 = the game's sound draws are neutralized (this run); 1 = they are left intact for the core's brotato_birth_audio/brotato_combat_audio model"
	report["sound_queue_clears"] = _sound_queue_clears
	report["floating_text_pins"] = _floating_text_pins
	report["particle_nodes_registered"] = _particle_nodes.size()
	report["particle_emissions_stopped"] = _particle_emissions_stopped
	report["particle_frames_swept"] = _particle_frames
	return report


func _write_meta() -> void:
	var facts := {}
	if _run_result.has("facts"):
		facts = _run_result["facts"]
	var meta := {
		"bridge_version": BRIDGE_VERSION,
		# Top-level audio parity mode for replay_cpu.py: 0 = this run's sound
		# draws were neutralized (SoundManager/SoundManager2D queues cleared and
		# the floating-text sound arrays emptied), 1 = the game's own sound draws
		# were left in place.  Also present as config.audio_rng.
		"audio_rng": int(config["audio_rng"]),
		# particles_rng states what this run actually did with CPUParticles2D's global
		# draws: 0 = suppression active (the bridge clears `emitting` on every
		# CPUParticles2D/Particles2D from a SceneTree.idle_frame handler before the
		# emitter processes it, so the core's particle model must not draw either).
		# Not a switch -- there is no legacy-particle mode in this build.
		"particles_rng": _particles_rng_effective(),
		"particles_rng_meaning": "0 = the game's per-particle global draws are suppressed by the bridge; gate the core's particle model off for this capture",
		"particles_rng_evidence": "%d particle frame(s) swept, %d CPUParticles2D/Particles2D registered, %d emission(s) stopped" % [
			_particle_frames, _particle_nodes.size(), _particle_emissions_stopped],
		"capture_kind": str(config["capture_kind"]),
		"config_fingerprint": _config_fingerprint(),
		"contract": _trace.contract_name(),
		"obs_complete": false if config["partial"] else true,
		"full_contract_compatible": false if config["partial"] else true,
		"trace_first_line": (("a '%s' header record (partial diagnostics mode)" % RLTrace.HEADER_RECORD)
			if config["partial"] else "a data record (no header)"),
		"obs_definition": _obs_report,
		"record_columns": RLTrace.SAMPLE_COLUMNS,
		"action": {
			"source": config["action_source"],
			"table": _inject_report.get("move_table", {}),
			"semantics": {
				"combat": "0 hold, 1 N, 2 NE, 3 E, 4 SE, 5 S, 6 SW, 7 W, 8 NW (core BT_MOVE_X/BT_MOVE_Y), applied by pressing/releasing the four analog_move_* actions the original movement behavior reads; 9-20 are nonmovement and HOLD, not a diagonal clamp",
				"shop": "0-3 buy that slot (item or weapon), 4 reroll, 8 skip to the next wave, 9-14 recycle owned weapon slot (action-9) through base_shop._on_item_discard_button_pressed, 15-20 combine owned weapon slot (action-15) through base_shop._on_item_combine_button_pressed",
				"level_up": "0-3 choose that offer"
			},
			"injection": _inject_report
		},
		"progression": _progress.report() if _progress != null else null,
		"policy": _policy.report() if _policy != null else null,
		"settings": _settings_with_effect_report(),
		"damage_resolution": _hurt_order_report(),
		"full_unlock": _full_unlock_report,
		"shop_pool": _shop_pool_report,
		"debug_grants": _grants_report,
		"rng_diag": _rng_diag_report(),
		"config": config,
		"run_boot": _run_result,
		"path_global": RLTrace.global_path(config["trace_path"]),
		"meta_path_global": RLTrace.global_path(config["meta_path"]),
		"summary_path_global": RLTrace.global_path(config["summary_path"]),
		"engine": {
			"version": Engine.get_version_info(),
			"iterations_per_second": Engine.iterations_per_second,
			"platform": OS.get_name(),
			"executable": OS.get_executable_path()
		},
		"facts": facts,
		"environment": {
			"editor_feature": OS.has_feature("editor"),
			"game_dir": Utils.get_game_dir(),
			"dlc_preload": _dlc_preload,
			"movement_probe": _movement_probe,
			"viewport_probe": _viewport_probe,
			"user_dir": ProjectSettings.globalize_path("user://")
		}
	}
	var error := RLTrace.write_json_file(config["meta_path"], meta)
	if error != OK:
		prints("RLBRIDGE: WARNING could not write meta file", config["meta_path"], "error", error)


# Policy mode must fail closed: an actual policy test that cannot load its artifact
# or that gets an action rejected must not continue as a scripted substitute, which
# could make a smoke run look like the model worked.  The trace so far is written
# and the process exits non-zero with the reason.
func _fail_policy(tick: int, reason: String) -> void:
	_failure = "policy_inference_failed"
	_failure_message = "tick %d: %s" % [tick, reason]
	_exit_code = 1
	printerr("RLBRIDGE: POLICY FAILURE -", _failure_message)
	_finish("policy_inference_failed")


func _policy_mode() -> bool:
	return str(config.get("action_source", "script")) == "policy"


func _fail_boot(code: String, message: String) -> void:
	_failure = code
	_failure_message = message
	_exit_code = 1
	prints("RLBRIDGE: BOOT FAILED", code, "-", message)
	_finish(code)


func _finish(reason: String) -> void:
	if _finished:
		return
	_finished = true

	# The boot-time snapshot is stale by now; refresh so the summary carries the
	# real attempt/served/applied counters instead of the initial zeros.
	if _inject != null:
		_inject_report = _inject.report()
	if _obs != null:
		# Same reason as the injection report: the boot-time snapshot is stale, so
		# the runtime notes/errors the observer accumulated are refreshed here.
		_obs_report = _obs.report()

	if _trace != null:
		_trace.close()
	if _enemy_diag_file != null:
		_enemy_diag_file.close()
		_enemy_diag_file = null
	if _enemy_diag_events_file != null:
		_enemy_diag_events_file.close()
		_enemy_diag_events_file = null

	var summary := {
		"ok": _failure.empty() and _player_seen,
		"audio_rng": int(config["audio_rng"]),
		"particles_rng": _particles_rng_effective(),
		"capture_kind": str(config["capture_kind"]),
		"config_fingerprint": _config_fingerprint(),
		"reason": reason,
		"failure": _failure,
		"failure_message": _failure_message,
		"exit_code": _exit_code,
		"bridge_version": BRIDGE_VERSION,
		"contract": _trace.contract_name() if _trace != null else "",
		"obs_complete": false if config["partial"] else true,
		"ticks": _ticks,
		"samples": _samples,
		"incomplete_rows": _incomplete_rows,
		"boot_tick": _boot_tick,
		"rows_written": 0 if _trace == null else _trace.rows_written,
		"enemy_diag_rows": _enemy_diag_rows,
		"enemy_diag_events": _enemy_diag_events,
		"enemy_diag_path": str(config.get("enemy_diag", "")),
		"arena_loaded_tick": _arena_loaded_tick,
		"arena_ticks": _arena_ticks,
		"scenes_seen": _scenes_seen,
		"phase_ticks": _phase_counts,
		"waves_seen": RunData.current_wave,
		"player_node_seen": _player_seen,
		"hp_first": _hp_first,
		"hp_last": _hp_last,
		"hp_min": _hp_min,
		"max_hp_seen": _max_hp_seen,
		"first_enemy_tick": _first_enemy_tick,
		"max_enemy_nodes": _max_enemy_nodes,
		"max_living_enemies": _max_living_enemies,
		"obs": _obs_report,
		"action": _inject_report,
		"progression": _progress.report() if _progress != null else null,
		"policy": _policy.report() if _policy != null else null,
		"settings": _settings_with_effect_report(),
		"damage_resolution": _hurt_order_report(),
		"full_unlock": _full_unlock_report,
		"shop_pool": _shop_pool_report,
		"debug_grants": _grants_report,
		"rng_diag": _rng_diag_report(),
		"run_boot": _run_result,
		"environment": {
			"editor_feature": OS.has_feature("editor"),
			"game_dir": Utils.get_game_dir(),
			"dlc_preload": _dlc_preload,
			"movement_probe": _movement_probe,
			"viewport_probe": _viewport_probe,
			"wave_end_probes": _wave_end_probes,
			"user_dir": ProjectSettings.globalize_path("user://")
		},
		"arena_snapshots": _arena_snapshots,
		"config": config,
		"trace_global_path": RLTrace.global_path(config["trace_path"]),
		"meta_global_path": RLTrace.global_path(config["meta_path"])
	}
	var error := RLTrace.write_json_file(config["summary_path"], summary)
	if error != OK:
		prints("RLBRIDGE: WARNING could not write summary file", config["summary_path"], "error", error)

	prints("RLBRIDGE: SUMMARY", JSON.print({
		"ok": summary["ok"],
		"reason": reason,
		"contract": summary["contract"],
		"samples": _samples,
		"waves": RunData.current_wave,
		"phase_ticks": _phase_counts,
		"max_hp_seen": _max_hp_seen,
		"first_enemy_tick": _first_enemy_tick,
		"max_living_enemies": _max_living_enemies,
		"action_applied_total": _inject_report.get("applied_total", 0),
		"progression": _progress.report() if _progress != null else null,
		"failure": _failure
	}))

	if config["quit_at_end"]:
		get_tree().quit(_exit_code)
