extends Reference
# Action selection and movement injection for the RL bridge.
#
# Action vectors are the core's own table (brotato_state.h BT_MOVE_X/BT_MOVE_Y):
#   0 hold, 1 N, 2 NE, 3 E, 4 SE, 5 S, 6 SW, 7 W, 8 NW
#   9..14 recycle owned weapon slot (action-9), 15..20 combine owned weapon slot
#   (action-15).  In the combat phase 9..20 are NONMOVEMENT: they press nothing
#   (a real HOLD, not a diagonal-movement clamp).  They are dispatched in the
#   shop phase by rl_progress through the game's own handlers.
# They are applied by pressing/releasing the four analog movement actions the
# original PlayerMovementBehavior reads, so the game's own movement behavior,
# Unit._can_move, collision and knockback stay in charge (see ANALOG_ACTIONS).
#
# Honesty rules:
#   * `action` recorded in the trace is the action this bridge actually asked
#     for; it is -1 when no source produced one.
#   * `action_applied` is 1 only when the player's own `_current_movement` after
#     the step equals the vector the ENGINE derives from the actions we pressed
#     (Input.get_vector at verify time) - i.e. the engine registered the movement.
#     Hold (action 0) presses nothing, so it can never be "applied"; it is still a
#     real, replayable action, and the first step of a run is seeded as one.
#   * In policy mode, mod_main rejects a non-policy choice and exits with the
#     inference error before applying any scripted fallback. Script mode remains
#     an explicitly selected source, not evidence of policy transfer.

const MOVE_X := [0.0, 0.0, 0.70710678, 1.0, 0.70710678, 0.0, -0.70710678, -1.0, -0.70710678,
	0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
	0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0]
const MOVE_Y := [0.0, -1.0, -0.70710678, 0.0, 0.70710678, 1.0, 0.70710678, 0.0, -0.70710678,
	0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
	0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0]
# Phase-factored action space (31 ids, unchanged by the v5 observation bump):
# 0..8 movement, 9..20 shop recycle/combine,
# 21..24 shop lock toggle, 25..28 shop ban, 29/30 crate take/recycle, level-up 4
# reroll.  Everything past 8 is nonmovement here (the movement table is padded so a
# masked/legal nonmovement id can never be mistaken for a direction).
const ACTION_COUNT := 31
const MOVE_ACTIONS := 9          # 0..8 are movement; 9..30 are nonmovement
const HOLD := 0

# The original PlayerMovementBehavior reads Input.get_vector() over these action
# names (player_movement_behavior.gd get_vector_for_input_prefix("analog_") for a
# single non-coop player), so pressing/releasing exactly these four actions from
# the bridge node is the native input path: MovementBehavior and Unit._can_move
# stay in charge, collision and knockback behave normally, and no private state is
# written.  The 9 core actions map onto the 4 directions the same way the core's
# BT_MOVE_X/BT_MOVE_Y table does.
const ANALOG_ACTIONS := ["analog_move_left", "analog_move_right", "analog_move_up", "analog_move_down"]
const BUTTON_ACTIONS := ["button_move_left", "button_move_right", "button_move_up", "button_move_down"]


# pending injection for the current physics tick
var pending_direction := Vector2.ZERO
var pending_action := -1

# bookkeeping for the last completed injection
var injected_action := -1
var injected_direction := Vector2.ZERO
var injected_frame := -1

# verification of the last completed injection
var applied_action := -1
var applied := 0
var applied_total := 0
var attempts := 0
var verify_mismatches := 0
# Real divergences only.  Inputs the GAME itself refused while it had frozen the
# player (wave-end clean_up / _can_move == false) are counted here instead; both
# leave `applied = 0`, so a replay never sees a registered action that was not.
var verify_game_inhibited := 0
var verify_error := ""
# Set by mod_main on the first arena tick after a shop / level-up phase: the
# player was not simulated while the arena was inactive, so its movement state
# cannot be compared with that tick's injection (see verify()).
var resync_pending := false

