extends Reference
# The full observation, ported 1:1 from bt_core_observe
# (ocean/brotato/brotato_core.h) + bt_weapon_observe (brotato_weapon_observe.h)
# and the schema in brotato_state.h:
#
#   [ 0..10] player   hp/max_hp, max_hp/64, regen/30, armor/20, px/2048, py/1536,
#                     iframe>0, phase, materials/100, level/20, xp/xp_need
#   [11..26] shop     per slot: tier/4, materials>=price, dominant_stat/10,
#                              clamp(dominant_value*0.1, -1, 1)
#   [27..34] level-up per slot: stat/12, clamp(value*0.1, -1, 1)
#   [35..64] enemies  six nearest by squared distance:
#                     (ex-px)/2048, (ey-py)/1536, ehp/emax, edmg*0.1, elite
#   [65..67] wave     wave/20, time_left/wave_duration, enemy_count/100
#   [68..76] projectiles three nearest with squared distance < 70000:
#                     dx/2048, dy/1536, dmg*0.1
#   [77..78] consumable nearest: dx/2048, dy/1536
#   [79..126] weapons  six owned slots x 8 (FROZEN map, WeaponSchema):
#                     id (canon+1)/(N_WEAPONS+1), tier/4, kind, cooldown_left/cooldown,
#                     damage*0.1, max_range/2048, crit_chance, cursed; empty slot = 8 zeros
#   [127..138] offers four shop offers x 3: type (0 empty/1 item/2 weapon),
#                     (canonical id+1)/(N+1) on the matching table, cursed
#   [139..140] profile character id (id+1)/(N_CHARACTERS+1), weapon slot limit/6
#   [141..160] shop action: currency flag, four offers x (price, eligible,
#                     affordable, buyable), reroll (price, free, available)
#   [161..202] Demon/endless target block (brotato_state.h BT_OBS_TARGET):
#                     harvesting/luck/endless squashed/wave, per-slot offer lock
#                     flags, crate offer (kind/id/cursed/ready), level-up reroll
#                     price, 24 guide-item owned counts, then the v5 tail:
#   [198]             ban tokens / BAN_MAX_TOKEN (0.0 when the ban UI is off)
#   [199..202]        level-up offer presence flag per slot (1.0 = a real offer)
#
# Every value comes from the live game: player/RunData accessors, the `Enemy`
# nodes in the scene, the `EnemyProjectiles` and `Consumables` containers, and
# the item/upgrade tables exported from the core's own generated headers
# (gen_obs_tables.py).  Nothing is padded with invented numbers: slots for
# absent enemies/projectiles/consumables are written as the core writes them
# (zero), and an empty shop/level-up slot uses the core's own empty-slot
# encoding, which is BT_ITEMS[0] / BT_UPGRADES[0]; non-item shop slots
# project to item index 0 in bt_core_observe.
#
# The level-up block is transcribed from bt_core_observe, not approximated:
# slot i carries BT_UPGRADES[offer].stat / 12.0f (the core divides by 12
# whatever the stat id is, so ids 12..18 exceed 1.0) and
# clamp(BT_UPGRADES[offer].value * 0.1f, -1, 1).  A live offer is therefore
# mapped through the emitted stat contract (`stat_ids`: game effect key -> core
# stat id) onto the same (stat, tier) row, and every failure to do so is
# reported with the offer's id, effect keys and the reason instead of falling
# back silently to the empty-slot encoding.
#
# Known divergence from the core, reported in the run meta:
#   * enemy ordering follows the scene tree (spawn order) rather than the core's
#     SoA index order, so exact ties between equidistant enemies may pick a
#     different one.

const OBS_SIZE := 203
# Demon/endless block (obs[161..202]), mirroring bt_core_observe's
# BT_OBS_TARGET block in brotato_core.h: 4 stat/scale fields, 9 shop-owned
# fields (offer lock flags, crate offer, level-up reroll price), 24 curated
# guide-item owned counts, then the v5 tail -- the ban-token budget and the four
# level-up offer presence flags.
const OBS_TARGET_STATS := 4
const OBS_TARGET_RESERVED := 9
const OBS_TARGET_ITEMS := 24
const OBS_TARGET_BAN := 1
const OBS_TARGET_LEVEL_UP := 4
const OBS_TARGET_BASE := 161
const OBS_TARGET_RESERVED_BASE := OBS_TARGET_BASE + OBS_TARGET_STATS
const OBS_TARGET_ITEMS_BASE := OBS_TARGET_RESERVED_BASE + OBS_TARGET_RESERVED
const OBS_TARGET_BAN_BASE := OBS_TARGET_ITEMS_BASE + OBS_TARGET_ITEMS
const OBS_TARGET_LEVEL_UP_BASE := OBS_TARGET_BAN_BASE + OBS_TARGET_BAN
# Curated ArosRising High-Endless guide items, in BT_OBS_GUIDE_ITEMS order
# (brotato_core.h).  Counts come from RunData.get_nb_item(String.hash(), 0):
# Keys.generate_hash is String.hash() (keys.gd:434-441) and
# ItemParentData.my_id_hash is Keys.generate_hash(my_id) (item_parent_data.gd:35).
const GUIDE_ITEM_IDS := [
	"item_bag", "item_crown", "item_hunting_trophy", "item_coupon",
	"item_dangerous_bunny", "item_recycling_machine", "item_lure", "item_anvil",
	"item_alien_eyes", "item_garden", "item_improved_tools",
	"item_explosive_shells", "item_plastic_explosive", "item_alien_tongue",
	"item_sad_tomato", "item_torture", "item_padding", "item_tardigrade",
	"item_silver_bullet", "item_grinds_magical_leaf", "item_vigilante_ring",
	"item_bloody_hand", "item_spicy_sauce", "item_alien_magic"]
const WEAPON_SLOTS := 6
const WEAPON_FIELDS := 8
const SHOP_OFFERS := 4
const SHOP_OFFER_FIELDS := 3
const SHOP_ACTION_FIELDS := 1 + SHOP_OFFERS * 4 + 3
const WEAPON_NONE := 0xFFFF
const SHOP_OFFER_EMPTY := 0.0
const SHOP_OFFER_ITEM := 1.0
const SHOP_OFFER_WEAPON := 2.0
const WEAPON_SLOT_LIMIT_DIVISOR := 6.0
# Compiled defaults mirror the core revision this port was written against; the
# values actually used are refreshed from brotato_obs_tables.json's
# core_revision.obs_constants when that artifact is loaded, so the bridge scales
# exactly like the core headers the tables were generated from.
const DEFAULT_ARENA_W := 2048.0
const DEFAULT_ARENA_H := 1536.0
const DEFAULT_WAVES := 20
const DEFAULT_MAX_ENEMIES := 100
# Policy-visible scale is independent of implementation pool capacity BT_MAX.
const ENEMY_COUNT_DENOMINATOR := 100.0
const SLOTS := 6
const PROJ_SLOTS := 3
const PROJ_MAX_D2 := 70000.0
const INACTIVE_SHOP_SLOT := 0
const INACTIVE_OFFER_SLOT := 0
# The core's own empty-slot level-up encoding (BT_UPGRADES[0]): stat 0 (max_hp),
# value 3.0 -> (0.0, 0.3).  Asserted against the tables at load.
const INACTIVE_OFFER_STAT := 0
const INACTIVE_OFFER_VALUE := 3.0
# brotato_items.h:34 BT_STAT_NONE.  brotato_upgrades.h:140-144: a BT_UPGRADES row
# is stored with stat == BT_STAT_NONE when the effect key has no core stat, which
# is exactly the weapon-slot upgrade (items/upgrades/weapon_slot/
# weapon_slot_effect.tres, key "weapon_slot", value 1, tier 0) that
# ItemService.get_upgrades() returns as the ONLY level-up offer while a
# weapon-slot upgrade is pending (item_service.gd:550-556).
const BT_STAT_NONE := 255

var tables := {}
var table_error := ""
var notes := []
var unmapped: Array = []
# live offers that were present but could not be mapped onto the core tables;
# the caller must mark such a row obs_complete = 0 instead of claiming a
# complete observation that silently fell back to the empty-slot encoding.
var unmapped_live: Array = []
var sample_errors: Array = []

# Stat contract emitted from the core's own BT_STAT_KEYS / BT_STAT_NAMES
# (index == core stat id): game effect key -> core stat id, plus the display
# name used in diagnostics.
var stat_ids := {}
var stat_names: Array = []
# BT_UPGRADES rows indexed by (stat, tier) so a live offer is encoded from the
# same entry bt_core_observe would have read.
var pool_values := {}
var unpooled := {}

# Canonical weapon / character tables (BT_WEAPONS / BT_CHARACTERS order).  The
# canonical id is the table index and the id string is the live game my_id, so a
# live node/offer/character maps to the same index the core used.
var weapons: Array = []
var characters: Array = []
var n_items := 0
var n_weapons := 0
var n_characters := 0

