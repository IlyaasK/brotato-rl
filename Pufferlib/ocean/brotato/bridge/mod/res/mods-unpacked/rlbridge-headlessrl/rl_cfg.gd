extends Reference
# Configuration for the RL bridge, read from the process environment.
#
# Every knob is an environment variable so that a harness can spawn the retail
# binary without patching anything else.  `from_env()` returns a plain
# Dictionary; the bridge treats a missing variable as "use the documented
# default" and records the resolved values in the run meta file, so a trace can
# always be attributed to the configuration that produced it.

const DEFAULT_CHARACTER := "character_bull"
const DEFAULT_DANGER := 5
const DEFAULT_ZONE := 0
const DEFAULT_MAX_TICKS := 3600
# Full-observation traces are sampled every physics tick by default: a replay
# consumer (replay_cpu) rightly rejects gaps, so the default must be 1.  Partial
# diagnostics mode may be sparse explicitly via BROTATO_RL_SAMPLE_EVERY.
const DEFAULT_SAMPLE_EVERY := 1

static func _env(key: String) -> String:
	var value: String = OS.get_environment(key)
	if value == null:
		return ""
	return value.strip_edges()

static func env_string(key: String, default_value: String) -> String:
	var value: String = _env(key)
	if value.empty():
		return default_value
	return value

static func env_int(key: String, default_value: int) -> int:
	var value: String = _env(key)
	if not value.is_valid_integer():
		return default_value
	return int(value)

static func env_bool(key: String, default_value: bool) -> bool:
	var value: String = _env(key)
	if value.empty():
		return default_value
	value = value.to_lower()
	return value == "1" or value == "true" or value == "yes" or value == "on"

# Parses "0,1,2,3,8" into [0,1,2,3,8]; ignores junk tokens.
static func _parse_int_list(raw: String) -> Array:
	var out := []
	for token in raw.split(",", false):
		var value: String = str(token).strip_edges()
		if value.is_valid_integer():
			out.push_back(int(value))
	return out


# Parses "item_lootworm, item_garden" into ["item_lootworm", "item_garden"].
static func _parse_string_list(raw: String) -> Array:
	var out := []
	for token in raw.split(",", false):
		var value: String = str(token).strip_edges()
		if not value.empty():
			out.push_back(value)
	return out


