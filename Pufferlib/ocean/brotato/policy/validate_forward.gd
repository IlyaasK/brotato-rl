extends SceneTree
# Standalone Godot validation fixture for the exported Brotato policy.
#
# Runs the real BrotatoPolicy recurrent forward pass (brotato_policy.gd) over a
# fixture produced by policy_reference.py's `fixture` command and writes the
# observed logits/value/action per step to JSON for the Python reference check.
#
# Run with the game's Godot binary (3.7.dev1):
#
#   /path/to/Godot --headless -s ocean/brotato/policy/validate_forward.gd -- \
#       --artifact=/tmp/pf/policy.json \
#       --fixture=/tmp/pf/fixture.json \
#       --out=/tmp/pf/godot_out.json
#
# Exit code 0 on success, 1 if the artifact could not be loaded or an expected
# sequence produced an error step. The parity judgement belongs to
# policy_reference.py `check`; this script only exercises the forward pass.

const Policy := preload("brotato_policy.gd")


func _init() -> void:
	var opts := _parse_args(OS.get_cmdline_args())
	for required in ["artifact", "fixture", "out"]:
		if not opts.has(required):
			printerr("validate_forward: missing --%s" % required)
			quit(2)
			return

	var policy = Policy.new()
	var loaded_code = policy.load_artifact(opts["artifact"])
	if loaded_code != OK:
		printerr("validate_forward: artifact load failed: %s" % policy.load_error)
		quit(1)
		return

	var fixture = _read_json(opts["fixture"])
	if fixture == null:
		quit(1)
		return

	var out := {
		"obs_size": policy.obs_size,
		"hidden": policy.hidden,
		"layers": policy.layers,
		"actions": policy.actions,
		"sequences": [],
		"guards": {},
	}

	var failed := false
	for seq in fixture["sequences"]:
		policy.reset()
		# A step's "mask" (optional, one entry per action) is the env's legality
		# vector: the validator must hand the forward the same mask the trainer's
		# rollout saw, so the masked argmax is exercised end to end.
		var seq_masks = seq.get("mask", [])
		var steps := []
		var step_index := 0
		for obs in seq["obs"]:
			var mask := PoolByteArray()
			if step_index < seq_masks.size():
				mask = _to_byte_array(seq_masks[step_index])
			var result = policy.step(_to_real_array(obs), mask)
			step_index += 1
			if result["ok"]:
				steps.append({
					"ok": true,
					"action": result["action"],
					"logits": _real_array_to_json(result["logits"]),
					"masked_logits": _real_array_to_json(result["masked_logits"]),
					"value": result["value"],
				})
			else:
				failed = true
				steps.append({"ok": false, "error": result["error"]})
		out["sequences"].append({"steps": steps})

	# Guard: a wrong-length observation must be rejected and must not mutate
	# the recurrent state; a non-finite observation must be rejected too.
	policy.reset()
	var short_obs := PoolRealArray()
	short_obs.resize(policy.obs_size + 1)
	for i in range(short_obs.size()):
		short_obs[i] = 0.25
	var before = policy.get_state()
	var bad1 = policy.step(short_obs)
	var after1 = policy.get_state()
	policy.reset()
	var nan_obs := PoolRealArray()
	nan_obs.resize(policy.obs_size)
	for i in range(nan_obs.size()):
		nan_obs[i] = 0.25
	nan_obs[0] = NAN
	var before2 = policy.get_state()
	var bad2 = policy.step(nan_obs)
	var after2 = policy.get_state()
	out["guards"] = {
		"wrong_size_ok": bad1["ok"],
		"wrong_size_error": bad1.get("error", ""),
		"nonfinite_ok": bad2["ok"],
		"nonfinite_error": bad2.get("error", ""),
		"state_preserved": _same_real_array(before, after1) and _same_real_array(before2, after2),
	}

	var file := File.new()
	if file.open(opts["out"], File.WRITE) != OK:
		printerr("validate_forward: cannot write %s" % opts["out"])
		quit(1)
		return
	file.store_string(JSON.print(out))
	file.close()
	print("validate_forward: wrote %s (%d sequences)" % [opts["out"], out["sequences"].size()])
	quit(1 if failed else 0)


func _parse_args(args: PoolStringArray) -> Dictionary:
	var opts := {}
	for arg in args:
		var text := str(arg)
		if text.begins_with("--") and text.find("=") > 2:
			var eq := text.find("=")
			opts[text.substr(2, eq - 2)] = text.substr(eq + 1)
	return opts


func _read_json(path: String):
	var file := File.new()
	if file.open(path, File.READ) != OK:
		printerr("validate_forward: cannot open %s" % path)
		return null
	var text := file.get_as_text()
	file.close()
	var parsed := JSON.parse(text)
	if parsed.error != OK:
		printerr("validate_forward: invalid JSON in %s: %s" % [path, parsed.error_string])
		return null
	return parsed.result


func _to_real_array(values) -> PoolRealArray:
	var out := PoolRealArray()
	out.resize(values.size())
	for i in range(values.size()):
		out[i] = float(values[i])
	return out


func _to_byte_array(values) -> PoolByteArray:
	var out := PoolByteArray()
	out.resize(values.size())
	for i in range(values.size()):
		out[i] = int(values[i]) & 0xFF
	return out


func _real_array_to_json(values: PoolRealArray) -> Array:
	var out := []
	for i in range(values.size()):
		out.append(float(values[i]))
	return out


func _same_real_array(a: PoolRealArray, b: PoolRealArray) -> bool:
	if a.size() != b.size():
		return false
	for i in range(a.size()):
		if a[i] != b[i]:
			return false
	return true
