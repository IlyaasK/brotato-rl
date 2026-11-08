extends Reference
# BrotatoPolicy — loads a portable policy artifact (see export_policy.py) and
# runs the exact forward pass of the trained PufferLib network:
#
#   encoder  : linear,  out = x @ W_enc^T          (src/algo.cu:575-584, puf_mm
#              src/algo.cu:137-146)
#   MinGRU   : per layer                            (mingru_gate src/algo.cu:218-244)
#              combined = x @ W^T -> [hidden, gate, proj]
#              gate_sig   = sigmoid_f(gate)
#              hidden_tilde = hidden >= 0 ? hidden + 0.5 : sigmoid_f(hidden)
#              mingru_out = lerp_f(state, hidden_tilde, gate_sig)
#              proj_sig   = sigmoid_f(proj)
#              out        = proj_sig * mingru_out + (1 - proj_sig) * x
#              next_state = mingru_out
#   decoder  : linear,  out = x @ W_dec^T;  first `actions` columns are logits,
#              the last column is the scalar value (src/pufferl.cu:702)
#
# Activation functions match the trainer helpers exactly (src/algo.cu:207/212):
#   sigmoid_f(x): z = exp(-abs(x)); x >= 0 ? 1/(1+z) : z/(1+z)
#   lerp_f(a,b,w): diff=b-a; abs(w) < 0.5 ? a + w*diff : b - diff*(1-w)
#
# Action mask (optional): the environment supplies a per-action legality vector
# (`Agent.action_mask`, src/pufferenv.h:29) that is NOT part of the observation.
# Masked-off logits are replaced by MASKED_LOGIT (-1e4, `load_logit_masked`
# src/algo.cu:1364-1371 — the same value the trainer feeds its softmax) before
# the argmax, so a masked action can never be selected. An empty mask means
# every action is legal.
#
# Recurrent state is kept per layer in the same layer-major order as the
# trainer (`state[layer * hidden + h]`, src/algo.cu:740-746).  Call reset()
# when an episode ends.
#
# No silent fallbacks: a missing/invalid artifact, a wrong observation length,
# a wrong mask length, or a non-finite observation is reported as an error and
# never replaced with zeros.
#
# Clean cutover: an artifact that does not declare its observation_schema, or
# whose declared schema width disagrees with its layout, is rejected outright -
# a padded or resized encoder is never accepted. The schema width and action
# count are the live core's (see the artifact's observation_schema); an artifact
# built at an older schema is refused by the bridge's obs-length check.

const SUPPORTED_FORMAT := "pufferlib-brotato-policy"
const SUPPORTED_VERSION := 1
# src/algo.cu:1364-1371: masked logits are -1e4, not -inf, before the softmax.
const MASKED_LOGIT := -1e4

var obs_size := 0
var hidden := 0
var layers := 0
var actions := 0
var continuous := false
var loaded := false
var load_error := ""
var metadata := {}

var _enc := PoolRealArray()      # [hidden * obs_size]
var _dec := PoolRealArray()      # [(actions + 1) * hidden]
var _mingru := []                # Array of PoolRealArray, each [3*hidden*hidden]
var _state := PoolRealArray()    # [layers * hidden]


func is_ready() -> bool:
	return loaded


