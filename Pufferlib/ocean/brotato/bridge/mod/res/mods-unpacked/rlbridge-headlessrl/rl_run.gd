extends Reference
# Canonical, source-faithful run bootstrap for the retail game.
#
# This reproduces exactly the calls the shipped UI makes when a player picks a
# character, a difficulty and then enters the arena.  Sources (decompiled from
# the original PCK, refs recorded in patch_manifest.json / the run meta):
#
#   res://ui/menus/title_screen/title_screen.gd        _ready()
#       RunData.current_zone = 0
#       RunData.reset()
#
#   res://ui/menus/run/character_selection.gd          _on_selections_completed()
#       RunData.add_character(character, player_index)
#       (Bull has weapon_slot == 0 and no starting items, so the shipped UI
#        takes the `RunData.add_starting_items_and_weapons()` branch instead of
#        the weapon-selection screen: see RunData.some_player_has_weapon_slots())
#
#   res://ui/menus/run/weapon_selection.gd             _on_selections_completed()
#       if a starting weapon is chosen:
#           RunData.add_weapon(element.item, player_index, true)   # is_selection
#       RunData.add_starting_items_and_weapons()
#       (BROTATO_RL_WEAPON reproduces the chosen element; with none configured
#        the character's own starting weapons/items are granted, which is what
#        the UI does when the weapon screen completes with nothing selected.)
#
#   res://ui/menus/run/difficulty_selection/difficulty_selection.gd
#                                                      _on_element_pressed()
#       RunData.current_difficulty = element.item.value
#       RunData.reset_elites_spawn()
#       RunData.init_elites_spawn()
#       RunData.enabled_dlcs = ProgressData.get_active_dlc_ids()
#       for effect in element.item.effects: effect.apply(0)
#       players_data[i].uses_ban / remaining_ban_token = ...
#       RunData.init_bosses_spawn()
#       RunData.current_run_accessibility_settings = ProgressData.settings.enemy_scaling.duplicate()
#
# No effect is applied by hand and no cache is bypassed: character effects go
# through RunData.add_character() -> add_item() -> apply_item_effects(), exactly
# as the UI does.  If that path fails, the failure is reported instead of being
# worked around (see `diagnostics`).

var steps: Array = []


func _log(message: String) -> void:
	steps.push_back(message)
	prints("RLBRIDGE:", message)


func _fail(error_code: String, message: String, extra: Dictionary = {}) -> Dictionary:
	_log("FAILED %s: %s" % [error_code, message])
	var result := {
		"ok": false,
		"error": error_code,
		"error_message": message,
		"steps": steps,
		"facts": extra
	}
	return result


# Environment diagnostics recorded on both success and failure so a run can be
# attributed without re-running it.
func environment_facts() -> Dictionary:
	var dlc_ids := []
	for dlc in ProgressData.available_dlcs:
		dlc_ids.push_back(str(dlc.my_id))
	var dlc_dir := Directory.new()
	return {
		"platform_type": Platform.get_type_as_string(),
		"editor_feature": OS.has_feature("editor"),
		"game_dir": Utils.get_game_dir(),
		"steam_singleton_available": Engine.has_singleton("Steam"),
		"available_dlc_ids": dlc_ids,
		"active_dlc_ids": ProgressData.get_active_dlc_ids(),
		"debug_has_dlc": DebugService.has_dlc,
		"dlc_dir_exists": dlc_dir.dir_exists("res://dlcs/"),
		"characters_loaded": ItemService.characters.size(),
		"difficulties_loaded": ItemService.difficulties.size(),
		"load_status": ProgressData.load_status,
		"global_user_dir": ProjectSettings.globalize_path("user://"),
		"engine_iterations_per_second": Engine.iterations_per_second,
		"engine_physics_fps": Engine.iterations_per_second
	}


