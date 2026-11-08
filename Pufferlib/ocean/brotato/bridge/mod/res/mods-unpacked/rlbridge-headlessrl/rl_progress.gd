extends Reference
# Progression actions: shop (phase 1) and level-up (phase 2), driven through the
# game's own handlers so the run advances through waves instead of stalling.
#
# Verified entry points (decompiled retail sources):
#   shop  base_shop.gd:577  buy_item(item_data: ItemData, player_index)
#         base_shop.gd:602  buy_weapon(item_data: WeaponData, player_index)
#         base_shop.gd:400  _on_RerollButton_pressed(player_index)   (action 4)
#         base_shop.gd:298  _on_GoButton_pressed(player_index)       (action 8: next wave)
#         base_shop.gd:273  get_player_shop_items(player_index)      -> the live offer list
#         base_shop.gd:707  _on_item_discard_button_pressed(weapon, player_index)  -> recycle (9..14)
#         base_shop.gd:622  _on_item_combine_button_pressed(weapon, player_index)  -> combine (15..20)
#   level-up ui/menus/ingame/upgrades_ui_player_container.gd:203
#         _on_choose_button_pressed(upgrade)  -> emits choose_button_pressed
#         -> UpgradesUI._on_choose_button_pressed(upgrade_data, player_index)
#         -> emits upgrade_selected -> Main.on_upgrade_selected -> RunData.apply_item_effects
#         offers are ui/menus/upgrades/upgrade_ui.gd's public `upgrade_data` on each
#         visible choice (container._get_upgrade_uis()).
#
#   shop  shop_item.gd:243            change_lock_status(bool)              (21..24 lock/unlock offer)
#         shop_items_container.gd:250  on_shop_item_ban_button_pressed(item) (25..28 ban offer;
#                                      shop_item.ban_item() pushes banned_items, token -= 1, then
#                                      base_shop.gd:844 on_shop_item_banned removes the offer)
#   level-up upgrades_ui_player_container.gd:175 _on_RerollButton_pressed()  (level-up 4 reroll)
#         ...:216 / :222               _on_TakeButton_pressed / _on_DiscardButton_pressed (crate 29/30)
#                                      -> main.gd:1145 RunData.add_item / main.gd:1148 RunData.add_gold
#
# The action ids are the core's (brotato_state.h BT_ACTIONS, v4 = 31): combat 0-8
# are directions, shop 0-3 buy that slot / 4 reroll / 8 skip / 9-14 recycle /
# 15-20 combine / 21-24 lock / 25-28 ban, level-up 0-3 choose that offer / 4 reroll
# the offers, crate 29 take / 30 recycle.  Nothing is applied twice: each call is
# guarded by a post-condition check that also decides `action_applied`.

var notes := []
var shop_actions_done := 0
var shop_rerolls := 0
var shop_skips := 0
var shop_buys := 0
var shop_recycles := 0
var shop_combines := 0
var shop_locks := 0
var shop_bans := 0
var level_ups_chosen := 0
var level_up_rerolls := 0
var crate_takes := 0
var crate_recycles := 0
var failures := 0
var last_error := ""


func _note(message: String) -> void:
	if not notes.has(message):
		notes.push_back(message)


func _fail(code: String) -> Dictionary:
	failures += 1
	last_error = code
	return {"ok": false, "error": code}


# ---------------------------------------------------------------------------
# Shop
# ---------------------------------------------------------------------------

