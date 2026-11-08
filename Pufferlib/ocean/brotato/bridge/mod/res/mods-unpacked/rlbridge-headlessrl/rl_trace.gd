extends Reference
# Trace writer for the RL bridge.
#
# Two artifacts, one writer:
#
#   full (default)  - one JSON object per completed physics step with the full
#                     203-float core observation (rl_obs.gd; v5 appended block
#                     161..202) and the contract
#                     fields required by tests/brotato_trace_compare.py:
#                     tick, action, obs, hp, wave, enemies, x, y.  No header line,
#                     so a full-contract validator accepts the file.
#   partial         - diagnostics only (BROTATO_RL_PARTIAL=1): a 6-value partial
#                     observation preceded by a `bridge_header` record that makes
#                     the file structurally invalid for a full-observation
#                     validator.  Nothing is padded.
#
# Column provenance is documented in ocean/brotato/bridge/state_contract.json and
# repeated in the run meta file; every value is read live from the game.

const PARTIAL_CONTRACT_NAME := "partial_v0"
const FULL_CONTRACT_NAME := "full_v5"
const PARTIAL_OBS_LENGTH := 6
const FULL_OBS_LENGTH := 203
const HEADER_RECORD := "bridge_header"

const SAMPLE_COLUMNS := [
	"tick", "action", "next_action", "action_applied", "obs", "hp", "max_hp", "wave", "enemies",
	"x", "y", "obs_version", "obs_complete", "contract", "difficulty", "zone", "gold",
	"level", "xp", "weapon_count", "living_enemies", "living_trees", "enemy_nodes", "player_nodes",
	"pet_nodes", "projectile_nodes", "consumable_nodes", "wave_in_progress",
	"wave_time_left", "wave_duration", "phase", "action_source", "policy_ok",
	"policy_error", "policy_value", "policy_error_count", "scene",
	"rng_math_draws", "rng_utils_draws", "rng_math_draws_step", "rng_utils_draws_step",
	"floating_text_nodes", "shop_state", "level_up_state", "action_file_index",
	"action_verify_error", "player_can_move", "player_cleaning_up", "player_move_locked",
	"living_enemies_nodes", "living_enemies_run_data", "mask"
]

var path := ""
var open_error := 0
var rows_written := 0
var header_written := false
var partial_mode := false

var _file = null


func configure(partial: bool) -> void:
	partial_mode = partial


func contract_name() -> String:
	return PARTIAL_CONTRACT_NAME if partial_mode else FULL_CONTRACT_NAME


func obs_length() -> int:
	return PARTIAL_OBS_LENGTH if partial_mode else FULL_OBS_LENGTH


# Opens the trace for writing.  Returns true on success; `open_error` holds the
# File.open() error code otherwise.
func open_trace(trace_path: String) -> bool:
	path = trace_path
	var file := File.new()
	open_error = file.open(path, File.WRITE)
	if open_error != OK:
		return false
	_file = file
	return true


func is_open() -> bool:
	return _file != null


# Writes the partial-contract header as the first line (partial mode only).
func write_header(extra: Dictionary = {}) -> int:
	if _file == null:
		return ERR_UNCONFIGURED
	if not partial_mode:
		return OK
	var header := {
		"record": HEADER_RECORD,
		"contract": PARTIAL_CONTRACT_NAME,
		"contract_version": 0,
		"obs_complete": 0,
		"obs_length": PARTIAL_OBS_LENGTH,
		"full_contract_compatible": 0,
		"note": "partial diagnostics trace: this header line is not a data record, so a full-observation contract validator cannot accept this file"
	}
	for key in extra.keys():
		header[key] = extra[key]
	_file.store_line(JSON.print(header))
	_file.flush()
	header_written = true
	return OK


func write_record(record: Dictionary) -> void:
	if _file == null:
		return
	_sanitize_non_finite(record)
	_file.store_line(JSON.print(record))
	rows_written += 1
	if rows_written % 10 == 0:
		_file.flush()


# JSON has no NaN/Inf literal, so a non-finite observation value would make the
# whole line unparseable for every consumer (parse_trace.py, the Python
# comparator, the replayer).  A non-finite value is therefore replaced by 0.0 and
# the row is marked incomplete with the offending indices recorded: the trace
# stays readable and never claims a complete observation it does not have.
# (parse_trace.py --check rejects obs_complete = 0, so such a run fails loudly.)
func _sanitize_non_finite(record: Dictionary) -> void:
	var obs = record.get("obs")
	if obs == null:
		return
	var bad := []
	for index in range(obs.size()):
		var value := float(obs[index])
		if is_nan(value) or is_inf(value):
			bad.push_back(index)
			obs[index] = 0.0
	record["obs"] = obs
	if bad.empty():
		return
	record["obs_complete"] = 0
	var errors = record.get("obs_errors")
	if typeof(errors) != TYPE_ARRAY:
		errors = []
	record["obs_errors"] = errors
	errors.push_back("non-finite observation value(s) at index %s replaced with 0.0" % str(bad))


func close() -> void:
	if _file == null:
		return
	_file.flush()
	_file.close()
	_file = null


# ---------------------------------------------------------------------------
# Live scene inspection
# ---------------------------------------------------------------------------

static func current_scene(tree: SceneTree) -> Node:
	if tree == null:
		return null
	return tree.current_scene


# Walks the scene below `root` and returns the first `Player` node that is
# inside the tree.  Player nodes are not added to any group by the game, so the
# lookup is done on the script class.
static func find_player(tree: SceneTree) -> Player:
	var root := current_scene(tree)
	if root == null:
		return null
	# The walker is Node-typed so it parses; the cast happens at this boundary.
	return _find_first_player(root, 0) as Player


static func _find_first_player(node: Node, depth: int) -> Node:
	if depth > 32:
		return null
	if node is Player and is_instance_valid(node):
		return node
	for child in node.get_children():
		var found := _find_first_player(child, depth + 1)
		if found != null:
			return found
	return null


static func count_entity_nodes(tree: SceneTree, kind: String) -> int:
	var root := current_scene(tree)
	if root == null:
		return 0
	return _count_entity_nodes(root, kind, 0)


static func _count_entity_nodes(node: Node, kind: String, depth: int) -> int:
	if depth > 32:
		return 0
	var count := 0
	match kind:
		"player":
			if node is Player:
				count += 1
		"enemy":
			if node is Enemy:
				count += 1
		"neutral":
			if node is Neutral:
				count += 1
		"structure":
			if node is Structure:
				count += 1
		"pet":
			if node is Pet:
				count += 1
	for child in node.get_children():
		count += _count_entity_nodes(child, kind, depth + 1)
	return count


static func container_child_count(tree: SceneTree, container_name: String) -> int:
	var scene := current_scene(tree)
	if scene == null:
		return 0
	var container := scene.get_node_or_null(container_name)
	if container == null:
		return 0
	return container.get_child_count()


# ---------------------------------------------------------------------------
# Side files
# ---------------------------------------------------------------------------

static func write_json_file(path: String, value) -> int:
	var file := File.new()
	var error := file.open(path, File.WRITE)
	if error != OK:
		return error
	file.store_string(JSON.print(value, "	"))
	file.flush()
	file.close()
	return OK


static func global_path(path: String) -> String:
	if path.begins_with("res://") or path.begins_with("user://"):
		return ProjectSettings.globalize_path(path)
	return path