# Applies the debug-service knobs the RL bridge owns.  Called before
# RunData.reset() because reset() reads DebugService.starting_wave.
func apply_debug_knobs(config: Dictionary) -> void:
	if config["no_save"]:
		DebugService.disable_saving = true
		_log("DebugService.disable_saving = true")
	if config["wave"] > 0:
		DebugService.starting_wave = config["wave"]
		_log("DebugService.starting_wave = %d" % config["wave"])
	if config["instant_waves"]:
		DebugService.instant_waves = true
		_log("DebugService.instant_waves = true")
	if int(config["wave_seconds"]) > 0:
		# main.gd:163-164 overrides the wave timer with this DebugService value:
		# short waves keep enemies (the spawner fires after ~2 s) and still reach
		# shops inside a bounded run.
		DebugService.custom_wave_duration = int(config["wave_seconds"])
		_log("DebugService.custom_wave_duration = %d" % int(config["wave_seconds"]))
	if config["invulnerable"]:
		DebugService.invulnerable = true
		_log("DebugService.invulnerable = true")
	if config["no_focus_pause"]:
		ProgressData.settings.pause_on_focus_lost = false
		_log("ProgressData.settings.pause_on_focus_lost = false")
	# Determinism: the game's visual-effect sites draw from the *global* RNG
	# stream, which the shared core does not model, so any extra draw shifts the
	# stream and every later gameplay value that reads it:
	#   global/effects_manager.gd:79-81   play_hit_effect  -> randf() < effect_scale,
	#                                     rand_range(-1, 1) x2 (reached only through
	#                                     _on_unit_took_damage, gated at :53)
	#   global/screenshaker.gd:36-42      _process         -> randf() x2 per frame
	#                                     (unreachable once shake() returns at :26)
	#   visual_effects/floating_text/floating_text.gd:17
	#                                     display()        -> rand_range(spread)
	#                                     (damage numbers gated at
	#                                     floating_text_manager.gd:65)
	# Turning these three settings off is the game's own switch for those sites.
	# Setting them here runs after ProgressData.apply_settings() (rl_env) and
	# before RunData.reset(), so the run uses them from its first tick; the live
	# values are recorded in the run meta.
	ProgressData.settings.screenshake = false
	ProgressData.settings.visual_effects = false
	ProgressData.settings.damage_display = false
	_log("ProgressData.settings.screenshake/visual_effects/damage_display = false (global-RNG effect sites off)")
	# A capture with no seed is NOT reproducible: the engine's startup
	# randomize() and Utils._rng.randomize() (utils.gd:115) would stand, so any
	# replay of the recording can only be compared up to the first RNG-derived
	# value (observed: a wave-20 deep start diverging at its first enemy spawn).
	# When BROTATO_RL_SEED is unset, pick a seed, APPLY it, and write it back to
	# `config` so it travels with the run meta -- replay_cpu.py replays
	# meta.config.seed, so the recording is reproducible from the meta alone.
	var run_seed := 0
	if not config["seed"].empty() and config["seed"].is_valid_integer():
		run_seed = int(config["seed"])
	else:
		run_seed = OS.get_ticks_usec() & 0x7fffffffffffffff
		config["seed"] = str(run_seed)
		_log("no BROTATO_RL_SEED given: picked and recorded seed %d (an unseeded capture is not replay-comparable)" % run_seed)
	seed(run_seed)
	# Gameplay uses Utils' independent RandomNumberGenerator; seed() alone
	# only controls the global stream used by effects and some spawn helpers.
	Utils.set_rng_seed(run_seed)
	_log("seed(%d), Utils.set_rng_seed(%d)" % [run_seed, run_seed])


