extends Reference
# Optional policy harness for the RL bridge.
#
# Loads the exported GDScript artifact produced by
# ocean/brotato/policy/export_policy.py (src: ocean/brotato/policy/brotato_policy.gd)
# and runs its forward pass on the bridge's observation vector.
#
# This class owns inference only. Observation rejection never mutates recurrent
# state; rl_inject verifies movement separately from the player's live state.
# mod_main aborts policy mode on loading/inference failure rather than applying
# a scripted substitute.

const POLICY_SCRIPT_PATH := "res://mods-unpacked/rlbridge-headlessrl/brotato_policy.gd"

var policy = null
var loaded := false
var load_error := ""
var obs_size := 0
var actions := 0
var hidden := 0
var layers := 0
var continuous := false
var metadata := {}

# Per-phase legal-id ranges read from the artifact's action_mask.legal_by_phase
# (metres of the sim's bt_action_mask, brotato_core.h).  Empty means "no mask":
# every id is legal, which is what an artifact without the clause gets.
var mask_rules := {}
# action_mask.phase_field_index (the observation slot carrying the phase) and
# the artifact's selectors: a selector replaces the plain "<phase>_<name>"
# clause with `use` while its when_nonzero observation field is nonzero (the
# crate clause is [[29,30]] and REPLACES 0..4 while a box is open, exactly like
# bt_action_mask).  Both indices come from the artifact, never hard-coded.
var mask_phase_field := -1
var mask_selectors := []
# Per-id conditionals (state-dependent legality a static phase range cannot
# express): {clause, id_base, count, tests}.  `clause` is a legal_by_phase key;
# the entry applies only while the resolved clause equals it (so the 2_crate
# selector replaces the 2_level_up rules, exactly like bt_action_legal).  Each
# test AND-combines: the ids are legal iff EVERY test passes.  Tests mirror the
# artifact's schema (export_policy.build_action_mask):
#   {op: "nonzero"|"equals", field_base, field_stride, value?}
#   {op: "block_duplicate", field_base, field_stride, count}
#   {op: "block_count_gte", field_base, field_stride, count, value}
#   {op: "ge", field, other, scale}
var mask_conditionals := []
var masked_steps := 0

# Action selection over the policy's masked logits:
#   "argmax" - the historical greedy pick (brotato_policy.gd's own action);
#   "sample" - (default) the softmax over the masked logits, ONE uniform per
#              decision and cumulative-probability selection.
# The PRNG is this harness's own xorshift32, seeded from the run seed plus a
# constant.  It is deliberately NOT the game's global RNG nor Utils._rng: an
# action draw must never shift a gameplay stream (that would desync the trace).
# The simulator driver (eval_game.py SIM_GLUE_SRC) and policy_reference.py run
# the identical algorithm on the same float32 logits, so with near-equal logits
# the two sides normally pick the same action.
const SAMPLE_SEED_CONST := 0x9E3779B9

var action_mode := "sample"
var sample_seed := 0
var samples_drawn := 0
var _rng_state := 0

var step_calls := 0
var step_ok := 0
var last_error := ""
var last_action := -1
var last_value := 0.0
var schema_warned := false
# legal_by_phase keys are "<phase>_<name>" (PolicyHarness's clause); the name is
# cosmetic, the phase digit is what selects the ranges.
const PHASE_NAMES := {0: "combat", 1: "shop", 2: "level_up"}


