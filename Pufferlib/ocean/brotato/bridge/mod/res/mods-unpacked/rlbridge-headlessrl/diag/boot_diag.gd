extends Node
# Boot diagnostics + loader-option repair for the RL bridge.
#
# Design constraints this file satisfies (learned the hard way):
#   * NO loader script is remapped and NOTHING inherits from a remapped path.  An
#     earlier version rewrote res://addons/mod_loader/mod_loader_store.gd.remap and
#     extended that script; because the store's own path then resolved to the
#     extension, the store's script failed to load, ModLoaderStore.ml_options was
#     null, and ModLoader's `if not ModLoaderStore.ml_options.enable_mods` printed
#     "Mods are currently disabled" WITHOUT any --disable-mods argument.
#   * The authoritative autoload binding comes from ProjectSettings
#     ("autoload/ModLoaderStore"), read here at runtime, not from any recovery tool.
#   * Observation uses the Engine main loop, not scene-tree signals, so it does not
#     depend on when autoload nodes are added relative to _ready.
#
# Output: one RLBRIDGE-DIAG line per stage on stdout, and the same JSON written to
# user://rl_bridge_diag.json and to BROTATO_RL_DIAG when that env var is set.
#
# No gameplay is performed.  The only state change is enabling ModLoader's mods
# option through its own dictionary if the loader resolved it false, recorded
# together with the original value.

const DIAG_DEFAULT_PATH := "user://rl_bridge_diag.json"
const POLL_LIMIT := 900

var _base := {}
var _polls := 0
var _done := false
var _repaired := false


static func _write(report: Dictionary) -> void:
	print("RLBRIDGE-DIAG:", JSON.print(report))
	var paths := [DIAG_DEFAULT_PATH]
	var override_path := OS.get_environment("BROTATO_RL_DIAG")
	if override_path != null and not override_path.empty():
		paths.push_back(override_path)
	for path in paths:
		var file := File.new()
		if file.open(path, File.WRITE) == OK:
			file.store_string(JSON.print(report, "\t"))
			file.flush()
			file.close()


static func _autoload_bindings() -> Dictionary:
	var names := ["ModLoaderStore", "ModLoader", "Platform", "RunData", "ItemService", "ProgressData"]
	var out := {}
	for name in names:
		var key: String = "autoload/" + str(name)
		out[name] = str(ProjectSettings.get_setting(key)) if ProjectSettings.has_setting(key) else "<unset>"
	return out


static func _feature_flags() -> Dictionary:
	return {
		"editor": OS.has_feature("editor"),
		"standalone": OS.has_feature("standalone"),
		"steam": OS.has_feature("steam"),
		"epic": OS.has_feature("epic"),
		"gog": OS.has_feature("gog")
	}


# Called from the offline Steam platform's _init (which is known to run: its guard
# output appears in every log).  Prints before any loader node exists.
static func record_base(stage: String) -> Dictionary:
	var report := {
		"stage": stage,
		"engine_version": Engine.get_version_info(),
		"executable": OS.get_executable_path(),
		"cmdline_args": Array(OS.get_cmdline_args()),
		"autoload_bindings": _autoload_bindings(),
		"features": _feature_flags(),
		"user_dir": ProjectSettings.globalize_path("user://"),
		"custom_user_dir_name": str(ProjectSettings.get_setting("application/config/custom_user_dir_name")),
		"use_custom_user_dir": str(ProjectSettings.get_setting("application/config/use_custom_user_dir"))
	}
	_write(report)
	return report


func _ready() -> void:
	_base = record_base("boot_diag._ready (child of the Steam offline platform)")
	set_process(true)
	_open_drawlog()


# --- optional per-physics-frame global-RNG draw log -------------------------
# Diagnostic only, and off unless BROTATO_RL_DRAWLOG names a file to write:
# the diagnostic engine build (tools/godot-src-rngdiag) exposes Math::default_rand's
# draw count as Engine.get_math_rand_draw_count() and RandomNumberGenerator's as
# get_draw_count(), i.e. exactly the two streams the CPU replay's shared core
# models (BtState.rng / BtState.utils_rng).  A frame-by-frame diff against a
# replay therefore localizes the frame where the game draws a different number
# of words.  A normal run sets no BROTATO_RL_DRAWLOG, so nothing is opened and
# nothing is written.
var _drawlog: File = null
var _drawlog_unsupported := false


func _open_drawlog() -> void:
	var path := OS.get_environment("BROTATO_RL_DRAWLOG")
	if path == null or path.empty():
		return
	var file := File.new()
	if file.open(path, File.WRITE) != OK:
		printerr("RLBRIDGE-DIAG: cannot open draw log '%s'" % path)
		return
	_drawlog = file
	_drawlog.store_line("# physics_frame math_draws utils_draws wave wave_time_left")
	_drawlog.flush()