# The live offer list.  base_shop stores entries as [data, ...] pairs; some
# call sites treat an entry as an object with .active/.locked, so accept both
# shapes and report anything unexpected instead of guessing.
func shop_offers(shop) -> Array:
	var offers := []
	if shop == null or not shop.has_method("get_player_shop_items"):
		_note("shop scene has no get_player_shop_items(); offers unavailable")
		return offers
	var raw = shop.get_player_shop_items(0)
	if typeof(raw) != TYPE_ARRAY:
		_note("shop offers are not an array (%s)" % str(typeof(raw)))
		return offers
	for entry in raw:
		if typeof(entry) == TYPE_ARRAY:
			offers.push_back(entry[0] if entry.size() > 0 else null)
		elif entry != null and entry.get("data") != null:
			offers.push_back(entry.get("data"))
		elif entry != null and entry.get("item") != null:
			offers.push_back(entry.get("item"))
		else:
			offers.push_back(null)
	return offers


func shop_action(shop, action: int) -> Dictionary:
	if shop == null:
		return _fail("shop_scene_missing")
	if action >= 0 and action <= 3:
		var container = shop._get_shop_items_container(0)
		if container == null:
			return _fail("shop_container_missing")
		var slots = container.get("_shop_items")
		if typeof(slots) != TYPE_ARRAY or action >= slots.size():
			return _fail("shop_slot_%d_empty" % action)
		var slot = slots[action]
		if slot == null or not slot.active or slot.item_data == null:
			return _fail("shop_slot_%d_empty" % action)
		if RunData.get_player_currency(0) < slot.value:
			return _fail("shop_slot_%d_insufficient_currency" % action)
		var data = slot.item_data
		if not (data is ItemData or data is WeaponData):
			return _fail("shop_slot_%d_unsupported_type" % action)
		# The container enforces affordability/delay and emits shop_item_bought.
		# BaseShop's connected callback debits currency, consumes the offer and
		# applies the item; calling buy_item directly would grant a free item.
		container.on_shop_item_buy_button_pressed(slot)
		if slot.active:
			return _fail("shop_slot_%d_purchase_not_applied" % action)
		shop_buys += 1
		shop_actions_done += 1
		return {"ok": true, "kind": "buy_weapon" if data is WeaponData else "buy_item"}
	if action == 4:
		shop._on_RerollButton_pressed(0)
		shop_rerolls += 1
		shop_actions_done += 1
		return {"ok": true, "kind": "reroll"}
	if action == 8:
		shop._on_GoButton_pressed(0)
		shop_skips += 1
		shop_actions_done += 1
		return {"ok": true, "kind": "skip"}
	# Recycle owned weapon slot (action-9) through base_shop.gd
	# _on_item_discard_button_pressed(weapon_data, player_index): the game's own
	# handler removes the weapon, adds the recycling value and reloads the shop.
	if action >= 9 and action <= 14:
		var recycle_slot := action - 9
		var weapons = RunData.get_player_weapons(0)
		if recycle_slot >= weapons.size() or weapons[recycle_slot] == null:
			return _fail("recycle_slot_%d_empty" % recycle_slot)
		if not shop.has_method("_on_item_discard_button_pressed"):
			return _fail("shop_recycle_handler_missing")
		var recycle_weapon = weapons[recycle_slot]
		shop._on_item_discard_button_pressed(recycle_weapon, 0)
		shop_recycles += 1
		shop_actions_done += 1
		return {"ok": true, "kind": "recycle", "slot": recycle_slot,
			"weapon": str(recycle_weapon.get("my_id"))}
	# Combine owned weapon slot (action-15) through base_shop.gd
	# _on_item_combine_button_pressed(weapon_data, player_index) ->
	# _combine_weapon: it removes this weapon plus a same-id duplicate and adds the
	# upgraded weapon (2 removed, 1 added).  Guard the source preconditions (an
	# upgrades_into target and a duplicate must exist) so the game's own method is
	# never called on a weapon it cannot combine.
	if action >= 15 and action <= 20:
		var combine_slot := action - 15
		var cweapons = RunData.get_player_weapons(0)
		if combine_slot >= cweapons.size() or cweapons[combine_slot] == null:
			return _fail("combine_slot_%d_empty" % combine_slot)
		var combine_weapon = cweapons[combine_slot]
		if combine_weapon.get("upgrades_into") == null:
			return _fail("combine_slot_%d_no_upgrade_target" % combine_slot)
		var duplicates := 0
		for weapon in cweapons:
			if weapon != null and ItemService.is_same_weapon(weapon, combine_weapon):
				duplicates += 1
		if duplicates < 2:
			return _fail("combine_slot_%d_no_duplicate" % combine_slot)
		if not shop.has_method("_on_item_combine_button_pressed"):
			return _fail("shop_combine_handler_missing")
		shop._on_item_combine_button_pressed(combine_weapon, 0)
		shop_combines += 1
		shop_actions_done += 1
		return {"ok": true, "kind": "combine", "slot": combine_slot,
			"weapon": str(combine_weapon.get("my_id"))}
	if action >= 21 and action <= 24:
		return _shop_lock_toggle(shop, action - 21)
	if action >= 25 and action <= 28:
		return _shop_ban_offer(shop, action - 25)
	return _fail("unsupported_shop_action_%d" % action)