func load_artifact(artifact_path: String) -> Dictionary:
	var script = load(POLICY_SCRIPT_PATH)
	if script == null:
		load_error = "policy script missing from the pack: %s" % POLICY_SCRIPT_PATH
		return {"ok": false, "error": load_error}
	policy = script.new()
	var code: int = policy.load_artifact(artifact_path)
	if code != OK:
		load_error = str(policy.load_error)
		return {"ok": false, "error": load_error, "code": code}
	loaded = true
	obs_size = policy.obs_size
	actions = policy.actions
	hidden = policy.hidden
	layers = policy.layers
	continuous = policy.continuous
	metadata = policy.metadata
	mask_rules = {}
	mask_selectors = []
	var action_mask = metadata.get("action_mask")
	if typeof(action_mask) == TYPE_DICTIONARY:
		mask_phase_field = int(action_mask.get("phase_field_index", -1))
		var by_phase = action_mask.get("legal_by_phase")
		if typeof(by_phase) == TYPE_DICTIONARY:
			for key in by_phase.keys():
				var ranges := []
				var entry = by_phase[key]
				if typeof(entry) == TYPE_ARRAY:
					for pair in entry:
						if typeof(pair) == TYPE_ARRAY and pair.size() >= 2:
							ranges.push_back([int(pair[0]), int(pair[1])])
				mask_rules[str(key)] = ranges
		var conds = action_mask.get("conditionals")
		mask_conditionals = []
		if typeof(conds) == TYPE_ARRAY:
			for c in conds:
				if typeof(c) != TYPE_DICTIONARY:
					continue
				var tests := []
				for t in c.get("tests", []):
					if typeof(t) != TYPE_DICTIONARY:
						continue
					tests.push_back({
						"op": str(t.get("op", "nonzero")),
						"field_base": int(t.get("field_base", -1)),
						"field_stride": int(t.get("field_stride", 0)),
						"value": float(t.get("value", 0.0)),
						"count": int(t.get("count", 0)),
						"field": int(t.get("field", -1)),
						"other": int(t.get("other", -1)),
						"scale": float(t.get("scale", 1.0))})
				mask_conditionals.push_back({
					"clause": str(c.get("clause", "")),
					"id_base": int(c.get("id_base", -1)),
					"count": int(c.get("count", 0)),
					"tests": tests})
		var selectors = action_mask.get("selectors")
		if typeof(selectors) == TYPE_ARRAY:
			for sel in selectors:
				if typeof(sel) != TYPE_DICTIONARY:
					continue
				var field_index := -1
				var when = sel.get("when_nonzero")
				if typeof(when) == TYPE_DICTIONARY:
					field_index = int(when.get("index", -1))
				mask_selectors.push_back({
					"phase": int(sel.get("phase", -1)),
					"use": str(sel.get("use", "")),
					"field_index": field_index})
	return {
		"ok": true,
		"error": "",
		"obs_size": obs_size,
		"actions": actions,
		"hidden": hidden,
		"layers": layers,
		"continuous": continuous,
		"artifact": artifact_path
	}


func reset() -> void:
	if loaded and policy.has_method("reset"):
		policy.reset()


# Select the action mode and seed the dedicated sampler.  mod_main calls this
# with config["action_mode"] and the run's resolved seed before the run starts
# (argmax mode still seeds: the sampler is then simply never drawn from).
func configure(mode: String, seed_value: int) -> void:
	action_mode = "argmax" if mode == "argmax" else "sample"
	sample_seed = seed_value
	_rng_state = (seed_value + SAMPLE_SEED_CONST) & 0xFFFFFFFF
	if _rng_state == 0:
		_rng_state = 1
	# Two warm-up draws so the first decision is not a bare seed value.
	_rng_next()
	_rng_next()


# One xorshift32 draw, masked to 32 bits at every step so the 64-bit GDScript
# ints reproduce the driver's uint32_t arithmetic exactly.
func _rng_next() -> int:
	var x := _rng_state
	x = (x ^ (x << 13)) & 0xFFFFFFFF
	x = (x ^ (x >> 17)) & 0xFFFFFFFF
	x = (x ^ (x << 5)) & 0xFFFFFFFF
	_rng_state = x
	return x


# Softmax over the masked logits, one uniform per decision, cumulative
# probability.  Masked-off ids carry MASKED_LOGIT (-1e4), whose exp() underflows
# to exactly 0.0, so an illegal action can never be drawn.
func _sample_action(masked_logits: PoolRealArray) -> int:
	var count := masked_logits.size()
	if count <= 0:
		return -1
	var best := masked_logits[0]
	for i in range(1, count):
		if masked_logits[i] > best:
			best = masked_logits[i]
	var probs := PoolRealArray()
	probs.resize(count)
	var total := 0.0
	for i in range(count):
		var p := exp(masked_logits[i] - best)
		probs[i] = p
		total += p
	var u := (float(_rng_next()) / 4294967296.0) * total
	var acc := 0.0
	for i in range(count):
		acc += probs[i]
		if u < acc:
			samples_drawn += 1
			return i
	samples_drawn += 1
	return count - 1


# Feeds one real observation vector.  Returns the action to record (-1 when the
# policy rejected the observation or is not loaded).
#
# obs[7] is the phase the core reports (0 combat, 1 shop, 2 level-up), so the
# bridge derives the same legality mask the trainer applies without needing the
# driver to pass it: the artifact's action_mask.legal_by_phase ranges for that
# phase are set to 1 and every other id to 0.  The forward replaces masked-off
# logits with MASKED_LOGIT before the argmax, exactly like the trainer
# (src/algo.cu load_logit_masked).  No clause in the artifact -> empty mask ->
# every id legal (the pre-mask behaviour).
# The clause key for this observation: the plain "<phase>_<name>" unless a
# selector for that phase has its when_nonzero field set (then the selector's
# `use` clause replaces it).
func mask_key(obs: PoolRealArray) -> String:
	if mask_phase_field < 0 or obs.size() <= mask_phase_field:
		return ""
	var phase := int(round(obs[mask_phase_field]))
	for sel in mask_selectors:
		if int(sel["phase"]) != phase:
			continue
		var idx: int = int(sel["field_index"])
		if idx >= 0 and idx < obs.size() and obs[idx] != 0.0:
			return str(sel["use"])
	return "%d_%s" % [phase, PHASE_NAMES.get(phase, "phase")]