# Last live weapon block (WEAPON_SLOTS * WEAPON_FIELDS floats) captured while the
# arena Player existed.  (Removed: caching the arena block across the shop hid
# buy/recycle/combine effects; the shop block is now read from the live owned
# inventory instead - see weapon_block().)

# active scales, refreshed from the tables' core_revision.obs_constants
var arena_w := DEFAULT_ARENA_W
var arena_h := DEFAULT_ARENA_H
var max_enemies := DEFAULT_MAX_ENEMIES
var waves := DEFAULT_WAVES
var core_revision := {}


func load_tables(path: String) -> bool:
	var file := File.new()
	if file.open(path, File.READ) != OK:
		table_error = "cannot open observation tables: %s" % path
		return false
	var parsed = JSON.parse(file.get_as_text())
	file.close()
	if parsed.error != OK or typeof(parsed.result) != TYPE_DICTIONARY:
		table_error = "observation tables are not valid JSON: %s" % path
		return false
	tables = parsed.result
	for key in ["items", "upgrades", "stat_ids", "stat_names", "weapons", "characters"]:
		if not tables.has(key):
			table_error = "observation tables lack %s: %s (regenerate with gen_obs_tables.py)" % [key, path]
			tables = {}
			return false
	var contract_error := _index_contract()
	if contract_error != "":
		table_error = contract_error
		tables = {}
		return false
	if tables.has("core_revision"):
		core_revision = tables["core_revision"]
	_apply_constants(core_revision.get("obs_constants", {}))
	return true


# Builds the (stat, tier) -> BT_UPGRADES value map and indexes the unpooled
# resources.  A table that does not describe the core's rows is a boot error,
# never a silent mismatch: a stale brotato_obs_tables.json (from before tiers
# were emitted) must not turn every live offer into a wrong feature.
func _index_contract() -> String:
	stat_ids = tables["stat_ids"]
	stat_names = tables["stat_names"]
	pool_values = {}
	unpooled = {}
	var upgrades: Array = tables["upgrades"]
	for i in range(upgrades.size()):
		var row = upgrades[i]
		if typeof(row) != TYPE_DICTIONARY or not row.has("tier") or not row.has("stat") or not row.has("value"):
			return "observation tables upgrade[%d] lacks stat/tier/value (stale artifact)" % i
		var row_stat := int(row["stat"])
		var row_tier := int(row["tier"])
		if row_stat < 0 or row_stat >= stat_names.size():
			return "observation tables upgrade[%d] stat %d outside the emitted contract" % [i, row_stat]
		if row_tier < 1 or row_tier > 4:
			return "observation tables upgrade[%d] tier %d outside 1..4" % [i, row_tier]
		var row_key := row_stat * 8 + (row_tier - 1)
		if pool_values.has(row_key):
			return "observation tables upgrade[%d] duplicates stat %d tier %d" % [i, row_stat, row_tier]
		pool_values[row_key] = float(row["value"])
	var unpooled_rows: Array = tables.get("unpooled_upgrades", [])
	for unpooled_row in unpooled_rows:
		if typeof(unpooled_row) == TYPE_DICTIONARY and unpooled_row.has("id"):
			unpooled[str(unpooled_row["id"])] = unpooled_row
			# A row with no core stat is the core's BT_STAT_NONE row (the weapon-slot
			# upgrade): bt_core_observe still encodes it from BT_UPGRADES, so it must
			# be in the (stat, tier) table the offer encoder reads.
			if unpooled_row.get("stat") == null:
				var none_tier := int(unpooled_row.get("tier", 0))
				var none_value = unpooled_row.get("value")
				if none_tier >= 1 and none_tier <= 4 and typeof(none_value) != TYPE_NIL:
					pool_values[BT_STAT_NONE * 8 + (none_tier - 1)] = float(none_value)
	# The empty-slot encoding must stay BT_UPGRADES[0].
	if not pool_values.has(INACTIVE_OFFER_STAT * 8):
		return "observation tables have no BT_UPGRADES[0] row (stat 0 tier 1)"
	if abs(float(pool_values[INACTIVE_OFFER_STAT * 8]) - INACTIVE_OFFER_VALUE) > 0.0001:
		return "observation tables BT_UPGRADES[0] value %s != %s: the empty-slot encoding shifted" % [
			str(pool_values[0]), str(INACTIVE_OFFER_VALUE)]
	for stat_key in stat_ids:
		var sid := int(stat_ids[stat_key])
		if sid < 0 or sid >= stat_names.size():
			return "observation tables stat_ids[%s] = %d outside the emitted contract" % [str(stat_key), sid]
	weapons = tables["weapons"]
	characters = tables["characters"]
	n_items = tables["items"].size()
	n_weapons = weapons.size()
	n_characters = characters.size()
	for i in range(n_weapons):
		var weapon_row = weapons[i]
		if typeof(weapon_row) != TYPE_DICTIONARY or not weapon_row.has("id"):
			return "observation tables weapons[%d] lacks an id (canonical BT_WEAPONS row)" % i
		if not weapon_row.has("my_id"):
			return "observation tables weapons[%d] lacks my_id (the unique per-tier id a live node/offer maps on)" % i
		if not weapon_row.has("tier") or not weapon_row.has("kind"):
			return "observation tables weapons[%d] lacks tier/kind (the obs tier/kind fields read the table row)" % i
	for i in range(n_characters):
		var character_row = characters[i]
		if typeof(character_row) != TYPE_DICTIONARY or not character_row.has("id"):
			return "observation tables characters[%d] lacks an id (canonical BT_CHARACTERS row)" % i
	# The emitted counts (when present) must agree with the tables, so a truncated
	# artifact cannot silently rescale the id denominators.
	var counts = tables.get("counts")
	if typeof(counts) == TYPE_DICTIONARY:
		if counts.has("weapons") and int(counts["weapons"]) != n_weapons:
			return "observation tables counts.weapons %d != weapons rows %d" % [int(counts["weapons"]), n_weapons]
		if counts.has("characters") and int(counts["characters"]) != n_characters:
			return "observation tables counts.characters %d != characters rows %d" % [int(counts["characters"]), n_characters]
	_note("stat contract: %d ids, %d upgrade rows, %d unpooled resources" % [
		stat_names.size(), pool_values.size(), unpooled.size()])
	_note("weapon/character contract: %d weapons, %d characters, %d items" % [n_weapons, n_characters, n_items])
	return ""


# Applies the constants emitted by gen_obs_tables.py (parsed from the core
# headers this port was generated from).  There is no compatibility mode: the
# bridge emits the current core's schema only, and the revision it used is
# recorded in the run meta.
func _apply_constants(constants) -> void:
	if typeof(constants) == TYPE_DICTIONARY:
		arena_w = float(constants.get("BT_ARENA_W", arena_w))
		arena_h = float(constants.get("BT_ARENA_H", arena_h))
		max_enemies = int(constants.get("BT_MAX", max_enemies))
		waves = int(constants.get("BT_WAVES", waves))
	# Drift guards: these are compiled literals here, so a mismatch with the
	# tables the run loaded means this bridge would emit a different schema than
	# the core it is paired with - a boot failure, never a silent divergence.
	var pinned_size := int(constants.get("BT_OBS_SIZE_PINNED", OBS_SIZE))
	if pinned_size != OBS_SIZE:
		table_error = "OBS_SIZE %d disagrees with the core tables (%d)" % [OBS_SIZE, pinned_size]
	var pinned_proj := float(constants.get("BT_OBS_PROJ_MAX_D2", PROJ_MAX_D2))
	if pinned_proj != PROJ_MAX_D2:
		table_error = "PROJ_MAX_D2 %s disagrees with the core tables (%s)" % [str(PROJ_MAX_D2), str(pinned_proj)]
	var pinned_actions := int(constants.get("BT_ACTIONS", 31))
	if pinned_actions != 31:
		table_error = "BT_ACTIONS %d disagrees with this bridge's 31-action table" % pinned_actions
	_note("observation scale: arena %sx%s, max_enemies %d, waves %d, obs %d" % [
		str(arena_w), str(arena_h), max_enemies, waves, OBS_SIZE])


func _note(message: String) -> void:
	if not notes.has(message):
		notes.push_back(message)


func item_index_by_id(id: String) -> int:
	if not tables.has("items"):
		return -1
	var prefix := str(tables.get("item_prefix", "item_"))
	var candidates := [id]
	if id.begins_with(prefix):
		candidates.push_back(id.substr(prefix.length(), id.length() - prefix.length()))
	var items: Array = tables["items"]
	for candidate in candidates:
		for index in range(items.size()):
			if str(items[index].get("id", "")) == candidate:
				return index
	if not unmapped.has(id):
		unmapped.push_back(id)
	return -1


# Canonical id lookups for the weapon/profile blocks.
#
# BT_WEAPONS rows are PER TIER: `id` is the family stem ("weapon_circular_saw"),
# which REPEATS across the tier rows, and `my_id` ("weapon_circular_saw_2") is the
# unique per-tier source id.  The live Weapon NODE carries WeaponData.weapon_id
# (the stem) and its tier (player.gd:376/380), so a node maps on (stem, tier);
# a live WeaponData (shop offer) has the unique my_id, so it maps on my_id.
func _normalize_id(id: String, prefix: String) -> String:
	if not prefix.empty() and id.begins_with(prefix):
		return id.substr(prefix.length(), id.length() - prefix.length())
	return id