# ChallengeData.stat_hash is written by exactly one method,
# challenges/global/challenge_data.gd:16-18 (ChallengeData._generate_hashes():
# `stat_hash = Keys.generate_hash(stat)`), and ChallengeService._generate_hashes()
# only fills my_id_hash (challenge_service.gd:158-163).  The game's own
# game-wide hash pass, ProgressData.add_unlocked_by_default() (progress_data.gd
# ~882-915), calls _generate_hashes() for items, weapons, upgrades and characters
# -- but not for challenges, and a fresh recovery of the retail bytecode contains
# no other caller for them.
#
# Base-game challenges all ship `stat = ""`, so this never mattered before the
# DLC: DLCData.add_resources() (global/dlc_data.gd, ~55-100) appends its
# challenges and calls ChallengeService.set_stat_challenges() WITHOUT generating
# their hashes, and DLC challenges do carry stats (e.g.
# dlcs/dlc_1/challenges/blind_greed_data.tres:24 `stat = "pickup_range"`,
# .../chal_curse `stat = "stat_curse"`).  They therefore enter stat_challenges
# with stat_hash == Keys.empty_hash == 5381, and the first apply_item_effects ->
# ChallengeService.check_stat_challenges() (challenge_service.gd:639-643) calls
# Utils.get_stat(5381, player_index) -> RunData.get_player_effect() indexes the
# player's effects dictionary with 5381, which raises and aborts add_character.
#
# The fix uses the game's own method for the resource class the game's pass
# misses, then rebuilds the stat challenge list exactly as add_resources() does.
# It does not set effects by hand, disable challenges, or suppress errors.
func ensure_challenge_stat_hashes() -> Dictionary:
	var generated := 0
	var skipped := 0
	for challenge in ChallengeService.challenges:
		if challenge == null or not challenge.has_method("_generate_hashes"):
			skipped += 1
			continue
		challenge._generate_hashes()
		generated += 1
	ChallengeService.set_stat_challenges()
	var pending := []
	for challenge in ChallengeService.stat_challenges:
		if challenge == null:
			continue
		var stat_hash = challenge.get("stat_hash")
		if typeof(stat_hash) == TYPE_INT and int(stat_hash) == Keys.empty_hash:
			pending.push_back(str(challenge.get("my_id")))
	if not pending.empty():
		printerr("RLBRIDGE: challenge stat hashes still empty after generation: %s" % str(pending))
	return {
		"challenges": ChallengeService.challenges.size(),
		"generated": generated,
		"skipped": skipped,
		"stat_challenges": ChallengeService.stat_challenges.size(),
		"still_empty": pending
	}


# The canonical run is a FULLY UNLOCKED profile: the core's shop pool is the whole
# lootable catalogue (brotato_weapon_shop.h bt_wshop_generate iterates every
# BTW_LOOTABLE weapon at the rolled tier, independent of the profile), so the
# game's pool must be the whole catalogue too.
#
# mod_main sets DebugService.generate_full_unlocked_save_file before
# ProgressData._ready(), so the game's own branch (progress_data.gd:134) already
# ran ProgressData.unlock_all() + save().  That call is timing-sensitive and
# incomplete on its own: it runs BEFORE the item/weapon/upgrade resources' deferred
# _generate_hashes() (item_parent_data.gd:30-36, weapon_data.gd:25-27,
# upgrade_data.gd:12-14), so
#   * unlock_all_items() pushed a single upgrade (they all still had
#     upgrade_id_hash == Keys.empty_hash), and
#   * unlock_all_weapoons() pushed each weapon's stale my_id_hash while
#     ItemService.init_unlocked_pool() (item_service.gd:124) tests
#     weapon.weapon_id_hash.
# Re-running it now that the hashes exist, plus the loader's own weapon clause
# (progress_data_loader_v3.gd:311-318 pushes weapon_id_hash -- the id the pool
# tests), makes the pool RunData.reset() is about to rebuild the full pool.
func _ensure_full_unlock() -> Dictionary:
	var before := {
		"items_unlocked": ProgressData.items_unlocked.size(),
		"weapons_unlocked": ProgressData.weapons_unlocked.size(),
		"upgrades_unlocked": ProgressData.upgrades_unlocked.size()
	}
	ProgressData.unlock_all()
	var weapon_ids_added := 0
	for weapon in ItemService.weapons:
		var weapon_id_hash = weapon.get_weapon_id_hash()
		if not ProgressData.weapons_unlocked.has(weapon_id_hash):
			ProgressData.weapons_unlocked.push_back(weapon_id_hash)
			weapon_ids_added += 1
	var after := {
		"items_unlocked": ProgressData.items_unlocked.size(),
		"weapons_unlocked": ProgressData.weapons_unlocked.size(),
		"upgrades_unlocked": ProgressData.upgrades_unlocked.size(),
		"characters_unlocked": ProgressData.characters_unlocked.size(),
		"challenges_completed": ProgressData.challenges_completed.size(),
		"weapon_id_hashes_added": weapon_ids_added
	}
	_log("full unlock completed -> %s" % JSON.print(after))
	return {
		"generate_full_unlocked_save_file": DebugService.generate_full_unlocked_save_file,
		"before": before,
		"after": after,
		"method": "ProgressData.unlock_all() + the loader's weapon-id clause (progress_data_loader_v3.gd:311-318), re-run after the resources' deferred _generate_hashes()"
	}