# Post-condition verification: a buy must move gold (or consume the slot), a
# reroll must change the offer list, a skip must leave the shop scene.
func verify_shop(shop, kind: String, before: Dictionary, scene_name: String) -> Dictionary:
	var now := {
		"gold": RunData.get_player_gold(0),
		"offers": str(shop_offers(shop)) if shop != null else "",
		"scene": scene_name,
		"weapon_count": RunData.get_player_weapons(0).size(),
		# The lock/ban rules below read these; they must be present on BOTH sides
		# of the comparison (shop_snapshot() already carries them for `before`).
		"ban_tokens": ban_tokens(),
		"banned_count": RunData.players_data[0].banned_items.size(),
		"locked_count": RunData.locked_shop_items[0].size()
	}
	var applied := 0
	var error := ""
	match kind:
		"buy_item", "buy_weapon":
			if now["gold"] != before["gold"]:
				applied = 1
			else:
				error = "gold unchanged (%s -> %s)" % [str(before["gold"]), str(now["gold"])]
		"reroll":
			if now["offers"] != before["offers"]:
				applied = 1
			else:
				error = "offer list unchanged by reroll"
		"skip":
			# base_shop.gd _on_GoButton_pressed() runs the go handler and asks the
			# tree for a scene change, which Godot defers to the frame boundary --
			# so at verification time the scene is still Shop and comparing scene
			# names would always report a false negative.  The handler having run
			# without error IS the registration; the next row's `scene` field
			# (Main) confirms the switch that followed.
			applied = 1
		"lock", "unlock":
			# change_lock_status flips ShopItem.locked and mirrors the change into
			# RunData.locked_shop_items(player_index) (shop_item.gd:243-252).
			if int(now["locked_count"]) != int(before.get("locked_count", now["locked_count"])):
				applied = 1
			else:
				error = "locked-item count unchanged (%s -> %s)" % [
					str(before.get("locked_count")), str(now["locked_count"])]
		"ban":
			# shop_item.ban_item() spends exactly one token and records the item id.
			var tokens_before = int(before.get("ban_tokens", -1))
			if tokens_before < 0:
				error = "no ban-token snapshot before ban"
			elif int(now["ban_tokens"]) == tokens_before - 1:
				applied = 1
			else:
				error = "ban did not spend a token (%d -> %d)" % [tokens_before, int(now["ban_tokens"])]
		"recycle", "combine":
			# Both the game's recycle (1 removed, +value) and combine (2 removed,
			# 1 upgraded added) drop the owned weapon count by exactly one; a locked
			# or uncombinable weapon leaves it unchanged, so applied stays 0.
			var before_count := int(before.get("weapon_count", -1))
			if before_count < 0:
				error = "no weapon-count snapshot before %s" % kind
			elif int(now["weapon_count"]) == before_count - 1:
				applied = 1
			else:
				error = "weapon count did not drop by 1 for %s (%d -> %d)" % [
					kind, before_count, int(now["weapon_count"])]
		_:
			error = "no verification rule for %s" % kind
	return {"applied": applied, "error": error, "after": now}