func weapon_prefix() -> String:
	return str(tables.get("weapon_prefix", tables.get("item_prefix", "weapon_")))


func weapon_index_by_my_id(my_id: String) -> int:
	if weapons.empty():
		return -1
	var prefix := weapon_prefix()
	var key := _normalize_id(my_id, prefix)
	var hit := -1
	for i in range(weapons.size()):
		if _normalize_id(str(weapons[i].get("my_id", "")), prefix) == key:
			if hit >= 0:
				unmapped_note("weapon my_id %s matches more than one BT_WEAPONS row" % my_id)
				return -1
			hit = i
	if hit < 0 and not unmapped.has(my_id):
		unmapped.push_back(my_id)
	return hit


func weapon_index_by_live(stem: String, tier: int) -> int:
	# A live node exposes the family stem and its own tier; the row's tier must
	# agree.  Never matched on the stem alone (it is not unique).
	if weapons.empty():
		return -1
	var prefix := weapon_prefix()
	var key := _normalize_id(stem, prefix)
	var hit := -1
	for i in range(weapons.size()):
		var row = weapons[i]
		if _normalize_id(str(row.get("id", "")), prefix) != key:
			continue
		if int(row.get("tier", -1)) != tier:
			continue
		if hit >= 0:
			unmapped_note("weapon stem %s tier %d matches more than one BT_WEAPONS row" % [stem, tier])
			return -1
		hit = i
	if hit < 0 and not unmapped.has("%s@%d" % [stem, tier]):
		unmapped.push_back("%s@%d" % [stem, tier])
	return hit


func character_index_by_id(id: String) -> int:
	if characters.empty():
		return -1
	var prefix := str(tables.get("character_prefix", "character_"))
	var key := _normalize_id(id, prefix)
	for i in range(characters.size()):
		if _normalize_id(str(characters[i].get("id", "")), prefix) == key:
			return i
	if not unmapped.has(id):
		unmapped.push_back(id)
	return -1


# A resolution note that is not an id (kept out of `unmapped` so the report's
# unmapped_ids stays a list of ids).
func unmapped_note(message: String) -> void:
	if not notes.has(message):
		notes.push_back(message)


func upgrade_stat_id(stat_key: String) -> int:
	# The emitted contract keys are the game's effect keys ("stat_max_hp", ...).
	var key := stat_key
	if not key.begins_with("stat_") and stat_ids.has("stat_" + key):
		key = "stat_" + key
	if stat_ids.has(key):
		return int(stat_ids[key])
	# A key with no core stat is not an error: the core stores that BT_UPGRADES row
	# with stat == BT_STAT_NONE (brotato_upgrades.h:140-144), which is how the
	# weapon-slot upgrade is encoded.  The (stat, tier) lookup and the value
	# comparison below still have to accept it, so a bogus key still fails loudly
	# there instead of being claimed here.
	return BT_STAT_NONE


func stat_display_name(stat_id: int) -> String:
	if stat_id == BT_STAT_NONE:
		return "BT_STAT_NONE"
	if stat_id < 0 or stat_id >= stat_names.size():
		return "stat_%d?" % stat_id
	return str(stat_names[stat_id])


# Duck-typed property read that never errors on a freed resource.
func _stat(source, key: String, what: String) -> float:
	if source == null or not is_instance_valid(source):
		_missing("%s unavailable (source invalid)" % what)
		return 0.0
	return _numeric(source.get(key), 0.0, what)


func _missing(message: String) -> void:
	if not sample_errors.has(message):
		sample_errors.push_back(message)
	_note(message)


# A live offer (or shop item) the bridge could not map onto the core's own
# tables.  Recorded with the offer id / effect keys and the reason so the row's
# obs_unmapped field names the cause instead of just the slot.
func _unmapped(message: String) -> void:
	unmapped_live.push_back(message)
	_note(message)


func _numeric(value, fallback: float, what: String) -> float:
	var t := typeof(value)
	if t == TYPE_REAL or t == TYPE_INT:
		var number := float(value)
		if not is_nan(number) and not is_inf(number):
			return number
	_missing("%s unavailable or non-finite (%s)" % [what, str(t)])
	return fallback


# ---------------------------------------------------------------------------
# Live scene inspection
# ---------------------------------------------------------------------------

func _collect(node: Node, kind: String, out: Array, depth: int = 0) -> void:
	if depth > 32 or out.size() > max_enemies * 8:
		return
	if kind == "enemy" and node is Enemy and is_instance_valid(node):
		out.push_back(node)
	if kind == "player" and node is Player and is_instance_valid(node):
		out.push_back(node)
	for child in node.get_children():
		_collect(child, kind, out, depth + 1)


func _container(scene: Node, name: String) -> Node:
	if scene == null:
		return null
	var container := scene.get_node_or_null(name)
	if container == null:
		_note("scene container '%s' not found; its observation slots read as empty" % name)
	return container


func enemy_features(enemies: Array, px: float, py: float) -> Array:
	# Mirrors the core's selection: over all live entries accumulate squared
	# distance in index order, then selection-sort the first min(n, SLOTS).  A
	# node that is still is_instance_valid but already mortally queued can carry
	# a non-finite position/stats, so each candidate is read ONCE and any entry
	# with a non-finite position, health ratio or damage is skipped (counted in
	# sample_errors) instead of poisoning the row with NaN/Inf; the core's own
	# alive-enemy list is finite by construction.
	var distances := []
	var feats := []
	for i in range(min(enemies.size(), max_enemies)):
		var enemy = enemies[i]
		if not is_instance_valid(enemy):
			sample_errors.push_back("enemy[%d]: invalid instance; skipped" % i)
			continue
		var position: Vector2 = enemy.global_position
		var hp := _stat(enemy.current_stats, "health", "enemy.health")
		var max_hp := _stat(enemy.max_stats, "health", "enemy.max_health")
		var damage := _stat(enemy.current_stats, "damage", "enemy.damage")
		var ratio := hp / max_hp if max_hp > 0.0 else 0.0
		if not (_finite(position.x) and _finite(position.y) and _finite(hp) and
				_finite(max_hp) and _finite(damage) and _finite(ratio)):
			sample_errors.push_back(
				"enemy[%d]: non-finite position/health/damage %s %s %s/%s %s; skipped" % [
					i, str(position), str(hp), str(hp), str(max_hp), str(damage)])
			continue
		var elite := 0.0
		var flag = enemy.get("is_elite")
		if typeof(flag) == TYPE_BOOL:
			elite = 1.0 if flag else 0.0
		var dx := position.x - px
		var dy := position.y - py
		distances.push_back(dx * dx + dy * dy)
		feats.push_back([(position.x - px) / arena_w, (position.y - py) / arena_h,
			ratio, damage * 0.1, elite])

	var count := distances.size()
	var used := min(count, SLOTS)
	var features := []
	for slot in range(used):
		var best := slot
		for j in range(slot + 1, count):
			if distances[j] < distances[best]:
				best = j
		if best != slot:
			var td = distances[slot]
			distances[slot] = distances[best]
			distances[best] = td
			var tf = feats[slot]
			feats[slot] = feats[best]
			feats[best] = tf
		for value in feats[slot]:
			features.push_back(value)
	return features


func _finite(value: float) -> bool:
	return not (is_nan(value) or is_inf(value))


func projectile_features(projectiles: Array, px: float, py: float) -> Array:
	var features := []
	var consumed := []
	for slot in range(PROJ_SLOTS):
		var best := -1
		var best_d := 1e18
		for i in range(projectiles.size()):
			if consumed.has(i):
				continue
			var node = projectiles[i]
			var position: Vector2 = node.global_position
			var dx := position.x - px
			var dy := position.y - py
			var d := dx * dx + dy * dy
			if d < best_d:
				best_d = d
				best = i
		if best >= 0 and best_d < PROJ_MAX_D2:
			var node = projectiles[best]
			var position: Vector2 = node.global_position
			var damage := _numeric(node.get_damage() if node.has_method("get_damage") else 0.0, 0.0, "projectile damage")
			features.push_back((position.x - px) / arena_w)
			features.push_back((position.y - py) / arena_h)
			features.push_back(damage * 0.1)
			consumed.push_back(best)
		else:
			features.push_back(0.0)
			features.push_back(0.0)
			features.push_back(0.0)
	return features


func nearest_consumable_features(consumables: Array, px: float, py: float) -> Array:
	var best := -1
	var best_d := 1e18
	for i in range(consumables.size()):
		var node = consumables[i]
		var position: Vector2 = node.global_position
		var dx := position.x - px
		var dy := position.y - py
		var d := dx * dx + dy * dy
		if d < best_d:
			best_d = d
			best = i
	if best < 0:
		return [0.0, 0.0]
	var node = consumables[best]
	var position: Vector2 = node.global_position
	return [(position.x - px) / arena_w, (position.y - py) / arena_h]