# The profile's unlocked state, plus the catalogue sizes it is measured against.
func full_unlock_report(extra: Dictionary = {}) -> Dictionary:
	var report := {
		"is_unlock_all_save": ProgressData.is_unlock_all_save(),
		"unlocked": {
			"zones": ProgressData.zones_unlocked.size(),
			"characters": ProgressData.characters_unlocked.size(),
			"upgrades": ProgressData.upgrades_unlocked.size(),
			"consumables": ProgressData.consumables_unlocked.size(),
			"weapons": ProgressData.weapons_unlocked.size(),
			"items": ProgressData.items_unlocked.size(),
			"difficulties": ProgressData.difficulties_unlocked.size(),
			"challenges_completed": ProgressData.challenges_completed.size()
		},
		"catalogue": {
			"items": ItemService.items.size(),
			"weapons": ItemService.weapons.size(),
			"upgrades": ItemService.upgrades.size(),
			"consumables": ItemService.consumables.size(),
			"characters": ItemService.characters.size(),
			"zones": ZoneService.zones.size()
		}
	}
	for key in extra.keys():
		report[key] = extra[key]
	return report


# The shop pool the run will draw from: ItemService._tiers_data, exactly the
# structure ItemService.get_pool()/get_player_shop_items() read.  `missing` is
# the direct full-pool check: every catalogue entry that is eligible for the pool
# (can_be_looted items/weapons with max_nb != 0, every upgrade, every
# unlocked_by_default consumable) must be present, and `missing` must be empty.
# TierData (item_service.gd:34): 0 ALL_ITEMS, 1 ITEMS, 2 WEAPONS,
# 3 CONSUMABLES, 4 UPGRADES.
func shop_pool_report() -> Dictionary:
	var tiers = ItemService.get("_tiers_data")
	if typeof(tiers) != TYPE_ARRAY or tiers.size() != 4:
		return {"available": false, "reason": "ItemService._tiers_data is not built"}
	var ids := {"items": {}, "weapons": {"": true}, "upgrades": {}, "consumables": {}}
	ids["weapons"].clear()
	var per_tier := []
	var totals := {"all": 0, "items": 0, "weapons": 0, "consumables": 0, "upgrades": 0}
	for tier in range(tiers.size()):
		var bucket = tiers[tier]
		if typeof(bucket) != TYPE_ARRAY or bucket.size() < 5:
			return {"available": false, "reason": "tier %d is malformed" % tier}
		per_tier.push_back({
			"tier": tier,
			"all": (bucket[0] as Array).size(),
			"items": (bucket[1] as Array).size(),
			"weapons": (bucket[2] as Array).size(),
			"consumables": (bucket[3] as Array).size(),
			"upgrades": (bucket[4] as Array).size()
		})
		totals["all"] += (bucket[0] as Array).size()
		totals["items"] += (bucket[1] as Array).size()
		totals["weapons"] += (bucket[2] as Array).size()
		totals["consumables"] += (bucket[3] as Array).size()
		totals["upgrades"] += (bucket[4] as Array).size()
		for elt in bucket[1]:
			ids["items"][str(elt.my_id)] = true
		for elt in bucket[2]:
			ids["weapons"][str(elt.my_id)] = true
		for elt in bucket[3]:
			ids["consumables"][str(elt.my_id)] = true
		for elt in bucket[4]:
			ids["upgrades"][str(elt.upgrade_id)] = true

	var expected := {"items": 0, "weapons": 0, "upgrades": 0, "consumables": 0}
	var missing := {"items": [], "weapons": [], "upgrades": [], "consumables": []}
	for item in ItemService.items:
		if not item.can_be_looted or item.max_nb == 0:
			continue
		expected["items"] += 1
		if not ids["items"].has(str(item.my_id)):
			missing["items"].push_back(str(item.my_id))
	for weapon in ItemService.weapons:
		if not weapon.can_be_looted:
			continue
		expected["weapons"] += 1
		if not ids["weapons"].has(str(weapon.my_id)):
			missing["weapons"].push_back(str(weapon.my_id))
	for upgrade in ItemService.upgrades:
		expected["upgrades"] += 1
		if not ids["upgrades"].has(str(upgrade.upgrade_id)):
			missing["upgrades"].push_back(str(upgrade.upgrade_id))
	for consumable in ItemService.consumables:
		if not consumable.unlocked_by_default:
			continue
		expected["consumables"] += 1
		if not ids["consumables"].has(str(consumable.my_id)):
			missing["consumables"].push_back(str(consumable.my_id))

	# `=` with an int annotation: a Dictionary-index expression is Variant, so
	# GDScript 3 cannot infer a type for `:=` here (parse error at mod load).
	var missing_total: int = missing["items"].size() + missing["weapons"].size() + missing["upgrades"].size() + missing["consumables"].size()
	return {
		"available": true,
		"source": "ItemService._tiers_data (rebuilt by RunData.reset() -> ItemService.init_unlocked_pool())",
		"totals": totals,
		"per_tier": per_tier,
		"expected": expected,
		"missing": missing,
		"missing_total": missing_total,
		"full_pool": missing_total == 0
	}