# Load and validate an artifact. Returns OK on success; on failure returns the
# File/JSON error code and sets `load_error`. All-or-nothing: on any problem the
# policy is left unloaded.
func load_artifact(path: String) -> int:
	loaded = false
	load_error = ""
	metadata = {}
	_enc = PoolRealArray()
	_dec = PoolRealArray()
	_mingru = []
	_state = PoolRealArray()

	var file := File.new()
	var err := file.open(path, File.READ)
	if err != OK:
		load_error = "cannot open artifact '%s' (error %d)" % [path, err]
		return err
	var text := file.get_as_text()
	file.close()

	var parsed := JSON.parse(text)
	if parsed.error != OK:
		load_error = "invalid JSON at line %d: %s" % [parsed.error_line, parsed.error_string]
		return ERR_PARSE_ERROR
	var doc = parsed.result
	if typeof(doc) != TYPE_DICTIONARY:
		load_error = "artifact root is not an object"
		return FAILED

	if doc.get("format", "") != SUPPORTED_FORMAT:
		load_error = "unsupported artifact format '%s'" % str(doc.get("format", ""))
		return FAILED
	if int(doc.get("format_version", -1)) != SUPPORTED_VERSION:
		load_error = "unsupported artifact version %s" % str(doc.get("format_version", ""))
		return FAILED

	var layout = doc.get("layout", null)
	if typeof(layout) != TYPE_DICTIONARY:
		load_error = "artifact is missing 'layout'"
		return FAILED

	var o := int(layout.get("obs_size", 0))
	var h := int(layout.get("hidden", 0))
	var l := int(layout.get("layers", 0))
	var a := int(layout.get("actions", 0))
	if o <= 0 or h <= 0 or l <= 0 or a <= 0:
		load_error = "invalid layout dimensions (obs=%d hidden=%d layers=%d actions=%d)" % [o, h, l, a]
		return FAILED

	# Clean-cutover guard: the artifact must declare the observation schema it was
	# exported with, and that width must equal the layout width. This rejects a
	# padded / hand-edited artifact whose encoder was resized without a schema.
	# The width itself is the live core's (161); the bridge rejects an artifact
	# whose obs_size does not match the observation it emits.
	var schema = doc.get("observation_schema", null)
	if typeof(schema) != TYPE_DICTIONARY or schema.size() == 0:
		load_error = "artifact has no observation_schema (refusing an artifact of unknown provenance)"
		return FAILED
	var declared_obs := int(schema.get("obs_size", -1))
	if declared_obs != o:
		load_error = "artifact observation_schema.obs_size %d disagrees with layout.obs_size %d (refusing a padded/incompatible artifact)" % [declared_obs, o]
		return FAILED

	var by_name := {}
	var tensors = doc.get("tensors", null)
	if typeof(tensors) != TYPE_ARRAY:
		load_error = "artifact is missing 'tensors'"
		return FAILED
	for entry in tensors:
		if typeof(entry) != TYPE_DICTIONARY:
			load_error = "tensor entry is not an object"
			return FAILED
		var tname := str(entry.get("name", ""))
		if tname == "":
			load_error = "tensor entry has no name"
			return FAILED
		if str(entry.get("dtype", "")) != "f32le":
			load_error = "tensor '%s' has unsupported dtype '%s'" % [tname, str(entry.get("dtype", ""))]
			return FAILED
		if typeof(entry.get("shape", null)) != TYPE_ARRAY:
			load_error = "tensor '%s' has no shape" % tname
			return FAILED
		var bytes := Marshalls.base64_to_raw(str(entry.get("data", "")))
		var values := _decode_f32(bytes)
		var numel := 0
		var shape := []
		for d in entry["shape"]:
			var di := int(d)
			if di <= 0:
				load_error = "tensor '%s' has non-positive dimension" % tname
				return FAILED
			shape.append(di)
			numel = di if numel == 0 else numel * di
		if values.size() != numel:
			load_error = "tensor '%s' has %d floats, shape implies %d" % [tname, values.size(), numel]
			return FAILED
		by_name[tname] = {"shape": shape, "data": values}

	# Encoder weight: [hidden, obs_size].
	var enc = _require(by_name, "encoder.weight", [h, o])
	if enc == null:
		return FAILED
	# Decoder weight: [actions + 1, hidden].
	var dec = _require(by_name, "decoder.weight", [a + 1, h])
	if dec == null:
		return FAILED
	var mingru := []
	for i in range(l):
		var w = _require(by_name, "mingru.%d.weight" % i, [3 * h, h])
		if w == null:
			return FAILED
		mingru.append(w["data"])

	_enc = enc["data"]
	_dec = dec["data"]
	_mingru = mingru
	obs_size = o
	hidden = h
	layers = l
	actions = a
	continuous = bool(layout.get("continuous", false))
	_state = PoolRealArray()
	_state.resize(l * h)
	metadata = {
		"format": doc.get("format", ""),
		"format_version": doc.get("format_version", 0),
		"layout": layout,
		"observation_schema": doc.get("observation_schema", {}),
		"action_map": doc.get("action_map", {}),
		# The artifact's action-mask contract (legal_by_phase, conditionals,
		# selectors, phase_field_index, masked_logit).  rl_policy.gd reads it from
		# here to derive the per-step mask the trainer applies before the argmax;
		# without this key the bridge would feed the network an unmasked logit
		# vector (every id legal), which is not the policy the artifact declares.
		"action_mask": doc.get("action_mask", {}),
		"notes": doc.get("notes", {}),
		"source_checkpoint": doc.get("source_checkpoint", {}),
		"weight_order": doc.get("weight_order", []),
	}
	loaded = true
	return OK


func _require(by_name: Dictionary, name: String, shape: Array):
	if not by_name.has(name):
		load_error = "artifact is missing tensor '%s'" % name
		return null
	var got = by_name[name]["shape"]
	if got.size() != shape.size():
		load_error = "tensor '%s' rank mismatch (got %s, expected %s)" % [name, str(got), str(shape)]
		return null
	for i in range(shape.size()):
		if int(got[i]) != int(shape[i]):
			load_error = "tensor '%s' shape mismatch (got %s, expected %s)" % [name, str(got), str(shape)]
			return null
	return by_name[name]