# ---------------------------------------------------------------------------
# Shop / level-up offers
#
# During combat the core's own shop[]/lvl_offers[] entries are 0, so the
# inactive encoding below is exactly what bt_core_observe would emit.  In the
# shop/level-up phases the bridge reads the live offers; an offer it cannot map
# onto the core tables keeps the inactive encoding and is reported in
# `unmapped_live` with the offer id, its effect keys and the reason.
# ---------------------------------------------------------------------------

func shop_slot_features(item_index: int, materials: float, live_price: float = -1.0) -> Array:
	if not tables.has("items") or item_index < 0:
		return [0.0, 0.0, 0.0, 0.0]
	var items: Array = tables["items"]
	var item: Dictionary = items[item_index]
	var price := _numeric(item.get("price", 0), 0.0, "item price")
	if live_price >= 0.0:
		price = live_price
	var mag := _numeric(item.get("mag", 0.0), 0.0, "item dominant magnitude")
	return [
		_numeric(item.get("tier", 0), 0.0, "item tier") / 4.0,
		1.0 if materials >= price else 0.0,
		_numeric(item.get("dom", 0), 0.0, "item dominant stat") / 10.0,
		clamp(mag * 0.1, -1.0, 1.0)
	]


func upgrade_slot_features(upgrade_index: int) -> Array:
	if not tables.has("upgrades") or upgrade_index < 0:
		return [0.0, 0.0]
	var upgrades: Array = tables["upgrades"]
	var upgrade: Dictionary = upgrades[upgrade_index]
	return [
		_numeric(upgrade.get("stat", 0), 0.0, "upgrade stat") / 12.0,
		clamp(_numeric(upgrade.get("value", 0.0), 0.0, "upgrade value") * 0.1, -1.0, 1.0)
	]


# ---------------------------------------------------------------------------
# Owned weapons, typed shop offers, profile  (FROZEN map, WeaponSchema)
#
# weapon block [79..126]: six slots x 8, slot i base 79+8i:
#   id (canon+1)/(N_WEAPONS+1), tier/4 (table row), kind (table row),
#   clamp(cooldown_left/cooldown, 0, 1) in ticks, damage*0.1, max_range/2048,
#   crit_chance, cursed.  Empty slot = eight zeros; id 0.0 is the only empty
#   sentinel (weapon id 0 is real, so it emits 1/(N+1) != 0).
# Source of each field, by phase (shared rule with WeaponSchema):
#   * arena (phase 0) and level-up (phase 2): the live Player's Weapon nodes
#     give cooldown/damage/range/crit; tier/kind come from the BT_WEAPONS row.
#   * shop (phase 1): the arena scene (and its Weapon nodes) is gone, so the
#     owned inventory is read live from RunData.get_player_weapons(0) (real
#     WeaponData in slot order), damage/range/crit resolved through the game's
#     own WeaponService.init_melee_stats/init_ranged_stats, and cooldown = 0.0
#     (no live scene: the source emits 0 there too).  Never a cached arena
#     snapshot - that would hide buy/recycle/combine effects from the next row.
# ---------------------------------------------------------------------------

func _bool_field(source, key: String) -> bool:
	if source == null or not is_instance_valid(source):
		return false
	var value = source.get(key)
	if typeof(value) == TYPE_BOOL:
		return value
	return false


func _empty_weapon_features() -> Array:
	var out := []
	for _i in range(WEAPON_FIELDS):
		out.push_back(0.0)
	return out


# Encodes one owned weapon slot from its live Weapon node.  tier and kind come
# from the BT_WEAPONS row (canonical id = row index); cooldown/damage/range/crit
# come from the live node's current_stats (the per-run effective values, not the
# table's base).  Records the reason and returns [] when the id is unknown.
func weapon_live_features(node, slot: int) -> Array:
	# The live Weapon node carries the family stem (WeaponData.weapon_id) and its
	# own tier; the unique per-tier row is (stem, tier).
	var stem := str(node.get("weapon_id"))
	var tier_raw = node.get("tier")
	if typeof(tier_raw) != TYPE_INT:
		_unmapped("weapon[%d]: %s live tier unavailable (%s)" % [slot, stem, str(tier_raw)])
		return []
	var live_tier := int(tier_raw)
	var index := weapon_index_by_live(stem, live_tier)
	if index < 0:
		_unmapped("weapon[%d]: %s tier %d is not a BT_WEAPONS row" % [slot, stem, live_tier])
		return []
	var row: Dictionary = weapons[index]
	var stats = node.get("current_stats")
	var cd_value = stats.get("cooldown") if stats != null and is_instance_valid(stats) else 0.0
	var cd := _numeric(cd_value, 0.0, "weapon[%d] cooldown" % slot)
	var cd_left := _numeric(node.get("_current_cooldown"), 0.0, "weapon[%d] cooldown_left" % slot)
	var damage := _numeric(stats.get("damage") if stats != null and is_instance_valid(stats) else 0.0,
		0.0, "weapon[%d] damage" % slot)
	var max_range := _numeric(stats.get("max_range") if stats != null and is_instance_valid(stats) else 0.0,
		0.0, "weapon[%d] max_range" % slot)
	var crit := _numeric(stats.get("crit_chance") if stats != null and is_instance_valid(stats) else 0.0,
		0.0, "weapon[%d] crit_chance" % slot)
	var cooldown_frac := 0.0
	if cd > 0.0:
		cooldown_frac = clamp(cd_left / cd, 0.0, 1.0)
	return [
		float(index + 1) / float(n_weapons + 1),
		_numeric(row.get("tier", 0), 0.0, "weapon[%d] tier" % slot) / 4.0,
		_numeric(row.get("kind", 0), 0.0, "weapon[%d] kind" % slot),
		cooldown_frac,
		damage * 0.1,
		max_range / arena_w,
		crit,
		1.0 if _bool_field(node, "is_cursed") else 0.0
	]


# Builds the 48 weapon floats from the live owned inventory.  Returns
# {"values": Array, "live": bool, "ok": bool, "source": String}.  `ok` is false
# (and a reason is recorded) when a live id was unknown or no live source exists,
# so the caller marks the row incomplete rather than claiming an invented weapon.
#   * `player` present (arena combat, level-up): the live Weapon nodes.
#   * else in the shop phase: RunData.get_player_weapons(0), the persistent
#     inventory the shop handlers mutate (so the pre-action row reflects the
#     effect of the previous action, and nothing is cached or stale).
func weapon_block(player, phase: int) -> Dictionary:
	var values: Array = []
	var nodes: Array = []
	var live := false
	if player != null and is_instance_valid(player):
		var current = player.get("current_weapons")
		if typeof(current) == TYPE_ARRAY:
			nodes = current
			live = true
	if live:
		var ok := true
		for slot in range(WEAPON_SLOTS):
			var slot_features := []
			if slot < nodes.size() and nodes[slot] != null and is_instance_valid(nodes[slot]):
				slot_features = weapon_live_features(nodes[slot], slot)
				if slot_features.empty():
					ok = false
					slot_features = _empty_weapon_features()
			else:
				slot_features = _empty_weapon_features()
			for value in slot_features:
				values.push_back(value)
		return {"values": values, "live": true, "ok": ok, "source": "arena_weapon_nodes"}
	if phase == 1:
		var owned: Array = RunData.get_player_weapons(0)
		var ok := true
		for slot in range(WEAPON_SLOTS):
			var slot_features := []
			if slot < owned.size() and owned[slot] != null:
				slot_features = weapon_rundata_features(owned[slot], slot)
				if slot_features.empty():
					ok = false
					slot_features = _empty_weapon_features()
			else:
				slot_features = _empty_weapon_features()
			for value in slot_features:
				values.push_back(value)
		return {"values": values, "live": false, "ok": ok, "source": "run_data_inventory"}
	_missing("weapon slots unavailable: no live Player node and phase %d has no owned-inventory source" % phase)
	var zeros := []
	for _i in range(WEAPON_SLOTS * WEAPON_FIELDS):
		zeros.push_back(0.0)
	return {"values": zeros, "live": false, "ok": false, "source": "none"}


# Resolves a durable WeaponData's effective stats through the game's own
# WeaponService (weapon.gd:init_stats calls exactly these), so the shop row's
# damage/range/crit are the source-resolved values, not a copied formula.
func _resolve_weapon_stats(data):
	var base = data.get("stats")
	if base == null or not is_instance_valid(base):
		_missing("weapon %s has no base stats resource" % str(data.get("my_id")))
		return null
	var args = WeaponServiceInitStatsArgs.new()
	args.sets = data.get("sets")
	args.effects = data.get("effects")
	# Mirrors weapon.gd init_stats(): ranged stats go through init_ranged_stats,
	# everything else through init_melee_stats.
	if base is RangedWeaponStats:
		return WeaponService.init_ranged_stats(base, 0, false, args)
	if base is MeleeWeaponStats:
		return WeaponService.init_melee_stats(base, 0, args)
	_missing("weapon %s base stats are neither MeleeWeaponStats nor RangedWeaponStats (%s)"
		% [str(data.get("my_id")), str(base.get_class())])
	return null