# Returns the resolved configuration.  `raw` keeps the raw environment string of
# every knob (including empty strings) for provenance.
static func from_env() -> Dictionary:
	var trace_path := env_string("BROTATO_RL_TRACE", "user://rl_trace.jsonl")
	var meta_path := env_string("BROTATO_RL_META", "")
	if meta_path.empty():
		meta_path = trace_path + ".meta.json"
	var summary_path := env_string("BROTATO_RL_SUMMARY", "")
	if summary_path.empty():
		summary_path = trace_path + ".summary.json"

	# Only the exact "argmax" turns the greedy path back on; every other value
	# (including an unset one) resolves to the sampled default.
	var action_mode := env_string("BROTATO_RL_ACTION_MODE", "sample").strip_edges().to_lower()
	if action_mode != "argmax":
		action_mode = "sample"

	var cfg := {
		"enabled": env_bool("BROTATO_RL", false),
		"character_id": env_string("BROTATO_RL_CHAR", DEFAULT_CHARACTER),
		# Explicit starting weapon: a canonical source id string (e.g.
		# "weapon_pistol"), resolved in-game from ItemService.weapons and added
		# through RunData.add_weapon(..., is_selection=true) then
		# RunData.add_starting_items_and_weapons(), exactly as the shipped weapon
		# selection screen does.  Empty = the character's own starting weapons
		# (Bull: none -> the default zero-slot weaponless run is preserved).
		"starting_weapon": env_string("BROTATO_RL_WEAPON", ""),
		"danger": env_int("BROTATO_RL_DANGER", DEFAULT_DANGER),
		"zone": env_int("BROTATO_RL_ZONE", DEFAULT_ZONE),
		# 0 means "leave DebugService.starting_wave alone" (retail default is 1).
		"wave": env_int("BROTATO_RL_WAVE", 0),
		"trace_path": trace_path,
		"meta_path": meta_path,
		"summary_path": summary_path,
		"max_ticks": env_int("BROTATO_RL_MAX_TICKS", DEFAULT_MAX_TICKS),
		"sample_every": max(1, env_int("BROTATO_RL_SAMPLE_EVERY", DEFAULT_SAMPLE_EVERY)),
		"seed": env_string("BROTATO_RL_SEED", ""),
		"force_english": env_bool("BROTATO_RL_ENGLISH", true),
		"no_save": env_bool("BROTATO_RL_NO_SAVE", true),
		# ProgressData.settings.pause_on_focus_lost defaults to true; an
		# unfocused window would freeze the arena and starve the trace.
		"no_focus_pause": env_bool("BROTATO_RL_NO_FOCUS_PAUSE", true),
		# Repairs JSON-null entries in the loaded settings (language, volume,
		# enemy_scaling, deactivated_dlcs, fullscreen, font_size, ...) using the
		# game's own defaults, then applies them through ProgressData.
		"repair_settings": env_bool("BROTATO_RL_REPAIR_SETTINGS", true),
		"apply_settings": env_bool("BROTATO_RL_APPLY_SETTINGS", true),
		"suppress_fullscreen": env_bool("BROTATO_RL_NO_FULLSCREEN", true),
		# Optional policy harness (see rl_policy.gd).  Off by default: without it
		# no action is ever produced and the trace keeps action = -1.
		# Retail DLC pack path; under the editor feature the game's own DLC lookup
		# resolves to res:// and cannot see it, so the bridge loads it explicitly.
		"dlc_pck": env_string("BROTATO_RL_DLC_PCK", ""),
		"policy": env_bool("BROTATO_RL_POLICY", false),
		# Full 203-float observation (v5: the 161-float core block plus the
		# appended Demon/endless block at 161..202, including the v5 ban budget
		# and level-up presence fields) by default; partial mode writes the
		# 6-value diagnostics trace with its bridge_header marker instead.
		"partial": env_bool("BROTATO_RL_PARTIAL", false),
		"obs_tables": env_string("BROTATO_RL_OBS_TABLES",
			"res://mods-unpacked/rlbridge-headlessrl/brotato_obs_tables.json"),
		# Action source: script (deterministic sequence), policy (falls back to
		# script with the reason recorded) or none (always hold).
		"action_source": env_string("BROTATO_RL_ACTION_SOURCE", "script"),
		# How a policy turns its masked logits into an action (policy source only):
		# "sample" (default) draws from the softmax over the masked logits, one
		# uniform per decision from rl_policy.gd's OWN seeded PRNG (seeded from
		# this run's seed + a constant; never the game's global or Utils RNG, so
		# no gameplay draw is consumed), and picks by cumulative probability --
		# the simulator driver and policy_reference.py run the identical
		# algorithm.  "argmax" is the historical greedy pick.  Anything else
		# resolves to "sample"; the raw value stays in raw_env.
		"action_mode": action_mode,
		"script_actions": env_string("BROTATO_RL_SCRIPT_ACTIONS", "8,2,4,6,0,3,1,5,7,0"),
		"script_hold_ticks": env_int("BROTATO_RL_SCRIPT_HOLD_TICKS", 30),
		# Optional deterministic shop sequence for the scripted source, e.g.
		# "0,1,2,3,8" = try to buy slot 0, then 1, 2, 3, then skip.  Empty (default)
		# delegates to the progression module's own scripted shop behaviour
		# (reroll then skip).  Used to exercise the real purchase path on purpose.
		"script_shop_actions": env_string("BROTATO_RL_SCRIPT_SHOP_ACTIONS", ""),
		"script_shop_actions_list": _parse_int_list(env_string("BROTATO_RL_SCRIPT_SHOP_ACTIONS", "")),
		# Action-list source (action_source=file): a pre-recorded action stream,
		# one id per decision point.  One id per line; the LAST whitespace token of
		# the line is the id; blank lines and lines starting with '#' are skipped.
		# Consumed one per physics frame in combat and one per state tick in the
		# shop/level-up phases; EOF holds (id 0) and never wraps.  A missing,
		# unreadable or unparsable file fails the boot closed.
		"action_file": env_string("BROTATO_RL_ACTION_FILE", ""),
		"policy_artifact": env_string("BROTATO_RL_POLICY_ARTIFACT",
			"res://mods-unpacked/rlbridge-headlessrl/brotato_policy.json"),
		# Full unlock: DebugService.generate_full_unlocked_save_file is set from
		# mod_main._ready() (which runs while ModLoader, autoload #7, enters the
		# tree) so that ProgressData (#10) takes the game's own fully-unlocked
		# branch -- unlock_all() + save() -- instead of load_game_file() +
		# add_unlocked_by_default().  The shop pool
		# (ItemService.init_unlocked_pool() -> ItemService._tiers_data, rebuilt by
		# RunData.reset()) is then the FULL retail pool, which is what the core
		# models.
		"full_unlock": env_bool("BROTATO_RL_FULL_UNLOCK", true),
		# Reads the per-tick global-Math and Utils-RandomNumberGenerator draw
		# counters (diagnostic engine godot-3.7-dev1-rngdiag only; guarded by
		# Engine.has_method) and records them in every trace row.
		"rng_diag": env_bool("BROTATO_RL_RNG_DIAG", true),
		# Determinism probe: when set to a path, every combat row also appends one
		# JSON line describing (a) the engine's physics/idle frame counters and
		# (b) every Enemy node the observer's own tree walk sees, in walk order,
		# with its instance id, sibling index, parent, queued-for-deletion flag,
		# position, health ratio and AnimationPlayer state.  Read-only: it draws no
		# RNG and mutates nothing, so a run with it must be byte-identical to the
		# same run without it.
		"enemy_diag": env_string("BROTATO_RL_ENEMY_DIAG", ""),
		# Deterministic damage resolution (parity oracle).  unit.gd:611 defers the
		# damage for every hurtbox/hitbox overlap, so the ORDER in which
		# simultaneous overlaps are resolved is the physics server's own
		# `area_entered` emission order.  That order is not reproducible across
		# runs (see the block comment on `_on_hurt_ordered`), and it decides which
		# enemy a one-hit projectile damages.  With this on, the bridge takes the
		# scene's hurt signal over and re-queues each step's events in ascending
		# (victim instance id, hitbox instance id) order.
		"order_hurts": env_bool("BROTATO_RL_ORDER_HURTS", true),
		# Debug item/weapon grants: comma-separated source ids added through the
		# game's own RunData.add_item()/add_weapon() after the run is configured.
		"grant_items": env_string("BROTATO_RL_GRANT_ITEMS", ""),
		"grant_items_list": _parse_string_list(env_string("BROTATO_RL_GRANT_ITEMS", "")),
		"grant_weapons": env_string("BROTATO_RL_GRANT_WEAPONS", ""),
		"grant_weapons_list": _parse_string_list(env_string("BROTATO_RL_GRANT_WEAPONS", "")),
		# Deterministic level-up sequence (e.g. "29,30,4,0") so crate take (29) and
		# crate recycle (30), offered by the level-up container's show_item() path,
		# can be exercised on purpose.  Empty = the module's own default.
		"script_level_up_actions": env_string("BROTATO_RL_SCRIPT_LEVELUP_ACTIONS", ""),
		"script_level_up_actions_list": _parse_int_list(env_string("BROTATO_RL_SCRIPT_LEVELUP_ACTIONS", "")),
		# DebugService.always_drop_crates: guarantees crate offers so the 29/30
		# drivers are reachable inside a bounded run.
		"always_crates": env_bool("BROTATO_RL_ALWAYS_CRATES", false),
		# Capture provenance: an explicit kind so a stress/granted capture can never
		# pass for a replay-comparable baseline.  baseline = the canonical config
		# (character + starting weapon, no item/weapon grants, no forced crates, no
		# instant waves); stress/pets/diag/other are recorded as themselves and the
		# bridge fails the boot closed if a baseline capture carries any of them.
		"capture_kind": env_string("BROTATO_RL_CAPTURE_KIND", "other"),
		"instant_waves": env_bool("BROTATO_RL_INSTANT_WAVES", false),
		# audio_rng (0/1) selects the audio parity mode and must match the core's
		# own audio_rng option for the run it is compared with:
		#   0 = neutralized (training/eval): the game's sound draws are suppressed --
		#       both sound queues are emptied before SoundManagerParent._process can
		#       pop a clip (so the per-clip pitch rand_range never runs) and the
		#       floating-text manager's four sound arrays are emptied (so
		#       display_icon's Array.pick_random never runs).
		#   1 = legacy: every sound draw the game makes is left intact, which is
		#       what the core's brotato_birth_audio/brotato_combat_audio model
		#       expects (that model draws one global bounded pick per admitted clip).
		"audio_rng": env_int("BROTATO_RL_AUDIO_RNG", 0),
		"endless": env_bool("BROTATO_RL_ENDLESS", false),
		# DebugService.custom_wave_duration (main.gd:163-164): shortens every wave so
		# a bounded run still reaches shops/level-ups while enemies still spawn,
		# unlike instant_waves which ends the wave before the spawner fires.
		"wave_seconds": env_int("BROTATO_RL_WAVE_SECONDS", -1),
		"invulnerable": env_bool("BROTATO_RL_INVULNERABLE", false),
		"quit_at_end": env_bool("BROTATO_RL_QUIT", true),
		"verbose": env_bool("BROTATO_RL_VERBOSE", true),
		"raw_env": {
			"BROTATO_RL": _env("BROTATO_RL"),
			"BROTATO_RL_CHAR": _env("BROTATO_RL_CHAR"),
			"BROTATO_RL_WEAPON": _env("BROTATO_RL_WEAPON"),
			"BROTATO_RL_DANGER": _env("BROTATO_RL_DANGER"),
			"BROTATO_RL_ZONE": _env("BROTATO_RL_ZONE"),
			"BROTATO_RL_WAVE": _env("BROTATO_RL_WAVE"),
			"BROTATO_RL_TRACE": _env("BROTATO_RL_TRACE"),
			"BROTATO_RL_META": _env("BROTATO_RL_META"),
			"BROTATO_RL_SUMMARY": _env("BROTATO_RL_SUMMARY"),
			"BROTATO_RL_MAX_TICKS": _env("BROTATO_RL_MAX_TICKS"),
			"BROTATO_RL_SAMPLE_EVERY": _env("BROTATO_RL_SAMPLE_EVERY"),
			"BROTATO_RL_SEED": _env("BROTATO_RL_SEED"),
			"BROTATO_RL_ENGLISH": _env("BROTATO_RL_ENGLISH"),
			"BROTATO_RL_NO_SAVE": _env("BROTATO_RL_NO_SAVE"),
			"BROTATO_RL_NO_FOCUS_PAUSE": _env("BROTATO_RL_NO_FOCUS_PAUSE"),
			"BROTATO_RL_REPAIR_SETTINGS": _env("BROTATO_RL_REPAIR_SETTINGS"),
			"BROTATO_RL_APPLY_SETTINGS": _env("BROTATO_RL_APPLY_SETTINGS"),
			"BROTATO_RL_NO_FULLSCREEN": _env("BROTATO_RL_NO_FULLSCREEN"),
			"BROTATO_RL_DLC_PCK": _env("BROTATO_RL_DLC_PCK"),
			"BROTATO_RL_POLICY": _env("BROTATO_RL_POLICY"),
			"BROTATO_RL_PARTIAL": _env("BROTATO_RL_PARTIAL"),
			"BROTATO_RL_OBS_TABLES": _env("BROTATO_RL_OBS_TABLES"),
			"BROTATO_RL_ACTION_SOURCE": _env("BROTATO_RL_ACTION_SOURCE"),
			"BROTATO_RL_ACTION_MODE": _env("BROTATO_RL_ACTION_MODE"),
			"BROTATO_RL_SCRIPT_ACTIONS": _env("BROTATO_RL_SCRIPT_ACTIONS"),
			"BROTATO_RL_SCRIPT_HOLD_TICKS": _env("BROTATO_RL_SCRIPT_HOLD_TICKS"),
			"BROTATO_RL_SCRIPT_SHOP_ACTIONS": _env("BROTATO_RL_SCRIPT_SHOP_ACTIONS"),
			"BROTATO_RL_ACTION_FILE": _env("BROTATO_RL_ACTION_FILE"),
			"BROTATO_RL_POLICY_ARTIFACT": _env("BROTATO_RL_POLICY_ARTIFACT"),
			"BROTATO_RL_INSTANT_WAVES": _env("BROTATO_RL_INSTANT_WAVES"),
			"BROTATO_RL_AUDIO_RNG": _env("BROTATO_RL_AUDIO_RNG"),
			"BROTATO_RL_CAPTURE_KIND": _env("BROTATO_RL_CAPTURE_KIND"),
			"BROTATO_RL_FULL_UNLOCK": _env("BROTATO_RL_FULL_UNLOCK"),
			"BROTATO_RL_RNG_DIAG": _env("BROTATO_RL_RNG_DIAG"),
			"BROTATO_RL_ENEMY_DIAG": _env("BROTATO_RL_ENEMY_DIAG"),
			"BROTATO_RL_ORDER_HURTS": _env("BROTATO_RL_ORDER_HURTS"),
			"BROTATO_RL_GRANT_ITEMS": _env("BROTATO_RL_GRANT_ITEMS"),
			"BROTATO_RL_GRANT_WEAPONS": _env("BROTATO_RL_GRANT_WEAPONS"),
			"BROTATO_RL_SCRIPT_LEVELUP_ACTIONS": _env("BROTATO_RL_SCRIPT_LEVELUP_ACTIONS"),
			"BROTATO_RL_ALWAYS_CRATES": _env("BROTATO_RL_ALWAYS_CRATES"),
			"BROTATO_RL_ENDLESS": _env("BROTATO_RL_ENDLESS"),
			"BROTATO_RL_WAVE_SECONDS": _env("BROTATO_RL_WAVE_SECONDS"),
			"BROTATO_RL_INVULNERABLE": _env("BROTATO_RL_INVULNERABLE"),
			"BROTATO_RL_QUIT": _env("BROTATO_RL_QUIT"),
			"BROTATO_RL_VERBOSE": _env("BROTATO_RL_VERBOSE")
		}
	}
	return cfg