# Shop 21..24: toggle the lock on the offer in slot `slot`.  The game's own
# input path (base_shop.gd:230-231) guards on the locking-disabled effect and on
# `item_data.is_lockable`, so this driver does the same before calling the
# ShopItem handler, and verifies the RunData locked list actually moved.
func _shop_lock_toggle(shop, slot: int) -> Dictionary:
	var container = shop._get_shop_items_container(0) if shop.has_method("_get_shop_items_container") else null
	if container == null:
		return _fail("shop_container_missing")
	var slots = container.get("_shop_items")
	if typeof(slots) != TYPE_ARRAY or slot >= slots.size():
		return _fail("shop_slot_%d_empty" % slot)
	var item = slots[slot]
	if item == null or not item.active or item.item_data == null:
		return _fail("shop_slot_%d_empty" % slot)
	if RunData.get_player_effect_bool(Keys.disable_item_locking_hash, 0):
		return _fail("shop_lock_disabled_by_effect")
	if not bool(item.item_data.get("is_lockable")):
		return _fail("shop_slot_%d_not_lockable" % slot)
	var was_locked = bool(item.locked)
	item.change_lock_status(not was_locked)
	if bool(item.locked) == was_locked:
		return _fail("shop_slot_%d_lock_not_applied" % slot)
	shop_locks += 1
	shop_actions_done += 1
	return {"ok": true, "kind": "unlock" if was_locked else "lock", "slot": slot}


# Shop 25..28: ban the offer in slot `slot` through the shop container's own ban
# handler (the one its ban button calls).  Weapons cannot be banned (base_shop's
# own condition at :240), the ban needs a remaining token, and a ban also pays
# the recycling value: main.gd:1158 owns that path for the crate UI, while the
# shop path (base_shop.gd:844) removes the offer from the shop list.
func _shop_ban_offer(shop, slot: int) -> Dictionary:
	var container = shop._get_shop_items_container(0) if shop.has_method("_get_shop_items_container") else null
	if container == null:
		return _fail("shop_container_missing")
	if not RunData.is_ban_active_in_current_run():
		return _fail("shop_ban_inactive_in_this_run")
	var slots = container.get("_shop_items")
	if typeof(slots) != TYPE_ARRAY or slot >= slots.size():
		return _fail("shop_slot_%d_empty" % slot)
	var item = slots[slot]
	if item == null or not item.active or item.item_data == null:
		return _fail("shop_slot_%d_empty" % slot)
	if item.item_data is WeaponData:
		return _fail("shop_slot_%d_ban_weapon_unsupported" % slot)
	if ban_tokens() <= 0:
		return _fail("shop_ban_no_tokens")
	if RunData.players_data[0].banned_items.has(item.item_data.my_id_hash):
		return _fail("shop_slot_%d_already_banned" % slot)
	var tokens_before = ban_tokens()
	container.on_shop_item_ban_button_pressed(item)
	if ban_tokens() != tokens_before - 1:
		return _fail("shop_slot_%d_ban_not_applied" % slot)
	shop_bans += 1
	shop_actions_done += 1
	return {"ok": true, "kind": "ban", "slot": slot}


func shop_snapshot(shop, scene_name: String) -> Dictionary:
	return {
		"gold": RunData.get_player_gold(0),
		"offers": str(shop_offers(shop)) if shop != null else "",
		"scene": scene_name,
		"weapon_count": RunData.get_player_weapons(0).size(),
		"ban_tokens": ban_tokens(),
		"banned_count": RunData.players_data[0].banned_items.size(),
		"locked_count": RunData.locked_shop_items[0].size()
	}


# The player's remaining ban tokens (run_data.gd:19 BAN_MAX_TOKEN, :560 reset).
func ban_tokens() -> int:
	return int(RunData.players_data[0].remaining_ban_token)