# Encodes one owned weapon slot from its durable WeaponData (shop phase): id is
# the unique per-tier my_id -> BT_WEAPONS row, tier/kind from the row, cooldown
# 0.0 (no live arena scene), damage/range/crit from the WeaponService-resolved
# stats, cursed from the resource.
func weapon_rundata_features(data, slot: int) -> Array:
	var my_id := str(data.get("my_id"))
	var index := weapon_index_by_my_id(my_id)
	if index < 0:
		_unmapped("weapon[%d]: %s is not in BT_WEAPONS" % [slot, my_id])
		return []
	var row: Dictionary = weapons[index]
	var stats = _resolve_weapon_stats(data)
	var damage := _numeric(stats.get("damage") if stats != null and is_instance_valid(stats) else 0.0,
		0.0, "weapon[%d] damage" % slot)
	var max_range := _numeric(stats.get("max_range") if stats != null and is_instance_valid(stats) else 0.0,
		0.0, "weapon[%d] max_range" % slot)
	var crit := _numeric(stats.get("crit_chance") if stats != null and is_instance_valid(stats) else 0.0,
		0.0, "weapon[%d] crit_chance" % slot)
	return [
		float(index + 1) / float(n_weapons + 1),
		_numeric(row.get("tier", 0), 0.0, "weapon[%d] tier" % slot) / 4.0,
		_numeric(row.get("kind", 0), 0.0, "weapon[%d] kind" % slot),
		0.0,   # shop phase: no live arena Weapon node -> the source emits 0
		damage * 0.1,
		max_range / arena_w,
		crit,
		1.0 if _bool_field(data, "is_cursed") else 0.0
	]


# Typed shop offer block [127..138]: 4 offers x (type, id, cursed).  type is 0
# empty/unavailable, 1 item, 2 weapon; id is (canonical index+1)/(N+1) on the
# item or weapon table; cursed is 0/1 (0 when empty).  `offers` is the live
# ShopItem list read once by observe() (so the legacy item block and this block
# describe the same offers).
func offer_block(phase: int, offers: Array) -> Array:
	var values: Array = []
	for i in range(SHOP_OFFERS):
		var offer_type := SHOP_OFFER_EMPTY
		var id_value := 0.0
		var cursed := 0.0
		if phase == 1 and i < offers.size() and offers[i] != null:
			var data = offers[i].item_data
			if data is WeaponData:
				var widx := weapon_index_by_my_id(str(data.my_id))
				if widx >= 0:
					offer_type = SHOP_OFFER_WEAPON
					id_value = float(widx + 1) / float(n_weapons + 1)
				else:
					_unmapped("shop_offer[%d]: %s is not in BT_WEAPONS" % [i, str(data.my_id)])
			elif data is ItemData:
				var iidx := item_index_by_id(str(data.my_id))
				if iidx >= 0:
					offer_type = SHOP_OFFER_ITEM
					id_value = float(iidx + 1) / float(n_items + 1)
				else:
					_unmapped("shop_offer[%d]: %s is not in BT_ITEMS" % [i, str(data.my_id)])
			else:
				_unmapped("shop_offer[%d]: offer data is not ItemData/WeaponData (%s)"
					% [i, str(data) if data != null else "<null>"])
			cursed = 1.0 if _bool_field(data, "is_cursed") else 0.0
		values.push_back(offer_type)
		values.push_back(id_value)
		values.push_back(cursed)
	return values


# Profile block [139..140]: (canonical character id+1)/(N_CHARACTERS+1) from the
# live run character, and the weapon slot limit / 6 (the limit is
# RunData.get_player_effect(weapon_slot_hash, 0)).
func profile_block() -> Array:
	var values := []
	var character = RunData.get_player_character(0)
	var character_id := -1
	if character != null:
		character_id = character_index_by_id(str(character.get("my_id")))
	if character_id < 0:
		_missing("profile character id unavailable (%s)" % (str(character) if character != null else "<null>"))
		values.push_back(0.0)
	else:
		values.push_back(float(character_id + 1) / float(n_characters + 1))
	var slot_limit := _numeric(RunData.get_player_effect(Keys.weapon_slot_hash, 0), 0.0,
		"profile weapon slot limit")
	values.push_back(slot_limit / WEAPON_SLOT_LIMIT_DIVISOR)
	return values


# Actionable shop block [141..160], mirroring bt_weapon_observe_append:
#   [141] currency flag: 1.0 when the run's shop currency is stat_max_hp
#         (Demon hp_shop, run_data.gd:1429-1437 get_player_currency), else 0.0
#         for gold.  It describes the four offer prices below.
#   offer i base 142 + 4*i: exact price in the run's shop currency units (the
#         live ShopItem.value, already ceil(/20) under hp_shop via
#         shop_item.gd:138), the game's own weapon buy gate eligibility
#         (container._can_weapon_be_bought, shop_items_container.gd:97-135; an
#         item gates on currency only, :56-65), affordability against
#         get_player_currency, and buyability including the purchase delay.
#   [158..160] reroll price / free / available, always GOLD even for Demon
#         (base_shop.gd:400-410 tests and removes get_player_gold, never
#         get_player_currency).
# Outside the shop phase (or with no live offer) every field is 0.0 - the
# core-empty encoding - and a value that could not be read is reported, never
# guessed.
func shop_action_block(phase: int, offers: Array, container, scene) -> Array:
	var values := []
	values.push_back(1.0 if phase == 1 and RunData.get_player_effect_bool(Keys.hp_shop_hash, 0) else 0.0)
	var currency := _numeric(RunData.get_player_currency(0), 0.0, "player shop currency") if phase == 1 else 0.0
	var purchase_ready := false
	if phase == 1:
		if container == null or not is_instance_valid(container):
			_missing("shop purchase delay unavailable")
		else:
			var delay = container.get("_is_delay_active")
			if typeof(delay) != TYPE_BOOL:
				_missing("shop purchase delay is not a boolean")
			else:
				purchase_ready = not delay
	for i in range(SHOP_OFFERS):
		var price := 0.0
		var eligible := 0.0
		var affordable := 0.0
		var buyable := 0.0
		if phase == 1 and i < offers.size() and offers[i] != null:
			var offer = offers[i]
			var data = offer.item_data
			if data is WeaponData:
				price = _numeric(offer.value, 0.0, "shop_offer[%d] price" % i)
				eligible = 1.0 if weapon_offer_eligible(container, offer) else 0.0
				affordable = 1.0 if currency >= price else 0.0
				buyable = 1.0 if eligible > 0.0 and affordable > 0.0 and purchase_ready else 0.0
			elif data is ItemData:
				price = _numeric(offer.value, 0.0, "shop_offer[%d] price" % i)
				eligible = 1.0   # items gate on currency only (container:56-65)
				affordable = 1.0 if currency >= price else 0.0
				buyable = affordable if purchase_ready else 0.0
			else:
				_unmapped("shop_action[%d]: offer data is not ItemData/WeaponData (%s)"
					% [i, str(data) if data != null else "<null>"])
		values.push_back(price)
		values.push_back(eligible)
		values.push_back(affordable)
		values.push_back(buyable)
	for value in reroll_features(phase, scene):
		values.push_back(value)
	return values


# The shop scene's own live weapon buy gate.  `_can_weapon_be_bought`
# (shop_items_container.gd:97-135) is pure and is the exact rule the core's
# bt_wshop_can_buy models; the only difference is lock_current_weapons, which no
# Bull/Knight/Demon effect produces, so the two agree on every reachable profile.
func weapon_offer_eligible(container, offer) -> bool:
	if container == null or not is_instance_valid(container) or not container.has_method("_can_weapon_be_bought"):
		_missing("weapon offer eligibility unavailable (no shop items container)")
		return false
	var result = container.call("_can_weapon_be_bought", offer)
	if typeof(result) == TYPE_BOOL:
		return result
	_missing("weapon offer eligibility returned %s" % str(result))
	return false


# Reroll button state [158..160], read from the live BaseShop (the shop scene):
# _reroll_price[0] is already 0 while a free reroll is pending
# (base_shop.gd:479-483), _free_rerolls / _has_bonus_free_reroll say why, and
# availability is the exact :406-410 gate (gold, not get_player_currency).
func reroll_features(phase: int, scene) -> Array:
	if phase != 1:
		return [0.0, 0.0, 0.0]
	var prices = scene.get("_reroll_price") if scene != null and is_instance_valid(scene) else null
	if typeof(prices) != TYPE_ARRAY or prices.size() < 1:
		_missing("shop reroll price unavailable (scene exposes no _reroll_price)")
		return [0.0, 0.0, 0.0]
	var price := _numeric(prices[0], 0.0, "reroll price")
	var free := false
	var frees = scene.get("_free_rerolls")
	if typeof(frees) != TYPE_ARRAY or frees.size() < 1:
		_missing("shop free-reroll count unavailable")
		return [0.0, 0.0, 0.0]
	if int(frees[0]) > 0:
		free = true
	var bonus = scene.get("_has_bonus_free_reroll")
	if typeof(bonus) != TYPE_ARRAY or bonus.size() < 1:
		_missing("shop bonus-reroll state unavailable")
		return [0.0, 0.0, 0.0]
	if bonus[0]:
		free = true
	var gold := _numeric(RunData.get_player_gold(0), 0.0, "player gold")
	return [price, 1.0 if free else 0.0, 1.0 if (free or gold >= price) else 0.0]