var _last_pending_frame := -1
var script_actions := [8, 2, 4, 6, 0, 3, 1, 5, 7, 0]
var input_applications := 0
var last_pressed := []
var script_hold_ticks := 30
var source := "script"
# action_source == "file": a pre-recorded stream, one id per decision point.
var action_file := ""
var action_file_actions := []
var action_file_index := 0
var action_file_lines := 0
var action_file_consumed := 0
var action_file_error := ""
var policy_steps_used := 0
var policy_fallbacks := 0
var last_policy_error := ""


func configure(config: Dictionary) -> void:
	source = str(config.get("action_source", "script"))
	script_hold_ticks = max(1, int(config.get("script_hold_ticks", 30)))
	script_actions.clear()
	for token in str(config.get("script_actions", "")).split(",", false):
		var value: String = str(token).strip_edges()
		if value.is_valid_integer():
			var action := int(value)
			if action >= 0 and action < ACTION_COUNT:
				script_actions.push_back(action)
	if script_actions.empty():
		script_actions = [8, 2, 4, 6, 0, 3, 1, 5, 7, 0]

	action_file = str(config.get("action_file", ""))
	if source == "file":
		_load_action_file(action_file)


# Fail-closed stream loader.  One id per line; the LAST whitespace-separated token
# of the line is the id (so a "<tick> <action>" stream or a bare "action" stream
# both read); blank lines and lines starting with '#' are skipped.  Any failure
# (missing file, unreadable, non-integer token, id outside 0..ACTION_COUNT-1)
# leaves `action_file_error` set with an empty list, so is_ready() is false and
# mod_main aborts the boot instead of silently holding.
func _load_action_file(path: String) -> void:
	action_file_actions.clear()
	action_file_index = 0
	action_file_lines = 0
	action_file_consumed = 0
	action_file_error = ""
	if path.empty():
		action_file_error = "BROTATO_RL_ACTION_FILE is empty"
		return
	var file := File.new()
	if not file.file_exists(path):
		action_file_error = "action file does not exist: %s" % path
		return
	var error := int(file.open(path, File.READ))
	if error != OK:
		action_file_error = "action file could not be opened (error %d): %s" % [error, path]
		return
	while not file.eof_reached():
		var line := file.get_line()
		action_file_lines += 1
		var stripped := line.replace("\t", " ").strip_edges()
		if stripped.empty() or stripped.begins_with("#"):
			continue
		var tokens := stripped.split(" ", false)
		var token := str(tokens[tokens.size() - 1]).strip_edges()
		if not token.is_valid_integer():
			action_file_error = "action file line %d has a non-integer id: '%s'" % [action_file_lines, stripped]
			action_file_actions.clear()
			break
		var id := int(token)
		if id < 0 or id >= ACTION_COUNT:
			action_file_error = "action file line %d id %d is outside 0..%d" % [action_file_lines, id, ACTION_COUNT - 1]
			action_file_actions.clear()
			break
		action_file_actions.push_back(id)
	file.close()
	if action_file_error.empty() and action_file_actions.empty():
		action_file_error = "action file has no usable ids: %s" % path


# True when the selected source has everything it needs to produce actions.
func is_ready() -> bool:
	if source != "file":
		return true
	return action_file_error.empty() and not action_file_actions.empty()


# One id per decision point; EOF holds (HOLD) and never wraps.
func next_file_action() -> int:
	if action_file_index >= action_file_actions.size():
		return HOLD
	var id := int(action_file_actions[action_file_index])
	action_file_index += 1
	action_file_consumed += 1
	return id




func direction_for(action: int) -> Vector2:
	if action < 0 or action >= ACTION_COUNT:
		return Vector2.ZERO
	return Vector2(MOVE_X[action], MOVE_Y[action])