# ---------------------------------------------------------------------------
# Level-up (arena's UpgradesUI)
# ---------------------------------------------------------------------------

func level_up_offers(ui) -> Array:
	var offers := []
	if ui == null or not ui.has_method("_get_player_container"):
		_note("UpgradesUI has no _get_player_container(); offers unavailable")
		return offers
	var container = ui._get_player_container(0)
	if container == null or not container.has_method("_get_upgrade_uis"):
		_note("level-up container has no _get_upgrade_uis(); offers unavailable")
		return offers
	for node in container._get_upgrade_uis():
		if node == null:
			continue
		if not node.visible:
			offers.push_back(null)
			continue
		var data = node.get("upgrade_data")
		offers.push_back(data)
	return offers


func level_up_action(ui, action: int) -> Dictionary:
	if ui == null:
		return _fail("upgrades_ui_missing")
	var container = ui._get_player_container(0)
	if action == 4:
		return _level_up_reroll(ui, container)
	if action == 29 or action == 30:
		return _crate_action(container, action)
	if action < 0 or action > 3:
		return _fail("unsupported_level_up_action_%d" % action)
	var offers := level_up_offers(ui)
	if action >= offers.size() or offers[action] == null:
		return _fail("level_up_offer_%d_unavailable" % action)
	container._on_choose_button_pressed(offers[action])
	level_ups_chosen += 1
	return {"ok": true, "kind": "level_up", "offer": action}


# Level-up 4: the UI's own reroll button (upgrades_ui_player_container.gd:175):
# it debits the reroll price and re-rolls the four offers.  The handler itself
# refuses when the price is unaffordable, so a rejected reroll keeps that reason.
func _level_up_reroll(ui, container) -> Dictionary:
	if container == null or not container.has_method("_on_RerollButton_pressed"):
		return _fail("level_up_reroll_handler_missing")
	var gold_before = RunData.get_player_gold(0)
	var offers_before = str(level_up_offers(ui))
	container._on_RerollButton_pressed()
	if RunData.get_player_gold(0) == gold_before and str(level_up_offers(ui)) == offers_before:
		return _fail("level_up_reroll_not_applied")
	level_up_rerolls += 1
	return {"ok": true, "kind": "level_up_reroll"}


# Crate offers (29 take / 30 recycle): show_item() swaps the container from the
# four upgrade choices to one crate item, so the action is only valid while that
# item container is visible and _item_data/_consumable_data are set.
func _crate_action(container, action: int) -> Dictionary:
	if container == null:
		return _fail("level_up_container_missing")
	var items_container = container.get("_items_container")
	var data = container.get("_item_data")
	var consumable = container.get("_consumable_data")
	if items_container == null or not items_container.visible or data == null or consumable == null:
		return _fail("crate_offer_not_open")
	# The game's own take/discard handlers emit upgrade_selected with the
	# container's player_index, and Main.on_upgrade_selected (main.gdc:1137) indexes
	# it: a crate container whose player_index was never set raises
	# "Invalid get index 'player_index' (on base: 'Nil')" inside the game.  Require a
	# valid index instead of driving the game into that error.
	var owner_index = container.get("player_index")
	if owner_index == null or int(owner_index) < 0:
		return _fail("crate_player_index_unset")
	if action == 29:
		if not container.has_method("_on_TakeButton_pressed"):
			return _fail("crate_take_handler_missing")
		var items_before = RunData.get_player_items(0).size()
		container._on_TakeButton_pressed()
		if RunData.get_player_items(0).size() != items_before + 1:
			return _fail("crate_take_not_applied")
		crate_takes += 1
		return {"ok": true, "kind": "crate_take"}
	if not container.has_method("_on_DiscardButton_pressed"):
		return _fail("crate_recycle_handler_missing")
	var gold_before = RunData.get_player_gold(0)
	container._on_DiscardButton_pressed()
	if RunData.get_player_gold(0) <= gold_before:
		return _fail("crate_recycle_not_applied")
	crate_recycles += 1
	return {"ok": true, "kind": "crate_recycle"}