# Encodes one live level-up offer exactly like bt_core_observe would have from
# BT_UPGRADES: (stat / 12.0f, clamp(value * 0.1f, -1, 1)).  The offer's dominant
# effect (the same largest-|value| rule the core uses for an item's dom) gives
# the game effect key; the emitted contract maps that to the core stat id and
# the core's own (stat, tier) row must exist and agree on the value, because the
# live value is what the game actually applied (it already includes any
# level_upgrades_modifications the run has).
# Returns [] when the slot has no offer resource (the caller then writes the
# core's empty-slot encoding) or when the offer cannot be mapped; only the
# latter is recorded as unmapped, which is what marks a row incomplete.
func upgrade_offer_features(data, slot: int) -> Array:
	if data == null:
		# No offer resource: the game legitimately shows fewer than four choices,
		# and a held item box leaves nodes without data.  That is the core's own
		# empty-slot case (bt_core_observe writes BT_UPGRADES[0]'s encoding when
		# lvl_offers[i] is unset), NOT a live offer that failed to map, so it is a
		# note and the caller keeps the empty encoding with a complete row.  Only a
		# non-null resource that cannot be mapped marks the row incomplete.
		_note("level_up[%d]: no offer resource (empty-slot encoding)" % slot)
		return []
	if not (data is UpgradeData):
		var what := str(data)
		if data is Object:
			what = "%s id=%s" % [data.get_class(), str(data.get("my_id"))]
		_unmapped("level_up[%d]: not UpgradeData (%s)" % [slot, what])
		return []
	var id := str(data.get("my_id"))
	var best_key := ""
	var best_value := 0.0
	var numeric_seen := false
	var keys := []
	for effect in data.effects:
		if effect == null:
			continue
		var raw_key = effect.get("key")
		var key := "?" if raw_key == null else str(raw_key)
		keys.push_back(key)
		var raw = effect.get("value")
		if typeof(raw) != TYPE_REAL and typeof(raw) != TYPE_INT:
			continue
		if abs(float(raw)) > abs(best_value):
			numeric_seen = true
			best_key = key
			best_value = float(raw)
	if best_key == "":
		if numeric_seen:
			_unmapped("level_up[%d]: %s dominant effect carries no key (keys %s)" % [slot, id, str(keys)])
		else:
			_unmapped("level_up[%d]: %s has no numeric effect (keys %s)" % [slot, id, str(keys)])
		return []
	var stat_id := upgrade_stat_id(best_key)
	if stat_id < 0:
		var pool_note := ""
		if unpooled.has(id):
			pool_note = ", resource %s is real but outside the level-up pool" % id
		_unmapped("level_up[%d]: %s key=%s has no core stat id (keys %s%s)"
			% [slot, id, best_key, str(keys), pool_note])
		return []
	var tier := int(data.get("tier")) + 1
	var pool_key := stat_id * 8 + (tier - 1)
	if not pool_values.has(pool_key):
		var why := ""
		if unpooled.has(id):
			why = ", resource %s is outside the level-up pool" % id
		_unmapped("level_up[%d]: %s maps to stat %d (%s) tier %d, which is not a BT_UPGRADES row%s"
			% [slot, id, stat_id, stat_display_name(stat_id), tier, why])
		return []
	var table_value := float(pool_values[pool_key])
	var expected_value := table_value
	if stat_id != BT_STAT_NONE:
		# item_service.gd:590-599: a pooled offer's effect values are the table
		# value scaled by level_upgrades_modifications and truncated with int(),
		# so that is what the live offer carries.  A BT_STAT_NONE row is never
		# scaled (brotato_core.h:1854) and the weapon-slot path bypasses
		# get_upgrade_data entirely (item_service.gd:550-556).
		var level_mod := float(RunData.get_player_effect(Keys.level_upgrades_modifications_hash, 0))
		if level_mod != 0.0:
			expected_value = float(int(table_value * (1.0 + level_mod / 100.0)))
	if abs(best_value - expected_value) > 0.0001:
		_unmapped("level_up[%d]: %s stat %d tier %d live value %s != core BT_UPGRADES value %s (modified expectation %s)"
			% [slot, id, stat_id, tier, str(best_value), str(table_value), str(expected_value)])
		return []
	return [float(stat_id) / 12.0, clamp(best_value * 0.1, -1.0, 1.0)]


# ---------------------------------------------------------------------------
# Observation
# ---------------------------------------------------------------------------