func _physics_process(_delta: float) -> void:
	if _drawlog == null:
		return
	if not Engine.has_method("get_math_rand_draw_count"):
		if not _drawlog_unsupported:
			_drawlog_unsupported = true
			printerr("RLBRIDGE-DIAG: engine has no get_math_rand_draw_count (unpatched build); draw log stays empty")
		return
	var math_draws := int(Engine.get_math_rand_draw_count())
	var utils_draws := -1
	var rng = Utils.get("_rng")
	if rng != null and rng.has_method("get_draw_count"):
		utils_draws = int(rng.get_draw_count())
	var wave := -1
	var time_left := -1.0
	var loop := Engine.get_main_loop()
	var tree := loop as SceneTree
	if tree != null and tree.current_scene != null:
		wave = int(RunData.current_wave)
		var wt = tree.current_scene.get_node_or_null("WaveTimer")
		if wt != null:
			time_left = float(wt.time_left)
	_drawlog.store_line("%d %d %d %d %.6f" % [
		Engine.get_physics_frames(), math_draws, utils_draws, wave, time_left])
	_drawlog.flush()


# Root children as seen through the Engine main loop (what actually exists at the
# moment we look), independent of scene-tree signal timing.
func _root_children() -> Array:
	var loop := Engine.get_main_loop()
	if loop == null:
		return []
	var tree := loop as SceneTree
	if tree == null or tree.root == null:
		return []
	var names := []
	for child in tree.root.get_children():
		names.push_back(str(child.name))
	return names


func _store_node() -> Node:
	var loop := Engine.get_main_loop()
	if loop == null:
		return null
	var tree := loop as SceneTree
	if tree == null or tree.root == null:
		return null
	return tree.root.get_node_or_null("ModLoaderStore")


func _process(_delta: float) -> void:
	if _done:
		return
	_polls += 1
	var store := _store_node()
	if store == null:
		if _polls >= POLL_LIMIT:
			_done = true
			set_process(false)
			var missing_report = _base.duplicate()
			missing_report["stage"] = "ModLoaderStore never appeared"
			missing_report["polls"] = _polls
			missing_report["root_children"] = _root_children()
			missing_report["note"] = "without the store there are no loader options; the mod cannot load"
			_write(missing_report)
		return

	_done = true
	set_process(false)
	var options = store.get("ml_options")
	var options_resource = load("res://addons/mod_loader/options/options.tres")
	var options_profile = null
	if options_resource != null:
		options_profile = options_resource.get("current_options")
	var override_keys := []
	if options_resource != null and options_resource.get("feature_override_options") != null:
		override_keys = options_resource.get("feature_override_options").keys()
	var script = store.get_script()

	var report := _base.duplicate()
	report["stage"] = "ModLoaderStore observed (options resolved; before ModLoader #6 reads them)"
	report["polls_until_store"] = _polls
	report["store_script_path"] = str(script.resource_path) if script != null else "<none: the store's script failed to load>"
	report["ml_options_present"] = typeof(options) == TYPE_DICTIONARY
	report["ml_options"] = options.duplicate() if typeof(options) == TYPE_DICTIONARY else str(options)
	report["enable_mods"] = options.get("enable_mods") if typeof(options) == TYPE_DICTIONARY else null
	report["options_resource_path"] = options_resource.resource_path if options_resource != null else "<load failed>"
	report["options_profile_path"] = options_profile.resource_path if options_profile != null else "<null>"
	report["options_profile_enable_mods"] = options_profile.get("enable_mods") if options_profile != null else null
	report["feature_override_keys"] = override_keys
	report["mod_loader_node_present"] = _store_node() != null
	report["root_children"] = _root_children()

	if typeof(options) == TYPE_DICTIONARY and not bool(options.get("enable_mods", true)):
		options["enable_mods"] = true
		_repaired = true
		report["repaired_enable_mods"] = true
		printerr("RLBRIDGE-DIAG: enable_mods was false (profile=%s, cmdline=%s); repaired to true for this run" % [
			str(report["options_profile_path"]), str(report["cmdline_args"])
		])
	elif typeof(options) != TYPE_DICTIONARY:
		report["repaired_enable_mods"] = false
		report["diagnosis_hint"] = ("ml_options is not a dictionary: the store's script did not initialise. ModLoader's "
			+ "'Mods are currently disabled' is then the null path (not ml_options.enable_mods == false), regardless of argv.")
	else:
		report["repaired_enable_mods"] = false
		report["diagnosis_hint"] = ("options resolved with enable_mods = true, so a disabled message would not come from this "
			+ "store instance; compare options_profile_path, cmdline_args and autoload_bindings before concluding anything.")
	if typeof(options) == TYPE_DICTIONARY and bool(options.get("enable_mods", true)) == false:
		report["diagnosis_hint"] = ("enable_mods resolved false from the options path: check options_profile_path/options_profile_enable_mods "
			+ "and cmdline_args (the loader sets false only for --disable-mods; anything else is engine-level)")
	_write(report)