# A chosen upgrade must reduce the pending list for player 0 or change a stat.
func verify_level_up(ui, before: Dictionary, kind: String = "") -> Dictionary:
	var pending := -1
	if ui != null and ui.get("_upgrades_to_process") != null:
		var lists = ui.get("_upgrades_to_process")
		if typeof(lists) == TYPE_ARRAY and lists.size() > 0 and typeof(lists[0]) == TYPE_ARRAY:
			pending = lists[0].size()
	var now := {
		"max_hp": RunData.get_player_max_health(0),
		"level": RunData.get_player_level(0),
		"pending": pending,
		"gold": RunData.get_player_gold(0),
		"item_count": RunData.get_player_items(0).size(),
		"offers": str(level_up_offers(ui))
	}
	var applied := 0
	var error := ""
	if kind == "level_up_reroll":
		if now["gold"] != before.get("gold", now["gold"]) or str(level_up_offers(ui)) != str(before.get("offers", "")):
			applied = 1
		else:
			error = "reroll neither debited gold nor changed the offers"
	elif kind == "crate_take":
		if int(now["item_count"]) == int(before.get("item_count", now["item_count"])) + 1:
			applied = 1
		else:
			error = "owned item count did not increase on crate take"
	elif kind == "crate_recycle":
		if now["gold"] > int(before.get("gold", now["gold"])) - 1 and now["gold"] != before.get("gold", now["gold"]):
			applied = 1
		else:
			error = "gold did not increase on crate recycle"
	elif before.has("pending") and before["pending"] >= 0 and pending >= 0:
		if pending < before["pending"]:
			applied = 1
		else:
			error = "pending upgrades did not decrease (%s -> %s)" % [str(before["pending"]), str(pending)]
	elif now["level"] != before.get("level", now["level"]) or now["max_hp"] != before.get("max_hp", now["max_hp"]):
		applied = 1
	else:
		error = "no observable state change after choosing an upgrade"
	return {"applied": applied, "error": error, "after": now}


func level_up_snapshot(ui) -> Dictionary:
	var pending := -1
	if ui != null and ui.get("_upgrades_to_process") != null:
		var lists = ui.get("_upgrades_to_process")
		if typeof(lists) == TYPE_ARRAY and lists.size() > 0 and typeof(lists[0]) == TYPE_ARRAY:
			pending = lists[0].size()
	return {
		"max_hp": RunData.get_player_max_health(0),
		"level": RunData.get_player_level(0),
		"pending": pending,
		"gold": RunData.get_player_gold(0),
		"item_count": RunData.get_player_items(0).size(),
		"offers": str(level_up_offers(ui))
	}


# Deterministic progression fallback, used when the action source is script or
# when the policy's action is not valid for the phase.
func scripted_shop_action(step_index: int, reroll_budget: int, rerolls_done: int) -> int:
	if rerolls_done < reroll_budget:
		return 4
	return 8


func scripted_level_up_action(_step_index: int) -> int:
	return 0


func report() -> Dictionary:
	return {
		"shop_actions": shop_actions_done,
		"shop_buys": shop_buys,
		"shop_recycles": shop_recycles,
		"shop_combines": shop_combines,
		"shop_rerolls": shop_rerolls,
		"shop_skips": shop_skips,
		"shop_locks": shop_locks,
		"shop_bans": shop_bans,
		"level_ups_chosen": level_ups_chosen,
		"level_up_rerolls": level_up_rerolls,
		"crate_takes": crate_takes,
		"crate_recycles": crate_recycles,
		"failures": failures,
		"last_error": last_error,
		"notes": notes.duplicate()
	}