func observe(tree: SceneTree, player, phase: int, wave_timer, fallback_position := Vector2.ZERO) -> Dictionary:
	unmapped_live.clear()
	sample_errors.clear()
	var obs := PoolRealArray()
	obs.resize(OBS_SIZE)
	# PoolRealArray.resize() does not initialise the elements it grows: a fresh
	# float32 array can carry a previous allocation's memory (observed on Linux:
	# obs[36] = 2.89e10, obs[48] = 5.87e17, and NaN patterns in the skipped enemy
	# slots).  The core's contract is 0 for an absent enemy/projectile/consumable,
	# so the buffer is zeroed explicitly before any block writes into it.
	for i in range(OBS_SIZE):
		obs[i] = 0.0

	var scene = tree.current_scene if tree != null else null
	# Shop/level-up states have no Player node; the caller then supplies the last
	# real arena position (recorded per row as position_carried).
	var position: Vector2 = fallback_position
	if player != null and is_instance_valid(player):
		position = player.global_position
	var px := position.x
	var py := position.y

	# --- player -----------------------------------------------------------
	var hp := _numeric(RunData.get_player_current_health(0), 0.0, "player health")
	var max_hp := _numeric(RunData.get_player_max_health(0), 0.0, "player max health")
	var regen := _numeric(Utils.get_stat(Keys.stat_hp_regeneration_hash, 0), 0.0, "player regen")
	var armor := _numeric(Utils.get_stat(Keys.stat_armor_hash, 0), 0.0, "player armor")
	var materials := _numeric(RunData.get_player_gold(0), 0.0, "player materials")
	var level := _numeric(RunData.get_player_level(0), 0.0, "player level")
	var xp := _numeric(RunData.get_player_xp(0), 0.0, "player xp")
	var xp_need := _numeric(RunData.get_next_level_xp_needed(0), 0.0, "player xp_need")

	var iframe := 0.0
	var timer = player.get("_invincibility_timer") if player != null and is_instance_valid(player) else null
	if timer != null and is_instance_valid(timer):
		iframe = 1.0 if (not timer.is_stopped() and timer.time_left > 0.0) else 0.0
	elif phase != 1:
		_missing("player InvincibilityTimer unavailable")

	obs[0] = hp / max_hp if max_hp > 0.0 else 0.0
	obs[1] = max_hp * 0.015625
	obs[2] = regen * 0.0333333
	obs[3] = armor * 0.05
	obs[4] = px / arena_w
	obs[5] = py / arena_h
	obs[6] = iframe
	obs[7] = float(phase)
	obs[8] = materials * 0.01
	obs[9] = level * 0.05
	obs[10] = xp / (xp_need if xp_need > 0.0 else 1.0)

	# --- shop slots -------------------------------------------------------
	var shop_indices := [INACTIVE_SHOP_SLOT, INACTIVE_SHOP_SLOT, INACTIVE_SHOP_SLOT, INACTIVE_SHOP_SLOT]
	var shop_prices := [-1.0, -1.0, -1.0, -1.0]
	var live_offers: Array = []
	var shop_container = null
	if phase == 1:
		shop_container = live_shop_container(scene)
		live_offers = _live_shop_offers(shop_container)
		for i in range(min(4, live_offers.size())):
			if live_offers[i] != null:
				var offer_data = live_offers[i].item_data
				shop_prices[i] = float(live_offers[i].value)
				if offer_data is ItemData:
					var index := item_index_by_id(str(offer_data.my_id))
					if index >= 0:
						shop_indices[i] = index
					else:
						_unmapped("shop[%d]: %s is not in BT_ITEMS" % [i, str(offer_data.my_id)])
				elif offer_data is WeaponData:
					# A weapon offer is not in BT_ITEMS; the core's item block emits
					# shop[i]=0 (the documented empty encoding) and the typed offer
					# block [127..138] carries the real weapon.  Not a divergence.
					pass
				else:
					_unmapped("shop[%d]: offer data is not ItemData/WeaponData (%s)"
						% [i, str(offer_data) if offer_data != null else "<null>"])
	var b := 11
	for i in range(4):
		var slot := shop_slot_features(shop_indices[i], materials, shop_prices[i])
		for value in slot:
			obs[b] = value
			b += 1

	# --- level-up slots ---------------------------------------------------
	var offer_indices := [INACTIVE_OFFER_SLOT, INACTIVE_OFFER_SLOT, INACTIVE_OFFER_SLOT, INACTIVE_OFFER_SLOT]
	var live_offer_slots := []
	# v5 presence flag per slot: 1.0 when the game shows a live offer at that
	# index (the sim's lvl_offers[i] != 0), 0.0 otherwise.  Emitted at
	# obs[199..202] below, mirroring bt_core_observe's bt_lvl_offer_present.
	var level_up_presence := [0.0, 0.0, 0.0, 0.0]
	var crate_offer_open := false
	if phase == 2:
		# A held item box hides the four choice nodes but does not clear their
		# queued upgrade_data.  The core emits BOTH: the four queued level-up
		# offers from lvl_offers[] (brotato_core.h bt_core_observe, written
		# regardless of crate_kind) and the box itself (crate.offer_kind and the
		# take/recycle action mask).  So while a box is open the hidden nodes' own
		# data is the offer set to encode, not an unmapped slot; the value/tier
		# validation in upgrade_offer_features still has to accept it.
		crate_offer_open = _crate_offer_open(scene)
		live_offer_slots = _live_level_up_offers(scene)
		if live_offer_slots.size() != 4:
			_missing("level-up UI exposes %d choice nodes, the core always has 4 offers"
				% live_offer_slots.size())
	for i in range(4):
		var slot := []
		if phase == 2 and i < live_offer_slots.size():
			var live_offer = live_offer_slots[i]
			if live_offer["visible"] or (crate_offer_open and live_offer["data"] != null):
				slot = upgrade_offer_features(live_offer["data"], i)
				level_up_presence[i] = 1.0
			else:
				# Hidden node with no retained data: the game showed fewer than four
				# offers (or the box flow).  The core's empty-slot encoding is the
				# documented value for such a slot, so this never makes a row
				# incomplete - only a live resource that fails to map does.
				_note("level_up[%d]: no live offer; empty-slot encoding" % i)
		if slot.empty():
			slot = upgrade_slot_features(offer_indices[i])
		for value in slot:
			obs[b] = value
			b += 1

	# --- enemies ----------------------------------------------------------
	var enemies: Array = []
	if scene != null:
		_collect(scene, "enemy", enemies)
	var enemy_features := enemy_features(enemies, px, py)
	for value in enemy_features:
		obs[b] = value
		b += 1
	var used_slots := int(enemy_features.size() / 5)
	# Absent enemy slots are the core's zeros, written explicitly (never left as
	# whatever the buffer held).
	for _pad in range((SLOTS - used_slots) * 5):
		obs[b] = 0.0
		b += 1

	# --- wave -------------------------------------------------------------
	var wave := _numeric(RunData.current_wave, 0.0, "wave")
	var time_left := 0.0
	var wave_duration := 0.0
	if wave_timer != null and is_instance_valid(wave_timer):
		time_left = _numeric(wave_timer.time_left, 0.0, "wave time_left")
		wave_duration = _numeric(wave_timer.wait_time, 0.0, "wave duration")
	obs[b] = wave / float(waves)
	b += 1
	obs[b] = (time_left / wave_duration) if wave_duration > 0.0 else 0.0
	b += 1
	obs[b] = float(min(enemies.size(), max_enemies)) * (1.0 / ENEMY_COUNT_DENOMINATOR)
	b += 1

	# --- enemy projectiles ------------------------------------------------
	var projectiles: Array = []
	var container := _container(scene, "EnemyProjectiles")
	if container != null:
		for child in container.get_children():
			if child is Projectile and is_instance_valid(child):
				projectiles.push_back(child)
	var projectile_features := projectile_features(projectiles, px, py)
	for value in projectile_features:
		obs[b] = value
		b += 1

	# --- consumables ------------------------------------------------------
	var consumables: Array = []
	var consumable_container := _container(scene, "Consumables")
	if consumable_container != null:
		for child in consumable_container.get_children():
			if child is Consumable and is_instance_valid(child):
				consumables.push_back(child)
	var consumable_features := nearest_consumable_features(consumables, px, py)
	for value in consumable_features:
		obs[b] = value
		b += 1

	# --- owned weapons [79..126] ------------------------------------------
	var weapon_block_result := weapon_block(player, phase)
	for value in weapon_block_result["values"]:
		obs[b] = value
		b += 1

	# --- typed shop offers [127..138] -------------------------------------
	var typed_offers := offer_block(phase, live_offers)
	for value in typed_offers:
		obs[b] = value
		b += 1

	# --- profile [139..140] -----------------------------------------------
	var profile := profile_block()
	for value in profile:
		obs[b] = value
		b += 1

	# --- actionable shop state [141..160] ---------------------------------
	var shop_action := shop_action_block(phase, live_offers, shop_container, scene)
	for value in shop_action:
		obs[b] = value
		b += 1

	# --- Demon / endless block [161..202] ---------------------------------
	obs[b] = _numeric(Utils.get_stat(Keys.stat_harvesting_hash, 0), 0.0, "harvesting") * 0.01
	b += 1
	obs[b] = _numeric(Utils.get_stat(Keys.stat_luck_hash, 0), 0.0, "luck") * 0.01
	b += 1
	var endless := _numeric(RunData.get_endless_factor(), 0.0, "endless factor")
	if endless < 0.0:
		endless = 0.0
	obs[b] = endless / (1.0 + endless)
	b += 1
	obs[b] = float(RunData.current_wave) / 50.0
	b += 1
	# --- shop-owned state [165..173] --------------------------------------
	# Lock flags are durable RunData state (run_data.gd:456-468), emitted in
	# every phase like the core's shop_locked[]; an offer is locked when its
	# own ItemParentData is the array element the lock action stored.
	# RunData stores each lock as a [item_data, wave_value] PAIR
	# (run_data.gd:461-463 lock_player_shop_item push_back([item_data, wave_value])),
	# so the locked entry is entry[0], and the game's own visual-lock match is by
	# my_id (base_shop.gd:277-289), which is what makes a lock survive a reroll
	# that re-instantiates the offer.  Matching the raw item_data alone (as this
	# did) can never hit and reported every slot unlocked.
	# A slot with a LIVE offer is locked when that offer's item is one of the
	# durable lock entries (matched by my_id, base_shop.gd:277-289).  A slot with
	# NO live offer keeps the durable entry's own flag instead of falling to
	# unlocked: the lock list survives the shop scene (run_data.gd:456-468 state,
	# cleared only on the NEXT shop entry at base_shop.gd:110-120), and the
	# records occupy the leading slots in push_back order -- base_shop.gd:360-380
	# fill_shop_items starts from `player_locked_items.duplicate()` and appends the
	# rolled offers after them, which is exactly the core's shop_locked[] rank
	# order.  Without this the GO frame (the wave already advanced, the shop scene
	# gone, the wave-7 shop's lock still in RunData) reported every slot unlocked
	# while the core's durable shop_locked[] read 1: v5_demon_s3 row 1854.
	# NOTE: captures recorded before this fix carry the old 0 there, so a
	# recapture is required to confirm the transition-row lock flag.
	var locked_items: Array = RunData.get_player_locked_shop_items(0)
	var n_locked_durable := 0
	for entry in locked_items:
		if typeof(entry) == TYPE_ARRAY and entry.size() >= 1 and entry[0] != null:
			n_locked_durable += 1
	for li in range(SHOP_OFFERS):
		var locked := 0.0
		if li < live_offers.size() and live_offers[li] != null:
			var locked_data = live_offers[li].item_data
			if locked_data != null:
				for entry in locked_items:
					if typeof(entry) != TYPE_ARRAY or entry.size() < 1 or entry[0] == null:
						continue
					if entry[0] == locked_data or str(entry[0].my_id) == str(locked_data.my_id):
						locked = 1.0
						break
		elif li < n_locked_durable:
			locked = 1.0
		obs[b] = locked
		b += 1
	var crate := _live_crate_offer(scene)
	obs[b] = crate["kind"]
	b += 1
	obs[b] = crate["id"]
	b += 1
	obs[b] = crate["cursed"]
	b += 1
	obs[b] = crate["ready"]
	b += 1
	obs[b] = crate["reroll_price"]
	b += 1
	for i in range(OBS_TARGET_ITEMS):
		# `=` (not `:=`): a method call on an autoload singleton has no static
		# type, so GDScript 3 rejects inference here (parse error at load).
		var owned = RunData.get_nb_item(GUIDE_ITEM_IDS[i].hash(), 0)
		obs[b] = float(min(owned, 4)) / 4.0
		b += 1

	# --- v5 tail: ban budget [198] + level-up presence [199..202] ----------
	# The ban-token budget is 0 when the ban UI is off, else remaining tokens /
	# BAN_MAX_TOKEN (bt_core_observe's BT_OBS_T_BAN; run_data.gd:19,559-560).
	# ban_mode folds settings.ban_mode_toggled (is_ban_mode_active) and the run's
	# uses_ban, exactly the gates shop_item.gd's ban button reads.
	var ban_budget := 0.0
	if RunData.is_ban_mode_active and RunData.is_ban_active_in_current_run() and RunData.players_data.size() > 0:
		var remaining_ban_token = RunData.players_data[0].remaining_ban_token
		ban_budget = _numeric(remaining_ban_token, 0.0, "remaining ban token") / float(RunData.BAN_MAX_TOKEN)
	obs[b] = ban_budget
	b += 1
	for i in range(OBS_TARGET_LEVEL_UP):
		obs[b] = level_up_presence[i]
		b += 1

	return {
		"obs": obs,
		"written": b,
		"phase": phase,
		"errors": sample_errors.duplicate(),
		"counts": {
			"enemies_nodes": enemies.size(),
			"enemies_slots": used_slots,
			"projectiles": projectiles.size(),
			"consumables": consumables.size(),
			"shop_active": 1 if phase == 1 else 0,
			"offers_active": 1 if phase == 2 else 0,
			"weapon_live": 1 if weapon_block_result["live"] else 0,
			"weapon_source": str(weapon_block_result["source"]),
			"weapon_ok": 1 if weapon_block_result["ok"] else 0
		},
		"notes": notes.duplicate(),
		"unmapped": unmapped.duplicate(),
		"unmapped_live": unmapped_live.duplicate()
	}