func _decode_f32(bytes: PoolByteArray) -> PoolRealArray:
	var out := PoolRealArray()
	if bytes.size() % 4 != 0:
		load_error = "tensor byte length %d is not a multiple of 4" % bytes.size()
		return out
	var count := bytes.size() / 4
	out.resize(count)
	var sp := StreamPeerBuffer.new()
	sp.big_endian = false
	sp.data_array = bytes
	for i in range(count):
		out[i] = sp.get_float()
	return out


# Zero the recurrent state. Must be called at episode start / after a terminal.
func reset() -> void:
	if _state.size() > 0:
		for i in range(_state.size()):
			_state[i] = 0.0


# Copy of the current recurrent state (layer-major, `layers * hidden` floats).
# The bridge uses this to snapshot across a terminal; the validator uses it to
# prove that a rejected observation never mutates state.
func get_state() -> PoolRealArray:
	var copy := PoolRealArray()
	copy.resize(_state.size())
	for i in range(_state.size()):
		copy[i] = _state[i]
	return copy


# Replace the recurrent state. Returns OK when the length matches, otherwise the
# state is left untouched and FAILED is returned.
func set_state(state: PoolRealArray) -> int:
	if state.size() != _state.size():
		return FAILED
	for i in range(state.size()):
		_state[i] = state[i]
	return OK


func _sigmoid_f(x: float) -> float:
	var z := exp(-abs(x))
	if x >= 0.0:
		return 1.0 / (1.0 + z)
	return z / (1.0 + z)


func _lerp_f(a: float, b: float, w: float) -> float:
	var diff := b - a
	if abs(w) < 0.5:
		return a + w * diff
	return b - diff * (1.0 - w)


func _linear(x: PoolRealArray, w: PoolRealArray, in_dim: int, out_dim: int) -> PoolRealArray:
	var y := PoolRealArray()
	y.resize(out_dim)
	for o in range(out_dim):
		var acc := 0.0
		var base := o * in_dim
		for i in range(in_dim):
			acc += x[i] * w[base + i]
		y[o] = acc
	return y


# Run one recurrent step. Returns a Dictionary:
#   {"ok": true,  "action": int, "logits": PoolRealArray,
#    "masked_logits": PoolRealArray, "value": float}
#   {"ok": false, "error": String}
# `mask` is an optional per-action legality vector (one byte per action, 1 =
# legal); an empty mask means every action is legal. Masked-off logits become
# MASKED_LOGIT before the argmax, exactly as the trainer does before its
# softmax (src/algo.cu:1364-1371). `logits` stays the raw decoder output, so a
# parity check can compare weights end to end; `masked_logits` is what a
# sampler must use.
# A failure never mutates the recurrent state and never substitutes zeros.
func step(obs: PoolRealArray, mask := PoolByteArray()) -> Dictionary:
	if not loaded:
		return {"ok": false, "error": "policy not loaded"}
	if obs.size() != obs_size:
		return {"ok": false, "error": "observation length %d, expected %d" % [obs.size(), obs_size]}
	if mask.size() != 0 and mask.size() != actions:
		return {"ok": false, "error": "action mask length %d, expected %d or none" % [mask.size(), actions]}
	for i in range(obs.size()):
		var v := obs[i]
		if is_nan(v) or is_inf(v):
			return {"ok": false, "error": "observation[%d] is not finite" % i}

	var x := _linear(obs, _enc, obs_size, hidden)
	var out := PoolRealArray()
	var combined := PoolRealArray()
	for l in range(layers):
		combined = _linear(x, _mingru[l], hidden, 3 * hidden)
		out.resize(hidden)
		var layer_base := l * hidden
		for hi in range(hidden):
			var hid_val := combined[hi]
			var gate_val := combined[hidden + hi]
			var proj_val := combined[2 * hidden + hi]
			var state_val := _state[layer_base + hi]
			var gate_sig := _sigmoid_f(gate_val)
			var tilde = hid_val + 0.5 if hid_val >= 0.0 else _sigmoid_f(hid_val)
			var mingru_out := _lerp_f(state_val, tilde, gate_sig)
			var proj_sig := _sigmoid_f(proj_val)
			_state[layer_base + hi] = mingru_out
			out[hi] = proj_sig * mingru_out + (1.0 - proj_sig) * x[hi]
		x = out

	var dec := _linear(x, _dec, hidden, actions + 1)
	var logits := PoolRealArray()
	var masked_logits := PoolRealArray()
	logits.resize(actions)
	masked_logits.resize(actions)
	var best := 0
	var best_val := -INF
	for ai in range(actions):
		var lv := dec[ai]
		var effective := lv
		if mask.size() != 0 and mask[ai] == 0:
			effective = MASKED_LOGIT
		logits[ai] = lv
		masked_logits[ai] = effective
		if effective > best_val:
			best_val = effective
			best = ai
	return {"ok": true, "action": best, "logits": logits,
		"masked_logits": masked_logits, "value": dec[actions]}