func configure(character_id: String, danger: int, zone: int, starting_weapon: String = "", endless: bool = false) -> Dictionary:
	var facts := environment_facts()
	var challenge_hashes := ensure_challenge_stat_hashes()
	facts["challenge_hashes"] = challenge_hashes
	steps.push_back("challenge stat hashes: %s" % str(challenge_hashes))
	prints("RLBRIDGE: challenge stat hashes ->", JSON.print(challenge_hashes))

	# Full unlock: the canonical run is a fully-unlocked profile.  mod_main sets
	# DebugService.generate_full_unlocked_save_file before ProgressData._ready(),
	# so the game's own branch (progress_data.gd:134) already ran unlock_all() +
	# save().  Verify it here and drive the same call as a fallback, BEFORE
	# RunData.reset() rebuilds the shop pool from these sets.
	var unlock_check := _ensure_full_unlock()
	facts["unlock_check"] = unlock_check

	var character = ItemService.get_element(ItemService.characters, Keys.generate_hash(character_id))
	if character == null:
		var by_id = ItemService.get_element_safe(ItemService.characters, character_id)
		return _fail(
			"character_not_found",
			"character '%s' (hash %d) is not in ItemService.characters; is the DLC pack loaded and owned? by_id lookup also returned: %s" % [
				character_id, Keys.generate_hash(character_id), str(by_id)
			],
			facts)

	var difficulty = ItemService.get_element(ItemService.difficulties, Keys.empty_hash, danger)
	if difficulty == null:
		return _fail(
			"difficulty_not_found",
			"no difficulty element with value %d in ItemService.difficulties (size %d)" % [
				danger, ItemService.difficulties.size()
			],
			facts)

	# --- title_screen.gd _ready() -----------------------------------------
	RunData.current_zone = zone
	RunData.reset()
	_log("RunData.current_zone = %d; RunData.reset()" % zone)

	# --- endless mode ------------------------------------------------------
	# Endless is the run menu's own toggle: ui/menus/run/endless_button.gd:8
	# copies ProgressData.settings.endless_mode_toggled into
	# RunData.is_endless_run when the run screen is built, and shop.gd:74 keeps
	# it true once the run is past wave 20.  RunData.reset() clears the flag
	# (run_data.gd:489), so both the setting the UI reads back and the run flag
	# are set after the reset.  run_data.gd:1766 then leaves is_last_wave false
	# and the run continues past wave 20 instead of ending at 20.
	if endless:
		ProgressData.settings.endless_mode_toggled = true
		RunData.is_endless_run = true
		_log("ProgressData.settings.endless_mode_toggled = true; RunData.is_endless_run = true")

	# --- character_selection.gd _on_selections_completed() ----------------
	RunData.add_character(character, 0)
	_log("RunData.add_character(%s, 0)" % character_id)
	if RunData.get_player_character(0) == null:
		return _fail("add_character_failed", "RunData.get_player_character(0) is null after add_character()", facts)

	# --- weapon_selection.gd _on_selections_completed() -------------------
	# An explicit starting weapon is added exactly like the shipped weapon
	# selection screen adds the chosen element: RunData.add_weapon(weapon, 0,
	# is_selection=true).  Then the character's own starting items/weapons are
	# granted.  With no explicit weapon the shipped UI either skips the weapon
	# screen entirely (no weapon slots) or completes it with nothing selected;
	# both paths end in RunData.add_starting_items_and_weapons().
	var weapon_slot_limit := int(RunData.get_player_effect(Keys.weapon_slot_hash, 0))
	facts["weapon_slot_limit"] = weapon_slot_limit
	if not starting_weapon.empty():
		var weapon = ItemService.get_element_safe(ItemService.weapons, starting_weapon)
		if weapon == null or not (weapon is WeaponData):
			return _fail(
				"starting_weapon_not_found",
				"starting weapon '%s' is not in ItemService.weapons (%d weapons loaded)" % [
					starting_weapon, ItemService.weapons.size()],
				facts)
		if weapon_slot_limit <= 0:
			return _fail(
				"starting_weapon_without_slots",
				"starting weapon '%s' configured but character '%s' has 0 weapon slots" % [
					starting_weapon, character_id],
				facts)
		var added = RunData.add_weapon(weapon, 0, true)
		_log("RunData.add_weapon(%s, 0, is_selection=true)" % starting_weapon)
		facts["starting_weapon"] = str(added.my_id) if added != null else starting_weapon
	else:
		facts["starting_weapon"] = ""

	RunData.add_starting_items_and_weapons()
	_log("RunData.add_starting_items_and_weapons()")
	facts["weapon_count"] = RunData.get_player_weapons(0).size()

	# --- difficulty_selection.gd _on_element_pressed() --------------------
	RunData.current_difficulty = difficulty.value
	RunData.reset_elites_spawn()
	RunData.init_elites_spawn()
	RunData.enabled_dlcs = ProgressData.get_active_dlc_ids()
	_log("RunData.current_difficulty = %d (element '%s')" % [danger, str(difficulty.name)])

	var effects_applied := 0
	for effect in difficulty.effects:
		if effect != null:
			effect.apply(0)
			effects_applied += 1
	_log("difficulty effects applied: %d" % effects_applied)

	for player_index in range(RunData.get_player_count()):
		RunData.players_data[player_index].uses_ban = RunData.is_ban_mode_active
		RunData.players_data[player_index].remaining_ban_token = RunData.BAN_MAX_TOKEN

	RunData.init_bosses_spawn()
	RunData.current_run_accessibility_settings = ProgressData.settings.enemy_scaling.duplicate()
	ProgressData.increment_stat("run_started")
	_log("init_bosses_spawn(); accessibility settings copied; run_started stat incremented")

	facts["character_found"] = str(character.my_id)
	facts["character_hash"] = character.my_id_hash
	facts["difficulty_element_name"] = str(difficulty.name)
	facts["difficulty_effects_applied"] = effects_applied
	facts["player_max_hp"] = RunData.get_player_max_health(0)
	facts["player_current_hp"] = RunData.get_player_current_health(0)
	facts["player_level"] = RunData.get_player_level(0)
	facts["player_gold"] = RunData.get_player_gold(0)
	facts["is_endless_run"] = RunData.is_endless_run
	facts["starting_wave"] = RunData.current_wave
	facts["wave"] = RunData.current_wave
	facts["zone"] = RunData.current_zone
	facts["difficulty"] = RunData.current_difficulty
	facts["elites_spawn_count"] = RunData.elites_spawn.size()
	facts["bosses_spawn_count"] = RunData.bosses_spawn.size()
	facts["unlock"] = full_unlock_report()
	facts["shop_pool"] = shop_pool_report()

	return {
		"ok": true,
		"error": "",
		"error_message": "",
		"steps": steps,
		"facts": facts
	}