# One conditional test, evaluated for the k-th id of its block (obs is the same
# vector the mask is derived from).  Mirrors export_policy.build_action_mask.
func _test_passes(t: Dictionary, obs: PoolRealArray, k: int) -> bool:
	var op := str(t["op"])
	if op == "ge":
		var a: int = int(t["field"])
		var b: int = int(t["other"])
		if a < 0 or a >= obs.size() or b < 0 or b >= obs.size():
			return false
		return obs[a] >= obs[b] * float(t["scale"])
	var field_base: int = int(t["field_base"])
	var stride: int = int(t["field_stride"])
	if field_base < 0:
		return false
	if op == "block_count_gte":
		var n := 0
		for j in range(int(t["count"])):
			var idx: int = field_base + j * stride
			if idx >= 0 and idx < obs.size() and obs[idx] != 0.0:
				n += 1
		return n >= int(t["value"])
	var field: int = field_base + k * stride
	if field < 0 or field >= obs.size():
		return false
	if op == "equals":
		return obs[field] == float(t["value"])
	if op == "block_duplicate":
		# a second slot in the block carries the same (nonzero) value
		# (bt_weapon_can_combine); the id field is never 0 for a real copy
		var mine := obs[field]
		if mine == 0.0:
			return false
		for j in range(int(t["count"])):
			if j == k:
				continue
			var other: int = field_base + j * stride
			if other >= 0 and other < obs.size() and obs[other] == mine:
				return true
		return false
	return obs[field] != 0.0


func legal_mask_for(obs: PoolRealArray) -> PoolByteArray:
	var mask := PoolByteArray()
	if actions <= 0:
		return mask
	var ranges = mask_rules.get(mask_key(obs), [])
	if typeof(ranges) != TYPE_ARRAY or ranges.empty():
		return mask
	mask.resize(actions)
	for i in range(actions):
		mask[i] = 0
	for pair in ranges:
		var lo: int = max(0, int(pair[0]))
		var hi: int = min(actions - 1, int(pair[1]))
		for i in range(lo, hi + 1):
			mask[i] = 1
	# conditionals: each entry is scoped to the resolved clause and AND-combines
	# its tests (the sim's bt_action_legal); ids inside the clause are masked
	# unless every test passes.
	var key := mask_key(obs)
	for c in mask_conditionals:
		var clause := str(c["clause"])
		if clause != "" and clause != key:
			continue
		var id_base: int = int(c["id_base"])
		for k in range(int(c["count"])):
			var target: int = id_base + k
			if target < 0 or target >= actions:
				continue
			var ok := true
			for t in c["tests"]:
				if not _test_passes(t, obs, k):
					ok = false
					break
			mask[target] = 1 if ok else 0
	return mask


func step(obs: PoolRealArray) -> Dictionary:
	if not loaded:
		return {"ok": false, "error": load_error, "action": -1}
	step_calls += 1
	if obs.size() != obs_size:
		last_error = "obs_schema_mismatch: bridge observed %d values, artifact expects %d" % [obs.size(), obs_size]
		if not schema_warned:
			schema_warned = true
			prints("RLBRIDGE: policy not fed -", last_error)
		return {"ok": false, "error": last_error, "action": -1}
	var mask := legal_mask_for(obs)
	if not mask.empty():
		masked_steps += 1
	var result: Dictionary = policy.step(obs, mask)
	if not result.get("ok", false):
		last_error = str(result.get("error", "unknown policy error"))
		return {"ok": false, "error": last_error, "action": -1}
	var action := int(result["action"])
	if action_mode == "sample":
		if not result.has("masked_logits"):
			last_error = "policy step returned no masked_logits to sample from"
			return {"ok": false, "error": last_error, "action": -1}
		var sampled := _sample_action(result["masked_logits"])
		if sampled < 0:
			last_error = "sampled action out of range (masked logits empty)"
			return {"ok": false, "error": last_error, "action": -1}
		action = sampled
	step_ok += 1
	last_error = ""
	last_action = action
	last_value = float(result.get("value", 0.0))
	return {
		"ok": true,
		"error": "",
		"action": last_action,
		"value": last_value,
		"logits": result.get("logits", PoolRealArray()),
		"masked_logits": result.get("masked_logits", PoolRealArray()),
		"sampled": action_mode == "sample"
	}


func report() -> Dictionary:
	return {
		"enabled": true,
		"loaded": loaded,
		"load_error": load_error,
		"obs_size": obs_size,
		"actions": actions,
		"hidden": hidden,
		"layers": layers,
		"continuous": continuous,
		"mask_rules": mask_rules.duplicate(),
		"masked_steps": masked_steps,
		"action_mode": action_mode,
		"sample_seed": sample_seed,
		"samples_drawn": samples_drawn,
		"step_calls": step_calls,
		"step_ok": step_ok,
		"last_error": last_error,
		"last_action": last_action,
		"metadata": metadata
	}