# Called by mod_main once per physics tick, before the player processes.
func set_action(action: int) -> void:
	pending_action = action
	pending_direction = direction_for(action)
	_last_pending_frame = Engine.get_physics_frames() if Engine.has_method("get_physics_frames") else -1
	attempts += 1





func _frame_now() -> int:
	if Engine.has_method("get_physics_frames"):
		return Engine.get_physics_frames()
	return -1


# Verifies that the previous tick's injected direction was actually registered by
# The vector the game itself derives from the currently pressed movement actions:
# the expectation the player's _current_movement must match, so verification asks
# the engine instead of assuming our table's value.
func current_input_vector() -> Vector2:
	return Input.get_vector("analog_move_left", "analog_move_right", "analog_move_up", "analog_move_down")


func expected_direction() -> Vector2:
	return current_input_vector()


# The game's own movement-inhibition state, read from the Player node: wave-end
# clean_up() (main.gd:808-810) sets cleaning_up = true and _can_move = false, and
# unit.gd:137 forces _current_movement to ZERO while !_can_move.  A wave-end
# injection therefore cannot be registered -- that is the game refusing input, not
# a divergence.
func _game_inhibited_player(player) -> bool:
	if player == null or not is_instance_valid(player):
		return false
	if bool(player.get("cleaning_up")):
		return true
	if player.get("_can_move") == false:
		return true
	return false


func verify(player) -> Dictionary:
	# verify() runs before set_action(): pending is the input for the transition
	# that just completed, not the older snapshot from the previous verification.
	injected_action = pending_action
	injected_direction = pending_direction
	injected_frame = _last_pending_frame
	verify_error = ""
	applied = 0
	applied_action = injected_action
	if injected_action < 0:
		verify_error = "no injection yet"
		return report_verify()
	if injected_action == HOLD:
		# HOLD is the native zero input, so nothing can be "registered"; it is a
		# real, replayable action and therefore not an error.
		verify_error = ""
		return report_verify()
	if resync_pending:
		# First arena tick after a shop / level-up phase: the player node was not
		# simulated while the arena was inactive (its behavior did not run), so its
		# _current_movement cannot be compared against this tick's injection yet.
		# Skipping the comparison here is a phase-transition resync, not a
		# movement mismatch; the next tick compares normally.
		resync_pending = false
		verify_error = "phase_transition_resync (no movement comparison on this tick)"
		return report_verify()
	if player == null or not is_instance_valid(player):
		verify_error = "player node unavailable"
		return report_verify()
	# The engine's own input vector is the reference: it is what the movement
	# behavior derived from the actions we pressed for that step.
	var expected := expected_direction()
	if expected == Vector2.ZERO:
		verify_error = ""
		return report_verify()
	var current = player.get("_current_movement")
	if typeof(current) != TYPE_VECTOR2:
		# Keep this visible as a verification failure: an absent field is still an
		# unregistered injection, and it must not be silently absorbed into another
		# counter (HOLD already returns earlier, so this is never the HOLD case).
		verify_mismatches += 1
		verify_error = "player._current_movement unavailable (state: %s)" % str(current)
		return report_verify()
	var current_vec: Vector2 = current
	if not current_vec.is_equal_approx(expected):
		# A genuine divergence only if the game was free to move the player.  At wave
		# end main.gd:808-810 clean_up() sets cleaning_up = true, _can_move = false and
		# _current_movement = ZERO, and unit.gd:137 forces ZERO while !_can_move, so an
		# injected movement CANNOT be registered there.  Classified from the GAME's own
		# state (source guard), counted separately, and `applied` stays 0: this is not
		# a divergence and not a suppression of the failure either.
		if _game_inhibited_player(player):
			verify_game_inhibited += 1
			verify_error = "game_inhibited_input: injected=%s expected=%s current=%s (cleaning_up=%s can_move=%s move_locked=%s)" % [
				str(injected_direction), str(expected), str(current_vec),
				str(player.get("cleaning_up")), str(player.get("_can_move")), str(player.get("_move_locked"))]
			return report_verify()
		verify_mismatches += 1
		verify_error = "movement mismatch: player=%s expected=%s (requested=%s)" % [
			str(current_vec), str(expected), str(injected_direction)]
		return report_verify()
	applied = 1
	applied_total += 1
	return report_verify()