func live_shop_container(scene: Node) -> Node:
	if scene != null and scene.has_method("_get_shop_items_container"):
		return scene.call("_get_shop_items_container", 0)
	return null


func _live_shop_offers(container) -> Array:
	var offers := []
	# Read visible ShopItem controls, not the compacted backing resource list:
	# their indices are the action slots and value is the actual purchase price.
	if container != null and is_instance_valid(container):
		var entries = container.get("_shop_items")
		if typeof(entries) == TYPE_ARRAY:
			for entry in entries:
				offers.push_back(entry if entry != null and entry.active else null)
			return offers
	_missing("shop controls unreadable from the live shop scene")
	return offers


# Reads the four live level-up choice nodes.  The accessor chain is the game's
# own: Main._upgrades_ui (main.gd:80) -> UpgradesUI._get_player_container(0)
# (upgrades_ui.gd:239) -> UpgradesUIPlayerContainer._get_upgrade_uis()
# (upgrades_ui_player_container.gd:171), whose nodes carry the public
# `upgrade_data` set by UpgradeUI.set_upgrade (upgrade_ui.gd:12).  Every slot
# reports why it is unusable; nothing is silently replaced by the empty-slot
# encoding.
func _live_level_up_offers(scene: Node) -> Array:
	var slots := []
	if scene == null:
		_missing("scene missing while reading level-up offers")
		return slots
	var ui = scene.get("_upgrades_ui")
	if ui == null or not is_instance_valid(ui) or not ui.has_method("_get_player_container"):
		_missing("Main._upgrades_ui inaccessible; level-up offers cannot be read")
		return slots
	var container = ui._get_player_container(0)
	if container == null or not is_instance_valid(container) or not container.has_method("_get_upgrade_uis"):
		_missing("level-up player container has no _get_upgrade_uis()")
		return slots
	var nodes: Array = container._get_upgrade_uis()
	for i in range(nodes.size()):
		var node = nodes[i]
		if node == null or not is_instance_valid(node):
			slots.push_back({"visible": false, "data": null, "node": null})
			continue
		if not node.visible:
			# The data is kept even when the node is hidden: when an item box is
			# open the four nodes are hidden but their queued upgrade_data is
			# still the offer set the core reports (see the level-up slot block
			# in observe()).
			slots.push_back({"visible": false, "data": node.get("upgrade_data"), "node": node})
			continue
		slots.push_back({"visible": true, "data": node.get("upgrade_data"), "node": node})
	return slots


# Reads the held crate (item box) offer and the level-up reroll price from the
# same upgrades-UI container the level-up slots come from
# (upgrades_ui_player_container.gd:33-41: _item_data / _consumable_data /
# _reroll_price / _reroll_button).  A crate item is always an ITEM offer
# (process_item_box draws from TierData.ITEMS, item_service.gd:207-217), and
# "ready" is simply "the offer exists" - the sim has no separate purchase-delay
# timer for it.  An absent offer is the empty encoding (all zero), which is what
# the core emits from crate_kind == BT_SHOP_NONE.
# True while the level-up container is showing an item box offer instead of the
# four upgrade choices (upgrades_ui_player_container.gd show_item(): _item_data
# and _consumable_data set, _items_container visible).
func _crate_offer_open(scene: Node) -> bool:
	if scene == null:
		return false
	var ui = scene.get("_upgrades_ui")
	if ui == null or not is_instance_valid(ui) or not ui.has_method("_get_player_container"):
		return false
	var container = ui._get_player_container(0)
	if container == null or not is_instance_valid(container):
		return false
	return container.get("_item_data") != null and container.get("_consumable_data") != null


func _live_crate_offer(scene: Node) -> Dictionary:
	var out := {"kind": 0.0, "id": 0.0, "cursed": 0.0, "ready": 0.0, "reroll_price": 0.0}
	if scene == null:
		return out
	var ui = scene.get("_upgrades_ui")
	if ui == null or not is_instance_valid(ui) or not ui.has_method("_get_player_container"):
		return out
	var container = ui._get_player_container(0)
	if container == null or not is_instance_valid(container):
		return out
	var item_data = container.get("_item_data")
	var consumable = container.get("_consumable_data")
	if item_data != null and consumable != null:
		var index := item_index_by_id(str(item_data.my_id))
		if index < 0:
			_unmapped("crate item %s is not in BT_ITEMS" % str(item_data.my_id))
			return out
		out["kind"] = SHOP_OFFER_ITEM
		out["id"] = float(index + 1) / float(n_items + 1)
		out["cursed"] = 1.0 if _bool_field(item_data, "is_cursed") else 0.0
		out["ready"] = 1.0
		return out
	# No crate: the level-up reroll price.  The button is hidden (and no price is
	# offered) when the game showed a single upgrade
	# (upgrades_ui_player_container.gd:75 `_reroll_button.visible = upgrades.size() > 1`).
	var button = container.get("_reroll_button")
	if button != null and is_instance_valid(button) and button.visible:
		out["reroll_price"] = _numeric(container.get("_reroll_price"), 0.0, "level-up reroll price")
	return out


func report() -> Dictionary:
	return {
		"obs_size": OBS_SIZE,
		"arena": {"w": arena_w, "h": arena_h},
		"scale": {
			"arena_w": arena_w,
			"arena_h": arena_h,
			"max_enemies": max_enemies,
			"enemy_count_denominator": ENEMY_COUNT_DENOMINATOR,
			"waves": waves,
			"source": "brotato_obs_tables.json core_revision.obs_constants" if not core_revision.empty() else "compiled defaults"
		},
		"core_revision": core_revision,
		"slots": {"enemies": SLOTS, "projectiles": PROJ_SLOTS},
		"tables_loaded": not tables.empty(),
		"table_error": table_error,
		"stat_contract": {
			"count": stat_names.size(),
			"names": stat_names.duplicate(),
			"keys": stat_ids.keys()
		},
		"upgrade_pool_rows": pool_values.size(),
		"unpooled_resources": unpooled.size(),
		"weapon_contract": {
			"count": n_weapons,
			"slots": WEAPON_SLOTS,
			"fields": WEAPON_FIELDS,
			"ids": weapons.duplicate()
		},
		"character_contract": {
			"count": n_characters,
			"ids": characters.duplicate()
		},
		"blocks": {
			"player": [0, 10],
			"items": [11, 26],
			"upgrades": [27, 34],
			"enemies": [35, 64],
			"wave": [65, 67],
			"projectiles": [68, 76],
			"consumable": [77, 78],
			"weapons": [79, 126],
			"offers": [127, 138],
			"profile": [139, 140],
			"shop_action": [141, 160]
		},
		"notes": notes.duplicate(),
		"unmapped_ids": unmapped.duplicate(),
		"unmapped_live_last": unmapped_live.duplicate(),
		"inactive_shop_slot_index": INACTIVE_SHOP_SLOT,
		"inactive_offer_slot_index": INACTIVE_OFFER_SLOT,
		"divergences": [
			"enemy ordering follows scene-tree (spawn) order, not the core SoA index order; equidistant ties may pick a different enemy",
			"shop/level-up slots read live only in those phases; during combat they carry the core's own empty-slot encoding (BT_ITEMS[0] / BT_UPGRADES[0])",
			"the weapon block is live in every phase: the arena Weapon nodes during combat/level-up and RunData.get_player_weapons(0) (WeaponService-resolved stats, cooldown 0.0) in the shop, so a buy/recycle/combine is visible in the next row",
			"the shop-action block [141..160] reads live only in the shop phase; elsewhere every field is the core-empty encoding 0.0. Weapon eligibility uses the game's own container._can_weapon_be_bought, which additionally checks lock_current_weapons; the core's bt_wshop_can_buy omits it, but no Bull/Knight/Demon effect produces lock_current_weapons, so the two agree on every reachable profile"
		]
	}
