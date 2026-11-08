extends Reference
# Settings repair for the RL bridge.
#
# The retail engine writes user:// settings where every option is read straight
# out of the loaded JSON.  A settings file produced outside a normal play
# session (for example one written while the Steam platform returned null for
# its language, or a partially merged file) can contain JSON nulls; the game
# then dereferences them in ProgressData.apply_settings(), in
# ProgressData._ready (settings.deactivated_dlcs) and in RunData.reset()
# (settings.enemy_scaling), which aborts those functions mid-way.
#
# The repair below is derived from the game's own defaults: the same three
# dictionaries ProgressData.init_settings() merges --
# init_general_options(), init_gameplay_options(), init_accessibilities_options()
# plus the five keys it sets inline -- are rebuilt here and used as the schema.
# Every key the game defines is then checked: a missing key, a null value, or a
# value whose type contradicts the default is replaced by the default; correct
# values (including int/float variants produced by JSON parsing) are kept, and
# keys the game does not define are left untouched.  Nothing else is modified.
#
# This must run before ProgressData.apply_settings() and before
# RunData.reset(), both of which consume those fields.

var repairs: Array = []
var default_key_count := 0


func default_settings() -> Dictionary:
	# Mirrors ProgressData.init_settings() without assigning ProgressData.settings.
	var defaults := {
		"version": ProgressData.VERSION,
		"endless_mode_toggled": false,
		"play_mode": RunData.PlayMode.SOLO,
		"ban_mode_toggled": true,
		"zone_selected": 0
	}
	defaults.merge(ProgressData.init_general_options())
	defaults.merge(ProgressData.init_gameplay_options())
	defaults.merge(ProgressData.init_accessibilities_options())
	return defaults


static func _types_compatible(default_value, current) -> bool:
	var default_type := typeof(default_value)
	var current_type := typeof(current)
	if default_type == current_type:
		return true
	# JSON parsing yields floats where the game's defaults hold ints.
	var default_numeric := default_type == TYPE_INT or default_type == TYPE_REAL
	var current_numeric := current_type == TYPE_INT or current_type == TYPE_REAL
	return default_numeric and current_numeric


func _repair_value(default_value, current, path: String) -> Array:
	if typeof(default_value) == TYPE_DICTIONARY:
		var repaired := {}
		if typeof(current) == TYPE_DICTIONARY:
			for key in current.keys():
				repaired[key] = current[key]
		var changed := false
		for key in default_value.keys():
			var key_name := str(key)
			var child_path: String = (path + "." + key_name) if not path.empty() else key_name
			var child_current = repaired[key] if repaired.has(key) else null
			var child := _repair_value(default_value[key], child_current, child_path)
			if child[1] or not repaired.has(key):
				repaired[key] = child[0]
				changed = true
		return [repaired, changed]

	if current == null or not _types_compatible(default_value, current):
		repairs.push_back("%s = %s (was %s)" % [path, str(default_value), str(current)])
		return [default_value, true]

	return [current, false]


# Repairs ProgressData.settings in place and returns a report for the run meta.
func sanitize_settings(force_english: bool) -> Dictionary:
	repairs.clear()
	var defaults := default_settings()
	default_key_count = defaults.size()
	var result := _repair_value(defaults, ProgressData.settings, "")
	var repaired: Dictionary = result[0]

	if force_english and str(repaired.get("language", "")) != "en":
		repairs.push_back("language = en (was %s)" % str(repaired.get("language", null)))
		repaired["language"] = "en"

	var previous_language = ProgressData.settings.get("language", null)
	ProgressData.settings = repaired

	return {
		"repairs": repairs,
		"repair_count": repairs.size(),
		"default_key_count": default_key_count,
		"language_before": str(previous_language),
		"language_after": str(ProgressData.settings.get("language", null))
	}


# Applies the repaired settings through the game's own entry point.  Fullscreen
# is suppressed via the debug flag the game itself consults so that a headless
# or windowed smoke run is not forced into fullscreen.
func apply_settings_safely(suppress_fullscreen: bool) -> Dictionary:
	if suppress_fullscreen:
		DebugService.no_fullscreen_on_launch = true
	ProgressData.apply_settings()
	return {
		"applied": true,
		"suppress_fullscreen": suppress_fullscreen,
		"fullscreen": ProgressData.settings.get("fullscreen", null),
		"font_size": ProgressData.settings.get("font_size", null),
		"locale": TranslationServer.get_locale()
	}