# Authoritative path: press/release the four analog movement actions so the game's
# own MovementBehavior computes the vector.  All four are released first, so the
# action state is unambiguous on every tick (including HOLD, which presses none).
func apply_input(action: int) -> void:
	for name in ANALOG_ACTIONS:
		Input.action_release(name)
	for name in BUTTON_ACTIONS:
		Input.action_release(name)
	last_pressed.clear()
	var dir := direction_for(action)
	if dir.x < -0.01:
		Input.action_press("analog_move_left")
		last_pressed.push_back("analog_move_left")
	elif dir.x > 0.01:
		Input.action_press("analog_move_right")
		last_pressed.push_back("analog_move_right")
	if dir.y < -0.01:
		Input.action_press("analog_move_up")
		last_pressed.push_back("analog_move_up")
	elif dir.y > 0.01:
		Input.action_press("analog_move_down")
		last_pressed.push_back("analog_move_down")
	input_applications += 1


func report_verify() -> Dictionary:
	return {
		"applied": applied,
		"action": applied_action,
		"injected_frame": injected_frame,
		"error": verify_error
	}


# Returns the verdict of the comparison that just ran and clears it, so a row only
# ever carries the error of ITS OWN comparison: shop/level-up rows (which never
# compare) and rows after them would otherwise repeat a stale wave-end string.
func consume_verify_error() -> String:
	var error := verify_error
	verify_error = ""
	return error


# Chooses the action for this tick.  `policy_result` is the outcome of the
# optional policy step (empty when the harness is disabled).
func choose_action(tick: int, policy, policy_result: Dictionary) -> Dictionary:
	if source == "none":
		return {"action": HOLD, "source": "none", "reason": ""}

	if source == "file":
		# One id per decision point from the recorded stream; EOF holds.
		return {"action": next_file_action(), "source": "file", "reason": ""}

	if source == "policy":
		if policy != null and not policy_result.empty() and bool(policy_result.get("ok", false)):
			policy_steps_used += 1
			return {"action": int(policy_result["action"]), "source": "policy", "reason": ""}
		policy_fallbacks += 1
		last_policy_error = str(policy_result.get("error", "policy unavailable"))
		return {"action": _script_action(tick), "source": "script_fallback", "reason": last_policy_error}

	return {"action": _script_action(tick), "source": "script", "reason": ""}


func _script_action(tick: int) -> int:
	if script_actions.empty():
		return HOLD
	var slot := int(tick / script_hold_ticks) % script_actions.size()
	return int(script_actions[slot])


func report() -> Dictionary:
	return {
		"source": source,
		"input_vector_now": str(current_input_vector()),
		"input_applications": input_applications,
		"analog_actions": ANALOG_ACTIONS,
		"attempts": attempts,
		"input_actions_pressed_last": last_pressed,
		"applied_total": applied_total,
		"verify_mismatches": verify_mismatches,
		"verify_game_inhibited": verify_game_inhibited,
		"policy_steps_used": policy_steps_used,
		"policy_fallbacks": policy_fallbacks,
		"last_policy_error": last_policy_error,
		"script_actions": script_actions,
		"script_hold_ticks": script_hold_ticks,
		"action_file": action_file,
		"action_file_lines": action_file_lines,
		"action_file_loaded": action_file_actions.size(),
		"action_file_consumed": action_file_consumed,
		"action_file_remaining": max(0, action_file_actions.size() - action_file_index),
		"action_file_error": action_file_error,
		"move_table": {"x": MOVE_X, "y": MOVE_Y}
	}
